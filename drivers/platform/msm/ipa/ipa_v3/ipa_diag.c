// SPDX-License-Identifier: GPL-2.0
/*
 * Round-12 IPA uC SMMU diagnostic: the static record ring, the printed
 * checkpoints, and the translation evidence taken at WDI enable.
 */

#include <linux/atomic.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/utsname.h>
#include <generated/utsrelease.h>

#include "ipa_i.h"
#include "ipa_diag.h"
#include "ipahal/ipahal_fltrt.h"

#define IPA12_FAULT_IOVA	0x20000000ULL
#define IPA12_NEIGHBOUR_HI	0x47e00000ULL
#define IPA12_NEIGHBOUR_LO	0x47bcf000ULL

struct ipa_diag_buf ipa_diag = {
	.magic = IPA_DIAG_MAGIC,
	.version = IPA_DIAG_VERSION,
	.capacity = IPA_DIAG_CAPACITY,
	.bufsize = sizeof(struct ipa_diag_rec),
	.build = "r12 " UTS_RELEASE,
};

static atomic_t ipa_diag_cursor = ATOMIC_INIT(0);
static atomic_t ipa_diag_built = ATOMIC_INIT(0);
static atomic_t ipa_diag_full = ATOMIC_INIT(0);
static atomic64_t ipa_diag_gen = ATOMIC64_INIT(0);
static atomic64_t ipa_diag_rt_last = ATOMIC64_INIT(0);
static atomic64_t ipa_diag_flt_last = ATOMIC64_INIT(0);

void arm_smmu_iova_walk_domain(struct iommu_domain *domain, unsigned long iova,
	unsigned int stage, unsigned int cb);
int arm_smmu_domain_cbndx(struct iommu_domain *domain);

static const char *ipa_diag_kind_name(u8 kind)
{
	return kind == IPA_DIAG_RT ? "rt" : "flt";
}

void ipa_diag_announce(void)
{
	if (atomic_xchg(&ipa_diag_built, 1))
		return;

	pr_info("IPA12 BUF va=%px size=%zu capacity=%u\n",
		(void *)&ipa_diag, sizeof(ipa_diag), IPA_DIAG_CAPACITY);
}

void ipa_diag_freeze(void)
{
	WRITE_ONCE(ipa_diag.frozen, 1);
	smp_wmb();
}

bool ipa_diag_is_invalid(void)
{
	return !!READ_ONCE(ipa_diag.invalid);
}

void ipa_diag_mark_invalid(const char *why)
{
	if (cmpxchg(&ipa_diag.invalid, 0u, 1u))
		return;

	pr_emerg("IPA12 INVALID reason=%s tags=%u\n", why,
		ipahal_diag_tag_count());
}

/*
 * Append one record. The sequence number is stored last with release
 * ordering, so a reader never sees a record body that is still being written.
 * No allocation, printk or lock is taken here.
 */
static void ipa_diag_record(const struct ipa_diag_rec *src)
{
	struct ipa_diag_rec *r;
	u32 idx;

	if (READ_ONCE(ipa_diag.frozen))
		return;

	idx = (u32)atomic_inc_return(&ipa_diag_cursor) - 1;
	if (idx >= IPA_DIAG_CAPACITY) {
		if (!atomic_xchg(&ipa_diag_full, 1)) {
			WRITE_ONCE(ipa_diag.overflow, 1);
			ipa_diag_mark_invalid("buffer_overflow");
		}
		return;
	}

	r = &ipa_diag.rec[idx];
	r->generation = src->generation;
	r->submitted_raw = src->submitted_raw;
	r->decoded_addr = src->decoded_addr;
	r->image_dma = src->image_dma;
	r->clone_dma = src->clone_dma;
	r->image_byte_offset = src->image_byte_offset;
	r->local_byte_offset = src->local_byte_offset;
	r->length = src->length;
	r->slot = src->slot;
	r->pipe = src->pipe;
	r->ret = src->ret;
	r->a = src->a;
	r->b = src->b;
	r->stage = src->stage;
	r->kind = src->kind;
	r->ip = src->ip;
	r->hashed = src->hashed;
	r->is_sys = src->is_sys;
	r->tagged = src->tagged;
	r->done = src->done;

	WRITE_ONCE(ipa_diag.count, idx + 1);
	smp_store_release(&r->seq, (u64)idx + 1);
	ipa_diag_announce();
}

void ipa_diag_alloc(struct device *dev, struct iommu_domain *domain,
	dma_addr_t dma, size_t size, void *cpu)
{
	struct ipa_diag_rec r = {
		.stage = IPA_DIAG_ALLOC,
		.kind = IPA_DIAG_NONE,
		.pipe = -1,
		.image_dma = (u64)dma,
		.decoded_addr = (u64)(uintptr_t)cpu,
		.a = (u32)size,
	};

	pr_info("IPA12 ALLOC dev=%s devp=%px domain=%px dma=%pad size=%zu cpu=%px\n",
		dev ? dev_name(dev) : "(null)", dev, domain, &dma, size, cpu);

	ipa_diag_record(&r);
}

void ipa_diag_tag(enum ipa_diag_kind kind, u8 ip, bool hashed, s32 pipe,
	u32 slot, u64 generation, dma_addr_t image_dma, u32 image_byte_offset,
	u32 local_byte_offset, u64 original_raw, u64 submitted_raw,
	u64 decoded_addr, dma_addr_t clone_dma, bool is_sys)
{
	struct ipa_diag_rec r = {
		.generation = generation,
		.submitted_raw = submitted_raw,
		.decoded_addr = decoded_addr,
		.image_dma = (u64)image_dma,
		.clone_dma = (u64)clone_dma,
		.image_byte_offset = image_byte_offset,
		.local_byte_offset = local_byte_offset,
		.slot = slot,
		.pipe = pipe,
		.a = (u32)(original_raw >> 32),
		.b = (u32)original_raw,
		.stage = IPA_DIAG_TAG,
		.kind = kind,
		.ip = ip,
		.hashed = hashed ? 1 : 0,
		.is_sys = is_sys ? 1 : 0,
		.tagged = 1,
	};

	ipa_diag_record(&r);
}

u64 ipa_diag_next_gen(void)
{
	return (u64)atomic64_inc_return(&ipa_diag_gen);
}

void ipa_diag_pub(u64 gen, enum ipa_diag_kind kind, u8 ip, bool hashed,
	dma_addr_t src, u32 local_offset, u32 length, u32 slots, u32 tagged,
	int ret, bool done)
{
	struct ipa_diag_rec r = {
		.generation = gen,
		.image_dma = (u64)src,
		.local_byte_offset = local_offset,
		.length = length,
		.slot = slots,
		.pipe = -1,
		.ret = ret,
		.a = tagged,
		.stage = IPA_DIAG_PUB,
		.kind = kind,
		.ip = ip,
		.hashed = hashed ? 1 : 0,
		.done = done ? 1 : 0,
	};

	if (done) {
		if (kind == IPA_DIAG_RT)
			atomic64_set(&ipa_diag_rt_last, gen);
		else
			atomic64_set(&ipa_diag_flt_last, gen);
	}

	pr_info("IPA12 PUB gen=%llu kind=%s ip=%u hash=%u src=%pad dst=%#x len=%u slots=%u tagged=%u ret=%d done=%u\n",
		gen, ipa_diag_kind_name(kind), ip, hashed ? 1 : 0, &src,
		local_offset, length, slots, tagged, ret, done ? 1 : 0);

	ipa_diag_record(&r);
}

static void ipa_diag_xlate_one(struct iommu_domain *domain, s32 cb,
	u64 iova, u64 len, u32 stage)
{
	phys_addr_t pa;

	if (!domain)
		return;

	pa = iommu_iova_to_phys(domain, (unsigned long)iova);
	pr_info("IPA12 XLATE stage=%u domain=%px cb=%d iova=%#llx len=%#llx pa=%#llx\n",
		stage, (void *)domain, cb, iova, len, (u64)pa);
}

static void ipa_diag_xlate_domain(struct iommu_domain *domain)
{
	phys_addr_t empty = ipahal_get_empty_fltrt_base();
	u32 i, n = ipahal_diag_tag_count();
	s32 cb = arm_smmu_domain_cbndx(domain);

	/* The footprint a table consumer may stride, not just its first byte. */
	ipa_diag_xlate_one(domain, cb, (u64)empty, 8, IPA_DIAG_ENABLE);
	ipa_diag_xlate_one(domain, cb, (u64)empty, 4096, IPA_DIAG_ENABLE);
	ipa_diag_xlate_one(domain, cb, IPA12_FAULT_IOVA, 8, IPA_DIAG_ENABLE);
	ipa_diag_xlate_one(domain, cb, IPA12_FAULT_IOVA, 4096,
		IPA_DIAG_ENABLE);
	ipa_diag_xlate_one(domain, cb, IPA12_NEIGHBOUR_HI, 4096,
		IPA_DIAG_ENABLE);
	ipa_diag_xlate_one(domain, cb, IPA12_NEIGHBOUR_LO, 4096,
		IPA_DIAG_ENABLE);

	for (i = 0; i < n; i++) {
		u64 tag = (u64)ipahal_diag_tag_addr(i);

		ipa_diag_xlate_one(domain, cb, tag, 8, IPA_DIAG_ENABLE);
		ipa_diag_xlate_one(domain, cb, tag, 4096, IPA_DIAG_ENABLE);
	}
}

static void ipa_diag_walk_domain(struct iommu_domain *domain)
{
	phys_addr_t empty = ipahal_get_empty_fltrt_base();
	u32 i, n = ipahal_diag_tag_count();
	u32 cb = (u32)arm_smmu_domain_cbndx(domain);

	if (!domain)
		return;

	arm_smmu_iova_walk_domain(domain, (unsigned long)empty,
		IPA_DIAG_ENABLE, cb);
	arm_smmu_iova_walk_domain(domain, (unsigned long)IPA12_FAULT_IOVA,
		IPA_DIAG_ENABLE, cb);
	for (i = 0; i < n; i++)
		arm_smmu_iova_walk_domain(domain,
			(unsigned long)ipahal_diag_tag_addr(i),
			IPA_DIAG_ENABLE, cb);
}

void ipa_diag_enable(u32 pipe)
{
	struct iommu_domain *ap = ipa3_get_smmu_domain();
	struct iommu_domain *uc = ipa3_get_uc_smmu_domain();
	u64 rt = (u64)atomic64_read(&ipa_diag_rt_last);
	u64 flt = (u64)atomic64_read(&ipa_diag_flt_last);
	u64 ns = (u64)ktime_get_ns();
	struct ipa_diag_rec r = {
		.generation = rt,
		.slot = pipe,
		.pipe = (s32)pipe,
		.a = (u32)flt,
		.stage = IPA_DIAG_ENABLE,
		.kind = IPA_DIAG_NONE,
	};

	pr_info("IPA12 ENABLE pipe=%u rt_gen=%llu flt_gen=%llu ns=%llu\n",
		pipe, rt, flt, ns);
	ipa_diag_record(&r);

	pr_info("IPA12 ENFORCE ap_cb=%d uc_cb=%d ap_domain=%px uc_domain=%px tags=%u invalid=%u\n",
		arm_smmu_domain_cbndx(ap), arm_smmu_domain_cbndx(uc),
		(void *)ap, (void *)uc, ipahal_diag_tag_count(),
		ipa_diag_is_invalid());

	ipa_diag_xlate_domain(ap);
	ipa_diag_xlate_domain(uc);
	ipa_diag_walk_domain(ap);
	ipa_diag_walk_domain(uc);
}

/*
 * Entered from the SMMU context-fault handler before its report/BUG sequence.
 * Freezing here keeps the first-fault record ahead of every later append.
 */
/* Entry point used by the SMMU driver; stamps the fault time itself. */
void ipa3_diag_smmu_fault(u32 cb, u32 sid, u32 sid_mask, u64 far, u32 fsr,
	u32 fsynr0, u32 fsynr1, u32 cbfrsynra, int smr_idx, int report_ret)
{
	ipa_diag_fault(cb, sid, sid_mask, far, fsr, fsynr0, fsynr1,
		cbfrsynra, smr_idx < 0 ? 0xffffffff : (u32)smr_idx,
		(u64)ktime_get_ns(), report_ret);
}

void ipa_diag_fault(u32 cb, u32 sid, u32 sid_mask, u64 far, u32 fsr,
	u32 fsynr0, u32 fsynr1, u32 cbfrsynra, u32 smr_idx, u64 ns,
	int report_ret)
{
	struct ipa_diag_rec r = {
		.stage = IPA_DIAG_FAULT,
		.kind = IPA_DIAG_NONE,
		.pipe = (s32)cb,
		.slot = sid,
		.submitted_raw = far,
		.decoded_addr = (u64)fsr,
		.clone_dma = (u64)fsynr0,
		.image_dma = (u64)fsynr1,
		.generation = ns,
		.ret = report_ret,
		.a = sid_mask,
		.b = cbfrsynra,
	};

	pr_emerg("IPA12 FAULT cb=%u sid=%#x far=%#llx fsr=%#x fsynr0=%#x ns=%llu report=%d\n",
		cb, sid, far, fsr, fsynr0, ns, report_ret);

	/* The fault record is the one append that must survive the freeze. */
	ipa_diag_freeze();
	{
		u32 idx = (u32)atomic_inc_return(&ipa_diag_cursor) - 1;
		struct ipa_diag_rec *slot;

		if (idx >= IPA_DIAG_CAPACITY) {
			WRITE_ONCE(ipa_diag.overflow, 1);
			return;
		}
		slot = &ipa_diag.rec[idx];
		*slot = r;
		WRITE_ONCE(ipa_diag.count, idx + 1);
		smp_store_release(&slot->seq, (u64)idx + 1);
	}

	/* Whatever the handler already published stays checkable. */
	pr_emerg("IPA12 FAULT sid_mask=%#x fsynr1=%#x cbfrsynra=%#x smr=%d tags=%u invalid=%u overflow=%u\n",
		sid_mask, fsynr1, cbfrsynra, (int)smr_idx,
		ipahal_diag_tag_count(), ipa_diag_is_invalid(),
		READ_ONCE(ipa_diag.overflow));
}

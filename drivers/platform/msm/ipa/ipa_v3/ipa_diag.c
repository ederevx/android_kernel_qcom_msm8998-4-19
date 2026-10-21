// SPDX-License-Identifier: GPL-2.0
/*
 * Round-17 IPA uC SMMU diagnostic: the static record ring, the memory-only
 * enable checkpoints, r14 init/commit tagging, and bounded aliases of the
 * live WDI uC mappings at 0x20000000 + original IOVA.
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

struct ipa_diag_buf ipa_diag = {
	.magic = IPA_DIAG_MAGIC,
	.version = IPA_DIAG_VERSION,
	.capacity = IPA_DIAG_CAPACITY,
	.bufsize = sizeof(struct ipa_diag_rec),
	.tags_cov = IPA_DIAG_COV_UNKNOWN,
	.build = "r17 " UTS_RELEASE,
};

static atomic_t ipa_diag_cursor = ATOMIC_INIT(0);
static atomic_t ipa_diag_built = ATOMIC_INIT(0);
static atomic_t ipa_diag_full = ATOMIC_INIT(0);
static atomic64_t ipa_diag_gen = ATOMIC64_INIT(0);
static atomic64_t ipa_diag_rt_last = ATOMIC64_INIT(0);
static atomic64_t ipa_diag_flt_last = ATOMIC64_INIT(0);

static atomic_t ipa_diag_match_once = ATOMIC_INIT(0);

struct ipa_diag_init_ctx {
	u8 valid;
	u8 kind;
	u8 ip;
	u8 hash;
	u32 dest;
};

static struct ipa_diag_init_ctx ipa_diag_init_ctx;

void ipa_diag_init_set(u8 kind, u8 ip, u8 hash, u32 dest)
{
	ipa_diag_init_ctx.kind = kind;
	ipa_diag_init_ctx.ip = ip;
	ipa_diag_init_ctx.hash = hash;
	ipa_diag_init_ctx.dest = dest;
	WRITE_ONCE(ipa_diag_init_ctx.valid, 1);
}

bool ipa_diag_init_take(u8 *kind, u8 *ip, u8 *hash, u32 *dest)
{
	if (!READ_ONCE(ipa_diag_init_ctx.valid))
		return false;

	*kind = ipa_diag_init_ctx.kind;
	*ip = ipa_diag_init_ctx.ip;
	*hash = ipa_diag_init_ctx.hash;
	*dest = ipa_diag_init_ctx.dest;
	WRITE_ONCE(ipa_diag_init_ctx.valid, 0);
	return true;
}

static const char *ipa_diag_kind_name(u8 kind)
{
	return kind == IPA_DIAG_RT ? "rt" : "flt";
}

void ipa_diag_announce(void)
{
	if (atomic_xchg(&ipa_diag_built, 1))
		return;

	pr_info("IPA17 BUF va=%px size=%zu capacity=%u cov=%u\n",
		(void *)&ipa_diag, sizeof(ipa_diag), IPA_DIAG_CAPACITY,
		ipa_diag.tags_cov);
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

	pr_emerg("IPA17 INVALID reason=%s tags=%u\n", why,
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
	r->tag_alloc = src->tag_alloc;
	r->tagged_slots = src->tagged_slots;
	r->phase = src->phase;

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

	pr_info("IPA17 ALLOC dev=%s devp=%px domain=%px dma=%pad size=%zu cpu=%px\n",
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

	if (ret || ipa_diag_is_invalid())
		pr_err("IPA17 PUBFAIL gen=%llu kind=%s ip=%u hash=%u src=%pad dst=%#x len=%u slots=%u tagged=%u ret=%d done=%u invalid=%u\n",
			gen, ipa_diag_kind_name(kind), ip, hashed ? 1 : 0,
			&src, local_offset, length, slots, tagged, ret,
			done ? 1 : 0, ipa_diag_is_invalid());

	ipa_diag_record(&r);
}

static atomic64_t ipa_diag_enable_seq = ATOMIC64_INIT(0);

static void ipa_diag_enable_record(u64 seq, u32 pipe, u32 ready,
	enum ipa_diag_phase phase, int command_ret)
{
	u64 rt = atomic64_read(&ipa_diag_rt_last);
	u64 flt = atomic64_read(&ipa_diag_flt_last);
	struct ipa_diag_rec r = {
		.generation = seq,
		.submitted_raw = (u64)ktime_get_ns(),
		.decoded_addr = rt,
		.clone_dma = flt,
		.tag_alloc = ipahal_diag_tag_count(),
		.tagged_slots = ipahal_diag_tagged_count(),
		.slot = pipe,
		.pipe = (s32)pipe,
		.ret = command_ret,
		.a = (u32)flt,
		.b = ready,
		.stage = IPA_DIAG_ENABLE,
		.kind = IPA_DIAG_NONE,
		.phase = (u8)phase,
	};

	ipa_diag_record(&r);

	if (phase != IPA_DIAG_PHASE_CHECKPOINT_EXIT)
		return;

	pr_info("IPA17 ENABLE seq=%llu pipe=%u rt_gen=%llu flt_gen=%llu tags=%u slots=%u ready=%#x ns=%llu\n",
		seq, pipe, rt, flt, r.tag_alloc, r.tagged_slots, ready,
		r.submitted_raw);
}

u64 ipa_diag_enable_begin(u32 pipe, u32 ready)
{
	u64 seq = (u64)atomic64_inc_return(&ipa_diag_enable_seq);

	ipa_diag_enable_record(seq, pipe, ready,
		IPA_DIAG_PHASE_CHECKPOINT_ENTER, 0);
	return seq;
}

void ipa_diag_enable_phase(u64 seq, u32 pipe, u32 ready,
	enum ipa_diag_phase phase, int command_ret)
{
	ipa_diag_enable_record(seq, pipe, ready, phase, command_ret);
}

static void ipa_diag_append_frozen(const struct ipa_diag_rec *src)
{
	u32 idx = (u32)atomic_inc_return(&ipa_diag_cursor) - 1;
	struct ipa_diag_rec *slot;

	if (idx >= IPA_DIAG_CAPACITY) {
		WRITE_ONCE(ipa_diag.overflow, 1);
		return;
	}

	slot = &ipa_diag.rec[idx];
	*slot = *src;
	WRITE_ONCE(ipa_diag.count, idx + 1);
	smp_store_release(&slot->seq, (u64)idx + 1);
}

void ipa_diag_init_record(u8 kind, u8 ip, u8 hash, u32 image_off,
	u32 local_off, u32 slot, u64 before, u64 after, u32 tag_id)
{
	struct ipa_diag_rec r = {
		.generation = (u64)tag_id,
		.submitted_raw = before,
		.clone_dma = after,
		.image_byte_offset = image_off,
		.local_byte_offset = local_off,
		.slot = slot,
		.pipe = -1,
		.a = tag_id,
		.stage = IPA_DIAG_INIT,
		.kind = kind,
		.ip = ip,
		.hashed = hash,
	};

	ipa_diag_record(&r);
}

void ipa_diag_initpub(u8 kind, u8 ip, u32 dst, u32 len, int ret, bool done)
{
	struct ipa_diag_rec r = {
		.image_dma = (u64)dst,
		.length = len,
		.pipe = -1,
		.ret = ret,
		.stage = IPA_DIAG_INITPUB,
		.kind = kind,
		.ip = ip,
		.done = done ? 1 : 0,
	};

	pr_info("IPA17 INITPUB dst=%#x len=%u ret=%d done=%u\n",
		dst, len, ret, done ? 1 : 0);
	ipa_diag_record(&r);
}

/* Round-17 WDI alias generation and records. */
static atomic64_t ipa_diag_alias_seq = ATOMIC64_INIT(0);

u64 ipa_diag_alias_gen_new(void)
{
	return (u64)atomic64_inc_return(&ipa_diag_alias_seq);
}

void ipa_diag_alias(u32 object, struct iommu_domain *domain, u64 original,
	u64 alias, u64 pa, u64 len, u32 prot, int ret, u64 generation)
{
	struct ipa_diag_rec r = {
		.generation = generation,
		.submitted_raw = original,
		.image_dma = alias,
		.clone_dma = pa,
		.length = (u32)len,
		.slot = prot,
		.pipe = -1,
		.a = object,
		.ret = ret,
		.stage = IPA_DIAG_ALIAS,
		.kind = IPA_DIAG_NONE,
	};

	pr_info("IPA17 ALIAS object=%u domain=%px original=%#llx alias=%#llx pa=%#llx len=%#llx prot=%#x ret=%d generation=%llu\n",
		object, domain, (unsigned long long)original,
		(unsigned long long)alias, (unsigned long long)pa,
		(unsigned long long)len, prot, ret,
		(unsigned long long)generation);
	ipa_diag_record(&r);
}

void ipa_diag_alias_state(u32 object, u64 generation, u32 event)
{
	struct ipa_diag_rec r = {
		.generation = generation,
		.a = object,
		.ret = (int)event,
		.pipe = -1,
		.stage = IPA_DIAG_ALIAS_STATE,
		.kind = IPA_DIAG_NONE,
	};

	pr_info("IPA17 ALIAS_STATE object=%u generation=%llu event=%u\n",
		object, (unsigned long long)generation, event);
	ipa_diag_record(&r);
}

void ipa_diag_wdiaddr(u32 pipe, u32 object, u32 field_off, u64 submitted)
{
	struct ipa_diag_rec r = {
		.submitted_raw = submitted,
		.image_byte_offset = field_off,
		.slot = pipe,
		.pipe = (s32)pipe,
		.a = object,
		.stage = IPA_DIAG_WDIADDR,
		.kind = IPA_DIAG_NONE,
	};

	pr_info("IPA17 WDIADDR pipe=%u object=%u field_off=%u submitted=%#llx\n",
		pipe, object, field_off, (unsigned long long)submitted);
	ipa_diag_record(&r);
}

void ipa_diag_match(u64 far)
{
	u64 base = (u64)ipahal_get_empty_fltrt_base();
	u64 size = (u64)ipahal_get_empty_fltrt_size();
	u64 tbase = 0, tsize = 0, last, off = 0;
	u32 id = 0, kind = IPA_DIAG_MATCH_NEITHER, flags = 0;
	u8 src = IPA_DIAG_SRC_COMMIT;
	struct ipa_diag_rec r;

	if (atomic_xchg(&ipa_diag_match_once, 1))
		return;

	if (ipahal_diag_tag_lookup(far, &id, &tbase, &tsize, &src)) {
		kind = (src == IPA_DIAG_SRC_INIT) ? IPA_DIAG_MATCH_INIT :
			IPA_DIAG_MATCH_COMMIT;
		base = tbase;
		size = tsize;
	} else if (far >= base && far < base + size) {
		kind = IPA_DIAG_MATCH_ORIGINAL;
	}

	if (kind != IPA_DIAG_MATCH_NEITHER) {
		off = far - base;
		if (far >= base && far < base + size)
			flags |= IPA_DIAG_MATCH_OBJECT;
		else if (far >= (base & PAGE_MASK) &&
			 far < PAGE_ALIGN(base + size))
			flags |= IPA_DIAG_MATCH_GRANULE;
	}

	last = max_t(u64, atomic64_read(&ipa_diag_rt_last),
		     atomic64_read(&ipa_diag_flt_last));

	pr_emerg("IPA17 MATCH far=%#llx kind=%u tag_id=%u base=%#llx size=%#llx off=%#llx last_pub_gen=%llu\n",
		far, kind, id, base, size, off, last);

	r = (struct ipa_diag_rec) {
		.generation = last,
		.submitted_raw = far,
		.image_dma = base,
		.length = (u32)size,
		.local_byte_offset = (u32)off,
		.a = flags,
		.b = id,
		.stage = IPA_DIAG_MATCH,
		.kind = kind,
	};
	ipa_diag_append_frozen(&r);
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

	pr_emerg("IPA17 FAULT cb=%u sid=%#x far=%#llx fsr=%#x fsynr0=%#x ns=%llu report=%d\n",
		cb, sid, far, fsr, fsynr0, ns, report_ret);

	/* The fault and match records are the appends that survive the freeze. */
	ipa_diag_freeze();
	ipa_diag_append_frozen(&r);
	ipa_diag_match(far);

	/* Whatever the handler already published stays checkable. */
	pr_emerg("IPA17 FAULT sid_mask=%#x fsynr1=%#x cbfrsynra=%#x smr=%d tags=%u invalid=%u overflow=%u\n",
		sid_mask, fsynr1, cbfrsynra, (int)smr_idx,
		ipahal_diag_tag_count(), ipa_diag_is_invalid(),
		READ_ONCE(ipa_diag.overflow));
}

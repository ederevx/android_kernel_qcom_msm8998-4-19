// SPDX-License-Identifier: GPL-2.0
/*
 * IPA SMMU diagnostics: the static record ring, WDI enable records,
 * WLAN-supplied address records, the alias records, and the context fault
 * with a classification against the aliases and WLAN addresses the host
 * registered.
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>
#include <generated/utsrelease.h>

#include "ipa_i.h"
#include "ipa_diag.h"

struct ipa_diag_buf ipa_diag = {
	.magic = IPA_DIAG_MAGIC,
	.version = IPA_DIAG_VERSION,
	.capacity = IPA_DIAG_CAPACITY,
	.bufsize = sizeof(struct ipa_diag_rec),
	.build = UTS_RELEASE,
};

static atomic_t ipa_diag_cursor = ATOMIC_INIT(0);
static atomic_t ipa_diag_built = ATOMIC_INIT(0);
static atomic_t ipa_diag_full = ATOMIC_INIT(0);

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

	pr_emerg("IPA17 INVALID reason=%s\n", why);
}

static void ipa_diag_announce(void)
{
	if (atomic_xchg(&ipa_diag_built, 1))
		return;

	pr_info("IPA17 BUF va=%px size=%zu capacity=%u\n",
		(void *)&ipa_diag, sizeof(ipa_diag), IPA_DIAG_CAPACITY);
}

/* Record shapes are stable; the sequence is published last. */
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
	r->phase = src->phase;
	r->ip = src->ip;
	r->hashed = src->hashed;
	r->is_sys = src->is_sys;
	r->tagged = src->tagged;
	r->done = src->done;

	WRITE_ONCE(ipa_diag.count, idx + 1);
	smp_store_release(&r->seq, (u64)idx + 1);
	ipa_diag_announce();
}

/* ------------------------------------------------------------------ */
/* Registered ranges: aliases and WLAN-supplied setup addresses.       */

struct ipa_diag_range {
	bool used;
	u32 kind;
	u32 object;
	u64 base;
	u64 len;
};

#define IPA_DIAG_RANGES 256
static struct ipa_diag_range ipa_diag_ranges[IPA_DIAG_RANGES];
static DEFINE_SPINLOCK(ipa_diag_ranges_lock);

static void ipa_diag_range_add(u32 kind, u32 object, u64 base, u64 len)
{
	unsigned long flags;
	struct ipa_diag_range *r;
	int i;

	if (!len)
		return;

	spin_lock_irqsave(&ipa_diag_ranges_lock, flags);
	for (i = 0; i < IPA_DIAG_RANGES; i++) {
		r = &ipa_diag_ranges[i];

		if (r->used && r->kind == kind && r->object == object &&
		    r->base == base && r->len == len) {
			spin_unlock_irqrestore(&ipa_diag_ranges_lock, flags);
			return;
		}
	}
	for (i = 0; i < IPA_DIAG_RANGES; i++) {
		if (!ipa_diag_ranges[i].used) {
			ipa_diag_ranges[i] = (struct ipa_diag_range) {
				.used = true, .kind = kind, .object = object,
				.base = base, .len = len,
			};
			break;
		}
	}
	spin_unlock_irqrestore(&ipa_diag_ranges_lock, flags);
}

static u32 ipa_diag_range_match(u64 far, u64 *base, u32 *object)
{
	unsigned long flags;
	struct ipa_diag_range *r;
	u32 kind = IPA_DIAG_FAULT_NONE;
	int i;

	*base = 0;
	*object = 0;
	spin_lock_irqsave(&ipa_diag_ranges_lock, flags);
	for (i = 0; i < IPA_DIAG_RANGES; i++) {
		r = &ipa_diag_ranges[i];

		if (!r->used || far < r->base || far - r->base >= r->len)
			continue;
		*base = r->base;
		*object = r->object;
		kind = r->kind;
		break;
	}
	spin_unlock_irqrestore(&ipa_diag_ranges_lock, flags);
	return kind;
}

/* ------------------------------------------------------------------ */
/* WDI enable.                                                         */

void ipa_diag_wlanmap(s32 pipe, u64 supplied, u32 is_pa, u64 iova,
	u32 len, int ret, u32 phase)
{
	struct ipa_diag_rec r = {
		.submitted_raw = supplied,
		.decoded_addr = iova,
		.length = len,
		.pipe = pipe,
		.ret = ret,
		.a = is_pa,
		.phase = (u8)phase,
		.stage = IPA_DIAG_WLANMAP,
	};

	pr_info("IPA17 WLANMAP pipe=%d supplied=%#llx kind=%u iova=%#llx len=%#x ret=%d phase=%u\n",
		pipe, (unsigned long long)supplied, is_pa,
		(unsigned long long)iova, len, ret, phase);
	ipa_diag_record(&r);
}

void ipa_diag_wlanstate(u32 enabled, u32 over_gsi)
{
	pr_err("IPA17 WLANSTATE source=params smmu_enabled=%u over_gsi=%u\n",
		enabled, over_gsi);
}

/* ------------------------------------------------------------------ */
/* Bounded aliases of the live WDI uC mappings.                       */

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
	};

	pr_info("IPA17 ALIAS object=%u domain=%px original=%#llx alias=%#llx pa=%#llx len=%#llx prot=%#x ret=%d generation=%llu\n",
		object, domain, (unsigned long long)original,
		(unsigned long long)alias, (unsigned long long)pa,
		(unsigned long long)len, prot, ret,
		(unsigned long long)generation);
	ipa_diag_record(&r);

	if (!ret) {
		ipa_diag_range_add(IPA_DIAG_FAULT_ALIAS_ORIGINAL, object,
			original, len);
		ipa_diag_range_add(IPA_DIAG_FAULT_ALIAS_COMPACT, object,
			alias, len);
	}
}

void ipa_diag_alias_state(u32 object, u64 generation, u32 event)
{
	struct ipa_diag_rec r = {
		.generation = generation,
		.a = object,
		.ret = (int)event,
		.pipe = -1,
		.stage = IPA_DIAG_ALIAS_STATE,
	};

	pr_info("IPA17 ALIAS_STATE object=%u generation=%llu event=%u\n",
		object, (unsigned long long)generation, event);
	ipa_diag_record(&r);
}

/* ------------------------------------------------------------------ */
/* Fault.                                                              */

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
	u64 base = 0;
	u32 object = 0;
	u32 match = ipa_diag_range_match(far, &base, &object);
	struct ipa_diag_rec r = {
		.stage = IPA_DIAG_FAULT,
		.pipe = (s32)cb,
		.slot = sid,
		.submitted_raw = far,
		.decoded_addr = (u64)fsr,
		.clone_dma = (u64)fsynr0,
		.image_dma = base,
		.generation = ns,
		.ret = report_ret,
		.a = match,
		.b = object,
		.local_byte_offset = match ? (u32)(far - base) : 0,
	};

	pr_emerg("IPA17 FAULT cb=%u sid=%#x far=%#llx fsr=%#x fsynr0=%#x ns=%llu report=%d\n",
		cb, sid, far, fsr, fsynr0, ns, report_ret);
	pr_emerg("IPA17 FAULT_MATCH kind=%u object=%u base=%#llx off=%#llx sid_mask=%#x fsynr1=%#x cbfrsynra=%#x smr=%d\n",
		match, object, (unsigned long long)base,
		match ? (unsigned long long)(far - base) : 0,
		sid_mask, fsynr1, cbfrsynra, (int)smr_idx);

	ipa_diag_freeze();
	ipa_diag_record(&r);

	pr_emerg("IPA17 FAULT_STATE invalid=%u overflow=%u\n",
		ipa_diag_is_invalid(), READ_ONCE(ipa_diag.overflow));
}

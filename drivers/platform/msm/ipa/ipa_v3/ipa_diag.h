/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Round-12 IPA uC SMMU diagnostic ring.
 *
 * Records live in a static, versioned buffer recovered from the crash dump and
 * decoded against the exact vmlinux that produced the capture (symbol
 * ipa_diag); the printed checkpoints keep the literal IPA12 schemas.
 */

#ifndef _IPA_DIAG_H_
#define _IPA_DIAG_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>

#define IPA_DIAG_MAGIC		0x49504131324459ULL	/* "IPA12DY" */
#define IPA_DIAG_VERSION	2u
#define IPA_DIAG_CAPACITY	2048u
#define IPA_DIAG_BUILD_LEN	64
/* One clone per published header slot; see ipahal_diag_tag_img(). */
#define IPA_DIAG_TAG_MAX	256

enum ipa_diag_stage {
	IPA_DIAG_ALLOC = 0,
	IPA_DIAG_TAG,
	IPA_DIAG_PUB,
	IPA_DIAG_ENABLE,
	IPA_DIAG_FAULT,
};

enum ipa_diag_kind {
	IPA_DIAG_RT = 0,
	IPA_DIAG_FLT,
	IPA_DIAG_NONE,
};

/*
 * 88 bytes, no implicit padding: 6*u64 + 6*u32 + 8*u8.
 * @submitted_raw is the word actually placed in the submitted image;
 * @decoded_addr its HAL decode; @clone_dma the tag substituted into it.
 */
struct ipa_diag_rec {
	u64 seq;		/* published last; 0 means not yet valid */
	u64 generation;
	u64 submitted_raw;
	u64 decoded_addr;
	u64 image_dma;
	u64 clone_dma;
	u32 image_byte_offset;
	u32 local_byte_offset;
	u32 length;
	u32 slot;
	s32 pipe;
	s32 ret;
	u32 a;
	u32 b;
	u8 stage;
	u8 kind;
	u8 ip;
	u8 hashed;
	u8 is_sys;
	u8 tagged;
	u8 done;
	u8 pad;
};

struct ipa_diag_buf {
	u64 magic;
	u32 version;
	u32 capacity;
	u32 bufsize;
	u32 count;
	u32 overflow;
	u32 frozen;
	u32 invalid;
	u64 seq_alloc;
	char build[IPA_DIAG_BUILD_LEN];
	struct ipa_diag_rec rec[IPA_DIAG_CAPACITY];
};

extern struct ipa_diag_buf ipa_diag;

void ipa_diag_announce(void);
void ipa_diag_alloc(struct device *dev, struct iommu_domain *domain,
	dma_addr_t dma, size_t size, void *cpu);
void ipa_diag_tag(enum ipa_diag_kind kind, u8 ip, bool hashed, s32 pipe,
	u32 slot, u64 generation, dma_addr_t image_dma, u32 image_byte_offset,
	u32 local_byte_offset, u64 original_raw, u64 submitted_raw,
	u64 decoded_addr, dma_addr_t clone_dma, bool is_sys);
u64 ipa_diag_next_gen(void);
void ipa_diag_pub(u64 gen, enum ipa_diag_kind kind, u8 ip, bool hashed,
	dma_addr_t src, u32 local_offset, u32 length, u32 slots, u32 tagged,
	int ret, bool done);
void ipa_diag_enable(u32 pipe);
void ipa_diag_fault(u32 cb, u32 sid, u32 sid_mask, u64 far, u32 fsr,
	u32 fsynr0, u32 fsynr1, u32 cbfrsynra, u32 smr_idx, u64 ns,
	int report_ret);
void ipa_diag_freeze(void);
bool ipa_diag_is_invalid(void);
void ipa_diag_mark_invalid(const char *why);

#endif /* _IPA_DIAG_H_ */

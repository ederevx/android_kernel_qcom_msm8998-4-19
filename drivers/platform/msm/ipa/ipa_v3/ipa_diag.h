/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Round-13 IPA uC SMMU diagnostic ring.
 *
 * Records live in a static, versioned buffer recovered from the crash dump and
 * decoded against the exact vmlinux that produced the capture (symbol
 * ipa_diag); the printed checkpoints keep the literal IPA13 schemas.
 */

#ifndef _IPA_DIAG_H_
#define _IPA_DIAG_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>

#define IPA_DIAG_MAGIC		0x49504131334459ULL	/* "IPA13DY" */
#define IPA_DIAG_VERSION	3u
#define IPA_DIAG_CAPACITY	2048u
#define IPA_DIAG_BUILD_LEN	64
/* One clone per published header slot; see ipahal_diag_tag_img(). */
#define IPA_DIAG_TAG_MAX	256

/*
 * Translation coverage of the tag clones from the uC context is not asserted:
 * round 12 saw cb=2 resolutions return both zero and non-zero for them.
 */
#define IPA_DIAG_COV_UNKNOWN	0u

enum ipa_diag_stage {
	IPA_DIAG_ALLOC = 0,
	IPA_DIAG_TAG,
	IPA_DIAG_PUB,
	IPA_DIAG_ENABLE,
	IPA_DIAG_FAULT,
	IPA_DIAG_MATCH,
};

enum ipa_diag_kind {
	IPA_DIAG_RT = 0,
	IPA_DIAG_FLT,
	IPA_DIAG_NONE,
};

enum ipa_diag_phase {
	IPA_DIAG_PHASE_NONE = 0,
	IPA_DIAG_PHASE_CHECKPOINT_ENTER,
	IPA_DIAG_PHASE_CHECKPOINT_EXIT,
	IPA_DIAG_PHASE_COMMAND_ENTER,
	IPA_DIAG_PHASE_COMMAND_RETURN,
};

/* FAR classification against the immutable clone registry. */
enum ipa_diag_match_kind {
	IPA_DIAG_MATCH_ORIGINAL = 0,	/* original empty-table range */
	IPA_DIAG_MATCH_TAG,		/* clone registry entry */
	IPA_DIAG_MATCH_NEITHER,
};

#define IPA_DIAG_MATCH_OBJECT	0x1	/* FAR inside the requested object */
#define IPA_DIAG_MATCH_GRANULE	0x2	/* FAR only inside its backing page */

/*
 * 96 bytes, no implicit padding: 6*u64 + 10*u32 + 8*u8.
 * @submitted_raw is the word actually placed in the submitted image, or the
 * fault FAR; @decoded_addr its HAL decode, or the latest completed RT
 * generation in an enable checkpoint; @clone_dma the tag substituted into it,
 * or the latest completed filter generation.
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
	s32 ret;		/* COMMAND_RETURN only for enable records */
	u32 a;
	u32 b;
	u32 tag_alloc;		/* clones allocated so far */
	u32 tagged_slots;	/* slots substituted so far */
	u8 stage;
	u8 kind;
	u8 phase;
	u8 ip;
	u8 hashed;
	u8 is_sys;
	u8 tagged;
	u8 done;
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
	u32 tags_cov;
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
u64 ipa_diag_enable_begin(u32 pipe, u32 ready);
void ipa_diag_enable_phase(u64 seq, u32 pipe, u32 ready,
	enum ipa_diag_phase phase, int command_ret);
void ipa_diag_match(u64 far);
void ipa_diag_fault(u32 cb, u32 sid, u32 sid_mask, u64 far, u32 fsr,
	u32 fsynr0, u32 fsynr1, u32 cbfrsynra, u32 smr_idx, u64 ns,
	int report_ret);
void ipa_diag_freeze(void);
bool ipa_diag_is_invalid(void);
void ipa_diag_mark_invalid(const char *why);

#endif /* _IPA_DIAG_H_ */

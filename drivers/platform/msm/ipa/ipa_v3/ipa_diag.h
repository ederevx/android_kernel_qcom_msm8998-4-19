/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IPA SMMU diagnostics: WDI enable, WLAN-supplied addresses, aliases and the
 * context fault. The ring is decoded against the exact vmlinux that produced
 * the capture (symbol ipa_diag).
 */

#ifndef _IPA_DIAG_H_
#define _IPA_DIAG_H_

#include <linux/types.h>

struct iommu_domain;

#define IPA_DIAG_MAGIC		0x49504131384459ULL
#define IPA_DIAG_VERSION	8u
#define IPA_DIAG_CAPACITY	1024u
#define IPA_DIAG_BUILD_LEN	64

enum ipa_diag_stage {
	IPA_DIAG_ALIAS = 0,
	IPA_DIAG_ALIAS_STATE,
	IPA_DIAG_FAULT,
	IPA_DIAG_WLANMAP,
};

enum ipa_diag_alias_event {
	IPA_DIAG_ALIAS_INSTALLED = 0,
	IPA_DIAG_ALIAS_UNMAPPED = 1,
	IPA_DIAG_ALIAS_UNMAP_FAILED = 3,
};

/* Fault classification against aliases and the WLAN setup addresses. */
enum ipa_diag_fault_match {
	IPA_DIAG_FAULT_NONE = 0,
	IPA_DIAG_FAULT_ALIAS_ORIGINAL = 1,
	IPA_DIAG_FAULT_ALIAS_COMPACT = 2,
	IPA_DIAG_FAULT_WLAN = 3,
};

/*
 * 96 bytes, no implicit padding: 6*u64 + 10*u32 + 8*u8.
 * @submitted_raw: WDI/WLAN supplied value or the fault FAR.
 * @decoded_addr: resulting IOVA, or the fault FSR.
 * @image_dma: alias address, or the matching range base.
 * @clone_dma: backing pa, or fault FSYNR0.
 */
struct ipa_diag_rec {
	u64 seq;
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
	u32 tag_alloc;
	u32 tagged_slots;
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

void ipa_diag_wlanstate(u32 enabled, u32 over_gsi);
void ipa_diag_wlanmap(s32 pipe, u64 supplied, u32 is_pa, u64 iova,
	u32 len, int ret, u32 phase);
void ipa_diag_fault(u32 cb, u32 sid, u32 sid_mask, u64 far, u32 fsr,
	u32 fsynr0, u32 fsynr1, u32 cbfrsynra, u32 smr_idx, u64 ns,
	int report_ret);
void ipa3_diag_smmu_fault(u32 cb, u32 sid, u32 sid_mask, u64 far, u32 fsr,
	u32 fsynr0, u32 fsynr1, u32 cbfrsynra, int smr_idx, int report_ret);
void ipa_diag_freeze(void);
bool ipa_diag_is_invalid(void);
void ipa_diag_mark_invalid(const char *why);

/* Bounded aliases of the live WDI uC mappings. */
u64 ipa_diag_alias_gen_new(void);
void ipa_diag_alias(u32 object, struct iommu_domain *domain, u64 original,
	u64 alias, u64 pa, u64 len, u32 prot, int ret, u64 generation);
void ipa_diag_alias_state(u32 object, u64 generation, u32 event);

#endif /* _IPA_DIAG_H_ */

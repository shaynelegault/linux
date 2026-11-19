/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_ACCOUNT_H
#define _APPLE_SFTL_ACCOUNT_H

#include <linux/types.h>

#define APPLE_SFTL_ACCOUNT_RECORDS 32U

/* Native subtype 5 keeps one little-endian tuple per closed block. */
struct apple_sftl_user_seq {
	u8 data[16];
};

struct apple_sftl_checkpoint;
struct apple_sftl_snapshot;

bool apple_sftl_account_context_pair(const struct apple_sftl_snapshot *snapshot, const u32 blocks[2]);
bool apple_sftl_account_gc_image(const struct apple_sftl_snapshot *before,
				 const struct apple_sftl_checkpoint *image, const u32 blocks[2],
				 u32 donor, u32 landing, u32 live, u16 footer_chunks);
/* Donor component of a post-GC state. The caller has validated its footer,
 * removed its final mapping and knows the counters represent the baseline
 * immediately before this retirement. A native save can precede stats refresh.
 */
int apple_sftl_account_retire(struct apple_sftl_checkpoint *image, u32 block,
			     const u8 *footer_meta);
bool apple_sftl_native_context_tag(u32 index, const u8 *meta);

void apple_sftl_summarize_user_seq(struct apple_sftl_user_seq *out, const u8 *runs, u32 count);
int apple_sftl_account_user_seq(struct apple_sftl_snapshot *snapshot,
				const struct apple_sftl_user_seq *updates);

/* Rebuild the map-derived fields of a 13G36 accounting image. A successful
 * update does not certify the other native accounting or block ownership.
 */
int apple_sftl_account_map(struct apple_sftl_checkpoint *image,
			   const struct apple_sftl_snapshot *map);

#endif

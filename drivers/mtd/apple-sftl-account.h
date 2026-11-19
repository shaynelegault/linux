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
struct apple_sftl_gc_io;
struct apple_sftl_snapshot;

#define APPLE_SFTL_CLEAN_POOL 6

struct apple_sftl_allocation {
	u16 landing;
	u16 erase[APPLE_SFTL_CLEAN_POOL + 1];
	u8 nr_erase;
};

/* Plan one healthy native allocation from current runtime roles and wear.
 * The caller owns capacity validation and must qualify every listed erase.
 */
int apple_sftl_account_plan_allocation(const struct apple_sftl_snapshot *runtime,
				       struct apple_sftl_allocation *plan);

int apple_sftl_account_read_stats(struct apple_sftl_checkpoint *image,
				  const struct apple_sftl_gc_io *io, void *priv);
bool apple_sftl_account_context_pair(const struct apple_sftl_snapshot *snapshot, const u32 blocks[2]);
bool apple_sftl_account_gc_image(const struct apple_sftl_snapshot *before,
				 const struct apple_sftl_checkpoint *image, const u32 blocks[2],
				 u32 donor, u32 landing, u32 live, u16 footer_chunks);
/* The ordinary donor is unmapped and its footer has been verified. */
int apple_sftl_account_retire(struct apple_sftl_checkpoint *image, u32 block,
			     const u8 *footer_meta);

enum apple_sftl_release {
	APPLE_SFTL_ERASED_DONOR,
	APPLE_SFTL_RELEASED_CONTEXT,
};

/* The caller has completed the provider erase or positively identified an
 * unused context reservation released by the native publication protocol.
 */
int apple_sftl_account_release(struct apple_sftl_checkpoint *image, u32 block,
			       enum apple_sftl_release event);
struct apple_sftl_runtime_stats {
	u32 valid, free, data, context, reserved;
	u32 stat37;
	u32 stat38_before_shift;
};
int apple_sftl_account_refresh(struct apple_sftl_checkpoint *image,
			       const struct apple_sftl_runtime_stats *runtime);
bool apple_sftl_native_context_tag(u32 index, const u8 *meta);

void apple_sftl_summarize_user_seq(struct apple_sftl_user_seq *out, const u8 *runs, u32 count);
int apple_sftl_account_user_seq(struct apple_sftl_snapshot *snapshot,
				const struct apple_sftl_user_seq *updates);

/* Rebuild the map-derived fields of a 13G36 accounting image. A successful
 * update does not certify the other native accounting or block ownership.
 */
int apple_sftl_account_map(struct apple_sftl_checkpoint *image,
			   const struct apple_sftl_snapshot *map);
int apple_sftl_account_saved_map(struct apple_sftl_checkpoint *image,
				 const struct apple_sftl_snapshot *map);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_PROVIDER_H
#define _APPLE_SFTL_PROVIDER_H

#include <linux/types.h>

/* IOFlashStorage-730 provider records from the 64 GB P105. Start zeroed.
 * The provider must serialize updates/snapshots, observe every event since
 * the seed and call apple_sftl_provider_lost() if that continuity is lost.
 */
struct apple_sftl_provider_stats {
	u8 vfl[88];	/* TLV 255 */
	u8 lower[464];	/* TLV 254 */
	bool valid;
};

enum apple_sftl_vfl_event {
	APPLE_SFTL_VFL_PROGRAM,
	APPLE_SFTL_VFL_READ,
	APPLE_SFTL_VFL_ERASE,
	APPLE_SFTL_VFL_SCATTER_BEGIN,
	APPLE_SFTL_VFL_SCATTER_ADD,
	APPLE_SFTL_VFL_SCATTER_FLUSH,
};

/* The first three values match the measured lower-provider batch opcodes. */
enum apple_sftl_lower_op {
	APPLE_SFTL_LOWER_READ = 1,
	APPLE_SFTL_LOWER_PROGRAM,
	APPLE_SFTL_LOWER_ERASE,
	APPLE_SFTL_LOWER_SINGLE_ERASE,
	APPLE_SFTL_LOWER_SYNC,
};

int apple_sftl_provider_seed(struct apple_sftl_provider_stats *stats, const u8 *vfl,
			     size_t vfl_bytes, const u8 *lower, size_t lower_bytes);
void apple_sftl_provider_lost(struct apple_sftl_provider_stats *stats);
int apple_sftl_provider_vfl(struct apple_sftl_provider_stats *stats,
			    enum apple_sftl_vfl_event event, u32 chunks);
int apple_sftl_provider_submit(struct apple_sftl_provider_stats *stats,
			       enum apple_sftl_lower_op op, u16 rows);
int apple_sftl_provider_complete(struct apple_sftl_provider_stats *stats,
				 enum apple_sftl_lower_op op, u8 vector_status,
				 const u8 *row_status, u16 rows);
int apple_sftl_provider_single_erase_done(struct apple_sftl_provider_stats *stats,
					  bool post_erase_sync_failed);
int apple_sftl_provider_read_stats(void *priv, u32 id, u8 *data, size_t bytes);

#endif

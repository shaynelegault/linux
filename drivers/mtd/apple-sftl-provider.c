// SPDX-License-Identifier: GPL-2.0-only
/* Replay the native 13G36 provider accounting from an observed checkpoint. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "apple-sftl-provider.h"

static void add_count(u8 *record, size_t offset, u32 count)
{
	u8 *field = record + offset;

	put_unaligned_le64(get_unaligned_le64(field) + count, field);
}

void apple_sftl_provider_lost(struct apple_sftl_provider_stats *stats)
{
	stats->valid = false;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_lost);

static int unknown_event(struct apple_sftl_provider_stats *stats)
{
	apple_sftl_provider_lost(stats);
	return -EOPNOTSUPP;
}

int apple_sftl_provider_seed(struct apple_sftl_provider_stats *stats, const u8 *vfl,
			     size_t vfl_bytes, const u8 *lower, size_t lower_bytes)
{
	if (!vfl || !lower || vfl_bytes != sizeof(stats->vfl) ||
	    lower_bytes != sizeof(stats->lower)) {
		apple_sftl_provider_lost(stats);
		return -EINVAL;
	}
	memcpy(stats->vfl, vfl, vfl_bytes);
	memcpy(stats->lower, lower, lower_bytes);
	stats->valid = true;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_seed);

/* The argument is the number of logical 4 KiB chunks in a native program,
 * contiguous read or scatter flush. The other events count one call.
 * Scatter flush receives the group's actual sum of 16-bit element widths.
 */
int apple_sftl_provider_vfl(struct apple_sftl_provider_stats *stats,
			    enum apple_sftl_vfl_event event, u32 chunks)
{
	u32 calls;

	if (!stats->valid)
		return -ENODATA;

	switch (event) {
	case APPLE_SFTL_VFL_PROGRAM:
		if (!chunks || chunks % 4)
			return unknown_event(stats);
		add_count(stats->vfl, 0, chunks);
		calls = 64;
		break;
	case APPLE_SFTL_VFL_READ:
		add_count(stats->vfl, 8, chunks);
		calls = 24;
		break;
	case APPLE_SFTL_VFL_SCATTER_FLUSH:
		add_count(stats->vfl, 8, chunks);
		calls = 56;
		break;
	case APPLE_SFTL_VFL_ERASE:
		if (chunks)
			return unknown_event(stats);
		add_count(stats->vfl, 16, 1);
		calls = 72;
		break;
	case APPLE_SFTL_VFL_SCATTER_BEGIN:
	case APPLE_SFTL_VFL_SCATTER_ADD:
		if (chunks)
			return unknown_event(stats);
		calls = event == APPLE_SFTL_VFL_SCATTER_BEGIN ? 40 : 48;
		break;
	default:
		return unknown_event(stats);
	}
	add_count(stats->vfl, calls, 1);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_vfl);

/* Batch dispatch (0x8108b278) charges rows before entering the physical
 * provider. Single-row erase (0x8108aba4) and sync (0x8108ad6c) take
 * separate native entry points.
 */
int apple_sftl_provider_submit(struct apple_sftl_provider_stats *stats,
			       enum apple_sftl_lower_op op, u16 rows)
{
	u32 offset;

	if (!stats->valid)
		return -ENODATA;
	switch (op) {
	case APPLE_SFTL_LOWER_READ:
		offset = 8;
		break;
	case APPLE_SFTL_LOWER_PROGRAM:
		offset = 0;
		break;
	case APPLE_SFTL_LOWER_ERASE:
		offset = 16;
		break;
	case APPLE_SFTL_LOWER_SINGLE_ERASE:
		if (rows != 1)
			return unknown_event(stats);
		add_count(stats->lower, 104, 1);
		offset = 16;
		break;
	case APPLE_SFTL_LOWER_SYNC:
		if (rows != 1)
			return unknown_event(stats);
		offset = 24;
		break;
	default:
		return unknown_event(stats);
	}
	add_count(stats->lower, offset, rows);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_submit);

/* Native completion (0x81089c68) reads element flags at +38, stride 12.
 * The caller must retain the original provider result through translation.
 */
int apple_sftl_provider_complete(struct apple_sftl_provider_stats *stats,
				 enum apple_sftl_lower_op op, u8 vector_status,
				 const u8 *row_status, u16 rows)
{
	u32 i;

	if (!stats->valid)
		return -ENODATA;
	if (op != APPLE_SFTL_LOWER_READ && op != APPLE_SFTL_LOWER_PROGRAM &&
	    op != APPLE_SFTL_LOWER_ERASE)
		return unknown_event(stats);
	if (vector_status == 0x40)
		return 0;
	if (rows && !row_status)
		return unknown_event(stats);
	for (i = 0; i < rows; i++) {
		u8 flags = row_status[i];

		if (op == APPLE_SFTL_LOWER_READ) {
			if (flags & BIT(3))
				add_count(stats->lower, 120, 1);
			else if (flags & BIT(0))
				add_count(stats->lower, 128, 1);
			if (flags & BIT(4))
				add_count(stats->lower, 136, 1);
			if (flags & BIT(1))
				add_count(stats->lower, 448, 1);
			if (flags & BIT(2))
				add_count(stats->lower, 456, 1);
		} else {
			bool program = op == APPLE_SFTL_LOWER_PROGRAM;

			if (flags & BIT(0))
				add_count(stats->lower, program ? 152 : 168, 1);
			if (flags & BIT(4))
				add_count(stats->lower, program ? 144 : 160, 1);
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_complete);

int apple_sftl_provider_single_erase_done(struct apple_sftl_provider_stats *stats,
					  bool post_erase_sync_failed)
{
	if (!stats->valid)
		return -ENODATA;
	if (post_erase_sync_failed)
		add_count(stats->lower, 168, 1);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_single_erase_done);

int apple_sftl_provider_read_stats(void *priv, u32 id, u8 *data, size_t bytes)
{
	const struct apple_sftl_provider_stats *stats = priv;

	if (!stats || !stats->valid)
		return -ENODATA;
	if (!data)
		return -EINVAL;
	if (id == 255 && bytes == sizeof(stats->vfl))
		memcpy(data, stats->vfl, bytes);
	else if (id == 254 && bytes == sizeof(stats->lower))
		memcpy(data, stats->lower, bytes);
	else
		return -EINVAL;
	return (int)bytes;
}
EXPORT_SYMBOL_GPL(apple_sftl_provider_read_stats);

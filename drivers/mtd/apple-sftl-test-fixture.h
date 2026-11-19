/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_TEST_FIXTURE_H
#define _APPLE_SFTL_TEST_FIXTURE_H

#include <linux/kernel.h>
#include <linux/unaligned.h>
#include "apple-sftl-reclaim.h"

/* Populate the measured layout in 32 zeroed synthetic records. Callers place
 * these in their own generation and supply its context block addresses.
 */
static inline void apple_sftl_test_native_prefix(struct apple_sftl_context_record *records)
{
	static const struct {
		u8 subtype, chunks;
		u16 entries;
	} groups[] = { { 1, 4, 0 }, { 2, 4, 0 }, { 3, 8, 512 },
		       { 5, 12, 256 }, { 6, 4, 1024 } };
	u32 i, pos, j, cursor = 0;
	u8 *stats;

	for (i = 0; i < ARRAY_SIZE(groups); i++) {
		for (j = 0; j < groups[i].chunks; j++) {
			u8 *meta = records[cursor++].meta;
			u32 start = j * groups[i].entries;
			u16 n = start >= APPLE_SFTL_SUBBLOCKS ? 0 :
				min_t(u32, groups[i].entries, APPLE_SFTL_SUBBLOCKS - start);

			meta[0] = 0x1f;
			meta[1] = groups[i].subtype;
			put_unaligned_le16(start, meta + 8);
			put_unaligned_le16(n, meta + 10);
			put_unaligned_le32(U32_MAX, meta + 12);
		}
	}
	put_unaligned_le32(1, records[0].data);
	put_unaligned_le32(1, records[0].data + 4);
	put_unaligned_le32(APPLE_SFTL_SUBBLOCKS, records[3].data + APPLE_SFTL_CHUNK - 4);
	stats = records[4].data;
	put_unaligned_le32(45, stats);
	pos = 4;
	for (i = 0; i < 45; i++) {
		u32 words = i < 43 ? 2 : (i == 43 ? 22 : 116);

		put_unaligned_le32(i < 43 ? i + 1 : (i == 43 ? 255 : 254), stats + pos);
		put_unaligned_le32(words, stats + pos + 4);
		pos += 8 + words * 4;
	}
}
#endif

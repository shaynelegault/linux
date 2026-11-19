// SPDX-License-Identifier: GPL-2.0-only
/*
 * SwissPPNFTL accounting on 13G36 64 GB media. cxtSaveSB at 0x8108e7e4
 * serializes the resident valid-LBA count into bytes 2..3 of each entry.
 * Stat 9 counts all valid LBAs, including internal keys past the disk end.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "apple-sftl-account.h"
#include "apple-sftl-reclaim.h"

#define SFTL_STATS_START	4
#define SFTL_SB_START		8
#define SFTL_USER_SEQ_START	16
#define SFTL_READ_COUNT_START	28
#define SFTL_SB_ENTRY_BYTES	8
#define SFTL_SB_PER_RECORD	(APPLE_SFTL_CHUNK / SFTL_SB_ENTRY_BYTES)

bool apple_sftl_native_context_tag(u32 index, const u8 *meta)
{
	u32 start = 0, entries = 0, stride = 0;
	u8 subtype;

	if (index < SFTL_STATS_START)
		subtype = 1;
	else if (index < SFTL_SB_START)
		subtype = 2;
	else if (index < SFTL_USER_SEQ_START) {
		subtype = 3;
		stride = SFTL_SB_PER_RECORD;
		start = (index - SFTL_SB_START) * stride;
	} else if (index < SFTL_READ_COUNT_START) {
		subtype = 5;
		stride = APPLE_SFTL_CHUNK / 16;
		start = (index - SFTL_USER_SEQ_START) * stride;
	} else if (index < APPLE_SFTL_ACCOUNT_RECORDS) {
		subtype = 6;
		stride = APPLE_SFTL_CHUNK / 4;
		start = (index - SFTL_READ_COUNT_START) * stride;
	} else {
		subtype = 4;
	}
	if (start < APPLE_SFTL_SUBBLOCKS)
		entries = min(stride, APPLE_SFTL_SUBBLOCKS - start);
	return meta && meta[0] == 0x1f && meta[1] == subtype &&
		get_unaligned_le16(meta + 8) == start &&
		get_unaligned_le16(meta + 10) == entries &&
		get_unaligned_le32(meta + 12) == U32_MAX;
}

/* Native close (0x8108bd60) samples the user sequence high word once per
 * footer run. Lengths and skipped U32_MAX values do not enter the mean.
 */
void apple_sftl_summarize_user_seq(struct apple_sftl_user_seq *out, const u8 *runs, u32 count)
{
	u32 i, low = U32_MAX, high = 0, samples = 0;
	u64 sum = 0;

	for (i = 0; i < count; i++, runs += APPLE_SFTL_META) {
		u32 seq = get_unaligned_le32(runs + 4);

		if (seq == U32_MAX)
			continue;
		low = min(low, seq);
		high = max(high, seq);
		sum += seq;
		samples++;
	}
	if (!samples) {
		low = 0;
		samples = 1;
	}
	put_unaligned_le32(low, out->data);
	put_unaligned_le32(high, out->data + 4);
	put_unaligned_le32(div_u64(sum, samples), out->data + 8);
	put_unaligned_le32(samples, out->data + 12);
}
EXPORT_SYMBOL_GPL(apple_sftl_summarize_user_seq);

static bool sftl_native_prefix(const struct apple_sftl_context_record *records, u32 count)
{
	u32 i;

	if (!records || count < APPLE_SFTL_ACCOUNT_RECORDS ||
	    get_unaligned_le32(records[3].data + APPLE_SFTL_CHUNK - 4) != APPLE_SFTL_SUBBLOCKS)
		return false;
	for (i = 0; i < APPLE_SFTL_ACCOUNT_RECORDS; i++)
		if (!apple_sftl_native_context_tag(i, records[i].meta))
			return false;
	return true;
}

static u8 *sftl_sb_entry(struct apple_sftl_context_record *records, u32 block)
{
	u32 record = SFTL_SB_START + block / SFTL_SB_PER_RECORD;
	u32 offset = block % SFTL_SB_PER_RECORD * SFTL_SB_ENTRY_BYTES;

	return records[record].data + offset;
}

bool apple_sftl_account_context_pair(const struct apple_sftl_snapshot *snapshot,
				    const u32 blocks[2])
{
	u32 i;

	if (!snapshot || snapshot->nr_opaque != APPLE_SFTL_ACCOUNT_RECORDS || !blocks ||
	    blocks[0] == blocks[1] || !sftl_native_prefix(snapshot->opaque, snapshot->nr_opaque))
		return false;
	for (i = 0; i < 2; i++) {
		const u8 *entry;

		if (!blocks[i] || blocks[i] >= APPLE_SFTL_SUBBLOCKS)
			return false;
		entry = sftl_sb_entry(snapshot->opaque, blocks[i]);
		/* Recovery assigns class 7 to context (0x1f) at 0x8108d762.
		 * The context allocator independently assigns it at 0x810937a4.
		 */
		if (entry[0] != 7 || get_unaligned_le16(entry + 2))
			return false;
	}
	return true;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_context_pair);

int apple_sftl_account_user_seq(struct apple_sftl_snapshot *snapshot,
				const struct apple_sftl_user_seq *updates)
{
	u32 block, per_record = APPLE_SFTL_CHUNK / sizeof(*updates);

	if (snapshot->nr_opaque != APPLE_SFTL_ACCOUNT_RECORDS ||
	    !sftl_native_prefix(snapshot->opaque, snapshot->nr_opaque))
		return -EBADMSG;
	for (block = 0; block < APPLE_SFTL_SUBBLOCKS; block++) {
		u32 record = SFTL_USER_SEQ_START + block / per_record;
		u32 offset = block % per_record * sizeof(*updates);

		/* A native close always has a nonzero sample count. */
		if (get_unaligned_le32(updates[block].data + 12))
			memcpy(snapshot->opaque[record].data + offset,
			       updates[block].data, sizeof(*updates));
	}
	return 0;
}

static int sftl_account_layout(const struct apple_sftl_checkpoint *image)
{
	u32 i;

	if (image->count <= APPLE_SFTL_ACCOUNT_RECORDS || image->count % 4 ||
	    !sftl_native_prefix(image->records, image->count))
		return -EOPNOTSUPP;
	for (i = APPLE_SFTL_ACCOUNT_RECORDS; i < image->count; i++)
		if (!apple_sftl_native_context_tag(i, image->records[i].meta))
			return -EOPNOTSUPP;
	return 0;
}

static u8 *sftl_stats(struct apple_sftl_checkpoint *image)
{
	u8 *payload = image->records[SFTL_STATS_START].data;
	u32 i, offset = sizeof(u32);

	/* The measured stats payload is a word-counted TLV list. IDs 1..43
	 * carry 64-bit counters, followed by two private variable-size blobs.
	 */
	if (get_unaligned_le32(payload) != 45)
		return NULL;
	for (i = 0; i < 45; i++) {
		u32 id, words;

		if (offset > APPLE_SFTL_CHUNK - 8)
			return NULL;
		id = get_unaligned_le32(payload + offset);
		words = get_unaligned_le32(payload + offset + 4);
		offset += 8;
		if (words > (APPLE_SFTL_CHUNK - offset) / 4 ||
		    (i < 43 && (id != i + 1 || words != 2)) ||
		    (i == 43 && (id != 255 || words != 22)) ||
		    (i == 44 && (id != 254 || words != 116)))
			return NULL;
		offset += words * 4;
	}
	return payload;
}

/* sftl_stats has checked all of the fixed-size leading counters. */
static u8 *sftl_stat(u8 *stats, u32 id)
{
	return stats + sizeof(u32) + (id - 1) * 16 + 8;
}

/* Apply the native post-GC state of a verified, presently unmapped donor.
 * It is dirty/free (9) at this point. Erase success and wear are later events.
 */
int apple_sftl_account_retire(struct apple_sftl_checkpoint *image, u32 block, const u8 *footer_meta)
{
	u32 reads_per_record = APPLE_SFTL_CHUNK / sizeof(u32);
	u8 *entry, *reads, *stats;
	u64 data, free;
	u16 chunks;
	int ret;

	if (!image || !image->records || !footer_meta || !block || block >= APPLE_SFTL_SUBBLOCKS)
		return -EINVAL;
	ret = sftl_account_layout(image);
	if (ret)
		return ret;
	chunks = get_unaligned_le16(footer_meta + 12);
	if (footer_meta[0] != 0x1c || chunks < 4 || chunks > 32 || chunks % 4 ||
	    footer_meta[1] >> 1 != chunks || get_unaligned_le16(footer_meta + 14) != 0xffff)
		return -EOPNOTSUPP;
	entry = sftl_sb_entry(image->records, block);
	if (entry[0] != 3 || entry[1] != footer_meta[1] || get_unaligned_le16(entry + 2))
		return -EOPNOTSUPP;
	stats = sftl_stats(image);
	if (!stats)
		return -EOPNOTSUPP;
	free = get_unaligned_le64(sftl_stat(stats, 10));
	data = get_unaligned_le64(sftl_stat(stats, 11));
	if (free >= APPLE_SFTL_SUBBLOCKS || !data || data > APPLE_SFTL_SUBBLOCKS - free)
		return -EBADMSG;
	reads = image->records[SFTL_READ_COUNT_START + block / reads_per_record].data +
		(block % reads_per_record) * sizeof(u32);
	/* cxt's stats IDs 10/11 copy native free/data counts (0x81094be2).
	 * Final invalidation clears reads and moves one block at 0x81090ade.
	 */
	entry[0] = 9;
	put_unaligned_le32(0, reads);
	put_unaligned_le64(free + 1, sftl_stat(stats, 10));
	put_unaligned_le64(data - 1, sftl_stat(stats, 11));
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_retire);

static int sftl_account_runs(const struct apple_sftl_snapshot *map, u16 *counts, u32 *total)
{
	u32 cursor = 0, i;

	for (i = 0; i < map->nr_runs; i++) {
		const struct apple_sftl_extent *run = &map->runs[i];
		u32 length = run->length, addr = run->provider;

		if (run->logical != cursor || !length || length > APPLE_SFTL_LOGICAL_PAGES - cursor)
			return -EBADMSG;
		cursor += length;
		if (addr == APPLE_SFTL_NO_ADDRESS)
			continue;
		if (addr >= APPLE_SFTL_SUBBLOCKS * APPLE_SFTL_STRIDE ||
		    length > APPLE_SFTL_SUBBLOCKS * APPLE_SFTL_STRIDE - addr)
			return -EBADMSG;
		*total += length;
		while (length) {
			u32 block = addr / APPLE_SFTL_STRIDE;
			u32 n = min(length, APPLE_SFTL_STRIDE - addr % APPLE_SFTL_STRIDE);

			if (counts[block] > APPLE_SFTL_STRIDE - n)
				return -EBADMSG;
			counts[block] += n;
			addr += n;
			length -= n;
		}
	}
	return cursor == APPLE_SFTL_LOGICAL_PAGES ? 0 : -EBADMSG;
}

int apple_sftl_account_map(struct apple_sftl_checkpoint *image,
			   const struct apple_sftl_snapshot *map)
{
	u32 old_total = 0, total = 0, block;
	u8 *valid;
	u16 *counts;
	int ret;

	if (!image || !image->records || !map || !map->runs || !map->nr_runs ||
	    map->nr_runs > APPLE_SFTL_LOGICAL_PAGES)
		return -EINVAL;
	ret = sftl_account_layout(image);
	if (ret)
		return ret;
	valid = sftl_stats(image);
	if (!valid)
		return -EOPNOTSUPP;
	valid = sftl_stat(valid, 9);
	for (block = 0; block < APPLE_SFTL_SUBBLOCKS; block++) {
		u16 n = get_unaligned_le16(sftl_sb_entry(image->records, block) + 2);

		if (n > APPLE_SFTL_STRIDE)
			return -EBADMSG;
		old_total += n;
	}
	if (get_unaligned_le64(valid) != old_total)
		return -EBADMSG;

	counts = kcalloc(APPLE_SFTL_SUBBLOCKS, sizeof(*counts), GFP_NOIO);
	if (!counts)
		return -ENOMEM;
	ret = sftl_account_runs(map, counts, &total);
	if (!ret) {
		for (block = 0; block < APPLE_SFTL_SUBBLOCKS; block++)
			put_unaligned_le16(counts[block], sftl_sb_entry(image->records, block) + 2);
		put_unaligned_le64(total, valid);
	}
	kfree(counts);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_map);

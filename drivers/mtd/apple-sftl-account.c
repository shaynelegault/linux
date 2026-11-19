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
#define SFTL_MAX_WEAR		0xffff

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

struct sftl_free_block {
	u32 block;
	u32 wear;
};

/* The refill list has six fixed slots, not a sorted wear queue. On equal
 * wear the native scan keeps the first occurrence; refill uses slot order.
 */
static void sftl_add_dirty(struct sftl_free_block pool[], u32 block, u16 wear)
{
	u32 i, replace = 0;

	for (i = 0; i < APPLE_SFTL_CLEAN_POOL; i++) {
		if (pool[i].block == U32_MAX) {
			replace = i;
			break;
		}
		if (pool[i].wear > pool[replace].wear)
			replace = i;
	}
	if (wear < pool[replace].wear)
		pool[replace] = (struct sftl_free_block) { block, wear };
}

/* Healthy 13G36 free-block allocation at 0x810932f8. Firmware first
 * replenishes the clean pool. It prefers a clean block within one wear
 * count of the least worn free block, using the candidates from the scan
 * before replenishment. A selected dirty block may already be in the pool.
 */
int apple_sftl_account_plan_allocation(const struct apple_sftl_snapshot *runtime,
				       struct apple_sftl_allocation *plan)
{
	struct sftl_free_block pool[APPLE_SFTL_CLEAN_POOL];
	struct sftl_free_block clean = { U32_MAX, U32_MAX }, least = clean;
	struct apple_sftl_allocation result = {};
	u32 i, nr_clean = 0;
	bool erased = false;

	if (!plan || !runtime || runtime->nr_opaque != APPLE_SFTL_ACCOUNT_RECORDS ||
	    !sftl_native_prefix(runtime->opaque, runtime->nr_opaque))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(pool); i++)
		pool[i] = (struct sftl_free_block) { U32_MAX, SFTL_MAX_WEAR };
	for (i = 0; i < APPLE_SFTL_SUBBLOCKS; i++) {
		const u8 *entry = sftl_sb_entry(runtime->opaque, i);
		u32 wear = get_unaligned_le16(entry + 6);

		if (entry[0] != 1 && entry[0] != 2 && entry[0] != 9)
			continue;
		if (!i || get_unaligned_le16(entry + 2))
			return -EBADMSG;
		if (wear < least.wear)
			least = (struct sftl_free_block) { i, wear };
		if (entry[0] == 1) {
			nr_clean++;
			if (wear < clean.wear)
				clean = (struct sftl_free_block) { i, wear };
		} else {
			sftl_add_dirty(pool, i, wear);
		}
	}
	if (least.block == U32_MAX)
		return -ENOSPC;
	result.landing = least.wear + 1 < clean.wear ? least.block : clean.block;
	for (i = 0; nr_clean < APPLE_SFTL_CLEAN_POOL && i < ARRAY_SIZE(pool); i++) {
		if (pool[i].block == U32_MAX)
			break;
		result.erase[result.nr_erase++] = pool[i].block;
		erased |= pool[i].block == result.landing;
		nr_clean++;
	}
	if (!erased && sftl_sb_entry(runtime->opaque, result.landing)[0] != 1) {
		/* The native halfword wraps. Leave that policy unqualified. */
		if (least.wear == SFTL_MAX_WEAR)
			return -EOVERFLOW;
		result.erase[result.nr_erase++] = result.landing;
	}
	*plan = result;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_plan_allocation);

static bool sftl_account_context_pair(struct apple_sftl_context_record *records, u32 count,
				      const u32 blocks[2])
{
	u32 i;

	if (!blocks || blocks[0] == blocks[1] || !sftl_native_prefix(records, count))
		return false;
	for (i = 0; i < 2; i++) {
		const u8 *entry;

		if (!blocks[i] || blocks[i] >= APPLE_SFTL_SUBBLOCKS)
			return false;
		entry = sftl_sb_entry(records, blocks[i]);
		/* Recovery assigns class 7 to context (0x1f) at 0x8108d762.
		 * The context allocator independently assigns it at 0x810937a4.
		 */
		if (entry[0] != 7 || get_unaligned_le16(entry + 2))
			return false;
	}
	return true;
}

bool apple_sftl_account_context_pair(const struct apple_sftl_snapshot *snapshot,
				    const u32 blocks[2])
{
	return snapshot && snapshot->nr_opaque == APPLE_SFTL_ACCOUNT_RECORDS &&
		sftl_account_context_pair(snapshot->opaque, snapshot->nr_opaque, blocks);
}
EXPORT_SYMBOL_GPL(apple_sftl_account_context_pair);

/* The next generation is saved after verified ordinary copies, before source
 * erase and before the caller's next write. Source footer reads have already
 * established its width. This writer emits the ordinary (stream zero) tags.
 */
bool apple_sftl_account_gc_image(const struct apple_sftl_snapshot *before,
				 const struct apple_sftl_checkpoint *image, const u32 blocks[2],
				 u32 donor, u32 landing, u32 live, u16 footer_chunks)
{
	u32 block, old, i, seen = 0, reads_per_record = APPLE_SFTL_CHUNK / sizeof(u32);
	const u8 *old_source, *source, *destination, *reads;

	if (!sftl_native_prefix(before->opaque, before->nr_opaque) ||
	    image->count <= APPLE_SFTL_ACCOUNT_RECORDS ||
	    !sftl_account_context_pair(image->records, image->count, blocks) ||
	    !donor || donor >= APPLE_SFTL_SUBBLOCKS || !landing || landing >= APPLE_SFTL_SUBBLOCKS ||
	    !live || footer_chunks < 4 || footer_chunks > 32 || footer_chunks % 4)
		return false;
	/* This bounded path retires one previously occupied context block.
	 * The save names freshly allocated sites and still serializes the old
	 * role/wear before any retirement, including when the old site is full.
	 */
	old = get_unaligned_le32(before->opaque[0].data + 4);
	if (!old || old >= APPLE_SFTL_SUBBLOCKS || old == blocks[0] || old == blocks[1] ||
	    sftl_sb_entry(before->opaque, old)[0] != 7 ||
	    get_unaligned_le16(sftl_sb_entry(before->opaque, old) + 6) == SFTL_MAX_WEAR ||
	    memcmp(sftl_sb_entry(before->opaque, old), sftl_sb_entry(image->records, old), 8))
		return false;
	for (i = 0; i < 2; i++) {
		const u8 *prior = sftl_sb_entry(before->opaque, blocks[i]);
		const u8 *next = sftl_sb_entry(image->records, blocks[i]);

		if (prior[0] != 1 || get_unaligned_le16(prior + 2) ||
		    (i == 1 && get_unaligned_le16(prior + 6) == SFTL_MAX_WEAR) ||
		    get_unaligned_le16(prior + 6) != get_unaligned_le16(next + 6))
			return false;
	}
	old_source = sftl_sb_entry(before->opaque, donor);
	source = sftl_sb_entry(image->records, donor);
	destination = sftl_sb_entry(image->records, landing);
	reads = image->records[SFTL_READ_COUNT_START + donor / reads_per_record].data +
		(donor % reads_per_record) * sizeof(u32);
	if (old_source[0] != 3 || source[0] != 9 || source[1] != old_source[1] ||
	    get_unaligned_le16(source + 6) == SFTL_MAX_WEAR ||
	    source[1] >> 1 != footer_chunks ||
	    get_unaligned_le16(source + 6) != get_unaligned_le16(old_source + 6) ||
	    get_unaligned_le16(source + 2) || get_unaligned_le32(reads) ||
	    destination[0] != 4 || destination[1] || get_unaligned_le16(destination + 2) != live)
		return false;
	for (block = 0; block < APPLE_SFTL_SUBBLOCKS; block++) {
		const u8 *entry = sftl_sb_entry(image->records, block);

		if (entry[0] != 4)
			continue;
		if (entry[1] > 1 || (seen & (1U << entry[1])))
			return false;
		seen |= 1U << entry[1];
	}
	return true;
}

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

static u8 *sftl_stats(struct apple_sftl_checkpoint *image, u8 **provider_stats)
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
		if (provider_stats && i >= 43)
			provider_stats[i - 43] = payload + offset;
		offset += words * 4;
	}
	return payload;
}

/* Native cxtSave writes its base page before fetching these two records.
 * The caller owns the pending image and discards it upon a short/error read.
 */
int apple_sftl_account_read_stats(struct apple_sftl_checkpoint *image,
				  const struct apple_sftl_gc_io *io, void *priv)
{
	u8 *data[2];
	int ret;

	if (!sftl_stats(image, data))
		return -EBADMSG;
	ret = io->read_native_stats(priv, 255, data[0], 88);
	if (ret != 88)
		return ret < 0 ? ret : -EIO;
	ret = io->read_native_stats(priv, 254, data[1], 464);
	return ret == 464 ? 0 : (ret < 0 ? ret : -EIO);
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
	u8 *entry, *reads;
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
	reads = image->records[SFTL_READ_COUNT_START + block / reads_per_record].data +
		(block % reads_per_record) * sizeof(u32);
	/* At 0x81090ade final invalidation only updates the entry and runtime
	 * totals. A save can see the previous cached free and data statistics.
	 */
	entry[0] = 9;
	put_unaligned_le32(0, reads);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_retire);

int apple_sftl_account_release(struct apple_sftl_checkpoint *image, u32 block,
			       enum apple_sftl_release event)
{
	u8 *entry;
	u16 wear;
	int ret;

	if (!image || !image->records || !block || block >= APPLE_SFTL_SUBBLOCKS ||
	    (event != APPLE_SFTL_ERASED_DONOR && event != APPLE_SFTL_RELEASED_CONTEXT))
		return -EINVAL;
	ret = sftl_account_layout(image);
	if (ret)
		return ret;
	entry = sftl_sb_entry(image->records, block);
	if (entry[0] != (event == APPLE_SFTL_ERASED_DONOR ? 9 : 7) ||
	    get_unaligned_le16(entry + 2))
		return -EOPNOTSUPP;
	wear = get_unaligned_le16(entry + 6);
	if (wear == SFTL_MAX_WEAR)
		return -EOVERFLOW;
	/* 0x81093958 follows completed ordinary erase; 0x81093848 also
	 * counts release of an unused context, which requires no erase.
	 */
	entry[0] = 1;
	put_unaligned_le16(wear + 1, entry + 6);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_release);

/* These seven fields, and only these fields, are sampled by 0x81094bcc.
 * Call only at the observed refresh boundary, with current runtime inputs.
 */
int apple_sftl_account_refresh(struct apple_sftl_checkpoint *image,
			       const struct apple_sftl_runtime_stats *runtime)
{
	u8 *stats;
	int ret;

	if (!image || !image->records || !runtime)
		return -EINVAL;
	ret = sftl_account_layout(image);
	if (ret)
		return ret;
	stats = sftl_stats(image, NULL);
	if (!stats)
		return -EOPNOTSUPP;
	if (runtime->valid > APPLE_SFTL_LOGICAL_PAGES ||
	    (u64)runtime->free + runtime->data + runtime->context + runtime->reserved >
			APPLE_SFTL_SUBBLOCKS)
		return -EBADMSG;
	put_unaligned_le64(runtime->valid, sftl_stat(stats, 9));
	put_unaligned_le64(runtime->free, sftl_stat(stats, 10));
	put_unaligned_le64(runtime->data, sftl_stat(stats, 11));
	put_unaligned_le64(runtime->context, sftl_stat(stats, 12));
	put_unaligned_le64(runtime->reserved, sftl_stat(stats, 13));
	put_unaligned_le64(runtime->stat37, sftl_stat(stats, 37));
	put_unaligned_le64(runtime->stat38_before_shift >> 6, sftl_stat(stats, 38));
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_account_refresh);

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

static int sftl_update_map_counts(struct apple_sftl_checkpoint *image,
				  const struct apple_sftl_snapshot *map, bool preserve_cache)
{
	u32 old_total = 0, total = 0, block;
	u8 *valid, *stats;
	u16 *counts;
	int ret;

	if (!image || !image->records || !map || !map->runs || !map->nr_runs ||
	    map->nr_runs > APPLE_SFTL_LOGICAL_PAGES)
		return -EINVAL;
	ret = sftl_account_layout(image);
	if (ret)
		return ret;
	stats = sftl_stats(image, NULL);
	if (!stats)
		return -EOPNOTSUPP;
	valid = sftl_stat(stats, 9);
	for (block = 0; block < APPLE_SFTL_SUBBLOCKS; block++) {
		u16 n = get_unaligned_le16(sftl_sb_entry(image->records, block) + 2);

		if (n > APPLE_SFTL_STRIDE)
			return -EBADMSG;
		old_total += n;
	}
	if (preserve_cache) {
		u32 id;

		if (get_unaligned_le64(valid) > APPLE_SFTL_LOGICAL_PAGES)
			return -EBADMSG;
		for (id = 10; id <= 13; id++)
			if (get_unaligned_le64(sftl_stat(stats, id)) > APPLE_SFTL_SUBBLOCKS)
				return -EBADMSG;
	} else if (get_unaligned_le64(valid) != old_total) {
		return -EBADMSG;
	}

	counts = kcalloc(APPLE_SFTL_SUBBLOCKS, sizeof(*counts), GFP_NOIO);
	if (!counts)
		return -ENOMEM;
	ret = sftl_account_runs(map, counts, &total);
	if (!ret) {
		for (block = 0; block < APPLE_SFTL_SUBBLOCKS; block++)
			put_unaligned_le16(counts[block], sftl_sb_entry(image->records, block) + 2);
		if (!preserve_cache)
			put_unaligned_le64(total, valid);
	}
	kfree(counts);
	return ret;
}

int apple_sftl_account_map(struct apple_sftl_checkpoint *image,
			   const struct apple_sftl_snapshot *map)
{
	return sftl_update_map_counts(image, map, false);
}
EXPORT_SYMBOL_GPL(apple_sftl_account_map);

int apple_sftl_account_saved_map(struct apple_sftl_checkpoint *image,
				 const struct apple_sftl_snapshot *map)
{
	return sftl_update_map_counts(image, map, true);
}
EXPORT_SYMBOL_GPL(apple_sftl_account_saved_map);

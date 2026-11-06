// SPDX-License-Identifier: GPL-2.0-only
/*
 * SwissPPNFTL checkpoint serialization and offline reclamation transactions.
 * Native metadata: 13G36 IOFlashStorage-730 formatter at 0x81092408.
 * Range payload: cxtNextBatch at 0x8108f07c and the recovery decoder.
 *
 * Opaque accounting payloads survive byte-for-byte. Updating their native
 * accounting and committing VFL bank rotation remain separate work. No erase
 * callback or runtime GC worker is registered by this file.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "apple-sftl-reclaim.h"

#define SFTL_SEQUENCE GENMASK_ULL(47, 0)
#define SFTL_PROVIDER (APPLE_SFTL_SUBBLOCKS * APPLE_SFTL_STRIDE)
#define SFTL_PAIRS ((APPLE_SFTL_CHUNK - 8) / 8)
#define SFTL_MAX_RECORDS (16U * APPLE_SFTL_STRIDE)
#define SFTL_UNMAPPED 0x1ffffffU

struct apple_sftl_gc {
	struct apple_sftl_snapshot snapshot;
	struct apple_sftl_gc_copy *copies;
	struct apple_sftl_checkpoint *checkpoint;
	u32 nr_copies, copied, max_records, victim, destination;
	u64 retire_sequence, copied_sequence;
	enum apple_sftl_gc_stage stage;
};

int apple_sftl_context_meta(u8 *meta, u8 subtype, u64 sequence,
			    u16 index, u16 entries)
{
	if (!meta || !subtype || subtype > 6 || sequence > SFTL_SEQUENCE)
		return -EINVAL;
	put_unaligned_le64(sequence << 16 | (u16)subtype << 8 | 0x1f, meta);
	put_unaligned_le16(index, meta + 8);
	put_unaligned_le16(entries, meta + 10);
	put_unaligned_le32(U32_MAX, meta + 12);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_context_meta);

static int sftl_check_map(const struct apple_sftl_snapshot *snapshot)
{
	u32 cursor = 0, i;

	if (!snapshot || !snapshot->runs || !snapshot->nr_runs ||
	    snapshot->nr_runs > APPLE_SFTL_LOGICAL_PAGES ||
	    snapshot->checkpoint_sequence > snapshot->sequence ||
	    snapshot->journal_sequence > snapshot->sequence ||
	    snapshot->sequence > SFTL_SEQUENCE)
		return -EINVAL;
	for (i = 0; i < snapshot->nr_runs; i++) {
		const struct apple_sftl_extent *run = &snapshot->runs[i];

		if (run->logical != cursor || !run->length ||
		    run->length > APPLE_SFTL_LOGICAL_PAGES - cursor ||
		    (run->provider != APPLE_SFTL_NO_ADDRESS &&
		     (run->provider >= SFTL_PROVIDER ||
		      run->length > SFTL_PROVIDER - run->provider)))
			return -EBADMSG;
		cursor += run->length;
	}
	return cursor == APPLE_SFTL_LOGICAL_PAGES ? 0 : -EBADMSG;
}

static int sftl_check_context(const struct apple_sftl_snapshot *snapshot)
{
	u32 i, previous = 0, names;

	for (i = 0; i < snapshot->nr_opaque; i++) {
		u8 subtype = snapshot->opaque[i].meta[1];
		u32 order = subtype >= 5 ? subtype - 1 : subtype;

		if (snapshot->opaque[i].meta[0] != 0x1f || !subtype ||
		    subtype == 4 || subtype > 6 || (!i && subtype != 1) ||
		    order < previous)
			return -EBADMSG;
		previous = order;
	}
	names = get_unaligned_le32(snapshot->opaque[0].data);
	if (!names || names > 16)
		return -EBADMSG;
	return 0;
}

static int sftl_checkpoint_count(const struct apple_sftl_snapshot *snapshot,
				 u32 max_records, u32 *count)
{
	u32 records;
	int ret = sftl_check_map(snapshot);

	if (ret)
		return ret;
	if (!snapshot->opaque || !snapshot->nr_opaque ||
	    snapshot->nr_opaque > SFTL_MAX_RECORDS ||
	    !max_records || max_records > SFTL_MAX_RECORDS)
		return -EINVAL;
	ret = sftl_check_context(snapshot);
	if (ret)
		return ret;
	records = snapshot->nr_opaque + (snapshot->nr_runs + SFTL_PAIRS - 1) / SFTL_PAIRS;
	records = (records + 3) & ~3U;
	if (records > max_records)
		return -ENOSPC;
	if (records > SFTL_SEQUENCE - snapshot->sequence)
		return -EOVERFLOW;
	*count = records;
	return 0;
}

void apple_sftl_snapshot_free(struct apple_sftl_snapshot *snapshot)
{
	if (!snapshot)
		return;
	kvfree(snapshot->runs);
	kvfree(snapshot->opaque);
	kfree(snapshot);
}
EXPORT_SYMBOL_GPL(apple_sftl_snapshot_free);

void apple_sftl_checkpoint_free(struct apple_sftl_checkpoint *checkpoint)
{
	if (!checkpoint)
		return;
	kvfree(checkpoint->records);
	kfree(checkpoint);
}
EXPORT_SYMBOL_GPL(apple_sftl_checkpoint_free);

int apple_sftl_checkpoint_build(const struct apple_sftl_snapshot *snapshot,
				const u32 *blocks, u32 nr_blocks,
				u32 max_records, struct apple_sftl_checkpoint **out)
{
	struct apple_sftl_checkpoint *checkpoint;
	u32 count, i, j, cursor = 0, run = 0, old_names;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = sftl_checkpoint_count(snapshot, max_records, &count);
	if (ret)
		return ret;
	if (!blocks || !nr_blocks || nr_blocks > 16)
		return -EINVAL;
	for (i = 0; i < nr_blocks; i++) {
		if (!blocks[i] || blocks[i] >= APPLE_SFTL_SUBBLOCKS)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (blocks[j] == blocks[i])
				return -EINVAL;
	}
	old_names = get_unaligned_le32(snapshot->opaque[0].data);
	checkpoint = kzalloc(sizeof(*checkpoint), GFP_KERNEL);
	if (!checkpoint)
		return -ENOMEM;
	checkpoint->records = kvzalloc(array_size(count, sizeof(*checkpoint->records)), GFP_KERNEL);
	if (!checkpoint->records) {
		kfree(checkpoint);
		return -ENOMEM;
	}
	checkpoint->count = count;
	checkpoint->first_sequence = snapshot->sequence + 1;
	checkpoint->sequence = snapshot->sequence + count;
	for (i = 0; i < snapshot->nr_opaque; i++) {
		struct apple_sftl_context_record *record = &checkpoint->records[i];

		*record = snapshot->opaque[i];
		apple_sftl_context_meta(record->meta, record->meta[1],
					checkpoint->first_sequence + i,
					get_unaligned_le16(record->meta + 8),
					get_unaligned_le16(record->meta + 10));
	}
	/* Only the decoded base list changes; its remaining native bytes survive. */
	memset(checkpoint->records[0].data + 4, 0, max(old_names, nr_blocks) * sizeof(u32));
	put_unaligned_le32(nr_blocks, checkpoint->records[0].data);
	for (i = 0; i < nr_blocks; i++)
		put_unaligned_le32(blocks[i], checkpoint->records[0].data + 4 + i * 4);
	for (i = snapshot->nr_opaque; i < count; i++) {
		struct apple_sftl_context_record *record = &checkpoint->records[i];

		memset(record->data, 0xff, sizeof(record->data));
		apple_sftl_context_meta(record->meta, 4,
					checkpoint->first_sequence + i, 0, 0);
		if (run == snapshot->nr_runs)
			continue;
		put_unaligned_le32(cursor, record->data);
		put_unaligned_le32(0xfffffff0, record->data + 4);
		for (j = 0; j < SFTL_PAIRS && run < snapshot->nr_runs; j++, run++) {
			const struct apple_sftl_extent *extent = &snapshot->runs[run];
			u32 address = extent->provider;

			if (address == APPLE_SFTL_NO_ADDRESS)
				address = SFTL_UNMAPPED;
			put_unaligned_le32(address, record->data + 8 + j * 8);
			put_unaligned_le32(extent->length, record->data + 12 + j * 8);
			cursor += extent->length;
		}
	}
	*out = checkpoint;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_checkpoint_build);

static bool sftl_overlap(u32 first, u32 length, u32 start, u32 end)
{
	return first < end && start < first + length;
}

static void sftl_gc_run(struct apple_sftl_snapshot *map, u32 logical,
			u32 length, u32 provider)
{
	struct apple_sftl_extent *run = &map->runs[map->nr_runs++];

	*run = (struct apple_sftl_extent) {
		.logical = logical, .length = length, .provider = provider,
	};
}

static bool sftl_gc_block_valid(const struct apple_sftl_gc_block *block)
{
	return block && block->block && block->block < APPLE_SFTL_SUBBLOCKS &&
		block->lanes && !block->reserved && block->last_sequence <= SFTL_SEQUENCE;
}

int apple_sftl_gc_plan(const struct apple_sftl_snapshot *snapshot,
		       const struct apple_sftl_gc_block *victim,
		       const struct apple_sftl_gc_block *destination,
			u32 max_records, size_t max_bytes, struct apple_sftl_gc **out)
{
	struct apple_sftl_gc *gc;
	u32 start, end, dest, capacity, copied = 0, i, count;
	size_t map_bytes, copy_bytes, opaque_bytes;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = sftl_checkpoint_count(snapshot, max_records, &count);
	if (ret)
		return ret;
	if (snapshot->sequence != snapshot->journal_sequence)
		return -EOPNOTSUPP;
	if (!sftl_gc_block_valid(victim) || !sftl_gc_block_valid(destination) ||
	    victim->blank || !victim->closed || !destination->blank ||
	    destination->last_sequence || victim->block == destination->block ||
	    victim->last_sequence > snapshot->sequence)
		return -EINVAL;
	start = victim->block * APPLE_SFTL_STRIDE;
	end = start + hweight16(victim->lanes) * 512;
	dest = destination->block * APPLE_SFTL_STRIDE;
	/* Eight native footer pages remain available when the destination closes. */
	capacity = hweight16(destination->lanes) * 512 - 32;
	for (i = 0; i < snapshot->nr_runs; i++) {
		const struct apple_sftl_extent *run = &snapshot->runs[i];

		if (run->provider != APPLE_SFTL_NO_ADDRESS &&
		    sftl_overlap(run->provider, run->length, dest, dest + APPLE_SFTL_STRIDE))
			return -EBADMSG;
	}
	map_bytes = array_size(snapshot->nr_runs * 3, sizeof(*gc->snapshot.runs));
	copy_bytes = array_size(snapshot->nr_runs, sizeof(*gc->copies));
	opaque_bytes = array_size(snapshot->nr_opaque, sizeof(*snapshot->opaque));
	if (map_bytes > max_bytes || copy_bytes > max_bytes - map_bytes ||
	    opaque_bytes > max_bytes - map_bytes - copy_bytes)
		return -ENOSPC;
	gc = kzalloc(sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;
	gc->snapshot.sequence = snapshot->sequence;
	gc->snapshot.checkpoint_sequence = snapshot->checkpoint_sequence;
	gc->snapshot.journal_sequence = snapshot->journal_sequence;
	gc->max_records = max_records;
	gc->victim = victim->block;
	gc->destination = destination->block;
	gc->retire_sequence = max(victim->last_sequence, snapshot->sequence);
	gc->copied_sequence = snapshot->sequence;
	gc->snapshot.runs = kvzalloc(map_bytes, GFP_KERNEL);
	gc->copies = kvzalloc(copy_bytes, GFP_KERNEL);
	gc->snapshot.opaque = kvzalloc(opaque_bytes, GFP_KERNEL);
	if (!gc->snapshot.runs || !gc->copies || !gc->snapshot.opaque) {
		ret = -ENOMEM;
		goto failed;
	}
	memcpy(gc->snapshot.opaque, snapshot->opaque,
	       snapshot->nr_opaque * sizeof(*snapshot->opaque));
	gc->snapshot.nr_opaque = snapshot->nr_opaque;
	for (i = 0; i < snapshot->nr_runs; i++) {
		const struct apple_sftl_extent *run = &snapshot->runs[i];
		u32 before, live, offset, address = run->provider;

		if (address != APPLE_SFTL_NO_ADDRESS &&
		    sftl_overlap(address, run->length, start, start + APPLE_SFTL_STRIDE) &&
		    min(address + run->length, start + APPLE_SFTL_STRIDE) > end - 32) {
			ret = -EBADMSG;
			goto failed;
		}
		if (address == APPLE_SFTL_NO_ADDRESS ||
		    !sftl_overlap(address, run->length, start, end)) {
			sftl_gc_run(&gc->snapshot, run->logical, run->length, address);
			continue;
		}
		before = start > address ? start - address : 0;
		live = min(address + run->length, end) - max(address, start);
		if (live > capacity - copied) {
			ret = -ENOSPC;
			goto failed;
		}
		if (before)
			sftl_gc_run(&gc->snapshot, run->logical, before, address);
		sftl_gc_run(&gc->snapshot, run->logical + before, live, dest + copied);
		gc->copies[gc->nr_copies++] = (struct apple_sftl_gc_copy) {
			.logical = run->logical + before, .length = live,
			.source = address + before, .destination = dest + copied,
		};
		copied += live;
		offset = before + live;
		if (offset < run->length)
			sftl_gc_run(&gc->snapshot, run->logical + offset,
				    run->length - offset, address + offset);
	}
	/* Copy records consume sequences before checkpoint serialization. */
	if (copied > SFTL_SEQUENCE - snapshot->sequence) {
		ret = -EOVERFLOW;
		goto failed;
	}
	gc->snapshot.sequence += copied;
	gc->snapshot.journal_sequence += copied;
	ret = sftl_checkpoint_count(&gc->snapshot, max_records, &count);
	if (ret)
		goto failed;
	gc->stage = gc->nr_copies ? APPLE_SFTL_GC_COPY : APPLE_SFTL_GC_CHECKPOINT;
	*out = gc;
	return 0;
failed:
	apple_sftl_gc_free(gc);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_plan);

void apple_sftl_gc_free(struct apple_sftl_gc *gc)
{
	if (!gc)
		return;
	apple_sftl_checkpoint_free(gc->checkpoint);
	kvfree(gc->snapshot.runs);
	kvfree(gc->snapshot.opaque);
	kvfree(gc->copies);
	kfree(gc);
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_free);

enum apple_sftl_gc_stage apple_sftl_gc_stage(const struct apple_sftl_gc *gc)
{
	return gc ? gc->stage : APPLE_SFTL_GC_FAILED;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_stage);

const struct apple_sftl_gc_copy *apple_sftl_gc_next_copy(const struct apple_sftl_gc *gc)
{
	if (!gc || gc->stage != APPLE_SFTL_GC_COPY)
		return NULL;
	return &gc->copies[gc->copied];
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_next_copy);

int apple_sftl_gc_copy_done(struct apple_sftl_gc *gc,
			    const struct apple_sftl_gc_copy *receipt, u64 sequence)
{
	const struct apple_sftl_gc_copy *expected = apple_sftl_gc_next_copy(gc);

	if (!expected || !receipt)
		return -EINVAL;
	if (memcmp(expected, receipt, sizeof(*receipt)) ||
	    sequence != gc->copied_sequence + expected->length) {
		gc->stage = APPLE_SFTL_GC_FAILED;
		return -EBADMSG;
	}
	gc->copied_sequence = sequence;
	if (++gc->copied == gc->nr_copies)
		gc->stage = APPLE_SFTL_GC_CHECKPOINT;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_copy_done);

int apple_sftl_gc_checkpoint(struct apple_sftl_gc *gc, const u32 *blocks,
			     u32 nr_blocks, struct apple_sftl_checkpoint **out)
{
	struct apple_sftl_checkpoint *image;
	u32 i;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!gc || gc->stage != APPLE_SFTL_GC_CHECKPOINT || gc->checkpoint)
		return -EINVAL;
	if (!blocks || !nr_blocks || nr_blocks > 16)
		return -EINVAL;
	for (i = 0; i < nr_blocks; i++)
		if (blocks[i] == gc->victim || blocks[i] == gc->destination)
			return -EINVAL;
	ret = apple_sftl_checkpoint_build(&gc->snapshot, blocks, nr_blocks,
					  gc->max_records, &gc->checkpoint);
	if (ret)
		return ret;
	ret = apple_sftl_checkpoint_build(&gc->snapshot, blocks, nr_blocks,
					  gc->max_records, &image);
	if (ret) {
		apple_sftl_checkpoint_free(gc->checkpoint);
		gc->checkpoint = NULL;
		return ret;
	}
	*out = image;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_checkpoint);

int apple_sftl_gc_checkpoint_done(struct apple_sftl_gc *gc,
				  const struct apple_sftl_checkpoint *readback)
{
	const struct apple_sftl_checkpoint *expected;

	if (!gc || gc->stage != APPLE_SFTL_GC_CHECKPOINT || !gc->checkpoint || !readback)
		return -EINVAL;
	expected = gc->checkpoint;
	if (!readback->records || readback->count != expected->count ||
	    readback->first_sequence != expected->first_sequence ||
	    readback->sequence != expected->sequence ||
	    memcmp(readback->records, expected->records,
		   expected->count * sizeof(*expected->records))) {
		gc->stage = APPLE_SFTL_GC_FAILED;
		return -EBADMSG;
	}
	gc->stage = APPLE_SFTL_GC_RECOVERY;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_checkpoint_done);

int apple_sftl_gc_recovered(struct apple_sftl_gc *gc,
			    const struct apple_sftl_snapshot *recovered)
{
	u32 wanted = 0, found = 0, offset = 0, length;
	int ret;

	if (!gc || gc->stage != APPLE_SFTL_GC_RECOVERY)
		return -EINVAL;
	ret = sftl_check_map(recovered);
	if (ret)
		goto failed;
	if (recovered->checkpoint_sequence != gc->checkpoint->sequence ||
	    recovered->sequence != gc->checkpoint->sequence ||
	    recovered->journal_sequence != recovered->sequence ||
	    recovered->checkpoint_sequence <= gc->retire_sequence) {
		ret = -EBADMSG;
		goto failed;
	}
	/* Compare logical mappings even when the cold reader coalesces ranges. */
	while (wanted < gc->snapshot.nr_runs && found < recovered->nr_runs) {
		const struct apple_sftl_extent *a = &gc->snapshot.runs[wanted];
		const struct apple_sftl_extent *b = &recovered->runs[found];
		u32 a_offset = offset - a->logical, b_offset = offset - b->logical;
		u32 a_address = a->provider, b_address = b->provider;

		if (a_address != APPLE_SFTL_NO_ADDRESS)
			a_address += a_offset;
		if (b_address != APPLE_SFTL_NO_ADDRESS)
			b_address += b_offset;
		if (a_address != b_address) {
			ret = -EBADMSG;
			goto failed;
		}
		length = min(a->length - a_offset, b->length - b_offset);
		offset += length;
		if (offset == a->logical + a->length)
			wanted++;
		if (offset == b->logical + b->length)
			found++;
	}
	if (offset != APPLE_SFTL_LOGICAL_PAGES) {
		ret = -EBADMSG;
		goto failed;
	}
	gc->stage = APPLE_SFTL_GC_VERIFIED;
	return 0;
failed:
	gc->stage = APPLE_SFTL_GC_FAILED;
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_recovered);

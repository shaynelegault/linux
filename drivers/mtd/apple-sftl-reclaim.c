// SPDX-License-Identifier: GPL-2.0-only
/*
 * SwissPPNFTL checkpoint serialization and verified reclamation transactions.
 * Native metadata: 13G36 IOFlashStorage-730 formatter at 0x81092408.
 * Range payload: cxtNextBatch at 0x8108f07c and the recovery decoder.
 *
 * Map-derived accounting is rebuilt for the measured native envelope.
 * The platform must account for the other native state before publication.
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
#include "apple-sftl-account.h"

#define SFTL_SEQUENCE GENMASK_ULL(47, 0)
#define SFTL_PROVIDER (APPLE_SFTL_SUBBLOCKS * APPLE_SFTL_STRIDE)
#define SFTL_PAIRS ((APPLE_SFTL_CHUNK - 8) / 8)
#define SFTL_MAX_RECORDS (16U * APPLE_SFTL_STRIDE)
#define SFTL_UNMAPPED 0x1ffffffU

struct apple_sftl_gc {
	struct apple_sftl_snapshot snapshot;
	struct apple_sftl_gc_copy *copies;
	struct apple_sftl_checkpoint *checkpoint;
	bool issued;
	u32 nr_copies, copied, max_records, victim, destination;
	u64 retire_sequence, copied_sequence, victim_sequence;
	u32 padding;
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
	/* A four-chunk native base cannot be downgraded to the compact form
	 * when its geometry or accounting prefix is damaged.
	 */
	if (snapshot->nr_opaque >= 4 && snapshot->opaque[3].meta[1] == 1 &&
	    (snapshot->nr_opaque != APPLE_SFTL_ACCOUNT_RECORDS ||
	     get_unaligned_le32(snapshot->opaque[3].data + APPLE_SFTL_CHUNK - 4) !=
		     APPLE_SFTL_SUBBLOCKS))
		return -EOPNOTSUPP;
	names = get_unaligned_le32(snapshot->opaque[0].data);
	if (!names || names > 16)
		return -EBADMSG;
	return 0;
}

static bool sftl_native_context(const struct apple_sftl_snapshot *snapshot)
{
	const struct apple_sftl_context_record *base = snapshot->opaque;

	return snapshot->nr_opaque >= 4 && base[0].meta[1] == 1 &&
		base[1].meta[1] == 1 && base[2].meta[1] == 1 && base[3].meta[1] == 1 &&
		get_unaligned_le32(base[3].data + APPLE_SFTL_CHUNK - 4) == APPLE_SFTL_SUBBLOCKS;
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
	records = (snapshot->nr_runs + SFTL_PAIRS - 1) / SFTL_PAIRS;
	/* cxtNextBatch pads the measured 64 GB range stream to whole batches. */
	if (sftl_native_context(snapshot))
		records = (records + APPLE_SFTL_NATIVE_MAP_BATCH - 1) &
			~(APPLE_SFTL_NATIVE_MAP_BATCH - 1);
	records += snapshot->nr_opaque;
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
	if (sftl_native_context(snapshot)) {
		ret = apple_sftl_account_map(checkpoint, snapshot);
		if (ret) {
			apple_sftl_checkpoint_free(checkpoint);
			return ret;
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
	u32 start, end, dest, capacity, copied = 0, i, count, footer;
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
	footer = victim->footer_chunks ? victim->footer_chunks : 32;
	if (footer < 4 || footer > 32 || footer % 4)
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
	gc->victim_sequence = victim->last_sequence;
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
		    min(address + run->length, start + APPLE_SFTL_STRIDE) > end - footer) {
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
	/* Program complete pages. Repeat the first key in the leading slots:
	 * its last copy then begins an uninterrupted run of live mappings.
	 */
	gc->padding = (4 - copied % 4) % 4;
	if (gc->padding) {
		for (i = 0; i < gc->nr_copies; i++)
			gc->copies[i].destination += gc->padding;
		for (i = 0; i < gc->snapshot.nr_runs; i++) {
			struct apple_sftl_extent *run = &gc->snapshot.runs[i];

			if (run->provider >= dest && run->provider < dest + capacity)
				run->provider += gc->padding;
		}
		copied += gc->padding;
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

u32 apple_sftl_gc_copy_chunks(const struct apple_sftl_gc *gc)
{
	if (!gc || gc->stage != APPLE_SFTL_GC_COPY || gc->copied)
		return 0;
	return gc->snapshot.sequence - gc->copied_sequence;
}

bool apple_sftl_gc_has_sequence_room(const struct apple_sftl_gc *gc, u32 pending)
{
	u32 count;

	if (!gc || sftl_checkpoint_count(&gc->snapshot, gc->max_records, &count))
		return false;
	return pending <= SFTL_SEQUENCE - gc->snapshot.sequence - count;
}

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
	    sequence != gc->copied_sequence + expected->length +
			(gc->copied ? 0 : gc->padding)) {
		gc->stage = APPLE_SFTL_GC_FAILED;
		return -EBADMSG;
	}
	gc->copied_sequence = sequence;
	if (++gc->copied == gc->nr_copies)
		gc->stage = APPLE_SFTL_GC_CHECKPOINT;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_copy_done);

static int sftl_gc_program(struct apple_sftl_gc_page *buffer,
			   const struct apple_sftl_gc_page *page,
			   const struct apple_sftl_gc_io *io, void *priv, u32 address)
{
	int ret;

	ret = io->read(priv, address, buffer);
	if (ret)
		return ret;
	if (memchr_inv(buffer, 0xff, sizeof(*buffer)))
		return -EBADMSG;
	ret = io->program(priv, address, page);
	if (ret)
		return ret;
	ret = io->read(priv, address, buffer);
	if (ret)
		return ret;
	return memcmp(page, buffer, sizeof(*page)) ? -EBADMSG : 0;
}

int apple_sftl_checkpoint_stage(const struct apple_sftl_checkpoint *image,
				u32 block, u16 lanes, const struct apple_sftl_gc_io *io,
				void *priv, struct apple_sftl_checkpoint **readback)
{
	struct apple_sftl_checkpoint *copy;
	struct apple_sftl_gc_page *pages;
	u32 capacity = hweight16(lanes) * 512, start = block * APPLE_SFTL_STRIDE;
	u32 i, j, order = 0;
	bool native;
	int ret;

	if (!readback)
		return -EINVAL;
	*readback = NULL;
	if (!image || !image->records || !image->count || image->count % 4 ||
	    !block || block >= APPLE_SFTL_SUBBLOCKS || image->count > capacity ||
	    !image->first_sequence || image->sequence > SFTL_SEQUENCE ||
	    image->first_sequence > image->sequence ||
	    image->sequence - image->first_sequence != image->count - 1 ||
	    !io || !io->read || !io->program)
		return -EINVAL;
	if (image->records[0].meta[1] != 1 ||
	    image->records[image->count - 1].meta[1] != 4 ||
	    !get_unaligned_le32(image->records[0].data) ||
	    get_unaligned_le32(image->records[0].data) > 16 ||
	    get_unaligned_le32(image->records[0].data + 4) != block)
		return -EBADMSG;
	native = image->records[3].meta[1] == 1;
	if (native && (image->count <= APPLE_SFTL_ACCOUNT_RECORDS ||
		       (image->count - APPLE_SFTL_ACCOUNT_RECORDS) % APPLE_SFTL_NATIVE_MAP_BATCH ||
		       get_unaligned_le32(image->records[3].data + APPLE_SFTL_CHUNK - 4) !=
			       APPLE_SFTL_SUBBLOCKS))
		return -EBADMSG;
	for (i = 0; i < image->count; i++) {
		const u8 *meta = image->records[i].meta;
		u32 group = meta[1] == 4 ? 7 : meta[1];

		if (group < order || (native && !apple_sftl_native_context_tag(i, meta)))
			return -EBADMSG;
		order = group;
		if (meta[0] != 0x1f || !meta[1] || meta[1] > 6 ||
		    get_unaligned_le64(meta) >> 16 != image->first_sequence + i ||
		    get_unaligned_le32(meta + 12) != U32_MAX)
			return -EBADMSG;
	}
	copy = kzalloc(sizeof(*copy), GFP_NOIO);
	if (!copy)
		return -ENOMEM;
	copy->records = kvzalloc(array_size(image->count, sizeof(*copy->records)), GFP_NOIO);
	pages = kvmalloc(2 * sizeof(*pages), GFP_NOIO);
	if (!copy->records || !pages) {
		ret = -ENOMEM;
		goto out;
	}
	/* Check the whole block before the first program, including the space
	 * past the new generation where a loader might find older records.
	 */
	for (i = 0; i < capacity; i += 4) {
		ret = io->read(priv, start + i, &pages[0]);
		if (ret)
			goto out;
		if (memchr_inv(&pages[0], 0xff, sizeof(*pages))) {
			ret = -EBADMSG;
			goto out;
		}
	}
	for (i = 0; i < image->count; i += 4) {
		for (j = 0; j < 4; j++) {
			memcpy(pages[1].data + j * APPLE_SFTL_CHUNK,
			       image->records[i + j].data, APPLE_SFTL_CHUNK);
			memcpy(pages[1].oob + j * APPLE_SFTL_META,
			       image->records[i + j].meta, APPLE_SFTL_META);
		}
		ret = sftl_gc_program(&pages[0], &pages[1], io, priv, start + i);
		if (ret)
			goto out;
		for (j = 0; j < 4; j++) {
			memcpy(copy->records[i + j].data,
			       pages[0].data + j * APPLE_SFTL_CHUNK, APPLE_SFTL_CHUNK);
			memcpy(copy->records[i + j].meta,
			       pages[0].oob + j * APPLE_SFTL_META, APPLE_SFTL_META);
		}
	}
	copy->count = image->count;
	copy->first_sequence = get_unaligned_le64(copy->records[0].meta) >> 16;
	copy->sequence = get_unaligned_le64(copy->records[copy->count - 1].meta) >> 16;
	*readback = copy;
	copy = NULL;
out:
	kvfree(pages);
	apple_sftl_checkpoint_free(copy);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_checkpoint_stage);

/* No erase or map publication occurs here. Even an indeterminate program
 * consumes the transaction: the caller must recover before writing again.
 */
int apple_sftl_gc_relocate(struct apple_sftl_gc *gc,
			   const struct apple_sftl_gc_io *io, void *priv)
{
	struct apple_sftl_gc_page *pages, *src, *dst;
	u32 i, offset, slot = 0, cached = U32_MAX, repeat, address;
	u64 sequence;
	int ret = 0;

	if (!gc || gc->stage != APPLE_SFTL_GC_COPY || gc->copied ||
	    !io || !io->read || !io->program)
		return -EINVAL;
	pages = kvmalloc(2 * sizeof(*pages), GFP_NOIO);
	if (!pages)
		return -ENOMEM;
	src = &pages[0];
	dst = &pages[1];
	address = gc->destination * APPLE_SFTL_STRIDE;
	sequence = gc->copied_sequence;
	repeat = 1 + gc->padding;
	for (i = 0; i < gc->nr_copies; i++) {
		const struct apple_sftl_gc_copy *copy = &gc->copies[i];

		for (offset = 0; offset < copy->length; offset++) {
			u32 from = copy->source + offset, key = copy->logical + offset;
			const u8 *meta;
			u64 stamp;
			u32 user_epoch;

			if (cached != (from & ~3U)) {
				cached = from & ~3U;
				ret = io->read(priv, cached, src);
				if (ret)
					goto failed;
			}
			meta = src->oob + (from & 3) * APPLE_SFTL_META;
			stamp = get_unaligned_le64(meta) >> 16;
			user_epoch = get_unaligned_le32(meta + 12);
			/* Only rewrite ordinary unflagged data in its native class. */
			if (meta[0] != 1 || meta[1] || !stamp ||
			    get_unaligned_le32(meta + 8) != key ||
			    user_epoch > stamp >> 16 ||
			    stamp > gc->victim_sequence) {
				ret = -EBADMSG;
				goto failed;
			}
			do {
				u8 *out = dst->oob + slot * APPLE_SFTL_META;

				memcpy(dst->data + slot * APPLE_SFTL_CHUNK,
				       src->data + (from & 3) * APPLE_SFTL_CHUNK,
				       APPLE_SFTL_CHUNK);
				sequence++;
				put_unaligned_le64(sequence << 16 | 1, out);
				put_unaligned_le32(key, out + 8);
				put_unaligned_le32(user_epoch, out + 12);
				if (++slot == 4) {
					ret = sftl_gc_program(src, dst, io, priv, address);
					if (ret)
						goto failed;
					cached = U32_MAX;
					slot = 0;
					address += 4;
				}
			} while (--repeat);
			repeat = 1;
		}
	}
	gc->copied = gc->nr_copies;
	gc->copied_sequence = sequence;
	gc->stage = APPLE_SFTL_GC_CHECKPOINT;
	goto out;
failed:
	gc->stage = APPLE_SFTL_GC_FAILED;
out:
	kvfree(pages);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_relocate);

int apple_sftl_gc_prepare_native(struct apple_sftl_gc *gc,
		apple_sftl_prepare_accounting prepare, void *priv,
		const u32 blocks[2], u32 capacity)
{
	int ret;

	if (!gc || !prepare || !blocks || !capacity || gc->stage != APPLE_SFTL_GC_COPY ||
	    gc->copied || gc->checkpoint)
		return -EINVAL;
	ret = prepare(priv, gc->snapshot.opaque, gc->snapshot.nr_opaque, &gc->snapshot,
		      gc->victim, gc->destination, blocks);
	if (ret)
		return ret;
	return apple_sftl_checkpoint_build(&gc->snapshot, blocks, 2,
					   min(gc->max_records, capacity), &gc->checkpoint);
}

int apple_sftl_gc_checkpoint(struct apple_sftl_gc *gc, const u32 *blocks,
			     u32 nr_blocks, struct apple_sftl_checkpoint **out)
{
	struct apple_sftl_checkpoint *image;
	u32 i;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!gc || gc->stage != APPLE_SFTL_GC_CHECKPOINT || gc->issued)
		return -EINVAL;
	if (!blocks || !nr_blocks || nr_blocks > 16)
		return -EINVAL;
	for (i = 0; i < nr_blocks; i++)
		if (blocks[i] == gc->victim || blocks[i] == gc->destination)
			return -EINVAL;
	if (!gc->checkpoint) {
		ret = apple_sftl_checkpoint_build(&gc->snapshot, blocks, nr_blocks,
						  gc->max_records, &gc->checkpoint);
		if (ret)
			return ret;
	} else {
		const u8 *base = gc->checkpoint->records[0].data;

		if (get_unaligned_le32(base) != nr_blocks)
			return -EINVAL;
		for (i = 0; i < nr_blocks; i++)
			if (get_unaligned_le32(base + 4 + 4 * i) != blocks[i])
				return -EINVAL;
	}
	image = kmalloc(sizeof(*image), GFP_NOIO);
	if (!image)
		return -ENOMEM;
	*image = *gc->checkpoint;
	image->records = kvmalloc(array_size(image->count, sizeof(*image->records)), GFP_NOIO);
	if (!image->records) {
		kfree(image);
		return -ENOMEM;
	}
	memcpy(image->records, gc->checkpoint->records,
	       array_size(image->count, sizeof(*image->records)));
	gc->issued = true;
	*out = image;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_sftl_gc_checkpoint);

int apple_sftl_gc_checkpoint_done(struct apple_sftl_gc *gc,
				  const struct apple_sftl_checkpoint *readback)
{
	const struct apple_sftl_checkpoint *expected;

	if (!gc || gc->stage != APPLE_SFTL_GC_CHECKPOINT || !gc->issued || !readback)
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

static bool sftl_maps_equal(const struct apple_sftl_snapshot *expected,
			    const struct apple_sftl_snapshot *recovered)
{
	u32 wanted = 0, found = 0, offset = 0, length;

	/* Compare logical mappings even when the cold reader coalesces ranges. */
	while (wanted < expected->nr_runs && found < recovered->nr_runs) {
		const struct apple_sftl_extent *a = &expected->runs[wanted];
		const struct apple_sftl_extent *b = &recovered->runs[found];
		u32 a_offset = offset - a->logical, b_offset = offset - b->logical;
		u32 a_address = a->provider, b_address = b->provider;

		if (a_address != APPLE_SFTL_NO_ADDRESS)
			a_address += a_offset;
		if (b_address != APPLE_SFTL_NO_ADDRESS)
			b_address += b_offset;
		if (a_address != b_address)
			return false;
		length = min(a->length - a_offset, b->length - b_offset);
		offset += length;
		if (offset == a->logical + a->length)
			wanted++;
		if (offset == b->logical + b->length)
			found++;
	}
	return offset == APPLE_SFTL_LOGICAL_PAGES;
}

/*
 * The durable checkpoint already supersedes every record in this closed
 * block. Its own map must exclude the block (checked by the caller before
 * replay), as must the full map independently recovered after replay.
 */
int apple_sftl_gc_plan_retired(const struct apple_sftl_snapshot *active,
			       const struct apple_sftl_snapshot *cold,
			       const struct apple_sftl_gc_block *victim,
			       u64 checkpoint_start)
{
	u32 i, first;
	int ret;

	ret = sftl_check_map(active);
	if (ret)
		return ret;
	ret = sftl_check_map(cold);
	if (ret)
		return ret;
	if (!sftl_gc_block_valid(victim) || !victim->closed || victim->blank ||
	    !checkpoint_start || checkpoint_start > cold->checkpoint_sequence ||
	    victim->last_sequence >= checkpoint_start)
		return -EOPNOTSUPP;
	if (active->sequence != cold->sequence ||
	    active->checkpoint_sequence != cold->checkpoint_sequence ||
	    active->journal_sequence != active->sequence ||
	    cold->journal_sequence != cold->sequence ||
	    !sftl_maps_equal(active, cold))
		return -EBADMSG;
	first = victim->block * APPLE_SFTL_STRIDE;
	for (i = 0; i < cold->nr_runs; i++) {
		const struct apple_sftl_extent *run = &cold->runs[i];

		if (run->provider != APPLE_SFTL_NO_ADDRESS &&
		    sftl_overlap(run->provider, run->length, first, first + APPLE_SFTL_STRIDE))
			return -EOPNOTSUPP;
	}
	return 0;
}

int apple_sftl_gc_recovered(struct apple_sftl_gc *gc,
			    const struct apple_sftl_snapshot *recovered)
{
	int ret;

	if (!gc || (gc->stage != APPLE_SFTL_GC_RECOVERY && gc->stage != APPLE_SFTL_GC_VERIFIED))
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
	if (!sftl_maps_equal(&gc->snapshot, recovered)) {
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

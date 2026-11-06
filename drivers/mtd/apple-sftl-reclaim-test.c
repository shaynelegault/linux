// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic checkpoints and supplied IO receipts; no media mutations. */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "apple-sftl-reclaim.h"

static void reclaim_source(struct apple_sftl_snapshot *snapshot,
			   struct apple_sftl_context_record *opaque,
			   struct apple_sftl_extent *runs, u32 count)
{
	unsigned int i;

	memset(opaque, 0x6b, sizeof(*opaque));
	apple_sftl_context_meta(opaque->meta, 1, 100, 17, 23);
	put_unaligned_le32(1, opaque->data);
	put_unaligned_le32(1, opaque->data + 4);
	for (i = 0; i < count; i++)
		runs[i] = (struct apple_sftl_extent) {
			.logical = i, .length = 1,
			.provider = i & 1 ? APPLE_SFTL_NO_ADDRESS : 4 * APPLE_SFTL_STRIDE + i,
		};
	runs[count - 1].length = APPLE_SFTL_LOGICAL_PAGES - count + 1;
	runs[count - 1].provider = APPLE_SFTL_NO_ADDRESS;
	*snapshot = (struct apple_sftl_snapshot) {
		.runs = runs, .nr_runs = count, .opaque = opaque, .nr_opaque = 1,
		.sequence = 200, .checkpoint_sequence = 101, .journal_sequence = 200,
	};
}

static void reclaim_metadata_vector(struct kunit *test)
{
	/* Native 0x81092408 output, with low-word sequence carry. */
	static const u8 expected[16] = {
		0x1f, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x00, 0x02, 0x09, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	u8 meta[16];

	KUNIT_ASSERT_EQ(test, apple_sftl_context_meta(meta, 6, 0x100000000ULL, 512, 9), 0);
	KUNIT_EXPECT_MEMEQ(test, meta, expected, sizeof(meta));
	memset(meta, 0x5a, sizeof(meta));
	KUNIT_EXPECT_EQ(test, apple_sftl_context_meta(meta, 0, 1, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, meta[0], 0x5a);
	KUNIT_EXPECT_EQ(test, apple_sftl_context_meta(meta, 4, 1ULL << 48, 0, 0), -EINVAL);
}

static void reclaim_checkpoint_fragments(struct kunit *test)
{
	struct apple_sftl_context_record *opaque = kunit_kmalloc(test, sizeof(*opaque), GFP_KERNEL);
	struct apple_sftl_extent *runs = kunit_kmalloc(test, 514 * sizeof(*runs), GFP_KERNEL);
	struct apple_sftl_checkpoint *image = NULL;
	struct apple_sftl_snapshot snapshot;
	u32 blocks[] = { 5, 6 };
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, opaque);
	KUNIT_ASSERT_NOT_NULL(test, runs);
	reclaim_source(&snapshot, opaque, runs, 514);
	KUNIT_ASSERT_EQ(test, apple_sftl_checkpoint_build(&snapshot, blocks, 2, 4, &image), 0);
	KUNIT_ASSERT_NOT_NULL(test, image);
	KUNIT_EXPECT_EQ(test, image->count, 4U);
	KUNIT_EXPECT_EQ(test, image->first_sequence, 201ULL);
	KUNIT_EXPECT_EQ(test, image->sequence, 204ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[0].data), 2U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[0].data + 4), 5U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[0].data + 8), 6U);
	KUNIT_EXPECT_MEMEQ(test, image->records[0].data + 12, opaque->data + 12, 4084);
	KUNIT_EXPECT_MEMEQ(test, image->records[0].meta + 8, opaque->meta + 8, 8);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[1].data), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[2].data), 511U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[3].data + 4), (u32)~0U);
	for (i = 1; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, image->records[i].meta[0], 0x1f);
		KUNIT_EXPECT_EQ(test, image->records[i].meta[1], 4);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(image->records[i].meta + 8), 0U);
	}
	apple_sftl_checkpoint_free(image);
	image = NULL;
	KUNIT_EXPECT_EQ(test,
			apple_sftl_checkpoint_build(&snapshot, blocks, 2, 3, &image), -ENOSPC);
	KUNIT_EXPECT_PTR_EQ(test, image, NULL);
	snapshot.sequence = (1ULL << 48) - 3;
	KUNIT_EXPECT_EQ(test,
			apple_sftl_checkpoint_build(&snapshot, blocks, 2, 4, &image), -EOVERFLOW);
}

static void reclaim_gc_transaction(struct kunit *test)
{
	struct apple_sftl_context_record *opaque = kunit_kmalloc(test, sizeof(*opaque), GFP_KERNEL);
	struct apple_sftl_extent runs[3];
	struct apple_sftl_extent recovered_runs[3];
	struct apple_sftl_snapshot snapshot, recovered;
	struct apple_sftl_gc_block victim = {
		.block = 4, .lanes = 3, .closed = true, .last_sequence = 199,
	};
	struct apple_sftl_gc_block destination = { .block = 7, .lanes = 3, .blank = true };
	const struct apple_sftl_gc_copy *copy;
	struct apple_sftl_checkpoint *image = NULL;
	struct apple_sftl_gc *gc = NULL;
	u32 blocks[] = { 5 };

	KUNIT_ASSERT_NOT_NULL(test, opaque);
	reclaim_source(&snapshot, opaque, runs, 3);
	runs[0].length = 2;
	runs[1] = (struct apple_sftl_extent) {
		.logical = 2, .length = 1, .provider = 6 * APPLE_SFTL_STRIDE,
	};
	runs[2] = (struct apple_sftl_extent) {
		.logical = 3, .length = APPLE_SFTL_LOGICAL_PAGES - 3,
		.provider = APPLE_SFTL_NO_ADDRESS,
	};
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_plan(&snapshot, &victim, &destination, 4,
						 65536, &gc), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_COPY);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_checkpoint(gc, blocks, 1, &image), -EINVAL);
	copy = apple_sftl_gc_next_copy(gc);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	KUNIT_EXPECT_EQ(test, copy->logical, 0U);
	KUNIT_EXPECT_EQ(test, copy->length, 2U);
	KUNIT_EXPECT_EQ(test, copy->destination, 7 * APPLE_SFTL_STRIDE);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_copy_done(gc, copy, 202), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_CHECKPOINT);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_checkpoint(gc, blocks, 1, &image), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_checkpoint_done(gc, image), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_RECOVERY);
	memcpy(recovered_runs, runs, sizeof(runs));
	recovered_runs[0].provider = 7 * APPLE_SFTL_STRIDE;
	recovered = snapshot;
	recovered.runs = recovered_runs;
	recovered.sequence = image->sequence;
	recovered.journal_sequence = image->sequence;
	recovered.checkpoint_sequence = image->sequence;
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_recovered(gc, &recovered), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_VERIFIED);
	/* The caller's original map and opaque accounting bytes remain intact. */
	KUNIT_EXPECT_EQ(test, runs[0].provider, 4 * APPLE_SFTL_STRIDE);
	KUNIT_EXPECT_EQ(test, opaque->data[200], 0x6b);
	apple_sftl_checkpoint_free(image);
	apple_sftl_gc_free(gc);
}

static void reclaim_gc_fail_stop(struct kunit *test)
{
	struct apple_sftl_context_record *opaque = kunit_kmalloc(test, sizeof(*opaque), GFP_KERNEL);
	struct apple_sftl_extent runs[2];
	struct apple_sftl_snapshot snapshot;
	struct apple_sftl_gc_block victim = {
		.block = 4, .lanes = 3, .closed = true, .last_sequence = 199,
	};
	struct apple_sftl_gc_block destination = { .block = 7, .lanes = 3, .blank = true };
	struct apple_sftl_gc *gc = NULL;
	struct apple_sftl_gc_copy receipt;
	struct apple_sftl_checkpoint *image = NULL;
	u32 blocks[] = { 5 };

	KUNIT_ASSERT_NOT_NULL(test, opaque);
	reclaim_source(&snapshot, opaque, runs, 2);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_plan(&snapshot, &victim, &destination, 4,
						 65536, &gc), 0);
	receipt = *apple_sftl_gc_next_copy(gc);
	receipt.destination++;
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_copy_done(gc, &receipt, 201), -EBADMSG);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_FAILED);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_checkpoint(gc, blocks, 1, &image), -EINVAL);
	apple_sftl_gc_free(gc);
	gc = NULL;
	victim.reserved = true;
	KUNIT_EXPECT_EQ(test,
			apple_sftl_gc_plan(&snapshot, &victim, &destination, 4,
					   65536, &gc), -EINVAL);
	victim.reserved = false;
	destination.block = 4;
	KUNIT_EXPECT_EQ(test,
			apple_sftl_gc_plan(&snapshot, &victim, &destination, 4,
					   65536, &gc), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, gc, NULL);
}

static void reclaim_obsolete_history_needs_checkpoint(struct kunit *test)
{
	struct apple_sftl_context_record *opaque = kunit_kmalloc(test, sizeof(*opaque), GFP_KERNEL);
	struct apple_sftl_extent runs[1];
	struct apple_sftl_snapshot snapshot;
	struct apple_sftl_gc_block victim = {
		.block = 4, .lanes = 3, .closed = true, .last_sequence = 199,
	};
	struct apple_sftl_gc_block destination = { .block = 7, .lanes = 3, .blank = true };
	struct apple_sftl_gc *gc = NULL;

	KUNIT_ASSERT_NOT_NULL(test, opaque);
	reclaim_source(&snapshot, opaque, runs, 1);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_plan(&snapshot, &victim, &destination, 4,
						 65536, &gc), 0);
	KUNIT_EXPECT_PTR_EQ(test, apple_sftl_gc_next_copy(gc), NULL);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_CHECKPOINT);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_recovered(gc, &snapshot), -EINVAL);
	apple_sftl_gc_free(gc);
}

static void reclaim_checkpoint_opaque_and_invalid(struct kunit *test)
{
	static const u8 subtypes[] = { 1, 2, 3, 5, 6 };
	struct apple_sftl_context_record *opaque;
	struct apple_sftl_extent runs[1];
	struct apple_sftl_snapshot snapshot;
	struct apple_sftl_checkpoint *image = NULL;
	u32 blocks[] = { 5, 5 };
	u32 i;

	opaque = kunit_kmalloc(test, 5 * sizeof(*opaque), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, opaque);
	reclaim_source(&snapshot, opaque, runs, 1);
	for (i = 1; i < 5; i++) {
		memset(&opaque[i], 0x50 + i, sizeof(opaque[i]));
		apple_sftl_context_meta(opaque[i].meta, subtypes[i], 100 + i, i * 16, 16);
	}
	snapshot.nr_opaque = 5;
	KUNIT_EXPECT_EQ(test,
			apple_sftl_checkpoint_build(&snapshot, blocks, 2, 8, &image), -EINVAL);
	KUNIT_ASSERT_EQ(test, apple_sftl_checkpoint_build(&snapshot, blocks, 1, 8, &image), 0);
	for (i = 1; i < 5; i++) {
		KUNIT_EXPECT_MEMEQ(test, image->records[i].data, opaque[i].data, 4096);
		KUNIT_EXPECT_MEMEQ(test, image->records[i].meta + 8, opaque[i].meta + 8, 8);
	}
	apple_sftl_checkpoint_free(image);
	image = NULL;
	opaque[2].meta[1] = 1;
	KUNIT_EXPECT_EQ(test,
			apple_sftl_checkpoint_build(&snapshot, blocks, 1, 8, &image), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, image, NULL);
}

static void reclaim_checkpoint_torn_receipts(struct kunit *test)
{
	struct apple_sftl_context_record *opaque = kunit_kmalloc(test, sizeof(*opaque), GFP_KERNEL);
	struct apple_sftl_extent runs[1];
	struct apple_sftl_snapshot snapshot, recovered;
	struct apple_sftl_gc_block victim = {
		.block = 4, .lanes = 3, .closed = true, .last_sequence = 199,
	};
	struct apple_sftl_gc_block destination = { .block = 7, .lanes = 3, .blank = true };
	struct apple_sftl_checkpoint *image;
	struct apple_sftl_gc *gc;
	u32 blocks[] = { 5 }, i;

	KUNIT_ASSERT_NOT_NULL(test, opaque);
	reclaim_source(&snapshot, opaque, runs, 1);
	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_EQ(test,
				apple_sftl_gc_plan(&snapshot, &victim, &destination, 4,
						   65536, &gc), 0);
		KUNIT_ASSERT_EQ(test, apple_sftl_gc_checkpoint(gc, blocks, 1, &image), 0);
		image->records[i].meta[2] ^= 1;
		KUNIT_EXPECT_EQ(test, apple_sftl_gc_checkpoint_done(gc, image), -EBADMSG);
		KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_FAILED);
		apple_sftl_checkpoint_free(image);
		apple_sftl_gc_free(gc);
	}
	KUNIT_ASSERT_EQ(test,
			apple_sftl_gc_plan(&snapshot, &victim, &destination, 4, 65536, &gc), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_checkpoint(gc, blocks, 1, &image), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_gc_checkpoint_done(gc, image), 0);
	recovered = snapshot;
	recovered.sequence = image->sequence;
	recovered.journal_sequence = image->sequence;
	recovered.checkpoint_sequence = image->sequence - 1;
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_recovered(gc, &recovered), -EBADMSG);
	KUNIT_EXPECT_EQ(test, apple_sftl_gc_stage(gc), APPLE_SFTL_GC_FAILED);
	apple_sftl_checkpoint_free(image);
	apple_sftl_gc_free(gc);
}

static void reclaim_gc_boundaries(struct kunit *test)
{
	struct apple_sftl_context_record *opaque = kunit_kmalloc(test, sizeof(*opaque), GFP_KERNEL);
	struct apple_sftl_extent runs[2];
	struct apple_sftl_snapshot snapshot;
	struct apple_sftl_gc_block victim = {
		.block = 4, .lanes = 3, .closed = true, .last_sequence = 199,
	};
	struct apple_sftl_gc_block destination = { .block = 7, .lanes = 1, .blank = true };
	struct apple_sftl_gc *gc = NULL;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, opaque);
	reclaim_source(&snapshot, opaque, runs, 2);
	snapshot.journal_sequence--;
	ret = apple_sftl_gc_plan(&snapshot, &victim, &destination, 4, 65536, &gc);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	snapshot.journal_sequence++;
	ret = apple_sftl_gc_plan(&snapshot, &victim, &destination, 4, 1, &gc);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	runs[0].length = 481;
	runs[1].logical = 481;
	runs[1].length = APPLE_SFTL_LOGICAL_PAGES - 481;
	ret = apple_sftl_gc_plan(&snapshot, &victim, &destination, 4, 65536, &gc);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	runs[0].length = 1;
	runs[0].provider += 1000;
	runs[1].logical = 1;
	runs[1].length = APPLE_SFTL_LOGICAL_PAGES - 1;
	ret = apple_sftl_gc_plan(&snapshot, &victim, &destination, 4, 65536, &gc);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, gc, NULL);
}

static struct kunit_case reclaim_cases[] = {
	KUNIT_CASE(reclaim_metadata_vector),
	KUNIT_CASE(reclaim_checkpoint_opaque_and_invalid),
	KUNIT_CASE(reclaim_checkpoint_torn_receipts),
	KUNIT_CASE(reclaim_gc_boundaries),
	KUNIT_CASE(reclaim_checkpoint_fragments),
	KUNIT_CASE(reclaim_gc_transaction),
	KUNIT_CASE(reclaim_gc_fail_stop),
	KUNIT_CASE(reclaim_obsolete_history_needs_checkpoint),
	{}
};

static struct kunit_suite reclaim_suite = {
	.name = "apple-sftl-reclaim",
	.test_cases = reclaim_cases,
};

kunit_test_suite(reclaim_suite);
MODULE_LICENSE("GPL");

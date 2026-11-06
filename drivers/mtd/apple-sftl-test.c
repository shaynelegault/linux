// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic VFL/SFTL medium. No firmware or captured device bytes. */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mtd/apple-ppn.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "apple-sftl.h"

#define TEST_LOGICAL_PAGES 15625016U
#define TEST_STRIDE 8192U
#define TEST_CHECKPOINT 101ULL
#define TEST_CLOSED_FIRST 102ULL
#define TEST_CLOSED_CHUNKS 1020U
#define TEST_OPEN_FIRST (TEST_CLOSED_FIRST + TEST_CLOSED_CHUNKS)

enum sftl_fault {
	SFTL_NO_FAULT,
	SFTL_CHECKPOINT_COPY,
	SFTL_LANE_COPY,
	SFTL_BOOTSTRAP_DUPLICATE,
	SFTL_INCOMPLETE_CONTEXT,
	SFTL_JOURNAL_GAP,
	SFTL_JOURNAL_DUPLICATE,
	SFTL_BAD_FOOTER,
	SFTL_READ_FAILURE,
	SFTL_REFRESH,
	SFTL_TRIM,
	SFTL_ALLOCATION_FLOOR,
	SFTL_INACTIVE_ADDRESS,
	SFTL_OPEN_HOLE,
	SFTL_UNKNOWN_STATUS,
	SFTL_TORN_FOOTER,
	SFTL_RUNTIME_FAILURE,
};

struct sftl_fixture {
	struct apple_ppn provider;
	enum sftl_fault fault;
	unsigned int reads;
};

static void sftl_test_meta(u8 *meta, u8 kind, u8 flags, u64 seq, u32 key)
{
	unsigned int i;

	meta[0] = kind;
	meta[1] = flags;
	for (i = 0; i < 6; i++)
		meta[2 + i] = seq >> (8 * i);
	put_unaligned_le32(key, meta + 8);
	put_unaligned_le32(seq >> 16, meta + 12);
}

static void sftl_test_members(u8 *data, enum sftl_fault fault)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		put_unaligned_le16(2087, data + 0x100 + i * 4);
		data[0x102 + i * 4] = i;
		data[0x103 + i * 4] = 0;
	}
	if (fault == SFTL_BOOTSTRAP_DUPLICATE)
		data[0x106] = 0;
}

static int sftl_test_context(struct sftl_fixture *f, unsigned int lane,
			     unsigned int page, u8 *data, u8 *oob, u8 *status)
{
	unsigned int i;

	if (lane >= 8)
		return 0;
	if (!page) {
		memset(data, 0, 16384);
		put_unaligned_le16(16, data + 10);
		put_unaligned_le16(2086, data + 12);
		put_unaligned_le16(2086, data + 14);
		sftl_test_members(data, f->fault);
		for (i = 0; i < 16; i++)
			put_unaligned_le16(i + 1, data + 0x200 + i * 32);
		oob[0] = 0x20;
		oob[1] = 0xff;
		oob[2] = 0xff;
		oob[3] = 0;
		put_unaligned_le32(lane < 4 ? 0xfffffff0 : 0xfffffff1, oob + 4);
		if (f->fault == SFTL_CHECKPOINT_COPY && lane == 1)
			data[0] ^= 1;
		*status = 0x40;
	} else if (page <= 16) {
		/* Lane tables occupy pages 1..16 in each of four replicas. */
		if (page <= 2)
			for (i = 1; i <= 4; i++)
				put_unaligned_le16(0, data + i * 2);
		oob[0] = 0x20;
		oob[1] = page - 1;
		oob[2] = 0;
		oob[3] = 0;
		if (f->fault == SFTL_LANE_COPY && lane == 1 && page == 1)
			put_unaligned_le16(0x8000, data + 2);
		*status = 0x40;
	}
	return 0;
}

static void sftl_test_checkpoint(struct sftl_fixture *f, u8 *data, u8 *oob)
{
	u8 *ranges = data + 4096;
	u32 address = 4 * TEST_STRIDE;

	memset(data, 0xff, 16384);
	memset(data, 0, 4096);
	put_unaligned_le32(1, data);
	put_unaligned_le32(1, data + 4);
	sftl_test_meta(oob, 0x1f, 1, 100, 0);
	if (f->fault == SFTL_INCOMPLETE_CONTEXT)
		return;
	if (f->fault == SFTL_INACTIVE_ADDRESS)
		address += 1024;
	put_unaligned_le32(0, ranges);
	put_unaligned_le32(0xfffffff0, ranges + 4);
	put_unaligned_le32(address, ranges + 8);
	put_unaligned_le32(1, ranges + 12);
	put_unaligned_le32(0x1ffffff, ranges + 16);
	put_unaligned_le32(TEST_LOGICAL_PAGES - 1, ranges + 20);
	sftl_test_meta(oob + 16, 0x1f, 4, TEST_CHECKPOINT, 0);
}

static void sftl_test_open(struct sftl_fixture *f, u8 *data, u8 *oob)
{
	u64 first = TEST_OPEN_FIRST;
	unsigned int i;

	if (f->fault == SFTL_JOURNAL_GAP)
		first++;
	if (f->fault == SFTL_JOURNAL_DUPLICATE)
		first--;
	for (i = 0; i < 4; i++) {
		memset(data + i * 4096, 'D' + i, 4096);
		sftl_test_meta(oob + i * 16, 1, 0, first + i, 2);
	}
	if (f->fault == SFTL_TRIM) {
		memset(data, 0, 4096);
		put_unaligned_le32(1, data);
		put_unaligned_le32(0, data + 4);
		put_unaligned_le32(1, data + 8);
		sftl_test_meta(oob, 2, 0, first, 0xffff0001);
	}
}

static void sftl_test_closed(struct sftl_fixture *f, unsigned int position,
			     u8 *data, u8 *oob)
{
	unsigned int i;

	if (position < 255) {
		for (i = 0; i < 4; i++) {
			u32 slot = position * 4 + i;

			memset(data + i * 4096, (slot + 1) & 0xff, 4096);
			sftl_test_meta(oob + i * 16, 1, 0,
				       TEST_CLOSED_FIRST + slot, slot + 1);
		}
	} else {
		u32 span = TEST_CLOSED_CHUNKS;

		memset(data, 0, 16384);
		put_unaligned_le32(0, data);
		put_unaligned_le32(0, data + 4);
		put_unaligned_le32(1, data + 8);
		put_unaligned_le32(TEST_CLOSED_CHUNKS, data + 12);
		if (f->fault == SFTL_ALLOCATION_FLOOR)
			span += 8;
		sftl_test_meta(oob, 0x1c, 8, TEST_CLOSED_FIRST, span);
		put_unaligned_le16(f->fault == SFTL_BAD_FOOTER ? 2 : 4, oob + 12);
		put_unaligned_le16(0xffff, oob + 14);
		for (i = 1; i < 4; i++)
			memcpy(oob + i * 16, oob, 16);
		if (f->fault == SFTL_TORN_FOOTER)
			oob[63] ^= 1;
	}
}

static int sftl_test_read_page(void *priv, unsigned int bus, unsigned int cau,
			       unsigned int block, unsigned int page,
			       bool context, u8 *data, u8 *oob, u8 *status)
{
	struct sftl_fixture *f = priv;
	unsigned int lane = cau * 2 + bus;
	unsigned int position = page * 2 + lane;

	f->reads++;
	if (bus >= 2 || cau >= 8 || block >= 2088 || page >= (context ? 64 : 128))
		return -EINVAL;
	memset(data, 0xff, 16384);
	memset(oob, 0xff, 64);
	*status = 0x49;
	if (context) {
		if (block == 2087)
			return sftl_test_context(f, lane, page, data, oob, status);
		return 0;
	}
	if (block == 3 && (f->fault == SFTL_RUNTIME_FAILURE ||
			   (!position && f->fault == SFTL_READ_FAILURE)))
		return -EIO;
	if (lane > 1)
		return 0;
	if (block == 1 && !position) {
		sftl_test_checkpoint(f, data, oob);
	} else if (block == 2 && !position) {
		sftl_test_open(f, data, oob);
	} else if (block == 2 && position == 2 && f->fault == SFTL_OPEN_HOLE) {
		unsigned int i;

		for (i = 0; i < 4; i++) {
			memset(data + i * 4096, 'H' + i, 4096);
			sftl_test_meta(oob + i * 16, 1, 0, TEST_OPEN_FIRST + 4 + i, 3);
		}
	} else if (block == 3) {
		sftl_test_closed(f, position, data, oob);
	} else if (block == 4 && !position) {
		memset(data, 'C', 4096);
		sftl_test_meta(oob, 1, 0, 10, 0);
	} else {
		return 0;
	}
	*status = block == 2 && f->fault == SFTL_REFRESH ? 0x42 : 0x40;
	if (block == 2 && f->fault == SFTL_UNKNOWN_STATUS)
		*status = 0x61;
	return 0;
}

static const struct apple_ppn_ops sftl_test_ops = {
	.read_page = sftl_test_read_page,
};

static struct sftl_fixture *sftl_test_fixture(struct kunit *test,
					      enum sftl_fault fault)
{
	struct sftl_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->fault = fault;
	f->provider.ops = &sftl_test_ops;
	f->provider.priv = f;
	f->provider.buses = 2;
	f->provider.caus = 8;
	f->provider.blocks = 2088;
	f->provider.pages = 128;
	return f;
}

static void sftl_test_expect_fill(struct kunit *test, const u8 *data, u8 fill)
{
	unsigned int i;

	for (i = 0; i < 4096; i++)
		if (data[i] != fill) {
			KUNIT_FAIL(test, "logical byte %u: expected %02x, got %02x",
				   i, fill, data[i]);
			return;
		}
}

static void sftl_test_complete_medium(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl_info info;
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_NOT_NULL(test, sftl);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.logical_pages, TEST_LOGICAL_PAGES);
	KUNIT_EXPECT_EQ(test, info.checkpoint_sequence, TEST_CHECKPOINT);
	KUNIT_EXPECT_EQ(test, info.sequence, TEST_OPEN_FIRST + 3);
	KUNIT_EXPECT_EQ(test, info.nr_runs, 5U);
	KUNIT_EXPECT_EQ(test, info.disk_pages, 15625000U);
	KUNIT_EXPECT_LT(test, f->reads, 12000U);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
	sftl_test_expect_fill(test, data, 'C');
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 2, data), 0);
	sftl_test_expect_fill(test, data, 'G');
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 6, data), 0);
	sftl_test_expect_fill(test, data, 6);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 2000, data), 0);
	sftl_test_expect_fill(test, data, 0);
	KUNIT_EXPECT_LT(test, apple_sftl_read(sftl, TEST_LOGICAL_PAGES, data), 0);
	apple_sftl_free(sftl);
}

static void sftl_test_reject_incomplete_or_ambiguous(struct kunit *test)
{
	static const enum sftl_fault faults[] = {
		SFTL_CHECKPOINT_COPY, SFTL_LANE_COPY, SFTL_BOOTSTRAP_DUPLICATE,
		SFTL_INCOMPLETE_CONTEXT, SFTL_JOURNAL_GAP,
		SFTL_JOURNAL_DUPLICATE, SFTL_BAD_FOOTER,
		SFTL_READ_FAILURE, SFTL_REFRESH, SFTL_UNKNOWN_STATUS,
		SFTL_TORN_FOOTER,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(faults); i++) {
		struct sftl_fixture *f = sftl_test_fixture(test, faults[i]);
		struct apple_sftl *sftl = NULL;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_EXPECT_LT_MSG(test, apple_sftl_load(&f->provider, &sftl), 0,
				    "fault %u", faults[i]);
		KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
		apple_sftl_free(sftl);
	}
}

static void sftl_test_trim_then_overwrite(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_TRIM);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
	sftl_test_expect_fill(test, data, 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 2, data), 0);
	sftl_test_expect_fill(test, data, 'G');
	apple_sftl_free(sftl);
}

static void sftl_test_preserve_footer_allocation_floor(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_ALLOCATION_FLOOR);
	struct apple_sftl_info info;
	struct apple_sftl *sftl = NULL;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence,
			TEST_CLOSED_FIRST + TEST_CLOSED_CHUNKS + 8 - 1);
	apple_sftl_free(sftl);
}

static void sftl_test_reject_other_geometry(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->provider.caus = 2;
	KUNIT_EXPECT_LT(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	KUNIT_EXPECT_EQ(test, f->reads, 0U);
}

static void sftl_test_reject_inactive_lane_gap(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_INACTIVE_ADDRESS);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	ret = apple_sftl_load(&f->provider, &sftl);
	if (!ret) {
		KUNIT_EXPECT_LT(test, apple_sftl_read(sftl, 0, data), 0);
		apple_sftl_free(sftl);
	} else {
		KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	}
}

static void sftl_test_scan_after_open_lane_hole(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_OPEN_HOLE);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, TEST_OPEN_FIRST + 7);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 3, data), 0);
	sftl_test_expect_fill(test, data, 'K');
	apple_sftl_free(sftl);
}

static void sftl_test_runtime_failure_stops_reads(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int reads;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	f->fault = SFTL_RUNTIME_FAILURE;
	KUNIT_EXPECT_LT(test, apple_sftl_read(sftl, 6, data), 0);
	reads = f->reads;
	KUNIT_EXPECT_LT(test, apple_sftl_read(sftl, 0, data), 0);
	KUNIT_EXPECT_EQ(test, f->reads, reads);
	apple_sftl_free(sftl);
}

static struct kunit_case apple_sftl_cases[] = {
	KUNIT_CASE(sftl_test_complete_medium),
	KUNIT_CASE(sftl_test_scan_after_open_lane_hole),
	KUNIT_CASE(sftl_test_runtime_failure_stops_reads),
	KUNIT_CASE(sftl_test_reject_incomplete_or_ambiguous),
	KUNIT_CASE(sftl_test_trim_then_overwrite),
	KUNIT_CASE(sftl_test_preserve_footer_allocation_floor),
	KUNIT_CASE(sftl_test_reject_other_geometry),
	KUNIT_CASE(sftl_test_reject_inactive_lane_gap),
	{}
};

static struct kunit_suite apple_sftl_suite = {
	.name = "apple-sftl",
	.test_cases = apple_sftl_cases,
};

kunit_test_suite(apple_sftl_suite);

MODULE_LICENSE("GPL");

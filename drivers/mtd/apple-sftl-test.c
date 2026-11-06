// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic VFL/SFTL medium. No firmware or captured device bytes. */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mtd/apple-ppn.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "apple-sftl.h"
#include "apple-sftl-limits.h"
#include "apple-sftl-reclaim.h"

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

struct sftl_test_page {
	u8 data[APPLE_PPN_PAGE_SIZE];
	u8 oob[APPLE_PPN_OOB_SIZE];
};

struct sftl_fixture {
	struct kunit *test;
	struct apple_ppn provider;
	enum sftl_fault fault;
	unsigned int reads;
	unsigned int programs;
	unsigned int fail_program;
	unsigned int tear_program;
	unsigned int program_outside;
	bool uncertain_program;
	bool no_candidates;
	bool dirty_candidate;
	bool vfl_candidate;
	bool full_lanes;
	u16 program_lanes;
	u64 sequence_bias;
	struct sftl_test_page **written[5];
	struct apple_sftl_checkpoint *checkpoint;
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

static void sftl_test_members(struct sftl_fixture *f, u8 *data)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		put_unaligned_le16(2087, data + 0x100 + i * 4);
		data[0x102 + i * 4] = i;
		data[0x103 + i * 4] = 0;
	}
	if (f->fault == SFTL_BOOTSTRAP_DUPLICATE)
		data[0x106] = 0;
	if (f->vfl_candidate)
		put_unaligned_le16(2056, data + 0x100);
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
		sftl_test_members(f, data);
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
		/* Three initially blank append candidates, single-lane by default. */
		if (page == 1 && !f->no_candidates)
			for (i = 5; i <= 7; i++)
				put_unaligned_le16(0, data + i * 2);
		if (f->full_lanes)
			put_unaligned_le16(0, data + 5 * 2);
		if (page == 1 && f->vfl_candidate) {
			put_unaligned_le16(0, data + 2056 * 2);
			put_unaligned_le16(0, data + 2057 * 2);
		}
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
	sftl_test_meta(oob, 0x1f, 1, 100 + f->sequence_bias, 0);
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
	sftl_test_meta(oob + 16, 0x1f, 4, TEST_CHECKPOINT + f->sequence_bias, 0);
}

static void sftl_test_open(struct sftl_fixture *f, u8 *data, u8 *oob)
{
	u64 first = TEST_OPEN_FIRST + f->sequence_bias;
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
		put_unaligned_le32((TEST_CLOSED_FIRST + f->sequence_bias) >> 16, data + 4);
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
				       TEST_CLOSED_FIRST + f->sequence_bias + slot, slot + 1);
		}
	} else {
		u32 span = TEST_CLOSED_CHUNKS;

		memset(data, 0, 16384);
		put_unaligned_le32(0, data);
		put_unaligned_le32((TEST_CLOSED_FIRST + f->sequence_bias) >> 16, data + 4);
		put_unaligned_le32(1, data + 8);
		put_unaligned_le32(TEST_CLOSED_CHUNKS, data + 12);
		if (f->fault == SFTL_ALLOCATION_FLOOR)
			span += 8;
		sftl_test_meta(oob, 0x1c, 8, TEST_CLOSED_FIRST + f->sequence_bias, span);
		put_unaligned_le16(f->fault == SFTL_BAD_FOOTER ? 2 : 4, oob + 12);
		put_unaligned_le16(0xffff, oob + 14);
		for (i = 1; i < 4; i++)
			memcpy(oob + i * 16, oob, 16);
		if (f->fault == SFTL_TORN_FOOTER)
			oob[63] ^= 1;
	}
}

static int sftl_test_candidate_slot(unsigned int block)
{
	if (block >= 5 && block <= 7)
		return block - 5;
	if (block == 2056 || block == 2057)
		return block - 2053;
	return -1;
}

static struct sftl_test_page *sftl_test_written(struct sftl_fixture *f,
						unsigned int slot, unsigned int lane,
						unsigned int page)
{
	if (!f->written[slot])
		return NULL;
	return f->written[slot][lane * 128 + page];
}

static int sftl_test_read_page(void *priv, unsigned int bus, unsigned int cau,
			       unsigned int block, unsigned int page,
			       bool context, u8 *data, u8 *oob, u8 *status)
{
	struct sftl_fixture *f = priv;
	unsigned int lane = cau * 2 + bus;
	unsigned int position = page * 2 + lane;
	int slot = sftl_test_candidate_slot(block);

	f->reads++;
	if (bus >= 2 || cau >= 8 || block >= 2088 || page >= (context ? 64 : 128))
		return -EINVAL;
	memset(data, 0xff, 16384);
	memset(oob, 0xff, 64);
	*status = 0x49;
	if (context) {
		if (block == 2087 || (f->vfl_candidate && block == 2056 && !lane))
			return sftl_test_context(f, lane, page, data, oob, status);
		return 0;
	}
	if (block == 3 && (f->fault == SFTL_RUNTIME_FAILURE ||
			   (!position && f->fault == SFTL_READ_FAILURE)))
		return -EIO;
	if (f->dirty_candidate && block == 5 && !lane && page == 127) {
		memset(data, 'D', APPLE_PPN_PAGE_SIZE);
		*status = 0x40;
		return 0;
	}
	if (slot >= 0 && sftl_test_written(f, slot, lane, page)) {
		struct sftl_test_page *written = sftl_test_written(f, slot, lane, page);

		memcpy(data, written->data, APPLE_PPN_PAGE_SIZE);
		memcpy(oob, written->oob, APPLE_PPN_OOB_SIZE);
		*status = 0x40;
		return 0;
	}
	if (lane > 1)
		return 0;
	if (block == 1 && f->checkpoint && position < f->checkpoint->count / 4) {
		unsigned int i;

		for (i = 0; i < 4; i++) {
			const struct apple_sftl_context_record *r;

			r = &f->checkpoint->records[position * 4 + i];

			memcpy(data + i * 4096, r->data, 4096);
			memcpy(oob + i * 16, r->meta, 16);
		}
	} else if (block == 1 && !position) {
		sftl_test_checkpoint(f, data, oob);
	} else if (block == 2 && !position) {
		sftl_test_open(f, data, oob);
	} else if (block == 2 && position == 2 && f->fault == SFTL_OPEN_HOLE) {
		unsigned int i;

		for (i = 0; i < 4; i++) {
			memset(data + i * 4096, 'H' + i, 4096);
			sftl_test_meta(oob + i * 16, 1, 0,
				       TEST_OPEN_FIRST + f->sequence_bias + 4 + i, 3);
		}
	} else if (block == 3) {
		sftl_test_closed(f, position, data, oob);
	} else if (block == 4 && !position) {
		memset(data, 'C', 4096);
		sftl_test_meta(oob, 1, 0, 10 + f->sequence_bias, 0);
	} else {
		return 0;
	}
	*status = block == 2 && f->fault == SFTL_REFRESH ? 0x42 : 0x40;
	if (block == 2 && f->fault == SFTL_UNKNOWN_STATUS)
		*status = 0x61;
	return 0;
}

#if IS_ENABLED(CONFIG_MTD_APPLE_SFTL_WRITE)
static int sftl_test_program_page(void *priv, unsigned int bus, unsigned int cau,
				  unsigned int block, unsigned int page,
				  const u8 *data, const u8 *oob)
{
	struct sftl_fixture *f = priv;
	struct sftl_test_page *written;
	unsigned int lane = cau * 2 + bus;
	int slot = sftl_test_candidate_slot(block);
	bool failure;

	f->programs++;
	if (bus >= 2 || cau >= 8 || slot < 0 || page >= 128 ||
	    (lane && (!f->full_lanes || block != 5))) {
		f->program_outside++;
		return -EPERM;
	}
	if (sftl_test_written(f, slot, lane, page))
		return -EEXIST;
	failure = f->programs == f->fail_program;
	if (failure && !f->uncertain_program)
		return -EIO;
	if (!f->written[slot]) {
		f->written[slot] = kunit_kcalloc(f->test, 16 * 128,
						 sizeof(*f->written[slot]), GFP_KERNEL);
		if (!f->written[slot])
			return -ENOMEM;
	}
	written = kunit_kmalloc(f->test, sizeof(*written), GFP_KERNEL);
	if (!written)
		return -ENOMEM;
	memcpy(written->data, data, APPLE_PPN_PAGE_SIZE);
	memcpy(written->oob, oob, APPLE_PPN_OOB_SIZE);
	f->written[slot][lane * 128 + page] = written;
	f->program_lanes |= 1U << lane;
	if (f->programs == f->tear_program) {
		written->oob[63] ^= 1;
		return -EIO;
	}
	return failure ? -EIO : 0;
}
#endif

static const struct apple_ppn_ops sftl_test_ops = {
	.read_page = sftl_test_read_page,
#if IS_ENABLED(CONFIG_MTD_APPLE_SFTL_WRITE)
	.program_page = sftl_test_program_page,
#endif
};

static struct sftl_fixture *sftl_test_fixture(struct kunit *test,
					      enum sftl_fault fault)
{
	struct sftl_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->test = test;
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

static void sftl_test_page_budget_stops_before_next_read(struct kunit *test)
{
	struct apple_sftl_io_limit limit;

	KUNIT_ASSERT_EQ(test, apple_sftl_limit_start(&limit, 2, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_end(&limit, 0, 1), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 2), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_end(&limit, 0, 3), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 4), -E2BIG);
	KUNIT_EXPECT_EQ(test, limit.reads, 2U);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 5), -EIO);
	KUNIT_EXPECT_EQ(test, limit.reads, 2U);
}

static void sftl_test_deadline_stops_before_next_read(struct kunit *test)
{
	struct apple_sftl_io_limit limit;

	KUNIT_ASSERT_EQ(test, apple_sftl_limit_start(&limit, 4, 1, 10), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 1000010), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, limit.reads, 0U);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 10), -EIO);
}

static void sftl_test_one_transaction_deadline_overrun(struct kunit *test)
{
	struct apple_sftl_io_limit limit;

	KUNIT_ASSERT_EQ(test, apple_sftl_limit_start(&limit, 4, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 999999), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_end(&limit, 0, 1000000), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 1000001), -EIO);
	KUNIT_EXPECT_EQ(test, limit.reads, 1U);
}

static void sftl_test_provider_failure_is_sticky(struct kunit *test)
{
	struct apple_sftl_io_limit limit;

	KUNIT_ASSERT_EQ(test, apple_sftl_limit_start(&limit, 4, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_end(&limit, -EBADMSG, 1), -EBADMSG);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_begin(&limit, 2), -EIO);
	KUNIT_EXPECT_EQ(test, limit.reads, 1U);
}

static void sftl_test_invalid_reader_policy(struct kunit *test)
{
	struct apple_sftl_io_limit limit;

	KUNIT_EXPECT_EQ(test, apple_sftl_limit_start(&limit, 0, 1, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_start(&limit, 1, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_limit_start(&limit, 1, 1, ~0ULL), -EINVAL);
}

#if IS_ENABLED(CONFIG_MTD_APPLE_SFTL_WRITE)
#define TEST_APPEND_FIRST (TEST_OPEN_FIRST + 4)
#define TEST_APPEND_BASE 2000U
#define TEST_APPEND_PROGRAMS 120U

static void sftl_test_append_replay_open(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	u8 *data = kunit_kmalloc(test, 16384, GFP_KERNEL);
	u8 *readback = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int chunk;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, readback);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'W', 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), 0);
	for (chunk = 0; chunk < 3; chunk++)
		memset(data + chunk * 4096, 'X' + chunk, 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 1, data, 3), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), 0);
	KUNIT_EXPECT_EQ(test, f->programs, 2U);
	KUNIT_EXPECT_EQ(test, f->program_outside, 0U);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 0, 0, 0));
	for (chunk = 0; chunk < 4; chunk++) {
		const u8 *meta = sftl_test_written(f, 0, 0, 0)->oob + chunk * 16;

		KUNIT_EXPECT_EQ(test, meta[0], (u8)1);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(meta) >> 16,
				TEST_APPEND_FIRST + chunk);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(meta + 8), TEST_APPEND_BASE);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(meta + 12),
				(u32)((TEST_APPEND_FIRST + chunk) >> 16));
	}
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, TEST_APPEND_FIRST + 7);
	for (chunk = 0; chunk < 4; chunk++) {
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + chunk, readback), 0);
		sftl_test_expect_fill(test, readback, 'W' + chunk);
	}
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 0, readback), 0);
	sftl_test_expect_fill(test, readback, 'C');
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 6, readback), 0);
	sftl_test_expect_fill(test, readback, 6);
	apple_sftl_free(sftl);
}

static int sftl_test_fill_append(struct apple_sftl *sftl, u8 *data)
{
	unsigned int program;
	int ret;

	for (program = 0; program < TEST_APPEND_PROGRAMS; program++) {
		memset(data, program + 1, 16384);
		ret = apple_sftl_write(sftl, TEST_APPEND_BASE + program * 4, data, 4);
		if (ret)
			return ret;
	}
	return 0;
}

static void sftl_test_append_replay_closed(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	u8 *data = kunit_kmalloc(test, 16384, GFP_KERNEL);
	u8 *readback = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int page;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, readback);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, sftl_test_fill_append(sftl, data), 0);
	KUNIT_EXPECT_EQ(test, f->programs, 128U);
	KUNIT_EXPECT_EQ(test, f->program_outside, 0U);
	for (page = 120; page < 128; page++) {
		const u8 *meta;

		KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 0, 0, page));
		meta = sftl_test_written(f, 0, 0, page)->oob;
		KUNIT_EXPECT_EQ(test, meta[0], (u8)0x1c);
		KUNIT_EXPECT_EQ(test, meta[1], (u8)64);
		KUNIT_EXPECT_EQ(test, get_unaligned_le64(meta) >> 16, TEST_APPEND_FIRST);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(meta + 8), 480U);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(meta + 12), (u16)32);
		KUNIT_EXPECT_EQ(test, get_unaligned_le16(meta + 14), (u16)0xffff);
		KUNIT_EXPECT_MEMEQ(test, meta, meta + 16, 16);
		KUNIT_EXPECT_MEMEQ(test, meta, meta + 32, 16);
		KUNIT_EXPECT_MEMEQ(test, meta, meta + 48, 16);
	}
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(sftl_test_written(f, 0, 0, 120)->data + 8),
			TEST_APPEND_BASE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(sftl_test_written(f, 0, 0, 120)->data + 12), 480U);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, TEST_APPEND_FIRST + 479);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, readback), 0);
	sftl_test_expect_fill(test, readback, 1);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 479, readback), 0);
	sftl_test_expect_fill(test, readback, 120);
	apple_sftl_free(sftl);
}

/* Repeating one key prevents summary coalescing: all 8160 data records
 * need separate entries, including those spanning each footer page.
 */
static void sftl_test_append_replay_full_lanes(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	const unsigned int programs = 16 * 128 - 8;
	const unsigned int chunks = programs * 4;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int program, lane, page, entry;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->full_lanes = true;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	for (program = 0; program < programs; program++) {
		memset(data, program + 1, 4096);
		KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), 0);
	}
	KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), 0);
	KUNIT_EXPECT_EQ(test, f->programs, 16U * 128);
	KUNIT_EXPECT_EQ(test, f->program_outside, 0U);
	KUNIT_EXPECT_EQ(test, f->program_lanes, (u16)0xffff);
	for (page = 0; page < 128; page++) {
		for (lane = 0; lane < 16; lane++) {
			struct sftl_test_page *written = sftl_test_written(f, 0, lane, page);
			unsigned int position = page * 16 + lane;
			unsigned int chunk;

			KUNIT_ASSERT_NOT_NULL(test, written);
			if (position >= programs) {
				const u8 *meta = written->oob;

				KUNIT_EXPECT_EQ(test, meta[0], (u8)0x1c);
				KUNIT_EXPECT_EQ(test, meta[1], (u8)64);
				KUNIT_EXPECT_EQ(test, get_unaligned_le64(meta) >> 16,
						TEST_APPEND_FIRST);
				KUNIT_EXPECT_EQ(test, get_unaligned_le32(meta + 8), chunks);
				KUNIT_EXPECT_EQ(test, get_unaligned_le16(meta + 12), (u16)32);
				KUNIT_EXPECT_EQ(test, get_unaligned_le16(meta + 14), (u16)0xffff);
				for (chunk = 1; chunk < 4; chunk++)
					KUNIT_EXPECT_MEMEQ(test, meta, meta + chunk * 16, 16);
				continue;
			}
			for (chunk = 0; chunk < 4; chunk++) {
				const u8 *meta = written->oob + chunk * 16;
				u64 sequence = TEST_APPEND_FIRST + position * 4 + chunk;

				KUNIT_EXPECT_EQ(test, meta[0], (u8)1);
				KUNIT_EXPECT_EQ(test, meta[1], (u8)0);
				KUNIT_EXPECT_EQ(test, get_unaligned_le64(meta) >> 16, sequence);
				KUNIT_EXPECT_EQ(test, get_unaligned_le32(meta + 8),
						TEST_APPEND_BASE);
				KUNIT_EXPECT_EQ(test, get_unaligned_le32(meta + 12),
						(u32)(sequence >> 16));
				sftl_test_expect_fill(test, written->data + chunk * 4096,
						      (u8)(position + 1));
			}
		}
	}
	for (entry = 0; entry < chunks; entry++) {
		struct sftl_test_page *footer = sftl_test_written(f, 0, 8 + entry / 1024, 127);
		const u8 *run = footer->data + (entry % 1024) * 16;

		KUNIT_EXPECT_EQ(test, get_unaligned_le32(run), entry);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(run + 4),
				(u32)((TEST_APPEND_FIRST + entry) >> 16));
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(run + 8), TEST_APPEND_BASE);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(run + 12), 1U);
	}
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(sftl_test_written(f, 0, 15, 127)->data +
					      (chunks % 1024) * 16 + 12), 0U);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, TEST_APPEND_FIRST + chunks - 1);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
	sftl_test_expect_fill(test, data, (u8)programs);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
	sftl_test_expect_fill(test, data, 'C');
	apple_sftl_free(sftl);
}

static void sftl_test_append_failure_stops_io(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int programs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	f->fail_program = 1;
	memset(data, 'F', 4096);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EIO);
	programs = f->programs;
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EIO);
	KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), -EIO);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 0, data), -EIO);
	KUNIT_EXPECT_EQ(test, f->programs, programs);
	KUNIT_EXPECT_EQ(test, f->program_outside, 0U);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
	sftl_test_expect_fill(test, data, 0);
	apple_sftl_free(sftl);
}

static void sftl_test_append_uncertain_program_replays(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	f->fail_program = 1;
	f->uncertain_program = true;
	memset(data, 'U', 4096);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EIO);
	KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), -EIO);
	KUNIT_EXPECT_EQ(test, f->programs, 1U);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
	sftl_test_expect_fill(test, data, 'U');
	apple_sftl_free(sftl);
}

static void sftl_test_append_interrupted_footer_replays(struct kunit *test)
{
	static const struct {
		unsigned int program;
		bool uncertain;
	} stop[] = {{121, false}, {122, false}, {128, false}, {128, true}};
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(stop); index++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl *sftl = NULL;
		u8 *data = kunit_kmalloc(test, 16384, GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_NULL(test, data);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		f->fail_program = stop[index].program;
		f->uncertain_program = stop[index].uncertain;
		KUNIT_EXPECT_EQ(test, sftl_test_fill_append(sftl, data), -EIO);
		KUNIT_EXPECT_EQ(test, f->programs, stop[index].program);
		KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), -EIO);
		apple_sftl_free(sftl);
		sftl = NULL;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 479, data), 0);
		sftl_test_expect_fill(test, data, 120);
		apple_sftl_free(sftl);
	}
}

static void sftl_test_append_torn_metadata_rejected(struct kunit *test)
{
	static const unsigned int torn[] = {1, 121};
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(torn); index++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl *sftl = NULL;
		u8 *data = kunit_kmalloc(test, 16384, GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_NULL(test, data);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		f->tear_program = torn[index];
		memset(data, 'T', 16384);
		if (torn[index] == 1)
			KUNIT_EXPECT_EQ(test,
					apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EIO);
		else
			KUNIT_EXPECT_EQ(test, sftl_test_fill_append(sftl, data), -EIO);
		apple_sftl_free(sftl);
		sftl = NULL;
		KUNIT_EXPECT_LT(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
		apple_sftl_free(sftl);
	}
}

static void sftl_test_append_scans_entire_candidate(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->dirty_candidate = true;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'B', 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), 0);
	KUNIT_EXPECT_PTR_EQ(test, sftl_test_written(f, 0, 0, 0), NULL);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 1, 0, 0));
	KUNIT_EXPECT_EQ(test, f->programs, 1U);
	KUNIT_EXPECT_EQ(test, f->program_outside, 0U);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
	sftl_test_expect_fill(test, data, 'B');
	apple_sftl_free(sftl);
}

static void sftl_test_append_reserves_vfl_members(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->vfl_candidate = true;
	f->no_candidates = true;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'V', 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), 0);
	KUNIT_EXPECT_PTR_EQ(test, sftl_test_written(f, 3, 0, 0), NULL);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 4, 0, 0));
	KUNIT_EXPECT_EQ(test, f->programs, 1U);
	KUNIT_EXPECT_EQ(test, f->program_outside, 0U);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
	sftl_test_expect_fill(test, data, 'V');
	apple_sftl_free(sftl);
}

static void sftl_test_append_closes_at_sequence_limit(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	const u64 limit = (1ULL << 48) - 1;
	u8 *data = kunit_kmalloc(test, 16384, GFP_KERNEL);
	unsigned int programs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->sequence_bias = limit - TEST_APPEND_FIRST - 479;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, sftl_test_fill_append(sftl, data), 0);
	programs = f->programs;
	KUNIT_EXPECT_EQ(test, programs, 128U);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, limit);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, f->programs, programs);
	KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), 0);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, limit);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 479, data), 0);
	sftl_test_expect_fill(test, data, 120);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, f->programs, programs);
	apple_sftl_free(sftl);
}

static void sftl_test_append_no_space_keeps_reads(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->no_candidates = true;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'N', 4096);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -ENOSPC);
	KUNIT_EXPECT_EQ(test, f->programs, 0U);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
	sftl_test_expect_fill(test, data, 'C');
	apple_sftl_free(sftl);
}

static void sftl_test_append_refuses_sequence_floor_gap(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_ALLOCATION_FLOOR);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'S', 4096);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, f->programs, 0U);
	KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
	sftl_test_expect_fill(test, data, 'C');
	apple_sftl_free(sftl);
}

static void sftl_test_append_rejects_invalid_window(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, 16384, GFP_KERNEL);
	unsigned int reads;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	reads = f->reads;
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, data, 5), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, 15625000, data, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, 15624999, data, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE, NULL, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, f->reads, reads);
	KUNIT_EXPECT_EQ(test, f->programs, 0U);
	apple_sftl_free(sftl);
}
#endif

static void sftl_test_checkpoint_cold_roundtrip(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl_snapshot *before = NULL, *after = NULL;
	struct apple_sftl_checkpoint *checkpoint = NULL;
	struct apple_sftl *sftl = NULL;
	u32 blocks[] = { 1 };

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_snapshot(sftl, 1, &before), -ENOSPC);
	KUNIT_EXPECT_PTR_EQ(test, before, NULL);
	KUNIT_ASSERT_EQ(test, apple_sftl_snapshot(sftl, 65536, &before), 0);
	apple_sftl_free(sftl);
	sftl = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_checkpoint_build(before, blocks, 1, 16, &checkpoint), 0);
	f->checkpoint = checkpoint;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_snapshot(sftl, 65536, &after), 0);
	KUNIT_EXPECT_EQ(test, after->checkpoint_sequence, checkpoint->sequence);
	KUNIT_EXPECT_EQ(test, after->nr_runs, before->nr_runs);
	KUNIT_EXPECT_MEMEQ(test, after->runs, before->runs,
			   before->nr_runs * sizeof(*before->runs));
	KUNIT_EXPECT_MEMEQ(test, after->opaque[0].data, before->opaque[0].data, 4096);
	apple_sftl_free(sftl);
	apple_sftl_snapshot_free(after);
	apple_sftl_snapshot_free(before);
	apple_sftl_checkpoint_free(checkpoint);
}

static struct kunit_case apple_sftl_cases[] = {
	KUNIT_CASE(sftl_test_page_budget_stops_before_next_read),
	KUNIT_CASE(sftl_test_deadline_stops_before_next_read),
	KUNIT_CASE(sftl_test_one_transaction_deadline_overrun),
	KUNIT_CASE(sftl_test_provider_failure_is_sticky),
	KUNIT_CASE(sftl_test_invalid_reader_policy),
#if IS_ENABLED(CONFIG_MTD_APPLE_SFTL_WRITE)
	KUNIT_CASE(sftl_test_append_replay_open),
	KUNIT_CASE(sftl_test_append_replay_closed),
	KUNIT_CASE(sftl_test_append_replay_full_lanes),
	KUNIT_CASE(sftl_test_append_failure_stops_io),
	KUNIT_CASE(sftl_test_append_uncertain_program_replays),
	KUNIT_CASE(sftl_test_append_interrupted_footer_replays),
	KUNIT_CASE(sftl_test_append_torn_metadata_rejected),
	KUNIT_CASE(sftl_test_append_scans_entire_candidate),
	KUNIT_CASE(sftl_test_append_reserves_vfl_members),
	KUNIT_CASE(sftl_test_append_closes_at_sequence_limit),
	KUNIT_CASE(sftl_test_append_no_space_keeps_reads),
	KUNIT_CASE(sftl_test_append_refuses_sequence_floor_gap),
	KUNIT_CASE(sftl_test_append_rejects_invalid_window),
#endif
	KUNIT_CASE(sftl_test_complete_medium),
	KUNIT_CASE(sftl_test_checkpoint_cold_roundtrip),
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

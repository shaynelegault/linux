// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "apple-sftl-account.h"
#include "apple-sftl-test-fixture.h"

#define TEST_ACCOUNT_RECORDS 36
#define TEST_VALID_STAT_OFFSET (4 + 8 * 16 + 8)

static void account_fixture(struct kunit *test, struct apple_sftl_checkpoint *image)
{
	u32 i;

	image->count = TEST_ACCOUNT_RECORDS;
	image->records = kunit_kcalloc(test, image->count, sizeof(*image->records),
				       GFP_KERNEL);
	if (!image->records)
		return;
	apple_sftl_test_native_prefix(image->records);
	for (i = 32; i < image->count; i++) {
		image->records[i].meta[0] = 0x1f;
		image->records[i].meta[1] = 4;
		put_unaligned_le32(U32_MAX, image->records[i].meta + 12);
	}
}

static void account_recompute_valid_lbas(struct kunit *test)
{
	struct apple_sftl_extent runs[] = {
		{ 0, 3, 512 * APPLE_SFTL_STRIDE - 1 },
		{ 3, APPLE_SFTL_LOGICAL_PAGES - 4, APPLE_SFTL_NO_ADDRESS },
		{ APPLE_SFTL_LOGICAL_PAGES - 1, 1, 2085 * APPLE_SFTL_STRIDE + 1 },
	};
	struct apple_sftl_snapshot map = { .runs = runs, .nr_runs = ARRAY_SIZE(runs) };
	struct apple_sftl_checkpoint image = {};
	u8 *stats, *entry;

	account_fixture(test, &image);
	KUNIT_ASSERT_NOT_NULL(test, image.records);
	stats = image.records[4].data;
	entry = image.records[8].data;
	/* A previously referenced donor has a stale count of four. */
	put_unaligned_le16(4, entry + 2);
	put_unaligned_le64(4, stats + TEST_VALID_STAT_OFFSET);
	entry[0] = 3;
	entry[1] = 16;
	put_unaligned_le16(111, entry + 6);
	stats[12] = 0xac;
	image.records[16].data[7] = 0x76;
	KUNIT_ASSERT_EQ(test, apple_sftl_account_map(&image, &map), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(entry + 2), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(entry + 511 * 8 + 2), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(image.records[9].data + 2), 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(image.records[12].data + 37 * 8 + 2), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(stats + TEST_VALID_STAT_OFFSET), 4ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(entry), 0x1003);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(entry + 6), 111);
	KUNIT_EXPECT_EQ(test, stats[12], (u8)0xac);
	KUNIT_EXPECT_EQ(test, image.records[16].data[7], (u8)0x76);
	runs[2].provider = APPLE_SFTL_NO_ADDRESS;
	KUNIT_ASSERT_EQ(test, apple_sftl_account_map(&image, &map), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(stats + TEST_VALID_STAT_OFFSET), 3ULL);
}

static void account_invalid_layout_or_map(struct kunit *test)
{
	struct apple_sftl_extent runs[] = {
		{ 0, 1, APPLE_SFTL_STRIDE }, { 1, APPLE_SFTL_LOGICAL_PAGES - 1, APPLE_SFTL_NO_ADDRESS },
	};
	struct apple_sftl_snapshot map = { .runs = runs, .nr_runs = ARRAY_SIZE(runs) };
	struct apple_sftl_checkpoint image = {};
	u8 *stats;

	account_fixture(test, &image);
	KUNIT_ASSERT_NOT_NULL(test, image.records);
	stats = image.records[4].data;
	put_unaligned_le16(1, image.records[9].meta + 8);
	KUNIT_EXPECT_EQ(test, apple_sftl_account_map(&image, &map), -EOPNOTSUPP);
	put_unaligned_le16(512, image.records[9].meta + 8);
	put_unaligned_le32(U32_MAX, stats + 8);
	KUNIT_EXPECT_EQ(test, apple_sftl_account_map(&image, &map), -EOPNOTSUPP);
	put_unaligned_le32(2, stats + 8);
	put_unaligned_le64(1, stats + TEST_VALID_STAT_OFFSET);
	KUNIT_EXPECT_EQ(test, apple_sftl_account_map(&image, &map), -EBADMSG);
	put_unaligned_le64(0, stats + TEST_VALID_STAT_OFFSET);
	runs[1].length--;
	KUNIT_EXPECT_EQ(test, apple_sftl_account_map(&image, &map), -EBADMSG);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(stats + TEST_VALID_STAT_OFFSET), 0ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(image.records[8].data + 8 + 2), 0);
}

static void account_footer_user_sequence(struct kunit *test)
{
	struct apple_sftl_user_seq summary = {};
	u8 footer[APPLE_SFTL_META * 4] = {};
	u32 high[] = { U32_MAX - 15, U32_MAX, 10, U32_MAX - 1 };
	u32 i;

	for (i = 0; i < ARRAY_SIZE(high); i++) {
		put_unaligned_le32(high[i], footer + i * APPLE_SFTL_META + 4);
		put_unaligned_le32(i + 1, footer + i * APPLE_SFTL_META + 12);
	}
	apple_sftl_summarize_user_seq(&summary, footer, ARRAY_SIZE(high));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data), 10U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 4), U32_MAX - 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 8), 2863311528U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 12), 3U);
	apple_sftl_summarize_user_seq(&summary, footer + APPLE_SFTL_META, 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 8), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 12), 1U);
	apple_sftl_summarize_user_seq(&summary, footer, 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 4), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(summary.data + 12), 1U);
}

static void account_context_and_retired_donor(struct kunit *test)
{
	struct apple_sftl_checkpoint image = {};
	struct apple_sftl_snapshot snapshot = {};
	u32 blocks[] = { 4, 1100 };
	u8 footer[APPLE_SFTL_META] = { 0x1c, 8 };
	u8 *p, *donor, *reads, *free, *data;

	account_fixture(test, &image);
	KUNIT_ASSERT_NOT_NULL(test, image.records);
	snapshot.nr_opaque = APPLE_SFTL_ACCOUNT_RECORDS;
	snapshot.opaque = image.records;
	p = image.records[10].data + 76 * 8;
	image.records[8].data[4 * 8] = 7;
	KUNIT_EXPECT_FALSE(test, apple_sftl_account_context_pair(&snapshot, blocks));
	p[0] = 7;
	KUNIT_EXPECT_TRUE(test, apple_sftl_account_context_pair(&snapshot, blocks));
	put_unaligned_le16(1, p + 2);
	KUNIT_EXPECT_FALSE(test, apple_sftl_account_context_pair(&snapshot, blocks));

	donor = image.records[10].data + 77 * 8; /* Block 1101. */
	reads = image.records[29].data + 77 * 4;
	free = image.records[4].data + TEST_VALID_STAT_OFFSET + 16;
	data = free + 16;
	donor[0] = 3;
	donor[1] = 8;
	put_unaligned_le16(47, donor + 6);
	put_unaligned_le16(4, footer + 12);
	put_unaligned_le16(0xffff, footer + 14);
	put_unaligned_le32(517, reads);
	put_unaligned_le64(10, free);
	put_unaligned_le64(12, data);
	put_unaligned_le16(5, donor + 2);
	KUNIT_EXPECT_EQ(test, apple_sftl_account_retire(&image, 1101, footer), -EOPNOTSUPP);
	put_unaligned_le16(0, donor + 2);
	footer[1] = 9;
	KUNIT_EXPECT_EQ(test, apple_sftl_account_retire(&image, 1101, footer), -EOPNOTSUPP);
	footer[1] = 8;
	put_unaligned_le64(0, data);
	KUNIT_EXPECT_EQ(test, apple_sftl_account_retire(&image, 1101, footer), -EBADMSG);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reads), 517U);
	KUNIT_EXPECT_EQ(test, donor[0], (u8)3);
	put_unaligned_le64(12, data);
	KUNIT_ASSERT_EQ(test, apple_sftl_account_retire(&image, 1101, footer), 0);
	KUNIT_EXPECT_EQ(test, donor[0], (u8)9);
	KUNIT_EXPECT_EQ(test, donor[1], (u8)8);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(donor + 6), 47);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reads), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(free), 11ULL);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(data), 11ULL);
	KUNIT_EXPECT_EQ(test, apple_sftl_account_retire(&image, 1101, footer), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(free), 11ULL);
}

static struct kunit_case account_cases[] = {
	KUNIT_CASE(account_recompute_valid_lbas),
	KUNIT_CASE(account_invalid_layout_or_map),
	KUNIT_CASE(account_footer_user_sequence),
	KUNIT_CASE(account_context_and_retired_donor),
	{}
};

static struct kunit_suite account_suite = {
	.name = "apple-sftl-accounting",
	.test_cases = account_cases,
};
kunit_test_suite(account_suite);
MODULE_LICENSE("GPL");

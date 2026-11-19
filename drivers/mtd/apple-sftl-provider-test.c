// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "apple-sftl-provider.h"

struct provider_fixture {
	struct apple_sftl_provider_stats stats;
	u8 vfl[88];
	u8 lower[464];
	u8 output[464];
};

static struct provider_fixture *provider_fixture(struct kunit *test)
{
	return kunit_kzalloc(test, sizeof(struct provider_fixture), GFP_KERNEL);
}

static void provider_tlv_snapshot(struct kunit *test)
{
	struct provider_fixture *f = provider_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 254, f->output, 464),
			-ENODATA);
	memset(f->vfl, 0xa5, sizeof(f->vfl));
	memset(f->lower, 0x5a, sizeof(f->lower));
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_seed(&f->stats, f->vfl, 88, f->lower, 464), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 255, f->output, 88), 88);
	KUNIT_EXPECT_MEMEQ(test, f->output, f->vfl, 88);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 254, f->output, 464), 464);
	KUNIT_EXPECT_MEMEQ(test, f->output, f->lower, 464);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 254, f->output, 88),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 252, f->output, 464),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_submit(&f->stats, 99, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 254, f->output, 464),
			-ENODATA);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_seed(&f->stats, f->vfl, 88, f->lower, 464), 0);
	apple_sftl_provider_lost(&f->stats);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_PROGRAM, 4),
			-ENODATA);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_seed(&f->stats, f->vfl, 88, f->lower, 463),
			-EINVAL);
}

static void provider_vfl_counts(struct kunit *test)
{
	struct provider_fixture *f = provider_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	put_unaligned_le64(0xfffffffffffffffcULL, f->vfl);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_seed(&f->stats, f->vfl, 88, f->lower, 464), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_PROGRAM, 8), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_READ, 19), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_ERASE, 0), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_SCATTER_BEGIN, 0), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_SCATTER_ADD, 0), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_SCATTER_ADD, 0), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_vfl(&f->stats, APPLE_SFTL_VFL_SCATTER_FLUSH, 13), 0);
	put_unaligned_le64(4, f->vfl);
	put_unaligned_le64(32, f->vfl + 8);
	put_unaligned_le64(1, f->vfl + 16);
	put_unaligned_le64(1, f->vfl + 24);
	put_unaligned_le64(1, f->vfl + 40);
	put_unaligned_le64(2, f->vfl + 48);
	put_unaligned_le64(1, f->vfl + 56);
	put_unaligned_le64(1, f->vfl + 64);
	put_unaligned_le64(1, f->vfl + 72);
	KUNIT_EXPECT_MEMEQ(test, f->stats.vfl, f->vfl, sizeof(f->vfl));
	KUNIT_EXPECT_MEMEQ(test, f->stats.lower, f->lower, sizeof(f->lower));
}

static void provider_lower_results(struct kunit *test)
{
	const u8 flags[] = { 0, 0x1f, 1, 0x10 };
	struct provider_fixture *f = provider_fixture(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	put_unaligned_le64(0xffffffffULL, f->lower + 8);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_seed(&f->stats, f->vfl, 88, f->lower, 464), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_submit(&f->stats, APPLE_SFTL_LOWER_READ, 4), 0);
	put_unaligned_le64(0x100000003ULL, f->lower + 8);
	KUNIT_EXPECT_MEMEQ(test, f->stats.lower, f->lower, sizeof(f->lower));
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_complete(&f->stats, APPLE_SFTL_LOWER_READ,
							   0x40, NULL, 4), 0);
	KUNIT_EXPECT_MEMEQ(test, f->stats.lower, f->lower, sizeof(f->lower));
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_complete(&f->stats, APPLE_SFTL_LOWER_READ,
							   0, flags, ARRAY_SIZE(flags)), 0);
	put_unaligned_le64(1, f->lower + 120);
	put_unaligned_le64(1, f->lower + 128);
	put_unaligned_le64(2, f->lower + 136);
	put_unaligned_le64(1, f->lower + 448);
	put_unaligned_le64(1, f->lower + 456);
	KUNIT_EXPECT_MEMEQ(test, f->stats.lower, f->lower, sizeof(f->lower));

	KUNIT_ASSERT_EQ(test, apple_sftl_provider_submit(&f->stats, APPLE_SFTL_LOWER_PROGRAM, 4), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_complete(&f->stats, APPLE_SFTL_LOWER_PROGRAM,
							   0, flags, ARRAY_SIZE(flags)), 0);
	put_unaligned_le64(4, f->lower);
	put_unaligned_le64(2, f->lower + 144);
	put_unaligned_le64(2, f->lower + 152);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_submit(&f->stats, APPLE_SFTL_LOWER_ERASE, 4), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_complete(&f->stats, APPLE_SFTL_LOWER_ERASE,
							   0, flags, ARRAY_SIZE(flags)), 0);
	put_unaligned_le64(4, f->lower + 16);
	put_unaligned_le64(2, f->lower + 160);
	put_unaligned_le64(2, f->lower + 168);
	KUNIT_EXPECT_MEMEQ(test, f->stats.lower, f->lower, sizeof(f->lower));

	KUNIT_ASSERT_EQ(test, apple_sftl_provider_submit(&f->stats, APPLE_SFTL_LOWER_SINGLE_ERASE, 1), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_single_erase_done(&f->stats, true), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_provider_submit(&f->stats, APPLE_SFTL_LOWER_SYNC, 1), 0);
	put_unaligned_le64(5, f->lower + 16);
	put_unaligned_le64(1, f->lower + 24);
	put_unaligned_le64(1, f->lower + 104);
	put_unaligned_le64(3, f->lower + 168);
	KUNIT_EXPECT_MEMEQ(test, f->stats.lower, f->lower, sizeof(f->lower));
	KUNIT_EXPECT_MEMEQ(test, f->stats.vfl, f->vfl, sizeof(f->vfl));
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_complete(&f->stats, APPLE_SFTL_LOWER_ERASE, 0,
							   NULL, 4), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_sftl_provider_read_stats(&f->stats, 254, f->output, 464),
			-ENODATA);
}

static struct kunit_case provider_cases[] = {
	KUNIT_CASE(provider_tlv_snapshot),
	KUNIT_CASE(provider_vfl_counts),
	KUNIT_CASE(provider_lower_results),
	{}
};

static struct kunit_suite provider_suite = {
	.name = "apple-sftl-provider",
	.test_cases = provider_cases,
};
kunit_test_suite(provider_suite);
MODULE_LICENSE("GPL");

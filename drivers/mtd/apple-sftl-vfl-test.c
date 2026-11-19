// SPDX-License-Identifier: GPL-2.0-only
/* Single-fragment, two-bank synthetic VFL media with physical replica cuts. */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "apple-sftl-vfl.h"

#define SOURCE_AGE (U32_MAX - 128)
#define TARGET_AGE (SOURCE_AGE - 64)

struct vfl_fixture {
	struct apple_sftl_vfl_source source;
	struct apple_ppn ppn;
	u8 *expected;
	u8 next[4];
	bool erased[4], bad_tail, target_ref, stale, bad_readback, short_erase;
	unsigned int mutations, cut;
};

static void test_tag(u8 *oob, u8 lane, u32 age)
{
	memset(oob, 0xff, 64);
	put_unaligned_le32(lane == 0xff ? 0x00ffff20 : 0x20 | (u32)lane << 8, oob);
	put_unaligned_le32(age, oob + 4);
	put_unaligned_le64(0, oob + 8);
}

static void test_context(struct vfl_fixture *f, u8 *data, bool initial, bool final)
{
	unsigned int i;

	memset(data, 0, APPLE_PPN_PAGE_SIZE);
	put_unaligned_le16(16, data + 10);
	put_unaligned_le16(2086, data + 12);
	put_unaligned_le16(2086, data + 14);
	for (i = 0; i < 8; i++) {
		put_unaligned_le16(f->source.members[i].block, data + 0x100 + i * 4);
		data[0x102 + i * 4] = f->source.members[i].lane;
	}
	for (i = 0; i < 16; i++) {
		u16 ref = initial && i == 9 ? 63 : i + 1;
		bool other = final || (f->target_ref && !i);

		ref |= (f->source.group ^ other) << 15;
		put_unaligned_le16(ref, data + 0x200 + 32 * i);
	}
}

static void test_target_page(struct vfl_fixture *f, unsigned int page, u8 *data, u8 *oob)
{
	if (!page || page == 17) {
		test_context(f, data, !page, page == 17);
		test_tag(oob, 0xff, TARGET_AGE - page);
	} else {
		/* Lane 9 received its last table update in source page 63. */
		memset(data, page == 10 ? 63 : page, APPLE_PPN_PAGE_SIZE);
		test_tag(oob, page - 1, TARGET_AGE - page);
	}
}

static int test_read(void *priv, unsigned int bus, unsigned int cau, unsigned int block,
		     unsigned int page, bool context, u8 *data, u8 *oob, u8 *status)
{
	struct vfl_fixture *f = priv;
	unsigned int member = 2 * cau + bus;

	if (!context || member >= 8 || block != 2087 || page >= 64)
		return -ERANGE;
	*status = 0x40;
	if (member >= 4) {
		member -= 4;
		if (page < f->next[member]) {
			test_target_page(f, page, data, oob);
			if (f->bad_readback && page == 16 && member == 3)
				data[APPLE_PPN_PAGE_SIZE - 1] ^= 1;
		} else if (!page && !f->erased[member]) {
			test_context(f, data, false, false);
			test_tag(oob, 0xff, f->stale ? SOURCE_AGE - 64 : U32_MAX);
		} else {
			*status = 0x49;
			memset(data, 0xff, APPLE_PPN_PAGE_SIZE);
			memset(oob, 0xff, APPLE_PPN_OOB_SIZE);
		}
	} else if (page == 62) {
		test_context(f, data, false, false);
		test_tag(oob, 0xff, SOURCE_AGE - page);
	} else if ((page >= 1 && page <= 16) || page == 63) {
		memset(data, page, APPLE_PPN_PAGE_SIZE);
		test_tag(oob, page == 63 ? 9 : page - 1, SOURCE_AGE - page);
		if (f->bad_tail && member == 3 && page == 63)
			data[17]++;
	} else {
		return -ERANGE;
	}
	return 0;
}

static int test_erase(void *priv, const struct apple_sftl_vfl_member *m)
{
	struct vfl_fixture *f = priv;

	if (m->block != 2087 || m->lane < 4 || m->lane >= 8 || f->erased[m->lane - 4])
		return -EINVAL;
	if (!f->short_erase)
		f->erased[m->lane - 4] = true;
	return ++f->mutations == f->cut ? -EIO : 0;
}

static int test_program(void *priv, const struct apple_sftl_vfl_member *m, u8 page,
			const u8 *data, const u8 *oob)
{
	struct vfl_fixture *f = priv;
	u8 expected_oob[APPLE_PPN_OOB_SIZE];

	if (m->block != 2087 || m->lane < 4 || m->lane >= 8 || !f->erased[m->lane - 4] ||
	    page > 17 || f->next[m->lane - 4] != page || f->mutations != 4U * page + m->lane)
		return -EINVAL;
	test_target_page(f, page, f->expected, expected_oob);
	if (memcmp(f->expected, data, APPLE_PPN_PAGE_SIZE) ||
	    memcmp(expected_oob, oob, sizeof(expected_oob)))
		return -EBADMSG;
	f->next[m->lane - 4]++;
	return ++f->mutations == f->cut ? -EIO : 0;
}

static const struct apple_ppn_ops test_read_ops = { .read_page = test_read };
static const struct apple_sftl_vfl_ops test_write_ops = {
	.erase = test_erase, .program = test_program,
};

static void vfl_rotate_test(struct kunit *test)
{
	struct vfl_fixture f = {
		.source = { .page = 62, .age = SOURCE_AGE - 62 },
		.ppn = { .ops = &test_read_ops, .buses = 2, .caus = 8, .blocks = 2088, .pages = 128 },
	};
	unsigned int i, cut;

	f.ppn.priv = &f;
	f.expected = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f.expected);
	for (i = 0; i < 8; i++) {
		f.source.members[i].block = 2087;
		f.source.members[i].lane = i;
	}

	/* Exercise every durable operation including a completed but unacknowledged
	 * last replica. No failure permits another program or a source erase.
	 */
	for (cut = 0; cut <= 76; cut++) {
		f.cut = cut;
		f.mutations = 0;
		memset(f.erased, 0, sizeof(f.erased));
		memset(f.next, 0, sizeof(f.next));
		KUNIT_ASSERT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f),
				cut ? -EIO : 0);
		KUNIT_EXPECT_EQ(test, f.mutations, cut ? cut : 76U);
	}
	memset(f.erased, 0, sizeof(f.erased));
	memset(f.next, 0, sizeof(f.next));
	f.cut = 0;
	f.mutations = 0;
	f.bad_tail = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f), -EBADMSG);
	f.bad_tail = false;
	f.target_ref = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f), -EOPNOTSUPP);
	f.target_ref = false;
	f.stale = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f), -EBADMSG);
	KUNIT_EXPECT_EQ(test, f.mutations, 0U);
	f.stale = false;
	f.short_erase = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f), -EBADMSG);
	KUNIT_EXPECT_EQ(test, f.mutations, 1U);
	f.short_erase = false;
	f.mutations = 0;
	f.bad_readback = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f), -EBADMSG);
	KUNIT_EXPECT_EQ(test, f.mutations, 72U);
	f.bad_readback = false;
	f.mutations = 0;
	memset(f.erased, 0, sizeof(f.erased));
	memset(f.next, 0, sizeof(f.next));
	f.source.group = 1;
	for (i = 0; i < 8; i++)
		f.source.members[i].lane = i ^ 4;
	KUNIT_EXPECT_EQ(test, apple_sftl_vfl_rotate(&f.ppn, &f.source, &test_write_ops, &f), 0);
	KUNIT_EXPECT_EQ(test, f.mutations, 76U);
}

static struct kunit_case vfl_cases[] = {
	KUNIT_CASE(vfl_rotate_test),
	{}
};

static struct kunit_suite vfl_suite = {
	.name = "apple-sftl-vfl-rotate",
	.test_cases = vfl_cases,
};
kunit_test_suite(vfl_suite);

MODULE_LICENSE("GPL");

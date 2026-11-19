// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic VFL/SFTL medium. No firmware or captured device bytes. */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mtd/apple-ppn.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "apple-sftl.h"
#include "apple-sftl-reclaim.h"
#include "apple-sftl-test-fixture.h"
#include "apple-sftl-vfl.h"

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

enum sftl_test_commit {
	SFTL_COMMIT_OK,
	SFTL_COMMIT_IO_ERROR,
	SFTL_COMMIT_BAD_READBACK,
	SFTL_COMMIT_UNPUBLISHED,
};

struct sftl_test_page {
	u8 data[APPLE_PPN_PAGE_SIZE];
	u8 oob[APPLE_PPN_OOB_SIZE];
};

struct sftl_fixture {
	struct kunit *test;
	struct apple_ppn provider;
	struct apple_ppn_ops ops;
	enum sftl_fault fault;
	unsigned int reads;
	bool rotating_vfl, vfl_table_changed, vfl_bad_age, vfl_stale_ref, vfl_age_collision;
	unsigned int vfl_cut, vfl_target;
	bool vfl_mount, slc_erased[8];
	unsigned int slc_mutations, slc_fail;
	struct sftl_test_page *slc[4][18];
	u16 vfl_tail_marker;
	unsigned int programs;
	unsigned int fail_program;
	unsigned int tear_program;
	unsigned int program_outside;
	bool uncertain_program;
	unsigned int append_candidates;
	unsigned int erases;
	bool fail_erase;
	unsigned int erase_pages;
	bool checkpoint_slot, show_staged, checkpoint_published;
	bool fail_reserve_once;
	unsigned int corrupt_stage_readback;
	unsigned int native_erase_cut, native_policy_calls;
	u16 erased_context_lanes;
	bool loaded_before_retirement, loaded_after_retirement;
	int native_policy_error;
	bool drop_context_role, keep_donor_role, duplicate_stream;
	unsigned int installs, recovery_reads;
	enum sftl_test_commit commit;
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
	unsigned int i, group = lane / 4, step = page * 4 + lane % 4;
	bool target = f->rotating_vfl && group == f->vfl_target;
	u32 origin = f->rotating_vfl ? (target ? 0xfffffe00 : 0xffffff00) :
				      (group ? 0xffffff00 : 0xfffffe00);
	u16 ref_group = target && !page ? 1 - group : group;

	if (target && f->vfl_age_collision)
		origin = 0xffffff00;
	if (lane >= 8 || (target && step >= f->vfl_cut))
		return 0;
	if (f->vfl_mount && f->slc_erased[lane]) {
		struct sftl_test_page *row = lane >= 4 && page < 18 ? f->slc[lane - 4][page] : NULL;

		if (row) {
			memcpy(data, row->data, APPLE_PPN_PAGE_SIZE);
			memcpy(oob, row->oob, APPLE_PPN_OOB_SIZE);
			*status = 0x40;
		}
		return 0;
	}
	if (!page || page == 17 || (f->vfl_mount && !group && page > 17)) {
		memset(data, 0, 16384);
		put_unaligned_le16(16, data + 10);
		put_unaligned_le16(2086, data + 12);
		put_unaligned_le16(2086, data + 14);
		sftl_test_members(f, data);
		for (i = 0; i < 16; i++)
			put_unaligned_le16((ref_group << 15) | (i + 1), data + 0x200 + i * 32);
		memset(oob, 0, 16);
		oob[0] = 0x20;
		oob[1] = 0xff;
		oob[2] = 0xff;
		oob[3] = 0;
		put_unaligned_le32(origin - page, oob + 4);
		if (f->vfl_stale_ref && target && !page)
			put_unaligned_le16((f->vfl_target << 15) | 1, data + 0x200);
		if (f->fault == SFTL_CHECKPOINT_COPY && lane == 1 && page == 17)
			data[0] ^= 1;
		*status = 0x40;
	} else if (page <= 16) {
		/* Lane tables occupy pages 1..16 in each of four replicas. */
		if (page <= 2)
			for (i = 1; i <= 4; i++)
				put_unaligned_le16(0, data + i * 2);
		/* Initially blank append candidates, single-lane by default. */
		if (page == 1)
			for (i = 5; i < 5 + f->append_candidates; i++)
				put_unaligned_le16(0, data + i * 2);
		if (page == 1 && f->checkpoint_slot)
			put_unaligned_le16(0, data + 7 * 2);
		if (f->full_lanes)
			put_unaligned_le16(0, data + 5 * 2);
		if (page == 1 && f->vfl_candidate) {
			put_unaligned_le16(0, data + 2056 * 2);
			put_unaligned_le16(0, data + 2057 * 2);
		}
		memset(oob, 0, 16);
		oob[0] = 0x20;
		oob[1] = page - 1;
		put_unaligned_le32(origin - page + (target && f->vfl_bad_age), oob + 4);
		if (target && f->vfl_tail_marker && page == 1)
			put_unaligned_le16(f->vfl_tail_marker, data + 2 * 2086);
		if (target && f->vfl_table_changed && page == 1)
			put_unaligned_le16(0x8000, data + 2);
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
	if (f->checkpoint_slot) {
		put_unaligned_le32(2, data);
		put_unaligned_le32(7, data + 8);
	}
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
	if (context && block == 2087 && !lane && !page && sftl_test_written(f, 2, 0, 23)) {
		if (!f->erased_context_lanes)
			f->loaded_before_retirement = true;
		if (f->erased_context_lanes == 3)
			f->loaded_after_retirement = true;
	}
	if (!context && block == 1 && (f->erased_context_lanes & (1U << lane)))
		return 0;
	if (context) {
		if (block == 2087 || (f->vfl_candidate && block == 2056 && !lane))
			return sftl_test_context(f, lane, page, data, oob, status);
		return 0;
	}
	if (f->checkpoint_slot && ((block == 1 && f->checkpoint_published) ||
	    (block == 7 && !f->checkpoint_published && !f->show_staged)))
		return 0;
	if (block == 7 && f->checkpoint_published && !page)
		f->recovery_reads++;
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

/* The GC scenarios own one normal lane of block 5. Other blocks hold
 * the saved context, open journal, or mapped data and must survive.
 */
static int sftl_test_erase_block(void *priv, unsigned int bus, unsigned int cau,
				 unsigned int block)
{
	struct sftl_fixture *f = priv;

	f->erases++;
	if (block != 5 || bus || cau || !f->written[0] ||
	    (f->checkpoint_slot && !f->recovery_reads))
		return -EPERM;
	memset(f->written[0] + 128 - f->erase_pages, 0,
	       f->erase_pages * sizeof(*f->written[0]));
	return f->fail_erase ? -EIO : 0;
}

static const struct apple_ppn_ops sftl_test_ops = {
	.read_page = sftl_test_read_page,
	.program_page = sftl_test_program_page,
};

static struct sftl_fixture *sftl_test_fixture(struct kunit *test,
					      enum sftl_fault fault)
{
	struct sftl_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->test = test;
	f->fault = fault;
	f->ops = sftl_test_ops;
	f->provider.ops = &f->ops;
	f->append_candidates = 3;
	f->erase_pages = 128;
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

static int sftl_test_slc_erase(void *priv, const struct apple_sftl_vfl_member *m)
{
	struct sftl_fixture *f = priv;

	if (m->block != 2087 || m->lane < 4 || m->lane >= 8 || f->slc_erased[m->lane])
		return -EINVAL;
	f->slc_erased[m->lane] = true;
	return ++f->slc_mutations == f->slc_fail ? -EIO : 0;
}

static int sftl_test_slc_program(void *priv, const struct apple_sftl_vfl_member *m,
				 u8 page, const u8 *data, const u8 *oob)
{
	struct sftl_fixture *f = priv;
	struct sftl_test_page *row;

	if (m->block != 2087 || m->lane < 4 || m->lane >= 8 || page >= 18 ||
	    !f->slc_erased[m->lane] || f->slc[m->lane - 4][page])
		return -EINVAL;
	row = kunit_kmalloc(f->test, sizeof(*row), GFP_KERNEL);
	if (!row)
		return -ENOMEM;
	memcpy(row->data, data, APPLE_PPN_PAGE_SIZE);
	memcpy(row->oob, oob, APPLE_PPN_OOB_SIZE);
	f->slc[m->lane - 4][page] = row;
	return ++f->slc_mutations == f->slc_fail ? -EIO : 0;
}

static void sftl_test_rotate_mounted(struct kunit *test)
{
	const struct apple_sftl_vfl_ops ops = {
		.erase = sftl_test_slc_erase, .program = sftl_test_slc_program,
	};
	unsigned int fault;
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, data);
	for (fault = 0; fault < 2; fault++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl_vfl_source source;
		struct apple_sftl *sftl = NULL;
		struct apple_sftl_info info;

		KUNIT_ASSERT_NOT_NULL(test, f);
		f->vfl_mount = true;
		f->append_candidates = 1;
		f->slc_fail = fault ? 2 : 0;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		memset(data, 'N', 4096);
		KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, 8, data, 1), 0);
		if (fault) {
			KUNIT_EXPECT_EQ(test, apple_sftl_rotate_vfl(sftl, &ops, f), -EIO);
			KUNIT_EXPECT_EQ(test, f->slc_mutations, 2U);
			KUNIT_EXPECT_EQ(test, apple_sftl_read(sftl, 8, data), -EIO);
			KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, 9, data, 1), -EIO);
		} else {
			KUNIT_ASSERT_EQ(test, apple_sftl_rotate_vfl(sftl, &ops, f), 0);
			KUNIT_EXPECT_EQ(test, f->slc_mutations, 76U);
			KUNIT_ASSERT_EQ(test, apple_sftl_vfl_source(sftl, &source), 0);
			KUNIT_EXPECT_EQ(test, source.group, (u8)1);
			KUNIT_EXPECT_EQ(test, source.page, (u8)17);
			/* This bank has unprogrammed space. A refused second
			 * rotation must leave the mounted writer usable.
			 */
			KUNIT_EXPECT_EQ(test, apple_sftl_rotate_vfl(sftl, &ops, f), -EOPNOTSUPP);
			KUNIT_EXPECT_EQ(test, f->slc_mutations, 76U);
			KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 8, data), 0);
			KUNIT_EXPECT_EQ(test, data[0], (u8)'N');
			memset(data, 'O', 4096);
			KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, 9, data, 1), 0);
		}
		apple_sftl_free(sftl);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		apple_sftl_get_info(sftl, &info);
		KUNIT_EXPECT_EQ(test, info.read_only, !!fault);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 8, data), 0);
		KUNIT_EXPECT_EQ(test, data[0], (u8)'N');
		if (!fault) {
			KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 9, data), 0);
			KUNIT_EXPECT_EQ(test, data[0], (u8)'O');
		}
		apple_sftl_free(sftl);
	}
}

/* Each native page has four independently programmed SLC replicas. */
static void sftl_test_vfl_rotation_cuts(struct kunit *test)
{
	/* Initial header, sixteen lane tables and final header, four copies each. */
	const unsigned int programs = 18 * 4;
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	u8 *data = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int group, cut;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->rotating_vfl = true;
	for (group = 0; group < 2; group++) {
		f->vfl_target = group;
		for (cut = 0; cut <= programs; cut++) {
			struct apple_sftl_vfl_source source;
			struct apple_sftl_info info;
			struct apple_sftl *sftl = NULL;

			f->vfl_cut = cut;
			KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
			apple_sftl_get_info(sftl, &info);
			KUNIT_EXPECT_EQ(test, info.read_only, cut < programs);
			KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
			KUNIT_EXPECT_EQ(test, data[0], (u8)'C');
			if (cut < programs) {
				KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, 0, data, 1), -EROFS);
				KUNIT_EXPECT_EQ(test, apple_sftl_vfl_source(sftl, &source), -EROFS);
			} else {
				KUNIT_ASSERT_EQ(test, apple_sftl_vfl_source(sftl, &source), 0);
				KUNIT_EXPECT_EQ(test, source.group, (u8)group);
				KUNIT_EXPECT_EQ(test, source.page, (u8)17);
			}
			apple_sftl_free(sftl);
		}
	}
	KUNIT_EXPECT_EQ(test, f->programs, 0U);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
}

static void sftl_test_vfl_rejects_ambiguous_tables(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->rotating_vfl = true;
	f->vfl_target = 1;
	f->vfl_cut = 8;
	f->vfl_age_collision = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	f->vfl_age_collision = false;
	f->vfl_tail_marker = 0x8003;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.read_only, true);
	apple_sftl_free(sftl);
	f->vfl_tail_marker = 3;
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	f->vfl_tail_marker = 0;
	f->vfl_table_changed = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	f->vfl_table_changed = false;
	f->vfl_bad_age = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	/* Four agreeing tables at the right coordinate but from a newer cycle. */
	f->vfl_bad_age = false;
	f->vfl_stale_ref = true;
	f->vfl_cut = 8;
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
	KUNIT_EXPECT_EQ(test, f->programs, 0U);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
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
	f->append_candidates = 0;
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
	f->append_candidates = 0;
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

/* Block 7 is the single-lane staging destination in this fixture. */
static int sftl_test_stage_read(void *priv, u32 addr, struct apple_sftl_gc_page *page)
{
	struct sftl_fixture *f = priv;
	u32 chunk = addr - 7 * TEST_STRIDE;
	u8 status;
	int ret;

	if (chunk >= 512 || (chunk & 3))
		return -ERANGE;
	ret = sftl_test_read_page(f, 0, 0, 7, chunk / 4, false, page->data, page->oob, &status);
	if (!ret && status == 0x40 && f->corrupt_stage_readback == chunk / 4 + 1)
		page->data[APPLE_PPN_PAGE_SIZE - 1] ^= 1;
	return ret;
}

static int sftl_test_stage_program(void *priv, u32 addr, const struct apple_sftl_gc_page *page)
{
	u32 chunk = addr - 7 * TEST_STRIDE;

	if (chunk >= 512 || (chunk & 3))
		return -ERANGE;
	return sftl_test_program_page(priv, 0, 0, 7, chunk / 4, page->data, page->oob);
}

static const struct apple_sftl_gc_io sftl_test_stage_io = {
	.read = sftl_test_stage_read,
	.program = sftl_test_stage_program,
};

/* Make the saved map span two physical program units by splitting its
 * unmapped tail. The first three map fragments leave logical space uncovered.
 * Both checkpoint generations are visible to every subsequent cold load.
 */
static int sftl_test_second_generation(struct sftl_fixture *f)
{
	struct apple_sftl_snapshot *snapshot = NULL, map;
	struct apple_sftl_checkpoint *image = NULL, *readback = NULL;
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_extent *runs, *tail;
	const u32 split = 1533, block = 7;
	u32 last, i;
	int ret;

	f->checkpoint_slot = true;
	f->show_staged = true;
	ret = apple_sftl_load(&f->provider, &sftl);
	if (ret)
		return ret;
	ret = apple_sftl_snapshot(sftl, 65536, &snapshot);
	apple_sftl_free(sftl);
	if (ret)
		return ret;
	map = *snapshot;
	last = map.nr_runs - 1;
	tail = &map.runs[last];
	if (tail->provider != APPLE_SFTL_NO_ADDRESS || tail->length <= split) {
		ret = -EINVAL;
		goto out;
	}
	runs = kunit_kcalloc(f->test, map.nr_runs + split, sizeof(*runs), GFP_KERNEL);
	if (!runs) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(runs, map.runs, last * sizeof(*runs));
	for (i = 0; i <= split; i++)
		runs[last + i] = (struct apple_sftl_extent) {
			.logical = tail->logical + i, .provider = APPLE_SFTL_NO_ADDRESS,
			.length = i == split ? tail->length - split : 1,
		};
	map.runs = runs;
	map.nr_runs += split;
	ret = apple_sftl_checkpoint_build(&map, &block, 1, 8, &image);
	if (ret)
		goto out;
	if (image->count != 8 || image->first_sequence != map.sequence + 1) {
		ret = -EINVAL;
		goto out;
	}
	ret = apple_sftl_checkpoint_stage(image, block, 1, &sftl_test_stage_io, f, &readback);
	if (ret) {
		if (readback)
			ret = -EINVAL;
	} else if (!readback || readback->count != image->count ||
		   memcmp(readback->records, image->records, image->count * sizeof(*image->records))) {
		ret = -EBADMSG;
	}
out:
	apple_sftl_checkpoint_free(readback);
	apple_sftl_checkpoint_free(image);
	apple_sftl_snapshot_free(snapshot);
	return ret;
}

static void sftl_test_context_power_cuts(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct sftl_test_page *saved[2];
	u8 *data = kunit_kmalloc(test, APPLE_SFTL_CHUNK, GFP_KERNEL);
	unsigned int cut;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_second_generation(f), 0);
	memcpy(saved, f->written[2], sizeof(saved));
	for (cut = 0; cut <= 2; cut++) {
		struct apple_sftl_info info;
		struct apple_sftl *sftl = NULL;

		f->written[2][0] = cut ? saved[0] : NULL;
		f->written[2][1] = cut == 2 ? saved[1] : NULL;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		apple_sftl_get_info(sftl, &info);
		KUNIT_EXPECT_EQ(test, info.checkpoint_sequence,
				cut == 2 ? TEST_APPEND_FIRST + 7 : TEST_CHECKPOINT);
		KUNIT_EXPECT_EQ(test, info.sequence, TEST_APPEND_FIRST - 1 + 4 * cut);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
		sftl_test_expect_fill(test, data, 'C');
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 2, data), 0);
		sftl_test_expect_fill(test, data, 'G');
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 6, data), 0);
		sftl_test_expect_fill(test, data, 6);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 2000, data), 0);
		sftl_test_expect_fill(test, data, 0);
		KUNIT_EXPECT_EQ(test, f->programs, 2U);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		apple_sftl_free(sftl);
	}
}

static void sftl_test_context_stage_failures(struct kunit *test)
{
	enum { BEFORE_PROGRAM, UNCERTAIN_PROGRAM, CORRUPT_READBACK };
	unsigned int page, fault;

	/* Losing power before either page, or losing its completion status,
	 * leaves different bytes on the medium. Reload only what reached NAND.
	 */
	for (page = 1; page <= 2; page++) {
		for (fault = BEFORE_PROGRAM; fault <= CORRUPT_READBACK; fault++) {
			struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
			struct apple_sftl *sftl = NULL;
			struct apple_sftl_info info;
			u8 *data = kunit_kmalloc(test, APPLE_SFTL_CHUNK, GFP_KERNEL);
			unsigned int persisted = fault == BEFORE_PROGRAM ? page - 1 : page;

			KUNIT_ASSERT_NOT_NULL(test, f);
			KUNIT_ASSERT_NOT_NULL(test, data);
			if (fault == CORRUPT_READBACK) {
				f->corrupt_stage_readback = page;
			} else {
				f->fail_program = page;
				f->uncertain_program = fault == UNCERTAIN_PROGRAM;
			}
			KUNIT_EXPECT_LT(test, sftl_test_second_generation(f), 0);
			KUNIT_EXPECT_EQ(test, f->programs, page);
			KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
			apple_sftl_get_info(sftl, &info);
			KUNIT_EXPECT_EQ(test, info.checkpoint_sequence,
					persisted == 2 ? TEST_APPEND_FIRST + 7 : TEST_CHECKPOINT);
			KUNIT_EXPECT_EQ(test, info.sequence, TEST_APPEND_FIRST - 1 + persisted * 4);
			KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 2, data), 0);
			sftl_test_expect_fill(test, data, 'G');
			KUNIT_EXPECT_EQ(test, f->programs, page);
			KUNIT_EXPECT_EQ(test, f->erases, 0U);
			apple_sftl_free(sftl);
		}
	}
}

static void sftl_test_checkpoint_stage_protects_tail(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct sftl_test_page *dirty = kunit_kmalloc(test, sizeof(*dirty), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, dirty);
	memset(dirty, 0xff, sizeof(*dirty));
	dirty->data[sizeof(dirty->data) - 1] = 0;
	KUNIT_ASSERT_EQ(test, sftl_test_program_page(f, 0, 0, 7, 127, dirty->data, dirty->oob), 0);
	KUNIT_EXPECT_EQ(test, sftl_test_second_generation(f), -EBADMSG);
	KUNIT_EXPECT_EQ(test, f->programs, 1U);
	KUNIT_EXPECT_PTR_EQ(test, sftl_test_written(f, 2, 0, 0), NULL);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 2, 0, 127));
	KUNIT_EXPECT_EQ(test, sftl_test_written(f, 2, 0, 127)->data[sizeof(dirty->data) - 1], 0);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
}

static void sftl_test_context_torn_append_replay(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	struct sftl_test_page *written;
	u8 *data = kunit_kmalloc(test, APPLE_SFTL_CHUNK, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_second_generation(f), 0);
	f->written[2][1] = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'W', APPLE_SFTL_CHUNK);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, 2, data, 1), 0);
	apple_sftl_free(sftl);
	written = sftl_test_written(f, 0, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, written);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(written->oob) >> 16, TEST_APPEND_FIRST + 4);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.checkpoint_sequence, TEST_CHECKPOINT);
	KUNIT_EXPECT_EQ(test, info.sequence, TEST_APPEND_FIRST + 7);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 2, data), 0);
	sftl_test_expect_fill(test, data, 'W');
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 6, data), 0);
	sftl_test_expect_fill(test, data, 6);
	KUNIT_EXPECT_EQ(test, f->programs, 3U);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
	apple_sftl_free(sftl);
}

static void sftl_test_reject_corrupt_generation(struct kunit *test)
{
	enum { BAD_CURSOR, DUPLICATE_STAMP, MISSING_STAMP, ORPHAN_PAYLOAD, NO_COMPLETE, DATA_GAP };
	unsigned int fault;

	for (fault = BAD_CURSOR; fault <= DATA_GAP; fault++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl *sftl = NULL;
		struct sftl_test_page *page;
		u8 *meta;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_EQ(test, sftl_test_second_generation(f), 0);
		f->written[2][1] = NULL;
		page = f->written[2][0];
		meta = page->oob + 3 * APPLE_SFTL_META;
		switch (fault) {
		case BAD_CURSOR:
			put_unaligned_le32(17, page->data + APPLE_SFTL_CHUNK);
			break;
		case DUPLICATE_STAMP:
			memcpy(meta, meta - APPLE_SFTL_META, APPLE_SFTL_META);
			break;
		case MISSING_STAMP:
			put_unaligned_le64((TEST_APPEND_FIRST + 4) << 16 | 0x041f, meta);
			break;
		case ORPHAN_PAYLOAD:
			memset(meta, 0xff, APPLE_SFTL_META);
			break;
		case NO_COMPLETE:
			f->fault = SFTL_INCOMPLETE_CONTEXT;
			break;
		case DATA_GAP:
			f->fault = SFTL_JOURNAL_GAP;
			break;
		}
		KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
		KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
		KUNIT_EXPECT_EQ(test, f->programs, 2U);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		apple_sftl_free(sftl);
	}
}

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

/* Place the final internal key in the old block. Exported disk reads
 * cannot see this key, but the allocation map still has to protect it.
 */
static int sftl_test_gc_internal_key(struct sftl_fixture *f,
				     struct apple_sftl_snapshot *map)
{
	struct sftl_test_page *page = sftl_test_written(f, 0, 0, 0);
	struct sftl_test_page *footer = sftl_test_written(f, 0, 0, 120);
	struct apple_sftl_extent *runs, *last;

	if (!page || !footer || get_unaligned_le32(footer->data + 12) != 480)
		return -EINVAL;
	put_unaligned_le32(TEST_LOGICAL_PAGES - 1, page->oob + 8);
	memcpy(footer->data + 16, footer->data, 16);
	put_unaligned_le32(TEST_LOGICAL_PAGES - 1, footer->data + 8);
	put_unaligned_le32(1, footer->data + 12);
	put_unaligned_le32(1, footer->data + 16);
	put_unaligned_le32(TEST_APPEND_BASE + 1, footer->data + 24);
	put_unaligned_le32(479, footer->data + 28);

	runs = kunit_kcalloc(f->test, map->nr_runs + 1, sizeof(*runs), GFP_KERNEL);
	if (!runs)
		return -ENOMEM;
	memcpy(runs, map->runs, map->nr_runs * sizeof(*runs));
	last = &runs[map->nr_runs - 1];
	if (last->provider != APPLE_SFTL_NO_ADDRESS || last->length < 2)
		return -EINVAL;
	last->length--;
	runs[map->nr_runs++] = (struct apple_sftl_extent) {
		.logical = TEST_LOGICAL_PAGES - 1, .length = 1, .provider = 5 * TEST_STRIDE,
	};
	map->runs = runs;
	return 0;
}

enum sftl_test_gc_checkpoint {
	GC_NO_CHECKPOINT,
	GC_CHECKPOINT_BEFORE_OVERWRITE,
	GC_CHECKPOINT_AFTER_OVERWRITE,
	GC_CHECKPOINT_WITH_INTERNAL_KEY,
};

static int sftl_test_gc_save(struct sftl_fixture *f, struct apple_sftl *sftl,
			     bool internal_key)
{
	struct apple_sftl_snapshot *snapshot = NULL, map;
	u32 blocks[] = { 1, 7 };
	int ret;

	ret = apple_sftl_snapshot(sftl, 65536, &snapshot);
	if (ret)
		return ret;
	map = *snapshot;
	if (internal_key)
		ret = sftl_test_gc_internal_key(f, &map);
	if (!ret)
		ret = apple_sftl_checkpoint_build(&map, blocks, f->checkpoint_slot ? 2 : 1, 16,
						  &f->checkpoint);
	apple_sftl_snapshot_free(snapshot);
	return ret;
}

/* Write two complete generations onto the only two available blocks.
 * Block 6 supersedes all the user data in block 5. The saved checkpoint is
 * an input to the test; normal writes perform the subsequent erase and reuse.
 */
static int sftl_test_gc_medium(struct sftl_fixture *f, u8 *data,
			       enum sftl_test_gc_checkpoint checkpoint)
{
	struct apple_sftl *sftl = NULL;
	unsigned int i;
	int ret;

	f->append_candidates = 2;
	ret = apple_sftl_load(&f->provider, &sftl);
	if (ret)
		return ret;
	ret = sftl_test_fill_append(sftl, data);
	if (ret)
		goto out;
	if (checkpoint == GC_CHECKPOINT_BEFORE_OVERWRITE) {
		ret = sftl_test_gc_save(f, sftl, false);
		if (ret)
			goto out;
		apple_sftl_free(sftl);
		ret = apple_sftl_load(&f->provider, &sftl);
		if (ret)
			return ret;
	}
	memset(data, 'L', APPLE_PPN_PAGE_SIZE);
	for (i = 0; i < TEST_APPEND_PROGRAMS; i++) {
		ret = apple_sftl_write(sftl, TEST_APPEND_BASE + 4 * i, data, 4);
		if (ret)
			goto out;
	}
	if (checkpoint == GC_CHECKPOINT_AFTER_OVERWRITE || checkpoint == GC_CHECKPOINT_WITH_INTERNAL_KEY)
		ret = sftl_test_gc_save(f, sftl, checkpoint == GC_CHECKPOINT_WITH_INTERNAL_KEY);
out:
	apple_sftl_free(sftl);
	return ret;
}

static void sftl_test_gc_reuses_obsolete_block(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	u32 new_key = TEST_APPEND_BASE + 480;
	unsigned int page;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_gc_medium(f, data, GC_CHECKPOINT_AFTER_OVERWRITE), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'R', 4096);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, new_key, data, 1), -ENOSPC);
	apple_sftl_free(sftl);
	f->ops.erase_block = sftl_test_erase_block;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, new_key, data, 1), 0);
	KUNIT_EXPECT_EQ(test, f->erases, 1U);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 0, 0, 0));
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(sftl_test_written(f, 0, 0, 0)->oob + 8),
			new_key);
	KUNIT_EXPECT_EQ(test, apple_sftl_flush(sftl), 0);
	apple_sftl_free(sftl);

	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, new_key, data), 0);
	sftl_test_expect_fill(test, data, 'R');
	for (page = 0; page < 480; page++) {
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + page, data), 0);
		sftl_test_expect_fill(test, data, 'L');
	}
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 0, data), 0);
	sftl_test_expect_fill(test, data, 'C');
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, 2, data), 0);
	sftl_test_expect_fill(test, data, 'G');
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

/* Apple also writes a one-page footer with sparse sequence allocation.
 * Model an older generation, followed later by all the mapped data.
 */
static void sftl_test_gc_short_native_footer(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct sftl_test_page *p;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	unsigned int page, chunk;
	u64 base = 10;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->sequence_bias = 2048;
	KUNIT_ASSERT_EQ(test, sftl_test_gc_medium(f, data, GC_CHECKPOINT_AFTER_OVERWRITE), 0);
	for (page = 0; page < 127; page++) {
		p = sftl_test_written(f, 0, 0, page);
		KUNIT_ASSERT_NOT_NULL(test, p);
		for (chunk = 0; chunk < 4; chunk++) {
			u32 slot = page * 4 + chunk;
			u64 sequence = base + slot + (slot >= 480 ? 8 : 0);

			sftl_test_meta(p->oob + 16 * chunk, 1, 0, sequence,
				       TEST_APPEND_BASE + slot % 480);
		}
	}
	p = sftl_test_written(f, 0, 0, 127);
	KUNIT_ASSERT_NOT_NULL(test, p);
	memset(p->data, 0, sizeof(p->data));
	put_unaligned_le32(TEST_APPEND_BASE, p->data + 8);
	put_unaligned_le32(480, p->data + 12);
	put_unaligned_le32(488, p->data + 16);
	put_unaligned_le32(TEST_APPEND_BASE, p->data + 24);
	put_unaligned_le32(28, p->data + 28);
	for (chunk = 0; chunk < 4; chunk++) {
		u8 *meta = p->oob + 16 * chunk;

		sftl_test_meta(meta, 0x1c, 8, base, 516);
		put_unaligned_le16(4, meta + 12);
		put_unaligned_le16(0xffff, meta + 14);
	}
	f->ops.erase_block = sftl_test_erase_block;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	memset(data, 'S', 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1), 0);
	KUNIT_EXPECT_EQ(test, f->erases, 1U);
	apple_sftl_free(sftl);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 480, data), 0);
	sftl_test_expect_fill(test, data, 'S');
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static void sftl_test_gc_preserves_protected_blocks(struct kunit *test)
{
	static const enum sftl_test_gc_checkpoint cases[] = {
		GC_NO_CHECKPOINT,              /* Needed to replay the journal */
		GC_CHECKPOINT_BEFORE_OVERWRITE, /* Still live in the durable map */
		GC_CHECKPOINT_WITH_INTERNAL_KEY,
		GC_CHECKPOINT_AFTER_OVERWRITE,  /* Closed footer hides a newer record */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl *sftl = NULL;
		u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
		unsigned int programs;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_NULL(test, data);
		KUNIT_ASSERT_EQ(test, sftl_test_gc_medium(f, data, cases[i]), 0);
		if (cases[i] == GC_CHECKPOINT_AFTER_OVERWRITE) {
			struct sftl_test_page *p = sftl_test_written(f, 0, 0, 64);

			KUNIT_ASSERT_NOT_NULL(test, p);
			sftl_test_meta(p->oob, 2, 0, f->checkpoint->sequence + 1, 0xffff0001);
		}
		f->ops.erase_block = sftl_test_erase_block;
		programs = f->programs;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_EXPECT_LT(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1), 0);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		apple_sftl_free(sftl);
		apple_sftl_checkpoint_free(f->checkpoint);
	}
}

static void sftl_test_context_pins_fallback_blocks(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl_snapshot *snapshot = NULL;
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	struct sftl_test_page *last;
	unsigned int cut, programs;
	u64 newest;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->checkpoint_slot = true;
	KUNIT_ASSERT_EQ(test, sftl_test_gc_medium(f, data, GC_CHECKPOINT_BEFORE_OVERWRITE), 0);
	KUNIT_ASSERT_EQ(test, sftl_test_second_generation(f), 0);
	last = f->written[2][1];
	newest = get_unaligned_le64(last->oob + 3 * APPLE_SFTL_META) >> 16;
	programs = f->programs;
	f->ops.erase_block = sftl_test_erase_block;
	for (cut = 1; cut <= 2; cut++) {
		f->written[2][1] = cut == 2 ? last : NULL;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_ASSERT_EQ(test, apple_sftl_snapshot(sftl, 65536, &snapshot), 0);
		KUNIT_EXPECT_EQ(test, snapshot->checkpoint_sequence,
				cut == 2 ? newest : f->checkpoint->sequence);
		KUNIT_EXPECT_EQ(test, snapshot->sequence, newest - (2 - cut) * 4);
		apple_sftl_snapshot_free(snapshot);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
		sftl_test_expect_fill(test, data, 'L');
		/* The older saved map still refers to block 5, now fully superseded. */
		KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1),
				-ENOSPC);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		apple_sftl_free(sftl);
	}
	apple_sftl_checkpoint_free(f->checkpoint);
}

static void sftl_test_gc_erase_failure(struct kunit *test)
{
	static const struct {
		unsigned int pages;
		bool fails;
	} outcome[] = {
		{ 0, true }, { 64, true }, { 128, true },
		{ 64, false }, /* A false success cannot bypass blank readback. */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(outcome); i++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl *sftl = NULL;
		u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
		unsigned int programs;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_NULL(test, data);
		KUNIT_ASSERT_EQ(test, sftl_test_gc_medium(f, data, GC_CHECKPOINT_AFTER_OVERWRITE), 0);
		f->ops.erase_block = sftl_test_erase_block;
		f->fail_erase = outcome[i].fails;
		f->erase_pages = outcome[i].pages;
		programs = f->programs;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_EXPECT_LT(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1), 0);
		KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1), -EIO);
		KUNIT_EXPECT_EQ(test, f->erases, 1U);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		apple_sftl_free(sftl);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 480, data), 0);
		sftl_test_expect_fill(test, data, 0);
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE, data), 0);
		sftl_test_expect_fill(test, data, 'L');
		if (outcome[i].pages == 64) {
			f->fail_erase = false;
			KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480,
							       data, 1), -ENOSPC);
			KUNIT_EXPECT_EQ(test, f->erases, 1U);
			KUNIT_EXPECT_EQ(test, f->programs, programs);
		}
		apple_sftl_free(sftl);
		apple_sftl_checkpoint_free(f->checkpoint);
	}
}

/* The saved map still owns five slots at the end of closed block 5.
 * Block 6 is the only remaining blank block.
 */
static int sftl_test_gc_live_medium(struct sftl_fixture *f, u8 *data)
{
	struct apple_sftl *sftl = NULL;
	unsigned int i;
	int ret;

	f->append_candidates = 2;
	f->checkpoint_slot = true;
	ret = apple_sftl_load(&f->provider, &sftl);
	if (ret)
		return ret;
	for (i = 0; i < TEST_APPEND_PROGRAMS - 1; i++) {
		memset(data, i + 1, 4096);
		ret = apple_sftl_write(sftl, TEST_APPEND_BASE, data, 1);
		if (ret)
			goto out;
	}
	for (i = 0; i < 4; i++)
		memset(data + i * 4096, 120 + i, 4096);
	ret = apple_sftl_write(sftl, TEST_APPEND_BASE + 1, data, 4);
	if (!ret)
		ret = sftl_test_gc_save(f, sftl, false);
out:
	apple_sftl_free(sftl);
	return ret;
}

static void sftl_test_gc_preserves_last_blank_capacity(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_gc_live_medium(f, data), 0);
	f->ops.erase_block = sftl_test_erase_block;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	/* Without a checkpoint writer, all 480 foreground pages must fit. */
	memset(data, 'W', APPLE_PPN_PAGE_SIZE);
	for (i = 0; i < TEST_APPEND_PROGRAMS; i++)
		KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5 + 4 * i,
						       data, 4), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 485, data, 1), -ENOSPC);
	KUNIT_EXPECT_EQ(test, f->programs, 256U);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
	apple_sftl_free(sftl);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	for (i = 0; i < 5; i++) {
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + i, data), 0);
		sftl_test_expect_fill(test, data, 119 + i);
	}
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 484, data), 0);
	sftl_test_expect_fill(test, data, 'W');
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static int sftl_test_gc_reserve(void *priv, u32 *blocks, u32 *nr_blocks)
{
	struct sftl_fixture *f = priv;

	if (f->fail_reserve_once) {
		f->fail_reserve_once = false;
		return -ENOMEM;
	}
	if (!f->checkpoint_slot || f->checkpoint_published || !*nr_blocks)
		return -ENOSPC;
	blocks[0] = 7;
	*nr_blocks = 1;
	return 0;
}

/* This fixture has two context slots with an atomic selection switch. Read
 * the new slot through the NAND provider, independently of the submitted image.
 */
static int sftl_test_gc_install(void *priv, const struct apple_sftl_checkpoint *image,
				struct apple_sftl_checkpoint **out)
{
	struct sftl_fixture *f = priv;
	struct apple_sftl_checkpoint *readback;
	struct sftl_test_page *page;
	unsigned int i;
	u8 status;
	int ret;

	f->installs++;
	*out = NULL;
	if (image->count != 4 || get_unaligned_le32(image->records[0].data + 4) != 7)
		return -EINVAL;
	page = kunit_kmalloc(f->test, sizeof(*page), GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	for (i = 0; i < 4; i++) {
		memcpy(page->data + i * 4096, image->records[i].data, 4096);
		memcpy(page->oob + i * 16, image->records[i].meta, 16);
	}
	ret = sftl_test_program_page(f, 0, 0, 7, 0, page->data, page->oob);
	if (ret || f->commit == SFTL_COMMIT_IO_ERROR)
		return ret ? ret : -EIO;
	f->show_staged = true;
	ret = sftl_test_read_page(f, 0, 0, 7, 0, false, page->data, page->oob, &status);
	f->show_staged = false;
	if (ret || status != 0x40)
		return ret ? ret : -EIO;
	readback = kzalloc(sizeof(*readback), GFP_KERNEL);
	if (!readback)
		return -ENOMEM;
	readback->records = kcalloc(4, sizeof(*readback->records), GFP_KERNEL);
	if (!readback->records) {
		kfree(readback);
		return -ENOMEM;
	}
	readback->count = 4;
	readback->first_sequence = get_unaligned_le64(page->oob) >> 16;
	readback->sequence = get_unaligned_le64(page->oob + 48) >> 16;
	for (i = 0; i < 4; i++) {
		memcpy(readback->records[i].data, page->data + i * 4096, 4096);
		memcpy(readback->records[i].meta, page->oob + i * 16, 16);
	}
	if (f->commit == SFTL_COMMIT_BAD_READBACK)
		readback->records[0].data[64] ^= 1;
	f->checkpoint_published = f->commit == SFTL_COMMIT_OK;
	*out = readback;
	return 0;
}

static const struct apple_sftl_checkpoint_ops sftl_test_gc_ops = {
	.reserve = sftl_test_gc_reserve,
	.install = sftl_test_gc_install,
};

static void sftl_test_gc_relocated_data(struct kunit *test, struct apple_sftl *sftl,
				u8 *data, u8 foreground)
{
	unsigned int i;

	for (i = 0; i < 5; i++) {
		KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + i, data), 0);
		sftl_test_expect_fill(test, data, 119 + i);
	}
	KUNIT_ASSERT_EQ(test, apple_sftl_read(sftl, TEST_APPEND_BASE + 5, data), 0);
	sftl_test_expect_fill(test, data, foreground);
}

/* Build a 96-record saved context from actual ordinary writes. The native
 * shape has four chunks of base and a full padded batch of 64 map records.
 * The old two-lane context really has records on both lanes.
 */
static int sftl_test_native_context(struct sftl_fixture *f)
{
	struct apple_sftl_snapshot *snapshot = NULL, map;
	struct apple_sftl_checkpoint *image = NULL;
	struct apple_sftl *sftl = NULL;
	const u32 blocks[] = { 1, 7 };
	int ret;

	ret = apple_sftl_load(&f->provider, &sftl);
	if (ret)
		return ret;
	ret = apple_sftl_snapshot(sftl, 65536, &snapshot);
	apple_sftl_free(sftl);
	if (ret)
		return ret;
	map = *snapshot;
	map.nr_opaque = 32;
	map.opaque = kvzalloc(array_size(map.nr_opaque, sizeof(*map.opaque)), GFP_KERNEL);
	if (!map.opaque) {
		ret = -ENOMEM;
		goto out;
	}
	apple_sftl_test_native_prefix(map.opaque);
	map.opaque[8].data[8 * blocks[0]] = 7;
	map.opaque[8].data[8 * blocks[1]] = 7;
	map.opaque[8].data[8 * 5] = 3;
	map.opaque[8].data[8 * 5 + 1] = 64;
	map.opaque[8].data[8 * 6] = 1;
	put_unaligned_le32(9, map.opaque[28].data + 5 * sizeof(u32));
	ret = apple_sftl_checkpoint_build(&map, blocks, 2, 96, &image);
	kvfree(map.opaque);
	if (ret)
		goto out;
	if (image->count != 96) {
		apple_sftl_checkpoint_free(image);
		ret = -EINVAL;
		goto out;
	}
	apple_sftl_checkpoint_free(f->checkpoint);
	f->checkpoint = image;
	f->show_staged = true;
out:
	apple_sftl_snapshot_free(snapshot);
	return ret;
}

static int sftl_test_native_medium(struct sftl_fixture *f, u8 *data)
{
	int ret = sftl_test_gc_live_medium(f, data);

	return ret ? ret : sftl_test_native_context(f);
}

/* Only this synthetic medium authorizes the remaining stream/role state. */
static int sftl_test_prepare_native(void *priv, struct apple_sftl_context_record *records,
				    u32 count, const struct apple_sftl_snapshot *map,
				    u32 victim, u32 landing, const u32 blocks[2])
{
	struct sftl_fixture *f = priv;

	f->native_policy_calls++;
	if (count != 32 || !map->nr_runs || records[0].meta[1] != 1 ||
	    blocks[0] != 7 || blocks[1] != 1 || victim != 5 || landing != 6)
		return -EINVAL;
	if (records[8].data[8 * victim] != 3 || records[8].data[8 * landing] != 1)
		return -EINVAL;
	if (f->drop_context_role)
		records[8].data[8 * blocks[1]] = 9;
	if (!f->keep_donor_role)
		records[8].data[8 * victim] = 9;
	put_unaligned_le32(0, records[28].data + victim * sizeof(u32));
	records[8].data[8 * landing] = 4;
	if (f->duplicate_stream)
		records[8].data[8 * 4] = 4;
	return f->native_policy_error;
}

static const struct apple_sftl_checkpoint_ops sftl_test_native_ops = {
	.prepare_native = sftl_test_prepare_native,
};

static int sftl_test_native_erase(void *priv, unsigned int bus, unsigned int cau, unsigned int block)
{
	struct sftl_fixture *f = priv;

	f->erases++;
	if (f->erases == f->native_erase_cut)
		return -EIO;
	if (cau || bus > 1)
		return -EPERM;
	if (block == 1 && f->loaded_before_retirement && !(f->erased_context_lanes & (1U << bus))) {
		f->erased_context_lanes |= 1U << bus;
		return 0;
	}
	if (block != 5 || bus || f->erased_context_lanes != 3 || !f->loaded_after_retirement)
		return -EPERM;
	memset(f->written[0], 0, 128 * sizeof(*f->written[0]));
	return 0;
}

static void sftl_test_gc_native_publication(struct kunit *test)
{
	struct apple_sftl_info info;
	unsigned int cut;

	for (cut = 0; cut <= 3; cut++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
		struct apple_sftl *sftl = NULL;
		u64 newest;
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_NULL(test, data);
		KUNIT_ASSERT_EQ(test, sftl_test_native_medium(f, data), 0);
		newest = f->checkpoint->sequence + 8 + 96;
		f->ops.erase_block = sftl_test_native_erase;
		f->native_erase_cut = cut;
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		apple_sftl_set_checkpoint_ops(sftl, &sftl_test_native_ops, f);
		memset(data, 'W', APPLE_SFTL_CHUNK);
		ret = apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1);
		KUNIT_EXPECT_EQ(test, ret, cut ? -EIO : 0);
		KUNIT_EXPECT_EQ(test, f->native_policy_calls, 1U);
		KUNIT_EXPECT_EQ(test, f->erases, cut ? cut : 3U);
		KUNIT_EXPECT_EQ(test, f->erased_context_lanes, cut == 1 ? 0 : (cut == 2 ? 1 : 3));
		KUNIT_EXPECT_EQ(test, f->loaded_before_retirement, true);
		KUNIT_EXPECT_EQ(test, f->loaded_after_retirement, !cut || cut == 3);
		if (cut)
			KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EIO);
		apple_sftl_free(sftl);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		apple_sftl_get_info(sftl, &info);
		KUNIT_EXPECT_EQ(test, info.checkpoint_sequence, newest);
		KUNIT_EXPECT_EQ(test, info.sequence, cut ? newest : newest + 4);
		sftl_test_gc_relocated_data(test, sftl, data, cut ? 0 : 'W');
		if (cut == 2) {
			KUNIT_EXPECT_EQ(test, info.read_only, true);
			KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1),
					-EROFS);
		}
		if (cut)
			KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 0, 0, 0));
		else
			KUNIT_EXPECT_PTR_EQ(test, sftl_test_written(f, 0, 0, 0), NULL);
		KUNIT_EXPECT_EQ(test, f->erases, cut ? cut : 3U);
		apple_sftl_free(sftl);
		if (cut == 2) {
			/* Residual data on the old lane must predate the new generation.
			 * A newer tag among its surviving records cannot be discarded.
			 */
			u8 *meta = f->checkpoint->records[4].meta;

			put_unaligned_le64((newest + 1) << 16 | 0x021f, meta);
			KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
			KUNIT_EXPECT_PTR_EQ(test, sftl, NULL);
			apple_sftl_free(sftl);
		}
		apple_sftl_checkpoint_free(f->checkpoint);
	}
}

static void sftl_test_gc_native_prefix_cuts(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	struct sftl_test_page *saved[24];
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	unsigned int cut, programs;
	u64 first;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_native_medium(f, data), 0);
	f->ops.erase_block = sftl_test_native_erase;
	f->native_erase_cut = 1;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_set_checkpoint_ops(sftl, &sftl_test_native_ops, f);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EIO);
	apple_sftl_free(sftl);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 2, 0, 23));
	memcpy(saved, f->written[2], sizeof(saved));
	first = f->checkpoint->sequence + 9;
	programs = f->programs;
	for (cut = 0; cut <= ARRAY_SIZE(saved); cut++) {
		memcpy(f->written[2], saved, cut * sizeof(*saved));
		memset(f->written[2] + cut, 0, (ARRAY_SIZE(saved) - cut) * sizeof(*saved));
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		apple_sftl_get_info(sftl, &info);
		KUNIT_EXPECT_EQ(test, info.checkpoint_sequence,
				cut == 24 ? first + 95 : f->checkpoint->sequence);
		KUNIT_EXPECT_EQ(test, info.sequence, first - 1 + cut * 4);
		if (cut && cut < 24) {
			KUNIT_EXPECT_EQ(test, info.read_only, true);
			KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EROFS);
		}
		sftl_test_gc_relocated_data(test, sftl, data, 0);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		KUNIT_EXPECT_EQ(test, f->erases, 1U);
		apple_sftl_free(sftl);
	}
	apple_sftl_checkpoint_free(f->checkpoint);
}

static void sftl_test_gc_native_policy_refuses(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	struct apple_sftl *sftl = NULL;
	unsigned int programs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_native_medium(f, data), 0);
	f->ops.erase_block = sftl_test_native_erase;
	f->native_policy_error = -EAGAIN;
	programs = f->programs;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_set_checkpoint_ops(sftl, &sftl_test_native_ops, f);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->native_policy_calls, 1U);
	f->native_policy_error = 0;
	f->drop_context_role = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EBADMSG);
	KUNIT_EXPECT_EQ(test, f->native_policy_calls, 2U);
	f->drop_context_role = false;
	f->keep_donor_role = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EBADMSG);
	f->keep_donor_role = false;
	f->duplicate_stream = true;
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EBADMSG);
	KUNIT_EXPECT_EQ(test, f->native_policy_calls, 4U);
	KUNIT_EXPECT_EQ(test, f->programs, programs);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
	sftl_test_gc_relocated_data(test, sftl, data, 0);
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

/* An unnamed context or reserved spare has the same erased bytes as a free
 * landing block. Its saved role still owns it, even in append-only mode.
 */
static void sftl_test_native_blank_ownership(struct kunit *test)
{
	static const struct { u8 role; u16 live; } cases[] = {
		{ 7, 0 }, { 8, 0 }, { 0xff, 0 }, { 4, 0 }, { 9, 0 }, { 2, 0 }, { 1, 1 },
	};
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	struct apple_sftl *sftl = NULL;
	unsigned int programs, i;
	u8 *entry;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_native_medium(f, data), 0);
	programs = f->programs;
	entry = f->checkpoint->records[8].data + 6 * 8;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		entry[0] = cases[i].role;
		put_unaligned_le16(cases[i].live, entry + 2);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1),
				-ENOSPC);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		apple_sftl_free(sftl);
	}
	entry[0] = 1;
	put_unaligned_le16(0, entry + 2);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), 0);
	KUNIT_EXPECT_EQ(test, f->programs, programs + 1);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static int sftl_test_erase_native_free(void *priv, unsigned int bus, unsigned int cau,
				       unsigned int block)
{
	struct sftl_fixture *f = priv;

	f->erases++;
	if (bus || cau || block != 5 || !f->written[0])
		return -EPERM;
	memset(f->written[0], 0, 128 * sizeof(*f->written[0]));
	return 0;
}

/* Both recovered maps have released the old ordinary data. Direct erase
 * still requires the native checkpoint to have retired its ownership.
 */
static void sftl_test_native_retired_ownership(struct kunit *test)
{
	static const struct { u8 role, flags; u16 live; } cases[] = {
		{ 3, 64, 0 }, { 3, 64, 1 }, { 7, 64, 0 }, { 8, 64, 0 },
		{ 9, 8, 0 }, { 9, 64, 1 },
	};
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	struct apple_sftl *sftl = NULL;
	unsigned int programs, i;
	u8 *entry;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->checkpoint_slot = true;
	KUNIT_ASSERT_EQ(test, sftl_test_gc_medium(f, data, GC_CHECKPOINT_AFTER_OVERWRITE), 0);
	KUNIT_ASSERT_EQ(test, sftl_test_native_context(f), 0);
	f->ops.erase_block = sftl_test_erase_native_free;
	programs = f->programs;
	entry = f->checkpoint->records[8].data + 5 * 8;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		entry[0] = cases[i].role;
		entry[1] = cases[i].flags;
		put_unaligned_le16(cases[i].live, entry + 2);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1),
				-ENOSPC);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		apple_sftl_free(sftl);
	}
	entry[0] = 9;
	entry[1] = 64;
	put_unaligned_le16(0, entry + 2);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1), 0);
	KUNIT_EXPECT_EQ(test, f->programs, programs + 1);
	KUNIT_EXPECT_EQ(test, f->erases, 1U);
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static void sftl_test_gc_retries_reservation(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	unsigned int programs;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_EQ(test, sftl_test_gc_live_medium(f, data), 0);
	f->ops.erase_block = sftl_test_erase_block;
	f->fail_reserve_once = true;
	programs = f->programs;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_set_checkpoint_ops(sftl, &sftl_test_gc_ops, f);
	memset(data, 'R', 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -ENOMEM);
	KUNIT_EXPECT_EQ(test, f->programs, programs);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
	KUNIT_EXPECT_EQ(test, f->installs, 0U);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), 0);
	KUNIT_EXPECT_EQ(test, f->erases, 1U);
	KUNIT_EXPECT_EQ(test, f->installs, 1U);
	apple_sftl_free(sftl);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	sftl_test_gc_relocated_data(test, sftl, data, 'R');
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static void sftl_test_gc_relocates_on_write(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL, *cold = NULL;
	struct apple_sftl_snapshot *before, *after;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	struct sftl_test_page *page, *copy;
	u8 saved[APPLE_PPN_OOB_SIZE];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	f->sequence_bias = 0x20000ULL - (TEST_APPEND_FIRST + 479 + 4 + 6);
	KUNIT_ASSERT_EQ(test, sftl_test_gc_live_medium(f, data), 0);
	/* The five live donor chunks already came from a previous user epoch. */
	page = sftl_test_written(f, 0, 0, 120);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, get_unaligned_le32(page->data + 475 * 16 + 12), 5U);
	put_unaligned_le32(0, page->data + 475 * 16 + 4);
	for (i = 475; i < 480; i++) {
		page = sftl_test_written(f, 0, 0, i / 4);
		KUNIT_ASSERT_NOT_NULL(test, page);
		put_unaligned_le32(0, page->oob + i % 4 * 16 + 12);
	}
	f->ops.erase_block = sftl_test_erase_block;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_set_checkpoint_ops(sftl, &sftl_test_gc_ops, f);
	memset(data, 'W', 4096);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), 0);
	KUNIT_EXPECT_EQ(test, f->installs, 1U);
	KUNIT_EXPECT_EQ(test, f->erases, 1U);
	KUNIT_EXPECT_PTR_EQ(test, sftl_test_written(f, 0, 0, 0), NULL);
	for (i = 0; i < 5; i++) {
		unsigned int slot = 3 + i; /* Head padding repeats the first key. */

		page = sftl_test_written(f, 1, 0, slot / 4);
		KUNIT_ASSERT_NOT_NULL(test, page);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->oob + (slot % 4) * 16 + 8),
				TEST_APPEND_BASE + i);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->oob + (slot % 4) * 16 + 12), 0U);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->oob + (slot % 4) * 16 + 4),
				i < 2 ? 1U : 2U);
		sftl_test_expect_fill(test, page->data + slot % 4 * 4096, 119 + i);
	}
	page = sftl_test_written(f, 1, 0, 2);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->oob + 8), TEST_APPEND_BASE + 5);
	sftl_test_gc_relocated_data(test, sftl, data, 'W');
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &cold), 0);
	sftl_test_gc_relocated_data(test, cold, data, 'W');
	apple_sftl_free(cold);

	/* Close the same block across the checkpoint's sequence gap. */
	memset(data, 'Z', APPLE_PPN_PAGE_SIZE);
	for (i = 3; i < TEST_APPEND_PROGRAMS; i++)
		KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 4 * i, data, 4), 0);
	/* All five sequential keys share one historical age across the weave carry. */
	page = sftl_test_written(f, 1, 0, 120);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->data + 3 * 16), 3U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->data + 3 * 16 + 4), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->data + 3 * 16 + 8), TEST_APPEND_BASE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(page->data + 3 * 16 + 12), 5U);
	KUNIT_ASSERT_EQ(test, get_unaligned_le32(page->data + 4 * 16 + 4), 2U);
	put_unaligned_le32(3, page->data + 4 * 16 + 4);
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &cold), -EBADMSG);
	apple_sftl_free(cold);
	put_unaligned_le32(2, page->data + 4 * 16 + 4);
	page = sftl_test_written(f, 1, 0, 127);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_EQ(test, page->oob[0], (u8)0x1c);
	/* A power cut before the last footer program leaves a recoverable prefix. */
	f->written[1][127] = NULL;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &cold), 0);
	sftl_test_gc_relocated_data(test, cold, data, 'W');
	apple_sftl_free(cold);
	/* Move one sequence gap outside the checkpoint, keeping the post-
	 * checkpoint journal and partial footer intact. Recovery must reject it.
	 */
	copy = sftl_test_written(f, 1, 0, 1);
	memcpy(saved, copy->oob, sizeof(saved));
	for (i = 0; i < 4; i++) {
		u8 *meta = copy->oob + i * 16;
		u64 seq = get_unaligned_le64(meta) >> 16;

		sftl_test_meta(meta, 1, 0, seq + 1, get_unaligned_le32(meta + 8));
	}
	KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &cold), -EBADMSG);
	apple_sftl_free(cold);
	memcpy(copy->oob, saved, sizeof(saved));
	f->written[1][127] = page;
	memset(data, 'Z', APPLE_PPN_PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 480, data, 1), 0);
	KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 0, 0, 0));
	KUNIT_EXPECT_EQ(test, f->erases, 1U);
	KUNIT_ASSERT_EQ(test, apple_sftl_snapshot(sftl, 65536, &before), 0);
	apple_sftl_free(sftl);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &cold), 0);
	KUNIT_ASSERT_EQ(test, apple_sftl_snapshot(cold, 65536, &after), 0);
	KUNIT_ASSERT_EQ(test, after->nr_runs, before->nr_runs);
	KUNIT_EXPECT_MEMEQ(test, after->runs, before->runs, before->nr_runs * sizeof(*before->runs));
	apple_sftl_snapshot_free(after);
	apple_sftl_snapshot_free(before);
	sftl_test_gc_relocated_data(test, cold, data, 'W');
	KUNIT_ASSERT_EQ(test, apple_sftl_read(cold, TEST_APPEND_BASE + 480, data), 0);
	sftl_test_expect_fill(test, data, 'Z');
	apple_sftl_free(cold);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static void sftl_test_gc_relocation_stops_before_erase(struct kunit *test)
{
	static const struct {
		enum sftl_test_commit commit;
		bool copy_error;
	} modes[] = {
		{ .copy_error = true },
		{ .commit = SFTL_COMMIT_IO_ERROR },
		{ .commit = SFTL_COMMIT_BAD_READBACK },
		{ .commit = SFTL_COMMIT_UNPUBLISHED },
	};
	unsigned int mode;

	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
		struct apple_sftl *sftl = NULL;
		u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
		unsigned int i, programs;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_NULL(test, data);
		/* Cold recovery must replay historical ages under the old checkpoint. */
		f->sequence_bias = 0x20000ULL - (TEST_APPEND_FIRST + 479 + 4 + 6);
		KUNIT_ASSERT_EQ(test, sftl_test_gc_live_medium(f, data), 0);
		f->ops.erase_block = sftl_test_erase_block;
		f->commit = modes[mode].commit;
		if (modes[mode].copy_error) {
			/* The second copied page lands, but completion reports a failure. */
			f->fail_program = f->programs + 2;
			f->uncertain_program = true;
		}
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		apple_sftl_set_checkpoint_ops(sftl, &sftl_test_gc_ops, f);
		KUNIT_EXPECT_LT(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), 0);
		KUNIT_EXPECT_EQ(test, f->installs, modes[mode].copy_error ? 0U : 1U);
		KUNIT_EXPECT_EQ(test, f->erases, 0U);
		for (i = 0; i < 128; i++)
			KUNIT_ASSERT_NOT_NULL(test, sftl_test_written(f, 0, 0, i));
		KUNIT_EXPECT_PTR_EQ(test, sftl_test_written(f, 1, 0, 2), NULL);
		programs = f->programs;
		KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5, data, 1), -EIO);
		KUNIT_EXPECT_EQ(test, f->programs, programs);
		apple_sftl_free(sftl);
		KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
		sftl_test_gc_relocated_data(test, sftl, data, 0);
		apple_sftl_free(sftl);
		if (modes[mode].copy_error) {
			struct sftl_test_page *page = sftl_test_written(f, 1, 0, 1);

			KUNIT_ASSERT_NOT_NULL(test, page);
			put_unaligned_le32(3, page->oob + 12);
			KUNIT_EXPECT_EQ(test, apple_sftl_load(&f->provider, &sftl), -EBADMSG);
			apple_sftl_free(sftl);
		}
		apple_sftl_checkpoint_free(f->checkpoint);
	}
}

static void sftl_test_gc_preserves_last_sequences(struct kunit *test)
{
	struct sftl_fixture *f = sftl_test_fixture(test, SFTL_NO_FAULT);
	struct apple_sftl *sftl = NULL;
	struct apple_sftl_info info;
	u8 *data = kunit_kmalloc(test, APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	const u64 limit = (1ULL << 48) - 1;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, data);
	/* Copy (8) and checkpoint (4) would exhaust the remaining sequence IDs. */
	f->sequence_bias = limit - (TEST_APPEND_FIRST + 479 + 4) - 12;
	KUNIT_ASSERT_EQ(test, sftl_test_gc_live_medium(f, data), 0);
	f->ops.erase_block = sftl_test_erase_block;
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_ASSERT_EQ(test, info.sequence, limit - 12);
	apple_sftl_set_checkpoint_ops(sftl, &sftl_test_gc_ops, f);
	memset(data, 'B', 4096);
	for (i = 0; i < 3; i++)
		KUNIT_ASSERT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 5 + i, data, 1), 0);
	KUNIT_EXPECT_EQ(test, apple_sftl_write(sftl, TEST_APPEND_BASE + 8, data, 1), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, f->erases, 0U);
	KUNIT_EXPECT_EQ(test, f->installs, 0U);
	KUNIT_EXPECT_EQ(test, f->programs, 131U);
	apple_sftl_free(sftl);
	KUNIT_ASSERT_EQ(test, apple_sftl_load(&f->provider, &sftl), 0);
	apple_sftl_get_info(sftl, &info);
	KUNIT_EXPECT_EQ(test, info.sequence, limit);
	sftl_test_gc_relocated_data(test, sftl, data, 'B');
	apple_sftl_free(sftl);
	apple_sftl_checkpoint_free(f->checkpoint);
}

static struct kunit_case apple_sftl_cases[] = {
	KUNIT_CASE(sftl_test_rotate_mounted),
	KUNIT_CASE(sftl_test_vfl_rotation_cuts),
	KUNIT_CASE(sftl_test_vfl_rejects_ambiguous_tables),
	KUNIT_CASE(sftl_test_native_blank_ownership),
	KUNIT_CASE(sftl_test_native_retired_ownership),
	KUNIT_CASE(sftl_test_gc_native_publication),
	KUNIT_CASE(sftl_test_gc_native_prefix_cuts),
	KUNIT_CASE(sftl_test_gc_native_policy_refuses),
	KUNIT_CASE(sftl_test_gc_retries_reservation),
	KUNIT_CASE(sftl_test_gc_preserves_last_sequences),
	KUNIT_CASE(sftl_test_gc_relocates_on_write),
	KUNIT_CASE(sftl_test_gc_relocation_stops_before_erase),
	KUNIT_CASE(sftl_test_gc_preserves_last_blank_capacity),
	KUNIT_CASE(sftl_test_gc_reuses_obsolete_block),
	KUNIT_CASE(sftl_test_gc_short_native_footer),
	KUNIT_CASE(sftl_test_gc_preserves_protected_blocks),
	KUNIT_CASE(sftl_test_gc_erase_failure),
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
	KUNIT_CASE(sftl_test_complete_medium),
	KUNIT_CASE(sftl_test_checkpoint_cold_roundtrip),
	KUNIT_CASE(sftl_test_context_power_cuts),
	KUNIT_CASE(sftl_test_context_stage_failures),
	KUNIT_CASE(sftl_test_checkpoint_stage_protects_tail),
	KUNIT_CASE(sftl_test_context_pins_fallback_blocks),
	KUNIT_CASE(sftl_test_context_torn_append_replay),
	KUNIT_CASE(sftl_test_reject_corrupt_generation),
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

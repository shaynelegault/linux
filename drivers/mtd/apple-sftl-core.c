// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple VFL/SwissPPNFTL recovery and append for the measured 64 GB PPN layout.
 *
 * Format and replay rules are ported from Orchard's P105 Python reference
 * (ftl_map.py, ftl_load.py, ftl_log.py and ftl_append.py). It was checked
 * against native iOS logical reads. This code contains no captured firmware
 * or device data. Unsupported formats fail before a block device is created.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "apple-sftl.h"
#include "apple-sftl-vfl.h"
#include "apple-sftl-reclaim.h"
#include "apple-sftl-account.h"

#define SFTL_LPAGES	15625016U
#define SFTL_DISK_PAGES	15625000U
#define SFTL_BLOCKS	2088U
#define SFTL_SUBBLOCKS	2086U
#define SFTL_LANES	16U
#define SFTL_STRIDE	8192U
#define SFTL_PROVIDER	(SFTL_SUBBLOCKS * SFTL_STRIDE)
#define SFTL_UNMAPPED	U32_MAX
#define SFTL_SEQUENCE	GENMASK_ULL(47, 0)
#define SFTL_MAX_CONTEXTS 16U
#define SFTL_META	16U
#define SFTL_CHUNK	4096U
#define SFTL_TRIM	0xffff0001U
/* cxtSaveSB serializes the role in byte zero of each 8-byte block entry. */
enum sftl_block_role {
	SFTL_CLEAN_FREE = 1,
	SFTL_CLOSED_DATA = 3,
	SFTL_OPEN_DATA = 4,
	SFTL_DIRTY_FREE = 9,
};

#define SFTL_FOOTER_CHUNKS 32U
#define SFTL_CLAIM_TIMEOUT (500U * HZ)
#define SFTL_FOOTER_BYTES (SFTL_FOOTER_CHUNKS * SFTL_CHUNK)
#define SFTL_GC_MAP_BYTES (8U * 1024 * 1024) /* per map, on the measured 512 MiB P105 */

struct sftl_run {
	struct rb_node node;
	u32 start, end, provider;
};

/* stamp packs the native 48-bit sequence with its flags and record type. */
struct sftl_record {
	u64 stamp;
	u32 key, address;
};

struct sftl_records {
	struct sftl_record *items;
	u32 count, capacity;
};

struct apple_sftl {
	struct apple_ppn *ppn;
	struct apple_sftl_vfl_source vfl_source;
	bool vfl_degraded, context_degraded, native_context;
	u8 roles[SFTL_SUBBLOCKS], block_flags[SFTL_SUBBLOCKS];
	struct rb_root runs;
	struct sftl_records context;
	struct apple_sftl_user_seq *closed_user_seq;
	u32 nr_runs;
	u16 lanes[SFTL_BLOCKS];
	DECLARE_BITMAP(reserved, SFTL_BLOCKS);
	DECLARE_BITMAP(vfl_reserved, SFTL_BLOCKS);
	DECLARE_BITMAP(unknown_lane_flags, SFTL_BLOCKS);
	DECLARE_BITMAP(checkpoint_used, SFTL_BLOCKS);
	DECLARE_BITMAP(old_closed, SFTL_BLOCKS);
	u64 checkpoint_start;
	u64 checkpoint_sequence, sequence, journal_sequence;
	DECLARE_BITMAP(candidates, SFTL_BLOCKS);
	DECLARE_BITMAP(fallback_occupied, SFTL_SUBBLOCKS);
	DECLARE_BITMAP(used, SFTL_BLOCKS);
	u32 next_candidate, append_block, append_position, append_capacity, spare;
	const struct apple_sftl_checkpoint_ops *checkpoint_ops;
	void *checkpoint_priv;
	u32 footer_runs;
	u64 append_base;
	u8 *footer;
	bool writer_prepared;
	u8 *page;
	u8 oob[APPLE_PPN_OOB_SIZE];
	struct mutex lock; /* serializes reads and the shared page buffer */
	bool failed;
};

static u64 sftl_sequence(const u8 *meta)
{
	return get_unaligned_le64(meta) >> 16;
}

static u64 sftl_stamp(const u8 *meta)
{
	return sftl_sequence(meta) | (u64)meta[1] << 48 | (u64)meta[0] << 56;
}

static unsigned int sftl_kind(const struct sftl_record *record)
{
	return record->stamp >> 56;
}

static unsigned int sftl_flags(const struct sftl_record *record)
{
	return (record->stamp >> 48) & 0xff;
}

static bool sftl_erased(const void *data, size_t size)
{
	return !memchr_inv(data, 0xff, size);
}

static int sftl_read_page(struct apple_sftl *sftl, unsigned int bus,
			  unsigned int cau, unsigned int block,
			  unsigned int page, bool context, u8 *data, u8 *oob)
{
	u8 status;
	int ret;

	ret = sftl->ppn->ops->read_page(sftl->ppn->priv, bus, cau, block,
					 page, context, data, oob, &status);
	if (ret)
		return ret;
	/* Refresh requires a writer. A reader must not hide that requirement. */
	if (status != 0x40 && status != 0x49)
		return -EIO;
	if (status == 0x49 && (!sftl_erased(data, APPLE_PPN_PAGE_SIZE) ||
			       !sftl_erased(oob, APPLE_PPN_OOB_SIZE)))
		return -EBADMSG;
	return 0;
}

static int sftl_translate(struct apple_sftl *sftl, u32 address,
			  unsigned int *bus, unsigned int *cau,
			  unsigned int *block, unsigned int *page,
			  unsigned int *chunk)
{
	u32 offset;
	unsigned int lane, wanted, count;
	u16 bitmap;

	if (address >= SFTL_PROVIDER)
		return -ERANGE;
	*chunk = address & 3;
	*block = address / SFTL_STRIDE;
	offset = (address % SFTL_STRIDE) / 4;
	bitmap = sftl->lanes[*block];
	count = hweight16(bitmap);
	if (!count || offset / count >= 128)
		return -ERANGE;
	*page = offset / count;
	wanted = offset % count;
	for (lane = 0; lane < SFTL_LANES; lane++) {
		if (!(bitmap & BIT(lane)))
			continue;
		if (!wanted--)
			break;
	}
	*bus = lane & 1;
	*cau = lane >> 1;
	return 0;
}

static int sftl_read_address(struct apple_sftl *sftl, u32 address,
			     unsigned int *chunk)
{
	unsigned int bus, cau, block, page;
	int ret;

	ret = sftl_translate(sftl, address, &bus, &cau, &block, &page, chunk);
	if (ret)
		return ret;
	return sftl_read_page(sftl, bus, cau, block, page, false,
			      sftl->page, sftl->oob);
}

static struct sftl_run *sftl_find(struct apple_sftl *sftl, u32 logical)
{
	struct rb_node *node = sftl->runs.rb_node;

	while (node) {
		struct sftl_run *run = rb_entry(node, struct sftl_run, node);

		if (logical < run->start)
			node = node->rb_left;
		else if (logical >= run->end)
			node = node->rb_right;
		else
			return run;
	}
	return NULL;
}

static void sftl_insert(struct apple_sftl *sftl, struct sftl_run *run)
{
	struct rb_node **link = &sftl->runs.rb_node, *parent = NULL;

	while (*link) {
		struct sftl_run *entry = rb_entry(*link, struct sftl_run, node);

		parent = *link;
		link = run->start < entry->start ? &parent->rb_left :
						  &parent->rb_right;
	}
	rb_link_node(&run->node, parent, link);
	rb_insert_color(&run->node, &sftl->runs);
	sftl->nr_runs++;
}

static int sftl_add_run(struct apple_sftl *sftl, u32 start, u32 end, u32 provider)
{
	struct sftl_run *run;

	if (start >= end || end > SFTL_LPAGES ||
	    (provider != SFTL_UNMAPPED &&
	     (provider >= SFTL_PROVIDER || end - start > SFTL_PROVIDER - provider)))
		return -EBADMSG;
	run = kmalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	run->start = start;
	run->end = end;
	run->provider = provider;
	sftl_insert(sftl, run);
	return 0;
}

static int sftl_split(struct apple_sftl *sftl, u32 point)
{
	struct sftl_run *left = sftl_find(sftl, point), *right;

	if (!left)
		return -EBADMSG;
	if (left->start == point)
		return 0;
	right = kmalloc(sizeof(*right), GFP_KERNEL);
	if (!right)
		return -ENOMEM;
	right->start = point;
	right->end = left->end;
	right->provider = left->provider == SFTL_UNMAPPED ? SFTL_UNMAPPED :
			  left->provider + point - left->start;
	left->end = point;
	sftl_insert(sftl, right);
	return 0;
}

static bool sftl_contiguous(struct sftl_run *left, struct sftl_run *right)
{
	return left->end == right->start &&
		((left->provider == SFTL_UNMAPPED && right->provider == SFTL_UNMAPPED) ||
		 (left->provider != SFTL_UNMAPPED && right->provider != SFTL_UNMAPPED &&
		  left->provider + left->end - left->start == right->provider));
}

static void sftl_merge(struct apple_sftl *sftl, struct sftl_run *run)
{
	struct rb_node *node = rb_prev(&run->node);
	struct sftl_run *neighbor;

	if (node) {
		neighbor = rb_entry(node, struct sftl_run, node);
		if (sftl_contiguous(neighbor, run)) {
			neighbor->end = run->end;
			rb_erase(&run->node, &sftl->runs);
			kfree(run);
			sftl->nr_runs--;
			run = neighbor;
		}
	}
	node = rb_next(&run->node);
	if (!node)
		return;
	neighbor = rb_entry(node, struct sftl_run, node);
	if (sftl_contiguous(run, neighbor)) {
		run->end = neighbor->end;
		rb_erase(node, &sftl->runs);
		kfree(neighbor);
		sftl->nr_runs--;
	}
}

static int sftl_replace(struct apple_sftl *sftl, u32 start, u32 length, u32 address)
{
	struct sftl_run *run, *new;
	struct rb_node *node, *next;
	u32 end;
	int ret;

	if (!length || start >= SFTL_LPAGES || length > SFTL_LPAGES - start ||
	    (address != SFTL_UNMAPPED &&
	     (address >= SFTL_PROVIDER || length > SFTL_PROVIDER - address)))
		return -EBADMSG;
	end = start + length;
	ret = sftl_split(sftl, start);
	if (ret)
		return ret;
	if (end < SFTL_LPAGES) {
		ret = sftl_split(sftl, end);
		if (ret)
			return ret;
	}
	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;
	run = sftl_find(sftl, start);
	for (node = &run->node; node; node = next) {
		run = rb_entry(node, struct sftl_run, node);
		if (run->start >= end)
			break;
		next = rb_next(node);
		rb_erase(node, &sftl->runs);
		kfree(run);
		sftl->nr_runs--;
	}
	new->start = start;
	new->end = end;
	new->provider = address;
	sftl_insert(sftl, new);
	sftl_merge(sftl, new);
	return 0;
}

static int sftl_record_add(struct sftl_records *records, u64 stamp,
			   u32 key, u32 address)
{
	struct sftl_record *new;
	u32 capacity;

	if (records->count == SFTL_PROVIDER)
		return -E2BIG;
	if (records->count == records->capacity) {
		capacity = min_t(u32, max(records->capacity * 2, 256U), SFTL_PROVIDER);
		new = kvrealloc(records->items, array_size(capacity, sizeof(*new)),
				GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		records->items = new;
		records->capacity = capacity;
	}
	records->items[records->count++] = (struct sftl_record) {
		.stamp = stamp, .key = key, .address = address,
	};
	return 0;
}

static int sftl_record_compare(const void *a, const void *b)
{
	u64 left = ((const struct sftl_record *)a)->stamp & SFTL_SEQUENCE;
	u64 right = ((const struct sftl_record *)b)->stamp & SFTL_SEQUENCE;

	return left < right ? -1 : left > right;
}

#define SFTL_VFL_PAGES 64
#define SFTL_VFL_COPIES 4
#define SFTL_VFL_HEADER 0xff

struct sftl_vfl_slot {
	u32 age;
	u8 tag, copies;
};

struct sftl_vfl_scan {
	struct sftl_vfl_slot slots[2][SFTL_VFL_PAGES];
	u8 sample[APPLE_PPN_PAGE_SIZE + APPLE_PPN_OOB_SIZE];
	u8 reference_data[APPLE_PPN_PAGE_SIZE];
};

static int sftl_vfl_members(struct apple_sftl *sftl)
{
	struct apple_sftl_vfl_member *members = sftl->vfl_source.members;
	unsigned int lane, i, j;
	u8 status;
	int ret;

	/* An interrupted erase can remove the usual lane-zero seed. Search only
	 * page zero in the measured bootstrap block for a surviving member list.
	 */
	for (lane = 0; lane < SFTL_LANES; lane++) {
		ret = sftl->ppn->ops->read_page(sftl->ppn->priv, lane & 1, lane >> 1,
						2087, 0, true, sftl->page, sftl->oob, &status);
		if (ret)
			return ret;
		/* Some nonmembers report 45 at the bootstrap coordinate. Once
		 * discovered, every actual member is reread with strict status.
		 */
		if (status == 0x45)
			continue;
		if (status != 0x40 && status != 0x49)
			return -EIO;
		if (status == 0x40 && get_unaligned_le32(sftl->oob) == 0x00ffff20)
			break;
		if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ||
		    !sftl_erased(sftl->oob, sizeof(sftl->oob)))
			return -EBADMSG;
	}
	if (lane == SFTL_LANES)
		return -EBADMSG;
	for (i = 0; i < 8; i++) {
		const u8 *row = sftl->page + 0x100 + 4 * i;

		members[i].block = get_unaligned_le16(row);
		members[i].lane = row[2];
		if (members[i].block < 2056 || members[i].block >= SFTL_BLOCKS ||
		    members[i].lane >= SFTL_LANES || row[3])
			return -EBADMSG;
		for (j = 0; j < i; j++)
			if (members[j].block == members[i].block && members[j].lane == members[i].lane)
				return -EBADMSG;
		__set_bit(members[i].block, sftl->vfl_reserved);
	}
	if (lane) {
		for (i = 0; i < 8; i++)
			if (members[i].lane == lane && members[i].block == 2087)
				return 0;
		return -EBADMSG;
	}
	return 0;
}

static int sftl_vfl_read(struct apple_sftl *sftl, unsigned int group, unsigned int copy,
			 unsigned int page)
{
	const struct apple_sftl_vfl_member *member = &sftl->vfl_source.members[group * 4 + copy];

	return sftl_read_page(sftl, member->lane & 1, member->lane >> 1, member->block,
			      page, true, sftl->page, sftl->oob);
}

static int sftl_vfl_read_slot(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			      unsigned int group, unsigned int page, struct sftl_vfl_slot *slot)
{
	const struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	unsigned int copy, i;
	int ret;

	for (copy = 0; copy < SFTL_VFL_COPIES; copy++) {
		ret = sftl_vfl_read(sftl, group, copy, page);
		if (ret)
			return ret;
		if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
			if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE))
				return -EBADMSG;
			continue;
		}
		if (slot->copies) {
			if (memcmp(scan->sample, sftl->page, APPLE_PPN_PAGE_SIZE) ||
			    memcmp(scan->sample + APPLE_PPN_PAGE_SIZE, sftl->oob,
				   sizeof(sftl->oob)))
				return -EBADMSG;
		} else {
			if (sftl->oob[0] != 0x20 || sftl->oob[3])
				return -EBADMSG;
			slot->tag = sftl->oob[1];
			slot->age = get_unaligned_le32(sftl->oob + 4);
			if (slot->age < BIT(31))
				return -EOPNOTSUPP;
			if (slot->tag == SFTL_VFL_HEADER) {
				if (sftl->oob[2] != 0xff)
					return -EBADMSG;
				for (i = 0; i < 8; i++) {
					const u8 *row = sftl->page + 0x100 + 4 * i;
					const struct apple_sftl_vfl_member *m = &source->members[i];

					if (get_unaligned_le16(row) != m->block ||
					    row[2] != m->lane || row[3])
						return -EBADMSG;
				}
			} else if (!page || slot->tag >= SFTL_LANES || sftl->oob[2]) {
				return -EBADMSG;
			}
			memcpy(scan->sample, sftl->page, APPLE_PPN_PAGE_SIZE);
			memcpy(scan->sample + APPLE_PPN_PAGE_SIZE, sftl->oob,
			       sizeof(sftl->oob));
		}
		slot->copies |= BIT(copy);
	}
	return 0;
}

/* Verify the replica set again when a checkpoint or table is consumed.
 * sample then contains the complete first programmed page and its tag.
 */
static int sftl_vfl_reread(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			   unsigned int group, unsigned int page)
{
	const struct sftl_vfl_slot *expected = &scan->slots[group][page];
	struct sftl_vfl_slot actual = {};
	int ret = sftl_vfl_read_slot(sftl, scan, group, page, &actual);

	if (ret)
		return ret;
	if (!actual.copies || actual.copies != expected->copies ||
	    actual.age != expected->age || actual.tag != expected->tag)
		return -EBADMSG;
	return 0;
}

static int sftl_vfl_index(struct apple_sftl *sftl, struct sftl_vfl_scan *scan)
{
	struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	unsigned int group, page;
	bool have_checkpoint = false;
	int ret;

	for (group = 0; group < 2; group++) {
		for (page = 0; page < SFTL_VFL_PAGES; page++) {
			struct sftl_vfl_slot *slot = &scan->slots[group][page];

			ret = sftl_vfl_read_slot(sftl, scan, group, page, slot);
			if (ret)
				return ret;
			if (!slot->copies)
				continue;
			if (!scan->slots[group][0].copies ||
			    (u64)slot->age + page != scan->slots[group][0].age)
				return -EBADMSG;
			if (group && slot->age <= scan->slots[0][0].age) {
				u32 other = scan->slots[0][0].age - slot->age;

				if (other < SFTL_VFL_PAGES && scan->slots[0][other].copies)
					return -EBADMSG;
			}
			if (slot->copies != 0xf) {
				sftl->vfl_degraded = true;
				continue;
			}
			if (slot->tag == SFTL_VFL_HEADER &&
			    (!have_checkpoint || slot->age < source->age)) {
				have_checkpoint = true;
				source->age = slot->age;
				source->group = group;
				source->page = page;
			}
		}
		if (scan->slots[group][0].copies != 0xf)
			sftl->vfl_degraded = true;
	}
	return have_checkpoint ? 0 : -EBADMSG;
}

static int sftl_vfl_load_refs(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			      unsigned int group, unsigned int page, u16 *refs)
{
	const struct sftl_vfl_slot *header = &scan->slots[group][page];
	unsigned int lane;
	int ret;

	ret = sftl_vfl_reread(sftl, scan, group, page);
	if (ret)
		return ret;
	/* The 13G36 loader compares FTLSuBlks (+12) with VFLSuBlks
	 * (+14). An older, smaller FTL is a different address space.
	 */
	if (get_unaligned_le16(scan->sample + 10) != 16 ||
	    get_unaligned_le16(scan->sample + 12) != SFTL_SUBBLOCKS ||
	    get_unaligned_le16(scan->sample + 14) != SFTL_SUBBLOCKS)
		return -EOPNOTSUPP;
	for (lane = 0; lane < SFTL_LANES; lane++) {
		const struct sftl_vfl_slot *table;
		u16 ref = get_unaligned_le16(scan->sample + 0x200 + 32 * lane);
		u16 row = ref & 0x7fff;

		if (row >= SFTL_VFL_PAGES)
			return -EBADMSG;
		table = &scan->slots[ref >> 15][row];
		/* A stale reference can point at the same lane in a later bank cycle. */
		if (table->copies != 0xf || table->tag != lane || table->age <= header->age)
			return -EBADMSG;
		refs[lane] = ref;
	}
	return 0;
}

static int sftl_vfl_same_table(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			       u16 ref, unsigned int group, unsigned int page)
{
	unsigned int block;
	int ret;

	ret = sftl_vfl_reread(sftl, scan, ref >> 15, ref & 0x7fff);
	if (ret)
		return ret;
	memcpy(scan->reference_data, scan->sample, APPLE_PPN_PAGE_SIZE);
	ret = sftl_vfl_reread(sftl, scan, group, page);
	if (ret)
		return ret;
	if (memcmp(scan->reference_data, scan->sample, 2 * SFTL_SUBBLOCKS) ||
	    memcmp(scan->reference_data + 2 * SFTL_BLOCKS, scan->sample + 2 * SFTL_BLOCKS,
		   APPLE_PPN_PAGE_SIZE - 2 * SFTL_BLOCKS))
		return -EBADMSG;
	/* Two trailing physical indices are outside every supported FTL
	 * address. Older inactive marker flags can differ during rotation.
	 */
	for (block = SFTL_SUBBLOCKS; block < SFTL_BLOCKS; block++) {
		u16 old = get_unaligned_le16(scan->reference_data + 2 * block);
		u16 new = get_unaligned_le16(scan->sample + 2 * block);

		if (old != new && !(old & new & BIT(15)))
			return -EBADMSG;
	}
	return 0;
}

/* During healthy rotation, unpublished table copies reproduce the source
 * payload. An updated table that the selected checkpoint does not name can
 * change ownership: a previous map alone is then insufficient for recovery.
 */
static int sftl_vfl_check_tail(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			       const u16 *refs)
{
	const struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	u16 other[SFTL_LANES];
	unsigned int group, page, lane;
	int ret;

	for (group = 0; group < 2; group++)
		for (page = 0; page < SFTL_VFL_PAGES; page++) {
			const struct sftl_vfl_slot *slot = &scan->slots[group][page];

			if (!slot->copies || slot->age > source->age ||
			    (group == source->group && page == source->page))
				continue;
			if (slot->age == source->age)
				return -EBADMSG;
			sftl->vfl_degraded = true;
			if (slot->tag != SFTL_VFL_HEADER) {
				ret = sftl_vfl_same_table(sftl, scan, refs[slot->tag], group, page);
				if (ret)
					return ret;
				continue;
			}
			ret = sftl_vfl_load_refs(sftl, scan, group, page, other);
			if (ret)
				return ret;
			for (lane = 0; lane < SFTL_LANES; lane++) {
				if (other[lane] == refs[lane])
					continue;
				ret = sftl_vfl_same_table(sftl, scan, refs[lane],
							  other[lane] >> 15, other[lane] & 0x7fff);
				if (ret)
					return ret;
			}
		}
	return 0;
}

static int sftl_vfl(struct apple_sftl *sftl)
{
	const struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	struct sftl_vfl_scan *scan;
	u16 refs[SFTL_LANES];
	unsigned int lane, block;
	int ret;

	ret = sftl_vfl_members(sftl);
	if (ret)
		return ret;
	scan = kvzalloc(sizeof(*scan), GFP_KERNEL);
	if (!scan)
		return -ENOMEM;
	ret = sftl_vfl_index(sftl, scan);
	if (ret)
		goto out;
	ret = sftl_vfl_load_refs(sftl, scan, source->group, source->page, refs);
	if (ret)
		goto out;
	ret = sftl_vfl_check_tail(sftl, scan, refs);
	if (ret)
		goto out;
	for (lane = 0; lane < SFTL_LANES; lane++) {
		u16 ref = refs[lane];

		if (!source->page && (ref >> 15) != source->group)
			sftl->vfl_degraded = true;
		ret = sftl_vfl_reread(sftl, scan, ref >> 15, ref & 0x7fff);
		if (ret)
			goto out;
		for (block = 0; block < SFTL_BLOCKS; block++) {
			u16 value = get_unaligned_le16(scan->sample + block * 2);

			if (!(value & BIT(15))) {
				sftl->lanes[block] |= BIT(lane);
				if (value)
					__set_bit(block, sftl->unknown_lane_flags);
			}
		}
	}
out:
	kvfree(scan);
	return ret;
}

int apple_sftl_vfl_source(struct apple_sftl *sftl, struct apple_sftl_vfl_source *out)
{
	int ret = 0;

	if (!sftl || !out)
		return -EINVAL;
	mutex_lock(&sftl->lock);
	if (sftl->vfl_degraded || sftl->context_degraded || sftl->failed)
		ret = -EROFS;
	else
		*out = sftl->vfl_source;
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_vfl_source);

/* Qualified SLC operations also exclude writes through raw MTD. The mount's
 * mutex keeps foreground writes, checkpoints and bank rotation serialized.
 */
int apple_sftl_rotate_vfl(struct apple_sftl *sftl, const struct apple_sftl_vfl_ops *ops,
			 void *priv)
{
	struct apple_sftl *cold;
	bool started = false;
	int ret;

	if (!sftl || !ops || !ops->erase || !ops->program)
		return -EOPNOTSUPP;
	cold = kvzalloc(sizeof(*cold), GFP_NOIO);
	if (!cold)
		return -ENOMEM;
	mutex_lock(&sftl->lock);
	if (sftl->failed) {
		ret = -EIO;
		goto out;
	}
	if (sftl->vfl_degraded || sftl->context_degraded) {
		ret = -EROFS;
		goto out;
	}
	ret = apple_sftl_vfl_rotate_checked(sftl->ppn, &sftl->vfl_source, ops, priv, &started);
	if (ret)
		goto out;
	cold->ppn = sftl->ppn;
	cold->page = sftl->page;
	ret = sftl_vfl(cold);
	if (ret)
		goto out;
	if (cold->vfl_degraded || cold->vfl_source.group == sftl->vfl_source.group ||
	    cold->vfl_source.page != SFTL_LANES + 1 ||
	    cold->vfl_source.age >= sftl->vfl_source.age ||
	    memcmp(cold->vfl_source.members, sftl->vfl_source.members, sizeof(sftl->vfl_source.members)) ||
	    memcmp(cold->lanes, sftl->lanes, sizeof(sftl->lanes)) ||
	    memcmp(cold->vfl_reserved, sftl->vfl_reserved, sizeof(sftl->vfl_reserved)) ||
	    memcmp(cold->unknown_lane_flags, sftl->unknown_lane_flags, sizeof(sftl->unknown_lane_flags))) {
		ret = -EBADMSG;
		goto out;
	}
	sftl->vfl_source = cold->vfl_source;
out:
	if (ret && started)
		sftl->failed = true;
	mutex_unlock(&sftl->lock);
	kvfree(cold);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_rotate_vfl);


static int sftl_context_names(struct apple_sftl *sftl, const u8 *payload,
			      u16 *pending, u32 *count)
{
	u32 names = get_unaligned_le32(payload), i;

	if (!names || names > SFTL_MAX_CONTEXTS)
		return -EBADMSG;
	for (i = 0; i < names; i++) {
		u32 name = get_unaligned_le32(payload + 4 + i * 4);

		if (!name || name >= SFTL_SUBBLOCKS)
			return -EBADMSG;
		if (test_bit(name, sftl->reserved))
			continue;
		if (*count == SFTL_MAX_CONTEXTS)
			return -E2BIG;
		__set_bit(name, sftl->reserved);
		pending[(*count)++] = name;
	}
	return 0;
}

static void sftl_clear_runs(struct apple_sftl *sftl)
{
	struct rb_node *node;

	while ((node = rb_first(&sftl->runs))) {
		rb_erase(node, &sftl->runs);
		kfree(rb_entry(node, struct sftl_run, node));
	}
	sftl->nr_runs = 0;
}

static int sftl_load_ranges(struct apple_sftl *sftl, const u8 *payload, u32 *cursor,
			    bool *short_range)
{
	u32 value = get_unaligned_le32(payload + 4), offset;

	if (value == U32_MAX)
		return *cursor == SFTL_LPAGES && get_unaligned_le32(payload) == U32_MAX ?
			0 : -EBADMSG;
	if (value != 0xfffffff0 || get_unaligned_le32(payload) != *cursor ||
	    *cursor == SFTL_LPAGES)
		return -EBADMSG;

	for (offset = 8; offset < SFTL_CHUNK; offset += 8) {
		u32 length = get_unaligned_le32(payload + offset + 4);
		int ret;

		value = get_unaligned_le32(payload + offset);
		if (value == U32_MAX)
			break;
		if (!length || length > SFTL_LPAGES - *cursor)
			return -EBADMSG;
		if (value == BIT(fls(SFTL_PROVIDER * 2 - 1) - 1) - 1)
			value = SFTL_UNMAPPED;
		ret = sftl_add_run(sftl, *cursor, *cursor + length, value);
		if (ret)
			return ret;
		*cursor += length;
	}
	/* A save interrupted between programs can end only after complete
	 * 511-pair fragments until the whole logical address space is covered.
	 */
	if (offset != SFTL_CHUNK && *cursor < SFTL_LPAGES)
		*short_range = true;
	return offset == 8 ? -EBADMSG : 0;
}

/* Native initial allocation takes an erased free block. A blank head alone
 * can also describe an unnamed context spare, a reserved block or a stream.
 * An older saved role can veto a new allocation while that fallback exists.
 */
static void sftl_load_block_roles(struct apple_sftl *sftl, const u8 *meta, const u8 *payload,
				  bool selected)
{
	u32 block = get_unaligned_le16(meta + 8), count = get_unaligned_le16(meta + 10);

	while (count--) {
		u8 role = payload[0];
		u16 live = get_unaligned_le16(payload + 2);

		if (selected) {
			if ((role == SFTL_CLEAN_FREE || role == SFTL_DIRTY_FREE) && live)
				role = 0;
			sftl->roles[block] = role;
			sftl->block_flags[block] = payload[1];
		} else if (role != SFTL_CLEAN_FREE || live) {
			__set_bit(block, sftl->fallback_occupied);
		}
		block++;
		payload += 8;
	}
}

/* -ENODATA distinguishes a valid prefix from malformed records or a read error. */
static int sftl_decode_generation(struct apple_sftl *sftl, const struct sftl_records *records,
				  u32 first, u32 end, unsigned long *named)
{
	u32 cursor = 0, map_records = 0, i, previous_order = 0;
	u64 first_sequence = records->items[first].stamp & SFTL_SEQUENCE;
	bool base_tags = true, native = false, short_range = false;

	for (i = first; i < end; i++) {
		const struct sftl_record *record = &records->items[i];
		u32 index = i - first, subtype = sftl_flags(record), chunk;
		u32 order = subtype == 4 ? 7 : subtype;
		const u8 *meta, *payload;
		int ret;

		if (!first_sequence || (record->stamp & SFTL_SEQUENCE) != first_sequence + index ||
		    order < previous_order || (record->address & 3) != (index & 3))
			return -EBADMSG;
		previous_order = order;
		ret = sftl_read_address(sftl, record->address, &chunk);
		if (ret)
			return ret;
		meta = sftl->oob + chunk * SFTL_META;
		payload = sftl->page + chunk * SFTL_CHUNK;
		if (sftl_stamp(meta) != record->stamp)
			return -EBADMSG;
		if (!index) {
			u32 names = get_unaligned_le32(payload), k;

			if (subtype != 1 || !names || names > SFTL_MAX_CONTEXTS)
				return -EBADMSG;
			for (k = 0; k < names; k++) {
				u32 block = get_unaligned_le32(payload + 4 + k * 4);

				if (!block || block >= SFTL_SUBBLOCKS ||
				    !test_bit(block, sftl->reserved) || test_bit(block, named))
					return -EBADMSG;
				__set_bit(block, named);
			}
		}
		if (!test_bit(record->address / SFTL_STRIDE, named))
			return -EBADMSG;
		if (index < 4 && subtype == 1) {
			base_tags &= apple_sftl_native_context_tag(index, meta);
			if (index == 3) {
				if (!base_tags ||
				    get_unaligned_le32(payload + SFTL_CHUNK - 4) != SFTL_SUBBLOCKS)
					return -EBADMSG;
				native = true;
			}
		}
		if (native && !apple_sftl_native_context_tag(index, meta))
			return -EBADMSG;
		if (native && subtype == 3)
			sftl_load_block_roles(sftl, meta, payload, true);
		if (subtype == 4) {
			ret = sftl_load_ranges(sftl, payload, &cursor, &short_range);
			if (ret)
				return ret;
			map_records++;
		}
	}

	/* cxtNextBatch pads the measured native map in groups of 64. Coverage
	 * alone can become complete many pages before the final batch does.
	 */
	if (cursor != SFTL_LPAGES || !map_records ||
	    (native && map_records % APPLE_SFTL_NATIVE_MAP_BATCH)) {
		if (short_range)
			return -EBADMSG;
		/* A partial program or a native aggregate that lost its tail does
		 * not establish the next safe write sequence. Recover for reads.
		 */
		if (native || (records->items[end - 1].address & 3) != 3)
			sftl->context_degraded = true;
		return -ENODATA;
	}
	sftl->native_context = native;
	return 0;
}

static int sftl_context(struct apple_sftl *sftl, const u8 *heads, struct sftl_records *journal)
{
	struct sftl_records records = {};
	DECLARE_BITMAP(gapped, SFTL_SUBBLOCKS) = {};
	DECLARE_BITMAP(named, SFTL_SUBBLOCKS);
	u16 pending[SFTL_MAX_CONTEXTS];
	u32 count = 0, block, at, i, first = 0, end, chunk, oldest;
	int ret = -EBADMSG;

	for (block = 1; block < SFTL_SUBBLOCKS; block++) {
		if (heads[block * SFTL_META] != 0x1f)
			continue;
		if (count == SFTL_MAX_CONTEXTS)
			goto out;
		pending[count++] = block;
		__set_bit(block, sftl->reserved);
	}
	for (at = 0; at < count; at++) {
		u16 blank_lanes = 0, populated = 0;
		u32 position, pages, lanes;
		bool gap = false;

		block = pending[at];
		lanes = hweight16(sftl->lanes[block]);
		pages = lanes * 128;
		if (!pages) {
			ret = -EBADMSG;
			goto out;
		}
		for (position = 0; position < pages; position++) {
			u16 lane = BIT(position % lanes);
			bool empty = false;

			ret = sftl_read_address(sftl, block * SFTL_STRIDE + position * 4, &chunk);
			if (ret)
				goto out;
			if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
				ret = sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ? 0 : -EBADMSG;
				if (ret)
					goto out;
				gap = true;
				blank_lanes |= lane;
				continue;
			}
			if (blank_lanes & lane) {
				ret = -EBADMSG;
				goto out;
			}
			populated |= lane;
			if (gap)
				__set_bit(block, gapped);
			for (chunk = 0; chunk < 4; chunk++) {
				const u8 *meta = sftl->oob + chunk * SFTL_META;
				const u8 *payload = sftl->page + chunk * SFTL_CHUNK;

				if (sftl_erased(meta, SFTL_META)) {
					if (!sftl_erased(payload, SFTL_CHUNK)) {
						ret = -EBADMSG;
						goto out;
					}
					empty = true;
					continue;
				}
				if (empty || meta[0] != 0x1f || !meta[1] || meta[1] > 6) {
					ret = -EBADMSG;
					goto out;
				}
				if (meta[1] == 1 && !chunk) {
					ret = sftl_context_names(sftl, payload, pending, &count);
					if (ret)
						goto out;
				}
				ret = sftl_record_add(&records, sftl_stamp(meta), 0,
						      block * SFTL_STRIDE + position * 4 + chunk);
				if (ret)
					goto out;
			}
		}
		if (test_bit(block, gapped) && hweight16(populated) == lanes) {
			ret = -EBADMSG;
			goto out;
		}
	}
	ret = -EBADMSG;
	if (!records.count)
		goto out;
	sort(records.items, records.count, sizeof(*records.items), sftl_record_compare, NULL);
	/* An interrupted lane-by-lane retirement can leave older fragments
	 * without their base. They cannot provide a recovery root.
	 */
	for (oldest = 0; oldest < records.count; oldest++) {
		const struct sftl_record *r = &records.items[oldest];

		if (sftl_flags(r) == 1 && !(r->address & 3))
			break;
		if (!test_bit(r->address / SFTL_STRIDE, gapped))
			goto out;
	}
	if (oldest == records.count)
		goto out;
	for (i = 1; i < records.count; i++)
		if (!sftl_record_compare(&records.items[i - 1], &records.items[i]))
			goto out;
	for (end = records.count; end > oldest; end = first) {
		first = end - 1;
		while (first > oldest && (sftl_flags(&records.items[first]) != 1 ||
				 (records.items[first].address & 3)))
			first--;
		memset(named, 0, sizeof(named));
		ret = sftl_decode_generation(sftl, &records, first, end, named);
		if (!ret)
			break;
		if (ret != -ENODATA)
			goto out;
		sftl_clear_runs(sftl);
	}
	if (end == oldest) {
		ret = -EBADMSG;
		goto out;
	}
	/* Retired lanes may be wholly erased while old lanes still hold
	 * verified context records. Require a newer intact generation which
	 * names that site, and keep the writer off until reconciliation.
	 */
	for (i = 0; i < records.count; i++) {
		block = records.items[i].address / SFTL_STRIDE;
		if (test_bit(block, gapped)) {
			if (i >= first || !test_bit(block, named)) {
				ret = -EBADMSG;
				goto out;
			}
			sftl->context_degraded = true;
		}
	}
	/* Only the selected generation can establish a data role. Canonical
	 * block tables from an older fallback can still veto initial allocation.
	 */
	for (i = 0; sftl->native_context && !sftl->context_degraded && i < first; i++) {
		const struct sftl_record *r = &records.items[i];
		const u8 *meta;
		u32 start;

		if (sftl_flags(r) != 3)
			continue;
		ret = sftl_read_address(sftl, r->address, &chunk);
		if (ret)
			goto out;
		meta = sftl->oob + chunk * SFTL_META;
		if (sftl_stamp(meta) != r->stamp) {
			ret = -EBADMSG;
			goto out;
		}
		start = get_unaligned_le16(meta + 8);
		if (start < SFTL_SUBBLOCKS && !(start % (SFTL_CHUNK / 8)) &&
		    apple_sftl_native_context_tag(8 + start / (SFTL_CHUNK / 8), meta))
			sftl_load_block_roles(sftl, meta, sftl->page + chunk * SFTL_CHUNK, false);
	}
	/* Retain the selected generation's opaque records for later snapshots. */
	for (i = first; i < end; i++) {
		const struct sftl_record *record = &records.items[i];

		if (sftl_flags(record) == 4)
			continue;
		ret = sftl_record_add(&sftl->context, record->stamp, record->key, record->address);
		if (ret)
			goto out;
	}
	/* Keep every possible fallback out of the erase path. */
	if (!first && end == records.count)
		sftl->checkpoint_start = records.items[0].stamp & SFTL_SEQUENCE;
	sftl->checkpoint_sequence = records.items[end - 1].stamp & SFTL_SEQUENCE;
	sftl->sequence = sftl->checkpoint_sequence;
	if (end < records.count) {
		/* Account for observed context sequences in normal journal order.
		 * Only their own IDs may bridge a gap between two data records.
		 */
		records.count -= end;
		memmove(records.items, records.items + end, records.count * sizeof(*records.items));
		*journal = records;
		records.items = NULL;
	}
	ret = 0;
out:
	kvfree(records.items);
	return ret;
}

static bool sftl_user_epoch_valid(const u8 *meta, u64 sequence)
{
	u32 user_epoch = get_unaligned_le32(meta + 12);

	/* Relocation restamps the weave sequence and retains the original user epoch. */
	if (meta[0] == 1 && !meta[1] && get_unaligned_le32(meta + 8) < SFTL_LPAGES)
		return user_epoch <= sequence >> 16;
	return user_epoch == sequence >> 16;
}

static int sftl_add_journal(struct sftl_records *journal, const u8 *meta,
			    u32 address, u64 checkpoint)
{
	u64 sequence = sftl_sequence(meta);

	if (sequence <= checkpoint || sftl_erased(meta, SFTL_META))
		return 0;
	if ((meta[0] != 1 && meta[0] != 2) ||
	    !sftl_user_epoch_valid(meta, sequence) ||
	    (meta[0] == 1 ? meta[1] > 1 : meta[1] != 0))
		return -EBADMSG;
	return sftl_record_add(journal, sftl_stamp(meta), get_unaligned_le32(meta + 8), address);
}

static int sftl_remember_user_seq(struct apple_sftl *sftl, u32 block, const u8 *footer, u32 runs)
{
	/* Four base chunks identify the layout validated by sftl_decode_generation. */
	if (sftl->context.count != APPLE_SFTL_ACCOUNT_RECORDS ||
	    sftl_flags(&sftl->context.items[3]) != 1)
		return 0;
	if (!sftl->closed_user_seq) {
		sftl->closed_user_seq = kvzalloc(SFTL_SUBBLOCKS * sizeof(*sftl->closed_user_seq),
						  GFP_NOIO);
		if (!sftl->closed_user_seq)
			return -ENOMEM;
	}
	apple_sftl_summarize_user_seq(&sftl->closed_user_seq[block], footer, runs);
	return 0;
}

static int sftl_footer(struct apple_sftl *sftl, const u8 *head, u32 block,
		       u32 pages, struct sftl_records *journal, u64 *floor)
{
	u8 footer_meta[16];
	u8 *footer;
	u32 chunks, span, index, position = 0, relative_end = 0;
	u64 base;
	unsigned int chunk;
	bool terminated = false;
	int ret;

	memcpy(footer_meta, sftl->oob, sizeof(footer_meta));
	base = sftl_sequence(footer_meta);
	chunks = get_unaligned_le16(footer_meta + 12);
	span = get_unaligned_le32(footer_meta + 8);
	if (chunks < 4 || chunks > 32 || chunks % 4 || footer_meta[1] >> 1 != chunks ||
	    get_unaligned_le16(footer_meta + 14) != 0xffff ||
	    base != sftl_sequence(head) || !span || base + span > SFTL_SEQUENCE + 1)
		return -EBADMSG;
	*floor = max(*floor, base + span - 1);
	if (base + span - 1 <= sftl->checkpoint_sequence) {
		if (head[0] == 1 && base && base + span - 1 < sftl->checkpoint_start)
			__set_bit(block, sftl->old_closed);
		return 0;
	}
	footer = kmalloc(chunks * SFTL_CHUNK, GFP_KERNEL);
	if (!footer)
		return -ENOMEM;
	for (index = 0; index < chunks / 4; index++) {
		ret = sftl_read_address(sftl, block * SFTL_STRIDE +
					(pages - chunks / 4 + index) * 4, &chunk);
		if (ret)
			goto out;
		for (chunk = 0; chunk < 4; chunk++)
			if (memcmp(footer_meta, sftl->oob + chunk * SFTL_META, SFTL_META)) {
				ret = -EBADMSG;
				goto out;
			}
		memcpy(footer + index * APPLE_PPN_PAGE_SIZE, sftl->page, APPLE_PPN_PAGE_SIZE);
	}
	for (index = 0; index < chunks * SFTL_CHUNK; index += 16) {
		u32 relative = get_unaligned_le32(footer + index);
		u32 key = get_unaligned_le32(footer + index + 8);
		u32 length = get_unaligned_le32(footer + index + 12), offset;

		if (!length) {
			terminated = true;
			break;
		}
		if (relative < relative_end || length > U32_MAX - relative ||
		    base + relative + length > SFTL_SEQUENCE + 1 ||
		    length > pages * 4 - chunks - position) {
			ret = -EBADMSG;
			goto out;
		}
		relative_end = relative + length;
		for (offset = 0; offset < length; offset++, position++) {
			u64 sequence = base + relative + offset;
			u32 actual_key = key >= 0xffff0000 ? key : key + offset;
			u32 address = block * SFTL_STRIDE + position;
			u8 meta[16];

			if (sequence <= sftl->checkpoint_sequence)
				continue;
			if (key < 0xffff0000 && actual_key < key) {
				ret = -EBADMSG;
				goto out;
			}
			if (actual_key == SFTL_TRIM) {
				ret = sftl_read_address(sftl, address, &chunk);
				if (ret)
					goto out;
				memcpy(meta, sftl->oob + chunk * SFTL_META, sizeof(meta));
				if (meta[0] != 2 || meta[1] || sftl_sequence(meta) != sequence ||
				    get_unaligned_le32(meta + 8) != SFTL_TRIM) {
					ret = -EBADMSG;
					goto out;
				}
			} else {
				meta[0] = 1;
				meta[1] = head[1];
				put_unaligned_le64(sequence << 16 | get_unaligned_le16(meta), meta);
				put_unaligned_le32(actual_key, meta + 8);
				if (!meta[1] && actual_key < SFTL_LPAGES)
					memcpy(meta + 12, footer + index + 4, sizeof(u32));
				else
					put_unaligned_le32(sequence >> 16, meta + 12);
			}
			ret = sftl_add_journal(journal, meta, address, sftl->checkpoint_sequence);
			if (ret)
				goto out;
		}
	}
	if (terminated && position == pages * 4 - chunks)
		ret = sftl_remember_user_seq(sftl, block, footer, index / SFTL_META);
	else
		ret = -EBADMSG;
out:
	kfree(footer);
	return ret;
}

static bool sftl_partial_footer(struct apple_sftl *sftl, const u8 *head,
				u32 position, u32 pages, u64 span)
{
	const u8 *meta = sftl->oob;
	unsigned int chunk;

	if (position < pages - SFTL_FOOTER_CHUNKS / 4 ||
	    meta[0] != 0x1c || meta[1] != SFTL_FOOTER_CHUNKS * 2 ||
	    sftl_sequence(meta) != sftl_sequence(head) ||
	    get_unaligned_le32(meta + 8) != span ||
	    get_unaligned_le16(meta + 12) != SFTL_FOOTER_CHUNKS ||
	    get_unaligned_le16(meta + 14) != 0xffff)
		return false;
	for (chunk = 1; chunk < 4; chunk++)
		if (memcmp(meta, sftl->oob + chunk * SFTL_META, SFTL_META))
			return false;
	return true;
}

/* A Linux block may close across an observed checkpoint save. For a torn
 * suffix, admit only the exact sequence IDs actually read from context OOB.
 * The leading portion of journal is sorted until all data has been gathered.
 */
static bool sftl_context_gap(const struct apple_sftl *sftl, const struct sftl_records *journal,
			     u32 nr_context, u64 first, u64 next)
{
	u32 low = 0, high = nr_context;
	u64 count = next - first;

	if (first == (sftl->context.items[0].stamp & SFTL_SEQUENCE) &&
	    next == sftl->checkpoint_sequence + 1)
		return true;
	if (count > nr_context)
		return false;
	while (low < high) {
		u32 mid = low + (high - low) / 2;

		if ((journal->items[mid].stamp & SFTL_SEQUENCE) < first)
			low = mid + 1;
		else
			high = mid;
	}
	return count && count <= nr_context - low &&
		(journal->items[low].stamp & SFTL_SEQUENCE) == first &&
		(journal->items[low + count - 1].stamp & SFTL_SEQUENCE) == next - 1;
}

static int sftl_journal(struct apple_sftl *sftl, const u8 *heads, struct sftl_records *journal)
{
	u64 floor = sftl->checkpoint_sequence, expected = floor + 1;
	u32 nr_context = journal->count;
	u32 block, position, i, pages;
	unsigned int chunk;
	int ret;

	for (block = 1; block < SFTL_SUBBLOCKS; block++) {
		const u8 *head = heads + block * SFTL_META;
		bool complete_prefix = true, partial_footer = false;
		u64 base = sftl_sequence(head), next = base;

		if (head[0] != 1 && head[0] != 2)
			continue;
		pages = hweight16(sftl->lanes[block]) * 128;
		ret = sftl_read_address(sftl, block * SFTL_STRIDE + (pages - 1) * 4, &chunk);
		if (ret)
			goto out;
		if (sftl->oob[0] == 0x1c) {
			ret = sftl_footer(sftl, head, block, pages, journal, &floor);
			if (ret)
				goto out;
			continue;
		}
		if (!sftl_erased(sftl->oob, sizeof(sftl->oob)) ||
		    !sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE)) {
			ret = -EBADMSG;
			goto out;
		}
		for (position = 0; position < pages; position++) {
			ret = sftl_read_address(sftl, block * SFTL_STRIDE + position * 4, &chunk);
			if (ret)
				goto out;
			if (sftl_erased(sftl->oob, sizeof(sftl->oob)) &&
			    !sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE)) {
				ret = -EBADMSG;
				goto out;
			}
			if (position < pages - SFTL_FOOTER_CHUNKS / 4) {
				for (chunk = 0; chunk < 4; chunk++) {
					const u8 *meta = sftl->oob + chunk * SFTL_META;
					u64 sequence = sftl_sequence(meta);

					if (meta[0] != 1 || meta[1] || sequence < next ||
					    !sftl_user_epoch_valid(meta, sequence))
						complete_prefix = false;
					/* A checkpoint occupies its own sequence numbers between pages. */
					if (complete_prefix && sequence > next &&
					    (!position || chunk || !sftl_context_gap(sftl, journal,
										   nr_context, next, sequence)))
						complete_prefix = false;
					next = sequence + 1;
				}
			}
			/* A closing footer may follow a checkpoint that consumed
			 * sequences between two data pages in the same block. Match
			 * its span to the observed data, including that interval.
			 */
			if (sftl->oob[0] == 0x1c) {
				if (!complete_prefix ||
				    !sftl_partial_footer(sftl, head, position, pages,
							 next - base)) {
					ret = -EBADMSG;
					goto out;
				}
				partial_footer = true;
				continue;
			}
			if (partial_footer && !sftl_erased(sftl->oob, sizeof(sftl->oob))) {
				ret = -EBADMSG;
				goto out;
			}
			for (chunk = 0; chunk < 4; chunk++) {
				ret = sftl_add_journal(journal, sftl->oob + chunk * SFTL_META,
						       block * SFTL_STRIDE + position * 4 + chunk,
						       sftl->checkpoint_sequence);
				if (ret)
					goto out;
			}
		}
	}
	sort(journal->items, journal->count, sizeof(*journal->items), sftl_record_compare, NULL);
	for (i = 0; i < journal->count; i++) {
		const struct sftl_record *record = &journal->items[i];
		u64 sequence = record->stamp & SFTL_SEQUENCE;

		ret = -EBADMSG;
		if (sequence != expected++)
			goto out;
		if (sftl_kind(record) == 0x1f)
			continue;
		if (sftl_kind(record) == 2 && record->key == SFTL_TRIM) {
			const u8 *payload;
			u32 count, offset;

			ret = sftl_read_address(sftl, record->address, &chunk);
			if (ret)
				goto out;
			ret = -EBADMSG;
			if (sftl_stamp(sftl->oob + chunk * SFTL_META) != record->stamp ||
			    get_unaligned_le32(sftl->oob + chunk * SFTL_META + 8) != record->key)
				goto out;
			payload = sftl->page + chunk * SFTL_CHUNK;
			count = get_unaligned_le32(payload);
			if (count > (SFTL_CHUNK - 4) / 8)
				goto out;
			for (offset = 0; offset < count; offset++) {
				u32 start = get_unaligned_le32(payload + 4 + offset * 8);
				u32 length = get_unaligned_le32(payload + 8 + offset * 8);

				if (!length)
					continue;
				ret = sftl_replace(sftl, start, length, SFTL_UNMAPPED);
				if (ret)
					goto out;
			}
		} else {
			ret = sftl_replace(sftl, record->key, 1, record->address);
			if (ret)
				goto out;
		}
		sftl->sequence = sequence;
	}
	sftl->journal_sequence = expected - 1;
	sftl->sequence = max(sftl->journal_sequence, floor);
	ret = 0;
out:
	return ret;
}

static void sftl_mark_mapped(struct apple_sftl *sftl, unsigned long *blocks)
{
	struct rb_node *node;
	u32 block;

	for (node = rb_first(&sftl->runs); node; node = rb_next(node)) {
		struct sftl_run *run = rb_entry(node, struct sftl_run, node);
		u32 end;

		if (run->provider == SFTL_UNMAPPED)
			continue;
		end = (run->provider + run->end - run->start - 1) / SFTL_STRIDE;
		for (block = run->provider / SFTL_STRIDE; block <= end; block++)
			__set_bit(block, blocks);
	}
}

int apple_sftl_load(struct apple_ppn *provider, struct apple_sftl **out)
{
	struct apple_sftl *sftl;
	struct sftl_records journal = {};
	u8 *heads;
	u32 block;
	unsigned int chunk;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!provider || !provider->ops || !provider->ops->read_page ||
	    provider->buses != 2 || provider->caus != 8 ||
	    provider->blocks != SFTL_BLOCKS || provider->pages != 128)
		return -EOPNOTSUPP;
	sftl = kzalloc(sizeof(*sftl), GFP_KERNEL);
	if (!sftl)
		return -ENOMEM;
	sftl->ppn = provider;
	sftl->runs = RB_ROOT;
	sftl->append_block = U32_MAX;
	sftl->next_candidate = 1;
	sftl->spare = U32_MAX;
	mutex_init(&sftl->lock);
	sftl->page = kmalloc(APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	heads = kcalloc(SFTL_BLOCKS, SFTL_META, GFP_KERNEL);
	if (!sftl->page || !heads) {
		ret = -ENOMEM;
		goto failed;
	}
	memset(heads, 0xff, SFTL_BLOCKS * SFTL_META);
	ret = sftl_vfl(sftl);
	if (ret)
		goto failed;
	for (block = 1; block < SFTL_SUBBLOCKS; block++) {
		if (!sftl->lanes[block])
			continue;
		ret = sftl_read_address(sftl, block * SFTL_STRIDE, &chunk);
		if (ret)
			goto failed;
		if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
			if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE)) {
				ret = -EBADMSG;
				goto failed;
			}
			__set_bit(block, sftl->candidates);
		} else if (sftl->oob[0] == 0x1f || sftl->oob[0] == 1 || sftl->oob[0] == 2) {
			memcpy(heads + block * SFTL_META, sftl->oob, SFTL_META);
		} else {
			ret = -EOPNOTSUPP;
			goto failed;
		}
	}
	ret = sftl_context(sftl, heads, &journal);
	if (ret)
		goto failed;
	/* A map entry superseded by journal replay still belongs to the saved map. */
	sftl_mark_mapped(sftl, sftl->checkpoint_used);
	ret = sftl_journal(sftl, heads, &journal);
	if (ret)
		goto failed;
	kvfree(journal.items);
	kfree(heads);
	*out = sftl;
	return 0;
failed:
	kvfree(journal.items);
	kfree(heads);
	apple_sftl_free(sftl);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_load);

void apple_sftl_free(struct apple_sftl *sftl)
{
	if (!sftl)
		return;
	sftl_clear_runs(sftl);
	kvfree(sftl->footer);
	kvfree(sftl->context.items);
	kvfree(sftl->closed_user_seq);
	kfree(sftl->page);
	kfree(sftl);
}
EXPORT_SYMBOL_GPL(apple_sftl_free);

int apple_sftl_read(struct apple_sftl *sftl, u32 logical_page, u8 *data)
{
	struct sftl_run *run;
	unsigned int chunk;
	u32 address;
	const u8 *meta;
	int ret = 0;

	if (!sftl || !data || logical_page >= SFTL_LPAGES)
		return -EINVAL;
	mutex_lock(&sftl->lock);
	if (sftl->failed) {
		ret = -EIO;
		goto out;
	}
	run = sftl_find(sftl, logical_page);
	if (!run) {
		ret = -EBADMSG;
		goto failed;
	}
	if (run->provider == SFTL_UNMAPPED) {
		memset(data, 0, SFTL_CHUNK);
		goto out;
	}
	address = run->provider + logical_page - run->start;
	ret = sftl_read_address(sftl, address, &chunk);
	if (ret)
		goto failed;
	meta = sftl->oob + chunk * SFTL_META;
	if ((meta[0] != 1 && meta[0] != 2) || get_unaligned_le32(meta + 8) != logical_page) {
		ret = -EBADMSG;
		goto failed;
	}
	memcpy(data, sftl->page + chunk * SFTL_CHUNK, SFTL_CHUNK);
	goto out;
failed:
	sftl->failed = true;
out:
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_read);

/* Caller holds sftl->lock. Every candidate is checked in full before use. */
static int sftl_writer_prepare(struct apple_sftl *sftl)
{
	u32 block;

	if (sftl->vfl_degraded || sftl->context_degraded)
		return -EROFS;
	if (sftl->writer_prepared)
		return 0;
	if (sftl->journal_sequence != sftl->sequence)
		return -EOPNOTSUPP;
	if (!sftl->ppn->ops->program_page)
		return -EROFS;
	sftl->footer = kvzalloc(SFTL_FOOTER_BYTES, GFP_KERNEL);
	if (!sftl->footer)
		return -ENOMEM;
	for (block = 0; block < SFTL_BLOCKS; block++)
		if (test_bit(block, sftl->reserved) || test_bit(block, sftl->vfl_reserved))
			__set_bit(block, sftl->used);
	__set_bit(0, sftl->used);
	__set_bit(SFTL_SUBBLOCKS, sftl->used);
	__set_bit(SFTL_SUBBLOCKS + 1, sftl->used);
	sftl_mark_mapped(sftl, sftl->used);
	sftl->writer_prepared = true;
	return 0;
}

/* Caller holds the live map lock or owns an unpublished cold loader. */
static void sftl_copy_map(struct apple_sftl *sftl, struct apple_sftl_snapshot *snapshot)
{
	struct rb_node *node;
	u32 i = 0;

	for (node = rb_first(&sftl->runs); node; node = rb_next(node)) {
		const struct sftl_run *run = rb_entry(node, struct sftl_run, node);

		snapshot->runs[i++] = (struct apple_sftl_extent) {
			.logical = run->start, .length = run->end - run->start,
			.provider = run->provider,
		};
	}
	snapshot->nr_runs = sftl->nr_runs;
	snapshot->sequence = sftl->sequence;
	snapshot->journal_sequence = sftl->journal_sequence;
	snapshot->checkpoint_sequence = sftl->checkpoint_sequence;
}

/* Verify the native summary against every chunk it describes.  A cold loader
 * can skip old footers, but their claimed sequence ceiling alone cannot
 * authorize erase. Unknown record kinds remain on media.
 */
static int sftl_verify_closed(struct apple_sftl *sftl, struct apple_sftl_gc_block *victim,
			      unsigned long deadline, bool relocate)
{
	u32 first = victim->block * SFTL_STRIDE, capacity = hweight16(victim->lanes) * 512;
	u32 chunks, span, i, position = 0, previous_end = 0;
	unsigned int chunk;
	u8 trailer[SFTL_META];
	u64 base;
	int ret;

	ret = sftl_read_address(sftl, first + capacity - 4, &chunk);
	if (ret)
		return ret;
	memcpy(trailer, sftl->oob, SFTL_META);
	chunks = get_unaligned_le16(trailer + 12);
	span = get_unaligned_le32(trailer + 8);
	base = sftl_sequence(trailer);
	if (trailer[0] != 0x1c || chunks < 4 || chunks > SFTL_FOOTER_CHUNKS ||
	    chunks % 4 || trailer[1] >> 1 != chunks ||
	    (sftl->native_context && ((trailer[1] & 1) || sftl->block_flags[victim->block] != trailer[1])) ||
	    get_unaligned_le16(trailer + 14) != 0xffff || !base || !span ||
	    base + span - 1 >= sftl->checkpoint_start)
		return -EOPNOTSUPP;
	for (i = 0; i < chunks; i += 4) {
		ret = sftl_read_address(sftl, first + capacity - chunks + i, &chunk);
		if (ret)
			return ret;
		for (chunk = 0; chunk < 4; chunk++)
			if (memcmp(sftl->oob + chunk * SFTL_META, trailer, SFTL_META))
				return -EBADMSG;
		memcpy(sftl->footer + i * SFTL_CHUNK, sftl->page, APPLE_PPN_PAGE_SIZE);
	}
	for (i = 0; i < chunks * SFTL_CHUNK; i += SFTL_META) {
		const u8 *entry = sftl->footer + i;
		u32 relative = get_unaligned_le32(entry), high = get_unaligned_le32(entry + 4);
		u32 key = get_unaligned_le32(entry + 8), length = get_unaligned_le32(entry + 12);
		u32 j;

		if (!length) {
			if (position != capacity - chunks)
				return -EBADMSG;
			victim->last_sequence = base + span - 1;
			victim->footer_chunks = chunks;
			return 0;
		}
		if ((!position && relative) || relative < previous_end || relative >= span ||
		    length > span - relative || length > capacity - chunks - position)
			return -EBADMSG;
		if (key >= SFTL_LPAGES || length > SFTL_LPAGES - key)
			return -EOPNOTSUPP;
		for (j = 0; j < length; j++, position++) {
			u64 seq = base + relative + j;
			const u8 *meta;

			if (!(position % 4)) {
				if (time_after_eq(jiffies, deadline))
					return -ETIMEDOUT;
				ret = sftl_read_address(sftl, first + position, &chunk);
				if (ret)
					return ret;
			}
			meta = sftl->oob + (position % 4) * SFTL_META;
			/* Recognized native classes outside this operation are ineligible. */
			if ((meta[0] == 2 && !meta[1]) ||
			    (relocate && meta[0] == 1 && meta[1] == 1))
				return -EOPNOTSUPP;
			if (meta[0] != 1 || meta[1] > 1 ||
			    sftl_sequence(meta) != seq || !sftl_user_epoch_valid(meta, seq) ||
			    get_unaligned_le32(meta + 8) != key + j || high > seq >> 16)
				return -EBADMSG;
			/* Native media also has runs whose summary differs from individual
			 * user epochs. Leave them intact until that encoding is understood.
			 */
			if (get_unaligned_le32(meta + 12) != high)
				return -EOPNOTSUPP;
		}
		previous_end = relative + length;
	}
	return -EBADMSG;
}

static int sftl_block_blank(struct apple_sftl *sftl, u32 block, unsigned long deadline,
			    bool *blank)
{
	u32 capacity = hweight16(sftl->lanes[block]) * 512, pos;
	unsigned int chunk;
	int ret;

	*blank = false;
	for (pos = 0; pos < capacity; pos += 4) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = sftl_read_address(sftl, block * SFTL_STRIDE + pos, &chunk);
		if (ret)
			return ret;
		if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ||
		    !sftl_erased(sftl->oob, APPLE_PPN_OOB_SIZE))
			return 0;
	}
	*blank = true;
	return 0;
}

static bool sftl_closed_candidate(struct apple_sftl *sftl, u32 block)
{
	return sftl->checkpoint_start && test_bit(block, sftl->old_closed) &&
		(!sftl->native_context || sftl->roles[block] == SFTL_CLOSED_DATA ||
		 sftl->roles[block] == SFTL_DIRTY_FREE) &&
		!test_bit(block, sftl->reserved) && !test_bit(block, sftl->vfl_reserved) &&
		!test_bit(block, sftl->unknown_lane_flags);
}

static bool sftl_retired_candidate(struct apple_sftl *sftl, u32 block)
{
	return !sftl->vfl_degraded && !sftl->context_degraded &&
		(!sftl->native_context || sftl->roles[block] == SFTL_DIRTY_FREE) &&
		sftl_closed_candidate(sftl, block) && !test_bit(block, sftl->checkpoint_used);
}

static int sftl_erase_verified(struct apple_sftl *sftl, u32 block, unsigned long deadline)
{
	unsigned int lane;
	bool blank;
	int ret;

	for (lane = 0; lane < SFTL_LANES; lane++) {
		if (!(sftl->lanes[block] & BIT(lane)))
			continue;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = sftl->ppn->ops->erase_block(sftl->ppn->priv, lane & 1, lane >> 1, block);
		if (ret)
			return ret;
	}
	ret = sftl_block_blank(sftl, block, deadline, &blank);
	return ret ? ret : (blank ? 0 : -EBADMSG);
}

static int sftl_reclaim_retired(struct apple_sftl *sftl, u32 *claimed,
				unsigned long deadline)
{
	struct apple_sftl_snapshot active = {}, saved = {};
	struct apple_sftl *cold = NULL;
	size_t active_bytes, cold_bytes;
	u32 block;
	int ret = -ENOSPC;

	if (!sftl->ppn->ops->erase_block)
		return -ENOSPC;
	/* The top 32 physical blocks can contain native bootstrap members. */
	for (block = 1; block < SFTL_BLOCKS - 32; block++) {
		struct apple_sftl_gc_block victim = {
			.block = block, .lanes = sftl->lanes[block], .closed = true,
		};

		if (test_bit(block, sftl->used) || !sftl_retired_candidate(sftl, block))
			continue;
		if (!cold) {
			ret = apple_sftl_load(sftl->ppn, &cold);
			if (ret)
				break;
			active_bytes = array_size(sftl->nr_runs, sizeof(*active.runs));
			cold_bytes = array_size(cold->nr_runs, sizeof(*saved.runs));
			if (active_bytes > SFTL_GC_MAP_BYTES || cold_bytes > SFTL_GC_MAP_BYTES) {
				ret = -ENOMEM;
				break;
			}
			active.runs = kvzalloc(active_bytes, GFP_KERNEL);
			saved.runs = kvzalloc(cold_bytes, GFP_KERNEL);
			if (!active.runs || !saved.runs) {
				ret = -ENOMEM;
				break;
			}
			sftl_copy_map(sftl, &active);
			sftl_copy_map(cold, &saved);
		}
		if (!sftl_retired_candidate(cold, block) ||
		    cold->checkpoint_start != sftl->checkpoint_start ||
		    cold->lanes[block] != victim.lanes) {
			ret = -EBADMSG;
			break;
		}
		ret = sftl_verify_closed(sftl, &victim, deadline, false);
		if (!ret)
			ret = apple_sftl_gc_plan_retired(&active, &saved, &victim,
							  cold->checkpoint_start);
		if (ret == -EOPNOTSUPP) {
			ret = -ENOSPC;
			continue;
		}
		if (ret)
			break;
		__set_bit(block, sftl->used);
		ret = sftl_erase_verified(sftl, block, deadline);
		if (ret) {
			/* Never reissue an ambiguous, possibly partial erase. */
			sftl->failed = true;
			break;
		}
		*claimed = block;
		break;
	}
	kvfree(saved.runs);
	kvfree(active.runs);
	apple_sftl_free(cold);
	return ret;
}

static int sftl_check_landing(struct apple_sftl *sftl, u32 block, unsigned long deadline)
{
	bool blank;
	int ret;

	/* A cached spare was either positively free or was erased and checked
	 * in this transaction after verifying an ordinary closed donor.
	 */
	if (!block || block >= SFTL_SUBBLOCKS || !sftl->lanes[block] ||
	    (sftl->native_context && sftl->roles[block] != SFTL_CLEAN_FREE &&
	     sftl->roles[block] != SFTL_CLOSED_DATA && sftl->roles[block] != SFTL_DIRTY_FREE) ||
	    test_bit(block, sftl->reserved) || test_bit(block, sftl->vfl_reserved) ||
	    test_bit(block, sftl->checkpoint_used))
		return -EBADMSG;
	ret = sftl_block_blank(sftl, block, deadline, &blank);
	return ret ? ret : (blank ? 0 : -EBADMSG);
}

static int sftl_take_blank(struct apple_sftl *sftl, u32 *out, unsigned long deadline)
{
	u32 block;
	bool blank;
	int ret;

	while (sftl->next_candidate < SFTL_SUBBLOCKS) {
		block = sftl->next_candidate++;
		if (!test_bit(block, sftl->candidates) || test_bit(block, sftl->used) ||
		    (sftl->native_context && (sftl->roles[block] != SFTL_CLEAN_FREE ||
					      test_bit(block, sftl->fallback_occupied))) ||
		    (sftl->checkpoint_ops && test_bit(block, sftl->checkpoint_used)) ||
		    !sftl->lanes[block])
			continue;
		__set_bit(block, sftl->used);
		ret = sftl_block_blank(sftl, block, deadline, &blank);
		if (ret) {
			sftl->failed = true;
			return ret;
		}
		if (blank) {
			*out = block;
			return 0;
		}
	}
	return -ENOSPC;
}

static void sftl_open_block(struct apple_sftl *sftl, u32 block)
{
	sftl->append_block = block;
	sftl->append_capacity = hweight16(sftl->lanes[block]) * 512 - SFTL_FOOTER_CHUNKS;
	sftl->append_position = 0;
	sftl->append_base = sftl->sequence + 1;
	sftl->footer_runs = 0;
	memset(sftl->footer, 0, SFTL_FOOTER_BYTES);
}

static int sftl_program_address(struct apple_sftl *sftl, u32 address,
				const u8 *data, const u8 *oob)
{
	unsigned int bus, cau, block, page, chunk;
	int ret;

	ret = sftl_translate(sftl, address, &bus, &cau, &block, &page, &chunk);
	if (ret)
		return ret;
	if (chunk)
		return -EINVAL;
	return sftl->ppn->ops->program_page(sftl->ppn->priv, bus, cau,
					    block, page, data, oob);
}

/* A short request repeats its final logical page to fill the program unit. */
static void sftl_data_meta(u8 *meta, u64 sequence, u32 logical_page)
{
	put_unaligned_le64(sequence << 16 | 1, meta);
	put_unaligned_le32(logical_page, meta + 8);
	put_unaligned_le32(sequence >> 16, meta + 12);
}

static void sftl_append_summary(struct apple_sftl *sftl, const u8 *meta)
{
	u32 relative = sftl_sequence(meta) - sftl->append_base;
	u32 high = get_unaligned_le32(meta + 12), key = get_unaligned_le32(meta + 8);
	u8 *entry;

	if (sftl->footer_runs) {
		u32 length;

		entry = sftl->footer + (sftl->footer_runs - 1) * SFTL_META;
		length = get_unaligned_le32(entry + 12);
		if (key < 0xffff0000 && get_unaligned_le32(entry + 4) == high &&
		    get_unaligned_le32(entry) + length == relative &&
		    get_unaligned_le32(entry + 8) + length == key) {
			put_unaligned_le32(length + 1, entry + 12);
			return;
		}
	}
	entry = sftl->footer + sftl->footer_runs++ * SFTL_META;
	put_unaligned_le32(relative, entry);
	put_unaligned_le32(high, entry + 4);
	put_unaligned_le32(key, entry + 8);
	put_unaligned_le32(1, entry + 12);
}

static int sftl_snapshot_locked(struct apple_sftl *sftl, size_t max_bytes,
				struct apple_sftl_snapshot **out);

struct sftl_move {
	struct apple_sftl *sftl;
	unsigned long deadline;
};

static int sftl_move_read(void *priv, u32 address, struct apple_sftl_gc_page *page)
{
	struct sftl_move *move = priv;
	struct apple_sftl *sftl = move->sftl;
	unsigned int chunk;
	int ret;

	if (time_after_eq(jiffies, move->deadline))
		return -ETIMEDOUT;
	ret = sftl_read_address(sftl, address, &chunk);
	if (ret)
		return ret;
	memcpy(page->data, sftl->page, sizeof(page->data));
	memcpy(page->oob, sftl->oob, sizeof(page->oob));
	return 0;
}

static int sftl_move_program(void *priv, u32 address, const struct apple_sftl_gc_page *page)
{
	struct sftl_move *move = priv;
	struct apple_sftl *sftl = move->sftl;
	unsigned int chunk;
	int ret;

	if (time_after_eq(jiffies, move->deadline))
		return -ETIMEDOUT;
	if (address != sftl->append_block * SFTL_STRIDE + sftl->append_position ||
	    sftl->append_position + 4 > sftl->append_capacity)
		return -ERANGE;
	ret = sftl_program_address(sftl, address, page->data, page->oob);
	if (ret)
		return ret;
	for (chunk = 0; chunk < 4; chunk++)
		sftl_append_summary(sftl, page->oob + chunk * SFTL_META);
	sftl->append_position += 4;
	return 0;
}

static const struct apple_sftl_gc_io sftl_move_ops = {
	.read = sftl_move_read,
	.program = sftl_move_program,
};

/* Adopt the state independently decoded from media; keep the landing block's
 * verified summary and cursor so subsequent writes can use its free tail.
 */
static void sftl_adopt_checkpoint(struct apple_sftl *sftl, struct apple_sftl *cold)
{
	struct rb_root old_runs = sftl->runs;
	struct sftl_records old_context = sftl->context;
	u32 block;

	sftl->runs = cold->runs;
	cold->runs = old_runs;
	sftl->nr_runs = cold->nr_runs;
	sftl->context = cold->context;
	cold->context = old_context;
	kvfree(sftl->closed_user_seq);
	sftl->closed_user_seq = cold->closed_user_seq;
	cold->closed_user_seq = NULL;
	sftl->sequence = cold->sequence;
	sftl->journal_sequence = cold->journal_sequence;
	sftl->checkpoint_sequence = cold->checkpoint_sequence;
	sftl->checkpoint_start = cold->checkpoint_start;
	sftl->vfl_source = cold->vfl_source;
	sftl->vfl_degraded = cold->vfl_degraded;
	sftl->context_degraded = cold->context_degraded;
	sftl->native_context = cold->native_context;
	memcpy(sftl->roles, cold->roles, sizeof(sftl->roles));
	memcpy(sftl->block_flags, cold->block_flags, sizeof(sftl->block_flags));
	for (block = 0; block < ARRAY_SIZE(sftl->candidates); block++)
		sftl->candidates[block] &= cold->candidates[block];
	memcpy(sftl->fallback_occupied, cold->fallback_occupied, sizeof(sftl->fallback_occupied));
	memcpy(sftl->reserved, cold->reserved, sizeof(sftl->reserved));
	memcpy(sftl->vfl_reserved, cold->vfl_reserved, sizeof(sftl->vfl_reserved));
	memcpy(sftl->checkpoint_used, cold->checkpoint_used, sizeof(sftl->checkpoint_used));
	memcpy(sftl->old_closed, cold->old_closed, sizeof(sftl->old_closed));
	memcpy(sftl->unknown_lane_flags, cold->unknown_lane_flags, sizeof(sftl->unknown_lane_flags));
	for (block = 0; block < SFTL_BLOCKS; block++)
		if (test_bit(block, sftl->reserved) || test_bit(block, sftl->vfl_reserved) ||
		    test_bit(block, sftl->checkpoint_used))
			__set_bit(block, sftl->used);
}

/* A native save on this profile uses two named normal-view context blocks.
 * Only accept a complete generation starting at page zero of the active block,
 * with a blank second block. The old generation remains readable while the
 * alternate is programmed. Both blocks must be exclusively context-owned.
 */
static bool sftl_owns_context_block(struct apple_sftl *sftl, u32 block)
{
	return !sftl->vfl_degraded && !sftl->context_degraded &&
		block && block < SFTL_BLOCKS - 32 && sftl->lanes[block] &&
		test_bit(block, sftl->reserved) && !test_bit(block, sftl->vfl_reserved) &&
		!test_bit(block, sftl->unknown_lane_flags) && !test_bit(block, sftl->checkpoint_used);
}

static int sftl_native_pair(struct apple_sftl *sftl, const struct apple_sftl_snapshot *saved,
			    u32 blocks[2], unsigned long deadline)
{
	const u8 *base = saved->opaque[0].data;
	u64 length;
	u32 slots, i, j;
	unsigned int chunk;
	bool blank;
	int ret;

	if (get_unaligned_le32(base) != 2 || !sftl->checkpoint_start)
		return -EOPNOTSUPP;
	blocks[0] = get_unaligned_le32(base + 8);
	blocks[1] = get_unaligned_le32(base + 4);
	if (!apple_sftl_account_context_pair(saved, blocks))
		return -EOPNOTSUPP;
	if (blocks[0] == blocks[1] || !sftl_owns_context_block(sftl, blocks[0]) ||
	    !sftl_owns_context_block(sftl, blocks[1]) ||
	    sftl->context.items[0].address != blocks[1] * SFTL_STRIDE)
		return -EOPNOTSUPP;
	for (i = 0; i < saved->nr_runs; i++) {
		const struct apple_sftl_extent *r = &saved->runs[i];
		u32 end;

		if (r->provider == SFTL_UNMAPPED)
			continue;
		end = (r->provider + r->length - 1) / SFTL_STRIDE;
		for (j = 0; j < 2; j++)
			if (r->provider / SFTL_STRIDE <= blocks[j] && blocks[j] <= end)
				return -EBADMSG;
	}
	length = sftl->checkpoint_sequence - sftl->checkpoint_start + 1;
	slots = hweight16(sftl->lanes[blocks[1]]) * 512;
	if (!length || length % 4 || length > slots)
		return -EOPNOTSUPP;
	for (i = 0; i < slots; i += 4) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		ret = sftl_read_address(sftl, blocks[1] * SFTL_STRIDE + i, &chunk);
		if (ret)
			return ret;
		if (i >= length) {
			if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ||
			    !sftl_erased(sftl->oob, APPLE_PPN_OOB_SIZE))
				return -EOPNOTSUPP;
			continue;
		}
		for (j = 0; j < 4; j++) {
			const u8 *meta = sftl->oob + j * SFTL_META;

			if (meta[0] != 0x1f || !meta[1] || meta[1] > 6 ||
			    sftl_sequence(meta) != sftl->checkpoint_start + i + j)
				return -EOPNOTSUPP;
		}
	}
	ret = sftl_block_blank(sftl, blocks[0], deadline, &blank);
	return ret ? ret : (blank ? 0 : -EOPNOTSUPP);
}

static int sftl_context_program(void *priv, u32 address, const struct apple_sftl_gc_page *page)
{
	struct sftl_move *move = priv;

	if (time_after_eq(jiffies, move->deadline))
		return -ETIMEDOUT;
	return sftl_program_address(move->sftl, address, page->data, page->oob);
}

static const struct apple_sftl_gc_io sftl_context_io = {
	.read = sftl_move_read,
	.program = sftl_context_program,
};

static int sftl_reload_plan(struct apple_sftl *sftl, struct apple_sftl_gc *gc,
			    struct apple_sftl **out)
{
	struct apple_sftl_snapshot *snapshot = NULL;
	int ret;

	apple_sftl_free(*out);
	*out = NULL;
	ret = apple_sftl_load(sftl->ppn, out);
	if (ret)
		return ret;
	ret = sftl_snapshot_locked(*out, SFTL_GC_MAP_BYTES, &snapshot);
	if (!ret)
		ret = apple_sftl_gc_recovered(gc, snapshot);
	apple_sftl_snapshot_free(snapshot);
	if (!ret && memcmp(sftl->lanes, (*out)->lanes, sizeof(sftl->lanes)))
		return -EBADMSG;
	return ret;
}

static int sftl_compact(struct apple_sftl *sftl, u32 landing, unsigned long deadline)
{
	struct apple_sftl_gc_block destination = {
		.block = landing, .lanes = sftl->lanes[landing], .blank = true,
	};
	struct sftl_move move = { .sftl = sftl, .deadline = deadline };
	struct apple_sftl_checkpoint *image = NULL, *readback = NULL;
	struct apple_sftl_snapshot *before = NULL;
	struct apple_sftl_gc *gc = NULL;
	struct apple_sftl *cold = NULL;
	u32 blocks[SFTL_MAX_CONTEXTS], nr = ARRAY_SIZE(blocks), block, slots, i;
	int ret = -ENOSPC;

	for (block = 1; block < SFTL_BLOCKS - 32; block++) {
		struct apple_sftl_gc_block victim = {
			.block = block, .lanes = sftl->lanes[block], .closed = true,
		};

		if (!sftl_closed_candidate(sftl, block) || !test_bit(block, sftl->checkpoint_used) ||
		    (sftl->native_context && sftl->roles[block] != SFTL_CLOSED_DATA))
			continue;
		if (!before) {
			ret = sftl_snapshot_locked(sftl, SFTL_GC_MAP_BYTES, &before);
			if (ret)
				goto out;
		}
		ret = sftl_verify_closed(sftl, &victim, deadline, true);
		if (ret == -EOPNOTSUPP) {
			ret = -ENOSPC;
			continue;
		}
		if (ret)
			goto out;
		ret = apple_sftl_gc_plan(before, &victim, &destination, SFTL_MAX_CONTEXTS * SFTL_STRIDE,
					 SFTL_GC_MAP_BYTES, &gc);
		if (ret == -ENOSPC || ret == -EOVERFLOW) {
			ret = -ENOSPC;
			continue;
		}
		if (ret)
			goto out;
		slots = apple_sftl_gc_copy_chunks(gc);
		/* Keep at least a page for this request, and gain usable capacity. */
		if (!slots || !apple_sftl_gc_has_sequence_room(gc, 4) ||
		    slots + 4 > hweight16(destination.lanes) * 512 - SFTL_FOOTER_CHUNKS ||
		    slots >= hweight16(victim.lanes) * 512 - SFTL_FOOTER_CHUNKS) {
			apple_sftl_gc_free(gc);
			gc = NULL;
			ret = -ENOSPC;
			continue;
		}
		if (sftl->checkpoint_ops->prepare_native) {
			nr = 2;
			ret = sftl_native_pair(sftl, before, blocks, deadline);
			if (!ret)
				ret = apple_sftl_gc_prepare_native(gc, sftl->checkpoint_ops->prepare_native,
						sftl->checkpoint_priv, blocks, hweight16(sftl->lanes[blocks[0]]) * 512);
		} else {
			ret = sftl->checkpoint_ops->reserve(sftl->checkpoint_priv, blocks, &nr);
		}
		if (ret)
			goto out;
		if (!nr || nr > ARRAY_SIZE(blocks)) {
			ret = -EINVAL;
			goto out;
		}
		for (i = 0; i < nr; i++) {
			u32 k;

			if (!blocks[i] || blocks[i] >= SFTL_SUBBLOCKS || blocks[i] == block ||
			    blocks[i] == landing || !test_bit(blocks[i], sftl->reserved)) {
				ret = -EINVAL;
				goto out;
			}
			for (k = 0; k < i; k++)
				if (blocks[k] == blocks[i]) {
					ret = -EINVAL;
					goto out;
				}
		}
		ret = sftl_check_landing(sftl, landing, deadline);
		if (ret)
			goto failed;
		sftl_open_block(sftl, landing);
		ret = apple_sftl_gc_relocate(gc, &sftl_move_ops, &move);
		if (ret) {
			if (apple_sftl_gc_stage(gc) == APPLE_SFTL_GC_COPY) {
				/* The executor could not start. No page was programmed. */
				sftl->append_block = U32_MAX;
				goto out;
			}
			goto failed;
		}
		ret = apple_sftl_gc_checkpoint(gc, blocks, nr, &image);
		if (ret)
			goto failed;
		if (sftl->checkpoint_ops->prepare_native)
			ret = apple_sftl_checkpoint_stage(image, blocks[0], sftl->lanes[blocks[0]],
							  &sftl_context_io, &move, &readback);
		else
			ret = sftl->checkpoint_ops->install(sftl->checkpoint_priv, image, &readback);
		if (readback == image) {
			readback = NULL;
			ret = -EBADMSG;
		} else if (readback && readback->records == image->records) {
			readback->records = NULL;
			ret = -EBADMSG;
		}
		if (ret)
			goto failed;
		ret = apple_sftl_gc_checkpoint_done(gc, readback);
		if (ret)
			goto failed;
		ret = sftl_reload_plan(sftl, gc, &cold);
		if (ret)
			goto failed;
		if (sftl->checkpoint_ops->prepare_native) {
			/* The new map has now been recovered with the fallback intact.
			 * Verify its physical location and both reservations before
			 * retiring the old context. Reload again before releasing data.
			 */
			if (!cold->native_context || cold->roles[landing] != SFTL_OPEN_DATA ||
			    cold->roles[block] != SFTL_DIRTY_FREE ||
			    cold->context.items[0].address != blocks[0] * SFTL_STRIDE ||
			    !sftl_owns_context_block(cold, blocks[0]) ||
			    !sftl_owns_context_block(cold, blocks[1])) {
				ret = -EBADMSG;
				goto failed;
			}
			ret = sftl_erase_verified(sftl, blocks[1], deadline);
			if (ret)
				goto failed;
			ret = sftl_reload_plan(sftl, gc, &cold);
			if (ret)
				goto failed;
		}
		if (!sftl_retired_candidate(cold, block) || test_bit(landing, cold->unknown_lane_flags) ||
		    test_bit(landing, cold->reserved) || test_bit(landing, cold->vfl_reserved) ||
		    (cold->native_context && (cold->roles[landing] != SFTL_OPEN_DATA ||
					      cold->roles[block] != SFTL_DIRTY_FREE))) {
			ret = -EBADMSG;
			goto failed;
		}
		ret = sftl_erase_verified(sftl, block, deadline);
		if (ret)
			goto failed;
		sftl_adopt_checkpoint(sftl, cold);
		sftl->spare = block;
		goto out;
	}
	goto out;
failed:
	sftl->failed = true;
out:
	apple_sftl_free(cold);
	apple_sftl_snapshot_free(before);
	apple_sftl_checkpoint_free(readback);
	apple_sftl_checkpoint_free(image);
	apple_sftl_gc_free(gc);
	return ret;
}

static bool sftl_can_compact(struct apple_sftl *sftl)
{
	u32 block;

	if (!sftl->checkpoint_ops || !sftl->ppn->ops->erase_block)
		return false;
	for (block = 1; block < SFTL_BLOCKS - 32; block++)
		if (sftl_closed_candidate(sftl, block) && test_bit(block, sftl->checkpoint_used))
			return true;
	return false;
}

static int sftl_claim_block(struct apple_sftl *sftl)
{
	unsigned long deadline = jiffies + SFTL_CLAIM_TIMEOUT;
	u32 block, spare;
	int ret;

	if (sftl->spare != U32_MAX) {
		block = sftl->spare;
		sftl->spare = U32_MAX;
		ret = sftl_check_landing(sftl, block, deadline);
		if (ret) {
			sftl->failed = true;
			return ret;
		}
	} else {
		ret = sftl_take_blank(sftl, &block, deadline);
		if (ret == -ENOSPC && !sftl->failed)
			ret = sftl_reclaim_retired(sftl, &block, deadline);
		if (ret)
			return ret;
	}
	/* Only reserve landing space when a complete commit backend exists. */
	if (!test_bit(block, sftl->unknown_lane_flags) && sftl_can_compact(sftl)) {
		ret = sftl_take_blank(sftl, &spare, deadline);
		if (!ret)
			sftl->spare = spare;
		else if (ret == -ENOSPC && !sftl->failed) {
			/* Use already-retired space ahead of copying live records.
			 * Keep the checked landing available for a later transaction.
			 */
			ret = sftl_reclaim_retired(sftl, &spare, deadline);
			if (!ret) {
				sftl->spare = block;
				sftl_open_block(sftl, spare);
				return 0;
			}
			if (ret != -ENOSPC || sftl->failed) {
				if (!sftl->failed)
					sftl->spare = block;
				return ret;
			}
			ret = sftl_compact(sftl, block, deadline);
			if (!ret || sftl->failed)
				return ret;
			if (ret != -ENOSPC && ret != -EOPNOTSUPP) {
				sftl->spare = block;
				return ret;
			}
		} else {
			if (!sftl->failed)
				sftl->spare = block;
			return ret;
		}
	}
	sftl_open_block(sftl, block);
	return 0;
}

void apple_sftl_set_checkpoint_ops(struct apple_sftl *sftl,
				   const struct apple_sftl_checkpoint_ops *ops, void *priv)
{
	mutex_lock(&sftl->lock);
	sftl->checkpoint_ops = NULL;
	if (ops && ((sftl->native_context && ops->prepare_native && !ops->reserve && !ops->install) ||
		    (!sftl->native_context && !ops->prepare_native && ops->reserve && ops->install)))
		sftl->checkpoint_ops = ops;
	sftl->checkpoint_priv = priv;
	mutex_unlock(&sftl->lock);
}
EXPORT_SYMBOL_GPL(apple_sftl_set_checkpoint_ops);

static int sftl_close_block(struct apple_sftl *sftl)
{
	u64 span = sftl->sequence - sftl->append_base + 1;
	u8 meta[SFTL_META];
	unsigned int chunk, index;
	int ret;

	if (!span || span > U32_MAX)
		return -EOVERFLOW;
	put_unaligned_le64(sftl->append_base << 16 | 0x401c, meta);
	put_unaligned_le32(span, meta + 8);
	put_unaligned_le16(SFTL_FOOTER_CHUNKS, meta + 12);
	put_unaligned_le16(0xffff, meta + 14);
	for (chunk = 0; chunk < 4; chunk++)
		memcpy(sftl->oob + chunk * SFTL_META, meta, SFTL_META);
	for (index = 0; index < SFTL_FOOTER_CHUNKS / 4; index++) {
		u32 address = sftl->append_block * SFTL_STRIDE +
			      sftl->append_capacity + index * 4;

		ret = sftl_program_address(sftl, address,
					   sftl->footer + index * APPLE_PPN_PAGE_SIZE,
					   sftl->oob);
		if (ret)
			return ret;
	}
	ret = sftl_remember_user_seq(sftl, sftl->append_block, sftl->footer, sftl->footer_runs);
	if (ret)
		return ret;
	sftl->append_block = U32_MAX;
	return 0;
}

int apple_sftl_write(struct apple_sftl *sftl, u32 logical_page,
		     const u8 *data, unsigned int count)
{
	u64 first_sequence;
	u32 address;
	unsigned int chunk;
	int ret;

	if (!sftl || !data || !count || count > 4 || logical_page >= SFTL_DISK_PAGES ||
	    count > SFTL_DISK_PAGES - logical_page)
		return -EINVAL;
	mutex_lock(&sftl->lock);
	if (sftl->failed) {
		ret = -EIO;
		goto out;
	}
	if (sftl->sequence > SFTL_SEQUENCE - 4) {
		ret = -EOVERFLOW;
		goto out;
	}
	ret = sftl_writer_prepare(sftl);
	if (ret)
		goto out;
	if (sftl->append_block == U32_MAX) {
		ret = sftl_claim_block(sftl);
		if (ret)
			goto out;
	}
	if (sftl->sequence > SFTL_SEQUENCE - 4) {
		ret = -EOVERFLOW;
		goto out;
	}
	address = sftl->append_block * SFTL_STRIDE + sftl->append_position;
	first_sequence = sftl->sequence + 1;
	for (chunk = 0; chunk < 4; chunk++) {
		unsigned int index = min(chunk, count - 1);

		memcpy(sftl->page + chunk * SFTL_CHUNK,
		       data + index * SFTL_CHUNK, SFTL_CHUNK);
		sftl_data_meta(sftl->oob + chunk * SFTL_META,
			       first_sequence + chunk, logical_page + index);
	}
	ret = sftl_program_address(sftl, address, sftl->page, sftl->oob);
	if (ret)
		goto failed;
	for (chunk = 0; chunk < 4; chunk++) {
		u32 key = logical_page + min(chunk, count - 1);

		ret = sftl_replace(sftl, key, 1, address + chunk);
		if (ret)
			goto failed;
		sftl_append_summary(sftl, sftl->oob + chunk * SFTL_META);
	}
	sftl->sequence += 4;
	sftl->journal_sequence = sftl->sequence;
	sftl->append_position += 4;
	if (sftl->append_position == sftl->append_capacity) {
		ret = sftl_close_block(sftl);
		if (ret)
			goto failed;
	}
	goto out;
failed:
	sftl->failed = true;
out:
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_write);

int apple_sftl_flush(struct apple_sftl *sftl)
{
	int ret;

	if (!sftl)
		return -EINVAL;
	mutex_lock(&sftl->lock);
	/* Each program is complete and verified before apple_sftl_write returns. */
	ret = sftl->failed ? -EIO : 0;
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_flush);

void apple_sftl_get_info(struct apple_sftl *sftl, struct apple_sftl_info *info)
{
	info->read_only = sftl->vfl_degraded || sftl->context_degraded;
	info->logical_pages = SFTL_LPAGES;
	info->disk_pages = SFTL_DISK_PAGES;
	info->nr_runs = sftl->nr_runs;
	info->checkpoint_sequence = sftl->checkpoint_sequence;
	info->sequence = sftl->sequence;
	info->journal_sequence = sftl->journal_sequence;
}
EXPORT_SYMBOL_GPL(apple_sftl_get_info);

static int sftl_snapshot_locked(struct apple_sftl *sftl, size_t max_bytes,
			struct apple_sftl_snapshot **out)
{
	struct apple_sftl_snapshot *snapshot;
	size_t map_bytes, context_bytes;
	u32 i = 0;
	unsigned int chunk;
	int ret = 0;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!sftl)
		return -EINVAL;
	if (sftl->failed) {
		ret = -EIO;
		goto unlock;
	}
	map_bytes = array_size(sftl->nr_runs, sizeof(*snapshot->runs));
	context_bytes = array_size(sftl->context.count, sizeof(*snapshot->opaque));
	if (map_bytes > max_bytes || context_bytes > max_bytes - map_bytes) {
		ret = -ENOSPC;
		goto unlock;
	}
	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto unlock;
	}
	snapshot->runs = kvzalloc(map_bytes, GFP_KERNEL);
	snapshot->opaque = kvzalloc(context_bytes, GFP_KERNEL);
	if (!snapshot->runs || !snapshot->opaque) {
		ret = -ENOMEM;
		goto failed;
	}
	sftl_copy_map(sftl, snapshot);
	for (i = 0; i < sftl->context.count; i++) {
		const struct sftl_record *reference = &sftl->context.items[i];
		struct apple_sftl_context_record *record = &snapshot->opaque[i];

		ret = sftl_read_address(sftl, reference->address, &chunk);
		if (ret) {
			sftl->failed = true;
			goto failed;
		}
		if (sftl_stamp(sftl->oob + chunk * SFTL_META) != reference->stamp) {
			ret = -EBADMSG;
			sftl->failed = true;
			goto failed;
		}
		memcpy(record->meta, sftl->oob + chunk * SFTL_META, SFTL_META);
		memcpy(record->data, sftl->page + chunk * SFTL_CHUNK, SFTL_CHUNK);
	}
	snapshot->nr_opaque = sftl->context.count;
	if (sftl->closed_user_seq) {
		ret = apple_sftl_account_user_seq(snapshot, sftl->closed_user_seq);
		if (ret)
			goto failed;
	}
	*out = snapshot;
	goto unlock;
failed:
	apple_sftl_snapshot_free(snapshot);
unlock:
	return ret;
}
int apple_sftl_snapshot(struct apple_sftl *sftl, size_t max_bytes,
			struct apple_sftl_snapshot **out)
{
	int ret;

	if (!sftl || !out) {
		if (out)
			*out = NULL;
		return -EINVAL;
	}
	mutex_lock(&sftl->lock);
	ret = sftl_snapshot_locked(sftl, max_bytes, out);
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_snapshot);

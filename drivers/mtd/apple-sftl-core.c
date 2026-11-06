// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple VFL/SwissPPNFTL read-only recovery for the measured 64 GB PPN layout.
 *
 * Format and replay rules are ported from Orchard's P105 Python reference
 * (ftl_map.py, ftl_load.py and ftl_log.py). That implementation was checked
 * against native iOS logical reads. This code contains no captured firmware
 * or device data. Unsupported formats fail before a block device is created.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
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
#include "apple-sftl-reclaim.h"

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

struct sftl_member {
	u16 block;
	u8 lane;
};

struct apple_sftl {
	struct apple_ppn *ppn;
	struct rb_root runs;
	struct sftl_records context;
	u32 nr_runs;
	u16 lanes[SFTL_BLOCKS];
	DECLARE_BITMAP(reserved, SFTL_BLOCKS);
	u64 checkpoint_sequence, sequence, journal_sequence;
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

static int sftl_vfl(struct apple_sftl *sftl)
{
	struct sftl_member members[8];
	u8 list[32], best_oob[64], best_members = 0;
	u8 *best, *table;
	u32 best_age = U32_MAX;
	unsigned int member, page, latest_page = 0, lane, block;
	int ret;

	best = kmalloc(APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	table = kmalloc(APPLE_PPN_PAGE_SIZE + APPLE_PPN_OOB_SIZE, GFP_KERNEL);
	if (!best || !table) {
		ret = -ENOMEM;
		goto out;
	}
	ret = sftl_read_page(sftl, 0, 0, 2087, 0, true, sftl->page, sftl->oob);
	if (ret)
		goto out;
	if (get_unaligned_le32(sftl->oob) != 0x00ffff20) {
		ret = -EBADMSG;
		goto out;
	}
	memcpy(list, sftl->page + 0x100, sizeof(list));
	for (member = 0; member < 8; member++) {
		unsigned int other;

		members[member].block = get_unaligned_le16(list + member * 4);
		members[member].lane = list[member * 4 + 2];
		if (members[member].block < 2056 || members[member].block >= SFTL_BLOCKS ||
		    members[member].lane >= SFTL_LANES || list[member * 4 + 3]) {
			ret = -EBADMSG;
			goto out;
		}
		for (other = 0; other < member; other++)
			if (members[other].block == members[member].block &&
			    members[other].lane == members[member].lane) {
				ret = -EBADMSG;
				goto out;
			}
	}
	for (member = 0; member < 8; member++) {
		for (page = 0; page < 64; page++) {
			u32 age;

			ret = sftl_read_page(sftl, members[member].lane & 1,
					     members[member].lane >> 1, members[member].block,
					     page, true, sftl->page, sftl->oob);
			if (ret)
				goto out;
			if (!page && memcmp(sftl->page + 0x100, list, sizeof(list))) {
				ret = -EBADMSG;
				goto out;
			}
			if (get_unaligned_le32(sftl->oob) != 0x00ffff20)
				continue;
			age = get_unaligned_le32(sftl->oob + 4);
			if (age < BIT(31)) {
				ret = -EOPNOTSUPP;
				goto out;
			}
			if (age > best_age)
				continue;
			if (!best_members || age < best_age) {
				best_age = age;
				latest_page = page;
				memcpy(best, sftl->page, APPLE_PPN_PAGE_SIZE);
				memcpy(best_oob, sftl->oob, sizeof(best_oob));
				best_members = BIT(member);
			} else {
				if (page != latest_page || best_members & BIT(member) ||
				    memcmp(best, sftl->page, APPLE_PPN_PAGE_SIZE) ||
				    memcmp(best_oob, sftl->oob, sizeof(best_oob))) {
					ret = -EBADMSG;
					goto out;
				}
				best_members |= BIT(member);
			}
		}
	}
	if ((best_members != 0x0f && best_members != 0xf0) ||
	    get_unaligned_le16(best + 10) != 16 ||
	    get_unaligned_le16(best + 12) != SFTL_SUBBLOCKS ||
	    get_unaligned_le16(best + 14) != SFTL_SUBBLOCKS) {
		ret = -EBADMSG;
		goto out;
	}
	for (lane = 0; lane < SFTL_LANES; lane++) {
		u16 reference = get_unaligned_le16(best + 0x200 + lane * 32);
		unsigned int group = reference >> 15;

		page = reference & 0x7fff;
		if (page >= 64) {
			ret = -EBADMSG;
			goto out;
		}
		for (member = group * 4; member < group * 4 + 4; member++) {
			ret = sftl_read_page(sftl, members[member].lane & 1,
					     members[member].lane >> 1, members[member].block,
					     page, true, sftl->page, sftl->oob);
			if (ret)
				goto out;
			if (sftl->oob[0] != 0x20 || sftl->oob[1] != lane ||
			    sftl->oob[2] || sftl->oob[3]) {
				ret = -EBADMSG;
				goto out;
			}
			if (member == group * 4) {
				memcpy(table, sftl->page, APPLE_PPN_PAGE_SIZE);
				memcpy(table + APPLE_PPN_PAGE_SIZE, sftl->oob, sizeof(sftl->oob));
			} else if (memcmp(table, sftl->page, APPLE_PPN_PAGE_SIZE) ||
				   memcmp(table + APPLE_PPN_PAGE_SIZE,
					  sftl->oob, sizeof(sftl->oob))) {
				ret = -EBADMSG;
				goto out;
			}
		}
		for (block = 0; block < SFTL_BLOCKS; block++)
			if (!(get_unaligned_le16(table + block * 2) & BIT(15)))
				sftl->lanes[block] |= BIT(lane);
	}
	ret = 0;
out:
	kfree(table);
	kfree(best);
	return ret;
}

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

static int sftl_context(struct apple_sftl *sftl, const u8 *heads)
{
	struct sftl_records records = {};
	u16 pending[SFTL_MAX_CONTEXTS];
	u32 count = 0, block, at, i, first = 0, cursor = 0, chunk;
	u64 previous = 0, fragment_sequence = 0;
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
		u32 position, pages;

		block = pending[at];
		pages = hweight16(sftl->lanes[block]) * 128;
		if (!pages) {
			ret = -EBADMSG;
			goto out;
		}
		for (position = 0; position < pages; position++) {
			ret = sftl_read_address(sftl, block * SFTL_STRIDE + position * 4, &chunk);
			if (ret)
				goto out;
			if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
				ret = sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ? 0 : -EBADMSG;
				if (ret)
					goto out;
				break;
			}
			for (chunk = 0; chunk < 4; chunk++) {
				const u8 *meta = sftl->oob + chunk * SFTL_META;
				const u8 *payload = sftl->page + chunk * SFTL_CHUNK;

				if (sftl_erased(meta, SFTL_META))
					continue;
				if (meta[0] != 0x1f || !meta[1] || meta[1] > 6) {
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
	}
	ret = -EBADMSG;
	if (!records.count)
		goto out;
	sort(records.items, records.count, sizeof(*records.items), sftl_record_compare, NULL);
	if (sftl_flags(&records.items[0]) != 1)
		goto out;
	for (i = 1; i < records.count; i++)
		if (sftl_flags(&records.items[i]) == 1 && sftl_flags(&records.items[i - 1]) != 1)
			first = i;
	if (sftl_flags(&records.items[records.count - 1]) != 4)
		goto out;
	for (i = first; i < records.count; i++) {
		const struct sftl_record *record = &records.items[i];
		u64 sequence = record->stamp & SFTL_SEQUENCE;
		const u8 *payload;
		u32 offset, value, length, names;

		if (i != first && sequence != previous + 1)
			goto out;
		previous = sequence;
		ret = sftl_read_address(sftl, record->address, &chunk);
		if (ret)
			goto out;
		ret = -EBADMSG;
		if (sftl_stamp(sftl->oob + chunk * SFTL_META) != record->stamp)
			goto out;
		payload = sftl->page + chunk * SFTL_CHUNK;
		if (i == first) {
			names = get_unaligned_le32(payload);
			if (!names || names > SFTL_MAX_CONTEXTS)
				goto out;
			for (offset = 0; offset < names; offset++) {
				value = get_unaligned_le32(payload + 4 + offset * 4);
				if (value >= SFTL_BLOCKS || !test_bit(value, sftl->reserved))
					goto out;
			}
		}
		if (sftl_flags(record) != 4)
			continue;
		if (fragment_sequence && sequence != fragment_sequence + 1)
			goto out;
		fragment_sequence = sequence;
		value = get_unaligned_le32(payload + 4);
		if (value == U32_MAX)
			continue;
		if (value != 0xfffffff0 || get_unaligned_le32(payload) != cursor)
			goto out;
		for (offset = 8; offset < SFTL_CHUNK; offset += 8) {
			value = get_unaligned_le32(payload + offset);
			length = get_unaligned_le32(payload + offset + 4);
			if (value == U32_MAX)
				break;
			if (!length || length > SFTL_LPAGES - cursor)
				goto out;
			if (value == BIT(fls(SFTL_PROVIDER * 2 - 1) - 1) - 1)
				value = SFTL_UNMAPPED;
			ret = sftl_add_run(sftl, cursor, cursor + length, value);
			if (ret)
				goto out;
			cursor += length;
			ret = -EBADMSG;
		}
	}
	if (cursor != SFTL_LPAGES || !fragment_sequence)
		goto out;
	/* Retain compact references so an explicit snapshot can preserve opaque data. */
	for (i = first; i < records.count; i++) {
		const struct sftl_record *record = &records.items[i];

		if (sftl_flags(record) == 4)
			continue;
		ret = sftl_record_add(&sftl->context, record->stamp, record->key,
				      record->address);
		if (ret)
			goto out;
	}
	sftl->checkpoint_sequence = fragment_sequence;
	sftl->sequence = fragment_sequence;
	ret = 0;
out:
	kvfree(records.items);
	return ret;
}

static int sftl_add_journal(struct sftl_records *journal, const u8 *meta,
			    u32 address, u64 checkpoint)
{
	u64 sequence = sftl_sequence(meta);

	if (sequence <= checkpoint || sftl_erased(meta, SFTL_META))
		return 0;
	if ((meta[0] != 1 && meta[0] != 2) ||
	    get_unaligned_le32(meta + 12) != sequence >> 16 ||
	    (meta[0] == 1 ? meta[1] > 1 : meta[1] != 0))
		return -EBADMSG;
	return sftl_record_add(journal, sftl_stamp(meta), get_unaligned_le32(meta + 8), address);
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
	    base != sftl_sequence(head) || !span || base + span > SFTL_SEQUENCE)
		return -EBADMSG;
	*floor = max(*floor, base + span - 1);
	if (base + span - 1 <= sftl->checkpoint_sequence)
		return 0;
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
				put_unaligned_le32(sequence >> 16, meta + 12);
			}
			ret = sftl_add_journal(journal, meta, address, sftl->checkpoint_sequence);
			if (ret)
				goto out;
		}
	}
	ret = terminated && position == pages * 4 - chunks ? 0 : -EBADMSG;
out:
	kfree(footer);
	return ret;
}

static int sftl_journal(struct apple_sftl *sftl, const u8 *heads)
{
	struct sftl_records journal = {};
	u64 floor = sftl->checkpoint_sequence, expected = floor + 1;
	u32 block, position, i, pages;
	unsigned int chunk;
	int ret;

	for (block = 1; block < SFTL_SUBBLOCKS; block++) {
		const u8 *head = heads + block * SFTL_META;

		if (head[0] != 1 && head[0] != 2)
			continue;
		pages = hweight16(sftl->lanes[block]) * 128;
		ret = sftl_read_address(sftl, block * SFTL_STRIDE + (pages - 1) * 4, &chunk);
		if (ret)
			goto out;
		if (sftl->oob[0] == 0x1c) {
			ret = sftl_footer(sftl, head, block, pages, &journal, &floor);
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
			for (chunk = 0; chunk < 4; chunk++) {
				ret = sftl_add_journal(&journal, sftl->oob + chunk * SFTL_META,
						       block * SFTL_STRIDE + position * 4 + chunk,
						       sftl->checkpoint_sequence);
				if (ret)
					goto out;
			}
		}
	}
	sort(journal.items, journal.count, sizeof(*journal.items), sftl_record_compare, NULL);
	for (i = 0; i < journal.count; i++) {
		const struct sftl_record *record = &journal.items[i];
		u64 sequence = record->stamp & SFTL_SEQUENCE;

		ret = -EBADMSG;
		if (sequence != expected++)
			goto out;
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
	sftl->journal_sequence = sftl->sequence;
	sftl->sequence = max(sftl->sequence, floor);
	ret = 0;
out:
	kvfree(journal.items);
	return ret;
}

int apple_sftl_load(struct apple_ppn *provider, struct apple_sftl **out)
{
	struct apple_sftl *sftl;
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
		} else if (sftl->oob[0] == 0x1f || sftl->oob[0] == 1 || sftl->oob[0] == 2) {
			memcpy(heads + block * SFTL_META, sftl->oob, SFTL_META);
		} else {
			ret = -EOPNOTSUPP;
			goto failed;
		}
	}
	ret = sftl_context(sftl, heads);
	if (ret)
		goto failed;
	ret = sftl_journal(sftl, heads);
	if (ret)
		goto failed;
	kfree(heads);
	*out = sftl;
	return 0;
failed:
	kfree(heads);
	apple_sftl_free(sftl);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_load);

void apple_sftl_free(struct apple_sftl *sftl)
{
	struct rb_node *node;

	if (!sftl)
		return;
	while ((node = rb_first(&sftl->runs))) {
		rb_erase(node, &sftl->runs);
		kfree(rb_entry(node, struct sftl_run, node));
	}
	kvfree(sftl->context.items);
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

void apple_sftl_get_info(struct apple_sftl *sftl, struct apple_sftl_info *info)
{
	info->logical_pages = SFTL_LPAGES;
	info->disk_pages = SFTL_DISK_PAGES;
	info->nr_runs = sftl->nr_runs;
	info->checkpoint_sequence = sftl->checkpoint_sequence;
	info->sequence = sftl->sequence;
}
EXPORT_SYMBOL_GPL(apple_sftl_get_info);

int apple_sftl_snapshot(struct apple_sftl *sftl, size_t max_bytes,
			struct apple_sftl_snapshot **out)
{
	struct apple_sftl_snapshot *snapshot;
	struct rb_node *node;
	size_t map_bytes, context_bytes;
	u32 i = 0;
	unsigned int chunk;
	int ret = 0;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!sftl)
		return -EINVAL;
	mutex_lock(&sftl->lock);
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
	for (node = rb_first(&sftl->runs); node; node = rb_next(node)) {
		const struct sftl_run *run = rb_entry(node, struct sftl_run, node);

		snapshot->runs[i++] = (struct apple_sftl_extent) {
			.logical = run->start, .length = run->end - run->start,
			.provider = run->provider,
		};
	}
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
	snapshot->nr_runs = sftl->nr_runs;
	snapshot->nr_opaque = sftl->context.count;
	snapshot->sequence = sftl->sequence;
	snapshot->journal_sequence = sftl->journal_sequence;
	snapshot->checkpoint_sequence = sftl->checkpoint_sequence;
	*out = snapshot;
	goto unlock;
failed:
	apple_sftl_snapshot_free(snapshot);
unlock:
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_snapshot);

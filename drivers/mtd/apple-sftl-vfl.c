// SPDX-License-Identifier: GPL-2.0-only
/* Healthy single-fragment VFL bank rotation on the measured 64 GB geometry. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "apple-sftl-vfl.h"

#define VFL_LANES	16
#define VFL_COPIES	4
#define VFL_PAGES	64
#define VFL_GLOBAL	0xff

struct vfl_page {
	u8 data[APPLE_PPN_PAGE_SIZE];
	u8 oob[APPLE_PPN_OOB_SIZE];
};

struct vfl_rotation {
	struct apple_ppn *ppn;
	const struct apple_sftl_vfl_source *source;
	const struct apple_sftl_vfl_ops *ops;
	void *priv;
	struct vfl_page checkpoint, tables[VFL_LANES], first, other;
};

static int vfl_read(struct vfl_rotation *r, unsigned int group, unsigned int member,
		    unsigned int page, struct vfl_page *out)
{
	const struct apple_sftl_vfl_member *m = &r->source->members[group * VFL_COPIES + member];
	u8 status;
	int ret;

	ret = r->ppn->ops->read_page(r->ppn->priv, m->lane & 1, m->lane >> 1,
				     m->block, page, true, out->data, out->oob, &status);
	if (ret)
		return ret;
	if (status == 0x40)
		return 0;
	if (status == 0x49 && !memchr_inv(out, 0xff, sizeof(*out)))
		return 0;
	return -EIO;
}

static int vfl_read_copies(struct vfl_rotation *r, unsigned int group,
			   unsigned int page, struct vfl_page *out)
{
	unsigned int i;
	int ret;

	for (i = 0; i < VFL_COPIES; i++) {
		ret = vfl_read(r, group, i, page, i ? &r->other : out);
		if (ret)
			return ret;
		if (i && memcmp(out, &r->other, sizeof(*out)))
			return -EBADMSG;
	}
	return 0;
}

/* 13G36 _programVFLCxt emits a single 16-byte tag for this geometry. */
static void vfl_tag(u8 *oob, u8 lane, u32 age)
{
	memset(oob, 0xff, APPLE_PPN_OOB_SIZE);
	memset(oob, 0, 16);
	oob[0] = 0x20;
	oob[1] = lane;
	oob[2] = lane == VFL_GLOBAL ? 0xff : 0;
	put_unaligned_le32(age, oob + 4);
}

static bool vfl_tag_matches(const struct vfl_page *p, u8 lane, u32 age)
{
	u8 expected[APPLE_PPN_OOB_SIZE];

	vfl_tag(expected, lane, age);
	return !memcmp(p->oob, expected, sizeof(expected));
}

static bool vfl_members_valid(const struct apple_sftl_vfl_source *s)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(s->members); i++) {
		const struct apple_sftl_vfl_member *m = &s->members[i];

		if (m->block < 2056 || m->block >= 2088 || m->lane >= VFL_LANES)
			return false;
		for (j = 0; j < i; j++)
			if (m->block == s->members[j].block && m->lane == s->members[j].lane)
				return false;
	}
	return true;
}

static bool vfl_members_match(const struct apple_sftl_vfl_source *s, const u8 *data)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s->members); i++) {
		const u8 *entry = data + 0x100 + i * 4;
		const struct apple_sftl_vfl_member *m = &s->members[i];

		if (get_unaligned_le16(entry) != m->block || entry[2] != m->lane || entry[3])
			return false;
	}
	return true;
}

static u8 *vfl_reference(struct vfl_page *p, unsigned int lane)
{
	return p->data + 0x200 + lane * 32;
}

/* The selected checkpoint and every older table it names must outlive erase. */
static int vfl_source_pages(struct vfl_rotation *r)
{
	const struct apple_sftl_vfl_source *s = r->source;
	struct vfl_page *cp = &r->checkpoint;
	unsigned int lane, page;
	int ret;

	ret = vfl_read_copies(r, s->group, s->page, cp);
	if (ret)
		return ret;
	if (!vfl_tag_matches(cp, VFL_GLOBAL, s->age) ||
	    !vfl_members_match(s, cp->data) ||
	    get_unaligned_le16(cp->data + 10) != VFL_LANES ||
	    get_unaligned_le16(cp->data + 14) != 2086 ||
	    memchr_inv(cp->data + 0x400, 0, sizeof(cp->data) - 0x400))
		return -EOPNOTSUPP;
	for (lane = 0; lane < VFL_LANES; lane++) {
		u8 *field = vfl_reference(cp, lane);
		u16 ref = get_unaligned_le16(field);

		page = ref & 0x7fff;
		if (ref >> 15 != s->group || page >= s->page ||
		    memchr_inv(field + 2, 0, 30))
			return -EOPNOTSUPP;
		ret = vfl_read_copies(r, s->group, page, &r->tables[lane]);
		if (ret)
			return ret;
		if (!vfl_tag_matches(&r->tables[lane], lane, s->age + (s->page - page)))
			return -EBADMSG;
	}
	/* A full bank can end with a table update after its last global save. */
	for (page = s->page + 1; page < VFL_PAGES; page++) {
		ret = vfl_read_copies(r, s->group, page, &r->first);
		if (ret)
			return ret;
		lane = r->first.oob[1];
		if (lane >= VFL_LANES ||
		    !vfl_tag_matches(&r->first, lane, s->age - (page - s->page)))
			return -EOPNOTSUPP;
		memcpy(&r->tables[lane], &r->first, sizeof(r->first));
		put_unaligned_le16((s->group << 15) | page, vfl_reference(cp, lane));
	}
	return 0;
}

/* Refuse a stale source receipt or an interrupted/replaced target bank. */
static int vfl_check_target(struct vfl_rotation *r)
{
	const struct apple_sftl_vfl_source *s = r->source;
	unsigned int page, lane;
	u32 first_age = 0;
	bool ended = false;
	int ret;

	for (page = 0; page < VFL_PAGES; page++) {
		ret = vfl_read_copies(r, !s->group, page, &r->first);
		if (ret)
			return ret;
		if (!memchr_inv(&r->first, 0xff, sizeof(r->first))) {
			ended = true;
			continue;
		}
		lane = r->first.oob[1];
		if (!page) {
			first_age = get_unaligned_le32(r->first.oob + 4);
			if (lane != VFL_GLOBAL)
				return -EOPNOTSUPP;
		}
		if (ended || (lane != VFL_GLOBAL && lane >= VFL_LANES) ||
		    (lane == VFL_GLOBAL && !vfl_members_match(s, r->first.data)) ||
		    first_age < page || first_age - page <= s->age + s->page ||
		    !vfl_tag_matches(&r->first, lane, first_age - page))
			return -EBADMSG;
	}
	return 0;
}

static int vfl_erase_target(struct vfl_rotation *r)
{
	unsigned int group = !r->source->group, i, page;
	int ret;

	for (i = 0; i < VFL_COPIES; i++) {
		ret = r->ops->erase(r->priv, &r->source->members[group * VFL_COPIES + i]);
		if (ret)
			return ret;
		for (page = 0; page < VFL_PAGES; page++) {
			ret = vfl_read(r, group, i, page, &r->first);
			if (ret)
				return ret;
			if (memchr_inv(&r->first, 0xff, sizeof(r->first)))
				return -EBADMSG;
		}
	}
	return 0;
}

static int vfl_program(struct vfl_rotation *r, unsigned int page, struct vfl_page *p,
		       u8 lane, u32 age)
{
	unsigned int group = !r->source->group, i;
	int ret;

	vfl_tag(p->oob, lane, age);
	for (i = 0; i < VFL_COPIES; i++) {
		ret = r->ops->program(r->priv, &r->source->members[group * VFL_COPIES + i],
				      page, p->data, p->oob);
		if (ret)
			return ret;
		ret = vfl_read(r, group, i, page, &r->first);
		if (ret)
			return ret;
		if (memcmp(&r->first, p, sizeof(*p)))
			return -EBADMSG;
	}
	return 0;
}

int apple_sftl_vfl_rotate(struct apple_ppn *ppn, const struct apple_sftl_vfl_source *s,
			  const struct apple_sftl_vfl_ops *ops, void *priv)
{
	struct vfl_rotation *r;
	u32 age;
	unsigned int lane;
	int ret;

	if (!ppn || !ppn->ops || !ppn->ops->read_page || !s ||
	    !ops || !ops->erase || !ops->program)
		return -EOPNOTSUPP;
	if (ppn->buses != 2 || ppn->caus != 8 || ppn->blocks != 2088 || ppn->pages != 128 ||
	    !vfl_members_valid(s) || s->group > 1 || s->page >= VFL_PAGES ||
	    s->age > U32_MAX - s->page ||
	    s->age < BIT(31) + (VFL_PAGES - s->page) + VFL_LANES + 1)
		return -EOPNOTSUPP;
	r = kvzalloc(sizeof(*r), GFP_NOIO);
	if (!r)
		return -ENOMEM;
	r->ppn = ppn;
	r->source = s;
	r->ops = ops;
	r->priv = priv;
	age = s->age - (VFL_PAGES - s->page);
	ret = vfl_source_pages(r);
	if (!ret)
		ret = vfl_check_target(r);
	if (!ret)
		ret = vfl_erase_target(r);
	if (!ret)
		ret = vfl_program(r, 0, &r->checkpoint, VFL_GLOBAL, age);
	for (lane = 0; !ret && lane < VFL_LANES; lane++) {
		ret = vfl_program(r, lane + 1, &r->tables[lane], lane, age - lane - 1);
		if (ret)
			break;
		put_unaligned_le16((!s->group << 15) | (lane + 1),
				   vfl_reference(&r->checkpoint, lane));
	}
	if (!ret)
		ret = vfl_program(r, VFL_LANES + 1, &r->checkpoint,
				  VFL_GLOBAL, age - VFL_LANES - 1);
	kvfree(r);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_vfl_rotate);

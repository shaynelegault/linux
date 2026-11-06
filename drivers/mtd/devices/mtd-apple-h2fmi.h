/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MTD_APPLE_H2FMI_H
#define MTD_APPLE_H2FMI_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define H2FMI_PARAMS_SIZE	512
#define H2FMI_CHUNKS		4
#define H2FMI_CHUNK_LO		1024
#define H2FMI_CHUNK_META		16
#define H2FMI_CHUNK_HI		3072
#define H2FMI_CHUNK_RAW		4112
#define H2FMI_PAGE_RAW		16448
#define H2FMI_PAGE_DATA		16384
#define H2FMI_PAGE_OOB		64
#define H2FMI_PAGE_SHIFT		14

#define PPN_ST_GOOD		0x40
#define PPN_ST_REFRESH		0x42
#define PPN_ST_RETIRED		0x45
#define PPN_ST_ERASED		0x49

struct h2fmi_geometry {
	u32 caus;
	u32 blocks;
	u32 pages;
	u32 page_bits;
	u32 block_bits;
	u32 cau_bits;
	u32 slc_pages;
};

/* Restrict discovery to the two measured PPN 1.5 package profiles. The
 * parameter response does not identify a PPN protocol revision. The existing
 * platform compatible continues to select that protocol explicitly.
 */
static inline int h2fmi_parse_geometry(const u8 *params, size_t len,
				       struct h2fmi_geometry *geometry)
{
	struct h2fmi_geometry g;
	u32 cell_bits, cells, raw_bytes;

	if (len != H2FMI_PARAMS_SIZE ||
	    memcmp(params, "PPN Device Info", sizeof("PPN Device Info") - 1))
		return -EINVAL;

	g.caus = get_unaligned_le32(params + 0x10);
	g.cau_bits = get_unaligned_le32(params + 0x14);
	g.blocks = get_unaligned_le32(params + 0x18);
	g.block_bits = get_unaligned_le32(params + 0x1c);
	g.pages = get_unaligned_le32(params + 0x20);
	g.slc_pages = get_unaligned_le32(params + 0x24);
	g.page_bits = get_unaligned_le32(params + 0x28);
	cell_bits = get_unaligned_le32(params + 0x2c);
	cells = get_unaligned_le32(params + 0x30);
	raw_bytes = get_unaligned_le32(params + 0x34);
	if (raw_bytes != H2FMI_PAGE_RAW || cell_bits != 1 || cells != 2)
		return -EOPNOTSUPP;
	if (g.caus == 2 && g.cau_bits == 4 && g.blocks == 1064 &&
	    g.block_bits == 11 && g.pages == 256 && g.slc_pages == 128 &&
	    g.page_bits == 8) {
		*geometry = g;
		return 0;
	}
	if (g.caus == 8 && g.cau_bits == 3 && g.blocks == 2088 &&
	    g.block_bits == 12 && g.pages == 128 && g.slc_pages == 64 &&
	    g.page_bits == 7) {
		*geometry = g;
		return 0;
	}
	return -EOPNOTSUPP;
}

static inline int h2fmi_page_row(const struct h2fmi_geometry *g,
				 unsigned int cau, unsigned int block,
				 unsigned int page, u32 *row)
{
	if (cau >= g->caus || block >= g->blocks || page >= g->pages)
		return -EINVAL;
	*row = page | block << g->page_bits |
	       cau << (g->page_bits + g->block_bits);
	return 0;
}

/* The VFL bootstrap's packed requests set the bit above the CAU field.
 * Keep this view separate from normal MTD offsets. Its address encoding
 * follows the saved firmware; live context selection is a separate step.
 */
static inline int h2fmi_slc_row(const struct h2fmi_geometry *g,
				unsigned int cau, unsigned int block,
				unsigned int page, u32 *row)
{
	u32 value;
	int ret;

	if (page >= g->slc_pages)
		return -EINVAL;
	ret = h2fmi_page_row(g, cau, block, page, &value);
	if (ret)
		return ret;
	*row = value | 1u << (g->page_bits + g->block_bits + g->cau_bits);
	return 0;
}

/* Unknown statuses must never lead to a data-out command or a good block. */
static inline int h2fmi_page_status(u8 status)
{
	switch (status) {
	case PPN_ST_GOOD:
	case PPN_ST_ERASED:
		return 0;
	case PPN_ST_REFRESH:
		return 1;
	default:
		return -EBADMSG;
	}
}

/* CMD 79 reports ready at bit 6 and program failure at bits 0 and 4. */
static inline int h2fmi_program_status(u8 status)
{
	return (status & BIT(6)) && !(status & (BIT(0) | BIT(4))) ? 0 : -EIO;
}

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L H2FMI + Hynix PPN NAND.
 *
 * The iPad mini 1 has two PPN packages, one on CE0 of each of the SoC's two
 * flash buses.  "PPN" means the package has its own controller: it does the
 * ECC and part of the bad-block work itself, so this is a *managed* NAND and
 * deliberately NOT a driver on top of drivers/mtd/nand/raw -- that framework
 * assumes the host computes ECC and would have to be fought the whole way.
 * What it is instead is a plain mtd_info per die with its own _read.
 *
 * Every register use and every command sequence here comes from iBEC's own
 * disassembly (H2fmi.c, H2fmi_ppn.c, fmiss_ppn.c) by way of the userspace tool
 * this is a port of, tools/nand/ppn.c, which read the whole NAND bit-for-bit
 * repeatably.  docs/research/p105-nand.md is the notebook.
 *
 * Geometry is read from each PPN package before registering any MTDs.
 * The supported packages have one CE per bus and either two or eight CAUs.
 * A page has 16448 bytes: four chunks of 1024 data bytes, 16 metadata
 * bytes and 3072 more data bytes.  The metadata is the FTL's, and becomes MTD's OOB; strip it and a
 * page is 16 KiB of data with 64 bytes of OOB. One MTD is exported per
 * CAU. Raw MTD offsets are independent of the iOS logical block addresses.
 *
 * This interface exposes reads through MTD. Logical-address translation and
 * allocation belong to the shared FTL above the physical page provider.
 * Optional programming accepts an erased full page and verifies its raw
 * data and metadata synchronously before reporting completion.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mtd/apple-ppn.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "mtd-apple-h2fmi.h"

#define H2FMI_BUSES		2
#define H2FMI_PAGE_MASK		(H2FMI_PAGE_DATA - 1)

/* The ADT's ce-bitmap is 0x101: CE0 on each bus, and nothing else.  Addressing
 * a CE with no chip behind it wedges the FMC in DDR -- it waits for a DQS that
 * never comes -- so this is never computed, only ever CE0. */
#define H2FMI_CE			0

/* FMI: the DMA/PIO side.  FMC: the NAND bus.  Offsets as iBEC uses them. */
#define FMI_CONFIG		0x00
#define FMI_CONTROL		0x04
#define FMI_STATUS		0x0c
#define FMI_INTEN		0x10
#define FMI_DATA		0x14	/* PIO FIFO; a read pops it */
#define FMI_DMA_STATUS		0x1c
#define FMI_PIO_CONFIG		0x34

#define FMC_ON			0x00
#define FMC_IF_CTRL		0x08
#define FMC_CE_CTRL		0x0c
#define FMC_RW_CTRL		0x10
#define FMC_CMD			0x14
#define FMC_ADDR0		0x18
#define FMC_ADDRNUM		0x20
#define FMC_DATANUM		0x24
#define FMC_INTMASK		0x40
#define FMC_STATUS		0x44
#define FMC_NAND_STATUS		0x48
#define FMC_STATUS_MASK		0x4c

/* The allowlist is the safety property, not a convenience: a sequence that is
 * not on it does not run.  Nothing here programs or erases. */
static const u8 h2fmi_read_only_ops[][2] = {
	{ 0x70, 0x00 },		/* read status */
	{ 0x92, 0x97 },		/* PPN device parameters */
	{ 0x77, 0x7d },		/* PPN operation status */
	{ 0x77, 0x00 },		/* PPN end of operation */
	{ 0x7a, 0x00 },		/* PPN data out */
	{ 0x0a, 0x37 },		/* PPN page read */
};

struct h2fmi_bus {
	void __iomem *fmi;
	void __iomem *fmc;
	void __iomem *ecc;
	struct mutex lock;	/* one command sequence on this bus at a time */
	u8 *page;		/* one raw page; only touched under lock */
	struct h2fmi_geometry geometry;
	bool failed;		/* block further I/O in this driver instance */
};

struct h2fmi_die {
	struct h2fmi *fmi;
	u8 bus;
	u8 cau;
};

struct h2fmi {
	struct device *dev;
	struct h2fmi_bus bus[H2FMI_BUSES];
	struct mtd_info *mtd;
	struct h2fmi_die *die;
	unsigned int ndies;
	struct platform_device *sftl;
};

/*
 * h2fmi_wait_done: poll until (reg & mask) == want, then write want back --
 * these bits are write-one-to-clear.  iBEC gives up after 100 ms and so do we.
 */
static int h2fmi_wait(struct h2fmi *fmi, void __iomem *base, u32 off,
		      u32 mask, u32 want)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(base + off, v, (v & mask) == want, 0, 100000);
	if (ret) {
		dev_err(fmi->dev, "timeout: reg +0x%x = 0x%08x, wanted 0x%x/0x%x\n",
			off, v, mask, want);
		return ret;
	}
	writel(want, base + off);
	return 0;
}

/*
 * FMC_CMD holds the first command byte, and the second one in bits 15:8.
 * Every command sequence passes through the read-operation allowlist.
 */
static int h2fmi_cmd_write(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op, u8 op2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(h2fmi_read_only_ops); i++)
		if (h2fmi_read_only_ops[i][0] == op &&
		    h2fmi_read_only_ops[i][1] == op2) {
			writel(op | op2 << 8, b->fmc + FMC_CMD);
			return 0;
		}

	dev_err(fmi->dev, "refusing NAND opcodes %02x %02x: not read-only\n", op, op2);
	return -EPERM;
}

static void h2fmi_clear_irqs(struct h2fmi_bus *b)
{
	writel(0, b->fmc + FMC_INTMASK);
	writel(0, b->fmi + FMI_INTEN);
	writel(0x31ffff, b->fmc + FMC_STATUS);
	writel(0xf, b->fmi + FMI_STATUS);
}

/* single command, FMC_RW_CTRL bit 0 */
static int h2fmi_cmd1(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op)
{
	int ret = h2fmi_cmd_write(fmi, b, op, 0);

	if (ret)
		return ret;
	writel(1, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 1, 1);
}

/* command, address bytes, second command: RW_CTRL 0xb */
static int h2fmi_cmd_addr_cmd(struct h2fmi *fmi, struct h2fmi_bus *b,
			      u8 op, u8 op2, u32 addr, int naddr)
{
	int ret;

	if (naddr < 1 || naddr > sizeof(addr))
		return -EINVAL;
	ret = h2fmi_cmd_write(fmi, b, op, op2);
	if (ret)
		return ret;
	writel(addr, b->fmc + FMC_ADDR0);
	writel((naddr + 7) & 7, b->fmc + FMC_ADDRNUM);
	writel(0xb, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 0xb, 0xb);
}

/*
 * h2fmi_ppn_get_operation_status + h2fmi_get_nand_status: 77 7D, then let the
 * FMC poll the status byte until it is ready.  iBEC waits on the interrupt;
 * this polls FMC_STATUS bit 5, as the userspace tool does.
 */
static int h2fmi_status(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *st)
{
	int ret;
	u32 v;

	ret = h2fmi_cmd_write(fmi, b, 0x77, 0x7d);
	if (ret)
		return ret;
	writel(3, b->fmc + FMC_RW_CTRL);
	ret = h2fmi_wait(fmi, b->fmc, FMC_STATUS, 3, 3);
	if (ret)
		return ret;

	h2fmi_clear_irqs(b);
	writel(0x100, b->fmi + FMI_INTEN);
	writel(readl(b->fmc + FMC_IF_CTRL) & ~0x100000u, b->fmc + FMC_IF_CTRL);
	writel(0x4040, b->fmc + FMC_STATUS_MASK);
	writel(0, b->fmc + FMC_DATANUM);
	writel(0x20, b->fmc + FMC_INTMASK);
	writel(0x50, b->fmc + FMC_RW_CTRL);

	ret = readl_poll_timeout(b->fmc + FMC_STATUS, v, v & 0x20, 0, 2000000);
	if (ret) {
		dev_err(fmi->dev, "status timeout: FMC+0x44 0x%08x FMI+0xc 0x%08x byte 0x%02x\n",
			v, readl(b->fmi + FMI_STATUS),
			(u8)readl(b->fmc + FMC_NAND_STATUS));
		writel(0, b->fmc + FMC_RW_CTRL);
		h2fmi_clear_irqs(b);
		return ret;
	}

	*st = readl(b->fmc + FMC_NAND_STATUS);
	writel(0, b->fmc + FMC_RW_CTRL);
	h2fmi_clear_irqs(b);
	return 0;
}

static int h2fmi_pio_read(struct h2fmi *fmi, struct h2fmi_bus *b,
			  u32 *dst, u32 bytes)
{
	u32 v;
	int ret;
	u32 i;

	ret = readl_poll_timeout(b->fmi + FMI_DMA_STATUS, v, v & 0x18, 0, 100000);
	if (ret) {
		dev_err(fmi->dev, "PIO timeout: FMI+0x1c = 0x%08x\n", v);
		return ret;
	}
	for (i = 0; i < bytes / 4; i++)
		dst[i] = readl(b->fmi + FMI_DATA);
	return 0;
}

/*
 * h2fmi_ppn_read_data_out: 7A, then the bytes by PIO in runs of at most 0x400.
 * FMI_PIO_CONFIG is (bytes << 8) | sectors.
 */
static int h2fmi_data_out(struct h2fmi *fmi, struct h2fmi_bus *b,
			  u8 *buf, u32 len)
{
	u32 off;
	int ret;

	ret = h2fmi_cmd1(fmi, b, 0x7a);
	if (ret)
		return ret;

	for (off = 0; off < len; ) {
		u32 n = min_t(u32, len - off, 0x400);
		u32 nw = n < 4 ? 4 : n & ~3u;

		writel(5, b->fmi + FMI_CONFIG);
		writel((nw << 8 & 0x7fc00) | 1, b->fmi + FMI_PIO_CONFIG);
		writel(3, b->fmi + FMI_CONTROL);
		ret = h2fmi_pio_read(fmi, b, (u32 *)(buf + off), nw);
		if (ret)
			return ret;
		off += nw;
	}
	return 0;
}

/*
 * h2fmi_device_reset without the PMGR pulse: stop the FMI and put the FMC back
 * the way it was.  iBoot leaves it in DDR (FMC_ON = 5) and that has to survive
 * every sequence, because getting back to DDR afterwards needs a reboot.
 */
static void h2fmi_device_reset(struct h2fmi_bus *b, u32 on, u32 if_ctrl)
{
	writel(6, b->fmi + FMI_CONTROL);
	writel(on, b->fmc + FMC_ON);
	writel(if_ctrl, b->fmc + FMC_IF_CTRL);
}

/* Caller holds b->lock. All exits release CE and restore inherited state. */
static int h2fmi_read_transaction(struct h2fmi *fmi, struct h2fmi_bus *b,
				  u8 op, u8 op2, u32 address, int naddr,
				  u8 *buffer, u32 length, u8 *status)
{
	u32 on, if_ctrl;
	int ret, end_ret;

	*status = 0;
	if (b->failed)
		return -EIO;
	on = readl(b->fmc + FMC_ON);
	if_ctrl = readl(b->fmc + FMC_IF_CTRL);
	if (on != 5)
		return -EIO;

	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_clear_irqs(b);
	ret = h2fmi_cmd_addr_cmd(fmi, b, op, op2, address, naddr);
	if (!ret)
		ret = h2fmi_status(fmi, b, status);
	/* An error status has no data phase. Device parameters require GOOD;
	 * normal pages also admit REFRESH and ERASED. Keep the status for the
	 * MTD reader and bad-block query without copying a stale page buffer.
	 */
	if (!ret && (op == 0x92 ? *status == PPN_ST_GOOD :
		     h2fmi_page_status(*status) >= 0))
		ret = h2fmi_data_out(fmi, b, buffer, length);

	/* Stop PIO before ending the operation, as in the parameter reader. */
	writel(6, b->fmi + FMI_CONTROL);
	end_ret = h2fmi_cmd1(fmi, b, 0x77);
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_device_reset(b, on, if_ctrl);
	if (ret || end_ret) {
		b->failed = true;
		return ret ? ret : end_ret;
	}
	return 0;
}

static int h2fmi_read_geometry(struct h2fmi *fmi, struct h2fmi_bus *b)
{
	u8 status = 0;
	int ret;

	mutex_lock(&b->lock);
	ret = h2fmi_read_transaction(fmi, b, 0x92, 0x97, 0, 1,
				     b->page, H2FMI_PARAMS_SIZE, &status);
	if (!ret && status != PPN_ST_GOOD)
		ret = -EIO;
	if (!ret)
		ret = h2fmi_parse_geometry(b->page, H2FMI_PARAMS_SIZE,
					   &b->geometry);
	mutex_unlock(&b->lock);
	return ret;
}

/* Strip the FTL's metadata out of a raw page: 4 x (1024 data, 16 meta, 3072
 * data) becomes 16384 of data and, if asked, 64 of OOB. */
static void h2fmi_split_page(const u8 *raw, u8 *data, u8 *oob)
{
	int c;

	for (c = 0; c < H2FMI_CHUNKS; c++) {
		const u8 *src = raw + c * H2FMI_CHUNK_RAW;

		if (data) {
			u8 *dst = data + c * (H2FMI_CHUNK_LO + H2FMI_CHUNK_HI);

			memcpy(dst, src, H2FMI_CHUNK_LO);
			memcpy(dst + H2FMI_CHUNK_LO,
			       src + H2FMI_CHUNK_LO + H2FMI_CHUNK_META,
			       H2FMI_CHUNK_HI);
		}
		if (oob)
			memcpy(oob + c * H2FMI_CHUNK_META,
			       src + H2FMI_CHUNK_LO, H2FMI_CHUNK_META);
	}
}

static int h2fmi_write_cmd(struct h2fmi *fmi, struct h2fmi_bus *b,
			   u8 op, u8 op2)
{
	if (!IS_ENABLED(CONFIG_MTD_APPLE_H2FMI_WRITE))
		return -EROFS;
	if (!((op == 0x8a || op == 0x17 || op == 0x79) && !op2))
		return -EPERM;
	writel(op | op2 << 8, b->fmc + FMC_CMD);
	return 0;
}

static int h2fmi_write_cmd1(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op)
{
	int ret = h2fmi_write_cmd(fmi, b, op, 0);

	if (ret)
		return ret;
	writel(1, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 1, 1);
}

/* Four row-address cycles follow the PPN program command. */
static int h2fmi_write_address(struct h2fmi *fmi, struct h2fmi_bus *b,
			       u32 row)
{
	int ret = h2fmi_write_cmd(fmi, b, 0x8a, 0);

	if (ret)
		return ret;
	writel(row, b->fmc + FMC_ADDR0);
	writel(3, b->fmc + FMC_ADDRNUM);
	writel(9, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 9, 9);
}

/* Same PIO FIFO used by iBEC's feature writer, with CONTROL=5 for data in.
 * Every chunk must finish before the next one or the program confirm.
 */
static int h2fmi_data_in(struct h2fmi *fmi, struct h2fmi_bus *b,
			 const u8 *buffer, u32 length)
{
	u32 offset, index, value;
	int ret;

	if (!length || length & 3)
		return -EINVAL;
	for (offset = 0; offset < length; ) {
		u32 bytes = min_t(u32, length - offset, 0x400);

		writel(5, b->fmi + FMI_CONFIG);
		writel((bytes << 8 & 0x7fc00) | 1,
		       b->fmi + FMI_PIO_CONFIG);
		writel(5, b->fmi + FMI_CONTROL);
		ret = readl_poll_timeout(b->fmi + FMI_DMA_STATUS, value,
					 value & 0x18, 0, 100000);
		if (ret)
			return ret;
		for (index = 0; index < bytes; index += 4)
			writel(get_unaligned_le32(buffer + offset + index),
			       b->fmi + FMI_DATA);
		ret = h2fmi_wait(fmi, b->fmi, FMI_STATUS, 2, 2);
		if (ret)
			return ret;
		offset += bytes;
	}
	return 0;
}

/* Program status uses 79, as in iBEC's write_bootpage path. */
static int h2fmi_write_status(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *status)
{
	u32 value;
	int ret = h2fmi_write_cmd1(fmi, b, 0x79);

	if (ret)
		return ret;
	h2fmi_clear_irqs(b);
	writel(0x100, b->fmi + FMI_INTEN);
	writel(readl(b->fmc + FMC_IF_CTRL) & ~0x100000u,
	       b->fmc + FMC_IF_CTRL);
	writel(0x4040, b->fmc + FMC_STATUS_MASK);
	writel(0, b->fmc + FMC_DATANUM);
	writel(0x20, b->fmc + FMC_INTMASK);
	writel(0x50, b->fmc + FMC_RW_CTRL);
	ret = readl_poll_timeout(b->fmc + FMC_STATUS, value,
				 value & 0x20, 0, 2000000);
	if (!ret)
		*status = readl(b->fmc + FMC_NAND_STATUS);
	writel(0, b->fmc + FMC_RW_CTRL);
	h2fmi_clear_irqs(b);
	return ret;
}

/* Caller holds the bus lock. A failed mutation permanently stops this bus. */
static int h2fmi_program_transaction(struct h2fmi *fmi, struct h2fmi_bus *b,
				     u32 row, const u8 *raw)
{
	u32 on, if_ctrl;
	u8 status = 0;
	int ret;

	if (!IS_ENABLED(CONFIG_MTD_APPLE_H2FMI_WRITE))
		return -EROFS;
	if (b->failed)
		return -EIO;
	on = readl(b->fmc + FMC_ON);
	if_ctrl = readl(b->fmc + FMC_IF_CTRL);
	if (on != 5)
		return -EIO;
	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_clear_irqs(b);
	ret = h2fmi_write_address(fmi, b, row);
	if (!ret)
		ret = h2fmi_data_in(fmi, b, raw, H2FMI_PAGE_RAW);
	if (!ret)
		ret = h2fmi_write_cmd1(fmi, b, 0x17);
	if (!ret)
		ret = h2fmi_write_status(fmi, b, &status);
	/* CMD 79 returns NAND write status, not the managed read result.
	 * iBEC 12H321 tests failure bits 0 and 4 after ready bit 6.
	 */
	if (!ret)
		ret = h2fmi_program_status(status);
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_device_reset(b, on, if_ctrl);
	if (ret) {
		b->failed = true;
		dev_err(fmi->dev, "PPN program row %08x: status %02x, error %d\n",
			row, status, ret);
	}
	return ret;
}

static void h2fmi_join_page(u8 *raw, const u8 *data, const u8 *oob)
{
	unsigned int chunk;

	for (chunk = 0; chunk < H2FMI_CHUNKS; chunk++) {
		u8 *dst = raw + chunk * H2FMI_CHUNK_RAW;
		const u8 *src = data + chunk * 4096;

		memcpy(dst, src, H2FMI_CHUNK_LO);
		memcpy(dst + H2FMI_CHUNK_LO, oob + chunk * H2FMI_CHUNK_META,
		       H2FMI_CHUNK_META);
		memcpy(dst + H2FMI_CHUNK_LO + H2FMI_CHUNK_META,
		       src + H2FMI_CHUNK_LO, H2FMI_CHUNK_HI);
	}
}

/* Canonical data and metadata form one complete PPN program unit. */
static int h2fmi_program_page(struct h2fmi *fmi, struct h2fmi_bus *b,
			      unsigned int cau, unsigned int block,
			      unsigned int page, const u8 *data, const u8 *oob)
{
	u32 row;
	u8 status, *expected;
	int ret;

	if (!IS_ENABLED(CONFIG_MTD_APPLE_H2FMI_WRITE))
		return -EROFS;
	if (!data || !oob)
		return -EINVAL;
	ret = h2fmi_page_row(&b->geometry, cau, block, page, &row);
	if (ret)
		return ret;
	/* Preserve the boot block and both package-reserved tail blocks. */
	if (!block || block >= b->geometry.blocks - 2)
		return -EPERM;
	expected = kmalloc(H2FMI_PAGE_RAW, GFP_KERNEL);
	if (!expected)
		return -ENOMEM;
	h2fmi_join_page(expected, data, oob);
	mutex_lock(&b->lock);
	ret = h2fmi_read_transaction(fmi, b, 0x0a, 0x37, row, 4,
				     b->page, H2FMI_PAGE_RAW, &status);
	if (ret)
		goto out;
	if (status != PPN_ST_ERASED ||
	    memchr_inv(b->page, 0xff, H2FMI_PAGE_RAW)) {
		ret = -EEXIST;
		goto out;
	}
	ret = h2fmi_program_transaction(fmi, b, row, expected);
	if (ret)
		goto out;
	ret = h2fmi_read_transaction(fmi, b, 0x0a, 0x37, row, 4,
				     b->page, H2FMI_PAGE_RAW, &status);
	if (ret || status != PPN_ST_GOOD ||
	    memcmp(b->page, expected, H2FMI_PAGE_RAW)) {
		b->failed = true;
		ret = ret ? ret : -EIO;
		dev_err(fmi->dev, "PPN readback row %08x: status %02x, error %d\n",
			row, status, ret);
		goto out;
	}
out:
	mutex_unlock(&b->lock);
	kfree(expected);
	return ret;
}

/* Publish MTD progress after the whole data/OOB unit passes readback. */
static int h2fmi_mtd_write_oob(struct mtd_info *mtd, loff_t to,
			       struct mtd_oob_ops *ops)
{
	struct h2fmi_die *die = mtd->priv;
	struct h2fmi *fmi = die->fmi;
	struct h2fmi_bus *b = &fmi->bus[die->bus];
	u64 page = (u64)to >> H2FMI_PAGE_SHIFT;
	u32 block = page >> b->geometry.page_bits;
	u32 pg = page & (b->geometry.pages - 1);
	int ret;

	ops->retlen = 0;
	ops->oobretlen = 0;
	if (!IS_ENABLED(CONFIG_MTD_APPLE_H2FMI_WRITE))
		return -EROFS;
	if (to < 0 || to >= mtd->size || to & H2FMI_PAGE_MASK ||
	    ops->mode != MTD_OPS_PLACE_OOB || ops->len != H2FMI_PAGE_DATA ||
	    ops->ooblen != H2FMI_PAGE_OOB || ops->ooboffs ||
	    !ops->datbuf || !ops->oobbuf)
		return -EINVAL;
	ret = h2fmi_program_page(fmi, b, die->cau, block, pg,
				 ops->datbuf, ops->oobbuf);
	if (!ret) {
		ops->retlen = H2FMI_PAGE_DATA;
		ops->oobretlen = H2FMI_PAGE_OOB;
	}
	return ret;
}

static int h2fmi_read_page_at(struct mtd_info *mtd, loff_t from,
			      u8 *data, u8 *oob, u8 *stp)
{
	struct h2fmi_die *die = mtd->priv;
	struct h2fmi *fmi = die->fmi;
	struct h2fmi_bus *b = &fmi->bus[die->bus];
	u64 page = (u64)from >> H2FMI_PAGE_SHIFT;
	u32 blk = page >> b->geometry.page_bits;
	u32 pg = page & (b->geometry.pages - 1);
	u32 row;
	u8 st = 0;
	int ret;

	if (from < 0 || from >= mtd->size || (from & H2FMI_PAGE_MASK))
		return -EINVAL;
	ret = h2fmi_page_row(&b->geometry, die->cau, blk, pg, &row);
	if (ret)
		return ret;

	mutex_lock(&b->lock);
	/* Supported PPN 1.5 packages require four row cycles, even when the
	 * fourth byte is zero. This is independent of the geometry's width.
	 */
	ret = h2fmi_read_transaction(fmi, b, 0x0a, 0x37, row, 4,
				     b->page, H2FMI_PAGE_RAW, &st);
	if (!ret && h2fmi_page_status(st) >= 0) {
		if (st == PPN_ST_ERASED &&
		    memchr_inv(b->page, 0xff, H2FMI_PAGE_RAW))
			ret = -EBADMSG;
		else
			h2fmi_split_page(b->page, data, oob);
	}
	mutex_unlock(&b->lock);

	*stp = st;
	return ret;
}

/* Canonical normal/context reads share the same complete transaction lock. */
static int h2fmi_ppn_read(void *priv, unsigned int bus, unsigned int cau,
			unsigned int block, unsigned int page, bool context,
			u8 *data, u8 *oob, u8 *status)
{
	struct h2fmi *fmi = priv;
	struct h2fmi_bus *b;
	u32 row;
	int ret;

	if (bus >= H2FMI_BUSES || !data || !oob || !status)
		return -EINVAL;
	b = &fmi->bus[bus];
	if (context)
		ret = h2fmi_slc_row(&b->geometry, cau, block, page, &row);
	else
		ret = h2fmi_page_row(&b->geometry, cau, block, page, &row);
	if (ret)
		return ret;

	/* Retired pages have no data phase. Never return a stale page buffer. */
	memset(data, 0, H2FMI_PAGE_DATA);
	memset(oob, 0, H2FMI_PAGE_OOB);
	mutex_lock(&b->lock);
	ret = h2fmi_read_transaction(fmi, b, 0x0a, 0x37, row, 4,
				     b->page, H2FMI_PAGE_RAW, status);
	if (!ret && h2fmi_page_status(*status) >= 0) {
		if (*status == PPN_ST_ERASED && memchr_inv(b->page, 0xff, H2FMI_PAGE_RAW))
			ret = -EIO;
		else
			h2fmi_split_page(b->page, data, oob);
	} else if (!ret && *status != PPN_ST_RETIRED) {
		ret = -EIO;
	}
	mutex_unlock(&b->lock);
	return ret;
}

static const struct apple_ppn_ops h2fmi_ppn_ops = {
	.read_page = h2fmi_ppn_read,
};

static int h2fmi_register_sftl(struct h2fmi *fmi)
{
	const struct h2fmi_geometry *g = &fmi->bus[0].geometry;
	struct apple_ppn provider = {
		.dev = fmi->dev,
		.ops = &h2fmi_ppn_ops,
		.priv = fmi,
		.buses = H2FMI_BUSES,
		.caus = g->caus,
		.blocks = g->blocks,
		.pages = g->pages,
	};

	/* This recovery implementation supports the measured 64 GB profile. */
	if (!IS_ENABLED(CONFIG_MTD_APPLE_SFTL) || g->caus != 8 ||
	    g->blocks != 2088 || g->pages != 128 || g->slc_pages != 64)
		return 0;
	fmi->sftl = platform_device_register_data(fmi->dev, "apple-sftl",
						 PLATFORM_DEVID_AUTO,
						 &provider, sizeof(provider));
	if (IS_ERR(fmi->sftl))
		return PTR_ERR(fmi->sftl);
	return 0;
}

/*
 * Turn the PPN's status byte into what MTD expects back from _read_oob: a
 * count of corrected bitflips, or a negative errno.
 *
 * The package corrects its own data and will not say how much it had to
 * correct, only whether the page wants rewriting -- so "wants rewriting"
 * becomes one bitflip, which is bitflip_threshold, which is what makes
 * mtd_read_oob() hand the caller -EUCLEAN.  That is the signal UBI scrubs on.
 * An erased page is not an error; a retired block is.
 */
static int h2fmi_bitflips(struct mtd_info *mtd, u8 st)
{
	int ret = h2fmi_page_status(st);

	if (ret < 0)
		dev_err_ratelimited(mtd->dev.parent,
				    "%s: page status 0x%02x\n", mtd->name, st);
	return ret;
}

/*
 * The whole read path.  mtd_read() builds an mtd_oob_ops and comes through
 * here too, so this has to cope with an unaligned start, any length, and
 * either buffer on its own -- a driver may implement _read or _read_oob but
 * not both, and _read_oob is the one that can carry the metadata.
 */
static int h2fmi_mtd_read_oob(struct mtd_info *mtd, loff_t from,
			      struct mtd_oob_ops *ops)
{
	size_t oobavail = mtd_oobavail(mtd, ops);
	size_t len = ops->len, ooblen = ops->ooblen;
	u8 *datbuf = ops->datbuf, *oobbuf = ops->oobbuf;
	u32 ooboffs = ops->ooboffs;
	u8 oob[H2FMI_PAGE_OOB];
	int maxbitflips = 0;
	u8 *page = NULL;
	int ret = 0;

	ops->retlen = 0;
	ops->oobretlen = 0;
	if (!len && !ooblen)
		return 0;

	while (len || ooblen) {
		loff_t base = from & ~(loff_t)H2FMI_PAGE_MASK;
		u32 skip = (u32)(from - base);
		bool direct = false;
		u8 *dst = NULL;
		u8 st = 0;
		int bf;

		/* A whole aligned page -- which is every page of a large read,
		 * and so nearly all of them -- is split straight into the
		 * caller's buffer.  The bounce is only for a partial page, and
		 * is only allocated if one turns up. */
		if (len) {
			if (!skip && len >= H2FMI_PAGE_DATA) {
				dst = datbuf;
				direct = true;
			} else {
				if (!page) {
					page = kmalloc(H2FMI_PAGE_DATA, GFP_KERNEL);
					if (!page) {
						ret = -ENOMEM;
						break;
					}
				}
				dst = page;
			}
		}

		ret = h2fmi_read_page_at(mtd, base, dst, oob, &st);
		if (ret) {
			/* MEMREAD retries ECC errors. No bytes were returned, so
			 * report a terminal error instead of repeating this page.
			 */
			if (ret == -EBADMSG)
				ret = -EIO;
			break;
		}

		bf = h2fmi_bitflips(mtd, st);
		if (bf < 0) {
			mtd->ecc_stats.failed++;
			ret = -EIO;
			break;
		}
		mtd->ecc_stats.corrected += bf;
		maxbitflips = max(maxbitflips, bf);

		if (len) {
			size_t n = min_t(size_t, len, H2FMI_PAGE_DATA - skip);

			if (!direct)
				memcpy(datbuf, page + skip, n);
			datbuf += n;
			len -= n;
			ops->retlen += n;
		}
		if (ooblen) {
			size_t n = min_t(size_t, ooblen, oobavail - ooboffs);

			memcpy(oobbuf, oob + ooboffs, n);
			oobbuf += n;
			ooblen -= n;
			ops->oobretlen += n;
			ooboffs = 0;
		}

		from = base + H2FMI_PAGE_DATA;
		if (from >= mtd->size)
			break;
	}

	kfree(page);
	return ret ? ret : maxbitflips;
}

/*
 * A retired block answers 0x45 on every page, which is the PPN saying so
 * directly -- there is no marker to interpret and no table to keep.
 */
static int h2fmi_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	u8 st = 0;
	int ret;

	ofs &= ~(loff_t)(mtd->erasesize - 1);
	ret = h2fmi_read_page_at(mtd, ofs, NULL, NULL, &st);
	if (ret)
		return ret;
	if (st == PPN_ST_RETIRED)
		return 1;
	return h2fmi_page_status(st) < 0 ? -EIO : 0;
}

static int h2fmi_setup_mtd(struct h2fmi *fmi, int idx)
{
	struct mtd_info *mtd = &fmi->mtd[idx];
	struct h2fmi_die *die = &fmi->die[idx];
	const struct h2fmi_geometry *g = &fmi->bus[0].geometry;

	die->fmi = fmi;
	die->bus = idx / g->caus;
	die->cau = idx % g->caus;

	mtd->priv = die;
	mtd->dev.parent = fmi->dev;
	mtd->owner = THIS_MODULE;
	mtd->name = devm_kasprintf(fmi->dev, GFP_KERNEL, "apple-nand%d", idx);
	if (!mtd->name)
		return -ENOMEM;

	mtd->type = MTD_NANDFLASH;
	/* PPN page reads expose the package metadata through MTD OOB. */
	mtd->flags = MTD_NO_ERASE;
	if (IS_ENABLED(CONFIG_MTD_APPLE_H2FMI_WRITE)) {
		mtd->flags |= MTD_WRITEABLE;
		mtd->_write_oob = h2fmi_mtd_write_oob;
	}
	mtd->erasesize = g->pages * H2FMI_PAGE_DATA;
	mtd->size = (u64)g->blocks * mtd->erasesize;
	mtd->writesize = H2FMI_PAGE_DATA;
	mtd->writebufsize = H2FMI_PAGE_DATA;
	mtd->oobsize = H2FMI_PAGE_OOB;
	mtd->oobavail = H2FMI_PAGE_OOB;
	/* The package corrects its own data and never says by how much, only
	 * whether the page wants rewriting.  So: one notional bit of strength,
	 * a threshold of one, and _read_oob returns 1 for "wants rewriting" --
	 * which mtd_read_oob() then reports to the caller as -EUCLEAN.  With
	 * ecc_strength left at 0 the core treats the device as having no ECC
	 * and throws that signal away. */
	mtd->ecc_strength = 1;
	mtd->bitflip_threshold = 1;

	/* _read_oob only: add_mtd_device() rejects a driver that has both. */
	mtd->_read_oob = h2fmi_mtd_read_oob;
	mtd->_block_isbad = h2fmi_mtd_block_isbad;

	return mtd_device_register(mtd, NULL, 0);
}

static int h2fmi_probe(struct platform_device *pdev)
{
	struct h2fmi *fmi;
	int i, ret;

	BUILD_BUG_ON(H2FMI_PAGE_DATA != (1 << H2FMI_PAGE_SHIFT));
	BUILD_BUG_ON(H2FMI_PAGE_RAW != 16448);

	fmi = devm_kzalloc(&pdev->dev, sizeof(*fmi), GFP_KERNEL);
	if (!fmi)
		return -ENOMEM;
	fmi->dev = &pdev->dev;

	/* Six regions, in the ADT's order: FMI, FMC, ECC for bus 0, then bus 1. */
	for (i = 0; i < H2FMI_BUSES; i++) {
		struct h2fmi_bus *b = &fmi->bus[i];

		b->fmi = devm_platform_ioremap_resource(pdev, i * 3 + 0);
		if (IS_ERR(b->fmi))
			return PTR_ERR(b->fmi);
		b->fmc = devm_platform_ioremap_resource(pdev, i * 3 + 1);
		if (IS_ERR(b->fmc))
			return PTR_ERR(b->fmc);
		b->ecc = devm_platform_ioremap_resource(pdev, i * 3 + 2);
		if (IS_ERR(b->ecc))
			return PTR_ERR(b->ecc);

		mutex_init(&b->lock);
		b->page = devm_kzalloc(&pdev->dev, H2FMI_PAGE_RAW, GFP_KERNEL);
		if (!b->page)
			return -ENOMEM;

		/* iBoot read the NAND to boot and leaves the block clocked and
		 * in DDR.  Nothing here turns it on: if iBoot did not, the
		 * state this driver would have to restore is not known. */
		dev_info(&pdev->dev, "bus %d: FMC_ON=0x%08x IF_CTRL=0x%08x\n",
			 i, readl(b->fmc + FMC_ON), readl(b->fmc + FMC_IF_CTRL));
	}

	/* Query both packages before publishing any part of the inventory. */
	for (i = 0; i < H2FMI_BUSES; i++) {
		ret = h2fmi_read_geometry(fmi, &fmi->bus[i]);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "bus %d: PPN discovery failed\n", i);
		if (i && memcmp(&fmi->bus[0].geometry, &fmi->bus[i].geometry,
				sizeof(fmi->bus[0].geometry)))
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "PPN package geometries disagree\n");
	}
	fmi->ndies = H2FMI_BUSES * fmi->bus[0].geometry.caus;
	fmi->mtd = devm_kcalloc(&pdev->dev, fmi->ndies, sizeof(*fmi->mtd), GFP_KERNEL);
	fmi->die = devm_kcalloc(&pdev->dev, fmi->ndies, sizeof(*fmi->die), GFP_KERNEL);
	if (!fmi->mtd || !fmi->die)
		return -ENOMEM;

	for (i = 0; i < fmi->ndies; i++) {
		ret = h2fmi_setup_mtd(fmi, i);
		if (ret) {
			while (--i >= 0)
				mtd_device_unregister(&fmi->mtd[i]);
			return ret;
		}
	}

	ret = h2fmi_register_sftl(fmi);
	if (ret) {
		for (i = 0; i < fmi->ndies; i++)
			mtd_device_unregister(&fmi->mtd[i]);
		return ret;
	}

	platform_set_drvdata(pdev, fmi);
	dev_info(&pdev->dev, "%u CAUs, %u blocks of %u KiB each\n",
		 fmi->ndies, fmi->bus[0].geometry.blocks,
		 fmi->mtd[0].erasesize / 1024);
	return 0;
}

static void h2fmi_remove(struct platform_device *pdev)
{
	struct h2fmi *fmi = platform_get_drvdata(pdev);
	int i;

	if (fmi->sftl)
		platform_device_unregister(fmi->sftl);
	for (i = 0; i < fmi->ndies; i++)
		mtd_device_unregister(&fmi->mtd[i]);
}

static const struct of_device_id h2fmi_of_match[] = {
	{ .compatible = "apple,s5l8940x-fmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, h2fmi_of_match);

static struct platform_driver h2fmi_driver = {
	.probe = h2fmi_probe,
	.remove = h2fmi_remove,
	.driver = {
		.name = "apple-h2fmi",
		.of_match_table = h2fmi_of_match,
	},
};
module_platform_driver(h2fmi_driver);

MODULE_DESCRIPTION("Apple S5L H2FMI / Hynix PPN NAND");
MODULE_LICENSE("GPL");

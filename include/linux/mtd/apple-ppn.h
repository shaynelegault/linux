/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MTD_APPLE_PPN_H
#define _LINUX_MTD_APPLE_PPN_H

#include <linux/types.h>

struct device;

#define APPLE_PPN_PAGE_SIZE 16384
#define APPLE_PPN_OOB_SIZE 64

/* All buffers have canonical data/OOB layout, including context reads. */
struct apple_ppn_ops {
	/* Success means the complete page and metadata passed exact readback. */
	int (*program_page)(void *priv, unsigned int bus, unsigned int cau,
			    unsigned int block, unsigned int page,
			    const u8 *data, const u8 *oob);
	int (*read_page)(void *priv, unsigned int bus, unsigned int cau,
			 unsigned int block, unsigned int page, bool context,
			 u8 *data, u8 *oob, u8 *status);
	/* Optional. Caller owns the complete physical erase extent in all views.
	 * Success means one normal-view CAU/block was erased and every byte
	 * of data and OOB read back blank. On failure the extent may have
	 * changed and must not be returned to the allocator.
	 */
	int (*erase_block)(void *priv, unsigned int bus, unsigned int cau,
			   unsigned int block);
};

struct apple_ppn {
	struct device *dev;
	const struct apple_ppn_ops *ops;
	void *priv;
	u16 buses;
	u16 caus;
	u16 blocks;
	u16 pages;
};

#endif

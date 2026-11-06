/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MTD_APPLE_PPN_H
#define _LINUX_MTD_APPLE_PPN_H

#include <linux/types.h>

struct device;

#define APPLE_PPN_PAGE_SIZE 16384
#define APPLE_PPN_OOB_SIZE 64

/* All buffers have canonical data/OOB layout, including context reads. */
struct apple_ppn_ops {
	int (*read_page)(void *priv, unsigned int bus, unsigned int cau,
			 unsigned int block, unsigned int page, bool context,
			 u8 *data, u8 *oob, u8 *status);
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

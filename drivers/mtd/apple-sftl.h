/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_H
#define _APPLE_SFTL_H

#include <linux/types.h>
#include <linux/mtd/apple-ppn.h>

struct apple_sftl;

struct apple_sftl_info {
	u32 logical_pages;
	u32 disk_pages;
	u32 nr_runs;
	u64 checkpoint_sequence;
	u64 sequence;
};

int apple_sftl_load(struct apple_ppn *provider, struct apple_sftl **out);
void apple_sftl_free(struct apple_sftl *sftl);
int apple_sftl_read(struct apple_sftl *sftl, u32 logical_page, u8 *data);
void apple_sftl_get_info(struct apple_sftl *sftl, struct apple_sftl_info *info);

#endif

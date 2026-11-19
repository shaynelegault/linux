/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_H
#define _APPLE_SFTL_H

#include <linux/types.h>
#include <linux/mtd/apple-ppn.h>

struct apple_sftl;
struct apple_sftl_checkpoint;

/* Internal checkpoint backend. reserve selects owned context blocks without
 * modifying media. install atomically publishes a native checkpoint, retires
 * the previous fallback, and returns independently read, caller-owned bytes.
 * An error may be indeterminate. The caller stops the instance on failure.
 */
struct apple_sftl_checkpoint_ops {
	int (*reserve)(void *priv, u32 *blocks, u32 *nr_blocks);
	int (*install)(void *priv, const struct apple_sftl_checkpoint *image,
		       struct apple_sftl_checkpoint **readback);
};

void apple_sftl_set_checkpoint_ops(struct apple_sftl *sftl,
				   const struct apple_sftl_checkpoint_ops *ops, void *priv);

struct apple_sftl_info {
	u32 logical_pages;
	u32 disk_pages;
	u32 nr_runs;
	u64 checkpoint_sequence;
	u64 sequence;
	u64 journal_sequence;
};

int apple_sftl_load(struct apple_ppn *provider, struct apple_sftl **out);
void apple_sftl_free(struct apple_sftl *sftl);
int apple_sftl_read(struct apple_sftl *sftl, u32 logical_page, u8 *data);
int apple_sftl_write(struct apple_sftl *sftl, u32 logical_page,
		     const u8 *data, unsigned int count);
int apple_sftl_flush(struct apple_sftl *sftl);
void apple_sftl_get_info(struct apple_sftl *sftl, struct apple_sftl_info *info);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_VFL_H
#define _APPLE_SFTL_VFL_H

#include <linux/mtd/apple-ppn.h>

struct apple_sftl;

struct apple_sftl_vfl_member {
	u16 block;
	u8 lane;
};

/* Location of the recovered checkpoint. Newer table records can follow it. */
struct apple_sftl_vfl_source {
	struct apple_sftl_vfl_member members[8];
	u32 age;
	u8 group, page;
};

int apple_sftl_vfl_source(struct apple_sftl *sftl,
			  struct apple_sftl_vfl_source *out);

/*
 * Only a backend with exclusive ownership of all eight SLC members, including
 * their physical aliases, may supply these operations. They must never remap
 * a bad member silently. On any error, reload before attempting more writes.
 */
struct apple_sftl_vfl_ops {
	int (*erase)(void *priv, const struct apple_sftl_vfl_member *member);
	int (*program)(void *priv, const struct apple_sftl_vfl_member *member,
		       u8 page, const u8 *data, const u8 *oob);
};

int apple_sftl_vfl_rotate(struct apple_ppn *ppn,
			  const struct apple_sftl_vfl_source *source,
			  const struct apple_sftl_vfl_ops *ops, void *priv);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MTD_APPLE_PPN_ACCESS_H
#define _LINUX_MTD_APPLE_PPN_ACCESS_H

#include <linux/errno.h>
#include <linux/rwsem.h>

struct apple_ppn_access {
	struct rw_semaphore lock;
	bool sftl_claimed;
};

static inline void apple_ppn_access_init(struct apple_ppn_access *access)
{
	init_rwsem(&access->lock);
	access->sftl_claimed = false;
}

/* Wait for every in-flight raw program and its readback before map discovery. */
static inline int apple_ppn_claim_sftl(struct apple_ppn_access *access)
{
	int ret = 0;

	down_write(&access->lock);
	if (access->sftl_claimed)
		ret = -EBUSY;
	else
		access->sftl_claimed = true;
	up_write(&access->lock);
	return ret;
}

/* devres releases the claim after failed probe or after the disk has drained. */
static inline void apple_ppn_release_sftl(void *data)
{
	struct apple_ppn_access *access = data;

	down_write(&access->lock);
	access->sftl_claimed = false;
	up_write(&access->lock);
}

/* Raw programs may still run on different buses when no map is cached. */
static inline int apple_ppn_begin_raw_write(struct apple_ppn_access *access)
{
	down_read(&access->lock);
	if (access->sftl_claimed) {
		up_read(&access->lock);
		return -EBUSY;
	}
	return 0;
}

static inline void apple_ppn_end_raw_write(struct apple_ppn_access *access)
{
	up_read(&access->lock);
}

#endif

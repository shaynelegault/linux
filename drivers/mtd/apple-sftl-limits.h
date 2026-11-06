/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_LIMITS_H
#define _APPLE_SFTL_LIMITS_H

#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/types.h>

/* The caller serializes begin/end with the provider transaction. */
struct apple_sftl_io_limit {
	u64 deadline_ns;
	u32 maximum_reads;
	u32 reads;
	bool failed;
};

static inline int apple_sftl_limit_start(struct apple_sftl_io_limit *limit,
				       u32 reads, u32 milliseconds, u64 now)
{
	u64 duration = (u64)milliseconds * 1000000ULL;

	if (!reads || !milliseconds ||
	    check_add_overflow(now, duration, &limit->deadline_ns))
		return -EINVAL;
	limit->maximum_reads = reads;
	limit->reads = 0;
	limit->failed = false;
	return 0;
}

static inline int apple_sftl_limit_check(struct apple_sftl_io_limit *limit, u64 now)
{
	if (limit->failed)
		return -EIO;
	if (now < limit->deadline_ns)
		return 0;
	limit->failed = true;
	return -ETIMEDOUT;
}

static inline int apple_sftl_limit_begin(struct apple_sftl_io_limit *limit, u64 now)
{
	int ret = apple_sftl_limit_check(limit, now);

	if (ret)
		return ret;
	if (limit->reads >= limit->maximum_reads)
		ret = -E2BIG;
	if (ret) {
		limit->failed = true;
		return ret;
	}
	limit->reads++;
	return 0;
}

static inline int apple_sftl_limit_end(struct apple_sftl_io_limit *limit,
				     int result, u64 now)
{
	if (!result && now >= limit->deadline_ns)
		result = -ETIMEDOUT;
	if (result)
		limit->failed = true;
	return result;
}

#endif

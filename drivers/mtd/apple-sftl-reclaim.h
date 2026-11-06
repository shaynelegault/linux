/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_SFTL_RECLAIM_H
#define _APPLE_SFTL_RECLAIM_H

#include <linux/types.h>

#define APPLE_SFTL_CHUNK 4096U
#define APPLE_SFTL_META 16U
#define APPLE_SFTL_STRIDE 8192U
#define APPLE_SFTL_SUBBLOCKS 2086U
#define APPLE_SFTL_LOGICAL_PAGES 15625016U
#define APPLE_SFTL_NO_ADDRESS ((u32)~0U)

struct apple_sftl;

struct apple_sftl_extent {
	u32 logical, length, provider;
};

struct apple_sftl_context_record {
	u8 meta[APPLE_SFTL_META];
	u8 data[APPLE_SFTL_CHUNK];
};

/* Owned whole-device map and unchanged non-map context payloads. */
struct apple_sftl_snapshot {
	struct apple_sftl_extent *runs;
	struct apple_sftl_context_record *opaque;
	u32 nr_runs, nr_opaque;
	u64 sequence, checkpoint_sequence, journal_sequence;
};

struct apple_sftl_checkpoint {
	struct apple_sftl_context_record *records;
	u32 count;
	u64 first_sequence, sequence;
};

int apple_sftl_snapshot(struct apple_sftl *sftl, size_t max_bytes,
			struct apple_sftl_snapshot **out);
void apple_sftl_snapshot_free(struct apple_sftl_snapshot *snapshot);
int apple_sftl_checkpoint_build(const struct apple_sftl_snapshot *snapshot,
				const u32 *blocks, u32 nr_blocks,
				u32 max_records, struct apple_sftl_checkpoint **out);
void apple_sftl_checkpoint_free(struct apple_sftl_checkpoint *checkpoint);
int apple_sftl_context_meta(u8 *meta, u8 subtype, u64 sequence,
			    u16 index, u16 entries);

/* A blank receipt describes an extent checked in full by the caller. */
struct apple_sftl_gc_block {
	u32 block;
	u16 lanes;
	bool blank, closed, reserved;
	u64 last_sequence;
};

struct apple_sftl_gc_copy {
	u32 logical, length, source, destination;
};

enum apple_sftl_gc_stage {
	APPLE_SFTL_GC_COPY,
	APPLE_SFTL_GC_CHECKPOINT,
	APPLE_SFTL_GC_RECOVERY,
	APPLE_SFTL_GC_VERIFIED,
	APPLE_SFTL_GC_FAILED,
};

struct apple_sftl_gc;

/* These routines plan and validate supplied bytes. They perform no media IO. */
int apple_sftl_gc_plan(const struct apple_sftl_snapshot *snapshot,
		       const struct apple_sftl_gc_block *victim,
		       const struct apple_sftl_gc_block *destination,
			u32 max_records, size_t max_bytes, struct apple_sftl_gc **out);
void apple_sftl_gc_free(struct apple_sftl_gc *gc);
enum apple_sftl_gc_stage apple_sftl_gc_stage(const struct apple_sftl_gc *gc);
const struct apple_sftl_gc_copy *apple_sftl_gc_next_copy(const struct apple_sftl_gc *gc);
int apple_sftl_gc_copy_done(struct apple_sftl_gc *gc,
			    const struct apple_sftl_gc_copy *receipt, u64 sequence);
int apple_sftl_gc_checkpoint(struct apple_sftl_gc *gc, const u32 *blocks,
			     u32 nr_blocks, struct apple_sftl_checkpoint **out);
int apple_sftl_gc_checkpoint_done(struct apple_sftl_gc *gc,
				  const struct apple_sftl_checkpoint *readback);
int apple_sftl_gc_recovered(struct apple_sftl_gc *gc,
			    const struct apple_sftl_snapshot *recovered);

#endif

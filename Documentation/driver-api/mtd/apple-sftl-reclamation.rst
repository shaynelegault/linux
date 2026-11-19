.. SPDX-License-Identifier: GPL-2.0-only

Apple SwissPPNFTL checkpoint and reclamation draft
=================================================

The measured 64 GB PPN layout shares one FTL across the logical partitions.
A Linux partition therefore provides no exclusive ownership of a physical
block. Reclamation must preserve the recovered map for the whole device.

The block writer allocates from verified blank blocks. An optional, verified
``erase_block`` provider operation lets the allocator also reclaim obsolete
blocks. With a complete native accounting policy the allocator can also stage
a checkpoint in the alternate context block and relocate live data.
P105 H2FMI does not enable physical erase or supply this policy. When verified
blank blocks run out on that profile, a write returns ``-ENOSPC``.

Reclaiming an obsolete block
----------------------------

The allocator records the blocks referenced by the saved checkpoint before
replaying the journal. It requires a single visible checkpoint generation
for erase eligibility. If an older generation is also visible, its fallback
map can still own blocks. An obsolete candidate must be absent from both
the saved map and the active map, including the logical keys outside the
exported disk. The allocator excludes VFL and FTL context blocks, the
bootstrap range and blocks with unknown active-lane flags. It loads the
medium again and compares the whole recovered map and its sequence numbers
with the active instance. On the measured native layout the saved accounting
must already mark an obsolete donor dirty-free with zero valid pages. A
closed data block that has lost its last reference is still ineligible for
immediate erase while its saved role is closed data.

The allocator reads the native footer and checks its runs against the
logical key and sequence in every data record. Its highest declared
sequence must precede the first record in the checkpoint. Even an obsolete
write or trim after the checkpoint can be needed for contiguous replay.

For an eligible block, the provider erases one active lane at a time.
The allocator checks every normal-view data and OOB byte for the erased
value before it hands the block to the waiting write. An indeterminate
erase or failed blank verification stops I/O on that writer instance.
Synthetic KUnit media exercise exhaustion, reclamation, reuse and cold
recovery through the block writer's allocation path.

Checkpoint images
-----------------

``apple_sftl_snapshot()`` takes an owned copy of the whole recovered range
map. It rereads the selected context's non-map records under the map lock,
checks their saved stamps and preserves their payloads. The caller supplies
a memory budget. Changed media stops further reads through that instance.

``apple_sftl_checkpoint_build()`` emits a bounded array of canonical 4 KiB
records. The metadata follows the 13G36 IOFlashStorage-730 formatter at
``0x81092408``: type 0x1f, subtype, a 48-bit sequence, an array index/count,
and a trailing 0xffffffff. The base record's decoded context-block list is
updated. Mapping records use subtype 4 with the native cursor/range encoding.
On the measured native format, the encoder pads the range stream to a
multiple of 64 records with subtype-4 padding. Each padding record
consumes a sequence value. Full logical coverage alone cannot identify the
last durable page of a native save.

The encoder recomputes the valid logical-page count of every superblock and
the associated global statistic (ID 9) from the whole planned map. Closing
a block or replaying a closed footer also updates its subtype-5 user
sequence statistics. Each footer run supplies one sample.

For a qualified closed donor, native finalization changes its class from
3 to 9 and clears its read count. A subsequent statistics refresh transfers
one block from the data count to the free count. The native checkpoint
writer can serialize the previous cached counts before that refresh. A
policy must establish the point of serialization as well as the allocation,
stream and wear transitions before allowing the first relocation write.
The native formatter also queries its provider for two statistics payloads
(IDs 255 and 254) on each save.

Saving and recovering a native context
--------------------------------------

The measured two-block format names its active and alternate contexts in the
base record. The writer verifies exclusive ownership of both and checks the
old generation's complete extent and the alternate's erased contents. It
programs the new context in 16 KiB units, reads back each page's data and
metadata, then cold-loads the medium with both generations visible. The
expected checkpoint sequence, target location, mapping and lane ownership
must agree. It erases the old context one lane at a time, verifies it blank
and cold-loads again before erasing the data donor.

Recovery selects a complete generation and replays later writes. A partially
written native map batch can expose sequence values without revealing how
many further values the interrupted operation reserved. Recovery from that
state is read-only. When an interrupted retirement leaves older context
fragments on some lanes, the complete newer generation must own the old
site and the remaining records must be provably older. Ambiguous fragments,
conflicting metadata and unaccounted journal gaps prevent recovery.

Relocating live data
--------------------

``apple_sftl_gc_plan()`` consumes a whole-device snapshot plus descriptions
of a closed victim and a fully checked blank destination. It rejects reserved
blocks, references into the victim's footer or inactive space, a destination
still referenced by the map, insufficient destination capacity and plan
buffers exceeding the caller's budget. A source with an allocation
floor above the last replayed record is excluded under the current recovery
contract. Copies retain the logical keys while moving their provider
addresses. Destination space leaves eight pages for a future native summary.

With checkpoint and erase support, allocation can use the last verified blank
block to compact a closed source still referenced by the saved checkpoint.
The allocator checks the source footer and data records before writing,
accepts unflagged type 1 data for relocation, and budgets enough destination
space and sequence numbers for the incoming write. The public KUnit tests
exercise the normal write path with native-format context pages, synthetic
media and a test accounting policy.

* ``apple_sftl_gc_relocate()`` reads each planned source key and checks its
  metadata. A copy gets a new physical write sequence and keeps the original
  user sequence's high 32 bits from its data tag. It assembles four 4 KiB
  records per program unit and repeats the first key when padding is needed.
  It checks that each destination page is blank, programs it, then compares
  all data and OOB bytes with readback.
* The writer reserves context blocks before copying. Each installed metadata
  and payload byte must match the submitted image.
* A fresh recovery snapshot must contain the planned whole-device mapping
  and the new checkpoint sequence. Equivalent coalescing is accepted. The
  allocator checks ownership and lane assignments again before erasing the
  source. The pending write uses the free tail of the destination, and the
  verified erased source becomes the next spare.

Standalone planner callers supply the blank, reserved and lane descriptions.
At runtime the allocator derives these from its VFL and context state and
from a scan of the candidate. Exposing the erase operation requires the
caller to own the complete physical erase extent across all affected views.

A copy I/O failure or mismatched receipt moves the transaction to its failed
state. A victim with live data or required replay history must survive
until a new checkpoint is durably installed and recovered. VERIFIED records
successful checks against the supplied checkpoint and recovery receipts.
Native accounting and physical mutation still require qualification.

Native VFL bank rotation
------------------------

VFL recovery uses two context groups with four copies per group on this
profile. The native rotation routine at ``0x810a1b68`` checks references into
the target group before reuse. On its healthy path, it erases the target
members in SLC view and writes an initial ff/ff checkpoint retaining references
to the old lane tables. It then copies those tables, updates the references
and writes a final ff/ff checkpoint. The old source bank survives this path.
The initial write at 0x810a1e5a and final write at 0x810a1e34 are separate
persisted steps. The healthy-path executor reads and verifies all four copies of
each source page, incorporates table updates after the last complete
checkpoint, verifies target erasure, and reads back the initial checkpoint,
all 16 tables and the final checkpoint. It uses separately qualified SLC
erase/program callbacks. Physical P105 supplies neither callback. VFL bank
rotation is independent of alternating the normal-view SwissPPNFTL context
blocks.

A fallback to older VFL replicas requires a complete agreeing quartet,
validated references and unchanged addressable ownership. Recovery reports
ambiguous media or unsupported FTL geometries and restricts degraded media
to reads. Scrub lists, bad-member replacement and physical aliasing require
additional protocol and hardware evidence.

Offline execution of the pinned native iBEC erase queue establishes queued
67/D7 commands, final-row 6A/D7, 79 polling and 77/7D completion. The native single-block
wrapper at 0x9ff09a0c shifts the block coordinate by the package's page-address
bit count. The 13G36 provider builder at 0x8109de94 also supplies CAU and view
fields. Both saved parameter copies agree: page bits 7, block bits 12 and CAU
bits 3. A normal block's page-0 row is ``(cau << 19) | (block << 7)``; the SLC
view adds bit 22. The normal view reports 128 pages per block. Physical erase
effects, the SLC alias relationship, timeout, retirement and partial completion
across CAUs remain unqualified. The H2FMI implementation issues the
single-row command and checks the erased status, data and OOB on all 128
normal-view pages. Enabling its callback on hardware requires a qualified
physical erase extent, exclusive ownership across all affected views, and
device recovery and interruption checks.

The source research began with `Cascadia's NAND notes
<https://github.com/rzxvx/Project-Cascadia/blob/c20b85ebb951c704d7e3a664c378dde8276e3928/docs/research/p105-nand.md>`_.
The private native differential fixtures retain source hashes and execute
against synthetic RAM. Public tests contain synthetic media only.

.. SPDX-License-Identifier: GPL-2.0-only

Apple SwissPPNFTL checkpoint and reclamation draft
=================================================

The measured 64 GB PPN layout shares one FTL across the logical partitions.
A Linux partition therefore provides no exclusive ownership of a physical
block. Reclamation must preserve the recovered map for the whole device.

The block writer allocates from verified blank blocks. An optional, verified
``erase_block`` provider operation lets the allocator also reclaim obsolete
blocks. A checkpoint backend with atomic installation and retirement of the
previous fallback additionally permits relocation of live data. P105 H2FMI
provides neither facility on the current hardware profile. When verified
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
with the active instance.

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
updated. Other payload bytes survive unchanged. Mapping records use subtype
4 with the native cursor/range encoding. Padding brings the image to full
16 KiB pages and consumes sequence values through subtype-4 padding records.
The resulting bytes round-trip through the current recovery decoder.

Opaque statistics, superblock state, user-sequence and read-count payloads
still describe the source checkpoint. Preserving them prevents accidental
data loss during format work; a native writer must update the fields whose
meaning depends on relocation. These images have not been qualified for
native checkpoint installation.

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

With both callbacks installed, allocation can use the last verified blank
block to compact a closed source still referenced by the saved checkpoint.
The allocator checks the source footer and data records before writing,
accepts unflagged type 1 data for relocation, and budgets enough destination
space and sequence numbers for the incoming write. The public KUnit tests
exercise this through the normal write path with a synthetic checkpoint
backend.

* ``apple_sftl_gc_relocate()`` reads each planned source key and checks its
  metadata. It assembles four 4 KiB records per program unit and repeats the
  first key when padding is needed. It checks that each destination page is
  blank, programs it, then compares all data and OOB bytes with readback.
* The checkpoint backend reserves context blocks before copying. After the
  copies complete it installs the new checkpoint, retires the previous
  fallback, and returns independent readback. Every metadata and payload
  byte must match the submitted image.
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
Native accounting and VFL rotation still require qualification.

Outstanding native protocol work
-------------------------------

VFL recovery uses two context groups with four copies per group on this
profile. The native rotation routine at ``0x810a1b68`` checks references into
the target group before reuse. On its healthy path, it erases the target
members in SLC view and writes an initial ff/ff checkpoint retaining references
to the old lane tables. It then copies those tables, updates the references
and writes a final ff/ff checkpoint. The old source bank survives this path.
The initial write at 0x810a1e5a and final write at 0x810a1e34 are separate
persisted steps. Torn-copy selection, retirement and replacement paths still
require tracing before a runtime writer can reproduce them.

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

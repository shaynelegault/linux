.. SPDX-License-Identifier: GPL-2.0-only

Apple SwissPPNFTL checkpoint and reclamation draft
=================================================

The measured 64 GB PPN layout shares one FTL across the logical partitions.
A Linux partition therefore provides no exclusive ownership of a physical
block. Reclamation must preserve the recovered map for the whole device.

The code in ``apple-sftl-reclaim.c`` serializes checkpoint images and plans
transactions from supplied snapshots and completion receipts. It registers
no media operation. The block device retains its existing access policy.

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

Offline transaction
-------------------

``apple_sftl_gc_plan()`` consumes a whole-device snapshot plus descriptions
of a closed victim and a fully checked blank destination. It rejects reserved
blocks, references into the victim's footer or inactive space, a destination
still referenced by the map, insufficient destination capacity and a working
plan buffers exceeding the caller's budget. A source with an allocation
floor above the last replayed record is excluded under the current recovery
contract. Copies retain the logical keys while
moving their provider addresses. Destination space leaves eight pages for
a future native summary.

The transaction advances through explicit receipt checks:

* Each copy receipt must match its planned logical range, source,
  destination and final sequence. The caller must issue and verify that IO.
* A complete checkpoint image is built only after the copy receipts arrive.
  Its readback must match every metadata and payload byte.
* A fresh recovery snapshot must contain the planned whole-device mapping
  and the new checkpoint sequence. Equivalent coalescing is accepted.

The supplied blank, reserved and lane descriptions are trusted inputs. They
must come from authoritative VFL and context ownership checks before any
future destructive IO can use a plan. The present receipt checks cannot
establish those physical ownership facts.

A mismatched receipt moves the transaction to its failed state. Even a victim
with zero current references must pass checkpoint and cold-recovery stages,
since the current replay contract can still require its obsolete history.
The final VERIFIED state means that these supplied receipts passed. It does
not certify native accounting or VFL commit durability.

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
across CAUs remain unqualified. The provider exposes no erase
callback. Enabling physical reclamation requires resolving those semantics
and qualifying the exact candidate with device recovery and interruption
checks.

The source research began with `Cascadia's NAND notes
<https://github.com/rzxvx/Project-Cascadia/blob/c20b85ebb951c704d7e3a664c378dde8276e3928/docs/research/p105-nand.md>`_.
The private native differential fixtures retain source hashes and execute
against synthetic RAM. Public tests contain synthetic media only.

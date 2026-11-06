.. SPDX-License-Identifier: GPL-2.0-only

Apple PPN VFL/SwissPPNFTL reader
==============================

The ``apple-sftl`` driver recovers the logical medium above Apple's PPN flash
mapping. H2FMI supplies complete 16 KiB pages and their 64 metadata bytes
through an aggregate provider. Context reads use the package's separate SLC
address view. The controller serializes each complete transaction.

Supported format
----------------

This implementation supports the measured 64 GB Hynix PPN profile with two
buses and eight CAUs per bus. Each CAU has 2,088 blocks of 128 pages. Profile
selection uses discovered geometry. Other layouts return ``-EOPNOTSUPP``.

Recovery selects four agreeing VFL checkpoint copies and checks each
replicated lane table. A VFL superblock has an 8,192-unit address stride;
units are 4 KiB. Blocks with fewer active lanes have an unused address gap
which the reader rejects.

SFTL subtype-4 context fragments describe the saved logical ranges. Recovery
validates the complete context sequence, then applies later data and trim
records in sequence order. Completed blocks carry summaries at their ends.
Open blocks are scanned completely because an interrupted stripe may leave
an erased lane before another completed lane. Summary allocation spans also
preserve the next sequence floor when it exceeds the last visible record.

The map has 15,625,016 entries, including sixteen internal keys. The block
device exposes 15,625,000 entries, or 64,000,000,000 bytes, with 512-byte sector
reads. Linux's disk name is ``applesftlN``. LwVM and filesystem interpretation
happen above this logical provider. Encrypted partition contents remain
ciphertext.

Failure and lifetime rules
--------------------------

The disk is read-only and the provider has no programming or erase operation.
Malformed context copies and incomplete journal sequences prevent disk
registration. A refresh status requires a writer and is reported as an error.
A failed runtime read stops subsequent reads from this map instance.

H2FMI unregisters the child before releasing its controller state. The block
frontend freezes and drains requests during removal. Its request buffer is
allocated before the disk becomes visible, while blocking requests serialize
access to that buffer and the provider.

The reader has finite operating limits. ``max_page_reads`` defaults to
32,768 physical page requests and ``max_io_time_ms`` defaults to ten minutes
from probe. Both are read-only module parameters and must be positive.
Recovery and subsequent block reads consume the same budget. A limit check
runs before each provider call; completion also checks the deadline, so an
existing bounded provider transaction is the maximum overrun. Any provider
failure or exhausted limit permanently stops that reader instance. These
limits support bounded read-only qualification; a mounted root must not rely
on an instance after its operating budget expires.

Tests
-----

``tools/testing/kunit/configs/apple_sftl.config`` enables the synthetic medium
tests. They generate VFL replicas and lane tables, plus an SFTL checkpoint
followed by completed and open data blocks. Tests cover trim ordering and
allocation floors. Corrupt replicas and sequence gaps must return an error
with no disk or usable map. Test inputs contain no captured device bytes.

The format rules were ported from Orchard's P105 Python reference
(``ftl_map.py``, ``ftl_load.py`` and ``ftl_log.py``), which was checked against
native iOS logical reads. A kernel object compile or synthetic test result
does not establish physical-controller timing or qualify a new device layout.

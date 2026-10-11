---
name: arm3-cell-backend-measurement
description: three specs (blkzcell/ztmpfs/fusion) then arm 3 wired into zbench -- cells beat zsmalloc on RAM at equal ratio; caught that session 39's arm 2 was already cells
project: linux
previous: 2026-10-11_openwrt-refocus.md
---

# Mission Log: Writing the Three Specs, Then Measuring Arm 3

Part 25 of the tmpfs transparent-compression series.  Session 41
(2026-10-11).  Two halves: a design session that produced three specs
(the block layer, tmpfs, and their fusion), then an implementation
session that wired arm 3 (ztmpfs on cells) into the rationalization
bench and measured it for the first time.  The brief, in the user's
words: "i want to explore a pure block layer for zcell.  enabling
quick compression and discard for fs that are not aware, such as
fat32 and ext2", "blkzcell is not about tmpfs/ram.  only about optimal
usage of sd/mmc", and at the close "so.  i want to implement the
arm3".

Nothing is posted upstream.  One rig commit (`ca08c60`); the specs are
local files.

## What We Did

### 1. Bootstrapped from the record

Read the three most recent logs (38-40): s1 branch green, rational-
ization bench arms 1-2 measured, OpenWrt fleet context, the refocus on
tmpfs and tmpfs+zcell.  Established the parked-item boundary: the
session-35 verdict killed zcells-as-RAM-blockdev on random writes; log
40 parked the NAND/thin-storage zcell layer until arm 3 was measured.

### 2. Designed the block layer (blkzcell) as a parallel track

Framed, per the user, as **optimal usage of the SD/MMC card**, not of
RAM.  On flash every term of the session-35 verdict flips: media
writes are ~1-10 MB/s so the staging cost is noise; compression buys
card lifetime + write-amplification + effective capacity; discard
synthesis fixes FAT32/ext2 never TRIMming; write combining turns the
random-overwrite pattern the card's FTL hates into sequential appends.

Design converged over several refinements:

- **Compressed at rest (architecture A)**, FTL-like, adaptive bounded
  RAM working set.
- **Segment = one erase group**, fixed-extent and erase-aligned
  outside (so discard == one TRIM), byte-packed unaligned inside, size
  varies in content but not extent.
- **Record format**: `struct zcell_rec { u8 type; u8 len[3]; le32
  crc32c; u8 data[] }` -- 8-byte header, LE explicit bytes (not C
  bitfields, which are implementation-defined), CRC over the whole
  `type+len` prefix plus payload so a torn header and torn payload are
  one detection path.  A record never straddles a segment.
- **Pure append-only, one open segment.**  Overwrite = new append +
  `live_bytes` decrement on the old segment; the old bytes are dead in
  place, awaited by the cleaner.
- **Flush seals** (append-only must hold from the card's perspective;
  rewriting an erase group is the FTL amplification we exist to
  prevent).  Flush = seal segment + index dump + card-cache flush, so
  after fsync both data and mapping are durable and replay is bounded
  by un-fsynced writes only.
- **Recovery**: monotonic 64-bit seq + linear header scan + drop
  detection.  The "find the smaller seq, the previous was last" rule
  is correct for every head position because seq never resets; a torn
  checkpoint at the head falls back to the previous good one by
  construction.
- **GC**: cost-benefit cleaner, relocate-unchanged, discard the
  emptied erase group; WA = 1/(victim live fraction), leveled
  device-wide; free-segment watermark + over-provisioning bound it.

Written to `specs/01_BLKZCELL_NAND_LAYER.md`.

### 3. Wrote the ztmpfs spec against the built s1 branch

The v1 slice is real and green on `upstream/s1`, so spec 02 documents
both the code and the RFC target: the `SWP_SHMEM_COMPRESS` + `{id,gen}`
xarray encoding (the load-bearing decision and the biggest upstream
review risk), two-counter refcounting (slots vs refs), the write-end
skip-list guard, stats-as-gauges, and the 3-option surface.  Motivation
is the measured arm 1/2 table.  Arm 3 is a later phase.  Written to
`specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md`.

### 4. Wrote the fusion spec -- arm 3 in full

The fusion (zcell adapted to RAM only, bolted onto ztmpfs) is the
**simplest** of the three because in tmpfs the page-cache xarray
*is* the mapping: no index dumps, no checkpoint, no recovery, no
discard.  The defining table is "why flash machinery is absent in
RAM".  It adds no new allocator code -- `mm/cell_store.c` is reused
verbatim; only the `use_cells` dispatch is wired.  Written to
`specs/03_ZTMPFS_ZCELL_FUSION.md`.

### 5. Wired arm 3 into the bench -- and found the arm-2 bug

Before wiring, verified how the backend is chosen.  On the feature
branch `fill_sb()` **derives a cell order from the filesystem size
when `cell=` is absent** (size=2G -> order 8).  Arm 2's mount passed
no `cell=`, so **arm 2 was already measuring cells, not zsmalloc** --
which retroactively relabels session 39's "arm 2 zsmalloc" numbers as
the cell backend, and would have collapsed arms 2 and 3 into the same
measurement.

Fix: force arm 2 to `cell=0` (true zsmalloc), make arm 3 `cell=64K`.
Arm 3 now differs from arm 2 ONLY in the storage backend.  Also fixed
the hardcoded `arm=2` label in arm_stats and the `arm > 2` bound in
main.  Committed to the rig (`ca08c60`).

### 6. Measured all three arms (first arm-3 run ever)

Non-KASAN measurement kernel (rig bzImage byte-identical to the tree
build, md5-verified), munin x6, 5 reps, CRC-verified.  The backend
split is provable from dmesg: arm 2 `cell_order=0`, no "cell storage
enabled"; arm 3 `order=4` + "cell storage enabled".

## What We Learned

### Technical

- **The default cell-order derivation is a silent backend switch.**
  `cell=` absent != zsmalloc; it means "derive from size".  Any caller
  that wants zsmalloc must say `cell=0` explicitly.  This is exactly
  the kind of resolved-state surprise the series has been burned by
  before (the stats-gauges trap, twice).
- **On flash the session-35 verdict inverts wholesale.**  The 2.7x
  random-write penalty was measured *in RAM* against zram; against
  1-10 MB/s media the same staging cost is invisible, and the win
  moves from "~0.1% RAM" to "bytes never written to the card".
- **Monotonic-seq + circular-scan recovery needs no superblock to keep
  current.**  A torn checkpoint is safe by construction because
  append-only puts the torn record at the head, where the scan stops.
- **A record cannot straddle a segment.**  GC relocates whole records;
  a straddling record's halves could be reclaimed at different times.
  This bounds the index dump to one segment (asserted), which is fine
  because the extent+RLE-encoded index compresses to KBs.
- **Bitfields are implementation-defined; on-media formats are not.**
  The `u8 type / u24 len` header must be explicit little-endian bytes
  so the layout is fixed by the format, not the compiler ABI.

### Process

- **Spec-first caught the arm-2 bug.**  Writing spec 03 (which states
  "arm 2 = zsmalloc backend, `cell=0`") forced the check of what arm 2
  actually resolved to, which is where the mislabel surfaced.  Had we
  wired arm 3 by copy-paste, arms 2 and 3 would have measured the same
  backend and the "payload wins" conclusion would have been an
  artifact.
- **Three specs in one sitting is a decision-density ceiling.**  Each
  spec leaned on the previous one's settled decisions; writing them
  back-to-back while the design was hot meant zero re-derivation.  The
  cross-reference tables (flash-vs-RAM) are what keep the three
  coherent as one system.
- **Verify the booted kernel is the measurement kernel, not the gate
  kernel.**  The rig's stored `.config` was a stale KASAN copy; the
  authoritative non-KASAN config was in the tree.  md5-matching the
  rig bzImage against the tree build settled it.  KASAN quarantine
  corrupts exactly the numbers this bench reports.

## What We Decided

- **blkzcell is a parallel track; arm 3 stays the series payload.**
  The session-35 RAM verdict does not extend to flash geometry.
- **Architecture A** (compressed at rest), adaptive bounded RAM,
  segment = erase group, pure append-only, seal-on-flush, monotonic-seq
  recovery, cost-benefit GC with a watermark.
- **Arm 2 is zsmalloc (`cell=0`), arm 3 is cells (`cell=64K`)** --
  backend isolated as the single variable.
- **The three specs are the written design of record** for blkzcell,
  ztmpfs, and the arm-3 fusion.

## Rules Added

- **`cell=` absent derives an order; zsmalloc requires `cell=0`.**  Any
  bench or test comparing backends must pass `cell=` explicitly on
  both arms, or the default derivation silently merges them.

- **A backend-comparison bench must be verified from dmesg, not from
  the mount string.**  The mount string says what was asked for; dmesg
  says what was created ("cell storage enabled, order=N").

## What We'd Do Differently

- Would have read `fill_sb()`'s `cell=`-absent branch BEFORE trusting
  session 39's arm-2 label.  The derivation had been in the tree since
  10-07 (carve C3/C4/C5), four days before the bench ran, so the
  mislabel was live for the whole rationalization.

- Will price the cell_order axis explicitly (spec 03's improvement
  target 1) rather than picking 64K as the arm-3 point; a single order
  cannot say whether the cell win grows or shrinks at other orders.

## Files Changed

| File | Purpose |
|------|---------|
| `specs/01_BLKZCELL_NAND_LAYER.md` | block layer for SD/MMC: segments, records, recovery, GC |
| `specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md` | ztmpfs v1 against the built s1 branch |
| `specs/03_ZTMPFS_ZCELL_FUSION.md` | arm 3: cell backend on tmpfs, RAM-only |
| rig `tests/test_zbench.c` (commit `ca08c60`) | arm 3 wired, arm 2 forced to `cell=0` |

## Test Results / Benchmarks

All three arms green, one non-KASAN kernel, munin x6 (29.3 MB, 4,284
files), 5 reps, CRC-verified (crc=0 size=0 miss=0 every arm):

| metric | arm 1 tmpfs+zramswap | arm 2 zsmalloc | arm 3 cells |
|---|---|---|---|
| live-set RAM (held_kb) | 26.1 MB | 24.6 MB | **22.1 MB** |
| compressor RAM (ram_total) | (never compressed) | 18.3 MB | **16.8 MB (-8.6%)** |
| Shmem held | 38.3 MB | 3.4 MB | 4.5 MB |
| compressed? | **no** (zram_orig=4096) | 8,730 pg, 2.12:1 | 8,443 pg, 2.11:1 |
| untar (write) | 0.093 s | 0.214 s | 0.196 s |
| verify (read) | 0.114 s | 0.119 s | 0.134 s |
| rm (free) | 0.022 s | 0.023 s | 0.022 s |

Headline: **cells hold the same live set as zsmalloc with 8.6% less
compressor RAM at an identical ratio, and write slightly faster.**
Arm 1's compression remains hypothetical under no pressure (never
swapped, 38.3 MB uncompressed Shmem).  Caveat the series demands:
held_kb drifts ~+/-10 MB common-mode per boot, so the 2.5 MB arm2-vs-3
gap is inside the noise -- the honest number is the stable `ram_total`
gauge.  Single boot per arm; M-boot medians are the standing rigor
item.

## Next Steps

1. **Pressure pass** -- all three arms through the squeeze so the
   survival row reads survives-vs-OOM per arm, not just arm 1/2.
2. **M-boot wrapper** for medians + IQR across boots (the in-boot reps
   give spread; the RFC table needs boot medians).
3. **cell_order sweep** -- spec 03's improvement target 1; price
   whether the cell win grows or shrinks off 64K.
4. Standing s1-track items unchanged: memory-failure answer for
   `SWP_SHMEM_COMPRESS`, in-tree selftests, Sphinx docs, RFC assembly.
5. blkzcell track: Phase 1 (record format + RAM segment staging, no
   I/O) when the block layer is picked up.

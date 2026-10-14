---
name: two-table-bench-rationalization-and-ram-stress
description: benchmark the new two-table model on both axes -- the rationalization bench (why not zram, no pressure) and the RAM-stress fill bench (survival beyond RAM); only ztmpfs/zsmalloc survives the >RAM regime
project: linux
previous: 2026-10-13_two-table-phase4-encoding-removal.md
---

# Mission Log: Benching the New Model, Rationalization and RAM-Stress

Part 32 of the tmpfs transparent-compression series.  Session 48
(2026-10-13).  A measurement session -- no kernel code changed.  Now that
the two-table mapping (Phases 1-4) is complete, run the two benches that
matter on the new model: the rationalization bench (`test_zbench`) that
answers "why not just swapon zram?", and the RAM-stress fill bench
(`test_fill`) that exercises the survival regime the first bench cannot
reach.  Both on the non-KASAN kernel (RAM numbers require it, per the
session-39 rule).

The tree is clean; the in-tree kernel is back to KASAN.

## What We Did

### 1. The rationalization bench (small dataset, no pressure)

`test_zbench <arm> <reps>` benches one policy per boot on the munin
dataset (~4.9 MB, 714 files), 7 in-boot reps, 2G RAM.  Median of 7:

| arm | policy | untar (write) | verify (read) | rm |
|---|---|---|---|---|
| 1 | tmpfs + zram swap | 15 ms | 22 ms | 5 ms |
| 2 | ztmpfs / zsmalloc | 31 ms | 26 ms | 6 ms |
| 3 | ztmpfs / cells (64K) | 30 ms | 24 ms | 5 ms |

All CRC ok, `fails=0`.  The compression ratio (arm 2, dataset resident):
5.96 MB -> 2.81 MB, ~2.1:1 (`comp_pages=1455`).

The finding that is the bench's whole reason to exist: **arm 1 (zram)
saved ~0 RAM.**  Its `swapfree_kb=524284` -- the zram device stayed
completely free.  With no memory pressure, zram swap never compressed a
page (it only swaps cold pages under pressure).  Arm 2/3 (ztmpfs)
compressed the dataset eagerly at write regardless of pressure, saving
~2 MB.  So the ~2x write cost of ztmpfs (31 vs 15 ms) is the
eager-compression CPU, and it buys RAM savings exactly where zram buys
nothing.

### 2. The RAM-stress bench (survival beyond RAM)

`test_fill <arm> 3072 ... 1` -- a 3G tmpfs on 2G RAM, write to ENOSPC
then a plain read-back.  This is the survival regime.

| arm | policy | outcome |
|---|---|---|
| 1 | tmpfs + zram swap (2G) | **OOM during fill** -- swap exhausted (`Free swap=0` of 2G), RAM full (~1.1 GB zram `zspages` + ~876 MB shmem) |
| 2 | ztmpfs / zsmalloc | **SURVIVED** -- wrote 3072 MB -> compressed to 1.6 GB (all 786432 pages, ~2:1), read back 3072 MB at 1.8 GB/s, CRC ok |
| 3 | ztmpfs / cells (64K) | OOM during fill -- cell-capacity limit (pre-existing, session 43) |

Only ztmpfs-on-zsmalloc survives.  zram swap needs a swap device to hold
the >RAM overflow; the 2G zram filled while ~876 MB of tmpfs pages were
still resident, and with both RAM and swap gone the OOM killer took the
writer -- swap-death thrash.  ztmpfs compresses the tmpfs pages in place
(no swap device), so 3.2 GB logical -> 1.6 GB in the pool -> fits in
RAM, and every byte reads back cleanly.

### 3. Checked the between-rep memory growth was not a leak

The ztmpfs arms in the rationalization bench showed `Shmem` growing
~5 MB/rep between reps and returning at umount.  Verified it is not a
leak: the built-in evict detector (`shmem_evict_inode`'s `pr_warn` on
leftover blocks) never fired (0x), and teardown showed `comp_pages=0`.
It is deferred inode eviction -- inodes cached after `rm -Rf`, freed at
umount; the bench has no memory pressure to run the shrinker.  One
unexplained asymmetry: arm 1's Shmem stayed flat across reps while the
ztmpfs arms accumulated (likely the compressed-inode eviction path
deferring differently).  Not a leak; noted for a possible follow-up.

## What We Learned

### Technical

- **The two benches measure different questions and both are needed.**
  The rationalization bench (small dataset) shows zram does nothing
  without pressure; the RAM-stress fill shows zram dies *with* pressure.
  Neither alone tells the story: the first says "ztmpfs compresses when
  zram won't," the second says "and that is the only reason it survives
  past RAM."

- **zram swap is bounded by the swap device; in-place compression is
  not.**  zram swap ran out (2G device full, ~876 MB still resident ->
  OOM).  ztmpfs/zsmalloc held 3.2 GB logical in a 1.6 GB pool with no
  separate swap capacity to exhaust.  That structural difference -- not
  a tuning difference -- is what separates the two in the survival
  regime.

- **The cell tier's 3G fill limit is real and unchanged** by the
  two-table work (it is a cell-capacity property, session 43).  Cells
  remain fine for correctness (KASAN suite) and smaller fills (1.5G), but
  not for the 3G/2G survival case.

- **Deferred inode eviction shows up as "memory not returned" in a
  no-pressure bench** and is easy to mistake for a leak.  The evict
  detector firing (or not) plus the teardown gauge is what distinguished
  them.

### Process

- **A bench that reports `fails=0` and clean CRC is the gate; the
  interesting signal is often in a side gauge** (arm 1's `swapfree` being
  fully free is the whole "why not zram" answer, not the timings).

- **The single-boot caveat matters and is still unaddressed.**  These are
  one boot with in-boot reps, not the M-boot median+IQR wrapper the logs
  have flagged since session 42.  The ratios (write ~2x, ~2:1
  compression, survival-vs-death) are robust; the absolute milliseconds
  are not quote-worthy yet.

## What We Decided

- **The new model's headline result is the survival one**: for tmpfs at
  or beyond RAM, eager in-place compression (ztmpfs/zsmalloc) survives
  where zram swap dies.  That is the sentence the RFC should lead with
  (the arm 1/2/3 rationalization table, per spec 02's Phase 6).

- **No code change from the benches.**  The rationalization numbers and
  the survival outcome match what the series predicted; the benches
  confirm the completed two-table model behaves as designed.

## Rules Added

- **A RAM bench must run on the non-KASAN kernel, and a throughput bench
  on a small dataset must not be read as a survival verdict.**  The
  survival question only exists in the >RAM fill; the small-dataset bench
  only answers the no-pressure rationalization question.

- **"Memory not returned after rm" in a no-pressure bench is deferred
  eviction until the evict detector says otherwise.**  Check the
  `pr_warn`-on-leftover-blocks detector and the teardown gauge before
  calling it a leak.

## What We'd Do Differently

- Would have built the M-boot median+IQR wrapper before quoting any
  timing.  It has been owed since session 42; every bench run since
  carries the single-boot caveat because of it.

- Would have run both benches back to back on one non-KASAN kernel
  instead of toggling KASAN off/on around each -- the toggling cost two
  extra full rebuilds for no benefit.

## Files Changed

None (measurement session).  Kernel and rig trees unchanged and clean.

## Test Results

Rationalization bench (`test_zbench`, 7 reps, fails=0, all CRC ok):

| arm | untar | verify | rm | compressed |
|---|---|---|---|---|
| 1 zram swap | 15 ms | 22 ms | 5 ms | none (swapfree full) |
| 2 zsmalloc | 31 ms | 26 ms | 6 ms | 5.96 -> 2.81 MB (~2.1:1) |
| 3 cells | 30 ms | 24 ms | 5 ms | similar |

RAM-stress fill (`test_fill 3072`, 3G/2G, read-back):

| arm | outcome |
|---|---|
| 1 zram swap | OOM -- swap exhausted (`Free swap=0`), RAM full |
| 2 zsmalloc | SURVIVED -- wrote 3072 MB -> 1.6 GB, read 3072 MB @ 1.8 GB/s, CRC ok |
| 3 cells | OOM during fill -- cell capacity (pre-existing) |

## Next Steps

1. **M-boot median+IQR wrapper** for the benches (owed since session
   42) so the timing ratios become quote-worthy.
2. **RFC assembly** (spec 02 Phase 6): lead with the arm 1/2/3
   rationalization table and the survival result.
3. Optional: the arm-1-vs-ztmpfs deferred-eviction asymmetry (not a
   leak; low priority).
4. Standing items unchanged: cell_order sweep, arm-3 capacity for large
   fills, the arm-3 rm/synchronous-free story at 64M.

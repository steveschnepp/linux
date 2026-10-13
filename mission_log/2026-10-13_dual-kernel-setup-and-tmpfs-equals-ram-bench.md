---
name: dual-kernel-setup-and-tmpfs-equals-ram-bench
description: keep two persistent kernels (KASAN + non-KASAN) so benches stop toggle-rebuilding, and add a tmpfs==RAM bench (no overcommit, real reclaim pressure); the cell tier now peers zsmalloc on large fills
project: linux
previous: 2026-10-13_cell-directory-scaling-and-size-ceiling.md
---

# Mission Log: Dual Kernels and the tmpfs==RAM Bench

Part 34 of the tmpfs transparent-compression series.  Session 50
(2026-10-13).  Two things: stop the KASAN/non-KASAN toggle-rebuild that
has cost a full kernel rebuild around every RAM bench, and add a bench at
the tmpfs == RAM size (no overcommit, but real memory pressure).  Plus a
comparison of the cell tier against its previous (pre directory-sizing)
iteration.

Source tree clean; the in-tree kernel is the KASAN one by convention.

## What We Did

### 1. Two persistent kernels, no more toggling

Every RAM/throughput bench needs the non-KASAN kernel (KASAN overhead
corrupts the numbers), and every correctness run needs KASAN -- so each
bench round was toggling CONFIG_KASAN and doing a full rebuild in each
direction.  Fixed by keeping two prebuilt kernels:

- The kernel build system only allows a parallel `O=` build from a
  pristine source ("The source tree is not clean"), so `make mrproper`
  cleared the in-tree build (artifacts only; source untouched).
- Two `O=` dirs now hold the two configs: `out/kasan` (CONFIG_KASAN=y)
  and `out/nokasan` (KASAN off).  Both built (`-j6` each, in parallel).
- `scripts/sync_kernel.sh [kasan|nokasan]` copies a prebuilt bzImage
  into the rig instantly.

Rebuild both after a source change with
`make O=out/{kasan,nokasan} -j12 bzImage`.  A bench round is now a
`sync_kernel.sh` + a QEMU run -- zero rebuild.

### 2. The tmpfs == RAM bench

`test_fill` is parameterised by size, so tmpfs == RAM is just
`TEST_FILLMB = RAM` (2G tmpfs on a 2G VM).  No overcommit, but filling
the tmpfs fills RAM, and the read-back decompresses the whole image
against the pool -- real reclaim pressure (the "both"-state cache drop)
without the OOM risk of the overcommit case.

| arm | write | compressed | read-back | CRC |
|---|---|---|---|---|
| 2 zsmalloc | 763 MB/s | 2.0 -> 1.02 GB (~2:1) | 1598 MB/s | ok |
| 3 cells (64K) | 685 MB/s | 2.0 -> 1.02 GB (~2:1) | 1906 MB/s | ok |

Both fill and read back cleanly at exact fit; compression leaves ~800 MB
free after filling 2 GB.  Cells read faster than zsmalloc here.

### 3. Cell tier vs its previous iteration

The meaningful delta is the directory-sizing fix (session 49):

- Before (session 48): 3G/2G cell fill **OOM'd** -- the directory was
  pinned at 256 cells (~16 MiB storage), so cells ran out long before
  the fill.
- After (session 49): directory derives from `size=` (~49152 cells for
  3G) -> fills and reads back 3072 MB cleanly, matching zsmalloc.

Throughput is otherwise comparable (cells ~10% slower write,
similar-or-faster read).  The fix turned cells from "dies on large
fills" into a peer of zsmalloc on the survival regime.

## What We Learned

### Technical

- **Parallel `O=` builds need a pristine source.**  The kernel refuses
  `make O=...` while in-tree artifacts exist, so two simultaneous
  config builds require `mrproper` first.  That is a one-time cost; after
  it, both kernels build and are swapped with a `cp`.

- **tmpfs == RAM is a distinct regime worth its own bench.**  It sits
  between the small-dataset rationalization bench (no pressure) and the
  overcommit fill (OOM risk): exact fit exercises reclaim on the
  read-back without the OOM confound, which is where the "both"-state
  cache-drop actually runs.

- **The cell tier's throughput is now competitive**, not just its
  capacity.  At tmpfs==RAM cells read ~19% faster than zsmalloc (1906 vs
  1598 MB/s) while writing ~10% slower -- so the choice between them is
  a workload question, not a "cells are slow" one.

### Process

- **The toggle-rebuild was pure waste and is gone.**  It cost a full
  kernel rebuild (minutes) around every RAM bench; now it is a `cp`.
  Worth doing the moment two variants are needed regularly.

- **Comparing to a "previous iteration" is only meaningful with the
  recorded numbers.**  The session-48 cell OOM was the baseline; no need
  to rebuild the old kernel to know the fix worked.

## What We Decided

- **KASAN is the synced-by-default kernel** (correctness dev); benches
  `sync_kernel.sh nokasan` first.  Both live under `out/`.

- **tmpfs==RAM joins the bench set** alongside the rationalization
  (small dataset) and RAM-stress (overcommit) benches -- three regimes,
  three questions.

## Rules Added

- **When two kernel variants are needed regularly, keep both as `O=`
  build dirs off a pristine source and swap with a sync script** -- never
  toggle the in-tree config back and forth.

- **A bench regime is defined by the tmpfs-size-to-RAM ratio**: small
  dataset (no pressure), tmpfs==RAM (exact fit, reclaim pressure), and
  tmpfs>RAM (overcommit, survival).  Name which one a number comes from.

## What We'd Do Differently

- Would have set the dual-kernel build up at session 39 (when the
  "RAM benches run non-KASAN" rule was established) instead of
  toggle-rebuilding for a dozen sessions.

## Files Changed

| File | Purpose |
|------|---------|
| `scripts/sync_kernel.sh` (rig, new) | copy a prebuilt kasan/nokasan kernel into the rig |
| `out/kasan/.config`, `out/nokasan/.config` (kernel, untracked) | the two persistent configs |
| (kernel source unchanged this session) | -- |

## Test Results

tmpfs==RAM bench (2G/2G, write + read-back, non-KASAN):

| arm | write | compressed | read-back | CRC |
|---|---|---|---|---|
| 2 zsmalloc | 763 MB/s | 2.0 -> 1.02 GB | 1598 MB/s | ok |
| 3 cells | 685 MB/s | 2.0 -> 1.02 GB | 1906 MB/s | ok |

Cell-vs-previous (3G/2G overcommit): session 48 OOM -> session 49
survives (directory sized from `size=`).

## Next Steps

1. RFC assembly (spec 02 Phase 6) -- the survival story now covers both
   backends across all three regimes.
2. The cell-size / cell-number sweep the user raised ("it depends on the
   workload") -- sweep cell_order x nr_cells against the three bench
   regimes to find per-workload optima.
3. Standing items unchanged: M-boot median+IQR wrapper, the arm-3
   rm/synchronous-free story at 64M.

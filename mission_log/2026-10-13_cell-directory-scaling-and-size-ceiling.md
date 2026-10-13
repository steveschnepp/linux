---
name: cell-directory-scaling-and-size-ceiling
description: size the cell directory from the mount (fixes the 256-cell capacity wall behind the 3G cell-fill OOM) and raise the cell-size ceiling to 16M; a 3G/2G cell fill now survives like zsmalloc
project: linux
previous: 2026-10-13_two-table-bench-rationalization-and-ram-stress.md
---

# Mission Log: Cell Directory Scaling and the 16M Ceiling

Part 33 of the tmpfs transparent-compression series.  Session 49
(2026-10-13).  A focused feature session, prompted by the RAM-stress
bench: arm 3 (cells) still OOM'd on the 3G/2G fill while zsmalloc
survived.  The user asked why 64K cells were limiting and whether a
bigger cell could be attached, then clarified the real want: the number
of cells should come from the mount size -- bigger mount, bigger cell
array.

Traced to a fixed default, not the block format.  Fixed it, and raised
the cell-size ceiling while here.  Everything is committed; the tree is
clean; the in-tree kernel is back to KASAN.

## What We Did

### 1. The capacity wall was the directory, not the block format

`cell_pool_create()` was passed `nr_cells = 0`, which falls back to
`CELL_DEFAULT_MAX_CELLS` (256).  So a cell-backed pool could never hold
more than 256 x cell_size of compressed blocks -- ~16 MiB with 64 KiB
cells.  A 3G tmpfs compresses to ~1.6 GB, so the fill ran out of cells
and OOM'd long before it filled.  That is the whole reason arm 3 died on
the RAM-stress bench while arm 2 (zsmalloc, unbounded pool) survived.

### 2. Size the directory from the mount

`shmem_comp_fill_sb()` now derives the directory capacity from the
mount's `size=` and the cell size:

```
nr_cells = size= / cell_size          /* ~2x headroom over the ~2:1 image */
  floor: CELL_DEFAULT_MAX_CELLS (256)
  cap:   SHMEM_COMP_CELL_MAX_CELLS (2M)
```

A bigger mount gets a bigger cell array.  `size=/cell_size` gives ~2x
headroom over the ~2:1 compressed image and covers COW churn (the store
path allocates the new block before the free path drops the old one, and
a freed block only returns its cell when the cell empties).  The floor
keeps small mounts at the legacy default; the cap bounds the directory
metadata (a slot is a `struct cell`, ~100 B) for absurdly large mounts
with tiny cells -- which the existing size-based cell-order derivation
avoids in practice by growing the cell instead.

### 3. Raise the cell-size ceiling to 16M

While here, raised `CELL_ORDER_MAX` from 4 MiB to 16 MiB and widened the
handle's offset field from 22 to 24 bits to match (16 MiB = 2^24).  A
cell is one `alloc_pages(order)` call, so an order above `MAX_PAGE_ORDER`
(the buddy's max; 10 = 4 MiB on this kernel) cannot come from a single
allocation without `CONFIG_ARCH_FORCE_MAX_ORDER`.  `cell_pool_create()`
now refuses those cleanly -- a pool that can never allocate its cells
would just fail every store and leave everything uncompressed, which is
a worse failure than refusing at mount.

## What We Learned

### Technical

- **The cell capacity limit was a directory default, not the block
  format.**  The bench made it look like a cell-size problem ("64K
  cells are limiting"), but the block format was fine -- the directory
  was hardcoded to 256 cells, so total storage was `256 x cell_size`
  regardless of the mount.  Big cells would have reduced the cell *count*
  but not raised the 256 *capacity*; sizing the array from the mount
  fixes the actual bound.

- **`nr_cells = 0` silently means 256.**  The `cell_pool_create` "zero
  means default" convention hid the fact that the tmpfs caller had no
  capacity derivation at all.  A default that caps a resource should be
  visible at the call site, not buried in the callee.

- **A >MAX_PAGE_ORDER cell is a hard allocation limit, not a pressure
  one.**  `alloc_pages(order)` for order > MAX_PAGE_ORDER returns NULL
  unconditionally, so a 16 MiB cell needs the arch config -- there is no
  "retry under pressure" path.  Refusing at pool-create is the honest
  behaviour.

### Process

- **The bench surfaced the bug; reading the caller confirmed it.**  The
  3G fill OOM pointed at "cells"; the `nr_cells = 0` at the call site
  was the answer in one grep.  Cheaper than trying a bigger cell first.

- **The user's "bigger mount, bigger cell array" was the right model**,
  and it is what the block-device backend already did (derive from
  disksize); the tmpfs path just never had it.

## What We Decided

- **Directory capacity derives from `size=`**, floored at the legacy
  default and capped at 2M cells.  The cell *order* derivation (already
  present) and the new *count* derivation now both scale with the mount.

- **16 MiB is the cell-size ceiling**, but it needs
  `CONFIG_ARCH_FORCE_MAX_ORDER >= 13` to actually allocate; on this
  kernel (MAX_PAGE_ORDER=10) cell=16M is refused cleanly at mount.  The
  default derivation never exceeds order 9, so it is unaffected.

## Rules Added

- **A capacity that comes from a default must be derived at the call
  site when the caller knows its bound.**  tmpfs knows `size=`; passing
  `0` (the callee's default) silently capped storage at 256 cells.

- **Refuse an unallocatable configuration at setup, not per-operation.**
  A pool whose cells cannot be allocated fails every store; refusing at
  pool-create is a clean mount-time failure instead.

## What We'd Do Differently

- Would have sized the directory from `size=` when the cell backend was
  first written -- the block-device backend already derived from
  disksize, and the tmpfs path's `nr_cells = 0` was an oversight that
  the 3G bench exposed months later.

## Files Changed

| File | Purpose |
|------|---------|
| `include/linux/cell_store.h` (`0d068dd12d4e`) | CELL_ORDER_MAX 4M -> 16M; SHMEM_COMP_CELL_MAX_CELLS (2M) |
| `include/linux/shmem_compress.h` (`0d068dd12d4e`) | offset field 22 -> 24 bits; encoding comment |
| `mm/shmem_compress.c` (`0d068dd12d4e`) | derive nr_cells from size= (floor 256, cap 2M) |
| `mm/cell_store.c` (`0d068dd12d4e`) | refuse order > MAX_PAGE_ORDER at pool-create |

## Test Results

| gate | result |
|---|---|
| build (KASAN) | 0 errors, 0 warnings |
| full KASAN suite (cell store/GC/index-race on size-derived directory) | exit 0, all ALL PASS |
| **3G/2G cell fill + read-back (arm 3)** | **wrote 3072 MB, read 3072 MB @ ~2.1 GB/s, CRC ok -- SURVIVES (was OOM)** |

The cell tier now matches zsmalloc on the survival bench.  Cell fill
throughput ~685 MB/s write vs zsmalloc's ~780 MB/s (cell metadata
overhead), both surviving.

## Next Steps

1. Re-run the RAM-stress bench across all three arms to confirm the
   full survival table with the fix (arm 1 zram still dies; arms 2+3
   both survive).
2. RFC assembly (spec 02 Phase 6) -- the survival story now covers both
   backends.
3. Standing items unchanged: M-boot median+IQR wrapper, cell_order
   sweep, the arm-3 rm/synchronous-free story at 64M.

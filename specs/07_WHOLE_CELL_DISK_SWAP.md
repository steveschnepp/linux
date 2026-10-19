# Whole-Cell Disk Swap Specification

## Overview

Add a third storage tier to ztmpfs: a cold, in-RAM cell is paged out
*whole* to a disk backing file and its RAM freed; it is paged back in
whole when any of its blocks is next read. The swap unit is the entire
cell, moved as raw bytes (a cell already holds compressed blobs, so the
swap path does no compression or decompression). The cell directory
(metadata) stays resident in RAM; only cell *data* leaves. This lets a
ztmpfs working set whose compressed footprint exceeds RAM page its cold
cells to cheap disk instead of OOMing.

## Motivation

ztmpfs compresses shmem pages into cells; the compressed cell is the
durable backing ("like a disk block"). Today that backing is RAM-only:
the cell is a buddy-allocated kernel page, and the kernel never swaps
kernel memory. So once compression headroom is exhausted, a large cold
working set has nowhere to go but OOM. The cell-swap design gives it a
disk tier.

The requirement (stated across the design session, see
`mission_log/2026-10-19_compressed-disk-spill-design.md`): page the
compressed backing to a swap device/file, keep it compressed, never
decompress on pageout, and do it at whole-cell granularity (raw), not
per-blob. Disk is cheaper than RAM, so per-cell alignment waste on disk
is acceptable.

## Goals

- A cold, non-active, in-RAM cell can be swapped out to a backing file
  (O_DIRECT), freeing its RAM (`pages_used` drops by the cell's pages).
- Reading a block from an on-disk cell transparently swaps the whole
  cell back into RAM first; the read is served unchanged.
- The cell directory stays resident; on-disk cells keep a directory
  slot + swap-extent record but hold no RAM.
- Freeing/evacuating a cell discards its backing extent (no stale disk
  copies).
- Default-off: with no backing configured, behaviour is byte-identical
  to today (a compile-clean no-op), so every phase bisects.

## Non-Goals

- Not a general swap replacement: only ztmpfs cell data pages to the
  backing; anon/shmem pages still use the kernel swap as today.
- No compression/decompression in the swap path (the cell is moved raw).
- No per-blob spill: the cell is the unit (see the mission log for why
  the ztmpfs blob/zswap model was rejected).
- No packed/multi-cell-per-extent on-disk format: one cell per extent,
  alignment waste accepted.
- No THP-large-swap reuse; the backing is a pool-owned file, not the
  kernel swap slot allocator (kernel memory is unswappable).
- No policy change to *which* pages get compressed (the aging pass
  already decides that); this spec only pages already-compressed cells.

## Design

### Backing store

The pool owns a backing file, opened at pool create when configured
(mount option `swapfile=<path>`, plus a size derived from the mount or
a `swapsize=` option). The file is `fallocate`d up front and opened
**O_DIRECT** so cell I/O bypasses the page cache -- buffered I/O would
pin the very RAM the swap frees (the reentrancy hazard that disqualified
swappable cells).

Space is managed as fixed cell-sized **extents**. A cell of order N is
`1 << N` pages, so one extent is `cell_size` bytes, aligned. The pool
keeps an in-RAM extent free-map (a bitmap over `nr_extents =
backing_size / cell_size`).

```c
struct cell_backing {
	struct file   *filp;        /* O_DIRECT backing file, or NULL */
	u64	     nr_extents;  /* extents in the file */
	unsigned long *free_map;    /* bitmap: 1 = extent free */
	struct mutex  map_lock;     /* protects free_map + extent I/O ordering */
	u64	     cell_size;   /* bytes per extent == bytes per cell */
};
```

I/O is `kernel_write`/`kernel_read` (O_DIRECT) at `extent * cell_size`,
length `cell_size`. A cell is page-aligned and physically contiguous, so
a single O_DIRECT transfer per direction suffices; the file offset is
extent-aligned, satisfying O_DIRECT's alignment requirement.

### Cell state

`struct cell` gains swap state. A cell is IN_RAM or ON_DISK, never both;
the transition is serialised per-cell.

```c
enum cell_state {
	CELL_IN_RAM = 0,   /* pages[] valid, base valid */
	CELL_ON_DISK,      /* pages[] NULL, backing extent holds the data */
};

struct cell {
	/* ...existing fields... */
	enum cell_state state;
	u64	     swap_extent;  /* backing extent index when ON_DISK */
	bool	     dirty;	       /* written since last swap-out */
	struct completion swap_comp; /* serialises concurrent swap-in */
};
```

`state`, `swap_extent` and `dirty` are guarded by `cp->lock`.
`swap_comp` serialises concurrent swap-in of the same cell (a second
reader waits for the first's swap-in to complete rather than issuing a
second O_DIRECT read).

### Swap-out (whole cell, raw)

Triggered from the aging workqueue (`shmem_comp_age_work` -> after a
maintain pass), which is process context and may sleep for I/O -- never
from the shrinker (reclaim context; I/O there risks recursion). The pool
exposes:

```c
/* Page @max out cold non-active IN_RAM cells. Returns cells swapped. */
unsigned int cell_pool_swap_out(struct cell_pool *cp, unsigned int max);
```

Selection: non-active, `state == CELL_IN_RAM`, `live_bytes > 0`, and
not currently a GC victim. (Non-active cells are content-immutable --
invariant #1 -- so no new block can land in a cell we are paging out.)
Prefer full cells first (biggest RAM win); the first cut may simply take
any eligible cell up to `max`.

Per cell:
1. Take `down_read(&c->lock)` to exclude a concurrent GC evacuation.
2. Under `cp->lock`: allocate an extent (clear a free bit under
   `map_lock`), record `swap_extent`, set `state = CELL_ON_DISK`, unlink
   from `cp->sparse` (so GC never harvests an on-disk cell).
3. `kernel_write` the cell's `cell_size` bytes to the extent (O_DIRECT),
   outside `cp->lock`.
4. `__free_pages(c->pages[0], cp->cell_order)`; clear `pages[0]`/`base`;
   subtract from `pages_used`. Drop the rwsem.

If the extent allocation or the write fails, roll back to IN_RAM (free
the extent if taken) and leave the cell resident.

### Swap-in (whole cell, raw)

`cell_load()` gains a swap-in prologue. If `state == CELL_ON_DISK`:

```c
static int cell_swap_in(struct cell_pool *cp, struct cell *c);
```

1. `reinit_completion(&c->swap_comp)` then take the swap-in role: under
   `cp->lock` re-check `state`; if still ON_DISK, mark an in-progress
   flag so a second concurrent loader waits on `c->swap_comp` instead of
   double-swapping.
2. `alloc_pages(cell_gfp(cp), cp->cell_order)` for the cell; under
   `cp->lock` install `pages[0]`/`base`, set `state = CELL_IN_RAM`,
   subtract the extent back to the free map, `complete(&c->swap_comp)`.
3. `kernel_read` the extent into the new pages (O_DIRECT), outside
   `cp->lock`. If the read fails, free the pages, restore ON_DISK, and
   return the error (the read is retried or fails the block load).

The caller then proceeds with the normal `memcpy` from `c->base + offset`.
A cell swapped back in rejoins the in-RAM pool (eligible for GC, and
re-swappable once cold again).

### Dirty tracking (write-after-swapin)

A non-active cell is immutable, so an IN_RAM cell that is ON_DISK's
source is never written while on disk. But a cell can be swapped in (for
a read) and then have a free block allocated into it *if it is (or
becomes) the active cell*. To keep the model simple: **the active cell is
never swapped out**, and a cell swapped in for a read is marked `dirty`
only if it is (or later becomes) active and is written. On the next
swap-out of a dirty cell the full cell is re-written (there is no
partial update: the swap image is always the whole cell). `dirty` is
therefore an optimization hint (skip swap-out of a just-written cell),
not a correctness requirement, because a cell's on-disk image is rewritten
in full on every swap-out.

### GC interaction

- `cell_pool_pick_victim()` skips `state == CELL_ON_DISK` cells (they
  hold no RAM to reclaim and are already cold).
- An on-disk cell is only GC'd after being swapped in: a read brings it
  resident, it becomes sparse-eligible, and a later pass evacuates it.
- Evacuation/empty-return of a cell frees its backing extent (see
  `cell_pool_return` and `cell_pool_release_victim` below).

### Free / evacuate / teardown

- `cell_free()` on an ON_DISK cell: decrement `live_bytes` (no RAM is
  held). If it reaches 0, free the extent and clear the slot. If it
  becomes sparse but non-empty, leave it on disk (disk is cheap); it is
  reclaimed after a swap-in.
- `cell_pool_return()` (empty cell -> buddy) also frees the extent if
  the cell had one.
- `cell_pool_release_victim()` frees the extent when it returns an
  emptied victim that had been on disk (evacuation discards the swap).
- `cell_pool_destroy()` frees every extent (closes the backing file);
  no swap image survives the pool.

### Locking

- `cp->lock` guards `state`, `swap_extent`, `dirty`, and the directory;
  it is never held across I/O (O_DIRECT read/write happen outside it).
- Per-cell rwsem (`c->lock`): swap-out takes `down_read` (excludes GC
  evacuation via `down_write`); `cell_load` already takes `down_read`.
  Swap-in installs pages under `cp->lock` and is serialised by
  `c->swap_comp` for concurrent loaders.
- `backing->map_lock` (mutex) guards the extent free-map; taken only for
  extent alloc/free, never across cell I/O ordering with `cp->lock`.
- Lock order: cell rwsem -> `cp->lock` (matches existing GC); `map_lock`
  is leaf.

### Mount option

`swapfile=<path>` (enables the backing; absent = no swap tier, current
behaviour) and `swapsize=<bytes>` (backing extent count; default derived
or a fixed multiple of the mount size). Parsed alongside the existing
options in `mm/shmem.c`; carried to `cell_pool_create()`.

## Build Changes

No new source files. `mm/cell_store.c` and `mm/shmem_compress.c` are
extended; the header `include/linux/cell_store.h` gains the backing and
cell-state declarations.

| Target | Change |
|--------|--------|
| `mm/cell_store.o` | +backing extent allocator, +cell state, +swap-in/out |
| `mm/shmem_compress.o` | +swapfile/swapsize opts, +aging hook to swap-out |
| `mm/shmem.o` | +mount-option parsing for swapfile/swapsize |

## File Manifest

| File | Action | Purpose |
|------|--------|---------|
| `specs/07_WHOLE_CELL_DISK_SWAP.md` | Create | This spec |
| `include/linux/cell_store.h` | Modify | `cell_backing`, `cell.state/swap_extent/dirty/swap_comp`, new APIs |
| `mm/cell_store.c` | Modify | extent alloc, O_DIRECT swap-in/out, state machine, free/GC/teardown extent cleanup |
| `include/linux/shmem_compress.h` | Modify | `swapfile`/`swapsize` opt plumbing into pool create |
| `mm/shmem.c` | Modify | parse `swapfile=`/`swapsize=` mount options |
| `mm/shmem_compress.c` | Modify | aging-pass hook calling `cell_pool_swap_out()`; pass backing opts to pool |

## Migration

Each phase compiles clean at default (no `swapfile=` -> no-op) and is
independently committable and bisectable.

### Phase 1: backing plumbing, no-op at default (est. small)

Add `struct cell_backing`, the extent free-map allocator, `cell.state`
(+ enum), `swap_extent`, `dirty`, `swap_comp`; `cell_pool_create`
accepts backing opts but with no `swapfile=` leaves `backing == NULL`
and every path identical to today. Compile gate 0/0; commit.

### Phase 2: swap-in / swap-out core (est. medium)

Implement `cell_swap_in()` and `cell_pool_swap_out()` with O_DIRECT
I/O; wire the state checks into `cell_load()` and skip ON_DISK in
`cell_pool_pick_victim()`. Gated behind `backing != NULL` (still absent
by default). Compile gate 0/0; commit.

### Phase 3: aging hook + free/GC/teardown extent cleanup (est. medium)

Call `cell_pool_swap_out()` from the aging workqueue; free extents in
`cell_free`/`cell_pool_return`/`release_victim`/`cell_pool_destroy`.
Compile gate + a boot test with `swapfile=` on a loop-backed file;
commit.

### Phase 4: mount option + validation (est. small)

Parse `swapfile=`/`swapsize=` in `mm/shmem.c`, plumb to pool create,
validate path/size. Compile gate; commit.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| O_DIRECT alignment/short-I/O handling on some filesystems | Med | Med | Cell and extent are page-aligned; loop/ext4 backings tested; fall back to resident on I/O error |
| Reclaim recursion (I/O allocating memory under pressure) | Low | High | Swap-out runs from the aging workqueue (process context), not the shrinker; O_DIRECT bypasses the backing page cache |
| Concurrent swap-in of one cell double-reads | Med | Med | `c->swap_comp` serialises loaders; re-check state under `cp->lock` |
| Swapping out a cell GC is evacuating | Low | High | swap-out takes `down_read(c->lock)`, GC holds `down_write`; pick_victim skips ON_DISK |
| Active-cell mutation during swap-out | Low | High | Active cell is never a swap-out candidate (invariant #1) |
| Backing file page cache re-pins RAM | Med | High | O_DIRECT on the backing file |

## Open Questions

- Backing-path provisioning: should `swapfile=` require the file to
  pre-exist, or may the pool create+`fallocate` it? (Decide in Phase 4;
  likely: pre-exist, size via `swapsize=`.)
- Swap-out policy: how many cells per aging pass, and a recency
  protection so a just-read cell is not immediately re-swapped? (First
  cut: a fixed `max` per pass, `dirty`-gated; refine with recency later.)
- Interaction with the watermark (specs/06): should a hot (RAM-pressure)
  flip suppress swap-out, or drive it? (Out of scope here; the aging
  cadence drives it.)

## References

- `mission_log/2026-10-19_compressed-disk-spill-design.md` -- the design
  session that produced this model (whole-cell, raw, in-RAM-or-on-disk).
- `specs/04_GC_REVERSE_INDEX.md` -- the GC/reverse-index this builds on.
- `specs/06_WATERMARK_RAM_TWO_PHASE.md` -- the watermark/aging cadence
  that triggers swap-out.
- `include/linux/cell_store.h` -- the cell invariants (#1 immutability of
  non-active cells makes whole-cell swap safe).

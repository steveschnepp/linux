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
- The cell directory stays resident; on-disk cells keep only a directory
  slot + a state flag (the backing extent is the cellid itself, 1:1) and
  hold no RAM.
- The backing is 1:1 with the cell directory and must be >= the pool's
  cell capacity (`nr_extents >= nslots`); a smaller backing is refused.
- Freeing/evacuating a cell frees only its RAM; its disk slot is
  permanent and overwritten on the next swap-out (no stale copy is ever
  read, because state gates it).
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

Space is a fixed set of cell-sized **extents**, mapped **1:1** to the
in-RAM cell directory: cell N lives at extent N (file offset
`N * cell_size`). A cell of order N is `1 << N` pages, so one extent is
`cell_size` bytes, aligned. Because every cell has a permanent slot,
there is no allocator, no free-map, and swap-out can never fail for
lack of space. The backing must hold at least one extent per directory
slot (`nr_extents >= nslots`, i.e. disk >= the pool's cell capacity);
a smaller backing is refused outright -- the tier exists to extend the
tmpfs past RAM, which needs disk >= capacity anyway. As a side benefit
the 1:1 mapping spreads disk writes across extents according to
per-cell churn (each extent is written only when its own cell pages
out), rather than concentrating them the way a first-fit allocator
would.

```c
struct cell_backing {
	struct file   *filp;        /* O_DIRECT backing file */
	u64	     nr_extents;  /* extents; >= pool->nslots */
	u64	     cell_size;   /* bytes per extent == bytes per cell */
};
```

I/O is `kernel_write`/`kernel_read` (O_DIRECT) at `cellid * cell_size`,
length `cell_size`. A cell is page-aligned and physically contiguous, so
a single O_DIRECT transfer per direction suffices; the file offset is
extent-aligned, satisfying O_DIRECT's alignment requirement.

### Cell state

`struct cell` gains swap state. A cell is IN_RAM or ON_DISK, never both;
the transition is serialised per-cell. The backing extent is the cellid
itself (1:1), so no per-cell disk offset is stored.

```c
enum cell_state {
	CELL_IN_RAM = 0,   /* pages[] valid, base valid */
	CELL_ON_DISK,      /* pages[] NULL, backing extent holds the data */
};

struct cell {
	/* ...existing fields... */
	enum cell_state state;
};
```

`state` is guarded by `cp->lock` and is stable under the cell rwsem
(swap-in and swap-out are the only writers, both under `down_write`).
Concurrent swap-in of one cell is serialised by that `down_write` (the
losers re-check state and find IN_RAM).

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
1. Take `down_write(&c->lock)` -- this excludes concurrent `cell_load`
   readers (which hold `down_read` across their memcpy) and a GC
   evacuation, and it serialises concurrent swap-ins of the same cell.
2. Under `cp->lock`: set `state = CELL_ON_DISK`, unlink from
   `cp->sparse` (so GC never harvests an on-disk cell). The disk slot is
   the cellid itself (1:1), so there is nothing to allocate.
3. `kernel_write` the cell's `cell_size` bytes to extent `cellid`
   (O_DIRECT), outside `cp->lock`.
4. `__free_pages(c->pages[0], cp->cell_order)`; clear `pages[0]`/`base`;
   subtract from `pages_used`. Drop the rwsem.

If the write fails, roll back `state` to IN_RAM and leave the cell
resident (the partial disk image is ignored).

### Swap-in (whole cell, raw)

`cell_load()` gains a swap-in prologue. If `state == CELL_ON_DISK`:

```c
static int cell_swap_in(struct cell_pool *cp, struct cell *c);
```

1. Take `down_write(&c->lock)`; re-check `state` under it. If already
   IN_RAM, a concurrent loader won -- return 0. The `down_write` is the
   serializer: N concurrent loaders collapse to one swap-in.
2. `alloc_pages(cell_gfp(cp), cp->cell_order)` for the cell;
   `kernel_read` extent `cellid` into the new pages (O_DIRECT), outside
   `cp->lock`. If the read fails, free the pages, leave state ON_DISK,
   and return the error.
3. Under `cp->lock` install `pages[0]`/`base`, set `state = CELL_IN_RAM`,
   add to `pages_used`. Drop the rwsem.

The caller then proceeds with the normal `memcpy` from `c->base + offset`.
A cell swapped back in rejoins the in-RAM pool (eligible for GC, and
re-swappable once cold again).

### Dirty tracking

None needed. A non-active cell is immutable (invariant #1), and the
active cell is never swapped out -- so a cell's bytes never change while
it is ON_DISK, and every swap-out writes the full cell. There is no
partial update and no dirty bit; a cell swapped in for a read simply
rejoins the resident pool and is rewritten wholesale on its next
swap-out.

### GC interaction

- `cell_pool_pick_victim()` skips `state == CELL_ON_DISK` cells (they
  hold no RAM to reclaim and are already cold).
- An on-disk cell is only GC'd after being swapped in: a read brings it
  resident, it becomes sparse-eligible, and a later pass evacuates it.
- Evacuation/empty-return of a cell leaves its disk slot alone (1:1: the
  extent is permanent and simply overwritten next time that cellid swaps
  out); only the RAM is freed.

### Free / evacuate / teardown

- `cell_free()` on an ON_DISK cell: decrement `live_bytes` (no RAM is
  held). If it reaches 0, drop the slot back to free (the disk extent is
  permanent and ignored). If it becomes sparse but non-empty, leave it on
  disk (disk is cheap); it is reclaimed after a swap-in.
- `cell_pool_return()` / `cell_pool_release_victim()` free only RAM (the
  disk slot is permanent and needs no release).
- `cell_pool_destroy()` closes the backing file; no swap image survives
  the pool.

### Locking

- `cp->lock` guards `state` and the directory; it is never held across
  I/O (O_DIRECT read/write happen outside it).
- Per-cell rwsem (`c->lock`): swap-out and swap-in take `down_write`
  (excludes `cell_load` readers and GC evacuation, and serialises
  concurrent swap-ins); `cell_load` takes `down_read` for its memcpy.
- Lock order: cell rwsem -> `cp->lock` (matches existing GC).

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
| `include/linux/cell_store.h` | Modify | `cell_backing` (1:1), `cell.state`, new APIs |
| `mm/cell_store.c` | Modify | extent alloc, O_DIRECT swap-in/out, state machine, free/GC/teardown extent cleanup |
| `include/linux/shmem_compress.h` | Modify | `swapfile`/`swapsize` opt plumbing into pool create |
| `mm/shmem.c` | Modify | parse `swapfile=`/`swapsize=` mount options |
| `mm/shmem_compress.c` | Modify | aging-pass hook calling `cell_pool_swap_out()`; pass backing opts to pool |

## Migration

Each phase compiles clean at default (no `swapfile=` -> no-op) and is
independently committable and bisectable.

### Phase 1: backing plumbing, no-op at default (est. small)

Add `struct cell_backing` (O_DIRECT file, 1:1 extents), `cell.state`
(+ enum); `cell_pool_create`
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
| Concurrent swap-in of one cell double-reads | Med | Med | `down_write` on the cell serialises loaders; re-check state under it |
| Swapping out a cell GC is evacuating | Low | High | swap-out takes `down_read(c->lock)`, GC holds `down_write`; pick_victim skips ON_DISK |
| Active-cell mutation during swap-out | Low | High | Active cell is never a swap-out candidate (invariant #1) |
| Backing file page cache re-pins RAM | Med | High | O_DIRECT on the backing file |

## Open Questions

- Backing-path provisioning: should `swapfile=` require the file to
  pre-exist, or may the pool create+`fallocate` it? (Decide in Phase 4;
  likely: pre-exist, size via `swapsize=`.)
- Swap-out policy: how many cells per aging pass, and a recency
  protection so a just-read cell is not immediately re-swapped? (First
  cut: a fixed `max` per pass; refine with recency later.)
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

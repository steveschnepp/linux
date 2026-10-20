---
name: whole-cell-swap-implementation
description: implement specs/07 -- whole-cell disk swap for ztmpfs in 4 bisectable phases (backing plumbing, O_DIRECT swap-in/out core, extent cleanup + aging hook, swapfile=/swapsize= mount options); compile-gated on i386/x86_64, runtime boot test still outstanding
project: linux
previous: 2026-10-19_compressed-disk-spill-design.md
---

# Mission Log: Implementing Whole-Cell Disk Swap

Part 61 of the tmpfs transparent-compression series.  Same day as the
design log (previous).  Implemented `specs/07_WHOLE_CELL_DISK_SWAP.md`
-- the tier that pages a cold, in-RAM cell out to a disk backing file
(whole, raw) and back in on access.  Four phases, each compile-gated and
committed separately so the feature bisects.

## What We Did

Spec first (`specs/07`, commit `970800324427`), grounded in a full read
of `mm/cell_store.c` and the cell invariants.  The load-bearing fact that
made the design safe: **invariant #1 -- only the active cell grows -- so
a non-active cell is content-immutable.**  Nothing writes a new block
into a cell that has stopped being active, which is exactly the
condition that lets you move the whole cell out from under its readers.

Then four phases, each a compile-clean no-op at default (no `swapfile=`
-> `cp->backing == NULL` -> every path byte-identical to before):

### Phase 1 (9d4e39ad541f): data structures

`struct cell_backing` (O_DIRECT file + extent free-map), `cell.state`
(`CELL_IN_RAM`/`CELL_ON_DISK`), `swap_extent`, `dirty`, and a per-cell
`swap_comp` completion (inited once at pool create, alongside the
rwsem).  Nothing reads them yet.

### Phase 2 (176eff79541d): the swap-in/out core

The correctness-critical part.  In `mm/cell_store.c`:
- Extent allocator (`cell_backing_alloc_extent`/`_free_extent`, a
  bitmap under `map_lock`).
- Raw O_DIRECT whole-cell I/O (`kernel_write`/`kernel_read`, one
  extent-aligned `cell_size` transfer per direction).
- `cell_swap_in()`: takes `down_write(&c->lock)`, re-checks state (the
  lock is the serializer that collapses N concurrent loaders into one),
  allocs pages, O_DIRECT-reads the extent, installs under `cp->lock`,
  recycles the extent.
- `cell_swap_out_one()`/`cell_pool_swap_out()`: `down_write`, flips to
  ON_DISK and unlinks from `cp->sparse` BEFORE the I/O (so a concurrent
  shrinker/GC sees it as on-disk and skips), frees the RAM after a
  successful write, rolls back to resident on failure.
- `cell_load()` gained a swap-in loop (an ON_DISK cell is paged in whole,
  then the memcpy serves from RAM).  `cell_pool_pick_victim()` skips
  ON_DISK cells.

The `down_write` (not `down_read`) on the state transitions is the key
locking decision: swap-out needs *exclusive* access (a concurrent
`cell_load` memcpy under `down_read` would otherwise be reading pages we
are freeing), and it doubles as the concurrent-swap-in serializer.

### Phase 3 (80398ac8d0dc): extent cleanup + the aging hook

`cell_free()` learned that an ON_DISK cell has `live_bytes` but no pages:
it must still be accounted (a truncate frees a block behind a paged-out
cell), and on empty it recycles the extent and drops the slot back to
free.  A sparse non-empty on-disk cell is left on disk (cheap) and
deliberately NOT linked into `cp->sparse` (GC needs resident pages).
`cell_pool_destroy()` tears the backing down.  `shmem_comp_age_work()`
calls `cell_pool_swap_out(SHMEM_COMP_SWAP_BATCH)` after `maintain`,
from the workqueue (sleepable I/O), outside `shmem_comp_mounts_lock` so
the I/O does not block other mounts.

### Phase 4 (a10ae8850c13): mount options + backing attach

`swapfile=<path>` and `swapsize=<bytes>` tmpfs options parsed into
`shmem_comp_opts`, plumbed through `shmem_comp_pool_create()` (signature
extended; single caller) to a new
`cell_pool_enable_backing()`: opens the file `O_DIRECT|O_RDWR`,
`vfs_fallocate`s to `swapsize` (or uses the file's existing size when 0),
and builds the all-free extent bitmap.  A failed attach degrades
gracefully (warn, cells stay resident, compression unaffected).

## What We Learned

### Technical

- **"Only the active cell grows" makes whole-cell swap safe.**  A
  non-active cell's content is immutable (only frees/GC touch it), so
  paging it out cannot race a store into it.  That invariant, already in
  `cell_store.h`, is what the whole tier rests on.
- **Swap-out must take the cell rwsem for WRITE, not read.**  The
  existing `cell_load` takes `down_read` for its memcpy; a swap-out
  freeing the pages under a concurrent reader would be a use-after-free.
  `down_write` excludes readers *and* is the natural serializer for
  concurrent swap-ins (losers re-check state and find IN_RAM).
- **Flip state to ON_DISK before the I/O, not after.**  A concurrent
  shrinker/GC consults `state`; flipping early means it sees the cell as
  on-disk and skips it during the write window.  Roll back to IN_RAM if
  the I/O fails.
- **The backing must be O_DIRECT.**  A buffered backing file's page
  cache would hold dirty pages -- pinning the very RAM the swap frees,
  and re-entering reclaim.  This is the same reentrancy hazard that
  disqualified swappable cell memory; O_DIRECT sidesteps it.
- **An ON_DISK cell has live_bytes but no pages.**  The original
  `cell_free` early-returned on `!pages[0]`, which would silently leak
  the extent (and skip accounting) when a truncate freed the last block
  of a paged-out cell.  The free path now branches on state first.
- **Kernel swap subsystem is unusable for kernel memory** (established in
  the design session) -- hence a pool-owned backing file, not swap slots.

### Process

- **Bisectable phases kept each commit green and reviewable.**  Phase 1
  (types) and Phase 2 (core) were inert until Phase 4 wired the mount
  option, so nothing was reachable -- and therefore nothing could break
  -- until the last phase.  The cost is that Phases 1-3 were
  compile-gated dead code until Phase 4; the benefit is that if Phase 4
  misbehaves, the earlier phases are known-good.
- **Grounding the spec in a full read of `cell_store.c` first** paid off:
  the immutable-active-cell invariant and the rwsem protocol were
  already documented there, and the design fell out of them rather than
  fighting them.

## What We Decided

- **Swap unit = the whole cell, moved raw.**  No compress/decompress in
  the swap path (the cell already holds compressed blobs); only the
  normal read-path decompression remains.
- **Backing = a pool-owned O_DIRECT file**, carved into cell-sized
  extents with an in-RAM free-map.  Not the kernel swap subsystem (it
  cannot swap kernel memory).
- **Backing attach is best-effort**: failure warns and leaves cells
  resident rather than failing the mount or disabling compression.
- **GC runs on in-RAM cells only**; an on-disk cell is GC'd only after a
  swap-in.  Sparse on-disk cells are left on disk (disk is cheap).
- **Deviation from the spec**, minor: the spec said backing opts flow
  *into* `cell_pool_create()`; implemented instead as a post-create
  setter `cell_pool_enable_backing()` called from
  `shmem_comp_pool_create()`.  This keeps `cell_pool_create`'s signature
  stable and is equivalent (called immediately after create, before the
  first store/swap).

## Rules Added

- **A cell state transition takes the cell rwsem for write** -- swap-out
  must exclude `cell_load` readers (which hold `down_read` across the
  memcpy), and `down_write` doubles as the concurrent-swap-in serializer.
- **Set ON_DISK before the swap write, roll back on I/O failure** -- so a
  concurrent shrinker/GC skips the cell during the write window.
- **The disk backing must be O_DIRECT** -- a buffered backing re-pins the
  freed RAM in its page cache and re-enters reclaim.
- **Account an ON_DISK cell's live_bytes on free** -- it holds no pages
  but its extent must be recycled on empty.

## What We'd Do Differently

- Would like a runtime test alongside each phase rather than only a
  compile gate.  The QEMU/loop-file boot test (mount with `swapfile=`,
  fill, force a swap-out, read back and verify) was not run in this
  session; it is the outstanding verification (see Next Steps).  The
  compile gate (i386 + x86_64 + full `mm/`) is necessary but not
  sufficient for a feature with this much concurrency and I/O.

## Files Changed

| File | Purpose |
|------|---------|
| `specs/07_WHOLE_CELL_DISK_SWAP.md` | the implementation contract |
| `include/linux/cell_store.h` | `cell_backing`, cell state/extent/dirty/comp, new API decls |
| `mm/cell_store.c` | extent allocator, O_DIRECT I/O, swap-in/out, state-aware free/GC/teardown, backing attach |
| `include/linux/shmem_compress.h` | `swapfile`/`swapsize_bytes` opts, pool-create signature |
| `mm/shmem_compress.c` | pool-create backing attach, opts_free, aging swap-out hook |
| `mm/shmem.c` | `swapfile=`/`swapsize=` mount-option parsing |

## Test Results / Benchmarks

- **Compile gate, i386 (kbuild-i386):** `cell_store.o`,
  `shmem_compress.o`, `shmem.o` -- 0 errors / 0 warnings after each of
  the four phases.
- **Compile gate, x86_64 (kbuild-x86_64):** the same objects clean.
- **Full `mm/` build, i386:** exit 0, no warnings/errors in any of the
  changed files.
- **Runtime behaviour: NOT yet tested** -- no boot/loop-file run in this
  session.  Correctness of the swap-in/out concurrency, the O_DIRECT
  path, and the extent lifecycle under a real workload is unverified.

## Next Steps

1. **Runtime verification (highest priority).**  Boot the i386-pae guest
   with a loop-backed file, mount tmpfs `cell=...,swapfile=<loopfile>`,
   fill past RAM, force an aging pass, and confirm: (a) `pages_used`
   drops as cells page out, (b) a read of a swapped page swaps the cell
   back in and returns correct data (CRC/manifest checks), (c) no
   regression with `swapfile=` absent.
2. **Stress the concurrency**: concurrent readers on a cell mid-swap-out,
   and truncate during swap, under KASAN (there is already a KASAN
   x86-64 build dir).
3. **Back it into the workload bed (spec 05)** once it runs, so the RAM
   saved and the swap-in latency are measured, not assumed.
4. Revisit the spec's open questions with real numbers: swap-out cells
   per pass, recency protection (avoid re-swapping a just-read cell),
   and interaction with the spec06 watermark.

---

## Session 77: The 1:1 cell-to-extent mapping (2026-10-19)

### What We Did

Reworked the disk mapping from a bitmap extent allocator to a **static
1:1 mapping**: cell N lives at extent N (file offset `N * cell_size`),
commit `28e259d`. Driven by two things from the follow-up discussion:

1. The SBC use case (R/O root + a R/W ztmpfs whose cold cells page to a
   backing file on a second MMC partition) where "disk is cheaper than
   RAM" and disk >> RAM, so a backing >= the pool's cell capacity is the
   natural sizing.
2. A wear-leveling concern: the first-cut allocator was **first-fit**,
   which concentrates writes on the low extents (a freed low extent is
   the first reused) -- bad for MMC/eMMC endurance. The user proposed
   aligning the disk layout with the in-RAM cell array.

The new rule: `nr_extents >= nslots` is **required**; a smaller backing
is refused with `-ENOSPC` (no allocator fallback). Everything else
follows: cellid *is* the disk slot, so there is no free-map, no
`map_lock`, no alloc/free-extent, no per-cell `swap_extent`/`dirty`/ 
`swap_comp`, and swap-out can never fail for lack of space. Net -29
lines.

### What We Learned

- **1:1 gives natural wear *spreading*, not active leveling.**  Each
  extent is written only when its own cell pages out, so writes distribute
  across extents by per-cell churn -- strictly better than first-fit's
  concentration. But a single hot cell still concentrates on *its* extent
  (round-robin would spread one hot cell across the device but loses the
  stable mapping). For a tmpfs, churn is spread across many cells, so
  1:1 is good enough; a per-cell rotation (`cellid ^ generation`) is the
  escape hatch if an endurance hotspot ever appears.
- **Requiring disk >= capacity is coherent with the feature's purpose.**
  The tier exists to extend the tmpfs past RAM; if the disk were smaller
  than the pool's cell capacity it could not extend anything, so refusing
  it outright loses nothing.
- **`down_write` on the state transition is the concurrent-swap-in
  serializer** -- once the extent allocator went away, the completion
  field (`swap_comp`) became redundant; the cell rwsem's write side
  already collapses N loaders into one swap-in.

### What We Decided

- **1:1 mapping, capacity required, allocator deleted** (`28e259d`).
- The **partition-not-loop** path is a later, separate change (drop-in
  behind `cell_backing`: `filp` -> `bdev`, `kernel_read/write` -> bio);
  the main case stays a regular O_DIRECT file.

### Files Changed (this session)

| File | Purpose |
|------|---------|
| `mm/cell_store.c` | 1:1 mapping: drop allocator, extent=cellid, capacity check |
| `include/linux/cell_store.h` | slim `cell_backing`, cell.state only |
| `specs/07_WHOLE_CELL_DISK_SWAP.md` | updated to the 1:1 design |

### Test Results (this session)

- i386 + x86_64 compile gates and full `mm/` build: 0 warnings / 0
  errors after the refactor.
- Runtime still untested (unchanged from Session 1).

---

## Session 78: 2-bit swap state, write-free re-swap, TRIM on free (2026-10-19)

### What We Did

Refined the on-disk model from a single `cell_state` enum to **two
independent bits** -- `in_ram` and `on_disk` -- with RAM as a cache of
the authoritative disk copy, commit `361dc3d`. Driven by the observation
that a cell does not *need* to be on disk, and once it is, its disk copy
should persist across RAM churn. The bits (in order): new cell = in_ram
only; first swap-out writes the extent -> on_disk only; swap-in sets
in_ram (on_disk stays); a re-swap under pressure just clears in_ram (NO
extent write); free() with on_disk set punches the extent.

Two consequences:

1. **Write-free re-swap** (`cell_swap_out_one` writes only when
   `!was_on_disk`). A non-active cell is immutable, so a resident copy of
   an on-disk cell is byte-identical to the disk copy -- a re-swap needs
   no write. A cell that churns between RAM and disk writes its extent
   once, not per swap: a direct flash-endurance win on the MMC use case.
2. **TRIM on free** (`cell_backing_discard_extent`). `cell_free()` on an
   emptied cell now disposes on BOTH RAM and disk: returns the pages (if
   in_ram) and `fallocate(FALLOC_FL_PUNCH_HOLE|KEEP_SIZE)` the extent (if
   on_disk). The punch can issue discard/TRIM, letting the eMMC/SSD
   reclaim the physical blocks and lowering write amplification on the
   extent's next use -- arguably *more* important for endurance than the
   allocation policy, because it hands dead blocks to the device's own
   wear-leveling. Best-effort (a fs without punch/discard no-ops).

Also closed a **latent GC race** found while re-checking the locking:
`cell_pool_pick_victim()` could grab a cell that a concurrent whole-cell
swap-out paged out between the sparse-list walk and `down_write` --
returning it would let `cell_relocate_one` read a NULL base. Fixed by
re-validating `in_ram` under `down_write` (bail if the resident copy
went away).

### What We Learned

- **RAM-as-cache-of-disk is the right frame**, and it falls out of
  invariant #1 (non-active cells immutable): the resident copy can never
  diverge from the disk copy, which is what makes the re-swap write-free
  and removes any need for a dirty bit.
- **Punch-on-free > allocate-then-overwrite for flash.** Handing a dead
  cell's blocks back via TRIM lets the device's wear-leveling work; just
  leaving stale bytes (the pre-Session-77 behaviour) keeps them "live" in
  the controller's view. Worth doing at the one point where the data
  provably dies (cell empty).
- **The state machine had a hidden invariant to protect**: pick_victim
  and swap-out both take the cell `down_write`, but a cell can move
  sparse<->resident between a list walk and the lock, so the harvest side
  must re-validate, not trust the list.

### What We Decided

- **Two bits (`in_ram`/`on_disk`), not an enum** -- the "both" state
  (RAM caching disk) is expressible and enables write-free re-swap.
- **Punch (TRIM) only on free**, not on swap-in (the disk copy is the
  data while resident; punching there would TRIM-then-rewrite churn).
- **Backing should be on a `discard`-mounted (or fstrim'd) fs** for the
  TRIM to reach the device; noted in specs/07.

### Files Changed (this session)

| File | Purpose |
|------|---------|
| `mm/cell_store.c` | 2-bit state, write-free re-swap, discard-on-free, pick_victim re-validation |
| `include/linux/cell_store.h` | `in_ram`/`on_disk` replace the enum |
| `specs/07_WHOLE_CELL_DISK_SWAP.md` | 2-bit model, state machine table, TRIM note |

### Test Results (this session)

- i386 + x86_64 compile gates and full `mm/` build: 0 warnings / 0 errors.
- Runtime still untested (the punch/TRIM and write-free paths in
  particular need the loop-file boot test, ideally on real eMMC).

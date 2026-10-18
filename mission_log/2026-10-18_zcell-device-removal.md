---
name: zcell-device-removal
description: remove the zcell block/swap device (mm/zcell.c, CONFIG_ZCELL + debug configs, engine node_put hook); keep ztmpfs -- the cell-swap idea is parked after it lost on both perf and OOM RAM-extension
project: linux
previous: 2026-10-18_zcell-bvec-and-groupwin.md
---

# Mission Log: Removing the zcell Block/Swap Device

Part 55 of the tmpfs transparent-compression series.  Same day as the
bvec/groupwin log (previous).  A deletion session: the zcell
compressed-RAM block device is removed; the cell-backed tmpfs (ztmpfs)
stays.

## The decision, and why

The zcell device was reopened in session 67 as "a zram replacement for
the 64M OpenWrt fleet."  Over this session it was measured on its two
value propositions and lost both:

1. **Perf**: at ratio-1 it ties zram on RAM (0.15%) but is ~0.85x on
   swap-in / ~1.14x on swap-out (session 71, reconfirmed).  The per-
   block overhead is diffuse and cannot be micro-opted to parity
   (sessions 72-73).
2. **RAM extension under OOM** (the fleet's real pain -- reclaim never
   converges before OOM on 64M): a no-madvise natural-reclaim bench
   (oombench) on i386-pae showed zcell extends RAM 2-4x SLOWER than
   zram and collapses intermittently (OOM on the second alloc wave, and
   on wave 1 at some sizes), while zram extends cleanly and repeatedly.
   Root cause: the whole-cell design needs contiguous order-4 (64K)
   cells, which fail under buddy fragmentation during reclaim -- zram's
   order-0 zsmalloc is immune.  Not the dense-slot inline (pre-inline
   collapses worse at 64M); not GC timing (collapses on wave 1, before
   any free).

3. **Swap-out merging** (the differentiator the mTHP bet rode on) is not
   worth building without large folios: the multi-page-bvec signal never
   fires on base-page swap (multi=0 across 6.2M writes), and the 5ms
   grouping-window "25% win" decomposes to pure lz4 frame-overhead
   amortization (~0% real on real-size pages).

With perf lost, OOM lost, and merging gated on a signal that does not
exist on the fleet's base-page swap, the device has no winning case
against zram.  Decision (user): **remove it, keep ztmpfs.**

## What We Did

Removed, with `dbe5277339fe`:
- `mm/zcell.c` (the device) -- deleted.
- `fs/Kconfig`: `CONFIG_ZCELL` + the two gated debug configs
  (`ZCELL_DEBUG_BVEC`, `ZCELL_DEBUG_GROUPWIN`) -- deleted.
- `mm/Makefile`: `obj-$(CONFIG_ZCELL) += zcell.o` -- deleted.
- The device-only frontend hook in the engine:
  `shmem_comp_pool_set_node_put()` + the `node_put`/`node_priv` fields
  in `shmem_comp_pool` + the dispatch in `shmem_comp_loc_pin_put()`.
  `shmem_comp_loc_pin_put()` simplifies to always the entry-free path
  (an entry pool's reverse index holds entries' embedded loc nodes, so
  the node always belongs to an entry).

Kept (all ztmpfs): the compression engine (`shmem_compress.o`), the
cell allocator (`cell_store.o`), the whole cell-storage API
(`shmem_comp_loc_*` / `cell_*`), the GC (incl. the per-pool demand
worker from `ac718642b597`), the shrinker, and the watermark (spec06).
zcell was a thin frontend over all of these; the device is the only
thing dropped.

Verified: i386 compile gate 0/0 on `shmem_compress.o` + `cell_store.o`;
no `zcell`/`node_put`/`ZCELL` references remain in the engine (only
historical prose comments).

## What We Learned

- **The cell-swap idea is parked, not disproven as a storage
  technology.**  The cell allocator itself (zero per-object metadata,
  whole-cell return) is sound and remains the ztmpfs backend; what
  failed was its fit as a *swap block device* against zram (contiguity
  under fragmentation, per-block overhead, no merge signal).
- **A delete is a decision worth logging with its measurements.**  The
  two commits that motivated this (demand GC, the OOM/merge
  measurements) plus this removal form the full arc: reopened -> measured
  -> lost on both axes -> parked.

## What We Decided

- **zcell device removed; ztmpfs kept.**  The cell-backed tmpfs path is
  unaffected.
- **The debug bvec/groupwin instruments are removed with the device**
  (they measured the device's swap path).  If a large-folio signal
  question returns for ztmpfs, they can be resurrected from git history.
- The real ztmpfs lever (from the same session) remains open and
  unrelated to this removal: nothing here changes ztmpfs behavior.

## Rules Added

- **When a feature loses its value proposition on measured grounds, a
  clean removal (with the motivating measurements cited in the commit)
  beats leaving dead code.**  The device was ~1000 lines + Kconfig +
  engine hook; all removed once the verdict was in.

## What We'd Do Differently

- Would have measured the device's OOM RAM-extension (the fleet's actual
  pain) at the same time as its perf, rather than ~10 sessions apart --
  the perf tie/loss was known in session 71 but the OOM contiguity loss
  only surfaced this session; both together are the honest verdict, and
  measuring them together would have parked the device sooner.
- Would have checked the merge signal (multi-page bvec) and the grouping
  win BEFORE designing the watermark-async + bvec-grouping feature -- the
  two gated instruments answered "no signal, no win" cheaply, after the
  design work was already done.

## Files Changed

| File | Purpose |
|------|---------|
| `mm/zcell.c` | deleted (the zcell block/swap device) |
| `fs/Kconfig` | -`CONFIG_ZCELL`, -`ZCELL_DEBUG_BVEC`, -`ZCELL_DEBUG_GROUPWIN` |
| `mm/Makefile` | -`obj-$(CONFIG_ZCELL) += zcell.o` |
| `mm/shmem_compress.c` | -`node_put`/`node_priv` fields, -`set_node_put`, simplified `loc_pin_put` |
| `include/linux/shmem_compress.h` | -`shmem_comp_pool_set_node_put()` decl |

## Test Results / Benchmarks

- i386 compile gate: 0 errors / 0 warnings on `shmem_compress.o` +
  `cell_store.o` after the removal.
- No `zcell`/`node_put`/`ZCELL` references remain in the engine, Makefile
  or Kconfig.

## Next Steps

1. ztmpfs is now the sole focus of the cell-storage work.  The open
   ztmpfs lever from this session: nothing in this removal changes
   ztmpfs; its perf A/B (no regression) and the demand-GC worker stand.
2. Standing backlog unchanged: H618 re-measure, RFC assembly, mTHP
   folio grouping (ztmpfs merge), workload bed (spec 05), BE runtime
   boot.
3. If the cell-as-swap idea is ever revisited, start from this session's
   measurements (perf, OOM contiguity, merge signal) and git history
   (`dbe5277339fe^` has the full device).

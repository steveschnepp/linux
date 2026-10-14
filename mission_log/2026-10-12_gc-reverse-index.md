---
name: gc-reverse-index
description: built the per-cell reverse index -- GC drain goes from O(total entries) to O(victim); race-soak + perf A/B (2.2x at 6K, 9.2x at 49K entries); caught the bench measuring nothing
project: linux
---

# Mission Log: The GC Reverse Index

Part 26 of the tmpfs transparent-compression series.  Session 42
(2026-10-12).  Implements `specs/04_GC_REVERSE_INDEX.md`, the arm-3
"GC efficiency" improvement target: a per-cell reverse index so the
cell GC drain walks a victim's own entries instead of the whole entry
table.  Four phases (storage+maintenance, drain rewrite, race soak,
perf A/B), five commits, all gates green.

The brief, in the user's words: "let's go your way and test.  build the
reverse idx.  then torture test it at low size and big size.  along
with perf", then "always commit.  bisectable.  and wrtie a mission log".

## What We Did

### 1. Grounded the design in the actual entry lifecycle

Before writing code, read the five functions the change touches:
`shmem_comp_entry_alloc` (creates, xa_alloc, returns id),
`shmem_comp_entry_free` (refs==0: xa_erase -> cell_free -> kfree_rcu),
`shmem_comp_drain_victim` (the old xa_for_each full-table scan),
`shmem_comp_cell_gc` (pick/drain/release loop), and
`shmem_comp_load_entry` (the reader's retry protocol).  The concurrency
falls out of these: the pin (`atomic_inc_not_zero`) is what keeps a cell
valid across a relocate memcpy, and `gc_lock` serialises GC passes so a
handle cannot change mid-drain.

### 2. Wrote the spec first (specs/04)

Pinned the three things that are dangerous to get wrong before any code:
the exhaustive-linkage completeness invariant (a live entry missing from
its cell's list leaks the cell), the unlink-before-`cell_free` ordering
(`cell_free` can return the cell and the slot be reused, so a stale node
would name a freed entry in a slot holding different data), and the
leaf-lock discipline (`index_lock` never held across alloc/memcpy/
cell_free/cell_relocate_one).  Also settled that batching (the earlier
idea) is *subsumed* by the index -- B existed only to amortize the O(N)
scan, which the index removes -- so the single-victim sequential drain
stays.

### 3. Phase 1 -- storage + maintenance, index unused by GC

Added `cell_node` to the entry, `cell_index[]` (one list head per
directory slot) + a single `index_lock` to the pool, allocated the array
in pool create, and threaded the three mutation points: alloc links, free
unlinks before `cell_free`, GC relocate moves.  GC still used the old
full-scan drain.  Gate: `test_cell_gc` passed -- maintenance alone is
correct.  Also fixed a pre-existing leak the new alloc exposed: the
pool-create failure paths freed the zsmalloc pool but not the cell pool
or its shrinker; routed create-failure and `pool_destroy` through one
`shmem_comp_pool_cells_teardown()`.

### 4. Phase 2 -- the reverse-index drain

Rewrote `drain_victim` to walk `cell_index[vic->cellid]`: detach one live
entry under the lock, relocate outside it, move the node to the
destination cell, drop the pin.  Two things the old `xa_for_each` did
not need: dying entries are *left* for their own `entry_free` to unlink
(not drained), and a relocate failure must put the entry back *and break*
-- re-adding to the head and looping would re-find it, unlike
`xa_for_each` which advances past.  Added `entry_put_ptr` (drop a pin by
pointer, no xa_load) and stored `ent->id` so teardown can run from a
pointer.  Gate: `test_cell_gc` + full KASAN suite, exit 0, 0 KASAN.

### 5. Phase 3 -- the race soak (the test that matters)

New `test_cell_index_race`: 4 writer threads doing overwrite+truncate
churn (entry alloc links, entry free unlinks) against a `gc_now` thread
firing drain passes (walk/detach/relocate/relink) concurrently, under
KASAN.  Verdicts: integrity (surviving files read back stably),
completeness (after final gc + full drain, no sparse cells linger and all
cells return -- a missed unlink shows here), liveness (no index_lock
hang).  Result: **836 concurrent GC passes in 6s, 0 write errors, stable
reads, 0 sparse cells after final GC, all cells returned, 0 KASAN.**

### 6. Phase 4 -- the perf A/B, and the bench that measured nothing

Added `gc_use_index` (module param, default 1) so both drains run on one
kernel: =1 walks the index, =0 restores the old xa_for_each full scan as
a named baseline (`drain_victim_scan`) that also moves the index node, so
the invariant holds in both arms and only the find-strategy differs.  New
`test_cell_gc_bench` (gated behind `test.cellgcbench=1`, non-KASAN
kernel) times a synchronous `gc_now`.

**The first bench run was garbage and I caught it.**  It reported scan
2.02 ms at 4K entries vs 1.96 ms at 16K -- flat, when a true O(N) scan
should 4x.  Root cause: `NPAGES=64` and a 64 KiB cell holds ~32 2-KiB
entries, so each 64-page file was *exactly 2 cells*; unlink()ing whole
files EMPTIED cells (tier-1 synchronous return) instead of leaving them
sparse, so GC had nothing to drain and the bench measured `maintain()` +
mount overhead.  Fixed by making `NPAGES=24` (cuts across cell
boundaries) and -- the important part -- **making the bench
self-validating**: it now FAILS if 0 sparse cells exist, so this exact
trap cannot silently return.

## What We Learned

### Technical

- **The completeness invariant is the whole design.**  Three mutation
  points (alloc/free/reloc) are exhaustive because exactly three events
  change an entry's cell.  A fourth path that changes a handle without
  moving the node would leak the cell *silently* -- no crash, just a
  cell that never empties.  That is why the race test asserts
  completeness (post-GC sparse==0), not just "no crash".

- **Unlink before `cell_free`, always.**  `cell_free` can return the cell
  to the buddy synchronously and `cell_make_active` can reuse the slot as
  a new cell.  A node left on `cell_index[cellid]` would then point into
  a freed entry from a slot holding different data.  The window is real
  and the ordering closes it.

- **A list walk cannot "skip and continue" the way xa_for_each can.**
  The old drain did `shmem_comp_entry_put(id); continue;` on a
  too-large/failure entry.  On a list, re-adding the entry to the head and
  looping re-finds the same entry -- infinite loop.  The reverse-index
  drain puts it back and *breaks*; `cell_gc`'s moved==0 stop signal bounds
  the non-progress case.

- **KASAN would have corrupted the perf A/B.**  It instruments every
  pointer deref, and the old full-scan walk has far more derefs than the
  index walk -- so KASAN would exaggerate the win and corrupt absolute
  numbers.  The perf run went on the non-KASAN kernel, per the session-39
  rule.

- **`gc_now` is synchronous** (`shmem_comp_cell_gc_all()` runs inline in
  `gc_now_store`), so userspace can time a drain by timing the write.
  That is the measurement hook; no kernel instrumentation needed.

### Process

- **Self-validating benches beat after-the-fact-fixed ones.**  The NPAGES trap cost
  one boot cycle; the fix that matters is the assertion (FAIL on 0 sparse
  cells), not the value.  A bench that *can* measure nothing should say so
  loudly.  This is the third time this series has hit "the bench measured
  the wrong thing" -- the lesson is now structural, not editorial.

- **Bisectable commits = one validated state each.**  Split the kernel
  work into (feature: storage+maintenance+drain) and (A/B param), because
  those were the two states I actually booted and gated.  Not "one commit
  per logical edit" -- one commit per *tested* state, so every commit in
  the history builds and passes.

- **Write the spec before the concurrency.**  The reverse index is the
  most race-prone change in the series so far.  Pinning the invariant,
  the ordering, and the lock discipline in specs/04 *before* coding meant
  the implementation was transcription, not design-under-pressure.

## What We Decided

- **The reverse index is built and always-on** (default `gc_use_index=1`).
  It wins even at 6K entries (2.2x), so the spec's auto-gate open question
  leans "no gate needed" -- but that is one data point; the M-boot run
  should confirm before locking it.
- **Batching is not built.**  The index subsumes it; single-victim
  sequential drain stays, `cell_store.c` untouched.
- **The A/B param stays a module param, not a mount option** -- it is a
  bench knob, not a user feature, and the upstream slice is zsmalloc-only.

## Rules Added

- **A GC/perf bench must assert its stress fired.**  If the thing being
  measured (sparse cells, swap activity, OOM) did not occur, the bench
  FAILS rather than reporting a number.  Applies to every bench in the
  rig from now on.

- **Bisectable means one boot-validated state per commit, not one edit
  per commit.**  When a feature spans several edits that were validated
  together, they land as one commit; the A/B/instrumentation splits off.

## What We'd Do Differently

- Would have written the self-validating assertion into the bench from
  the first draft.  I only added it *after* the flat-number tell, which
  cost a boot cycle.  The NPAGES=64 alignment trap was predictable -- a
  cell is ~32 entries, so round-multiple file sizes empty rather than
  sparsify.

- The perf numbers are single-boot, 5 in-boot reps.  The *ratio* growth
  (2.2x -> 9.2x) is the trustworthy signal; the absolute scan times are
  noisy (large scan read faster than small -- drift).  The M-boot
  median+IQR wrapper is still owed before any of this is quote-worthy.

## Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | `cell_index` + `index_lock`, alloc/free/reloc linkage, reverse-index drain, `entry_put_ptr`, `gc_use_index` + `drain_victim_scan`, `cells_teardown` helper |
| `include/linux/shmem_compress.h` | `cell_node` + `id` fields in the entry |
| `specs/04_GC_REVERSE_INDEX.md` | the design of record |
| rig `tests/test_cell_index_race.c` | concurrent store/free/GC soak |
| rig `tests/test_cell_gc_bench.c` | GC drain A/B + boot gating |
| rig `guest/init.sh` | race test in suite, bench boot mode |
| rig `scripts/run_qemu.sh` | `TEST_CELLGCBENCH` passthrough |

## Test Results / Benchmarks

KASAN kernel for correctness, non-KASAN for perf.

| gate | result |
|---|---|
| `test_cell_gc` (maintenance + drain) | ALL PASS |
| full KASAN suite, Phases 1+2 | exit 0, 0 KASAN |
| `test_cell_index_race` (4 writers + gc_now, 6s) | ALL PASS, 836 GC passes, 0 KASAN |

Perf A/B (non-KASAN, 1G, self-validated 40/40 sparse cells):

| scale | entries | scan | index | speedup |
|---|---|---|---|---|
| small | ~6K | 3.19 ms | 1.46 ms | 2.2x |
| large | ~49K | 1.66 ms | 0.18 ms | 9.2x |

The 8x entry spread shows the O(total)-vs-O(victim) signature -- the gap
widens with N, which is the point.  Caveat: single boot, so trust the
ratio growth over the absolute ms.

## Next Steps

1. **M-boot wrapper** for the perf A/B (medians + IQR across boots) --
   the standing rigor item; the ratio needs it before it is quote-worthy.
2. **Decide the auto-gate** once M-boot confirms: always-on index (the
   2.2x-at-6K lean) vs a scale threshold.
3. **cell_order sweep** (spec 03 target 1) -- whether the cell win grows
   or shrinks off 64K, now that GC scan cost is off the table at these
   sizes.
4. Standing arm-3 items: the synchronous-free rm story at 64M (the
   shrinker evacuation cost under pressure), and the small-order
   directory wall.

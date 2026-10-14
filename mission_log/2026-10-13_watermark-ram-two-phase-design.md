---
name: watermark-ram-two-phase-design
description: spec 06 -- re-base the watermark on RAM usage, flip on the write path, unify bg compression and evacuation GC into one worker with two triggers (1Hz cadence + highwater kick) and an age gate against the write-compress-read ripple
project: linux
previous: 2026-10-13_zcell-watermark-triangle.md
---

# Mission Log: The Watermark Redesign (Spec 06)

Part 38 of the tmpfs transparent-compression series.  Session 52
(2026-10-13, third log of the day).  The design session that turns the
triangle finding into a fix: the user specified and refined a two-phase
watermark policy across four iterations, with two superseded versions
recorded below.  Deliverable: `specs/06_WATERMARK_RAM_TWO_PHASE.md`.

## What We Did

Started from the triangle verdict (only eager survives at exact-fit;
the flip is polled at 1 Hz against a sub-second RAM exhaustion).
Iterated the design with the user:

1. **Initial two-phase proposal:** phase 1 = write-path flip (sync
   compression between HIGH and LOW); phase 2 = background helper
   compressing all uncompressed pages, LRU-ordered, so RAM relief does
   not rely on tmpfs deletes.
2. **Signal correction (superseded v1):** thresholds read **mount**
   usage -- immediately corrected by the user to **RAM** usage ("much
   better").  Mount usage measures the wrong scarcity: compression
   buys RAM, so RAM pressure is the honest signal.  Verified the
   primitive: `si_mem_available()` is exported (mm.h:4163), accounts
   free + evictable cache, and the write path already routes every
   non-compressing write through `shmem_comp_should_compress()`
   (mm/shmem.c:3409) -- the flip lands there with zero call-site
   change.
3. **One-shot correction (superseded v2):** phase 2 was specified as
   a one-shot walk, never relaunched, LOW resetting the trigger.
4. **Unification (final):** keep the 1 Hz maintenance pass ("that 1Hz
   compaction is really a good idea... leave it that way") and merge
   the highwater compressor into it: ONE worker, TWO triggers (1 Hz
   cadence + immediate kick at the chill->hot flip), duties ordered
   eager-compress-then-evacuation-GC, each auto-skipped when its
   candidate set is empty.  The taxonomy collapses from "3 ways into
   zcell" (sync / 1Hz GC / highwater compressor) to 2 ways: sync, and
   the unified worker in one of two modes.
5. **Age gate:** the calm (1 Hz) mode compresses only pages that
   survived a previous pass ("seen" marker on the folio);
   under pressure (hot) the gate lifts and compression is immediate.
   Rationale given by the user: avoid the ripple -- write page,
   bg-compress it a moment later, read it back, decompress: two CPU
   burns and no benefit.  Verified `shmem_comp_age_round()` has NO
   age check today -- any plain folio found by `find_get_entries()`
   is compressed however young, so the ripple is real.

While here, answered a side question: does a `gc_percent` exist (GC
flush when a cell drops below N% usage after free)?  Yes and no --
`CELL_IS_SPARSE(live_bytes, cell_size) = live_bytes*2 <= cell_size`
is a hardcoded 50% byte-utilization classifier evaluated in
`cell_free()`, linking sparse cells to `cp->sparse`; but crossing the
line only feeds the victim queue.  The flush runs from four async
drivers: shrinker (memory pressure), store-exhaustion retry, `gc_now`
sysfs, and the ~4:1 interleave inside the aging pass.  No per-free
synchronous evacuation by design (batching economics).  Decision: no
`gc_percent` knob until spec 05's churn profiles show 50%
misbehaving.  Also spotted: stale comment at
`include/linux/cell_store.h:175` ("SPARSE_THRESHOLD objects" -- the
macro is byte-utilization now).

## What We Learned

### Technical

- `si_mem_available()` gives the RAM-usage signal cheaply enough for
  the write path (bounded counter reads; throttle to every Nth write
  exists as a fallback).
- The unified worker's two duties compose: duty 1 (chilled residents)
  is the write-side backlog, duty 2 (evacuation GC) is the cell-side
  compaction; both auto-skip, so a calm pass on a compressed system
  costs nearly nothing.
- The age gate falls out of a per-folio seen-marker for free: first
  observation marks, a later pass compresses.  Effective minimum age
  = one pass interval, no timestamps.
- GC driver #4 (the aging-pass interleave) survives this design --
  earlier drafts of spec 06 removed the periodic pass and with it the
  interleave; keeping the 1 Hz pass removes that risk entirely.

### Process

- Four design iterations in one sitting; two superseded versions
  recorded in the spec's evolution and here.  The user's corrections
  were all in the same direction: simpler unification, honest
  signals, and one mechanism with modes instead of three mechanisms.

## What We Decided

- `specs/06_WATERMARK_RAM_TWO_PHASE.md`: RAM-usage signal with
  unchanged option syntax; write-path flip with hysteresis (phase 1);
  unified `maint_work` with 1 Hz cadence + highwater kick, duties
  eager-compress-then-GC, auto-skip (phase 2); age gate in calm mode
  only; anchors 0:0 / 100:100 preserved by construction; 50%
  `CELL_IS_SPARSE` stays fixed (no `gc_percent`).
- Superseded: mount-usage signal (v1); one-shot never-relaunched
  drain (v2).  Both recorded so neither is re-proposed blindly.

## Rules Added

- **A policy trigger should be evaluated where the decision takes
  effect** -- the flip belongs on the write path, not in a 1 Hz poll,
  when the demand spike is sub-second.
- **When the user says "keep X" about an existing mechanism, fold it
  into the design rather than working around it** -- the 1 Hz pass
  came back as the worker's cadence and made the design smaller.

## What We'd Do Differently

- Read the existing pass machinery fully (age_round, its triggers,
  the sparse/GC wiring) BEFORE the first spec draft: the "remove the
  periodic pass" idea cost a revision once the user wanted it kept.

## Files Changed

| File | Purpose |
|------|---------|
| `specs/06_WATERMARK_RAM_TWO_PHASE.md` (kernel repo, new) | This spec (v3 of the design) |

## Test Results / Benchmarks

None -- design session.  The triangle rerun (21 combos, 2G/2G) is the
spec's Phase C gate; the 384M cliff torture matrix is the regression
gate for the kept 1 Hz pass.

## Next Steps

1. Implement spec 06 Phase A (write-path flip, RAM signal), then
   Phase B (unified worker + age gate), then Phase C (triangle +
   matrix rerun).
2. Spec 05 Phase 1 (workload engine) remains the other pending
   implementation thread.
3. Standing items unchanged: RFC assembly (spec 02 Phase 6), M-boot
   median+IQR wrapper, arm-3 rm/synchronous-free at 64M.

---

## Refinement pass: gc_percent per mount, per-mount worker (2026-10-13)

### What We Did

Two review questions on the spec, both resolved against the code:

1. **`gc_percent` per mount: yes, and nearly free.**  Verified the
   pool is per mount (`comp->pool = shmem_comp_pool_create(...)` in
   `shmem_comp_fill_sb`) and `cell_free()` already receives the pool,
   so the 50% `CELL_IS_SPARSE` classifier becomes a pool field
   (`gc_percent=N`, default 50 = today's behavior, clamped 10..90)
   behind a threshold-taking inline.  Spec 06 gained the `gc_percent`
   section, build/manifest rows, migration Phase D, and a risk row;
   the stale "SPARSE_THRESHOLD objects" comment at cell_store.h:175
   is a folded-in fix.
2. **Worker per mount: already is, and should stay.**  `age_work`
   was `INIT_DELAYED_WORK`-ed per mount in `shmem_comp_fill_sb`;
   `maint_work` keeps that.  A single global worker was considered
   and rejected in the spec's comparison table: lifecycle hazards
   (put_super must cancel one own work vs a global worker pinning
   every mount), zero-cost idleness (unarmed mounts cost nothing vs
   a always-on global tick under the mount-list lock), pass
   isolation, and natural kick routing (a flip schedules exactly its
   own mount).  The global list stays event-driven (gc_all,
   shrinker) as today.  Also specified: the kick's pass re-arms the
   cadence while hot, keeping the GC interleave alive during pressure
   since hot-mode writes never arm it.

### What We Decided

- `gc_percent` is in spec 06 (Phase D), per mount, default 50.
- The unified worker is per mount; no global worker.

### Files Changed

| File | Purpose |
|------|---------|
| `specs/06_WATERMARK_RAM_TWO_PHASE.md` | gc_percent section, per-mount worker verdict, hot-cadence re-arm, Phase D |

---
name: gc-on-exhaustion
description: compacting on directory exhaustion -- the churn gate, why GC-on-exhaustion could not bootstrap, and the sequential drain (session 25)
project: linux
previous: 2026-10-09_cellzram-workloads-and-capacity.md
---

# Mission Log: GC-on-Exhaustion, and the Two Ways It Failed to Work

Part 9 of the tmpfs transparent-compression series.  Session 25
(2026-10-09), picking up exactly where Part 8 ended: session 24's
capacity fix made the cell directory big enough for the advertised
`disksize`, and recorded GC-on-exhaustion (plan decision #3b) as "the
only remaining way writes can fail IOERR".

This session implemented #3b.  It took three attempts, and each
attempt failed for a different, measurable reason.  Worth recording in
full because the plan's own rationale for #3b was incomplete in two
separate ways, and the first attempt would have shipped as a no-op.

Kernel commits: `94d78f6ae3da`.
Rig commits: `9ac4a20` (test_churn).

## Session 25: compacting on directory exhaustion

### What We Did

1. **Read the plan's #3b against the code before writing anything.**
   The plan said: `cell_make_active` returns `-ENOMEM` when no free
   slot exists, "WITHOUT trying GC.  Make it run one GC pass to drain
   sparse cells, and fail only if that also comes up empty."

   That cannot work as written, from the code alone: the evacuation
   algorithm (`shmem_comp_cell_gc`) relocates via
   `cell_relocate_one` -> `cell_make_active`, which needs a *free
   directory slot*.  At exhaustion the directory is 100% occupied and
   the active cell is full, so the pass has no destination -- it moves
   nothing, frees nothing, and the retried store fails identically.
   Compacting a full directory needs one slot held back to bootstrap.

   Rather than stop there, the session's real finding came from
   measuring it, per session 21's "read the function, not the field
   name" and session 22's "stop theorizing and instrument".

2. **`tests/test_churn.c` -- the gate this workload class needed.**
   A small incompressible working set overwritten many times.  The
   directory grows with bytes WRITTEN (the bump pointer only advances;
   holes are reclaimed only by evacuating a whole cell), so churn
   fragments it while live data stays flat.

   **The first version did not reproduce**, and the reason is worth
   keeping: sequential full-set overwrites free 1:1 with allocation,
   every cell drains, and tier-1 returns it inline.  Measured: `phys`
   stayed at exactly 32 cells after 64 MiB written -- zero directory
   growth.  Random block selection scatters the frees instead, and that
   is what fragments.  A negative result that redirected the design.

   Also caught on run 1: `memparse()` reads a bare number as BYTES, so
   `disksize="16"` made a **16-byte** device.  cellzram rejected it
   (-EINVAL, page-alignment check); zram accepted it and returned
   ENOSPC on all 16384 writes.  The raw `mm_stat` dump exposed it
   instantly (`orig=4096` on a "16 MiB" device) -- session 21's
   auditable-transform rule earning its keep on the first run again.

3. **Attempt 1: a held-back slot ("reservation").**  Cap a store at
   `max_cells - 1`; only a GC caller may take the last slot.  Build,
   run: no change.  Instrumented rather than re-theorised.

   ```
   exhaust: sparse=66 occupied=83 max=84 -> gc nv=8 moved=64
   exhaust: sparse=65 occupied=84 max=84 -> gc nv=8 moved=0
   ```

   Three facts from one line.  The reservation worked
   (`occupied=83 max=84`).  Sparse linking worked (`sparse=66`), which
   killed this session's own guess that the cells were not sparse.
   And GC moved **exactly one cell's worth** (64 objects) then stopped,
   freeing nothing.

   Root cause: `GC_BATCH=8` victims all shared the ONE destination
   cell the held-back slot funds.  A victim holds at most one cell of
   live data, so that cell got split eight ways, no victim reached
   zero, and only an EMPTY victim returns a slot.  Batching against one
   destination is a fixed point.

4. **Attempt 2: drain victims sequentially.**  One victim at a time,
   each all the way to empty, so the emptied victim funds the slot the
   next drain needs.  Better (`nv=64 moved=1041`) but still stalled --
   and the stall moved, rather than disappearing.

   ```
   exhaust: sparse=67 occupied=84 max=84 -> gc nv=64 moved=1039 returns=64 pages_used=2368
   exhaust: sparse=69 occupied=85 max=85 -> gc nv=1  moved=0   returns=64 pages_used=5440
   ```

   `pages_used` was the tell: pass 1 compacted 84 cells down to **37**.
   GC works.  But it bootstrapped from the extra slot and left live data
   sitting in it; the store path then re-fragmented to a full 85/85 with
   a full active cell, and pass 2 had no destination at all.

5. **Attempt 3 (landed): drain the compaction slot too.**  At the end
   of a pass, if the extra slot still holds data, drain it like any
   other victim -- the slots the pass just freed are its destination.
   That restores the invariant "the compaction slot is free at the start
   of every pass", which is what attempts 1 and 2 actually needed.

   Result: every pass now drains 64 victims with returns climbing
   65 -> 130 -> 197, `next_free` no longer pinned at a full cell, and
   the gate green.

### What We Learned

#### Technical

- **The plan's #3b rationale was incomplete in two independent ways.**
  (a) it needs a bootstrap slot the design did not have; (b) even with
  one, batched victims can never empty.  Both were invisible from the
  plan text and obvious from two instrumented lines.  A design note
  that names a mechanism ("drain sparse cells") is not a design: the
  destination, the funding order, and the invariant that keeps the next
  pass working all have to be stated.

- **Evacuating one cell into another cell is slot-neutral.**  Allocate
  one, free one.  Compaction only pays when several victims' data packs
  into FEWER cells.  This is why the drain must go to completion per
  victim: a half-drained victim costs a slot and frees nothing.

- **A compaction mechanism has a state invariant, not just a happy
  path.**  Attempts 1 and 2 both worked on their first pass and failed
  on their second.  "Does it work" was the wrong question; "what state
  does it leave behind for the next invocation" was the right one.  Any
  pass that bootstraps from a scarce resource must return it.

- **Overwrite churn fragments a bump allocator badly.**  7.3 MiB of
  live data in a 21 MiB directory -- 2.9x amplification -- with every
  cell individually defensible.  Nobody was "leaking"; the free path
  linked sparse cells exactly as designed.  The pathology is emergent
  from the invariant, not a bug in any one function.

- **Sequential full-set overwrite cannot fragment a bump allocator.**
  Freeing tracks allocation, cells drain, tier-1 returns them inline.
  Measured at zero directory growth.  The gate that finds this class of
  bug needs RANDOM scatter, not a uniform pass.

- **`memparse()` treats a bare number as bytes.**  `disksize="16"` is
  16 bytes, not 16 MiB.  Two drivers, two different symptoms from one
  mistake: cellzram -EINVAL (its page-alignment check), zram ENOSPC on
  every write (it accepted a 16-byte device).

#### Process

- **The raw-stat-line rule caught a test bug on run 1, again.**  Had
  the parsed columns been trusted, "16 MiB device fails with ENOSPC"
  would have looked like a capacity bug worth a session.  `orig=4096`
  closed it in one glance.

- **A negative result redirected the design before it cost a build.**
  The sequential-overwrite version passing on the baseline kernel was
  more informative than a failure would have been: it said the premise
  ("churn accumulates cells") was true only for a specific access
  pattern, and named it.

- **Instrument, do not re-theorise -- the third time this project has
  paid for the rule.**  Two of this session's three hypotheses were
  wrong (cells not sparse; the reservation being sufficient).  The
  `pr_err` on the failure path named both in one build cycle.  The
  throttled, aggregated print (`nv`, `moved`, `returns`, `pages_used`)
  is the shape that works: enough to localize, not enough to bury the
  serial console.

- **A/B against the pre-change kernel first.**  The baseline run is
  what proved the gate reproduces at all; without it, attempt 1's
  "no change" would have been ambiguous between a broken gate and a
  broken fix.

### What We Decided

- **cellzram compacts on exhaustion instead of failing writes.**
  `shmem_comp_store()` runs one GC pass when `cell_store()` runs out of
  slots, retries once, then fails honestly -- the same contract zram
  has for a genuinely full device.  One pass, not a loop: an unbounded
  retry turns memory pressure into a livelock on the write path.

- **The directory allocates `max_cells + 1` slots** (`cp->nslots`), the
  extra one compaction-only.  Stores still fill all `max_cells`, so the
  advertised capacity from session 24 is unchanged.  A "stop one short"
  reservation was rejected in favour of this: it silently costs the
  caller a cell of capacity for a mechanism most pools never need.

- **GC drains victims sequentially and to completion** (`GC_BATCH` ->
  `GC_MAX_VICTIMS`), and drains the compaction slot before returning.
  The batched shape is deleted, not kept as an option: it is a fixed
  point, not a tuning trade-off.

- **A drain that moves nothing stops the pass.**  Otherwise the next
  pick returns the same re-linked cell and the pass spins.  Bounded by
  `GC_MAX_VICTIMS`; the shrinker re-invokes if sparse cells remain.

- **The cell path requires a sleepable caller.**  Documented on
  `shmem_comp_store()` rather than enforced: every current caller
  meets it (cellzram kmallocs its bounce buffer with `GFP_NOIO` in
  this very path; tmpfs compresses from `write_end` or the aging
  work), and GC cannot recurse into the store path because it
  relocates through `cell_relocate_one()`, which never re-enters it.

### Rules Added

- **A compaction mechanism must state its state invariant, not just
  its action.**  If a pass bootstraps from a scarce resource (a slot, a
  buffer, a lock), say what returns it and when.  A pass that works
  once and stalls on the second invocation is the signature of a
  missing invariant.

- **Evacuating one unit into another unit of the same size is
  slot-neutral.**  Before designing a compaction pass, check that
  several victims can pack into FEWER destination units than they
  occupy -- otherwise the pass moves data and frees nothing.

- **A gate for allocator fragmentation must scatter its frees.**
  Uniform overwrites of a fixed set are self-healing in a bump
  allocator; random block selection is what fragments.  If a
  fragmentation gate passes on the first try, suspect the access
  pattern before trusting the allocator.

- **`memparse()` reads a bare number as bytes.**  Any sysfs store that
  takes a size must write an explicit unit or a byte count.

### What We'd Do Differently

- Would have instrumented on the FIRST failure instead of reasoning to
  a confident, wrong conclusion ("the cells are not sparse").  The
  instrumentation took one build cycle and settled three questions at
  once; the reasoning took two.

- Would have asked "what state does this leave for the next pass"
  before implementing attempt 1.  Both failures were second-pass
  failures, and neither attempt's design mentioned a second pass.

- Would have written the churn gate with random scatter from the
  start.  The sequential version was a natural first draft and it
  cannot reproduce the bug it was written for -- a gate should encode
  the failure mode, not the happy path.

- Would have named the exhaustion path's three failure modes
  (no slot / no memory / nothing sparse) in the #3b design note.  The
  retry handles all three but they want different diagnostics, and
  today they are indistinguishable in the output.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cell_store.c` | `cell_make_active(cp, len, for_gc)`; allocate `nslots = max_cells + 1`; `cell_free`/`cell_load` bounds follow |
| `include/linux/cell_store.h` | invariant #6 (the compaction slot); `nslots`; `GC_BATCH` -> `GC_MAX_VICTIMS` |
| `mm/shmem_compress.c` | `shmem_comp_drain_victim()`; sequential drain + compaction-slot drain in `shmem_comp_cell_gc()`; GC-on-exhaustion retry in `shmem_comp_store()` |
| `~/tmpfs-compress-test/tests/test_churn.c` | new: random-scatter overwrite churn, cellzram vs zram, raw `mm_stat` dump |
| `~/tmpfs-compress-test/guest/init.sh` | `test.churn=1` isolation gate |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | `TEST_CHURN`, `TEST_HEADROOM` passthrough |

### Test Results

**The gate (churn, 64 passes x 2048 blocks, 16 MiB device):**

| kernel | cellzram | zram |
|--------|----------|------|
| baseline | 122575 write errors, errno=5, 2048 bad, 0 verified | clean |
| this change | **0 write errors, 2048 verified** | clean |

cellzram `phys` 22020096 (84 x 256 KiB = the cap) -> 20971520 (20 MiB),
live data unchanged at ~7.3-8 MiB.  Write throughput 154.5 MB/s
(baseline, failing) -> 12.8 MB/s (attempt 2, GC stalled) -> **124.7
MB/s** (landed).

**Instrumentation trail** (removed before commit):

```
attempt 1: exhaust sparse=66 occupied=83/84 -> gc nv=8  moved=64
           exhaust sparse=65 occupied=84/84 -> gc nv=8  moved=0
attempt 2: exhaust sparse=67 occupied=84/84 -> gc nv=64 moved=1039 returns=64 pages_used=2368 (37 cells)
           exhaust sparse=69 occupied=85/85 -> gc nv=1  moved=0   returns=64 pages_used=5440 (85 cells)
attempt 3: exhaust sparse=67 occupied=84/84 -> gc nv=64 moved=1103 returns=65  pages_used=2368
           exhaust sparse=69 occupied=84/84 -> gc nv=64 moved=1329 returns=130 pages_used=2560
           exhaust sparse=66 occupied=84/84 -> gc nv=64 moved=1252 returns=197 pages_used=2496
```

**Full acceptance, all green, exit 0:**

| gate | result |
|------|--------|
| churn | 2 passed / 0 failed |
| cellzram suite | 77 / 0 |
| randio | 4 / 0 (unchanged -- capacity fix still holds) |
| fsdev | 19 / 0 |
| torture | 231 / 0 |
| entryrace (QEMU_CPUS=4) | 0 underflows, quiescent verify PASS x4, stale_value_retries 0 |
| full suite (QEMU_CPUS=4, KASAN) | exit 0 |
| `CONFIG_TMPFS_COMPRESSION=n` build | `mm/shmem.o` clean, 0 warnings (both modified objects are config-gated and absent) |

Note: `mm/shmem.c` is untouched, so the off-n gate is a confirmation
rather than a risk.

### Next Steps

1. randio and churn both belong in the per-commit rotation now; churn
   is the only gate that feeds random-scatter incompressible churn.
2. `cell_headroom_pct` is still an unmeasured 30.  With compaction
   working, the right value probably drops -- the headroom exists to
   absorb exactly the churn this pass now drains.  Sweeping it is the
   next session.
3. Watch the compaction cost in the field: the landed gate runs at
   124.7 MB/s vs the baseline's 154.5, so churn-heavy workloads pay
   ~20% for correctness they previously paid as IOERR.  If that
   matters, the lever is `GC_MAX_VICTIMS` and the per-pass budget, not
   the mechanism.
4. The retry's three failure modes (no slot / no memory / nothing
   sparse) are still indistinguishable in the output; a counter for
   each would make the next diagnosis one build cheaper.

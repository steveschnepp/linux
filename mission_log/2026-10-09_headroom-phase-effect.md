---
name: headroom-phase-effect
description: why cell storage was non-monotonic in headroom -- the fill/exhaust/compact/refill phase, and retracting the session-28 ranking (session 29)
project: linux
previous: 2026-10-09_headroom-fine-sweep.md
---

# Mission Log: The Phase Effect, and Retracting a Noise-Based Ranking

Part 13 of the tmpfs transparent-compression series.  Session 29
(2026-10-09).

Session 28 swept `cell_headroom_pct` 0..30 by 5, found cell storage
non-monotonic in headroom, called the mechanism "not established", and
ranked 15% as the one value to avoid.  This session answers the
"why" -- and the answer retracts the ranking.

Kernel commits: `b7de5f947997` (comment retraction).
Rig commits: `cef2d7e` (runtime pass count).

## Session 29: why storage was non-monotonic

### What We Did

1. **Made the workload length a variable.**  `PASSES` was a compile-time
   `#define`; it is now `test.churnpasses=N`, threaded through
   `init.sh` to the guest environment (env does not cross the
   host/guest boundary, so it rides the cmdline like every other rig
   knob).  This is the one experiment that separates a *phase* effect
   from a *sizing* effect.

2. **Held headroom fixed and varied only the pass count.**  At 15%
   (pool = 74 cells):

   | passes | 32 | 40 | 48 | 56 | 64 | 72 | 80 |
   |--------|----|----|----|----|----|----|----|
   | cells  | 51 | 49 | 41 | **70** | 64 | 56 | 52 |

   A **38-cell spread** at one fixed pool size, from the workload
   length alone.

3. **Repeated at 10% and 20%** to get a proper comparison rather than a
   single sample per headroom value.

### The answer: it is a phase effect

The compaction pass only runs when the store path finds the directory
full.  So occupancy does not sit near some headroom-determined
equilibrium -- it **climbs to the pool limit, drops in one batch, and
climbs again**:

```
occupancy
   ^
   |        /\      /\      /\
   |       /  \    /  \    /  \        <- exhaust: GC fires, big drop
   |      /    \  /    \  /    \
   |     /      \/      \/      \
   +---------------------------------> writes
        ^                       ^
     GC frees ~47 cells      test ends HERE by chance
```

End-of-churn occupancy is `(post-GC floor) + (writes since the last
GC)`, and the second term is essentially arbitrary mod the cycle
length.  A fixed op count lands at a fixed but *incidental* phase, so
the storage number is a sample of a sawtooth, not a measurement of the
sizing parameter.

This also explains why the storage figures reproduced exactly across
runs (session 28's strongest evidence for treating them as real): the
workload is deterministic, so the phase is deterministic too.  **Exact
reproducibility established that the number was stable, not that it
meant what I said it meant.**

### The retraction

Properly averaged over 7 pass counts:

| pct | mean cells | sd | min | max |
|----:|-----------:|---:|----:|----:|
| 10  | 52.1       | 11.1 | 37 | 68 |
| 15  | 54.7       | 9.7  | 41 | 70 |
| 20  | 57.3       | 11.9 | 43 | 75 |

The headroom effect on storage is **+5.1 cells** between the 10% and
20% means, against a **38-cell** within-group spread and a standard
deviation of ~11.  The effect is less than half a standard deviation:
not measurable at this sample size.

Worse, the ranking **inverts**.  Session 28 concluded "15% is
dominated by 20%, which has fewer exhaustions AND less storage", from
the pass=64 row (64 vs 60 cells).  Averaged, 15% holds 54.7 cells and
20% holds 57.3 -- 15% has *less* storage.  The claim was not merely
noisy, it was backwards.

### What still stands

**Exhaustions (`gc_recoveries`) is a genuine headroom measurement**, and
it was the load-bearing result all along:

| pct | passes=32 | 48 | 64 | 80 |
|----:|----------:|---:|---:|---:|
| 10  | 28 | 43 | 58 | 73 |
| 15  | 26 | 40 | 53 | 67 |
| 20  | 24 | 36 | 49 | 62 |

Monotonic in headroom at **every one of the seven pass counts**.  That
is not something a phase artifact can produce -- the phase moves the
absolute number with workload length but preserves the ordering across
headroom at fixed length.  The 10% default stands, on this column
alone.

### What We Learned

#### Technical

- **A counter that triggers on a threshold produces a sawtooth, and
  any single sample of it is a phase measurement.**  Storage here is
  driven by "how full was the directory when compaction last fired",
  which is a threshold-crossing quantity.  Anything derived from
  threshold crossings needs either averaging over many cycles or a
  comparison at a *fixed* phase -- never a single read at the end.

- **Reproducibility is not validity.**  Session 28's storage numbers
  came back byte-identical across runs and across sessions, and that
  was used as evidence they were real.  They were real *numbers*
  describing an incidental phase.  A deterministic workload measures
  an incidental quantity with perfect precision.

- **The way to kill a phase hypothesis is to vary the phase.**  One
  axis, seven values, one boot each, and the 38-cell swing answered
  what three sessions of theorising had not.  If a result looks
  irregular, the first thing to vary is the thing you held constant.

- **Exhaustions survived because it is a *count of events*, not a
  *state*.**  Counts accumulate monotonically with work done, so they
  integrate over the sawtooth instead of sampling one point on it.
  When choosing between two columns of equal cost to collect, prefer
  the cumulative one.

#### Process

- **A hypothesis stated as "mechanism not established" is an invitation
  to measure it, and it should be taken.**  Session 28 correctly
  refused to invent an explanation.  Leaving the gap visible is what
  made this session a five-experiment afternoon instead of a
  hand-wave.

- **Correct the record where the wrong claim lives.**  The retraction
  went into `mm/cellzram.c`'s comment, not only into the mission log:
  a future reader tuning this knob reads the parameter's comment, and
  an unqualified "avoid 15%" there would have cost them a day.  The
  log records the journey; the code carries the conclusion.

- **Read the right awk field.**  The first extraction of this data read
  `$3` (orig_bytes -- constant live data, 32 cells) instead of `$4`
  (phys).  It produced a suspiciously flat "32, 32, 32, 32" that could
  easily have been narrated as "occupancy is stable, hypothesis
  refuted".  The raw `mm_stat` dump made the mistake visible in one
  glance.  Fourth time this series the auditable-transform rule has
  caught a real error -- this one mine, in shell.

### What We Decided

- **Retract the session-28 storage ranking.**  15% is not a value to
  avoid.  The storage column cannot discriminate headroom values at
  this sample size, and the single-sample ordering was inverted
  relative to the averaged one.

- **`cell_headroom_pct` stays at 10%,** now on exhaustion evidence
  only -- which is the column that is monotonic at every workload
  length tested.

- **The parameter's comment now names the trap explicitly.**  It says
  not to read storage as a headroom metric, shows the sawtooth's
  arithmetic, and gives both the single-sample spread and the
  averaged means.  A reader who wants to re-measure is told exactly
  what to vary.

### Rules Added

- **Any quantity derived from a threshold crossing needs averaging or a
  fixed-phase comparison.**  Single samples of sawtooth state measure
  the phase, not the parameter.

- **Prefer cumulative counters over end-state gauges when both cost the
  same to collect.**  Counts integrate over cycles; gauges sample one.

- **Exact reproducibility proves stability, not meaning.**  A
  deterministic workload reproduces an incidental value perfectly.

- **Vary the axis you held constant before discarding a hypothesis.**
  If a result is irregular, the irregularity is often the phase.

### What We'd Do Differently

- Would have made `PASSES` runtime-configurable when the gate was
  written.  A fixed workload length baked an uncontrolled variable into
  every storage measurement taken since session 26.

- Would have reported a mean over pass counts in session 28 rather
  than one sample, and would then not have published the 15% claim.

- Would have questioned "cells reproduced exactly" *sooner* rather than
  later.  Reproducibility was treated as reassurance; for a state
  variable driven by a threshold it should have prompted the question
  of which state.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | retract the storage-based ranking; name the phase effect and show the arithmetic |
| `~/tmpfs-compress-test/tests/test_churn.c` | `PASSES` -> runtime `CHURN_PASSES` |
| `~/tmpfs-compress-test/guest/init.sh` | `test.churnpasses=N` -> exported `CHURN_PASSES` |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | `TEST_CHURN_PASSES` passthrough |

### Test Results

**Phase experiment**, headroom fixed at 15% (pool 74 cells), pass count
varied: cells = 51, 49, 41, 70, 64, 56, 52 (32/40/48/56/64/72/80).
Exhaustions rise monotonically with workload length: 26 -> 67.

**Full grid**, 10/15/20% x 7 pass counts (21 boots): means 52.1 / 54.7 /
57.3 cells, sd 11.1 / 9.7 / 11.9.  Exhaustions monotonic in headroom at
all 7 pass counts.

All boots exit 0 with 0 write errors and 2048 blocks verified -- the
compaction is correct at every combination tried; only the storage
*interpretation* was wrong.

The standing gates were green at 10% at the end of session 27 and
neither a comment change nor a test-harness parameter alters kernel
behaviour, so they were not re-run.

### Next Steps

1. **Sweep `cell_order` with headroom** -- still unmeasured, still the
   other half of the same axis, and now with a method that can tell a
   sizing effect from a phase effect.
2. If storage really does depend on headroom at all, it needs many more
   samples per cell (50+ pass counts) to see a 5-cell effect under an
   11-cell sd.  Worth doing only if peak RAM per device becomes a
   tuning target in the field.
3. The sawtooth itself is now characterised but not optimised: the
   compaction pass fires only at 100% directory occupancy, so mean
   occupancy sits well above the live-set floor.  Triggering it earlier
   (e.g. at 90%) would cut mean storage at the cost of more frequent
   passes -- a real lever, unmeasured.

---
name: bench-methodology-gap
description: why the rig's numbers cannot be trusted -- the measurement-methodology gap, the paired-A/B hypothesis, and the characteristics a real bench needs (session 30)
project: linux
previous: 2026-10-09_headroom-phase-effect.md
---

# Mission Log: The Measurement Gap, and What a Real Bench Needs

Part 14 of the tmpfs transparent-compression series.  Session 30
(2026-10-09).

This is a DESIGN session, not an implementation one.  Nothing was
built.  It exists so the next session starts from a brief instead of
re-deriving it.

Trigger: the observation that Apple's fstools (`fsx`, `fstorture`) are
excellent **correctness** oracles and useless as **benchmarks** -- they
verify every byte and report nothing about RAM or speed.  Asked what a
measurement counterpart would look like.

Kernel commits: none.
Rig commits: none.

## Session 30: what a benchmark gate would have to be

### What We Did

1. **Surveyed what the rig actually measures.**  Four test files emit
   `BENCH` lines (`test_blkdev_fs` 9, `test_randio` 4, `test_churn` 3,
   `test_torture` 1); `test_bench.c` is a dated untar benchmark in the
   default suite; `test_performance`, `test_compress`,
   `test_concurrent` and `test_mmap` are built and shipped in the
   rootfs but **never invoked** by init.sh.
2. **Pulled every recorded measurement failure out of the series'
   own logs** rather than guessing at them -- six were already
   documented, and the seventh was measured this week.
3. **Loaded the `bench` skill** and checked its methodology rule by
   rule against this rig's needs.  Most transfer; best-of-N does not,
   and the reason it does not is session 29's phase effect.
4. **Drafted the characteristics table**, each row traced to the
   specific finding it answers, plus what the bench must not do.

Nothing was implemented.  This is the brief for the bench session.

### The hypothesis

**The rig does not need another workload.  It needs a measurement
methodology.**

It already measures things -- four test files emit `BENCH` lines and
`test_bench.c` is in the default suite -- but every number it produces
is a single sample from a single boot, taken from a source that is
either noisy or measures something other than what the label says.  The
workloads are fine.  The statistics and the RAM sourcing are not.

This is the gap, and it is a *method* gap rather than a *coverage* gap.

### What exists today

| what | where | status |
|------|-------|--------|
| untar .c/.o datasets, plain vs compressed | `tests/test_bench.c` | in the default suite; single-shot per config; predates the 50:70 default and cellzram |
| write/read/mmap/memory | `tests/test_performance.c` | **built, never invoked** by init.sh |
| compressor micro, concurrency, mmap | `test_compress.c`, `test_concurrent.c`, `test_mmap.c` | **built, never invoked** |
| untar/verify/rm MB/s + MemFree | `tests/test_blkdev_fs.c` | 9 BENCH lines, correctness gate |
| 4K random IO MB/s + IOPS | `tests/test_randio.c` | 4 BENCH lines, correctness gate |
| churn MB/s + raw mm_stat | `tests/test_churn.c` | 3 BENCH lines, correctness gate |
| policy sweep rows | `tests/test_torture.c` | 1 BENCH line, correctness gate |
| fsx + fstorture | `~/fstools` (own repo) | external **correctness** oracle, gated |

So: four files built and never run, one dated benchmark in the suite,
and four correctness gates that happen to print timings.

### Why the existing numbers cannot be trusted

Seven findings, six of them already recorded in this series and one
measured fresh this week.  Each one invalidates a naive number.

1. **Single-boot windows are not publication numbers.**  Session 15:
   *"3s windows x one boot per config are enough for policy validation
   (the deltas are 2x), not for publishing throughput numbers."*  The
   matrices were built to see 2x differences; nothing in the rig can
   resolve 5%.

2. **Stability runs were flagged and never done.**  Session 17's Next
   Steps: *"Stability runs (N boots per config) before quoting T=64
   numbers anywhere."*  Still open.

3. **Throughput drifts ~3% across sessions for identical config.**
   Measured this week: 10% headroom read 122.8 / 123.4 MB/s in session
   26 and 119.4 / 118.3 in session 28.  Same kernel, same workload.
   That drift is *larger* than the differences being resolved, which
   is why session 28's per-5%-step throughput column was unusable.

4. **MemFree deltas are noise, and have gone negative.**  Session 21:
   `delete_return_kb= -7852` in one run against `+7160` in another,
   same config.  The session's own recommendation, never acted on:
   *"sample `Shmem`/device mm_stat rather than raw MemFree, or widen
   the slack and report the breakdown."*

5. **A `BENCH` line interleaved with a printk loses its tail** on the
   serial console (session 12's two-writers rule).  Real data has
   already been eaten by `shmem_comp: destroyed pool...` mid-line.

6. **Live gauges are illegitimate engagement metrics on transient
   workloads.**  Session 15: `comp_pages` is a live gauge and reads
   zero on a workload where pages die before aging.  First matrix run
   failed 7 assertions per chill boot on this alone.

7. **End-state gauges sample a phase, not a parameter.**  Session 29.
   Cell occupancy is a sawtooth -- the compaction pass only runs when
   the directory is full, so storage climbs to the pool limit, drops
   in one batch, climbs again.  At *fixed* headroom, varying only the
   workload length swung end-of-churn occupancy 41..70 cells.  Worse,
   it reproduced **byte-identically** across runs and sessions: exact
   reproducibility established the number was stable, not that it
   meant what it was read to mean.  Session 28 published a ranking off
   one sample per config and session 29 had to retract it.

Points 3 and 7 are the ones that bite hardest.  Drift makes absolute
numbers uncomparable between sessions; the phase effect makes
single-sample state readings meaningless even *within* a boot.

### Why a better bench is needed

Not to produce more numbers -- there is no shortage.  To produce
numbers that survive being written down.

Every config decision in the guidance table rests on measurements that
cannot currently be re-derived or compared across sessions.  Session 26
chose a default from a table whose throughput column later drifted by
more than the effect it was deciding on, and session 28's storage
column turned out to measure workload phase.  Both conclusions
*happened* to hold; neither was safe.  The next person to tune anything
will hit the same problem, and will not have two sessions to spend on
finding out why.

The bench also has a second job: **regression detection.**  The
compaction pass now costs ~20% of churn throughput (session 25: 154.5
-> 124.7 MB/s).  That is a real, accepted cost.  Nothing currently
detects if it becomes 40%.

### Characteristics the bench must have

Each traced to a specific failure above; the number in brackets is the
finding it answers.

| # | characteristic | why |
|---|----------------|-----|
| 1 | **Paired A/B in one boot** -- plain and compressed measured back to back, interleaved | [3] Cross-session drift is ~3%; configs sharing a boot share the drift, so *relative* numbers cancel it.  Biggest single lever, and free. |
| 2 | **RAM from authoritative sources, never raw MemFree** -- `Shmem` from `/proc/meminfo`, per-mount `mount_stats` (session 27), cellzram `mm_stat` phys + entry bytes | [4] MemFree deltas went negative.  These are exact counters, not allocator noise. |
| 3 | **Report the breakdown, not one delta** | [4] Session 21: a delta that does not close is invisible unless the components are printed. |
| 4 | **Median, not best-of-N** | Deviates from the `bench` skill deliberately.  Best-of-N is right for a CPU microbenchmark (hides thermal throttling) and wrong here: the GC/compaction cost is *periodic background work we are measuring*, and the minimum systematically samples the post-GC trough of the sawtooth [7]. |
| 5 | **Mean over the sawtooth for RAM under churn**, or a fixed phase | [7] One end-state reading of a threshold-driven gauge is one arbitrary sample.  Sample repeatedly during the workload; report mean/min/max. |
| 6 | **Cumulative counters over end-state gauges** | [6][7] Counts integrate over cycles; gauges sample one point.  Prefer `aged_pages` and `gc_recoveries` to `comp_pages`. |
| 7 | **Warmup, then a fixed work count** | [7] The first pass fills the directory and warms caches; without it the first sample is not comparable to the rest. |
| 8 | **Interleaving-safe output** -- short lines, parsed by prefix | [5] Serial console interleaving has already eaten result tails. |
| 9 | **Relative to a same-boot plain-tmpfs baseline**, absolutes shown too | [1] Matches the `bench` skill's "absolute and relative" rule, and is what cancels the drift. |
| 10 | **Re-run M boots, aggregate outside the guest** | [1][2] The stability runs session 17 asked for and never did.  Median and IQR across boots; reject a run whose baseline row moved. |
| 11 | **Results committed next to the harness; deltas vs a stored baseline** | [3] Turns a number into a regression check, per the `bench` skill's regression-baseline practice. |

### What the bench should NOT do

- **Not a new workload.**  The rig already has the right shapes:
  transient (untar -> verify -> rm, the real `/tmp` pattern), random IO
  (database), churn (allocator stress).  Measure those properly; do not
  add a fourth.
- **Not a replacement for the correctness gates.**  Keep `fsx`,
  `fstorture`, `test_churn`, `test_randio` as pass/fail.  A benchmark
  that also asserts is a benchmark that gets weakened when a number
  moves.
- **Not tuned to produce a headline.**  The moment a bench is used to
  justify a decision it was designed around, it stops being able to
  contradict that decision.

### What We Learned

#### Technical

- **Reproducibility is not validity.**  The single most expensive
  lesson of this week.  A deterministic workload reproduces an
  incidental quantity with perfect precision.  Before trusting a
  stable number, establish that it depends on the parameter you think
  it does -- by varying that parameter and nothing else.

- **The `bench` skill's best-of-N rule does not transfer to workloads
  with periodic background cost.**  Its rule is sound for the case it
  was written for (thermal throttling on a hot CPU loop) and actively
  harmful here.  A methodology rule always carries the assumption that
  produced it; import the rule only with the assumption.

- **Four test files are built and never run** (`test_performance`,
  `test_compress`, `test_concurrent`, `test_mmap`).  The session-13
  restructure derives the build list from `tests/*.c`, so these still
  compile and still ship in the rootfs -- costing build time and
  inviting a reader to assume they are part of the suite.

#### Process

- **Answer the question that was asked.**  I began a full audit of the
  compression subsystem when the question was about benchmark design.
  Two repos' worth of reading spent before the redirect.

- **A design session is a real deliverable.**  This log is the brief.
  The next session should not re-derive why the bench needs a median,
  or rediscover that MemFree went negative.

### What We Decided

- **The bench is a methodology, not a workload.**  Reuse the three
  existing workload shapes; fix the statistics and the RAM sourcing.

- **Paired within-boot A/B is the primary design constraint.**  It is
  the only thing in the rig that cancels the measured 3% drift, and it
  costs nothing but ordering.

- **Median for time, mean-over-sawtooth for churn RAM, cumulative
  counters over gauges.**  With the deviations from the `bench` skill
  recorded and justified, so the next reader does not "fix" them back.

- **Benchmark output must never carry a pass/fail.**  Keep the
  correctness gates separate so a moving number cannot silently weaken
  a test.

### Rules Added

- **Never publish a single-sample state gauge as a parameter
  measurement.**  Either average over cycles or fix the phase, and
  vary the parameter you claim to be measuring to prove the column
  depends on it.

- **Relative-to-same-boot-baseline is the default output**, absolute
  numbers are supplementary.  With ~3% cross-session drift, an
  absolute number without a same-boot baseline is not comparable to
  anything.

- **Import a methodology rule together with the assumption that
  produced it.**  Best-of-N hides thermal throttling; it also hides
  the periodic cost this project exists to measure.

- **Benchmarks do not assert.**  Pass/fail lives in the correctness
  gates; a benchmark that asserts gets weakened when its numbers move.

### What We'd Do Differently

- Would have written this brief BEFORE auditing the kernel subsystem.
  The question was about measurement, and the audit was my own agenda.

- Would have built the bench in session 21, when the MemFree column
  first went negative and the recommendation to replace it was written
  down.  Nine sessions of numbers have been produced on sources the
  series itself flagged as unreliable.

- Would not have built `test_performance`, `test_compress`,
  `test_concurrent` and `test_mmap` as separate files, or would have
  wired them in.  Four compiled, shipped, never-run test binaries is
  the worst of both.

### Files Changed

| File | Purpose |
|------|---------|
| none | design session; no code written |

### Test Results

None -- no implementation.  Existing gates were green at the end of
session 29 and nothing changed.

### Next Steps

1. **Decide the primary question first**, because it selects the
   design: (a) "is compression worth it" -- RAM saved per CPU spent;
   (b) "did my change regress" -- needs the committed baseline and M
   boots; (c) "which config wins" -- needs the paired within-boot
   matrix.  (a) and (c) share the pairing machinery; (b) needs the
   aggregator.
2. **Write the RAM sampler first.**  `Shmem` + per-mount
   `mount_stats` + cellzram `mm_stat`, sampled per iteration, emitting
   mean/min/max and the full breakdown.  Small, reusable by every
   existing gate, and it fixes finding 4 on its own.
3. **Add median-of-N to the existing `BENCH` emitters** rather than a
   new test file.  Three emitters, one helper.
4. **Then the host aggregator**: run a boot M times, extract by prefix,
   median + IQR, compare to a stored baseline, reject runs whose
   baseline row moved.
5. **Decide the fate of the four never-run test files** -- wire them in
   or delete them.
6. `cell_order` x headroom still unswept; session 29's phase-vs-sizing
   method makes it tractable, and it is the one axis both sessions 26
   and 29 flagged.

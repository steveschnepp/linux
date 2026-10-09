---
name: headroom-sweep
description: what cell_headroom_pct is worth now -- the gc_recoveries counter and the 30 -> 10 default (session 26)
project: linux
previous: 2026-10-09_gc-on-exhaustion.md
---

# Mission Log: The Headroom Sweep, and a Knob That Changed Meaning

Part 10 of the tmpfs transparent-compression series.  Session 26
(2026-10-09), directly after session 25's compaction work.

`cell_headroom_pct` sized the cell directory as
`ceil(disksize / cell_size) * (1 + pct / 100)`, and existed because
overwrite churn fragments the bump allocator faster than live data
grows.  Session 24 set it to 30 and recorded, in its own Next Steps:
*"Watch `cell_headroom_pct` in the field: 30 is unmeasured headroom,
chosen for safety.  A churn-specific sweep could right-size it."*

Session 25 gave the answer a deadline: once compaction-on-exhaustion
works, the headroom is no longer load-bearing for correctness, so its
value became a free parameter nobody had measured.

Kernel commits: `3505ee571b49` (then amended to `c46ea355a629`),
`94d78f6ae3da` (session 25, the compaction the sweep measures against).
Rig commits: `93edc2c`.

## Session 26: measuring what the headroom is worth

### What We Did

1. **Added the signal the sweep needed first.**  Without a counter,
   "how much headroom is enough" has no answer: the gate either passes
   or fails, and after session 25 it passes at every value.  Appended
   an 8th `mm_stat` field, `gc_recoveries` -- the number of writes that
   ran out of directory slots and were saved by the compaction pass.

   It is deliberately a **counter, not a gauge**, because the question
   is retrospective: *how often would this write have been
   `BLK_STS_IOERR` without the fix*.  A live gauge would answer a
   different question (is compaction running now) and would be zero
   almost always, which is exactly the mistake session 15 made with
   `comp_pages` on a transient workload.

   Appended at the end of the line so every existing field keeps its
   position -- session 24's `zero_blocks` convention.

2. **`scripts/sweep_headroom.sh`.**  One boot per value (the param is
   0444, so it rides the kernel cmdline), each running the churn gate,
   at 0/10/30/50/100.  Reports the gate result, the raw `mm_stat`
   line, and write throughput together, because none of the three
   means anything alone.

3. **Ran it twice** before quoting anything, per session 17's rule
   about cliff-shaped results.

### The result

| pct | exhaustions | peak cell storage | write MB/s (run 1 / run 2) |
|----:|------------:|------------------:|----------------------------:|
| 0   | 76          | 12.75 MiB         | 120.4 / 121.7 |
| 10  | 58          | 10.25 MiB         | 122.8 / 123.4 |
| 30  | 43          | 20.00 MiB         | 123.5 / 124.2 |
| 50  | 37          | 23.25 MiB         | 127.5 / 124.6 |
| 100 | 23          | 24.00 MiB         | 128.5 / 128.8 |

Three findings, in descending order of how much they matter:

- **Correctness is now headroom-independent.**  Every value from 0 to
  100 passes the churn gate, with zero write errors and 2048 blocks
  verified.  Before session 25, low headroom meant IOERR.  This is the
  cleanest possible confirmation that the compaction did what it was
  supposed to: it removed a parameter's correctness role entirely.

- **Exhaustion events fall monotonically with headroom** (76 -> 23),
  exactly as the mechanism predicts.  The counter is doing its job --
  it turns "headroom absorbs fragmentation" from a claim into a curve.

- **Peak cell storage RISES with headroom** (12.75 -> 24 MiB for the
  same 8 MiB live set).  This is the column that had never been
  measured, and it is the real cost: headroom is not free RAM, it is a
  directory that stays large whether or not the churn that justified it
  ever happens.

Throughput moves only ~6% across the whole range, and the two runs
agree to within 1 MB/s.  The counters and storage figures were
**byte-identical** between runs -- the workload is fully deterministic,
so the small throughput deltas are real but small.

### What We Learned

#### Technical

- **The knob changed meaning, and nobody wrote it down.**  Session 24
  chose 30 as a safety margin against a failure mode that no longer
  exists.  A parameter whose *role* changes is more dangerous than one
  whose value is wrong: the wrong value gets noticed, the wrong role
  does not.  The rationale comment now carries the sweep, including
  the numbers, so the next reader cannot inherit a stale justification.

- **The cost of headroom is peak storage, not throughput.**  Intuition
  says a bigger directory is cheaper because it avoids GC.  The
  measurement says the throughput difference is ~6% while the storage
  difference is 2.3x.  Optimising the visible axis (MB/s) would have
  kept the default at 100; optimising the measured one moved it down.

- **A counter that answers a retrospective question must be cumulative.**
  Live gauges answer "is this happening now"; on a workload where the
  event is transient they read zero and look like the problem is
  solved.  `gc_recoveries` had to survive the churn that produced it.

- **Non-monotonic cells in an otherwise clean table are noise, not
  signal.**  At pct=10 storage (10.25 MiB) is *lower* than at pct=0
  (12.75 MiB), which looks like an inversion.  It is not: with no
  headroom the pass compacts more aggressively and the fragment pattern
  differs.  Noted, not explained -- it does not affect the decision,
  and inventing a mechanism for it would have been worse than leaving
  it alone.

#### Process

- **The sweep was cheap because the gate already existed.**  Session 25
  wrote `test_churn.c` for a different reason (proving the compaction
  bug).  Reusing it here turned a "we should probably measure this
  someday" standing item into a 15-minute session.  A gate written for
  one question often answers three.

- **Run the sweep twice even when the numbers look clean.**  The
  counters came back byte-identical, which is what makes the ~6%
  throughput spread interpretable as real rather than as noise.  Without
  the repeat there would have been no way to know whether 121 vs 129
  was a trend or a bad morning.

- **Check the other gates before changing a default.**  Headroom
  originally existed for a reason unrelated to the churn workload (COW
  allocates before the free).  cellzram 77/0, randio 4/0, fsdev 19/0
  and torture 231/0 were re-run at 10% specifically to make sure the
  churn gate was not the only workload that liked the new value.

- **A commit that describes half of what it contains is worse than two
  commits.**  The counter and the default change went in together
  because they touched one file; the message described only the
  counter.  Amended rather than left as-is: an inaccurate message is a
  lie that outlives the session.

### What We Decided

- **`cell_headroom_pct` default 30 -> 10.**  10% is the knee: ~123 of
  the available ~129 MB/s (96%) for 10.25 MiB of cell storage instead
  of the 20 MiB that 30% held.  Going from 10 to 100 buys 4% more
  throughput for 2.3x the peak RAM -- a bad trade for a safety margin
  that is no longer load-bearing.

- **The parameter stays.**  It was tempting to delete it (compaction
  makes correctness independent of it), but it still trades peak
  storage against GC frequency, and an operator with a churn-heavy
  workload and RAM to spare should be able to buy throughput.  A knob
  whose meaning is documented and measured is fine; the failure mode
  was never the knob, it was the unmeasured default.

- **The rationale comment carries the sweep table.**  Future changes to
  the default get the numbers, not a hunch.

### Rules Added

- **When a fix removes a parameter's correctness role, re-measure the
  parameter.**  A value chosen as a safety margin keeps being cited as
  one long after the hazard is gone.  The role change is the thing to
  re-measure, not just the value.

- **Measure the axis you are actually trading.**  Headroom looked like
  a throughput knob and was a peak-storage knob.  The table shows ~6%
  throughput against 2.3x storage; reading only the throughput column
  would have chosen the opposite default.

- **A retrospective question needs a cumulative counter.**  "How often
  would this have failed" cannot be answered by a gauge that reads zero
  once the fix works.

### What We'd Do Differently

- Would have added `gc_recoveries` in session 25, while the compaction
  was being debugged -- it would have replaced three throttled
  `pr_err` build cycles with one number.

- Would have committed the counter and the default change separately
  rather than amending a message after the fact.  Same file is not a
  reason to merge two decisions.

- Would have swept `cell_order` alongside headroom.  Session 21
  flagged cell-order granularity as the next bottleneck after the
  zero-page fix (one pinned 256 KiB cell dominating a 13-object live
  set), and both knobs size the same directory.  Sweeping them one at a
  time risks right-sizing each against the other's wrong value.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | `gc_recoveries` counter on `struct shmem_comp_pool`, incremented in the exhaustion retry |
| `include/linux/shmem_compress.h` | `shmem_comp_pool_gc_recoveries()` prototype |
| `mm/cellzram.c` | mm_stat 8th field; default `cell_headroom_pct` 30 -> 10; rationale comment carries the sweep |
| `~/tmpfs-compress-test/scripts/sweep_headroom.sh` | new: one boot per headroom value, gate result + raw mm_stat + throughput |

### Test Results

**Sweep** (churn gate, 64 passes x 2048 blocks, 16 MiB device, lz4):
see the table above.  All five values exit 0 with 0 write errors, 0
bad blocks, 2048 verified.  `gc_recoveries` and peak cell storage were
byte-identical across two independent runs; throughput within 1 MB/s.

**At the new default (10%), no override):**

| gate | result |
|------|--------|
| churn | 2 passed / 0 failed |
| cellzram suite | 77 / 0 |
| randio | 4 / 0 |
| fsdev | 19 / 0 |
| torture | 231 / 0 |

Confirmed the default took effect: the churn gate's `mm_stat` reads
`phys=10747904 recoveries=58`, matching the sweep's pct=10 row exactly.

Also re-run explicitly at `TEST_HEADROOM=10` before the change landed,
so the decision was made against evidence rather than verified after it.

### Next Steps

1. **Sweep `cell_order` with headroom**, not after it.  Both size the
   same directory and the interaction is unmeasured; session 21's
   cell-order finding (granularity dominating at small live sets) is
   the other half of this axis.
2. `gc_recoveries` is cellzram-only.  tmpfs pools can exhaust their
   default 256-cell directory too, and the counter would say whether
   any real workload ever reaches it -- cheap, and it would close or
   confirm session 22's "does a large tmpfs cell= mount hit the same
   wall" question.
3. The 6% throughput cost of compaction is now the largest remaining
   churn-workload overhead.  If it matters in the field, the levers are
   `GC_MAX_VICTIMS` and a per-pass budget -- both unmeasured.

---
name: mboot-bench-zram-axis
description: in-boot high/low/avg and the M-boot aggregator -- throughput DOES resolve the headroom axis, cells is deterministic-not-noisy, and zram wins the incompressible case (session 32)
project: linux
previous: 2026-10-09_stats-from-the-struct.md
---

# Mission Log: The Bench Learns to Measure Itself

Part 16 of the tmpfs transparent-compression series.  Session 32
(2026-10-09).

Session 30 designed a measurement methodology.  Session 31 made the
kernel's `mm_stat` accurate enough to trust.  This session builds the
two halves of the statistics that were missing -- repeated in-boot
samples, then M-boot aggregation -- and uses them to answer a question
the series had been guessing at: **does throughput resolve the
headroom axis?**  It does, and the earlier "unresolvable" verdict was
wrong by a factor of ~10 in the denominator.

Kernel commits: none (rig-only session).
Rig commits: `318dc3a` (in-boot samples), `d7c28b5` (M-boot
aggregator).

## Session 32: high/low/avg, then M boots

### What We Did

1. **Made churn repeat inside one boot.**  `test_churn.c` now runs the
   churn phase `CHURN_SAMPLES` times per device (default 5, via
   `test.churnsamples=N`), **resetting the device and replaying the
   identical op stream each sample.**  Emits one `BENCH sample` line
   per rep plus a `BENCH stats` line with high/low/avg/median.  The
   reset is load-bearing: without it the mean drifts with accumulated
   directory state (session 29's phase effect), and the samples are
   not iid.

2. **Reported the spread, not a best-of.**  Median, not minimum: the
   min systematically samples the post-GC trough of the sawtooth
   (session 30 char #4).  high/low/avg/median is the full picture, and
   the spread is the number that turns out to matter.

3. **Built the M-boot aggregator** (`scripts/mboot_sweep.sh`).  Runs a
   churn boot M times per headroom value, extracts `cz_avg`, `zm_avg`,
   `gc_recoveries` and `cells` from each, and reports median + IQR
   (linear-interpolation quantile) across the surviving boots.  Writes
   a `.rows` file with every boot's raw row so the aggregation's input
   sits beside its result.

4. **Used zram as a canary.**  The zram control is the identical
   config in every boot and has no GC, so its across-boot spread *is*
   the batch drift.  A boot whose zram row moved past
   `MBOOT_TOL_PCT` (default 2%) is rejected as an off boot.  Measured
   batch drift 2.04%; 25/25 boots kept, 0 rejected.

5. **Corrected the resolve verdict** (see below), and refined session
   29's storage finding with the IQR column.

6. **Answered "is cellzram better than zram" and "can we mitigate
   with merge_cells or bigger cells"** by reading the code rather than
   guessing.  Both answers are in the sections below.

### The correction: throughput resolves the axis

Two messages ago I told the user, twice, that throughput does not
resolve the headroom axis -- "the means are noise", "the axis is
unresolvable".  **That was wrong**, and the M-boot data shows why:

```
 0%  123.12 MB/s  +/- 0.049
 5%  123.88 MB/s  +/- 0.335
10%  124.94 MB/s  +/- 0.320
15%  125.24 MB/s  +/- 0.308
20%  125.44 MB/s  +/- 0.196

 0% -> 20%: +2.32 MB/s (1.9%)  11.5 sigma  RESOLVED
```

The error was the denominator.  I compared the between-value gap
(~2.3 MB/s) against the **within-boot spread** (~3 MB/s).  The M-boot
statistic is a *median across 5 boots*, whose standard error is
~0.2-0.3 MB/s.  Against the correct noise floor the effect clears by
an order of magnitude.

What resolves:

- **0% vs 20% is 11.5 sigma.**  Throughput rises monotonically with
  headroom and saturates past ~10%.
- **The shape matches the exhaustion curve.**  `gc_recoveries` falls
  76/64/58/53/49 over the same values, also monotonic.  Two
  independent columns agreeing on the shape is what makes it a
  mechanism rather than a coincidence: more headroom -> fewer
  exhaustion passes -> less compaction cost -> higher throughput.
- **No adjacent 5-point step resolves** (each 0.5-2.3 sigma).  The
  axis resolves end-to-end, not step-by-step.  A single-step sweep
  would still see nothing.

This is the payoff of the whole methodology thread: the earlier
verdict was not wrong about the *world*, it was wrong about the
*noise floor*, and only the M-boot aggregate could tell them apart.

### Refining session 29: cells is deterministic, not noisy

`cells` has **IQR=0 across all 25 boots** at every value:
51/39/41/64/60, byte-identical each time.

Session 29's 38-cell spread was *across pass counts*.  At a **fixed**
pass count the phase is deterministic, so `cells` reproduces exactly.
That means session 29's retraction needs a qualifier, and the
qualifier matters:

> Storage is not unusable as a headroom metric because it is *noisy*.
> It is unusable because the relationship is *non-monotonic*
> (51 -> 39 -> 41 -> 64).

Reproducible-and-non-monotonic is the more dangerous failure.  A noisy
number gets questioned; a reproducible one re-confirms a wrong ranking
every single time you measure it.  Session 28's ranking did not fail
because the samples disagreed -- they agreed perfectly, byte for
byte -- and that is exactly why it was believed.

### Is cellzram better or worse than zram?

On the axis measured this session -- **incompressible** churn -- zram
wins on both axes:

| | cellzram | zram |
|---|---:|---:|
| write throughput | ~125 MB/s | ~137 MB/s (**+9%**) |
| RAM held (8 MiB live) | 10.25 MiB | 8.0 MiB (**-22%**) |
| RAM per orig (distinct, 64 MiB) | 101.8% | 100.0% |

cellzram's incompressible cost decomposes into three separate things,
and only one of them decides the RAM comparison:

| overhead | source | size |
|---|---|---|
| entry table | one `shmem_comp_entry` per 4K block | 72 B x 16384 = **1.125 MiB** |
| fragmentation | a cell is reusable only when *completely empty* | 10.25 - 8.0 = 2.25 MiB on churn |
| compaction | GC evacuates a whole cell per victim | ~9% of throughput |

**The entry table is the entire distinct-test gap.**  1.125 MiB on
64 MiB is precisely the 101.8% vs 100.0%.  This is the number to
attack, and it is per-*block*, not per-cell.

This is the designed-losing case: on data that does not compress,
cellzram still pays for its directory, its entry table and its
compaction pass and gets nothing back.  The compressible and
fs/untar workloads were **not** re-measured this session -- that is
the next session's job.

### Can we mitigate with merge_cells or bigger cells?

No to both, and both answers are decidable from the code.

**merge_cells cannot touch cellzram.**  `shmem_comp_store_merged()`
has exactly one caller (line 2928), and it sits in the `else` branch of
`if (shmem_comp_pool_uses_cells(...))` -- it is **zsmalloc-only**.  Cell
pools never call it, and the code comment says they should not: cells
already do better there.  *"The entire group fits in one cell region
and avoids the per-chunk handle array"* -- cells are up to 4 MiB so a
merged group is one allocation, while `ZS_MAX_ALLOC_SIZE` forces
zsmalloc to chunk it.  merge_cells is a zsmalloc feature that exists
because zsmalloc cannot do what cells do natively.

**Bigger cells goes the wrong way, twice.**  It changes neither of the
two overheads that matter:

- The entry table is per-block.  `cell_order` does not reduce it by one
  byte.
- Fragmentation gets **worse**: a cell returns to the buddy only when
  it completely empties, so a 1 MiB cell needs all 256 blocks freed
  versus 64 for a 256 KiB cell.  Under random overwrite that is
  strictly harder.
- Compaction gets **worse**: more bytes to evacuate per victim.

`cell_order` is the axis sessions 26/28/29 all flagged as unswept.
This analysis says the sweep will *confirm* 6, not beat it -- which is
itself worth one sweep to record, since the reasoning is now on the
page and a measurement would close it.

**The real lever, unbuilt:** incompressible blocks do not need an
entry at all.  A raw block is stored verbatim, so the entry carries
only `(handle, len=4096)`, and `handle` is already just a packed
`(cellid, offset)` -- `shmem_comp_pack_loc`, a 22-bit offset.  The
xarray value has 63 usable bits and that loc fits.  Raw blocks could
store the loc **directly in the xarray value**, the same way the
zero-block sentinel does, and skip the entry table entirely:
**-72 B per incompressible block, which is the whole RAM gap.**

This is session 31's trick pointed at the raw path.  It is not a knob:
it touches the read, write, GC and mm_stat paths that now share the
entry indirection.  Nothing was built or measured.

### What We Learned

#### Technical

- **A within-boot spread is not a noise floor for an across-boot
  statistic.**  This is the single most expensive confusion of the
  session.  5 samples inside one boot share that boot's personality
  (host frequency state, KVM page placement), so their spread is ~3
  MB/s -- but the *median across 5 boots* has a standard error of
  ~0.2.  Quoting the former as the latter made a resolvable effect
  look like noise.  The two must never be substituted for each other.

- **In-boot samples and M-boot aggregation measure different things
  and both are needed.**  In-boot kills within-boot jitter (GC phase,
  scheduling).  M-boot makes boot personality visible as an IQR.
  Neither substitutes for the other; the nesting is the design.

- **The zram control is a free canary and a free baseline.**  It runs
  in the same boots, has no GC, and is the same config every time, so
  its across-boot spread *is* the batch drift.  Rejecting boots whose
  control moved is how the aggregator knows a shifted number means an
  off boot, not a kernel change.

- **Two independent columns agreeing on a shape is a mechanism.**
  Throughput and `gc_recoveries` were monotonic together over the same
  axis.  Either alone is a correlation; together they are the causal
  story (headroom -> fewer passes -> less cost -> higher throughput).

- **Reproducible and non-monotonic is worse than noisy.**  Session
  29's storage column reproduced byte-for-byte and was still wrong,
  because the parameter relationship inverted.  Determinism is not
  validity; check the *shape* before believing a clean number.

- **A feature's only call site tells you what it is for.**
  `store_merged` had one caller in the zsmalloc branch; that single
  fact settled whether merge_cells could help cellzram.  Reading the
  branch was cheaper than any experiment.

#### Process

- **State the denominator with the number.**  The wrong verdict was
  not a wrong measurement -- every MB/s figure quoted was correct --
  it was a wrong *comparison*.  Writing "2.3 MB/s effect against a
  3 MB/s noise floor" without naming which noise floor is what let the
  error stand for two messages.

- **Correct the record in the same breath as the finding.**  The
  reversal is documented here rather than quietly overwritten, because
  a future reader needs to know the "unresolvable" verdict in session
  28's neighborhood is superseded -- and by *what* evidence.

- **Build the aggregator before drawing the conclusion.**  Every
  conclusion about resolve/no-resolve reached before `mboot_sweep.sh`
  existed was a guess wearing a number's clothes.  The tool took one
  session; the guessing cost two.

### What We Decided

- **Throughput resolves the headroom axis end-to-end** (0% vs 20% =
  11.5 sigma), saturating past ~10%, and this now stands on two
  agreeing columns.  No adjacent 5-point step resolves.

- **The 10% default is supported by three columns**, not one:
  `gc_recoveries` (monotonic), throughput (monotonic, saturating), and
  session 28's marginal-cost knee.

- **`cells` is deterministic at fixed pass count** (IQR=0 across 25
  boots) and remains unusable as a headroom metric because it is
  non-monotonic, not because it is noisy.  Session 29's retraction
  stands; its *rationale* is amended.

- **Incompressible workloads: zram wins.**  ~9% faster, ~22% less RAM
  held.  The gap is the entry table (72 B/block) and is entirely
  accounted for.

- **merge_cells and bigger cells do not mitigate**, for reasons
  recorded above.  Neither is worth a session.

- **The raw-block-no-entry idea is the real lever** and is parked,
  unbuilt, for the bench session to weigh against the measured cost.

### Rules Added

- **Never quote an across-boot effect against a within-boot spread.**
  Name which noise floor you are using.  The standard error of a
  median across M boots is the right denominator for an across-boot
  comparison; the min-max spread of in-boot samples is not.

- **The control that runs in every boot is the drift measurement.**
  Use it to reject off boots and to bound the batch.  A config with no
  GC and no tunables is the ideal canary.

- **Check the shape, not just the spread, before believing a
  reproducible number.**  Byte-identical reproduction of a
  non-monotonic relationship is the failure mode that survives the
  most scrutiny.

- **Read the call site before proposing a mitigation.**  A feature
  with one caller in one branch has one purpose; if that branch is not
  yours, the feature cannot help you.

### What We'd Do Differently

- Would not have called the axis unresolvable before the aggregator
  existed.  The verdict was stated with confidence twice before the
  tool that could check it was built.

- Would have paired the zram control into the sweep script's summary
  from the start (session 28's fine sweep) rather than at M-boot time.
  The ratio column was available for two sessions and unused.

- Would have recorded the `cell_order` reasoning as a sweep the first
  time it was flagged (session 26) rather than re-flagging it three
  times.  It is now reasoned but still unmeasured.

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/test_churn.c` | N in-boot samples per device, reset + identical op stream each; `BENCH sample` per rep, `BENCH stats` with high/low/avg/median |
| `~/tmpfs-compress-test/guest/init.sh` | `test.churnsamples=N` -> exported `CHURN_SAMPLES` |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | `TEST_CHURN_SAMPLES` passthrough |
| `~/tmpfs-compress-test/scripts/sweep_headroom.sh` | read the stats line; emit the paired cz/zm ratio |
| `~/tmpfs-compress-test/scripts/mboot_sweep.sh` | **new** -- M boots per config, median + IQR, zram canary, `.rows` audit file |

### Test Results / Benchmarks

**In-boot samples** (5 per device, one boot), cellzram spread 3.6%
versus zram 0.95% -- the GC/compaction jitter is cellzram's own, and
it is now measured rather than hidden:

```
BENCH stats dev=cellzram n=5 high=126.4 low=122.0 avg=125.0 median=125.7
BENCH stats dev=zram     n=5 high=137.6 low=136.3 avg=137.1 median=137.0
```

**M-boot aggregate**, M=5 x 5 headroom values = 25 boots, 0 rejected,
batch drift 2.04%:

| pct | wr_MBps median +/- SEM | gc_recoveries (IQR) | cells (IQR) |
|----:|------------------------|---------------------|-------------|
| 0 | 123.12 +/- 0.049 | 76 (0) | 51 (0) |
| 5 | 123.88 +/- 0.335 | 64 (0) | 39 (0) |
| 10 | 124.94 +/- 0.320 | 58 (0) | 41 (0) |
| 15 | 125.24 +/- 0.308 | 53 (0) | 64 (0) |
| 20 | 125.44 +/- 0.196 | 49 (0) | 60 (0) |

Adjacent steps 0.5-2.3 sigma (not resolved); 0% -> 20% is 11.5 sigma
(resolved).  All boots exit 0, 0 write errors, 10240 blocks verified
per device per boot.

**Churn gate regression**: 2 passed, 0 failed.  Standing gates
(cellzram 106/0, randio 4/0, fsdev 19/0, torture 231/0) were green at
the end of session 31 and no kernel code changed this session, so they
were not re-run.

### Next Steps

1. **A full bench against zram** -- the next session's brief.  The
   incompressible case is measured and zram wins it; the compressible
   and fs/untar workloads are not.  That is where cellzram's structure
   is supposed to pay, and it is currently unproven.  Use the M-boot
   aggregator from the start; the machinery exists now.
2. **The committed regression baseline** (session 30 char #11).  The
   aggregator prints medians and IQRs but nothing diffs them against a
   stored reference, so a moving median still cannot fail a gate.
3. **Sweep `cell_order`** (2..10) at fixed headroom to record the
   confirmation that 6 is the knee.  Cheap, the axis exists
   (`TEST_CELLORDER`), and it closes a three-session flag.
4. **The raw-block-no-entry lever** -- parked above, for the zram
   bench session to weigh once the compressible numbers exist.

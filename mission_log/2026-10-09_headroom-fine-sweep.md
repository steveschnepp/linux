---
name: headroom-fine-sweep
description: sweeping the overprovision 0..30 by 5 -- why 15% is the point to avoid, and why MB/s cannot resolve this axis (session 28)
project: linux
previous: 2026-10-09_per-mount-comp-stats.md
---

# Mission Log: The Fine Headroom Sweep, and the 15% Trap

Part 12 of the tmpfs transparent-compression series.  Session 28
(2026-10-09).

Session 26 moved `cell_headroom_pct` 30 -> 10 on a coarse sweep
(0/10/30/50/100) and called 10% the knee.  This session re-sweeps
0..30 by 5 at the user's request, with the prior hypothesis that the
sweet spot is nearer 15%.

It is not.  15% turns out to be the single worst value in the range.

Kernel commits: `121b2e95e062` (comment-only; the parameter stays at
10).  Rig: `scripts/sweep_headroom.sh`, unchanged -- it already took a
value list.

## Session 28: the overprovision at 5-point resolution

### What We Did

Ran `sweep_headroom.sh "0 5 10 10 15 20 25 30"` -- no, exactly:
`"0 5 10 15 20 25 30"` -- twice, and compared.  Seven boots per run,
each running the churn gate (64 passes x 2048 incompressible blocks on
a 16 MiB device).

| pct | cells | exhaustions | MiB stored | MB/s (run 1 / run 2) |
|----:|------:|------------:|-----------:|-----------------------------|
| 0   | 51    | 76          | 9.75       | 116.6 / 114.9 |
| 5   | 39    | 64          | 9.75       | 118.5 / 120.6 |
| 10  | 41    | 58          | 10.25      | 119.4 / 118.3 |
| 15  | **64** | 53         | **16.00**  | 119.8 / 120.7 |
| 20  | 60    | 49          | 15.00      | 119.5 / 120.9 |
| 25  | 62    | 46          | 15.50      | 121.3 / 120.8 |
| 30  | 80    | 43          | 20.00      | 121.1 / 120.9 |

`cells` is cell storage at end of churn in 256 KiB units
(`phys` / cell_size); `exhaustions` is `gc_recoveries`, the number of
writes the compaction pass had to save.

**Both runs agreed exactly on cells and exhaustions.**  Throughput
moved +-2 MB/s between them.

### The findings

1. **15% is dominated by 20%.**  20% has fewer exhaustions (49 vs 53)
   *and* less storage (60 vs 64 cells).  No operating point should be
   worse than one to its right on both axes, and this one is.  The
   10->15 step is the worst trade in the whole range: +23 cells to
   avoid 5 exhaustions, against 5->10's +2 cells to avoid 6.

   Marginal exhaustions avoided per extra cell, per +5% step:

   | step | d exhaustions | d cells | avoided per extra cell |
   |------|--------------:|--------:|----------------------:|
   | 0->5 | -12 | -12 | free (both improve) |
   | 5->10 | -6 | +2 | **3.00** |
   | 10->15 | -5 | +23 | 0.22 |
   | 15->20 | -4 | -4 | free (both improve) |
   | 20->25 | -3 | +2 | 1.50 |
   | 25->30 | -3 | +18 | 0.17 |

   The knee is at 10%, as session 26 concluded -- now with the
   resolution to see that everything past it pays badly.

2. **Cell storage is non-monotonic in headroom.**  It drops 51 -> 39
   going 0 -> 5, jumps 41 -> 64 going 10 -> 15, and drops again
   64 -> 60 going 15 -> 20.  Reproduced exactly across runs and across
   sessions (0/10/30 match session 26's numbers byte for byte), so this
   is a real property of the allocator, not run noise.

   Mechanism NOT established.  The compaction pass only triggers when
   the store path runs out of slots, so end-of-churn occupancy depends
   on the fragment pattern at the instant of exhaustion -- which moves
   with the directory size.  That is a plausible story, not a measured
   one, and the code comment says exactly that.

3. **MB/s cannot resolve this axis, and the reason is worth stating.**
   Cross-session drift on identical configuration was ~3% (10%
   measured 122.8/123.4 in session 26, 119.4/118.3 here), which is
   *larger* than the difference between any two headroom values in the
   5..30 range (118.5 to 121.3).  The two integer columns reproduced
   exactly; the float column did not.  The decision had to rest on the
   integers.

### What We Learned

#### Technical

- **A "sweet spot" hypothesis should be tested against dominance, not
  just against its neighbours.**  15% looked plausible next to 10 and
  20 individually.  Checked against 20 on *both* axes at once it is
  strictly worse, which no amount of looking at one neighbour at a time
  would have shown.  For any two-objective knob, plot both axes and
  look for the dominated points before declaring a winner.

- **Non-monotonic cost in a sizing parameter is a signal, not noise,
  once it reproduces.**  The 41 -> 64 cell jump at 15% is far outside
  the +-0 cell run-to-run spread.  Reporting it as a smooth curve with
  an interpolated knee would have been wrong in a way that hid the
  actual hazard.

- **Reproducing an earlier session's numbers exactly is itself
  evidence.**  0/10/30 came back byte-identical to session 26, which
  is what licenses treating the non-monotonicity as real rather than as
  a bad boot.

#### Process

- **Sweep the resolution you intend to decide at.**  Session 26's
  0/10/30/50/100 could not have found a 15% trap; it would have
  interpolated straight over it.  The coarse sweep was right for
  choosing a decade, and wrong for choosing a value inside it.

- **Name which column carries the decision before reading the table.**
  Three columns were printed; only two were usable.  Saying so up
  front is what stopped the MB/s drift from being narrated as a
  5%-step trend.

- **Say "not established" out loud.**  The non-monotonicity has a
  plausible mechanism and no measurement behind it.  Writing the
  plausible story *and* marking it unproven is the difference between a
  useful note and a future bug hunt through a wrong explanation.

### What We Decided

- **`cell_headroom_pct` stays at 10.**  The fine sweep confirms
  session 26's knee with the resolution to defend it: 10% is the last
  point before the marginal cost collapses by an order of magnitude.

- **15% is documented as the value to avoid.**  Not "suboptimal" --
  dominated.  A future reader who intuits "a bit more headroom is
  safer" lands exactly there, so the comment names it.

- **No code change.**  This session edited one comment.  The
  measurement existed to test a hypothesis, and the hypothesis was
  wrong; the deliverable is the evidence, not a diff.

### Rules Added

- **Test a sweet-spot hypothesis against dominance, not neighbours.**
  If the knob trades two objectives, a value can beat both its
  neighbours on one axis and still be strictly worse than a point
  further away.  Check the whole front, not the adjacent pairs.

- **Decide on the columns that reproduce.**  When one column drifts
  across sessions by more than the effect you are measuring, it is not
  evidence -- regardless of how intuitive it looks.

- **A coarse sweep picks a decade; a fine sweep picks a value.**  Do
  not resolve a parameter to 5% on a sweep whose steps are 20%.

### What We'd Do Differently

- Would have run this fine sweep *before* changing the default in
  session 26.  10% happened to be right, and it was chosen from a
  table too coarse to have caught it if it had been wrong.

- Would have printed a `cells` column in session 26.  It derived
  `phys` / cell_size by hand this session; the sweep script should emit
  it so the storage axis is visible without arithmetic.

- Would have swept `cell_order` alongside, as session 26 also flagged.
  Both size the same directory and the interaction is still unmeasured;
  the non-monotonicity here may well be a cell-order artifact.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | `cell_headroom_pct` rationale: fine sweep table, marginal analysis, the 15% trap, and the unestablished mechanism flagged as such |

### Test Results

Sweep run twice, seven boots each.  Cells and exhaustions identical
between runs at every value; throughput within 2 MB/s.  No gate change
-- this is the same churn gate at seven parameter values, all exit 0
with 0 write errors and 2048 blocks verified.

The standing gates were green at 10% at the end of session 27 and the
kernel behaviour is unchanged by a comment, so they were not re-run.

### Next Steps

1. **Sweep `cell_order` with headroom.**  Both size the same
   directory; the non-monotonic storage may be a cell-order artifact,
   and right-sizing one against the other's wrong value is a real
   risk.  Session 26 and now 28 have both flagged it.
2. **Emit a `cells` column from `sweep_headroom.sh`** so the storage
   axis is directly visible.
3. If the 15% trap has a mechanism, it is probably worth finding: a
   sizing knob whose cost is non-monotonic will surprise whoever tunes
   it by interpolation.

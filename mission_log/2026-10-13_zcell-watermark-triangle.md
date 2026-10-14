---
name: zcell-watermark-triangle
description: 21-combo compress_watermark LOW:HIGH triangle (step 20, HIGH >= LOW) at 2G tmpfs == 2G RAM on arm 3 -- only eager 0:0 survives; the flip is 1s-granular with a 2MiB/pass drain against sub-second RAM exhaustion
project: linux
previous: 2026-10-13_workload-testbed-spec.md
---

# Mission Log: The Zcell Watermark Triangle

Part 37 of the tmpfs transparent-compression series.  Session 52
(2026-10-13, same day as the test-bed spec log).  The user asked for a
quick sweep of the zcell watermark: `compress_watermark=LOW:HIGH` has
only ever been exercised as `0:0` (eager) on the cell backend.  The
user specified the shape: a triangle, LOW:HIGH in steps of 20 with
HIGH >= LOW (21 combos), at the 2G tmpfs == 2G RAM exact-fit regime,
arm 3.

## What We Did

### 1. Plumbing (`test.fillwm=`), with the resolved-line rule applied up front

`test_fill` gained a 6th argument (watermark) wired through
`guest/init.sh` (`test.fillwm=`) and `scripts/run_qemu.sh`
(`TEST_FILLWM`); both compressed arms use it; default `0:0`.  Applying
session 51's silent-fallback rule from the start, `arm_setup()` now
prints `FILLMOUNT opts=` (the string passed) and, after a successful
mount, `FILLMOUNT resolved=` (the guest's real `/proc/mounts` line).
Committed in the rig (4 files).

### 2. Sanity boot before the 21 boots

512M / `0:40`: the resolved line shows `compress_watermark=0:40` --
the param provably reaches the guest.  (This run later turned out to
contain the smoking gun -- see below.)

### 3. The sweep (`scripts/sweep_fillwm.sh`)

21 one-boot combos (0:0 .. 100:100, triangle), `QEMU_MEMORY=2G`,
`TEST_FILLMB=2048`, readback on, nokasan.  Per-combo logs plus a
summary table keyed on the `ZB*` lines.

### 4. Result: eager wins by knockout

| region | outcome |
|---|---|
| 0:0 (eager) | survives: wrote 2048 MB @ 726.5 MB/s, read @ 1927.5 MB/s, CRC ok, `comp_pages=524288` (every page, ~2:1), ~800 MB free |
| all 20 non-eager combos | OOM: `test_fill` (oom_score_adj=1000 by design) killed mid-fill; 7 boots printed a flip, every one at **usage 92%** |

### 5. Mechanism (traced in `mm/shmem_compress.c`)

- `comp->hot = (wm_high == 0)`: any positive HIGH starts *chill* --
  writes land uncompressed.
- The only mode flip lives in `shmem_comp_age_work()`, armed with
  `SHMEM_COMP_AGE_DELAY_MS = 1000` -- no flip for the first second.
- Drain budget: `SHMEM_COMP_AGE_BUDGET=128` pages x
  `SHMEM_COMP_AGE_MAX_ROUNDS=4` = 512 pages = **2 MiB per pass, once
  per second**.
- A chill fill runs at ~2.5 GB/s (measured 2550 MB/s at 512M), so 2 GB
  is exhausted well under 1 s -- OOM fires before the first pass can
  help.
- Smoking gun: the 512M/`0:40` sanity run (fits uncompressed, so it
  survived) ended with `comp_pages=0` -- the 0.2 s fill completed
  before any pass ran and compressed nothing at all.
- The seven late flips all at 92% usage: the OOM-throttled writer
  finally letting a scheduled pass run, at the deathbed.

## What We Learned

### Technical

- **The watermark band has no engagement window at tmpfs==RAM against
  a fast sequential writer.**  Flip granularity is 1 s; RAM exhaustion
  is sub-second; the drain (2 MiB/s) is 4-5 orders of magnitude short
  of a ~1.8 GB backlog.  Eager inline compression (~730 MB/s) is the
  only mode that survives this regime.
- **The policy is not broken -- its envelope excludes this regime.**
  The torture-based policy matrix at the 384M cliff showed bands
  working because that workload lives for minutes, giving the 1 s flip
  real time.  This sweep defines one edge of the envelope; the
  minutes-long churn steady states of the workload bed (spec 05) are
  where bands can still earn their keep.
- **A harness exit code is not a test result.**  The OOM kill takes
  `test_fill` (it volunteers via oom_score_adj) and init continues, so
  `run_qemu.sh` can exit 0 over a dead fill.  The `ZBSUMMARY` line --
  or its absence -- is the truth.

### Process

- Sanity-boot the plumbing (one non-default combo, check the resolved
  mount line) before spending 21 boots on it.  Session 51's lesson,
  applied proactively; cost ~40 s, removed the re-sweep risk.
- The user's triangle shape (HIGH >= LOW only) is the right shape for
  a hysteresis sweep: LOW > HIGH combos are invalid by the parser and
  simply do not exist.

## What We Decided

- **Record the envelope finding; no kernel change this session.**
  Deferral cannot be made safe for the exact-fit fast fill by tuning
  the numbers the triangle already covers; a fix would need a
  qualitatively faster flip (e.g. write-path flip on threshold
  crossing) and is not wanted yet -- the workload bed will tell us
  whether bands matter at all for realistic churn.
- The band-vs-churn question moves to spec 05: W1/W3/W4 run for
  minutes at steady occupancy, the regime where the flip can engage.

## Rules Added

- **Always write the mission log after each meaningful action; never
  ask.**  The user's standing rule, set this session; made mandatory in
  the mission-log skill itself.
- **Key bench results on the test's own summary line, not the
  harness exit code** -- a harness that survives its test's death
  reports the harness's fate otherwise.

## What We'd Do Differently

- Nothing structural.  If anything: inspect the first sanity run's
  gauges (`comp_pages`) immediately -- the 512M run already contained
  the full mechanism (comp_pages=0 under deferral) before the 21-boot
  sweep was launched.

## Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_fill.c` | 6th arg watermark; `FILLMOUNT opts=/resolved=` lines; ZBSETUP prints cell/wm |
| rig `guest/init.sh` | parse `test.fillwm=`, pass as 6th arg |
| rig `scripts/run_qemu.sh` | `TEST_FILLWM` -> `test.fillwm=` |
| rig `scripts/sweep_fillwm.sh` (new) | the 21-combo triangle driver + summary table |
| `results/sweep_fillwm_tri_2048M_20261013_145140/` | 21 logs + summary |

## Test Results / Benchmarks

zcell watermark triangle (arm 3, 2048M tmpfs == 2G RAM, fill+readback,
nokasan, one boot per combo):

- 0:0: wrote 2048 MB @ 726.5 MB/s; read-back @ 1927.5 MB/s; CRC ok;
  comp_pages=524288 (all, ~2:1); ~800 MB free after the fill.
- 0:20 .. 100:100 (20 combos): OOM, `test_fill` killed mid-fill; 7 flips
  observed, all at usage 92%; no combo produced a ZBSUMMARY.

## Next Steps

1. Spec 05 Phase 1: workload engine core + W4 in verify mode (KASAN).
2. Optional completing probe: 0:20 at `QEMU_MEMORY=4G` -- no OOM, flip
   engages, measures the drain rate honestly ("works, just not in
   time" proven rather than inferred).
3. Band-vs-churn measurement lands naturally in the workload bed
   (minutes-long steady states).
4. Standing items unchanged: RFC assembly (spec 02 Phase 6), M-boot
   median+IQR wrapper, arm-3 rm/synchronous-free at 64M.

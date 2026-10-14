---
name: workload-testbed-spec
description: define the workload test bed from established prior art (fsx/fstorture already in the rig, filebench personalities, postmark, fio conventions, SNIA steady state, YCSB zipfian) as two suites with different contracts -- correctness pass/fail vs performance numbers
project: linux
previous: 2026-10-13_cell-geometry-sweep.md
---

# Mission Log: The Workload Test Bed Spec

Part 36 of the tmpfs transparent-compression series.  Session 52
(2026-10-13).  A design session -- no kernel or rig code changed; the
deliverable is `specs/05_WORKLOAD_TESTBED.md`.  Prompted by the
session-51 open question: the fill workload sees only half the
tradeoff, so design tests for varying load -- a mix of writes, reads,
deletes, appends, eventually modify in place.

## What We Did

The spec was shaped by five user corrections, each moving it from
invented to grounded:

1. **First draft:** invented profiles P1-P6, to be grounded by tracing
   real tools (dpkg, make, logrotate) before writing numbers.
2. **Rename-over correction:** modify-in-place is surprisingly rare in
   UNIX; the dominant mutation pattern is write-new + rename-over.  The
   op taxonomy was reworked: rename-over is a first-class op (its
   distinct stressors: transient 2x logical space, file-identity churn,
   implied deletes); in-place pwrite is a minority-but-real pattern
   (databases, mmap) and the only path driving
   decompress-modify-recompress.  Verified in the code:
   `shmem_decompress_folio()` (swap-in-like, group decompress), then
   dirty, then recompress with alloc-new-before-free-old.
3. **Usage-first inversion:** define the test bed independent of how
   the kernel operates -- the kernel adapts to usage, not the reverse.
   Independence rules drafted: profiles come from cited workload
   definitions; metrics are user-visible only; a parameter justified by
   "the kernel does X" is wrong by construction.
4. **Two-suite split:** correctness tests and performance tests are
   different species with different contracts -- PASS/FAIL on
   invariants, KASAN, every change, small scale, vs numbers with no
   thresholds, nokasan, on demand, regime scale.  The hinge: ONE
   seeded, replayable op engine, run as `verify=1` (shadow-CRC
   invariants) in Suite A and `verify=0` (timers + meminfo) in Suite B,
   so a perf anomaly at `(profile, seed)` replays as a correctness run.
5. **Prior-art audit** ("don't reinvent the wheel"): the rig already
   runs Apple's fstools -- `fsx` (single-file random op stream with
   data verification) and `fstorture` (directory-tree reference model)
   -- via `test.fstools=1` on eager-compressed mounts.  The correctness
   wheel exists; its gaps are mmap (`-m`), punch (`-d`), seed replay
   (`-S`), and scale (`-N 2000`).  For performance: filebench
   personalities (fileserver/webserver/varmail) supply the mixed
   profiles, postmark the steady-state churn transaction model, fio the
   measurement conventions, SNIA PTS the steady-state criterion for the
   retention curve, YCSB the zipfian hot set.  The only genuinely new
   metric is ours: RAM-per-logical-byte for a compressing tmpfs under
   churn.

Wrote `specs/05_WORKLOAD_TESTBED.md`: the two-suite contract table,
the engine interface (`struct wl_profile`, `enum wl_op`, seeded
xorshift64*), six cited profiles (W1 fileserver with rename-over added
as a flowop, W2/W2z webserver, W3 varmail, W4 postmark-churn, W5
build-scratch, W6 in-place random), exact metric definitions, the
cmdline surface (`test.workload=`, `test.seed=`, `test.wl*`), and a
4-phase migration.

## What We Learned

### Technical

- The op-stream-with-verification wheel (fsx) and the
  reference-model-over-a-tree wheel (fstorture) are already integrated
  in the rig.  The missing correctness coverage is specific flags and
  scale, not the absence of a generator.
- An in-place write to a compressed page is the only op with
  asymmetric cost (group decompress + recompress + store + free-old);
  rename-over never touches that path.  Both are correctness-covered
  today by fsx's rewrite ops at small scale.
- Published workload personalities assume block devices: their
  absolute numbers are not comparable to ours.  What we borrow is
  their *structure* (op probabilities, size distributions,
  transaction model); all comparisons stay arm-vs-arm and
  kernel-vs-kernel, stated in every results header.

### Process

- Four corrections in a row is a signal: the first draft was invented
  rather than audited.  Auditing existing rig coverage (one `ls` of
  `tests/` + `test.fstools=1` in init.sh) should have preceded the
  shadow-model proposal -- fsx already was one.
- "Two suites, two contracts" resolved a recurring ambiguity: a
  retention number in a perf run is data; the same number failing to
  return to baseline in a correctness run is a bug.  Same measurement,
  different consequence.

## What We Decided

- `specs/05_WORKLOAD_TESTBED.md` is the contract: two suites over one
  seeded engine; six borrowed profiles; user-visible metrics only
  (rho = RAM/logical byte, retention ratio, SNIA-style steady state,
  per-op-class throughput and p50/p99); no upstream binaries (no
  package installs; a thin C reimplementation of the flowop subset,
  cited); no kernel changes in this spec.
- The session-51 open question (churn half of the geometry tradeoff)
  is answered inside spec 05's Phase 4: W1/W4 x cell geometry via the
  existing `test.fillcell=`.

## Rules Added

- **Do not invent workload shapes.**  Every op mix, size distribution,
  and hot-set rule must name its published source; only metrics may be
  new.
- **The test bed is usage-defined.**  A parameter justified by kernel
  behavior is wrong by construction; the kernel adapts to the bed.
- **Always write the mission log after each meaningful action; never
  ask.**  The user's standing rule, set this session; made mandatory in
  the mission-log skill itself so every future session sees it.

## What We'd Do Differently

- Audit the rig's existing test inventory before designing any new
  test layer -- the fstools integration made half of the first draft
  redundant.

## Files Changed

| File | Purpose |
|------|---------|
| `specs/05_WORKLOAD_TESTBED.md` (kernel repo, new) | This spec |

## Test Results / Benchmarks

None -- spec-only session.  Measurements are deferred to spec 05's
migration phases and, for the watermark axis, to the sweep in the next
log of this day.

## Next Steps

1. Spec 05 Phase 1: engine core + W4 (postmark-churn) in verify mode
   under KASAN.
2. The zcell watermark triangle sweep (interleaved this session; see
   the next log).
3. Standing items unchanged: RFC assembly (spec 02 Phase 6), M-boot
   median+IQR wrapper, the arm-3 rm/synchronous-free story at 64M.

---
name: spec06-phase-c-triangle
description: spec06 Phase C -- the 21-combo watermark triangle rerun flips from 1/21 survivors (pre-spec06) to 15/21; the 6 OOMs are exactly the HIGH=100 rows where the write-path flip lands at/after the OOM point
project: linux
previous: 2026-10-14_entry-race-static-analysis.md
---

# Mission Log: spec06 Phase C -- Watermark Triangle Rerun

Part 41 of the tmpfs transparent-compression series.  Session 53
(2026-10-14).  The Phase C gate for spec 06 (specs/06_WATERMARK_RAM_
TWO_PHASE.md): rerun the 21-combo `compress_watermark` LOW:HIGH triangle
at 2G tmpfs == 2G RAM on the spec06 kernel, and compare against the
pre-spec06 baseline that motivated the redesign.

## Prerequisites this session cleared first

1. **Experiment code removed.**  Reverted the spin-vs-wait experiment
   (`git revert aa80349`, self-contained in mm/shmem.c) and dropped the
   rig's `TEST_EXP*` plumbing.  Verified: reader lock sites back to plain
   `folio_lock`, no `exp_*`/`ZBLK` symbols left, bash syntax valid.  The
   entry-race fix (unconditional `wake_up_folio_waiters` walk +
   `shmem_undo_range` truncate drains) is untouched -- 3 drain calls in
   shmem.c, unconditional walk in filemap.c.
2. **Both kernels rebuilt clean.**  `make O=out/{kasan,nokasan} bzImage`,
   no experiment symbols in either `shmem.o`.
3. **Full KASAN suite: 3/3 clean.**  Default boot, QEMU_CPUS=4.  Each run:
   exit 0, entry-race all 4 phases `PASS quiescent`, zero real KASAN
   splats (`BUG: KASAN|use-after-free|GPF|WARNING: CPU` all 0), zero
   hangs.  (The two grep "hits" on the FAIL/KASAN pattern were the test's
   own banner text -- "Race test failures: 0 (KASAN log is
   authoritative...) -- false positives, checked in context.)

## What We Did

Restored spec06 from `stash@{0}` (popped clean; no conflict -- spec06
touches `shmem_write_end`/opts parsing, the entry-race fix touches
`shmem_undo_range`, different regions).  Committed as `82202978e96e`.
Built nokasan (the triangle is a throughput/regime test -- no KASAN tax),
synced, and sanity-booted first (the session-51 silent-fallback rule):
arm 3, 512M, wm=0:40 -> flip fired on RAM usage, resolved mount line
carried `compress_watermark=0:40,gc_percent=50` into the guest, CRC ok,
fails=0.

Then the full triangle (`scripts/sweep_fillwm.sh 2048`), 21 one-boot
combos, arm 3, 2G tmpfs == 2G RAM, fill + read-back.

## The Result: 1/21 -> 15/21

| kernel | survivors (crc ok, fails=0) | OOM |
|---|---|---|
| pre-spec06 (10-13 baseline, `..._20261013_162702`) | **1/21** (only eager 0:0) | 20 |
| spec06 (now, `..._20261014_135405`) | **15/21** | 6 |

**Clean partition, no corruption:** the 6 that OOM are exactly the 6
`HIGH=100` combos (0:100, 20:100, 40:100, 60:100, 80:100, 100:100).
Every survivor: crc=ok, fails=0, wrote the full 2048 MB.  Zero crc
mismatches, zero non-zero fails across all survivors.

### Mechanism (confirmed in the logs, not inferred)

- **Survivors** flip hot on RAM usage *mid-fill* and compress inline
  before OOM: `tmpfs: mount tmpfs compression hot (ram usage 40%)`, then
  a full 2048 MB write at 965-1662 MB/s.  This is spec06's write-path
  flip doing exactly what the design promised -- evaluated where the
  decision takes effect, on the honest signal (RAM, not mount usage).
- **The 6 HIGH=100 OOMs** are the expected policy envelope, not a bug.
  HIGH=100 means "flip only at 100% usage", which lands at/after the OOM
  point on a ~2.5 GB/s sequential fill into 2G -- the flip fires
  ("compression hot (ram usage 100%)") but too late to save the fill.
  `100:100` is the `nocompress` sentinel by construction (resolved line:
  `nocompress`), so it OOMs by design.

## What We Learned

- spec06's write-path flip moved the survivability boundary from "only
  eager survives" to "every band with HIGH < 100 survives" at the
  exact-fit fast-fill regime.  The original 10-13 finding ("the policy is
  not broken; its envelope excludes this regime") was the right diagnosis
  -- the fix was a qualitatively faster flip (write-path, RAM signal),
  exactly as that log's "would need" note predicted.
- HIGH=100 is the honest ceiling: a flip threshold at 100% usage cannot
  engage before OOM on a sub-second exhaustion.  This is the envelope
  edge, documented, not a regression.

## What We Decided

- **spec06 Phase C PASSES.**  15/21 crc-clean survivors vs the 1/21
  baseline; the 6 failures are the HIGH=100 envelope (flip lands too
  late by construction), zero corruption.  spec06 stands as specified.

## What We'd Do Differently

- Would have run the full KASAN suite on the FIXED kernel BEFORE
  restoring spec06, not after -- the ordering here (revert experiment ->
  suite -> spec06 -> triangle) worked, but it meant the suite ran on a
  kernel without spec06 while the triangle ran on a kernel with it, so a
  spec06-introduced KASAN issue would only surface in a later suite run.
  Next time: one kernel config, suite then gate, no interleaved restore.
- The sanity boot (512M, 0:40) before the 21-boot triangle was right and
  cheap (~40s) -- would do it identically again.  It caught nothing this
  time but is the standing lesson from session 51 made proactive.
- Nothing about the fix or the gate itself needs changing; the triangle
  shape (HIGH >= LOW, step 20) is the correct hysteresis sweep and the
  HIGH=100 envelope is now understood, so those 6 OOMs are expected data
  rather than surprises.

## Test Results

- Full KASAN suite (pre-triangle): 3/3 runs, entry-race 4/4 phases PASS
  each, 0 real splats, 0 hangs.
- spec06 sanity boot (512M, 0:40): flip on RAM usage 40%, gc_percent=50
  in resolved line, crc ok, fails=0.
- Triangle 2048M arm 3: 15/21 survive crc-ok/fails-0; 6 HIGH=100 OOM
  (expected envelope).  Write 965-1662 MB/s, read 2260-3895 MB/s on
  survivors.

## Files Changed

| File | Purpose |
|------|---------|
| kernel `mm/shmem.c` (+4) | experiment reverted; entry-race truncate drains intact; spec06 restored |
| kernel `mm/filemap.c` | entry-race fix: unconditional `wake_up_folio_waiters` walk |
| kernel `mm/shmem_compress.c`, `mm/cell_store.c`, `include/linux/{cell_store,shmem_compress}.h` | spec06 implementation (restored from stash, committed) |
| rig `scripts/run_qemu.sh` | `TEST_EXP*` plumbing removed |

## Next Steps

1. spec06 is now implemented + Phase C gated.  Remaining spec06 items:
   Phase D migration is in (gc_percent); the 384M cliff torture matrix is
   the regression gate for the kept 1 Hz pass -- still pending.
2. The entry-race fix + spec06 are both committed on
   `feature/tmpfs-compression`.  Consider an RFC branch state (spec 02
   Phase 6) now that the two long-running threads have landed.
3. Standing items unchanged: M-boot median+IQR, arm-3 rm/synchronous-free
   at 64M, workload-bed spec 05 Phase 1.

---
name: umount-double-free-verdict
description: session 35's first next-step was "fix the known tmpfs merge bug" -- the bug was already fixed on 10-06; verified, gated with a remount-cycle test, and the stale record corrected
project: linux
previous: 2026-10-10_cellzram-removal.md
---

# Mission Log: The Merge Bug That Was Already Fixed

Part 21 of the tmpfs transparent-compression series.  Session 37
(2026-10-10).  The brief: session 35's next-step #1, "fix the known
tmpfs merge bug before anything else" -- a slab corruption (double-
release in the decompress/eviction interplay) recorded in the
`sweep_merge.sh` header, said to detonate on the mount after a
compress/umount cycle, and declared blocking for upstream.

The bug does not exist anymore.  It was fixed on 2026-10-06.

## What We Did

1. **Read the bug record where it lives.**  `scripts/sweep_merge.sh`'s
   header and session 35's next-steps both describe the bug.  The
   header dates to the rig's initial import (`213fb8d`) -- before the
   fix.

2. **Read the foundation log's bug hunt** (session 2, 2026-10-06).
   The hunt ended with a KASAN verdict: **`BUG: KASAN: double-free in
   shmem_free_fc`** -- `shmem_free_fc()` freed `ctx->comp_compressor`
   twice (a duplicated `#ifdef/kfree()` pair from the session-1
   hardening round).  Every compressed umount double-freed the
   kmalloc-8 compressor string; the poisoned freelist detonated at the
   next mount's option-string allocation.  That is exactly the
   recorded symptom ("detonates on the mount AFTER a compress/umount
   cycle") -- and the entry lifecycle the header blamed was proven
   innocent in the same session.  Fixed in `5f0aca809a9d`.

3. **Verified the fix is in the current tree, not just the log.**
   `shmem_comp_opts_free()` is one `kfree` + NULL; `shmem_free_fc()`
   calls it once; the session-2 companion fix `XA_LIMIT(1, UINT_MAX)`
   (ID-0 phantom entry) is in place in `shmem_compress.c`.

4. **Discovered the rig kernel has been KASAN-instrumented the whole
   time.**  The rig's persistent config carries `CONFIG_KASAN=y`
   (session 2 left it on).  Every "green suite" for 30 sessions --
   including today's -- was KASAN-grade evidence.  A planned KASAN
   side-build turned into a no-op discovery: `autoconf.h` already had
   it.

5. **Closed the blind spot that hid the bug for a session.**  No test
   did many back-to-back data-bearing mount cycles; `test_mount_sem`
   mounts repeatedly but with empty mounts, and the merge sweep is
   one-cycle-per-boot by design.  Wrote `test_remount_cycle.c`: N
   (default 24, isolated `TEST_REMOUNT=N`) cycles of
   mount(compress) -> write 96 KiB -> verify -> truncate -> umount in
   one boot, rotating lz4/zstd and merge 1/8 and interleaving plain
   mounts so any double-release detonates in the gate, not silently.

6. **Verified and committed.**  64 isolated cycles and 24 in-suite on
   the KASAN kernel: zero KASAN reports, zero oopses.  Full tmpfs
   suite green (exit 0, 0 FAILs) with the new gate included.  Rig
   commit `f9799d1` adds the gate and corrects the stale
   bug-blaming comments in `sweep_merge.sh` and `init.sh`.

## What We Learned

### Technical

- **A stale bug record outlives its bug.**  The fix landed 2026-10-06;
  the header comment blaming a "known slab-corruption bug" was never
  updated, and session 35's next-steps inherited it as the first
  blocker for upstream.  Four sessions later, nobody had re-verified
  that the bug still existed.  Bug notes without provenance decay
  into lore.

- **The oops site is not the bug site** (session 2's lesson, now
  load-bearing twice): the crash was in slab allocation during mount
  parsing; the cause was a duplicated kfree in fs_context teardown,
  two subsystems from the suspected decompress/eviction interplay.
  The suspected code was innocent both times it was suspected.

- **The rig kernel has silently been a debug kernel since session 2.**
  `CONFIG_KASAN=y` in the persistent config means every gate run for
  30 sessions doubled as a KASAN pass -- free rigor that was never
  claimed or recorded.  Worth knowing both directions: the gates are
  stronger than advertised, and a future "turn on KASAN" task is
  already done.

- **The detonation pattern (poisoned cache consumed by the NEXT
  operation) needs a test that performs the consuming operation.**
  Single-cycle boots were designed to be contamination-free -- which
  is also bug-blind.  The suite's many mounts looked like coverage;
  they were empty mounts, not data-bearing compress/umount/remount
  cycles.

### Process

- **Re-verify scheduled bug-fix work before doing it.**  The whole
  session could have been one grep + one build + one test run -- and
  after the archaeology, it was.  The next-steps list is a place
  bugs go to become lore; entries need provenance.

- **The fix for a "known bug" is not complete without a regression
  gate that would have caught the original bug.**  The old suite
  passed over the double-free for a session; the new gate detonates
  on it by construction.

## What We Decided

- **The bug is closed as already-fixed, with evidence**: fix commit
  located and verified in tree, 64+24 remount cycles clean under
  KASAN, full suite green, zero reports.

- **`test_remount_cycle` is a permanent gate** -- in the default
  suite and as an isolated boot mode (`TEST_REMOUNT=N`).

- **No kernel change was needed this session.**  The tree carried the
  fix since `5f0aca809a9d`; the work was verification, a gate, and
  correcting the record.

## Rules Added

- **A "known bug" note must carry provenance** (fix commit, or
  reproduction recipe + date seen).  Before scheduling bug-fix work
  from a comment or a next-steps list, re-verify the bug still
  exists.

- **Any bug that hid behind a test-design blind spot gets a
  dedicated regression gate**, even after the fix -- the suite that
  passed over it is not the suite that guards it.

## What We'd Do Differently

- Session 35's next-steps would have re-verified the sweep header's
  claim before declaring it upstream-blocking.  The header was
  written before the fix and said so in its own git history -- one
  `git log --follow` away.

- Session 2 would have updated `sweep_merge.sh`'s header in the fix
  commit.  Comment corrections that ride along with the fix are
  free; they become archaeology later.

## Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_remount_cycle.c` | **new** -- the umount double-free regression gate (N compress/umount cycles per boot) |
| rig `guest/init.sh` | gate wired into the default suite + `test.remount=` boot mode; stale bug comment corrected |
| rig `scripts/run_qemu.sh` | `TEST_REMOUNT=` passthrough |
| rig `scripts/sweep_merge.sh` | header corrected: the blamed bug was fixed by `5f0aca809a9d`; one-boot-per-value stays as methodology |

## Test Results / Benchmarks

| run | result |
|---|---|
| remount gate, isolated, 64 cycles (KASAN kernel) | exit 0, 0 KASAN reports, 0 oopses |
| remount gate, in-suite, 24 cycles | pass |
| full tmpfs suite (KASAN kernel, post-gate) | exit 0, 0 FAILs, 0 KASAN reports |

## Next Steps

Back to the pivot list (session 35), now with its first item resolved:

1. ~~Fix the known tmpfs merge bug~~ -- **resolved: fixed
   2026-10-06 (`5f0aca809a9d`), verified today, regression gate
   added.**
2. **Scope the upstreamable slice**: `merge_pages`, the compression
   pool (zsmalloc backend), mount options, Kconfig.
3. **The measurement story**: compressible-non-zero dataset against
   uncompressed tmpfs, M-boot rigor.  The largest unmeasured question
   in the series.
4. **Sphinx-verify the rst** (needs docutils; blocked by the
   no-install rule -- user approval required).

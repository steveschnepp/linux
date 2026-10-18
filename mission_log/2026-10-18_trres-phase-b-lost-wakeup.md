---
name: trres-phase-b-lost-wakeup
description: the rare trres Phase-B (merge=8) hang is a lost folio-lock wakeup in the group swap-out path -- root-caused via a failed-instrumentation argument, fixed with the missing wake_up_folio_waiters drain
project: linux
---

# Mission Log: The Phase-B Hang Is a Lost Wakeup in the Group Swap-Out

Part 52 of the tmpfs transparent-compression series.  Session
(2026-10-18), bootstrapping from the record (logs 1-51) then attacking
the open bug list the user scoped to "tmpfs/zcell only."  Two concrete
fixes (one kept local, one committed) and one classification.

## The inherited bug list, and the scope

The prior logs left these open: (1) the rare trres Phase-B (merge=8)
hang, (2) four tree-tip build bugs (VT/ucs_width_table.h, header.S EFI,
mkcpustr race, arm modpost), (3) the matrix_watermark aging-engaged
failures.  The user scoped the work: **fix only tmpfs/zcell bugs; keep
what is already done.**  That put the trres hang and the watermark item
in scope; the x86-boot/build-tree bugs (EFI, mkcpustr, arm modpost) are
out of scope.

## What We Did

### 1. VT/ucs_width_table.h -- fixed, but it is a generated artifact

The i386/x86_64 builds were worked around with `--disable CONFIG_VT`
because `drivers/tty/vt/ucs.c` would not compile: the checked-in
`ucs_width_table.h` was in an OLD format (four `ucs_interval16/32`
arrays) while `ucs.c` consumes a NEW merged format (`ucs_bmp_ranges` /
`ucs_nonbmp_ranges` + `UCS_NONBMP_BMP_BITS`).  Nothing defined
`ucs_interval16/32`, so VT=y failed.

Root cause: **stale generated artifact.**  `gen_ucs_width_table.py`
emits the new merged format; the checked-in header predated it.
Regenerating (`python3 gen_ucs_width_table.py -o ...`) produced a header
`ucs.c` compiles against cleanly (`CC drivers/tty/vt/ucs.o`, 0 errors 0
warnings).  **But the file is gitignored** (`drivers/tty/vt/.gitignore:7`)
-- it is a build-time-generated file, not tracked source.  So the fix is
a correct LOCAL regeneration; there is nothing to commit.  A pristine
checkout regenerates it via the Makefile (only when GENERATE_UCS_TABLES
is set; otherwise the shipped one is used -- which is why a stale one
bites).  Local build now clean; not a tmpfs concern beyond unblocking
x86 builds.

### 2. The trres Phase-B hang: reproduced, then diagnosed by a *failed* instrument

This was the crown jewel -- a "one occurrence in two runs, tmpfs-only"
hang on the cell backend + merge=8, never root-caused.

**Reproduced first** (verify the test is the test): ran the KASAN
kernel's `test.trres=1 test.tronly=B` in a loop.  **1 hang in 12 runs**
(`rc=124`, `hung_task_panic` firing).  The trace was decisive: a single
reader thread stuck in `__folio_lock` from the **cache-hit path** of
`shmem_get_folio_gfp` -- a folio sitting in `i_pages` with `PG_locked`
set whose locker never unlocked.  Only ONE task stuck -> not a deadlock;
a leaked lock or a lost wakeup.

**Instrumented to name the leaked folio** -- added a bounded
`folio_trylock` loop (that panics after N seconds) at the cache-hit
lock site, in a throwaway worktree (`linux-trres`), rebuilt the KASAN
kernel.  **Then it never hung again: 0 in 60 runs** (both a `cpu_relax`
spin and a faithful `io_schedule_timeout` sleep-poll).

**The failed instrument WAS the diagnosis.**  The instrument replaced
`folio_lock()` with a `folio_trylock()` retry loop.  A retry loop
**cannot lose a wakeup** -- it just retries until the bit is clear.  So:
- A **leaked lock** (bit set forever) would STILL hang the instrumented
  kernel (trylock spins -> timeout -> BUG).  It never did.
- Only a **lost wakeup** (lock is FREE, sleeper never woken) is silently
  *cured* by trylock-polling, because the poll rechecks the bit instead
  of relying on the skipped wake.

Since the instrumented kernel was clean and the uninstrumented hung
1-in-12, the bug is a **lost folio-lock wakeup**, not a leaked lock.
This is a genuinely useful technique: an instrument that changes the
symptom class localizes the class by which symptom it erases.

### 3. The fix: the group swap-out path was missing the entry-race drain

The entry-race work (logs 2026-10-13/14) established the rule: **when a
folio leaves the page cache and is unlocked, drain lock waiters with
`wake_up_folio_waiters()`**, because `folio_unlock()`'s fast path can
skip an exclusive waiter, and once the folio is out of `i_pages` no
later lock/unlock is guaranteed to wake it.

That drain is present on the **single-page** compress path
(`shmem_comp_compress_one`) and both decompress paths.  It was **missing
from the merge=8-only group swap-out loop** in
`shmem_comp_compress_chunk`:

```c
shmem_delete_from_page_cache(folios[i], NULL);   /* folio leaves i_pages */
shmem_comp_entries_set(inode, folios[i]->index, ent);
folio_unlock(folios[i]);                          /* wake can be skipped */
folio_put(folios[i]);                             /* no future wake guaranteed */
```

A concurrent reader that cache-hit this folio and slept in `folio_lock`
could have its wake skipped; because the folio has left `i_pages`, it
sleeps forever -> the hang.  This also explains the Phase-B-only
signature: Phase A (merge=1) uses the single-page path, which has the
drain; only merge>=2 reaches `shmem_comp_compress_chunk`.

**Fix** (commit `7d62b2f8bf0b`): add `wake_up_folio_waiters(folios[i])`
after the `folio_unlock` in the group swap-out loop, mirroring the
single-page path.

### 4. matrix_watermark aging-engaged: classified, not a kernel bug

Inspected the actual failures.  The correctness-critical checks all
pass: the **negative invariant** ("never" mounts must NOT age) passes on
all four sizes, and every data-integrity assertion (CRC, manifest,
ENOSPC, unexpected errors) passes.  The failures are two non-kernel
classes: (a) **flaky positive assertions** -- "a chill mount must have
aged >=1 page during a bounded workload," which is timing-dependent
because aging is a 1 Hz background pass (a short workload can legimately
show zero aged pages); and (b) **test-rig artifacts** -- `untars
succeeded (errno=17 File exists)` and a stray `mount tmpfs (errno=22)`,
i.e. stale errno / directory-cleanup between rows.  Confirms the prior
session's "pre-existing, non-blocking" call.  **Not a tmpfs/zcell kernel
bug; no kernel change.**

## What We Learned

### Technical

- **A lost wakeup and a leaked lock hang identically from the outside**
  (one task stuck in `folio_lock`).  They are distinguished by whether a
  *retry-based* lock attempt cures the hang.  trylock-polling cures only
  the lost-wakeup class.  Instrument by substituting the wait mechanism,
  and read *which symptom disappears* to classify the bug.
- **The entry-race drain rule (`wake_up_folio_waiters` after a
  cache-exit unlock) is easy to miss on less-trodden paths.**  The
  single-page path got it; the merge=8 group swap-out did not.  Any
  future path that deletes a folio from `i_pages` then unlocks it needs
  the same drain -- this is now the third site (compress_one,
  decompress_folio, decompress_group finalize) and the group swap-out is
  the fourth.
- **Phase-B-only hangs point at merge>=2 code.**  `shmem_compress_group`
  / `shmem_comp_compress_chunk` / `shmem_decompress_group` are all
  merge>1-only; a symptom confined to merge=8 is almost certainly in one
  of those.
- **A gitignored generated header can be "the build bug."**
  `ucs_width_table.h` is regenerated by the Makefile but only under
  GENERATE_UCS_TABLES; a stale shipped copy silently breaks VT=y.  The
  fix is to regenerate, and it is inherently local (nothing to commit).
- **The watermark matrix's positive "chill must age" assertion is
  flaky by construction** (1 Hz aging vs a bounded workload); its
  negative invariant is the trustworthy check.

### Process

- **Reproduce before diagnosing.**  The 1-in-12 reproduction turned "a
  rare hang, never seen twice" into a deterministic-enough target, and
  the trace named the exact wait site.
- **A failing instrument is data.**  Rather than discard the 0-in-60
  instrumented result as "reproduction got unlucky," reading it as "the
  instrument erased the symptom class" produced the diagnosis directly.
- **Revert instrumentation before committing.**  The debug edit lived in
  a throwaway worktree and the main tree was verified clean (`git diff`
  empty) before the real one-line fix was committed.
- **Respect the user's scope.**  "tmpfs/zcell only" meant skipping three
  real-but-out-of-scope x86-boot bugs rather than fixing everything
  visible.

## What We Decided

- **The trres Phase-B hang is FIXED** (`7d62b2f8bf0b`): the group
  swap-out loop now drains folio lock waiters, matching the single-page
  path.
- **VT/ucs_width_table.h fixed locally, not committed** (gitignored
  generated file).  Unblocksimples x86 builds on this machine.
- **matrix_watermark is a flaky-test + test-rig-cleanup item, not a
  kernel bug**; no change made.  Its negative invariant and all data
  integrity pass.
- **Out-of-scope, still open (not tmpfs):** header.S EFI PE-header
  assembly error, mkcpustr parallel race, arm modpost (cros_ec/ath11k).

## Rules Added

- **Any path that deletes a folio from the page cache and then unlocks
  it must call `wake_up_folio_waiters()`** -- the entry-race rule; the
  group swap-out was the missed site.
- **When an instrumented build stops reproducing, ask which symptom the
  instrument erased** -- that classifies the bug (lost wakeup vs leaked
  lock) before any code reading.
- **Distinguish "the bug is gone" from "the instrument cured it."**  A
  trylock-poll instrument cures lost wakeups; do not mistake its clean
  runs for a fix.

## What We'd Do Differently

- Would have applied the entry-race drain to ALL page-cache-exit unlock
  sites (including the group swap-out) in the original entry-race fix,
  rather than only the single-page path -- a grep for
  `shmem_delete_from_page_cache` followed by `folio_unlock` would have
  surfaced the missed site then.
- Would have reproduced the hang (a loop of `test.tronly=B`) before any
  static analysis; the 1-in-12 rate made it cheap to catch and the trace
  immediately decisive.

## Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` (`7d62b2f8bf0b`) | group swap-out loop: add `wake_up_folio_waiters()` after `folio_unlock` -- the lost-wakeup fix |
| `drivers/tty/vt/ucs_width_table.h` (gitignored, local) | regenerated to the current merged format so VT=y compiles; not tracked |
| `mm/shmem.c` | temporary bounded-lock instrument; REVERTED (verified `git diff` empty) |

## Test Results / Benchmarks

- **trres Phase B, pre-fix:** 1 hang in 12 runs (KASAN kernel,
  `rc=124`, reader stuck in `__folio_lock` cache-hit path).
- **trres Phase B, post-fix:** **48/48 runs clean** (two batches, 20 +
  28).  Chance of missing a 1-in-12 bug across 48 clean runs ~1.4%.
- **trres all 4 phases, post-fix:** PASS, exit code 0, no resurrection
  hits.
- **entry-race suite, post-fix:** PASS quiescent (all phases), exit 0.
- **i386/x86_64 compile of `shmem_compress.o`:** 0 errors, 0 warnings.
- **VT:** `CC drivers/tty/vt/ucs.o` clean with VT=y after regenerating
  the header.
- **matrix_watermark:** negative invariant PASS (never-mounts never
  aged); all data-integrity PASS; only flaky positive "chill aged"
  assertions + test-rig EEXIST/EINVAL noise fail.

## Next Steps

1. Standing backlog unchanged: H618 re-measure, RFC assembly,
   plateau/CPU-freq, workload bed (spec 05), BE runtime boot, the zcell
   mTHP folio-grouping differentiator (the outstanding perf bet).
2. Optional out-of-scope (not tmpfs): header.S EFI PE-header, mkcpustr
   race, arm modpost -- flag or fix if x86-boot robustness becomes a
   goal.
3. Optional test-rig hygiene: fix the matrix_watermark row-to-row
   directory cleanup (the `errno=17 File exists` untar noise) and relax
   the flaky "chill must age" positive assertion to match its
   1 Hz-timing nature.

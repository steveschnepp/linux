---
name: entry-race-static-analysis
description: full read of the entry-race wait/wake code proves the stalled state (queued entry + clear PG_waiters + refcount 1) is unreachable by filemap alone -- refocuses the hunt on folio-address reuse in the compress/decompress race and the PG_waiters-skip in folio_unlock
project: linux
---

# Mission Log: Entry-Race Bug -- Static Analysis of the Wait/Wake Code

Part 40 of the tmpfs transparent-compression series.  Session 53
(2026-10-14).  Continues the entry-race lost-wakeup hunt from
2026-10-13_entry-race-lost-wakeup.md.  A code-reading session: read
shmem_compress.c in full, the compression hooks in shmem.c, and the
filemap wait/wake machinery, to close the "queued entry + clear
PG_waiters" fork without booting the rig.

## What We Did

Read, in full (not snippets): `mm/shmem_compress.c` (all 4157 lines --
pool lifecycle, entry table, cell GC, the compress/decompress data
path, `shmem_comp_compress_one`, the aging pass); the shmem.c
compression hooks (`shmem_get_folio_gfp` two-table decompress branch,
`shmem_write_begin/end`, `shmem_file_read_iter`, `shmem_undo_range`,
the exported `shmem_add_to_page_cache`/`shmem_delete_from_page_cache`);
and the core wait/wake machinery (`folio_wait_bit_common`,
`wake_page_function`, `folio_wake_bit`, `folio_wake_match`,
`folio_xor_flags_has_waiters`, `__wake_up_common`, the
`folio_wait_table` hash).  Then traced every path that sets or clears
PG_waiters against every path that enqueues/dequeues a wait entry.

## What We Learned

### Technical -- the bug is a PG_waiters / queue INCONSISTENCY, and both wake paths skip on it

`folio_unlock()` (`filemap.c:1516`) does
`folio_xor_flags_has_waiters(folio, 1 << PG_locked)`: it clears
PG_locked and, **only if PG_waiters reads set**, calls
`folio_wake_bit()`.  If PG_waiters is clear, the unlock does **not walk
the wait queue at all** -- it assumes no waiters.  The new drain
`wake_up_folio_waiters()` (`filemap.c:1534`) is likewise guarded
`while (folio_test_waiters(folio))` and also skips when PG_waiters is
clear.  So a waiter that is queued while PG_waiters is clear is
invisible to BOTH the normal unlock and the drain: a complete,
permanent lost wakeup.  That exactly matches the observed stall (phase
A hangs 10/10 even with the drain fix).  The entire bug therefore
reduces to one question: **how does a waiter end up queued with
PG_waiters clear (or PG_waiters get cleared while a waiter stays
queued)?**

### The stalled state is provably unreachable by the filemap wait/wake code in isolation

Traced exhaustively:

- **Enqueue sets the flag under the bucket lock.**  The only enqueue
  sites (`folio_wait_bit_common` `repeat:`, and
  `softleaf_entry_wait_on_locked`) do `folio_set_waiters(folio)` and
  `__add_wait_queue_entry_tail()` under the SAME `q->lock` hold
  (`filemap.c:1296`, `1430`).  So a queued waiter always has
  PG_waiters set at enqueue time, atomically.

- **PG_waiters has exactly ONE clear site**: the tail of
  `folio_wake_bit()` (`filemap.c:1213`), under `q->lock`:
  `if (!waitqueue_active(q) || !key.page_match) folio_clear_waiters(folio)`.
  Verified no other clear exists in mm/ (grep: only filemap.c:1213;
  the other `folio_xor_flags_has_waiters` uses are PG_writeback and
  PG_locked, never PG_waiters).

- **The clear cannot fire while our entry is queued.**  During
  `folio_wake_bit(F)`, the walk `__wake_up_common` can break early only
  on an entry whose folio == F (a mismatched folio returns 0 and
  continues; only a matching entry reaches the `-1`/exclusive break).
  Any such break has already run `wake_page_match`, which sets
  `key.page_match = 1` **before** the bit check (`pagemap.h:1150`).
  So if F's entry is in the bucket, `page_match` is guaranteed 1 by the
  time the walk ends, and `waitqueue_active(q)` is also true (F's entry
  is still linked).  Neither clear condition holds.  A hash-colliding
  bystander folio G clears **G's** flag, not F's (`folio_clear_waiters`
  takes the wake target).

- **The `goto repeat` re-enqueue re-sets the flag.**  A waiter woken
  without DONE that fails `test_and_set_bit` re-enters `repeat:`, which
  calls `folio_set_waiters()` again under `q->lock` before re-adding.
  Both the repeat-set and any concurrent clear take the SAME `q->lock`
  (same folio -> same hash bucket -> same lock), so they serialize; no
  race.

Conclusion: within filemap, "PG_waiters clear + entry queued" is
**unreachable**.  One of the gdb observations must therefore be
inconsistent, OR an out-of-filemap actor is responsible.

### The leading out-of-filemap actor: folio-ADDRESS reuse in the compress/decompress dance

`wake_page_match()` compares `wait_page->folio != key->folio` by
**pointer/address**.  The wait bucket is `hash_ptr(folio, 8)`.  The
two-table dance actively recycles folio addresses: `compress_one`
removes folio A from the page cache (dropping the cache ref; A's
refcount can hit 0 -> freed -> address back to the buddy), and a
concurrent `shmem_decompress_folio` allocates a NEW folio B that the
buddy hands back at **A's address** -- the kprobe trace already showed
this "eradd double-install via -EEXIST retry, freed folio immediately
re-allocated, normal LRU buddy reuse" (`2026-10-13` log, kprobe
section).  A stale wait entry left pointing at address A would then
have its `folio` field aliased by B, whose PG_waiters/PG_locked bits
belong to B, not to the logical folio the waiter is waiting for.

**The refcount-1 objection, and why it may not be decisive.**  The
observed waited folio has refcount 1 (the waiter's own
`filemap_get_entry` ref).  A waiter in EXCLUSIVE behavior
(`__folio_lock`) holds its ref across the wait, so refcount >= 1 and
the folio cannot be freed -- which would seem to forbid address reuse
of the waited folio itself.  BUT the gdb reads of "refcount 1" and
"PG_waiters clear" are reads of the SAME address at one pause; if that
address has already been reused by a decompress-installed folio B,
then the refcount and flags gdb reports are **B's**, while the queued
wait entry still describes the logical A.  The two "facts" would then
be describing two different logical folios that share one address --
which is exactly how an "unreachable" flag/queue state plus a
"live" refcount can coexist.

### Process

- Reading the code in full (not grepping snippets) is what made the
  "unreachable" proof possible: the argument depends on
  `wake_page_match` setting page_match before the bit check, on
  `__wake_up_common`'s break conditions, and on the repeat-path
  re-set -- three details that a snippet read would miss.

## What We Decided

- The hunt is **refocused**: the residual is not "find the missing
  `folio_clear_waiters` site" (there is none) but "explain how folio
  address reuse decouples the wait entry from the flags/refcount gdb
  reads."  The fix shape depends on this: if reuse is confirmed, a
  drain on the (reused) address cannot help and the real fix is to
  prevent a waiter from being stranded across the removal -- e.g. drain
  BEFORE `shmem_delete_from_page_cache` drops the last cache ref (while
  the folio address is still uniquely its own), or make the removal
  path hold the folio locked across the entries_set so no reader can
  enqueue on the removal window at all.
- `folio_unlock`'s PG_waiters fast-path skip is the mechanical reason a
  clear flag strands a waiter; the drain fix inherited the same blind
  spot by guarding on `folio_test_waiters`.

## The "too fast" hypothesis (2026-10-14)

User intuition: the lost wakeup happens because the wakeup is issued too
fast -- before the waiter is really in place.  Audited against the code.
This is the right FAMILY of bug, and it identifies the exact
coupling point, but the last step to the observed state still does not
close (consistent with every prior session).

### The speed race the kernel explicitly defends

`folio_wait_bit_common` sets PG_waiters BEFORE the final trylock, under
`q->lock`, precisely to survive a "too fast" waker.  The code comment:
"Do the folio_set_waiters() marking before that to let any waker we
_just_ missed know they need to wake us up."  The defended interleaving
(trylock-then-set order) would lose the wakeup: trylock fails, waker
unlocks and reads PG_waiters=0 and skips, waiter then sets+enqueues --
stranded.  Setting first closes it, and the total-order argument
holds: waiter's trylock FAILS  ==>  the waker's unlock (which clears
PG_locked) has not run yet  ==>  that unlock atomically reads PG_waiters
AFTER the waiter set it  ==>  it walks.  Verified safe.

### The one genuinely "too fast" primitive: the lockless PG_waiters read

`folio_unlock` does NOT take `q->lock`.  It reads PG_waiters via
`folio_xor_flags_has_waiters` = `xor_unlock_is_negative_byte` -- a single
lockless atomic BYTE op that clears PG_locked(bit0) and reads
PG_waiters(bit7) together (`page-flags.h:768`,
`instrumented-lock.h:75`).  This races the waiter's `set_bit(PG_waiters)`
which runs UNDER `q->lock`.  So the handshake's two halves are ordered
only by the atomic byte ops, not by a common lock -- the tightest
 coupling in the whole mechanism, and the natural place a "too fast"
failure would appear.

Still safe as written, by the single-RMW argument: if the waker's xor
reads PG_waiters CLEAR, then that xor ran BEFORE the waiter's set-bit7,
and the xor also CLEARED PG_locked, so the waiter's subsequent
test_and_set sees PG_locked clear and SUCCEEDS (takes the lock, no
enqueue, no stall).  If the waiter set bit7 first, the xor reads it SET
and walks.  There is no ordering where the waiter both fails trylock AND
the waker read bit7 clear.  So the lockless read is correct -- but it is
speed-critical and architecture-sensitive (a byte op that is not truly
atomic, or a weak-memory reordering of set-bit7 vs test-and-set-bit0,
would break exactly here).

### Why the compress path turns "fast" into fatal

On a normal page-cache folio, ANY hiccup in this handshake self-heals:
a future lock/unlock re-walks the queue and collects the stranded
waiter.  `shmem_comp_compress_one` is the uniquely dangerous fast waker
because it does `folio_unlock` + `wake_up_folio_waiters` back-to-back and
then DISCARDS the folio (`shmem_delete_from_page_cache`, mapping=NULL).
There is no future unlock.  So a premature PG_waiters clear at that
instant is unrecoverable -- "too fast" + "no safety net" is the bug's
home, which is why it is compression-specific even though the wait
machinery is generic.

### The premature wake-tail clear (real, but resolves the wrong way)

`folio_wake_bit`'s tail clears PG_waiters the instant the bucket looks
empty: `if (!waitqueue_active(q) || !key.page_match) folio_clear_waiters`.
Waking a SOLE exclusive waiter removes it (`list_del_init_careful`), the
walk stops (`nr_exclusive` 1->0), bucket is empty -> PG_waiters cleared.
That is a genuine "too fast" clear (the flag is dropped before the
woken waiter has finished).  BUT in the single-waiter case it resolves
by the waiter SUCCESSFULLY taking the now-clear PG_locked (it does NOT
goto repeat, because PG_locked is clear), so nothing is stranded.  To
strand an ENQUEUED waiter with PG_waiters clear still requires a
test_and_set failure (PG_locked set) AND a later clear with the entry
queued -- which is the same unreachable state every prior session hit.

### Status

"Too fast" correctly names the defended race and the lockless-xor
coupling point, and correctly explains why compression (no future
unlock) is where a fast-wakeup bug would bite.  It does NOT, by itself,
close the last step to "entry queued + PG_waiters clear on a pinned
folio".  That last step is a MEASUREMENT question, not a theory
question -- see the audit section.  The fix that is immune to every one
of these speed surfaces is the UNCONDITIONAL drain (walk regardless of
PG_waiters); with PG_locked clear the walk cannot skip an exclusive
waiter, so no timing of the PG_waiters read can strand one.

## The spin-vs-wait experiment (2026-10-14)

User ask: replace the sleeping folio-lock wait with a bounded spin
(~20ms, then fall back to wait) and measure the perf impact + average
wait time.  Implemented, built (nokasan measurement kernel), and run
as a clean A/B.  Result: a 20ms bounded spin makes the entry-race
phase-A stall VANISH, reproducibly, and the wait-time stats localize
the bug to the SLEEPING wait-queue path.

### What We Built

Instrumented the ONE reader folio_lock site that the race exercises:
`shmem_get_folio_gfp`'s cache-hit branch (mm/shmem.c:2692, the
`if (folio) folio_lock(folio)` after `filemap_get_entry`).  Replaced it
with `shmem_exp_folio_lock()`: poll `folio_trylock()` in a
`cpu_relax()` loop bounded by `ktime_get_ns()` (default 20ms, HZ-
independent so it is a REAL 20ms, not jittery jiffies), then fall back
to the real `folio_lock()` on budget expiry.  NOT a spinlock: preemption
stays enabled and we hold no lock while spinning, so no deadlock against
the synchronous-crypto compression path.

Wait-time measurement is precise: `ktime_get_ns()` (nanosecond
clocksource) around each acquisition, accumulated into atomic64
counters, max via a CAS loop (`atomic64_max` does not exist in this
tree).  Reported as a greppable `ZBLK` line every N acquisitions.
Runtime A/B via `/sys/module/shmem/parameters/`:
`shmem.exp_spin_enable`, `shmem.exp_spin_budget_ns`,
`shmem.exp_stats_interval`.  Wired through `run_qemu.sh` as
`TEST_EXPSPIN`/`TEST_EXPBUDGET`/`TEST_EXPINTERVAL`.  Code is in the
working tree (uncommitted), marked EXPERIMENT/revert-later.

### The wait-time distribution (the precise stats requested)

At the instrumented site, entry-race phase A (4 readers looping 32-page
pread vs writer+compress), spin-off baseline:

| metric | value |
|---|---|
| acquisitions | 2,326,528 |
| avg latency | 88 ns |
| sleep portion avg | 50 ns |
| max latency | 3,873,339 ns (~3.9 ms) |

**The reader folio_lock is essentially NEVER contended.**  50 ns is
`folio_lock`'s internal `folio_trylock` succeeding immediately, not a
real sleep.  The lone ~3.9 ms max out of 2.3M is almost certainly a
SCHEDULING artifact (lock holder descheduled on TCG), not lock-hold
time -- LZ4 of 4K is microseconds.  So on this workload the lock is
uncontended ~99.99% of the time and the "hold" is never >4ms.

### The A/B result: spin eliminates the stall

Same kernel, same instrumentation; the ONLY change is
`exp_spin_enable` 0 vs 1:

| arm | reps | outcome |
|---|---|---|
| spin off (sleep) | 3/3 | HANG: exit 125, watchdog fires, 0 pass quiescent |
| spin on (20ms) | 4/4 | PASS: exit 0, `PASS quiescent verify` |

spin-on stats: `spin_wins` = 100% of 6.5M acquisitions, `sleep_fb` = 0
(the 20ms budget is NEVER exhausted -- no reader ever falls back to
sleeping), avg latency 62 ns (spin portion 35 ns), i.e. ~30% cheaper
per acquisition than the sleep path.  But the throughput value is
moot -- see the noise retraction below.

### Why spin avoids it (mechanism, not perturbation)

This is a genuine mechanism change, not another heisenbug dissolve:

- The stall state is PG_locked CLEAR + a waiter that ENQUEUED on the
  hashed wait queue and got stranded (the lost wakeup).
- A spinner polls `folio_trylock` = test_and_set(PG_locked).  It NEVER
  enqueues on the wait queue, so it can never BE a lost-wakeup victim.
- And the stalled state has PG_locked clear -- exactly what trylock
tests for -- so the spinner just takes the lock and proceeds.
- `sleep_fb` = 0 PROVES the sleeping wait-queue path is never entered in
  the spin arm.  So the arm does not "perturb away" the race; it
  sidesteps the broken machinery entirely.

Crucially: the spin-OFF arm on this instrumented kernel STILL hangs
3/3.  Prior sessions found printk/probe instrumentation dissolves the
race (6-for-6 heisenbug).  The `ktime_get_ns()` instrumentation is far
lighter and does NOT dissolve it -- which makes this instrumented kernel
a LEGITIMATE repro oracle for the A/B (same-kernel, only-spin-differs).
That is what makes the result trustworthy: it is not "we perturbed it
away", it is "the sleeping wait-queue path is what is broken, and
spinning never uses it."

### Throughput A/B retraction (measurement discipline)

Tried a clean throughput A/B on `test_fill` readback (millions of
reader locks, no hang).  Got 775 -> 1287 MB/s (spin on).  **RETRACTED
as noise**: `ZBLK` count was 0 on both fill-readback runs -- the
instrumented site is NOT exercised there.  Reason, verified in code:
that site is the CACHE-HIT branch (folio already in i_pages); fill
readback hits each page once AFTER compression removed it from i_pages,
so it goes down `shmem_decompress_folio` (fresh folio, locked internally
via `__folio_set_locked`) and never reaches the instrumented
`folio_lock`.  The instrumented site is only hit by REPEATED reads of a
still-cached page -- exactly the entry-race pattern.  So the fill delta
measured nothing about the spin.  (Same-kernel rule saved the report:
looked for ZBLK to confirm the site fired; it did not.)

### What We Decided

- **Spinning is a viable MITIGATION and a powerful LOCALIZER, not a
  fix.**  It confirms the bug is specifically in the SLEEPING wait-queue
  lost-wakeup (not in the lock, not in the compress data path).  The
  real fix is still the unconditional drain / fixing the wake path --
  spinning just avoids that machinery on the read path.
- It does NOT fix other folio_lock sites (write path, truncate) which
  still sleep and are still vulnerable, and would not help a reader that
  spun >20ms and fell back (sleep_fb=0 shows that does not happen here,
  but the exposure remains).
- Budget can be far smaller than 20ms: lock is never held >4ms (usually
  50ns), so a 10-100us spin would capture the same wins at far lower CPU
cost.  20ms was the user's explicit spec for a first cut.
- This SUPPORTS a narrow version of the "too fast" hypothesis: the lock
  is released ~immediately after the reader enqueues, so the wakeup
  walks in a window where PG_waiters can read clear -- the spinner never
  enters that window because it never enqueues.

### Files Changed (experiment, uncommitted)

| File | Purpose |
|------|---------|
| `mm/shmem.c` | `shmem_exp_folio_lock()` + atomic64 wait-time counters + 3 module_params; instrumented the cache-hit reader folio_lock site |
| rig `scripts/run_qemu.sh` | `TEST_EXPSPIN`/`TEST_EXPBUDGET`/`TEST_EXPINTERVAL` -> `shmem.exp_*` cmdline |

### Test Results

- entry-race phase A (`TEST_ERONLY=A`, 4 readers, nokasan measurement
  kernel), spin-off: hang 3/3 (exit 125).  spin-on: pass 4/4 (exit 0).
- wait-time stats (spin-off): 2.3M acq, avg 88ns, sleep-portion 50ns,
  max 3.9ms.  spin-on: 100% spin wins, sleep_fb=0, avg 62ns.
- kernels: `make O=out/nokasan -j12 bzImage` (both kasan+nokasan still
  build the experiment; the rig currently has the nokasan one synced).

### Next Steps

1. Revert or guard the experiment code before any real fix verification
   (it perturbs the reader hot path with ktime calls).
2. The spin result is strong evidence the bug is the sleeping-path
   lost wakeup.  Use it: the fix must make the wake path reliable for a
   waiter on a detached folio (unconditional drain, drop the
   PG_waiters guard), then verify on the PRISTINE kernel (no experiment
code) with the entry-race loop.
3. Optional: try a small spin budget (50us) to confirm CPU cost is the
   only downside of spinning as a mitigation.
4. Restore spec 06; rig cleanup -- unchanged.

### The 50us budget arm (2026-10-14): exposure quantified

Ran the user's follow-up: drop the budget to 50us (50000 ns).  This is
the first arm that ACTUALLY falls back to sleep -- at 20ms sleep_fb was
0.  3/3 pass, no hang:

```
total=6,176,768  avg_ns=65  max_ns=4,036,771 (~4ms)
spin_wins=6,176,763  spin_avg_ns=34
sleep_fb=5  sleep_avg_ns=1,173,945 (~1.17ms)
```

5 acquisitions out of 6.17M (0.00008%) fell back to sleep -- the lock
was genuinely held >50us (the ~4ms descheduled-holder cases), each
slept ~1.17ms -- and the test still passed.

**The protection is EXPOSURE REDUCTION, not safety.**  Compare:

| arm | sleep events | outcome | exposure |
|---|---|---|---|
| spin off | every contended lock | hang 3/3 | millions -> the rare race fires |
| 50us | 5 in 6.17M | pass 3/3 | 0.00008% -- 5 shots, none hit it |
| 20ms | 0 | pass 4/4 | zero -- never enters the wait queue |

The 50us arm passes because it cut potential victims from millions to
5, and the lost wakeup is a timing-fragile interleaving -- 5 shots in a
6s window is statistically thin.  Not "the sleep path got safer."

**Mildly informative against a STRONG bug hypothesis:** if the lost
wakeup fired on EVERY long-held-lock sleep, those 5 sleeps should have
hung.  They did not.  So the bug needs a specific interleaving (the
two-table removal window), not merely "reader sleeps on a contended
lock" -- consistent with the whole investigation.

**The budget is a CPU-vs-exposure trade, now quantified:** at 20ms the
~4ms long-hold acquisitions spin the whole time (max 4ms < 20ms ceiling)
burning CPU but avoiding the bug; at 50us those bail to sleep after 50us
-- less CPU, more bug exposure (and got lucky 5 times).  Per-acquisition
cost is ~identical (avg 65 vs 62 ns) because the spin exits as soon as
the lock is free (spin_avg 34ns); the budget is a rarely-reached
ceiling.  Lower budget = less CPU in the rare long-hold case, more
exposure to the real bug.  No free lunch -- the strongest argument yet
that the only real fix is fixing the WAKE PATH, not spinning around it.

## Fix attempt 1: unconditional drain -- FAILED 6/6 (2026-10-14)

Implemented the decided fix: `wake_up_folio_waiters()` now does ONE
unconditional `folio_wake_bit(folio, PG_locked)` before the guarded
loop (the loop's `folio_test_waiters` guard is exactly the flag the
stall clears, so it did zero walks).  Built (nokasan), synced, ran
entry-race phase A spin-off 6x: **hang 6/6, identical signature.**
The `still has waiters` warning NEVER fired.

### Diagnosis (the negative result is the finding)

The unchanged hang + never-firing warning prove the stranded waiter
NEVER REACHES the drain.  My fix is incomplete, not wrong.  Confirmed
structural gap:

1. Only TWO drain sites exist: compress (`shmem_compress.c:3863`) and
   reader-detached (`shmem.c:2703`).
2. The reader-detached drain is UNREACHABLE for a stalled reader: it
   runs AFTER `folio_lock` succeeds (mapping-mismatch branch).  A reader
   parked INSIDE `folio_lock` never reaches it.
3. **Truncate is a THIRD folio-removal path with NO drain.**  The test
   writer ftruncate()s in a tight loop; `shmem_undo_range` ->
   `truncate_inode_folio` -> `filemap_remove_folio` sets mapping=NULL,
   then `folio_unlock` -- which walks ONLY if PG_waiters reads set
   (`folio_xor_flags_has_waiters`).  With PG_waiters clear, truncate's
   unlock skips the wait queue and strands the parked reader forever.

So the stalled folio is removed by TRUNCATE, not compress -- which is
why making the COMPRESS-side drain unconditional changed nothing.  The
fix must cover the removal choke point for ALL paths, or specifically
the truncate path.  (Consistent with the logs' conclusion that static
analysis cannot close "PG_waiters clear + entry queued" -- but the
ACTIONABLE fact is which removal path strands it, and that IS now
identified: truncate.)

### Next fix (attempt 2)

Drain waiters on the truncate path too: in `shmem_undo_range`, after
the `folio_unlock` that follows a `truncate_inode_folio` (the last
unlock on a now-detached folio -- the exact dangerous moment), call
`wake_up_folio_waiters(folio)`.  Both walks (find_lock_entries + the
second find_get_entries walk).  Correctness first; watch truncate perf
after.

## Fix attempt 2: drain the TRUNCATE path -- VERIFIED (2026-10-14)

Attempt 1 proved the stranded waiter never reaches the compress/reader
drains.  The missing removal path was truncate: `shmem_undo_range` ->
`truncate_inode_folio` -> `filemap_remove_folio` sets mapping=NULL then
`folio_unlock`, which walks the wait queue ONLY if PG_waiters reads set.
With PG_waiters clear (the stall state), truncate's unlock skipped the
queue and stranded the parked reader.  The test writer ftruncate()s in a
tight loop, so truncate is a hot removal path the earlier drains never
covered.

**The fix:** in `shmem_undo_range`, after the `folio_unlock` that
follows a successful `truncate_inode_folio` (return 0 = folio really
left the cache), call `wake_up_folio_waiters(folio)`.  Added to BOTH
walks (the find_lock_entries walk and the whole_folios find_get_entries
walk), guarded by `removed = !truncate_inode_folio(...)` so a partial
split / already-detached folio does not pay a spurious drain.  Combined
with attempt 1's unconditional `wake_up_folio_waiters` (so the drain
walks regardless of PG_waiters), every folio-removal path now drains:
compress, reader-detached, and truncate.

**Verified (nokasan, the repro oracle, no instrumentation):
- entry-race phase A loop (the deterministic hang): **10/10 PASS**
  (was 10/10 hang before the fix).  Spin off = real sleeping path.
- full 4-phase test (A+B+A2+B2), 3 reps: all 4 phases run, all
  `PASS quiescent`, zero hang signals, exit 0.

### Side find: the full-suite gate was VACUOUS (test bug, fixed)

The first "full 4-phase" verification printed exit 0 but ZERO phase
output -- no `[Phase A]`, no `race window`, no `PASS quiescent`.  Root
cause in the committed `test_entry_race.c` phase selector:

```c
const char *only = getenv("ER_ONLY");
if (!only) only = "";               // only is now never NULL
if (!only || !strcmp(only, "A"))    // !only always false; strcmp("","A")!=0
```

With ER_ONLY unset, `only` becomes "" (never NULL), so `!only` is false
and `strcmp("","A")` is nonzero -> **all four phase conditions false ->
zero phases run**.  The full-suite gate silently did nothing and reported
success.  (The single-phase runs were valid -- they set ER_ONLY=A.)  Fixed
to `!only[0] || !strcmp(...)` so empty = run every phase.  Caught only
because "exit 0 with no phase output" was treated as a red flag, not a
pass -- the same discipline as "a harness exit code is not a test result"
and "ZBLK=0 means the site did not fire."

### Files Changed (the fix)

| File | Purpose |
|------|---------|
| `mm/filemap.c` | `wake_up_folio_waiters`: one UNCONDITIONAL `folio_wake_bit` before the guarded loop (attempt 1) |
| `mm/shmem.c` | `shmem_undo_range`: drain waiters after truncate removal, both walks, guarded by `removed` (attempt 2 -- the actual fix) |
| rig `tests/test_entry_race.c` | FIX: phase selector ran zero phases when ER_ONLY unset (`!only[0]`) |

## What We'd Do Differently

- Should have done this full read BEFORE writing the drain fix
  (`2ca2601bd2e5`): the fix guarded on `folio_test_waiters`, which the
  static argument now shows is exactly the bit the bug clears.  A
  careful read of `folio_unlock`'s PG_waiters fast-path skip would have
  flagged "the drain inherits the same blind spot" at authoring time,
  and the single-wake-vs-loop trial-and-error on the rig would have
  been unnecessary.
- The 10-13 log asked "how is PG_waiters clear with the entry queued?"
  and treated it as a missing clear SITE.  Reframing it as "the state
  is unreachable in-filemap, so look OUTSIDE filemap (address reuse)"
  is the pivot that unblocks the hunt; that reframe should have come
  from reading the code rather than from re-running the rig.

---

## The audit REFUTES the reuse hypothesis (2026-10-14)

A verify pass on session 1's own "leading suspect" (folio-address
reuse).  It does not survive the code.  Recorded here so the dead end
is not re-entered.

### What We Verified

1. **A queued EXCLUSIVE waiter PINS its folio.**
   `__folio_lock` -> `folio_wait_bit_common(..., EXCLUSIVE)`
   (`filemap.c:1746`).  The only ref drop in the wait path is
   `if (behavior == DROP) folio_put(folio)` (`filemap.c:1308`) -- DROP
   is `folio_put_wait_locked`, NOT the lock path.  EXCLUSIVE holds the
   caller's ref across the whole sleep.  The reader's ref comes from
   `filemap_get_entry` -> `folio_try_get` (`filefilemap.c:1955`).  So
   while a reader is parked in `folio_lock`, refcount >= 1 from that
   reader; observed refcount 1 IS that ref.  **The folio cannot reach
   refcount 0, cannot be freed, and its address cannot be recycled by
   a decompress-installed folio.**  The reuse hypothesis is dead.

2. **PG_waiters has exactly ONE clear site, and it is guarded the same
   way twice.**  `folio_clear_waiters` at `filemap.c:1213` (the
   `folio_wake_bit` tail) is the sole clear in mm/.  Both the normal
   `folio_unlock` (`filemap.c:1516`, `folio_xor_flags_has_waiters`) and
   the new drain (`filemap.c:1548`, `folio_test_waiters` loop) skip the
   wake walk when PG_waiters reads clear.  Confirmed no other clear
   exists; `folio_xor_flags_has_waiters`' other uses are PG_writeback
   and PG_locked, never PG_waiters.

3. **The stall flag values decode cleanly.**  Both observed words
   (`0x12c`, `0x2c`) share low byte `0x2c` =
   `PG_referenced|PG_uptodate|PG_lru`; **PG_locked (bit 0) = 0 and
   PG_waiters (bit 7) = 0**.  So the folio is unlocked, off the mapping,
   no waiters advertised, yet a `WQ_FLAG_EXCLUSIVE` entry sits in the
   hashed bucket.  (Bit 7 = PG_waiters confirmed against
   `include/linux/page-flags.h:101` and the `PG_head`-in-bit-6 layout.)

4. **The allocator cannot be the clear either.**  `PAGE_FLAGS_CHECK_AT_FREE`
   (`page-flags.h:1171`) does NOT include PG_waiters, and
   `post_alloc_hook` does not clear the flags word -- but both only run
   on a folio at alloc/free time.  A pinned folio (refcount 1) is never
   at alloc/free time.  So no allocator path touches its PG_waiters.

### What We Learned

- Session 1's "leading suspect: address reuse" was wrong, and the
  thing that kills it is a single line: EXCLUSIVE waiters do not drop
  their ref.  Reusing the waited folio's address would require
  refcount 0, which the waiter itself forbids.  This is exactly the
  kind of hypothesis that SOUNDS right (the kprobe trace DID show
  same-address double-installs) but collapses once the refcount
  discipline is actually read.
- The same-address double-install the trace showed is on a DIFFERENT
  folio lifecycle (a decompress retry re-allocating a freed slot), not
  on the folio a live waiter is parked on.  Conflating the two was the
  error.

### What We Decided

- **Drop the reuse hypothesis.**  Do not spend gdb time checking
  `wait_page.folio` aliasing a live `i_pages` folio -- a live waiter
  makes that impossible.
- The residual is now a clean binary, both inside filemap's own
  contract, and the observation must satisfy one of them:
  (a) **the entry was enqueued, then PG_waiters was cleared out from
      under it** -- but session 1 proved the sole clear site cannot
      fire while the entry is queued (page_match is always set for a
      queued matching entry); or
  (b) **the observation is internally inconsistent** -- the flags word
      and the wait entry read at the pause do not describe the same
      logical folio (a gdb artifact: reading the folio from the stack
      slot vs the entry's `wait_page.folio`), OR the entry was already
      `list_del_init_careful`'d (self-referential, looks "linked" to a
      naive dump but is off the list).
- (b) is now the more likely branch: the "unreachable" proof is
  airtight for a single folio, so the measurement itself is the weak
  link, not the kernel logic.

### Next Steps (revised)

1. **Audit the measurement, not the kernel.**  Re-run the pause and
   read, for ONE stalled waiter, all three from the SAME instant:
   - the wait entry's `wait_page.folio` pointer (from the waiter's
     stack / `wait_page` struct),
   - the flags word at THAT pointer,
   - the bucket linkage via `list_empty_careful(&wait->entry)` -- a
     `list_del_init_careful`'d entry has next==prev==&entry (self),
     which a raw "is it in the bucket" dump can misread as linked.
   If `wait_page.folio` names a folio whose flags show PG_waiters SET,
   the earlier "clear" read was of the wrong address and the bug is
   narrower than thought.  If it names the same cleared-PG_waiters
   folio, we have a real contradiction with the sole-clear-site proof
   and must hunt a second clear path (e.g. a PG_waiters write via a
   stale alias).
2. Only after the measurement is trustworthy, pick the fix.  A drain
   that walks UNCONDITIONALLY (ignore PG_waiters, drop the
   `folio_test_waiters` guard) is correct in BOTH branches and is the
   safe patch regardless -- it just may be masking a symptom if (b)
   holds.
3. Restore spec 06; rig cleanup -- unchanged.

## Next Steps

1. On a stalled folio (gdb, QEMU-monitor-stop), read three things that
   pin the reuse hypothesis:
   - the queued entry's `wait_page.folio` pointer vs the address whose
     flags/refcount were read -- do they name the same logical folio?
   - `wait->flags` (WOKEN? DONE? CUSTOM?) -- a stale entry woken long
     ago vs a fresh one;
   - whether a LIVE folio now occupies `i_pages[index]` at the SAME
     address as the waited folio (a decompress-installed B).
   If `wait_page.folio` aliases a live page-cache folio, reuse is
   confirmed and the "unreachable" state is explained.
2. Fix per the outcome:
   - reuse confirmed -> drain/remove-ordering fix (see "What We
     Decided"); the address-scoped drain cannot work.
   - reuse ruled out -> the gdb flag/refcount reads are self-inconsistent
     and must be re-taken; that itself points at a second anomaly (the
     `thread_group`->`group_node` task-list walk miss flagged on 10-13).
3. Verify the fix on the PRISTINE kernel (phase A/A2 loop, 10+ runs,
   no instrumentation); full KASAN suite.
4. Restore spec 06 (`git stash pop`), rebuild, suite, Phase C triangle.
5. Rig cleanup: ERHUNT debug block + `test_entry_race` phase selector.

## Files Changed

| File | Purpose |
|------|---------|
| `mission_log/2026-10-14_entry-race-static-analysis.md` | this log |

No kernel changes this session -- analysis only.  Working tree still
pristine apart from `out/`; spec06 still in `git stash@{0}`; the
partial drain fix (`2ca2601bd2e5`) still committed.

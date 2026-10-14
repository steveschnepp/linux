---
name: entry-race-lost-wakeup
description: pre-existing lost folio-lock wakeup in the two-table compression dance -- readers sleep forever on a cleared PG_locked; full state pinned via gdb on a paused QEMU; kprobe on folio_unlock armed to close the last gap; spec06 implementation stashed mid-hunt
project: linux
previous: 2026-10-13_watermark-ram-two-phase-design.md
---

# Mission Log: The Entry-Race Lost-Wakeup Hunt

Part 39 of the tmpfs transparent-compression series.  Session 52
(2026-10-13, fourth log of the day).  Started as spec 06 Phase C
(the triangle rerun); became a deep kernel-bug hunt when the rerun
revealed a suite hang.  Status at writing: the bug's state is fully
pinned, the mechanism is narrowed to one unresolved fork, and the
closing kprobe experiment is about to run.

## Session state -- READ FIRST

- **`git stash@{0}` "spec06 wip" holds the complete spec 06
  implementation** (mm/shmem_compress.c, mm/shmem.c, mm/cell_store.c,
  include/linux/cell_store.h, include/linux/shmem_compress.h): the
  RAM-usage write-path flip, the unified maintenance worker with
  highwater kick + age gate, and per-mount gc_percent.  The kernel
  working tree is otherwise PRISTINE (old kernel); all ERHUNT
  instrumentation and delay injections were `git checkout`-ed away
  before the gdb phase.
- **Both KASAN builds now have DWARF** (CONFIG_DEBUG_INFO_DWARF5=y,
  DEBUG_INFO_NONE off; full -g rebuild at 19:34, bzImage+vmlinux in
  sync) -- gdb can read every struct type.  Codegen unchanged.
- Rig has TEMP debug plumbing: `test.eronly=A|B|A2|B2`,
  `test.erwm=`, `test.erextra=` (phase selector in test_entry_race),
  and an init.sh entryrace debug block (sysrq dumps, hung_task_panic=0,
  tracefs mount, kprobe setup + auto trace dump on stall).  All to be
  reverted after the hunt.  The rig also carries the gc_percent fill
  plumbing (test_fill argv[7], test.fillgcp=) from spec 06.
- spec 06 gates run so far: the 2G/2G wm=0:20 gcp=20 gate boot PASSED
  on the spec06 kernel (flip fired "compression hot (ram usage 20%)",
  mount survived, gc_percent=20 visible in the kernel-generated
  /proc/mounts line); the full KASAN suite HUNG at test_entry_race --
  which this log shows is pre-existing, so the suite result says
  nothing against spec 06.  Phase C (triangle rerun on the spec06
  kernel) is STILL PENDING.

## What We Did

### 1. The suite hang, and proving it pre-existing

The spec-06 KASAN suite hung at the entry-race test: two reader
threads blocked forever in `pread -> shmem_get_folio_gfp ->
folio_lock`, then hung_task panic.  Suspecting my own changes, I
stashed them and rebuilt the OLD kernel: **the old kernel hangs
identically, 3/3** -- pre-existing.  (The user's directive: fix
either way.)

### 2. Narrowing the surface

- Phase selector in test_entry_race: phase A (merge=1, eager, cells)
  hangs deterministically; **A2 (zsmalloc, cell=0) hangs too** ->
  shared path, not cell-specific.
- `compress_watermark=100:100` (never compresses, no entries ever):
  **passes clean** -> the bug needs the compression paths.
- Audited line by line, all clean: shmem_file_read_iter (unlocks
  immediately after get_folio), shmem_write_begin/end (hwpoison path
  unlocks; write_end unlocks before compress), the get_folio_gfp
  compression hook (-EEXIST/-ENOENT retry), shmem_decompress_folio
  (every error path unlocks+puts; success returns locked per
  contract), shmem_comp_compress_one (unlock on every skip/error),
  shmem_undo_range (both walks + partial), truncate/invalidate hooks.

### 3. Delay-injection bisect: the race is timing-fragile

10 ms injections at each candidate window (S1 write_end
unlock->compress gap, S2 compress_one delete->entries_set, S3
decompress install->return, S4 truncate xa_store->entry_drop): none
hang -- **any perturbation dissolves the race**, including the
cheapest printk in the wait path and even trylock-first.  A
heisenbug in the strongest sense; only the pristine kernel
reproduces it.

### 4. gdb on a paused QEMU (the user's suggestion)

- QEMU gdbstub + **QEMU-monitor `stop`** (needed: `target remote`
  halts only CPU0; inferior calls refuse while other vCPUs run).
- DWARF rebuild so gdb sees types; offsets double-checked via
  `ptype /o`.
- The blocked threads are invisible to a `task_struct.tasks` walk
  (55 tasks, wraps after the main thread) though khungtaskd found
  them -- found via **brute-force RAM scan for comm** instead.
  Side anomaly, unresolved, possibly a second bug: this 7.3 kernel
  renamed `thread_group` -> `group_node`, and an equivalent of
  for_each_process_thread missed threads that the kernel's own
  detector found.
- `task->stack` reads EMPTY for the sleeping tasks (direct-map
  alias); the live SP is `task->thread.sp` (offset 0x940+24).
  Symbolizing the stack from SP reconstructs the exact call chain.

### 5. The pinned state (the evidence table)

Two readers, distinct folios (index 27 / index 20):

| fact | value |
|---|---|
| wait | `folio_lock` (PG_locked, bit 0), exclusive handoff style |
| PG_locked | **CLEAR** on the waited folio |
| folio->mapping | **NULL** (removed from the page cache) |
| refcount | **1** -- the waiter's own filemap_get_entry ref |
| wait entry | still enqueued in the CORRECT hashed bucket (`folio_wait_table`, func=wake_page_function, WQ_FLAG_EXCLUSIVE) |
| bucket | single entry, correctly linked -- no abort, no skip possible |
| lock owner | none anywhere (full task scan) |

A wake walk on that bucket with the bit clear MUST wake the waiter
(`wake_page_function`: match -> test_bit clear -> WOKEN ->
wake_up_state).  Every folio_unlock does that walk; no raw
`__folio_clear_locked` exists in the compression code.  Therefore:
**the last path that locked this page-struct never folio_unlock()ed
this same page-struct** -- a lock/unlock pair mismatched across the
two-table replacement moment (cache folio + compressed entry
transiently coexist; write_end unlocks -> compress_one re-locks ->
delete_from_page_cache (mapping NULL) -> entries_set -> unlock,
racing a reader that grabbed its ref just before the removal).

### 6. The closing tool: kprobes on lock/unlock

tracefs is NOT mounted in the rig guest (had to `mount -t tracefs`).
Kprobe fetch syntax: `arg1` failed for `folio_lock`;
**`folio=%di:u64` registered fine for `folio_unlock` (eru)**.  With
both probes: let the stall establish, dump the ring buffer, and grep
the waited folio's address -- a last event of `erl` with no `eru`
after it proves the mismatched-owner fork and names the guilty task.

## What We Learned

### Technical

- **The lost-wakeup shape**: waiter asleep on a bit that is clear,
  still correctly queued, wake never walked.  The only consistent
  mechanisms are (a) an unlock that never happened on that address
  (owner/page mismatch) or (b) a refcount-0 window that made
  wake_page_function skip -- (b) is ruled out here (refcount never
  drops below the waiter's own ref).
- **A folio removed from the page cache with one stale reader ref
  looks exactly like**: mapping NULL, refcount 1, uptodate, on LRU,
  unlocked.  That state is "compress_one removed it, all producer
  refs dropped, wake never delivered".
- Debugging toolkit that now exists and works:
  QEMU `-s` + monitor `stop` for a fully-paused VM; gdb raw-memory
  python scripts (no inferior calls needed except symbols);
  `task->thread.sp` for sleeping-task stacks; brute-force comm scan
  when task lists lie; kprobe `%di:u64` fetch; tracefs must be
  mounted explicitly in this guest.
- Heisenbug handling: perturbation-sensitive races need non-invasive
  tools (kprobe ring buffers, gdb) -- printk probes destroyed the
  race three times before switching strategy.

### Process

- **Baseline first**: proving the hang pre-existing via stash +
  rebuild turned "my bug" into "a bug" in one step and de-risked the
  spec06 work immediately.
- The mission-log-first instruction from the user proved its worth
  mid-hunt: the stash state and pending gates are recorded where the
  next session can find them, not in a chat transcript.

## What We Decided

- **No fix until the last gap closes**: the kprobe experiment
  distinguishes "unlock never ran" from "unlock ran, wake lost".
  The fix differs completely between those two worlds.
- spec 06 stays stashed until the hunt ends; then: pop, rebuild both
  kernels, rerun the full KASAN suite (which must now also pass
  test_entry_race OR the entry-race hang gets fixed first), and run
  Phase C's triangle.

## Rules Added

- **QEMU gdbstub: `target remote` stops only CPU0.**  For inferior
  function calls, pause the whole VM via the QEMU monitor.
- **tracefs is not mounted in the rig guest**; mount it before any
  tracing.
- **kprobe fetch: use `%di:u64` (register), not `arg1`**, in this
  kernel.

## What We'd Do Differently

- Would have gone straight to kprobes/gdb after the second
  heisenbug observation instead of trying four printk variants; the
  perturbation destroyed the evidence each time.

## Files Changed

| File | Purpose |
|------|---------|
| kernel `out/kasan/.config` | DEBUG_INFO_DWARF5=y (persistent; enables all future gdb work) |
| `git stash@{0} "spec06 wip"` | the complete spec 06 implementation (5 kernel files) |
| rig `tests/test_entry_race.c` | TEMP: ER_ONLY/ER_WM/ER_EXTRA phase selector |
| rig `guest/init.sh` | TEMP: entryrace debug block (sysrq, hung_task_panic=0, tracefs, kprobes + auto-dump) |
| rig `scripts/run_qemu.sh` | TEST_ERONLY/TEST_ERWM/TEST_EREXTRA plumbing (keep or drop with the block) |
| host `/tmp/erhunt*.py`, `/tmp/scan_*.py`, `/tmp/wq.py` | gdb raw-memory analysis scripts (ephemeral, recreate from this log if needed) |

## Test Results / Benchmarks

- entry-race phase A: pristine old kernel 3/3 hang; spec06 kernel
  same hang (pre-existing); zsmalloc phase A2 same hang; never-compress
  (100:100) clean pass; all four delay injections clean (race
  dissolved).
- spec06 gate boot (2G/2G, wm=0:20, gcp=20, KASAN): flip at "ram
  usage 20%", fill+read-back survive, CRC ok, mount line shows
  gc_percent=20.  Write 91.6 MB/s is the KASAN tax, not the number.

## Next Steps

1. Run the closing kprobe experiment: erl (`folio_lock folio=%di:u64`)
   + eru, stall, dump, count erl-vs-eru per folio address in the
   trace tail -- find the waited folio's last event.
2. Fix the bug per the fork's answer; verify with phase A + A2 +
   full phases, KASAN.
3. Restore spec 06: `git stash pop`, rebuild both kernels, full
   KASAN suite, then Phase C triangle rerun + torture matrix.
4. Rig cleanup: remove the init.sh debug block and test_entry_race
   phase selector (or keep the selector behind an ifdef-less env
   guard -- decide at cleanup).
5. Standing items unchanged: RFC assembly (spec 02 Phase 6), M-boot
   median+IQR, arm-3 rm/synchronous-free at 64M, workload-bed spec
   05 Phase 1.

---

## The kprobe experiment (2026-10-13)

Ran the closing experiment the log above sets up.  Result: **the
fork is closed on the essential question** -- the unlock DID happen
and its wake walk DID run; the wake was lost/skipped.  The bug lives
in the "wake never delivered" world, not the "missing unlock" world.

### What We Did

1. Rare-event kprobes (`eradd` on shmem_add_to_page_cache, `erdel`
   on shmem_delete_from_page_cache, `erun` on folio_unlock, all
   `folio=%di:u64`) + auto trace dump on stall (tracefs had to be
   mounted; `arg1` fetch failed, `%di:u64` works; a failed probe
   registration silently gated the event enable -- fixed).
2. The stall reproduced with these probes live (dump at ~30 s).
   gdb (monitor-stop + attach) pinned three waiters this run
   (pids 93/94/96, indexes 7/10/19), all the same signature:
   flags=0x12c, refcount=1 (their own filemap ref), mapping NULL,
   lock bit clear, wait entry correctly queued.
3. The trace dump holds the full lifecycle of the waited folios.
   For the index-7 folio: `eradd` (writer install) -> ~24 `erun`
   (readers' lock/read/unlock loops) -> `erun, eradd, erun, erdel,
   erun` (the writer's final cycle ending in compress_one's removal
   **followed by its unlock with its wake walk**).  The unlock's
   walk is the folio's LAST recorded event.  Yet the waiter for
   that folio slept on, correctly queued, bit clear.
4. Adding an `erlk` probe on __folio_lock (contended path only)
   dissolved the race again -- the sixth and final heisenbug
   observation; that probe would have named the last contended
   locker, which remains the one untraced event class.

### What We Learned

### Technical

- **Mechanism family confirmed**: `wake_page_function`'s exclusive
  handoff branch skips a waiter when the lock bit reads set at walk
  time (`return -1`), or the waiter enqueued after the last walk.
  Either way the invariant that saves mainline is "a future
  lock/unlock on this address will re-walk the queue" -- and
  **compress_one breaks it**: once `shmem_delete_from_page_cache`
  removes the folio, that address is no longer in the mapping, so no
  future lock/unlock is guaranteed (the page is freed and reused for
  non-pagecache duty -- exactly the observed waited-folio state:
  mapping NULL, stale flags, refcount 1).  Any waiter skipped in
  that final walk is abandoned forever.
- The trace also showed readers doing `eradd` (decompress installs)
  and same-address double-installs via the -EEXIST retry (freed
  folio immediately re-allocated -- normal LIFO buddy reuse, not the
  bug).
- Probe overhead at folio_lock/unlock granularity (every page op
  system-wide) is enough to dissolve this race; only rare-event
  probes (add/delete at op rate) preserve it.  __folio_lock (the
  contended-only path) was still too hot in this workload.

### What We Decided

- **Fix shape (covers both remaining sub-forks with one change)**:
  in `shmem_comp_compress_one`, once the folio has left the page
  cache, drain/rewake its lock waiters explicitly before returning
  -- e.g., after the removal, loop `wake_up_page`-style wake walks
  until no exclusive waiter remains (or convert the removal-side
  unlock to a non-skippable wake).  A later walk with the bit clear
  wakes the abandoned waiter in either sub-world.
- Verification: the fix must make phase A/A2 pass on the PRISTINE
  kernel (no probes) repeatedly -- the pristine kernel is the
  reproduction oracle; instrumented kernels prove nothing here.

### Next Steps (updated)

1. Implement the compress_one waiter-drain fix on the PRISTINE
   kernel; run entry-race phases A/A2/B/B2 in loop (10+ runs,
   pristine) to confirm the stall is gone; full KASAN suite.
2. Then restore spec 06 (`git stash pop`), rebuild, suite, Phase C
   triangle rerun + torture matrix.
3. Rig cleanup of the ERHUNT debug block + phase selector.
4. Optional later: re-run with erlk-only probes to name the last
   contended locker (mechanistic completeness, not needed for the
   fix).

---

## The fix attempt (2026-10-13)

Implemented the waiter drain on the pristine kernel.  Result: **partial**
-- strictly beneficial, committed (`2ca2601bd2e5`), residual mechanism
sharpened to a single unexplained state.

### What We Did

1. `wake_up_folio_waiters()` (exported from filemap.c): walks the
   folio's PG_locked wait queue until PG_waiters clears, bounded with
   a warning.  First a single unconditional wake; then upgraded to the
   loop after stats showed partial effect.
2. Drain sites: `shmem_comp_compress_one()` after the folio leaves the
   page cache; `shmem_get_folio_gfp()`'s mapping-mismatch exit (each
   departing stale-ref holder drains before dropping its ref).
3. ERHUNT probes removed from init.sh for verification runs (probe
   overhead dissolves the race -- six for six).

### Test Results (the pristine oracle, 10 runs each, no instrumentation)

| kernel | phase A (cells) | phase A2 (zsmalloc) |
|---|---|---|
| unpatched | 0/10 (deterministic in early 3/3 runs) | 0/10 early runs (no 10x baseline taken) |
| single wake after removal | 0/10 | 3/10 |
| drain loop (waiters flag) | 0/10 | 6/10 |
| + reader-side drain | 0/10 | 6/10 |

The zsmalloc improvement is real; the cells phase is untouched by any
variant.  (Build-to-build timing noise is acknowledged: the 3/10 vs
6/10 spread may partially reflect that.)

### The residual mechanism, sharpened

The stalled folios' flags (0x12c / 0x2c) have **PG_waiters (bit 7)
CLEAR while the wait entry is verified still linked in the hashed
bucket**.  Consequences:

- Every later `folio_unlock` takes the fast path (xor sees no waiters)
  and never walks -- nobody can ever collect that entry again.
- The drain-loop's guard (`folio_test_waiters`) reads the very flag
  that is wrongly clear, so the loop exits without walking -- the
  single-wake variant was actually the better collector.
- The ONLY `folio_clear_waiters` call site in all of mm is the
  wake-tail (`filemap.c`): `if (!waitqueue_active(q) ||
  !key.page_match) folio_clear_waiters(folio)`.  The anomalous state
  requires a walk for F that found the bucket non-empty but with NO
  matching entry for F (so page_match stayed 0) at a moment when F's
  waiter was out of the bucket -- the waiter's `goto repeat` gap
  (woken without DONE, re-test fails, re-enqueue) -- AND, per the
  code as read, the repeat path re-sets PG_waiters under q->lock, so
  static analysis still cannot close the loop.  One link between the
  observed state and the code remains unexplained.

### Next Steps (revised, precise)

1. Phase A still hangs 10/10 on the CURRENT (fixed) kernel with NO
   probes -- so gdb raw-state inspection works again: next session
   should read, on a stalled folio: PG_waiters, the waiter's
   `wait->flags` (WOKEN?), the bucket list integrity, and the
   `sysctl_page_lock_unfairness`-derived CUSTOM path involvement.
   The single unexplained fact to kill: "queued entry + clear
   PG_waiters".
2. Candidates once explained: a fix at the true clear site, or the
   general rule "any wake of a detached folio must walk regardless of
   PG_waiters" (make the drain unconditional, drop the flag guard).
3. Then: restore spec 06 (`git stash pop`), rebuild, suite, Phase C.
4. Rig cleanup of the debug block and phase selector.

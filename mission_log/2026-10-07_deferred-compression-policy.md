---
name: deferred-compression-policy
description: death-before-pop deferred compression -- the Stage 2 machinery, the watermark/size/merge policy matrices, and the default flip to 50:70 (roadmap addendum + sessions 14-20)
project: linux
previous: 2026-10-07_test-rig-maturation.md
---

# Mission Log: Deferred Compression -- Stage 2, the Policy Matrices, and the Default Flip

Part 7 of the tmpfs transparent-compression series, split verbatim from the
combined log `2026-10-05_tmpfs-transparent-compression.md` (kept until the
split is validated). Session numbers are preserved so cross-references keep
resolving.

Covers the Stage 2 deferral policy: the open-work roadmap recorded between
sessions 11 and 12, then sessions 14-20 (2026-10-08 - 2026-10-09) -- the
maintenance-pass machinery, the policy matrices across watermark/size/merge
axes (including the 384M correction), the configuration guidance, and the
default flip 0:0 -> 50:70 that completed the plan's landing sequence.

> **Note (2026-10-09):** the "What We Decided" / "What We'd Do
> Differently" sections of sessions 14-20 were NOT written
> contemporaneously.  They were reconstructed on 2026-10-09 from the
> pi session transcripts and the sessions' own text, when the linter
> started enforcing the template.  Content is grounded; in-the-moment
> hindsight is by definition missing.

## Roadmap addendum: the open-work expansion -- Stage 2 machinery and cellzram hardening (recorded 2026-10-07, between sessions 11 and 12)

Status of the tree at writing (verified by grep, not assumption): the
watermark fields exist in mm/shmem_compress.c, but there is no marker
list, no shmem_comp_maintain(), no stampede guard in mm/cell_store.c,
and no per-mount stat readers in mm/shmem.c. Both items below are open.

### #1: Stage 2 machinery -- death-before-pop aging, in detail

The design (plan, "Compression policy v2") is settled; what is
missing is the machinery. Stage 1 (sessions 8-9) landed the OPTION
surface with the default still 0:0, i.e. eager = today's behavior.
The write path consults the watermark only for the never sentinel
(100:100 -> no pool). Stage 2 makes the knob mean something.

What Stage 2 builds, in dependency order:

1. **Candidate markers.** In chill mode (usage < LOW) writes land
   uncompressed, and each eligible page gets a cheap marker (list_add
   under a spinlock) so the maintenance pass can find it without a
   full page-cache walk. Unlink and truncate purge the marker: the
   counter skipped_dead is the design's payoff -- transient files
   die unread, and death-before-pop means their bytes never pay a
   single compression cycle. This is the reason deferred compression
   beats "compress less eagerly": the cache of un-compressed
   candidates is self-pruning.

2. **shmem_comp_maintain() -- one pass per mount, three jobs in
   order:** (a) MANDATORY compression of eligible survivors --
   order-0, uptodate, unmapped, unpinned folios, clean pages
   included, compress_threshold IGNORED (any survivor >= 1 page is
   worth its RAM; this is the semantics split session 9 documented
   in the knob); (b) sparse-cell evacuation (the session-3 GC pass,
   utilization-based, batched); (c) hot/chill mode flips. Budgets
   alternate ~4:1 compression:GC so crypto and GC memcpy do not
   starve each other -- compression also RAISES cell density, which
   makes later GC cheaper, so survivors-first is not just fairness.

3. **Triggers:** the pool shrinker (memory pressure), gc_now
   (deterministic tests), and a periodic delayed_work armed only
   while candidates are pending.

4. **Mode flips with hysteresis, in the maintenance pass only.**
   The write path cost stays one READ_ONCE(comp->hot) -- never a
   percpu_counter compare. Hot mode (usage >= HIGH) compresses
   inline as today; the HIGH mark is a SOFT limit by design:
   compression releases size= accounting (compressed entries are
   accounted like swap pages; shmem_recalc_inode unaccounts blocks),
   so compressing hot data drops used_blocks back below the mark.
   Effective capacity for compressible data is size=/ratio.

5. **Default flip 0:0 -> 50:70**, then validation: the torture sweep
   becomes the policy matrix -- never / 0:0 / 50:70 / 50:100 against
   the session-7 eager baseline. Predictions already on paper:
   T=1 single-job throughput toward plain (~26 vs 17.1 MB/s -- writes
   stop paying inline crypto), skipped_dead absorbing transient
   churn, survivor ratio unchanged (~48% stored), bounded in-window
   Shmem overshoot. Watch stale_value_retries in every matrix row:
   it is the early-warning for the ID-recycling storms the panic
   layer would otherwise answer with a stopped machine.

Risks to design against while building: the stampede guard (session
3, still open) is the maintenance pass's companion -- a free burst
can link sparse cells faster than a lazy shrinker drains them, and
Stage 2's deferred mode makes candidates MORE numerous, not fewer.
Per-mount stat readers (comp_pages/comp_bytes, also still open) are
how the policy matrix observes hot/chill behavior per mount; without
them the matrix reads global stats only.

### #3: cellzram hardening -- what "phase 3" means, in detail

cellzram status (session 5): spec committed (8121630), device
implemented (~740 lines, CONFIG_TMPFS_COMP_BLKDEV), 77-assertion rig
test green, swap trial proven (MADV_PAGEOUT, ~127 MB swapped out,
full drain after signature punch). What remains is the spec's own
risk table coming due:

1. **Bounded internal ENOMEM retry on write.** Today a compression
   or store failure fails the bio with BLK_STS_IOERR (zram's
   pattern). tmpfs degrades gracefully under pressure; a block
   device cannot -- swap happens to retry because the failed page
   stays in the swap cache, but raw device users see EIO. The
   mitigation has a built-in synergy: cellzram's pools register the
   same shrinker/GC as tmpfs, so a bounded retry can give cell
   evacuation a chance to free the memory it needs, instead of
   failing a write that a few microseconds of GC would have
   absorbed. Bounded = a small retry count with backoff; this must
   NOT become an unbounded sleep in the bio path.

2. **Concurrent KASAN soak.** The 77-assertion suite exercises each
   path -- write/read round-trips, discard drain, reset, algo
   matrix, swap trial -- but never all at once on one device. The
   soak: multiple dd writers + gc_now + discard storm simultaneously
   on the KASAN kernel (SLUB_TINY off). The risk table's
   "reset races in-flight I/O" item is covered by design
   (blk_freeze_queue drains bios before the table erase) but this
   soak is the proof, not the argument. Per the session-10 KASAN
   scope rule: cell pages are buddy-backed, so the soak's real
   signal is the panic layer + quiescent verify, not the KASAN log.

3. **Swap slot-free -> discard dependence.** Swap reclamation of
   freed slots depends on the discard path being honoured. The
   session-5 trial proved swapout; the phase-3 test is the other
   half -- swap-slot free moving MemFree via discard. If the swap
   code needs SWP_DISCARDABLE hints wired for our entries, this is
   where they go.

4. **The v2 lever -- per-cell batched entries.** The metadata tax
   per written block (~64 B slab per 4 KiB = 16 MiB per GiB, same
   class as zram's per-page table) is the largest remaining RAM
   overhead once data is compressed: at 8 GiB of written blocks it
   is ~128 MiB of entries. One entry per N contiguous blocks (e.g.
   32 blocks = 128 KiB per entry) cuts it ~32x. Costs, all of which
   are design work before code: partial discard inside a batched
   region needs decompress-split (cut the discarded block's bytes
   out of the decompressed buffer, recompress the rest -- or
   refcount sub-ranges); the read path becomes
   offset-within-entry (machinery already exists from tmpfs merged
   groups -- the divisor chunking and head recovery); and a
   single-block write into a batched region invalidates the whole
   batch (rewrite the region or split the entry). The plan calls
   this "the single biggest optimization lever left; needs a
   design pass before v2" -- do not start it inside a hardening
   session.

5. **Low-priority but named:** comp_algorithm=none still allocates
   idle per-CPU acomp contexts (needs a pool_create variant);
   the "cellzram" device name is a placeholder (upstream picks);
   sysfs group placement (device pools appear under
   /sys/fs/tmpfs/compression/ -- rename the group
   compression-agnostically or give cellzram its own); global
   shmem_comp_global_stats mixes tmpfs and cellzram numbers
   (accepted for the prototype; per-device mm_stat is
   authoritative).

Sequencing note for future sessions: Stage 2 is the main line and
item 4 (batched entries) explicitly waits for a design pass -- the
honest split is Stage 2 machinery + its stampede/per-mount-stat
companions first, cellzram hardening 1-3 as the parallel block-dev
track, batched entries only once both are green and the design has
been written down. Upstream posting (tmpfs.rst docs, help-text
wording, sysfs-group naming) trails all of it.

---

## Session 14: Stage 2 machinery completed and gated (2026-10-08)

### What We Did

Finished Stage 2 (walk-based aging / death-before-pop) from the
half-done working tree (+408 uncommitted lines) to a landed, gated
kernel commit `c893c70b4ba8`, following the plan's landing sequence
(steps 2+3; the default stays 0:0 until the matrix validates 50:70).

What the WIP already had (read before writing a line -- it was ~90%):
struct fields, mounts list + mutex, fill_sb/put_super skeleton,
hot-flag consult, arm_aging, note_unmap, comp_stats lines, gc_now
extension, the compress_one(allow_clean) refactor, and a single-round
maintain walk.

What was missing, and what I added:

1. **Rounds + re-arm.** The WIP aged 128 pages once per trigger. The
   plan calls for up to 4 rounds x 128 pages with a cell-GC batch per
   round (~4:1 aging:GC), and re-arming the delayed work when the last
   round hit its budget. Extracted the walk into
   shmem_comp_age_round() (re-walks per round; already-compressed
   pages are xarray values, skipped cheaply).
2. **Teardown order amended.** The WIP's "cancel_sync before list_del"
   is safe only because nothing re-armed. With re-arm, a concurrent
   gc_now/pressure pass could re-arm a comp that put_super is about to
   free: cancel returns, list_del, kfree, then the re-armed work fires
   on freed memory. Fix: delist under the mutex FIRST, cancel outside
   the mutex after (the work takes the mutex, so holding it across the
   cancel would deadlock).
3. **Shrinker trigger.** The plan's "triggered by the shrinker" became
   shmem_comp_pressure_work: the shrinker only schedules (atomic-safe,
   coalescing); passes run in kworker context where GFP_KERNEL is
   honest -- the recorded deviation, and now the code comment says so.
   It arms ALL compressed mounts; the per-pass budget bounds the cost.
4. **The gate**: tests/test_watermark_policy.c + init.sh
   test.watermark=1 + run_qemu TEST_WATERMARK=1.

### What We Learned

- Reading the existing WIP before planning saved the session: the
  diff answered most design questions (locking contract, teardown
  rationale, walk structure) that would otherwise have been re-derived
  from the plan text -- and the plan-vs-WIP delta (re-arm, rounds,
  shrinker) fell out precisely because both were fresh.
- The re-arm race is the classic delayed_work teardown trap: "cancel
  then free" is only correct if NOBODY can re-schedule after your last
  check. Delist-then-cancel is the standard shape when a work item can
  re-arm itself or its siblings.
- Test-writing trap (caught by re-reading the draft before building):
  verify_pages() initially memcmp'd its read buffer against ITSELF --
  always equal. A verification helper that cannot fail is worse than
  none; the two-buffer (got/want) rewrite is the pattern.
- ENOENT on mount() in the guest means the MOUNTPOINT is missing, not
  the filesystem type: gates must mkdir their mountpoints (the fstools
  gate learned this in session 11; test_watermark_policy now mkdirs in
  try_mount() to be self-contained).
- gc_now running maintain() synchronously inside the sysfs write makes
  the test deterministic WITHOUT polling: stat deltas measured around
  one write(2) are exact, provided the 1s armed work cannot interleave
  -- measurement windows of microseconds against a 1s delay.
- Hysteresis flips are observable in TWO independent ways, which
  cross-validate: dmesg pr_info ("compression hot (usage 72%)") and
  behavior (inline compression resuming). The gate asserts behavior;
  the log line is free corroboration.
- The off-n build gate (CONFIG_TMPFS_COMPRESSION=n) keeps earning its
  keep: shmem.c hooks are ifdef'd, and one mechanical mistake there
  breaks every non-compression config -- cheap to check, expensive to
  discover downstream.

### What We Decided

- Stage 2 shape: candidate markers for chill-mode writes;
  shmem_comp_maintain() per mount -- mandatory survivor compression,
  then sparse-cell evacuation, then mode flips -- with ~4:1
  compression:GC budget alternation (survivors first also raises
  cell density, making later GC cheaper).
- Teardown is delist-then-cancel: delayed_work can re-arm itself or
  siblings, so "cancel then free" races the re-schedule.
- gc_now runs maintain() synchronously inside the sysfs write,
  making stat deltas exact without polling.

### What We'd Do Differently

- Re-read verification helpers before building on them:
  verify_pages() memcmp'd its read buffer against itself -- a
  helper that cannot fail is worse than none; two-buffer got/want
  is the pattern.
- mkdir gate mountpoints unconditionally: ENOENT on mount() names
  the mountpoint, not the fs, and session 11 had already taught
  this.

### Test Results

- test_watermark_policy: 7/7 PASS (chill writes uncompressed; gc_now
  aged delta >= 32; aged data round-trips; skipped_dead >= 16;
  sub-threshold pages aged despite threshold=1M; hot flip at 72%
  resumes inline compression; chill flip resumes skipped_dead).
- Entry-race gate (QEMU_CPUS=4): 0 underflows, stale_value_retries 0,
  quiescent verify PASS, exit 0.
- Default suite: exit 0 -- eager 0:0 behavior unchanged as designed.
- dmesg flips: "compression hot (usage 72%)" / "compression chill
  (usage 0%)" x2.
- CONFIG_TMPFS_COMPRESSION=n off-gate build: clean.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | rounds/re-arm in maintain, age_round helper, pressure work, teardown reorder |
| `mm/shmem.c`, `include/linux/shmem_compress.h` | (from WIP) hooks + fill_sb(sb) signature |
| `TMPFS_COMPRESSION_PLAN.md` | Status 2026-10-08: steps 2+3 landed |
| `~/tmpfs-compress-test/tests/test_watermark_policy.c` | the policy gate |
| `~/tmpfs-compress-test/guest/init.sh` | test.watermark=1 isolation gate |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_WATERMARK passthrough |

### Next Steps

- Step 4: policy matrix on the torture sweep -- never / 0:0 / 50:70 /
  50:100 vs the session-7 eager numbers (T=1 throughput, survivor
  ratio, Shmem overshoot, stale_value_retries).
- Step 5: default flip 0:0 -> 50:70 + suite + matrix re-run.
- Per-mount comp_stats readers (standing gap) matter more once mounts
  are chill by default; the matrix currently works around this with
  global deltas and one mount per boot.

---

## Session 15: the policy matrix -- deferral costs nothing here (2026-10-08)

### What We Did

Plan landing step 4: ran the Stage 2 policy matrix -- the torture
sweep once per GLOBAL watermark config (never100:100 / eager 0:0 /
50:70 / 50:100), driven through the Stage 1 sysfs default knob: the
torture configs mount WITHOUT compress_watermark=, so init.sh writing
/sys/fs/tmpfs/compression/compress_watermark from test.wm= on the
cmdline reaches every config without touching test_torture's table.

One boot per config (one mount alive at a time keeps the global
comp_stats deltas unambiguous), `scripts/matrix_watermark.sh` extracts
the T=1/T=64 comparison rows.

### Results (lz4_m1 rows, verify MB/s)

| watermark | T=1 | T=64 | live comp T=64 | comp RAM T=64 |
|-----------|-----|------|----------------|---------------|
| never 100:100 | 24.8 | 186.8 | 0 | 0 MB |
| eager 0:0 | 18.5 | 98.9 | 80314 | 166 MB |
| 50:70 | 24.7 | 188.4 | 298 | 1 MB |
| 50:100 | 24.7 | 195.3 | 240 | 1 MB |
| plain (same boots) | 26.2 | 178.0 | 0 | 0 MB |

- **T=1 prediction confirmed**: chill recovers ~80% of the eager->
  plain gap (24.7 vs 18.5 vs 26.2).  The plan predicted "toward
  plain (26 vs 17.1)"; landed at 24.7 vs 18.5.
- **T=64: chill ~= plain, ~2x eager** (188-195 vs 161-178 vs 98.9).
  Deferral costs nothing at high concurrency on a transient-heavy
  workload -- the workload's own rm phase is the dominant "reader",
  so pages that die before being read never pay a compression cycle.
- **RAM: the death-before-pop win, quantified**: 166 MB live
  compression RAM (eager) vs 1 MB (chill) at T=64.
- All four boots: 0 CRC/miss/short/ENOSPC/unexpected failures.
- 50:70 never flipped hot under this workload (live set far below
  70% of size=1G): here it behaves as pure deferral.  The flip path
  is covered by test_watermark_policy instead.

### What We Learned

- **Gate-design lesson (the session's real finding)**: the first
  matrix run failed 7 assertions per chill boot -- all "compression
  engaged (comp_pages > 0)" per-point checks.  comp_stats
  compressed_pages is a LIVE gauge, and a transient workload
  (untar -> verify -> rm) legitimately shows ZERO live compressed
  pages at any sampled instant in chill mode: pages either died
  before aging (death-before-pop working as designed) or were aged
  and then immediately decompressed by the verify read.  The
  assertion was measuring eager-mode engagement semantics.  Fix:
  per-CONFIG cumulative check (aged_pages grew across the config)
  for chill; negative invariant (no aging) for never; per-point
  gauge kept only for eager.  Rule: a deferral policy's engagement
  is a cumulative property, never an instantaneous one.
- A live gauge and a cumulative counter answer different questions;
  comp_stats has both (compressed_pages live, aged_pages cumulative)
  and the FIRST matrix run mixed them up.  Name the axis you are
  asserting on.
- The sysfs default knob (Stage 1) earned its keep: the entire
  matrix required zero mount-option plumbing -- one knob write in
  init.sh.
- Chill T=64 measured slightly ABOVE plain T=64 in the same boots
  (188-195 vs 161-178): plausible (smaller live footprint -> less
  shmem lock contention) but within the plain run-to-run spread
  (161-186 across boots); do not quote the exact number without
  stability runs.
- 3s windows x one boot per config are enough for policy validation
  (the deltas are 2x), not for publishing throughput numbers.

### What We Decided

- The matrix asserts engagement per-CONFIG on the CUMULATIVE
  aged_pages counter for chill (death-before-pop means the live
  gauge is legitimately zero); per-point live gauges stay only for
  eager, never keeps the no-aging negative invariant.
- 3s windows x one boot per config validate POLICY (the deltas are
  2x); they are not throughput publication numbers.

### What We'd Do Differently

- Name the assertion axis (live gauge vs cumulative counter) before
  writing the gate: the first run failed 7 assertions per chill
  boot, all measuring eager-mode engagement semantics.

### Test Results / Benchmarks

- Matrix run 1: never 226/0, eager 261/0, 50:70 254/7, 50:100
  254/7 (the 7 = per-point engagement, gate bug, see above).
- Matrix run 2 (corrected gate): never 232/0, eager 261/0, 50:70
  231/0, 50:100 231/0 -- all green, including never's no-aging
  negative invariant and chill's per-config aged_pages growth.

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/test_torture.c` | policy-aware engagement checks (point/config/none) |
| `~/tmpfs-compress-test/guest/init.sh` | test.wm= hook -> sysfs default knob |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_WM passthrough |
| `~/tmpfs-compress-test/scripts/matrix_watermark.sh` | the matrix runner |
| `TMPFS_COMPRESSION_PLAN.md` | step-4 status + numbers |

### Next Steps

- Step 5: default flip 0:0 -> 50:70, then suite + matrix re-run.
  The matrix says the flip is safe for transient-heavy workloads;
  the read-heavy recompression-thrash gap (#3 in the plan's known
  gaps) is the one to watch in the re-run.
- Stability runs (N boots per config) before quoting T=64 numbers
  anywhere.
- Per-mount comp_stats readers (standing gap) -- more valuable with
  every step toward chill-by-default.

---

## Session 16: one-boot policy matrix, and the size axis (2026-10-08)

### What We Did

Two rounds on the matrix, both driven by sharp user observations:

**Round 1 -- size, not watermark.** The first matrix (session 15)
ran size=1G mounts and50:70 never flipped: the band was never
exercised. Asked whether to move the watermark levels or the tmpfs
size: equivalent for reaching the band, but size is the axis that
varies in the field AND the only one probing the hard-cap boundary.
Wrote scripts/sweep_size.sh (20 boots:5 sizes x4 watermarks), started
it -- then killed it after the next observation.

**Round 2 -- one boot, not twenty.** compress_watermark= is a MOUNT
option; only the global sysfs knob was boot-global. So the whole
cross product became per-mount rows inside ONE torture boot:
test_torture.c generates16 policy rows (192M/384M/512M/1G x
never/0:0/50:70/50:100) under TORTURE_POLICY=1 (init.sh
test.policy=1), next to the6 classic backend rows. Engagement mode
is resolved per config from its own options (config_engage()), with
the sysfs knob only as fallback default. Win: one ~10-min boot
instead of ~2h, and23 mounts/22 umounts per boot -- every row
re-exercises fill_sb/put_super including the Stage 2 teardown
(delist + cancel_delayed_work_sync), which one-boot-per-combo never
touched after the first mount.

### The policy map (lz4_m1, T=64, untar failures)

| size | never | eager 0:0 | 50:70 | 50:100 |
|------|-------|-----------|-------|--------|
| 192M | death | death | death after flip@91% | death |
| 384M | fail (9) | marginal (4) | **CLEAN, flip@89%, 154 MB/s** | fail (9) |
| 512M | ok | ok | clean, flip@75%, 166 MB/s | ok |
| 1G | ok | ok | clean, no flip (31%), 187 MB/s | ok |

Flip attributions (dmesg, in-stream + final dmesg re-dump = x2 each):
50:70 goes hot at 192M (91%), 384M (89%), 512M (75%); never at 1G.

### What We Learned

- **The chill backlog inflates usage past the naive peak.** Eager's
  peak live set (~314 MB: comp_pages=80314 x 4K, NOT 674 MB --
  session 15 double-divided by the ratio) predicts flip boundaries
  below ~450 MB, yet 512M flips at 75%: deferred-but-not-yet-aged
  pages charge FULL size= blocks. The backlog is exactly the
  quantity the flip exists to drain -- now visible as the flip's
  usage number.
- **At the capacity cliff the adaptive policy wins.** At 384M/T=64:
  never and 50:100 (no inline relief) fail untars, eager is
  marginal, 50:70 flips hot and is CLEAN at 1.5x eager throughput.
  This is the strongest evidence yet for 50:70 as the default: it
  degrades gracefully exactly where eager is fragile, and behaves as
  pure deferral where there is headroom.
- **Deferral cannot fix capacity, only CPU**: 192M is below the live
  set under every policy, including after the flip.
- The enospc column was lying by omission: it counts mkdir() ENOSPC
  only; tar(1)'s write-path capacity deaths surface as untar_fail>0
  with enospc=0 (system() yields exit codes, not errnos). Documented
  in-code; the "clean vs fail" pattern across identical capacities
  was only interpretable after spotting this.
- fs_parse takes the FIRST value of a duplicated key: a config row
  embedding size= under run_config's unconditional "size=%s,%s" prep
  would silently keep the default size. The row format must own the
  whole options string (the test now skips the prep when options
  already carry size=).
- Background-launching a long sweep with nohup inside the agent
  harness works (survives the tool-call boundary), and mid-flight
  pkill of script+QEMU is clean. The earlier "head: cannot open"
  was a filesystem-sync race, same class as the write-tool one --
  sleep before reading freshly created files.

### What We Decided

- compress_watermark= is a MOUNT option (only the sysfs knob is
  boot-global); the cross product runs as 16 per-mount rows in ONE
  boot (23 mounts / 22 umounts) instead of ~20 boots / ~2h -- every
  row re-exercises fill_sb/put_super including the Stage 2
  teardown, which one-boot-per-combo never touched again.
- A config row embedding size= owns the whole options string:
  fs_parse keeps the FIRST value of a duplicated key, so the
  unconditional size= prep is skipped when the row carries one.

### What We'd Do Differently

- Check an option's scope (mount vs boot-global) before building a
  sweep around it; the 20-boot sweep script was written and
  deleted the same day.

### Test Results

- One-boot matrix: 882 passed / 11 failed (all11 = untar_fail at
  192M and the 384M cliff rows -- the map itself, not gate bugs).
- Post-storm: Shmem back to baseline across22 mount/umount cycles;
  zero live compressed pages; Stage 2 teardown survived22 cycles.

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/test_torture.c` | policy rows, per-config engage, size= ownership, enospc docs |
| `~/tmpfs-compress-test/guest/init.sh` | test.policy=1 -> TORTURE_POLICY=1 |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_POLICY passthrough |
| `~/tmpfs-compress-test/scripts/matrix_watermark.sh` | rewritten: one-boot runner + digest |
| `TMPFS_COMPRESSION_PLAN.md` | step-4 v2 status + the map |

### Next Steps

- Step 5: default flip 0:0 -> 50:70. Both matrices (deferral at 1G,
  cliff at 384M/512M) now back it.
- Classify untar failures as capacity deaths in-test (read
  used_blocks vs max after a failed untar) so the enospc column
  stops lying by omission.
- Per-mount comp_stats readers (standing gap) -- the one-boot matrix
  works around them with per-config deltas on global counters.

---

## Session 17: the watermark axis, and eating a correction (2026-10-08)

### What We Did

Swept watermark levels at fixed size, per the plan's remaining axis:
12 per-mount rows in ONE boot (test.torture TORTURE_WMSWEEP=1 ->
init.sh test.wmsweep=1): eager/never anchors, LOW sweep @HIGH=70
(0/25/50/70), HIGH sweep @LOW=50 (50/60/80/90/100), wide-band probe
25:90.  sweep_wm.sh [size] runs it; flip lines are attributed to
configs by serial position (dmesg re-dump excluded via the "Kernel
log (dmesg):" marker).

The sweep immediately exposed a gate bug: config_engage() mapped
0:70 to the eager per-point check, but 0:70 starts CHILL (fill_sb:
hot = (wm_high == 0)) and, with LOW=0, can only leave chill at usage
0.  Only the true eager sentinel (0:0) now gets the per-point check.

Ran the sweep at 384M twice, then at 512M.

### What We Learned

- **The correction (say it plainly): the session-16 "384M cliff"
  result was noise.** Run 2 at 384M reshuffled every failure:
  never clean->4, 25:70 4->0, 50:100 8->0, 25:90 7->12.  Mechanism:
  size=384M = 98304 blocks; compressed entries charge 1 block each
  (swap-style), so eager's comp_pages=82k + in-flight uncompressed
  ~= the cap.  EVERY mode at 384M/T=64 lives within +/-5% of the
  ceiling, and whether a tar(1) call trips ENOSPC is scheduling
  noise.  The v2 map's "50:70 survives where eager is marginal" was
  luck of the draw.  Only the robust rows survive as findings:
  192M is below the live set under every policy; >=512M has
  headroom; the 384M row must be discarded.
- **Rule: measure the policy axis only where capacity is not the
  dominant variable.** A knife-edge size confounds EVERY other axis;
  two identical boots differing in outcomes is the tell.  Cheap
  repeatability check (one rerun) before interpreting any
  cliff-shaped result -- the20-boot sweep we killed would have
  burned2h to produce the same confounded map.
- **512M sweep (466/0), the real watermark results**: throughput at
  T=64 tracks TIME SPENT HOT, not the level: eager (always hot)
  99.1 MB/s; earliest flip (50:60, flip@67%) 131.3; 50:50 144.9;
  50:70 165.2 (flip@72%); late flips (0:70, 25:70, flip@77%) ~178;
  never-flip levels (50:80/90/100, 25:90 -- usage peaks ~77%) cluster
  161-176.  Zero-width bands (50:50, 70:70) flap without breaking
  anything: the hysteresis mechanics survive their degenerate case.
- Flip usage numbers map the backlog: HIGH=50 -> flips at 60%,
  HIGH=60 -> 67%, HIGH=70 -> 72-77%.  The flip fires at the first
  maintain pass after crossing HIGH, so the reported usage is
  HIGH+backlog-at-cadence, a direct read on how far the deferred set
  runs ahead of the pass.
- T=1: deferral is free at EVERY level (26.2-27.8 vs eager 18.5 =
  plain speed).  The CPU bill only appears after a flip makes writes
  compress inline -- consistent with session 15's T=1 numbers, now
  shown to be level-independent.
- 50:70 at headroom sizes flips only under real pressure (72% peak)
  and defers otherwise.  No reason to move the default levels; step
  5 (the flip) stands.

### What We Decided

- The 384M row is DISCARDED: at 98304 blocks every mode lives
  within +/-5% of the cap at T=64, so untar outcomes are
  scheduling noise (identical boots reshuffle every failure); the
  watermark axis is measured at 512M instead (466/0).
- Rule: one cheap identical-boot rerun before interpreting any
  cliff-shaped result; two boots disagreeing is the tell.
- The real watermark map: T=64 throughput tracks TIME SPENT HOT,
  not the level; 50:70 sits in the fast cluster AND relieves
  pressure -- the reason it is the default candidate.

### What We'd Do Differently

- Run the repeatability check before narrating a cliff; the
  session-16 "50:70 wins the 384M cliff" reading was luck and cost
  the next session to retract.

### Test Results

- 384M run 1: 457+/11 untar fails (all T=64, scattered); run 2:
  different scatter (9 fails).  Never oopses; post-storm clean both
  runs (Shmem back to baseline).
- 512M: 466 passed / 0 failed; flips at 60/67/72/77% attributed to
  50:50/50:60/50:70/{0:70,25:70,70:70}; no flip at HIGH>=80.

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/test_torture.c` | wmsweep rows, config_engage (0,0)-only point check |
| `~/tmpfs-compress-test/guest/init.sh` | test.wmsweep=1 -> TORTURE_WMSWEEP=1 |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_WMSWEEP passthrough |
| `~/tmpfs-compress-test/scripts/sweep_wm.sh` | the sweep runner + flip attribution |
| `TMPFS_COMPRESSION_PLAN.md` | step-4 v3 status + correction |

### Next Steps

- Step 5: default flip 0:0 -> 50:70 (both axes now measured; levels
  unchanged).
- Repeat-boot discipline in the harness: any capacity-cliff-shaped
  result gets ONE rerun before it enters the plan.
- The untar-fail-as-capacity-death classification (standing) --
  would have flagged the 384M confound earlier.

---

## Session 18: merge x watermark, and the parallel-edit trap (2026-10-08)

### What We Did

Two additions to the torture gate, then the merge sweep:

1. Thread axis: test.tsw=1,2,...,128 (TORTURE_TSWEEP; MAX_THREADS
   64->256) -- prepared for oversubscription sweeps.  The T-curves
   from the existing 512M watermark sweep already answer most of it:
   eager plateaus at T=4 (inline lz4 saturates the 4 vCPUs: 74
   MB/s at T=4, 99 at T=64) while deferral keeps scaling (105 ->
   178).  Full curve table (512M, lz4_m1): eager 18.5/38.6/74.4/
   74.1/80.7/99.2/99.1 vs 50:70 26.3/51.2/99.1/107.1/119.5/140.9/
   165.2 and never 26.2/52.6/103.7/108.2/118.9/127.3/154.8
   (T=1..64).  Deferral is free at T=1-2 and its advantage GROWS
   with T.

2. Merge axis: TORTURE_MERGESWEEP=1 (test.mergesweep=1) --
   merge 1/2/4/8/16/32 x {eager 0:0, 50:70} + never anchor as
   per-mount rows at test.tsize= (sweep_merge_wm.sh).  Clean run
   534/0 at 512M:
   - Eager: engagement collapses with merge (live comp pages 78k ->
     512 from m1 to m32; munin's small files never fill groups at
     EOF), stored ratio of the compressing subset degrades 47.9% ->
     70.5%, throughput gains modestly (108 -> 140).  CPU
     amortization cannot pay back near-zero engagement.
   - 50:70: merge irrelevant (145-190 band, no trend, comp pages
     ~200-2000) -- the aging pass compresses SINGLES, bypassing
     groups by design.  50:70 beats eager at EVERY merge.
   - Conclusion: with deferral as the default, merge_pages belongs
     at 1; the knob is meaningful only for eager mounts with
     large-file workloads.

### What We Learned

- **THE PARALLEL-EDIT TRAP (the session's big process lesson).**
  The never-anchor row initially mounted with an EMPTY name and
  options (a real bug: `all[n++] = { mnames[n], mopts[n], 1 }` --
  unspecified evaluation order; the increment can sequence before
  the RHS reads n, pointing the row at the next, empty slot).  I
  fixed it with the k=n pattern -- but issued the `edit` tool call
  and the `gcc + repack + sweep` bash call IN THE SAME PARALLEL
  BLOCK.  The compiler read the pre-fix source.  The rerun showed
  the identical failure -- and I misread that as "the fix didn't
  work" and went down a multi-step detour: strings(1) on a binary
  where -O2 had folded snprintf into immediate stores (literal
  legitimately absent from .rodata!), objdump archaeology of glibc
  pthread internals after a bad sed range, a host repro that
  PROVED the logic correct (the strongest clue -- it compiled the
  CURRENT source separately), until first principles produced the
  answer: THREE runs with byte-identical results (534/7 every time)
  was the tell that NOTHING was changing.  Sequential rebuild +
  repack: 534/0, green.
- Rules extracted:
  1. Dependent tool calls (edit -> build) are SEQUENTIAL, always --
     the same trap fired twice this session (write+chmod earlier).
  2. Identical results across "different" runs means the experiment
     did not change, not that the system is deterministic.
  3. strings(1) proving a literal "missing" proves nothing about
     -O2 codegen: constant snprintf/strcpy get folded into
     immediates.  Disassemble or run; don't grep bytes.
  4. A standalone repro compiled from the CURRENT source is the
     fastest oracle when binary-vs-source provenance is in doubt --
     and it bisects the problem into "environment" vs "code" in one
     step.
- merge_pages and the policy model compose: write-time group
  dispatch vs aging-pass singles is now an OBSERVED asymmetry with
  numbers, not just a design note.

### What We Decided

- merge_pages stays default 1: under 50:70 the aging pass does
  singles, so write-time group dispatch is an eager/large-file
  niche; the cross (534/0) measured the asymmetry with numbers.
- The empty-anchor row bug is fixed with the k=n pattern; the
  all[n++] = { mnames[n], ... } form relies on unspecified
  evaluation order.

### What We'd Do Differently

- THE PARALLEL-EDIT TRAP: the `edit` and the `gcc + repack + sweep`
  went out in one parallel tool block; the compiler read pre-fix
  source.  Dependent tool calls are SEQUENTIAL, always -- the same
  class of trap bit twice in this session.
- Three byte-identical "different" runs (534/7) meant the
  experiment never changed, not that the system is deterministic;
  identical results across runs is the tell.
- strings(1) proving a literal absent proves nothing at -O2
  (constant folding); disassemble or run.
- A standalone repro compiled from the CURRENT source bisects
  binary-vs-source doubt in one step.

### Test Results

- sweep_merge_wm 512M: 534 passed / 0 failed (clean after the
  rebuild); earlier runs: 533/7 x3 (the anchor bug, all identical).
- T-curves extracted from the 512M watermark sweep (see above).

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/test_torture.c` | mergesweep rows, tsw override, anchor fix, MAX_THREADS 256 |
| `~/tmpfs-compress-test/guest/init.sh` | test.mergesweep=, test.tsw= hooks |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_MERGESWEEP, TEST_TSW passthrough |
| `~/tmpfs-compress-test/scripts/sweep_wm.sh` | watermark sweep runner + flip attribution |
| `~/tmpfs-compress-test/scripts/sweep_merge_wm.sh` | merge x watermark runner |
| `TMPFS_COMPRESSION_PLAN.md` | step-4 v4 status |

### Next Steps

- The oversubscription sweep is one boot away (test.tsw=64,128 at
  512M): does T=128 push usage past HIGH=80 and flip the
  never-flip levels?  Watch capacity: T=128 grows the live set and
  can push 512M back toward the knife edge -- check untar_fail.
- Step 5 (default flip) remains the next kernel change.
- Per-mount comp_stats readers (standing gap).

---

## Session 19: the analysis -- what config when (2026-10-08)

### What We Did

No new boots: synthesized the four measurement sessions (15-18, plus
1/7 backend work) into the standing "Configuration guidance" section
of TMPFS_COMPRESSION_PLAN.md -- a decision table + rules, each row
traceable to a measured matrix row.  This entry mirrors it with the
full reasoning trail.

### The findings, by decision

**1. The default: lz4 + compress_watermark=50:70 + headroomed size=.**
The only configuration that wins in all three regimes measured:
free at low concurrency (T=1-2: 26.2-26.3 MB/s = plain speed vs
eager 18.5, i.e. eager's -30% CPU bill vanishes), ~2x eager at high
concurrency (T=64: 165-190 vs 99), and 1 MB vs 166 MB live
compression RAM (the death-before-pop win: transient pages die
before paying a compression cycle -- skipped_dead is the metric).
At the capacity cliff (384M) it was the most robust policy
observed: flips hot and relieves inline where never/50:100 died.

**2. merge_pages: eager/large-file niche only.** The cross
(merge 1..32 x {eager, 50:70}, 534/0) showed the asymmetry with
numbers: write-time group dispatch vs aging-pass SINGLES.  Eager:
engagement collapses 78k -> 512 live comp pages (m1 -> m32; small
files never fill a group at EOF), the compressing subset's ratio
degrades 47.9% -> 70.5%, and CPU amortization buys only 108 -> 140.
Under 50:70: statistically irrelevant (145-190 band, no trend).
Default merge=1 stands; group-aware aging is a follow-up reserved
for zstd + long-lived large files (lz4's ratio is merge-insensitive
per session 1).

**3. zstd: ratio-bound, large-file, long-lived only.** +10-12%
ratio (36-38% vs ~48% stored) but 12.4 vs 18.5 MB/s at T=1, and
under deferral its write-side edge disappears entirely -- the aging
path does the work (as singles, in kworker, off the workload's
critical path, which is a feature for throughput but means zstd
buys nothing but ratio under the default policy).

**4. compress_threshold: leave at 64K.** Write-time gate only; the
aging pass ignores it by design (grate #2 dissolved -- sub-threshold
survivors age, asserted by test_watermark_policy).  Under the
default policy the knob is decorative; under eager it only tunes
how much inline CPU small files cost.

**5. size= is the one hard constraint.** Within ~10% of peak live
set EVERY policy dies stochastically (384M: failures reshuffled
across identical boots; compressed entries charge ~1 block each,
swap-style, so compression buys RAM-pressure relief, not size=
capacity).  Rule: size= >= 1.5-2x peak live set, and never tune or
measure policy inside the noise band.  The 192M row (death under
all policies) and >=512M rows (clean under all policies) are the
only robust capacity conclusions; the 384M row was discarded.

**6. Throughput tracks time-spent-hot, not the level.** HIGH sweep
at 512M: early flip (50:60, flip@67%) slowest at 131; 50:50 144.9;
50:70 (flip@72%) 165.2; late flips (0:70, 25:70, flip@77%) ~178;
never-flip levels (50:80/90/100, 25:90 -- usage peaks ~77%) cluster
161-176 = deferral.  50:70 sits in the fast cluster AND relieves
pressure -- the reason it is the default candidate, not 0:70 (a
one-way switch: LOW=0 can only leave hot at usage 0) or 50:60
(relieves too early, pays too much CPU).

**7. Robustness findings that de-risk the flip.** Degenerate bands
(50:50, 70:70 flapping) are stable; 22+ fill_sb/put_super cycles
per boot including Stage 2 teardown are clean; entry-race gate 0
underflows / stale_value_retries 0; eager plateaus at vCPU count
(T=4 of 4) while deferral scales -- the eager penalty GROWS with
concurrency, so the flip's benefit is workload-dependent in the
right direction.

### What We Learned

- Synthesis is a session-shaped deliverable: the decision table
  forced three restatements (the 384M caveat moved from "finding"
  to "rule"; merge demoted from "knob" to "niche"; zstd qualified)
  that no single matrix run would have produced.
- Every recommendation row traces to a specific matrix row; where
  the data was noise (384M) or single-boot (the 145-190 band), the
  table says so instead of smoothing it.
- The guidance section in the PLAN doc (not the mission log) is the
  right home for durable recommendations; this entry is the
  provenance.

### What We Decided

- Durable recommendations live in TMPFS_COMPRESSION_PLAN.md's
  "Configuration guidance" section; this log is the provenance
  trail (every row traces to the matrix that produced it).
- The decision set: lz4 + 50:70 + headroomed size= (>= 1.5-2x peak
  live set) is the default answer; merge/zstd/threshold are eager
  or large-file niches; size= is the one hard constraint;
  throughput tracks time-spent-hot, not the level.

### What We'd Do Differently

- Synthesis is a session-shaped deliverable: schedule it after the
  matrices instead of squeezing it in; it forced three
  restatements (384M caveat -> rule, merge demoted to niche, zstd
  qualified) that no single matrix run would have produced.

### Test Results

No new runs; all numbers cited come from:
- session 15/17: watermark sweeps 1G and 512M
- session 16: size x watermark one-boot matrix (882/11 -> corrected
  interpretation in 17)
- session 18: merge x watermark (534/0) + T-curves
- session 14: test_watermark_policy (7/7), entry-race (0 underflows)
- sessions 1/7: backend parity, compressor behavior

### Files Changed

| File | Purpose |
|------|---------|
| `TMPFS_COMPRESSION_PLAN.md` | "Configuration guidance -- what config when" section |

### Next Steps

- Step 5: default flip 0:0 -> 50:70, then suite + the guidance
  tables re-validated under the new default.
- Read-heavy re-compression thrash (gap #3) remains the one open
  behavioral question the matrices did not exercise.
- Per-mount comp_stats readers: every "when" row above currently
  leans on global counters or dmesg attribution; per-mount would
  make the guidance operational rather than analytical.

---

## Session 20: the default flip (2026-10-09)

### What We Did

Landed plan step 5: the built-in default compress_watermark flipped
0:0 (eager) ->50:70 (mm/shmem_compress.c static initializer; the
sysfs knob initializes from it).  Explicit mount
compress_watermark=0:0 still restores eager via watermark_seen, so
the flip is opt-out, not mandatory.

The flip's blast radius was the TEST SUITE, not the kernel: every
mount without an explicit watermark now starts chill, which breaks
any test asserting WRITE-TIME compression.  Policy: mechanism tests
pin eager explicitly (9 test files + the fstools gate in init.sh +
test_mount_sem's write-time case); the policy stays covered by the
dedicated gates.  test_torture's classic rows deliberately keep NO
watermark -- they follow the global default and thereby validate
inheritance (engage=config(aged) auto-resolution;231/0).

### What We Learned

- The parallel-edit trap fired a THIRD time (edit + build in one
  tool block; the rebuilt binary lacked the source's fix -- caught
  this time in seconds by grepping the binary for the new string
  before booting).  The rule is now reflex: any edit->build->run
  chain is sequential, and binary provenance is verified BEFORE a
 10-min boot, not after a failure.
- The engage auto-resolution design paid for itself at the flip:
  config_engage() reading the global knob as fallback meant the
  classic torture rows needed zero code changes to stay valid under
  the new default -- and became inheritance tests for free.
- Flipping a default is cheap in the kernel (a static initializer)
  and expensive in the tests (10 files) -- the test suite had
  encoded the default in23 mount option strings.  Worth
  remembering: when a default lives implicitly in many call sites,
  the explicit-pin convention for MECHANISM tests is what keeps the
  default flippable later.

### What We Decided

- The built-in default flips 0:0 -> 50:70 as a static initializer;
  explicit compress_watermark=0:0 restores eager via
  watermark_seen (opt-out, not mandatory), 100:100 stays the never
  sentinel.
- Mechanism tests pin eager explicitly (9 test files + the fstools
  gate + test_mount_sem's write-time case); classic torture rows
  stay deliberately unpinned and thereby validate default
  inheritance (engage=config(aged) auto-resolution).

### What We'd Do Differently

- Keep the explicit-pin convention for MECHANISM tests: the flip's
  blast radius was the TEST SUITE (23 mount option strings had
  encoded the default), not the kernel -- when a default lives
  implicitly in many call sites, pinning it explicitly is what
  keeps it flippable later.
- Verify binary provenance (grep the new string in the built
  kernel) BEFORE every long boot, never after a failure: the
  parallel-edit trap fired a third time and the pre-boot strings
  check caught it in seconds.

### Test Results (all under the new default)

- Full suite: exit0 (9 mount lines show inherited watermark=50:70)
- test_watermark_policy:7/7 ALL PASSED
- entry-race (QEMU_CPUS=4): 0 underflows, quiescent verify PASS,
  stale_value_retries0
- torture default boot:231 passed /0 failed (classic rows =
  inheritance validation)

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | built-in default wm_low/high =50/70 + rationale comment |
| `~/tmpfs-compress-test/tests/*.c` (10 files) | mechanism tests pin compress_watermark=0:0 |
| `~/tmpfs-compress-test/guest/init.sh` | fstools gate explicit eager |
| `TMPFS_COMPRESSION_PLAN.md` | step-5 status; landing sequence complete |

### Next Steps

The plan's landing sequence is COMPLETE.  Standing items:
- read-heavy re-compression thrash (gap#3) -- now the default path,
  worth a dedicated gate before upstream posting
- group-aware aging (zstd + large files only, per the guidance)
- per-mount comp_stats readers (the guidance's ops story needs them)
- upstream posting: tmpfs.rst, help-text wording, sysfs naming
- re-run the policy matrix under the flipped default if any sweep
  mixes explicit and inherited mounts (the wmsweep rows are all
  explicit; classic rows now inherit)

---


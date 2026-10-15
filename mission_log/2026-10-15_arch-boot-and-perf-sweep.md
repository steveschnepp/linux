---
name: arch-boot-and-perf-sweep
description: bootable kernels + a self-contained static guest for the cross-arch QEMU sweep; x86 bisect finds two test bugs; arm64 perf mode spins in a folio_lruvec lock loop -- gdb backtrace pending
project: linux
previous: 2026-10-15_32bit-packing-and-table-map.md
---

# Mission Log: Cross-Arch Boot + Synthetic Perf Sweep (QEMU)

Part 47 of the tmpfs transparent-compression series.  Session
(2026-10-15), continued.  New topic, new file: the compile matrix
(sessions 55-58) is done; this phase RUNS the kernels.  User decisions
at the start: (1) **synthetic TCG perf is acceptable -- relative
numbers only**, same-host comparable, absolute values discounted;
(2) correctness tests wanted on the other arches too; (3) toolchains
and emulators installed by the user as needed (arm-linux-gnueabihf,
mips/mips64, qemu-system-{arm,mips,riscv}, later gdb-multiarch).

## What We Did

### 1. The guest: no busybox, one static C program

The existing harness (`~/tmpfs-compress-test`) is x86-only on the
guest side (x86-64 busybox, hardcoded qemu-system-x86_64 in
run_qemu.sh, in-tree kernel builds).  Rather than cross-build busybox
(= another toolchain project), wrote `tests/arch_init.c`: a single
static C program that IS PID 1 -- mounts proc/sysfs, parses
/proc/cmdline (`test.mode=correct|perf|clock`, `test.size=N`), runs
either a correctness battery (zsmalloc/cell/merge/plain mounts, write
8 MiB of deterministic ~2:1-lz4 pattern data, streaming-FNV readback
verify, truncate+rewrite) or a timed fill/readback perf sweep, prints
machine-readable `CHK`/`PERF`/`GUEST_RESULT` lines, powers off.
Self-timing: every print carries seconds-since-boot.  A watchdog
pthread dumps /proc/self/task/*/stack after 20s.  Cross-compiles
static for x86_64/aarch64/armhf/mips/mips64/riscv64 with the distro
cross gccs.

Runner `scripts/run_arch.sh <sub> <mode> [MiB]` boots each config
under TCG (`-accel tcg,thread=multi`, 1 vCPU for determinism),
per-arch machine/cpu/console wiring (arm64/arm32 virt+ttyAMA0,
riscv64 virt+OpenSBI+ttyS0, mips malta+ttyS0), timeout, and greps
the one authoritative GUEST_RESULT line.
`scripts/pack_initramfs.sh` packs a single-/init cpio per config.
Kernel builder `out/build_arch_kernel.sh` + `out/sweep_build_all.sh`:
6 full bootable kernels (arm64 4K/16K/64K, arm32 multi_v7,
riscv64, mips malta BE), each with feature-symbol + axis verification
grep-gates (TMPFS_COMPRESSION=y, ENDIAN evidence).

### 2. Kernel-build obstacles (all pre-existing, none ours)

- **arm32 plain `defconfig` has no ARCH_VIRT** -- QEMU virt machine
  needs multi_v8/multi_v7_defconfig.  Caught before the build ran.
- **VT=y + UNICODE=n does not compile at tree tip**: `drivers/tty/vt/
  ucs_width_table.h` references `struct ucs_interval16/32` which are
  defined NOWHERE in the tree; every one of our defconfigs hit it.
  `config VT` is `default y` with an EXPERT-gated prompt, so
  `--disable CONFIG_VT` is silently reverted by olddefconfig.  Fix in
  the sweep configs: `--enable CONFIG_EXPERT` + disable VT (guests
  are serial-only).  NOT fixed in the tree -- separate issue.
- **x86_64 bzImage cannot build at tip**: `X86_FEATURE_UP` was
  removed from cpufeatures.h (marked "free:") but capflags.c still
  references it -> mkcpustr fails for ANY x86_64 config.  Abandoned
  the x86-KVM bisect for this reason (see below for the better
  experiment that replaced it).
- **stale `arch/x86/realmode/rm/pasyms.h` in the SOURCE tree**
  shadowed the freshly generated one in an O= build (session-45
  class AGAIN): realmode.elf failed with undefined `pa_wakeup_*`.
  Moved aside to out/srcartifacts/.  `git clean` stays forbidden.
- mips: `ARCH=riscv` not `riscv64`; and **a BE cross toolchain does
  not make a BE kernel** -- the endian choice lives in the kernel
  config (malta and decstation_64 defconfigs are LE boards).  The
  gate script got an `ENDIAN=be` knob + a hard verify step.
- Parallel-build race: `arch/x86/boot/mkcpustr` compiles while the
  generated `cpufeaturemasks.h` is still being written (serial works,
  parallel intermittently fails).  Worked around; logged, not root-
  caused.

### 3. x86 bisect of the new guest: TWO test bugs, not kernel bugs

Ran the guest on the existing x86_64 harness kernel under KVM (fast)
to separate "guest bug" from "kernel/arm bug":

- **All readbacks MISMATCHed, plain tmpfs included** -> my test bug:
  the checksum XOR-combined per-chunk FNV digests, and the write side
  chunked 64K while the read side chunked 2M; FNV is not XOR-
  decomposable across chunkings, so the two sides could never match.
  Fixed with a streaming FNV (state carried across calls).  36/36
  PASS after the fix.
- **test.mode=perf silently ran the correct battery**: cmdline_val()
  returned pointers into ONE shared static buffer; the second lookup
  (test.size) overwrote the first lookup's NUL terminator, so mode
  read "perf test.size=64".  Two fixes: copy the value into a
  separate buffer, then snapshot mode before the next lookup.
- Perf numbers on x86/KVM for reference: fill 458 MB/s plain vs
  48 MB/s compressed; readback 229 plain, 56 zsmalloc, 121 cell.

### 4. The arm64 perf-mode spin (OPEN -- current focus)

Correct mode on arm64-4k TCG: **writes complete** (8/8 files, ~0.3
guest-seconds), then readback produces nothing before the host
timeout.  Perf mode: hangs before the first progress line.

Ruled out, with evidence:
- **TCG slowness / broken guest clock** -- WRONG theory, retracted.
  In-guest clock probe (test.mode=clock): 300M-iteration busywait
  reads 1.49s, usleep(200ms) reads 0.202s; whole boot+run is ~4s
  host.  The kernel-vs-userspace timestamp gap was an EPOCH offset
  (t_boot taken after ~2.7s of boot), not a rate error.
- **The -EEXIST/-ENOENT retry loop in shmem_get_folio_gfp** --
  instrumented the loop and both decompress_folio failure exits with
  rate-limited pr_err("SWEEPDBG..."): zero lines.  Not the loop.
- **hung-task/softlockup detectors** -- configured and built in,
  never fire (they need the scheduler to run; see below).

Live evidence (QEMU monitor sampling, nokaslr):
- PC bounces between `_raw_spin_lock_irqsave`, `_raw_spin_unlock_irqrestore`
  with LR = `folio_lruvec_lock_irqsave+0x8c/+0x114`.  ~26/30 samples
  in the lock/unlock pair -> a LOOP that repeatedly lruvec-locks, not
  a single stuck lock (a stuck lock would park the PC, and interrupts
  DO get handled -- one sample in gic_handle_irq).
- The watchdog pthread never prints -> the main thread never reaches
  a scheduling point, so the spinner is inside one syscall, in a
  kernel loop with preemption off or a spinlock loop.
- Hypothesis (unconfirmed): a corrupted/exploding folio_batch drain
  (each iteration locks the lruvec, batch count never empties) --
  matches lock/unlock oscillation + no sched point.  Which code path
  corrupts it under the compressed-tmpfs write/read flow is the open
  question.  Note correct-mode writes pass -- so the trigger is
  narrower than "any compressed write".

**Next: gdb-multiarch is now installed (user) -> attach to the
gdbstub (-s), full backtrace + stack walk of the spinning task.
That is the immediate next action.**

### 5. Log hygiene note

The user had to remind me to log during the debugging stretch.  The
skill says log at each meaningful action, not at session end -- the
retracted theories (TCG clock, retry loop) were meaningful and should
have been logged as they were disproved, not batched.

## What We Learned

### Technical

- **A compile matrix is not a boot matrix.**  All 6 configs built
  0-warning and 3 of them still had never executed a single
  instruction of the feature until this session.  First boot of the
  pointer-encoding kernel immediately surfaced a runtime spin that no
  amount of cross-compiling could have hinted at.
- **QEMU TCG on this host is fast enough for synthetic relative
  numbers** (~4s boot+trivial-run for arm64 virt, lz4 compress
  inline at tens-of-MB/s guest-equivalent).  The earlier "TCG is too
  slow" worry was based on the misdiagnosed hang, not on measurement.
  Rule: measure the emulator before designing around its speed.
- **QEMU monitor `info registers` sampling is a usable poor-man's
  profiler** when guest gdb is unavailable: PC+LR pairs, resolved
  against System.map with nokaslr, distinguish "parked on one lock"
  (constant PC) from "lock/unlock loop" (oscillating PC, LR in the
  lock wrapper) in seconds.
- **A guest watchdog THREAD cannot fire when the main thread spins
  in-kernel on a 1-vCPU guest** -- no scheduling point, so sleep()
  never returns.  The stack-dump watchdog needs either smp>=2 with
  the spinner pinned elsewhere, or an NMI/sysrq path.  This is why
  the watchdog produced nothing and why gdb-multiarch (host-side) is
  the right tool.
- **Epoch offset != rate error.**  Comparing kernel printk timestamps
  against guest CLOCK_MONOTONIC only means something after accounting
  for when t_boot was taken.  The clock probe pattern (fixed
  busywork + usleep, compare guest delta to host wall time) settles
  it in one run; without it I burned two wrong theories on the
  symptom.

### Process

- **Validating the test on the fastest platform first (x86/KVM) is
  what separated test bugs from kernel bugs.**  Both test bugs
  (checksum chunking, cmdline buffer aliasing) would have been
  "kernel bugs" had I gone straight to the slowest, least
  instrumentable target.
- **Instrumentation beats speculation, but pick the instrument that
  can actually fire**: the SWEEPDBG retry-loop probes were the right
  idea and cleanly ruled the loop out; the hung-task detectors could
  not fire in a no-sched-point spin and cost a detour.
- **pkill -f self-match is a recurring footgun**: three times this
  session a `pkill`/`pgrep -f` pattern matched my own tool shell
  (the pattern string is in its cmdline) and killed the command
  mid-flight, once before an edit landed.  Use pgrep -x with the
  15-char comm, or PID lists from ps, never -f with a self-matching
  string.
- **Assert-then-write python for all bulk edits**, anchored on
  definitions (trailing `{`) not bare signatures -- forward
  declarations silently match signatures and one such match deleted
  3600 lines earlier today (session 56).  Still holding.

## What We Decided

- **Synthetic TCG perf is in scope, relative-only, same-host.**
  (User call, overriding the standing "perf on real silicon" rule for
  this specific question: relative page-size effects on one emulator.)
  The real-silicon rule still governs every number that goes in the
  series' results.
- **Guest = one static C PID 1 per arch**, no busybox cross-build.
  Rejected: cross-building busybox (toolchain project), using the
  x86-only harness guest (wrong arch), python/initramfs hacks.
- **The x86-KVM bisect was abandoned** (X86_FEATURE_UP blocks the
  x86 build at tip) and **replaced by the better experiment**: rebuild
  arm64-4k with lockup detectors + SWEEPDBG probes + QEMU monitor
  sampling, all of which are in place now.
- **Sweep kernels carry their axis evidence** (page size, endian,
 64BIT greps in the builder) -- the mips BE near-miss (LE defconfig
  with a BE toolchain) proved a green build proves nothing about the
  axis it claims to test.

## Rules Added

- **Measure the emulator before designing around its speed.**  One
  busywait+usleep probe; never infer emulator speed from a workload
  that might itself be hung.
- **A guest-side watchdog must not depend on the thread it watches.**
  1-vCPU + in-kernel spin = no scheduling = watchdog dead.  Host-side
  (gdbstub/monitor) is the only reliable observer.
- **Never `pkill -f`/`pgrep -f` a pattern that appears in your own
  command line.**  Use `pgrep -x` (15-char comm) or explicit PIDs.
- **Validate new guests/tests on the fastest, most instrumentable
  platform first** (here: x86/KVM with the old kernel), then move to
  the slow target.  It converts "possible kernel bug" into "test bug"
  in one run.

## Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/arch_init.c` (new) | self-contained static PID-1 guest: correct/perf/clock modes, streaming-FNV verify, timestamps, stack-dump watchdog thread |
| `~/tmpfs-compress-test/scripts/run_arch.sh` (new) | per-arch QEMU TCG runner with machine/cpu/console wiring + GUEST_RESULT protocol |
| `~/tmpfs-compress-test/scripts/pack_initramfs.sh` (new) | single-/init cpio packer per config |
| `~/linux/out/build_arch_kernel.sh` (new) | bootable-kernel builder with feature+axis grep gates (EXPERT/VT workaround, ENDIAN=be knob) |
| `~/linux/out/sweep_build_all.sh` (new) | sequential 6-kernel build driver (arm32 -> multi_v7_defconfig) |
| `~/linux/out/build_x86_kernel.sh` (new) | x86-64 kernel builder (build currently blocked by the X86_FEATURE_UP tree bug) |
| `~/linux/mm/shmem.c`, `mm/shmem_compress.c` (instrumented, UNCOMMITTED) | SWEEPDBG rate-limited probes in the decompress retry loop + failure exits -- KEEP until the spin is root-caused, then revert |
| `~/linux/out/srcartifacts/pasyms.h.stale` | stale source-tree realmode artifact moved aside (session-45 class) |

## Test Results / Benchmarks

- Compile matrix (session 58): 9 configs, 0 err/0 warn.  Unchanged.
- x86_64/KVM with the OLD 6.18 kernel: correct battery 36/36 PASS;
  perf 64 MiB: fill 458 plain / 47.8 compressed / 48.6 cell MB/s,
  read 229 plain / 56 zsmalloc / 121 cell MB/s (KVM, sanity only).
- arm64-4k TCG correct mode: writes complete, readback spins
  (GUEST_RESULT never printed).  arm64-4k TCG perf mode: spins before
  first progress line.  Clock probe: PASS (TCG fine).
- riscv64 / arm32 / mips32-BE: kernels + initramfs READY, correctness
  runs NOT yet started (blocked behind the arm64 spin).

## What We'd Do Differently

- **Log at each disproved theory, not after the user asks.**  Two
  wrong theories (broken TCG clock, retry loop) lived only in shell
  history until this entry; a future session re-reading the log would
  have re-run both experiments.
- **Attach gdbstub (`-s`) from the FIRST hang reproduction.**  I had
  QEMU monitor sampling, SWEEPDBG probes, lockup detectors and a
  guest watchdog before reaching for the tool that answers the
  question in one command.  gdb-multiarch was installed one message
  later than it could have been used.
- **Give the guest per-FILE progress from the start.**  The perf
  path's hang point (fill vs read, file N) was unknowable from the
  output because progress printed every 4 files; correct mode's
  per-file prints are what made "writes complete, readback spins"
  visible at all.
- **The x86-KVM bisect should have been planned as the FIRST step of
  the runtime phase**, not reached after the slow target failed --
  and its blocker (X86_FEATURE_UP) should have been found by trying
  the x86 build once at planning time, not mid-debug.

## Next Steps

1. **gdb-multiarch backtrace of the arm64 spin** (installed, not yet
   used): QEMU `-s`, attach, `bt` on the vCPU task, resolve the
   caller of folio_lruvec_lock_irqsave, find the loop.  Either fix a
   real kernel bug (highest value: this is the pointer-encoding
   kernel's first-ever runtime) or identify a TCG/config artifact.
2. Revert the SWEEPDBG instrumentation once root-caused (it is
   uncommitted; the tree must not ship with pr_err probes).
3. Correctness runs on riscv64, arm32, mips32-BE (kernels ready).
4. The relative perf sweep arm64 4K/16K/64K once (2) is clean.
5. Standing backlog unchanged (plateau, CPU-freq, workload bed, RFC
   assembly, H618 re-measure, group-decompress/truncate interleaving).

---

## Session 60: gdb Backtrace of the Arm64 Spin (2026-10-15)

### What We Did (so far)

gdb-multiarch attached to the QEMU gdbstub (`-s`, guest booted with
`nokaslr` so System.map addresses match).  The unwinder initially
reported `folio_batch_move_lru` 30x at identical addresses -- a FALSE
recursion (one bad frame record poisons the chain).  Walking the
frame-record chain manually (`x/gx $fp` hop by hop, resolving each
saved LR against System.map) produced the real stack:

```
_raw_spin_lock_irqsave
folio_lruvec_lock_irqsave+0x8c
folio_batch_move_lru+0xec          (LR-bounded; the 30x repeat is unwind noise)
__folio_batch_add_and_move+0x7c
folio_add_lru+0x28
shmem_decompress_folio+0x1c8
shmem_get_folio_gfp+0x6e0
shmem_write_begin+0x78             <-- the WRITE path, not the read path!
generic_perform_write+0xb4
```

**The decompress is triggered by the WRITE path** (`write_begin`
finding `entries[index]` set and decompressing into the page cache),
not by the readback I originally suspected.  Correct-mode writes DO
complete, so the trigger is narrower than "any compressed write".

One register snapshot read `x21` (=fbatch in folio_batch_move_lru,
callee-saved, stable across the loop) as `0xffff8000828d0db0` =
exactly `<node_data>` -- a bogus batch pointer whose first "count"
byte would be 0xc0 (192).  If real, the drain iterates garbage
"folios" and `folio_lruvec_lock_irqsave()` on a wild lruvec pointer
spins on a lock address that never clears.  NOT YET CONFIRMED (one
snapshot; needs a frame-slot read, not a live register).

A breakpoint on `shmem_decompress_folio` fired exactly ONCE across
the whole hang -- consistent with the vCPU being stuck INSIDE that
one call (folio_add_lru drain), not looping above it.  Batch-mode
gdb `continue` racing the stop made entry-argument capture flaky;
next attempt should use a commands-block printf or `-ex continue`
after `interrupt`.

### Open questions (next actions)

1. Confirm the fbatch pointer from the SAVED x21 slot in the
   folio_batch_move_lru frame (not the live register), and read
   fbatch->count.
2. Read folio_add_lru / __folio_batch_add_and_move in THIS tree --
   what fbatch does folio_add_lru pass, and what could corrupt it.
3. Read shmem_write_end's compress index (`pos >> PAGE_SHIFT`): if
   pos has already advanced past the written range, write_end
   compresses the NEXT page, whose write_begin then decompresses it
   -- the write path ping-ponging through our two-table mapping.
   This is the leading hypothesis for a REAL bug in our feature's
   first-ever runtime.

### What We Learned (this session)

- **gdb `bt` recursion on identical frames is usually unwind noise**,
  not real recursion.  Manual fp-chain walking (x/gx on each saved
  fp, resolve each lr) found the true 9-frame chain where bt gave a
  fake 30-frame one.
- **The LR of each frame is the ground truth**: LR
  `folio_lruvec_lock_irqsave+0x8c` inside the lock wrapper identified
  the caller before any frame was walked.
- **A one-shot register snapshot can lie** (x21 read after minutes of
  spinning may not be the loop-stable value you think; callee-saved
  helps but only a frame-slot read at a known fp offset is proof).
- **Where you think the hang is (readback) and where it is (write
  path) can differ entirely**; the original correct-mode/write-mode
  split evidence was the clue I nearly discarded.

### Next

Continue from the open questions above; revert SWEEPDBG once
root-caused; then riscv64/arm32/mips32-be correctness, then the
arm64 4K/16K/64K relative sweep.

---

## Session 61: Root Cause -- Decompress Folios Were Never memcg-Charged (2026-10-15)

### What We Did

The attribution sequence after the gdb backtrace (write path,
`write_begin -> decompress -> folio_add_lru -> folio_batch_move_lru ->
folio_lruvec_lock_irqsave` spinning in its memcg validate-retry loop):

1. `cgroup_disable=memory` on the cmdline -> **perf PASS**.  The spin
   is memcg-related; workaround confirmed, root cause still open.
2. `test.mode=proc` (read /proc/version into a fresh buffer): PASS ->
   generic anon-fault path is healthy.
3. `test.mode=plainread` (plain tmpfs write+read, same buffers,
   NO compression): PASS -> tmpfs plumbing healthy.
4. `test.mode=compread` (compressed write, prefaulted read buffer):
   STILL HUNG, and the smp=2 RCU-stall dump caught it with the trace
   `write_begin -> decompress_folio -> folio_add_lru -> ... ->
   folio_lruvec_lock_irqsave+0x114` -- the retry loop at
   mm/memcontrol.c:1501 (`lruvec_memcg(lruvec) != folio_memcg(folio)`
   -> unlock -> goto retry, forever).
5. Source diff against shmem's own path: `shmem_alloc_and_add_folio()`
   (mm/shmem.c:2083) charges every page-cache folio with
   `mem_cgroup_charge(folio, fault_mm, gfp)`.  Our decompress paths
   call the exported `shmem_alloc_folio()` (allocation only) and then
   insert into the page cache + LRU **with no charge ever**.  An
   uncharged folio can never satisfy the lruvec validate -> infinite
   retry -> hard hang, no warning, no lockup-detector output (the
   retry loop holds the rcu_read_lock and never schedules).

**Fix (applied, verified):** `mem_cgroup_charge(folio, NULL, gfp)`
immediately after `shmem_alloc_folio()` in BOTH `shmem_decompress_folio`
(single-page) and `shmem_decompress_group` (each merged slice), NULL mm
matching the write path; -ENOMEM -> folio_put + entry_put on failure.
All SWEEPDBG probes reverted (mm/shmem.c retry probe, the two
shmem_compress.c pr_err probes, the sweepdbg counter).  Result:
compread PASS with memcg ENABLED; full correct battery 36/36 PASS;
perf arm64-4k: fill 35.0 compressed / 33.2 cell / 107.7 plain MB/s,
read 45.1 / 49.9 / 91.5 (TCG, relative only).  Runner's `^GUEST_RESULT`
greps fixed (timestamp prefix broke the anchor).

### What We Learned

- **The hang hid behind three false theories** (TCG clock broken,
  decompress retry loop, bad folio batch) because each explained part
  of the evidence.  The instruments that finally worked, in order:
  guest clock probe (killed theory 1), SWEEPDBG retry probes (killed
  theory 2), QEMU monitor PC/LR sampling (located the function),
  gdb-multiarch manual fp-chain walk (located the caller), smp=2
  RCU-stall dump (authoritative full trace, showed memcg retry loop),
  and finally a 4-step attribution ladder (cgroup_disable -> proc ->
  plain -> compressed) that isolated the feature-specific trigger.
- **An infinite in-kernel retry loop defeats every in-guest detector**:
  hung-task needs a sleeping task, softlockup needs the scheduler to
  observe it (timer-driven prints did fire on smp=2, but the smp=1
  guest stayed silent).  Host-side observation (gdbstub, stall dumps)
  is the only reliable witness -- reaffirms the session-59 rule.
- **The real bug was ours and had been visible since the feature was
  written**: any decompressed folio added to the LRU without
  mem_cgroup_charge.  It could never fire on the 6.18 harness kernel
  (different memcg internals -- folio_lruvec there does not validate
  under the lock) nor under cgroup_disable=memory.  The arch/boot
  matrix found a BUG CLASS the compile matrix structurally cannot see:
  **first execution of the pointer-encoding kernel, and the first
  boot of ANY kernel of this tree with MEMCG=y + compressed tmpfs.**
- **memcmp-style attribution ladders beat single experiments**: each
  step (proc / plain / compressed, with and without memcg) removed one
  suspect class with a ~30s run.  Four runs isolated "feature-specific,
  memcg-conditional, read+write-path" from "everything else".
- mem_cgroup_charge's NULL mm is legal and matches the tmpfs write
  path (fault_mm is NULL for non-fault callers); charge failure must
  unwind folio_put + entry_put, not just return.

### What We Decided

- **The fix lives in shmem_compress.c (both decompress paths), not in
  the exported shmem_alloc_folio()** -- the export stays a pure
  allocator (allocation policy belongs to the caller), and charging
  belongs where the folio enters the cache/LRU, mirroring
  shmem_alloc_and_add_folio's structure.
- **Workaround `cgroup_disable=memory` rejected for the sweep** --
  with the fix the real configuration (MEMCG=y) passes, so the sweep
  runs the honest config.
- Runner grep anchors relaxed (timestamp-prefixed lines).

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | memcg-charge decompressed folios (single + group slices); probes reverted |
| `mm/shmem.c` | SWEEPDBG retry probe reverted to the clean `goto repeat` |
| `~/tmpfs-compress-test/scripts/run_arch.sh` | grep anchors fixed for timestamped lines |
| `~/tmpfs-compress-test/tests/arch_init.c` | +test.mode=proc / plainread / compread attribution modes |

### Next

1. Commit the charge fix (+ probe reverts) as its own commit.
2. Full matrix: arm64-4k/16k/64k perf, correctness on riscv64,
   arm32, mips32-BE -- kernels for 16k/64k/others need the charge
   fix rebuilt in (they were built before the fix).
3. Relative page-size analysis, final log pass.

## Session 62 (interim): arm64 Sweep Done; Third Stale-Artifact Bug (2026-10-15)

### Known so far (appending mid-work per the standing rule)

**arm64 page-size perf sweep COMPLETE** (TCG, 1 vCPU, 64 MiB, same
host, relative numbers only; second 4k run in parens for variance):

| phase | 4K | 16K | 64K |
|---|---|---|---|
| fill compressed | 33.5 (34.0) | 30.9 | 29.9 |
| read compressed | 42.7 (47.8) | 47.8 | 47.8 |
| fill cell | 31.2 | 29.7 | 29.9 |
| read cell | 49.2 | 49.8 | 49.5 |
| fill plain | 107.0 | 108.4 | 108.1 |
| read plain | 92.5 | 92.7 | 92.6 |

Readings (preliminary, single runs): plain baseline is FLAT across
page sizes (good -- the emulator is stable, differences in the
compressed arms are signal, not drift).  Compressed fill degrades
~10% from 4K to 64K (33.5 -> 29.9); compressed read IMPROVES ~12%
4K->16K then plateaus (42.7 -> 47.8).  Cell backend read is flat
~49-50 everywhere.  Run-to-run variance on 4k fill was ~1.5%, on
read larger (~10%: 42.7 vs 47.8 on repeat) -- so the read 4K-vs-16K
delta is borderline noise; the fill trend is within 2-3x variance of
being real.  For the series' results this needs N>=5 reps; for a
"direction of effect" answer: page size barely matters here, and
what signal exists says larger pages slightly hurt fill, slightly
help read at 16K.  Honest bottom line: **no dramatic page-size
effect in the synthetic sweep** -- plausible because the workload
compresses in PAGE_SIZE units but the zsmalloc/cell storage and the
crypto cost per byte dominate, and TCG blurs what remains.

**Third stale-source-artifact bug (session-45 class, now with a new
flavor):** after the memcg fix, arm32/riscv64/mips32-be rebuilds
failed at modpost with `section header offset=11258999068426292 ...
bigger than filesize` -- modpost parsing a valid ELF32 vmlinux.o as
ELF64.  Root cause: a STALE `scripts/mod/elfconfig.h` in the SOURCE
tree (dated Oct 14, left by the harness's in-tree x86_64 build)
saying `#define KERNEL_ELFCLASS ELFCLASS64`.  `modpost.c` quote-
includes "elfconfig.h", which resolves to the INCLUDING FILE's own
directory (source tree) before any -I path, so the freshly generated
out-tree ELFCLASS32 copy was never seen.  Moved the stale one to
out/srcartifacts/ (with pasyms.h.stale from earlier today).  arm32
then built clean (zImage 11.4M).

**Current state (in progress):** riscv64 and mips32-be rebuilds now
fail with the ucs_width_table.h / CONFIG_UNICODE error again -- the
olddefconfig repair I ran to fix their missing auto.conf apparently
re-enabled VT/UNICODE (the config disable from build_arch_kernel.sh
may not have survived, or olddefconfig re-added VT since its prompt is
EXPERT-gated and EXPERT state differs).  Not yet diagnosed; the fix
should be re-applying the scripts/config disables + olddefconfig on
those two trees.  Then: correctness runs on riscv64/arm32/mips32-be
(kernels nearly ready), then the final analysis and log pass.

### Rules re-affirmed (and broken once today)

- **Log at each meaningful step, even mid-diagnosis.**  The user had
  to remind me twice today; this interim entry is the compliance pass.
  Everything above was knowable 20 minutes ago.

## Session 62b (interim): riscv64 PASSES; arm32 READBACK CORRUPTION; mips no console (2026-10-15)

### Known so far (mid-diagnosis, logged per the rule)

- **riscv64 correct battery: PASS** (first non-x86/arm64 arch to run
  the feature end-to-end).  Rebuild needed CONFIG_CRYPTO=y back (the
  olddefconfig repair had left it =m -> undefined crypto_* at link),
  and the killed-earlier builds had left missing auto.conf -> fixed by
  olddefconfig re-runs.
- **arm32: DATA CORRUPTION on the zsmalloc arm** -- all 8 files
  written through the compressed tmpfs read back with FNV mismatch,
  and single-page rewrite_verify also mismatches.  Cell-arm mount
  starts, then the run stalls ~20s (watchdog fired, dumped stacks;
  the end-of-run "Attempted to kill init exitcode=0" is just
  reboot(RB_POWER_OFF) returning on qemu virt -- benign, needs an
  exit(0)+spin fallback in the guest).  This is EXACTLY the bug class
  the matrix exists to catch: 32-bit-only data corruption in the
  compress/decompress path that 4 passing configs (x86_64, arm64-4k/
  16k/64k, riscv64) could not see.  NOT YET ROOT-CAUSED.  Next
  instrumentations: first-differing-offset + length reporting in the
  guest verify, then bisect zsmalloc-vs-lz4-vs-our-code (e.g. write
  with compress=none on the same 32-bit kernel to rule the test out;
  compare compressed bytes via comp_stats).
- **mips32-BE: zero console output** -- the config grep for
  SERIAL_8250/SERIAL_8250_CONSOLE came back EMPTY: malta's serial
  driver is missing after my config re-application round (the original
  builder script never explicitly enabled it; malta defconfig must
  have had it before the repair clobbered something).  Fix: enable
  CONFIG_SERIAL_8250{,_CONSOLE} (+ EARLY_PRINTK) on mips32-be, rebuild.
- The 6th-config matrix status NOW: arm64-4k/16k/64k correct+perf
  PASS, riscv64 correct PASS, arm32 corrupt (open), mips32-be no
  console (config fix in progress).

### Open questions

1. arm32 corruption: test artifact or feature bug?  Highest-priority
   -- 32-bit data corruption would block the series exactly like the
   original pack_loc bug did.
2. mips serial config.
3. Guest reboot() fallback so end-of-run panics stop masking results.

## Session 62c (interim): arm32 Corruption Characterized (2026-10-15)

### Known so far (mid-diagnosis)

The instrumented guest (first-differing-offset reporter, streaming
verify with regenerated expected bytes) ran on arm32:

- **zs-arm readback: len=1048576 CORRECT, firstdiff=0 on all 8
  files** -- not truncation; the decompressed content is wrong from
  byte 0.  Single-page rewrite_verify also mismatches.  So the
  compress->store->load->decompress chain returns wrong data on 32-bit
  for EVERY page.  A systematic per-page error, not memory garbage
  (garbage would put firstdiff at random offsets).
- **cell-arm write phase then spins**: after the cell mount line at
  0.84s, no further output; the run-1 watchdog dump shows tid 1 with
  an EMPTY stack (task RUNNING, not blocked = busy spin) and tid 43
  (the watchdog) reading /proc.  End-of-run "Attempted to kill init
  exitcode=0x00000000" is NOT the reboot() path -- exitcode 0 means
  PID 1 EXITED CLEANLY, which should be impossible before
  GUEST_RESULT.  Still unexplained; candidate: the cell-arm write on
  arm32 crashes userspace or the process is reaped oddly.  (Note: my
  new byte-scan verifier is O(filesize * 64KiB) per file -- absurdly
  slow in principle but the 8 files reported by 0.80s, so the slowness
  did NOT bite here; still needs replacing with a blockwise compare
  before the final runs.)
- **Bisect plan (next runs):**
  1. arm32, mount cell=64K only (skip zsmalloc): if cell readback is
     CLEAN -> bug is zsmalloc-path-specific on 32-bit; if corrupt too
     -> lz4/crypto or our common pack path on 32-bit.
  2. arm32 with compress=none: rules the test + tmpfs plumbing in/out
     on 32-bit (expected clean; plainread passed but only 64K).
  3. smp=2 RCU-stall dump of the cell-arm spin (worked on arm64).
  4. Suspects for a systematic byte-0-wrong on 32-bit: the (u64)
     handle cast chain (lossless per analysis, but verify empirically),
     zs_obj_read_begin bounce path on 32-bit, lz4 asm on arm (defconfig
     may enable ARM-optimized LZ4 -- CONFIG_LZ4_* asm!), crypto acomp
     scatterlist 32-bit handling.
- **riscv64 correct: PASS.**  mips32-be: serial re-enabled
  (SERIAL_8250{,_CONSOLE}), vmlinux-only rebuild started (mkimage
  missing, U-Boot ITB not needed for -kernel boot).
- Guest improvements landed: reboot() fallback (spin, so PID-1 exit
  panics stop masking results -- though this run predated it), and the
  firstdiff/len mismatch reporter.

### Open questions (priority order)

1. arm32 zs corruption root cause (cell-vs-zs bisect run pending).
2. arm32 cell-arm spin + the clean-exit-of-init mystery.
3. mips32-be console + correctness run (rebuild in progress).
4. Replace the O(n^2) byte-scan verifier with blockwise diff.

## Session 63: TWO HIGHMEM BUGS FOUND AND FIXED on arm32 (2026-10-16)

### What We Did

The arm32 failures (zs readback corruption from byte 0; cell-arm first
write crashing) were root-caused in a multi-step attribution:

1. `firstdiff=0`, correct length on every zs file -> systematic
   per-page error, not truncation.  Cell arm: kernel oops with
   `PC=mmiocpy, r0=NULL, LR=0x50`, later a traced
   `SWEEPDBG store: base=00000000 ...` -> cell_store memcpying to a
   NULL cell base on the FIRST store.
2. A `highmem=off` cmdline test appeared to refute the highmem theory
   -- but the kernel log exposed the flaw in my own test:
   **"Unknown kernel command line parameters highmem=off"** -- the
   parameter does not exist in this tree, highmem stayed enabled, and
   the zone dump proved HighMem existed.  The theory was never tested.
3. The decisive experiment: same kernel, **-m 512** (RAM below the
   highmem boundary -> no highmem pages can exist).  BOTH arms PASS.
   Both arm32 failures are HIGHMEM bugs.  One mechanism, two sites:

**Bug A (cell backend, ours):** `cell_gfp()` returned
`GFP_TRANSHUGE` (which contains GFP_HIGHUSER_MOVABLE -> __GFP_HIGHMEM)
or `GFP_KERNEL` for cell pages, then `cell_make_active` stored
`c->base = page_address(page)`.  page_address() is NULL for highmem
pages -> cell_store memcpy to NULL -> oops/garbage.  Cells hold a
PERMANENT kernel mapping by design (bump allocator, readers memcpy
from c->base for the cell's whole life), so they must be lowmem by
construction.  Fix: strip `__GFP_HIGHMEM|__GFP_MOVABLE` in
cell_gfp() (this tree has no __GFP_NOHIGHMEM -- custom gfp rework).

**Bug B (crypto path, ours):** `shmem_comp_compress/decompress` build
their scatterlists with `sg_init_one(&sg, virt_addr, len)`, which
derives `struct page *` via virt_to_page().  The single-page data
path passed **kmap_local_folio() addresses** -- fixmap-range addresses
on 32-bit+highmem, for which virt_to_page() is INVALID: the crypto
compressed the WRONG memory and wrote decompressed output to the WRONG
page.  That is the byte-0 corruption on every page.  Invisible on
64-bit (kmap == direct map; virt_to_page works) and with small RAM.
zsmalloc itself is safe (it strips __GFP_HIGHMEM from zspage allocs at
mm/zsmalloc.c:448/460 and kmaps in zs_obj_read/write_begin/end); the
bug was only on our folio-side bounce.  Fix: bounce the folio through
a lowmem kmalloc buffer in both shmem_comp_compress_one (source side)
and shmem_decompress_folio (destination side); the merged-group paths
already used kmalloc buffers and were safe.

**Verification:** arm32 at -m 1536 (the failing config): full correct
battery across ALL arms (zs, cell, merge, plain) **PASS**.

### What We Learned

- **A boot parameter that the kernel does not recognize is silently
  ignored** -- "Unknown kernel command line parameters" is printed ONCE
  among hundreds of boot lines.  A negative experiment is only as good
  as its verification that the experiment actually took effect.  I
  lost a cycle concluding "highmem refuted" from a test that never
  ran; the zone dump in the same log contradicted it.  Rule: when a
  cmdline knob is load-bearing for a diagnosis, grep the log for its
  echo in the command line AND for the "Unknown parameters" warning.
- **The best negative control was memory sizing**: -m 512 vs -m 1536
  toggles whether highmem pages CAN exist, with zero rebuilds.  When
  a hypothesis maps to a resource threshold, prefer the cheap toggle
  (RAM size, cpu count, smp) over a config rebuild.
- **kmap_local + sg_init_one is a 32-bit-highmem trap**: the address
  is valid to dereference but NOT to hand to anything that does
  virt_to_page.  The general rule for our code: crypto/DMA APIs take
  pages or lowmem addresses; kmap addresses stay inside the
  dereference window (kmap/kunmap pair).  Recorded as a house rule.
- **page_address() = NULL is the highmem smoke signal** for any code
  storing a long-term kernel mapping of an allocated page; grep our
  code for page_address on alloc_pages results (cell_store was the
  only remaining one; zsmalloc strips the bit itself).
- The first attribution attempt used a WRONG cmdline knob and nearly
  produced a false "not our bug" conclusion; the crash-register dump
  (r0=NULL) and the store-trace probe are what kept the truth
  reachable.  Direct argument tracing beat another round of theory.

### Rules Added

- **kmap_local addresses never leave the kunmap window**: no
  virt_to_page/sg_init_one/DMA on them; bounce through kmalloc for
  any API that maps addresses to pages.
- **Long-lived page mappings must be lowmem by construction**
  (__GFP_HIGHMEM stripped): a stored page_address must never be NULL.
- **Verify a cmdline control took effect** (grep the echoed command
  line + "Unknown parameters") before trusting a negative result.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cell_store.c` | cell_gfp(): strip __GFP_HIGHMEM/__GFP_MOVABLE (cells need a permanent direct mapping); SWEEPDBG probe removed |
| `mm/shmem_compress.c` | lowmem kmalloc bounce for the folio side of compress (compress_one) and decompress (decompress_folio) -- sg_init_one must never see kmap_local addresses |

(UNCOMMITTED at time of writing; commit follows once the remaining
arch rebuilds + regression runs are green.)

### Next

Rebuild+reverify all other kernels with the fixes (background), mips32
console + battery, then commit + final analysis.

## Session 64: Final Matrix, Perf Sweep, and Wrap (2026-10-16)

### Correctness matrix -- ALL PASS (5 of 6 bootable)

| config | toolchain | correct battery |
|---|---|---|
| arm64-4K | aarch64-linux-gnu | PASS |
| arm64-16K | aarch64-linux-gnu | PASS |
| arm64-64K | aarch64-linux-gnu | PASS |
| arm32 (1.5G, HIGHMEM) | arm-linux-gnueabihf | **PASS** (after the two HIGHMEM fixes) |
| riscv64 | riscv64-linux-gnu | PASS |
| mips32-BE | mips-linux-gnu | **boot blocked** (see below); compiles 0/0 |

The correct battery = zsmalloc/cell/merge/plain mounts, write 8 MiB of
deterministic ~2:1-lz4 data per mount, streaming-FNV readback verify,
truncate+rewrite verify.  The two HIGHMEM fixes (session 63) took
arm32 from corrupt/hanging to PASS with no regressions elsewhere.

### Relative perf sweep (arm64, TCG, 1 vCPU, 64 MiB, FIXED kernels)

| phase | 4K | 16K | 64K | (4K rerun) |
|---|---|---|---|---|
| fill compressed | 32.5 | 33.2 | 32.0 | 34.3 |
| read compressed | 42.5 | 43.8 | 44.6 | 46.9 |
| fill cell | 31.5 | 33.1 | 32.4 | -- |
| read cell | 47.1 | 48.2 | 46.4 | -- |
| fill plain | 104.4 | 105.7 | 106.0 | -- |
| read plain | 91.8 | 91.8 | 88.8 | -- |

MB/s.  **Conclusion: page size is flat within noise in this synthetic
sweep.**  Run-to-run variance on the same config is ~5% fill / ~10%
read -- larger than every cross-page-size delta.  What signal exists:
compressed read trends slightly UP with page size (42.5 -> 44.6,
+5% 4K->64K: fewer, larger decompress calls amortize the per-page
folio alloc+charge+pin), compressed fill is flat (crypto cost per byte
dominates and is page-size-insensitive at 4K-64K), the cell backend
tracks zsmalloc within 5%, and the plain baseline is flat (sanity:
the emulator is stable, so the compressed arms are comparable).  For
the series this needs N>=5 reps and real hardware; as a first
direction-of-effect answer the honest reading is "no dramatic
page-size effect; compression cost dominates".

Note: the bounce fix (session 63) added one memcpy to the single-page
compress+decompress paths on ALL arches; the numbers above are from
the fixed kernels and are ~3% below the pre-fix ones -- the price of
being correct on 32-bit+highmem, paid uniformly.

### mips32-BE: boot blocked, NOT a feature bug

Evidence: kernel is a valid BE MIPS32 ELF (file: "ELF 32-bit MSB
executable, MIPS"), config verified (CPU_BIG_ENDIAN=y, SERIAL_8250=y,
console=y), but ZERO serial output with ttyS0/ttyS1/earlycon, with or
without initrd, with default or 24Kf CPU.  QEMU monitor shows the CPU
parked at **0xbfc00380 -- the MIPS cache-error vector** -- the CPU
never reaches the kernel.  This custom tree (heavy local mods:
nstree/nscommon/map_anon_folio_pte_nopf etc.) appears to have a
broken MIPS boot path, or a QEMU-malta handshake regression.  NOT
pursued further: the BE AXIS is covered at compile level (mips32-BE
builds 0/0 and is in the matrix), and a BE RUNTIME can be had another
way (s390x under KVM if the user installs gcc-s390x-linux-gnu; s390x
is BE64 and KVM-accelerated).  Logged as a machine/port issue, not
shmem_comp.

### Session total (the whole sweep, sessions 59-64)

- New per-arch test rig: static PID-1 guest (no busybox), 6 bootable
  kernels, per-arch QEMU runner, attribution modes (proc/plainread/
  compread/clock), first-diff verifier, TEST_ARMS backend selector,
  stack-dump watchdog thread.
- **Three real bugs found**, all invisible to the compile matrix:
  1. memcg: decompressed folios never charged -> infinite
     folio_lruvec_lock retry -> hard hang (all arches with MEMCG=y
     would hit it; only arm64 booted first).
  2. cell_store: highmem cell pages -> page_address NULL -> memcpy to
     NULL (arm32).
  3. crypto sg: sg_init_one on kmap_local addresses -> compress/
     decompress wrong memory on 32-bit+highmem (arm32).
- Commits: `d9170a7d1c4f` (memcg charge), `d4dc816b1b77` (HIGHMEM x2),
  plus mission-log commits.  SWEEPDBG probes all reverted; tree clean.

### Files changed (final)

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | memcg-charge decompress folios; lowmem crypto bounces (both HIGHMEM fixes) |
| `mm/cell_store.c` | cell_gfp(): lowmem-only cell pages |
| `mm/shmem.c` | (probe only, reverted -- no net change) |
| `~/tmpfs-compress-test/tests/arch_init.c` | the cross-arch guest (new) |
| `~/tmpfs-compress-test/scripts/{run_arch,pack_initramfs}.sh` | the cross-arch runner (new) |
| `~/linux/out/build_arch_kernel.sh` + rebuild scripts | bootable-kernel builders with axis verification |

### Next Steps

1. **H618 re-measure** -- the real perf question; this sweep answered
   "page size is flat within noise under emulation", which predicts
   the H618 comparison is about zsmalloc-vs-cell and watermark policy,
   not page size.  Standing backlog items unchanged (plateau, CPU-freq,
   workload bed, RFC assembly, group-decompress/truncate interleaving).
2. BE runtime via s390x+KVM if the user wants runtime BE coverage
   (install gcc-s390x-linux-gnu); mips boot debug only if asked.
3. The three fixes deserve spec/commit-message cross-links in the
   RFC assembly pass (specs/02 pointer encoding already updated).

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

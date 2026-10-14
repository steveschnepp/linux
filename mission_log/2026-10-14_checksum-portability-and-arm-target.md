---
name: checksum-portability-and-arm-target
description: pure-C autovec-friendly checksum beats intrinsics (and is the RIGHT choice for the real ARM SBC target); dedicated-initrd bench; -cpu host vs qemu64 CPUID effect; session-restart handoff with H618/KASAN context
project: linux
previous: 2026-10-14_read-verify-checksum.md
---

# Mission Log: Checksum Portability + the ARM SBC Target (Session Restart Handoff)

Part 44 of the tmpfs compression series.  Session 53 (2026-10-14),
continued.  A session-restart handoff: picks up from the read-verify
checksum PoC, lands on the final checksum choice, discovers the REAL
target is an ARM SBC (Allwinner H618), and records the state for the
next session.

## What We Did

### 1. Pure-C autovectorization-friendly checksum (the winner)

Added two pure-C candidates to `poc/hash_bench.c` (no intrinsics):
`h_poly_c_single` (one accumulator) and `h_poly_c4` (four independent
accumulators, mixed to 64 bits).  The question: does plain C auto-vec to
match the `poly32_sse2` intrinsic, so we can drop the SSE4.1 dependency?

Result: **`poly_c4` (pure C) matches the intrinsic exactly** (278 ns/page
vs 273, both ~14.7 GB/s) and is order-sensitive (catches the word swap
plain xor+sum misses).  `poly_c_single` (one accumulator) is much slower
(662 ns) -- the 4 independent accumulators are what give the compiler the
ILP / packable reduction it needs.  **Takeaway: carefully-written
autovec-friendly C is as fast as intrinsics, and more portable.**

BUT: the auto-vec only happens when SSE4.1 is available on the compile
target.  `vpmulld` (32x32->32 multiply) is SSE4.1; at the default SSE2
target GCC cannot emit it, so `poly_c4` falls back to ~378 ns (still
faster than memcmp's 482).  `build_tests.sh` uses plain `-O2` (SSE2
default), so wiring `poly_c4` into `test_matrix` needs either `-msse4.1`
added to the build flags OR acceptance of the slower scalar path.  NOT
YET WIRED -- `tests/test_matrix.c` has an incomplete edit (an SSE4.1
`poly32_sse2` that will NOT compile under build_tests.sh's flags; the
pure-C `poly_c4` is the intended replacement but is not yet in the
reader).

### 2. Dedicated initrd to bench in a KVM guest (not yet run to completion)

Built `poc/hashbench_init.c` (a self-contained /init that benches and
reboots) and packed a minimal initrd.  First boot **SIGILL'd**: the
`-march=native` build emitted AVX (VEX-prefixed `vmovdqa`), and QEMU's
default CPU model (`qemu64`, no `-cpu host`) does not expose AVX -> the
guest kernel never enabled XCR0.AVX -> #UD.  This is the CPUID/OS-gate
mechanism: KVM controls feature DISCOVERY (CPUID); the guest kernel
enables register state (CR4/XCR0) for what CPUID advertises; hiding a
feature means the OS does not enable it, so use faults.  `-cpu host`
fixes it (guest sees host ISA).  Re-ran with `-cpu host`: bench completed
in-guest, numbers close to host (poly_c4 288 ns/page in guest).

### 3. -cpu host vs qemu64: does it change the tmpfs numbers? (measured)

Added a `QEMU_CPU` override to `run_qemu.sh` (both QEMU sites).  A/B'd
the SAME kernel + test with `qemu64` (old default) vs `host`:
mostly within n=1 noise; `20:80` read was +4.8% on host (plausible:
kernel CPUID-patches memcpy/copy_user to AVX2/fast-string, and that read
path leans on copy_to_user).  Small, but a comparability break.  **DECISION:
keep `qemu64` as the rig default** (so the entire prior benchmark series
stays comparable); `QEMU_CPU=host` is an explicit opt-in for native-ISA
binaries.  Reverted the default; override kept.

### 4. THE BIG ONE: the real target is an ARM SBC (Allwinner H618)

User: the main purpose of ztmpfs is SBCs, mostly **H618 and equivalents**
(ARM Cortex-A53, ARMv8-A, in-order quad-core, NEON, ~1.5GHz).  This
reframes the whole checksum + benchmark work:

- **The x86 SIMD discussion is target-irrelevant.**  `poly32_sse2` uses
  `_mm_mullo_epi32` (SSE4.1) -- does NOT exist on ARM.  Had we wired the
  intrinsic in, it would be x86-only dead weight.  The pure-C `poly_c4`
  is the RIGHT choice for ARM: auto-vectorizes to NEON, portable, no
  intrinsics.  (The choice was made on x86 reasoning but is correct for
  the target.)
- **The in-order A53 makes the ILP argument STRONGER.**  In-order cannot
  find instruction-level parallelism the way OoO Xeon does -- you must
  expose it.  `poly_c4`'s 4 independent accumulators are exactly that.
  Serial candidates (FNV/murmur/crc32) get RELATIVELY worse on in-order.
- **None of the ns/page numbers transfer.**  278 ns is a 3.5GHz OoO
  Xeon; the A53 is ~1.5GHz in-order.  Absolute numbers several x worse;
  relative gaps widen in favor of multi-accumulator forms.  The
  ARCHITECTURAL reasoning transfers; the figures do not.
- **The entire tmpfs benchmark series (triangle, matrix, thread-scaling)
  is x86-KVM-based and does NOT represent the H618.**  lz4 throughput,
  the eager/deferred tradeoff, the "4 threads sweet spot" -- could shift
  on a 1.5GHz in-order quad-core with less RAM bandwidth.  Qualitative
  findings likely hold; numbers and optimal band need re-measuring.

### 5. User's plan (next session)

- Will launch a series of benches on the **H618 box when the code is
  stable**; **only aiming for perf** there.
- **ARM will need KASAN**, because **memory ordering is different** --
  so correctness/KASAN runs are planned on ARM, not just perf.
- Session restart: another agent in another session had switched branches;
  user already switched BACK to the correct branch.  This log is the
  handoff point.

## What We Learned

### Technical

- **Carefully-written autovec-friendly C matches intrinsics** for the
  polynomial checksum: `poly_c4` (4 independent accumulators, restrict,
  flat reduction) = 278 ns vs the SSE4.1 intrinsic's 273.  The 4
  accumulators are essential (single-accumulator = 662 ns).  Prefer
  portable C over intrinsics when they tie -- it ships to ARM unchanged.
- **32x32->32 multiply is SSE4.1 (`vpmulld`)**; no SSE2 equivalent, so a
  32-bit polynomial cannot auto-vec at the default SSE2 target.  Needs
  `-msse4.1`+ or NEON on ARM (where 32-bit multiply is baseline NEON).
- **QEMU default CPU is `qemu64`** (no AVX); `-march=native` binaries
  SIGILL unless `-cpu host`.  The mechanism is CPUID feature DISCOVERY +
  the guest kernel enabling register state (XCR0 for AVX) for what
  CPUID advertises.  KVM does not intercept ordinary SIMD; the fault is
  the OS-gate (XCR0), not hypervisor blocking.
- **`-cpu host` shifts some tmpfs numbers** (kernel CPUID-patches
  memcpy/copy_user to AVX2/fast-string): ~5% on the deferred read path,
  mostly noise elsewhere at n=1.  Small but a comparability break.

### Process

- **The 3rd too-good-to-be-true measurement this session** (the hash
  bench memcmp at 818/938 GB/s) was GCC hoisting a loop-invariant
  compare out of the timed loop.  Standing rule now: a number that
  exceeds physical L1 bandwidth is a HARNESS artifact, not a fast CPU.
- **Reuse the proven `init.c` supervisor** for guest bench initrds; do
  NOT put reboot() logic in the bench itself (mix of concerns).  A
  dedicated initrd (init.c + init.sh + bench) is the clean way to test
  C code in a KVM guest.
- `init.c` has NO general cmdline options (only `init.reboot=`); init.sh
  reads /proc/cmdline itself -- that's how all test modes are selected.

## What We Decided

- **Checksum for test_matrix: `poly_c4` (pure C, 4-accumulator
  polynomial).**  Not the SSE4.1 intrinsic (x86-only, wrong for ARM).
  Wiring NOT yet done; `tests/test_matrix.c` has a partial/incomplete
  edit that must finish with the pure-C version (and either add
  `-msse4.1` to build_tests.sh or accept the scalar path on x86).
- **`qemu64` stays the rig default**; `QEMU_CPU=host` is an explicit
  opt-in.  Prior benchmark series stays comparable.
- **Target is H618 (ARM Cortex-A53); only perf on the SBC; KASAN on ARM
  for correctness** (memory ordering differs).  Re-measure the key
  benchmarks on ARM before trusting x86-derived numbers for the SBC.

## Rules Added

- **Prefer portable autovec-friendly C over intrinsics when performance
  ties.**  It ships to the ARM target unchanged; intrinsics are
  ISA-locked.  For SIMD-friendly reductions, use multiple independent
  accumulators (exposes ILP; critical on in-order ARM).
- **A measurement that looks impossibly good is a harness bug** (loop
  invariants let GCC hoist the work out of the timed loop).  Third
  occurrence this session; treat any number above physical L1 bandwidth
  as suspect until the loop is proven streaming/non-invariant.
- **Bench C code in a KVM guest via a dedicated initrd reusing the
  proven init.c supervisor**, not by adding reboot logic to the bench.

## What We'd Do Differently

- Would have asked about the TARGET ARCHITECTURE before spending a whole
  thread optimizing an x86-specific intrinsic.  The pure-C `poly_c4`
  turned out to be right for ARM, but that was luck -- the intrinsic
  (`poly32_sse2`) was the initial winner and would have been a wrong
  call for the SBC.
- Would have checked the guest CPU model (qemu64, no AVX) before
  building a `-march=native` guest binary; the SIGILL was predictable.
- Would have tested `-cpu host` on the tmpfs numbers BEFORE changing the
  default; changing it first and reverting after was a detour.

## Files Changed

| File | Purpose |
|------|---------|
| rig `poc/hash_bench.c` | + pure-C poly_c_single/poly_c4 candidates; SSE4.2 crc32 runtime guard (uncommitted) |
| rig `poc/hashbench_init.c` (new, uncommitted) | self-contained guest initrd bench (uses reboot; superseded by the init.c-reuse approach) |
| rig `scripts/run_qemu.sh` | `QEMU_CPU` override at both QEMU sites (default reverted to qemu64) (uncommitted) |
| rig `tests/test_matrix.c` | INCOMPLETE edit: an SSE4.1 `poly32_sse2` that won't compile under build_tests.sh; must become pure-C `poly_c4` (uncommitted) |
| rig `poc/hb_*` | compiled PoC binaries (should be gitignored; stray) |

## Test Results

- Guest bench (dedicated initrd, `-cpu host`, native build): poly_c4
  288 ns/page in-guest (vs 278 on host) -- close; confirms the guest
  can run it when the ISA is exposed.
- `-cpu host` vs `qemu64` tmpfs A/B (n=1): 0:0 write -1.6%, 0:0 read
  +0.9%, 20:80 write +2.3%, 20:80 read +4.8%.  Mostly noise; the read
  shift is plausible (AVX2 copy_user).
- Checksum candidates (streaming, host): poly_c4/poly32_sse2 ~278 ns,
  xorsum_sse2 290, memcmp 482.  poly_c4 catches bitflip + word swap.

## Next Steps

1. FINISH wiring `poly_c4` (pure C) into `tests/test_matrix.c`'s reader
   -- replace the incomplete SSE4.1 edit; decide whether to add
   `-msse4.1` to build_tests.sh (x86 auto-vec) or accept scalar.  The
   ARM build will NEON-auto-vec it either way.
2. Re-run the thread-scaling sweep to confirm the read-verify overhead
   drops with poly_c4 (vs memcmp).
3. Standing items unchanged: the plateau mechanism (lockstat), CPU-freq
   confound, cliff-churn matrix (watermark last gate), workload phase 1,
   RFC assembly (two-table).
4. LONGER-TERM (user's plan): re-measure key benchmarks on the H618 ARM
   box when code is stable (perf only there); KASAN on ARM for
   correctness (memory ordering differs).  The x86 KVM series is a
   proxy, not the target.

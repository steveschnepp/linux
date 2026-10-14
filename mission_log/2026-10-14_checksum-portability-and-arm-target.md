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

---

## poly_c4 finalized into test_matrix (2026-10-14, session 53 continued)

Picks up this log's "Next Steps #1" (finish wiring the pure-C `poly_c4`
into the rig's read-verify).  Closes the checksum thread.

### What We Did

- In the RIG tree (`~/tmpfs-compress-test`, NOT the kernel repo --
  `tests/`, `scripts/`, `guest/` live there), `tests/test_matrix.c` still
  carried the INCOMPLETE edit this log warned about: `build_pagepool` and
  the read-back verify both called `poly32_sse2`, the SSE4.1 intrinsic
  (`_mm_mullo_epi32`).  Confirmed it does NOT build under `build_tests.sh`'s
  plain `-O2` (SSE2 default): `inlining failed in call to always_inline
  '_mm_mullo_epi32': target specific option mismatch`.
- Replaced `poly32_sse2` with the pure-C `poly_c4` (4 independent
  accumulators, `restrict`, flat reduction) -- the exact function this log
  chose.  Updated BOTH call sites (`build_pagepool` + the read verify) so
  the digest stays self-consistent, and dropped `#include <immintrin.h>`
  (no intrinsics remain anywhere in the file).
- Added the WHY as comments on `poly_c4`: why the position-weighted
  polynomial (order-sensitivity that xor+sum lacks), why plain C over the
  intrinsic (H618/NEON portability), why 4 accumulators (in-order A53 ILP
  + auto-vec; single-accumulator was 2x slower), why `restrict`, and why
  we do NOT add `-msse4.1` to `build_tests.sh`.

### Decision: do NOT add -msse4.1 to build_tests.sh

This log left the flag question open.  Resolved to ACCEPT THE SCALAR PATH
on x86 and keep `build_tests.sh` at plain `-O2`, because:
- `build_tests.sh` compiles EVERY guest test with one flag set; an ISA
  flag would lift the required-feature floor for the whole suite.
- The guest runs under QEMU's default `qemu64` model, which we keep as the
  rig default for cross-run comparability; we do not want binaries
  depending on SSE4.1 being exposed.
- On the ARM target the same C NEON-auto-vectorizes regardless of x86
  flags, so `-msse4.1` buys nothing where it matters.
- Under `-O2` the scalar path is still faster than the memcmp it replaces,
  so x86 test runs are not meaningfully slower.
If an x86 guest ever needs the last few %, clip an SSE4.1 clone into this
one TU behind `__builtin_cpu_supports` (the hash_bench avx2-candidate
pattern) rather than lifting the suite's ISA floor.

### Test Results

- `gcc -static -O2 -Wall -Wextra` (build_tests.sh flags): now exit 0, no
  warnings (was a hard compile error on `_mm_mullo_epi32`).
- Host functional check (`out/tm_check.c`, includes the test TU behind a
  renamed `main`, calls the REAL `build_pagepool`/`poly_c4`):
  self-consistency 0 mismatches, bitflip_caught=1, wordswap_caught=1 --
  PASS.  Confirms pool-builder and reader use the same function and the
  hash is order-sensitive.

### Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_matrix.c` | poly32_sse2 -> pure-C poly_c4 at both call sites; drop immintrin.h; why-comments; -msse4.1 rationale |

### Next Steps

- Re-run the thread-scaling sweep to confirm read-verify overhead drops
  with poly_c4 vs memcmp (this log's Next Steps #2).
- Rig build via build_tests.sh + build_rootfs.sh, verify the STAGED
  binary (the standing rule) before trusting a guest run.

---

## SSE2 SIMD for the checksum + poly_c8_widen adoption (2026-10-14, session 53 continued)

Follow-on to the poly_c4 wiring above.  User question: can the read-verify
checksum SIMD on SSE2 (the build_tests.sh baseline, no -msse4.1), and if not,
what is missing / can the formula be tweaked?  Answer: yes, by widening the
polynomial's accumulator from mod 2^32 to mod 2^64.  New host PoC
`poc/hash_sse2.c`; the winner (poly_c8_widen) is now wired into test_matrix.c.

### What was missing

The old checksum needed w[i]*K[i] as a 32x32->32 multiply = `pmulld` =
`_mm_mullo_epi32` = **SSE4.1**.  SSE2 has NO 32x32->32.  But SSE2 DOES have
`pmuludq` (`_mm_mul_epu32`): 32x32->**64**, widening, even lanes only.  So the
missing piece was never capability -- it was PRECISION SHAPE.

### The formula tweak

Accumulate mod 2^64 instead of mod 2^32 (a WIDENING polynomial
`h = SUM_i w_i * K^i (mod 2^64)`).  Now every product is 32x32->64 and maps to
`pmuludq` (SSE2) / NEON mul (ARM), not `pmulld`.  Two side benefits: the hash
gets STRONGER (64-bit accumulate = ~1/2^64 collision per page vs ~1/2^32), and
the widening C form is what the auto-vectorizer likes at plain -O2.  Verified:
GCC emits `pmuludq`/`paddq` for poly_c8_widen's loop at `-O2 -mno-sse4`
(objdump: pmuludq=12, pmulld=0), i.e. pure SSE2, no intrinsic hand-writing.

### THE ALGORITHM CATALOG (all candidates, formulas + SSE2-only numbers)

Weights for all: `polyK[i] = K^i mod 2^32`, K = 0x9E3779B1 (odd), i = 0..1023;
one weight per 32-bit page word, so position changes contribution (order
sensitivity).  `mix64(a0,a1,a2,a3) = h; h=a0^(a1*0x100000001b3);
h^=(a2<<32)|(a3>>32); h*=0x9E3779B97F4A7C15; return h^(h>>32)` -- odd-multiply
and xor-shift are bijections mod 2^64, so the mix preserves order-sensitivity.

| # | name | formula / shape | ISA | ns/page (SSE2 build) |
|---|------|-----------------|-----|----------------------|
| 1 | poly32_sse2 | SUM w_i*K^i mod 2^32, 4x `_mm_mullo_epi32` | SSE4.1 | (n/a at SSE2; was 278 on x86 w/ SSE4.1) |
| 2 | poly_c4 | a_j += w[i+j]*K[i+j] (u32, mod 2^32), j=0..3; fold | needs pmulld to vec | 429 (scalar) |
| 3 | poly_c4_widen | a_j += (u64)w[i+j]*K[i+j] (mod 2^64), j=0..3 | SSE2 (pmuludq) | 407 |
| 4 | **poly_c8_widen** | a0..a3,b0..b3 (8x u64) += (u64)w[i+j]*K[i+j]; fold (a^b) lanes | SSE2 (pmuludq) | **348** |
| 5 | poly_muludq | hand SSE2: vacc0=even words, vacc1=odd (srli 4), `_mm_mul_epu32`+`_mm_add_epi64` | SSE2 | 378 |
| 6 | xorsum_sse2 | s+=v (epi64), x^=v; return combine -- ORDER-BLIND control | SSE2 | 264 (blind) |
| 7 | memcmp | 2-region compare, the thing we replace | baseline | 484 |

Pseudocode for the two that matter:

```
poly_c4_widen(b, n):                     poly_c8_widen(b, n):
  u64 a0=a1=a2=a3=0                        u64 a[8]={0}
  for i in 0,4,8.. step 4:                 for i in 0,8,16.. step 8:
    a0+=(u64)w[i]*K[i]                       for j in 0..7:
    a1+=(u64)w[i+1]*K[i+1]                     a[j]+=(u64)w[i+j]*K[i+j]
    a2+=(u64)w[i+2]*K[i+2]                   h=a[0]^a[4]; g=a[1]^a[5] ...
    a3+=(u64)w[i+3]*K[i+3]                   return mix64(h,g,a[2]^a[6],a[3]^a[7])
  return mix64(a0,a1,a2,a3)
```

### What we learned

- **The hand-written intrinsic (#5) is unnecessary.**  GCC's auto-vec of the
  8-accumulator plain-C form (#4, 348) BEAT my 2-accumulator hand `_mm_mul_epu32`
  version (#5, 378).  Same standing lesson: good C > intrinsics -- more ILP
  (8 acc) beats tighter code.
- **Widening to 64-bit is a strict upgrade here**: SSE2-clean (baseline ISA, no
  -msse4.1 needed at all now), NEON-clean on ARM, AND a stronger hash.  The
  earlier "32-bit accumulate" was the only thing forcing SSE4.1.
- An AVX2 comparison build of hash_sse2 SIGILL'd (exit 132): this host is Ivy
  Bridge (AVX, no AVX2), the trap the checksum log already documented.  SSE2 is
  the reference; the AVX2 ceiling is unmeasured on this host.

### Decision: adopt poly_c8_widen in test_matrix.c

Replaced poly_c4 with poly_c8_widen (#4) as the read-verify checksum.  It is
~19% faster than poly_c4 and ~28% faster than memcmp on SSE2, order-sensitive
(catches the word swap), portable (auto-vecs to NEON on the H618, where 8
accumulators expose even MORE ILP for the in-order A53), and strengthens the
digest to 64-bit.  polyK table (32-bit weights) is reused unchanged; the
checksum path is now stdint (uint32_t weights / uint64_t products+digest).

### Test Results

- `gcc -static -O2 -Wall -Wextra` (build_tests.sh flags): exit 0, no warnings.
- Functional check (`out/tm_check.c`, includes the test TU behind a renamed
  main, calls the REAL build_pagepool/poly_c8_widen): self-consistency 0
  mismatches, bitflip_caught=1, wordswap_caught=1 -- PASS.
- hash_sse2 SSE2-only bench: poly_c8_widen 348 ns/page (vs poly_c4 429,
  memcmp 484, xorsum_sse2 264-blind).  objdump: pmuludq present, pmulld absent.

### Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_matrix.c` | poly_c4 -> poly_c8_widen (8-acc widening, stdint) at both call sites; updated why-comments; +<stdint.h> |
| rig `poc/hash_sse2.c` (new) | SSE2-only checksum bench: poly_c4/c4_widen/c8_widen/muludq/xorsum/memcmp, auto-vec + objdump evidence |

### Next Steps

- Re-run the thread-scaling sweep to confirm read-verify overhead drops with
  poly_c8_widen vs memcmp (and vs the old poly_c4).
- Re-measure poly_c8_widen on the H618 once the code is stable (expect NEON
  auto-vec; 8 acc should help the in-order core).
- Rig build via build_tests.sh + build_rootfs.sh, verify the STAGED binary.

---

## Cross-ISA validation of the widening checksum: aarch64 + rv64 (2026-10-14, session 53 continued)

User installed aarch64 + rv64 cross gcc and qemu-user.  Built
`poc/hash_portable.c` (NO intrinsics, one plain-C source) to PROVE the
poly_c8_widen checksum is architecture-independent -- not just assert it -- and
to observe the auto-vectorizer per target.

### Setup

- Toolchains: `aarch64-linux-gnu-gcc`, `riscv64-linux-gnu-gcc` (both GCC 14.2);
  `qemu-aarch64`, `qemu-riscv64` (TCG) to run the cross binaries.
- `poc/hash_portable.c`: poly_c8_widen (the shipped one) + poly_c4 (32-bit ref)
  + xorsum (order-blind control) + memcmp; correctness (bitflip + wordswap) +
  streaming bench.  Compiles for all three ISAs from the SAME source.
- Builds: x86 `-O2`; aarch64 `-O2 -static`; rv64 `-O2 -static` (rv64gc, scalar);
  rv64 `-march=rv64gcv -mabi=lp64d` (RVV).

### Result: correctness is IDENTICAL on every ISA

| target | poly_c8_widen auto-vec | bitflip / wordswap | verdict |
|--------|------------------------|--------------------|---------|
| x86-64 (SSE2) | pmuludq, 16-byte | 1 / 1 | PASS |
| aarch64 (NEON) | NEON, 16-byte | 1 / 1 | PASS |
| riscv64 (RVV) | RVV, 16-byte | 1 / 1 | PASS |
| riscv64 (rv64gc baseline) | scalar (no V ext) | 1 / 1 | PASS |

xorsum control: 1 / 0 (blind) on ALL three -> the word-swap test is meaningful,
not vacuous, so the PASSes are real.

### What we learned

- **The widening polynomial is truly ISA-independent.**  Same digest, same
  order-sensitivity on x86, ARM, and RISC-V.  The math (position-weighted,
  mod 2^64) simply does not depend on the ISA.
- **It auto-vectorizes wherever a widening vector multiply exists**: NEON
  (umlal), RVV, SSE2 (pmuludq) -- all picked up at plain `-O2`.  On rv64gc (no
  V extension) it is scalar but still correct and still beats memcmp.  So the
  SAME source is SIMD on the H618 (NEON) with zero code changes -- the whole
  reason we moved off the SSE4.1 intrinsic.
- **qemu-user TCG timing is emulation, NOT real-silicon perf** (the A53 is
  in-order; the RV64 core is different).  Only correctness and the
  vectorization report transfer; absolute ns/page must be re-measured on real
  boards.  (Native x86 ordering was preserved under TCG: poly_c8_widen the
  fastest order-sensitive hash, beating memcmp on every ISA.)
- **RV64 handles the 32->64 widening natively even without RVV** (RV64 `mul` on
  zero-extended 32-bit words), so there is no correctness/portability gap on
  baseline rv64 -- only less SIMD.

### Files Changed

| File | Purpose |
|------|---------|
| rig `poc/hash_portable.c` (new) | cross-ISA validation bench (no intrinsics); correctness + auto-vec evidence |

### Commits (rig `~/tmpfs-compress-test`) -- bisectable, each leaves the test suite building

- `run_qemu: add QEMU_CPU override (default qemu64...)`
- `poc: hash_bench -- add pure-C poly_c_single/poly_c4 + SSE4.2 crc32 guard`
- `poc: add hash_sse2 -- widening polynomial SIMD on SSE2 (pmuludq, no SSE4.1)`
- `tests: test_matrix read-verify -> poly_c8_widen (SSE2/NEON widening polynomial)`
- `gitignore: ignore hash_sse2/hb_* binaries + generated images/`
- `poc: add hash_portable -- cross-ISA proof of the widening checksum (aarch64/rv64)`

### Next Steps

- Real perf re-measure on the H618 (NEON) and, if available, an RV64 board --
  TCG numbers do not count.
- Re-run the rig thread-scaling sweep with poly_c8_widen vs the old memcmp/poly_c4.

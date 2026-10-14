---
name: read-verify-checksum
description: poc/hash_bench -- which ALU-only per-page checksum replaces the memcmp read-verify; finds the hoisting trap (memcmp reported 818 GB/s), lands on a position-weighted polynomial hash (SIMD + order-sensitive) at 1.5x memcmp speed
project: linux
previous: 2026-10-14_matrix-thread-scaling.md
---

# Mission Log: The Read-Verify Checksum (hash_bench PoC)

Part 43 of the tmpfs transparent-compression series.  Session 53
(2026-10-14).  A standalone host PoC (poc/hash_bench.c) to pick the
ALU-only per-page checksum that replaces the memcmp-per-page read verify
in test_matrix.  Follows the audit that found the memcmp verify reads TWO
memory regions (the just-read page AND the expected pagepool slot), i.e.
double cache pressure + source bandwidth over the timed read path.

## What We Did

Built `poc/hash_bench.c`, a host-side microbenchmark over the real ~2:1
compressible page texture, measuring: xorsum (`s+=v; x^=v`), xorsum2
(`a+=v; c^=a` -- order-sensitive), SSE2/AVX2 vectorized xorsum, FNV,
murmur3, crc32 (u8/u64), a position-weighted polynomial (poly32), and
memcmp.  Host: Xeon E5-1650 v2 @ 3.5 GHz, SSE4.2+AVX, NO AVX2 (Ivy
Bridge -- the AVX2 variant compiles out via `__builtin_cpu_supports`).

### The hoisting trap (three attempts before trustworthy numbers)

The first two runs reported memcmp at **818 then 938 GB/s / 4-5 ns per
page** -- physically impossible (exceeds L1 bandwidth at 3.5 GHz).  Cause:
the checksum/compare loop hashed a FIXED buffer against a FIXED reference,
so every iteration returned the same value and **GCC hoisted the whole
call out of the timed loop**.  A `asm volatile("" ::: "memory")` barrier
did NOT stop the memcmp builtin.  The only trustworthy loop is a
**STREAMING** one -- each iteration processes a DIFFERENT page, so nothing
is loop-invariant and nothing can be hoisted.  This mirrors the earlier
PRNG-in-the-write-path bug: a measurement that looks too good usually is.
Also fixed an arithmetic bug (GB/s multiplied by REPS while the timed
window covered one rep -> 3x inflation; ns/page was the correct column).

### Results (streaming, 64 MiB, ns/page | GB/s | bitflip | wordswap | SIMD)

| candidate | ns/page | GB/s | bitflip | wordswap | SIMD |
|---|---|---|---|---|---|
| **poly32_sse2** | 336 | 12.2 | Y | **Y** | Y |
| xorsum_sse2 | 290 | 14.1 | Y | **blind** | Y |
| xorsum (scalar) | 370 | 11.1 | Y | blind | - |
| xorsum2 (scalar) | 377 | 10.9 | Y | Y | - |
| crc32u64 | 463 | 8.9 | Y | Y | - |
| fnv | 592 | 6.9 | Y | Y | - |
| murmur | 594 | 6.9 | Y | Y | - |
| memcmp | 508 | 8.1 | Y | Y | - |

(Numbers vary run-to-run ~5%; the ordering is stable.)

## What We Learned

### Technical

- **xor+sum wins on speed because it is associative+commutative**: the two
  accumulators (sum, xor) are independent, so the loop vectorizes to a
  horizontal reduce (16 B/cycle SSE2, would be 32 AVX2).  FNV/murmur/crc32
  are SERIAL dependency chains (`h = (h^w)*P` -- each word depends on the
  prior h, multiply ~3 cycle latency, no SIMD), stuck at 1 word / ~2-3
  cycles.
- **Plain xor+sum is ORDER-BLIND**: it depends only on the multiset of
  8-byte words, not their order.  Verified: swapping two words yields the
  SAME checksum (`wordswap_caught=0`).  The blindness is not specific to
  8-byte adjacency -- ANY permutation of the 512 words is invisible.  For
  this use case that is a THEORETICAL miss (a decompression bug produces
  garbage bytes, not a clean permutation), but it is a real property.
- **xorsum2** (`a+=v; c^=a`, xors the RUNNING sum) is order-sensitive and
  catches the word swap, at scalar speed (377 ns).
- **The construction that achieves BOTH SIMD and order-sensitivity is a
  position-weighted polynomial**: `h = SUM_i w_i * K^i (mod 2^32)`, 4x32
  words at a time via `_mm_mullo_epi32`, with a precomputed K^i table (4
  KiB, L1).  Each word's WEIGHT depends on its index (breaks
  order-blindness) but each term is computed independently (no serial dep
  -> vectorizes).  Caught bitflip AND word swap at 336 ns / 12.2 GB/s.
- **User's candidate formulas, evaluated**: `x = (x+v)*prime ^ v` is a
  serial rolling hash (no SIMD, slower than plain xorsum).  `x*v+(prime^v)`
  with constant x is still order-blind after the AC reduce (each word's
  contribution independent of position).  A constant seed XORed into the
  accumulator does NOT break order-blindness -- that is a property of the
  REDUCTION, not the init.  Only a POSITION-DEPENDENT weight does.
- **poly32_sse2 is 32-bit accumulation** (mod 2^32): ~1-in-4e9 collision
  per page, fine for a corruption check; for more margin run two
  independent polynomials (different K) and XOR, at ~2x cost.

### Process

- **A measurement that looks too good usually is.**  Both impossible
  memcmp numbers (818, 938 GB/s) were hoisting artifacts.  The fix was
  always the same: make the loop non-invariant (stream different data per
  iteration).  Third occurrence of this class of bug in the session (with
  the PRNG-in-write-path and the vacuous phase gate) -- worth a standing
  rule.
- Building the AVX2 variant with a `target(\"avx2\")` attribute + runtime
  `__builtin_cpu_supports` guard kept the rest of the binary AVX2-free, so
  the PoC runs on this no-AVX2 host without SIGILL (the first attempt
  died exit 132).

## What We Decided

- **Wire poly32_sse2 into test_matrix** as the read-verify checksum: it
  is order-sensitive (catches the word swap plain xor+sum misses),
  SIMD-fast, and 1.5x faster than the memcmp it replaces (336 vs 508
  ns/page), single-region vs two-region.  The 16% cost over order-blind
  xorsum_sse2 is worth the robustness.
- The full-verify semantics (badpages counter) stay; only the per-page
  comparison primitive changes from memcmp to a precomputed poly32 table.
- Trust the ns/page column and the STREAMING loop; treat any in-place
  checksum/compare microbench as hoist-suspect until proven streaming.

## Files Changed

| File | Purpose |
|------|---------|
| rig `poc/hash_bench.c` (new) | standalone checksum microbench: candidates, streaming harness (anti-hoist), correctness checks (bitflip, word swap) |

## Test Results

- Streaming 64 MiB, ns/page: poly32_sse2 336, xorsum_sse2 290, xorsum2
  377, crc32u64 463, memcmp 508, fnv 592, murmur 594.
- Correctness: xorsum catches bitflip but is blind to word swap;
  xorsum2 and poly32 catch BOTH; poly32 is the only SIMD one that does.
- AVX2 variants skipped (host lacks AVX2); SSE2 numbers are the reference.

## What We'd Do Differently

- Would have written the STREAMING harness from the start -- the fixed-
  buffer loop invited GCC hoisting and cost two bogus runs (memcmp at
  818/938 GB/s) before the impossible numbers flagged it.  Rule of thumb
  for any microbench: if a number exceeds physical L1 bandwidth, suspect
  the harness before suspecting the CPU.
- Would have measured the SIMD-vs-serial distinction FIRST (it is the
  architectural reason xor+sum wins) rather than building 8 candidates
  and sorting them empirically.  The associative/commutative property
  predicts the winner without a benchmark.
- Would have included the position-weighted polynomial in the FIRST batch
  -- it was only added after the user asked whether SIMD+order-sensitivity
  was possible.  It turned out to be the best answer; it should have been
  an obvious candidate alongside xor+sum.
- The correctness test (bitflip/wordswap) was added late; it should ship
  with any checksum benchmark from the start, since speed without a
  robustness check is half the question.

## Next Steps

1. Wire poly32_sse2 into test_matrix's reader (precompute pool_hash in
   build_pagepool; replace memcmp with hash==table[slot]); re-run the
   thread-scaling sweep to confirm the read-path verify overhead drops.
2. Standing items unchanged: the plateau mechanism (lockstat), CPU-freq
   confound, cliff-churn matrix, workload phase 1, RFC assembly.

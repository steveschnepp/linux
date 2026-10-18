---
name: compression-algo-availability-sweep
description: sweep which crypto-API compression algos ztmpfs supports on i386-pae / amd64 / arm32 -- all 7 zram algos work on all three; zlib is not a registered acomp anywhere
project: linux
previous: 2026-10-18_mount-options-sysfs-file.md
---

# Mission Log: Compression-Algo Availability Sweep

Part 58 of the tmpfs transparent-compression series.  Same day as the
mount_options log (previous).  A measurement session: the engine picks
its algorithm through the crypto API (`compress=` -> `crypto_has_acomp`),
so which algorithms it can actually use is a per-arch, per-config
question.  Swept it on three architectures.

## What We Did

Added a `test.mode=algos` sweep to the cross-arch guest
(`arch_init.c`) that mounts tmpfs with each candidate crypto-API
compression algorithm and reports OK / UNAVAILABLE per algo, then ran it
on i386-pae, amd64 (x86_64), and arm32.  The three kernels were built
with all the crypto compression configs enabled; amd64 was added to the
arch-guest rig (it previously covered i386/arm/riscv/mips but not
x86_64), and the kernel builder stopped referencing the removed
`CONFIG_ZCELL`.  No kernel change -- the sweep measures existing
behavior.

## How it's measured

`shmem_comp_opts_set_algo()` rejects an unavailable algorithm with
`-EINVAL` (the mount fails), so a successful `mount -o compress=<algo>`
means `crypto_has_acomp()` passed and the engine can use it.  Added a
`test.mode=algos` to the cross-arch guest (`arch_init.c`) that loops over
candidate algos, mounts tmpfs with each, and reports OK / UNAVAILABLE.
No sysfs needed -- the mount result is the ground truth.

## Result: 7 of 8 algos, identical on all three arches

| algo | i386-pae | amd64 | arm32 |
|---|---|---|---|
| lz4 | OK | OK | OK |
| lz4hc | OK | OK | OK |
| lzo | OK | OK | OK |
| lzo-rle | OK | OK | OK |
| zstd | OK | OK | OK |
| deflate | OK | OK | OK |
| 842 | OK | OK | OK |
| zlib | **UNAVAILABLE** | **UNAVAILABLE** | **UNAVAILABLE** |

The kernels were built with all the crypto compression configs enabled
(`CRYPTO_LZ4/LZ4HC/LZO/ZSTD/DEFLATE/842`).  `zlib` is not a registered
crypto `acomp` anywhere, so it is unsupported on every arch.

## What it means

- **ztmpfs supports the full zram algorithm set** (lz4, lz4hc, lzo,
  lzo-rle, zstd, deflate, 842) -- the same seven zram exposes -- and the
  coverage is **arch-independent**.  The crypto-API route (vs zram's
  native `lib/` backends) costs some throughput but does not cost
  algorithm breadth: any crypto `acomp` works, uniformly.
- This closes the open question from the "crypto API vs native lib"
  discussion: the generality the crypto API buys is real (the whole zram
  set, selectable per mount via `compress=`), and `zlib` is the only
  gap (no crypto `acomp`).

## Rig changes (harness commit `f19292c`)

- `arch_init.c`: the `algos` sweep mode (plus earlier this arc: the
  `oom` no-madvise bench, `merge_pages` knob, a 32-bit varargs segfault
  fix, a `cmdline_val`-buffer snapshot fix).
- `run_arch.sh` / `pack_initramfs.sh`: added **amd64 (x86_64)** as an
  arch-guest target (the flow previously covered i386/arm/riscv/mips but
  not x86_64), plus sub-name patterns.
- `build_arch_kernel.sh`: dropped `CONFIG_ZCELL` (the device was removed
  from the kernel earlier this arc).

## What We Learned

- **The crypto API's algorithm coverage matches zram's native set** on
  the architectures that matter -- so "crypto overhead for generality"
  did not actually reduce the algorithm choices available to ztmpfs.
- **`zlib` has no crypto `acomp`** -- it is the one algorithm zram could
  theoretically add that ztmpfs cannot, and it is not in zram's set
  either, so the practical coverage is identical.

## What We Decided

- **No kernel change** -- this is a measurement of existing behavior.
- **amd64 added to the arch-guest rig** so cross-arch sweeps cover the
  x86-64 target alongside i386/arm.

## Rules Added

- **To test "can the engine use algo X", mount with `compress=X`** and
  read the result -- `set_algo` turns `crypto_has_acomp` failure into a
  failed mount, so no sysfs probing is needed.

## What We'd Do Differently

- Nothing material; the mount-result trick kept the test to a few lines
  per arch and directly answered the question.

## Files Changed

| File | Purpose |
|------|---------|
| harness `tests/arch_init.c` (`f19292c`) | +`algos` sweep mode |
| harness `scripts/run_arch.sh` | +amd64 arch-guest case, sub-name patterns |
| harness `scripts/pack_initramfs.sh` | +amd64, sub-name patterns |
| harness `scripts/build_arch_kernel.sh` | -`CONFIG_ZCELL` (device removed) |

(Kernel tree unchanged -- the sweep measures existing behavior.)

## Test Results / Benchmarks

- i386-pae, amd64, arm32: lz4 / lz4hc / lzo / lzo-rle / zstd / deflate /
  842 all `OK`; zlib `UNAVAILABLE` on all three.  Identical coverage.

## Next Steps

1. Standing ztmpfs backlog: H618 re-measure, RFC assembly, mTHP folio
   grouping, workload bed (spec 05), BE runtime boot.
2. `shmem_compress.c` split (deferred from the previous log): 5 files +
   `shmem_compress_internal.h`.
3. If a native fast-path is ever pursued for throughput, lz4/zstd are the
   natural first candidates (the common defaults) -- but note the algo
   set itself is already complete.

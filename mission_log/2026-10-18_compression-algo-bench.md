---
name: compression-algo-bench
description: benchmark the 7 supported compression algos on ztmpfs (munin-2.0.78 realistic data) across i386-pae/amd64/arm32 -- lz4/lzo fast at ~2x, zstd best ratio at ~4x slower, lz4hc/deflate/842 dominated
project: linux
previous: 2026-10-18_compression-algo-availability-sweep.md
---

# Mission Log: Compression-Algo Throughput + Ratio Bench

Part 59 of the tmpfs transparent-compression series.  Same day as the
algo-availability sweep (previous).  That sweep answered *which* algos
work; this one answers *how fast* and *at what ratio* -- the
speed-vs-ratio tradeoff that decides a sensible default and what to
recommend per workload.

## What We Did

Added `test.mode=algobench` to the arch guest: for each of the 7
supported algos, mount tmpfs eagerly with `compress=<algo>`, write the
tar's raw bytes repeatedly to fill 128 MiB (timed), read it back
(timed), and take the ratio from `mount_stats` `bytes=`.  Ran it on
i386-pae, amd64, and arm32.  (First switched the data source from the
synthetic fill to munin -- see below.)  No kernel change.

## Data: munin-2.0.78, not synthetic fill

The first cut used the rig's synthetic `fill_page` (~half PRNG, ~half
repeatable text) and produced absurd ratios (~150-200x) -- the
repeatable half compresses almost away, so the ratio was an artifact.
Switched to **munin-2.0.78.tar raw bytes** (5.4 MiB, 714 real files):
realistic ~2:1 data, so the ratios are meaningful.  The raw tar bytes
are the workload (no extraction needed).  Harness `f463be0`.

Method: for each algo, mount tmpfs `compress=<algo>` eager
(threshold=1, watermark=0:0), write the tar's bytes repeatedly to fill
128 MiB (timed), read it back (timed), and take the ratio from
`mount_stats` `bytes=` (logical/compressed).  `test.mode=algobench` in
the arch guest.

## Results

**i386-pae (KVM):**

| algo | write MB/s | ratio | read MB/s |
|---|---|---|---|
| lz4 | 285 | 1.95 | 1022 |
| lz4hc | 64 | 1.95 | 1130 |
| lzo | 319 | 2.23 | 854 |
| lzo-rle | 318 | 2.23 | 882 |
| zstd | 71 | 2.50 | 204 |
| deflate | 26 | 2.34 | 157 |
| 842 | 37 | 1.89 | 88 |

**amd64 (KVM):**

| algo | write MB/s | ratio | read MB/s |
|---|---|---|---|
| lz4 | 382 | 1.95 | 1319 |
| lz4hc | 70 | 1.95 | 1431 |
| lzo | 397 | 2.23 | 1038 |
| lzo-rle | 397 | 2.23 | 1095 |
| zstd | 98 | 2.50 | 301 |
| deflate | 30 | 2.34 | 200 |
| 842 | 44 | 1.89 | 127 |

**arm32 (TCG -- absolute MB/s is emulator speed, not native; only the
ordering and ratio transfer):**

| algo | write MB/s | ratio | read MB/s |
|---|---|---|---|
| lz4 | 24 | 1.95 | 49 |
| lz4hc | 5 | 1.95 | 49 |
| lzo | 25 | 2.23 | 52 |
| lzo-rle | 25 | 2.23 | 52 |
| zstd | 8 | 2.50 | 22 |
| deflate | 3 | 2.34 | 23 |
| 842 | 5 | 1.89 | 11 |

## What We Learned

- **Ratio is arch-independent**: identical to 2 decimals on all three
  (lz4/lz4hc 1.95, lzo/lzo-rle 2.23, zstd 2.50, deflate 2.34, 842
  1.89) -- a good sanity check; only speed varies.
- **lz4 / lzo / lzo-rle are the sweet spot**: fastest writes (285-397 on
  x86) at ratio ~2.0-2.2.  lzo edges lz4 on ratio (2.23 vs 1.95) at
  similar speed.
- **zstd buys the best ratio (2.50) at ~4x the write cost** of lz4/lzo.
- **Two algos are dominated on this data**:
  - **lz4hc == lz4 ratio (1.95) but ~5x slower to compress** -- lz4hc
    gives no ratio win here, so lz4 strictly dominates it.
  - **deflate is beaten by zstd on both axes** -- zstd is ~3x faster
    (98 vs 30) AND better ratio (2.50 vs 2.34).  deflate is strictly
    worse.
  - 842 is slowest-ish AND worst ratio (1.89) -- dominated by lz4/lzo.
- **Read (decompress) is fast for lz4/lzo/lz4hc** (~1000-1400 on amd64)
  and much slower for zstd/deflate/842 (127-301).

## What We Decided

- **lz4 (or lzo) is the sensible default** -- fast, ~2-2.2x.  zstd is
  the pick when ratio matters more than write throughput.  lz4hc,
  deflate, and 842 are dominated on realistic data and are not worth
  recommending (though they remain available via `compress=`).
- No kernel change; the bench lives in the arch guest
  (`test.mode=algobench`).

## Rules Added

- **Bench compression algos on realistic data (munin), not the synthetic
  fill** -- the synthetic fill's repeatable half inflates the ratio ~75x
  and hides the real tradeoff.
- **Ratio is arch-independent; only speed varies** -- so a ratio measured
  on one arch transfers, but throughput numbers must be per-arch (and
  TCG figures are emulator speed, not native).

## What We'd Do Differently

- Would have used munin from the start; the synthetic-fill ratio was a
  wasted first cut.

## Files Changed

| File | Purpose |
|------|---------|
| harness `tests/arch_init.c` (`f463be0`) | `algobench` on munin-tar raw bytes (write/read MB/s + ratio) |
| harness `scripts/pack_initramfs.sh` | carry munin-2.0.78.tar in the arch initramfs |

## Test Results / Benchmarks

See the three tables above (i386-pae, amd64, arm32-TCG).  Ratios
1.89-2.50, identical across arches; write 26-397 MB/s on x86, ~3-25 on
arm32-TCG (emulator).

## Next Steps

1. Standing ztmpfs backlog: H618 re-measure, RFC assembly, mTHP folio
   grouping, workload bed (spec 05), BE runtime boot.
2. `shmem_compress.c` split (deferred): 5 files +
   `shmem_compress_internal.h`.
3. Consider lz4-vs-lzo as the default if not already settled
   (CONFIG_TMPFS_COMPRESSOR_DEFAULT) -- the data favors lzo on ratio at
   equal speed, lz4 on decompress speed.

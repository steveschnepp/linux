---
name: zcell-verdict-tmpfs-pivot
description: the open-cell buffer is correct but loses to zram on random writes; tmpfs merge_pages is the real, free lever -- verdict, and the pivot to upstreamable tmpfs compression
project: linux
previous: 2026-10-10_ftl-open-cell-buffer.md
---

# Mission Log: The Zcell Verdict and the tmpfs Pivot

Part 19 of the tmpfs transparent-compression series.  Session 35
(2026-10-10).  Session 34 built the FTL open-cell write-gathering buffer
for cellzram (steps 1-4, all committed, all gates green).  This session
benches it apples-to-apples, reaches a verdict, and pivots.

The `previous` field above is same-day ordering only, not a semantic link
-- session 34's log and this one are independent entries the filename
date sorts.

## What We Did

1. **Benched the open-cell buffer apples-to-apples**: cellzram at
   ratio 16 (physical=64K, open_units=8) vs the ratio-1 baseline vs zram,
   on the same kernel, with the in-boot zram control cancelling drift.
   Throughput used the M-boot aggregator (M=5, median + IQR); RAM came
   from the fsdev `distinct` benchmark.
2. **Found and fixed three bugs** the compressible split test could not
   see.  The churn bench -- random overwrite of incompressible data --
   exposed all three; they are committed (`e961aac2f3cd`), gates re-run
   green.  Detailed under "What We Learned".
3. **Ran the tmpfs `merge_pages` sweep** (1..16) to see the same 16-page
   grouping on the tmpfs side, where the page cache absorbs writes.
4. **Reached a verdict and pivoted**: zcells-for-block-devices loses to
   zram; the grouping is the real lever and tmpfs gets it free; the next
   phase is pure tmpfs, targeting upstream.

## The verdict

Apples-to-apples, same kernel, zram as the in-boot control:

| workload | ratio 1 | ratio 16 + buffer | zram | winner |
|---|---|---|---|---|
| **churn** (random-overwrite throughput) | **118.4 MB/s** | 44.4 MB/s | ~139 MB/s | ratio 1 / zram -- buffer is **2.7x slower** |
| **distinct** (incompressible RAM) | 101.8% of orig | **100.1%** | 100.0% | buffer -- closes the entry-table gap |
| **untar** (sequential throughput) | ~20 MB/s | ~20 MB/s | ~20 MB/s | neutral (I/O-bound) |

tmpfs `merge_pages` sweep (seq 144-page dataset, zsmalloc):

| merge | entries | total_compressed |
|---|---|---|
| 1 | 144 | 407,987 |
| 8 | 18 | 405,128 |
| 16 | **9** | ~405,000 |

Entry count scales exactly `1/merge`; cross-page compression gain is real
but small (~0.7% on seq).

**The finding.**  The 16-page grouping is cheap and beneficial: 16x fewer
entries, marginally better compression.  The difference is *who pays for
the write path*:

- **tmpfs `merge_pages=16`** gets it **free** -- writes hit the
  uncompressed page cache and compress at reclaim.  That is the design's
  "piece 2 for free", now measured.
- **cellzram `physical=64K`** must **stage** it -- a block device has no
  page cache, so it builds one (the open-cell buffer) and pays **2.7x
  random-write throughput** for it.

So for the block device, **zcells loses to zram**: the RAM win is real but
tiny (100.1% vs 100.0%), the throughput cost is severe.  For tmpfs, the
grouping is the real lever and it is free.  The value is in tmpfs, not in
cellzram.

## What We Learned

### Technical

- **A block device has no page cache, so any write-gathering it does is a
  page-cache substitute that costs throughput.**  tmpfs amortizes
  compression across reclaim; a block device pays on every write.  The
  open-cell buffer is exactly such a substitute, and it works -- it is
  just not worth its throughput cost against zram.

- **A bench must exercise the data profile the feature targets, not just
  the code path.**  Three bugs survived the compressible, sequential split
  test and fell to the churn bench (random overwrite, incompressible):

  1. **gc_lock self-deadlock.**  `drain_staged` ran under `gc_lock` and
     called `shmem_comp_store()`, whose directory-exhaustion path
     re-takes `gc_lock` (a non-recursive mutex) to run the cell GC --
     same task, same mutex.  The drain now stores via `cell_store()`
     directly and never recursively GCs.
  2. **GC ordering.**  `cell_gc` drained staged buffers *before* the
     `drain_victim` compaction, so when the cell directory was full the
     staged store failed and the compaction that would make room ran too
     late -- one pass never converged and the write path starved for open
     slots.  `drain_staged` now runs *last*, after compaction.
  3. **Silent corruption on incompressible data.**  A drained
     incompressible unit is stored RAW (`length == unit_bytes`);
     `stage_open`'s seeding always *decompressed* the seed, so inflating
     a raw blob yielded garbage that leaked into the re-staged unit and
     every read was wrong.  `stage_open` now memcpy's a raw seed,
     matching `unit_load`.

  All three are the same lesson: correctness gates say green; only a bench
  on the target data profile says 2.7x (or, here, corrupt).

- **merge_pages entry count is exactly `1/merge`**; the compression gain
  from cross-page context is real but small on sequential data (~0.7%).
  The entry-table reduction is the primary win, and it is free on tmpfs.

### Process

- **Apples-to-apples on the same kernel with the in-boot zram control is
  what turned "the buffer works" into "the buffer loses."**  Every gate
  was green; only the throughput bench, measured against the ratio-1
  baseline and zram in the same boots, revealed the 2.7x.  Correctness
  and performance are different questions and need different benches.

- **The design predicted this and we built it anyway.**  The session-34
  spec named "random-write amplification" as the central trade and said
  the payoff must be proven on a compressible workload first.  We built
  the buffer before measuring that.  Measuring the unfavorable profile
  first would have saved the build.

## What We Decided

- **Verdict: park the open-cell buffer for cellzram.**  It is committed,
  correct (cellzram 106/0, split 24/0, fsdev 19/0, torture 231/0, randio
  4/0, tmpfs green), and dormant at default (`open_units=0`).  It is **not
  reverted** -- the series is append-only and the code plus its
  measurements are the record.  But it is not the path forward.

- **Pivot to pure tmpfs.**  The grouping (`merge_pages`) is the real
  lever and tmpfs gets it free.  The next phase focuses on tmpfs
  compression, where the value is.

- **Ultimate goal: upstream.**  The tmpfs transparent-compression work
  targets upstream inclusion.  This reframes everything -- API surface,
  code quality, documentation, and the measurement story must all meet the
  upstream bar, and the experimental cell/cellzram path must be separable
  from the upstreamable tmpfs+zsmalloc path.

## Rules Added

- **Bench the data profile before building the feature.**  If a design
  names a known-bad workload (here: random writes), measure that workload
  on a stub before implementing the full thing.  The prediction is cheap
  to confirm and expensive to ignore.

- **Green gates are not a performance verdict.**  Correctness and
  throughput need separate benches; only the latter, against a baseline
  and a control in the same boots, says whether a feature is worth it.

- **Never revert a mission's code to "clean up" the story.**  The
  verdict is a mission log, not a `git revert`.  Parked, correct, dormant
  code is part of the record.

## What We'd Do Differently

- Would have benched random-overwrite + incompressible churn on a stub
  staging path *before* building the open-cell buffer end to end.  The
  design's own "random-write amplification" warning was the signal; a
  half-day stub bench would have confirmed it and redirected to tmpfs
  sooner.

- Would not have needed three bug-fix commits found the hard way if the
  first bench had used the target data profile.  The bugs were real and
  worth finding, but they were found by accident rather than by design of
  the measurement.

## Files Changed

None this session -- analysis and benchmarking only.  The three bug fixes
were committed during the bench (`e961aac2f3cd`).  Reference:

| Artifact | Note |
|---|---|
| `mm/cellzram.c`, `mm/shmem_compress.c`, `include/linux/shmem_compress.h` | the open-cell buffer (session 34 + the session-35 fixes); parked, dormant at `open_units=0` |
| `mission_log/2026-10-10_ftl-open-cell-buffer.md` | the design/spec that built it |
| `~/tmpfs-compress-test/results/` | the M-boot `.summary`/`.rows` and fsdev/merge sweep outputs behind every number above |

## Test Results / Benchmarks

All standing gates green at the end of the bench (ratio 1 unless noted):

| gate | result |
|---|---|
| cellzram (ratio 1) | 106/0 |
| split (ratio 16) | 24/0 |
| fsdev (ratio 1 / ratio 16) | 19/0, 19/0 |
| torture | 231/0 |
| randio | 4/0 |
| tmpfs suite | green |

Throughput (M-boot, M=5, zram control): ratio 1 = 118.4 MB/s (86% of
zram), ratio 16 + buffer = 44.4 MB/s (32% of zram), zram = ~139 MB/s.
RAM (`distinct`): ratio 1 = 101.8%, ratio 16 = 100.1%, zram = 100.0%.
tmpfs merge sweep: 144 -> 9 entries across merge 1 -> 16.

## Next Steps

The tmpfs pivot.  In priority order:

1. **Scope the upstreamable slice.**  What is the minimal, reviewable
   tmpfs transparent-compression feature?  `merge_pages` grouping, the
   compression pool (zsmalloc backend), the mount options, the Kconfig.
   Separate it cleanly from the experimental cell/cellzram path so the
   upstream diff does not carry the cell allocator.
2. **Fix the known tmpfs merge bug before anything else.**  The
   `sweep_merge.sh` header records a slab-corruption (double-release in
   the decompress/eviction interplay) that detonates on a mount after a
   compress/umount cycle.  Upstream is impossible with that present; it is
   the first item.
3. **The measurement story for upstream.**  Upstream needs a compelling
   number, and it must be honest: a compressible-non-zero workload (the
   series' deferred open question -- text/binaries/logs, not zeros or
   incompressible synthetics) measured against uncompressed tmpfs, with
   the M-boot rigor.  The merge sweep is the RAM half; a throughput and
   compression-ratio story on real data is the missing half.
4. **Documentation to upstream bar.**  `Documentation/filesystems/tmpfs-
   compression.rst` exists but was never Sphinx-built (no docutils in the
   environment).  Before upstream, it must build clean and match the
   house style.
5. **Decide the cell/cellzram fate separately.**  It stays parked and
   dormant; whether it is ever revived, deleted, or kept as an
   out-of-tree experiment is a question for after the tmpfs upstream
   effort has a shape.

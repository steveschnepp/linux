---
name: zcell-oom-and-demand-gc
description: zcell loses the RAM-extension game to zram under OOM (whole-cell floor + alloc-side contiguity, not the inline or GC timing); per-pool demand GC committed as async infra; watermark-async + bvec grouping designed but not built
project: linux
previous: 2026-10-18_trres-phase-b-lost-wakeup.md
---

# Mission Log: zcell Under OOM, Demand GC, and the Merge Design

Part 53 of the tmpfs transparent-compression series.  Same day as the
trres fix (previous log).  This session: scoped bug fixes, a ztmpfs perf
regression A/B, then a deep dive into zcell's RAM-extension behavior
under OOM -- which produced a negative result, a committed async-GC
building block, and a merge design.

## What We Did

### 1. Scope: tmpfs/zcell bugs only

After the trres fix, the user narrowed the remaining work to
tmpfs/zcell.  VT/ucs_width_table.h (a stale generated artifact,
regenerated locally -- gitignored, nothing to commit) and the x86-boot
build bugs were set aside as out of scope.

### 2. ztmpfs perf regression A/B (before/after the perf work)

The perf work (per-CPU compression buffer `4a0c1f1`, dead `ktime_get`
removal `f1f2194`, entry-overhead trim `0e946ee`, loc-node/ptr-chase
extraction `f6cc4dd`, dense slot table `0fe6a70`) is all in the shared
engine and active at merge=1, so an A/B was run to check for
regression.

- Baseline `331a9ba94e51` (session 71) vs HEAD; i386-pae first, then
  amd64 (the i386 arch guest's perf mode was broken -- see below).
- Result: **no regression.**  Per-config write deltas -2.5%..+2.3%
  against a 5-10% host-noise band; read flat (+0.9%..+3.7%); RAM
  footprint bit-identical.  The merge=1 "control" showed an even larger
  spurious swing (+10.6%) than the treatment, proving the deltas are
  noise.

Three pre-existing i386-pae rig bugs surfaced and were fixed (they
blocked the bench, none caused by our kernel changes):
- **32-bit varargs segfault**: `printf("%lu", bytes >> 20)` passed a
  `uint64_t` to `%lu` (4 bytes on i386), shifting every later vararg --
  garbage `dt` and a segfault on the read line's `%s`.  Works by luck
  on 64-bit.  Fixed by casting to `unsigned long` (6 sites).
- **`sz` clobbered by the shared `cmdline_val()` buffer**: a later
  lookup reused the static buffer `sz` pointed at, turning
  `test.size=192` into `"8"`.  Fixed by snapshotting into its own
  buffer (the pattern the code already used for `mode`).
- `run_arch.sh` i386 case didn't match `i386-pae-*` sub-names.

Plus a `test.mergepages=` knob in the arch guest's perf mode so the
merge path is reachable on i386-pae.

### 3. zram vs zcell OOM bench (RAM extension, no madvise)

The pitch: we cannot win on perf, so can we win on features -- specifically
RAM extension under OOM (the fleet's pain: reclaim never converges before
OOM on 64M boxes).  Built `oombench` (arch_init.c): a forked child
allocates+dirty with **no MADV_PAGEOUT** (real apps never issue it;
swap-out is the kernel's own reclaim under pressure), detecting OOM-kill
and measuring peak anon / extension factor / rate / memory-returned.
RAM swept 64/96/128/192M on i386-pae.

Result (per RAM size, zram vs zcell):
- **zram**: extends RAM cleanly and repeatedly (>=512M both waves, no
  OOM) at 282-801 MB/s.
- **zcell**: extension rate 2-4x slower (139-383 MB/s), and collapses
  intermittently -- OOMs on the SECOND alloc wave (after free) at most
  sizes, down to 0.86x extension (peak 100M) at 116M.  zram never
  collapses.

### 4. Root-cause: not the inline, not GC timing -- contiguity + throughput

Tested the hypothesis that the wave-2 collapse was a side effect of the
dense-slot inline (removing the ptr indirection).  Built the pre-inline
kernel (`f6cc4dd096c1`) and re-swept: **pre-inline collapses just as
badly, worse at 64M (w1 peak 104M)**.  The collapse MOVES between
runs/kernels -- fragmentation/timing-dominated, not a structural
regression from the inline.  Rate even slightly improved post-inline.

The real limiter is the **alloc side** during natural reclaim:
- zcell needs **contiguous order-4 (64K) cells**; under a tight box,
  buddy fragmentation makes these fail -> `cell_store` cannot swap the
  page out -> reclaim cannot free anon -> OOM.  GC returning old cells
  does not help allocate new contiguous ones.
- Plus throughput: zcell's compress+store is 2-4x slower, so a fast
  allocating child can outpace reclaim -> OOM.  zram's order-0 zsmalloc
  needs no contiguity and is faster -- immune to both.

Collapses occur on **wave 1** (before any free), so free-side GC cannot
address them.  This corrected the initial "GC loses the race to OOM"
hypothesis: GC timing is secondary; contiguity + throughput dominate.

### 5. Per-pool demand-triggered cell GC (committed)

Even though GC is not the OOM silver bullet, the async on-demand
machinery is worth having (returns memory promptly on free, and is the
infra the swap-out watermark will drive).  Implemented a per-pool
`cell_gc_work` delayed_work: armed from `shmem_comp_loc_dispose` (the
unified free choke point for device + tmpfs), coalescing + debounced
(20ms), leading-edge, kworker context.  Replaces the fixed 1Hz GC
cadence (the per-mount `maintain()` drops its interleaved `cell_gc()`;
keeps only page aging).  Commit `ac718642b597`.

Verified: i386 compile gate 0/0; OOM sweep on the new kernel shows no
regression (84M clean), but does NOT fix the collapse (as the
root-cause analysis predicted -- it is alloc-side).

### 6. Merge on swap zcell without mTHP (design only)

Extensive design discussion on getting the merging (cross-page ratio)
differentiator without mTHP:
- The swap layer emits **one multi-page bvec per large folio**
  (`swap_add_folio`: `bvec_set_folio(..., folio_size(folio))`).  That
  bvec is the grouping signal and is **mTHP-independent** -- it exists
  whenever any large folio is swapped.  mTHP only raises the hit rate.
- Device-side staging of base-page swap writes gives **no ratio win**:
  swap slots are assigned on demand, slot order != memory adjacency, so
  grouped unrelated pages share no redundancy (the grouping trap).
- **Watermark for async/sync**: reuse spec06 (`shmem_comp_ram_usage_pct`
  is global; hot -> compress sync for immediate relief, chill -> stage +
  compress async 5-20ms; the highwater kick `mod_delayed_work(...,0)`
  flushes staging when pressure rises, closing the transient-staging-
  memory hole).
- **GC-time regrouping** (uncompress+recompress adjacent per-page blocks):
  rejected -- redundant with compress-time bvec grouping, and impossible
  for base pages (no adjacency signal).

None of the watermark-async-staging or bvec-grouping is implemented --
it is the deferred "commit 3" the logs kept deferring, and it is
substantial (staging buffers, sync/async paths, group read/free
bookkeeping where 1 slot != 1 block).

## What We Learned

### Technical

- **zcell does not win RAM extension over zram under OOM.**  The
  whole-cell design's contiguity requirement (order-4 cells) fails under
  buddy fragmentation during natural reclaim; zram's order-0 zsmalloc is
  immune.  The whole-cell RETURN (memory back on free) is real but does
  not compensate -- cells only return when completely empty, and under
  churn they rarely are in time.
- **The OOM collapse is alloc-side, not free-side GC timing.**  Collapses
  on wave 1 (before any free) prove it.  Contiguity + throughput, not GC,
  dominate.
- **The dense-slot inline is NOT the collapse cause.**  Pre-inline
  collapses worse at 64M; the collapse is intermittent and moves.
- **A failed instrument localizes the bug class.**  The trres hang
  diagnosis: a `folio_trylock`-retry instrument cured the hang (0-in-60
  vs 1-in-12 uninstrumented), which is only possible for a LOST WAKEUP
  (a leaked lock would still hang the retry loop).  Read which symptom an
  instrument erases.
- **The entry-race drain rule applies to ANY page-cache-exit unlock.**
  The group swap-out path was missing `wake_up_folio_waiters`; that was
  the trres Phase-B hang.
- **32-bit varargs**: `%lu` fed a `uint64_t` is a silent portability bug
  on i386 (4-byte `unsigned long`); it shifts all later varargs and can
  segfault on a `%s`.  Cast or use `PRIu64`.
- **`cmdline_val()` returns a shared static buffer** -- snapshot before
  the next lookup clobbers it.
- **Merge/grouping needs the memory-adjacency signal** (a large-folio
  bvec).  Base-page swap slots carry none, so no device-side staging or
  GC regrouping can produce a ratio win for them.

### Process

- **Distinguish "the fix worked" from "the hypothesis was wrong."**  The
  demand GC is correct and useful, but the OOM collapse is elsewhere
  (contiguity).  Reporting the negative result honestly is more valuable
  than claiming the fix landed.
- **A/B against the suspect commit** (pre-inline vs post-inline) settles
  "did our change cause this" cleanly.

## What We Decided

- **trres Phase-B fixed** (commit `7d62b2f8bf0b`), 48/48 clean.
- **matrix_watermark is not a kernel bug** (flaky positive assertion +
  test-rig noise; negative invariant and data integrity pass).
- **ztmpfs perf: no regression** across the perf-work span.
- **zcell OOM: demand GC committed** (`ac718642b597`) as async infra,
  but the real fix is contiguity (smaller `cell_order`) / throughput --
  not GC.  Open as its own track.
- **Watermark-async staging + folio-bvec grouping: designed, NOT
  implemented** (deferred commit-3; substantial).  GC-time regrouping:
  rejected.

## Rules Added

- **Any path that deletes a folio from the page cache and then unlocks it
  must call `wake_up_folio_waiters()`** (the group swap-out was the
  missed site).
- **When an instrumented build stops reproducing, ask which symptom the
  instrument erased** -- that classifies lost-wakeup vs leaked-lock.
- **`%lu` with a `uint64_t` arg is a 32-bit varargs bug** -- cast to
  `unsigned long` or use `PRIu64`.
- **`cmdline_val()`-style shared buffers must be snapshotted** before the
  next lookup.
- **Swap merging needs the large-folio bvec signal**; device-side staging
  of base-page swap slots yields no ratio win (slot order != adjacency).

## What We'd Do Differently

- Would have built the OOM bench (no-madvise, natural reclaim) BEFORE
  concluding zcell's feature story -- the madvise-based swapbench hid the
  real reclaim-vs-OOM behavior.
- Would have A/B'd against the pre-inline commit at the FIRST sign of
  the collapse, rather than theorizing about the inline.

## Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` (`7d62b2f8bf0b`) | group swap-out: `wake_up_folio_waiters` drain (trres fix) |
| `mm/shmem_compress.c` (`ac718642b597`) | per-pool demand-triggered debounced cell GC (async infra) |
| `drivers/tty/vt/ucs_width_table.h` (gitignored, local) | regenerated to current merged format (VT=y compiles) |
| harness `tests/arch_init.c` | merge knob, oombench mode, 32-bit varargs fix, sz-snapshot fix |
| harness `scripts/run_arch.sh` | i386-pae-* case, TEST_MERGE / TEST_OOMWAVE knobs |

## Test Results / Benchmarks

- **trres Phase-B**: 1-in-12 hang -> 48/48 clean after the fix.
- **ztmpfs perf A/B (session71->HEAD)**: no regression; write deltas
  within the 5-10% noise band, read flat, RAM bit-identical.
- **OOM RAM sweep (i386-pae)**: zram extends RAM cleanly both waves
  (no OOM); zcell 2-4x slower and collapses intermittently on wave 2
  (and wave 1 at some sizes).  Not fixed by the demand GC (alloc-side).
- **i386 compile gates**: 0 errors / 0 warnings on every change.

## Next Steps

1. **Root-cause the zcell OOM contiguity**: sweep `cell_order` (0/2/4)
   on the device to test whether smaller cells remove the collapse.
   This is the actual RAM-extension fix.
2. **Watermark-gated sync/async staging + folio-bvec grouping** (the
   deferred commit-3): reuse spec06 `ram_usage_pct` + highwater kick;
   group a multi-page bvec as one `store_merged` unit; group read/free
   bookkeeping.  This is the merge differentiator, mTHP-optional.
3. Standing backlog: H618 re-measure, RFC assembly, mTHP folio grouping,
   workload bed (spec 05), BE runtime boot.

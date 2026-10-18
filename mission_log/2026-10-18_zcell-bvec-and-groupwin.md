---
name: zcell-bvec-and-groupwin
description: measured the two merge prerequisites -- bvec size (multi=0: no folio-grouping signal on base-page swap) and 5ms grouping-window ratio (25% win = pure frame-overhead artifact, ~0% real on real data); both gated debug instrumentation committed
project: linux
previous: 2026-10-18_zcell-oom-and-demand-gc.md
---

# Mission Log: Measuring the Merge Prerequisites (bvec size + grouping win)

Part 54 of the tmpfs transparent-compression series.  Same day as the
zcell-OOM log (previous).  A measurement session: before building the
swap-out merge (watermark-async staging + folio-bvec grouping), verify
the two empirical prerequisites -- (1) does the swap layer ever send a
multi-page bvec, and (2) does grouping a ~5ms batch of swap-out pages
actually beat per-page compression.  Both answers came back negative,
and both are now gated debug instrumentation committed for re-use.

## What We Did

### 1. bvec-size verification (CONFIG_ZCELL_DEBUG_BVEC)

Added instrumentation to `zcell_bio_write` counting the bio bvec size
distribution, gated behind a new `CONFIG_ZCELL_DEBUG_BVEC` (default n).
Ran the OOM bench on i386-pae.

Result: **~6.2M bvecs, multi=0, max=0.**  Every swap write is exactly
`PAGE_SIZE`.  The folio-grouping signal -- a multi-page bvec
(`bv_len > PAGE_SIZE`) meaning "one large folio arrived" -- never fires.
Base-page swap (no mTHP/THP) produces no large folios, so bvec-grouping
as designed would be dead code on this workload.  Confirms the merge
differentiator is large-folio-gated.  (Committed `6afb5dc74b19`.)

### 2. Grouping-window ratio A/B (CONFIG_ZCELL_DEBUG_GROUPWIN)

The bvec result raised the obvious follow-up: even without a large-folio
bvec, could async swap-out staging group a ~5ms batch of base-page
writes for a ratio win?  Added a second gated instrument
(`CONFIG_ZCELL_DEBUG_GROUPWIN`): buffer a ~5ms / 128-page window of
swap-out writes, compress each page individually (ratio-1 baseline) vs
the whole window as one block, log the win per window.

Result: 1904 windows, a consistent **~25% win**.  Decomposing it was the
real finding: `indiv=3456` for 128 pages = **27 B/page**; `grouped=2568`
= 20 B/page; the delta `888 B` ~= **127 extra lz4 frames x ~7 B/frame**.
The entire win is **per-frame header amortization, not cross-page
redundancy**.  And it only looks like 25% because the oombench pages are
pathological -- the child dirties ONE byte per 4K page (rest zeros), so
each compresses to ~27 B and the ~7 B frame header is 26% of it.  On
real ~2KB anon pages that header is ~0.35%, so the grouping win
collapses to ~0.35%.  Grouping 5ms of base-page swap buys essentially
nothing.  (Committed `dd00007d8bb2`.)

### 3. Debug-gate hygiene

Both instruments are default-n Kconfig options, compiled out entirely
unless enabled (verified: gate-off `zcell.o` has zero debug strings;
gate-on has them; i386 compile gate 0/0 both ways).  Fixed one
implementation slip -- the grouping functions were initially nested
inside `zcell_bio_write` (invalid storage class); moved to file scope.

## What We Learned

### Technical

- **The folio-grouping signal does not exist on base-page swap.**  Zero
  multi-page bvecs across 6.2M writes.  The merge design is correct but
  has nothing to act on without large folios (mTHP/THP or a 64K-page
  arch).
- **A grouping-ratio bench must use realistic data.**  The oombench
  near-zero pages (1 byte dirtied/page) made per-frame lz4 overhead 26%
  of the compressed size, inflating a "grouping win" that is really just
  header amortization.  Decomposing indiv-grouped into per-frame pieces
  exposed it.  A trustworthy win number needs ~2KB real anon pages
  (swapbench's fill_page data), where the frame floor is ~0.35%.
- **"Grouping win" and "frame-overhead amortization" are easy to
  conflate** -- a 25% win on 27 B pages is the same 7 B/frame that is
  0.35% on 2 KB pages.  Always normalize by the per-page compressed size
  before claiming a redundancy win.

### Process

- **Measure both prerequisites before building the feature.**  Two cheap
  gated instruments answered "is there a signal" and "is there a win"
  -- both no -- saving the watermark-async + bvec-grouping build (the
  deferred commit-3) from being written against a nonexistent signal.
- **Gate debug instrumentation behind default-n Kconfig, verify
  compiled-out**, so the hot path is untouched unless explicitly enabled.

## What We Decided

- **Swap-out merging is not worth building for base-page swap**: no
  multi-page bvec (signal) and ~0 grouping win (redundancy).  It only
  pays if large folios appear (mTHP/THP/64K-page), which is the lever
  the fleet work was trying to avoid.
- **Both debug instruments kept** (gated, committed) for re-running on a
  THP-enabled or 64K-page kernel where large folios might show up.
- The watermark-async + bvec-grouping design stands but is now parked
  behind "does a large-folio signal ever appear," not "build it."

## Rules Added

- **A grouping/compression-ratio win must be normalized by per-page
  compressed size**, or per-frame header amortization masquerades as
  cross-page redundancy (a 25% win on 27 B pages = 0.35% on 2 KB pages).
- **Debug kernel instrumentation goes behind a default-n Kconfig and is
  verified compiled-out**, so the production hot path is untouched.

## What We'd Do Differently

- Would have run the grouping measurement on realistic (~2:1) data from
  the start instead of the oombench near-zero pages -- the first number
  (25%) was misleading until decomposed.

## Files Changed

| File | Purpose |
|------|---------|
| `fs/Kconfig` | +`CONFIG_ZCELL_DEBUG_BVEC`, +`CONFIG_ZCELL_DEBUG_GROUPWIN` (both default n) |
| `mm/zcell.c` (`6afb5dc74b19`) | bvec-size distribution counters + report (gated) |
| `mm/zcell.c` (`dd00007d8bb2`) | ~5ms grouping-window ratio A/B: individual vs one-block (gated) |

## Test Results / Benchmarks

- **bvec size (i386-pae OOM bench)**: 6.2M bvecs, multi=0 max=0 -- all
  PAGE_SIZE, no folio-grouping signal.
- **Grouping window**: 1904 windows, avg win 25%; decomposes to ~7B x
  127 frames (frame overhead), ~0% real redundancy on real-size pages.
- **i386 compile gate**: 0/0 with each debug config enabled and disabled;
  gate-off `zcell.o` carries zero debug strings.

## Next Steps

1. If a large-folio signal is ever needed: re-run both instruments on a
   **THP-enabled** kernel or a **64K-page aarch64** kernel, where
   multi-page bvecs / real grouping redundancy might appear.
2. Re-run the grouping-window ratio under **swapbench** (realistic ~2:1
   data) for a trustworthy win number, if the oombench artifact needs
   confirming on real pages.
3. The real zcell RAM-extension lever remains **`cell_order`** (alloc-
   side contiguity), unrelated to merge -- open from the previous log.
4. Standing backlog: H618 re-measure, RFC assembly, mTHP folio grouping,
   workload bed (spec 05), BE runtime boot.

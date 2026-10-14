---
name: two-table-phase3-write-invalidate
description: phase 3 of the two-table mapping -- write-invalidate the stale entry at modify time + stop the aging pass re-compressing clean both-state caches; both z backends confirmed intact
project: linux
previous: 2026-10-13_two-table-phase2-read-back.md
---

# Mission Log: Two-Table Phase 3, Write-Invalidate and Cache Churn

Part 30 of the tmpfs transparent-compression series.  Session 46
(2026-10-13).  Phase 3 of the two-table mapping: establish the invariant
"a dirty folio has no compressed backing" and stop the aging pass
wastefully re-compressing pages that are already compressed.  A short,
contained session -- the hard reclaim work was Phase 2; this is the
spec's write-invalidate plus a now-stale policy in the aging path.

Everything is committed; the kernel tree is clean at Phase 3; the
in-tree kernel is back to KASAN.

## What We Did

### 1. Write-invalidate at the write, not at the recompress

The spec (specs/02, "Invalidation on modify") says a write to a folio
whose `entries[N]` is set makes that compressed copy stale, so the write
frees the entry and the dirty folio becomes the sole copy.  Before this
session, that free happened only at the *next re-compress* (the
compress-path choke point), so a modified both-state folio carried a
stale entry until write_end's eager recompress (write path) or the aging
pass (mmap path) got to it.

Added `shmem_comp_invalidate_write()`, called from `shmem_write_begin()`
after a folio is obtained for `SGP_WRITE`: it frees `entries[index]` (via
the existing `shmem_comp_invalidate_index`) and drops the matching
swapped charge.  Process context (i_rwsem held), so the entry teardown
and the recalc are safe.  Placed after the hwpoison check so a failed
begin does not free the backing before `write_end` commits.

The user's framing captured why this is the natural model: once the
entry is freed, the folio is a sole copy -- *exactly like a freshly
written page* -- and the existing machinery takes over: write_end's eager
recompress (hot mode) or the aging pass (deferred/chill) compresses it
and disposes the folio.  No new lifecycle; the invalidated page re-enters
the well-trodden write path.

### 2. The aging pass no longer re-compresses clean both-state caches

`shmem_comp_compress_one()`'s comment still said "the aging pass allows
clean pages: a decompressed-on-read survivor is exactly the cold data
worth re-compressing."  That was true in Phase 1, when decompress *freed*
the entry -- a decompressed page was genuinely uncompressed.  In the
both-state model the entry *survives*, so a clean decompressed folio is
already compressed: `entries[index]` names its durable copy and the folio
is just a reclaimable cache of it.  Re-compressing it frees that entry
and stores an identical one -- pure churn for no memory gain.

Added a skip in `shmem_comp_compress_one()`: if the pass is the aging
pass (`allow_clean`), the folio is clean, and `entries[index]` is set,
skip.  A *dirty* both-state folio's entry IS stale and worth
re-compressing (the choke-point invalidate handles it), so only clean
caches are skipped.  This makes clean both-state caches stable: they
persist as caches until truncated or reclaimed, rather than being churned
every aging round.

### 3. Confirmed the z backend is still selectable and works

The user asked to ensure the compressed data's backend -- zsmalloc or the
cell backend -- remains choosable.  It is: the `cell=<size>` mount option
selects the cell backend (there is no `mergecell=`; the option is
`cell=`), the default is zsmalloc, and the change is backend-agnostic
(entries/invalidate/truncate all operate on the per-inode entry table;
only the blob load/store dispatches on `shmem_comp_pool_uses_cells`).

Verified on both:
- **KASAN suite**: green, and it exercises both backends -- the default
  tests use zsmalloc, the cell tests (`cell=64K`) cover
  store/GC/index-race.
- **zsmalloc read-back gate**: 3G/2G fill + plain `read()` green.
- **cell backend**: its KASAN tests pass, and a 1.5G fill + read-back on
  arm 3 is green (CRC ok).  Its 3G fill still OOMs -- the pre-existing
  cell-capacity limit recorded in session 43, not a regression from this
  work (the write-invalidate and aging-skip are both no-ops for the fresh
  writes a fill performs).

## What We Learned

### Technical

- **A modified both-state folio is just a fresh write once invalidated.**
  Freeing the stale entry at `write_begin` hands the page back to the
  ordinary write path (eager recompress at write_end, or aging), so the
  write-invalidate adds no new state machine -- it *removes* one special
  case.

- **The aging pass's "re-compress decompressed-on-read survivors" policy
  was load-bearing in Phase 1 and became churn in Phase 2.**  A policy
  that was correct against one page lifecycle silently turns into waste
  when the lifecycle changes; the comment was the tell, and updating it
  mattered as much as the skip itself.

- **`shmem_write_begin` is the safe invalidate point; a dirty_folio hook
  is not.**  `folio_mark_dirty` can run in atomic context, and the
  invalidate needs the entry teardown plus a `shmem_recalc_inode` (which
  may block on quota).  write_begin is process context under i_rwsem, so
  it is both correct and simple.

- **The cell backend's 3G fill OOM is capacity, not correctness.**  It
  stores 64 KiB cells and runs out of room before zsmalloc does at the
  same logical size; the data path (store/decompress/GC) is fine, as its
  KASAN tests and the 1.5G read-back show.

### Process

- **Backend coverage is cheap to re-verify and worth stating explicitly.**
  The KASAN suite already mounts the cell backend for its cell tests, so
  "both backends pass" was one suite run, not a new harness.

- **Re-confirming the primary gate (zsmalloc) alongside the backend
  probe kept the two questions separate**: "does Phase 3 break the gate?"
  (no) and "does the cell backend still work?" (yes, modulo its
  pre-existing capacity limit).

## What We Decided

- **Write-invalidate lives in `shmem_write_begin`**, not a `dirty_folio`
  hook.  Process context, matches the spec's "the write frees the
  entry," and reuses the existing write lifecycle.

- **The aging pass skips clean both-state caches.**  It recompresses only
  dirty or uncompressed pages.  The choke-point invalidate stays, for the
  dirty-folio case.

- **No `release_folio`, no new a_ops hook.**  Consistent with Phase 2's
  minimal-surface conclusion: the LRU class (file vs anon) is what makes
  a decompression cache reclaimable, and the write lifecycle is what
  frees a stale one.

## What We'd Do Differently

- Would have caught the stale "re-compress a decompressed-on-read
  survivor" comment in Phase 2, when the both-state model landed.  It
  was the one place the aging path still encoded the Phase-1 lifecycle,
  and it survived two commits before this session noticed it.

- Would have probed the cell backend's read-back in Phase 2 alongside
  zsmalloc, rather than only confirming the zsmalloc gate.  The backend
  question was easy to answer and worth having in the record from the
  start.

## Files Changed

| File | Purpose |
|------|---------|
| `include/linux/shmem_compress.h` (`de75be7f4835`) | `shmem_comp_invalidate_write()` declaration |
| `mm/shmem.c` (`de75be7f4835`) | `shmem_write_begin` calls the write-invalidate after a successful get |
| `mm/shmem_compress.c` (`de75be7f4835`) | `shmem_comp_invalidate_write()` helper; aging-skip for clean both-state caches; comment corrected |

## Test Results

| gate | result |
|---|---|
| build (KASAN) | 0 errors, 0 warnings |
| full KASAN suite (zsmalloc default + `cell=64K` cell tests) | exit 0, all ALL PASS, 0 KASAN |
| 3G/2G read-back gate, zsmalloc (arm 2) | wrote 3072 MB, read 3072 MB, CRC ok, no OOM |
| cell backend 1.5G write/read-back (arm 3) | wrote 1536 MB, read 1536 MB, CRC ok |
| cell backend 3G fill (arm 3) | OOM during fill -- pre-existing cell-capacity limit (session 43), not a regression |

## Next Steps

1. **Phase 4**: remove the now-dead `SWP_SHMEM_COMPRESS` encoding +
   `{id,gen}` packers; the compressed mapping lives entirely in
   `entries[]` now, so the reserved swap type and its helpers are unused
   in the data path.  Re-assess the blast radius on this branch vs s1.
2. **Arm-3 capacity** (optional, separate track): the cell backend runs
   out of room before zsmalloc at the same logical size; worth a look if
   the cell tier is to carry large fills.
3. Standing items unchanged: M-boot the perf numbers, cell_order sweep,
   the arm-3 rm/synchronous-free story at 64M.

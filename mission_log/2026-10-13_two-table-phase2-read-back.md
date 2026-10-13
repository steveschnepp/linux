---
name: two-table-phase2-read-back
description: phase 2 of the two-table mapping -- the "both" state + the read-back OOM fix; the hard part was that a shmem folio is swapbacked/anon and reclaim skips anon without swap
project: linux
---

# Mission Log: Two-Table Phase 2 and the Read-Back OOM

Part 29 of the tmpfs transparent-compression series.  Session 45
(2026-10-13).  This is the session the session-44 handoff set up: land
Phase 2 of the two-table mapping, gated on "the 3G/2G read-back works on
a plain read()."  The mapping half was straightforward; the gate half
was not, and the reason was a reclaim property nobody in the series had
confronted yet.

Everything is committed.  The kernel tree is clean at Phase 2; the rig
tree is clean; the in-tree kernel is back to KASAN.

## What We Did

### 0. Bootstrapped from the logs, then committed the stray work

Read the newest mission logs (session 44's handoff first).  Before any
Phase 2 work, committed the one real uncommitted artifact: the stray
`mission_log/2026-10-09_headroom-fine-sweep.md` (session 28's log,
lint-clean) -> `03c61326632`.  Two other untracked items were junk, not
work: a 9.7 MB file literally named `-l` (an accidental `git log --graph`
dump, removed by the user) and `out/` (build artifacts, left alone).

### 1. Grounded the change, and found Phase 2 was bigger than advertised

Read the current data path before editing: `shmem_decompress_folio`'s
tail, the entry refcount (`refs`/`slots`), the truncate path, and
`shmem_recalc_inode`.  The load-bearing realization: **the "both" state
is impossible while the compressed id lives in the same `i_pages[index]`
slot as the folio** -- a folio and a `{id,gen}` value cannot coexist in
one xarray slot.  So Phase 2 necessarily *relocates the compressed
mapping out of `i_pages` and into `entries[]`*, which is the data-path
half of the encoding removal the handoff had scheduled for Phase 4.
Presented this scope finding; the user chose a self-contained Phase 2
(also free entries on truncate/unlink/evict) and a two-commit slice.

### 2. Commit A (`09af5745e027`) -- the two-table mapping

Moved the compressed mapping into `entries[]`, which now stores a packed
`{id, gen}` as a plain `xa_value` (no swap-type tag; the table never
holds folios or real swap entries, so it needs no type discrimination)
and owns the entry's durable reference (the `slots` counter):

- **compress**: `shmem_delete_from_page_cache(folio, NULL)` clears
  `i_pages[index]` instead of storing the `{id,gen}` swap-value (this
  needed `shmem_replace_entry`'s `VM_BUG_ON(!replacement)` relaxed so a
  NULL replacement -- clear the slot -- is legal).  `entries[index]`
  becomes the authoritative backing.
- **read**: `shmem_get_folio_gfp` detects compression via
  `entries[index]` when `i_pages` is empty and decompresses, KEEPING the
  entry (the "both" state) -- no `entry_release` in the decompress tail;
  the compressed gauge and the entry's swapped charge are retained.
- **truncate**: `shmem_undo_range` calls `shmem_comp_truncate_range` to
  free the range's entries[] (the i_pages walks no longer see compressed
  pages), folding the freed pages into the `nr_swaps_freed` recalc.
- **evict**: `shmem_comp_entries_destroy` frees any straggler then the
  table.
- **write-invalidate at the compress choke point**: re-compressing a
  modified both-state folio frees its stale `entries[]` slot first.

**Bug caught immediately**: `test_cell_index_race` failed with "cells
survive full drain".  The writers overwrite+truncate; overwriting a
both-state folio re-compresses it, which overwrote `entries[index]` with
the new id without freeing the old entry -- a leak.  This is the
write-invalidate the handoff had deferred to Phase 3, but a self-contained
Phase 2 must not leak, so it moved to the compress choke point
(`shmem_comp_invalidate_index`).  After that the suite was green.

### 3. Commit B -- the read-back gate, and a long wrong turn through release_folio

The gate OOM'd, and getting to the real cause took several wrong turns
that are worth recording.

First I wrote the design the spec named: a `release_folio` that drops the
both-state folio.  It compiled and the suite passed, but it was **inert**:
`folio_needs_release()` only fires for `folio_has_private()` or
`AS_RELEASE_ALWAYS`, and shmem sets neither.  Wiring `AS_RELEASE_ALWAYS`
then collides with the reclaim contract from two directions:

- **truncate** calls `release_folio` too (via `truncate_cleanup_folio`),
  and if it removes the folio the outer `filemap_remove_folio` hits a
  NULL mapping.
- even in **reclaim**, removing the folio inside `release_folio` breaks
  `__remove_mapping`'s `BUG_ON(mapping != folio_mapping(folio))`, because
  the free path's local `mapping` was captured before the call.

I also tried the cheap LRU lever -- dropping `folio_set_referenced` from
the decompress tail so cache folios start inactive.  That did move them
off the active LRU (`active_anon` -> `inactive_anon` in the OOM dump) but
**still OOM'd**.

The definitive diagnosis came from the OOM dumps plus `get_scan_count`:
without a swap device, `get_scan_count()` sets `scan_balance = SCAN_FILE`
and **never scans the anon LRU**.  A shmem folio is swapbacked, so the
both-state cache sat on the anon LRU and reclaim never even looked at it
-- it accumulated until the OOM killer took the reader.  That is the
actual read-OOM mechanism, and it is *why* `release_folio` could never
have worked: it is reached only after the anon/file scan decision.

At this point the user gave the guiding principle -- "minimize the patch
surface on upstream.  hooks only if possible.  but do not write unreadable
code to achieve it.  it has to be natural" -- which pointed straight at
the right answer.  A decompression cache **is a file page**: it is cached
from "disk" (the compressed entry), so it belongs on the file LRU, not
the swappable-anon LRU.  Commit B (`68d55e76c290`) does exactly that, with
no new hook at all:

- after `shmem_add_to_page_cache()` (which requires the swapbacked bit
  set during install), **clear the swapbacked bit** and add the folio to
  the **file** LRU, where reclaim scans it even with no swap and drops the
  clean cache via `__remove_mapping`; the entry survives and a later read
  re-decompresses.
- keep `folio_set_referenced` dropped: a cache cheap to re-materialise
  should be dropped promptly, not retained as a hot page.
- also fixed `shmem_comp_invalidate_index` to clear `entries[index]`
  *before* dropping the entry's reference, so the slot never dangles onto
  a freed id (a recycled id could be mistaken for live backing).

### 4. The rig read-back gate, and the non-KASAN dance

`test_fill`'s read-back still used the mmap+MADV_DONTNEED workaround,
which faults a whole 32 MiB file in (all mapped) before the zap -- that
OOMs regardless of kernel changes.  The handoff had already said to
switch it to a plain `read()` once Phase 2 landed, so I did (rig commit
`5a55e7a`).

The fill test runs on a non-KASAN kernel (KASAN overhead distorts a 2G
box).  There is no rig toggle for that, and a parallel `O=` build is
refused while the in-tree build exists, so I toggled `CONFIG_KASAN` off
in `/home/snide/linux/.config`, built, ran the gate, then restored KASAN
by re-running `build_kernel.sh` (which recopies the rig's saved KASAN
config from `build/kernel/.config`).  The KASAN config was backed up to
`out/config.kasan.bak` throughout.

## What We Learned

### Technical

- **The read-back OOM is a reclaim-scan property, not a "no release_folio"
  property.**  `get_scan_count()` sets `SCAN_FILE` when there is no swap,
  so the anon LRU is never scanned.  A shmem folio is swapbacked and
  therefore lives on the anon LRU, so a decompressed page was invisible
  to reclaim.  The series had blamed the absence of `release_folio`; the
  real barrier is upstream of it, in the scan decision.

- **`release_folio` is the wrong lever for shmem, and not only because
  it is not wired.**  Its contract is to release *buffers*, not remove
  the folio; the reclaim free path keeps a stale `mapping` local and then
  calls `__remove_mapping`, which BUGs if the folio was already detached.
  Any future "make a shmem folio droppable" work must move the folio's
  LRU class, not add a removal hook.

- **A decompression cache is semantically a file page.**  Clearing
  `swapbacked` after `shmem_add_to_page_cache()` (which asserts it) and
  before `folio_add_lru()` is both minimal and the honest model -- the
  compressed entry is the backing store, exactly like a disk block.

- **`folio_set_referenced` matters, but it was not the binding cause.**
  Removing it moved cache folios from the active to the inactive anon
  LRU, visible in the OOM dump -- useful, and kept, but it did not fix
  the gate on its own because the anon LRU was not scanned at all.

- **The write-invalidate cannot be deferred out of a self-contained
  Phase 2.**  Re-compressing a modified both-state folio overwrites
  `entries[index]`; without freeing the old entry first, that leaks, and
  `test_cell_index_race`'s completeness check found it in one run.

- **`shmem_replace_entry` had `VM_BUG_ON(!replacement)`** -- it could not
  store NULL.  The two-table compress path needs to clear `i_pages`, so
  that assertion had to be relaxed (storing NULL is a normal xarray
  clear).

### Process

- **The OOM dumps were the whole investigation.**  Each one named the
  allocating site (`shmem_decompress_folio`) and the LRU state
  (`active_anon`/`inactive_anon`, `all_unreclaimable?`, `dirty`).  Reading
  `get_scan_count` only after seeing "inactive but still accumulating"
  is what turned "reclaim is not keeping up" into "reclaim is not
  looking."  One more blind code change would have cost another boot
  cycle each time.

- **The user's "minimal surface, hooks only if possible" was the deciding
  constraint**, and it was right: the correct fix ended up smaller than
  the incorrect one I had already written.  When a hook fights the
  substrate, the substrate is telling you the abstraction is wrong.

- **The non-KASAN gate and the KASAN suite answer different questions**,
  and both were needed: the suite catches correctness (the
  non-swapbacked change could have broken truncate/GC -- it did not,
  green3x), the non-KASAN fill catches the survival property the suite
  cannot see.

## What We Decided

- **Phase 2 = Commit A (two-table mapping) + Commit B (cache is a file
  page),** both on `feature/tmpfs-compression`, plus the rig `read()`
  gate.  Encoding removal stays at Phase 4 (the `{id,gen}` value no
  longer lives in `i_pages`, so the constant and its packers are now dead
  code in the data path).

- **No `release_folio`, no `AS_RELEASE_ALWAYS`.**  The decompression
  cache is made reclaimable by its LRU class instead.  This is the
  minimal-surface outcome and it is also the more orthodox one.

- **The write-invalidate lives at the compress choke point for now**,
  not in the write path.  Phase 3 can move it to modify-time (dirty
  implies `entries[N]==0`) and skip re-compressing clean both-state
  caches, which today get wastefully recompressed by the aging path.

## Rules Added

- **A shmem page is swapbacked and therefore anon-LRU; without swap,
  reclaim does not scan it.**  Anything that wants a tmpfs page to be
  reclaimable *without a swap device* must make it a file-LRU page
  (clear swapbacked), not add a release hook.

- **When a fix fails, read the reclaim/scan path for the mechanism before
  guessing again.**  The OOM dump plus `get_scan_count` settled in one
  read what three code changes had not.

- **A self-contained phase must not leak on the path it introduces.**  If
  re-compress overwrites a mapping slot, it must free the old occupant in
  the same choke point, even if the "proper" invalidate lands in a later
  phase.

## What We'd Do Differently

- Would have read `get_scan_count` the moment the first OOM showed
  `all_unreclaimable? no` with clean folios, instead of trying
  `release_folio` and then `folio_set_referenced`.  The LRU-class answer
  was one grep away and I spent two boot cycles not finding it.

- Would have written the plain-`read()` gate *before* building a kernel
  to test the mmap workaround against -- the workaround was already
  documented as obsolete in the handoff.

- Would have named the non-KASAN build procedure (toggle + restore via
  `build_kernel.sh`) in an earlier log; reconstructing it cost a build
  cycle.

## Files Changed

| File | Purpose |
|------|---------|
| `include/linux/shmem_compress.h` (`09af5745e027`) | new `entries[]` decls (present/truncate_range), `shmem_decompress_folio` new signature, `slots`/`refs` doc updated to entries[] semantics |
| `mm/shmem.c` (`09af5745e027`) | relaxed `shmem_replace_entry` (allow NULL), two-table read-path detection, `shmem_undo_range` entries[]-clear |
| `mm/shmem_compress.c` (`09af5745e027`, `68d55e76c290`) | entries[] table (pack/unpack/lookup/present/drop/truncate_range/destroy), decompress rework, compress clears `i_pages`, `invalidate_index`, non-swapbacked cache + no `folio_set_referenced` |
| rig `tests/test_fill.c` (`5a55e7a`) | read-back via plain `read()` |

## Test Results

| gate | result |
|---|---|
| Commit A build | 0 errors, 0 warnings |
| full KASAN suite (Commit A) | exit 0, all ALL PASS, 0 KASAN |
| full KASAN suite (Commit B, x3) | exit 0, all ALL PASS, 0 KASAN, `test_cell_index_race` stable |
| **Phase 2 gate: 3G/2G read-back, plain `read()`** | **wrote 3072 MB, read 3072 MB at ~2013 MB/s, CRC sample ok, no OOM, exit 0** |

Fill gauge (arm 2, zsmalloc, 2G RAM): `comp_pages=786432
compr_bytes=1.64GB uncomp_bytes=3.22GB ram_total=1.75GB`.  Write
~786 MB/s; read-back ~2013 MB/s with the cache reclaim keeping up.

## Next Steps

1. **Phase 3**: truncate/modify clear `entries[]`; establish the
   "dirty folio implies `entries[N]==0`" invariant in the write path, and
   have the aging path skip re-compressing clean both-state caches (today
   it wastefully recompresses them via the choke-point invalidate).
2. **Phase 4**: remove the now-dead `SWP_SHMEM_COMPRESS` encoding +
   `{id,gen}` packers; re-assess the blast radius on this branch vs s1.
3. Standing items unchanged: M-boot the perf numbers, cell_order sweep,
   the arm-3 rm/synchronous-free story at 64M.

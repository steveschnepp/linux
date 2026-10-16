---
name: group-decompress-truncate-race
description: the standing "group-decompress/truncate interleaving" question is a REAL race -- read/fault-path decompress installs folios into page-cache slots a truncate already cleared, resurrecting data past EOF and into punched holes; both single and group paths affected
project: linux
previous: 2026-10-16_tooling-centralization.md
---

# Mission Log: The Decompress-Install vs Truncate Race

Part 50 of the tmpfs transparent-compression series.  Session
(2026-10-16), picking up the standing backlog item "group-decompress/
truncate interleaving race" (carried in the Next Steps of parts 47-49,
originating from session 54's stale-comment find).  This log records
the investigation: the race is REAL, it is broader than "group", and
the exact interleaving and missing defence are now pinned by code
reading.

## The origin of the question

Session 54 (see 2026-10-15_32bit-packing-and-table-map.md) noticed
that `shmem_decompress_group`'s comment claimed per-slice install
races were resolved by `shmem_add_to_page_cache()` "validating the
replicated ID+generation" -- but the calls pass `shadow/expected=NULL`
and that function never reads `info->comp_entries`.  Session 56 fixed
the COMMENT (it now names the real defences: -EEXIST retry, pin
failure -> -ENOENT) but never investigated whether a race actually
exists behind the bogus comment.  It does.

## What We Did

Full code reading of the interleaving, in this tree:

1. `shmem_decompress_folio` (shmem_compress.c:3336) and
   `shmem_decompress_group` (:2897): pin `ent` via
   `shmem_comp_entries_get` (RCU + `inc_not_zero`), decompress into a
   bounce, allocate a folio per slice, then
   `shmem_add_to_page_cache(sib, mapping, sidx, NULL, gfp)` -- install
   into `i_pages` with `expected=NULL`, i.e. "succeeds iff the slot is
   EMPTY".
2. `shmem_add_to_page_cache` (shmem.c:908): with `expected=NULL` any
   conflict -> -EEXIST; an empty slot always installs.  It knows
   nothing about `info->comp_entries` (separate xarray).
3. `shmem_undo_range` (shmem.c:1148) truncate hook: FIRST
   `shmem_comp_truncate_range` (:1183) claims (xa_erase) every
   `entries[]` slot in [start, end), THEN the `find_lock_entries`
   walks remove cached folios.  All under i_rwsem (setattr/fallocate/
   evict).
4. Caller locking: `shmem_file_read_iter` (shmem.c:3451) takes NO
   i_rwsem (its own comment: "reads (unlike writes) are called
   without i_rwsem protection against truncate"); `shmem_fault`
   (:2766) takes none ("we cannot take i_rwsem in fault");
   `shmem_file_write_iter` (:3577) takes i_rwsem exclusive;
   `shmem_write_begin` (:3377) documents "i_rwsem is held by caller".

### The interleaving (both single-page and group paths)

```
Reader (no lock)                          Truncator (i_rwsem)
-------------                             -------------------
filemap_get_entry -> NULL
entries_present -> true
decompress_folio: pin ent
  [group: load + decompress -- sleeps]
                                          i_size = newsize (truncate)
                                          shmem_undo_range:
                                            comp_truncate_range:
                                              claim+erase entries[idx..]
                                            i_pages walk: nothing installed
                                          unlock
add_to_page_cache(idx, expected=NULL)
  -> slot EMPTY -> install SUCCEEDS
folio stays in cache past i_size /
 inside a punched hole, "both state"
comment now false (entries[idx] == NULL)
```

### Why every existing defence misses it

- **The pin outlives the slot.**  `entries_get` pins before truncate
  erases the slot; a live pin never fails, so the `-ENOENT -> retry`
  branch in `shmem_get_folio_gfp` (shmem.c:2570) is never triggered by
  this race.  Nothing re-reads the slot after the pin.
- **-EEXIST never fires**: the truncate leaves `i_pages[idx]` EMPTY
  (that is the point of truncate), and the install target is that
  empty slot.
- **`expected=NULL`**: upstream's swapin
  (`shmem_swapin_folio` -> `shmem_add_to_page_cache(...,
  swp_to_radix_entry(swp), ...)`) installs CONDITIONAL on the slot
  still holding the value entry it read -- a truncated swap entry
  makes the install fail.  Our compressed entry lives in a different
  xarray, so there is nothing to pass as `expected`.  This is exactly
  the check the deleted stale comment claimed existed: the upstream
  defence mechanism exists, and the two-table mapping dropped it
  without a replacement.
- **i_size recheck does not exist** on the decompress branch (it
  returns early, like the upstream cache-hit path), and would not
  cover punch-hole anyway (no i_size change).

### Consequences if hit

- Stale uptodate folio(s) in `i_pages` at indexes >= new i_size
  (truncate) or inside a punched hole (punch): tmpfs's "beyond i_size
  / holes read as zeros" invariant is broken.  Observable: ftruncate
  down + ftruncate up (or a hole read) serves the OLD data instead of
  zeros -- a data-resurrection/corruption bug, the classic truncate
  bug class.
- Group case resurrects up to nr_pages-1 sibling folios per hit, not
  just the requested index.
- Stats drift: `mapping->nrpages` / NR_SHMEM / NR_FILE_PAGES inflated;
  the entry's swapped charge was already dropped by truncate, so the
  alloced gauge undercounts.  (Minor next to the correctness hit.)
- The racing reader itself consumes nothing stale (read_iter
  re-checks i_size after get_folio), and a full-page rewrite via
  write() overwrites a resurrected folio -- which is why the existing
  test_entry_race (readers pread vs writer ftruncate+rewrite loops)
  passes while the bug lives: it never reads a hole or an extended
  range.

## What We Learned

### Technical

- **The pointer-encoding pin is deliberately lifetime-only**: it says
  the ENTRY is alive, nothing about the SLOT still naming it.  That
  is correct for GC/reclaim, but the install path needs the slot's
  presence, not the entry's aliveness -- and the install path never
  re-checks it.  The session-56 encoding change (xa_erase claim) fixed
  a double-drop on the CLEAR side but introduced no check on the
  INSTALL side.
- **The upstream pattern for "install what you read from a value
  slot" is the `expected` parameter**, and our two-table mapping
  silently lost it.  Any future install-from-entry path must carry an
  equivalent revalidation.
- **Truncate's order inside `shmem_undo_range` is the key to a
  correct fix**: slots are cleared over the WHOLE range first, folio
  walks second.  Therefore any folio installed after the walk passed
  its index necessarily finds its slot already cleared -- a POST-
  install slot revalidation catches the race completely (a pre-check
  only shrinks the window).
- **Read/fault paths are the only exposed callers** (no i_rwsem);
  write-path decompress (write_begin) is excluded by the caller's
  lock.  Same split as the entry-race bug.

### Process

- The question sat in the backlog four logs because the stale comment
  was "fixed" (reworded) instead of investigated -- fixing a comment
  that describes a non-existent defence should always raise "does the
  defence need to exist?".
- Reading the caller locking (read_iter/fault/write_iter) is what
  turned "maybe a race" into "reachable race with a named
  interleaving"; it took one grep pass.

## What We Decided

- **The race is real and worth fixing now** (data resurrection past
  EOF / into holes; correctness class, blocks upstream).
- **Fix shape (designed this session, implemented next): post-install
  slot revalidation**, per slice, in both `shmem_decompress_folio`
  and `shmem_decompress_group`:
  - After `shmem_add_to_page_cache` succeeds, re-read
    `entries[sidx]`; if it no longer names `ent`, undo the install:
    `shmem_delete_from_page_cache(sib, NULL)` + folio unlock +
    `wake_up_folio_waiters` drain (the entry-race drain -- this
    removal can be the folio's last lock/unlock) + put, and return
    -ENOENT for the requested slice (retry; entries_present now false
    -> alloc path -> i_size check -> zeros/-EINVAL).
  - Group: keep sibling folios LOCKED from install until the whole
    group is validated, then unlock+put.  Holding the lock across the
    window (a) blocks a racing write_begin (so a "slot cleared" post-
    check can only mean truncate, never the write-invalidate, making
    removal safe without a dirty-state dance), and (b) blocks reclaim
    from detaching a sibling before its post-check.
  - Safety argument: slot alive at post-check (which runs after
    install) implies truncate's erase of that slot happens after the
    install, hence its i_pages walk -- which runs after the erase --
    happens after the install and removes the folio itself.  Slot
    dead at post-check implies we remove it ourselves.  Either way no
    folio survives past the truncate.
- No i_size-based check: it would not cover punch-hole, which is the
  same `shmem_undo_range` path.

## Rules Added

- **Any path that INSTALLS a folio derived from a compressed entry
  must revalidate `entries[idx] == ent` after the install** (the
  two-table analogue of upstream's `expected=` swap-install).  The
  pin guarantees the entry's life, not the slot's claim on the cache.
- **A comment describing a defence that does not exist in the code is
  a bug report, not a doc bug** -- rewording it must trigger an
  investigation of whether the defence is needed.

## What We'd Do Differently

- Would have investigated the stale comment the moment it was found
  (session 54) instead of rewording it (session 56) and letting it
  ride the backlog four logs -- the comment was a reviewer waving at
  exactly this race.
- The upstream `expected=` precedent should have been grepped when
  the two-table install sites were written; "why does swapin pass an
  expected entry and we pass NULL?" is a one-line question that would
  have found this before first boot.

## Files Changed

| File | Purpose |
|------|---------|
| `mission_log/2026-10-16_group-decompress-truncate-race.md` | this log (investigation only; no code yet) |

## Test Results / Benchmarks

None this session -- analysis only.  Reproducer plan (next): a test
that (1) writes+compresses a file, (2) loops readers pread against a
writer ftruncate-down/ftruncate-up and punch-hole, (3) after
quiescence verifies a full read of the extended range and the punched
hole is all zeros.  The existing entry-race test cannot see the bug
because it always rewrites full pages after extending.

## Next Steps

1. Implement the post-install revalidation (single + group paths);
   i386 + x86_64 compile gates.
2. Write the resurrection reproducer test in the rig; verify it
   FAILS on the unpatched kernel and PASSES on the fixed one (pristine
   oracle rule); loop it under KASAN.
3. Full KASAN suite regression; then rig cleanup and the standing
   backlog (H618 re-measure, RFC assembly, plateau/CPU-freq, workload
   bed).

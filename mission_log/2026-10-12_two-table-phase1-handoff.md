---
name: two-table-phase1-handoff
description: spec 02 updated + phase 1 of the two-table mapping committed (entries[] shadow); phase 2 (the "both" state / read-OOM fix) is the handoff point for a fresh session
project: linux
previous: 2026-10-12_fill-test-and-two-table-mapping.md
---

# Mission Log: Two-Table Mapping, Phase 1, and the Handoff

Part 28 of the tmpfs transparent-compression series.  Session 44
(2026-10-12).  A short implementation session that landed the spec
update and Phase 1 of the two-table mapping, then deliberately stopped
at the hardest phase so a fresh session can pick it up with full
attention.  The brief, in the user's words: "yes.  but mission log
FIRST.  then spec2.  then code then test.  commiting all along the
way.  ...  bisectable!" and, at the close, "write the mission log.  so
incan continue in a fresh session".

Everything is committed.  The tree is clean at Phase 1.

## What We Did

### 1. Committed the uncommitted work first

Before anything else, committed the fill test that was sitting
uncommitted from session 43 (`8bf9acb`: `tests/test_fill.c` +
`test.fill=` boot mode + `TEST_FILL*` passthroughs).  The commit
message records the known read-back limitation honestly and names the
kernel-design gap (a page is compressed XOR uncompressed) as the cause,
not a test bug.

### 2. Updated spec 02 for the two-table mapping (`eed7dade8c76`)

Rewrote the core of `specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md`:

- **The two-table mapping** replaces "the xarray encoding": a compressed
  page is compressed / uncompressed / BOTH, so the mapping is `i_pages[]`
  (folios; the existing page cache, which the MM depends on and cannot be
  replaced) + a new per-inode `entries[]` xarray (compressed entry ids),
  same `pgoff_t` index space, 0 = none.
- **Page lifecycle section** -- the "both" state, each transition an
  independent single-table write (write / GC compress / read / reclaim /
  read-again / modify).
- **The hot-page cost** stated honestly: folio (4K) + blob (~2K) = ~6K
  for a hot compressed page, reclaimable to ~2K/page + the hot set.
- **The accounting wrinkle**: a "both" page is charged in both
  `nrpages` and `swapped`; reclaim must uncharge the folio without
  losing the entry's charge.
- **`SWP_SHMEM_COMPRESS` removed** as a consequence: the compressed
  mapping moves out of the page-cache slot into `entries[]`, dissolving
  spec 02's flagged biggest upstream review risk (a reserved swap type
  multiplexed with folios and real swap entries).  The RFC cover letter
  no longer carries the encoding question.
- Migration rephased: Phase 2 is now the two-table mapping, gated on
  "the 3G/2G read-back works on a plain read()".

### 3. Decided the branch and phasing (ask_user)

- **Branch: `feature/tmpfs-compression`** (not `upstream/s1`).  Chosen
  for the rig test setup that already boots this kernel.  Note the
  tradeoff recorded at the time: the `{id,gen}` encoding is referenced
  in ~26 places in `shmem.c` on this branch (vs ~2 on s1), and the cell
  backend + the just-committed GC reverse index both live here -- so the
  eventual encoding removal (Phase 4) has a bigger blast radius on this
  branch than it would on s1.
- **Phasing: 4 commits, encoding removed last**, so the old path stays a
  fallback until the new one is proven.

### 4. Phase 1 committed (`890f006d8bd3`)

The safe phase: `entries[]` infrastructure + a dual-write *shadow* that
nothing reads yet, so the `{id,gen}` slot stays authoritative and
behavior is unchanged.  The plumbing (lazy per-inode xarray, the
cmpxchg lazy-init race, the evict lifecycle) is proven under the full
KASAN suite before the read path switches over in Phase 2.

What landed:

- `include/linux/shmem_fs.h`: `shmem_inode_info` gains
  `struct xarray *comp_entries` (lazily allocated; NULL = never
  compressed, so uncompressed inodes pay one pointer).
- `include/linux/shmem_compress.h`: declares
  `shmem_comp_entries_destroy()`.
- `mm/shmem_compress.c`:
  - `shmem_comp_entries_set()` -- lazily allocates the per-inode xarray
    (cmpxchg so two concurrent first-compresses cannot leak or
    double-install), stores `xa_mk_value(id)` (id 0 never allocated, so
    0/NULL = no compressed copy; **no reference taken** -- Phase-1 shadow
    only, the slot owns lifetimes).  A forward declaration is needed
    because the merge-group compress path appears earlier in the file.
  - `shmem_comp_entries_destroy()` -- NULL-safe `xchg` + `xa_destroy` +
    `kfree`.
  - **Both** compress paths dual-write: single-page
    (`shmem_comp_compress_one`) and merge-group (one id covers the whole
    group, so each index of the group records the same id).
- `mm/shmem.c`: `__shmem_get_inode` sets `comp_entries = NULL`;
  `shmem_evict_inode` calls `shmem_comp_entries_destroy(inode)` after
  `shmem_truncate_range`.

**Gate: full KASAN suite green** -- `test_cell_gc`,
`test_cell_index_race`, `test_mount_sem` all ALL PASS, exit 0, 0 real
KASAN reports.  (This is the same suite session 42's reverse index
passes; it exercises the compress path that now dual-writes.)

## What We Learned

### Technical

- **`memset(info, 0, ...)` in `__shmem_get_inode` already zeroes the
  whole struct**, so an explicit `= NULL` for a new field is redundant
  but worth keeping as documentation of the lazy-init contract.  There
  is no early-return-leak risk for a field that starts NULL.
- **`init_xarray` does not exist; it is `xa_init(xa)`.**  Cost one
  build cycle.  (The kernel's xarray init is `xa_init` /
  `xa_init_flags`.)
- **The two-table field must go in `shmem_inode_info`, not
  `shmem_sb_info`.**  The first edit put `comp_entries` next to the
  per-mount `comp` pointer in the SB struct -- caught by the build
  ("no member named comp_entries" in the inode paths).  The per-inode
  table and the per-mount pool pointer are easy to conflate when the
  compression state is split across both structs.
- **The Phase-1 shadow holds no reference on purpose.**  Dual-writing
  the id while the slot stays authoritative means `entries[]` is
  write-only bookkeeping; it cannot leak or UAF because nothing reads
  it and nothing frees based on it.  That is what makes Phase 1 safe to
  land before the reference-ownership change in Phase 2.

### Process

- **Stopped at the phase boundary, not mid-phase.**  Phase 1 is a
  coherent, bisectable, suite-green commit.  Phase 2 (the "both" state)
  changes the reference-ownership contract of the whole compress/
  decompress/truncate trio and adds a reclaim path that does not exist
  today -- a subtle-mistake-is-silent-corruption zone.  Rather than
  rush it at the tail of a long session, the session ended here so a
  fresh one can do it with full attention.  This is the "green gates
  are not a performance verdict" discipline applied to session length:
  the safe increment landed; the delicate one gets a clean start.
- **Commit-along-the-way held.**  Every artifact is committed:
  fill test (`8bf9acb`), mission log 43 (`47ad73da7918`), spec 02
  (`eed7dade8c76`), Phase 1 (`890f006d8bd3`), and this log.

## What We Decided

- **Target branch: `feature/tmpfs-compression`.**  Phase 4 (encoding
  removal) will have a larger blast radius here (~26 encoding sites +
  cell backend + reverse index) than on s1; that cost is accepted for
  the test-setup convenience, and is worth re-checking when Phase 4
  arrives.
- **4 phases, encoding removed last.**  Phase 1 done; Phases 2-4 below.
- **This session stops at Phase 1.**  Phase 2 is the handoff.

## The Handoff: Phase 2 in Detail

Phase 2 is the correctness-critical one -- the actual "both" state that
fixes the read-back OOM.  It changes **three things together**, and
getting one wrong is a UAF or a leak, not a test failure:

1. **Reference ownership flips.**  Today the page-cache slot owns the
   entry's reference; `shmem_decompress_folio()` *releases* it
   (`shmem_comp_entry_release(id, gen)`) after installing the folio.
   In the "both" state, `entries[N]` keeps that reference while the
   folio also lives.  So the decompress tail, the truncate path
   (`shmem_free_swap` / `shmem_comp_free_swap`), and the entry-free path
   all change their refcount contract together.  The two-counter scheme
   (entries-ref + reader pins, spec 02) is the target contract.

2. **A reclaim path that does not exist.**  The point of Phase 2 is that
   reclaim can drop the uncompressed folio and keep the entry.  shmem
   has **no `release_folio` today** -- this is exactly why the read-back
   OOM'd (verified in session 43: shmem_aops has no release_folio, so
   decompressed pages are not clean-file-cache droppable).  Adding one
   that identifies "this folio is a decompression cache of a live
   entry, drop it, keep the entry, restore the accounting" is new MM
   reclaim code in upstream-target source.  The user's model: the
   compressed entry is durable, the uncompressed folio is a reclaimable
   cache of it.

3. **The accounting wrinkle is load-bearing.**  A "both" page is charged
   in both `nrpages` and `swapped`; reclaim must uncharge the folio
   without losing the entry's charge.  This session (and the series)
   has repeatedly shown this class of bug hides until the exact
   mechanism is checked.

**Phase 2 gate (from spec 02):** the 3G/2G read-back works on a plain
`read()` -- no OOM -- plus the existing smoke/remount/suite gates.

**Where the code is:** `mm/shmem_compress.c` `shmem_decompress_folio()`
(line ~3332) is the decompress entry point; it pins the entry
generation-validated, decompresses into a fresh folio, and installs it
via `shmem_add_to_page_cache()` with the comp_entry as the expected old
slot, then releases the slot reference.  That tail is the heart of the
change.

**Phase 3** (after 2): truncate/modify clear `entries[]` -- the
write-invalidate frees the stale entry, `shmem_undo_range` clears both
tables; invariant: a dirty folio implies `entries[N]==0`.

**Phase 4** (last): remove the `SWP_SHMEM_COMPRESS` encoding +
`{id,gen}` packing now that `entries[]` is authoritative everywhere.
On this branch that is ~26 sites in `shmem.c` plus the cell/reverse-index
interactions -- re-assess the blast radius (and whether to port to s1
first) when Phase 4 arrives.

## Files Changed

| File | Purpose |
|------|---------|
| `specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md` (`eed7dade8c76`) | two-table mapping, "both" lifecycle, swap-type encoding removed |
| `include/linux/shmem_fs.h` (`890f006d8bd3`) | `shmem_inode_info.comp_entries` |
| `include/linux/shmem_compress.h` (`890f006d8bd3`) | `shmem_comp_entries_destroy()` |
| `mm/shmem_compress.c` (`890f006d8bd3`) | `entries_set`/`entries_destroy`, dual-write in both compress paths |
| `mm/shmem.c` (`890f006d8bd3`) | inode init + evict wiring |
| rig `tests/test_fill.c` etc. (`8bf9acb`, carried from session 43) | the fill test + boot mode |

## Test Results

| gate | result |
|---|---|
| Phase 1 build (`shmem_compress.o`, `shmem.o`) | 0 errors, 0 warnings |
| full KASAN suite (cell_gc, cell_index_race, mount_sem, ...) | exit 0, all ALL PASS, 0 real KASAN |

## What We'd Do Differently

- Would have checked the xarray init API name (`xa_init`, not
  `init_xarray`) and put the field in the right struct
  (`shmem_inode_info`, not next to the per-mount `comp` pointer) before
  the first build; both cost one build cycle each and both are
  five-second greps.

- Would have named the 4-phase split and the branch choice *before*
  writing spec 02, rather than asking mid-implementation.  The spec was
  written for a single-branch, single-lifecycle change; the branch
  question (s1 vs feature) and the encoding-removal blast radius were
  settled after the spec text was already committed, so the spec's
  "remove the encoding" framing is slightly optimistic about the
  feature-branch cost.  Worth reconciling if Phase 4 lands on this
  branch.

## Next Steps

1. **Phase 2** (the handoff, detailed above): the "both" state --
   decompress keeps the entry, add the reclaim `release_folio`, fix the
   refcount + accounting contracts together.  Gate: 3G/2G read-back on
   a plain `read()`.
2. **Phase 3**: truncate/modify clear `entries[]`; dirty-implies-empty
   invariant.
3. **Phase 4**: remove the `SWP_SHMEM_COMPRESS` encoding; re-assess the
   blast radius on this branch vs s1.
4. **Rerun the fill test's read-back** once Phase 2 lands, to confirm
   the kernel-level fix (the mmap+MADV_DONTNEED test workaround in
   `test_fill.c` can then be removed in favor of a plain read).
5. Standing items unchanged: M-boot the perf numbers, cell_order sweep,
   the arm-3 rm/synchronous-free story at 64M.

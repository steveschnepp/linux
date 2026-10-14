# ztmpfs: Transparent Compression for tmpfs Specification

## Overview

`ztmpfs` is transparent, eager compression of tmpfs file data inside the page
cache. A tmpfs mount with `compress=lz4` compresses each file page as it is
written and decompresses it transparently on read/fault, so a compressible
workload occupies a fraction of the RAM an uncompressed tmpfs would. There is
no separate block device, no swap, no upfront RAM reservation, and no
application change.

This is the `feature/tmpfs-compression` / `upstream/s1` track. v1 (the
upstreamable slice) uses a zsmalloc backend and is implemented and green on
the `upstream/s1` branch; the cell-backed bulk tier (arm 3) is the payload of
the series and the subject of a later phase.

## Motivation

tmpfs holds a large share of a modern system's transient data -- build caches,
package extractions, container layers, logs, temp tables -- 40-70% of which is
compressible. Today that data sits uncompressed in the page cache until
reclaim or OOM. The existing answers each miss:

- **zram-swap** compresses only cold swapped-out pages; its compression is
  hypothetical without memory pressure, and under a hard squeeze the box
  OOM-kills before reclaim converges.
- **zswap** likewise compresses only at swapout time and requires swap.
- **Neither** compresses actively-written tmpfs files, and both add a device /
  swap configuration layer.

ztmpfs collapses the fast and bulk tiers: the folio exists uncompressed only
transiently during the write, then lives compressed in the page cache and is
decompressed on first read. Compression is unconditional, not
reclaim-driven.

This is measured, not rhetorical (session 39 `test_zbench`, arm 1 vs arm 2,
lz4, munin dataset, 29.3 MB apparent):

| metric | arm 1 tmpfs+zramswap | arm 2 ztmpfs/zsmalloc |
|---|---|---|
| live-set RAM | 38.4 MB | **21.6 MB (-44%)** |
| compression | never (hypothetical) | 2.1:1, unconditional |
| untar (write) | 0.091 s | 0.177 s (1.9x) |
| first-read | 0.088 s | 0.114 s (1.3x) |
| 256M squeeze to 2 MB free | **OOM-killed** | **survives, +15 MB anon headroom** |

The rationalization in one line: arm 1's compression never happens without
pressure and the box dies before it converges; arm 2's is unconditional, costs
1.9x write / 1.3x first-read, and buys survival. ztmpfs targets the regime
where the canonical answer dies -- the 64M-1G SBC fleet (`openwrt-refocus`).

## Goals

- Transparent per-page compression of tmpfs file data at write time, with
  transparent decompression on read/fault, unmodified applications.
- A RAM guarantee: a compressible workload occupies materially less RAM than
  uncompressed tmpfs, with no swap and no device.
- Unconditional (eager, write-end) compression, not reclaim-driven.
- A read-only `comp_stats` surface honest about live RAM footprint.
- Graceful degradation: a pool failure never fails a mount -- it runs
  uncompressed.
- A clean, reviewable upstream slice (v1) separable from the experimental
  cell/cellzram allocator (arm 3).

## Non-Goals

- Not swap, not zram, not zswap. No block device, no swap slot, no
  swappiness. Compression lives in the page cache.
- Not a watermark/hot-chill policy in v1. v1 compresses eagerly at write end;
  the watermark WHEN-layer and the aging workqueue are cut from the upstream
  slice (present in the prototype, deferred).
- Not the cell allocator in v1. v1 is zsmalloc-backend only; the cell-backed
  bulk tier (arm 3) is a later phase and shares only the entry API.
- Not compressed merged groups in v1 (`merge_pages` cut). Single-page
  records.
- No remount reconfiguration. Options are set at mount; a pool never switches
  backend after creation.
- Not memory-failure handling in v1's first posting (a standing s1 item, not
  a design non-goal forever).

## Design

### Where compression sits

Compression is a tmpfs-internal, per-mount feature hanging off one pointer in
`shmem_sb_info.comp` (`NULL` = uncompressed mount). `mm/shmem.c` never
touches compression internals; it calls a small hook surface and
`mm/shmem_compress.c` owns everything else. v1's hook surface is deliberately
minimal.

```
write_end:   shmem_comp_should_compress(inode)
               -> shmem_compress_folio(inode, index, folio)   [after unlock]
read/fault:  shmem_get_folio -> i_pages[N] empty but entries[N] set
               -> shmem_decompress_folio(...)   [folio set, entries[N] KEPT]
reclaim:     release_folio -> drop folio, keep entries[N]  [compressed survives]
modify:      write to a cached folio -> free entry, clear entries[N]
truncate:    shmem_undo_range -> clear i_pages[N] + entries[N]  [drop ref]
put_super:   shmem_comp_put_super(sbinfo)                      [destroy pool]
show_options:shmem_comp_show_options(seq, sbinfo)              [/proc/mounts truth]
```

### The two-table mapping (the load-bearing decision)

A compressed page must be representable as **compressed, uncompressed, or
both** -- never compressed-XOR-uncompressed. That distinction is what lets a
read materialize an uncompressed folio while the compressed entry survives, so
reclaim can drop the folio and a later read re-decompresses. A single slot
cannot hold "both", so the mapping is two parallel tables, same `pgoff_t`
index space:

```
mapping->i_pages[N]   -> folio       (the existing page cache; NULL = no folio)
comp->entries[N]      -> entry id    (new per-inode xarray; 0 = no compressed copy)
```

`i_pages` *is* the folio table -- the MM (fault path, `filemap`, truncate)
operates on it and it cannot be replaced. `entries` is new, lazily allocated
per inode (only when the inode first compresses a page, so uncompressed files
pay ~nothing). Both are keyed by the same page index, so `entries[N]` is the
compressed backing for exactly `i_pages[N]`'s page N. The heavy metadata
(zsmalloc handle, length, refcounts) still lives in a global ID table in
`shmem_compress.c`; `entries[N]` holds only the small id.

This replaces -- and **removes** -- the earlier design where a compressed page
was a third slot kind in `i_pages`: a `{id, gen}` pair packed into an `xa_value`
under a reserved swap type, `SWP_SHMEM_COMPRESS`, multiplexed alongside folios
and real swap entries. That encoding was flagged as the single biggest
upstream review risk (a reserved swap type colliding with real swap entries or
a future pte-marker scheme). The two-table mapping dissolves it: the
compressed mapping lives in its own per-inode xarray, not in the page cache. A
private per-inode map alongside the page cache is *more* orthodox FS design
(btrfs extent maps, f2fs node maps) than the multiplexing it replaces, and
there is no reserved swap type, no `swp_entry` packing, no type-tagging.

### Page lifecycle (the "both" state)

The compressed entry is durable; the uncompressed folio is a reclaimable
cache of it. A page is in one of these states, and the two tables make each
transition a single independent write (no slot flip):

```
write        i_pages[N]=folio(dirty)  entries[N]=0        uncompressed
GC compress  i_pages[N]=0             entries[N]=id       compressed (folio freed)
read         i_pages[N]=folio         entries[N]=id       BOTH (entry kept)
reclaim      i_pages[N]=0             entries[N]=id       compressed (folio dropped)
read again   i_pages[N]=folio         entries[N]=id       BOTH (re-decompress)
modify       i_pages[N]=folio(dirty)  entries[N]=0        uncompressed (id freed, stale)
```

Each axis is independent: reclaim clears `i_pages[N]` and leaves `entries[N]`
alone; invalidate-on-write clears `entries[N]` and leaves `i_pages[N]` alone.
A dirty folio must have `entries[N]==0` (its compressed copy, if any, is
stale); a clean folio may have `entries[N]` set (it is a cache of that entry).

**The cost, stated honestly:** a hot compressed page occupies the folio (4K)
*and* the compressed blob (~2K) = ~6K, marginally more than uncompressed alone.
Under pressure the uncompressed folios are reclaimable and drop to
compressed-only, so the steady state degrades to ~2K/page + the hot set. For
tmpfs at or beyond RAM (the target regime) the reclaimable property is worth
far more than the hot-page premium -- it is the write path's "unconditional
compression buys survival" thesis extended to reads, and it is what makes
reading a >RAM tmpfs work at all.

**Accounting:** shmem charges `alloced == nrpages + swapped`. A hot compressed
page is both a page-cache folio and a compressed blob, so the accounting must
say so honestly and reclaim must uncharge the folio on drop without losing the
entry's charge. This is priced under Risks.

### Compression path (write end)

`shmem_comp_should_compress()` gates on: mount has an enabled pool, the inode
is not `nocompress`, the file is at/over the threshold. Then
`shmem_compress_folio()` (in `shmem.c`'s write_end, after `folio_unlock`) runs
the skip-list guard and compresses:

```
shmem_comp_compress_one(inode, index, folio):
  skip if folio->mapping != mapping or !uptodate
  skip if folio_mapped(folio)            // a reader may be mid-access
  skip if folio_maybe_dma_pinned(folio)  // cannot relocate under a pin
  skip if folio_order(folio)             // large folios not compressed in v1
  skip if !folio_test_dirty(folio)
  comp_len = shmem_comp_compress(pool, src, PAGE_SIZE, dst, &cap)
  if comp_len did not shrink: skip       // store raw, no decompress cycle
  handle = shmem_comp_store(pool, dst, comp_len)     // zs_malloc
  id = shmem_comp_entry_alloc(pool, handle, comp_len, 1, ...)
  xa_store(comp->entries, index, id)     // entries[N] = id
  free the uncompressed folio; clear i_pages[N]
```

The raw/compressed discriminator is the stored length: a block that did not
shrink is stored raw and never pays a decompress cycle on read. Compression
runs after `folio_unlock` so the write is already visible; the folio is freed
once `entries[N]` is set. Result: `entries[N]=id`, `i_pages[N]=NULL` --
compressed-only.

### Decompression path (read / fault)

`shmem_get_folio()` finds `i_pages[N]` empty but `entries[N]=id`, pins the
entry, decompresses into a fresh folio, and sets `i_pages[N]=folio` **without
clearing `entries[N]`** -- this is the "both" state. The entry stays alive,
so reclaim can later drop the folio and a subsequent read re-decompresses. A
stale id (entry freed by truncate, id recycled) is rejected by a generation
check on the entry and the lookup is retried.

### Invalidation on modify

A write to a folio whose `entries[N]` is set makes the compressed copy stale:
the write frees the entry, clears `entries[N]`, and the dirty folio becomes
the sole copy. GC re-compresses it later. This keeps the invariant *dirty
folio implies `entries[N]==0`*.

### Two-counter refcounting

Each compressed page's entry carries two counters:

- **entries-ref** -- one per `entries[N]` naming the id (the durable
  reference; v1: always 1).
- **refs** -- that plus temporary reader pins taken during decompression.

The separation exists because a reader must pin the entry before loading from
storage, but truncate can clear `entries[N]` concurrently. With one counter,
truncate could free the entry mid-decompress; with two, truncate decrements
only the entries-ref and the reader's pin on `refs` keeps the entry alive
until decompress completes. The entry frees its zsmalloc block when `refs`
reaches zero, by whichever of release/put gets there first.

### Pool and compression contexts

`shmem_comp_pool_create(algo, ...)` builds a zsmalloc pool plus per-CPU
`crypto_acomp` contexts (the acomp API is called from the owning CPU only).
v1 is zsmalloc-backend only; `shmem_comp_pool_uses_cells()` exists in the API
but is always false in v1. The algorithm resolves at mount: mount option,
else the sysfs default, else the Kconfig default. A pool-creation failure is
graceful: the mount runs uncompressed, never fails.

### Mount options (v1 surface)

```
compress=ALGO        enable, choose algorithm (lz4 default; "none" = off)
nocompress           disable for this mount (per-inode opt-out honored)
compress_threshold=SIZE   min file size (bytes, memparse) for write-time
                          compression; 1 = compress everything
```

`/proc/mounts` tells the truth: uncompressed mounts print `,nocompress`, and
the resolved-state line says WHY compression is off (`shmem_comp_off_reason`:
default-off, `nocompress`, `compress=none`, watermark). No `cell=`, no
watermark, no `merge_pages` in v1.

### Stats (read-only)

`struct shmem_comp_stats` (via `comp_stats` sysfs) is **gauges, not
counters** -- this is the trap that hid a smoke-test "failure" twice. Reads
decompress and frees release entries, so a snapshot after the data is gone
reads zero. Live gauges must be sampled while data is resident:

```
compressed_pages, uncompressed_pages,
total_compressed_bytes, total_compressed_bytes, ratio,
compress_fails, decompress_fails, stale_value_retries,
ram_pool_bytes, ram_entry_bytes, ram_entries, ram_total_bytes
```

`ram_total_bytes` vs the uncompressed size of the same data is the honest
measure of whether compression saves RAM. Read-only in v1; the global
writable sysfs defaults are cut from the upstream slice.

### Arm 3: the cell-backed bulk tier (later phase; the payload)

v1's zsmalloc backend is the wedge. The payload is a second backend -- the
**cell allocator** (`mm/cell_store.c`) -- selected per-pool, where a
compressed block is stored in a cell (a compound page, bump-allocated,
byte-packed) instead of a zsmalloc object. The cell tier's properties
appreciate exactly at small RAM: O(1) bump allocation, zero per-object
metadata, a floor of one cell, and synchronous return to the buddy on free.
The entry API, the two-counter refcounting, and the xarray encoding are
unchanged; only `shmem_comp_store/load/free` dispatch to the cell backend
when `pool->use_cells`.

Arm 3 is driven through the same `test_zbench` harness (the session-33 rule:
no blind zcell changes). Its known improvement targets are the cell_order
axis, the fixed-size directory wall at small orders, and the GC /
synchronous-free rm story. The cell allocator is shared *conceptually* with
`blkzcell` (`specs/01_BLKZCELL_NAND_LAYER.md`) but shares no code path: one
compresses RAM-resident tmpfs folios, the other manages flash segments below a
block device.

### Build changes

| Target | Change |
|---|---|
| `fs/Kconfig` | `CONFIG_TMPFS_COMPRESSION` (bool, default n, depends TMPFS) selecting `CRYPTO` acomp + lz4, `ZSMALLOC` |
| `mm/Makefile` | `shmem_compress.o` (and `cell_store.o` in the arm-3 phase) |
| `mm/shmem.c` | the ~16 hook hunks (write_end, get_folio, release_folio, undo_range, put_super, show_options) + 3 fs_parameters |
| `mm/shmem_compress.c` | the core: pool, entry table, `entries[]` per-inode map, data path, stats, sysfs |
| `include/linux/shmem_compress.h` | v1 API with the ownership contracts |
| `include/linux/shmem_fs.h`, `mm/internal.h` | plumbing (no exports; `shmem.c` and `shmem_compress.c` link into the same image) |

The two-table mapping **removes** the earlier `SWP_SHMEM_COMPRESS` /
`swapops.h` `{id,gen}`-in-swp_entry encoding: the compressed mapping lives in
the per-inode `entries[]` xarray, not multiplexed into the page cache. No
reserved swap type, no `swp_entry` packing.

### File manifest

| File | Action | Purpose |
|---|---|---|
| `specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md` | Create | This spec |
| `mm/shmem_compress.c` | Modify (exists on `upstream/s1`) | core: pool, entry table, data path, stats, sysfs |
| `mm/cell_store.c` | Later phase | cell bulk-tier backend (arm 3) |
| `mm/shmem.c` | Modify | hook hunks + 3 mount options |
| `include/linux/shmem_compress.h` | Modify | v1 API |
| `include/linux/shmem_fs.h`, `mm/internal.h`, `mm/Makefile`, `fs/Kconfig` | Modify | plumbing |
| `Documentation/filesystems/tmpfs.rst` | Modify | mount options + `comp_stats` docs (Sphinx-verify) |
| `tools/testing/selftests/tmpfs/` | Create | port the s1 smoke gate + remount-cycle gate |

### Migration

The v1 slice is already implemented and green on `upstream/s1` (3 commits,
zero-warning build, checkpatch 0 errors, smoke gate 6/6 on KASAN). The phases
below are the *remaining* path to upstream, plus the arm-3 payload.

**Phase 1 -- v1 slice (DONE on `upstream/s1`).** zsmalloc backend, 3 mount
options, eager write-end compression, read-only stats, graceful pool failure.
Gate: `test_s1_smoke` 6/6.

**Phase 2 -- the two-table mapping + "both" lifecycle.** Implement `entries[]`,
the compressed/uncompressed/both states, the entry pin via `entries[N]`,
write-invalidate, and reclaim drop-folio-keep-entry. Remove the
`SWP_SHMEM_COMPRESS` swap-type encoding. Gate: the 3G/2G read-back works on a
plain `read()` (no OOM), plus the existing smoke/remount gates.

**Phase 3 -- memory-failure answer.** A memory-failure hook for compressed
entries (the hook surface is known; a standing s1 item). Keeps the build
green; a corrupted compressed entry must not poison a read.

**Phase 4 -- in-tree selftests.** Port `test_s1_smoke` + the remount-cycle
gate into `tools/testing/selftests/`. The zbench methodology ports.

**Phase 5 -- docs.** Port the rst to v1's exact surface (3 options,
`comp_stats`), Sphinx-verify (blocked on no-install in the environment).

**Phase 6 -- RFC assembly (local until posted).** Cut the fine-grained series
from the branch: prep -> two-table mapping -> dormant core -> decompress ->
compress -> truncate release -> options -> mf handling -> stats -> selftests
-> docs. The two-table mapping means the RFC **no longer carries a reserved
swap type**, removing the encoding question from the cover letter; it leads
with the arm 1/2 rationalization table.

**Phase 7 -- arm 3, the cell bulk tier (payload; parallel track).** Add the
cell backend to the pool, drive it through `test_zbench` with the three named
improvement targets. Separate series, after v1 has a shape.

### Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| 1.9x write / 1.3x read CPU cost is a hard sell vs zram on CPU-bound boxes | Med | Med | The story targets the survival regime (64M-1G) where arm 1 dies; the 2G build boxes are honestly arm 1's home and get no work |
| A hot compressed page costs folio (4K) + blob (~2K) = ~6K, more than uncompressed alone | Med | Med | The uncompressed folio is reclaimable (the entry survives), so under pressure it drops to compressed-only; the steady state is ~2K/page + the hot set. Priced in the Motivation as the survival trade |
| The `entries[]` + folio double-charge confuses shmem's `alloced == nrpages + swapped` accounting | Med | High | Compress charges `swapped` (the blob) and the folio charges `nrpages`; reclaim uncharges the folio on drop without losing the entry's charge. Verified in the Phase-2 gate with an accounting assertion |
| `entries[N]` staleness: a reader that pinned an id while truncate cleared it could act on a recycled id | Low | High | A generation on the entry (validated at pin time) rejects the stale id; simpler than the old value-encoding but not zero |
| Stats-as-gauges misread by reviewers as "compression did not run" | High | Low | Documented in the rst and the sysfs help; live-sampling rule stated at every snapshot |
| Large-folio / THP tmpfs silently uncompressed | Med | Low | Explicit `folio_order` skip in v1; a documented limitation, not a silent one |
| Cell tier (arm 3) does not beat zsmalloc on the bench, orphaning the payload | Med | High | Arm 3 is driven through `test_zbench` before any blind cell change (session-33 rule); the verdict is measured, and parking is an accepted outcome (cellzram precedent) |
| KASAN corrupts RAM-measurement numbers | High | Med | RAM benches run a non-KASAN kernel (session-39 rule); correctness gates keep KASAN |

### Open Questions

- **The `entries[]` staleness generation** -- whether a full generation
  counter is needed or a lighter epoch suffices, given `entries[]` is now a
  dedicated table (not multiplexed with real swap entries). Pinned down during
  the Phase-2 implementation.
- **Hot-page premium tuning** -- whether to proactively drop clean cached
  folios under low pressure (not just at the reclaim watermark) to keep the
  uncompressed footprint near the hot set. Deferred until the Phase-2 numbers
  exist.
- **Arm-3 backend value:** does the cell tier beat zsmalloc on the target
  fleet, measured? Determines whether arm 3 is a second series or parked.
- **Threshold default:** the v1 `compress_threshold` default (1 = compress
  everything vs a sub-page floor) tuned against the bench.
- **Large folios:** whether v1 should compress `folio_order > 0` or document
  it as unsupported. Currently skipped.

### References

- branch `upstream/s1` (3 commits) -- the v1 reimplementation off Linux
  7.3-rc6; the source of truth for the code described here.
- `mission_log/2026-10-11_upstream-s1-reimplementation.md` -- the v1 slice,
  its build/gate state, and the standing s1 items.
- `mission_log/2026-10-12_fill-test-and-two-table-mapping.md` -- the fill
  test, the read-back OOM, and the two-table mapping decision (the source of
  the "both" lifecycle and the removal of the swap-type encoding).
- `mission_log/2026-10-11_ztmpfs-rationalization-bench.md` -- the 3-arm bench,
  the arm 1/2 numbers in Motivation, the stats-as-gauges and non-KASAN rules.
- `mission_log/2026-10-11_openwrt-refocus.md` -- the fleet, the refocus on
  tmpfs and tmpfs+zcell, the arm-3 framing.
- `mission_log/2026-10-05_tmpfs-compression-foundation.md` and the series
  logs 1-21 -- the prototype history the v1 slice was carved from.
- `mm/shmem_compress.c`, `include/linux/shmem_compress.h` -- the v1 core and
  API with the ownership contracts.
- `mm/cell_store.c` -- the cell allocator (arm 3 bulk tier); also the shared
  discipline behind `blkzcell` (`specs/01_BLKZCELL_NAND_LAYER.md`).
- `specs/01_BLKZCELL_NAND_LAYER.md` -- the companion block layer for
  SD/MMC; shares the cell-allocator concept, no code path.
- `specs/03_ZTMPFS_ZCELL_FUSION.md` -- arm 3; inherits this page lifecycle and
  the two-table mapping.

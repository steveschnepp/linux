# ztmpfs + zcell: RAM-Only Cell Bulk Tier Specification

## Overview

This is the fusion of `ztmpfs` (`specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md`)
and the cell allocator that also backs `blkzcell`
(`specs/01_BLKZCELL_NAND_LAYER.md`): the zcell storage system **adapted to RAM
only** and bolted onto ztmpfs as its compression-pool backend. A ztmpfs mount
with `compress=lz4,cell=ORDER` stores each compressed page in a
bump-allocated, byte-packed cell instead of a zsmalloc object. It is the same
cell discipline as blkzcell -- O(1) bump allocation, zero per-object metadata,
a floor of one cell, synchronous return to the buddy -- with all the
flash-specific machinery removed.

This is **arm 3**, the payload of the whole series. Arms 1-2 are measured;
arm 3 is what the cell work has been driving toward.

## Motivation

ztmpfs v1 (arm 2) compresses into zsmalloc. zsmalloc is a general compressed-
object store: size classes, per-object bookkeeping, its own page management.
On the survival-regime fleet -- 64M-1G SBCs (`openwrt-refocus`) -- every byte
of allocator metadata and every retained partially-free page is a direct tax
on the memory savings compression is supposed to deliver. At small RAM the
dominant cost is not the compressed data but the allocator's residue that
never returns to the buddy.

The cell allocator removes that residue by construction:

- **Zero per-object metadata.** A block is stored at its exact compressed
  length; no size class, no free list, no per-object header. Every byte in a
  cell is live data.
- **Synchronous buddy return.** A cell whose `live_bytes` hits zero frees its
  pages to the buddy before `cell_free()` returns. MemFree tracks tmpfs usage
  exactly, with no shrinker lag and no retained slab cache.
- **O(1) bump allocation.** No fragmentation analysis, no search. One pointer
  advance.
- **A floor of one cell.** The smallest non-empty footprint is one cell
  (default 64 KiB), not zsmalloc's page-per-zspage overhead.

The cell system's flash twin (`blkzcell`) proves the same allocator under a
block device. Here it serves RAM-resident tmpfs folios, where the mapping is
already in the page cache and nothing needs to survive a reboot -- so the
index dumps, checkpoints, recovery scan, and erase-group discard that
`blkzcell` requires are all absent. That asymmetry is the entire reason this
spec is short.

## Goals

- Store ztmpfs compressed pages in a cell backend (`cell_store.c`) selected
  per-pool, behind the existing `store/load/free` contract.
- Return cell memory to the buddy synchronously on free, so MemFree reflects
  live tmpfs usage without a shrinker.
- Preserve the v1 entry API, `{id,gen}` xarray encoding, two-counter
  refcounting, and stats surface unchanged -- the backend swap is invisible
  above `shmem_comp_store/load/free`.
- Drive the cell tier's three known improvement targets (cell_order axis,
  small-order directory wall, GC/synchronous-free rm story) through the
  `test_zbench` harness, not blind.

## Non-Goals

- **No index dumps, no checkpoint, no recovery.** tmpfs is volatile; the
  page-cache xarray *is* the mapping and dies with the mount. Nothing is
  persisted. (This is the defining difference from `blkzcell`.)
- **No discard, no erase groups, no segments.** Those are flash-geometry
  concerns; cells are compound RAM pages.
- **No cross-mount or global cell pool in v1.** One cell pool per mount, like
  the zsmalloc pool.
- Not a block device, not swap. RAM-resident folios only.
- Not merged groups (`merge_pages`) in v1 -- single-page records, matching the
  v1 slice. The cell backend supports larger blocks (up to 4 MiB cells) but
  v1 feeds it one page at a time.
- No remount backend switch. `cell=` is set at mount; a pool never changes
  backend after creation.

## Design

### The fusion, in one picture

```
ztmpfs (spec 02)                          this spec (arm 3)
  write_end -> compress -> store  --+
  read/fault <- decompress <- load  |    dispatch on pool->use_cells:
  entry free -> free          ------+      use_cells=false -> zs_malloc  (arm 2)
                                          use_cells=true  -> cell_store (arm 3)

blkzcell (spec 01) -- the SAME cell_store.c, but RAM cells backing a block
device, wrapped in segments + index dumps + recovery + discard. Those wrappers
are exactly what this spec omits.
```

`mm/cell_store.c` and `include/linux/cell_store.h` are the shared allocator.
This spec adds no new allocator code; it wires the existing cell pool into
ztmpfs's compression pool and removes nothing, because the flash wrappers were
never in `cell_store.c` -- they live (or would live) in `blkzcell.c`.

### Backend dispatch (already in the prototype)

The compression pool carries exactly one backing store, chosen at creation:

```c
struct shmem_comp_pool {
	/* exactly one of zs_pool / cells is non-NULL; use_cells selects */
	union { struct zs_pool *zs_pool; struct cell_pool *cells; };
	bool use_cells;   /* set by cell= at mount; never changes */
	...
};
```

`shmem_comp_store()` / `shmem_comp_load()` / `shmem_comp_free()` dispatch on
`use_cells` (the dispatch sites exist on `feature/tmpfs-compression`):

```c
/* store */
if (pool->use_cells) {
	if (cell_store(pool->cells, data, len, &cellid, &offset))
		return 0;                             /* -ENOMEM -> keep raw */
	handle = shmem_comp_pack_loc(cellid, offset); /* {cellid,offset} in one ulong */
} else {
	handle = zs_malloc(pool->zs_pool, len, gfp, NUMA_NO_NODE);
}

/* load */
if (pool->use_cells)
	return cell_load(pool->cells, cellid, offset, dst, len);

/* free */
if (pool->use_cells)
	cell_free(pool->cells, cellid, offset, len);
else
	zs_free(pool->zs_pool, handle);
```

The `handle` for a cell-backed entry is not a zsmalloc pointer -- it is the
packed `{cellid, offset}` location (`shmem_comp_pack_loc`): cellid biased
+1 so `{cellid=0, offset=0}` never encodes to the handle-0 "storage failed"
sentinel, offset in the low 22 bits (a cell is at most 4 MiB). One
`unsigned long` carries the whole location, so the entry table and the
`{id,gen}` xarray encoding are byte-for-byte identical across backends.

### The cell itself (unchanged, RAM-only)

From `cell_store.h`: a cell is one physically-contiguous compound page (a
single `alloc_pages(order)` call), used as a bump arena. The invariants that
this fusion depends on:

1. **Only the active cell grows**; the bump pointer is monotonic, never
   decreased in place. Holes from freed blocks are reclaimed by evacuating the
   whole cell, never by reusing gaps. This is O(1) with no fragmentation
   analysis -- and it is *fine* in RAM because eviction is a memcpy, not a
   flash write.
2. **A cell with `live_bytes == 0` returns to the buddy synchronously** -- no
   deferred work, no RCU. `cell_free()` returning means the pages are on the
   buddy freelist. This is the property that makes MemFree track tmpfs usage.
3. **`cell->base` is virtually contiguous** (direct map over one compound
   page), so a block spanning a page boundary inside the cell is a single
   memcpy. No kmap, no bounce buffer.
4. **One directory slot beyond `max_cells` is held for compaction**, not for
   stores, so a drain of a full directory has a bootstrap destination.

`CELL_ORDER_MIN=2` (16 KiB), `CELL_ORDER_DEF=4` (64 KiB), `CELL_ORDER_MAX=10`
(4 MiB). A cell is GC-sparse when `live_bytes * 2 <= cell_size`.

### Why there is no index dump / recovery here

`blkzcell` needs an on-media index (sector -> segment/offset/len), a
monotonic-seq checkpoint, a drop-scan recovery walk, and erase-group discard
because its data outlives the mapping in volatile RAM and lives on a card the
FS cannot re-derive. **None of that applies to tmpfs + cell:**

| flash concern (blkzcell) | why absent in RAM (this spec) |
|---|---|
| index dump to media | the mapping is the page-cache xarray; it is the index |
| monotonic-seq checkpoint | no persistence; the mount is volatile |
| drop-scan recovery walk | nothing to recover; a reboot loses tmpfs by definition |
| erase-group-aligned discard | cells are buddy pages, freed synchronously |
| segment = erase group, append-only on media | a cell is a RAM arena; "append-only" is just the bump pointer |

The handle `{cellid, offset}` is a RAM location valid for the life of the
entry, guarded by the entry reference pin (a reader pins before `cell_load`,
so the cell cannot be returned to the buddy mid-read). Correctness rests
entirely on the two-counter refcounting and the cell rwsem -- both already in
spec 02 and `cell_store.h` -- not on any on-media structure.

### GC: shrinker-driven evacuation

`cell_free()` only links a sparse cell onto `cp->sparse`; evacuation runs
asynchronously under the shrinker (memory pressure) or the debug hook
(`shmem_comp_cell_gc_all()`). One pass (`GC_MAX_VICTIMS=64` sequential
drains) in `shmem_compress.c`:

```
down_write(gc_lock)
for each victim in cp->sparse (via cell_pool_pick_victim, never the active cell):
    scan the entry table for blocks residing in victim
    for each live block:
        cell_relocate_one(victim -> active)   // bump-alloc + memcpy
        update the entry's {cellid, offset}   // via generation-safe swap
    cell_pool_release_victim(victim)           // empty -> buddy; else re-sparse
up_write(gc_lock)
```

Lock order (from `cell_store.h`): a cell's rwsem is always taken **before**
`cp->lock`; GC holds `down_write(victim)` then takes `cp->lock` for the bump
allocation; `cell_free()` takes `down_read` when it might return a cell, then
`cp->lock` for the recheck. The `gc_lock` serialises passes so two shrinker
scans cannot relocate the same object concurrently.

Relocation is a RAM memcpy and the entry's handle is rewritten under the same
generation, so a reader pinned on the old location retries (stale-value
rejection) and finds the new one. This is why no write-ahead or shadow mapping
is needed: the xarray slot update *is* the commit.

### Mount option and selection

`cell=ORDER` (e.g. `cell=4` for 64 KiB cells, the default derivation; `cell=0`
= zsmalloc / no cells). Parsed by `shmem_comp_opts_set_cell()`. At
`fill_sb()`, a cell-backed pool calls
`cell_pool_create(order, use_thp, nr_cells)` with `nr_cells` derived from the
tmpfs size plus COW headroom (0 = `CELL_DEFAULT_MAX_CELLS=256`). The backend
is fixed for the pool's life; a cell-pool creation failure is graceful (the
mount runs uncompressed, or falls back per policy -- never fails the mount).

### Stats

`comp_stats` is identical across backends. The cell-specific live figures
(`ram_pool_bytes` for a cell pool = `pages_used << cell_order`) surface
through the same `ram_pool_bytes` field, so the honest "does this save RAM"
number (`ram_total_bytes` vs uncompressed size) is measured the same way for
zsmalloc and cell backends. Gauges, not counters -- sampled while resident.

### What the three improvement targets are (the arm-3 payload)

These are the open cell questions the bench must drive (session-33 rule: no
blind cell changes):

1. **cell_order axis.** Larger cells amortize per-cell metadata and directory
   slots but raise the floor and the compaction memcpy cost; smaller cells
   lower the floor but fill the directory. Sweep `cell_order` 2..10 against
   RAM and throughput on the target fleet points.
2. **Small-order directory wall.** At small `cell_order` the fixed-size
   directory (`max_cells`) fills fast and the compaction-only slot becomes the
   bottleneck; characterize where the wall is and whether the directory should
   grow.
3. **GC / synchronous-free rm story.** `rm -Rf` frees thousands of blocks;
   verify the synchronous buddy return actually converges MemFree without
   OOM, and price the shrinker's evacuation cost under the churn workload.

### Build changes

| Target | Change |
|---|---|
| `fs/Kconfig` | `CONFIG_TMPFS_COMPRESSION` gains the cell backend (already gated by it; `cell_store.o` added to `mm/Makefile` under the same config) |
| `mm/Makefile` | `cell_store.o` linked when `CONFIG_TMPFS_COMPRESSION` |
| `mm/cell_store.c`, `include/linux/cell_store.h` | unchanged -- shared allocator, already present |
| `mm/shmem_compress.c` | the `use_cells` dispatch, cell pool create/destroy, GC pass (present on `feature/tmpfs-compression`, cut from `upstream/s1`) |
| `mm/shmem.c` | `cell=` option (`shmem_comp_opts_set_cell`) |
| `include/linux/shmem_compress.h` | `cell_order` in opts, `shmem_comp_pool_cells()`, `cell_gc_all()` |

### File manifest

| File | Action | Purpose |
|---|---|---|
| `specs/03_ZTMPFS_ZCELL_FUSION.md` | Create | This spec |
| `mm/cell_store.c` | Reuse (exists) | the shared cell allocator; no changes |
| `include/linux/cell_store.h` | Reuse (exists) | cell/pool structs + invariant contract |
| `mm/shmem_compress.c` | Modify | `use_cells` dispatch, cell pool lifecycle, GC pass (port from `feature/tmpfs-compression`) |
| `include/linux/shmem_compress.h` | Modify | `cell_order` opt, `shmem_comp_pool_cells()`, `shmem_comp_cell_gc_all()` |
| `mm/shmem.c` | Modify | `cell=` mount option |
| `mm/Makefile` | Modify | `cell_store.o` under `CONFIG_TMPFS_COMPRESSION` |
| `tools/testing/selftests/tmpfs/` | Modify | cell-backed smoke + churn/rm gates, cell_order sweep |

### Migration

Builds on the v1 slice (spec 02 Phase 1, done on `upstream/s1`). The cell
backend already exists on `feature/tmpfs-compression` and was cut from the v1
slice; this phase re-introduces it as arm 3, separately from the upstream v1
series.

**Phase 1 -- wire the cell backend into the s1 pool.** Port the `use_cells`
dispatch, cell pool create/destroy, and the `cell=` option onto the `s1`
slice. Gate: mount `cell=4`, write/read/truncate round-trip identical to the
zsmalloc smoke gate. *~1-2 days.*

**Phase 2 -- GC pass + shrinker.** Port the evacuation pass and shrinker
registration. Gate: the churn gate (random overwrite of incompressible data)
converges; no KASAN on the relocation path. *~2 days.*

**Phase 3 -- the three improvement-target benches.** Drive cell_order sweep,
the small-order directory wall, and the rm/synchronous-free story through
`test_zbench` at the 256M and 64M fleet points, arms 2 (zsmalloc) vs 3 (cell)
on the same kernel. This is the payload measurement. *~3 days.*

**Phase 4 -- verdict.** Compare arm 3 vs arm 2 on RAM, write, and read. If
the cell tier wins on the survival regime, it becomes the default cell backend
for the fleet; if not, it is parked with its measurements (cellzram
precedent: a correct, measured, dormant backend is a valid outcome). *~1 day.*

### Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Cell tier does not beat zsmalloc on the bench, orphaning the payload | Med | High | Arm 3 is measured through `test_zbench` before any default is chosen; parking with measurements is an accepted, principled outcome |
| At small cell_order the directory wall stalls the write path under churn | Med | Med | Improvement-target sweep (Phase 3) prices the wall; the compaction-only slot already bootstraps a full-directory drain |
| Synchronous buddy return does not converge MemFree under `rm -Rf` without OOM at 64M | Med | High | The rm/synchronous-free gate (Phase 3); if it does not converge, add a bounded synchronous drain before OOM |
| GC evacuation (memcpy under shrinker) adds latency the shrinker cannot afford at 64M | Med | Med | `GC_MAX_VICTIMS` bounds a pass; measure shrinker stall in Phase 3; tune the sparse threshold |
| A reader pinned on a relocated block misses the entry update | Low | High | Generation-safe swap + stale-value retry (already in v1); the cell rwsem excludes return-during-load |
| `cell=` reopens the option surface the v1 slice deliberately cut | Low | Med | `cell=` is arm-3-only, not part of the upstream v1 series; kept separable so the RFC diff carries no cell code |

### Open Questions

- **Default cell_order for the fleet.** Derived from tmpfs size at mount, or a
  fixed 64 KiB? Tuned in the Phase-3 sweep.
- **Directory growth vs fixed wall.** Whether `max_cells` should grow on
  demand at small orders, or whether the wall is acceptable and simply priced.
- **Backend default.** Whether cell or zsmalloc is the eventual default for
  the survival regime -- decided by the Phase-4 verdict, not before.
- **Synchronous vs bounded-drain free.** Whether `cell_free()` should ever
  hand back a batch instead of one cell, if single-cell return proves too
  spiky at 64M.

### References

- `specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md` -- the ztmpfs host feature
  (v1 slice, entry API, `{id,gen}` encoding, refcounting, stats).
- `specs/01_BLKZCELL_NAND_LAYER.md` -- the flash twin of the same cell
  allocator; the source of the "what RAM omits" contrast table.
- `mm/cell_store.c`, `include/linux/cell_store.h` -- the shared cell
  allocator and its six design invariants.
- `feature/tmpfs-compression` branch -- where the `use_cells` dispatch and GC
  pass already live; the port source.
- `mission_log/2026-10-11_openwrt-refocus.md` -- arm-3 framing, the survival
  regime, the "zcell = arm 3 bulk tier only" scope.
- `mission_log/2026-10-11_ztmpfs-rationalization-bench.md` -- arms 1-2
  measured; arm 3 reserved in `test_zbench`; the session-33 no-blind-cell rule.
- `mission_log/2026-10-07_cellzram-block-device.md` and the cellzram series
  -- the cell allocator's origin and the O_DIRECT / unit-suffix / gauges
  lessons.

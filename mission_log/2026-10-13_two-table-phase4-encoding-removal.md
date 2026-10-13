---
name: two-table-phase4-encoding-removal
description: phase 4 of the two-table mapping -- remove the now-dead SWP_SHMEM_COMPRESS reserved swap type and its {id,gen} packers, dissolving the series' biggest upstream review risk
project: linux
previous: 2026-10-13_two-table-phase3-write-invalidate.md
---

# Mission Log: Two-Table Phase 4, Removing the Swap Encoding

Part 31 of the tmpfs transparent-compression series.  Session 47
(2026-10-13).  Phase 4, the last of the two-table mapping phases: delete
the `SWP_SHMEM_COMPRESS` reserved swap type and the `{id,gen}` packers
that encoded compressed pages into the page-cache xarray.  Since Phase 2
the compressed mapping has lived entirely in the per-inode `entries[]`
table, so all of this was dead code -- the work was proving it was dead
and removing it cleanly.

Everything is committed; the kernel tree is clean; the in-tree kernel is
back to KASAN.

## What We Did

### 1. Mapped every reference before touching anything

Grepped the tree for the whole encoding surface: `SWP_SHMEM_COMPRESS`,
`make_shmem_comp_entry`, `shmem_comp_entry_id`/`_gen`,
`is_shmem_comp_entry`, `xa_is_shmem_comp`, `shmem_comp_type_check`,
`shmem_comp_free_swap`, `SWP_SHMEM_COMPRESS_NUM`, and the
`SHMEM_COMP_ENTRY_GEN_*` mask.  Confirmed which were live: after Phase 2,
the only consumer of a comp swap value left in the tree was the dead
branch in `shmem_free_swap()` (i_pages never holds a comp value now), and
the predicates only referenced each other.  The `GEN_BITS`/`GEN_MASK`
mask is the one live piece -- `shmem_comp_pack()` (the entries[] table)
uses it.

### 2. Removed the encoding, kept the generation mask

- **swapops.h**: dropped the four swap packers
  (`make_shmem_comp_entry`, `shmem_comp_entry_id`, `shmem_comp_entry_gen`,
  `is_shmem_comp_entry`).  Kept `SHMEM_COMP_ENTRY_GEN_BITS`/`_MASK` and
  rewrote their comment for the entries[] use (the old rationale -- the
  55-bit swp offset budget -- no longer applied).
- **swap.h**: removed `SWP_SHMEM_COMPRESS` and
  `SWP_SHMEM_COMPRESS_NUM`, and dropped the `- SWP_SHMEM_COMPRESS_NUM`
  term from `MAX_SWAPFILES`.  Every reserved swap type is referenced
  symbolically (`SWP_HWPOISON`, `SWP_MIGRATION_*`, `SWP_DEVICE_*`,
  `SWP_PTE_MARKER`), so the one-slot renumbering is safe and the total
  still fits `MAX_SWAPFILES_SHIFT`; `MAX_SWAPFILES` simply gains one
  regular swap-file slot.
- **shmem_compress.h**: removed the dead predicates `xa_is_shmem_comp`
  and `shmem_comp_type_check`, the `shmem_comp_free_swap` declaration,
  and updated the overview/comments that still described the page-cache
  encoding.
- **shmem_compress.c**: removed `shmem_comp_type_check` (def +
  export) and `shmem_comp_free_swap` (def), and fixed the comments that
  named them.
- **shmem.c**: removed the dead comp branch in `shmem_free_swap()`.

Net -212 lines.  The compressed mapping is now a private per-inode
xarray -- closer to a btrfs extent map than to a swap entry.

## What We Learned

### Technical

- **The encoding was fully dead after Phase 2, which is the point of the
  phasing.**  Encoding removal was deliberately scheduled last so the old
  path stayed a fallback until `entries[]` was authoritative; by Phase 4
  the only reference left was one dead `if` branch.  The residue of a
  removed design is easiest to delete once nothing depends on it.

- **Removing a reserved swap type is safe because every type is
  symbolic.**  `SWP_SHMEM_COMPRESS` sat above `SWP_PTE_MARKER`; deleting
  it raised `MAX_SWAPFILES` by one and shifted the reserved types up by
  one, but nothing hardcodes a type number -- they are always compared via
  their constants, and the total still fits `MAX_SWAPFILES_SHIFT`.

- **The generation mask outlives the encoding that motivated it.**  It
  moved from "pack {id,gen} into a swp offset" to "pack {id,gen} into an
  entries[] xa_value" -- same staleness problem, same23-bit generation,
  different container.  Keeping the mask and updating its comment was
  cheaper and clearer than relocating it.

### Process

- **A full grep of the encoding surface before editing turned the removal
  into transcription.**  Knowing exactly which of the eight symbols were
  live (one: the GEN mask) meant no guessing and no "did I miss a caller"
  build cycle.

- **Re-running both gates on a dead-code removal is cheap insurance.**  A
  removal "cannot" regress, but the swap-type renumbering touched
  `MAX_SWAPFILES`, so confirming the KASAN suite (both backends) and the
  read-back gate still passed was worth the two builds.

## What We Decided

- **The reserved swap type is gone.**  This was the single biggest
  upstream review risk the series had flagged (a reserved swap type
  multiplexed into the page cache with folios and real swap entries); the
  RFC no longer carries an encoding question.

- **The generation mask stays in swapops.h** for now.  It is only used by
  the entries[] pack, so it could move to shmem_compress.h later, but the
  churn was not worth it in a removal commit.

## What We'd Do Differently

- Nothing significant.  The removal was the cleanest of the four phases
  because the phasing (encoding last) did its job; if anything, the
  earlier phases could have deleted the dead `shmem_free_swap` branch as
  soon as Phase 2 made it unreachable, rather than leaving it for Phase 4.

## Files Changed

| File | Purpose |
|------|---------|
| `include/linux/swapops.h` (`2ade38871ab9`) | removed the four {id,gen} swap packers; kept + re-commented the generation mask |
| `include/linux/swap.h` (`2ade38871ab9`) | removed SWP_SHMEM_COMPRESS + NUM; MAX_SWAPFILES reclaims the slot |
| `include/linux/shmem_compress.h` (`2ade38871ab9`) | removed xa_is_shmem_comp, shmem_comp_type_check, shmem_comp_free_swap decl; updated overview/comments |
| `mm/shmem_compress.c` (`2ade38871ab9`) | removed shmem_comp_type_check + shmem_comp_free_swap; comment fixes |
| `mm/shmem.c` (`2ade38871ab9`) | removed the dead comp branch in shmem_free_swap() |

## Test Results

| gate | result |
|---|---|
| build (KASAN, full) | 0 errors, 0 warnings |
| full KASAN suite (zsmalloc + `cell=64K`) | exit 0, all ALL PASS, 0 KASAN |
| 3G/2G read-back gate, zsmalloc (arm 2) | wrote 3072 MB, read 3072 MB, CRC ok, no OOM |

## Next Steps

The two-table mapping (Phases 1-4) is complete: the compressed mapping
lives in `entries[]`, decompressed pages are reclaimable file-cache
pages, writes invalidate the stale backing, clean caches are not churned,
and the reserved swap type is gone.

Standing items unchanged: M-boot the perf numbers, cell_order sweep, the
arm-3 rm/synchronous-free story at 64M, and (optional) the arm-3 cell
capacity limit for large fills.  The RFC assembly (spec 02's Phase 6) is
the natural next track.

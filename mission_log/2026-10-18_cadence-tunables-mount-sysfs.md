---
name: cadence-tunables-mount-sysfs
description: make the aging/GC cadence tunables (age_delay_ms, age_budget, age_rounds, age_batch, gc_debounce_ms) per-mount mount options + RW sysfs defaults; clamp bounds stay constants; the R/O whole-state-per-mount file is deferred
project: linux
previous: 2026-10-18_zcell-device-removal.md
---

# Mission Log: Mount + Sysfs for the Aging/GC Cadence Tunables

Part 56 of the tmpfs transparent-compression series.  Same day as the
zcell-removal log (previous).  A tuning-surface change: five internal
constants become per-mount mount options and RW sysfs defaults, so the
aging/GC cadence is tunable per mount (and the global default at
runtime) instead of hardcoded.

## The five tunables

| tunable | constant (default) | what it tunes |
|---|---|---|
| `age_delay_ms=` | `SHMEM_COMP_AGE_DELAY_MS` (1000) | aging-pass period |
| `age_budget=` | `SHMEM_COMP_AGE_BUDGET` (128) | pages compressed per round |
| `age_rounds=` | `SHMEM_COMP_AGE_MAX_ROUNDS` (4) | rounds per aging pass |
| `age_batch=` | `SHMEM_COMP_AGE_BATCH_INODES` (64) | inodes pinned per pass |
| `gc_debounce_ms=` | `SHMEM_COMP_CELL_GC_DEBOUNCE_MS` (20) | demand-GC debounce |

The clamp bounds stay constants (`SHMEM_COMP_GC_PERCENT_MIN/MAX`,
`SHMEM_COMP_MERGE_MAX`) -- those are validation limits, not tunables.

## What We Did

### Step 1 (commit `81b1f6bab5e0`): mount options

Added the five as per-mount mount options, following the existing
`merge_pages`/`gc_percent` pattern (explicit `*_seen` flags so 0 is a
legal value distinct from "not given"; validated ranges in the parser).
The tuning defaults moved from `shmem_compress.c` to
`<linux/shmem_compress.h>` so the mount parser (validation) and the
engine share one definition.

- The four `age_*` are per-mount: fields in `shmem_comp_sb`, read by
  `shmem_comp_maintain()`.  The inode batch stays a fixed-size stack
  array capped at `SHMEM_COMP_AGE_BATCH_INODES` (tunable downward via
  `age_batch=`, not upward -- the array bound is the cap).
- `gc_debounce_ms` is per-pool (it drives the demand-GC armer), so it
  flows through `shmem_comp_pool_create()` into `pool->gc_debounce_ms`;
  the armer reads that.
- `mount` prints the resolved values.

### Step 2 (commit `1ec90767d838`): RW sysfs defaults

Exposed the same five under `/sys/fs/tmpfs/compression/` as RW (0644)
attributes, same pattern as `merge_pages`/`compress_watermark`
(`shmem_comp_defaults_lock`, validated store, a macro pairing
show/store per attr to keep boilerplate down).  `fill_sb()` now resolves
each from the **sysfs default** (via a locked combined getter) when the
mount option is absent.  Resolution order matches the other tunables:
**mount option > sysfs default > built-in constant.**  The built-in
`SHMEM_COMP_AGE_*` / `SHMEM_COMP_CELL_GC_DEBOUNCE_MS` constants remain
the initial values of the globals.

Existing defaults (`compression`, `compressor`, `merge_pages`,
`compress_watermark`) stay RW unchanged.

## What We Learned

- **`age_batch` has a hard cap** (the fixed-size inode batch array on
  the maintain stack), so it is tunable downward but not upward past
  `SHMEM_COMP_AGE_BATCH_INODES`.  The other four are freely tunable in
  their validated range.
- **`gc_debounce_ms` is per-pool, not per-mount** -- it had to ride
  `pool_create()` rather than live in `shmem_comp_sb`, unlike the four
  `age_*` fields.  Worth remembering for any future per-pool tunable.
- The macro-paired show/store keeps a five-attribute RW block to a
  handful of lines instead of ten near-identical functions.

## What We Decided

- **All five become mount options + RW sysfs defaults**; clamp bounds
  stay constants.
- **Resolution order** mount > sysfs-default > built-in, matching the
  existing tunables.
- **Step 3 deferred** (not built): a read-only sysfs file showing the
  whole state, one line per mount `<mountpoint> <mount options>`.  The
  user wants to refine that design first.

## Rules Added

- **A per-pool tunable rides `pool_create()`, not `shmem_comp_sb`** (the
  sb is per-mount; the pool is the shared object the per-pool machinery
  like the GC armer reads).
- **A tunable backed by a fixed-size stack array must declare its cap**
  (`age_batch` -> `SHMEM_COMP_AGE_BATCH_INODES`) and be documented as
  bounded in that direction.

## What We'd Do Differently

- Would have made the five sysfs defaults from the start in the same
  pass as the mount options (they are two halves of one tuning surface);
  splitting them into two commits was for bisectability per the user's
  request, which is fine, but the design is one change.

## Files Changed

| File | Purpose |
|------|---------|
| `include/linux/shmem_compress.h` | +5 opts fields, tuning defaults moved here, `pool_create()` +gc_debounce_ms |
| `mm/shmem.c` | +5 Opt_ enum / fsparam / parse cases (validated ranges) |
| `mm/shmem_compress.c` | +sb `age_*` fields, +pool `gc_debounce_ms`, fill_sb resolution, +5 RW sysfs attrs + getter, maintain/armer read the new fields |

## Test Results / Benchmarks

- i386 compile gate: 0 errors / 0 warnings on `shmem.o` +
  `shmem_compress.o` after each of step 1 and step 2.

## Next Steps

1. **Step 3 (deferred, to refine):** a read-only sysfs file with the
   whole state -- one line per mount, `<mountpoint> <mount options>`.
   Design to be refined before building (what exactly each line carries:
   the full effective option set per mount?).
2. Standing ztmpfs backlog unchanged: H618 re-measure, RFC assembly,
   mTHP folio grouping, workload bed (spec 05), BE runtime boot.

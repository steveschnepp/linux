---
name: mount-options-sysfs-file
description: add the read-only /sys/fs/tmpfs/compression/mount_options (whole effective option set, one line per mount, dev=MAJOR:MINOR, 4K truncation guard); the shmem_compress.c file split is deferred
project: linux
previous: 2026-10-18_cadence-tunables-mount-sysfs.md
---

# Mission Log: mount_options Sysfs File

Part 57 of the tmpfs transparent-compression series.  Same day as the
cadence-tunables log (previous).  A small, focused addition: the
read-only "whole state per mount" file that log deferred as step 3.

## What it is

`/sys/fs/tmpfs/compression/mount_options` (RO) reconstructs the
**effective** mount option set for every compressed mount, one line per
mount, keyed `dev=MAJOR:MINOR`:

```
dev=8:0 compress=lz4 compress_threshold=65536 compress_watermark=50:70
merge_pages=8 cell=6 gc_percent=50 age_delay_ms=1000 age_budget=128
age_rounds=4 age_batch=64 gc_debounce_ms=20
```

It complements the existing `mount_stats` (live state: mode, watermark,
counters) with **pure config** -- the two files each do one job.  Fields
are the resolved values from `shmem_comp_sb` (+ `pool->gc_debounce_ms`,
which is per-pool).  Commit `c1d356dffc4f`.

## What We Did

Added `mount_options_show()` + an `__ATTR_RO(mount_options)` to
`shmem_compress.c`, registered in the sysfs group.  It walks
`shmem_comp_mounts` under `shmem_comp_mounts_lock` (same serialization
as `mount_stats`/`gc_now`) and emits one reconstructed option line per
compressed mount.  A truncation guard breaks the walk before the
single-page sysfs buffer fills.  Also settled (before building) the two
design questions the alternatives review surfaced: the mount identifier
(`dev=MAJOR:MINOR`, not a path) and the 4K cap (a 64K buffer is not
possible in sysfs -- `show()` is handed exactly one `PAGE_SIZE`).

## Design decisions (from the alternatives review)

- **Format**: single line per mount, pure option reconstruction (Alt B),
  not a config+counters mix (Alt C) and not a per-mount subdirectory
  (Alt D).  Matches the ask `<mountpoint> <mount options>`.
- **Identifier**: `dev=MAJOR:MINOR`, same as `mount_stats` -- stable; a
  reader maps it to a path via `/proc/self/mountinfo` if needed.  The
  real mountpoint path is NOT shown (resolving it needs a namespace
  walk, which a sysfs read can't do reliably).
- **4K cap**: sysfs `show()` is handed exactly one `PAGE_SIZE` buffer by
  kernfs; it cannot be enlarged (a 64K request is not possible -- the
  buffer size is `PAGE_SIZE`, arch-dependent, 4K on the x86 target).  A
  truncation guard stops cleanly before the single page fills
  (~15-25 mounts).  For uncapped output the interface would have to
  become a seq_file (proc/debugfs), which deviates from "sysfs".
- **Scope**: only compressed mounts appear -- a mount forced off
  (`nocompress`, `compress=none`, or `watermark=100:100`) has no `comp`
  and is not on the list.

## What We Learned

- **A sysfs attribute cannot exceed one PAGE_SIZE**; anything needing
  more lines must be a seq_file (proc/debugfs) or many small files (a
  per-mount subdirectory), not a bigger buffer.
- `sb->s_dev` (MAJOR:MINOR) is the only mount identifier a sysfs read
  can produce reliably; a path needs a namespace walk.

## What We Decided

- **mount_options built as Alt B** (sysfs RO, pure config, dev=MAJOR:MINOR,
  truncation guard).
- **The `shmem_compress.c` split is deferred** (user).  It was surveyed:
  4820 lines, clean sections (core / gc / pagecache / policy / sysfs),
  but real coupling (shared pool/mount/defaults/stats statics +
  cross-section fns like `shmem_comp_cell_gc`) that would need a private
  header.  Proposed as 5 files + `shmem_compress_internal.h`; parked for
  later.

## Rules Added

- **sysfs output is capped at one PAGE_SIZE** -- design the content to
  fit, or use a seq_file; never assume a larger buffer.

## What We'd Do Differently

- Nothing material; the alternatives review up front (identifier, 4K cap,
  config-vs-state) kept the implementation small and correct.

## Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` (`c1d356dffc4f`) | +`mount_options_show` + attr (RO, one line per mount, truncation guard) |

## Test Results / Benchmarks

- i386 compile gate: 0 errors / 0 warnings on `shmem_compress.o`.

## Next Steps

1. **`shmem_compress.c` split** (deferred): 5 files
   (`shmem_compress.c` core + `_gc` + `_pagecache` + `_policy` +
   `_sysfs`) + `shmem_compress_internal.h`; gate after each file.
2. Standing ztmpfs backlog: H618 re-measure, RFC assembly, mTHP folio
   grouping, workload bed (spec 05), BE runtime boot.

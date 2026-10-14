---
name: ztmpfs-rationalization-bench
description: why-compress-tmpfs at all, answered with a bench -- the 3-arm rationalization (tmpfs+zramswap vs ztmpfs/zsmalloc vs ztmpfs/zcell), arms 1-2 measured, 44% RAM at 1.9x write cost
project: linux
previous: 2026-10-11_upstream-s1-reimplementation.md
---

# Mission Log: The Rationalization Bench

Part 23 of the tmpfs transparent-compression series.  Session 39
(2026-10-11).  The brief, in the user's words: "the real point of our
feat is zcell ... we need to rationalize otherwise we'll be rejected,
and for good reason ... the test is 3 parts: baseline tmpfs+zram.
ztmpfs/zram.  ztmpfs/zcell ... also the zcell part needs to be
improved ... let's test the first 2 parts ... also have a RAM
constrained VM, to stress the system."

Nothing is posted upstream.  All prep, all local.

## What We Did

### 1. The first-principles frame

A "ztmpfs" is always the same three-part machine:

```
  uncompressed fast tier  +  compressed bulk tier  +  movement policy
```

The existing answers differ only in where the tiers live:

- **tmpfs + zram swap** (the canonical one -- Android ships it on
  billions of devices): fast tier = page cache, bulk = zram/zsmalloc,
  policy = *reclaim-driven*.  Adaptive for free: hot data never pays
  compression; under pressure the system converges to "everything
  cold compressed".  No new code.
- **ext4/ext2 on zram**: the same machine with writeback instead of
  swap.  Niche (live images, embedded); the rig's old fsdev gate
  already showed it tracks zram's numbers wherever the device does.
- **ztmpfs (ours)**: the tiers *collapse*.  No persistent fast tier --
  the folio exists only transiently during decompress; everything is
  compressed at all times, eagerly, in the page cache itself.  A RAM
  guarantee (whole file, no swap sizing, no swappiness) bought with a
  decompress on the first read of anything.

zcell is not a fourth architecture: it is a better *bulk tier*
(O(1) bump alloc, zero per-object metadata, synchronous return to the
buddy, a floor of one cell), orthogonal to the policy above.  So the
strategy: **v1 is the wedge, zcell is the payload**, and the bench is
the instrument that prices both.

### 2. The 3-arm test

| arm | fast tier | bulk tier | policy |
|---|---|---|---|
| 1 | page cache | zram/zsmalloc | reclaim (today's answer) |
| 2 | none | zsmalloc, in page cache | eager inline |
| 3 | none | **cells**, in page cache | eager inline (pending, with the zcell improvements) |

Same dataset (munin-2.0.78, real-world compressible, CRC-verified),
same kernel (the feature branch -- it speaks every arm's option
surface), same boots, same aggregator.  Compression held constant
(lz4, 4K blocks) so the comparison isolates *architecture*, not
knobs.  Arm 3 plugs into the same harness when the cell work lands;
its known improvement targets (from the series logs) are the
cell_order axis, the fixed-size directory wall at small orders, and
the GC/synchronous-free behavior.

### 3. The bench (`test_zbench.c`, rig `61b7fd6`)

Per rep: untar (timed write) -> CRC verify (timed read) -> rm -Rf
(timed free), MemFree/Shmem sampled at every boundary, live gauges
(comp_stats / zram mm_stat) captured while the dataset is resident.
N copies of the tree scale the live set; an optional pressure phase
fills incompressible anon memory (xorshift, never zeros -- zram
would store those for free) down to a target, reporting filler and
swap activity.  Two VM sizes: 1G (no-pressure regime) and 256M
(constrained).  Results go to a file, printed after dmesg.

### 4. The measurement kernel

The rig's KASAN config corrupts RAM measurement: freed slabs sit in
quarantine, so "MemFree returns to baseline" climbs a staircase and
never lands (observed as a growing -7 MB -> -30 MB deficit across
reps before the cause was identified).  Built a **non-KASAN
measurement kernel** (`bzImage #125`, KASAN off, SLUB_TINY on) by
toggling the shared config; the KASAN config stays for the correctness
gates.  RAM benches run non-KASAN from now on.

### 5. First-look results (arms 1-2)

Non-KASAN kernel, munin x6 (29.3 MB apparent, 35.75 MB of pages),
5 in-boot reps, M=1 boot:

**1G, no pressure:**

| metric | arm 1 tmpfs+zramswap | arm 2 ztmpfs/zram |
|---|---|---|
| live-set RAM | **38.4 MB** (Shmem delta; swapfree full, zram ~0) | **21.6 MB** (18.2 pool + 3.4 shmem) -- **-44%** |
| compressed? | never (its compression is hypothetical) | 8,730 pages, 16.9 MB stored, 2.1:1 |
| untar | 0.091 s | 0.177 s (1.9x) |
| verify | 0.088 s | 0.114 s (1.3x) |
| rm | 0.022 s | 0.022 s |

**256M, squeezed to 2 MB free:**

| metric | arm 1 | arm 2 |
|---|---|---|
| outcome | **OOM-killed** | **survives** |
| filler absorbed | -- | 133-139 MB vs arm 1's 117-121 MB (+15 MB anon headroom) |

The rationalization, in one sentence each: *arm 1's compression never
happens without pressure, and under pressure the box dies before it
converges; arm 2's is unconditional, costs 1.9x write / 1.3x
first-read, and buys survival.*  Both halves go in the cover letter.

## What We Learned

### Technical

- **The comp_stats figures are gauges, not counters** (the trap that
  hid the s1 smoke test's "failure" and then arm 2's "no
  compression"): reads decompress and rm frees, so any snapshot after
  the dataset is gone reads zero.  Live gauges must be sampled while
  the data is resident.  Twice burned, one lesson.

- **KASAN quarantine breaks RAM accounting.**  Dentry/inode slabs
  freed by rm sit in quarantine, so MemFree never returns to
  baseline and the deficit grows per rep.  Correctness runs keep
  KASAN; measurement runs get a non-KASAN kernel.  The toggle is
  cheap (one scripts/config pass + rebuild) and the config is shared,
  so the two kernels are the same tree.

- **`swapon` needs a swap signature** -- `mkswap` first, always, even
  on a fresh zram device.  EINVAL with no other symptom.

- **Busybox serial drops long lines under printk noise.**  In-boot
  rep lines vanished (1 of 5 survived in one run).  The fix: the test
  writes to a file; init prints dmesg first, then cats the file.  The
  rig's historical BENCH lines had the same exposure.

- **zram stores what you give it**: the pressure filler uses
  xorshift bytes, not zeros -- zeros would compress to nothing and
  understate arm 1's swap cost.

- **The tmpfs+zramswap baseline had never been measured in 39
  sessions.**  Every prior number was vs raw zram or vs uncompressed
  tmpfs; the canonical competitor was absent from the table until
  now.

### Process

- **Six bench bugs, six one-line lessons**, each costing a QEMU
  cycle: swapon-without-mkswap; walk paths double-prefixed
  (`munin-2.0.78/munin-2.0.78/file`); empty-rel leading slash;
  copy dirs created *before* arm_setup mounts over them; rm_rf
  removing the copy dirs themselves (recreate after); sysfs_long
  parsing key-style files as zram columns (`atoi("compressed_pages")
  == 0` -> a constant zero that looked exactly like "compression did
  not run").  Every one was invisible in code review and loud in one
  boot.

- **The first-look table needed the gauges to be believed.**  MemFree
  deltas drifted +-10 MB common-mode across reps in both arms; the
  Shmem delta and comp_stats ram_total were stable and exact.  When a
  signal is smaller than the drift, find the gauge that measures the
  quantity directly -- do not average noise into a number.

- **A stress test that stops at its target never stresses.**  The
  first pressure phase targeted 32 MB free and forced no swap at all
  (swapused=0 everywhere).  Only dropping the target to the knee made
  the OOM-vs-survive divergence appear.

## What We Decided

- **The rationalization story is now measured, not rhetorical**: arm
  1's compression is hypothetical without pressure; arm 2's is
  unconditional at a stated CPU cost.  The RFC cover letter's first
  table is this bench.
- **Arm 3 (ztmpfs/zcell) lands through this same harness**, and its
  run is the zcell improvement driver: cell_order sweep, directory
  sizing at small orders, GC efficiency, the synchronous-free rm
  story.  No blind zcell changes -- the session-33 rule.
- **Measurement kernels are non-KASAN**; correctness gates stay
  KASAN.  Recorded so the choice stops being re-derived.
- **The patch-series split** (from this session's earlier review of
  the s1 branch): prep -> encoding -> dormant core -> decompress
  (dead code) -> compress (live) -> truncate release -> options ->
  mf handling -> stats -> selftests -> docs, with fixes folded into
  the patch that caused them and no fixup blobs after v1.

## Rules Added

- **Stats snapshots only while the data is resident.**  Gauge-style
  readings (live counts) return to zero after free; sample at the
  phase boundary that answers the question.

- **RAM measurement runs on a non-KASAN kernel.**  Quarantine
  corrupts freelist-return accounting; correctness runs keep KASAN.

- **Guest output goes through a file when the serial is noisy.**
  dmesg first, then one clean cat; never interleave benchmark lines
  with kernel printks on the console.

- **A stress phase must reach the knee, not a comfortable target.**
  Verify the stress actually fired (swap activity, OOM, or filler
  hitting the mmap wall) or it measured nothing.

## What We'd Do Differently

- Would have sampled the exact gauges (Shmem delta, comp_stats
  ram_total) from the first rep instead of discovering them after two
  rounds of noisy MemFree deltas.

- Would have checked the tmpfs+zramswap baseline's existence BEFORE
  39 sessions of "vs zram" claims -- the most obvious reviewer
  question was unmeasured until now.

- The bench's arm-1 OOM boundary needs a sweep (zpress just above /
  at / below the knee) to become a survival curve rather than a
  single anecdote; that is the next bench iteration.

## Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_zbench.c` | **new** -- the 3-arm rationalization bench (arms 1-2 live, arm 3 slot ready) |
| rig `guest/init.sh` | `test.zbench=1\|2` boot mode; file-backed results after dmesg |
| rig `scripts/run_qemu.sh` | `TEST_ZBENCH`, `TEST_ZREPS`, `TEST_ZPRESS`, `TEST_ZCOPIES` passthroughs |
| kernel `.config` toggles | non-KASAN measurement kernel (`bzImage #125`) built alongside the KASAN gate config |

## Test Results / Benchmarks

All four matrix points green (fails=0, 5 reps, CRC-verified 4,284
files / 29.3 MB per rep):

| point | rc | headline |
|---|---|---|
| arm1 1G | 0 | 38.4 MB live set, never swaps |
| arm2 1G | 0 | 21.6 MB live set (-44%), 2.1:1, 1.9x write / 1.3x read |
| arm1 256M press | killed | OOM at 2 MB free target |
| arm2 256M press | 0 | survives, +15 MB anon headroom |

## Next Steps

1. **M-boot wrapper** for medians + IQR across boots (the in-boot
   reps give spread; the RFC table needs boot medians).
2. **Arm-1 survival curve**: zpress sweep around the knee so the
   stress row reads "survives down to X MB free" per arm, not one
   OOM anecdote.
3. **Arm 3 (ztmpfs/zcell)** through the same harness, with the zcell
   improvement targets named above -- the payload of the whole
   series.
4. The s1-branch items still standing: memory-failure answer,
   in-tree selftests (the zbench methodology ports), Sphinx-verified
   docs, and the RFC assembly (local only).

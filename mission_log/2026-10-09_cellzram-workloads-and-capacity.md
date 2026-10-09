---
name: cellzram-workloads-and-capacity
description: cellzram under real workloads -- zram head-to-head, the zero-page optimization, the 256-cell capacity bug, and the capacity fix with u32 widening (sessions 21-24)
project: linux
---

# Mission Log: cellzram under Load -- Zero Pages, the 256-Cell Cap, and the Capacity Fix

Part 8 (current end) of the tmpfs transparent-compression series, split
verbatim from the combined log `2026-10-05_tmpfs-transparent-compression.md`
(kept until the split is validated). Session numbers are preserved so
cross-references keep resolving.

Covers sessions 21-24 (2026-10-09): the fs-workload and random-IO gates
against zram head-to-head, the zero-page optimization, the randio gate
exposing the hardcoded 256-cell directory cap, and the capacity fix
(capacity-derived nr_cells + headroom, u32 cellid widening) that closed the
bug with all gates green.

- Previous: `2026-10-07_deferred-compression-policy.md`

## Session 21: cellzram for block devices -- fs workloads, zram head-to-head, and the zero-page finding (2026-10-09)

### What We Did

Pivoted from the tmpfs landing sequence (complete as of session 20) to
the cellzram block-device track: the spec's motivation promised the
cell allocator's properties for "workloads that require a `struct
block_device`: swap, ext4/xfs, O_DIRECT databases, journal devices,"
but sessions 1-20 only ever exercised cellzram as a *synthetic* device
(dd round-trips, discard drain, reset, one swap trial). This session
built the missing workload layer -- filesystems and random I/O,
head-to-head against zram -- and found the one thing that actually
matters for fs-on-cellzram.

1. **Kernel config.** Added `CONFIG_EXT4_FS` + `CONFIG_EXT4_USE_FOR_EXT2`
   (busybox mke2fs makes an ext2 image; the ext4 driver mounts it) and
   `CONFIG_ZRAM` to the rig build script. First zram build forced
   LZO-only (`ZRAM_BACKEND_FORCE_LZO` is `def_bool` when no backend is
   selected) with default `lzo-rle` -- an unfair head-to-head against
   cellzram's lz4 default. Fixed: enabled `ZRAM_BACKEND_LZ4` +
   `ZRAM_BACKEND_ZSTD`, set `ZRAM_DEF_COMP_LZ4`. bzImage 4,035,584 ->
   4,039,680 B.

2. **`tests/test_blkdev_fs.c`** -- the fs farm. Per device (cellzram0,
   zram0) x algo (lz4, plus cellzram `none` as the uncompressed-cells
   baseline): set disksize 256M -> mke2fs -> `mount -t ext4 -o discard`
   -> untar the munin tree 8x (timed) -> CRC32-verify every file
   against the host manifest (timed) -> rm -Rf (timed) -> fstrim ->
   umount -> reset. Reuses test_torture.c's CRC32/manifest code. BENCH
   lines carry untar/verify/rm MB/s + MemFree snapshots + device RAM.
   Hard asserts limited to correctness (CRC, untar, teardown recovery);
   the RAM/throughput columns are reported, not asserted (the
   quiescent-verify rule -- no mutators here, but the gauges are still
   racy against slab/page-cache noise).

3. **`tests/test_randio.c`** -- the database-shaped workload. 4K random
   write/read, buffered + O_DIRECT, both raw on the device (O_DIRECT
   only -- session 5's rule: raw block I/O that isn't O_DIRECT tests
   the page cache, not the device) and on an ext4 file. Per-block
   xorshift pattern keyed by (block, round) with a round tracker, so
   every read block is verified against its generating content.

4. **Rig gates.** `test.fsdev=1` -> `test_blkdev_fs`, `test.randio=1` ->
   `test_randio` (init.sh isolation blocks + run_qemu.sh `TEST_FSDEV` /
   `TEST_RANDIO` passthrough). Both are ONE-boot-per-run isolation
   gates like cellzram/entryrace/watermark.

5. **The mm_stat unit misparse.** First run's `dev_ram_kb` column read
   nonsense: cellzram `none` = 8799144 ("8.4 GB" on a 256 MB disk),
   zram = 81920 ("80 KB" for 124 MB live). Root cause: BOTH drivers
   emit **bytes** (`zram: mem_used << PAGE_SHIFT`), and the column was
   labeled KB and compared against KB fields. Fixed: `dev_ram_mb` (bytes
   / MIB) + a raw-mm_stat BENCH line so the parse is *verifiable, not
   trusted*.

6. **The zero-page finding (the session's real result).** With the
   parse fixed, the fs-phase numbers were:

   | device | orig | compr | phys/mem_used | same_pages | entries |
   |--------|------|-------|---------------|------------|---------|
   | cellzram lz4 | 8441856 | 54118 | 262144 (64 cells) | - | 2061 |
   | zram lz4 | 8441856 | 867 | 81920 (20 pages) | **2048** | - |
   | cellzram none | 8441856 | 8441856 | 8650752 | - | 2061 |

   Same orig (apples-to-apples), throughput dead even (untar 20.3 vs
   20.4, verify 77.2 vs 78.0, rm 58.5 vs 59.1 MB/s), but zram held
   20 pages vs cellzram's 64 cells + 2061 entries. The tell:
   `same_pages=2048` of 2061 written pages. I initially narrated this
   as "zram deduplicated the 8 identical copies" -- **wrong**, and the
   user caught it. `page_same_filled()` (zram_drv.c:333) compares every
   word to `page[0]`: it catches *uniform-fill* pages (all zeros, all
   0xFF), NOT identical-content pages. The 2048 are zero pages from
   ext4 metadata (unused inode tables, bitmaps, padding), which
   filesystems are full of. zram stores each as an 8-byte element;
   cellzram has no such fast path and pays a lz4-compressed cell +
   ~72 B entry for every one. That is the entire RAM gap -- a missing
   optimization, not a worse allocator.

7. **The fix design (settled, not yet implemented at time of writing).**
   For a block device, "written as all-zeros" is indistinguishable from
   "never written" -- both read back as zeros via the read-miss
   zero-fill (brd semantics). So an all-zero page needs NO storage.
   ~20 lines in `cellzram_write_block()`, before the compress+store+alloc:

   ```c
   if (page_all_zero(src)) {          /* word-at-a-time scan */
       void *old = xa_erase(&dev->table, block_nr);
       if (old)
           cellzram_put_entry(dev, xa_to_value(old));
       atomic64_add(PAGE_SIZE, &dev->orig_bytes);
       atomic64_inc(&dev->zero_pages);
       return 0;
   }
   ```

   The `xa_erase`+`put` is byte-identical to `cellzram_bio_discard`
   (cellzram.c:374) and `slot_free_notify` -- three paths that already
   converge on "no slot, cell returned inline"; the zero-write is a
   fourth, the only one that needs no fs cooperation. Net CPU *win*
   too (a 512-word scan replaces lz4 + store + entry_alloc). No new
   entry type, no read-path change, no mount option. One new counter
   `zero_pages` in mm_stat (mirrors zram's `same_pages` so the
   head-to-head stays legible).

8. **Discard support confirmed** (the user's question): cellzram sets
   both `.max_hw_discard_sectors` AND `.max_write_zeroes_sectors` +
   `.max_hw_wzeroes_unmap_sectors` = UINT_MAX (cellzram.c:656-666).
   The WRITE_ZEROES trio is required because `fallocate(PUNCH_HOLE)`
   on a block device reaches the driver as WRITE_ZEROES via
   `BLKDEV_ZERO_NOFALLBACK`, which demands hw write-zeroes offload --
   discard limits alone get -EOPNOTSUPP. Both route to
   `cellzram_bio_discard`. The fs-farm run corroborated it:
   `trim_extra_kb=0` everywhere because ext4's `-o discard` had already
   freed everything during rm, leaving fstrim nothing to trim.

### What We Learned

#### Technical

- **zram's same-filled fast path is NOT dedup.** `page_same_filled()`
  (zram_drv.c:333) fires only when every word in the page equals the
  same value -- all-zeros, all-0xFF. Two identical pages of real text
  do NOT collapse. A counter named `same_pages` invited the dedup
  story; the function refuted it. Read the function, not the field
  name.

- **Filesystem metadata is zero-heavy; file data is not.** ext4 on a
  256 MB device wrote 2048 zero pages out of 2061 for the munin untar
  -- unused inode tables, bitmaps, padding. This is the whole reason
  the block-device zero-page optimization is valuable, AND the reason
  the tmpfs version is worth much less (tmpfs's workloads are file
  *data*, which is almost never all-zeros). The hit rate is
  workload-shaped, and the two surfaces have different workloads.

- **The zero-page trick is free only on a surface whose hole reads
  back as zeros.** cellzram: read-miss zero-fill (brd). tmpfs:
  `SGP_READ` zero-fills holes (shmem.c:2576). Both qualify. A page of
  all-0xFF does NOT -- a hole returns zeros, not 0xFF -- so catching
  non-zero uniform fills needs a real entry encoding (sentinel length +
  fill value in the handle), which is the expensive version and not
  worth it: lz4 already compresses a uniform page to ~15 B.

- **For tmpfs the same idea is strictly more work and less payoff.**
  The block device skips the `xa_store` (free); tmpfs must
  *un-materialize* an already-allocated folio (delete from page cache
  -> hole) under the compress path's guard set (clean, order-0,
  unmapped, unpinned), and it only gains on ranges *written* with
  zeros (unwritten ranges are already free holes). Plus an observable
  behavior change the block device doesn't have: `st_blocks` /
  SEEK_HOLE would report written-zeros as a hole.

- **zram defaults to LZO unless a backend is explicitly enabled.**
  `ZRAM_BACKEND_FORCE_LZO` is `def_bool !LZ4 && !LZ4HC && !ZSTD &&
  !DEFLATE && !842` -- a bare `--enable CONFIG_ZRAM` yields lzo-rle and
  an unfair comparison against any lz4-default device.

- **mm_stat units are bytes on BOTH cellzram and zram.** A column
  labeled KB holding bytes produces confidently-wrong numbers ("8.4 GB
  on a 256 MB disk"). Emit the raw sysfs line next to the parsed value
  so the transform is auditable.

#### Process

- **The parallel-edit trap did NOT fire this session** -- every
  edit -> build -> run chain was sequential, and binary provenance was
  verified with `strings` BEFORE each 300 s boot (the session-20 rule).
  Caught the mm_stat misparse in one run instead of debugging a stale
  binary. Keep doing this.

- **An atomic edit batch reverted on one failed hunk** (session 4's
  lesson, reapplied): a `struct dev` oldText with the wrong comment
  text failed edits[1] and reverted edits[0] silently. Check exact
  bytes (the comment block) before batching.

- **Copy-paste across test files brings the wrong constants.**
  `run_distinct_ram` referenced `BLK`/`N_BLOCKS` from test_randio.c
  into test_blkdev_fs.c (which uses `PAGE`, `N_COPIES`) -- a clean
  gcc failure, but only because I compile-check every test standalone
  (`gcc -Wall -Wextra -static`) before packing the rootfs.

- **A BENCH line interleaved with a kernel printk loses its tail on
  the serial console** (session 12's two-writers rule). The cellzram
  `none` config's MemFree columns were eaten by
  `shmem_comp: destroyed pool...`. Machine-readable results that must
  survive interleaving want to be short, or parsed by prefix.

### What We Decided

- **cellzram gets the zero-page optimization; tmpfs does not (yet).**
  cellzram: proven hit rate (2048/2061 on fs metadata), ~20 lines, free
  (no new entry type, no read change, no option), net CPU win. tmpfs:
  defer behind a counting probe in the aging walk that measures the
  zero-page rate on the actual torture workloads first (session 17's
  rule: don't build on a guessed number).

- **Only the full-zeros case, not zram's general uniform-fill.** The
  free trick needs hole-reads-as-zeros, which only zeros satisfy.
  Non-zero uniform fills are rare and already near-optimal under lz4.

- **The optimization is unconditional** (no mount/sysfs option),
  matching session 8's "no extra grates" rule -- it's always correct
  and always cheaper, so a knob would only be a new ABI surface.

- **A distinct-data RAM phase was added to `test_blkdev_fs.c`** (writes
  N_BLOCKS distinct 4K blocks raw, no two alike, so uniform-fill has
  nothing to catch) to give the pure cell-vs-zsmalloc allocator
  comparison, separate from the fs-metadata workload where
  same-filled dominates. NOTE: this phase has NOT yet produced output
  -- run3 was aborted before reaching it. Pending.

### Rules Added

- **Read the function, not the field name.** A counter's name
  (`same_pages`) suggests a mechanism; only the implementing function
  confirms it. Before narrating what a metric means, read its producer.

- **Sysfs-stat transforms must be auditable in the output.** When a
  test parses a kernel stat line into a BENCH column, emit the raw line
  alongside the parsed value (or a checksum of it) so a unit/parse bug
  is visible in the log, not silently wrong.

- **Cross-test copy-paste gets a standalone compile-check.** Any test
  helper moved between rig test files is compiled with `-Wall -Wextra
  -static` on its own before it goes into the rootfs -- undefined
  constants and wrong-scope refs fail there, not in the guest.

### What We'd Do Differently

- Would have dumped the raw mm_stat line in the FIRST version of the
  fs test. The bytes-vs-KB misparse produced three confidently-wrong
  BENCH columns before the raw line made it obvious in one glance.

- Would have read `page_same_filled()` before saying "dedup" out loud.
  The user caught the misattribution; the function is four lines and
  would have corrected me in thirty seconds.

- Would have set `QEMU_MEMORY` / timeouts with headroom for the
  distinct-data phase (64 MB of O_DIRECT writes x 4 configs) instead of
  letting run3 abort at 300 s before it executed.

- The fs-farm MemFree-after-rm column is noisy against slab/page-cache
  noise and one run went NEGATIVE (cellzram lz4, `delete_return_kb=
  -7852` in run3 vs `+7160` in run2). If the "does delete return RAM
  inline" question needs a hard number, sample `Shmem`/device mm_stat
  rather than raw MemFree, or widen the slack and report the breakdown
  (session 6's post-storm lesson).

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/scripts/build_kernel.sh` | +EXT4_FS, +EXT4_USE_FOR_EXT2, +ZRAM +LZ4/ZSTD backends, ZRAM_DEF_COMP_LZ4 |
| `~/tmpfs-compress-test/tests/test_blkdev_fs.c` | new: ext4+discard untar farm on cellzram0 vs zram0; CRC manifest verify; raw-mm_stat BENCH; distinct-data RAM phase (pending) |
| `~/tmpfs-compress-test/tests/test_randio.c` | new: 4K random IO, buffered + O_DIRECT, raw device + ext4 file, cellzram vs zram |
| `~/tmpfs-compress-test/guest/init.sh` | test.fsdev=1 / test.randio=1 isolation gates |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_FSDEV / TEST_RANDIO passthrough |
| `/home/snide/linux/mm/cellzram.c` | (next) zero-page optimization: page_all_zero() + write_block branch + zero_pages counter |

### Test Results

**fsdev gate** (`QEMU_MEMORY=2G TEST_FSDEV=1`), kernel with EXT4+ZRAM:

- All 3 fs configs: **CRC 5712 files verified, 0 mismatch** (8 x 714),
  all untars succeeded, teardown MemFree recovered near baseline,
  **exit 0**. ext4 mounted `-o discard` on both devices.
- Throughput dead even, cellzram vs zram (lz4): untar 20.3 / 20.4,
  verify 77.2 / 78.0, rm 58.5 / 59.1 MB/s.
- RAM (fs phase, lz4): cellzram phys 262144 + entry 148392 = ~0.4 MB
  vs zram mem_used 81920 = ~0.08 MB -- the gap explained entirely by
  zram's `same_pages=2048` uniform-fill fast path on ext4's zero pages.
- cellzram `none` (uncompressed cells): verify 71.7 MB/s, RAM 8650752
  B = 8.4 MB (stores raw, as expected -- the compression-RAM-benefit
  baseline).
- `trim_extra_kb=0` on every config: fstrim found nothing because
  ext4's discard-on-free already released it during rm (discard works).
- Caveat recorded: one cellzram lz4 `delete_return_kb` was negative
  (-7852 in run3) -- the MemFree-delta column is noise-dominated.

- **`tests/test_randio.c` has been written and compile-checked but NOT
  yet run.** Pending.
- **The distinct-data RAM phase has NOT produced output** (run3 aborted
  before reaching it). Pending.
- **The zero-page optimization is designed, not implemented.** Pending.

### Implementation: the zero-page optimization (2026-10-09, same day)

Landed the design from step 7 above. Three edits to `mm/cellzram.c`,
one commit:

1. `cellzram_page_all_zero()` -- word-at-a-time scan (512 compares on
   x86_64), placed before the write path.
2. `struct cellzram` gains `atomic64_t zero_blocks`, zeroed on reset.
3. Early branch at the top of `cellzram_write_block()`, before the
   compress+store+alloc: if the source page is all-zero, `xa_erase` the
   slot, `cellzram_put_entry()` the old id (reusing the discard path's
   exact protocol), `orig_bytes += PAGE_SIZE`, `zero_blocks++`, return.
   No cell, no entry, no crypto.  Applies in raw (`none`) mode too.
4. `mm_stat` emits a 7th field `zero_blocks` (mirrors zram's
   `same_pages` so the head-to-head stays legible); appended at the end
   so existing field positions are stable for parsers.

**Result (fsdev gate, all asserts green, exit 0):**

| metric (cellzram lz4, fs-metadata workload) | before | after |
|---|--:|--:|
| objects | 2061 | **13** |
| entry_bytes | 148392 | **936** |
| compr_bytes | 54118 | **870** |
| zero_blocks | - | **2048** |

The 2048 zero pages are now free storage.  Cross-validation: cellzram's
`zero_blocks=2048` matches zram's `same_pages=2048` exactly, and 13
surviving objects = 2061-2048 -- two independent implementations agree
on precisely which pages are uniform-fill.  Accounting is consistent
(orig_bytes unchanged at 8441856; only the stored representation
collapsed).

**But the headline RAM column did NOT flip, and the reason is the real
finding.**  cellzram's `phys` is `262144` = exactly ONE 256 KiB cell
(the `cell_order=6` default).  The 13 surviving real entries pin one
cell, and that cell's allocation granularity now *dominates* the
footprint: dev_ram 0.25 MB vs zram's 0.08 MB.  The optimization
removed the dominant cost (2048 zero pages) and thereby *revealed the
next bottleneck* -- cell-size granularity at tiny live sets.  This is
a cell-order-vs-workload artifact, not a zero-page defect: with
hundreds of real blocks the cell amortizes; with 13 it is 99% empty
slack.  Options if the small-live-set case ever matters: a smaller
`cell_order` module param for the device pools, or a tier-1 return of
under-filled cells.  Not pursued -- the fs-metadata win the
optimization was built for (2048 blocks) is real and landed; the
residual is a different (pre-existing) tuning axis.

**The distinct-data phase ran for the first time** (it was aborted in
run3).  On 64 MiB of incompressible random blocks, no two alike:

| device | algo | dev_ram | ram/orig |
|---|---|--:|--:|
| cellzram | lz4 | 65.1 MB | 101.8% |
| zram | lz4 | 64.0 MB | 100.0% |
| cellzram | zstd | 65.1 MB | 101.8% |
| zram | zstd | 64.0 MB | 100.0% |

This is the pure allocator comparison (no uniform-fill pages to catch):
the **+1.8% is cellzram's per-block entry table** (16384 x 72 B =
1.18 MB), the spec's known metadata risk now quantified for the first
time.  zram stores incompressible pages as `huge_pages` (its last two
mm_stat fields both read 16384) with no comparable per-page table tax
beyond the page itself.  On incompressible data neither device saves
RAM (expected -- there is nothing to compress); cellzram pays a small
table premium over zram.  At 4 KiB blocks that is ~16 MiB per GiB
written, matching the spec's risk row.

### Next Steps

1. ~~Implement the cellzram zero-page optimization~~ DONE (see above);
   landed, gated green, `zero_blocks` in mm_stat.  Follow-on IF the
   small-live-set case ever matters: cell_order tuning or tier-1
   return of under-filled cells (the residual 0.25 MB floor is one
   pinned 256 KiB cell, not a zero-page cost).
2. **Run the randio gate** (`TEST_RANDIO=1`) -- written, never run.
3. tmpfs zero-page probe: add a counter in the aging walk that
   counts all-zero folios across the torture workloads; let the
   measured rate decide whether the tmpfs un-materialize version is
   worth its complexity.
4. cellzram phase-3 hardening (spec risk table): bounded ENOMEM retry,
   concurrent KASAN soak, swap slot-free -> discard path.
5. Entry-table metadata (+1.8% on incompressible data, measured
   above) is the spec's batched-entries v2 lever; still gated on its
   design pass.

---

## Session 22: the randio gate finds a real cellzram bug -- the 256-cell directory cap (2026-10-09)

### What We Did

Ran the new `test_randio` gate (session 21, written but never run) and
it failed on cellzram while zram passed the identical workload.  Spent
the session driving the failure to a confirmed root cause, using the
A/B + trace discipline accumulated over the project.  The bug is
pre-existing (NOT the zero-page commit) and is a genuine capacity bug
in the cell allocator.

1. **The failure.**  `TEST_RANDIO=1`: cellzram raw O_DIRECT phase
   passes clean (16384 written, bad=0, rderr=0), then the ext4 file
   phase dies with `BLK_STS_IOERR` on writes (blk status 10), ext4
   "I/O error while writing superblock", ~2500 bad blocks on readback.
   zram: 0 bad, both phases.  Exit 1.

2. **A/B against the pre-change kernel** (session 9's gate rule, applied
   before touching my own diff): built `be3f32e6acc4` (pre-zero-page)
   in a throwaway `git worktree` + ccache, ran the same gate.  **Same
   failure, same shape** -- cellzram fs phase fails (2503 bad, 64 I/O
   errors), zram passes.  Verdict: pre-existing, not a regression from
   the zero-page optimization.  (Which makes sense: that change only
   *reduces* cell demand and never touches cell allocation.)

3. **Two wrong theories, killed by measurement.**
   - *Order-6 fragmentation*: hypothesized the `cell_order=6` (256 KiB)
     contiguous `alloc_pages` fails under fs-workload memory churn
     where zram's page-granular zsmalloc does not.  Tested by adding a
     `TEST_CELLORDER=2` cmdline passthrough (`cellzram.cell_order=`).
     **Refuted**: IOERR count identical (64) at order-2 and order-6.
     (Also noted: at order-2 the *raw* phase degraded to written=1024
     with a `clocksource ... read timed out`, so order-2 has its own
     problem -- not a clean comparison.)
   - Stopped guessing after two misses; switched to instrumentation.

4. **The trace named it** (session-2 methodology: pr_err on each
   failure path -> one build cycle -> the culprit).  Added a distinct
   `pr_err` to all three ENOMEM paths in `cellzram_write_block` (bounce
   kmalloc / shmem_comp_store / entry_alloc).  Result: **every failure
   is `FAIL=comp_store len=4096`** -- `shmem_comp_store` returning 0 for
   a *full 4096-byte* block (the randio test writes xorshift-random
   data, so lz4 cannot shrink it -> `-ENOSPC` -> stored raw at exactly
   PAGE_SIZE).  Instrumentation removed after; tree back to committed
   state (empty diff verified).

5. **Root cause: the 256-cell directory cap.**  Following the chain:
   `shmem_comp_store` -> `cell_store` (pool->use_cells) ->
   `cell_make_active` returning NULL.  `cell_make_active`
   (cell_store.c:133) returns NULL when either the `alloc_pages` fails
   OR **no free directory slot exists** (`for slot < max_cells`).  And
   `max_cells` is **hardcoded to 256** in `cell_pool_create`
   (cell_store.c:53) -- it is NOT sized from `disksize` or from the
   amount of data the device is asked to hold.

   The arithmetic seals it: capacity = `max_cells` x cell_size =
   256 x (1<<cell_order) x 4 KiB.
   - order-6: 256 x 256 KiB = **64 MiB**; the fs phase writes 16384 x
     4 KiB = 64 MiB of incompressible data -> exactly fills the
     directory -> the next `cell_make_active` finds no slot -> NULL ->
     ENOMEM -> IOERR.
   - order-2 (the "refutation" run, re-read): the raw phase stopped at
     **written=1024 = 4 MiB = 256 x 16 KiB** -- again exactly 256
     cells.  Both orders independently hit the same 256-cell wall.
     That cross-order identity is what proves the directory cap (not
     fragmentation, which would not scale with cell size this way).

   Root cause CONFIRMED: **cellzram silently caps physical storage at
   `max_cells(256) x cell_size` regardless of `disksize`; a workload
   storing more incompressible data than that gets BLK_STS_IOERR on
   writes.**  The device advertised disksize=256M but can only hold 64
   MiB at the default cell_order=6.

6. **Why the other gates never caught it.**  The fs-farm gate (session
   21) untars the munin tree -- highly compressible, so the *stored*
   footprint is tiny and never approaches 64 MiB.  test_cellzram
   (session 5) round-trips a few MiB.  Only the randio gate's
   64 MiB of *incompressible* data reaches the cap.  This is exactly
   the "import an independent oracle" lesson: a new workload class
   (random/incompressible at scale) found a bug every prior workload
   missed.

### What We Learned

#### Technical

- **`max_cells` is a fixed 256, not derived from disksize.**  The cell
  pool directory is a statically-allocated array of 256 slots
  (`cell_pool_create`), so the *physical* capacity of any cell-backed
  store is `256 x cell_size`, independent of the logical `disksize`.
  For tmpfs this never surfaced (mounts hold compressible page cache,
  and the shrinker/GC + size= accounting bound it); for a block device
  that promises 256 MiB and must store incompressible sectors, it is a
  hard, silent wall.

- **The failure is deterministic, not pressure.**  The tell I nearly
  missed: the IOERR count was EXACTLY 64 every run, across cell_order
  values.  Random ENOMEM under memory pressure varies run to run; an
  exact reproducible count is the signature of a *capacity* limit.  If
  a "memory" failure is suspiciously deterministic, suspect a fixed
  array/counter, not the allocator.

- **Cross-order identity localizes a cap.**  That order-2 stopped at
  exactly 256 x 16 KiB and order-6 at exactly 256 x 256 KiB is what
  turned "it fails" into "the directory is 256 slots."  When a limit
  changes proportionally with a sizing parameter, the parameter *is*
  the limit.

- **Incompressible-at-scale is a distinct workload class.**  Compressed
  storage bugs live in the ratio; capacity bugs live in the
  incompressible tail (lz4 `-ENOSPC` -> raw store at full PAGE_SIZE).
  A test suite that only feeds compressible data cannot find the
  second kind.

- **`-ENOSPC` -> raw-at-PAGE_SIZE is the path that hits the cap.**
  Every `FAIL=comp_store len=4096` was a block lz4 could not shrink.
  Compressed blocks (len < 4096) amortize the directory far better;
  the raw path is what races to 256 cells.

#### Process

- **Gate A/B before blaming the diff -- again paid off.**  Without the
  `be3f32e6acc4` baseline I would have hunted my own zero-page commit
  for a bug that predates it by a day.  The worktree+ccache baseline
  cost ~3 minutes.

- **Two wrong theories then instrumentation.**  I theorized dedup
  (session 21, wrong), then order-6 fragmentation (wrong), before
  reverting to the pr_err trace that named the function in ONE build
  cycle.  The lesson from session 2 applies to myself: stop theorizing
  after a miss or two and instrument.

- **A negative result ("lowering cell_order did not fix it") still
  carries the answer if you re-read it.**  I initially read the order-2
  run as a pure refutation; only on review did written=1024 = 4 MiB =
  256 cells jump out as *positive* evidence for the directory cap.
  Re-read failed experiments for the shape of the truth, not just
  pass/fail.

### What We Decided

- **This is a REAL pre-existing cellzram bug, confirmed, to fix.**  Not
  a test bug, not memory pressure, not the zero-page commit.  The fix
  direction (NOT yet implemented): size `max_cells` from the device's
  required capacity (`disksize / cell_size`, with headroom), or make
  the directory grow dynamically.  The cleanest is to pass a cell
  count hint into `cell_pool_create` from cellzram's disksize-set path
  (tmpfs pools keep a bounded default).

- **The randio gate stays red until fixed** -- it is correctly reporting
  a capacity bug, so "fix the test to pass" would be wrong.  Recorded
  as a known-open failure with a confirmed root cause.

### Rules Added

- **Deterministic "memory" failures are capacity bugs.**  An exact,
  reproducible failure count under a nominal OOM points at a fixed
  array/counter/limit, not the page allocator.  Check sizing
  parameters before theorizing about pressure.

- **Cross-parameter identity localizes the cap.**  If the failure point
  scales exactly proportionally with a sizing knob (cell order, block
  size), that knob multiplied by a constant IS the limit.

- **Incompressible-at-scale is a required test class** for any
  compressed store.  Feed random data at the device's advertised
  capacity, not just compressible datasets, or capacity walls stay
  invisible.

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | +TEST_CELLORDER= -> cellzram.cell_order= cmdline passthrough (the A/B instrument) |
| `/home/snide/linux/mm/cellzram.c` | (instrumented then reverted; empty diff) DBGCELL pr_errs named comp_store, then removed |
| `/home/snide/linux/mm/cell_store.c` | (read-only) root cause: max_cells=256 hardcoded at cell_pool_create |

### Test Results

- **randio gate, cellzram: FAIL (real bug).**  Raw phase 16384 written,
  bad=0.  fs phase: ~2500 bad blocks, 64 I/O errors, BLK_STS_IOERR on
  writes, ext4 superblock write error.  zram identical workload: PASS,
  0 bad, both phases.
- **A/B baseline (be3f32e6acc4, pre-zero-page): identical failure** --
  cellzram fs phase 2503 bad / 64 errors, zram clean.  Confirms
  pre-existing.
- **DBGCELL trace: 100% `FAIL=comp_store len=4096`** across all
  failures; zero cbuf_kmalloc, zero entry_alloc.
- **Capacity math confirmed at two orders**: order-6 raw+fs reaches
  64 MiB = 256 x 256 KiB; order-2 raw stops at 4 MiB = 256 x 16 KiB.
  Both = exactly max_cells(256) x cell_size.
- fsdev/fstrim gate (same session, after zero-page): **exit 0**, fstrim
  verified working on both devices (259,993,600 B trimmed each), all
  CRC pass.  The zero-page optimization and fstrim are green; the
  randio failure is orthogonal and older.

### Next Steps

1. **Fix the 256-cell cap**: pass a cell-count hint (disksize/cell_size
   + headroom) into `cell_pool_create` from cellzram's disksize-set
   path, OR grow the directory dynamically.  tmpfs pools keep a bounded
   default so their memory cost does not change.  Re-run the randio
   gate (cellzram fs phase must go green, zram unchanged).
2. **Decide the tmpfs side**: does a large tmpfs cell= mount hit the
   same 256-cell wall?  Likely not (compressible + size= accounting),
   but a targeted test (big incompressible file on a cell= tmpfs) would
   confirm the blast radius before the fix.
3. Re-verify the distinct-data phase (session 21) under the fix -- the
   64 MiB incompressible set is *exactly* the cap, so it likely
   under-counted cellzram's true cost by failing to store everything.
4. The randio gate should join the standing per-commit rotation once
   green (it is the only gate feeding incompressible-at-capacity data).

---

## Session 23: bootstrap for the capacity fix -- pages[1] landed, widen-vs-slice settled (2026-10-09)

**Purpose of this entry: let a fresh session implement the fix without
re-deriving anything.**  Read this, then go to "Implementation Plan".

### What We Did

1. **Landed the metadata prerequisite** (`152a30027085`):
   `struct cell.pages[1 << CELL_ORDER_MAX]` -> `pages[1]`.  The array
   was dead -- every access in both TUs is `pages[0]`, never indexed,
   never `sizeof`/`memset`'d.  A cell is one physically-contiguous
   compound page (single `alloc_pages(order)`), so the head reference
   alone is the allocation.  `sizeof(struct cell)` 8296 -> 112 B; a
   1024-cell directory drops 8.1 MB -> 112 KB.  This is what makes a
   capacity-sized directory affordable instead of trading the
   correctness bug for a memory bug.

2. **Gates re-run post-change, all as expected**: cellzram 77/0,
   fsdev exit 0, randio byte-identical pre-existing failure (25915
   verified, errno=5, 64 I/O errors).  The randio failure is untouched
   -- correct, this commit touched no allocation logic.

3. **Settled widen-vs-slice** (user question: "can we slice per 64k
   cells?"): the u16 cellid ceiling is SOFT, but the answer is widen,
   not slice.  Verified bit budget of the live handle:

   ```c
   shmem_comp_pack_loc(u16 cellid, u32 offset)
       return ((unsigned long)(cellid + 1) << 32) | offset;
   // bits  63        48 47     32 31     22 21    0
   //       [ unused 16b][cellid+1][unused 10b][offset 22b]
   ```

   The handle is a full `unsigned long` (64 b), stored in
   `entry.handle` -- NOT a swp_entry, so no 55-bit squeeze.  26 spare
   bits sit in it today (offset needs 22 of its 32; cellid+1 uses bits
   32-47, leaving 48-63 empty).  The u16 is a *type choice*:
   `cellid` is declared u16 in struct cell, `max_cells`, and ~15
   function signatures -- that is the only wall.  A two-level
   {slice, within-slice} directory would need a slice index in those
   same spare bits AND add a second indirection + slice-aware GC.
   Widening cellid u16->u32 uses the bits directly: one flat directory,
   ~15 signature changes, +2 B in struct cell.  24-bit cellid = 16.7M
   cells = 256 GB at order-2.

4. **Confirmed the ceiling is not the bug.**  `max_cells=256`
   (cell_store.c:53) is the bug.  u16's 65535 = 64x what a 256 MiB
   device needs (1024 cells); the ceiling only binds above 15 GB
   (order-6) or 1 TB (order-2) of incompressible data on one device.
   So: derive max_cells (fixes the failure); widen cellid LATER if a
   >15 GB device ever appears -- cheap, self-contained, not a
   prerequisite.

### What We Learned

- **`struct page` cannot be embedded by value.**  It is a slot in the
  kernel's `vmemmap`, indexed by PFN (`pfn_to_page(pfn) = vmemmap +
  pfn`); that slot IS the page's authoritative state (refcount,
  mapcount, buddy flags) and `__free_pages()` mutates that exact slot.
  A copy in struct cell would be a second inert object.  Only a
  reference (pointer or PFN) is meaningful.

- **But the pointer was redundant with `base` anyway.**  `base =
  page_address(page)` and `page = virt_to_page(base)` are bijective for
  direct-map pages.  Checked the precondition: `cell_gfp()` returns
  `GFP_KERNEL`/`GFP_TRANSHUGE`, never `__GFP_HIGHMEM`, so every cell is
  direct-map and the equivalence holds (on highmem `page_address()`
  returns NULL and it breaks -- worth checking, not assuming).  The
  `pages[]` field could be DELETED (recover with `virt_to_page(c->base)`
  at the two `__free_pages` sites); user chose the conservative
  `pages[1]` instead, deferring the derivation until it is understood
  cold.  Right call: smallest mechanical change, identical semantics.

- **A type used in ~15 signatures is a wall you can see; a type used
  in one inline packing function is a wall you can route around.**  The
  u16 looked hard until the bit budget showed the handle was half
  empty.  When a ceiling surprises you, price the ENCODING, not the
  declaration.

- **Style rule (user, standing):** terse but always motivated -- every
  claim carries its mechanism.  "Otherwise it's cult not science."

### Files / Commits

| commit | what |
|--------|------|
| `152a30027085` | cell_store: pages[1024] -> pages[1] (8296 -> 112 B/cell) |
| `1511001` (rig) | test_randio + TEST_CELLORDER passthrough + EXT4/ZRAM config |
| `5cb7a6e2b98c` | cellzram zero-page optimization (session 21, green) |

| location | fact |
|----------|------|
| `mm/cell_store.c:53` | `cp->max_cells = 256;` -- THE BUG |
| `mm/cell_store.c:36` | `cell_pool_create(unsigned int order, bool use_thp)` -- add capacity param here |
| `include/linux/cell_store.h:139` | its prototype |
| `mm/cellzram.c:517` | `shmem_comp_pool_create(algo, cell_order)` -- disksize IS known here (`size_bytes`, set 2 lines below to `cz->logical_bytes`) |
| `mm/shmem_compress.c:1891` | tmpfs call site -- must stay BOUNDED (no disksize; keep a default) |
| `include/linux/cell_store.h` | struct cell (now 112 B), struct cell_pool (max_cells u16) |

### Implementation Plan (for the next session)

**Goal:** derive `max_cells` from device capacity so cellzram can store
its advertised `disksize` in incompressible data.  Fixes the randio
gate.

1. **Thread a cell-count hint** through the two create layers:
   `cell_pool_create(order, use_thp, nr_cells)` ->
   `shmem_comp_pool_create(algo, cell_order, nr_cells)`.
   - cellzram (`cellzram.c:517`): pass
     `DIV_ROUND_UP(size_bytes, cell_size) + headroom`.
   - tmpfs (`shmem_compress.c:1891`): pass 0 / a bounded default --
     behavior unchanged; tmpfs page cache is compressible and bounded
     by size= accounting, never hit this.

2. **Open decision #1 -- the u16 ceiling.**  Clamp `nr_cells` to
   `U16_MAX` and let incompressible writes past `65535 x cell_size`
   fail IOERR (recommended: matches zram, which also cannot store
   unbounded incompressible data; compressed data needs far fewer
   cells so it is unaffected), OR reject the `disksize` with EINVAL at
   set time if it needs >65535 cells at the chosen order (cleaner
   contract, but rejects valid compressed workloads).  RECOMMENDED:
   clamp + one `pr_warn` stating the incompressible cap at this order.

3. **Open decision #2 -- headroom, and the gap derivation cannot
   close.**  The write path COW-allocates the new block before
   `xa_store` frees the old, AND a freed block only returns a cell to
   the buddy when the whole cell empties (`live_bytes == 0`), else it
   waits for GC.  So overwrite churn accumulates partially-filled cells
   past `live_data / cell_size`.  Two separable options:
   a. Fixed multiplier headroom (e.g. +25%) -- simple, arbitrary.
   b. **GC-on-exhaustion**: `cell_make_active` currently returns
      `-ENOMEM` when no free slot exists, WITHOUT trying GC.  Make it
      run one GC pass to drain sparse cells, and fail only if that
      also comes up empty.  This is the more robust half: a bigger
      directory reduces how OFTEN exhaustion happens; this fixes WHY
      exhaustion is fatal.  Recommend (b), possibly with (a) as a
      small safety margin.

4. **Acceptance:** randio gate cellzram fs phase must go GREEN (was
   25915 verified / ~2500 bad / errno=5); zram unchanged; cellzram
   77/0; fsdev exit 0; config-off build clean.  Then re-run the
   distinct-data phase (session 21) -- its 64 MiB incompressible set
   is EXACTLY the old cap, so it likely under-counted cellzram's true
   cost and its 101.8%-of-zram number needs re-measuring under the fix.

5. **Do NOT widen cellid in this change.**  It is a separate,
   self-contained follow-up, only needed above 15 GB/device.  Keep the
   capacity fix bisectable on its own.

### Next Steps (ordered)

1. Implement the plan above (one bisectable commit; rig gates green).
2. Re-measure distinct-data RAM under the fix (the 101.8% may move).
3. Optional follow-up: widen cellid u16->u32 using the handle's spare
   bits, IF a >15 GB cellzram device is ever a target.
4. Optional: delete `pages[]` entirely (recover via `virt_to_page(c->base)`);
   deferred until understood cold.
5. randio joins the per-commit rotation once green.

---

## Session 24: the capacity fix lands, and the repack that fell out of it (2026-10-09)

Executed session 23's implementation plan (all 5 steps), then kept
going on the u32 question the bootstrap had deferred. Three kernel
commits, all gates green including the one that had been red since
session 22.

### What We Did

1. **`e30aceffc459` -- the capacity fix (plan step 1).**
   `cell_pool_create(order, use_thp, nr_cells)` ->
   `shmem_comp_pool_create(algo, cell_order, nr_cells)`.
   cellzram derives `nr_cells = DIV_ROUND_UP(disksize, cell_size) +
   headroom`, headroom a module param `cell_headroom_pct`, default
   **30** (user choice), validated 0..100 at init.  tmpfs passes 0 ->
   CELL_DEFAULT_MAX_CELLS (256), its memory cost unchanged.  The
   plan's decision #2 resolved as recommended: clamp, do not reject.
   (The clamp moved later -- see commit 3.)

2. **Design Q&A that reshaped the follow-up** (the session's real
   content -- five questions, each changing the answer):
   - *"u32 cellid cost?"* -- the wall was never the type; it was the
     handle encoding (`cellid + 1 << 32`, room for 16 bits).  Type
     widening is ~15 signatures + locals.
   - *"move cellid and liveobjects to fill a 64b?"* -- yes: evicting
     two redundant fields made the u32 pair size-neutral (below).
   - *"bool active could be live_obj != 0?"* -- NO, and the
     distinction matters: active-with-zero-objects (just allocated)
     and inactive-with-objects (previous cells, the common write-
     burst state) are different states; `live_objects != 0` conflates
     exactly the two the GC wants told apart.  The correct
     derivation is `c == cp->active`, already used by
     `cell_pool_pick_victim()`.
   - *"all bools -> one u32 index at the top?"* -- the slot already
     exists (`cp->active`, a pointer).  Index vs pointer: moot,
     the directory is kvcalloc'd once and never reallocates, so
     pointers cannot dangle; pointer wins on NULL-is-zero-init.
   - *"at end to avoid misalignment / header more common?"* --
     header is right for pool-wide state (that IS the evictions),
     but appends cost stride: the in-place repack fills bytes that
     already existed, so no array element moves.

3. **`362abeaae38a` -- drop the shadows.** `struct cell` cached
   `u8 order` (= cp->cell_order, same for every cell) and `bool
   active` (mirror of cp->active).  Both are pool-wide facts
   duplicated per-cell: drift bugs waiting for a third update site
   (cell_make_active and cell_pool_return each maintained the pair).
   13 read sites now use `cp->cell_order`; the one reader of
   `active` (cell_stats debug print) compares `c == cp->active`.

4. **`3d7c4b4799a7` -- u32 widening + re-pack.** `live_objects`
   u16->u32 (fixes the overflow: ~26 B lz4 blocks in an order-9/10
   cell = ~160k objects > 65535); `cellid` u16->u32 (removes the
   65535-cell directory ceiling); `max_cells`/`nr_cells` u32; loop
   counters and locals follow.  The u32 pair lands in the 8 bytes
   the old `u16+u16+u8+bool+pad` occupied -- struct cell measured
   **80 B before and after** (compile-probe:
   `char (*)[sizeof(struct cell)] = 1` reads the size from the error).
   Handle re-pack at `<< CELL_LOC_OFFSET_BITS` (22): offset 22 bits
   (max cell 4 MiB), cellid+1 33 bits, 9 spare in the 64-bit
   entry.handle; +1 bias preserved (handle 0 = storage-failed
   sentinel, the session-3 rule).  Comment now says: re-price the
   ENCODING, not the declaration.

### What We Learned

- **Config-dependent sizeof: the log's "112 B" was the rig's KASAN/
  lockdep config** (lockdep inflates rw_semaphore); the plain build
  reads 80.  Quote deltas, not absolutes, across configs.
- **The clamp's location matters for -Wtype-limits:** clamping an
   `unsigned int` parameter to UINT_MAX is always-false and warns.
  The guard belongs at the cellzram boundary (u64 disksize
  arithmetic) where the wide type actually is.
- **`page_same_filled`-class lesson, new flavor: read the field
  budget, not the field.**  The u16 cellid looked like a type wall
  for two sessions; it was a shift-amount wall in one inline
  function.  Session 23 predicted this; confirming it in 20 minutes
  validated the "price the encoding" rule.
- **A design Q&A beats a design doc.**  The five-question chain
  (this entry's step 2) eliminated fields the plan had not
  questioned; each answer was derived from a state analysis
  (four-state table for active), not a style preference.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cell_store.c` | nr_cells param + default; u32 ids/loops; order/active reads -> cp |
| `include/linux/cell_store.h` | struct cell repack (80 B, no order/active); u32 fields; CELL_DEFAULT_MAX_CELLS; invariant #5 |
| `mm/shmem_compress.c` | pool_create nr_cells pass-through; 3 sites -> cp->cell_order / c == cp->active; u32 locals |
| `include/linux/shmem_compress.h` | pack/unpack u32 at <<22; CELL_LOC_OFFSET_BITS/MAX |
| `mm/cellzram.c` | cell_headroom_pct param (default 30); disksize-derived nr_cells + u64 truncation guard |

### Test Results

| gate | result |
|------|--------|
| randio (THE acceptance) | **4 passed, 0 failed**, exit 0 -- fs phase 28414 blocks verified, 0 bad (was ~2500 bad / 64 IOERR); zram unchanged |
| cellzram suite | 77/0 |
| fsdev + distinct-data | 19/0; distinct phase now stores all 16384 blocks: 64 MiB phys + 1179648 B entries = **101.8% of zram, CONFIRMED** (session 21's number was measured AT the old cap but the ratio held) |
| full suite (QEMU_CPUS=4, KASAN) | exit 0, no Oops/KASAN |
| entryrace | 0 underflows, quiescent verify PASS x4, stale_value_retries 0 |
| config-off build (worktree, CONFIG_TMPFS_COMPRESSION=n) | mm/shmem.o + mm/cell_store.o clean, 0 warnings |

Kernel commits: `e30aceffc459`, `362abeaae38a`, `3d7c4b4799a7`
(each build-clean on its own; bzImage #83/#84/#85/#86).

### Next Steps

1. randio joins the per-commit rotation (plan step 5; it is green).
2. Optional: GC-on-exhaustion (plan decision #3b) -- still open, and
   now the only remaining way writes can fail IOERR (the directory
   no longer caps practical workloads; churn does, at 30% headroom).
3. Watch `cell_headroom_pct` in the field: 30 is unmeasured
   headroom, chosen for safety.  A churn-specific sweep could
   right-size it.
4. Re-check session 23's remaining optionals: delete `pages[]`
   entirely (`virt_to_page(c->base)`), >15 GB devices at order-6
   now possible -- the u32 widening removed the ceiling, so the
   "IF a big device appears" condition can now be tested by simply
   setting disksize.


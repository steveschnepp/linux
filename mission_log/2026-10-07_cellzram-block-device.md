---
name: cellzram-block-device
description: cellzram -- the compressed-RAM block device on the cell allocator, and its unit-bug parade (session 5)
project: linux
previous: 2026-10-07_tmpfs-upstream-carve.md
---

# Mission Log: cellzram -- the Block Device, and a Parade of Unit Bugs

Part 3 of the tmpfs transparent-compression series, split verbatim from the
combined log `2026-10-05_tmpfs-transparent-compression.md` (kept until the
split is validated). Session numbers are preserved so cross-references keep
resolving.

Covers session 5 (2026-10-07): implementing mm/cellzram.c (~740 lines) on top
of shmem_comp -- bio paths, block table, sysfs, swap trial -- and the nine
bugs the rig found, which landed the mandatory unit-suffix naming rule and
the O_DIRECT-for-block-devices rule.

## Session 5: cellzram -- the block device, and a parade of unit bugs (2026-10-07)

### What We Did

Implemented the committed cellzram spec (compressed RAM block device
on the cell allocator) end to end: `mm/cellzram.c` (~740 lines),
`CONFIG_TMPFS_COMP_BLKDEV` in fs/Kconfig next to its dependency,
mm/Makefile, and a 77-assertion rig test gated behind
`test.cellzram=1`. One kernel commit (the device is a single coherent
unit -- the spec's phases 1-3 landed together because the code was
written in one pass; bisectability comes from the commit being
atomic and green, not from artificially splitting it) plus the
rig wiring.

The device reuses shmem_comp wholesale: a private cell-backed pool,
the global entry ID table, pinned relocation-safe loads, entry
refcounting, the GC/shrinker. The block table is a per-device xarray
mapping block_nr -> entry id; entry length is the raw/compressed
discriminator; writes replace atomically via xa_store's
return-old (this tree has no xa_exchange); readers pin with
entry_get before touching storage and retry on a lost race.

**The bug parade** (each found by the rig, each a lesson):

1. **sysfs name collision**: a device attribute named `stat` makes
   device_add_disk() fail with -EEXIST -- the block layer owns
   `/sys/block/X/stat` on every gendisk. Renamed to `io_stat`
   (zram's precedent).
2. **disksize is bytes, test wrote MiB**: every set_disksize failed
   EINVAL. zram semantics: bytes.
3. **Buffered bdev I/O hid the device**: pwrite/pread on a raw block
   device goes through the page cache, so "round-trips" were
   verified against the cache, mm_stat counters read zero (bios not
   emitted yet), and discard freed nothing. The test now uses
   O_DIRECT with page-aligned buffers: deterministic device-path
   verification.
4. **load_entry copies *len bytes, clamped only to the cell
   boundary**: passing PAGE_SIZE as capacity copied adjacent
   bump-allocated garbage past every compressed block and returned
   *len=4096, which the raw/compressed discriminator then read as
   "raw". Compressed readbacks were garbage. The caller must pass
   the entry's stored length.
5. **kfree() on the bio page**: the read path's decompress branch
   did `kfree(tmp)` unconditionally, where tmp == dst (the kmap'd
   bio page) in the common full-page case. SLOB/SLUB's
   free_large_kmalloc warned (page refcount 1026!) -- a random
   kernel page was being freed on every compressed read.
6. **bi_size is bytes, sector_t is sectors**: the discard loop
   computed `end = (sector + bio->bi_iter.bi_size) >> 3`, mixing
   units -- punch(0, 16M) freed blocks 0..2097152, i.e. the whole
   device. Caught because "objects halved" read objects=0.
   -> The user's rule landed here: rename every variable with its
   unit (block_nr, len_bytes, dlen_bytes, logical_bytes,
   size_bytes, start_block/end_block, orig_bytes/compr_bytes/
   phys_bytes) so the type error is visible in the name.
7. **Test-side bugs** (not device bugs, but they burned two runs):
   the FNV compare initialized h2 from 0 while the fill functions
   return the offset basis (every checksum "failed"); the matrix
   readback seeded the random quarter by device block while the
   write seeded by loop index (only blocks >= 6144 mismatched --
   identical h2 across all three algorithms was the tell that the
   device was innocent).
8. **Swapout is not guaranteed by pressure**: 1.5 GB of touched anon
   on a 2G KASAN guest left MemFree at 60 MB and the kernel never
   allocated a single swap slot (watermarks not crossed, and
   MADV_PAGEOUT returned ENOSYS because the rig config lacked
   CONFIG_ADVISE_SYSCALLS). Fix: enable the config and use
   MADV_PAGEOUT on the cold half -- deterministic, watermark-
   independent. Swap writes then hit the device (io_writes 1 ->
   32640 blocks = ~127 MB swapped out).
9. **mkswap residue**: the swap signature block is not a swap slot,
   so swapoff legitimately leaves it stored -- one live block
   pinning a whole 64-page cell. The test punches block 0 after
   swapoff and then asserts a full drain.

Punch-hole support itself needed write-zeroes-unmap queue limits:
blkdev_fallocate maps PUNCH_HOLE to blkdev_issue_zeroout with
BLKDEV_ZERO_NOFALLBACK, which requires hardware write-zeroes offload
(max_write_zeroes_sectors + max_hw_wzeroes_unmap_sectors); discard
limits alone get -EOPNOTSUPP. My submit path treats WRITE_ZEROES as
discard, so punched blocks free their cells inline.

### What We Learned

**Technical**

- Raw block devices buffer: any test or user of /dev/cellzramN
  (or /dev/zramN) that wants to observe device state must use
  O_DIRECT, or account for writeback timing. Swap is exempt (swap
  I/O is direct by construction), which is why the swap trial is the
  honest device-path test.
- shmem_comp's load contract: `*len` is capacity in, stored length
  out, clamped only to the cell boundary -- the entry's length field
  is the authority, not the buffer size.
- The block layer owns `stat` on every gendisk; device attributes
  must pick another name.
- Forcing swap in a test: watermarks are a bad lever (KASAN guests
  sit above min at MemFree=60M); MADV_PAGEOUT is deterministic and
  needs CONFIG_ADVISE_SYSCALLS.
- Debugging sysfs reads that return zeros: print the struct pointer
  from both the writer and the reader (%p is hashed by the kernel --
  use it to compare sameness, not identity) before assuming two
  instances exist.

**Process**

- The unit-suffix convention (user-imposed mid-session) would have
  caught bug 6 at review time; it is now project law for new code.
- perl -pi for systemic renames + git diff review + edit tool for
  stragglers worked, but the word-boundary regex still ate
  `#include <linux/blk-mq.h>` (a `-` terminates a word). Always grep
  for clobbered include paths and format strings after a bulk rename.
- Identical wrong output across independent configurations (the same
  h2 for lz4/zstd/none) localizes the bug to the test, not the code
  under test.

### What We Decided

- One commit for the whole device (spec phases 1-3 together), with
  the rig suite green as the gate; the carve commits were split
  because they move existing behavior, the device commit adds new
  behavior atomically.
- io_stat instead of stat (block layer owns the name); noted as a
  spec deviation in the commit message.
- mkswap-signature residue handled by the test (punch block 0), not
  by the device -- the device has no business special-casing it.
- CONFIG_ADVISE_SYSCALLS enabled in the rig config (test tooling
  need, not a device dependency).

### Rules Added

- **Unit suffixes are mandatory** in project kernel code: _bytes,
  _pages, _sectors, _kb, _order as appropriate. A count without a
  unit suffix in a block-device or compression path is a review
  blocker.
- **Bulk renames: perl + diff review + grep for clobbered includes
  and strings**; fix stragglers with the edit tool.
- **Tests of a block device use O_DIRECT** -- buffered I/O verifies
  the page cache, not the device.

### What We'd Do Differently

- The rig test should have been O_DIRECT from the first draft; the
  buffered-I/O illusion cost a full debug cycle (mm_stat zeros,
  "corrupt" readbacks that were cache hits).
- Check /proc/sys/vm/swappiness and madvise support BEFORE designing
  a swap-pressure test; the watermark lesson was predictable in
  hindsight.
- The sysfs `stat` collision is a five-second grep of the block
  layer's own attributes away; do that when adding any gendisk
  attribute.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | The device: bio paths, block table, sysfs, swap notify, lifecycle |
| `fs/Kconfig` | CONFIG_TMPFS_COMP_BLKDEV (bool, default n, depends TMPFS_COMPRESSION) |
| `mm/Makefile` | cellzram.o |
| `mm/shmem.c`, `mm/shmem_compress.c`, `include/linux/shmem_compress.h` | Unit suffixes on carve-era fields/locals |
| rig `tests/test_cellzram.c` | 77-assertion suite (round-trips, discard drain, reset, algo matrix, swap trial) |
| rig `initrd/init`, `scripts/run_qemu.sh`, `scripts/build_tests.sh` | test.cellzram=1 gating, TEST_CELLZRAM passthrough, build + busybox applets |

### Test Results

- cellzram gated boot (TEST_CELLZRAM=1, 2G, KASAN kernel #59):
  **77 passed, 0 failed.** Swap trial: io_writes 1 -> 32640 blocks
  (~127 MB swapped out under MADV_PAGEOUT pressure), SwapFree
  131068 -> 0, full drain (objects==0, cell_stats empty) after
  swapoff + signature punch.
- Full tmpfs suite after all commits: **exit code 0**, no failures,
  no KASAN/Oops (race suite: 0 failures).
- mm_stat during the lz4 roundtrip: ratio_per_10000 = 3500 (35% of
  original -- lz4 on mixed text/random), phys < logical throughout.

### Next Steps

- cellzram phase-3 hardening per the spec's risk table: bounded
  internal retry on ENOMEM (v1 matches zram's fail-the-write), and a
  concurrent dd-writers + gc_now + discard-storm KASAN soak (the
  suite touches each path but not all concurrently on the device).
- v2 lever from the spec's open questions: per-cell batched entries
  (one entry per N contiguous blocks) to cut entry metadata ~32x at
  8 GiB scale.
- Upstream series: docs (tmpfs.rst mount options + sysfs), the
  experimental help-text wording, and the sysfs-group naming question
  (device pools appearing under /sys/fs/tmpfs/compression/).

---

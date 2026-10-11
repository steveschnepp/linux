# blkzcell: Compressed Log-Structured Block Layer for SD/MMC Specification

## Overview

`blkzcell` is a block layer that sits between an unmodified filesystem
(FAT32, ext2) and an SD/MMC card. It compresses writes, concatenates the
variable-length compressed records append-only into erase-group-aligned
segments, synthesizes discard for filesystems that never send TRIM, and
reclaims dead space with a log-structured cleaner. The filesystem never
learns it exists; the card only ever sees erase-aligned sequential appends
and whole-erase-group TRIMs.

The goal is *optimal usage of the card*, not optimal usage of RAM. The
scarce resources are card write bandwidth, flash endurance, and effective
capacity on cheap commodity eMMC/SD. RAM (1-4 GB on the target SBCs) is the
working material, not the constraint.

## Motivation

The fleet is SBCs with 1-4 GB RAM booting from eMMC/SD (see
`mission_log/2026-10-11_openwrt-refocus.md`). Filesystems like FAT32
(boot partitions) and ext2 (small rootfs/data) issue random 4K overwrites
and never send TRIM. On a cheap card this is the worst possible workload:

- Random overwrites force the card's internal FTL garbage collector to copy
  pages constantly; cheap cards fall off a throughput cliff and die young.
- With no TRIM, stale blocks accumulate; the FTL's free pool shrinks until
  GC has no headroom and write amplification spikes toward unbounded.
- Effective capacity erodes as the card fills with garbage it cannot reuse.

`blkzcell` fixes this below the filesystem. It converts the random-overwrite
pattern into sequential appends and erase-aligned TRIMs (what the FTL is
optimized for), compresses data so fewer bytes are programmed (endurance +
capacity), and reclaims stale space itself so the card's GC never starves.

This work was parked in `mission_log/2026-10-11_openwrt-refocus.md` with the
revisit condition "after arm 3 is integrated and measured." It is consciously
reopened as a **parallel track**: the session-35 verdict
(`2026-10-10_zcell-verdict-tmpfs-pivot.md`) killed zcells-as-RAM-blockdev on
*random RAM writes* and says nothing about flash geometry, where O(1) bump
allocation and synchronous free are a different game.

## Goals

- Serve an unmodified FAT32/ext2 block device on SD/MMC with transparent
  compression and synthesized discard.
- Convert random overwrites to erase-group-aligned sequential appends; the
  card never sees an in-place overwrite of a sealed segment.
- Honor `flush`/`FUA` so that after `fsync` returns, both the data and its
  mapping are durable and checkpointed.
- Recover the full sector -> data mapping after an unclean power cut by a
  linear header scan plus a bounded replay, with no superblock to keep
  current.
- Bound the RAM working set (open segment + index + GC buffer) adaptively as
  a percentage of RAM, capped.
- Reuse the proven allocator discipline from `mm/cell_store.c` and the bio
  skeleton from the deleted `mm/cellzram.c` (`49b5571006b4^`).

## Non-Goals

- Not a tmpfs/ztmpfs feature. Compression of RAM-resident tmpfs files is the
  separate `feature/tmpfs-compression` track; blkzcell shares no code path
  with it beyond conceptual reference.
- Not a raw-NAND (MTD) layer. SD/MMC hides its NAND behind a card FTL; this
  drives the card's *block* interface, not `mtd`. UBI/UBIFS is the MTD
  analog, not a dependency.
- Not a zoned device driver. No ZNS/SMR host-managed zoned support in v1.
- No snapshots, cloning, encryption, or RAID.
- No resize of a live device in v1.
- No swap-on-blkzcell in v1 (the `cellzram` swap trial is reference only).
- No auto-tuning of compression algorithm in v1 (lz4 fixed default; sysfs
  override for test).

## Design

### Device model

A `blkzcell` device is a `gendisk` created via a control node, styled on
zram (`/sys/class/zram-control/hot_add`). It wraps one backing block device
(the SD/MMC partition). Configuration is by sysfs before first use:

| Attribute | Meaning |
|---|---|
| `backing_dev` | path to the backing block device (e.g. `/dev/mmcblk0p2`); set once |
| `disksize` | *logical* size in bytes presented to the FS; must be <= physical capacity (v1: no over-provision; see Risks) |
| `comp_algorithm` | `lz4` (default) or `none` |
| `io_stat` | cumulative event counters (NOT `stat` -- the block layer owns that name; `cellzram` lesson) |

The device is formatted on first write of `disksize` (writes a superblock +
initial index dump). `reset` re-formats to empty.

### On-media layout

```
[ sb0 | sb1 | seg 0 | seg 1 | ... | seg N-1 ]
```

- Two superblocks (`sb0`, `sb1`), erase-group-aligned, mirror each other;
  each holds `{magic, format_version, seq, seg_count, seg_bytes,
  logical_bytes, crc32c}`. Written alternately at checkpoint time so a torn
  superblock is caught by CRC and the mirror is used. This is the *only*
  fixed-position structure; everything else is append-only.
- Segments are the append log. Segment `i` occupies a fixed byte extent
  `seg_bytes`, aligned to the backing device's erase group
  (`discard_granularity` / `pref_erase`, read at open from queue limits).
  `seg_bytes` is immutable for the device's life.

### Segment geometry

A segment is **fixed-extent and erase-aligned** on media (so `discard(seg)`
== exactly one erase-group TRIM), but its **contents are byte-packed with no
internal alignment**. Segment size on media is fixed; the packed content
length within it varies.

```
segment (seg_bytes = one erase group, e.g. 512 KiB - 4 MiB):
  [ rec0 ][ rec1 ][ rec2 ] ... [ recK ][ free slack to seg_bytes ]
```

This is the flash translation of `mm/cell_store.c`'s cell: a RAM cell of
`order N` becomes an erase-group segment of `seg_bytes`. The byte-packed
bump allocation, the `live_bytes` accounting, the sparse list, and the
pick-victim / relocate-one / release-victim GC protocol all carry over
unchanged; only the backing changes from `alloc_pages()` (RAM, memcpy) to a
flash extent (I/O).

### Record format

Every record in a segment (data or index dump) has an 8-byte header. The
header is little-endian explicit bytes, NOT C bitfields (bitfield layout is
implementation-defined; this format lives on a card across boots/compilers).

```c
/* on-media record header, little-endian, 8 bytes, never padded */
struct zcell_rec {
	__u8   type;      /* 0 = data, 1 = index dump, +namespace */
	__u8   len[3];    /* payload bytes, little-endian u24 */
	__le32 crc32c;    /* crc32c over type + len[] + data[0..len) */
	__u8   data[];    /* payload; next rec at 8 + len */
} __packed;

#define ZCELL_REC_HDR		8
#define ZCELL_REC_MAX		((1U << 24) - 1)   /* 16 MiB >> any segment */
#define ZCELL_REC_T_DATA	0
#define ZCELL_REC_T_DUMP	1
```

- **CRC covers the whole `type + len` prefix plus the payload**, so a torn
  `type`, a torn `len`, and a torn payload are all caught by one test -- the
  recovery walk's stop condition.
- **A record never straddles a segment boundary.** Before appending,
  `if (next_free + ZCELL_REC_HDR + len > seg_bytes) seal(); open();`
  (identical to `cell_store`'s `if (next_free + len > cell_size)` rule).
  GC relocates whole records; a straddling record could have its halves
  reclaimed at different times. The index dump must therefore fit in one
  segment payload -- asserted at write (it is KBs after encoding; see below).
- **Byte-packed inside, erase-aligned outside.** The card's smallest write is
  a 512-byte block, so the *last* record of a sealed segment is written with
  one short block whose trailing bytes are stale; that is fine because CRC
  covers only `len` bytes and the next record is in the next segment. Pad at
  the I/O call, never in the format.

### RAM index (the mapping)

A RAM xarray maps filesystem sector -> record location. It is extent-based
to bound its size: a run of filesystem sectors whose records are contiguous
in a segment maps as one extent.

```c
struct zcell_extent {
	u64 fs_sector;     /* first filesystem sector covered */
	u32 nr_sectors;    /* run length */
	u32 seg_nr;        /* backing segment */
	u32 byte_off;      /* byte offset of first record within segment */
};
/* xarray key = fs_sector; value = one or more zcell_extent (multi-index) */
```

Consecutive appends land consecutively in the open segment, so sequential
filesystem writes collapse to a handful of extents. This is what keeps the
index proportional to *contiguous runs*, not to raw sector count, and it
bounds the working set on a 1-4 GB box.

### Write path (RMW / overwrite)

A filesystem write of a 4K block is compressed and appended to the open
segment; the old location, if any, is marked dead. Nothing is modified in
place.

```
submit_bio(WRITE, block B):
  compress B -> payload (or raw if incompressible; type=DATA)
  if (open segment would overflow): seal(); open()
  off = open.next_free
  append rec{type=DATA, len, crc32c, payload} to open segment RAM buffer
  open.next_free += 8 + len;  open.live_bytes += 8 + len
  index[B] = extent{B, 1, open.seg_nr, off}   // replaces prior extent for B
  if (old location exists): old_seg.live_bytes -= old_len   // mark dead
```

An overwrite is a new append plus a `live_bytes` decrement on the old
segment. The old bytes are dead in place, awaited by the cleaner. Reads use
the index, so they always find the newest copy; the dead bytes are never read
and never reused until GC reclaims the whole segment.

### Flush path (seal on flush)

`REQ_OP_FLUSH` / FUA MUST be honored: after `fsync` returns, the data and its
mapping are durable. Because the card cannot have the same erase group
rewritten repeatedly (that is the random-overwrite FTL amplification we exist
to prevent), a flush **seals** the open segment rather than re-writing it.

```
REQ_OP_FLUSH:
  seal(open):                    // one sequential extent write to card
      write open.next_free bytes of the open buffer to card at seg slot
      (remainder of the erase group left unwritten/erased)
      submit card cache flush
  append index dump (see Checkpoint) as a DATA-less DUMP record in a fresh seg
  open = new empty segment
```

The cost is internal fragmentation: a flush-heavy workload yields many
partially-filled segments, later evacuated by the cleaner (capacity/GC
pressure, never correctness). This matches ZFS/nilfs2 `fsync` behavior and is
acceptable for the bursty-fsync SBC target (log appends, config rewrites,
package-db churn). A database fsync storm is explicitly not the target; the
escape hatch (a separate small journal region) is a v2 lever, not v1.

### Checkpoint (index dump) and recovery

The index dump is the whole extent-encoded, RLE-compressed index, appended as
a `type=DUMP` record. Because filesystem writes are sequential, the index is
almost entirely runs and compresses to KBs against a 512 KiB-4 MiB segment.
A dump must fit in one segment payload (asserted).

Each checkpoint writes the superblock (alternating `sb0`/`sb1`) with an
incrementing `seq`, and appends the dump. **Recovery** is a linear header
scan; monotonic `seq` finds the newest checkpoint without a persisted head
pointer:

```
mount:
  best = NULL
  for each segment in physical order 0..N-1:
      read 8-byte rec header
      if crc32c fails or type not in {DATA, DUMP}: break   // unwritten/torn tail
      if rec is DUMP and rec.seq <= best.seq: break        // older lap -> stop
      if rec is DUMP: best = this dump
      step to next record via len
  load best superblock + best dump            // rebuild the extent index
  replay: for each DATA record after best dump (physical order):
      index[sector] = its extent              // self-describing; newest wins
  open = next free segment after the last valid record
```

Why "once you find a smaller `seq`, the previous was the last" is correct for
every head position: `seq` is 64-bit and never resets, so it climbs across
laps. When the circular log's head has wrapped to physical `h`, segments
`0..h-1` hold the fresh lap (high seq) and `h..N-1` hold the prior lap (lower
seq); the scan climbs then drops exactly at `h`, and the checkpoint before the
drop is newest. On the first (never-wrapped) lap there is no drop and the scan
stops at the unwritten tail; the last valid record read is newest. One loop
handles both. A torn checkpoint at the head is caught by CRC and falls back to
the previous good one -- append-only makes this safe by construction with no
double-buffered superblock.

After an `fsync`, everything at or before that point is checkpointed, so the
replay tail is bounded by *un-fsynced* writes only: a cleanly-fsynced system
mounts with near-zero replay.

### GC / cleaner (the roll)

Dead space is reclaimed by relocation, never in-place compaction -- the log
rolls forward.

```
cleaner pass:
  pick victim = sealed segment with lowest live_bytes / seg_bytes (cost-benefit)
  if victim holds the newest valid dump: skip (pinned until a newer dump exists)
  for each live record in victim:
      read record from card (RAM buffer)
      if open segment would overflow: seal(); open()
      append record unchanged to open segment; update index to new extent
      victim.live_bytes -= record_len
  if victim.live_bytes == 0:
      index no longer references victim
      discard(victim)            // ONE erase-group TRIM to the card
      mark segment free
  else: re-link victim as sparse for a later pass
```

Write amplification is `1 / (victim live fraction)`, **leveled across the
whole device** by cost-benefit victim selection (pick the sparsest segment
first), not concentrated on the hot block. This is exactly
`cell_pool_pick_victim` / `cell_relocate_one` / `cell_pool_release_victim`
from `mm/cell_store.c`, with the copy being card-RAM-card instead of
memcpy-to-memcpy.

The cleaner runs when free segments drop below a **watermark** (see below) or
on a background tick / shrinker. Records are relocated *unchanged* in v1 (no
decompress-recompress); re-compression on relocate is a v2 lever.

### Free-segment watermark and over-provisioning

Cost-benefit cleaning keeps WA bounded only while there is free-space headroom
to relocate into. On a full card every victim is mostly live and WA spikes
toward unbounded. Therefore:

- A **free-segment watermark** forces cleaning (or stalls writes) before the
  log runs out of tail room. Writes block when free segments < watermark
  until the cleaner frees some.
- **Over-provisioning**: v1 presents `disksize` <= physical capacity, leaving
  slack segments the cleaner uses as relocation targets. The exact slack
  (e.g. keep 10-15% of segments free) is a tunable; exhaustion under a
  sustained full-device write is tracked as a metric (see `io_stat`).

`io_stat` exposes at least: `appends`, `seals`, `flush_seals`, `dumps`,
`cleaner_passes`, `records_relocated`, `segments_discarded`,
`write_amplification_permille` (bytes written to card / bytes the FS wrote),
`watermark_stalls`.

### Compression

`crypto_acomp` with `lz4` default (fast; the whole point is "quick
compression" -- CPU cost must stay below the media-write cost it saves). A 4K
filesystem block is compressed to its exact length and stored; if compression
does not shrink it, the block is stored raw (`len == 4096`) with no
compress/decompress cycle on read (the raw/compressed discriminator is the
stored length, `cellzram` design). Per-CPU acomp contexts, as in
`mm/shmem_compress.c`.

### Build changes

| Target | Change |
|---|---|
| `fs/Kconfig` | new `CONFIG_BLKZCELL` (tristate, depends `BLOCK`, selects `CRC32C`, `CRYPTO_LIB_DEFLATE`/acomp lz4) |
| `mm/Makefile` | unchanged; blkzcell is a block driver |
| `drivers/block/Makefile` | `obj-$(CONFIG_BLKZCELL) += blkzcell.o` |
| `drivers/block/Kconfig` | blkzcell entry (or `fs/Kconfig` next to the compression family, per house preference) |

`mm/cell_store.c` is **reference, not a link dependency**: its cells are RAM
pages (`alloc_pages`); blkzcell's segments are flash extents. blkzcell
implements its own segment staging that mirrors `cell_store`'s discipline
(bump alloc, `live_bytes`, sparse list, victim/relocate/release protocol).
The header-walk, CRC, checkpoint, and cleaner are new code.

### File manifest

| File | Action | Purpose |
|---|---|---|
| `specs/01_BLKZCELL_NAND_LAYER.md` | Create | This spec |
| `drivers/block/blkzcell.c` | Create | gendisk, sysfs, bio paths (read/write/discard/flush), segment staging, append, seal |
| `drivers/block/blkzcell_gc.c` | Create | cleaner: victim pick, relocate, discard, watermark |
| `drivers/block/blkzcell_map.c` | Create | extent xarray index, checkpoint dump/load, recovery walk |
| `drivers/block/blkzcell.h` | Create | `struct zcell_rec`, `struct zcell_extent`, `struct zcell_seg`, device struct |
| `drivers/block/Makefile` | Modify | add `blkzcell.o` |
| `drivers/block/Kconfig` | Modify | `CONFIG_BLKZCELL` |
| `include/uapi/linux/blkzcell.h` | Create | io_stat struct for the sysfs ABI |
| `tools/testing/selftests/blkzcell/` | Create | port of the rig gates (round-trip, discard, flush-durability, power-cut recovery) |

### Migration

Each phase leaves the build green and is independently testable.

**Phase 1 -- record format + segment staging (no I/O).**
`struct zcell_rec`, CRC32C, byte-packed append into a RAM segment buffer,
straddle guard, seal-on-full. Unit-test the format and the drop-scan recovery
logic in isolation. *~1 day.*

**Phase 2 -- bio read/write + extent index, RAM-backed segments.**
gendisk, sysfs (`disksize`, `io_stat`), submit_bio for read/write against an
in-memory segment array (no card yet). Extent xarray index. Port the
`cellzram` 77-assertion round-trip gate (`O_DIRECT`, `cellzram` lesson).
*~2 days.*

**Phase 3 -- flush + checkpoint + recovery.**
Seal on flush, index dump, superblock double-buffer, monotonic-seq recovery
walk. Power-cut test: kill mid-write, remount, verify mapping recovered.
*~2 days.*

**Phase 4 -- backing device + real I/O + discard.**
Point at a real block device (file-backed loop in the rig, then `/dev/sdX`).
Sealed-segment writes, GC reads, erase-group-aligned TRIM. Discard-synthesis
gate: FAT32/ext2 over blkzcell, verify TRIMs reach the card and free space.
*~3 days.*

**Phase 5 -- cleaner + watermark + WA measurement.**
Cost-benefit victim selection, watermark stalls, `io_stat` WA counter. Bench:
random-overwrite workload on ext2-over-blkzcell vs raw, measure
`write_amplification_permille` and host-file bytes written. *~2 days.*

**Phase 6 -- power-fail soak + rig fleet points.**
Unclean-shutdown loop under KVM; OpenWrt-shaped small-file churn on the 256M
and 64M rig points. *~2 days.*

### Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Erase group too large on cheap SD (multi-MiB) -> huge open segment / RAM spike, or tiny -> WA from many partial segs | Med | Med | Read `discard_granularity` at open; adapt the RAM budget to it; assert a sane `seg_bytes` range and refuse pathological cards |
| Recovery walk reads the whole log on a large full card (slow mount) | Med | Med | Dump-on-flush bounds replay to un-fsynced writes; header-only scan (not full data read); optional "scan from last known good segment" hint in the superblock (v2) |
| WA unbounded when the device fills under a sustained full write | Med | High | Free-segment watermark stalls writes; over-provision slack; track WA and exhaustion in `io_stat`; document the operating envelope |
| Checkpoint-per-flush fragments the log on fsync-heavy workloads | Low | Med | Accepted for the SBC target; incremental dump (delta since last) and journal-region escape hatch are v2 levers |
| CRC32C on every record is CPU cost on the write hot path | Low | Low | crc32c is HW-accelerated on ARM64/x86; measured in Phase 5; if it dominates, CRC the header only and rely on the FS for data integrity |
| Card lies about discard granularity / does not TRIM | Med | Med | Detect via queue limits; fall back to write-zeroes; if neither, degrade to "GC reclaims in our map, card space not physically freed" and warn |
| A record's extent spans the index dump boundary, confusing replay | Low | High | Records never straddle segments; dump pinned until a newer dump exists; asserted in the cleaner |

### Open Questions

- **`disksize` vs physical capacity.** v1 sets `disksize` <= physical. Should
  over-provision be user-visible (a `reserve_pct` knob) or auto-derived from
  `seg_count`? Decided during Phase 5 measurement of WA vs capacity.
- **Segment-size sanity range.** What is the minimum viable `seg_bytes` (a
  very small erase group) before WA from partial segments dominates? Needs a
  Phase 5 sweep.
- **Cleaner trigger.** Watermark + background tick + shrinker: which
  combination, and at what thresholds, on a memory-tight 64M box? Tuned in
  Phase 6 against the rig fleet points.
- **Re-compression on relocate.** v1 relocates records unchanged. If the
  cleaner re-compresses (new algorithm, better ratio), WA and CPU trade off.
  Deferred until baseline WA is measured.

### References

- `mm/cell_store.c` + `include/linux/cell_store.h` -- the allocator
  discipline (bump alloc, `live_bytes`, sparse list, victim/relocate GC) this
  mirrors.
- `git show 49b5571006b4^:mm/cellzram.c` -- the deleted bio/submit skeleton,
  block table xarray, discard-as-cell-free, WRITE_ZEROES handling, 77-assertion
  test.
- `mission_log/2026-10-10_zcell-verdict-tmpfs-pivot.md` -- why zcells lost as
  a RAM blockdev (2.7x random-write) and why that verdict does NOT apply to
  flash.
- `mission_log/2026-10-11_openwrt-refocus.md` -- the fleet (SBC, eMMC/SD,
  1-4 GB RAM), the parked-item revisit condition this spec consciously reopens.
- `mission_log/2026-10-07_cellzram-block-device.md` -- O_DIRECT test rule,
  `io_stat`-not-`stat`, unit-suffix rule, discard/punch-hole mechanics.
- `drivers/mmc/core/queue.c` -- `discard_granularity` / `pref_erase` /
  `mmc_erase_group_aligned`, the erase-group source for `seg_bytes`.
- `drivers/block/zram/` -- device-model/sysfs precedent; `mm/zswap.c`,
  `fs/f2fs/` + `fs/btrfs/compression.c` (inline compression in a log-structured
  allocator), `fs/nilfs2/` (checkpoint/roll-forward recovery precedent, no
  compression).

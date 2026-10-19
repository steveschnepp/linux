---
name: compressed-disk-spill-design
description: design the disk-spill tier for ztmpfs -- paging cold compressed blobs to a swap device/file with no decompress on pageout; rejects swappable cells (reclaim reentrancy + cell forfeiture), picks the zswap-style engine-managed spill through the swap subsystem
project: linux
---

# Mission Log: Paging Compressed Blobs to Disk (the Tier-3 Spill Design)

Part 60 of the tmpfs transparent-compression series.  A design session
(2026-10-19), bootstrapping from the record (logs 1-59) then answering a
run of architecture questions about whether ztmpfs can page its
compressed backing out to disk.  No kernel code changed; this log
captures the mapped design space and the chosen mechanism so a future
session does not re-derive it.

## The requirement (user)

ztmpfs must be able to page its compressed backing to disk.  A page that
was compressed into a cell but has been unread for a while should not be
pinned in RAM even in compressed form -- it should reach a swap
device/file and come back transparently.  Two constraints were stated
and re-stated: page out COMPRESSED (never decompress on pageout), and
the paging must be transparent.

## What We Did

The session was a sequence of architecture questions, each answered
from the code and the earlier logs.  In order:

### 0. Entry point: are watermark-async + bvec grouping relevant to ztmpfs?

The session opened by asking whether the deferred "commit 3"
(watermark-async staging + folio-bvec grouping) carried over to ztmpfs
now that the zcell device is gone.  Answer: those were the **zcell swap
device's** merge bet -- the multi-page bvec is the *swap layer's*
large-folio signal for grouping anon pages by memory adjacency
(logs 54-55).  ztmpfs already merges its own pages by **file index**
(`merge_pages`, the `shmem_comp_compress_chunk` group path), a different
and already-built mechanism.  So the bvec grouping has no carrier in
ztmpfs; the spec06 watermark, however, is live in ztmpfs.

### 1. "Can ztmpfs still swap out its pages, compressed, no decompress on pageout?"

Grounded in `mm/shmem_compress.c`.  The answer is **yes, already, by
design -- but not via a swap device.**  The model (confirmed in the
decompress path's comments) is:

- Compression **is** the pageout.  On reclaim the folio is compressed
  into a cell and leaves `i_pages` (`shmem_comp_compress_one` /
  `_compress_chunk`, both compress-only).  No decompression on any
  pageout path.
- The compressed entry (`entries[index]` -> `shmem_comp_entry`, backed
  by a cell) **is the durable backing -- "like a disk block."**
- The decompressed page you read back is installed **non-swapbacked**
  (`__folio_clear_swapbacked`) on the *file* LRU: a reclaimable cache of
  the entry, dropped by `__remove_mapping` (so a >RAM read-back
  survives).  `shmem_writeout()` never fires on it.

So ztmpfs already evicts compressed with no decompress-on-pageout.  What
it does **not** do is reach disk: the cell is **kernel memory** (buddy
pages), which the kernel never swaps.

### 2. "Can't the compressed blob pages be swapped in/out transparently by the kernel?"

**No.**  Cells are buddy-allocated kernel pages.  The kernel's swap
machinery only unmaps process page-table entries for anon/file LRU
pages; kernel memory has no such mapping, so `try_to_unmap` never sees
it.  This is a hard boundary, not a tunable.  Hence "transparent kernel
paging of the blobs" is impossible; if compressed data is to reach disk,
**the engine must move it there itself.**

### 3. "Can the cells be allocated in swap-backed memory?" (so the kernel pages them)

**Functionally yes, architecturally disqualified.**  Storing blobs as
pages of a hidden shmem file would let the kernel page them to swap
transparently (a shmem page holds arbitrary bytes and is written raw to
a slot on reclaim).  But it costs two things:

- **It forfeits the cell allocator.**  Cells are contiguous order-N
  blocks with whole-cell return, zero per-object metadata, and a
  merge-group-in-one-region.  Swappable shmem pages are per-page
  reclaimable; none of that survives.
- **Reclaim reentrancy.**  The engine compresses from the reclaim/aging
  path.  If its own storage is swappable, the reclaim path can recurse
  into swapping the engine's own data -- the classic deadlock that
  `zswap`/`zsmalloc` avoid precisely by keeping the compressed pool in
  unreclaimable kernel memory.

### 4. "Can we back it from a swap device/file?  At the end I need a way to page on disk."

**Yes -- this is the design.**  The swap subsystem backs a bdev *or* a
file interchangeably (`swap_info_struct`; `swap_writepage` /
`read_swap_cache` are identical for both), so whatever the box has
`swapon`'d is a valid spill target.  The engine reuses it zswap-style:

- **Eviction (cold blob -> disk):** the aging pass picks a cold
  compressed entry, allocates a swap slot, writes the **compressed
  blob** into the slot (plus a small header), frees the cell, and
  records the swap offset in the entry (third entry state:
  swap-backed).
- **Swapin (read):** on a fault/read whose `entries[index]` is
  swap-backed, read the compressed blob from the slot, decompress into
  the page cache (optionally re-cache the blob in a cell -- promotion).
- **Context:** the spill does disk I/O, so it runs from the sleepable
  aging workqueue (`shmem_comp_age_work`), not inside reclaim's
  `shrink_folio_list`.  The cadence tunables (`age_delay_ms`,
  `age_budget`, `age_rounds`, `age_batch`) are already the "unread for a
  while" knobs.

The cold-detection machinery **already exists** (the aging pass:
`shmem_comp_maintain` / `age_round` / `note_unmap` / `wm_update`).
Today it moves uncompressed -> compressed.  The spill adds a second,
slower tier: compressed -> swap.

## The three-tier model (the session's mental frame)

| tier | lives where | evicted how | reaches disk? |
|------|-------------|-------------|---------------|
| uncompressed page | page cache | -> compressed into a cell | no |
| compressed blob | cell (kernel RAM) | -> **spill to a swap slot** | **yes (tier 3)** |
| compressed blob | swap slot (disk) | -> swapin + decompress | is the disk |

## What We Learned

### Technical

- **Kernel memory is never swapped.**  Swap unmaps process PTEs; a
  buddy/kmalloc-backed cell has no mapping, so no transparent paging of
  the compressed store is possible.  Any disk spill is engine-managed.
- **ztmpfs already satisfies "compress on pageout, never decompress."**
  Compression is the eviction; the decompressed page is a non-swapbacked
  file cache of the durable compressed entry.  Nothing was lost when the
  zcell *device* was removed -- that was a separate block/swap device,
  not the tmpfs compression path.
- **Reclaimable != swappable.**  File pages are dropped (and re-read
  from a disk source of truth); anon/shmem pages are written to swap.
  To make a blob's page *droppable* you still need it on disk first --
  so the spill is unavoidable either way.
- **Why the compressed store must stay unreclaimable:** zswap/zsmalloc
  keep their pools in kernel memory for exactly the reentrancy reason;
  backing it with swappable pages reintroduces the hazard the design
  exists to avoid.

### Process

- The disk-spill requirement was approached by eliminating the tempting
  "let the kernel do it transparently" options one at a time (transparent
  kernel swap -> impossible; swappable cells -> disqualified), which
  isolates the engine-managed spill as the only design that keeps the
  cell architecture.  Naming *why each cheaper option fails* is what
  makes the chosen one obviously correct.

## What We Decided

- **Tier-3 design: engine-managed spill of cold compressed blobs
  through the swap subsystem (the zswap model).**  Reuses `swapon`'d
  devices and files; keeps the cell allocator for the hot tier; runs
  from the sleepable aging workqueue.
- **Swappable (shmem-backed) cells: rejected** -- reclaim reentrancy +
  cell-allocator forfeiture.
- **Kernel-transparent swap of cell pages: impossible** (kernel memory
  is unswappable).
- **Pageout stays compress-only**; the spill writes the compressed blob,
  never a decompressed page.

## Open design choice (not settled this session)

- **Per-slot vs packed on-disk format.**
  - *Reuse swap slots:* one blob per PAGE_SIZE slot.  Simple, reuses
    the whole swap subsystem, works for device + file.  Cost: a sub-page
    blob occupies a full slot, so disk footprint per spilled page is
    1 slot -- compression's win is RAM, not disk capacity.
  - *Packed custom format:* pack blobs into disk blocks -> more logical
    pages per GB of swap.  Cost: manage our own disk-space allocation
    (a file/partition with a custom allocator) -- essentially rebuilding
    what the zcell device was.  The ghost to avoid unless capacity, not
    just RAM, is the goal.
  - Leaning **per-slot reuse** for a first implementation.

## Rules Added

- **The compressed store must live in unreclaimable kernel memory**;
  any disk spill is an explicit engine path through the swap subsystem,
  never a swappable backing (reclaim reentrancy).
- **To page tmpfs data to disk compressed, extend shmem's manual swap
  management (swap slots) with a compressed payload** -- do not expect
  the kernel to swap kernel memory, and do not decompress on pageout.

## What We'd Do Differently

- Would have jumped straight to "the compressed cell is kernel memory,
  so the kernel can't page it" in turn 2 instead of first re-deriving
  the bvec/watermark relevance -- the unswappable-kernel-memory fact
  settles most of the space at once.

## Files Changed

| File | Purpose |
|------|---------|
| (none) | design session -- no kernel code changed |

## Test Results / Benchmarks

None -- design only.  No gates re-run.

## Next Steps

1. **Decide per-slot vs packed** on-disk format (see open design
   choice).  Per-slot reuse is the pragmatic default.
2. **Spec the tier-3 spill** if it is to be built: swap-slot alloc +
   compressed write from the aging workqueue, a swap-backed entry state,
   a swapin+decompress path (cannot reuse `shmem_swapin_folio`, which
   expects an uncompressed page), and optional re-cache-on-swapin.
3. The gate that decides *when* to build it: the compressed footprint
   of the real working set vs RAM (the compressible-non-zero workload
   the logs keep flagging as unmeasured).  The user has stated disk
   paging is a hard requirement ("at the end I need a way to page on
   disk"), so this is a committed feature, not a measurement-gated
   maybe -- but the measurement still sizes the per-slot disk need.

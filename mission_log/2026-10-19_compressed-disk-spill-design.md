---
name: compressed-disk-spill-design
description: design the disk-spill tier for ztmpfs -- swap the WHOLE cell (raw, already-compressed bytes) to a swap device/file; a cell is in RAM or on disk, metadata stays in RAM; rejects per-blob zswap spill, swappable cells, and kernel-transparent paging
project: linux
---

# Mission Log: Paging Cells to Disk (the Tier-3 Swap Design)

Part 60 of the tmpfs transparent-compression series.  A design session
(2026-10-19), bootstrapping from the record (logs 1-59) then converging
on how ztmpfs pages its compressed backing out to disk.  No kernel code
changed; this log captures the mapped design space, the rejected
options, and the chosen whole-cell swap mechanism so a future session
does not re-derive it.

## The requirement (user)

ztmpfs must page its compressed backing to disk: a cell that has been
cold for a while should not be pinned in RAM.  Constraints stated and
re-stated across the session: page out COMPRESSED (never decompress on
pageout); the paging must be transparent; and finally -- **swap the
whole cell, not the content, and do not compress/decompress in the swap
path** because the cell already holds compressed blobs.

## What We Did

The session was a sequence of architecture questions, each answered from
the code and the earlier logs, converging on the mechanism.

### 0. Entry point: are watermark-async + bvec grouping relevant to ztmpfs?

The deferred "commit 3" (watermark-async staging + folio-bvec grouping)
was the **zcell swap device's** merge bet -- the multi-page bvec is the
*swap layer's* large-folio signal for grouping anon pages by memory
adjacency (logs 54-55).  ztmpfs already merges its own pages by **file
index** (`merge_pages`, `shmem_comp_compress_chunk`), a different,
already-built mechanism.  So bvec grouping has no carrier in ztmpfs;
the spec06 watermark, however, is live in ztmpfs.

### 1. "Can ztmpfs still swap out its pages, compressed, no decompress on pageout?"

**Yes, already -- but not via a swap device.**  In `mm/shmem_compress.c`
compression **is** the pageout: on reclaim the folio is compressed into
a cell and leaves `i_pages` (`shmem_comp_compress_one`/`_compress_chunk`,
both compress-only).  The compressed entry (`entries[index]` ->
`shmem_comp_entry`, backed by a cell) **is the durable backing -- "like a
disk block."**  The decompressed page you read back is installed
**non-swapbacked** on the *file* LRU: a reclaimable cache of the entry,
dropped by `__remove_mapping` (so a >RAM read-back survives);
`shmem_writeout()` never fires on it.  What is missing is any path to
disk -- the cell is **kernel memory**, which the kernel never swaps.

### 2. "Can't the compressed blob pages be swapped in/out transparently by the kernel?"

**No.**  Cells are buddy-allocated kernel pages.  The kernel's swap
machinery only unmaps process page-table entries for anon/file LRU
pages; kernel memory has no mapping, so `try_to_unmap` never sees it.
A hard boundary, not a tunable.  Any disk reach is engine-managed.

### 3. "Can the cells be allocated in swap-backed memory?" (so the kernel pages them)

**Functionally yes, architecturally disqualified.**  Storing blobs as
pages of a hidden shmem file would let the kernel page them transparently
(a shmem page holds arbitrary bytes, written raw to a slot on reclaim).
But it **forfeits the cell allocator** (contiguous order-N blocks,
whole-cell return, zero per-object metadata, merge-group-in-one-region --
all lost to per-page reclaimable pages) and reintroduces **reclaim
reentrancy**: the engine compresses from the reclaim/aging path, so if
its own storage is swappable, reclaim recurses into swapping the engine's
own data -- the classic deadlock `zswap`/`zsmalloc` avoid by keeping the
compressed pool in unreclaimable kernel memory.

### 4. "Can we back it from a swap device/file?  At the end I need a way to page on disk."

**Yes.**  The swap subsystem backs a bdev *or* a file interchangeably
(`swap_info_struct`; `swap_writepage`/`read_swap_cache` identical for
both), so any `swapon`'d target is valid.  The first cut was framed as a
zswap-style spill of **individual compressed blobs** into swap slots,
with a decompress on swapin.  **The user corrected this** -- see the
decision below: swap the **whole cell**, raw, not the per-blob content.

## The chosen model: whole-cell swap, raw

- **Swap unit = the whole cell, moved as raw bytes.**  A cell already
  holds compressed blobs, so the swap path does **no compression or
  decompression** -- it is a straight copy of an order-N contiguous
  region to/from disk.  The swap tier is content-agnostic.
- **A cell is wholly in RAM or wholly on disk, never both.**  Only the
  cell *data* pages out; the cell **directory (metadata) stays in RAM
  always**.
- **Access to a block in an on-disk cell swaps the whole cell in
  first**, then serves from RAM (read amplification by design).
- **A cell is write-once at block granularity** -- only free blocks get
  allocated, never in-place modify.  So an on-disk cell is immutable
  until it is brought back to RAM.
- **Evacuation invalidates the swap** -- when GC relocates a sparse
  cell's live blocks and frees the cell, its disk copy is discarded
  (`swapfree`), so no stale disk copy survives.

### Why it beats the per-blob (zswap) spill

No per-blob swap-slot bookkeeping; no decompress-on-swapin (the swap
moves raw bytes; the only decompression is the normal read path, blob ->
page, which is unchanged); and the cell's physical contiguity makes the
disk I/O a simple contiguous copy.

### The three-tier frame

| tier | lives where | evicted how | reaches disk? |
|------|-------------|-------------|---------------|
| uncompressed page | page cache | -> compressed into a cell | no |
| compressed blobs | cell (kernel RAM) | -> **whole cell swapped out (raw)** | **yes (tier 3)** |
| whole cell | swap extent (disk) | -> whole cell swapped in, then read | is the disk |

## Decisions the spec must nail

1. **Scatter vs contiguous on disk.**  An order-N cell is 2^N pages.
   Scatter/gather across 2^N PAGE_SIZE swap slots (the directory records
   the slot list per on-disk cell) is the pragmatic reuse of the swap
   allocator; on-disk contiguity is a perf optimization, not correctness.
   A contiguous large slot would need THP-large-swap, which is optional.
2. **Write-after-swapin.**  Writing a free block requires the cell in
   RAM and makes the RAM copy authoritative (the disk copy is stale) --
   a dirty-bit at cell granularity, re-written on the next eviction.
3. **GC scope.**  Run GC on in-RAM cells; an on-disk cell is only GC'd
   after being swapped in, then evacuated with its swap discarded.  Cold
   disk cells stay immutable.
4. **Sizing.**  Swap must cover 2^N slots per cell; the directory's
   per-cell RAM cost is small versus the data it lets leave RAM.

## What We Learned

### Technical

- **Kernel memory is never swapped** -- swap unmaps process PTEs; a
  buddy/kmalloc-backed cell has no mapping.  Any disk spill is
  engine-managed, never transparent kernel paging.
- **ztmpfs already satisfies "compress on pageout, never decompress."**
  The zcell *device* removal did not touch this -- that was a separate
  block/swap device, not the tmpfs compression path.
- **The swap unit should follow the allocator's unit.**  Because cells
  are already the allocation/RAM unit (contiguous order-N, write-once),
  swapping the whole cell reuses that invariant instead of cutting
  across it -- which is why whole-cell raw swap is simpler than per-blob
  spill.  The compression happened once, at store time; the swap tier
  need not know about it.
- **Reclaimable != swappable**, and the compressed store must stay
  unreclaimable (reentrancy) -- so it is the engine, from a sleepable
  context, that moves cells to disk.

### Process

- The requirement was met by eliminating the tempting "let the kernel do
  it" options one at a time (transparent kernel swap -> impossible;
  swappable cells -> disqualified), which isolated the engine-managed
  path; then the user simplified the payload from per-blob to
  whole-cell, which removed the decompress-on-swapin and the per-blob
  bookkeeping.  Naming *why each cheaper option fails* is what makes the
  chosen one obviously correct.

## What We Decided

- **Tier-3: whole-cell swap through the swap subsystem (device or
  file).**  Raw move of the order-N cell; no compress/decompress in the
  swap path; the cell is in RAM or on disk; metadata always in RAM;
  swap-in-whole-cell on access; write-once blocks; discard swap on
  evacuation.
- **Per-slot alignment waste accepted** (user: *disk is cheaper than
  RAM*) -- the compression win is RAM; a sub-page/partial cell on disk
  is a fine trade.
- **Swappable (shmem-backed) cells: rejected** -- reclaim reentrancy +
  cell-allocator forfeiture.
- **Kernel-transparent swap of cell pages: impossible** (kernel memory
  is unswappable).
- **Pageout stays compress-only** for the uncompressed->compressed
  transition; the disk swap moves raw cell bytes.

## Rules Added

- **The compressed store must live in unreclaimable kernel memory**; any
  disk reach is an explicit engine path, never a swappable backing
  (reclaim reentrancy).
- **Swap at the allocator's granularity, raw** -- a cell already holds
  compressed blobs, so paging a cell needs no (de)compression; do not
  decompress on pageout.
- **A paged-out cell is immutable until swapped in; freeing/evacuating
  it discards its swap** -- never leave a stale disk copy.

## What We'd Do Differently

- Would have jumped straight to "the cell is kernel memory, so the
  kernel can't page it" and "swap at the cell unit, raw" instead of
  first framing a per-blob zswap spill -- the unswappable-kernel-memory
  fact plus the write-once-cell invariant settle the design at once.

## Files Changed

| File | Purpose |
|------|---------|
| (none) | design session -- no kernel code changed |

## Test Results / Benchmarks

None -- design only.  No gates re-run.

## Next Steps

1. **Spec the whole-cell swap**: swap-slot allocation for a 2^N-page
   cell (scatter/gather), the on-disk/in-RAM cell states in the
   directory, whole-cell swap-out from the sleepable aging workqueue,
   whole-cell swap-in on access, the cell dirty-bit for
   write-after-swapin, and GC's evacuate-then-discard-swap.
2. The measurement that *sizes* the disk need (not whether to build --
   disk paging is a committed requirement): the compressed footprint of
   the real working set vs RAM (the compressible-non-zero workload the
   logs keep flagging as unmeasured).
3. Confirm the read-amplification trade is acceptable on the target
   workload (one-page read pulls a whole cell, 2^N pages) -- mitigated
   because a cell backs multiple pages and disk is cheap.

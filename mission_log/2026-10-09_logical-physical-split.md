---
name: logical-physical-split
description: FTL-style logical/physical block split for cellzram -- RMW as a driver obligation, the validity bitmap placed post-decompress, built in 5 bisectable steps (session 33, in progress)
project: linux
previous: 2026-10-09_mboot-bench-zram-axis.md
---

# Mission Log: The Logical/Physical Split

Part 17 of the tmpfs transparent-compression series.  Session 33
(2026-10-09), **in progress** -- this log is appended as each step
lands, per the bisectable-build discipline this session is run under.

The goal: present a 4 KiB logical block with a 64 KiB physical block,
compressing one blob per physical unit, so the entry table drops ~16x.
That table (72 B per 4 KiB block = 1.125 MiB on 64 MiB) is the *entire*
incompressible RAM gap session 32 identified between cellzram and
zram.  This is the lever that closes it.

Kernel commits: `8e642a58d87b` (step 1).  Rig: unchanged so far.

## The design, as settled this session

### Correctness is the driver's obligation, not the filesystem's

The user framed this correctly and it drove the whole design: **RMW is
for correctness.**  Whether a filesystem *uses* the advertised
physical size to align its allocations is a separate, purely
performance question.  On a real 512e drive (512 logical / 4K
physical) the drive does RMW internally on every 512-byte write and
the filesystem never has to know.

Verified in the block layer:

- **Nothing enforces writes >= physical_block_size.**  Only
  `logical_block_size` is enforced (`blk_validate_limits`).  A 4 KiB
  write to a physical=64K device goes straight through to the driver.
- **The block layer does not RMW.**  No read-modify-write anywhere in
  `block/*.c`.  So the driver owes it, exactly as with NAND/FTL.
- `discard_granularity` is raised to
  `max(granularity, physical_block_size)` by `blk_validate_limits`, so
  **DISCARD arrives whole-unit aligned**.  The partial-unit case only
  arises from punch-hole, which reaches cellzram as `WRITE_ZEROES` --
  and that path is *not* aligned to physical.  So the bitmap's
  partial-clear path is needed only for punch-hole.
- `physical_block_size` has no block-layer upper bound (only
  "power of 2, >= logical").  The real ceiling is that a compression
  unit is stored as ONE cell region, so `physical <= cell_size`.

Correction recorded: earlier in the session I argued that advertising
physical=64K "buys nothing because ext4 doesn't read it."  That
conflated the correctness contract with the optimization.  ext4 indeed
never reads `bdev_physical_block_size` (verified: no reference in
`fs/ext4/`); it reads its own `s_raid_stripe_width` superblock field or
the `stripe=` mount option.  But that is irrelevant to the contract.
The user was right and I was wrong to wave it away.

### The validity bitmap: post-decompress, not pre-compress

The user specified the mechanism: a bitmap of which logical sectors in
a unit are live.  Write flips a bit to 1, fills the data, recompresses
the whole unit.  Discard flips a bit to 0 and **does not recompress**;
on read, bit=0 maps to zeros.  Discarded sectors' bytes stay in the
compressed blob until GC, so used sectors can still share compression
context with discarded ones.

I asked and answered where the bitmap must live: **post-decompress --
i.e. in the entry as uncompressed metadata, NOT inside the compressed
blob.**  The reasoning, which is not a close call:

- If the bitmap were pre-compress (inside the blob), clearing one bit
  would require decompress -> patch bitmap -> recompress.  That
  rebuilds RMW on the *discard* path, destroying the entire point of
  the design.  Discard must be a bit-flip with no recompress.
- A blob-internal bitmap makes the validity mask depend on the blob's
  own integrity: a torn/corrupt blob corrupts the mask too, so you
  cannot distinguish "these sectors are dead" from "these bytes are
  garbage."  Post-decompress, the bitmap is authoritative independent
  state, consistent with `length`/`nr_pages` already living in the
  entry as truth about the blob.

Bitmap sizing: `physical/logical` bits = 16 for 64K/4K.  Bounded by
`cell_size/logical` because physical <= cell_size.  `DECLARE_BITMAP`.

### The five steps, bisectable by construction

The user's discipline: step by step, always correct, measuring every
step, bisectable.  Each step ends green at default with identical
behaviour, so a regression bisects to the step that introduced it.

| # | step | default behaviour |
|---|------|-------------------|
| 1 | logical/physical params + queue_limits + validation | **no-op** (ratio 1) |
| 2 | unit abstraction: `unit_nr = block_nr / ratio`, offset | identity at ratio 1 |
| 3 | per-entry valid bitmap | 1 bit, always set |
| 4 | enable ratio > 1: write-RMW, read mask, discard bit-flip | only when configured |
| 5 | measure: entry table, RAM, untar vs churn | n/a |

Also fixed early: `logical` and `physical` are **device-creation-time**
options for the block device (module params, powers of two) --
mount-time for tmpfs.  The two sizes are implemented via the existing
**merge** machinery (`nr_pages`), which tmpfs already uses and cellzram
currently hardcodes to 1.

## Step 1: the parameter surface (commit 8e642a58d87b)

### What We Did

1. **Added `logical_block_size` and `physical_block_size`** module
   params, both powers of two, both defaulting to `PAGE_SIZE`.
2. **Validated them** in `cellzram_init`: logical in
   512..`BLK_MAX_BLOCK_SIZE` via `blk_validate_block_size`; physical a
   power of two >= logical; physical <= `PAGE_SIZE << cell_order` (the
   cell-size bound, which the block layer does not impose).
3. **Derived `cellzram_unit_ratio`** = physical/logical, logged at
   init.  1 at default = split disabled.
4. **Wired `queue_limits` from the params.**  All four fields
   (`logical_block_size`, `physical_block_size`, `io_min`, `io_opt`)
   land on `PAGE_SIZE` at default, so the limits are byte-identical to
   before the change.
5. **Set `discard_granularity = physical_block_size`.**  The block
   layer raises it to physical anyway, but stating it keeps the intent
   visible: discards arrive whole-unit aligned, WRITE_ZEROES does not.

### What We Learned

- **`BLK_MAX_BLOCK_SIZE` is `SZ_64K` iff `CONFIG_TRANSPARENT_HUGEPAGE`,
  else `PAGE_SIZE`.**  THP is off in the rig config, so logical is
  capped at 4 KiB here -- which is exactly what this design wants, but
  it would silently cap a future 64K-logical experiment.  Worth
  remembering when that axis is swept.
- **`SZ_64K` is `1UL << 16`, i.e. `unsigned long`.**  Two `%d`
  format warnings from printing it; fixed to `%lu` with a cast.  Minor,
  but it is the kind of thing that makes a "clean" step not clean.
- **A dropped `return -EINVAL` in an edit.**  The first pass removed
  the early-return when replacing a block, which would have turned a
  validation failure into fall-through.  Caught by re-reading the
  diff, not by the compiler -- the compiler cannot know the return was
  meant to be there.

### Test Results

Three gates re-run at default, all green, limits byte-identical:

| gate | result | baseline |
|------|--------|----------|
| cellzram | 106/0 | 106/0 |
| fsdev (blkdev ext4) | 19/0 | 19/0 |
| churn | 2/0 | 2/0 |

churn avg 124.3 MB/s (cellzram) vs 125.0 baseline -- within the
in-boot noise, exactly as expected for a change that touches no code
path.  Init log confirms the no-op:

```
cellzram: logical=4096 physical=4096 unit_ratio=1 (split disabled)
```

### What We Decided

- **RMW is the driver's obligation, not the fs's.**  The block layer
  enforces only logical on writes and does no RMW itself, so when
  physical > logical the read-modify-write is cellzram's job -- the
  FTL model, correctness first, optimize being-RAM-backed afterwards.
- **The validity bitmap is post-decompress, in the entry.**  Not
  inside the compressed blob: a blob-internal bitmap would require a
  full decompress-recompress to clear one bit on discard, rebuilding
  RMW on the exact path the design exists to make free.  It also keeps
  the mask authoritative when the blob is torn.
- **Discard is a bit-flip with no recompress.**  Stale bytes for
  discarded sectors stay in the blob until GC, so used sectors can
  still share compression context with discarded ones.
- **`physical <= cell_size` is the real ceiling** -- the block layer
  sets no upper bound on physical, but a unit is stored as one cell
  region.  This bound also caps the bitmap at cell_size/logical bits.
- **Step 1 lands as a verified no-op.**  Params and validation only;
  queue_limits byte-identical at default, all gates green.  Every
  later step bisects against this baseline.

### What We'd Do Differently

- Would not have argued that advertising physical=64K "buys nothing
  because ext4 doesn't read it."  That conflated the correctness
  contract with the optimization, and the user had to correct it.  A
  device contract is not contingent on one filesystem's tuning.
- Would have checked the block layer's write-size enforcement *before*
  proposing stripe= mount options as the mechanism.  The enforcement
  gap is the whole design; the mount option is a later optimization.
- Would have read the diff for the dropped `return -EINVAL` rather
  than trusting the edit.  The compiler cannot know a return was meant
  to be there.

## Step 2: the unit rekey (commit 081be4e7df8f)

### What We Did

1. **Rekeyed the block table by unit index.**
   `cellzram_unit_of(block_nr) = block_nr / unit_ratio`.  At ratio 1
   that is the identity -- `block_nr / 1 == block_nr` -- so this is
   byte-identical at default, which is what makes it bisectable.
2. **Applied it at all four boundaries** where a block number enters:
   `bio_read`, `bio_write`, `bio_discard`, `slot_free_notify`.  The
   read/write helpers now take and store a `unit_nr`, named as such,
   so the rekey is honest at every level rather than a block number
   that quietly means a unit.
3. **Rejected ratio > 1 until step 4.**  Once the key is a unit index,
   a write to logical block N lands in unit N/ratio and clobbers its
   siblings, because the RMW that makes a partial write safe does not
   exist yet.  Rejecting keeps every commit correct on its own rather
   than correct-only-at-default.

### What We Learned

#### Technical

- **The discard loop rewrite underflowed, and the gates would not have
  caught it.**  My first version used a unit-range loop
  (`unit <= unit_of(end_block - 1)`).  Correct for aligned discards,
  but on a zero-length bio at sector 0, `end_block - 1` wraps to
  `ULONG_MAX` and the loop sweeps the entire table.  No test in the
  suite issues a zero-length discard, so all three gates would have
  gone green over it.  The original `< end_block` guard was naturally
  safe against empty; the rewrite lost that property.  Fixed by
  mapping each block to its unit inside the original loop -- identity
  at ratio 1, safe against empty, idempotent at ratio > 1.

- **A rekey that is the identity at default still needs the ratio>1
  path rejected.**  "It behaves the same at default" is not the same
  as "it is correct."  The rekey made ratio>1 actively dangerous in a
  way it had not been after step 1 (where the table was still keyed by
  block number and ratio>1 merely advertised a size it did not use).
  Each step changes which configurations are safe, and that has to be
  re-derived, not assumed.

#### Process

- **Read the loop bound, not just the loop body.**  The bug was in the
  termination condition, introduced by rewriting a correct loop into a
  cleverer one.  The cleverer version was shorter and wrong.

### Test Results

Three gates re-run at default, all green, identical to step 1:

| gate | result | step 1 |
|------|--------|--------|
| cellzram | 106/0 | 106/0 |
| fsdev (blkdev ext4) | 19/0 | 19/0 |
| churn | 2/0 | 2/0 |

churn avg 124.3 MB/s vs step 1's 124.3 -- unchanged, as required for
a rekey that is the identity at ratio 1.

## Step 3: the validity bitmap, zero-cost at ratio 1 (commit 98bc5eeaa7f7)

### What We Did

1. **Added a trailing flexible array `valid[]` to `shmem_comp_entry`**,
   sized `BITS_TO_LONGS(unit_ratio)` words when the split is on and
   **zero words when it is off.**  At ratio 1 the entry's *existence*
   is already the bitmap, so `struct_size(ent, valid, 0) ==
   sizeof(*ent)` and the default allocation is byte-identical to
   before the field existed.
2. **Verified the zero-cost claim numerically** rather than asserting
   it: a standalone struct with the same layout prints
   `sizeof=40  struct_size(valid,0)=40  struct_size(valid,1)=48`.
   40 == 40 at ratio 1, +8 bytes at 16 sectors.  An entry-table
   shrink must not begin by growing the entry.
3. **Landed the bitmap in the entry, uncompressed** -- the decision
   from the design section, now in code with the reasoning in the
   struct comment.
4. **Added `pool->unit_ratio`** with `shmem_comp_pool_set_unit_ratio()`
   and `shmem_comp_pool_unit_ratio()`.  cellzram sets it immediately
   after `pool_create`, **before the first entry alloc**, because the
   ratio sizes every entry and is not retroactive.  NULL/0 pools read
   as 1 so callers can divide by it unconditionally.
5. **Did not wire the bitmap into any path.**  Read, write and discard
   still ignore `valid[]`.  That is deliberate: it is what keeps this
   step a no-op at *every* ratio, not just the default.

### What We Learned

#### Technical

- **`ksize()`-based RAM accounting survives a flexible array
  automatically.**  `shmem_comp_ram_entry_bytes` uses `ksize(ent)`,
  which returns the real allocation including bitmap words.  No change
  was needed and none was made -- the accounting got *more* accurate
  for free.  Worth checking rather than assuming a field addition
  breaks derived accounting; here it did not.

- **`kfree_rcu(ent, rcu)` is unaffected by a trailing flexible
  array.**  It resolves `rcu` via `offsetof` at compile time; the
  array lives after it and does not move it.  Checked because getting
  this wrong is a silent free-of-the-wrong-address, not a crash with
  a useful trace.

- **`struct_size()` needs `<linux/overflow.h>` and `bitmap_fill()` /
  `BITS_TO_LONGS` need `<linux/bitmap.h>`,** and `shmem_compress.c`
  included neither.  Added both.  Trivial, but it is the kind of
  thing that only surfaces at the first compile of the step that
  needs it.

#### Process

- **Verify a "zero-cost" claim with a number, not a sentence.**  The
  flexible-array trick is *usually* zero-cost at count 0, but "usually"
  is how a 40-byte entry quietly becomes 48.  One standalone compile
  settled it in fifteen seconds and turned an assumption into a
  measured fact -- the same discipline this session applies to the
  kernel, pointed at the host toolchain.

### Test Results

Three gates re-run at default, all green, identical to steps 1-2:

| gate | result | step 2 |
|------|--------|--------|
| cellzram | 106/0 | 106/0 |
| fsdev (blkdev ext4) | 19/0 | 19/0 |
| churn | 2/0 | 2/0 |

churn avg 123.8 MB/s vs step 2's 124.3 -- within in-boot noise, as
required for a step that allocates byte-identical entries.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | `logical_block_size` / `physical_block_size` params, power-of-2 + cell-size validation, `cellzram_unit_ratio`, queue_limits derived from params (step 1) |
| `mm/cellzram.c` | `cellzram_unit_of()`, block table rekeyed by unit index at all four bio boundaries, read/write helpers take `unit_nr`, ratio>1 rejected until step 4 (step 2) |
| `mm/cellzram.c` | sets pool unit ratio after `pool_create`, before first entry alloc (step 3) |
| `include/linux/shmem_compress.h` | `valid[]` flexible array on `shmem_comp_entry` with the post-decompress rationale; `unit_ratio` on the pool; ratio setter/getter decls (step 3) |
| `mm/shmem_compress.c` | `<overflow.h>` + `<bitmap.h>`; bitmap sized at alloc (0 words at ratio 1), `bitmap_fill` on fresh units; ratio setter/getter (step 3) |

### Next Steps

Superseded -- see "The pivot" below.  Steps 4-5 of the split were
never completed because the measurement that should have preceded them
showed the split's payoff does not exist on a realistic workload.  The
steps are left in this log as written because the reasoning that led
them astray is the useful part.

## The pivot: are we chasing ghosts? (commit bbea20a5e864)

Mid-step-4 the user stopped the work and asked two questions that
reframed the whole session: *what is the goal of the journey, and do
we go there or chase ghosts?* and *rethink from first principles*
-- then, crucially, **"tmpfs merge is akin to bigger physical size."**

That last observation is exact, not an analogy.  tmpfs `merge_pages=16`
and cellzram `physical=64K` produce the **same storage layout**: one
compressed blob spanning N consecutive 4K pages, one cell region, one
entry, `nr_pages=N`.  The tmpfs path already prefers cells for it --
the code comment says cells do it better than zsmalloc because "the
entire group fits in one cell region and avoids the per-chunk handle
array."

### Why tmpfs gets merge free and cellzram would not

Tracing `shmem_decompress_group` to its only caller
(`shmem_decompress_folio`, a READ path) exposed the asymmetry that
matters: tmpfs decompresses a group **into the page cache** and all
writes land there, uncompressed.  Compression in tmpfs is a
*reclaim-time* activity.  cellzram is a block device with no page
cache, so every 4K write would hit the compressed blob directly --
decompress 64K, patch, **recompress 64K, synchronously**, ~20x the
CPU per write versus today.

So the split was porting piece 1 of tmpfs's model (one blob per N
pages) without piece 2 (a buffer that absorbs writes uncompressed).
That is what the proposed write-gathering buffer would have supplied,
and it is the "leverage being RAM-backed" round the user named.

### The measurement that settled it

Before building either, the untar workload -- compressible,
sequential-append, the shape the cell scheme is *for* -- was read from
the logs.  It had already run in this session's gates and never been
examined.  Session 32's verdict rested on two gaps with different
causes, and I had spent the session fixing the one that only exists in
a synthetic test:

| workload | gap vs zram | cause | does the split fix it |
|---|---|---|---|
| `distinct` (64 MiB incompressible) | 1.125 MiB | entry table (16384 x 72 B) | **yes, entirely** |
| `untar` (8 MiB compressible) | 176 KiB | **cell granularity floor** | **936 bytes of it** |

untar's mm_stat: `orig=8441856 compr=870 phys=262144 objects=13
entry_bytes=936`.  The entry table is **936 bytes** -- the whole RAM
footprint the split was built to shrink.  The real gap is that 870
bytes of compressed data forces a whole 256 KiB cell, where zsmalloc's
size classes need only 80 KiB.  Finishing step 4 would have cost ~200
lines of correctness-critical RMW to save 936 bytes.

The 256 KiB floor is `cell_order`, and it is the axis sessions 26/28/29
all flagged as unswept.  The direction was also wrong: bigger cells
worsen both the floor *and* fragmentation (a cell returns to the buddy
only when it completely empties, so a bigger cell is harder to empty).

### What was also learned about the workloads

untar's `zero_blocks=2048` of 2061 total blocks means the dataset is
**almost entirely zeros** -- inode tables, bitmaps, padding.  Its
870-byte `compr_bytes` is not a compression win; it is session 31's
zero-block sentinel doing its job.  So untar measures zero-handling and
cell granularity, **not** compression on real data.  `distinct` is
incompressible, untar is zeros: **no workload in the rig measures
compression ratio on genuinely compressible non-zero data.**  That is
now the largest unmeasured question in the series.

## The cell_order sweep (order 2 / 4 / 6, both workloads)

Ran `TEST_CELLORDER=2/4/6` across untar and churn, churn twice to check
reproducibility.  The axis already exists in the rig
(`cellzram.cell_order`, module param, range 2..10, default 6).

**untar -- order 2 wins decisively:**

| order | cell | RAM held | untar MB/s | verify | rm |
|---|---|---:|---:|---:|---:|
| 2 | 16 KiB | **16 KiB** | 20.3 | **79.5** | **60.8** |
| 4 | 64 KiB | 64 KiB | 20.2 | 77.4 | 55.8 |
| 6 | 256 KiB | 256 KiB | 20.6 | 76.1 | 55.5 |
| *zram* | -- | *80 KiB* | *20.4* | *75.5* | *64.4* |

The floor is **exactly one cell** at every order, which confirms the
mechanism.  At order 2 that is **16 KiB, beating zram's 80 KiB by 5x**
-- where order 6 lost to zram by 3.2x.  Throughput is flat-to-better:
`rm` improves 9.5% (smaller cells return to the buddy sooner on
delete), verify +4.5%.

**churn -- order 2 nearly closes the throughput gap but costs RAM:**

| order | cell | avg MB/s | gc_recoveries | phys held |
|---|---|---:|---:|---:|
| 2 | 16 KiB | **133.6 / 131.9** | **0 / 0** | 16.6 MiB |
| 4 | 64 KiB | 109.1 / 108.2 | 148 / 148 | 17.3 MiB |
| 6 | 256 KiB | 125.3 / 125.2 | 58 / 58 | **10.25 MiB** |
| *zram* | -- | *136.3* | -- | *8.0 MiB* |

Both integer columns reproduce **byte-identically** across two
independent boots (gc = 0/148/58 twice), and throughput tracks gc
inversely.  Three findings:

1. **order 2 nearly matches zram on throughput** (2% gap, down from 8%
   at order 6) because `gc_recoveries = 0`: a 16 KiB cell holds 4
   blocks, so it empties naturally under random overwrite and returns
   inline.  The compaction pass -- session 25's accepted ~9% cost --
   never fires.  This is the mechanism predicted from first principles,
   now measured.
2. **But phys is worse at order 2** (16.6 vs 10.25 MiB).  With no GC
   there is no compaction, so fragmentation accumulates until the
   directory is nearly full.  GC at order 6 was doing real work:
   evacuating sparse cells and coalescing toward the live set.
3. **order 4 is a genuine local minimum** -- worse than both neighbors
   on throughput (109 MB/s) with the *most* GC activity (148).  It
   reproduces exactly, so it is not noise.  The likely mechanism:
   between the two regimes -- too big to empty naturally (so GC runs),
   too small for each pass to coalesce much (so it runs often).
   Untested hypothesis, but the gc column is unambiguous.

### What was also fixed

While verifying the device-size cost of small cells, found a **false
claim in a comment** (`bbea20a5e864`): the disksize sizing path said
the cell-count hint "is clamped to U16_MAX inside cell_pool_create".
There is no such clamp -- `cell_pool_create` takes the hint as given
and `kvcalloc()`s the directory array, and `cell_store.c` states
explicitly that no clamp is needed because the caller owns the
arithmetic.  The stale claim implied a safety bound that does not
exist.  The real ceiling is the directory array itself, scaling as
1/cell_size: order 2 needs 64x more slots than order 6 for the same
disksize, so a large device at a small cell_order can exhaust memory on
the directory before storing a byte.

### What We Decided (this section supersedes the split plan)

- **The split is parked, not finished.**  Steps 1-4a stay committed and
  dormant at ratio 1 (byte-identical, revertible).  Step 4 -- the ~200
  lines of RMW correctness core -- is not built, because its measured
  payoff on a realistic workload is 936 bytes.
- **`cell_order` is the axis that matters**, and the direction is
  *down*, not up.  It fixes the untar RAM floor (16 KiB beats zram's
  80 KiB) and churn throughput (gc goes to zero) at once.
- **The churn RAM/throughput trade is real and unresolved**: order 2
  wins throughput, order 6 wins RAM.  Which dominates depends on the
  deployment, and no default should change until that is chosen
  deliberately.
- **The compression-ratio question is open and now the priority.**  No
  rig workload measures compressible non-zero data.  Every conclusion
  about "where cellzram wins" rests on zero-heavy or incompressible
  synthetic data.

### Files Changed (this section)

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | corrected the false U16_MAX clamp claim; documented the real 1/cell_size directory cost |

### Next Steps (revised)

1. **Decide the cell_order default deliberately**, on the
   throughput-vs-RAM trade, not by inertia.  Order 2 is better on both
   measured workloads except churn RAM; that is a strong case, but it
   caps device size via the directory array and that limit needs a
   number before the default moves.
2. **Build the compressible-non-zero workload.**  This is the largest
   gap in the series: a real dataset (text, binaries, logs) written to
   the device, measuring compression ratio and RAM against zram.  Every
   "cellzram wins here" claim depends on it and none has been measured.
3. **Decide the split's fate** on the strength of (2).  If a
   compressible workload shows the entry table or the compression-ratio
   gain from 64K context matters, step 4 becomes worth building.  If
   not, revert steps 1-4a and record why.

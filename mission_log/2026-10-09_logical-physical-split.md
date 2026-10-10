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

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | `logical_block_size` / `physical_block_size` params, power-of-2 + cell-size validation, `cellzram_unit_ratio`, queue_limits derived from params (step 1) |
| `mm/cellzram.c` | `cellzram_unit_of()`, block table rekeyed by unit index at all four bio boundaries, read/write helpers take `unit_nr`, ratio>1 rejected until step 4 (step 2) |

### Next Steps

Within this session (steps 3-5 of the design above):

3. **Per-entry valid bitmap** -- `DECLARE_BITMAP`, sized
   `physical/logical`.  At ratio 1 it is one bit, always set.
4. **Enable ratio > 1** -- the correctness core: write-RMW
   (decompress whole unit, patch sector, recompress whole unit), read
   masks by bitmap, discard flips bits and frees the unit when the
   bitmap reaches 0.  Needs a dedicated correctness test before any
   performance claim.
5. **Measure** -- entry table size, RAM held, untar vs churn at
   physical=64K vs 4K.  The entry table should drop ~16x; that is the
   number this whole session exists to move.

Open question to settle in step 4: whether the write-triggered
recompress should zero the discarded sectors' stale bytes (shrinks the
unit, and is free since we are already paying the full recompress) or
keep them (lets them contribute cross-sector compression matches, per
the user's spec).  Implementing as specified -- keep them -- and
leaving the zeroing variant as a one-line alternative for the
benchmark to decide.

---
name: zcell-zram-reexploration
description: reopening the cell idea as a zram replacement for the OpenWrt fleet -- the grouping trap vs the swap lifecycle, the userspace-PoC-to-in-kernel pivot, the component-naming glossary, and cell_get_total_pages()
project: linux
previous: 2026-10-16_group-decompress-truncate-race.md
---

# Mission Log: Reopening the Cell Backend as a zram Replacement

Part 51 of the tmpfs transparent-compression series.  Session 67
(2026-10-16), same day as the truncate-race fix (previous log).  A
design-and-decision session with one small code landing.  The brief, in
the user's words: "i still want to reexplore the block level zcell
blkzcell as a swap," clarified to **zcell as a zram replacement** (a
compressed-RAM block/swap device to displace zram on the 64M OpenWrt
fleet), "design review, no code yet" -- which then produced one accessor
commit and a decision to build an in-kernel microbench.

## What We Did

### 1. Bootstrapped from the record, then re-read the block-device lineage

Read the latest logs (the 32-bit packing handoff, the cross-arch boot
sweep, the i386-PAE gate, tooling centralization, the truncate race)
plus the whole block-device thread: `cellzram-block-device`,
`zcell-verdict-tmpfs-pivot`, `cellzram-removal`,
`arm3-cell-backend-measurement`, `openwrt-refocus`, and `specs/01`.
The parked verdict and the deleted `cellzram` were treated as settled;
the re-exploration had to earn its way past them on new information.

### 2. Design review: what is actually NEW since the session-35 verdict

The verdict killed zcells-as-RAM-blockdev on one measurement (2.7x
slower than zram on random-overwrite churn, for a ~0.1% RAM win).  Three
things have moved, and they are the whole reason to reopen:

1. **The fleet is now known** (session 40): 64M IoT boxes where
   reclaim never converges before OOM, so compression must be
   unconditional.  zram-swap is what OpenWrt ships today and it
   OOM-kills.  The 2.7x was measured on a workstation churn bench, not
   this workload.
2. **arm 3 measured cells BEATS zsmalloc**: -8.6% compressor RAM at an
   identical ratio on real data (the `ram_total` gauge).  zram uses
   zsmalloc.  So the allocator zram uses is measurably the less
   RAM-efficient one -- a new, positive signal that did not exist when
   the verdict was written.
3. **The subsystem matured far past the deleted `cellzram`**: pointer
   encoding, u64 handle, HIGHMEM fixes, memcg charge, the group/merge
   path.  Any device built now sits on far more robust code.

### 3. The grouping trap, and the swap-lifecycle hypothesis

Stated the verdict's core finding as an architecture fact and named the
exact tension:

> Cells only beat zsmalloc when GROUPED (many objects per cell); but
> grouping needs write-gathering (staging), and on a block device that
> staging is a page-cache substitute that cost 2.7x.  At ratio 1 (no
> staging) cells were WORSE than zram on RAM (101.8% vs 100.0%) because
> of the whole-cell floor.  The RAM win and the staging cost are bought
> together.

Then the hypothesis that may dissolve it: **swap slots are write-once,
read-once, freed-on-swapin -- there is no in-place overwrite.**  The
three churn bugs that forced the open-cell staging buffer (gc_lock
self-deadlock, GC-ordering starvation, raw-seed corruption) were ALL
triggered by the overwrite pattern.  If a cell is filled once per
cell-life and freed whole, the seed/re-stage machinery has no reason to
exist.  This is the load-bearing open question: does swap's lifecycle
let us group pages into cells WITHOUT the overwrite-driven staging?
If yes, we get the -8.6% without the 2.7x; if no, we are back in the
trap.  Unmeasured -- the point of the planned bench.

### 4. Userspace PoC considered, then PIVOTED to in-kernel

The user first proposed a userspace PoC that mocks the kernel API and
reuses the real `cell_store.c`/`zsmalloc.c`.  I assessed the dependency
surface (a real feasibility check, not a guess):

- `cell_store.c`: 5 headers, no per-CPU, no kmap, pure bump+memcpy.
  A faithful mock is ~200 lines, low risk.
- `zsmalloc.c`: **2428 lines across 17 heavy headers** -- per-CPU
  zspages, kmap, movable_operations, shrinker, workqueue, scatterlist,
  `zpdesc.h`.  Faithful mock is a big surface with real risk of
  measuring a subtly-wrong zsmalloc.

The decisive argument was NOT effort, it was **mechanism**: the cells
value proposition -- compound `alloc_pages(order)` cells, synchronous
whole-cell return to the buddy, "MemFree tracks usage with no shrinker"
-- is fundamentally about the REAL buddy allocator.  A userspace `mmap`
mock has no buddy, no fragmentation, no order-N allocation, so it would
measure the algorithm and miss the mechanism, and the mechanism IS the
-8.6% pitch.  Three supporting facts flipped the decision:

- **The zsmalloc arm needs no mock**: real `zram` (zsmalloc-backed) is
  already in our sweep kernels and is the production baseline.
- **KVM iteration on this rig is sub-second** (session 48: i386-PAE
  boot+battery under KVM), so "in-kernel is a slow QEMU slog" was a
  false assumption -- iteration cost is comparable to a native binary.
- The real code path already runs end to end in these kernels.

Decision: **in-kernel microbench, both allocators, real buddy.**  The
userspace mock survives only as an optional cell_store-only policy
sweeper (where the buddy is not the variable); the userspace *zsmalloc*
mock was dropped.

### 5. The "huge sector swap" question, answered from source

Read `read_swap_header`/`setup_swap_clusters_info` in `mm/swapfile.c`.
Swap is hardwired **PAGE_SIZE-granular**: slot count, the header (page
0), and every offset are `>> PAGE_SHIFT` / `last_page` / `p->max`.  The
swap layer submits PAGE_SIZE bios, so the device's **logical block size
must be <= PAGE_SIZE** -- a device exposing a 64K logical block cannot
service a 4K write.  So "one swap slot = one whole cell" (which would
make grouping free) **cannot go through the swap interface**.  Huge
cells live INSIDE the device (exactly how zram hides zsmalloc pools
behind 4K logical blocks).  The two ways around: a zswap-style hook
(compress straight into a cell pool, no gendisk, no slot granularity)
or a huge-logical-block device for a NON-swap consumer (back to the FS
overwrite verdict).

### 6. Component naming -- pin the vocabulary (user-driven)

The user flagged the earlier "zcells also contains the compression
layer" confusion and wanted the naming locked before building.  Grepped
the tree: **no code identifiers exist** for `zcell`/`blkzcell`/
`cellzram` (one stray comment at `shmem_compress.c:2395`) -- they are
spec/log/conversation words, which is why they drifted.  Laid out the
four real on-disk components (cell allocator / shmem_comp engine /
zsmalloc / zram), the layering, the ambiguity map, and a proposed
canonical glossary.  **Ratification is still pending** (the user went on
to drill into the accessor); recorded as an open decision below.

### 7. Landed `cell_get_total_pages()`

The user asked whether the cell allocator needs a `zs_get_total_pages()`
analogue, then a `cell_compact()` analogue.  Answered both (see What We
Learned), then -- per "just cell_get_total_pages(), defer the rest" --
added only the accessor, and wired it into the one live site that
already reached into the struct field cross-file.

Commit `a92cc476f7eb` "shmem_comp/cell_store: add
cell_get_total_pages() footprint accessor": header decl + impl
(`atomic_long_read(&cp->pages_used)`, NULL-safe, mirrors
`zs_get_total_pages()`); `shmem_comp_get_stats()` now calls it instead
of `atomic_long_read(&pool->cells->pages_used)`.  Both backends report
footprint through symmetric calls now.  i386 gate: 0 errors, 0 warnings
(`out/kbuild-i386`, both objects fresh).

## What We Learned

### Technical

- **The grouping trap is the whole design question for a cell-backed
  zram.**  cells < zsmalloc on RAM only when grouped; grouping needs
  staging; staging cost 2.7x on the FS-overwrite profile.  Any
  cell-block-device pitch must show it escapes this, not re-measure it.
- **Swap's write-once lifecycle is the plausible escape.**  Slots are
  alloc'd, written once, freed on swapin; no in-place overwrite means
  the overwrite-driven staging (and its three churn bugs) may be
  unnecessary.  This is a hypothesis to measure, not a conclusion.
- **`cell_compact()` self-contained is structurally impossible.**  Cells
  have NO per-object index by design (header invariant #1: no
  per-object header, no size-class, no free list) -- that is the
  zero-metadata win.  `cell_relocate_one()` takes `data`/`len` as
  ARGUMENTS because the allocator cannot find its own objects; the
  "who lives in the victim" scan is the CALLER's job (shmem_comp's
  entry table; a frontend's block table).  **Metadata-free storage and
  self-compaction are a package deal** -- zsmalloc is the mirror (it
  pays per-object metadata SO it can self-compact via `zs_compact()`).
  A callback-wrapper `cell_compact()` would just relocate shmem_comp's
  existing GC driver loop down a layer -- defer until a second frontend
  wants to share it.
- **Do NOT give cells an internal object index** to make compaction
  self-contained: it costs ~8B/object (the metadata tax the design
  exists to avoid, on every store/free) and our engine ALWAYS has an
  external index already (entry table / block table) -- paying twice is
  pure waste for our use.
- **Swap is PAGE_SIZE-granular** (slot addressing + bios); a swap
  device's logical block size must be <= PAGE_SIZE.  Huge cells are an
  internal-to-the-device property, never a swap-visible sector.
- **A userspace mock cannot exercise the buddy allocator.**  Cells'
  headline properties (compound alloc, synchronous whole-cell return,
  MemFree tracking) live entirely in the real buddy, which an `mmap`
  arena cannot reproduce.  Disqualifying for the footprint question.
- **`shmem_comp_pool_cell_pages()` is a naming trap**: it returns
  `1U << cell_order` (pages PER cell), not total -- reads like "total
  cell pages" next to the new `cell_get_total_pages()`.  Rename
  deferred, flagged.

### Process

- **Pin the component glossary before building across a multi-component
  system.**  The "zcells also contains the compression layer" stumble
  was a vocabulary gap, not a design gap; naming it now (four real
  components, an ambiguity map, a proposed canonical set) is cheaper
  than another misunderstanding mid-build.
- **Enumerate a mock's dependency surface BEFORE committing to a
  userspace PoC.**  One grep of the includes (zsmalloc's 17 heavy
  headers) turned "reuse both files in userspace" into "reuse the easy
  one, mock nothing, go in-kernel."  Cheap check, avoided a rabbit hole.
- **When a hypothesis maps to a mechanism a cheap instrument cannot
  reach, change the instrument, not the hypothesis.**  The footprint
  question is about the buddy; the userspace PoC cannot see the buddy;
  so the in-kernel bench is the correct instrument, not a compromise.

## What We Decided

- **zcell-as-zram is reopened as a legitimate track IFF the swap
  lifecycle dissolves the staging cost** (the swap-lifecycle
  hypothesis above).  That is
  the question the bench must answer first; nothing else is worth
  building until it is.
- **In-kernel microbench is the primary instrument** (not a userspace
  mock): a small harness that creates a `zs_pool` and a `cell_pool`,
  replays a swap-shaped object stream into both, and reports footprint
  (`cell_get_total_pages()` vs `zs_get_total_pages()`), throughput, and
  GC cost -- plus a concurrency sweep to expose the `cp->lock`
  single-active-cell serialization vs zsmalloc's per-CPU zspages.
- **Canonical glossary PROPOSED, ratification pending** (the 3-way
  choice: keep "zcell"=engine+backend / drop it for explicit terms /
  other).  The factual inventory (4 components, layering, ambiguity map)
  is settled regardless.
- **Deferred until proven useful**: a `cell_compact()` callback wrapper,
  the `shmem_comp_pool_cell_pages()` rename, any internal cell object
  index.
- **`cell_get_total_pages()` landed** (small, verified, committed).

## Rules Added

- **A userspace mock is disqualified for anything whose value lives in
  the real buddy allocator.**  Compound-page allocation, whole-cell
  return, and MemFree-tracking are buddy mechanisms; an `mmap` arena
  cannot exercise them.  Mock only the algorithm, and only when the
  mechanism is not the variable.
- **Pin the component glossary before building across a multi-component
  system** -- especially when informal names ("zcell"/"cells"/"zram")
  span code, specs, and deleted history.
- **Enumerate a target's dependency surface before reusing it in a
  foreign environment** (userspace mock, cross-arch, etc.).  One include
  grep is cheaper than a faithfulness rabbit hole.

## What We'd Do Differently

- Would have pinned the naming glossary the moment "zcells also
  contains the compression layer" surfaced, instead of echoing "zcell"
  loosely for several turns -- the vocabulary drift was foreseeable and
  cheap to head off.
- Would have enumerated zsmalloc's includes BEFORE proposing to reuse
  both files in userspace; the 2428-line/17-header surface would have
  sent us in-kernel one message earlier.
- Will keep `shmem_comp_pool_cell_pages()` from ever sitting next to
  `cell_get_total_pages()` in a reviewer's diff without the rename --
  the two names are a confusion pair.

## Files Changed

| File | Purpose |
|------|---------|
| `include/linux/cell_store.h` | declare `cell_get_total_pages()` (+ doc comment) |
| `mm/cell_store.c` | implement `cell_get_total_pages()` |
| `mm/shmem_compress.c` | `shmem_comp_get_stats()` calls the accessor instead of poking `pool->cells->pages_used` |

(Kernel commit `a92cc476f7eb`.)

## Test Results / Benchmarks

- **i386 compile gate: 0 errors, 0 warnings** (`out/kbuild-i386`,
  `mm/cell_store.o` + `mm/shmem_compress.o` rebuilt fresh).  The accessor
  is a 5-line NULL-safe `atomic_long_read` wrapper using a symbol
  already used elsewhere in the file -- low risk, gate-confirmed.
- No benchmark this session -- the bench is the next deliverable (see
  Next Steps).  Every performance number above (the -8.6%, the 2.7x,
  the 101.8%-at-ratio-1) is cited from prior logs, not re-measured.

## Next Steps

1. **Ratify the canonical glossary** (the pending 3-way choice) and add
   it to the top of the log set so future sessions inherit it.
2. **Build the in-kernel microbench** -- `mm/cellbench.c` + a
   Kconfig/Makefile hook + a `gc_now`-style sysfs trigger under
   `/sys/fs/tmpfs/compression/`.  It creates a `zs_pool` and a
   `cell_pool`, replays a swap-shaped stream (4K page -> ~2:1 length
   distribution, write-once/free-on-swapin) into both, and drives
   `pick_victim`/`relocate_one`/`release_victim` directly (which also
   validates the primitive API a future block frontend would use).
3. **Measure**: footprint (`cell_get_total_pages()` vs
   `zs_get_total_pages()` against real `nr_free_pages`), throughput
   (objects/s), GC cost (cell evacuation passes vs `zs_compact()`), and
   a **multi-threaded sweep** to expose the `cp->lock` serialization vs
   zsmalloc per-CPU.  This answers the swap-lifecycle hypothesis and the two open
   questions (footprint + concurrency) before any device code.
4. **Decide the shape** from the bench: swap-only device vs zswap-style
   hook vs defer -- gated on whether cells actually beat zsmalloc for a
   swap stream.
5. Standing backlog unchanged: H618 re-measure, RFC assembly,
   plateau/CPU-freq, workload bed (spec 05), BE runtime boot.

---

## Session 68: Swap Form, Private Pools, and the mTHP Page-Group Insight (2026-10-16)

(This append is session 68; same file/topic as session 67's reexploration.)

### What We Did

Session 67 reopened zcell-as-zram as a design track and landed
`cell_get_total_pages()`.  This session converged the actual device
design; the centerpiece is a new insight that dissolves the
grouping/staging problem that killed the old cellzram.

**1. Form: a thin cell-backed swap gendisk (not a zswap cache).**  The
fleet's pain is zram-swap (a gendisk that IS swap); zswap is a cache in
front of a backing disk swap, and the 64M boxes have no disk.  So: a
gendisk, mkswap/swapon, matching zram.

**2. The "big file in ztmpfs" reframing (user's).**  The swap storage is
conceptually a big region of shmem_comp-managed compressed blocks,
keyed by SWAP OFFSET instead of file page-index and NOT installed into
page-cache folios -- but everything that manages the compressed-storage
lifecycle is reused from the engine:
- `shmem_comp_pool` (private per device), compression contexts, cell/zs backend
- `shmem_comp_compress`/`decompress` (sync compression, HIGHMEM-safe)
- `shmem_comp_store`/`load`/`free` -- ALREADY folio-free and
  handle-keyed (pool + handle, no inode/entry/folio); the tmpfs
  two-table mapping + folio install are a SEPARATE layer on top that a
  swap consumer SKIPS
- the 1 Hz aging/GC (`cell_gc_all`) -- exactly the mechanism that
  reclaims fragmented cells from free-on-swapin
- the watermark (spec-06), the shrinker, the stats
New (thin glue only): a `swpentry -> handle` xarray (replaces the
two-table mapping) + the swap-integration surface.

**3. Pool model correction + sharing deferred.**  I misspoke that
mounts might share a "global pool."  The code is unambiguous: `fill_sb`
(:2275) ALWAYS calls `shmem_comp_pool_create()`, which kzallocs a
FRESH pool (refcount=1, its own `cell_pool`) onto a global LIST (:216)
that is bookkeeping only (stats / GC-all walk it).  No dedup, no join.
So each mount -> private pool (per-mount compress=/cell=/gc_percent
honored); each gendisk -> private pool.  The refcount "sharing"
machinery exists but is not exercised.  USER DECISION: sharing
deferred -- a shared pool couples every consumer's watermark/GC/tuning
into one state machine, a debugging tar pit; private pools until
single-pool behavior is well-characterized.

**4. Honesty surfacing: the old cellzram WAS this thin device.**  Its
default (`open_units=0`, ratio 1) `cellzram_write_block` does exactly
`shmem_comp_compress -> shmem_comp_store -> shmem_comp_entry_alloc ->
xa_store` (block table), on a private `shmem_comp_pool`.  So
rebuilding the thin device at ratio 1 re-runs a MEASURED, LOST config:
0.86x zram throughput on churn, 101.8% of-orig RAM (session 35).
Engine maturity since (pointer encoding, HIGHMEM, memcg, u64 handle)
is CORRECTNESS, not that throughput.  So ratio-1 is a known ~tie-or-lose
baseline, not the goal.

**5. THE INSIGHT: mTHP gives page groups for free, dissolving staging.**
The whole reason the old cellzram lost was grouping: cells beat
zsmalloc only when grouped (the -8.6% came from 16-page merge), but a
block device has no page cache, so it MANUFACTURED groups by staging
(the open-cell buffer), costing 2.7x on overwrite.  The new
realization, confirmed in `mm/page_io.c`:
- `__swap_writepage(ctx, folio)` is FOLIO-based and does
  `swap_add_folio(ctx, folio, WRITE)` -- the WHOLE folio as one unit;
  `count_mthp_stat` tracks `folio_order`.
- With `CONFIG_THP_SWAP`, a large anon folio (order N = 2^N contiguous
  pages) reserves 2^N contiguous swap slots and is written as ONE unit
  (`swapfile.c:1750` hard-rejects multi-page slot alloc without it).
- Base pages: no grouping (one page per bio).  mTHP (multi-size THP,
  small orders 16K-64K): anon memory comes back as large folios,
  swap-out hands the device an N-page folio, and it compresses the
  WHOLE FOLIO as one compression unit -- cross-page redundancy = the
  -8.6% win -- with NO device-side staging.
This dissolves the 2.7x: the groups are pre-assembled by the MM, not
buffered by the device.  The engine already speaks multi-page units:
`shmem_comp_compress(slen = merge_pages multiple)` +
`shmem_comp_store_merged` (len > PAGE_SIZE chunks).  So the device gets
EVEN THINNER: no staging logic at all -- `submit_bio` -> compress the
folio as one unit -> `store_merged` -> record `swpentry -> handle`.
Grouping = mTHP, compression = engine, storage = cells, GC = engine
1 Hz aging.

**6. arm32 page size (user question, verified).**  arm32 is 4K base
pages, FIXED (`out/kbuild-arm32/.config`: `CONFIG_PAGE_SIZE_4KB=y`,
`CONFIG_PAGE_SIZE_LESS_THAN_64KB=y` -- ARM32 has no 16K/64K base-page
option).  aarch64 SELECTS 4K/16K/64K (`CONFIG_ARM64_{4K,16K,64K}_PAGES`);
our matrix runs all three.  Consequence for mTHP: the 64K-PAGE aarch64
kernel is the ideal testbed -- a base PAGE IS 64K, so EVERY anon folio
is >= 64K and one cell (order 4 = 64K) holds ~one page; grouping is
trivial and there is no sub-cell-staging question at all.  arm32 (4K)
and aarch64-4K are the worst case for the cell floor and want mTHP
orders that make folios >= a cell.

### What We Learned (session 68)

### Technical

- **Swap is folio-based, not page-based, at the write path**:
  `__swap_writepage` takes a folio and `swap_add_folio` submits it
  whole.  Multi-size THP (mTHP) means a swap "page group" is just a
  large anon folio -- the grouping lever lives in the MM's anon folio
  allocation, NOT in the swap device.
- **`CONFIG_THP_SWAP` gates multi-page swap-slot allocation**
  (`swapfile.c:1750`).  `SWAPFILE_CLUSTER` = `HPAGE_PMD_NR` (512) with
  THP_SWAP, else 256.
- **The engine's store/load/free are already folio-free and
  handle-keyed** -- a swap consumer skips the entire tmpfs two-table
  mapping + folio-install layer.  "Reuse the engine, skip the folios"
  is exactly right at the API level.
- **Each mount/pool is private; the global pool list is bookkeeping.**
  No dedup in `pool_create`; refcount sharing exists but is unexercised.
- **The old cellzram-at-default IS the thin device** -- its 0.86x/101.8%
  is the ratio-1 baseline to reproduce, not to re-derive as a discovery.
- **arm32 = 4K fixed base pages; aarch64 = 4K/16K/64K selectable.**  A
  64K-page kernel makes every folio >= a 64K cell, collapsing the
  grouping question.

### Process

- **"Commit often for bisect" (user, standing):** the device will be
  built in small green commits (skeleton+wiring, private pool +
  disksize, submit_bio+compress+store, swapin-free+GC, mTHP grouping,
  sysfs, measurement), each compiling and gate-checked.
- **Verify a design against the deleted prior art before building.**
  Retrieving cellzram.c from git history (`git show 49b5571006b4^`)
  exposed that the "new thin device" was already built and measured --
  which reframed the build around the mTHP grouping differentiator
  instead of a re-derivation.

### What We Decided (session 68)

- **Form: thin cell-backed swap gendisk** (not a zswap cache).
- **Reuse the whole shmem_comp engine, private pool per device**; only
  new code = `swpentry -> handle` map + swap glue + per-folio
  compression via the merge path.
- **Pool sharing deferred** (debugging/tuning coupling).
- **Grouping via mTHP (small orders 16K-64K), NOT device staging.**
  This is the differentiator: if zram compresses per-page (it is
  page-granular), our per-folio compression achieves a ratio zram
  structurally cannot, and mTHP unlocks it only for us.
- **Build with mTHP**; the 64K-page aarch64 kernel is the primary mTHP
  testbed.

### Next Steps (session 68, superseding session 67's bench plan)

1. Verify the differentiator: does zram compress per-page or per-folio
   in `zram_bvec_write` under mTHP?  (one look; decides whether
   per-folio grouping is a real ratio win over zram)
2. Check the mTHP config surface on a 64M guest + on the 64K-page
   aarch64 kernel (which mTHP orders actually produce large anon folios
   under the fleet workload).
3. Build the thin device staged, committing each green step (see
   Process).
4. Measure vs zram with mTHP enabled on the fleet-shaped workload; the
   win is per-folio compression ratio + cells' zero-metadata storage,
   the cost is mTHP reliability on fragmented 64M.

---

## Session 69: The zcell Device, Ratio-1 (2026-10-16)

(Session 69; same file as the design sessions 67-68.  Build phase.)

### What We Did

Confirmed the differentiator, then built the ratio-1 device in two
bisectable commits.

**1. Confirmed zram is per-page (the differentiator is real).**
`zram_bio_write()` caps each bvec at PAGE_SIZE
(`bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset)`) and calls
`zram_write_page(zram, page, index)` -- one page compressed
independently into its own zsmalloc object.  Even when a large mTHP
folio is swapped out, zram splits it into per-page compressions and
gets ZERO cross-page redundancy.  A zcell device that compresses the
whole folio as one unit (the merge path) achieves a ratio zram
structurally cannot.  mTHP unlocks that only for us.

**2. Commit `d1c18dc255cc` -- export `shmem_comp_entry_get()`.**
The post-pointer-encoding entry API lost the generic id-based
`entry_get`; a device frontend needs to pin an entry (from its own
block table) for a race-free read.  Factored the RCU + inc_not_zero pin
out of `shmem_comp_entries_get()` into an exported
`shmem_comp_entry_get(ent)`.  i386 gate 0/0.

**3. Commit `553231150228` -- the zcell device (ratio-1).**
`mm/zcell.c` + `Kconfig(ZCELL)` + `Makefile`.  A thin gendisk over a
private `shmem_comp_pool`:
- block table `block_nr -> struct shmem_comp_entry *` (the POINTER, not
  an id).  Makes GC relocation transparent: the engine moves
  `ent->handle` in place, a reader holding the pointer always loads the
  fresh handle, no table update on GC.
- write: `shmem_comp_compress` -> `shmem_comp_store` ->
  `shmem_comp_entry_alloc` -> `xa_store(pointer)`; overwrite releases
  the old entry.
- read: `xa_load` + `entry_get` (RCU) -> `load_entry` (cell-specific,
  has the GC-retry) -> decompress if `ent->length < PAGE_SIZE`.
- `swap_slot_free_notify` -> `xa_erase` + `entry_release` -> the cell
  returns to the buddy inline (synchronous reclaim zram lacks).
- sysfs: disksize / comp_algorithm / mm_stat (via
  `cell_get_total_pages`) / io_stat / reset.
i386: `zcell.o` 0/0; all 13 `shmem_comp_`/`cell_` engine symbols resolve
against `shmem_compress.o`/`cell_store.o` (verified with nm).

### What We Learned (session 69)

### Technical
- **The current entry API is cell-backend-specific on the read side**:
  `shmem_comp_load_entry()` unpacks `pack_loc` and uses `cp->cells`, so
  it only works for a cell pool (fine -- zcell is cell-only by design).
  The device reads `ent->length` (immutable post-alloc) to tell raw
  (`== PAGE_SIZE`) from compressed (`< PAGE_SIZE`).
- **Storing the entry POINTER (not an id) in the block table makes GC
  transparent** -- the one design choice that lets the device reuse the
  engine's cell GC without a table-relocation hook.
- **A block device needs `<linux/blk-mq.h>`** for
  `blk_mq_freeze_queue`/`unfreeze` (reset/remove) -- implicit
  declaration otherwise.
- **The full-vmlinux build is blocked by the known VT/ucs_width_table.h
  tree-tip bug** (session 47), unrelated to zcell; the arch gate works
  around it with `--disable CONFIG_VT`.  Object compile + nm symbol
  resolution stand in for the link check until a bootable build.

### What We Decided (session 69)
- **Ratio-1 first** (the known-baseline config: old cellzram's default
  minus staging), to establish a correct, testable device on the modern
  engine before layering the novel mTHP grouping.
- **Cell-only backend** (load_entry is cell-specific; zsmalloc users
  have zram).

### Next Steps (session 69)
1. Boot-test: build a bootable x86_64 kernel with ZCELL=y (VT off, via
   the rig builder), mkswap/swapon /dev/zcell0, swap-pressure
   correctness + mm_stat/io_stat in the rig guest.
2. mTHP config surface (which orders yield large anon folios on 64M +
   the 64K-page aarch64 kernel; CONFIG_THP_SWAP).
3. Commit 3: mTHP folio grouping (the differentiator).
4. Measure cell-swap vs zram on the fleet workload.

---

## Session 70: The Device Boots and Works as Swap (2026-10-17)

(Session 70; same file.  Boot-test phase for the ratio-1 device.)

### What We Did

Built a bootable x86_64 kernel with `ZCELL=y`, wrote a focused boot
test, and exercised `/dev/zcell0` as real swap.  **All checks PASS.**

**1. Kernel build -- the stale-artifact rule reaffirmed.**  The first
build (from the main tree `/home/snide/linux`, `build_arch_kernel.sh
x86_64 zcell --enable CONFIG_ZCELL`) produced a kernel that died at boot
with `uncompression error -- System halted`: the session-66 stale-artifact
class (the main tree's historical in-tree builds leave stale generated
files that corrupt the gzip payload).  Rebuilt from a **pristine git
worktree** (`git worktree add /home/snide/linux-zcell HEAD`; `SRC=`
override) -> clean boot (`Decompressing Linux... done`).  The session-66
rule ("bootable kernels are built from a pristine worktree, not the main
tree") holds and paid off immediately.

**2. The boot test** (`tests/test_zcell.c`, rig `2a8e450`):
- O_DIRECT block round-trip: write patterned pages, read back, verify.
- mm_stat sanity (orig/compr/mem_used, ratio in (0,1)).
- Swap trial: mkswap + swapon + anon pressure + MADV_PAGEOUT; verify the
  device takes swap writes (io_writes up, SwapFree down), then **DATA
  INTEGRITY** -- fault every chunk back in and byte-compare the pattern,
  which exercises the full compress->store->swapout->swapin->load->
  decompress path.  swapoff must return the cells.

**3. Two TEST bugs on first boot** (not device bugs), fixed in rig
`440d376`:
- The fill + integrity loops used `n < 16 * 4096` (=65536 pages = 268MB)
  against a 16MB mapping -> segfault (write past the mmap).  Fixed to
  `CHUNK_PAGES = 16*MIB/PAGE` (=4096).
- test_swap re-set disksize already set by the round-trip (-EBUSY);
  reset the device between the two tests first.
The device itself was innocent: the round-trip PASSED and compression
worked on the very first boot, before either test bug was fixed.

### Test Results (the decisive boot, QEMU/KVM, 1G, x86_64 worktree kernel)

```
PASS sysfs disksize present
PASS set disksize 16M / write patterned blocks / read back matches pattern
mm_stat: orig=262144 compr=41344 mem=65536 ratio/10000=1577   (6.3:1)
PASS mm_stat fields populated / compression ratio in (0,10000)
PASS set disksize 64M (after reset) / swapon / SwapTotal > 0
pressure: 48 x 16M anon chunks, MemFree 863256 -> 97452 kB
SwapTotal=65532 SwapFree=0 kB; device io_writes 65 -> 16448
PASS swap writes hit the device / SwapFree dropped
PASS swapped data read back intact (no corruption)
post-swapoff mm_stat mem_used=64 kB
PASS cells returned after swapoff
Test exit code: 0
```

### What We Learned (session 70)

- **The zcell device works end to end as swap.**  The thin gendisk over
  the shmem_comp engine, the private pool, the pointer-based block
  table, the entry machinery, and the swap integration all function
  together on first real execution (modulo two test bugs).
- **Data integrity through the full swap round-trip is CONFIRMED**: the
  compress->store->swapout->swapin->load->decompress chain returns
  byte-identical data under real swap I/O and memory pressure.
- **The synchronous whole-cell return works live in the swap context**:
  after swapoff `mem_used` dropped 65536 -> 64 kB, i.e. the cells went
  back to the buddy inline as swapin freed the slots.  This is the
  property zram's architecture lacks (it retains zspages for reuse), and
  it is why MemFree tracks usage on a cell device.
- **Compression on real swap pages is strong**: 6.3:1 on the (admittedly
  compressible) test pattern; real anon pages will vary.
- **The session-66 stale-artifact rule is load-bearing for x86_64**: the
  main tree cannot produce a bootable x86_64 kernel; always use a
  pristine worktree.
- **A data-integrity check is the only thing that proves a swap device.**
  "Swap writes hit the device" (io_writes) proves the WRITE path; only
  faulting the pages back in and comparing proves the READ path.  The
  old cellzram test checked io_writes but not readback integrity; this
  test checks both.

### What We Decided (session 70)
- **The ratio-1 baseline is validated and correct.**  It is now a solid
  foundation to layer the mTHP folio grouping (the differentiator) on top.
- **Bootable-kernel builds go through a pristine worktree** (session-66
  rule, now applied to the zcell track).

### Next Steps (session 70)
1. mTHP config surface: which orders produce large anon folios on a 64M
   guest + the 64K-page aarch64 kernel (`CONFIG_THP_SWAP`, THP sysfs).
2. Commit 3: mTHP folio grouping -- compress the whole swap folio as one
   unit via the merge path (the differentiator vs zram's per-page).
3. Measure cell-swap vs zram (the build script already enables ZRAM, so
   both devices are in the same kernel) on the fleet workload.

---

## Session 71: The Ratio-1 Baseline -- zcell vs zram (2026-10-17)

(Session 71; same file.  The honest baseline the mTHP grouping must beat.)

### What We Did

Benched the ratio-1 device against zram on real swap traffic, on the
i386 rig (KVM-fast, and 32-bit representative of the fleet's arm32/arm7
cores).

**Use case: cold-anon tiering** -- the zram-swap scenario OpenWrt ships.
An idle working set is swapped out to compressed RAM to free memory for
an active burst, then swapped back in on wakeup.  The backend (zsmalloc
in zram vs cells in zcell) decides (a) how much host RAM the compressed
cold set costs, (b) how fast it swaps back.  This is the fleet's real
behavior, not a synthetic.

**The bench** (`test.mode=swapbench`, added to the sweep guest
`arch_init.c`, rig `2369ff8` + `5a0a98b`).  Self-contained (no busybox):
mkswap header in C, swapon syscall, swap out a cold ~2:1 anon set via
MADV_PAGEOUT, read mm_stat (backend RAM), fault back in (timed + data
integrity), swapoff.  zram first, then zcell, identical bytes both.
i386 kernel with ZCELL+ZRAM+SWAP+ADVISE=y, KVM, 1536M, 32M cold set,
64M swap device.

**Two build/runtime fixes on the way:**
1. **A real 32-bit bug in zcell.c** (kernel `e9d45c8e96e5`): the i386
   vmlinux link failed with `undefined reference to '__udivdi3'` --
   `DIV_ROUND_UP(u64,u64)` and the mm_stat ratio emit a libgcc 64-bit
   divide the kernel does not link.  Fixed with `div64_u64()`
   (linux/math64.h).  **The compile gate could not catch this** (it
   compiles objects, does not link vmlinux); only the bootable 32-bit
   build did.  The arch matrix keeps earning its keep.
2. **No /dev in the sweep initramfs** (rig `5a0a98b`): the guest never
   mounts devtmpfs, so /dev/zram0 + /dev/zcell0 were missing (mkswap
   ENOENT).  mount("devtmpfs") at the start of swapbench.

### Test Results (3 reps, i386/KVM, near-zero variance)

Identical compressed bytes both backends (same lz4, same data -- so the
ONLY difference is the storage backend):

| metric | zram (zsmalloc) | zcell (cells) | delta |
|---|---|---|---|
| compressed bytes | 18,628,465 | 18,628,465 | identical |
| backend overhead (mem/compr) | 1.032 | 1.031 | zcell -0.15% (tie) |
| swap-out time | 44.5 ms | 52.5 ms | zcell ~18% slower (0.85x) |
| swap-in throughput | 610 MB/s | 500 MB/s | zcell ~18% slower (0.82x) |
| data integrity | intact | intact | both correct |

### Verdict

**At ratio-1 zcell is NOT competitive with zram: it ties on RAM
efficiency (0.15%, noise) and is ~18% slower on swap throughput in both
directions.**  This confirms the session-35 0.86x prior on the modern
engine with real swap traffic and a realistic ~1.8:1 data profile.  The
zero-per-object-metadata cell advantage does NOT show at ratio-1 for
~1K compressed objects (zsmalloc's per-object overhead is also small
there); it needs the grouped/larger objects the mTHP path would create.

### What We Learned (session 71)

- **A 32-bit runtime build catches __udivdi3; the compile gate cannot**
  (compile does not link vmlinux).  Any runtime u64/u64 divide in our
  code is a latent i386/arm32 link bomb.  div64_u64() everywhere.
- **The baseline is low-variance on i386/KVM** (3 reps, same data ->
  byte-identical compressed size, +/-1% timing), so an ~18% throughput
  gap is real, not noise.
- **At ratio-1, cells and zsmalloc are RAM-equivalent** for ~1K
  objects.  The cell win is a GROUPING win, not a per-object win -- which
  is exactly the mTHP bet.
- **The swapbench-in-the-sweep-guest pattern works**: self-contained
  mkswap+swapon+A/B in one static guest, no busybox, cross-compiles to
  any arch.  Reusable for the mTHP re-measure.

### What We Decided (session 71)
- **The ratio-1 baseline is established: tie on RAM, ~0.82-0.85x on
  throughput.**  This is the floor.  The mTHP folio grouping must
  clear it to make zcell worth deploying over zram.

### Next Steps (session 71)
1. mTHP config surface (which orders yield large anon folios on the
   i386/64M target + the 64K-page aarch64 kernel; CONFIG_THP_SWAP).
2. Commit 3: mTHP folio grouping -- compress the whole swap folio as one
   unit via shmem_comp_store_merged (the cross-page ratio zram cannot
   get).
3. Re-run swapbench with mTHP on: does the per-folio compression ratio
   beat zram's per-page enough to offset the ~18% throughput deficit?

---

## Session 72: The Ratio-1 Optimizations -- Small Win, Corrected Diagnosis (2026-10-17)

(Session 72; same file.  Follows the session-71 baseline.)

### What We Did

Implemented the two ratio-independent optimizations diagnosed in session
71, measured them, and found the diagnosis was wrong about MAGNITUDE.

**Commit `0e946ee06132`** (engine + device):
1. `shmem_comp_read_entry()` -- decompress DIRECTLY from the cell under
   its rwsem (no bounce).  Removed the kmalloc + copy-out + kfree per
   compressed read.  Device read path uses it.
2. `shmem_comp_entry_alloc_local()` -- register a device entry WITHOUT
   the global `shmem_comp_entries` xa_alloc (id=0 sentinel; entry_free
   guards the erase on id != 0).  GC finds device entries via the
   reverse index.  Device write path uses it.

**Measured (swapbench, i386/KVM, 3 reps, same data):**

| metric | baseline zcell | optimized zcell | zram ref |
|---|---|---|---|
| swap-in MB/s | ~500 | ~532 (+6%) | ~635 |
| swap-out time | 52.5 ms | 51.5 ms (~2%) | 44.5 ms |
| zcell vs zram | 0.82x | 0.84x | 1.0x |

Real, but MODEST -- the two "dominant suspects" were only ~1-2 ms each.

### The corrected diagnosis: the gap is DIFFUSE, not one hotspot

Traced the shmem_comp wrappers: `shmem_comp_compress` AND
`shmem_comp_decompress` do, on EVERY call: `mutex_lock(&pcpu->lock)` +
**two `ktime_get()` clock reads** (always-on per-call stats timing) +
sg setup + `crypto_wait_req`.  That is ~4 clock reads + 2 mutex pairs
PER SWAP BLOCK round-trip, on the hot path, for every shmem_comp user
(tmpfs included).  Add the entry-refcount + rwsem residue, and the ~16%
gap vs zram's lean `zcomp` (get_cpu + sg + crypto, no mutex, no timing)
is **death by a thousand small per-block costs, not one hotspot**.

The always-on `ktime_get()` timing is pure hot-path instrumentation
accumulated into comp_stats -- a candidate easy trim (gate or drop it),
but a shared-engine change and only worth ~1-2 ms.

### What We Learned (session 72)
- **A code-traced "dominant cost" can be wrong about magnitude.**  The
  global xa_alloc and the bounce were real but small; the gap is the
  accumulated generality overhead (per-CPU mutex, always-on timing,
  entry refcount, rwsem) vs zram's purpose-built path.
- **Diffuse per-block overhead is exactly what GROUPING amortizes** --
  and cannot be killed by any single ratio-1 micro-opt.  This
  VALIDATES the mTHP bet: one op per N pages amortizes the entire
  per-block tax (mutex, timing, refcount, rwsem) at once, plus the
  compression-ratio win.
- **The per-call ktime_get stats timing is hot-path instrumentation**
  in the shared engine -- worth gating for all shmem_comp users, but a
  separate change.

### What We Decided (session 72)
- **Stop ratio-1 micro-optimization.**  The gap is diffuse; further
  trims are death-by-cuts with diminishing returns.
- **Proceed to mTHP grouping** -- it amortizes the whole per-block tax
  AND wins compression ratio, the two things ratio-1 cannot.

### Next Steps (session 72)
1. mTHP config surface + the grouping commit (compress a swap folio as
   one unit via shmem_comp_store_merged).
2. Re-run swapbench with mTHP on: does the per-folio ratio beat zram's
   per-page enough to clear the ~0.84x base?
3. (Optional, separate) gate the per-call ktime_get stats timing in the
   engine -- helps tmpfs + device everywhere.

---

## Session 73: The Two Config-Independent Trims -- Correct, Modest (2026-10-17)

(Session 73; same file.  Follows session 72.)

### What We Did

Implemented the two config-independent trims (no mTHP dependency), then
rebuilt the i386 kernel and re-ran swapbench to check correctness +
measure.

- **Trim 1** `f1f219413243`: drop the dead per-call `ktime_get` timing
  from compress/decompress (2 clock reads/call, 4 per swap block; the
  `duration` was computed and never used).
- **Trim 2** `4a0c1f123353`: per-CPU compression buffer +
  `shmem_comp_compress_store()` -- compress+store in one step under the
  held per-CPU lock, removing the per-block `kmalloc(PAGE_SIZE)+kfree`
  for the compression output (zram's zcomp pattern).

### Test Results (i386/KVM, 3 reps, fresh boots)

**Correctness: PASS on every rep** -- `intact=1` (data integrity through
the full swap round-trip), `GUEST_RESULT: PASS n=0`, no corruption, no
deadlock.  This validates the `compress_store` lock-ordering (holding
the per-CPU mutex across `cell_store`'s sleeping `alloc_pages`) at
runtime, not just by reasoning.

**Perf (median of 3):**

| metric | zcell (trimmed) | zram | zcell vs zram |
|---|---|---|---|
| swap-out time | 49.5 ms | 44.0 ms | 1.14x (was 1.18x baseline) |
| swap-in | 533 MB/s | 628 MB/s | 0.85x (was 0.82x baseline) |
| RAM overhead | 1.031 | 1.032 | tie (unchanged) |

Swap-in variance was notable: 525 / 533 / 639 MB/s across reps (one rep
~zram parity; unexplained, 3 reps insufficient to characterize).

### Verdict

**The two trims are correct and help only modestly.**  Cumulative over
ALL FOUR config-independent trims (bounce, global xa_alloc, ktime,
per-CPU buffer), zcell moved 1.18x -> 1.14x swap-out and 0.82x ->
0.85x swap-in -- ~3-4% total.  **zcell does NOT reach parity on the
default (no-mTHP) config**, confirming session 72's diagnosis: the gap
is diffuse per-block overhead, not a killable hotspot, and micro-opts
cannot close it.  Against the bar "at least equal on default config,"
zcell currently fails.

### What We Learned (session 73)
- **Runtime validation of the lock-ordering change** (mutex across a
  sleeping alloc) passed -- no deadlock, no corruption, 3 fresh boots.
- **Four sensible trims recovered only ~3-4%.**  The remaining ~14-15%
  is structural (the entry indirection zram avoids by storing the handle
  inline, plus the engine's general per-block tax).  This is strong
  evidence the ratio-1 path cannot be micro-opted to parity.
- **Swap-in shows real run-to-run variance** (525-639) that zram does
  not (626-633) -- possibly GC/rwsem interaction; needs more reps to
  characterize.

### What We Decided (session 73)
- **The config-independent trim avenue is exhausted** (diminishing
  returns: ~1%/further trim).  Stop here.
- The **entry-elimination** idea (store the handle directly in the
  block table, no device GC relocation -- removes the entry indirection
  zram doesn't have) is the one remaining structural lever for parity;
  it is a design change, held for the "think creatively" round.

### Next Steps (session 73)
Held per user: no next-steps planned this session.  Standing state:
zcell correct, ~0.85x zram swap-in / 1.14x swap-out / RAM-tie on
default config; entry-elimination is the identified (unimplemented)
path toward parity.

---

## Session 74: The Indirection Question -- loc-Node Extraction (2026-10-17)

(Session 74; same file.  Starts the session-73 held lever:
entry-elimination / "indirection suppression", driven by the user's
observation.)

### What We Did

**1. Answered "why does the entry exist" field by field, in code.**
The zcell read chain is block_nr -> xa_load -> entry * -> ent->handle
(pack_loc: cellid+offset) -> cell data: a ptr-to-ptr, exactly as the
user said.  The entry exists for four reasons, each traced to code:
(a) GC relocation transparency -- the drain updates ent->handle in
place, so a table holding the POINTER never needs a fix-up (the
session-69 design comment); (b) the refs/slots lifetime pin across
decompress vs swap_slot_free_notify; (c) fields the 64-bit handle
cannot carry (length -- the raw-vs-compressed discriminator, pool,
the reverse-index node); (d) tmpfs's one-to-many sharing (one entry
backs N entries[] slots for a merge group).  Only (a), (b), (c) are
needed by the device; (d) is pure overhead it pays for nothing.

**2. zsmalloc precedent read (the decisive comparison).**  zram's
dense table stores `struct zram_table_entry { unsigned long handle;
... }` inline per disk page -- but its handle is a STABLE ID, not a
location: zs_malloc's handle goes through record_obj(handle, obj),
and migrate_zspage() re-records the SAME handle to the new location
(zsmalloc.c:1677-1705, obj_malloc()'s `link->handle = handle |
OBJ_ALLOCATED_TAG`).  So zsmalloc maintains a central
handle->location mapping and updates IT on compaction; owners are
never touched.  That is the ptr-to-ptr the user distrusts -- hidden
inside zsmalloc, paid as handle-table pages per object.  Our
pack_loc encodes the location directly (one hop, zero central
table), which is why OUR relocation must fix up owners.  Design
conclusion: keep pack_loc (the direct encoding is the cheap part);
suppress the OWNER NODE's separate allocation instead.

**3. The dense-slot protocol designed on paper before coding.**  The
hard part is that a table slot is IMMORTAL and reused across
generations, while an entry is per-generation -- so the refs==0
teardown, which reads node->handle to free the right location, breaks
under slot reuse (a new generation could install its handle under a
dying generation's pins).  Resolved with a generation protocol:
- slot state = {present (under per-slot spinlock), refs, handle}.
  present==false && refs>0 = DYING (pins draining, last put frees via
  the preserved handle); present==false && refs==0 && handle!=0 =
  mid-dispose; all clear = EMPTY.
- Install waits for EMPTY, so a dying generation's pins always drain
  against their own handle.  This is what makes refcount sharing
  across generations safe -- no ABA.
- Drop (swap free / discard / overwrite-hide) clears present under
  the slot lock, then drops the durable ref: the common case (no
  pins) frees synchronously inline (cells -> buddy, the headline
  property); the pinned case hands off to the last put.
- GC needs NO slot lock: it pins (inc_not_zero), detaches,
  relocates, WRITE_ONCEs the handle, relinks, puts.  present is
  never touched by GC, so no resurrection is possible (the old
  ptr-to-ptr design needed no such argument; the dense design does,
  and it holds).
- Reader: lockless pin (present check, inc_not_zero, present
  recheck); a spurious pin is put and retried.  No RCU: slot
  addresses are stable (preallocated), and the pin keeps storage
  alive; relocation is handled by loc_read's existing
  re-read-under-cell-rwsem retry.
- Deadlock audit: writers wait for pins WITHOUT holding the slot
  lock (spinlock across a sleeping decompress would also break);
  readers take no locks before the pin; GC holds only the victim
  rwsem + index_lock and never takes a slot lock.  No cycles.

**4. Commit `f6cc4dd096c1` -- the engine extraction (this session's
landing).**  `struct shmem_comp_loc_node { u64 handle; unsigned int
length; atomic_t refs; struct list_head cell_node; }` extracted and
embedded at the entry's head; ~50 field accesses renamed to
`ent->loc.*`; the GC drain walks bare nodes and drops its pin via
`shmem_comp_loc_pin_put()` (frontend hook `pool->node_put` when set,
entry put otherwise); new exported `shmem_comp_loc_link/dispose/
read` + `shmem_comp_pool_set_node_put()`.  entry_alloc/alloc_local,
entry_free, load_entry/read_entry became thin wrappers -- pure
refactor, no behavior change.

**5. Gate-script bug found (pre-existing, fixed).**  The i386 gate
suddenly lost CONFIG_TMPFS_COMPRESSION: ZSMALLOC is PROMPTLESS at
tree tip -- scripts/config --enable writes it but olddefconfig drops
it, taking TMPFS_COMPRESSION with it.  build_arch_gate.sh already
carried the lesson (ZRAM selects ZSMALLOC); build_i386.sh predated
it.  Fixed + added mm/zcell.o to the gated object list (harness
commit 9ef1cef).  Symptom recognition was one grep: the gate's
"did the feature survive" line showed only CRYPTO_LZ4 left.

### What We Learned (session 74)

### Technical

- **zsmalloc handles are stable ids through a central mapping, not
  locations.**  migrate_zspage re-records the same handle
  (obj_malloc's record_obj).  zram's "inline handle in a dense
  table" therefore hides a ptr-to-ptr inside zsmalloc (handle-table
  pages per object).  pack_loc's direct location encoding is
  genuinely one hop; its cost is owner-fixup on relocate, which the
  node-in-table design absorbs.
- **An immortal reused table slot cannot share one refcount across
  generations.**  Either install waits for the old generation to
  fully drain (chosen: swap writes a slot once, so the wait is
  free), or each generation needs its own object (the entry again).
- **GC must never take a per-slot lock** -- the reader path is
  slot-lock -> cell rwsem, so a GC path of victim-rwsem -> slot-lock
  would be an AB-BA deadlock.  The refs-pin protocol keeps GC
  lock-free w.r.t. slots; `present` being GC-invisible is what kills
  the resurrection hazard the naive clear-handle drop would have.

### Process

- **Design the concurrency protocol on paper BEFORE the code** --
  the generation states (EMPTY/DYING/INSTALLING), the exactly-once
  durable-ref drop, and the lock-order audit all fit in the head
  once written down; the code then matched it on the first gate run.
- **Whitespaces in kernel oldText anchors: `cat -A` is the ground
  truth.**  Three edit retries were lost to tabs-vs-spaces in comment
  bodies (including a 4-tab-vs-5-tab in a continuation line); the
  fix was to verify the exact block with `sed -n | cat -A` and splice
  via python when the anchor kept drifting.
- **A failed compile gate can be a config failure, not a code
  failure** -- check the "feature survived" grep before reading
  compiler errors.

### What We Decided (session 74)

- **Dense per-block slot table for zcell** (the user's call, after
  "computing a ptr from index is way cheaper than a ram lookup"):
  block_nr indexes a preallocated `struct zcell_slot` embedding the
  loc node; no xarray, no per-block kmalloc, no entry pointer.
  pack_loc stays as the (single, irreducible) data indirection.
- **Engine extraction committed first** (pure refactor, gate-green)
  so the device rewrite is a separate bisectable step.

### Files Changed (session 74)

| File | Purpose |
|------|---------|
| `include/linux/shmem_compress.h` | +`struct shmem_comp_loc_node`, entry embeds it, +loc API decls |
| `mm/shmem_compress.c` | field renames, loc_link/dispose/pin_put, node-based drain + loc_load/loc_read cores |
| `~/tmpfs-compress-test/scripts/build_i386.sh` | ZRAM-not-ZSMALLOC fix, +zcell.o (harness 9ef1cef) |

(Kernel commit `f6cc4dd096c1`; i386 gate 0/0 on all 5 objects.)

### Next Steps (session 74)

1. Commit 2: the zcell dense slot table -- slot struct, install/
   drop/read/reset protocols per the design above, drop
   entry_alloc_local usage (then remove it if unused).
2. i386 gate + boot test (round-trip + swap integrity) + swapbench
   A/B vs the session-73 baseline (0.85x in / 1.14x out).
3. Log session 75 with the numbers.

---

## Session 75: Dense Slots Landed, Correctness Pass Launched, the Config Slash (2026-10-17)

(Intermediate append -- the full-matrix correctness pass is still
running; results below are build-phase only.)

### What We Did

**1. Commit `0fe6a709c80b` -- the dense slot table (the session-74
design, implemented).**  `struct zcell_slot { loc; present; lock }`
in a kvcalloc'd array indexed by block_nr, allocated at disksize=.
The generation protocol exactly as designed: install waits for EMPTY
(present==false && refs==0 && handle==0); drop hides under the slot
lock then drops the durable ref (no-pin case frees synchronously --
cells to buddy inline); the last put disposes and clears the handle;
GC takes no slot lock and never touches present (no resurrection);
reader pins lockless (present, inc_not_zero, present-recheck,
spurious pin put+retry); reset gained a pin-drain wait before
pool_put, closing the old entry-based reset's late-entry_put window.
Dead API removed with it (zcell was the only consumer):
`shmem_comp_entry_alloc_local()`, `shmem_comp_entry_get()`,
`shmem_comp_read_entry()`; the doc moved to `shmem_comp_loc_read()`.
i386 gate 0/0 on all five objects.

**2. Full-matrix correctness pass written and launched**
(harness `1e1384e`, `scripts/dense_correctness_pass.sh`): pristine
worktree at HEAD -> all 8 arch guests+initramfs rebuilt -> 10 sweep
kernels (ZCELL=y) -> arch battery on 8 subs -> x86_64 KASAN suite
(default + zcell boot + trunc-resurrect + entry-race) -> the five
ztmpfs config sweeps.  Sequential, per-step logs, summary table.
Guests/initramfs all green; i386 + i386-pae kernels green.

**3. arm builds died -- diagnosed as tree-tip defconfig breakage,
not our code.**  Both failed at MODPOST (after vmlinux linked):
arm32/multi_v7 = cros_ec modules with unresolved symbols,
arm64 = ath11k_pci (Qualcomm WiFi) same class.  The mm/ code built
fine on both.  User interrupted the pass mid-arm64 ("super long")
and called the config slash.

**4. The config slash (harness `a3d6b1c`).**  The sweep kernels are
QEMU -kernel + initrd + serial and NOTHING else, so the builder's
fixed block now disables everything the runners cannot reach --
uniformly on every arch:
- CONFIG_MODULES=n -- kills the modpost failure class AND ~1537
  arm64 / 958 arm32 module builds (the "super long" was mostly this)
- NET=n (no net devices; cascades wifi/bt/netfilter), SOUND, DRM/FB/
  LOGO, MEDIA_SUPPORT, USB_SUPPORT, SCSI/ATA/NVMe/MD/dm, MMC, MTD,
  IIO/HWMON/THERMAL/HW-watchdog, loop/ramdisk, every filesystem
  except tmpfs (user: "add several fs drivers later if needed"), input
  fat, EFI+EFI_STUB (all archs now, was x86-only), IOMMUs; ACPI off
  on DT-booted archs (arm64/riscv/mips), kept on x86 pc.
- Feature set UNIFORM for every arch: ZRAM + ZCELL + SWAP +
  ADVISE_SYSCALLS + TMPFS_COMPRESSION in the fixed block -- every
  arch can run every zram/ztmpfs/zcell test (the user's consistency
  requirement).  New hard gates both directions: the quartet must be
  =y; MODULES/NET must not resurface (the slash cannot silently rot
  against a defconfig change).
- Caller flags still apply AFTER the slash -> any disabled driver/fs
  can be re-enabled per experiment without editing the block.

**5. Slash validated on the two broken/slow archs** (clean O= dirs):
arm32 rc=0 in 4m49s (was 12+ min, dead); arm64-4k rc=0 in 8m7s (was
22+ min, dead).  i386 probe: 1456 -> 1040 =y (-29%), 14 -> 0 =m.

### What We Learned (session 75)

### Technical

- **A modpost failure after a successful vmlinux link is a config
  smell, not a code smell** -- at tree tip, defconfig-selected module
  sets break against each other (cros_ec, ath11k).  For a kernel
  that can never load a module, CONFIG_MODULES=n is both the fix and
  a large build-time cut.
- **MODULES=n is the biggest single lever in the whole slash**: the
  arm64 build's module forest (1537 =m) dwarfed everything else and
  was the "super long" build the user killed.
- **The dense-slot protocol survived implementation exactly as
  designed on paper** -- no redesign was needed during coding; the
  gate passed first try after the two mechanical fixes (node-typed
  drain field accesses, and a whitespace-mangled anchor).

### Process

- **Correctness before perf, explicitly** (user: "performance
  without correctness is futile"): the swapbench A/B is queued
  BEHIND the matrix pass, not run alongside it.  The session-71
  entry-table kernel artifact (build/arch/i386zcell/kernel) is
  preserved untouched as the future A/B control.
- **Diagnose the failure class before slashing** -- the slash was
  justified by reading the modpost errors first; a blind slash would
  have masked whether OUR build was broken on arm.
- The pass orchestrator's design (per-step logs, failures don't
  abort the rest, summary table) paid off immediately: three
  independent failures were classifiable from two log files without
  re-running anything.

### What We Decided (session 75)

- **QEMU-only slash, brutal but uniform** (user): the matrix kernels
  carry zero hardware surface beyond the QEMU machines; SBC/real
  kernels get their own configs later; disabled things return per
  need via caller flags.
- **Uniform zram/ztmpfs/zcell feature set across all archs**,
  enforced by gates.
- **A/B deferred until the matrix is green.**

### Files Changed (session 75)

| File | Purpose |
|------|---------|
| `mm/zcell.c` (linux, `0fe6a709c80b`) | dense slot table + generation protocol; xa/entry/RCU path removed |
| `mm/shmem_compress.c`, `include/linux/shmem_compress.h` | dead entry-front-end API removed; doc moved to loc_read |
| rig `scripts/dense_correctness_pass.sh` (`1e1384e`) | the full-matrix pass orchestrator |
| rig `scripts/build_arch_kernel.sh` (`a3d6b1c`) | the slash + uniform features + gates |

### Test Results / Benchmarks (session 75)

- i386 compile gate: 0 errors 0 warnings (5 objects) at `0fe6a709c80b`.
- Correctness pass so far: guests+initramfs 8/8 PASS; kernels i386,
  i386-pae PASS; arm32/arm64-4k PASS post-slash (4m49s / 8m7s);
  battery/suite/sweeps NOT YET RUN.
- Config shrink (i386 probe): 1456 -> 1040 =y; modules 0.
- No perf numbers this session; swapbench A/B pending.

### Next Steps (session 75)

1. Relaunch the full pass (kernels phase onward) with the slashed
   builder: remaining kernels = arm64-16k/64k, riscv64, mips32-be,
   x86_64 plain + KASAN; then the arch battery, the x86_64 KASAN
   suite (incl. test.zcell boot on the dense device), the config
   sweeps.
2. Triage any arch failures from the serial signatures in
   out/dense_pass/.
3. Only then: the swapbench A/B vs the preserved entry-table kernel
   (dense vs entry, zram as in-kernel control).

---

## Session 76: The Hang Hunt -- Rig Lies, One Leaked Pin, and Two Rig Hardening Rounds (2026-10-17)

(Intermediate append; the matrix pass's final sweep -- matrix_watermark
-- was still running at write time.)

### What We Did

**1. The full-matrix pass ran and mostly cleared.**  Guests+initramfs
8/8; kernels 10/10 after the config slash; arch battery 8/8 PASS
(36/36 checks each -- i386, i386-pae/PAE-highmem, arm32,
arm64-4k/16k/64k, riscv64, mips32-be; every result re-verified as
authentic, not runner-reported); x86_64 KASAN default suite PASS
(verified "Test exit code: 0"); four of five ztmpfs config sweeps
PASS; matrix_watermark pending.

**2. The runner was producing FALSE PASSES -- two mechanisms, both
fixed.**
- run_qemu.sh captured `$?` after `qemu ... | tee LOG`: that is
  TEE's exit status.  A host-killed or crashed guest -> tee exits 0
  -> exit 0 -> PASS.  My manual kill of the wedged zcell-boot QEMU
  turned a hang into "PASS suite-zcell-boot" in the summary -- caught
  because the serial log had no GUEST_RESULT at all.  Fixed:
  EXIT_CODE=${PIPESTATUS[0]}.
- A panic=1 guest reboots after panic; -no-reboot makes QEMU exit 0
  on that reboot -- so a panic would ALSO read as PASS.  Fixed: the
  runner greps "Kernel panic" in the serial log and fails explicitly.
- Hardening: both runners now boot with panic=1 oops=panic
  hung_task_panic=1 softlockup_panic=1 -- a wedged guest can never
  outlive the run, and the full multi-task hung-task report lands in
  the serial log instead of being killed blind.  All runner edits
  atomic-mv'd so in-flight steps kept their open inodes.
- (Self-inflicted, logged: `pkill -f qemu-system-x86_64` matched my
  OWN shell's command line and killed the tool session; use
  pgrep -x / PID loops.)

**3. The zcell boot test wedged -- reproducibly, and invisibly to
every detector.**  Round-trip green, then silence at the reset/swap
trial, with only "clocksource: Watchdog remote CPU N read timed
out".  No hung-task (the waiters are SCHED_NORMAL), no softlockup
(prehemptible cpu_relax spin).  Outer timeout was the only exit.

**4. Diagnostic commit `2fd08efa8b0d`: bounded waits as
assertions.**  The device's wait loops (read pin-retry, write
drain-wait, reset pin-drain) now bound at 10s: print
slot/present/refs/handle and panic -- with panic=1 the guest reboots
and the serial log NAMES the stuck generation.  Principle recorded:
when code waits for an invariant, write the wait as an assertion,
not as an open-ended spin.  (The reset drain was a leak oracle all
along; it had been written as a silent loop.)

**5. The bug: `b149fcd1a1c8` -- the dense read path leaked a pin.**
zcell_read_block()'s pinned path returned shmem_comp_loc_read()
directly and never called zcell_slot_put(): every successful block
read leaked one refs count.  The round-trip leaked 64; reset dropped
the durable refs and the drain loop then spun forever on refs==1.
Reproduced on BOTH the plain and KASAN kernels (not KASAN-specific;
KASAN only changed where the test stalled).  The regression entered
with the dense rewrite: the old code was linear (read; put; return);
the goto-restructure lost the cleanup on the new exit.

**6. Fixed and verified end to end.**  i386 gate 0/0; rebuilt both
x86_64 kernels; test.zcell on plain AND KASAN: 13/13 PASS both --
swap writes hit the device, SwapFree exhausted (device-full churn),
swapped data read back INTACT, cells returned to buddy after swapoff,
guest exit 0.  i386 swapbench (pre-fix kernel) had already PASSED --
the leak is invisible without a reset-wait (swapbench never resets),
which is exactly why the boot test caught it and the bench did not.

### What We Learned (session 76)

### Technical

- **A schedulable spin defeats every kernel hang detector.**  A
  task looping in cpu_relax() with preemption enabled is neither
  D-state (hung_task) nor a soft lockup -- it just never finishes.
  Silent forever.  Bounded waits that panic are the only loud
  failure mode for that class.
- **The leak oracle existed and I had written it as a silent loop.**
  The reset drain asserting refs==0 would have named the leaking
  slot on the very first boot test.
- **Why the bench could not see it**: swapbench never resets, so the
  leaked pins only delayed frees into spare RAM.  A test that waits
  for an invariant (reset drain) is a different -- and here,
  decisive -- class of check than a test that measures throughput.
- **tee-in-a-pipeline eats exit codes.**  Any `cmd | tee log` must
  read PIPESTATUS, or crashes become successes.
- The rare trres one-off hang (Phase B merge=8, one occurrence in
  two runs, tmpfs-only) is NOT this bug -- separate race, still open.

### Process

- **"Performance without correctness is futile" (user) ordered the
  whole session**: the A/B stayed queued while the matrix and the
  device test were made honest.  Good thing: the runner itself was
  fabricating passes.
- **Verify runner-reported results against the serial logs.**  Every
  PASS tonight was re-checked for GUEST_RESULT/Test exit code in the
  actual log; two "PASSes" were false and one hang was hiding.
- **Foreground over background** (user): background builds/runs
  caused avoidable bookkeeping (lost processes, a self-inflicted
  pkill).  The host compiles fast enough that fg is cheap.
- Code-review lesson (user question, answered in commit): make leaks
  structurally hard -- callee-consumes-the-pin APIs, resource pairs
  named and never split by a goto, invariant waits written as
  assertions.  Review the boring glue as hard as the novel protocol.

### Files Changed (session 76)

| File | Purpose |
|------|---------|
| rig `scripts/run_qemu.sh` | PIPESTATUS, panic-discriminator, panic= cmdline (atomic mv x3) |
| rig `scripts/run_arch.sh` | panic= cmdline (atomic mv) |
| `mm/zcell.c` (`2fd08ef`) | bounded slot waits -> dump+panic |
| `mm/zcell.c` (`b149fcd`) | the fix: zcell_slot_put after loc_read; bounded reset drain |

### Test Results (session 76)

- Batteries 8/8 PASS (36/36 each, verified); KASAN default suite PASS
  (verified); trres + entryrace re-runs PASS (verified); 4/5 sweeps
  PASS; matrix_watermark pending.
- test.zcell pre-fix: hang on plain AND KASAN (reproduced, diagnosed).
- test.zcell POST-FIX: 13/13 PASS on plain and KASAN -- integrity +
  cells-to-buddy at device-full churn, exit 0 both.
- i386 swapbench: PASS (pre-fix kernel).

### Next Steps (session 76)

1. matrix_watermark result; then the whole pass rerun at b149fcd?
   (batteries are tmpfs-only; the fix touches zcell.c alone -- the
   zcell test + swapbench are its gates, both green.  Optional.)
2. The swapbench A/B (dense vs entry) is now UNLOCKED; rerun the
   fixed i386 kernel against the preserved entry baseline.
3. The rare trres Phase-B race: loop test.trres=1 N times on the
   KASAN kernel to characterize.
4. mTHP folio grouping (the differentiator) next on the feature
   track.

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

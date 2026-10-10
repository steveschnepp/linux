---
name: ftl-open-cell-buffer
description: FTL-style open-cell write-gathering buffer for cellzram -- replaces the parked split's per-write RMW with gather-and-close; a design/spec log, nothing built yet
project: linux
---

# Mission Log: The Open-Cell Write-Gathering Buffer

Part 18 of the tmpfs transparent-compression series.  Session 34
(2026-10-10), **design only** -- nothing is implemented.  This log IS
the spec: the build follows it in bisectable steps.

Session 33 parked the logical/physical split because its payoff did not
exist on a realistic workload: finishing the read-modify-write core
would have cost ~200 correctness-critical lines to save 936 bytes.  The
pivot log named the missing piece in passing -- the split ported "one
compressed blob per N pages" (piece 1) without the uncompressed
write-absorbing buffer that makes it cheap (piece 2), which tmpfs gets
free from its page cache.  This session designs piece 2, modeled on the
NAND FTL's open-block list, and in doing so revives the split.

Kernel commits: none (design session).
Rig commits: none.

## What We Did

1. **Re-read the two write paths to ground the asymmetry.**  tmpfs
   writes land in the page cache uncompressed (`shmem.c`, after
   `folio_unlock`); compression runs at reclaim or on the aging pass.
   cellzram (`cellzram.c`) is a block device with no page cache, so
   every 4K bio write hits the compressed representation directly.
   That is the whole efficiency gap: tmpfs has an uncompressed buffer
   absorbing writes, cellzram does not.

2. **Confirmed the storage/grouping machinery the buffer needs already
   exists.**  The buffer is the only new thing:
   - `shmem_comp_compress(pool, src, slen, dst, &len)` takes `slen` as a
     merge multiple, so it compresses a whole unit in one call.
   - `shmem_comp_store(pool, data, len)` on a **cell** pool stores
     `len > PAGE_SIZE` as ONE bump region and one handle.  No
     `store_merged` (that is the zsmalloc-only chunked path).
   - `shmem_comp_entry_alloc(..., nr_pages=N, NULL, 0, NULL)` already
     represents N pages as one entry -- exactly how a tmpfs merge group
     is stored today.
   - `shmem_comp_entry_sector_set/clear()` (the parked step-4a
     accessors) flip a discard bit on a closed unit with no recompress.
   - `shmem_comp_load_entry` + `shmem_comp_decompress` read a closed
     unit back.

3. **Modeled the buffer on the NAND FTL open-block list.**  A zcell
   (compression unit) is "open" while an uncompressed buffer of
   `physical` bytes stages its writes; closing compresses the buffer
   into one blob and stores it in a cell region.  Writes to an open
   unit are memcpy-fast; a write to a closed unit opens it (one
   decompress) instead of paying decompress-recompress per write.

4. **Locked the three policy forks** with the user before writing the
   design down: **sync close** (for now), **open-count as a module
   param, default chosen later by the bench**, and **buffer first, the
   compressible workload measures it**.

## The Design (this log is the spec)

### The FTL mapping

| NAND FTL concept | cellzram gather buffer |
|---|---|
| block | zcell = compression unit = `physical`, N logical blocks (`cellzram_unit_of`) |
| page program | logical 4K write |
| open block | open-cell buffer: `physical` uncompressed bytes + N-bit valid map |
| open-block list (bounded) | open-unit set, LRU, bounded by `open_max` |
| close / seal block | compress buffer -> one blob -> `cell_store` -> entry + bitmap |
| read an open page | copy from buffer (no decompress; faster than today) |
| partial-page program (RMW) | eliminated -- the buffer absorbs it |

The split's step-4 RMW core is **replaced**, not built.  Opening a
closed unit for a partial write costs one decompress (the "read" of
RMW, paid once per open/close cycle), not a decompress+recompress per
write.

### Data structures

```c
/* One open compression unit: an uncompressed staging buffer. */
struct cz_open_unit {
	unsigned long		unit_nr;	/* block-table key */
	void			*buf;		/* `physical` bytes, uncompressed */
	unsigned long		*valid;		/* ratio bits: live logical blocks */
	struct list_head	lru;		/* MRU at head, victim at tail */
	refcount_t		refs;		/* read pins; close drains these */
	bool			closing;	/* set by close; new readers retry */
	struct rcu_head		rcu;
};
```

The device holds only eviction bookkeeping -- **no second lookup
structure**:

```c
struct cellzram {
	/* ... existing fields ... */
	struct list_head	open_lru;	/* staged units, MRU at head */
	unsigned int		open_count;	/* staged units */
	unsigned int		open_max;	/* module param open_units */
	spinlock_t		open_lock;	/* protects open_lru, open_count, refs */
};
```

The **block table is the single lookup** (session 31's principle, now
literal).  A slot is one of four things:

| table slot | meaning |
|---|---|
| absent | never written (reads zeros) |
| `xa_mk_value(0)` | zero sentinel (reads zeros) |
| `xa_mk_value(id)` | closed unit, blob in a cell region |
| `xa_mk_pointer(cz_open_unit)` | staged unit (open or closing) |

A `struct cz_open_unit *` stored via `xa_store` is an untagged pointer
entry, so `xa_is_value()` is false for it and the existing value
dispatch cleanly separates "closed/sentinel" (value) from "staged"
(pointer).  Reads, writes, discards, and the `mm_stat` walk all do one
table load and switch on that.

### The paths

- **write(block)**: `unit = cellzram_unit_of(block)`.  Load the table
  slot.  Pointer and `!closing` -> copy into `buf` at the in-unit
  offset, set the valid bit, done (no compression).  Pointer and
  `closing` -> the unit is mid-close; retry the write against the table
  (hazard 1 below).  Value or absent -> `open_unit(unit, block, data)`.
- **open(unit, block, data)**: if `open_count == open_max`, `close(LRU
  tail)` first.  Alloc a `physical` buffer and **seed it fully**:
  absent/sentinel -> `memset` zeros; closed blob -> one decompress of
  the entry; then apply `data` to `block`'s offset and set its valid
  bit.  **Publish last**: `xa_store(pointer)` atomically replaces the
  entry id with the buffer pointer (freeing the old entry, whose cell
  region is reclaimed once any concurrent reader's pin drops).  Only
  after that store does the unit appear staged, so a reader never
  observes a half-seeded buffer.  A full-unit write may skip the
  decompress seed (the write fills every block); a partial write over a
  closed blob must seed from it.  Then link `open_lru`, `open_count++`.
- **read(block)**: `ent = xa_load(unit)`.  `xa_is_value(ent)` ->
  sentinel (zeros) or entry id (pin, decompress, put): today's path.
  Pointer -> `cz_open_unit`: pin the buffer (`refcount_get` under
  `open_lock`), copy the block, `refcount_put`.  No decompress, no
  device-wide lock held across the copy.
- **discard(block)**: staged -> clear the valid bit in the buffer (no
  recompress; if no valid bit remains, drop the unit).  Closed ->
  `shmem_comp_entry_sector_clear` bit-flip; erase the entry when no bit
  remains.  Sentinel -> nothing.  discard reads the table (pointer
  first) exactly as read does.
- **close(unit)**: (1) under `open_lock`: set `closing`, unlink
  `open_lru`, `open_count--`.  The table **still points at the buffer**
  -- this is what removes the invisible window.  (2) Drop `open_lock`;
  `compress(buf, physical)` -> `shmem_comp_store` (one cell region) ->
  `shmem_comp_entry_alloc(nr_pages=N)` + copy the valid bitmap.
  Incompressible -> store raw at `physical`.  `cell_store` failure ->
  **un-close** (clear `closing`, relink the LRU, keep the buffer -- it
  holds the only copy; never drop it on failure).  (3) `xa_store(entry
  id)` over the pointer (the atomic handoff), drain reader refs, free
  the buffer.
- **shrinker**: force-close every staged unit (they are uncompressed
  RAM) so the buffer yields under memory pressure.

Lifecycle:

```
              write to a closed/unwritten unit
   closed ------------------------------------> staged  (seed, then publish)
     ^                                            |
     |  close: compress -> store -> xa_store(id)  |  evict LRU / shrinker
     +--------------------------------------------+
```

### The read path and the no-invisible-window invariant

The invariant that makes reads correct and simple:

> **At every instant a unit's table slot points at valid data, or is
> absent (meaning zeros).  Never "in between."**

This rules out the two failure modes the two-structure draft had:

- **No precedence race.**  With a separate `open` xarray consulted
  first, a read had to check `open` then `table` and pick a winner; a
  unit mid-open or mid-close could be momentarily in neither, and the
  read returned zeros for written data.  One table means one load and
  one dispatch.

- **No invisible window across close.**  close does not clear the slot
  before compressing; it flips the unit to `closing` but leaves the
  pointer, so a read arriving mid-close still finds the buffer, pins
  it, and copies.  `xa_store(entry id)` is the single atomic handoff;
  only after it does the decompress path take over.  The refcount drain
  before `kfree(buf)` finishes any reader that pinned during the
  handoff -- the same protocol the entry pin already uses against
  `xa_exchange` on the write path.

- **Reads of a staged unit are faster than today**, not slower: a memcpy
  from the uncompressed buffer, no crypto.  Only a read of a `closing`
  unit waits behind the refcount drain, and only for one copy's duration.

### Locking and the correctness-critical core

`open_lock` is a **spinlock held only for the table-pointer / LRU /
refcount manipulation** -- never across the sleepable compress+store
(`cell_store` -> `alloc_pages` may sleep; a spinlock across it is
illegal).  close runs its compress+store **outside** `open_lock`,
having already marked the unit `closing` so eviction and writes leave it
alone.  `open_lock` is a leaf with respect to `cp->lock` (dropped before
`cell_store` takes it); nothing under `cp->lock` touches the staged set.

Three hazards are the correctness-critical core -- exactly the lines the
split's RMW would have been -- and are where review and concurrent-io
tests must concentrate:

1. **write vs close.**  A write that loads a `closing` pointer must not
   mutate the buffer close is compressing (a torn write would land in
   the blob).  The first landing resolves it by **retry**: re-load the
   table; once close installs the entry id, open the unit fresh.
   Deliberately *not* a `complete()` on the unit's own memory -- close
   frees the `cz_open_unit` right after signalling, and a waiter inside
   `wait_for_completion()` on freed memory is a use-after-free.  If the
   bench shows the retry livelocks under a hot unit, the fix is a
   per-unit completion owned by the table entry, not the buffer.

2. **reset/remove vs close.**  A shrinker- or `gc_now`-initiated close
   can be mid-`xa_store` while reset walks the table erasing slots.
   reset already freezes the queue and takes `io_lock`; close must take
   `io_lock` around its step-3 `xa_store` (or reset must drain `closing`
   units first) so the install cannot race the erase and leak a cell
   region.  A new place `io_lock` is required; state it in the close
   comment when built.

3. **close failure.**  If `cell_store` fails, close must `un-close`
   (clear `closing`, relink the LRU) rather than free the buffer: the
   buffer is the only copy of acknowledged writes.  Dropping it loses
   data; a retry (or the shrinker) re-attempts the close later.

Read pinning is the one familiar part: pin the buffer under `open_lock`,
copy outside it, put -- the entry-pin protocol pointed at a buffer
instead of a cell region.

### The central trade (honest)

The buffer trades **random-write amplification for compressible-workload
ratio gains**.  A hot or sequential unit (untar) amortizes: one
decompress to open + one compress to close across N writes, and gets a
better ratio (cross-block context) plus a 16x smaller entry table.  A
scattered random 4K write to a cold unit pays open(decompress
physical) + write + evict(compress physical) -- *worse* than ratio-1
per-block storage.  This is exactly FTL behavior.  It means the
feature's payoff must be validated against the compressible-non-zero
workload (series open-question #1), not the churn gate, and that a
random-write workload is the expected regression to characterize.

### Accounting

Open-buffer RAM is uncompressed staging: `open_count * physical`.  It
is neither cell storage nor compressed data and must be reported
separately in `mm_stat` (a new field) so the vs-zram comparison stays
honest.  `orig_bytes` counts a written-but-open block via the table's
open-marker, so the walk stays exact.  At close the bytes move from
"open staging" to "cell phys + compressed"; the walk must not
double-count across the transition.

### Bisectable build ladder

Each step ends green at default (ratio 1), so a regression bisects to
the step that introduced it.

| # | step | default behaviour |
|---|------|-------------------|
| 1 | `struct cz_open_unit`, `open` xarray, LRU, `open_max` param (default 0 = off), open/close helpers -- all dormant | no-op |
| 2 | wire write/read/discard through the staged-table pointer entries, behind `ratio > 1 && open_max > 0` | no-op (ratio 1) |
| 3 | enable ratio > 1 (drop the step-4 rejection in `cellzram_init`); seed-on-open; close compresses the unit as one blob; validity bitmap live for closed-unit discard | no-op at default; new ratio>1 test |
| 4 | eviction (LRU close when full) + shrinker force-close + `mm_stat` open-staging field | no-op at default |
| 5 | measure: entry table, compression ratio, throughput vs zram, random-write amplification; pick the `open_max` default | n/a |

Steps 1-2 are dormant scaffolding at ratio 1, like the parked split.
Step 3 flips the feature on when configured.  Step 5 depends on the
compressible workload (open-question #1), which the user chose to build
*after* the buffer and use as its proof harness.

## What We Learned

### Technical

- **The grouping machinery already existed; only the write-absorbing
  buffer did not.**  Reusing `entry_alloc(nr_pages=N)`, one-region cell
  store, and the sector-bitmap accessors means the buffer adds no new
  storage format.  Confirming this by reading the call sites (not
  assuming) is what made the design a small delta instead of a new
  subsystem.

- **cells store a multi-page blob as ONE region; zsmalloc needs
  `store_merged`.**  The cell path never chunked, because a cell region
  is contiguous and up to 4 MiB.  This is why the buffer's close is a
  single `shmem_comp_store` and why the entry-table and ratio wins are
  available to cellzram for free once writes stop hitting the blob.

- **A spinlock cannot span `cell_store`.**  close must detach under
  `open_lock` and drop it before the sleepable compress+store.  Getting
  this structure wrong is a "sleep in atomic" bug, so the detach/store
  split is stated explicitly in the design rather than left to the
  implementer.

- **The buffer is the block device's missing page cache, and that is a
  known cost, not a surprise.**  tmpfs amortizes grouping via its page
  cache; cellzram must build a small bounded one.  The uncompressed
  staging RAM (`open_count * physical`) is the price, and it must be
  accounted separately or the vs-zram RAM comparison lies.

### Process

- **Model on a known system (NAND FTL) to inherit its policy vocabulary.**
  "Open block list, close-on-evict, bounded open count" is a design
  pattern with known failure modes (random-write amplification), so
  naming it FTL-style surfaces those modes before they are coded.

- **Design forks belong in a decision handshake, not a wall of text.**
  Sync-vs-async close, the buffer bound, and build order were settled
  with the user in three questions before the spec was written, so the
  spec records decisions instead of open alternatives.

- **The spec is a mission log when the journey is the artifact.**  No
  code changed; writing it as a dated log keeps the parked-split pivot,
  the buffer design, and the build ladder in one append-only place the
  series already reads in order.

## What We Decided

- **The open-cell buffer replaces the split's step-4 RMW core.**  The
  split's steps 1-4a stay parked and dormant; step 4 (RMW) is not built.
  The buffer is how ratio>1 becomes efficient, and it revives the split.

- **Sync close for now.**  The evicting write compresses the LRU buffer
  inline.  FTL-faithful, simple, one compress per N writes amortized.
  The write-latency spike is accepted; async close is a later measured
  refinement if the bench shows the spike matters.

- **The open-count is a module param (`open_units`), default chosen by
  the bench later.**  Modeled on `cell_order`/`cell_headroom_pct`: land
  the knob, sweep it, pick the default from data.  Default 0 (off)
  until measured.

- **Buffer first; the compressible-non-zero workload measures it.**  The
  workload the series has been missing becomes the harness that proves
  the buffer's ratio/entry-table win.  Risk accepted: building
  correctness-critical code before confirming the payoff on real data.

- **The block table is the single lookup** -- staged units are pointer
  entries in it, not a second xarray, so reads/writes/discards/`mm_stat`
  do one load and there is no precedence race.  The no-invisible-window
  invariant (a slot always points at valid data, or is absent = zeros)
  is what keeps reads correct across open and close.

- **A closed unit's discard is a bitmap bit-flip** (the parked step-4a
  accessors go live); an open unit's discard clears a valid bit in the
  buffer.  Neither recompresses.

## Rules Added

- **A spinlock never spans a possibly-sleeping store.**  Detach under
  the lock, drop it, then compress+store.  Applies to any bounded-buffer
  cache that closes by allocating.

- **Uncompressed staging RAM is accounted separately from cell storage.**
  Any write-gathering buffer must report its peak (`count * unit`) as
  its own figure, or the compression-vs-baseline RAM comparison is
  wrong.

- **Validate a grouping feature against the workload it is meant to
  win, and characterize the workload it is meant to lose.**  The buffer
  wins sequential/hot units and loses scattered random writes; measuring
  only the win reproduces session 33's mistake of building against a
  synthetic.

## What We'd Do Differently

- Would not have built the split's steps toward an RMW core before
  asking whether a write-absorbing buffer made RMW unnecessary.  The
  pivot (and this design) show the buffer was the real missing piece
  all along; the RMW path was a detour that cost a session to park.

- Would have reached for the FTL analogy at the start of session 33
  rather than mid-step-4.  Naming the mechanism "open blocks" up front
  would have produced this design before any code was written.

## Files Changed

None -- design-only session.  No kernel or rig code changed; the gates
were not re-run.

The build that follows this spec will touch:

| File | Action | Purpose |
|------|--------|---------|
| `mm/cellzram.c` | modify | `struct cz_open_unit`, `open` xarray + LRU + `open_lock`, `open_units` param; wire write/read/discard/close; drop the ratio>1 rejection (step 3); eviction + shrinker; `mm_stat` open-staging field |
| `include/linux/shmem_compress.h` | modify (likely) | open/close helpers if any cross into the pool; otherwise unchanged -- the buffer is device-local |
| `~/tmpfs-compress-test/tests/test_cellzram.c` | modify | ratio>1 correctness: open/write/read/discard round-trip, close-on-evict, concurrent io during close, zero-block + bitmap discard |
| `~/tmpfs-compress-test/tests/` (compressible workload) | create | real dataset (text/binaries/logs) measuring compression ratio vs zram -- the buffer's proof harness (series open-question #1) |

## Test Results / Benchmarks

None -- nothing implemented.  The bisectable ladder re-runs the standing
gates (cellzram, fsdev, churn, randio, torture) at each step and must
stay green at default.  Step 5 is where the ratio/entry-table and
random-write-amplification numbers land, and it is gated on the
compressible workload existing.

## Next Steps

1. **Build step 1**: `cz_open_unit`, `open` xarray, LRU, `open_units`
   param (default 0), open/close helpers -- dormant, green no-op.
2. **Steps 2-3**: wire the paths behind `ratio>1 && open_max>0`, then
   enable ratio>1 with seed-on-open and one-blob close.  Add the
   ratio>1 round-trip and concurrent-close tests.
3. **Step 4**: eviction + shrinker force-close + `mm_stat` open-staging
   accounting.
4. **Build the compressible-non-zero workload** (open-question #1) and
   use it as step 5's harness.
5. **Step 5**: measure entry table, compression ratio, throughput vs
   zram, and random-write amplification; sweep `open_units` and pick the
   default.

Open questions to resolve during the build:

- **`open_max` sizing.**  A small fixed count (8-16) is the FTL
  default; the bench picks it.  Does it need to track `cell_order`
  (buffer size) or the device's hot-unit working set?
- **Seed-on-open cost.**  Does the partial-write-open decompress amortize
  on the compressible workload?  The full-unit-write-skips-seed fast
  path is in; whether the decompress-on-partial-open dominates random
  workloads is the amplification to measure.
- **Does close need to be async after all?**  Only the bench's
  write-latency column can say; sync is the landing point.
- **`physical` default.**  The buffer only helps at ratio > 1, so a
  `physical_block_size` > PAGE_SIZE default becomes worth considering
  once the buffer proves out -- but the cell_order sweep and this knob
  interact (buffer RAM = `open_count * physical`) and must be swept
  together.

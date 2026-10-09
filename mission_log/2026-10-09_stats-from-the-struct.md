---
name: stats-from-the-struct
description: cellzram's mm_stat counters drifted on the zero-block path -- walking the block table on sysfs read instead (session 31)
project: linux
previous: 2026-10-09_bench-methodology-gap.md
---

# Mission Log: Stats From the Struct, Not the Counter

Part 15 of the tmpfs transparent-compression series.  Session 31
(2026-10-09).

Session 30 was a design brief: the rig's numbers cannot be trusted,
and the first implementation step was an accurate RAM sampler.  This
session executes the part that lives in the kernel -- making cellzram's
`mm_stat` fully accurate, walking the structs on the sysfs read rather
than maintaining counters that drift.

Kernel commit: `f15cc7a4360b` (cellzram: derive mm_stat from the block
table).  Rig commit: `cef2d7e` -> `c41894e` (zero-block accounting
regression test).

## Session 31: the stats had a real bug

### What We Did

1. **Audited every `mm_stat` counter against the state it claims to
   describe.**  Four atomics in `struct cellzram` (`orig_bytes`,
   `compr_bytes`, `objects`, `zero_blocks`) were maintained on the
   write path.  The block table (`cz->table`) is the authoritative
   record of what is written; the counters were a shadow of it, and
   the shadow had drifted.

2. **Found the drift, not by testing but by reading.**  The zero-block
   write path **erased the table slot** (so the block looked
   never-written) and *added* `orig_bytes`.  Consequences:

   - A repeated zero write to the same block finds `old == NULL`, so
     nothing is subtracted -- `orig_bytes` counts *writes*, not live
     blocks.  `orig_bytes += PAGE_SIZE` on every repeat, forever.
   - A data write over a zero block also finds `old == NULL` (the slot
     was erased), so the zero write's `+PAGE_SIZE` is never reclaimed:
     one live block, `orig_bytes` = 2 pages.
   - `zero_blocks` only ever increments; overwrite and discard never
     decrement it.

   ext4 metadata issues zero writes constantly -- this is not a rare
   path.  Every repeated zero write inflated the logical-bytes figure
   by one page.

3. **Made the block table the single source of truth.**  Zero blocks
   now hold a sentinel slot (`xa_mk_value(0)`; entry ids are never 0,
   guaranteed by `XA_LIMIT(1, UINT_MAX)` in `entry_alloc`) instead of
   erasing the slot.  The table is the complete picture of what is
   written.  Still no entry and no cell for a zero block -- the
   sentinel is an xarray value slot, not storage.

4. **Rewrote `mm_stat_show` as a table walk.**  `xa_for_each` under
   RCU; `io_lock` serialises against reset and remove (which free the
   pool -- this also closed a pre-existing window where the old code
   read `cz->pool` unlocked and could UAF against a concurrent reset).
   `objects` and `zero_blocks` are entry counts, `compr_bytes` is the
   sum of live entry lengths, `orig_bytes` is
   `(objects + zero_blocks) << PAGE_SHIFT`.  Exact by construction.

5. **Deleted the four atomics and every add/sub site.**  `put_entry`
   became `put_slot`: drop the reference, skip the sentinel.  Smaller
   code than what it replaced.

6. **Fixed an `xa_store` ENOMEM path on the way.**  The old code fed
   the `XA_ERROR` pointer to `xa_to_value()` and leaked the entry it
   had just allocated.  Now it frees the entry and returns the error.

7. **Added a regression test** (`test_zero_block_accounting`,
   `mm_stat8()` to reach fields 7-8) that drives every transition the
   old counters got wrong and holds the invariant
   `orig_bytes == (objects + zero_blocks) * PAGE` after each.

Nothing was implemented from session 30's RAM-sampler design (the
`Shmem` + `mount_stats` + `mm_stat` sampler).  This session is the
kernel-side prerequisite: `mm_stat` had to be accurate before a sampler
could trust it.

### What We Learned

#### Technical

- **Counters that shadow a table drift; the table does not.**  The
  rule from session 29 ("prefer cumulative counters over gauges")
  holds *within* a working design, but this was neither: it was a
  redundant second copy of state, and the two copies disagreed the
  first time a path updated one and not the other.  When the
  authoritative structure already exists and a read is rare, walking
  it beats maintaining a parallel copy.

- **The zero-block path was the whole bug.**  Every non-zero write
  (store, overwrite, discard) was correctly balanced -- `put_entry`
  subtracted exactly what the store added.  Only the path that *erased
  the slot instead of replacing it* broke the invariant, because
  "erased" and "never written" are indistinguishable to a counter.  A
  counter can only track state it can observe; the erase made the
  previous state unobservable.

- **`xa_mk_value(0)` is a usable sentinel** because entry ids come from
  `XA_LIMIT(1, UINT_MAX)` -- the entry allocator explicitly excludes 0
  as the failure return.  The sentinel costs one xarray value slot
  (~8 B amortized), not a cell.

- **`xa_store` returns an error pointer, and `xa_to_value` on it is
  garbage.**  The tree has no `xa_exchange`, so the replace-and-return-
  old pattern must check `IS_ERR` before touching the old value.  The
  old code did not, and would leak the entry on ENOMEM.

- **Reading `cz->pool` outside `io_lock` was a UAF against reset.**
  `reset_store` frees the pool and NULLs the pointer under `io_lock`;
  the old `mm_stat_show` read it with no lock.  The walk takes the
  lock, which fixes it as a side effect.  Found only because the
  rewrite needed the lock anyway -- it had been latent.

#### Process

- **"Ensure the stats are fully accurate" is a reading task before it
  is a writing task.**  The user's instruction to walk the structs
  pointed at the fix, but the *decision* to walk came from reading the
  counter updates and finding the unbalanced one.  The walk is the
  design; the drift is the justification.  Both were needed.

- **The regression test is what makes the fix a fix.**  A green gate
  before and after is not evidence the bug existed.  The new
  assertions fail on the old kernel by construction (the repeated-zero
  write reads `o_data + 2*PAGE` on the old counters), so the test
  proves the bug and locks the fix in one move.

- **Established numbers reproduce exactly across the change.**  The
  churn gate's `gc_recoveries` came back at 58 -- session 28/29's
  number for the same config.  The walk did not move a single
  established result; it only fixed the columns that were wrong.  When
  a measurement refactor is correct, the trustworthy columns are
  bit-identical afterward.

### What We Decided

- **`mm_stat` is a table walk, not counters.**  The four atomics
  (`orig_bytes`, `compr_bytes`, `objects`, `zero_blocks`) are gone.
  `io_stat`'s four counters stay: they are event counts (cumulative,
  one increment per completed op), which is the shape session 29
  endorsed -- they do not shadow a structure and cannot drift.

- **Zero blocks hold a sentinel slot, not "no slot".**  Still no entry
  and no cell; the storage win over zram's `same_pages` is preserved
  (a value slot, not a cell).  The table becomes the complete record.

- **The field layout is unchanged** (8 fields, same order), so every
  parser in the rig and the vs-zram head-to-head are unaffected.  On
  zero-free workloads the numbers are bit-identical (fsdev `distinct`:
  objects=16384, orig=67108864 = 16384 x 4096).

- **The walk's cost is accepted.**  O(live blocks) on a rare sysfs
  read.  The comment says why: correctness beats speed on this path,
  and the counters had already produced a wrong number in anger.

### Rules Added

- **Do not maintain a second copy of state that a structure already
  holds.**  A counter that shadows a table drifts the first time a path
  updates one and not the other.  Walk the structure on read when the
  read is rare.

- **A counter can only track state it can observe.**  If a path erases
  or replaces the record of the previous state, the counter can no
  longer see it and will go wrong.  Either keep the record (sentinel
  slot) or do not count the transition.

- **Check `xa_store`'s return for `IS_ERR` before using it as an old
  value.**  The error pointer is not a value; `xa_to_value` on it is
  garbage and the just-allocated entry leaks.

- **Any `->pool` deref in a sysfs read takes `io_lock`.**  Reset and
  remove free the pool under it; an unlocked read is a UAF.

- **A regression test must fail on the old code by construction.**  A
  green gate before and after proves nothing; the assertion has to be
  one the old implementation would have violated.

### What We'd Do Differently

- Would have walked the table from the start.  The counters were added
  incrementally as features landed (compression, then zero-blocks,
  then discard), and no step revisited whether the shadow still
  matched the structure.  A table-backed read would have been less
  code on day one.

- Would have caught the `cz->pool` UAF in review rather than by
  accident during a rewrite.  It was latent in every prior session
  that read `mm_stat` concurrently with a reset.

- Would not have built the counters on the zero-block path as an
  afterthought.  "Store nothing" is a storage optimization that broke
  an accounting invariant, and the two concerns were never separated.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | mm_stat from a block-table walk; zero-block sentinel slot; four atomics removed; `put_entry` -> `put_slot`; `xa_store` ENOMEM check; `io_lock` in `mm_stat_show` |
| `~/tmpfs-compress-test/tests/test_cellzram.c` | `mm_stat8()` (fields 7-8); `test_zero_block_accounting()` holds `orig_bytes == (objects + zero_blocks) * PAGE` across all zero-block transitions |

### Test Results

**cellzram** (`test.cellzram=1` boot): **106 passed, 0 failed** (was
77; +29 new assertions).  New test green on every transition:

```
[cellzram] mm_stat exact over zero-block transitions
  PASS zero write bumps zero_blocks, not objects
  PASS zero write bumps orig_bytes by one page
  PASS repeated zero write is idempotent in mm_stat
  PASS data-over-zero: objects +1, zero_blocks back
  PASS data-over-zero does not double-count orig_bytes
  PASS zero-over-data: zero_blocks +1, objects back
  PASS discard of a zero block decrements zero_blocks
```

**Standing gates, all green** (QEMU, one boot each):

| gate | result | standing |
|------|--------|----------|
| cellzram | 106/0 | 77/0 (+29 new) |
| randio | 4/0 | 4/0 |
| fsdev (blkdev ext4) | 19/0 | 19/0 |
| torture | 231/0 | 231/0 |
| churn | exact, exit 0 | exact |

**Established numbers reproduce exactly across the change.**  The
churn gate's raw `mm_stat` line:

```
BENCH raw_mmstat dev=/sys/block/cellzram0 line=8388608 8388608 10747904 2048 147456 10000 0 58
```

`objects=2048`, `orig=8388608` (= 2048 x 4096), `gc_recoveries=58` --
session 28/29's number for the same config.  The walk moved no
established result; it only fixed the columns that were wrong.

The fsdev `distinct` workload (zero-free by construction, xorshift
blocks) is bit-identical to the counter-based numbers:

```
BENCH mm_stat distinct dev=cellzram algo=zstd raw=67108864 67108864 67108864 16384 1179648 10000 0 0
```

`objects=16384`, `orig=67108864` = 16384 x 4096.

### Next Steps

1. **The RAM sampler from session 30** (`Shmem` + per-mount
   `mount_stats` + cellzram `mm_stat`, sampled per iteration, emitting
   mean/min/max and the full breakdown).  `mm_stat` is now accurate
   enough to be one of its three sources.  This is the next
   implementation step.
2. **Add median-of-N to the existing `BENCH` emitters** (three files,
   one helper) -- session 30's step 3, still open.
3. **Decide the fate of the four never-run test files** (`test_performance`,
   `test_compress`, `test_concurrent`, `test_mmap`): wire them in or
   delete them.
4. **`cell_order` x headroom** still unswept (sessions 26, 28, 29 all
   flagged it), and now with a method that can tell a sizing effect
   from a phase effect.
5. **The `xa_store` ENOMEM path is fixed but untested** -- an ENOMEM
   injection for the block-table store would confirm the entry is
   freed, not leaked.  Low priority; the path is rare and the fix is
   obviously correct on read.

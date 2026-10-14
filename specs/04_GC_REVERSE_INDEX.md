# Cell GC Reverse Index Specification

## Overview

Add a per-cell reverse index to the cell-backed compression pool so GC drains
a victim cell by walking only that cell's own entries, instead of scanning the
entire global entry table to find them. This turns the dominant GC cost from
O(total entries) per victim into O(entries in the victim), making cell GC
viable on multi-GiB tmpfs where the full-table scan is measured in seconds.

## Motivation

The cell GC (`shmem_comp_drain_victim()`) finds which entries reside in a
victim cell by `xa_for_each()` over the whole `shmem_comp_entries` table,
unpacking each handle and comparing cellid. That is **O(N)** where N is every
live compressed block in the pool -- for a handful of entries that actually
live in the victim. Measured shape: a 100 MB pool at ~2 KB blocks is N ~ 50K,
so one drain is ~50K iterations to relocate ~16 entries; a full reclaim of S
sparse cells is O(S/B x N). At 8 GiB (N ~ 1.9M) a single drain is seconds of
pure table-walking.

The scan dominates relocation by ~1000x (50K xarray iterations ~ ms, vs a 32
KB memcpy ~ us), so amortizing it is the whole game. Batching B victims per
scan (spec-discussed) reduces the multiplier to O(N/B) but leaves the O(N)
floor. The reverse index removes the floor entirely.

This is arm-3 improvement target "GC efficiency" from
`specs/03_ZTMPFS_ZCELL_FUSION.md`.

## Goals

- Drain a victim cell in O(entries in that cell), not O(total entries).
- Preserve every existing correctness property: reader safety under
  concurrent GC relocation, synchronous buddy return on empty, the
  slot-chaining convergence, and no entry ever lost (completeness).
- Keep the cell allocator (`mm/cell_store.c`) free of compression-layer
  knowledge -- the index maps *entries* to cells and lives in
  `mm/shmem_compress.c`.
- A/B measurable: the perf test compares index-on vs index-off on the same
  kernel to prove the win and locate the crossover.

## Non-Goals

- Not a batched GC. The reverse index subsumes batching's benefit (see
  "Batching is subsumed" below); the multi-victim window work is parked, not
  built here.
- Not a change to `mm/cell_store.c`. The index is a compression-layer
  structure; the allocator stays pure.
- Not a fix for the cell-size derivation (constant-K). The index is orthogonal
  to cell size and works at any order; the sizing refinement is a separate,
  adjacent change.
- Not a reverse index for the zsmalloc backend. zsmalloc has its own internal
  compaction; this is cell-only.

## Design

### Data structures

Two additions, both in `mm/shmem_compress.c`'s private structures:

```c
struct shmem_comp_entry {
	/* ... existing fields ... */
	struct list_head cell_node;	/* linkage in pool->cell_index[...] */
};

struct shmem_comp_pool {
	/* ... existing fields ... */
	struct list_head *cell_index;	/* array[nslots]: entry lists per cell */
	spinlock_t index_lock;		/* guards all cell_index list ops */
};
```

- `cell_index` is an array of `struct list_head`, one per directory slot
  (`cp->nslots`), allocated in `cell_pool_create`'s caller (pool create) and
  freed at pool destroy. Indexed by `cellid`.
- `index_lock` is a **single** per-pool spinlock guarding every `cell_index`
  list mutation and walk. One lock, not per-cell, because mutations are
  short (a `list_add`/`list_del`) and contended only between the rare GC pass
  and entry alloc/free -- not worth `nslots` locks.
- `cell_node` costs 16 bytes per entry (one `list_head`). This is the memory
  price of the index; see "Cost" below.

**Invariant (completeness):** every live cell-backed entry appears on exactly
one `cell_index[cellid]` list, and that cellid equals the cellid in its
`handle`. A live entry not on its cell's list is unreachable to GC -> the cell
never empties -> permanent leak. This invariant is what the whole design
protects, and what the torture test verifies.

### The three mutation points

The invariant holds because there are exactly three events that change an
entry's cell, and each maintains the list under `index_lock`:

**1. Alloc -- link onto the new cell's list.** In `shmem_comp_entry_alloc()`,
after `xa_alloc()` succeeds and the handle is known:

```c
if (pool->use_cells) {
	u32 cellid, offset;
	shmem_comp_unpack_loc(handle, &cellid, &offset);
	spin_lock(&pool->index_lock);
	list_add_tail(&ent->cell_node, &pool->cell_index[cellid]);
	spin_unlock(&pool->index_lock);
}
```

Runs with no other lock held (`entry_alloc` is called from the store path
after `cell_store()` has released `cp->lock`). `index_lock` is a leaf here.

**2. Free -- unlink before releasing storage.** In
`shmem_comp_entry_free()`, after `xa_erase()` and *before* `cell_free()`:

```c
if (ent->pool->use_cells) {
	u32 cellid, offset;
	shmem_comp_unpack_loc(ent->handle, &cellid, &offset);
	spin_lock(&ent->pool->index_lock);
	list_del_init(&ent->cell_node);	/* node is on cell_index[cellid] */
	spin_unlock(&ent->pool->index_lock);
}
```

Ordering is mandatory: **unlink, then `cell_free()`.** `cell_free()` may
return the cell to the buddy synchronously and the slot may be reused as a
new cell; a stale node left on `cell_index[cellid]` would then point into a
freed entry from a slot that now names different data. `entry_free` runs only
from the refs==0 path (one thread, guarded by `dec_and_test` + `xa_erase`),
so this unlink cannot race another unlink of the same node.

**3. Relocate -- move between lists.** In the GC drain (below), under
`index_lock`: `list_del_init` from the victim's list, then after relocation
`list_add_tail` onto the active cell's list.

### The drain (per-entry detach-relocate-relink)

Replaces the `xa_for_each` body of `shmem_comp_drain_victim()`. No bucket, no
restart -- one entry at a time, so `index_lock` is taken per entry (short,
uncontended):

```c
static unsigned int
shmem_comp_drain_victim(struct shmem_comp_pool *pool, struct cell *vic)
{
	unsigned int moved = 0;

	for (;;) {
		struct shmem_comp_entry *ent = NULL;
		unsigned long h;
		u32 vcellid, cellid, offset;
		unsigned int len;
		u32 nc, no;

		/* detach the first LIVE entry on the victim's list */
		spin_lock(&pool->index_lock);
		list_for_each_entry(ent, &pool->cell_index[vic->cellid],
				    cell_node) {
			if (atomic_inc_not_zero(&ent->refs)) {
				list_del_init(&ent->cell_node);
				break;
			}
			/* dying entry: leave it; its entry_free() unlinks it
			 * once we drop index_lock. Its own cell_free() does
			 * the live_bytes accounting -- GC need not touch it. */
			ent = NULL;
		}
		spin_unlock(&pool->index_lock);
		if (!ent)
			break;	/* no live entries remain in this victim */

		/* ent is pinned (refs >= 1) and detached: it cannot be freed
		 * and its handle cannot change under us (only GC relocates,
		 * and gc_lock serialises GC passes). */
		h = READ_ONCE(ent->handle);
		shmem_comp_unpack_loc(h, &cellid, &offset);
		/* cellid == vic->cellid by the completeness invariant */

		len = ent->length;
		if (cell_relocate_one(pool->cells, vic, vic->base + offset,
				      len, &nc, &no)) {
			/* relocate failed (dir full / too big): put the entry
			 * back on the victim's list so a later pass retries. */
			spin_lock(&pool->index_lock);
			list_add(&ent->cell_node,
				 &pool->cell_index[vic->cellid]);
			spin_unlock(&pool->index_lock);
			shmem_comp_entry_put_unpinned(ent); /* drop our pin */
			break;
		}
		WRITE_ONCE(ent->handle, shmem_comp_pack_loc(nc, no));
		moved++;

		/* relink onto the destination cell's list */
		spin_lock(&pool->index_lock);
		list_add_tail(&ent->cell_node, &pool->cell_index[nc]);
		spin_unlock(&pool->index_lock);

		/* drop our pin; may hit refs==0 -> entry_free() unlinks from
		 * the DESTINATION list (handle already updated) -- correct. */
		shmem_comp_entry_put_unpinned(ent);
	}
	return moved;
}
```

Notes that make this correct:

- **The pin is the safety, not the lock** (same as the current design).
  `atomic_inc_not_zero` under `index_lock` guarantees the entry stays alive
  (refs >= 1) through relocate, so `vic->base + offset` cannot be freed
  mid-memcpy (a pinned entry keeps `live_bytes > 0`, so the cell cannot be
  returned to the buddy).
- **A dying entry is skipped, not drained.** Its `entry_free()` unlinks it and
  its `cell_free()` decrements `live_bytes`, possibly emptying the cell --
  which `cell_pool_release_victim()` then returns. GC does not need to touch
  dying entries at all.
- **`shmem_comp_entry_put_unpinned()`** is a new helper: drop a pin taken by
  pointer (decrement refs, and if it hits 0 run the free path) without the
  `xa_load` that `entry_put(id)` does. Avoids a table lookup on the hot drain.
- **Relocate-failure puts the entry back** on the victim's list before
  dropping the pin, so nothing is stranded off-list.

### Lock ordering

`index_lock` is a **leaf lock**, taken only for a `list_add`/`list_del` or a
bounded list scan, and never across an allocation, `memcpy`, `cell_free()`, or
`cell_relocate_one()`:

- GC path: `gc_lock` (mutex) -> victim `down_write` -> `index_lock` (leaf).
- Alloc path: `index_lock` (leaf, no other locks held).
- Free path: `index_lock` (leaf, no other locks held; taken *before*
  `cell_free()`, which takes `cp->lock` and a cell `down_read`).

`index_lock` never nests inside itself and is never held when calling into
`cell_store.c`, so it cannot deadlock against `cp->lock` or a cell rwsem.

### Reader safety is unchanged

`shmem_comp_load_entry()`'s retry loop (read handle, `down_read` the cell,
re-read handle, retry if changed) is untouched. The reverse index changes only
how GC *finds* entries, not how readers validate them. A reader that read a
stale handle still detects the move and retries.

### Batching is subsumed

The earlier "batch B victims per scan" idea existed solely to amortize the
O(N) full-table scan. The reverse index removes that scan, so there is no
longer a per-victim O(N) cost to amortize -- B's only remaining benefit would
be amortizing the per-victim fixed overhead (`pick_victim` lock, a few list
walks), which is negligible. Therefore **batching is not built here**; the
single-victim sequential drain (`pick_victim` -> `drain` -> `release`) stays,
and the slot-chaining convergence logic in `cell_store.c` is untouched.

### Cost

| item | 64 MB pool | 8 GiB pool |
|---|---|---|
| entries (N, ~2 KB blocks) | ~32K | ~1.9M |
| index memory (`cell_node`, 16 B/entry) | ~0.5 MB (0.8% of RAM) | ~30 MB (0.4% of RAM) |
| `cell_index` array (`nslots` x 16 B) | ~64 KB | ~64 KB |
| drain cost per victim | O(entries in victim) | O(entries in victim) |

The 16 B/entry is the write-path price. It is always paid once built. Whether
to gate it on pool scale (skip the index for small pools where the O(N) scan
is already cheap and 0.8% RAM matters) is an open question the perf A/B
answers -- see Open Questions. The first cut builds it always-on for cell
pools to keep one drain code path.

### Store-path integration

`shmem_comp_store()` calls `cell_store()` (bump-alloc, returns `{cellid,
offset}`) *then* `entry_alloc()`. The index link happens in `entry_alloc`
(point 1), so a store that fails `entry_alloc` (returns 0) has already
reserved bump space via `cell_store()` but created no entry -- that space is
dead and reclaimed by GC as sparse. This is unchanged from today; the index
adds no new failure mode here because it links only after the entry exists.

### Stats

No new counters required for correctness. For the perf/validation story add
one gauge to `comp_stats`:

```c
unsigned long ram_index_bytes;	/* nslots * sizeof(list_head) */
```

The per-entry `cell_node` cost is already inside each entry's `ksize()` (it
is part of the struct), so `ram_entry_bytes` accounts it automatically.

## Build Changes

| Target | Change |
|---|---|
| `mm/shmem_compress.c` | add `cell_node` linkage, `cell_index` array + `index_lock`, thread the three mutation points, rewrite `drain_victim`, add `entry_put_unpinned` |
| `include/linux/shmem_compress.h` | add `ram_index_bytes` to `shmem_comp_stats` |
| `mm/cell_store.h` / `mm/cell_store.c` | **no change** |
| `mm/Makefile` | no change |

## File Manifest

| File | Action | Purpose |
|---|---|---|
| `specs/04_GC_REVERSE_INDEX.md` | Create | This spec |
| `mm/shmem_compress.c` | Modify | `cell_index` array, `index_lock`, `cell_node` in entry, alloc/free/relocate linkage, new `drain_victim`, `entry_put_unpinned` |
| `include/linux/shmem_compress.h` | Modify | `ram_index_bytes` stat field |
| `mm/shmem_compress.c` (sysfs) | Modify | expose `ram_index_bytes` in `comp_stats` |
| rig `tests/test_cell_gc.c` | Modify | completeness + race assertions under the index |
| rig `tests/test_cell_index_race.c` | Create | concurrent store/free/GC soak, KASAN |
| rig `tests/test_zbench.c` | Modify | GC-heavy mode + index on/off A/B |

## Migration

Each phase leaves the build green.

**Phase 1 -- structures + maintenance, index unused by GC.** Add `cell_node`,
`cell_index`, `index_lock`; wire alloc/free/relink. GC still uses the old
`xa_for_each` drain. Gate: existing `test_cell_gc` passes (the index is
maintained but not yet read -- proves maintenance alone is correct).

**Phase 2 -- reverse-index drain.** Rewrite `drain_victim` to walk
`cell_index`. Add `entry_put_unpinned`. Gate: `test_cell_gc` (sparse cells
gone after GC), `test_cell_storage`, `test_torture`, all CRC-clean on the
KASAN kernel. *This is the correctness-critical phase.*

**Phase 3 -- race soak.** New `test_cell_index_race`: N threads doing
concurrent overwrite/truncate on cell-backed files while `gc_now` fires in a
loop; KASAN + the completeness assertion (post-GC sparse count == 0, live
blocks still CRC-verify). *~the phase that would catch a missed unlink.*

**Phase 4 -- perf A/B.** A `cell_index=0|1` module/boot param (or a second
kernel) to run the old full-scan drain vs the reverse index. Bench a GC-heavy
churn at ~64 MB and ~2 GiB live sets; report drain time and reclaim wall
time. Locates the crossover and decides the auto-gate open question.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Missed unlink (a live entry not on any list) -> cell never empties -> silent leak | Med | High | Three mutation points are exhaustive (alloc/free/reloc); completeness asserted in torture (post-GC sparse==0); Phase 3 race soak under KASAN |
| Dangling `cell_node` (entry freed but node still linked) -> list walk UAF | Med | High | Free unlinks *before* `cell_free()`; unlink and free are same-thread (refs==0, dec_and_test); KASAN catches any residual |
| `index_lock` deadlock against `cp->lock` / cell rwsem | Low | High | `index_lock` is a leaf, never held across alloc/memcpy/`cell_free`/`cell_relocate_one`; lock order documented and asserted by lockdep |
| Double `list_del` of the same node | Low | High | `list_del_init` everywhere; a detached node (`INIT`) is never re-deleted; only the refs==0 thread frees |
| 16 B/entry overhead undercuts the small-RAM story at 64 MB | Med | Med | 0.8% at 64 MB; if the perf A/B shows the scan is cheap there, gate the index on pool scale (open question) |
| Relocate failure stranding an entry off-list | Low | Med | Failure path relinks to the victim's list before dropping the pin |

## Open Questions

- **Auto-gate on pool scale.** Below ~64K entries the O(N) scan is ~160 us
  and the index is pure 16 B/entry overhead. Should the index be built only
  above an entry-count threshold (two drain paths) or always-on (one path)?
  Decided by the Phase 4 A/B: if index-on is not slower at 64 MB, keep it
  always-on for simplicity.
- **`index_lock` granularity.** A single lock assumes alloc/free contention is
  rare. If the race soak shows it hot on many-core, move to a striped lock or
  per-cell locks. Deferred until measured.
- **Interaction with the constant-K cell sizing.** The index is orthogonal,
  but bigger cells mean fewer lists with more entries each; whether that
  changes the drain profile enough to matter is untested. Separate change.

## References

- `specs/03_ZTMPFS_ZCELL_FUSION.md` -- arm 3, the GC-efficiency improvement
  target this implements; the cell backend's design.
- `mm/shmem_compress.c` (feature branch) -- `shmem_comp_entry_alloc`,
  `shmem_comp_entry_free`, `shmem_comp_drain_victim`, `shmem_comp_cell_gc`,
  `shmem_comp_load_entry`: the five functions this change touches or preserves.
- `include/linux/cell_store.h` + `mm/cell_store.c` -- `struct cell`
  (`pages[1]`, one compound page), `cell_relocate_one`, `cell_free`
  synchronous return, `cell_pool_pick_victim`: the allocator contract the
  index sits on top of and must not violate.
- `mission_log/2026-10-11_arm3-cell-backend-measurement.md` -- arm 3 measured;
  the B-vs-C analysis and the 8 GiB scan-cost arithmetic that motivate the
  reverse index over batching.
- rig `tests/test_cell_gc.c` -- the existing GC gate (sparse-cells-before,
  gone-after) that validates completeness.

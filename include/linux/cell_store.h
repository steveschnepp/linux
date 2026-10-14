/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cell_store.h - Bump allocator for compressed data blocks
 *
 * A cell is a single physically-contiguous allocation used as a
 * bump-allocated arena.  Blocks are stored at exact sizes — there is
 * no per-object header, no size-class rounding, no free list.  This
 * matters because the blocks are already small (compressed pages or
 * merged groups) and every byte of metadata is a direct tax on the
 * memory savings that compression is supposed to deliver.
 *
 * Design invariants (maintainers must preserve all of these):
 *
 *   1. Only the active cell grows.  The bump pointer is monotonic;
 *      it is never decreased in place.  Holes left by freed blocks
 *      are reclaimed by evacuating the entire cell, not by reusing
 *      gaps.  This keeps allocation O(1) with no fragmentation
 *      analysis.
 *
 *   2. A cell whose live_bytes reaches zero is returned to the buddy
 *      allocator synchronously.  No background compaction, no lazy
 *      reclamation.  The caller of cell_free() can rely on the pages
 *      being available to the page allocator when it returns.
 *
 *   3. cell->base is virtually contiguous because the pages come from
 *      a single alloc_pages() call and the kernel direct map makes
 *      physically contiguous pages virtually contiguous on every
 *      architecture this code targets.  Any offset within the cell is
 *      reachable by pointer arithmetic; no kmap or bounce buffer is
 *      needed for reads or writes that span page boundaries.
 *
 *   4. The bump pointer only advances under cp->lock.  The memcpy
 *      that writes block data happens after the lock is released;
 *      callers must ensure the source buffer remains valid until the
 *      memcpy completes.  In the compression path this is guaranteed
 *      by the entry reference pin.
 *
 *   5. A cell carries no pool-wide state: the allocation order is
 *      cp->cell_order (identical for every cell in the pool) and the
 *      allocation target's identity is cp->active.  Both used to be
 *      cached per-cell as well; a shadow copy can drift from the
 *      authoritative pool field, and every reader already holds cp.
 *
 *   6. One slot beyond the advertised capacity belongs to compaction,
 *      not to a store.  cell_make_active() only lets a GC caller take
 *      it.  A drain relocates a sparse cell's blocks into a fresh
 *      cell, so on a completely full directory it has no destination
 *      unless one slot is held back -- without it, evacuating a full
 *      directory is a fixed point that moves nothing and frees
 *      nothing.  The slot is allocated ON TOP of max_cells
 *      (cp->nslots), so the advertised capacity is unchanged.
 */
#ifndef _LINUX_CELL_STORE_H
#define _LINUX_CELL_STORE_H

#include <linux/types.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>

/*
 * Cell size bounds.  Minimum 16 KiB: smaller cells cannot hold even a
 * single merged compression group (merge_pages * PAGE_SIZE) without
 * splitting, and the per-cell metadata (struct cell ≈ 128 B) becomes
 * a significant fraction of the cell itself.  Maximum 16 MiB: large
 * cells cut the directory/array size and the per-cell metadata cost
 * for big mounts.  A cell is one alloc_pages(order) call, so an order
 * above the buddy's max (MAX_PAGE_ORDER) needs
 * CONFIG_ARCH_FORCE_MAX_ORDER; cell_pool_create() refuses those
 * cleanly rather than build a pool that cannot allocate its cells.
 */
#define CELL_ORDER_MIN	2		/* 16 KiB  */
#define CELL_ORDER_MAX	12		/* 16 MiB  */
#define CELL_ORDER_DEF	4		/* 64 KiB  */

/*
 * Directory capacity for pools whose caller has no capacity bound.
 * A block device derives its cell count from disksize instead; this
 * default only bounds tmpfs page-cache pools, which are compressible
 * and size=-accounted.
 */
#define CELL_DEFAULT_MAX_CELLS	256

/*
 * Upper bound on a tmpfs pool's cell directory.  The directory is sized
 * from the mount's size= (bigger mount -> bigger array) so the pool can
 * hold the compressed image of the whole filesystem; without this the
 * legacy 256-cell default caps storage at 256 x cell_size (~16 MiB with
 * 64 KiB cells), which is why a large cell-backed fill ran out of cells.
 * The cap bounds the directory metadata (a slot is a struct cell, ~100 B)
 * for absurdly large mounts with tiny cells -- which the size-based
 * cell-order derivation avoids in practice by growing the cell instead.
 */
#define SHMEM_COMP_CELL_MAX_CELLS	(2U << 20)	/* 2 Mi cells */

/*
 * A cell is a GC candidate when it is below gc_percent byte
 * utilization: live_bytes * 100 <= cell_size * gc_percent.  Byte
 * utilization, not object count -- twelve 2 KB blocks in a 64 KiB
 * cell is 75% waste whether we call that "a few objects" or not, and
 * evacuating returns a whole cell to the buddy for the price of
 * twelve memcpys.  Default 50 (the historical threshold); per-mount
 * via gc_percent=, clamped 10..90 at parse.  The active cell is never
 * a candidate (it takes new allocations), so every pass strictly
 * reduces memory held.
 */
#define CELL_IS_SPARSE_PERCENT_DEFAULT	50U

#define CELL_IS_SPARSE(live_bytes, cell_size) \
	((live_bytes) * 2 <= (cell_size))

/*
 * Upper bound on victims drained by one compaction pass.  A pass
 * drains SEQUENTIALLY -- each emptied victim funds the directory slot
 * the next drain needs -- so this bounds the pass rather than
 * batching it.  64 is generous for the workloads seen so far (the
 * churn gate converges in a handful of drains) and keeps a single
 * shrinker invocation from scanning the entry table indefinitely.
 */
#define GC_MAX_VICTIMS	64

struct cell {
	/*
	 * Exactly one slot: a cell is one physically-contiguous compound
	 * page (a single alloc_pages(order) call), so the head page
	 * reference describes the whole allocation -- page_address() for
	 * the data, __free_pages(head, order) to release it.  No tail
	 * page needs its own slot; the head is stored so the free path
	 * can find it.
	 */
	struct page	*pages[1];
	void		*base;		/* virtual base = page_address(pages[0]) */
	u32		next_free;	/* bump offset; monotonic during cell life */
	u32		live_bytes;	/* sum of live block lengths */
	u32		live_objects;	/* count of live blocks */
	u32		cellid;		/* index into pool->cells[] */
	/*
	 * Held for read during block access, for write during
	 * compaction.  Compaction must not run concurrently with a
	 * reader that is about to memcpy from a cell being evacuated.
	 */
	struct rw_semaphore lock;
	struct list_head node;		/* linkage in pool->sparse list */
};

struct cell_pool {
	/*
	 * Protects: active pointer, the cell directory (including
	 * free-slot reuse), every cell's next_free/live_bytes/
	 * live_objects/active/node fields, and the sparse list.  Lock
	 * order: a cell's rwsem is always taken BEFORE cp->lock, never
	 * after — GC holds down_write(victim) and then takes cp->lock
	 * for bump allocation; cell_free takes down_read when it might
	 * return a cell, then cp->lock for the recheck.
	 */
	spinlock_t	lock;
	/*
	 * Serialises GC passes.  Without it two shrinker scans could
	 * relocate the same object concurrently and corrupt accounting.
	 */
	struct mutex	gc_lock;
	struct cell	*active;	/* current allocation target, or NULL */
	struct cell	*cells;		/* directory array, indexed by cellid */
	u32		nr_cells;	/* high-water mark of directory use */
	u32		max_cells;	/* advertised directory capacity */
	/*
	 * Allocated directory slots = max_cells + 1.  The extra slot is
	 * COMPACTION-ONLY: a normal store may never take it.  A drain
	 * needs a destination before its victim can be emptied, and the
	 * emptied victim is what frees the slot -- so without a slot held
	 * back the pass has no bootstrap and stalls exactly when the
	 * directory is full.  Unlike a "stop one short" reservation, this
	 * costs the caller nothing: stores can still fill all max_cells.
	 */
	u32		nslots;
	u8		cell_order;	/* alloc_pages order, same for all cells */
	unsigned int	gc_percent;	/* sparse threshold: byte-utilization
					 * percent (mount gc_percent=, default 50) */
	bool		use_thp;	/* request THP-backed pages */
	struct list_head sparse;	/* cells below gc_percent utilization */
	atomic_long_t	pages_used;	/* (1 << cell_order) * live cells */
	/*
	 * Registered by the compression layer when the pool is cell-
	 * backed; NULL otherwise.  The shrinker is the async GC trigger
	 * under memory pressure.
	 */
	struct shrinker	*shrinker;
};

/*
 * Create a pool.  Zero cells are allocated upfront; the first
 * cell_store() call allocates the active cell on demand.  This
 * matters because a tmpfs mount may enable cell storage but never
 * actually compress anything (e.g. all data is incompressible).
 *
 * @nr_cells is the directory capacity: the pool can never hold more
 * than nr_cells x cell_size of stored blocks, so a caller that knows
 * its worst case (incompressible data up to a device's disksize)
 * must size it accordingly, with headroom for COW churn.  Zero uses
 * CELL_DEFAULT_MAX_CELLS.  Counts above UINT_MAX are clamped (the
 * handle encoding itself would also stop at cellid < 2^32).
 */
static inline bool cell_is_sparse(const struct cell_pool *cp,
				  unsigned long live_bytes)
{
	unsigned long cell_size = 1UL << (cp->cell_order + PAGE_SHIFT);
	unsigned long pct = cp->gc_percent ? cp->gc_percent :
					       CELL_IS_SPARSE_PERCENT_DEFAULT;

	return live_bytes * 100 <= cell_size * pct;
}

struct cell_pool *cell_pool_create(unsigned int order, bool use_thp,
				   unsigned int nr_cells,
				   unsigned int gc_percent);
void cell_pool_destroy(struct cell_pool *cp);

/*
 * Store @len bytes at the next free offset in the active cell.
 * Returns 0 and writes {cellid, offset} on success.
 *
 * On -ENOMEM no cell could be allocated; the caller must handle
 * this (typically by leaving the page uncompressed).
 */
int cell_store(struct cell_pool *cp, const void *data, unsigned int len,
	       u32 *cellid_out, u32 *offset_out);

/*
 * Account for @len bytes being freed from cell @cellid.
 *
 * When live_bytes drops to zero the cell's pages are returned to the
 * buddy allocator before this function returns.  This is the key
 * behavioural difference from a slab-style allocator: there is no
 * retained partially-free pool, so system-wide MemFree reflects
 * actual tmpfs usage without waiting for a shrinker.
 */
void cell_free(struct cell_pool *cp, u32 cellid, u32 offset,
	       unsigned int len);

/*
 * Read @len bytes from cell @cellid at @offset into @dst.
 * On return *len is the number of bytes actually copied (clamped to
 * the cell boundary as a safety measure — callers must ensure the
 * block is fully contained within the cell).
 *
 * The caller must hold an entry reference pin to guarantee the cell
 * has not been returned to the buddy allocator.
 */
int cell_load(struct cell_pool *cp, u32 cellid, u32 offset,
	      void *dst, unsigned int *len);

/*
 * Async GC interface.  cell_free() only links sparse cells; the
 * evacuation pass (shmem_compress.c) uses these to move live
 * objects into the active cell and release the emptied victim.
 */
struct cell *cell_pool_pick_victim(struct cell_pool *cp);
int cell_relocate_one(struct cell_pool *cp, struct cell *victim,
		      const void *data, unsigned int len,
		      u32 *new_cellid, u32 *new_offset);
void cell_pool_release_victim(struct cell_pool *cp, struct cell *victim);

#endif /* _LINUX_CELL_STORE_H */

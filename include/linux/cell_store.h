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
 * a significant fraction of the cell itself.  Maximum 4 MiB: large
 * enough for any merge group, small enough that a single alloc_pages()
 * order-10 request has a reasonable chance of succeeding under
 * memory pressure.
 */
#define CELL_ORDER_MIN	2		/* 16 KiB  */
#define CELL_ORDER_MAX	10		/* 4 MiB   */
#define CELL_ORDER_DEF	4		/* 64 KiB  */

/*
 * A cell with fewer live objects than this is a compaction
 * candidate.  The threshold exists because evacuating a cell costs
 * one read+write per live block; evacuating a cell that is nearly
 * full would waste more bandwidth than it saves.  Four is chosen so
 * that a cell that has lost most of its blocks is prioritized over
 * one that has lost a few.
 */
#define CELL_SPARSE_THRESHOLD	4

struct cell {
	/*
	 * Only pages[0] is used: the pages are physically contiguous
	 * (single alloc_pages() call), so the page_address of the head
	 * page is the base of the entire cell.  The array exists to
	 * hold the allocation so __free_pages() can find the head.
	 */
	struct page	*pages[1 << CELL_ORDER_MAX];
	void		*base;		/* virtual base = page_address(pages[0]) */
	u32		next_free;	/* bump offset; monotonic during cell life */
	u32		live_bytes;	/* sum of live block lengths */
	u16		live_objects;	/* count of live blocks */
	u16		cellid;		/* index into pool->cells[] */
	u8		order;		/* alloc_pages order for this cell */
	bool		active;		/* is this the allocation target? */
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
	 * Protects: active pointer, nr_cells, every cell's
	 * next_free/live_bytes/live_objects/active/node fields, and
	 * the sparse list.  Held briefly during allocation and free;
	 * never held across a memcpy or a page allocator call.
	 */
	spinlock_t	lock;
	struct cell	*active;	/* current allocation target, or NULL */
	struct cell	*cells;		/* directory array, indexed by cellid */
	u16		nr_cells;	/* cells currently allocated */
	u16		max_cells;	/* directory capacity */
	u8		cell_order;	/* alloc_pages order, same for all cells */
	bool		use_thp;	/* request THP-backed pages */
	struct list_head sparse;	/* cells below SPARSE_THRESHOLD objects */
	atomic_long_t	pages_used;	/* (1 << cell_order) * nr_live_cells */
};

/*
 * Create a pool.  Zero cells are allocated upfront; the first
 * cell_store() call allocates the active cell on demand.  This
 * matters because a tmpfs mount may enable cell storage but never
 * actually compress anything (e.g. all data is incompressible).
 */
struct cell_pool *cell_pool_create(unsigned int order, bool use_thp);
void cell_pool_destroy(struct cell_pool *cp);

/*
 * Store @len bytes at the next free offset in the active cell.
 * Returns 0 and writes {cellid, offset} on success.
 *
 * On -ENOMEM no cell could be allocated; the caller must handle
 * this (typically by leaving the page uncompressed).
 */
int cell_store(struct cell_pool *cp, const void *data, unsigned int len,
	       u16 *cellid_out, u32 *offset_out);

/*
 * Account for @len bytes being freed from cell @cellid.
 *
 * When live_bytes drops to zero the cell's pages are returned to the
 * buddy allocator before this function returns.  This is the key
 * behavioural difference from a slab-style allocator: there is no
 * retained partially-free pool, so system-wide MemFree reflects
 * actual tmpfs usage without waiting for a shrinker.
 */
void cell_free(struct cell_pool *cp, u16 cellid, u32 offset,
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
int cell_load(struct cell_pool *cp, u16 cellid, u32 offset,
	      void *dst, unsigned int *len);

#endif /* _LINUX_CELL_STORE_H */

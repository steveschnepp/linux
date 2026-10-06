/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cell_store.h - FTL-inspired bump allocator for tmpfs compression
 *
 * A cell is a single physically-contiguous allocation used as a
 * bump-allocated arena for compressed blocks.  Objects are stored
 * at exact sizes with zero per-object overhead.  Empty cells return
 * to the buddy allocator immediately.
 *
 * Invariants:
 *   - Only the active cell grows (bump pointer).
 *   - A cell with live_bytes == 0 is returned to the buddy.
 *   - cell->base is virtually contiguous (direct map), so any
 *     offset is plain pointer arithmetic.
 */
#ifndef _LINUX_CELL_STORE_H
#define _LINUX_CELL_STORE_H

#include <linux/types.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>

#define CELL_ORDER_MIN	2		/* 16 KiB  */
#define CELL_ORDER_MAX	10		/* 4 MiB   */
#define CELL_ORDER_DEF	4		/* 64 KiB  */
#define CELL_SPARSE_THRESHOLD	4	/* objects below this → sparse */

struct cell {
	struct page	*pages[1 << CELL_ORDER_MAX];
	void		*base;		/* page_address(pages[0]) */
	u32		next_free;	/* bump pointer, monotonic */
	u32		live_bytes;	/* decremented on free */
	u16		live_objects;
	u16		cellid;
	u8		order;		/* alloc_pages order */
	bool		active;		/* current allocation target */
	struct rw_semaphore lock;	/* readers vs compaction */
	struct list_head node;		/* in pool's sparse list */
};

struct cell_pool {
	spinlock_t	lock;		/* protects active + directory */
	struct cell	*active;
	struct cell	*cells;		/* array, indexed by cellid */
	u16		nr_cells;
	u16		max_cells;
	u8		cell_order;
	bool		use_thp;	/* GFP_TRANSHUGE | __GFP_COMP */
	struct list_head sparse;	/* cells eligible for compaction */
	atomic_long_t	pages_used;	/* for ram_* stats */
};

struct cell_pool *cell_pool_create(unsigned int order, bool use_thp);
void cell_pool_destroy(struct cell_pool *cp);

int cell_store(struct cell_pool *cp, const void *data, unsigned int len,
	       u16 *cellid_out, u32 *offset_out);
void cell_free(struct cell_pool *cp, u16 cellid, u32 offset,
	       unsigned int len);
int cell_load(struct cell_pool *cp, u16 cellid, u32 offset,
	      void *dst, unsigned int *len);

#endif /* _LINUX_CELL_STORE_H */

// SPDX-License-Identifier: GPL-2.0
/*
 * cell_store.c - FTL-inspired bump allocator for tmpfs compression
 *
 * Cells are bump-allocated arenas for compressed blocks.  Zero
 * per-object overhead.  Empty cells return to buddy immediately.
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/cell_store.h>

static gfp_t cell_gfp(struct cell_pool *cp)
{
	if (cp->use_thp)
		return GFP_TRANSHUGE | __GFP_COMP | __GFP_NOWARN;
	return GFP_KERNEL | __GFP_NOWARN;
}

/*
 * cell_pool_create - Allocate a cell pool with fixed-order cells
 * @order: alloc_pages order per cell (2..10)
 * @use_thp: allocate from THP zones when possible
 *
 * Returns NULL on allocation failure.  Caller must free with
 * cell_pool_destroy().  The pool starts with zero cells; the first
 * cell_pool_new_cell() call allocates the active cell on demand.
 */
struct cell_pool *cell_pool_create(unsigned int order, bool use_thp)
{
	struct cell_pool *cp;

	if (order < CELL_ORDER_MIN || order > CELL_ORDER_MAX)
		return NULL;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return NULL;

	spin_lock_init(&cp->lock);
	INIT_LIST_HEAD(&cp->sparse);
	cp->cell_order = order;
	cp->use_thp = use_thp;
	cp->max_cells = 256;	/* soft cap; grows on demand */
	cp->cells = kvcalloc(cp->max_cells, sizeof(struct cell),
			      GFP_KERNEL);
	if (!cp->cells) {
		kfree(cp);
		return NULL;
	}

	return cp;
}

/*
 * cell_pool_destroy - Free all cells and the pool itself
 * @cp: pool to destroy
 *
 * Returns every cell's pages to the buddy allocator and frees the
 * pool.  Must not be called while any entry still references a cell.
 */
void cell_pool_destroy(struct cell_pool *cp)
{
	u16 i;

	if (!cp)
		return;

	for (i = 0; i < cp->nr_cells; i++) {
		struct cell *c = &cp->cells[i];

		if (c->pages[0])
			__free_pages(c->pages[0], c->order);
	}

	kvfree(cp->cells);
	kfree(cp);
}

/*
 * cell_pool_new_cell - Allocate a new cell and make it active
 * @cp: pool
 *
 * Called with cp->lock held.  Allocates one cell of cell_order pages
 * from the buddy allocator (THP-backed when use_thp is set).  Returns
 * the new active cell, or NULL on allocation failure.
 */
static struct cell *cell_pool_new_cell(struct cell_pool *cp)
{
	struct cell *c;
	struct page *page;
	u16 id;

	if (cp->nr_cells >= cp->max_cells)
		return NULL;

	id = cp->nr_cells;
	c = &cp->cells[id];

	page = alloc_pages(cell_gfp(cp), cp->cell_order);
	if (!page)
		return NULL;

	c->pages[0] = page;
	c->base = page_address(page);
	c->next_free = 0;
	c->live_bytes = 0;
	c->live_objects = 0;
	c->cellid = id;
	c->order = cp->cell_order;
	c->active = true;
	init_rwsem(&c->lock);
	INIT_LIST_HEAD(&c->node);

	cp->nr_cells++;
	cp->active = c;
	atomic_long_add(1UL << cp->cell_order, &cp->pages_used);

	return c;
}

/*
 * cell_pool_return - Release a cell's pages to the buddy allocator
 * @cp: pool
 * @c: cell to release (must have live_bytes == 0)
 *
 * Called with cp->lock held.  Clears the cell slot so it can be
 * reused by the next cell_pool_new_cell().
 */
static void cell_pool_return(struct cell_pool *cp, struct cell *c)
{
	if (!c->pages[0])
		return;

	list_del_init(&c->node);
	__free_pages(c->pages[0], c->order);
	c->pages[0] = NULL;
	c->base = NULL;
	c->active = false;

	atomic_long_sub(1UL << c->order, &cp->pages_used);

	/* If the active cell was returned, the next alloc creates a new one */
	if (cp->active == c)
		cp->active = NULL;
}

/*
 * cell_store - Bump-allocate space in the active cell and copy data
 * @cp: cell pool
 * @data: source buffer
 * @len: data length in bytes
 * @cellid_out: output cell ID
 * @offset_out: output offset within cell
 *
 * Return: 0 on success, -ENOMEM if no cell could be allocated.
 *
 * Called without cp->lock.  Takes the lock internally for the bump
 * allocation, releases it, then does the memcpy.  The caller must
 * ensure @data remains valid until the memcpy completes (the entry
 * pin guarantees this in the compression path).
 */
int cell_store(struct cell_pool *cp, const void *data,
	       unsigned int len, u16 *cellid_out, u32 *offset_out)
{
	struct cell *c;
	u32 off;

	spin_lock(&cp->lock);

	c = cp->active;
	if (!c || c->next_free + len > (1UL << c->order) * PAGE_SIZE) {
		c = cell_pool_new_cell(cp);
		if (!c) {
			spin_unlock(&cp->lock);
			return -ENOMEM;
		}
	}

	off = c->next_free;
	c->next_free += len;
	c->live_bytes += len;
	c->live_objects++;

	spin_unlock(&cp->lock);

	memcpy(c->base + off, data, len);

	*cellid_out = c->cellid;
	*offset_out = off;
	return 0;
}

/*
 * cell_free - Mark a block as freed within its cell
 * @cp: cell pool
 * @cellid: cell containing the block
 * @offset: offset within cell (unused for bookkeeping, kept for
 *          future per-block tracking and for compaction's use)
 * @len: block length in bytes
 *
 * Decrements live_bytes and live_objects.  When live_bytes drops to
 * zero the cell's pages are returned to the buddy allocator
 * immediately — the user sees MemFree increase without waiting for
 * compaction.  Sparse cells (few live objects) are added to the
 * pool's evacuation list.
 */
void cell_free(struct cell_pool *cp, u16 cellid, u32 offset,
	       unsigned int len)
{
	struct cell *c;

	spin_lock(&cp->lock);

	if (cellid >= cp->nr_cells) {
		spin_unlock(&cp->lock);
		return;
	}

	c = &cp->cells[cellid];
	if (!c->pages[0]) {
		spin_unlock(&cp->lock);
		return;
	}

	c->live_bytes -= len;
	c->live_objects--;

	if (c->live_bytes == 0) {
		/* Cell fully empty — return to buddy immediately */
		cell_pool_return(cp, c);
	} else if (c->live_objects < CELL_SPARSE_THRESHOLD) {
		/* Sparse — eligible for evacuation */
		if (list_empty(&c->node))
			list_add(&c->node, &cp->sparse);
	}

	spin_unlock(&cp->lock);
}

/*
 * cell_load - Read a block from a cell into a destination buffer
 * @cp: cell pool
 * @cellid: cell containing the block
 * @offset: offset within cell
 * @dst: destination buffer
 * @len: [in,out] buffer size on input, data size on output
 *
 * Return: 0 on success, -EINVAL if the cell is not valid.
 *
 * Cells are virtually contiguous (direct map), so this is a plain
 * memcpy regardless of page boundaries within the cell.  No bounce
 * buffer, no kmap.  The caller must hold the entry pin to ensure
 * the cell has not been freed.
 */
int cell_load(struct cell_pool *cp, u16 cellid, u32 offset,
	      void *dst, unsigned int *len)
{
	struct cell *c;
	u32 cell_size;

	if (cellid >= cp->nr_cells)
		return -EINVAL;

	c = &cp->cells[cellid];
	if (!c->pages[0] || !c->base)
		return -EINVAL;

	cell_size = (1UL << c->order) * PAGE_SIZE;
	if (offset >= cell_size)
		return -EINVAL;

	if (offset + *len > cell_size)
		*len = cell_size - offset;

	memcpy(dst, c->base + offset, *len);
	return 0;
}

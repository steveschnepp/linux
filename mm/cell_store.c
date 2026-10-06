// SPDX-License-Identifier: GPL-2.0
/*
 * cell_store.c - Bump allocator for compressed data blocks
 *
 * See cell_store.h for the design invariants.  This file implements
 * the pool lifecycle (create/destroy), the bump allocation path, and
 * the free path that returns empty cells to the buddy allocator.
 *
 * Locking model:
 *   - cp->lock (spinlock) protects the active pointer, the cell
 *     directory, and per-cell counters.  It is never held across a
 *     memcpy, a page allocator call, or any operation that might sleep.
 *   - Per-cell rwsem (c->lock) is reserved for compaction.  Readers
 *     take it for shared access; compaction takes it exclusively
 *     before evacuating a cell.  It is not used on the hot store/load
 *     paths — those rely on the entry reference pin to keep the cell
 *     alive.
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/cell_store.h>

#define cell_dbg(fmt, ...) \
	pr_debug("cell_store: " fmt "\n", ##__VA_ARGS__)

static gfp_t cell_gfp(struct cell_pool *cp)
{
	if (cp->use_thp)
		return GFP_TRANSHUGE | __GFP_COMP | __GFP_NOWARN;
	return GFP_KERNEL | __GFP_NOWARN;
}

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
	cp->max_cells = 256;
	cp->cells = kvcalloc(cp->max_cells, sizeof(struct cell),
			      GFP_KERNEL);
	if (!cp->cells) {
		kfree(cp);
		return NULL;
	}

	cell_dbg("pool created: order=%u (%lu KiB/cell), use_thp=%d",
		 order, (PAGE_SIZE << order) / 1024, use_thp);

	return cp;
}

void cell_pool_destroy(struct cell_pool *cp)
{
	u16 i;

	if (!cp)
		return;

	cell_dbg("destroy pool: %u cells, order=%u, %ld pages",
		 cp->nr_cells, cp->cell_order,
		 atomic_long_read(&cp->pages_used));

	for (i = 0; i < cp->nr_cells; i++) {
		struct cell *c = &cp->cells[i];

		if (c->pages[0])
			__free_pages(c->pages[0], c->order);
	}

	kvfree(cp->cells);
	kfree(cp);
}

/*
 * Allocate one cell of cell_order pages and install it as the active
 * allocation target.  Must be called with cp->lock held.
 *
 * Returns NULL when the directory is full or the page allocator
 * cannot satisfy the request.  The caller treats this as -ENOMEM;
 * the compression path falls back to leaving pages uncompressed.
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

	cell_dbg("new cell %u: base=%p order=%u (%lu KiB)",
		 id, c->base, cp->cell_order,
		 (PAGE_SIZE << cp->cell_order) / 1024);

	return c;
}

/*
 * Release a cell's pages back to the buddy allocator.  Must be called
 * with cp->lock held and only when the cell has zero live bytes.
 *
 * The pages are freed synchronously — no deferred work, no RCU grace
 * period.  This is safe because the caller guarantees no entry still
 * references the cell (live_bytes reached zero through the normal
 * free path, which runs after the last entry is erased from the
 * global entry table).
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

	cell_dbg("return cell %u: freed %lu KiB, %ld pages left",
		 c->cellid, (PAGE_SIZE << c->order) / 1024,
		 atomic_long_read(&cp->pages_used));

	if (cp->active == c)
		cp->active = NULL;
}

int cell_store(struct cell_pool *cp, const void *data,
	       unsigned int len, u16 *cellid_out, u32 *offset_out)
{
	struct cell *c;
	u32 off;
	u32 cell_size;

	/*
	 * A block larger than a single cell can never be stored: the
	 * bump allocator places one block per region and there is no
	 * multi-cell spanning.  Reject loudly here rather than
	 * allocating a fresh cell and memcpy'ing past its end — the
	 * caller treats this as incompressible and keeps the pages
	 * uncompressed.
	 */
	cell_size = (1UL << cp->cell_order) * PAGE_SIZE;
	if (len == 0 || len > cell_size)
		return -E2BIG;

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

	/*
	 * The lock is released before the memcpy to avoid holding a
	 * spinlock across a potentially large copy.  The bump pointer
	 * has already reserved the space, so no other CPU can write
	 * into this region.  The caller must keep @data valid until
	 * this memcpy completes.
	 */
	memcpy(c->base + off, data, len);

	cell_dbg("store: cell=%u offset=%u len=%u (next_free=%u/%lu, %u objects)",
		 c->cellid, off, len,
		 c->next_free, (1UL << c->order) * PAGE_SIZE,
		 c->live_objects);

	*cellid_out = c->cellid;
	*offset_out = off;
	return 0;
}

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

	cell_dbg("free: cell=%u offset=%u len=%u (live_bytes=%u, objects=%u)",
		 cellid, offset, len, c->live_bytes, c->live_objects);

	if (c->live_bytes == 0) {
		cell_pool_return(cp, c);
	} else if (c->live_objects < CELL_SPARSE_THRESHOLD) {
		if (list_empty(&c->node))
			list_add(&c->node, &cp->sparse);
	}

	spin_unlock(&cp->lock);
}

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

	/*
	 * No kmap or bounce buffer: the cell's pages are physically
	 * contiguous (single alloc_pages call), so the direct map
	 * gives us a single virtual range.  A block that spans a page
	 * boundary within the cell is still a single memcpy.
	 */
	memcpy(dst, c->base + offset, *len);

	cell_dbg("load: cell=%u offset=%u len=%u", cellid, offset, *len);

	return 0;
}

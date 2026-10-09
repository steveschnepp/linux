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
#include <linux/mutex.h>
#include <linux/shrinker.h>
#include <linux/cell_store.h>

#define cell_dbg(fmt, ...) \
	pr_debug("cell_store: " fmt "\n", ##__VA_ARGS__)

static gfp_t cell_gfp(struct cell_pool *cp)
{
	if (cp->use_thp)
		return GFP_TRANSHUGE | __GFP_COMP | __GFP_NOWARN;
	return GFP_KERNEL | __GFP_NOWARN;
}

struct cell_pool *cell_pool_create(unsigned int order, bool use_thp,
				   unsigned int nr_cells)
{
	struct cell_pool *cp;
	u16 i;

	if (order < CELL_ORDER_MIN || order > CELL_ORDER_MAX)
		return NULL;

	/*
	 * Directory capacity bounds the pool's PHYSICAL storage: at most
	 * max_cells x cell_size can ever be stored, regardless of what the
	 * caller promised its users.  A caller that knows its capacity
	 * (a block device with a disksize) must pass it, plus headroom:
	 * the store path COW-allocates the new block before the free path
	 * drops the old one, and a freed block only returns its cell to
	 * the buddy when the whole cell empties, so overwrite churn
	 * accumulates partially-filled cells past live_data / cell_size.
	 *
	 * Zero means "caller has no capacity bound" (tmpfs: the page cache
	 * is compressible and bounded by size= accounting, so the legacy
	 * default suffices).  The count is clamped to the u16 directory
	 * index: beyond 65535 cells the incompressible-data cap at this
	 * order is the ceiling, not memory.
	 */
	if (!nr_cells)
		nr_cells = CELL_DEFAULT_MAX_CELLS;
	if (nr_cells > U16_MAX) {
		pr_warn("cell_store: %u cells requested, capping at %u; "
			"incompressible storage on this pool is limited to "
			"%u x %lu KiB\n",
			nr_cells, U16_MAX, U16_MAX,
			(PAGE_SIZE << order) / 1024);
		nr_cells = U16_MAX;
	}

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return NULL;

	spin_lock_init(&cp->lock);
	mutex_init(&cp->gc_lock);
	INIT_LIST_HEAD(&cp->sparse);
	cp->cell_order = order;
	cp->use_thp = use_thp;
	cp->max_cells = nr_cells;
	cp->cells = kvcalloc(cp->max_cells, sizeof(struct cell),
			      GFP_KERNEL);
	if (!cp->cells) {
		kfree(cp);
		return NULL;
	}

	/*
	 * Initialise every cell rwsem exactly once.  GC returns cells
	 * to the buddy and cell_pool_new_cell() reuses directory slots;
	 * re-running init_rwsem on a reused slot would corrupt the lock
	 * state if a stale reader still holds it from the previous
	 * incarnation.
	 */
	for (i = 0; i < cp->max_cells; i++)
		init_rwsem(&cp->cells[i].lock);

	cell_dbg("pool created: order=%u (%lu KiB/cell), use_thp=%d, "
		 "max_cells=%u (%llu MiB incompressible cap)",
		 order, (PAGE_SIZE << order) / 1024, use_thp,
		 cp->max_cells,
		 ((unsigned long long)cp->max_cells << order) >>
			(20 - PAGE_SHIFT));

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
			__free_pages(c->pages[0], cp->cell_order);
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
/*
 * cell_pool_new_cell - Allocate a new cell and make it active
 * @cp: pool
 *
 * Called with cp->lock held.  Scans the directory for a slot freed
 * by a previous return — GC hands whole cells back frequently and a
 * monotonic high-water mark would exhaust the directory even while
 * most slots are empty.  Allocates from the buddy allocator
 * (THP-backed when use_thp is set).  Returns the new active cell,
 * or NULL when the directory is full or memory is unavailable.
 */
/*
 * cell_make_active - ensure cp->active has room for @len bytes
 * @cp: pool
 * @len: block size that must fit
 *
 * Returns with cp->lock HELD and cp->active non-NULL with at least
 * @len bytes free, or NULL with the lock released (-ENOMEM).
 *
 * The page allocation happens OUTSIDE the spinlock — GFP_KERNEL may
 * sleep, which is illegal under spin_lock().  After reacquiring the
 * lock we re-check: a concurrent caller may have installed an active
 * cell while we were in the page allocator, in which case our page
 * is freed and theirs is used.  This makes concurrent store paths
 * safe without ever allocating in atomic context.
 */
static struct cell *cell_make_active(struct cell_pool *cp, unsigned int len)
{
	u32 cell_size = (1UL << cp->cell_order) * PAGE_SIZE;
	struct cell *c;
	struct page *page;

	spin_lock(&cp->lock);
	c = cp->active;
	if (c && c->next_free + len <= cell_size)
		return c;
	spin_unlock(&cp->lock);

	page = alloc_pages(cell_gfp(cp), cp->cell_order);

	spin_lock(&cp->lock);

	/* Re-check: someone may have made a cell while we allocated */
	c = cp->active;
	if (c && c->next_free + len <= cell_size) {
		if (page)
			__free_pages(page, cp->cell_order);
		return c;
	}

	if (!page) {
		spin_unlock(&cp->lock);
		return NULL;
	}

	/* Find a free directory slot (GC returns cells for reuse) */
	c = NULL;
	{
		u16 slot;

		for (slot = 0; slot < cp->max_cells; slot++) {
			if (!cp->cells[slot].pages[0]) {
				c = &cp->cells[slot];
				break;
			}
		}
		if (c)
			c->cellid = slot;
	}
	if (!c) {
		__free_pages(page, cp->cell_order);
		spin_unlock(&cp->lock);
		return NULL;
	}

	c->pages[0] = page;
	c->base = page_address(page);
	c->next_free = 0;
	c->live_bytes = 0;
	c->live_objects = 0;
	INIT_LIST_HEAD(&c->node);
	/* rwsem initialised once at pool create — slots are reused */

	if (c->cellid >= cp->nr_cells)
		cp->nr_cells = c->cellid + 1;
	cp->active = c;
	atomic_long_add(1UL << cp->cell_order, &cp->pages_used);

	cell_dbg("new cell %u: base=%p order=%u (%lu KiB)",
		 c->cellid, c->base, cp->cell_order,
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
	__free_pages(c->pages[0], cp->cell_order);
	c->pages[0] = NULL;
	c->base = NULL;

	atomic_long_sub(1UL << cp->cell_order, &cp->pages_used);

	cell_dbg("return cell %u: freed %lu KiB, %ld pages left",
		 c->cellid, (PAGE_SIZE << cp->cell_order) / 1024,
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
	if (!c || c->next_free + len > cell_size) {
		spin_unlock(&cp->lock);
		c = cell_make_active(cp, len);
		if (!c)
			return -ENOMEM;
		/* cell_make_active returns with cp->lock held */
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
		 c->next_free, (1UL << cp->cell_order) * PAGE_SIZE,
		 c->live_objects);

	*cellid_out = c->cellid;
	*offset_out = off;
	return 0;
}

void cell_free(struct cell_pool *cp, u16 cellid, u32 offset,
	       unsigned int len)
{
	struct cell *c;
	bool empty;

	spin_lock(&cp->lock);

	if (cellid >= cp->max_cells) {
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
	empty = (c->live_bytes == 0);

	cell_dbg("free: cell=%u offset=%u len=%u (live_bytes=%u, objects=%u)%s",
		 cellid, offset, len, c->live_bytes, c->live_objects,
		 empty ? " -> empty" :
		 CELL_IS_SPARSE(c->live_bytes,
				1UL << cp->cell_order << PAGE_SHIFT) ?
		 " -> sparse" : "");

	if (!empty &&
	    CELL_IS_SPARSE(c->live_bytes,
			   1UL << cp->cell_order << PAGE_SHIFT)) {
		/*
		 * Sparse: hand to the async GC.  Evacuation never runs in
		 * the freeing thread's context — one GC pass batches many
		 * cells into a single entry-table scan and a single
		 * active-cell fill, which is strictly cheaper per cell
		 * than per-free synchronous evacuation.
		 */
		if (list_empty(&c->node))
			list_add(&c->node, &cp->sparse);
		spin_unlock(&cp->lock);
		return;
	}

	spin_unlock(&cp->lock);

	if (!empty)
		return;

	/*
	 * Tier 1: the cell is empty — nothing to move, so the pages go
	 * back to the buddy inline.  This is what makes MemFree track
	 * tmpfs usage without waiting for a shrinker.  down_read excludes
	 * a concurrent GC holding this cell as victim; without it we
	 * could __free_pages() memory a GC reader is memcpy'ing from.
	 * The recheck under both locks closes the window where GC
	 * relocated the last objects and returned the cell itself.
	 */
	down_read(&c->lock);
	spin_lock(&cp->lock);
	if (c->pages[0] && c->live_bytes == 0)
		cell_pool_return(cp, c);
	spin_unlock(&cp->lock);
	up_read(&c->lock);
}

int cell_load(struct cell_pool *cp, u16 cellid, u32 offset,
	      void *dst, unsigned int *len)
{
	struct cell *c;
	u32 cell_size;

	if (cellid >= cp->max_cells)
		return -EINVAL;

	c = &cp->cells[cellid];

	/*
	 * Take the read side of the cell rwsem so a GC evacuation
	 * (down_write on the same cell) cannot return the pages to
	 * the buddy while we memcpy from them.  Uncontended in the
	 * common case: readers run in parallel; only eviction of
	 * THIS cell excludes them.
	 */
	down_read(&c->lock);

	if (!c->pages[0] || !c->base) {
		up_read(&c->lock);
		return -EINVAL;
	}

	cell_size = (1UL << cp->cell_order) * PAGE_SIZE;
	if (offset >= cell_size) {
		up_read(&c->lock);
		return -EINVAL;
	}

	if (offset + *len > cell_size)
		*len = cell_size - offset;

	/*
	 * No kmap or bounce buffer: the cell's pages are physically
	 * contiguous (single alloc_pages call), so the direct map
	 * gives us a single virtual range.  A block that spans a page
	 * boundary within the cell is still a single memcpy.
	 */
	memcpy(dst, c->base + offset, *len);
	up_read(&c->lock);

	cell_dbg("load: cell=%u offset=%u len=%u", cellid, offset, *len);

	return 0;
}

/*
 * Async GC primitives.  The evacuation algorithm lives in
 * shmem_compress.c — finding which objects reside in a victim cell
 * requires scanning the entry table, which is private to the
 * compression layer.  These helpers implement the allocator side
 * under a strict protocol:
 *
 *   pick_victim(): harvests the first cell from cp->sparse and
 *     takes down_write on it.  No reader can be inside cell_load()
 *     for this cell, and cell_free()'s empty-return path blocks on
 *     the same lock, so the victim is quiescent for relocation.
 *
 *   relocate_one(): bump-allocates space in the active cell and
 *     copies one block out of the victim.  Adjusts both cells'
 *     accounting under cp->lock.  Returns -ENOMEM when no cell can
 *     back the copy; the caller then leaves remaining objects in
 *     place and the victim is re-linked as sparse.
 *
 *   release_victim(): rechecks under cp->lock — if the victim came
 *     empty (objects relocated or freed concurrently), return it to
 *     the buddy; otherwise re-link it as sparse for a later pass.
 *     Always drops down_write.
 */
struct cell *cell_pool_pick_victim(struct cell_pool *cp)
{
	struct cell *c = NULL, *tmp;

	spin_lock(&cp->lock);
	list_for_each_entry_safe(c, tmp, &cp->sparse, node) {
		/*
		 * Never harvest the active cell: relocating its
		 * objects would target itself, leaking bump space on
		 * every pass without ever emptying the cell.
		 */
		if (c == cp->active)
			continue;
		list_del_init(&c->node);
		spin_unlock(&cp->lock);
		down_write(&c->lock);
		return c;
	}
	spin_unlock(&cp->lock);
	return NULL;
}

int cell_relocate_one(struct cell_pool *cp, struct cell *victim,
		      const void *data, unsigned int len,
		      u16 *new_cellid, u32 *new_offset)
{
	struct cell *a;
	u32 off;
	u32 cell_size = (1UL << cp->cell_order) * PAGE_SIZE;

	if (len == 0 || len > cell_size)
		return -E2BIG;

	spin_lock(&cp->lock);

	a = cp->active;
	if (!a || a->next_free + len > cell_size) {
		spin_unlock(&cp->lock);
		a = cell_make_active(cp, len);
		if (!a)
			return -ENOMEM;
		/* cell_make_active returns with cp->lock held */
	}

	off = a->next_free;
	a->next_free += len;
	a->live_bytes += len;
	a->live_objects++;

	/*
	 * The victim's counters move with the block.  The victim's
	 * rwsem is held for write by the caller, so no reader can
	 * observe intermediate state; cp->lock serialises against
	 * cell_free() of OTHER cells.
	 */
	victim->live_bytes -= len;
	victim->live_objects--;

	spin_unlock(&cp->lock);

	memcpy(a->base + off, data, len);

	cell_dbg("relocate: %u bytes cell=%u->%u offset=%u",
		 len, victim->cellid, a->cellid, off);

	*new_cellid = a->cellid;
	*new_offset = off;
	return 0;
}

void cell_pool_release_victim(struct cell_pool *cp, struct cell *victim)
{
	bool empty;

	spin_lock(&cp->lock);
	empty = (victim->live_bytes == 0 && victim->pages[0]);
	if (empty) {
		cell_pool_return(cp, victim);
	} else if (victim->pages[0] &&
		   CELL_IS_SPARSE(victim->live_bytes,
				  1UL << cp->cell_order << PAGE_SHIFT)) {
		if (list_empty(&victim->node))
			list_add(&victim->node, &cp->sparse);
	}
	spin_unlock(&cp->lock);

	up_write(&victim->lock);
}

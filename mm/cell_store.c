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
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/fcntl.h>
#include <linux/cell_store.h>

#define cell_dbg(fmt, ...) \
	pr_debug("cell_store: " fmt "\n", ##__VA_ARGS__)

static gfp_t cell_gfp(struct cell_pool *cp)
{
	/*
	 * __GFP_NOHIGHMEM is load-bearing on 32-bit (arm32 + HIGHMEM):
	 * a cell holds a PERMANENT kernel mapping (c->base =
	 * page_address()), and page_address() is NULL for highmem
	 * pages.  GFP_TRANSHUGE is highmem-capable, so without this
	 * flag a cell page could land in highmem and cell_store would
	 * memcpy to NULL -- observed as a crash (r0=NULL in mmiocpy)
	 * on the first store to a cell-backed pool under arm32 TCG.
	 * On 64-bit the flag is a no-op.
	 */
	if (cp->use_thp)
		return (GFP_TRANSHUGE & ~(__GFP_HIGHMEM | __GFP_MOVABLE)) |
			__GFP_COMP | __GFP_NOWARN;
	return (GFP_KERNEL & ~(__GFP_HIGHMEM | __GFP_MOVABLE)) |
		__GFP_NOWARN;
}

/*
 * Whole-cell disk swap (specs/07): the backing extent allocator and the
 * O_DIRECT whole-cell I/O.  Everything here is inert until the pool has
 * a backing (cp->backing != NULL, set only when a swapfile= is
 * configured); every caller guards on that, so a NULL backing is a
 * byte-for-byte no-op.
 */

/*
 * Raw O_DIRECT whole-cell I/O under the 1:1 mapping (specs/07): cell N
 * lives at extent N, file offset N * cell_size.  The cellid is the disk
 * slot, so there is no extent parameter and no allocation.
 */
static int cell_backing_write_cell(struct cell_pool *cp, struct cell *c)
{
	loff_t pos = (loff_t)c->cellid * cp->backing->cell_size;
	size_t len = cp->backing->cell_size;
	ssize_t w;

	w = kernel_write(cp->backing->filp, c->base, len, &pos);
	return (w == (ssize_t)len) ? 0 : (w < 0 ? (int)w : -EIO);
}

static int cell_backing_read_cell(struct cell_pool *cp, struct cell *c,
				  void *base)
{
	loff_t pos = (loff_t)c->cellid * cp->backing->cell_size;
	size_t len = cp->backing->cell_size;
	ssize_t r;

	r = kernel_read(cp->backing->filp, base, len, &pos);
	return (r == (ssize_t)len) ? 0 : (r < 0 ? (int)r : -EIO);
}

struct cell_pool *cell_pool_create(unsigned int order, bool use_thp,
				   unsigned int nr_cells,
				   unsigned int gc_percent)
{
	struct cell_pool *cp;
	u32 i;

	if (order < CELL_ORDER_MIN || order > CELL_ORDER_MAX)
		return NULL;
	/*
	 * A cell is one alloc_pages(order) call.  An order above the buddy's
	 * max cannot come from a single allocation without
	 * CONFIG_ARCH_FORCE_MAX_ORDER; refuse cleanly here rather than build
	 * a pool that can never allocate its cells (a store would just fail
	 * and leave every block uncompressed).
	 */
	if (order > MAX_PAGE_ORDER)
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
	 * default suffices).  @nr_cells is already unsigned int, so no
	 * clamp is needed here: the caller owns any wider arithmetic and
	 * truncates before the call.
	 */
	if (!nr_cells)
		nr_cells = CELL_DEFAULT_MAX_CELLS;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return NULL;

	spin_lock_init(&cp->lock);
	mutex_init(&cp->gc_lock);
	INIT_LIST_HEAD(&cp->sparse);
	cp->cell_order = order;
	cp->use_thp = use_thp;
	cp->gc_percent = gc_percent ? : CELL_IS_SPARSE_PERCENT_DEFAULT;
	cp->max_cells = nr_cells;
	/* +1 slot: compaction-only bootstrap, see invariant #6 */
	cp->nslots = cp->max_cells + 1;
	cp->cells = kvcalloc(cp->nslots, sizeof(struct cell),
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
	for (i = 0; i < cp->nslots; i++)
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
	u32 i;

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

	/*
	 * Whole-cell swap: tear the backing down.  On-disk cells hold no
	 * pages (nothing to free above); their extents live inside the
	 * backing file, whose blocks are the file owner's to reclaim once
	 * we drop the reference.
	 */
	if (cp->backing) {
		if (cp->backing->filp)
			fput(cp->backing->filp);
		kfree(cp->backing);
	}

	kvfree(cp->cells);
	kfree(cp);
}

unsigned long cell_get_total_pages(struct cell_pool *cp)
{
	if (!cp)
		return 0;
	return (unsigned long)atomic_long_read(&cp->pages_used);
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
 * @for_gc: caller is evacuating a sparse cell, not satisfying a store
 *
 * Returns with cp->lock HELD and cp->active non-NULL with at least
 * @len bytes free, or NULL with the lock released (-ENOMEM).
 *
 * COMPACTION SLOT: a store may fill every advertised cell, but a GC
 * caller may take the one extra slot allocated beyond them
 * (cp->nslots).  A drain relocates a sparse cell's blocks into a
 * fresh cell, and only the emptied victim frees a slot -- so on a
 * completely full directory there is no destination unless one slot
 * is held back, and evacuating it becomes a fixed point that moves
 * nothing and frees nothing.  The extra slot bootstraps the first
 * drain; the victims it empties fund the rest.
 *
 * The page allocation happens OUTSIDE the spinlock — GFP_KERNEL may
 * sleep, which is illegal under spin_lock().  After reacquiring the
 * lock we re-check: a concurrent caller may have installed an active
 * cell while we were in the page allocator, in which case our page
 * is freed and theirs is used.  This makes concurrent store paths
 * safe without ever allocating in atomic context.
 */
static struct cell *cell_make_active(struct cell_pool *cp, unsigned int len,
				     bool for_gc)
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

	/* Find a free directory slot (GC returns cells for reuse).  A
	 * store is capped at cp->max_cells so the compaction-only slot
	 * beyond it stays free for a GC caller; see the note above.
	 * Index-based rather than free-count-based so the rule is obvious
	 * at the loop bound and cannot drift from a counter. */
	c = NULL;
	{
		u32 slot;
		u32 limit = for_gc ? cp->nslots : cp->max_cells;

		for (slot = 0; slot < limit; slot++) {
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
	       unsigned int len, u32 *cellid_out, u32 *offset_out)
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
		c = cell_make_active(cp, len, false);
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

void cell_free(struct cell_pool *cp, u32 cellid, u32 offset,
	       unsigned int len)
{
	struct cell *c;
	bool empty;

	spin_lock(&cp->lock);

	if (cellid >= cp->nslots) {
		spin_unlock(&cp->lock);
		return;
	}

	c = &cp->cells[cellid];
	/*
	 * A never-allocated or already-returned slot has no pages and no
	 * swap state.  An ON_DISK cell has live_bytes but no resident
	 * pages -- it must still be accounted (a truncate can free a
	 * block behind a paged-out cell).
	 */
	if (!c->pages[0] && c->state != CELL_ON_DISK) {
		spin_unlock(&cp->lock);
		return;
	}

	c->live_bytes -= len;
	c->live_objects--;
	empty = (c->live_bytes == 0);

	cell_dbg("free: cell=%u offset=%u len=%u (live_bytes=%u, objects=%u)%s",
		 cellid, offset, len, c->live_bytes, c->live_objects,
		 empty ? " -> empty" :
		 cell_is_sparse(cp, c->live_bytes) ? " -> sparse" : "");

	if (c->state == CELL_ON_DISK) {
		/*
		 * No RAM is held, so the only reclamation is the disk
		 * extent.  On empty, recycle the extent and drop the slot
		 * back to the free state.  A sparse non-empty on-disk cell
		 * is left on disk (disk is cheap) and reclaimed after a
		 * later swap-in; it is deliberately NOT linked into the
		 * sparse list, because GC needs resident pages.
		 */
		if (empty) {
			/* 1:1: the disk slot is permanent (extent cellid), so
			 * there is nothing to free -- just drop the slot back
			 * to the free state. */
			c->state = CELL_IN_RAM;	/* free slot again */
			c->live_objects = 0;
			spin_unlock(&cp->lock);
			return;
		}
		spin_unlock(&cp->lock);
		return;
	}

	if (!empty && cell_is_sparse(cp, c->live_bytes)) {
		/*
		 * Sparse (gc_percent byte utilization, default 50): hand to
		 * the async GC.  Evacuation never runs in the freeing
		 * thread's context -- one GC pass batches many cells into a
		 * single entry-table scan and a single active-cell fill,
		 * which is strictly cheaper per cell than per-free
		 * synchronous evacuation.
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

/*
 * Swap an ON_DISK cell back into RAM (whole, raw).  Takes c->lock for
 * write: that excludes concurrent cell_load readers and a GC eviction,
 * and it is the serializer that collapses N concurrent loaders into one
 * swap-in (the losers re-check state under the lock and find IN_RAM).
 * The O_DIRECT read sleeps but never runs under cp->lock.
 */
static int cell_swap_in(struct cell_pool *cp, struct cell *c)
{
	struct cell_backing *b = cp->backing;
	struct page *page;
	void *base;
	int ret;

	if (!b || !b->filp)
		return -EINVAL;

	down_write(&c->lock);
	if (c->state == CELL_IN_RAM) {		/* lost the race; already in */
		up_write(&c->lock);
		return 0;
	}
	if (c->state != CELL_ON_DISK) {		/* free slot */
		up_write(&c->lock);
		return -EINVAL;
	}

	page = alloc_pages(cell_gfp(cp), cp->cell_order);
	if (!page) {
		up_write(&c->lock);
		return -ENOMEM;
	}
	base = page_address(page);

	ret = cell_backing_read_cell(cp, c, base);
	if (ret) {
		__free_pages(page, cp->cell_order);
		up_write(&c->lock);
		return ret;
	}

	spin_lock(&cp->lock);
	c->pages[0] = page;
	c->base = base;
	c->state = CELL_IN_RAM;
	atomic_long_add(1UL << cp->cell_order, &cp->pages_used);
	spin_unlock(&cp->lock);

	up_write(&c->lock);
	return 0;
}

int cell_load(struct cell_pool *cp, u32 cellid, u32 offset,
	      void *dst, unsigned int *len)
{
	struct cell *c;
	u32 cell_size;

	if (cellid >= cp->nslots)
		return -EINVAL;

	c = &cp->cells[cellid];

	/*
	 * Whole-cell swap-in loop: a cell found ON_DISK is paged back in
	 * (whole, raw) and we retry.  c->state is stable under down_read
	 * because only swap-in/swap-out mutate it and both take
	 * down_write.  A concurrent swap-out could re-page the cell out
	 * between our up_read and the next down_read, so this loops; it
	 * terminates once the cell stays resident long enough to serve
	 * the memcpy below (which holds down_read throughout).
	 */
	for (;;) {
		down_read(&c->lock);
		if (c->state != CELL_ON_DISK) {
			if (c->pages[0] && c->base)
				break;	/* resident: serve below */
			up_read(&c->lock);
			return -EINVAL;	/* free slot */
		}
		up_read(&c->lock);
		{
			int ret = cell_swap_in(cp, c);
			if (ret)
				return ret;
		}
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
 * Swap one resident cell out to the backing (whole, raw).  Takes
 * c->lock for write so it cannot race a cell_load memcpy or a GC
 * evacuation.  The state flips to ON_DISK (and the cell unlinks from the
 * sparse list) BEFORE the I/O so a concurrent shrinker/GC sees it as
 * on-disk and skips it.  On I/O failure the cell rolls back to resident.
 * Process context only (sleeps on I/O): the aging workqueue, never the
 * shrinker (reclaim context; I/O there risks recursion).
 */
static int cell_swap_out_one(struct cell_pool *cp, struct cell *c)
{
	int ret;

	down_write(&c->lock);
	spin_lock(&cp->lock);
	if (c == cp->active || c->state != CELL_IN_RAM || !c->pages[0] ||
	    c->live_bytes == 0) {
		spin_unlock(&cp->lock);
		up_write(&c->lock);
		return -EBUSY;
	}
	/* 1:1 mapping: this cell's permanent disk slot is extent cellid, so
	 * there is nothing to allocate and no ENOSPC possible.  Flip state
	 * before the I/O so a concurrent shrinker/GC skips the cell. */
	c->state = CELL_ON_DISK;
	if (!list_empty(&c->node))
		list_del_init(&c->node);
	spin_unlock(&cp->lock);

	/* c->base is valid and stable: we hold c->lock for write. */
	ret = cell_backing_write_cell(cp, c);
	if (!ret) {
		__free_pages(c->pages[0], cp->cell_order);
		spin_lock(&cp->lock);
		c->pages[0] = NULL;
		c->base = NULL;
		atomic_long_sub(1UL << cp->cell_order, &cp->pages_used);
		spin_unlock(&cp->lock);
	} else {
		/* Roll back to resident; the (partial) disk image is ignored
		 * because state returns to IN_RAM. */
		spin_lock(&cp->lock);
		c->state = CELL_IN_RAM;
		spin_unlock(&cp->lock);
	}
	up_write(&c->lock);
	return ret;
}

unsigned int cell_pool_swap_out(struct cell_pool *cp, unsigned int max)
{
	unsigned int n = 0;
	u32 i;

	if (!cp->backing || !cp->backing->filp)
		return 0;

	for (i = 0; i < cp->nr_cells && n < max; i++) {
		struct cell *c = &cp->cells[i];

		/* Selection is a heuristic (re-checked under lock in
		 * cell_swap_out_one): resident, non-empty, non-sparse.  The
		 * active cell and sparse cells (GC's job) are skipped. */
		if (c->state != CELL_IN_RAM || c->live_bytes == 0 ||
		    cell_is_sparse(cp, c->live_bytes))
			continue;
		if (cell_swap_out_one(cp, c) == 0)
			n++;
	}
	return n;
}

int cell_pool_enable_backing(struct cell_pool *cp, const char *path,
			     u64 size_bytes)
{
	struct cell_backing *b;
	struct file *filp;
	u64 cell_size, nr;

	if (!cp || !path || !path[0])
		return -EINVAL;
	cell_size = (u64)PAGE_SIZE << cp->cell_order;

	filp = filp_open(path, O_RDWR | O_DIRECT | O_LARGEFILE, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	if (size_bytes) {
		int ret = vfs_fallocate(filp, 0, 0, size_bytes);

		if (ret) {
			fput(filp);
			return ret;
		}
	} else {
		size_bytes = i_size_read(file_inode(filp));
	}

	/* One extent == one cell; the file must be a positive multiple. */
	if (size_bytes < cell_size || (size_bytes & (cell_size - 1))) {
		fput(filp);
		return -EINVAL;
	}

	/*
	 * 1:1 mapping requires a permanent slot per directory entry, so
	 * the backing must hold at least one extent per cell.  A smaller
	 * backing is refused outright (no allocator fallback): the whole
	 * tier exists to extend the tmpfs past RAM, which needs disk >=
	 * the pool's cell capacity anyway.
	 */
	nr = size_bytes / cell_size;
	if (nr < cp->nslots) {
		fput(filp);
		pr_warn("cell_store: swap backing %s too small (%llu extents "
			"< %u cells); need >= %llu bytes\n", path, nr,
			cp->nslots, (u64)cp->nslots * cell_size);
		return -ENOSPC;
	}

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b) {
		fput(filp);
		return -ENOMEM;
	}
	b->filp = filp;
	b->nr_extents = nr;
	b->cell_size = cell_size;

	cp->backing = b;
	pr_info("cell_store: whole-cell swap backing %s, %llu extents x "
		"%llu KiB (1:1 with %u cells)\n", path, nr, cell_size / 1024,
		cp->nslots);
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
		/* Never harvest an ON_DISK cell: it holds no RAM to reclaim
		 * (swap-out already unlinked it from sparse; this is a
		 * defensive skip for a cell freed->sparse while on disk). */
		if (c->state == CELL_ON_DISK)
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
		      u32 *new_cellid, u32 *new_offset)
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
		/* for_gc=true: the evacuation target may take the compaction
		 * slot -- this is the one caller that must be able to. */
		a = cell_make_active(cp, len, true);
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

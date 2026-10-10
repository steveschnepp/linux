// SPDX-License-Identifier: GPL-2.0
/*
 * mm/cellzram.c - compressed RAM block device on the cell allocator
 *
 * cellzram is zram's contract (logical capacity set at runtime via the
 * disksize sysfs attribute, physical allocation on demand) with the
 * cell allocator's storage properties: zero per-object metadata,
 * direct-map reads with no bounce buffer, and synchronous return of
 * emptied cells to the buddy allocator.
 *
 * Reuse contract with shmem_comp (mm/shmem_compress.c):
 *   - The device owns a private struct shmem_comp_pool created with
 *     cell storage; all hot paths (compress, store, pinned load,
 *     entry pin/retry, free dispatch via entry_put) are shmem_comp's,
 *     unchanged. Device pools join the global shmem_comp_pools list,
 *     so /sys/fs/tmpfs/compression/gc_now triggers GC on them and the
 *     pool shrinker reclaims under memory pressure. The sysfs group is
 *     tmpfs-named for a block-device pool: cosmetic, accepted for the
 *     prototype.
 *   - The block table maps block_nr to a shmem_comp_entry id (values
 *     only, via xa_mk_value), the same ID table tmpfs compression
 *     uses. Entry length is the raw/compressed discriminator:
 *     length == PAGE_SIZE is a raw block (algo=none, or a page that
 *     did not shrink), length < PAGE_SIZE is compressed. shmem_comp_
 *     compress() returns -ENOSPC when output >= input, so a stored
 *     block of exactly PAGE_SIZE is unambiguously raw.
 *   - Entry lifetime: the table slot owns one entry reference
 *     (refs == slots == 1). xa_exchange (write COW), xa_erase
 *     (discard, reset, swap-slot-free) each drop it via
 *     shmem_comp_entry_put(), which frees the cell region when the
 *     last reference goes -- cells return to the buddy inline.
 *     Readers pin with shmem_comp_entry_get() (inc_unless_zero under
 *     RCU) before touching data and retry the table lookup if the pin
 *     fails, so a concurrent overwrite cannot free storage mid-read.
 *
 * Locking: the block table is the xarray's own lock; entry lifetime
 * is refcounts; cell memory has per-cell rwsems (GC vs readers). No
 * device-wide lock on the read or write path. io_lock serialises
 * reset (and disksize/comp_algorithm sysfs) only; reset additionally
 * freezes the queue so no bio is in flight while the table is erased.
 *
 * Failure handling: a failed store (genuine memory pressure) sets
 * bio->bi_status = BLK_STS_IOERR, zram's pattern for a failed write.
 * A bio-based driver has no blk-mq requeue path; the status reaches
 * the caller as -ENOMEM via blk_status_to_errno(). Swap survives it:
 * a failed swap write leaves the page in the swap cache and the next
 * reclaim pass retries. This is the one path where the block device
 * is stricter than tmpfs (which silently keeps pages uncompressed):
 * a block device cannot decline to store data.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/swap.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <crypto/acompress.h>
#include <linux/shmem_compress.h>
#include <linux/cell_store.h>

#define SECTOR_SHIFT_BITS	3	/* 4 KiB logical blocks -> sector >> 3 */
#define CELLZRAM_NAME		"cellzram"

struct cellzram {
	struct gendisk		*disk;
	/*
	 * Private compression pool: per-CPU acomp contexts, the cell
	 * pool (cell storage), entry-table membership, shrinker. All
	 * hot paths are shmem_comp's.
	 */
	struct shmem_comp_pool	*pool;
	/*
	 * block table: unit index -> entry id. Sparse by nature: unwritten
	 * blocks have no slot and read as zeros. Values only (entry ids via
	 * xa_mk_value), so no pointer/value mixing.
	 */
	struct xarray		table;
	u64			logical_bytes;	/* bytes; 0 until disksize set */
	bool			compressed;	/* false when algo == none */
	char			algo[CRYPTO_MAX_ALG_NAME];
	/*
	 * Event counters for io_stat: cumulative, one increment per
	 * completed operation, safe to sample at any time.  The storage
	 * figures (mm_stat) carry no counters at all -- they are walked
	 * from the block table on read; see mm_stat_show.
	 */
	atomic64_t		io_reads;
	atomic64_t		io_writes;
	atomic64_t		failed_writes;
	atomic64_t		discarded_blocks;
	struct mutex		io_lock;	/* serialises reset + sysfs */
};

static int nr_devices = 1;
module_param(nr_devices, int, 0444);
MODULE_PARM_DESC(nr_devices, "Number of cellzram devices to create");

static int cell_order = 6;	/* 256 KiB cells */
module_param(cell_order, int, 0444);
MODULE_PARM_DESC(cell_order,
		 "alloc_pages order for device cell pools (2..10)");

/*
 * Directory headroom over the incompressible worst case.  disksize /
 * cell_size cells store disksize of incompressible data exactly, but
 * the store path COW-allocates before the old block is freed and an
 * emptied cell (not a partially freed one) is what returns to the
 * buddy, so overwrite churn transiently needs more cells than the
 * live set.  This percentage is that buffer.
 */
/*
 * Directory headroom over the incompressible worst case.  disksize /
 * cell_size cells store disksize of incompressible data exactly, but
 * the store path COW-allocates before the old block is freed and an
 * emptied cell (not a partially freed one) is what returns to the
 * buddy, so overwrite churn transiently needs more cells than the
 * live set.  This percentage is that buffer.
 *
 * MEASURED (sweep_headroom.sh, the churn gate): compaction-on-
 * exhaustion (94d78f6ae3da) makes this a PERFORMANCE knob, not a
 * correctness one -- every value from 0 to 100 passes the gate, and
 * what changes is how often a store has to pay for a GC pass.
 *
 * EXHAUSTIONS (mm_stat field 8) is the column that measures this knob.
 * It falls monotonically with headroom at EVERY workload length tried
 * (10/15/20% x 32..80 passes), so it is a genuine headroom effect:
 *
 *   pct    passes=32 / 48 / 64 / 80
 *   10          28 / 43 / 58 / 73
 *   15          26 / 40 / 53 / 67
 *   20          24 / 36 / 49 / 62
 *
 * DO NOT read cell storage as a headroom metric.  An earlier revision
 * of this comment ranked headroom values by end-of-churn cell storage
 * and called 15% the value to avoid; that was a measurement artifact,
 * and the ranking is the reverse of what it claimed.
 *
 * Storage depends on WHERE a fixed-size workload stops inside the
 * fill -> exhaust -> compact -> refill cycle.  The compaction pass
 * only runs when the directory is full, so occupancy climbs to the
 * pool limit, drops in one batch, and climbs again -- end-of-churn
 * occupancy is (post-GC floor) + (writes since the last GC), and the
 * latter is essentially arbitrary mod the cycle length.  Measured at
 * FIXED headroom (15%, pool 74 cells), varying only the pass count:
 * 51, 49, 41, 70, 64, 56, 52 cells -- a 38-cell spread that swamps the
 * 5-cell difference between the 10% and 20% means.
 *
 *   pct    mean cells (7 pass counts)   sd
 *   10              52.1               11.1
 *   15              54.7                9.7
 *   20              57.3               11.9
 *
 * 10% stays the default on the exhaustion curve alone, which is the
 * column that reproduces.  All standing gates green (cellzram 77/0,
 * randio 4/0, fsdev 19/0, torture 231/0).
 */
static int cell_headroom_pct = 10;
module_param(cell_headroom_pct, int, 0444);
MODULE_PARM_DESC(cell_headroom_pct,
		 "Cell directory headroom over disksize (percent)");

/*
 * FTL-style logical/physical split.  logical is the granularity the
 * block layer addresses (and the only size it enforces on writes);
 * physical is the compression unit -- one blob per physical block,
 * with a per-unit validity bitmap so a discarded logical sector reads
 * as zeros without recompressing the unit.
 *
 * Set at device creation, both powers of two.  logical is capped by
 * BLK_MAX_BLOCK_SIZE (PAGE_SIZE without THP).  physical has no block-
 * layer bound but MUST fit in one cell, since a unit is stored as a
 * single cell region -- that is the limit that matters, and it also
 * bounds the unit bitmap at cell_size / logical bits.
 *
 * Defaults are both PAGE_SIZE: ratio 1, one blob per 4 KiB block,
 * bitmaps degenerate to a single set bit.  That is byte-for-byte the
 * pre-split behaviour, so this is a no-op until a larger physical is
 * requested.
 */
static int logical_block_size = PAGE_SIZE;
module_param(logical_block_size, int, 0444);
MODULE_PARM_DESC(logical_block_size,
		 "Logical block size in bytes (power of 2, default PAGE_SIZE)");

static int physical_block_size = PAGE_SIZE;
module_param(physical_block_size, int, 0444);
MODULE_PARM_DESC(physical_block_size,
		 "Physical block size in bytes (power of 2, >= logical, <= cell size)");

/*
 * Staged-write buffers per device (the FTL open-cell model): the
 * bounded count of uncompressed unit buffers a write may hold open
 * before a slot must be reclaimed by compression (GC, or -- only when
 * the count is exhausted -- the write itself).  Each buffer is one
 * cell (cell_order), so this bounds uncompressed staging RAM at
 * open_units * cell_size.  0 = staging disabled, the default: the
 * device is byte-for-byte today's per-block behaviour.  The real
 * default is a bench decision (mission_log/2026-10-10_ftl-open-
 * cell-buffer.md), so this ships off until that data exists.
 */
#define OPEN_UNITS_MAX	1024
static int open_units = 0;
module_param(open_units, int, 0444);
MODULE_PARM_DESC(open_units,
		 "Staged uncompressed write buffers per device (0 = disabled)");

/* logical sectors per compression unit; 1 when the split is disabled */
static unsigned int cellzram_unit_ratio;

static unsigned int cellzram_unit_sectors(void)
{
	return cellzram_unit_ratio ? cellzram_unit_ratio : 1;
}

/*
 * Logical block number -> compression unit index, the block table's
 * key.  At ratio 1 this is the identity, which is what makes step 2 a
 * no-op at default.  The in-unit logical offset is derived the same
 * way and is consumed by the RMW paths (steps 4+), not here.
 */
static unsigned long cellzram_unit_of(unsigned long block_nr)
{
	return block_nr / cellzram_unit_sectors();
}

static int cellzram_major;

static struct cellzram **cellzram_devs;

/*
 * Drop the block table's reference on @old (a stored table value):
 * frees the entry, which frees the cell region when it was the last
 * reference.  Sentinel zero-blocks (xa_mk_value(0)) hold no entry,
 * so there is nothing to free for those.
 */
static void cellzram_put_slot(void *old)
{
	unsigned int id = xa_to_value(old);

	if (id)
		shmem_comp_entry_put(id);
}

/*
 * Read one logical block. @dst has room for @len_bytes bytes (<= PAGE_SIZE).
 * Unwritten blocks read as zeros (brd semantics).
 */
static int cellzram_read_block(struct cellzram *dev, unsigned long block_nr,
			       void *dst, unsigned int len_bytes)
{
	unsigned long unit_nr = cellzram_unit_of(block_nr);
	unsigned int id, dlen_bytes;
	void *src;
	int ret;

	rcu_read_lock();
	for (;;) {
		void *entry = xa_load(&dev->table, unit_nr);

		if (!entry) {
			rcu_read_unlock();
			memset(dst, 0, len_bytes);
			return 0;
		}
		id = xa_to_value(entry);
		if (!id) {
			/* zero-block sentinel: nothing stored, reads zero */
			rcu_read_unlock();
			memset(dst, 0, len_bytes);
			return 0;
		}
		/*
		 * Pin before touching storage: a concurrent overwrite
		 * (xa_exchange + put) may be freeing this entry and its
		 * cell region right now. Retry the lookup if we lost.
		 */
		if (shmem_comp_entry_get(id, NULL, &dlen_bytes, NULL, NULL, NULL))
			break;
	}
	rcu_read_unlock();

	if (cellzram_unit_sectors() > 1) {
		/* ratio>1: the whole-unit read lives in shmem_compress
		 * (validity mask, staged memcpy, closed decompress). */
		unsigned int blk = block_nr % cellzram_unit_sectors();

		if (len_bytes == PAGE_SIZE) {
			ret = shmem_comp_unit_load(id, blk, dst, len_bytes);
		} else {
			void *tmp = kmalloc(PAGE_SIZE, GFP_NOIO);

			if (!tmp) {
				shmem_comp_entry_put(id);
				return -ENOMEM;
			}
			ret = shmem_comp_unit_load(id, blk, tmp, PAGE_SIZE);
			if (!ret)
				memcpy(dst, tmp, len_bytes);
			kfree(tmp);
		}
		shmem_comp_entry_put(id);
		return ret;
	}

	if (dlen_bytes == PAGE_SIZE && len_bytes == PAGE_SIZE) {
		/* raw block: load straight into the destination */
		ret = shmem_comp_load_entry(id, dst, &dlen_bytes);
		shmem_comp_entry_put(id);
		return ret;
	}

	src = kmalloc(PAGE_SIZE, GFP_NOIO);
	if (!src) {
		shmem_comp_entry_put(id);
		return -ENOMEM;
	}
	/*
	 * dlen_bytes is the entry's stored length (from entry_get above):
	 * load_entry copies *len_bytes bytes and only clamps to the cell
	 * boundary, so passing PAGE_SIZE here would copy adjacent
	 * bump-allocated garbage past a compressed block.
	 */
	ret = shmem_comp_load_entry(id, src, &dlen_bytes);
	shmem_comp_entry_put(id);
	if (ret) {
		kfree(src);
		return ret;
	}

	if (dlen_bytes == PAGE_SIZE) {
		memcpy(dst, src, len_bytes);		/* raw block, partial segment */
	} else if (len_bytes == PAGE_SIZE) {
		unsigned int out = PAGE_SIZE;

		ret = shmem_comp_decompress(dev->pool, src, dlen_bytes, dst, &out);
	} else {
		/* decompress writes a full page: bounce a partial segment */
		void *tmp = kmalloc(PAGE_SIZE, GFP_NOIO);
		unsigned int out = PAGE_SIZE;

		if (!tmp) {
			kfree(src);
			return -ENOMEM;
		}
		ret = shmem_comp_decompress(dev->pool, src, dlen_bytes, tmp, &out);
		if (!ret)
			memcpy(dst, tmp, len_bytes);
		kfree(tmp);
	}
	kfree(src);
	return ret;
}

/*
 * True if every word in the page is zero.  Word-at-a-time (512 compares
 * on x86_64), never touches memory we do not own.  Used to short-circuit
 * all-zero writes: such a block is stored as nothing (see
 * cellzram_write_block), which is safe only because a read-miss
 * zero-fills -- so "all zeros" and "never written" read back identically.
 */
static bool cellzram_page_all_zero(const void *src)
{
	const unsigned long *p = src;
	size_t i, n = PAGE_SIZE / sizeof(*p);

	for (i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

/*
 * ratio>1 write: stage the unit (an uncompressed gather buffer) and write
 * one logical block into it.  A normal write only stages -- it compresses
 * nothing.  When the pool has no free open slot this returns -ENOMEM
 * (-> BLK_STS_RESOURCE): the async drain / eviction that frees a slot is
 * a later step, so for now a full pool rejects the write rather than
 * silently losing data.  See mission_log/2026-10-10_ftl-open-cell-buffer.md.
 */
static int cellzram_write_unit(struct cellzram *dev, unsigned long unit_nr,
			       unsigned long block_nr, const void *src)
{
	unsigned int ratio = cellzram_unit_sectors();
	unsigned int blk = block_nr % ratio;
	unsigned int old_id = 0, new_id;
	unsigned long handle = 0;
	void *old;
	int ret;

	rcu_read_lock();
	{
		void *e = xa_load(&dev->table, unit_nr);

		if (e)
			old_id = xa_to_value(e);
	}
	rcu_read_unlock();

	/* all-zero block: clear the unit's valid bit, store no data; the
	 * block then reads as zeros via the validity mask. */
	if (cellzram_page_all_zero(src)) {
		if (old_id) {
			shmem_comp_entry_get(old_id, NULL, NULL, NULL, NULL, NULL);
			shmem_comp_entry_sector_clear(old_id, blk);
			shmem_comp_entry_put(old_id);
		}
		return 0;
	}

	/* pin the old entry (if any) so it stays alive across the stage */
	if (old_id)
		shmem_comp_entry_get(old_id, &handle, NULL, NULL, NULL, NULL);

	if (old_id && shmem_comp_is_staged(handle)) {
		shmem_comp_entry_put(old_id);
		/* -EAGAIN means it was drained mid-write; step 2 has no
		 * drain, so it cannot fire here -- surface it if it does. */
		return shmem_comp_stage_write(old_id, blk, src, PAGE_SIZE);
	}

	/* closed or unwritten: stage a fresh buffer seeded from old_id */
	new_id = shmem_comp_stage_open(dev->pool, old_id);
	if (old_id)
		shmem_comp_entry_put(old_id);
	if (!new_id)
		return -ENOMEM;	/* pool full: no slot (drain is a later step) */

	ret = shmem_comp_stage_write(new_id, blk, src, PAGE_SIZE);
	if (ret) {
		shmem_comp_entry_put(new_id);
		return ret;
	}

	/* publish: swap the staged entry into the table, drop the old */
	old = xa_store(&dev->table, unit_nr, xa_mk_value(new_id), GFP_NOIO);
	if (IS_ERR(old)) {
		shmem_comp_entry_put(new_id);
		return PTR_ERR(old);
	}
	if (old)
		cellzram_put_slot(old);
	return 0;
}

/*
 * Write one full logical block. Stores raw in phase-1 mode
 * (compressed == false); phase 2 wires compression in with the entry
 * length as the discriminator. Replaces any previous block (COW): the
 * old entry is put after the exchange, so a reader pinned on it keeps
 * its data alive until it is done.
 */
static int cellzram_write_block(struct cellzram *dev, unsigned long block_nr,
				const void *src)
{
	unsigned long unit_nr = cellzram_unit_of(block_nr);

	if (cellzram_unit_sectors() > 1)
		return cellzram_write_unit(dev, unit_nr, block_nr, src);

	/*
	 * all-zero page: store a sentinel slot (xa_mk_value(0); entry ids
	 * are never 0), no entry/cell -- a zero block reads back as zeros
	 * via the sentinel check in cellzram_read_block, indistinguishable
	 * from an unwritten block to the reader.  Filesystem metadata
	 * (unused inode tables, bitmaps, padding) is zero-heavy, so this
	 * turns ext4's own zero writes into an xarray value slot instead
	 * of lz4 + a cell + an entry.  Net win: a 512-word scan replaces
	 * the whole store path.  Applies in raw (none) mode too.
	 *
	 * The slot is held (rather than erased, as before) so the block
	 * table stays the complete picture of what is written: overwrite
	 * and discard can see the old state, and mm_stat is an exact
	 * table walk instead of four counters that drifted on every
	 * repeated zero write.
	 */
	if (cellzram_page_all_zero(src)) {
		void *old = xa_store(&dev->table, unit_nr, xa_mk_value(0),
				     GFP_NOIO);

		if (IS_ERR(old))
			return PTR_ERR(old);
		if (old)
			cellzram_put_slot(old);
		return 0;
	}
	unsigned long handle;
	unsigned int id;
	unsigned int len_bytes = PAGE_SIZE;
	const void *data = src;
	void *cbuf = NULL;
	void *old;
	int ret;

	if (dev->compressed) {
		cbuf = kmalloc(PAGE_SIZE, GFP_NOIO);
		if (!cbuf)
			return -ENOMEM;
		ret = shmem_comp_compress(dev->pool, src, PAGE_SIZE,
					  cbuf, &len_bytes);
		if (ret) {
			/* -ENOSPC: store raw, the length marks it */
			len_bytes = PAGE_SIZE;
		} else {
			data = cbuf;
		}
	}

	handle = shmem_comp_store(dev->pool, data, len_bytes);
	if (!handle) {
		kfree(cbuf);
		return -ENOMEM;
	}

	/* Table-internal block: never referenced from a page cache
	 * xarray value, so no generation is needed (NULL out-param). */
	id = shmem_comp_entry_alloc(dev->pool, handle, len_bytes, 1, NULL, 0,
				    NULL);
	if (!id) {
		u32 cellid;
		u32 offset_bytes;

		/* pack_loc biases cellid +1: unpack, never raw shifts */
		shmem_comp_unpack_loc(handle, &cellid, &offset_bytes);
		cell_free(shmem_comp_pool_cells(dev->pool),
			  cellid, offset_bytes, len_bytes);
		kfree(cbuf);
		return -ENOMEM;
	}
	kfree(cbuf);

	/*
	 * Atomic replace-and-return-old (this tree has no xa_exchange):
	 * readers may see either value; the pin protocol in
	 * cellzram_read_block() covers the swap.  On ENOMEM the old slot
	 * is left untouched; free the entry just made (its teardown
	 * releases the cell region) rather than leaking it, and never
	 * feed an XA_ERROR pointer to xa_to_value().
	 */
	old = xa_store(&dev->table, unit_nr, xa_mk_value(id), GFP_NOIO);
	if (IS_ERR(old)) {
		shmem_comp_entry_put(id);
		return PTR_ERR(old);
	}
	if (old)
		cellzram_put_slot(old);
	return 0;
}

static void cellzram_bio_read(struct cellzram *dev, struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bv;
	sector_t sector = bio->bi_iter.bi_sector;

	bio_for_each_segment(bv, bio, iter) {
		void *mem = kmap_local_page(bv.bv_page) + bv.bv_offset;
		unsigned long block_nr = sector >> SECTOR_SHIFT_BITS;
		int ret;

		if (bv.bv_offset == 0 && bv.bv_len == PAGE_SIZE)
			ret = cellzram_read_block(dev, block_nr, mem, bv.bv_len);
		else {
			void *tmp = kmalloc(PAGE_SIZE, GFP_NOIO);

			if (!tmp) {
				bio->bi_status = BLK_STS_RESOURCE;
				kunmap_local(mem);
				return;
			}
			ret = cellzram_read_block(dev, block_nr, tmp, PAGE_SIZE);
			if (!ret)
				memcpy(mem, tmp, bv.bv_len);
			kfree(tmp);
		}
		kunmap_local(mem);
		if (ret) {
			bio->bi_status = BLK_STS_IOERR;
			return;
		}
		atomic64_inc(&dev->io_reads);
		sector += bv.bv_len >> SECTOR_SHIFT;
	}
}

static void cellzram_bio_write(struct cellzram *dev, struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bv;
	sector_t sector = bio->bi_iter.bi_sector;

	bio_for_each_segment(bv, bio, iter) {
		void *mem = kmap_local_page(bv.bv_page) + bv.bv_offset;
		unsigned long block_nr = sector >> SECTOR_SHIFT_BITS;
		int ret;

		if (bv.bv_offset == 0 && bv.bv_len == PAGE_SIZE)
			ret = cellzram_write_block(dev, block_nr, mem);
		else {
			void *tmp = kmalloc(PAGE_SIZE, GFP_NOIO);

			if (!tmp) {
				bio->bi_status = BLK_STS_RESOURCE;
				kunmap_local(mem);
				return;
			}
			memcpy(tmp, mem, bv.bv_len);
			if (bv.bv_len < PAGE_SIZE)
				memset(tmp + bv.bv_len, 0,
				       PAGE_SIZE - bv.bv_len);
			ret = cellzram_write_block(dev, block_nr, tmp);
			kfree(tmp);
		}
		kunmap_local(mem);
		if (ret) {
			atomic64_inc(&dev->failed_writes);
			bio->bi_status = BLK_STS_IOERR;
			return;
		}
		atomic64_inc(&dev->io_writes);
		sector += bv.bv_len >> SECTOR_SHIFT;
	}
}

/*
 * Drop one logical block.  At ratio 1 the unit IS the block, so erase the
 * slot.  At ratio>1 clear the block's validity bit and erase the unit only
 * when no bit remains.  Counts one discarded block per call that found
 * data.  Used by both discard and swap-slot-free.
 */
static void cellzram_drop_block(struct cellzram *dev, unsigned long block_nr)
{
	unsigned long unit_nr = cellzram_unit_of(block_nr);
	void *old;

	if (cellzram_unit_sectors() > 1) {
		unsigned int id, blk = block_nr % cellzram_unit_sectors();

		rcu_read_lock();
		{
			void *e = xa_load(&dev->table, unit_nr);

			id = e ? xa_to_value(e) : 0;
		}
		rcu_read_unlock();
		if (!id)
			return;
		shmem_comp_entry_get(id, NULL, NULL, NULL, NULL, NULL);
		shmem_comp_entry_sector_clear(id, blk);
		atomic64_inc(&dev->discarded_blocks);
		if (!shmem_comp_entry_any_valid(id)) {
			old = xa_erase(&dev->table, unit_nr);
			if (old)
				cellzram_put_slot(old);
		}
		shmem_comp_entry_put(id);
		return;
	}
	old = xa_erase(&dev->table, unit_nr);
	if (old) {
		cellzram_put_slot(old);
		atomic64_inc(&dev->discarded_blocks);
	}
}

/*
 * Discard frees blocks synchronously: each entry_put drops the table
 * reference, and the cell returns to the buddy inline when its last
 * object goes. ext4-style workloads see MemFree move before the
 * deleting syscall returns.
 */
static void cellzram_bio_discard(struct cellzram *dev, struct bio *bio)
{
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned long start_block = sector >> SECTOR_SHIFT_BITS;
	/* bi_size is in bytes: convert to sectors before >> 3 */
	unsigned long end_block = (sector + (bio->bi_iter.bi_size >> SECTOR_SHIFT)) >>
						SECTOR_SHIFT_BITS;
	unsigned long block_nr;

	/*
	 * Discard is whole-unit aligned (discard_granularity == physical),
	 * so erasing by unit index is correct.  Iterating blocks and
	 * mapping each to its unit keeps the original `< end_block` guard,
	 * which is safe against a zero-length bio (`end_block - 1` would
	 * underflow and sweep the whole table); at ratio 1 unit_nr ==
	 * block_nr, so this is byte-identical to the old loop.  Repeated
	 * units erase once and then find nothing -- idempotent.  The
	 * partial-unit case (punch-hole -> WRITE_ZEROES) is step 4's: it
	 * clears one bitmap bit instead of dropping the unit.
	 */
	for (block_nr = start_block; block_nr < end_block; block_nr++)
		cellzram_drop_block(dev, block_nr);
}

static void cellzram_submit_bio(struct bio *bio)
{
	struct cellzram *dev = bio->bi_bdev->bd_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		cellzram_bio_read(dev, bio);
		break;
	case REQ_OP_WRITE:
		cellzram_bio_write(dev, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		cellzram_bio_discard(dev, bio);
		break;
	default:
		bio->bi_status = BLK_STS_IOERR;
		break;
	}
	bio_endio(bio);
}

/*
 * Swap reclaim: the swap code calls this on every freed slot
 * regardless of swapon flags. Each notify maps the slot index to a
 * block number (PAGE_SIZE swap slots == 4 KiB logical blocks, so the
 * numbers coincide) and drops the entry: the cell returns to the
 * buddy before the freeing reclaim pass completes. This is the
 * synchronous reclamation zram's architecture lacks.
 */
static void cellzram_slot_free_notify(struct block_device *bdev,
				      unsigned long block_nr)
{
	struct cellzram *dev = bdev->bd_disk->private_data;

	cellzram_drop_block(dev, block_nr);
}

static const struct block_device_operations cellzram_devops = {
	.submit_bio		= cellzram_submit_bio,
	.swap_slot_free_notify	= cellzram_slot_free_notify,
	.owner			= THIS_MODULE,
};

/* ------------------------------------------------------------------ */
/* sysfs: /sys/block/cellzramN/                                       */
/* ------------------------------------------------------------------ */

static ssize_t disksize_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;

	return sysfs_emit(buf, "%llu\n", cz->logical_bytes);
}

static ssize_t disksize_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t len_bytes)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;
	struct shmem_comp_pool *pool;
	u64 size_bytes;
	u64 cell_bytes;
	u64 nr_cells;
	char *algo;

	size_bytes = memparse(buf, NULL);
	if (!size_bytes || (size_bytes & (PAGE_SIZE - 1)))
		return -EINVAL;

	mutex_lock(&cz->io_lock);
	if (cz->logical_bytes) {
		mutex_unlock(&cz->io_lock);
		return -EBUSY;
	}

	/*
	 * The pool is created here, on first use. shmem_comp_pool_
	 * create() validates the algorithm with crypto_has_acomp(),
	 * which "none" fails -- in raw mode the pool is created with
	 * lz4 anyway and the compressed flag alone gates crypto (the
	 * contexts idle; noted inefficiency, see plan Open Questions).
	 */
	algo = cz->algo[0] ? cz->algo : "lz4";

	/*
	 * Size the cell directory for the advertised capacity: a block
	 * device must be able to store its whole disksize even when not
	 * one byte compresses (incompressible blocks are stored raw at
	 * PAGE_SIZE each, one cell region apiece).  Headroom covers the
	 * COW-before-free churn described at cell_headroom_pct.
	 *
	 * There is NO clamp on max_cells inside cell_pool_create -- it
	 * takes the hint as given, and the directory array is
	 * kvcalloc(nslots, sizeof(struct cell)).  So the real ceiling on
	 * incompressible capacity is the array allocation itself, and it
	 * scales with 1/cell_size: order 2 (16 KiB cells) needs 64x more
	 * slots than order 6 for the same disksize.  A large device at a
	 * small cell_order can exhaust memory on the directory alone
	 * before storing a byte.  Size cell_order with the device.
	 */
	cell_bytes = (u64)PAGE_SIZE << cell_order;
	nr_cells = DIV_ROUND_UP(size_bytes, cell_bytes);
	nr_cells += DIV_ROUND_UP(nr_cells * cell_headroom_pct, 100);
	if (nr_cells > UINT_MAX)
		nr_cells = UINT_MAX;	/* cell_pool_create clamps to U16_MAX */

	pool = shmem_comp_pool_create(algo, cell_order, nr_cells);
	if (IS_ERR(pool)) {
		mutex_unlock(&cz->io_lock);
		return PTR_ERR(pool);
	}
	/*
	 * Must precede the first entry alloc: the ratio sizes every
	 * entry's validity bitmap and is not retroactive.  At ratio 1
	 * this costs zero bitmap words, so the default path allocates
	 * byte-identical entries to a pool without the field.
	 */
	shmem_comp_pool_set_unit_ratio(pool, cellzram_unit_ratio);
	/*
	 * Staged-write bound.  Dormant at open_units == 0 (the default):
	 * no opens[] array is allocated and the device behaves
	 * byte-for-byte as before.  See mission_log/2026-10-10_ftl-
	 * open-cell-buffer.md.
	 */
	shmem_comp_pool_set_open_max(pool, open_units);

	cz->pool = pool;
	cz->logical_bytes = size_bytes;
	set_capacity(cz->disk, size_bytes >> SECTOR_SHIFT);
	pr_info("%s: disksize=%llu algo=%s cell_order=%d\n",
		cz->disk->disk_name, size_bytes, cz->compressed ? algo : "none",
		cell_order);
	mutex_unlock(&cz->io_lock);
	return len_bytes;
}
static DEVICE_ATTR_RW(disksize);

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;

	return sysfs_emit(buf, "%s\n", cz->compressed ? cz->algo : "none");
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len_bytes)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;
	char algo[CRYPTO_MAX_ALG_NAME];
	size_t n = strscpy(algo, buf, sizeof(algo));

	if (n < 0)
		return -EINVAL;
	algo[strcspn(algo, "\n")] = '\0';

	mutex_lock(&cz->io_lock);
	/* switchable only before first use or after reset */
	if (cz->logical_bytes) {
		mutex_unlock(&cz->io_lock);
		return -EBUSY;
	}
	if (!strcmp(algo, "none")) {
		cz->compressed = false;
		strscpy(cz->algo, "lz4", sizeof(cz->algo));
	} else if (crypto_has_acomp(algo, 0, 0)) {
		cz->compressed = true;
		strscpy(cz->algo, algo, sizeof(cz->algo));
	} else {
		mutex_unlock(&cz->io_lock);
		return -EINVAL;
	}
	mutex_unlock(&cz->io_lock);
	return len_bytes;
}
static DEVICE_ATTR_RW(comp_algorithm);

static ssize_t mm_stat_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;
	unsigned long long orig_bytes, compr_bytes = 0, phys_bytes = 0;
	unsigned long long objects = 0, entry_bytes, zero_blocks = 0;
	unsigned long long unit_live = 0;
	unsigned long long gc_recoveries;
	unsigned long idx;
	void *entry;

	/*
	 * The block table is the state; these numbers are walked from
	 * it, not maintained on the write path.  Incremental accounting
	 * here drifted: the zero-block path erased the slot and added
	 * orig_bytes, so a repeated zero write added it again with
	 * nothing to subtract, a data write over a zero block
	 * double-counted, and zero_blocks never decremented on
	 * overwrite or discard.  The walk is O(live blocks) and a
	 * sysfs read is rare -- correctness beats speed on this path.
	 *
	 * io_lock serialises against reset and remove, which free the
	 * pool; the walk itself runs under RCU against concurrent
	 * bios, and an entry freed mid-walk is skipped by its failed
	 * pin (the snapshot is consistent per entry, not atomic across
	 * them -- nothing in the write path takes a lock covering all
	 * blocks).
	 */
	mutex_lock(&cz->io_lock);
	rcu_read_lock();
	xa_for_each(&cz->table, idx, entry) {
		unsigned int id = xa_to_value(entry);
		unsigned int len;

		if (!id) {	/* zero-block sentinel */
			zero_blocks++;
			continue;
		}
		if (shmem_comp_entry_get(id, NULL, &len, NULL, NULL, NULL)) {
			objects++;
			compr_bytes += len;	/* staged entries report 0 */
			if (cellzram_unit_sectors() > 1)
				unit_live += shmem_comp_entry_valid_count(id);
			shmem_comp_entry_put(id);
		}
	}
	rcu_read_unlock();

	gc_recoveries = shmem_comp_pool_gc_recoveries(cz->pool);
	if (cz->pool && shmem_comp_pool_uses_cells(cz->pool))
		phys_bytes = (unsigned long long)
			atomic_long_read(&shmem_comp_pool_cells(cz->pool)->
							     pages_used)
			<< PAGE_SHIFT;
	mutex_unlock(&cz->io_lock);

	/* staged (uncompressed) buffers are RAM the vs-zram comparison must
	 * see; neither cell storage nor compressed data. */
	phys_bytes += shmem_comp_pool_staged_bytes(cz->pool);

	if (cellzram_unit_sectors() > 1)
		orig_bytes = unit_live << PAGE_SHIFT;
	else
		orig_bytes = (objects + zero_blocks) << PAGE_SHIFT;
	/*
	 * entry_bytes is reported separately from phys_used: phys_used
	 * is cell storage only, so the vs-zram comparison stays
	 * like-for-like (zram's mem_used_total includes its per-page
	 * table; we account ours explicitly).
	 */
	entry_bytes = objects * sizeof(struct shmem_comp_entry);

	return sysfs_emit(buf, "%llu %llu %llu %llu %llu %llu %llu %llu\n",
			  orig_bytes, compr_bytes, phys_bytes, objects, entry_bytes,
			  compr_bytes ? compr_bytes * 10000 / orig_bytes : 0,
			  zero_blocks, gc_recoveries);
}
static DEVICE_ATTR_RO(mm_stat);

/*
 * Named io_stat, not stat: the block layer owns "stat" on every
 * gendisk (/sys/block/X/stat); a device attribute with that name
 * makes device_add_disk() fail with -EEXIST. zram uses io_stat for
 * the same reason.
 */
static ssize_t io_stat_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;

	return sysfs_emit(buf, "%llu %llu %llu %llu\n",
			  atomic64_read(&cz->discarded_blocks),
			  atomic64_read(&cz->io_reads),
			  atomic64_read(&cz->io_writes),
			  atomic64_read(&cz->failed_writes));
}
static DEVICE_ATTR_RO(io_stat);

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len_bytes)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct cellzram *cz = disk->private_data;
	unsigned int memflags;
	unsigned long block_nr;
	void *entry;

	if (!sysfs_streq(buf, "1") && !sysfs_streq(buf, "y"))
		return -EINVAL;

	mutex_lock(&cz->io_lock);
	/*
	 * Freeze the queue first: bio submission holds a
	 * q_usage_counter reference, so when the freeze completes no
	 * bio is in flight and the table erase below cannot race a
	 * read or write. Capacity goes to 0 before the pool is
	 * destroyed so nothing can arrive afterwards.
	 */
	memflags = blk_mq_freeze_queue(cz->disk->queue);
	set_capacity(cz->disk, 0);
	cz->logical_bytes = 0;

	while ((entry = xa_find(&cz->table, &block_nr, ULONG_MAX,
				XA_PRESENT))) {
		void *old = xa_erase(&cz->table, block_nr);

		if (old)
			cellzram_put_slot(old);
	}

	if (cz->pool) {
		shmem_comp_pool_put(cz->pool);
		cz->pool = NULL;
	}

	atomic64_set(&cz->io_reads, 0);
	atomic64_set(&cz->io_writes, 0);
	atomic64_set(&cz->failed_writes, 0);
	atomic64_set(&cz->discarded_blocks, 0);

	blk_mq_unfreeze_queue(cz->disk->queue, memflags);
	mutex_unlock(&cz->io_lock);
	return len_bytes;
}
static DEVICE_ATTR_WO(reset);

static struct attribute *cellzram_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_comp_algorithm.attr,
	&dev_attr_mm_stat.attr,
	&dev_attr_io_stat.attr,
	&dev_attr_reset.attr,
	NULL,
};

ATTRIBUTE_GROUPS(cellzram);

/* ------------------------------------------------------------------ */
/* device lifecycle                                                    */
/* ------------------------------------------------------------------ */

static int cellzram_add(int id)
{
	struct queue_limits lim = {
		.logical_block_size	= logical_block_size,
		.physical_block_size	= physical_block_size,
		.io_min			= physical_block_size,
		.io_opt			= physical_block_size,
		.max_hw_discard_sectors	= UINT_MAX,
		/*
	 * discard_granularity is raised to physical_block_size by
	 * blk_validate_limits(), so discards arrive whole-unit
	 * aligned.  Punch-hole reaches us as WRITE_ZEROES, which
	 * the block layer does NOT align to physical -- that path
	 * needs the per-unit bitmap to clear a single logical
	 * sector without recompressing.
	 */
		.discard_granularity	= physical_block_size,
		/*
		 * Punch-hole (fallocate) reaches the device as
		 * REQ_OP_WRITE_ZEROES through the BLKDEV_ZERO_NOFALLBACK
		 * path, which requires hardware write-zeroes offload --
		 * discard limits alone get -EOPNOTSUPP. The unmap variant
		 * is what makes zeroing release storage like discard.
		 */
		.max_write_zeroes_sectors = UINT_MAX,
		.max_hw_wzeroes_unmap_sectors = UINT_MAX,
		.features		= BLK_FEAT_STABLE_WRITES |
					  BLK_FEAT_SYNCHRONOUS,
	};
	struct cellzram *cz;
	int ret;

	cz = kzalloc(sizeof(*cz), GFP_KERNEL);
	if (!cz)
		return -ENOMEM;

	xa_init(&cz->table);
	mutex_init(&cz->io_lock);
	strscpy(cz->algo, "lz4", sizeof(cz->algo));
	cz->compressed = true;

	cz->disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(cz->disk)) {
		ret = PTR_ERR(cz->disk);
		goto out_free_dev;
	}

	cz->disk->major = cellzram_major;
	cz->disk->first_minor = id;
	cz->disk->minors = 1;
	cz->disk->flags |= GENHD_FL_NO_PART;
	cz->disk->fops = &cellzram_devops;
	cz->disk->private_data = cz;
	snprintf(cz->disk->disk_name, sizeof(cz->disk->disk_name),
		 CELLZRAM_NAME "%d", id);

	set_capacity(cz->disk, 0);
	ret = device_add_disk(NULL, cz->disk, cellzram_groups);
	if (ret)
		goto out_cleanup_disk;

	cellzram_devs[id] = cz;
	pr_info("Added device: %s\n", cz->disk->disk_name);
	return 0;

out_cleanup_disk:
	put_disk(cz->disk);
out_free_dev:
	kfree(cz);
	return ret;
}

static void cellzram_remove(struct cellzram *cz)
{
	unsigned int memflags;
	unsigned long block_nr;
	void *entry;

	mutex_lock(&cz->io_lock);
	memflags = blk_mq_freeze_queue(cz->disk->queue);
	set_capacity(cz->disk, 0);
	cz->logical_bytes = 0;
	while ((entry = xa_find(&cz->table, &block_nr, ULONG_MAX,
				XA_PRESENT))) {
		void *old = xa_erase(&cz->table, block_nr);

		if (old)
			cellzram_put_slot(old);
	}
	if (cz->pool) {
		shmem_comp_pool_put(cz->pool);
		cz->pool = NULL;
	}
	blk_mq_unfreeze_queue(cz->disk->queue, memflags);
	mutex_unlock(&cz->io_lock);

	device_del(disk_to_dev(cz->disk));
	put_disk(cz->disk);
	kfree(cz);
}

static int __init cellzram_init(void)
{
	int i, ret;

	if (nr_devices < 1 || nr_devices > 256) {
		pr_err(CELLZRAM_NAME ": nr_devices must be 1..256\n");
		return -EINVAL;
	}
	if (cell_order < CELL_ORDER_MIN || cell_order > CELL_ORDER_MAX) {
		pr_err(CELLZRAM_NAME ": cell_order must be %d..%d\n",
		       CELL_ORDER_MIN, CELL_ORDER_MAX);
		return -EINVAL;
	}
	if (cell_headroom_pct < 0 || cell_headroom_pct > 100) {
		pr_err(CELLZRAM_NAME ": cell_headroom_pct must be 0..100\n");
		return -EINVAL;
	}
	if (!is_power_of_2(logical_block_size) ||
	    blk_validate_block_size(logical_block_size)) {
		pr_err(CELLZRAM_NAME
		       ": logical_block_size must be a power of 2 in 512..%lu\n",
		       (unsigned long)BLK_MAX_BLOCK_SIZE);
		return -EINVAL;
	}
	if (!is_power_of_2(physical_block_size) ||
	    physical_block_size < logical_block_size) {
		pr_err(CELLZRAM_NAME
		       ": physical_block_size must be a power of 2 >= logical_block_size\n");
		return -EINVAL;
	}
	/*
	 * The unit is stored as one cell region, so it must fit in a
	 * cell.  This is the real ceiling on physical -- the block
	 * layer sets none -- and it bounds the unit bitmap at
	 * physical/logical bits.
	 */
	if (physical_block_size > (PAGE_SIZE << cell_order)) {
		pr_err(CELLZRAM_NAME
		       ": physical_block_size %d exceeds cell size %lu\n",
		       physical_block_size,
		       (unsigned long)PAGE_SIZE << cell_order);
		return -EINVAL;
	}
	if (open_units < 0 || open_units > OPEN_UNITS_MAX) {
		pr_err(CELLZRAM_NAME ": open_units must be 0..%d\n",
		       OPEN_UNITS_MAX);
		return -EINVAL;
	}
	cellzram_unit_ratio = physical_block_size / logical_block_size;
	/*
	 * Step 2 rekeys the block table by unit index.  At ratio 1 that is
	 * the identity, but at ratio > 1 the RMW that makes it safe does
	 * not exist until step 4 -- a write to logical block N would land
	 * in unit N/ratio and clobber its siblings.  Reject until then so
	 * every commit in the series is correct on its own.
	 */
	if (cellzram_unit_sectors() > 1 && open_units <= 0) {
		pr_err(CELLZRAM_NAME
		       ": physical > logical requires open_units > 0 (the open-cell buffer)\n");
		return -EINVAL;
	}
	pr_info(CELLZRAM_NAME
		": logical=%d physical=%d unit_ratio=%u open_units=%d%s\n",
		logical_block_size, physical_block_size,
		cellzram_unit_ratio, open_units,
		cellzram_unit_ratio == 1 ? " (split disabled)" : "");

	cellzram_major = register_blkdev(0, CELLZRAM_NAME);
	if (cellzram_major < 0)
		return cellzram_major;

	cellzram_devs = kcalloc(nr_devices, sizeof(*cellzram_devs),
				GFP_KERNEL);
	if (!cellzram_devs) {
		ret = -ENOMEM;
		goto out_unregister;
	}

	for (i = 0; i < nr_devices; i++) {
		ret = cellzram_add(i);
		if (ret)
			goto out_remove;
	}

	pr_info(CELLZRAM_NAME ": %d device(s) registered (major %d)\n",
		nr_devices, cellzram_major);
	return 0;

out_remove:
	while (--i >= 0)
		cellzram_remove(cellzram_devs[i]);
	kfree(cellzram_devs);
out_unregister:
	unregister_blkdev(cellzram_major, CELLZRAM_NAME);
	return ret;
}

static void __exit cellzram_exit(void)
{
	int i;

	for (i = 0; i < nr_devices; i++)
		if (cellzram_devs[i])
			cellzram_remove(cellzram_devs[i]);
	kfree(cellzram_devs);
	unregister_blkdev(cellzram_major, CELLZRAM_NAME);
}

module_init(cellzram_init);
module_exit(cellzram_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cell-backed compressed RAM block device");

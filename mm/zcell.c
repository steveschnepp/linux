// SPDX-License-Identifier: GPL-2.0
/*
 * zcell.c - Cell-backed compressed RAM block device
 *
 * A zram-style compressed-RAM block device whose storage backend is the
 * shmem_comp engine on the cell allocator (use_cells), not zsmalloc.
 * It is intended to be used as swap (mkswap/swapon) on RAM-constrained
 * systems, where the cell allocator's zero per-object metadata and
 * synchronous whole-cell return to the buddy hold more data per RAM byte
 * than zram's zsmalloc pools.
 *
 * This file is the THIN frontend only.  Everything that manages the
 * compressed-storage lifecycle is reused from the shmem_comp engine:
 *   - a PRIVATE shmem_comp_pool per device (created on disksize=)
 *   - shmem_comp_compress/decompress   (sync compression, HIGHMEM-safe)
 *   - shmem_comp_store/load            (handle-keyed storage, cell backend)
 *   - the reverse index            (so the engine's 1 Hz GC can
 *                                   relocate this device's blocks)
 *   - the shrinker / watermark / cell GC
 * The device adds only: a per-device DENSE block table, the bio
 * path, and the swap-slot-free hook.
 *
 * The block table is a preallocated array of struct zcell_slot,
 * indexed by block number: block_nr COMPUTES the slot address -- no
 * xarray, no per-block allocation, no pointer chase, the metadata is
 * one array index away.  Each slot embeds the engine's
 * shmem_comp_loc_node (handle, length, refs, reverse-index node), so
 * GC relocation stays transparent exactly as the old
 * entry-pointer-in-table design made it: the engine moves
 * node->handle in place and every reader of the slot loads the fresh
 * location.
 *
 * Ratio 1: one logical block (PAGE_SIZE) = one compression unit.  mTHP
 * folio grouping (compress a whole swap folio as one unit) is a later
 * step; see mission_log/2026-10-16_zcell-zram-reexploration.md.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/cell_store.h>
#include <linux/shmem_compress.h>

#define ZCELL_NAME		"zcell"
#define ZCELL_ALGO_MAX		64
/* bi_sector is 512-byte; a logical block is one PAGE_SIZE page. */
#define ZCELL_BLOCK_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)

/*
 * One slot per logical block.  The loc node is embedded DIRECTLY (not
 * pointed at): computing the slot from block_nr is cheaper than any RAM
 * lookup of a pointer, and there is no per-block allocation to make or
 * free.  @present is the reader-visible liveness flag; it and the
 * install/drop transitions are guarded by @lock.  GC never takes @lock
 * and never touches @present -- it relocates storage behind a live
 * block by updating loc.handle in place.
 *
 * Generation protocol (why one refs counter is safe on a reused slot):
 * a new generation is installed only when present == false, refs == 0
 * and handle == 0 (EMPTY).  A dying generation keeps its handle in the
 * slot until its last pin drops (the put frees the storage, then
 * clears the handle), so pins always drain against their own
 * generation's location -- no ABA, no refcount sharing across
 * generations.
 */
struct zcell_slot {
	struct shmem_comp_loc_node loc;
	bool present;
	spinlock_t lock;
};

/*
 * Bounded slot waits: a wait longer than this is a protocol bug, not
 * contention -- pins are held across at most one <=PAGE_SIZE
 * copy/decompress.  Report the stuck slot and panic (the rig runs
 * with panic=1, so the guest reboots and the serial log names the
 * stuck generation) instead of spinning silently forever, which is
 * what schedulable cpu_relax loops do past a hang (no hung-task, no
 * softlockup -- the KASAN zcell-boot hangs of 2026-10-17).
 */
#define ZCELL_WAIT_DEADLINE	(10 * HZ)

struct zcell;

static void zcell_wait_stuck(const char *fn, struct zcell *zc,
			     unsigned long block_nr,
			     struct zcell_slot *slot)
{
	pr_err("zcell: %s stuck slot=%lu present=%d refs=%d handle=%llx\n",
	       fn, block_nr, slot->present,
	       atomic_read(&slot->loc.refs),
	       (unsigned long long)slot->loc.handle);
	panic("zcell: slot wait exceeded %d jiffies", ZCELL_WAIT_DEADLINE);
}

struct zcell {
	struct gendisk		*disk;
	/* Private compression pool: cell backend + per-CPU contexts +
	 * reverse index + shrinker + GC. All storage lifecycle lives
	 * here, in the engine. */
	struct shmem_comp_pool	*pool;
	/*
	 * Dense block table: index == block_nr.  Allocated at disksize=
	 * (the block count is fixed then), freed at reset.  A block is
	 * live iff slot->present; unwritten blocks read as zeros.
	 */
	struct zcell_slot	*table;
	unsigned long		nr_blocks;
	u64			logical_bytes;	/* bytes; 0 until disksize= */
	bool			compressed;	/* false when algo == none */
	char			algo[ZCELL_ALGO_MAX];
	/* Accounting for mm_stat (device-local; per-pool mem comes from
	 * cell_get_total_pages()). */
	atomic64_t		orig_data_bytes;
	atomic64_t		compr_data_bytes;
	/* Event counters for io_stat: cumulative, safe to sample. */
	atomic64_t		io_reads;
	atomic64_t		io_writes;
	atomic64_t		failed_writes;
	atomic64_t		discarded_blocks;
#ifdef CONFIG_ZCELL_DEBUG_BVEC
	/* bvec size distribution (verify folio-grouping signal) */
	atomic64_t		bvec_total;
	atomic64_t		bvec_multi;
	u32			bvec_max;
	bool			bvec_reported;
#endif
	struct mutex		io_lock;	/* serialises reset + sysfs */
};

static int nr_devices = 1;
module_param(nr_devices, int, 0444);
MODULE_PARM_DESC(nr_devices, "Number of zcell devices to create");

static int cell_order = 4;	/* 64 KiB cells */
module_param(cell_order, int, 0444);
MODULE_PARM_DESC(cell_order, "alloc_pages order for device cell pools (2..12)");

static int cell_headroom_pct = 10;
module_param(cell_headroom_pct, int, 0444);
MODULE_PARM_DESC(cell_headroom_pct,
		 "Cell directory headroom over disksize (percent)");

static int gc_percent;
module_param(gc_percent, int, 0444);
MODULE_PARM_DESC(gc_percent,
		 "Sparse threshold percent (0 = engine default 50)");

static int zcell_major;
static struct zcell **zcell_devs;

/*
 * The refs==0 teardown for device slots; also installed as the engine's
 * node_put hook so the GC drain's pin drop lands here.  Releases the
 * block's storage (reverse-index unlink + cell_free: the cell returns
 * to the buddy inline when its last block goes) and marks the slot
 * empty.  Exactly one thread runs the teardown: the dec_and_test
 * winner.
 */
static void zcell_slot_put(void *priv, struct shmem_comp_loc_node *node)
{
	struct zcell *zc = priv;

	if (!atomic_dec_and_test(&node->refs))
		return;
	shmem_comp_loc_dispose(zc->pool, node);
	/*
	 * present is already false (only the durable-ref drop, which must
	 * precede the last pin, clears it); clearing the handle completes
	 * the EMPTY state the install loop waits for.
	 */
	WRITE_ONCE(node->handle, 0);
}

/* ---- block helpers ------------------------------------------------ */

static unsigned long zcell_block_of(sector_t sector)
{
	return sector >> ZCELL_BLOCK_SHIFT;
}

/* ---- data path ---------------------------------------------------- */

/*
 * Read one logical block.  @dst has room for @len_bytes (<= PAGE_SIZE).
 * Unwritten blocks read as zeros (brd semantics).
 */
static int zcell_read_block(struct zcell *zc, unsigned long block_nr,
			    void *dst, unsigned int len_bytes)
{
	struct zcell_slot *slot = &zc->table[block_nr];
	struct shmem_comp_loc_node *node = &slot->loc;
	unsigned long deadline = jiffies + ZCELL_WAIT_DEADLINE;
	int ret;

	for (;;) {
		u64 h;

		if (!READ_ONCE(slot->present))
			break;	/* empty or dying -> zeros */
		h = READ_ONCE(node->handle);
		if (h && atomic_inc_not_zero(&node->refs)) {
			/*
			 * Pin succeeded.  Re-check present: a drop or
			 * overwrite that hid the block between our handle
			 * read and the pin owns its teardown -- our pin
			 * only delays it.  Retry and observe the outcome; a
			 * pin can never be mistaken for another generation's
			 * (install waits for refs == 0).
			 */
			if (READ_ONCE(slot->present))
				goto pinned;
			zcell_slot_put(zc, node);
		}
		if (time_after(jiffies, deadline))
			zcell_wait_stuck("read", zc, block_nr, slot);
		cpu_relax();
	}

	memset(dst, 0, len_bytes);
	return 0;

pinned:
	/* Decompress directly from the cell under its rwsem -- no bounce
	 * buffer.  loc_read re-reads the handle under the cell lock and
	 * retries if GC relocated the block; the pin keeps the storage
	 * alive across that. */
	ret = shmem_comp_loc_read(zc->pool, node, dst, NULL);
	zcell_slot_put(zc, node);
	return ret;
}

static int zcell_write_block(struct zcell *zc, unsigned long block_nr,
			     const void *src)
{
	struct zcell_slot *slot = &zc->table[block_nr];
	struct shmem_comp_loc_node *node = &slot->loc;
	unsigned long deadline;
	unsigned int len_bytes;
	u64 handle;

	/* Compress + store first; the slot is untouched until the new
	 * block is safely in a cell. */
	if (zc->compressed) {
		handle = shmem_comp_compress_store(zc->pool, src, PAGE_SIZE,
						   &len_bytes);
	} else {
		handle = shmem_comp_store(zc->pool, src, PAGE_SIZE);
		len_bytes = PAGE_SIZE;
	}
	if (!handle)
		return -ENOMEM;

	deadline = jiffies + ZCELL_WAIT_DEADLINE;
	for (;;) {
		spin_lock(&slot->lock);
		if (!slot->present && !atomic_read(&node->refs) &&
		    !READ_ONCE(node->handle))
			break;	/* EMPTY: install below */
		if (slot->present) {
			/*
			 * Hide the old generation; we now own its durable
			 * ref.  The flag flip under the lock elects exactly
			 * one dropper: a concurrent swap_slot_free_notify
			 * sees present == false and returns.
			 */
			slot->present = false;
			spin_unlock(&slot->lock);
			zcell_slot_put(zc, node);
		} else {
			/* DYING (pins draining) or mid-dispose. */
			spin_unlock(&slot->lock);
		}
		/*
		 * Wait out the old generation: the last put frees its
		 * storage and clears the handle.  On the swap workload
		 * this wait never fires (slots are written once; the
		 * swap layer serialises read vs write per slot).
		 */
		while (atomic_read(&node->refs) ||
		       READ_ONCE(node->handle)) {
			if (time_after(jiffies, deadline))
				zcell_wait_stuck("write", zc, block_nr,
						  slot);
			cpu_relax();
		}
	}

	node->length = len_bytes;
	atomic_set(&node->refs, 1);	/* the durable ref */
	WRITE_ONCE(node->handle, handle);
	shmem_comp_loc_link(zc->pool, node);
	slot->present = true;
	spin_unlock(&slot->lock);

	atomic64_add(PAGE_SIZE, &zc->orig_data_bytes);
	atomic64_add(len_bytes, &zc->compr_data_bytes);
	return 0;
}

/* Drop one logical block (ratio 1: unit == block).  Clears the slot and
 * drops the table's durable ref; the cell returns to the buddy inline
 * when the last object goes. */
static void zcell_drop_block(struct zcell *zc, unsigned long block_nr,
			     bool count)
{
	struct zcell_slot *slot = &zc->table[block_nr];

	spin_lock(&slot->lock);
	if (!slot->present) {
		spin_unlock(&slot->lock);
		return;
	}
	slot->present = false;
	spin_unlock(&slot->lock);

	if (count)
		atomic64_inc(&zc->discarded_blocks);

	/*
	 * Drop the durable ref: the common case (no pins) frees the
	 * storage synchronously HERE -- the cell goes back to the buddy
	 * before the freeing reclaim pass completes, which is the
	 * synchronous reclamation zram's architecture lacks.  With pins
	 * outstanding, the last put frees instead.
	 */
	zcell_slot_put(zc, &slot->loc);
}

/* ---- bio path ----------------------------------------------------- */

static void zcell_bio_read(struct zcell *zc, struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bv;
	sector_t sector = bio->bi_iter.bi_sector;

	bio_for_each_segment(bv, bio, iter) {
		void *mem = kmap_local_page(bv.bv_page) + bv.bv_offset;
		unsigned long block_nr = zcell_block_of(sector);
		int ret;

		if (bv.bv_offset == 0 && bv.bv_len == PAGE_SIZE)
			ret = zcell_read_block(zc, block_nr, mem, bv.bv_len);
		else {
			void *tmp = kmalloc(PAGE_SIZE, GFP_NOIO);

			if (!tmp) {
				bio->bi_status = BLK_STS_RESOURCE;
				kunmap_local(mem);
				return;
			}
			ret = zcell_read_block(zc, block_nr, tmp, PAGE_SIZE);
			if (!ret)
				memcpy(mem, tmp, bv.bv_len);
			kfree(tmp);
		}
		kunmap_local(mem);
		if (ret) {
			bio->bi_status = BLK_STS_IOERR;
			return;
		}
		atomic64_inc(&zc->io_reads);
		sector += bv.bv_len >> SECTOR_SHIFT;
	}
}

static void zcell_bio_write(struct zcell *zc, struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bv;
	sector_t sector = bio->bi_iter.bi_sector;

	bio_for_each_segment(bv, bio, iter) {
		void *mem = kmap_local_page(bv.bv_page) + bv.bv_offset;
		unsigned long block_nr = zcell_block_of(sector);
		int ret;

#ifdef CONFIG_ZCELL_DEBUG_BVEC
		/* bvec size distribution (verify the folio-grouping signal:
		 * does swap ever send bv_len > PAGE_SIZE?) */
		atomic64_inc(&zc->bvec_total);
		if (bv.bv_len > PAGE_SIZE) {
			atomic64_inc(&zc->bvec_multi);
			if (bv.bv_len > zc->bvec_max)
				zc->bvec_max = bv.bv_len;
			if (!zc->bvec_reported) {
				zc->bvec_reported = true;
				pr_info("zcell: MULTI-PAGE bvec first block=%lu bv_len=%u (folio-grouping signal)\n",
					block_nr, bv.bv_len);
			}
		}
		if ((atomic64_read(&zc->bvec_total) & 0xffff) == 0)
			pr_info("zcell: bvec stats total=%lld multi=%lld max=%u\n",
				atomic64_read(&zc->bvec_total),
				atomic64_read(&zc->bvec_multi), zc->bvec_max);
#endif

		if (bv.bv_offset == 0 && bv.bv_len == PAGE_SIZE)
			ret = zcell_write_block(zc, block_nr, mem);
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
			ret = zcell_write_block(zc, block_nr, tmp);
			kfree(tmp);
		}
		kunmap_local(mem);
		if (ret) {
			atomic64_inc(&zc->failed_writes);
			bio->bi_status = BLK_STS_IOERR;
			return;
		}
		atomic64_inc(&zc->io_writes);
		sector += bv.bv_len >> SECTOR_SHIFT;
	}
}

static void zcell_bio_discard(struct zcell *zc, struct bio *bio)
{
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned long start_block = zcell_block_of(sector);
	unsigned long end_block = zcell_block_of(sector +
					(bio->bi_iter.bi_size >> SECTOR_SHIFT));
	unsigned long block_nr;

	for (block_nr = start_block; block_nr < end_block; block_nr++)
		zcell_drop_block(zc, block_nr, true);
}

static void zcell_submit_bio(struct bio *bio)
{
	struct zcell *zc = bio->bi_bdev->bd_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		zcell_bio_read(zc, bio);
		break;
	case REQ_OP_WRITE:
		zcell_bio_write(zc, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zcell_bio_discard(zc, bio);
		break;
	default:
		bio->bi_status = BLK_STS_IOERR;
		break;
	}
	bio_endio(bio);
}

/*
 * Swap reclaim: the swap code calls this on every freed slot.  Each
 * notify maps the slot index to a block number (PAGE_SIZE swap slots ==
 * PAGE_SIZE logical blocks, so the numbers coincide) and drops the
 * entry: the cell returns to the buddy before the freeing reclaim pass
 * completes.  This is the synchronous reclamation zram's architecture
 * lacks.
 */
static void zcell_slot_free_notify(struct block_device *bdev,
				   unsigned long block_nr)
{
	struct zcell *zc = bdev->bd_disk->private_data;

	zcell_drop_block(zc, block_nr, false);
}

static const struct block_device_operations zcell_devops = {
	.submit_bio		= zcell_submit_bio,
	.swap_slot_free_notify	= zcell_slot_free_notify,
	.owner			= THIS_MODULE,
};

/* ---- sysfs: /sys/block/zcellN/ ------------------------------------ */

static ssize_t disksize_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;

	return sysfs_emit(buf, "%llu\n", zc->logical_bytes);
}

static ssize_t disksize_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t len_bytes)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;
	struct shmem_comp_pool *pool;
	struct zcell_slot *table;
	unsigned long nr_blocks, i;
	u64 size_bytes;
	u64 cell_bytes;
	u64 nr_cells;
	char *algo;

	size_bytes = memparse(buf, NULL);
	if (!size_bytes || (size_bytes & (PAGE_SIZE - 1)))
		return -EINVAL;

	mutex_lock(&zc->io_lock);
	if (zc->logical_bytes) {
		mutex_unlock(&zc->io_lock);
		return -EBUSY;
	}

	/* "none" fails crypto_has_acomp(); in raw mode create the pool
	 * with lz4 anyway and the compressed flag alone gates crypto. */
	algo = zc->algo[0] ? zc->algo : "lz4";

	/* Size the cell directory for the advertised capacity: a block
	 * device must store its whole disksize even when nothing
	 * compresses (incompressible blocks are stored raw at PAGE_SIZE
	 * each).  Headroom covers COW-before-free churn. */
	cell_bytes = (u64)PAGE_SIZE << cell_order;
	/* div64_u64: on 32-bit, a runtime u64/u64 DIV_ROUND_UP emits
	 * __udivdi3, which the kernel does not link (libgcc). */
	nr_cells = div64_u64(size_bytes + cell_bytes - 1, cell_bytes);
	nr_cells += div64_u64(nr_cells * (u64)cell_headroom_pct + 99, 100);
	if (nr_cells > UINT_MAX)
		nr_cells = UINT_MAX;

	pool = shmem_comp_pool_create(algo, cell_order, nr_cells, gc_percent);
	if (IS_ERR(pool)) {
		mutex_unlock(&zc->io_lock);
		return PTR_ERR(pool);
	}

	/* Dense block table: one slot per logical block, indexed by
	 * block_nr.  Allocated here (the block count is fixed now),
	 * freed by reset. */
	nr_blocks = size_bytes >> PAGE_SHIFT;
	table = kvcalloc(nr_blocks, sizeof(*table), GFP_KERNEL);
	if (!table) {
		shmem_comp_pool_put(pool);
		mutex_unlock(&zc->io_lock);
		return -ENOMEM;
	}
	for (i = 0; i < nr_blocks; i++)
		spin_lock_init(&table[i].lock);

	/* The engine's GC drain drops its pins for this pool's bare slot
	 * nodes through our teardown (not the entry free path). */
	shmem_comp_pool_set_node_put(pool, zcell_slot_put, zc);

	zc->pool = pool;
	zc->table = table;
	zc->nr_blocks = nr_blocks;
	zc->logical_bytes = size_bytes;
	set_capacity(zc->disk, size_bytes >> SECTOR_SHIFT);
	pr_info("%s: disksize=%llu algo=%s cell_order=%d\n",
		zc->disk->disk_name, size_bytes,
		zc->compressed ? algo : "none", cell_order);
	mutex_unlock(&zc->io_lock);
	return len_bytes;
}
static DEVICE_ATTR_RW(disksize);

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;

	return sysfs_emit(buf, "[%s] lz4 lzo lzo-rle zstd none\n",
			  zc->algo[0] ? zc->algo : "lz4");
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;
	char algo[ZCELL_ALGO_MAX];
	size_t slen;

	slen = strscpy(algo, buf, sizeof(algo));
	if (slen < 0)
		return slen;
	/* strip trailing newline */
	if (slen && algo[slen - 1] == '\n')
		algo[slen - 1] = '\0';

	mutex_lock(&zc->io_lock);
	if (zc->logical_bytes) {	/* pool already created */
		mutex_unlock(&zc->io_lock);
		return -EBUSY;
	}
	if (!strcmp(algo, "none")) {
		zc->compressed = false;
		zc->algo[0] = '\0';
	} else {
		zc->compressed = true;
		strscpy(zc->algo, algo, sizeof(zc->algo));
	}
	mutex_unlock(&zc->io_lock);
	return len;
}
static DEVICE_ATTR_RW(comp_algorithm);

static ssize_t mm_stat_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;
	u64 orig, compr, mem_used = 0;

	orig = atomic64_read(&zc->orig_data_bytes);
	compr = atomic64_read(&zc->compr_data_bytes);
	if (zc->pool)
		mem_used = (u64)cell_get_total_pages(
				shmem_comp_pool_cells(zc->pool)) << PAGE_SHIFT;

	/* orig compr mem_used orig/compr ratio per-10000 */
	return sysfs_emit(buf,
			  "%llu %llu %llu %llu\n",
			  orig, compr, mem_used,
			  orig ? div64_u64(compr * 10000, orig) : 0);
}
static DEVICE_ATTR_RO(mm_stat);

static ssize_t io_stat_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;

	return sysfs_emit(buf,
			  "%llu %llu %llu %llu\n",
			  atomic64_read(&zc->io_reads),
			  atomic64_read(&zc->io_writes),
			  atomic64_read(&zc->failed_writes),
			  atomic64_read(&zc->discarded_blocks));
}
static DEVICE_ATTR_RO(io_stat);

/*
 * Quiesce and empty a device: hide + release every block, wait out
 * outstanding pins, drop the pool and the table.  Caller holds io_lock
 * and must have frozen the queue first (no bios in flight; only
 * transient GC pins can remain).
 */
static void zcell_reset_locked(struct zcell *zc)
{
	unsigned long block_nr;

	set_capacity(zc->disk, 0);
	zc->logical_bytes = 0;

	for (block_nr = 0; block_nr < zc->nr_blocks; block_nr++)
		zcell_drop_block(zc, block_nr, false);

	/*
	 * Wait for outstanding pins (finished readers -- the queue is
	 * frozen -- and transient GC pins) so no teardown can run after
	 * the pool is dropped: the last put dereferences zc->pool.
	 */
	for (block_nr = 0; block_nr < zc->nr_blocks; block_nr++) {
		struct zcell_slot *slot = &zc->table[block_nr];
		unsigned long deadline = jiffies + ZCELL_WAIT_DEADLINE;

		while (atomic_read(&slot->loc.refs) ||
		       READ_ONCE(slot->loc.handle)) {
			if (time_after(jiffies, deadline))
				zcell_wait_stuck("reset", zc, block_nr, slot);
			cpu_relax();
		}
	}

	if (zc->pool) {
		shmem_comp_pool_put(zc->pool);
		zc->pool = NULL;
	}
	kvfree(zc->table);
	zc->table = NULL;
	zc->nr_blocks = 0;
	atomic64_set(&zc->orig_data_bytes, 0);
	atomic64_set(&zc->compr_data_bytes, 0);
}

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;
	unsigned int memflags;

	mutex_lock(&zc->io_lock);
	memflags = blk_mq_freeze_queue(zc->disk->queue);
	zcell_reset_locked(zc);
	blk_mq_unfreeze_queue(zc->disk->queue, memflags);
	mutex_unlock(&zc->io_lock);
	return len;
}
static DEVICE_ATTR_WO(reset);

static struct attribute *zcell_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_comp_algorithm.attr,
	&dev_attr_mm_stat.attr,
	&dev_attr_io_stat.attr,
	&dev_attr_reset.attr,
	NULL,
};

static const struct attribute_group zcell_attr_group = {
	.attrs = zcell_attrs,
};

static const struct attribute_group *zcell_groups[] = {
	&zcell_attr_group,
	NULL,
};

/* ---- device lifecycle --------------------------------------------- */

static int zcell_add(int id)
{
	struct queue_limits lim = {
		.logical_block_size	= PAGE_SIZE,
		.physical_block_size	= PAGE_SIZE,
		.io_min			= PAGE_SIZE,
		.io_opt			= PAGE_SIZE,
		.max_hw_discard_sectors	= UINT_MAX,
		.discard_granularity	= PAGE_SIZE,
		.max_write_zeroes_sectors = UINT_MAX,
		.max_hw_wzeroes_unmap_sectors = UINT_MAX,
		.features		= BLK_FEAT_STABLE_WRITES |
					  BLK_FEAT_SYNCHRONOUS,
	};
	struct zcell *zc;
	int ret;

	zc = kzalloc(sizeof(*zc), GFP_KERNEL);
	if (!zc)
		return -ENOMEM;

	mutex_init(&zc->io_lock);
	strscpy(zc->algo, "lz4", sizeof(zc->algo));
	zc->compressed = true;

	zc->disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(zc->disk)) {
		ret = PTR_ERR(zc->disk);
		goto out_free_dev;
	}

	zc->disk->major = zcell_major;
	zc->disk->first_minor = id;
	zc->disk->minors = 1;
	zc->disk->flags |= GENHD_FL_NO_PART;
	zc->disk->fops = &zcell_devops;
	zc->disk->private_data = zc;
	snprintf(zc->disk->disk_name, sizeof(zc->disk->disk_name),
		 ZCELL_NAME "%d", id);

	set_capacity(zc->disk, 0);
	ret = device_add_disk(NULL, zc->disk, zcell_groups);
	if (ret)
		goto out_cleanup_disk;

	zcell_devs[id] = zc;
	pr_info("Added device: %s\n", zc->disk->disk_name);
	return 0;

out_cleanup_disk:
	put_disk(zc->disk);
out_free_dev:
	kfree(zc);
	return ret;
}

static void zcell_remove(struct zcell *zc)
{
	unsigned int memflags;

	mutex_lock(&zc->io_lock);
	memflags = blk_mq_freeze_queue(zc->disk->queue);
	zcell_reset_locked(zc);
	blk_mq_unfreeze_queue(zc->disk->queue, memflags);
	mutex_unlock(&zc->io_lock);

	device_del(disk_to_dev(zc->disk));
	put_disk(zc->disk);
	kfree(zc);
}

static int __init zcell_init(void)
{
	int i, ret;

	if (nr_devices < 1 || nr_devices > 256) {
		pr_err(ZCELL_NAME ": nr_devices must be 1..256\n");
		return -EINVAL;
	}
	if (cell_order < CELL_ORDER_MIN || cell_order > CELL_ORDER_MAX) {
		pr_err(ZCELL_NAME ": cell_order must be %d..%d\n",
		       CELL_ORDER_MIN, CELL_ORDER_MAX);
		return -EINVAL;
	}
	if (cell_headroom_pct < 0 || cell_headroom_pct > 100) {
		pr_err(ZCELL_NAME ": cell_headroom_pct must be 0..100\n");
		return -EINVAL;
	}
	if (gc_percent && (gc_percent < 10 || gc_percent > 90)) {
		pr_err(ZCELL_NAME ": gc_percent must be 0 or 10..90\n");
		return -EINVAL;
	}

	zcell_major = register_blkdev(0, ZCELL_NAME);
	if (zcell_major < 0)
		return zcell_major;

	zcell_devs = kcalloc(nr_devices, sizeof(*zcell_devs), GFP_KERNEL);
	if (!zcell_devs) {
		ret = -ENOMEM;
		goto out_unregister;
	}

	for (i = 0; i < nr_devices; i++) {
		ret = zcell_add(i);
		if (ret)
			goto out_remove;
	}

	pr_info(ZCELL_NAME ": %d device(s) registered (major %d)\n",
		nr_devices, zcell_major);
	return 0;

out_remove:
	while (--i >= 0)
		zcell_remove(zcell_devs[i]);
	kfree(zcell_devs);
out_unregister:
	unregister_blkdev(zcell_major, ZCELL_NAME);
	return ret;
}

static void __exit zcell_exit(void)
{
	int i;

	for (i = 0; i < nr_devices; i++)
		if (zcell_devs[i])
			zcell_remove(zcell_devs[i]);
	kfree(zcell_devs);
	unregister_blkdev(zcell_major, ZCELL_NAME);
}

module_init(zcell_init);
module_exit(zcell_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cell-backed compressed RAM block device (swap)");

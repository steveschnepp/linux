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
 *   - the entry table + reverse index  (so the engine's 1 Hz GC can
 *                                       relocate this device's blocks)
 *   - the shrinker / watermark / cell GC
 * The device adds only: a per-device block table (block_nr -> entry
 * pointer), the bio path, and the swap-slot-free hook.
 *
 * Block table stores the struct shmem_comp_entry * DIRECTLY (not an id).
 * That makes GC relocation transparent: the engine moves ent->handle in
 * place, and a reader holding the pointer always loads the fresh handle.
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
#include <linux/xarray.h>
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

struct zcell {
	struct gendisk		*disk;
	/* Private compression pool: cell backend + per-CPU contexts +
	 * reverse index + shrinker + GC. All storage lifecycle lives
	 * here, in the engine. */
	struct shmem_comp_pool	*pool;
	/* block table: block_nr -> struct shmem_comp_entry *.  Sparse;
	 * unwritten blocks have no slot and read as zeros. */
	struct xarray		table;
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

/* ---- block helpers ------------------------------------------------ */

static unsigned long zcell_block_of(sector_t sector)
{
	return sector >> ZCELL_BLOCK_SHIFT;
}

/* ---- entry helpers ------------------------------------------------ */

/* The table owns the entry's durable (slot) reference.  Dropping it on
 * overwrite / discard / swap-free releases that ref; the entry (and its
 * cell region) frees when the last ref of any kind drops. */
static void zcell_put_entry(void *entry)
{
	struct shmem_comp_entry *ent = entry;

	if (ent)
		shmem_comp_entry_release(ent);
}

/* ---- data path ---------------------------------------------------- */

/*
 * Read one logical block.  @dst has room for @len_bytes (<= PAGE_SIZE).
 * Unwritten blocks read as zeros (brd semantics).
 */
static int zcell_read_block(struct zcell *zc, unsigned long block_nr,
			    void *dst, unsigned int len_bytes)
{
	struct shmem_comp_entry *ent;
	unsigned int dlen;
	int ret;

	/* Pin the entry: xa_load + entry_get inside one RCU section.
	 * Retry while the entry is dying (an overwrite/swap-free raced
	 * us); the racing path erases or replaces the slot, so this
	 * converges to a live entry or to NULL (empty). */
	for (;;) {
		bool dying = false;

		rcu_read_lock();
		ent = xa_load(&zc->table, block_nr);
		if (ent && !shmem_comp_entry_get(ent))
			dying = true;
		rcu_read_unlock();
		if (!dying)
			break;
		cpu_relax();
	}

	if (!ent) {
		memset(dst, 0, len_bytes);
		return 0;
	}

	/* ent->length is the STORED length, immutable after alloc (only
	 * the handle moves on GC), so it is safe under the pin. */
	dlen = ent->length;
	if (dlen == PAGE_SIZE) {
		/* raw block: load straight into the destination */
		unsigned int l = len_bytes;

		ret = shmem_comp_load_entry(ent, dst, &l);
	} else {
		void *src = kmalloc(PAGE_SIZE, GFP_NOIO);
		unsigned int l = dlen;

		if (!src) {
			shmem_comp_entry_put(ent);
			return -ENOMEM;
		}
		ret = shmem_comp_load_entry(ent, src, &l);
		if (!ret) {
			unsigned int out = PAGE_SIZE;

			ret = shmem_comp_decompress(zc->pool, src, dlen,
						    dst, &out);
		}
		kfree(src);
	}
	shmem_comp_entry_put(ent);
	return ret;
}

static int zcell_write_block(struct zcell *zc, unsigned long block_nr,
			     const void *src)
{
	struct shmem_comp_entry *ent, *old;
	unsigned int len_bytes = PAGE_SIZE;
	const void *data = src;
	void *cbuf = NULL;
	u64 handle;
	int ret;

	if (zc->compressed) {
		cbuf = kmalloc(PAGE_SIZE, GFP_NOIO);
		if (!cbuf)
			return -ENOMEM;
		ret = shmem_comp_compress(zc->pool, src, PAGE_SIZE, cbuf,
					  &len_bytes);
		if (ret)
			len_bytes = PAGE_SIZE;	/* -ENOSPC: store raw */
		else
			data = cbuf;
	}

	handle = shmem_comp_store(zc->pool, data, len_bytes);
	if (!handle) {
		kfree(cbuf);
		return -ENOMEM;
	}

	ent = shmem_comp_entry_alloc(zc->pool, handle, len_bytes, 1, NULL, 0);
	if (!ent) {
		shmem_comp_free(zc->pool, handle);
		kfree(cbuf);
		return -ENOMEM;
	}
	kfree(cbuf);

	/* Atomic replace-and-return-old (no xa_exchange).  The new
	 * entry's slot ref is now owned by the table; the old entry's
	 * slot ref is dropped here. */
	old = xa_store(&zc->table, block_nr, ent, GFP_NOIO);
	if (IS_ERR(old)) {
		/* never installed: release the slot ref we hold, which
		 * frees the entry and its storage. */
		shmem_comp_entry_release(ent);
		return PTR_ERR(old);
	}
	if (old)
		zcell_put_entry(old);

	atomic64_add(PAGE_SIZE, &zc->orig_data_bytes);
	atomic64_add(len_bytes, &zc->compr_data_bytes);
	return 0;
}

/* Drop one logical block (ratio 1: unit == block).  Erases the slot and
 * releases the table's durable ref; the cell returns to the buddy inline
 * when the last object goes. */
static void zcell_drop_block(struct zcell *zc, unsigned long block_nr,
			     bool count)
{
	void *old = xa_erase(&zc->table, block_nr);

	if (old) {
		zcell_put_entry(old);
		if (count)
			atomic64_inc(&zc->discarded_blocks);
	}
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

	zc->pool = pool;
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

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct zcell *zc = disk->private_data;
	unsigned long block_nr;
	unsigned int memflags;
	void *entry;

	mutex_lock(&zc->io_lock);
	memflags = blk_mq_freeze_queue(zc->disk->queue);
	set_capacity(zc->disk, 0);
	zc->logical_bytes = 0;
	while ((entry = xa_find(&zc->table, &block_nr, ULONG_MAX,
				XA_PRESENT))) {
		void *old = xa_erase(&zc->table, block_nr);

		if (old)
			zcell_put_entry(old);
	}
	if (zc->pool) {
		shmem_comp_pool_put(zc->pool);
		zc->pool = NULL;
	}
	atomic64_set(&zc->orig_data_bytes, 0);
	atomic64_set(&zc->compr_data_bytes, 0);
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

	xa_init(&zc->table);
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
	unsigned long block_nr;
	void *entry;

	mutex_lock(&zc->io_lock);
	memflags = blk_mq_freeze_queue(zc->disk->queue);
	set_capacity(zc->disk, 0);
	zc->logical_bytes = 0;
	while ((entry = xa_find(&zc->table, &block_nr, ULONG_MAX,
				XA_PRESENT))) {
		void *old = xa_erase(&zc->table, block_nr);

		if (old)
			zcell_put_entry(old);
	}
	if (zc->pool) {
		shmem_comp_pool_put(zc->pool);
		zc->pool = NULL;
	}
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

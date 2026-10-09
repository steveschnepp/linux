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
	 * block_nr -> entry id. Sparse by nature: unwritten blocks
	 * have no slot and read as zeros. Values only (entry ids via
	 * xa_mk_value), so no pointer/value mixing.
	 */
	struct xarray		table;
	u64			logical_bytes;	/* bytes; 0 until disksize set */
	bool			compressed;	/* false when algo == none */
	char			algo[CRYPTO_MAX_ALG_NAME];
	/* stats for mm_stat / stat */
	atomic64_t		orig_bytes;
	atomic64_t		compr_bytes;
	atomic64_t		objects;
	atomic64_t		io_reads;
	atomic64_t		io_writes;
	atomic64_t		failed_writes;
	atomic64_t		discarded_blocks;
	/*
	 * all-zero writes stored as nothing (see cellzram_write_block):
	 * a zero block reads back identically to an unwritten one via the
	 * read-miss zero-fill, so no slot/entry/cell is held. Reported in
	 * mm_stat to mirror zram's same_pages for the head-to-head.
	 */
	atomic64_t		zero_blocks;
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
static int cell_headroom_pct = 30;
module_param(cell_headroom_pct, int, 0444);
MODULE_PARM_DESC(cell_headroom_pct,
		 "Cell directory headroom over disksize (percent)");

static int cellzram_major;

static struct cellzram **cellzram_devs;

/*
 * Drop one block-table reference on @id: account the stored length
 * (read under the entry pin, which cannot fail -- we own the table
 * reference) then release the table reference, which frees the cell
 * region when it was the last one.
 */
static void cellzram_put_entry(struct cellzram *dev, unsigned int id)
{
	unsigned int len_bytes = 0;

	/* get() cannot fail: the caller owns the table reference */
	if (shmem_comp_entry_get(id, NULL, &len_bytes, NULL, NULL, NULL)) {
		atomic64_sub(len_bytes, &dev->compr_bytes);
		shmem_comp_entry_put(id);	/* drop the pin */
	}
	atomic64_dec(&dev->objects);
	/* every entry represents one logical block (nr_pages == 1) */
	atomic64_sub(PAGE_SIZE, &dev->orig_bytes);
	shmem_comp_entry_put(id);		/* drop the table reference */
}

/*
 * Read one logical block. @dst has room for @len_bytes bytes (<= PAGE_SIZE).
 * Unwritten blocks read as zeros (brd semantics).
 */
static int cellzram_read_block(struct cellzram *dev, unsigned long block_nr,
			       void *dst, unsigned int len_bytes)
{
	unsigned int id, dlen_bytes;
	void *src;
	int ret;

	rcu_read_lock();
	for (;;) {
		void *entry = xa_load(&dev->table, block_nr);

		if (!entry) {
			rcu_read_unlock();
			memset(dst, 0, len_bytes);
			return 0;
		}
		id = xa_to_value(entry);
		/*
		 * Pin before touching storage: a concurrent overwrite
		 * (xa_exchange + put) may be freeing this entry and its
		 * cell region right now. Retry the lookup if we lost.
		 */
		if (shmem_comp_entry_get(id, NULL, &dlen_bytes, NULL, NULL, NULL))
			break;
	}
	rcu_read_unlock();

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
 * Write one full logical block. Stores raw in phase-1 mode
 * (compressed == false); phase 2 wires compression in with the entry
 * length as the discriminator. Replaces any previous block (COW): the
 * old entry is put after the exchange, so a reader pinned on it keeps
 * its data alive until it is done.
 */
static int cellzram_write_block(struct cellzram *dev, unsigned long block_nr,
				const void *src)
{
	/*
	 * all-zero page: store nothing, exactly as discard does (erase the
	 * slot, drop the ref, cell returns to the buddy inline).  A zero
	 * block reads back as zeros via the read-miss path, indistinguishable
	 * from an unwritten block, so we hold no slot/entry/cell for it.
	 * Filesystem metadata (unused inode tables, bitmaps, padding) is
	 * zero-heavy, so this turns ext4's own zero writes into free storage
	 * without the fs having to issue a discard.  Net win: a 512-word scan
	 * replaces lz4 + store + entry_alloc.  Applies in raw (none) mode too.
	 */
	if (cellzram_page_all_zero(src)) {
		void *old = xa_erase(&dev->table, block_nr);

		if (old)
			cellzram_put_entry(dev, xa_to_value(old));
		atomic64_add(PAGE_SIZE, &dev->orig_bytes);
		atomic64_inc(&dev->zero_blocks);
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

	/* live accounting: one logical block, @len_bytes stored bytes */
	atomic64_add(PAGE_SIZE, &dev->orig_bytes);
	atomic64_add(len_bytes, &dev->compr_bytes);
	atomic64_inc(&dev->objects);

	/*
	 * Atomic replace-and-return-old (this tree has no xa_exchange):
	 * readers may see either value; the pin protocol in
	 * cellzram_read_block() covers the swap.
	 */
	old = xa_store(&dev->table, block_nr, xa_mk_value(id), GFP_NOIO);
	if (old)
		cellzram_put_entry(dev, xa_to_value(old));
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

	for (block_nr = start_block; block_nr < end_block; block_nr++) {
		void *old = xa_erase(&dev->table, block_nr);

		if (old) {
			cellzram_put_entry(dev, xa_to_value(old));
			atomic64_inc(&dev->discarded_blocks);
		}
	}
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
	void *old = xa_erase(&dev->table, block_nr);

	if (old) {
		cellzram_put_entry(dev, xa_to_value(old));
		atomic64_inc(&dev->discarded_blocks);
	}
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
	 * COW-before-free churn described at cell_headroom_pct.  The
	 * hint is clamped to U16_MAX inside cell_pool_create, which
	 * caps incompressible storage at U16_MAX x cell_size beyond
	 * that -- warned there, at the only place the cap is known.
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
	unsigned long long orig_bytes, compr_bytes, phys_bytes = 0, objects, entry_bytes;
	unsigned long long zero_blocks;

	orig_bytes = atomic64_read(&cz->orig_bytes);
	compr_bytes = atomic64_read(&cz->compr_bytes);
	objects = atomic64_read(&cz->objects);
	zero_blocks = atomic64_read(&cz->zero_blocks);
	if (cz->pool && shmem_comp_pool_uses_cells(cz->pool))
		phys_bytes = (unsigned long long)
			atomic_long_read(&shmem_comp_pool_cells(cz->pool)->
							     pages_used)
			<< PAGE_SHIFT;
	/*
	 * entry_bytes is reported separately from phys_used: phys_used
	 * is cell storage only, so the vs-zram comparison stays
	 * like-for-like (zram's mem_used_total includes its per-page
	 * table; we account ours explicitly).
	 */
	entry_bytes = objects * sizeof(struct shmem_comp_entry);

	return sysfs_emit(buf, "%llu %llu %llu %llu %llu %llu %llu\n",
			  orig_bytes, compr_bytes, phys_bytes, objects, entry_bytes,
			  compr_bytes ? compr_bytes * 10000 / orig_bytes : 0,
			  zero_blocks);
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
			cellzram_put_entry(cz, xa_to_value(old));
	}

	if (cz->pool) {
		shmem_comp_pool_put(cz->pool);
		cz->pool = NULL;
	}

	atomic64_set(&cz->orig_bytes, 0);
	atomic64_set(&cz->compr_bytes, 0);
	atomic64_set(&cz->objects, 0);
	atomic64_set(&cz->io_reads, 0);
	atomic64_set(&cz->io_writes, 0);
	atomic64_set(&cz->failed_writes, 0);
	atomic64_set(&cz->discarded_blocks, 0);
	atomic64_set(&cz->zero_blocks, 0);

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
		.logical_block_size	= PAGE_SIZE,
		.physical_block_size	= PAGE_SIZE,
		.io_min			= PAGE_SIZE,
		.io_opt			= PAGE_SIZE,
		.max_hw_discard_sectors	= UINT_MAX,
		.discard_granularity	= PAGE_SIZE,
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
			cellzram_put_entry(cz, xa_to_value(old));
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

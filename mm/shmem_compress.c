// SPDX-License-Identifier: GPL-2.0
/*
 * mm/shmem_compress.c - Transparent compression for tmpfs
 *
 * This file implements the compression pool infrastructure for tmpfs
 * transparent page-level compression.
 *
 * The compression subsystem provides:
 * - Per-CPU compression contexts using kernel crypto API (LZ4/LZO)
 * - zsmalloc-based storage for compressed page data (cell storage via
 *   mm/cell_store.c when the cell= mount option is used)
 * - Pool lifecycle management (create/destroy)
 * - Page-cache integration: compress on write, decompress on access
 * - Per-mount state (struct shmem_comp_sb) and mount-option parsing
 * - Global defaults + sysfs interface (/sys/fs/tmpfs, group
 *   "compression")
 * - Statistics tracking for sysfs reporting
 *
 * Carve contract (see TMPFS_COMPRESSION_PLAN.md, "Upstream Shape"):
 * this translation unit is the sole in-tree consumer of the exported
 * shmem primitives (shmem_recalc_inode, shmem_add_to_page_cache,
 * shmem_delete_from_page_cache, shmem_alloc_folio -- each exported
 * from mm/shmem.c inside #ifdef CONFIG_TMPFS_COMPRESSION). Adding a
 * second consumer of those exports is a design change, not a
 * convenience. mm/shmem.c keeps only the dispatch hooks (write_end,
 * get_folio_gfp, free_swap), the mount-option ABI table and the
 * opts/fill_sb plumbing.
 *
 * Ownership rules (details in include/linux/shmem_compress.h):
 * - sbinfo->comp owns its pool and compressor string; freed by
 *   shmem_comp_put_super(), safe on partial init (mount failure).
 * - shmem_comp_opts owns its compressor string; freed by
 *   shmem_comp_opts_free() at fs_context teardown.
 * - Sysfs registration is driven by tmpfs_sysfs_init() in mm/shmem.c
 *   with the tmpfs kobject as parent; best-effort (tmpfs works
 *   without the group). The defaults globals live here, guarded by
 *   shmem_comp_defaults_lock.
 *
 * Design Notes:
 * - Compression uses the crypto acomp (asynchronous compression) API
 * - Each CPU has its own compression context to avoid lock contention
 * - zsmalloc provides efficient variable-size allocation for compressed data
 * - Similar architecture to zswap but integrated directly into tmpfs
 *
 * Compression Flow:
 *   1. Page is compressed via shmem_comp_compress()
 *   2. Compressed data stored via shmem_comp_store() (zsmalloc handle
 *      or cell {cellid, offset}, depending on the pool backend)
 *   3. Page cache slot replaced with the entry ID (SWP_SHMEM_COMPRESS)
 *
 * Decompression Flow:
 *   1. xarray entry ID decoded via shmem_comp_entry_id()
 *   2. Data loaded via shmem_comp_load() / shmem_comp_load_entry()
 *   3. Page decompressed via shmem_comp_decompress()
 *
 * Performance Considerations:
 * - Decompression is the hot path (page faults, reads)
 * - Compression happens on writeback (less latency-sensitive)
 * - LZ4 provides fastest decompression for 4KB pages
 * - Per-CPU contexts eliminate lock contention
 *
 * See also:
 * - include/linux/shmem_compress.h (interfaces)
 * - mm/zswap.c (similar compression for swap)
 * - mm/zsmalloc.c (compressed data storage)
 */

#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/overflow.h>
#include <linux/bitmap.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/zsmalloc.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/workqueue.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <crypto/acompress.h>
#include <linux/shmem_compress.h>
#include <linux/shmem_fs.h>
#include <linux/cell_store.h>
#include <linux/rwsem.h>
#include <linux/refcount.h>
#include "internal.h"	/* find_get_entries() */

#define cell_dbg(fmt, ...) \
	pr_debug("shmem_comp: " fmt "\n", ##__VA_ARGS__)

/* GC implementation lives after the entry table (needs the xarray) */
static void shmem_comp_cell_gc(struct shmem_comp_pool *pool);
static void shmem_comp_open_free_slot(struct shmem_comp_pool *pool,
				      unsigned int idx);
static unsigned long shmem_comp_cell_count(struct shrinker *shrinker,
					   struct shrink_control *sc);
static unsigned long shmem_comp_cell_scan(struct shrinker *shrinker,
					  struct shrink_control *sc);

/* Stage 2: walk-based aging (deferred compression) */
static void shmem_comp_maintain(struct shmem_comp_sb *comp);
static void shmem_comp_age_work(struct work_struct *work);

/*
 * Per-CPU compression context.
 *
 * Each CPU maintains its own compression context to avoid lock contention
 * during compression/decompression operations. The crypto API requires
 * per-CPU contexts for efficient asynchronous operation.
 *
 * The buffer is used as scratch space for compression output before
 * it's copied to the zsmalloc pool.
 */
struct shmem_comp_pcpu {
	struct crypto_acomp *acomp;	/* crypto compression tfm */
	struct acomp_req *req;		/* compression request */
	struct crypto_wait wait;	/* async wait queue */
	struct mutex lock;		/* protects req usage */
};

/*
 * Compression pool structure.
 *
 * A pool combines zsmalloc storage with per-CPU compression contexts.
 * Multiple tmpfs mounts can share a pool, or each can have its own.
 *
 * Pool Lifecycle:
 *   create() → [in use] → destroy()
 *
 * Reference Counting:
 * Pools use simple refcounting to support sharing between mounts.
 * When refcount reaches zero, the pool is destroyed.
 */
/*
 * Per-mount compression state. Created by shmem_comp_fill_sb(), freed
 * by shmem_comp_put_super(); reachable from mm/shmem.c only through
 * the opaque pointer shmem_sb_info.comp.
 *
 * Stage 2 (walk-based aging) adds the deferred-compression policy
 * state: a hot/chill mode flag read on the write path, and a periodic
 * maintenance pass that ages eligible survivors. See
 * TMPFS_COMPRESSION_PLAN.md "Stage 2 machinery".
 */
struct shmem_comp_sb {
	bool enabled;
	char *compressor;		/* owned; freed with the sb */
	unsigned int threshold_bytes;		/* min file size to compress (write time), bytes */
	unsigned int watermark_low;		/* chill return, percent usage (0:0 = eager) */
	unsigned int watermark_high;		/* hot compress, percent usage */
	unsigned int merge_pages;		/* pages per compression group */
	unsigned int cell_order;	/* cell storage order, informational */
	struct shmem_comp_pool *pool;	/* compression pool */
	atomic_long_t pages;		/* compressed pages (this mount) */
	atomic_long_t bytes;		/* compressed bytes stored (this mount) */
	/* Stage 2: deferred-compression policy state */
	struct super_block *sb;		/* owning sb (inode walk, usage, s_umount) */
	bool hot;			/* mode flag: WRITE_ONCE in maintain,
					 * READ_ONCE on the write path */
	struct delayed_work age_work;	/* per-mount maintenance pass */
	struct list_head list;		/* on shmem_comp_mounts */
	atomic_long_t aged_pages;	/* pages compressed by the aging pass */
	atomic_long_t skipped_dead;	/* chilled pages freed without compression */
};

/*
 * One staged (uncompressed) compression unit: a buffer of `unit_ratio *
 * PAGE_SIZE` bytes holding a not-yet-compressed cellzram unit.  Lives in
 * the pool's bounded opens[] array and is reached from an entry handle
 * whose bit 63 is set (index in the low bits).  The rwsem mirrors a
 * cell's: a staged reader holds it down_read across its memcpy; GC's
 * staged drain (a later step) takes down_write to exclude readers while
 * it flips the handle and frees the buffer -- the same contract
 * shmem_comp_load_entry() relies on for cells.
 */
struct shmem_comp_open {
	struct page		*page;	/* compound page, the unit buffer */
	unsigned int		order;	/* alloc order of @page */
	struct rw_semaphore	lock;	/* staged readers vs drain */
	refcount_t		refs;	/* pinned staged readers */
	bool			in_use;
	struct list_head	lru;	/* pool->open_lru when in_use */
};

struct shmem_comp_pool {
	/*
	 * Storage backing.  Exactly one of zs_pool / cells is non-NULL;	 * use_cells selects which.  The choice is made at pool creation
	 * time (driven by the cell= mount option) and never changes.	 * Both backends implement the same store/load/free contract:	 * store returns an opaque handle, load copies data out, free	 * releases the storage.  Callers dispatch on use_cells without	 * needing to know which backend is active.	 */
	struct zs_pool *zs_pool;		/* zsmalloc storage, NULL if cell-backed */
	struct cell_pool *cells;		/* cell storage, NULL if zsmalloc-backed */
	bool use_cells;
	struct shmem_comp_pcpu __percpu *pcpu;	/* per-CPU compression contexts */
	char algo_name[CRYPTO_MAX_ALG_NAME];	/* algorithm name */
	atomic_t refcount;			/* shared across mounts using this pool */
	/*
	 * Writes that ran out of directory slots and were saved by the
	 * exhaustion GC pass.  A counter, not a gauge: it answers "how
	 * often would this write have failed before session 25", which
	 * is the number a headroom sweep needs.  Appended to cellzram's
	 * mm_stat so the field order of existing parsers stays stable.
	 */
	atomic64_t gc_recoveries;
	/*
	 * Logical sectors per compression unit (the logical/physical
	 * split).  1 = split disabled, which is every pool except a
	 * cellzram device configured with physical > logical.  Drives
	 * the size of shmem_comp_entry.valid and which code paths may
	 * touch it.
	 */
	unsigned int unit_ratio;
	/*
	 * Staged-write buffer bound (the FTL open-cell model): the max
	 * number of uncompressed unit buffers a cellzram device may hold
	 * open at once.  0 = staging disabled, which is every pool except
	 * a cellzram device configured with open_units > 0; tmpfs pools
	 * never stage.  Set once at setup (disksize_store) before io, like
	 * unit_ratio.  The opens[] array is allocated here, once, in the
	 * single-threaded setup path (disksize_store) -- a pool that never
	 * stages (open_max == 0) allocates nothing beyond this scalar,
	 * which kzalloc already zeroed.
	 */
	unsigned int open_max;
	/*
	 * Staged open-buffer slots (open-cell buffer).  opens[] has
	 * open_max entries, allocated in set_open_max() when open_max > 0.
	 * open_lock protects in_use/page/refs/lru/open_count and the LRU
	 * order; it is a leaf with respect to a slot's own rwsem and to
	 * cp->lock.
	 */
	struct shmem_comp_open	*opens;
	unsigned int		open_count;
	spinlock_t		open_lock;
	struct list_head	open_lru;	/* MRU head -> victim tail */
	struct list_head list;			/* global pools list */
};

/* Global list of all compression pools */
static LIST_HEAD(shmem_comp_pools);
/* Protects shmem_comp_pools list modifications */
static DEFINE_SPINLOCK(shmem_comp_pools_lock);
/* Counter for generating unique pool names */
static atomic_t shmem_comp_pools_count = ATOMIC_INIT(0);

/*
 * Global list of compressed mounts (struct shmem_comp_sb.list), used by
 * gc_now to run a maintenance pass on every participating mount. A
 * MUTEX (not a spinlock): shmem_comp_maintain() sleeps (crypto, page
 * cache walks). Taken by the aging work, gc_now, and put_super -- never
 * by a shrinker or any reclaim path, so holding it across a GFP_KERNEL
 * allocation inside maintain() cannot deadlock against reclaim.
 */
static LIST_HEAD(shmem_comp_mounts);
static DEFINE_MUTEX(shmem_comp_mounts_lock);

/*
 * Async shrinker trigger (see shmem_comp_pressure_pass definition for
 * the why).
 */
static void shmem_comp_pressure_pass(struct work_struct *work);
static DECLARE_WORK(shmem_comp_pressure_work, shmem_comp_pressure_pass);


/* Global tallies for the aging policy (comp_stats output) */
static atomic_long_t shmem_comp_aged_pages;
static atomic_long_t shmem_comp_skipped_dead;

/*
 * Aging-pass tuning. The budget bounds the crypto work per maintenance
 * pass so a mount full of incompressible data cannot monopolize the
 * CPU; the batch bounds how many inodes are pinned at once.
 */
#define SHMEM_COMP_AGE_BUDGET		128U	/* pages per round */
#define SHMEM_COMP_AGE_BATCH_INODES	64U	/* inodes pinned per pass */
#define SHMEM_COMP_AGE_DELAY_MS		1000U	/* armed-work delay */
#define SHMEM_COMP_AGE_MAX_ROUNDS	4U	/* rounds per pass (~2 MiB) */

/* Global statistics */
static struct shmem_comp_stats shmem_comp_global_stats = {
	.compressed_pages = 0,
	.uncompressed_pages = 0,
	.total_compressed_bytes = 0,
	.total_uncompressed_bytes = 0,
	.ratio = 0,
	.compress_fails = 0,
	.decompress_fails = 0,
};

/* Spinlock protecting statistics updates */
static DEFINE_SPINLOCK(shmem_comp_stats_lock);

/*
 * Live RAM footprint accounting for the ram_* fields of
 * struct shmem_comp_stats. Byte counters use ksize() so they reflect
 * what the allocator actually handed out (slab buckets, page-order
 * rounding for large kmallocs), not the requested size.
 */
static atomic_long_t shmem_comp_ram_merged_bytes = ATOMIC_INIT(0);
static atomic_long_t shmem_comp_ram_entry_bytes = ATOMIC_INIT(0);
static atomic_t shmem_comp_ram_entries = ATOMIC_INIT(0);

/*
 * Per-CPU context management.
 */

/**
 * shmem_comp_pcpu_init - Initialize per-CPU compression context
 * @pcpu: Per-CPU context to initialize
 * @algo: Compression algorithm name
 *
 * Return: 0 on success, negative errno on failure
 *
 * Allocates crypto compression tfm and request for this CPU.
 * The tfm is specific to the algorithm and CPU.
 */
static int shmem_comp_pcpu_init(struct shmem_comp_pcpu *pcpu,
				const char *algo)
{
	int ret;

	memset(pcpu, 0, sizeof(*pcpu));
	mutex_init(&pcpu->lock);

	/* Allocate crypto compression tfm for this CPU */
	pcpu->acomp = crypto_alloc_acomp(algo, 0, 0);
	if (IS_ERR(pcpu->acomp)) {
		ret = PTR_ERR(pcpu->acomp);
		pcpu->acomp = NULL;
		return ret;
	}

	/* Allocate compression request */
	pcpu->req = acomp_request_alloc(pcpu->acomp);
	if (!pcpu->req) {
		crypto_free_acomp(pcpu->acomp);
		pcpu->acomp = NULL;
		return -ENOMEM;
	}

	/* Initialize crypto wait queue */
	crypto_init_wait(&pcpu->wait);

	return 0;
}

/**
 * shmem_comp_pcpu_free - Free per-CPU compression context
 * @pcpu: Per-CPU context to free
 *
 * Releases all resources associated with the per-CPU context.
 */
static void shmem_comp_pcpu_free(struct shmem_comp_pcpu *pcpu)
{
	if (pcpu->req) {
		acomp_request_free(pcpu->req);
		pcpu->req = NULL;
	}

	if (pcpu->acomp) {
		crypto_free_acomp(pcpu->acomp);
		pcpu->acomp = NULL;
	}
}

/*
 * Pool lifecycle management.
 */

/**
 * shmem_comp_pool_create - Create a compression pool
 * @algo: Compression algorithm name ("lz4", "lzo", or "lzo-rle")
 * @cell_order: Cell size as alloc_pages order, or 0 for zsmalloc
 * @nr_cells: Cell directory capacity hint, or 0 for the default
 *
 * Return: Pointer to pool on success, ERR_PTR on failure
 *
 * Creates a new compression pool with the specified algorithm.
 * Allocates:
 * - zsmalloc pool for compressed data storage
 * - Per-CPU compression contexts
 *
 * @nr_cells bounds physical storage for cell pools (nr_cells x
 * cell_size); see cell_pool_create().  A caller that knows its
 * capacity -- a block device with a disksize -- passes it; a caller
 * that does not (tmpfs) passes 0.
 *
 * The pool is added to the global list for tracking.
 * Callers must call shmem_comp_pool_put() when done.
 */
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo,
						unsigned int cell_order,
						unsigned int nr_cells)
{
	struct shmem_comp_pool *pool;
	int cpu;
	int ret;
	int pool_id;

	/* Validate algorithm */
	if (!crypto_has_acomp(algo, 0, 0)) {
		pr_warn("shmem_comp: algorithm '%s' not available\n", algo);
		return ERR_PTR(-ENODEV);
	}

	/* Allocate pool structure */
	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	/* Store algorithm name */
	strscpy(pool->algo_name, algo, sizeof(pool->algo_name));

	/* Initialize refcount */
	atomic_set(&pool->refcount, 1);

	/* Cell storage: no zsmalloc pool needed */
	if (cell_order > 0) {
		pool->cells = cell_pool_create(cell_order, cell_order >= 4,
					       nr_cells);
		if (!pool->cells) {
			pr_warn("shmem_comp: failed to create cell pool\n");
			kfree(pool);
			return ERR_PTR(-ENOMEM);
		}
		pool->use_cells = true;
		pool->zs_pool = NULL;

		/*
		 * Register the shrinker: sparse cells are reclaimed
		 * only under memory pressure — the free path never
		 * evacuates synchronously so that one GC pass can
		 * batch many cells into a single entry-table scan.
		 */
		pool->cells->shrinker = shrinker_alloc(0,
			"tmpfs-comp-cell:%s", pool->algo_name);
		if (pool->cells->shrinker) {
			pool->cells->shrinker->count_objects =
				shmem_comp_cell_count;
			pool->cells->shrinker->scan_objects =
				shmem_comp_cell_scan;
			pool->cells->shrinker->private_data = pool;
			shrinker_register(pool->cells->shrinker);
		} else {
			pr_warn("shmem_comp: cell shrinker alloc failed; "
				"sparse cells wait for pool teardown\n");
		}

		pr_info("shmem_comp: cell storage enabled, order=%u (%lu KiB/cell)\n",
			cell_order, (PAGE_SIZE << cell_order) / 1024);
	} else {
		/* Create zsmalloc pool for compressed data storage */
		pool_id = atomic_inc_return(&shmem_comp_pools_count);
		pool->zs_pool = zs_create_pool(algo);
		if (!pool->zs_pool) {
			pr_warn("shmem_comp: failed to create zsmalloc pool\n");
			kfree(pool);
			return ERR_PTR(-ENOMEM);
		}
		pool->use_cells = false;
		pool->cells = NULL;
	}

	/* Allocate per-CPU contexts */
	pool->pcpu = alloc_percpu(struct shmem_comp_pcpu);
	if (!pool->pcpu) {
		zs_destroy_pool(pool->zs_pool);
		kfree(pool);
		return ERR_PTR(-ENOMEM);
	}

	/* Initialize per-CPU contexts */
	for_each_possible_cpu(cpu) {
		struct shmem_comp_pcpu *pcpu = per_cpu_ptr(pool->pcpu, cpu);

		ret = shmem_comp_pcpu_init(pcpu, algo);
		if (ret) {
			/* Clean up already-initialized CPUs */
			int i;

			for_each_possible_cpu(i) {
				if (i >= cpu)
					break;
				shmem_comp_pcpu_free(per_cpu_ptr(pool->pcpu, i));
			}
			free_percpu(pool->pcpu);
			zs_destroy_pool(pool->zs_pool);
			kfree(pool);
			return ERR_PTR(ret);
		}
	}

	/* Add to global list */
	spin_lock(&shmem_comp_pools_lock);
	list_add_tail(&pool->list, &shmem_comp_pools);
	spin_unlock(&shmem_comp_pools_lock);

	pr_info("shmem_comp: created pool %d with algorithm '%s'\n",
		pool_id, algo);

	return pool;
}

/**
 * shmem_comp_pool_destroy - Destroy a compression pool
 * @pool: Pool to destroy
 *
 * Frees all resources associated with the pool:
 * - Per-CPU compression contexts
 * - zsmalloc pool
 * - Pool structure
 *
 * Note: All compressed data in the pool is lost.
 * The caller must ensure no other references exist.
 */
void shmem_comp_pool_destroy(struct shmem_comp_pool *pool)
{
	int cpu;

	if (!pool)
		return;

	/* Remove from global list */
	spin_lock(&shmem_comp_pools_lock);
	list_del(&pool->list);
	spin_unlock(&shmem_comp_pools_lock);

	/* Free per-CPU contexts */
	if (pool->pcpu) {
		for_each_possible_cpu(cpu)
			shmem_comp_pcpu_free(per_cpu_ptr(pool->pcpu, cpu));
		free_percpu(pool->pcpu);
	}

	/* Destroy storage pool */
	if (pool->cells) {
		if (pool->cells->shrinker)
			shrinker_free(pool->cells->shrinker);
		cell_pool_destroy(pool->cells);
	}
	if (pool->zs_pool)
		zs_destroy_pool(pool->zs_pool);

	pr_info("shmem_comp: destroyed pool with algorithm '%s'\n",
		pool->algo_name);

	kfree(pool);
}

bool shmem_comp_pool_uses_cells(struct shmem_comp_pool *pool)
{
	return pool && pool->use_cells;
}

struct cell_pool *shmem_comp_pool_cells(struct shmem_comp_pool *pool)
{
	return pool ? pool->cells : NULL;
}

/*
 * shmem_comp_pool_gc_recoveries - writes saved by the exhaustion pass
 * Cumulative since pool creation; see the field's comment.
 */
unsigned long shmem_comp_pool_gc_recoveries(struct shmem_comp_pool *pool)
{
	if (!pool)
		return 0;
	return (unsigned long)atomic64_read(&pool->gc_recoveries);
}

unsigned int shmem_comp_pool_cell_pages(struct shmem_comp_pool *pool)
{
	if (!pool || !pool->use_cells || !pool->cells)
		return 0;
	return 1U << pool->cells->cell_order;
}

/*
 * shmem_comp_pool_set_unit_ratio - set logical sectors per unit
 * @pool: target pool
 * @ratio: logical blocks per compression unit; 1 = split disabled
 *
 * Must be called before the first entry is allocated on @pool: the
 * ratio sizes every entry's validity bitmap at alloc time and is not
 * retroactive.  A ratio-1 pool allocates zero bitmap words, which is
 * why the default path is byte-identical to a pool without the field.
 */
void shmem_comp_pool_set_unit_ratio(struct shmem_comp_pool *pool,
				    unsigned int ratio)
{
	if (pool)
		pool->unit_ratio = ratio ? ratio : 1;
}

/*
 * shmem_comp_pool_set_open_max - set the staged-write buffer bound
 * @pool: target pool
 * @open_max: max simultaneous uncompressed staging buffers; 0 = off
 *
 * Must be called before the first stage on @pool (it bounds the opens[]
 * array).  A 0 value keeps staging disabled and is byte-for-byte the
 * pre-open-cell behaviour, which is what makes this a no-op until a
 * device asks for buffers.
 */
void shmem_comp_pool_set_open_max(struct shmem_comp_pool *pool,
				  unsigned int open_max)
{
	unsigned int i;

	if (!pool)
		return;
	pool->open_max = open_max;
	if (!open_max)
		return;	/* staging disabled */

	/*
	 * Allocate the bounded open-buffer array once, here in the
	 * single-threaded setup path, so the write path never races the
	 * lazy init.  On failure degrade to staging off (open_max = 0):
	 * cellzram then reads 0 and stores units compressed, which is
	 * correct, just without the write-gathering benefit.
	 */
	pool->opens = kcalloc(open_max, sizeof(*pool->opens), GFP_KERNEL);
	if (!pool->opens) {
		pool->open_max = 0;
		return;
	}
	INIT_LIST_HEAD(&pool->open_lru);
	spin_lock_init(&pool->open_lock);
	for (i = 0; i < open_max; i++)
		init_rwsem(&pool->opens[i].lock);
}

unsigned int shmem_comp_pool_open_max(struct shmem_comp_pool *pool)
{
	return pool ? pool->open_max : 0;
}

/**
 * shmem_comp_pool_put - Release reference to a pool
 * @pool: Pool to release
 *
 * Decrements the pool refcount. If refcount reaches zero, the pool
 * is destroyed.
 */
void shmem_comp_pool_put(struct shmem_comp_pool *pool)
{
	if (!pool)
		return;

	if (atomic_dec_and_test(&pool->refcount))
		shmem_comp_pool_destroy(pool);
}

/*
 * XArray entry type checking.
 */

/**
 * shmem_comp_type_check - Check if xa_value is a compression entry
 * @entry: xa_value to check
 *
 * Return: true if entry is a compressed page entry, false otherwise
 *
 * Checks if the xa_value uses the SWP_SHMEM_COMPRESS type tag.
 * This function should only be called after xa_is_value() returns true.
 *
 * The check extracts the swap type from the value and compares it
 * against SWP_SHMEM_COMPRESS. If CONFIG_TMPFS_COMPRESSION is disabled,
 * this always returns false.
 */
bool shmem_comp_type_check(const void *entry)
{
#ifdef CONFIG_TMPFS_COMPRESSION
	swp_entry_t swap;

	/* Convert xa_value back to swp_entry_t format */
	swap.val = xa_to_value(entry);

	/* Check if this is a compression entry */
	return swp_type(swap) == SWP_SHMEM_COMPRESS;
#else
	return false;
#endif
}
EXPORT_SYMBOL_GPL(shmem_comp_type_check);

/*
 * Compression/decompression operations.
 */

/**
 * shmem_comp_compress - Compress a page
 * @pool: Compression pool to use
 * @src: Source data (PAGE_SIZE bytes)
 * @dst: Destination buffer for compressed data
 * @dst_len: [in,out] Buffer size on input, compressed size on output
 *
 * Return: 0 on success, -ENOSPC if compression doesn't reduce size,
 *         other negative errno on failure
 *
 * Compresses PAGE_SIZE bytes from src to dst using the pool's algorithm.
 * If the compressed size is >= PAGE_SIZE, returns -ENOSPC to indicate
 * the data is incompressible.
 *
 * Must be called from the CPU that owns the per-CPU context (i.e.,
 * use in process context on the local CPU).
 */
int shmem_comp_compress(struct shmem_comp_pool *pool,
			const void *src, unsigned int slen,
			void *dst, unsigned int *dst_len)
{
	struct shmem_comp_pcpu *pcpu;
	struct scatterlist input, output;
	unsigned int dlen;
	ktime_t start, end;
	s64 duration;
	int ret;

	if (!pool || !src || !dst || !dst_len || slen == 0)
		return -EINVAL;

	pcpu = this_cpu_ptr(pool->pcpu);
	dlen = *dst_len;

	mutex_lock(&pcpu->lock);

	start = ktime_get();

	/* Setup scatterlists for crypto operation */
	sg_init_one(&input, src, slen);
	sg_init_one(&output, dst, dlen);

	/* Configure compression request */
	acomp_request_set_params(pcpu->req, &input, &output,
				 slen, dlen);

	/* Execute compression (synchronous wait for async op) */
	ret = crypto_wait_req(crypto_acomp_compress(pcpu->req),
			      &pcpu->wait);

	end = ktime_get();
	duration = ktime_to_ns(ktime_sub(end, start));

	if (!ret) {
		dlen = pcpu->req->dlen;
		*dst_len = dlen;

		/* Check if compression actually reduced size */
		if (dlen >= slen) {
			ret = -ENOSPC;
		}
	} else {
		/* Update failure statistics */
		spin_lock(&shmem_comp_stats_lock);
		shmem_comp_global_stats.compress_fails++;
		spin_unlock(&shmem_comp_stats_lock);
	}

	mutex_unlock(&pcpu->lock);

	return ret;
}

/**
 * shmem_comp_decompress - Decompress a page
 * @pool: Compression pool to use
 * @src: Compressed source data
 * @src_len: Compressed data size
 * @dst: Destination buffer (PAGE_SIZE bytes)
 *
 * Return: 0 on success, negative errno on failure
 *
 * Decompresses src_len bytes from src to PAGE_SIZE bytes in dst.
 *
 * This is the hot path for page faults and reads - performance is critical.
 * Must be called from the CPU that owns the per-CPU context.
 */
int shmem_comp_decompress(struct shmem_comp_pool *pool,
			  const void *src, unsigned int slen,
			  void *dst, unsigned int *dst_len)
{
	struct shmem_comp_pcpu *pcpu;
	struct scatterlist input, output;
	unsigned int dlen;
	ktime_t start, end;
	s64 duration;
	int ret;

	if (!pool || !src || !dst || !dst_len || slen == 0)
		return -EINVAL;

	pcpu = this_cpu_ptr(pool->pcpu);
	dlen = *dst_len;

	mutex_lock(&pcpu->lock);

	start = ktime_get();

	/* Setup scatterlists for crypto operation */
	sg_init_one(&input, src, slen);
	sg_init_one(&output, dst, dlen);

	/* Configure decompression request */
	acomp_request_set_params(pcpu->req, &input, &output,
				 slen, dlen);

	/* Execute decompression */
	ret = crypto_wait_req(crypto_acomp_decompress(pcpu->req),
			      &pcpu->wait);

	end = ktime_get();
	duration = ktime_to_ns(ktime_sub(end, start));

	if (!ret) {
		*dst_len = pcpu->req->dlen;
	} else {
		/* Update failure statistics */
		spin_lock(&shmem_comp_stats_lock);
		shmem_comp_global_stats.decompress_fails++;
		spin_unlock(&shmem_comp_stats_lock);
	}

	mutex_unlock(&pcpu->lock);

	return ret;
}

/*
 * zsmalloc storage operations.
 */

/**
 * shmem_comp_store - Store compressed data in pool
 * @pool: Compression pool
 * @data: Compressed data to store
 * @len: Data length in bytes
 *
 * Return: zsmalloc handle on success, 0 on failure
 *
 * Allocates space in zsmalloc pool and copies compressed data there.
 * The handle can be used to retrieve the data later via shmem_comp_load().
 *
 * Uses GFP_NOWAIT to avoid blocking in atomic contexts. If allocation
 * fails, returns 0 and caller should handle as compression failure.
 *
 * The CELL path is different: it may run one GC pass on directory
 * exhaustion, so it requires a sleepable caller.  Every cell-path
 * caller meets that today; see the use_cells branch below.
 */
unsigned long shmem_comp_store(struct shmem_comp_pool *pool,
			       const void *data, unsigned int len)
{
	unsigned long handle;
	gfp_t gfp;

	if (!pool || !data || len == 0)
		return 0;

	/*
	 * Cell storage: the bump allocator returns a packed
	 * {cellid, offset} location.  No bounce buffer, no zsmalloc
	 * handle — the data lands directly in the cell at the exact
	 * offset returned.  There is no per-object size cap beyond
	 * the cell size itself (up to 4 MiB), so merged compression
	 * groups are stored as a single block.
	 */
	if (pool->use_cells) {
		u32 cellid;
		u32 offset;

		if (!cell_store(pool->cells, data, len, &cellid, &offset))
			return shmem_comp_pack_loc(cellid, offset);

		/*
		 * Directory exhaustion.  Overwrite churn piles up
		 * partially-filled cells -- a cell only returns to the buddy
		 * when it empties completely -- so a pool can run out of
		 * slots while most of its cells are still half empty.
		 * Evacuating those is what makes room, and only this layer
		 * can do it: the victim scan needs the entry table.  One
		 * pass, then one retry, then fail.  An unbounded retry would
		 * turn memory pressure into a livelock on the write path,
		 * and when nothing sparse is left to drain the second store
		 * fails the same way -- the honest answer for a genuinely
		 * full device, matching zram.
		 *
		 * Sleepable context by contract: every cell-path caller runs
		 * in process or kworker context (cellzram kmallocs its
		 * bounce buffer with GFP_NOIO in this very path; tmpfs
		 * compresses from write_end or the aging work).  gc_lock is
		 * a mutex for the same reason.  GC itself cannot recurse
		 * back here -- it relocates through cell_relocate_one(),
		 * which never re-enters this function.
		 */
		mutex_lock(&pool->cells->gc_lock);
		shmem_comp_cell_gc(pool);
		mutex_unlock(&pool->cells->gc_lock);
		atomic64_inc(&pool->gc_recoveries);

		if (cell_store(pool->cells, data, len, &cellid, &offset))
			return 0;
		return shmem_comp_pack_loc(cellid, offset);
	}

	/* zsmalloc path: ZS_MAX_ALLOC_SIZE is PAGE_SIZE */
	if (len > PAGE_SIZE)
		return 0;

	gfp = GFP_NOWAIT | __GFP_NOWARN;
	handle = zs_malloc(pool->zs_pool, len, gfp, NUMA_NO_NODE);
	if (!handle)
		return 0;

	zs_obj_write(pool->zs_pool, handle, (void *)data, len);

	return handle;
}

/**
 * shmem_comp_load - Load compressed data from pool
 * @pool: Compression pool
 * @handle: zsmalloc handle from shmem_comp_store()
 * @dst: Destination buffer
 * @len: [in,out] Buffer size on input, data size on output
 *
 * Return: 0 on success, negative errno on failure
 *
 * Retrieves compressed data from zsmalloc pool using the handle.
 * The data is copied to dst buffer.
 */
int shmem_comp_load(struct shmem_comp_pool *pool,
		    unsigned long handle, void *dst,
		    unsigned int *len)
{
	void *bounce, *src;

	if (!pool || !dst || !len || handle == 0)
		return -EINVAL;

	/*
	 * Cell storage: the handle encodes {cellid, offset}.  The
	 * entry reference pin held by the caller guarantees the cell
	 * has not been returned to the buddy allocator.
	 */
	if (pool->use_cells) {
		u32 cellid;
		u32 offset;

		shmem_comp_unpack_loc(handle, &cellid, &offset);
		return cell_load(pool->cells, cellid, offset, dst, len);
	}

	/*
	 * zsmalloc path.  zs_obj_read_begin() returns a direct pointer
	 * when the object is contained within one page of its zspage,
	 * or copies into a bounce buffer when it spans two pages.  The
	 * bounce buffer is necessary because the two pages of a zspage
	 * are not virtually contiguous — unlike cell storage, where
	 * the pages come from a single alloc_pages() call.
	 */
	bounce = kmalloc(*len, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	src = zs_obj_read_begin(pool->zs_pool, handle, *len, bounce);
	if (!src) {
		kfree(bounce);
		return -EINVAL;
	}

	memcpy(dst, src, *len);

	zs_obj_read_end(pool->zs_pool, handle, *len, src);
	kfree(bounce);

	return 0;
}

int shmem_comp_store_merged(struct shmem_comp_pool *pool,
			    const void *data, unsigned int len,
			    unsigned long **handles_out,
			    unsigned short *nchunks_out)
{
	unsigned short nchunks, i;
	unsigned int offset;
	unsigned long *handles;

	if (!pool || !data || len == 0 || !handles_out || !nchunks_out)
		return -EINVAL;

	nchunks = DIV_ROUND_UP(len, PAGE_SIZE);
	handles = kmalloc_array(nchunks, sizeof(*handles), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;

	offset = 0;
	for (i = 0; i < nchunks; i++) {
		unsigned int chunk = min_t(unsigned int, PAGE_SIZE,
					   len - offset);

		handles[i] = shmem_comp_store(pool,
					      (const char *)data + offset,
					      chunk);
		if (!handles[i]) {
			/* free already-stored chunks */
			unsigned short j;

			for (j = 0; j < i; j++)
				shmem_comp_free(pool, handles[j]);
			kfree(handles);
			return -ENOMEM;
		}
		offset += chunk;
	}

	*handles_out = handles;
	*nchunks_out = nchunks;
	return 0;
}

int shmem_comp_load_merged(struct shmem_comp_pool *pool,
			   const unsigned long *handles,
			   unsigned short nchunks,
			   void *dst, unsigned int *len)
{
	unsigned short i;
	unsigned int offset;

	if (!pool || !handles || !dst || !len || nchunks == 0)
		return -EINVAL;

	offset = 0;
	for (i = 0; i < nchunks; i++) {
		unsigned int chunk = min_t(unsigned int, PAGE_SIZE,
					   *len - offset);
		int ret;

		ret = shmem_comp_load(pool, handles[i],
				      (char *)dst + offset, &chunk);
		if (ret)
			return ret;
		offset += chunk;
	}

	*len = offset;
	return 0;
}

void shmem_comp_free_merged(struct shmem_comp_pool *pool,
			    unsigned long *handles,
			    unsigned short nchunks)
{
	unsigned short i;

	if (!pool || !handles)
		return;

	for (i = 0; i < nchunks; i++)
		shmem_comp_free(pool, handles[i]);
	kfree(handles);
}

/**
 * shmem_comp_free - Free compressed data from pool
 * @pool: Compression pool
 * @handle: zsmalloc handle to free
 *
 * Releases the zsmalloc allocation associated with the handle.
 * The handle must not be used after this call.
 */
void shmem_comp_free(struct shmem_comp_pool *pool, unsigned long handle)
{
	if (!pool || handle == 0)
		return;

	zs_free(pool->zs_pool, handle);
}

/*
 * Compression entry ID table.
 *
 * zsmalloc handles are kernel pointers and cannot be packed into the
 * xarray value stored in the page cache. Compressed data is registered
 * here and referenced by a small ID from the page cache entry.
 */

/* XA_FLAGS_ALLOC reserves index 0: the page cache uses 0 for "empty" */
static DEFINE_XARRAY_FLAGS(shmem_comp_entries, XA_FLAGS_ALLOC);

/*
 * shmem_comp_pool_unit_ratio - logical sectors per unit
 * Returns 1 for NULL pools and for every pool with the split disabled,
 * so callers can divide by it unconditionally.
 */
unsigned int shmem_comp_pool_unit_ratio(struct shmem_comp_pool *pool)
{
	if (!pool || !pool->unit_ratio)
		return 1;
	return pool->unit_ratio;
}

/*
 * Validity-bitmap accessors for split pools (unit_ratio > 1).
 *
 * The bitmap is per-entry, uncompressed, and lives outside the
 * compressed blob so discard can clear a bit without recompressing
 * (see shmem_comp_entry.valid).  These are the ONLY way the block
 * device touches it: the entry lives in this file's private xarray,
 * so cellzram cannot reach valid[] directly.
 *
 * At unit_ratio == 1 no bitmap words are allocated and existence of
 * the entry IS the bitmap -- every accessor reports the entry present,
 * which keeps the disabled-split path byte-identical.
 */

/* is logical sector @idx live in unit @id?  false for a dead or
 * absent entry, so a caller can use it directly as a read mask. */
bool shmem_comp_entry_sector_valid(unsigned int id, unsigned int idx)
{
	struct shmem_comp_entry *ent;
	unsigned int ratio;
	bool live = false;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent)
		goto out;
	ratio = shmem_comp_pool_unit_ratio(ent->pool);
	if (ratio == 1) {
		live = true;	/* presence is the bitmap */
	} else if (idx < ratio &&
		   atomic_inc_not_zero(&ent->refs)) {
		live = test_bit(idx, ent->valid);
		shmem_comp_entry_put(id);
	}
out:
	rcu_read_unlock();
	return live;
}

/* is ANY logical sector in unit @id still live?  A unit whose mask
 * reaches zero is garbage: free it rather than hold an empty blob. */
bool shmem_comp_entry_any_valid(unsigned int id)
{
	struct shmem_comp_entry *ent;
	unsigned int ratio;
	bool live = false;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent)
		goto out;
	ratio = shmem_comp_pool_unit_ratio(ent->pool);
	if (ratio == 1) {
		live = true;
	} else if (atomic_inc_not_zero(&ent->refs)) {
		live = !bitmap_empty(ent->valid, ratio);
		shmem_comp_entry_put(id);
	}
out:
	rcu_read_unlock();
	return live;
}

/* mark logical sector @idx dead in unit @id.  A bit-flip, no I/O and
 * no recompress: the stale bytes stay in the blob until the unit is
 * rewritten or freed.  Returns true if the unit is now entirely dead,
 * so the caller can drop it. */
bool shmem_comp_entry_sector_clear(unsigned int id, unsigned int idx)
{
	struct shmem_comp_entry *ent;
	unsigned int ratio;
	bool now_dead = false;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent)
		goto out;
	ratio = shmem_comp_pool_unit_ratio(ent->pool);
	if (ratio > 1 && idx < ratio &&
	    atomic_inc_not_zero(&ent->refs)) {
		clear_bit(idx, ent->valid);
		now_dead = bitmap_empty(ent->valid, ratio);
		shmem_comp_entry_put(id);
	}
out:
	rcu_read_unlock();
	return now_dead;
}

/* copy the validity mask from unit @src_id to @dst_id.  A split write
 * replaces the entry (COW) but must preserve which sibling sectors are
 * still live, so the new entry inherits the old mask before the written
 * sector is set. */
void shmem_comp_entry_valid_copy(unsigned int dst_id, unsigned int src_id)
{
	struct shmem_comp_entry *dst, *src;
	unsigned int ratio;

	rcu_read_lock();
	src = xa_load(&shmem_comp_entries, src_id);
	dst = xa_load(&shmem_comp_entries, dst_id);
	if (!src || !dst)
		goto out;
	ratio = shmem_comp_pool_unit_ratio(src->pool);
	if (ratio > 1 && ratio == shmem_comp_pool_unit_ratio(dst->pool) &&
	    atomic_inc_not_zero(&src->refs) &&
	    atomic_inc_not_zero(&dst->refs)) {
		bitmap_copy(dst->valid, src->valid, ratio);
		shmem_comp_entry_put(src_id);
		shmem_comp_entry_put(dst_id);
	}
out:
	rcu_read_unlock();
}

/* mark logical sector @idx live in unit @id.  Inverse of sector_clear;
 * used after a split write installs the new entry. */
void shmem_comp_entry_sector_set(unsigned int id, unsigned int idx)
{
	struct shmem_comp_entry *ent;
	unsigned int ratio;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent)
		goto out;
	ratio = shmem_comp_pool_unit_ratio(ent->pool);
	if (ratio > 1 && idx < ratio && atomic_inc_not_zero(&ent->refs)) {
		set_bit(idx, ent->valid);
		shmem_comp_entry_put(id);
	}
out:
	rcu_read_unlock();
}


/*
 * Generation counter for entry incarnations (swapops.h packs 23 bits
 * of it into every page cache xarray value, next to the ID) and the
 * count of stale xarray values those generations rejected. A stale
 * value is normal under truncate + rewrite churn -- the caller retries
 * -- but a counter climbing without bound points at an ID-recycling
 * storm worth investigating.
 */
static atomic64_t shmem_comp_gen_counter = ATOMIC64_INIT(0);
static atomic64_t shmem_comp_stale_retries;

/*
 * Entry lookup and reference discipline.
 *
 * Every page cache slot holding the ID owns one entry reference
 * (slots, initialised to nr_pages at alloc). Readers that need the
 * entry-owned data outside RCU take an additional pin with get(); the
 * entry -- and its zsmalloc block and merged buffer -- is freed when
 * the total reference count (slots + pins) drops to zero, whichever
 * kind of reference drops it. release() additionally tracks slots so
 * shmem_free_swap() can fire its once-per-group accounting on the
 * last slot even while a reader pin keeps the entry alive.
 */

/*
 * Teardown for the last reference of any kind. Runs outside RCU; the
 * entry cannot be resurrected (get() fails on refs == 0) and no other
 * thread can reach this path (atomic dec_and_test, xa_erase guard).
 * The pool outlives every entry: inode eviction drains all slots
 * before put_super() destroys the pool, and reader pins cannot
 * outlive their inode.
 */
static void shmem_comp_entry_free(unsigned int id)
{
	struct shmem_comp_entry *ent;

	/* xa_erase() takes the xarray lock internally */
	ent = xa_erase(&shmem_comp_entries, id);
	if (!ent)
		return;

	/* Teardown runs only from the refs==0 path (dec_and_test); a
	 * live entry here means double teardown. Debug kernels panic. */
	VM_BUG_ON(atomic_read(&ent->refs) != 0);

	/* Un-account live RAM before freeing (ksize needs a live
	 * object; do it before kfree/kfree_rcu). */
	atomic_dec(&shmem_comp_ram_entries);
	atomic_long_sub(ksize(ent), &shmem_comp_ram_entry_bytes);
	if (ent->handles)
		atomic_long_sub(ksize(ent->handles),
				 &shmem_comp_ram_merged_bytes);

	/*
	 * Release the backing storage.  Cell storage needs the block
	 * length to decrement live_bytes, which is why cell_free() is
	 * called directly here rather than through shmem_comp_free().	 * The zsmalloc free path does not need the length — zs_free()	 * recovers it from the size class encoded in the handle.	 */
	if (ent->handle) {
		if (shmem_comp_is_staged(ent->handle)) {
			/* staged buffer, not a cell region: free the slot */
			shmem_comp_open_free_slot(ent->pool,
						   shmem_comp_open_idx(ent->handle));
		} else if (ent->pool->use_cells) {
			u32 cellid;
			u32 offset;

			shmem_comp_unpack_loc(ent->handle,
					      &cellid, &offset);
			cell_free(ent->pool->cells, cellid, offset,
				  ent->length);
		} else {
			shmem_comp_free(ent->pool, ent->handle);
		}
	}
	if (ent->handles)
		shmem_comp_free_merged(ent->pool, ent->handles,
				       ent->nchunks);
	kfree_rcu(ent, rcu);
}

static bool shmem_comp_entry_get_common(unsigned int id, bool check_gen,
					unsigned int gen,
					unsigned long *handle,
					unsigned int *length,
					unsigned int *nr_pages,
					unsigned long **handles,
					unsigned short *nchunks)
{
	struct shmem_comp_entry *ent;
	bool found = false;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (ent && check_gen && READ_ONCE(ent->gen) != gen) {
		/* Stale xarray value: this ID names a different
		 * incarnation. Reject without touching the new entry. */
		atomic64_inc(&shmem_comp_stale_retries);
	} else if (ent && atomic_inc_not_zero(&ent->refs)) {
		/* inc_unless_zero: fail on a dying entry instead of
		 * pinning memory that teardown is about to free. */
		if (handle)
			*handle = ent->handle;
		if (length)
			*length = ent->length;
		if (nr_pages)
			*nr_pages = ent->nr_pages;
		if (handles)
			*handles = ent->handles;
		if (nchunks)
			*nchunks = ent->nchunks;
		found = true;
	}
	rcu_read_unlock();

	return found;
}

bool shmem_comp_entry_get(unsigned int id, unsigned long *handle,
			  unsigned int *length, unsigned int *nr_pages,
			  unsigned long **handles,
			  unsigned short *nchunks)
{
	/* Bare-ID pin: table-internal users only (cellzram, whose
	 * entries are pinned by their own table references). Anything
	 * that reached the ID through a page cache xarray value must
	 * use shmem_comp_entry_get_gen(). */
	return shmem_comp_entry_get_common(id, false, 0, handle, length,
					   nr_pages, handles, nchunks);
}

bool shmem_comp_entry_get_gen(unsigned int id, unsigned int gen,
			      unsigned long *handle,
			      unsigned int *length,
			      unsigned int *nr_pages,
			      unsigned long **handles,
			      unsigned short *nchunks)
{
	return shmem_comp_entry_get_common(id, true, gen, handle, length,
					   nr_pages, handles, nchunks);
}

void shmem_comp_entry_put(unsigned int id)
{
	struct shmem_comp_entry *ent;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent) {
		rcu_read_unlock();
		return;
	}
	if (!atomic_dec_and_test(&ent->refs)) {
		if (unlikely(atomic_read(&ent->refs) < 0)) {
			rcu_read_unlock();
			panic("shmem_comp: entry %u refs underflow -- "
			      "reference accounting corrupted", id);
		}
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

	shmem_comp_entry_free(id);
}

bool shmem_comp_entry_release(unsigned int id, unsigned int gen)
{
	struct shmem_comp_entry *ent;
	bool last_slot;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent) {
		rcu_read_unlock();
		return false;
	}
	if (READ_ONCE(ent->gen) != gen) {
		/* Stale xarray value: the entry this slot referenced died
		 * and the ID was recycled. The old entry's accounting ran
		 * when IT died; do not charge the new incarnation. */
		atomic64_inc(&shmem_comp_stale_retries);
		rcu_read_unlock();
		return false;
	}
	last_slot = atomic_dec_and_test(&ent->slots);
	if (unlikely(atomic_read(&ent->slots) < 0)) {
		rcu_read_unlock();
		panic("shmem_comp: entry %u slots underflow -- "
		      "page cache slot released twice", id);
	}
	if (!atomic_dec_and_test(&ent->refs)) {
		if (unlikely(atomic_read(&ent->refs) < 0)) {
			rcu_read_unlock();
			panic("shmem_comp: entry %u refs underflow -- "
			      "reference accounting corrupted", id);
		}
		rcu_read_unlock();
		return last_slot;
	}
	rcu_read_unlock();

	shmem_comp_entry_free(id);
	return last_slot;
}

unsigned int shmem_comp_entry_alloc(struct shmem_comp_pool *pool,
				    unsigned long handle, unsigned int length,
				    unsigned short nr_pages,
				    unsigned long *handles,
				    unsigned short nchunks,
				    unsigned int *gen)
{
	struct shmem_comp_entry *ent;
	unsigned int id;
	unsigned int g;
	unsigned int ratio = pool->unit_ratio ? pool->unit_ratio : 1;
	unsigned int vbm_words;
	int ret;

	/*
	 * valid[] gets BITS_TO_LONGS(ratio) words when the split is on
	 * and ZERO when it is off, so a ratio-1 entry is the same size
	 * it was before the field existed.  A fresh unit is entirely
	 * live, so all bits start set -- the write path clears bits on
	 * discard from there.
	 */
	vbm_words = ratio > 1 ? BITS_TO_LONGS(ratio) : 0;
	ent = kmalloc(struct_size(ent, valid, vbm_words), GFP_KERNEL);
	if (!ent)
		return 0;
	if (vbm_words)
		bitmap_fill(ent->valid, ratio);

	ent->handle = handle;
	ent->length = length;
	ent->nr_pages = nr_pages;
	g = (unsigned int)(atomic64_inc_return(&shmem_comp_gen_counter) &
			    SHMEM_COMP_ENTRY_GEN_MASK);
	ent->gen = g;
	ent->handles = handles;
	ent->nchunks = nchunks;
	ent->pool = pool;
	atomic_set(&ent->refs, nr_pages);
	atomic_set(&ent->slots, nr_pages);

	/*
	 * xa_alloc() takes the xarray lock internally. Wrapping it in
	 * xa_lock() deadlocks against itself (non-recursive spinlock,
	 * observed as a soft lockup in QEMU).
	 *
	 * XA_FLAGS_ALLOC does not reserve index 0 in this tree, and the
	 * xarray limit below excludes it explicitly: ID 0 is the failure
	 * sentinel for both callers (a 0 return means "allocation
	 * failed"), so id is never 0 here.
	 */
	ret = xa_alloc(&shmem_comp_entries, &id, ent,
			XA_LIMIT(1, UINT_MAX), GFP_KERNEL | __GFP_NOWARN);
	if (ret) {
		kfree(ent);
		return 0;
	}

	/* Live RAM footprint: slab-rounded entry + handle array size */
	atomic_inc(&shmem_comp_ram_entries);
	atomic_long_add(ksize(ent), &shmem_comp_ram_entry_bytes);
	if (handles)
		atomic_long_add(ksize(handles),
				 &shmem_comp_ram_merged_bytes);

	if (gen)
		*gen = g;
	return id;
}

/*
 * shmem_comp_cell_gc - evacuate sparse cells into the active cell
 * @pool: cell-backed compression pool
 *
 * The async half of the cell allocator.  cell_free() only links
 * sparse cells; this pass does the actual relocation.  It runs from
 * the shrinker (memory pressure), from the gc_now debug hook, and
 * from the store path when a cell-backed pool runs out of directory
 * slots -- never from the free path itself.
 *
 * Protocol, per victim:
 *   pick_victim()    harvests a sparse cell and takes down_write on
 *                    it; no reader can be inside cell_load() for it,
 *                    and cell_free()'s empty-return path blocks on
 *                    the same lock.
 *   scan             pins each entry residing in that victim
 *                    (inc_not_zero -- dying entries are skipped, their
 *                    own free path handles the accounting), copies
 *                    the block, relocates it, updates the entry
 *                    handle with a WRITE_ONCE.
 *   release_victim() rechecks under cp->lock -- empty -> buddy, still
 *                    sparse -> re-link for a later pass.
 *
 * Draining is SEQUENTIAL and goes to completion per victim; see the
 * rationale below for why a batched scan cannot work here.
 *
 * Lock order: victim rwsem → cp->lock, matching cell_free()
 * (down_read → cp->lock).  The reader's retry loop in
 * shmem_comp_load_entry() closes the window where a stale handle
 * outlives the relocation.
 */
/*
 * shmem_comp_drain_victim - relocate every block out of one victim
 * @pool: cell-backed compression pool
 * @vic: victim cell, held for write by the caller
 *
 * One entry-table scan.  Returns the number of blocks relocated; zero
 * means nothing could move (all pinned, or too large for a cell), which
 * callers use as a stop signal so a re-linked victim is not re-harvested
 * forever.
 */
static unsigned int shmem_comp_drain_victim(struct shmem_comp_pool *pool,
					    struct cell *vic)
{
	struct cell_pool *cp = pool->cells;
	unsigned int moved = 0;
	unsigned long id;
	struct shmem_comp_entry *ent;

	xa_for_each(&shmem_comp_entries, id, ent) {
		unsigned long h;
		u32 cellid;
		u32 offset;
		unsigned int len;
		u32 nc;
		u32 no;
		int ret;

		if (ent->pool != pool)
			continue;

		h = READ_ONCE(ent->handle);
		if (!h)
			continue;	/* merged zsmalloc entry */
		shmem_comp_unpack_loc(h, &cellid, &offset);

		if (cellid != vic->cellid)
			continue;

		/* Pin: skip entries whose last reference is dropping */
		if (!atomic_inc_not_zero(&ent->refs))
			continue;

		len = ent->length;
		if (offset + len > (1UL << cp->cell_order) * PAGE_SIZE) {
			shmem_comp_entry_put(id);
			continue;
		}

		ret = cell_relocate_one(cp, vic, vic->base + offset, len,
					&nc, &no);
		if (ret == 0) {
			WRITE_ONCE(ent->handle, shmem_comp_pack_loc(nc, no));
			moved++;
		}
		shmem_comp_entry_put(id);
	}
	return moved;
}

/*
 * shmem_comp_drain_staged - compress staged open buffers into cell regions
 * @pool: cell-backed pool with staging enabled
 *
 * The async half of the open-cell buffer: a staged unit holds its
 * uncompressed bytes in an open-buffer slot; this compresses it into a
 * cell region and flips the entry handle from staged (bit 63 set) to a
 * cell location.  A reader that raced the flip sees the changed handle
 * under the slot's down_read and retries into the closed path -- the same
 * contract shmem_comp_load_entry() relies on for cells.  Runs from the
 * cell GC (shrinker, gc_now, store-exhaustion), all of which hold
 * gc_lock, so two drains cannot overlap.  An incompressible unit is stored
 * raw at unit_bytes; unit_load() detects that (length == unit_bytes) and
 * extracts the block without decompressing.
 */
static void shmem_comp_drain_staged(struct shmem_comp_pool *pool)
{
	unsigned long id;
	struct shmem_comp_entry *ent;

	if (!pool->opens)
		return;

	xa_for_each(&shmem_comp_entries, id, ent) {
		unsigned long h, new_handle;
		unsigned int ratio, comp_len, idx, unit_bytes;
		struct shmem_comp_open *op;
		void *dbuf, *cbuf;
		int ret;

		if (ent->pool != pool)
			continue;
		h = READ_ONCE(ent->handle);
		if (!h || !shmem_comp_is_staged(h))
			continue;
		if (!atomic_inc_not_zero(&ent->refs))
			continue;	/* dying; its own free path handles it */
		if (READ_ONCE(ent->handle) != h) {
			shmem_comp_entry_put(id);
			continue;	/* changed under the pin */
		}

		ratio = pool->unit_ratio;
		unit_bytes = (unsigned int)ratio * PAGE_SIZE;
		idx = shmem_comp_open_idx(h);
		op = &pool->opens[idx];

		down_write(&op->lock);
		if (READ_ONCE(ent->handle) != h || !op->in_use || !op->page) {
			up_write(&op->lock);
			shmem_comp_entry_put(id);
			continue;	/* already drained / reused */
		}
		dbuf = page_address(op->page);
		cbuf = kmalloc(unit_bytes, GFP_KERNEL);
		if (!cbuf) {
			up_write(&op->lock);
			shmem_comp_entry_put(id);
			continue;	/* stay staged; try a later pass */
		}
		comp_len = unit_bytes;
		ret = shmem_comp_compress(pool, dbuf, unit_bytes, cbuf,
					  &comp_len);
		if (ret)
			comp_len = unit_bytes;	/* incompressible: store raw */
		new_handle = shmem_comp_store(pool, ret ? dbuf : cbuf,
					      comp_len);
		kfree(cbuf);
		if (!new_handle) {
			up_write(&op->lock);
			shmem_comp_entry_put(id);
			continue;	/* no cell space; stay staged */
		}
		/* flip under the same down_write the readers exclude on */
		WRITE_ONCE(ent->length, comp_len);
		WRITE_ONCE(ent->handle, new_handle);
		if (op->page) {
			__free_pages(op->page, op->order);
			op->page = NULL;
		}
		up_write(&op->lock);

		spin_lock(&pool->open_lock);
		if (op->in_use) {
			op->in_use = false;
			list_del(&op->lru);
			pool->open_count--;
		}
		spin_unlock(&pool->open_lock);
		shmem_comp_entry_put(id);
	}
}

static void shmem_comp_cell_gc(struct shmem_comp_pool *pool)
{
	struct cell_pool *cp = pool->cells;
	unsigned int drained = 0, moved_total = 0;
	unsigned int i;

	/* Compress staged open buffers into cell regions first (the async
	 * close of the open-cell buffer), then drain sparse cells. */
	shmem_comp_drain_staged(pool);

	/*
	 * Drain victims ONE AT A TIME, each all the way to empty.
	 *
	 * The earlier shape harvested up to GC_BATCH victims first and
	 * then did a single entry-table scan for all of them.  That
	 * cannot make progress once the directory is full: a victim
	 * holds at most one cell of live data, so the ONE new cell the
	 * compaction slot funds gets split across the whole batch, no
	 * victim reaches zero live_bytes, and only an EMPTY victim
	 * returns a slot to the buddy.  Measured on the churn gate: the
	 * batched pass moved exactly one cell's worth of blocks (64
	 * objects) and freed nothing, on every pass -- a fixed point.
	 *
	 * Draining a single victim completely is what frees the slot
	 * that funds the next drain, so the pass converges instead of
	 * stalling.  The cost is one table scan per victim rather than
	 * one per batch, bounded by GC_MAX_VICTIMS and only paid when
	 * compaction is actually needed (memory pressure, or a store
	 * that ran out of directory slots).
	 */
	for (i = 0; i < GC_MAX_VICTIMS; i++) {
		struct cell *vic;
		unsigned int moved;

		vic = cell_pool_pick_victim(cp);
		if (!vic)
			break;

		moved = shmem_comp_drain_victim(pool, vic);

		/* Emptied victim goes back to the buddy, freeing the
		 * directory slot the next drain needs. */
		cell_pool_release_victim(cp, vic);
		drained++;
		moved_total += moved;

		/*
		 * Nothing left this victim: its remaining objects are
		 * pinned or too large to relocate.  Stop, or the next
		 * pick returns the same re-linked cell and we spin.
		 * The shrinker re-invokes if more sparse cells remain.
		 */
		if (!moved)
			break;
	}

	/*
	 * LEAVE THE COMPACTION SLOT FREE.  The pass bootstrapped from
	 * the extra slot beyond max_cells, so it may still hold the last
	 * victim's data.  If that is left in place the slot is occupied
	 * next time, and a later pass that meets a full directory with a
	 * full active cell has no destination at all -- it relocates
	 * nothing and frees nothing, which is exactly the stall this
	 * mechanism exists to prevent (measured: the second pass moved 0
	 * objects on every subsequent store).  The slots this pass just
	 * freed are the destination for that last drain.
	 */
	if (cp->cells[cp->max_cells].pages[0]) {
		struct cell *scratch = &cp->cells[cp->max_cells];

		down_write(&scratch->lock);
		moved_total += shmem_comp_drain_victim(pool, scratch);
		cell_pool_release_victim(cp, scratch);
	}

	cell_dbg("gc pass: %u victims drained, %u objects moved",
		 drained, moved_total);

}

/*
 * Shrinker: one GC pass per pool per invocation.  count_objects
 * reports sparse cells as reclaimable objects; scan reclaims them.
 */
static unsigned long shmem_comp_cell_count(struct shrinker *shrinker,
					   struct shrink_control *sc)
{
	struct shmem_comp_pool *pool = shrinker->private_data;
	struct cell_pool *cp = pool->cells;
	unsigned long n = 0;
	struct cell *c;

	spin_lock(&cp->lock);
	list_for_each_entry(c, &cp->sparse, node)
		n++;
	spin_unlock(&cp->lock);
	return n;
}

static unsigned long shmem_comp_cell_scan(struct shrinker *shrinker,
					  struct shrink_control *sc)
{
	struct shmem_comp_pool *pool = shrinker->private_data;
	struct cell_pool *cp = pool->cells;
	unsigned long before, after, freed_cells = 0;

	if (!(sc->gfp_mask & __GFP_DIRECT_RECLAIM))
		return SHRINK_STOP;

	/*
	 * Stage 2: reclaim pressure triggers aging asynchronously --
	 * scheduling is atomic-safe and coalescing, the actual passes
	 * run later in kworker context (see shmem_comp_pressure_pass).
	 */
	schedule_work(&shmem_comp_pressure_work);

	/* One pass per scan invocation; the shrinker re-invokes if
	 * more sparse cells remain.  "Objects" are sparse cells. */
	mutex_lock(&cp->gc_lock);
	before = shmem_comp_cell_count(shrinker, sc);
	shmem_comp_cell_gc(pool);
	after = shmem_comp_cell_count(shrinker, sc);
	mutex_unlock(&cp->gc_lock);

	freed_cells = before > after ? before - after : 0;
	return freed_cells;
}


/*
 * shmem_comp_load_entry - pinned, relocation-safe block load
 * @id: entry ID (caller need not pin)
 * @dst: destination buffer
 * @len: [in,out] buffer size in, data size out
 *
 * Return: 0 on success, -ENOENT if the entry is gone, -EIO if the
 * cell vanished without the handle changing (unreachable unless the
 * entry table and cell accounting disagree).
 *
 * The reader's side of the GC contract.  A plain load could memcpy
 * from a cell that GC relocated and returned to the buddy between
 * the caller's entry_get() and the copy.  This helper re-reads the
 * entry handle under the cell's read lock and retries if GC moved
 * the block — the same -EEXIST-retry shape the page-cache install
 * path uses.  The entry pin taken here keeps the entry (and its
 * accounting) alive across the retry.
 */
int shmem_comp_load_entry(unsigned int id, void *dst, unsigned int *len)
{
	struct shmem_comp_entry *ent;
	struct cell_pool *cp;
	unsigned int budget = 8;
	int ret = -EIO;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent || !atomic_inc_not_zero(&ent->refs)) {
		rcu_read_unlock();
		return -ENOENT;
	}
	rcu_read_unlock();

	cp = ent->pool->cells;
	if (!cp)
		goto out;

	while (budget--) {
		unsigned long h = READ_ONCE(ent->handle);
		u32 cellid;
		u32 offset;
		struct cell *c;
		u32 cell_size;

		if (!h)
			break;
		shmem_comp_unpack_loc(h, &cellid, &offset);
		c = &cp->cells[cellid];

		down_read(&c->lock);
		/*
		 * Re-read under the lock: GC updates the handle while
		 * holding down_write on this very rwsem, so a change
		 * here means the block moved — retry with the fresh
		 * location.  Slot reuse cannot fool this check: the
		 * handle would differ, and an unchanged handle implies
		 * the original cell is still live (its last object is
		 * this one, pinned by us).
		 */
		if (READ_ONCE(ent->handle) != h) {
			up_read(&c->lock);
			continue;
		}
		if (!c->pages[0] || !c->base) {
			up_read(&c->lock);
			continue;
		}

		cell_size = (1UL << cp->cell_order) * PAGE_SIZE;
		if (offset + *len > cell_size)
			*len = cell_size - offset;
		memcpy(dst, c->base + offset, *len);
		up_read(&c->lock);
		ret = 0;
		break;
	}

out:
	shmem_comp_entry_put(id);
	return ret;
}

/* Debug hook: run one GC pass on every cell-backed pool. */
/*
 * shmem_comp_open_free_slot - release one staged open-buffer slot
 * @pool: pool owning the slot
 * @idx: slot index (from a staged handle's low bits)
 *
 * Frees the buffer page and returns the slot to the free pool.  The
 * down_write mirrors a cell's: it excludes a staged reader mid-memcpy.
 * Called from entry teardown (refs == 0, so no reader is pinned) today;
 * the staged drain of a later step reuses it under the same rwsem
 * contract while the entry lives.
 */
static void shmem_comp_open_free_slot(struct shmem_comp_pool *pool,
				      unsigned int idx)
{
	struct shmem_comp_open *op;

	if (!pool->opens || idx >= pool->open_max)
		return;
	op = &pool->opens[idx];

	down_write(&op->lock);
	if (op->page) {
		__free_pages(op->page, op->order);
		op->page = NULL;
	}
	up_write(&op->lock);

	spin_lock(&pool->open_lock);
	if (op->in_use) {
		op->in_use = false;
		list_del(&op->lru);
		pool->open_count--;
	}
	spin_unlock(&pool->open_lock);
}

/*
 * shmem_comp_stage_open - create a staged (uncompressed) unit entry
 * @pool: cell-backed pool with staging enabled (open_max > 0, ratio > 1)
 * @seed_id: existing closed entry to seed from, or 0 for a fresh unit
 *
 * Return: the new entry id, or 0 when no slot is free (the caller stores
 * the unit compressed instead) or on allocation failure.
 *
 * The returned entry carries a staged handle (bit 63 set, low bits the
 * slot index), length 0 (not yet compressed), and one table reference
 * (nr_pages = 1: cellzram's block table holds a single slot per unit,
 * regardless of how many logical blocks the unit spans).  Its validity
 * bitmap is @seed_id's, or empty for a fresh unit -- the caller sets the
 * written block's bit.  The buffer is the ONLY copy of staged data until
 * it is compressed, so the entry's lifetime owns the slot (freed in
 * shmem_comp_entry_free).
 */
unsigned int shmem_comp_stage_open(struct shmem_comp_pool *pool,
				   unsigned int seed_id)
{
	unsigned int ratio = pool->unit_ratio;
	unsigned int order, idx, id, i;
	struct shmem_comp_open *op;
	struct shmem_comp_entry *ent;
	struct page *page;
	unsigned long h;

	if (!pool->opens || ratio < 2)
		return 0;

	order = get_order((size_t)ratio * PAGE_SIZE);
	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page)
		return 0;

	/* claim a free slot (open_max is small; a linear scan is fine) */
	spin_lock(&pool->open_lock);
	idx = pool->open_max;
	if (pool->open_count < pool->open_max) {
		for (i = 0; i < pool->open_max; i++)
			if (!pool->opens[i].in_use) {
				idx = i;
				break;
			}
	}
	if (idx == pool->open_max) {
		spin_unlock(&pool->open_lock);
		__free_pages(page, order);
		return 0;	/* pool full: caller stores compressed */
	}
	op = &pool->opens[idx];
	op->in_use = true;
	op->page = page;
	op->order = order;
	list_add(&op->lru, &pool->open_lru);	/* MRU at head */
	pool->open_count++;
	spin_unlock(&pool->open_lock);

	/* seed: decompress the seed blob into the buffer (else stays zero).
	 * load_entry copies *len bytes -- the STORED length, not a capacity --
	 * so fetch the seed's length first; passing unit_bytes here would copy
	 * adjacent cell garbage past the compressed blob and fail to inflate. */
	if (seed_id) {
		unsigned int comp_len = 0;
		unsigned int out = (unsigned int)ratio * PAGE_SIZE;
		void *cbuf;
		int ret;

		if (!shmem_comp_entry_get(seed_id, NULL, &comp_len, NULL,
					   NULL, NULL))
			goto fail;	/* seed vanished */
		cbuf = kmalloc(comp_len, GFP_KERNEL);
		if (!cbuf) {
			shmem_comp_entry_put(seed_id);
			goto fail;
		}
		ret = shmem_comp_load_entry(seed_id, cbuf, &comp_len);
		shmem_comp_entry_put(seed_id);
		if (ret) {
			kfree(cbuf);
			goto fail;
		}
		ret = shmem_comp_decompress(pool, cbuf, comp_len,
					    page_address(page), &out);
		kfree(cbuf);
		if (ret)
			goto fail;
	}

	/* create the staged entry: slot handle, length 0, one table ref */
	h = shmem_comp_pack_open(idx);
	id = shmem_comp_entry_alloc(pool, h, 0, 1, NULL, 0, NULL);
	if (!id)
		goto fail;

	/* set the validity bitmap: copy the seed's, else empty */
	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (ent) {
		if (seed_id) {
			struct shmem_comp_entry *src =
				xa_load(&shmem_comp_entries, seed_id);

			if (src)
				bitmap_copy(ent->valid, src->valid, ratio);
			else
				bitmap_zero(ent->valid, ratio);
		} else {
			bitmap_zero(ent->valid, ratio);
		}
	}
	rcu_read_unlock();
	if (!ent) {
		/* unreachable (we hold the only reference); be safe */
		shmem_comp_entry_put(id);
		goto fail;
	}
	return id;

fail:
	shmem_comp_open_free_slot(pool, idx);
	return 0;
}

/*
 * shmem_comp_unit_load - copy logical block @blk of a unit into @dst
 * @id: entry id (cellzram block table); the caller need not pin it
 * @blk: in-unit logical block index (0..ratio-1)
 * @dst: destination, @len bytes
 * @len: bytes to copy (<= PAGE_SIZE)
 *
 * Return: 0 on success, -ENOENT if the entry vanished, -EIO on a
 * corrupt/short decompress.
 *
 * The whole ratio>1 read, staged or closed, kept here so the staged-
 * vs-drain protocol lives next to the entry and slot it guards.  A block
 * whose validity bit is clear (discarded, or never written) reads as
 * zeros.  A staged block is memcpy'd under the slot's down_read, with the
 * handle re-checked under the lock (retry if a drain moved it); a closed
 * block is decompressed from its cell region, exactly as load_entry does.
 */
int shmem_comp_unit_load(unsigned int id, unsigned int blk, void *dst,
			 unsigned int len)
{
	struct shmem_comp_entry *ent;
	struct shmem_comp_pool *pool;
	unsigned int ratio;
	unsigned int budget = 8;
	int ret = -EIO;
	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent || !atomic_inc_not_zero(&ent->refs)) {
		rcu_read_unlock();
		return -ENOENT;
	}
	rcu_read_unlock();

	pool = ent->pool;
	ratio = pool->unit_ratio ? pool->unit_ratio : 1;
	if (blk >= ratio) {
		shmem_comp_entry_put(id);
		return -EINVAL;
	}

	/* validity: a discarded / never-written block reads zeros */
	if (!shmem_comp_entry_sector_valid(id, blk)) {
		memset(dst, 0, len);
		shmem_comp_entry_put(id);
		return 0;
	}

	while (budget--) {
		unsigned long h = READ_ONCE(ent->handle);

		if (!h) {
			ret = -EIO;
			break;
		}
		if (shmem_comp_is_staged(h)) {
			unsigned int idx = shmem_comp_open_idx(h);
			struct shmem_comp_open *op = &pool->opens[idx];

			down_read(&op->lock);
			if (READ_ONCE(ent->handle) != h ||
			    !op->in_use || !op->page) {
				up_read(&op->lock);
				continue;	/* drained; retry */
			}
			memcpy(dst, page_address(op->page) +
			       (size_t)blk * PAGE_SIZE, len);
			up_read(&op->lock);
			shmem_comp_entry_put(id);
			return 0;
		} else {
			unsigned int comp_len = ent->length;
			unsigned int out = (unsigned int)ratio * PAGE_SIZE;
			void *cbuf, *dbuf;
			int r;

			cbuf = kmalloc(comp_len, GFP_KERNEL);
			if (!cbuf) {
				ret = -ENOMEM;
				break;
			}
			r = shmem_comp_load_entry(id, cbuf, &comp_len);
			if (r) {
				kfree(cbuf);
				ret = r;
				break;
			}
			if (comp_len == (unsigned int)ratio * PAGE_SIZE) {
				/* raw unit (incompressible): the blob IS the
				 * uncompressed unit; extract the block directly */
				memcpy(dst, cbuf + (size_t)blk * PAGE_SIZE, len);
				kfree(cbuf);
				shmem_comp_entry_put(id);
				return 0;
			}
			dbuf = kmalloc((size_t)ratio * PAGE_SIZE, GFP_KERNEL);
			if (!dbuf) {
				kfree(cbuf);
				ret = -ENOMEM;
				break;
			}
			r = shmem_comp_decompress(pool, cbuf, comp_len,
						  dbuf, &out);
			kfree(cbuf);
			if (r || out != (unsigned int)ratio * PAGE_SIZE) {
				kfree(dbuf);
				ret = r ? r : -EIO;
				break;
			}
			memcpy(dst, dbuf + (size_t)blk * PAGE_SIZE, len);
			kfree(dbuf);
			shmem_comp_entry_put(id);
			return 0;
		}
	}
	shmem_comp_entry_put(id);
	return ret;
}

/*
 * shmem_comp_stage_write - write @len bytes into a staged unit's block
 * @id: staged entry id (cellzram block table); caller need not pin
 * @blk: in-unit logical block index (0..ratio-1)
 * @src / @len: the logical block data (<= PAGE_SIZE)
 *
 * Return: 0 on success, -ENOENT if the entry vanished, -EAGAIN if the
 * unit is no longer staged (a drain compressed it; the caller re-stages).
 *
 * Writes into the buffer under the slot's down_read -- safe alongside a
 * reader of a DIFFERENT block (disjoint bytes) and a writer of another
 * block; same-block ordering is the filesystem's job.  Marks the block
 * valid and moves the slot to the LRU head.  The down_write that would
 * exclude this is only ever taken by the drain, which first flips the
 * handle so a racing writer retries.
 */
int shmem_comp_stage_write(unsigned int id, unsigned int blk,
			   const void *src, unsigned int len)
{
	struct shmem_comp_entry *ent;
	struct shmem_comp_pool *pool;
	unsigned int ratio, idx, budget = 8;
	int ret = -EAGAIN;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (!ent || !atomic_inc_not_zero(&ent->refs)) {
		rcu_read_unlock();
		return -ENOENT;
	}
	rcu_read_unlock();

	pool = ent->pool;
	ratio = pool->unit_ratio;
	if (blk >= ratio) {
		shmem_comp_entry_put(id);
		return -EINVAL;
	}

	while (budget--) {
		unsigned long h = READ_ONCE(ent->handle);
		struct shmem_comp_open *op;

		if (!h || !shmem_comp_is_staged(h)) {
			ret = -EAGAIN;	/* not staged; caller re-stages */
			break;
		}
		idx = shmem_comp_open_idx(h);
		op = &pool->opens[idx];
		down_read(&op->lock);
		if (READ_ONCE(ent->handle) != h ||
		    !op->in_use || !op->page) {
			up_read(&op->lock);
			continue;	/* drained; retry */
		}
		memcpy(page_address(op->page) + (size_t)blk * PAGE_SIZE,
		       src, len);
		shmem_comp_entry_sector_set(id, blk);
		spin_lock(&pool->open_lock);
		list_move(&op->lru, &pool->open_lru);	/* MRU head */
		spin_unlock(&pool->open_lock);
		up_read(&op->lock);
		shmem_comp_entry_put(id);
		return 0;
	}
	shmem_comp_entry_put(id);
	return ret;
}

/* Number of live logical blocks in a unit (set validity bits). */
unsigned int shmem_comp_entry_valid_count(unsigned int id)
{
	struct shmem_comp_entry *ent;
	unsigned int ratio, n = 0;

	rcu_read_lock();
	ent = xa_load(&shmem_comp_entries, id);
	if (ent) {
		ratio = ent->pool->unit_ratio;
		if (ratio > 1)
			n = bitmap_weight(ent->valid, ratio);
		else
			n = 1;
	}
	rcu_read_unlock();
	return n;
}

/* Uncompressed staging bytes currently held by a pool's open buffers. */
unsigned long shmem_comp_pool_staged_bytes(struct shmem_comp_pool *pool)
{
	unsigned long n;

	if (!pool || !pool->opens)
		return 0;
	spin_lock(&pool->open_lock);
	n = (unsigned long)pool->open_count * pool->unit_ratio * PAGE_SIZE;
	spin_unlock(&pool->open_lock);
	return n;
}

void shmem_comp_cell_gc_all(void)
{
	struct shmem_comp_pool *pool;

	spin_lock(&shmem_comp_pools_lock);
	list_for_each_entry(pool, &shmem_comp_pools, list) {
		if (!pool->use_cells || !pool->cells)
			continue;
		mutex_lock(&pool->cells->gc_lock);
		shmem_comp_cell_gc(pool);
		mutex_unlock(&pool->cells->gc_lock);
	}
	spin_unlock(&shmem_comp_pools_lock);
}

/*
 * Statistics functions.
 */

/**
 * shmem_comp_get_stats - Get global compression statistics
 * @stats: Output structure for statistics
 *
 * Populates the stats structure with current compression metrics.
 * Calculated compression ratio is based on total bytes stored.
 */
void shmem_comp_get_stats(struct shmem_comp_stats *stats)
{
	struct shmem_comp_pool *pool;
	unsigned long pool_pages = 0;

	if (!stats)
		return;

	spin_lock(&shmem_comp_stats_lock);

	*stats = shmem_comp_global_stats;

	/* Calculate compression ratio (percent * 100 for two decimals) */
	if (stats->total_uncompressed_bytes > 0) {
		stats->ratio = (stats->total_compressed_bytes * 10000) /
			       stats->total_uncompressed_bytes;
	} else {
		stats->ratio = 0;
	}

	stats->ram_entry_bytes = atomic_long_read(&shmem_comp_ram_entry_bytes);
	stats->ram_merged_bytes =
		atomic_long_read(&shmem_comp_ram_merged_bytes);
	stats->ram_entries = atomic_read(&shmem_comp_ram_entries);
	stats->stale_value_retries =
		(unsigned long)atomic64_read(&shmem_comp_stale_retries);

	spin_unlock(&shmem_comp_stats_lock);

	/*
	 * zsmalloc footprint: sum live zspage pages over all pools.
	 * zs_get_total_pages() reports pages currently held by the pool;
	 * empty zspages may be retained for reuse (released by the pool
	 * shrinker under pressure), so this is RAM *held*, not RAM in use.
	 */
	spin_lock(&shmem_comp_pools_lock);
	list_for_each_entry(pool, &shmem_comp_pools, list) {
		if (pool->use_cells)
			pool_pages += atomic_long_read(&pool->cells->pages_used);
		else if (pool->zs_pool)
			pool_pages += zs_get_total_pages(pool->zs_pool);
	}
	spin_unlock(&shmem_comp_pools_lock);

	stats->ram_pool_bytes = pool_pages << PAGE_SHIFT;
	stats->ram_total_bytes = stats->ram_pool_bytes +
		stats->ram_merged_bytes + stats->ram_entry_bytes;
}

/**
 * shmem_comp_cell_stats_show - Fill buffer with cell pool statistics
 * @buf: output buffer
 * @size: buffer size
 *
 * Return: number of bytes written (sysfs convention).
 *
 * Iterates all pools with cell storage and dumps per-cell state:
 * cellid, offset, live_bytes, live_objects, active/sparse flags.
 */
int shmem_comp_cell_stats_show(char *buf, size_t size)
{
	struct shmem_comp_pool *pool;
	size_t off = 0;
	int pool_id = 0;

	spin_lock(&shmem_comp_pools_lock);
	list_for_each_entry(pool, &shmem_comp_pools, list) {
		struct cell_pool *cp = pool->cells;
		u32 i;

		if (!pool->use_cells || !cp)
			continue;

		off += scnprintf(buf + off, size - off,
			"pool %d: order=%u use_thp=%d nr_cells=%u pages_used=%ld\n",
			pool_id, cp->cell_order, cp->use_thp,
			cp->nr_cells, atomic_long_read(&cp->pages_used));

		for (i = 0; i < cp->nr_cells; i++) {
			struct cell *c = &cp->cells[i];

			if (!c->pages[0])
				continue;

			off += scnprintf(buf + off, size - off,
				"  cell %u: offset=%u/%lu live=%u/%u %s%s\n",
				c->cellid, c->next_free,
				(1UL << cp->cell_order) * PAGE_SIZE,
				c->live_objects, c->live_bytes,
				c == cp->active ? "ACTIVE" : "",
				list_empty(&c->node) ? "" : " SPARSE");

			if (off >= size - 1)
				break;
		}

		pool_id++;
	}
	spin_unlock(&shmem_comp_pools_lock);

	return off;
}

/**
 * shmem_comp_update_stats - Update compression statistics
 * @compressed: true if page was compressed, false for decompression
 * @orig_size: Original page size (PAGE_SIZE)
 * @comp_size: Compressed size
 *
 * Updates global statistics counters. Called after successful
 * compression or decompression operations.
 */
void shmem_comp_update_stats(bool compressed, unsigned int orig_size,
			      unsigned int comp_size)
{
	spin_lock(&shmem_comp_stats_lock);

	if (compressed) {
		shmem_comp_global_stats.compressed_pages++;
		shmem_comp_global_stats.uncompressed_pages++;
		shmem_comp_global_stats.total_compressed_bytes += comp_size;
		shmem_comp_global_stats.total_uncompressed_bytes += orig_size;
	} else {
		if (shmem_comp_global_stats.compressed_pages > 0)
			shmem_comp_global_stats.compressed_pages--;
		if (shmem_comp_global_stats.uncompressed_pages > 0)
			shmem_comp_global_stats.uncompressed_pages--;
		if (shmem_comp_global_stats.total_compressed_bytes >= comp_size)
			shmem_comp_global_stats.total_compressed_bytes -= comp_size;
		if (shmem_comp_global_stats.total_uncompressed_bytes >= orig_size)
			shmem_comp_global_stats.total_uncompressed_bytes -= orig_size;
	}

	spin_unlock(&shmem_comp_stats_lock);
}

/*
 * Global compression defaults.
 *
 * Overridable at runtime via the sysfs group "compression" under
 * /sys/fs/tmpfs. Guarded by
 * shmem_comp_defaults_lock: the mount path reads them while mounting,
 * the sysfs handlers replace/free them at any time. Mount options
 * always win over these defaults (see shmem_comp_resolve_algo()).
 */
static DEFINE_MUTEX(shmem_comp_defaults_lock);
static bool shmem_comp_default_enabled;
static char *shmem_comp_default_compressor;
static unsigned int shmem_comp_default_merge = 1;
static unsigned int shmem_comp_default_wm_low = 50;	/* Stage 5 flip
							 * (2026-10-08): built-in
							 * default 50:70 -- chill
							 * below 50% usage, hot at
							 * or above 70%, hysteresis
							 * band between.  Measured
							 * rationale in
							 * TMPFS_COMPRESSION_PLAN.md
							 * "Configuration guidance":
							 * free at low concurrency,
							 * ~2x eager at high, 1MB vs
							 * 166MB live compression RAM
							 * on transient workloads,
							 * and the only policy that
							 * relieves inline at the
							 * capacity cliff.  Explicit
							 * compress_watermark=0:0 on
							 * the mount restores eager
							 * (watermark_seen). */
static unsigned int shmem_comp_default_wm_high = 70;

static const char *shmem_comp_off_reason_str(enum shmem_comp_off_reason reason)
{
	switch (reason) {
	case SHMEM_COMP_OFF_NOFLAG:
		return "nocompress";
	case SHMEM_COMP_OFF_ALGO_NONE:
		return "compress=none";
	case SHMEM_COMP_OFF_WATERMARK:
		return "compress_watermark=100:100";
	case SHMEM_COMP_OFF_DEFAULT:
	default:
		return "not participating (default off)";
	}
}

/*
 * shmem_comp_opts_init - Initialise parsed mount options to defaults
 * @opts: options to initialise
 *
 * "compress" inherits the global sysfs default (mounts that never
 * mention a compression option follow it); everything else is unset
 * (zero/NULL) until fill_sb() substitutes the defaults.
 */
void shmem_comp_opts_init(struct shmem_comp_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	mutex_lock(&shmem_comp_defaults_lock);
	opts->compress = shmem_comp_default_enabled;
	mutex_unlock(&shmem_comp_defaults_lock);
}

void shmem_comp_opts_free(struct shmem_comp_opts *opts)
{
	kfree(opts->compressor);
	opts->compressor = NULL;
}

/*
 * shmem_comp_opts_set_algo - Parse the compress= mount option
 * @opts: options to update
 * @algo: algorithm string from the mount option (may be NULL/empty)
 *
 * Return: 0 on success, -EINVAL when @algo names an unavailable
 * algorithm (caller frees @algo and reports the mount error).
 *
 * Consumes @algo on success: stored in opts->compressor, or freed for
 * "none"/empty (which merely toggle opts->compress). Empty and "none"
 * both mean "no compression"; anything else must be an acomp algo or
 * the mount fails early on typos.
 */
int shmem_comp_opts_set_algo(struct shmem_comp_opts *opts, char *algo)
{
	if (!algo || !algo[0]) {
		/* compress with the default algorithm */
		opts->compress = true;
		kfree(algo);
		return 0;
	}
	if (!strcmp(algo, "none")) {
		opts->compress = false;
		opts->off_reason = SHMEM_COMP_OFF_ALGO_NONE;
		kfree(algo);
		return 0;
	}
	if (!crypto_has_acomp(algo, 0, 0))
		return -EINVAL;		/* not consumed */
	kfree(opts->compressor);
	opts->compressor = algo;
	opts->compress = true;
	return 0;
}

/*
 * shmem_comp_opts_set_cell - Parse the cell= mount option
 * @opts: options to update
 * @val: cell size in bytes (memparse syntax)
 *
 * Return: 0 on success, -EINVAL on a bad value (mount fails).
 *
 * cell=0 explicitly opts out of cell storage (zsmalloc backend); the
 * cell_seen flag distinguishes that from "not given", which lets
 * fill_sb() derive an order from the filesystem size.
 */
int shmem_comp_opts_set_cell(struct shmem_comp_opts *opts, const char *val)
{
	unsigned long bytes = memparse(val, NULL);

	if (bytes == 0) {
		opts->cell_order = 0;
		opts->cell_seen = true;
		return 0;
	}
	if (!is_power_of_2(bytes) ||
	    bytes < (PAGE_SIZE << CELL_ORDER_MIN) ||
	    bytes > (PAGE_SIZE << CELL_ORDER_MAX))
		return -EINVAL;
	opts->cell_order = ilog2(bytes / PAGE_SIZE);
	opts->cell_seen = true;
	return 0;
}

/*
 * shmem_comp_opts_set_watermark - Parse the compress_watermark= mount option
 * @opts: options to update
 * @val: "LOW:HIGH" percents (decimal)
 *
 * Return: 0 on success, -EINVAL on a bad value (mount fails).
 *
 * Sentinels: 0:0 = always (eager), 100:100 = never (identical to
 * nocompress, no pool), 50:100 = pure deferral. low <= high is
 * required (hysteresis band, never inverted). The watermark_seen flag
 * distinguishes an explicit "0:0" from "not given", which lets
 * fill_sb() take the sysfs default only when the mount did not ask.
 */
int shmem_comp_opts_set_watermark(struct shmem_comp_opts *opts, const char *val)
{
	unsigned int low, high;
	char *rest;

	low = simple_strtoul(val, &rest, 10);
	if (*rest != ':')
		return -EINVAL;
	high = simple_strtoul(rest + 1, &rest, 10);
	if (*rest || low > 100 || high > 100 || low > high)
		return -EINVAL;
	opts->watermark_low = low;
	opts->watermark_high = high;
	opts->watermark_seen = true;
	return 0;
}

static unsigned int shmem_comp_default_merge_get(void)
{
	unsigned int v;

	mutex_lock(&shmem_comp_defaults_lock);
	v = shmem_comp_default_merge;
	mutex_unlock(&shmem_comp_defaults_lock);
	return v;
}

static char *shmem_comp_resolve_algo(const char *mount_algo)
{
	char *algo;

	if (mount_algo && mount_algo[0])
		return kstrdup(mount_algo, GFP_KERNEL);

	mutex_lock(&shmem_comp_defaults_lock);
	if (shmem_comp_default_compressor)
		algo = kstrdup(shmem_comp_default_compressor, GFP_KERNEL);
	else
		algo = kstrdup(CONFIG_TMPFS_COMPRESSOR_DEFAULT, GFP_KERNEL);
	mutex_unlock(&shmem_comp_defaults_lock);
	return algo;
}

static void shmem_comp_default_watermark_get(unsigned int *low,
					      unsigned int *high)
{
	mutex_lock(&shmem_comp_defaults_lock);
	*low = shmem_comp_default_wm_low;
	*high = shmem_comp_default_wm_high;
	mutex_unlock(&shmem_comp_defaults_lock);
}

static inline struct shmem_sb_info *shmem_comp_get_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * shmem_comp_fill_sb - Create the compression state for a new mount
 * @sbinfo: superblock info of the mount being filled
 * @opts: parsed mount options (never NULL)
 * @tmpfs_bytes: filesystem size in bytes, for cell-order derivation
 *
 * Return: 0 on success (also when the mount runs uncompressed),
 * -ENOMEM when the algorithm string cannot be allocated (mount fails).
 *
 * Algorithm resolution: mount option, else sysfs default, else Kconfig
 * default; empty strings and "none" all mean "no compression". Pool
 * creation failure is graceful: a warning and an uncompressed mount,
 * never a failed mount. On success sbinfo->comp is non-NULL and owns
 * the pool and the compressor string (freed by put_super()).
 *
 * Watermark resolution (WHEN layer): explicit mount option, else the
 * sysfs default, else the built-in default (0:0 = eager). The 100:100
 * sentinel resolves to "never": no pool, no memory overhead --
 * identical to nocompress, spelled out so a mount can be policy-forced
 * off without the WHAT layer. After fill_sb() the sysfs globals are
 * never read again for this mount.
 */
int shmem_comp_fill_sb(struct super_block *sb,
		       struct shmem_sb_info *sbinfo,
		       struct shmem_comp_opts *opts,
		       unsigned long tmpfs_bytes)
{
	enum shmem_comp_off_reason reason = opts->off_reason;
	struct shmem_comp_sb *comp;
	unsigned int wm_low, wm_high;
	char *algo;
	unsigned int cell_order;

	sbinfo->comp = NULL;

	if (opts->watermark_seen) {
		wm_low = opts->watermark_low;
		wm_high = opts->watermark_high;
	} else {
		shmem_comp_default_watermark_get(&wm_low, &wm_high);
	}

	if (!opts->compress) {
		pr_info("tmpfs: compression off (%s)\n",
			shmem_comp_off_reason_str(reason));
		return 0;
	}

	comp = kzalloc(sizeof(*comp), GFP_KERNEL);
	if (!comp)
		return -ENOMEM;

	algo = shmem_comp_resolve_algo(opts->compressor);
	if (!algo) {
		kfree(comp);
		return -ENOMEM;
	}

	if (strcmp(algo, "none") == 0) {
		/* "none": mount is uncompressed */
		if (reason == SHMEM_COMP_OFF_DEFAULT)
			reason = SHMEM_COMP_OFF_ALGO_NONE;
		pr_info("tmpfs: compression off (%s)\n",
			shmem_comp_off_reason_str(reason));
		kfree(algo);
		kfree(comp);
		return 0;
	}

	/*
	 * 100:100 sentinel: never compress. No pool, no memory
	 * overhead -- identical resolved state to nocompress.
	 */
	if (wm_low == 100 && wm_high == 100) {
		pr_info("tmpfs: compression off (%s)\n",
			shmem_comp_off_reason_str(SHMEM_COMP_OFF_WATERMARK));
		kfree(algo);
		kfree(comp);
		return 0;
	}

	cell_order = opts->cell_order;

	/*
	 * Default cell order when cell= is not given:
	 * scale with the filesystem size so ~100-1000 cells
	 * cover the tmpfs.  Larger tmpfs -> larger cells ->
	 * fewer alloc_pages calls, less per-cell metadata.
	 * Merged groups use larger blocks, so bump one order
	 * to keep ~10-30 blocks per cell.
	 *
	 * Explicit cell= always wins.  cell=0 opts out of
	 * cell storage entirely (zsmalloc backend) -- the
	 * cell_seen flag distinguishes that from "not given",
	 * which triggers the size-based derivation below.
	 *
	 * The pool's directory stays at the default capacity:
	 * tmpfs page cache is compressible and bounded by
	 * size= accounting, so it cannot walk into the
	 * incompressible-storage wall a block device hits.
	 */
	if (cell_order == 0 && !opts->cell_seen) {
		unsigned int merge_pages = opts->merge_pages ?: 1;

		if (tmpfs_bytes >= (256UL << 20))
			cell_order = 8;	/* 1 MiB */
		else if (tmpfs_bytes >= (32UL << 20))
			cell_order = 6;	/* 256 KiB */
		else
			cell_order = 4;	/* 64 KiB */

		if (merge_pages >= 8 && cell_order < CELL_ORDER_MAX)
			cell_order++;
	}

	comp->pool = shmem_comp_pool_create(algo, cell_order, 0);
	if (IS_ERR(comp->pool)) {
		pr_warn("tmpfs: failed to create compression pool, "
			"running uncompressed\n");
		comp->pool = NULL;
		kfree(algo);
		kfree(comp);
		return 0;
	}

	comp->enabled = true;
	/* ownership transfers */
	comp->compressor = algo;
	comp->threshold_bytes = opts->threshold_bytes ?: 65536;
	comp->watermark_low = wm_low;
	comp->watermark_high = wm_high;
	comp->cell_order = cell_order;
	comp->merge_pages = opts->merge_pages ?:
		      (shmem_comp_default_merge_get() ?: 1);
	atomic_long_set(&comp->pages, 0);
	atomic_long_set(&comp->bytes, 0);
	/* Stage 2: deferred-compression policy state */
	comp->sb = sb;
	/*
	 * Initial mode: eager (0:0) mounts start hot so today's inline
	 * behavior is unchanged. Any positive HIGH watermark starts chill
	 * (usage at mount is 0) -- the first maintenance pass flips it.
	 */
	comp->hot = (wm_high == 0);
	INIT_DELAYED_WORK(&comp->age_work, shmem_comp_age_work);
	atomic_long_set(&comp->aged_pages, 0);
	atomic_long_set(&comp->skipped_dead, 0);
	mutex_lock(&shmem_comp_mounts_lock);
	list_add(&comp->list, &shmem_comp_mounts);
	mutex_unlock(&shmem_comp_mounts_lock);
	sbinfo->comp = comp;

	pr_info("tmpfs: compression on, algo=%s merge_pages=%u threshold=%u "
		"cell_order=%u watermark=%u:%u\n",
		comp->compressor, comp->merge_pages, comp->threshold_bytes,
		comp->cell_order, comp->watermark_low, comp->watermark_high);
	return 0;
}

/*
 * shmem_comp_put_super - Release the compression state of a mount
 * @sbinfo: superblock info being torn down
 *
 * Safe on a partially initialized or NULL comp: the mount-failure path
 * in shmem_fill_super() calls this after any failure following
 * sbinfo allocation.
 */
void shmem_comp_put_super(struct shmem_sb_info *sbinfo)
{
	struct shmem_comp_sb *comp = sbinfo->comp;

	if (!comp)
		return;

	/*
	 * Stage 2 teardown ordering, revised for the re-arm race.  The
	 * maintain pass RE-ARMS age_work when its budget is exhausted,
	 * so "cancel first, delist later" would leave a window where a
	 * concurrent gc_now/pressure pass re-arms a comp that put_super
	 * is about to free (cancel returns, list_del, kfree -- then the
	 * re-armed work fires on freed memory).  Delist FIRST so no
	 * caller can find this comp again, then cancel outside the
	 * mutex: the work takes shmem_comp_mounts_lock, so holding it
	 * across the cancel would deadlock (work waits for the lock,
	 * put_super waits for the work).  A concurrent pass that already
	 * found this comp finishes (and may re-arm) before put_super
	 * gets the mutex -- and the cancel below then removes that
	 * re-armed pending work.  After list_del + completed cancel no
	 * path can schedule age_work for this comp: write_end arms are
	 * excluded anyway (the sb is shutting down under s_umount
	 * write) and maintain itself can no longer be reached.
	 */
	mutex_lock(&shmem_comp_mounts_lock);
	list_del(&comp->list);
	mutex_unlock(&shmem_comp_mounts_lock);
	cancel_delayed_work_sync(&comp->age_work);

	if (comp->pool)
		shmem_comp_pool_put(comp->pool);
	kfree(comp->compressor);
	kfree(comp);
	sbinfo->comp = NULL;
}

bool shmem_comp_should_compress(struct inode *inode)
{
	struct shmem_comp_sb *comp = shmem_comp_get_sb(inode->i_sb)->comp;

	/*
	 * Stage 2: the write path consults the hot/chill mode flag with a
	 * single READ_ONCE. In chill mode (usage below the LOW watermark)
	 * writes land uncompressed; the maintenance pass ages them later
	 * (death-before-pop). Eager mounts (0:0) keep hot=true always.
	 */
	return comp && comp->enabled && comp->pool &&
		READ_ONCE(comp->hot) &&
		i_size_read(inode) >= comp->threshold_bytes;
}

/*
 * shmem_comp_arm_aging - Ask for a maintenance pass on this mount
 * @inode: an inode on a compressed mount
 *
 * Called from shmem_write_end() when a write did NOT compress inline
 * (chill mode, or a sub-threshold write in hot mode). The page it left
 * behind is a candidate for the aging pass. Scheduling an already-
 * pending delayed_work is cheap (test-and-set), so this stays off the
 * hot path's critical section. No-op on uncompressed mounts.
 */
void shmem_comp_arm_aging(struct inode *inode)
{
	struct shmem_comp_sb *comp = shmem_comp_get_sb(inode->i_sb)->comp;

	if (!comp || !comp->pool)
		return;
	schedule_delayed_work(&comp->age_work,
			      msecs_to_jiffies(SHMEM_COMP_AGE_DELAY_MS));
}

/*
 * shmem_comp_note_unmap - Record an uncompressed page leaving the cache
 * @inode: inode whose folio was truncated/evicted
 *
 * Called from shmem_undo_range() next to each full folio removal. When
 * the mount is chilled, such a page died without ever paying a
 * compression cycle -- the CPU death-before-pop saved. Counted only
 * while chilled: an eager-mode death is not a policy win (that page
 * was already attempted at write time and was incompressible, mapped,
 * or below threshold). Covers unlink, truncate, fallocate punch and
 * evict (all route through shmem_undo_range).
 */
void shmem_comp_note_unmap(struct inode *inode)
{
	struct shmem_comp_sb *comp = shmem_comp_get_sb(inode->i_sb)->comp;

	if (!comp || !comp->pool || READ_ONCE(comp->hot))
		return;
	atomic_long_inc(&comp->skipped_dead);
	atomic_long_inc(&shmem_comp_skipped_dead);
}

/*
 * shmem_comp_show_options - Print the resolved compression config
 * @seq: seq_file of /proc/mounts
 * @sbinfo: superblock info of the mount
 *
 * Called from shmem_show_options(): /proc/mounts tells the truth about
 * compression. A mount that does not compress says so (nocompress) --
 * the cell256k-class bug (a mount benchmarking as plain tmpfs, with
 * nothing in the mount line showing why) is visible exactly here. A
 * compressed mount shows its effective algo, watermark and geometry
 * (merge_pages only when non-default; cell_order is deliberately
 * internal).
 */
void shmem_comp_show_options(struct seq_file *seq,
			     struct shmem_sb_info *sbinfo)
{
	struct shmem_comp_sb *comp = sbinfo->comp;

	if (!comp || !comp->enabled) {
		seq_puts(seq, ",nocompress");
		return;
	}
	seq_printf(seq, ",compress=%s", comp->compressor);
	seq_printf(seq, ",compress_watermark=%u:%u",
		   comp->watermark_low, comp->watermark_high);
	if (comp->merge_pages > 1)
		seq_printf(seq, ",merge_pages=%u", comp->merge_pages);
}

/*
 * shmem_comp_free_swap - Release one compressed page-cache slot
 * @mapping: mapping whose mount owns the entry
 * @radswap: the radix entry holding the compression ID
 *
 * Called from shmem_free_swap() for compression entries only. The slot
 * erase happens there under the xas lock; here we drop the slot's
 * entry reference and update the once-per-group accounting. get/release
 * are generation-validated: between the erase and this call the entry
 * can die and the ID be recycled -- a stale value must not be charged
 * against the new incarnation. On a stale value got/last are both
 * false: the old entry's accounting already ran when IT died.
 */
void shmem_comp_free_swap(struct address_space *mapping, void *radswap)
{
	struct shmem_comp_sb *comp =
		shmem_comp_get_sb(mapping->host->i_sb)->comp;
	swp_entry_t entry = radix_to_swp_entry(radswap);
	unsigned int id = shmem_comp_entry_id(entry);
	unsigned int gen = shmem_comp_entry_gen(entry);
	unsigned int length = 0, entry_nr = 1;
	bool got, last;

	got = shmem_comp_entry_get_gen(id, gen, NULL, &length, &entry_nr,
					NULL, NULL);
	last = shmem_comp_entry_release(id, gen);
	if (got)
		shmem_comp_entry_put(id);
	if (comp && last && got) {
		atomic_long_sub(entry_nr, &comp->pages);
		atomic_long_sub(length, &comp->bytes);
		shmem_comp_update_stats(false, entry_nr * PAGE_SIZE, length);
	}
}

#ifdef CONFIG_SYSFS
/*
 * Sysfs attributes under /sys/fs/tmpfs, group "compression".
 *
 * The "compression", "compressor" and "merge_pages" attributes are the
 * global defaults for mounts that do not override them; "comp_stats",
 * "cell_stats" and "gc_now" expose the live state of every pool, and
 * "mount_stats" breaks the per-mount counters out so several compressed
 * mounts can be observed in one boot.  Registered by
 * tmpfs_sysfs_init() in mm/shmem.c with the tmpfs kobject as parent.
 */
static ssize_t compression_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&shmem_comp_defaults_lock);
	ret = sysfs_emit(buf, "%d\n", shmem_comp_default_enabled);
	mutex_unlock(&shmem_comp_defaults_lock);
	return ret;
}

static ssize_t compression_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	bool enabled;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	mutex_lock(&shmem_comp_defaults_lock);
	shmem_comp_default_enabled = enabled;
	mutex_unlock(&shmem_comp_defaults_lock);
	pr_info("tmpfs: default compression %s\n",
		enabled ? "enabled" : "disabled");
	return count;
}
static struct kobj_attribute compression_attr = __ATTR(compression, 0644,
					compression_show, compression_store);

static ssize_t compressor_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&shmem_comp_defaults_lock);
	ret = sysfs_emit(buf, "%s\n",
			 shmem_comp_default_compressor ?:
			 CONFIG_TMPFS_COMPRESSOR_DEFAULT);
	mutex_unlock(&shmem_comp_defaults_lock);
	return ret;
}

static ssize_t compressor_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	char *algo;

	algo = kstrdup(buf, GFP_KERNEL);
	if (!algo)
		return -ENOMEM;

	/* Strip newline */
	algo[strcspn(algo, "\n")] = '\0';

	/* Validate: "none" disables, anything else must be an acomp algo */
	if (strcmp(algo, "none") != 0 && !crypto_has_acomp(algo, 0, 0)) {
		pr_warn("tmpfs: compressor '%s' not available\n", algo);
		kfree(algo);
		return -EINVAL;
	}

	mutex_lock(&shmem_comp_defaults_lock);
	/* Free old value. "none" is stored as-is: default-following
	 * mounts resolve algo "none" and run uncompressed (Layer 1
	 * normalization -- one resolved state, no pool). */
	kfree(shmem_comp_default_compressor);
	shmem_comp_default_compressor = algo;
	pr_info("tmpfs: default compressor set to %s\n", algo);
	mutex_unlock(&shmem_comp_defaults_lock);
	return count;
}
static struct kobj_attribute compressor_attr = __ATTR(compressor, 0644,
					compressor_show, compressor_store);

static ssize_t comp_stats_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct shmem_comp_stats stats;

	shmem_comp_get_stats(&stats);

	return sysfs_emit(buf,
		"compressed_pages: %lu\n"
		"uncompressed_pages: %lu\n"
		"total_compressed_bytes: %lu\n"
		"total_uncompressed_bytes: %lu\n"
		"ratio: %lu.%02lu%%\n"
		"compress_fails: %lu\n"
		"decompress_fails: %lu\n"
		"stale_value_retries: %lu\n"
		"aged_pages: %lu\n"
		"skipped_dead: %lu\n"
		"ram_pool_bytes: %lu\n"
		"ram_merged_bytes: %lu\n"
		"ram_entry_bytes: %lu\n"
		"ram_entries: %lu\n"
		"ram_total_bytes: %lu\n",
		stats.compressed_pages,
		stats.uncompressed_pages,
		stats.total_compressed_bytes,
		stats.total_uncompressed_bytes,
		stats.ratio / 100,
		stats.ratio % 100,
		stats.compress_fails,
		stats.decompress_fails,
		stats.stale_value_retries,
		(unsigned long)atomic_long_read(&shmem_comp_aged_pages),
		(unsigned long)atomic_long_read(&shmem_comp_skipped_dead),
		stats.ram_pool_bytes,
		stats.ram_merged_bytes,
		stats.ram_entry_bytes,
		stats.ram_entries,
		stats.ram_total_bytes);
}
static ssize_t merge_pages_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&shmem_comp_defaults_lock);
	ret = sysfs_emit(buf, "%u\n", shmem_comp_default_merge);
	mutex_unlock(&shmem_comp_defaults_lock);
	return ret;
}

static ssize_t merge_pages_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(buf, 10, &v);
	if (ret)
		return ret;
	if (v < 1 || v > SHMEM_COMP_MERGE_MAX)
		return -EINVAL;

	mutex_lock(&shmem_comp_defaults_lock);
	shmem_comp_default_merge = v;
	mutex_unlock(&shmem_comp_defaults_lock);
	pr_info("tmpfs: default merge_pages set to %u\n", v);
	return count;
}
static struct kobj_attribute merge_pages_attr = __ATTR(merge_pages, 0644,
					merge_pages_show, merge_pages_store);

static ssize_t compress_watermark_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	int ret;

	mutex_lock(&shmem_comp_defaults_lock);
	ret = sysfs_emit(buf, "%u:%u\n", shmem_comp_default_wm_low,
			 shmem_comp_default_wm_high);
	mutex_unlock(&shmem_comp_defaults_lock);
	return ret;
}

static ssize_t compress_watermark_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct shmem_comp_opts opts;	/* reuses the mount-option parser */
	char *val;
	int ret;

	val = kstrdup(buf, GFP_KERNEL);
	if (!val)
		return -ENOMEM;
	val[strcspn(val, "\n")] = '\0';

	/*
	 * One grammar, one validator: the sysfs default knob and the
	 * mount option share shmem_comp_opts_set_watermark(), so the
	 * sentinel semantics (0:0 eager, 100:100 never) cannot drift.
	 */
	memset(&opts, 0, sizeof(opts));
	ret = shmem_comp_opts_set_watermark(&opts, val);
	kfree(val);
	if (ret)
		return ret;

	mutex_lock(&shmem_comp_defaults_lock);
	shmem_comp_default_wm_low = opts.watermark_low;
	shmem_comp_default_wm_high = opts.watermark_high;
	mutex_unlock(&shmem_comp_defaults_lock);
	pr_info("tmpfs: default compress_watermark set to %u:%u\n",
		opts.watermark_low, opts.watermark_high);
	return count;
}
static struct kobj_attribute compress_watermark_attr =
	__ATTR(compress_watermark, 0644, compress_watermark_show,
	       compress_watermark_store);

/*
 * Per-mount counters.  comp_stats above aggregates every pool, so on a
 * boot with several compressed mounts it cannot attribute anything to
 * a single one -- the policy matrix worked around that by running one
 * mount per boot and reading global deltas (sessions 15-18), which
 * cost a boot per configuration.  This file removes the workaround.
 *
 * One line per compressed mount.  @dev is sb->s_dev in decimal, the
 * same number `stat -c %d <mountpoint>` prints, so a test can pick out
 * its own mount:
 *
 *   grep "^dev=$(stat -c %d /mnt/x) " /sys/fs/tmpfs/compression/mount_stats
 *
 * Chosen over a per-mount kobject because the only consumer is a
 * reader; a kobject tree would add lifetime management tied to
 * fill_sb/put_super to serve one grep.
 */
static ssize_t mount_stats_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct shmem_comp_sb *comp;
	int len = 0;

	/*
	 * The list mutex serialises against put_super's teardown, same
	 * as gc_now.  Only atomic counter reads happen inside -- no
	 * allocation, no sleeping call -- so this is safe from a sysfs
	 * read regardless of what the mount is doing.
	 */
	mutex_lock(&shmem_comp_mounts_lock);
	list_for_each_entry(comp, &shmem_comp_mounts, list) {
		unsigned int major = MAJOR(comp->sb->s_dev);
		unsigned int minor = MINOR(comp->sb->s_dev);

		len += sysfs_emit_at(buf, len,
			"dev=%u:%u stat_dev=%lu mode=%s wm=%u:%u "
			"pages=%ld bytes=%ld aged=%ld skipped=%ld "
			"cells=%d\n",
			major, minor,
			(unsigned long)comp->sb->s_dev,
			READ_ONCE(comp->hot) ? "hot" : "chill",
			comp->watermark_low, comp->watermark_high,
			atomic_long_read(&comp->pages),
			atomic_long_read(&comp->bytes),
			atomic_long_read(&comp->aged_pages),
			atomic_long_read(&comp->skipped_dead),
			comp->pool && shmem_comp_pool_uses_cells(comp->pool));
	}
	mutex_unlock(&shmem_comp_mounts_lock);

	return len;
}
static struct kobj_attribute mount_stats_attr = __ATTR_RO(mount_stats);

static struct kobj_attribute comp_stats_attr = __ATTR_RO(comp_stats);

static ssize_t cell_stats_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return shmem_comp_cell_stats_show(buf, PAGE_SIZE);
}
static struct kobj_attribute cell_stats_attr = __ATTR_RO(cell_stats);

/*
 * Debug hook: run one async GC pass on all cell-backed pools.
 * Lets tests exercise evacuation deterministically instead of
 * waiting for memory pressure to hit the shrinker.
 */
static ssize_t gc_now_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct shmem_comp_sb *comp;
	bool v;
	int ret;

	ret = kstrtobool(buf, &v);
	if (ret)
		return ret;
	if (!v)
		return count;

	/*
	 * Stage 2: gc_now runs one maintenance pass on every compressed
	 * mount (aging + per-pool GC), then the pool-wide cell GC. The
	 * mount pass is the deterministic test trigger for the policy
	 * matrix -- it exercises aging without waiting for the armed work.
	 * Held under shmem_comp_mounts_lock so the pass is serialized
	 * against put_super's teardown.
	 */
	mutex_lock(&shmem_comp_mounts_lock);
	list_for_each_entry(comp, &shmem_comp_mounts, list)
		shmem_comp_maintain(comp);
	mutex_unlock(&shmem_comp_mounts_lock);

	shmem_comp_cell_gc_all();
	return count;
}
static struct kobj_attribute gc_now_attr = __ATTR(gc_now, 0200, NULL,
						  gc_now_store);

static struct attribute *tmpfs_comp_attrs[] = {
	&compression_attr.attr,
	&compressor_attr.attr,
	&merge_pages_attr.attr,
	&compress_watermark_attr.attr,
	&comp_stats_attr.attr,
	&mount_stats_attr.attr,
	&cell_stats_attr.attr,
	&gc_now_attr.attr,
	NULL,
};

static const struct attribute_group tmpfs_comp_attribute_group = {
	.attrs = tmpfs_comp_attrs,
	.name = "compression",
};

int shmem_comp_sysfs_register(struct kobject *parent)
{
	return sysfs_create_group(parent, &tmpfs_comp_attribute_group);
}

void shmem_comp_sysfs_unregister(void)
{
	/*
	 * tmpfs sysfs lives for the lifetime of the system (the tmpfs
	 * filesystem is never unregistered), so there is no teardown
	 * path yet. Kept for API symmetry with register().
	 */
}
#endif /* CONFIG_SYSFS */

/*
 * Compression data path: compress on write, decompress on access.
 *
 * Moved here from mm/shmem.c (carve C1). This translation unit is the
 * sole in-tree consumer of the exported shmem primitives
 * (shmem_recalc_inode, shmem_add_to_page_cache,
 * shmem_delete_from_page_cache, shmem_alloc_folio); adding a second
 * consumer of those exports is a design change, not a convenience.
 */


int shmem_compress_folio(struct inode *inode, pgoff_t index,
				struct folio *folio);
static struct folio *shmem_comp_get_folio_noalloc(struct inode *inode,
						  pgoff_t pgoff);
static int shmem_decompress_group(struct inode *inode, pgoff_t index,
				   unsigned int id, unsigned int gen,
				   unsigned int comp_len,
				   unsigned short nr_pages,
				   gfp_t gfp, struct folio **foliop);
static int shmem_compress_group(struct inode *inode, pgoff_t last);

/*
 * Fetch an existing folio without allocating: used by group
 * compression to gather the sibling pages of a merge group. The
 * shmem_get_folio_noalloc() helper is only compiled with
 * CONFIG_USERFAULTFD, so keep a local wrapper that works
 * unconditionally.
 */
static struct folio *shmem_comp_get_folio_noalloc(struct inode *inode,
						  pgoff_t pgoff)
{
	struct folio *folio;
	int err;

	err = shmem_get_folio(inode, pgoff, 0, &folio, SGP_NOALLOC);
	if (err)
		return ERR_PTR(err);

	return folio;
}

/*
 * Decompress a merged group (merge_pages > 1) and install one folio
 * per slice. The ID entry is replicated in every slot of the group;
 * each successfully installed slice releases one reference, and the
 * entry plus its zsmalloc block are freed with the last release.
 *
 * The folio covering @index is returned locked (shmem_get_folio()
 * contract); sibling slices remain cached, so sequential reads of a
 * group cost a single decompression.
 *
 * Per-slot races (concurrent reader, truncation) are resolved by
 * shmem_add_to_page_cache(): it only installs a folio if the slot
 * still holds the replicated ID+generation, otherwise -EEXIST is
 * returned and the caller retries.
 */
static int shmem_decompress_group(struct inode *inode, pgoff_t index,
				   unsigned int id, unsigned int gen,
				   unsigned int comp_len,
				   unsigned short nr_pages,
				   gfp_t gfp, struct folio **foliop)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	pgoff_t head = index - (index % nr_pages);
	unsigned long *handles = NULL;
	unsigned short nchunks = 0;
	void *cbuf, *dbuf, *dst;
	unsigned int dlen;
	unsigned short slice;
	int ret = 0;

	*foliop = NULL;

	/*
	 * Pin the entry for the whole group decompress. The pin is
	 * generation-validated: the caller's xarray value can be stale
	 * (truncate freed the entry, a rewrite recycled the ID), and a
	 * bare-ID pin could land on the wrong incarnation -- torn reads
	 * plus a slot release charged to an entry this path never
	 * owned. Every error path below drops this pin; the final put
	 * balances it. (The cell-backed path used to drop a pin it
	 * never took: refs corruption on every cell+merge group
	 * decompress whose ID got recycled.)
	 */
	if (!shmem_comp_entry_get_gen(id, gen, NULL, NULL, NULL,
				      &handles, &nchunks))
		return -ENOENT;

	/*
	 * Load the compressed data.  Cell storage uses the pinned,
	 * retry-safe loader: GC may relocate the block between the
	 * caller's entry_get() and this copy, and a stale handle would
	 * memcpy from a cell already returned to the buddy.  Zsmalloc
	 * loads chunk by chunk through the handle array.  load_entry()
	 * re-pins internally; our pin above keeps the ID from being
	 * recycled underneath it.
	 */
	if (shmem_comp_pool_uses_cells(sbinfo->comp->pool)) {
		cbuf = kmalloc(comp_len, GFP_KERNEL);
		if (!cbuf) {
			shmem_comp_entry_put(id);
			return -ENOMEM;
		}
		ret = shmem_comp_load_entry(id, cbuf, &comp_len);
		if (ret) {
			kfree(cbuf);
			shmem_comp_entry_put(id);
			return ret;
		}
	} else {
		if (!handles) {
			shmem_comp_entry_put(id);
			return -ENOENT;
		}

		cbuf = kmalloc(comp_len, GFP_KERNEL);
		if (!cbuf) {
			shmem_comp_entry_put(id);
			return -ENOMEM;
		}

		ret = shmem_comp_load_merged(sbinfo->comp->pool, handles,
					      nchunks, cbuf, &comp_len);
		if (ret) {
			kfree(cbuf);
			shmem_comp_entry_put(id);
			return ret;
		}
	}

	dbuf = kmalloc((size_t)nr_pages * PAGE_SIZE, GFP_KERNEL);
	if (!dbuf) {
		kfree(cbuf);
		shmem_comp_entry_put(id);
		return -ENOMEM;
	}

	dlen = (unsigned int)nr_pages * PAGE_SIZE;
	ret = shmem_comp_decompress(sbinfo->comp->pool, cbuf, comp_len,
				    dbuf, &dlen);
	if (ret) {
		kfree(cbuf);
		kfree(dbuf);
		shmem_comp_entry_put(id);
		return ret;
	}

	/* Data corruption: a successful group decompress must produce
	 * exactly nr_pages pages. See the single-page check. */
	if (unlikely(dlen != (unsigned int)nr_pages * PAGE_SIZE))
		panic("shmem_comp: group decompressed %u bytes, expected "
		      "%u -- compressed data corrupt (id=%u)",
		      dlen, (unsigned int)((unsigned int)nr_pages * PAGE_SIZE), id);

	for (slice = 0; slice < nr_pages; slice++) {
		pgoff_t sidx = head + slice;
		struct folio *sib;

		/*
		 * A decompressed slice is a regular shmem page: it must be
		 * swapbacked (shmem_add_to_page_cache() asserts it) and on
		 * the LRU once installed (reclaim sees only LRU pages). The
		 * folio is private until it is stored, so the locked and
		 * swapbacked bits are set directly.
		 */
		sib = shmem_alloc_folio(gfp, 0, SHMEM_I(inode), sidx);
		if (!sib) {
			ret = -ENOMEM;
			break;
		}
		__folio_set_locked(sib);
		__folio_set_swapbacked(sib);

		dst = kmap_local_folio(sib, 0);
		memcpy(dst, dbuf + (size_t)slice * PAGE_SIZE, PAGE_SIZE);
		kunmap_local(dst);
		folio_mark_uptodate(sib);

		ret = shmem_add_to_page_cache(sib, mapping, sidx,
					       swp_to_radix_entry(
						       make_shmem_comp_entry(id, gen)),
					       gfp);
		if (ret) {
			folio_unlock(sib);
			folio_put(sib);
			if (sidx == index) {
				/* requested slice lost the race: retry */
				shmem_comp_entry_put(id);
				kfree(dbuf);
				return ret;
			}
			continue;
		}

		folio_add_lru(sib);

		/* Pair with the per-slice recalc in the compress path. */
		shmem_recalc_inode(inode, 0, -1);

		/* Consume this slice's slot reference; the pin taken
		 * above keeps the entry (and cbuf) alive regardless. */
		shmem_comp_entry_release(id, gen);

		if (sidx == index) {
			folio_set_referenced(sib);
			*foliop = sib;	/* locked, ref for caller */
		} else {
			folio_unlock(sib);
			folio_put(sib);
		}
		ret = 0;
	}

	/* One group decompressed: update accounting once */
	atomic_long_sub(nr_pages, &sbinfo->comp->pages);
	atomic_long_sub(comp_len, &sbinfo->comp->bytes);
	shmem_comp_update_stats(false, (unsigned int)nr_pages * PAGE_SIZE,
				comp_len);

	/* Drop the group pin taken at function entry: entry teardown
	 * (zsmalloc chunks + handle array) runs here if the slice loop
	 * consumed every slot reference. */
	shmem_comp_entry_put(id);

	kfree(cbuf);
	kfree(dbuf);
	return *foliop ? 0 : ret;
}

/*
 * Compress a completed merge group: @last is the index of the final
 * page of the group, so head = last + 1 - comp_merge. All pages are
 * copied into one buffer and compressed as a single block; the entry
 * ID is then replicated into every slot of the group.
 *
 * Folios stay locked across the compression to keep the group stable;
 * the inode i_rwsem (held by the write path) already excludes
 * truncation of this inode.
 */
static int shmem_comp_compress_chunk(struct inode *inode, pgoff_t head,
				     unsigned int nr)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct folio **folios;
	struct folio *folio;
	void *kbuf, *cbuf = NULL, *src;
	unsigned int comp_len;
	unsigned int id;
	unsigned int gen;
	unsigned int i;
	int ret = 0;

	if (!nr || nr > SHMEM_COMP_MERGE_MAX)
		return -EINVAL;

	/*
	 * Heap-allocate the folio array: at SHMEM_COMP_MERGE_MAX the
	 * pointer table alone is 8KiB, far too much for the kernel stack.
	 */
	folios = kcalloc(nr, sizeof(*folios), GFP_KERNEL);
	if (!folios)
		return -ENOMEM;

	kbuf = kmalloc((size_t)nr * PAGE_SIZE, GFP_KERNEL);
	if (!kbuf) {
		kfree(folios);
		return -ENOMEM;
	}

	for (i = 0; i < nr; i++) {
		folio = shmem_comp_get_folio_noalloc(inode, head + i);
		if (IS_ERR(folio)) {
			ret = PTR_ERR(folio);
			goto cleanup;
		}
		if (!folio || !folio_test_uptodate(folio)) {
			if (folio) {
				folio_unlock(folio);
				folio_put(folio);
			}
			ret = -EIO;
			goto cleanup;
		}
		/*
		 * The replicated-ID swap below removes every group
		 * member from the page cache. A member with live PTEs
		 * (shared mmap, forked children) must not be swapped:
		 * stores would land in the orphan while read() serves
		 * the compressed copy. Folio locks exclude new faults,
		 * so folio_mapped() is stable. Order-0 only, like the
		 * single-page path. Members are not required to be
		 * dirty: a decompressed group is clean by construction
		 * and recompressing it is safe. Skip the whole group;
		 * members stay uncompressed.
		 */
		if (folio_mapped(folio) || folio_maybe_dma_pinned(folio) ||
		    folio_order(folio)) {
			folio_unlock(folio);
			folio_put(folio);
			ret = 0;
			goto cleanup;
		}
		src = kmap_local_folio(folio, 0);
		memcpy(kbuf + (size_t)i * PAGE_SIZE, src, PAGE_SIZE);
		kunmap_local(src);
		folios[i] = folio;	/* stays locked until swap-out */
	}

	cbuf = kmalloc((size_t)nr * PAGE_SIZE, GFP_KERNEL);
	if (!cbuf) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* One crypto call covers the whole group */
	comp_len = (unsigned int)nr * PAGE_SIZE;
	ret = shmem_comp_compress(sbinfo->comp->pool, kbuf,
				  (unsigned int)nr * PAGE_SIZE,
				  cbuf, &comp_len);
	if (ret || comp_len >= nr * PAGE_SIZE) {
		/* incompressible group: keep pages uncompressed */
		ret = 0;
		goto cleanup;
	}

	/*
	 * Store the compressed group.  Cell storage uses a single
	 * block allocation — the entire group fits in one cell region
	 * and avoids the per-chunk handle array.  Zsmalloc chunks the
	 * group because ZS_MAX_ALLOC_SIZE is PAGE_SIZE; cell storage
	 * has no such limit (cells are up to 4 MiB).
	 */
	if (shmem_comp_pool_uses_cells(sbinfo->comp->pool)) {
		unsigned long handle;

		handle = shmem_comp_store(sbinfo->comp->pool,
					   cbuf, comp_len);
		if (!handle) {
			ret = 0;
			goto cleanup;
		}
		id = shmem_comp_entry_alloc(sbinfo->comp->pool, handle,
					     comp_len, nr, NULL, 0, &gen);
		if (!id) {
			u32 cellid;
			u32 offset;

			shmem_comp_unpack_loc(handle, &cellid, &offset);
			cell_free(shmem_comp_pool_cells(sbinfo->comp->pool),
				  cellid, offset, comp_len);
			ret = 0;
			goto cleanup;
		}
	} else {
		unsigned long *handles = NULL;
		unsigned short nchunks = 0;

		ret = shmem_comp_store_merged(sbinfo->comp->pool,
					      cbuf, comp_len,
					      &handles, &nchunks);
		if (ret) {
			ret = 0;
			goto cleanup;
		}
		id = shmem_comp_entry_alloc(sbinfo->comp->pool, 0,
					     comp_len, nr,
					     handles, nchunks, &gen);
		if (!id) {
			shmem_comp_free_merged(sbinfo->comp->pool,
					       handles, nchunks);
			ret = 0;
			goto cleanup;
		}
	}

	/*
	 * Compressed slots are accounted like swap pages: raise
	 * swapped before the slot swap lowers nrpages, so a racing
	 * shmem_recalc_inode() never sees the pages as freed (the
	 * same ordering shmem_writeout() relies on). Pairs with the
	 * -nr_swaps_freed recalc at the end of shmem_undo_range(),
	 * which restores the charge when the slots are truncated.
	 */
	shmem_recalc_inode(inode, 0, nr);

	/* Swap every folio of the group for the replicated ID entry */
	for (i = 0; i < nr; i++) {
		shmem_delete_from_page_cache(folios[i],
			swp_to_radix_entry(make_shmem_comp_entry(id, gen)));
		folio_unlock(folios[i]);
		folio_put(folios[i]);
	}

	atomic_long_add(nr, &sbinfo->comp->pages);
	atomic_long_add(comp_len, &sbinfo->comp->bytes);
	shmem_comp_update_stats(true, (unsigned int)nr * PAGE_SIZE,
				comp_len);

	kfree(cbuf);
	kfree(kbuf);
	kfree(folios);
	return 0;

cleanup:
	for (i = 0; i < nr; i++) {
		if (folios[i]) {
			folio_unlock(folios[i]);
			folio_put(folios[i]);
		}
	}
	kfree(cbuf);
	kfree(kbuf);
	kfree(folios);
	return ret;
}

/*
 * Choose how many pages of a merge group to compress together.
 *
 * Cell storage holds one compressed block per cell region and a
 * block cannot span cells, so a group whose compressed form might
 * exceed a cell must be split.  Compression never grows data
 * (incompressible groups are discarded), so bounding the
 * uncompressed slice by the cell size guarantees the compressed
 * slice fits.  The slice must also divide the merge size: the
 * group head is merge-aligned (the call site only fires on a full
 * group), so any divisor of the merge keeps every slice head aligned,
 * which is what shmem_decompress_group() relies on to recover the
 * head from an interior index (index - index % nr_pages).
 *
 * merge <= 1024, so the divisor scan is bounded and cheap.  A
 * prime merge larger than the cell falls back to single pages —
 * inefficient but correct, and unreachable for power-of-two merge
 * values with cells >= 16 KiB.
 */
static unsigned int shmem_comp_chunk_nr(struct shmem_sb_info *sbinfo,
					unsigned int nr)
{
	unsigned int cell_pages = shmem_comp_pool_cell_pages(sbinfo->comp->pool);
	unsigned int d;

	if (!cell_pages || nr <= cell_pages)
		return nr;

	for (d = cell_pages; d > 1; d--)
		if (nr % d == 0)
			return d;
	return 1;
}

static int shmem_compress_group(struct inode *inode, pgoff_t last)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	unsigned int nr = sbinfo->comp->merge_pages;
	unsigned int chunk, off;
	pgoff_t head;

	if (nr < 2 || nr > SHMEM_COMP_MERGE_MAX)
		return -EINVAL;

	head = last + 1 - nr;
	chunk = shmem_comp_chunk_nr(sbinfo, nr);

	for (off = 0; off < nr; off += chunk) {
		int ret = shmem_comp_compress_chunk(inode, head + off,
						    min(chunk, nr - off));
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * shmem_decompress_folio - Decompress a page from the compression pool
 * @inode: shmem inode
 * @index: page index in the file
 * @comp_entry: compression entry found in the page cache
 * @gfp: allocation flags for the replacement folio
 * @foliop: output, receives the decompressed folio
 *
 * Return: 0 on success, negative errno on failure
 *
 * Loads the compressed data from the zsmalloc pool, decompresses it
 * into a freshly allocated folio and atomically swaps the compression
 * entry for that folio in the page cache xarray. The folio is returned
 * locked and with a reference, matching the shmem_get_folio() contract.
 *
 * Races with a concurrent compress/decompress of the same index are
 * resolved by shmem_add_to_page_cache(): it only installs the folio if
 * the slot still holds @comp_entry, otherwise -EEXIST is returned and
 * the caller retries.
 */
int shmem_decompress_folio(struct inode *inode, pgoff_t index,
				  swp_entry_t comp_entry, gfp_t gfp,
				  struct folio **foliop)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	unsigned long handle;
	unsigned int comp_len;
	unsigned int id;
	unsigned int gen;
	unsigned int nr_pages;
	unsigned int dlen;
	void *src, *dst;
	int ret;

	*foliop = NULL;

	if (!sbinfo->comp)
		return -ENODEV;

	/*
	 * Pin the entry: handle/comp_len are used below outside RCU,
	 * and truncate can erase our slot (dropping the last slot
	 * reference) at any point. Generation-validated: the slot value
	 * this pin is derived from can be stale (entry died, ID
	 * recycled); fails with -ENOENT and the caller retries against
	 * the updated slot.
	 */
	id = shmem_comp_entry_id(comp_entry);
	gen = shmem_comp_entry_gen(comp_entry);
	if (!shmem_comp_entry_get_gen(id, gen, &handle, &comp_len,
				       &nr_pages, NULL, NULL))
		return -ENOENT;

	/*
	 * Merged group (merge_pages > 1): the ID+generation is
	 * replicated in all nr_pages slots of the group. Decompress the
	 * whole block and install one folio per slice; the requested
	 * slice is returned. The group path pins the entry itself
	 * (generation-validated); drop our pin first so the reference
	 * accounting stays one-pin-per-get.
	 */
	if (nr_pages > 1) {
		shmem_comp_entry_put(id);
		return shmem_decompress_group(inode, index, id, gen,
					       comp_len, nr_pages,
					       gfp, foliop);
	}

	/* Read the compressed bytes out of the pool first.
	 * GFP_KERNEL: transient slab buffer, must not carry __GFP_HIGHMEM.
	 */
	src = kmalloc(comp_len, GFP_KERNEL);
	if (!src) {
		shmem_comp_entry_put(id);
		return -ENOMEM;
	}

	/*
	 * Cell storage: the handle copied above can go stale between
	 * entry_get() and here — GC may relocate the block and return
	 * its cell to the buddy.  The retry-safe loader revalidates the
	 * handle under the cell's read lock.  Zsmalloc handles never
	 * move under us (objects stay put until zs_free), so the plain
	 * load is safe there.
	 */
	if (shmem_comp_pool_uses_cells(sbinfo->comp->pool))
		ret = shmem_comp_load_entry(id, src, &comp_len);
	else
		ret = shmem_comp_load(sbinfo->comp->pool, handle, src, &comp_len);
	if (ret) {
		kfree(src);
		shmem_comp_entry_put(id);
		return ret;
	}

	/*
	 * A decompressed page is a regular shmem page: it must be
	 * swapbacked (shmem_add_to_page_cache() asserts it) and on the
	 * LRU once installed (reclaim sees only LRU pages). The folio is
	 * private until it is stored, so the locked and swapbacked bits
	 * are set directly.
	 */
	folio = shmem_alloc_folio(gfp, 0, SHMEM_I(inode), index);
	if (!folio) {
		kfree(src);
		shmem_comp_entry_put(id);
		return -ENOMEM;
	}
	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);

	/* Decompress into the folio */
	dst = kmap_local_folio(folio, 0);
	dlen = PAGE_SIZE;
	ret = shmem_comp_decompress(sbinfo->comp->pool, src, comp_len,
				    dst, &dlen);
	kunmap_local(dst);
	kfree(src);

	if (ret) {
		folio_unlock(folio);
		folio_put(folio);
		shmem_comp_entry_put(id);
		return ret;
	}

	/* Data corruption: a successful decompress of a single-page
	 * block must produce exactly one page. Anything else means the
	 * compressed stream or the entry metadata is wrong -- serving
	 * it to userspace would be silent corruption, so stop. */
	if (unlikely(dlen != PAGE_SIZE))
		panic("shmem_comp: decompressed %u bytes, expected %u "
		      "-- compressed data corrupt (id=%u)",
		      dlen, (unsigned int)PAGE_SIZE, id);

	folio_mark_uptodate(folio);

	/*
	 * Atomically replace the compression entry with the folio.
	 * @comp_entry is passed as the expected xarray slot contents.
	 */
	ret = shmem_add_to_page_cache(folio, mapping, index,
				       swp_to_radix_entry(comp_entry), gfp);
	if (ret) {
		folio_unlock(folio);
		folio_put(folio);
		shmem_comp_entry_put(id);
		return ret;
	}

	/*
	 * nrpages rose with the folio install; drop the swap-style
	 * charge the compression raised (see shmem_compress_folio()).
	 */
	shmem_recalc_inode(inode, 0, -1);

	folio_add_lru(folio);

	/* Consume the slot's reference; our pin keeps the entry (and
	 * its zsmalloc block) alive until put() below, which runs the
	 * teardown when no other reference remains. */
	shmem_comp_entry_release(id, gen);

	/* Update accounting */
	atomic_long_dec(&sbinfo->comp->pages);
	atomic_long_sub(comp_len, &sbinfo->comp->bytes);
	shmem_comp_update_stats(false, PAGE_SIZE, comp_len);

	shmem_comp_entry_put(id);

	folio_set_referenced(folio);
	/* Hand back the folio locked, with a reference for the caller */
	*foliop = folio;
	return 0;
}

/**
 * shmem_compress_folio - Compress a folio and store in compression pool
 * @inode: shmem inode
 * @index: Page index in file
 * @folio: Folio to compress
 *
 * Return: 0 on success, negative errno on failure
 *
 * Compresses the folio data and stores it in the compression pool.
 * The folio is removed from the page cache and replaced with a
 * compressed entry. This allows the memory to be reclaimed while
 * preserving the data for later decompression.
 *
 * The compression is skipped if:
 * - The data doesn't compress well (incompressible)
 * - The compression pool is not available
 * - Memory allocation fails
 */
static int shmem_comp_compress_one(struct inode *inode, pgoff_t index,
				   struct folio *folio, bool allow_clean)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	swp_entry_t comp_entry;
	void *src, *comp_buf;
	unsigned int comp_len;
	unsigned int id;
	unsigned int gen;
	unsigned long handle;
	int ret;

	if (!sbinfo->comp)
		return -ENOTSUPP;

	/* Lock folio to ensure stability during compression */
	folio_lock(folio);

	/* Verify folio is still valid and uptodate */
	if (folio->mapping != mapping || !folio_test_uptodate(folio)) {
		folio_unlock(folio);
		return -ENOENT;
	}

	/*
	 * The slot swap below removes the folio from the page cache.
	 * That is only safe while nothing else can observe the folio:
	 * live PTEs (shared mmap, forked children) keep writing to the
	 * orphaned folio while read() serves the compressed copy. Folio
	 * locks exclude new faults (shmem_get_folio() takes the lock
	 * before mapping), so folio_mapped() is stable here. A pinned
	 * folio may be under DMA. Order-0 only: the compress and
	 * replace paths below assume a single page. write_end just
	 * dirtied this folio; clean means the state changed under us
	 * (allow_clean=false). The aging pass allows clean pages: a
	 * decompressed-on-read survivor is exactly the cold data worth
	 * re-compressing. Skipping is not an error: the page simply
	 * stays uncompressed.
	 */
	if (folio_mapped(folio) || folio_maybe_dma_pinned(folio) ||
	    folio_order(folio) || (!allow_clean && !folio_test_dirty(folio))) {
		folio_unlock(folio);
		return 0;
	}

	/* Get source data from folio */
	src = kmap_local_folio(folio, 0);

	/* Allocate buffer for compressed data */
	comp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!comp_buf) {
		kunmap_local(src);
		folio_unlock(folio);
		return -ENOMEM;
	}

	/* Compress the page */
	comp_len = PAGE_SIZE;
	ret = shmem_comp_compress(sbinfo->comp->pool, src, PAGE_SIZE,
				 comp_buf, &comp_len);
	kunmap_local(src);

	if (ret || comp_len >= PAGE_SIZE) {
		/* Compression didn't reduce size - skip */
		kfree(comp_buf);
		folio_unlock(folio);
		return -ENOSPC;
	}

	/* Store compressed data in zsmalloc pool */
	handle = shmem_comp_store(sbinfo->comp->pool, comp_buf, comp_len);
	kfree(comp_buf);

	if (!handle) {
		folio_unlock(folio);
		return -ENOMEM;
	}

	/*
	 * Register the compressed data in the entry ID table. The
	 * zsmalloc handle is a kernel pointer and cannot be packed
	 * into the xarray value, so the page cache stores only the
	 * {ID, generation} pair. The entry owns the handle from here
	 * (freed with the entry).
	 */
	id = shmem_comp_entry_alloc(sbinfo->comp->pool, handle, comp_len,
				    1, NULL, 0, &gen);
	if (!id) {
		shmem_comp_free(sbinfo->comp->pool, handle);
		folio_unlock(folio);
		return -ENOMEM;
	}
	comp_entry = make_shmem_comp_entry(id, gen);

	/*
	 * Compressed slots are accounted like swap pages: raise
	 * swapped before the slot swap lowers nrpages, so a racing
	 * shmem_recalc_inode() never sees the page as freed (the
	 * same ordering shmem_writeout() relies on). Pairs with the
	 * -nr_swaps_freed recalc at the end of shmem_undo_range(),
	 * which restores the charge when the slot is truncated.
	 */
	shmem_recalc_inode(inode, 0, 1);

	/*
	 * Atomically swap the folio for the compression entry in the
	 * page cache xarray. shmem_delete_from_page_cache() replaces
	 * the slot under xa_lock and drops the page cache's reference
	 * to the folio; the caller keeps its own reference.
	 */
	shmem_delete_from_page_cache(folio, swp_to_radix_entry(comp_entry));

	/* Update accounting */
	atomic_long_inc(&sbinfo->comp->pages);
	atomic_long_add(comp_len, &sbinfo->comp->bytes);
	shmem_comp_update_stats(true, PAGE_SIZE, comp_len);

	/*
	 * The folio has left the page cache; the caller still owns a
	 * reference and releases it after we return. Return 1 = compressed,
	 * 0 = skipped, negative = error (the aging budget counts 1 as work
	 * done).
	 */
	folio_unlock(folio);

	return 1;
}

/*
 * shmem_compress_folio - Compress a freshly-written folio (write path)
 * @inode: shmem inode
 * @index: page index
 * @folio: the uncompressed folio (unlocked; caller holds a reference)
 *
 * Write-path wrapper around shmem_comp_compress_one(): dispatches the
 * merge_pages>1 group case and requires dirty (allow_clean=false --
 * write_end just dirtied the folio). Returns 0 on success or skip; the
 * write path does not distinguish the two.
 */
int shmem_compress_folio(struct inode *inode, pgoff_t index,
			struct folio *folio)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	int ret;

	if (!sbinfo->comp)
		return -ENOTSUPP;

	/*
	 * merge_pages > 1: compress only when a full group has just
	 * been written (index is the last page of the group). Tail
	 * pages that do not fill a group remain uncompressed.
	 */
	if (sbinfo->comp->merge_pages > 1) {
		if ((index + 1) % sbinfo->comp->merge_pages == 0)
			return shmem_compress_group(inode, index);
		return 0;
	}

	ret = shmem_comp_compress_one(inode, index, folio, false);
	return ret < 0 ? ret : 0;
}

/*
 * shmem_comp_age_round - One bounded aging round over an inode batch
 * @comp: per-mount compression state
 * @batch: pinned shmem inodes (owned and released by the caller)
 * @nb: batch length
 * @budget: in/out page budget; decremented per compressed page
 *
 * Walks each inode's page cache from the start.  Already-compressed
 * pages come back as xarray values and are skipped cheaply, so
 * re-walking per round costs only the value scan.  Returns the number
 * of pages compressed; stops early when *budget hits 0.
 */
static unsigned int shmem_comp_age_round(struct shmem_comp_sb *comp,
					 struct inode **batch,
					 unsigned int nb,
					 unsigned int *budget)
{
	unsigned int aged = 0;
	unsigned int i;

	for (i = 0; i < nb && *budget; i++) {
		struct address_space *mapping = batch[i]->i_mapping;
		struct folio_batch fbatch;
		pgoff_t indices[FOLIO_BATCH_SIZE];
		pgoff_t index = 0;
		unsigned int j;

		folio_batch_init(&fbatch);
		while (*budget &&
		       find_get_entries(mapping, &index, (pgoff_t)-1,
					&fbatch, indices)) {
			for (j = 0; j < folio_batch_count(&fbatch); j++) {
				struct folio *folio = fbatch.folios[j];
				int ret;

				if (xa_is_value(folio))
					continue;	/* compressed entry or swap */
				ret = shmem_comp_compress_one(batch[i],
							       indices[j], folio, true);
				if (ret > 0) {
					aged++;
					(*budget)--;
				}
				if (!*budget)
					break;
			}
			/*
			 * find_get_entries() leaves xa_is_value entries (swap
			 * and compression entries) in the batch.
			 * folio_batch_release() -> folios_put() would
			 * folio_put_refs() them, treating the small value as
			 * a folio pointer (wild-memory-access in
			 * folios_put_refs). Prune them first -- same pattern
			 * as shmem_undo_range().
			 */
			folio_batch_remove_exceptionals(&fbatch);
			folio_batch_release(&fbatch);
			cond_resched();
		}
	}
	return aged;
}

/*
 * shmem_comp_maintain - One walk-based aging pass for a mount
 * @comp: the per-mount compression state
 *
 * Walks the mount's inodes (sb->s_inodes, prune_icache pinning pattern)
 * and compresses eligible uncompressed survivors via
 * shmem_comp_compress_one(allow_clean=true). This is the deferred-
 * compression half of death-before-pop: chill-mode writes land
 * uncompressed and are aged here instead of at write time.
 *
 * Concurrency contract (TMPFS_COMPRESSION_PLAN.md "Stage 2 machinery"):
 * the caller (shmem_comp_age_work, gc_now, or the shrinker's
 * pressure pass) holds
 * shmem_comp_mounts_lock. This function additionally takes s_umount
 * READ via down_read_trylock -- never a blocking down_read -- so it
 * cannot deadlock with put_super()'s cancel_delayed_work_sync (which
 * runs under s_umount write). Holding s_umount read excludes
 * evict_inodes for exactly as long as we hold inode references, so a
 * chilled file deleted under memory pressure cannot leave a busy inode
 * after unmount. Every __iget is balanced by an iput before return.
 *
 * Rounds: up to SHMEM_COMP_AGE_MAX_ROUNDS rounds of
 * SHMEM_COMP_AGE_BUDGET pages each (the round budget bounds the
 * per-pass cost regardless of data compressibility), with one cell-GC
 * batch after every round -- the ~4:1 aging:GC alternation:
 * compression raises cell density, which is what makes GC cheaper.
 *
 * Re-arm (plan step 4): if the last round hit its budget, candidates
 * likely remain, so schedule age_work again; otherwise idle until the
 * next write (or the shrinker) arms it.
 */
static void shmem_comp_maintain(struct shmem_comp_sb *comp)
{
	struct super_block *sb = comp->sb;
	struct inode *inode;
	struct inode *batch[SHMEM_COMP_AGE_BATCH_INODES];
	unsigned int nb = 0, i, r;
	bool more = false;

	if (!comp->pool || !sb)
		return;

	/*
	 * Mode flip with hysteresis. usage = used_blocks * 100 /
	 * max_blocks. The band (LOW, HIGH) keeps the current mode. For
	 * 0:0 (eager) the hot condition always holds -> permanently hot,
	 * never flips to chill (1 page on a large tmpfs reads usage 0;
	 * the sentinel must not flinch).
	 */
	if (comp->watermark_high == 0) {
		WRITE_ONCE(comp->hot, true);
	} else {
		struct shmem_sb_info *sbinfo = sb->s_fs_info;
		unsigned long max_blocks = sbinfo->max_blocks;
		unsigned long used = percpu_counter_sum(&sbinfo->used_blocks);
		unsigned int usage = max_blocks ? (used * 100 / max_blocks) : 0;
		bool hot = READ_ONCE(comp->hot);

		if (usage >= comp->watermark_high)
			hot = true;
		else if (usage <= comp->watermark_low)
			hot = false;
		/* else: usage in the hysteresis band, keep current mode */
		if (hot != READ_ONCE(comp->hot)) {
			WRITE_ONCE(comp->hot, hot);
			pr_info("tmpfs: mount %s compression %s (usage %u%%)\n",
				sb->s_id, hot ? "hot" : "chill", usage);
		}
	}

	if (!down_read_trylock(&sb->s_umount))
		return;	/* unmount in progress; skip this pass */

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		if (nb >= SHMEM_COMP_AGE_BATCH_INODES)
			break;
		if (!shmem_mapping(inode->i_mapping))
			continue;
		spin_lock(&inode->i_lock);
		if (icount_read(inode) == 0 ||
		    (inode_state_read(inode) & (I_FREEING | I_WILL_FREE | I_NEW))) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		batch[nb++] = inode;
	}
	spin_unlock(&sb->s_inode_list_lock);

	for (r = 0; r < SHMEM_COMP_AGE_MAX_ROUNDS; r++) {
		unsigned int budget = SHMEM_COMP_AGE_BUDGET;
		unsigned int aged = shmem_comp_age_round(comp, batch, nb,
							 &budget);

		if (aged) {
			atomic_long_add(aged, &comp->aged_pages);
			atomic_long_add(aged, &shmem_comp_aged_pages);
		}
		/* ~4:1 alternation: one GC batch per round of aging --
		 * compression raises cell density, which is what makes
		 * the next GC cheaper */
		if (comp->pool->use_cells)
			shmem_comp_cell_gc(comp->pool);
		if (budget) {
			/* candidates exhausted within the budget: done */
			break;
		}
		more = true;	/* round hit its budget: likely more remain */
	}

	/* batch is shared by all rounds; release once at the end */
	for (i = 0; i < nb; i++)
		iput(batch[i]);

	up_read(&sb->s_umount);

	/*
	 * Re-arm while still holding shmem_comp_mounts_lock: put_super
	 * delists under the same mutex BEFORE canceling the work (see the
	 * teardown ordering comment), so this schedule can no longer
	 * target a comp on its way out.
	 */
	if (more)
		schedule_delayed_work(&comp->age_work,
				      msecs_to_jiffies(SHMEM_COMP_AGE_DELAY_MS));
}

/*
 * shmem_comp_age_work - delayed_work callback: run one aging pass
 *
 * Runs on system_wq (process context; GFP_KERNEL is honest here --
 * unlike a shrinker, which is why the shrinker only arms this work and
 * never compresses inline). Holds shmem_comp_mounts_lock so the pass is
 * serialized against gc_now and against put_super's teardown.
 */
static void shmem_comp_age_work(struct work_struct *work)
{
	struct shmem_comp_sb *comp = container_of(to_delayed_work(work),
						  struct shmem_comp_sb, age_work);

	mutex_lock(&shmem_comp_mounts_lock);
	shmem_comp_maintain(comp);
	mutex_unlock(&shmem_comp_mounts_lock);
}

/*
 * shmem_comp_pressure_pass - async shrinker trigger for aging
 *
 * The shrinker runs under fs_reclaim; taking shmem_comp_mounts_lock
 * there would deadlock against maintain()'s own GFP_KERNEL
 * allocations (crypto contexts, kmalloc buffers, cell alloc_pages) --
 * the reclaim-recursion hazard session 3 hit once. So the shrinker
 * only schedules this work (atomic-safe, coalescing); the passes run
 * in kworker context where GFP_KERNEL is honest. Recorded deviation
 * from the plan's "triggered by the shrinker": the trigger is
 * asynchronous, and it runs ALL compressed mounts, not just those on
 * the pressuring pool -- the per-pass budget bounds the cost either
 * way, and pool membership is not worth a lock on the shrinker path.
 */
static void shmem_comp_pressure_pass(struct work_struct *work)
{
	struct shmem_comp_sb *comp;

	mutex_lock(&shmem_comp_mounts_lock);
	list_for_each_entry(comp, &shmem_comp_mounts, list)
		shmem_comp_maintain(comp);
	mutex_unlock(&shmem_comp_mounts_lock);
}

/*
 * Module initialization.
 */

static int __init shmem_comp_init(void)
{
	pr_info("shmem_comp: transparent compression for tmpfs initialized\n");
	return 0;
}

static void __exit shmem_comp_exit(void)
{
	pr_info("shmem_comp: transparent compression for tmpfs exited\n");
}

module_init(shmem_comp_init);
module_exit(shmem_comp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve SCHNEPP <steve.schnepp@pwkf.org>");
MODULE_DESCRIPTION("Transparent compression for tmpfs");

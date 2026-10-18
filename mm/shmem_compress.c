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
 *   3. entries[index] records the entry ID (the folio leaves i_pages)
 *
 * Decompression Flow:
 *   1. entries[index] resolved to {id, gen}
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
#include <linux/mm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/zsmalloc.h>
#include "shmem_comp_zscompat.h"	/* leveled zsmalloc API (shcomp_zs_*) */
#include <linux/swap.h>
#include <linux/workqueue.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <crypto/acompress.h>
#include <linux/memcontrol.h>
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
static void shmem_comp_pool_gc_work(struct work_struct *work);
static int shmem_comp_entries_set(struct inode *inode, pgoff_t index,
				  struct shmem_comp_entry *ent);
static unsigned int shmem_comp_invalidate_index(struct inode *inode,
						pgoff_t index);
static unsigned long shmem_comp_cell_count(struct shrinker *shrinker,
					   struct shrink_control *sc);
static unsigned long shmem_comp_cell_scan(struct shrinker *shrinker,
					  struct shrink_control *sc);

/* Stage 2: walk-based aging (deferred compression) */
static void shmem_comp_maintain(struct shmem_comp_sb *comp, bool hot);
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
	/*
	 * Compression output buffer (PAGE_SIZE).  Owned by this CPU's
	 * context and used by shmem_comp_compress_store() under @lock, so
	 * there is no per-call kmalloc for the compression output: the
	 * buffer stays valid across compress+store because the lock is held
	 * until the store's memcpy has read it.
	 */
	u8 *outbuf;
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
	unsigned int watermark_low;		/* chill return, RAM-usage percent (0:0 = eager) */
	unsigned int watermark_high;		/* hot compress, RAM-usage percent */
	unsigned int gc_percent;		/* cell GC sparse threshold (mirror of pool) */
	unsigned int merge_pages;		/* pages per compression group */
	unsigned int cell_order;	/* cell storage order, informational */
	struct shmem_comp_pool *pool;	/* compression pool */
	atomic_long_t pages;		/* compressed pages (this mount) */
	atomic_long_t bytes;		/* compressed bytes stored (this mount) */
	/* Stage 2: deferred-compression policy state */
	struct super_block *sb;		/* owning sb (inode walk, usage, s_umount) */
	bool hot;			/* mode flag: WRITE_ONCE in wm_update,
					 * READ_ONCE on the write path */
	bool wm_kick_spent;		/* one highwater kick per flip; reset at LOW */
	struct delayed_work age_work;	/* per-mount maintenance pass */
	/* Aging cadence tuning (mount-overridable; default = the header
	 * SHMEM_COMP_AGE_* constants). */
	unsigned int age_delay_ms;	/* aging-pass period */
	unsigned int age_budget;	/* pages compressed per round */
	unsigned int age_rounds;	/* rounds per pass */
	unsigned int age_batch;		/* inodes pinned per pass */
	struct list_head list;		/* on shmem_comp_mounts */
	atomic_long_t aged_pages;	/* pages compressed by the aging pass */
	atomic_long_t skipped_dead;	/* chilled pages freed without compression */
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
	unsigned int gc_debounce_ms;	/* demand-GC debounce (mount-overridable) */
	/*
	 * Writes that ran out of directory slots and were saved by the
	 * exhaustion GC pass.  A counter, not a gauge: it answers "how
	 * often would this write have failed before session 25", which
	 * is the number a headroom sweep needs.
	 */
	atomic64_t gc_recoveries;
	/*
	 * Reverse index (cell-backed pools only): one entry list per
	 * directory slot, so GC drains a victim by walking its own cells
	 * instead of the whole entry table.  cell_index is an array of
	 * cp->nslots list heads, allocated only when use_cells.  Every
	 * live cell-backed block node (an entry's embedded loc node, or
	 * a device table slot's) sits on cell_index[cellid] (from its
	 * handle's cellid) for its whole life -- alloc links, free
	 * unlinks, GC relocate moves.  A live node missing from its
	 * cell's list is invisible to GC and leaks the cell, so the three
	 * mutations are exhaustive by construction.
	 *
	 * index_lock is a SINGLE leaf spinlock guarding all list ops.  It
	 * is never held across an alloc, memcpy, cell_free(), or
	 * cell_relocate_one(); lock order is gc_lock -> cell rwsem ->
	 * index_lock, and the alloc/free paths take it with no other lock
	 * held.  See specs/04_GC_REVERSE_INDEX.md.
	 */
	struct list_head *cell_index;
	spinlock_t index_lock;
	struct list_head list;			/* global pools list */
	/*
	 * Demand-triggered cell GC (per pool: device, tmpfs mount, ...).
	 * Armed from the cell-object free path (loc_dispose); coalescing +
	 * debounced so a burst of frees merges into one evacuation pass.
	 * Replaces the fixed 1 Hz GC cadence.  Cancelled in pool_destroy().
	 */
	struct delayed_work cell_gc_work;
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
/* Aging/GC cadence defaults moved to <linux/shmem_compress.h>. */

/* gc_percent= mount option: clamped range and default */
#define SHMEM_COMP_GC_PERCENT_MIN	10U
#define SHMEM_COMP_GC_PERCENT_MAX	90U

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

	/* Per-CPU compression output buffer (see shmem_comp_compress_store) */
	pcpu->outbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!pcpu->outbuf) {
		acomp_request_free(pcpu->req);
		pcpu->req = NULL;
		crypto_free_acomp(pcpu->acomp);
		pcpu->acomp = NULL;
		return -ENOMEM;
	}

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
	if (pcpu->outbuf) {
		kfree(pcpu->outbuf);
		pcpu->outbuf = NULL;
	}

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
/*
 * shmem_comp_pool_cells_teardown - free the cell backend + reverse index
 * @pool: pool whose cell storage (if any) is being torn down
 *
 * Frees the reverse index, its lock's lists' backing array, the cell pool,
 * and its shrinker, in that order.  Idempotent on a pool that never had cell
 * storage (both fields NULL): the create() failure paths call this on the
 * zsmalloc branch too, where both are NULL and it is a no-op.  This is the
 * single teardown point for the cell backend so the create() failure paths
 * and pool_destroy() cannot drift apart.
 */
static void shmem_comp_pool_cells_teardown(struct shmem_comp_pool *pool)
{
	if (pool->cells && pool->cells->shrinker)
		shrinker_free(pool->cells->shrinker);
	kvfree(pool->cell_index);
	pool->cell_index = NULL;
	if (pool->cells)
		cell_pool_destroy(pool->cells);
	pool->cells = NULL;
	pool->use_cells = false;
}

struct shmem_comp_pool *shmem_comp_pool_create(const char *algo,
						unsigned int cell_order,
						unsigned int nr_cells,
						unsigned int gc_percent,
						unsigned int gc_debounce_ms)
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
	pool->gc_debounce_ms = gc_debounce_ms;

	/* Initialize refcount */
	atomic_set(&pool->refcount, 1);

	/* Per-pool demand-triggered cell GC (armed from loc_dispose). */
	INIT_DELAYED_WORK(&pool->cell_gc_work, shmem_comp_pool_gc_work);

	/* Cell storage: no zsmalloc pool needed */
	if (cell_order > 0) {
		pool->cells = cell_pool_create(cell_order, cell_order >= 4,
					       nr_cells, gc_percent);
		if (!pool->cells) {
			pr_warn("shmem_comp: failed to create cell pool\n");
			kfree(pool);
			return ERR_PTR(-ENOMEM);
		}
		pool->use_cells = true;
		pool->zs_pool = NULL;

		/*
		 * Reverse index: one entry list per directory slot, sized
		 * to the cell pool's nslots (known only now, after
		 * cell_pool_create).  Init every head and the single leaf
		 * lock that guards them.
		 */
		spin_lock_init(&pool->index_lock);
		pool->cell_index = kvcalloc(pool->cells->nslots,
					   sizeof(*pool->cell_index),
					   GFP_KERNEL);
		if (!pool->cell_index) {
			pr_warn("shmem_comp: cell_index alloc failed\n");
			cell_pool_destroy(pool->cells);
			kfree(pool);
			return ERR_PTR(-ENOMEM);
		}
		{
			u32 s;

			for (s = 0; s < pool->cells->nslots; s++)
				INIT_LIST_HEAD(&pool->cell_index[s]);
		}

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
		shmem_comp_pool_cells_teardown(pool);
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
			shmem_comp_pool_cells_teardown(pool);
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

	/* Stop any queued/running demand GC before teardown.  The armer
	 * (loc_dispose) only runs while the pool is alive, so after this
	 * no work can touch the pool. */
	cancel_delayed_work_sync(&pool->cell_gc_work);

	/* Free per-CPU contexts */
	if (pool->pcpu) {
		for_each_possible_cpu(cpu)
			shmem_comp_pcpu_free(per_cpu_ptr(pool->pcpu, cpu));
		free_percpu(pool->pcpu);
	}

	/* Destroy storage pool */
	shmem_comp_pool_cells_teardown(pool);
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
	int ret;

	if (!pool || !src || !dst || !dst_len || slen == 0)
		return -EINVAL;

	pcpu = this_cpu_ptr(pool->pcpu);
	dlen = *dst_len;

	mutex_lock(&pcpu->lock);

	/* Setup scatterlists for crypto operation */
	sg_init_one(&input, src, slen);
	sg_init_one(&output, dst, dlen);

	/* Configure compression request */
	acomp_request_set_params(pcpu->req, &input, &output,
				 slen, dlen);

	/* Execute compression (synchronous wait for async op) */
	ret = crypto_wait_req(crypto_acomp_compress(pcpu->req),
			      &pcpu->wait);

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
	int ret;

	if (!pool || !src || !dst || !dst_len || slen == 0)
		return -EINVAL;

	pcpu = this_cpu_ptr(pool->pcpu);
	dlen = *dst_len;

	mutex_lock(&pcpu->lock);

	/* Setup scatterlists for crypto operation */
	sg_init_one(&input, src, slen);
	sg_init_one(&output, dst, dlen);

	/* Configure decompression request */
	acomp_request_set_params(pcpu->req, &input, &output,
				 slen, dlen);

	/* Execute decompression */
	ret = crypto_wait_req(crypto_acomp_decompress(pcpu->req),
			      &pcpu->wait);

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
 * shmem_comp_compress_store - compress @src and store it, with no per-call
 *                              kmalloc
 * @pool: compression pool
 * @src: source data (at most PAGE_SIZE -- single-page compression)
 * @slen: source length in bytes
 * @clen_out: optional out; the STORED length (compressed size, or @slen
 *            when the block was stored raw because it did not compress)
 *
 * Compresses into the per-CPU @outbuf (held under pcpu->lock across both
 * the compress and the store, so the buffer cannot be reused by another
 * compress on this CPU mid-store).  Falls back to storing @src raw when
 * compression does not reduce size.  Returns the store handle, or 0 on
 * failure.
 *
 * Single-page only (slen <= PAGE_SIZE); a grouped/multi-page variant
 * would need a larger per-CPU buffer.
 */
u64 shmem_comp_compress_store(struct shmem_comp_pool *pool,
			      const void *src, unsigned int slen,
			      unsigned int *clen_out)
{
	struct shmem_comp_pcpu *pcpu;
	struct scatterlist input, output;
	const void *data;
	unsigned int clen;
	u64 handle;
	int ret;

	if (!pool || !src || slen == 0 || slen > PAGE_SIZE)
		return 0;

	pcpu = this_cpu_ptr(pool->pcpu);
	mutex_lock(&pcpu->lock);

	/* compress src -> per-CPU outbuf; capacity is slen (a compressed
	 * block never exceeds its input, else we store raw). */
	sg_init_one(&input, src, slen);
	sg_init_one(&output, pcpu->outbuf, slen);
	acomp_request_set_params(pcpu->req, &input, &output, slen, slen);
	ret = crypto_wait_req(crypto_acomp_compress(pcpu->req),
			      &pcpu->wait);

	if (!ret && pcpu->req->dlen < slen) {
		clen = pcpu->req->dlen;
		data = pcpu->outbuf;
	} else {
		if (ret) {
			spin_lock(&shmem_comp_stats_lock);
			shmem_comp_global_stats.compress_fails++;
			spin_unlock(&shmem_comp_stats_lock);
		}
		/* incompressible or crypto failure: store raw */
		clen = slen;
		data = src;
	}

	/* Store while still holding pcpu->lock so @outbuf stays valid for
	 * the store's memcpy.  (The cell path may sleep in alloc_pages;
	 * holding a mutex across it is fine.) */
	handle = shmem_comp_store(pool, data, clen);

	mutex_unlock(&pcpu->lock);

	if (clen_out)
		*clen_out = clen;
	return handle;
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
u64 shmem_comp_store(struct shmem_comp_pool *pool,
		     const void *data, unsigned int len)
{
	u64 handle;
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
		 * in process or kworker context (tmpfs compresses from
		 * write_end or the aging work).  gc_lock is a mutex for the
		 * same reason.  GC itself cannot recurse back here -- it
		 * relocates through cell_relocate_one(), which never
		 * re-enters this function.
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
	handle = shcomp_zs_malloc(pool->zs_pool, len, gfp);
	if (!handle)
		return 0;

	shcomp_zs_write(pool->zs_pool, (unsigned long)handle,
			data, len);

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
		    u64 handle, void *dst,
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

	src = shcomp_zs_read_begin(pool->zs_pool, (unsigned long)handle,
				 *len, bounce);
	if (!src) {
		kfree(bounce);
		return -EINVAL;
	}

	memcpy(dst, src, *len);

	shcomp_zs_read_end(pool->zs_pool, (unsigned long)handle, *len,
			  src);
	kfree(bounce);

	return 0;
}

int shmem_comp_store_merged(struct shmem_comp_pool *pool,
			    const void *data, unsigned int len,
			    u64 **handles_out,
			    unsigned short *nchunks_out)
{
	unsigned short nchunks, i;
	unsigned int offset;
	u64 *handles;

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
			   const u64 *handles,
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
			    u64 *handles,
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
void shmem_comp_free(struct shmem_comp_pool *pool, u64 handle)
{
	if (!pool || handle == 0)
		return;

	/* zs_free() is the kernel API: unsigned long.  zsmalloc
	 * handles originate there and always fit; the cast is a
	 * lossless round-trip, not a truncation. */
	zs_free(pool->zs_pool, (unsigned long)handle);
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
 * Entry identity is the POINTER, not an id+generation pair (the old
 * two-table {id, gen} xa_value encoding).  A reader loads the entry
 * pointer from the per-inode entries[] table under rcu_read_lock() and
 * pins it with atomic_inc_not_zero() in the SAME RCU section.
 * kfree_rcu defers the free past a grace period, so the object a
 * reader loaded cannot be reclaimed while its section is open: the pin
 * either succeeds on the original incarnation or fails on a dying
 * entry.  No ABA, no generation stamp, no width-limited packing --
 * identity and liveness both come from RCU plus the refcount.
 *
 * The slot is the CLAIM for the durable reference: whoever erases it
 * (xa_erase returns the old value under the xarray lock) owns the slot
 * ref and is the only thread that may drop it.  This replaces the
 * generation check on the release path -- the race it defended against
 * (a stale reader acting on a recycled id) cannot exist when the
 * reference travels as the pointer itself.
 */

/*
 * GC drain strategy (reverse index vs full entry-table scan).
 *
 * The reverse index (specs/04) makes a drain O(entries in the victim)
 * instead of O(total entries).  This knob exists for the A/B perf
 * measurement: 0 restores the old xa_for_each full scan so both drains
 * run on the same kernel and can be toggled at runtime via
 * /sys/module/shmem_compress/parameters/gc_use_index.  Default 1 (the
 * index); the full-scan path is kept only as the comparison baseline.
 */
static bool gc_use_index = true;
module_param(gc_use_index, bool, 0644);
MODULE_PARM_DESC(gc_use_index,
		 "cell GC drain uses the reverse index (default Y; N = full entry-table scan)");


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
/*
 * shmem_comp_loc_link - put a loc node on its cell's reverse-index list
 * @pool: pool that owns the node's storage
 * @node: node whose @handle already names a cell (storage allocated)
 *
 * Called once per block generation, after the storage exists and
 * before the frontend presents the block to readers.  From here until
 * dispose, GC can find and relocate the block.  The handle's cellid
 * is the list index; it is stable until GC relocates (gc_lock
 * serialises that against this link).
 */
void shmem_comp_loc_link(struct shmem_comp_pool *pool,
			 struct shmem_comp_loc_node *node)
{
	INIT_LIST_HEAD(&node->cell_node);
	if (pool->use_cells) {
		u32 cellid, offset;

		shmem_comp_unpack_loc(node->handle, &cellid, &offset);
		spin_lock(&pool->index_lock);
		list_add_tail(&node->cell_node, &pool->cell_index[cellid]);
		spin_unlock(&pool->index_lock);
	}
}

/*
 * Per-pool demand-triggered cell GC.
 *
 * Replaces the fixed 1 Hz GC cadence: the pool's cell evacuation runs
 * on demand, armed from the cell-object free path (loc_dispose -- the
 * unified choke point for both the device table slots and tmpfs
 * entries, so one worker covers device, tmpfs mount, and anything else
 * with a cell pool).  Coalescing + debounced: schedule_delayed_work()
 * is a no-op if already pending, so a burst of N frees within the
 * debounce window merges into ONE evacuation pass.  Leading-edge (fires
 * DEBOUNCE_MS after the FIRST free) so a sustained free stream cannot
 * starve GC.  Bounded latency (a few ms) beats the OOM killer, which
 * fires only after reclaim has already failed -- the race the fixed
 * 1 Hz cadence and the shrinker both lost under a tight-RAM OOM
 * scenario (zcell failed to re-extend RAM on a second alloc wave).
 *
 * Runs in kworker context (process; GFP_KERNEL in cell_relocate_one is
 * honest here, unlike a shrinker -- which is why the shrinker only
 * arms work and never GCs under fs_reclaim).  Lifetime: pool_destroy()
 * calls cancel_delayed_work_sync(), and the armer only runs while the
 * pool is alive (a live node is being disposed), so no work can run on
 * a freed pool.
 */
static void shmem_comp_pool_gc_work(struct work_struct *work)
{
	struct shmem_comp_pool *pool;

	pool = container_of(to_delayed_work(work), struct shmem_comp_pool,
			    cell_gc_work);
	if (!pool->cells)
		return;
	mutex_lock(&pool->cells->gc_lock);
	shmem_comp_cell_gc(pool);
	mutex_unlock(&pool->cells->gc_lock);
}

/* Arm (or coalesce onto) the pool's cell GC.  Atomic-safe: only queues
 * work.  Called from loc_dispose on every cell-object free. */
static void shmem_comp_pool_arm_gc(struct shmem_comp_pool *pool)
{
	if (!pool || !pool->use_cells)
		return;
	schedule_delayed_work(&pool->cell_gc_work,
			      msecs_to_jiffies(pool->gc_debounce_ms));
}

/*
 * shmem_comp_loc_dispose - unlink a dead node and release its storage
 * @pool: pool that owns the storage
 * @node: node at refs==0; the caller owns the last reference
 *
 * Two releases, unlink first:
 *   - Reverse index: a stale node left on cell_index[cellid] would
 *     point into a cell that cell_free() may return to the buddy
 *     synchronously and that a later alloc may reuse for different
 *     data; the next GC walk would touch freed memory.  Only the
 *     refs==0 thread runs here (dec_and_test), so no other thread
 *     can be unlinking this node concurrently.  A node GC currently
 *     holds detached (list_del_init) is a self-linked singleton;
 *     list_del on it is harmless (the frontend re-links via
 *     loc_link on the node's next generation).
 *   - Storage: cell storage needs the block length to decrement
 *     live_bytes, which is why cell_free() is called directly rather
 *     than through shmem_comp_free().  The zsmalloc free path does
 *     not need the length -- zs_free() recovers it from the size
 *     class encoded in the handle.
 *
 * Does NOT touch node->handle: an entry is dying and gets freed
 * whole; a table slot clears its handle itself (marking itself
 * empty).
 */
void shmem_comp_loc_dispose(struct shmem_comp_pool *pool,
			    struct shmem_comp_loc_node *node)
{
	if (pool->use_cells) {
		spin_lock(&pool->index_lock);
		list_del(&node->cell_node);
		spin_unlock(&pool->index_lock);
	}

	if (node->handle) {
		if (pool->use_cells) {
			u32 cellid;
			u32 offset;

			shmem_comp_unpack_loc(node->handle,
					      &cellid, &offset);
			cell_free(pool->cells, cellid, offset,
				  node->length);
			/* A cell-object just went free: this may have made a
			 * cell sparse.  Arm the pool's demand GC (coalescing +
			 * debounced) so the partial cell is evacuated and
			 * returned to the buddy promptly, without waiting for
			 * the shrinker/OOM race.  Both the device and tmpfs
			 * free paths funnel through here, so one armer covers
			 * all cell pools. */
			shmem_comp_pool_arm_gc(pool);
		} else {
			shmem_comp_free(pool, node->handle);
		}
	}
}

/*
 * shmem_comp_loc_pin_put - drop a pin taken on a loc node
 * @pool: pool the node belongs to
 * @node: node the caller pinned (atomic_inc_not_zero on loc.refs)
 *
 * The refs==0 teardown runs the entry free path: an entry pool's
 * reverse index holds entries' embedded loc nodes, so the node always
 * belongs to an entry.
 */
void shmem_comp_loc_pin_put(struct shmem_comp_pool *pool,
			    struct shmem_comp_loc_node *node)
{
	shmem_comp_entry_put(container_of(node, struct shmem_comp_entry,
					  loc));
}

static void shmem_comp_entry_free(struct shmem_comp_entry *ent)
{
	/* xa_erase() takes the xarray lock internally.  The guard is the
	 * double-teardown detector: an id is in the table for exactly the
	 * lifetime of its entry, so a failed erase means this entry was
	 * already torn down. */
	/* Global-table entries: the erase is the teardown guard (an id is
	 * in the table for exactly the entry's lifetime; a failed erase =
	 * already torn down).  Device-local entries (ent->id == 0, via
	 * entry_alloc_local) are never in the global table, so skip the
	 * erase; the caller's atomic dec_and_test is the sole double-
	 * teardown guard for them (only one thread observes refs hit 0). */
	if (ent->id) {
		if (!xa_erase(&shmem_comp_entries, ent->id))
			return;
	}

	/* Teardown runs only from the refs==0 path (dec_and_test); a
	 * live entry here means double teardown. Debug kernels panic. */
	VM_BUG_ON(atomic_read(&ent->loc.refs) != 0);

	/* Un-account live RAM before freeing (ksize needs a live
	 * object; do it before kfree/kfree_rcu). */
	atomic_dec(&shmem_comp_ram_entries);
	atomic_long_sub(ksize(ent), &shmem_comp_ram_entry_bytes);
	if (ent->handles)
		atomic_long_sub(ksize(ent->handles),
				 &shmem_comp_ram_merged_bytes);

	/*
	 * Reverse index unlink + storage release, in that order (the
	 * rationale lives on shmem_comp_loc_dispose).  Zsmalloc entries
	 * were never linked; the use_cells guard skips the lock for them.
	 */
	shmem_comp_loc_dispose(ent->pool, &ent->loc);

	if (ent->handles)
		shmem_comp_free_merged(ent->pool, ent->handles,
				       ent->nchunks);
	kfree_rcu(ent, rcu);
}

/*
 * shmem_comp_entries_get - resolve entries[index] to a pinned entry
 * @inode: shmem inode
 * @index: page index
 * @handle, @length, @nr_pages, @handles, @nchunks: optional field
 *         copies out (valid while the returned pin is held)
 *
 * Return: pinned entry, or NULL when there is no compressed backing
 * for @index, or the entry is already dying (both mean "entry
 * vanished"; the caller retries against the updated tables).
 *
 * The ONE sanctioned way from a page-cache slot to a live entry: the
 * pointer load and the inc_not_zero pin happen inside a single
 * rcu_read_lock section, which is what makes the pointer an identity
 * (see the note above the entry table).  The caller drops the pin with
 * shmem_comp_entry_put() after its last use.
 */
static struct shmem_comp_entry *
shmem_comp_entries_get(struct inode *inode, pgoff_t index, u64 *handle,
		       unsigned int *length, unsigned int *nr_pages,
		       u64 **handles, unsigned short *nchunks)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa = READ_ONCE(info->comp_entries);
	struct shmem_comp_entry *ent;

	if (!xa)
		return NULL;
	rcu_read_lock();
	ent = xa_load(xa, index);
	if (ent && !atomic_inc_not_zero(&ent->loc.refs))
		ent = NULL;
	rcu_read_unlock();

	if (ent) {
		if (handle)
			*handle = ent->loc.handle;
		if (length)
			*length = ent->loc.length;
		if (nr_pages)
			*nr_pages = ent->nr_pages;
		if (handles)
			*handles = ent->handles;
		if (nchunks)
			*nchunks = ent->nchunks;
	}
	return ent;
}

/*
 * shmem_comp_entries_names - does entries[index] still name @ent?
 * @ent: an entry the caller holds a pin on
 *
 * The install-side revalidation for decompress: the pin from
 * entries_get guarantees the ENTRY's lifetime, but says nothing about
 * the SLOT -- a truncate/punch may have claimed (xa_erased) it while
 * the caller decompressed.  Installing into the i_pages slot that
 * truncate left empty would resurrect a folio past i_size or inside a
 * hole, where tmpfs must serve zeros.  The pointer comparison happens
 * inside one rcu_read_lock section; the caller's own pin keeps the
 * object (and its address) alive, so no ABA is possible.
 */
static bool shmem_comp_entries_names(struct inode *inode, pgoff_t index,
				     struct shmem_comp_entry *ent)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa = READ_ONCE(info->comp_entries);
	bool named;

	if (!xa)
		return false;
	rcu_read_lock();
	named = xa_load(xa, index) == ent;
	rcu_read_unlock();
	return named;
}

void shmem_comp_entry_put(struct shmem_comp_entry *ent)
{
	if (!ent)
		return;
	if (!atomic_dec_and_test(&ent->loc.refs)) {
		if (unlikely(atomic_read(&ent->loc.refs) < 0))
			panic("shmem_comp: entry %u refs underflow -- "
			      "reference accounting corrupted", ent->id);
		return;
	}
	shmem_comp_entry_free(ent);
}

bool shmem_comp_entry_release(struct shmem_comp_entry *ent)
{
	bool last_slot;

	/* The caller erased the entries[] slot (xa_erase returns the old
	 * value atomically), so it owns the slot's durable reference:
	 * this drop needs no validation, and the entry is kept alive
	 * across the call by the very reference being dropped.  The old
	 * generation check lived here because an id could have been
	 * recycled between lookup and release; a pointer cannot be
	 * recycled while we hold the erase that named it. */
	if (!ent)
		return false;
	last_slot = atomic_dec_and_test(&ent->slots);
	if (unlikely(atomic_read(&ent->slots) < 0))
		panic("shmem_comp: entry %u slots underflow -- "
		      "page cache slot released twice", ent->id);
	if (!atomic_dec_and_test(&ent->loc.refs)) {
		if (unlikely(atomic_read(&ent->loc.refs) < 0))
			panic("shmem_comp: entry %u refs underflow -- "
			      "reference accounting corrupted", ent->id);
		return last_slot;
	}

	shmem_comp_entry_free(ent);
	return last_slot;
}

struct shmem_comp_entry *
shmem_comp_entry_alloc(struct shmem_comp_pool *pool, u64 handle,
		       unsigned int length, unsigned short nr_pages,
		       u64 *handles, unsigned short nchunks)
{
	struct shmem_comp_entry *ent;
	unsigned int id;
	int ret;

	ent = kmalloc(sizeof(*ent), GFP_KERNEL);
	if (!ent)
		return 0;

	ent->loc.handle = handle;
	ent->loc.length = length;
	ent->nr_pages = nr_pages;
	ent->handles = handles;
	ent->nchunks = nchunks;
	ent->pool = pool;
	atomic_set(&ent->loc.refs, nr_pages);
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
		return NULL;
	}
	ent->id = id;

	/* Live RAM footprint: slab-rounded entry + handle array size */
	atomic_inc(&shmem_comp_ram_entries);
	atomic_long_add(ksize(ent), &shmem_comp_ram_entry_bytes);
	if (handles)
		atomic_long_add(ksize(handles),
				 &shmem_comp_ram_merged_bytes);

	/*
	 * Reverse index: link onto the owning cell's list so GC can find
	 * this block without scanning the whole entry table.  Only cell-
	 * backed pools index; zsmalloc entries leave cell_node unlinked
	 * (their compaction is internal to zsmalloc).  Runs after xa_alloc
	 * so the entry is reachable by id, before return so the invariant
	 * "every live cell entry is on its cell's list" holds from the
	 * moment the entry can be seen.  The handle's cellid is the list
	 * index; it is stable for the entry's life here (only GC relocates,
	 * and gc_lock serialises that against this alloc).
	 */
	shmem_comp_loc_link(pool, &ent->loc);

	return ent;
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
/*
 * shmem_comp_drain_victim_scan - the pre-index full-table drain
 * @pool: cell-backed compression pool
 * @vic: victim cell, held for write by the caller
 *
 * The original drain: walk the WHOLE entry table to find the entries whose
 * handle names @vic.  O(total entries) per victim.  Kept ONLY as the A/B
 * baseline for gc_use_index=0 (specs/04 Phase 4); the reverse-index drain
 * is the real path.  The relocate step moves the reverse-index node too, so
 * the index stays correct even while this scan path is selected.
 */
static unsigned int shmem_comp_drain_victim_scan(struct shmem_comp_pool *pool,
						 struct cell *vic)
{
	struct cell_pool *cp = pool->cells;
	unsigned int moved = 0;
	unsigned long id;
	struct shmem_comp_entry *ent;

	xa_for_each(&shmem_comp_entries, id, ent) {
		u64 h;
		u32 cellid, offset;
		unsigned int len;
		u32 nc, no;
		int ret;

		if (ent->pool != pool)
			continue;
		h = READ_ONCE(ent->loc.handle);
		if (!h)
			continue;
		shmem_comp_unpack_loc(h, &cellid, &offset);
		if (cellid != vic->cellid)
			continue;
		if (!atomic_inc_not_zero(&ent->loc.refs))
			continue;
		len = ent->loc.length;
		if (offset + len > (1UL << cp->cell_order) * PAGE_SIZE) {
			shmem_comp_entry_put(ent);
			continue;
		}
		ret = cell_relocate_one(cp, vic, vic->base + offset, len,
					&nc, &no);
		if (ret == 0) {
			WRITE_ONCE(ent->loc.handle,
				   shmem_comp_pack_loc(nc, no));
			if (pool->use_cells) {
				spin_lock(&pool->index_lock);
				list_move_tail(&ent->loc.cell_node,
					       &pool->cell_index[nc]);
				spin_unlock(&pool->index_lock);
			}
			moved++;
		}
		shmem_comp_entry_put(ent);
	}
	return moved;
}

static unsigned int shmem_comp_drain_victim(struct shmem_comp_pool *pool,
					    struct cell *vic)
{
	struct cell_pool *cp = pool->cells;
	unsigned int moved = 0;

	/* A/B baseline: the old full-table scan, when gc_use_index=0. */
	if (!gc_use_index)
		return shmem_comp_drain_victim_scan(pool, vic);

	/*
	 * Reverse-index drain: walk pool->cell_index[vic->cellid], not the
	 * whole entry table.  This is O(entries in the victim), not
	 * O(total entries) -- the point of the index (specs/04).
	 *
	 * Per iteration we detach ONE live entry, relocate it outside the
	 * lock, then either move it to the destination list (success) or
	 * put it back and stop (failure).  We cannot "put back and
	 * continue" like the old xa_for_each did: re-adding to the head
	 * list would re-find the same entry next scan and spin.  So a
	 * failure stops the drain; the entry stays reachable on the
	 * victim's list for a later pass, and cell_gc's moved==0 stop
	 * signal bounds the non-progress case.
	 */
	for (;;) {
		struct shmem_comp_loc_node *node = NULL, *iter;
		u64 h;
		u32 cellid, offset;
		unsigned int len;
		u32 nc, no;
		int ret;

		/*
		 * Detach the first LIVE node on the victim's list.  Under
		 * index_lock so a concurrent dispose's unlink (also under
		 * index_lock) cannot yank a node out from under the walk.
		 * A dying node (pin fails) is left on the list: its own
		 * refs==0 teardown unlinks it and releases the storage.
		 */
		spin_lock(&pool->index_lock);
		list_for_each_entry(iter, &pool->cell_index[vic->cellid],
				    cell_node) {
			if (atomic_inc_not_zero(&iter->refs)) {
				list_del_init(&iter->cell_node);
				node = iter;
				break;
			}
		}
		spin_unlock(&pool->index_lock);
		if (!node)
			break;	/* no live blocks remain in this victim */

		/*
		 * Pinned (refs >= 1) and detached: the block cannot be
		 * freed and its handle cannot change under us (only GC
		 * relocates, serialised by gc_lock).  The pin -- not the
		 * index_lock -- is what keeps vic->base valid across the
		 * memcpy below.
		 */
		h = READ_ONCE(node->handle);
		shmem_comp_unpack_loc(h, &cellid, &offset);

		/* The reverse index is keyed by the handle's cellid, so a
		 * live node on this list must name this cell.  A violation
		 * means the alloc/free/reloc linkage is broken; catch it
		 * loudly rather than read the wrong cell's memory. */
		VM_BUG_ON(cellid != vic->cellid);

		len = node->length;
		if (offset + len > (1UL << cp->cell_order) * PAGE_SIZE) {
			/* Defensive: a live block should never be too big for
			 * its own cell (would mean corrupt accounting).  Put it
			 * back and stop -- cannot loop-and-continue. */
			spin_lock(&pool->index_lock);
			list_add(&node->cell_node,
				 &pool->cell_index[vic->cellid]);
			spin_unlock(&pool->index_lock);
			shmem_comp_loc_pin_put(pool, node);
			break;
		}

		ret = cell_relocate_one(cp, vic, vic->base + offset, len,
					&nc, &no);
		if (ret) {
			/* Relocate failed (directory full / ENOMEM).  Put the
			 * node back on the victim's list so it is not stranded
			 * off-index, drop the pin, and stop. */
			spin_lock(&pool->index_lock);
			list_add(&node->cell_node,
				 &pool->cell_index[vic->cellid]);
			spin_unlock(&pool->index_lock);
			shmem_comp_loc_pin_put(pool, node);
			break;
		}

		WRITE_ONCE(node->handle, shmem_comp_pack_loc(nc, no));
		/* Relink onto the destination cell's list: the handle now
		 * names nc, so the "list == handle's cellid" invariant
		 * requires the move. */
		spin_lock(&pool->index_lock);
		list_add_tail(&node->cell_node, &pool->cell_index[nc]);
		spin_unlock(&pool->index_lock);
		moved++;

		/* Drop our pin; may hit refs==0 -> the refs==0 teardown
		 * unlinks from the DESTINATION list (handle already
		 * updated), decrementing nc's live_bytes -- correct. */
		shmem_comp_loc_pin_put(pool, node);
	}
	return moved;
}

static void shmem_comp_cell_gc(struct shmem_comp_pool *pool)
{
	struct cell_pool *cp = pool->cells;
	unsigned int drained = 0, moved_total = 0;
	unsigned int i;

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
 * shmem_comp_loc_load - relocation-safe block load on a pinned loc node
 * @pool: pool that owns the node's storage
 * @node: loc node, pinned by the caller (refs >= 1 held)
 * @dst: destination buffer
 * @len: [in,out] buffer size in, data size out
 *
 * Return: 0 on success, -ENOENT if @node is NULL, -EINVAL if the
 * pool is not cell-backed, -EIO if the block could not be located
 * within the retry budget (unreachable unless the node table and
 * cell accounting disagree).
 *
 * The reader's side of the GC contract.  A plain load could memcpy
 * from a cell that GC relocated and returned to the buddy between the
 * caller's pin and the copy.  This helper re-reads the node handle
 * under the cell's read lock and retries if GC moved the block -- the
 * same retry shape the page-cache install path uses.  The caller's
 * pin keeps the block (and its accounting) alive across the retry;
 * nothing here re-resolves an id, so nothing here can go stale.
 */
static int shmem_comp_loc_load(struct shmem_comp_pool *pool,
			       struct shmem_comp_loc_node *node,
			       void *dst, unsigned int *len)
{
	struct cell_pool *cp;
	unsigned int budget = 8;
	int ret = -EIO;

	if (!node)
		return -ENOENT;
	cp = pool->cells;
	if (!cp)
		return -EINVAL;

	while (budget--) {
		u64 h = READ_ONCE(node->handle);
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
		 * here means the block moved -- retry with the fresh
		 * location.  Slot reuse cannot fool this check: the
		 * handle would differ, and an unchanged handle implies
		 * the original cell is still live (its last object is
		 * this one, pinned by us).
		 */
		if (READ_ONCE(node->handle) != h) {
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

	return ret;
}

int shmem_comp_load_entry(struct shmem_comp_entry *ent, void *dst,
			  unsigned int *len)
{
	if (!ent)
		return -ENOENT;
	return shmem_comp_loc_load(ent->pool, &ent->loc, dst, len);
}

int shmem_comp_loc_read(struct shmem_comp_pool *pool,
			struct shmem_comp_loc_node *node, void *dst,
			bool *raw)
{
	struct cell_pool *cp;
	unsigned int budget = 8;
	int ret = -EIO;

	if (!node)
		return -ENOENT;
	cp = pool->cells;
	if (!cp)
		return -EINVAL;

	while (budget--) {
		u64 h = READ_ONCE(node->handle);
		u32 cellid, offset, dlen;
		struct cell *c;
		u32 cell_size;

		if (!h)
			break;
		shmem_comp_unpack_loc(h, &cellid, &offset);
		c = &cp->cells[cellid];

		down_read(&c->lock);
		/* Re-read under the lock: GC relocates the handle while holding
		 * down_write on this rwsem, so a change means the block moved --
		 * retry with the fresh location. */
		if (READ_ONCE(node->handle) != h) {
			up_read(&c->lock);
			continue;
		}
		if (!c->pages[0] || !c->base) {
			up_read(&c->lock);
			continue;
		}

		dlen = node->length;
		cell_size = (1UL << cp->cell_order) * PAGE_SIZE;
		if (offset + dlen > cell_size) {
			up_read(&c->lock);
			break;
		}

		if (dlen == PAGE_SIZE) {
			memcpy(dst, c->base + offset, PAGE_SIZE);
			if (raw)
				*raw = true;
			ret = 0;
		} else {
			unsigned int out = PAGE_SIZE;

			ret = shmem_comp_decompress(pool,
						    c->base + offset, dlen,
						    dst, &out);
			if (raw)
				*raw = false;
		}
		up_read(&c->lock);
		break;
	}

	return ret;
}

/* Debug hook: run one GC pass on every cell-backed pool. */
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
			pool_pages += cell_get_total_pages(pool->cells);
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
			"pool %d: order=%u use_thp=%d nr_cells=%u pages_used=%ld gc_percent=%u\n",
			pool_id, cp->cell_order, cp->use_thp,
			cp->nr_cells, atomic_long_read(&cp->pages_used),
			cp->gc_percent);

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

/*
 * shmem_comp_opts_set_gc_percent - Parse the gc_percent= mount option
 * @opts: options to update
 * @val: byte-utilization percent (decimal), clamped 10..90
 *
 * Return: 0 on success, -EINVAL on a bad value (mount fails).
 * Default (not given) is CELL_IS_SPARSE_PERCENT_DEFAULT (50), the
 * historical threshold.  The _seen flag lets fill_sb() pass 0 (pool
 * default) only when the mount did not ask.
 */
int shmem_comp_opts_set_gc_percent(struct shmem_comp_opts *opts,
				   const char *val)
{
	unsigned int pct;
	char *rest;

	pct = simple_strtoul(val, &rest, 10);
	if (*rest || pct < SHMEM_COMP_GC_PERCENT_MIN ||
	    pct > SHMEM_COMP_GC_PERCENT_MAX)
		return -EINVAL;
	opts->gc_percent = pct;
	opts->gc_percent_seen = true;
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
	unsigned int nr_cells;

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
	 * The pool's directory is sized from size= below, so the
	 * cell-backed pool can hold the compressed image of the whole
	 * mount (the legacy 256-cell default capped storage at
	 * 256 x cell_size and starved large fills).
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

	/*
	 * Directory capacity from the filesystem size: a bigger mount
	 * gets a bigger cell array, so the pool can hold the compressed
	 * image of the whole tmpfs.  size=/cell_size gives ~2x headroom
	 * over the ~2:1 compressed image and covers COW churn (the store
	 * path allocates before the free path drops the old block, and a
	 * freed block only returns its cell when the cell empties).  Bound
	 * it to keep the directory metadata sane: floor at the legacy
	 * default, cap at SHMEM_COMP_CELL_MAX_CELLS.
	 */
	{
		unsigned long n = tmpfs_bytes / (PAGE_SIZE << cell_order);

		if (n < CELL_DEFAULT_MAX_CELLS)
			n = CELL_DEFAULT_MAX_CELLS;
		if (n > SHMEM_COMP_CELL_MAX_CELLS)
			n = SHMEM_COMP_CELL_MAX_CELLS;
		nr_cells = (unsigned int)n;
	}

	comp->pool = shmem_comp_pool_create(algo, cell_order, nr_cells,
					   opts->gc_percent,
					   opts->gc_debounce_seen ?
					   opts->gc_debounce_ms :
					   SHMEM_COMP_CELL_GC_DEBOUNCE_MS);
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
	comp->gc_percent = opts->gc_percent;
	comp->cell_order = cell_order;
	comp->merge_pages = opts->merge_pages ?:
		      (shmem_comp_default_merge_get() ?: 1);
	comp->age_delay_ms = opts->age_delay_seen ? opts->age_delay_ms :
			     SHMEM_COMP_AGE_DELAY_MS;
	comp->age_budget = opts->age_budget_seen ? opts->age_budget :
			   SHMEM_COMP_AGE_BUDGET;
	comp->age_rounds = opts->age_rounds_seen ? opts->age_rounds :
			   SHMEM_COMP_AGE_MAX_ROUNDS;
	comp->age_batch = opts->age_batch_seen ? opts->age_batch :
			  SHMEM_COMP_AGE_BATCH_INODES;
	atomic_long_set(&comp->pages, 0);
	atomic_long_set(&comp->bytes, 0);
	/* Stage 2: deferred-compression policy state */
	comp->sb = sb;
	/*
	 * Initial mode: eager (0:0) mounts start hot so inline behavior
	 * is unchanged. Any positive HIGH watermark starts chill (RAM
	 * usage at mount is 0) -- the write path flips it when pressure
	 * arrives (shmem_comp_wm_update).
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
		"cell_order=%u watermark=%u:%u gc_percent=%u "
		"age_delay=%ums age_budget=%u age_rounds=%u age_batch=%u\n",
		comp->compressor, comp->merge_pages, comp->threshold_bytes,
		comp->cell_order, comp->watermark_low, comp->watermark_high,
		comp->gc_percent, comp->age_delay_ms, comp->age_budget,
		comp->age_rounds, comp->age_batch);
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

/*
 * shmem_comp_ram_usage_pct - Percent of RAM that is NOT available
 *
 * The watermark signal (specs/06): compression buys RAM, so RAM
 * pressure is the honest trigger -- mount usage was a proxy that
 * inverts in the overcommit/exact-fit regimes.  si_mem_available()
 * accounts free pages plus evictable file cache, the same number
 * sysinfo(2) reports as "available".
 */
static unsigned int shmem_comp_ram_usage_pct(void)
{
	long avail = si_mem_available();
	long total = (long)totalram_pages();

	if (total <= 0)
		return 0;
	if (avail < 0)
		avail = 0;
	return (unsigned int)clamp((total - avail) * 100 / total, 0L, 100L);
}

/*
 * shmem_comp_wm_update - Evaluate the hot/chill flip against RAM usage
 * @comp: the per-mount compression state
 *
 * Called from shmem_comp_should_compress() on the write path, so the
 * flip is evaluated where the decision takes effect: at exact-fit a
 * 1 Hz poll loses the race to sub-second RAM exhaustion (the zcell
 * triangle: every non-eager combo OOM'd with the flip arriving at
 * 92% -- the deathbed).  Hysteresis unchanged: >= HIGH -> hot,
 * <= LOW -> chill, band holds.  The first chill->hot transition arms
 * the maintenance pass for an immediate backlog drain (the one-shot
 * highwater kick; a return to chill resets it).
 */
static void shmem_comp_wm_update(struct shmem_comp_sb *comp)
{
	unsigned int usage;
	bool hot = READ_ONCE(comp->hot);

	/* eager sentinel: 0:0 is permanently hot, never flinches */
	if (comp->watermark_high == 0) {
		if (!hot)
			WRITE_ONCE(comp->hot, true);
		return;
	}
	usage = shmem_comp_ram_usage_pct();
	if (usage >= comp->watermark_high)
		hot = true;
	else if (usage <= comp->watermark_low)
		hot = false;
	else
		return;	/* hysteresis band: hold */

	if (hot == READ_ONCE(comp->hot))
		return;
	WRITE_ONCE(comp->hot, hot);
	pr_info("tmpfs: mount %s compression %s (ram usage %u%%)\n",
		comp->sb->s_id, hot ? "hot" : "chill", usage);
	if (hot) {
		/* highwater kick: run a backlog pass NOW (mod_delayed_work
		 * re-arms a pending cadence timer to 0).  Once per flip:
		 * wm_kick_spent is reset by the return to chill. */
		if (!comp->wm_kick_spent) {
			comp->wm_kick_spent = true;
			mod_delayed_work(system_wq, &comp->age_work, 0);
		}
	} else {
		comp->wm_kick_spent = false;
	}
}

bool shmem_comp_should_compress(struct inode *inode)
{
	struct shmem_comp_sb *comp = shmem_comp_get_sb(inode->i_sb)->comp;

	/*
	 * Write-path decision (specs/06 phase 1): evaluate the
	 * hot/chill flip against RAM usage here, then read the cached
	 * mode.  In chill mode writes land uncompressed and the
	 * maintenance pass ages them later (death-before-pop, now
	 * age-gated in calm mode).  Eager mounts (0:0) keep hot=true
	 * always.
	 */
	if (!comp || !comp->enabled || !comp->pool)
		return false;
	shmem_comp_wm_update(comp);
	return READ_ONCE(comp->hot) &&
		i_size_read(inode) >= comp->threshold_bytes;
}

/*
 * shmem_comp_arm_aging - REMOVED (specs/06)
 *
 * The write path no longer arms the maintenance pass on every
 * uncompressed write: the pass is armed by the 1 Hz cadence (while a
 * backlog exists) and kicked immediately at the highwater flip
 * (shmem_comp_wm_update).  A young chilled page is protected by the
 * calm-mode age gate instead of by deferral.
 */

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
	if (comp->pool && comp->pool->use_cells)
		seq_printf(seq, ",gc_percent=%u",
			   comp->gc_percent ?:
			   CELL_IS_SPARSE_PERCENT_DEFAULT);
	if (comp->merge_pages > 1)
		seq_printf(seq, ",merge_pages=%u", comp->merge_pages);
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
	 * against put_super's teardown.  hot=true: an explicit gc_now is
	 * an immediate-relief request, so it bypasses the calm-mode age
	 * gate (specs/06).
	 */
	mutex_lock(&shmem_comp_mounts_lock);
	list_for_each_entry(comp, &shmem_comp_mounts, list)
		shmem_comp_maintain(comp, true);
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
				   struct shmem_comp_entry *ent,
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
 * per slice. The entry pointer is replicated in every slot of the
 * group; the group holds one durable slot ref per slice, and the
 * entry plus its storage are freed with the last release.
 *
 * The folio covering @index is returned locked (shmem_get_folio()
 * contract); sibling slices remain cached, so sequential reads of a
 * group cost a single decompression.
 *
 * NOTE: the comment this replaced claimed per-slot install races were
 * resolved by shmem_add_to_page_cache() validating the replicated
 * ID+generation -- that check never existed (add_to_page_cache does
 * not look at info->comp_entries).  The real races are resolved by
 * the -EEXIST retry in shmem.c (folio filled the slot), by the
 * entries_get pin below (entry vanished -> -ENOENT, retry), and --
 * for the truncate/punch race the old comment gestured at -- by the
 * post-install slot revalidation in the finalize pass below.
 */
static int shmem_decompress_group(struct inode *inode, pgoff_t index,
				   struct shmem_comp_entry *ent,
				   unsigned int comp_len,
				   unsigned short nr_pages,
				   gfp_t gfp, struct folio **foliop)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	pgoff_t head = index - (index % nr_pages);
	u64 *handles = NULL;
	unsigned short nchunks = 0;
	void *cbuf, *dbuf, *dst;
	struct folio **sibs = NULL;
	unsigned int dlen;
	unsigned short slice;
	int ret = 0;

	*foliop = NULL;

	/*
	 * @ent arrives PINNED from the caller (entries_get in
	 * shmem_decompress_folio); this function owns that pin and
	 * drops it on every path.  The pin keeps the entry -- and its
	 * handle array -- alive for the whole group decompress.
	 */
	handles = ent->handles;
	nchunks = ent->nchunks;

	/*
	 * Load the compressed data.  Cell storage uses the pinned,
	 * retry-safe loader: GC may relocate the block between the
	 * caller's pin and this copy, and a stale handle would memcpy
	 * from a cell already returned to the buddy.  Zsmalloc loads
	 * chunk by chunk through the handle array.
	 */
	if (shmem_comp_pool_uses_cells(sbinfo->comp->pool)) {
		cbuf = kmalloc(comp_len, GFP_KERNEL);
		if (!cbuf) {
			shmem_comp_entry_put(ent);
			return -ENOMEM;
		}
		ret = shmem_comp_load_entry(ent, cbuf, &comp_len);
		if (ret) {
			kfree(cbuf);
			shmem_comp_entry_put(ent);
			return ret;
		}
	} else {
		if (!handles) {
			shmem_comp_entry_put(ent);
			return -ENOENT;
		}

		cbuf = kmalloc(comp_len, GFP_KERNEL);
		if (!cbuf) {
			shmem_comp_entry_put(ent);
			return -ENOMEM;
		}

		ret = shmem_comp_load_merged(sbinfo->comp->pool, handles,
					      nchunks, cbuf, &comp_len);
		if (ret) {
			kfree(cbuf);
			shmem_comp_entry_put(ent);
			return ret;
		}
	}

	dbuf = kmalloc((size_t)nr_pages * PAGE_SIZE, GFP_KERNEL);
	/* sibs tracks the installed slices so the finalize pass can
	 * validate/unlock them; heap because nr_pages reaches
	 * SHMEM_COMP_MERGE_MAX (1024). */
	sibs = kcalloc(nr_pages, sizeof(*sibs), GFP_KERNEL);
	if (!dbuf || !sibs) {
		kfree(cbuf);
		kfree(dbuf);
		kfree(sibs);
		shmem_comp_entry_put(ent);
		return -ENOMEM;
	}

	dlen = (unsigned int)nr_pages * PAGE_SIZE;
	ret = shmem_comp_decompress(sbinfo->comp->pool, cbuf, comp_len,
				    dbuf, &dlen);
	if (ret) {
		kfree(cbuf);
		kfree(dbuf);
		shmem_comp_entry_put(ent);
		return ret;
	}

	/* Data corruption: a successful group decompress must produce
	 * exactly nr_pages pages. See the single-page check. */
	if (unlikely(dlen != (unsigned int)nr_pages * PAGE_SIZE))
		panic("shmem_comp: group decompressed %u bytes, expected "
		      "%u -- compressed data corrupt (id=%u)",
		      dlen, (unsigned int)((unsigned int)nr_pages * PAGE_SIZE),
		      ent->id);

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
		/* memcg-charge before cache/LRU insertion -- see the
		 * long comment in shmem_decompress_folio(). */
		if (mem_cgroup_charge(sib, NULL, gfp)) {
			folio_put(sib);
			ret = -ENOMEM;
			break;
		}
		__folio_set_locked(sib);
		__folio_set_swapbacked(sib);

		dst = kmap_local_folio(sib, 0);
		memcpy(dst, dbuf + (size_t)slice * PAGE_SIZE, PAGE_SIZE);
		kunmap_local(dst);
		folio_mark_uptodate(sib);

		ret = shmem_add_to_page_cache(sib, mapping, sidx, NULL, gfp);
		if (ret) {
			folio_unlock(sib);
			folio_put(sib);
			if (sidx == index) {
				/* requested slice lost the race: retry.
				 * Break (not return) so the finalize pass
				 * below still unlocks the slices already
				 * installed. */
				break;
			}
			continue;
		}

		/* A decompression cache is a file page, not swappable anon
		 * memory (see shmem_decompress_folio()). */
		__folio_clear_swapbacked(sib);
		folio_add_lru(sib);

		/* Both state (validated in the finalize pass below):
		 * entries[sidx] still names the group entry, so no
		 * per-slice release and the entry's swapped charge is
		 * retained.  Reclaim drops the folio later, keeping the
		 * entry.  Not marked referenced -- a reclaimable cache,
		 * see shmem_decompress_folio(). */

		/* Slices stay LOCKED until the whole group is validated
		 * below; sibs[] tracks the installed ones. */
		sibs[slice] = sib;
		ret = 0;
	}

	/*
	 * Finalize: validate every installed slice against the slot it
	 * came from (the post-install revalidation, explained in
	 * shmem_decompress_folio()).  A truncate/punch that cleared
	 * entries[sidx] while we decompressed leaves the i_pages slot
	 * EMPTY, so the install above succeeded and would resurrect a
	 * folio past i_size or inside a hole, where tmpfs must serve
	 * zeros.  shmem_undo_range clears slots before its i_pages
	 * walks, so "slot still names ent here" implies truncate's own
	 * walk runs after our install and removes the folio itself --
	 * either way no folio survives the truncate.  Holding every
	 * slice locked until here also blocks a racing write_begin (a
	 * cleared slot can only mean truncate, never the write-
	 * invalidate, so removing is safe) and blocks reclaim from
	 * detaching a sibling before its check.
	 */
	for (slice = 0; slice < nr_pages; slice++) {
		struct folio *sib = sibs[slice];
		pgoff_t sidx = head + slice;

		if (!sib)
			continue;
		if (!shmem_comp_entries_names(inode, sidx, ent)) {
			/* The folio just left the page cache: drain lock
			 * waiters the PG_waiters fast path can skip
			 * (entry-race lost-wakeup fix -- no future
			 * lock/unlock is guaranteed). */
			shmem_delete_from_page_cache(sib, NULL);
			folio_unlock(sib);
			wake_up_folio_waiters(sib);
			folio_put(sib);
			if (sidx == index)
				ret = -ENOENT;	/* retry against the updated
						 * tables */
			continue;
		}
		if (sidx == index) {
			*foliop = sib;	/* locked, ref for caller */
		} else {
			folio_unlock(sib);
			folio_put(sib);
		}
	}

	/* The group entry survives (entries[] still names the surviving
	 * slots' entry); the compressed gauge is NOT decremented here --
	 * it drops when the entry is freed on truncate/evict.  Drop the
	 * group pin. */
	shmem_comp_entry_put(ent);

	kfree(cbuf);
	kfree(dbuf);
	kfree(sibs);
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
	struct shmem_comp_entry *ent;
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
		u64 handle;

		handle = shmem_comp_store(sbinfo->comp->pool,
					   cbuf, comp_len);
		if (!handle) {
			ret = 0;
			goto cleanup;
		}
		ent = shmem_comp_entry_alloc(sbinfo->comp->pool, handle,
					      comp_len, nr, NULL, 0);
		if (!ent) {
			u32 cellid;
			u32 offset;

			shmem_comp_unpack_loc(handle, &cellid, &offset);
			cell_free(shmem_comp_pool_cells(sbinfo->comp->pool),
				  cellid, offset, comp_len);
			ret = 0;
			goto cleanup;
		}
	} else {
		u64 *handles = NULL;
		unsigned short nchunks = 0;

		ret = shmem_comp_store_merged(sbinfo->comp->pool,
					      cbuf, comp_len,
					      &handles, &nchunks);
		if (ret) {
			ret = 0;
			goto cleanup;
		}
		ent = shmem_comp_entry_alloc(sbinfo->comp->pool, 0,
					      comp_len, nr,
					      handles, nchunks);
		if (!ent) {
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
		/* Free a modified both-state folio's stale compressed copy
		 * before overwriting its entries[] slot (see the single-page
		 * path). */
		shmem_recalc_inode(inode, 0,
			   -(long)shmem_comp_invalidate_index(inode,
							folios[i]->index));
		shmem_delete_from_page_cache(folios[i], NULL);
		/* Two-table: entries[index] names the compressed entry;
		 * i_pages[index] is cleared (the folio leaves the page
		 * cache).  One entry pointer covers the whole group. */
		shmem_comp_entries_set(inode, folios[i]->index, ent);
		folio_unlock(folios[i]);
		/* The folio just left the page cache: drain lock waiters the
		 * PG_waiters fast path can skip (entry-race lost-wakeup fix,
		 * with the single-page drain in shmem_comp_compress_one).  The
		 * group swap-out path was missing it: a concurrent reader that
		 * cache-hit this folio and slept in folio_lock() could have its
		 * wake skipped, and since the folio is out of i_pages no later
		 * lock/unlock is guaranteed to wake it -- a lost wakeup that
		 * hangs the reader (observed as the trres Phase-B merge=8 hang). */
		wake_up_folio_waiters(folios[i]);
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
 * @gfp: allocation flags for the replacement folio
 * @foliop: output, receives the decompressed folio
 *
 * Return: 0 on success, negative errno on failure
 *
 * Resolves the compressed entry from the per-inode entries[] table (the
 * two-table mapping), loads and decompresses it into a freshly allocated
 * folio, and installs the folio in the empty page-cache slot.  The entry
 * is KEPT -- entries[index] continues to name it, putting the page in the
 * "both" state -- so reclaim can later drop the folio and a later read
 * re-decompresses.  The folio is returned locked and with a reference,
 * matching the shmem_get_folio() contract.
 *
 * A racing truncate/punch that clears entries[index] frees the entry,
 * but a pin taken before the clear keeps it alive -- so the install
 * below is followed by a slot revalidation: a truncate leaves the
 * i_pages slot EMPTY, which an expected=NULL install would happily
 * refill with a resurrected folio.  A failed revalidation undoes the
 * install and returns -ENOENT; the caller retries against the updated
 * tables.  A folio that loses the install race gets -EEXIST and the
 * caller retries.
 */
int shmem_decompress_folio(struct inode *inode, pgoff_t index,
			   gfp_t gfp, struct folio **foliop)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	struct shmem_comp_entry *ent;
	u64 handle;
	unsigned int comp_len;
	unsigned int nr_pages;
	unsigned int dlen;
	void *src, *dst;
	int ret;

	*foliop = NULL;

	if (!sbinfo->comp)
		return -ENODEV;

	/* Resolve entries[index] -> pinned entry in ONE rcu_read_lock
	 * section (entries_get).  A racing truncate that cleared the
	 * slot freed the entry; the pin fails on the dying incarnation
	 * (-ENOENT -> retry). */
	ent = shmem_comp_entries_get(inode, index, &handle, &comp_len,
				      &nr_pages, NULL, NULL);
	if (!ent)
		return -ENOENT;

	/*
	 * Merged group (merge_pages > 1): the entry pointer is
	 * replicated in all nr_pages slots of the group. Decompress the
	 * whole block and install one folio per slice; the requested
	 * slice is returned. Hand our pin to the group path (it owns
	 * and drops it) so the reference accounting stays one-pin-per
	 * get.
	 */
	if (nr_pages > 1)
		return shmem_decompress_group(inode, index, ent,
					       comp_len, nr_pages,
					       gfp, foliop);

	/* Read the compressed bytes out of the pool first.
	 * GFP_KERNEL: transient slab buffer, must not carry __GFP_HIGHMEM.
	 */
	src = kmalloc(comp_len, GFP_KERNEL);
	if (!src) {
		shmem_comp_entry_put(ent);
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
		ret = shmem_comp_load_entry(ent, src, &comp_len);
	else
		ret = shmem_comp_load(sbinfo->comp->pool, handle, src, &comp_len);
	if (ret) {
		kfree(src);
		shmem_comp_entry_put(ent);
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
		shmem_comp_entry_put(ent);
		return -ENOMEM;
	}
	/*
	 * Charge the folio to memcg BEFORE it enters the page cache and
	 * the LRU, exactly as shmem_alloc_and_add_folio() does for the
	 * write/fault paths (mm/shmem.c).  shmem_alloc_folio() only
	 * allocates; an uncharged folio added to the LRU spins forever
	 * in folio_lruvec_lock_irqsave()'s memcg-validate retry loop on
	 * kernels that re-check lruvec_memcg vs folio_memcg under the
	 * lock (observed as a hard hang on arm64 TCG with MEMCG=y;
	 * invisible on the 6.18 harness kernel and with
	 * cgroup_disable=memory).  NULL mm matches the write path.
	 */
	if (mem_cgroup_charge(folio, NULL, gfp)) {
		folio_put(folio);
		kfree(src);
		shmem_comp_entry_put(ent);
		return -ENOMEM;
	}
	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);

	/*
	 * Decompress into a lowmem bounce, then copy into the folio --
	 * same reason as the compress bounce: the crypto sg must be
	 * built from direct-mapped memory, never from a kmap_local
	 * address (32-bit + HIGHMEM: virt_to_page is invalid there).
	 */
	dst = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!dst) {
		folio_unlock(folio);
		folio_put(folio);
		shmem_comp_entry_put(ent);
		kfree(src);
		return -ENOMEM;
	}
	dlen = PAGE_SIZE;
	ret = shmem_comp_decompress(sbinfo->comp->pool, src, comp_len,
				    dst, &dlen);
	kfree(src);
	if (!ret) {
		void *kaddr = kmap_local_folio(folio, 0);

		memcpy(kaddr, dst, PAGE_SIZE);
		kunmap_local(kaddr);
	}
	kfree(dst);

	if (ret) {
		folio_unlock(folio);
		folio_put(folio);
		shmem_comp_entry_put(ent);
		return ret;
	}

	/* Data corruption: a successful decompress of a single-page
	 * block must produce exactly one page. Anything else means the
	 * compressed stream or the entry metadata is wrong -- serving
	 * it to userspace would be silent corruption, so stop. */
	if (unlikely(dlen != PAGE_SIZE))
		panic("shmem_comp: decompressed %u bytes, expected %u "
		      "-- compressed data corrupt (id=%u)",
		      dlen, (unsigned int)PAGE_SIZE, ent->id);

	folio_mark_uptodate(folio);

	/*
	 * Install the folio in the empty slot (expected NULL).  The
	 * compressed entry is KEPT: entries[index] still names it (the
	 * "both" state).  A racing fill of the slot yields -EEXIST
	 * (retry); a racing truncate that freed the entry yields -ENOENT
	 * on the next re-pin.
	 */
	ret = shmem_add_to_page_cache(folio, mapping, index, NULL, gfp);
	if (ret) {
		folio_unlock(folio);
		folio_put(folio);
		shmem_comp_entry_put(ent);
		return ret;
	}

	/*
	 * Post-install slot revalidation -- the two-table analogue of
	 * upstream's expected-swap install (shmem_swapin_folio passes
	 * the swap entry it read, so a truncate that removed it fails
	 * the install; our compressed entry lives in a different
	 * xarray, so the slot claim has to be re-checked here).  A
	 * truncate/punch that cleared entries[index] while we
	 * decompressed leaves the i_pages slot EMPTY, so the install
	 * above succeeds and would resurrect a folio past i_size or
	 * inside a hole.  The pin keeps the ENTRY alive but says
	 * nothing about the SLOT; if the slot no longer names our entry,
	 * undo the install.  The folio is still locked, so no writer
	 * could have claimed it (a racing write_begin blocks on
	 * folio_lock) -- a cleared slot can only mean truncate/punch,
	 * and shmem_undo_range clears slots before its i_pages walks,
	 * so this check closes the window completely: either the slot
	 * still names ent (and truncate's later walk removes the folio
	 * itself), or we remove it now.
	 */
	if (!shmem_comp_entries_names(inode, index, ent)) {
		shmem_delete_from_page_cache(folio, NULL);
		folio_unlock(folio);
		/* The folio just left the page cache: drain lock waiters
		 * the PG_waiters fast path can skip (entry-race lost-
		 * wakeup fix -- no future lock/unlock is guaranteed). */
		wake_up_folio_waiters(folio);
		folio_put(folio);
		shmem_comp_entry_put(ent);
		return -ENOENT;
	}

	/*
	 * Both-state accounting: the folio adds nrpages while the entry
	 * keeps its swapped charge, so alloced (the size= budget) is
	 * unchanged -- the folio is a reclaimable cache of the one logical
	 * page, not a second copy charged to the budget.  The compressed
	 * gauge stays (the entry survives); it drops when the entry is
	 * freed on truncate/evict.  Drop our pin: entries[index] holds the
	 * durable reference now.
	 *
	 * The folio is a reclaimable CACHE of the compressed entry, not a
	 * swappable anon page -- the entry is its durable backing, like a
	 * disk block.  Clear the swapbacked bit (shmem's own add-to-page-
	 * cache requires it set, so it goes only after the install) so the
	 * folio lands on the FILE lru: reclaim scans file pages even with no
	 * swap and drops this clean cache via __remove_mapping, which is what
	 * lets a >RAM read-back survive.  A swapbacked folio would sit on the
	 * anon lru, which reclaim skips without swap (get_scan_count ->
	 * SCAN_FILE), accumulating until OOM.  It is also deliberately NOT
	 * marked referenced: a cache cheap to re-materialise should be dropped
	 * promptly under pressure, not retained as a hot page.
	 */
	__folio_clear_swapbacked(folio);
	folio_add_lru(folio);
	shmem_comp_entry_put(ent);

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

/*
 * The per-inode entries[] table (the two-table mapping, specs/02).  Each
 * present slot holds the POINTER of the struct shmem_comp_entry backing
 * that page index; an absent slot = no compressed copy.  No swap-type
 * tag: entries[] is a dedicated table that never holds folios or real
 * swap entries, and a kmalloc pointer is always distinguishable from
 * the xarray's internal/value encodings.  The slot owns the entry's
 * durable reference (counted by the entry's `slots` counter).
 *
 * Pointer = identity: readers pin under rcu_read_lock (entries_get),
 * clearers claim the slot with xa_erase (exactly one clearer can own
 * the slot ref).  The old packed {id, gen} xa_value is gone -- its
 * 55-bit payload did not fit the 31-bit xa_mk_value budget on 32-bit,
 * and RCU plus the slot-claim make the generation redundant.
 */

static int shmem_comp_entries_set(struct inode *inode, pgoff_t index,
				  struct shmem_comp_entry *ent)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa;
	struct xarray *old;

	xa = READ_ONCE(info->comp_entries);
	if (!xa) {
		xa = kmalloc(sizeof(*xa), GFP_KERNEL);
		if (!xa)
			return -ENOMEM;
		xa_init(xa);
		old = cmpxchg(&info->comp_entries, NULL, xa);
		if (old) {
			/* lost the lazy-init race; use the winner's */
			kfree(xa);
			xa = old;
		}
	}
	xa_store(xa, index, ent, GFP_KERNEL);
	return 0;
}

/*
 * shmem_comp_entries_claim - atomically take ownership of entries[index]
 * @inode: shmem inode
 * @index: page index being cleared (truncate, write-invalidate)
 *
 * Return: the slot's entry with its durable reference transferred to
 * the caller, or NULL when the slot is absent (or a concurrent clearer
 * got there first).
 *
 * xa_erase() returns the old value under the xarray lock, so exactly
 * one thread can claim a given slot ref.  This is the whole stale-slot
 * defence on the clear path: the old code re-validated {id, gen}
 * because an id could be recycled between lookup and release; a
 * pointer cannot be recycled while we hold the erase that named it.
 * The caller drops the claimed ref with shmem_comp_entry_release().
 */
static struct shmem_comp_entry *
shmem_comp_entries_claim(struct inode *inode, pgoff_t index)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa = READ_ONCE(info->comp_entries);

	if (!xa)
		return NULL;
	return xa_erase(xa, index);
}

/*
 * shmem_comp_entries_present - true if entries[index] names an entry
 *
 * Quick presence test for the read path, distinguishing a compressed
 * page (i_pages empty, entries[] set) from a genuine hole (both
 * empty).  No reference is taken and the pointer is not dereferenced;
 * RCU only needs to keep the xa_node walk valid for the load.
 */
bool shmem_comp_entries_present(struct inode *inode, pgoff_t index)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa = READ_ONCE(info->comp_entries);
	void *entry;

	if (!xa)
		return false;
	rcu_read_lock();
	entry = xa_load(xa, index);
	rcu_read_unlock();
	return entry != NULL;
}

/*
 * shmem_comp_entry_drop - drop one claimed entries[] slot reference
 * @inode: shmem inode
 * @ent: entry claimed via shmem_comp_entries_claim() (slot ref owned)
 *
 * Returns 1 (one logical page whose shmem swapped charge the caller
 * must drop via a single recalc).  The entry is alive by construction
 * -- the claimed slot ref keeps it so until release() drops it -- so
 * the accounting fields are read directly, no pin needed.  The
 * per-mount compression gauge is corrected once, on the entry's last
 * slot.
 */
static unsigned int shmem_comp_entry_drop(struct inode *inode,
					  struct shmem_comp_entry *ent)
{
	struct shmem_comp_sb *comp = shmem_comp_get_sb(inode->i_sb)->comp;
	unsigned int length, nr;
	bool last;

	length = ent->loc.length;
	nr = ent->nr_pages;
	last = shmem_comp_entry_release(ent);
	if (comp && last) {
		atomic_long_sub(nr, &comp->pages);
		atomic_long_sub(length, &comp->bytes);
		shmem_comp_update_stats(false, nr * PAGE_SIZE, length);
	}
	return 1;
}

/*
 * shmem_comp_invalidate_index - free a modified page's stale compressed copy
 * @inode: shmem inode
 * @index: page index being modified (re-compressed)
 *
 * A both-state folio that was written still names its (now stale)
 * compressed copy in entries[index].  The compress point is the single
 * choke point that overwrites entries[index], so freeing the old entry
 * here keeps a re-compressed page from leaking it, and drops that
 * entry's swapped charge.  Returns the shmem swapped pages to recalc.
 */
static unsigned int shmem_comp_invalidate_index(struct inode *inode,
						pgoff_t index)
{
	struct shmem_comp_entry *ent;

	/*
	 * Claim the slot first: entries[index] must never dangle onto a
	 * freed entry, and the claim (xa_erase) is what makes us the ONE
	 * owner of the slot's durable reference -- a concurrent truncator
	 * racing this clear gets NULL and does nothing, instead of both
	 * threads dropping the same ref (the old double-drop window the
	 * generation check only half-covered: it rejected recycled ids
	 * but could not stop two live readers of the same {id, gen}).
	 */
	ent = shmem_comp_entries_claim(inode, index);
	if (!ent)
		return 0;
	return shmem_comp_entry_drop(inode, ent);
}

/*
 * shmem_comp_invalidate_write - a write made this page's compressed copy stale
 *
 * The write-invalidate (specs/02): a write to a folio that still has a
 * surviving compressed backing (the "both" state) makes that backing
 * stale, so free it -- the folio becomes the sole copy and write_end's
 * recompress stores a fresh one.  This keeps the invariant "a dirty folio
 * has no compressed backing" from the moment of the write, rather than
 * only once the page happens to be re-compressed.  Called from
 * shmem_write_begin() (process context, i_rwsem held), so the entry
 * teardown and the swapped recalc are safe.
 */
void shmem_comp_invalidate_write(struct inode *inode, pgoff_t index)
{
	unsigned int freed = shmem_comp_invalidate_index(inode, index);

	if (freed)
		shmem_recalc_inode(inode, 0, -(long)freed);
}

/*
 * shmem_comp_truncate_range - free compressed entries in [start, end)
 *
 * The truncate hook (shmem_undo_range): clear every entries[] slot in the
 * range, dropping each entry's durable reference (the entry frees when the
 * last reference -- slot or reader pin -- goes).  Returns the number of
 * logical pages freed so the caller drops the shmem swapped charge in one
 * recalc.  Safe on an absent table.
 */
unsigned int shmem_comp_truncate_range(struct inode *inode, pgoff_t start,
				       pgoff_t end)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa = READ_ONCE(info->comp_entries);
	unsigned int freed = 0;
	unsigned long index = start;
	void *entry;

	if (!xa || start >= end)
		return 0;

	/* xa_find/xa_find_after is the safe erase-during-iterate pattern:
	 * claim the current slot (which removes it), then continue from
	 * the next present entry.  The claim beats any concurrent
	 * write-invalidate on the same index: exactly one of the two
	 * clearers gets the ref. */
	entry = xa_find(xa, &index, end - 1, XA_PRESENT);
	while (entry) {
		struct shmem_comp_entry *ent;

		ent = shmem_comp_entries_claim(inode, index);
		if (ent)
			freed += shmem_comp_entry_drop(inode, ent);
		entry = xa_find_after(xa, &index, end - 1, XA_PRESENT);
	}
	return freed;
}

/*
 * shmem_comp_entries_destroy - free an inode's entries[] and the table
 * @inode: shmem inode being evicted
 *
 * Called from shmem_evict_inode() after truncate.  shmem_comp_truncate_range()
 * has normally already emptied the table; this is the safety net that
 * frees any straggler so no compressed block outlives its inode (the
 * shmem swapped recalc is skipped -- the inode is going away).  Safe on
 * NULL (most inodes never compress, so the table is never allocated).
 */
void shmem_comp_entries_destroy(struct inode *inode)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct xarray *xa;

	shmem_comp_truncate_range(inode, 0, (pgoff_t)-1);

	xa = xchg(&info->comp_entries, NULL);
	if (xa)
		xa_destroy(xa);
	kfree(xa);
}

static int shmem_comp_compress_one(struct inode *inode, pgoff_t index,
				   struct folio *folio, bool allow_clean)
{
	struct shmem_sb_info *sbinfo = shmem_comp_get_sb(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	void *src, *comp_buf;
	unsigned int comp_len;
	struct shmem_comp_entry *ent;
	u64 handle;
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
	 * (allow_clean=false). The aging pass allows clean pages, but
	 * skips a clean both-state folio (already compressed, below).
	 * Skipping is not an error: the page simply stays as it was.
	 */
	if (folio_mapped(folio) || folio_maybe_dma_pinned(folio) ||
	    folio_order(folio) || (!allow_clean && !folio_test_dirty(folio))) {
		folio_unlock(folio);
		return 0;
	}

	/*
	 * A clean both-state folio is already compressed: entries[index]
	 * names its durable copy and the folio is just a reclaimable cache
	 * of it.  Re-compressing would free that entry and store an
	 * identical one -- pure churn for no memory gain, so skip.  Only
	 * the aging pass reaches this (write_end requires dirty); a DIRTY
	 * both-state folio's entry is stale and IS worth re-compressing,
	 * which the choke-point invalidate below handles.  This replaces
	 * the old "re-compress a decompressed-on-read survivor" policy,
	 * which was right when decompress freed the entry but is wrong now
	 * that the entry survives.
	 */
	if (allow_clean && !folio_test_dirty(folio) &&
	    shmem_comp_entries_present(inode, index)) {
		folio_unlock(folio);
		return 0;
	}

	/*
	 * Bounce the folio through a lowmem kmalloc buffer BEFORE the
	 * crypto call.  sg_init_one() derives struct page * via
	 * virt_to_page(), which is INVALID for kmap_local addresses on
	 * 32-bit + HIGHMEM (fixmap range): arm32 would compress the
	 * WRONG memory and store the result -- observed as readback
	 * corruption from byte 0 on every page.  Invisible on 64-bit
	 * (kmap == direct map) and with small RAM (no highmem pages
	 * exist).  kmalloc is always direct-mapped, so the sg is
	 * valid on every arch.
	 */
	comp_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!comp_buf) {
		folio_unlock(folio);
		return -ENOMEM;
	}
	src = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!src) {
		kfree(comp_buf);
		folio_unlock(folio);
		return -ENOMEM;
	}
	{
		void *kaddr = kmap_local_folio(folio, 0);

		memcpy(src, kaddr, PAGE_SIZE);
		kunmap_local(kaddr);
	}

	/* Compress the page */
	comp_len = PAGE_SIZE;
	ret = shmem_comp_compress(sbinfo->comp->pool, src, PAGE_SIZE,
				 comp_buf, &comp_len);
	kfree(src);

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
	 * zsmalloc handle (or packed cell location) lives in the
	 * entry, which entries[index] names by POINTER; the page cache
	 * itself stays compression-free.  The entry owns the handle
	 * from here (freed with the entry).
	 */
	ent = shmem_comp_entry_alloc(sbinfo->comp->pool, handle, comp_len,
				     1, NULL, 0);
	if (!ent) {
		shmem_comp_free(sbinfo->comp->pool, handle);
		folio_unlock(folio);
		return -ENOMEM;
	}
	/* A modified both-state folio still names its (now stale)
	 * compressed copy; free it here -- the compress point is the
	 * single choke point that overwrites entries[index], so this
	 * keeps a re-compressed page from leaking its old entry (and
	 * drops that entry's swapped charge). */
	shmem_recalc_inode(inode, 0,
			   -(long)shmem_comp_invalidate_index(inode, index));

	/*
	 * Compressed slots are accounted like swap pages: raise
	 * swapped before the slot clear lowers nrpages, so a racing
	 * shmem_recalc_inode() never sees the page as freed (the
	 * same ordering shmem_writeout() relies on). Pairs with the
	 * -freed recalc at the end of shmem_undo_range(), which
	 * restores the charge when the entry is truncated.
	 */
	shmem_recalc_inode(inode, 0, 1);

	/* Two-table mapping: clear i_pages[index] (the folio leaves the
	 * page cache; the caller keeps its own reference) and record the
	 * compressed entry in entries[index].  The compressed mapping
	 * lives in entries[] now, not in the page cache. */
	shmem_delete_from_page_cache(folio, NULL);
	shmem_comp_entries_set(inode, index, ent);

	/* Update accounting */
	atomic_long_inc(&sbinfo->comp->pages);
	atomic_long_add(comp_len, &sbinfo->comp->bytes);
	shmem_comp_update_stats(true, PAGE_SIZE, comp_len);

	/*
	 * The folio has left the page cache; the caller still owns a
	 * reference and releases it after we return. Return 1 = compressed,
	 * 0 = skipped, negative = error (the aging budget counts 1 as work
	 * done).
	 *
	 * Drain the folio's lock waiters (entry-race lost-wakeup fix):
	 * folio_unlock()'s wake walk may skip an exclusive waiter when the
	 * lock bit momentarily reads set, betting on a future unlock -- but
	 * the folio is out of the mapping now, so no future lock/unlock on
	 * it is guaranteed, and a skipped waiter would sleep forever.  With
	 * the bit clear the extra walk cannot skip.
	 */
	folio_unlock(folio);
	wake_up_folio_waiters(folio);

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
					 unsigned int *budget,
					 bool hot)
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
				/*
				 * Calm-mode age gate (specs/06): a young
				 * chilled page that gets bg-compressed and
				 * then immediately read pays compression AND
				 * decompression for nothing (the ripple).
				 * First sight just marks (PG_referenced);
				 * only pages that survived a previous pass
				 * are compressed.  Under pressure (hot) the
				 * gate lifts: the backlog goes immediately.
				 */
				if (!hot && !folio_test_referenced(folio)) {
					folio_set_referenced(folio);
					continue;
				}
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
static void shmem_comp_maintain(struct shmem_comp_sb *comp, bool hot)
{
	struct super_block *sb = comp->sb;
	struct inode *inode;
	struct inode *batch[SHMEM_COMP_AGE_BATCH_INODES];
	unsigned int batch_n = min_t(unsigned int, comp->age_batch,
				     SHMEM_COMP_AGE_BATCH_INODES);
	unsigned int nb = 0, i, r;
	bool more = false;

	if (!comp->pool || !sb)
		return;

	/*
	 * The hot/chill flip moved to the write path (specs/06:
	 * shmem_comp_wm_update, RAM-usage signal).  This pass no longer
	 * flips; it captures the mode once and runs the two duties:
	 * age-gated backlog compression (calm) or immediate (hot), then
	 * the evacuation GC interleave.
	 */

	if (!down_read_trylock(&sb->s_umount))
		return;	/* unmount in progress; skip this pass */

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		if (nb >= batch_n)
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

	for (r = 0; r < comp->age_rounds; r++) {
		unsigned int budget = comp->age_budget;
		unsigned int aged = shmem_comp_age_round(comp, batch, nb,
							 &budget, hot);

		if (aged) {
			atomic_long_add(aged, &comp->aged_pages);
			atomic_long_add(aged, &shmem_comp_aged_pages);
		}
		/* Cell GC moved off the fixed 1 Hz cadence: it is now a
		 * per-pool demand-triggered worker armed from the free path
		 * (see shmem_comp_pool_arm_gc).  This pass keeps only the
		 * mount-specific page aging (compressing uncompressed
		 * survivors), which is not tied to cell frees. */
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

	/* (specs/06 bisect) re-arm only on remaining budget; the hot
	 * cadence variant is under investigation for the entry-race
	 * hang. */
	if (more)
		schedule_delayed_work(&comp->age_work,
				      msecs_to_jiffies(comp->age_delay_ms));
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
	/* mode captured once per pass (specs/06): hot = pressure, so the
	 * backlog half compresses immediately; calm = age-gated. */
	bool hot = READ_ONCE(comp->hot);

	mutex_lock(&shmem_comp_mounts_lock);
	shmem_comp_maintain(comp, hot);
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

	/* hot=true: memory pressure lifts the calm-mode age gate
	 * (specs/06) -- the backlog must go now. */
	mutex_lock(&shmem_comp_mounts_lock);
	list_for_each_entry(comp, &shmem_comp_mounts, list)
		shmem_comp_maintain(comp, true);
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

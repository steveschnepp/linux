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
#include <crypto/acompress.h>
#include <linux/shmem_compress.h>
#include <linux/shmem_fs.h>
#include <linux/cell_store.h>

#define cell_dbg(fmt, ...) \
	pr_debug("shmem_comp: " fmt "\n", ##__VA_ARGS__)

/* GC implementation lives after the entry table (needs the xarray) */
static void shmem_comp_cell_gc(struct shmem_comp_pool *pool);
static unsigned long shmem_comp_cell_count(struct shrinker *shrinker,
					   struct shrink_control *sc);
static unsigned long shmem_comp_cell_scan(struct shrinker *shrinker,
					  struct shrink_control *sc);

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
	struct list_head list;			/* global pools list */
};

/* Global list of all compression pools */
static LIST_HEAD(shmem_comp_pools);
/* Protects shmem_comp_pools list modifications */
static DEFINE_SPINLOCK(shmem_comp_pools_lock);
/* Counter for generating unique pool names */
static atomic_t shmem_comp_pools_count = ATOMIC_INIT(0);

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
 *
 * Return: Pointer to pool on success, ERR_PTR on failure
 *
 * Creates a new compression pool with the specified algorithm.
 * Allocates:
 * - zsmalloc pool for compressed data storage
 * - Per-CPU compression contexts
 *
 * The pool is added to the global list for tracking.
 * Callers must call shmem_comp_pool_put() when done.
 */
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo,
						unsigned int cell_order)
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
		pool->cells = cell_pool_create(cell_order, cell_order >= 4);
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
		u16 cellid;
		u32 offset;

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
		u16 cellid;
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
		if (ent->pool->use_cells) {
			u16 cellid;
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
	int ret;

	ent = kmalloc(sizeof(*ent), GFP_KERNEL);
	if (!ent)
		return 0;

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
 * sparse cells; this pass does the actual relocation.  Batching is
 * the point: up to GC_BATCH victims are harvested first, then ONE
 * entry-table scan relocates every live block that resides in any
 * of them, then all emptied cells return to the buddy.  Per-free
 * synchronous evacuation could never amortise the scan this way.
 * Runs from the shrinker (memory pressure) or the gc_now debug
 * hook — never from the free path.
 *
 * Protocol:
 *   pick_victim()    harvests a sparse cell and takes down_write on
 *                    it; no reader can be inside cell_load() for it,
 *                    and cell_free()'s empty-return path blocks on
 *                    the same lock.  Repeated up to GC_BATCH times.
 *   scan             pins each entry residing in any victim
 *                    (inc_not_zero — dying entries are skipped, their
 *                    own free path handles the accounting), copies
 *                    the block, relocates it, updates the entry
 *                    handle with a WRITE_ONCE.
 *   release_victim() per victim: rechecks under cp->lock — empty →
 *                    buddy, still sparse → re-link for a later pass.
 *
 * Lock order: victim rwsem → cp->lock, matching cell_free()
 * (down_read → cp->lock).  The reader's retry loop in
 * shmem_comp_load_entry() closes the window where a stale handle
 * outlives the relocation.
 */
static void shmem_comp_cell_gc(struct shmem_comp_pool *pool)
{
	struct cell_pool *cp = pool->cells;
	struct cell *victims[GC_BATCH];
	unsigned int nv = 0, v;
	unsigned long id;
	struct shmem_comp_entry *ent;
	unsigned int moved = 0;

	/* Harvest phase: batch victims under their write locks */
	while (nv < GC_BATCH) {
		struct cell *c = cell_pool_pick_victim(cp);

		if (!c)
			break;
		victims[nv++] = c;
	}
	if (!nv)
		return;

	/* One scan relocates blocks from every victim */
	xa_for_each(&shmem_comp_entries, id, ent) {
		unsigned long h;
		u16 cellid;
		u32 offset;
		unsigned int len;
		u16 nc;
		u32 no;
		struct cell *vic = NULL;
		int ret;

		if (ent->pool != pool)
			continue;

		h = READ_ONCE(ent->handle);
		if (!h)
			continue;	/* merged zsmalloc entry */
		shmem_comp_unpack_loc(h, &cellid, &offset);

		for (v = 0; v < nv; v++) {
			if (victims[v]->cellid == cellid) {
				vic = victims[v];
				break;
			}
		}
		if (!vic)
			continue;

		/* Pin: skip entries whose last reference is dropping */
		if (!atomic_inc_not_zero(&ent->refs))
			continue;

		len = ent->length;
		if (offset + len > (1UL << vic->order) * PAGE_SIZE) {
			shmem_comp_entry_put(id);
			continue;
		}

		ret = cell_relocate_one(cp, vic,
					 vic->base + offset, len,
					 &nc, &no);
		if (ret == 0) {
			WRITE_ONCE(ent->handle,
				   shmem_comp_pack_loc(nc, no));
			moved++;
		}
		shmem_comp_entry_put(id);
	}

	/* Release phase: emptied cells go back to the buddy */
	for (v = 0; v < nv; v++)
		cell_pool_release_victim(cp, victims[v]);

	cell_dbg("gc pass: moved %u objects, released %u victim cells",
		 moved, nv);
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
		u16 cellid;
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

		cell_size = (1UL << c->order) * PAGE_SIZE;
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
		u16 i;

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
				(1UL << c->order) * PAGE_SIZE,
				c->live_objects, c->live_bytes,
				c->active ? "ACTIVE" : "",
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
static unsigned int shmem_comp_default_wm_low;	/* built-in default 0:0 = eager */
static unsigned int shmem_comp_default_wm_high;

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
int shmem_comp_fill_sb(struct shmem_sb_info *sbinfo,
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

	comp->pool = shmem_comp_pool_create(algo, cell_order);
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
	if (sbinfo->comp) {
		if (sbinfo->comp->pool)
			shmem_comp_pool_put(sbinfo->comp->pool);
		kfree(sbinfo->comp->compressor);
		kfree(sbinfo->comp);
		sbinfo->comp = NULL;
	}
}

bool shmem_comp_should_compress(struct inode *inode)
{
	struct shmem_comp_sb *comp = shmem_comp_get_sb(inode->i_sb)->comp;

	return comp && comp->enabled && comp->pool &&
		i_size_read(inode) >= comp->threshold_bytes;
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
 * "cell_stats" and "gc_now" expose the live state of every pool.
 * Registered by tmpfs_sysfs_init() in mm/shmem.c with the tmpfs
 * kobject as parent.
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
	bool v;
	int ret;

	ret = kstrtobool(buf, &v);
	if (ret)
		return ret;
	if (v)
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
		      dlen, (unsigned int)nr_pages * PAGE_SIZE, id);

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
			u16 cellid;
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
		      dlen, PAGE_SIZE, id);

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
int shmem_compress_folio(struct inode *inode, pgoff_t index,
				struct folio *folio)
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
	 * dirtied this folio; clean means the state changed under us.
	 * Skipping is not an error: the page simply stays uncompressed.
	 */
	if (folio_mapped(folio) || folio_maybe_dma_pinned(folio) ||
	    folio_order(folio) || !folio_test_dirty(folio)) {
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
	 * The folio has left the page cache; the caller (write_end)
	 * still owns a reference and releases it after we return.
	 */
	folio_unlock(folio);

	return 0;
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

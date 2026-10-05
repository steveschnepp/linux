// SPDX-License-Identifier: GPL-2.0
/*
 * mm/shmem_compress.c - Transparent compression for tmpfs
 *
 * This file implements the compression pool infrastructure for tmpfs
 * transparent page-level compression.
 *
 * The compression subsystem provides:
 * - Per-CPU compression contexts using kernel crypto API (LZ4/LZO)
 * - zsmalloc-based storage for compressed page data
 * - Pool lifecycle management (create/destroy)
 * - Statistics tracking for sysfs reporting
 *
 * Design Notes:
 * - Compression uses the crypto acomp (asynchronous compression) API
 * - Each CPU has its own compression context to avoid lock contention
 * - zsmalloc provides efficient variable-size allocation for compressed data
 * - Similar architecture to zswap but integrated directly into tmpfs
 *
 * Compression Flow:
 *   1. Page is compressed via shmem_comp_compress()
 *   2. Compressed data stored via shmem_comp_store()
 *   3. xarray entry replaced with shmem_comp_entry encoding
 *
 * Decompression Flow:
 *   1. xarray entry decoded via shmem_radix_to_comp_entry()
 *   2. Data loaded via shmem_comp_load()
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
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/zsmalloc.h>
#include <crypto/acompress.h>
#include <linux/shmem_compress.h>

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
	char *buffer;			/* PAGE_SIZE scratch buffer */
	struct mutex lock;		/* protects req/buffer usage */
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
struct shmem_comp_pool {
	struct zs_pool *zs_pool;		/* zsmalloc storage pool */
	struct shmem_comp_pcpu __percpu *pcpu;	/* per-CPU contexts */
	char algo_name[CRYPTO_MAX_ALG_NAME];	/* algorithm name */
	atomic_t refcount;			/* reference count */
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
	.avg_compress_time_ns = 0,
	.avg_decompress_time_ns = 0,
};

/* Spinlock protecting statistics updates */
static DEFINE_SPINLOCK(shmem_comp_stats_lock);

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

	/* Allocate scratch buffer for compression output */
	pcpu->buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!pcpu->buffer) {
		acomp_request_free(pcpu->req);
		crypto_free_acomp(pcpu->acomp);
		pcpu->req = NULL;
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
	if (pcpu->buffer) {
		kfree(pcpu->buffer);
		pcpu->buffer = NULL;
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
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo)
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

	/* Create zsmalloc pool for compressed data storage */
	pool_id = atomic_inc_return(&shmem_comp_pools_count);
	pool->zs_pool = zs_create_pool(algo);
	if (!pool->zs_pool) {
		pr_warn("shmem_comp: failed to create zsmalloc pool\n");
		kfree(pool);
		return ERR_PTR(-ENOMEM);
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

	/* Destroy zsmalloc pool */
	if (pool->zs_pool)
		zs_destroy_pool(pool->zs_pool);

	pr_info("shmem_comp: destroyed pool with algorithm '%s'\n",
		pool->algo_name);

	kfree(pool);
}

/**
 * shmem_comp_pool_get - Get reference to a pool
 * @pool: Pool to reference
 *
 * Increments the pool refcount. Callers must call shmem_comp_pool_put()
 * when done with the pool.
 */
void shmem_comp_pool_get(struct shmem_comp_pool *pool)
{
	if (pool)
		atomic_inc(&pool->refcount);
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
			const void *src, void *dst,
			unsigned int *dst_len)
{
	struct shmem_comp_pcpu *pcpu;
	struct scatterlist input, output;
	unsigned int dlen;
	ktime_t start, end;
	s64 duration;
	int ret;

	if (!pool || !src || !dst || !dst_len)
		return -EINVAL;

	pcpu = this_cpu_ptr(pool->pcpu);
	dlen = *dst_len;

	mutex_lock(&pcpu->lock);

	start = ktime_get();

	/* Setup scatterlists for crypto operation */
	sg_init_one(&input, src, PAGE_SIZE);
	sg_init_one(&output, dst, dlen);

	/* Configure compression request */
	acomp_request_set_params(pcpu->req, &input, &output,
				 PAGE_SIZE, dlen);

	/* Execute compression (synchronous wait for async op) */
	ret = crypto_wait_req(crypto_acomp_compress(pcpu->req),
			      &pcpu->wait);

	end = ktime_get();
	duration = ktime_to_ns(ktime_sub(end, start));

	if (!ret) {
		dlen = pcpu->req->dlen;
		*dst_len = dlen;

		/* Check if compression actually reduced size */
		if (dlen >= PAGE_SIZE) {
			ret = -ENOSPC;
		} else {
			/* Update statistics */
			spin_lock(&shmem_comp_stats_lock);
			shmem_comp_global_stats.avg_compress_time_ns =
				(shmem_comp_global_stats.avg_compress_time_ns +
				 duration) / 2;
			spin_unlock(&shmem_comp_stats_lock);
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
			  const void *src, unsigned int src_len,
			  void *dst)
{
	struct shmem_comp_pcpu *pcpu;
	struct scatterlist input, output;
	ktime_t start, end;
	s64 duration;
	int ret;

	if (!pool || !src || !dst || src_len == 0)
		return -EINVAL;

	pcpu = this_cpu_ptr(pool->pcpu);

	mutex_lock(&pcpu->lock);

	start = ktime_get();

	/* Setup scatterlists for crypto operation */
	sg_init_one(&input, src, src_len);
	sg_init_one(&output, dst, PAGE_SIZE);

	/* Configure decompression request */
	acomp_request_set_params(pcpu->req, &input, &output,
				 src_len, PAGE_SIZE);

	/* Execute decompression */
	ret = crypto_wait_req(crypto_acomp_decompress(pcpu->req),
			      &pcpu->wait);

	end = ktime_get();
	duration = ktime_to_ns(ktime_sub(end, start));

	if (!ret) {
		/* Update statistics */
		spin_lock(&shmem_comp_stats_lock);
		shmem_comp_global_stats.avg_decompress_time_ns =
			(shmem_comp_global_stats.avg_decompress_time_ns +
			 duration) / 2;
		spin_unlock(&shmem_comp_stats_lock);
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

	if (!pool || !data || len == 0 || len > PAGE_SIZE)
		return 0;

	/* Use non-blocking allocation to avoid sleeping */
	gfp = GFP_NOWAIT | __GFP_NOWARN;

	handle = zs_malloc(pool->zs_pool, len, gfp, NUMA_NO_NODE);
	if (!handle)
		return 0;

	zs_obj_write(pool->zs_pool, handle, data, len);

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
	unsigned int obj_size;

	if (!pool || !dst || !len || handle == 0)
		return -EINVAL;

	obj_size = zs_get_object_size(pool->zs_pool, handle);
	if (obj_size > *len)
		return -ENOSPC;

	zs_obj_read(pool->zs_pool, handle, dst, obj_size);
	*len = obj_size;

	return 0;
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
	if (!stats)
		return;

	spin_lock(&shmem_comp_stats_lock);

	*stats = shmem_comp_global_stats;

	/* Calculate compression ratio */
	if (stats->total_uncompressed_bytes > 0) {
		stats->ratio = (stats->total_compressed_bytes * 100) /
			       stats->total_uncompressed_bytes;
	} else {
		stats->ratio = 0;
	}

	spin_unlock(&shmem_comp_stats_lock);
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
		shmem_comp_global_stats.total_compressed_bytes += comp_size;
		shmem_comp_global_stats.total_uncompressed_bytes += orig_size;
	} else {
		if (shmem_comp_global_stats.compressed_pages > 0)
			shmem_comp_global_stats.compressed_pages--;
		if (shmem_comp_global_stats.total_compressed_bytes >= comp_size)
			shmem_comp_global_stats.total_compressed_bytes -= comp_size;
		if (shmem_comp_global_stats.total_uncompressed_bytes >= orig_size)
			shmem_comp_global_stats.total_uncompressed_bytes -= orig_size;
	}

	spin_unlock(&shmem_comp_stats_lock);
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

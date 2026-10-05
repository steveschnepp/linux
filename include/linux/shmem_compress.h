/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SHMEM_COMPRESS_H
#define __LINUX_SHMEM_COMPRESS_H

/*
 * shmem_compress.h - Internal interfaces for tmpfs transparent compression
 *
 * This header defines the internal structures and interfaces used by the
 * tmpfs compression subsystem. The compression feature provides transparent
 * page-level compression for tmpfs files, storing compressed data in
 * zsmalloc pools without requiring upfront RAM reservation.
 *
 * Design Overview:
 * - Pages are compressed using LZ4/LZO via the kernel crypto API
 * - Compressed data is stored in zsmalloc pools (grow on demand)
 * - Page cache xarray stores compressed entry handles instead of folios
 * - Decompression happens transparently on page fault/read access
 *
 * Entry Encoding:
 * Compressed entries are stored in the page cache xarray as xa_value
 * entries, similar to how swap entries are stored. The encoding packs
 * the zsmalloc handle and compressed length into a single unsigned long:
 *
 *   Bit layout (64-bit):
 *   ┌─────────────────────────────────────────────────────────┐
 *   │ 63      40 │ 39                  12 │ 11       0      │
 *   │   unused   │     zs_handle (28)    │  length (12)     │
 *   └─────────────────────────────────────────────────────────┘
 *
 *   - zs_handle: zsmalloc allocation handle (supports up to 256MB pools)
 *   - length: compressed size in bytes (max 4095, fits in 12 bits)
 *
 * Compression Flow:
 *   write() → page dirty → compress_folio() → zs_malloc() → replace xarray
 *
 * Decompression Flow:
 *   read()/fault → xarray lookup → xa_is_shmem_comp() → decompress_folio()
 *
 * See also:
 * - mm/shmem.c (main tmpfs implementation)
 * - mm/zswap.c (similar compression for swap)
 * - mm/zsmalloc.c (compressed data storage)
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/xarray.h>
#include <linux/zsmalloc.h>
#include <crypto/acompress.h>

/**
 * struct shmem_comp_entry - Compressed page entry stored in xarray
 * @handle: zsmalloc allocation handle for compressed data
 * @length: Compressed size in bytes (max 4095)
 * @referenced: Whether entry was recently accessed (for LRU eviction)
 *
 * This structure represents a compressed page in the tmpfs page cache.
 * It is encoded into an xa_value for storage in the xarray.
 */
struct shmem_comp_entry {
	unsigned long handle;		/* zsmalloc handle */
	unsigned int length;		/* compressed size in bytes */
	bool referenced;		/* for LRU eviction tracking */
};

/**
 * struct shmem_comp_stats - Compression statistics
 * @compressed_pages: Number of currently compressed pages
 * @uncompressed_pages: Number of uncompressed pages
 * @total_compressed_bytes: Total bytes used by compressed data
 * @total_uncompressed_bytes: Total bytes before compression
 * @ratio: Compression ratio (percentage * 100)
 * @compress_fails: Failed compression attempts
 * @decompress_fails: Failed decompression attempts
 * @avg_compress_time_ns: Average compression time (nanoseconds)
 * @avg_decompress_time_ns: Average decompression time (nanoseconds)
 *
 * Statistics structure for reporting compression metrics via sysfs.
 */
struct shmem_comp_stats {
	unsigned long compressed_pages;
	unsigned long uncompressed_pages;
	unsigned long total_compressed_bytes;
	unsigned long total_uncompressed_bytes;
	unsigned long ratio;		/* percentage * 100 */
	unsigned long compress_fails;
	unsigned long decompress_fails;
	unsigned long avg_compress_time_ns;
	unsigned long avg_decompress_time_ns;
};

/*
 * Compressed entry encoding/decoding functions.
 *
 * These pack/unpack shmem_comp_entry into xa_value format for storage
 * in the page cache xarray. Similar to swp_to_radix_entry()/radix_to_swp_entry()
 * but for compression entries instead of swap entries.
 */

/**
 * shmem_comp_to_radix_entry - Encode compressed entry as xarray value
 * @entry: Compressed entry to encode
 *
 * Return: xa_value suitable for storing in page cache xarray
 *
 * Packs the zsmalloc handle and compressed length into an xa_value.
 * The handle is shifted left by 12 bits, with length in the lower bits.
 */
static inline void *shmem_comp_to_radix_entry(const struct shmem_comp_entry *entry)
{
	unsigned long val;

	val = (entry->handle << 12) | (entry->length & 0xFFF);
	return xa_mk_value(val);
}

/**
 * shmem_radix_to_comp_entry - Decode xarray value to compressed entry
 * @arg: xa_value from page cache xarray
 *
 * Return: Decoded shmem_comp_entry structure
 *
 * Unpacks an xa_value into a shmem_comp_entry. The referenced flag is
 * set to true by default; callers should clear it if needed.
 */
static inline struct shmem_comp_entry shmem_radix_to_comp_entry(void *arg)
{
	unsigned long val = xa_to_value(arg);
	struct shmem_comp_entry entry;

	entry.handle = val >> 12;
	entry.length = val & 0xFFF;
	entry.referenced = true;	/* default to referenced */
	return entry;
}

/*
 * XArray entry type checking functions.
 *
 * The tmpfs page cache xarray can contain three types of entries:
 * 1. Folio pointers (uncompressed pages) - normal case
 * 2. Swap entries (xa_mk_value with swap type) - existing swap support
 * 3. Compressed entries (xa_mk_value with compression type) - new
 *
 * We use a reserved swap type range to distinguish compressed entries.
 * This avoids needing a separate encoding scheme.
 */

/**
 * shmem_comp_type_check - Check if xa_value is a compression entry
 * @entry: xa_value to check
 *
 * Return: true if entry is a compressed page entry, false otherwise
 *
 * Checks if the xa_value uses the reserved compression type tag.
 * This must be called only after xa_is_value() returns true.
 */
bool shmem_comp_type_check(const void *entry);

/**
 * xa_is_shmem_comp - Check if xarray entry is a compressed page
 * @entry: xarray entry to check
 *
 * Return: true if entry is a compressed page, false otherwise
 *
 * Convenience wrapper that checks both xa_is_value() and compression type.
 */
static inline bool xa_is_shmem_comp(const void *entry)
{
	if (!xa_is_value(entry))
		return false;
	return shmem_comp_type_check(entry);
}

/*
 * Compression pool management.
 *
 * The compression pool manages zsmalloc storage and crypto compression
 * contexts. Each superblock can have its own pool, or share a global pool.
 *
 * Pool lifecycle:
 *   create() → [compress/decompress operations] → destroy()
 *
 * Per-CPU contexts:
 * Each CPU has its own compression context to avoid lock contention.
 * The crypto API requires per-CPU contexts for efficient operation.
 */

/**
 * shmem_comp_pool_create - Create a compression pool
 * @algo: Compression algorithm name ("lz4", "lzo", or "lzo-rle")
 *
 * Return: Pointer to pool on success, ERR_PTR on failure
 *
 * Creates a new compression pool with the specified algorithm.
 * Allocates zsmalloc pool and per-CPU compression contexts.
 *
 * The caller must hold no locks that could conflict with allocation.
 */
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo);

/**
 * shmem_comp_pool_destroy - Destroy a compression pool
 * @pool: Pool to destroy
 *
 * Frees all resources associated with the pool including zsmalloc
 * pool and per-CPU compression contexts. All compressed data is lost.
 *
 * Note: The caller must ensure no other references exist to the pool.
 */
void shmem_comp_pool_destroy(struct shmem_comp_pool *pool);

/**
 * shmem_comp_compress - Compress a page
 * @pool: Compression pool to use
 * @src: Source data (PAGE_SIZE bytes)
 * @dst: Destination buffer for compressed data
 * @dst_len: [in,out] Buffer size on input, compressed size on output
 *
 * Return: 0 on success, negative errno on failure
 *
 * Compresses PAGE_SIZE bytes from src to dst using the pool's algorithm.
 * If compression doesn't reduce size, returns -ENOSPC.
 *
 * Must be called from the CPU that owns the per-CPU context.
 */
int shmem_comp_compress(struct shmem_comp_pool *pool,
			const void *src, void *dst,
			unsigned int *dst_len);

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
 * This is the hot path - must be as fast as possible.
 *
 * Must be called from the CPU that owns the per-CPU context.
 */
int shmem_comp_decompress(struct shmem_comp_pool *pool,
			  const void *src, unsigned int src_len,
			  void *dst);

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
 */
unsigned long shmem_comp_store(struct shmem_comp_pool *pool,
			       const void *data, unsigned int len);

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
 */
int shmem_comp_load(struct shmem_comp_pool *pool,
		    unsigned long handle, void *dst,
		    unsigned int *len);

/**
 * shmem_comp_free - Free compressed data from pool
 * @pool: Compression pool
 * @handle: zsmalloc handle to free
 *
 * Releases the zsmalloc allocation associated with the handle.
 * The handle must not be used after this call.
 */
void shmem_comp_free(struct shmem_comp_pool *pool, unsigned long handle);

/*
 * Statistics functions.
 */

/**
 * shmem_comp_get_stats - Get global compression statistics
 * @stats: Output structure for statistics
 *
 * Populates the stats structure with current compression metrics.
 */
void shmem_comp_get_stats(struct shmem_comp_stats *stats);

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
			      unsigned int comp_size);

/**
 * shmem_comp_pool_get - Get reference to a pool
 * @pool: Pool to reference
 *
 * Increments the pool refcount. Callers must call shmem_comp_pool_put()
 * when done with the pool.
 */
void shmem_comp_pool_get(struct shmem_comp_pool *pool);

/**
 * shmem_comp_pool_put - Release reference to a pool
 * @pool: Pool to release
 *
 * Decrements the pool refcount. If refcount reaches zero, the pool
 * is destroyed.
 */
void shmem_comp_pool_put(struct shmem_comp_pool *pool);

#endif /* __LINUX_SHMEM_COMPRESS_H */

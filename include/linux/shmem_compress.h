/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SHMEM_COMPRESS_H
#define __LINUX_SHMEM_COMPRESS_H

struct shmem_comp_pool;
struct cell_pool;
struct shmem_sb_info;
struct shmem_inode_info;

/*
 * shmem_compress.h - Internal interfaces for tmpfs transparent compression
 *
 * This header defines the internal structures and interfaces used by the
 * tmpfs compression subsystem. The compression feature provides transparent
 * page-level compression for tmpfs files, storing compressed data in
 * zsmalloc pools without requiring upfront RAM reservation.
 *
 * Design Overview:
 * - Pages are compressed via the kernel crypto acomp API (lz4 default,
 *   any installed algorithm selectable per mount)
 * - Compressed data is stored in zsmalloc pools (grow on demand)
 * - The page cache xarray stores xa_value slots carrying a small entry
 *   ID (SWP_SHMEM_COMPRESS type). zsmalloc handles are kernel pointers
 *   and cannot be packed into xarray values, so a global ID table
 *   (struct shmem_comp_entry in mm/shmem_compress.c) maps ID ->
 *   {zsmalloc handle, compressed length, merge size, refcounts}
 * - Decompression happens transparently on page fault/read access
 *
 * Compression Flow:
 *   write() -> compress_folio() -> zs_malloc() -> entry_alloc() -> swap xarray slot
 *
 * Decompression Flow:
 *   read()/fault -> xa_is_shmem_comp() -> entry_get() -> decompress_folio()
 *
 * Entry lifetime:
 *   Each page cache slot holding the ID owns one entry reference;
 *   readers take an additional reference with shmem_comp_entry_get()
 *   and must drop it with shmem_comp_entry_put() after their last use
 *   of the entry-owned data. The entry -- and its zsmalloc block and
 *   merged-group buffer -- is freed when the last reference of either
 *   kind drops. Entries must not outlive their pool: inode eviction
 *   drains every slot before put_super() destroys the pool, and a
 *   reader pin cannot outlive its inode (the open file holds a ref).
 *
 * See also:
 * - mm/shmem.c (main tmpfs implementation)
 * - mm/zswap.c (similar compression for swap)
 * - mm/zsmalloc.c (compressed data storage)
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/swap.h>

struct kobject;

/*
 * Maximum pages per merge group (merge_pages mount option / sysfs).
 *
 * Merged groups live in a kmalloc(N * PAGE_SIZE) buffer (zsmalloc caps
 * objects at PAGE_SIZE), so N * PAGE_SIZE must not exceed KMALLOC_MAX_SIZE
 * (1 << (MAX_PAGE_ORDER + PAGE_SHIFT) = 4MiB on x86_64 with the default
 * MAX_PAGE_ORDER of 10). 1024 pages = 4MiB is therefore the hard ceiling:
 * larger values would make every group buffer allocation fail and silently
 * disable compression for the mount.
 */
#define SHMEM_COMP_MERGE_MAX	1024
#include <linux/xarray.h>
#include <linux/zsmalloc.h>
#include <crypto/acompress.h>

/**
 * struct shmem_comp_entry - Compressed page metadata
 * @handle: zsmalloc allocation handle for compressed data (a kernel
 *          pointer, must not be packed into the xarray value)
 * @length: Compressed size in bytes
 * @nr_pages: number of consecutive pages merged into this block
 *            (1 = single page, >1 = merge_pages group)
 * @refs: liveness count = page cache slots + reader pins; the entry
 *        (and its zsmalloc block / merged buffer) is freed when this
 *        drops to zero, whichever reference kind drops it
 * @slots: page cache slots only; gates the once-per-group accounting
 *         in shmem_free_swap() (last slot, not last reference)
 * @data: kmalloc'd block for merged groups: zsmalloc caps objects at
 *        PAGE_SIZE, so blocks larger than that live here; freed with
 *        the entry
 * @pool: pool that owns @handle; the entry frees the zsmalloc block
 *        itself when the last reference drops
 * @rcu: for kfree_rcu() after removal from the ID table
 *
 * Registered in a global ID table; the page cache xarray only stores
 * the small ID via make_shmem_comp_entry().
 */
struct shmem_comp_entry {
	/*
	 * Storage location.  For zsmalloc-backed pools this is a
	 * zs_malloc() handle.  For cell-backed pools it encodes
	 * {cellid, offset} — the two fields fit in one unsigned long
	 * because cellid is 16 bits and offset is bounded by the cell
	 * size (max 4 MiB = 22 bits).  Reusing the handle field avoids
	 * growing every entry by the size of a second pointer, which
	 * matters because the entry table is proportional to the number
	 * of compressed blocks in the system.
	 *
	 * pool->use_cells determines which interpretation applies.  A
	 * pool never switches backing store after creation, so the
	 * interpretation is stable for the lifetime of the entry.
	 */
	unsigned long handle;
	unsigned int length;		/* compressed size in bytes */
	unsigned short nr_pages;	/* pages merged into this block */
	/*
	 * Two-counter reference scheme:
	 *   slots  — page-cache xarray slots that point at this entry.
	 *            Decrement when a slot is cleared (truncate,
	 *            decompress-after-read, file deletion).
	 *   refs   — slots plus temporary pins taken by readers during
	 *            decompression.  The entry (and its storage) is
	 *            freed when refs reaches zero.
	 *
	 * The separation exists because a reader must pin the entry
	 * before loading from storage, but the page-cache slot may be
	 * cleared concurrently by truncate.  If a single counter were
	 * used, truncate could free the entry while a reader is mid-
	 * decompress.  With two counters, truncate decrements only
	 * slots; the reader's pin on refs keeps the entry alive until
	 * the decompress completes.
	 */
	atomic_t refs;
	atomic_t slots;
	/*
	 * Merged-group zsmalloc chunks.  When merge_pages > 1 and the
	 * pool is zsmalloc-backed, the compressed group is split into
	 * PAGE_SIZE chunks, each stored as a separate zs_malloc()
	 * object.  @handles is a kmalloc'd array of those handles,
	 * owned by the entry and freed with it.  Cell-backed pools
	 * store merged groups as a single block, so handles/nchunks
	 * are unused (NULL/0) in that case.
	 */
	unsigned long *handles;
	unsigned short nchunks;
	/*
	 * The pool that owns the storage.  Needed at free time to
	 * dispatch to the correct release function (zs_free vs
	 * cell_free) and to look up the compression context for
	 * decompression.
	 */
	struct shmem_comp_pool *pool;
	struct rcu_head rcu;
};

/*
 * Encode/decode a cell location into the handle field.
 *
 * cellid is biased by +1 so that the first object of the first cell
 * (cellid=0, offset=0) never encodes to handle 0.  Handle 0 is the
 * universal "storage failed" sentinel in the compression paths —
 * without the bias, that object would be silently discarded on every
 * fresh pool.
 *
 * Lossless because cellid never exceeds 16 bits (max_cells is 256)
 * and offset never exceeds 22 bits (max cell size is 4 MiB); the
 * biased cellid fits comfortably in the upper 32 bits.
 */
static inline unsigned long shmem_comp_pack_loc(u16 cellid, u32 offset)
{
	return ((unsigned long)(cellid + 1) << 32) | offset;
}

static inline void shmem_comp_unpack_loc(unsigned long handle,
					 u16 *cellid, u32 *offset)
{
	*cellid = (handle >> 32) - 1;
	*offset = handle & 0xFFFFFFFFUL;
}

/**
 * struct shmem_comp_stats - Compression statistics
 * @compressed_pages: Number of currently compressed pages
 * @uncompressed_pages: Number of uncompressed pages
 * @total_compressed_bytes: Total bytes used by compressed data
 * @total_uncompressed_bytes: Total bytes before compression
 * @ratio: Compression ratio (percentage * 100)
 * @compress_fails: Failed compression attempts
 * @decompress_fails: Failed decompression attempts
 * @ram_pool_bytes: RAM held by zsmalloc pools (live zspage pages)
 * @ram_merged_bytes: RAM held by merged-group kmalloc buffers
 * @ram_entry_bytes: RAM held by live ID-table entries (slab sizes)
 * @ram_entries: Number of live ID-table entries
 * @ram_total_bytes: Sum of pool + merged + entry RAM
 *
 * The ram_* fields report what the compression subsystem currently
 * occupies in RAM, as opposed to total_*_bytes which track cumulative
 * data flow. ram_total_bytes vs the uncompressed size of the same data
 * is the honest measure of whether compression saves RAM: for merged
 * groups the kmalloc buffer is allocated at full group size and never
 * shrunk to comp_len, so ram_merged_bytes can approach the uncompressed
 * size even when the compressed data is small.
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
	/* Live RAM footprint (see kerneldoc above) */
	unsigned long ram_pool_bytes;
	unsigned long ram_merged_bytes;
	unsigned long ram_entry_bytes;
	unsigned long ram_entries;
	unsigned long ram_total_bytes;
};

/*
 * Compressed entry xarray representation.
 *
 * The page cache xarray stores only a small entry ID, encoded via
 * make_shmem_comp_entry()/shmem_comp_entry_id() in <linux/swapops.h>.
 * The zsmalloc handle lives in the ID table (struct shmem_comp_entry)
 * because handles are kernel pointers and do not fit xarray values.
 */

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
 * Per-mount compression state and parsed mount options (carve C3/C4).
 *
 * shmem_comp_fill_sb() creates a struct shmem_comp_sb for mounts with
 * compression enabled; shmem_comp_put_super() frees it. It hangs off
 * the single pointer shmem_sb_info.comp (NULL = uncompressed mount),
 * so mm/shmem.c never touches compression internals directly.
 *
 * Ownership rules:
 *  - opts->compressor is owned by the opts (freed by opts_free());
 *    fill_sb() copies it out with shmem_comp_resolve_algo(), so the
 *    fs_context can outlive a failed mount.
 *  - comp->compressor is owned by the comp and freed by put_super().
 *  - fill_sb() failure before pool creation fails the mount (-ENOMEM);
 *    pool-creation failure is graceful: the mount runs uncompressed.
 *
 * The shmem_comp_sb definition is visible here only while the
 * compression implementation block still lives in mm/shmem.c (carve
 * C1). Once it moves into mm/shmem_compress.c the definition becomes
 * private to that file.
 */
struct shmem_comp_sb {
	bool enabled;
	char *compressor;		/* owned; freed with the sb */
	unsigned int threshold;		/* min file size to compress, bytes */
	unsigned int merge;		/* pages per compression group */
	unsigned int cell_order;	/* cell storage order, informational */
	struct shmem_comp_pool *pool;	/* compression pool */
	atomic_long_t pages;		/* compressed pages (this mount) */
	atomic_long_t bytes;		/* compressed bytes stored (this mount) */
};

/*
 * Parsed compression mount options: compress=, nocompress,
 * compress_threshold=, merge_pages=, cell=. "Unset" means zero/NULL;
 * shmem_comp_fill_sb() substitutes the sysfs/Kconfig defaults for
 * those. threshold-0-means-default is the one quirk: there is no way
 * to ask for "compress everything" other than threshold=1.
 */
struct shmem_comp_opts {
	bool compress;			/* false = nocompress / compress=none */
	char *compressor;		/* algorithm, NULL = default */
	unsigned int threshold;		/* bytes; 0 = default */
	unsigned int merge;		/* pages; 0 = sysfs default */
	unsigned int cell_order;	/* 0 = derive from tmpfs size */
	bool cell_seen;			/* cell= given (incl. cell=0 opt-out) */
};

void shmem_comp_opts_init(struct shmem_comp_opts *opts);
void shmem_comp_opts_free(struct shmem_comp_opts *opts);

/*
 * Mount-option parsing helpers, called from shmem_parse_one() so the
 * option ABI stays greppable in shmem_fs_parameters while the logic
 * lives here. set_algo() consumes @algo on success (stores it, or
 * frees it for "none"/empty); on -EINVAL @algo is untouched and the
 * caller frees it.
 */
int shmem_comp_opts_set_algo(struct shmem_comp_opts *opts, char *algo);
int shmem_comp_opts_set_cell(struct shmem_comp_opts *opts, const char *val);

/*
 * fill_sb: resolve the algorithm (mount option, else sysfs default,
 * else Kconfig default), derive the cell order from @tmpfs_bytes when
 * cell= was not given, and create the pool. Returns 0 on success (also
 * when the mount runs uncompressed), -ENOMEM when the algorithm string
 * cannot be allocated (mount fails).
 *
 * put_super: release the pool and free the comp; safe to call on a
 * partially initialized or NULL comp (mount-failure path).
 */
int shmem_comp_fill_sb(struct shmem_sb_info *sbinfo,
		       struct shmem_comp_opts *opts,
		       unsigned long tmpfs_bytes);
void shmem_comp_put_super(struct shmem_sb_info *sbinfo);

/*
 * Write-path hook (shmem_write_end): true when this inode's mount has
 * an enabled pool and the file is at/over the compression threshold.
 */
bool shmem_comp_should_compress(struct inode *inode);

/*
 * Eviction hook (shmem_free_swap): release one compressed page-cache
 * slot through the entry ID table. Compression entries are not swap
 * entries; routing them through the swap code WARNs and leaks the
 * compressed block. The xarray erase itself stays in shmem_free_swap()
 * (under the xas lock); this only drops the slot reference and updates
 * per-mount accounting.
 */
void shmem_comp_free_swap(struct address_space *mapping, void *radswap);

/*
 * Compression data path (mm/shmem_compress.c), called from the three
 * dispatch sites in mm/shmem.c (write_end, get_folio_gfp) and from the
 * eviction hook.
 */
int shmem_compress_folio(struct inode *inode, pgoff_t index,
			 struct folio *folio);
int shmem_decompress_folio(struct inode *inode, pgoff_t index,
			   swp_entry_t comp_entry, gfp_t gfp,
			   struct folio **foliop);

/*
 * shmem primitives exported for the tmpfs compression backend
 * (mm/shmem_compress.c). Exported inside CONFIG_TMPFS_COMPRESSION so
 * no new API exists when the config is off.
 */
bool shmem_recalc_inode(struct inode *inode, long alloced, long swapped);
int shmem_add_to_page_cache(struct folio *folio,
			    struct address_space *mapping,
			    pgoff_t index, void *shadow, gfp_t gfp);
void shmem_delete_from_page_cache(struct folio *folio, void *radswap);
struct folio *shmem_alloc_folio(gfp_t gfp, int order,
				struct shmem_inode_info *info,
				pgoff_t index);

/*
 * Global compression defaults and sysfs interface.
 *
 * The defaults (enabled, compressor, merge_pages) apply to tmpfs mounts
 * that do not override them with mount options; storage and the sysfs
 * attribute handlers live in mm/shmem_compress.c. Guarded by
 * shmem_comp_defaults_lock because the sysfs handlers replace/free the
 * values at any time.
 */

/*
 * Sysfs registration: creates the "compression" attribute group under
 * the parent kobject (the tmpfs kobject, from tmpfs_sysfs_init()).
 * Registration is best-effort: tmpfs keeps working without the group.
 */
int shmem_comp_sysfs_register(struct kobject *parent);
void shmem_comp_sysfs_unregister(void);

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
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo,
						unsigned int cell_order);

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

/*
 * Accessors for pool storage-backend queries.  The struct
 * shmem_comp_pool definition is private to shmem_compress.c; these
 * let callers dispatch without pulling zsmalloc/cell headers into
 * every translation unit.
 */
bool shmem_comp_pool_uses_cells(struct shmem_comp_pool *pool);
struct cell_pool *shmem_comp_pool_cells(struct shmem_comp_pool *pool);
unsigned int shmem_comp_pool_cell_pages(struct shmem_comp_pool *pool);

/*
 * Pinned, relocation-safe block load for cell storage.  Retries if
 * GC moves the block mid-read; see implementation for the protocol.
 */
int shmem_comp_load_entry(unsigned int id, void *dst, unsigned int *len);

/* Debug hook: run one async GC pass on every cell-backed pool. */
void shmem_comp_cell_gc_all(void);

/**
 * shmem_comp_compress - Compress a block of memory
 * @pool: Compression pool to use
 * @src: Source data
 * @slen: Source length in bytes (PAGE_SIZE, or a merge_pages multiple)
 * @dst: Destination buffer for compressed data
 * @dst_len: [in,out] buffer capacity on input, compressed size output
 *
 * Return: 0 on success, -ENOSPC if compression does not reduce size,
 *         other negative errno on failure
 *
 * Must be called from the CPU that owns the per-CPU context.
 */
int shmem_comp_compress(struct shmem_comp_pool *pool,
			const void *src, unsigned int slen,
			void *dst, unsigned int *dst_len);

/**
 * shmem_comp_decompress - Decompress a block of memory
 * @pool: Compression pool to use
 * @src: Compressed source data
 * @slen: Compressed data size
 * @dst: Destination buffer
 * @dst_len: [in,out] buffer capacity input, decompressed size output
 *
 * Return: 0 on success, negative errno on failure
 *
 * This is the hot path - must be as fast as possible.
 * Must be called from the CPU that owns the per-CPU context.
 */
int shmem_comp_decompress(struct shmem_comp_pool *pool,
			  const void *src, unsigned int slen,
			  void *dst, unsigned int *dst_len);

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
 * shmem_comp_store_merged - Store compressed data as zsmalloc chunks
 * @pool: Compression pool
 * @data: Compressed data to store
 * @len: Data length in bytes (may exceed PAGE_SIZE)
 * @handles_out: Output array of zsmalloc handles (caller frees)
 * @nchunks_out: Output number of chunks
 *
 * Return: 0 on success, negative errno on failure.
 *
 * Splits @data into chunks of at most PAGE_SIZE bytes and stores each
 * chunk as a separate zsmalloc object. Fills @handles_out (kmalloc'd
 * array of nchunks handles) and @nchunks_out. On failure, frees any
 * partially stored chunks and returns an error.
 */
int shmem_comp_store_merged(struct shmem_comp_pool *pool,
			    const void *data, unsigned int len,
			    unsigned long **handles_out,
			    unsigned short *nchunks_out);

/**
 * shmem_comp_load_merged - Load compressed data from zsmalloc chunks
 * @pool: Compression pool
 * @handles: Array of zsmalloc handles
 * @nchunks: Number of handles
 * @dst: Destination buffer
 * @len: [in,out] Buffer size on input, data size on output
 *
 * Return: 0 on success, negative errno on failure.
 *
 * Loads all chunks and concatenates them into @dst.
 */
int shmem_comp_load_merged(struct shmem_comp_pool *pool,
			   const unsigned long *handles,
			   unsigned short nchunks,
			   void *dst, unsigned int *len);

/**
 * shmem_comp_free_merged - Free zsmalloc chunks
 * @pool: Compression pool
 * @handles: Array of zsmalloc handles
 * @nchunks: Number of handles
 *
 * Frees all zsmalloc chunks and the handle array itself.
 */
void shmem_comp_free_merged(struct shmem_comp_pool *pool,
			    unsigned long *handles,
			    unsigned short nchunks);

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
 * Compression entry ID table.
 *
 * The page cache xarray can only store small integer values, but
 * zsmalloc handles are kernel pointers. Entries are registered here
 * and referenced by ID from the page cache.
 */

/**
 * shmem_comp_entry_alloc - Register compressed data, get an entry ID
 * @pool: pool that owns @handle (the entry frees the zsmalloc block
 *        itself when its last reference drops)
 * @handle: zsmalloc handle from shmem_comp_store() (0 for merged groups)
 * @length: compressed data length in bytes
 * @nr_pages: pages merged into this block (1 for single pages)
 * @data: kmalloc'd compressed block for merged groups (owned by the
 *        entry, freed with the entry); NULL for single pages
 *
 * Return: entry ID (>0) on success, 0 on allocation failure
 *
 * The ID is stored once per page cache slot of the group via
 * make_shmem_comp_entry(); refs and slots are both initialised to
 * @nr_pages. Each slot is released with shmem_comp_entry_release().
 */
unsigned int shmem_comp_entry_alloc(struct shmem_comp_pool *pool,
				    unsigned long handle, unsigned int length,
				    unsigned short nr_pages,
				    unsigned long *handles,
				    unsigned short nchunks);

/**
 * shmem_comp_entry_get - Pin an entry and copy its fields out
 * @id: entry ID
 * @handle: output, zsmalloc handle (0 for merged groups), may be NULL
 * @length: output, compressed length in bytes, may be NULL
 * @nr_pages: output, pages merged into this block (1 = single), may be NULL
 * @data: output, entry-owned merged-group buffer, may be NULL
 *
 * Return: true if the entry was pinned
 *
 * Takes a reference on the entry so the caller may use the returned
 * handle/data outside RCU: truncate can erase the caller's page cache
 * slot at any point, and the last slot release frees the entry (and
 * its data) immediately. Every successful get() must be paired with
 * exactly one shmem_comp_entry_put() after the caller's last use.
 * Fails when the entry is already dying (refs == 0); the caller
 * should treat that as "entry vanished" and retry its lookup.
 */
bool shmem_comp_entry_get(unsigned int id, unsigned long *handle,
			  unsigned int *length, unsigned int *nr_pages,
			  unsigned long **handles,
			  unsigned short *nchunks);

/**
 * shmem_comp_entry_put - Drop a reference taken by shmem_comp_entry_get()
 * @id: entry ID
 *
 * The entry (and its zsmalloc block / merged buffer) is freed when the
 * last reference of any kind -- page cache slot or reader pin -- drops.
 */
void shmem_comp_entry_put(unsigned int id);

/**
 * shmem_comp_entry_release - Drop one page cache slot's reference
 * @id: entry ID
 *
 * Return: true if this was the last slot
 *
 * Each page cache slot holding the ID calls this once when the slot is
 * replaced by a decompressed folio or erased by truncate/eviction. The
 * return value gates the once-per-group accounting in shmem_free_swap();
 * the entry itself is freed only when the last reference of any kind
 * (slot or reader pin) drops -- by whichever of release()/put() gets
 * there first.
 */
bool shmem_comp_entry_release(unsigned int id);

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
int shmem_comp_cell_stats_show(char *buf, size_t size);

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
 * shmem_comp_pool_put - Release reference to a pool
 * @pool: Pool to release
 *
 * Decrements the pool refcount. If refcount reaches zero, the pool
 * is destroyed.
 */
void shmem_comp_pool_put(struct shmem_comp_pool *pool);

#endif /* __LINUX_SHMEM_COMPRESS_H */

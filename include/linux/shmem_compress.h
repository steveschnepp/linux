/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SHMEM_COMPRESS_H
#define __LINUX_SHMEM_COMPRESS_H

struct shmem_comp_pool;
struct cell_pool;
struct shmem_sb_info;
struct shmem_inode_info;
struct seq_file;

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
 * - Each page's compressed backing lives in a per-inode entries[] table
 *   (an xarray whose slots carry the entry POINTER); the entry
 *   (struct shmem_comp_entry in mm/shmem_compress.c) holds the
 *   zsmalloc handle / packed cell location, compressed length, group
 *   size and refcounts, and is also registered in a global ID table
 *   for GC and teardown bookkeeping
 * - Decompression happens transparently on page fault/read access
 *
 * Compression Flow:
 *   write() -> compress_folio() -> zs_malloc() -> entry_alloc() -> entries[index]
 *
 * Decompression Flow:
 *   read()/fault -> entries[index] -> entry_get() -> decompress_folio()
 *
 * Entry lifetime:
 *   Each page cache slot naming the entry owns one durable reference
 *   (claimed on install, dropped via the slots[] erase); readers take
 *   an additional pin under rcu_read_lock() and must drop it with
 *   shmem_comp_entry_put() after their last use of the entry-owned
 *   data. The entry -- and its zsmalloc block and merged-group buffer
 *   -- is freed when the last reference of either kind drops. Entries must not outlive their pool: inode eviction
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
 * Registered in a global ID table keyed by @id (GC bookkeeping,
 * teardown); the per-inode entries[] table stores the entry POINTER
 * directly -- RCU plus the slot-claim protocol replace the old packed
 * {id, generation} xa_value, which could not fit a 32-bit xa slot.
 */
struct shmem_comp_entry {
	/*
	 * Storage location.  For zsmalloc-backed pools this is a
	 * zs_malloc() handle (widened to u64; the kernel API hands out
	 * unsigned long, which fits into u64 on every arch).  For
	 * cell-backed pools it encodes {cellid, offset} -- cellid+1 at
	 * bit 24 (40 bits) and offset in the low 24 bits (max cell is
	 * 16 MiB, CELL_ORDER_MAX), so the encoding needs all 64 bits on
	 * EVERY arch.  Fixed-width (u64), never unsigned long: on
	 * 32-bit unsigned long silently truncates cellid to 8 bits
	 * (wrong cell, corrupt read, no compiler warning).  Reusing the
	 * handle field avoids growing every entry by the size of a
	 * second pointer, which matters because the entry table is
	 * proportional to the number of compressed blocks in the
	 * system.
	 *
	 * pool->use_cells determines which interpretation applies.  A
	 * pool never switches backing store after creation, so the
	 * interpretation is stable for the lifetime of the entry.
	 */
	u64 handle;
	/*
	 * The xarray index this entry occupies in shmem_comp_entries,
	 * assigned by xa_alloc().  Stable for the entry's life (a new
	 * entry gets a fresh xa_alloc).  The GC drain walks the reverse
	 * index by pointer and needs the id to drop its pin / drive the
	 * refs==0 teardown without an xa_load per entry.
	 */
	unsigned int id;
	unsigned int length;		/* compressed size in bytes */
	unsigned short nr_pages;	/* pages merged into this block */
	/*
	 * Two-counter reference scheme:
	 *   slots  — entries[] slots (the per-inode two-table mapping)
	 *            that name this entry; the durable reference.  One per
	 *            entries[] slot (a merge group writes one per page).
	 *            Decrement when a slot is cleared (truncate,
	 *            write-invalidate, file deletion).
	 *   refs   — slots plus temporary pins taken by readers during
	 *            decompression.  The entry (and its storage) is
	 *            freed when refs reaches zero.
	 *
	 * The separation exists because a reader must pin the entry
	 * before loading from storage, but an entries[] slot may be
	 * cleared concurrently by truncate.  If a single counter were
	 * used, truncate could free the entry while a reader is mid-
	 * decompress.  With two counters, truncate decrements only
	 * slots; the reader's pin on refs keeps the entry alive until
	 * the decompress completes.  Decompress does NOT release a slot:
	 * the entry is kept as the durable backing of the uncompressed
	 * folio cache (the "both" state), so reclaim can drop the folio
	 * and a later read re-decompresses.
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
	u64 *handles;
	unsigned short nchunks;
	/*
	 * The pool that owns the storage.  Needed at free time to
	 * dispatch to the correct release function (zs_free vs
	 * cell_free) and to look up the compression context for
	 * decompression.
	 */
	struct shmem_comp_pool *pool;
	/*
	 * Reverse-index linkage: which cell holds this block.  For
	 * cell-backed pools only (zsmalloc entries leave it unlinked).
	 * The block lives on pool->cell_index[cellid] for the whole time
	 * its handle names that cell; alloc links it, free unlinks it,
	 * GC relocate moves it.  Guarded by pool->index_lock.  The 16 B
	 * cost is the price of O(entries-in-victim) GC instead of an
	 * O(total-entries) table scan; see specs/04_GC_REVERSE_INDEX.md.
	 */
	struct list_head cell_node;
	struct rcu_head rcu;
};

/*
 * Encode/decode a cell location into the handle field.
 *
 * The offset field is 24 bits because a valid offset is always
 * < cell_size and the largest cell is 16 MiB (CELL_ORDER_MAX); the
 * cellid field takes the remaining 40 bits (u32 + the +1 bias).
 */
#define CELL_LOC_OFFSET_BITS	24
#define CELL_LOC_OFFSET_MAX	(1ULL << CELL_LOC_OFFSET_BITS)

/*
 * Encode/decode a cell location into the handle field.
 *
 * cellid is biased by +1 so that the first object of the first cell
 * (cellid=0, offset=0) never encodes to handle 0.  Handle 0 is the
 * universal "storage failed" sentinel in the compression paths —
 * without the bias, that object would be silently discarded on every
 * fresh pool.
 *
 * Layout: [cellid+1, 40 bits at 24][offset, 24 bits].
 * offset never exceeds 24 bits (max cell size is 16 MiB, so a valid
 * offset is < 2^24); cellid+1 takes the remaining 40 bits, more than
 * enough for a u32 cellid (a directory can hold up to UINT_MAX cells,
 * though the kvcalloc fails long before that).  The handle is a full
 * u64 stored in entry.handle -- not a swp_entry -- so all 64 bits are
 * available on every arch (the per-inode entries[] table stores the
 * entry POINTER, so it never competes for bits here).
 *
 * When widening or narrowing the fields, re-price THIS encoding, not
 * the struct cell declaration: the u16 cellid wall was never in the
 * type, it was here.
 */
static inline u64 shmem_comp_pack_loc(u32 cellid, u32 offset)
{
	return (((u64)cellid + 1) << CELL_LOC_OFFSET_BITS) |
		(offset & (CELL_LOC_OFFSET_MAX - 1));
}

static inline void shmem_comp_unpack_loc(u64 handle,
					 u32 *cellid, u32 *offset)
{
	*cellid = (handle >> CELL_LOC_OFFSET_BITS) - 1;
	*offset = handle & (CELL_LOC_OFFSET_MAX - 1);
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
 * The compressed mapping lives in the per-inode entries[] table (a
 * dedicated xarray whose slots hold entry pointers), not in the page
 * cache, so no reserved swap type or page-cache type tag is needed.
 * See mm/shmem_compress.c for the entry table and the pointer/RCU
 * identity protocol.
 */

/*
 * Per-mount compression state and parsed mount options (carve C3/C4).
 *
 * shmem_comp_fill_sb() creates the per-mount compression state for
 * mounts with compression enabled; shmem_comp_put_super() frees it. It
 * hangs off the single pointer shmem_sb_info.comp (NULL = uncompressed
 * mount), so mm/shmem.c never touches compression internals directly.
 *
 * The definition of struct shmem_comp_sb is private to
 * mm/shmem_compress.c (carve C4); only the pointer crosses into
 * shmem_fs.h.
 *
 * Ownership rules:
 *  - opts->compressor is owned by the opts (freed by opts_free());
 *    fill_sb() copies it out with shmem_comp_resolve_algo(), so the
 *    fs_context can outlive a failed mount.
 *  - comp->compressor is owned by the comp and freed by put_super().
 *  - fill_sb() failure before pool creation fails the mount (-ENOMEM);
 *    pool-creation failure is graceful: the mount runs uncompressed.
 */
struct shmem_comp_sb;

/*
 * Why a mount runs uncompressed (option surface v2): the resolved-state
 * line at mount must say WHY, not just that compression is off.
 */
enum shmem_comp_off_reason {
	SHMEM_COMP_OFF_DEFAULT = 0,	/* participation default off (sysfs) */
	SHMEM_COMP_OFF_NOFLAG,		/* nocompress */
	SHMEM_COMP_OFF_ALGO_NONE,	/* compress=none (or default algo none) */
	SHMEM_COMP_OFF_WATERMARK,	/* compress_watermark=100:100 */
};

/*
 * Parsed compression mount options: compress=, nocompress,
 * compress_threshold=, compress_watermark=, merge_pages=, cell=.
 * "Unset" means zero/NULL; shmem_comp_fill_sb() substitutes the
 * sysfs/Kconfig defaults for those. threshold_bytes takes memparse
 * syntax at mount ("64K"); 0 means "use the default" -- there is no
 * way to ask for "compress everything" other than
 * threshold_bytes=1. The threshold applies to WRITE-TIME compression
 * only; the aging pass (Stage 2) compresses any eligible survivor
 * >= 1 page.
 *
 * watermark_{low,high} is the WHEN layer (compress_watermark=LOW:HIGH,
 * percents, low <= high; 0:0 = always/eager, 100:100 = never).
 * watermark_seen distinguishes "explicit 0:0" from "not given":
 * fill_sb() takes the sysfs default only when !seen, so a mount
 * asking for 0:0 never silently receives the global default.
 */
struct shmem_comp_opts {
	bool compress;			/* false = nocompress / compress=none */
	enum shmem_comp_off_reason off_reason;	/* why compress is false */
	char *compressor;		/* algorithm, NULL = default */
	unsigned int threshold_bytes;		/* write-time min file size, bytes; 0 = default */
	unsigned int watermark_low;		/* WHEN: chill return, percent usage */
	unsigned int watermark_high;		/* WHEN: hot compress, percent usage */
	bool watermark_seen;			/* compress_watermark= given */
	unsigned int merge_pages;		/* pages; 0 = sysfs default */
	unsigned int cell_order;	/* 0 = derive from tmpfs size */
	bool cell_seen;			/* cell= given (incl. cell=0 opt-out) */
	unsigned int gc_percent;	/* cell GC sparse threshold, percent byte
					 * utilization; 0 = default (50) */
	bool gc_percent_seen;		/* gc_percent= given */
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
int shmem_comp_opts_set_gc_percent(struct shmem_comp_opts *opts,
				   const char *val);

/*
 * Parse the compress_watermark=LOW:HIGH mount option (percents,
 * low <= high). Return: 0 on success, -EINVAL on a bad value (mount
 * fails).
 */
int shmem_comp_opts_set_watermark(struct shmem_comp_opts *opts, const char *val);

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
int shmem_comp_fill_sb(struct super_block *sb,
		       struct shmem_sb_info *sbinfo,
		       struct shmem_comp_opts *opts,
		       unsigned long tmpfs_bytes);
void shmem_comp_put_super(struct shmem_sb_info *sbinfo);

/*
 * show_options: print the resolved per-mount compression config into
 * /proc/mounts (called from shmem_show_options()). Uncompressed
 * mounts print ",nocompress" -- /proc/mounts must tell the truth.
 */
void shmem_comp_show_options(struct seq_file *seq,
			     struct shmem_sb_info *sbinfo);

/*
 * Write-path hook (shmem_write_end): evaluates the hot/chill flip
 * against RAM usage (shmem_comp_wm_update, specs/06), then returns
 * true when this inode's mount has an enabled pool, the mount is in
 * hot mode, and the file is at/over the compression threshold
 * (threshold_bytes). In chill mode this returns false and the write
 * lands uncompressed (the maintenance pass ages it later).
 */
bool shmem_comp_should_compress(struct inode *inode);

/*
 * Truncate hook (shmem_undo_range): record an uncompressed folio
 * leaving the page cache while the mount is chilled -- a page that
 * died without paying a compression cycle (death-before-pop's saving).
 */
void shmem_comp_note_unmap(struct inode *inode);

/*
 * Two-table mapping (specs/02): a compressed page's entry id lives in
 * the per-inode entries[] xarray (same pgoff index space, 0/NULL = no
 * compressed copy), NOT in the page cache.  i_pages[] holds only folios.
 * The entries[] slot owns the entry's durable reference.
 */

/*
 * Quick presence test: true if entries[index] names a compressed entry.
 * Used by the read path to distinguish a compressed page (i_pages empty,
 * entries[] set) from a genuine hole (both empty).  No reference is taken.
 */
bool shmem_comp_entries_present(struct inode *inode, pgoff_t index);

/*
 * Truncate hook (shmem_undo_range): clear and free every compressed
 * entry in [start, end).  Drops each entry's durable reference (driving
 * teardown when the last reference -- entry or reader pin -- goes),
 * restores the per-mount compression accounting, and returns the number
 * of logical pages whose entries were freed so the caller can drop the
 * shmem swapped charge in one recalc.  Safe on an absent table.
 */
unsigned int shmem_comp_truncate_range(struct inode *inode, pgoff_t start,
				       pgoff_t end);

/*
 * Write-invalidate hook (shmem_write_begin): a write to a folio with a
 * surviving compressed backing (the "both" state) makes it stale; free it
 * so the folio becomes the sole copy, keeping "a dirty folio has no
 * compressed backing".  Process context only (i_rwsem held).
 */
void shmem_comp_invalidate_write(struct inode *inode, pgoff_t index);

/*
 * Eviction hook (shmem_evict_inode): free any remaining entries[] and
 * the table itself (the two-table mapping, specs/02).  Safe on NULL
 * (most inodes never compress).  shmem_comp_truncate_range() has
 * normally already emptied the table; this is the safety net that
 * guarantees no compressed block outlives its inode.
 */
void shmem_comp_entries_destroy(struct inode *inode);

/*
 * Compression data path (mm/shmem_compress.c), called from the three
 * dispatch sites in mm/shmem.c (write_end, get_folio_gfp) and from the
 * eviction hook.
 */
int shmem_compress_folio(struct inode *inode, pgoff_t index,
			 struct folio *folio);
int shmem_decompress_folio(struct inode *inode, pgoff_t index,
			   gfp_t gfp, struct folio **foliop);

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
 * @cell_order: Cell size as alloc_pages order, or 0 for zsmalloc
 * @nr_cells: Cell directory capacity hint, or 0 for the default
 *
 * Return: Pointer to pool on success, ERR_PTR on failure
 *
 * Creates a new compression pool with the specified algorithm.
 * Allocates zsmalloc pool and per-CPU compression contexts.
 *
 * @nr_cells bounds the physical storage of a cell pool; a caller
 * that knows its worst case passes it (plus headroom), a caller
 * that does not passes 0.  See cell_pool_create().
 *
 * The caller must hold no locks that could conflict with allocation.
 */
struct shmem_comp_pool *shmem_comp_pool_create(const char *algo,
						unsigned int cell_order,
						unsigned int nr_cells,
						unsigned int gc_percent);

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
unsigned long shmem_comp_pool_gc_recoveries(struct shmem_comp_pool *pool);
unsigned int shmem_comp_pool_cell_pages(struct shmem_comp_pool *pool);

/*
 * Pinned, relocation-safe block load for cell storage, on an entry
 * the caller has already pinned.  Retries if GC moves the block
 * mid-read; see implementation for the protocol.
 */
int shmem_comp_load_entry(struct shmem_comp_entry *ent, void *dst,
			  unsigned int *len);

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
u64 shmem_comp_store(struct shmem_comp_pool *pool,
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
		    u64 handle, void *dst,
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
			    u64 **_out,
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
			   const u64 *handles,
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
			    u64 *,
			    unsigned short nchunks);

/**
 * shmem_comp_free - Free compressed data from pool
 * @pool: Compression pool
 * @handle: zsmalloc handle to free
 *
 * Releases the zsmalloc allocation associated with the handle.
 * The handle must not be used after this call.
 */
void shmem_comp_free(struct shmem_comp_pool *pool, u64 handle);

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
 * Return: the entry (pinned by its initial refs == slots ==
 * @nr_pages), or NULL on allocation failure
 *
 * The caller installs the returned pointer once per entries[] slot of
 * the group (shmem_comp_entries_set(); each install consumes one of
 * the @nr_pages durable references). Each slot is released with
 * shmem_comp_entry_release() on the entry claimed from that slot.
 */
struct shmem_comp_entry *
shmem_comp_entry_alloc(struct shmem_comp_pool *pool, u64 handle,
		       unsigned int length, unsigned short nr_pages,
		       u64 *handles, unsigned short nchunks);

/**
 * shmem_comp_entry_put - Drop a reader pin taken on an entry
 * @ent: the pinned entry (NULL is a no-op)
 *
 * The entry (and its storage) is freed when the last reference of any
 * kind -- page cache slot or reader pin -- drops.
 */
void shmem_comp_entry_put(struct shmem_comp_entry *ent);

/**
 * shmem_comp_entry_release - Drop a claimed page cache slot's reference
 * @ent: entry claimed from the slot via the entries[] erase
 *
 * Return: true if this was the last slot
 *
 * The caller must own the slot's durable reference -- i.e. it erased
 * entries[index] and got @ent back (xa_erase returns the old value,
 * so exactly one thread can). The return value gates the once-per-
 * group accounting in shmem_free_swap(); the entry itself is freed
 * when the last reference of any kind (slot or reader pin) drops --
 * by whichever of release()/put() gets there first. No staleness
 * check is needed or possible: the pointer names the object, and the
 * claim proves this thread owned the ref it is dropping.
 */
bool shmem_comp_entry_release(struct shmem_comp_entry *ent);

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

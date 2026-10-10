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
 * - The page cache xarray stores xa_value slots carrying a small entry
 *   ID (SWP_SHMEM_COMPRESS type). zsmalloc handles are kernel pointers
 *   and cannot be packed into xarray values, so a global ID table
 *   (struct shmem_comp_entry in mm/shmem_compress.c) maps ID ->
 *   {zsmalloc handle, compressed length, group size, refcounts}
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
 * the small {id, generation} pair via make_shmem_comp_entry().
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
	 * Incarnation stamp, packed into the page cache xarray value
	 * alongside the ID (see swapops.h). Pin and release validate it
	 * so a stale xarray value cannot charge a recycled ID.
	 */
	unsigned int gen;
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
	/*
	 * Per-unit validity bitmap: which logical sectors inside this
	 * compression unit still hold live data.  Only meaningful when
	 * pool->unit_ratio > 1 (the logical/physical split, cellzram).
	 *
	 * Lives HERE, uncompressed, and never inside the compressed blob:
	 * a blob-internal bitmap would need a full decompress-recompress
	 * to flip one bit on discard, rebuilding the RMW the split exists
	 * to avoid, and it would make the validity mask depend on the
	 * blob's own integrity.  Post-decompress, the mask is independent
	 * authoritative state -- discard is a bit-flip with no recompress.
	 *
	 * A flexible array sized at alloc: BITS_TO_LONGS(ratio) words
	 * when the split is on, ZERO words when it is off.  At ratio 1
	 * the entry's existence is already the bitmap, so the default
	 * allocation stays byte-identical to before this field existed.
	 * That is non-negotiable: an entry-table shrink must not begin by
	 * growing the entry.  Any access must therefore be ratio-guarded
	 * -- at ratio 1 no words are allocated and valid[0] is OOB.
	 */
	unsigned long valid[];
};

/*
 * Encode/decode a cell location into the handle field.
 *
 * The offset field is 22 bits because a valid offset is always
 * < cell_size and the largest cell is 4 MiB (CELL_ORDER_MAX); the
 * cellid field takes the next 33 bits (u32 + the +1 bias).
 */
#define CELL_LOC_OFFSET_BITS	22
#define CELL_LOC_OFFSET_MAX	(1UL << CELL_LOC_OFFSET_BITS)

/*
 * Encode/decode a cell location into the handle field.
 *
 * cellid is biased by +1 so that the first object of the first cell
 * (cellid=0, offset=0) never encodes to handle 0.  Handle 0 is the
 * universal "storage failed" sentinel in the compression paths —
 * without the bias, that object would be silently discarded on every
 * fresh pool.
 *
 * Layout: [63..55 unused][cellid+1, 33 bits at 22][offset, 22 bits].
 * offset never exceeds 22 bits (max cell size is 4 MiB, so a valid
 * offset is < 2^22); cellid+1 needs up to 33 bits because cellid is
 * a u32 (a directory can hold up to UINT_MAX cells, though the
 * kvcalloc fails long before that).  The handle is a full unsigned
 * long stored in entry.handle -- not a swp_entry -- so all 64 bits
 * are available (see the 55-bit {id,gen} packing in swapops.h, which
 * is a separate, tighter encoding for tmpfs xarray values).
 *
 * When widening or narrowing the fields, re-price THIS encoding, not
 * the struct cell declaration: the u16 cellid wall was never in the
 * type, it was here.
 */
static inline unsigned long shmem_comp_pack_loc(u32 cellid, u32 offset)
{
	return ((unsigned long)(cellid + 1) << CELL_LOC_OFFSET_BITS) |
		(offset & (CELL_LOC_OFFSET_MAX - 1));
}

static inline void shmem_comp_unpack_loc(unsigned long handle,
					 u32 *cellid, u32 *offset)
{
	*cellid = (handle >> CELL_LOC_OFFSET_BITS) - 1;
	*offset = handle & (CELL_LOC_OFFSET_MAX - 1);
}

/*
 * Staged (uncompressed) handle -- the FTL open-cell model.
 *
 * Bit 63 marks the handle as an OPEN-BUFFER SLOT INDEX rather than a
 * cell region; the low bits carry the index into the pool's bounded
 * opens[] array.  Bit 63 is free: shmem_comp_pack_loc() uses bits
 * 54..0 (offset 22 + cellid+1 33), leaving 63..55 spare (see the
 * encoding note above).  A staged unit and a closed unit are the SAME
 * entry -- only the handle flips -- so shmem_comp_unpack_loc() is
 * called only on cell-region handles (bit 63 clear); dispatch is one
 * shmem_comp_is_staged() test.  Bit 63 set also guarantees a staged
 * handle is never 0, so it cannot collide with the "storage failed"
 * sentinel any more than a cell region can.
 *
 * Step 1 of the open-cell buffer: the encoding and the open_max knob
 * only, dormant (no opens[] array is allocated and no path stages).
 * See mission_log/2026-10-10_ftl-open-cell-buffer.md.
 */
#define CELL_STAGED_BIT		(1UL << 63)

static inline unsigned long shmem_comp_pack_open(unsigned int idx)
{
	return CELL_STAGED_BIT | idx;
}

static inline bool shmem_comp_is_staged(unsigned long handle)
{
	return handle & CELL_STAGED_BIT;
}

static inline unsigned int shmem_comp_open_idx(unsigned long handle)
{
	return (unsigned int)(handle & ~CELL_STAGED_BIT);
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
	/*
	 * Stale xarray values rejected by generation validation
	 * (shmem_comp_entry_get_gen/release). Normal under truncate +
	 * rewrite churn -- the caller retries -- but a counter that
	 * climbs without bound points at an ID-recycling storm.
	 */
	unsigned long stale_value_retries;
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
 * Write-path hook (shmem_write_end): true when this inode's mount has
 * an enabled pool, the mount is in hot mode, and the file is at/over
 * the compression threshold (threshold_bytes). In chill mode this
 * returns false and the write lands uncompressed (see arm_aging).
 */
bool shmem_comp_should_compress(struct inode *inode);

/*
 * Write-path hook (shmem_write_end): arm the mount's aging pass when a
 * write did NOT compress inline (chill mode or sub-threshold write).
 * No-op on uncompressed mounts; scheduling an already-pending work is
 * cheap.
 */
void shmem_comp_arm_aging(struct inode *inode);

/*
 * Truncate hook (shmem_undo_range): record an uncompressed folio
 * leaving the page cache while the mount is chilled -- a page that
 * died without paying a compression cycle (death-before-pop's saving).
 */
void shmem_comp_note_unmap(struct inode *inode);

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
						unsigned int nr_cells);

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
void shmem_comp_pool_set_unit_ratio(struct shmem_comp_pool *pool,
				    unsigned int ratio);
unsigned int shmem_comp_pool_unit_ratio(struct shmem_comp_pool *pool);

/*
 * Staged-write bound (open-cell buffer).  Set once at setup before io,
 * like unit_ratio.  0 = staging disabled (every pool but a cellzram
 * device configured with open_units > 0); the opens[] array itself is
 * allocated lazily on the first stage, so a pool that never stages
 * holds no extra state.  The getter lands with the code that reads it
 * (step 2).
 */
void shmem_comp_pool_set_open_max(struct shmem_comp_pool *pool,
				  unsigned int open_max);
unsigned int shmem_comp_pool_open_max(struct shmem_comp_pool *pool);

/*
 * Staged-write buffers (the FTL open-cell buffer; cellzram at ratio>1).
 * A unit is staged: its uncompressed bytes live in a pool open-buffer
 * slot (handle bit 63 set, low bits the slot index) until GC or a full
 * pool compresses it into a cell region.  See
 * mission_log/2026-10-10_ftl-open-cell-buffer.md.
 *
 * stage_open: create a staged entry, seeded from @seed_id's blob (or a
 * fresh zeroed unit when @seed_id is 0).  Returns the entry id, or 0 when
 * no slot is free (the caller stores the unit compressed instead).
 *
 * unit_load: copy logical block @blk of a unit (staged OR closed) into
 * @dst, honouring the validity bitmap (a discarded block reads zeros).
 * This is the whole ratio>1 read, so the staged-vs-drain protocol stays
 * inside shmem_compress where the entry and slot live.
 */
unsigned int shmem_comp_stage_open(struct shmem_comp_pool *pool,
				   unsigned int seed_id);
int shmem_comp_unit_load(unsigned int id, unsigned int blk, void *dst,
			 unsigned int len);
int shmem_comp_stage_write(unsigned int id, unsigned int blk,
			   const void *src, unsigned int len);
unsigned int shmem_comp_entry_valid_count(unsigned int id);
unsigned long shmem_comp_pool_staged_bytes(struct shmem_comp_pool *pool);
void shmem_comp_gc_now(struct shmem_comp_pool *pool);

/*
 * Validity-bitmap accessors for split pools (unit_ratio > 1).  The
 * bitmap lives in the entry, outside the compressed blob, so discard
 * clears a bit without recompressing.  At unit_ratio == 1 no words are
 * allocated and these report on the entry's presence instead, keeping
 * the disabled-split path identical.
 */
bool shmem_comp_entry_sector_valid(unsigned int id, unsigned int idx);
bool shmem_comp_entry_any_valid(unsigned int id);
bool shmem_comp_entry_sector_clear(unsigned int id, unsigned int idx);
void shmem_comp_entry_sector_set(unsigned int id, unsigned int idx);
void shmem_comp_entry_valid_copy(unsigned int dst_id, unsigned int src_id);

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
 * The ID+generation pair is stored once per page cache slot of the
 * group via make_shmem_comp_entry(); refs and slots are both
 * initialised to @nr_pages. Each slot is released with
 * shmem_comp_entry_release(). @gen receives the entry generation the
 * caller must carry in every xarray value it installs and pass to
 * get_gen()/release().
 */
unsigned int shmem_comp_entry_alloc(struct shmem_comp_pool *pool,
				    unsigned long handle, unsigned int length,
				    unsigned short nr_pages,
				    unsigned long *handles,
				    unsigned short nchunks,
				    unsigned int *gen);

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

/*
 * shmem_comp_entry_get_gen - Generation-validated pin (page cache paths)
 * @id: entry ID from the xarray value
 * @gen: generation from the same xarray value
 * @handle: output, zsmalloc handle (0 for merged groups), may be NULL
 * @length: output, compressed length in bytes, may be NULL
 * @nr_pages: output, pages merged into this block (1 = single), may be NULL
 * @handles: output, merged-group handle array, may be NULL
 * @nchunks: output, merged-group chunk count, may be NULL
 *
 * Return: true if the entry was pinned
 *
 * Same contract as shmem_comp_entry_get(), plus generation
 * validation: fails when the ID now names a different incarnation
 * (the xarray value the caller carried is stale -- truncate freed the
 * entry and a rewrite recycled the ID). Stale rejections are counted
 * in stale_value_retries; the caller treats failure as "entry
 * vanished" and retries its slot lookup. Use this (never the bare
 * get) for anything that reached the ID through a page cache xarray
 * value; bare get() stays for table-internal users (cellzram) whose
 * entries are pinned by their own table references.
 */
bool shmem_comp_entry_get_gen(unsigned int id, unsigned int gen,
			      unsigned long *handle,
			      unsigned int *length,
			      unsigned int *nr_pages,
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
 * @id: entry ID from the xarray value
 * @gen: generation from the same xarray value
 *
 * Return: true if this was the last slot
 *
 * Each page cache slot holding the ID calls this once when the slot is
 * replaced by a decompressed folio or erased by truncate/eviction. The
 * return value gates the once-per-group accounting in shmem_free_swap();
 * the entry itself is freed only when the last reference of any kind
 * (slot or reader pin) drops -- by whichever of release()/put() gets
 * there first. A stale value (generation mismatch: the entry died and
 * the ID was recycled) is rejected without touching the new
 * incarnation and counted in stale_value_retries.
 */
bool shmem_comp_entry_release(unsigned int id, unsigned int gen);

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

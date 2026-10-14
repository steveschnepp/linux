/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mm/shmem_comp_zscompat.h -- leveled zsmalloc API for tmpfs compression
 *
 * Private to mm/ (CONFIG_TMPFS_COMPRESSION), included by shmem_compress.c.
 *
 * The feature calls the shcomp_zs_* primitives so that shmem_compress.c is
 * identical across the master, 6.18 and 6.12 trees.  This is the 6.18 copy:
 * it maps the primitives onto 6.18's zsmalloc API, where the object-read
 * primitives are the 3-argument zs_obj_read_begin(pool, handle, local_copy)
 * / zs_obj_read_end(pool, handle, handle_mem) (no mem_len, unlike master's
 * 7.0+ form) and writes are a discrete zs_obj_write().
 *
 * The shim exposes only those discrete primitives; composing them -- the
 * bounce-buffer allocation + memcpy around read_begin/end -- is feature
 * logic and stays in shmem_comp_load(), unchanged per tree.
 *
 * Downstream-maintenance aid; not sent upstream.
 */
#ifndef SHMEM_COMP_ZSCOMPAT_H
#define SHMEM_COMP_ZSCOMPAT_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/numa.h>	/* NUMA_NO_NODE */
#include <linux/pagevec.h>	/* PAGEVEC_SIZE */
#include <linux/zsmalloc.h>

/* 6.18 names the folio-batch capacity PAGEVEC_SIZE; it became FOLIO_BATCH_SIZE
 * in 7.0+.  Level it so shmem_compress.c uses one name on every tree. */
#ifndef FOLIO_BATCH_SIZE
#define FOLIO_BATCH_SIZE PAGEVEC_SIZE
#endif

/* inode_state_read() (which reads inode->i_state.__state under i_lock)
 * arrived in 7.x; on 6.18 inode->i_state is a plain unsigned long, read by
 * the caller under i_lock. */
#ifndef inode_state_read
#define inode_state_read(inode) ((inode)->i_state)
#endif

#define shcomp_zs_malloc(pool, len, gfp) \
	zs_malloc((pool), (len), (gfp), NUMA_NO_NODE)

#define shcomp_zs_write(pool, handle, data, len) \
	zs_obj_write((pool), (handle), (void *)(data), (len))

/* 6.18: 3-arg read primitives; the shim's uniform (pool,handle,len,copy)
 * signature drops the unused len so feature call sites stay identical. */
#define shcomp_zs_read_begin(pool, handle, len, local_copy) \
	zs_obj_read_begin((pool), (handle), (local_copy))

#define shcomp_zs_read_end(pool, handle, len, handle_mem) \
	zs_obj_read_end((pool), (handle), (handle_mem))

#endif /* SHMEM_COMP_ZSCOMPAT_H */

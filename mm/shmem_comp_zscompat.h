/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mm/shmem_comp_zscompat.h -- leveled zsmalloc API for tmpfs compression
 *
 * Private to mm/ (CONFIG_TMPFS_COMPRESSION), included by shmem_compress.c.
 *
 * The feature calls the shcomp_zs_* primitives so that shmem_compress.c is
 * identical across the master, 6.18 and 6.12 trees.  Each tree carries its
 * own copy of this header, mapping the primitives onto that tree's zsmalloc
 * API.  On master these are pure pass-throughs to the native discrete calls,
 * so here the shim is a no-op wrapper.
 *
 * The shim exposes only the DISCRETE primitives, matching master's
 * zs_malloc / zs_obj_write / zs_obj_read_begin / zs_obj_read_end.  Composing
 * them -- e.g. the bounce-buffer allocation + memcpy around read_begin/end --
 * is feature logic and stays in shmem_comp_load(), unchanged per tree.  Only
 * 6.12, whose zsmalloc has no discrete write call, needs a non-trivial
 * (map+copy+unmap) implementation of shcomp_zs_write().
 *
 * This is a downstream-maintenance aid: drop this header and inline the four
 * calls when sending the feature upstream.
 */
#ifndef SHMEM_COMP_ZSCOMPAT_H
#define SHMEM_COMP_ZSCOMPAT_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/numa.h>	/* NUMA_NO_NODE */
#include <linux/zsmalloc.h>

#define shcomp_zs_malloc(pool, len, gfp) \
	zs_malloc((pool), (len), (gfp), NUMA_NO_NODE)

#define shcomp_zs_write(pool, handle, data, len) \
	zs_obj_write((pool), (handle), (void *)(data), (len))

#define shcomp_zs_read_begin(pool, handle, len, local_copy) \
	zs_obj_read_begin((pool), (handle), (len), (local_copy))

#define shcomp_zs_read_end(pool, handle, len, handle_mem) \
	zs_obj_read_end((pool), (handle), (len), (handle_mem))

#endif /* SHMEM_COMP_ZSCOMPAT_H */

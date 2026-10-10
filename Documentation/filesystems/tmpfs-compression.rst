.. SPDX-License-Identifier: GPL-2.0

.. _tmpfs_compression:

=============================
Transparent tmpfs compression
=============================

This document is a high-level tour of the tmpfs transparent-compression
implementation as it currently exists: what is in the tree, how the
pieces fit together, and what has been measured. It deliberately does
not restate the design rationale or the session-by-session history,
both of which live outside the kernel tree. Read this first, then
follow the code.

Compression is transparent: applications see ordinary tmpfs. Pages are
compressed on the way into a storage backend and decompressed on the way
back out, and the filesystem semantics are unchanged.

Overview
========

A tmpfs mount with ``compress=lz4`` sends pages through a compression
pool at reclaim time and stores the result in one of two interchangeable
backends: **zsmalloc**, which is mature and size-classed, or **cells**,
a bump allocator built for this work. The cell backend trades
size-class flexibility for O(1) allocation, zero per-object metadata,
and a synchronous return of freed RAM to the page allocator.

Layer stack
===========

::

    +------------------------------------------------------------+
    |                     applications                           |
    +------------------------------------------------------------+
                                |
                                v
   +----------+
   |  tmpfs   |
   |compress= |
   |merge_pages
   +----------+
        |
        v
   +----------------------------------------------------+
   |        policy: hot/chill watermarks, aging,         |
   |                 merge groups                        |
   +----------------------------------------------------+
        |
        v
   +----------------------------------------------------+
   |            compression core (shmem_compress)        |
   |      per-CPU acomp contexts, entry table (xarray)   |
   +----------------------------------------------------+
                        |
          +-------------+-------------+
          |             use_cells     |
          v                           v
   +-------------+          +-----------------+
   |   zsmalloc  |          |  cell allocator |
   | size classes|          |  (cell_store)   |
   +-------------+          +-----------------+

The compression core does not know which backend is active. Both
implement the same three-verb contract -- ``store`` returns an opaque
handle, ``load`` copies data out, ``free`` releases storage -- and
callers dispatch on a single ``use_cells`` flag.

The cell allocator
==================

A **cell** is a single physically contiguous allocation used as a
bump-allocated arena. Compressed blocks are stored at exact size: no
per-object header, no size-class rounding, no free list::

    +-------------------------------------------------------+
    | cell (e.g. 256 KiB)                                   |
    |                                                       |
    | +--------+ +--------+ +--------+          +---------+ |
    | | block A| | block B| | block C|          | bump -> | |
    | | 1.2 KB | | 0.9 KB | | 2.1 KB |          |  free   | |
    | +--------+ +--------+ +--------+          |  space  | |
    |                                           +---------+ |
    +-------------------------------------------------------+
             ^                                       ^
             |                                       |
      alloc_pages() when                     cell_free() when
      the active cell fills                  live_bytes == 0

The blocks are already small -- compressed pages or merged groups --
so every byte of metadata is a direct tax on the savings compression is
supposed to deliver.

Design invariants
-----------------

These are load-bearing. Breaking one silently corrupts the model.

======================================  ==================================================
invariant                               why
======================================  ==================================================
Only the active cell grows; the bump    Keeps allocation O(1) with no fragmentation
pointer never retreats                  analysis.
A cell reaching ``live_bytes == 0``     ``cell_free()`` returns with the pages already
returns to the buddy **synchronously**  available to the page allocator.
``cell->base`` is direct-map            No kmap, no bounce buffer; reads span pages by
contiguous                              pointer arithmetic.
The bump advances under ``cp->lock``;   The lock is never held across a copy or a
the memcpy happens after                sleep.
A cell carries no pool-wide state        A shadow copy would drift from the
                                        authoritative pool field.
One directory slot beyond capacity is   Otherwise evacuating a full directory is a
reserved for compaction                 fixed point that frees nothing.
======================================  ==================================================

Compaction
----------

A cell is reusable only as a whole, so holes left by freed blocks are
reclaimed by evacuating the entire cell rather than by reusing gaps::

    sparse cell                fresh cell
    +----------------+         +----------------+
    | A .  .  .  . C |  copy   | A B C .  .  .  |
    +----------------+ ------> +----------------+
         |                              |
         | live_bytes == 0              |
         v                              |
    buddy allocator <--------------------+

Three triggers: the **shrinker** under memory pressure, the ``gc_now``
sysfs hook for manual maintenance, and the **store path** when a
cell-backed pool runs out of directory slots. The last keeps a full
directory from failing writes.

Compaction takes the per-cell rwsem exclusively; readers hold it shared
and additionally pin the entry, so a victim cannot be freed while a
read is in flight.

The entry table
===============

Every stored block is referenced by an ID rather than a raw pointer.
The ID is what appears in a page-cache xarray slot::

    page cache slot -----> entry ID -----> shmem_comp_entry
                           (+ generation)      |
                                               +-- handle --> backend
                                               |              storage
                                               +-- length
                                               +-- refs / slots

Three properties worth knowing:

* **Two reference counters.** ``slots`` counts page-cache slots
  pointing at the entry; ``refs`` counts slots plus temporary reader
  pins. The split exists so that truncation cannot free storage out
  from under a reader mid-decompress.

* **A generation stamp.** Packed into the xarray value alongside the
  ID, so a stale xarray value can never charge a recycled ID.

* **merge_pages.** One entry can span N consecutive pages compressed as
  a single group: one store, one handle, no per-chunk handle array.

Policy: hot, chill, and merge
=============================

Compression is not free, so not every page deserves it. Two watermarks
express a hysteresis band::

    pool usage
         |
         |  reaches watermark_high
         +-------------------------> hot: compress aggressively
         |                                |
         |  drops to watermark_low        | ratio poor, or page re-read
         +-------------------------> chill: back off
                                      |
                                      +--> stays compressed --> hot

``compress_watermark=LOW:HIGH`` sets the band as percent usage; ``0:0``
means eager. Pages whose data does not shrink are simply kept
uncompressed -- the entry length field is the raw/compressed
discriminator, so no separate metadata is needed to track it.

Measurement
===========

A QEMU guest boots a static initrd and runs self-verifying test
binaries: pool and entry-table unit tests, mount-semantics gates, cell
allocator and GC tests, and torture/scale runs::

    host scripts --cmdline knobs--> QEMU guest
                                        |
                                        v
                                    /init -> init.sh
                                        |
                                        v
                            self-verifying test binaries
                                        |
                                        v
                            per-test pass/fail summaries

Two methodology rules were learned the hard way and are now enforced:

* **Never quote a single sample.** Cross-boot drift (around 3%) is
  larger than many of the effects being resolved. A benchmark runs N
  in-boot samples and reports high/low/avg/median, then runs M full
  boots and reports median plus IQR across them.

* **Use a control that runs in every boot.** A benchmark compares
  against a reference running in the same boot, so the ratio between
  them cancels that boot's drift. The reference also serves as a
  canary: a boot whose reference row moved is an off boot and is
  rejected.

Status
======

An FTL-style logical/physical block split and its follow-up, an
open-cell write-gathering buffer, were built on this machinery for a
compressed RAM block device (cellzram) and then removed. The device
lost to zram on random-overwrite throughput for a negligible RAM win,
so the pivot is to the tmpfs path alone. The design, builds and
verdict live in the project's session logs and in git history; the tree
carries only what this document describes.

**The largest open question.** No workload in the test rig measures
compression ratio on genuinely compressible, non-zero data. The
``distinct`` workload is incompressible; untar is approximately 2048
zero blocks out of 2061. Any compression-ratio claim that rests on
zero-heavy or incompressible synthetic data does not generalize. Until
a real dataset -- text, binaries, logs -- is measured against
uncompressed tmpfs, the compression-ratio question is open.

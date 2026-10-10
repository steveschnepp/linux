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

The same compression machinery is reused by **cellzram**, a compressed
RAM block device offering zram's contract (runtime-configurable
capacity, allocation on demand) on top of the cell backend.

Layer stack
===========

::

    +------------------------------------------------------------+
    |                     applications                           |
    +---------------------------+--------------------------------+
                                |
        +-----------------------+-----------------------+
        |                                               |
        v                                               v
   +----------+                                  +--------------+
   |  tmpfs   |                                  | block device |
   |compress= |                                  |  (cellzram)  |
   |merge_pages                                    +--------------+
   +----------+                                         |
        |                                               |
        v                                               |
   +----------------------------------------------------+
   |        policy: hot/chill watermarks, aging,         |
   |                 merge groups                        |
   +----------------------------------------------------+
        |                                               |
        +-----------------------+-----------------------+
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
callers dispatch on a single ``use_cells`` flag. This is what allows
cellzram to reuse every hot path unchanged.

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
cell-backed pool runs out of directory slots. The last is how cellzram
avoids write failures it previously hit.

Compaction takes the per-cell rwsem exclusively; readers hold it shared
and additionally pin the entry, so a victim cannot be freed while a
read is in flight.

The entry table
===============

Every stored block is referenced by an ID rather than a raw pointer.
The ID is what appears in a page-cache xarray slot or in a block
device's table::

    page cache slot -----> entry ID -----> shmem_comp_entry
    or block slot          (+ generation)      |
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

cellzram
========

cellzram applies zram's contract -- capacity set at runtime via
``disksize``, physical allocation on demand -- on top of the cell
backend::

    bio -----> block table (xarray) ----+----> cell region
               block_nr -> entry ID     |      (+ entry)
                                        |
                                        +----> zero-block sentinel
                                               (no cell, no entry)

    discard / punch-hole -----> synchronous free -----> buddy allocator

Two design points are not obvious:

* **Zero blocks are free.** An all-zero write stores a sentinel in the
  table instead of compressing. Filesystem metadata is zero-heavy, so
  this turns ext4's own zero writes into essentially free storage.

* **Discard frees synchronously.** When the last reference to a cell
  region drops, the pages return to the buddy inline. An ext4 delete
  returns RAM before the syscall returns -- a property zram's
  architecture cannot offer.

``mm_stat`` is derived by walking the block table on read rather than
from incremental counters. An earlier counter-based implementation
drifted on the zero-block path: a repeated zero write double-counted
``orig_bytes``. The walk is exact by construction.

Measurement
===========

A QEMU guest boots a static initrd and runs self-verifying tests. Five
are pass/fail gates; the remainder are unit or exploratory tests::

    host scripts --cmdline knobs--> QEMU guest
                                        |
                                        v
                                    /init -> init.sh
                                        |
        +-----------+-----------+-----------+-----------+
        |           |           |           |           |
     cellzram    randio     fsdev/untar  torture     churn
        |           |           |           |           |
        +-----------+-----------+-----------+-----------+
                                        |
                                        v
                              BENCH lines + raw mm_stat
                                        |
                                        v
                                 host aggregator

Two methodology rules were learned the hard way and are now enforced:

* **Never quote a single sample.** Cross-boot drift (around 3%) is
  larger than many of the effects being resolved. A benchmark runs N
  in-boot samples and reports high/low/avg/median, then runs M full
  boots and reports median plus IQR across them.

* **Use the control that runs in every boot.** zram runs alongside
  cellzram in the same boot, so the ratio between them cancels that
  boot's drift. zram also serves as a canary: a boot whose zram row
  moved is an off boot and is rejected.

Measured results
================

Current state of the ``cell_order`` axis (allocation size, 2..10,
default 6 meaning 256 KiB).

**untar workload** (compressible, append-heavy) -- smaller cells win:

========  ========  ===========  ============  ==========
order     cell      RAM held     verify MB/s   rm MB/s
========  ========  ===========  ============  ==========
2         16 KiB    **16 KiB**   79.5          60.8
6         256 KiB   256 KiB      76.1          55.5
zram      --        *80 KiB*     *75.5*        *64.4*
========  ========  ===========  ============  ==========

**churn workload** (random overwrite) -- a real tradeoff:

========  ===========  ==============  ============
order     avg MB/s     GC recoveries   RAM held
========  ===========  ==============  ============
2         **133.6**    **0**           16.6 MiB
6         125.3        58              **10.25 MiB**
zram      *136.3*      --              *8.0 MiB*
========  ===========  ==============  ============

At order 2 a cell holds only four blocks, so it empties naturally under
random overwrite and returns inline. The compaction pass never fires,
and cellzram comes within 2% of zram on throughput. But with no GC
there is no compaction either, so fragmentation accumulates and more RAM
is held. Order 6 pays roughly 9% throughput for GC but keeps RAM closer
to the live set. Neither order dominates.

Status
======

A logical/physical block split was under development -- compressing 16
consecutive 4 KiB blocks as one unit, the same storage layout that
tmpfs ``merge_pages`` already produces. It is parked. The landed steps
are dormant, byte-identical scaffolding (parameters, unit rekey,
validity bitmap, accessors); the read-modify-write core was not built.
The reason: the split's only measured payoff was a 936-byte entry table
on the untar workload, against roughly 200 lines of correctness-critical
read-modify-write. The real untar gap was cell granularity, which
``cell_order`` fixes directly.

**The largest open question.** No workload in the test rig measures
compression ratio on genuinely compressible, non-zero data. The
``distinct`` workload is incompressible; untar is approximately 2048
zero blocks out of 2061. Every claim about where cellzram wins rests on
zero-heavy or incompressible synthetic data. Until a real dataset --
text, binaries, logs -- is measured against zram, the compression-ratio
question is open, and the parked split should be decided on that
evidence rather than by momentum.

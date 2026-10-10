---
name: cellzram-removal
description: remove the cellzram block device and its pool-side staging machinery -- delete-forward scoping of the upstreamable tmpfs slice, history kept
project: linux
previous: 2026-10-10_zcell-verdict-tmpfs-pivot.md
---

# Mission Log: Removing the Blockdev

Part 20 of the tmpfs transparent-compression series.  Session 36
(2026-10-10).  Session 35 reached the verdict (the open-cell buffer
loses to zram on random writes; the value is in tmpfs) and named the
next phase; this session executes the first scoping move: delete the
block device and everything that existed only to serve it.

The `previous` field above is same-day ordering only -- session 35's
log ends with the verdict, this one starts from it.

## What We Did

1. **Removed the device** (kernel commit `49b5571006b4`):
   `mm/cellzram.c` (1270 lines), the `mm/Makefile` line, and the
   `CONFIG_TMPFS_COMP_BLKDEV` Kconfig block.  Built clean
   (`bzImage #115`, zero warnings); `olddefconfig` dropped the symbol
   from the rig's persistent config without complaint.

2. **Removed the pool-side staging machinery** (kernel commit
   `0735958a29f8`, -938 lines): everything in `mm/shmem_compress.c` /
   `include/linux/shmem_compress.h` that existed only for cellzram at
   ratio > 1 -- `struct shmem_comp_open`, the pool
   `opens[]/open_max/open_count/open_lock/open_lru` state, the pool
   `unit_ratio` and its accessors, `stage_open`, `unit_load`,
   `stage_write`, `open_free_slot`, `drain_staged`, `gc_now`,
   `entry_valid_count`, `pool_staged_bytes`, the entry `valid[]`
   flexible array, the five sector-bitmap accessors, and the
   `CELL_STAGED_BIT` staged-handle inlines.  Built clean
   (`bzImage #116`, zero warnings).

3. **Cleaned the rig** (rig commit `49632d3`): deleted
   `test_cellzram.c`, `test_blkdev_fs.c`, `test_randio.c`,
   `test_churn.c`, `sweep_headroom.sh`, `mboot_sweep.sh`; removed the
   five dedicated boot modes from `guest/init.sh` (cellzram, split,
   fsdev, randio, churn) and the device parameter passthroughs from
   `scripts/run_qemu.sh` (`TEST_CELLZRAM`, `TEST_FSDEV`,
   `TEST_RANDIO`, `TEST_CHURN*`, `TEST_HEADROOM`, `TEST_CELLORDER`,
   `TEST_PHYSICAL`, `TEST_OPENUNITS`, `TEST_CELLZRAM_SPLIT`).

4. **Ran the tmpfs suite on the post-removal kernel**: full QEMU run,
   `init.sh exited: exit code 0`, zero FAIL lines across every test
   binary.

5. **Rewrote the architecture doc** (commit `ab63f86fc8fe`):
   `Documentation/filesystems/tmpfs-compression.rst` now describes
   only the tmpfs path -- block-device branch out of the layer stack,
   the cellzram section and the device-measured `cell_order` tables
   gone, the Status section stating the removal honestly.  Also
   dropped a stale cellzram reference from a `cell_store.c` comment.

## What We Learned

### Technical

- **The device was the small part.**  `cellzram.c` was 1270 lines,
  but the staging machinery threaded through the shared pool was
  another 938 -- nearly 40% of the removal lived in code that other
  callers shared.  A "delete the file" instinct would have left a
  header exporting eight functions with no caller and a pool struct
  carrying three dead fields.

- **Deleting a function leaves its comment opener behind if the edit
  starts below line 1.**  Three times the removal left a dangling
  `/*` (the gc_now block, twice-fixed comment titles): once it is a
  `"/*" within comment` warning and then a wall of parse errors far
  from the site.  The build caught it both times; the fix is to start
  a deletion at the comment opener, not the function signature.

- **A sysfs hook and a removed function shared a name.**  The pool
  has a `gc_now` sysfs attribute (a tmpfs maintenance hook) and
  cellzram had a `shmem_comp_gc_now()` helper.  Grepping for
  `gc_now` after removal shows ~12 surviving hits -- all the sysfs
  facility, none of the helper.  Name collisions like this are how a
  removal either over-deletes or looks incomplete; read each hit
  before judging.

- **`shmem_comp_load_entry` survives the removal.**  The tmpfs
  shrinker and decompress paths call it (two call sites remain); the
  mission logs describe it as the protocol cellzram relied on, which
  reads like a dependency but is not one.  Call-site greps, not log
  memory, decide what goes.

### Process

- **A vocabulary sweep finds what a symbol sweep misses.**  After
  removing the symbols, grepping for the *vocabulary* (`cellzram`,
  `staged`, `staging`, `open_max`, `unit_ratio`, `valid[]`,
  `open-cell`, `sector_`) caught comments, doc prose, and a Kconfig
  help text the symbol grep could not.

- **The no-install rule bit again.**  The rst was hand-checked
  (ASCII-clean, table alignment) but still cannot be Sphinx-verified:
  no docutils in the environment.  Same gap session 33 recorded; it
  remains open.

- **Two builds per removal step is cheap insurance.**  Each kernel
  commit got its own full build before landing, so the failing
  comment-termination bug bisected to the commit that introduced it
  (the staging removal), not to a combined diff.

## What We Decided

- **Full removal, not device-only.**  The user chose (via decision
  handshake) to remove the pool-side staging too: the upstream slice
  must not carry cellzram-only code, and dormant machinery in shared
  files is exactly what review notices.

- **Delete-forward, history preserved.**  No history rewrite: the
  design, the builds and the verdict stay reachable through the
  session-33..35 commits.  This is the session-35 rule ("never revert
  a mission's code to clean up the story") honored in its intended
  spirit -- the removal is a scoping decision for the upstream slice,
  recorded as new commits and a new log, not an erasure.

- **The doc tells the truth.**  The rst's Status section names the
  removal and points at the history rather than pretending the
  experiments never happened; the device-measured numbers left the
  doc because they describe code that no longer exists, not because
  they were wrong.

- **The rig keeps only the tmpfs suite.**  The four device gates
  (cellzram, split, fsdev, randio, churn) and the two device-only
  sweeps are gone; the tmpfs suite, torture, entryrace and the merge
  sweeps stay.

## Rules Added

- **Removing a feature means grepping its vocabulary, not just its
  symbols.**  Comments, docs and Kconfig help text carry the feature
  name long after the identifiers are gone.

- **Start a code deletion at the comment opener.**  Function doc
  comments open above the signature; an edit anchored at the
  signature leaves a dangling `/*` that turns the next build into a
  parse-error maze.

- **Before deleting a name, check it is not shared with a surviving
  facility.**  `gc_now` was both; only one of them was cellzram's.

## What We'd Do Differently

- Would have grepped the whole tree for `cellzram` *before* writing
  the removal edits -- the `cell_store.c` comment and the doc's ten
  references were found by the after-sweep, and finding them first
  would have made the diff complete on the first pass.

- Would have removed the doc references in the same commit as the
  code.  Kernel convention is doc-travels-with-code; the doc landed
  one commit later here, which is acceptable for a scoping series but
  not the ideal shape.

## Files Changed

| File | Purpose |
|------|---------|
| `mm/cellzram.c` | **deleted** -- the compressed RAM block device |
| `mm/Makefile` | cellzram.o object line gone |
| `fs/Kconfig` | `TMPFS_COMP_BLKDEV` gone |
| `mm/shmem_compress.c` | staging machinery gone (-836 lines); cell GC, shrinker, sysfs hooks untouched |
| `include/linux/shmem_compress.h` | staged-handle inlines, `valid[]`, staging/split declarations gone |
| `mm/cell_store.c` | stale cellzram comment reference dropped |
| `Documentation/filesystems/tmpfs-compression.rst` | tmpfs-only architecture tour; honest Status section |
| rig: `tests/`, `guest/init.sh`, `scripts/run_qemu.sh`, sweep scripts | device tests, boot modes and knobs gone (rig `49632d3`) |

## Test Results / Benchmarks

| gate | result |
|---|---|
| build after device removal | `bzImage #115`, 0 warnings |
| build after staging removal | `bzImage #116`, 0 warnings |
| build after doc/comment pass | `bzImage #117`, 0 warnings |
| tmpfs suite (full QEMU run) | `init.sh` exit code 0, 0 FAIL lines |

The cellzram/split/fsdev/randio/churn gates no longer exist by
decision, not by failure.  Their last recorded state (all green,
device parked) is in the session-35 log.

## Next Steps

The tmpfs pivot, now unblocked by the scoping move, in the order
session 35 set:

1. **The known tmpfs merge bug, first.**  The `sweep_merge.sh` header
   records a slab corruption (double-release in the decompress/
   eviction interplay) that detonates on a mount after a
   compress/umount cycle.  Upstream is impossible with it present.
2. **Scope the upstreamable slice**: `merge_pages`, the compression
   pool (zsmalloc backend), the mount options, the Kconfig -- now
   trivially separable, the cell/cellzram code being gone.
3. **The measurement story**: a compressible-non-zero dataset
   (text/binaries/logs) against uncompressed tmpfs, with M-boot
   rigor.  Still the series' largest unmeasured question.
4. **Sphinx-verify the rst** once docutils is available (blocked by
   the no-install rule; needs user approval to install or a machine
   that has it).
5. ~~Decide the cell/cellzram fate~~ -- **resolved this session:
   removed, history kept.**

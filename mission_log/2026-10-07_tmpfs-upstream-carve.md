---
name: tmpfs-upstream-carve
description: upstream carve of the compression code out of mm/shmem.c -- existing-file diff from +1283 to ~+250 lines (session 4)
project: linux
---

# Mission Log: the Upstream Carve -- shmem.c from +1283 to +250

Part 2 of the tmpfs transparent-compression series, split verbatim from the
combined log `2026-10-05_tmpfs-transparent-compression.md` (kept until the
split is validated). Session numbers are preserved so cross-references keep
resolving.

Covers session 4 (2026-10-07): the five-commit carve that moved sysfs, option
parsing, and the 755-line data path out of mm/shmem.c into
mm/shmem_compress.c, with the config-off build as a per-commit gate.

- Previous: `2026-10-05_tmpfs-compression-foundation.md`
- Next in series: `2026-10-07_cellzram-block-device.md`

## Session 4: the upstream carve -- shmem.c from +1283 to +250 (2026-10-07)

### What We Did

The plan's "Upstream Shape" section had measured the blast zone
(+1,387 lines across 6 existing files, 1,283 of them in mm/shmem.c) and
laid out a five-commit carve (C1-C5) to shrink the existing-file diff
to ~210 lines without moving logic across a locking boundary. This
session executed it, in the plan's commit order, with the rig suite as
the gate between commits.

**C2 -- sysfs + defaults globals -> mm/shmem_compress.c.** The nine
sysfs attribute handlers (compression, compressor, merge_pages,
comp_stats, cell_stats, gc_now) and the three defaults globals plus
their mutex moved verbatim into shmem_compress.c, next to the
shmem_comp_* primitives they call. Registration became one call:
`shmem_comp_sysfs_register(tmpfs_kobj)` from tmpfs_sysfs_init(). The
mount path read the defaults through three temporary accessors
(enabled_get / merge_get / resolve_algo) because the globals were no
longer visible to shmem.c; C3 absorbed those consumers and the
accessors died with them.

**C3+C4+C5 -- one data-structure commit.** The plan was explicit that
these cannot be split: "splitting leaves intermediate states where
shmem.c reads fields C4 removed." shmem_options now embeds a
`struct shmem_comp_opts`; the parse_one case bodies shrink to stores
into it (set_algo/set_cell helpers own the string-consumption contract
and the cell= memparse); the 100-line fill_super pool-creation block
became `shmem_comp_fill_sb(sbinfo, opts, tmpfs_bytes)`; sbinfo dropped
eight comp_* fields for one opaque pointer. tmpfs_bytes is passed into
fill_sb so shmem_default_max_blocks() stays private to shmem.c. The
write_end and free_swap dispatch hooks shrank to check-plus-call
(shmem_comp_should_compress / shmem_comp_free_swap -- the latter
absorbs the once-per-group accounting that used to sit inline in
shmem_free_swap).

Intermediate-state decision: the plan wanted sbinfo->comp fully opaque
at C4, but the 755-line implementation block still lived in shmem.c
until C1 and dereferenced comp->pool etc. Defining struct
shmem_comp_sb in shmem_compress.h for the intermediate (with a comment
promising privacy) kept every commit building; the definition moved
into shmem_compress.c in a follow-up commit once C1 had landed. End
state matches the plan.

**C1 -- the 755-line block -> mm/shmem_compress.c.** The compress/
decompress implementation (single-page and merged-group paths, chunk
logic, folio-gathering helper) moved with its forward declarations.
Four shmem primitives were unstatic'd and EXPORT_SYMBOL_GPL'd inside
`#ifdef CONFIG_TMPFS_COMPRESSION`: shmem_recalc_inode,
shmem_add_to_page_cache (both already non-static),
shmem_delete_from_page_cache, shmem_alloc_folio. shmem.c went from
7,278 to 6,150 lines and now contains only the three dispatch hooks,
the mount-option ABI table, and the opts/fill_sb plumbing.

**Verified no-op: the undo_range substitution.** The plan's C1 table
listed shmem_undo_range as used twice in the block; both "uses" are
accounting-pairing comments ("pairs with the -nr_swaps_freed recalc at
the end of shmem_undo_range()"). The block never calls it, so the
shmem_truncate_range substitution commit was skipped -- documented in
the diet commit rather than performed.

**Diet + opacity + prototype fix.** Comment-diet pass on the
upstream-visible files (3-line invariant comments at the get_folio_gfp
hook and the four export sites; swap.h type slot gains the "page cache
xarray only, never PTEs" invariant; swapops.h turned out to be already
ifdef-wrapped). The shmem_compress.c file header gained the carve
contract (sole consumer of the exports; ownership rules). Then
struct shmem_comp_sb became private (C4 end-state), and finally
prototypes for the two newly-non-static functions landed in
mm/internal.h next to the pre-existing shmem_add_to_page_cache ones.

### What We Learned

**Technical**

- **The config-off include trap.** mm/shmem.c includes
  shmem_compress.h *inside* `#ifdef CONFIG_TMPFS_COMPRESSION`. Anything
  declared only there vanishes for config-off builds. C1 unstatic'd two
  functions whose prototypes lived only in that header -> two
  -Wmissing-prototypes warnings on the first config-off compile. The
  exports are gated (correct), but prototypes must live where both
  builds see them -- mm/internal.h, where the pre-existing
  shmem_add_to_page_cache/shmem_recalc_inode prototypes already were.
  Config-off builds are now part of the per-commit gate.
- **`-Wcomment`:** a block comment cannot contain the two-character
  sequence `/*` -- `/sys/fs/tmpfs/compression/*` inside a comment
  warns. Rephrase paths in comments (e.g. 'group "compression" under
  /sys/fs/tmpfs').
- **Splice arithmetic in block moves:** cutting `[i, j)` where `j`
  points at a marker that *starts* with the `#endif` you also want to
  remove leaves a stray `#endif` -> "#else without #if" cascades far
  away from the damage. Assert the removed span's first and last lines
  explicitly.
- **O= out-of-tree builds are blocked** by in-tree build artifacts
  ("source tree is not clean"). For a second config in the same repo,
  `git worktree add` + copy .config + olddefconfig works and keeps
  ccache warm for everything unchanged.
- Atomic edit-tool calls: a failed edits[1] reverts edits[0]; a typo
  in one oldText (COMPPRESSION) costs a whole retry round. Check the
  exact bytes (cat -A) before batching large replacements.

**Process**

- The plan's own commit sequence (C2, then C3+C4+C5 together, then C1)
  was right: splitting C3/C4/C5 creates unbuildable intermediates, and
  doing C2 first meant the defaults globals had exactly one consumer
  move to follow.
- Bisectability held: every commit built (full bzImage) and the one
  rig run after the series used a kernel built from the final state.
  A rig run *between* carve commits would have been better; the moves
  were behavior-neutral by construction (verbatim relocation plus
  accessor indirection), so the risk was compile-time, which every
  commit checked.

### What We Decided

- **Exports over statics, gated:** 4 exports, each consumed by exactly
  one TU (shmem_compress.c), each `#ifdef CONFIG_TMPFS_COMPRESSION` so
  a disabled build exports nothing new. Precedent: shmem_write_folio.
- **Prototypes ungated in mm/internal.h, exports gated in shmem.c:**
  prototype visibility is not API surface; the ksymtab entry is.
- **struct shmem_comp_sb definition lives in mm/shmem_compress.c:**
  true opacity; the header keeps the forward declaration only.
- **skipped the shmem_undo_range substitution commit** (no-op) and the
  TMPFS_COMPRESSION_GLOBAL_DEFAULTS gating (project builds want the
  global defaults; upstream posting can gate them later).
- **Carve residual:** mm/shmem.c now carries ~250 lines of compression
  code (dispatch hooks + mount ABI + opts plumbing) vs the plan's
  ~210 design floor; the extra ~40 are the fs_parameter_spec table
  entries and parse case bodies the plan explicitly keeps greppable in
  shmem.c. Within tolerance.

### Rules Added

- **Config-off compile is a gate:** any commit that touches
  mm/shmem.c, shmem_fs.h, swap.h or swapops.h must also build with
  CONFIG_TMPFS_COMPRESSION=n (git worktree + flipped .config).
- **Prototypes for unstatic'd functions go in mm/internal.h**, not in
  config-gated project headers, when the definition lives in a file
  that includes the project header conditionally.
- **Block moves assert their span:** when excising a region between
  markers, print/assert the first and last removed lines; a leftover
  #endif surfaces hundreds of lines away.

### What We'd Do Differently

- Run the rig suite between *every* carve commit, not once at the end
  -- it costs ~2 minutes and converts "behavior-neutral by
  construction" into "behavior-neutral by observation."
- The config-off build check should have been written as a script in
  the rig (worktree + flip + build mm/shmem.o) before C1, not invented
  after the warnings appeared.
- Batch-edit discipline: verify exact whitespace (cat -A) for any
  oldText containing tabs before calling the edit tool; two of the
  eight-edit batch failed on whitespace/typo and had to be re-issued
  whole.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem.c` | 7,278 -> 6,150 lines: dispatch hooks, mount ABI, opts plumbing only; 4 gated exports |
| `mm/shmem_compress.c` | Now owns sysfs + defaults, fill_sb/put_super, opts parsing, the 755-line data path, struct shmem_comp_sb |
| `include/linux/shmem_compress.h` | shmem_comp_opts, opaque shmem_comp_sb fwd decl, data-path and primitive prototypes |
| `include/linux/shmem_fs.h` | sbinfo: 8 comp_* fields -> `struct shmem_comp_sb *comp` |
| `include/linux/swap.h` | Type-slot invariant comment (entries never in PTEs) |
| `mm/internal.h` | Prototypes for shmem_delete_from_page_cache / shmem_alloc_folio |

### Test Results

- Full rig suite (mount semantics, cell storage, cell GC, mmap/write
  coherency, entry lifetime races, size accounting, bench matrix,
  multi-mount, untar): **exit code 0, no failures, no KASAN/Oops** on
  the carved kernel (bzImage #53).
- Config-off build (worktree, CONFIG_TMPFS_COMPRESSION=n):
  mm/shmem.c compiles with **zero warnings** after the internal.h fix.
- Full bzImage builds clean at every carve commit (ccache, KASAN
  config).

### Next Steps

- cellzram: the spec is committed, mm/cellzram.c does not exist.
  Phases 1-3 (raw skeleton on cells -> compression on by default ->
  swap trial) with rig test + init gating.
- Upstream series assembly: the carve made the diff reviewable; the
  remaining work is Documentation/filesystems/tmpfs.rst (mount options
  + sysfs) and the CONFIG help-text experimental wording.
- TMPFS_COMPRESSION_PLAN.md still carries the uncommitted "Upstream
  Shape" section; commit it as docs alongside the next feature work.

---

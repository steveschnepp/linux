---
name: group-decompress-truncate-race
description: the standing "group-decompress/truncate interleaving" question is a REAL race -- read/fault-path decompress installs folios into page-cache slots a truncate already cleared, resurrecting data past EOF and into punched holes; both single and group paths affected
project: linux
previous: 2026-10-16_tooling-centralization.md
---

# Mission Log: The Decompress-Install vs Truncate Race

Part 50 of the tmpfs transparent-compression series.  Session
(2026-10-16), picking up the standing backlog item "group-decompress/
truncate interleaving race" (carried in the Next Steps of parts 47-49,
originating from session 54's stale-comment find).  This log records
the investigation: the race is REAL, it is broader than "group", and
the exact interleaving and missing defence are now pinned by code
reading.

## The origin of the question

Session 54 (see 2026-10-15_32bit-packing-and-table-map.md) noticed
that `shmem_decompress_group`'s comment claimed per-slice install
races were resolved by `shmem_add_to_page_cache()` "validating the
replicated ID+generation" -- but the calls pass `shadow/expected=NULL`
and that function never reads `info->comp_entries`.  Session 56 fixed
the COMMENT (it now names the real defences: -EEXIST retry, pin
failure -> -ENOENT) but never investigated whether a race actually
exists behind the bogus comment.  It does.

## What We Did

Full code reading of the interleaving, in this tree:

1. `shmem_decompress_folio` (shmem_compress.c:3336) and
   `shmem_decompress_group` (:2897): pin `ent` via
   `shmem_comp_entries_get` (RCU + `inc_not_zero`), decompress into a
   bounce, allocate a folio per slice, then
   `shmem_add_to_page_cache(sib, mapping, sidx, NULL, gfp)` -- install
   into `i_pages` with `expected=NULL`, i.e. "succeeds iff the slot is
   EMPTY".
2. `shmem_add_to_page_cache` (shmem.c:908): with `expected=NULL` any
   conflict -> -EEXIST; an empty slot always installs.  It knows
   nothing about `info->comp_entries` (separate xarray).
3. `shmem_undo_range` (shmem.c:1148) truncate hook: FIRST
   `shmem_comp_truncate_range` (:1183) claims (xa_erase) every
   `entries[]` slot in [start, end), THEN the `find_lock_entries`
   walks remove cached folios.  All under i_rwsem (setattr/fallocate/
   evict).
4. Caller locking: `shmem_file_read_iter` (shmem.c:3451) takes NO
   i_rwsem (its own comment: "reads (unlike writes) are called
   without i_rwsem protection against truncate"); `shmem_fault`
   (:2766) takes none ("we cannot take i_rwsem in fault");
   `shmem_file_write_iter` (:3577) takes i_rwsem exclusive;
   `shmem_write_begin` (:3377) documents "i_rwsem is held by caller".

### The interleaving (both single-page and group paths)

```
Reader (no lock)                          Truncator (i_rwsem)
-------------                             -------------------
filemap_get_entry -> NULL
entries_present -> true
decompress_folio: pin ent
  [group: load + decompress -- sleeps]
                                          i_size = newsize (truncate)
                                          shmem_undo_range:
                                            comp_truncate_range:
                                              claim+erase entries[idx..]
                                            i_pages walk: nothing installed
                                          unlock
add_to_page_cache(idx, expected=NULL)
  -> slot EMPTY -> install SUCCEEDS
folio stays in cache past i_size /
 inside a punched hole, "both state"
comment now false (entries[idx] == NULL)
```

### Why every existing defence misses it

- **The pin outlives the slot.**  `entries_get` pins before truncate
  erases the slot; a live pin never fails, so the `-ENOENT -> retry`
  branch in `shmem_get_folio_gfp` (shmem.c:2570) is never triggered by
  this race.  Nothing re-reads the slot after the pin.
- **-EEXIST never fires**: the truncate leaves `i_pages[idx]` EMPTY
  (that is the point of truncate), and the install target is that
  empty slot.
- **`expected=NULL`**: upstream's swapin
  (`shmem_swapin_folio` -> `shmem_add_to_page_cache(...,
  swp_to_radix_entry(swp), ...)`) installs CONDITIONAL on the slot
  still holding the value entry it read -- a truncated swap entry
  makes the install fail.  Our compressed entry lives in a different
  xarray, so there is nothing to pass as `expected`.  This is exactly
  the check the deleted stale comment claimed existed: the upstream
  defence mechanism exists, and the two-table mapping dropped it
  without a replacement.
- **i_size recheck does not exist** on the decompress branch (it
  returns early, like the upstream cache-hit path), and would not
  cover punch-hole anyway (no i_size change).

### Consequences if hit

- Stale uptodate folio(s) in `i_pages` at indexes >= new i_size
  (truncate) or inside a punched hole (punch): tmpfs's "beyond i_size
  / holes read as zeros" invariant is broken.  Observable: ftruncate
  down + ftruncate up (or a hole read) serves the OLD data instead of
  zeros -- a data-resurrection/corruption bug, the classic truncate
  bug class.
- Group case resurrects up to nr_pages-1 sibling folios per hit, not
  just the requested index.
- Stats drift: `mapping->nrpages` / NR_SHMEM / NR_FILE_PAGES inflated;
  the entry's swapped charge was already dropped by truncate, so the
  alloced gauge undercounts.  (Minor next to the correctness hit.)
- The racing reader itself consumes nothing stale (read_iter
  re-checks i_size after get_folio), and a full-page rewrite via
  write() overwrites a resurrected folio -- which is why the existing
  test_entry_race (readers pread vs writer ftruncate+rewrite loops)
  passes while the bug lives: it never reads a hole or an extended
  range.

## What We Learned

### Technical

- **The pointer-encoding pin is deliberately lifetime-only**: it says
  the ENTRY is alive, nothing about the SLOT still naming it.  That
  is correct for GC/reclaim, but the install path needs the slot's
  presence, not the entry's aliveness -- and the install path never
  re-checks it.  The session-56 encoding change (xa_erase claim) fixed
  a double-drop on the CLEAR side but introduced no check on the
  INSTALL side.
- **The upstream pattern for "install what you read from a value
  slot" is the `expected` parameter**, and our two-table mapping
  silently lost it.  Any future install-from-entry path must carry an
  equivalent revalidation.
- **Truncate's order inside `shmem_undo_range` is the key to a
  correct fix**: slots are cleared over the WHOLE range first, folio
  walks second.  Therefore any folio installed after the walk passed
  its index necessarily finds its slot already cleared -- a POST-
  install slot revalidation catches the race completely (a pre-check
  only shrinks the window).
- **Read/fault paths are the only exposed callers** (no i_rwsem);
  write-path decompress (write_begin) is excluded by the caller's
  lock.  Same split as the entry-race bug.

### Process

- The question sat in the backlog four logs because the stale comment
  was "fixed" (reworded) instead of investigated -- fixing a comment
  that describes a non-existent defence should always raise "does the
  defence need to exist?".
- Reading the caller locking (read_iter/fault/write_iter) is what
  turned "maybe a race" into "reachable race with a named
  interleaving"; it took one grep pass.

## What We Decided

- **The race is real and worth fixing now** (data resurrection past
  EOF / into holes; correctness class, blocks upstream).
- **Fix shape (designed this session, implemented next): post-install
  slot revalidation**, per slice, in both `shmem_decompress_folio`
  and `shmem_decompress_group`:
  - After `shmem_add_to_page_cache` succeeds, re-read
    `entries[sidx]`; if it no longer names `ent`, undo the install:
    `shmem_delete_from_page_cache(sib, NULL)` + folio unlock +
    `wake_up_folio_waiters` drain (the entry-race drain -- this
    removal can be the folio's last lock/unlock) + put, and return
    -ENOENT for the requested slice (retry; entries_present now false
    -> alloc path -> i_size check -> zeros/-EINVAL).
  - Group: keep sibling folios LOCKED from install until the whole
    group is validated, then unlock+put.  Holding the lock across the
    window (a) blocks a racing write_begin (so a "slot cleared" post-
    check can only mean truncate, never the write-invalidate, making
    removal safe without a dirty-state dance), and (b) blocks reclaim
    from detaching a sibling before its post-check.
  - Safety argument: slot alive at post-check (which runs after
    install) implies truncate's erase of that slot happens after the
    install, hence its i_pages walk -- which runs after the erase --
    happens after the install and removes the folio itself.  Slot
    dead at post-check implies we remove it ourselves.  Either way no
    folio survives past the truncate.
- No i_size-based check: it would not cover punch-hole, which is the
  same `shmem_undo_range` path.

## Rules Added

- **Any path that INSTALLS a folio derived from a compressed entry
  must revalidate `entries[idx] == ent` after the install** (the
  two-table analogue of upstream's `expected=` swap-install).  The
  pin guarantees the entry's life, not the slot's claim on the cache.
- **A comment describing a defence that does not exist in the code is
  a bug report, not a doc bug** -- rewording it must trigger an
  investigation of whether the defence is needed.

## What We'd Do Differently

- Would have investigated the stale comment the moment it was found
  (session 54) instead of rewording it (session 56) and letting it
  ride the backlog four logs -- the comment was a reviewer waving at
  exactly this race.
- The upstream `expected=` precedent should have been grepped when
  the two-table install sites were written; "why does swapin pass an
  expected entry and we pass NULL?" is a one-line question that would
  have found this before first boot.

## Files Changed

| File | Purpose |
|------|---------|
| `mission_log/2026-10-16_group-decompress-truncate-race.md` | this log (investigation only; no code yet) |

## Test Results / Benchmarks

None this session -- analysis only.  Reproducer plan (next): a test
that (1) writes+compresses a file, (2) loops readers pread against a
writer ftruncate-down/ftruncate-up and punch-hole, (3) after
quiescence verifies a full read of the extended range and the punched
hole is all zeros.  The existing entry-race test cannot see the bug
because it always rewrites full pages after extending.

## Next Steps

1. ~~Implement the post-install revalidation~~ -- DONE, see Session 66.
2. ~~Write the resurrection reproducer; verify FAIL on unpatched,
   PASS on fixed; KASAN loop~~ -- DONE, see Session 66.
3. Standing backlog (H618 re-measure, RFC assembly, plateau/CPU-freq,
   workload bed).

---

## Session 66: Fix Implemented, Reproducer Built, A/B Verified (2026-10-16)

(The investigation above is the series' session 65; this append is
session 66.)

### What We Did

**The fix (commit `abf3b8835c29`, mm/shmem_compress.c):**

- New helper `shmem_comp_entries_names(inode, index, ent)` -- RCU
  slot read, pointer comparison inside the rcu section (the caller's
  pin makes ABA impossible).
- `shmem_decompress_folio`: after the successful install, revalidate
  `entries[index] == ent`; on failure undo the install
  (`shmem_delete_from_page_cache(folio, NULL)` + unlock +
  `wake_up_folio_waiters` drain + put) and return -ENOENT (the
  get_folio_gfp retry then goes down the alloc path, which re-checks
  i_size -- correct zeros/-EINVAL past EOF).
- `shmem_decompress_group`: deferred finalize.  Slices stay LOCKED
  from install until the whole group is validated; `sibs[]` (kcalloc,
  heap because nr_pages reaches 1024) tracks installs; the
  requested-slice -EEXIST early return became a break so finalize
  still unlocks installed slices.  Holding the locks across the
  window blocks racing write_begin (a cleared slot can only mean
  truncate -- removing is safe, no dirty-folio hazard) and blocks
  reclaim from detaching a sibling before its check.  Dead slices
  get the same undo + drain; a dead requested slice yields -ENOENT.
- Stale-comment cleanup: the decompress_folio doc comment still
  described the removed {id, gen} generation scheme AND claimed the
  pin handled the truncate race; the decompress_group NOTE now names
  the third defence; an orphaned pre-pointer-encoding comment block
  (session-56 leftover, sat right before the current one) deleted.
- Gates: i386 + x86_64 compile, 0 errors 0 warnings.

**The reproducer (rig: `tests/test_trunc_resurrect.c`):**

- Per phase (A merge=1, B merge=8, A2/B2 with cell=0): seed 32 pages
  of 'A' (threshold=1, wm=0:0 -> every page compressed), then loop
  (main thread truncator + 4 pread-looping reader threads): pwrite
  full (repopulates compressed entries) -> ftruncate(HALF) -> 500us
  window -> ftruncate(full) -> scan [HALF,FILE_SIZE) for nonzero;
  then punch page 2 -> window -> scan the hole.  Any nonzero byte in
  a range the truncate/punch just cleared can ONLY be a decompress
  install against a cleared slot -- no TOCTOU ambiguity (unlike
  entry_race, the truncator never rewrites data readers observe, and
  a reader's stale read lands in its private buffer, never the file).
- Wired: init.sh `test.trres=1` boot mode + default-suite entry;
  run_qemu.sh `TEST_TRRES`/`TEST_TRONLY` knobs (+TR_ONLY env).

**A/B kernels:** both built with `build_arch_kernel.sh x86_64` (new
x86_64 case) from GIT WORKTREES -- pre-fix at `cc098c4cd639`
(/home/snide/linux-prefixe), fixed at `abf3b8835c29`.  Why worktrees:
see the stale-artifact finding below.

### Test Results (the decisive A/B)

| kernel | Phase A (merge=1) | Phase B (merge=8) | A2 (zs) | B2 (merge=8 zs) |
|---|---|---|---|---|
| pre-fix `cc098c4cd639` | PASS 0/3925 | **FAIL 720 hits/4388** | PASS 0/3921 | **FAIL 650 hits/4365** |
| fixed `abf3b8835c29` | PASS 0/3937 | PASS 0/4397 | PASS 0/3937 | PASS 0/4348 |

Hits = iterations where the extended range (or punched page) read
back nonzero.  Resurrected extent was the full 8-page group (32768
bytes) -- the group path resurrects whole groups, exactly as analyzed.

- 5/5 repeat runs clean on the fixed kernel.
- Entry-race regression on the fixed kernel: all 4 phases PASS
  quiescent (the new finalize drain and the entry-race drain
  coexist).
- Full KASAN suite on the fixed kernel: **exit 0**, zero KASAN/UBSAN
  splats, new test passes in-suite (0 hits at KASAN-slower iteration
  counts, ~2000-3300/phase).

### What We Learned

### Technical

- **The group path is the practical trigger, the single-page path is
  near-unreachable on fast x86/KVM.**  0 hits in ~3900 single-page
  iterations vs ~16% hit rate in group iterations.  Mechanism: the
  race needs the pin-to-install gap to span `shmem_undo_range`'s
  clear-then-walk sequence (slots cleared FIRST, folio walks after),
  i.e. tens of microseconds.  A 1-page decompress (~10us) rarely
  spans it; an 8-page group decompress (one crypto call + 8
  alloc/charge/memcpy/installs, ~100us) usually does.  The
  single-page revalidation stays (correct-by-analysis; slow targets
  -- TCG, 1GHz OpenWrt, heavy load -- widen the window) but the
  honest statement is: merge>=2 is where this bug bites.
- **The punch-hole flavor scored 0 hits even pre-fix in 6s** -- its
  exposure per iteration is one page's group for one brief window.
  Kept as a detector (it is the no-i_size-change flavor, worth
  covering), but the truncate flavor is the discriminator.
- **`build_x86_kernel.sh`'s kernels have NEVER booted.**  Two failure
  signatures on x86-64: "32-bit relocation outside of kernel!" with
  KASLR, "uncompression error" with nokaslr -- a broken payload, not
  a config toggle.  The builder predates the EFI fix and its O= dir
  carried 7 stale build generations.  Superseded by the validated
  `build_arch_kernel.sh x86_64` path; do not use it.
- **STALE-ARTIFACT CLASS, THIRD FLAVOR: a boot-broken payload from a
  tainted main tree.**  The same builder + same config produced a
  boot-failing kernel from /home/snide/linux (uncompression error)
  and a clean-booting one from a pristine git worktree.  The main
  tree carries stale generated files from historical IN-TREE builds
  (build_kernel.sh builds in-tree; elfconfig.h/pasyms.h were two
  prior catches).  Symptom this time: the gzip payload the
  decompressor reads is corrupt -- invisible to compile gates,
  visible only at boot.
- **The suite's compressor rotation needs CRYPTO_ZSTD (+LZO/RLE):**
  remount_cycle and multimount mount with compress=zstd; the sweep
  config only had LZ4 -> "compressor 'zstd' unavailable" -> -EINVAL
  -> two suite failures that look like kernel bugs but are pure
  config.  Also DETECT_HUNG_TASK/SOFTLOCKUP_DETECTOR for the
  hung_task_panic/softlockup_panic sysctls init.sh writes.

### Process

- **The A/B discipline carried the whole session:** the reproducer
  was only trusted after it FAILED on the pre-fix kernel ("verify the
  test is the test"), and the two suite failures were only classified
  as config artifacts by checking the serial log signature
  ("compressor unavailable" != data-path bug).
- **Worktree A/B builds** (git worktree add at each commit) gave
  pristine-source kernels without touching the main tree -- cheaper
  and safer than mrproper dance; the worktree doubles as the
  pre-fix/fixed artifact source.
- The builder advertised `[extra scripts/config flags...]` in its
  usage but NEVER applied them (`"$@"` unused) -- a silent no-op
  knob.  Fixed; now callers can override the fixed block (used for
  --enable CONFIG_KASAN).

### Rules Added

- **Bootable kernels are built from a pristine git worktree, not the
  main tree** -- the main tree's historical in-tree builds leave
  stale generated files that can corrupt the boot payload invisibly
  to compile gates.  (Until the main tree is mrproper'd once.)
- **A mount failure in the suite is a config smell first:** grep the
  serial log for "unavailable"/"Invalid argument" before hunting
  kernel bugs; the suite rotates compressors the config must carry.

### Files Changed (this session)

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` (linux, `abf3b8835c29`) | the fix: post-install slot revalidation, group deferred finalize, comment cleanup |
| rig `tests/test_trunc_resurrect.c` (new) | the resurrection reproducer (4 phases, truncate + punch flavors) |
| rig `guest/init.sh` | test.trres=1 boot mode + default-suite entry |
| rig `scripts/run_qemu.sh` | TEST_TRRES/TRONLY knobs; nokaslr on the cmdline (x86-64 sweep kernel dies in the decompressor under KASLR) |
| rig `scripts/build_arch_kernel.sh` | +x86_64 case, SRC= override (worktree builds), extra-flag application (was a silent no-op), zstd/lzo + hung-task/softlockup for suite compat |
| rig `scripts/build_x86_kernel.sh` | EFI disable (harmless; script superseded -- its kernels never booted) |
| `/home/snide/linux-prefixe` (git worktree) | pristine-source build worktree at the fix commit (pre-fix commit history available) |

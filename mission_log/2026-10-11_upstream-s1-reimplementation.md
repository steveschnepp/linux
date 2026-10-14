---
name: upstream-s1-reimplementation
description: upstream/s1 branch off Linux 7.3-rc6 -- the v1 slice reimplemented to production quality from the prototype, built and smoke-tested green; prep only, nothing posted
project: linux
---

# Mission Log: Reimplementing the Upstream Slice

Part 22 of the tmpfs transparent-compression series.  Session 38
(2026-10-11).  The brief: v1 is the target for entry and initial
acceptance.  Craft a new branch off master (Linux 7.3-rc6,
`a90ee4305c4a`) and selectively repatch the code, so the R&D branch
becomes the prototype and `upstream/s1` becomes the production-quality
reimplementation.  Strictly local preparation -- nothing is posted
anywhere.

## What We Did

1. **Branched `upstream/s1` off master.**  Bare base, no series: the
   working tree starts at zero compression code, so every line on the
   branch is a deliberate inclusion.

2. **Extracted the prototype as reference** (`git show
   feature/tmpfs-compression:<path>` into `out/proto/`) and cut the
   v1 subset out of the 3,656-line `shmem_compress.c` with
   line-precise range deletions (13 ranges, verified at both edges
   before every cut, executed bottom-up): the cell backend, merged
   groups (`store_merged`/`compress_group`/`decompress_group`), the
   watermark/hot-chill policy, the aging workqueue, the global
   writable sysfs defaults, and every cell accessor.  Then the seams:
   pool struct, `fill_sb`/`put_super`/`should_compress`/`show_options`
   rewritten lean, entry API signatures simplified (handles/nchunks
   parameters dropped with the merged-group machinery).

3. **Reimplemented the integration surface by hand** on the s1 base:
   `swap.h` (the `SWP_SHMEM_COMPRESS` type, comment carried over
   verbatim -- the encoding design was already reviewed-quality),
   `swapops.h` ({32b id, 23b gen} packing, verbatim), `shmem_fs.h`,
   `internal.h`, `Kconfig`, `Makefile`, and the 16 `shmem.c` hook
   hunks (options trimmed to `compress=`/`nocompress`/
   `compress_threshold=`; no `cell=`, no watermark, no merge).

4. **The `EXPORT_SYMBOL_GPL` correction landed**: the four exported
   shmem internals became `internal.h` declarations.  `shmem.c` and
   `shmem_compress.c` link into the same image; the prototype's
   exports were simply wrong for an in-tree consumer.

5. **Found that the large-folio guard already existed.**  The
   session-37 critique said "zero folio_test_large anywhere" --
   `shmem_comp_compress_one` skips on `folio_order(folio)` in its
   skip-list, which the grep missed because it never spells
   "large".  The critique was wrong; the guard was in the code.  The
   v1 slice inherits it unchanged.  Corrected in the record.

6. **Built and verified.**  `bzImage #118`, zero warnings.  checkpatch
   on the full slice: **0 errors, 7 warnings** (down from the
   prototype's 45): one MAINTAINERS nag (new file under mm/, no
   update needed), the deliberate `panic()`/`VM_BUG_ON` accounting
   assertions, and four split quoted strings in those panics.

7. **Wrote the v1 gate: `test_s1_smoke`.**  No existing rig test
   speaks the v1 option surface (they all pass cut options), so the
   smoke gate mounts `compress=lz4,compress_threshold=1`, does two
   full mount/compress/verify/truncate/umount cycles, checks
   `comp_stats` counters after write, `/proc/mounts` truth, and
   nocompress.  **6/6, exit 0, zero oopses on the KASAN kernel.**

8. **Committed s1 as three buildable patches** (`ecf1a59b8cc8`
   encoding+internals, `54e70dc688f3` pool+table+data path,
   `ba26d4dfd20c` shmem wiring) and the rig gate (`7f35f8c`).

## What We Learned

### Technical

- **Cutting a subset out of a mature file is mostly seam work.**
  The ranged deletions were mechanical and safe with edge
  verification; every bug came from a *seam*: a dangling comment
  opener (three times -- see below), a clipped `return`/`}` tail, a
  struct field left referenced by an accounting line.  The compiler
  finds them all in one build; the discipline is reading the build
  log before touching code.

- **My own mission-log rule got violated twice in one session.**
  "Start a code deletion at the comment opener" -- twice the range
  delete started mid-comment or mid-function and left a `*/` orphan
  or ate a tail (`entry_alloc`'s `return id; }`, `get_stats`'s
  comment head, `opts_set_algo`'s tail).  The rule exists precisely
  because this failure is recurrent; knowing the rule did not prevent
  it.  The mitigation that works is mechanical: after any range
  deletion, grep for orphan `*/` lines and rebuild immediately.

- **A userspace `#define FILE` shadows stdio's `FILE`.**  The smoke
  test named its test-file macro `FILE`; every later `FILE *f`
  declaration parsed as an expression.  Cost one build cycle; the
  error message ("'f' undeclared" at its own declaration line) is
  the tell.

- **Compression stats return to zero after a full read.**  Reads
  decompress and release the entries; the counters are gauges, not
  cumulative.  The smoke gate initially read stats after verify and
  saw zero -- the kernel was right and the test was wrong.  Same
  shape as session 2's "the bench measured the wrong thing" lesson.

- **The trace-build methodology paid for itself again.**  When the
  first smoke run showed zero compressed pages, three `pr_err`
  probes (compress_one entry, compress failure, should_compress
  false) in one build cycle localized it to the test's read order --
  the kernel path was fully exercised for all 24 pages.

### Process

- **Prototype-as-reference beats cherry-pick for a reimplementation.**
  The prototype's 153 commits contain decisions that were later
  reversed (staged handles, cell dispatch, sysfs defaults); copying
  the *final* file state and cutting down to v1 transmits only what
  survived review by measurement.

- **`build_tests.sh` continues silently past a failed test build.**
  Twice a broken test source shipped a stale binary into the rootfs
  and the guest ran the old one.  A rig improvement is owed: fail
  hard on test build failure (or timestamp-check the binary).

- **Nothing was posted anywhere.**  The brief's constraint held: the
  branch, the gate, the record -- all local.

## What We Decided

- **v1 shape confirmed in code**: `compress=`, `nocompress`,
  `compress_threshold=`; zsmalloc backend only; eager write-end
  compression with the skip-list guard (mapped/DMA-pinned/large/
  clean); read-only `comp_stats`; graceful pool-failure (uncompressed
  mount, never a failed mount); no remount changes.
- **Three commits, each building**, instead of the eleven-patch
  split sketched in the proposal.  The fine-grained series is cut
  from this branch when the RFC is actually assembled -- including
  the selftests and docs patches that do not exist yet.
- **The s1 kernel runs on the rig's KASAN config** -- the smoke gate
  is KASAN-grade evidence, same as every suite run.

## Rules Added

- **After any ranged code deletion, grep for orphan `*/` and rebuild
  before proceeding.**  The comment-opener rule needs a verification
  step, not just a intent.

- **Test macros never shadow libc/kernel identifiers.**  Prefix test
  file-path macros (`TEST_PATH`), never `FILE`/`FD`/`PATH`.

## What We'd Do Differently

- Would have written the smoke gate FIRST, against the v1 option
  surface, then cut the kernel to fit it -- the gate is the spec.
  As written, the gate came after and caught only the test's own
  bugs plus the stats-ordering surprise.

- Would have used `build_tests.sh` output instead of `/dev/null` the
  first time; the silent-stale-binary trap cost two QEMU cycles.

## Files Changed

| File | Purpose |
|------|---------|
| branch `upstream/s1` (3 commits) | the v1 reimplementation off Linux 7.3-rc6 |
| `mm/shmem_compress.c` | ~1,500-line core: pool, entry table, data path, stats |
| `include/linux/shmem_compress.h` | v1 API with the proven ownership contracts |
| `include/linux/swap.h`, `swapops.h` | `SWP_SHMEM_COMPRESS` + {id,gen} encoding |
| `include/linux/shmem_fs.h`, `mm/internal.h`, `mm/Makefile`, `fs/Kconfig` | plumbing, no exports |
| `mm/shmem.c` | the 16 hook hunks, 3 mount options |
| rig `tests/test_s1_smoke.c` (+ init.sh/run_qemu.sh modes) | the v1 gate |

## Test Results / Benchmarks

| run | result |
|---|---|
| s1 build | `bzImage #118`, 0 errors, 0 warnings |
| checkpatch, full s1 diff | 0 errors, 7 warnings (declared) |
| s1 smoke gate (KASAN kernel) | 6 passed, 0 failed, exit 0, 0 oopses |

## Next Steps

1. **Memory-failure answer** for `SWP_SHMEM_COMPRESS` entries
   (s1 patch candidate; the hook surface is known).
2. **In-tree selftests** (`tools/testing/selftests/`) -- port the
   smoke gate + remount-cycle gate.
3. **Docs**: port the rst to s1's surface, Sphinx-verify.
4. **The measurement story** for the cover letter: compressible-
   non-zero dataset vs uncompressed tmpfs, M-boot rigor.
5. **RFC assembly** (local only until the user posts): the 11-patch
   split cut from this branch, cover letter front-loading the
   `SWP_SHMEM_COMPRESS`-vs-pte-marker question.

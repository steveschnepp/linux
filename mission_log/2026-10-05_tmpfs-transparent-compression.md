---
name: tmpfs-transparent-compression
description: implemented transparent page-level compression for tmpfs (shmem), from plan to QEMU-validated prototype on branch feature/tmpfs-compression
project: linux
---

# Mission Log: Transparent Compression for tmpfs

---

## What We Did

The goal: give tmpfs transparent, per-page compression without reserving RAM
the way zram does. zram is a compressed block device that reserves memory
upfront; zswap only compresses cold pages during swapout. We wanted active
tmpfs files compressed in place, transparent to applications, configured via
sysfs/mount options like zswap's interface.

1. **Plan first.** Wrote `TMPFS_COMPRESSION_PLAN.md` (kept in the kernel tree
   as the one allowed accessory doc): architecture (zsmalloc pool + crypto
   acomp + xarray entries), rationale vs zram, phased implementation, and a
   QEMU-based test strategy.

2. **Designed the entry encoding iteratively.** First attempt packed the
   zsmalloc handle into the xarray value (`(handle << 12) | len`). That is
   wrong: zsmalloc handles are kernel pointers (`handle_to_obj()` dereferences
   the handle as an address). QEMU crashed in `zs_obj_read_begin()`. Final
   design: a global xarray ID table (`shmem_comp_entry_alloc/get/release`)
   maps a small ID -> `struct shmem_comp_entry` (handle + length + refcount);
   page cache slots store only the ID via
   `swp_entry(SWP_SHMEM_COMPRESS, id)`.

3. **Implemented in bisect-friendly commits** on `feature/tmpfs-compression`
   (~25 commits): Kconfig (`CONFIG_TMPFS_COMPRESSION`,
   `CONFIG_TMPFS_COMPRESSOR_DEFAULT="lz4"`), `include/linux/shmem_compress.h`,
   `mm/shmem_compress.c` (per-CPU crypto contexts, zsmalloc pool, stats),
   SWP_SHMEM_COMPRESS type in swap.h/swapops.h, superblock fields, mount
   options, pool lifecycle, sysfs under `/sys/fs/tmpfs/compression/`
   (compression, compressor, merge_pages, comp_stats), decompression on page
   fault, compression on write_end.

4. **Built a QEMU test rig** (`/home/snide/tmpfs-compress-test`): tinyconfig
   kernel (1.3MB bzImage, ~1min builds), static-initrd guest with busybox +
   static test programs, `run_qemu.sh` that propagates the guest test exit
   code. Tests: `test_seq_compress` (round-trip of `seq 1 100000`),
   `test_mmap`, `test_performance`, `test_concurrent`, `test_bench`
   (compressor x merge matrix), `test_multimount`.

5. **Debugged with QEMU as the oracle** -- every bug below was found by
   booting the kernel and running the tests, then fixed in its own commit:
   - **xarray self-deadlock**: `xa_alloc()` takes the xarray lock internally;
     wrapping it in `xa_lock()` spun forever in `queued_spin_lock_slowpath`
     (soft lockup). Removed manual locking; `XA_FLAGS_ALLOC` reserves ID 0.
   - **zsmalloc bounce buffer**: `zs_obj_read_begin()` writes into a
     caller-provided `local_copy` when an object spans two zspage pages;
     passing NULL caused a NULL deref on read. Now kmalloc's a bounce buffer.
   - **Compression globals inside `#ifdef CONFIG_TRANSPARENT_HUGEPAGE`**:
     builds without THP got undefined refs from the sysfs handlers. Moved
     above the THP block.
   - **`shmem_delete_from_page_cache()` argument**: replaced the
     delete(NULL)+add(__GFP_NOFAIL) sequence with a single atomic
     `shmem_delete_from_page_cache(folio, radswap)`.
   - **folio_lock recursion**: write_end held the folio lock when calling
     the compression path; moved compression after `folio_unlock()`.
   - **Eviction leak**: `shmem_free_swap()` treated compression entries as
     swap entries -> `Bad swap file entry` WARN + leaked blocks on umount.
     Routed `SWP_SHMEM_COMPRESS` through `shmem_comp_entry_release()`.
   - **Slab gfp WARN**: decompress paths passed mapping gfp (contains
     `__GFP_HIGHMEM`) to kmalloc; bounce buffers now use `GFP_KERNEL`.

6. **Added `merge_pages`**: compress N consecutive pages as one N*4KB block
   (mount `compress_merge=N` / `merge_pages=N`, sysfs `merge_pages`, 1..64).
   Groups are size-aligned; the ID is replicated in all N slots with
   `refs = N`; each slot that decompresses releases one ref. Discovery:
   `ZS_MAX_ALLOC_SIZE` is `PAGE_SIZE` in this tree, so merged blocks live in
   an entry-owned kmalloc buffer instead of zsmalloc.

7. **Measured with real data**:
   - `seq 1 100000` (588,895 B): lz4/merge=1 358,546 B (68.38%),
     zstd/merge=1 62,816 B (11.88%), zstd/merge=8 59,391 B (10.66%).
     lz4 barely benefits from merging (seq lines share only 4-5 byte runs,
     which is LZ4's minimum match length); zstd gains ~10% relative.
   - Benchsuite (untar of kernel .c/.o tars into compressed /tmp, timed
     untar + read, checksum-verified): rows completed at ~35 MB/s untar /
     ~124 MB/s read with 0 mismatches; the compressed merge=1/8 rows were
     interrupted by the oops described under Next Steps.
   - Multi-mount test (`test_multimount`): mounts plain + `compressor=lz4,
     merge_pages=1` + `compressor=zstd,merge_pages=8` simultaneously;
     kernel #15/#16 runs died on the algo/empty-string bugs (fixed) and the
     open oops, so its final table is still unverified.

8. **QEMU ergonomics**: guest `poweroff` needs ACPI the tiny kernel lacks, so
   QEMU idled forever; init now does `reboot -f` and QEMU runs with
   `-no-reboot` (QEMU exits on guest reboot). `run_qemu.sh` parses
   `Test exit code: N` from the serial log and propagates it as its own exit
   code; `timeout` remains only as a hang safety net.

9. **Hardening round** (most recent commits): `compressor=`/`merge_pages=`
   mount aliases matching the sysfs names, mount-time `crypto_has_acomp()`
   validation (typos fail the mount), sysfs `none` clears the global default
   instead of storing the string `"none"`, fill_super treats empty algorithm
   strings as unset. Note: valued mount options are now required; bare
   `compress` no longer parses (`fsparam_string` needs a value) and tests
   were updated to `compress=lz4,merge_pages=8` form.

---

## What We Learned

### Technical

- zsmalloc handles are pointers, not integers. Any design that packs them
  into compact values will corrupt them. Use an indirection table.
- xarray API functions (`xa_alloc`, `xa_erase`, `xa_store`) take the
  xarray lock internally; never wrap them in `xa_lock()`/`xa_unlock()` --
  single-CPU spin deadlock in `queued_spin_lock_slowpath`.
- `zs_obj_read_begin()` needs a caller-provided bounce buffer when an object
  spans two zspage pages; `NULL` only works for page-contained objects.
- `ZS_MAX_ALLOC_SIZE == PAGE_SIZE` in this kernel tree: zsmalloc cannot
  store blocks larger than 4KB, so merge_pages groups need a different
  backing store (entry-owned kmalloc buffer).
- Per-page LZ4 on 4KB blocks is weak on data whose redundancy is short
  (seq text: 68%); zstd gets 12% on the same data per page, close to
  whole-file compression. Compressor choice matters far more than
  merge_pages for ratio.
- merge_pages helps write bandwidth (one crypto call per N pages
  amortizes per-page fixed costs) but the MVP read path (install N slice
  folios per group) was ~4x slower than per-page reads.
- Compress-on-write pays full compression cost even for incompressible data
  (zstd wrote random data at 11-16 MB/s); a cheap entropy probe or deferred
  compression is needed.
- `fsparam_flag` and `fsparam_string` must not share a mount-option key:
  fs_parse returns the first match, so the flag silently rejected
  `compress=lz4`. Also, once only a string spec remains, a bare
  `compress` (no `=value`) no longer parses.
- tinyconfig + only-the-needed-options gives a 1.3MB kernel that builds in
  ~1 minute with ccache -- ideal for kernel bring-up loops.
- `qemu-system-x86_64 -no-reboot` exits the VM on guest reboot; combined
  with a guest `reboot -f` this beats any monitor/QMP watching scheme for
  automated test rigs.
- Kernel `CC = gcc` assignment ignores environment CC; ccache must be
  injected on the make command line (`make CC="ccache gcc" HOSTCC=...`),
  see `scripts/kbuild.sh`.

### Process

- The QEMU test rig paid for itself immediately: three correctness bugs
  (handle packing, xarray deadlock, bounce buffer) were found in the first
  boot, each in its own commit, so `git bisect` works.
- Commit per fix, author = git default user, no Signed-off-by (per user
  instruction; amend later if needed).
- Debug printks (`pr_err("DBG ...")`) + lockup detectors
  (`hung_task_panic`, `softlockup_panic`) turned a silent hang into a
  symbolizable RIP + call trace within one build cycle.
- Symbolizing oops RIPs against the built `vmlinux` (`nm vmlinux | sort |
  awk '$1 <= RIP'`) is the fastest way from "random crash" to the function.
- Realistic datasets (untar of kernel .c/.o trees) exposed bugs the
  synthetic ones missed (eviction path on umount, per-file overheads).

---

## What We Decided

- **Page-level compression** (4KB), not whole-file: only design that keeps
  mmap and random access working; whole-file compression breaks both.
- **LZ4 as default** for write-path cost; **zstd selectable** at runtime
  via sysfs or mount for ratio-critical workloads.
- **Entry ID table** between page cache and zsmalloc (handles don't fit in
  xarray values).
- **Replicated ID entries + refcount** for merge_pages groups; per-slot
  atomic claim via `shmem_add_to_page_cache(expected=...)`.
- **Entry-owned kmalloc data for merged blocks** (zsmalloc 4KB cap);
  single pages keep zsmalloc.
- **Compress in write_end for MVP** (simple, synchronous), with deferred/
  shrinker compression and an incompressibility probe recorded as follow-ups.
- **Test rig lives outside the kernel tree** (`/home/snide/tmpfs-compress-test`);
  kernel tree carries only `TMPFS_COMPRESSION_PLAN.md` and now
  `mission_log/`.
- **Documentation lives in code comments**; accessory .md files are
  forbidden (PLAN is the explicit exception), per user instruction.

---

## Rules Added

- **Docs in source:** no standalone doc files for implementation details;
  document in kernel-style comments. `TMPFS_COMPRESSION_PLAN.md` is the
  single exception.
- **Manual edits:** user performs edits via the edit tool, not generated
  patch scripts (scripts may silently mis-anchor).
- **Script workflow:** for shell artifacts use write -> chmod -> exec as
  separate steps.
- **Valued mount options:** tmpfs compression options always take values
  (`compress=lz4`, `compressor=zstd`, `merge_pages=8`); bare flags for
  these are gone.
- **ccache for kernel builds:** use `scripts/kbuild.sh` (wraps
  `make CC="ccache gcc" HOSTCC="ccache gcc"`).

---

## What We'd Do Differently

- Get the mount-option spec right in the first commit (single valued param
  from day one) instead of flag+string collision and a later break of the
  bare form.
- Validate algorithm names at parse time immediately, not after a silent
  uncompressed fallback confused three test runs.
- Build the eviction/truncate path awareness into the FIRST entry-type
  commit (audit every `xa_is_value` consumer when adding a new entry type);
  `shmem_free_swap()` bit us late.
- Add refcount/lifetime assertions in the entry table from day one -- the
  currently open oops (xa-value bit pattern used as a slab pointer in the
  single-page decompress/release path) smells like a double-release that
  asserts would have caught.
- Run the benchsuite against multiple kernel builds during development, not
  only at milestones; the untar dataset found the eviction bug.
- Start QEMU at 1G, not 256M; the zstd pool + repeated pool create/destroy
  caused OOM-killer ambiguity before the real bug was found.

---

## Files Changed

| File | Purpose |
|------|---------|
| `TMPFS_COMPRESSION_PLAN.md` | Design plan (allowed accessory doc) |
| `mission_log/2026-10-05_tmpfs-transparent-compression.md` | This log |
| `fs/Kconfig` | CONFIG_TMPFS_COMPRESSION, CONFIG_TMPFS_COMPRESSOR_DEFAULT |
| `include/linux/shmem_compress.h` | Entry/pool structs, API, kerneldoc |
| `include/linux/shmem_fs.h` | comp_pool/comp_enabled/comp_merge in shmem_sb_info |
| `include/linux/swap.h` | SWP_SHMEM_COMPRESS type slot |
| `include/linux/swapops.h` | make_shmem_comp_entry()/shmem_comp_entry_id() |
| `mm/Makefile` | shmem_compress.o build rule |
| `mm/shmem.c` | mount options, sysfs, compress/decompress paths, group support, eviction fix |
| `mm/shmem_compress.c` | pool, per-CPU crypto ctx, ID table, stats |
| `/home/snide/tmpfs-compress-test/` | QEMU rig: scripts, tests, initrd (outside kernel git) |

---

## Test Results / Benchmarks

Guest: QEMU x86_64, tinyconfig kernel (#16), serial console.

`seq 1 100000` (588,895 B), data verified line-by-line, 0 fails:

| compressor | merge_pages | stored | ratio |
|------------|-------------|--------|-------|
| lz4        | 1           | 358,546 B | 68.38% |
| lz4        | 8           | 378,597 B | 67.96% |
| zstd       | 1           |  62,816 B | 11.88% |
| zstd       | 8           |  59,391 B | 10.66% |

Untar bench (kernel .c/.o tars, 400 files, checksum-verified rows):

| dataset | algo | untar MB/s | read MB/s | mism |
|---------|------|-----------:|----------:|-----:|
| c-src   | none/lz4/zstd (merge=0 rows) | ~34 | ~124 | 0 |

Compressed merge=1/8 rows and the multi-mount table are pending re-run
after the open oops is fixed.

Synthetic 8MB bench (older datasets): lz4 write 73-150 MB/s, zstd write
11-17 MB/s (compress-on-write tax, even on incompressible data); merge=8
raised lz4 write ~73 -> 120 MB/s but dropped merge-path reads ~84 -> 20 MB/s
(MVP per-slice install overhead).

---

## Next Steps

1. **Fix the open oops** (kernel #16): RIP `ffffffff810f1096` in the slab
   allocator during `test_seq_compress`, RBX=`0x36a588003` (xa-value
   bit pattern used as pointer) -- audit `shmem_comp_entry_release()` /
   `shmem_comp_free()` ordering in the single-page decompress path and the
   `shmem_free_swap()` interplay; add refcount assertions.
2. **Capture the full matrix**: compressor x merge x dataset untar bench +
   `test_multimount` table on the fixed kernel at QEMU_MEMORY=1G.
3. Deferred compression (shrinker/workqueue) instead of synchronous
   write_end, plus a cheap entropy probe to skip incompressible pages.
4. Faster merge=8 read path (install one order-3 folio per group instead
   of N slices).
5. Per-mount statistics (current comp_stats are global).
6. Try larger merge (32/64) with zstd and long-range-redundancy data.
7. mmap() tests against compressed files (test_mmap exists but has not run
   since the entry-table redesign).

---

## Session 2: RAM accounting, the merge sweep, and the great bug hunt (2026-10-06)

### What We Did

Session 1 ended with an open oops ("xa-value bit pattern used as a slab
pointer") and a suspicion that merge_pages might not save any RAM. Session 2
answers both -- and the answers surprised us twice.

1. **RAM usage stats** (`629af1361aee`). Session 1's ratio table measured
   compressed *bytes*, not RAM -- merged groups store data in kmalloc buffers
   that the byte counts never showed. Added live footprint accounting to
   `struct shmem_comp_stats`, emitted as new `comp_stats` fields:
   `ram_pool_bytes` (zsmalloc pages via `zs_get_total_pages()` summed over
   the pool list), `ram_merged_bytes` and `ram_entry_bytes` (ksize-accurate,
   inc/dec in `shmem_comp_entry_alloc/release`), `ram_entries`,
   `ram_total_bytes`. `ram_total_bytes` vs the uncompressed size is the
   honest savings measure.

2. **merge cap 64 -> 1024** (`f77749acaaf3`). The cap existed only because
   `shmem_compress_group()` kept `struct folio *folios[64]` on the stack;
   1024 entries = 8KiB, so the array moved to `kcalloc()`. Cap enforced at
   all three sites from one header constant (`SHMEM_COMP_MERGE_MAX`). 1024
   is the hard ceiling: merged groups are `kmalloc(N * PAGE_SIZE)` and
   `KMALLOC_MAX_SIZE = 4MiB` on x86_64 (MAX_PAGE_ORDER 10); larger values
   would make every group buffer allocation fail silently.

3. **The merge sweep** (new `scripts/sweep_merge.sh` + `test.merge=` cmdline
   mode in the guest init). One QEMU boot per merge value (1,2,4,8,16) to
   keep measurements uncontaminated -- we *believed* the oops fired on the
   mount after a cycle, so single-cycle boots looked safe. Dataset:
   `seq 1 100000` = 144 pages, which divides evenly by all five merge
   values (no tail groups). Mount uses `compress_threshold=1`, not 0:
   `fill_super` does `ctx->comp_threshold ?: 65536`, so 0 silently means
   "default 64KiB" and the i_size gate would make coverage depend on merge
   size. The gate also explained the old "128 of 144 pages" mystery from
   session 1 -- it is by design, not a bug.

4. **Sweep verdict on RAM** (the answer to "do we really save RAM or did we
   lose it all to alignment"). Uncompressed baseline 589,824 B (144 pages):

   | merge | compressed data | RAM after write | verdict |
   |------:|----------------:|----------------:|---------|
   | 1     | 409,915 B       | 488,448 B       | ~17% saved |
   | 2     | 407,987 B       | 594,432 B       | net LOSS |
   | 4     | 405,974 B       | 592,128 B       | net LOSS |
   | 8     | 405,468 B       | 590,976 B       | net LOSS |
   | 16    | 405,128 B       | 590,400 B       | net LOSS |

   Merged kmalloc buffers total *exactly* the uncompressed size for every
   power of two (`kmalloc(N * PAGE_SIZE)` is exact page multiples, every
   group gets a full-size buffer, never shrunk to `comp_len`). Bigger groups
   compress better (-12.8% data at merge=16) but it does not matter: the
   data lives in buffers that always sum to the whole file. After a full
   read, everything decompresses (savings exist only for cold pages) and --
   pre-fix -- one entry + one full group buffer leaked per cycle.

5. **The bug hunt**, in four acts:

   a. *The +1 fingerprint.* Every sweep row showed
      `ram_entries == update_stats_calls + 1`, with full buffers and a leak
      surviving read+umount. `entry_alloc()` has exactly two callers, each
      followed unconditionally by `update_stats(true)` -- so a phantom entry
      should have been impossible.

   b. *Trace build.* `pr_err("DBG ...")` at alloc/release/stats plus
      `dump_stack()` in the `shmem_get_folio` decompress branch. The trace
      showed the smoking gun in four lines: `alloc id=0 ... data=X`,
      `alloc id=1 ... data=X` (same cbuf address -- freed and reused!), one
      missing stats line. `xa_alloc` had handed out **ID 0**, which both
      callers treat as "allocation failed": the group path freed the
      entry-owned cbuf (dangling pointer), skipped stats and the slot swap,
      and pinned the entry in the global table forever. Root cause:
      `xa_limit_32b = XA_LIMIT(0, UINT_MAX)` and -- contrary to what
      session 1's log recorded as fact -- `XA_FLAGS_ALLOC` does **not**
      reserve index 0 in this tree. Fix: `XA_LIMIT(1, UINT_MAX)`. The sweep
      went clean (entries == stats, zero drain after read).

   c. *But the oops survived.* The full suite still died at the 4th mount
      with the same `0xffff007366706d74` freelist garbage (= ASCII
      `"tmpfs\0"`). The phantom was a real bug, not THE bug.

   d. *KASAN.* Enabled on the test kernel -- which required
      `--disable CONFIG_SLUB_TINY` first: `KASAN depends on SYSFS &&
      !SLUB_TINY`. The suite then completed end-to-end (KASAN quarantine
      keeps freelists intact, so the corruption never detonated) but
      printed the verdict: **`BUG: KASAN: double-free in shmem_free_fc+0x84`**
      -- `shmem_free_fc()` freed `ctx->comp_compressor` twice, a duplicated
      `#ifdef/kfree()` pair from the session-1 hardening round. Every
      umount of a compressed mount double-freed the kmalloc-8 compressor
      string; the poisoned freelist detonated at the *next* mount's
      option-string allocation. That is why single-cycle sweep boots never
      crashed: nothing consumed the poisoned cache afterwards. The
      compression entry lifecycle -- the suspect for a whole session -- was
      innocent of the oops.

6. **Fixes committed** (4 commits, narratives in the messages):
   - `629af1361aee` RAM usage stats
   - `f77749acaaf3` merge cap 1024 + heap folio array
   - `5f0aca809a9d` the double-free in `shmem_free_fc` (the oops)
   - `a2a924bc5e4d` entry ID allocation and release paths: ID-0 limit;
     `shmem_free_swap()` released merged-group entries only when
     `handle != 0` (leaking entry+buffer per truncated group slot, no
     stats pairing); `shmem_decompress_group()` freed the entry-owned cbuf
     on dbuf failure and kfree'd an *uninitialized* pointer when the entry
     was gone; `shmem_comp_entry_get()` returned pointers outside RCU --
     replaced by `shmem_comp_entry_query()` (fields copied under RCU; the
     xarray stays private to shmem_compress.c)

7. **Test-rig fixes** (outside kernel git): `build_kernel.sh` expanded its
   ccache args unquoted (`CC="ccache gcc"` split into broken shell words ->
   /bin/sh "Unterminated quoted string" inside kbuild); now a bash array.
   `build_tests.sh` silently never rebuilt `test_seq_compress` (relative
   path after `cd tests/`) -- wasted one cycle on a stale binary; build it
   manually. `run_qemu.sh` gained `TEST_MERGE=` -> `test.merge=` cmdline.

8. **Comment discipline** (user rule, applied retroactively): code comments
   now state invariants only; the what-happened narratives (KASAN evidence,
   symptom tables) live in the git commit messages where they belong. Four
   narrative comments were rewritten before committing.

9. **Review handoff**: `/home/snide/tmpfs-compress-review-prompt.md` (outside
   the kernel tree) -- a self-contained prompt for a fresh session to review
   the full 30-commit branch diff: base SHA, architecture summary, what was
   fixed and why, the nine known-open issues, test-rig recipes (sweep, QEMU
   + gdb via GDB_PORT, KASAN), expected invariants.

### What We Learned

#### Technical

- `XA_FLAGS_ALLOC` does **not** reserve xarray index 0 in this tree.
  `xa_limit_32b` starts at 0; "reserved" IDs must come from the limit the
  caller passes (`XA_LIMIT(1, UINT_MAX)`). Session 1's log recorded the
  opposite as fact -- upstream doc habits lie; read the tree.
- `KASAN depends on !SLUB_TINY`. The tinyconfig debug recipe is:
  `scripts/config --disable CONFIG_SLUB_TINY --enable CONFIG_KASAN`, then
  rebuild. KASAN's quarantine lets a double-free-hobbled suite run to
  completion -- the *reports* are the signal, not the crashes.
- Cross-mount corruption hides in single-cycle test boots. Freelist
  poisoning from mount A's umount detonates when mount B allocates from
  the same cache. "Contamination-resistant" and "bug-revealing" test
  designs compete; run both.
- The oops site is not the bug site. A GPF in `___slab_alloc` during
  mount-option parsing was blamed on the compression entry lifecycle for
  an entire session; the cause was a duplicated kfree in fs_context
  teardown two subsystems away. When corruption manifests far from its
  cause, instrument the FREE paths (KASAN) before theorizing about the
  crash path.
- For lz4, merge_pages > 1 can **never** save RAM with the current design:
  full-size group buffers + power-of-two page-order rounding for >8KiB
  kmalloc eat the entire compression gain. Only zstd (12% ratio) would
  benefit from shrinking buffers to comp_len -- and even krealloc cannot
  help lz4 (680KiB rounds back up to a 1MiB order-8 block). The sweep
  table is the regression baseline for the fix.
- `ctx->comp_threshold ?: 65536` makes 0 indistinguishable from unset:
  `compress_threshold=0` cannot disable the i_size gate; use 1.
- `shmem_free_swap()` had `if (handle && release(id))` -- handle-gating
  the wrong thing: merged-group entries carry handle 0, so truncate and
  eviction never released them. Release must be unconditional; only the
  zsmalloc free is handle-dependent.
- `kfree(uninitialized_pointer)` is UB that looks like random freelist
  corruption -- `shmem_decompress_group()` did exactly that on the
  entry-gone path.
- Zsmalloc packing measured: lz4 seq pages cost ~3.3KiB of pool per 2.8KiB
  of data (mixed size classes -> partial zspages); still far better than
  merged kmalloc buffers.

#### Process

- Trace-build methodology paid for itself: four `pr_err` sites + one
  `dump_stack` -> one build cycle -> a four-line trace that named the bug
  (id=0, reused cbuf address, missing stats line). But the trace explained
  the *stats anomaly*, not the *oops* -- fixing the traced bug left the
  original symptom standing. Keep the original symptom (here: zero oopses
  in the full suite) as the acceptance criterion, always.
- The suggestion to run QEMU under gdb was right in spirit; KASAN
  answered first because it needs no breakpoint hypotheses. gdb
  (`GDB_PORT=1 ./scripts/run_qemu.sh`, `-s -S`) stays wired for
  stepping through whatever the review session turns up.
- Hunk-level staging (`git diff | awk split | git apply --cached`) gave
  bisectable commits when fixes interleaved in one file: the oops fix
  (`5f0aca809a9d`) is reviewable independently of the entry fixes
  (`a2a924bc5e4d`).
- Measure RAM with allocator-truthful counters (ksize, zs_get_total_pages),
  not requested sizes -- slab buckets and page-order rounding ARE the
  story.

### What We Decided

- **ID 0 stays the failure sentinel**; the xarray limit is clamped to
  `XA_LIMIT(1, UINT_MAX)` instead of restructuring the alloc API -- smallest
  change that restores the documented invariant.
- **`shmem_comp_entry_query()` replaces `shmem_comp_entry_get()`**; raw
  entry pointers must not outlive an RCU read-side section, so the API now
  copies fields out; the xarray itself is private to mm/shmem_compress.c.
- **Release is unconditional in eviction paths**; handle-gating applies
  only to the zsmalloc free; stats pair on the last release.
- **RAM stats are global** (per-mount remains session-1 follow-up #5).
- **merge cap = 1024 pages** as the kmalloc ceiling, single constant.
- **The RAM-spoiling fix is the next task** (deferred by explicit user
  decision): candidates are shrink-to-comp_len (helps zstd only),
  zsmalloc-chunked storage for merged blocks (split comp_len into 4KiB
  objects; page-granular, works for any ratio), or restricting merge to
  high-ratio compressors. The sweep table above is the before-picture.
- **Branch review goes to a fresh session** with the prepared prompt;
  commit messages carry the evidence chains.

### Rules Added

- **Comments are invariants:** code comments state why the code is as it
  is; what-happened narratives (bug stories, KASAN evidence) belong in git
  commit messages.
- **Cross-mount bugs need cross-mount tests:** single-cycle boots hide
  freelist/double-free bugs; keep a multi-cycle suite in the rotation.
- **KASAN recipe for this tree:** disable SLUB_TINY first, then enable
  KASAN; reports land in results/serial_*.log even when the suite passes.
- **Sweep invariants for the test rig:** every merge row must show
  `ram_entries == compressed_pages` after write and all-zero `ram_*` after
  a full read.
- **Acceptance criteria name the original symptom:** "no oopses in the
  full suite", not "the traced bug is fixed".

### What We'd Do Differently

- Session 1 recorded "XA_FLAGS_ALLOC reserves ID 0" without checking the
  tree. Verify API semantics against the source of the tree you are
  building, not against memory or upstream documentation.
- The oops was misattributed for a full session. The mission log's own
  next-step #1 ("audit release ordering") sent us hunting the wrong
  subsystem; a single KASAN boot at the *start* of the hunt would have
  named `shmem_free_fc` in two minutes. KASAN (or slub_debug=FZ) belongs
  in the rig's default recipe for any crash investigation.
- The sweep's single-cycle design -- chosen to avoid contamination -- is
  also what hid the cross-mount double-free. Next time: sweep AND
  a two-cycle canary boot.
- `build_tests.sh` silently skipping `test_seq_compress` cost a build
  cycle on a stale binary; rig scripts should fail loudly when a target
  binary is not produced.
- The header comment block in `include/linux/shmem_compress.h` still
  documents the abandoned handle-packing encoding -- stale docs survived
  two sessions because nothing forces doc review on redesign commits.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem.c` | comp_stats ram_* output; merge cap sites; heap folio array; free_fc double-free fix; free_swap release+stats pairing; decompress_group/folio fixes; query() call sites |
| `mm/shmem_compress.c` | XA_LIMIT(1) id clamp; shmem_comp_entry_query(); RAM footprint counters; get_stats pool-page sum |
| `include/linux/shmem_compress.h` | SHMEM_COMP_MERGE_MAX; ram_* stats fields; query() API; get() removed |
| `include/linux/swapops.h` | stale comment fix (ID 0 is the sentinel) |
| `/home/snide/tmpfs-compress-test/scripts/build_kernel.sh` | ccache args via bash array (quoting fix) |
| `/home/snide/tmpfs-compress-test/scripts/run_qemu.sh` | TEST_MERGE= -> test.merge= cmdline |
| `/home/snide/tmpfs-compress-test/scripts/sweep_merge.sh` | new: one-boot-per-merge RAM sweep |
| `/home/snide/tmpfs-compress-test/tests/test_seq_compress.c` | max_num + threshold args; MemFree snapshots |
| `/home/snide/tmpfs-compress-test/initrd/init` | sweep mode (test.merge=); merge=256/1024 runs |
| `/home/snide/tmpfs-compress-review-prompt.md` | fresh-session branch review prompt (outside kernel tree) |

### Test Results / Benchmarks

Post-fix verification on the committed tree (normal config, QEMU 1G):

- **Sweep** (`results/sweep_merge_20261006_103747.summary`): merge
  1/2/4/8/16 -- `ram_entries == compressed_pages` in every row (144/72/36/
  18/9), all `ram_*` counters **0** after a full read, data verified
  line-by-line, 0 oopses.
- **Full suite** (merge 1/8/256/1024 + zstd + bench + multimount, single
  boot): `Test exit code: 0`, zero `Oops:` lines. This is the sequence
  that always died at the 4th mount before the fixes (kernels #17/#19).
- **KASAN run** (debug kernel, SLUB_TINY off): suite completed; exactly
  one report, `double-free in shmem_free_fc+0x84` -- which became commit
  `5f0aca809a9d`.
- The RAM verdict table in "What We Did" step 4 is the baseline for the
  merge-buffers fix; note the post-fix rows compress *more* data (the
  previously-skipped first group now participates).

### Next Steps

1. **Fix the RAM spoiling** (the deferred task): merged-group buffers are
   never shrunk and kmalloc page-order rounding destroys lz4 merge savings.
   Evaluate zsmalloc-chunked merged storage (split comp_len into 4KiB
   objects -- page-granular, compressor-agnostic) vs shrink-to-comp_len
   (zstd-only benefit) vs restricting merge to zstd. Use the sweep table
   as the regression baseline; re-run `sweep_merge.sh` after the change.
2. **Fresh-session branch review** with
   `/home/snide/tmpfs-compress-review-prompt.md` (30 commits, base
   `a90ee4305c4a`).
3. Session-1 carry-overs still open: deferred compression + entropy probe;
   faster merge read path (order-3 folio per group); per-mount stats;
   threshold semantics (0 -> 65536); stale handle-packing doc block in
   shmem_compress.h; mmap tests; zstd merge matrix at 1G.
4. Optional hardening: refcount assertions in the entry table (session-1
   ask; the query() API makes them cheap to add), and a two-cycle canary
   boot in the rig's default rotation.

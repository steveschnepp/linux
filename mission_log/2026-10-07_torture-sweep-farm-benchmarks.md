---
name: torture-sweep-farm-benchmarks
description: the munin untar-farm torture sweep at 1..64 threads, and the ratio/speed/RAM analysis behind the configuration guidance (sessions 6-7)
project: linux
---

# Mission Log: the Torture Sweep and the Ratio/Speed/RAM Analysis

Part 4 of the tmpfs transparent-compression series, split verbatim from the
combined log `2026-10-05_tmpfs-transparent-compression.md` (kept until the
split is validated). Session numbers are preserved so cross-references keep
resolving.

Covers sessions 6-7 (2026-10-07): building the test_torture compilation-farm
benchmark (1..64 threads x 6 mount configs on the munin tree), fixing the
rig environment (KVM acceleration, ACPI/SMP), correcting the cell256k
mis-comparison, and the steady-state analysis that concluded lz4 +
merge_pages=1 is the Pareto default for small-file farms.

> **Note (2026-10-09):** session 6's "What We'd Do Differently" and
> session 7's "What We Decided" / "What We'd Do Differently" sections
> were NOT written contemporaneously.  They were reconstructed on
> 2026-10-09 from the pi session transcripts and the sessions' own
> text, when the linter started enforcing the template.  Content is
> grounded; in-the-moment hindsight is by definition missing.

- Previous: `2026-10-07_cellzram-block-device.md`

## Session 6: the torture sweep -- munin untar farm, 1..64 threads (2026-10-07)

### What We Did

Built `tests/test_torture.c`: a torture + scaling benchmark shaped
like a compilation farm. The workload pivoted twice during design
(synthetic object-file churn -> real dataset) and settled on:

- **Dataset**: munin-2.0.78.tar (5.4 MiB, 714 regular files, text-
  heavy perl tree) injected into the initrd, with a CRC32+size
  manifest generated on the host (`scripts/gen_manifest.c`, same
  IEEE CRC32 implementation compiled into the guest test) so the
  guest only ever computes CRC32s -- no reference untar, no cmp.
- **Per-thread lifecycle, in isolation**: mkdir -> untar (busybox
  tar) -> CRC32-verify every file against the manifest -> rm -Rf.
  Threads never touch each other's directories; the concurrency is
  happenstance. The tar's page cache is preloaded before the sweep
  (we measure tmpfs, not initramfs read speed); drop_caches is never
  used -- it does not happen in real life.
- **Sweep**: 1,2,4,..,64 threads x 6 mount configs (lz4 merge=1,
  lz4 merge=8, zstd, cell=256K+merge=4, zsmalloc baseline,
  nocompress), fixed 3s window per point, BENCH lines with
  verify_MBps / files_per_s / live comp_pages.
- **Post-storm RAM check**: after every thread's final rm -Rf lands
  together (no engineered storm needed), assert Shmem back to
  pre-test baseline and zero live compressed pages; MemFree gets a
  slack because dentry/inode slab caches from millions of churned
  files legitimately hold ~66 MiB (Slab 95 MB, SReclaimable 24 MB,
  Cached 5.4 MB = the tar page cache) -- and Shmem, the precise
  tmpfs metric, comes back to baseline within 200 kB.

**Rig environment bugs found en route** (none were kernel bugs):

1. **QEMU had no acceleration**: run_qemu.sh never passed -enable-kvm,
   so every rig run to date had been single-threaded TCG emulation --
   which is why a 4-vCPU guest used 100% of one host CPU. Fixed:
   `-accel kvm -accel tcg,thread=multi` (KVM first, multithreaded
   TCG fallback). With KVM, QMP confirms 4 vCPUs, and the sweep
   saturates ~400% host CPU in its windows (dips between points are
   mount/join phases -- not a kernel lock).
2. **CONFIG_ACPI was off in the rig kernel**: modern QEMU presents
   SMP topology via ACPI MADT, so a non-ACPI kernel fell back to the
   MP table and brought up 1 CPU even with -smp 4. Enabled
   CONFIG_ACPI=y; also raised CONFIG_NR_CPUS 2 -> 8 (the rig was
   capped at 2). Guest now reports 4 CPUs.
3. **run_qemu.sh's exit code lies on crashes**: a segfaulting test
   leaves no "Test exit code:" line, so the script returns QEMU's own
   exit (0). Noted; the serial log remains the source of truth.

**Test-side bug parade** (all found by the rig, all mine):

1. `threshold=1` is not a mount option -- it is `compress_threshold=`.
   Five of six configs failed to mount with EINVAL; only "plain" ran
   for two sweeps. The dead `absent` counter (declared, never
   incremented) then hid the real symptom.
2. `walk_verify` built top-level paths without the base directory
   (branched on `rel[0]` instead of `dir[0]`), so lstat missed
   everything and the recursion never ran; the recursion's return
   value (the file count) was discarded, so "extracted nothing" and
   "extracted 714 files" looked identical: short=13566 = 19 iters x
   714, zero everywhere else. Replaced the dead counter with a real
   `short_files` = manifest_count - found per iteration.
3. **Stack overflow via recursion + big frame**: `walk_verify` had a
   64 KiB read buffer on the stack; munin's tree depth (~4) x 72 KiB
   frames > the 256 KiB worker stack -> segfault (write to [rsp+0x18]
   right after `sub rsp,0x120c8`). Buffer went thread-local, stacks
   raised to 512 KiB.
4. `comp_pages` sampled after thread join reads 0 by construction
   (everything already unlinked); moved the sample inside the window.
5. First CPU sampler never checked liveness and happily reported 0%
   from a dead pid for 370 s while the real run had finished --
   exactly the failure mode the user called out ("the qemu did stop,
   but you didn't notice"). Fixed: pgrep -n each tick, break on exit.

### What We Learned

**Technical**

- Under KVM, a guest without ACPI sees 1 CPU no matter what -smp
  says; check CONFIG_ACPI before blaming QEMU or the workload for
  "why is my 4-CPU VM using one core".
- The block-device/tmpfs sysfs option names are easy to get subtly
  wrong (`threshold` vs `compress_threshold`); EINVAL on mount is the
  tell, but only if the failure is not swallowed by a dead counter.
- Recursive directory walks with per-frame scratch buffers are a
  stack-overflow trap; count-with-return + thread-local scratch is
  the safe shape.
- Shmem returning to baseline is the honest "RAM usage is 0"
  verification for tmpfs; MemFree never fully returns on a churn
  workload because slab caches (dentry/inode) hold metadata -- report
  the breakdown rather than widening the assertion blindly.

**Process**

- The manifest-upfront design (host-generated CRC32 table) removed an
  entire class of guest-side work and made verification O(files)
  without any reference state in the guest.
- Fixed-window weak scaling (constant work per thread, measure
  steady-state throughput) is the right bench shape for "how many
  build jobs can this box feed"; thread counts as powers of two give
  the saturation curve directly.
- Sample the metric you care about DURING steady state, not after
  teardown.

### What We Decided

- Window 3.0s x 7 thread points x 6 configs: ~4 min per boot, well
  within rig timeouts, enough iterations per point for stable
  numbers (T=1 does ~12-17 untar/verify/rm cycles per window).
- ENOSPC on a size=1G mount with a ~400 MiB live set is a failure
  (leak detector), not a skip.
- MemFree post-storm slack 256 MB with the residue printed (Slab/
  SReclaimable/Cached); Shmem assert stays tight (baseline + 4 MB).
- No drop_caches, no taskset, no engineered storm: the test stays
  plausible for a farm because farms do not do those things either.

### What We'd Do Differently

- Verify column picks against the raw output before narrating
  numbers; two awk column picks read the wrong fields in one
  session.
- Check the guest kernel config (ACPI) before blaming QEMU or the
  workload for CPU anomalies; the missing CONFIG_ACPI made every
  -smp VM single-core and aimed the investigation at the host.
- Sample host-side metrics alongside guest BENCH lines from the
  start; the "why 400%?" question this triggered cost a detour.

### Test Results

Final run (KVM, 4 vCPUs,2 G guest, kernel #62 with ACPI+NR_CPUS=8):

- **226 passed, 0 failed** across 42 sweep points; every CRC matched,
  every manifest entry present, zero ENOSPC, zero unexpected errors.
- Scaling at T=64 (verify_MBps, munin tree): **plain 182.9, cell256k
  179.0 (98% of plain), lz4_m8 116-140, lz4_m1 98.9, zstd 98.7,
  zsmalloc 98.9**. Headline: cell storage matches plain tmpfs
  throughput under farm load while zsmalloc-backed lz4 costs ~45% --
  the same story the merge sweep told for RAM, now for CPU.
- Compression engagement during windows: live comp_pages 1.4K
  (T=1) to 52K (zstd T=64).
- Host CPU during sweep windows: ~400% (4 KVM vCPUs), confirming the
  environment fix; no kernel-side serialization signal.
- Post-storm: Shmem 54892 kB vs baseline 54684 kB (tmpfs data
  freed), zero live compressed pages, residue explained by slab
  caches + the 5.4 MB tar page cache.

### Next Steps

- Re-run the torture sweep after future compression changes as a
  standing regression gate (it is wired: TEST_TORTURE=1
  QEMU_CPUS=4 ./scripts/run_qemu.sh).
- lz4_m1 flattens at ~99 MB/s from T=4 upward while cell256k keeps
  scaling to 179 -- worth profiling whether the merge=1 path or
  zsmalloc-free cell path is the differentiator at T=64 (likely the
  per-block kmalloc + entry table churn in the single-page path).
- run_qemu.sh should propagate the guest's real exit code (parse
  "Test exit code:" and fail loudly when it is missing instead of
  returning QEMU's status).

---

## Session 7: torture sweep corrections and the ratio/speed tension (2026-10-07)

### What We Did

Continued the torture sweep from session 6 with three corrections and
one new measurement dimension, ending in the full ratio/speed/RAM
analysis. All numbers below come from the final green run
(261 passed, 0 failed; KVM, 4 vCPUs, 2 G guest, kernel #62).

**Correction 1 -- cell256k never compressed.** The config was
`cell=256K,merge_pages=4,compress_threshold=1` with no `compress=`
option. `shmem_comp_opts_init()` seeds `compress` from the global
sysfs default, which is *off* (`compression: 0` at boot, visible in
the init output). The mount ran plain tmpfs with extra option
parsing: `comp_pages=0` at every sweep point, and its 184.5 MB/s was
just plain-tmpfs noise. This invalidated session 6's headline --
"cell storage = 98% of plain throughput" compared two *uncompressed*
runs.

**Correction 2 -- the honest backend comparison.** `lz4_m1` has no
`cell=` either, but fill_sb *auto-derives* cell_order=8 for a size=1G
tmpfs, so it is cell-backed all along. The like-for-like merge=1
comparison is therefore lz4_m1 (cells) vs zsmalloc (cell=0):
98.9 vs 99.1 MB/s, 47.7% vs 47.6% stored -- **parity**. The "zsmalloc
costs 45%" claim compared compressed zsmalloc against the
uncompressed cell256k; it was wrong. Cells' wins remain the
controlled-measurement ones: synchronous RAM return on free/discard
and the lower RAM footprint from sweep_merge.

**Correction 3 -- the CPU sampler.** Session 6's sampler never
checked process liveness and reported 0% from a dead pid for 370 s
while the real run had finished. Fixed: pgrep -n every tick, break
when the pid disappears. Also fixed the tick math: /proc/PID/stat
fields 14+15 (utime+stime) are in USER_HZ (100 ticks/s), so
CPU% = delta_ticks / elapsed_seconds -- the earlier "39040%" samples
were 100x inflated. Corrected samples: **~400% host CPU** in sweep
windows, 150-250% between points (mount/join phases). Four KVM
vCPUs saturating four host cores; no kernel-side serialization
signal at the environment level.

**New instrumentation -- ratio and RAM.** A one-pass comp_stats
snapshot (`total_compressed_bytes` / `total_uncompressed_bytes` /
`ram_total_bytes` / `compressed_pages`) is taken every ~200 ms inside
each window; the busiest instant (max live comp_pages) is kept, and
BENCH lines now carry `ratio_pct` (stored/original x100) and
`ram_mb`. Sampling the max matters: with merge_pages>1 live
comp_pages is bursty (a group exists only once N pages accumulate),
so a single sample can catch the pool between groups even though
compression is engaged -- this false-failed the new engagement guard
at lz4_m8 T=1 and cell256k T=2 until the max-over-samples fix.

**New hard guard.** Every config except "nocompress" must show
`comp_pages > 0` at its busiest in-window sample, or the point
fails. A mount that silently falls back to uncompressed (the
cell256k bug class) can never benchmark as plain tmpfs again.

### The ratio/speed/RAM analysis

Steady state at T=64 (4 vCPUs, munin farm workload: untar + CRC32
verify + rm -Rf per thread, 3 s windows):

| config | verify MB/s | stored/original | vs plain | RAM snapshot |
|--------|------------:|----------------:|---------:|-------------:|
| plain (nocompress) | 173.0 | -- | 100% | -- |
| zstd merge=1 | 98.9 | 37% (2.7:1) | 57% | 100M |
| lz4 merge=1 (auto cells, 1M) | 98.9 | 48% (2.1:1) | 57% | 174M |
| zsmalloc lz4 merge=1 | 99.1 | 48% (2.1:1) | 57% | 150M |
| lz4 merge=4 (256K cells) | 112.6 | 65-68% | 65% | 64M |
| lz4 merge=8 (2M cells) | 131.2 | 68-70% | 76% | 104M |

The pipeline ceiling without compression is 173 MB/s (tar extract +
CRC32 + tree walk on 4 vCPUs); every compressed config is measured
against that.

**Algorithm axis: zstd buys 11 points of ratio at ~1.4x the CPU per
byte.** 48% -> 37% stored is 23% less RAM for the same data. The
cost is visible wherever compression is the bottleneck: single job
12.4 vs 17.1 MB/s (-27%), T=16: 51 vs 85 MB/s (-40%). At T=64 both
sit at ~98.9 MB/s -- but NOT because zstd got cheap: lz4_m1
flatlines from T=4 onward (see below) while zstd keeps scaling
(57 -> 99 from T=32 to T=64). The saturation convergence masks
zstd's per-byte cost behind the merge=1 serialization ceiling; the
honest per-byte comparison is at T<=32.

**Merge axis: for a small-file farm, merge>1 is a trap.** munin's
714 files average ~9 KiB. Merge groups compress only when a FULL
group accumulates (merge=4 -> 16 KiB, merge=8 -> 32 KiB); tail pages
that never fill a group stay uncompressed by design. Result: ratio
collapses from 48% (merge=1) to 65-70% -- merge=8 stores ~40% more
bytes than merge=1 on this workload. Its speed win (131 vs 99 MB/s,
+32%) is partly real (amortized crypto calls per group) and partly
illusory (it compresses less data). The earlier untar/seq sweeps
showed the opposite ratio behavior on large sequential files, where
groups fill completely. Rule: match merge_pages to the file-size
distribution, not to the mount.

**Storage backend axis: cells and zsmalloc are CPU- and
ratio-equivalent** on this workload (98.9/48% vs 99.1/47.6%). The
ram_mb column is an instant snapshot (busiest sample of each window)
and is NOT trustworthy for cross-config comparison -- different
configs catch different live-data moments (cell256k's 64M vs
lz4_m1's 174M at T=64 reflects sample timing, not allocator
efficiency; the controlled sweep_merge methodology remains the RAM
authority). Cells' documented wins stand: synchronous buddy return
on free/discard/swap-slot-free, and the lower RAM footprint at
merge<=4 from the merge sweep.

**Bottom line for a compilation farm:**
- lz4 + merge=1 is the Pareto default: half the RAM (2.1:1) at 57%
  of the uncompressed pipeline throughput, and the best ratio per
  CPU-cycle of anything measured.
- zstd when RAM is the binding constraint (build caches, cold
  artifacts, CI scratch): 2.7:1 for ~1.4x the compression CPU; at
  low job counts expect 25-40% less throughput, at full saturation
  the difference hides behind other bottlenecks.
- Avoid merge>1 unless files are mostly multi-hundred-KiB; on
  small-file workloads it raises throughput by *not compressing*.
- Worst case (zstd, all cores busy) still stores 37% of the bytes
  while leaving 57% of the untar/CRC pipeline -- the tension is
  real but bounded.

**Performance lead left in the data:** lz4_m1 (merge=1) flattens at
~99 MB/s from T=4 upward while merge=8 keeps scaling to 131 and
plain to 173. merge=1 does one crypto call + one entry-table
insert + one kmalloc per 4 KiB page; the plateau smells like
serialization in that per-page path (entry table xa_lock? the
per-CPU context mutex? shmem_write_end folio_lock churn?), not CPU
exhaustion -- zstd, which does strictly more CPU work per byte,
scales past it. Profiling this is the next perf task; if the
per-page overhead is the issue, a merge=2 or per-CPU batched entry
allocation may recover much of merge=8's throughput without its
small-file ratio loss.

### What We Decided

- Session 6's headline claims are corrected in-place: cell256k was
  silently uncompressed (cell= without compress= follows the global
  default-off), lz4_m1 is cell-backed (parity with zsmalloc, killing
  the "zsmalloc costs 45%" claim), and the CPU sampler's tick math
  was fixed (/proc/stat ticks are USER_HZ).
- Every future sweep carries a `comp_pages > 0` engagement guard so
  a config can never again silently benchmark as plain tmpfs.

### What We'd Do Differently

- Assert per-config that the mount options took effect BEFORE
  publishing numbers; three of session 6's conclusions were
  measurement artifacts of configs not doing what their names
  claimed.
- Read counter semantics before writing the sampler; the 100x
  USER_HZ tick math made every CPU% fictional.

### Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_torture.c` | cell256k config fixed (compress=lz4), config table gains expect_comp column, engagement guard (max comp_pages > 0), comp_snapshot() ratio/RAM instrumentation, BENCH ratio_pct + ram_mb fields, liveness-aware sampling |
| rig `scripts/gen_manifest.c` | (session 6) chdir fix for tree-relative manifest paths |
| rig `scripts/run_qemu.sh` | -accel kvm -accel tcg,thread=multi |
| rig kernel config | CONFIG_ACPI=y, CONFIG_NR_CPUS=8 |

### Test Results

Final: **261 passed, 0 failed** (42 sweep points x 6 configs +
post-storm RAM checks). Host CPU ~400% in sweep windows. Post-storm:
Shmem back to baseline, zero live compression RAM; MemFree residue
explained by slab caches + the 5.4 MB tar page cache.

### Next Steps

1. **Profile the merge=1 plateau** (see above) -- the largest
   unanswered perf question in the dataset.
2. Re-baseline the farm numbers after any change to the per-page
   compress path; the torture sweep is the regression gate
   (TEST_TORTURE=1 QEMU_CPUS=4 ./scripts/run_qemu.sh).
3. A merge-sweep-style controlled run on the munin tree specifically
   (fixed data volume, per-merge RAM accounting) would confirm the
   small-file tail hypothesis with hard RAM numbers instead of
   ratio-only evidence.

---

---
name: matrix-thread-scaling
description: single-boot multi-threaded arm x watermark matrix (1.5G/2G) -- 3 arms, RAM/MB/s/CPU per cell; three build bugs caught before the numbers could be trusted; follows with a thread-scaling sweep on the 3 best bands
project: linux
previous: 2026-10-14_spec06-phase-c-triangle.md
---

# Mission Log: Multi-Threaded Arm x Watermark Matrix + Thread Scaling

Part 42 of the tmpfs transparent-compression series.  Session 53
(2026-10-14).  A new benchmark capability (test_matrix) plus the run it
enables: all three storage arms across the watermark triangle, at
1.5G tmpfs on 2G RAM, multi-threaded, measuring RAM + read/write MB/s +
CPU, in ONE boot via mount/umount (no reboot per combo).

## What We Did

### 1. Built test_matrix (new guest test) -- single-boot, MT, CPU-measuring

The existing test_fill is single-threaded, one-arm-per-boot, and measures
no CPU; sweep_fillwm.sh reboots per combo (21 boots for the triangle).
test_matrix.c replaces all that for the matrix question:

- **One boot.**  Each (arm, watermark) cell mount()s a fresh tmpfs, runs
  an MT fill + MT read-back, umount()s, and VERIFIES the pool released by
  checking MemFree returns toward its pre-mount baseline (with a sync +
  sleep(2) settle so lazy slab/RCU/LRU frees don't masquerade as a
  retained pool).  Turns a 43-boot sweep into one boot.
- **Multi-threaded.**  N writer threads (barrier-synced start), each
  writes its own file range; N readers likewise.  Default N=QEMU_CPUS.
- **CPU per phase** from /proc/stat busy-delta (system-wide) -- catches
  the kworker compression/GC threads that per-process accounting misses.
- **Arms:** 1 = tmpfs+zram swap (no watermark axis -> baseline, runs
  once), 2 = zmalloc, 3 = zcell (both get the full watermark triangle).
- Output to /tmp/mx.out, cat'd by init.sh AFTER the test exits.

### 2. The run: 1.5G tmpfs on 2G RAM, 4 threads, 3 arms, full triangle

43 cells, one boot, exit 0.  All 43 ZMX lines clean, zero CRC mismatch.
Columns: write MB/s, read MB/s, write CPU%, read CPU%, peak-available
RAM MB, compression-pool MB.

Baseline (tmpfs+zram): write 10404, read 20588, wcpu 95, rcpu 100.

Compressed arms, selected rows (zmalloc / zcell):

| band | write | read | pool | ram_avail |
|---|---|---|---|---|
| 0:0 (eager) | 2619 / 2323 | 5682 / 6600 | 855 / 829 | 1066 / 1087 |
| 0:60 | 4909 / 5915 | 11971 / 12329 | 238 / 235 | 570 / 582 |
| 40:60 | 5863 / 5943 | 12837 / 13165 | 240 / 235 | 572 / 574 |
| 20:80 | 11394 / 11635 | 20006 / 20256 | 18 / 14 | 382 / 378 |
| 100:100 | 13673 / 13798 | 21658 / 20946 | 0 / 0 | 381 / 371 |

(Full 21-band tables in the run log matrix_full3.log.)

## What We Learned

### Technical

- **The eager->deferred trade is a clean 5x dial.**  Eager 0:0 is the
  slowest write (2.3-2.6 GB/s) because every page is compressed inline;
  it buys the most RAM back (pool 830-855 MB, peak-avail ~1.07 GB).
  Never-compress 100:100 is 13.7 GB/s but the pool is empty and
  peak-avail collapses to ~371 MB (the whole 1.5G sits uncompressed).
  Every band step up trades pool for write speed smoothly.
- **The HIGH=80/100 rows are where write jumps to 11-13 GB/s** -- past
  the point the background worker can keep up, so most pages never get
  compressed (pool drops to 0-25 MB).  Deferral is free speed when
  there is headroom.
- **CPU is pegged 86-100% across the WHOLE matrix** -- 4 threads
  saturate the compression path regardless of band.  So the band trades
  RAM for write latency, NOT for CPU.  (Single runs; the mid-band wcpu
  spread 76-98% is partly noise, not signal.)
- **zmalloc and zcell are near-identical at every band** (within 5-10%).
  The cell backend's advantage never shows in a fits-in-RAM fill; its
  win was always GC/headroom under sustained churn, not raw fill speed.
- **zram is a different animal.**  Its 10.4 GB/s write is not inline
  compression -- it writes to page cache and swaps to zram lazily in the
  background.  It LOOKS like a deferred band but defers all compression
  cost to swap-out.  Not directly comparable to the two compressed arms.
- **The envelope, in one sentence, contrasting the earlier 2G/2G exact-
  fit triangle:** at exact fit (no headroom) eager 0:0 was the ONLY
  survivor and deferral OOM'd; at 1.5G/2G (512 MB headroom) nothing
  OOMs and deferral becomes free speed.  Headroom is what makes
  deferral viable.

### Process -- three build bugs caught before the numbers could be trusted

All three would have silently corrupted the deliverable; each was caught
by verifying the artifact, not trusting the build.

1. **Files were 128 KB, not 32 MB.**  I wrote `for (p = 0; p < FILE_MB;
   p++)` where FILE_MB=32 -- that is 32 PAGES, not 32 MiB.  Each thread
   wrote one tiny file; `wrote_mb=0.5` in the smoke run gave it away.
   Fixed with an explicit PAGES_PER_FILE.
2. **Kernel printks split ~40% of result lines mid-field.**  The rig
   passes ignore_loglevel, so pr_info (pool create/destroy) hits the
   serial console and interleaves with printf, corrupting values
   (e.g. "gauge_k" + "shmem_comp:").  A single write() of a pre-built
   buffer was NOT enough (the console is not atomic across CPUs).  Fixed
   the rig's way: write results to /tmp/mx.out, cat it in init.sh AFTER
   the test exits -- removes the concurrency entirely.
3. **The staged-binary trap (the one that cost a full re-run).**  I
   gcc'd directly to build/bin/test_matrix, which packs to /test_matrix
   in the guest.  But build_tests.sh outputs to build/bin/bin/ ->
   /bin/test_matrix, and init.sh invokes /bin/test_matrix -- the OLD
   binary.  So the new binary never ran: /tmp/mx.out was never written
   (old binary printf'd to stdout) and init.sh cat'd a missing file.
   Caught because "cat: can't open /tmp/mx.out" plus 43 ZMX lines on the
   CONSOLE (not the file) was the unmistakable signature of the old
   binary.  Fixed by building via build_tests.sh AND verifying the
   STAGED artifact (strings build/rootfs/bin/test_matrix) referenced
   mx.out before running.  This is exactly the repo's "verify the staged
   artifact, not the compiled one" rule earning its keep.

Also fixed en route: an inverted leak check (a POSITIVE free_delta means
more free after umount = fine; the earlier "leak" flags were +12-15 MB,
i.e. non-leaks), and a `sync;` (no-op statement) that had to be
`sync();` for the settle delay to actually run.

## What We Decided

- test_matrix is the tool for arm x watermark x thread questions going
  forward; test_fill stays for the single-cell quick check.
- For thread-scaling, probe the 3 bands that span the compression-work
  spectrum: **0:0** (eager, heaviest inline, pool 855 MB),
  **40:60** (balanced mid, sweet spot), **20:80** (deferred, pool ~15 MB,
  write already ~11-12 GB/s).  These are the informative points for
  whether threads help: heavy/mid/light compression work.

## Rules Added

- **Verify the STAGED binary, not the compiled one.**  After any
  build_tests/build_rootfs, confirm the artifact at the PATH init.sh
  actually invokes (strings/grep the staged file) before trusting a run.
  Direct gcc into build/bin/ ships to the wrong guest path.
- **Results that must survive kernel printks go to a FILE, cat'd after
  the test exits -- not to the console.**  A single write() is not atomic
  against the serial console under SMP; the file+late-cat is.
- **Check getenv() results for EMPTY strings, not just NULL.**  init.sh
  exports absent params as "", and atoi/atof("")==0 silently clobbered
  defaults (cap->0 capped every fill; zram->0).  Guard on e[0].

## What We'd Do Differently

- Would have built via build_tests.sh (not direct gcc) from the start
  and verified the staged path -- the direct-gcc path shipped to the
  wrong guest location and cost one full 43-cell re-run.
- Would have written results to a file from the FIRST version (the rig
  already does this for test_fill); the console-output attempt and the
  single-write() attempt were both predictable failures given
  ignore_loglevel.
- The leak check should have been thought through before the first run
  (positive-vs-negative delta); as written it flagged non-leaks and I
  had to re-derive the correct direction after the fact.

## Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_matrix.c` (new) | single-boot MT arm x watermark matrix, /proc/stat CPU, file output, pool-release check |
| rig `guest/init.sh` | matrix mode (test.matrix=1 + MX_* params), cat /tmp/mx.out after exit |
| rig `scripts/run_qemu.sh` | TEST_MATRIX + TEST_MX* -> test.mx* cmdline |

## Test Results

- Matrix run (1.5G/2G, 4 threads, arms 1+2+3, full triangle): 43 cells
  in one boot, exit 0, 43/43 clean ZMX lines, 0 CRC mismatch, 0 garbled.
  Numbers tabulated above; full log matrix_full3.log.
- Pool-release check: no cell showed a retained pool beyond settle noise
  (the few negative free_delta values are within the lazy-free envelope
  after sleep(2), not leaks).

## The thread-scaling sweep (2026-10-14)

Follow-up: thread-scaling on the 3 best bands, zcell (primary backend;
the matrix showed zmalloc ~= zcell so one arm suffices).  1.5G/2G, 8
vCPUs (so the 8-thread case is not oversubscribed), ONE boot via the new
MX_TSCALE (comma thread-count list) -- 3 bands x 1/2/4/8 threads = 12
cells.  Bands chosen to span the compression-work spectrum: 0:0 (eager,
heaviest inline), 40:60 (balanced mid), 20:80 (deferred, pool ~15 MB).

### Write MB/s

```
threads:   1       2       4       8     | 1->8
0:0      713    1267    2134    2808     |  3.9x
40:60   1825    3252    5358    7638     |  4.2x
20:80   3124    6250   12350   12660     |  4.0x
```

### Read MB/s

```
threads:   1       2       4       8
0:0     1619    3893    6124    6720
40:60   4304    8137   12617   11072   <- regress at 8
20:80   6049   10180   18278   15261   <- regress at 8
```

### CPU busy% (write phase)

```
0:0      12.5    24.9    49.7    87.6
40:60    12.6    24.4    44.5    80.6
20:80    12.5    24.5    47.5    78.1
```

### What we learned

- **Near-linear to 4 threads, band-dependent at 8.** 1->2 and 2->4 are
  ~1.7-2.0x on every band -- clean scaling, no contention to 4. The
  4->8 step diverges: eager 0:0 still gains 1.32x (CPU 87.6%, the
  highest -- it does the most inline compression so keeps all cores
  busy); deferred 20:80 is FLAT (1.02x, 12.3->12.7 GB/s).
- **The deferred band hits a NON-CPU bottleneck at 4 threads.** 20:80 at
  t=8 is only 78.1% CPU yet throughput plateaus -- so the limit is
  memory bandwidth or write-path/worker serialization, not core count.
  Eager is CPU-bound (still climbing at 88%); deferred is not.
- **Reads REGRESS at 8 threads on the deferred bands** (20:80
  18.3->15.3 GB/s, -16%; 40:60 12.6->11.1, -12%).  Read sweet spot is
  4 threads.  More readers than useful contend on the decompress path
  and page-cache locks.
- **Practical: 4 threads is the efficient point for every band.**  Beyond
  4 only eager still gains; deferred bands are faster in absolute terms
  but scale worse because they are already past their (non-CPU)
  bottleneck at 4.

### Process: strtok bug cost the first run

The first sweep produced 4/12 cells with exit 0 -- a PARTIAL, not a
pass.  Cause: the band loop and the thread loop both used plain
strtok(), which shares ONE static parse pointer; the inner thread loop
clobbered the outer's state after the first band, so only 0:0 ran.
Caught because "exit 0 with 4 of 12 cells" was treated as a red flag
not a result (same discipline as the vacuous-phase-gate and ZBLK=0
catches).  Fixed to strtok_r with SEPARATE save pointers (wm_sv for the
band loop, tok_sv for the thread loop); the re-run gave all 12 clean.
Note for the record: nested strtok over the same delimiter is a latent
bug whenever an inner loop tokenizes inside an outer loop -- use
strtok_r with distinct saveptrs.

### Files Changed (thread-scaling)

| File | Purpose |
|------|---------|
| rig `tests/test_matrix.c` | run_cell takes per-cell nt; MX_TSCALE thread-count list; strtok_r fix |
| rig `guest/init.sh` | parse test.mxts -> MX_TSCALE |
| rig `scripts/run_qemu.sh` | TEST_MXTS -> test.mxts cmdline |

### Test Results (thread-scaling)

- 12 cells (3 bands x 1/2/4/8), one boot, exit 0, 12/12 clean, 0 CRC
  mismatch, 0 garbled.  Full log tscale2.log.

## Audit: what the analysis was missing (2026-10-14)

Self-critique of the matrix + thread-scaling numbers, then the missing
experiments.  Five weaknesses found; two changed conclusions.

### The five weaknesses

1. **Every number was n=1.**  No error bars.  The "4->8 plateau" on
   deferred and the "read regression at 8 threads" rested on single
   runs.  Unacceptable per the bench discipline.
2. **Warm-cache reads.**  Read-back ran right after write with
   decompressed folios still in page cache -- measured
   decompress-from-CACHE, not from-pool.  Possibly optimistic.
3. **Every page byte-identical.**  gpage generated once, written
   393216x.  Unrealistically uniform for lz4 (though -- see below --
   this turned out NOT to matter for a per-page compressor).
4. **"Non-CPU bottleneck at 4 threads" was inferred, not measured.**
   Concluded from CPU=78%<100%; never measured bandwidth or locks.
5. **Thread-scaling confounded by CPU frequency.**  No ACPI cpufreq in
   the guest; under KVM the host controls turbo, so 1-thread may boost
   and 8-thread drop clocks.  Unmeasurable from the guest -- a stated
   limitation.

Also: `avail_peak` is sampled AFTER the write loop (end-of-fill, not a
true peak), and the zram baseline stays incomparable (lazy swap).

### A bug the audit caught in its own fix

Attempted weakness-3 fix (per-page-unique data) called fill_page() -- a
2048-step PRNG per page -- INSIDE the timed write loop.  Throughput
COLLAPSED 4x (20:80 t=4 write 12350 -> 3111 MB/s) while CPU% stayed
flat (47.5 -> 48.3): it was measuring userspace PRNG, not kernel
compression.  The identical-page originals were the CORRECT
measurement.  Deeper: cross-page uniqueness buys nothing for lz4
(per-page compressor, never sees across-page redundancy) -- it changes
neither per-page time nor ratio.  Fixed by pre-building a 256-page
pool OUTSIDE the timed loop and cycling it: non-identical pages, zero
PRNG in the measured path.  Throughput recovered to ~10584.

### Missing test 1: error bars (n=5, write MB/s, mean +/- sd)

| band | t=1 | t=2 | t=4 | t=8 |
|---|---|---|---|---|
| 0:0 eager | 725 +/- 18 | 1275 +/- 22 | 2223 +/- 33 | 2840 +/- 32 |
| 20:80 deferred | 2909 +/- 147 | 5716 +/- 210 | 10584 +/- 257 | 10880 +/- 528 |

- **CONFIRMED: deferred plateaus at 4.**  4->8 delta (296) is under one
  stddev (528) -- flat, not a gain.  The earlier "4->8 plateau" claim
  survives the error bars.
- **CONFIRMED: eager still gains at 8.**  2223->2840 (+28%), tiny sd.
- **RETRACTED: the "read regression at 8 threads."**  With n=5 and the
  fixed data, reads rise monotonically (20:80: 4535->8842->15421->19690)
  -- no regression.  The earlier claim was n=1 noise.  The "4-thread
  read sweet spot" is withdrawn.
- Caveat: the full read-verify (memcmp/page) runs inside the timed read
  loop, so READ numbers carry ~10-15% verify overhead; WRITE numbers are
  clean (pool fix).  Trust writes more.

### Missing test 2: cold read (drop_caches before read)

| band | pool | warm read | cold read |
|---|---|---|---|
| 0:0 | 848 MB | 5908-6278 | 5936 |
| 40:60 | ~240 MB | -- | 11861 |
| 20:80 | 24 MB | 15421 | 17064 |

- **Weakness-2 concern DEFUSED for the eager band.**  0:0's
  decompressed folios are file-backed cache that drop_caches genuinely
  drops, so cold IS decompress-from-pool.  Cold ~= warm: the read
  numbers were not meaningfully inflated; lz4 decompress across 8
  threads is cheap vs memory bandwidth.
- **Caveat for deferred bands:** 20:80 has only 24MB compressed, so
  most pages are uncompressed shmem that drop_caches cannot drop --
  "cold" is not cold there.  No conclusion, and low stakes (little
  decompression happens at that band).

### What still stands untested

- **Weakness-4 (plateau mechanism):** still inferred, not measured.  The
  error bars CONFIRM the plateau is real, but whether it is memory
  bandwidth, write-path serialization, or the background worker remains
  unseparated.  Would need lockstat or a bandwidth-bound control.
- **Weakness-5 (CPU frequency/turbo):** unmeasurable from the guest;
  stated as a limitation.  The tiny error bars (eager +/- 32 on 2840)
  suggest host freq is stable across reps, which bounds -- but does not
  eliminate -- the confound.

### Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_matrix.c` | pre-built page pool (PRNG out of timed path); full read-verify (badpages); MX_COLDREAD; MX_REPEAT + rep= field |
| rig `guest/init.sh`, `scripts/run_qemu.sh` | MX_COLDREAD / MX_REPEAT plumbing |

## Next Steps

1. Thread-scaling sweep on 0:0 / 40:60 / 20:80, 1/2/4/8 threads, zcell
   (primary backend; zmalloc shown near-identical so one arm suffices
   unless a difference emerges).
2. Consider a sustained-churn variant (the workload bed, spec 05) where
   the cell backend's GC/headroom advantage would actually show -- this
   matrix is a fits-in-RAM fill and cannot reveal it.
3. Standing items unchanged: RFC assembly (two-table), M-boot
   median+IQR, arm-3 rm/synchronous-free at 64M.

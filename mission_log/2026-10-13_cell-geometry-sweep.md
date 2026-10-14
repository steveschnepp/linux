---
name: cell-geometry-sweep
description: sweep cell size (16K..4M) vs cell count at a fixed 2G tmpfs -- write throughput scales with cell size and plateaus ~512K-1M, read is flat; the "it depends on the workload" tradeoff made concrete
project: linux
previous: 2026-10-13_dual-kernel-setup-and-tmpfs-equals-ram-bench.md
---

# Mission Log: The Cell-Geometry Sweep

Part 35 of the tmpfs transparent-compression series.  Session 51
(2026-10-13).  A measurement session.  The user's question: with the
cell-directory now sized from the mount, what cell geometry is best --
more/smaller cells or fewer/bigger ones -- and their instinct that "it
depends on the workload."  Swept cell size against cell count at a fixed
2G tmpfs to make the tradeoff concrete.

Source tree clean; rig committed; both kernels prebuilt; KASAN synced.

## What We Did

### 1. Wired a cell-size override so the geometry can be swept

`test_fill`'s arm-3 mount hardcoded `cell=64K`.  Added
`test.fillcell=<size>` through the kernel command line (the same path as
`test.fill=`/`test.fillmb=`), so a sweep can vary `cell=` at a fixed
tmpfs.  Because `nr_cells` now derives as `size= / cell_size`, varying
`cell=` sweeps *both* the cell size and the cell count together -- which
is exactly the "more/smaller vs fewer/bigger" axis.  NULL falls back to
64K.  (First attempt used a host env var, which never reached the guest;
the guest reads params from the cmdline, so it was wired through that.)

### 2. The sweep

2G tmpfs (uncompressed budget; the cells hold the ~1G compressed image
with the derived 2x headroom), `cell=` from 16K to 4M (order 2..10),
read-back on, one clean boot per geometry:

| cell | order | nr_cells | write | read |
|---|---|---|---|---|
| 16K | 2 | 131072 | 276 MB/s | 1967 |
| 32K | 3 | 65536 | 552 | 1998 |
| 64K | 4 | 32768 | 735 | 2000 |
| 128K | 5 | 16384 | 769 | 1941 |
| 256K | 6 | 8192 | 813 | 1959 |
| 512K | 7 | 4096 | 817 | 1990 |
| **1M** | **8** | **2048** | **818** | 1984 |
| 2M | 9 | 1024 | 820 | 1994 |
| 4M | 10 | 512 | 818 | 1992 |

All CRC ok; compression ~2:1 at every geometry.

## What We Learned

### Technical

- **Write throughput is strongly cell-size-sensitive and plateaus.**
  It climbs from 276 MB/s (16K cells) to ~817 MB/s and saturates around
  512K-1M (order 7-8); beyond that, flat.  The mechanism is cell
  allocations: a tiny cell is refilled (alloc_pages + directory slot)
  every ~16K of compressed data, so 131072 small cells pay ~64x more
  allocations on the sequential-write path than 2048 big ones.

- **Read throughput is geometry-insensitive** (~1950-2000 MB/s
  everywhere).  A read is a bounded memcpy within one cell; the cell size
  never enters the path.

- **The default derivation is well-placed for this workload.**  A 2G
  mount derives order 8 (1M cells), which sits exactly on the write
  plateau -- so the size-based default is not just "reasonable," it is
  the write-optimal point for a sequential fill.

- **The bench's explicit `cell=64K` is in the slow region.**  The
  fill/rationalization benches mount arm 3 with cell=64K (order 4,
  ~735 MB/s write) -- ~10% below the order-8 plateau.  Worth revisiting
  whether those benches should use the derived default instead.

- **The fill workload only shows half the tradeoff.**  Bigger cells
  reclaim more coarsely (a cell is freed only when nearly empty, so a
  4M cell with one live block pins 4M); smaller cells evacuate finer.
  A sequential fill never frees sparsely, so it cannot see the regime
  where small cells win.  The "it depends" answer needs a churn/sparse
  bench to complete it.

### Process

- **Sweeping a coupled axis means sweeping one knob.**  Because
  `nr_cells` derives from `size=/cell_size`, varying `cell=` moves both
  the size and the count along a fixed-capacity line -- no second knob to
  coordinate.

- **The guest reads params from the kernel command line, not the host
  environment.**  The first override attempt (a host env var) silently
  fell back to the default in every run; wiring it through the cmdline
  (like every other test param) fixed it.  A silent fallback is the
  failure mode to watch for in rig param plumbing.

## What We Decided

- **For sequential-write workloads the sweet spot is ~512K-1M cells
  (order 7-8)**, which the default derivation already picks.  No default
  change needed.

- **The churn/sparse side of the tradeoff is unmeasured** -- a follow-up
  bench (overwrite + free churn, measuring retained memory and GC cost
  per geometry) would complete the "it depends" answer.

## Rules Added

- **When two derived quantities are coupled by a formula, sweep the one
  knob that moves both** -- do not coordinate two sweeps that the
  derivation already locks together.

- **A rig param that silently falls back to its default is a bug.**  If
  an override is expected, verify the guest actually received it (the
  mount line) before trusting the numbers.

## What We'd Do Differently

- Would have wired the override through the command line from the start
  (every other test param does); the host-env attempt cost one full
  9-point sweep that measured the same geometry nine times.

## Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_fill.c` (`13d24a5`) | `fill_cell` from argv[5]; arm-3 mount uses it |
| rig `guest/init.sh` (`13d24a5`) | parse `test.fillcell=`, pass as 5th arg |
| rig `scripts/run_qemu.sh` (`13d24a5`) | append `test.fillcell=` to the cmdline |

## Test Results

Cell-geometry sweep (2G tmpfs, read-back, all CRC ok): write 276 -> 818
MB/s as cell size grows 16K -> 1M, plateau beyond; read flat ~1950-2000
MB/s; compression ~2:1 throughout.  Write-optimal ~512K-1M (order 7-8).

## Next Steps

1. **The churn/sparse bench** to complete the tradeoff: overwrite+free
   churn at several geometries, measuring retained memory and GC drain
   cost.  This is the half the fill workload cannot see.
2. Re-run the rationalization/RAM-stress benches with the derived default
   geometry (order 8) instead of the hardcoded cell=64K, to see if the
   cell tier's numbers improve.
3. RFC assembly (spec 02 Phase 6).
4. Standing items unchanged: M-boot median+IQR wrapper, the arm-3
   rm/synchronous-free story at 64M.

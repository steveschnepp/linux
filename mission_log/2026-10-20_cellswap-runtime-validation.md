---
name: cellswap-runtime-validation
description: first runtime test of whole-cell disk swap in QEMU (test.mode=cellswap) -- found and fixed 3 real kernel bugs (in_ram on fresh cells, read-path swap-in, __udivdi3), validated device+ext2+ext4 backings PASS, vfat corrupts
project: linux
---

# Mission Log: Runtime Validation of Whole-Cell Disk Swap

Part 62 of the tmpfs transparent-compression series.  First real boot +
run of the tier built in Sessions 77-78 (specs/07).  The compile gate
(i386/x86_64 objects + full mm/) had passed, but booting and actually
paging cells to a disk backing found three real bugs and validated the
feature end to end.

## What We Did

Built `test.mode=cellswap` in the arch guest (`tests/arch_init.c`): mount
tmpfs with `swap=<backing>`, write **seq** data (`1\n2\n3\n...` -- trivially
validatable, highly compressible), force a swap-out pass, then assert
(a) the `swap_out` counter advanced, (b) the data reads back
checksum-correct (swap-in returns it), (c) `swap_in` advanced.  Wired a
QEMU `-drive` (raw sparse image, `if=virtio,cache=none`) into
`run_arch.sh`.  Test knobs: `test.swap`, `test.swapsize`, `test.swpmnt`
(ext4 device to mount for the file case), `test.fstype`, `test.cell`,
`test.mib`.

The run: 128-256M RAM, 1G tmpfs, 2G raw-device backing, 256M seq.  Three
kernel bugs surfaced, each fixed and re-validated:

### Bug 1: `cell_make_active` never set `in_ram` (swap-out was a no-op)

`swap_out` stayed 0 while cells were full.  The 2-bit refactor set
`in_ram=true` only in `cell_swap_in`, so every freshly-created cell read
as not-resident -- and `cell_pool_swap_out`'s `!c->in_ram` filter skipped
them all.  Fixed: a fresh cell is `in_ram=true, on_disk=false`.

### Bug 2: the read path is `shmem_comp_loc_load/loc_read`, not `cell_load`

I had put the swap-in prologue in `cell_load()`.  But the tmpfs read
path is `shmem_comp_loc_read`/`shmem_comp_loc_load`, which access cells
directly (`down_read` + memcpy) and returned `-EIO` for an on-disk cell
(`pages[0]`/`base` NULL) after a retry budget -- so reading a paged-out
cell gave `read err errno=5`.  Fixed: both now call an exported
`cell_ensure_resident()` to swap the cell in, then retry.

### Bug 3: runtime u64 division needs `__udivdi3` (absent on 32-bit)

`cell_pool_enable_backing` did `size_bytes / cell_size` -- a runtime u64
division the i386 kernel can't link (`__udivdi3` undefined).  The
object-level compile gate missed it; the **full kernel link** caught it.
`cell_size` is a power of two (`PAGE_SIZE << order`), so shift by
`PAGE_SHIFT + cell_order` instead.

### Also fixed along the way

- **Device size via `f_mapping->host`**: `i_size_read(file_inode())` on a
  block device reads the devtmpfs device-node inode (size 0), not the
  bdev -- so `swap=/dev/vdX` failed `-EINVAL`.  Read the bdev inode size.
- **`fallocate` fallback**: ext2/vfat lack `fallocate` (`-EOPNOTSUPP`);
  a file backing now falls back to the file's current size (caller
  pre-sizes via `ftruncate`).
- **Deterministic swap-out trigger**: `gc_now=1` now also
  `mod_delayed_work`s the aging work (and a new
  `shmem_comp_swap_out_all()` snapshots pool cell_pool pointers under the
  pools spinlock then swaps out outside it), so tests don't depend on the
  aging workqueue's timing.

## What We Learned

- **The compile gate is necessary but not sufficient.**  The full kernel
  link caught `__udivdi3`; only a boot caught the `in_ram` and read-path
  bugs.  Runtime validation on a real device is mandatory for this
  feature.
- **The read path was not where I assumed.**  I put the swap-in in
  `cell_load()` (the block-device-style loader) but the tmpfs read uses
  `shmem_comp_loc_read`/`loc_load`.  Worth remembering: the engine has
  two cell-access paths, and the swap-in must cover the one tmpfs
  actually uses.
- **`in_ram` is load-bearing for selection** -- a 2-bit refactor silently
  disabled swap-out because the fresh-cell path forgot to set it.  State
  flags set in only one of several cell-lifecycle paths are a trap.
- **Backing filesystem matters.**  Device (raw), ext2, ext4 all PASS.
  **vfat corrupts the read-back** (0 bytes after the swap cycle) -- its
  O_DIRECT/file behavior under the swap load is unreliable; vfat is not a
  suitable backing fs.  f2fs untestable (no `mkfs.f2fs` on the host).

## What We Decided

- **Backing fs**: device (raw), ext2, ext4, f2fs all validated PASS.
  **vfat not suitable** -- root-caused to `fat_direct_IO()` (fs/fat/inode.c):
  an O_DIRECT *write* beyond the file's allocated size (`mmu_private`)
  returns 0, silently falling back to a *buffered* write.  Our
  ftruncate'd backing has `i_size` set but `mmu_private` not grown (FAT
  allocates clusters lazily), so swap-out writes went buffered while
  swap-in O_DIRECT reads hit disk -> incoherent -> corruption.  A
  fundamental FAT limitation for O_DIRECT writes to an unallocated file,
  not a cell-swap bug.
- **`gc_now` is the deterministic swap-out trigger** for tests (sync
  `swap_out_all`); the aging workqueue remains the production trigger.

## Test Results

i386-pae, 256M seq, 1G tmpfs, 2G backing, RAM 256M:

| backing | swap_out | data | swap_in | result |
|---------|----------|------|---------|--------|
| raw device (/dev/vda) | 2011 cells | 269480502/269480502 checksum ok | 2010 cells | **PASS** |
| ext2 file | PASS | checksum ok | PASS | **PASS** |
| ext4 file | PASS | checksum ok | PASS | **PASS** |
| vfat file | PASS | **0 bytes (corrupt)** | PASS | **FAIL** |
| f2fs file | PASS | checksum ok | PASS | **PASS** |

## Files Changed

| File | Purpose |
|------|---------|
| `mm/cell_store.c` | in_ram fix, cell_ensure_resident, f_mapping->host size, fallocate fallback, __udivdi3 shift |
| `mm/shmem_compress.c` | loc_load/loc_read swap-in, swap_out_all, gc_now trigger |
| `tests/arch_init.c` | test.mode=cellswap, ftruncate backing, test.fstype |
| `scripts/run_arch.sh` | -drive wiring, test.swap/swapsize/swpmnt/cell/fstype/mib |

## What We'd Do Differently

- **Run the full kernel link, not just the object gate, from the start.**
  It catches link-only failures (`__udivdi3`) the per-object gate
  cannot; the boot then catches the rest.  The object gate alone gave
  false confidence.
- **Trace the real code path before instrumenting.**  I added the
  swap-in to `cell_load()` on the assumption it was the read path; a
  quick trace to `shmem_comp_loc_read`/`loc_load` would have saved a
  debug round.  The engine has two cell-access paths -- know which one
  the caller uses.
- **Write the boot test alongside the feature, not after.**  The three
  bugs (in_ram, read path, __udivdi3) all would have surfaced during
  development had `test.mode=cellswap` existed earlier, instead of after
  the tier looked "done" at compile level.

## Next Steps

1. **vfat**: documented as unsupported for the backing (fat_direct_IO
   degrades O_DIRECT writes to buffered beyond mmu_private).
2. ~~x86_64 run~~ **done** (Session 79): device + ext4-file PASS on
   amd64.
3. Standing ztmpfs backlog: H618 re-measure, RFC assembly, workload bed
   (spec 05).

---

## Session 79: amd64 (x86_64) validation (2026-10-21)

Built the x86_64 arch guest + kernel (all fs enabled) and ran the same
`cellswap` test under KVM:

| backing (x86_64) | result |
|---|---|
| raw device (/dev/vda) | **PASS** (swap_out + checksum + swap_in) |
| ext2 file (/mnt/d/back) | **PASS** |
| ext4 file (/mnt/d/back) | **PASS** |
| f2fs file (/mnt/d/back) | **PASS** |

All four (device + ext2 + ext4 + f2fs) pass on amd64, matching
i386-pae -- the cell/swap code is architecture-neutral (only throughput
differs).  vfat was not re-run on amd64 (its failure is fs-specific --
`fat_direct_IO`'s O_DIRECT->buffered degradation, unrelated to arch).

Runner note: the sub must be `x86_64` (underscore) to match
`run_arch.sh`'s case and place the kernel at `build/arch/x86_64/kernel`
-- the `x86-64` sub builds a kernel the runner does not recognize.

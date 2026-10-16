---
name: i386-pae-highmem-gate
description: adding an i386-PAE runtime gate -- a second highmem implementation + 64-bit phys_addr_t on a 32-bit kernel; found the tree's HIGHMEM64G removal and a stale capflags.c ref that blocks ALL x86 builds
project: linux
---

# Mission Log: i386-PAE Highmem Runtime Gate

Part 48 of the tmpfs transparent-compression series.  Session
(2026-10-16), picking up after the cross-arch boot + perf sweep
(previous log, sessions 59-64).  New axis: **i386 with PAE + highmem**,
suggested by the user.  This is a runtime gate -- booting a 32-bit x86
kernel with highmem enabled -- not just a compile gate.

## The idea, and why it closes a real gap

The two HIGHMEM bugs from session 63 (cell_store `page_address()` NULL;
crypto `sg_init_one` on `kmap_local` addresses) were found and fixed on
**arm32 only**.  A grep of the existing i386 compile gate's `.config`
showed why that left a hole:

```
# CONFIG_HIGHMEM4G is not set
# CONFIG_X86_PAE is not set
```

The i386 gate -- the very config that caught the 32-bit width bug -- is
**NOHIGHMEM**.  So highmem has only ever been exercised on ARM's
implementation.  x86 32-bit highmem is an untested axis, and it is a
mainstream config (the standard distro 32-bit server kernel for >4GB
RAM).  i386+PAE also turns on `PHYS_ADDR_T_64BIT` while `unsigned long`
stays 32-bit -- a mixed-width config that independently exercises the
u64-handle work from sessions 55-56 on a second arch.

## What We Did

### 1. Verified the Kconfig symbols before writing anything (avoided a silent drop)

The plan was to enable `HIGHMEM64G` (the classic 3-way highmem choice).
Grepping `arch/x86/Kconfig` first showed this tree does **not** have
that structure:

- `HIGHMEM64G` **was removed** -- `arch/x86/mm/init_32.c:568` literally
  prints "Support for CONFIG_HIGHMEM64G was removed!".  Enabling it
  would have been a silent no-op.
- The real knobs are two **independent bools**: `HIGHMEM4G` ("High
  Memory Support", the highmem toggle) and `X86_PAE` ("PAE (Physical
  Address Extension) Support"), the latter `select`s
  `PHYS_ADDR_T_64BIT`.
- `HIGHMEM` is `def_bool HIGHMEM4G` (so enabling HIGHMEM4G sets it).
- `X86_HAVE_PAE` is `def_bool y` gated on a CPU-model list; the i386
  defconfig already picked `M686`, so `X86_HAVE_PAE=y` was already set.

Correct config: `--enable CONFIG_HIGHMEM4G --enable CONFIG_X86_PAE`.
This is the session-45 "grep the Kconfig, don't trust the symbol name"
lesson paid off again -- a wrong choice-member name here fails silently.

### 2. Found and fixed a tree bug that blocks ALL x86 builds

Before committing to a full bzImage, checked whether the session-59
x86_64 build blocker (a stale `X86_FEATURE_UP` reference) also hits the
32-bit build.  It does -- `capflags.c` -> `mkcpustr` runs for **every**
x86 build, 32- and 64-bit alike:

- `arch/x86/kernel/cpu/capflags.c:54` still had
  `[X86_FEATURE_UP] = "up"`.
- `arch/x86/include/asm/cpufeatures.h:81` has the feature retired:
  `/* free: was #define X86_FEATURE_UP ... */`.

The i386 compile gate never hit this because it only built 4 `mm/`
objects -- it never ran the CPU-tooling build.  Fixed by deleting the
one dead `capflags.c` line (the only dangling ref; the other freed bit,
`PVUNLOCK`, was already clean).  This unblocks x86_64 bzImage too --
the session-59 blocker, root-caused and closed.  **Pre-existing tree
bug, not ours.**

### 3. Bootable i386-PAE kernel + 32-bit guest + runner

- Extended `out/build_arch_kernel.sh` (the bootable-kernel builder) with
  an `i386` case (`CROSS=""`, `IMG=arch/x86/boot/bzImage`) and a `PAE=1`
  env knob.  The config reuses the sweep's bootable bits (INITRD,
  DEVTMPFS, BINFMT_ELF, PROC/SYSFS) and the EXPERT+disable-VT/UNICODE
  workaround for the tree-tip `ucs_width_table.h` bug.
- **Axis gate** (the "verify the test is the test" rule): the build
  hard-fails unless `HIGHMEM=y`, `X86_PAE=y`, `X86_32=y`,
  `PHYS_ADDR_T_64BIT=y` and `64BIT` is NOT set.  All verified on this
  config before the build ran.
- New `~/tmpfs-compress-test/scripts/build_arch_guest.sh`: builds
  `tests/arch_init.c` static per arch; i386 = `gcc -m32 -static -lpthread`
  (32-bit `libc.a` present).  Produced `ELF 32-bit LSB executable,
  Intel i386, statically linked`.
- `pack_initramfs.sh`: added `i386-pae` -> `init-i386`.
- `run_arch.sh`: added an `i386-pae` case (`qemu-system-i386 -machine pc`,
  console=ttyS0) and made the accelerator a variable -- i386 uses **KVM**
  when `/dev/kvm` is usable (near-native), else TCG with `-cpu max`.
  Also fixed a **latent bug**: `common=(...)` used to be built *before*
  the arch case, so the `console=ttyS0` prepend for riscv/mips never
  reached the `-append` (arrays capture at definition).  `common` is now
  built *after* the case.

### 4. A SECOND tree-tip x86 build blocker: the EFI PE boot header

After the capflags fix the build got much further, then died assembling
the boot header:

```
arch/x86/boot/header.S:86: Error: invalid operands (*UND* and *UND*) for `+'
```

Line 86 is `.long setup_size + ZO_efi_pe_entry` inside the
`CONFIG_EFI_STUB` PE/COFF `optional_header` block.  Both symbols are
defined (zoffset.h carries `ZO_efi_pe_entry`; setup_size resolves
elsewhere) yet the sum would not relocate at tree tip.  Rather than
rabbit-hole on an x86 boot-header issue orthogonal to our feature,
**disabled EFI** (`--disable CONFIG_EFI_STUB --disable CONFIG_EFI`) for
the sweep kernel: QEMU `-kernel bzImage` boots via the legacy setup
header and never uses EFI, and every `setup_size`/`ZO_efi_pe_entry`
reference sits inside the EFI_STUB guard (header.S:44-224), so dropping
EFI skips the whole block.  Build then succeeded.  Pre-existing tip
issue, logged not chased.

### Build status (interim)

Kernel building (`-j12`), already past `mkcpustr` (the capflags fix
held -- it is compiling sched/fs/fpu).  Guest + initramfs ready.  Boot
runs (see below) pending.

## What We Learned

- **The compile gate's own config can hide an axis.**  The i386 gate
  found the width bug but ran NOHIGHMEM, so it could never have caught
  the highmem bugs even at compile time.  A gate that tests one property
  (width) does not implicitly cover a neighbouring one (highmem).
- **`HIGHMEM64G` is gone; highmem on x86-32 is now `HIGHMEM4G` +
  `X86_PAE` as independent bools.**  Verify against the tree's Kconfig,
  not memory of older kernels.
- **`capflags.c`/`mkcpustr` is on the critical path of EVERY x86 build.**
  A single retired-but-referenced feature bit breaks all of x86, 32 and
  64 alike.  The session-59 "x86_64 cannot build at tip" mystery and
  this i386 blocker are the same one-line bug.
- **A bash array captures values at definition**, so a case statement
  that mutates a variable AFTER the array is built has no effect -- this
  had been silently dropping `console=ttyS0` from the riscv/mips
  cmdlines.

## What We Decided

- **i386-PAE is a runtime gate, not just compile** -- the point is booting
  a 32-bit kernel with live highmem, which the compile gate could never
  do.  Kept in the arch matrix.
- **Disable EFI on the sweep kernel** -- QEMU `-kernel` never uses EFI, and
  the tree-tip EFI PE-header assembly bug is orthogonal to our feature.
- **The capflags.c one-line fix stays** (dead-ref removal) even though the
  file is untracked in this tree -- it is required for ANY x86 build.

## What We'd Do Differently

- Would have attempted a full x86 bzImage build (not just 4 `mm/` objects)
  on day one of the i386 gate -- it would have surfaced BOTH x86 build
  blockers (capflags, header.S EFI) a session earlier, and they blocked the
  entire x86 runtime axis, not just our feature.
- Would have grepped the existing i386 `.config` for HIGHMEM the moment the
  highmem bugs were found on arm32 -- the "second highmem sample" gap was
  one grep away.

## Files Changed

| File | Purpose |
|------|---------|
| `arch/x86/kernel/cpu/capflags.c` | removed stale `[X86_FEATURE_UP]` (unblocks all x86 builds); untracked in this tree |
| `out/build_arch_kernel.sh` | +i386 case, +PAE=1 knob, +axis gates, +EFI disable (untracked out/) |
| `~/tmpfs-compress-test/scripts/build_arch_guest.sh` (new) | build arch_init.c static per arch incl. i386 (`gcc -m32`) |
| `~/tmpfs-compress-test/scripts/{pack_initramfs,run_arch}.sh` | +i386-pae cases; runner ACCEL var (KVM), console= fix |
| `mission_log/2026-10-16_i386-pae-highmem-gate.md` | this log |

## Test Results / Benchmarks

**i386-PAE highmem gate: PASS.**  Correctness battery (zs/cell/merge/
plain, 8 MiB each, streaming-FNV readback + truncate/rewrite verify)
under KVM, one clean A/B on RAM size -- the session-63 memory-sizing
lever -- same kernel both runs:

| run | RAM | highmem evidence (boot log) | result |
|---|---|---|---|
| highmem | -m 1536 | `649MB HIGHMEM available`; HighMem zone `[mem 0x375fe000-0x5ffdffff]`; `665480K highmem` | **36/36 CHK-PASS, GUEST_RESULT: PASS** |
| negative control | -m 512 | `0MB HIGHMEM available`; `HighMem empty`; `0K highmem` | **36/36 CHK-PASS, GUEST_RESULT: PASS** |

The -m 1536 run put ~649MB of real highmem behind the feature; the fixed
decompress path (`kmap_local` + lowmem bounce) and the lowmem-only cell
pages handled it with no crash and no corruption.  This confirms the two
HIGHMEM fixes from arm32 (session 63) **generalize to x86's highmem
implementation** -- a second, architecturally independent highmem sample
(2-level/sections on ARM vs 3-level/64-bit-PTE on x86 PAE).  It also runs
the u64-handle work on a mixed-width config (32-bit `unsigned long`,
64-bit `phys_addr_t` via `PHYS_ADDR_T_64BIT`).  KVM made boot + battery
sub-second; i386 is the fastest arch in the matrix to iterate on.

## Next Steps

1. **Standing backlog unchanged** -- H618 re-measure (the real perf
   question), group-decompress/truncate interleaving race, RFC assembly,
   plateau/CPU-freq/workload-bed items.  The arch matrix is now stronger:
   width (i386, i386-PAE, arm32, mips32) + highmem (i386-PAE, arm32) +
   64-bit (x86_64 compile, arm64 4K/16K/64K, riscv64, mips64) + endian
   (LE + mips BE at compile level).
2. Decide whether to commit the reusable build/gate scripts now living
   in the untracked `out/` dir (build_arch_kernel.sh with the i386/PAE
   additions, build_arch_guest.sh) -- they are deliverables but `out/` is
   not gitignored and not committed.
3. Two pre-existing tip-tree bugs are fixed-but-uncommitted in our tree
   and worth flagging upstream separately: the `capflags.c`
   `X86_FEATURE_UP` dangling ref (blocks ALL x86 builds) and the
   `header.S` EFI PE-header assembly error.  Neither is ours.

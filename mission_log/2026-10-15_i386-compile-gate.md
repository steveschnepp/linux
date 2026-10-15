---
name: i386-compile-gate
description: first arch-matrix gate -- i386 catches a real 32-bit break in the two-table packed entry (shmem_comp_pack assumes a 64-bit unsigned long), plus the unselectable-ZSMALLOC and stale-artifact gotchas
project: linux
---

# Mission Log: The i386 Compile Gate (First Arch-Matrix Run)

Part 45 of the tmpfs transparent-compression series.  Session
(2026-10-15).  The target is an H618 SBC, but upstream requires the
feature to be correct on every arch, so the priority inverts: an
arch-correctness matrix gates everything, and perf on any one board comes
after.  This is the first gate -- **i386**, chosen because it is the
cheapest and it carries the 32-bit width risk that amd64/arm64 cannot
expose.  It found a real bug on the first try.

## What We Did

### Setup: out-of-tree i386 build, no new toolchain

No `i686-linux-gnu-gcc` is installed and we may not install one, but none
is needed: the kernel builds i386 via plain `gcc -m32`, and the kernel is
freestanding so no 32-bit libc is required.  Verified `gcc -m32 -c` works.
`ARCH=i386` maps to `SRCARCH=x86` (Makefile:413).

Everything builds **out-of-tree** (`O=out/kbuild-i386`) so the master
tree's `.config` is never touched.  This matters because the boundary log
already recorded one accidental `.config` regeneration via `make
mrproper`.

### Blocker 1: ZSMALLOC cannot be enabled by hand (real gotcha)

`TMPFS_COMPRESSION` depends on `TMPFS && CRYPTO && ZSMALLOC`
(fs/Kconfig:214).  TMPFS and CRYPTO were already `=y` in the i386
defconfig, but **`CONFIG_TMPFS_COMPRESSION` never appeared in the
generated `.config` at all** (grep count 0, not even "is not set").

Cause: `config ZSMALLOC` (mm/Kconfig:125) is a **tristate with no prompt
and no dependencies**.  A no-prompt symbol is driven purely by `select`
statements, so `scripts/config --enable CONFIG_ZSMALLOC` writes it into
`.config` and `olddefconfig` then silently drops it.  The only two
`select ZSMALLOC` sites are ZRAM (drivers/block/zram/Kconfig:5) and ZSWAP
(mm/Kconfig:26).

Fix: enable ZRAM (`--enable CONFIG_ZRAM --enable CONFIG_ZRAM_BACKEND_LZ4
--enable CONFIG_ZRAM_BACKEND_ZSTD --disable
CONFIG_ZRAM_DEF_COMP_LZORLE`), which drags ZSMALLOC in.  After that
`CONFIG_TMPFS_COMPRESSION=y` landed correctly on i386 with **no
`CONFIG_64BIT`** -- so the Kconfig symbol itself is genuinely 32-bit
selectable, and the failure further down is a code bug, not a config one.

### Blocker 2: a STALE-ARTIFACT false positive (not an i386 bug)

`make prepare` first died in a HOST tool:

```
scripts/mod/file2alias.c:1508: error: 'SIZE_mcb_device_id' undeclared
  did you mean 'SIZE_mhi_device_id'?
```

Chased it far enough to prove it is NOT real.  The source tree is a
**fully-built x86_64 tree** (`git clean -nXd` dry run lists 3483 ignored
artifacts: System.map, .vmlinux.o, .config.old, ...).  It still held a
stale `scripts/mod/devicetable-offsets.h` from that x86_64 build with
`mcb=0`, and the host build resolved the SOURCE-tree header ahead of the
freshly generated `out/kbuild-i386/scripts/mod/devicetable-offsets.h`
which had `mcb=1`.  Both `DEVID(mcb_device_id)` (devicetable-offsets.c:202)
and `{"mcb", SIZE_mcb_device_id, ...}` (file2alias.c:1508) are
unguarded, so any tree where the generated header lacks the macro breaks
modpost.  Moving the one stale header aside made `prepare` exit 0.

**Did NOT run `git clean`.**  It is destructive to the working built tree
and forbidden without explicit approval.  Instead the artifacts were
backed up to `out/srcartifacts-backup*.tar.gz` and moved aside:
`.config`, `include/config`, `include/generated`,
`arch/x86/include/generated` (the `outputmakefile` check at Makefile:705
refuses `O=` while any of those four exist), plus the stale
`scripts/mod/devicetable-offsets.h`.

### The run: 3 of 4 objects clean, shmem_compress breaks

| object | i386 result |
|---|---|
| `mm/cell_store.o` | OK, 0 warnings |
| `mm/shmem.o` | OK, 0 warnings |
| `mm/filemap.o` | OK, 0 warnings |
| `mm/shmem_compress.o` | **FAIL: 2 errors** |

```
mm/shmem_compress.c:3592:29: error: left shift count >= width of type
  [-Werror=shift-count-overflow]
mm/shmem_compress.c:3601:33: error: right shift count >= width of type
  [-Werror=shift-count-overflow]
```

### The bug: the two-table packed entry is 64-bit-only by design

```c
static inline void *shmem_comp_pack(unsigned int id, unsigned int gen)
{
	return xa_mk_value(((unsigned long)(gen & SHMEM_COMP_ENTRY_GEN_MASK)
			    << 32) | id);
}

static inline void shmem_comp_unpack(void *entry, unsigned int *id,
				     unsigned int *gen)
{
	unsigned long v = xa_to_value(entry);
	*id  = (unsigned int)v;
	*gen = (unsigned int)(v >> 32);
}
```

This is spec 02's two-table mapping: the compressed mapping stores a
packed `{gen, id}` xarray value, `id` indexing a separate entry xarray
(`xa_alloc(&shmem_comp_entries, &id, ent, ...)`, :1321) and `gen`
rejecting a stale id once an entry is freed and the id recycled.

Why it is a design problem, not a warning to silence:

- `SHMEM_COMP_ENTRY_GEN_BITS` is **23** (include/linux/swapops.h:148), so
  the packed value spans gen(23) + id(32) = **55 bits**.
- `xa_mk_value(v)` is `(void *)((v << 1) | 1)` with
  `WARN_ON((long)v < 0)`, so a value entry holds
  `sizeof(unsigned long) * 8 - 1` usable bits: **63 on 64-bit, 31 on
  32-bit**.
- On i386 `unsigned long` is 32 bits, so `<< 32` / `>> 32` are UB (the
  compile error), and even with the shifts fixed there is nowhere to put
  55 bits of payload in a 31-bit field.
- The `id` is an `xa_alloc`-assigned ID, so it is not a small bounded
  counter -- there is no existing MAX_ENTRIES cap to lean on.

So the packed-entry scheme **only works on 64-bit**.  This is exactly the
class of break an arch matrix exists to catch, and upstream would reject a
feature that does not build on 32-bit.

## What We Learned

### Technical

- **`config ZSMALLOC` is un-selectable-by-hand** (no prompt, no deps).  Any
  config that needs it must go through ZRAM or ZSWAP.  A missing dependency
  like this fails *silently* -- `TMPFS_COMPRESSION` simply did not appear
  in `.config`, with no error anywhere.  Always grep the generated
  `.config` for the symbol rather than trusting `scripts/config`'s exit
  status.
- **An out-of-tree `O=` build refuses while the source tree holds
  `.config`, `include/config`, or `arch/$(SRCARCH)/include/generated`**
  (Makefile:705).  Top-level `include/generated` alone is not in the
  check; `arch/x86/include/generated` is.
- **A fully-built source tree silently poisons out-of-tree builds.**  The
  stale `scripts/mod/devicetable-offsets.h` shadowed the fresh one because
  the host tool's include path prefers the source tree.  Symptom was a
  confusing modpost error naming an unrelated device.  When an out-of-tree
  build fails in a HOST tool, suspect a stale source-tree artifact before
  suspecting the architecture.
- **`xa_mk_value` gives you `sizeof(long)*8 - 1` bits, not
  `sizeof(long)*8`** -- the `<<1|1` tag plus the sign-bit WARN_ON cost two
  bits.  Packing anything wider than that is a latent arch bug.  On 32-bit
  that budget is a very tight 31 bits.
- **i386 `gcc -m32` needs no cross-toolchain and no libc** for a kernel
  build; a compile gate costs nothing but a config and a `make`.

### Process

- **The compile gate paid for itself on run 1.**  It cost one config and a
  few minutes and immediately produced a real upstream-blocking bug that
  the entire x86_64/KVM series was structurally incapable of finding.
- **Two of the three obstacles were environmental, not the feature's
  fault** (unselectable ZSMALLOC; stale source-tree artifacts).  Keeping
  those separate from the real bug required deliberately proving each one
  was not an i386 problem before moving on -- the modpost error in
  particular looked like a 32-bit break and was not.
- **A dry-run `git clean -nXd` is a safe way to inventory build
  pollution** without touching anything.  Useful triage; the actual
  cleanup stayed a user decision.

## What We Decided

- **Priority inverted to arch-correctness first, then perf.**  H618 is the
  motivation; upstream is the gate.  Correctness sweeps are emulatable on
  the workstation under QEMU TCG (any arch, any page size, BE/LE, 32/64
  bit); perf only runs on real silicon (H618, the RV64 board, amd64 and
  i386 on the workstation).
- **i386 is kept in the matrix** -- it is the only cheap way to exercise
  the 32-bit width class, and it just proved its worth.
- **The stale-artifact cleanup stays a user decision.**  Nothing was
  destroyed; backups are in `out/srcartifacts-backup*.tar.gz`.  Running
  `git clean` on a fully-built tree is destructive and was not done.

## Rules Added

- **Never trust `scripts/config --enable` -- grep the generated
  `.config`.**  No-prompt symbols (ZSMALLOC) are silently dropped by
  `olddefconfig`, and the dependent symbol vanishes without an error.
- **When an out-of-tree build fails in a HOST tool, check for a stale
  source-tree artifact first.**  A fully-built tree shadows fresh
  generated headers via the include path and produces misleading errors
  that look like architecture bugs.
- **Check the packing budget before widening a value.**  `xa_mk_value`
  holds `sizeof(long)*8 - 1` bits; anything wider is a 32-bit bug waiting
  to be found by the first arch gate.

## What We'd Do Differently

- Would have grepped the generated `.config` for `TMPFS_COMPRESSION`
  right after `olddefconfig` instead of only checking exit codes; the
  silent drop cost a confusing detour.
- Would have run `git clean -nXd` as the FIRST step of any out-of-tree
  build on a previously-built tree -- it immediately explains most
  stale-artifact hazards.
- Would have started the arch matrix with the compile gate on day one of
  the series rather than after a full x86_64 benchmark campaign; the bug
  was always there and was never going to be found on x86_64.

## Files Changed

| File | Purpose |
|------|---------|
| `out/build_i386.sh` (new) | reproducible i386 compile gate: defconfig -> enable deps -> olddefconfig -> prepare -> build the 4 arch-sensitive objects |
| `out/kbuild-i386/` (new) | out-of-tree i386 build dir (O=) |
| `out/srcartifacts-backup*.tar.gz` (new) | backups of every source-tree artifact moved aside |
| `out/srcartifacts/` (new) | the moved-aside artifacts themselves (restorable) |
| none in `mm/` or `include/` | the bug is found, NOT yet fixed |

## Test Results

- `CONFIG_TMPFS_COMPRESSION=y` achieved on i386 with no `CONFIG_64BIT`:
  the Kconfig is 32-bit-selectable.
- `mm/cell_store.o`, `mm/shmem.o`, `mm/filemap.o`: i386 clean, 0
  warnings each.
- `mm/shmem_compress.o`: 2 errors, both shift-count-overflow at :3592 and
  :3601, both in the two-table packed entry.

## Next Steps

1. **Fix the 32-bit packing** in `shmem_comp_pack`/`shmem_comp_unpack`.
   Three candidate directions, needing a decision:
   - (a) Shrink gen+id to fit 31 bits -- needs a NEW hard cap on
     concurrent compressed entries (id is `xa_alloc`-assigned today, no
     cap exists) and shrinks gen so it wraps faster, weakening the
     stale-id ABA defence.  A correctness cost, not a free fix.
   - (b) Store a pointer via `xa_mk_pointer` to the already-kmalloc'd
     `ent` (:1295) instead of packing -- removes the width problem
     entirely but changes the id-recycle/gen staleness scheme, which is
     the thing spec 02 was built around.
   - (c) Declare the feature `depends on 64BIT` for now, ship 64-bit
     upstream first, and record 32-bit as explicitly unsupported.  Fast,
     but reviewers will ask, and it does not close the gate.
2. Re-run the gate after the fix; `mm/shmem_compress.o` must reach 0
   errors on i386.
3. Extend the gate to the other cheap arches: arm32 (32-bit + ARM
   ordering), arm64 with **16K and 64K pages** (the `PAGE_SIZE <<
   cell_order` coupling and the `use_thp` threshold), and a big-endian
   build (s390x or powerpc64-be) to confirm the cell format really is
   endian-safe.
4. Restore or permanently remove the moved-aside source-tree artifacts --
   user decision.

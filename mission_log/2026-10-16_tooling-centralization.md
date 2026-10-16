---
name: tooling-centralization
description: moving the reusable build/gate scripts into tmpfs-compress-test, the in-dir .gitignore rule for local build dirs, and the useful-vs-throwaway script triage
previous: 2026-10-16_i386-pae-highmem-gate.md
project: linux
---

# Mission Log: Tooling Centralization and the `out/` Ignore Rule

Part 49 of the tmpfs transparent-compression series.  Session
(2026-10-16), continuing the same day as the i386-PAE highmem gate
(previous log).  Small, decision-heavy wrap-up: the build/gate scripts
had been accumulating in the linux tree's untracked `out/` dir, and the
user wanted them centralized and `out/` kept out of the repo entirely.

## What We Did

### 1. Triaged `out/` into reusable scripts vs. scratch drivers

`out/` held 8 `.sh` files.  Read each to judge which were worth keeping:

| script | verdict |
|---|---|
| `build_arch_kernel.sh` | **keep** -- the master bootable-kernel builder (any arch/sub, config flags, axis gates, now +i386/PAE) |
| `build_arch_gate.sh` | **keep** -- compile-only gate for the 4 arch-sensitive objects |
| `build_i386.sh` | **keep** -- original fast i386 compile gate |
| `build_x86_kernel.sh` | **keep** -- x86-64 bootable builder (now unblocked by the capflags fix) |
| `sweep_build_all.sh` | **keep** -- sequential whole-sweep build driver |
| `rebuild_all_kernels.sh` | **drop** -- hardcoded subset, raw `make`, no config gates; superseded by `build_arch_kernel.sh` + `sweep_build_all.sh` |
| `rebuild_rest.sh` | **drop** -- ad-hoc "rebuild the rest" subset driver |
| `rebuild_rest2.sh` | **drop** -- another ad-hoc subset; near-duplicate of the above |

The three `rebuild_*.sh` are session scratch: overlapping hardcoded
arch lists, no config-script setup, no axis verification.  They stay in
the (now-ignored) `out/` as loose files, not committed.

### 2. Moved the 5 keepers to `~/tmpfs-compress-test/scripts/`

All 5 use absolute paths (`SRC=/home/snide/linux`, `OUT=$SRC/out/...`,
`DEST=/home/snide/tmpfs-compress-test/build/arch/...`) and `cd "$SRC"`,
so relocating the files changes nothing about how they run -- they still
build *from* the kernel tree and land images in the harness repo.  Only
one inter-script reference needed fixing: `sweep_build_all.sh` called
`S=out/build_arch_kernel.sh` (relative), now the absolute path in the
new home.

Also committed the cross-arch guest/runner/packer
(`tests/arch_init.c`, `run_arch.sh`, `pack_initramfs.sh`) -- they were
untracked in the harness repo, and the moved scripts depend on them, so
leaving them loose would have made the toolchain incoherent (a committed
`build_arch_guest.sh` compiling a missing `arch_init.c`).  Left
`poc/hashbench_init.c` untracked -- unrelated to the sweep.

### 3. The `out/` ignore -- user course-corrected my approach

First pass: added `/out/` to the linux tree's root `.gitignore`.  The
user rejected this -- root `.gitignore` is a tracked, shared file and
project-local scratch dirs do not belong in it.  Correct approach, per
the user: **put a `.gitignore` inside `out/` itself**, leaving the root
`.gitignore` untouched.

`out/.gitignore` is a single `*` (plus a comment).  Verified with
`git check-ignore -v`: it ignores `out/`, `out/sweep-kernels`, **and
`out/.gitignore` itself** (the `*` matches the dotfile), so the whole
directory is invisible to git and nothing about it is ever tracked.
Root `.gitignore` reverted to a zero diff; the linux tree's
`git status` is empty.

## What We Learned

### Technical

- **A `.gitignore` placed inside a directory can fully hide that
  directory, including the `.gitignore` file itself** -- a bare `*`
  matches dotfiles in gitignore semantics.  This is the right pattern
  for a local build/scratch dir you never want tracked, because it needs
  no change to any shared, tracked ignore file.
- **Absolute paths make shell tooling relocatable.**  Every keeper
  script resolved `SRC`/`OUT`/`DEST` absolutely and `cd`'d to the source
  tree, so moving the files was a pure `mv` with one call-site fix.  A
  script that assumed it lived in `out/` (relative self-reference) would
  have broken; only `sweep_build_all.sh` did, in one spot.
- **A committed script that compiles an uncommitted source is
  incoherent.**  `build_arch_guest.sh` builds `tests/arch_init.c`; both
  had to land together or neither was useful to anyone cloning the repo.

### Process

- **"Useful vs. throwaway" is a judgment worth recording.**  Three of
  the eight scripts were near-duplicate scratch drivers that would have
  rotted in the repo; naming the criterion (hardcoded subset, no config
  gates, superseded by a parameterized builder) makes the same triage
  fast next time.
- **The user's `.gitignore` preference is a project convention, not a
  one-off** -- root `.gitignore` stays clean; local dirs carry their own
  ignore file.  Recorded as a rule below because it will recur every time
  a scratch dir appears.

## What We Decided

- **Centralize all reusable sweep tooling in
  `~/tmpfs-compress-test/scripts/`**, tracked in that repo.  The linux
  tree carries no tracked tooling -- only the kernel source and its
  gitignored `out/` scratch.
- **`out/` is ignored via an in-dir `.gitignore`, never the root one.**
  Rejected: root `.gitignore` entry (pollutes a tracked shared file).
- **The `rebuild_*.sh` scratch drivers stay uncommitted** in `out/`;
  `build_arch_kernel.sh` + `sweep_build_all.sh` replace them.
- Committed the whole coherent cross-arch toolchain to the harness repo
  (commit `0fd0f68`); left `poc/hashbench_init.c` alone.

## Rules Added

- **Local build/scratch dirs get their own `.gitignore`, not a root
  `.gitignore` entry.**  A `*`-only `.gitignore` inside the dir hides the
  dir and the file itself; the tracked root ignore file stays untouched.
- **Kernel/sweep build scripts live in `~/tmpfs-compress-test/scripts/`,
  use absolute `SRC`/`OUT`/`DEST` paths, and `cd` to the source tree** --
  so they are relocatable and always build from the canonical kernel
  tree.
- **When committing a build script, commit the sources it needs** (or
  vice versa) -- a toolchain half-committed is useless to clone.

## What We'd Do Differently

- Would have put the reusable scripts in the harness repo (and `out/`
  under an in-dir `.gitignore`) from the moment they were written, rather
  than accumulating them in the kernel tree's scratch dir and migrating
  after the fact.  The kernel tree should never have held tooling.

## Files Changed

| File | Purpose |
|------|---------|
| `~/linux/.gitignore` | reverted -- no `/out/` entry (zero diff) |
| `~/linux/out/.gitignore` (new) | single `*`; hides `out/` (and itself) from git |
| `~/tmpfs-compress-test/scripts/build_arch_{kernel,gate,guest}.sh` | moved/added; the builders + compile gate |
| `~/tmpfs-compress-test/scripts/build_{i386,x86_kernel}.sh` | moved; x86 builders |
| `~/tmpfs-compress-test/scripts/sweep_build_all.sh` | moved; path ref to build_arch_kernel.sh fixed |
| `~/tmpfs-compress-test/scripts/{run_arch,pack_initramfs}.sh`, `tests/arch_init.c` | committed (were untracked); +i386-pae support |
| `~/linux/out/rebuild_{all_kernels,rest,rest2}.sh` | left as ignored scratch, not committed |

## Next Steps

1. Standing backlog unchanged: H618 re-measure (the real perf question),
   group-decompress/truncate interleaving race, RFC assembly,
   plateau/CPU-freq/workload-bed items.
2. Two pre-existing tip-tree bugs found this session are fixed or
   worked around locally but worth flagging upstream separately: the
   `capflags.c` `X86_FEATURE_UP` dangling ref (blocks ALL x86 builds)
   and the `header.S` EFI PE-header assembly error.

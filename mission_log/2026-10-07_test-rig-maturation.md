---
name: test-rig-maturation
description: the QEMU rig grows up -- Apple fstools as an external oracle, the C PID 1 supervisor and the POSIX_TIMERS gotcha, and the guest/ vs build/ repo split (sessions 11-13)
project: linux
---

# Mission Log: Test-Rig Maturation -- fstools, PID 1, and the Repo Split

Part 6 of the tmpfs transparent-compression series, split verbatim from the
combined log `2026-10-05_tmpfs-transparent-compression.md` (kept until the
split is validated). Session numbers are preserved so cross-references keep
resolving.

Covers sessions 11-13 (2026-10-07 - 2026-10-08): porting Apple's fstools
(fsx + fstorture) into the guest as a third-party oracle, replacing the
shell /init with a static C PID 1 supervisor (and the CONFIG_POSIX_TIMERS
gotcha that made alarm() silently dead), and restructuring the rig repo into
tracked guest/ vs disposable build/.

- Previous: `2026-10-07_compression-policy-option-surface.md`

## Session 11: Apple's fstools in the guest -- fsx + fstorture on compressed tmpfs (2026-10-07)

### What We Did

Brought https://github.com/apple/fstools into the QEMU test guest:
cloned, ported to Linux, built statically, wired into the initramfs,
and gated behind `TEST_FSTOOLS=1`. The repo is Apple's filesystem
**testing** tools -- `fsx` (the classic filesystem exerciser) and
`fstorture` (reference-model directory-tree torture) -- which is
better news than expected: these are exactly the kind of third-party
torture workloads this rig exists for.

**The port** (all patches local to the rig, none upstream):

- `fstools-compat/` -- four Linux shims, force-included or -I'd:
  - `sys/paths.h`: `_PATH_FORKSPECIFIER` -> `""` (Darwin named forks
    do not exist; keeps fsx's fork code inert).
  - `membership.h`: `uuid_t` + `mbr_uid_to_uuid()` stub deriving a
    deterministic pseudo-UUID from the uid (guest has no Open
    Directory; fstorture needs a stable per-user token for its ACL
    path, which the Linux branch disables anyway).
  - `sys/acl.h`: Darwin ACL API (ACL_TYPE_EXTENDED,
    ACL_EXTENDED_ALLOW/DENY, perm/tag constants) as ENOSYS-returning
    stubs -- fstorture's ACL path is NFSv4-ACL semantics with no
    Linux equivalent, and the port branch forces acl=0.
  - `fsx_linux.h`: force-included. Maps Darwin xattr arity to Linux
    (fgetxattr 6->4, fsetxattr 6->5 keeping options-as-flags,
    fremovexattr 3->2), F_NOCACHE -> invalid fcntl cmd (999, no-op),
    and pulls stdint.h for uintptr_t.
- `fstorture.c`: `#ifdef __APPLE__` guard around the getattrlist(2)
  capability probe (and its statfs.f_mntonname use -- Linux statfs
  has no such field). Linux branch: assume symlinks+hardlinks, ACLs
  off, print "(Linux port)" capabilities lines.
- `fsx.c`: default EA name patched to `"user.fsx"` on Linux -- the
  Darwin default is the program name (`/bin/fsx`), namespace-less,
  rejected by Linux with EOPNOTSUPP. This was found by the gate, not
  by reading.
- Build quirks: `-static`, fstorture needs `-include string.h
  -include signal.h -DF_NOCACHE=999 -lm`; fsx needs `-include
  fsx_linux.h`. Reproducible via `scripts/build_fstools.sh`.

**Rig wiring:** binaries into `initrd/bin/`; `test.fstools=1` gate in
`initrd/init` (mounts compressed tmpfs -- lz4, merge_pages=1,
compress_threshold=1 -- runs fsx -N 2000, fsx -e -N 500, fstorture 1
process x 25 cycles, dumps comp_stats, exits with the summed status);
`TEST_FSTOOLS=1` env hook in `run_qemu.sh`.

**Gate dead ends (rig-side, all fixed):**

1. fstorture was given two separate tmpfs mounts as its two roots.
   Its core operations rename/hardlink BETWEEN the roots, so every
   such op failed EXDEV -- the tool printed errors but exited 0 and
   "completed" 25 cycles testing nothing. Silent-failure trap: a
   filesystem torture tool that cannot operate on the filesystem is
   not a test. Fix: one mount, two subdirectories.
2. The subdirectories were mkdir'd BEFORE the mount, so the mount
   hid them; fstorture exited1 on "root1 does not exist". Fix:
   mkdir after mount (paths inside a new mount must be created
   inside it).
3. fsx -e failed with EOPNOTSUPP (the EA-name port gap above).

**Final results** (kernel #69, panic asserts armed):
fsx 2139 ops "completed A-OK!" exit 0; fsx -e 500 xattr ops A-OK
exit 0 (through the Darwin-arity shims, on a compressed mount);
fstorture 25 cycles clean exit 0; `Test exit code: 0`; comp_stats
13 compressed / 13 uncompressed pages, stale_value_retries 0; both
mount lines resolved correctly (cell-backed, lz4, watermark=0:0).

### What We Learned

- The "third-party torture workload" genre is the strongest possible
  external validation: fsx's pattern-verification model checked our
  compress->store->decompress path under2139 random ops without a
  single mismatch. When our own tests all pass, importing an
  independent oracle is the next-best evidence.
- fstorture's exit code does not reflect its printed errors -- it
  "completed" while every cross-root op was EXDEV. Exit code is not
  a verdict; the operation log is. Same class as the session-10
  lesson, different tool.
- Darwin->Linux ports fail in predictable layers: headers
  (sys/paths.h, membership.h), API shapes (xattr arity,
  getattrlist), semantics (EA namespaces), and build glue (missing
  string.h/signal.h transitive includes). Shimming the headers got
  compilation; only in-guest runs exposed the semantic gaps.
- macOS capability probing has no Linux equivalent worth faking in
  detail: assume what Linux guarantees (symlinks, hardlinks) and
  disable what has no counterpart (NFSv4 ACLs).

### What We Decided

- fstools live in the rig as a gated workload (`TEST_FSTOOLS=1`),
  not part of the default suite -- they are external validation, not
  the standing acceptance gate.
- Port patches stay local and are documented in build_fstools.sh
  (a fresh clone needs them redone; none are worth upstreaming here).
- fsx -e stays in the gate: it is the only coverage of the xattr
  shim layer.

### Rules Added

- **Same-filesystem rule:** any torture tool taking two roots
  (source/destination trees) must run them on ONE filesystem unless
  cross-device behavior is the explicit subject; EXDEV turns the run
  into a no-op that still exits 0.
- **Mount-then-mkdir rule:** paths that live inside a new mount are
  created after the mount, never before it.

### What We'd Do Differently

- Grep a Darwin port for hardcoded namespaces/paths up front;
  fsx's xattr namespace was hardwired to macOS and surfaced only
  when its 500 xattr ops ran.
- Record every shim/patch in the log at the moment it is written;
  the four shims and the fsx default patch lived only in the build
  script until session close.

### Files Changed

| File | Purpose |
|------|---------|
| rig `fstools-src/` | cloned apple/fstools + local Linux patches (fstorture.c capability guard, fsx.c EA name) |
| rig `fstools-compat/` | Linux shims: sys/paths.h, membership.h, sys/acl.h, fsx_linux.h |
| rig `scripts/build_fstools.sh` | reproducible static build of fsx + fstorture into initrd/bin |
| rig `scripts/run_qemu.sh` | TEST_FSTOOLS=1 -> test.fstools=1 |
| rig `initrd/init` | fstools gate: compressed tmpfs mounts, fsx / fsx -e / fstorture, comp_stats dump |
| rig `initrd/bin/fsx`, `fstorture` | static binaries (~1 MB each) |

### Test Results / Benchmarks

`TEST_FSTOOLS=1 QEMU_CPUS=4`, kernel #69: fsx 2139 ops A-OK (exit
0); fsx -e 500 xattr ops A-OK (exit 0); fstorture 25 cycles clean
(exit 0); Test exit code 0; comp_stats 13/13 pages,
stale_value_retries 0; zero oopses/warnings beyond boot noise.

### Next Steps

1. Optional: fold a short fstools run into the default suite rotation
   (e.g., fsx -N 500) so every kernel build gets the external-oracle
   check, not just explicit gate boots.
2. fstorture longer/time-based runs (-t) as a Stage-2 torture
   companion alongside the munin untar farm.
3. Stage 2 machinery remains the main line (marker list,
   shmem_comp_maintain(), default flip to 50:70, policy matrix).

### Addendum: the rig becomes a git repo, fstools stays its own (2026-10-07)

User decision, same day: the rig keeps its path
`/home/snide/tmpfs-compress-test` (rejected the name linux-rig), and
-- standing rule -- **any new project subdirectory must be
git-initialized on creation**.

- Rig repo: initial import `213fb8d` on `main` (89 files, 8,979
  insertions). Before this the rig lived outside version control and
  per-session rig work was recorded only in this mission log; history
  starts at the import. Bisectability convention going forward, same
  as the kernel tree: one logical change = one commit, rig green at
  every commit, derived artifacts ignored so diffs stay reviewable.
  The rig is the acceptance gate the kernel history bisects against;
  gate runs are quoted in the kernel mission log, which pairs the
  two repositories' histories.
- Tracked: scripts/, tests/ (our C suite -- the user's explicit
  boundary: OUR fs tests live in OUR repo), initrd/init, busybox +
  applet symlinks (the one prebuilt binary no script produces),
  .gitkeep placeholders in the guest mount points (proc/sys/mnt/tmp;
  git cannot track empty dirs), munin-2.0.78.tar + munin.crc
  (external inputs), README.md. Ignored: rootfs.cpio.gz, kernel/,
  results/, generated testdata, built test/tool binaries,
  fstools-src/ + fstools-compat/ (superseded).
- fstools boundary (final shape after the day's back-and-forth):
  Apple's fstools stays its OWN repo -- `~/fstools`, branch
  `linux-port`, commit `0ef92df` -- carrying the source guards,
  linux-compat/ shims and LINUX-PORT.md. The rig consumes it via
  scripts/build_fstools.sh (checks out the branch, builds into
  initrd/bin); nothing fstools is vendored into the rig repo.
- Two build_initrd.sh fixes rode along with the import: guest mount
  points are mkdir'd at pack time (the .gitkeep files make git
  remember them; the mkdir keeps non-git checkouts correct), and
  the dataset-tar step now uses absolute paths (it silently required
  cwd = rig root before -- it bit this session once already).
- Verified from a foreign cwd: build_fstools.sh (uses
  ~/fstools @ linux-port 0ef92df), build_initrd.sh (cpio contains
  all four .gitkeep files), and a full guest boot on the
  repo-built initrd: fsx 2148 ops A-OK, fsx -e 500 xattr ops
  A-OK, fstorture clean, Test exit code 0.

---

## Session 12: C PID 1 supervisor + the POSIX_TIMERS gotcha (2026-10-08)

### What We Did

The rig's guest `/init` (a 9KB shell script) was redone as a split:

1. `/init` is now a tiny static C program (`~/tmpfs-compress-test/tests/init.c`,
   ~220 lines). It mounts `/proc`, parses `init.reboot=<seconds>` from the
   kernel cmdline, forks `/init.sh` once, reaps every child attached to it
   via a SIGCHLD handler, and calls `reboot(RB_AUTOBOOT)` as soon as the
   MAIN script exits (stray grandchildren are reaped but never delay the
   reboot) or when the `alarm()` watchdog fires.
2. The old script moved to `initrd/init.sh` and lost its `reboot -f`/`sleep`
   tails: it just prints everything and `exit ${TEST_EXIT}`. The
   `Test exit code: N` contract that `run_qemu.sh` parses is unchanged.
3. `run_qemu.sh` gained `INIT_REBOOT=<n>` (passes `init.reboot=<n>`) and a
   watchdog-kill detection: a reboot before tests finish exits 125 instead
   of masquerading as success.

Design evolution during the session: first cut was a sigtimedwait reap
loop; simplified to the user's suggestion -- install handlers, `pause()`,
reboot from the SIGCHLD handler when the main child's pid is reaped.
Handler code only uses async-signal-safe calls (write/waitpid/sync/reboot);
SIGCHLD is blocked across fork() so no exit slips past the
fork-returns/main_pid-set window (the mask survives execve, so the child
unblocks it before execl).

### What We Learned

- **The killer finding: this tinyconfig kernel has `CONFIG_POSIX_TIMERS`
  off.** tinyconfig runs with EXPERT=y, which exposes POSIX_TIMERS as an
  opt-out; without it the kernel links `kernel/time/posix-stubs.o` and
  `alarm()`/`setitimer()`/`nanosleep()` silently do nothing. Symptoms were
  bizarre until the config was checked: the `init.reboot=N` watchdog armed
  ("watchdog armed: reboot in 3 s") but never fired across three boots, and
  a `sleep 30` init.sh "exited with code 0" within 3 seconds. **Never blame
  your C code for a silent timer twice -- check CONFIG_POSIX_TIMERS first.**
- `kernel/time/Makefile` in this tree builds `hrtimer.o` as `obj-y`
  (HIGH_RES_TIMERS only upgrades precision), so enabling POSIX_TIMERS has
  no HRT dependency and is safe on the tick. Fixed in the rig's
  `scripts/build_kernel.sh` (`--enable CONFIG_POSIX_TIMERS`).
- PID 1 signal rules: the kernel drops default-disposition signals sent to
  the unkillable init, but signals WITH an installed handler are delivered
  normally (the SIGNAL_UNKILLABLE drop in `get_signal()` only applies to
  the default-action path -- see kernel/signal.c around line 3037).
- `/proc/cmdline` ends with `\n`: a parser that requires the token to be
  followed by `\0` or space silently rejects the last (or only) parameter.
  This exact bug made `init.reboot=5` never parse while `init.reboot=3`
  with a trailing token did -- watch for `\n`, `\t` in cmdline parsers.
- Serial interleaving is normal: kernel printks and PID 1's untimestamped
  printf share the console, so lines like "init: launched /  with
  arguments:" are two writers crossing mid-line, not corruption.
- `run_qemu.sh` exit-code contract: guest result = last
  "Test exit code: N" in the serial log; watchdog kill = 125; QEMU
  `-no-reboot` exit = pass-through.

### What We Decided

- The shell /init is replaced by a static C PID 1 supervisor: it
  reaps children, reports per-test results, and enforces the exit
  code the shell pipeline could not.
- Guest exit-code contract: last "Test exit code: N" wins; watchdog
  kill = 125; QEMU -no-reboot exit = pass-through.

### What We'd Do Differently

- Check .config for a silently-dead primitive before debugging
  logic: alarm() was a no-op without CONFIG_POSIX_TIMERS, and its
  failure mode is silence, not an error.

### Files Changed

| File | Purpose |
|------|---------|
| `~/tmpfs-compress-test/tests/init.c` | new PID 1 supervisor (reap + watchdog) |
| `~/tmpfs-compress-test/initrd/init.sh` | old init script, now a supervised child; reboot/sleep tails removed |
| `~/tmpfs-compress-test/initrd/init` | now the compiled static supervisor binary |
| `~/tmpfs-compress-test/scripts/build_kernel.sh` | `--enable CONFIG_POSIX_TIMERS` (watchdog + guest sleep need it) |
| `~/tmpfs-compress-test/scripts/build_initrd.sh` | old sh-heredoc fallback removed; builds/validates C init + init.sh |
| `~/tmpfs-compress-test/scripts/build_tests.sh` | builds tests/init.c -> initrd/init |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | INIT_REBOOT passthrough; watchdog kill -> exit 125 |
| `~/tmpfs-compress-test/README.md` | init/init.sh split documented |

### Test Results

- Normal path: `TEST_MERGE=1` sweep boot -> "init: /init.sh exited: exit
  code 0" -> RB_AUTOBOOT -> QEMU exits -> run_qemu propagates 0.
- Watchdog path: `INIT_REBOOT=3` with the full suite -> "init: watchdog
  armed: reboot in 3 s" then "init: watchdog expired ... -- rebooting"
  mid-suite -> QEMU exits -> run_qemu exits 125.
- Isolation trick that found it: swapped init.sh for `sleep 30` only; its
  instant "exit code 0" proved guest nanosleep was dead too, which pointed
  at the kernel config rather than the supervisor.

### Next Steps

- Consider re-adding `sleep 1` in init.sh before exit now that
  nanosleep works again (probably unnecessary; the supervisor reboots
  immediately).
- If the rig kernel config is ever regenerated from scratch, POSIX_TIMERS
  must be re-enabled or both the watchdog and guest sleeps break again
  silently.

---

## Session 13: repo restructure -- tracked guest/ vs disposable build/ (2026-10-08)

### What We Did

The rig repo's `initrd/` tree mixed git-tracked rootfs content with
build-generated artifacts, and the `.gitignore` compensated with an
ever-finer denylist (`initrd/bin/test_*` ignored but `busybox` tracked;
`testdata/*.bin` ignored but `munin-2.0.78.tar` tracked).  Restructured
into a two-tree scheme:

- `guest/` -- tracked source that ships VERBATIM at the same path
  (`init.sh` -> /init.sh, `bin/busybox` + applet symlinks -> /bin,
  `testdata/munin*` -> /testdata)
- `tests/` -- guest program sources (unchanged)
- `scripts/` -- host tooling (unchanged role)
- `build/` -- 100% disposable, one gitignore line: `bin/` (compiled
  programs from build_tests.sh / build_fstools.sh), `rootfs/` (staging
  rebuilt from scratch by build_rootfs.sh), `rootfs.cpio.gz`, `kernel/`

`build_initrd.sh` became `build_rootfs.sh`: fresh staging every run =
`cp -a guest/.` + overlay `build/bin/.` + mknod + generated testdata ->
cpio.  Fails loudly if the compiled `/init` supervisor is missing.
The tracked `initrd/init` binary is gone -- it is derived from
`tests/init.c` like every other binary.

### What We Learned

- **The migration flushed out two latent bugs**, both variations of
  "generated stuff with no build rule":
  1. `build_tests.sh`'s hand-maintained TESTS list had drifted from
     `tests/*.c`: eight test binaries (bench, cell_gc, cell_storage,
     entry_race, mount_sem, mmap_coherent, multimount, size_limit)
     were invoked by `init.sh` but built by NO script -- they only
     existed as untracked leftovers in the old `initrd/bin/`.  The
     first fresh build from the new layout made the suite exit 127
     ("not found" x8).  Fix: derive the build list from `tests/*.c`
     so drift is structurally impossible.  Lesson: a tracked *output*
     hides missing build rules; deleting it is the only way to find
     out what actually produces the tree.
  2. `tests/test_concurrent.c` missed `<sys/time.h>`; implicit
     `gettimeofday` is a hard error on GCC 14, so it had never built
     on this host -- silently, because build_tests.sh treats a failed
     compile as non-fatal.
- Design detail worth keeping: binary builds and rootfs assembly must
  stay independent.  The first cut had build_tests.sh output straight
  into the staging tree, which build_rootfs.sh then wipes at the
  start of every run -- binaries vanished between the two steps.
  The `build/bin/` output dir + overlay copy breaks the cycle.
- `git mv` of symlink farms (busybox applet links) preserves the
  links; untracked leftovers next to them do not survive `rm -rf`,
  which is exactly the intended purge.
- Old root-level `rootfs.cpio.gz` became visible only after the
  gitignore rewrite dropped its entry -- rewrites of ignore files
  should be diffed against `git status` of the OLD tree first.

### What We Decided

- The repo splits into tracked guest/ (ships verbatim) and
  disposable build/ (staging, binaries, packed cpio, kernel);
  build_rootfs.sh rebuilds staging from scratch every pack.
- Derived artifacts (the init binary, rootfs.cpio.gz) stop being
  tracked; a stale binary cannot ship.

### What We'd Do Differently

- Design the build graph (which outputs are derived, who wipes
  them) before wiring scripts: the first cut wrote binaries into
  the staging tree the packer wipes, and they vanished between
  steps.
- Diff .gitignore rewrites against `git status` of the OLD tree;
  the stale rootfs.cpio.gz stayed invisible until the rewrite
  dropped its entry.

### Files Changed (high level)

| File | Purpose |
|------|---------|
| `guest/{init.sh,bin/*,testdata/munin*}` | moved from initrd/, now the tracked verbatim rootfs source |
| `scripts/build_rootfs.sh` | replaces build_initrd.sh: fresh staging + pack |
| `scripts/build_tests.sh` | glob-derived test list, outputs to build/bin/ |
| `scripts/{build_kernel,build_fstools,run_qemu,run_tests,quick_test,setup_test_env}.sh` | retargeted to build/ paths |
| `tests/test_concurrent.c` | missing <sys/time.h> (GCC 14 hard error) |
| `.gitignore`, `README.md` | two-tree rule documented |

### Test Results

Full pipeline from the new layout: build_tests (16 tests + init, all
OK) + build_fstools (fsx, fstorture) + build_rootfs (14M cpio, all
binaries verified present) -> sweep boot exit 0; INIT_REBOOT=3
watchdog kill mid-suite -> run_qemu exit 125.

### Next Steps

- The CI snippet in README.md was updated mechanically but has never
  actually run; if the rig ever goes to CI, QEMU+KVM availability and
  the kernel build from /home/snide/linux are the first blockers.
- `setup_test_env.sh` still hardcodes the rig path like everything
  else; a REPO="$(cd "$(dirname "$0")/.." && pwd)" sweep across the
  scripts would make the repo relocatable.

---

---
name: openwrt-refocus
description: the fleet is OpenWrt (64M IoT + 2G build boxes) -- squashfs+overlay+ztmpfs stack designed, OpenWrt 25.12.5 booted under KVM, refocused on tmpfs and tmpfs+zcell
project: linux
previous: 2026-10-11_ztmpfs-rationalization-bench.md
---

# Mission Log: OpenWrt, the Fleet, and the Refocus

Part 24 of the tmpfs transparent-compression series.  Session 40
(2026-10-11).  No kernel code changed; this is a design-and-recon
session that ended in a deliberate narrowing.  The brief, in the
user's words: "i have 64MB sbc and 2G ones.  the 64M ones are iot,
the 2G are headless build boxes", "i'm really interested for its use
in openwrt", and at the close: "i think we have too many loose ends
open.  lets refocus purely on tmpfs and tmpfs+zcell".

Nothing is posted upstream.  Nothing is built this session beyond the
stock OpenWrt image's own boot.

## What We Did

1. **Bootstrapped from the record** (logs 37-39): s1 branch green,
   rationalization bench arms 1-2 measured (arm 2: -44% live-set RAM,
   1.9x write, survives the 256M squeeze where arm 1 OOM-kills),
   arm 3 (zcell bulk tier) reserved in the `test_zbench` harness.

2. **Mapped the fleet to the 3-arm story.**  64M IoT = survival
   regime: reclaim never converges before OOM, so compression must be
   unconditional -> arm 2/3 territory, where the canonical answer
   (tmpfs+zramswap) dies.  2G build boxes = comfortable regime,
   CPU-saturated: reclaim-driven policy is adaptive for free, hot
   files never pay compression -> arm 1 wins, no work owed there.

3. **Reaffirmed the zcell scope.**  Session 35's verdict killed
   zcells-as-RAM-blockdev on random writes; it never tested zcell as
   the bulk tier *inside* ztmpfs (arm 3).  `cell_store.c` survived
   the cellzram removal on `feature/tmpfs-compression`; arm 3 is
   buildable.  Its properties (zero per-object metadata, floor of
   one cell, synchronous buddy return) appreciate exactly at 64M.

4. **Settled squashfs vs ext2 for the ro lower layer.**  squashfs:
   2-3x smaller flash image, less boot/cold-read I/O, RO enforced by
   construction, fragment packing suits small-file IoT rootfs.  The
   RAM story is a tie (both read into evictable page cache, dropped
   under pressure, no swap).  ext2's only win is no decompress CPU
   on cache miss -- noise against a slow flash read.  No work item;
   context only.

5. **Mapped the stack onto OpenWrt's real mount layout.**

   ```
   rom       = squashfs lower            (every OpenWrt image)
   /         = overlayfs
   /overlay  = flash upper (jffs2/ubifs; ext4 on x86 -- see below)
   /tmp      = tmpfs, NO size cap        <-- the ztmpfs target
   ```

   OpenWrt's answer to RAM pressure today is the zram-swap package
   -- that is our arm 1, the Android pattern bolted on, with device
   sizing, writeback and swappiness knobs.  /tmp growth with no
   swap is a direct OOM path on 64M boxes.  ztmpfs is arm 2 with
   zero configuration.  Two ztmpfs homes: /tmp on every device, and
   the volatile-overlay upper (failsafe/testing/write-averse mode,
   where the entire system state is RAM -- compressed upper is the
   point of that mode).  Upstream-first (s1) is the enabler:
   OpenWrt tracks mainline and will not carry big out-of-tree mm
   patches; adoption becomes a mount-option change, no image-layout
   rework, since squashfs+overlay compatibility is total.

6. **Downloaded real OpenWrt** (rig `images/openwrt/25.12.5/`,
   sha256-verified against the published `sha256sums`):
   `squashfs-combined.img` (grub p1 + squashfs p2, 104 MiB),
   `squashfs-rootfs.img` (raw squashfs, xz, id 4 -- the piece for
   `-kernel our_bzImage root=/dev/vda`), `kernel.bin` (their
   6.12.94), `targz-rootfs.tar.gz` (inspectable userspace).  The
   stock image booted to login console under QEMU: procd, ubus,
   overlay upper mounted, network up.

7. **Corrected TCG to KVM.**  My hand-rolled smoke-boot omitted the
   accelerator; QEMU silently fell back to full emulation.  The rig's
   own `run_qemu.sh` already does it right (`-accel kvm` + `-accel
   tcg,thread=multi` fallback).  Re-ran with `-enable-kvm -cpu host`.

8. **Checked the rig kernel config for the our-kernel boot.**
   Present: `BLOCK`, `EXT4_FS`, `DEVTMPFS`, `TMPFS` + compression.
   Absent: `SQUASHFS` (+ xz decompressor), `OVERLAY_FS`, PCI, any
   virtio/SCSI/ATA driver -- the rig config is minimal because the
   rig boots initramfs.  These additions are the next build's delta.

9. **Refocused** (user decision at close): pure tmpfs and
   tmpfs+zcell.  Parked items are named under Next Steps with revisit
   conditions, so they stop haunting the list.

## What We Learned

### Technical

- **The fleet IS the OpenWrt ecosystem.**  Every application-search
  question from earlier sessions resolves against real hardware:
  64M IoT boards are the ath79/ramips/ipq806x class; 2G boxes run
  the buildroot.  The 3-arm bench was always measuring this split;
  it just didn't know the fleet.

- **On x86, `mount_root` switches the overlay upper to ext4**
  (boot log: "mount_root: switching to ext4 overlay").  jffs2/ubifs
  are MTD-only.  Consequence for the testbed: QEMU's overlay upper
  is NOT the fleet's flash upper; the fleet-representative upper
  modes are jffs2/ubifs (persistent) or tmpfs (volatile) -- the
  latter is where ztmpfs plugs in.

- **The standalone OpenWrt `rootfs.img` is a raw squashfs
  filesystem** (magic `hsqs` at offset 0, no partition table, xz
  compressor id 4).  Bootable as `-kernel <ours> -drive
  file=rootfs.img,if=virtio,ro -append "root=/dev/vda
  rootfstype=squashfs rootwait console=ttyS0"` once the config has
  SQUASHFS+xz, OVERLAY_FS, VIRTIO_BLK/PCI.

- **KVM vs TCG on the same image, 256M:** procd init 13.7s -> 6.2s,
  network up ~40s -> 10s.  Boot-time deltas (the number OpenWrt
  maintainers will ask for first) are measurable at KVM resolution.

- **The mksquashfs tooling blocker mostly dissolves.**  Real
  OpenWrt images serve as their own squashfs lowers; the tool is
  only needed if we want custom-dataset squashfs images inside
  `test_zbench`.  The no-install question stays open for that case
  only.

- **zram-swap on OpenWrt is arm 1 in the wild.**  The forum
  complaint cycle (OOM on 64M boxes, bolt on zram, tune three knobs)
  is the exact gap arm 2 fills with zero configuration.  "Android
  ships arm 1 at billions of devices; for no-swap embedded arm 2 is
  the simpler machine" is a cover-letter line with a real audience.

### Process

- **One-off QEMU commands must inherit the rig's flags.**  The rig
  had KVM wired correctly; my hand-rolled invocation silently lost
  it and cost a full TCG boot cycle.  The harness exists; use it, or
  copy its invocation verbatim.

- **User hardware context reframes an application search faster than
  any bench.**  Thirty-nine sessions of generic 1G/256M points, and
  the two real deployments arrived in one sentence.  Ask for the
  fleet early; the matrix falls out of it.

- **Loose-ends hygiene, per the user's complaint:** parked work gets
  an explicit name and a revisit condition in the log, and Next
  Steps stays short.  A long list where everything is "open" is how
  focus leaks.

## What We Decided

- **Application focus: OpenWrt, ztmpfs on /tmp (and the volatile
  overlay upper), at the 64M regime.**  The bench (test_zbench) and
  the real-distro boot are the two instruments; both exist.

- **squashfs is the ro lower, settled, no work item.**  The RFC's
  deployment section describes the stack; it does not benchmark the
  lower.

- **zcell = arm 3 bulk tier only.**  Improvement targets unchanged
  from session 39: cell_order axis, fixed-size directory wall at
  small orders, GC/synchronous-free rm story.  Driven through the
  same harness -- the session-33 rule (no blind zcell changes).

- **The 2G build boxes are arm 1's home and get no work.**  The RFC
  says so honestly: arm 1 wins where it works, arm 2/3 wins where
  the system dies.

- **Refocus, per the user:** pure tmpfs and tmpfs+zcell.  Everything
  else is parked with a revisit condition (see Next Steps).

## Rules Added

- **Every QEMU invocation for this work runs on KVM** (`-enable-kvm
  -cpu host`, or the rig's `-accel kvm` + tcg-fallback pattern).
  Verify it engaged (boot-time sanity: procd init should be single
  digit seconds) -- silent TCG fallback wastes a whole boot cycle.

- **Parked work is named in the log with its revisit condition.**
  "Open" without a condition is how a next-steps list decays into
  lore (session 37's lesson, now applied prospectively).

## What We'd Do Differently

- Would have asked for the actual deployment fleet before session
  ~20; the bench matrix would have grown fleet-shaped (64M point,
  small-file churn workload) instead of generically.

- Would have bootstrapped the OpenWrt smoke test through the rig's
  QEMU invocation instead of a hand-rolled command; the rig already
  encodes the accelerator flags and serial handling.

- The squashfs-vs-ext2 comparison could have been one paragraph of
  reasoning instead of a discussion thread -- the RAM stories are
  identical by construction, so the decision was never bench-
  material.

## Files Changed

| File (under rig root `/home/snide/tmpfs-compress-test/`) | Purpose |
|------|---------|
| `images/openwrt/25.12.5/openwrt-...-squashfs-combined.img(.gz)` | stock bootable OpenWrt disk image (sha256-verified) |
| `images/openwrt/25.12.5/openwrt-...-squashfs-rootfs.img(.gz)` | raw squashfs rootfs for our-kernel boots |
| `images/openwrt/25.12.5/openwrt-...-kernel.bin` | stock 6.12.94 kernel for comparison boots |
| `images/openwrt/25.12.5/openwrt-...-targz-rootfs.tar.gz` | inspectable OpenWrt userspace tree |
| `images/openwrt/25.12.5/sha256sums` + `boot_stock.log` + `boot_kvm.log` | verification record and boot logs (TCG, KVM) |
| `mission_log/2026-10-11_openwrt-refocus.md` | this log |

No kernel or rig code changed this session.

## Test Results / Benchmarks

| run | result |
|---|---|
| sha256sum -c, 4 OpenWrt artifacts | all OK |
| stock OpenWrt 25.12.5, TCG, 256M | full boot to login; procd 13.7s, net ~40s |
| stock OpenWrt 25.12.5, KVM (`-enable-kvm -cpu host`), 256M | full boot to login; procd 6.2s, net 10s |

These are recon numbers (image sanity, accelerator delta), not
series results.

## Next Steps

Refocused list, in order:

1. **OpenWrt rootfs on our kernel.**  Config delta: `SQUASHFS` +
   xz decompressor, `OVERLAY_FS`, `VIRTIO_PCI`/`VIRTIO_BLK`; build
   the `feature/tmpfs-compression` kernel; boot the downloaded
   rootfs.img; flip `/tmp` to `mount -t tmpfs -o compress=lz4`
   (ztmpfs); measure boot-time delta and memory vs the stock kernel,
   at 256M and 64M, on KVM.
2. **`test_zbench` fleet points:** 64M VM + OpenWrt-shaped
   small-file churn workload (boot writes, log appends, rename-based
   config updates), arms 1/2, same gauges.
3. **Arm 3: zcell bulk tier** through the same harness, with the
   three named improvement targets.  The payload of the series.

Standing s1-track items (unchanged, from sessions 38/39):
memory-failure answer for `SWP_SHMEM_COMPRESS`, in-tree selftests,
Sphinx-verified docs (blocked on no-install), RFC assembly.

Parked, with revisit conditions:

- **zcell layer for NAND/thin storage** -- revisit after arm 3 is
  integrated and measured.  (The session-35 verdict scoped zcells
  out of RAM blockdevs on random writes; it says nothing about flash
  geometry, where O(1) bump alloc and synchronous free are a
  different game.)
- **2G build-box tuning / deferred-compression policy** -- no work;
  arm 1 wins there.  Revisit only if a build-box workload shows up
  that arm 1 handles badly.
- **ext2-vs-squashfs for the ro lower** -- settled (squashfs);
  reopened only by new information about the fleet's storage.
- **mksquashfs tooling** -- needed only for custom-dataset squashfs
  lowers inside `test_zbench`; the OpenWrt images cover the real
  case.  Revisit if the bench needs its own lower image.

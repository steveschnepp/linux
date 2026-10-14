---
name: per-mount-comp-stats
description: per-mount compression counters -- closing the session-14 standing gap so several compressed mounts are observable in one boot (session 27)
project: linux
previous: 2026-10-09_headroom-sweep.md
---

# Mission Log: Per-Mount Counters, and What One File Replaced

Part 11 of the tmpfs transparent-compression series.  Session 27
(2026-10-09).

This closes the oldest standing item in the series.  Session 14
recorded it while building the Stage 2 machinery; it reappeared in the
Next Steps of sessions 15, 16, 18 and 19, each time with the same
sentence and one more reason it mattered:

> *"Per-mount comp_stats readers (standing gap) -- the one-boot matrix
> works around them with per-config deltas on global counters."*

Kernel commits: `4c033366a1c8`.
Rig commits: `69d9c4b`.

## Session 27: per-mount compression counters

### What We Did

1. **`mount_stats`, one file.**  `/sys/fs/tmpfs/compression/mount_stats`
   emits one line per compressed mount:

   ```
   dev=0:20 stat_dev=20 mode=chill wm=50:70 pages=0 bytes=0 aged=0 skipped=0 cells=1
   dev=0:19 stat_dev=19 mode=hot   wm=0:0  pages=0 bytes=0 aged=0 skipped=0 cells=1
   ```

   `stat_dev` is `sb->s_dev` in decimal -- the number
   `stat -c %d <mountpoint>` prints -- so a test picks out its own
   mount with one grep instead of needing a name or an index.

2. **A single file rather than a per-mount kobject tree.**  The only
   consumer is a reader.  A kobject per mount would add lifetime
   management tied to `fill_sb`/`put_super` to serve one grep, and the
   mount list plus mutex it would have to walk anyway already exists
   for `gc_now` and the aging work.  Deliberately the smallest thing
   that closes the gap.

3. **`tests/test_mount_stats.c`.**  Mounts an eager (`0:0`) and a
   chill (`50:70`) tmpfs *together* -- precisely the configuration the
   matrix could not observe, and the reason it ran one mount per boot.

### The result, and the assertion that matters

```
A: mode=hot   pages=49 aged=0  skipped=0     (0:0 eager)
B: mode=chill pages=0  aged=0  skipped=64    (50:70 chill)

after gc_now: A.aged=15  B.aged=0  (global delta 15)
```

The gate asserts **attribution, not magnitude**.  Magnitude is
workload-dependent and would have made a brittle gate; attribution is
the property the reader exists for, and it is exactly what aggregated
counters cannot express:

- Unlinking B's chilled pages moves **B's `skipped_dead` to 64** while
  **A's stays 0**.  A compressed at write time, so it had nothing to
  skip.  With global counters there is no way to ask which mount
  produced a number, let alone to see that one did not.
- After `gc_now`, the per-mount `aged` counters **sum exactly to the
  global delta** (15 = 15 + 0).  Not just non-overlapping -- complete.
  This is the check that would catch a mount silently missing from the
  list, which a per-mount-only reader could not.

### What We Learned

#### Technical

- **The gap was cheap to close, and expensive to live without.**  The
  reader is ~35 lines using a list and a mutex that already existed.
  Five sessions recorded it as a standing item because each one was
  busy with something more urgent, and every one of them then worked
  around it (one mount per boot; global deltas; dmesg attribution for
  flip lines).  The workaround cost -- a boot per configuration, and a
  matrix that could not cross mounts -- was repeatedly larger than the
  fix.

- **A counter list answers questions a summary cannot.**  The summary
  (`comp_stats`) and the list (`mount_stats`) are the same data at
  different cardinalities, and the list is strictly more informative.
  Session 15's live-gauge-vs-cumulative-counter mistake was a
  *related* failure: with one number for N mounts, the reader has to
  guess which question the number answers.

- **`stat -c %d` and `sb->s_dev` are the same value**, which is what
  makes a stable join key possible without inventing a mount name.
  tmpfs has no useful per-mount identifier otherwise: `sb->s_id` is
  `"tmpfs"` for every mount.  Emitting both `MAJOR:MINOR` (for humans)
  and the raw `dev_t` (for grepping) costs nothing and removes the
  need for a caller to know the conversion.

#### Process

- **The raw-dump rule caught two consecutive parser bugs before it
  could look like a kernel bug.**  Both failed runs printed the exact
  line that could not be parsed:
  1. `lz4` is not a mount parameter -- the option is `compress=lz4`.
     tmpfs rejected it loudly (`Unknown parameter 'lz4'`), so this one
     was free.
  2. The `sscanf` format skipped `wm=` and `bytes=`, jumping from
     `mode=` straight to `pages=`.  Had the raw line not been printed,
     "no line for /mnt/ms_a" would have pointed the investigation at
     the kernel rather than at my own format string.

  Two parser bugs in a file that also dumps its input.  The rule has
  now caught three separate bugs across this series (bytes-vs-KB
  misparse, `memparse()` bare number, and these two).

- **Confirm a parser against the real line on the HOST before spending
  another boot.**  After the second failure the format was checked with
  a 10-line standalone repro compiled from the current source, which
  returned `n=9` in a second.  Session 18's rule -- a standalone repro
  compiled from the current source bisects binary-vs-source doubt in
  one step -- applied just as well to format-vs-reality doubt, and
  saved a 2-minute QEMU cycle.

- **Serial interleaving mangled two PASS lines** ("h-before-pop)" in
  the middle of a result).  Session 12's two-writers rule: a kernel
  printk crossing a test printf on the shared console.  Not corruption,
  and not worth fixing -- but it is why the gate's verdict is the
  `Test exit code` and the pass/fail tally, never a grepped line.

### What We Decided

- **`mount_stats` is one read-only file, not a kobject tree.**  Sized
  to the consumer.  If a per-mount *write* control ever appears (a
  per-mount `gc_now`, say), that is the point at which a real kobject
  earns its lifetime machinery.

- **The gate asserts attribution and completeness, not magnitudes.**
  "B's unlink did not move A's counter" and "the parts sum to the
  whole" are properties of the reader; "skipped == 64" is a property of
  the workload and would have made the gate fragile.

- **The raw line is printed by the gate, permanently.**  Not a debug
  aid removed after the session -- the same discipline that caught
  three bugs is the reason it stays.

### Rules Added

- **When a standing gap keeps being worked around, price the
  workaround.**  Five sessions recorded this one; each workaround cost
  a boot per configuration or a weaker inference.  A gap that survives
  several sessions is usually cheap to close and expensive to keep.

- **A reader over N instances should be checked for completeness, not
  only for its own values.**  Per-mount numbers that do not sum to the
  known global total mean an instance is missing -- a failure a
  per-mount-only reader cannot see.

- **Validate a sysfs format string against the real line on the host
  before the next boot.**  A parse failure looks exactly like a kernel
  bug until the input is printed, and printing it is free.

### What We'd Do Differently

- Would have written this in session 14, when it was first identified
  and the workaround was first being built.  It is ~35 lines over
  existing machinery, and the five sessions of workaround around it
  were each more expensive than the fix.

- Would have written the raw dump *before* the first parse attempt
  rather than adding it after the first failure.  It is now in the gate
  permanently, which is the right place, but the first boot of a new
  parser should print its input by default.

- Would have run the standalone parser check before the second boot
  rather than after it.  The rule existed (session 18) and was simply
  not applied to a new class of doubt -- format vs reality.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem_compress.c` | `mount_stats_show()` + attribute registration; sysfs group comment updated |
| `~/tmpfs-compress-test/tests/test_mount_stats.c` | new: two compressed mounts at once, attribution + completeness assertions, raw dump |
| `~/tmpfs-compress-test/guest/init.sh` | `test.mountstats=1` isolation gate |
| `~/tmpfs-compress-test/scripts/run_qemu.sh` | `TEST_MOUNTSTATS` passthrough |

### Test Results

**The gate** (`TEST_MOUNTSTATS=1`): **8 passed / 0 failed, exit 0.**

| assertion | result |
|-----------|--------|
| two mounts with distinct `st_dev` | 19 and 20 |
| `mount_stats` lists both in one boot | 2 lines |
| A (`0:0`) reports `hot` | yes |
| B (`50:70`) reports `chill` | yes |
| A compressed at write time (`pages > 0`) | 49 |
| B counted `skipped_dead` on unlink | 64 |
| **A's `skipped_dead` stayed 0** | yes -- B's unlink did not leak |
| **per-mount `aged` sums to global delta** | 15 == 15 + 0 |

**No regressions** from adding a sysfs reader to a hot file:

| gate | result |
|------|--------|
| watermark policy | ALL PASSED (0 failures) |
| torture | 231 / 0 |
| multimount | 4 / 0 |
| full suite (QEMU_CPUS=4, KASAN) | exit 0 |

`mm/shmem.c` untouched, so the `CONFIG_TMPFS_COMPRESSION=n` gate is
unaffected -- `shmem_compress.o` does not exist in a config-off build.

### Next Steps

1. The guidance table's "when" rows can now be read operationally
   rather than analytically.  Re-running the policy matrix with several
   mounts per boot would collapse a ~10-boot sweep into one; worth doing
   the next time any watermark or size axis is re-measured.
2. `gc_recoveries` (session 26) is still cellzram-only.  A per-mount
   equivalent would say whether any real tmpfs workload ever exhausts
   its default 256-cell directory -- cheap now that the reader exists.
3. A per-mount `gc_now` would be the first *write* control, and the
   point at which the single-file design should be revisited in favour
   of a real kobject.

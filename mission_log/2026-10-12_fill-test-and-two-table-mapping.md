---
name: fill-test-and-two-table-mapping
description: fill test for 3G/2G overcommit + 1.5G/2G perf; read-back OOM traced to compressed-XOR-uncompressed page design; two-table (i_pages + entries[]) mapping decided
project: linux
previous: 2026-10-12_gc-reverse-index.md
---

# Mission Log: The Fill Test and the Two-Table Mapping

Part 27 of the tmpfs transparent-compression series.  Session 43
(2026-10-12).  A measurement session that hit a wall, traced it to a
design flaw, and turned into a design decision.  The brief, in the
user's words: "on a 2G qemu, use a 3G tmpfs.  fill it.  with zram, with
tmpfs+zram, with tmpfs+zcell", "i also want to undertand the
performance of the setup.  2G qemu, use a 1.5G tmpfs.  filled", then,
after the read-back OOM: "why the test does oom on read? ... it is up
to the kernel to actively reclaim pages ... that is the tension:
currently a page can be either compressed or uncompressed.  it actaully
need to be both", and finally "why not have 2 tables xarray[] and
folios[] with 0 being empty ... same size for both tables (SoA)".

Nothing is posted upstream.  One rig commit this segment (`8bf9acb`,
the fill test).  The spec + code decisions below are the next steps,
committed after this log.

## What We Did

### 1. The accounting fact that shaped the test

Before writing the fill test, checked whether compressing a page
releases its `size=` charge.  It does not: the compress path does
`shmem_recalc_inode(inode, 0, nr)` which raises `swapped` by `nr` as
`nrpages` drops by `nr`, so `freed` stays 0 and the block charge is
retained.  A compressed page is charged against `size=` exactly like a
swap page.  Consequence: **compression saves RAM, not the size
budget** -- a `size=3G` tmpfs ENOSPCs at 3G of *logical* data
regardless of compression.  So the 3G/2G test is purely "does RAM
survive the fill," and ENOSPC (not OOM) is the success signal for a
surviving arm.

### 2. Wrote `test_fill.c` (rig commit `8bf9acb`)

One parameterized test, `test_fill <arm> <size_mb> [zram_mb]
[readback]`, gated behind `test.fill=<arm>`.  Fills a `size=` tmpfs
with ~2:1 lz4 data until ENOSPC or a time cap; reports write MB/s,
bytes written, final RAM, and the arm's gauge (zram mm_stat /
comp_stats).  Three robustness fixes, each earned by a failure:

- **explicit `size=` on the mount** -- without it tmpfs defaults to
  half of RAM, so a 3G fill on 2G ENOSPC'd at ~1G (the first run's
  "1001 MB wrote, enospc, 685 MB still free" was this, not a kernel
  result).
- **`oom_score_adj=1000`** -- the first runs lost whole results when
  the OOM killer took the *harness shell* before it could `cat` the
  results file (arm 3 showed zero output).  Making the test the
  preferred victim keeps the shell alive to report.
- **FILL_CAP_S time cap** -- arm 1 (zram swap) hit swap-death thrash
  and hung the boot at the 900s timeout; the cap turns that into a
  partial result.

Also fixed a CRC check that compared every byte against the first-step
seed (my bug, not corruption).

### 3. The overcommit result, and the read-back wall

3G tmpfs / 2G RAM, write-only (readback=0):

| arm | wrote | enospc | write MB/s | outcome |
|---|---|---|---|---|
| 1 tmpfs+zramswap | -- | -- | -- | swap-death thrash, capped |
| 2 zsmalloc | 3072 MB | no | ~1000 | **survived**, 1.6GB stored for 3.2GB |
| 3 cells | -- | -- | -- | OOM (harness lost, before the oom_score_adj fix) |

Arm 2 wrote the full 3072 MB -- compression let a 3G tmpfs fill on 2G
RAM, which is the survival story.  But the **read-back OOM'd**: reading
the compressed tmpfs materialized uncompressed folios until the box
died.

### 4. The OOM diagnosis -- and a wrong fix

First guessed the read buffer accumulated; no -- it was a single reused
32MB buffer.  The real cause: **reading a compressed tmpfs page
decompresses it into a full 4K uncompressed folio in the page cache AND
frees the ~2KB compressed entry.**  So reading back 3G accumulates 3G of
uncompressed pages with nothing reclaimable behind them.  The page
cache is the accumulator.

Attempted `mmap` + `madvise(MADV_DONTNEED)` per file.  **It did not
work** -- verified in the source: `madvise_dontneed_single_vma()` calls
`zap_vma_range_batched()`, which zaps the process's PTEs but does *not*
free the shmem page cache (shmem has no `release_folio`; its pages are
not clean-file-cache droppable).  So the decompressed pages stayed
resident and the read still OOM'd.

### 5. The user's design correction -- a page must be both

The OOM is not a test bug; it is a kernel-design gap.  The current
design is **binary**: a page is compressed XOR uncompressed, and the
xarray slot ping-pongs.  Compressing frees the folio and swaps the slot
to an `{id,gen}` value; reading frees the entry and swaps the slot back
to a folio.  There is never both.  That is *why* reads accumulate.

The corrected model: **the compressed entry is durable, the uncompressed
folio is a reclaimable cache of it.**

```
write        -> [ uncompressed ]             folio, no entry yet
GC compress  -> [ compressed  ]              entry created; folio freed
read         -> [ compressed + uncompressed ] decompress to folio, KEEP entry
reclaim/GC   -> [ compressed  ]              drop folio; entry survives
read again   -> [ compressed + uncompressed ] re-decompress on demand
modify       -> drop entry (stale); folio sole copy; GC re-compresses
```

This makes reads stop accumulating: reclaim drops uncompressed folios
as fast as the reader materializes them.  It is the normal page-cache
relationship (disk authoritative, cache reclaimable), with "disk" being
a compressed blob in zsmalloc/cells.

### 6. The representation -- two tables, and why it beats the alternatives

The xarray slot holds *one* thing, so "both" needs a representation.
Two were considered:

- **`folio->private` + slot flip** (my fallback): keep the entry id in
  `folio->private`, flip the slot folio<->value on reclaim.  Works, but
  a state machine, repurposes `private`, and does not remove the
  `SWP_SHMEM_COMPRESS` encoding.
- **Two parallel tables** (the user's): `i_pages[N]` = folio (the
  existing page cache, which *is* the folio table -- the MM operates on
  it and it cannot be replaced), and a new `entries[N]` xarray, same
  pgoff index space, holding the compressed entry id; 0 = none.

The two-table shape wins: "both" is a stable state (both non-empty)
rather than a flip; reclaim clears `i_pages[N]` and invalidate-on-write
clears `entries[N]`, each independent; hot reads stay a direct folio
lookup; refcounting simplifies (the entry lives while `entries[N]`
names it, no folio->ent coupling); truncate is two independent
range-clears.

**The bigger win: it dissolves the `SWP_SHMEM_COMPRESS` encoding.**
Spec 02 flags packing `{id,gen}` into a reserved swap-type `xa_value`
multiplexed into the page-cache slot as the single biggest upstream
review risk.  Two tables remove it outright -- the compressed mapping
lives in its own per-inode xarray, not multiplexed with folios and real
swap entries.  A private per-inode map alongside the page cache is
*more* orthodox FS design (btrfs extent maps, f2fs node maps) than the
multiplexing it replaces.

## What We Learned

### Technical

- **`size=` is charged on logical bytes, not physical.**  A compressed
  page is charged like a swap page (`shmem_recalc_inode(inode, 0, nr)`
  pairs the `nrpages` drop with a `swapped` rise, freed stays 0).
  Compression saves RAM, never the size budget.  This decided the whole
  test shape.

- **MADV_DONTNEED does not free shmem page cache.**  It zaps the
  process PTEs; the pages stay resident because shmem has no
  `release_folio`.  Verified in `mm/madvise.c` + `mm/shmem.c` before
  concluding.  The wrong fix cost one boot cycle; the source check is
  what turned "it still OOMs" into "here is why."

- **A >RAM tmpfs read-back cannot work while a page is compressed XOR
  uncompressed.**  The decompressed folio replaces the compressed entry,
  so a full read accumulates uncompressed pages with nothing to reclaim
  against.  The fix is architectural (keep both), not a test workaround.

### Process

- **The fill test's three robustness fixes were all "the bench measured
  the wrong thing" catches** -- the size= default, the shell OOM, the
  swap-death hang.  Fourth time this series.  The `oom_score_adj` +
  time-cap + explicit-size pattern is now what any OOM-prone fill test
  in the rig should carry.

- **A wrong fix that fails identically to the original bug is a signal
  to read the source, not to try another guess.**  mmap+MADV_DONTNEED
  OOM'd exactly like plain read; the source check (no `release_folio`)
  settled it in one read where another guess would have cost another
  boot.

- **The user's design instinct corrected two layers at once** -- the
  page lifecycle AND the upstream encoding risk.  The two-table mapping
  is not just the read-OOM fix; it removes spec 02's biggest review
  risk.  Worth noting that a representation question ("2 tables?")
  dissolved a problem ("the swap-type encoding") we had accepted as
  permanent.

## What We Decided

- **The page lifecycle becomes "compressed is durable, uncompressed is a
  reclaimable cache."**  A page can be compressed, uncompressed, or
  both; never compressed-XOR-uncompressed.

- **Representation: two parallel tables** -- `i_pages[]` (existing page
  cache, folios) + a new `entries[]` xarray (compressed entry ids),
  same pgoff index space, 0 = empty.  `i_pages` cannot be replaced (the
  MM depends on it); `entries` is new and lazily allocated per inode.

- **The `SWP_SHMEM_COMPRESS` reserved-swap-type encoding is removed** as
  a consequence -- the compressed mapping moves out of the page-cache
  slot into `entries[]`.  This closes spec 02's biggest upstream review
  risk.

- **Order of work, per the user: mission log (this) -> spec 02 update
  -> code -> test, committing bisectably at each step.**

## Rules Added

- **A fill/stress test that can OOM must set `oom_score_adj=1000`, pass
  an explicit mount size, and carry a time cap** -- so the test is the
  OOM victim (harness survives to report), the fill target matches the
  mount limit, and thrash shows as partial not a hang.

- **When a fix fails identically to the original bug, read the kernel
  source for the mechanism before guessing again.**  One source read
  beat one boot cycle.

## What We'd Do Differently

- Would have set `oom_score_adj` and the time cap in the *first* draft
  of test_fill; three separate runs were lost or corrupted before the
  pattern clicked.

- Would have checked `shmem_aops` for `release_folio` *before*
  attempting the mmap+MADV_DONTNEED fix -- the absence of
  `release_folio` is the whole answer and it was a two-line grep away.

## Files Changed

| File | Purpose |
|------|---------|
| rig `tests/test_fill.c` (commit `8bf9acb`) | the fill test: throughput + RAM, 3 arms, OOM-aware |
| rig `guest/init.sh` | `test.fill=<arm>` boot mode |
| rig `scripts/run_qemu.sh` | `TEST_FILL*` passthroughs |

No kernel code changed this segment.  The two-table mapping is the next
step: spec 02 update, then code, then the read-back OOM test on a plain
`read()`.

## Test Results / Benchmarks

Non-KASAN kernel, 2G RAM.

3G tmpfs / 2G RAM, write-only (overcommit survival):

| arm | wrote | enospc | write MB/s | gauge | outcome |
|---|---|---|---|---|---|
| 1 tmpfs+zramswap | -- | -- | -- | -- | swap-death thrash, capped |
| 2 zsmalloc | 3072 MB | no | ~1000 | 1.6GB stored / 3.2GB | **survived** |
| 3 cells | -- | -- | -- | -- | OOM (pre-fix harness loss) |

Arm 2: comp_pages=786432, compr=1.64GB, uncomp=3.22GB, ram_total
1.75GB, CRC sample ok.  Arm 1 thrashed to swap-death (the "compression
is hypothetical without pressure, and dies before it converges" story
again).  Arm 3 needs a rerun on the fixed harness.

Read-back (readback=1): **OOM on every compressed arm** -- the
two-table mapping fix is what makes this work; the test-side
mmap+MADV_DONTNEED did not (shmem pages are not PTE-zappable to free).

## Next Steps

1. **Spec 02 update** -- rewrite the data-path and page-lifecycle
   sections around the two-table model; add the "both" state and its
   accounting (folio 4K + blob ~2K = ~6K for a hot compressed page,
   reclaim uncharges the folio) to Goals/Risks; mark
   `SWP_SHMEM_COMPRESS` encoding as removed.
2. **Code** -- implement `entries[]`, the lifecycle transitions, the
   entry pin via `entries[N]`, the write-invalidate, the reclaim
   drop-folio-keep-entry.  Remove the swap-type encoding.
3. **Test** -- rerun the 3G/2G read-back on a plain `read()`; confirm
   the OOM is gone at the kernel level.  Then the 1.5G/2G performance
   case (which fits in RAM and should be clean throughput).
4. **Rerun arm 3** on the fixed harness (oom_score_adj) so all three
   arms have overcommit-survival numbers.
5. Standing items: M-boot the perf numbers, cell_order sweep, the
   arm-3 rm/synchronous-free story at 64M.

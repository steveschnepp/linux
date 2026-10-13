# Watermark RAM-Pressure Two-Phase Policy Specification

## Overview

Re-base `compress_watermark=LOW:HIGH` on **RAM usage** instead of mount
usage, evaluate the hot/chill flip on the write path (phase 1:
synchronous compression while RAM is scarce), and unify the background
machinery into **one worker with two triggers** -- the existing 1 Hz
cadence and an immediate kick at the highwater flip (phase 2).  Each
pass compresses the chilled backlog first (age-gated in calm mode,
immediate under pressure), then runs the evacuation GC as today.  The
triangle sweep showed the current policy cannot engage at tmpfs==RAM
against a fast writer; this spec makes it engage where it matters and
stays calm where it does not.

## Motivation

Failures of the Stage-2 policy as built, both exposed by
`mission_log/2026-10-13_zcell-watermark-triangle.md`:

1. **Wrong actuator latency.** The flip lives only in the 1 Hz
   maintenance pass; at exact-fit, RAM exhaustion is sub-second, so
   the flip fires at the deathbed (usage 92% in every non-eager boot)
   and drains a corpse at 2 MiB/s.
2. **Wrong signal.** Mount usage measures the wrong scarcity.
   Compression buys RAM; the trigger should be RAM pressure.  A
   half-full mount on an exhausted box needs compression; a full mount
   on a 4x-oversized box does not.

And one behavioral gap the calm path has today: the aging pass
compresses **any** resident uncompressed folio, however young.  A page
written, compressed by the bg job a moment later, then read back pays
compression *and* decompression CPU for nothing -- the ripple effect.
Calm-mode compression should only touch settled pages.

## Goals

- Flip evaluated on the write path with hysteresis against RAM usage:
  `usage >= HIGH` -> hot, `usage <= LOW` -> chill, band holds.
- Phase 1 (sync): hot means every write compresses inline via the
  existing `shmem_compress_folio()` path, unchanged.
- Phase 2 (background): ONE worker, two trigger sources:
  - the 1 Hz cadence (existing `age_work` arming behavior), and
  - an immediate kick on the chill->hot flip (the highwater
    compressor; one kick per flip, reset by the return to chill).
- Pass duty order: (1) eager-compress chilled residents, auto-skipped
  when none exist; (2) evacuation GC -- the existing ~4:1 interleave,
  unchanged.
- **Age gate:** in calm mode (chill; cadence-driven) only pages that
  survived a previous pass are compressed ("seen" marker).  Under
  pressure (hot mode, i.e. the highwater has fired) the age gate is
  lifted and backlog compression is immediate.
- Anchors preserved by construction: `0:0` always hot; `100:100`
  effectively never.
- Validation: the 2G/2G triangle rerun survives at every HIGH; the
  384M cliff torture matrix shows no regression.

## Non-Goals

- No per-mount RAM accounting: the signal is global; the option stays
  per-mount.
- No shrinker/reclaim integration change (the shrinker remains one of
  the GC's async drivers, untouched).
- No change to `compress=`, `compress_threshold=`, `merge_pages=`, or
  cell geometry options.  (`gc_percent` was requested per mount during
  review and is included below.)
- No guaranteed LRU-ordered backlog walk (see Open Questions).

## Design

### The signal

```c
/* Percent of RAM that is NOT available.  si_mem_available() is
 * exported (include/linux/mm.h) and accounts free pages and evictable
 * file cache -- the same number sysinfo(2) shows as "available". */
static unsigned int shmem_comp_ram_usage_pct(void)
{
	long avail = si_mem_available();
	long total = (long)totalram_pages();

	if (total <= 0)
		return 0;
	if (avail < 0)
		avail = 0;
	return (unsigned int)clamp((total - avail) * 100 / total, 0L, 100L);
}
```

Syntax and rendering of `compress_watermark=LOW:HIGH` are unchanged;
the percentages now read against RAM.  The default (50:70) re-reads as
"flip at 70% RAM used, release at 50%" -- sane for every box size.

### Phase 1: the write-path flip (sync path)

The `shmem_write_end()` call site is unchanged; the decision function
gains the flip:

```c
bool shmem_comp_should_compress(struct inode *inode)
{
	struct shmem_comp_sb *comp =
		shmem_comp_get_sb(inode->i_sb)->comp;

	if (!comp || !comp->enabled || !comp->pool)
		return false;
	shmem_comp_wm_update(comp);	/* flip, both directions */
	return READ_ONCE(comp->hot) &&
		i_size_read(inode) >= comp->threshold_bytes;
}

static void shmem_comp_wm_update(struct shmem_comp_sb *comp)
{
	unsigned int usage;
	bool hot = READ_ONCE(comp->hot);

	if (comp->watermark_high == 0) {		/* eager 0:0 */
		if (!hot)
			WRITE_ONCE(comp->hot, true);
		return;
	}
	usage = shmem_comp_ram_usage_pct();
	if (usage >= comp->watermark_high)
		hot = true;
	else if (usage <= comp->watermark_low)
		hot = false;
	else
		return;				/* hysteresis band: hold */

	if (hot == READ_ONCE(comp->hot))
		return;
	WRITE_ONCE(comp->hot, hot);
	pr_info("tmpfs: mount %s compression %s (ram usage %u%%)\n",
		comp->sb->s_id, hot ? "hot" : "chill", usage);
	if (hot)
		schedule_work(&comp->maint_work);	/* highwater kick */
	else
		comp->wm_kick_spent = false;	/* LOW resets the kick */
}
```

The highwater kick fires once per flip (`wm_kick_spent` guards it);
the return to chill resets it.  The 1 Hz cadence is the always-on
part and is unaffected.

### Phase 2: the unified background worker

The existing `age_work` machinery survives, generalized into
`maint_work` with two trigger sources and two ordered duties:

```c
static void shmem_comp_maint_work(struct work_struct *work)
{
	...
	bool hot = READ_ONCE(comp->hot);	/* mode at pass start */
	/* duty 1: eager compression of chilled residents.
	 *   hot (highwater pressure): compress immediately, no age gate.
	 *   chill (calm): compress only folios seen in a previous pass.
	 *   auto-skips when no candidates exist. */
	shmem_comp_maintain(comp, hot /* immediate? */, ...);
	/* duty 2: evacuation GC, the existing ~4:1 interleave */
	if (comp->pool->use_cells)
		shmem_comp_cell_gc(comp->pool);
	...
}
```

| Trigger | When | Effect |
|---|---|---|
| 1 Hz cadence | existing arming: `write_end` arms on a non-compressing write; the pass re-arms while its budget was exhausted | calm-mode maintenance at today's cadence |
| highwater kick | chill->hot flip, once per flip | immediate full backlog compression under pressure |

The kick's pass re-arms the cadence while the mount is hot, so the GC
interleave stays alive during pressure even though hot-mode writes
(which compress inline) never arm the cadence themselves.

**The worker stays per mount.**  `age_work` was already per-mount
(`INIT_DELAYED_WORK` inside `shmem_comp_fill_sb`); `maint_work` keeps
that.  A single global worker was considered and rejected:

| | per mount (kept) | single global worker |
|---|---|---|
| lifecycle | `put_super` cancels its own work; no cross-mount refs | worker must pin every mount it touches; unmount races multiply |
| idleness | a mount with no writes and no backlog never arms; costs zero | global tick iterates all mounts under the global lock, always |
| isolation | one mount's slow pass cannot stall another | head-of-line blocking; one huge inode list starves the rest |
| kick routing | the flip schedules exactly its own mount's work | flip must queue onto the shared worker with cross-mount locking |

The global mount list (`shmem_comp_mounts`) stays event-driven only
(`gc_now`'s `gc_all`, the shrinker), as today.

**The age gate.**  `shmem_comp_age_round()` today compresses every
plain folio it finds via `find_get_entries()`.  The gate adds a
per-folio "seen" marker (one spare `PG_*` folio flag, test-and-set):

- calm pass: an unmarked candidate is marked and skipped; a marked
  candidate is compressed (marker dies with the folio).
- pressure pass (mount hot): compress unconditionally; marking is
  skipped.

Effective minimum age in calm mode = one pass interval (>= 1 s),
which covers exactly the ripple window (write -> bg compress -> read
-> decompress) without per-page timestamps.

**Pass structure and bounds are unchanged**: up to
`SHMEM_COMP_AGE_MAX_ROUNDS` rounds of `SHMEM_COMP_AGE_BUDGET` pages,
`cond_resched()` per batch, `down_read_trylock(&sb->s_umount)` +
inode `__iget` pinning, cancellation at `put_super`
(`cancel_delayed_work_sync` as today).  Auto-skip: duty 1 ends early
when `shmem_comp_maintain` finds no candidates ("end of
not-yet-compressed"); duty 2 ends early when the sparse list is empty.

### gc_percent (per mount)

The 50% `CELL_IS_SPARSE` classifier becomes a per-mount knob.  It is
cheap because the pool is per mount (`comp->pool =
shmem_comp_pool_create(...)` in `shmem_comp_fill_sb`) and
`cell_free()` already receives the pool:

- Mount option: `gc_percent=N` (byte-utilization percent; default 50
  = today's behavior exactly; clamped 10..90 so both extremes stay
  meaningful).
- `CELL_IS_SPARSE` becomes a threshold-taking inline:
  `cell_is_sparse(cp, live_bytes)` =
  `live_bytes * 100 <= (1UL << cp->cell_order << PAGE_SHIFT) *
  cp->gc_percent`.
- Surface: printed in the seq_show mount line and as a `cell_stats`
  gauge; parsed with the other cell options; out-of-range values
  refused at mount like other options.
- (Fold-in: fix the stale "below SPARSE_THRESHOLD objects" comment at
  `include/linux/cell_store.h:175` -- the classifier is byte
  utilization, not object count.)
- Semantics: a cell becomes a GC candidate below N% byte utilization,
  evaluated on free() as today; the async flush drivers are unchanged.
- Note the free path reads `cp->gc_percent` from the same cache line
  it already touches; no measurable cost.

### The taxonomy, final

| Access path | When | What |
|---|---|---|
| direct (sync) | every write, hot mode | `shmem_compress_folio()` inline |
| unified bg worker, calm | 1 Hz cadence, chill | age-gated backlog compression + evacuation GC |
| unified bg worker, pressure | highwater kick (then hot cadence) | immediate backlog compression + evacuation GC |

GC's other drivers -- shrinker, store-exhaustion retry, `gc_now` --
are untouched.

### State table

| RAM usage | mode | new writes | bg worker |
|---|---|---|---|
| crosses HIGH upward | chill -> hot | sync compress | immediate kick; backlog compressed without age gate |
| above HIGH, hot | hot | sync compress | cadence continues; duty 1 auto-skips once backlog empty |
| in band, was hot | hot (holds) | sync compress | as above |
| in band, was chill | chill (holds) | land uncompressed | 1 Hz, age-gated |
| crosses LOW downward | hot -> chill | land uncompressed | 1 Hz, age-gated; kick trigger reset |
| `HIGH == 0` (eager) | hot always | sync compress | duty 1 always auto-skips (no chilled residents) |

## Build Changes

| Target | Change |
|---|---|
| `mm/shmem_compress.c` | write-path flip (`shmem_comp_wm_update`, RAM signal); rename/generalize `age_work` -> `maint_work` with the highwater kick; age gate in `shmem_comp_age_round` (seen marker); duty ordering |
| `include/linux/shmem_compress.h` | `struct shmem_comp_sb`: add `wm_kick_spent`; spare folio flag for the seen marker (or reuse per-folio storage if cheaper); `gc_percent` option parse + seq_show |
| `mm/cell_store.c` | `gc_percent` threshold in the sparse classifier; `cell_stats` gauge |
| `include/linux/cell_store.h` | pool `gc_percent` field; `CELL_IS_SPARSE` -> threshold inline; stale comment fix (L175) |
| `mm/shmem.c` | no change (call site already routes through `should_compress`) |

## File Manifest

| File | Action | Purpose |
|---|---|---|
| `specs/06_WATERMARK_RAM_TWO_PHASE.md` | Create | This spec |
| `mm/shmem_compress.c` | Modify | Phase 1 + unified worker + age gate |
| `include/linux/shmem_compress.h` | Modify | State fields, folio marker |
| `mm/cell_store.c` | Modify | gc_percent in the sparse classifier + gauge |
| `include/linux/cell_store.h` | Modify | Pool field, threshold inline, stale comment fix |

## Migration

### Phase A: phase 1 (write-path flip, RAM signal) -- est. half a day

1. Add `shmem_comp_ram_usage_pct()` + `shmem_comp_wm_update()`; wire
   into `shmem_comp_should_compress()`.  The existing periodic pass
   stays untouched and remains harmless (its flip becomes redundant
   but identical in effect).
2. Gate: KASAN suite green; triangle rerun shows survival extending
   past `0:0` (predicted boundary HIGH ~60-80 by the
   backlog+compressed-remainder budget).

### Phase B: unified worker + age gate -- est. half a day

1. Generalize `age_work` into `maint_work`; add the highwater kick in
   `shmem_comp_wm_update`; implement duty ordering and auto-skip; add
   the seen-marker age gate to `shmem_comp_age_round`.
2. Gate: KASAN suite green; 384M cliff torture matrix no regression;
   logs show a kick immediately at flips and the backlog reaching
   zero; a calm-mode log shows young pages skipped at least once.

### Phase C: validation -- est. half a day

1. Full 21-combo triangle rerun at 2G/2G + the torture policy matrix.
2. Results and envelope update as a mission log.

### Phase D: gc_percent (per mount) -- est. 2 hours

1. Pool field + option parse + clamp (10..90, default 50), threshold
   inline replacing `CELL_IS_SPARSE`, seq_show + `cell_stats` gauge,
   stale comment fix.
2. Gate: KASAN suite green; default (50) shows byte-identical GC
   behavior to today; a gc_percent=20 mount evicts at a visibly
   different density in `cell_stats`.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `si_mem_available()` cost on the write path | Med | Med | bounded counter reads; throttle to every Nth write behind a constant if a bench regresses |
| Age gate delays calm compression by >= 1 pass (RAM relief slower in chill) | Low | Low | chill is by definition the no-pressure state; the highwater kick lifts the gate the moment pressure appears |
| Seen marker needs a spare folio flag | Med | Low | fall back to a second observation via xarray iteration order; isolated helper either way |
| Global signal: all compressed mounts flip together | Low | Low | per-mount hysteresis independent; documented |
| Kick + cadence contend on the same worker (schedule_work while running) | Low | Low | `schedule_work` on a running work is a no-op; the running pass picks up the mode change via `READ_ONCE(comp->hot)` |
| One-shot semantics from the earlier iteration are gone: worker runs at 1 Hz forever | Low | Low | that is today's behavior, which the user explicitly wants kept |
| gc_percent misconfig (low -> frequent evacuation copy cost; high -> exhaustion-path GC only) | Low | Low | clamped 10..90; default 50 is exactly today's behavior |

## Open Questions

- Calm-mode age: "seen once" (= >= 1 pass) or "seen N times" (= >= N
  passes)?  v1: once; raise only if the workload bed shows ripple.
- Drain stop at exactly LOW vs a small dead band above it: v1 exactly
  LOW (hysteresis band already provides damping).
- LRU-ordered backlog walk to minimize decompress-on-read for the
  most-recently-used pages: deferred until spec 05's profiles measure
  a read penalty.
- `skipped_dead` accounting: keep the periodic-pass accounting as is
  (it survives with the pass) -- no change needed.

## References

- [specs/02_ZTMPFS_TRANSPARENT_COMPRESSION.md] -- the feature and its
  Stage-2 origin
- [specs/04_GC_REVERSE_INDEX.md] -- evacuation GC (duty 2, unchanged),
  `CELL_IS_SPARSE` 50% classifier
- [specs/05_WORKLOAD_TESTBED.md] -- churn profiles for validation
- [mission_log/2026-10-07_deferred-compression-policy.md] -- original
  Stage-2 policy (mount-usage signal, periodic pass)
- [mission_log/2026-10-13_zcell-watermark-triangle.md] -- the failure
  this spec fixes (eager-only survival, flip at 92%)

---
name: compression-policy-option-surface
description: option surface v2 and the compression policy model -- the compress_watermark design, Stage 1 landing, and the refs-underflow/ABA fix (sessions 8-10)
project: linux
---

# Mission Log: Option Surface v2, Stage 1, and the Underflow Fix

Part 5 of the tmpfs transparent-compression series, split verbatim from the
combined log `2026-10-05_tmpfs-transparent-compression.md` (kept until the
split is validated). Session numbers are preserved so cross-references keep
resolving.

Covers sessions 8-10 (2026-10-07): the three-layer option model and
compress_watermark=LOW:HIGH design (session 8, spec only), the Stage 1
landing of the option surface with the 100:100 never-sentinel and
show_options visibility (session 9), and the refs-underflow fix -- the
phantom put plus the {32-bit ID, 23-bit generation} ABA encoding -- with the
panic-on-corruption assert layer (session 10).

- Previous: `2026-10-07_torture-sweep-farm-benchmarks.md`

## Session 8: option surface v2 and the compression policy model (2026-10-07)

### What We Did

Design session (no kernel code; the spec landed as commit
`49879bd0ddcb`, "docs: plan -- option surface v2 + compression policy
v2"). Worked through the option surface that had grown organically
across sessions, and the deferred-compression policy that started as
"think about writeback" and sharpened into death-before-pop aging.

**The organic-growth audit.** Enumerated the full surface: 7 mount
option spellings (two of them pure aliases: compress=/compressor=,
compress_merge=/merge_pages=), 6 sysfs knobs, 1 Kconfig string, plus
silently-derived state (cell_order from tmpfs size, +1 when
merge>=8). The grates, named honestly:

1. Three "off" spellings that were not quite synonyms (nocompress,
   compress=none, sysfs compressor=none) -- a fourth
   (compress_ratio=100:100) was proposed and would have made it worse.
2. compress_threshold=64K default silently disables compression for
   small-file farm workloads -- the torture suite had to pass
   threshold=1 to see ANY compression.
3. Unit soup: cell= takes memparse syntax, compress_threshold= raw
   bytes, merge_pages a count, ratio percent.
4. /proc/mounts shows nothing about compression -- the cell256k
   torture bug (a mount benchmarking as plain tmpfs) was invisible
   exactly there.
5. cell= conflates storage backend with geometry, and geometry
   silently depends on ANOTHER option (merge) and on mount size.
6. Naming landmine: "compress_ratio" collides with compression ratio
   in comp_stats (35.00%). Rejected in favor of compress_watermark,
   which also reads correctly to anyone who knows VM watermarks.

**The coherent model -- three layers, one concept per knob, no
aliases** (user decision: aliases dropped; this is a project branch,
upstream can re-add spellings at posting time):

- Layer 1 WHAT: compress=<algo> (none = off) | nocompress. All "off"
  spellings normalize to one resolved state: no pool.
- Layer 2 WHEN: compress_watermark=LOW:HIGH. Sentinels: 0:0 always
  (eager), 100:100 never (= nocompress, no pool), 50:100 pure
  deferral (maintenance-only aging), 50:70 default target.
- Layer 3 GEOMETRY: merge_pages (documented large-file knob),
  cell=SIZE|0, compress_threshold now memparse AND semantically
  write-time-only (the aging pass compresses any eligible survivor
  >= 1 page -- small survivors are worth their RAM; this single
  clarification dissolves grate #2).
- Global sysfs mirrors the defaults (compressor, compression bool,
  merge_pages, new compress_watermark). Deliberately internal, no
  knobs: cell_order derivation, GC/maintenance batch sizes, per-CPU
  contexts, entry table.

**The policy -- death-before-pop aging** (user's core idea):
transient files are the point of tmpfs, so compressing them at write
time spends CPU on data that dies unread. Writes land uncompressed
in chill mode (cheap candidate marker: list_add under a spinlock);
unlink/truncate purges the marker; hot mode (usage >= HIGH) compresses
inline as today; one maintenance pass per mount (shrinker, gc_now,
periodic delayed_work) MANDATORILY compresses eligible survivors and
then evacuates sparse cells, budgets alternating ~4:1. Compression
releases size= accounting (compressed entries are accounted like swap
pages; shmem_recalc_inode unaccounts blocks), so hot-mode compression
drops usage below the mark -- the70% high-water is a soft limit, and
effective capacity for compressible data is size=/ratio. Mode flips
happen in the maintenance pass only: the write path cost is one
READ_ONCE(comp->hot), never a percpu_counter compare.

**The per-mount vs global-defaults analysis** (the question this log
entry exists to preserve):

- Precedence, per option, resolved ONCE at mount:
  `mount option > global sysfs default > built-in default`.
  shmem_comp_opts_init() seeds opts from the current sysfs values
  (copied out under shmem_comp_defaults_lock, never pointers -- the
  session-2 UAF fix); parse_one overrides what the mount passed;
  fill_sb resolves the rest and freezes the outcome into
  sbinfo->comp. After fill_super the sysfs globals are never read on
  any hot path. Sysfs changes affect only FUTURE mounts; remount
  cannot change compression state at all (SHMEM_SEEN_COMPRESS guard)
  -- policy is mount-lifetime by design.

- Resolution matrix (the intended division of labor):
  - `mount -t tmpfs /a` with sysfs compression=1 compressor=zstd
    watermark=50:70 -> zstd, 50:70 (all defaults).
  - `-o compress=lz4` -> lz4 (mount wins), 50:70 (default).
  - `-o compress=lz4,compress_watermark=0:0` -> lz4 eager (mount
    wins both).
  - `-o nocompress` -> uncompressed (mount beats compression=1).
  - plain mount with sysfs compression=0 -> uncompressed.
  - `echo zstd > sysfs/compressor` afterwards -> affects only
    future default-following mounts; the mounted ones are frozen.
  The sysfs default is how an operator says "everything on this box
  ages at 50:70 unless a mount opts out"; individual
  latency-sensitive mounts pin 0:0 or 50:100.

- **The subtlety that would have bitten: unset != explicit zero.**
  compress_watermark=0:0 ("always") is a valid explicit value that
  collides with zero-init ("not specified"). This is exactly why
  shmem_comp_opts already carries cell_seen (cell=0 opt-out vs not
  given). Watermark needs bool watermark_seen in the opts struct;
  fill_sb takes the sysfs default only when !seen. Without it a mount
  asking for 0:0 would silently receive the global default -- the
  same bug class as cell256k.

- Visibility is the operational half of coping with per-mount
  config: the resolved-state line at mount prints the full outcome
  AND why a mount is uncompressed ((nocompress) / (compress=none) /
  (compress_watermark=100:100)); show_options prints the effective
  per-mount config into /proc/mounts (compress=zstd,
  compress_watermark=50:70,merge_pages=8) so any mount's resolved
  state is auditable after the fact.

- Layer 3 deliberately has NO global default: cell= derives from the
  mount's own size= and threshold is workload-shaped -- per-mount by
  nature. Global knobs for them would be additive later if a use
  case appears.

### What We Learned

- Option surfaces accrete ABI faster than they accrete meaning;
  an audit pass ("list every knob, unit, alias, and who reads it")
  is cheap and finds the dead weight (aliases, unit soup, invisible
  state) before it becomes upstream ABI.
- The cell256k torture bug was not just a test bug -- it was an
  option-model bug (participation following a default the operator
  couldn't see). Fixing the model (resolved-state print +
  show_options + watermark_seen) is what actually prevents the class.
- Naming: never introduce a knob name that collides with an existing
  metric name in the same feature ("ratio").
- Policy knobs want sentinel values that map onto the existing
  vocabulary (100:100 == nocompress, no pool) rather than a parallel
  enum -- one mental model, no fourth way to say "off".

### What We Decided

- No aliases on mount options (project branch; upstream re-adds at
  posting if desired). compress_watermark over policy-enum+band.
- Stage 1 (next code commit): option surface v2 end-to-end with the
  built-in default watermark 0:0 (eager = today's behavior), so the
  surface is real before the machinery changes behavior; the never
  sentinel works immediately (no pool).
- Stage 2: marker list + maintenance pass + hot/cold flips; THEN the
  default flips to 50:70 and the torture sweep becomes the policy
  matrix (never / 0:0 / 50:70 / 50:100 configs, predicting T=1
  toward plain 26 MB/s vs today's 17.1, skipped_dead absorbing
  transient churn, survivor ratio ~48% unchanged, bounded Shmem
  overshoot).
- compress_threshold semantics split: write-time-only knob; aging
  compresses survivors of any size >= 1 page.

### Files Changed

| File | Purpose |
|------|---------|
| `TMPFS_COMPRESSION_PLAN.md` | "Option Surface v2" + "Compression policy v2" sections (commit 49879bd0ddcb) |

### Test Results

None this session (design only). Stage 1 acceptance: build clean,
rig suite green with aliases removed (test_mount_sem updated to
canonical spellings), watermark sentinel mounts resolving as
specified, show_options lines visible in /proc/mounts.

### Next Steps

1. Stage 1 code: drop aliases from shmem_fs_parameters, memparse for
   compress_threshold, compress_watermark end-to-end (parse ->
   opts + watermark_seen -> fill_sb sentinel handling -> sb fields ->
   show_options helper -> sysfs default knob), built-in default 0:0,
   resolved-state mount line including why-off. Update rig
   test_mount_sem; add a 100:100 sentinel case.
2. Stage 2: marker list + unlink/truncate purge + shmem_comp_maintain
   (survivors mandatory, cells GC, 4:1 budget, hysteresis flips) +
   shrinker/periodic triggers; flip default to 50:70; torture policy
   matrix A/B.
3. Keep the torture sweep as the standing regression gate for all of
   it (TEST_TORTURE=1 QEMU_CPUS=4).

---

## Session 9: Stage 1 -- option surface v2 lands end-to-end (2026-10-07)

### What We Did

Implemented Stage 1 of the option surface v2 plan from session 8: the
watermark knob and coherent option model land end-to-end with the
built-in default 0:0 (eager = today's behavior), so the surface is
real before the machinery changes behavior. Four bisectable kernel
commits, rig suite green, plus one regression hunt that ended in a
surprise verdict.

**Commit sequence (each build-clean, rig-checked):**

1. `a222524` -- drop the option aliases: `compressor=` (alias of
   `compress=`) and `compress_merge=` (alias of `merge_pages=`) leave
   `shmem_fs_parameters[]`. One spelling per concept; the sysfs
   default knobs keep their own names (they are global defaults, not
   mount-option aliases). Rig tests updated to canonical spellings
   (`test_mount_sem.c`, `test_multimount.c` -- both live outside the
   kernel git).
2. `8e03528` -- `compress_threshold=` takes memparse units (`64K`),
   matching `cell=`; unit soup gone. The semantics split is documented
   where it lives: the knob is write-time-only; the Stage 2 aging pass
   compresses any eligible survivor >= 1 page.
3. `7f16e14` -- `compress_watermark=LOW:HIGH` end-to-end, mount side:
   parse (`shmem_comp_opts_set_watermark()`, low <= high enforced) ->
   opts + `watermark_seen` -> fill_sb resolution (mount > sysfs >
   built-in 0:0) -> `shmem_comp_sb` fields -> resolved-state mount
   line. The 100:100 never-sentinel resolves to no pool, no memory
   overhead -- identical resolved state to nocompress. All "off"
   spellings now normalize to one state, with a WHY in the mount line
   (`nocompress` / `compress=none` / `compress_watermark=100:100` /
   `not participating (default off)`) via `enum shmem_comp_off_reason`.
4. `275a232` -- the visibility half: sysfs `compress_watermark`
   default knob (reuses the mount-option parser -- one grammar, one
   validator, sentinel semantics cannot drift) and
   `shmem_comp_show_options()`: /proc/mounts tells the truth
   (`,nocompress` for uncompressed mounts;
   `compress=<algo>,compress_watermark=LOW:HIGH[,merge_pages=N]` for
   compressed ones; cell_order stays deliberately internal). Plus a
   real pre-existing incoherence fixed on the way: sysfs
   `compressor=none` used to be stored as NULL, which silently fell
   back to the Kconfig default algo -- "none" is now stored as-is, so
   default-following mounts resolve algo none and run uncompressed
   (Layer 1 normalization: one resolved state, no pool).

**The unset-vs-explicit-zero collision** (session 8's predicted
subtlety, confirmed in code): `compress_watermark=0:0` is a valid
explicit value that collides with zero-init. `watermark_seen` in the
opts struct makes fill_sb take the sysfs default only when the mount
did not ask -- same bug class as `cell=0` vs not-given
(`cell_seen`). Rig-proven: with sysfs at 50:70, a mount passing
`compress_watermark=0:0` shows `compress_watermark=0:0` in
/proc/mounts and compresses eagerly; a default-following mount shows
`50:70`.

**Rig work:** test_mount_sem rewritten (10 cases: canonical
spellings, 100:100 never-sentinel, explicit 0:0 vs sysfs default
collision, memparse threshold, /proc/mounts truth x2, remount
rejects incl. compress_watermark, sysfs default). A new isolation
gate landed: `TEST_ENTRYRACE=1` -> `test.entryrace=1` in
run_qemu.sh + initrd/init (mirrors the torture/cellzram gates) --
needed for the investigation below.

**The regression hunt (the interesting part):** the full-suite boot
on the Stage-1 kernel showed `read errors: 2` in the entry-lifetime
race test plus ~158 `shmem_comp: entry N refs underflow` WARNs from
`shmem_decompress_folio() -> shmem_comp_entry_release()` -- zero
occurrences in ~50 prior serial logs. The diff provably did not touch
the entry/data path, but "provably harmless" is not a verdict, so:
added the isolation gate, reproduced deterministically on the
Stage-1 kernel (~170 WARNs/boot, 3/3 boots), then built the
PRE-change kernel (`e3a1861`) in a throwaway `git worktree` (ccache
made it ~2.5 min) and ran the same gate: **~173-177 WARNs/boot,
identical read-error shape.** Verdict: not a regression from this
session's commits -- a pre-existing latent entry-lifetime race that
the isolated gate exposes deterministically. Full-suite boots on old
kernels were timing-marginal clean; a fresh-boot gate hammers the
race immediately. Both worktrees/images cleaned up; rig restored to
the Stage-1 kernel.

### What We Learned

- **A new isolation gate must be A/B'd against the pre-change kernel
  before declaring a regression.** The gate itself changes timing
  (fresh boot, immediate hammering) versus full-suite context; a
  symptom that appears only under the new gate may predate everything
  you changed. The worktree+ccache baseline build cost ~3 minutes and
  settled a question that code reading could not.
- Mechanism hypothesis for the underflow (recorded for the future
  investigation): the compression entry xarray encodes the entry ID
  only, with no generation counter. A reader that samples the slot
  value, loses the race to truncate (entry freed, ID recycled by a
  rewrite), then pins the recycled ID, decompresses the WRONG entry's
  data (userspace sees bad bytes -- the "read errors"), and its
  `entry_release()` is applied to the new entry -- dropping a slot
  reference it never owned. Later teardown of the real owner
  underflows. This is the P1 #5 class the entry-race test hunts; the
  get/put pinning fixed the UAF, but the stale-value + ID-reuse hole
  remains. A generation field in the entry encoding (or rejecting
  stale values at get time) is the likely fix -- separate work item.
- `test_entry_race` has an accounting bug: `run_phase()` resets the
  error counters per phase and `failures = read_errors + write_errors`
  is computed after the LAST phase only, so `Race test failures: 0`
  can print (and exit 0) while Phase A had read errors. The WARN
  storm in dmesg is the real verdict -- which the test's own comment
  already says ("KASAN log is authoritative"), but the exit code
  disagrees with the printed FAIL line, which is worse than useless in
  automation.
- Pre-existing incoherences surface while implementing adjacent code:
  the sysfs `compressor=none` -> NULL -> Kconfig-fallback bug was
  invisible until the "all off spellings normalize to one resolved
  state" rule gave a crisp definition of what "none" must mean.
- Mutex discipline: `shmem_comp_opts_init()` was reading the sysfs
  default `enabled` flag without `shmem_comp_defaults_lock`; cheap to
  fix while touching the function, but it took the session-8 decision
  ("copied out under the lock") to even look.

### What We Decided

- Stage 1 = surface only, default 0:0 eager. The write path does not
  yet consult the watermark (except the never-sentinel, which removes
  the pool). Stage 2 flips the machinery and the default (50:70).
- show_options prints `compress_watermark` always (when compression
  is on) and `merge_pages` only when non-default; cell_order stays
  internal -- observable at mount, free to change, not /proc/mounts
  ABI.
- The entry-ID-reuse race is a KNOWN-OPEN issue, pre-existing,
  reproducible: recipe `TEST_ENTRYRACE=1 QEMU_CPUS=4` (~170 underflow
  WARNs per boot on both pre- and post-Stage-1 kernels). It does not
  block Stage 2 but must be fixed before any upstream posting.
- Test-rig rule (below) about gate A/B.

### Rules Added

- **Gate A/B rule:** when a newly added isolation gate (or any new
  test configuration) surfaces a failure, run the SAME gate on a
  kernel built from the pre-change commit before investigating your
  own diff. Only an A/B difference is a regression attributable to the
  change under review.
- **Counter-survival rule:** when a test resets counters per phase,
  the final failure sum must aggregate all phases, not the last one.
  Check this before trusting a test's exit code in automation.
- **One grammar, one validator:** sysfs default knobs that mirror
  mount options must reuse the mount option's parser
  (`shmem_comp_opts_set_watermark()` case in point) so semantics
  cannot drift between entry points.

### What We'd Do Differently

- Built the pre-change baseline FIRST, before re-running the Stage-1
  kernel three times. The reproduction was never in doubt (3/3
  already); the only open question was attribution, and the baseline
  answers that directly. One boot each, then conclude.
- The full-suite log review initially mis-counted "read errors"
  (pattern `read errors: [02]` matched both values); a targeted
  `grep -c "read errors: 2"` per log would have established the
  "only today" fact in one command instead of a misleading table.

### Files Changed

| File | Purpose |
|------|---------|
| `mm/shmem.c` | aliases dropped; compress_watermark enum/table/parse; threshold memparse; nocompress off_reason; show_options hook |
| `mm/shmem_compress.c` | watermark parse helper + defaults global/getter; fill_sb watermark resolution + never sentinel + why-off mount lines; sysfs compress_watermark attr; compressor=none normalization; shmem_comp_show_options(); opts_init under lock |
| `include/linux/shmem_compress.h` | opts struct: watermark_{low,high}, watermark_seen, off_reason; `enum shmem_comp_off_reason`; set_watermark + show_options decls; seq_file fwd decl |
| rig `tests/test_mount_sem.c` | 10-case rewrite: canonical spellings, sentinel, seen-collision, memparse, /proc/mounts truth, remount rejects |
| rig `tests/test_multimount.c` | canonical spellings |
| rig `scripts/run_qemu.sh` | TEST_ENTRYRACE=1 -> test.entryrace=1 |
| rig `initrd/init` | test.entryrace=1 isolation gate |
| `TMPFS_COMPRESSION_PLAN.md` | Stage 1 status + known-open note |

### Test Results / Benchmarks

Full suite, Stage-1 kernel #63 (KASAN, QEMU_CPUS=4,
results/serial_20261007_161445.log):

- `Test exit code: 0`; seq sweeps, mmap coherency (0 failures),
  size limit, cell storage (ALL PASS), cell GC (ALL PASS), bench
  matrix, multimount all green.
- `test_mount_sem: ALL PASS` -- all 10 cases, including the new
  sentinel/collision/memparse/proc-mounts cases. dmesg shows the new
  resolved-state lines doing their job: `tmpfs: compression on,
  algo=lz4 merge_pages=1 threshold=1 cell_order=8 watermark=0:0`,
  `tmpfs: compression off (compress=none)`, `tmpfs: compression off
  (compress_watermark=100:100)`.
- Entry-race anomaly (explained above): Phase A `read errors: 2`,
  ~158 `refs underflow` WARNs. Isolated gate on Stage-1 kernel #63:
  180/169/167 WARNs over 3 boots. Isolated gate on pre-change kernel
  `e3a1861`: 173/177/174 WARNs over 3 boots. Same class, both
  kernels -- pre-existing.

### Next Steps

1. Stage 2 machinery: candidate marker list (list_add under
   spinlock, unlink/truncate purge = skipped_dead),
   `shmem_comp_maintain()` (survivors mandatory >= 1 page, sparse
   cell evacuation, ~4:1 alternating budgets, hysteresis mode flips),
   triggers (shrinker, gc_now, periodic delayed_work); flip built-in
   default to 50:70; torture policy matrix A/B (never / 0:0 / 50:70 /
   50:100 vs today's eager numbers).
2. Entry-ID-reuse race (known-open, pre-existing): generation
   counter in the entry encoding or stale-value rejection at
   entry_get; fix `test_entry_race` phase-counter aggregation first
   so the gate's exit code means something.
3. Standing gates: full suite per commit (`run_tests.sh`,
   QEMU_CPUS=4); entry-race isolation gate for entry work
   (`TEST_ENTRYRACE=1`); torture sweep for Stage 2 policy work
   (`TEST_TORTURE=1`).

---

## Session 10: the refs-underflow fix, and a zero-page hunt that wasn't (2026-10-07)

### What We Did

Fixed the known-open `refs underflow` race from session 9, added the
panic-on-corruption assert layer (the user's standing rule: "any data
corruption shall be a kernel panic"), and then spent the bulk of the
session chasing a second symptom -- mid-race zero pages -- that turned
out to NOT be a kernel bug at all.

**The underflow fix (commit `22ab9e4`).** Two defects, one storm
(~170 WARNs per isolated-gate boot, reproduced on pre- and
post-Stage-1 kernels):

1. *Phantom put* in the cell-backed branch of
   `shmem_decompress_group()`: that path took NO entry pin
   (`shmem_comp_load_entry()` re-pins internally) but dropped one at
   the end -- refs corruption on every cell+merge group decompress
   whose ID got recycled. The group path now takes ONE
   generation-validated pin at entry; every error path and the final
   put balance it.
2. *ABA in the xarray value*: the value encoded the entry ID only, so
   a stale value (truncate freed the entry, rewrite recycled the ID)
   was indistinguishable from the new incarnation -- the pin landed
   on the wrong entry (torn reads) and its slot release was charged
   to an entry the stale reader never owned. Entries now carry a
   23-bit generation packed beside the 32-bit ID in the 55-bit swp
   offset field (`SHMEM_COMP_ENTRY_GEN_{BITS,MASK}` in swapops.h);
   `shmem_comp_entry_get_gen()`/`shmem_comp_entry_release(id, gen)`
   validate it and reject stale values (-ENOENT, caller retries),
   counted in `stale_value_retries` (new comp_stats line).

Bit budget: swp offset is 55 bits on 64-bit targets (64 -
SWP_OFFSET_SHIFT + SWP_TYPE_BITS = 64 - 14 + 5). 32-bit ID (xa_alloc
range unchanged) + 23-bit generation = exactly 55. Generation wrap at
2^23 is harmless: ABA requires the same ID recycled exactly 2^23
allocs later, inside a sub-microsecond load-to-pin window. Bare-ID
`shmem_comp_entry_get()` stays for cellzram (table-internal blocks,
pinned by their own table references, never in xarray values).
cellzram.c's `entry_alloc` call passes NULL for the optional gen
out-param.

Gate result: underflow 180/169/167/173 WARNs per boot -> **0/0/0/0**.

**The panic layer (commit `d0cc4d2`).** Corruption stops the machine:
refs underflow (put/release) -> panic; slots underflow (release,
double slot release) -> panic; `VM_BUG_ON(refs != 0)` in
`entry_free()` (double teardown, debug kernels); decompressed-length
mismatch on single-page AND group paths -> panic (a successful crypto
decompress must produce exactly the stored page count; `req->dlen` is
the crypto-reported actual length, so the check is live). Staleness
(generation mismatch) deliberately stays a counted, retried
condition -- stale values are the NORMAL outcome of truncate+rewrite
churn, and retry is correct behavior. Warning-free on all green runs:
if any of these ever fires, the in-memory state was already wrong.

**The zero-page hunt (the interesting part).** Post-fix gate boots
still showed 1-11 "read errors" per phase. Investigation sequence:

1. *Instrumented the test first-class*: split counters (pread syscall
   errors vs bad-byte reads), hex dumps of the first bad bytes, added
   `cell=0` (zsmalloc) phases A2/B2, and -- per session 9's own rule
   -- fixed the phase-counter aggregation bug (verdict now sums all
   phases; the suite honestly reported exit 1 while this was open).
2. *Characterized the symptom*: exactly ONE page (bad=4096) of ZEROS
   at a page-aligned offset, inside full-length reads (r=131072),
   on BOTH cell and zsmalloc backends. No KASAN, no WARNs.
3. *Kernel probes* (temporary, since removed): hole detector in
   `shmem_get_folio_gfp`'s SGP_READ miss branch (+ dump_stack for the
   first 5), removal tracers in both `shmem_undo_range` loops, an
   anomaly detector in `shmem_comp_free_swap`. Results: hole probe
   FIRED 38-46/boot at idx < i_size -- including idx=0 with
   i_size=131072 (complete file!); stacks showed only
   `shmem_file_read_iter -> shmem_get_folio -> shmem_get_folio_gfp`;
   every undo removal showed i_size=0 (legit truncates); free_swap
   anomaly: 0 hits.
4. *Code audit of every slot-clearing site* in shmem.c
   (`xas_store(NULL)`, `filemap_remove_folio`): shmem_free_swap
   (probed, clean), undo_range loops (probed, i_size=0),
   get_folio_gfp's SGP_CACHE/falloc error path (SGP_READ never
   allocates), userfaultfd mfill (unused), shmem_alloc_and_add_folio
   ENOSPC path (no ENOSPC at 128K in 1G), shmem_writeout (inactive).
   All accounted for -- yet the holes were real at read time.
5. *Resolution*: NOT a kernel bug. The writer truncates to 0 and
   rewrites in a tight loop; a reader samples i_size (large,
   pre-truncate) at the top of a `shmem_file_read_iter` iteration,
   then hits the slot AFTER truncate cleared it, correctly zero-fills
   that page (POSIX hole semantics within the sampled size), and
   continues -- and the fast rewrite has re-materialized the later
   pages as 'A'. Full-length read, a few zero pages inside. The
   probe's "i_size=131072" print is the POST-REWRITE value sampled
   AFTER the tree read saw the truncation-window hole: two samples at
   different instants, paired coincidentally. (The TSO "reader must
   see i_size=0" argument only orders one reader's two loads against
   the writer's stores; it says nothing about the reader's OWN tree
   read happening earlier than its own i_size read.)
6. *Fixed the verdict model*, not the kernel: mid-race zero pages are
   informational (TOCTOU); the authoritative check is quiescent --
   after the writer stops, write a known pattern (threshold=1
   compresses every page, exercising the full compress->store->read->
   decompress path), read back, verify byte-for-byte. Pread syscall
   errors (errno not ENOENT/EIO) remain hard failures. All probes
   removed; kernel tree back to clean committed state.

Post-fix verdict: gate 3 boots x 4 phases -- exit 0, quiescent verify
PASS everywhere, zero pages informational (2-10 per phase), 0
underflows, 0 panics; full suite exit 0 (mount_sem, cell_storage,
cell_gc ALL PASS).

### What We Learned

- **Corruption verdicts in racy workloads require quiescent
  verification.** The test was counting POSIX-correct truncate-vs-read
  TOCTOU as corruption and crying wolf; the kernel was clean. The
  "original symptom" discipline from session 4 cuts both ways: name
  the symptom precisely, then verify it survives when the race is
  removed.
- **Instrumented probes can pair samples coincidentally.** A probe
  that reads tree state and i_size at two different instants prints a
  combination that never simultaneously existed. When a probe result
  contradicts the memory model, suspect the probe's own sampling
  before suspecting the kernel.
- **KASAN-generic is blind to buddy-backed storage.** Cell storage
  (and any page-allocated buffer) can be used-after-free without a
  KASAN report. A clean KASAN log exonerates slab objects only.
  Noted permanently in the test's header comment.
- **Two real bugs were hiding behind one loud symptom.** The
  underflow WARNs (phantom put + ABA) were genuine and are fixed; the
  "read errors" that shared their test were benign. Fixing the test's
  verdict model and the kernel's accounting were independent acts.
- Test-first instrumentation beats kernel-first: the hex dump
  (zeros, page-aligned, exactly one page) cracked the case in one
  run; the kernel probe round-trips only confirmed what the dump
  already implied.
- `shmem_replace_entry` + `BUG_ON(error)` in
  `shmem_delete_from_page_cache` means a compress swap whose slot had
  already moved is a kernel BUG, not a silent failure -- useful
  negative evidence during the audit (never fired).

### What We Decided

- Encoding: {32-bit ID, 23-bit generation} in the swp offset; wrap
  harmless (argument in the swapops.h comment). xa_alloc range
  unchanged.
- Validation policy: staleness = -ENOENT + retry + stale_value_retries
  counter (normal race); accounting corruption (refs/slots
  underflow, double teardown) and data corruption (decompress length
  mismatch) = panic. The panic layer is the user's standing rule,
  applied at every boundary where wrongness becomes observable.
- test_entry_race verdict = pread sys errors + write errors +
  quiescent verify failures. Mid-race zero pages: informational,
  hex-dumped (bounded).
- Session 9's entry-ID-reuse known-open is CLOSED: root causes found
  and fixed, asserts armed, gate green.

### Rules Added

- **Quiescent-verify rule:** any "data corruption" verdict in a test
  with concurrent mutators must be confirmed by a quiescent
  write-verify after the mutators stop. Mid-race byte counting is
  diagnostic output, never the verdict.
- **Single-instant sampling rule:** kernel probes that print
  correlated state must sample it at one instant (or accept that
  multi-sample prints can show impossible combinations).
- **Panic-not-warn rule (tmpfs compression):** accounting corruption
  (refcount/slot underflow, double teardown) and data corruption
  (decompress length mismatch) panic unconditionally. Race outcomes
  (stale values) retry and count. No WARN middle ground.
- **KASAN scope rule:** a clean KASAN log exonerates slab objects
  only; buddy-backed storage (cell pages, order-0 allocations) needs
  explicit checks or quiescent verification.

### What We'd Do Differently

- Instrumented the TEST (hex dumps, split counters) before the
  kernel. The dump's "zeros, page-aligned, one page" was the whole
  answer; the probe campaign confirmed it at the cost of three
  build/boot rounds.
- The session-9 lesson ("gate A/B before blaming your diff") was
  applied correctly this time and paid off immediately -- baseline
  kernel reproduced the underflow, which redirected the hunt from
  "what did we break" to "what was always broken". Keep doing this.
- Would have added the quiescent verify when the entry-race test was
  FIRST written (session 4 review P1 #5): its verdict model has been
  wrong-shaped since day one; sessions 6-9 inherited the ambiguity.

### Files Changed

| File | Purpose |
|------|---------|
| `include/linux/swapops.h` | {id, gen} packing: make_shmem_comp_entry(id, gen), shmem_comp_entry_gen(), GEN_BITS/MASK, 55-bit budget comment |
| `include/linux/shmem_compress.h` | entry gen field + kerneldoc; get_gen/release decls; alloc gen out-param; stale_value_retries in stats |
| `mm/shmem_compress.c` | gen counter + stale counter; get_common/get/get_gen; gen-validated release; balanced group pin (phantom-put fix); alloc gen out; free_swap/decompress/compress call sites; panic layer (refs/slots underflow, length mismatch); VM_BUG_ON double teardown; stale_value_retries in get_stats + comp_stats_show |
| `mm/cellzram.c` | entry_alloc NULL gen out-param (table-internal blocks) |
| `mm/shmem.c` | net zero (debug probes added then removed) |
| rig `tests/test_entry_race.c` | split counters, hex dumps, cell=0 phases A2/B2, phase aggregation fix, quiescent verify verdict model, TOCTOU documentation |
| rig `initrd/init` | comp_stats dump in entry-race gate |
| `TMPFS_COMPRESSION_PLAN.md` | status: underflow closed, panic policy |

### Test Results / Benchmarks

- Isolated gate (`TEST_ENTRYRACE=1 QEMU_CPUS=4`, KASAN kernel):
  underflow WARNs/boot 180/169/167/173 (pre-fix #63) -> 0/0/0/0
  (post-fix #64) -> 0/0/0 (assert-armed #69, 3 boots). Quiescent
  verify: 12/12 PASS. Zero pages during race: 2-10 per phase,
  informational. `stale_value_retries`: 0 on all post-fix boots (the
  ABA validator is armed; no stale value has reached a pin since).
- Full suite (#69, asserts armed): `Test exit code: 0`;
  test_mount_sem / test_cell_storage / test_cell_gc ALL PASS; 0
  underflows; 0 panics. (Suite previously honest-reported exit 1
  while the race test's verdict model was wrong-shaped -- now green
  for the right reason.)

### Next Steps

1. Stage 2 machinery (the plan's next stage): candidate marker list
   (list_add under spinlock, unlink/truncate purge = skipped_dead),
   `shmem_comp_maintain()` (survivors mandatory >= 1 page, sparse
   cell evacuation, ~4:1 alternating budgets, hysteresis flips),
   triggers (shrinker, gc_now, periodic delayed_work); flip built-in
   default to 50:70; torture policy matrix A/B (never / 0:0 / 50:70 /
   50:100 vs eager numbers).
2. Watch `stale_value_retries` in the torture matrix -- the counter
   is the early-warning instrument for ID-recycling storms that the
   panic layer would otherwise meet with a stopped machine.
3. The panic asserts are only as good as the runs that exercise
   them: keep the entry-race gate (TEST_ENTRYRACE=1) and full suite
   as standing gates for all data-path commits.

---

---
name: 32bit-packing-and-table-map
description: session-restart handoff -- the full compression data-structure map (3 xarrays + the GC reverse index), why union/masking cannot widen an xa slot, and a SECOND silent 32-bit bug in shmem_comp_pack_loc
project: linux
previous: 2026-10-15_i386-compile-gate.md
---

# Mission Log: 32-bit Packing, the Table Map, and a Second Silent Bug

Part 46 of the tmpfs transparent-compression series.  Session
(2026-10-15), continued.  A **session-restart handoff**: the user wants a
fresh session to pick up with the whole subsystem in context, so this log
carries the data-structure map as well as this session's conclusions.

## What We Did

### 1. Established the arch posture

The H618 SBC is the *why* the project exists, but upstream requires the
feature to be correct on every arch, so **arch-correctness gates perf on
any one board**.  The user has real hardware for: H618 (aarch64), an RV64
board, and amd64 + i386 on the workstation.  Everything else is
emulatable: correctness sweeps run under QEMU TCG (any arch, page size,
endian, width); perf only on real silicon.

The i386 compile gate (previous log) found the first 32-bit break.

### 2. The house rule the user set

**In our code use only fixed-width types (`u8`/`u16`/`u32`/`u64`); never
rely on `unsigned long`/`long` for a value with a fixed logical width.**
`unsigned long` is pointer-width, so it is arch-dependent.  (`int` and
`unsigned int` are 32-bit on every Linux arch, so those are not the
hazard; `unsigned long` and `long` are.)

Scope note: `unsigned long` remains correct for genuinely pointer-sized
or size-like things (`size_t`, addresses, `phys_addr_t`) and for matching
kernel API signatures such as `xa_mk_value(unsigned long)`.  The rule is
about **fixed logical data**, not blanket replacement.

### 3. Why a union / masking / zero-extension cannot widen the slot

Asked directly whether the pointer could be zero-extended or unioned into
a wider field: **no**.

- Every `xa_node` stores `void *slots[XA_CHUNK_SIZE]` -- pointer-width for
  **all** xarrays on a given arch.  Two arrays give two independent
  *encodings*, not two slot widths.
- A union is type punning (same bits, a different view), not extra space.
  Only the pointer-width portion is transferred to `xa_store()`.
- Masking only removes bits; zero-extending a 31-bit value yields 64 bits
  whose new top bits are zeros -- no information created.  Storing that
  into a 32-bit slot drops them.
- On 64-bit the trick works only because canonical userspace pointers
  leave the upper bits free; that is the whole basis of `xa_mk_value`,
  `xa_mk_internal` and `swp_entry`.  63 usable bits > 55 needed.  On
  32-bit there is no headroom: **31 usable bits** (`sizeof(long)*8 - 1`,
  the `-1` from `(v << 1) | 1` plus the `WARN_ON((long)v < 0)` sign bit).

### 4. RETRACTED an option mid-analysis -- and why it matters

We had been considering storing only the `id` in the per-inode table and
reading the generation from the entry struct instead (`ent->gen` already
exists).  **This is broken.**  `shmem_comp_entries_lookup()` unpacks the
`gen` out of the per-inode table and passes it to
`shmem_comp_entry_get_common()`, which compares it against `ent->gen`
(:1149).  Those two copies must be **independent** -- that comparison is
the entire stale-id defence.  If the gen came from `ent` itself, the
check would compare `ent->gen` against itself and always pass, so a stale
reader holding a freed-then-reused id would silently read the wrong
page's data.

Lesson: the retraction was only caught because we read the actual lookup
path instead of reasoning from the struct.  Do not "simplify" the packed
{id, gen} into an id alone.

### 5. The free path is already RCU (this reorders the options)

```c
kfree_rcu(ent, rcu);                                   /* :1133 */
rcu_read_lock();                                       /* :1147 */
ent = xa_load(&shmem_comp_entries, id);
if (ent && check_gen && READ_ONCE(ent->gen) != gen)    /* :1149 */
	/* Stale xarray value */
```

The entry is freed with `kfree_rcu` and read under `rcu_read_lock`.

### 6. Found a SECOND, SILENT 32-bit bug (this is the important new one)

`include/linux/shmem_compress.h:190`:

```c
#define CELL_LOC_OFFSET_BITS	24

static inline unsigned long shmem_comp_pack_loc(u32 cellid, u32 offset)
{
	return ((unsigned long)(cellid + 1) << CELL_LOC_OFFSET_BITS) |
		(offset & (CELL_LOC_OFFSET_MAX - 1));
}
```

`cellid` lives at **bit 24**, so on i386 only 8 bits of it survive (bits
24-31).  The `struct shmem_comp_entry` comment says cellid is 16 bits and
offset needs 22, i.e. ~38-40 bits total.  On 32-bit that silently
truncates.

Unlike the `gen << 32` break, **this one produces no compile error**: the
shift count (24) is a constant *below* the 32-bit width and the shifted
value (`cellid + 1`) is runtime, so `-Wshift-count-overflow` stays quiet.
Any cellid > 255 wraps to a **wrong cell** -- silent data corruption on
read, not a build failure.  This is the exact failure mode the
fixed-width house rule exists to prevent, and it is strictly more
dangerous than the bug that failed to compile.

Note the struct comment is also stale: it says offset is 22 bits but the
constant is 24.

## What We Learned

### Technical

- **An xarray slot is pointer-width for every array on an arch**, so two
  arrays cannot have two slot widths.  They can have two *encodings*,
  which is a different and real freedom.
- **The `xa_mk_value` budget is `sizeof(long)*8 - 1`**: 63 bits on 64-bit,
  31 on 32-bit.  The lost two bits are the `<<1` tag and the
  `WARN_ON((long)v < 0)` sign bit.  On 64-bit this is cheap because
  canonical userspace pointers leave the upper bits free; on 32-bit the
  31 bits are all there is.
- **`gen` must be an independent copy.**  The stale-id check at :1149
  compares a gen read from the per-inode table against `ent->gen`.  Take
  the gen from `ent` instead and the check becomes vacuous -- it would
  always pass -- and a recycled id would silently serve another page's
  data.
- **`kfree_rcu` + `rcu_read_lock` is already the lifetime mechanism**, so
  a raw pointer is safe for the duration of a read.  That is what makes
  the pointer encoding viable, and what weakens the gen-wrap ABA worry.
- **A constant shift below the type width never warns, even when the
  value overflows it.**  This is the whole difference between bug #1
  (loud) and bug #2 (`pack_loc`, silent).  Any `<< N` on an
  arch-dependent type must be checked by hand, not by the compiler.
- **Two 32-bit breaks were already present in code whose comments did the
  bit arithmetic out loud** ("16 bits", "22 bits") while the type
  (`unsigned long`) silently disagreed.  Doing width arithmetic in a
  comment but not in the type is the smell to grep for.

### Process

- **Reading the lookup path beat reasoning from the struct.**  The broken
  option in step 4 looked correct from the struct definition and was
  disproved by five lines of `shmem_comp_entries_lookup()` +
  `shmem_comp_entry_get_common()`.
- **One grep would have found bug #2 a session earlier**: `unsigned long`
  combined with `<<` across the feature.  Run the width audit in the same
  pass as the first break, not after.
- **A compile gate is the cheapest correctness tool in the series.**  It
  cost one config and minutes, and produced two real upstream blockers.
- **House rules belong in the log immediately**, not at session end -- the
  fixed-width rule was set mid-conversation and is the lens every later
  finding was read through.

## What We Decided

- **Arch-correctness gates perf.**  H618 is the motivation; upstream is
  the gate.  Correctness sweeps are emulatable under QEMU TCG; perf runs
  only on real silicon.
- **Adopt the fixed-width-only house rule** for the feature: `u32`/`u64`
  for anything with a defined bit width, `unsigned long` only for
  pointer/size-shaped values and kernel API signatures.
- **Rejected the id-only encoding** for table 2 (the gen would become
  self-comparing and the staleness check vacuous).
- **Leaning to option (b)** -- store `xa_mk_pointer(ent)` in table 2 --
  because it removes the width problem on every arch by construction and
  may collapse table 3 entirely.  Gated on the read-side RCU audit.
- **`handle`/`pack_loc` is fixed independently of that choice**; it is a
  second, silent corruption bug and outranks the compile error in
  urgency.
- No source changes were made this session.

## The Data-Structure Map (for the fresh session)

Four distinct structures.  Do not conflate them.

| # | Structure | Scope | Key | Value | Purpose |
|---|-----------|-------|-----|-------|---------|
| 1 | shmem `i_pages` | per-inode | page index | folio / swp_entry | the normal page cache; unchanged |
| 2 | `info->comp_entries` (`shmem_fs.h:70`) | per-inode, **lazy** alloc (`shmem.c:3189`) | page index | **packed {id, gen}** (a value) | spec 02 two-table mapping; the compressed slot for a page |
| 3 | `shmem_comp_entries` (`DEFINE_XARRAY_FLAGS(..., XA_FLAGS_ALLOC)`, `:1022`) | **global** | id (xa_alloc-assigned) | `struct shmem_comp_entry *` | the entry table; refcounted, shared by merge groups |
| 4 | `pool->cell_index[cellid]` reverse index | per-pool | cellid | `list_head` of `ent->cell_node` | GC drain is O(entries-in-victim) not O(total); `specs/04_GC_REVERSE_INDEX.md` |

`struct shmem_comp_entry` (shmem_compress.h:96) fields that matter here:
- `unsigned long handle` -- storage location; zs_malloc handle, OR
  `pack_loc({cellid, offset})` for cell pools.  **Arch-dependent width;
  this is bug #2.**
- `unsigned int id` -- its index in table 3, stable for its life.
- `unsigned int gen` -- incarnation stamp, packed with the id into table
  2.  Pin and release validate it.
- `atomic_t refs` / `atomic_t slots` -- two-counter scheme: `slots` =
  table-2 slots naming this entry (the durable ref); `refs` = slots plus
  temporary reader pins during decompress.  Split so truncate can drop a
  slot without freeing an entry a reader is mid-decompress on.
- `struct list_head cell_node` -- reverse-index linkage (cell pools only).
- `struct rcu_head rcu` -- for `kfree_rcu`.

Read path: table 2 lookup -> {id, gen} -> table 3 `xa_load(id)` ->
validate `ent->gen == gen` -> `atomic_inc_not_zero(&ent->refs)` ->
`unpack_loc(ent->handle)` -> cell/zsmalloc load.

## The 32-bit Fix: Options, Corrected

All three table-2 encodings share the same budget: **31 bits on 32-bit,
63 on 64-bit**.  The payload needs id + gen = 55 bits today.

- **(a) Shrink the split to fit 31 bits** (e.g. id 20 / gen 11).  I first
  called this an ABA risk; the `kfree_rcu` finding weakens that a lot --
  ABA would need the *same* id freed and reallocated 2048 times inside one
  RCU grace period.  Probably safe, but it needs a **hard cap on
  concurrent entries that does not exist today** (`xa_alloc` is
  unbounded), and it trades an absurd margin (23 bits) for a merely very
  unlikely one (11).  Reviewers will ask.
- **(b) Store `xa_mk_pointer(ent)` in table 2.**  A pointer fills the slot
  on every arch by definition -- no packing, no width problem.  And since
  the free is `kfree_rcu` and reads are under `rcu_read_lock`, the object
  is guaranteed alive for the whole read, which would make the `gen`
  check redundant and might collapse table 3 away (table 2 could point
  straight at the refcounted `ent`).  **A real simplification, but a
  spec-02 change.**  Gated on the audit below.
- **(c) `depends on 64BIT` for now.**  Unblocks the series, does not close
  the gate.

Regardless of (a)/(b)/(c), `handle`/`pack_loc` needs its own fix -- it is
independent of the table-2 encoding.

## House Rules Added

- **Fixed-width types only for fixed logical data.**  `u32`/`u64`, never
  `unsigned long`, for anything with a defined bit width (packed fields,
  IDs, encoded handles).  `unsigned long` is reserved for genuinely
  pointer/size-shaped values and for kernel API signatures.
- **Check the packing budget before widening a value.**  `xa_mk_value`
  holds `sizeof(long)*8 - 1` bits.  Anything wider is a 32-bit bug the
  first arch gate will find.
- **A shift by a constant below the type width is not a compile error even
  when the value overflows.**  `-Wshift-count-overflow` only fires for
  constant counts >= width.  Silent truncation (bug #2) is strictly worse
  than a build failure -- audit every `<< N` on an arch-dependent type by
  hand.
- **Do not reason a concurrency fix from the struct alone; read the lookup
  path.**  That is what caught the broken option in step 4.

## What We'd Do Differently

- Would have run the i386 gate before writing the two-table mapping, not
  after a full x86_64 benchmark campaign; both bugs were always there.
- Would have used `u64`/`u32` for `handle` and the packed entry from the
  start -- the comments already do the bit arithmetic ("16 bits", "22
  bits") that `unsigned long` cannot honour on 32-bit.  Doing the width
  arithmetic in a comment but not in the type is the smell.
- Would have grepped the whole feature for `unsigned long` + `<< N` in
  one pass at the time of bug #1; that single grep would have found bug
  #2 immediately instead of a session later.

## Files Changed

| File | Purpose |
|------|---------|
| none | analysis session; no source changes |

## Next Steps

1. **Audit the read-side RCU discipline** to decide whether option (b) is
   viable: find every consumer of a table-2 value and confirm it stays
   inside one `rcu_read_lock` section with no intervening sleep.  Any
   caller that stashes {id, gen} across a sleep kills (b), which is
   probably exactly why `gen` exists.
2. **Fix `handle`/`pack_loc`** for 32-bit (independent of the above).
   Options: narrow cellid to 8 bits on 32-bit (caps cells at 255), reduce
   `CELL_LOC_OFFSET_BITS` on 32-bit, or split into two `u32` fields in the
   entry struct (costs 4 bytes per entry, and the comment says the entry
   table is proportional to compressed blocks -- so measure).
3. Pick (a)/(b)/(c) for the table-2 encoding and re-run the i386 gate;
   `mm/shmem_compress.o` must reach 0 errors.
4. Extend the gate to arm32, arm64 with 16K and 64K pages (the
   `PAGE_SIZE << cell_order` coupling and the `use_thp` threshold), and a
   big-endian build (cell-format endian safety).
5. Standing backlog: plateau mechanism (lockstat), CPU-freq confound,
   workload bed (spec 05, sustained churn), RFC assembly (two-table),
   H618 re-measure of every x86 number.

---

## Session 54: Full Source Read and the Read-Side RCU Audit (2026-10-15)

### What We Did

Read the whole subsystem source in the fresh session, as planned:
`mm/shmem_compress.c` (4264 lines, in full), `include/linux/shmem_compress.h`
(832 lines, in full), plus the compression call sites in `mm/shmem.c`
(get_folio_gfp :2576, write_begin :3404, write_end :3442,
undo_range :1183/:1506), `include/linux/shmem_fs.h` (`comp_entries`
:70) and the GEN_BITS block in `include/linux/swapops.h` :139-150.

Then ran the next-steps item 1: **the read-side RCU audit** that gates
option (b). Full consumer inventory of table-2 values:

| Consumer | Pattern | Stashes {id,gen} across a sleep? |
|---|---|---|
| `shmem_decompress_folio` :3296 | lookup -> `get_gen` pin (adjacent) | No; pin carried across sleeps, which is a ref, not a stashed id |
| `shmem_decompress_group` :2964 | receives {id,gen}, pins immediately | No |
| `shmem_comp_entry_drop` :3712 | `get_gen` + `release`, revalidates | No |
| `shmem_comp_invalidate_index` :3745 | lookup -> xa_store NULL -> drop | id/gen copied out; slot ref keeps entry alive find->drop |
| `shmem_comp_truncate_range` :3776 | xa_find -> unpack -> drop | Same: slot's durable ref covers the window |
| `shmem_comp_load_entry` :1660 | re-resolves by id under RCU, pins internally | Takes id, but pins before any sleep |
| GC drain :1400/:1521 | pins BY POINTER already (`inc_not_zero` on iter) | N/A -- the (b)-shaped pattern, in production today |

**Audit verdict: (b) is viable.** No caller stashes {id, gen} across a
sleep without holding a pin. Every load+pin pair is adjacent (both
inside one `rcu_read_lock` section), and the truncator's find->drop
window is covered by the slot's own durable reference, not by the gen.
The one re-resolution by id after RCU unlock (`load_entry`) pins
internally before sleeping. The GC drain already does exactly the
pointer-pin discipline (b) would require -- `shmem_comp_entry_put_ptr()`
exists and is in production.

Also noted while reading (follow-up, not a 32-bit blocker): the
`shmem_decompress_group` comment :2936 claims slice-install races are
resolved by `shmem_add_to_page_cache()` "only installing if the slot
still holds the replicated ID+generation" -- but the calls pass
`shadow=NULL` and that function never looks at `info->comp_entries`.
The comment describes a check that does not exist in the code; worth a
separate look at the truncate-vs-group-decompress race, independent of
the encoding decision.

### What We Learned (this session)

- **Why the gen is packed WITH the id (the ptr/gen question, asked and
  answered):** an id is a recyclable *name*, not an identity. Resolving
  it is a fresh `xa_load(&shmem_comp_entries, id)` in a NEW RCU section,
  where the entry may be gone and a new one installed at the same id --
  `ent->gen != gen` is the only thing that catches that. The gen must
  travel in the same xa slot as the id because it is only meaningful
  when read atomically with the id; if it were read from the entry
  itself it would be a self-comparison (the retracted option, step 4).
- **A pointer is an identity; RCU is its incarnation check.** Under (b)
  the reader loads the pointer and takes `inc_not_zero` inside one
  `rcu_read_lock` section. `kfree_rcu` cannot reclaim the address until
  after a grace period, which the reader's own RCU section blocks, so
  at pin time the object is provably the original (dying -> pin fails
  -> retry; alive -> correct object). No ABA, no gen needed. The gen is
  a software emulation of exactly this RCU guarantee, required only
  because the id scheme decouples the slot read from the resolution.
- **The pointer-pin discipline is already in production** (GC drain +
  `put_ptr`), so (b) is a generalization of existing code, not a new
  invention.
- **`handle` is `unsigned long` in the struct** (`shmem_compress.h:101`),
  so pack_loc's 40-bit cellid at bit 24 truncates to 8 bits on 32-bit
  at BOTH ends (the field itself, not just the shift). The fix must
  widen the FIELD (u64, per the house rule), not just the expression.

### What We Decided (this session)

- **Option (b) is no longer gated on an unknown** -- the audit is done
  and it passes. Recommendation stands: (b) as the real fix, (c)
  `depends on 64BIT` only as an interim unblocker if the series must
  move before (b) lands.
- **`handle` fix is u64**, independent of (b): change the struct field
  to `u64` and pack/unpack over `u64`; re-price the struct for padding
  on 32-bit (id/length/nr_pages/gen currently align to 4; a u64 field
  may add tail padding -- measure `ksize` impact, the entry table is
  RAM-proportional). No code written yet; plan presented to the user.

### Next Steps (updated order)

1. Fix `handle`/pack_loc: `u64` field + pack/unpack, fix the stale
   struct comment ("fit in one unsigned long"), i386 gate to 0 errors.
2. Implement (b): entries[] stores `xa_mk_pointer(ent)`; put() and
   load_entry() take the pointer; gen retired (or kept as a debug-only
   assert); entries_present() becomes a NULL check; update spec 02.
3. Re-run the i386 gate after each; then extend the arch matrix
   (arm32, arm64 16K/64K, BE) per the standing plan.
4. Separate look at the group-decompress/truncate race the stale
   comment hints at.

---

## Session 55: Step 1 -- u64 Handle Surface (2026-10-15)

User decision at session start: **go 1-3, commit often for bisectability,
log every meaningful step, and extend the house rule -- everything in
OUR code or OUR API is fixed-width or a pointer** (unsigned long only
where a kernel API forces it, converted at the boundary).

### What We Did

Commit `c43e860fb42c` "shmem_comp: widen the handle surface to u64".

- `struct shmem_comp_entry.handle`: `unsigned long` -> `u64`. The FIELD
  itself truncated on 32-bit, not just the shift expression -- fixing
  pack_loc alone would have left the bug live.
- `CELL_LOC_OFFSET_MAX`: `1UL` -> `1ULL` (the mask must be 64-bit too).
- Whole handle API surface -> `u64` end-to-end: `shmem_comp_store`/
  `load`/`free`, `store_merged`/`load_merged`/`free_merged` (handle
  arrays), `entry_alloc`/`entry_get`/`entry_get_gen` params, the GC
  drain locals, and the decompress-path locals.
- `unsigned long` survives ONLY at kernel-API boundaries with explicit
  lossless casts + comment: `zs_free`, `shcomp_zs_write/read_begin/
  read_end`, `xa_for_each` index, shrinker count/scan returns.
- Also fixed stale struct comment (said "cellid 16 bits, offset 22
  bits"; real layout is cellid+1 at bit 24, offset 24 bits).

### Gates

- i386 (`out/kbuild-i386`): only the 2 KNOWN table-2 pack/unpack errors
  remain (bug #1) -- expected, they are step 2. Handle/pack_loc errors: 0.
- x86_64 (`out/kbuild-x86_64`, new out-of-tree dir, ZRAM-dragged
  ZSMALLOC per the i386-gate lesson): 0 errors, 0 warnings.

### Process Note

- The edit-tool exact-match loop against UTF-8 em-dashes in comments
  failed twice (atomic, no partial writes); switched to python
  assert-then-write scripts, which also double as content verification.
- First header pass silently MISSED two prototype lines (return type of
  shmem_comp_store, a continuation line of load_merged) because the
  line filter keyed on "handle" in the line; the conflicting-types
  compile error caught both immediately.  Lesson: verify by grep for
  the old type AFTER a mechanical rename, not just by diffing.

### Next

Step 2: option (b) -- entries[] stores xa_mk_pointer(ent), retire the
{id, gen} packing and the gen counter; i386 must reach 0 errors.

---

## Session 56: Step 2 -- Pointer Encoding, Gen Retired (2026-10-15)

### What We Did

Commit `6ac41848119e` "shmem_comp: entries[] stores entry pointers;
retire the {id, gen} packing" (+ spec-02 update commit).  Option (b),
executed:

- **entries[] slots hold the `struct shmem_comp_entry *` directly.**
  `shmem_comp_pack/unpack` (the 55-bit xa_mk_value encoding, bug #1)
  deleted; the swapops.h `SHMEM_COMP_ENTRY_GEN_BITS/MASK` block
  deleted; the swapops.h include in shmem_compress.c deleted.
- **Read:** `shmem_comp_entries_get()` -- pointer load +
  `atomic_inc_not_zero` inside ONE `rcu_read_lock` section.  kfree_rcu
  defers reuse past a grace period, so the pin succeeds on the original
  or fails on a dying entry.  No ABA, no gen.
- **Clear:** `shmem_comp_entries_claim()` = `xa_erase` (returns old
  value under the xarray lock) -- exactly one thread owns the slot's
  durable ref; `entry_release()` drops it unvalidated.  This closes a
  latent double-drop window: the old gen check rejected recycled ids
  but could NOT stop two live readers of the same {id, gen} both
  dropping the same slot ref (truncate vs write-invalidate were only
  safe by lock discipline, not by the encoding).
- **API reshape:** `entry_alloc` returns the entry (no id/gen
  out-params); `entry_put` pointer-based (GC drain's `put_ptr` folded
  in); `load_entry` takes a caller-pinned entry (no id
  re-resolution); `decompress_group` takes the caller's pin;
  `entry_get_gen`, `entry_get` (callerless after the switch) and the
  `stale_value_retries` stat removed.
- **Spec 02** updated (intro, decompress path, risk row, open question
  resolved).
- Fixed the stale `decompress_group` comment (claimed
  `add_to_page_cache` validates replicated ID+gen on install; that
  check never existed).

### Gates

x86_64 AND i386: `shmem_compress.o`, `cell_store.o`, `shmem.o`,
`filemap.o` -- **0 errors, 0 warnings on both**.  The i386 gate is
CLOSED: both the loud bug (#1, pack/unpack shifts) and the silent one
(#2, pack_loc) are gone, and the whole {id,gen} class of 32-bit width
bug is deleted rather than patched.

### What We Learned (this session)

- **A slice-based rewrite script must anchor on DEFINITIONS, not
  signatures.**  First attempt at the release-path rewrite used
  `s.index("static unsigned int shmem_comp_invalidate_index(struct
  inode *inode, ...")` which matched the FORWARD DECLARATION at line
  ~106 instead of the definition at ~3745, deleting 3632 of 4264 lines
  (caught immediately by `wc -l`; restored from HEAD via `git show` --
  the committed u64 work was safe, only the in-flight stage-4 edits
  were lost and they were re-scripted).  Rule: anchor multi-line
  slices on a following `{` or a unique doc comment, and ALWAYS
  `wc -l` / compile after a bulk script edit before moving on.
- **Assert-then-write python scripts double as verification** -- every
  replacement asserts count==1, so an ambiguous pattern fails loudly
  before anything is written.  This caught 3 miscounts (put occurrences)
  in-flight.
- **The claim-based clear is strictly stronger than the gen check.**
  Designing the pointer scheme exposed that the old release-side gen
  check was load-bearing for a race the encoding could not actually
  close (two concurrent clearers of one slot); the xa_erase claim
  closes it by construction.  The encoding switch fixed a concurrency
  hole, not just a width bug.
- **Comment archaeology pays**: the `decompress_group` comment
  described a validate-on-install check that never existed in
  `shmem_add_to_page_cache` (it receives `shadow=NULL` and never sees
  `info->comp_entries`).  The real defenses (-EEXIST retry, pin
  failure -> -ENOENT) are now documented where they live.

### What We'd Do Differently

- Would have written the whole rewrite as ONE python pass with
  count-asserted anchors (which is what pass 1-3 eventually became)
  instead of four incremental ones; each incremental pass was a chance
  to match a forward declaration.  The forward-decl/definition
  ambiguity is a known hazard of editing C with text tools -- grep -n
  the anchor FIRST, every time.

### Next

Step 3: extend the arch matrix -- arm32, arm64 with 16K and 64K pages,
big-endian build -- then the standing backlog.

---

## Session 57: Step 3 -- Arch Matrix Extension (2026-10-15)

### What We Did

New reusable gate script `out/build_arch_gate.sh <ARCH> <subdir>
[extra scripts/config flags]` -- defconfig, ZRAM-drags-ZSMALLOC (the
i386-gate lesson baked in), olddefconfig, grep-verify the feature
symbol survived, prepare, build the 4 arch-sensitive objects with
per-object warning counts.  Ran the matrix with the toolchains
present on the workstation (no installs permitted):

| config | toolchain | result |
|---|---|---|
| i386 (32-bit) | host gcc -m32 | 0 errors, 0 warnings (session 55/56) |
| x86_64 | host gcc | 0 errors, 0 warnings |
| arm64 4K pages | aarch64-linux-gnu-gcc | 0 errors, 0 warnings |
| arm64 **16K pages** | aarch64-linux-gnu-gcc | 0 errors, 0 warnings |
| arm64 **64K pages** | aarch64-linux-gnu-gcc | 0 errors, 0 warnings |
| riscv64 | riscv64-linux-gnu-gcc | 0 errors, 0 warnings |

**GAP (blocked on toolchain, not on code):** arm32 and big-endian
(s390x or powerpc64-be) builds cannot run -- no
`arm-linux-gnueabihf-gcc`, no s390x/powerpc cross compilers installed,
and installing is forbidden without user approval.  The BE cell-format
endian-safety question and the arm32 second 32-bit width sample remain
OPEN until a toolchain is provided.  The i386 32-bit sample already
covers the width class; BE is the remaining untested axis.

Note: `ARCH=riscv`, not `riscv64` -- the arch dir is arch/riscv (the
first run failed on `arch/riscv64/Makefile: No such file`; cheap fix,
but it cost a full defconfig cycle to discover).

### Current State

The 32-bit fix (steps 1+2) is complete and gated on 6 configs.  The
series builds warning-free on every arch reachable with installed
toolchains: 32-bit (i386), 64-bit LE (x86_64, arm64 at all three page
sizes, riscv64).  Outstanding: arm32 + BE gates (toolchain-gated), the
group-decompress/truncate interleaving question from session 54 (the
stale comment is fixed, the underlying race question is not
investigated), and the standing backlog (plateau mechanism, CPU-freq
confound, workload bed spec 05, RFC assembly, H618 re-measure).

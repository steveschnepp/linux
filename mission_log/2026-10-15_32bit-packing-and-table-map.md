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

# PAX — algorithm of an INSERT

Page format **v6** (`PAX_PAGE_VERSION 6`). This document follows one row from
the executor down to the byte written in a page, and gives the algorithm of
every function on the way. Line references are to `pax_am.c` at the commit where
this file was written; the code is the authority if they drift.

---

## 1. Entry points

Three table-AM callbacks can produce an inserted row. Only the first two do work.

| callback | implementation | notes |
|---|---|---|
| `tuple_insert` | `pax_tuple_insert` (4028) | the real path: one row per call |
| `multi_insert` | `pax_multi_insert` (991) | a `for` loop over `tuple_insert`, nothing more |
| `tuple_insert_speculative` | `pax_tuple_insert_speculative` (976) | rejects |
| `tuple_complete_speculative` | `pax_tuple_complete_speculative` (984) | rejects |

```c
static void
pax_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
                 CommandId cid, uint32 options, BulkInsertState bistate)
{
    int i;
    for (i = 0; i < nslots; i++)
        pax_tuple_insert(rel, slots[i], cid, options, bistate);
}
```

So `COPY` and `INSERT ... SELECT` take exactly the same path as a single-row
`INSERT`, one `ReadBuffer`/lock/`GenericXLog` round trip per row. There is no
bulk path and no `bistate` caching.

The executor reaches this through the `tableam.h` inline wrapper:

```c
static inline void
table_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
                   uint32 options, BulkInsertState bistate)
{
    rel->rd_tableam->tuple_insert(rel, slot, cid, options, bistate);
}
```

**Signature note.** `options` is `uint32_t` for `PG_VERSION_NUM >= 190000` and
`int` before that; `pax_am.c:4026` carries the `#if`. Only
`TABLE_INSERT_FROZEN` is examined.

### The two rejections at the top of the function

```c
if (IsParallelWorker())
    ereport(ERROR, ... "pax table AM does not support parallel inserts");

if (cid == InvalidCommandId)
    elog(ERROR, "pax: invalid insert command ID");
```

Parallel insert is refused outright: two workers inserting into the same page
would race on the `memmove` in step 4. A single-worker build is a correctness
requirement here, not a tuning choice.

---

## 2. Phase A — detach the row from the slot

Before a single byte of the page is touched, all values are copied out. This is
not a convenience: it is a correctness requirement.

```c
if (!TTS_EMPTY(slot))
    ExecMaterializeSlot(slot);

values  = palloc(sizeof(Datum) * natts);
isnulls = palloc(sizeof(bool)   * natts);
voffs   = palloc(sizeof(OffsetNumber) * natts);

for (i = 0; i < natts; i++)
{
    voffs[i]  = InvalidOffsetNumber;
    values[i] = slot_getattr(slot, i + 1, &isnulls[i]);

    if (!isnulls[i] && attr->attlen == -1)
        values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
}
```

The reason, in the code's own words: for `INSERT ... SELECT` the source may point
into a PAX page — *possibly the very page being written* — and the `memmove` calls
in step 4 would move it out from under the reader. `ExecMaterializeSlot` forces
the slot to hold a private copy, and `PG_DETOAST_DATUM` does the same for
potentially-toasted varlena values.

`voffs[]` holds, for each column, the byte offset where its variable-length value
was eventually stored. It stays `InvalidOffsetNumber` for NULL and fixed-width
columns.

Then:

```c
xmin = GetCurrentTransactionId();
if (options & TABLE_INSERT_FROZEN)
    xmin = FrozenTransactionId;

CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
```

`xmin` is the **current subtransaction** XID, not the top-level one. That is
what makes `ROLLBACK TO SAVEPOINT` leave the physical row permanently invisible
even if the outer transaction commits — the row records an XID that will abort.
The SSI check is deliberately coarse: it is relation-level, with no predicate
locks, because PAX has only sequential scans.

---

## 3. The page, for reference

```text
offset 0
  PageHeaderData                        24 B
  PaxPageHeader                         8 + 2*natts, MAXALIGNed
      -> PaxPageHeaderPtr = page + 24
      n_tuples, meta_offset, free_space, flags, offsets[natts]
  metadata region                       n_tuples * 32 B      <- pd_lower rises
  column region 0  [NULL bitmap][slots]
  column region 1  [NULL bitmap][slots]
  ...
  free space
  variable-length payloads                                  <- pd_upper falls
  PaxSpecialData                        8 B
offset 8192
```

Three invariants worth holding on to:

- **There is no line-pointer array.** A version is addressed by
  `(block, tupno)`, and `t_ctid` stores `(block, tupno + 1)` — a *logical*
  index, not a byte offset. That is why step 4 may move data freely: no stored
  link points at a byte position.
- **`offsets[i]` are byte offsets**, not line-pointer numbers. `PaxOffsetIsValid`
  is therefore `(off != InvalidOffsetNumber && off < BLCKSZ)`, not
  `OffsetNumberIsValid` — the latter is bounded by `BLCKSZ / sizeof(ItemIdData)
  = 2048` and would reject every byte in the upper half of the page.
- **Slots are packed with no `MAXALIGN`.** `pax_get_value` reads through
  `memcpy`, so a misaligned slot is correct.

---

## 4. Phase B — choose a page (step 1)

### 4.1 `pax_insert_space_hint` — the ask to the FSM

```c
need = SizeOfPaxTupleMetaData;                   /* 32 */
for each attribute:
    need += pax_slot_stride(attr);                /* slot + 1 */
    need += sizeof(uint64);                       /* worst bitmap growth */
    if (varlena and not null):
        need += MAXALIGN(len);
return Max(1, Min(need, PaxMaxFSMRequestSize));   /* clamped to 8160 */
```

Two deliberate properties:

**It is an over-estimate, not a lower bound.** `pax_insert_space_needed` counts
the *actual* bitmap growth of this row, which is `pax_bitmap_size(tupno+1) -
pax_bitmap_size(tupno)`. Because `pax_bitmap_size` MAXALIGNs to 8, that delta is
either **0 or 8** bytes per column — 8 only on the versions that cross an
8-version boundary (tupno 0, 8, 16, …). The hint charges `sizeof(uint64)` = 8
per column, which is therefore not a loose bound but *exactly* the worst case.

If the hint were a lower bound instead, a page holding between `hint` and
`needed` free bytes would be proposed by the FSM, rejected under the exclusive
lock, released, and proposed again forever: the page selection loop would not
terminate. That was a real hang, and an under-estimate in a hint that drives a
retry loop is a **liveness** bug, not a packing inefficiency.

**It is clamped at `PaxMaxFSMRequestSize` = 8160.** That constant is private to
`freespace.c` (`#define MaxFSMRequestSize MaxHeapTupleSize`), so `pax_am.c`
recomposes the expression and pins it:

```c
#define PaxMaxFSMRequestSize \
    (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)))

StaticAssertDecl(MaxHeapTupleSize == PaxMaxFSMRequestSize,
                 "pax: FSM request ceiling moved; update PaxMaxFSMRequestSize");
```

Clamping *down* is only safe because the value is already an over-estimate, so any
clamp can only make it larger. At 20 columns the unclamped ask reaches 10448 and
the FSM answers `invalid FSM request size`.

### 4.2 Candidate selection

```c
nblocks = RelationGetNumberOfBlocks(rel);
target  = RelationGetTargetBlock(rel);
if (target != InvalidBlockNumber && target >= nblocks)
    target = InvalidBlockNumber;          /* relation shrank */

if (target == InvalidBlockNumber)
{
    target = GetPageWithFreeSpace(rel, hint);
    if (target == InvalidBlockNumber && nblocks > 0)
        target = nblocks - 1;             /* silent FSM: try the last page */
}
```

Three sources, in order of cost:

1. **`RelationGetTargetBlock`** — a per-backend hint in the relcache, set at the
   end of the previous successful insert (step 5). Free, and it makes sequential
   inserts land on the same page.
2. **`GetPageWithFreeSpace(rel, hint)`** — an O(1) FSM lookup.
3. **`nblocks - 1`** — the append-at-the-end fallback, needed because the FSM is
   silent on a relation that has just been created or after a crash. Without it,
   every row would get its own page.

### 4.3 The retry loop

```c
while (target != InvalidBlockNumber && target < nblocks)
{
    CHECK_FOR_INTERRUPTS();
    b = ReadBuffer(rel, target);
    LockBuffer(b, BUFFER_LOCK_EXCLUSIVE);
    p = BufferGetPage(b);

    if (!pax_page_is_valid(p)) { ... break or error ... }

    if (special->n_attrs != natts) { ... error ... }

    h          = PaxPageHeaderPtr(p);
    needed     = pax_insert_space_needed(rel, values, isnulls, h->n_tuples, h);
    free_space = pax_page_free_space(p);

    if (free_space >= needed) { buf = b; break; }

    UnlockReleaseBuffer(b);
    target = RecordAndGetPageWithFreeSpace(rel, target, free_space, hint);
}
```

The decisive detail is that `h->n_tuples` is read **after** the exclusive lock.
The FSM is only a proposal; the page state can have changed since the lookup.
On rejection the *actual* free space is recorded, which excludes that page from
future proposals, and another candidate is asked for. Termination comes from
`RecordAndGetPageWithFreeSpace` returning `InvalidBlockNumber`, which it does
when no page holds `hint` bytes — and that is sound precisely because `hint >= needed`.

### 4.4 The two predicate helpers

```c
pax_page_is_valid(page):
    if (PageGetSpecialSize(page) < SizeOfPaxSpecialData)  return false;
    return special->version == PAX_PAGE_VERSION
        && special->magic   == PAX_SPECIAL_MAGIC
        && (special->flags & PAX_FLAG_HAS_XMIN_XMAX)
        && (special->flags & PAX_FLAG_HAS_VERSIONS);

pax_page_free_space(page):
    return (Size) (phdr->pd_upper - phdr->pd_lower);
```

A page failing `pax_page_is_valid` is fatal unless it is `PageIsNew`, i.e. a
block just extended. A valid page whose `n_attrs` disagrees with the tuple
descriptor is also fatal: that is a page written by a different schema, and
decoding it with the current strides would be silent corruption.

### 4.5 `pax_insert_space_needed` — the exact requirement

```c
bitmap_delta = pax_bitmap_size(tupno + 1) - pax_bitmap_size(tupno);   /* 0 or 8 */
need = 32;                                    /* the metadata record */
for each attribute:
    need += pax_slot_stride(attr) + bitmap_delta;
    if (varlena and not null)
        need += MAXALIGN(len);
if (need > BLCKSZ)  ereport(ERROR, "pax: row is too large for one page");
```

with the two size primitives:

```c
pax_slot_stride(attr):   return attr->attlen > 0 ? attr->attlen : 2;
pax_bitmap_size(n):      return n <= 0 ? 0 : MAXALIGN((n + 7) / 8);
```

`pax_bitmap_size` is `MAXALIGN`ed, so its step is 8 and not 1: it goes
0, 8, 16, 24, … and the growth charged to one column is 0 or 8 bytes. It is
aligned because the value table that follows it starts there, and slots are read
with `memcpy`. The span of a region is *not* canonical — a region may carry
padding from when it was opened — which is why the reader derives the bitmap size
from the version count and never from `offsets[i+1] - offsets[i]`.

The two `MaxAllocSize - need` guards catch a `Datum` whose reported length would
overflow the accumulator.

---

## 5. Phase C — extension, WAL, and a fresh page

### 5.1 Relation extension

```c
LockRelationForExtension(rel, ExclusiveLock);
buf = ReadBuffer(rel, P_NEW);
LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
if (!PageIsNew(page)) { ... "pax: concurrent extension returned a non-new page" ... }
page_is_new = true;
```

`P_NEW` does not by itself serialise extension: two inserters can resolve the same
not-yet-initialised block. The extension lock is what prevents that.

### 5.2 Generic WAL

```c
wal_state = GenericXLogStart(rel);
page = GenericXLogRegisterBuffer(wal_state, buf,
                                 page_is_new ? GENERIC_XLOG_FULL_IMAGE : 0);
```

`page` is a **private copy** of the buffer. Every mutation in steps 2 to 4 is
applied to that copy, and `GenericXLogFinish()` applies it to the shared buffer
and emits the WAL record atomically. A partially written page can therefore never
reach disk.

### 5.3 `pax_page_init` and the too-large check

```c
if (page_is_new)
{
    pax_page_init(page, natts);
    needed    = pax_insert_space_needed(rel, values, isnulls, 0, NULL);
    available = pax_page_free_space(page);
    if (available < needed)
        ereport(ERROR, "pax: row is too large for one page",
                errdetail("The row needs %zu bytes but a new PAX page has only %zu bytes.",
                          needed, available));
}
```

```c
pax_page_init(page, n_attrs):
    PageInit(page, BLCKSZ, SizeOfPaxSpecialData);
    special->version = 6; special->magic = 0x5041;
    special->flags = HAS_XMIN_XMAX | HAS_VERSIONS; special->n_attrs = n_attrs;

    header_size = MAXALIGN(SizeOfPaxPageHeaderFixed + n_attrs * 2);
    phdr->n_tuples = 0;
    phdr->free_space = BLCKSZ - (24 + header_size);
    for (i) phdr->offsets[i] = InvalidOffsetNumber;

    pd_lower    = 24 + header_size;
    phdr->meta_offset = pd_lower;
```

A new page starts with the header, then the metadata region begins immediately.
At the 20-column schema used by the regression suite the header is
`24 + MAXALIGN(8 + 20*2) = 72` bytes, leaving 8112 usable.

The largest single value that still fits on an empty page was measured at
**7784 bytes** for the regression suite's row shape. That is not a constant of
the format: the row's *other* varlena columns (`t1`, `t3`, `num`, `iv1`, `ba`)
still take ~64 bytes of arena, and the 20 bitmap slots plus 92 bytes of fixed
strides plus the 32-byte metadata record are charged too. A row with only the
one large column and nothing else would go higher. Past the limit the row
cannot be placed even on an empty page, because the regions and the metadata
have to grow along with it.

---

## 6. Phase D — slot selection, and in-place reuse

```c
reusing = false;
if (phdr->n_tuples > 0)
{
    for (c = 0; c < natts; c++)
        if (!PaxOffsetIsValid(phdr->offsets[c])) break;

    if (c == natts)                                   /* all regions exist */
    {
        payload_needed = pax_payload_space_needed(rel, values, isnulls);
        if (pax_page_free_space(page) >= payload_needed)
            for (c = 0; c < phdr->n_tuples; c++)
                if (pax_meta_is_unused(&meta_base[c])) { tupno = c; reusing = true; break; }
    }
}
if (!reusing)
    tupno = phdr->n_tuples;
```

A slot marked `PAX_VERSION_UNUSED` by VACUUM still owns its slot in every region
and its bit in every bitmap. Reusing it therefore requires **no growth at all** —
only overwriting values in place. That is what makes space reclaimed by VACUUM
usable where it lies.

Three guards, each for a distinct reason:

- **All regions must already exist.** A column never written on this page is NULL
  on every row and has no slot to reuse. Creating one would have to grow the page,
  which is exactly what appending would cost anyway.
- **The variable-length values must fit.** Reuse saves the slot and the metadata
  but the payloads are still allocated, so they must be accounted separately:

  ```c
  pax_payload_space_needed(rel, values, isnulls):
      need = 0;
      for each attribute:
          if (null or attlen >= 0) continue;
          need += MAXALIGN(attlen == -2 ? strlen(p) + 1 : VARSIZE_ANY(p));
      return need;
  ```

  Checked *before* committing to the choice.
- **`tupno < MaxOffsetNumber`**, checked after, since `OffsetNumber` is 16 bits
  while a page could otherwise be asked to hold more versions.

---

## 7. The four write steps

### Step 2 — variable-length payloads, first

```c
for (i = 0; i < natts; i++)
{
    if (isnulls[i] || attr->attlen >= 0) continue;
    voffs[i] = pax_alloc_payload(page, values[i], attr->attlen);
    if (!PaxOffsetIsValid(voffs[i])) elog(ERROR, "pax: no space left ...");
}
```

```c
pax_alloc_payload(page, value, attlen):
    len = (attlen == -2) ? strlen(p) + 1 : VARSIZE_ANY(p);
    aligned_len = MAXALIGN(len);
    if (pd_upper - pd_lower < aligned_len) return InvalidOffsetNumber;
    pd_upper -= aligned_len;
    memcpy(page + pd_upper, p, len);
    return pd_upper;
```

Payloads grow **downward from `pd_upper`**, and they are allocated *before* any
`memmove`, while the free-space check from phase C is still fresh. Doing it later
would mean re-deriving the space after each region has shifted.

### Step 3 — the metadata record

```c
meta_size = pax_meta_region_size(page, phdr);
if (meta_size != (Size) phdr->n_tuples * SizeOfPaxTupleMetaData)
    elog(ERROR, "pax: inconsistent transaction metadata region");

meta_at = phdr->meta_offset + tupno * 32;
if (!reusing)
    pax_insert_bytes(page, phdr, natts, PAX_NO_REGION, meta_at, 32);

meta = ((PaxTupleMetaData *) (page + phdr->meta_offset)) + tupno;
memset(meta, 0, 32);
meta->xmin = xmin;  meta->xmax = InvalidTransactionId;
meta->cmin = cid;   meta->cmax = InvalidCommandId;
meta->locker_mxid = InvalidMultiXactId;
```

```c
pax_meta_region_size(page, phdr):
    start = phdr->meta_offset;
    end = (special->n_attrs > 0 && PaxOffsetIsValid(phdr->offsets[0]))
            ? phdr->offsets[0] : pd_lower;
    if (end < start) elog(ERROR, "pax: invalid transaction metadata region");
    return end - start;
```

The invariant is `region == n_tuples * 32`. It is deliberately checked against
`n_tuples`, **not** against `tupno`: on reuse `tupno < n_tuples` and the region
does not change, so comparing with `tupno` would reject every reuse.

`PAX_NO_REGION` (`-1`) is passed because the metadata region is not a column
region: no `offsets[j]` may be shifted, only later metadata records.

### Step 4 — one region per column

For each attribute, in order:

```c
/* 4a. open the region if this is the column's first write on the page */
if (!PaxOffsetIsValid(phdr->offsets[i]))
    phdr->offsets[i] = pd_lower;
region_start = phdr->offsets[i];

/* 4b. grow the NULL bitmap to cover bit tupno */
cur  = pax_bitmap_size(tupno);
want = reusing ? cur : pax_bitmap_size(tupno + 1);
if (want > cur)
{
    at = region_start + cur;
    pax_insert_bytes(page, phdr, natts, i, at, want - cur);
    memset(page + at, 0, want - cur);
}
bmp = (bits8 *) (page + region_start);

/* 4c. reserve the slot */
at = region_start + pax_bitmap_size(reusing ? n_tuples : tupno + 1) + tupno * stride;
if (!reusing)
    pax_insert_bytes(page, phdr, natts, i, at, stride);

/* 4d. set the NULL bit, then write the value */
if (isnulls[i]) bmp[tupno >> 3] &= ~(1 << (tupno & 7));
else            bmp[tupno >> 3] |=  (1 << (tupno & 7));

if (isnulls[i])                    memset(page + at, 0, stride);
else if (attlen > 0 && attbyval)   switch (attlen) { case 1: *(char*)page+at = DatumGetChar(...); ... }
else if (attlen > 0)               memcpy(page + at, DatumGetPointer(v), attlen);
else                               *(OffsetNumber *)(page + at) = voffs[i];
```

Three details in step 4c are easy to get wrong and are commented as such in the
code:

- The value table's base is the **real** bitmap size, not the one being built.
  On a plain append they coincide; on reuse `tupno < n_tuples`, so
  `pax_bitmap_size(tupno)` would be too small and the offset would land in the
  middle of the existing table, shifting every following slot. The reader
  (`pax_build_page_layout`) derives it the same way, so they must agree exactly.
- The region opens at `pd_lower` **to the byte**. No alignment constraint is
  imposed because reads go through `memcpy`.
- By-value types are written by width. Treating `Datum` as a pointer for an
  `int4` would dereference the integer and segfault.

### `pax_insert_bytes` — the primitive everything else is built on

```c
pax_insert_bytes(page, phdr, n_attrs, region_idx, at, len):
    Assert(at <= pd_lower);
    if (pd_lower > pd_upper || len > pd_upper - pd_lower)
        elog(ERROR, "pax: page has no space for %zu bytes", len);

    if (len > 0)
        memmove(page + at + len, page + at, pd_lower - at);

    pd_lower += len;

    for (j = 0; j < n_attrs; j++)
    {
        if (j == region_idx) continue;
        if (PaxOffsetIsValid(phdr->offsets[j]) && phdr->offsets[j] >= at)
            phdr->offsets[j] += len;
    }
```

Three properties, and they are the reason v4 is as simple as it is:

1. **It shifts everything above `at` upward** — the metadata records after the
   insertion point, and every later column region.
2. **It repairs the only index that needs it**, `offsets[]`. That is possible
   because `t_ctid` stores logical `(block, tupno)` pairs and not byte offsets, so
   no other stored reference exists to fix.
3. **Its cost is O(bytes above `at`)**, once per attribute, so an insert moves
   `sum over columns of (pd_lower - region_start[i])` bytes. On the 6-column
   `demo` page that is 7126 bytes — 87% of a page — to store one row. This is the
   defining cost of the format, and it is why the chunked v5 existed.

Note `pax_meta_region_size` needs no repair after a `pax_insert_bytes` on a
region: the metadata region's *start* is `meta_offset` (never shifted) and its
end is `offsets[0]` (shifted), so the two stay consistent by construction.

---

## 8. Phase E — finalise

```c
if (!reusing) phdr->n_tuples++;
phdr->free_space = pax_page_free_space(page);

ItemPointerSet(&slot->tts_tid, BufferGetBlockNumber(buf), tupno + 1);
ItemPointerCopy(&slot->tts_tid, &meta->t_ctid);      /* t_ctid = self: a leaf */
meta->flags = 0;                                     /* reuse: clear UNUSED */
slot->tts_tableOid = RelationGetRelid(rel);
pslot->tuple_meta = *meta;                           /* cached for the caller */

available = pax_page_free_space(page);
GenericXLogFinish(wal_state);

RelationSetTargetBlock(rel, BufferGetBlockNumber(buf));
if (available < BLCKSZ)
    RecordPageWithFreeSpace(rel, BufferGetBlockNumber(buf), available);

if (extension_locked) UnlockRelationForExtension(rel, ExclusiveLock);
UnlockReleaseBuffer(buf);
pfree(values); pfree(isnulls); pfree(voffs);
```

- **TID offset is `tupno + 1`**, because TID offsets start at 1. There is no line
  pointer to be consistent with; this is a convention of the format.
- **`t_ctid` points at itself.** It is a leaf. An UPDATE rewrites the
  *predecessor's* `t_ctid` to point here, and `pax_meta_satisfies_snapshot` makes
  any stale link land on an invisible version so the chain stops.
- **`GenericXLogFinish` before the FSM update**, deliberately: the space published
  must be the space that is actually on the page.
- **`RecordPageWithFreeSpace` after every insert.** Heap delegates this to VACUUM;
  PAX never returns free space by itself — UPDATE and DELETE write only into
  already-allocated metadata records — so without this the FSM would stay silent
  and every insert would extend the relation.

---

## 9. Call graph

```text
table_tuple_insert / table_multi_insert                    execNode / COPY
  └─ pax_tuple_insert
       ├─ ExecMaterializeSlot, PG_DETOAST_DATUM            detach the row
       ├─ GetCurrentTransactionId
       ├─ CheckForSerializableConflictIn
       ├─ pax_insert_space_hint ............ FSM ceiling, over-estimate
       ├─ RelationGetNumberOfBlocks
       ├─ RelationGetTargetBlock
       ├─ GetPageWithFreeSpace
       ├─ [loop] ReadBuffer / LockBuffer
       │        ├─ pax_page_is_valid
       │        ├─ pax_insert_space_needed
       │        │     ├─ pax_slot_stride
       │        │     └─ pax_bitmap_size
       │        ├─ pax_page_free_space
       │        └─ RecordAndGetPageWithFreeSpace
       ├─ LockRelationForExtension / ReadBuffer(P_NEW)
       ├─ GenericXLogStart / GenericXLogRegisterBuffer
       ├─ pax_page_init                      (new page only)
       ├─ pax_payload_space_needed            (reuse test)
       ├─ pax_meta_is_unused                 (reuse test)
       ├─ pax_meta_region_size               (invariant)
       ├─ pax_alloc_payload          ── step 2, pd_upper descends
       ├─ pax_insert_bytes            ── step 3, metadata
       ├─ pax_insert_bytes            ── step 4, per column:
       │     ├─ pax_bitmap_size
       │     └─ (memmove + offsets[] fix-up)
       └─ GenericXLogFinish
            ├─ RelationSetTargetBlock
            └─ RecordPageWithFreeSpace
```

---

## 10. Error paths

Every one aborts the WAL record and releases both locks before raising.

| condition | error |
|---|---|
| parallel worker | `pax table AM does not support parallel inserts` |
| `cid == 0` | `pax: invalid insert command ID` |
| page fails `pax_page_is_valid` and is not new | `pax: invalid page %u in relation %s` |
| `special->n_attrs != natts` | `pax: attribute count mismatch (page %u vs tupdesc %d)` |
| accumulator would overflow | `pax: row size exceeds supported limits` |
| `need > BLCKSZ`, or a fresh page cannot hold it | `pax: row is too large for one page` |
| `ReadBuffer(P_NEW)` returned an initialised page | `pax: concurrent extension returned a non-new page` |
| `tupno >= MaxOffsetNumber` | `pax: too many tuples on page %u` |
| a `memmove` runs past `pd_upper` | `pax: page has no space for %zu bytes` |
| `meta_offset` invalid | `pax: missing transaction metadata region` |
| region size ≠ `n_tuples * 32` | `pax: inconsistent transaction metadata region` |
| payload does not fit | `pax: no space left in page for a variable-length value` |

---

## 11. Locking and concurrency

Two locks, in this order:

1. **`LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE)`** — held across phases C to E.
   Serialises all writers to that page, and excludes readers for the duration.
2. **`LockRelationForExtension(rel, ExclusiveLock)`** — taken only when the
   insert has to extend the relation, and released at the very end.

The lock ordering is buffer before relation, and it is uniform across the whole
AM. That uniformity is not incidental: §14 of `analyse1.md` records a backend
wedging on a multi-row UPDATE because one path took a content lock where another
path expected none, so `next LockBuffer()` waited on a lock the same backend
still held. Any new insert path must follow the same order.

Readers take `BUFFER_LOCK_SHARE` and are excluded for the duration of an insert,
which is why the row had to be detached from the slot in phase A: an
`INSERT ... SELECT` reading from the page being written would otherwise be
reading memory that a `memmove` is about to move.

`XLogInsert` is never called directly; `GenericXLogFinish` emits the record.

---

## 12. What this costs, and where the alternatives were tried

Measured on the 20-column regression schema:

| quantity | value |
|---|---|
| bytes memmoved per INSERT | 7126 on the 99-version 6-column `demo` page (87% of a page) |
| `memmove` calls per INSERT | 1 for the metadata record, +1 per column, +1 more per column whose NULL bitmap crosses an 8-version boundary |
| `offsets[]` repair passes | one per `pax_insert_bytes`, i.e. one per call above |
| metadata | 32 B per version |
| largest single value on an empty page | 7784 B measured, for the regression row shape at 20 columns |
| density vs heap | 1.17× at 20 columns, 1.10× at 12 int columns |

Step 4 is `O(n_attrs)` memmoves of `O(bytes above the region)`. That is the cost
the chunked format of v5 existed to remove, by making regions immovable. It was
built, measured, and reverted: chunks made a row's address stable — the
precondition for the ~60% scan win from lazy materialization — but they pinned
rows-per-page to `PAX_CHUNK_MAX_ROWS`, cost 24% of the density gain on wide
tables, and forced payload compaction off. `analyse1.md` §18 has the full
ledger, including what it would take to reintroduce immovability cheaply.

The figures above are reproduced by `./docs/inspect_pax_page.py demo 0
--schema`, which simulates the same insertion arithmetic in Python and so
computes the memmove volume independently of this document.

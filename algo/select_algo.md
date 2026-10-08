# PAX — algorithm of a SELECT

Page format **v6** (`PAX_PAGE_VERSION 6`). This document follows one row from
the executor down to the bytes it read, and gives the algorithm of every
function on the way. It is the read-side companion to
[`insert_algo.md`](insert_algo.md). Line references are to `pax_am.c` at the
commit where this file was written; the code is the authority if they drift.

---

## 1. Entry points

A `SELECT` reaches PAX through one of four scan shapes, and the *same* loop
serves three of them.

| table-AM callback | implementation | used for |
|---|---|---|
| `scan_begin` | `pax_scan_begin` (3664) | open a scan |
| `scan_rescan` | `pax_scan_rescan` (3727) | restart it |
| `scan_getnextslot` | `pax_scan_getnextslot` (3843) | sequential scan |
| `scan_getnextslot_tidrange` | `pax_scan_getnextslot_tidrange` (3965) | `WHERE ctid = …` / `ctid BETWEEN` |
| `scan_end` | `pax_scan_end` (3711) | close, release the pin |
| `scan_set_tidrange` | `pax_scan_set_tidrange` (3927) | set the TID bounds |
| `tuple_satisfies_snapshot` | `pax_tuple_satisfies_snapshot` (5617) | re-check a slot the executor holds |
| `index_fetch_begin/tuple` | `pax_index_fetch_begin` / `pax_index_fetch_tuple` (719 / 754) | index-only scans |

There is no `scan_gettuple`: PG19 requires `scan_getnextslot`, and a slot is
what PAX returns. The executor reaches it through `tableam.h`:

```c
static inline bool
table_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                       TupleTableSlot *slot)
{
    slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);

    /* We don't expect actual scans using NoMovementScanDirection */
    Assert(direction == ForwardScanDirection ||
           direction == BackwardScanDirection);

    return sscan->rs_rd->rd_tableam->scan_getnextslot(sscan, direction, slot);
}
```

(`access/tableam.h`, verbatim. Note its `Assert` admits only forward or
backward, while `pax_scan_getnextslot` also accepts `NoMovementScanDirection` —
PAX is more permissive than the callers the header anticipates.)

### What is rejected at `scan_begin`

```c
if ((flags & (SO_TYPE_SEQSCAN | SO_TYPE_TIDSCAN |
              SO_TYPE_TIDRANGESCAN | SO_TYPE_ANALYZE)) == 0)
    ereport(ERROR, ... "supports only sequential and TID table scans");
if (nkeys != 0)
    ereport(ERROR, ... "does not support scan keys");
if (pscan != NULL)
    ereport(ERROR, ... "does not support parallel table scans");
```

- **No scan keys.** Qualifying conditions are pushed above the AM, never into
  it. `nkeys != 0` is a hard error, not a silent full scan.
- **No parallel table scan.** One backend per scan: the page layout cache below
  is per-scan state, and the content lock is held across materialization.
- **No backward scan.** Checked per call in `pax_scan_getnextslot`.
- **`SO_TYPE_ANALYZE` is accepted** and behaves specially — see §6.

`scan_begin` then allocates the descriptor, seeds `current_tupno = -1`,
`current_block = 0`, `nblocks = RelationGetNumberOfBlocks(rel)`, and calls
`PredicateLockRelation(rel, snapshot)` for SSI.

---

## 2. The page, for reference

```text
offset 0
  PageHeaderData                        24 B
  PaxPageHeader                         8 + 2*natts, MAXALIGNed
      -> PaxPageHeaderPtr = page + 24
      n_tuples, meta_offset, free_space, flags, offsets[natts]
  metadata region                       n_tuples * 32 B
  column region 0  [NULL bitmap][slots]
  column region 1  [NULL bitmap][slots]
  ...
  free space
  variable-length payloads
  PaxSpecialData                        8 B
offset 8192
```

**There is no line-pointer array.** A row is `(block, tupno)` and `t_ctid`
stores `(block, tupno + 1)` — a logical index, not a byte offset. That is the
property the whole read path leans on: slots move when an inserter memmoves a
region, and nothing stored breaks.

---

## 3. The scan loop: `pax_scan_getnextslot`

```c
if ((scan->rs_base.rs_flags & SO_TYPE_SEQSCAN) == 0)
    elog(ERROR, "pax: non-sequential scan used sequential callback");

if (direction != ForwardScanDirection &&
    direction != NoMovementScanDirection)
    ereport(ERROR, ... "does not support backward scans");

ExecClearTuple(slot);

while (scan->current_block < scan->nblocks)
{
    CHECK_FOR_INTERRUPTS();

    buf = pax_scan_lock_current_page(scan, rel);
    page = BufferGetPage(buf);

    if (!pax_page_is_valid(page)) { ... elog(ERROR, "invalid page %u") ... }

    pdesc = pax_scan_page_desc(scan, page, scan->current_block, tupdesc);
    tupno = pax_find_visible_tuple(scan, pdesc, scan->current_tupno + 1);

    if (tupno >= 0)
    {
        pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
        ExecMaterializeSlot(slot);
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        scan->current_tupno = tupno;
        return true;
    }

    LockBuffer(buf, BUFFER_LOCK_UNLOCK);
    pax_scan_unpin_current(scan);
    scan->current_tupno = -1;
    scan->current_block++;
}

pax_scan_unpin_current(scan);
return false;
```

Four decisions in twenty lines, each load-bearing:

**The descriptor is rebuilt on every call even though the pin is reused.** The
content lock is dropped between calls, so a concurrent inserter may have
memmoved the page. The pin proves the buffer is not evicted; it proves nothing
about the bytes still being where they were.

**`ExecClearTuple(slot)` before the loop, not per row.** The slot is cleared
once, then reused for every returned row.

**Materialization happens under the content lock.** A pin blocks eviction but
not a memmove: an inserter holding the exclusive lock can still shift the
regions while the slot is live. Copied values are therefore mandatory, not an
optimization.

**`LockBuffer(buf, BUFFER_LOCK_UNLOCK)`, not `UnlockReleaseBuffer`.** The pin is
kept so a concurrent UPDATE or DELETE of *this* row can still take an exclusive
lock. The pin is dropped only when moving to the next page or ending the scan.

`scan->current_tupno` is advanced on return and reset to `-1` when the page is
exhausted, so the next call restarts at `tupno = 0` of the next page.

---

## 4. Holding the page: `pax_scan_lock_current_page`

```c
if (BufferIsValid(scan->current_buf) &&
    BufferGetBlockNumber(scan->current_buf) == scan->current_block)
{
    LockBuffer(scan->current_buf, BUFFER_LOCK_SHARE);
    return scan->current_buf;
}

pax_scan_unpin_current(scan);
scan->current_buf = ReadBuffer(rel, scan->current_block);
LockBuffer(scan->current_buf, BUFFER_LOCK_SHARE);
return scan->current_buf;
```

A sequential scan calls this once per row but only pays `ReadBuffer` once per
page: within a page it is a pin-count lookup plus a shared lock. This is the
optimization the shared-buffer comparison measures — the per-tuple
`ReadBuffer` cost is gone.

The caller owns the content lock and must release it with
`LockBuffer(buf, BUFFER_LOCK_UNLOCK)`. That asymmetry is deliberate and is
spelled out in the function's comment.

---

## 5. The page descriptor

### 5.1 `pax_scan_page_desc` — reuse or rebuild

```c
oldc = MemoryContextSwitchTo(scan->desc_ctx);

if (pax_layout_is_current(scan->cached_desc, page, blkno, phdr, tupdesc))
    desc = scan->cached_desc;
else
{
    pax_scan_drop_cached_desc(scan);
    desc = pax_build_page_layout(page, blkno, tupdesc);
    scan->cached_desc = desc;
}

MemoryContextSwitchTo(oldc);
return desc;
```

The whole switch is scoped to `scan->desc_ctx`, a memory context owned by the
scan. Refreshing can reallocate `tuple_meta`, and that memory must belong to
the scan rather than the caller's context.

### 5.2 `pax_layout_is_current` — the cache-validity test

```c
if (desc == NULL || desc->page != page)            return false;
if (desc->stamp_blkno != blkno)                   return false;
if (desc->n_tuples != phdr->n_tuples)             return false;
if (desc->stamp_flags != phdr->flags)             return false;
if (desc->stamp_meta_offset != phdr->meta_offset) return false;
if (desc->n_attrs != tupdesc->natts)              return false;
for (i = 0; i < desc->n_attrs; i++)
    if (desc->stamp_offsets[i] != phdr->offsets[i]) return false;
return true;
```

A private snapshot of the header is taken when the descriptor is built
(`stamp_blkno`, `stamp_flags`, `stamp_meta_offset`, and a private copy of
`offsets[]`). The scan reuses the descriptor only if the page header is
bit-for-bit what it was.

**The `offsets[]` comparison is the one that matters.** An insert shifts every
region after the insertion point, so any change invalidates the cached column
pointers. Checking `n_tuples` alone would miss an insert that grew an existing
region without adding a version — which is exactly what an insert into a
present column does.

### 5.3 `pax_build_page_layout` — what a descriptor holds

Validates the page, then per attribute builds a `PaxMinipage`:

```c
mp->stride     = pax_slot_stride(attr);     /* attlen, or 2 for varlena */
mp->n_values   = phdr->n_tuples;
mp->null_bitmap = (bits8 *) (base + off);
mp->data       = base + off + pax_bitmap_size(desc->n_tuples);
```

Three cases:

- **column never written on this page** — `offsets[i]` is
  `InvalidOffsetNumber`, so `mp->data = NULL` and every row reads as NULL.
- **fixed-length** — `mp->data` points at the slot array. Values are read with
  `memcpy` (see `pax_get_value`), because regions are packed to the byte and a
  slot can be misaligned for its type.
- **by-reference fixed (uuid, interval, …)** — a `scratch` buffer is allocated
  in the descriptor's own context and the value is copied there, so the executor
  is never handed a misaligned address. Allocated only when
  `attlen > 0 && attlen ∉ {1,2,4,8}`.

Layout validation, all of it fatal:

```c
rsize = pax_region_size(page, phdr, n_attrs, i);   /* span to the next region */
used  = pax_region_used(n_tuples, stride);         /* bitmap + n x stride */
if (rsize < used)
    elog(ERROR, "pax: inconsistent page layout for column %d ...");

pax_meta_region_size(page, phdr) must equal n_tuples * 32
```

The span may *exceed* `used`: a region is opened 8-aligned, so the previous
one leaves up to 7 bytes of padding. The span only has to be able to contain
the region.

### 5.4 `pax_meta_ensure` — lazy metadata

The scan does **not** revalidate metadata it has already passed. Since the scan
only moves forward, entries below `meta_checked_through` are never re-read:

```c
if (i < desc->meta_checked_through)
    return;

memcpy(&desc->tuple_meta[i],
       (char *) page + phdr->meta_offset + i * SizeOfPaxTupleMetaData,
       SizeOfPaxTupleMetaData);
pax_validate_tuple_meta(meta, i);

if (i + 1 > desc->meta_checked_through)
    desc->meta_checked_through = i + 1;
```

One 32-byte `memcpy` plus validation per version, each exactly once per scan of
the page. Revalidating all of them on every call would be O(n²) over a page.

`pax_build_page_desc()` — used by the one-shot paths (UPDATE, DELETE, row
locking, single-TID read) — instead calls `pax_refresh_tuple_meta()`, which
validates and copies the whole region at once and sets
`meta_checked_through = n_tuples`.

---

## 6. Visibility

### 6.1 `pax_find_visible_tuple`

```c
for (tupno = start; tupno < pdesc->n_tuples; tupno++)
{
    CHECK_FOR_INTERRUPTS();
    pax_meta_ensure(pdesc, pdesc->page, tupno);

    if (pax_meta_is_unused(&pdesc->tuple_meta[tupno]))
        continue;                    /* VACUUM reclaimed it: it does not exist */

    if (scan->rs_base.rs_snapshot == NULL)
        return tupno;                /* ANALYZE path, see below */

    if (pax_meta_satisfies_snapshot(&pdesc->tuple_meta[tupno],
                                    scan->rs_base.rs_snapshot))
        return tupno;
}
return -1;
```

Two things worth stating:

- **A NULL snapshot means ANALYZE.** `table_beginscan_analyze` passes `NULL`.
  There is no snapshot to filter by, so the scan stops at the first version
  that is not `UNUSED`, and `pax_meta_classify()` decides afterwards whether it
  is live or dead. This is the `SO_TYPE_ANALYZE` case accepted in `scan_begin`.
- **Returning the first match, not the newest.** The scan is physical and
  forward: it returns the first version visible to the snapshot, which is
  correct because only one version of a chain is ever visible at a time.

### 6.2 `pax_meta_satisfies_snapshot`

The single choke point for visibility — the scan, both TID-fetch paths and both
chain followers all go through it, which is why a stale `t_ctid` cannot expose a
reclaimed version.

```c
if (pax_meta_is_unused(meta))                      return false;
if (!pax_meta_xmin_visible(meta, snapshot))         return false;
if (snapshot->snapshot_type == SNAPSHOT_ANY)        return true;
if (!TransactionIdIsValid(meta->xmax))               return true;   /* not deleted */

if (TransactionIdIsCurrentTransactionId(meta->xmax))
    return snapshot->snapshot_type == SNAPSHOT_SELF ? false
                                                    : meta->cmax >= snapshot->curcid;

if (snapshot->snapshot_type == SNAPSHOT_SELF)
    return (TransactionIdIsInProgress(meta->xmax) ||
            !TransactionIdDidCommit(meta->xmax));

if (XidInMVCCSnapshot(meta->xmax, snapshot))         return true;
if (!TransactionIdDidCommit(meta->xmax))             return true;
return false;
```

### 6.3 `pax_meta_xmin_visible`

```c
if (meta->xmin == FrozenTransactionId ||
    meta->xmin == BootstrapTransactionId)    return true;
if (TransactionIdIsCurrentTransactionId(meta->xmin))
    return self_snapshot || meta->cmin < snapshot->curcid;
if (self_snapshot)
    return !TransactionIdIsInProgress(meta->xmin) &&
            TransactionIdDidCommit(meta->xmin);
if (XidInMVCCSnapshot(meta->xmin, snapshot)) return false;
return TransactionIdDidCommit(meta->xmin);
```

`cmin` is what makes a row visible to the command that inserted it and to
later commands in the same transaction, but not to a sibling command — which is
why `meta->cmin < snapshot->curcid` is a strict inequality.

Only `SNAPSHOT_MVCC` and `SNAPSHOT_SELF` are supported; anything else is a hard
error rather than a guess.

---

## 7. Materialization

### 7.1 `pax_store_tuple_slot`

```c
if (slot->tts_flags & TTS_FLAG_SHOULDFREE)
    pax_slot_clear(slot);                 /* ANALYZE does not clear between rows */

pslot->buffer = buffer;
IncrBufferRefCount(buffer);               /* the slot owns its own pin */
pslot->page   = BufferGetPage(buffer);
pslot->pdesc  = pdesc;
pslot->tupno  = tupno;
pslot->owns_pdesc = owns_pdesc;          /* false for the scan: it is cached */
pslot->tuple_meta = pdesc->tuple_meta[tupno];   /* a copy, by value */

slot->tts_flags &= ~TTS_FLAG_EMPTY;
slot->tts_nvalid = 0;                     /* no attribute extracted yet */
ItemPointerSet(&slot->tts_tid, BufferGetBlockNumber(buffer), tupno + 1);
```

Two subtleties, both commented in the code:

- The `TTS_FLAG_SHOULDFREE` reset. A regular scan clears the slot between rows;
  **ANALYZE does not**, it calls this callback again with the flag still set.
  Without the reset, `pax_slot_clear()` would later `pfree()` a pointer that
  aims into a page.
- `owns_pdesc = false`. The scan's descriptor is cached in `scan->cached_desc`
  and outlives the call; freeing it here would be a use-after-free on the next
  row. The slot owns only its pin and its own copy of the version metadata.

### 7.2 `pax_slot_getsomeattrs` and `pax_get_value`

```c
/* a PAX page always carries every column */
for (i = base->tts_nvalid; i < natts; i++)
    base->tts_values[i] = pax_get_value(slot->pdesc, i, slot->tupno,
                                        &base->tts_isnull[i]);
base->tts_nvalid = natts;
```

`pax_get_value` is where a row is actually decoded:

- **NULL check first**, from the per-column bitmap (`att_isnull(tupno, bitmap)`,
  PostgreSQL's convention: bit 0 = NULL).
- **`mp->data == NULL`** (column never written on this page) → NULL.
- **Fixed length**, read by `memcpy` at `mp->data + tupno * stride`, then
  `CharGetDatum` / `Int16GetDatum` / `Int32GetDatum` / `Int64GetDatum` by width.
  `memcpy` is mandatory: regions are packed to the byte, so a slot can be
  misaligned for its type.
- **Other by-reference fixed** → copied into `mp->scratch` and a pointer to that
  returned, never a pointer into the page.
- **Varlena** → a 2-byte `OffsetNumber` is read from the slot; it is an absolute
  page offset, and the returned `Datum` points into the page at `page + off`. An
  invalid offset reads as NULL.

### 7.3 `pax_slot_materialize` — where the cost is

```c
if (TTS_SHOULDFREE(base))
    return;                              /* already detached */

slot_getallattrs(base);                 /* extract EVERY column */

for (i = 0; i < desc->natts; i++)
{
    if (base->tts_isnull[i] || attr->attbyval) continue;
    base->tts_values[i] = datumCopy(base->tts_values[i], false, attr->attlen);
}

base->tts_flags |= TTS_FLAG_SHOULDFREE;
if (slot->owns_pdesc && slot->pdesc) pax_free_page_desc(slot->pdesc);
if (BufferIsValid(slot->buffer)) { ReleaseBuffer(slot->buffer); slot->buffer = InvalidBuffer; }
slot->page = NULL; slot->pdesc = NULL; slot->tupno = -1; slot->owns_pdesc = false;
```

This is the step the scan pays on every row, and it is the AM's largest cost.
Three facts make it expensive:

1. **`slot_getallattrs()` extracts all `natts` columns**, including the ones the
   query never asked for. A `SELECT` of one column out of twenty does the work
   of twenty.
2. **It runs while the content lock is held**, so a writer is blocked for the
   duration.
3. By-value values are copied for free (they are the `Datum`), but every
   by-reference value — including all four text columns here — is `datumCopy`'d
   out of the page, because the page may be memmoved under the slot.

Once materialized, the pin is released: the row no longer depends on the page.
`has_tuple_meta` and `tuple_meta` deliberately survive, so the executor can
still re-check visibility with `pax_tuple_satisfies_snapshot()`.

Measured on 20 000 rows of the 20-column schema (`sql/pax_am.sql:171`),
`EXPLAIN (ANALYZE, TIMING OFF)`, best of five, all four plans plain seq scans.
Both builds are given because the gap between them is larger than most of the
effects being discussed:

| query | cassert (`-O0`) | optimized |
|---|---|---|
| PAX `count(*)`, 20 cols | 24.7 ms | 4.47 ms |
| PAX 1 of 20 columns | 24.1 ms | 4.53 ms |
| PAX all 20 columns | 23.9 ms | 4.40 ms |
| PAX all 2 columns | 9.1 ms | 1.37 ms |
| heap `count(*)`, 20 cols | 3.19 ms | 0.83 ms |
| heap 1 of 20 columns | 3.46 ms | 1.07 ms |
| heap all 20 columns | 2.30 ms | 0.66 ms |
| heap all 2 columns | 2.66 ms | 0.43 ms |

The first three PAX rows are indistinguishable **on both builds**, which is the
eager materialization showing itself: `count(*)` pays for all 20 columns, and a
1-column projection pays for all 20 columns too. That is the structural finding
and it does not depend on the build. The absolute numbers do: the `-O0` build
is ~5x slower, so quoting `25 ms` as "the cost" would overstate it by that
factor. The ratio is the stable figure — PAX runs ~5-7x heap for this scan
either way.

Reading the column slope off the optimized rows: 18 extra columns cost
4.40 - 1.37 = 3.0 ms, so about 0.17 ms per column per 20 000 rows. A projection
that fetched only the columns it was asked for would land near that floor
instead of at the full-table cost.

heap avoids this because a heap tuple never moves, so the executor can keep
by-reference `Datum`s pointing into it and leave `tts_nvalid = 0` — it
materializes only what the query reads, and `count(*)` materializes nothing.
PAX's regions *do* move on insert, so it cannot hand out pointers into the
page; see `analyse1.md` §18.

---

## 8. TID and index paths

### 8.1 `pax_scan_set_tidrange` / `pax_scan_getnextslot_tidrange`

`scan_set_tidrange` seeks: it unpins, drops the cached descriptor, resets to
block 0, and positions `current_block` / `current_tupno` from `mintid`
(`current_tupno = offset - 2`, so the loop restarts one below the bound). An
inverted range sets `tidrange_done` immediately.

`pax_scan_getnextslot_tidrange` is the same loop with an upper-bound test
before the page is read and again after visibility is resolved, because
`pax_find_visible_tuple` may land past `maxtid`. Two rejections: backward
scans, and it is the only path that honours TID bounds at all.

### 8.2 `pax_tuple_tid_valid`

```c
return OffsetNumberIsValid(ItemPointerGetOffsetNumber(tid)) &&
       ItemPointerGetBlockNumber(tid) < RelationGetNumberOfBlocks(scan->rs_rd);
```

Cheap structural check only. The offset is `tupno + 1`, so this is `tupno < n`,
modulo the block bound.

### 8.3 Index scans and index-only scans

An index scan does not go through `scan_getnextslot` at all: nbtree resolves
TIDs and calls `pax_tuple_fetch_row_version()` for each. Index-only scans call
`pax_index_fetch_begin()` then `pax_index_fetch_tuple()`, which reads the single
version at the given TID, checks it with `pax_meta_satisfies_snapshot()`, and
materializes under the content lock.

**No visibility map.** PAX never calls `visibilitymap_set()`, so no page is
ever all-visible, and `nodeIndexOnlyscan` always takes the "dirty" path. Both
callbacks exist and index-only scans work; they just cannot skip the
visibility test. `analyse1.md` explains why the payoff would be one comparison
against a silent-wrong-answer risk in the insert path.

Note that PAX's index entries are per-version: an UPDATE writes a new entry for
the new version, and the old entry still resolves to the old, now-invisible
version. `pax_index_fetch_tuple` never follows `t_ctid`, so it sets
`*call_again = false` unconditionally.

---

## 9. Call graph

```text
table_scan_getnextslot                              nodeSeqscan / nodeIndexscan
  └─ pax_scan_begin                          (once)
  └─ pax_scan_getnextslot
       ├─ ExecClearTuple
       ├─ pax_scan_lock_current_page ........ ReadBuffer once per page
       ├─ pax_page_is_valid
       ├─ pax_scan_page_desc
       │     ├─ pax_layout_is_current ......... cache-validity test
       │     └─ pax_build_page_layout ......... per-column minipages
       ├─ pax_find_visible_tuple
       │     ├─ pax_meta_ensure ............... one 32 B memcpy per version
       │     ├─ pax_meta_is_unused
       │     └─ pax_meta_satisfies_snapshot
       │           ├─ pax_meta_xmin_visible ... cmin, XidInMVCCSnapshot
       │           └─ xmax / cmax checks
       ├─ pax_store_tuple_slot
       ├─ ExecMaterializeSlot
       │     └─ pax_slot_materialize
       │           ├─ slot_getallattrs
       │           │     └─ pax_slot_getsomeattrs
       │           │           └─ pax_get_value .... bitmap, memcpy, varlena offset
       │           └─ datumCopy (by-reference columns only)
       ├─ LockBuffer(BUFFER_LOCK_UNLOCK) ..... pin kept, content lock dropped
       └─ on exhaustion: pax_scan_unpin_current, current_block++
```

---

## 10. Error paths

| condition | error |
|---|---|
| scan type other than seq / TID / TID-range / analyze | `pax table AM supports only sequential and TID table scans` |
| `nkeys != 0` | `pax table AM does not support scan keys` |
| `pscan != NULL` | `pax table AM does not support parallel table scans` |
| `direction` backward | `pax table AM does not support backward scans` / `... backward TID scans` |
| `seqscan` flag absent in `getnextslot` | `pax: non-sequential scan used sequential callback` |
| `natts > tupdesc->natts` | `pax: invalid attribute number %d` |
| page fails `pax_page_is_valid` | `pax: invalid page %u in relation %s` |
| `special->version` / `magic` mismatch | `pax: invalid page version %u` / `pax: invalid page magic %u` |
| `special->n_attrs != natts` | `pax: attribute count mismatch (page %u vs tupdesc %d)` |
| header flags missing | `pax: page lacks required transaction/version metadata` |
| `meta_offset` invalid | `pax: missing transaction metadata region` |
| metadata region ≠ `n_tuples * 32` | `pax: inconsistent transaction metadata region` |
| region span < bitmap + n × stride | `pax: inconsistent page layout for column %d` |
| snapshot type other than MVCC / SELF | `pax table AM does not support snapshot type %d` |
| slot without metadata reaches `tuple_satisfies_snapshot` | `pax tuple slot has no transaction metadata` |

Every layout check is a hard `elog(ERROR)`, never a guess. A columnar page
decoded with the wrong stride yields wrong values silently, so the descriptor
builder refuses rather than repairs.

---

## 11. Locking and concurrency

One lock, held across the whole per-row body:

```
ReadBuffer / cached pin
LockBuffer(buf, BUFFER_LOCK_SHARE)      <- pax_scan_lock_current_page
   ... visibility, materialization ...
LockBuffer(buf, BUFFER_LOCK_UNLOCK)     <- content lock released, pin kept
```

No relation-level lock is taken for reading, and no extension lock. Writers take
`BUFFER_LOCK_EXCLUSIVE` on the same buffer, always buffer-before-relation — the
uniform ordering that §14 of `analyse1.md` records as the fix for a multi-row
UPDATE deadlock.

Two consequences of releasing the content lock while keeping the pin:

- An UPDATE or DELETE of the returned row can take the exclusive lock; the slot
  still holds a pin, so the buffer cannot be evicted out from under it.
- The values are already copied, so the caller is safe if the page changes
  immediately afterwards. The pin outliving the lock is what makes that
  argument hold.

`scan_rescan`, `scan_end` and the page-advance path all call
`pax_scan_unpin_current`, so a scan cannot leak pins across rescan or
completion.

---

## 12. What this costs

Per page: one `ReadBuffer`, one shared content lock per row, one layout
descriptor (rebuilt only if the header changed), one 32-byte metadata `memcpy`
and validation per version, and one `datumCopy` per by-reference column per
row.

| measurement | value |
|---|---|
| pages holding 20 000 rows, 20 cols | 579 (35 rows/page) |
| `ReadBuffer` calls for those rows | 579, one per page, not 20 000 |
| `pax_build_page_layout` calls per page | 1, while the header is unchanged |
| metadata copies per version per scan | exactly 1 |
| 20-col scan, 1-column projection | 4.53 ms (heap 1-col: 1.07 ms) |
| 20-col scan, 20-column projection | 4.40 ms (heap 20-col: 0.66 ms) |
| 2-col scan, whole row | 1.37 ms (heap 2-col: 0.43 ms) |

Optimized build; the `-O0` figures are about 5x higher throughout and are in
§7.3.

The first three rows are the design working. The rest are the problem:
projection width does not change the time — a 1-column projection costs the
same as all 20 — because `pax_slot_materialize()` extracts every column under
the content lock.

The arithmetic for fixing it: a 1-column query currently pays 4.53 ms, and the
per-column slope says the width it actually wants costs about 1.4 ms. That is
~70% of the scan time on the optimized build (~60% on `-O0`, from the same
arithmetic). It needs letting `pax_slot_getsomeattrs()` fetch on demand and
keeping by-reference `Datum`s pointing into the page instead of copying them
out — which needs row addresses that do not move. They do not, in v4, and
that is the trade §18 of `analyse1.md` documents: the chunked v5 made them
stable and paid for it in page density.

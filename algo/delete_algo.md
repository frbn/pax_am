# PAX — algorithm of a DELETE

Page format **v6** (`PAX_PAGE_VERSION 6`). This document follows one row from
the executor to the bits that change, then follows the space it gave up as far
as it actually goes. It is the write-side companion to
[`select_algo.md`](select_algo.md) and [`insert_algo.md`](insert_algo.md). Line
references are to `pax_am.c` at the commit where this file was written; the code
is the authority if they drift.

---

## 1. The shape of a DELETE in PAX, in one paragraph

PAX deletes by **marking, in place**. `tuple_delete` writes `xmax` = the current
transaction, `cmax` = the current command, and `t_ctid` = the row's own TID
into that row's 32-byte metadata record, all inside one Generic WAL record. No
new version is appended, no page is memmoved, and **the relation does not shrink
by a single byte**. The row stops being visible immediately, because visibility
already consults `xmax`. The bytes it occupied — 32 bytes of metadata plus its
variable-length payloads — are still there, and stay there until a VACUUM marks
the slot `UNUSED` and compacts the payloads away. That is the whole difference
from heap, and it is the reason this document has two halves.

| stage | what happens | relation size |
|---|---|---|
| `DELETE` | `xmax`, `cmax`, `t_ctid := self`, WAL-logged | **unchanged** |
| `VACUUM` | mark `UNUSED`, compact payloads, update FSM | unchanged, except trailing pages are truncated |
| later `INSERT` | reuses the `UNUSED` slots, guided by the FSM | grows only if no slot fits |

## 2. Entry point

The executor reaches PAX through one inline:

```c
static inline TM_Result
table_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
                   uint32 options, Snapshot snapshot, Snapshot crosscheck,
                   bool wait, TM_FailureData *tmfd)
{
    return rel->rd_tableam->tuple_delete(rel, tid, cid, options,
                                         snapshot, crosscheck,
                                         wait, tmfd);
}
```

`access/tableam.h`, verbatim. `.tuple_delete = pax_tuple_delete`.

A `DELETE` is therefore **two** AM calls, at two different times:

| when | callback | what it does |
|---|---|---|
| statement time | `pax_tuple_delete` (5037) | mark the row dead |
| VACUUM time | `pax_relation_index_delete_tuples` (887) | let index AMs drop the entries |

There is no `tuple_delete_speculative`; the executor's speculative-insert path
is rejected outright by `tuple_insert_speculative`. Row-level security and
`ON DELETE` triggers are handled above the AM. Partition-row moves are rejected
(see §3).

---

## 3. `pax_tuple_delete` — the whole function

```c
if (IsInParallelMode())
    ereport(ERROR, ... "cannot delete tuples during a parallel operation");
if (cid == InvalidCommandId)
    elog(ERROR, "pax: invalid DELETE command ID");
if (!wait)
    ereport(ERROR, ... "does not support non-waiting DELETE");
if (options & TABLE_DELETE_CHANGING_PARTITION)
    ereport(ERROR, ... "does not support partition-row moves");

(void) snapshot;
xid = GetCurrentTransactionId();
tmfd->traversed = false;
result = pax_lock_version_for_dml(rel, tid, cid, crosscheck, wait,
                                  XLTW_Delete, tmfd, &buf, &pdesc,
                                  &meta, &self_tid, &have_tuplock);
if (result == TM_Invisible)
    ereport(ERROR, ... "attempted to delete invisible tuple");
if (result != TM_Ok)
    return result;

CheckForSerializableConflictIn(rel, tid,
                               ItemPointerGetBlockNumber(&self_tid));

meta.xmax = xid;
meta.cmax = cid;
ItemPointerCopy(&self_tid, &meta.t_ctid);
wal_state = GenericXLogStart(rel);
page = GenericXLogRegisterBuffer(wal_state, buf, 0);
*pax_page_tuple_meta(page, ItemPointerGetOffsetNumber(&self_tid) - 1) = meta;
GenericXLogFinish(wal_state);

pax_free_page_desc(pdesc);
UnlockReleaseBuffer(buf);
if (have_tuplock)
    UnlockTuple(rel, &self_tid, AccessExclusiveLock);

return TM_Ok;
```

Four things are load-bearing.

**`(void) snapshot;` — the snapshot argument is ignored.** PAX decides whether
the row can be deleted using `cid` and the version's own `xmin`/`xmax`, not the
caller's snapshot. `crosscheck` *is* used, and only for foreign-key checks
(§4.6).

**Exactly one row is written, and it is 32 bytes.** `pax_page_tuple_meta` (4584)
resolves to the row's record inside the page's metadata region:

```c
static PaxTupleMetaData *
pax_page_tuple_meta(Page page, int tupno)
{
    PaxPageHeader *phdr = PaxPageHeaderPtr(page);
    Assert(tupno >= 0 && tupno < phdr->n_tuples);
    Assert(PaxOffsetIsValid(phdr->meta_offset));
    return ((PaxTupleMetaData *) ((char *) page + phdr->meta_offset)) + tupno;
}
```

Nothing else on the page moves. That is why no descriptor invalidation, no
`offsets[]` fixup, and no inbound-`t_ctid` repair is needed — the properties the
INSERT path has to work hard to preserve are free here, precisely because DELETE
does not move anything.

**`t_ctid := self_tid` marks the row a chain leaf.** `self_tid` is the row's own
TID, `(blkno, offset)`, as returned by `pax_lock_version_for_dml`. Writing it
means a chain walk that arrives here stops, and — because
`ItemPointerEquals(self_tid, &meta->t_ctid)` is how `pax_classify_version`
distinguishes *deleted* from *updated* — a later DELETE of the same row reports
`TM_Deleted` rather than following a chain that does not exist.

**The write is one Generic WAL record, under the content lock already held.**
`pax_lock_version_for_dml` returns with `BUFFER_LOCK_EXCLUSIVE` on `buf` still
held; `GenericXLogRegisterBuffer` then hands back the page and
`GenericXLogFinish` commits the record before the lock is dropped. A crash
therefore replays the marking atomically — the row is either dead or alive, and
`xmax`/`cmax`/`t_ctid` are never seen half-written.

---

## 4. `pax_lock_version_for_dml` — finding and judging the version

This is where the real work is, and it is shared verbatim with UPDATE (only
`wait_op` differs). Its contract: **acquire the heavyweight tuple lock, then
return the exact physical version with the page content lock held.**

```c
have_lock = pax_acquire_dml_tuplock(rel, tid, wait);
if (!have_lock) { ...; return TM_BeingModified; }

for (;;)
{
    blkno   = ItemPointerGetBlockNumber(tid);
    offset  = ItemPointerGetOffsetNumber(tid);
    if (blkno >= RelationGetNumberOfBlocks(rel) || !OffsetNumberIsValid(offset))
        elog(ERROR, "pax: tuple identifier outside relation \"%s\"", ...);

    buf = ReadBuffer(rel, blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    if (!pax_page_is_valid(page)) { UnlockReleaseBuffer(buf); elog(ERROR, ...); }

    desc  = pax_build_page_desc(page, RelationGetDescr(rel));
    tupno = (int) offset - 1;
    if (tupno >= desc->n_tuples) { ...; elog(ERROR, "pax: tuple offset does not exist in relation \"%s\"", ...); }

    *meta = desc->tuple_meta[tupno];
    ItemPointerSet(self_tid, blkno, offset);
    status = pax_classify_version(meta, self_tid, cid);
    ...
}
```

### 4.1 Why the heavyweight lock comes first

```c
static bool
pax_acquire_dml_tuplock(Relation rel, const ItemPointerData *tid, bool wait)
{
    if (wait) { LockTuple(rel, tid, AccessExclusiveLock); return true; }
    return ConditionalLockTuple(rel, tid, AccessExclusiveLock, false);
}
```

The loop below *drops the content lock* to wait for an outdating transaction
(§4.5). Without the heavyweight lock held across that gap, two writers could
both see a live row, both drop the content lock, both wake, and both write
`xmax` — last writer wins, first one's DELETE silently vanishes. `LockTuple` is
taken **before** the first `ReadBuffer`, and released only after the WAL record
is finished, so the window does not exist.

Lock order is buffer-after-relation-lock, the same uniform order the UPDATE path
uses; the multi-row UPDATE deadlock fixed in `analyse1.md` §14 was an ordering
violation of exactly this.

### 4.2 The descriptor is rebuilt for every row

```c
static PaxPageDesc *
pax_build_page_desc(Page page, TupleDesc tupdesc)
{
    PaxPageDesc *desc;

    desc = pax_build_page_layout(page, InvalidBlockNumber, tupdesc);
    pax_refresh_tuple_meta(desc, page);
    return desc;
}
```

Unlike the sequential scan, which caches its descriptor in `scan->cached_desc`
and reuses it while the page header is unchanged (`pax_layout_is_current`), the
DML path builds a fresh one per row: `palloc` the descriptor, `palloc` and copy
the `offsets[]` stamp, build one `PaxMinipage` per attribute, then
`pax_refresh_tuple_meta`, which copies **the entire metadata region** —
`n_tuples * 32` bytes — and runs `pax_validate_tuple_meta` on every one of them.

That is O(n_tuples + n_attrs) of work to change 32 bytes. Measured in §7, it is
*not* the dominant cost, but it is the obvious first thing to fix.

### 4.3 `pax_classify_version` — the seven-way verdict

```c
if (pax_meta_is_unused(meta))
    return PaxVersionDeleted;                     /* VACUUM reclaimed it */

if (TransactionIdIsCurrentTransactionId(meta->xmin)) {
    if (meta->cmin >= cid) return PaxVersionInvisible;
} else if (meta->xmin != FrozenTransactionId &&
          meta->xmin != BootstrapTransactionId &&
          (TransactionIdIsInProgress(meta->xmin) ||
           !TransactionIdDidCommit(meta->xmin)))
    return PaxVersionInvisible;

if (!TransactionIdIsValid(meta->xmax)) return PaxVersionLive;

if (TransactionIdIsCurrentTransactionId(meta->xmax))
    return meta->cmax >= cid ? PaxVersionSelfModified : PaxVersionInvisible;

if (TransactionIdIsInProgress(meta->xmax)) return PaxVersionInProgress;

if (TransactionIdDidCommit(meta->xmax))
    return ItemPointerEquals(self_tid, &meta->t_ctid) ?
        PaxVersionDeleted : PaxVersionUpdated;

/* The outdating transaction aborted; this physical version is live. */
return PaxVersionLive;
```

The `UNUSED` check comes first, and deliberately: a reclaimed version is reported
as *deleted*, so a chain walk stops at it instead of following its `t_ctid`. Its
offset stays valid, so a stale inbound link resolves to this version and to
nothing beyond. A DELETE against a VACUUMed row is therefore a clean
`TM_Deleted`, not a corruption.

Note the third-from-last line: an **aborted** outdating transaction leaves the
version `PaxVersionLive`. That is how a rolled-back `DELETE` frees its row again
with no repair pass.

### 4.4 Row locks can force `xmax` early

PAX stores row locks in a MultiXact (`locker_mxid`), never in `xmax`. Before
accepting a live version, the function checks for a conflicting live lock:

```c
if (status == PaxVersionLive) {
    pax_get_locker_state(meta, LockTupleExclusive, &locker_state);
    if (TransactionIdIsValid(locker_state.conflict_xid)) {
        if (wait) { ...wait on it...; continue; }
        meta->xmax = locker_state.conflict_xid;
        status = PaxVersionInProgress;
    }
}
```

`pax_get_locker_state` (4665) walks the MultiXact members and reports the
strongest mode held by the current transaction plus, for *other* in-progress
members, the first one whose mode conflicts. Conflict is decided by
`pax_lock_modes_conflict` (4621), a four-case table over `KeyShare` / `Share` /
`NoKeyExclusive` / `Exclusive`. If `wait` is false — which `pax_tuple_delete`
has already rejected, so this is reachable only from UPDATE — `xmax` is set to
the conflicting xid and the version reported in-progress, which is how heap
signals a would-be deadlock.

### 4.5 Waiting

```c
if (status == PaxVersionInProgress && wait) {
    TransactionId xwait = meta->xmax;
    pax_free_page_desc(desc);
    UnlockReleaseBuffer(buf);
    XactLockTableWait(xwait, rel, self_tid, wait_op);
    continue;
}
```

The content lock **is** dropped before waiting — otherwise the outdating
transaction could never make progress and the wait would deadlock against
itself. This is exactly why the heavyweight lock was taken first (§4.1). On
wake-up the loop re-reads the page from scratch and re-classifies; if the
outdating transaction rolled back, the row is live again and the DELETE proceeds.

### 4.6 `crosscheck` and failure reporting

```c
if (status == PaxVersionLive &&
    crosscheck != InvalidSnapshot &&
    !pax_meta_satisfies_snapshot(meta, crosscheck))
    status = PaxVersionUpdated;
```

`crosscheck` is the RI visibility check. A row invisible to it is reported as
`TM_Updated` so the caller raises a serialization failure rather than deleting
something a foreign key still points at.

Every non-OK outcome goes through one table:

```c
switch (status) {
    case PaxVersionInvisible:   result = TM_Invisible;      break;
    case PaxVersionSelfModified:result = TM_SelfModified;   break;
    case PaxVersionInProgress:  result = TM_BeingModified;  break;
    case PaxVersionUpdated:     result = TM_Updated;        break;
    case PaxVersionDeleted:     result = TM_Deleted;        break;
    case PaxVersionLive:        pg_unreachable();
}
pax_fill_failure_data(tmfd, meta, result, false);
```

and `pax_fill_failure_data` (4847) hands back the row's own `t_ctid`, `xmax`,
`cmax`, and `traversed = false` — PAX never reports chain traversal, because
DELETE follows no chain: it judges the exact version the TID names.

---

## 5. The space, later: `pax_relation_vacuum`

`pax_relation_vacuum` (1495) is a four-step pass over the whole relation.

```c
dead_items = TidStoreCreateLocal((Size) vac_work_mem * 1024, true);
nblocks = RelationGetNumberOfBlocks(rel);

for (blkno = 0; blkno < nblocks; blkno++) {
    buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, strat);
    (void) pax_vacuum_page(rel, buf, blkno, params,
                           &tups_vacuumed, &tups_recently_dead,
                           &all_unused, dead_items);
    if (all_unused) nkept = blkno;       /* trailing empty run */
    else            nkept = blkno + 1;   /* this page stays used */
}
```

The `TidStore` is PAX's own. It exists because `vacuumlazy.c` builds heap's
internally and exposes no table-AM callback to fill it — without a file of dead
TIDs, no index entry can ever be removed.

### 5.1 `pax_vacuum_page` — marking `UNUSED`

Under `BUFFER_LOCK_EXCLUSIVE`, with one Generic WAL record covering both the
marking and the compaction:

```c
for (i = 0; i < phdr->n_tuples; i++) {
    PaxTupleMetaData *m = &meta[i];
    if (pax_meta_is_unused(m)) continue;

    switch (pax_meta_classify(m)) {
        case PAX_VERSION_RECENT: all_unused = false; n_recent++; continue;
        case PAX_VERSION_LIVE:
            all_unused = false;
            if (freezing && TransactionIdIsValid(m->xmin) &&
                m->xmin != FrozenTransactionId && TransactionIdDidCommit(m->xmin) &&
                TransactionIdPrecedes(m->xmin, oldest))
            { m->xmin = FrozenTransactionId; changed = true; }
            continue;
        case PAX_VERSION_DEAD: break;
    }

    if (pax_meta_has_live_locker(m)) { all_unused = false; n_recent++; continue; }
    if (m->locker_mxid != InvalidMultiXactId)
        m->locker_mxid = InvalidMultiXactId;     /* drop the MultiXact ref */
    m->flags |= PAX_VERSION_UNUSED;
    changed = true;
    dead_offsets[n_dead] = (OffsetNumber) (i + 1);
    n_dead++;
}
```

`pax_meta_classify` (462) is a *different* classifier from
`pax_classify_version`, and deliberately so: it has no `cid` and no current
transaction, so it answers "dead for everyone?" — `xmin` aborted means the row
never existed; `xmax` committed means it is gone. A `PAX_VERSION_RECENT` version
is one whose fate is still undecided, and it is left alone.

**A live row lock blocks reclamation** (`pax_meta_has_live_locker`, 492): a
reader that took `SELECT ... FOR SHARE` and followed the chain from this row
could still be walking it, so the slot is not reused. `MultiXactIdIsRunning(...,
isLockOnly = true)` is used rather than `GetMultiXactIdMembers()` because the
latter's array points into the MultiXact cache and would cost an SLRU access.

The dead TIDs are handed to the `TidStore` **after** the buffer is released —
the list is only needed by the index pass, which re-reads it under its own locks.

### 5.2 Why versions are never physically removed

```c
/*
 * Contrainte propre au format : les liens t_ctid sont des offsets PHYSIQUES
 * (page, indice de version). Retirer une version décalerait tous les slots
 * suivants de toutes les colonnes, et casserait les liens entrants pointant
 * sur cette page depuis d'autres. On ne compacte donc pas les versions [...]
 */
```

This is the load-bearing constraint of the whole format. A version index is a
logical `(block, tupno)` pair, and `t_ctid` stores it verbatim. Removing version
*i* would shift every later version on that page down by one **in every column
region**, invalidating `t_ctid` links that arrive from other pages. So a dead
version is flagged `UNUSED` in place: the slot stays, the bytes stay, and
`INSERT` may reuse the slot later.

### 5.3 `pax_vacuum_compact_payload` — the bytes that do come back

The metadata is only 32 bytes per row. The real space is in the
variable-length payloads, which live in the `pd_upper` zone and are *not* part of
any region, so they can move without invalidating anything.

```c
for (c = 0; c < tupdesc->natts; c++) {
    if (attr->attlen >= 0) continue;                  /* inline in the region */
    if (!PaxOffsetIsValid(phdr->offsets[c])) continue; /* column absent */

    for (t = 0; t < n_tuples; t++) {
        if (pax_meta_is_unused(&meta[t])) continue;   /* nobody reads it now */
        if (att_isnull(t, bmp)) continue;
        memcpy(&off, ...offset table..., sizeof(OffsetNumber));
        if (!PaxOffsetIsValid(off)) continue;
        len = (attr->attlen == -2) ? strlen(...) + 1 : VARSIZE_ANY(...);
        refs[n_refs] = (PaxPayloadRef) { off, 0, MAXALIGN(len), c, t };
        n_refs++;
    }
}

qsort(refs, n_refs, sizeof(PaxPayloadRef), pax_payload_ref_cmp);  /* high → low */

cursor = top;                                       /* BLCKSZ - SizeOfPaxSpecialData */
for (i = 0; i < n_refs; i++) {
    cursor -= refs[i].len;
    refs[i].new_off = (OffsetNumber) cursor;
    if (refs[i].new_off != refs[i].old_off)
        memmove((char *) page + cursor, (char *) page + refs[i].old_off, refs[i].len);
}
/* then rewrite each column's offset table entry in place */
...
memset((char *) page + old_upper, 0, cursor - old_upper);
((PageHeader) page)->pd_upper = (LocationIndex) cursor;
```

**Descending order is what makes it safe.** `pax_payload_ref_cmp` (1078) sorts
highest offset first, so every destination is at or below its source and no
not-yet-copied source can be overwritten.

Three invariants make it legal, and each is a reason the scan stays correct:

- `n_tuples` is unchanged, so no version index moves and every `t_ctid` — including
  inbound links from other pages — stays exact.
- The column offset tables live inside the regions, below `pd_lower`. They are
  not shifted; the new offset is written into the existing entry.
- A scan re-reads the offset table at every materialization and does so under
  the content lock, so it cannot observe a half-finished move. Its cached page
  descriptor stays valid too: neither the region offsets nor `meta_offset` moved.

The vacated bytes are `memset` to zero rather than left stale, so a raw page dump
does not resurrect dead values.

This runs even when no version was marked this pass, because an earlier pass may
have left dead payloads behind. It is what makes reclaimed space *usable*: slot
reuse needs contiguous free space, and a page full of dead rows has almost none
otherwise — measured at 52 bytes free on average in `analyse1.md`, well under
what a new row needs.

### 5.4 Truncation of trailing empty pages

Only a **trailing** run of all-`UNUSED` pages can be removed; a dead page
followed by a live one must stay, because the file is a sequence.

```c
if (nkept < nblocks) {
    if (!ConditionalLockRelation(rel, AccessExclusiveLock)) { ...; goto out; }
    while (new_rel_pages > 0) {                       /* re-verify from the end */
        buf = ReadBufferExtended(rel, MAIN_FORKNUM, new_rel_pages - 1, RBM_NORMAL, NULL);
        ...
        if (!empty) break;
        new_rel_pages--;
    }
    if (new_rel_pages < RelationGetNumberOfBlocks(rel)) {
        for (b = new_rel_pages; b < nblocks; b++) RecordPageWithFreeSpace(rel, b, 0);
        RelationTruncate(rel, new_rel_pages);
    }
    UnlockRelation(rel, AccessExclusiveLock);
}
```

The forward `nkept` is only a rough bound; the backward re-verification is what
makes the cut exact, because between the marking pass and the truncation an
inserter may have added rows. `AccessExclusiveLock` is required for the same
reason heap requires it. Dropped pages get their FSM entries zeroed first, since
those blocks no longer exist.

Measured: deleting the trailing half of a 34-page table and vacuuming gives 17
pages. Deleting the *leading* half leaves all 34, correctly — page 16 is live.

### 5.5 FSM

```c
out:
    for (blkno = 0; blkno < RelationGetNumberOfBlocks(rel); blkno++) {
        ...
        free_space = pax_page_free_space(BufferGetPage(buf));
        ...
        RecordPageWithFreeSpace(rel, blkno, free_space);
    }
```

with `pax_page_free_space` being simply `pd_upper - pd_lower`. This is what makes
the reclaimed slots findable: without it the map still describes the pages as
full and INSERT never looks at them. Measured, from a fresh backend so only the
FSM can steer the insert: after UPDATE + VACUUM, absorbing 3500 new rows cost
**+3 pages** where an equivalent never-vacuumed table needed **+106**
(`analyse1.md`).

### 5.6 Index cleanup — `pax_vacuum_indexes`

```c
if (tups_vacuumed > 0 && (params->options & VACOPT_SKIP_LOCKED) == 0)
    pax_vacuum_indexes(rel, dead_items, strat);
```

Order matters: the table is cleaned first, so that when the index AM asks "is
this TID dead?", the answer already exists. Each index is opened with an
explicit `AccessShareLock` — `index_open(oid, NoLock)` would run nbtree's
deletion pass while mutating shared buffers with no lock at all, which the
cassert build catches immediately.

Two callbacks serve the index AMs, and they are different code paths:

| path | callback | mechanism |
|---|---|---|
| `index_bulk_delete` walk | `pax_vac_tid_reaped` (1432) | membership test in the `TidStore` |
| `index_delete_tuples` | `pax_relation_index_delete_tuples` (887) | reads the table page |

```c
static bool
pax_vac_tid_reaped(ItemPointer itemptr, void *state)
{
    return TidStoreIsMember((TidStore *) state, itemptr);
}
```

`pax_relation_index_delete_tuples` is the table-AM side of the same question. It
sorts by block so each page is read once, and for each TID:

```c
deletable = pax_meta_is_unused(&meta) ||
    (pax_meta_classify(&meta) == PAX_VERSION_DEAD &&
     !pax_meta_has_live_locker(&meta));
if (istatus->knowndeletable) deletable = true;
if (!deletable) break;              /* the rest of the array goes untouched */
pax_meta_advance_conflict_horizon(&meta, &snapshotConflictHorizon);
finalndeltids = i + 1;
...
delstate->ndeltids = finalndeltids;  /* shrink: not just an optimization */
```

The `knowndeletable` short-circuit comes from the index AM's own scan. Shrinking
`ndeltids` is load-bearing, not a nicety: the index AM is allowed to conclude
there is nothing to do from it.

**Bottom-up deletion is declined outright:**

```c
if (delstate->bottomup) {
    delstate->ndeltids = 0;
    return InvalidTransactionId;
}
```

nbtree asks the table AM to speculatively consider every entry on an index page
to avoid a fillfactor stall. It is an optimization, not an obligation, and heap's
cost model for it rests on MVCC visibility and page free space — two notions PAX
does not have. Guessing would be dishonest, so PAX declines and nbtree simply
does not progress on that pass. The same reasoning appears in
`analyse1.md` §13.

---

## 6. What a concurrent reader sees

`pax_meta_satisfies_snapshot` (3259) is the single choke point, and DELETE is
visible through it without any extra mechanism:

```c
if (pax_meta_is_unused(meta))              return false;
if (!pax_meta_xmin_visible(meta, snapshot)) return false;
if (snapshot->snapshot_type == SNAPSHOT_ANY) return true;
if (!TransactionIdIsValid(meta->xmax))       return true;   /* not deleted */
if (TransactionIdIsCurrentTransactionId(meta->xmax))
    return snapshot->snapshot_type == SNAPSHOT_SELF ? false
                                                    : meta->cmax >= snapshot->curcid;
...
```

An in-flight DELETE is invisible to everyone but its own transaction, which sees
it via `cmax`. A committed DELETE is invisible to everyone. An aborted one leaves
`xmax` pointing at a transaction that did not commit, and the final
`if (!TransactionIdDidCommit(meta->xmax)) return true;` makes the row visible
again. No repair pass is involved.

Measured, index included: after `DELETE FROM ix WHERE k = 42` with **no** vacuum,
`SELECT count(*) FROM ix WHERE k = 42` returns 0 and still uses an Index Only
Scan on `ix_k`. The index entry is physically present until VACUUM; the table AM
is what makes the row disappear.

---

## 7. What it costs

All figures: 20 000 rows of the 20-column schema (`sql/pax_am.sql:171`), best of
five, `EXPLAIN (ANALYZE, TIMING OFF)`, `max_parallel_workers_per_gather = 0`.

| operation | optimized | cassert (`-O0`) |
|---|---|---|
| PAX `DELETE` 10 000 rows, 1 column | 35.6 ms | 146.5 ms |
| PAX `DELETE` 10 000 rows, 2 columns | 34.3 ms | 141.3 ms |
| PAX `DELETE` 10 000 rows, 20 columns | 35.5 ms | 148.6 ms |
| heap `DELETE` 10 000 rows, 20 columns | **3.29 ms** | 13.4 ms |
| PAX `VACUUM`, 1 column | 9 ms | 18 ms |
| PAX `VACUUM`, 2 columns | 9 ms | 18 ms |
| PAX `VACUUM`, 20 columns | 13 ms | 33 ms |
| heap `VACUUM`, 20 columns | **1 ms** | 3 ms |
| relation size after `DELETE` | unchanged | unchanged |

**PAX DELETE is ~11x heap, and the cost is per row, not per page or per column.**
The three PAX rows are flat across 1, 2 and 20 columns *and* across tables
holding 224, 148 and 34 rows per page — a 6.6x spread in `n_tuples` that moves
the time by 4%. That rules out `pax_refresh_tuple_meta`, which copies
`n_tuples * 32` bytes and validates every entry per deleted row: if it dominated,
the 224-rows-per-page table would be several times slower than the
34-rows-per-page one. It is still the obvious first optimization (§4.2), just not
the current bottleneck.

What remains per row, all visible in the source: `LockTuple` + `UnlockTuple`
(two heavyweight-lock round trips, taken unconditionally — §4.1), one
`ReadBuffer` plus exclusive content lock, and one Generic WAL record. **I could
not measure the split between them**: no sampling profiler was available
(`perf` is not installed), and no gdb-based sampling run was performed, so no
profile was taken and no attribution is claimed beyond the column/page-flatness
result above. `gdb` itself does attach to a PAX backend in the current
environment; the limitation is the absent profiler, not ptrace.

VACUUM is ~10x heap on the same data. It touches every page, rebuilds nothing but
does compact every payload, and it writes a Generic WAL record per changed page.

---

## 8. Call graph

```text
table_tuple_delete                                        access/tableam.h
  └─ pax_tuple_delete                                     (5037)
       ├─ IsInParallelMode / option checks ................ §3 rejections
       ├─ GetCurrentTransactionId
       ├─ pax_lock_version_for_dml ......................... (4895)
       │    ├─ pax_acquire_dml_tuplock .................... (4877) LockTuple
       │    └─ loop:
       │         ├─ ReadBuffer + LockBuffer(EXCLUSIVE)
       │         ├─ pax_page_is_valid
       │         ├─ pax_build_page_desc ................... (2695)  rebuilt per row
       │         │    ├─ pax_build_page_layout ............. (2399)  minipages
       │         │    └─ pax_refresh_tuple_meta ........... (2651)  n_tuples x 32 B
       │         ├─ pax_classify_version .................. (4806)
       │         ├─ pax_get_locker_state .................. (4665)  MultiXact
       │         │    └─ pax_lock_modes_conflict .......... (4621)
       │         ├─ pax_meta_satisfies_snapshot ........... (3259)  crosscheck only
       │         ├─ XactLockTableWait .................... drops content lock
       │         └─ pax_fill_failure_data ................. (4847)
       ├─ CheckForSerializableConflictIn
       ├─ meta.xmax/cmax/t_ctid := ... .................... 32 bytes
       ├─ GenericXLogStart / RegisterBuffer / Finish
       ├─ pax_page_tuple_meta ............................. (4584)
       ├─ pax_free_page_desc
       └─ UnlockReleaseBuffer + UnlockTuple

VACUUM
  └─ pax_relation_vacuum .................................. (1495)
       ├─ TidStoreCreateLocal
       ├─ per page: pax_vacuum_page ....................... (1273)
       │    ├─ pax_meta_is_unused / pax_meta_classify .... (448 / 462)
       │    ├─ pax_meta_has_live_locker .................. (496)
       │    ├─ pax_vacuum_compact_payload ................. (1124)
       │    │    └─ pax_payload_ref_cmp .................. (1078)  high -> low
       │    └─ TidStoreSetBlockOffsets
       ├─ trailing-run truncation + RelationTruncate
       ├─ FSM: pax_page_free_space + RecordPageWithFreeSpace (3063)
       └─ pax_vacuum_indexes ............................. (1446)
            └─ per index: index_bulk_delete(pax_vac_tid_reaped) (1432)
                        index_vacuum_cleanup
  bottom-up path:
  └─ pax_relation_index_delete_tuples ..................... (887)
       └─ pax_index_delete_tuples ........................ (1015)
```

---

## 9. Error paths

| condition | error |
|---|---|
| `IsInParallelMode()` | `cannot delete tuples during a parallel operation` |
| `cid == InvalidCommandId` | `pax: invalid DELETE command ID` |
| `wait == false` | `pax table AM does not support non-waiting DELETE` |
| `TABLE_DELETE_CHANGING_PARTITION` | `pax table AM does not support partition-row moves` |
| `pax_lock_version_for_dml` gets an invalid TID | `pax: invalid tuple identifier for UPDATE/DELETE` |
| block past EOF, or bad offset | `pax: tuple identifier outside relation "%s"` |
| page fails `pax_page_is_valid` | `pax: invalid page %u in relation "%s"` |
| `tupno >= n_tuples` | `pax: tuple offset does not exist in relation "%s"` |
| `TM_Invisible` returned to the caller | `attempted to delete invisible tuple` |
| index entry points at a nonexistent slot | `index "%s" contains an entry pointing to an invalid tuple (%u,%u) in table "%s"`, hint `The index should be dropped and recreated.` |
| page invalid during vacuum | `pax: invalid page %u while vacuuming` |

Note the asymmetry in the last-but-one row: an index entry pointing past EOF
after a truncate is tolerated and merely left for the next pass, but an entry
pointing at a *nonexistent slot on an existing page* is treated as corruption
and raised, which heap does not do.

`TM_Invisible` is the one case escalated from a return value to an `ERROR`: the
executor asked to delete a row it should have filtered out, and silently
returning would hide a planner or visibility bug.

---

## 10. Locking and concurrency

Three locks, in a fixed order:

```
LockTuple(AccessExclusiveLock)         heavyweight, taken FIRST
  ReadBuffer + LockBuffer(EXCLUSIVE)   content lock, dropped only to wait
    ... classify, write 32 bytes, GenericXLogFinish ...
  UnlockReleaseBuffer
UnlockTuple(AccessExclusiveLock)
```

- **Buffer after relation, always.** The same order UPDATE uses; violating it is
  what wedged the backend on a multi-row UPDATE with an index
  (`analyse1.md` §14).
- **The content lock is released while waiting** for an outdating xact, and only
  while waiting. That is the one window in which another writer could touch the
  row, and the heavyweight lock is what closes it.
- **A concurrent scan is unaffected.** It holds a *shared* content lock, so it
  either sees the row before the DELETE's WAL record finishes or not at all. It
  never sees a half-written `xmax`: the 32 bytes land in one record.
- **A row-level `SELECT ... FOR UPDATE` is honoured, not ignored.** `xmax` is
  never used to store a lock, so a lock-only MultiXact member cannot be mistaken
  for a deletion by `pax_meta_classify` — the reason the two classifiers in §4.3
  and §5.1 are separate functions.

---

## 11. Limitations, stated plainly

- **A DELETE returns no space.** Not one byte, until VACUUM. Measured in §7.
- **Index entries outlive the DELETE** until VACUUM removes them. The row is
  invisible in the meantime, so results are correct — but the index is larger
  than it needs to be, and an index-only scan visits entries that resolve to
  nothing.
- **Dead versions are never compacted out**, by design (§5.2). Long-running
  update/delete churn on a PAX table grows the file monotonically except for
  trailing-page truncation.
- **Per-row cost is ~11x heap** and flat in width, which is the number to
  improve first (§7).
- **Bottom-up index deletion is declined** (§5.6), so index vacuuming can make
  less progress per pass than on a heap table.
- **A DELETE is never parallelized** — the planner does not choose a parallel
  plan for it. Note the contrast with `SELECT`: on a 61 157-page PAX table the
  planner *does* choose a 4-worker parallel plan for `count(*)`, and executing it
  fails outright with `pax table AM does not support parallel table scans`. Any
  sufficiently large PAX table is exposed to that; `max_parallel_workers_per_gather
  = 0` or `ALTER TABLE ... SET (parallel_workers = 0)` avoids it.
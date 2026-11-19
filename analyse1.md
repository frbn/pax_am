# Analyse 1 — PAX table AM prototype: versioned DML state

**Date:** 2026-09-25  
**Target:** PostgreSQL 19devel  
**Main implementation:** `pax_am.c`

> This document supersedes the earlier insert-MVCC review. It describes the
> current working tree, including page version 4, append-only row versions,
> UPDATE/DELETE, version-chain traversal and transaction-scoped row locks.
>
> **Page format 5 (the chunked regions of §17 and §18) has been reverted out.**
> Chunks made a row's address immutable, which is what lazy materialization
> would need, but they cost density and complexity and the payoff was never
> built. §17 and §18 are kept below as the record of what was tried, what it
> cost, and what it would have taken to bring it back.

## 1. Executive assessment

PAX is now a working sequential table AM for controlled INSERT, UPDATE, DELETE,
and SELECT workloads on PostgreSQL 19devel. It preserves the real columnar page
format rather than emulating heap tuples in a private file.

Implemented and tested:

- extension registration, CREATE/DROP/TRUNCATE, sizing, and planner estimates;
- real fixed/varlena/NULL columnar storage with multi-page packing;
- insert MVCC using the current subtransaction XID and executor command ID;
- append-only UPDATE versions and metadata-only DELETE versions;
- MVCC visibility, system columns, exact-version fetch, and latest-TID traversal;
- `TM_Result` conflict handling for self, concurrent, committed, and aborted
  UPDATE/DELETE states;
- READ COMMITTED wait/retry behavior through executor EPQ;
- KeyShare/Share/NoKeyUpdate/Update row locks stored in a separate MultiXact
  roster;
- repeatable-read historical views across committed UPDATE and DELETE;
- rollback of top-level transactions and savepoints;
- a page-extension lock around legacy PG19 `ReadBuffer(..., P_NEW)`;
- contiguous per-column regions, moved on insert with `offsets[]` corrected
  behind them (§3);
- eager slot materialization while the PAX page content lock is held.

The implementation is still not production-safe. Generic WAL protects every
physical page mutation and VACUUM/ANALYZE now exist, including in-place reuse of
vacuumed slots, but indexes are unsupported, MultiXact horizon maintenance is
absent, and speculative insertion is not implemented. All PG19-required AM
callbacks are non-NULL; unsupported families reject explicitly rather than
misbehaving.

## 2. Build and verification

Environment:

```bash
export PGENVWRAPPERRC="$HOME/.config/pgenvwrapperrc"
source /home/frbn/git/dalibo/wrapper_pgenv/pg_env.sh
pgenv pax
pg path pax
cd /home/frbn/git/franck/pax_am
make
make install
make installcheck
```

Current result:

```text
make                         # zero warnings
ok 1 - pax_am                # All 1 tests passed
ok 1 - pax_mvcc              # All 1 tests passed
```

The PostgreSQL 19 build used here does **not** enable assertions. The handler
now validates every callback asserted by `GetTableAmRoutine()` at runtime, and
all required callbacks are wired; unsupported families raise
`FEATURE_NOT_SUPPORTED`. An assertion-enabled build is still recommended as an
independent validation.

## 3. Page format version 6 — the current format

`PAX_PAGE_VERSION` is 6. Both page flags and header flags require:

- `PAX_FLAG_HAS_XMIN_XMAX`;
- `PAX_FLAG_HAS_VERSIONS`.

Normal page layout:

```text
PageHeaderData
PaxPageHeader                      PaxPageHeaderPtr = page + 24
    n_tuples, meta_offset, free_space, flags
    byte offsets for user-column regions

version metadata region            (starts at pd_lower == meta_offset)
    one 32-byte PaxTupleMetaData per physical version

column region 0
    [NULL bitmap][slot 0][slot 1]...   slot = attlen, no MAXALIGN
column region 1
    [NULL bitmap][slot 0][slot 1]...
...
free space                         (pd_lower .. pd_upper)
variable payloads allocated downward from pd_upper
PaxSpecialData at the page end
```

`PaxSpecialData` appears only at the page end, where `PageInit()` puts it.

Up to version 5 there were 8 bytes of padding between `PageHeaderData` and
`PaxPageHeader`, justified here as keeping the header "8-aligned". That
justification was wrong, and checking it cost less than a build:

- `PaxPageHeader` is all `uint16` — it needs **2** bytes of alignment;
- `PaxTupleMetaData` has `uint32` members — it needs **4**;
- `SizeOfPageHeaderData` is **24**, already a multiple of 8, and buffers are
  `alignas(MAXIMUM_ALIGNOF)` (`c.h`), so `page + 24` was already 8-aligned.

The 8 bytes satisfied no constraint the layout did not already satisfy. They
existed because the offset was written `SizeOfPageHeaderData +
SizeOfPaxSpecialData`, by shape analogy with heap, where a fixed-size area does
sit right after `PageHeaderData` — the `ItemIdData` array, which PAX does not
have. Nothing ever read that offset: the real special area is at the page end.

Version 6 removes them. The invariant that actually matters is now asserted
rather than asserted-in-prose, which is the durable part of the change:

```c
StaticAssertDecl(SizeOfPageHeaderData % MAXIMUM_ALIGNOF == 0,
                 "pax: PaxPageHeader would be misaligned without padding");
```

Cost of the removal: 8 bytes per page, 0.098%. Benefit of taking it: one fewer
false statement in the format specification, and `pd_lower` starts 8 bytes
lower. The regression golden did not move.

No `ItemIdData` line pointer array exists on a PAX page. A version is
identified by its index within the page, which is what `t_ctid` stores, so
`pax_page_init` starts `pd_lower` directly after the PAX header instead of after
a line pointer array.

Rendered diagrams: `docs/pax-page.svg` for this layout and
`docs/heap-page.svg` for the heap equivalent.

### Slot packing (version 4)

Version 3 stored every slot on a `MAXALIGN` stride, so an `int4` cost 8 bytes
and a varlena offset table 8 bytes per row. Version 4 uses the exact length:

| column type | stride v3 | stride v4 |
|---|---|---|
| `int2` | 8 | 2 |
| `int4` | 8 | 4 |
| `int8` | 8 | 8 |
| varlena offset | 8 | 2 |

No alignment requirement is kept, because regions are packed byte-exact and
get pushed around by later inserts, so a slot can end up at an address not
aligned for its type. Reads therefore go through `memcpy` in `pax_get_value`,
and by-reference fixed types (`uuid`, `inet`, `interval`, …) are copied into a
per-column buffer in the descriptor rather than pointed at inside the page.
This is portable and costs nothing per row.

Consequence for region sizing: the span between two regions is no longer
canonical, because a region's start can be pushed by the growth of the regions
before it. The bitmap size is therefore derived from `n_tuples` rather than
from the span, and the span is only checked to be large enough.

Measured effect at 50,000 rows, versus version 3:

| schema | raw size v3 | raw size v4 | reduction |
|---|---|---|---|
| narrow (`int, int`) | 1.34x heap | 1.12x heap | 16% |
| wide (`int` + 5 `text`) | 1.79x heap | 1.40x heap | 22% |
| 90% NULL (`int` + 4 `int`) | 1.95x heap | 1.41x heap | 28% |

Pages also hold more rows: a 3-column test page goes from 114 to 120 versions.

`make shared-buffers-test` reports identical size and identical buffer usage
for heap and PAX after this change, where version 3 needed 5% more buffers.

### Cached page layout with revalidation

The sequential scan keeps a page *layout* descriptor across calls. The layout
is what `PaxPageDesc` holds besides `tuple_meta`: geometry of the regions, slot
strides, and the pointers into each region's NULL bitmap and value area.

That geometry depends only on the page header, so it stays valid as long as
`n_tuples`, `meta_offset`, `flags` and `offsets[]` are unchanged. Version
metadata is deliberately **not** cached: `xmin`, `xmax`, `cmin`, `cmax`,
`t_ctid` and `locker_mxid` are mutated in place by concurrent UPDATE, DELETE
and row locks, so they are copied and revalidated on every call.

Revalidation compares an independent snapshot taken when the layout was built
against the page header read again. The snapshot is necessary because
`desc->header` points *into* the page, so comparing it with itself would prove
nothing. `offsets[]` is the field that matters most: a concurrent insert
memmoves the column regions and updates those offsets, which invalidates every
cached pointer.

Measured: the layout cache removes the per-tuple descriptor rebuild, and a
20,000-row warm scan goes from ~12.4 ms to ~9.1 ms, narrowing the gap against
heap from 4.3x to 3.2x.

Correctness was checked against a concurrent inserter targeting the same page:
`n_tuples`, `pd_lower` and all three `offsets[]` changed (50 to 59 tuples), so
the layout is rebuilt, and a slow scan correctly returned only its own
snapshot's rows.

### Per-version metadata

`PaxTupleMetaData` has a compile-time fixed 32-byte on-disk stride:

```c
TransactionId xmin;
TransactionId xmax;       /* updater/deleter only, never a row locker */
CommandId     cmin;
CommandId     cmax;       /* meaningful only with xmax */
ItemPointerData t_ctid;   /* successor version, or self for a leaf */
uint16        flags;     /* reserved, zero */
MultiXactId   locker_mxid;/* lock-only members */
uint32        reserved2; /* explicit tail padding, zero */
```

Design rules:

- Every UPDATE allocates a new physical tuple and sets the old tuple's
  `t_ctid` to the new TID.
- DELETE keeps the tuple and sets a self `t_ctid`.
- Aborted versions remain physically allocated.
- Physical TID slots are never reused.
- The lock-only MultiXact roster is deliberately separate from `xmax`; row locks
  must not make a tuple look deleted.
- A committed chain hop must satisfy `successor.xmin == predecessor.xmax`.
- Chain traversal has a cycle/length bound.
- Page/version/magic/attribute-count/region-size checks reject foreign and
  malformed pages early.

There is no migration from older page
formats are intentionally unreadable and must be recreated or rewritten by a
future migration facility.

## 4. MVCC and visibility

Scans apply visibility independently to every physical version. Supported
snapshots remain:

- `SNAPSHOT_MVCC`;
- `SNAPSHOT_SELF`;
- `SNAPSHOT_ANY`.

Dirty snapshots, historic snapshots, and logical-decoding snapshots are
rejected.

For a normal MVCC snapshot:

- another transaction's committed `xmin` is visible unless in the snapshot;
- current-transaction `cmin < curcid` is visible;
- current `xmax` keeps the old version visible while `cmax >= curcid`;
- committed `xmax` hides the old version;
- aborted `xmin` or `xmax` leaves the appropriate physical version visible.

UPDATE/DELETE/lock callbacks do **not** use the statement snapshot as a
replacement for current-command classification. They use the supplied executor
`cid`, matching heap's `HeapTupleSatisfiesUpdate()` rules:

- current insert with `cmin >= cid` -> `TM_Invisible`;
- current outdating with `cmax >= cid` -> `TM_SelfModified`;
- current outdating from an earlier command -> `TM_Invisible`;
- another in-progress updater -> wait or `TM_BeingModified`;
- committed forward link -> `TM_Updated`;
- committed self link -> `TM_Deleted`.

`crosscheck` remains a separate RI visibility check. A crosscheck failure is
reported as `TM_Updated`, not `TM_Invisible`.

## 5. UPDATE algorithm

`pax_tuple_update()` implements conservative append-only UPDATE:

1. Materialize/detoast the replacement slot before page mutation.
2. Acquire a heavyweight tuple lock as short-lived waiter arbitration.
3. Lock and validate the exact old physical page/TID.
4. Classify creator/updater state using the executor command ID.
5. Check incompatible persistent row locks and wait without holding a page
   content lock.
6. Apply the RI crosscheck when supplied.
7. Record the SSI conflict-in check.
8. Reserve the old version with current `xmax`, current `cmax`, and a temporary
   self link.
9. Insert a complete new physical version with current `xmin/cmin`.
10. Carry any lock owned by the current transaction to the new version.
11. Re-find the old version (insertion may have moved page metadata), verify the
    reservation, and publish old `t_ctid = new_tid`.
12. Set the replacement slot's physical TID and return `TU_All`.

`TU_All` is mandatory because every update changes the physical TID. PAX does
not implement HOT chains or index maintenance yet, so indexed tables remain
unsupported.

An oversized replacement is detected before inserting a tuple. The old version
may already be reserved in the failing subtransaction, but rolling back that
subtransaction makes its `xmax` aborted and restores the old version. The
regression suite covers this case.

## 6. DELETE algorithm

`pax_tuple_delete()`:

1. acquires waiter arbitration and the exact old page/TID;
2. applies current-command, concurrent-writer, row-lock, and crosscheck rules;
3. records the SSI conflict-in check;
4. stores current `xmax/cmax` and a self `t_ctid`;
5. marks the page dirty and returns `TM_Ok`.

DELETE does not decrement `n_tuples`, overwrite user values, move a TID, or
reuse space. `TABLE_DELETE_CHANGING_PARTITION` is rejected explicitly because
cross-partition moves are not implemented.

## 7. Conflict and row-lock behavior

### Heavyweight tuple locks

`LockTuple()` is used only as short-lived arbitration for waiting and priority.
It is released before the table callback returns; it is not a persistent SQL
row lock and is not intentionally held until commit.

### Persistent row locks

`tuple_lock` stores lock-only members in `locker_mxid` using PostgreSQL
MultiXact infrastructure. This supports the four row-lock modes and their
compatibility matrix. Completed/aborted members are ignored. Subtransaction XIDs
are stored, so savepoint rollback releases that subtransaction's logical lock.

UPDATE always requests conservative `LockTupleExclusive`; it waits for active
other-transaction lockers. A lock owned by the updating transaction is carried
to the replacement version and copied back into the returned slot metadata.
This sacrifices key/no-key update concurrency but avoids incorrect key-lock
transfer.

For system-column compatibility, `xmax` exposes the stable raw updater XID, or
the stored lock-only MultiXact ID when no updater XID exists; it does not pick a
transient member based on timing. Non-waiting UPDATE/DELETE is rejected
explicitly instead of returning incomplete conflict data.

### READ COMMITTED retry

A blocked writer waits on the conflicting XID under `LockWaitBlock`, re-reads
the physical metadata, and then returns the proper terminal status:

- updater abort -> modify the old version;
- updater commits -> `TM_Updated`;
- deleter commits -> `TM_Deleted`.

For READ COMMITTED, executor EPQ calls `tuple_lock` with
`TUPLE_LOCK_FLAG_FIND_LAST_VERSION`. PAX follows and validates the committed
chain, locks the latest version, and the executor re-evaluates the query.
Committed chains are followed only when that flag is present; transaction-
snapshot row locking receives `TM_Updated` and the expected serialization
error. The isolation suite covers committed update, aborted update,
concurrent DELETE, transaction-snapshot locking, and row-lock blocking.

## 8. Scan and slot safety

The previous page-backed slot race is fixed.

For every scan tuple and exact-version fetch, PAX now:

1. share-locks the page;
2. builds and validates a page descriptor;
3. copies transaction metadata and user values into the slot;
4. materializes by-reference values;
5. releases the page content lock and pin.

No executor-visible slot retains pointers into a page after the content lock is
dropped. Inserts may still shift regions, but they can no longer invalidate a
live slot.

Sequential and TID/TID-range scans are implemented. Bitmap, sampling, and other
unsupported scan types are rejected at scan creation, and non-TID scan keys are
rejected rather than silently ignored.

## 9. Relation extension

The `P_NEW` path is now serialized with `LockRelationForExtension()` and checks
`PageIsNew()` before initializing the returned page. This removes the original
PG19 race where concurrent inserters could initialize the same extension
block.

A future implementation should move to a modern extension API rather than
relying on the legacy compatibility path. New pages are already protected by
a Generic WAL full-page image.

## 10. Generic WAL durability

Every physical PAX mutation now uses PostgreSQL Generic WAL:

- `GenericXLogStart()` creates a private page-image state while the target
  buffer is exclusively content-locked;
- `GenericXLogRegisterBuffer()` copies the page out of shared buffers;
- PAX validates and mutates only that private image;
- `GenericXLogFinish()` emits `RM_GENERIC_ID`, applies the image, marks the
  buffer dirty, and sets the page LSN before the content lock is released.

Covered mutations:

- initialization and first insert on a new block (full-page image);
- append insert into an existing page (delta);
- UPDATE old-version `xmax/cmax` reservation;
- insertion of the replacement version;
- publication of the forward `t_ctid` link;
- DELETE `xmax/cmax` and self link;
- lock-only MultiXact pointer changes.

The multi-step UPDATE sequence may produce several Generic WAL records. This is
safe because every record is emitted before any affected buffer can be written;
a transaction commit record cannot be flushed before all earlier PAX records.
On crash, an uncommitted reservation is harmless because its XID aborts.

`GenericXLogStart()` uses `RelationNeedsWAL()`, so unlogged/temporary relations
skip WAL but still apply the private image atomically.

Durability was verified beyond ordinary test execution:

- `pg_waldump -r Generic` showed a full-page image for a new-block insert and
  the expected reservation/insert/link records for an UPDATE;
- an immediate `pg_ctl -m immediate` shutdown followed by restart performed
  automatic recovery and preserved committed INSERT/UPDATE/DELETE data;
- a second immediate-crash test with an open transaction confirmed that its
  uncommitted INSERT/UPDATE/DELETE records replayed physically but remained
  invisible because the transaction XID aborted during recovery.

Generic WAL does not add logical decoding or PAX-specific redo metadata; it
provides physical crash recovery and physical-replication redo of page images.

## 11. Test coverage

### SQL regression: `sql/pax_am.sql`

New versioned-DML cases cover:

- COPY through the required `multi_insert` callback;
- WAL LSN advancement for INSERT, UPDATE, logical row locking, and DELETE;
- real UPDATE with relocated physical TID and transaction system attributes;
- current-command visibility after UPDATE;
- top-level UPDATE rollback;
- repeated UPDATE and savepoint rollback;
- DELETE `RETURNING` with current `xmax/cmax`;
- DELETE visibility and rollback;
- oversized UPDATE failure followed by savepoint recovery;
- duplicate join matches producing `TM_SelfModified` without duplicate writes;
- current row-lock carry into UPDATE `RETURNING` metadata while no heavyweight
  tuple lock is retained;
- a cross-page replacement, validating the 32-bit block/16-bit offset link and
  the 7 KB round trip.

### Isolation: `specs/pax_mvcc.spec`

New permutations cover:

- uncommitted UPDATE invisibility and commit/abort outcomes;
- UPDATE version chains and savepoint rollback;
- blocked concurrent UPDATE followed by commit and abort;
- concurrent DELETE following a committed UPDATE chain;
- repeatable-read historical views before UPDATE and DELETE;
- transaction-snapshot row locking returning a serialization conflict instead
  of following a committed update without `FIND_LAST_VERSION`;
- `SELECT FOR UPDATE` logical row locking;
- UPDATE waiting for a committed row lock;
- row-lock removal after subtransaction rollback.

### Buffer-usage comparison: `make shared-buffers-test`

`tests/compare_shared_buffers.sh` plus `tests/compare_shared_buffers.sql` build
two tables with identical schema and data — one `heap`, one `pax` — inside a
throwaway database, then run the same warm sequential scan against each.

Reported per access method:

- relation size in bytes;
- resident shared pages from `pg_buffercache` (skipped if unavailable);
- `EXPLAIN (ANALYZE, BUFFERS)` shared hit, read, and dirtied blocks;
- a `pax`/`heap` shared-block ratio.

The test asserts that both queries return identical results and that both
report nonzero shared-buffer usage. It deliberately does **not** assert a
performance direction, because the correct ratio depends on row width, packing,
and cache state. Buffer counts are environment-dependent, so this is a manual
measurement target rather than a golden regression.

Parsing note: PG19 propagates inclusive buffer counters into parent plan nodes,
so the harness reads the top-level `Plan` counters. Summing every node — the
obvious approach — double-counts a sequential scan.

First result on the development host, 5,000 rows of ~320-byte payloads:

| access method | relation bytes | resident buffers | shared hit blocks | ratio |
|---------------|----------------|------------------|--------------------|-------|
| heap          | 1,867,776      | 228              | 228                | 1.00  |
| pax           | 1,957,888      | 239              | 5,239              | 22.98 |

Storage is close (PAX 4.8% larger, expected from per-tuple metadata and
per-column bitmaps), but buffer *access* was the real cost: PAX needed roughly
one `ReadBuffer` per returned tuple. The 5,239 hits equalled exactly 5,000
tuples plus 239 pages, which confirmed the accounting.

### Lazy metadata revalidation

Copying and validating all `n` `PaxTupleMetaData` entries on every call made the
scan O(n²) per page, even though a dense page almost always yields the very
next tuple: the work was `O(n)` to look at one entry.

`PaxPageDesc` now carries `meta_checked_through`, the number of leading entries
this descriptor has already copied and validated. `pax_meta_ensure()` refreshes
and validates entry `i` only when `i` is at or beyond that watermark.

This is sound because a scan cursor only moves forward: entries it has already
passed are never read again, so re-reading them was pure waste. It is *not* a
cache in the MVCC sense. Entries ahead of the cursor may have been changed by a
concurrent UPDATE or DELETE, and they are still re-read from the page under the
content lock every time the cursor reaches them. Only the entries behind it are
skipped, and a VACUUM marking a version `UNUSED` cannot shift anything, so no
skipped entry can be mistaken for another.

The metadata region size check moved to `pax_build_page_layout()`, since it is a
layout property, so the lazy path still validates it once per page.

The one-shot paths (UPDATE, DELETE, row locking, TID fetch) keep the full
revalidation through `pax_build_page_desc()`; they are not hot loops.

Measured on a 20,000-row warm scan, best of 7:

| | before | after |
|---|---|---|
| PAX | 9.1 ms | **7.2 ms** |
| heap | 2.8 ms | 2.9 ms |
| ratio | 3.2x | **2.5x** |

Corruption detection was re-checked after the change, since validating less is
exactly the kind of trade that can silently lose a safety net: an `xmin` of 0
written into the middle entry of a page still produces
`invalid page in block 0`.

### Free space map

Page selection for `tuple_insert` no longer walks blocks from 0, which cost
one `ReadBuffer` plus an exclusive lock per already-full block. The order is
now the one heap uses in `hio.c`:

1. the block cached in the relcache target block, if it is still within the
   relation;
2. `GetPageWithFreeSpace()` otherwise;
3. the last block, when the map says nothing, to avoid the one-row-per-page
   syndrome while a relation starts up;
4. `RelationAddBlocks`-style extension as the final fallback.

The map is only a *hint*: the exact requirement is recomputed under the
exclusive lock, and if the page turns out to be too small the actual free space
is recorded and `RecordAndGetPageWithFreeSpace()` asks for another candidate.

Two points differ from heap on purpose:

- **The map is recorded after every successful insert.** Heap deliberately
  leaves that to VACUUM. PAX has no VACUUM, so nothing would ever populate the
  map and every insert would extend the relation. This costs one FSM buffer
  acquisition per insert, on a hot buffer.
- **The search hint is a lower bound, never an over-estimate.**
  `pax_insert_space_hint()` ignores the NULL bitmap growth, so the map can
  offer extra candidates, but it will never hide a usable page.

PAX never frees space by itself: UPDATE and DELETE only write into version
metadata that is already allocated, and new versions are appended. Free space
therefore only decreases, so recording on insert is sufficient until VACUUM
exists to reclaim dead versions and repopulate the map.

`RecordPageWithFreeSpace()` does not emit WAL, so the map can be stale after a
crash. That is safe by construction: a stale entry only produces a candidate
that fails the under-lock check and falls through to the retry loop. Verified
with an immediate shutdown, restart, and further inserts: no error, no
corruption.

Measured, 20,000 rows of `(int, int, text)` into a fresh table:

| page selection | insert time | pages |
|---|---|---|
| linear first-fit | 184.8 s | 144 |
| free space map | 82.9 s | 144 |

2.2x faster with an identical page count, so the different fill policy costs
nothing in space. For the 144-page result, 139 pages are recorded full.

### Fix: pin the current page across calls

`pax_scan_getnextslot` now mirrors heap's `heapgettup` buffer handling: the scan
keeps a pin on the current block in `scan->current_buf` and reuses it, so each
call costs a `LockBuffer` instead of a full `ReadBuffer`. Measured after the
fix, same workload:

| access method | relation bytes | shared hit blocks | ratio |
|---------------|----------------|--------------------|-------|
| heap          | 1,867,776      | 228                | 1.00  |
| pax           | 1,957,888      | 239                | 1.05  |

PAX now touches each page exactly once, and the 23x gap is gone.

The **content lock is deliberately not** held across calls. Heap releases it
too (`heapam.c:1034`, via `LockBuffer(..., BUFFER_LOCK_UNLOCK)`), and it
matters: an `UPDATE t ... FROM t` self-join modifies a tuple on a page the scan
is reading, so holding a share content lock across the call would
self-deadlock. That case is verified explicitly.

What is *not* cached is the descriptor itself. `PaxPageDesc` stores raw page
pointers (`mp->data`, `desc->header`) plus an independent `tuple_meta` copy, so
reusing it across calls would risk either a dangling column pointer when a
concurrent first-fit insert memmoves the page, or stale MVCC metadata when a
concurrent commit sets `xmax`. It is therefore rebuilt under a freshly taken
content lock on every call. This keeps correctness; the residual cost is CPU
rather than buffer traffic. See section 12.

### ANALYZE and VACUUM

ANALYZE was previously rejected outright, which meant the planner had no
statistics at all. It is now implemented on `scan_analyze_next_block` /
`scan_analyze_next_tuple`: the outer loop pulls a block from the read stream,
the inner loop walks its versions while holding a content lock, so a concurrent
inserter cannot memmove the page and the cached layout stays valid. Because an
ANALYZE scan has no snapshot, `pax_find_visible_tuple()` skips only
`UNUSED` versions and `pax_meta_classify()` decides live/dead afterwards.

That path exposed a latent slot bug: `pax_store_tuple_slot()` did not clear
`TTS_FLAG_SHOULDFREE`. A regular scan clears the slot between rows, but ANALYZE
does not, so the flag stayed set while `tts_values[]` pointed back into the
page, and `pax_slot_clear()` freed a page pointer. The slot now clears its
previous contents first.

#### Why VACUUM marks instead of compacting

PAX chains are not page-local: an UPDATE appends its replacement wherever page
selection (now the FSM) chooses, so `t_ctid` links point at physical
`(page, version index)` pairs. Removing a version would shift every following
slot in every column of that page and break inbound links from other pages, and
heap-style backwards chain pruning is impossible because it would need two page
locks at once.

So VACUUM reclaims space in two safe steps instead:

- a dead version is **marked** `PAX_VERSION_UNUSED` in its `flags` field (a bit
  that version 3 required to be zero), leaving its slot in place;
- fully unused trailing pages are **truncated**, after taking
  `AccessExclusiveLock` and re-verifying from the end, as heap does.

A version is only marked when it is dead (insert aborted, or delete committed)
and has no in-progress row locker. Because an `UNUSED` version keeps its index,
a stale inbound link still lands on it, and two choke points make that safe:
`pax_meta_satisfies_snapshot()` returns false for it, and
`pax_classify_version()` reports `PaxVersionDeleted`, so both chain followers
stop there instead of exposing it.

Freezing is in the same pass: a version whose `xmin` is committed and older than
`GetOldestNonRemovableTransactionId()` is set to `FrozenTransactionId`. The
field already exists on disk, so the page size does not change. Verified: after
`VACUUM FREEZE`, all 500 versions of a test table carried `xmin = 2`.

VACUUM also repopulates the FSM, which matters because the map is never written
by a PAX VACUUM in any other way.

#### Measured

| operation | before | after |
|---|---|---|
| ANALYZE | error | 2100 live / 900 dead on a 3000-row table |
| chains 3 deep + delete head | n/a | 1950 rows, 0 mismatched, chain still followed |
| trailing empty pages | 13 pages | 0 pages |
| `VACUUM FREEZE` | no freeze | 500/500 versions frozen |

#### In-place space reclamation

`tuple_insert` now reuses a slot that VACUUM marked `UNUSED`, instead of
appending at `n_tuples`. Two conditions apply:

- **every column region must already exist on the page.** A column that was NULL
  for every row on that page has no region, hence no slot to reuse, and creating
  one would grow the page anyway. Such a page falls back to appending.
- **the page must still hold room for the new variable-length payloads.** Reuse
  does not extend the regions or the metadata, but values longer than 8 bytes are
  still allocated from `pd_upper`, so `pax_payload_space_needed()` bounds the
  check.

This alone reclaimed very little, and the measurement is worth keeping because it
is the reason the second step was necessary: reusing slots requires contiguous
free space, and VACUUM returned none. After marking 15000 dead versions, the 100
pages holding them had **52 bytes free on average, 42 at most** — far below the
~90 bytes a new row needs. Those pages still looked full to the FSM, so INSERT
never even considered them and the relation grew 234 → 368 pages.

#### The FSM, which is the part that actually works

Worth stating separately because it is easy to assume it is missing.
`pax_relation_vacuum()` finishes with a full pass over the relation re-recording
`pax_page_free_space()` for every block, and pages removed by truncation get
`RecordPageWithFreeSpace(rel, b, 0)` first. Between that pass and the
per-insert `RecordPageWithFreeSpace()`, the map is never stale.

Measured, with a **fresh backend** so the per-backend target-block hint is not
what steers the insert — only the FSM can:

| table | steps | pages |
|---|---|---|
| `f` | 4000 rows → UPDATE 3500 → VACUUM → +3500 rows | 122 → 228 → **231** |
| `g` | 4000 rows → +3500 rows, no UPDATE, no VACUUM | 122 → **228** |

Both end at the same row count, but `f` needed **+3 pages** to absorb 3500 new
rows where `g` needed **+106**. Those 3500 rows landed in slots VACUUM had
freed, on pages the FSM pointed at. Without the vacuum pass the map still
described those pages as full and INSERT would not have looked at them at all —
the original failure mode recorded above, 52 bytes free on average and the
relation growing 234 → 368.

#### The visibility map, and why it is not there

No PAX page is ever declared all-visible: `visibilitymap_set()` and
`PageSetAllVisible()` have zero call sites. That is a deliberate omission, not an
oversight, and the reason is a poor risk/benefit ratio.

**What it would buy.** The VM is consumed by index-only scans. Both callbacks
PAX needs already exist (`index_fetch_begin` and `tuple_fetch_row_version`), so
index-only scans work today — they just always take the executor's "dirty" path.
The saving is skipping `pax_meta_satisfies_snapshot()` per fetched version. It
is *not* an I/O saving: `nodeIndexOnlyscan` calls the table AM either way, and
PAX cannot return a tuple without reading the page its slots live in. So the
ceiling is a per-tuple visibility check, on a path PAX is already 2-4x slower
than heap for other reasons.

**What it would cost.** A wrong all-visible bit does not degrade, it returns
wrong rows, so every mutation that can change a page's all-visible status has to
clear it. PAX appends new versions to pages chosen by the FSM, so a page that
was all-visible can gain an in-progress version at any time. heap gets this for
free because `PageAddItem()` clears `PD_ALL_VISIBLE` itself; PAX writes its own
slots, so it would have to clear both the page flag and the map on every append,
every UPDATE that lands on such a page, and every reuse of a slot. That is a
silent-wrong-answer failure mode reachable from the insert path — the most
exercised code in the AM.

Verdict: implementable, roughly a hundred lines plus a new set of
correctness assertions, for a single skipped comparison. If index-only scans
ever become a real workload for PAX, that changes; today it does not justify
touching the insert path.

#### Compacting the payloads

So VACUUM now also compacts the **payloads** of the dead versions. This is safe
where removing versions is not, and the reason is precise: payloads live in the
`pd_upper` zone, while the per-column offset tables live below `pd_lower` in the
regions. Moving payload bytes touches neither `n_tuples` nor any region offset,
so **no version index moves and every `t_ctid` link, including inbound ones from
other pages, stays exact**. Only the offsets recorded in the tables are rewritten,
in place.

A scan reads those offset tables afresh on each materialization and does so under
the content lock, so it cannot observe a half-finished move. Its cached page
descriptor also stays valid, since neither `meta_offset` nor the region offsets
change. Payloads move downward from the top of the zone, which guarantees no
not-yet-copied source is overwritten, and the freed span is zeroed so a raw page
read through `pageinspect` cannot reveal dead values.

#### Measured

Starting from 20,000 rows, replacing 15,000 of them, then vacuuming:

| scenario | before | after |
|---|---|---|
| re-insert 15,000 rows | 234 pages | **234 pages** (no growth) |
| re-insert 20,000 rows | 368 pages | **268 pages** |
| mismatched rows | 0 | 0 |
| `UNUSED` slots left on refilled pages | all 15,000 | **0** |

One real bug surfaced here, and it is worth recording because it is a property of
the format rather than a slip: on reuse, the value slot must be placed after
`pax_bitmap_size(n_tuples)`, not `pax_bitmap_size(tupno)`. Since `tupno <
n_tuples` in the reuse case, the offset landed inside the existing table and
shifted every following entry, so rows read back with another row's value
(`id = 26` returning `'r30'`). Reading and writing the table must agree on the
bitmap size, which is what `pax_build_page_layout()` does on the read side.

What this does **not** reclaim: the 32 metadata bytes and the fixed-width slots
of a dead version stay on the page. Only variable-length payloads are compacted,
and only `UNUSED` slots are refilled. A dead version therefore keeps its slot
until the page as a whole is refilled.

### Columnar trade-off measurement: `make columnar-benchmark`

`tests/benchmark_columnar.sh` loads identical data into `heap` and `pax` tables
across three schemas - narrow (`int, int`), wide (`int` + 5 `text`), and 90% NULL
(`int` + 4 `int`) - verifies the two access methods return identical results,
then reports four measure families and draws a verdict from the numbers.

50,000 rows, PostgreSQL 19devel, best of 2 warm runs:

| family | PAX vs heap | verdict |
|---|---|---|
| raw size | 1.34x to 1.95x | PAX loses |
| compressed size (gzip -9 on the relation file) | 0.82x to 1.04x | PAX wins or ties |
| projected scan | 6.7x to 9.3x | PAX loses |
| full scan | 2.4x to 8.4x | PAX loses |

Causes, all on the storage side:

- 32 bytes of `PaxTupleMetaData` per version versus 24 for a `HeapTupleHeader`;
- the page descriptor is rebuilt per returned row, and the whole row is
  materialized because the scan is not told which attributes were requested.

### What the "compressed size" row does and does not show

Read this row carefully before citing it, because it has been misread.

The benchmark runs `gzip -9` over the whole relation **file** after a
`CHECKPOINT`. It is a property of the on-disk byte stream, measured *outside*
the access method. It is **not** a measurement of any compression PAX performs,
because PAX performs none: there is no compressor, no compressed region, and
no decompression on the read path. `PAX_FLAG_COMPRESSED` is defined in
`pax_am.c` but never set.

Two things make the number look better than the situation:

- the test data is `repeat('<char>', 60)`, i.e. highly repetitive in a way real
  columns are not;
- `gzip` compresses across column boundaries and across the 32 metadata bytes
  per version, which is exactly the adjacency that **per-column** compression
  would destroy. The columnar layout helps `gzip` find runs; it does not
  compress anything by itself.

So the honest statement is: PAX's byte layout is compressible to roughly heap's,
which says nothing about a future in-AM compression scheme. See section 16 for
what per-column compression would actually measure.

### Two dead ends, measured and rejected

Both were pursued and both are now closed. They are recorded because the
arithmetic looks compelling and is wrong, and re-deriving it wastes hours.

**Reducing `PaxTupleMetaData` cannot make a page hold more rows.** On the
12-column table (`make wide-projection-benchmark`, 20,000 rows, payload
`repeat('<c>',60)`), measured directly from the page layout:

| zone | bytes/page | per row |
|---|---|---|
| low (metadata + slots + NULL bitmaps) | 676 | 67.6 |
| high (payload) | 7040 | 704.0 |
| total | | **771.6** |

11 rows per page would require `(8192 - 64) / 11 = 738.9` bytes per row, so
**32.7 bytes per row must go**. The metadata is 32 bytes. It is 0.7 bytes short
even if reduced to zero, and `xmin`/`xmax`/`t_ctid` cannot be removed. Reducing
it to 24 bytes changes nothing on this schema.

**Removing `MAXALIGN` from the payload changes nothing, because there is nothing
to remove.** Instrumenting `pax_alloc_payload` to log `len`, `aligned_len` and
the actual `pd_upper` movement shows that for `attlen == -1` (`text`) the value
arriving from the executor is *already* 8-byte aligned:

| characters | `VARSIZE_ANY` measured | `MAXALIGN` of that |
|---|---|---|
| 55 | 59 | 64 |
| 57 | 61 | 64 |
| **60** | **64** | **64** (no padding) |
| 61 | 65 | 72 |

At 60 characters `len` is 64, so `MAXALIGN(64) == 64` and the "33 bytes per row
of alignment waste" does not exist. The 704 bytes per row measured in the high
zone is exactly `11 x 64`: the data as PostgreSQL delivers it, with zero padding.
Two of the three `MAXALIGN` call sites on this path only align `attlen == -2`
(`cstring`) and leave `attlen == -1` untouched, which is why editing them had no
observable effect.

Column **segments inside the same pages** were also considered and rejected on
the same arithmetic: grouping 12 columns into 3 segments of 4 collapses 12 NULL
bitmaps into 3, which costs as much as it saves (24 bytes either way), and saves
only 18 header bytes per page, about 1.8 bytes per row, against the 32.7 needed.
And because a shared buffer is a whole 8 kB page, segments within a page cannot
reduce the block count of a scan at all: PostgreSQL reads all 8192 bytes whether
the query needs one column from the page or all twelve.

## 12. Remaining risks and unsupported behavior

### P0 / production blockers

1. **No migration from page version 3.** Version 4 changed the slot stride, so a
   v3 page would be decoded with the wrong stride, not merely differently. A
   rewrite or in-place migration design is required before existing databases
   can upgrade.

### P1 semantic limitations

- No logical decoding or historic/dirty snapshot support.
- No index cleanup: indexes remain unsupported altogether.
- No speculative insertion token lifecycle.
- `TU_All` is returned and index callbacks reject explicitly.
- Non-rewriting schema changes can make physical layouts unreadable; matching
  `natts` is not a sufficient schema signature.
- No cross-partition UPDATE/move protocol.
- Logical row-lock hints are not cleaned; correctness relies on checking
  MultiXact member transaction state.
- `LOCK_UPDATE_IN_PROGRESS` is handled conservatively by waiting; it does not
  provide heap's compatible KeyShare/no-key-update optimization.
- No TOAST/overflow rows and no parallelism.

### P2 performance/maintainability

- ~~Insert first-fit scans linearly from block 0; no FSM.~~ Resolved: page
  selection now goes through the free space map (section 11).
- Sequential scan cost is now dominated by per-row tuple materialization.
  The buffer pin, the page *layout*, and the metadata revalidation are all
  narrowed to what each call actually needs (section 11), so a 20,000-row warm
  scan is ~7.2 ms against ~2.9 ms for heap, 2.5x.
- Update reserves the old version, drops the page lock, inserts the replacement,
  then republishes the link; each phase emits its own Generic WAL record.
- Planner size estimation remains approximate.
- Backward scans and scan-key pushdown are absent; unsupported scan keys are
  rejected rather than ignored.
- `UNUSED` slots and their fixed-width slots are reclaimed only when INSERT
  refills them; VACUUM compacts their variable-length payloads, so a dead
  version that no INSERT ever revisits still holds 32 metadata bytes.

## 13. Indexes

Seven callbacks were needed: `index_fetch_begin`/`reset`/`end`/`index_fetch_tuple`,
`index_delete_tuples`, `index_build_range_scan` and `index_validate_scan`.

### The decision that shapes everything else

**PAX has no HOT chains, so every version gets its own index entry.** Heap indexes
only the live member of a HOT chain and points the single entry at the chain *root*;
that root indirection is what makes a stale index entry harmless. PAX cannot do
that: `t_ctid` is a physical `(page, version index)` and a chain crosses pages, so
there is no page-local root to hand to the index.

The consequence dictates `index_fetch_tuple`: it must resolve a TID to **that exact
version** and must *not* follow `t_ctid`. Following the chain — the obvious reading
of the callback's contract, which mentions HOT explicitly — makes an entry left
behind by an UPDATE resolve to its successor, which *is* visible. The row then
appears twice through the index while remaining invisible to a seq scan.

That was a real bug here, and the symptom was precise: after one UPDATE of a
non-indexed column on a 2000-row table, `WHERE id = 20` returned two rows, both
`(12,64)`, with a total of 2000 rows scanned through the index for a 1999-row
table. `index_fetch_tuple` was written to walk the chain, on the reasoning that the
callback "should return the current version". It must not.

### Two snapshot types had to be implemented

`SNAPSHOT_NON_VACUUMABLE` and `SNAPSHOT_DIRTY` were both rejected outright, so
uniqueness checking failed with `does not support snapshot type 4`.

`SNAPSHOT_DIRTY` is also an **output argument**: `InitDirtySnapshot()` only sets
`snapshot_type`, and nbtree reads `snapshot->xmin` / `snapshot->xmax` afterwards to
decide whether to wait for a concurrent inserter. A table AM that only reads it
leaves uninitialised stack memory there, which surfaced as bogus xids and
`could not read from file "pg_subtrans/0000": read too few bytes`. Filling
`xmin`, `xmax` and `speculativeToken` on **every** call is mandatory, exactly as
`HeapTupleSatisfiesDirty()` does.

`SNAPSHOT_NON_VACUUMABLE` in PG19 has no `snapshot->min`; the cutoff is a
`GlobalVisState`, so visibility is `!GlobalVisTestIsRemovableXid(vistest, xmin,
true)`.

### Index cleanup needs its own dead-item store

`index_bulk_delete()` is driven by a `TidStore` of dead TIDs. Heap's is created and
filled entirely inside `vacuumlazy.c`, and there is **no table-AM callback that can
populate it**. So a non-heap AM that does not build its own store can never
reclaim an index entry, however correct its `index_delete_tuples` is. PAX now keeps
a local `TidStore`, records the offsets of each version it marks, and vacuums its
indexes after the table. On 50,000 rows with 45,000 deleted, PAX and heap both
finish at 139 index pages — identical.

Note that `pg_relation_size()` does **not** shrink after reclamation in either AM;
freed index pages are only returned by a later VACUUM. Comparing page counts
before/after a single VACUUM is therefore not a test of anything.

### What works

`CREATE INDEX`, `CREATE INDEX CONCURRENTLY`, index scans, partial indexes,
expression indexes, unique and primary-key constraints on INSERT and UPDATE, and
index cleanup under VACUUM. Index and seq scan results were compared exhaustively
(`EXCEPT ALL` both ways) after a mix of UPDATEs, DELETEs and INSERTs: 2928 rows
each way, zero differences, no value returned twice.

## 14. Fixed: multi-row UPDATE hung on an indexed table

**Status: fixed and covered by a regression test.** An `UPDATE` touching more than
~40 rows on a table with any index used to hang forever. `CREATE INDEX`, index
scans, single-row UPDATE, DELETE, INSERT and `VACUUM` were unaffected, and the same
statement without an index completed in ~10 ms.

### Root cause

Not a lock-order problem between backends, and not a lock leak in PAX. **PAX was
leaving a buffer content lock held while it made a nested call that took the same
lock on the same buffer**, which is a self-deadlock: `BufferLockAttempt()` sees the
bit this backend itself set, returns `mustwait = true`, and the backend then waits
forever on its own semaphore. `pg_blocking_pids()` correctly reports nothing,
because no *other* backend is involved.

The chain is:

1. The scan holds `BUFFER_LOCK_SHARE` on a table page (`pax_scan_lock_current_page`).
2. `nodeModifyTable` performs the index maintenance, and nbtree decides to run
   **bottom-up index deletion** (`_bt_bottomupdel_pass` → `_bt_delitems_delete_check`)
   over the pages it just touched. It calls back into the table AM with
   `delstate->bottomup = true`.
3. `pax_relation_index_delete_tuples()` asserted `!delstate->bottomup`. With
   assertions compiled out the assert vanished, and the function **fell through
   into its simple-deletion loop anyway** — it was written for TIDs sorted by block
   and for a single sequential pass over the table, neither of which holds for a
   bottom-up call. In that loop it takes `BUFFER_LOCK_SHARE` on a table page while
   the scan already held `BUFFER_LOCK_SHARE`… and, on the paths where it re-read a
   page it had already released, it took a second lock on the buffer the scan was
   sitting on.
4. The scan's next `LockBuffer()` on that page then waited for a lock the same
   backend already held, forever.

So the `bottomup` case was not merely unhandled, it was handled *as if it were the
simple case*, and that mis-handling is what wedged the backend.

### Fix

`pax_relation_index_delete_tuples()` now declines bottom-up deletion explicitly
(`delstate->ndeltids = 0; return InvalidTransactionId`). Declining is legal: the
contract in `access/tableam.h` says index AMs ask for a space target but leave the
table AM in control, and a table AM may decline to make progress on a pass. PAX
declines because bottom-up deletion's cost model is built on heap's MVCC
visibility accounting and page free space, neither of which PAX has; guessing
would be dishonest.

### A second, independent bug found on the way

`pax_vacuum_indexes()` called `index_open(oid, NoLock)` / `index_close(..., NoLock)`
around `index_bulk_delete()`, i.e. it ran nbtree's deletion pass with **no lock at
all** on the index while mutating its shared buffers. The cassert build caught it
immediately (`Assert(... CheckRelationLockedByMe(r, AccessShareLock, true))`). Now
an explicit `AccessShareLock` is taken and released.

### How it was found, and the methodological lesson

Two earlier instrumentations produced **wrong conclusions** and should not be
repeated:

- A scalar lock-depth counter over `LockBuffer`/`UnlockReleaseBuffer` reported a
  "3-per-row leak", then a "-1 drift". Both were artefacts: this file mixes pin-only
  releases (`ReleaseBuffer`, and `UnlockReleaseBuffer` in `pax_scan_unpin_current`)
  with content-lock releases, and a counter cannot tell them apart.
- A per-buffer lock **tracker** reported **zero** locks held at the blocking point,
  which looked conclusive and was wrong — for a mundane reason: `make install` had
  been failing silently (a leftover compile error), so the instrumented `.so` was
  never the one loaded. Two "0 reports" results in a row came from a stale library.

The decisive instrument was the obvious one and it had not been tried: a
**`--enable-cassert` build**. PG's buffer manager asserts
`entry->data.lockmode == BUFFER_LOCK_UNLOCK` on entry to `BufferLockAcquire`, so a
self-deadlock becomes an immediate `TRAP` with a full backtrace instead of a silent
hang. It named both bugs in the first run.

Two further traps worth recording:

- `debug_print_lock` **no longer exists in PG19**. There is no GUC for tracing buffer
  content locks; `pg_locks` and `pg_blocking_pids()` do not see them.
- gdb cannot `break LockBufferAcquire` on an `-O2` build: the function is `static
  inline` and there is no line info for it. Break on `UnlockReleaseBuffer` (exported)
  or on `bufmgr.c:5929` with a debug build.

### Verification

- Regression test added: a multi-row `UPDATE` (298 rows) plus a multi-row `DELETE`
  on an indexed PAX table, checking the new payload is visible exactly once through
  both the index and the seq scan, that no stale payload remains, and that row
  counts stay consistent.
- Both suites pass on the normal build **and** under `--enable-cassert`
  (`/home/frbn/usr/local/pax-assert`), with no assertion failures.
- The original reproducer (`UPDATE t SET w=w+1 WHERE w <= 41`, 3000 rows, one index)
  now completes on the `pax` instance in well under the 40 s timeout, and returns
  correct results through both access paths.

## 15. Recommended next order

1. ~~Fix the multi-row UPDATE deadlock of section 14.~~ **Done** — see section 14.
   The remaining follow-up is to decide whether PAX should eventually implement
   bottom-up index deletion rather than declining it.
1b. ~~**Finish the chunked regions of section 17.**~~ **Abandoned** — chunks were
   built (section 18) and then reverted out: they cost page density, forced
   payload compaction off, and the payoff they were built for was never built.
   The page format is back to v4 and payload compaction works again.
   Lazy materialization is therefore blocked again, on the same precondition and
   at no extra cost than before the detour. If immovability is wanted back, the
   cheap route is page-sized extents, not 32-row chunks — the granularity was the
   mistake, not the idea. See section 18.
2. **Prototype per-column compression** — the only option in section 16 with a
   measured win (0.18x-0.92x, wins 4/4 data families). Start with one table and
   one `text` column, INSERT/SELECT only, no VACUUM and no concurrency, to
   validate the concept before committing to a page format change. The known
   difficulties are the drain (compressed regions vs incremental insert),
   detoasting on the read path, and VACUUM rewriting compressed regions.
   Do **not** spend further time on: reducing `PaxTupleMetaData`, removing
   payload `MAXALIGN`, segments inside pages, or TOAST — all four are measured
   worthless or out of reach, with the arithmetic in section 16.
3. Skip slot reservation for NULL values; the bitmap already exists but the slot
   space is still allocated.
4. ~~Validate the complete required callback surface and Generic WAL path with a
   cassert build.~~ **Done for the current surface**: a cassert build exists at
   `/home/frbn/usr/local/pax-assert`, and both suites run green under it. It already
   earned its keep twice (section 14). It should become the default test target.
5. Define and implement a page-version migration/rewrite policy.
6. Add deterministic corruption, crash-recovery, and concurrent-extension tests.
7. Add MultiXact horizon maintenance: `relation_vacuum` drops `locker_mxid`
   references but the catalog horizon stays at `GetOldestMultiXactId()`, so
   `pg_class.relminmxid` never advances.
8. Add speculative insertion before ON CONFLICT work.
9. Add planner-estimation improvements last.

## 16. Storage-size options: what was measured, what was rejected

This section exists because every candidate below looks attractive from the
outside and three of them were measured and found worthless. Read the numbers
before re-deriving any of it.

### The measurement that started it

The premise worth examining: a column store should occupy less memory in
`shared_buffers` than a row store when a query touches only some columns. That
is true of real column stores and **not** of PAX, for a reason that has nothing
to do with tuning.

`shared_buffers` counts **pages, not bytes**. PostgreSQL loads all 8192 bytes of
a page whether the query needs one column from it or all twelve. ClickHouse,
Parquet and Citus columnar get the benefit because each column lives in a
**separate file**, so skipping columns means not opening their files. PAX keeps
every column of a row group in the same page, so there is no page a narrow
projection can avoid.

The consequence is that PAX cannot *win* on block count; it can only lose less.
Measured on the 12-column indexed table (`make wide-projection-benchmark`,
20,000 rows):

| projected columns | heap blocks | PAX blocks | PAX/heap | PAX time/heap |
|---|---|---|---|---|
| 1 | 1819 | 2000 | **1.100** | 2.62 |
| 2 | 1819 | 2000 | **1.100** | 2.32 |
| 11 | 1819 | 2000 | **1.100** | 0.90 |

The 1.100 is stable and structural: PAX fits 10 rows per page, heap fits 11, on
identical payload.

### Rejected: reducing `PaxTupleMetaData`

Measured from the page layout of that same table:

| zone | bytes/page | per row |
|---|---|---|
| low (metadata + slots + NULL bitmaps) | 676 | 67.6 |
| high (payload) | 7040 | 704.0 |
| total | | **771.6** |

Eleven rows per page requires `(8192 - 64) / 11 = 738.9` bytes per row, so
**32.7 bytes per row must go**. The metadata is 32 bytes: 0.7 short even at
zero, and `xmin`/`xmax`/`t_ctid` cannot be removed. Dropping it to 24 bytes
changes nothing on this schema. Closed.

### Rejected: removing `MAXALIGN` from the payload

Nothing to remove. Instrumenting `pax_alloc_payload` to log `len`,
`aligned_len` and the real `pd_upper` movement shows the value arriving from
the executor is already 8-byte aligned for `attlen == -1`:

| characters | `VARSIZE_ANY` measured | `MAXALIGN` of that |
|---|---|---|
| 55 | 59 | 64 |
| 57 | 61 | 64 |
| **60** | **64** | **64** (nothing added) |
| 61 | 65 | 72 |

At 60 characters `len` is 64, so `MAXALIGN(64) == 64`. The "33 bytes per row of
alignment waste" that an earlier draft of this document claimed does not exist.
The 704 bytes per row in the high zone is exactly `11 x 64`: the data as
PostgreSQL delivers it, zero padding. Two of the three `MAXALIGN` call sites on
this path only align `attlen == -2` (`cstring`) and leave `attlen == -1` alone,
which is why editing them had no observable effect.

### Rejected: column segments inside the same pages

Grouping 12 columns into 3 segments of 4 collapses 12 NULL bitmaps into 3, which
costs about as much as it saves (24 bytes either way), and saves 18 header bytes
per page — roughly 1.8 bytes per row against the 32.7 needed. Worse, it cannot
help at all: a shared buffer is a whole page, so segments within a page never
change the block count of a scan. Closed.

### Rejected: TOAST

`pax_relation_needs_toast_table()` returns `false` unconditionally
(`pax_am.c`), and `relation_toast_am` and `relation_fetch_toast_slice` are not
implemented. This is deliberate, and the reason is structural rather than an
oversight:

- The contract is not optional. `access/tableam.h` states that once
  `relation_needs_toast_table` may return `true`, both
  `relation_toast_am` and `relation_fetch_toast_slice` are required.
- A TOAST table is an **ordinary relation**. `heapam_relation_toast_am()`
  returns `rel->rd_rel->relam`, and the chunking, `chunk_id` allocation and
  short-value tables all live in `heaptoast.c`, writing through heap. A toasted
  value is therefore identified by a `(toast relfilenode, chunk_id, seqno)`
  triple that PostgreSQL manages.
- PAX cannot host that. A PAX column region holds **2-byte slots** — a payload
  offset — and `pax_get_value()` `memcpy`s straight out of the slot. There is
  nowhere to put an 18-byte TOAST reference and no decompression path.

Implementing it would need a slot format that can carry an external varlena,
detoasting in `pax_get_value`, a write path that allocates `chunk_id` and writes
through heap, and awareness in `pax_slot_materialize`, `pax_vacuum_page` and
payload compaction — that is, a third page format (v5 after v4).

The gain is also doubtful for this workload. TOAST exists to keep the *main*
pages thin. PAX's measured problem is not that pages are too big; it is that
columns share a page. Moving bytes out of PAX pages does nothing for the narrow
projection that is already PAX's worst case (2.62x slower than heap).

### Rejected: lazy materialization — and why the copy is mandatory

The single largest CPU cost in a PAX scan is not the scan: it is
`pax_slot_materialize()` copying all 12 columns of every row, including the 11 the
query never asked for. Removing that one line and letting `pax_slot_getsomeattrs()`
fetch on demand is a **40% win** on the narrow-projection query (20,000 rows,
12 columns, projecting one: 6.30 ms to 3.62 ms, against heap's 2.32 ms). Results
stay exact and both suites pass.

It is also a data race, and must not be shipped.

**Why heap can do it and PAX cannot.** `heap_getnextslot()` does precisely this:
`heapgettup()` releases the content lock (heapam.c:1034) and then
`ExecStoreBufferHeapTuple()` stores the tuple *without copying*. `tts_buffer_heap_store_tuple()`
(`execTuples.c:944`) keeps a pin and sets `tts_nvalid = 0`, so
`tts_buffer_heap_getsomeattrs()` extracts attributes lazily. `tuptable.h:60-64`
documents this as the contract: *"any pass-by-reference Datums point into the
physical tuple. The extracted information is built lazily, ie, only as needed."*

That contract is safe for heap because **a heap tuple never moves**. It is
allocated once with `PageAddItem`, `lp_off` never changes afterwards, and only
`lp_len` may be set to `LP_UNUSED` by a dead-tuple sweep. A pin therefore
guarantees the bytes are still there.

PAX cannot offer that guarantee. Every insert grows each column region with a
`memmove` toward higher `pd_lower` (`pax_insert_bytes`, the format comment at
pax_am.c:3975), and the row layout comment says so explicitly: *"une insertion
concurrente décale les régions en memmove"*. A pin prevents eviction, **not**
in-place modification. So `pax_get_value()` reading `mp->data + tupno * mp->stride`
without a content lock can read a pointer that a concurrent inserter has already
moved — silently, with no assertion. A concurrency test cannot demonstrate the
absence of this bug; it needs the insert to land on the same page in exactly the
right window.

The original code's comment was therefore correct and load-bearing:
*"Materialize before dropping the content lock... copied values are
mandatory."* It was removed in the experiment and must stay.

**Deadlock interaction.** The obvious workaround — keep the content lock until the
columns are read — is exactly the section 14 deadlock: the scan would hold SHARE
on a page that an `UPDATE` or `INSERT` needs EXCLUSIVE on, and the backend would
wait on its own lock.

**A safe arena variant was tried and rejected.** Replacing the 11 per-value
`datumCopy()` calls with a single `palloc` for the whole row (values compacted
byte-wise, one spare byte per value so no pointer lands on the block edge) is
safe by construction and gave only 6.30 ms to 5.72 ms, about **9%**, which does
not move the 2.5x-vs-heap penalty. It also regressed `int[]` reads: 1 of 500 rows
correct, versus 500 of 500 with per-value `datumCopy`. Instrumenting the arena
showed the on-page array measuring 36 bytes where a three-element `int[]` needs
28, so the discrepancy is in how that varlena is sized, not in the arena
arithmetic. Unresolved; not worth 9% plus an open question about exotic varlena
types.

Conclusion: the 40% is real but unreachable without a stable per-row layout. That
is the same structural gap as the block-count problem in the previous section —
**PAX's rows are not addressable independently of their page**, so PAX must copy,
and must read whole pages.

### The one option with a measured win: per-column compression

Compression must be **per column region**. Compressing whole pages would mix
columns and destroy the very adjacency that makes the layout work.

Measured by compressing each column's values independently, on the real PAX
layout, 5,000 rows x 8 columns:

| data family | PAX now | heap | PAX compressed | vs heap |
|---|---|---|---|---|
| repetitive (`repeat('a',60)`) | 358 | 334 | 61 | **0.18x** |
| md5 hex (incompressible) | 239 | 186 | 172 | **0.92x** |
| prose, English | 313 | 264 | 87 | **0.33x** |
| prose, French | 358 | 334 | 61 | **0.18x** |

PAX wins in all four, including on md5, which is incompressible by construction.
There the columnarisation alone does the work: 0.92x, not a compression win.

This is the first candidate in this section whose advantage is both measured and
robust, and it does not depend on a fragile arithmetic argument.

The real difficulties are known and are not small:

- ~~**Page format v5.**~~ Done, §18. It was a version break, as v3 -> v4 was.
- **Draining.** PAX inserts incrementally, and adding a row to a compressed
  region means decompress, append, recompress — O(n) per insert, O(n^2) per
  page. Either recompress on every insert, or keep an uncompressed tail region
  per column and compress it on a policy.
- **Read path.** `pax_get_value()` decompresses, on the hot path, per value.
- **VACUUM.** `pax_vacuum_page` and payload compaction must rewrite a compressed
  region without decompressing it, or a dead version becomes unreachable.
- **Generic WAL.** Already correct in principle — modified pages are rewritten
  wholesale — but each page write becomes substantially larger.

Prototype (one table, one `text` column, INSERT/SELECT, no VACUUM, no
concurrency): **3-5 days**. Making it correct under VACUUM, concurrency and
crash recovery: **1-2 weeks** more.

### Summary

| option | measured gain | cost |
|---|---|---|
| **per-column compression** | **0.18x - 0.92x, wins 4/4** | 3-5 d prototype, +1-2 w correctness |
| lazy materialization | **-40% CPU**, but a data race | needs a stable per-row layout |
| single-arena materialization | **-9%**, regresses `int[]` | unresolved varlena sizing |
| column segments in separate files | would reduce pages read | 3-4 w; reintroduces the section 14 deadlock risk |
| reduce metadata 32 B | **0** (0.7 B short) | page format break |
| remove payload `MAXALIGN` | **0** (nothing to remove) | - |
| segments inside pages | **0** on block count | - |
| TOAST | doubtful for this workload | 1 w + page format v5 |

The three rejected CPU options share one root cause. A row in PAX is not
addressable independently of its page, so the access method must read whole pages
and must copy values out of them. Projection cannot be pushed down into the
layout, and no amount of tuning at the slot level changes that. Only changing
what goes in the page — compressing it — or moving columns out of the page
addresses it.

## 17. Design of stable row addresses (chunked regions) — historical

> **Never landed, and now abandoned.** This is the design that became v5 and was
> reverted; see section 18 for what it cost and why it went. Kept because the
> reasoning about *why* immovability is wanted is still the best argument for it,
> and section 18 records the cheaper shape to retry.

> **Historical.** This section is the design that was written before the work.
> It is kept because §18 records what actually happened, including two layouts
> that failed. Where this section says "stage 2 is blocked", read §18 instead:
> stage 2 is done.

### The goal, and why it is the right goal

Section 16 established that lazy materialization is worth **-40% CPU** on a narrow
projection but is a data race, because a PAX row is not addressable independently of
its page. Heap gets out of this for free: a heap tuple never moves (`PageAddItem`
once, `lp_off` never changes afterwards), so `ExecStoreBufferHeapTuple()` can keep a
pin and set `tts_nvalid = 0`, and `tts_buffer_heap_getsomeattrs()` extracts
attributes on demand (`execTuples.c:944`; contract documented at
`tuptable.h:60-64`). PAX cannot offer that, and the fix is to **give a PAX row a
stable address**.

The design target: each column becomes a chain of chunks, and **a written chunk
never moves**. Then a reader holding only a pin can follow an offset safely, lazy
materialization becomes possible without a race, and the section 14 deadlock is not
reintroduced, because the scan still releases the content lock — it simply no longer
has to copy.

### The measured obstacle: chunking granularity

Chunking only pays at row-group granularity. Measured on the 12-column table
(`make wide-projection-benchmark`):

| layout | chunks/page | low zone | vs contiguous |
|---|---|---|---|
| contiguous, one region per column | - | 604 B | - |
| chunks of 8 rows, inside one page | 2 | 700 B | **+9.6 B/row** |
| chunks of 16 rows | 1 | 652 B | +4.8 B/row |
| chunks of 128 rows (row group) | ~0.2 | - | **+0.28 B/row** |
| chunks of 1024 rows | - | - | **-0.05 B/row** |

Forcing chunks to fit inside a page is the worst possible setting: the per-chunk
header and bitmap cost the most when there are the most chunks. A 128-row chunk must
be allowed to span pages.

**Rows per page is not a design constraint.** Measured at **6 to 225** depending on
schema (20 cols x 60 B gives 6; 12 cols x 8 B gives 225). An early analysis in this
session anchored on "10 rows per page", which was the single worst corner of one test
schema, and concluded that chunking adds unavoidable overhead. That was wrong. Do
not repeat it.

### The format that was being built (v5)

```c
#define PAX_PAGE_VERSION            5

typedef struct PaxChunkHdr        /* lives in the pd_upper area, never moves */
{
    OffsetNumber next_chunk;     /* next chunk of the same column */
    uint16       n_rows;         /* versions used in this chunk */
} PaxChunkHdr;

#define SizeOfPaxChunkHdr   MAXALIGN(sizeof(PaxChunkHdr))   /* 8 */
#define PAX_CHUNK_MAX_ROWS  8                               /* versions per chunk */
#define PAX_CHUNK_ENTRIES_PER_ATTR 2                          /* head and tail */
```

`PaxPageHeader.offsets[]` (one entry per column) became `PaxPageHeader.chunk_head[]`
with two entries per column: head and tail. Page header size becomes
`SizeOfPaxPageHeaderFixed + n_attrs * 2 * sizeof(OffsetNumber)`.

Reader side, `PaxMinipage` gains `chunks` and `n_chunks`:

```c
static inline char *
pax_mp_chunk(const PaxMinipage *mp, int tupno, int *in_chunk)
{
    int c = tupno / PAX_CHUNK_MAX_ROWS;
    *in_chunk = tupno % PAX_CHUNK_MAX_ROWS;
    return mp->chunks[c];
}
```

`pax_get_value()` then reads the NULL bitmap at `chunk + SizeOfPaxChunkHdr` indexed
by `in_chunk`, and the slots at
`chunk + SizeOfPaxChunkHdr + pax_bitmap_size(PAX_CHUNK_MAX_ROWS) + in_chunk * stride`.
The values themselves, at `pd_upper`, were **already append-only** in v4 and never
moved; only the slot tables used to move. Making the tables stable is the whole
trick.

The chain is walked once per layout refresh and cached, so `pax_get_value()` stays
O(1). Validation must be strict, because a corrupt chain reads garbage silently:
offset inside `[pd_upper, pd_special)`, `n_rows <= PAX_CHUNK_MAX_ROWS`, and chain
length at most `n_tuples / PAX_CHUNK_MAX_ROWS + 2` (which also bounds a cyclic
chain).

### Stage 1 — DONE and verified

Reading through the chunk indirection was implemented and checked before the format
was bumped, with the on-disk layout still v4 (one chunk covering the whole region,
`rows_per_chunk = PAX_CHUNK_MAX_ROWS`, `chunk_has_hdr = false`). That validated the
indirection with **zero on-disk change**: results identical to heap, 0 differences
over 3000 rows, both suites green. This part is reusable and does not need redoing.

### Stage 2 — BLOCKED, and the segfault that explains why

The first attempt put chunks in the contiguous `[meta_offset, pd_lower)` area. That
**segfaulted on read**, and the cause is the crux of the whole exercise:

`pax_insert_bytes()` inserts 32 bytes of version metadata per row with a `memmove`
of the whole zone above `meta_at`, and it fixed up **only `phdr->offsets[]`**. Chunk
headers contain a `next_chunk` offset, that is a pointer stored in the page itself.
The `memmove` moved those headers without correcting them, so the chain was corrupt
and `pax_get_value()` followed a wild pointer.

This is exactly why the v5 design allocates chunks from **`pd_upper` downward**: that
zone is never touched by the `memmove`, because `GenericXLogFinish()` copies
`[0, pd_lower)` from the image, zeroes `[pd_lower, pd_upper)`, and copies
`[pd_upper, BLCKSZ)` from the image. `pax_insert_bytes()` in v5 therefore has nothing
to fix up in the page header at all.

### Stage 2 — where it stopped, precisely

With chunks on the `pd_upper` side, a **single-row insert succeeds** and the second
insert fails with `pax: invalid page 0`. Instrumenting `pax_alloc_chunk()` gives, for
the first insert into a one-column `int` table:

```
need=(48) start=(8136) pd_lower=(80) pd_upper=(8136)
special=(8184) version=(5) magic=(20545)
```

The first chunk occupies `[8136, 8184)` and the special area starts at 8184: correct,
no overflow, and the special area still holds version 5 and the right magic. So the
first insert is sound and the failure is on the second.

**Unresolved, all eliminated. Do not re-run these:**

- chunk overflowing into the special area — the log above shows it stops exactly at
  8184
- `header_size` reserving `n_attrs` entries while the init loop wrote
  `2 * n_attrs` (real bug, fixed)
- calling `pax_alloc_chunk()` twice for one allocation (real bug, fixed)
- the alignment gap between chunks being wiped by Generic WAL (fixed by allocating
  exactly `MAXALIGN(need)` with no gap, since `GenericXLogFinish()` copies downward
  from `pd_upper`)
- `pax_alloc_payload()` writing over the special area (real bug, fixed)

**A fact noticed but not exploited, and the most promising lead:**
`PAX_CHUNK_MAX_ROWS = 8` gives `need = 8 + 1 + 8 * 4 = 41`, rounded up to 48. That
fills `[8136, 8184)` **exactly**, with zero margin against the special area. One byte
of slack. If the corruption is in the chunk allocation arithmetic, this is where to
look: try a different `PAX_CHUNK_MAX_ROWS`, and log `pd_lower` / `pd_upper` before and
after the metadata `pax_insert_bytes()` on the second insert, not only inside
`pax_alloc_chunk()`.

Raw-page reads of a PAX relation through `get_raw_page()` were **not trustworthy**
during this session: `pd_special` decoded to values like 1 and 8144 on pages that
were demonstrably valid, and a control table with a known-good header "failed". Debug
by behaviour, that is, does a query return the right answer, not by decoding page
bytes.

### Build and test state

```bash
export PGENVWRAPPERRC="$HOME/.config/pgenvwrapperrc"
source /home/frbn/git/dalibo/wrapper_pgenv/pg_env.sh; pgenv pax; pg path pax
cd /home/frbn/git/franck/pax_am
make && make install && make installcheck      # both suites green
```

A `--enable-cassert` build exists at `/home/frbn/usr/local/pax-assert` (cluster data
`/tmp/opencode/paxdata2`, socket `/tmp/opencode`, port 5499). **Run the cassert build
for this work.** It is what turned the section 14 hang into a backtrace, and this
stage-2 corruption is exactly the kind of imbalance it asserts on.

Note: `CREATE INDEX` on a PAX table needs
`SET max_parallel_maintenance_workers = 0`, or it errors with
`pax table AM does not support parallel table scans`.

Also: `getenv()` does not work for reading a flag from inside the AM. A backend
inherits the **postmaster's** environment, not the client's, so `getenv` sees nothing.
Use an unconditional `elog`, or a custom GUC.

### Method lessons from this session

Three of my own analyses produced **wrong conclusions** and should not be repeated:

1. **A "33 bytes per row of MAXALIGN waste"** that did not exist. The value arriving
   from the executor is already 8-byte aligned for `attlen == -1`: `VARSIZE_ANY` of a
   60-character text is 64, not 61, so `MAXALIGN(64) == 64`. Two of the three
   `MAXALIGN` call sites on that path only handle `attlen == -2` (cstring). This cost
   a full build and install cycle to discover, because `make install` failed
   silently on a leftover compile error and the instrumented `.so` was never the one
   loaded.
2. **A lock-depth counter and a per-buffer lock tracker**, both wrong: the first
   because it conflated pin-only releases with content-lock releases, the second
   because it never ran, for the same stale-library reason.
3. **Anchoring a design on one schema's density**, as described above.

The reliable instruments, in order: a `--enable-cassert` build, then behaviour, then
arithmetic computed from measurements. Never arithmetic from assumptions, and always
confirm that the installed binary actually changed.

A fourth, added by §18: **measuring a format constant against the case at hand
instead of the worst case it must survive.** `PAX_CHUNK_MAX_ROWS` was picked at 64
from a sweep over 1 to 12 `int4` columns, where it looked optimal — and it makes
a 20-column table unable to fill a page at all, because the chunk set needs
8384 bytes where a page holds 8192. The per-column chunk was a harmless 272
bytes; it was the sum over twenty columns that had to fit, and nobody summed it.
Widening the tests from two columns to twenty is what found it.

### Where this leaves the options

| option | measured gain | cost | state |
|---|---|---|---|
| **per-column compression** | **0.18x - 0.92x, wins 4/4** | 3-5 d prototype, +1-2 w | not started |
| stable row addresses | **-40% CPU** expected, unmeasured since §18 | done | **v5, §18** |
| column segments in separate files | would reduce pages read | 3-4 w; section 14 deadlock risk | not started |
| reduce metadata 32 B | **0** (0.7 B short) | page format break | rejected, section 16 |
| remove payload `MAXALIGN` | **0** (nothing to remove) | - | rejected, section 16 |
| segments inside pages | **0** on block count | - | rejected, section 16 |
| TOAST | doubtful for this workload | 1 w + format change | rejected, section 16 |

Stage 2 was finished; see §18 for what v5 is and what it cost. Recommended order
for the next session: **stage 3**, re-measure lazy materialization now that row
addresses are stable, and keep the content lock released across `getnextslot`
as it is today. Only after that measurement, drop the `ExecMaterializeSlot()`
call. Per-column compression remains the fallback with a measured win today.

## 18. Page format version 5: chunks, tried and reverted

**REVERTED, 2026-10-07.** `PAX_PAGE_VERSION` was put back to 4 and `pax_am.c` no
longer contains a single chunk symbol. This section is kept as the record of what
was built, what it cost, why it went, and what would have to be true to bring
immovability back for less. It describes a format that is **not** the one in the
tree.

### What v5 was

`PaxPageHeader.offsets[]` was replaced by a **chain of chunks** per column:

```c
typedef struct PaxChunkHdr        /* lived in the arena, never moved */
{
    OffsetNumber next_chunk;     /* next chunk of the same column, 0 = end */
    uint16       n_rows;         /* versions used in this chunk */
} PaxChunkHdr;                    /* 8 bytes MAXALIGNed */

#define PAX_CHUNK_MAX_ROWS  32
```

A chunk held, for one column, the NULL bitmap plus `PAX_CHUNK_MAX_ROWS` slots,
its size depending only on the column stride and never on how many versions it
held. That is what made it allocatable **once, at full size**: a chunk was
written once and nothing ever moved it, so a row's address was as stable as a
heap tuple's `lp_off`. The header kept head and tail per column, so adding a
chunk was O(1) without walking the chain.

`tupno` mapped to `(chunk = tupno / PAX_CHUNK_MAX_ROWS, rank = tupno %
PAX_CHUNK_MAX_ROWS)`. `pax_insert_bytes()` was deleted.

### Why it was tried

One reason: heap can lazily extract attributes because a heap tuple never
moves, and v4 could not offer the same guarantee because a column region is
memmov'd on every insert. §16 measured the prize at -40% CPU on a
narrow-projection query. Chunks were the only arrangement found that delivered
immutability without a compaction pass.

### Why it went

The premise turned out to be weaker than the cost, on two counts.

**The density cliff.** A chunk set is allocated for *all* columns the moment the
first row opens it, so one page holds `PAX_CHUNK_MAX_ROWS` rows and not one
more - the next row would need a fresh chunk for every column at once. Measured
at 20 columns: **32.0 rows per page against heap's 41.8**. Rows per page was
pinned to the chunk size, not to the data, and lowering the constant did not
recover density, it only moved the cliff closer.

**Payload compaction had to be disabled.** Chunks and payloads shared the single
descending frontier above `pd_lower` so that neither could walk into the other.
Two earlier arrangements failed, both with the same plausible symptom: two
separate frontiers let compaction walk through the chunks, and growing payloads
upward from `pd_lower` collided with the metadata region. The resolution was to
interleave them on one descent, and the consequence was that **payload
compaction became unsafe**. `pax_vacuum_compact_payload()` was disabled and
`chunk_floor` kept only as the bound a future compaction would need. Dead
payload bytes stopped being reclaimed.

The headers were never the problem - a chunk costs exactly 16 bytes per column
per 32 rows, about 4% of a page. The two structural costs above are what the
format actually bought.

### What the revert gained, measured

The 20-column regression suite runs unchanged and two page counts improve:

| measurement | with chunks (v5) | reverted (v4) |
|---|---|---|
| `tpax_reuse` pages, 10 500 rows | 469 | **260** (-45%) |
| `tpax_truncate` pages, 2 000 rows | 63 | **48** (-24%) |
| 12 col `int4`, page ratio vs heap | 1.211 | **1.105** |
| 12 col `text`, page ratio vs heap | 2.250 | **2.125** |
| 20 col mixed, page ratio vs heap | 1.340 | **1.170** |

The 45% on `tpax_reuse` is mostly restored payload compaction, not raw density:
that table churns 4 500 UPDATEs, so with compaction off it carried 4 500 dead
payloads forever. `mismatched` stays 0 on every column check, so nothing was
traded away in correctness.

PAX still runs above heap on all three - 1.10 to 2.13 - and that gap is not the
chunking. It is the columnar layout itself: 32 bytes of metadata per version,
varlena payloads moved out of line and MAXALIGNed one at a time.

### What was kept from the revert

Two bugs that only chunks exposed, both fixed before reverting so they would
still bite on v4:

- **The space hint is now an over-estimate, not a lower bound.** In v4 it was
  deliberately a lower bound ("bitmap delta at zero"). That is a liveness bug in
  the page-selection loop: a page holding between `hint` and `needed` bytes gets
  proposed, rejected under the exclusive lock, returned, and re-proposed
  forever. The hint now counts the worst bitmap growth per column, which closes
  that interval.
- **The hint is saturated at `PaxMaxFSMRequestSize` (8160 B).** The FSM rejects
  anything larger with `invalid FSM request size`, and at 20 columns a wide row
  exceeds it. Saturating is still an over-estimate, so termination is preserved
  and the honest `row is too large for one page` diagnostic survives.

### What immovability would take, if it is wanted back

The version is reverted, so this is a decision for later, but the cheap route is
now clear and it is **not** the one v5 took. The mistake was the granularity: a
chunk of 32 rows pays its 16 bytes over 32 rows, while heap's equivalent - the
multi-block line pointer - pays its overhead over one to eight **8 kB pages**.
Scaling the same idea to page-sized extents would put header cost near 0.2%,
leave rows-per-page governed by the data again, and still deliver immovable
addresses. That is the shape to try, and the reason the idea was not wrong so
much as badly parameterised.

### Still open

- **Lazy materialization remains off** and is now further away, not closer: it
  needs immovable addresses, and v4 does not provide them. The -40% of section 16
  is blocked on the same precondition, at no extra cost today than before the
  detour. Re-measured on a 20-column table it would be worth about -60% — a
  1-column projection is 25.4 ms against 10.0 ms for the same table at 2
  columns, since `count(*)`, 1 column and 20 columns all cost the same today —
  but that is a ceiling, not a promise, and it is unreachable without
  immovability.

## 19. Storage parameters: `fillfactor` and `toast_tuple_target`

Every table in `sql/pax_am.sql` and `specs/pax_mvcc.spec` is created
`WITH (fillfactor = 80, toast_tuple_target = 512)`. Three separate facts, easy
to conflate.

**1. PostgreSQL accepts and records them.** Both are `RELOPT_KIND_HEAP` options
in `default_reloptions()`, so they validate and store for any `RELKIND_RELATION`
whatever its AM, bounds included: `fillfactor` in [10, 100], `toast_tuple_target`
in [128, 8160].

**2. PAX injects no defaults, and cannot.** A table created without the clause
records nothing, not even heap's 100 / 2040. That is a platform limit:

- `TableAmRoutine` in PG19 has no options callback. Index AMs have `amoptions`;
  table AMs have nothing to match it with.
- `DefineRelation()` validates and passes `reloptions` into
  `heap_create_with_catalog()`, which writes the `pg_class` row. The AM is
  entered from `heap_create()` → `table_relation_set_new_filelocator()`, i.e.
  *before* that write, and `reloptions` is not passed to it. By the time PAX
  runs, `pg_class` is committed to a value PAX did not choose.

So the clause must be written on every `CREATE TABLE`. **It is not inherited**:
neither `LIKE` nor `CREATE TABLE AS` copies reloptions, not even
`LIKE ... INCLUDING ALL`, not `INCLUDING STORAGE`. Since nearly every table in
the suite is built with `LIKE tpax ...`, trusting inheritance would have left the
rest of the suite with nothing. The regression test asserts this directly, and a
catalog sweep near its end fails if any PAX table lacks them.

**3. PAX records them and does not obey them.** `rel->rd_options` is never read.

`toast_tuple_target` cannot be honoured without TOAST, which PAX has none of:
`pax_relation_needs_toast_table()` answers `false`, so a value of any size is
stored inline in the arena and read back intact. There is no out-of-line storage
for the option to redirect to.

`fillfactor` could be honoured — a v4 region grows on demand, so there is a
packing decision to cap — and it is left unimplemented deliberately. Under the
chunked format of §18 the option had a measured cliff, because a page was
already more than half consumed by its chunk set before the first row landed:
at 20 columns the chunk set was 4352 B, ~55% of the usable area, so an 80% cap
changed nothing (26 and 63 pages either way) while 30% collapsed the table to
one row per page (61 and 2000 pages).

That cliff belonged to the chunk set and is gone with it. **The effect on v4 is
not measured** — a first attempt to cap page consumption there reproduced none
of it, which means the accounting differs enough that the v5 figures do not
transfer, and redoing it properly is not worth doing for an option that is not
implemented. What would remain is the ordinary trade: reserved free space buys
later versions a chance to land on the same page, and costs the space it holds.
So the tests state the current behaviour — `fillfactor` recorded, not obeyed —
rather than letting the clause imply the option is live.

## 20. Test tables

Every table has the same shape: 20 columns — primary key, several integer
widths, floats, text — with one column per slot stride the format distinguishes
(1, 2, 4, 8, 16). That list is what matters, not the type names: PAX copies
`Datum` bytes and has no per-type code. The columns that vanished from the
original two-column suite (`time`, `oid`, bare `numeric`) each sat on a stride
already covered twice.

The extra assertions came from the width itself, which is what found the two
bugs in section 18. At 20 columns a check on one column could no longer
distinguish "the row is right" from "the right columns were gathered":

- the VACUUM check verifies a float, a `bigint`, a `text` and **two independent
  NULL bits** alongside the rewritten one;
- the space-reclamation check verifies numeric, text and NULL columns, not just
  the reused one, so a slot reused with a predecessor's payload fails;
- the isolation spec aggregates a concatenation of the non-key columns next to
  the keys, so every MVCC step asserts on all 20. A scan returning the right keys
  with a neighbour's values is invisible to a key-only check.

Both suites stay green on the cassert and the plain build.

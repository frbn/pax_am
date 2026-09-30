--
-- pax_am: extension / table access-method test
--
-- Part 1 registers the access method, part 2 exercises the full table
-- lifecycle through the pax AM: CREATE / INSERT / SELECT / index-free
-- scans / pg_relation_size / INSERT ... SELECT / TRUNCATE / DROP,
-- including a round-trip over one row of many different data types.
--

CREATE EXTENSION pax_am;

-- the extension must register exactly one table access method
SELECT amname, amhandler
FROM pg_am
WHERE amtype = 't'
ORDER BY amname;

-- and its handler must be the C function shipped by the extension
SELECT amhandler = 'pax_tableam_handler(internal)'::regprocedure AS handler_ok
FROM pg_am
WHERE amname = 'pax';

-- extension metadata
SELECT extname, extversion FROM pg_extension WHERE extname = 'pax_am';

--
-- table lifecycle (this used to segfault: relation_set_new_filelocator
-- was NULL in pax_methods)
--
CREATE TABLE tpax (a int, b text) USING pax;

-- the table must be registered on the pax access method
SELECT a.amname
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
WHERE c.relname = 'tpax';

-- a fresh table is empty
SELECT count(*) FROM tpax;

INSERT INTO tpax VALUES (1, 'hello'), (2, NULL);

-- storage must actually have been created (relation_size callback).
-- NB: checked after INSERT — a brand-new relation has 0 blocks.
SELECT pg_relation_size('tpax') > 0 AS has_storage;

SELECT count(*) FROM tpax;

-- values must round-trip: fixed-length, varlena and NULL
SELECT a, b FROM tpax ORDER BY a;

-- TID offset is tupno + 1, so the first tuple is ctid (0,1)
SELECT ctid, a FROM tpax ORDER BY a LIMIT 1;

--
-- many rows: forces several pages in pax_tuple_insert and
-- grows the per-column NULL bitmap past its first 64-tuple capacity
--
INSERT INTO tpax SELECT g, 'row ' || g FROM generate_series(3, 502) g;
SELECT count(*) FROM tpax;
SELECT count(*) FROM tpax WHERE b LIKE 'row %';
SELECT a, b FROM tpax WHERE a = 250;
SELECT pg_relation_size('tpax') > 16384 AS spans_pages;

--
-- INSERT ... SELECT from another pax table: exercises the slot
-- materialize/copyslot path on top of the scan
--
CREATE TABLE tpax_copy (a int, b text) USING pax;
INSERT INTO tpax_copy SELECT a, b FROM tpax;
SELECT count(*) FROM tpax_copy;
SELECT (SELECT count(*) FROM tpax) = (SELECT count(*) FROM tpax_copy) AS same_count;
SELECT a, b FROM tpax_copy WHERE a = 1;

-- COPY uses the required multi_insert callback; PAX implements it as the same
-- columnar append path used by tuple_insert.
CREATE TABLE tpax_copy_multi (a int, b text) USING pax;
COPY tpax_copy_multi FROM STDIN;
1	one
2	two
\.
SELECT a, b FROM tpax_copy_multi ORDER BY a;
DROP TABLE tpax_copy_multi;

--
-- one row covering every storage shape: by-value columns of length
-- 1/2/4/8, by-reference fixed (uuid, interval), varlena (text, varchar,
-- bytea, numeric), plus a second row that is all NULL (null bitmap).
--
-- pg_regress exports PGDATESTYLE=Postgres, MDY: pin the rendering here so
-- the expected output does not depend on that environment.
--
CREATE TABLE tpax_types (
    c_bool bool, i2 smallint, i4 int, i8 bigint, f4 real, f8 double precision,
    num numeric, txt text, vc varchar(20), ba bytea, uu uuid,
    d date, t time, ts timestamp, iv interval, ch "char", oidv oid
) USING pax;

SET datestyle = 'ISO, MDY';
SET intervalstyle = 'postgres';

INSERT INTO tpax_types VALUES (
    true, -12345, -123456789, 1234567890123, 1.5, -0.25,
    1234.56, 'hello pax', 'varchar value', '\x0102a0',
    '123e4567-e89b-12d3-a456-426614174000',
    '2024-01-15', '13:45:30', '2024-01-15 13:45:30', '00:01:30', 'x', 42);

SELECT * FROM tpax_types;

INSERT INTO tpax_types (i4, txt) VALUES (7, NULL);
SELECT i4, txt FROM tpax_types ORDER BY i4;
SELECT count(*) FROM tpax_types;

--
-- basic MVCC: rows created by earlier commands in the current transaction are
-- visible to that transaction, while rows from an aborted transaction are
-- hidden from later snapshots.  System transaction attributes are stored too.
--
CREATE TABLE tpax_mvcc (id int) USING pax;
INSERT INTO tpax_mvcc VALUES (0);

-- exact-TID and TID-range scans use the versioned table-AM callbacks
SELECT ctid, id FROM tpax_mvcc WHERE ctid = '(0,1)';
SELECT array_agg(id ORDER BY id) AS tid_range_ids
FROM tpax_mvcc
WHERE ctid BETWEEN '(0,1)' AND '(0,1)';

BEGIN;
INSERT INTO tpax_mvcc VALUES (1);
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
SELECT id,
       xmin = pg_current_xact_id()::text::xid AS own_xmin,
       xmax = '0'::xid AS no_xmax,
       cmin IS NOT NULL AS has_cmin,
       cmax IS NOT NULL AS has_cmax,
       cmin = cmax AS shared_command
FROM tpax_mvcc
WHERE id = 1;
INSERT INTO tpax_mvcc VALUES (2);
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
ROLLBACK;

-- the aborted transaction's physical rows remain invisible
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
SELECT count(*) FROM tpax_mvcc WHERE ctid = '(0,2)';

INSERT INTO tpax_mvcc VALUES (3);
INSERT INTO tpax_mvcc VALUES (4)
RETURNING id, xmin <> '0'::xid AS has_xmin,
          xmax = '0'::xid AS no_xmax, cmin = cmax AS shared_command;
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
SELECT id, xmin <> '0'::xid AS has_xmin, xmax = '0'::xid AS no_xmax
FROM tpax_mvcc
ORDER BY id;
DROP TABLE tpax_mvcc;

--
-- versioned UPDATE / DELETE: old physical versions retain xmin/xmax/cmax
-- and a forward t_ctid, while normal MVCC scans expose only the newest
-- committed version.
--
CREATE TABLE tpax_dml (id int, payload text) USING pax;
SELECT pg_current_wal_insert_lsn() AS before_insert_wal \gset
INSERT INTO tpax_dml VALUES (1, 'old'), (2, 'keep');
SELECT pg_current_wal_insert_lsn() > :'before_insert_wal'::pg_lsn
       AS insert_wrote_wal;
SELECT ctid AS old_tid FROM tpax_dml WHERE id = 1 \gset
SELECT pg_current_wal_insert_lsn() AS before_update_wal \gset

UPDATE tpax_dml
SET payload = 'new'
WHERE id = 1
RETURNING id, payload, ctid <> :'old_tid' AS relocated,
          xmin <> '0'::xid AS has_xmin,
          xmax = '0'::xid AS no_xmax,
          cmin = cmax AS shared_command;
SELECT pg_current_wal_insert_lsn() > :'before_update_wal'::pg_lsn
       AS update_wrote_wal;
SELECT array_agg(id ORDER BY id) AS ids,
       array_agg(payload ORDER BY id) AS payloads
FROM tpax_dml;

BEGIN;
UPDATE tpax_dml SET payload = 'rolled back' WHERE id = 1;
SELECT payload FROM tpax_dml WHERE id = 1;
ROLLBACK;
SELECT payload FROM tpax_dml WHERE id = 1;

-- a second update follows the version chain; rolling back the subtransaction
-- restores the version written by the first update
BEGIN;
UPDATE tpax_dml SET payload = 'first' WHERE id = 1;
SAVEPOINT pax_update_sp;
UPDATE tpax_dml SET payload = 'second' WHERE id = 1;
SELECT payload FROM tpax_dml WHERE id = 1;
ROLLBACK TO pax_update_sp;
SELECT payload FROM tpax_dml WHERE id = 1;
COMMIT;
SELECT payload FROM tpax_dml WHERE id = 1;

-- A current logical row lock is carried to the replacement version and is
-- reflected by the UPDATE RETURNING slot.  Heavyweight tuple locks are only
-- short-lived waiter arbitration and are not retained.
BEGIN;
SELECT pg_current_wal_insert_lsn() AS before_lock_wal \gset
SELECT id FROM tpax_dml WHERE id = 2 FOR UPDATE;
SELECT pg_current_wal_insert_lsn() > :'before_lock_wal'::pg_lsn
       AS lock_wrote_wal;
UPDATE tpax_dml SET payload = 'carried lock'
WHERE id = 2
RETURNING payload, xmax <> '0'::xid AS lock_visible;
SELECT count(*) = 0 AS no_heavyweight_tuple_lock
FROM pg_locks WHERE locktype = 'tuple';
ROLLBACK;
SELECT payload FROM tpax_dml WHERE id = 2;

SELECT pg_current_wal_insert_lsn() AS before_delete_wal \gset
DELETE FROM tpax_dml
WHERE id = 1
RETURNING id, payload,
          xmax = pg_current_xact_id()::text::xid AS deleting_xid,
          cmax IS NOT NULL AS has_cmax;
SELECT pg_current_wal_insert_lsn() > :'before_delete_wal'::pg_lsn
       AS delete_wrote_wal;
SELECT array_agg(id ORDER BY id) AS remaining_ids FROM tpax_dml;

BEGIN;
DELETE FROM tpax_dml WHERE id = 2;
SELECT count(*) AS visible_during_delete FROM tpax_dml;
ROLLBACK;
SELECT array_agg(id ORDER BY id) AS ids_after_rollback FROM tpax_dml;

-- A replacement that cannot fit is rejected after reserving the old version;
-- rolling back that subtransaction makes the old version current again.
BEGIN;
SAVEPOINT pax_large_update_sp;
UPDATE tpax_dml SET payload = repeat('x', 10000) WHERE id = 2;
ROLLBACK TO pax_large_update_sp;
SELECT payload FROM tpax_dml WHERE id = 2;
COMMIT;

-- Multiple join matches in one command produce TM_SelfModified after the
-- first write; PostgreSQL must ignore the duplicate without a second version.
UPDATE tpax_dml AS a
SET payload = a.payload || ' once'
FROM tpax_dml AS b
WHERE a.id = b.id AND a.id = 2 AND b.id IN (2, 2);
SELECT payload FROM tpax_dml WHERE id = 2;
DELETE FROM tpax_dml AS a
USING tpax_dml AS b
WHERE a.id = b.id AND a.id = 2 AND b.id IN (2, 2);
SELECT count(*) AS rows_after_duplicate_delete FROM tpax_dml;
DROP TABLE tpax_dml;

-- Force a replacement version onto another page and verify that the 32-bit
-- block plus 16-bit offset t_ctid link is followed by later version locking.
CREATE TABLE tpax_chain (id int, payload text) USING pax;
INSERT INTO tpax_chain
SELECT g, repeat('x', 50) FROM generate_series(0, 99) g;
SELECT ctid AS old_tid FROM tpax_chain WHERE id = 0 \gset
UPDATE tpax_chain
SET payload = repeat('y', 7000)
WHERE id = 0
RETURNING length(payload) AS new_length,
          split_part(trim(both '()' from ctid::text), ',', 1)::integer <>
          split_part(trim(both '()' from :'old_tid'), ',', 1)::integer
          AS changed_block;
SELECT count(*) AS chain_rows,
       max(length(payload)) FILTER (WHERE id = 0) AS updated_length
FROM tpax_chain;
DROP TABLE tpax_chain;

-- ANALYZE (scan_analyze_next_block / scan_analyze_next_tuple)
CREATE TABLE tpax_analyze (id int, grp int, val numeric) USING pax;
INSERT INTO tpax_analyze
SELECT g, g % 10, g * 1.5 FROM generate_series(1, 3000) g;
-- Three tenths of the rows die, so VACUUM/ANALYZE must report them as dead.
DELETE FROM tpax_analyze WHERE id % 10 < 3;
ANALYZE tpax_analyze;
SELECT count(*) AS analyzed_rows
FROM tpax_analyze;
SELECT attname, null_frac
FROM pg_stats
WHERE tablename = 'tpax_analyze' AND attname = 'id';
DROP TABLE tpax_analyze;

-- VACUUM (relation_vacuum callback)
--
-- Dead versions are marked in place rather than compacted: t_ctid links are
-- physical (page, version index), and removing a version would shift every
-- following slot in every column and break inbound links from other pages.
-- A stale link must therefore resolve to an invisible version, not to a
-- shifted one.
CREATE TABLE tpax_vacuum (id int, payload text) USING pax;
INSERT INTO tpax_vacuum
SELECT g, 'v' || g FROM generate_series(1, 2000) g;

-- Build a chain three deep, then delete the head: after VACUUM the two
-- predecessors are reclaimed while the live version must stay reachable.
UPDATE tpax_vacuum SET payload = payload || '-u1' WHERE id <= 500;
UPDATE tpax_vacuum SET payload = payload || '-u2' WHERE id <= 200;
DELETE FROM tpax_vacuum WHERE id <= 50;

VACUUM tpax_vacuum;

-- The surviving chain must still read back exactly as before.
SELECT count(*) AS vacuum_rows,
       count(*) FILTER (WHERE payload <> expected) AS mismatched
FROM (
    SELECT payload,
           CASE WHEN id <= 200 THEN 'v' || id || '-u1-u2'
                WHEN id <= 500 THEN 'v' || id || '-u1'
                ELSE 'v' || id END AS expected
    FROM tpax_vacuum
) s;

-- A row lock and an update must still follow the chain after vacuum.
BEGIN;
SELECT id FROM tpax_vacuum WHERE id = 300 FOR UPDATE;
COMMIT;
UPDATE tpax_vacuum SET payload = payload || '-u3' WHERE id = 300;
SELECT payload FROM tpax_vacuum WHERE id = 300;

-- Fully empty trailing pages are truncated away.
CREATE TABLE tpax_truncate (id int, payload text) USING pax;
INSERT INTO tpax_truncate
SELECT g, 'v' || g FROM generate_series(1, 2000) g;
SELECT pg_relation_size('tpax_truncate') / 8192 AS pages_before_vacuum
FROM tpax_truncate LIMIT 1;
DELETE FROM tpax_truncate;
VACUUM tpax_truncate;
SELECT count(*) AS truncated_rows,
       pg_relation_size('tpax_truncate') / 8192 AS pages_after_vacuum
FROM tpax_truncate;
DROP TABLE tpax_vacuum;
DROP TABLE tpax_truncate;

-- TRUNCATE (relation_nontransactional_truncate callback)
TRUNCATE tpax;
SELECT count(*) FROM tpax;

DROP TABLE tpax;
DROP TABLE tpax_copy;
DROP TABLE tpax_types;

DROP EXTENSION pax_am;

-- a second CREATE/DROP cycle must work (extension is relocatable)
CREATE EXTENSION pax_am;
DROP EXTENSION pax_am;

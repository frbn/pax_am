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

-- exact-TID fetch uses the same snapshot visibility routine
SELECT ctid, id FROM tpax_mvcc WHERE ctid = '(0,1)';

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

--
-- pax_am: extension / table access-method test
--
-- Part 1 registers the access method, part 2 exercises the full table
-- lifecycle through the pax AM: CREATE / INSERT / SELECT / index-free
-- scans / pg_relation_size / INSERT ... SELECT / TRUNCATE / DROP,
-- including a round-trip over one row of many different data types.
--
-- Toutes les tables de ce fichier ont la meme forme : 20 colonnes, une clef
-- primaire, des entiers, des flottants et du texte. C'est deliberé.
--
-- Le format v5 range les slots par colonnes dans des chunks de taille fixe
-- (PAX_CHUNK_MAX_ROWS = 32). Un jeu de chunks occupe
-- n_attrs x (16 + 32 x pas), et il doit tenir dans une page : a 20 colonnes
-- melangees cela fait 4352 octets, contre 8192 disponibles. Une table large
-- exerce donc la contrainte qui compte, alors que deux colonnes ne verifient
-- que la partie triviale de la geometrie.
--
-- Les colonnes couvrent tous les pas de slot que le format distingue :
--   1 o  (bool, "char")        2 o  (text, numeric, varlena)
--   4 o  (int4, real, date)   8 o  (int8, float8, timestamp)
--  16 o  (interval, uuid)
--
-- Ce sont les pas qui comptent, pas les noms de type : PAX copie des octets de
-- Datum et n'a pas de code par type. Les colonnes de la version a 2 colonnes
-- qui ont disparu -- time (8 o), oid (4 o), varchar (2 o), numeric sans
-- précision (2 o) -- retombaient chacune sur un pas déjà couvert deux fois.
-- Un type d'un pas inédit serait en revanche un vrai manque, d'où cette liste.
--
-- Chaque table porte aussi les mêmes paramètres de stockage :
--
--   WITH (fillfactor = 80, toast_tuple_target = 512)
--
-- Le WITH est répété partout, et c'est délibéré. Ni LIKE (même avec INCLUDING
-- ALL ou INCLUDING STORAGE) ni CREATE TABLE AS ne recopient reloptions, et
-- PostgreSQL 19 ne donne pas à un AM de table le moyen d'injecter ses propres
-- valeurs par défaut : TableAmRoutine n'a pas de callback d'options, et
-- DefineRelation() valide et enregistre reloptions avant que l'AM ne soit
-- appelé. Donc une table dérivée par LIKE n'aurait rien, et il faudrait l'écrire
-- -- le test de la section « paramètres de stockage » plus bas verrouille ce
-- comportement pour qu'on ne le croie pas involontairement.
--
-- Voir la section « paramètres de stockage » pour ce que ces deux valeurs font
-- et ne font pas aujourd'hui : PAX les enregistre, et n'en obéit pas encore.
--
CREATE EXTENSION pax_am;

-- A parallel index build drives the table AM from more than one worker, and
-- PAX does not support concurrent insertion into the same page from two
-- backends. Single-worker builds only, so the tests measure one thing at
-- a time.
SET max_parallel_maintenance_workers = 0;

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
-- storage parameters
--
-- Every table below is created WITH (fillfactor = 80, toast_tuple_target =
-- 512). This section pins down three separate things, which are easy to
-- conflate: that PostgreSQL accepts and records the values, that they can be
-- changed afterwards, and what the PAX AM currently does with them. The third
-- is "nothing", and that is asserted rather than assumed.
--

-- Recorded verbatim, in the order given.
CREATE TABLE tpax_opt (id int, v text)
USING pax WITH (fillfactor = 80, toast_tuple_target = 512);
SELECT reloptions FROM pg_class WHERE relname = 'tpax_opt';

-- A table created WITHOUT the clause has no reloptions at all: PAX does not
-- inject defaults of its own. It cannot. TableAmRoutine has no options
-- callback in PG19, and DefineRelation() validates and stores reloptions
-- before table_relation_set_new_filelocator() ever runs, so by the time the AM
-- is entered pg_class is already committed to a value it did not choose.
CREATE TABLE tpax_opt_bare (id int, v text) USING pax;
SELECT coalesce(reloptions::text, '(none)') AS reloptions FROM pg_class
WHERE relname = 'tpax_opt_bare';

-- Nothing is recorded, not even a default. PostgreSQL's own fallbacks
-- (fillfactor 100, toast_tuple_target TOAST_TUPLE_THRESHOLD) live in
-- rd_options, in memory, and are only visible to the AM -- there is no SQL to
-- read them back through, which is exactly why the empty catalog row above is
-- the only evidence available from a test.
SELECT reloptions IS NULL AS nothing_recorded_for_a_bare_table
FROM pg_class WHERE relname = 'tpax_opt_bare';

-- Changing them after the fact goes through ALTER TABLE, like any table.
ALTER TABLE tpax_opt SET (fillfactor = 60);
SELECT reloptions FROM pg_class WHERE relname = 'tpax_opt';
ALTER TABLE tpax_opt RESET (fillfactor);
SELECT reloptions FROM pg_class WHERE relname = 'tpax_opt';

-- Out-of-range values are refused by PostgreSQL's own bounds, at CREATE and at
-- ALTER alike. Recorded here because the bounds differ per option and a
-- regression in either would show up as a golden change and nothing else.
CREATE TABLE tpax_opt_bad (id int) USING pax WITH (fillfactor = 101);
CREATE TABLE tpax_opt_bad (id int) USING pax WITH (fillfactor = 9);
CREATE TABLE tpax_opt_bad (id int) USING pax WITH (toast_tuple_target = 127);
CREATE TABLE tpax_opt_bad (id int) USING pax WITH (toast_tuple_target = 8161);
ALTER TABLE tpax_opt SET (toast_tuple_target = 1);

-- LIKE does not carry storage parameters over -- not even INCLUDING ALL, not
-- even INCLUDING STORAGE. This is the trap: almost every table below is built
-- with LIKE, so putting the clause on one base table and trusting inheritance
-- would leave the rest of the suite with no parameters at all.
CREATE TABLE tpax_opt_like (LIKE tpax_opt INCLUDING ALL) USING pax;
CREATE TABLE tpax_opt_ctas AS SELECT * FROM tpax_opt;
SELECT c.relname, coalesce(c.reloptions::text, '(none)') AS reloptions
FROM pg_class c
WHERE c.relname LIKE 'tpax_opt%' AND c.relkind = 'r'
ORDER BY c.relname;

-- PAX never builds a TOAST table, whatever toast_tuple_target says: the AM's
-- relation_needs_toast_table() answers false. A value well past the 512 target
-- is therefore stored inline, in the arena, and reads back unchanged.
SELECT reltoastrelid = 0 AS no_toast_table FROM pg_class WHERE relname = 'tpax_opt';
INSERT INTO tpax_opt VALUES (1, repeat('x', 2000));
SELECT id, length(v) AS stored_inline, v = repeat('x', 2000) AS reads_back
FROM tpax_opt;

-- And fillfactor does not currently bound how full a page gets. Two tables
-- filled identically, one at 10 and one at 100, cost exactly the same, which is
-- what "recorded but not obeyed" looks like from the outside. Comparing the two
-- rather than pinning an absolute page count keeps the assertion meaningful if
-- the geometry ever changes: what is being tested is the *absence* of an
-- effect, and that stays true across a layout change.
--
-- Obeying it is not automatically a cost, either, and the measurement is worth
-- recording because the first guess was wrong. A throwaway patch capping the
-- consumed fraction of each page at fillfactor, on 2 000 rows:
--
--     fillfactor      12 col     20 col
--          100       26 pages   63 pages
--           80       26 pages   63 pages      <- no change at all
--           30       61 pages  2000 pages     <- one row per page
--
-- 80 is free because a PAX page is already more than half consumed before its
-- first row: at 20 columns the chunk set alone is 4352 o, about 55% of the
-- usable area, so an 80% cap never binds on the data that is actually there.
-- Below that threshold it collapses, because the chunk set no longer fits
-- inside the cap and every row needs a fresh page. So the window for
-- fillfactor on a wide table is roughly [chunk-set fraction, 100], and 80 sits
-- inside it. Honouring the option is therefore a policy decision, not a
-- performance cliff -- and it is not implemented, so the test says so rather
-- than letting the clause imply otherwise.
CREATE TABLE tpax_opt_ff_lo (id int, v text)
USING pax WITH (fillfactor = 10, toast_tuple_target = 512);
INSERT INTO tpax_opt_ff_lo SELECT g, 'v' || g FROM generate_series(1, 2000) g;
CREATE TABLE tpax_opt_ff_hi (LIKE tpax_opt_ff_lo INCLUDING DEFAULTS)
USING pax WITH (fillfactor = 100, toast_tuple_target = 512);
INSERT INTO tpax_opt_ff_hi SELECT * FROM tpax_opt_ff_lo;
SELECT (SELECT pg_relation_size('tpax_opt_ff_lo') / 8192) AS pages_at_fillfactor_10,
       (SELECT pg_relation_size('tpax_opt_ff_hi') / 8192) AS pages_at_fillfactor_100,
       (SELECT pg_relation_size('tpax_opt_ff_lo')
          = pg_relation_size('tpax_opt_ff_hi')) AS fillfactor_has_no_effect_yet;
DROP TABLE tpax_opt_ff_lo, tpax_opt_ff_hi, tpax_opt_ctas, tpax_opt_like,
           tpax_opt_bare, tpax_opt;

--
-- table lifecycle (this used to segfault: relation_set_new_filelocator
-- was NULL in pax_methods)
--
CREATE TABLE tpax (id int PRIMARY KEY, k2 smallint, k4 int, k8 bigint, f4 real, f8 double precision, num numeric(12,2), t1 text, t2 text, t3 varchar(32), t4 text, b1 bool, d1 date, ts1 timestamp, iv1 interval, uu uuid, ba bytea, ch "char", i_null int, t_null text) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);

-- the table must be registered on the pax access method
SELECT a.amname
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
WHERE c.relname = 'tpax';

-- a fresh table is empty
SELECT count(*) FROM tpax;

INSERT INTO tpax (id, t1, t2) VALUES (1, 'hello', 'world'), (2, NULL, NULL);

-- storage must actually have been created (relation_size callback).
-- NB: checked after INSERT — a brand-new relation has 0 blocks.
SELECT pg_relation_size('tpax') > 0 AS has_storage;

SELECT count(*) FROM tpax;

-- values must round-trip: by-value lengths 1/2/4/8, by-reference 16,
-- varlena, and NULL — every slot width the format distinguishes
SELECT id, k2, k4, k8, f4, f8, num, t1, t2, t3, t4,
       b1, d1, ts1, iv1, uu, ba, ch, i_null, t_null
FROM tpax ORDER BY id;

-- a row of defaults: every column NULL except the key
SELECT id, k2 IS NULL AS k2_null, f8 IS NULL AS f8_null,
       t1 IS NULL AS t1_null, uu IS NULL AS uu_null
FROM tpax ORDER BY id;

-- TID offset is tupno + 1, so the first tuple is ctid (0,1)
SELECT ctid, id FROM tpax ORDER BY id LIMIT 1;

--
-- many rows: forces several pages in pax_tuple_insert, opens a second chunk
-- per column (PAX_CHUNK_MAX_ROWS = 32), and grows every NULL bitmap past
-- its first byte
--
INSERT INTO tpax
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, repeat('x', 30) || g, 'v' || g,
       CASE WHEN g % 5 = 0 THEN NULL ELSE 'w' || g END,
       g % 2 = 0,
       DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2,
       CASE WHEN g % 7 = 0 THEN NULL ELSE 'n' || g END
FROM generate_series(3, 502) g;
SELECT count(*) FROM tpax;
SELECT count(*) FROM tpax WHERE t1 LIKE 'texte %';
SELECT id, k2, k4, f8, t1, t4, uu FROM tpax WHERE id = 250;
-- the NULL bitmap must round-trip at both ends of a chunk
SELECT count(*) FILTER (WHERE t4 IS NULL) AS t4_null,
       count(*) FILTER (WHERE t_null IS NULL) AS t_null_null,
       count(*) FILTER (WHERE t4 IS NULL AND t_null IS NULL) AS both_null
FROM tpax;
SELECT pg_relation_size('tpax') > 16384 AS spans_pages;

--
-- INSERT ... SELECT from another pax table: exercises the slot
-- materialize/copyslot path on top of the scan
--
CREATE TABLE tpax_copy (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_copy SELECT * FROM tpax;
SELECT count(*) FROM tpax_copy;
SELECT (SELECT count(*) FROM tpax) = (SELECT count(*) FROM tpax_copy) AS same_count;
SELECT id, t1, t2, k8 FROM tpax_copy WHERE id = 1;

-- COPY uses the required multi_insert callback; PAX implements it as the same
-- columnar append path used by tuple_insert.
CREATE TABLE tpax_copy_multi (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
COPY tpax_copy_multi (id, k4, f8, t1, t2) FROM STDIN;
1	10	1.5	one	two
2	20	2.5	three	four
\.
SELECT id, k4, f8, t1, t2 FROM tpax_copy_multi ORDER BY id;
-- the columns left out of the COPY list must be NULL, not stale
SELECT count(*) FILTER (WHERE t3 IS NULL) AS t3_null,
       count(*) FILTER (WHERE uu IS NULL) AS uu_null
FROM tpax_copy_multi;
DROP TABLE tpax_copy_multi;

--
-- one row covering every storage shape, on the same 20-column layout used
-- everywhere else: by-value 1/2/4/8, by-reference fixed 16 (uuid,
-- interval), varlena (text, varchar, bytea, numeric), plus a second row
-- that is NULL in every optional column (null bitmap).
--
-- pg_regress exports PGDATESTYLE=Postgres, MDY: pin the rendering here so
-- the expected output does not depend on that environment.
--
CREATE TABLE tpax_types (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);

SET datestyle = 'ISO, MDY';
SET intervalstyle = 'postgres';

INSERT INTO tpax_types VALUES (
    42, -12345, -123456789, 1234567890123, 1.5, -0.25, -1234.56,
    'hello pax', 'a somewhat longer text', 'varchar value', 'w',
    true, '2024-01-15', '2024-01-15 13:45:30', '00:01:30',
    '123e4567-e89b-12d3-a456-426614174000', '\x0102a0', 'x', 99, 'not null');

SELECT * FROM tpax_types;

-- negative values must round-trip for every signed type
INSERT INTO tpax_types (id, k2, k4, k8, f4, f8, num) VALUES (7, -1, -2, -3, -0.5, -1e10, -0.01);
SELECT id, k2, k4, k8, f4, f8, num FROM tpax_types ORDER BY id;

-- a row NULL in every optional column, key aside
INSERT INTO tpax_types (id) VALUES (8);
SELECT id, k2 IS NULL AS k2_null, f8 IS NULL AS f8_null, t1 IS NULL AS t1_null,
       uu IS NULL AS uu_null, iv1 IS NULL AS iv1_null, ba IS NULL AS ba_null
FROM tpax_types WHERE id = 8;
SELECT count(*) FROM tpax_types;

--
-- basic MVCC: rows created by earlier commands in the current transaction are
-- visible to that transaction, while rows from an aborted transaction are
-- hidden from later snapshots.  System transaction attributes are stored too.
--
CREATE TABLE tpax_mvcc (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_mvcc (id, k4, f8, t1) VALUES (0, 10, 0.5, 'mvcc');

-- exact-TID and TID-range scans use the versioned table-AM callbacks
SELECT ctid, id FROM tpax_mvcc WHERE ctid = '(0,1)';
SELECT array_agg(id ORDER BY id) AS tid_range_ids
FROM tpax_mvcc
WHERE ctid BETWEEN '(0,1)' AND '(0,1)';

BEGIN;
INSERT INTO tpax_mvcc (id, k4, f8, t1) VALUES (1, 11, 1.5, 'uncommitted');
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
SELECT id,
       xmin = pg_current_xact_id()::text::xid AS own_xmin,
       xmax = '0'::xid AS no_xmax,
       cmin IS NOT NULL AS has_cmin,
       cmax IS NOT NULL AS has_cmax,
       cmin = cmax AS shared_command
FROM tpax_mvcc
WHERE id = 1;
INSERT INTO tpax_mvcc (id, k4, f8, t1) VALUES (2, 12, 2.5, 'subxact');
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
ROLLBACK;

-- the aborted transaction's physical rows remain invisible
SELECT array_agg(id ORDER BY id) FROM tpax_mvcc;
SELECT count(*) FROM tpax_mvcc WHERE ctid = '(0,2)';

INSERT INTO tpax_mvcc (id, k4, f8, t1) VALUES (3, 13, 3.5, 'three');
INSERT INTO tpax_mvcc (id, k4, f8, t1) VALUES (4, 14, 4.5, 'four')
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
CREATE TABLE tpax_dml (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
SELECT pg_current_wal_insert_lsn() AS before_insert_wal \gset
INSERT INTO tpax_dml
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'old', 'keep', 'v1', NULL,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2, NULL
FROM generate_series(1, 2) g;
SELECT pg_current_wal_insert_lsn() > :'before_insert_wal'::pg_lsn
       AS insert_wrote_wal;
SELECT ctid AS old_tid FROM tpax_dml WHERE id = 1 \gset
SELECT pg_current_wal_insert_lsn() AS before_update_wal \gset

UPDATE tpax_dml
SET t2 = 'new', f8 = -1.25
WHERE id = 1
RETURNING id, t2, f8, ctid <> :'old_tid' AS relocated,
          xmin <> '0'::xid AS has_xmin,
          xmax = '0'::xid AS no_xmax,
          cmin = cmax AS shared_command;
SELECT pg_current_wal_insert_lsn() > :'before_update_wal'::pg_lsn
       AS update_wrote_wal;
SELECT array_agg(id ORDER BY id) AS ids,
       array_agg(t2 ORDER BY id) AS payloads,
       array_agg(f8::text ORDER BY id) AS floats
FROM tpax_dml;

BEGIN;
UPDATE tpax_dml SET t2 = 'rolled back', f8 = 99 WHERE id = 1;
SELECT t2, f8 FROM tpax_dml WHERE id = 1;
ROLLBACK;
SELECT t2, f8 FROM tpax_dml WHERE id = 1;

-- a second update follows the version chain; rolling back the subtransaction
-- restores the version written by the first update
BEGIN;
UPDATE tpax_dml SET t2 = 'first', f8 = 1 WHERE id = 1;
SAVEPOINT pax_update_sp;
UPDATE tpax_dml SET t2 = 'second', f8 = 2 WHERE id = 1;
SELECT t2, f8 FROM tpax_dml WHERE id = 1;
ROLLBACK TO pax_update_sp;
SELECT t2, f8 FROM tpax_dml WHERE id = 1;
COMMIT;
SELECT t2, f8 FROM tpax_dml WHERE id = 1;

-- A current logical row lock is carried to the replacement version and is
-- reflected by the UPDATE RETURNING slot.  Heavyweight tuple locks are only
-- short-lived waiter arbitration and are not retained.
BEGIN;
SELECT pg_current_wal_insert_lsn() AS before_lock_wal \gset
SELECT id FROM tpax_dml WHERE id = 2 FOR UPDATE;
SELECT pg_current_wal_insert_lsn() > :'before_lock_wal'::pg_lsn
       AS lock_wrote_wal;
UPDATE tpax_dml SET t2 = 'carried lock', f8 = 3
WHERE id = 2
RETURNING t2, f8, xmax <> '0'::xid AS lock_visible;
SELECT count(*) = 0 AS no_heavyweight_tuple_lock
FROM pg_locks WHERE locktype = 'tuple';
ROLLBACK;
SELECT t2, f8 FROM tpax_dml WHERE id = 2;

SELECT pg_current_wal_insert_lsn() AS before_delete_wal \gset
DELETE FROM tpax_dml
WHERE id = 1
RETURNING id, t2,
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
--
-- At 20 columns the ceiling is much lower than a bare page: a new page must
-- also pay the whole chunk set (4352 o here), so anything past ~3.7 KB of
-- payload is refused. The refusal used to come out as a low-level FSM error
-- ("invalid FSM request size") instead, because the space hint asked the FSM
-- for more than the FSM can represent; it must now be the row-size error, and
-- the old version must still be there afterwards.
BEGIN;
SAVEPOINT pax_large_update_sp;
UPDATE tpax_dml SET t2 = repeat('x', 10000) WHERE id = 2;
ROLLBACK TO pax_large_update_sp;
SELECT t2, f8 FROM tpax_dml WHERE id = 2;
COMMIT;

-- Multiple join matches in one command produce TM_SelfModified after the
-- first write; PostgreSQL must ignore the duplicate without a second version.
UPDATE tpax_dml AS a
SET t2 = a.t2 || ' once', f8 = a.f8 + 1
FROM tpax_dml AS b
WHERE a.id = b.id AND a.id = 2 AND b.id IN (2, 2);
SELECT t2, f8 FROM tpax_dml WHERE id = 2;
DELETE FROM tpax_dml AS a
USING tpax_dml AS b
WHERE a.id = b.id AND a.id = 2 AND b.id IN (2, 2);
SELECT count(*) AS rows_after_duplicate_delete FROM tpax_dml;
DROP TABLE tpax_dml;

-- Force a replacement version onto another page and verify that the 32-bit
-- block plus 16-bit offset t_ctid link is followed by later version locking.
--
-- The 3700-byte payload is chosen against the geometry, not by habit: it must
-- exceed the free space left on the row's own page yet still fit on a fresh
-- one. On this schema the chunk set is 4352 o, so a fresh page has ~3.7 KB for
-- payloads and the boundary falls just there. 1000 o would stay in place, and
-- 5000 o no longer fits at all -- see the tpax_dml case above, which asserts
-- that refusal.
CREATE TABLE tpax_chain (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_chain
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       repeat('x', 50), repeat('y', 50), 'v' || g,
       CASE WHEN g % 5 = 0 THEN NULL ELSE 'w' || g END,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2,
       CASE WHEN g % 7 = 0 THEN NULL ELSE 'n' || g END
FROM generate_series(0, 99) g;
SELECT ctid AS old_tid FROM tpax_chain WHERE id = 0 \gset
UPDATE tpax_chain
SET t2 = repeat('y', 3700)
WHERE id = 0
RETURNING length(t2) AS new_length,
          split_part(trim(both '()' from ctid::text), ',', 1)::integer <>
          split_part(trim(both '()' from :'old_tid'), ',', 1)::integer
          AS changed_block;
SELECT count(*) AS chain_rows,
       max(length(t2)) FILTER (WHERE id = 0) AS updated_length
FROM tpax_chain;
DROP TABLE tpax_chain;

-- ANALYZE (scan_analyze_next_block / scan_analyze_next_tuple)
CREATE TABLE tpax_analyze (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_analyze
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, repeat('x', 30) || g, 'v' || g, NULL,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2, NULL
FROM generate_series(1, 3000) g;
-- Three tenths of the rows die, so VACUUM/ANALYZE must report them as dead.
DELETE FROM tpax_analyze WHERE id % 10 < 3;
ANALYZE tpax_analyze;
SELECT count(*) AS analyzed_rows
FROM tpax_analyze;
SELECT attname, null_frac
FROM pg_stats
WHERE tablename = 'tpax_analyze' AND attname IN ('id', 'f8', 't1')
ORDER BY attname;
DROP TABLE tpax_analyze;

-- VACUUM (relation_vacuum callback)
--
-- Dead versions are marked in place rather than compacted: t_ctid links are
-- physical (page, version index), and removing a version would shift every
-- following slot in every column and break inbound links from other pages.
-- A stale link must therefore resolve to an invisible version, not to a
-- shifted one.
CREATE TABLE tpax_vacuum (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_vacuum
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, repeat('x', 30) || g, 'v' || g,
       CASE WHEN g % 5 = 0 THEN NULL ELSE 'w' || g END,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2,
       CASE WHEN g % 7 = 0 THEN NULL ELSE 'n' || g END
FROM generate_series(1, 2000) g;

-- Build a chain three deep, then delete the head: after VACUUM the two
-- predecessors are reclaimed while the live version must stay reachable.
UPDATE tpax_vacuum SET t2 = t2 || '-u1' WHERE id <= 500;
UPDATE tpax_vacuum SET t2 = t2 || '-u2' WHERE id <= 200;
DELETE FROM tpax_vacuum WHERE id <= 50;

VACUUM tpax_vacuum;

-- The surviving chain must still read back exactly as before. Checking
-- one text column is not enough at this width: the replacement version also
-- rewrites every other column, so a chunk or a payload that had moved would
-- show up in a float or in a NULL bit.
--
-- t2 was seeded to repeat('x',30) || g, so its expected value is that same
-- seed plus the suffix each UPDATE appended.
SELECT count(*) AS vacuum_rows,
       count(*) FILTER (WHERE t2 <> expected) AS mismatched,
       count(*) FILTER (WHERE f8 <> g * 1.25) AS float_mismatched,
       count(*) FILTER (WHERE k8 <> g * 100000) AS bigint_mismatched,
       count(*) FILTER (WHERE t1 <> 'texte ' || g) AS t1_mismatched,
       count(*) FILTER (WHERE t4_null <> (g % 5 = 0)) AS t4_null_mismatched,
       count(*) FILTER (WHERE tnull_null <> (g % 7 = 0)) AS tnull_mismatched
FROM (
    SELECT id AS g, f8, k8, t1, t2,
           t4 IS NULL AS t4_null, t_null IS NULL AS tnull_null,
           repeat('x', 30) || id ||
               CASE WHEN id <= 200 THEN '-u1-u2'
                    WHEN id <= 500 THEN '-u1'
                    ELSE '' END AS expected
    FROM tpax_vacuum
) s;

-- A row lock and an update must still follow the chain after vacuum.
BEGIN;
SELECT id FROM tpax_vacuum WHERE id = 300 FOR UPDATE;
COMMIT;
UPDATE tpax_vacuum SET t2 = t2 || '-u3' WHERE id = 300;
SELECT t2 FROM tpax_vacuum WHERE id = 300;

-- In-place space reclamation: VACUUM marks the dead versions UNUSED and
-- compacts their variable-length payloads, so a later INSERT reuses the freed
-- slots instead of extending the relation.
--
-- Row counts and page counts are asserted only through comparisons, never as
-- absolute numbers: how many rows fit on a page depends on the packed layout,
-- which is an implementation detail rather than a contract.
CREATE TABLE tpax_reuse (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_reuse
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, 'v' || g, 'v' || g, NULL,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2, NULL
FROM generate_series(1, 6000) g;

CREATE TABLE tpax_reuse_size AS
SELECT pg_relation_size('tpax_reuse') / 8192 AS pages FROM tpax_reuse;

-- Three quarters of the rows are replaced, so their old versions become dead
-- and their payloads are compacted away by VACUUM.
UPDATE tpax_reuse SET t2 = t2 || '-u' WHERE id <= 4500;
VACUUM tpax_reuse;

-- Re-inserting the same number of rows must fit in the space already owned:
-- the relation may grow a little to hold the new versions, but must not double.
INSERT INTO tpax_reuse
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, 'r' || g, 'v' || g, NULL,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2, NULL
FROM generate_series(1, 4500) g;

-- Every column is checked, not only the one that was rewritten: a reused slot
-- that kept the payload of its predecessor, or shifted one column by one row,
-- shows up as a mismatch in a float or in a NULL bit.
SELECT (SELECT count(*) FROM tpax_reuse) AS rows_after_reuse,
       (SELECT count(*) FROM tpax_reuse
         WHERE t2 NOT IN ('r' || id, 'v' || id, 'v' || id || '-u')) AS mismatched,
       (SELECT count(*) FROM tpax_reuse
         WHERE f8 <> id * 1.25 OR k8 <> id * 100000) AS numeric_mismatched,
       (SELECT count(*) FROM tpax_reuse
         WHERE t1 <> 'texte ' || id) AS text_mismatched,
       (SELECT count(*) FROM tpax_reuse
         WHERE (t4 IS NOT NULL) OR (t_null IS NOT NULL)) AS null_mismatched,
       (SELECT pg_relation_size('tpax_reuse') / 8192
          FROM tpax_reuse_size LIMIT 1) AS pages_before,
       (SELECT pg_relation_size('tpax_reuse') / 8192
          FROM tpax_reuse LIMIT 1) AS pages_after,
       (SELECT pg_relation_size('tpax_reuse') / 8192 FROM tpax_reuse LIMIT 1)
         < 2 * (SELECT pg_relation_size('tpax_reuse') / 8192
                FROM tpax_reuse_size LIMIT 1)
         AS space_reclaimed;

-- Reusing a slot must not resurrect the metadata of the version it replaced:
-- the row count is unchanged by the reuse itself.
SELECT count(*) AS versions_per_page_are_stable
FROM tpax_reuse;

DROP TABLE tpax_reuse_size;
DROP TABLE tpax_reuse;

-- Indexes. Every version carries its own index entry, so an index scan must
-- resolve a TID to that exact version and must NOT follow t_ctid: an entry left
-- behind by an UPDATE would otherwise resolve to the replacement version and
-- make the row appear twice, while remaining invisible to a seq scan.
CREATE TABLE tpax_index (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_index
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, 'v' || g, 'v' || g,
       CASE WHEN g % 5 = 0 THEN NULL ELSE 'w' || g END,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2,
       CASE WHEN g % 7 = 0 THEN NULL ELSE 'n' || g END
FROM generate_series(1, 2000) g;
CREATE INDEX tpax_index_id_idx ON tpax_index (id);

SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id, t2 FROM tpax_index WHERE id = 500;
-- the whole 20-column row must come back through the index path
SELECT id, k8, f8, num, t1, t2, t4, uu, ba, i_null, t_null
FROM tpax_index WHERE id = 500;
RESET enable_seqscan;

-- An UPDATE writes a new version elsewhere and inserts a new index entry for
-- it. The entry for the old TID stays until the index is vacuumed, and must
-- yield the old version (invisible), not the new one.
UPDATE tpax_index SET t2 = t2 || '-u' WHERE id = 20;
SET enable_seqscan = off;
SELECT count(*) AS updated_row_via_index FROM tpax_index WHERE id = 20;
RESET enable_seqscan;
SELECT count(*) AS updated_row_via_seqscan FROM tpax_index WHERE id = 20;

-- Updating an indexed column moves the row to a new key entirely.
UPDATE tpax_index SET id = id + 100000 WHERE id = 21;
SET enable_seqscan = off;
SELECT count(*) AS old_key FROM tpax_index WHERE id = 21;
SELECT count(*) AS new_key FROM tpax_index WHERE id = 100021;
RESET enable_seqscan;

-- DELETE: the entry must resolve to nothing.
DELETE FROM tpax_index WHERE id = 22;
SET enable_seqscan = off;
SELECT count(*) AS deleted_row_via_index FROM tpax_index WHERE id = 22;
RESET enable_seqscan;

-- A multi-row UPDATE on an indexed table. Every row needs its new version
-- written, its old index entry replaced, and the old version marked dead,
-- while the scan is still walking the pages. nbtree then runs its own
-- deletion checks over the pages it touched, which calls back into the table
-- AM. This is where an unbalanced buffer content lock used to wedge the
-- backend: the scan's next LockBuffer() waited on a lock it still held.
UPDATE tpax_index SET t2 = t2 || '-m' WHERE id <= 300;

-- The multi-row UPDATE must be complete and consistent: the new payload is
-- visible once, the old one not at all, and both access paths agree.
SELECT count(*) AS multi_updated_via_seqscan
FROM tpax_index WHERE t2 LIKE '%-m';
SET enable_seqscan = off;
SELECT count(*) AS multi_updated_via_index FROM tpax_index WHERE t2 LIKE '%-m';
RESET enable_seqscan;
SELECT count(*) AS stale_payload_left FROM tpax_index WHERE t2 LIKE '%-u';

-- A multi-row UPDATE rewrites all 20 columns of each row, so the untouched
-- columns are what prove the replacement version was assembled column by
-- column rather than copied wholesale.
SELECT count(*) AS multi_updated_columns_wrong
FROM tpax_index
WHERE id <= 300
  AND (k8 <> id * 100000 OR f8 <> id * 1.25 OR t1 <> 'texte ' || id
       OR (t4 IS NULL) <> (id % 5 = 0)
       OR (t_null IS NULL) <> (id % 7 = 0));

-- A multi-row DELETE exercises the same path from the other side.
DELETE FROM tpax_index WHERE id <= 100;
SET enable_seqscan = off;
SELECT count(*) AS multi_deleted_via_index FROM tpax_index WHERE id <= 100;
RESET enable_seqscan;
SELECT count(*) AS rows_left FROM tpax_index;

-- The index scan and the seq scan must agree on every row: any stale entry
-- resolving to a live version would show up as a difference here.
CREATE TEMP TABLE tpax_index_seq AS SELECT * FROM tpax_index;
SET enable_seqscan = off;
CREATE TEMP TABLE tpax_index_idx AS SELECT * FROM tpax_index;
RESET enable_seqscan;
SELECT (SELECT count(*) FROM tpax_index_seq) AS rows_via_seqscan,
       (SELECT count(*) FROM tpax_index_idx) AS rows_via_indexscan,
       (SELECT count(*) FROM (
          (SELECT * FROM tpax_index_idx
             EXCEPT ALL SELECT * FROM tpax_index_seq)
          UNION ALL
          (SELECT * FROM tpax_index_seq
             EXCEPT ALL SELECT * FROM tpax_index_idx)) d) AS differences;

-- A unique index must be enforced, on INSERT and on UPDATE. The visibility
-- snapshot used for that check is SnapshotDirty, which also reports the xid of
-- a concurrent inserter; it must be filled in on every call, since nbtree reads
-- those output fields.
CREATE TABLE tpax_unique (LIKE tpax INCLUDING ALL) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_unique
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, 'v' || g, 'v' || g, NULL,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2, NULL
FROM generate_series(1, 500) g;
INSERT INTO tpax_unique (id, t2) VALUES (1, 'duplicate');
INSERT INTO tpax_unique (id, t2) VALUES (9999, 'free');
UPDATE tpax_unique SET id = 2 WHERE id = 3;
UPDATE tpax_unique SET id = 8888 WHERE id = 4;
SELECT id FROM tpax_unique WHERE id IN (2, 3, 4, 8888) ORDER BY id;

-- Index cleanup: VACUUM marks the dead versions, and the index scan then asks
-- whether each of its entries may go. Without that second half the dead
-- entries would stay forever. Page counts are not asserted: PostgreSQL returns
-- freed index space only lazily, so their exact value is not a contract.
DELETE FROM tpax_index WHERE id % 3 = 0;
VACUUM tpax_index;
SET enable_seqscan = off;
SELECT count(*) AS rows_after_index_vacuum FROM tpax_index;
RESET enable_seqscan;
SELECT count(*) AS rows_after_index_vacuum_seqscan FROM tpax_index;

DROP TABLE tpax_index;
DROP TABLE tpax_unique;

-- Fully empty trailing pages are truncated away.
CREATE TABLE tpax_truncate (LIKE tpax INCLUDING DEFAULTS) USING pax
WITH (fillfactor = 80, toast_tuple_target = 512);
INSERT INTO tpax_truncate
SELECT g, g % 100, g, g * 100000, g / 4.0, g * 1.25, g * 1.5,
       'texte ' || g, 'v' || g, 'v' || g, NULL,
       g % 2 = 0, DATE '2024-01-01' + (g % 900),
       TIMESTAMP '2024-01-01 00:00:00' + (g || ' sec')::interval,
       (g || ' min')::interval,
       ('123e4567-e89b-12d3-a456-42661417400' || (g % 10))::uuid,
       '\x0102'::bytea, 'x', g * 2, NULL
FROM generate_series(1, 2000) g;
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

-- Sweep: whatever PAX tables are still alive at this point must all carry the
-- two storage parameters. This is the assertion that would catch a CREATE TABLE
-- added to this file without the WITH clause -- the mistake that a per-table
-- spot check would miss, and one that LIKE-based inheritance would hide.
SELECT count(*) AS pax_tables_missing_the_expected_options
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
WHERE a.amname = 'pax' AND c.relkind = 'r'
  AND (c.reloptions IS DISTINCT FROM
       ARRAY['fillfactor=80', 'toast_tuple_target=512']::text[]);

-- ... and there is at least one, so the sweep above is not vacuously true.
SELECT count(*) > 0 AS sweep_is_not_vacuous
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
WHERE a.amname = 'pax' AND c.relkind = 'r';

DROP TABLE tpax;
DROP TABLE tpax_copy;
DROP TABLE tpax_types;

-- Dropping the extension must not leave a PAX table behind, whatever its
-- parameters: relation_drop removes the storage, not reloptions.
SELECT count(*) AS pax_tables_left_after_drops
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
WHERE a.amname = 'pax' AND c.relkind = 'r';

DROP EXTENSION pax_am;

-- a second CREATE/DROP cycle must work (extension is relocatable)
CREATE EXTENSION pax_am;
DROP EXTENSION pax_am;

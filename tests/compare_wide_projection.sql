-- Shared buffers and scan time for a narrow projection on a wide table,
-- heap versus PAX.
--
-- This is a measurement, not a golden regression: block counts and timings
-- depend on cache state. The runner creates an isolated database and drops it
-- afterwards. Nothing here asserts that PAX must be smaller or faster.

\echo '=== Wide projection benchmark (12 columns, index on the projected key) ==='

CREATE EXTENSION pax_am;

-- Twelve columns: one integer key plus eleven 60-byte payloads. The key is
-- indexed so the table matches the "indexed wide table" case; the projection
-- queries are forced to sequential scans so both AMs are measured the same way.
CREATE TABLE heap_wide (
    id  integer,
    c1  text, c2  text, c3  text, c4  text, c5  text, c6  text,
    c7  text, c8  text, c9  text, c10 text, c11 text
) USING heap;

CREATE TABLE pax_wide (
    id  integer,
    c1  text, c2  text, c3  text, c4  text, c5  text, c6  text,
    c7  text, c8  text, c9  text, c10 text, c11 text
) USING pax;

\echo 'Loading :rows rows into both access methods...'

INSERT INTO heap_wide
SELECT g,
       repeat('a', 60), repeat('b', 60), repeat('c', 60), repeat('d', 60),
       repeat('e', 60), repeat('f', 60), repeat('g', 60), repeat('h', 60),
       repeat('i', 60), repeat('j', 60), repeat('k', 60)
FROM generate_series(1, :rows) AS g;

INSERT INTO pax_wide
SELECT * FROM heap_wide;

-- CREATE INDEX on a PAX table uses a parallel maintenance worker by default,
-- which the AM does not support; force it serial.
SET max_parallel_maintenance_workers = 0;
CREATE INDEX heap_wide_id_idx ON heap_wide (id);
CREATE INDEX pax_wide_id_idx  ON pax_wide  (id);
RESET max_parallel_maintenance_workers;

-- Force sequential scans: the point is page traffic for a projection, not the
-- index access path.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_tidscan = off;
SET max_parallel_workers_per_gather = 0;



-- Warm both relations with the measurement queries themselves, so the reported
-- run is a warm-cache run.
SELECT count(*), sum(hashtext(c1)::bigint) FROM heap_wide;
SELECT count(*), sum(hashtext(c1)::bigint) FROM pax_wide;

\echo ''
\echo 'Relation size on disk (a PAX page also carries 32 bytes of metadata per'
\echo 'version, so equal data does not mean equal page count):'

SELECT 'heap' AS access_method,
       pg_relation_size('heap_wide') / 8192 AS table_pages,
       pg_relation_size('heap_wide_id_idx') / 8192 AS index_pages
UNION ALL
SELECT 'pax',
       pg_relation_size('pax_wide') / 8192,
       pg_relation_size('pax_wide_id_idx') / 8192;

-- Runs the actual aggregate for a given projection width, so the equivalence
-- check below measures the same thing the buffer measurement reports.
CREATE FUNCTION wide_check(ncols integer, relation text)
RETURNS TABLE (count bigint, checksum numeric)
LANGUAGE plpgsql
AS $$
DECLARE
    all_cols   text;
    query_text text;
    n           bigint;
    c           numeric;
BEGIN
    all_cols := (SELECT string_agg('hashtext(' || x || ')::bigint', ' + '
                                   ORDER BY x)
                 FROM unnest(ARRAY['c1','c2','c3','c4','c5','c6','c7','c8',
                                   'c9','c10','c11']) AS x);

    query_text := CASE ncols
        WHEN 1 THEN format(
            'SELECT count(*), sum(hashtext(c1)::bigint)::numeric FROM %s',
            relation)
        WHEN 2 THEN format(
            'SELECT count(*), sum(hashtext(c1)::bigint + hashtext(c2)::bigint)'
            '::numeric FROM %s', relation)
        ELSE format('SELECT count(*), sum(%s)::numeric FROM %s',
                    all_cols, relation)
    END;

    EXECUTE query_text INTO n, c;

    RETURN QUERY SELECT n, c;
END;
$$;

-- The helper takes the projection width and the relation as separate arguments
-- so the caller cannot pass a query that references the wrong number of
-- columns, and so the measured access method stays known.
CREATE FUNCTION wide_usage(ncols integer, relation text)
RETURNS TABLE (
    columns_projected integer,
    access_method     text,
    shared_hit_blocks bigint,
    shared_read_blocks bigint,
    exec_ms           numeric
)
LANGUAGE plpgsql
AS $$
DECLARE
    plan_text text;
    all_cols  text;
    query_text text;
BEGIN
    all_cols := (SELECT string_agg('hashtext(' || x || ')::bigint', ' + '
                                   ORDER BY x)
                 FROM unnest(ARRAY['c1','c2','c3','c4','c5','c6','c7','c8',
                                   'c9','c10','c11']) AS x);

    query_text := CASE ncols
        WHEN 1 THEN format(
            'SELECT count(*), sum(hashtext(c1)::bigint) FROM %s', relation)
        WHEN 2 THEN format(
            'SELECT count(*), sum(hashtext(c1)::bigint + hashtext(c2)::bigint)'
            ' FROM %s', relation)
        ELSE format('SELECT count(*), sum(%s) FROM %s', all_cols, relation)
    END;

    -- Verify the projection really is what we claim before reporting it.
    IF (SELECT count(DISTINCT x)
        FROM unnest(ARRAY['c1','c2','c3','c4','c5','c6','c7','c8',
                          'c9','c10','c11']) AS x
        WHERE query_text LIKE '%' || x || '%') <> ncols THEN
        RAISE EXCEPTION 'query for % columns does not reference % columns',
            ncols, ncols;
    END IF;

    FOR plan_text IN
        EXECUTE format(
            'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) %s',
            query_text)
    LOOP
        NULL;   -- keep the loop; only the last row matters
    END LOOP;

    RETURN QUERY SELECT
        ncols,
        relation,
        coalesce(substring(plan_text FROM '"Shared Hit Blocks": ([0-9]+)')::bigint, 0),
        coalesce(substring(plan_text FROM '"Shared Read Blocks": ([0-9]+)')::bigint, 0),
        coalesce(substring(plan_text FROM '"Execution Time": ([0-9.]+)')::numeric, 0);
END;
$$;

CREATE TEMP TABLE wide_measurement AS
SELECT u.*
FROM (VALUES (1), (2), (11)) AS n(ncols)
CROSS JOIN (VALUES ('heap_wide'), ('pax_wide')) AS am(t)
CROSS JOIN LATERAL wide_usage(n.ncols, am.t) AS u;

\echo ''
\echo 'Warm-cache shared buffers and scan time, by number of projected columns:'
SELECT
    columns_projected,
    max(shared_hit_blocks + shared_read_blocks) FILTER (WHERE access_method = 'heap_wide')
        AS heap_blocks,
    max(shared_hit_blocks + shared_read_blocks) FILTER (WHERE access_method = 'pax_wide')
        AS pax_blocks,
    round(max(exec_ms) FILTER (WHERE access_method = 'heap_wide'), 2) AS heap_ms,
    round(max(exec_ms) FILTER (WHERE access_method = 'pax_wide'), 2) AS pax_ms,
    round(
        max(shared_hit_blocks + shared_read_blocks) FILTER (WHERE access_method = 'pax_wide')::numeric
        / NULLIF(max(shared_hit_blocks + shared_read_blocks) FILTER (WHERE access_method = 'heap_wide'), 0),
        3) AS blocks_pax_over_heap,
    round(
        max(exec_ms) FILTER (WHERE access_method = 'pax_wide')
        / NULLIF(max(exec_ms) FILTER (WHERE access_method = 'heap_wide'), 0),
        3) AS time_pax_over_heap
FROM wide_measurement
GROUP BY columns_projected
ORDER BY columns_projected;

\echo ''
\echo 'Result equivalence: heap and PAX must agree on every projection.'

-- Compare the three projections by building the same aggregate on both sides.
-- A single-row comparison keeps the row count and the checksum together.
CREATE TEMP TABLE projection_equivalence AS
SELECT n.ncols,
       (SELECT ROW(h.count, h.checksum)
        FROM wide_check(n.ncols, 'heap_wide') AS h)
       IS NOT DISTINCT FROM
       (SELECT ROW(p.count, p.checksum)
        FROM wide_check(n.ncols, 'pax_wide') AS p) AS identical
FROM (VALUES (1), (2), (11)) AS n(ncols);

SELECT ncols, identical FROM projection_equivalence ORDER BY ncols;

DO $$
DECLARE
    bad integer;
BEGIN
    SELECT count(*) INTO bad
    FROM projection_equivalence WHERE NOT identical;

    IF bad > 0 THEN
        RAISE EXCEPTION
            'heap and PAX disagree on % projection(s)', bad;
    END IF;
END;
$$;

\echo ''
\echo '--- Where the extra PAX pages come from ---------------------------'
\echo 'Every figure below is measured from the catalog at run time and'
\echo 'interpolated, so it cannot drift away from the format the way a'
\echo 'hand-written figure does. The one thing stated rather than measured is'
\echo 'the payload, because it is arithmetic on the schema: eleven 60-char'
\echo 'text columns, each a 61-byte varlena, each MAXALIGNed to 64 bytes.'

SELECT 11 * 64 AS payload_b_per_row \gset
SELECT (SELECT count(*) FROM heap_wide) AS n \gset
SELECT pg_relation_size('heap_wide') / 8192 AS heap_pages \gset
SELECT pg_relation_size('pax_wide')  / 8192 AS pax_pages  \gset
SELECT round(pg_relation_size('heap_wide')::numeric / :n, 1) AS heap_bpr \gset
SELECT round(pg_relation_size('pax_wide')::numeric  / :n, 1) AS pax_bpr  \gset
SELECT round(:pax_bpr - :heap_bpr, 1) AS bpr_gap \gset
SELECT round(:n::numeric / :heap_pages, 2) AS heap_rows_per_page \gset
SELECT round(:n::numeric / :pax_pages,  2) AS pax_rows_per_page  \gset
SELECT round(:pax_pages::numeric / :heap_pages, 3) AS page_ratio \gset

\echo ''
\echo '  payload value bytes, same for both :' :payload_b_per_row 'B/row'
\echo '  heap, everything but payload    :' :heap_bpr 'B/row'
\echo '  pax,  everything but payload    :' :pax_bpr 'B/row'
\echo ''
\echo '  heap :' :heap_pages 'pages,' :heap_rows_per_page 'rows/page'
\echo '  pax  :' :pax_pages  'pages,' :pax_rows_per_page  'rows/page'
\echo ''
\echo 'PAX spends' :bpr_gap 'B/row more on bookkeeping than heap, and that is the'
\echo 'whole difference -- the payload is byte-identical. For PAX that'
\echo 'bookkeeping is the 32-byte PaxTupleMetaData per version plus, per column,'
\echo 'a NULL bitmap and one 2-byte offset. For heap it is the HeapTupleHeader'
\echo 'plus a 4-byte ItemIdData line pointer.'
\echo ''
\echo 'That gap decides the row count, because a page is only 8192 bytes:'
\echo 'measured, heap fits' :heap_rows_per_page 'rows per page and PAX fits'
\echo :pax_rows_per_page '-- the' :page_ratio 'x page ratio that the shared-buffer figures'
\echo 'above report for the identical query.'
\echo ''
\echo 'One trap worth naming: do NOT re-derive that boundary by multiplying the'
\echo 'bytes-per-row figures above by the row count. Those figures are relation'
\echo 'sizes divided by rows, so each one already contains its share of the'
\echo 'per-page PageHeaderData; multiplying by 11 counts the page header eleven'
\echo 'times and can produce a figure above 8192 for a table that demonstrably'
\echo 'fits 11 rows. The measured rows-per-page is the fact; the average is only'
\echo 'a way to attribute the difference to bookkeeping.'
\echo ''
\echo '--- How to read the block counts -----------------------------------'
\echo 'Shared buffers count PAGES, not bytes, and PostgreSQL reads a whole 8 kB'
\echo 'page into the buffer cache whether the query needs 1 column from it or'
\echo '11. A columnar layout INSIDE the page therefore cannot reduce the block'
\echo 'count of a sequential scan: every page holding the requested column still'
\echo 'has to be read. PAX keeps all 12 columns of a row group in one page, so'
\echo 'there is no page it can skip.'
\echo ''
\echo 'This is the honest conclusion: shared-buffer footprint is the one metric'
\echo 'where a page-resident columnar layout structurally cannot win. The'
\echo 'narrow-projection advantage needs column segments in SEPARATE relations,'
\echo 'or a buffer manager that can fetch part of a page. Neither exists today.'
\echo ''
\echo 'Where PAX does measure well -- compressed size, and full-width scan time'
\echo '-- is reported by tests/benchmark_columnar.sh, which measures it on this'
\echo 'pass. No figure is quoted here on its authority: this script never'
\echo 'measures compressed size, so a range written into this note would be a'
\echo 'number carried over from someone else''s session -- which is exactly'
\echo 'how the 0.82x-1.04x that used to sit here went stale.'
\echo ''
\echo 'Benchmark completed.'

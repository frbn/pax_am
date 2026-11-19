-- Compare buffer usage for an identical query on heap and PAX.
--
-- This is a measurement test, not a golden regression: buffer hit/read counts
-- depend on cache state.  The shell runner creates an isolated database and
-- removes it afterwards.

\echo 'Creating identical heap and PAX tables...'

CREATE EXTENSION pax_am;

-- pg_buffercache is optional: it adds resident-page counts on top of the
-- EXPLAIN BUFFERS numbers, which are always available.
DO $$
BEGIN
    CREATE EXTENSION pg_buffercache;
EXCEPTION
    WHEN undefined_file THEN
        RAISE NOTICE 'pg_buffercache unavailable; skipping resident-page counts';
END;
$$;

CREATE TABLE heap_buffer_probe (
    id      integer,
    payload text
) USING heap;

CREATE TABLE pax_buffer_probe (
    id      integer,
    payload text
) USING pax;

INSERT INTO heap_buffer_probe
SELECT g, repeat(md5(g::text), 10)
FROM generate_series(1, :rows) AS g;

INSERT INTO pax_buffer_probe
SELECT g, repeat(md5(g::text), 10)
FROM generate_series(1, :rows) AS g;

-- Keep both access methods on the same sequential-scan plan.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_tidscan = off;
SET max_parallel_workers_per_gather = 0;

-- Warm both relations before measuring.  Both warm-up statements are the
-- measurement query, so only the second execution is reported.
SELECT count(*), sum(length(payload)), sum(hashtext(payload))
FROM heap_buffer_probe
WHERE id % 211 = 0;

SELECT count(*), sum(length(payload)), sum(hashtext(payload))
FROM pax_buffer_probe
WHERE id % 211 = 0;

CREATE FUNCTION pax_buffer_usage(query_text text)
RETURNS TABLE (
    shared_hit_blocks    bigint,
    shared_read_blocks   bigint,
    shared_dirtied_blocks bigint
)
LANGUAGE plpgsql
AS $$
DECLARE
    plan_row     record;
    plan_text    text;
    hit_blocks   bigint := 0;
    read_blocks  bigint := 0;
    dirtied_blocks bigint := 0;
BEGIN
    FOR plan_row IN
        EXECUTE format(
            'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, FORMAT JSON) %s',
            query_text)
    LOOP
        plan_text := plan_row."QUERY PLAN"::text;
    END LOOP;

    IF plan_text IS NULL THEN
        RAISE EXCEPTION 'EXPLAIN returned no plan for: %', query_text;
    END IF;

    -- Node counters are inclusive: PostgreSQL copies child buffer usage into
    -- the parent.  Read the first (top-level Plan) occurrence so children are
    -- not counted a second time.
    hit_blocks := coalesce(
        substring(plan_text FROM '"Shared Hit Blocks": ([0-9]+)')::bigint,
        0);
    read_blocks := coalesce(
        substring(plan_text FROM '"Shared Read Blocks": ([0-9]+)')::bigint,
        0);
    dirtied_blocks := coalesce(
        substring(plan_text FROM '"Shared Dirtied Blocks": ([0-9]+)')::bigint,
        0);

    RETURN QUERY SELECT hit_blocks, read_blocks, dirtied_blocks;
END;
$$;

CREATE FUNCTION resident_shared_buffers(table_oid regclass)
RETURNS bigint
LANGUAGE plpgsql
STABLE
AS $$
BEGIN
    IF to_regclass('pg_buffercache') IS NULL THEN
        RETURN NULL;
    END IF;

    RETURN (
        SELECT count(*)
        FROM pg_buffercache AS bc
        WHERE bc.reldatabase = (SELECT oid FROM pg_database
                                WHERE datname = current_database())
          AND bc.relfilenode = pg_relation_filenode(table_oid)
          AND bc.relforknumber = 0
    );
END;
$$;

CREATE TEMP TABLE buffer_measurement AS
SELECT *
FROM (
    SELECT
        'heap'::text AS access_method,
        pg_relation_size('heap_buffer_probe'::regclass) AS relation_bytes,
        resident_shared_buffers('heap_buffer_probe'::regclass)
            AS resident_shared_buffers,
        u.shared_hit_blocks,
        u.shared_read_blocks,
        u.shared_dirtied_blocks,
        u.shared_hit_blocks + u.shared_read_blocks AS shared_blocks_used
    FROM pax_buffer_usage(
        'SELECT count(*), sum(length(payload)), sum(hashtext(payload))
         FROM heap_buffer_probe WHERE id % 211 = 0') AS u

    UNION ALL

    SELECT
        'pax'::text,
        pg_relation_size('pax_buffer_probe'::regclass),
        resident_shared_buffers('pax_buffer_probe'::regclass),
        u.shared_hit_blocks,
        u.shared_read_blocks,
        u.shared_dirtied_blocks,
        u.shared_hit_blocks + u.shared_read_blocks
    FROM pax_buffer_usage(
        'SELECT count(*), sum(length(payload)), sum(hashtext(payload))
         FROM pax_buffer_probe WHERE id % 211 = 0') AS u
) AS measurements;

\echo ''
\echo 'Warm-cache buffer usage for the identical sequential-scan query:'
SELECT *
FROM buffer_measurement
ORDER BY access_method;

\echo ''
\echo 'Result equivalence check:'
SELECT NOT EXISTS (
    (SELECT count(*), sum(length(payload)), sum(hashtext(payload))
     FROM heap_buffer_probe WHERE id % 211 = 0
     EXCEPT ALL
     SELECT count(*), sum(length(payload)), sum(hashtext(payload))
     FROM pax_buffer_probe WHERE id % 211 = 0)
    UNION ALL
    (SELECT count(*), sum(length(payload)), sum(hashtext(payload))
     FROM pax_buffer_probe WHERE id % 211 = 0
     EXCEPT ALL
     SELECT count(*), sum(length(payload)), sum(hashtext(payload))
     FROM heap_buffer_probe WHERE id % 211 = 0)
) AS identical_results;

-- Validate the measurement itself, but deliberately do not assert that PAX
-- must be larger or smaller: page packing and cache state affect the result.
DO $$
DECLARE
    heap_resident bigint;
    pax_resident  bigint;
    heap_used     bigint;
    pax_used      bigint;
BEGIN
    SELECT resident_shared_buffers, shared_blocks_used
      INTO heap_resident, heap_used
      FROM buffer_measurement WHERE access_method = 'heap';

    SELECT resident_shared_buffers, shared_blocks_used
      INTO pax_resident, pax_used
      FROM buffer_measurement WHERE access_method = 'pax';

    -- Resident-page counts are only checked when pg_buffercache was available.
    IF heap_resident IS NOT NULL AND pax_resident IS NOT NULL
       AND (heap_resident <= 0 OR pax_resident <= 0) THEN
        RAISE EXCEPTION
            'relations are not resident in shared buffers (heap=%, pax=%)',
            heap_resident, pax_resident;
    END IF;

    IF heap_used <= 0 OR pax_used <= 0 THEN
        RAISE EXCEPTION
            'EXPLAIN BUFFERS returned no shared-buffer usage (heap=%, pax=%)',
            heap_used, pax_used;
    END IF;

    IF (SELECT ARRAY[count(*), sum(length(payload)), sum(hashtext(payload))]
        FROM heap_buffer_probe WHERE id % 211 = 0)
       IS DISTINCT FROM
       (SELECT ARRAY[count(*), sum(length(payload)), sum(hashtext(payload))]
        FROM pax_buffer_probe WHERE id % 211 = 0) THEN
        RAISE EXCEPTION 'heap and PAX query results differ';
    END IF;
END;
$$;

\echo ''
\echo 'Shared-buffer access ratio (same query, warm cache):'
SELECT
    (SELECT shared_blocks_used FROM buffer_measurement
      WHERE access_method = 'heap')        AS heap_shared_blocks,
    (SELECT shared_blocks_used FROM buffer_measurement
      WHERE access_method = 'pax')         AS pax_shared_blocks,
    round(
        (SELECT shared_blocks_used FROM buffer_measurement
          WHERE access_method = 'pax')::numeric
        / NULLIF((SELECT shared_blocks_used FROM buffer_measurement
                   WHERE access_method = 'heap'), 0),
        2)                                  AS pax_over_heap;

\echo ''
\echo 'Shared-buffer comparison completed successfully.'

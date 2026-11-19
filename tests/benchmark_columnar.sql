-- Mesures comparatives heap / PAX sur des donnees identiques.
--
-- Ce test ne suppose PAS que PAX gagne : il rapporte les quatre familles de
-- mesures et laisse le verdict s'en degager. Deux d'entre elles sont
-- actuellement favorables a PAX (compressibilite du format), deux ne le sont
-- pas encore (taille sur disque, vitesse de lecture). Voir README.md.
--
-- Sortie : TSV sur stdout, consomme par benchmark_columnar.sh.

\set QUIET on

CREATE EXTENSION pax_am;

-- ===========================================================================
-- Schemas de test
-- ===========================================================================
-- A : etroite, colonnes de longueur fixe
-- B : large, une colonne fixe + cinq texte
-- C : 90 % de NULL

CREATE TABLE a_h (id int, qty int) USING heap;
CREATE TABLE a_p (id int, qty int) USING pax;

CREATE TABLE b_h (id int, c1 text, c2 text, c3 text, c4 text, c5 text) USING heap;
CREATE TABLE b_p (id int, c1 text, c2 text, c3 text, c4 text, c5 text) USING pax;

CREATE TABLE c_h (id int, a int, b int, c int, d int) USING heap;
CREATE TABLE c_p (id int, a int, b int, c int, d int) USING pax;

INSERT INTO a_h SELECT g, g % 100              FROM generate_series(1, :rows) g;
INSERT INTO a_p SELECT g, g % 100              FROM generate_series(1, :rows) g;

INSERT INTO b_h SELECT g, 'texte'||g, 'texte'||g, 'texte'||g, 'texte'||g, 'texte'||g
FROM generate_series(1, :rows) g;
INSERT INTO b_p SELECT g, 'texte'||g, 'texte'||g, 'texte'||g, 'texte'||g, 'texte'||g
FROM generate_series(1, :rows) g;

INSERT INTO c_h SELECT g,
       CASE WHEN g % 10 = 0 THEN g END, CASE WHEN g % 10 = 0 THEN g END,
       CASE WHEN g % 10 = 0 THEN g END, CASE WHEN g % 10 = 0 THEN g END
FROM generate_series(1, :rows) g;
INSERT INTO c_p SELECT g,
       CASE WHEN g % 10 = 0 THEN g END, CASE WHEN g % 10 = 0 THEN g END,
       CASE WHEN g % 10 = 0 THEN g END, CASE WHEN g % 10 = 0 THEN g END
FROM generate_series(1, :rows) g;

-- Pas de ANALYZE : le scan d'echantillonnage de PAX n'est pas supporte, et les
-- deux tables sont de toute facon balayees en sequentiel.

-- ===========================================================================
-- Verification : les deux AM doivent rendre exactement les memes resultats
-- ===========================================================================
DO $$
DECLARE
    bad text := '';
BEGIN
    IF (SELECT count(*) FROM a_h) <> (SELECT count(*) FROM a_p)
       OR (SELECT sum(qty) FROM a_h) <> (SELECT sum(qty) FROM a_p)
    THEN bad := bad || 'A '; END IF;

    IF (SELECT count(*) FROM b_h) <> (SELECT count(*) FROM b_p)
       OR (SELECT sum(length(c1)) FROM b_h) <> (SELECT sum(length(c1)) FROM b_p)
       OR (SELECT sum(id) FROM b_h) <> (SELECT sum(id) FROM b_p)
    THEN bad := bad || 'B '; END IF;

    IF (SELECT count(*) FROM c_h) <> (SELECT count(*) FROM c_p)
       OR (SELECT count(a) FROM c_h) <> (SELECT count(a) FROM c_p)
    THEN bad := bad || 'C '; END IF;

    IF bad <> '' THEN
        RAISE EXCEPTION 'resultats divergents entre heap et PAX sur : %', bad;
    END IF;
END;
$$;

-- ===========================================================================
-- Mesure de temps d'execution
-- ===========================================================================
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION exec_ms(query_text text)
RETURNS numeric
LANGUAGE plpgsql
VOLATILE
AS $$
DECLARE
    plan_row record;
    plan_text text;
    best     numeric := NULL;
BEGIN
    -- SUMMARY doit rester actif : en PG19, SUMMARY OFF retire aussi
    -- "Execution Time" du plan JSON.
    FOR plan_row IN
        EXECUTE format(
            'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) %s',
            query_text)
    LOOP
        plan_text := plan_row."QUERY PLAN"::text;
    END LOOP;

    -- "Execution Time" n'apparait qu'une fois, au niveau racine du plan
    SELECT (regexp_matches(plan_text, '"Execution Time": ([0-9.]+)', 'g'))[1]::numeric
      INTO best;

    RETURN COALESCE(best, 0);
END;
$$;

-- Meilleure de :runs executions, sur cache chaud (une execution de chauffe).
CREATE FUNCTION best_ms(query_text text, runs int)
RETURNS numeric
LANGUAGE plpgsql
VOLATILE
AS $$
DECLARE
    b   numeric := NULL;
    cur numeric;
    i   int;
BEGIN
    PERFORM exec_ms(query_text);                 -- chauffe
    FOR i IN 1..runs LOOP
        cur := exec_ms(query_text);
        IF b IS NULL OR cur < b THEN
            b := cur;
        END IF;
    END LOOP;
    RETURN b;
END;
$$;

-- ===========================================================================
-- Resultat : une ligne TSV par schema
-- ===========================================================================
\pset format unaligned
\pset tuples_only on
\pset fieldsep '|'

SELECT sch,
       heap_size,
       pax_size,
       heap_file,
       pax_file,
       proj_heap_ms,
       proj_pax_ms,
       full_heap_ms,
       full_pax_ms
FROM (
    SELECT * FROM (VALUES
        ('A etroite',
         (SELECT pg_relation_size('a_h')),
         (SELECT pg_relation_size('a_p')),
         (SELECT pg_relation_filepath('a_h')),
         (SELECT pg_relation_filepath('a_p')),
         (SELECT best_ms('SELECT sum(id) FROM a_h', :runs)),
         (SELECT best_ms('SELECT sum(id) FROM a_p', :runs)),
         (SELECT best_ms('SELECT sum(id+qty) FROM a_h', :runs)),
         (SELECT best_ms('SELECT sum(id+qty) FROM a_p', :runs))),
        ('B large',
         (SELECT pg_relation_size('b_h')),
         (SELECT pg_relation_size('b_p')),
         (SELECT pg_relation_filepath('b_h')),
         (SELECT pg_relation_filepath('b_p')),
         (SELECT best_ms('SELECT sum(id) FROM b_h', :runs)),
         (SELECT best_ms('SELECT sum(id) FROM b_p', :runs)),
         (SELECT best_ms(
             'SELECT sum(length(c1)+length(c2)+length(c3)+length(c4)+length(c5)) FROM b_h',
             :runs)),
         (SELECT best_ms(
             'SELECT sum(length(c1)+length(c2)+length(c3)+length(c4)+length(c5)) FROM b_p',
             :runs))),
        ('C 90% NULL',
         (SELECT pg_relation_size('c_h')),
         (SELECT pg_relation_size('c_p')),
         (SELECT pg_relation_filepath('c_h')),
         (SELECT pg_relation_filepath('c_p')),
         (SELECT best_ms('SELECT count(a) FROM c_h', :runs)),
         (SELECT best_ms('SELECT count(a) FROM c_p', :runs)),
         (SELECT best_ms('SELECT sum(coalesce(a,0)+coalesce(b,0)+coalesce(c,0)+coalesce(d,0)) FROM c_h', :runs)),
         (SELECT best_ms('SELECT sum(coalesce(a,0)+coalesce(b,0)+coalesce(c,0)+coalesce(d,0)) FROM c_p', :runs)))
    ) AS t(sch, heap_size, pax_size, heap_file, pax_file,
           proj_heap_ms, proj_pax_ms, full_heap_ms, full_pax_ms)
) AS m;

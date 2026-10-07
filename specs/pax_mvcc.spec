# Basic PAX MVCC visibility and subtransaction abort tests
#
# The table is 20 columns wide (primary key, integers, floats, text), the same
# layout sql/pax_am.sql uses. It matters here for a specific reason: every
# visibility decision in PAX is taken per version in the per-column metadata,
# not per row. A scan that shows the right *keys* while showing a neighbour's
# *values* is a failure a key-only check cannot see, so the steps below assert
# on the concatenation of the non-key columns too, through pax_mvcc_rows.

setup
{
    CREATE TABLE pax_mvcc (
        id int PRIMARY KEY, k2 smallint, k4 int, k8 bigint,
        f4 real, f8 double precision, num numeric(12,2),
        t1 text, t2 text, t3 varchar(32), t4 text,
        b1 bool, d1 date, ts1 timestamp, iv1 interval,
        uu uuid, ba bytea, ch "char", i_null int, t_null text
    ) USING pax
    WITH (fillfactor = 80, toast_tuple_target = 512);

    INSERT INTO pax_mvcc
    VALUES (0, 0, 0, 0, 0.0, 0.0, 0.00,
            'row0', 'r0', 'r0', NULL,
            false, DATE '2024-01-01', TIMESTAMP '2024-01-01 00:00:00',
            '0 min'::interval,
            '00000000-0000-0000-0000-000000000000'::uuid,
            '\x00'::bytea, '0', 0, NULL);

    -- One text row per slot width the format distinguishes: by-value 4 (k4),
    -- by-value 8 (f8), by-reference fixed 16 (uu), by-reference 1 (ch), and
    -- two varlena columns that are NULL in some rows, so the NULL bitmap of
    -- each column is exercised independently.
    --
    -- Careful when commenting in here: the spec lexer only skips its own
    -- comment marker, so a closing brace written in a SQL comment below would
    -- end this block early and the parser would then choke on the rest.
    CREATE VIEW pax_mvcc_rows AS
    SELECT id,
           t1 || '/' || t2 || '/' || k4::text || '/' || f8::text || '/'
               || coalesce(t4, '-') || '/' || coalesce(t_null, '-') || '/'
               || coalesce(uu::text, '-') || '/' || ch::text AS row_text
    FROM pax_mvcc;
}

teardown
{
    DROP VIEW pax_mvcc_rows;
    DROP TABLE pax_mvcc;
}

session s1
step s1_begin { BEGIN; }
step s1_insert1 {
    INSERT INTO pax_mvcc
    VALUES (1, 1, 11, 1100, 1.1, 1.25, 1.50,
            'uncommitted', 'u1', 'u', 'ut4',
            true, DATE '2024-02-02', TIMESTAMP '2024-02-02 02:02:02',
            '1 min'::interval,
            '00000000-0000-0000-0000-000000000001'::uuid,
            '\x01'::bytea, '1', 7, 'nt1');
}
step s1_own_attrs {
    SELECT id,
           xmin = pg_current_xact_id()::text::xid AS own_xmin,
           xmax = '0'::xid AS no_xmax,
           cmin IS NOT NULL AS has_cmin,
           cmax IS NOT NULL AS has_cmax,
           cmin = cmax AS shared_command
    FROM pax_mvcc WHERE id = 1;
}
step s1_own_row {
    SELECT row_text FROM pax_mvcc_rows WHERE id = 1;
}
step s1_savepoint { SAVEPOINT pax_sp; }
step s1_insert2 {
    INSERT INTO pax_mvcc
    VALUES (2, 2, 12, 1200, 2.2, 2.50, 2.50,
            'subxact', 's2', 's', NULL,
            false, DATE '2024-03-03', TIMESTAMP '2024-03-03 03:03:03',
            '2 min'::interval,
            '00000000-0000-0000-0000-000000000002'::uuid,
            '\x02'::bytea, '2', 8, NULL);
}
step s1_see_subxact {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s1_rollback_subxact { ROLLBACK TO pax_sp; }
step s1_after_subxact_abort {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s1_abort { ROLLBACK; }
step s1_commit { COMMIT; }
step s1_update { UPDATE pax_mvcc SET id = 10 WHERE id = 0; }
step s1_update_savepoint { SAVEPOINT pax_update_sp; }
step s1_second_update { UPDATE pax_mvcc SET id = 20 WHERE id = 10; }
step s1_see_second_update {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s1_rollback_update_savepoint {
    ROLLBACK TO pax_update_sp;
}
step s1_see_first_update {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s1_delete { DELETE FROM pax_mvcc WHERE id = 10; }
step s1_see_delete {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}

session s2
step s2_uncommitted {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_snapshot_begin {
    BEGIN ISOLATION LEVEL REPEATABLE READ;
}
step s2_snapshot_before {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_snapshot_still {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_snapshot_commit { COMMIT; }
step s2_after_abort {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_after_commit {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_see_values {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_lock_begin { BEGIN; }
step s2_lock_row {
    SELECT id, row_text FROM pax_mvcc_rows WHERE id = 0 FOR UPDATE;
}
step s2_lock_commit { COMMIT; }
step s2_sub_lock_begin { BEGIN; }
step s2_sub_lock_savepoint { SAVEPOINT pax_lock_sp; }
step s2_sub_lock_row {
    SELECT id, row_text FROM pax_mvcc_rows WHERE id = 0 FOR UPDATE;
}
step s2_sub_lock_rollback { ROLLBACK TO pax_lock_sp; }
step s2_sub_lock_commit { COMMIT; }
step s2_dml_snapshot_begin {
    BEGIN ISOLATION LEVEL REPEATABLE READ;
}
step s2_dml_snapshot_before {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_dml_snapshot_still {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_dml_snapshot_commit { COMMIT; }
step s2_rr_lock_conflict {
    SELECT id, row_text FROM pax_mvcc_rows WHERE id = 0 FOR UPDATE;
}
step s2_rr_rollback { ROLLBACK; }
step s2_conflict_begin {
    BEGIN ISOLATION LEVEL READ COMMITTED;
}
step s2_conflict_update {
    UPDATE pax_mvcc SET id = id + 10 WHERE id >= 0;
}
step s2_conflict_see {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_conflict_commit { COMMIT; }
step s2_delete_conflict_begin {
    BEGIN ISOLATION LEVEL READ COMMITTED;
}
step s2_delete_conflict {
    DELETE FROM pax_mvcc WHERE id >= 0;
}
step s2_delete_conflict_see {
    SELECT array_agg(id ORDER BY id) AS ids,
           array_agg(row_text ORDER BY id) AS rows
    FROM pax_mvcc_rows;
}
step s2_delete_conflict_commit { COMMIT; }

# An uncommitted insert is visible only to its own transaction.
permutation s1_begin s1_insert1 s1_own_attrs s1_own_row s2_uncommitted s1_commit s2_after_commit

# A rolled-back subtransaction row is hidden even from its top-level xact.
permutation s1_begin s1_insert1 s1_savepoint s1_insert2 s1_see_subxact s1_rollback_subxact s1_after_subxact_abort s2_uncommitted s1_abort s2_after_abort

# A repeatable-read snapshot keeps its original view across another commit.
permutation s1_begin s1_insert1 s2_snapshot_begin s2_snapshot_before s1_commit s2_snapshot_still s2_snapshot_commit s2_after_commit

# An uncommitted UPDATE is visible only to its own transaction.
permutation s1_begin s1_update s2_see_values s1_commit s2_see_values

# A concurrent READ COMMITTED writer waits, follows the committed version
# chain through TM_Updated/TUPLE_LOCK_FLAG_FIND_LAST_VERSION, and retries EPQ.
permutation s1_begin s1_update s2_conflict_begin s2_conflict_update(*) s1_commit s2_conflict_see s2_conflict_commit
permutation s1_begin s1_update s2_conflict_begin s2_conflict_update(*) s1_abort s2_conflict_see s2_conflict_commit

# Concurrent DELETE follows the committed update chain and rechecks the row.
permutation s1_begin s1_update s2_delete_conflict_begin s2_delete_conflict(*) s1_commit s2_delete_conflict_see s2_delete_conflict_commit

# A rolled-back UPDATE leaves the old version visible.
permutation s1_begin s1_update s2_see_values s1_abort s2_see_values

# Version chains follow repeated updates, and savepoint rollback exposes the
# version written by the preceding command in the same transaction.
permutation s1_begin s1_update s1_update_savepoint s1_second_update s1_see_second_update s1_rollback_update_savepoint s1_see_first_update s2_see_values s1_commit s2_see_values

# DELETE follows the same visibility and rollback rules as UPDATE.
permutation s1_begin s1_update s1_delete s1_see_delete s2_see_values s1_commit s2_see_values
permutation s1_begin s1_delete s2_see_values s1_abort s2_see_values

# A repeatable-read transaction keeps the pre-UPDATE and pre-DELETE versions.
permutation s2_dml_snapshot_begin s2_dml_snapshot_before s1_begin s1_update s1_commit s2_dml_snapshot_still s2_dml_snapshot_commit s2_see_values
permutation s2_dml_snapshot_begin s2_dml_snapshot_before s1_begin s1_delete s1_commit s2_dml_snapshot_still s2_dml_snapshot_commit s2_see_values

# Under a transaction snapshot, committed concurrent DML is reported to the
# executor instead of silently following the chain without FIND_LAST_VERSION.
permutation s2_dml_snapshot_begin s2_dml_snapshot_before s1_begin s1_update s1_commit s2_rr_lock_conflict s2_rr_rollback

# The row-lock callback stores a transaction-scoped logical lock; UPDATE waits
# for it and then succeeds after the locking transaction commits.
permutation s2_lock_begin s2_lock_row s1_begin s1_update(*) s2_lock_commit s1_commit s2_see_values

# A lock acquired in an aborted subtransaction does not block a later writer.
permutation s2_sub_lock_begin s2_sub_lock_savepoint s2_sub_lock_row s2_sub_lock_rollback s1_begin s1_update s1_commit s2_sub_lock_commit s2_see_values

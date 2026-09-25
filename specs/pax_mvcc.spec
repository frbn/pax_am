# Basic PAX MVCC visibility and subtransaction abort tests

setup
{
    CREATE TABLE pax_mvcc (id int) USING pax;
    INSERT INTO pax_mvcc VALUES (0);
}

teardown
{
    DROP TABLE pax_mvcc;
}

session s1
step s1_begin { BEGIN; }
step s1_insert1 { INSERT INTO pax_mvcc VALUES (1); }
step s1_own_attrs {
    SELECT id,
           xmin = pg_current_xact_id()::text::xid AS own_xmin,
           xmax = '0'::xid AS no_xmax,
           cmin IS NOT NULL AS has_cmin,
           cmax IS NOT NULL AS has_cmax,
           cmin = cmax AS shared_command
    FROM pax_mvcc WHERE id = 1;
}
step s1_savepoint { SAVEPOINT pax_sp; }
step s1_insert2 { INSERT INTO pax_mvcc VALUES (2); }
step s1_see_subxact {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}
step s1_rollback_subxact { ROLLBACK TO pax_sp; }
step s1_after_subxact_abort {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}
step s1_abort { ROLLBACK; }
step s1_commit { COMMIT; }

session s2
step s2_uncommitted {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}
step s2_snapshot_begin {
    BEGIN ISOLATION LEVEL REPEATABLE READ;
}
step s2_snapshot_before {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}
step s2_snapshot_still {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}
step s2_snapshot_commit { COMMIT; }
step s2_after_abort {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}
step s2_after_commit {
    SELECT array_agg(id ORDER BY id) FROM pax_mvcc;
}

# An uncommitted insert is visible only to its own transaction.
permutation s1_begin s1_insert1 s1_own_attrs s2_uncommitted s1_commit s2_after_commit

# A rolled-back subtransaction row is hidden even from its top-level xact.
permutation s1_begin s1_insert1 s1_savepoint s1_insert2 s1_see_subxact s1_rollback_subxact s1_after_subxact_abort s2_uncommitted s1_abort s2_after_abort

# A repeatable-read snapshot keeps its original view across another commit.
permutation s1_begin s1_insert1 s2_snapshot_begin s2_snapshot_before s1_commit s2_snapshot_still s2_snapshot_commit s2_after_commit

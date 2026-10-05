#!/usr/bin/env bash
#
# Compare shared-buffer usage and scan time between heap and PAX on a *wide*
# table (a dozen columns, one of them indexed), for queries that project only a
# few columns.
#
# The motivation is legitimate: columnar storage should let a query that touches
# 2 of 12 columns avoid reading the other 10. Whether PAX actually delivers that
# is an empirical question, and this script reports what it measures -- in
# either direction. See the note at the bottom of the SQL file.
#
# It is a measurement, not a golden regression: block counts depend on cache
# state, so nothing here asserts that PAX must win.
#
#   PAX_WIDE_TEST_ROWS=20000 ./tests/compare_wide_projection.sh
#
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
pg_config=${PG_CONFIG:-pg_config}
pg_bindir=$("$pg_config" --bindir)
psql_bin="$pg_bindir/psql"
createdb_bin="$pg_bindir/createdb"
dropdb_bin="$pg_bindir/dropdb"
rows=${PAX_WIDE_TEST_ROWS:-20000}
database=${PAX_WIDE_TEST_DB:-pax_wide_test_$$}

cleanup() {
    "$dropdb_bin" --if-exists --force "$database" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$createdb_bin" "$database"
"$psql_bin" -X -v ON_ERROR_STOP=1 -v "rows=$rows" -d "$database" \
    -f "$script_dir/compare_wide_projection.sql"

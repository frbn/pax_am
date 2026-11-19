#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
pg_config=${PG_CONFIG:-pg_config}
pg_bindir=$("$pg_config" --bindir)
psql_bin="$pg_bindir/psql"
createdb_bin="$pg_bindir/createdb"
dropdb_bin="$pg_bindir/dropdb"
rows=${PAX_BUFFER_TEST_ROWS:-5000}
database=${PAX_BUFFER_TEST_DB:-pax_buffer_test_$$}

cleanup() {
    "$dropdb_bin" --if-exists --force "$database" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$createdb_bin" "$database"
"$psql_bin" -X -v ON_ERROR_STOP=1 -v "rows=$rows" -d "$database" \
    -f "$script_dir/compare_shared_buffers.sql"

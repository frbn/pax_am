# pax_am

PAX (“Partition Attributes Across”) is a PostgreSQL table access-method
prototype using a column-oriented physical page layout.

The current prototype supports sequential INSERT/UPDATE/DELETE/SELECT, basic
MVCC version chains, and row locking for PostgreSQL 19devel. It does **not**
implement Generic WAL, indexes, VACUUM, freezing, speculative insertion, or a
v2-to-v3 page migration, and must not be used for production data.

Build and test against a configured PostgreSQL 19 installation with:

```sh
make
make install
make installcheck
```

See `analyse1.md` for the current format, algorithms, test coverage, and known
limitations.

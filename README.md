# pax_am

PAX (“Partition Attributes Across”) is a PostgreSQL table access-method
prototype using a column-oriented physical page layout.

The current prototype supports sequential and TID INSERT/UPDATE/DELETE/SELECT,
basic MVCC version chains, row locking, COPY, byte-packed columnar storage,
free-space-map-driven inserts, VACUUM/ANALYZE, and Generic WAL crash recovery
for PostgreSQL 19devel.

It does **not** implement speculative insertion or a page-version migration, and
must not be used for production data. VACUUM marks dead versions and freezes them,
reclaims their variable-length payloads in place, and lets a later INSERT reuse the
vacated slots; a dead version that no INSERT refills still holds its 32 metadata
bytes.

Indexes work for reads, writes, unique constraints, multi-row UPDATE/DELETE and
VACUUM cleanup. PAX declines nbtree's bottom-up index deletion — that is an
optimisation the index AM may ask for and the table AM may refuse — so index
cleanup progresses more slowly than on heap. See `analyse1.md`, section 13, for
what is implemented and section 14 for the multi-row UPDATE deadlock that was fixed
and how to reproduce it under `--enable-cassert`.

Build and test against a configured PostgreSQL 19 installation with:

```sh
make
make install
make installcheck
```

An assertion-enabled build is worth using as the default test target: PG asserts
`lockmode == BUFFER_LOCK_UNLOCK` on entry to `BufferLockAcquire`, so a buffer
content-lock self-deadlock becomes an immediate `TRAP` with a backtrace instead of
a silent hang. Buffer content locks are invisible to `pg_locks`,
`pg_blocking_pids()` and `lock_timeout`, so there is no other way to see them.

```sh
make PG_CONFIG=/path/to/cassert/bin/pg_config
cp pax_am.so /path/to/cassert/lib/postgresql/pax_am.so
make installcheck PG_CONFIG=/path/to/cassert/bin/pg_config \
                 PGHOST=/socket/dir PGPORT=NNNN PGUSER=you
```

### Test tables

Every table in `sql/pax_am.sql` and `specs/pax_mvcc.spec` has the same shape:
20 columns — primary key, several integer widths, floats, text — with one column
per slot stride the format distinguishes (1, 2, 4, 8, 16). Each is created
`WITH (fillfactor = 80, toast_tuple_target = 512)`.

Two things about that clause are worth knowing before adding a table:

- It is **not inherited**. Neither `LIKE` nor `CREATE TABLE AS` copies
  reloptions, not even `LIKE ... INCLUDING ALL`. A regression sweep near the end
  of the suite fails if any PAX table is missing them, so a forgotten clause is
  caught rather than silently tolerated.
- PAX **records both options and obeys neither** today. `fillfactor` cannot bound
  a page below its chunk set — at 20 columns that is ~55% of the usable area, so
  `fillfactor = 80` would be free but anything lower collapses the table to one
  row per page. `toast_tuple_target` has nothing to redirect to, PAX having no
  TOAST. Both are measured and recorded in `analyse1.md`.

To compare shared-buffer usage for one identical query on a `heap` table and
a `pax` table:

```sh
make shared-buffers-test
```

This measurement target creates a throwaway database, loads identical data
into both access methods, runs the same warm sequential scan against each, and
reports `EXPLAIN (ANALYZE, BUFFERS)` shared hit/read/dirtied blocks plus the
resident page count from `pg_buffercache` (optional). It also asserts that both
queries return identical results. Buffer counts are cache- and
environment-dependent, so it is intentionally not part of `installcheck` and has
no golden output. Set `PAX_BUFFER_TEST_ROWS` to change the row count (default
5000). It is the regression guard for scan buffer access: PAX should stay within
a small factor of heap (roughly 1.05x at the default size) rather than one
`ReadBuffer` per tuple.

### Mesurer les avantages (et les inconvénients) de PAX

```sh
make columnar-benchmark
```

Charge des données identiques dans des tables `heap` et `pax` sur trois schémas
— étroite, large, et 90 % de NULL — puis rapporte quatre familles de mesures :
taille sur disque, taille après compression gzip, lecture avec projection
partielle, et lecture complète. Le test vérifie d'abord que les deux AM
renvoient des résultats identiques, puis conclut sans rien supposer du
résultat. Runtime ~8 s pour 50 000 lignes ; ajustez avec `PAX_BENCH_ROWS` et
`PAX_BENCH_RUNS`.

Résultat actuel, 50 000 lignes, PostgreSQL 19devel :

| Famille | PAX vs heap | Verdict |
|---|---|---|
| Taille brute | 1,12x à 1,41x | PAX perd |
| Taille compressée | 0,75x à 0,93x | **PAX gagne partout** |
| Lecture projetée | 6,4x à 8,8x | PAX perd |
| Lecture complète | 2,4x à 8,7x | PAX perd |

Concrètement : **le seul avantage aujourd'hui mesuré est la taille après
compression**, où PAX est désormais 7 % à 25 % plus petit que heap. En
contrepartie il occupe toujours 12 % à 41 % d'espace brut en plus, et la
projection partielle — l'argument central du stockage columnaire — n'est pas
exploitée : PAX y reste plus lent que heap. Ce sont des mesures, pas des
prévisions ; voir `analyse1.md` sections 8 et 12 pour les causes côté code.

## Schémas de stockage

Quatre diagrammes SVG illustrent la disposition physique des pages :

- [`docs/heap-page.svg`](docs/heap-page.svg) — page heap classique : line
  pointers, tuples autoporteurs, espace libre central.
- [`docs/pax-page.svg`](docs/pax-page.svg) — page PAX **v4, format périmé** :
  une région contiguë par colonne, déplacée à chaque insertion.
- [`docs/pax-page-v5.svg`](docs/pax-page-v5.svg) — **le format courant (v5)** :
  chaque colonne est une chaîne de chunks de taille fixe, alloués une fois et
  jamais déplacés. C'est ce qui donne à une ligne PAX une adresse stable, comme
  une ligne de heap.
- [`docs/pax-page-reelle.svg`](docs/pax-page-reelle.svg) — **une page PAX
  réellement dumpée**, avec les offsets et tailles lus dans les octets bruts.
  Même outil que `pax-page-v5.svg`, mais sur le cas `ventes` ci-dessous, qui
  montre les valeurs nulles.

La comparaison visuelle fait ressortir l'inversion principale : là où heap
range les tuples en lignes et doit donc traverser tous les en-têtes pour lire
une colonne, PAX range les colonnes côte à côte et n'a plus qu'à suivre les
chaînes de chunks.

Le passage de v4 à v5 se lit dans le schéma : en v4 les régions vivaient sous
`pd_lower` et chaque insertion les.memmoveait, en corrigeant `offsets[]` au
passage. En v5 la zone basse ne porte plus que les métadonnées de version, et
les slots vivent dans l'arène, au-dessus de `pd_upper`, où chunks et charges
utiles **s'entrelacent** sur une descente commune. Le schéma les dessine à leur
vrai offset précisément parce qu'ils se mélangent.

### Régénérer le schéma d'une page réelle

`docs/inspect_pax_page.py` dumper une page avec `pageinspect.get_raw_page()`,
la décode selon les structures de `pax_am.c`, et produit le SVG :

```sh
./docs/inspect_pax_page.py ventes 0 --db ma_base --verify -o docs/pax-page-reelle.svg
```

`--verify` contrôle la cohérence du layout avant de dessiner : chaque chaîne de
chunks se termine bien sur le maillon désigné par sa queue, `n_rows` ne dépasse
pas `PAX_CHUNK_MAX_ROWS`, aucun chunk ne déborde de l'arène, et la zone des
métadonnées fait exactement `n_tuples × 32` octets.

`PAX_CHUNK_MAX_ROWS` est recopié en dur dans le script : il doit être remis à
jour à la main quand la constante change dans `pax_am.c`, faute de quoi le
décodeur lirait les chunks au mauvais endroit — silencieusement, et seulement
sur les tables assez larges pour avoir plus d'un chunk.

Les deux schémas livrés ont été produits sur ces tables :

```sql
-- docs/pax-page-reelle.svg : 3 colonnes, valeurs nulles
CREATE TABLE ventes (id integer, nom text, quantite integer) USING pax;
INSERT INTO ventes SELECT g,
       CASE WHEN g % 11 = 0 THEN NULL ELSE 'client-' || g END,
       CASE WHEN g % 7  = 0 THEN NULL ELSE (g * 3) % 1000 END
FROM generate_series(1, 120) g;

-- docs/pax-page-v5.svg : 6 colonnes, tous les pas de slot
CREATE TABLE demo (id integer, label text, small integer,
                   ratio double precision, note text, flag boolean) USING pax;
INSERT INTO demo SELECT g, 'ligne ' || g, g % 97, g * 1.5::float8,
       CASE WHEN g % 4 = 0 THEN NULL ELSE 'note-' || g END,
       (g % 2 = 0)
FROM generate_series(1, 200) g;
```

Soit 120 versions sur le bloc 0 de `ventes`, et 96 sur celui de `demo` avec
trois chunks de 32 versions par colonne et 24 valeurs nulles dans `note`. Les
chunks et les charges utiles sont entrelacés dans l'arène, ce que montre la
dernière section de chaque schéma : ils sont dessinés à leur vrai offset, dans
l'ordre des adresses, et non empilés en deux zones.

See `analyse1.md` for the current format, algorithms, test coverage, and known
limitations.

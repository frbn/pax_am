# pax_am

PAX (“Partition Attributes Across”) is a PostgreSQL table access-method
prototype using a column-oriented physical page layout.

The current prototype supports sequential and TID INSERT/UPDATE/DELETE/SELECT,
basic MVCC version chains, row locking, COPY, and Generic WAL crash recovery for
PostgreSQL 19devel. It does **not** implement indexes, VACUUM, freezing,
speculative insertion, or a v2-to-v3 page migration, and must not be used for
production data.

Build and test against a configured PostgreSQL 19 installation with:

```sh
make
make install
make installcheck
```

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
| Taille brute | 1,34x à 1,95x | PAX perd |
| Taille compressée | 0,82x à 1,04x | PAX gagne ou égalité |
| Lecture projetée | 6,7x à 9,3x | PAX perd |
| Lecture complète | 2,4x à 8,4x | PAX perd |

Concrètement : **le seul avantage aujourd'hui mesuré est la taille après
compression**. PAX occupe jusqu'à deux fois plus d'espace brut, mais la
disposition columnaire ramène la taille compressée au niveau de heap, voire en
dessous. En revanche la projection partielle — l'argument central du stockage
columnaire — n'est pas exploitée, et PAX s'y révèle plus lent que heap. Ce sont
des mesures, pas des prévisions : voir `analyse1.md` sections 8 et 12 pour les
causes côté code.

## Schémas de stockage

Trois diagrammes SVG illustrent la disposition physique des pages :

- [`docs/heap-page.svg`](docs/heap-page.svg) — page heap classique : line
  pointers, tuples autoporteurs, espace libre central.
- [`docs/pax-page.svg`](docs/pax-page.svg) — page PAX v3 : aucun line pointer,
  métadonnées columnaires de 32 o par version, une région par colonne, valeurs
  varlena en haut de page.
- [`docs/pax-page-reelle.svg`](docs/pax-page-reelle.svg) — **une page PAX
  réellement dumpée**, avec les offsets et tailles lus dans les octets bruts.

La comparaison visuelle fait ressortir l'inversion principale : là où heap
range les tuples en lignes et doit donc traverser tous les en-têtes pour lire
une colonne, PAX range les colonnes côte à côte et n'a plus qu'à suivre les
offsets de `PaxPageHeader`.

### Régénérer le schéma d'une page réelle

`docs/inspect_pax_page.py` dumper une page avec `pageinspect.get_raw_page()`,
la décode selon les structures de `pax_am.c`, et produit le SVG :

```sh
./docs/inspect_pax_page.py ventes 0 --db ma_base --verify -o docs/pax-page-reelle.svg
```

`--verify` contrôle la cohérence du layout avant de dessiner : régions
contiguës, total égal à `BLCKSZ`, et taille de chaque région conforme à
`bitmap_de_NULL + n_tuples × pas`. Le schéma livré a été produit sur la table

```sql
CREATE TABLE ventes (id integer, nom text, quantite integer) USING pax;
INSERT INTO ventes SELECT g,
       CASE WHEN g % 11 = 0 THEN NULL ELSE 'client-' || g END,
       CASE WHEN g % 7  = 0 THEN NULL ELSE (g * 3) % 1000 END
FROM generate_series(1, 120) g;
```

soit 114 versions sur le bloc 0, 40 octets libres, et 10 / 16 valeurs nulles
respectivement dans `nom` et `quantite`.

See `analyse1.md` for the current format, algorithms, test coverage, and known
limitations.

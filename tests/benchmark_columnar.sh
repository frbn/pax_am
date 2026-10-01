#!/usr/bin/env bash
# Mesure les effets reels de la disposition columnaire PAX face a heap.
#
# Quatre familles de mesures :
#   1. taille sur disque
#   2. compressibilite du format (gzip)
#   3. lecture avec projection partielle
#   4. lecture de toutes les colonnes
#
# Le test ne suppose rien du resultat : il rapporte, puis conclut. Une des
# quatre familles est actuellement favorable a PAX, trois ne le sont pas.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
pg_config=${PG_CONFIG:-pg_config}
pg_bindir=$("$pg_config" --bindir)
psql_bin="$pg_bindir/psql"
createdb_bin="$pg_bindir/createdb"
dropdb_bin="$pg_bindir/dropdb"

rows=${PAX_BENCH_ROWS:-50000}
runs=${PAX_BENCH_RUNS:-2}
database=${PAX_BENCH_DB:-pax_bench_$$}

cleanup() {
    "$dropdb_bin" --if-exists --force "$database" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$createdb_bin" "$database"

raw=$("$psql_bin" -X -q -v ON_ERROR_STOP=1 -v "rows=$rows" -v "runs=$runs" \
        -d "$database" -f "$script_dir/benchmark_columnar.sql")

data_dir=$("$psql_bin" -X -At -d "$database" -c 'SHOW data_directory')
"$psql_bin" -X -q -d "$database" -c 'CHECKPOINT;' >/dev/null

# Mesure la compressibilite du format pour chaque fichier de relation.
declare -a rowsout=()
while IFS='|' read -r sch hs ps hf pf phh php fhh fhp; do
    [ -z "$sch" ] && continue
    hg=$(gzip -9 -c "$data_dir/$hf" | wc -c)
    pz=$(gzip -9 -c "$data_dir/$pf" | wc -c)
    rowsout+=("$sch" "$hs" "$ps" "$hg" "$pz" "$phh" "$php" "$fhh" "$fhp")
done <<< "$raw"

python3 - "$rows" "$runs" "${rowsout[@]}" <<'PY'
import sys

rows, runs = int(sys.argv[1]), int(sys.argv[2])
vals = sys.argv[3:]
n = len(vals) // 9
recs = []
for i in range(n):
    c = vals[i * 9:(i + 1) * 9]
    recs.append((c[0], int(c[1]), int(c[2]), int(c[3]), int(c[4]),
                 float(c[5]), float(c[6]), float(c[7]), float(c[8])))

def kb(x):
    return f"{x:,}".replace(",", " ")

def ms(x):
    return f"{x:.1f}".replace(".", ",")

def ratio(x):
    return f"{x:.2f}".replace(".", ",")

W = 66
print()
print("=" * 78)
print(" PAX vs heap — mesures réelles".center(78))
print(f" {rows:,} lignes par table, meilleure de {runs} exécutions, cache chaud".center(78))
print("=" * 78)

print()
print(" 1. TAILLE SUR DISQUE ET COMPRESSIBILITÉ DU FORMAT")
print(" " + "-" * 76)
print(f" {'schéma':<12} {'heap (o)':>10} {'pax (o)':>10} {'pax/heap':>9} "
      f"{'heap gz':>9} {'pax gz':>9} {'pax/heap gz':>13}")
for sch, hs, ps, hg, pz, *_ in recs:
    print(f" {sch:<12} {kb(hs):>10} {kb(ps):>10} {ratio(ps/hs)+'x':>9} "
          f"{hg/hs*100:>8.1f}% {pz/ps*100:>8.1f}% {ratio(pz/hg)+'x':>13}")

print()
print(" 2. TEMPS DE LECTURE (ms)")
print(" " + "-" * 76)
print(f" {'schéma':<12} {'requête':<18} {'heap':>9} {'pax':>9} {'pax/heap':>10}")
for r in recs:
    sch, hs, ps, hg, pz, phh, php, fhh, fhp = r
    print(f" {sch:<12} {'projection':<18} {ms(phh):>9} {ms(php):>9} "
          f"{ratio(php/phh)+'x':>10}")
    print(f" {'':<12} {'toutes colonnes':<18} {ms(fhh):>9} {ms(fhp):>9} "
          f"{ratio(fhp/fhh)+'x':>10}")

print()
print(" 3. VERDICT, PAR FAMILLE DE MESURE")
print(" " + "-" * 76)
wins = {k: 0 for k in ("taille brute", "taille compressée",
                       "lecture projetée", "lecture complète")}
ties = {k: 0 for k in wins}
tot = {k: 0 for k in wins}
TOL = 0.02          # en dessous de 2 %, on neclaim rien


def judge(name, p, h, rf):
    tot[name] += 1
    r = p / h
    if r < 1 - TOL:
        wins[name] += 1
        return f"{rf:>8}  PAX gagne"
    if r > 1 + TOL:
        return f"{rf:>8}  PAX perd"
    ties[name] += 1
    return f"{rf:>8}  égalité   "


for r in recs:
    sch, hs, ps, hg, pz, phh, php, fhh, fhp = r
    print(f"   {sch}")
    for name, p, h, rf in [
        ("taille brute",     ps,    hs,   f"{ps/hs:.2f}x"),
        ("taille compressée", pz,   hg,   f"{pz/hg:.2f}x"),
        ("lecture projetée",  php,  phh,  f"{php/phh:.2f}x"),
        ("lecture complète",  fhp,  fhh,  f"{fhp/fhh:.2f}x"),
    ]:
        print(f"     {name:<17}" + judge(name, p, h, rf))
    print()

print(" BILAN  " + "  ".join(
    f"{k}: {v} victoire(s)/{tot[k]}" + (f" +{ties[k]} égalité(s)" if ties[k] else "")
    for k, v in wins.items()))
print(" " + "-" * 76)
print()
print(" Lecture du résultat")
print(" " + "-" * 76)
for name, v in wins.items():
    t = ties.get(name, 0)
    n_ = tot[name]
    if v == n_:
        verdict = "avantage démontré sur tous les schémas"
    elif v == 0 and t == 0:
        verdict = "PAX perd sur tous les schémas"
    elif v == 0:
        verdict = f"PAX perd partout, {t} égalité(s)"
    else:
        verdict = f"PAX gagne sur {v}/{n_}" + (f", {t} égalité(s)" if t else "")
    print(f"   {name:<17} {verdict}")
print()
PY

cat <<'NOTE'
 Où en est PAX aujourd'hui :

   « taille compressée » est le seul avantage réellement mesuré. PAX occupe
   1,3x à 2x plus d'espace brut que heap, mais une fois compressé il retombe
   autour de la taille de heap, voire en dessous. La disposition columnaire
   range des valeurs de même type côte à côte, ce qui compense la surcharge
   brute. C'est utile pour les sauvegardes, l'archivage et le stockage froid, et
   c'est la précondition d'une compression embarquée (PAX_FLAG_COMPRESSED existe
   mais n'est pas implémenté).

   Taille brute et vitesse de lecture sont défavorables à PAX en l'état :
     - 32 octets de métadonnées par version, contre 24 pour un HeapTupleHeader ;
     - le slot d'une valeur NULL est quand même réservé : le bitmap ne coûte
       rien mais ne fait pas économiser le slot ;
     - une version morte reste sur la page, marquée inutilisée, et seules ses
       charges utiles sont compactées par VACUUM.

   La projection partielle, argument central du stockage columnaire, n'est pas
   encore exploitée : le scan ne connaît pas la liste des attributs demandés et
   reconstruit toutes les régions, puis matérialise la tupline entière. C'est ce
   qui explique l'écart de 7x à 9x sur la lecture projetée — l'inverse exact de
   ce qu'on attendrait d'un stockage columnaire. Voir analyse1.md, sections 8 et 12.
NOTE

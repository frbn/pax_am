#!/usr/bin/env python3
"""
Inspecte une page PAX reelle et emet un schema SVG fidele aux octets lus.

Deux sorties possibles :

  defaut   le dump d'une page, zone par zone, avec les valeurs lues ;
  --schema le schema abstrait du format (zones + ce que fait un INSERT),
           ancre sur la meme page reelle.

Les structures decodees correspondent exactement a pax_am.c (format v6) :

  PageHeaderData (24 o)          storage/bufpage.h
    pd_lsn 8, pd_checksum 2, pd_flags 2, pd_lower 2,
    pd_upper 2, pd_special 2, pd_pagesize_version 2, pd_prune_xid 4

  PaxPageHeader                     PaxPageHeaderPtr = page + 24
    n_tuples 2, meta_offset 2, free_space 2, flags 2, offsets[n_attrs] 2

  PaxTupleMetaData : 32 o par version
    xmin 4, xmax 4, cmin 4, cmax 4, t_ctid 6, flags 2, locker_mxid 4, pad 4

  region par colonne : [bitmap de NULL][slots]
    slot = attlen si longueur fixe, 2 o si varlena, sans MAXALIGN
    (inchangé depuis la version 4 du format de page)

  PaxSpecialData (8 o) en fin de page
    version 2, flags 2, n_attrs 2, magic 2

Usage:
    inspect_pax_page.py <relation> <block> [-o out.svg] [--db base] [--rows N]
    inspect_pax_page.py <relation> <block> --schema -o schema.svg
"""

import argparse
import os
import struct
import subprocess
import sys

BLCKSZ = 8192
SIZEOF_PAGE_HEADER = 24
SIZEOF_PAX_SPECIAL = 8          # la VRAIE zone speciale, en FIN de page.
                                # Ne plus la confondre avec un bourrage :
                                # jusqu'en version 5 du format de page,
                                # PaxPageHeaderPtr vaudait page + 24 + 8. Ce
                                # bourrage est supprimé (version 6) : il ne
                                # satisfiait aucun alignement, 24 étant déjà
                                # multiple de 8.
SIZEOF_TUPLE_META = 32
PAX_HEADER_FIXED = 8


def maxalign(n):
    return (n + 7) & ~7


def bitmap_size(n):
    if n <= 0:
        return 0
    return maxalign((n + 7) // 8)


def slot_stride(attlen):
    """v4 : pas exact = attlen, ou 2 octets pour un varlena (OffsetNumber)."""
    return attlen if attlen and attlen > 0 else 2


def run_sql(psql, db, sql):
    r = subprocess.run([psql, "-X", "-At", "-v", "ON_ERROR_STOP=1", "-d", db, "-c", sql],
                       check=True, capture_output=True, text=True)
    return r.stdout.strip()


def fetch_page(psql, db, relation, blk):
    return bytes.fromhex(run_sql(psql, db,
                                 f"SELECT encode(get_raw_page('{relation}', {blk}), 'hex')"))


def fetch_columns(psql, db, relation):
    rows = run_sql(psql, db,
                   f"SELECT attnum, attname, attlen FROM pg_attribute "
                   f"WHERE attrelid = '{relation}'::regclass AND attnum > 0 "
                   f"AND NOT attisdropped ORDER BY attnum").splitlines()
    return [dict(attnum=int(a), name=b, attlen=int(c)) for a, b, c in
            (r.split("|") for r in rows)]


def parse_page(page):
    p = {}
    (p["pd_lsn"], p["pd_checksum"], p["pd_flags"], p["pd_lower"], p["pd_upper"],
     p["pd_special"], p["pd_pagesize_version"], p["pd_prune_xid"]) = \
        struct.unpack_from("<QHHHHHHI", page, 0)

    base = SIZEOF_PAGE_HEADER
    p["pax_header_off"] = base
    p["n_tuples"], p["meta_offset"], p["free_space"], p["pax_flags"] = \
        struct.unpack_from("<HHHH", page, base)

    p["special_version"], p["special_flags"], p["n_attrs"], p["special_magic"] = \
        struct.unpack_from("<HHHH", page, p["pd_special"])

    p["offsets"] = list(struct.unpack_from("<%dH" % p["n_attrs"], page, base + PAX_HEADER_FIXED))
    p["pax_header_size"] = maxalign(PAX_HEADER_FIXED + p["n_attrs"] * 2)

    p["meta"] = []
    for i in range(p["n_tuples"]):
        o = p["meta_offset"] + i * SIZEOF_TUPLE_META
        xmin, xmax, cmin, cmax = struct.unpack_from("<IIII", page, o)
        # ItemPointerData = { BlockNumberData bi (4 o) ; OffsetNumber bo (2 o) }
        blk, = struct.unpack_from("<I", page, o + 16)
        off, = struct.unpack_from("<H", page, o + 20)
        flags, = struct.unpack_from("<H", page, o + 22)
        mxid, = struct.unpack_from("<I", page, o + 24)
        pad, = struct.unpack_from("<I", page, o + 28)
        p["meta"].append(dict(xmin=xmin, xmax=xmax, cmin=cmin, cmax=cmax,
                              blk=blk, off=off, flags=flags, mxid=mxid, pad=pad))

    p["regions"] = []
    for i in range(p["n_attrs"]):
        s = p["offsets"][i]
        if s in (0xFFFF, 0):
            p["regions"].append(None)
            continue
        if i + 1 < p["n_attrs"] and p["offsets"][i + 1] not in (0xFFFF, 0):
            e = p["offsets"][i + 1]
        else:
            e = p["pd_lower"]
        p["regions"].append(dict(attnum=i, start=s, end=e, size=e - s))

    p["meta_end"] = p["regions"][0]["start"] if p["regions"] and p["regions"][0] else p["pd_lower"]
    return p


def null_count(page, p, reg, attlen):
    bmp = reg["size"] - p["n_tuples"] * slot_stride(attlen)
    if bmp <= 0:
        return 0
    return sum(1 for t in range(p["n_tuples"]) if is_null(page, reg, t))


def decode_fixed(page, off, attlen):
    if attlen == 2:
        return struct.unpack_from("<h", page, off)[0]
    if attlen == 4:
        return struct.unpack_from("<i", page, off)[0]
    if attlen == 8:
        return struct.unpack_from("<q", page, off)[0]
    return None


def is_null(page, reg, t):
    """
    Lit le bit de NULL du bitmap de la region.

    pax_am.c utilise att_isnull() de tupmacs.h : convention PostgreSQL,
    bit a 0 = NULL, bit a 1 = valeur presente.
    """
    return not (page[reg["start"] + t // 8] & (1 << (t % 8)))


def decode_varlena(page, off):
    """
    Decode un varlena selon varatt.h (little-endian) :

        VARATT_IS_1B  : (header & 0x01) == 0x01, taille = (header >> 1) & 0x7F
        VARATT_IS_4B_C: (header & 0x03) == 0x02  -> PGLZ, non decode ici
        sinon 4B non compresse, taille = (hdr32 >> 2) & 0x3FFFFFFF

    Les valeurs alloquees par pax_alloc_payload() a un offset MAXALigne
    utilisent la forme 4 octets.
    """
    if off <= 0 or off >= len(page):
        return None, 0
    b0 = page[off]

    if b0 & 0x01:                      # en-tete 1 octet
        size = (b0 >> 1) & 0x7F
        start = off + 1
    else:                              # en-tete 4 octets
        if (b0 & 0x03) == 0x02:        # PGLZ : on ne decompresse pas
            hdr = struct.unpack_from("<I", page, off)[0]
            return "<compressé PGLZ>", maxalign((hdr >> 2) & 0x3FFFFFFF)
        hdr = struct.unpack_from("<I", page, off)[0]
        size = (hdr >> 2) & 0x3FFFFFFF
        start = off + 4

    if size <= 0 or start > len(page) or off + size > len(page):
        return None, 0
    try:
        text = page[start:off + size].decode("utf-8")
    except UnicodeDecodeError:
        return None, maxalign(size)
    return sanitize(text), maxalign(size)


def sanitize(s):
    """Remplace tout caractere non imprimable : la sortie doit rester du XML valide."""
    return "".join(ch if (ch.isprintable() or ch == " ") else "." for ch in s)


def collect_varlena(page, p, colinfo, limit=4):
    """Récupère les valeurs varlena réellement référencées par les slots."""
    seen = {}
    for c in colinfo:
        if c["attlen"] > 0 or c["reg"] is None:
            continue
        reg, stride, bmp = c["reg"], c["stride"], c["bmp"]
        for t in range(p["n_tuples"]):
            if is_null(page, reg, t):
                continue
            off, = struct.unpack_from("<H", page, reg["start"] + bmp + t * stride)
            if off in seen:
                continue
            txt, aligned = decode_varlena(page, off)
            seen[off] = (txt, aligned)
    # pax_alloc_payload empile vers pd_upper : l'offset le plus eleve est le plus ancien
    items = sorted(seen.items(), key=lambda kv: -kv[0])
    return items[:limit], len(seen)


def page_segments(p):
    """
    Segmentation exacte de la page, du haut vers le bas.

    Utilisee par verify_layout() et par le mode --schema, pour que le schema
    dessine les memes octets que le controle verifie.
    """
    segs, cur = [], 0

    def add(label, size, extra=None):
        nonlocal cur
        segs.append((label, cur, cur + size, extra))
        cur += size

    add("PageHeaderData", SIZEOF_PAGE_HEADER)
    add("PaxPageHeader", p["pax_header_size"])
    add("meta", p["meta_end"] - p["meta_offset"])
    for r in p["regions"]:
        add(f"col{r['attnum']}", r["size"])
    add("libre", p["pd_upper"] - p["pd_lower"])
    add("varlena", (BLCKSZ - SIZEOF_PAX_SPECIAL) - p["pd_upper"])
    add("PaxSpecialData", SIZEOF_PAX_SPECIAL)
    return segs


def insert_delta(p, cols):
    """
    Ce que ferait UN INSERT sur cette page, calcule comme pax_tuple_insert().

    Pour chaque colonne la region gagne exactement
        pax_bitmap_size(tupno+1) - pax_bitmap_size(tupno)  +  pas
    octets, inseres a offsets[i] : ce qui se trouve au-dela est memmove, et
    tout offsets[j] >= at est augmente de la meme longueur.  La metadonnee
    gagne 32 octets, les valeurs hors ligne sont prises au-dessus de pd_upper.

    v4 ne pose aucune contrainte d'alignement (les lectures passent par
    memcpy), donc les deltas sont calcules au byte pres.
    """
    n = p["n_tuples"]
    bmp_grow = bitmap_size(n + 1) - bitmap_size(n)
    out = dict(n=n, bmp_grow=bmp_grow, meta= SIZEOF_TUPLE_META, cols=[])
    for i, c in enumerate(cols):
        r = p["regions"][i]
        stride = slot_stride(c["attlen"])
        out["cols"].append(dict(name=c["name"], attlen=c["attlen"],
                                start=(r["start"] if r else None),
                                size=(r["size"] if r else 0),
                                stride=stride, delta=bmp_grow + stride))
    out["total"] = (out["meta"] + sum(c["delta"] for c in out["cols"]))
    return out


def verify_layout(page, p, cols):
    """
    Controle de coherence du layout decode.

    Verifie que les regions sont contigues, que leur somme fait exactement
    BLCKSZ, et que chaque taille correspond a la formule du code
    (bitmap de NULL + n_tuples x pas).  Renvoie la liste des anomalies.
    """
    problems = []
    segs = page_segments(p)
    cur = segs[-1][2]

    for i in range(len(segs) - 1):
        if segs[i][2] != segs[i + 1][1]:
            problems.append((segs[i][0], f"trou avant {segs[i + 1][0]}"))
    if cur != BLCKSZ:
        problems.append(("total", f"{cur} != {BLCKSZ}"))

    n = p["n_tuples"]
    exp = n * SIZEOF_TUPLE_META
    if p["meta_end"] - p["meta_offset"] != exp:
        problems.append(("meta", f"{p['meta_end'] - p['meta_offset']} != {n}x32={exp}"))

    # v4 : le span peut depasser la taille utile car chaque region s'ouvre
    # alignee sur 8 octets ; il doit simplement pouvoir la contenir.
    for i, r in enumerate(p["regions"]):
        if r is None or i >= len(cols):
            continue
        atlen = cols[i]["attlen"]
        stride = slot_stride(atlen)
        want = bitmap_size(n) + n * stride
        if r["size"] < want:
            problems.append((f"col{i}", f"span {r['size']} < besoin {want}"))
        # v4 ne pose plus aucune contrainte d'alignement : les lectures
        # passent par memcpy cote AM. Le span peut donc depasser la taille
        # utile si une region a ete ouverte avant qu'une autre ne pousse.

    return problems


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def emit_schema(p, cols, relation, block):
    """
    Schema abstrait du format v6 : le squelette de la page et ce qu'un INSERT
    y deplace.

    Ce que les deux dumps ne peuvent pas montrer. Un dump est un etat fige ; ce
    qui definit v4, c'est le mouvement. Le schema est donc trace autour de la
    segmentation reelle (page_segments, la meme que --verify) et de
    l'arithmetique reelle d'un INSERT (insert_delta, calculee comme
    pax_tuple_insert), ancrees sur une page reelle dont on donne la provenance.
    """
    segs = page_segments(p)
    d = insert_delta(p, cols)

    # Insertion simulee colonne par colonne, comme pax_tuple_insert() :
    # pd_lower croit au fur et a mesure, donc le volume memmove n'est pas la
    # somme de (pd_lower_initial - start) mais l'integrale de l'etat courant.
    cur = p["pd_lower"]
    moved = 0
    for c in d["cols"]:
        if c["start"] is not None:
            moved += cur - c["start"]
        cur += c["delta"]
    d["moved"] = moved
    d["pd_lower_after"] = cur

    O = []
    W, H0 = 1200, 1560
    PX, PW = 40, 500
    IN = PX + 12
    IW = PW - 24
    tx = 588

    H = H0   # recalcule a la fin sur le contenu reellement trace
    O.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" font-family="\'DejaVu Sans\',Helvetica,Arial,sans-serif">')
    O.append('<title>Schéma du format de page PAX v6 — ce que fait un INSERT</title>')
    O.append('<style>'
             '.t{font-size:20px;font-weight:600;fill:#1b2733}'
             '.s{font-size:12px;fill:#5b6b7c}'
             '.h{font-size:14px;font-weight:600;fill:#1b2733}'
             '.b{font-size:11.5px;font-weight:600;fill:#1b2733}'
             '.l{font-size:11.5px;fill:#1b2733}'
             '.m{font-family:\'DejaVu Sans Mono\',Menlo,monospace}'
             '.z{font-size:10px;fill:#45566a}'
             '.zr{font-size:10px;fill:#45566a;text-anchor:end}'
             '.f{stroke:#33475b;stroke-width:2}'
             '</style>')
    O.append(f'<rect width="{W}" height="{H}" fill="#fff"/>')

    maxy = [0]

    def txt(x, y, s, cls="z", anchor=None):
        a = f' text-anchor="{anchor}"' if anchor else ""
        maxy[0] = max(maxy[0], y)
        O.append(f'<text class="{cls}" x="{x}" y="{y}"{a}>{s}</text>')

    y = 40
    txt(24, y, 'Schéma du format de page PAX v6 — une région par colonne', 't')
    y += 20
    txt(24, y, 'Zones tracées à leur taille réelle ; arithmétique de l\'INSERT '
               'prise dans pax_tuple_insert().', 's')
    y += 16
    txt(24, y, f'Ancré sur le bloc {block} de {esc(relation)} : '
               f'{d["n"]} versions, {len(cols)} colonnes.', 's')

    # ---------------- squelette -------------------------------------------
    y += 26
    txt(PX, y, 'la page, du haut vers le bas', 'b')
    y += 8

    fills = {"PageHeaderData": "#dbe4ee", "PaxPageHeader": "#c9dcea",
             "meta": "#e4ecdc",
             "libre": "#fdf6e3", "varlena": "#f3e6de",
             "PaxSpecialData": "#dbe4ee"}

    def h_for(size):
        return max(15.0, min(74.0, size / 8192.0 * 470))

    top_of = {}
    for label, lo, hi, _ in segs:
        size = hi - lo
        top_of[label] = y
        h = h_for(size)
        fill = "#dfe9f2" if label.startswith("col") else fills.get(label, "#eee")
        dsh = ' stroke-dasharray="4 2"' if label == "libre" else ""
        O.append(f'<rect class="f" x="{PX}" y="{y}" width="{PW}" height="{h}" '
                 f'fill="{fill}"{dsh}/>')
        key = f"col{label[3:]}" if label.startswith("col") else label
        if size <= 0:
            txt(IN, y + 11, f'{key} — vide', 'z')
        elif h < 26:
            txt(IN, y + h - 5, f'{key} · {size} o', 'z')
        else:
            txt(IN, y + 15, key, 'b')
            txt(PX + PW - 10, y + 15, f'{size} o', 'zr', 'end')
            txt(PX + PW - 10, y + 28, f'[{lo}, {hi})', 'zr', 'end')
        y += h

    y += 16
    txt(PX, y, 'offset 0 en haut, 8192 en bas.', 'l')
    y += 15
    txt(PX, y, 'pd_lower monte quand une région ou la métadonnée grandit ;', 'l')
    y += 15
    txt(PX, y, 'pd_upper descend quand une valeur hors ligne est allouée.', 'l')
    y += 15
    txt(PX, y, 'L\'espace libre est ce qui reste entre les deux.', 'l')
    y += 24

    # ---------------- le detail des zones ---------------------------------
    txt(PX, y, 'Ce que contient chaque zone', 'h')
    y += 20

    def zone_detail(label, lo, hi, _):
        nonlocal y
        size = hi - lo
        if label.startswith("col"):
            i = int(label[3:])
            if i >= len(cols):
                return
            c = cols[i]
            stride = slot_stride(c["attlen"])
            bmp = size - d["n"] * stride
            slack = size - (bmp + d["n"] * stride)
            txt(PX, y, f'· région {i} — {esc(c["name"])}', 'b')
            txt(PX + PW - 10, y, f'{size} o', 'zr', 'end')
            y += 15
            txt(PX + 14, y, f'bitmap {bmp} o  +  {d["n"]} slots × {stride} o  = '
                            f'{bmp + d["n"] * stride} o', 'm')
            if slack:
                txt(PX + 14 + 200, y, f'+{slack} o de bourrage', 'z')
            y += 20
        elif label == "meta":
            txt(PX, y, '· zone de métadonnées', 'b')
            txt(PX + PW - 10, y, f'{size} o', 'zr', 'end')
            y += 15
            txt(PX + 14, y, f'{d["n"]} × {SIZEOF_TUPLE_META} o = '
                            f'{d["n"] * SIZEOF_TUPLE_META} o', 'm')
            txt(PX + 14 + 130, y, 'une PaxTupleMetaData par version', 'z')
            y += 20
        elif label == "PaxPageHeader":
            txt(PX, y, '· PaxPageHeader', 'b')
            txt(PX + PW - 10, y, f'{size} o', 'zr', 'end')
            y += 15
            txt(PX + 14, y, f'n_tuples, meta_offset, free_space, flags,', 'm')
            txt(PX + 14, y + 13, f'puis offsets[{len(cols)}] — un offset par colonne', 'm')
            y += 33
        elif label == "varlena":
            txt(PX, y, '· valeurs hors ligne', 'b')
            txt(PX + PW - 10, y, f'{size} o', 'zr', 'end')
            y += 15
            txt(PX + 14, y, 'le slot ne contient que l\'offset absolu, 2 o', 'm')
            y += 20

    for seg in segs:
        zone_detail(*seg)

    y += 14
    txt(PX, y, 'Ce que fait UN INSERT', 'h')
    y += 20
    for c in d["cols"]:
        txt(PX, y, f'· {esc(c["name"]):<10} +{c["delta"]} o', 'm')
        txt(PX + 190, y, f'= bitmap {d["bmp_grow"]} o + slot {c["stride"]} o', 'z')
        y += 17
    txt(PX, y, f'· {"métadonnées":<10} +{d["meta"]} o', 'm')
    y += 17
    txt(PX, y, f'  pd_lower : {p["pd_lower"]} → {d["pd_lower_after"]}', 'm')
    y += 17
    txt(PX, y, "  valeurs hors ligne : pd_upper descend d'autant", 'l')
    y += 26

    txt(PX, y, 'Le coût, mesuré sur cette page', 'h')
    y += 20
    txt(PX, y, f'· {d["moved"]} octets memmovés par INSERT — une fois par', 'l')
    y += 16
    txt(PX + 14, y, 'colonne, chaque pax_insert_bytes() décalant', 'l')
    y += 16
    txt(PX + 14, y, 'tout ce qui suit la région qu\'il agrandit', 'l')
    y += 16
    txt(PX, y, f'· {len(d["cols"])} memmove et {len(d["cols"])} passes sur '
               f'offsets[]', 'l')
    y += 16
    txt(PX + 14, y, 'par ligne insérée', 'l')
    y += 16
    pct = round(100.0 * d["moved"] / BLCKSZ)
    txt(PX, y, f'· soit {pct} % d\'une page déplacée pour écrire', 'l')
    y += 16
    txt(PX + 14, y, 'une seule ligne', 'l')
    y += 16
    txt(PX, y, '· un INSERT coûte donc O(lignes déjà présentes)', 'l')
    y += 16
    txt(PX + 14, y, '× colonnes — pas O(1) comme un tuple heap', 'l')
    y += 26

    txt(PX, y, 'Pourquoi déplacer ne casse rien', 'h')
    y += 20
    for line in [
        'Un t_ctid PAX ne contient pas un offset d\'octet : il contient',
        '(bloc, index de version). Décaler une région ne déplace donc aucun',
        'lien entrant — un lien vers la version 7 pointe toujours vers',
        'l\'index 7, où qu\'elle soit devenue. Le seul ascenseur à corriger',
        'est offsets[], et la boucle le reconstruit.',
    ]:
        txt(PX, y, line, 'l')
        y += 15
    y += 4
    for line in [
        'C\'est la contrepartie exacte de heap : là, lp_off est un offset',
        'réel et un tuple ne bouge jamais ; PAX bouge mais n\'a que des',
        'liens logiques. Laquelle des deux échange vaut mieux est la',
        'question ouverte de analyse1.md §18.',
    ]:
        txt(PX, y, line, 'l')
        y += 15

    # ---------------- colonne droite ---------------------------------------
    rx, rw = tx, W - tx - 40
    ry = 104          # sous le titre, qui occupe toute la largeur

    def panel(title, lines, fill, stroke, mono_from=0):
        """Boite de titre + lignes, hauteur calculee sur le contenu."""
        nonlocal ry
        n = len(lines)
        h = 26 + n * 15 + 12
        O.append(f'<rect x="{rx}" y="{ry}" width="{rw}" height="{h}" '
                 f'fill="{fill}" stroke="{stroke}"/>')
        txt(rx + 14, ry + 21, title, 'h')
        yy = ry + 42
        for i, line in enumerate(lines):
            txt(rx + 14, yy, line, 'm' if i >= mono_from else 'l')
            yy += 15
        ry += h + 26

    panel('Pas de slot, par type', [
        '1 o    bool, "char"                     ch',
        '2 o    text, numeric, bytea, varchar   t1 t2 t3 t4',
        '4 o    int4, real, date                 id k4 f4 d1',
        '8 o    int8, float8, timestamp          k8 f8 ts1',
        '16 o   interval, uuid                   iv1 uu',
        '',
        'Pas de MAXALIGN depuis la v4 : les lectures passent par memcpy.',
    ], '#f7f9fb', '#c3cedb')

    panel('Bitmap de NULL', [
        'Un bit par version, dimensionné par le nombre de',
        'versions — jamais par le span de la région, qui',
        'est bourré d\'alignement et donc non canonique.',
        '',
        f'sur cette page : {bitmap_size(d["n"])} o pour {d["n"]} versions',
    ], '#f7f9fb', '#c3cedb', mono_from=4)

    panel('Ce que ce schéma ne montre pas', [
        'Le balayage. pax_slot_materialize() copie toutes les',
        'colonnes de chaque ligne, sous le verrou de contenu de',
        'la page, même pour une projection d\'une seule colonne :',
        'count(*), 1 colonne et 20 colonnes coûtent la même',
        'chose. C\'est le poste de CPU n°1 de PAX, mesuré',
        '~ -60 % récupérable — et il est bloqué sur une adresse',
        'de ligne stable, que v4 ne fournit pas. Le format à',
        'chunks de la v5 le faisait, au prix de la densité.',
    ], '#fdf6e3', '#e0d2a8')

    panel('Vérifier une page', [
        'inspect_pax_page.py TBL 0 --verify -o page.svg',
        '',
        'Régions contiguës, total = 8192 o.',
        'Les dumps docs/pax-page.svg et docs/pax-page-reelle.svg',
        'montrent le même format sur des tables concrètes.',
    ], '#f7f9fb', '#c3cedb', mono_from=0)

    panel('Ce qui n\'existe plus', [
        'La v5 a remplacé offsets[] par des chaînes de chunks de',
        'taille fixe, pour que les lignes ne bougent plus. Elle a',
        'été revertie : elle coûtait de la densité de page et',
        'imposait de désactiver la compaction des charges utiles.',
        'Le code est revenu à v4 ; la piste reste dans analyse1.md',
        'sections 17 et 18.',
    ], '#eef2f6', '#c3cedb')

    # Le canevas est dimensionne sur le contenu traces, pas sur une constante
    # devinee : le mode --schema est susceptible d'etre relu sur d'autres
    # tables, avec d'autres nombres de colonnes.
    h = int(maxy[0] + 28)
    O[0] = O[0].replace(f'height="{H0}"', f'height="{h}"')
    O.append('</svg>')
    return "\n".join(O), W, h
def cid(v):
    """CommandId invalide = 0xFFFFFFFF : trop long pour la ligne, on l'abrege."""
    return "—" if v == 0xFFFFFFFF else str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("relation")
    ap.add_argument("block", type=int)
    ap.add_argument("-o", "--output", default="pax-page-reelle.svg")
    ap.add_argument("--psql", default="psql")
    ap.add_argument("--db", default=None)
    ap.add_argument("--rows", type=int, default=5, help="valeurs montrées par colonne")
    ap.add_argument("--meta-rows", type=int, default=5)
    ap.add_argument("--verify", action="store_true",
                    help="controle la coherence du layout avant de produire le SVG")
    ap.add_argument("--schema", action="store_true",
                    help="schema abstrait du format (zones + ce que fait un INSERT) "
                         "au lieu du dump d'une page")
    args = ap.parse_args()

    db = args.db or os.environ.get("PGDATABASE") or "postgres"
    page = fetch_page(args.psql, db, args.relation, args.block)
    if len(page) != BLCKSZ:
        sys.exit(f"page inattendue : {len(page)} octets")

    cols = fetch_columns(args.psql, db, args.relation)
    p = parse_page(page)

    if args.schema:
        problems = verify_layout(page, p, cols) if args.verify else []
        if problems:
            for what, why in problems:
                print(f"  ATTENTION  {what} : {why}", file=sys.stderr)
            sys.exit("layout incoherent, schema non produit")
        svg, W, H = emit_schema(p, cols, args.relation, args.block)
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(svg)
        print(f"ecrit : {args.output}")
        return

    n = p["n_tuples"]
    shown = min(args.rows, n)
    meta_shown = min(args.meta_rows, n)

    free = p["pd_upper"] - p["pd_lower"]
    varlena_lo, varlena_hi = p["pd_upper"], BLCKSZ - SIZEOF_PAX_SPECIAL

    # --- valeur de depart des slots de chaque colonne -------------------------
    colinfo = []
    for i, c in enumerate(cols):
        reg = p["regions"][i]
        if reg is None:
            colinfo.append(dict(c, reg=None, bmp=0, stride=0, nulls=0, first=[]))
            continue
        stride = slot_stride(c["attlen"])
        bmp = reg["size"] - n * stride
        nulls = null_count(page, p, reg, c["attlen"])
        first = []
        for t in range(shown):
            # Le bitmap fait foi : un slot NULL ne doit pas être décodé,
            # son contenu n'est pas une valeur.
            if is_null(page, reg, t):
                first.append(None)
                continue
            slot = reg["start"] + bmp + t * stride
            if c["attlen"] > 0:
                first.append(decode_fixed(page, slot, c["attlen"]))
            else:
                off, = struct.unpack_from("<H", page, slot)
                txt, _ = decode_varlena(page, off)
                first.append(txt)
        colinfo.append(dict(c, reg=reg, bmp=bmp, stride=stride, nulls=nulls, first=first))

    O = []
    W, H = 1000, 1180
    O.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" font-family="\'DejaVu Sans\',Helvetica,Arial,sans-serif">')
    O.append(f'<title>Page PAX v6 réelle — {esc(args.relation)} bloc {args.block}</title>')
    O.append('<style>'
             '.t{font-size:19px;font-weight:600;fill:#1b2733}'
             '.s{font-size:12px;fill:#5b6b7c}'
             '.b{font-size:12.5px;font-weight:600;fill:#1b2733}'
             '.l{font-size:11.5px;fill:#1b2733}'
             '.m{font-family:\'DejaVu Sans Mono\',Menlo,monospace}'
             '.z{font-size:9.5px;fill:#45566a}'
             '.zr{font-size:9.5px;fill:#45566a;text-anchor:end}'
             '.n{font-size:11px;fill:#45566a}'
             '.nb{font-size:12px;fill:#1b2733;font-weight:600}'
             '.f{stroke:#33475b;stroke-width:2}'
             '</style>')

    O.append(f'<rect width="{W}" height="{H}" fill="#fff"/>')
    O.append(f'<text class="t" x="24" y="36">Page PAX v6 réelle — bloc {args.block} de '
             f'{esc(args.relation)}</text>')
    O.append(f'<text class="s" x="24" y="58">Dump binaire via pageinspect.get_raw_page(), '
             f'décodé selon les structures de pax_am.c — toutes les tailles ci-dessous sont '
             f'lues, pas estimées.</text>')

    PX, PW = 120, 470          # colonne de la page
    IN = PX + 14               # texte interne
    IW = PW - 28
    y = 82

    def box(h, fill, dash=None):
        nonlocal y
        d = f' stroke-dasharray="{dash}"' if dash else ""
        O.append(f'<rect class="f" x="{PX}" y="{y}" width="{PW}" height="{h}" fill="{fill}"{d}/>')

    def txt(x, yy, s, cls="z", anchor=None):
        a = f' text-anchor="{anchor}"' if anchor else ""
        O.append(f'<text class="{cls}" x="{x}" y="{yy}"{a}>{s}</text>')

    # --- PageHeaderData -----------------------------------------------------
    h = 62
    box(h, "#dbe4ee")
    txt(IN, y + 18, "PageHeaderData", "b")
    txt(PX + PW - 14, y + 18, f"24 o — offset 0", "zr", "end")
    txt(IN, y + 34, f"pd_lsn 0x{p['pd_lsn']:016X} · pd_checksum {p['pd_checksum']} · "
                    f"pd_flags {p['pd_flags']}", "z m")
    txt(IN, y + 48, f"pd_lower <tspan font-weight='600'>{p['pd_lower']}</tspan> · "
                    f"pd_upper <tspan font-weight='600'>{p['pd_upper']}</tspan> · "
                    f"pd_special {p['pd_special']}", "z m")
    y += h

    # --- PaxPageHeader ------------------------------------------------------
    h = 58
    box(h, "#c9dcea")
    txt(IN, y + 18, "PaxPageHeader", "b")
    txt(PX + PW - 14, y + 18, f"{p['pax_header_size']} o — offset {p['pax_header_off']}", "zr", "end")
    txt(IN, y + 33, f"n_tuples <tspan font-weight='600'>{n}</tspan> · "
                    f"meta_offset <tspan font-weight='600'>{p['meta_offset']}</tspan> · "
                    f"free_space {p['free_space']} · flags 0x{p['pax_flags']:04X}", "z m")
    offs = "  ".join(f"offsets[{i}]={o}" for i, o in enumerate(p["offsets"]))
    txt(IN, y + 47, offs, "z m")
    y += h

    # --- metadonnees --------------------------------------------------------
    meta_sz = p["meta_end"] - p["meta_offset"]
    h = 40 + meta_shown * 16 + (14 if n > meta_shown else 4)
    box(h, "#f6cdd2")
    txt(IN, y + 18, "Région de métadonnées", "b")
    txt(PX + PW - 14, y + 18, f"{n} × 32 = {meta_sz} o", "zr", "end")
    txt(IN, y + 32, f"offsets [0..{meta_sz - 1}] — une PaxTupleMetaData par version", "z m")
    yy = y + 40
    for t in range(meta_shown):
        m = p["meta"][t]
        O.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#f0b6bd" '
                 f'stroke="#cf8891" stroke-width="1"/>')
        leaf = (m["blk"] == args.block and m["off"] == t + 1)
        nxt = "→ soi-même" if leaf else f"→ ({m['blk']},{m['off']})"
        txt(IN + 6, yy + 11,
            f"v{t}  xmin {m['xmin']}  xmax {m['xmax']}  cmin {m['cmin']}  "
            f"cmax {cid(m['cmax'])}  mxid {m['mxid']}  t_ctid {nxt}", "z m")
        yy += 16
    if n > meta_shown:
        txt(IN + 6, yy + 4, f"… {n - meta_shown} versions supplémentaires", "z")
    y += h

    # --- regions de colonnes ------------------------------------------------
    for i, c in enumerate(colinfo):
        reg = c["reg"]
        if reg is None:
            continue
        slots = c["stride"] * n
        h = 58 + (2 if shown > 1 else 1) * 15
        box(h, "#cfe3d3" if c["attlen"] > 0 else "#d9d6ea")
        txt(IN, y + 18, f"Région colonne {i} — {esc(c['name'])} "
                        f"({esc('int' + str(c['attlen'])) if c['attlen'] > 0 else 'varlena'})", "b")
        txt(PX + PW - 14, y + 18, f"[{reg['start']}, {reg['end']}) = {reg['size']} o", "zr", "end")
        txt(IN, y + 33, f"bitmap de NULL : {c['bmp']} o   +   {n} slots × "
                        f"{c['stride']} o = {slots} o   →   {c['nulls']} NULL", "z m")
        yy = y + 38
        if c["attlen"] > 0:
            O.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#bcd9c4" '
                     f'stroke="#7fae8b" stroke-width="1"/>')
            txt(IN + 6, yy + 11, f"slots de {c['stride']} o (valeur {c['attlen']} o + align)",
                "z m")
            yy += 15
        else:
            O.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#c6c2dd" '
                     f'stroke="#8d86b5" stroke-width="1"/>')
            txt(IN + 6, yy + 11, "slots de 2 o = offset ABSOLU de la valeur en haut de page", "z m")
            yy += 15
        vals = ", ".join("NULL" if v is None else str(v) for v in c["first"])
        O.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#ffffff" '
                 f'stroke="#7fae8b" stroke-width="1" stroke-dasharray="3 2"/>')
        txt(IN + 6, yy + 11, f"premières valeurs : {esc(vals)}" +
            (f"   … ({n - shown} de plus)" if n > shown else ""), "z m")
        y += h

    # --- espace libre -------------------------------------------------------
    h = 52
    box(h, "#f4f6f8", "5 3")
    txt(IN, y + 20, "Espace libre", "l")
    txt(PX + PW - 14, y + 20, f"[{p['pd_lower']}, {p['pd_upper']}) = {free} o", "zr", "end")
    txt(IN, y + 37, f"page à {100.0 * (p['pd_lower'] - SIZEOF_PAGE_HEADER) / BLCKSZ:.1f} % "
                    f"occupée — first-fit n'a plus de place pour une ligne", "z")
    y += h

    # --- varlena ------------------------------------------------------------
    vitems, vtotal = collect_varlena(page, p, colinfo)
    h = 40 + max(1, len(vitems)) * 16
    box(h, "#e4ddf0")
    txt(IN, y + 18, "Valeurs varlena", "b")
    txt(PX + PW - 14, y + 18, f"[{varlena_lo}, {varlena_hi}) = {varlena_hi - varlena_lo} o", "zr", "end")
    txt(IN, y + 32, f"pd_upper descend à chaque allocation — {vtotal} valeurs référencées",
        "z m")
    yy = y + 38
    for off, (text, aligned) in vitems:
        O.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#d3cbe6" '
                 f'stroke="#8d86b5" stroke-width="1"/>')
        label = f"@{off}  « {esc(text) if text is not None else '?'} »  varlena {aligned} o"
        txt(IN + 6, yy + 11, label, "z m")
        yy += 16
    y += h

    # --- special ------------------------------------------------------------
    h = 48
    box(h, "#f0dcef")
    txt(IN, y + 19, "PaxSpecialData", "b")
    txt(PX + PW - 14, y + 19, f"8 o — offset {p['pd_special']}", "zr", "end")
    txt(IN, y + 35, f"version {p['special_version']} · magic 0x{p['special_magic']:04X} · "
                    f"n_attrs {p['n_attrs']} · flags 0x{p['special_flags']:04X}", "z m")
    y += h

    O.append('</svg>')

    if args.verify:
        problems = verify_layout(page, p, cols)
        for label, msg in problems:
            print(f"[{label}] {msg}", file=sys.stderr)
        if problems:
            sys.exit("layout incoherent")
        print("layout vérifié : régions contiguës, total = 8192 o", file=sys.stderr)

    out = "\n".join(O)
    if args.output == "-":
        print(out)
    else:
        with open(args.output, "w") as f:
            f.write(out + "\n")
        print(f"écrit : {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()

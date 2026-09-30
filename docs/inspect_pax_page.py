#!/usr/bin/env python3
"""
Inspecte une page PAX reelle et emet un schema SVG fidele aux octets lus.

Les structures decodees correspondent exactement a pax_am.c (format v3) :

  PageHeaderData (24 o)          storage/bufpage.h
    pd_lsn 8, pd_checksum 2, pd_flags 2, pd_lower 2,
    pd_upper 2, pd_special 2, pd_pagesize_version 2, pd_prune_xid 4

  8 o de padding d'alignement
    PaxPageHeaderPtr = page + SizeOfPageHeaderData + SizeOfPaxSpecialData

  PaxPageHeader
    n_tuples 2, meta_offset 2, free_space 2, flags 2, offsets[n_attrs] 2

  PaxTupleMetaData : 32 o par version
    xmin 4, xmax 4, cmin 4, cmax 4, t_ctid 6, flags 2, locker_mxid 4, pad 4

  region par colonne : [bitmap de NULL][slots]
    v4 : slot = attlen si longueur fixe, 2 o si varlena

  PaxSpecialData (8 o) en fin de page
    version 2, flags 2, n_attrs 2, magic 2

Usage:
    inspect_pax_page.py <relation> <block> [-o out.svg] [--db base] [--rows N]
"""

import argparse
import os
import struct
import subprocess
import sys

BLCKSZ = 8192
SIZEOF_PAGE_HEADER = 24
SIZEOF_PAX_SPECIAL = 8
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

    base = SIZEOF_PAGE_HEADER + SIZEOF_PAX_SPECIAL
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


def verify_layout(page, p, cols):
    """
    Controle de coherence du layout decode.

    Verifie que les regions sont contigues, que leur somme fait exactement
    BLCKSZ, et que chaque taille correspond a la formule du code
    (bitmap de NULL + n_tuples x pas).  Renvoie la liste des anomalies.
    """
    problems = []
    segs, cur = [], 0

    def add(label, size):
        nonlocal cur
        segs.append((label, cur, cur + size))
        cur += size

    add("PageHeaderData", SIZEOF_PAGE_HEADER)
    add("padding", SIZEOF_PAX_SPECIAL)
    add("PaxPageHeader", p["pax_header_size"])
    add("meta", p["meta_end"] - p["meta_offset"])
    for r in p["regions"]:
        add(f"col{r['attnum']}", r["size"])
    add("libre", p["pd_upper"] - p["pd_lower"])
    add("varlena", (BLCKSZ - SIZEOF_PAX_SPECIAL) - p["pd_upper"])
    add("PaxSpecialData", SIZEOF_PAX_SPECIAL)

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
    args = ap.parse_args()

    db = args.db or os.environ.get("PGDATABASE") or "postgres"
    page = fetch_page(args.psql, db, args.relation, args.block)
    if len(page) != BLCKSZ:
        sys.exit(f"page inattendue : {len(page)} octets")

    cols = fetch_columns(args.psql, db, args.relation)
    p = parse_page(page)

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
    O.append(f'<title>Page PAX v3 réelle — {esc(args.relation)} bloc {args.block}</title>')
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
    O.append(f'<text class="t" x="24" y="36">Page PAX v3 réelle — bloc {args.block} de '
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

    # --- padding ------------------------------------------------------------
    h = 20
    box(h, "#eef2f6", "4 2")
    txt(IN, y + 14, "padding d'alignement — PaxPageHeaderPtr = page + 24 + 8", "z")
    txt(PX + PW - 14, y + 14, f"8 o — offset {SIZEOF_PAGE_HEADER}", "zr", "end")
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

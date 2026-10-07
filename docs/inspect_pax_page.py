#!/usr/bin/env python3
"""
Inspecte une page PAX reelle et emet un schema SVG fidele aux octets lus.

Les structures decodees correspondent exactement a pax_am.c (format v5) :

  PageHeaderData (24 o)          storage/bufpage.h
    pd_lsn 8, pd_checksum 2, pd_flags 2, pd_lower 2,
    pd_upper 2, pd_special 2, pd_pagesize_version 2, pd_prune_xid 4

  8 o de padding d'alignement
    PaxPageHeaderPtr = page + SizeOfPageHeaderData + SizeOfPaxSpecialData

  PaxPageHeader
    n_tuples 2, meta_offset 2, free_space 2, flags 2,
    chunk_floor 2, chunk_head[2 x n_attrs] 2

  PaxTupleMetaData : 32 o par version
    xmin 4, xmax 4, cmin 4, cmax 4, t_ctid 6, flags 2, locker_mxid 4, pad 4

  Chunks, dans l'arene au-dessus de pd_upper, un par tranche de
  PAX_CHUNK_MAX_ROWS versions :
    PaxChunkHdr : next_chunk 2, n_rows 2   (8 o avec l'alignement)
    puis bitmap de NULL (toujours dimensionne pour PAX_CHUNK_MAX_ROWS)
    puis PAX_CHUNK_MAX_ROWS slots

  Un chunk alloue ne bouge plus : c'est ce qui donne a une ligne PAX une
  adresse stable.

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
SizeOfChunkHdr = 8          # MAXALIGN(sizeof(PaxChunkHdr))
SIZEOF_PAX_SPECIAL = 8
SIZEOF_TUPLE_META = 32
PAX_HEADER_FIXED = 10

# Doit etre maintenu en accord avec PAX_CHUNK_MAX_ROWS de pax_am.c.
PAX_CHUNK_MAX_ROWS = 32


def maxalign(n):
    return (n + 7) & ~7


def bitmap_size(n):
    if n <= 0:
        return 0
    return maxalign((n + 7) // 8)


def slot_stride(attlen):
    """Pas exact du slot = attlen, ou 2 octets pour un varlena (OffsetNumber)."""
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
                   f"SELECT attnum, attname, attlen, format_type(atttypid, atttypmod) "
                   f"FROM pg_attribute "
                   f"WHERE attrelid = '{relation}'::regclass AND attnum > 0 "
                   f"AND NOT attisdropped ORDER BY attnum").splitlines()
    # attnum est 1-based dans le catalogue, et les chaines de chunks comme les
    # slots sont indexes par la position de la colonne : on rebascule donc une
    # fois pour toutes, ici.
    return [dict(attnum=int(a) - 1, name=b, attlen=int(c), typ=d)
            for a, b, c, d in (r.split("|") for r in rows)]


def parse_page(page, strides=None):
    """
    strides : pas de slot par colonne (attlen si > 0, sinon 2). Sans lui, la
    taille d'un chunk reste inconnue, car elle ne depend que du pas.
    """
    p = {}
    (p["pd_lsn"], p["pd_checksum"], p["pd_flags"], p["pd_lower"], p["pd_upper"],
     p["pd_special"], p["pd_pagesize_version"], p["pd_prune_xid"]) = \
        struct.unpack_from("<QHHHHHHI", page, 0)

    base = SIZEOF_PAGE_HEADER + SIZEOF_PAX_SPECIAL
    p["pax_header_off"] = base
    p["n_tuples"], p["meta_offset"], p["free_space"], p["pax_flags"], \
        p["chunk_floor"] = struct.unpack_from("<HHHHH", page, base)

    p["special_version"], p["special_flags"], p["n_attrs"], p["special_magic"] = \
        struct.unpack_from("<HHHH", page, p["pd_special"])

    n = p["n_attrs"] * 2
    p["chunk_head"] = list(struct.unpack_from("<%dH" % n, page, base + PAX_HEADER_FIXED))
    p["pax_header_size"] = maxalign(PAX_HEADER_FIXED + n * 2)
    p["meta_end"] = p["pd_lower"]

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

    # Chaines de chunks, une par colonne. La taille d'un chunk se deduit du
    # pas de sa colonne, donc il faut connaître le descripteur : on la laisse
    # nulle ici et verify_layout() la recalcule depuis cols.
    p["chains"] = []
    for i in range(p["n_attrs"]):
        off = p["chunk_head"][i * 2]
        tail = p["chunk_head"][i * 2 + 1]
        chain = []
        seen = set()
        while off not in (0xFFFF, 0):
            if off in seen:
                break                      # cycle : on s'arrete
            seen.add(off)
            nxt, nrows = struct.unpack_from("<HH", page, off)
            stride = strides[i] if strides and i < len(strides) else 0
            chain.append(dict(off=off, next=nxt, n_rows=nrows,
                              size=chunk_size(stride)))
            off = nxt
        p["chains"].append(dict(chain=chain, tail=tail))

    return p


def chunk_size(stride):
    """Taille d'un chunk : en-tete + bitmap plein + PAX_CHUNK_MAX_ROWS slots.

    Elle ne depend que du PAS de la colonne, jamais du nombre de versions
    deja presentes : c'est ce qui permet d'allouer un chunk une fois pour
    toutes, a sa taille definitive.
    """
    return 8 + bitmap_size(PAX_CHUNK_MAX_ROWS) + PAX_CHUNK_MAX_ROWS * stride


def chunk_of(p, col, tupno):
    """Renvoie (chunk, rang dans le chunk) pour une version d'une colonne."""
    chain = p["chains"][col]["chain"]
    if not chain:
        return None, 0
    ci = tupno // PAX_CHUNK_MAX_ROWS
    if ci >= len(chain):
        return None, 0
    return chain[ci], tupno % PAX_CHUNK_MAX_ROWS


def slot_offset(p, col, stride, tupno):
    """Offset absolu du slot d'une version, ou None si la colonne est absente."""
    ch, in_chunk = chunk_of(p, col, tupno)
    if ch is None:
        return None
    return ch["off"] + 8 + bitmap_size(PAX_CHUNK_MAX_ROWS) + in_chunk * stride


def is_null_row(page, p, col, tupno):
    """Bit de NULL de cette version, dans le bitmap de SON chunk."""
    ch, in_chunk = chunk_of(p, col, tupno)
    if ch is None:
        return True
    bmp = ch["off"] + 8
    return not (page[bmp + in_chunk // 8] & (1 << (in_chunk % 8)))


def null_count(page, p, col, attlen):
    if not p["chains"][col]["chain"]:
        return 0
    return sum(1 for t in range(p["n_tuples"]) if is_null_row(page, p, col, t))


def decode_fixed(page, off, attlen):
    """Valeur de longueur fixe, lue par little-endian comme le fait l'AM.

    attlen 1 est un bool (bool CharGetDatum -> 't' / 'f'), pas un entier
    affiche : le montrer en tant que tel evite de croire a un int errone.
    """
    if attlen == 1:
        return "true" if page[off] else "false"
    if attlen == 2:
        return struct.unpack_from("<h", page, off)[0]
    if attlen == 4:
        return struct.unpack_from("<i", page, off)[0]
    if attlen == 8:
        return struct.unpack_from("<q", page, off)[0]
    if attlen == 6:                     # tid : (BlockNumber, OffsetNumber)
        blk, = struct.unpack_from("<I", page, off)
        o2, = struct.unpack_from("<H", page, off + 4)
        return f"({blk},{o2})"
    return f"<{attlen} octets>"





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
    raw = page[start:off + size]
    # Un varlena n'est pas forcément du texte : numeric, bytea, uuid... Le
    # dire est plus utile qu'un point d'interrogation.
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return f"<binaire {len(raw)} o>", maxalign(size)
    if not text.isprintable():
        return f"<binaire {len(raw)} o>", maxalign(size)
    return sanitize(text), maxalign(size)


def sanitize(s):
    """Remplace tout caractere non imprimable : la sortie doit rester du XML valide."""
    return "".join(ch if (ch.isprintable() or ch == " ") else "." for ch in s)


def collect_varlena(page, p, colinfo, limit=4):
    """
    Valeurs varlena reellement referencees par les slots, {offset: (texte, taille)}.

    On passe par le CONTENU du slot, pas par une plage d'offsets : la zone des
    charges utiles et celle des chunks se melangent dans l'arene, donc deviner
    quelles valeurs existent en balayant [pd_upper, top) ramasserait aussi des
    octets de chunk.
    """
    seen = {}
    for c in colinfo:
        if c["attlen"] > 0 or not c["chain"]:
            continue
        # fetch_columns() a deja rebascule attnum en index de colonne 0-based.
        col, stride = c["attnum"], c["stride"]
        for t in range(p["n_tuples"]):
            if is_null_row(page, p, col, t):
                continue
            slot = slot_offset(p, col, stride, t)
            if slot is None:
                continue
            off, = struct.unpack_from("<H", page, slot)
            if not off or off in seen:
                continue
            txt, aligned = decode_varlena(page, off)
            seen[off] = (txt, aligned)
    # pax_alloc_payload empile vers pd_upper : l'offset le plus eleve est le plus ancien
    items = sorted(seen.items(), key=lambda kv: -kv[0])
    return (items if limit is None else items[:limit]), len(seen)


def verify_layout(page, p, cols):
    """
    Controle de coherence du layout decode.

    En version 5 il n'y a plus de regions contigues : la zone basse ne porte
    que les metadonnees, et le reste de l'arene est un melange de chunks et de
    charges utiles. On verifie donc ce qui reste verifiable :

      - la zone des metadonnees fait exactement n_tuples x 32 octets ;
      - chaque chaine de chunks se termine bien sur le maillon designe par la
        queue, et chaque maillon tient dans l'arene ;
      - n_rows ne depasse pas PAX_CHUNK_MAX_ROWS ;
      - la taille d'un chunk est bien celle de la formule du code.
    """
    problems = []
    n = p["n_tuples"]
    top = BLCKSZ - SIZEOF_PAX_SPECIAL

    if p["meta_end"] - p["meta_offset"] != n * SIZEOF_TUPLE_META:
        problems.append(("meta",
                         f"{p['meta_end'] - p['meta_offset']} != {n}x32="
                         f"{n * SIZEOF_TUPLE_META}"))
    if p["pd_lower"] > top or p["pd_upper"] > top:
        problems.append(("bornes", "pd_lower ou pd_upper au-dela de la zone speciale"))

    max_chunks = n // PAX_CHUNK_MAX_ROWS + 2
    for i, cc in enumerate(p["chains"]):
        chain = cc["chain"]
        size = chain[0]["size"] if chain else 0
        if len(chain) > max_chunks:
            problems.append((f"col{i}", f"{len(chain)} chunks > {max_chunks}"))
        want = -(-n // PAX_CHUNK_MAX_ROWS) if n else 0
        if len(chain) != want:
            problems.append((f"col{i}",
                             f"{len(chain)} chunks pour {n} versions, attendu {want}"))
        for k, ch in enumerate(chain):
            if ch["n_rows"] > PAX_CHUNK_MAX_ROWS:
                problems.append((f"col{i}.{k}",
                                 f"n_rows {ch['n_rows']} > {PAX_CHUNK_MAX_ROWS}"))
            if ch["off"] < p["pd_upper"] or ch["off"] + ch["size"] > top:  # noqa: E501
                problems.append((f"col{i}.{k}",
                                 f"chunk [{ch['off']}, {ch['off'] + ch['size']}) "
                                 f"hors arene"))
        if chain and cc["tail"] not in (0xFFFF, 0) and cc["tail"] != chain[-1]["off"]:
            problems.append((f"col{i}", "la queue ne designe pas le dernier maillon"))

    if p["chunk_floor"] != top and p["chunk_floor"] < p["pd_upper"]:
        problems.append(("chunk_floor",
                         f"{p['chunk_floor']} sous pd_upper {p['pd_upper']}"))

    return problems


def collect_arena(page, p, colinfo):
    """
    Blocs de l'arène, à leur VRAI offset, chunks et charges utiles confondus.

    C'est indispensable en v5 : les deux familles partagent la descente de
    pd_upper et s'entrelacent. Les dessiner comme deux zones contigues
    mentirait sur le format.
    """
    blocks = []
    for i, c in enumerate(colinfo):
        for k, ch in enumerate(c["chain"]):
            blocks.append(dict(kind="chunk", col=i, name=c["name"], seq=k,
                               off=ch["off"], size=ch["size"], n_rows=ch["n_rows"],
                               typ=c["attlen"]))
    for off, (text, aligned) in varlena_map(page, p, colinfo).items():
        blocks.append(dict(kind="payload", col=None, name=text, seq=0,
                           off=off, size=aligned, n_rows=None, typ=-1))
    # Adresses croissantes : c'est l'ordre de lecture de l'arene, et pd_upper
    # descend donc on lit du haut vers le bas.
    blocks.sort(key=lambda b: -b["off"])
    return blocks


def varlena_map(page, p, colinfo):
    """Toutes les valeurs varlena referenciaes. Deux valeurs peuvent partage un
    offset si deux versions portent le meme texte : d'ou un dict."""
    items, _ = collect_varlena(page, p, colinfo, limit=None)
    return dict(items)


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
    strides = [slot_stride(c["attlen"]) for c in cols]
    p = parse_page(page, strides)

    n = p["n_tuples"]
    shown = min(args.rows, n)
    meta_shown = min(args.meta_rows, n)

    free = p["pd_upper"] - p["pd_lower"]
    varlena_lo, varlena_hi = p["pd_upper"], BLCKSZ - SIZEOF_PAX_SPECIAL

    # --- premiere valeur de chaque version, via le chunk qui la contient ------
    colinfo = []
    for i, c in enumerate(cols):
        chain = p["chains"][i]["chain"] if i < len(p["chains"]) else []
        stride = strides[i]
        nulls = null_count(page, p, i, c["attlen"])
        first = []
        for t in range(shown):
            if is_null_row(page, p, i, t):
                first.append(None)
                continue
            slot = slot_offset(p, i, stride, t)
            if slot is None:
                first.append(None)
                continue
            if c["attlen"] > 0:
                v = decode_fixed(page, slot, c["attlen"])
                # float4 / float8 sont lisibles en flottant. Sans cela on
                # afficherait la reinterpretation de l'IEEE 754 en entier : le
                # nombre affiche est exact mais totalement trompeur.
                #
                # "double precision" ne contient PAS le mot "float", il faut
                # donc tester le type, pas supposer un nom.
                if c["attlen"] == 8 and c["typ"] in ("double precision", "float8"):
                    v = struct.unpack_from("<d", page, slot)[0]
                elif c["attlen"] == 4 and c["typ"] in ("real", "float4"):
                    v = struct.unpack_from("<f", page, slot)[0]
                first.append(v)
            else:
                off, = struct.unpack_from("<H", page, slot)
                txt, _ = decode_varlena(page, off)
                first.append(txt)
        total = sum(ch["size"] for ch in chain)
        colinfo.append(dict(c, chain=chain, stride=stride, nulls=nulls,
                            first=first, total=total))

    # Le dessin est produit dans B[], puis la racine SVG est creee a la fin :
    # la hauteur depend du nombre de blocs d'arene, donc elle ne peut pas etre
    # fixee a l'avance sans risquer de tronquer le schema.
    B = []
    O = []
    W = 1040
    PX0 = 120
    B.append(f'<title>Page PAX v5 réelle — {esc(args.relation)} bloc {args.block}</title>')
    B.append('<style>'
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

    B.append(f'<text class="t" x="24" y="36">Page PAX v5 réelle — bloc {args.block} de '
             f'{esc(args.relation)}</text>')
    B.append(f'<text class="s" x="24" y="58">Dump binaire via pageinspect.get_raw_page(), '
             f'décodé selon les structures de pax_am.c — toutes les tailles ci-dessous sont '
             f'lues, pas estimées.</text>')

    PX, PW = 120, 470          # colonne de la page
    IN = PX + 14               # texte interne
    IW = PW - 28
    y = 82

    def box(h, fill, dash=None):
        nonlocal y
        d = f' stroke-dasharray="{dash}"' if dash else ""
        B.append(f'<rect class="f" x="{PX}" y="{y}" width="{PW}" height="{h}" fill="{fill}"{d}/>')

    def txt(x, yy, s, cls="z", anchor=None):
        a = f' text-anchor="{anchor}"' if anchor else ""
        B.append(f'<text class="{cls}" x="{x}" y="{yy}"{a}>{s}</text>')

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
    offs = "  ".join(f"chunk_head[{i}]={o}" for i, o in
                     enumerate(p["chunk_head"][:8]))
    txt(IN, y + 47,
        f"chunk_floor {p['chunk_floor']}  " + offs +
        (f"  …" if len(p["chunk_head"]) > 8 else ""), "z m")
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
        B.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#f0b6bd" '
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

    # --- chunks, une chaîne par colonne -------------------------------------
    for i, c in enumerate(colinfo):
        if not c["chain"]:
            continue
        total, nch = c["total"], len(c["chain"])
        h = 58 + (2 if shown > 1 else 1) * 15
        box(h, "#cfe3d3" if c["attlen"] > 0 else "#d9d6ea")
        txt(IN, y + 18, f"Chaîne de chunks — colonne {i} {esc(c['name'])} "
                        f"({esc(c['typ'])})",
            "b")
        txt(PX + PW - 14, y + 18,
            f"{nch} chunk(s) = {total} o", "zr", "end")
        txt(IN, y + 33,
            f"{SizeOfChunkHdr} o d'en-tête + bitmap {bitmap_size(PAX_CHUNK_MAX_ROWS)} o + "
            f"{PAX_CHUNK_MAX_ROWS} slots × {c['stride']} o = {chunk_size(c['stride'])} o"
            f"  →  {c['nulls']} NULL sur {n} versions", "z m")
        yy = y + 38
        lbl = (f"slots de {c['stride']} o" if c["attlen"] > 0
               else "slots de 2 o = offset ABSOLU de la valeur dans l'arène")
        col = ("#bcd9c4", "#7fae8b") if c["attlen"] > 0 else ("#c6c2dd", "#8d86b5")
        B.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="{col[0]}" '
                 f'stroke="{col[1]}" stroke-width="1"/>')
        txt(IN + 6, yy + 11,
            "offsets : " + ", ".join(str(ch["off"]) for ch in c["chain"][:6]) +
            (f", … ({nch - 6} de plus)" if nch > 6 else ""), "z m")
        yy += 15
        B.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="14" fill="#ffffff" '
                 f'stroke="{col[1]}" stroke-width="1" stroke-dasharray="3 2"/>')
        txt(IN + 6, yy + 11,
            f"premières valeurs : {esc(', '.join('NULL' if v is None else str(v) for v in c['first']))}"
            + (f"   … ({n - shown} de plus)" if n > shown else ""), "z m")
        y += h

    # --- espace libre -------------------------------------------------------
    h = 52
    box(h, "#f4f6f8", "5 3")
    txt(IN, y + 20, "Espace libre", "l")
    txt(PX + PW - 14, y + 20, f"[{p['pd_lower']}, {p['pd_upper']}) = {free} o", "zr", "end")
    txt(IN, y + 37, f"zone basse : métadonnées de version, plus aucune région de colonnes — "
                    f"en v5 les slots vivent dans les chunks de l'arène", "z")
    y += h

    # --- carte physique de l'arene -------------------------------------------
    # En v5 chunks et charges utiles partagent UNE descente et s'entrelacent.
    # Les montrer en deux zones contigues dirait faux sur le format, donc on
    # les dessine a leur vrai offset, dans l'ordre des adresses croissantes.
    blocks = collect_arena(page, p, colinfo)
    MAPW = 300
    MAXROWS = 18
    shown_blocks = blocks[:MAXROWS]
    h = 34 + (len(shown_blocks) + 1) * 15 + (10 if len(blocks) > MAXROWS else 0)
    box(h, "#e4ddf0")
    txt(IN, y + 18, "Arène — chunks et charges utiles entrelacés", "b")
    txt(PX + PW - 14, y + 18,
        f"[{p['pd_upper']}, {varlena_hi}) = {varlena_hi - p['pd_upper']} o", "zr", "end")
    txt(IN, y + 32,
        f"pd_upper descend : {len([b for b in blocks if b['kind'] == 'chunk'])} chunks + "
        f"{len([b for b in blocks if b['kind'] == 'payload'])} charges utiles référencées, "
        f"adresses croissantes", "z m")
    yy = y + 40
    for b in shown_blocks:
        if b["kind"] == "chunk":
            fill, stroke = "#bcd9c4", "#7fae8b" if b["typ"] > 0 else "#c6c2dd"
            if b["typ"] <= 0:
                fill, stroke = "#c6c2dd", "#8d86b5"
            lbl = (f"@{b['off']:<5} chunk c{b['col']}.{b['seq']} — {esc(b['name'])} "
                   f"{b['size']} o, {b['n_rows']} versions")
        else:
            fill, stroke = "#d3cbe6", "#8d86b5"
            lbl = (f"@{b['off']:<5} charge utile — {esc(str(b['name']))} "
                   f"{b['size']} o")
        B.append(f'<rect x="{IN}" y="{yy}" width="{IW}" height="13" fill="{fill}" '
                 f'stroke="{stroke}" stroke-width="1"/>')
        txt(IN + 6, yy + 10, lbl, "z m")
        yy += 15
    if len(blocks) > MAXROWS:
        txt(IN + 6, yy + 6, f"… {len(blocks) - MAXROWS} blocs suivants, non montrés", "z")
    txt(IN + 6, yy + 6 + (10 if len(blocks) > MAXROWS else 0),
        f"zone libre basse : [{p['pd_lower']}, {p['pd_upper']}) = {free} o", "z")
    y += h

    # --- special ------------------------------------------------------------
    h = 48
    box(h, "#f0dcef")
    txt(IN, y + 19, "PaxSpecialData", "b")
    txt(PX + PW - 14, y + 19, f"8 o — offset {p['pd_special']}", "zr", "end")
    txt(IN, y + 35, f"version {p['special_version']} · magic 0x{p['special_magic']:04X} · "
                    f"n_attrs {p['n_attrs']} · flags 0x{p['special_flags']:04X}", "z m")
    y += h

    # La racine doit etre le PREMIER element du document : le fond blanc passe
    # donc en premiere position de B, une fois H connue. L'ajouter a la fin
    # produirait deux elements racine, donc un fichier XML invalide.
    H = y + 24
    B.insert(0, f'<rect width="{W}" height="{H}" fill="#fff"/>')
    O.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'viewBox="0 0 {W} {H}" '
             f'font-family="\'DejaVu Sans\',Helvetica,Arial,sans-serif">')
    O.extend(B)
    O.append('</svg>')

    if args.verify:
        problems = verify_layout(page, p, cols)
        for label, msg in problems:
            print(f"[{label}] {msg}", file=sys.stderr)
        if problems:
            sys.exit("layout incoherent")
        print("layout vérifié : chaînes de chunks cohérentes, bornes de l'arène "
              "respectées", file=sys.stderr)

    out = "\n".join(O)
    if args.output == "-":
        print(out)
    else:
        with open(args.output, "w") as f:
            f.write(out + "\n")
        print(f"écrit : {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()

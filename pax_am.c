/*
 * pax_am.c
 *
 * Table Access Method PostgreSQL basée sur le format PAX
 * (Partition Attributes Across).
 *
 * couvre :
 *   - structures de page PAX
 *   - initialisation de page
 *   - construction du descripteur de page
 *   - extraction de valeurs (pax_get_value)
 *   - slot custom avec materialization lazy
 *   - scan séquentiel basique
 *   - insertion réelle : première page libre, valeurs variables en haut de
 *     page, régions de colonnes qui grandissent, bitmap de NULL par colonne
 *
 * Limitations :
 *   - pas de FSM réel (recherche first-fit linéaire sur les pages)
 *   - pas de MVCC (xmin/xmax non gérés : toute ligne est visible)
 *   - pas de Generic WAL (perte potentielle en cas de crash)
 *   - pas de UPDATE / DELETE / index (callbacks NULL)
 *   - pas de vacuum
 *   - Generic WAL non implémenté
 *
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "common/relpath.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "storage/off.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/* extension */
PG_MODULE_MAGIC;

#define PAX_PAGE_VERSION            1

#define PAX_FLAG_HAS_NULLS          0x0001
#define PAX_FLAG_HAS_VARLENA        0x0002
#define PAX_FLAG_COMPRESSED         0x0004
#define PAX_FLAG_HAS_XMIN_XMAX      0x0008

/* bits8 abandonned in v19*/
#if PG_VERSION_NUM >= 190000
typedef uint8 bits8;
#endif

/* could be usefull */
#define PAX_DEBUG_LOG(...) \
	ereport(DEBUG1, (errhidestmt(true), errmsg(__VA_ARGS__)))
    
/* ------------------------------------------------------------------ */
/* Structures de page                                                  */
/* ------------------------------------------------------------------ */

typedef struct PaxSpecialData
{
    uint16      version;        /* PAX_PAGE_VERSION */
    uint16      flags;
    uint16      n_attrs;
    uint16      reserved;
} PaxSpecialData;

#define SizeOfPaxSpecialData    MAXALIGN(sizeof(PaxSpecialData))

typedef struct PaxPageHeader
{
    uint16      n_tuples;       /* nombre de tuples sur la page */
    uint16      max_tuples;     /* capacité de la page (0 = non calculé) */
    uint16      free_space;     /* approximation car certains champs à taille variable */
    uint16      flags;

    /*
     * offsets[i] = offset absolu depuis le début de la page
     * vers le début de la minipage de la colonne de l'indice.
     */
    OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} PaxPageHeader;

#define SizeOfPaxPageHeaderFixed    offsetof(PaxPageHeader, offsets)

/* là où commence l'en-tête PAX dans une page (càd après l'en-tête et les données spéciales). */
#define PaxPageHeaderPtr(page)  \
    ((PaxPageHeader *) ((char *) (page) + SizeOfPageHeaderData + SizeOfPaxSpecialData))

/*
 * Nos « offsets » sont des positions EN OCTETS dans la page (0..BLCKSZ-1),
 * pas des numéros de line pointer : OffsetNumberIsValid() (borné à
 * MaxOffsetNumber = BLCKSZ / sizeof(ItemIdData) = 2048) rejetterait tout
 * octet situé dans la moitié haute de la page.
 */
#define PaxOffsetIsValid(off) \
    ((off) != InvalidOffsetNumber && (off) < BLCKSZ)

/* ------------------------------------------------------------------ */
/* Descripteurs en mémoire                                             */
/* ------------------------------------------------------------------ */

typedef struct PaxMinipage
{
    char       *data;           /* pointeur vers les valeurs */
    int16       attlen;         /* longueur fixe ou -1 */
    char        attalign;
    bool        is_varlena;
    bool        has_nulls;
    bits8      *null_bitmap;    /* peut être NULL */
    uint16      n_values;
} PaxMinipage;

typedef struct PaxPageDesc
{
    Page            page;
    PaxSpecialData *special;
    PaxPageHeader  *header;
    PaxMinipage    *minipages;
    int             n_attrs;
    int             n_tuples;
    MemoryContext   ctx;        /* contexte dans lequel le desc a été alloué */
} PaxPageDesc;

/* ------------------------------------------------------------------ */
/* TupleTableSlot                                                     */
/* ------------------------------------------------------------------ */

typedef struct PaxTupleTableSlot
{
    TupleTableSlot  base;           /* DOIT être le premier champ */

    Buffer          buffer;         /* pin détenu par CE slot */
    Page            page;           /* pointe dans le buffer ci-dessus */
    PaxPageDesc    *pdesc;          /* EMPRUNTÉ au scan (PaxScanDesc) */
    int             tupno;          /* index dans la page */

    /*
     * Les valeurs se rangent dans tts_values / tts_isnull 
     */
} PaxTupleTableSlot;

/* ------------------------------------------------------------------ */
/* Scan descriptor                                                     */
/* ------------------------------------------------------------------ */

typedef struct PaxScanDescData
{
    TableScanDescData rs_base;      /* DOIT être le premier champ */

    Buffer          current_buf;
    PaxPageDesc    *current_pdesc;
    int             current_tupno;
    BlockNumber     current_block;
} PaxScanDescData;

typedef PaxScanDescData *PaxScanDesc;

/* ------------------------------------------------------------------ */
/* Protos                                                             */
/* ------------------------------------------------------------------ */

/* Page mngmt */
static void         pax_page_init(Page page, int n_attrs);
static bool         pax_page_is_valid(Page page);
static PaxPageDesc *pax_build_page_desc(Page page, TupleDesc tupdesc);
static void         pax_free_page_desc(PaxPageDesc *desc);

/* Value */
static Datum        pax_get_value(PaxPageDesc *desc, int attno, int tupno, bool *isnull);

/* Size / page layout */
static Size         pax_slot_stride(Form_pg_attribute attr);
static Size         pax_insert_space_needed(Relation rel, Datum *values,
                                            bool *isnulls, int tupno);
static OffsetNumber pax_alloc_payload(Page page, Datum value, int16 attlen);
static Size         pax_region_size(Page page, PaxPageHeader *phdr,
                                    int n_attrs, int i);
static Size         pax_page_free_space(Page page);
static void         pax_insert_bytes(Page page, PaxPageHeader *phdr,
                                     int n_attrs, int region_idx,
                                     Size at, Size len);

/* Slot callbacks */
static const TupleTableSlotOps *pax_slot_callbacks(Relation rel);
static void         pax_slot_init(TupleTableSlot *slot);
static void         pax_slot_release(TupleTableSlot *slot);
static void         pax_slot_clear(TupleTableSlot *slot);
static void         pax_slot_getsomeattrs(TupleTableSlot *slot, int natts);
static Datum        pax_slot_getsysattr(TupleTableSlot *slot, int attnum,
                                        bool *isnull);
static bool         pax_slot_is_current_xact_tuple(TupleTableSlot *slot);
static void         pax_slot_materialize(TupleTableSlot *slot);
static void         pax_slot_copyslot(TupleTableSlot *dstslot,
                                      TupleTableSlot *srcslot);
static HeapTuple     pax_slot_copy_heap_tuple(TupleTableSlot *slot);
static MinimalTuple pax_slot_copy_minimal_tuple(TupleTableSlot *slot,
                                                Size extra);

/* Scan callbacks */
static TableScanDesc pax_scan_begin(Relation rel, Snapshot snapshot,
                                    int nkeys, ScanKey key,
                                    ParallelTableScanDesc pscan, uint32 flags);
static void         pax_scan_end(TableScanDesc sscan);
static void         pax_scan_rescan(TableScanDesc sscan, ScanKey key,
                                    bool set_params, bool allow_strat,
                                    bool allow_sync, bool allow_pagemode);
static bool         pax_scan_getnextslot(TableScanDesc sscan,
                                         ScanDirection direction,
                                         TupleTableSlot *slot);

/* Insert                   */
/* le proto a changé en v19 */
/*                          */
#if PG_VERSION_NUM >= 190000
static void         pax_tuple_insert(Relation rel, TupleTableSlot *slot,
                                     CommandId cid, uint32 options,
                                     BulkInsertState bistate);
#else
static void         pax_tuple_insert(Relation rel, TupleTableSlot *slot,
                                     CommandId cid, int options,
                                     BulkInsertState bistate);
#endif

/* DDL / planner (pour CREATE TABLE) */
static void         pax_relation_set_new_filelocator(Relation rel,
                                                     const RelFileLocator *newrlocator,
                                                     char persistence,
                                                     TransactionId *freezeXid,
                                                     MultiXactId *minmulti);
static void         pax_relation_nontransactional_truncate(Relation rel);
static uint64       pax_relation_size(Relation rel, ForkNumber forkNumber);
static bool         pax_relation_needs_toast_table(Relation rel);
static void         pax_relation_estimate_size(Relation rel, int32 *attr_widths,
                                               BlockNumber *pages, double *tuples,
                                               double *allvisfrac);

/* Handler pour l'extension */
Datum               pax_tableam_handler(PG_FUNCTION_ARGS);

/* ------------------------------------------------------------------ */
/* TableAmRoutine                                                      */
/* ------------------------------------------------------------------ */

static const TableAmRoutine pax_methods = {
    .type = T_TableAmRoutine,

    /* Slot */
    .slot_callbacks = pax_slot_callbacks,

    /* Scan */
    .scan_begin     = pax_scan_begin,
    .scan_end       = pax_scan_end,
    .scan_rescan    = pax_scan_rescan,
    .scan_getnextslot = pax_scan_getnextslot,

    /* Insert (minimal) */
    .tuple_insert   = pax_tuple_insert,

    /* DDL / stockage — (pour CREATE TABLE / TRUNCATE / planner) */
    .relation_set_new_filelocator = pax_relation_set_new_filelocator,
    .relation_nontransactional_truncate = pax_relation_nontransactional_truncate,
    .relation_size  = pax_relation_size,
    .relation_needs_toast_table = pax_relation_needs_toast_table,
    .relation_estimate_size = pax_relation_estimate_size,

    /*
     * Tous les autres callbacks restent NULL pour l'instant.
     * PostgreSQL plantera s'ils sont appelés
     */
};

/* ------------------------------------------------------------------ */
/* Page management                                                     */
/* ------------------------------------------------------------------ */

static void
pax_page_init(Page page, int n_attrs)
{
    PaxSpecialData *special;
    PaxPageHeader  *phdr;
    Size            header_size;
    int             i;

    Assert(n_attrs > 0 && n_attrs <= MaxTupleAttributeNumber);

    PageInit(page, BLCKSZ, SizeOfPaxSpecialData);

    special = (PaxSpecialData *) PageGetSpecialPointer(page);
    special->version  = PAX_PAGE_VERSION;
    special->flags    = 0;
    special->n_attrs  = (uint16) n_attrs;
    special->reserved = 0;

    header_size = SizeOfPaxPageHeaderFixed + n_attrs * sizeof(OffsetNumber);
    header_size = MAXALIGN(header_size);

    phdr = (PaxPageHeader *) ((char *) page + SizeOfPageHeaderData + SizeOfPaxSpecialData);

    phdr->n_tuples   = 0;
    phdr->max_tuples = 0;
    phdr->free_space = (uint16) (BLCKSZ - (SizeOfPageHeaderData + SizeOfPaxSpecialData + header_size));
    phdr->flags      = 0;

    for (i = 0; i < n_attrs; i++)
        phdr->offsets[i] = InvalidOffsetNumber;

    ((PageHeader) page)->pd_lower =
        (LocationIndex) (SizeOfPageHeaderData + SizeOfPaxSpecialData + header_size);
}

static bool
pax_page_is_valid(Page page)
{
    PaxSpecialData *special;

    if (PageGetSpecialSize(page) < SizeOfPaxSpecialData)
        return false;

    special = (PaxSpecialData *) PageGetSpecialPointer(page);
    return (special->version == PAX_PAGE_VERSION);
}

/*
 * Construit un PaxPageDesc à partir d'une page.
 * Les métadonnées de colonnes (attlen, attalign, is_varlena) sont
 * récupérées depuis le TupleDesc.
 */
static PaxPageDesc *
pax_build_page_desc(Page page, TupleDesc tupdesc)
{
    PaxPageDesc    *desc;
    PaxSpecialData *special;
    PaxPageHeader  *phdr;
    int             i;
    char           *base = (char *) page;

    special = (PaxSpecialData *) PageGetSpecialPointer(page);

    if (special->version != PAX_PAGE_VERSION)
        elog(ERROR, "pax: invalid page version %u", special->version);

    if (special->n_attrs != tupdesc->natts)
        elog(ERROR, "pax: attribute count mismatch (page %u vs tupdesc %d)",
             special->n_attrs, tupdesc->natts);

    phdr = PaxPageHeaderPtr(page);

    desc = (PaxPageDesc *) palloc0(sizeof(PaxPageDesc));
    desc->page     = page;
    desc->special  = special;
    desc->header   = phdr;
    desc->n_attrs  = special->n_attrs;
    desc->n_tuples = phdr->n_tuples;
    desc->ctx      = CurrentMemoryContext;

    desc->minipages = (PaxMinipage *) palloc0(sizeof(PaxMinipage) * desc->n_attrs);

    for (i = 0; i < desc->n_attrs; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        PaxMinipage      *mp   = &desc->minipages[i];
        OffsetNumber      off  = phdr->offsets[i];
        Size              stride;
        Size              rsize;
        Size              bmp;

        mp->attlen     = attr->attlen;
        mp->attalign   = attr->attalign;
        mp->is_varlena = (attr->attlen < 0);
        mp->has_nulls  = false;     /* true si un bitmap est présent */
        mp->null_bitmap = NULL;
        mp->n_values   = phdr->n_tuples;

        if (!PaxOffsetIsValid(off))
        {
            /* colonne jamais écrite sur cette page */
            mp->data = NULL;
            continue;
        }

        stride = pax_slot_stride(attr);
        rsize  = pax_region_size(page, phdr, desc->n_attrs, i);

        if (rsize < (Size) desc->n_tuples * stride)
            elog(ERROR, "pax: inconsistent page layout for column %d "
                        "(region %zu, %d tuples x %zu)",
                 i, rsize, desc->n_tuples, stride);

        /*
         * Région = [bitmap de NULL][valeurs] sans espace perdu : tout le
         * reste est la taille du bitmap (0 => absent).
         */
        bmp = rsize - (Size) desc->n_tuples * stride;

        if (bmp > 0)
        {
      /* TODO: bits8 abandonné par la v19 */
            mp->null_bitmap = (bits8 *) (base + off);
            mp->has_nulls   = true;
        }
        mp->data = base + off + bmp;
    }

    return desc;
}

static void
pax_free_page_desc(PaxPageDesc *desc)
{
    if (desc == NULL)
        return;

    if (desc->minipages)
        pfree(desc->minipages);
    pfree(desc);
}

/* ------------------------------------------------------------------ */
/* Extraction de valeur d'un PaxPageDesc                              */
/* ------------------------------------------------------------------ */

static Datum
pax_get_value(PaxPageDesc *desc, int attno, int tupno, bool *isnull)
{
    PaxMinipage *mp;
    char        *ptr;

  /* sécurité contre les valeurs incohérentes */
    Assert(desc != NULL);
    Assert(attno >= 0 && attno < desc->n_attrs);
    Assert(tupno >= 0 && tupno < desc->n_tuples);

  /* récupération de l'attr de la minipage en fn de son num dans le PaxPageDesc */ 
    mp = &desc->minipages[attno];

    /*
     * Bitmap de NULL : convention PostgreSQL, bit à 0 = NULL,
     * bit à 1 = valeur présente (att_isnull dans macros  tupmacs.h).
     */
    if (mp->has_nulls && mp->null_bitmap && att_isnull(tupno, mp->null_bitmap))
    {
        *isnull = true;
        return (Datum) 0;
    }
    *isnull = false;

    if (mp->data == NULL)
    {
        *isnull = true;
        return (Datum) 0;
    }

    /* page de champs de longueur fixe : une valeur par tuple, pas à pas MAXALIGN(attlen) */
    if (!mp->is_varlena)
    {
        ptr = mp->data + ((Size) tupno * MAXALIGN(mp->attlen));

        switch (mp->attlen)
        {
            case 1:
                return CharGetDatum(*ptr);
            case 2:
                return Int16GetDatum(*(int16 *) ptr);
            case 4:
                return Int32GetDatum(*(int32 *) ptr);
            case 8:
                return Int64GetDatum(*(int64 *) ptr);
            default:
                /* by-reference fixed (uuid, macaddr, …) */
                return PointerGetDatum(ptr);
        }
    }

    /* page de champs de longueur variable : table d'offsets, un entrée MAXALIGNée par tuple */
    {
        OffsetNumber off =
            *(OffsetNumber *) (mp->data +
                               ((Size) tupno * MAXALIGN(sizeof(OffsetNumber))));

        if (!PaxOffsetIsValid(off))
        {
            *isnull = true;
            return (Datum) 0;
        }

        ptr = (char *) desc->page + off;
        /* on retourne le datum contenu dans la page dont l'offset est ok */
        return PointerGetDatum(ptr);
    }
}

/* ------------------------------------------------------------------ */
/* Layout des régions de colonnes / allocation                         */
/* ------------------------------------------------------------------ */

/*
 * Pas d'unification à la volée : chaque valeur fixe occupe
 * MAXALIGN(attlen) octets et chaque valeur variable une entrée d'offset
 * MAXALIGN(OffsetNumber) octets. Toutes les régions démarrent donc
 * alignées, et la lecture (pax_get_value) et l'écriture sont d'accord
 * sur le pas.
 */
static Size
pax_slot_stride(Form_pg_attribute attr)
{
    if (attr->attlen > 0)
        return MAXALIGN(attr->attlen);

    /* attlen -1 (varlena) ou -2 (cstring) : table d'offsets */
    return MAXALIGN(sizeof(OffsetNumber));
}

/*
 * Espace (borne supérieure) exigé pour insérer ce tuple dans une page qui
 * contient déjà "tupno" tuples. 
 */
static Size
pax_insert_space_needed(Relation rel, Datum *values, bool *isnulls, int tupno)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    Size        bitmap_need = MAXALIGN(((Size) tupno + 8) / 8);
    Size        need = 0;
    int         i;

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        need += pax_slot_stride(attr);
        need += bitmap_need;

        if (isnulls[i] || attr->attlen >= 0)
            continue;

        if (attr->attlen == -2)
            need += MAXALIGN(strlen(DatumGetPointer(values[i])) + 1);
        else
            need += MAXALIGN(VARSIZE_ANY(DatumGetPointer(values[i])));
    }

    return need;
}

/*
 * Alloue la représentation « en haut de page » (pd_upper descendante) d'une
 * valeur variable.  Renvoie InvalidOffsetNumber si la page est pleine.
 */
static OffsetNumber
pax_alloc_payload(Page page, Datum value, int16 attlen)
{
    PageHeader      phdr = (PageHeader) page;
    Size            len;
    Size            aligned_len;
    OffsetNumber    off;

    if (attlen == -2)
        len = strlen(DatumGetPointer(value)) + 1;
    else
        len = VARSIZE_ANY(DatumGetPointer(value));

    aligned_len = MAXALIGN(len);

    if ((Size) (phdr->pd_upper - phdr->pd_lower) < aligned_len)
        return InvalidOffsetNumber;

    phdr->pd_upper -= (LocationIndex) aligned_len;
    off = (OffsetNumber) phdr->pd_upper;

    memcpy((char *) page + off, DatumGetPointer(value), len);
    return off;
}

/* Taille de la région de colonne i : elle va jusqu'à la région suivante,
 * ou jusqu'à pd_lower si c'est la dernière. */
static Size
pax_region_size(Page page, PaxPageHeader *phdr, int n_attrs, int i)
{
    LocationIndex start = phdr->offsets[i];
    LocationIndex end;

    Assert(PaxOffsetIsValid(start));

    if (i + 1 < n_attrs && PaxOffsetIsValid(phdr->offsets[i + 1]))
        end = phdr->offsets[i + 1];
    else
        end = ((PageHeader) page)->pd_lower;

    Assert(end >= start);
    return (Size) (end - start);
}

/* Espace libre réel : entre la fin des régions et les valeurs en haut. */
static Size
pax_page_free_space(Page page)
{
    PageHeader phdr = (PageHeader) page;

    return (Size) (phdr->pd_upper - phdr->pd_lower);
}

/*
 * Insère "len" octets (doit être MAXALIGNé) à la position absolue "at" en
 * décalant tout ce qui suit vers la droite.  Les régions qui commencent à
 * "at" ou après sont décalées, sauf celle qu'on remplit (region_idx).
 *
 * C'est ce décalage qui permet à une colonne de grandir sans quitter sa
 * région : les zones de haut niveau (bitmap de pd_upper) ne sont pas
 * touchées car on ne déplace que [at, pd_lower[.
 */
static void
pax_insert_bytes(Page page, PaxPageHeader *phdr, int n_attrs,
                 int region_idx, Size at, Size len)
{
    PageHeader  hdr = (PageHeader) page;
    int         j;

    Assert(len == MAXALIGN(len));
    Assert(at <= (Size) hdr->pd_lower);

    if (len > 0)
        memmove((char *) page + at + len, (char *) page + at,
                (Size) hdr->pd_lower - at);

    hdr->pd_lower += (LocationIndex) len;

    for (j = 0; j < n_attrs; j++)
    {
        if (j == region_idx)
            continue;
        if (PaxOffsetIsValid(phdr->offsets[j]) && phdr->offsets[j] >= at)
            phdr->offsets[j] = (OffsetNumber) (phdr->offsets[j] + len);
    }
}

/* ------------------------------------------------------------------ */
/* Slot callbacks                                                      */
/* ------------------------------------------------------------------ */

static const TupleTableSlotOps TTSOpsPax;

static const TupleTableSlotOps *
pax_slot_callbacks(Relation rel)
{
    return &TTSOpsPax;
}

static void
pax_slot_init(TupleTableSlot *base)
{
    PaxTupleTableSlot *slot = (PaxTupleTableSlot *) base;

    /* le cœur a palloc0é la structure : tout est déjà à zéro */
    slot->buffer = InvalidBuffer;
    slot->page   = NULL;
    slot->pdesc  = NULL;
    slot->tupno  = -1;
}

/* Pas de ressource à libérer au-delà de clear(). */
static void
pax_slot_release(TupleTableSlot *base)
{
    pax_slot_clear(base);
}

/*
 * PG19 : le callback doit remplir les attributs 0..natts-1, positionner
 * tts_nvalid à natts, appeler slot_getmissingattrs() si la ligne contient
 * moins d'attributs, et refuser natts > TupleDesc->natts.
 */
static void
pax_slot_getsomeattrs(TupleTableSlot *base, int natts)
{
    PaxTupleTableSlot *slot = (PaxTupleTableSlot *) base;
    int         i;

    if (natts > base->tts_tupleDescriptor->natts)
        elog(ERROR, "pax: invalid attribute number %d", natts);

    if (!BufferIsValid(slot->buffer) || slot->pdesc == NULL || slot->tupno < 0)
    {
        /*
         * Aucune ligne en mémoire : tout est NULL (à distinguer d'un attribut
         * « manquant » géré par slot_getmissingattrs()).
         */
        for (i = base->tts_nvalid; i < natts; i++)
        {
            base->tts_values[i] = (Datum) 0;
            base->tts_isnull[i] = true;
        }
        base->tts_nvalid = natts;
        return;
    }

    /* une page PAX porte toujours toutes les colonnes */
    for (i = base->tts_nvalid; i < natts; i++)
        base->tts_values[i] = pax_get_value(slot->pdesc, i, slot->tupno,
                                            &base->tts_isnull[i]);

    base->tts_nvalid = natts;
}

static Datum
pax_slot_getsysattr(TupleTableSlot *base, int attnum, bool *isnull)
{
    /* Les colonnes système (xmin, ctid, …) ne sont pas gérées pour l'instant. */
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pax table AM does not support system column %d", attnum)));
    *isnull = true;                 /* pas atteint */
    return (Datum) 0;
}

/*
 * Pas de MVCC encore : on ne sait pas si une ligne vient de la transaction
 * courante. On répond « non », ce qui pousse les déclencheurs RI à vérifier
 * l'existence de la ligne comme pour une ligne ancienne.
 */
static bool
pax_slot_is_current_xact_tuple(TupleTableSlot *base)
{
    return false;
}

static void
pax_slot_clear(TupleTableSlot *base)
{
    PaxTupleTableSlot *slot = (PaxTupleTableSlot *) base;
    TupleDesc   desc = base->tts_tupleDescriptor;
    int         i;

    /*
     * Si materialize() a copié des valeurs par référence dans tts_mcxt,
     * c'est à nous de les libérer.
     */
    if (TTS_SHOULDFREE(base) && desc != NULL)
    {
        for (i = 0; i < desc->natts; i++)
        {
            Form_pg_attribute attr = TupleDescAttr(desc, i);

            if (!base->tts_isnull[i] && !attr->attbyval &&
                DatumGetPointer(base->tts_values[i]) != NULL)
                pfree(DatumGetPointer(base->tts_values[i]));

            base->tts_values[i] = (Datum) 0;
            base->tts_isnull[i] = false;
        }
        base->tts_flags &= ~TTS_FLAG_SHOULDFREE;
    }

    if (BufferIsValid(slot->buffer))
    {
        ReleaseBuffer(slot->buffer);
        slot->buffer = InvalidBuffer;
    }

    /*
     * Le descripteur de page appartient au scan (libéré à la page suivante,
     * au scan_end ou au rescan) : le slot ne fait que l'emprunter et ne doit
     * surtout pas le libérer — sinon double free / use-after-free.
     */
    slot->page   = NULL;
    slot->pdesc  = NULL;
    slot->tupno  = -1;

    base->tts_flags |= TTS_FLAG_EMPTY;
    base->tts_nvalid = 0;
    ItemPointerSetInvalid(&base->tts_tid);

    /* Ne pas appeler ExecClearTuple ici : c'est clear() qui est appelé. */
}

/*
 * Rend la ligne indépendante de la page : on déforme tout, on copie les
 * valeurs par référence dans tts_mcxt, puis on relâche le pin. Après ça,
 * le pin appartient au slot seulement s'il l'a gardé — ici on le libère.
 */
static void
pax_slot_materialize(TupleTableSlot *base)
{
    TupleDesc   desc = base->tts_tupleDescriptor;
    MemoryContext oldcxt;
    PaxTupleTableSlot *slot = (PaxTupleTableSlot *) base;
    int         i;

    if (TTS_SHOULDFREE(base))
        return;                     /* déjà détachée */

    slot_getallattrs(base);

    oldcxt = MemoryContextSwitchTo(base->tts_mcxt);
    for (i = 0; i < desc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(desc, i);

        if (base->tts_isnull[i] || attr->attbyval)
            continue;

        base->tts_values[i] = datumCopy(base->tts_values[i], false, attr->attlen);
    }
    MemoryContextSwitchTo(oldcxt);

    base->tts_flags |= TTS_FLAG_SHOULDFREE;

    /* la ligne ne dépend plus de la page : on peut rendre le pin */
    if (BufferIsValid(slot->buffer))
    {
        ReleaseBuffer(slot->buffer);
        slot->buffer = InvalidBuffer;
    }
    slot->page   = NULL;
    slot->pdesc  = NULL;
    slot->tupno  = -1;
}

static void
pax_slot_copyslot(TupleTableSlot *dstbase, TupleTableSlot *srcbase)
{
    TupleDesc   desc = srcbase->tts_tupleDescriptor;
    int         i;

    ExecClearTuple(dstbase);        /* libère pin + copies éventuelles */

    slot_getallattrs(srcbase);

    for (i = 0; i < desc->natts; i++)
    {
        dstbase->tts_values[i] = srcbase->tts_values[i];
        dstbase->tts_isnull[i] = srcbase->tts_isnull[i];
    }

    dstbase->tts_flags &= ~TTS_FLAG_EMPTY;
    dstbase->tts_nvalid = desc->natts;
    dstbase->tts_tid     = srcbase->tts_tid;
    dstbase->tts_tableOid = srcbase->tts_tableOid;

    pax_slot_materialize(dstbase);  /* détache de la page source */
}

static HeapTuple
pax_slot_copy_heap_tuple(TupleTableSlot *base)
{
    Assert(!TTS_EMPTY(base));
    slot_getallattrs(base);
    return heap_form_tuple(base->tts_tupleDescriptor,
                           base->tts_values, base->tts_isnull);
}

static MinimalTuple
pax_slot_copy_minimal_tuple(TupleTableSlot *base, Size extra)
{
    Assert(!TTS_EMPTY(base));
    slot_getallattrs(base);
    return heap_form_minimal_tuple(base->tts_tupleDescriptor,
                                   base->tts_values, base->tts_isnull, extra);
}

/*
 * Ops complètes : sans init/clear/materialize/copyslot, tout appel de
 * ExecMaterializeSlot / ExecCopySlot / RETURNING / trigger plante.
 * get_heap_tuple reste NULL (aucun HeapTuple « possédé ») : le cœur utilise
 * alors copy_heap_tuple.
 */
static const TupleTableSlotOps TTSOpsPax = {
    .base_slot_size = sizeof(PaxTupleTableSlot),
    .init           = pax_slot_init,
    .release        = pax_slot_release,
    .clear          = pax_slot_clear,
    .getsomeattrs   = pax_slot_getsomeattrs,
    .getsysattr     = pax_slot_getsysattr,
    .is_current_xact_tuple = pax_slot_is_current_xact_tuple,
    .materialize    = pax_slot_materialize,
    .copyslot       = pax_slot_copyslot,
    .get_heap_tuple = NULL,
    .get_minimal_tuple = NULL,
    .copy_heap_tuple = pax_slot_copy_heap_tuple,
    .copy_minimal_tuple = pax_slot_copy_minimal_tuple,
};

/* ------------------------------------------------------------------ */
/* Scan callbacks                                                      */
/* ------------------------------------------------------------------ */

static TableScanDesc
pax_scan_begin(Relation rel, Snapshot snapshot,
               int nkeys, ScanKey key,
               ParallelTableScanDesc pscan, uint32 flags)
{
    PaxScanDesc scan;

    scan = (PaxScanDesc) palloc0(sizeof(PaxScanDescData));

    scan->rs_base.rs_rd        = rel;
    scan->rs_base.rs_snapshot  = snapshot;
    scan->rs_base.rs_nkeys     = nkeys;
    scan->rs_base.rs_key       = key;
    scan->rs_base.rs_flags     = flags;
    /* scan->rs_base.rs_parallel = pscan; */

    scan->current_buf    = InvalidBuffer;
    scan->current_pdesc  = NULL;
    scan->current_tupno  = -1;
    scan->current_block  = 0;

    return (TableScanDesc) scan;
}

static void
pax_scan_end(TableScanDesc sscan)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;

    if (BufferIsValid(scan->current_buf))
    {
        ReleaseBuffer(scan->current_buf);
        scan->current_buf = InvalidBuffer;
    }

    if (scan->current_pdesc)
    {
        pax_free_page_desc(scan->current_pdesc);
        scan->current_pdesc = NULL;
    }

    pfree(scan);
}

static void
pax_scan_rescan(TableScanDesc sscan, ScanKey key,
                bool set_params, bool allow_strat,
                bool allow_sync, bool allow_pagemode)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;

    if (BufferIsValid(scan->current_buf))
    {
        ReleaseBuffer(scan->current_buf);
        scan->current_buf = InvalidBuffer;
    }

    if (scan->current_pdesc)
    {
        pax_free_page_desc(scan->current_pdesc);
        scan->current_pdesc = NULL;
    }

    scan->current_tupno = -1;
    scan->current_block = 0;

    if (key && scan->rs_base.rs_nkeys > 0)
        scan->rs_base.rs_key = key;
}

static bool
pax_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                     TupleTableSlot *slot)
{
    PaxScanDesc         scan = (PaxScanDesc) sscan;
    PaxTupleTableSlot  *pslot;
    Relation            rel = scan->rs_base.rs_rd;
    TupleDesc           tupdesc = RelationGetDescr(rel);
    BlockNumber         nblocks;
    Buffer              buf;
    Page                page;
    PaxPageDesc        *pdesc;
    int                 tupno;

    /* On s'attend à recevoir notre propre type de slot */
    pslot = (PaxTupleTableSlot *) slot;

    /*
     * Libère le contenu précédent : on ne fait que pointer dans la page,
     * le descripteur appartient au scan. ExecClearTuple → pax_slot_clear.
     */
    ExecClearTuple(slot);

    /* === Continuer sur la page courante === */
    if (BufferIsValid(scan->current_buf))
    {
        pdesc = scan->current_pdesc;
        tupno = scan->current_tupno + 1;

        if (tupno < pdesc->n_tuples)
        {
            scan->current_tupno = tupno;

            pslot->buffer = scan->current_buf;
            IncrBufferRefCount(scan->current_buf);
            pslot->page   = BufferGetPage(scan->current_buf);
            pslot->pdesc  = pdesc;          /* emprunté, libéré par le scan */
            pslot->tupno  = tupno;

            /* pas ExecStoreVirtualTuple : cela poserait tts_nvalid = natts
             * et tuerait la déformation paresseuse de getsomeattrs */
            slot->tts_flags &= ~TTS_FLAG_EMPTY;
            slot->tts_nvalid = 0;
            /* offset de TID = tupno + 1 (les offsets de TID débutent à 1) */
            ItemPointerSet(&slot->tts_tid,
                           BufferGetBlockNumber(scan->current_buf),
                           (OffsetNumber) (tupno + 1));
            return true;
        }

        /* Page terminée */
        ReleaseBuffer(scan->current_buf);
        scan->current_buf = InvalidBuffer;
        pax_free_page_desc(scan->current_pdesc);
        scan->current_pdesc = NULL;
    }

    /* === Chercher la page suivante non vide === */
    nblocks = RelationGetNumberOfBlocks(rel);

    while (scan->current_block < nblocks)
    {
        buf = ReadBuffer(rel, scan->current_block);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        if (!pax_page_is_valid(page))
        {
            UnlockReleaseBuffer(buf);
            scan->current_block++;
            continue;
        }

        pdesc = pax_build_page_desc(page, tupdesc);

        /*
         * On ne garde PAS le content lock entre deux appels de
         * scan_getnextslot : le verrou resterait posé jusqu'au scan_end et
         * une écriture concurrente finirait par deadlock. On ne conserve que
         * le pin, suffisant pour que pdesc->... reste adressable.
         * (Concurrence réduite : notre insert écrit toujours une page neuve.)
         */
        UnlockBuffer(buf);

        if (pdesc->n_tuples > 0)
        {
            scan->current_buf   = buf;
            scan->current_pdesc = pdesc;
            scan->current_tupno = 0;
            scan->current_block++;

            pslot->buffer = buf;
            IncrBufferRefCount(buf);
            pslot->page   = page;
            pslot->pdesc  = pdesc;          /* emprunté, libéré par le scan */
            pslot->tupno  = 0;

            slot->tts_flags &= ~TTS_FLAG_EMPTY;
            slot->tts_nvalid = 0;
            ItemPointerSet(&slot->tts_tid, BufferGetBlockNumber(buf),
                           (OffsetNumber) 1);  /* tupno 0 → offset de TID 1 */
            return true;
        }

        pax_free_page_desc(pdesc);
        ReleaseBuffer(buf);
        scan->current_block++;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Insertion (prototype)                                               */
/* ------------------------------------------------------------------ */

/*
 * Insertion réelle :
 * - choisit la première page ayant assez de place (sinon en crée une),
 * - écrit les valeurs variables en haut de page (pd_upper descendante),
 * - fait grandir la région de chaque colonne ([bitmap][valeurs]) via des
 *   memmove vers la droite (pd_lower montante),
 * - pose le bit de NULL dans le bitmap de chaque colonne.
 *
 * Reste à faire (voir analysis0.md) : MVCC (xmin/xmax), Generic WAL,
 * gestion FSM au lieu d'un premier libre naïf.
 */
#if PG_VERSION_NUM >= 190000
static void
pax_tuple_insert(Relation rel, TupleTableSlot *slot,
                 CommandId cid, uint32 options,
                 BulkInsertState bistate)
#else 
static void
pax_tuple_insert(Relation rel, TupleTableSlot *slot,
                 CommandId cid, int options,
                 BulkInsertState bistate)
#endif

{
    Buffer          buf = InvalidBuffer;
    Page            page;
    PageHeader      pghdr;
    PaxPageHeader  *phdr;
    TupleDesc       tupdesc = RelationGetDescr(rel);
    int             natts = tupdesc->natts;
    BlockNumber     nblocks;
    BlockNumber     blk;
    int             tupno;
    int             i;
    Datum          *values;
    bool           *isnulls;
    OffsetNumber   *voffs;

    /*
     * Détache la ligne du slot d'origine : pour INSERT ... SELECT la source
     * peut pointer dans une page PAX (voire celle-ci) que les memmove
     * ci-dessous déplaceraient sous les pieds du lecteur.
     */
    if (!TTS_EMPTY(slot))
        ExecMaterializeSlot(slot);

    values  = (Datum *) palloc(sizeof(Datum) * natts);
    isnulls = (bool *) palloc(sizeof(bool) * natts);
    voffs   = (OffsetNumber *) palloc(sizeof(OffsetNumber) * natts);

    /* Lire et détacher (detoast) toutes les valeurs AVANT toute écriture. */
    for (i = 0; i < natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        voffs[i]  = InvalidOffsetNumber;
        values[i] = slot_getattr(slot, i + 1, &isnulls[i]);

        if (!isnulls[i] && attr->attlen == -1)
            values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
    }

    /* === 1. Choisir une page ayant assez de place (premier libre) === */
    nblocks = RelationGetNumberOfBlocks(rel);

    for (blk = 0; blk < nblocks; blk++)
    {
        Buffer          b = ReadBuffer(rel, blk);
        Page            p;
        PaxPageHeader  *h;
        Size            needed;

        LockBuffer(b, BUFFER_LOCK_EXCLUSIVE);
        p = BufferGetPage(b);

        if (!pax_page_is_valid(p))
        {
            /* Page jamais initialisée (extension au zéro) : on l'initie ;
             * page d'un format étranger : on la saute. */
            if (PageIsNew(p))
                pax_page_init(p, natts);
            else
            {
                UnlockReleaseBuffer(b);
                continue;
            }
        }

        /* n_tuples lu APRÈS le lock : l'état peut avoir changé */
        h      = PaxPageHeaderPtr(p);
        needed = pax_insert_space_needed(rel, values, isnulls, h->n_tuples);

        if (pax_page_free_space(p) >= needed)
        {
            buf = b;
            break;
        }

        UnlockReleaseBuffer(b);
    }

    if (!BufferIsValid(buf))
    {
        buf = ReadBuffer(rel, P_NEW);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        pax_page_init(BufferGetPage(buf), natts);
    }

    page  = BufferGetPage(buf);
    pghdr = (PageHeader) page;
    phdr  = PaxPageHeaderPtr(page);
    tupno = phdr->n_tuples;

    /* === 2. Valeurs variables : en haut de page, avant tout décalage === */
    for (i = 0; i < natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        if (isnulls[i] || attr->attlen >= 0)
            continue;

        voffs[i] = pax_alloc_payload(page, values[i], attr->attlen);
        if (!PaxOffsetIsValid(voffs[i]))
            elog(ERROR, "pax: no space left in page for a variable-length value "
                        "(free %zu, attlen %d, pd_lower %u, pd_upper %u, n_tuples %d)",
                 pax_page_free_space(page), attr->attlen,
                 pghdr->pd_lower, pghdr->pd_upper, phdr->n_tuples);
    }

    /* === 3. Une région par colonne : [bitmap de NULL][valeurs] === */
    for (i = 0; i < natts; i++)
    {
        Form_pg_attribute attr   = TupleDescAttr(tupdesc, i);
        Size              stride = pax_slot_stride(attr);
        Size              region_start;
        Size              rsize;
        Size              cur;        /* taille actuelle du bitmap */
        Size              want;       /* taille voulue du bitmap */
        Size              at;
        bits8            *bmp;

        /* Première écriture de cette colonne : la région s'ouvre à la fin
         * de la zone des régions (pd_lower). */
        if (!PaxOffsetIsValid(phdr->offsets[i]))
            phdr->offsets[i] = (OffsetNumber) pghdr->pd_lower;

        region_start = phdr->offsets[i];
        rsize = pax_region_size(page, phdr, natts, i);
        Assert(rsize >= (Size) tupno * stride);
        cur = rsize - (Size) tupno * stride;     /* = bitmap seul */

        /* 3a) le bitmap doit couvrir le bit "tupno" (0 = NULL, 1 = valeur) */
        want = MAXALIGN(((Size) tupno + 8) / 8);
        if (want > cur)
        {
            at = region_start + cur;
            pax_insert_bytes(page, phdr, natts, i, at, want - cur);
            memset((char *) page + at, 0, want - cur);
            cur = want;
        }
        bmp = (bits8 *) ((char *) page + region_start);

        /* 3b) réserver l'emplacement de la valeur dans la région */
        at = region_start + cur + (Size) tupno * stride;
        pax_insert_bytes(page, phdr, natts, i, at, stride);

        /* 3c) positionner le bit, puis écrire la valeur */
        if (isnulls[i])
            bmp[tupno >> 3] &= (bits8) ~(1 << (tupno & 0x07));
        else
            bmp[tupno >> 3] |= (bits8) (1 << (tupno & 0x07));

        if (isnulls[i])
            memset((char *) page + at, 0, stride);
        else if (attr->attlen > 0)
        {
            char *ptr = (char *) page + at;

            /*
             * Colonne passée par VALEUR (bool, int2, int4, int8, float4,
             * float8, timestamp, …) : la valeur EST le Datum, la traiter
             * comme un pointeur déréférencerait un entier → SIGSEGV.
             * On écrit donc l'entier, à l'endroit exact de ce que
             * pax_get_value lira (CharGetDatum/Int16/Int32/Int64).
             */
            if (attr->attbyval)
            {
                switch (attr->attlen)
                {
                    case 1:
                        *(char *) ptr = DatumGetChar(values[i]);
                        break;
                    case 2:
                        *(int16 *) ptr = DatumGetInt16(values[i]);
                        break;
                    case 4:
                        *(int32 *) ptr = DatumGetInt32(values[i]);
                        break;
                    case 8:
                        *(int64 *) ptr = DatumGetInt64(values[i]);
                        break;
                    default:
                        elog(ERROR, "pax: unsupported byval attribute "
                                    "length %d", attr->attlen);
                }
            }
            else
                memcpy(ptr, DatumGetPointer(values[i]), attr->attlen);
        }
        else
            ((OffsetNumber *) ((char *) page + at))[0] = voffs[i];
    }

    /* === 4. Finaliser === */
    phdr->n_tuples++;
    phdr->free_space = (uint16) pax_page_free_space(page);

    /* TID : offset de TID = tupno + 1 (les offsets de TID débutent à 1) */
    ItemPointerSet(&(slot->tts_tid), BufferGetBlockNumber(buf),
                   (OffsetNumber) (tupno + 1));

    /* TODO: Generic WAL */

    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);

    pfree(values);
    pfree(isnulls);
    pfree(voffs);
}

/* ------------------------------------------------------------------ */
/* Callbacks DDL / planner                                             */
/* ------------------------------------------------------------------ */

/*
 * Création du stockage d'une nouvelle relation.
 *
 * C'est ce callback qui débloque `CREATE TABLE ... USING pax` : sans lui,
 * heap_create_with_catalog() appelle un pointeur NULL et le backend plante
 * (signal 11). Copié tel quel de heapam_relation_set_new_filelocator().
 */
static void
pax_relation_set_new_filelocator(Relation rel,
                                 const RelFileLocator *newrlocator,
                                 char persistence,
                                 TransactionId *freezeXid,
                                 MultiXactId *minmulti)
{
    SMgrRelation srel;

    /* Pas de MVCC : même initialisation que heap, inoffensive ici. */
    *freezeXid = RecentXmin;
    *minmulti  = GetOldestMultiXactId();

    srel = RelationCreateStorage(*newrlocator, persistence, true);

    /* Table non journalisée : il faut un init fork reconstruit au démarrage. */
    if (persistence == RELPERSISTENCE_UNLOGGED)
    {
        smgrcreate(srel, INIT_FORKNUM, false);
        log_smgrcreate(newrlocator, INIT_FORKNUM);
    }

    smgrclose(srel);
}

/* TRUNCATE : on vide les forks principaux de la relation. */
static void
pax_relation_nontransactional_truncate(Relation rel)
{
    RelationTruncate(rel, 0);
}

/* pg_relation_size() et comparses : nos données sont dans les forks normaux. */
static uint64
pax_relation_size(Relation rel, ForkNumber forkNumber)
{
    return table_block_relation_size(rel, forkNumber);
}

/*
 * Appelé par heap_toast... avant toute copie dépassée de page. Le prototype
 * ne gère pas encore le débordement d'une ligne trop grosse → on répond non
 * (l'insert d'une valeur > page échouera proprement côté allocation).
 */
static bool
pax_relation_needs_toast_table(Relation rel)
{
    return false;
}

/* Surcoût par tuple / espace utilisable par page, pour l'estimateur du
 * planificateur : approximations, améliorables quand la page mûrira. */
#define PAX_OVERHEAD_BYTES_PER_TUPLE  (2 * sizeof(OffsetNumber))
#define PAX_USABLE_BYTES_PER_PAGE     \
    (BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(PaxSpecialData)))

static void
pax_relation_estimate_size(Relation rel, int32 *attr_widths,
                           BlockNumber *pages, double *tuples,
                           double *allvisfrac)
{
    table_block_relation_estimate_size(rel, attr_widths, pages, tuples,
                                       allvisfrac,
                                       PAX_OVERHEAD_BYTES_PER_TUPLE,
                                       PAX_USABLE_BYTES_PER_PAGE);
}

/* ------------------------------------------------------------------ */
/* Handler                                                             */
/* ------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(pax_tableam_handler);

Datum
pax_tableam_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&pax_methods);
}

/* ------------------------------------------------------------------ */
/* Fin du fichier                                                      */
/* ------------------------------------------------------------------ */

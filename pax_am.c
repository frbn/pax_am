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
 *   - MVCC d'insertion implémenté, mais pas de versions UPDATE / DELETE
 *   - pas de Generic WAL (perte potentielle en cas de crash)
 *   - pas de vacuum, ni de gel des tuples
 *   - pas d'index, de parallélisme ou d'opérations DDL non réécrivantes
 *
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/transam.h"
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
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/* extension */
PG_MODULE_MAGIC;

#define PAX_PAGE_VERSION            2
#define PAX_SPECIAL_MAGIC           0x5041 /* "PA" */

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
    uint16      magic;
} PaxSpecialData;

#define SizeOfPaxSpecialData    MAXALIGN(sizeof(PaxSpecialData))

/*
 * Transaction metadata is stored columnarly, once per tuple, immediately
 * before the user-column regions.  Command IDs are meaningful only while the
 * corresponding transaction is current, but storing them separately keeps the
 * visibility rules simple and leaves room for future xmax updates.
 */
typedef struct PaxTupleMetaData
{
    TransactionId xmin;
    TransactionId xmax;
    CommandId     cmin;
    CommandId     cmax;
} PaxTupleMetaData;

#define SizeOfPaxTupleMetaData   MAXALIGN(sizeof(PaxTupleMetaData))

StaticAssertDecl(sizeof(PaxTupleMetaData) == SizeOfPaxTupleMetaData,
                 "PAX tuple metadata must have a fixed on-disk size");

typedef struct PaxPageHeader
{
    uint16      n_tuples;       /* nombre de tuples sur la page */
    OffsetNumber meta_offset;   /* début de la région des métadonnées */
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
#define PAX_NO_REGION          (-1)

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
    PaxTupleMetaData *tuple_meta; /* copie indépendante de la page */
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
    PaxPageDesc    *pdesc;          /* emprunté au scan ou détenu par le slot */
    int             tupno;          /* index dans la page */
    bool            owns_pdesc;
    bool            has_tuple_meta;
    PaxTupleMetaData tuple_meta;

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
    BlockNumber     nblocks;
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
static Size         pax_bitmap_size(int n_tuples);
static Size         pax_insert_space_needed(Relation rel, Datum *values,
                                            bool *isnulls, int tupno);
static OffsetNumber pax_alloc_payload(Page page, Datum value, int16 attlen);
static Size         pax_region_size(Page page, PaxPageHeader *phdr,
                                    int n_attrs, int i);
static Size         pax_page_free_space(Page page);
static Size         pax_meta_region_size(Page page, PaxPageHeader *phdr);
static void         pax_insert_bytes(Page page, PaxPageHeader *phdr,
                                     int n_attrs, int region_idx,
                                     Size at, Size len);
static bool         pax_meta_satisfies_snapshot(const PaxTupleMetaData *meta,
                                                Snapshot snapshot);
static int          pax_find_visible_tuple(PaxScanDesc scan, PaxPageDesc *pdesc,
                                           int start);
static void         pax_store_tuple_slot(Relation rel, TupleTableSlot *slot,
                                          Buffer buffer, PaxPageDesc *pdesc,
                                          int tupno, bool owns_pdesc);

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

/* Tuple lookup / visibility */
static bool         pax_tuple_fetch_row_version(Relation rel, ItemPointer tid,
                                                 Snapshot snapshot,
                                                 TupleTableSlot *slot);
static bool         pax_tuple_tid_valid(TableScanDesc sscan, ItemPointer tid);
static void         pax_tuple_get_latest_tid(TableScanDesc sscan,
                                             ItemPointer tid);
static bool         pax_tuple_satisfies_snapshot(Relation rel,
                                                   TupleTableSlot *slot,
                                                   Snapshot snapshot);

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

    /* Tuple lookup / MVCC visibility */
    .tuple_fetch_row_version = pax_tuple_fetch_row_version,
    .tuple_tid_valid = pax_tuple_tid_valid,
    .tuple_get_latest_tid = pax_tuple_get_latest_tid,
    .tuple_satisfies_snapshot = pax_tuple_satisfies_snapshot,

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

    Assert(n_attrs >= 0 && n_attrs <= MaxTupleAttributeNumber);

    PageInit(page, BLCKSZ, SizeOfPaxSpecialData);

    special = (PaxSpecialData *) PageGetSpecialPointer(page);
    special->version  = PAX_PAGE_VERSION;
    special->flags    = PAX_FLAG_HAS_XMIN_XMAX;
    special->n_attrs  = (uint16) n_attrs;
    special->magic = PAX_SPECIAL_MAGIC;

    header_size = SizeOfPaxPageHeaderFixed + n_attrs * sizeof(OffsetNumber);
    header_size = MAXALIGN(header_size);

    phdr = (PaxPageHeader *) ((char *) page + SizeOfPageHeaderData + SizeOfPaxSpecialData);

    phdr->n_tuples   = 0;
    phdr->free_space = (uint16) (BLCKSZ - (SizeOfPageHeaderData + SizeOfPaxSpecialData + header_size));
    phdr->flags      = PAX_FLAG_HAS_XMIN_XMAX;

    for (i = 0; i < n_attrs; i++)
        phdr->offsets[i] = InvalidOffsetNumber;

    ((PageHeader) page)->pd_lower =
        (LocationIndex) (SizeOfPageHeaderData + SizeOfPaxSpecialData + header_size);
    phdr->meta_offset = (OffsetNumber) ((PageHeader) page)->pd_lower;
}

static bool
pax_page_is_valid(Page page)
{
    PaxSpecialData *special;

    if (PageGetSpecialSize(page) < SizeOfPaxSpecialData)
        return false;

    special = (PaxSpecialData *) PageGetSpecialPointer(page);
    return (special->version == PAX_PAGE_VERSION &&
            special->magic == PAX_SPECIAL_MAGIC &&
            (special->flags & PAX_FLAG_HAS_XMIN_XMAX) != 0);
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
    if (special->magic != PAX_SPECIAL_MAGIC)
        elog(ERROR, "pax: invalid page magic %u", special->magic);

    if (special->n_attrs != tupdesc->natts)
        elog(ERROR, "pax: attribute count mismatch (page %u vs tupdesc %d)",
             special->n_attrs, tupdesc->natts);

    if ((special->flags & PAX_FLAG_HAS_XMIN_XMAX) == 0)
        elog(ERROR, "pax: page does not contain transaction metadata");

    phdr = PaxPageHeaderPtr(page);

    desc = (PaxPageDesc *) palloc0(sizeof(PaxPageDesc));
    desc->page     = page;
    desc->special  = special;
    desc->header   = phdr;
    desc->n_attrs  = special->n_attrs;
    desc->n_tuples = phdr->n_tuples;
    desc->ctx      = CurrentMemoryContext;

    if ((phdr->flags & PAX_FLAG_HAS_XMIN_XMAX) == 0)
        elog(ERROR, "pax: page header does not contain transaction metadata");

    if (desc->n_tuples > 0)
    {
        Size expected = (Size) desc->n_tuples * SizeOfPaxTupleMetaData;
        Size actual;

        if (!PaxOffsetIsValid(phdr->meta_offset))
            elog(ERROR, "pax: missing transaction metadata region");

        actual = pax_meta_region_size(page, phdr);
        if (actual != expected)
            elog(ERROR, "pax: inconsistent transaction metadata region "
                        "(region %zu, %d tuples x %zu)",
                 actual, desc->n_tuples, SizeOfPaxTupleMetaData);

        desc->tuple_meta = (PaxTupleMetaData *)
            palloc(sizeof(PaxTupleMetaData) * desc->n_tuples);
        memcpy(desc->tuple_meta,
               (char *) page + phdr->meta_offset,
               expected);

        for (i = 0; i < desc->n_tuples; i++)
        {
            PaxTupleMetaData *meta = &desc->tuple_meta[i];

            if (!TransactionIdIsValid(meta->xmin))
                elog(ERROR, "pax: tuple %d has invalid xmin", i);
            if (meta->cmin == InvalidCommandId)
                elog(ERROR, "pax: tuple %d has invalid cmin", i);
            if (!TransactionIdIsValid(meta->xmax) &&
                meta->cmax != InvalidCommandId)
                elog(ERROR, "pax: tuple %d has cmax without xmax", i);
            if (TransactionIdIsValid(meta->xmax) &&
                meta->cmax == InvalidCommandId)
                elog(ERROR, "pax: tuple %d has xmax without cmax", i);
        }
    }

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
        if (bmp != pax_bitmap_size(desc->n_tuples))
            elog(ERROR, "pax: invalid NULL bitmap size for column %d "
                        "(region %zu, tuples %d, bitmap %zu)",
                 i, rsize, desc->n_tuples, bmp);

        if (bmp > 0)
        {
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

    if (desc->tuple_meta)
        pfree(desc->tuple_meta);
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

static Size
pax_bitmap_size(int n_tuples)
{
    if (n_tuples <= 0)
        return 0;

    return MAXALIGN(((Size) n_tuples + 7) / 8);
}

/*
 * Espace (borne supérieure) exigé pour insérer ce tuple dans une page qui
 * contient déjà "tupno" tuples. 
 */
static Size
pax_insert_space_needed(Relation rel, Datum *values, bool *isnulls, int tupno)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    Size        bitmap_delta = pax_bitmap_size(tupno + 1) -
        pax_bitmap_size(tupno);
    Size        need = SizeOfPaxTupleMetaData;
    Size        amount;
    int         i;

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        amount = pax_slot_stride(attr) + bitmap_delta;
        if (amount > MaxAllocSize - need)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pax: row size exceeds supported limits")));
        need += amount;

        if (isnulls[i] || attr->attlen >= 0)
            continue;

        if (attr->attlen == -2)
            amount = MAXALIGN(strlen(DatumGetPointer(values[i])) + 1);
        else
            amount = MAXALIGN(VARSIZE_ANY(DatumGetPointer(values[i])));
        if (amount > MaxAllocSize - need)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pax: row size exceeds supported limits")));
        need += amount;
    }

    if (need > BLCKSZ)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pax: row is too large for one page")));

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

/* Transaction metadata ends at the first user region (or pd_lower). */
static Size
pax_meta_region_size(Page page, PaxPageHeader *phdr)
{
    PaxSpecialData *special;
    LocationIndex start = phdr->meta_offset;
    LocationIndex end;

    Assert(PaxOffsetIsValid(start));

    special = (PaxSpecialData *) PageGetSpecialPointer(page);
    if (special->n_attrs > 0 && PaxOffsetIsValid(phdr->offsets[0]))
        end = phdr->offsets[0];
    else
        end = ((PageHeader) page)->pd_lower;

    if (end < start)
        elog(ERROR, "pax: invalid transaction metadata region");
    return (Size) (end - start);
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

    if (hdr->pd_lower > hdr->pd_upper ||
        len > (Size) (hdr->pd_upper - hdr->pd_lower))
        elog(ERROR, "pax: page has no space for %zu bytes", len);

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

static bool
pax_meta_satisfies_snapshot(const PaxTupleMetaData *meta, Snapshot snapshot)
{
    bool self_snapshot;

    if (snapshot == NULL)
        elog(ERROR, "pax: cannot test tuple visibility without a snapshot");

    if (snapshot->snapshot_type == SNAPSHOT_ANY)
        return true;

    self_snapshot = snapshot->snapshot_type == SNAPSHOT_SELF;
    if (snapshot->snapshot_type != SNAPSHOT_MVCC && !self_snapshot)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support snapshot type %d",
                        (int) snapshot->snapshot_type)));

    if (snapshot->snapshot_type == SNAPSHOT_MVCC)
        Assert(snapshot->regd_count > 0 || snapshot->active_count > 0);

    if (meta->xmin == FrozenTransactionId ||
        meta->xmin == BootstrapTransactionId)
    {
        /* frozen/bootstrap inserting transaction is visible */
    }
    else if (TransactionIdIsCurrentTransactionId(meta->xmin))
    {
        if (!self_snapshot && meta->cmin >= snapshot->curcid)
            return false;       /* inserted by the current command */
    }
    else if (self_snapshot)
    {
        if (TransactionIdIsInProgress(meta->xmin) ||
            !TransactionIdDidCommit(meta->xmin))
            return false;
    }
    else
    {
        if (XidInMVCCSnapshot(meta->xmin, snapshot))
            return false;       /* inserter still in progress */
        if (!TransactionIdDidCommit(meta->xmin))
            return false;       /* inserter aborted */
    }

    if (!TransactionIdIsValid(meta->xmax))
        return true;

    if (TransactionIdIsCurrentTransactionId(meta->xmax))
        return self_snapshot ? false : meta->cmax >= snapshot->curcid;

    if (self_snapshot)
    {
        if (TransactionIdIsInProgress(meta->xmax) ||
            !TransactionIdDidCommit(meta->xmax))
            return true;
        return false;
    }

    if (XidInMVCCSnapshot(meta->xmax, snapshot))
        return true;            /* deleter still in progress */
    if (!TransactionIdDidCommit(meta->xmax))
        return true;            /* deleter aborted */

    return false;               /* deleter committed */
}

static int
pax_find_visible_tuple(PaxScanDesc scan, PaxPageDesc *pdesc, int start)
{
    int tupno;

    for (tupno = start; tupno < pdesc->n_tuples; tupno++)
    {
        CHECK_FOR_INTERRUPTS();
        if (pax_meta_satisfies_snapshot(&pdesc->tuple_meta[tupno],
                                        scan->rs_base.rs_snapshot))
            return tupno;
    }

    return -1;
}

static void
pax_store_tuple_slot(Relation rel, TupleTableSlot *slot, Buffer buffer,
                     PaxPageDesc *pdesc, int tupno, bool owns_pdesc)
{
    PaxTupleTableSlot *pslot = (PaxTupleTableSlot *) slot;

    pslot->buffer = buffer;
    IncrBufferRefCount(buffer);
    pslot->page = BufferGetPage(buffer);
    pslot->pdesc = pdesc;
    pslot->tupno = tupno;
    pslot->owns_pdesc = owns_pdesc;
    pslot->has_tuple_meta = true;
    pslot->tuple_meta = pdesc->tuple_meta[tupno];

    slot->tts_flags &= ~TTS_FLAG_EMPTY;
    slot->tts_nvalid = 0;
    slot->tts_tableOid = RelationGetRelid(rel);
    ItemPointerSet(&slot->tts_tid,
                   BufferGetBlockNumber(buffer),
                   (OffsetNumber) (tupno + 1));
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
    slot->owns_pdesc = false;
    slot->has_tuple_meta = false;
    memset(&slot->tuple_meta, 0, sizeof(slot->tuple_meta));
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
    PaxTupleTableSlot *slot = (PaxTupleTableSlot *) base;

    if (!slot->has_tuple_meta)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pax tuple slot has no transaction metadata")));

    *isnull = false;
    switch (attnum)
    {
        case MinTransactionIdAttributeNumber:
            return TransactionIdGetDatum(slot->tuple_meta.xmin);
        case MinCommandIdAttributeNumber:
            return CommandIdGetDatum(slot->tuple_meta.cmin);
        case MaxTransactionIdAttributeNumber:
            return TransactionIdGetDatum(slot->tuple_meta.xmax);
        case MaxCommandIdAttributeNumber:
            /* Heap exposes the raw shared command field when xmax is absent. */
            return CommandIdGetDatum(TransactionIdIsValid(slot->tuple_meta.xmax) ?
                                     slot->tuple_meta.cmax :
                                     slot->tuple_meta.cmin);
        default:
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("pax table AM does not support system column %d",
                            attnum)));
            return (Datum) 0;       /* keep compiler quiet */
    }
}

static bool
pax_slot_is_current_xact_tuple(TupleTableSlot *base)
{
    PaxTupleTableSlot *slot = (PaxTupleTableSlot *) base;

    return slot->has_tuple_meta &&
        slot->tuple_meta.xmin != FrozenTransactionId &&
        TransactionIdIsCurrentTransactionId(slot->tuple_meta.xmin);
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

    if (slot->owns_pdesc && slot->pdesc != NULL)
        pax_free_page_desc(slot->pdesc);

    if (BufferIsValid(slot->buffer))
    {
        ReleaseBuffer(slot->buffer);
        slot->buffer = InvalidBuffer;
    }

    /*
     * A scan owns its descriptor and the slot only borrows it.  A descriptor
     * obtained by tuple_fetch_row_version(), however, is owned by the slot.
     */
    slot->page   = NULL;
    slot->pdesc  = NULL;
    slot->tupno  = -1;
    slot->owns_pdesc = false;
    slot->has_tuple_meta = false;
    memset(&slot->tuple_meta, 0, sizeof(slot->tuple_meta));

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

    if (slot->owns_pdesc && slot->pdesc != NULL)
        pax_free_page_desc(slot->pdesc);

    /* la ligne ne dépend plus de la page : on peut rendre le pin */
    if (BufferIsValid(slot->buffer))
    {
        ReleaseBuffer(slot->buffer);
        slot->buffer = InvalidBuffer;
    }
    slot->page   = NULL;
    slot->pdesc  = NULL;
    slot->tupno  = -1;
    slot->owns_pdesc = false;
    /* has_tuple_meta and tuple_meta deliberately survive materialization */
}

static void
pax_slot_copyslot(TupleTableSlot *dstbase, TupleTableSlot *srcbase)
{
    TupleDesc   desc = srcbase->tts_tupleDescriptor;
    PaxTupleTableSlot *dstslot = (PaxTupleTableSlot *) dstbase;
    PaxTupleTableSlot *srcslot = (PaxTupleTableSlot *) srcbase;
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
    if (srcbase->tts_ops == &TTSOpsPax)
    {
        dstslot->has_tuple_meta = srcslot->has_tuple_meta;
        dstslot->tuple_meta = srcslot->tuple_meta;
    }

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

    if (pscan != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support parallel table scans")));

    RelationIncrementReferenceCount(rel);

    scan = (PaxScanDesc) palloc0(sizeof(PaxScanDescData));
    scan->rs_base.rs_rd        = rel;
    scan->rs_base.rs_snapshot  = snapshot;
    scan->rs_base.rs_nkeys     = nkeys;
    scan->rs_base.rs_flags     = flags;
    if (nkeys > 0 && key != NULL)
    {
        scan->rs_base.rs_key = palloc_array(ScanKeyData, nkeys);
        memcpy(scan->rs_base.rs_key, key, sizeof(ScanKeyData) * nkeys);
    }
    else
        scan->rs_base.rs_key = NULL;

    scan->current_buf    = InvalidBuffer;
    scan->current_pdesc  = NULL;
    scan->current_tupno  = -1;
    scan->current_block  = 0;
    scan->nblocks        = RelationGetNumberOfBlocks(rel);

    if (flags & SO_TYPE_SEQSCAN)
        PredicateLockRelation(rel, snapshot);

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

    if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
        UnregisterSnapshot(scan->rs_base.rs_snapshot);

    RelationDecrementReferenceCount(scan->rs_base.rs_rd);
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
    scan->nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

    if (scan->rs_base.rs_key)
        pfree(scan->rs_base.rs_key);
    if (key && scan->rs_base.rs_nkeys > 0)
    {
        scan->rs_base.rs_key = palloc_array(ScanKeyData,
                                            scan->rs_base.rs_nkeys);
        memcpy(scan->rs_base.rs_key, key,
               sizeof(ScanKeyData) * scan->rs_base.rs_nkeys);
    }
    else
        scan->rs_base.rs_key = NULL;
}

static bool
pax_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                     TupleTableSlot *slot)
{
    PaxScanDesc  scan = (PaxScanDesc) sscan;
    Relation     rel = scan->rs_base.rs_rd;
    TupleDesc    tupdesc = RelationGetDescr(rel);
    Buffer       buf;
    Page         page;
    PaxPageDesc *pdesc;
    int          tupno;

    /* The previous slot may borrow scan->current_pdesc, so clear it first. */
    ExecClearTuple(slot);

    if (BufferIsValid(scan->current_buf))
    {
        pdesc = scan->current_pdesc;
        tupno = pax_find_visible_tuple(scan, pdesc,
                                       scan->current_tupno + 1);
        if (tupno >= 0)
        {
            scan->current_tupno = tupno;
            pax_store_tuple_slot(rel, slot, scan->current_buf, pdesc,
                                 tupno, false);
            return true;
        }

        ReleaseBuffer(scan->current_buf);
        scan->current_buf = InvalidBuffer;
        pax_free_page_desc(scan->current_pdesc);
        scan->current_pdesc = NULL;
    }

    while (scan->current_block < scan->nblocks)
    {
        CHECK_FOR_INTERRUPTS();

        buf = ReadBuffer(rel, scan->current_block);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        if (!pax_page_is_valid(page))
        {
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 scan->current_block, RelationGetRelationName(rel));
        }

        pdesc = pax_build_page_desc(page, tupdesc);
        tupno = pax_find_visible_tuple(scan, pdesc, 0);

        /*
         * The descriptor's transaction metadata is copied, but user values
         * still borrow the page.  Keep the existing pin-based lazy design for
         * now; the remaining concurrent page-move race is tracked separately.
         */
        UnlockBuffer(buf);

        if (tupno >= 0)
        {
            scan->current_buf = buf;
            scan->current_pdesc = pdesc;
            scan->current_tupno = tupno;
            scan->current_block++;
            pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
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
 * - stocke xmin/cmin dans la région de métadonnées,
 * - fait grandir la région de chaque colonne ([bitmap][valeurs]) via des
 *   memmove vers la droite (pd_lower montante),
 * - pose le bit de NULL dans le bitmap de chaque colonne.
 *
 * Reste à faire : Generic WAL, gestion FSM, versions UPDATE/DELETE et
 * vacuum.
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
    TransactionId   xmin;
    PaxTupleMetaData *meta;
    Size             needed;
    Size             available;
    Size             meta_at;
    Size             meta_size;

    if (IsParallelWorker())
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support parallel inserts")));
    if (cid == InvalidCommandId)
        elog(ERROR, "pax: invalid insert command ID");

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

    /*
     * Store the current subtransaction XID, not the top-level XID.  A rollback
     * to a savepoint must therefore make this physical row permanently
     * invisible even if the outer transaction eventually commits.
     */
    xmin = GetCurrentTransactionId();
    if (options & TABLE_INSERT_FROZEN)
        xmin = FrozenTransactionId;

    /* Coarse relation-level SSI check; PAX currently has only sequential scans. */
    CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);

    /* === 1. Choisir une page ayant assez de place (premier libre) === */
    nblocks = RelationGetNumberOfBlocks(rel);

    for (blk = 0; blk < nblocks; blk++)
    {
        Buffer          b;
        Page            p;
        PaxSpecialData *special;
        PaxPageHeader  *h;

        CHECK_FOR_INTERRUPTS();
        b = ReadBuffer(rel, blk);

        LockBuffer(b, BUFFER_LOCK_EXCLUSIVE);
        p = BufferGetPage(b);

        if (!pax_page_is_valid(p))
        {
            if (PageIsNew(p))
                pax_page_init(p, natts);
            else
            {
                UnlockReleaseBuffer(b);
                elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                     blk, RelationGetRelationName(rel));
            }
        }

        special = (PaxSpecialData *) PageGetSpecialPointer(p);
        if (special->n_attrs != natts)
        {
            UnlockReleaseBuffer(b);
            elog(ERROR, "pax: attribute count mismatch (page %u vs tupdesc %d)",
                 special->n_attrs, natts);
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
        page = BufferGetPage(buf);
        pax_page_init(page, natts);
        needed = pax_insert_space_needed(rel, values, isnulls, 0);
        available = pax_page_free_space(page);
        if (available < needed)
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pax: row is too large for one page"),
                     errdetail("The row needs %zu bytes but a new PAX page has only %zu bytes.",
                               needed, available)));
        }
    }

    page  = BufferGetPage(buf);
    pghdr = (PageHeader) page;
    phdr  = PaxPageHeaderPtr(page);
    tupno = phdr->n_tuples;

    if (tupno >= MaxOffsetNumber)
    {
        UnlockReleaseBuffer(buf);
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pax: too many tuples on page %u",
                        BufferGetBlockNumber(buf))));
    }

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

    /* === 3. Transaction metadata: one fixed record per tuple === */
    if (!PaxOffsetIsValid(phdr->meta_offset))
        elog(ERROR, "pax: missing transaction metadata region");
    meta_size = pax_meta_region_size(page, phdr);
    if (meta_size != (Size) tupno * SizeOfPaxTupleMetaData)
        elog(ERROR, "pax: inconsistent transaction metadata region");

    meta_at = (Size) phdr->meta_offset +
        (Size) tupno * SizeOfPaxTupleMetaData;
    pax_insert_bytes(page, phdr, natts, PAX_NO_REGION, meta_at,
                     SizeOfPaxTupleMetaData);
    meta = ((PaxTupleMetaData *) ((char *) page + phdr->meta_offset)) + tupno;
    memset(meta, 0, SizeOfPaxTupleMetaData);
    meta->xmin = xmin;
    meta->xmax = InvalidTransactionId;
    meta->cmin = cid;
    meta->cmax = InvalidCommandId;

    /* === 4. Une région par colonne : [bitmap de NULL][valeurs] === */
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
        want = pax_bitmap_size(tupno + 1);
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

    /* === 5. Finaliser === */
    phdr->n_tuples++;
    phdr->free_space = (uint16) pax_page_free_space(page);

    /* TID : offset de TID = tupno + 1 (les offsets de TID débutent à 1) */
    ItemPointerSet(&(slot->tts_tid), BufferGetBlockNumber(buf),
                   (OffsetNumber) (tupno + 1));
    slot->tts_tableOid = RelationGetRelid(rel);
    if (slot->tts_ops == &TTSOpsPax)
    {
        PaxTupleTableSlot *pslot = (PaxTupleTableSlot *) slot;

        pslot->has_tuple_meta = true;
        pslot->tuple_meta = *meta;
    }

    /* TODO: Generic WAL */

    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);

    pfree(values);
    pfree(isnulls);
    pfree(voffs);
}

/* ------------------------------------------------------------------ */
/* Tuple lookup / MVCC callbacks                                       */
/* ------------------------------------------------------------------ */

static bool
pax_tuple_fetch_row_version(Relation rel, ItemPointer tid,
                            Snapshot snapshot, TupleTableSlot *slot)
{
    BlockNumber  blkno;
    OffsetNumber offset;
    int          tupno;
    Buffer       buf;
    Page         page;
    PaxPageDesc *pdesc;
    bool         visible;

    ExecClearTuple(slot);

    if (!ItemPointerIsValid(tid))
        return false;

    blkno = ItemPointerGetBlockNumber(tid);
    offset = ItemPointerGetOffsetNumber(tid);
    if (!OffsetNumberIsValid(offset) ||
        blkno >= RelationGetNumberOfBlocks(rel))
        return false;

    buf = ReadBuffer(rel, blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);

    if (!pax_page_is_valid(page))
    {
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: invalid page %u in relation \"%s\"",
             blkno, RelationGetRelationName(rel));
    }

    pdesc = pax_build_page_desc(page, RelationGetDescr(rel));
    tupno = (int) offset - 1;
    visible = tupno < pdesc->n_tuples &&
        pax_meta_satisfies_snapshot(&pdesc->tuple_meta[tupno], snapshot);

    if (visible)
        pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, true);
    else
        pax_free_page_desc(pdesc);

    UnlockReleaseBuffer(buf);
    return visible;
}

static bool
pax_tuple_tid_valid(TableScanDesc sscan, ItemPointer tid)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;
    OffsetNumber offset;

    if (!ItemPointerIsValid(tid))
        return false;

    offset = ItemPointerGetOffsetNumber(tid);
    return OffsetNumberIsValid(offset) &&
        ItemPointerGetBlockNumber(tid) < scan->nblocks;
}

static void
pax_tuple_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    if (!pax_tuple_tid_valid(sscan, tid))
        elog(ERROR, "pax: invalid tuple identifier");
    /* PAX has no update-version chain yet, so this TID is already latest. */
}

static bool
pax_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
                              Snapshot snapshot)
{
    PaxTupleTableSlot *pslot = (PaxTupleTableSlot *) slot;

    (void) rel;
    if (!pslot->has_tuple_meta)
        elog(ERROR, "pax tuple slot has no transaction metadata");

    return pax_meta_satisfies_snapshot(&pslot->tuple_meta, snapshot);
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

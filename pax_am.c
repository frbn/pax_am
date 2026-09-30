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
 *   - versions UPDATE / DELETE append-only, sans VACUUM ni gel des tuples
 *   - Generic WAL couvre les mutations de page ; l'insertion spéculative reste
 *     non supportée
 *   - pas d'index, de parallélisme ou d'opérations DDL non réécrivantes
 *
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/generic_xlog.h"
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

/*
 * 1 = format initial
 * 2 = introduction des colonnes en régions
 * 3 = métadonnées de version séparées (32 o) + maillons t_ctid
 * 4 = slots packés sans suralignement (pas = attlen, ou 2 pour un varlena).
 *     Une page v3 serait relue avec un pas faux : les versions 3 et 4 sont
 *     incompatibles, pas seulement différentes.
 */
#define PAX_PAGE_VERSION            4
#define PAX_SPECIAL_MAGIC           0x5041 /* "PA" */

#define PAX_FLAG_HAS_NULLS          0x0001
#define PAX_FLAG_HAS_VARLENA        0x0002
#define PAX_FLAG_COMPRESSED         0x0004
#define PAX_FLAG_HAS_XMIN_XMAX      0x0008
#define PAX_FLAG_HAS_VERSIONS       0x0010

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
    ItemPointerData t_ctid;     /* physical successor, or self for a leaf */
    uint16        flags;       /* reserved; must be zero in page version 3 */
    MultiXactId   locker_mxid; /* lock-only members; never an updater */
    uint32        reserved2;   /* explicit tail padding; must be zero */
} PaxTupleMetaData;

#define SizeOfPaxTupleMetaData   MAXALIGN(sizeof(PaxTupleMetaData))

StaticAssertDecl(sizeof(PaxTupleMetaData) == SizeOfPaxTupleMetaData,
                 "PAX tuple metadata must have a fixed on-disk size");
StaticAssertDecl(SizeOfPaxTupleMetaData == 32,
                 "PAX version-3 tuple metadata must be exactly 32 bytes");
StaticAssertDecl(offsetof(PaxTupleMetaData, t_ctid) == 16,
                 "PAX tuple metadata transaction fields must occupy 16 bytes");

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
    char       *scratch;        /* copie de travail pour les lectures par
                                 * référence : les régions ne sont pas
                                 * alignées pour leur type, on ne fait donc
                                 * jamais d'accès direct déaligné */
    int16       attlen;         /* longueur fixe ou -1 */
    Size        stride;         /* pas d'un slot = attlen, ou 2 pour varlena */
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

    int             current_tupno;   /* last returned tuple index on block */
    BlockNumber     current_block;
    BlockNumber     nblocks;
    ItemPointerData tidrange_max;
    bool            tidrange_done;

    /*
     * Pin on the page being scanned, held across getnextslot calls exactly
     * like heap's rs_cbuf.  PaxPageDesc stores raw pointers into the page
     * (mp->data, desc->header), so the page must stay pinned for as long as a
     * descriptor derived from it can be in use.
     *
     * The content lock is deliberately NOT held here: it is taken and dropped
     * inside each call, after the tuple has been materialized.  Holding a
     * share content lock across calls would self-deadlock an UPDATE or DELETE
     * that needs to modify a tuple on this very block.
     */
    Buffer          current_buf;
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
static Size         pax_region_used(int n_tuples, Size stride);
static Size         pax_bitmap_size(int n_tuples);
static Size         pax_insert_space_needed(Relation rel, Datum *values,
                                            bool *isnulls, int tupno,
                                            PaxPageHeader *phdr);
static OffsetNumber pax_alloc_payload(Page page, Datum value, int16 attlen);
static Size         pax_region_size(Page page, PaxPageHeader *phdr,
                                    int n_attrs, int i);
static Size         pax_page_free_space(Page page);
static Size         pax_meta_region_size(Page page, PaxPageHeader *phdr);
static void         pax_insert_bytes(Page page, PaxPageHeader *phdr,
                                     int n_attrs, int region_idx,
                                     Size at, Size len);
static bool         pax_meta_xmin_visible(const PaxTupleMetaData *meta,
                                          Snapshot snapshot);
static bool         pax_meta_satisfies_snapshot(const PaxTupleMetaData *meta,
                                                Snapshot snapshot);
static TransactionId pax_locker_xmax(const PaxTupleMetaData *meta);
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
static void         pax_scan_set_tidrange(TableScanDesc sscan,
                                           ItemPointer mintid,
                                           ItemPointer maxtid);
static bool         pax_scan_getnextslot_tidrange(TableScanDesc sscan,
                                                  ScanDirection direction,
                                                  TupleTableSlot *slot);
static void         pax_scan_unpin_current(PaxScanDesc scan);
static Buffer       pax_scan_lock_current_page(PaxScanDesc scan, Relation rel);

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

/* Versioned UPDATE / DELETE / row locking */
static TM_Result    pax_tuple_delete(Relation rel, ItemPointer tid,
                                     CommandId cid, uint32 options,
                                     Snapshot snapshot, Snapshot crosscheck,
                                     bool wait, TM_FailureData *tmfd);
static TM_Result    pax_tuple_update(Relation rel, ItemPointer otid,
                                     TupleTableSlot *slot, CommandId cid,
                                     uint32 options, Snapshot snapshot,
                                     Snapshot crosscheck, bool wait,
                                     TM_FailureData *tmfd,
                                     LockTupleMode *lockmode,
                                     TU_UpdateIndexes *update_indexes);
static TM_Result    pax_tuple_lock(Relation rel, ItemPointer tid,
                                   Snapshot snapshot, TupleTableSlot *slot,
                                   CommandId cid, LockTupleMode mode,
                                   LockWaitPolicy wait_policy, uint8 flags,
                                   TM_FailureData *tmfd);

/* Required PG19 callbacks that are not implemented yet. */
static void         pax_report_unsupported(const char *feature);
static Size         pax_parallelscan_estimate(Relation rel);
static Size         pax_parallelscan_initialize(Relation rel,
                                                ParallelTableScanDesc pscan);
static void         pax_parallelscan_reinitialize(Relation rel,
                                                   ParallelTableScanDesc pscan);
static struct IndexFetchTableData *pax_index_fetch_begin(Relation rel,
                                                          uint32 flags);
static void         pax_index_fetch_reset(struct IndexFetchTableData *data);
static void         pax_index_fetch_end(struct IndexFetchTableData *data);
static bool         pax_index_fetch_tuple(struct IndexFetchTableData *scan,
                                          ItemPointer tid, Snapshot snapshot,
                                          TupleTableSlot *slot,
                                          bool *call_again, bool *all_dead);
static TransactionId pax_index_delete_tuples(Relation rel,
                                             TM_IndexDeleteOp *delstate);
static void         pax_tuple_insert_speculative(Relation rel,
                                                 TupleTableSlot *slot,
                                                 CommandId cid, uint32 options,
                                                 BulkInsertState bistate,
                                                 uint32 specToken);
static void         pax_tuple_complete_speculative(Relation rel,
                                                   TupleTableSlot *slot,
                                                   uint32 specToken,
                                                   bool succeeded);
static void         pax_multi_insert(Relation rel, TupleTableSlot **slots,
                                     int nslots, CommandId cid, uint32 options,
                                     BulkInsertState bistate);
static void         pax_relation_copy_data(Relation rel,
                                            const RelFileLocator *newrlocator);
static void         pax_relation_copy_for_cluster(Relation oldtable,
                                                  Relation newtable,
                                                  Relation oldindex,
                                                  bool use_sort,
                                                  TransactionId oldestxmin,
                                                  TransactionId *xid_cutoff,
                                                  MultiXactId *multi_cutoff,
                                                  double *num_tuples,
                                                  double *tups_vacuumed,
                                                  double *tups_recently_dead);
static void         pax_relation_vacuum(Relation rel,
                                         const VacuumParams *params,
                                         BufferAccessStrategy bstrategy);
static bool         pax_scan_analyze_next_block(TableScanDesc scan,
                                                ReadStream *stream);
static bool         pax_scan_analyze_next_tuple(TableScanDesc scan,
                                                double *liverows,
                                                double *deadrows,
                                                TupleTableSlot *slot);
static double        pax_index_build_range_scan(Relation table_rel,
                                                 Relation index_rel,
                                                 IndexInfo *index_info,
                                                 bool allow_sync,
                                                 bool anyvisible,
                                                 bool progress,
                                                 BlockNumber start_blockno,
                                                 BlockNumber numblocks,
                                                 IndexBuildCallback callback,
                                                 void *callback_state,
                                                 TableScanDesc scan);
static void         pax_index_validate_scan(Relation table_rel,
                                            Relation index_rel,
                                            IndexInfo *index_info,
                                            Snapshot snapshot,
                                            ValidateIndexState *state);
static bool         pax_scan_sample_next_block(TableScanDesc scan,
                                               SampleScanState *scanstate);
static bool         pax_scan_sample_next_tuple(TableScanDesc scan,
                                               SampleScanState *scanstate,
                                               TupleTableSlot *slot);

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
/* Required callbacks not implemented by this prototype               */
/* ------------------------------------------------------------------ */

static void
pax_report_unsupported(const char *feature)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pax table AM does not support %s", feature)));
}

static Size
pax_parallelscan_estimate(Relation rel)
{
    pax_report_unsupported("parallel table scans");
    return 0;
}

static Size
pax_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
    pax_report_unsupported("parallel table scans");
    return 0;
}

static void
pax_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
    pax_report_unsupported("parallel table scans");
}

static struct IndexFetchTableData *
pax_index_fetch_begin(Relation rel, uint32 flags)
{
    pax_report_unsupported("index scans");
    return NULL;
}

static void
pax_index_fetch_reset(struct IndexFetchTableData *data)
{
    pax_report_unsupported("index scans");
}

static void
pax_index_fetch_end(struct IndexFetchTableData *data)
{
    pax_report_unsupported("index scans");
}

static bool
pax_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid,
                      Snapshot snapshot, TupleTableSlot *slot,
                      bool *call_again, bool *all_dead)
{
    pax_report_unsupported("index scans");
    return false;
}

static TransactionId
pax_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    pax_report_unsupported("index tuple deletion");
    return InvalidTransactionId;
}

static void
pax_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
                             CommandId cid, uint32 options,
                             BulkInsertState bistate, uint32 specToken)
{
    pax_report_unsupported("speculative insertion");
}

static void
pax_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
                               uint32 specToken, bool succeeded)
{
    pax_report_unsupported("speculative insertion");
}

static void
pax_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
                 CommandId cid, uint32 options, BulkInsertState bistate)
{
    int i;

    for (i = 0; i < nslots; i++)
        pax_tuple_insert(rel, slots[i], cid, options, bistate);
}

static void
pax_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
    pax_report_unsupported("physical relation copying");
}

static void
pax_relation_copy_for_cluster(Relation oldtable, Relation newtable,
                              Relation oldindex, bool use_sort,
                              TransactionId oldestxmin,
                              TransactionId *xid_cutoff,
                              MultiXactId *multi_cutoff,
                              double *num_tuples, double *tups_vacuumed,
                              double *tups_recently_dead)
{
    pax_report_unsupported("CLUSTER");
}

static void
pax_relation_vacuum(Relation rel, const VacuumParams *params,
                     BufferAccessStrategy bstrategy)
{
    pax_report_unsupported("VACUUM");
}

static bool
pax_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
    pax_report_unsupported("ANALYZE table scans");
    return false;
}

static bool
pax_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
                            double *deadrows, TupleTableSlot *slot)
{
    pax_report_unsupported("ANALYZE table scans");
    return false;
}

static double
pax_index_build_range_scan(Relation table_rel, Relation index_rel,
                           IndexInfo *index_info, bool allow_sync,
                           bool anyvisible, bool progress,
                           BlockNumber start_blockno, BlockNumber numblocks,
                           IndexBuildCallback callback, void *callback_state,
                           TableScanDesc scan)
{
    pax_report_unsupported("index builds");
    return 0;
}

static void
pax_index_validate_scan(Relation table_rel, Relation index_rel,
                        IndexInfo *index_info, Snapshot snapshot,
                        ValidateIndexState *state)
{
    pax_report_unsupported("index validation");
}

static bool
pax_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
    pax_report_unsupported("TABLESAMPLE");
    return false;
}

static bool
pax_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
                           TupleTableSlot *slot)
{
    pax_report_unsupported("TABLESAMPLE");
    return false;
}

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
    .scan_set_tidrange = pax_scan_set_tidrange,
    .scan_getnextslot_tidrange = pax_scan_getnextslot_tidrange,

    /* Parallel scans are rejected with an explicit error. */
    .parallelscan_estimate = pax_parallelscan_estimate,
    .parallelscan_initialize = pax_parallelscan_initialize,
    .parallelscan_reinitialize = pax_parallelscan_reinitialize,

    /* Index fetch is unsupported. */
    .index_fetch_begin = pax_index_fetch_begin,
    .index_fetch_reset = pax_index_fetch_reset,
    .index_fetch_end = pax_index_fetch_end,
    .index_fetch_tuple = pax_index_fetch_tuple,

    /* Tuple lookup / MVCC visibility */
    .tuple_fetch_row_version = pax_tuple_fetch_row_version,
    .tuple_tid_valid = pax_tuple_tid_valid,
    .tuple_get_latest_tid = pax_tuple_get_latest_tid,
    .tuple_satisfies_snapshot = pax_tuple_satisfies_snapshot,
    .index_delete_tuples = pax_index_delete_tuples,

    /* Insert and versioned DML */
    .tuple_insert   = pax_tuple_insert,
    .tuple_insert_speculative = pax_tuple_insert_speculative,
    .tuple_complete_speculative = pax_tuple_complete_speculative,
    .multi_insert   = pax_multi_insert,
    .tuple_delete   = pax_tuple_delete,
    .tuple_update   = pax_tuple_update,
    .tuple_lock     = pax_tuple_lock,

    /* DDL / stockage — (pour CREATE TABLE / TRUNCATE / planner) */
    .relation_set_new_filelocator = pax_relation_set_new_filelocator,
    .relation_nontransactional_truncate = pax_relation_nontransactional_truncate,
    .relation_copy_data = pax_relation_copy_data,
    .relation_copy_for_cluster = pax_relation_copy_for_cluster,
    .relation_vacuum = pax_relation_vacuum,

    /* ANALYZE and index-build scans are rejected explicitly. */
    .scan_analyze_next_block = pax_scan_analyze_next_block,
    .scan_analyze_next_tuple = pax_scan_analyze_next_tuple,
    .index_build_range_scan = pax_index_build_range_scan,
    .index_validate_scan = pax_index_validate_scan,

    .relation_size  = pax_relation_size,
    .relation_needs_toast_table = pax_relation_needs_toast_table,
    .relation_estimate_size = pax_relation_estimate_size,

    /* TABLESAMPLE is unsupported. */
    .scan_sample_next_block = pax_scan_sample_next_block,
    .scan_sample_next_tuple = pax_scan_sample_next_tuple,

    /* Optional callbacks remain NULL; all PG19-required slots are wired. */
};

static void
pax_validate_table_am_routine(void)
{
    if (pax_methods.scan_begin == NULL ||
        pax_methods.scan_end == NULL ||
        pax_methods.scan_rescan == NULL ||
        pax_methods.scan_getnextslot == NULL ||
        pax_methods.parallelscan_estimate == NULL ||
        pax_methods.parallelscan_initialize == NULL ||
        pax_methods.parallelscan_reinitialize == NULL ||
        pax_methods.index_fetch_begin == NULL ||
        pax_methods.index_fetch_reset == NULL ||
        pax_methods.index_fetch_end == NULL ||
        pax_methods.index_fetch_tuple == NULL ||
        pax_methods.tuple_fetch_row_version == NULL ||
        pax_methods.tuple_tid_valid == NULL ||
        pax_methods.tuple_get_latest_tid == NULL ||
        pax_methods.tuple_satisfies_snapshot == NULL ||
        pax_methods.index_delete_tuples == NULL ||
        pax_methods.tuple_insert == NULL ||
        pax_methods.tuple_insert_speculative == NULL ||
        pax_methods.tuple_complete_speculative == NULL ||
        pax_methods.multi_insert == NULL ||
        pax_methods.tuple_delete == NULL ||
        pax_methods.tuple_update == NULL ||
        pax_methods.tuple_lock == NULL ||
        pax_methods.relation_set_new_filelocator == NULL ||
        pax_methods.relation_nontransactional_truncate == NULL ||
        pax_methods.relation_copy_data == NULL ||
        pax_methods.relation_copy_for_cluster == NULL ||
        pax_methods.relation_vacuum == NULL ||
        pax_methods.scan_analyze_next_block == NULL ||
        pax_methods.scan_analyze_next_tuple == NULL ||
        pax_methods.index_build_range_scan == NULL ||
        pax_methods.index_validate_scan == NULL ||
        pax_methods.relation_size == NULL ||
        pax_methods.relation_needs_toast_table == NULL ||
        pax_methods.relation_estimate_size == NULL ||
        pax_methods.scan_sample_next_block == NULL ||
        pax_methods.scan_sample_next_tuple == NULL)
        elog(ERROR, "pax: incomplete PG19 table AM routine");
}

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
    special->flags    = PAX_FLAG_HAS_XMIN_XMAX | PAX_FLAG_HAS_VERSIONS;
    special->n_attrs  = (uint16) n_attrs;
    special->magic = PAX_SPECIAL_MAGIC;

    header_size = SizeOfPaxPageHeaderFixed + n_attrs * sizeof(OffsetNumber);
    header_size = MAXALIGN(header_size);

    phdr = (PaxPageHeader *) ((char *) page + SizeOfPageHeaderData + SizeOfPaxSpecialData);

    phdr->n_tuples   = 0;
    phdr->free_space = (uint16) (BLCKSZ - (SizeOfPageHeaderData + SizeOfPaxSpecialData + header_size));
    phdr->flags      = PAX_FLAG_HAS_XMIN_XMAX | PAX_FLAG_HAS_VERSIONS;

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
            (special->flags & PAX_FLAG_HAS_XMIN_XMAX) != 0 &&
            (special->flags & PAX_FLAG_HAS_VERSIONS) != 0);
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

    if ((special->flags & PAX_FLAG_HAS_XMIN_XMAX) == 0 ||
        (special->flags & PAX_FLAG_HAS_VERSIONS) == 0)
        elog(ERROR, "pax: page lacks required transaction/version metadata");

    phdr = PaxPageHeaderPtr(page);

    desc = (PaxPageDesc *) palloc0(sizeof(PaxPageDesc));
    desc->page     = page;
    desc->special  = special;
    desc->header   = phdr;
    desc->n_attrs  = special->n_attrs;
    desc->n_tuples = phdr->n_tuples;
    desc->ctx      = CurrentMemoryContext;

    if ((phdr->flags & PAX_FLAG_HAS_XMIN_XMAX) == 0 ||
        (phdr->flags & PAX_FLAG_HAS_VERSIONS) == 0)
        elog(ERROR, "pax: page header lacks required transaction/version metadata");

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
            if (!ItemPointerIsValid(&meta->t_ctid) ||
                !OffsetNumberIsValid(ItemPointerGetOffsetNumber(&meta->t_ctid)))
                elog(ERROR, "pax: tuple %d has invalid t_ctid", i);
            if (meta->flags != 0)
                elog(ERROR, "pax: tuple %d has unknown metadata flags %#x",
                     i, meta->flags);
            if (meta->reserved2 != 0)
                elog(ERROR, "pax: tuple %d has nonzero metadata padding",
                     i);
        }
    }

    desc->minipages = (PaxMinipage *) palloc0(sizeof(PaxMinipage) * desc->n_attrs);

    for (i = 0; i < desc->n_attrs; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        PaxMinipage      *mp   = &desc->minipages[i];
        OffsetNumber      off  = phdr->offsets[i];
        MemoryContext     oldc;
        Size              stride;
        Size              rsize;
        Size              used;
        Size              bmp;

        mp->attlen     = attr->attlen;
        mp->attalign   = attr->attalign;
        mp->is_varlena = (attr->attlen < 0);
        mp->has_nulls  = false;     /* true si un bitmap est présent */
        mp->null_bitmap = NULL;
        mp->n_values   = phdr->n_tuples;
        mp->stride     = pax_slot_stride(attr);
        mp->scratch    = NULL;

        if (!PaxOffsetIsValid(off))
        {
            /* colonne jamais écrite sur cette page */
            mp->data = NULL;
            continue;
        }

        /* Tampon de recopie pour les colonnes de longueur fixe passées par
         * référence : elles sont rendues depuis le descripteur et non depuis
         * la page, pour ne jamais exposer une adresse déalignée. */
        if (attr->attlen > 0 && attr->attlen != 1 && attr->attlen != 2 &&
            attr->attlen != 4 && attr->attlen != 8)
        {
            oldc = MemoryContextSwitchTo(desc->ctx);
            mp->scratch = palloc(attr->attlen);
            MemoryContextSwitchTo(oldc);
        }

        stride = mp->stride;
        rsize  = pax_region_size(page, phdr, desc->n_attrs, i);
        used   = pax_region_used(desc->n_tuples, stride);

        /*
         * Le span jusqu'à la région suivante peut dépasser la taille utile :
         * chaque région s'ouvre alignée sur 8 octets, donc la fin de la
         * précédente laisse jusqu'à 7 octets de bourrage. Ce qui compte est
         * que le span puisse contenir la région.
         */
        if (rsize < used)
            elog(ERROR, "pax: inconsistent page layout for column %d "
                        "(span %zu, besoin %zu = bitmap %zu + %d tuples x %zu)",
                 i, rsize, used, pax_bitmap_size(desc->n_tuples),
                 desc->n_tuples, stride);

        bmp = pax_bitmap_size(desc->n_tuples);

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

    /* Longueur fixe : la valeur est lue par memcpy car le slot n'est pas
     * nécessairement aligné pour son type (les régions sont compactées au
     * byte près et repoussées par les insertions suivantes). */
    if (!mp->is_varlena)
    {
        ptr = mp->data + ((Size) tupno * mp->stride);

        switch (mp->attlen)
        {
            case 1:
            {
                char c;

                memcpy(&c, ptr, 1);
                return CharGetDatum(c);
            }
            case 2:
            {
                int16 v;

                memcpy(&v, ptr, sizeof(int16));
                return Int16GetDatum(v);
            }
            case 4:
            {
                int32 v;

                memcpy(&v, ptr, sizeof(int32));
                return Int32GetDatum(v);
            }
            case 8:
            {
                int64 v;

                memcpy(&v, ptr, sizeof(int64));
                return Int64GetDatum(v);
            }
            default:
                /* by-reference fixed (uuid, macaddr, …) : on recopie dans le
                 * descripteur plutôt que de pointer dans la page, pour ne pas
                 * exposer à l'exécuteur une adresse non alignée. */
                memcpy(mp->scratch, ptr, mp->attlen);
                return PointerGetDatum(mp->scratch);
        }
    }

    /* page de champs de longueur variable : table d'offsets, 2 octets par tuple */
    {
        OffsetNumber off;

        memcpy(&off, mp->data + ((Size) tupno * mp->stride),
               sizeof(OffsetNumber));

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
 * Pas d'un slot : la longueur exacte, sans suralignement.
 *
 * Un int4 occupe donc 4 octets et non 8, une OffsetNumber 2 et non 8.
 *
 * Aucune contrainte d'alignement n'est conservée : les régions sont
 * compactées au byte près et repoussées par les insertions suivantes, donc
 * un slot peut se retrouver à une adresse non alignée pour son type. La
 * lecture passe systématiquement par memcpy (voir pax_get_value), ce qui
 * reste correct et portable sur toute architecture.
 *
 * Les colonnes varlena stockent une OffsetNumber (offset absolu de la valeur
 * allouée en haut de page), soit 2 octets.
 */
static Size
pax_slot_stride(Form_pg_attribute attr)
{
    if (attr->attlen > 0)
        return (Size) attr->attlen;

    /* attlen -1 (varlena) ou -2 (cstring) : table d'offsets */
    return sizeof(OffsetNumber);
}

/* Taille utile d'une région : bitmap de NULL puis les slots. */
static Size
pax_region_used(int n_tuples, Size stride)
{
    return pax_bitmap_size(n_tuples) + (Size) n_tuples * stride;
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
 *
 * "phdr" permet de ne provisionner le bourrage d'alignement que pour les
 * colonnes réellement absentes de la page ; passer NULL revient à supposer
 * qu'aucune région n'existe encore (page neuve).
 */
static Size
pax_insert_space_needed(Relation rel, Datum *values, bool *isnulls,
                        int tupno, PaxPageHeader *phdr)
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
 * Insère "len" octets à la position absolue "at" en décalant tout ce qui
 * suit vers la droite.  Les régions qui commencent à "at" ou après sont
 * décalées, sauf celle qu'on remplit (region_idx).
 *
 * C'est ce décalage qui permet à une colonne de grandir sans quitter sa
 * région : les zones de haut niveau (bitmap de pd_upper) ne sont pas
 * touchées car on ne déplace que [at, pd_lower[.
 *
 * "len" n'est plus forcément multiple de 8 : insérer un slot vaut strlen,
 * soit 2 ou 4 octets. C'est sans conséquence, les lectures se faisant par
 * memcpy.
 */
static void
pax_insert_bytes(Page page, PaxPageHeader *phdr, int n_attrs,
                 int region_idx, Size at, Size len)
{
    PageHeader  hdr = (PageHeader) page;
    int         j;

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
pax_meta_xmin_visible(const PaxTupleMetaData *meta, Snapshot snapshot)
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
        return true;

    if (TransactionIdIsCurrentTransactionId(meta->xmin))
        return self_snapshot || meta->cmin < snapshot->curcid;

    if (self_snapshot)
        return !TransactionIdIsInProgress(meta->xmin) &&
            TransactionIdDidCommit(meta->xmin);

    if (XidInMVCCSnapshot(meta->xmin, snapshot))
        return false;
    return TransactionIdDidCommit(meta->xmin);
}

static bool
pax_meta_satisfies_snapshot(const PaxTupleMetaData *meta, Snapshot snapshot)
{
    if (!pax_meta_xmin_visible(meta, snapshot))
        return false;

    if (snapshot->snapshot_type == SNAPSHOT_ANY)
        return true;

    if (!TransactionIdIsValid(meta->xmax))
        return true;

    if (TransactionIdIsCurrentTransactionId(meta->xmax))
        return snapshot->snapshot_type == SNAPSHOT_SELF ?
            false : meta->cmax >= snapshot->curcid;

    if (snapshot->snapshot_type == SNAPSHOT_SELF)
    {
        if (TransactionIdIsInProgress(meta->xmax) ||
            !TransactionIdDidCommit(meta->xmax))
            return true;
        return false;
    }

    if (XidInMVCCSnapshot(meta->xmax, snapshot))
        return true;
    if (!TransactionIdDidCommit(meta->xmax))
        return true;

    return false;
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

/* see tupletable.h */

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
            return TransactionIdGetDatum(pax_locker_xmax(&slot->tuple_meta));
        case MaxCommandIdAttributeNumber:
            /* Heap exposes the shared command field when no update is active. */
            return CommandIdGetDatum(
                TransactionIdIsValid(slot->tuple_meta.xmax) &&
                (TransactionIdIsCurrentTransactionId(slot->tuple_meta.xmax) ||
                 TransactionIdIsInProgress(slot->tuple_meta.xmax)) ?
                slot->tuple_meta.cmax : slot->tuple_meta.cmin);
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

    (void) key;
    if ((flags & SO_TYPE_SEQSCAN) == 0 &&
        (flags & SO_TYPE_TIDSCAN) == 0 &&
        (flags & SO_TYPE_TIDRANGESCAN) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM supports only sequential and TID table scans")));
    if (nkeys != 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support scan keys")));
    if (pscan != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support parallel table scans")));

    RelationIncrementReferenceCount(rel);

    scan = (PaxScanDesc) palloc0(sizeof(PaxScanDescData));
    scan->rs_base.rs_rd        = rel;
    scan->rs_base.rs_snapshot  = snapshot;
    scan->rs_base.rs_nkeys     = 0;
    scan->rs_base.rs_flags     = flags;
    scan->rs_base.rs_key       = NULL;

    scan->current_tupno  = -1;
    scan->current_block  = 0;
    scan->nblocks        = RelationGetNumberOfBlocks(rel);
    scan->current_buf    = InvalidBuffer;
    ItemPointerSetInvalid(&scan->tidrange_max);
    scan->tidrange_done = false;

    PredicateLockRelation(rel, snapshot);

    return (TableScanDesc) scan;
}

static void
pax_scan_end(TableScanDesc sscan)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;

    pax_scan_unpin_current(scan);

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

    (void) key;
    pax_scan_unpin_current(scan);
    scan->current_tupno = -1;
    scan->current_block = 0;
    scan->nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);
    ItemPointerSetInvalid(&scan->tidrange_max);
    scan->tidrange_done = false;
}

/*
 * Drop the pin taken on the current page, if any.
 */
static void
pax_scan_unpin_current(PaxScanDesc scan)
{
    if (BufferIsValid(scan->current_buf))
    {
        ReleaseBuffer(scan->current_buf);
        scan->current_buf = InvalidBuffer;
    }
}

/*
 * Return scan->current_block pinned and content-locked for reading.
 *
 * When the previous call left a pin on this very block we reuse it and only
 * re-take the content lock; that is a pin-count lookup rather than a buffer
 * manager read, which is what removes the per-tuple ReadBuffer cost measured
 * in the shared-buffer comparison test.
 *
 * The caller owns the returned content lock and must drop it with
 * LockBuffer(buf, BUFFER_LOCK_UNLOCK) -- deliberately not
 * UnlockReleaseBuffer(), because the pin stays cached in the scan.
 */
static Buffer
pax_scan_lock_current_page(PaxScanDesc scan, Relation rel)
{
    if (BufferIsValid(scan->current_buf) &&
        BufferGetBlockNumber(scan->current_buf) == scan->current_block)
    {
        LockBuffer(scan->current_buf, BUFFER_LOCK_SHARE);
        return scan->current_buf;
    }

    pax_scan_unpin_current(scan);
    scan->current_buf = ReadBuffer(rel, scan->current_block);
    LockBuffer(scan->current_buf, BUFFER_LOCK_SHARE);

    return scan->current_buf;
}

static bool
pax_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                     TupleTableSlot *slot)
{
    PaxScanDesc  scan = (PaxScanDesc) sscan;
    Relation     rel = scan->rs_base.rs_rd;
    TupleDesc    tupdesc = RelationGetDescr(rel);

    if ((scan->rs_base.rs_flags & SO_TYPE_SEQSCAN) == 0)
        elog(ERROR, "pax: non-sequential scan used sequential callback");

    if (direction != ForwardScanDirection &&
        direction != NoMovementScanDirection)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support backward scans")));

    ExecClearTuple(slot);

    while (scan->current_block < scan->nblocks)
    {
        Buffer       buf;
        Page         page;
        PaxPageDesc *pdesc;
        int          tupno;

        CHECK_FOR_INTERRUPTS();

        buf = pax_scan_lock_current_page(scan, rel);
        page = BufferGetPage(buf);

        if (!pax_page_is_valid(page))
        {
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            pax_scan_unpin_current(scan);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 scan->current_block, RelationGetRelationName(rel));
        }

        /*
         * Rebuilt on every call even though the pin is reused: the content
         * lock was dropped since the last call, so a concurrent inserter may
         * have memmoved this page and invalidated cached column pointers.
         */
        pdesc = pax_build_page_desc(page, tupdesc);
        tupno = pax_find_visible_tuple(scan, pdesc,
                                       scan->current_tupno + 1);

        if (tupno >= 0)
        {
            /*
             * Materialize before dropping the content lock.  A pin prevents
             * eviction, but an inserter may still memmove this page while the
             * slot is live; copied values are therefore mandatory.
             */
            pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
            ExecMaterializeSlot(slot);
            pax_free_page_desc(pdesc);

            /*
             * Keep the pin, drop only the content lock, so that an UPDATE or
             * DELETE of this tuple can still take an exclusive lock.
             */
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);

            scan->current_tupno = tupno;
            return true;
        }

        pax_free_page_desc(pdesc);
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        pax_scan_unpin_current(scan);
        scan->current_tupno = -1;
        scan->current_block++;
    }

    pax_scan_unpin_current(scan);

    return false;
}

static void
pax_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
                      ItemPointer maxtid)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;

    pax_scan_unpin_current(scan);
    scan->tidrange_done = false;
    scan->current_tupno = -1;
    scan->current_block = 0;
    scan->nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

    if (ItemPointerIsValid(mintid))
    {
        scan->current_block = ItemPointerGetBlockNumber(mintid);
        if (OffsetNumberIsValid(ItemPointerGetOffsetNumber(mintid)))
            scan->current_tupno =
                (int) ItemPointerGetOffsetNumber(mintid) - 2;
    }

    if (ItemPointerIsValid(maxtid))
    {
        if (ItemPointerIsValid(mintid) &&
            ItemPointerCompare(mintid, maxtid) > 0)
        {
            scan->tidrange_done = true;
            return;
        }
        scan->tidrange_max = *maxtid;
    }
    else
        ItemPointerSetInvalid(&scan->tidrange_max);

    if (scan->current_block >= scan->nblocks)
        scan->tidrange_done = true;
}

static bool
pax_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
                              TupleTableSlot *slot)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;
    Relation    rel = scan->rs_base.rs_rd;
    TupleDesc   tupdesc = RelationGetDescr(rel);

    if (direction != ForwardScanDirection &&
        direction != NoMovementScanDirection)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support backward TID scans")));

    ExecClearTuple(slot);

    while (!scan->tidrange_done && scan->current_block < scan->nblocks)
    {
        Buffer       buf;
        Page         page;
        PaxPageDesc *pdesc;
        int          start = scan->current_tupno + 1;
        int          tupno;
        BlockNumber  max_block = InvalidBlockNumber;

        CHECK_FOR_INTERRUPTS();

        if (ItemPointerIsValid(&scan->tidrange_max))
        {
            max_block = ItemPointerGetBlockNumber(&scan->tidrange_max);
            if (scan->current_block > max_block)
            {
                scan->tidrange_done = true;
                break;
            }
            if (scan->current_block == max_block &&
                start > (int) ItemPointerGetOffsetNumber(&scan->tidrange_max))
            {
                scan->tidrange_done = true;
                break;
            }
        }

        buf = pax_scan_lock_current_page(scan, rel);
        page = BufferGetPage(buf);
        if (!pax_page_is_valid(page))
        {
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            pax_scan_unpin_current(scan);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 scan->current_block, RelationGetRelationName(rel));
        }

        pdesc = pax_build_page_desc(page, tupdesc);
        tupno = pax_find_visible_tuple(scan, pdesc, start);
        if (tupno >= 0 &&
            ItemPointerIsValid(&scan->tidrange_max) &&
            scan->current_block == max_block &&
            tupno + 1 > (int) ItemPointerGetOffsetNumber(&scan->tidrange_max))
            tupno = -1;

        if (tupno >= 0)
        {
            pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
            ExecMaterializeSlot(slot);
            pax_free_page_desc(pdesc);
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            scan->current_tupno = tupno;
            return true;
        }

        pax_free_page_desc(pdesc);
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        pax_scan_unpin_current(scan);
        scan->current_tupno = -1;
        if (ItemPointerIsValid(&scan->tidrange_max) &&
            scan->current_block == max_block)
            scan->tidrange_done = true;
        else
            scan->current_block++;
    }

    pax_scan_unpin_current(scan);

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
 * Chaque mutation est appliquée via Generic WAL avant que la page puisse
 * atteindre le disque.  Reste à faire : gestion FSM et vacuum.
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
    GenericXLogState *wal_state;
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
    bool             page_is_new = false;
    bool             extension_locked = false;

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
            {
                buf = b;
                page_is_new = true;
                break;
            }

            UnlockReleaseBuffer(b);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 blk, RelationGetRelationName(rel));
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
        needed = pax_insert_space_needed(rel, values, isnulls, h->n_tuples,
                                         PaxPageHeaderPtr(p));

        if (pax_page_free_space(p) >= needed)
        {
            buf = b;
            break;
        }

        UnlockReleaseBuffer(b);
    }

    if (!BufferIsValid(buf))
    {
        /*
         * P_NEW does not itself serialize relation extension.  The extension
         * lock prevents two PAX inserters from concurrently resolving the
         * same new block before either has initialized it.
         */
        LockRelationForExtension(rel, ExclusiveLock);
        extension_locked = true;
        buf = ReadBuffer(rel, P_NEW);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buf);
        if (!PageIsNew(page))
        {
            UnlockReleaseBuffer(buf);
            UnlockRelationForExtension(rel, ExclusiveLock);
            elog(ERROR, "pax: concurrent extension returned a non-new page");
        }
        page_is_new = true;
    }

    /*
     * Mutate a private page image.  GenericXLogFinish() applies it to the
     * shared buffer and emits the WAL record atomically, so the page can
     * never reach disk before its WAL.
     */
    wal_state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(wal_state, buf,
                                     page_is_new ?
                                     GENERIC_XLOG_FULL_IMAGE : 0);
    if (page_is_new)
    {
        pax_page_init(page, natts);
        needed = pax_insert_space_needed(rel, values, isnulls, 0, NULL);
        available = pax_page_free_space(page);
        if (available < needed)
        {
            GenericXLogAbort(wal_state);
            UnlockReleaseBuffer(buf);
            if (extension_locked)
                UnlockRelationForExtension(rel, ExclusiveLock);
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pax: row is too large for one page"),
                     errdetail("The row needs %zu bytes but a new PAX page has only %zu bytes.",
                               needed, available)));
        }
    }

    pghdr = (PageHeader) page;
    phdr  = PaxPageHeaderPtr(page);
    tupno = phdr->n_tuples;

    if (tupno >= MaxOffsetNumber)
    {
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        if (extension_locked)
            UnlockRelationForExtension(rel, ExclusiveLock);
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
    meta->locker_mxid = InvalidMultiXactId;

    /* === 4. Une région par colonne : [bitmap de NULL][valeurs] === */
    for (i = 0; i < natts; i++)
    {
        Form_pg_attribute attr   = TupleDescAttr(tupdesc, i);
        Size              stride = pax_slot_stride(attr);
        Size              region_start;
        Size              cur;        /* taille actuelle du bitmap */
        Size              want;       /* taille voulue du bitmap */
        Size              at;
        bits8            *bmp;

        /*
         * Première écriture de cette colonne : la région s'ouvre à la fin de
         * la zone des régions (pd_lower), au byte près. Aucune contrainte
         * d'alignement n'est posée ici : les lectures passent par memcpy
         * (voir pax_get_value), donc un slot déaligné reste correct.
         */
        if (!PaxOffsetIsValid(phdr->offsets[i]))
            phdr->offsets[i] = (OffsetNumber) pghdr->pd_lower;

        region_start = phdr->offsets[i];

        /* 3a) le bitmap doit couvrir le bit "tupno" (0 = NULL, 1 = valeur).
         * Sa taille se déduit du nombre de versions, et non du span de la
         * région : le bourrage d'alignement rend le span non canonique. */
        cur  = pax_bitmap_size(tupno);
        want = pax_bitmap_size(tupno + 1);
        if (want > cur)
        {
            at = region_start + cur;
            pax_insert_bytes(page, phdr, natts, i, at, want - cur);
            memset((char *) page + at, 0, want - cur);
        }
        bmp = (bits8 *) ((char *) page + region_start);

        /* 3b) réserver l'emplacement de la valeur dans la région */
        at = region_start + want + (Size) tupno * stride;
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
    ItemPointerCopy(&slot->tts_tid, &meta->t_ctid);
    meta->flags = 0;
    slot->tts_tableOid = RelationGetRelid(rel);
    if (slot->tts_ops == &TTSOpsPax)
    {
        PaxTupleTableSlot *pslot = (PaxTupleTableSlot *) slot;

        pslot->has_tuple_meta = true;
        pslot->tuple_meta = *meta;
    }

    GenericXLogFinish(wal_state);
    if (extension_locked)
        UnlockRelationForExtension(rel, ExclusiveLock);
    UnlockReleaseBuffer(buf);

    pfree(values);
    pfree(isnulls);
    pfree(voffs);
}

/* ------------------------------------------------------------------ */
/* Versioned UPDATE / DELETE / row locking                             */
/* ------------------------------------------------------------------ */

static PaxTupleMetaData *
pax_page_tuple_meta(Page page, int tupno)
{
    PaxPageHeader *phdr = PaxPageHeaderPtr(page);

    Assert(tupno >= 0 && tupno < phdr->n_tuples);
    Assert(PaxOffsetIsValid(phdr->meta_offset));
    return ((PaxTupleMetaData *) ((char *) page + phdr->meta_offset)) + tupno;
}

typedef struct PaxLockerState
{
    bool            current_found;
    LockTupleMode   current_mode;
    bool            active_any;
    TransactionId   conflict_xid;
} PaxLockerState;

static MultiXactStatus
pax_multixact_status_for_mode(LockTupleMode mode)
{
    switch (mode)
    {
        case LockTupleKeyShare:
            return MultiXactStatusForKeyShare;
        case LockTupleShare:
            return MultiXactStatusForShare;
        case LockTupleNoKeyExclusive:
            return MultiXactStatusForNoKeyUpdate;
        case LockTupleExclusive:
            return MultiXactStatusForUpdate;
    }

    elog(ERROR, "pax: invalid tuple lock mode %d", (int) mode);
    return MultiXactStatusForKeyShare;
}

static bool
pax_lock_modes_conflict(LockTupleMode existing, LockTupleMode requested)
{
    if (existing == LockTupleKeyShare)
        return requested == LockTupleExclusive;
    if (existing == LockTupleShare)
        return requested >= LockTupleNoKeyExclusive;
    if (existing == LockTupleNoKeyExclusive)
        return requested >= LockTupleShare;
    return true;
}

static LockTupleMode
pax_lock_mode_from_multixact(MultiXactStatus status)
{
    switch (status)
    {
        case MultiXactStatusForKeyShare:
            return LockTupleKeyShare;
        case MultiXactStatusForShare:
            return LockTupleShare;
        case MultiXactStatusForNoKeyUpdate:
            return LockTupleNoKeyExclusive;
        case MultiXactStatusForUpdate:
            return LockTupleExclusive;
        case MultiXactStatusNoKeyUpdate:
        case MultiXactStatusUpdate:
            elog(ERROR, "pax: update status found in lock-only MultiXact");
    }

    elog(ERROR, "pax: invalid lock-only MultiXact status %d", (int) status);
    return LockTupleKeyShare;
}

static TransactionId
pax_locker_xmax(const PaxTupleMetaData *meta)
{
    if (TransactionIdIsValid(meta->xmax))
        return meta->xmax;
    if (MultiXactIdIsValid(meta->locker_mxid))
        return (TransactionId) meta->locker_mxid;
    return InvalidTransactionId;
}

static void
pax_get_locker_state(const PaxTupleMetaData *meta, LockTupleMode requested,
                     PaxLockerState *state)
{
    MultiXactMember *members = NULL;
    int         nmembers;
    int         i;

    memset(state, 0, sizeof(*state));
    state->current_mode = LockTupleKeyShare;
    state->conflict_xid = InvalidTransactionId;

    if (!MultiXactIdIsValid(meta->locker_mxid))
        return;

    nmembers = GetMultiXactIdMembers(meta->locker_mxid, &members, false, true);
    for (i = 0; i < nmembers; i++)
    {
        LockTupleMode member_mode = pax_lock_mode_from_multixact(members[i].status);

        if (TransactionIdIsCurrentTransactionId(members[i].xid))
        {
            state->active_any = true;
            if (!state->current_found || member_mode > state->current_mode)
            {
                state->current_found = true;
                state->current_mode = member_mode;
            }
        }
        else if (TransactionIdIsInProgress(members[i].xid))
        {
            state->active_any = true;
            if (pax_lock_modes_conflict(member_mode, requested))
            {
                state->conflict_xid = members[i].xid;
                break;
            }
        }
    }

    if (members)
        pfree(members);
}

static MultiXactId
pax_add_persistent_locker(const PaxTupleMetaData *meta, LockTupleMode mode,
                         bool *changed)
{
    PaxLockerState state;
    MultiXactStatus status = pax_multixact_status_for_mode(mode);
    MultiXactId result;

    pax_get_locker_state(meta, mode, &state);
    if (state.current_found && state.current_mode >= mode)
    {
        *changed = false;
        return meta->locker_mxid;
    }

    MultiXactIdSetOldestMember();
    if (state.active_any)
        result = MultiXactIdExpand(meta->locker_mxid,
                                   GetCurrentTransactionId(), status);
    else
    {
        MultiXactMember member;

        member.xid = GetCurrentTransactionId();
        member.status = status;
        result = MultiXactIdCreateFromMembers(1, &member);
    }

    *changed = true;
    return result;
}

static void
pax_set_locker_mxid(Relation rel, const ItemPointerData *tid,
                    MultiXactId locker_mxid,
                    PaxTupleMetaData *updated_meta)
{
    BlockNumber blkno = ItemPointerGetBlockNumber(tid);
    OffsetNumber offset = ItemPointerGetOffsetNumber(tid);
    Buffer buf;
    Page page;
    GenericXLogState *wal_state;
    PaxPageDesc *pdesc;
    PaxTupleMetaData meta;
    int tupno;

    buf = ReadBuffer(rel, blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    wal_state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(wal_state, buf, 0);
    if (!pax_page_is_valid(page))
    {
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: invalid page %u while storing tuple locks",
             blkno);
    }

    pdesc = pax_build_page_desc(page, RelationGetDescr(rel));
    tupno = (int) offset - 1;
    if (tupno < 0 || tupno >= pdesc->n_tuples)
    {
        pax_free_page_desc(pdesc);
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: tuple offset does not exist while storing locks");
    }
    meta = pdesc->tuple_meta[tupno];
    pax_free_page_desc(pdesc);

    if (meta.locker_mxid == locker_mxid)
    {
        if (updated_meta != NULL)
            *updated_meta = meta;
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        return;
    }

    meta.locker_mxid = locker_mxid;
    *pax_page_tuple_meta(page, tupno) = meta;
    if (updated_meta != NULL)
        *updated_meta = meta;
    GenericXLogFinish(wal_state);
    UnlockReleaseBuffer(buf);
}

typedef enum PaxVersionStatus
{
    PaxVersionLive,
    PaxVersionInvisible,
    PaxVersionSelfModified,
    PaxVersionInProgress,
    PaxVersionUpdated,
    PaxVersionDeleted
} PaxVersionStatus;

static PaxVersionStatus
pax_classify_version(const PaxTupleMetaData *meta,
                     const ItemPointerData *self_tid, CommandId cid)
{
    if (TransactionIdIsCurrentTransactionId(meta->xmin))
    {
        if (meta->cmin >= cid)
            return PaxVersionInvisible;
    }
    else if (meta->xmin != FrozenTransactionId &&
             meta->xmin != BootstrapTransactionId &&
             (TransactionIdIsInProgress(meta->xmin) ||
              !TransactionIdDidCommit(meta->xmin)))
        return PaxVersionInvisible;

    if (!TransactionIdIsValid(meta->xmax))
        return PaxVersionLive;

    if (TransactionIdIsCurrentTransactionId(meta->xmax))
        return meta->cmax >= cid ?
            PaxVersionSelfModified : PaxVersionInvisible;

    if (TransactionIdIsInProgress(meta->xmax))
        return PaxVersionInProgress;

    if (TransactionIdDidCommit(meta->xmax))
        return ItemPointerEquals(self_tid, &meta->t_ctid) ?
            PaxVersionDeleted : PaxVersionUpdated;

    /* The outdating transaction aborted; this physical version is live. */
    return PaxVersionLive;
}

static void
pax_fill_failure_data(TM_FailureData *tmfd,
                      const PaxTupleMetaData *meta,
                      TM_Result result, bool traversed)
{
    tmfd->ctid = meta->t_ctid;
    tmfd->xmax = meta->xmax;
    tmfd->cmax = result == TM_SelfModified ? meta->cmax : InvalidCommandId;
    tmfd->traversed = traversed;
}

static LOCKMODE
pax_lockmode_for_tuple_mode(LockTupleMode mode)
{
    switch (mode)
    {
        case LockTupleKeyShare:
            return AccessShareLock;
        case LockTupleShare:
            return RowShareLock;
        case LockTupleNoKeyExclusive:
            return ExclusiveLock;
        case LockTupleExclusive:
            return AccessExclusiveLock;
    }

    elog(ERROR, "pax: invalid tuple lock mode %d", (int) mode);
    return NoLock;
}

static bool
pax_acquire_dml_tuplock(Relation rel, const ItemPointerData *tid, bool wait)
{
    if (wait)
    {
        LockTuple(rel, tid, AccessExclusiveLock);
        return true;
    }

    return ConditionalLockTuple(rel, tid, AccessExclusiveLock, false);
}

/*
 * Acquire the heavyweight tuple lock, then return the exact physical version
 * with its page content lock held.  The heavyweight lock closes the race
 * between dropping the content lock to wait for an outdating xact and another
 * writer changing the same tuple.
 */
static TM_Result
pax_lock_version_for_dml(Relation rel, const ItemPointerData *tid,
                         CommandId cid, Snapshot crosscheck, bool wait,
                         XLTW_Oper wait_op, TM_FailureData *tmfd,
                         Buffer *buffer, PaxPageDesc **pdesc,
                         PaxTupleMetaData *meta, ItemPointerData *self_tid,
                         bool *have_tuplock)
{
    bool have_lock;

    if (!ItemPointerIsValid(tid))
        elog(ERROR, "pax: invalid tuple identifier for UPDATE/DELETE");

    have_lock = pax_acquire_dml_tuplock(rel, tid, wait);
    if (!have_lock)
    {
        ItemPointerCopy(tid, &tmfd->ctid);
        tmfd->xmax = InvalidTransactionId;
        tmfd->cmax = InvalidCommandId;
        tmfd->traversed = false;
        return TM_BeingModified;
    }

    for (;;)
    {
        BlockNumber  blkno = ItemPointerGetBlockNumber(tid);
        OffsetNumber offset = ItemPointerGetOffsetNumber(tid);
        PaxVersionStatus status;
        Buffer       buf;
        Page         page;
        PaxPageDesc *desc;
        int          tupno;

        if (blkno >= RelationGetNumberOfBlocks(rel) ||
            !OffsetNumberIsValid(offset))
            elog(ERROR, "pax: tuple identifier outside relation \"%s\"",
                 RelationGetRelationName(rel));

        buf = ReadBuffer(rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buf);
        if (!pax_page_is_valid(page))
        {
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 blkno, RelationGetRelationName(rel));
        }

        desc = pax_build_page_desc(page, RelationGetDescr(rel));
        tupno = (int) offset - 1;
        if (tupno >= desc->n_tuples)
        {
            pax_free_page_desc(desc);
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: tuple offset does not exist in relation \"%s\"",
                 RelationGetRelationName(rel));
        }

        *meta = desc->tuple_meta[tupno];
        ItemPointerSet(self_tid, blkno, offset);
        status = pax_classify_version(meta, self_tid, cid);

        if (status == PaxVersionLive)
        {
            PaxLockerState locker_state;

            pax_get_locker_state(meta, LockTupleExclusive, &locker_state);
            if (TransactionIdIsValid(locker_state.conflict_xid))
            {
                if (wait)
                {
                    TransactionId xwait = locker_state.conflict_xid;

                    pax_free_page_desc(desc);
                    UnlockReleaseBuffer(buf);
                    UnlockTuple(rel, self_tid, AccessExclusiveLock);
                    XactLockTableWait(xwait, rel, self_tid, wait_op);
                    LockTuple(rel, self_tid, AccessExclusiveLock);
                    continue;
                }

                meta->xmax = locker_state.conflict_xid;
                status = PaxVersionInProgress;
            }
        }

        if (status == PaxVersionLive &&
            crosscheck != InvalidSnapshot &&
            !pax_meta_satisfies_snapshot(meta, crosscheck))
            status = PaxVersionUpdated;

        if (status == PaxVersionLive)
        {
            *buffer = buf;
            *pdesc = desc;
            *have_tuplock = true;
            return TM_Ok;
        }

        if (status == PaxVersionInProgress && wait)
        {
            TransactionId xwait = meta->xmax;

            pax_free_page_desc(desc);
            UnlockReleaseBuffer(buf);
            XactLockTableWait(xwait, rel, self_tid, wait_op);
            continue;
        }

        {
            TM_Result result;

            switch (status)
            {
                case PaxVersionInvisible:
                    result = TM_Invisible;
                    break;
                case PaxVersionSelfModified:
                    result = TM_SelfModified;
                    break;
                case PaxVersionInProgress:
                    result = TM_BeingModified;
                    break;
                case PaxVersionUpdated:
                    result = TM_Updated;
                    break;
                case PaxVersionDeleted:
                    result = TM_Deleted;
                    break;
                case PaxVersionLive:
                    pg_unreachable();
            }

            pax_fill_failure_data(tmfd, meta, result, false);
            pax_free_page_desc(desc);
            UnlockReleaseBuffer(buf);
            UnlockTuple(rel, tid, AccessExclusiveLock);
            return result;
        }
    }
}

static TM_Result
pax_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
                 uint32 options, Snapshot snapshot, Snapshot crosscheck,
                 bool wait, TM_FailureData *tmfd)
{
    Buffer          buf = InvalidBuffer;
    Page            page;
    GenericXLogState *wal_state;
    PaxPageDesc    *pdesc = NULL;
    PaxTupleMetaData meta;
    ItemPointerData self_tid;
    bool            have_tuplock = false;
    TransactionId   xid;
    TM_Result       result;

    if (IsInParallelMode())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                 errmsg("cannot delete tuples during a parallel operation")));
    if (cid == InvalidCommandId)
        elog(ERROR, "pax: invalid DELETE command ID");
    if (!wait)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support non-waiting DELETE")));
    if (options & TABLE_DELETE_CHANGING_PARTITION)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support partition-row moves")));

    (void) snapshot;
    xid = GetCurrentTransactionId();
    tmfd->traversed = false;
    result = pax_lock_version_for_dml(rel, tid, cid, crosscheck, wait,
                                      XLTW_Delete, tmfd, &buf, &pdesc,
                                      &meta, &self_tid, &have_tuplock);
    if (result == TM_Invisible)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("attempted to delete invisible tuple")));
    if (result != TM_Ok)
        return result;

    CheckForSerializableConflictIn(rel, tid,
                                   ItemPointerGetBlockNumber(&self_tid));

    meta.xmax = xid;
    meta.cmax = cid;
    ItemPointerCopy(&self_tid, &meta.t_ctid);
    wal_state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(wal_state, buf, 0);
    *pax_page_tuple_meta(page,
                          ItemPointerGetOffsetNumber(&self_tid) - 1) = meta;
    GenericXLogFinish(wal_state);

    pax_free_page_desc(pdesc);
    UnlockReleaseBuffer(buf);
    if (have_tuplock)
        UnlockTuple(rel, &self_tid, AccessExclusiveLock);

    return TM_Ok;
}

static TM_Result
pax_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
                 CommandId cid, uint32 options, Snapshot snapshot,
                 Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
                 LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
    Buffer          buf = InvalidBuffer;
    Page            page;
    GenericXLogState *wal_state;
    PaxPageDesc    *pdesc = NULL;
    PaxTupleMetaData meta;
    PaxTupleMetaData new_meta;
    ItemPointerData self_tid;
    ItemPointerData new_tid;
    MultiXactId    carry_locker_mxid = InvalidMultiXactId;
    PaxLockerState locker_state;
    PaxTupleTableSlot *pslot = NULL;
    bool            have_tuplock = false;
    TransactionId   xid;
    TM_Result       result;

    if (IsInParallelMode())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                 errmsg("cannot update tuples during a parallel operation")));
    if (cid == InvalidCommandId)
        elog(ERROR, "pax: invalid UPDATE command ID");
    if (!wait)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pax table AM does not support non-waiting UPDATE")));
    (void) options;
    (void) snapshot;

    *lockmode = LockTupleExclusive;
    *update_indexes = TU_None;
    tmfd->traversed = false;

    /* The new values may be backed by another PAX page; detach them first. */
    ExecMaterializeSlot(slot);

    xid = GetCurrentTransactionId();
    result = pax_lock_version_for_dml(rel, otid, cid, crosscheck, wait,
                                      XLTW_Update, tmfd, &buf, &pdesc,
                                      &meta, &self_tid, &have_tuplock);
    if (result == TM_Invisible)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("attempted to update invisible tuple")));
    if (result != TM_Ok)
        return result;

    pax_get_locker_state(&meta, LockTupleExclusive, &locker_state);
    if (locker_state.current_found)
        carry_locker_mxid = meta.locker_mxid;

    CheckForSerializableConflictIn(rel, otid,
                                   ItemPointerGetBlockNumber(&self_tid));

    /*
     * Reserve the old version first.  While xmax names our in-progress xact,
     * another writer cannot advance the chain.  Insert the replacement between
     * that reservation and publication of the forward t_ctid link.
     */
    meta.xmax = xid;
    meta.cmax = cid;
    ItemPointerCopy(&self_tid, &meta.t_ctid);
    wal_state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(wal_state, buf, 0);
    *pax_page_tuple_meta(page,
                          ItemPointerGetOffsetNumber(&self_tid) - 1) = meta;
    GenericXLogFinish(wal_state);
    pax_free_page_desc(pdesc);
    UnlockReleaseBuffer(buf);

    pax_tuple_insert(rel, slot, cid, 0, NULL);
    new_tid = slot->tts_tid;
    if (MultiXactIdIsValid(carry_locker_mxid))
    {
        if (slot->tts_ops == &TTSOpsPax)
            pslot = (PaxTupleTableSlot *) slot;
        pax_set_locker_mxid(rel, &new_tid, carry_locker_mxid,
                            pslot != NULL ? &new_meta : NULL);
        if (pslot != NULL)
        {
            pslot->has_tuple_meta = true;
            pslot->tuple_meta = new_meta;
        }
    }

    /* Re-find the old version because insertion may have moved its metadata. */
    buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&self_tid));
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    wal_state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(wal_state, buf, 0);
    if (!pax_page_is_valid(page))
    {
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: invalid old-version page after UPDATE");
    }
    pdesc = pax_build_page_desc(page, RelationGetDescr(rel));
    if ((int) ItemPointerGetOffsetNumber(&self_tid) - 1 >= pdesc->n_tuples)
    {
        pax_free_page_desc(pdesc);
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: old UPDATE version disappeared");
    }
    meta = pdesc->tuple_meta[ItemPointerGetOffsetNumber(&self_tid) - 1];
    if (meta.xmax != xid || meta.cmax != cid ||
        !ItemPointerEquals(&meta.t_ctid, &self_tid))
    {
        pax_free_page_desc(pdesc);
        GenericXLogAbort(wal_state);
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: old UPDATE version changed unexpectedly");
    }

    ItemPointerCopy(&new_tid, &meta.t_ctid);
    *pax_page_tuple_meta(page,
                          ItemPointerGetOffsetNumber(&self_tid) - 1) = meta;
    GenericXLogFinish(wal_state);
    pax_free_page_desc(pdesc);
    UnlockReleaseBuffer(buf);

    if (have_tuplock)
        UnlockTuple(rel, &self_tid, AccessExclusiveLock);

    slot->tts_tableOid = RelationGetRelid(rel);
    *update_indexes = TU_All;
    return TM_Ok;
}

static TM_Result
pax_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
               TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
               LockWaitPolicy wait_policy, uint8 flags,
               TM_FailureData *tmfd)
{
    LOCKMODE        lockmode = pax_lockmode_for_tuple_mode(mode);
    ItemPointerData current = *tid;
    ItemPointerData locked_tid;
    TransactionId   expected_xmin = InvalidTransactionId;
    bool            have_tuplock = false;
    bool            traversed = false;
    uint64          hops = 0;

    if (cid == InvalidCommandId)
        elog(ERROR, "pax: invalid row-lock command ID");
    (void) snapshot;

    ItemPointerSetInvalid(&locked_tid);
    tmfd->traversed = false;

    for (;;)
    {
        BlockNumber  blkno;
        BlockNumber  nblocks = RelationGetNumberOfBlocks(rel);
        OffsetNumber offset;
        uint64       max_hops = ((uint64) nblocks + 1) * MaxOffsetNumber;
        Buffer       buf;
        PaxPageDesc *pdesc;
        PaxTupleMetaData meta;
        PaxVersionStatus status;

        if (!ItemPointerIsValid(&current))
            elog(ERROR, "pax: invalid tuple identifier in lock request");
        if (++hops > max_hops)
            elog(ERROR, "pax: tuple version chain is cyclic or too long");

        blkno = ItemPointerGetBlockNumber(&current);
        offset = ItemPointerGetOffsetNumber(&current);
        if (blkno >= nblocks || !OffsetNumberIsValid(offset))
            elog(ERROR, "pax: tuple identifier outside relation \"%s\"",
                 RelationGetRelationName(rel));

        buf = ReadBuffer(rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        if (!pax_page_is_valid(BufferGetPage(buf)))
        {
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 blkno, RelationGetRelationName(rel));
        }
        pdesc = pax_build_page_desc(BufferGetPage(buf), RelationGetDescr(rel));
        if ((int) offset - 1 >= pdesc->n_tuples)
        {
            pax_free_page_desc(pdesc);
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: tuple offset does not exist in relation \"%s\"",
                 RelationGetRelationName(rel));
        }
        meta = pdesc->tuple_meta[offset - 1];
        status = pax_classify_version(&meta, &current, cid);
        pax_free_page_desc(pdesc);
        UnlockReleaseBuffer(buf);

        if (TransactionIdIsValid(expected_xmin) &&
            meta.xmin != expected_xmin)
        {
            if (have_tuplock)
            {
                UnlockTuple(rel, &locked_tid, lockmode);
                have_tuplock = false;
            }
            ItemPointerCopy(&current, &tmfd->ctid);
            tmfd->xmax = expected_xmin;
            tmfd->cmax = InvalidCommandId;
            tmfd->traversed = traversed;
            *tid = current;
            return TM_Deleted;
        }

        if (status == PaxVersionLive)
        {
            PaxLockerState locker_state;
            MultiXactId new_locker_mxid;
            bool locker_changed;

            if (!have_tuplock || !ItemPointerEquals(&locked_tid, &current))
            {
                switch (wait_policy)
                {
                    case LockWaitBlock:
                        LockTuple(rel, &current, lockmode);
                        break;
                    case LockWaitSkip:
                        if (!ConditionalLockTuple(rel, &current, lockmode,
                                                   false))
                            return TM_WouldBlock;
                        break;
                    case LockWaitError:
                        if (!ConditionalLockTuple(rel, &current, lockmode,
                                                   true))
                            ereport(ERROR,
                                    (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                                     errmsg("could not obtain lock on row in relation \"%s\"",
                                            RelationGetRelationName(rel))));
                        break;
                }
                locked_tid = current;
                have_tuplock = true;

                /* An update may have won before the heavyweight lock. */
                continue;
            }

            pax_get_locker_state(&meta, mode, &locker_state);
            if (TransactionIdIsValid(locker_state.conflict_xid))
            {
                switch (wait_policy)
                {
                    case LockWaitBlock:
                        UnlockTuple(rel, &locked_tid, lockmode);
                        have_tuplock = false;
                        XactLockTableWait(locker_state.conflict_xid, rel,
                                          &current, XLTW_Lock);
                        LockTuple(rel, &current, lockmode);
                        locked_tid = current;
                        have_tuplock = true;
                        break;
                    case LockWaitSkip:
                        if (!ConditionalXactLockTableWait(
                                locker_state.conflict_xid, false))
                        {
                            UnlockTuple(rel, &locked_tid, lockmode);
                            have_tuplock = false;
                            return TM_WouldBlock;
                        }
                        break;
                    case LockWaitError:
                        if (!ConditionalXactLockTableWait(
                                locker_state.conflict_xid, true))
                        {
                            UnlockTuple(rel, &locked_tid, lockmode);
                            have_tuplock = false;
                            ereport(ERROR,
                                    (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                                     errmsg("could not obtain lock on row in relation \"%s\"",
                                            RelationGetRelationName(rel))));
                        }
                        break;
                }
                continue;
            }

            new_locker_mxid = pax_add_persistent_locker(&meta, mode,
                                                        &locker_changed);
            if (locker_changed)
                pax_set_locker_mxid(rel, &current, new_locker_mxid, NULL);

            if (!pax_tuple_fetch_row_version(rel, &current, SnapshotAny, slot))
                elog(ERROR, "pax: failed to fetch row while taking tuple lock");

            UnlockTuple(rel, &locked_tid, lockmode);
            have_tuplock = false;
            *tid = current;
            tmfd->traversed = traversed;
            return TM_Ok;
        }


        if (status == PaxVersionUpdated &&
            (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) != 0)
        {
            if (have_tuplock)
            {
                UnlockTuple(rel, &locked_tid, lockmode);
                have_tuplock = false;
            }
            expected_xmin = meta.xmax;
            current = meta.t_ctid;
            traversed = true;
            continue;
        }

        if (status == PaxVersionInProgress)
        {
            switch (wait_policy)
            {
                case LockWaitBlock:
                    XactLockTableWait(meta.xmax, rel, &current, XLTW_Lock);
                    break;
                case LockWaitSkip:
                    if (!ConditionalXactLockTableWait(meta.xmax, false))
                    {
                        pax_fill_failure_data(tmfd, &meta,
                                              TM_BeingModified, traversed);
                        if (have_tuplock)
                        {
                            UnlockTuple(rel, &locked_tid, lockmode);
                            have_tuplock = false;
                        }
                        return TM_WouldBlock;
                    }
                    break;
                case LockWaitError:
                    if (!ConditionalXactLockTableWait(meta.xmax, true))
                        ereport(ERROR,
                                (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                                 errmsg("could not obtain lock on row in relation \"%s\"",
                                        RelationGetRelationName(rel))));
                    break;
            }
            continue;
        }

        {
            TM_Result result;

            switch (status)
            {
                case PaxVersionInvisible:
                    result = TM_Invisible;
                    break;
                case PaxVersionSelfModified:
                    result = TM_SelfModified;
                    break;
                case PaxVersionUpdated:
                    result = TM_Updated;
                    break;
                case PaxVersionDeleted:
                    result = TM_Deleted;
                    break;
                case PaxVersionInProgress:
                case PaxVersionLive:
                    pg_unreachable();
            }

            pax_fill_failure_data(tmfd, &meta, result, traversed);
            if (have_tuplock)
            {
                UnlockTuple(rel, &locked_tid, lockmode);
                have_tuplock = false;
            }
            *tid = current;
            return result;
        }
    }
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
    {
        pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
        ExecMaterializeSlot(slot);
    }
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
        ItemPointerGetBlockNumber(tid) <
        RelationGetNumberOfBlocks(scan->rs_base.rs_rd);
}

static void
pax_tuple_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    PaxScanDesc scan = (PaxScanDesc) sscan;
    Relation    rel = scan->rs_base.rs_rd;
    Snapshot    snapshot = scan->rs_base.rs_snapshot;
    ItemPointerData current = *tid;
    ItemPointerData last_visible;
    TransactionId expected_xmin = InvalidTransactionId;
    uint64      hops = 0;

    ItemPointerSetInvalid(&last_visible);

    for (;;)
    {
        BlockNumber  blkno;
        BlockNumber  nblocks = RelationGetNumberOfBlocks(rel);
        uint64       max_hops = ((uint64) nblocks + 1) * MaxOffsetNumber;
        OffsetNumber offset;
        int          tupno;
        Buffer       buf;
        PaxPageDesc *pdesc;
        PaxTupleMetaData meta;
        ItemPointerData next;
        TransactionId xmax;
        bool          follow;

        if (!ItemPointerIsValid(&current))
            elog(ERROR, "pax: invalid tuple identifier in version chain");
        blkno = ItemPointerGetBlockNumber(&current);
        offset = ItemPointerGetOffsetNumber(&current);
        if (blkno >= nblocks || !OffsetNumberIsValid(offset))
            elog(ERROR, "pax: tuple identifier outside relation in version chain");

        if (++hops > max_hops)
            elog(ERROR, "pax: tuple version chain is cyclic or too long");

        buf = ReadBuffer(rel, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        if (!pax_page_is_valid(BufferGetPage(buf)))
        {
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 blkno, RelationGetRelationName(rel));
        }

        pdesc = pax_build_page_desc(BufferGetPage(buf),
                                    RelationGetDescr(rel));
        tupno = (int) offset - 1;
        if (tupno >= pdesc->n_tuples)
        {
            pax_free_page_desc(pdesc);
            UnlockReleaseBuffer(buf);
            elog(ERROR, "pax: tuple offset does not exist in version chain");
        }
        meta = pdesc->tuple_meta[tupno];
        next = meta.t_ctid;
        xmax = meta.xmax;
        pax_free_page_desc(pdesc);
        UnlockReleaseBuffer(buf);

        if (TransactionIdIsValid(expected_xmin) &&
            meta.xmin != expected_xmin)
            elog(ERROR, "pax: update chain points to an unrelated tuple");

        if (pax_meta_satisfies_snapshot(&meta, snapshot))
            last_visible = current;

        if (!ItemPointerEquals(&current, &next) &&
            !TransactionIdIsValid(xmax))
            elog(ERROR, "pax: nonself version link has no xmax");

        follow = ItemPointerEquals(&current, &next) ? false :
            (TransactionIdIsCurrentTransactionId(xmax) ||
             (!TransactionIdIsInProgress(xmax) &&
              TransactionIdDidCommit(xmax)));

        if (!follow)
        {
            *tid = ItemPointerIsValid(&last_visible) ? last_visible : current;
            return;
        }

        expected_xmin = xmax;
        current = next;
    }
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

    /* PAX stores transaction metadata per version; these catalog horizons
     * remain conservative because freezing and vacuum are not implemented. */
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
#define PAX_OVERHEAD_BYTES_PER_TUPLE  \
    (SizeOfPaxTupleMetaData + 2 * sizeof(OffsetNumber))
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
    pax_validate_table_am_routine();
    PG_RETURN_POINTER(&pax_methods);
}

/* ------------------------------------------------------------------ */
/* Fin du fichier                                                      */
/* ------------------------------------------------------------------ */

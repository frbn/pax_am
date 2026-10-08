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
 *   - carte de visibilité (VM) jamais écrite : aucune page PAX n'est déclarée
 *     all-visible. Les scans index-only fonctionnent (les deux callbacks sont
 *     là), mais prennent toujours le chemin "dirty" et ne peuvent pas sauter le
 *     test de visibilité. Voir analyse1.md pour pourquoi le gain est faible et
 *     le piège de correction élevé.
 *   - le FSM, lui, est bien mis à jour : à chaque insertion, et une fois de
 *     plus par VACUUM (pax_relation_vacuum)
 *   - versions UPDATE / DELETE append-only, sans VACUUM ni gel des tuples
 *   - Generic WAL couvre les mutations de page ; l'insertion spéculative reste
 *     non supportée
 *   - pas d'index, de parallélisme ou d'opérations DDL non réécrivantes
 *
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/genam.h"
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
#include "catalog/index.h"
#include "commands/progress.h"
#include "pgstat.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "common/relpath.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/freespace.h"
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
#include "utils/relcache.h"
#include "postmaster/autovacuum.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "access/tidstore.h"


/* extension */
PG_MODULE_MAGIC;

/*
 * 1 = format initial
 * 2 = introduction des colonnes en régions
 * 3 = métadonnées de version séparées (32 o) + maillons t_ctid
 * 4 = slots packés sans suralignement (pas = attlen, ou 2 pour un varlena).
 *     Une page v3 serait relue avec un pas faux : les versions 3 et 4 sont
 *     incompatibles, pas seulement différentes.
 * 5 = (retiré) chunks de taille fixe par colonne. Abandonné, cf. analyse1.md.
 * 6 = suppression des 8 octets de bourrage entre PageHeaderData et
 *     PaxPageHeader. pd_lower baisse de 8 sur toutes les pages.
 *
 * On saute volontairement le numéro 5 : il a porté le format à chunks. Lui
 * réutiliser serait un piège — une page à chunks porte version 5, et la relire
 * comme le format courant produirait des slots faux sans lever la moindre
 * erreur. Les numéros se consomment, ils ne se réattribuent pas.
 */
#define PAX_PAGE_VERSION            6
#define PAX_SPECIAL_MAGIC           0x5041 /* "PA" */

#define PAX_FLAG_HAS_NULLS          0x0001
#define PAX_FLAG_HAS_VARLENA        0x0002
#define PAX_FLAG_COMPRESSED         0x0004
#define PAX_FLAG_HAS_XMIN_XMAX      0x0008
#define PAX_FLAG_HAS_VERSIONS       0x0010

/*
 * Bit de PaxTupleMetaData.flags marquant une version désormais inutilisée.
 *
 * VACUUM ne compacte pas les régions : elles sont columnaires, et retirer une
 * version décalerait les slots de toutes les colonnes, ce qui casserait les
 * liens t_ctid entrants pointant sur des offsets physiques. La version est
 * donc marquee sur place, et INSERT reutilise son emplacement.
 *
 * Le champ 'flags' devait etre nul en version 3 ; la version 4 lui donne
 * cette affectation.
 */
#define PAX_VERSION_UNUSED          0x0001

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

/*
 * Où commence l'en-tête PAX dans une page : juste après PageHeaderData.
 *
 * Il y avait ici 8 octets de bourrage, justifiés par un « header 8-aligned »
 * qui ne tenait pas : PaxPageHeader ne contient que des uint16 (alignement 2),
 * PaxTupleMetaData seulement des uint32 (alignement 4), et SizeOfPageHeaderData
 * vaut 24, déjà multiple de 8 — le tampon de base l'est aussi. Les 8 octets ne
 * satisfaisaient donc aucune contrainte que la disposition ne satisfasse déjà.
 * Ils étaient là parce que l'offset s'écrivait + SizeOfPaxSpecialData, par
 * analogie de forme avec heap. La zone spéciale réelle est à la fin de la
 * page, écrite par PageInit() ; rien n'a jamais été lu à cet endroit.
 *
 * L'invariant réel, celui qui compte, est que l'en-tête tombe sur une frontière
 * MAXIMUM_ALIGNO. Il est posé plus bas en StaticAssert, pas ici : un chiffre
 * dans un commentaire n'est pas une garantie.
 */
#define PaxPageHeaderPtr(page)  \
    ((PaxPageHeader *) ((char *) (page) + SizeOfPageHeaderData))

/*
 * Le tampon vient de ReadBuffer et vient d'un AlignedBuffer, déclaré
 * `alignas(MAXIMUM_ALIGNOF) char data[BLCKSZ]` (c.h). Il suffit donc que
 * SizeOfPageHeaderData soit multiple de MAXIMUM_ALIGNOF pour que
 * PaxPageHeader, puis pd_lower et la région de métadonnées, heredent de cet
 * alignement. C'est le seul vrai invariant ici, et il vaut pour 24 = 3 x 8
 * sans qu'aucun octet de bourrage soit nécessaire.
 */
StaticAssertDecl(SizeOfPageHeaderData % MAXIMUM_ALIGNOF == 0,
                 "pax: PaxPageHeader would be misaligned without padding");

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

    /*
     * Instantané de la mise en page, utilisé par pax_layout_is_current().
     *
     * header et special pointent DANS la page : les comparer à eux-mêmes ne
     * prouverait rien. Il faut une copie indépendante des champs qui
     * déterminent la géométrie des régions, pour pouvoir vérifier que le
     * cache décrit toujours la page courante.
     */
    BlockNumber     stamp_blkno;
    uint16          stamp_flags;
    OffsetNumber    stamp_meta_offset;
    OffsetNumber   *stamp_offsets;    /* n_attrs entrées */
    int             meta_capacity;    /* versions que tuple_meta peut contenir */
    /*
     * Curseur de revalidation : les entrées [0, meta_checked_through) ont été
     * copiées et validées par ce descripteur. Un balayage n'avançant que vers
     * l'avant, elles ne seront plus relues, ce qui évite de recopier toute la
     * région à chaque tupline retournée (cf. pax_meta_ensure).
     */
    int             meta_checked_through;
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

    /* ANALYZE : la page courante est épuisée quand la boucle interne s'arrête. */
    bool            analyze_done;

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

    /*
     * Mise en page de la page courante, conservée d'une tupline à l'autre et
     * revalidée à chaque appel (pax_layout_is_current). Les métadonnées de
     * version ne sont pas mises en cache : elles sont recopiées à chaque
     * appel. Le contexte survit au scan entier ; le descripteur est
     * reconstruit dès que l'en-tête de page change.
     */
    PaxPageDesc    *cached_desc;
    MemoryContext   desc_ctx;
} PaxScanDescData;

typedef PaxScanDescData *PaxScanDesc;

/* ------------------------------------------------------------------ */
/* Protos                                                             */
/* ------------------------------------------------------------------ */

/* Page mngmt */
static void         pax_page_init(Page page, int n_attrs);
static bool         pax_page_is_valid(Page page);
static PaxPageDesc *pax_build_page_layout(Page page, BlockNumber blkno,
                                           TupleDesc tupdesc);
static bool         pax_layout_is_current(const PaxPageDesc *desc, Page page,
                                           BlockNumber blkno,
                                           PaxPageHeader *phdr,
                                           TupleDesc tupdesc);
static void         pax_refresh_tuple_meta(PaxPageDesc *desc, Page page);
static void         pax_meta_ensure(PaxPageDesc *desc, Page page, int i);
static PaxPageDesc *pax_build_page_desc(Page page, TupleDesc tupdesc);
static void         pax_free_page_desc(PaxPageDesc *desc);

/* Value */
static Datum        pax_get_value(PaxPageDesc *desc, int attno, int tupno, bool *isnull);

/* Size / page layout */
static Size         pax_slot_stride(Form_pg_attribute attr);
static Size         pax_region_used(int n_tuples, Size stride);
static Size         pax_bitmap_size(int n_tuples);

/*
 * Plafond d'une demande au FSM.
 *
 * MaxFSMRequestSize n'est pas exporté par freespace.c : c'est MaxHeapTupleSize,
 * qu'il définit à partir de ce que la FSM sait représenter. On recompose donc
 * l'expression, et on vérifie qu'elle n'a pas bougé d'une version à l'autre.
 * Une dérive se traduirait en « invalid FSM request size », donc en plantage
 * net plutôt qu'en corruption, mais indolore jusqu'au premier INSERT large.
 */
#define PaxMaxFSMRequestSize \
    (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)))

StaticAssertDecl(MaxHeapTupleSize == PaxMaxFSMRequestSize,
                 "pax: FSM request ceiling moved; update PaxMaxFSMRequestSize");

static Size         pax_insert_space_hint(Relation rel, Datum *values,
                                          bool *isnulls);
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
/*
 * Etat d'une version pour l'analyse et le nettoyage.
 *
 * Il ne s'agit PAS de la visibilité MVCC d'un snapshot : c'est la question
 * « cette version peut-elle encore être utile ? », celle que se pose VACUUM.
 */
typedef enum PaxVersionState
{
    PAX_VERSION_LIVE,          /* inserée et non supprimée */
    PAX_VERSION_DEAD,          /* insertion annulée, ou suppression validée */
    PAX_VERSION_RECENT         /* en cours : ni supprimable, ni comptabilisable */
} PaxVersionState;

static bool
pax_meta_is_unused(const PaxTupleMetaData *meta)
{
    return (meta->flags & PAX_VERSION_UNUSED) != 0;
}

/*
 * Classe une version pour VACUUM et ANALYZE.
 *
 * Une version est morte si son insertion a été annulée (xmin aborté : la ligne
 * n'a jamais existé pour personne) ou si sa suppression a été validée. Les
 * lignes verrouillées ne passent pas par xmax : elles sont dans locker_mxid,
 * ce qui évite de les confondre avec une suppression.
 */
static PaxVersionState
pax_meta_classify(const PaxTupleMetaData *meta)
{
    if (pax_meta_is_unused(meta))
        return PAX_VERSION_DEAD;

    if (!TransactionIdIsValid(meta->xmin))
        return PAX_VERSION_RECENT;

    if (TransactionIdDidAbort(meta->xmin))
        return PAX_VERSION_DEAD;

    if (TransactionIdIsCurrentTransactionId(meta->xmin))
        return PAX_VERSION_LIVE;

    if (!TransactionIdDidCommit(meta->xmin))
        return PAX_VERSION_RECENT;

    if (!TransactionIdIsValid(meta->xmax))
        return PAX_VERSION_LIVE;

    if (TransactionIdDidAbort(meta->xmax))
        return PAX_VERSION_LIVE;

    if (!TransactionIdDidCommit(meta->xmax))
        return PAX_VERSION_RECENT;

    return PAX_VERSION_DEAD;
}

/*
 * Vrai si un verrou de ligne peut encore empêcher la réutilisation d'une
 * version : un membre du MultiXact en cours suffit.
 */
static bool
pax_meta_has_live_locker(const PaxTupleMetaData *meta)
{
    if (!MultiXactIdIsValid(meta->locker_mxid))
        return false;

    /*
     * isLockOnly : le MultiXact de PAX ne contient que des verrous de ligne,
     * jamais qu'un verrou. Un membre encore en cours suffit à interdire la
     * réutilisation de l'emplacement.
     *
     * Cette forme est préférée à GetMultiXactIdMembers() car le tableau
     * renvoyé par cette dernière pointe dans le cache local de MultiXact :
     * il ne doit pas être libéré, et le parcours coûterait un accès SLRU.
     */
    return MultiXactIdIsRunning(meta->locker_mxid, true /* isLockOnly */);
}

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
static PaxPageDesc *pax_scan_page_desc(PaxScanDesc scan, Page page,
                                        BlockNumber blkno, TupleDesc tupdesc);
static void         pax_scan_drop_cached_desc(PaxScanDesc scan);

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
static TransactionId pax_relation_index_delete_tuples(Relation rel,
                                                      TM_IndexDeleteOp *delstate);
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

/*
 * Index scans.
 *
 * Un index ne stocke que le TID d'une version, et cette version a sa PROPRE
 * entrée : contrairement à heap, il n'y a pas de chaîne HOT, donc pas de racine
 * de chaîne à remonter. Un UPDATE réinsère une entrée pointant sur la nouvelle
 * version, et l'entrée de l'ancienne version reste en place jusqu'au VACUUM de
 * l'index.
 *
 * Conséquence directe et importante : il ne faut PAS remonter la chaîne t_ctid
 * ici. L'entrée qui désigne l'ancienne version doit rendre l'ancienne version,
 * invisible pour le snapshot, afin que le filtre du lecteur la rejette. Si on
 * suivait la chaîne, l'entrée périmée renverrait la version suivante, qui est
 * visible, et la ligne apparaîtrait deux fois à l'écran alors que la table n'en
 * contient qu'une. C'est exactement ce que fait une chaîne HOT de heap, et
 * c'est pourquoi heap n'a pas ce problème : là, la seule entrée désigne la
 * racine de la chaîne et non une version.
 *
 * Le descripteur de page est reconstruit à chaque TID : on saute d'une page à
 * l'autre, et un tampon memoïsé ne serait pas réutilisable.
 */
typedef struct PaxIndexFetchData
{
    IndexFetchTableData xs_base;   /* DOIT être le premier champ */
    Buffer          xs_cbuf;       /* page courante, épinglée */
    BlockNumber     xs_cblkno;
} PaxIndexFetchData;

static struct IndexFetchTableData *
pax_index_fetch_begin(Relation rel, uint32 flags)
{
    PaxIndexFetchData *xscan = palloc0_object(PaxIndexFetchData);

    xscan->xs_base.rel = rel;
    xscan->xs_base.flags = flags;
    xscan->xs_cbuf = InvalidBuffer;
    xscan->xs_cblkno = InvalidBlockNumber;

    return &xscan->xs_base;
}

static void
pax_index_fetch_reset(struct IndexFetchTableData *data)
{
    PaxIndexFetchData *xscan = (PaxIndexFetchData *) data;

    if (BufferIsValid(xscan->xs_cbuf))
    {
        ReleaseBuffer(xscan->xs_cbuf);
        xscan->xs_cbuf = InvalidBuffer;
    }
    xscan->xs_cblkno = InvalidBlockNumber;
}

static void
pax_index_fetch_end(struct IndexFetchTableData *data)
{
    PaxIndexFetchData *xscan = (PaxIndexFetchData *) data;

    pax_index_fetch_reset(data);
    pfree(xscan);
}

static bool
pax_index_fetch_tuple(struct IndexFetchTableData *data, ItemPointer tid,
                      Snapshot snapshot, TupleTableSlot *slot,
                      bool *call_again, bool *all_dead)
{
    PaxIndexFetchData  *xscan = (PaxIndexFetchData *) data;
    Relation           rel = xscan->xs_base.rel;
    BlockNumber        blkno;
    OffsetNumber       offset;
    int                tupno;
    Buffer             buf;
    PaxPageDesc       *pdesc;
    PaxTupleMetaData   meta;
    bool               visible;

    /*
     * Une seule version par entrée d'index : il n'y a donc jamais de second
     * maillon à rendre pour le même TID.
     */
    *call_again = false;

    if (all_dead != NULL)
        *all_dead = false;

    if (!ItemPointerIsValid(tid))
        return false;

    blkno = ItemPointerGetBlockNumber(tid);
    offset = ItemPointerGetOffsetNumber(tid);
    if (blkno >= RelationGetNumberOfBlocks(rel) ||
        !OffsetNumberIsValid(offset))
        return false;

    CHECK_FOR_INTERRUPTS();

    if (!BufferIsValid(xscan->xs_cbuf) || blkno != xscan->xs_cblkno)
    {
        xscan->xs_cbuf = ReleaseAndReadBuffer(xscan->xs_cbuf, rel, blkno);
        xscan->xs_cblkno = blkno;
    }
    buf = xscan->xs_cbuf;

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    if (!pax_page_is_valid(BufferGetPage(buf)))
    {
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        elog(ERROR, "pax: invalid page %u in relation \"%s\"",
             blkno, RelationGetRelationName(rel));
    }

    pdesc = pax_build_page_desc(BufferGetPage(buf), RelationGetDescr(rel));
    tupno = (int) offset - 1;
    if (tupno >= pdesc->n_tuples)
    {
        /*
         * La page a été tronquée après la construction de l'index. L'entrée
         * ne désigne plus rien : on la déclare morte pour que l'AM d'index la
         * supprime au prochain passage, plutôt que d'échouer.
         */
        pax_free_page_desc(pdesc);
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        if (all_dead != NULL)
            *all_dead = true;
        return false;
    }

    meta = pdesc->tuple_meta[tupno];
    visible = !pax_meta_is_unused(&meta) &&
        pax_meta_satisfies_snapshot(&meta, snapshot);

    if (visible)
    {
        /*
         * Matérialisation sous le verrou de contenu : les valeurs sont
         * copiées, donc le pin pourra être relâché par l'appelant.
         */
        pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
        ExecMaterializeSlot(slot);
    }
    pax_free_page_desc(pdesc);
    LockBuffer(buf, BUFFER_LOCK_UNLOCK);

    return visible;
}

/*
 * Fait avancer l'horizon de conflit à partir des métadonnées d'une version.
 *
 * Même règle que HeapTupleHeaderAdvanceConflictHorizon() : une version dont
 * l'insertion a été annulée, ou que sa propre transaction a supprimée, ne peut
 * rien avoir touché, donc son xmax est ignoré. Seule une version insérée par
 * une transaction validée, et remplacée par une AUTRE transaction, borne les
 * lecteurs capables de voir ce que l'on supprime.
 */
static void
pax_meta_advance_conflict_horizon(const PaxTupleMetaData *meta,
                                  TransactionId *horizon)
{
    if (meta->xmin != FrozenTransactionId &&
        !TransactionIdDidCommit(meta->xmin))
        return;

    if (TransactionIdIsValid(meta->xmax) &&
        !TransactionIdEquals(meta->xmax, meta->xmin) &&
        TransactionIdPrecedes(*horizon, meta->xmax))
        *horizon = meta->xmax;
}

/*
 * Nettoyage d'index : détermine quelles entrées d'index pointant sur des
 * versions de PAX peuvent être supprimées.
 *
 * L'AM d'index fournit une liste de TID de table trouvés dans une de ses pages.
 * Pour chacun, il faut répondre « cette version est-elle supprimable ? ». Si
 * oui, l'entrée d'index correspondante peut disparaître.
 *
 * Contrairement à heap, une entrée d'index PAX ne pointe pas sur une chaîne
 * HOT regroupée sur une page : PAX ne fait pas de mise à jour HOT, et une
 * chaîne traverse les pages, t_ctid étant un offset physique. Chaque version
 * a donc sa propre entrée d'index, et le TID fourni est déjà celui de la
 * version à tester. Il n'y a pas de chaîne à parcourir : la seule nuance est
 * qu'un verrou de ligne encore actif empêche de conclure.
 *
 * On renvoie le « snapshotConflictHorizon » : la valeur qui borne les
 * transactions capables de voir les entrées supprimées. Elle sert en Hot
 * Standby à créer un conflit de récupération au bon moment.
 */
static TransactionId
pax_relation_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    TransactionId snapshotConflictHorizon = InvalidTransactionId;
    Buffer        buf = InvalidBuffer;
    BlockNumber   buf_blkno = InvalidBlockNumber;
    int           i;
    int           finalndeltids = 0;

    /*
     * Suppression d'index descendante (bottom-up) : nbtree nous demande de
     * considérer speculativement toutes les entrées d'une page d'index pour
     * eviter un remplissage. C'est une optimisation, pas une obligation :
     * refuser est toujours permis, l'AM d'index se contente alors de ne pas
     * progresser sur ce passage. PAX préfère refuser plutôt que d'inventer
     * une evaluation de cout comparable a celle de heap, qui repose sur la
     * visibilite MVCC et les tailles de page -- deux notions que PAX n'a pas.
     */
    if (delstate->bottomup)
    {
        delstate->ndeltids = 0;
        return InvalidTransactionId;
    }

    /*
     * Les TID sont triés par bloc : on ne relit la page qu'une fois par bloc.
     */
    for (i = 0; i < delstate->ndeltids; i++)
    {
        TM_IndexDelete     *deltid = &delstate->deltids[i];
        TM_IndexStatus     *istatus = delstate->status + deltid->id;
        ItemPointer         htid = &deltid->tid;
        BlockNumber         blkno = ItemPointerGetBlockNumber(htid);
        PaxPageDesc        *pdesc;
        PaxTupleMetaData    meta;
        int                 tupno;
        bool                deletable;

        CHECK_FOR_INTERRUPTS();

        if (blkno >= RelationGetNumberOfBlocks(rel))
        {
            /*
             * L'entrée désigne une page qui n'existe plus : la relation a pu
             * être tronquée après la construction de l'index. Ce n'est pas
             * récupérable ici et heap ne le traite pas non plus comme une
             * corruption ; on s'arrête et le reste du tableau sera retraité au
             * prochain passage de VACUUM sur l'index.
             */
            break;
        }

        if (!BufferIsValid(buf) || blkno != buf_blkno)
        {
            if (BufferIsValid(buf))
                UnlockReleaseBuffer(buf);
            buf = ReadBuffer(rel, blkno);
            buf_blkno = blkno;
        }

        LockBuffer(buf, BUFFER_LOCK_SHARE);
        if (!pax_page_is_valid(BufferGetPage(buf)))
        {
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            elog(ERROR, "pax: invalid page %u in relation \"%s\"",
                 blkno, RelationGetRelationName(rel));
        }

        pdesc = pax_build_page_desc(BufferGetPage(buf),
                                    RelationGetDescr(rel));
        tupno = (int) ItemPointerGetOffsetNumber(htid) - 1;

        if (tupno < 0 || tupno >= pdesc->n_tuples)
        {
            /*
             * L'index désigne un emplacement inexistant sur cette page. Un
             * index sain ne peut pas contenir cela : c'est de la corruption,
             * et, contrairement à heap qui le tolère, on le signale.
             */
            pax_free_page_desc(pdesc);
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("index \"%s\" contains an entry pointing to an"
                            " invalid tuple (%u,%u) in table \"%s\"",
                            RelationGetRelationName(delstate->irel), blkno,
                            ItemPointerGetOffsetNumber(htid),
                            RelationGetRelationName(rel)),
                     errhint("The index should be dropped and recreated.")));
        }

        meta = pdesc->tuple_meta[tupno];
        pax_free_page_desc(pdesc);
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);

        /*
         * Supprimable si la version est morte pour de bon et qu'aucun verrou
         * de ligne ne retient plus la version : un lecteur verrouillé pourrait
         * encore suivre la chaîne à partir d'elle.
         */
        deletable = pax_meta_is_unused(&meta) ||
            (pax_meta_classify(&meta) == PAX_VERSION_DEAD &&
             !pax_meta_has_live_locker(&meta));

        if (istatus->knowndeletable)
            deletable = true;

        if (!deletable)
            break;              /* les entrées suivantes ne sont pas examinées */

        pax_meta_advance_conflict_horizon(&meta, &snapshotConflictHorizon);

        finalndeltids = i + 1;
    }

    if (BufferIsValid(buf))
        UnlockReleaseBuffer(buf);

    /*
     * Les entrées non supprimables sont à la fin du tableau : le rétrécir
     * informe l'appelant. Ce n'est pas qu'une optimisation, l'AM d'index est
     * autorisé à s'appuyer sur ndeltids pour décider qu'il n'y a rien à faire.
     */
    delstate->ndeltids = finalndeltids;

    return snapshotConflictHorizon;
}

static TransactionId
pax_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    return pax_relation_index_delete_tuples(rel, delstate);
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

/*
 * Une charge utile (valeur de longueur variable) à déplacer lors d'une
 * compaction. old_off est l'offset courant dans la page, lu dans la table
 * d'offsets de la colonne.
 */
typedef struct PaxPayloadRef
{
    OffsetNumber  old_off;
    OffsetNumber  new_off;
    Size          len;        /* longueur alignée */
    int           col;
    int           tupno;
} PaxPayloadRef;

static int
pax_payload_ref_cmp(const void *a, const void *b)
{
    const PaxPayloadRef *x = (const PaxPayloadRef *) a;
    const PaxPayloadRef *y = (const PaxPayloadRef *) b;

    /* Décroissant : on traite le plus haut d'abord. */
    if (x->old_off > y->old_off)
        return -1;
    if (x->old_off < y->old_off)
        return 1;
    return 0;
}

/*
 * VACUUM, compactage des charges utiles des versions mortes.
 *
 * Les versions mortes sont marquées PAX_VERSION_UNUSED sur place : le format
 * interdit de retirer une version, car t_ctid est un offset PHYSIQUE
 * (page, indice de version) et les liens entrants pointent ici depuis d'autres
 * pages. Mais les octets qu'occupaient leurs valeurs de longueur variable, eux,
 * sont dans la zone pd_upper et peuvent être rendus sans toucher aux régions.
 *
 * C'est ce qui rend la place récupérée réellement réutilisable : sans cela,
 * INSERT ne réutilise un emplacement que s'il reste de la place contiguë, et
 * une page pleine n'en a presque jamais.
 *
 * Trois propriétés rendent l'opération sûre :
 *
 *  - n_tuples ne change pas, donc aucun index de version ne bouge et les liens
 *    t_ctid, y compris ceux pointant sur cette page depuis une autre, restent
 *    exacts. Seuls des octets de charge utile bougent.
 *  - les tables d'offsets de colonnes vivent sous pd_lower, dans les régions.
 *    Elles ne sont pas décalées : on y réécrit la nouvelle valeur en place.
 *  - un balayage relit la table d'offsets à chaque matérialisation et le fait
 *    sous le verrou de contenu, donc il ne peut pas observer un décalage à
 *    moitié fait. Son descripteur de page reste valide aussi : ni les offsets
 *    de régions ni meta_offset ne bougent.
 *
 * Les charges utiles sont déplacées en descendant depuis le haut de la zone,
 * ce qui garantit qu'aucune donnée source non encore copiée n'est écrasée :
 * chaque destination est à une adresse inférieure ou égale à sa source, et
 * toutes les sources au-dessus ont déjà été traitées.
 *
 * Renvoie true si des octets ont été récupérés.
 */
static bool
pax_vacuum_compact_payload(Page page, PaxPageHeader *phdr, TupleDesc tupdesc)
{
    PaxTupleMetaData  *meta;
    PaxPayloadRef     *refs;
    Size               n_refs = 0;
    Size               i;
    int                c;
    int                t;
    int                n_tuples = phdr->n_tuples;
    Size               top = BLCKSZ - SizeOfPaxSpecialData;
    Size               cursor;
    Size               old_upper;
    bool               changed = false;

    if (n_tuples == 0)
        return false;

    meta = (PaxTupleMetaData *) ((char *) page + phdr->meta_offset);

    refs = (PaxPayloadRef *) palloc(sizeof(PaxPayloadRef) *
                                    (Size) n_tuples *
                                    (Size) MaxTupleAttributeNumber);
    memset(refs, 0, sizeof(PaxPayloadRef) * (Size) n_tuples *
           (Size) MaxTupleAttributeNumber);

    for (c = 0; c < tupdesc->natts; c++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, c);
        Size              bmp_size;
        Size              region_start;
        bits8            *bmp;

        if (attr->attlen >= 0)
            continue;                   /* valeur en ligne dans la région */
        if (!PaxOffsetIsValid(phdr->offsets[c]))
            continue;                   /* colonne absente : tout est NULL */

        region_start = phdr->offsets[c];
        bmp_size = pax_bitmap_size(n_tuples);
        bmp = (bits8 *) ((char *) page + region_start);

        for (t = 0; t < n_tuples; t++)
        {
            OffsetNumber off;
            Size         len;

            /* Une version morte n'est plus lue par personne. */
            if (pax_meta_is_unused(&meta[t]))
                continue;
            if (att_isnull(t, bmp))
                continue;

            memcpy(&off, (char *) page + region_start + bmp_size +
                   (Size) t * sizeof(OffsetNumber), sizeof(OffsetNumber));
            if (!PaxOffsetIsValid(off))
                continue;

            if (attr->attlen == -2)
                len = strlen((char *) page + off) + 1;
            else
                len = VARSIZE_ANY((char *) page + off);

            refs[n_refs].old_off = off;
            refs[n_refs].len = MAXALIGN(len);
            refs[n_refs].col = c;
            refs[n_refs].tupno = t;
            n_refs++;
        }
    }

    if (n_refs == 0)
    {
        pfree(refs);
        /* Toutes les versions sont mortes ou la page n'a aucune valeur
         * variable : la zone de charge utile est entièrement libérable. */
        changed = ((Size) ((PageHeader) page)->pd_upper != top);
        ((PageHeader) page)->pd_upper = (LocationIndex) top;
        return changed;
    }

    qsort(refs, n_refs, sizeof(PaxPayloadRef), pax_payload_ref_cmp);

    cursor = top;
    for (i = 0; i < n_refs; i++)
    {
        PaxPayloadRef *r = &refs[i];

        cursor -= r->len;
        r->new_off = (OffsetNumber) cursor;

        if (r->new_off != r->old_off)
        {
            memmove((char *) page + cursor, (char *) page + r->old_off,
                    r->len);
            changed = true;
        }
    }

    /* Réécrit les tables d'offsets avec les nouvelles positions. */
    for (i = 0; i < n_refs; i++)
    {
        PaxPayloadRef *r = &refs[i];
        Size           region_start = phdr->offsets[r->col];
        Size           bmp_size = pax_bitmap_size(n_tuples);

        if (r->new_off == r->old_off)
            continue;

        memcpy((char *) page + region_start + bmp_size +
               (Size) r->tupno * sizeof(OffsetNumber),
               &r->new_off, sizeof(OffsetNumber));
    }

    /*
     * pd_upper remonte : les charges utiles occupent [cursor, top) au lieu de
     * [old_upper, top), donc la zone [old_upper, cursor) redevient libre.
     *
     * On l'efface : elle contenait des valeurs mortes, et une page brute
     * lisible par pageinspect ne doit pas les révéler.
     */
    if (changed)
    {
        old_upper = (Size) ((PageHeader) page)->pd_upper;
        Assert(cursor >= old_upper);
        memset((char *) page + old_upper, 0, cursor - old_upper);
    }

    ((PageHeader) page)->pd_upper = (LocationIndex) cursor;

    pfree(refs);

    return changed;
}

/*
 * VACUUM, passe de nettoyage sur une page.
 *
 * Contrainte propre au format : les liens t_ctid sont des offsets PHYSIQUES
 * (page, indice de version). Retirer une version décalerait tous les slots
 * suivants de toutes les colonnes, et casserait les liens entrants pointant
 * sur cette page depuis d'autres. On ne compacte donc pas les versions : une
 * version morte est marquée PAX_VERSION_UNUSED sur place, son emplacement
 * restant réutilisable par INSERT. Seules les charges utiles des versions
 * mortes sont compactées (pax_vacuum_compact_payload), ce qui ne touche ni
 * n_tuples ni les régions.
 *
 * Renvoie true si la page a été modifiée (pour rejouer le WAL plus bas).
 */
static bool
pax_vacuum_page(Relation rel, Buffer buf, BlockNumber blkno,
                const VacuumParams *params, double *tups_vacuumed,
                double *tups_recently_dead, bool *page_all_unused,
                TidStore *dead_items)
{
    Page            page;
    PaxPageHeader  *phdr;
    GenericXLogState *wal_state;
    PaxTupleMetaData *meta;
    int              i;
    OffsetNumber     dead_offsets[MaxOffsetNumber];
    int              n_dead = 0;
    int              n_recent = 0;
    bool             changed = false;
    bool             all_unused;
    bool             freezing = (params->options & VACOPT_FREEZE) != 0;
    TransactionId    oldest = GetOldestNonRemovableTransactionId(rel);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buf);

    if (!pax_page_is_valid(page))
    {
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pax: invalid page %u while vacuuming", blkno);
    }

    phdr = PaxPageHeaderPtr(page);

    if (phdr->n_tuples == 0)
    {
        *page_all_unused = true;
        UnlockReleaseBuffer(buf);
        return false;
    }

    wal_state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(wal_state, buf, 0);
    phdr = PaxPageHeaderPtr(page);

    meta = (PaxTupleMetaData *) ((char *) page + phdr->meta_offset);
    all_unused = true;

    for (i = 0; i < phdr->n_tuples; i++)
    {
        PaxTupleMetaData *m = &meta[i];

        if (pax_meta_is_unused(m))
            continue;

        switch (pax_meta_classify(m))
        {
            case PAX_VERSION_RECENT:
                /*
                 * Transaction en cours : la version reste, et ne compte pas
                 * comme morte.
                 */
                all_unused = false;
                n_recent++;
                continue;

            case PAX_VERSION_LIVE:
                all_unused = false;

                /*
                 * Gel. Une version dont xmin est validé et antérieur au plus
                 * vieux xmin encore actif ne peut plus être vue par personne :
                 * on la fige. Le champ existe déjà en page, la taille sur
                 * disque ne change pas.
                 */
                if (freezing &&
                    TransactionIdIsValid(m->xmin) &&
                    m->xmin != FrozenTransactionId &&
                    TransactionIdDidCommit(m->xmin) &&
                    TransactionIdPrecedes(m->xmin, oldest))
                {
                    m->xmin = FrozenTransactionId;
                    changed = true;
                }
                continue;

            case PAX_VERSION_DEAD:
                break;
        }

        /*
         * Version morte. Un verrou de ligne encore actif peut empêcher un
         * lectureur de suivre la chaîne : on ne marque pas alors.
         */
        if (pax_meta_has_live_locker(m))
        {
            all_unused = false;
            n_recent++;
            continue;
        }

        if (m->locker_mxid != InvalidMultiXactId)
        {
            /* Plus aucun membre actif : on lâche la référence au MultiXact. */
            m->locker_mxid = InvalidMultiXactId;
        }
        m->flags |= PAX_VERSION_UNUSED;
        changed = true;

        /*
         * On note le TID pour le nettoyage des index. L'offset de TID vaut
         * tupno + 1, comme partout ailleurs dans le format.
         */
        dead_offsets[n_dead] = (OffsetNumber) (i + 1);
        n_dead++;
    }

    /*
     * Compactage des charges utiles, dans la même image WAL : la page est
     * réécrite au completion, donc le compactage est rejoué en cas de crash.
     *
     * On compacte même si aucune version n'a été marquée, car une passe
     * précédente a pu laisser des charges utiles mortes derrière elle.
     */
    if (pax_vacuum_compact_payload(page, phdr, RelationGetDescr(rel)))
        changed = true;

    if (changed)
    {
        phdr->free_space = (uint16) pax_page_free_space(page);
        GenericXLogFinish(wal_state);
    }
    else
        GenericXLogAbort(wal_state);

    UnlockReleaseBuffer(buf);

    *page_all_unused = all_unused;
    *tups_vacuumed += n_dead;
    *tups_recently_dead += n_recent;

    /*
     * Les TID des versions viennent de mourir sont versés au fichier, pour que
     * les AM d'index puissent décider quelles entrées supprimer. Cela se fait
     * APRÈS le déverrouillage : la liste n'est utile qu'à la passe d'index, qui
     * la relira sous ses propres verrous.
     */
    if (n_dead > 0 && dead_items != NULL)
    {
        CHECK_FOR_INTERRUPTS();
        TidStoreSetBlockOffsets(dead_items, blkno, dead_offsets, n_dead);
    }

    return changed;
}

/*
 * Réponse de l'AM d'index qui demande si une version de table est morte.
 *
 * Elle ne connaît que le TID : on le cherche donc dans le fichier des versions
 * mortes récolté pendant la passe sur la table. C'est exactement le mécanisme
 * de heap, dont le callback fait la même chose.
 */
static bool
pax_vac_tid_reaped(ItemPointer itemptr, void *state)
{
    return TidStoreIsMember((TidStore *) state, itemptr);
}

/*
 * Nettoyage des index de la relation.
 *
 * Chaque AM d'index parcourt ses propres entrées et interroge la table pour
 * savoir lesquelles peuvent disparaître. On lui fournit l'index et le fichier
 * des TID morts ; c'est ce que fait vac_bulkdel_one_index() pour heap, en deux
 * temps : suppression, puis nettoyage final qui rend les pages libérées.
 */
static void
pax_vacuum_indexes(Relation rel, TidStore *dead_items,
                   BufferAccessStrategy bstrategy)
{
    List       *indexoidlist;
    ListCell   *lc;

    indexoidlist = RelationGetIndexList(rel);

    foreach(lc, indexoidlist)
    {
        Relation            indrel;
        IndexVacuumInfo      ivinfo;
        IndexBulkDeleteResult *istat;

        /*
         * index_open() n'acquiert aucun verrou avec NoLock, alors que le
         * tampon partage est modifie par nbtree pendant index_bulk_delete().
         * On tient donc un AccessShareLock explicite, liberé au meme endroit.
         */
        indrel = index_open(lfirst_oid(lc), AccessShareLock);

        ivinfo.index = indrel;
        ivinfo.heaprel = rel;
        ivinfo.analyze_only = false;
        ivinfo.report_progress = false;
        ivinfo.estimated_count = true;
        ivinfo.message_level = DEBUG2;
        ivinfo.num_heap_tuples = 0;
        ivinfo.strategy = bstrategy;

        /*
         * L'AM d'index parcourt ses entrées et demande si la version visée est
         * morte ; pax_vac_tid_reaped() répond par le fichier ci-dessus. Le
         * chemin « suppression descendante » passe, lui, par
         * pax_relation_index_delete_tuples(), qui consulte les pages.
         */
        istat = index_bulk_delete(&ivinfo, NULL, pax_vac_tid_reaped,
                                  dead_items);

        istat = index_vacuum_cleanup(&ivinfo, istat);
        pfree(istat);

        index_close(indrel, AccessShareLock);
    }

    list_free(indexoidlist);
}

static void
pax_relation_vacuum(Relation rel, const VacuumParams *params,
                     BufferAccessStrategy bstrategy)
{
    BufferAccessStrategy strat = bstrategy;
    BlockNumber  nblocks;
    BlockNumber  blkno;
    BlockNumber  nkept = 0;
    double       tups_vacuumed = 0;
    double       tups_recently_dead = 0;
    TidStore    *dead_items;
    /*
     * Même règle que vacuumlazy : un autovacuum peut avoir sa propre mémoire de
     * travail, bornée pour ne pas laisser un seul VACUUM consommer celle du
     * cluster entier.
     */
    int          vac_work_mem = AmAutoVacuumWorkerProcess() &&
        autovacuum_work_mem != -1 ?
        autovacuum_work_mem : maintenance_work_mem;

    /*
     * File des TID de versions mortes.
     *
     * C'est ce qui permet de nettoyer les index. L'AM d'index vaWalking ses
     * entrées et nous demandera, pour chacune, « cette version est-elle
     * morte ? » ; on répondra par l'appartenance au fichier. Sans lui, aucune
     * entrée d'index ne peut être supprimée, alors que la table elle-même est
     * bien nettoyée.
     *
     * On ne peut pas réutiliser celui de heap : il est créé et alimenté
     * entièrement dans vacuumlazy.c, et il n'existe aucun callback d'AM de
     * table pour y verser les TID morts. D'où ce fichier local.
     */
    dead_items = TidStoreCreateLocal((Size) vac_work_mem * 1024, true);

    nblocks = RelationGetNumberOfBlocks(rel);

    for (blkno = 0; blkno < nblocks; blkno++)
    {
        Buffer buf;
        bool   all_unused;

        CHECK_FOR_INTERRUPTS();

        buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, strat);
        (void) pax_vacuum_page(rel, buf, blkno, params,
                               &tups_vacuumed, &tups_recently_dead,
                               &all_unused, dead_items);

        if (all_unused)
            nkept = blkno;       /* encore traçable jusqu'ici */
        else
            nkept = blkno + 1;   /* cette page reste utilisée */
    }

    /*
     * Troncature des pages finales entièrement inutilisées.
     *
     * Comme heap, on exige AccessExclusiveLock : entre le nettoyage et la
     * troncature, un inserter a pu ajouter des lignes sur ces pages.
     */
    if (nkept < nblocks)
    {
        BlockNumber new_rel_pages = nkept;

        if (!ConditionalLockRelation(rel, AccessExclusiveLock))
        {
            UnlockRelation(rel, AccessExclusiveLock);
            goto out;
        }

        /*
         * Reverifier depuis la fin : le nombre de pages a pu changer, et des
         * lignes ont pu être insérées pendant le nettoyage.
         */
        while (new_rel_pages > 0)
        {
            Buffer buf;
            Page   page;
            PaxPageHeader *phdr;
            bool   empty;
            int    i;
            int    n_tuples;

            CHECK_FOR_INTERRUPTS();

            buf = ReadBufferExtended(rel, MAIN_FORKNUM, new_rel_pages - 1,
                                      RBM_NORMAL, NULL);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buf);

            if (!pax_page_is_valid(page))
            {
                UnlockReleaseBuffer(buf);
                break;
            }

            phdr = PaxPageHeaderPtr(page);
            n_tuples = phdr->n_tuples;
            empty = true;

            for (i = 0; i < n_tuples; i++)
            {
                PaxTupleMetaData *m;

                m = ((PaxTupleMetaData *)
                     ((char *) page + phdr->meta_offset)) + i;
                if (!pax_meta_is_unused(m))
                {
                    empty = false;
                    break;
                }
            }
            UnlockReleaseBuffer(buf);

            if (!empty)
                break;
            new_rel_pages--;
        }

        if (new_rel_pages < RelationGetNumberOfBlocks(rel))
        {
            /* Les entrées FSM des pages retirées ne désignent plus rien. */
            BlockNumber b;

            for (b = new_rel_pages; b < nblocks; b++)
                RecordPageWithFreeSpace(rel, b, 0);

            RelationTruncate(rel, new_rel_pages);
        }
        UnlockRelation(rel, AccessExclusiveLock);
    }

out:
    /* Le FSM est renseigné à l'insertion ; on le remet à jour. */
    for (blkno = 0; blkno < RelationGetNumberOfBlocks(rel); blkno++)
    {
        Buffer buf;
        Size   free_space;

        buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, strat);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        free_space = pax_page_free_space(BufferGetPage(buf));
        UnlockReleaseBuffer(buf);

        RecordPageWithFreeSpace(rel, blkno, free_space);
    }

    /*
     * Nettoyage des index, après la table : c'est l'ordre qu'exige la
     * suppression d'une entrée, puisque l'AM d'index doit pouvoir constater que
     * la version qu'elle désigne est morte.
     *
     * On reproduit ici ce que fait vacuumlazy.c pour heap. Le fichier de TID
     * morts est le nôtre, et non celui de heap : il n'existe aucun callback
     * d'AM de table pour alimenter celui de vacuumlazy, qui reste donc
     * inaccessible à qui n'est pas heap.
     */
    if (tups_vacuumed > 0 && (params->options & VACOPT_SKIP_LOCKED) == 0)
        pax_vacuum_indexes(rel, dead_items, strat);

    TidStoreDestroy(dead_items);
}

/*
 * ANALYZE. La boucle externe choisit les pages via le read stream, la boucle
 * interne les parcourt. Le verrou de contenu est tenu pendant toute la page :
 * un inserter ne peut donc pas y faire de memmove, et la mise en page reste
 * valide d'un tuple au suivant.
 */
static bool
pax_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
    PaxScanDesc  sdesc = (PaxScanDesc) scan;
    Relation     rel = scan->rs_rd;
    TupleDesc    tupdesc = RelationGetDescr(rel);
    Buffer       buf;
    Page         page;
    PaxPageDesc *desc;

    CHECK_FOR_INTERRUPTS();

    buf = read_stream_next_buffer(stream, NULL);
    if (!BufferIsValid(buf))
    {
        sdesc->analyze_done = true;
        return false;
    }

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);

    /*
     * Une page vide ou non initialisée n'a rien à échantillonner : on la
     * saute, sans erreur, comme heap le fait pour une page sans line pointer.
     * Le contrat autorise à renvoyer false pour un bloc non échantillonnable.
     */
    if (!pax_page_is_valid(page))
    {
        UnlockReleaseBuffer(buf);
        return false;
    }

    sdesc->current_buf = buf;
    sdesc->current_block = BufferGetBlockNumber(buf);
    sdesc->current_tupno = -1;
    sdesc->analyze_done = false;

    pax_scan_drop_cached_desc(sdesc);
    desc = pax_scan_page_desc(sdesc, page, sdesc->current_block, tupdesc);

    /* Une page sans version n'a rien à échantillonner. */
    if (desc->n_tuples == 0)
        sdesc->analyze_done = true;

    return true;
}

static bool
pax_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
                            double *deadrows, TupleTableSlot *slot)
{
    PaxScanDesc sdesc = (PaxScanDesc) scan;
    Relation    rel = scan->rs_rd;
    TupleDesc   tupdesc = RelationGetDescr(rel);

    while (!sdesc->analyze_done)
    {
        Page         page;
        PaxPageDesc *desc;
        int          tupno;
        PaxVersionState state;

        page = BufferGetPage(sdesc->current_buf);
        desc = pax_scan_page_desc(sdesc, page, sdesc->current_block, tupdesc);

        tupno = pax_find_visible_tuple(sdesc, desc,
                                       sdesc->current_tupno + 1);

        /*
         * pax_find_visible_tuple filtre sur le snapshot du scan, qui est
         * SnapshotAny pour une analyse : il ne fait donc ici que sauter les
         * versions inutilisées. Le décompte mort/vivant est fait séparément.
         */
        if (tupno < 0)
        {
            sdesc->analyze_done = true;
            break;
        }

        sdesc->current_tupno = tupno;
        state = pax_meta_classify(&desc->tuple_meta[tupno]);

        switch (state)
        {
            case PAX_VERSION_LIVE:
                *liverows += 1;
                pax_store_tuple_slot(rel, slot, sdesc->current_buf, desc,
                                     tupno, false);
                ExecMaterializeSlot(slot);
                return true;

            case PAX_VERSION_DEAD:
                *deadrows += 1;
                break;

            case PAX_VERSION_RECENT:
            default:
                /*
                 * Ligne en cours d'insertion ou de suppression : ni comptée
                 * comme morte (elle ne l'est pas encore), ni échantillonnée,
                 * comme le fait heap pour INSERT_IN_PROGRESS.
                 */
                break;
        }
    }

    /* Page épuisée : on rend le tampon, la boucle externe prend la suite. */
    LockBuffer(sdesc->current_buf, BUFFER_LOCK_UNLOCK);
    pax_scan_unpin_current(sdesc);
    sdesc->current_tupno = -1;
    pax_scan_drop_cached_desc(sdesc);

    return false;
}

/*
 * Construction d'index (CREATE INDEX).
 *
 * Le parcours est celui d'un balayage séquentiel ordinaire : on materialize
 * chaque version et on la passe à l'AM d'index. Deux différences avec heap
 * méritent d'être notées.
 *
 * Pas de chaînes HOT. Heap n'indexe que la version vivante d'une chaîne et la
 * rattache au TID de la racine, ce qui préserve la chaîne et permet à
 * PostgreSQL de ne pas mettre à jour l'index. PAX n'a pas d'équivalent :
 * chaque version a sa propre entrée d'index, ce qui est plus coûteux à
 * l'insertion mais beaucoup plus simple, et correct dès lors que la chaîne est
 * suivie à la lecture (pax_index_fetch_walk). La contrepartie est qu'un UPDATE
 * qui modifie une colonne indexée doit réinsérer une entrée d'index.
 *
 * Le snapshot. Comme heap, une construction non concurrente utilise
 * SnapshotAny et fait son propre contrôle de viabilité, parce qu'elle doit
 * indexer les versions RECENTLY_DEAD pour ne pas casser les transactions
 * déjà ouvertes. Une construction concurrente, elle, prend un snapshot MVCC et
 * indexe ce qui est visible.
 */
static double
pax_index_build_range_scan(Relation table_rel, Relation index_rel,
                           IndexInfo *index_info, bool allow_sync,
                           bool anyvisible, bool progress,
                           BlockNumber start_blockno, BlockNumber numblocks,
                           IndexBuildCallback callback, void *callback_state,
                           TableScanDesc scan)
{
    bool        is_system_catalog = IsSystemRelation(table_rel);
    bool        checking_uniqueness;
    Snapshot    snapshot;
    bool        need_unregister_snapshot = false;
    TransactionId OldestXmin = InvalidTransactionId;
    double      reltuples = 0;
    ExprState  *predicate;
    TupleTableSlot *slot;
    EState     *estate;
    ExprContext *econtext;
    BlockNumber previous_blkno = InvalidBlockNumber;
    ScanDirection direction = ForwardScanDirection;
    double      processed = 0;
    double      total = 0;

    Datum       values[INDEX_MAX_KEYS];
    bool        isnull[INDEX_MAX_KEYS];

    Assert(OidIsValid(index_rel->rd_rel->relam));

    checking_uniqueness = (index_info->ii_Unique ||
                           index_info->ii_ExclusionOps != NULL);

    Assert(!(anyvisible && checking_uniqueness));

    estate = CreateExecutorState();
    econtext = GetPerTupleExprContext(estate);
    slot = table_slot_create(table_rel, NULL);
    econtext->ecxt_scantuple = slot;

    predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

    if (!IsBootstrapProcessingMode() && !index_info->ii_Concurrent)
        OldestXmin = GetOldestNonRemovableTransactionId(table_rel);

    if (!scan)
    {
        if (!TransactionIdIsValid(OldestXmin))
        {
            snapshot = RegisterSnapshot(GetTransactionSnapshot());
            need_unregister_snapshot = true;
        }
        else
            snapshot = SnapshotAny;

        scan = table_beginscan_strat(table_rel, snapshot, 0, NULL,
                                     true, allow_sync);
    }
    else
        snapshot = scan->rs_snapshot;

    Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
    Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
           !TransactionIdIsValid(OldestXmin));
    Assert(snapshot == SnapshotAny || !anyvisible);

    if (progress)
    {
        total = (double) RelationGetNumberOfBlocks(table_rel);
        pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
                                     (uint64) total);
    }

    /*
     * Limites de parcours. Le balayage de PAX n'implémente pas le syncscan, donc
     * les bornes sont toujours posées explicitement.
     */
    if (!allow_sync || start_blockno != 0 ||
        numblocks != InvalidBlockNumber)
    {
        ItemPointerData mintid;
        ItemPointerData maxtid;

        /*
         * On traduit la plage de blocs en plage de TID : les versions d'une
         * page sont contiguës en TID, donc une plage de blocs est exactement
         * une plage de TID. C'est ce que table_rescan_tidrange attend, et
         * PAX n'implémente pas le syncscan.
         */
        if (start_blockno > 0)
            ItemPointerSet(&mintid, start_blockno - 1, 0);
        else
            ItemPointerSetInvalid(&mintid);

        if (numblocks != InvalidBlockNumber)
        {
            BlockNumber last = start_blockno + numblocks;

            if (last > RelationGetNumberOfBlocks(table_rel))
                last = RelationGetNumberOfBlocks(table_rel);
            ItemPointerSet(&maxtid, last, MaxOffsetNumber);
        }
        else
            ItemPointerSetInvalid(&maxtid);

        table_rescan_tidrange(scan, &mintid, &maxtid);
    }

    for (;;)
    {
        bool        tupleIsAlive;
        TransactionId xwait;
        PaxScanDesc pscan = (PaxScanDesc) scan;

        CHECK_FOR_INTERRUPTS();

    recheck:

        if (!table_scan_getnextslot(scan, direction, slot))
            break;

        if (pscan->current_block != previous_blkno)
        {
            previous_blkno = pscan->current_block;
            if (progress)
            {
                processed = (double) (previous_blkno + 1);
                pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
                                             (uint64) processed);
            }
        }

        if (snapshot == SnapshotAny)
        {
            /*
             * Contrôle de viabilité explicite, sur les métadonnées de version
             * copiées dans le slot. Même critère que
             * pax_scan_analyze_next_tuple(), sinon CREATE INDEX et ANALYZE
             * produiraient des valeurs de reltuples très différentes.
             */
            PaxTupleMetaData meta = ((PaxTupleTableSlot *) slot)->tuple_meta;

            /* Place libérée par VACUUM : elle n'a plus de lecture à faire. */
            if (pax_meta_is_unused(&meta))
                continue;

            switch (pax_meta_classify(&meta))
            {
                case PAX_VERSION_DEAD:
                    /* Définitivement morte : ni indexée ni comptée. */
                    continue;

                case PAX_VERSION_LIVE:
                    tupleIsAlive = true;
                    reltuples += 1;
                    break;

                case PAX_VERSION_RECENT:
                    /*
                     * On l'indexe quand même : une transaction déjà ouverte
                     * doit pouvoir s'en servir une fois l'index construit. Elle
                     * ne compte pas dans reltuples.
                     */
                    tupleIsAlive = false;
                    break;
            }

            if (TransactionIdIsInProgress(meta.xmin) &&
                !TransactionIdIsCurrentTransactionId(meta.xmin))
            {
                xwait = meta.xmin;

                /*
                 * Inattendu hors catalogue système : cela signifie qu'une
                 * insertion concurrente a eu lieu alors qu'on tient un
                 * ShareLock. Heap avertit aussi dans ce cas.
                 */
                if (!is_system_catalog)
                    elog(WARNING,
                         "concurrent insert in progress within table \"%s\"",
                         RelationGetRelationName(table_rel));

                /*
                 * Indexer une insertion en cours ferait échouer une unique
                 * à tort : on attend la fin de la transaction, puis on
                 * réexamine.
                 */
                if (checking_uniqueness)
                {
                    XactLockTableWait(xwait, table_rel, &slot->tts_tid,
                                      XLTW_InsertIndexUnique);
                    CHECK_FOR_INTERRUPTS();
                    goto recheck;
                }

                tupleIsAlive = true;
                reltuples += 1;
            }
            else if (TransactionIdIsInProgress(meta.xmax) &&
                     !TransactionIdIsCurrentTransactionId(meta.xmax) &&
                     !TransactionIdIsCurrentTransactionId(meta.xmin))
            {
                xwait = meta.xmax;

                if (!is_system_catalog)
                    elog(WARNING,
                         "concurrent delete in progress within table \"%s\"",
                         RelationGetRelationName(table_rel));

                /*
                 * Supposer la ligne morte ferait manquer une violation
                 * d'unicité, donc on attend avant de conclure.
                 */
                if (checking_uniqueness)
                {
                    XactLockTableWait(xwait, table_rel, &slot->tts_tid,
                                      XLTW_InsertIndexUnique);
                    CHECK_FOR_INTERRUPTS();
                    goto recheck;
                }

                /*
                 * Sans contrôle d'unicité, on l'indexe mais sans la compter,
                 * comme pour une version récemment morte.
                 */
                tupleIsAlive = true;
                reltuples += 1;
            }
        }
        else
        {
            /* table_scan_getnextslot a fait le contrôle de temps. */
            tupleIsAlive = true;
            reltuples += 1;
        }

        MemoryContextReset(econtext->ecxt_per_tuple_memory);

        /* Index partiel : on écarte ce qui ne satisfait pas le prédicat. */
        if (predicate != NULL && !ExecQual(predicate, econtext))
            continue;

        /*
         * Extrait les attributs utilisés par l'index, et évalue les
         * expressions éventuelles.
         */
        FormIndexDatum(index_info, slot, estate, values, isnull);

        /*
         * PAX n'a pas de chaîne HOT : le TID à indexer est celui de la version
         * materializee, tel quel.
         */
        callback(index_rel, &slot->tts_tid, values, isnull, tupleIsAlive,
                 callback_state);
    }

    if (progress)
        pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
                                     (uint64) total);

    table_endscan(scan);

    if (need_unregister_snapshot)
        UnregisterSnapshot(snapshot);

    ExecDropSingleTupleTableSlot(slot);
    FreeExecutorState(estate);

    /* Ils pointaient sur l'estate qui vient d'être détruit. */
    index_info->ii_ExpressionsState = NIL;
    index_info->ii_PredicateState = NULL;

    return reltuples;
}

/*
 * Validation d'un index en cours de construction (CREATE INDEX CONCURRENTLY).
 *
 * Deuxième passage, destiné à attraper les écritures concurrentes que le
 * premier passage a pu manquer. La table est rejouée dans le snapshot de
 * l'appelant, et ses TID sont fusionnés avec ceux que l'index contient, triés
 * dans un tuplesort.
 *
 * Contrairement à ce que suggère le nom, ce passage ne se contente pas de
 * signaler : comme heap, il RÉINSÈRE les entrées manquantes. C'est ce qui rend
 * CREATE INDEX CONCURRENTLY capable d'aboutir malgré les écritures concurrentes.
 *
 * PAX n'a pas de chaînes HOT, donc pas de TID racine à reconstruire : le TID de
 * la version materializee est directement celui à comparer et à réinsérer.
 */
static void
pax_index_validate_scan(Relation table_rel, Relation index_rel,
                        IndexInfo *index_info, Snapshot snapshot,
                        ValidateIndexState *state)
{
    TableScanDesc scan;
    TupleTableSlot *slot;
    EState       *estate;
    ExprContext   *econtext;
    ExprState     *predicate;
    ItemPointer    indexcursor = NULL;
    ItemPointerData decoded;
    bool          tuplesort_empty = false;
    BlockNumber   previous_blkno = InvalidBlockNumber;
    PaxScanDesc   pscan;

    Datum         values[INDEX_MAX_KEYS];
    bool          isnull[INDEX_MAX_KEYS];

    Assert(OidIsValid(index_rel->rd_rel->relam));

    estate = CreateExecutorState();
    econtext = GetPerTupleExprContext(estate);
    slot = table_slot_create(table_rel, NULL);
    econtext->ecxt_scantuple = slot;

    predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

    scan = table_beginscan(table_rel, snapshot, 0, NULL, 0);
    pscan = (PaxScanDesc) scan;

    pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
                                 RelationGetNumberOfBlocks(table_rel));

    while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        ItemPointerData heapcursor = slot->tts_tid;

        CHECK_FOR_INTERRUPTS();

        state->htups += 1;

        if ((previous_blkno == InvalidBlockNumber) ||
            (pscan->current_block != previous_blkno))
        {
            pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
                                         pscan->current_block);
            previous_blkno = pscan->current_block;
        }

        /*
         * Fusion : on avance dans le tuplesort tant qu'il n'a pas atteint ou
         * dépassé le TID courant de la table.
         */
        while (!tuplesort_empty &&
               (!indexcursor ||
                ItemPointerCompare(indexcursor, &heapcursor) < 0))
        {
            Datum ts_val;
            bool  ts_isnull;

            tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
                                                   false, &ts_val, &ts_isnull,
                                                   NULL);
            Assert(tuplesort_empty || !ts_isnull);
            if (!tuplesort_empty)
            {
                itemptr_decode(&decoded, DatumGetInt64(ts_val));
                indexcursor = &decoded;
            }
            else
                indexcursor = NULL;   /* reste propre */
        }

        /*
         * Le tuplesort a dépassé (ou est épuisé) sans avoir rencontré ce TID :
         * l'entrée manque dans l'index, on la réinsère.
         */
        if (tuplesort_empty ||
            ItemPointerCompare(indexcursor, &heapcursor) > 0)
        {
            MemoryContextReset(econtext->ecxt_per_tuple_memory);

            /* Index partiel : on écarte ce qui ne satisfait pas le prédicat. */
            if (predicate != NULL && !ExecQual(predicate, econtext))
                continue;

            FormIndexDatum(index_info, slot, estate, values, isnull);

            /*
             * Une version morte mais récemment morte doit tout de même être
             * indexée, sinon une transaction déjà ouverte ne la reverrait pas
             * alors qu'elle l'a peut-être indexée elle-même. Le contrôle
             * d'unicité reste en revanche nécessaire : c'est lui qui détecte un
             * conflit réel.
             */
            index_insert(index_rel, values, isnull, &heapcursor, table_rel,
                         index_info->ii_Unique ?
                         UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
                         false, index_info);

            state->tups_inserted += 1;
        }
    }

    table_endscan(scan);

    ExecDropSingleTupleTableSlot(slot);
    FreeExecutorState(estate);

    /* Ils pointaient sur l'estate qui vient d'être détruit. */
    index_info->ii_ExpressionsState = NIL;
    index_info->ii_PredicateState = NULL;
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

    phdr = PaxPageHeaderPtr(page);

    phdr->n_tuples   = 0;
    phdr->free_space = (uint16) (BLCKSZ - (SizeOfPageHeaderData + header_size));
    phdr->flags      = PAX_FLAG_HAS_XMIN_XMAX | PAX_FLAG_HAS_VERSIONS;

    for (i = 0; i < n_attrs; i++)
        phdr->offsets[i] = InvalidOffsetNumber;

    ((PageHeader) page)->pd_lower =
        (LocationIndex) (SizeOfPageHeaderData + header_size);
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
 * Construit la partie « mise en page » d'un PaxPageDesc : géométrie des
 * régions, pas des slots, pointeurs vers les bitmap et les valeurs.
 *
 * Cette partie ne dépend QUE de l'en-tête de page. Elle reste donc valable
 * d'une tupline à l'autre tant que n_tuples, meta_offset, flags et offsets[]
 * ne changent pas : c'est ce que permet de la mettre en cache
 * (pax_layout_is_current).
 *
 * Les métadonnées de version, elles, sont relues à chaque appel par
 * pax_refresh_tuple_meta() car elles changent au fil des UPDATE et DELETE
 * concurrents.
 */
static PaxPageDesc *
pax_build_page_layout(Page page, BlockNumber blkno, TupleDesc tupdesc)
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

    /* Instantané indépendant, pour la revalidation (voir PaxPageDesc). */
    desc->stamp_blkno      = blkno;
    desc->stamp_flags      = phdr->flags;
    desc->stamp_meta_offset = phdr->meta_offset;
    desc->stamp_offsets    = (OffsetNumber *)
        palloc(sizeof(OffsetNumber) * desc->n_attrs);
    memcpy(desc->stamp_offsets, phdr->offsets,
           sizeof(OffsetNumber) * desc->n_attrs);

    /*
     * Tampon des métadonnées de version, dimensionné une fois pour la page.
     * Le contenu est ensuite rempli à la demande (pax_meta_ensure) ou en
     * bloc (pax_refresh_tuple_meta), selon le chemin appelant.
     *
     * La taille de la région de métadonnées est une propriété de la mise en
     * page : elle est vérifiée ici pour que le chemin paresseux en bénéficie
     * autant que la revalidation complète.
     */
    if (desc->n_tuples > 0)
    {
        if (!PaxOffsetIsValid(phdr->meta_offset))
            elog(ERROR, "pax: missing transaction metadata region");
        if (pax_meta_region_size(page, phdr) !=
            (Size) desc->n_tuples * SizeOfPaxTupleMetaData)
            elog(ERROR, "pax: inconsistent transaction metadata region "
                        "(region %zu, %d tuples x %zu)",
                 pax_meta_region_size(page, phdr), desc->n_tuples,
                 SizeOfPaxTupleMetaData);
    }

    desc->tuple_meta = (PaxTupleMetaData *)
        palloc(SizeOfPaxTupleMetaData * (desc->n_tuples > 0 ? desc->n_tuples : 1));
    desc->meta_capacity = desc->n_tuples;
    desc->meta_checked_through = 0;

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

/*
 * Vrai si la mise en page en cache décrit encore la page courante.
 *
 * On compare l'instantané (copie indépendante) à l'en-tête relu : c'est la
 * seule façon de détecter une modification, puisque desc->header pointe dans
 * la page et ne fournirait aucune référence antérieure.
 */
static bool
pax_layout_is_current(const PaxPageDesc *desc, Page page, BlockNumber blkno,
                      PaxPageHeader *phdr, TupleDesc tupdesc)
{
    int i;

    if (desc == NULL || desc->page != page)
        return false;

    /* Le buffer épinglé garantit qu'il s'agit bien du même bloc. */
    if (desc->stamp_blkno != blkno)
        return false;

    /* Un changement de tupline est le seul cas normal de réutilisation. */
    if (desc->n_tuples != phdr->n_tuples)
        return false;

    if (desc->stamp_flags != phdr->flags ||
        desc->stamp_meta_offset != phdr->meta_offset)
        return false;

    if (desc->n_attrs != tupdesc->natts)
        return false;

    /*
     * offsets[] : une insertion concurrente décale les régions en memmove,
     * donc toute modification invalide les pointeurs mis en cache.
     */
    for (i = 0; i < desc->n_attrs; i++)
    {
        if (desc->stamp_offsets[i] != phdr->offsets[i])
            return false;
    }

    return true;
}

/*
 * Relit et revalide les métadonnées de version dans un descripteur dont la
 * mise en page est déjà valide.
 *
 * xmin, xmax, cmin, cmax, t_ctid et locker_mxid sont modifiés en place par les
 * UPDATE, DELETE etdéverrouillages concurrents : ils ne sont jamais mis en
 * cache, seulement recopiés. La revalidation complète est conservée : c'est
 * elle qui transforme une page corrompue en erreur franche plutôt qu'en
 * décision de visibilité erronée.
 */
static void
pax_validate_tuple_meta(const PaxTupleMetaData *meta, int i)
{
    if (!TransactionIdIsValid(meta->xmin))
        elog(ERROR, "pax: tuple %d has invalid xmin", i);
    if (meta->cmin == InvalidCommandId)
        elog(ERROR, "pax: tuple %d has invalid cmin", i);
    if (!TransactionIdIsValid(meta->xmax) && meta->cmax != InvalidCommandId)
        elog(ERROR, "pax: tuple %d has cmax without xmax", i);
    if (TransactionIdIsValid(meta->xmax) && meta->cmax == InvalidCommandId)
        elog(ERROR, "pax: tuple %d has xmax without cmax", i);
    if (!ItemPointerIsValid(&meta->t_ctid) ||
        !OffsetNumberIsValid(ItemPointerGetOffsetNumber(&meta->t_ctid)))
        elog(ERROR, "pax: tuple %d has invalid t_ctid", i);
    if ((meta->flags & ~PAX_VERSION_UNUSED) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pax: tuple %d has unknown metadata flags %#x",
                        i, (int) meta->flags)));
    if (meta->reserved2 != 0)
        elog(ERROR, "pax: tuple %d has nonzero metadata padding", i);
}

/*
 * S assure que l'entrée i est à jour dans desc, en la recopiant et en la
 * validant si elle n'a pas déjà été consommée.
 *
 * C'est le point clé du balayage : le descripteur de page survit d'une
 * tupline à l'autre, et recopier puis revalider les n métadonnées à chaque
 * appel donnait un comportement O(n²) par page alors que l'on n'examine
 * presque toujours qu'une seule entrée. Le curseur de lecture n'avance que
 * vers l'avant, donc une entrée déjà consommée ne sera plus relue : il suffit
 * de garantir les entrées au-delà du curseur.
 */
static void
pax_meta_ensure(PaxPageDesc *desc, Page page, int i)
{
    PaxPageHeader      *phdr = desc->header;
    PaxTupleMetaData   *meta;

    Assert(i >= 0 && i < desc->n_tuples);

    if (i < desc->meta_checked_through)
        return;

    meta = &desc->tuple_meta[i];
    memcpy(meta,
           (char *) page + phdr->meta_offset +
           (Size) i * SizeOfPaxTupleMetaData,
           SizeOfPaxTupleMetaData);
    pax_validate_tuple_meta(meta, i);

    if (i + 1 > desc->meta_checked_through)
        desc->meta_checked_through = i + 1;
}

static void
pax_refresh_tuple_meta(PaxPageDesc *desc, Page page)
{
    PaxPageHeader  *phdr = desc->header;
    Size            expected;
    int             i;

    desc->n_tuples = phdr->n_tuples;

    for (i = 0; i < desc->n_attrs; i++)
        desc->minipages[i].n_values = desc->n_tuples;

    if (desc->n_tuples == 0)
    {
        desc->meta_checked_through = 0;
        return;
    }

    expected = (Size) desc->n_tuples * SizeOfPaxTupleMetaData;

    if (!PaxOffsetIsValid(phdr->meta_offset))
        elog(ERROR, "pax: missing transaction metadata region");

    if (pax_meta_region_size(page, phdr) != expected)
        elog(ERROR, "pax: inconsistent transaction metadata region "
                    "(region %zu, %d tuples x %zu)",
             pax_meta_region_size(page, phdr), desc->n_tuples,
             SizeOfPaxTupleMetaData);

    memcpy(desc->tuple_meta,
           (char *) page + phdr->meta_offset,
           expected);

    for (i = 0; i < desc->n_tuples; i++)
        pax_validate_tuple_meta(&desc->tuple_meta[i], i);

    desc->meta_checked_through = desc->n_tuples;
}

/*
 * Construit un descripteur complet (mise en page + métadonnées). Utilisé par
 * les chemins qui ne balayent pas de façon répétée : UPDATE, DELETE,
 * verrouillage, lecture directe d'un TID.
 */
static PaxPageDesc *
pax_build_page_desc(Page page, TupleDesc tupdesc)
{
    PaxPageDesc *desc;

    desc = pax_build_page_layout(page, InvalidBlockNumber, tupdesc);

    pax_refresh_tuple_meta(desc, page);

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
    if (desc->stamp_offsets)
        pfree(desc->stamp_offsets);
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
 * Espace requis pour les seules valeurs de longueur variable.
 *
 * C'est la part de l'insertion qui consomme réellement du pd_upper, et la
 * seule part à vérifier pour réutiliser un emplacement UNUSED : les slots et
 * les bits de bitmap existent déjà.
 */
static Size
pax_payload_space_needed(Relation rel, Datum *values, bool *isnulls)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    Size        need = 0;
    int         i;

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

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
 * Espace demande au FSM pour trouver une page d'accueil.
 *
 * C'est une MAJORATION, pas une borne inferieure, et c'est delibere.
 *
 * La raison est la terminaison du parcours de selection. La boucle qui essaie
 * les pages candidates sort par RecordAndGetPageWithFreeSpace(), qui ne rend
 * jamais deux fois la meme page. Mais elle ne rend InvalidBlockNumber que
 * lorsqu'aucune page n'a la place demandee. Si cette place etait une borne
 * inferieure, une page disposant d'entre hint et needed octets serait
 * proposee, rejetee sous le verrou exclusif, rendue, puis re-proposee
 * indefiniment. C'est un bug de vivacite, pas une inefficacite d'empaquetage :
 * l'UPDATE ne finit pas.
 *
 * La majoration porte sur la part la plus facile a manquer, le delta de
 * bitmap. Une region qui passe de n a n+1 versions peut voir son bitmap
 * grandir, et pax_insert_space_needed() compte cette croissance. Compter ici
 * le pire cas par colonne - sizeof(uint64), la granularite du bitmap -
 * referme l'intervalle [hint, needed) et rend la boucle finie quelle que soit
 * la page proposee.
 *
 * Le prix est un remplissage legerement moins dense, borne a 8 octets par
 * colonne et par ligne inseree, donc a 160 octets sur une table de 20 colonnes
 * pour une page deja bien remplie.
 *
 * Enfin la valeur est saturee : le FSM n'accepte qu'une demande entre 1 et
 * MaxFSMRequestSize, 8160 o, et refuse tout le reste par « invalid FSM
 * request size ». La borne n'est donc pas naturelle - a 20 colonnes, une
 * ligne large peut la depasser. Saturer ne coute rien a la terminaison, c'est
 * encore une majoration et la plus forte que le FSM accepte, et cela laisse le
 * diagnostic honnete remonter de pax_insert_space_needed(), qui sait dire
 * combien d'octets la ligne demande et combien la page neuve en offre.
 * Saturer plus bas serait une sous-estimation, donc le piege ci-dessus.
 */
static Size
pax_insert_space_hint(Relation rel, Datum *values, bool *isnulls)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    Size        need = SizeOfPaxTupleMetaData;
    Size        amount;
    int         i;

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        /* Un slot de plus dans la region de cette colonne... */
        need += pax_slot_stride(attr);

        /* ... et le pire agrandissement de son bitmap. */
        need += sizeof(uint64);

        if (isnulls[i] || attr->attlen >= 0)
            continue;

        if (attr->attlen == -2)
            amount = MAXALIGN(strlen(DatumGetPointer(values[i])) + 1);
        else
            amount = MAXALIGN(VARSIZE_ANY(DatumGetPointer(values[i])));
        need += amount;
    }

    /* 1 octet minimum : 0 demanderait la categorie 1 sans rien garantir. */
    return Max((Size) 1, Min(need, (Size) PaxMaxFSMRequestSize));
}

/*
 * Espace (borne supérieure) exigé pour insérer ce tuple dans une page qui
 * contient déjà "tupno" tuples.
 *
 * "phdr" permet de ne provisionner que ce qui concerne les colonnes
 * réellement absentes de la page ; passer NULL revient à supposer qu'aucune
 * région n'existe encore (page neuve).
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

    /*
     * SNAPSHOT_NON_VACUUMABLE : visible si la version pourrait être visible
     * pour QUELQUE transaction. C'est l'inverse de « supprimable », que la
     * fonction de visibilité globale tranche avec l'horizon de vacuum. C'est
     * le snapshot utilisé pour décider ce que VACUUM peut marquer.
     *
     * SNAPSHOT_DIRTY : c'est le snapshot utilisé par le contrôle d'unicité
     * d'un AM d'index pour détecter un doublon concurrent. Il voit les
     * transactions validées, celles encore en cours, ET les commandes
     * précédentes de la transaction courante.
     */
    if (snapshot->snapshot_type == SNAPSHOT_NON_VACUUMABLE)
    {
        /*
         * Visible si elle pourrait l'être pour quelque transaction : c'est
         * l'inverse de « supprimable », que le test global décide avec
         * l'horizon de vacuum et l'inconnue des transactions en cours.
         */
        if (meta->xmin == FrozenTransactionId ||
            meta->xmin == BootstrapTransactionId)
            return true;
        if (TransactionIdIsCurrentTransactionId(meta->xmin))
            return true;
        return !GlobalVisTestIsRemovableXid(snapshot->vistest,
                                            meta->xmin, true);
    }

    if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
    {
        TransactionId raw_xmin;
        TransactionId raw_xmax;

        /*
         * Ce snapshot sert aussi d'ARGUMENT DE SORTIE : l'AM d'index y lit les
         * xid des transactions concurrentes pour décider s'il doit attendre
         * leur sort avant de conclure à un conflit d'unicité. Il faut donc les
         * REMPLIR à chaque appel.
         *
         * InitDirtySnapshot() ne pose que snapshot_type : sans cette
         * initialisation, nbtree lirait des données de pile non initialisées,
         * d'où des xid fantômes et des plantages dans pg_subtrans. C'est
         * exactement ce que produit l'oubli de ce remplissage.
         */
        snapshot->xmin = InvalidTransactionId;
        snapshot->xmax = InvalidTransactionId;
        snapshot->speculativeToken = 0;

        raw_xmin = meta->xmin;
        raw_xmax = meta->xmax;

        /* --- Qui a inséré ? --- */
        if (raw_xmin == FrozenTransactionId ||
            raw_xmin == BootstrapTransactionId)
            ;                       /* toujours visible */
        else if (TransactionIdIsCurrentTransactionId(raw_xmin))
            return true;           /* nos propres écritures sont visibles */
        else if (TransactionIdIsInProgress(raw_xmin))
        {
            /*
             * Insérée par une autre transaction en cours : la ligne existe pour
             * elle, c'est donc un conflit potentiel. On rend le xid pour que
             * l'appelant attende avant de se prononcer.
             */
            snapshot->xmin = raw_xmin;
            return true;
        }
        else if (!TransactionIdDidCommit(raw_xmin))
            return false;          /* insertion annulée : invisible */

        /* L'insertion est validée : reste à savoir qui a remplacé la ligne. */
        if (!TransactionIdIsValid(raw_xmax))
            return true;

        if (TransactionIdIsCurrentTransactionId(raw_xmax))
            return false;          /* nous l'avons supprimée : pas de conflit */

        if (TransactionIdIsInProgress(raw_xmax))
        {
            snapshot->xmax = raw_xmax;
            return true;
        }

        if (TransactionIdDidCommit(raw_xmax))
            return false;          /* supprimée pour de bon */

        return true;               /* la suppression a été annulée */
    }

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
    /*
     * Version marked UNUSED by VACUUM: invisible to everyone, always.
     *
     * This is the single choke point for visibility, so making it here covers
     * the sequential scan, the TID fetches, and both chain followers
     * (pax_get_latest_tuple and the FIND_LAST_VERSION path in tuple_lock).
     * A stale t_ctid therefore lands on an invisible version and the chain
     * stops, instead of exposing a version that was reclaimed.
     */
    if (pax_meta_is_unused(meta))
        return false;

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

        /*
         * Recopie et validation à la demande. Le balayage n'avance que vers
         * l'avant, donc les entrées déjà consommées ne sont jamais relues et ne
         * seraient qu'un coût O(n²) si on les revalidait à chaque appel.
         */
        pax_meta_ensure(pdesc, pdesc->page, tupno);

        /* Une version marquée inutilisée par VACUUM n'existe plus. */
        if (pax_meta_is_unused(&pdesc->tuple_meta[tupno]))
            continue;

        /*
         * ANALYZE n'a pas de snapshot (table_beginscan_analyze passe NULL) :
         * on ne filtre alors que les versions inutilisées, et c'est
         * pax_meta_classify() qui décide ensuite si la version est vivante ou
         * morte. Un scan normal a toujours un snapshot.
         */
        if (scan->rs_base.rs_snapshot == NULL)
            return tupno;

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

    /*
     * The slot may still hold a previously materialized tuple. A regular scan
     * clears it between rows, but ANALYZE does not: it calls this callback
     * again while the slot still carries TTS_FLAG_SHOULDFREE.
     *
     * Without this reset, the flag would stay set while tts_values[] points
     * back into the page, and pax_slot_clear() would then pfree() a page
     * pointer. Clear the old contents first.
     */
    if (slot->tts_flags & TTS_FLAG_SHOULDFREE)
        pax_slot_clear(slot);

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
    if ((flags & (SO_TYPE_SEQSCAN | SO_TYPE_TIDSCAN |
                  SO_TYPE_TIDRANGESCAN | SO_TYPE_ANALYZE)) == 0)
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
    scan->cached_desc    = NULL;
    scan->desc_ctx       = AllocSetContextCreate(CurrentMemoryContext,
                                                  "pax scan page layout",
                                                  ALLOCSET_DEFAULT_SIZES);
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
    pax_scan_drop_cached_desc(scan);
    MemoryContextDelete(scan->desc_ctx);

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
    pax_scan_drop_cached_desc(scan);
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

/* Jette la mise en page en cache, sans détruire son contexte. */
static void
pax_scan_drop_cached_desc(PaxScanDesc scan)
{
    if (scan->cached_desc)
    {
        pax_free_page_desc(scan->cached_desc);
        scan->cached_desc = NULL;
    }
}

/*
 * Renvoie un descripteur exploitable pour la page courante, en réutilisant la
 * mise en page si l'en-tête de page n'a pas bougé.
 *
 * L'appelant doit détenir le verrou de contenu sur la page : c'est lui qui
 * garantit que desc->header et desc->minipages[].data restent valides. Les
 * métadonnées de version sont recopiées à chaque appel quoi qu'il arrive.
 */
static PaxPageDesc *
pax_scan_page_desc(PaxScanDesc scan, Page page, BlockNumber blkno,
                   TupleDesc tupdesc)
{
    PaxPageHeader  *phdr = PaxPageHeaderPtr(page);
    MemoryContext   oldc;
    PaxPageDesc    *desc;

    /*
     * Les deux branches travaillent dans desc_ctx : le rafraîchissement peut
     * réallouer tuple_meta (si la page a gagné des versions), et cette
     * mémoire doit appartenir au contexte du scan, pas à celui de l'appelant.
     */
    oldc = MemoryContextSwitchTo(scan->desc_ctx);

    if (pax_layout_is_current(scan->cached_desc, page, blkno, phdr, tupdesc))
    {
        desc = scan->cached_desc;
    }
    else
    {
        pax_scan_drop_cached_desc(scan);
        desc = pax_build_page_layout(page, blkno, tupdesc);
        scan->cached_desc = desc;
    }

    /*
     * Aucune revalidation ici : le balayage remplit les métadonnées à la
     * demande via pax_meta_ensure(), au fur et à mesure qu'il les examine.
     * Les chemins « one-shot » (UPDATE, DELETE, verrouillage, lecture d'un
     * TID) utilisent pax_build_page_desc(), qui revalide tout.
     */

    MemoryContextSwitchTo(oldc);

    return desc;
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
        pdesc = pax_scan_page_desc(scan, page, scan->current_block, tupdesc);
        tupno = pax_find_visible_tuple(scan, pdesc,
                                       scan->current_tupno + 1);

        if (tupno >= 0)
        {
            /*
             * Materialize before dropping the content lock.  A pin prevents
             * eviction, but an inserter may still memmove this page while the
             * slot is live; copied values are therefore mandatory.
             *
             * pdesc is the scan's cached descriptor: it stays alive across
             * calls and is refreshed on the next one, so it must NOT be freed
             * here. The slot owns only its own pin and its own copy of the
             * version metadata.
             */
            pax_store_tuple_slot(rel, slot, buf, pdesc, tupno, false);
            ExecMaterializeSlot(slot);

            /*
             * Keep the pin, drop only the content lock, so that an UPDATE or
             * DELETE of this tuple can still take an exclusive lock.
             */
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);

            scan->current_tupno = tupno;
            return true;
        }

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
    pax_scan_drop_cached_desc(scan);
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

        pdesc = pax_scan_page_desc(scan, page, scan->current_block, tupdesc);
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
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            scan->current_tupno = tupno;
            return true;
        }

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
 * atteindre le disque.  Reste à faire : vacuum.
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
    int             tupno;
    int             i;
    Datum          *values;
    bool           *isnulls;
    OffsetNumber   *voffs;
    TransactionId   xmin;
    PaxTupleMetaData *meta;
    Size             needed;
    Size             available;
    Size             hint;
    Size             payload_needed;
    bool             reusing;
    Size             meta_at;
    Size             meta_size;
    BlockNumber      target;
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

    /*
     * === 1. Choisir une page ayant assez de place, via le FSM ===
     *
     * Le parcours linéaire « premier bloc assez grand depuis 0 » coûtait un
     * un ReadBuffer et un verrou par bloc déjà pleine. Le FSM répond en O(1)
     * à « quelle page a de la place » et ne sert qu'à proposer une
     * candidate : la place réelle est revérifiée sous verrou exclusif avant
     * toute écriture.
     *
     * Contrairement à heap, le FSM est enregistré après chaque insertion
     * réussie. Heap délègue ce travail à VACUUM ; PAX n'a pas de VACUUM,
     * donc rien ne renseignerait jamais la carte et chaque insertion
     * étendrait la relation.
     */
    hint   = pax_insert_space_hint(rel, values, isnulls);
    nblocks = RelationGetNumberOfBlocks(rel);

    /* 1a) la page cible mémorisée par ce backend (relcache), si elle existe */
    target = RelationGetTargetBlock(rel);
    if (target != InvalidBlockNumber && target >= nblocks)
        target = InvalidBlockNumber;      /* relation rétrécie : invalide */

    if (target == InvalidBlockNumber)
    {
        /* 1b) le FSM */
        target = GetPageWithFreeSpace(rel, hint);
        if (target == InvalidBlockNumber && nblocks > 0)
        {
            /*
             * FSM muet (relation qui démarre, ou entrées absentes après un
             * crash) : on tente la dernière page, comme heap, pour éviter le
             * syndrome « une ligne par page » au démarrage.
             */
            target = nblocks - 1;
        }
    }

    while (target != InvalidBlockNumber && target < nblocks)
    {
        Buffer          b;
        Page            p;
        PaxSpecialData *special;
        PaxPageHeader  *h;
        Size            free_space;

        CHECK_FOR_INTERRUPTS();

        b = ReadBuffer(rel, target);
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
                 target, RelationGetRelationName(rel));
        }

        special = (PaxSpecialData *) PageGetSpecialPointer(p);
        if (special->n_attrs != natts)
        {
            UnlockReleaseBuffer(b);
            elog(ERROR, "pax: attribute count mismatch (page %u vs tupdesc %d)",
                 special->n_attrs, natts);
        }

        /* n_tuples lu APRÈS le lock : l'état peut avoir changé */
        h          = PaxPageHeaderPtr(p);
        needed     = pax_insert_space_needed(rel, values, isnulls, h->n_tuples,
                                             h);
        free_space = pax_page_free_space(p);

        if (free_space >= needed)
        {
            buf = b;
            break;
        }

        /*
         * Pas assez de place : le FSM était optimiste (ou périmé). On
         * enregistre l'état réel de cette page, ce qui l'exclut du parcours,
         * puis on demande un autre candidat.
         */
        UnlockReleaseBuffer(b);
        target = RecordAndGetPageWithFreeSpace(rel, target, free_space, hint);
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

    /*
     * Cherche un emplacement libéré par VACUUM pour le réutiliser.
     *
     * Un emplacement UNUSED conserve déjà son slot dans chaque région et son
     * bit dans chaque bitmap : il suffit donc de réécrire les valeurs par
     * dessus, sans étendre ni les régions ni les métadonnées. C'est ce qui
     * rend la place récupérée par le vacuum utilisable en place.
     *
     * On ne réutilise que si la page possède déjà toutes les régions de
     * colonnes : une colonne absente (jamais écrite sur cette page, donc NULL
     * partout) n'a pas de slot à réutiliser et devrait en créer un, ce qui
     * exigerait d'agrandir la page — autant ajouter une version à la fin.
     *
     * Les valeurs variables restent allouées en haut de page et consomment
     * donc de la place : la réutilisation n'est possible que si la page a la
     * place pour ces seules valeurs, ce que pax_payload_space_needed()
     * calcule. On vérifie cela AVANT de commiter le choix.
     */
    reusing = false;
    if (phdr->n_tuples > 0)
    {
        PaxTupleMetaData *meta_base;
        int                c;

        for (c = 0; c < natts; c++)
        {
            if (!PaxOffsetIsValid(phdr->offsets[c]))
                break;
        }
        if (c == natts)
        {
            payload_needed = pax_payload_space_needed(rel, values, isnulls);
            if (pax_page_free_space(page) >= payload_needed)
            {
                meta_base = ((PaxTupleMetaData *)
                             ((char *) page + phdr->meta_offset));
                for (c = 0; c < phdr->n_tuples; c++)
                {
                    if (pax_meta_is_unused(&meta_base[c]))
                    {
                        tupno = c;
                        reusing = true;
                        break;
                    }
                }
            }
        }
    }

    if (!reusing)
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

    /*
     * La région de métadonnées fait toujours n_tuples x 32 octets. La
     * vérifier contre tupno ne vaudrait que pour un ajout simple : en
     * réutilisation tupno < n_tuples alors que la région, elle, ne change pas.
     */
    meta_size = pax_meta_region_size(page, phdr);
    if (meta_size != (Size) phdr->n_tuples * SizeOfPaxTupleMetaData)
        elog(ERROR, "pax: inconsistent transaction metadata region "
                    "(region %zu, %d tuples x %zu)",
             meta_size, phdr->n_tuples, SizeOfPaxTupleMetaData);

    meta_at = (Size) phdr->meta_offset +
        (Size) tupno * SizeOfPaxTupleMetaData;

    /*
     * En réutilisation, l'emplacement 32 octets existe déjà : on le
     * réécrit sur place. Sinon on insère les octets, ce qui décale les
     * versions suivantes et les régions.
     */
    if (!reusing)
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
        want = reusing ? cur : pax_bitmap_size(tupno + 1);
        if (want > cur)
        {
            at = region_start + cur;
            pax_insert_bytes(page, phdr, natts, i, at, want - cur);
            memset((char *) page + at, 0, want - cur);
        }
        bmp = (bits8 *) ((char *) page + region_start);

        /*
         * 3b) réserver l'emplacement de la valeur dans la région. En
         * réutilisation le slot existe déjà, on n'insère rien ; seule la
         * valeur est réécrite par-dessus.
         *
         * La base de la table des valeurs est la taille RÉELLE du bitmap, pas
         * celle que l'insertion est en train de construire. En ajout simple
         * les deux coïncident (want vient d'être atteint). En réutilisation
         * elles diffèrent : tupno < n_tuples, donc pax_bitmap_size(tupno)
         * serait trop petit, et l'offset partirait dans le milieu de la
         * table existante, décalant tous les emplacements suivants. C'est
         * ici que se lit le bitmap à la lecture (pax_build_page_layout),
         * donc il faut exactement la même valeur.
         */
        at = region_start +
            pax_bitmap_size(reusing ? phdr->n_tuples : tupno + 1) +
            (Size) tupno * stride;
        if (!reusing)
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
    /* En réutilisation, l'emplacement existe déjà : n_tuples ne bouge pas. */
    if (!reusing)
        phdr->n_tuples++;
    phdr->free_space = (uint16) pax_page_free_space(page);

    /* TID : offset de TID = tupno + 1 (les offsets de TID débutent à 1) */
    ItemPointerSet(&(slot->tts_tid), BufferGetBlockNumber(buf),
                   (OffsetNumber) (tupno + 1));
    ItemPointerCopy(&slot->tts_tid, &meta->t_ctid);
    meta->flags = 0;                /* réutilisation : plus UNUSED */
    slot->tts_tableOid = RelationGetRelid(rel);
    if (slot->tts_ops == &TTSOpsPax)
    {
        PaxTupleTableSlot *pslot = (PaxTupleTableSlot *) slot;

        pslot->has_tuple_meta = true;
        pslot->tuple_meta = *meta;
    }

    available = pax_page_free_space(page);

    GenericXLogFinish(wal_state);

    /*
     * Memorise la page comme cible des insertions suivantes de ce backend, et
     * publie l'espace libre restant dans le FSM.
     *
     * On le fait APRÈS GenericXLogFinish() pour que l'espace publié soit
     * conforme à ce qui est réellement sur la page. PAX ne rend jamais
     * d'espace libre par elle-même (UPDATE et DELETE n'écrivent que dans les
     * métadonnées de version, déjà allouées) : l'espace ne fait donc que
     * décroître, et cette mise à jour suffit entre deux VACUUM.
     */
    RelationSetTargetBlock(rel, BufferGetBlockNumber(buf));
    if (available < BLCKSZ)
        RecordPageWithFreeSpace(rel, BufferGetBlockNumber(buf), available);

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
    /*
     * Version reclaimed by VACUUM: treat it as deleted so that a chain walk
     * stops here rather than following its t_ctid. Its offset stays valid, so
     * a stale inbound link resolves to this version and to nothing beyond.
     */
    if (pax_meta_is_unused(meta))
        return PaxVersionDeleted;

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

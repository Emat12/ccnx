/**
 * @file sync/SyncActions.c
 *  
 * Part of CCNx Sync.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation.
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details. You should have received
 * a copy of the GNU Lesser General Public License along with this library;
 * if not, write to the Free Software Foundation, Inc., 51 Franklin Street,
 * Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "SyncActions.h"
#include "SyncNode.h"
#include "SyncPrivate.h"
#include "SyncRoot.h"
#include "SyncTreeWorker.h"
#include "SyncUtil.h"

#include <ccn/ccn.h>
#include <ccn/charbuf.h>
#include <ccn/coding.h>
#include <ccn/indexbuf.h>
#include <ccn/schedule.h>

#include <ccnr/ccnr_msg.h>
#include <ccnr/ccnr_sync.h>
#include <ccnr/ccnr_private.h>

#define M 1000000
// various configuration parameters
// TBD: get them from an external source?
static int cachePurgeTrigger = 60;      // cache entry purge, in seconds
static int cacheCleanBatch = 16;        // seconds between cleaning batches
static int cacheCleanDelta = 8;         // cache clean batch size
static int adviseNeedReset = 1;         // reset value for adviseNeed
static int updateStallDelta = 15;       // seconds used to determine stalled update
static int updateNeedDelta = 6;         // seconds for adaptive update
static int shortDelayMicros = 1000;     // short delay for quick reschedule
static int compareAssumeBad = 20;       // secs since last fetch OK to assume compare failed
static int nodeSplitTrigger = 4000;     // in bytes, triggers node split
static int exclusionLimit = 1000;       // in bytes, limits exclusion list size
static int stableTimeTrig = 10;         // trigger for storing stable point (secs)
static int hashSplitTrigger = 17;       // trigger for splitting based on hash (n/255)
static int namesYieldInc = 100;         // number of names to inc between yield tests
static int namesYieldMicros = 20*1000;  // number of micros to use as yield trigger
static char *syncStableSuffix = "SyncStable";

static int syncScope = 2;

enum SyncCompareState {
    SyncCompare_init,
    SyncCompare_preload,
    SyncCompare_busy,
    SyncCompare_waiting,
    SyncCompare_done
};

struct SyncCompareData {
    struct SyncRootStruct *root;    /**< parent root for this comparison */
    struct SyncTreeWorkerHead *twL; /**< local tree walker state */
    struct SyncTreeWorkerHead *twR; /**< remote tree walker state */
    struct ccn_charbuf *hashL;      /**< hash for root of local sync tree */
    struct ccn_charbuf *hashR;      /**< hash for root of remote sync tree */
    struct ccn_charbuf *cbL;        /**< local tree scratch */
    struct ccn_charbuf *cbR;        /**< remote tree scratch */
    struct ccn_charbuf *lagL;       /**< local lag name */
    int *lagMatch;                  /**< lagging # of matching components */
    struct SyncActionData *errList; /**< actions that had errors for this compare */
    int errsQueued;                 /**< names added during this comparison */
    int namesAdded;                 /**< names added during this comparison */
    int nodeFetchBusy;              /**< number of busy remote node fetches */
    int nodeFetchFailed;            /**< number of failed remote node fetches */
    int contentPos;                 /**< position of next content to fetch */
    int contentFetchBusy;           /**< number of busy content fetches */
    int contentFetchFailed;         /**< number of failed content fetches */
    struct ccn_scheduled_event *ev; /**< progress event */
    enum SyncCompareState state;    /**< summary state of comparison */
    sync_time lastFetchOK;          /**< time marker for last successul node/content fetch */
    sync_time startTime;            /**< time marker for compare data creation */
    sync_time lastEnter;            /**< time marker for last compare step entry */
    sync_time lastMark;             /**< time marker for stall determination */
    int64_t maxHold;                /**< max time thread was held by compare */
};

enum SyncUpdateState {
    SyncUpdate_init,
    SyncUpdate_inserted,
    SyncUpdate_busy,
    SyncUpdate_error,
    SyncUpdate_done
};

struct SyncUpdateData {
    struct SyncRootStruct *root;
    enum SyncUpdateState state;
    struct SyncNameAccum *sort;
    struct SyncNodeAccum *nodes;
    struct SyncTreeWorkerHead *tw;
    struct ccn_charbuf *cb;
    IndexSorter_Base ixBase;
    IndexSorter_Index ixPos;
    int nameLenAccum;
    int namesAdded;
    int initLen;
    sync_time startTime;
    sync_time entryTime;
    int64_t maxHold;
    int preSortCount;
    int postSortCount;
};


///////////////////////////////////////////////////////////////////////////
///// General internal routines
///////////////////////////////////////////////////////////////////////////

static struct SyncActionData *
newActionData(enum SyncRegisterActionKind kind) {
    struct SyncActionData *data = NEW_STRUCT(1, SyncActionData);
    data->startTime = SyncCurrentTime();
    data->kind = kind;
    data->state = SyncActionState_init;
    return data;
}

static void
linkActionData(struct SyncRootStruct *root, struct SyncActionData *data) {
    data->root = root;
    data->next = root->actions;
    data->ccnr = root->base->ccnr;
    data->state = SyncActionState_sent;
    root->actions = data;
}

static void
delinkActionData(struct SyncActionData *data) {
    if (data == NULL) return;
    if (data->state == SyncActionState_sent) {
        // remove from the action chain in the root
        struct SyncRootStruct *root = data->root;
        if (root == NULL) return;
        struct SyncActionData *each = root->actions;
        struct SyncActionData *lag = NULL;
        data->state = SyncActionState_loose;
        while (each != NULL) {
            struct SyncActionData *next = each->next;
            if (data == each) {
                data->next = NULL;
                if (lag == NULL) root->actions = next;
                else lag->next = next;
                break;
            }
            lag = each;
            each = next;
        }
    } else {
        if (data->state == SyncActionState_error) {
            // remove from the errList chain in the comparison
            struct SyncCompareData *comp = data->comp;
            if (comp == NULL) return;
            struct SyncActionData *each = comp->errList;
            struct SyncActionData *lag = NULL;
            data->state = SyncActionState_loose;
            while (each != NULL) {
                struct SyncActionData *next = each->next;
                if (data == each) {
                    data->next = NULL;
                    if (comp->errsQueued > 0) comp->errsQueued--;
                    if (lag == NULL) comp->errList = next;
                    else lag->next = next;
                    break;
                }
                lag = each;
                each = next;
            }
        }
    }
}

static int
moveActionData(struct SyncActionData *data, enum SyncActionState dstState) {
    // moves the action data to the given state queue
    // (must be SyncActionState_sent or SyncActionState_error)
    // returns 1 for success, 0 for not possible
    if (data == NULL) return 0;
    if (dstState == SyncActionState_error && data->state != SyncActionState_sent)
        return 0;
    if (dstState == SyncActionState_sent && data->state != SyncActionState_error)
        return 0;
    struct SyncRootStruct *root = data->root;
    struct SyncCompareData *comp = data->comp;
    if (root == NULL || comp == NULL) return 0;
    delinkActionData(data);
    if (dstState == SyncActionState_sent) {
        data->next = root->actions;
        root->actions = data;
    } else {
        data->next = comp->errList;
        comp->errList = data;
        comp->errsQueued++;
    }
    data->state = dstState;
    return 1;
}

static struct SyncActionData *
destroyActionData(struct SyncActionData *data) {
    if (data != NULL) {
        delinkActionData(data);
        // remove any resources
        if (data->prefix != NULL)
            ccn_charbuf_destroy(&data->prefix);
        if (data->hash != NULL)
            ccn_charbuf_destroy(&data->hash);
        data->next = NULL;
        data->root = NULL;
        data->comp = NULL;
        free(data);
    }
    return NULL;
}

static char *
getCmdStr(enum SyncRegisterActionKind kind) {
    switch (kind) {
        case SRI_Kind_AdviseInt:
        case SRI_Kind_RootAdvise:
            return "\xC1.S.ra";
        case SRI_Kind_FetchInt:
        case SRI_Kind_NodeFetch:
            return "\xC1.S.nf";
        case SRI_Kind_RootStats:
            return "\xC1.S.rs";
        default:
            return NULL;
    }
}

static char *
getKindStr(enum SyncRegisterActionKind kind) {
    switch (kind) {
        case SRI_Kind_None:
            return "None";
        case SRI_Kind_AdviseInt:
        case SRI_Kind_RootAdvise:
            return "RootAdvise";
        case SRI_Kind_FetchInt:
        case SRI_Kind_NodeFetch:
            return "NodeFetch";
        case SRI_Kind_RootStats:
            return "RootStats";
        case SRI_Kind_Content:
            return "Content";
        default:
            return NULL;
    }
}

static void
setCovered(struct SyncHashCacheEntry *ce) {
    char *here = "Sync.setCovered";
    if (ce->state & SyncHashState_covered) {
        // nothing to do, already covered
    } else if (ce->state & SyncHashState_remote) {
        // only set this bit if a remote hash has been entered
        struct SyncRootStruct *root = ce->head->root;
        if (root->base->debug >= CCNL_FINER) {
            char *hex = SyncHexStr(ce->hash->buf, ce->hash->length);
            SyncNoteSimple(root, here, hex);
            free(hex);
        }
        ce->state |= SyncHashState_covered;
    }
}

static int
isCovered(struct SyncHashCacheEntry *ce) {
    if (ce->state & SyncHashState_covered) return 1;
    if (ce->state & SyncHashState_local) {
        setCovered(ce);
        return 1;
    }
    return 0;
}

static int
compareHash(struct ccn_charbuf *hashX, struct ccn_charbuf *hashY) {
    if (hashX == hashY) return 0;
    if (hashX == NULL) return -1;
    if (hashY == NULL) return 1;
    size_t lenX = hashX->length;
    size_t lenY = hashY->length;
    if (lenX < lenY) return -1;
    if (lenX > lenY) return 1;
    return memcmp(hashX->buf, hashY->buf, lenX);
}

// take a list of names and sort them, removing duplicates!
// should leave src empty  
static struct SyncNameAccum *
sortNames(struct SyncRootStruct *root, struct SyncNameAccum *src) {
    char *here = "Sync.sortNames";
    IndexSorter_Index ixLim = src->len;
    IndexSorter_Base ixBase = IndexSorter_New(ixLim, -1);
    ixBase->sorter = SyncNameAccumSorter;
    ixBase->client = src;
    IndexSorter_Index ix = 0;
    for (ix = 0; ix < ixLim; ix++) IndexSorter_Add(ixBase, ix);
    struct SyncNameAccum *dst = SyncAllocNameAccum(ixLim);
    struct ccn_charbuf *lag = NULL;
    for (ix = 0; ix < ixLim; ix++) {
        IndexSorter_Index j = IndexSorter_Rem(ixBase);
        if (j >= ixLim) {
            SyncNoteFailed(root, here, "rem failed", __LINE__);
            break;
        }
        struct ccn_charbuf *name = src->ents[j].name;
        src->ents[j].name = NULL;
        if (name == NULL) {
            SyncNoteFailed(root, here, "name == NULL", __LINE__);
            break;
        }
        if (lag == NULL || SyncCmpNames(lag, name) != 0) {
            // only append the name if it is not a duplicate
            SyncNameAccumAppend(dst, name, src->ents[j].data); // XXXXXX
            lag = name;
        } else {
            // this name needs to be destroyed
            ccn_charbuf_destroy(&name);
        }
    }    
    src->len = 0;
    IndexSorter_Free(&ixBase);
    return dst;
}

int
reportExclude(struct SyncRootStruct *root, struct ccn_buf_decoder *d) {
    char *here = "Sync.reportExclude";
    int res = -1;
    if (ccn_buf_match_dtag(d, CCN_DTAG_Exclude)) {
        res = d->decoder.element_index;
        ccn_buf_advance(d);
        // optional Any | Bloom not present
        while (ccn_buf_match_dtag(d, CCN_DTAG_Component)) {
            size_t cs = 0;
            const unsigned char *cp = NULL;
            ccn_buf_advance(d);
            if (ccn_buf_match_blob(d, &cp, &cs)) {
                ccn_buf_advance(d);
                char *hex = SyncHexStr(cp, cs);
                SyncNoteSimple(root, here, hex);
                free(hex);
                ccn_buf_check_close(d);
            }
        }
        ccn_buf_check_close(d);
    }
    if (d->decoder.state < 0)
        res = d->decoder.state;
    if (res < 0)
        SyncNoteSimple(root, here, "parse failed");
    return res;
}

#define StatsLine(XXX) \
if (stats->XXX) \
pos += snprintf(s+pos, lim-pos, ", %s %ju", #XXX, (uintmax_t) stats->XXX);

static struct ccn_charbuf *
formatStats(struct SyncRootStruct *root, struct ccn_charbuf *cb) {
    struct SyncRootStats *stats = root->priv->stats;
    const unsigned char *bufL = root->currentHash->buf;
    int lenL = root->currentHash->length;
    struct SyncNodeComposite *ncL = NULL;
    char s[2000];
    int lim = sizeof(s);
    int pos = 0;
    sync_time now = SyncCurrentTime();
    int ru_ok = -1;
#ifdef RUSAGE_SELF
    struct rusage ru;
    ru_ok = getrusage(RUSAGE_SELF, &ru);
#endif
    struct ccn_charbuf *hash = root->currentHash;
    struct SyncCompareData *comp = root->compare;
    struct SyncUpdateData *update = root->update;
    if (lenL > 0) {
        struct SyncHashCacheEntry *ceL = SyncHashLookup(root->ch, bufL, lenL);
        if (ceL != NULL) {
            SyncCacheEntryFetch(ceL);
            ncL = ceL->ncL;
        }
    }
    
    pos += snprintf(s+pos, lim-pos, "stats for root#%u", root->rootId);
    if (hash->length > 0) {
        // show the current hash
        char *hex = SyncHexStr(hash->buf, hash->length);
        pos += snprintf(s+pos, lim-pos, ", currentHash %s", hex);
        free(hex);
    }
    if (comp != NULL) {
        struct ccn_charbuf *hashR = comp->hashR;
        if (hashR != NULL && hashR->length > 0) {
            char *hex = SyncHexStr(hashR->buf, hashR->length);
            pos += snprintf(s+pos, lim-pos, ", remoteHash %s", hex);
            free(hex);
        }
        intmax_t dt = SyncDeltaTime(comp->startTime, now);
        pos += snprintf(s+pos, lim-pos, ", compareBusy %jd", dt);
    }
    if (update != NULL) {
        intmax_t dt = SyncDeltaTime(update->startTime, now);
        pos += snprintf(s+pos, lim-pos, ", updateBusy %jd", dt);
    }
    
    if (root->priv->lastHashChange != 0) {
        uintmax_t x = root->priv->lastHashChange;
        pos += snprintf(s+pos, lim-pos, ", lastHashChange %ju.%06u",
                        x / M, (unsigned) (x % M));
    }
    
    if (root->namesToAdd != NULL) {
        intmax_t rem = root->namesToAdd->len;
        if (rem > 0)
            pos += snprintf(s+pos, lim-pos, ", namesToAdd %jd", rem);
    }
    if (root->namesToFetch != NULL) {
        intmax_t rem = root->namesToFetch->len;
        if (comp != NULL) rem = rem - comp->contentPos;
        if (rem > 0)
            pos += snprintf(s+pos, lim-pos, ", namesToFetch %jd", rem);
    }
    if (ncL != NULL) {
        pos += snprintf(s+pos, lim-pos, ", treeDepth %ju", (uintmax_t) 
                        ncL->treeDepth);
        pos += snprintf(s+pos, lim-pos, ", treeNames %ju", (uintmax_t) 
                        ncL->leafCount);
        pos += snprintf(s+pos, lim-pos, ", treeBytes %ju", (uintmax_t) 
                        ncL->byteCount + ncL->cb->length);
    }
    
    StatsLine(comparesDone);
    StatsLine(lastCompareMicros);
    StatsLine(updatesDone);
    StatsLine(lastUpdateMicros);
    StatsLine(nodesCreated);
    StatsLine(nodesShared);
    StatsLine(rootAdviseSent);
    StatsLine(rootAdviseSeen);
    StatsLine(rootAdviseReceived);
    StatsLine(rootAdviseTimeout);
    StatsLine(rootAdviseFailed);
    StatsLine(nodeFetchSent);
    StatsLine(nodeFetchSeen);
    StatsLine(nodeFetchReceived);
    StatsLine(nodeFetchTimeout);
    StatsLine(nodeFetchFailed);
    StatsLine(nodeFetchBytes);
    StatsLine(contentFetchSent);
    StatsLine(contentFetchReceived);
    StatsLine(contentFetchTimeout);
    StatsLine(contentFetchFailed);
    StatsLine(contentFetchBytes);
    
#ifdef RUSAGE_SELF
    if (ru_ok >= 0) {
        pos += snprintf(s+pos, lim-pos, ", maxrss %ju",
                        (uintmax_t) ru.ru_maxrss);
        pos += snprintf(s+pos, lim-pos, ", utime %ju.%06u",
                        (uintmax_t) ru.ru_utime.tv_sec,
                        (unsigned) ru.ru_utime.tv_usec);
        pos += snprintf(s+pos, lim-pos, ", stime %ju.%06u",
                        (uintmax_t) ru.ru_stime.tv_sec,
                        (unsigned) ru.ru_stime.tv_usec);
    }
#endif   
    ccn_charbuf_append(cb, s, pos);
    return cb;
}

#undef StatsLine

static struct SyncNameAccum *
exclusionsFromHashList(struct SyncRootStruct *root, struct SyncHashInfoList *list) {
    struct SyncNameAccum *acc = SyncAllocNameAccum(0);
    int count = 0;
    int limit = exclusionLimit;
    
    if (root->currentHash->length > 0) {
        // if the current hash is not empty, start there
        struct ccn_charbuf *hash = root->currentHash;
        struct ccn_charbuf *name = ccn_charbuf_create();
        count = count + hash->length + 8;
        ccn_name_init(name);
        ccn_name_append(name, hash->buf, hash->length);
        SyncNameAccumAppend(acc, name, 0);
    }
    
    while (list != NULL) {
        struct SyncHashCacheEntry *ce = list->ce;
        if (ce != NULL && (ce->state & SyncHashState_remote)
            && (ce->state & SyncHashState_covered)) {
            // any remote root known to be covered is excluded
            struct ccn_charbuf *hash = ce->hash;
            count = count + hash->length + 8;
            if (count > limit)
                // exclusion list is getting too long, so ignore earlier roots
                break;
            struct ccn_charbuf *name = ccn_charbuf_create();
            ccn_name_init(name);
            ccn_name_append(name, hash->buf, hash->length);
            SyncNameAccumAppend(acc, name, 0);
        }
        list = list->next;
    }
    if (acc->len == 0) {
        SyncFreeNameAccum(acc);
        return NULL;
    }
    struct SyncNameAccum *lag = acc;
    if (acc->len == 0) {
        // empty liust convention is NULL
        acc = NULL;
    } else {
        // exclusion list must be sorted
        acc = sortNames(root, acc);
    }
    SyncFreeNameAccum(lag);
    return acc;
}

static struct ccn_charbuf *
constructCommandPrefix(struct SyncRootStruct *root,
                       enum SyncRegisterActionKind kind) {
    struct ccn_charbuf *prefix = ccn_charbuf_create();
    int res = 0;
    ccn_name_init(prefix);
    if (root->topoPrefix != NULL && root->topoPrefix->length > 0) {
        // the topo (if any) always comes first
        res |= SyncAppendAllComponents(prefix, root->topoPrefix);
    }
    // the command comes after the topo
    ccn_name_append_str(prefix, getCmdStr(kind));
    res |= ccn_name_append(prefix, root->sliceHash->buf, root->sliceHash->length);
    
    if (res < 0) {
        ccn_charbuf_destroy(&prefix);
    }
    return prefix;
}

static struct SyncNodeComposite *
extractNode(struct SyncRootStruct *root, struct ccn_upcall_info *info) {
    // first, find the content
    char *here = "Sync.extractNode";
    const unsigned char *cp = NULL;
    size_t cs = 0;
    size_t ccnb_size = info->pco->offset[CCN_PCO_E];
    const unsigned char *ccnb = info->content_ccnb;
    int res = ccn_content_get_value(ccnb, ccnb_size, info->pco,
                                    &cp, &cs);
    if (res < 0 || cs < DEFAULT_HASH_BYTES) {
        SyncNoteFailed(root, here, "ccn_content_get_value", __LINE__);
        return NULL;
    }
    
    // second, parse the object
    struct SyncNodeComposite *nc = SyncAllocComposite(root->base);
    struct ccn_buf_decoder ds;
    struct ccn_buf_decoder *d = ccn_buf_decoder_start(&ds, cp, cs);
    res |= SyncParseComposite(nc, d);
    if (res < 0) {
        // failed, so back out of the allocations
        SyncNoteFailed(root, here, "bad parse", -res);
        SyncFreeComposite(nc);
        nc = NULL;
    }
    return nc;
}

static int
noteRemoteHash(struct SyncRootStruct *root, struct SyncHashCacheEntry *ce, int add) {
    char *here = "Sync.noteRemoteHash";
    int debug = root->base->debug;
    struct ccnr_handle *ccnr = root->base->ccnr;
    struct ccn_charbuf *hash = ce->hash;
    int hl = hash->length;
    if (hl == 0) return 0;
    struct SyncHashInfoList *head = root->priv->remoteSeen;
    struct SyncHashInfoList *each = head;
    struct SyncHashInfoList *lag = NULL;
    sync_time mark = SyncCurrentTime();
    ce->lastUsed = mark;
    ce->lastRemoteFetch = mark;
    if (ce->state & SyncHashState_local)
        setCovered(ce);
    while (each != NULL) {
        if (ce == each->ce) {
            if (lag != NULL) {
                // move it to the front
                lag->next = each->next;
                each->next = head;
                root->priv->remoteSeen = each;
            }
            break;
        }
        lag = each;
        each = each->next;
    }
    if (each == NULL && add) {
        // need a new entry
        each = NEW_STRUCT(1, SyncHashInfoList);
        each->next = head;
        root->priv->remoteSeen = each;
    }
    if (debug >= CCNL_FINE) {
        char *hex = SyncHexStr(hash->buf, hash->length);
        char *extra = "";
        if (ce->state & SyncHashState_covered) extra = "covered, ";
        ccnr_msg(ccnr, "%s, root#%u, %s%s", here, root->rootId, extra, hex);
        free(hex);
    }
    if (each != NULL) {
        each->ce = ce;
        ce->busy++;
        each->lastSeen = mark;
    }
    return 1;
}

static struct SyncHashInfoList *
chooseRemoteHash(struct SyncRootStruct *root) {
    struct SyncHashInfoList *each = root->priv->remoteSeen;
    sync_time now = SyncCurrentTime();
    int64_t limit = ((int64_t)root->base->priv->rootAdviseLifetime)*3*M;
    struct SyncHashInfoList *lag = NULL;
    while (each != NULL) {
        struct SyncHashCacheEntry *ce = each->ce;
        struct SyncHashInfoList *next = each->next;
        if (ce != NULL
            && (ce->state & SyncHashState_remote)
            && ((ce->state & SyncHashState_covered) == 0)) {
            // choose the first entry that is remote and not covered
            int64_t dt = SyncDeltaTime(ce->lastUsed, now);
            if (dt < limit) return each;
            ce = NULL;
        }
        if (ce == NULL || (ce->state & SyncHashState_covered)) {
            // prune this entry
            if (lag == NULL) root->priv->remoteSeen = next;
            else lag->next = next;
            free(each);
        } else
            lag = each;
        each = next;
    }
    return NULL;
}

static int
fauxError(struct SyncBaseStruct *base) {
    // returns 1 with probability fauxErrorTrigger percent [roughly]
    if (base != NULL && base->priv->fauxErrorTrigger > 0) {
        int fet = base->priv->fauxErrorTrigger;
        if (fet > 0) {
            int r = random() % 100;
            if (r < fet) return 1;
        }
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////
///// Comparison internal routines
///////////////////////////////////////////////////////////////////////////

static void
destroyCompareData(struct SyncCompareData *data) {
    if (data == NULL) return;
    struct SyncRootStruct *root = data->root;
    struct SyncPrivate *priv = root->base->priv;
    if (root != NULL) {
        while (data->errList != NULL) {
            struct SyncActionData *sad = data->errList;
            destroyActionData(sad);
        }
        root->namesToFetch = SyncFreeNameAccumAndNames(root->namesToFetch);
        root->compare = NULL;
        struct SyncActionData *each = root->actions;
        // break the link from the action to the compare
        while (each != NULL) {
            if (each->comp == data) each->comp = NULL;
            each = each->next;
        }
    }
    if (priv->comparesBusy > 0) priv->comparesBusy--;
    ccn_charbuf_destroy(&data->hashL);
    ccn_charbuf_destroy(&data->hashR);
    ccn_charbuf_destroy(&data->cbL);
    ccn_charbuf_destroy(&data->cbR);
    data->twL = SyncTreeWorkerFree(data->twL);
    data->twR = SyncTreeWorkerFree(data->twR);
    if (data->ev != NULL && root != NULL) {
        data->ev->evdata = NULL;
        ccn_schedule_cancel(root->base->sched, data->ev);
    }
    free(data);
}

static void
abortCompare(struct SyncCompareData *data, char *why) {
    // this compare failed due to a node fetch or content fetch failure
    // we could get repeated failures if we try the same remote node,
    // so remove it from the seen remote nodes, then destroy the compare data
    if (data == NULL) return;
    struct SyncRootStruct *root = data->root;
    if (root != NULL) {
        char *here = "Sync.abortCompare";
        struct SyncBaseStruct *base = root->base;
        struct SyncRootPrivate *priv = root->priv;
        struct SyncHashInfoList *list = priv->remoteSeen;
        struct SyncHashInfoList *lag = NULL;
        struct ccn_charbuf *hash = data->hashR;
        while (list != NULL) {
            struct SyncHashInfoList *next = list->next;
            struct SyncHashCacheEntry *ce = list->ce;
            if (ce != NULL && compareHash(ce->hash, hash) == 0) {
                // found the failed root, so remove the remote entry
                // if we really needed it it will come back via root advise
                if (base->debug >= CCNL_INFO) {
                    // maybe this should be a warning?
                    char *hex = SyncHexStr(hash->buf, hash->length);
                    ccnr_msg(root->base->ccnr,
                             "%s, root#%u, remove remote hash %s",
                             here, root->rootId, hex);
                    free(hex);
                }
                list->next = NULL;
                list->ce = NULL;
                if (ce->busy > 0) ce->busy--;
                if (lag == NULL) priv->remoteSeen = next;
                else lag->next = next;
                free(list);
                break;
            }
            lag = list;
            list = next;
        }
        if (root->base->debug >= CCNL_WARNING)
            SyncNoteSimple(root, here, why);
    }
    destroyCompareData(data);
}

static int
extractBuf(struct ccn_charbuf *cb, struct SyncNodeComposite *nc, struct SyncNodeElem *ne) {
    struct ccn_buf_decoder ds;
    struct ccn_buf_decoder *d = SyncInitDecoderFromElem(&ds, nc, ne);
    ccn_charbuf_reset(cb);
    int res = SyncAppendElementInner(cb, d);
    return res;    
}

static struct SyncHashCacheEntry *
ensureRemoteEntry(struct SyncCompareData *data,
                  const unsigned char * xp,
                  ssize_t xs) {
    char *here = "Sync.ensureRemoteEntry";
    struct SyncRootStruct *root = data->root;
    struct SyncHashCacheEntry *ce = SyncHashEnter(root->ch, xp, xs, SyncHashState_remote);
    if (ce == NULL) {
        // and why did this fail?
        SyncNoteFailed(root, here, "bad enter", __LINE__);
        return ce;
    }
    if (ce->state & SyncHashState_local) setCovered(ce);
    return ce;
}

static struct SyncHashCacheEntry *
cacheEntryForElem(struct SyncCompareData *data,
                  struct SyncNodeComposite *nc,
                  struct SyncNodeElem *ne,
                  int remote) {
    char *here = "Sync.cacheEntryForElem";
    struct SyncRootStruct *root = data->root;
    struct ccn_buf_decoder ds;
    struct ccn_buf_decoder *d = SyncInitDecoderFromOffset(&ds, nc,
                                                          ne->start,
                                                          ne->stop);
    const unsigned char * xp = NULL;
    ssize_t xs = 0;
    SyncGetHashPtr(d, &xp, &xs);
    if (xs == 0 || xp == NULL) {
        // no hash?  this could be a problem
        SyncNoteFailed(root, here, "no hash", __LINE__);
        return NULL;
    }
    struct SyncHashCacheEntry *ce = NULL;
    if (remote > 0) {
        // the entry should be remote
        ce = ensureRemoteEntry(data, xp, xs);
    } else {
        // local entry, fetch it if missing
        ce = SyncHashLookup(root->ch, xp, xs);
        if (SyncCacheEntryFetch(ce) < 0) {
            SyncNoteFailed(root, here, "bad fetch", __LINE__);
            return NULL;
        }
    }
    if (ce == NULL) {
        // this entry should already exist
        SyncNoteFailed(root, here, "bad lookup", __LINE__);
        return ce;
    }
    ce->lastUsed = data->lastEnter;
    return ce;
}

static int
comparisonFailed(struct SyncCompareData *data, char *why, int line) {
    SyncNoteFailed(data->root, "Sync.CompareAction", why, line);
    data->state = SyncCompare_waiting;
    return -1;
}

static int
addNameFromCompare(struct SyncCompareData *data) {
    char *here = "Sync.addNameFromCompare";
    struct SyncRootStruct *root = data->root;
    int debug = root->base->debug;
    struct ccn_charbuf *name = data->cbR;
    if (root->namesToFetch == NULL)
        root->namesToFetch = SyncAllocNameAccum(0);
    SyncNameAccumAppend(root->namesToFetch, SyncCopyName(name), 0);
    struct SyncTreeWorkerEntry *tweR = SyncTreeWorkerTop(data->twR);
    tweR->pos++;
    tweR->count++;
    data->namesAdded++;
    if (debug >= CCNL_FINE) {
        SyncNoteUri(root, here, "added", name);
    }
    return 0;
}

/*
 * doPreload(data) walks the remote tree, and requests a fetch for every remote
 * node that is not covered locally and has not been fetched,
 * and is not being fetched.  This allows large trees to be fetched in parallel,
 * speeding up the load process.
 */
static int
doPreload(struct SyncCompareData *data) {
    struct SyncRootStruct *root = data->root;
    struct SyncTreeWorkerHead *twR = data->twR;
    int busyLim = root->base->priv->maxFetchBusy;
    for (;;) {
        if (data->nodeFetchBusy > busyLim) return 0;
        if (twR->level <= 0) break;
        struct SyncTreeWorkerEntry *ent = SyncTreeWorkerTop(twR);
        if (ent->cacheEntry == NULL)
            return -1;
        struct SyncHashCacheEntry *ceR = ent->cacheEntry;
        if (ceR == NULL
            || ceR->state & SyncHashState_fetching
            || ceR->state & SyncHashState_covered
            || ceR->state & SyncHashState_local) {
            // not a needed node, so pop it
        } else if (ceR->ncR != NULL) {
            // visit the children
            struct SyncNodeComposite *ncR = ceR->ncR;
            int lim = ncR->refLen;
            while (ent->pos < lim) {
                struct SyncNodeElem *ep = &ncR->refs[ent->pos];
                if ((ep->kind & SyncElemKind_leaf) == 0)
                    break;
                ent->pos++;
            }
            if (ent->pos < lim) {
                struct SyncNodeElem *ep = &ncR->refs[ent->pos];
                struct SyncHashCacheEntry *sub = cacheEntryForElem(data, ncR, ep, 1);
                if (sub == NULL)
                    return -1;
                ent = SyncTreeWorkerPush(twR);
                if (ent == NULL)
                    return -1;
                continue;
            }
        } else {
            // init the fetch, then pop
            SyncStartNodeFetch(root, ceR, data);
        }
        // common exit to pop and iterate
        ent = SyncTreeWorkerPop(twR);
        if (ent != NULL) ent->pos++;
    }
    while (data->nodeFetchBusy < busyLim) {
        // restart the failed node fetches (while we can)
        struct SyncActionData *sad = data->errList;
        if (sad == NULL) break;
        struct SyncHashCacheEntry *ceR = SyncHashLookup(root->ch,
                                                        sad->hash->buf,
                                                        sad->hash->length);
        SyncStartNodeFetch(root, ceR, data);
        destroyActionData(sad);
    }
    
    if (data->nodeFetchBusy > 0) return 0;
    if (data->errList != NULL) return 0;
    if (twR->level > 0) return 0;
    return 1;
}

/*
 * doComparison(data) is a key routine, because it determines what is
 * present in data->twR that is not present in data->twL.  It does so by
 * walking the two trees, L and R, in increasing name order.  To gain efficiency
 * doComparison avoids examining nodes in R that are already covered, and nodes
 * in L that have been bypassed in the walk of R.
 *
 * Ideally doComparison allows determination of k differences in O(k*log(N))
 * steps, where N is the number of names in the union of L and R.  However, if
 * the tree structures differ significantly the cost can be as high as O(N).
 */
static int
doComparison(struct SyncCompareData *data) {
    struct SyncRootStruct *root = data->root;
    struct SyncTreeWorkerHead *twL = data->twL;
    struct SyncTreeWorkerHead *twR = data->twR;
    
    for (;;) {
        struct SyncTreeWorkerEntry *tweR = SyncTreeWorkerTop(twR);
        if (tweR == NULL) {
            // the remote is done, so no more names to add
            return 1;
        }
        struct SyncHashCacheEntry *ceR = tweR->cacheEntry;
        if (ceR == NULL)
            return comparisonFailed(data, "bad cache entry for R", __LINE__);
        ceR->lastUsed = data->lastEnter;
        if (tweR->pos == 0 && isCovered(ceR)) {
            // short cut, nothing in R we don't have
            size_t c = tweR->count;
            tweR = SyncTreeWorkerPop(twR);
            if (tweR != NULL) {
                tweR->pos++;
                tweR->count += c;
            }
            continue;
        }
        struct SyncNodeComposite *ncR = ceR->ncR;
        if (ncR == NULL) {
            // top remote node not present, so go get it
            int nf = SyncStartNodeFetch(root, ceR, data);
            if (nf == 0) {
                // TBD: duplicate, so ignore the fetch?
                // for now, this is an error!
                return comparisonFailed(data, "node fetch duplicate?", __LINE__);
            } else if (nf > 0) {
                // node fetch started OK
            } else {
                // node fetch failed to initiate
                return comparisonFailed(data, "bad node fetch for R", __LINE__);
            }
            return 0;
        }
        if (tweR->pos >= ncR->refLen) {
            // we just went off the end of the current remote node, so pop it
            // skip over the processed element if we still have a node
            size_t c = tweR->count;
            if (c == 0) {
                // nothing was added, so this node must be covered
                setCovered(ceR);
            }
            tweR = SyncTreeWorkerPop(twR);
            if (tweR != NULL) {
                tweR->pos++;
                tweR->count += c;
            }
            continue;
        }
        struct SyncNodeElem *neR = SyncTreeWorkerGetElem(twR);
        if (neR == NULL)
            return comparisonFailed(data, "bad element for R", __LINE__);
        
        if (extractBuf(data->cbR, ncR, neR) < 0)
            // the remote name/hash extract failed
            return comparisonFailed(data, "bad extract for R", __LINE__);
        
        struct SyncTreeWorkerEntry *tweL = SyncTreeWorkerTop(twL);
        if (tweL == NULL) {
            // L is now empty, so add R
            if (neR->kind == SyncElemKind_node) {
                // to add a node R, push into it
                struct SyncHashCacheEntry *subR = cacheEntryForElem(data, ncR, neR, 1);
                if (subR == NULL || SyncTreeWorkerPush(twR) == NULL)
                    return comparisonFailed(data, "bad cache entry for R", __LINE__);
            } else {
                // R is a leaf
                addNameFromCompare(data);
            }
        } else {
            struct SyncHashCacheEntry *ceL = tweL->cacheEntry;
            if (SyncCacheEntryFetch(ceL) < 0)
                return comparisonFailed(data, "bad cache entry for L", __LINE__);
            struct SyncNodeComposite *ncL = ceL->ncL;
            ceL->lastUsed = data->lastEnter;
            if (tweL->pos >= ncL->refLen) {
                // we just went off the end of the current local node, so pop it
                tweL = SyncTreeWorkerPop(twL);
                if (tweL != NULL) tweL->pos++;
                continue;
            }
            struct SyncNodeElem *neL = SyncTreeWorkerGetElem(twL);
            if (neL == NULL || extractBuf(data->cbL, ncL, neL) < 0) {
                // the local name/hash extract failed
                return comparisonFailed(data, "bad extract for L", __LINE__);
            }
            if (neR->kind == SyncElemKind_node) {
                // quick kill for a remote node?
                struct SyncHashCacheEntry *subR = cacheEntryForElem(data, ncR, neR, 1);
                if (subR == NULL)
                    return comparisonFailed(data, "bad element for R", __LINE__);
                if (isCovered(subR)) {
                    // nothing to add, this node is already covered
                    // note: this works even if the remote node is not present!
                    tweR->pos++;
                    continue;
                }
                if (subR->ncR == NULL) {
                    // there is a remote hash, but no node present,
                    // so push into it to force the fetch
                    if (SyncTreeWorkerPush(twR) == NULL)
                        return comparisonFailed(data, "bad push for R", __LINE__);
                    continue;
                }
                
                if (neL->kind == SyncElemKind_leaf) {
                    // L is a leaf, R is a node that is present
                    enum SyncCompareResult scr = SyncNodeCompareMinMax(subR->ncR, data->cbL);
                    switch (scr) {
                        case SCR_before:
                            // L < Min(R), so advance L
                            tweL->pos++;
                            break;
                        case SCR_max:
                            // L == Max(R), advance both
                            tweL->pos++;
                            tweR->pos++;
                            break;
                        default:
                            // in all other cases, dive into R
                            if (SyncTreeWorkerPush(twR) == NULL)
                                return comparisonFailed(data, "bad push for R", __LINE__);
                            break;
                    }
                    
                } else {
                    // both L and R are nodes, test for L being present
                    struct SyncHashCacheEntry *subL = cacheEntryForElem(data, ncL, neL, 0);
                    if (subL == NULL || subL->ncL == NULL)
                        return comparisonFailed(data, "bad cache entry for L", __LINE__);
                    // both L and R are nodes, and both are present
                    struct SyncNodeComposite *ncL = subL->ncL;
                    struct SyncNodeComposite *ncR = subR->ncR;
                    int cmp = SyncCmpNames(ncR->minName, ncL->maxName);
                    if (cmp > 0) {
                        // Min(R) > Max(L), so advance L
                        tweL->pos++;
                    } else {
                        // dive into both nodes
                        if (SyncTreeWorkerPush(twL) == NULL)
                            return comparisonFailed(data, "bad push for L", __LINE__);
                        if (SyncTreeWorkerPush(twR) == NULL)
                            return comparisonFailed(data, "bad push for R", __LINE__);
                    }
                }
            } else {
                // R is a leaf
                if (neL->kind == SyncElemKind_leaf) {
                    // both L and R are names, so the compare is simple
                    int cmp = SyncCmpNames(data->cbL, data->cbR);
                    if (cmp == 0) {
                        // L == R, so advance both
                        tweL->pos++;
                        tweR->pos++;
                    } else if (cmp < 0) {
                        // L < R, advance L 
                        tweL->pos++;
                    } else {
                        // L > R, so add R
                        addNameFromCompare(data);
                    }
                } else {
                    // R is a leaf, but L is a node
                    struct SyncHashCacheEntry *subL = cacheEntryForElem(data, ncL, neL, 0);
                    if (subL == NULL || subL->ncL == NULL)
                        return comparisonFailed(data, "bad cache entry for L", __LINE__);
                    enum SyncCompareResult scr = SyncNodeCompareMinMax(subL->ncL, data->cbR);
                    switch (scr) {
                        case SCR_before:
                            // R < Min(L), so add R
                            addNameFromCompare(data);
                            break;
                        case SCR_max:
                            // R == Max(L), advance both
                            tweL->pos++;
                            tweR->pos++;
                            break;
                        case SCR_min:
                            // R == Min(L), advance R
                            tweR->pos++;
                            break;
                        case SCR_after:
                            // R > Max(L), advance L
                            tweL->pos++;
                            break;
                        case SCR_inside:
                            // Min(L) < R < Max(L), so dive into L
                            if (SyncTreeWorkerPush(twL) == NULL)
                                return comparisonFailed(data, "bad push for L", __LINE__);
                            break;
                        default:
                            // this is really broken
                            return comparisonFailed(data, "bad min/max compare", __LINE__);
                    }
                    
                }
            }
        }
    }
}

static int
fetchStablePoint(struct SyncBaseStruct *base, ccnr_hwm *ptr) {
    int res = 0;
    struct ccn_charbuf *cb = ccn_charbuf_create();
    struct ccn_charbuf *name = SyncCopyName(base->priv->localHostPrefix);
    res |= ccn_name_append_str(name, syncStableSuffix);
    struct ccn_charbuf *interest = SyncGenInterest(name, 1, 0, -1, 1, NULL);
    if (interest == NULL) return -__LINE__;
    // TBD: check the signature!
    res |= r_sync_lookup(base->ccnr, interest, cb);
    if (res >= 0) {
        // parse the object
        const unsigned char *xp = NULL;
        size_t xs = 0;
        res |= SyncPointerToContent(cb, NULL, &xp, &xs);
        if (res >= 0) {
            // extract the value
            uintmax_t val = 0;
            static char *key = "stable ";
            struct ccn_buf_decoder ds;
            struct ccn_buf_decoder *d = ccn_buf_decoder_start(&ds, xp, xs);
            res = -__LINE__;
            if (ccn_buf_match_dtag(d, CCN_DTAG_StringValue)) {
                ccn_buf_advance(d);
                res = -__LINE__;
                if (ccn_buf_match_blob(d, &xp, &xs)) {
                    int ks = strlen(key);
                    ccn_buf_advance(d);
                    res = -__LINE__;
                    if (xs > ks && strncmp(key, (char *) xp, ks) == 0) {
                        xp = xp + ks;
                        for (;;) {
                            unsigned char c = *xp;
                            if (c < '0' || c > '9') break;
                            val = val * 10 + (c - '0');
                            xp++;
                            res = 0;
                        }
                        if (ptr != NULL && res == 0)
                            *ptr = ccnr_hwm_decode(base->ccnr, val);
                    }
                }
            }
        }
    }
    
    ccn_charbuf_destroy(&cb);
    ccn_charbuf_destroy(&name);
    ccn_charbuf_destroy(&interest);
    return res;
}

static int
storeStablePoint(struct SyncBaseStruct *base, ccnr_hwm point) {
    struct ccn_charbuf *x = ccn_charbuf_create();
    struct ccn_charbuf *name = SyncCopyName(base->priv->localHostPrefix);
    int res = 0;
    char temp[32];
    int nc = snprintf(temp, sizeof(temp), "stable %ju",
                      ccnr_hwm_encode(base->ccnr, point));
    // TBD: find a better encoding & use better tags?
    res |= ccnb_append_tagged_blob(x, CCN_DTAG_StringValue, temp, nc);
    res |= ccn_name_append_str(name, syncStableSuffix);
    res |= ccn_create_version(base->ccn, name, CCN_V_NOW, 0, 0);
    res |= ccn_name_append_numeric(name, CCN_MARKER_SEQNUM, 0);
    if (res >= 0) {
        // sign and store the buffer
        // freshness need not be long at all
        struct ccn_charbuf *cob = SyncSignBuf(base, x, name,
                                              1, CCN_SP_FINAL_BLOCK);
        if (cob != NULL) {
            res |= r_sync_local_store(base->ccnr, cob);
            ccn_charbuf_destroy(&cob);
        }
    }
    ccn_charbuf_destroy(&name);
    ccn_charbuf_destroy(&x);
    return res;
}

static int
isRootStableEnough(struct SyncRootStruct *root, ccnr_hwm target) {
    if (root->namesToAdd->len == 0) return 1;
    return 0;
}

// purge the nodes associated with cache entries that have not been
// recently used, provided that the nodes are not reachable from the current
// sync tree root
static void
purgeOldEntries(struct SyncRootStruct *root) {
    char *here = "Sync.purgeOldEntries";
    struct SyncHashCacheHead *ch = root->ch;
    struct ccn_charbuf *hashL = root->currentHash;
    if (hashL == NULL || hashL->length == 0) return;
    struct SyncHashCacheEntry *ceL = SyncHashLookup(ch,
                                                    hashL->buf,
                                                    hashL->length);
    if (ceL == NULL) return;
    struct SyncTreeWorkerHead *twL = SyncTreeWorkerCreate(ch, ceL, 0);
    sync_time now = SyncCurrentTime();
    int64_t trigger = cachePurgeTrigger*M;
    SyncHashClearMarks(ch);
    SyncTreeMarkReachable(twL, 0);
    int hx = 0;
    for (hx = 0; hx < ch->mod; hx++) {
        struct SyncHashCacheEntry *ce = ch->ents[hx];
        while (ce != NULL) {
            if ((ce->state & SyncHashState_marked) == 0
                && ce->state & SyncHashState_stored) {
                // stable, but not reachable using current tree
                struct SyncNodeComposite *ncL = ce->ncL;
                if (ncL != NULL) {
                    int64_t dt = SyncDeltaTime(ce->lastUsed, now);
                    if (dt > trigger) {
                        // old enough to know better
                        ce->ncL = NULL;
                        ncL = SyncNodeDecRC(ncL);
                        if (root->base->debug >= CCNL_FINE) {
                            char *hex = SyncHexStr(ce->hash->buf, ce->hash->length);
                            SyncNoteSimple(root, here, hex);
                            free(hex);
                        }
                    }
                }
            }
            ce = ce->next;
        }
    }
    SyncTreeWorkerFree(twL);
}

static int
CompareAction(struct ccn_schedule *sched,
              void *clienth,
              struct ccn_scheduled_event *ev,
              int flags) {
    char *here = "Sync.CompareAction";
    struct SyncCompareData *data = (struct SyncCompareData *) ev->evdata;
    if (data == NULL || data->root == NULL) {
        // invalid, not sure how we got here
        return -1;
    }
    data->lastEnter = SyncCurrentTime();
    struct SyncRootStruct *root = data->root;
    struct ccnr_handle *ccnr = root->base->ccnr;
    int debug = root->base->debug;
    if (data->ev != ev || flags & CCN_SCHEDULE_CANCEL) {
        // orphaned or cancelled
        if (debug >= CCNL_FINE)
            SyncNoteSimple(root, here, "orphan?");
        data->ev = NULL;
        return -1;
    }
    
    int delay = shortDelayMicros;
    switch (data->state) {
        case SyncCompare_init:
            // nothing to do, flow into next state
            if (debug >= CCNL_FINE)
                SyncNoteSimple(root, here, "init");
            data->state = SyncCompare_preload;
        case SyncCompare_preload: {
            // nothing to do (yet), flow into next state
            if (debug >= CCNL_FINE)
                SyncNoteSimple(root, here, "preload");
            struct SyncHashCacheEntry *ceR = SyncHashLookup(root->ch,
                                                            data->hashR->buf,
                                                            data->hashR->length);
            SyncTreeWorkerInit(data->twR, ceR, 1);
            int res = doPreload(data);
            if (res < 0) {
                abortCompare(data, "doPreload failed");
                return -1;
            }
            if (res == 0) {
                // not yet preloaded
                if (data->nodeFetchBusy > 0) {
                    // rely on SyncRemoteFetchResponse to restart us
                    data->ev = NULL;
                    delay = -1;
                }
                break;
            }
            // before switch to busy, reset the remote tree walker
            SyncTreeWorkerInit(data->twR, ceR, 1);
            data->state = SyncCompare_busy;
        }
        case SyncCompare_busy: {
            // come here when we are comparing the trees
            if (debug >= CCNL_FINE)
                SyncNoteSimple(root, here, "busy");
            int res = doComparison(data);
            if (res < 0) {
                abortCompare(data, "doComparison failed");
                return -1;
            }
            if (data->errList != NULL) {
                // we had a load started during compare, so retreat a state
                data->state = SyncCompare_preload;
                if (debug >= CCNL_WARNING)
                    SyncNoteSimple(root, here, "retreat one state");
                break;
            }
            if (res == 0)
                // comparison not yet complete
                break;
            // either full success or failure gets here
            data->state = SyncCompare_waiting;
        }
        case SyncCompare_waiting: {
            if (debug >= CCNL_FINE)
                SyncNoteSimple(root, here, "waiting");
            struct SyncNameAccum *namesToFetch = root->namesToFetch;
            int busyLim = root->base->priv->maxFetchBusy;
            int len = ((namesToFetch != NULL) ? namesToFetch->len : 0);
            if (debug >= CCNL_FINE) {
                int pos = data->contentPos;
                ccnr_msg(ccnr, "%s, root#%u, pos %d, names %d",
                         here, root->rootId, pos, len);
            }
            while (data->contentFetchBusy < busyLim
                   && data->contentPos < len) {
                // initiate the content fetches
                int pos = data->contentPos;
                struct ccn_charbuf *name = namesToFetch->ents[pos].name;
                SyncStartContentFetch(root, name, data);
                data->contentPos = pos + 1;
            }
            while (data->contentFetchBusy < busyLim) {
                // restart the failed fetches
                struct SyncActionData *sad = data->errList;
                if (sad == NULL) break;
                SyncStartContentFetch(root, sad->prefix, data);
                destroyActionData(sad);
            }
            if (data->contentFetchBusy > 0) {
                // rely on SyncRemoteFetchResponse to restart us
                data->ev = NULL;
                delay = -1;
                break;
            }
            data->state = SyncCompare_done;
        }
        case SyncCompare_done: {
            // cleanup
            sync_time now = SyncCurrentTime();
            int64_t mh = SyncDeltaTime(data->lastEnter, now);
            int64_t dt = SyncDeltaTime(data->startTime, now);
            root->priv->stats->comparesDone++;
            root->priv->stats->lastCompareMicros = dt;
            if (mh > data->maxHold) data->maxHold = mh;
            mh = (mh + 500) / 1000;
            dt = (dt + 500) / 1000;

            if (debug >= CCNL_INFO) {
                int reportStats = root->base->priv->syncActionsPrivate & 4;
                char temp[64];
                snprintf(temp, sizeof(temp)-2,
                         "%d.%03d secs [%d.%03d], %d names added",
                         (int) (dt / 1000), (int) (dt % 1000),
                         (int) (mh / 1000), (int) (mh % 1000),
                         (int) data->namesAdded);
                SyncNoteSimple2(root, here, "done", temp);
                if (reportStats) {
                    struct ccn_charbuf *cb = ccn_charbuf_create();
                    formatStats(root, cb);
                    char *str = ccn_charbuf_as_string(cb);
                    ccnr_msg(root->base->ccnr, "%s, %s", here, str);
                    ccn_charbuf_destroy(&cb);
                }
            }
            destroyCompareData(data);
            return -1;
        }
        default: break;
    }
    int64_t mh = SyncDeltaTime(data->lastEnter, SyncCurrentTime());
    if (mh > data->maxHold) data->maxHold = mh;
    return delay;
}

static void
kickCompare(struct SyncCompareData *scd, struct SyncActionData *action) {
    // we just got content for a particular action
    // may need to restart CompareAction
    if (scd != NULL && scd->ev == NULL) {
        struct ccn_scheduled_event *ev = ccn_schedule_event(scd->root->base->sched,
                                                            shortDelayMicros,
                                                            CompareAction,
                                                            scd,
                                                            0);
        scd->ev = ev;
    }
}

///////////////////////////////////////////////////////////////////////////
///// Tree building internal routines
///////////////////////////////////////////////////////////////////////////

static struct SyncHashCacheEntry *
newNodeCommon(struct SyncRootStruct *root,
              struct SyncNodeAccum *nodes,
              struct SyncNodeComposite *nc) {
    // finish building and inserting a local node
    char *here = "Sync.newNodeCommon";
    int debug = root->base->debug;
    struct ccnr_handle *ccnr = root->base->ccnr;
    if (nc == NULL || nc->hash == NULL) {
        SyncNoteFailed(root, here, "bad node", __LINE__);
        return NULL;
    }
    struct SyncHashCacheHead *ch = root->ch;
    struct ccn_charbuf *hash = nc->hash;
    struct SyncHashCacheEntry *ce = SyncHashLookup(ch, hash->buf, hash->length);
    SyncCacheEntryFetch(ce);
    if (ce != NULL && ce->ncL != NULL) {
        // and equivalent local node is already in the cache
        // so get rid of the new node and return the existing entry
        if (debug >= CCNL_FINE) {
            char *hex = SyncHexStr(hash->buf, hash->length);
            SyncNoteSimple2(root, here, "suppressed duplicate", hex);
            free(hex);
        }
        SyncFreeComposite(nc);
        nc = ce->ncL;
        root->priv->stats->nodesShared++;
    } else {
        struct SyncPrivate *priv = root->base->priv;
        // must ensure that there is a local cache entry
        ce = SyncHashEnter(ch, hash->buf, hash->length, SyncHashState_local);
        if (ce == NULL) {
            // this should not have happened!
            SyncNoteFailed(root, here, "bad enter", __LINE__);
            SyncNodeDecRC(nc);
            return NULL;
        }
        ce->ncL = nc;
        if (ce->state & SyncHashState_remote)
            setCovered(ce);
        // queue this cache entry for storing
        ce->state |= SyncHashState_storing;
        if (priv->storingTail == NULL) {
            // storing queue is empty
            priv->storingHead = ce;
        } else {
            // append to the tail
            priv->storingTail->storing = ce;
        }
        priv->storingTail = ce;
        priv->nStoring++;
        root->priv->stats->nodesCreated++;
        if (nc->cb->length >= nodeSplitTrigger) {
            // if this happens then our split estimate was wrong!
            if (debug >= CCNL_INFO)
                ccnr_msg(ccnr,
                         "%s, root#%u, cb->length (%d) >= nodeSplitTrigger (%d)",
                         here, root->rootId,
                         (int) nc->cb->length, (int) nodeSplitTrigger);
        }
    }
    SyncNodeIncRC(nc);
    SyncAccumNode(nodes, nc);
    return ce;
}

static struct SyncHashCacheEntry *
nodeFromNodes(struct SyncRootStruct *root, struct SyncNodeAccum *na) {
    char *here = "Sync.nodeFromNodes";
    struct SyncHashCacheHead *ch = root->ch;
    struct SyncBaseStruct *base = root->base;
    struct ccnr_handle *ccnr = base->ccnr;
    int debug = base->debug;
    int lim = na->len;
    if (lim == 0) {
        SyncNoteFailed(root, here, "empty", __LINE__);
        return NULL;
    }
    if (lim == 1) {
        // just return the singleton node
        struct SyncNodeComposite *nc = na->ents[0];
        if (nc == NULL || nc->hash == NULL) {
            SyncNoteFailed(root, here, "bad node", __LINE__);
            return NULL;
        }
        struct SyncHashCacheEntry *ce = SyncHashLookup(ch,
                                                       nc->hash->buf,
                                                       nc->hash->length);
        if (ce == NULL)
            SyncNoteFailed(root, here, "bad lookup", __LINE__);
        return ce;
    }
    
    int accLim = nodeSplitTrigger - nodeSplitTrigger/8;
    struct SyncNodeAccum *nodes = SyncAllocNodeAccum(0);
    struct SyncHashCacheEntry *ce = NULL;
    int j = 0;
    while (j < lim) {
        int maxLen = 0;
        int i = j;
        struct SyncNodeComposite *nc = SyncAllocComposite(base);
        int accLen = nc->cb->length;
        // first, loop to find the run length
        while (i < lim && accLen < accLim) {
            struct SyncNodeComposite *elem = na->ents[i];
            i++;
            int nodeLen = elem->hash->length + 8;
            if (nodeLen > maxLen) maxLen = nodeLen;
            accLen = accLen + nodeLen + (maxLen - nodeLen) * 2;
        }
        
        // append the references in the run
        while (j < i) {
            struct SyncNodeComposite *elem = na->ents[j];
            j++;
            SyncNodeAddNode(nc, elem);
        }
        SyncEndComposite(nc); // finish the node
        ce = newNodeCommon(root, nodes, nc);
    }
    // go recursive just in case we need the extra levels
    ce = nodeFromNodes(root, nodes);
    nodes = SyncFreeNodeAccum(nodes);
    if (debug >= CCNL_FINE) {
        ccnr_msg(ccnr, "%s, root#%u, %d refs", here, root->rootId, lim);
    }
    return ce;
}

extern int
SyncStartSliceEnum(struct SyncRootStruct *root) {
    char *here = "Sync.SyncStartSliceEnum";
    struct SyncBaseStruct *base = root->base;
    if (base->priv->sliceBusy == 0) {
        int debug = root->base->debug;
        struct ccn_charbuf *name = root->namingPrefix;
        struct ccn_charbuf *nin = SyncGenInterest(name,
                                                  -1, -1, -1, -1,
                                                  NULL);
        int res = r_sync_enumerate(base->ccnr, nin);
        
        ccn_charbuf_destroy(&nin);
        if (res > 0) {
            if (debug >= CCNL_INFO)
                SyncNoteUri(root, here, "slice enum start", name);
            base->priv->sliceBusy = res;
            root->priv->sliceBusy = res;
            return 1;
        } else if (debug >= CCNL_SEVERE) {
            SyncNoteUri(root, here, "slice enum failed", name);
            return -1;
        }
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////
///// Main dispatching routine, the heart beat
///////////////////////////////////////////////////////////////////////////

static int
HeartbeatAction(struct ccn_schedule *sched,
                void *clienth,
                struct ccn_scheduled_event *ev,
                int flags) {
    char *here = "Sync.HeartbeatAction";
    struct SyncBaseStruct *base = (struct SyncBaseStruct *) ev->evdata;
    if (base == NULL || base->priv == NULL || (flags & CCN_SCHEDULE_CANCEL)) {
        // TBD: and why did this happen? (can't report it, though)
        return -1;
    }
    
    struct SyncPrivate *priv = base->priv;
    if (priv->sliceEnum > 0) {
        // we are still busy enumerating the slices, so reschedule
        return shortDelayMicros;
    }
    
    // check for first root that needs a slice enumeration
    struct SyncRootStruct *root = priv->rootHead;
    while (root != NULL) {
        if (root->priv->sliceBusy < 0 && priv->sliceBusy == 0) {
            // this root needs an enumeration
            if (SyncStartSliceEnum(root) < 0)
                return priv->heartbeatMicros;
            return shortDelayMicros;
        }
        root = root->next;
    }
    sync_time now = SyncCurrentTime();
    root = priv->rootHead;
    int64_t lifeMicros = ((int64_t) priv->rootAdviseLifetime)*M;
    int64_t needMicros = ((int64_t) updateNeedDelta)*M;
        
    while (root != NULL) {
        struct SyncRootPrivate *rp = root->priv;
        struct SyncCompareData *comp = root->compare;
        if (rp->sliceBusy < 0 && priv->sliceBusy == 0) {
            // this root needs an enumeration
            if (SyncStartSliceEnum(root) < 0)
                return priv->heartbeatMicros;
        } else if (priv->sliceBusy > 0) {
            // this root is busy enumerating
        } else if (root->update != NULL) {
            // update is busy, so don't process this root
        } else if (comp == NULL) {
            // only run the update when not comparing
            size_t addLen = root->namesToAdd->len;
            int64_t deltaAdvise = SyncDeltaTime(rp->lastAdvise, now);
            int64_t deltaUpdate = SyncDeltaTime(rp->lastUpdate, now);
            int64_t needUpdate = needMicros;
            if (addLen == rp->prevAddLen)
                // no change recently, so 
                needUpdate = rp->stats->lastUpdateMicros * 2;
            if (rp->adviseNeed <= 0 && deltaAdvise > lifeMicros)
                // it's been a while since the last RootAdvise
                rp->adviseNeed = adviseNeedReset;
            if (deltaUpdate >= needUpdate) {
                // TBD: determine if this is a good algorithm for adaptive update  
                if (addLen > 0) {
                    // need to update the root
                    SyncUpdateRoot(root);
                }
                if (root->currentHash->length > 0) {
                    struct SyncHashCacheEntry *ce = SyncHashLookup(root->ch,
                                                                   root->currentHash->buf,
                                                                   root->currentHash->length);
                    if (ce != NULL && (ce->state & SyncHashState_local)) {
                        if (rp->adviseNeed > 0
                            || ce != rp->lastLocalSent) {
                            SyncSendRootAdviseInterest(root);
                        }
                    }
                } else
                    // empty hash, so try for a starting response
                    SyncSendRootAdviseInterest(root);
                if (root->update == NULL) {
                    struct SyncHashInfoList *x = chooseRemoteHash(root);
                    if (x != NULL) {
                        SyncStartCompareAction(root, x->ce->hash);
                    }
                }
            }
            rp->prevAddLen = root->namesToAdd->len;
        } else {
            // running a compare, check for stall or excessive time since last fetch
            int64_t dt = SyncDeltaTime(comp->lastMark, now);
            if (dt > updateStallDelta*M) {
                // periodic stall warning
                if (base->debug >= CCNL_WARNING)
                    SyncNoteSimple(root, here, "compare stalled?");
                comp->lastMark = now;
            }
            // test for fatal stall (based on last fetch time)
            dt = SyncDeltaTime(comp->lastFetchOK, now);
            if (dt > compareAssumeBad*M) {
                abortCompare(comp, "no progress");
            }
        }
        // TBD: prune eldest remote roots from list
        // TBD: prune old remote node entries from cache
        root = root->next;
    }
    int64_t deltaClean = SyncDeltaTime(priv->lastCacheClean, now);
    if (priv->useRepoStore && deltaClean >= cacheCleanDelta*M) {
        // time to try to clean a batch of cache entries
        // TBD: reclaim local nodes when not used for a while?
        int cleanRem = cacheCleanBatch;
        while (cleanRem > 0) {
            struct SyncHashCacheEntry *ce = priv->storingHead;
            if (ce == NULL) break;
            struct SyncHashCacheEntry *ceN = ce->storing;
            SyncCacheEntryStore(ce);
            priv->storingHead = ceN;
            if (ceN == NULL) priv->storingTail = ceN;
            if (priv->nStoring > 0) priv->nStoring--;
            root = ce->head->root;
            ccnr_hwm chw = ce->stablePoint;
            if (ccnr_hwm_compare(base->ccnr, chw, root->priv->stablePoint) > 0) {
                // the node that just got stored had a better stablePoint for the node
                root->priv->stablePoint = chw;
                root->priv->lastStable = now;
                if (ccnr_hwm_compare(base->ccnr, chw, priv->stableTarget) > 0)
                    priv->stableTarget = chw;
                if (base->debug >= CCNL_INFO) {
                    char temp[64];
                    snprintf(temp, sizeof(temp),
                             "newly stable at %ju",
                             ccnr_hwm_encode(base->ccnr, chw));
                    SyncNoteSimple(root, here, temp);
                }
            }
            cleanRem--;
        }
        priv->lastCacheClean = now;
    }
    if (priv->stableEnabled && priv->useRepoStore
        && priv->nStoring == 0
        && ccnr_hwm_compare(base->ccnr, priv->stableTarget, priv->stableStored) > 0) {
        // test for stability
        int unstable = 1;
        int64_t dt = SyncDeltaTime(priv->lastStable, now);
        if (dt > stableTimeTrig*M) {
            // worth examining the roots for stability
            unstable = 0;
            root = priv->rootHead;
            while (root != NULL) {
                if (!isRootStableEnough(root, priv->stableTarget)) {
                    unstable++;
                    break;
                }
                root = root->next;
            }
        }
        if (unstable == 0) {
            // can store out the stable target
            if (base->debug >= CCNL_INFO) {
                char temp[64];
                snprintf(temp, sizeof(temp),
                         "stable target reached at %ju",
                         ccnr_hwm_encode(base->ccnr, priv->stableTarget));
                ccnr_msg(base->ccnr, "%s, %s", here, temp);
            }
            int spRes = storeStablePoint(base, priv->stableTarget);
            if (spRes < 0 && base->debug >= CCNL_WARNING) {
                ccnr_msg(base->ccnr, "%s, warning: stable target not stored", here);
            }
            priv->lastStable = now;
            priv->stableStored = priv->stableTarget;
        }
    }
    return priv->heartbeatMicros;
}


///////////////////////////////////////////////////////////////////////////
///// External routines
///////////////////////////////////////////////////////////////////////////

extern int
SyncStartHeartbeat(struct SyncBaseStruct *base) {
    static char *here = "Sync.SyncStartHeartbeat";
    int res = -1;
    if (base != NULL && base->sched != NULL) {
        int debug = base->debug;
        struct ccnr_handle *ccnr = base->ccnr;
        struct SyncPrivate *priv = base->priv;
        struct ccn_charbuf *nin = SyncGenInterest(priv->sliceCmdPrefix,
                                                  -1, -1, -1, -1,
                                                  NULL);
        
        // once the slices are started we try to restart at the last commit point
        // (there may be none for a fresh repo, or if this feature is disabled)
        ccnr_hwm commitPoint = CCNR_NULL_HWM;
        if (priv->stableEnabled && priv->useRepoStore) {
            res = fetchStablePoint(base, &commitPoint);
            if (res < 0 && base->debug >= CCNL_WARNING) 
                ccnr_msg(base->ccnr, "%s, no stable recovery point", here);
        }
        r_sync_notify_after(base->ccnr, commitPoint);

        // at startup we ask for all of the existing slices
        res = r_sync_enumerate(ccnr, nin);
        ccn_charbuf_destroy(&nin);
        if (res > 0) {
            priv->sliceEnum = res;
            if (debug >= CCNL_INFO)
                ccnr_msg(ccnr, "%s, slice enumerate started, %d", here, res);
            res = 0;
        } else if (debug >= CCNL_WARNING) {
            // it is OK to fail here, since 
            ccnr_msg(ccnr, "%s, no slices found", here);
        }
        
        // next we schedule the heartbeat itself
        struct ccn_scheduled_event *ev = ccn_schedule_event(base->sched,
                                                            priv->heartbeatMicros,
                                                            HeartbeatAction,
                                                            base,
                                                            0);
        
        res = 0;
        if (ev == NULL) {
            if (debug >= CCNL_SEVERE)
                ccnr_msg(ccnr, "%s, initial schedule failed!", here);
            res = -1;
        }
    }
    return res;
}

// callback for when an interest gets a response
// used when fetching a remote content object by explicit name
// or when fetching a remote node
extern enum ccn_upcall_res
SyncRemoteFetchResponse(struct ccn_closure *selfp,
                        enum ccn_upcall_kind kind,
                        struct ccn_upcall_info *info) {
    static char *here = "Sync.SyncRemoteFetchResponse";
    struct SyncActionData *data = selfp->data;
    enum ccn_upcall_res ret = CCN_UPCALL_RESULT_OK;
    switch (kind) {
        case CCN_UPCALL_FINAL:
            selfp->data = destroyActionData(data);
            free(selfp);
            break;
        case CCN_UPCALL_CONTENT_UNVERIFIED:
            // TBD: fix this when we can actually verify
            // return CCN_UPCALL_RESULT_VERIFY;
#if (CCN_API_VERSION >= 4004)
        case CCN_UPCALL_CONTENT_RAW:
        case CCN_UPCALL_CONTENT_KEYMISSING:
#endif
        case CCN_UPCALL_INTEREST_TIMED_OUT:
        case CCN_UPCALL_CONTENT: {
            if (data == NULL) break;
            struct ccnr_handle *ccnr = data->ccnr;
            struct SyncRootStruct *root = data->root;
            struct SyncCompareData *comp = data->comp;
            if (root == NULL) break;
            int debug = root->base->debug;
            struct SyncRootStats *stats = root->priv->stats;
            size_t bytes = 0;
            int faux = fauxError(root->base);
            sync_time now = SyncCurrentTime();
            if (ccnr != NULL && info != NULL && info->pco != NULL && faux == 0
                && kind != CCN_UPCALL_INTEREST_TIMED_OUT)
                bytes = info->pco->offset[CCN_PCO_E];
            if (debug >= CCNL_INFO) {
                char temp[64];
                char *ns = "node";
                char *ks = "ok";
                if (faux) ks = "faux error";
                if (data->kind == SRI_Kind_Content) ns = "content";
                if (kind == CCN_UPCALL_INTEREST_TIMED_OUT) ks = "timeout!";
                int64_t dt = SyncDeltaTime(data->startTime, now);
                dt = (dt + 500) / 1000;
                if (bytes > 0) 
                    snprintf(temp, sizeof(temp),
                             "%s, %s, %d.%03d secs, %u bytes",
                             ns, ks, (int) (dt / 1000), (int) (dt % 1000),
                             (unsigned) bytes);
                else
                    snprintf(temp, sizeof(temp),
                             "%s, %s, %d.%03d secs",
                             ns, ks, (int) (dt / 1000), (int) (dt % 1000));
                SyncNoteUri(root, here, temp, data->prefix);
            }
            
            switch (data->kind) {
                case SRI_Kind_Content: {
                    if (bytes > 0) {
                        // we fetched the content, so store it to the repo
                        ret = r_sync_upcall_store(ccnr, CCN_UPCALL_CONTENT, info);
                        if (ret < 0) {
                            // note this specific failure cause
                            bytes = 0;
                            if (debug >= CCNL_SEVERE)
                                SyncNoteFailed(root, here, "content store", __LINE__);
                        } else {
                            // we need to update the tree, too
                            if (debug >= CCNL_FINE)
                                SyncNoteSimple(root, here, "content stored");
                        }
                    }
                    if (comp != NULL && comp->contentFetchBusy > 0)
                        comp->contentFetchBusy--;
                    if (bytes > 0) {
                        // content fetch wins
                        stats->contentFetchReceived++;
                        stats->contentFetchBytes += bytes;
                        if (comp != NULL)
                            comp->lastFetchOK = now;
                    } else {
                        // content fetch failed
                        if (kind == CCN_UPCALL_INTEREST_TIMED_OUT)
                            stats->contentFetchTimeout++;
                        stats->contentFetchFailed++;
                        if (comp != NULL) {
                            // remember that this one failed
                            comp->contentFetchFailed++;
                            if (!moveActionData(data, SyncActionState_error))
                                SyncNoteFailed(root, here, "moveActionData", __LINE__);
                            selfp->data = NULL;
                        }
                    }
                    // wake up CompareAction to handle more content
                    kickCompare(comp, data);
                    break;
                }
                case SRI_Kind_NodeFetch: {
                    // node fetch response
                    const unsigned char *xp = data->hash->buf;
                    ssize_t xs = data->hash->length;
                    char *hex = SyncHexStr(xp, xs);
                    struct SyncHashCacheEntry *ce = SyncHashLookup(root->ch, xp, xs);
                    if (bytes <= 0) {
                        // did not get the node at all
                    } else if (ce != NULL && (isCovered(ce) || ce->ncR != NULL)) {
                        // there was a race, and we no longer need this
                        // for stats, count this as a success
                        if (debug >= CCNL_FINE) {
                            SyncNoteSimple2(root, here, "remote node covered", hex);
                        }
                    } else {
                        // we actually need the node that arrived
                        struct SyncNodeComposite *ncR = extractNode(root, info);
                        if (ncR == NULL) {
                            // decoding error, so can't use
                            if (debug >= CCNL_SEVERE)
                                SyncNoteSimple2(root, here, "extractNode failed", hex);
                            bytes = 0;
                        } else {
                            // the entry can now be completed
                            ce = SyncHashEnter(root->ch, xp, xs, SyncHashState_remote);
                            ce->ncR = ncR;
                            SyncNodeIncRC(ncR);
                            if (debug >= CCNL_INFO) {
                                SyncNoteSimple2(root, here, "remote node entered", hex);
                            }
                            if (comp == NULL) {
                                if (debug >= CCNL_ERROR)
                                    SyncNoteSimple(root, here, "remote node comp == NULL");
                            }
                        }
                    }
                    if (comp != NULL && comp->nodeFetchBusy > 0)
                        comp->nodeFetchBusy--;
                    if (bytes > 0) {
                        // node fetch wins
                        stats->nodeFetchReceived++;
                        stats->nodeFetchBytes += bytes;
                        if (comp != NULL)
                            comp->lastFetchOK = now;
                    } else {
                        // node fetch fails
                        if (kind == CCN_UPCALL_INTEREST_TIMED_OUT)
                            stats->nodeFetchTimeout++;
                        else stats->nodeFetchFailed++;
                        if (comp != NULL) {
                            // remember that this one failed
                            if (!moveActionData(data, SyncActionState_error))
                                SyncNoteFailed(root, here, "moveActionData", __LINE__);
                            comp->nodeFetchFailed++;
                            selfp->data = NULL;
                        }
                    }
                    if (ce != NULL && (ce->state & SyncHashState_fetching))
                        // we are no longer fetching this node
                        ce->state -= SyncHashState_fetching;
                    kickCompare(comp, data);
                    free(hex);
                    break;
                }
                default:
                    // SHOULD NOT HAPPEN
                    ret = CCN_UPCALL_RESULT_ERR;
                    break;
            }
            break;
        }
        default:
            // SHOULD NOT HAPPEN
            ret = CCN_UPCALL_RESULT_ERR;
            break;
    }
    return ret;
}

extern int
SyncStartNodeFetch(struct SyncRootStruct *root,
                   struct SyncHashCacheEntry *ce,
                   struct SyncCompareData *comp) {
    static char *here = "Sync.SyncStartNodeFetch";
    enum SyncRegisterActionKind kind = SRI_Kind_NodeFetch;
    struct SyncBaseStruct *base = root->base;
    int debug = base->debug;
    struct ccn *ccn = base->ccn;
    if (ccn == NULL)
        return SyncNoteFailed(root, here, "bad ccn handle", __LINE__);
    // first, check for existing fetch of same hash
    struct ccn_charbuf *hash = ce->hash;
    struct SyncActionData *data = root->actions;
    if (ce->state & SyncHashState_fetching)
        // already busy
        return 0;
    while (data != NULL) {
        if (data->kind == kind && compareHash(data->hash, hash) == 0)
            return 0;
        data = data->next;
    }
    
    struct ccn_closure *action = NEW_STRUCT(1, ccn_closure);
    data = newActionData(kind);
    struct ccn_charbuf *name = constructCommandPrefix(root, kind);
    int res = -1;
    char *why = "constructCommandPrefix";
    if (name != NULL) {
        data->skipToHash = SyncComponentCount(name);
        ccn_name_append(name, hash->buf, hash->length);
        data->prefix = name;
        data->hash = ccn_charbuf_create();
        ccn_charbuf_append_charbuf(data->hash, hash);
        data->comp = comp;
        action->data = data;
        action->p = &SyncRemoteFetchResponse;
        
        struct ccn_charbuf *template = SyncGenInterest(NULL,
                                                       syncScope,
                                                       base->priv->fetchLifetime,
                                                       -1, 1, NULL);
        res = ccn_express_interest(ccn, name, action, template);
        if (res < 0) {
            why = "ccn_express_interest";
            if (debug >= CCNL_SEVERE) {
                char *hex = SyncHexStr(hash->buf, hash->length);
                SyncNoteSimple2(root, here, "failed to express interest", hex);
                free(hex);
            }
        } else {
            root->priv->stats->nodeFetchSent++;
            if (debug >= CCNL_INFO) {
                char *hex = SyncHexStr(hash->buf, hash->length);
                SyncNoteSimple2(root, here, "fetching", hex);
                free(hex);
            }
        }
        ccn_charbuf_destroy(&template);
    }
    if (res >= 0) {
        // link the request into the root
        linkActionData(root, data);
        comp->nodeFetchBusy++;
        ce->state |= SyncHashState_fetching;
        res = 1;
    } else {
        // return the storage
        comp->nodeFetchFailed++;
        data = destroyActionData(data);
        free(action);
        if (debug >= CCNL_SEVERE)
            SyncNoteFailed(root, here, why, __LINE__);
    }
    return res;
}

extern int
SyncStartContentFetch(struct SyncRootStruct *root,
                      struct ccn_charbuf *name,
                      struct SyncCompareData *comp) {
    static char *here = "Sync.SyncStartContentFetch";
    struct SyncBaseStruct *base = root->base;
    int debug = base->debug;
    struct ccn *ccn = base->ccn;
    if (ccn == NULL || name == NULL)
        return SyncNoteFailed(root, here, "bad ccnr handle", __LINE__);
    
    // first, test to see if the content is already in the repo (yes, it happens)
    struct ccn_charbuf *interest = SyncGenInterest(name, 1, 0, 0, -1, NULL); 
    int res = r_sync_lookup(base->ccnr, interest, NULL);
    ccn_charbuf_destroy(&interest);
    
    if (res >= 0) {
        // this name is already in the Repo, no need to fetch
        // (ignore the accession number through this path)
        if (debug >= CCNL_INFO)
            SyncNoteUri(root, here, "ignored, already present", name);
        SyncAddName(root->base, name, 0);
        res = 0;
    } else {
        struct ccn_closure *action = NEW_STRUCT(1, ccn_closure);
        struct SyncActionData *data = newActionData(SRI_Kind_Content);
        data->prefix = ccn_charbuf_create();
        ccn_charbuf_append_charbuf(data->prefix, name);
        data->comp = comp;
        action->data = data;
        action->p = &SyncRemoteFetchResponse;
        data->skipToHash = -1;  // no hash here
        struct ccn_charbuf *template = SyncGenInterest(NULL,
                                                       syncScope,
                                                       base->priv->fetchLifetime,
                                                       0, -1, NULL);
        res = ccn_express_interest(ccn, name, action, template);
        ccn_charbuf_destroy(&template);
        if (res >= 0) {
            // link the request into the root
            root->priv->stats->contentFetchSent++;
            linkActionData(root, data);
            res = 1;
            if (debug >= CCNL_INFO)
                SyncNoteUri(root, here, "fetching", name);
            comp->contentFetchBusy++;
        } else {
            // return the storage
            if (debug >= CCNL_SEVERE)
                SyncNoteUri(root, here, "failed", name);
            data = destroyActionData(data);
            free(action);
            comp->contentFetchFailed++;
        }
    }
    return res;
}

static struct SyncActionData *
SyncFindAction(struct SyncRootStruct *root, enum SyncRegisterActionKind kind) {
    struct SyncActionData *each = root->actions;
    while (each != NULL) {
        if (each->kind == kind) return each;
        each = each->next;
    }
    return NULL;
}

extern int
SyncAddName(struct SyncBaseStruct *base,
            struct ccn_charbuf *name,
            ccnr_accession item) {
    static char *here = "Sync.SyncAddName";
    struct SyncPrivate *priv = base->priv;
    int debug = base->debug;
    struct SyncRootStruct *root = priv->rootHead;
    int count = 0;
    while (root != NULL) {
        if (SyncRootLookupName(root, name) == SyncRootLookupCode_covered) {
            // ANY matching root gets an addition
            // add the name for later processing
            struct ccn_charbuf *prev = NULL;
            int pos = root->namesToAdd->len;
            if (pos > 0) prev = root->namesToAdd->ents[pos-1].name;
            if (prev != NULL && SyncCmpNames(name, prev) == 0) {
                // this is a duplicate, so forget it!
                if (debug >= CCNL_FINE) {
                    SyncNoteUri(root, here, "ignore dup", name);
                }
            } else {
                // not obviously a duplicate
                uintmax_t itemNum = ccnr_accession_encode(base->ccnr, item);
                SyncNameAccumAppend(root->namesToAdd, SyncCopyName(name), itemNum);
                if (item != CCNR_NULL_ACCESSION)
                    root->priv->highWater = ccnr_hwm_update(base->ccnr,
                                                            root->priv->highWater,
                                                            item);
                count++;
                if (debug >= CCNL_FINE) {
                    char temp[64];
                    // TBD: improve item reporting?
                    if (item >= CCNR_MIN_ACCESSION && item <= CCNR_MAX_ACCESSION) {
                        snprintf(temp, sizeof(temp), "added, %ju", itemNum);
                    } else {
                        snprintf(temp, sizeof(temp), "no accession");
                    }
                    SyncNoteUri(root, here, temp, name);
                }
            }
        }
        root = root->next;
    }
    if (item != CCNR_NULL_ACCESSION)
        base->highWater = ccnr_hwm_update(base->ccnr, base->highWater, item);
    return count;
}

static int
findAndDeleteRoot(struct SyncBaseStruct *base, char *here,
                  const unsigned char *hp, ssize_t hs) {
    struct SyncRootStruct *root = base->priv->rootHead;
    int debug = base->debug;
    while (root != NULL) {
        struct ccn_charbuf *sh = root->sliceHash;
        if (sh->length == hs && memcmp(sh->buf, hp, hs) == 0) {
            // matching an existing root, so delete it
            if (debug >= CCNL_INFO) {
                char *hex = SyncHexStr(hp, hs);
                ccnr_msg(base->ccnr,
                         "%s, root#%u, deleted, %s",
                         here, root->rootId, hex);
                free(hex);
            }
            // need to remove any pending stores for deleted roots
            struct SyncPrivate *priv = base->priv;
            struct SyncHashCacheEntry *ce = priv->storingHead;
            struct SyncHashCacheEntry *lag = NULL;
            while (ce != NULL) {
                struct SyncHashCacheEntry *ceN = ce->storing;
                if (ce->head->root == root) {
                    // this root is going away, so delink the pending store
                    if (lag == NULL) priv->storingHead = ceN;
                    else lag->storing = ceN;
                    if (priv->nStoring > 0) priv->nStoring--;
                } else lag = ce;
                if (ceN == NULL) priv->storingTail = lag;
                ce = ceN;
            }
            SyncRemRoot(root);
            return 1;
        }
        root = root->next;
    }
    if (debug >= CCNL_FINE) {
        char *hex = SyncHexStr(hp, hs);
        ccnr_msg(base->ccnr,
                 "%s, root not found, %s",
                 here, hex);
        free(hex);
    }
    return 0;
}

extern int
SyncHandleSlice(struct SyncBaseStruct *base, struct ccn_charbuf *name) {
    char *here = "Sync.SyncHandleSlice";
    char *why = NULL;
    struct ccnr_handle *ccnr = base->ccnr;
    int debug = base->debug;
    const unsigned char *hp = NULL;
    ssize_t hs = 0;
    int match = SyncPrefixMatch(base->priv->sliceCmdPrefix, name, 0);
    if (match < 0) return match;
    // the component after the prefix should be the hash
    SyncGetComponentPtr(name, match, &hp, &hs);
    why = "invalid hash";
    if (hs > 0 && hs < MAX_HASH_BYTES) {
        // we pass the first smoke test
        struct ccn_charbuf *content = ccn_charbuf_create();
        struct ccn_charbuf *interest = SyncGenInterest(name, 1, 0, -1, 1, NULL);
        int lookupRes = -__LINE__;
        if (interest != NULL) {
            lookupRes = r_sync_lookup(ccnr, interest, content);
            ccn_charbuf_destroy(&interest);
        }
        why = "bad fetch";
        if (lookupRes >= 0 && content->length > 0) {
            // we got the content
            struct ccn_parsed_ContentObject pcos;
            struct ccn_parsed_ContentObject *pco = &pcos;
            int parseRes = ccn_parse_ContentObject(content->buf,
                                                   content->length,
                                                   pco, NULL);
            const unsigned char *xp = NULL;
            size_t xs = 0;
            why = "bad content object";
            if (parseRes >= 0) {
                if (pco->type == CCN_CONTENT_GONE) {
                    findAndDeleteRoot(base, here, hp, hs);
                    ccn_charbuf_destroy(&content);
                    return 0;
                } else {
                    why = "bad content start";
                    parseRes = SyncPointerToContent(content, pco, &xp, &xs);
                    if (debug >= CCNL_SEVERE && (xs <= 0 || parseRes < 0)) {
                        // we can't get the pointer, so somebody is wrong
                        ssize_t start = pco->offset[CCN_PCO_B_Content];
                        ssize_t stop = pco->offset[CCN_PCO_E_Content];
                        int len = stop-start;
                        char *hex = SyncHexStr(content->buf+start, len);
                        ccnr_msg(ccnr,
                                 "%s, invalid content start, line %d, len %d, %s",
                                 here, -parseRes, len, hex);
                        free(hex);
                    }
                }
            }
            if (parseRes >= 0) {
                struct SyncRootStruct *root = base->priv->rootHead;
                while (root != NULL) {
                    struct ccn_charbuf *sh = root->sliceHash;
                    if (sh->length == hs && memcmp(sh->buf, hp, hs) == 0) {
                        // we already have this slice (or at least the hash matches)
                        // ignore anything else (first arrival wins)
                        if (debug >= CCNL_FINE) {
                            char *hex = SyncHexStr(hp, hs);
                            ccnr_msg(ccnr,
                                     "%s, new root ignored for slice %s",
                                     here, hex);
                            free(hex);
                        }
                        ccn_charbuf_destroy(&content);
                        return 0;
                    }
                    root = root->next;
                }
                why = "no content tag";
                struct ccn_buf_decoder rds;
                struct ccn_buf_decoder *rd = NULL;
                rd = ccn_buf_decoder_start(&rds, xp, xs);
                root = SyncRootDecodeAndAdd(base, rd);
                why = "slice decode";
                if (root != NULL) {
                    struct ccn_charbuf *sh = root->sliceHash;
                    if (sh->length == hs && memcmp(sh->buf, hp, hs) == 0) {
                        // this slice is new
                        if (debug >= CCNL_INFO) {
                            char *hex = SyncHexStr(hp, hs);
                            SyncNoteSimple2(root, here, "new root for slice", hex);
                            free(hex);
                        }
                        ccn_charbuf_destroy(&content);
                        return 1;
                    } else {
                        // hashes don't match, so whoever wrote the slice is at fault
                        // destroy the root, since it may well be bogus
                        // (we could have checked earlier, but the end-to-end check is better)
                        if (debug >= CCNL_WARNING) {
                            char *hexL = SyncHexStr(sh->buf, sh->length);
                            char *hexR = SyncHexStr(hp, hs);
                            ccnr_msg(ccnr, "%s, failed, hashes not equal, L %s, R %s",
                                     here, hexL, hexR);
                            free(hexL);
                            free(hexR);
                        }
                        root = SyncRemRoot(root);
                        if (root != NULL) {
                            // failed to remove the root, this could be nasty
                            SyncNoteFailed(root, here, "root not removed", __LINE__);
                        }
                    }
                }
                
            }
        }
        if (debug >= CCNL_SEVERE)
            ccnr_msg(ccnr, "%s, failed! (%s)", here, why);
        ccn_charbuf_destroy(&content);
    }
    return -1;
}

extern enum ccn_upcall_res
SyncInterestArrived(struct ccn_closure *selfp,
                    enum ccn_upcall_kind kind,
                    struct ccn_upcall_info *info) {
    static char *here = "Sync.SyncInterestArrived";
    struct SyncActionData *data = selfp->data;
    enum ccn_upcall_res ret = CCN_UPCALL_RESULT_OK;
    switch (kind) {
        case CCN_UPCALL_FINAL:
            data = destroyActionData(data);
            free(selfp);
            break;
        case CCN_UPCALL_INTEREST: {
            struct SyncRootStruct *root = data->root;
            if (root == NULL) break;
            struct SyncRootPrivate *rp = root->priv;
            struct SyncBaseStruct *base = root->base;
            int debug = base->debug;
            int skipToHash = data->skipToHash;
            const unsigned char *buf = info->interest_ccnb;
            struct ccn_indexbuf *comps = info->interest_comps;
            char *hexL = NULL;
            char *hexR = NULL;
            if ((info->pi->answerfrom & CCN_AOK_NEW) == 0) {
                // TBD: is this the right thing to do?
                if (debug >= CCNL_INFO)
                    SyncNoteUri(root, here, "CCN_AOK_NEW = 0", data->prefix);
                break;
            }
            switch (data->kind) {
                case SRI_Kind_None:
                    // not an active request, so ignore
                    break;
                case SRI_Kind_RootStats: {
                    char *who = getKindStr(data->kind);
                    struct ccn_charbuf *name = SyncCopyName(data->prefix);
                    ccn_create_version(info->h, name, CCN_V_NOW, 0, 0);
                    ccn_name_append_numeric(name, CCN_MARKER_SEQNUM, 0);
                    if (debug >= CCNL_FINE)
                        SyncNoteUri(root, here, who, name);
                    struct ccn_charbuf *cb = ccn_charbuf_create();
                    struct timeval tv = {0};
                    gettimeofday(&tv, 0);
                    char temp[128];
                    int pos = snprintf(temp, sizeof(temp),
                                       "%ju.%06u: ", 
                                       (uintmax_t) tv.tv_sec,
                                       (unsigned) tv.tv_usec);
                    ccn_charbuf_append(cb, temp, pos);
                    formatStats(root, cb);
                    struct ccn_charbuf *cob = SyncSignBuf(base, cb, name,
                                                          1, CCN_SP_FINAL_BLOCK);
                    int res = ccn_put(info->h, cob->buf, cob->length);
                    if (res >= 0) {
                        // we have success!
                        if (debug >= CCNL_INFO)
                            SyncNoteUri(root, here, "response sent", name);
                    } else {
                        if (debug >= CCNL_SEVERE)
                            SyncNoteUri(root, here, "response failed", name);
                    }
                    ccn_charbuf_destroy(&name);
                    ccn_charbuf_destroy(&cb);
                    ccn_charbuf_destroy(&cob);
                    ret = CCN_UPCALL_RESULT_INTEREST_CONSUMED;
                    break;
                }
                case SRI_Kind_AdviseInt:
                case SRI_Kind_FetchInt: {
                    const unsigned char *bufR = NULL;
                    size_t lenR = 0;
                    struct SyncHashCacheEntry *ceL = NULL;
                    struct SyncHashCacheEntry *ceR = NULL;
                    const unsigned char *bufL = root->currentHash->buf;
                    char *who = getKindStr(data->kind);
                    size_t lenL = root->currentHash->length;
                    ccn_name_comp_get(buf, comps, skipToHash, &bufR, &lenR);
                    if (bufR == NULL || lenR == 0) {
                        if (data->kind == SRI_Kind_FetchInt) {
                            // not well-formed, so ignore it
                            if (debug >= CCNL_SEVERE)
                                SyncNoteSimple2(root, here, who, "failed, no remote hash");
                            return ret;
                        }
                    } else hexR = SyncHexStr(bufR, lenR);
                    
                    if (debug >= CCNL_INFO) {
                        if (hexR == NULL)
                            SyncNoteSimple2(root, here, who, "empty remote hash");
                        else SyncNoteSimple3(root, here, who, "remote hash", hexR);
                    }
                    if (data->kind == SRI_Kind_AdviseInt) {
                        // worth noting the remote root
                        if (debug >= CCNL_FINER) {
                            ssize_t start = info->pi->offset[CCN_PI_B_Exclude];
                            ssize_t stop = info->pi->offset[CCN_PI_E_Exclude];
                            if (stop > start) {
                                // we appear to have an exclusion
                                struct ccn_buf_decoder ds;
                                struct ccn_buf_decoder *d = &ds;
                                ccn_buf_decoder_start(d, buf+start, stop - start);
                                reportExclude(root, d);
                            }
                        }
                        if (lenR != 0) {
                            ceR = SyncHashEnter(root->ch, bufR, lenR, SyncHashState_remote);
                            sync_time lastMark = ceR->lastRemoteFetch;
                            noteRemoteHash(root, ceR, 1);
                            rp->adviseNeed = adviseNeedReset;
                            // force any old interest to be inactive
                            if (lastMark == 0) {
                                // not entered, so we need to do a RootAdvise
                                struct SyncActionData *data = SyncFindAction(root, SRI_Kind_RootAdvise);
                                if (data != NULL) data->kind = SRI_Kind_None;
                            }
                        }
                        rp->stats->rootAdviseSeen++;
                    } else {
                        rp->stats->nodeFetchSeen++;
                    }
                    
                    if (lenL == 0) {
                        if (debug >= CCNL_INFO)
                            SyncNoteSimple2(root, here, who, "ignored (empty local root)");
                        if (lenR == 0) {
                            // both L and R are empty, so suppress short-term thrashing
                            rp->adviseNeed = 0;
                        } else if (root->namesToAdd->len > 0) {
                            if (debug >= CCNL_FINE)
                                SyncNoteSimple2(root, here, who, "new tree needed");
                        }
                        break;
                    }
                    if (data->kind == SRI_Kind_AdviseInt
                        && lenR == lenL && memcmp(bufL, bufR, lenR) == 0) {
                        // hash given is same as our root hash, so ignore the request
                        if (debug >= CCNL_INFO)
                            SyncNoteSimple2(root, here, who, "ignored (same hash)");
                        purgeOldEntries(root);
                        break;
                    }
                    
                    long fresh = base->priv->rootAdviseFresh;
                    // excessive freshness may be a problem when there is an A-B-C
                    // routing, and a node shows up in B's cache that mentions
                    // subnodes that C cannot reach
                    // TBD: come up with a better solution!
                    
                    hexL = SyncHexStr(bufL, lenL);
                    
                    rp->adviseNeed = adviseNeedReset;
                    
                    // we need to respond with our local root node
                    
                    if (data->kind == SRI_Kind_AdviseInt) {
                        // get the entry for the local root node
                        // should expire fairly quickly
                        ceL = SyncHashLookup(root->ch, bufL, lenL);
                        if (debug >= CCNL_INFO) {
                            SyncNoteSimple3(root, here, who, "local hash", hexL);
                        }
                    } else {
                        // get the entry for the requested local tree node
                        ceL = SyncHashLookup(root->ch, bufR, lenR);
                    }
                    // test for local root node being present
                    if (SyncCacheEntryFetch(ceL) < 0) {
                        // requested local node is probably not ours
                        if (debug >= CCNL_FINE) {
                            SyncNoteSimple3(root, here, who, "no local node", hexL);
                        }
                        break;
                    }
                    struct SyncNodeComposite *ncL = ceL->ncL;
                    
                    // root advise: name is prefix + hashIn + hashOut
                    // node fetch: name is prefix + hashIn
                    // empty hashes are OK, but must be encoded
                    struct ccn_charbuf *name = SyncCopyName(data->prefix);
                    ccn_name_append(name, bufR, lenR);
                    if (data->kind == SRI_Kind_AdviseInt)
                        // respond with the current local hash
                        ccn_name_append(name, bufL, lenL);
                    
                    // the content object is based on the node
                    struct ccn_charbuf *cob = NULL;
                    if (data->kind == SRI_Kind_FetchInt) {
                        // node fetch results need not expire
                        cob = ncL->content;
                    }
                    if (cob == NULL)
                        // don't already have it, so make it
                        cob = SyncSignBuf(base, ncL->cb, name,
                                          fresh, CCN_SP_FINAL_BLOCK);
                    
                    if (cob != NULL) {
                        // we have a response encoded
                        if (ccn_content_matches_interest(cob->buf, cob->length,
                                                         1, NULL,
                                                         info->interest_ccnb,
                                                         info->pi->offset[CCN_PI_E],
                                                         info->pi)) {
                            // we match the interest
                            int res = ccn_put(info->h, cob->buf, cob->length);
                            if (res >= 0) {
                                // we have success!
                                if (debug >= CCNL_INFO)
                                    SyncNoteUri(root, here, "response sent", name);
                            } else {
                                if (debug >= CCNL_SEVERE)
                                    SyncNoteUri(root, here, "response failed", name);
                            }
                            ret = CCN_UPCALL_RESULT_INTEREST_CONSUMED;
                        } else {
                            // the exclusion filter disallows it
                            if (debug >= CCNL_FINE)
                                SyncNoteUri(root, here, "no match", name);
                        }
                        if (data->kind == SRI_Kind_FetchInt) {
                            // ownership of the encoding transfers to the node
                            ncL->content = cob;
                        } else {
                            // for root advise, don't hold on to the encoding
                            // (it's not signed right for Node Fetch)
                            ccn_charbuf_destroy(&cob);
                        }
                    }
                    ccn_charbuf_destroy(&name);
                    break;
                }
                default:
                    // SHOULD NOT HAPPEN
                    ret = CCN_UPCALL_RESULT_ERR;
                    break;
            }
            if (hexL != NULL) free(hexL);
            if (hexR != NULL) free(hexR);
            break;
        }
        default:
            // SHOULD NOT HAPPEN
            ret = CCN_UPCALL_RESULT_ERR;
            break;
    }
    return ret;
    
}

static int
SyncRegisterInterest(struct SyncRootStruct *root,
                     enum SyncRegisterActionKind kind) {
    static char *here = "Sync.SyncRegisterInterest";
    int res = 0;
    struct SyncBaseStruct *base = root->base;
    int debug = base->debug;
    if (base->ccn == NULL)
        return -__LINE__;
    struct ccn_charbuf *prefix = constructCommandPrefix(root, kind);
    if (prefix != NULL) {
        // so far we have built the full prefix for the interest
        struct ccn_closure *action = NEW_STRUCT(1, ccn_closure);
        struct SyncActionData *data = newActionData(kind);
        data->prefix = prefix;
        data->skipToHash = SyncComponentCount(prefix);
        action->data = data;
        action->p = &SyncInterestArrived;
        
        // we can register the prefix
        res |= ccn_set_interest_filter(root->base->ccn, prefix, action);
        if (res < 0) {
            if (debug >= CCNL_SEVERE)
                SyncNoteUri(root, here, "ccn_set_interest_filter failed", prefix);
            data = destroyActionData(data);
        } else {
            linkActionData(root, data);
            if (debug >= CCNL_INFO)
                SyncNoteUri(root, here, getKindStr(kind), prefix);
        }
    } else {
        // bad input, so delete the prefix
        res = SyncNoteFailed(root, here, "bad prefix", __LINE__);
    }
    return res;
}

extern int
SyncRegisterInterests(struct SyncRootStruct *root) {
    char *here = "Sync.SyncRegisterInterests";
    struct SyncBaseStruct *base = root->base;
    struct ccn *ccn = base->ccn;
    if (ccn == NULL) return -1;
    int res = 0;
    if (base->debug >= CCNL_INFO) {
        // report the root registration and the hex values
        char *hex = SyncHexStr(root->sliceHash->buf, root->sliceHash->length);
        struct ccn_charbuf *uriTopo = NULL;
        char *msgTopo = "??";
        struct ccn_charbuf *topoPrefix = root->topoPrefix;
        if (topoPrefix != NULL && topoPrefix->length > 0) {
            uriTopo = SyncUriForName(topoPrefix);
            msgTopo = ccn_charbuf_as_string(uriTopo);
        }
        struct ccn_charbuf *uriPrefix = NULL;
        char *msgPrefix = "??";
        struct ccn_charbuf *namingPrefix = root->namingPrefix;
        if (namingPrefix != NULL && namingPrefix->length > 0) {
            uriPrefix = SyncUriForName(namingPrefix);
            msgPrefix = ccn_charbuf_as_string(uriPrefix);
        }
        
        ccnr_msg(root->base->ccnr,
                 "%s, root#%u, topo %s, prefix %s, hash %s",
                 here, root->rootId, msgTopo, msgPrefix, hex);
        
        struct SyncNameAccum *filter = root->filter;
        if (filter != NULL) {
            int i = 0;
            for (i = 0; i < filter->len; i++) {
                struct ccn_charbuf *uri = SyncUriForName(filter->ents[i].name);
                ccnr_msg(root->base->ccnr,
                         "%s, root#%u, op %d, pattern %s",
                         here, root->rootId,
                         (int) filter->ents[i].data,
                         ccn_charbuf_as_string(uri));
                ccn_charbuf_destroy(&uri);
            }
        }
        if (uriTopo != NULL) ccn_charbuf_destroy(&uriTopo);
        if (uriPrefix != NULL) ccn_charbuf_destroy(&uriPrefix);
        free(hex);
    }
    res |= SyncRegisterInterest(root, SRI_Kind_AdviseInt);
    res |= SyncRegisterInterest(root, SRI_Kind_FetchInt);
    res |= SyncRegisterInterest(root, SRI_Kind_RootStats);
    root->priv->adviseNeed = adviseNeedReset;
    return res;
}

// callback for when a root advise interest gets a response
extern enum ccn_upcall_res
SyncRootAdviseResponse(struct ccn_closure *selfp,
                       enum ccn_upcall_kind kind,
                       struct ccn_upcall_info *info) {
    static char *here = "Sync.SyncRootAdviseResponse";
    struct SyncActionData *data = selfp->data;
    enum ccn_upcall_res ret = CCN_UPCALL_RESULT_OK;
    switch (kind) {
        case CCN_UPCALL_FINAL:
            data = destroyActionData(data);
            free(selfp);
            break;
        case CCN_UPCALL_CONTENT_UNVERIFIED:
            ret = CCN_UPCALL_RESULT_VERIFY;
            break;
#if (CCN_API_VERSION >= 4004)
        case CCN_UPCALL_CONTENT_KEYMISSING:
            ret = CCN_UPCALL_RESULT_FETCHKEY;
            break;
#endif
        case CCN_UPCALL_INTEREST_TIMED_OUT: {
            if (data == NULL || info == NULL ||
                data->root == NULL || data->kind != SRI_Kind_RootAdvise) {
                // not active, no useful info
            } else {
                sync_time now = SyncCurrentTime();
                struct SyncRootStruct *root = data->root;
                int debug = root->base->debug;
                root->priv->stats->rootAdviseTimeout++;
                if (debug >= CCNL_INFO) {
                    char temp[64];
                    int64_t dt = SyncDeltaTime(data->startTime, now);
                    dt = (dt + 500) / 1000;
                    snprintf(temp, sizeof(temp),
                             "timeout, %d.%03d secs",
                             (int) (dt / 1000), (int) (dt % 1000));
                    SyncNoteUri(root, here, temp, data->prefix);
                }
                data->startTime = now;
                // as long as we need a response, keep expressing it
                ret = CCN_UPCALL_RESULT_REEXPRESS;
            }
            break;
        }
#if (CCN_API_VERSION >= 4004)
        case CCN_UPCALL_CONTENT_RAW:
#endif
        case CCN_UPCALL_CONTENT: {
            if (data == NULL || info == NULL ||
                data->root == NULL || data->kind != SRI_Kind_RootAdvise) {
                // not active, no useful info
                break;
            }
            struct SyncRootStruct *root = data->root;
            int debug = root->base->debug;
            if (debug >= CCNL_INFO) {
                struct ccn_charbuf *nm = SyncNameForIndexbuf(info->content_ccnb,
                                                             info->content_comps);
                size_t bytes = info->pco->offset[CCN_PCO_E];
                char temp[64];
                int64_t dt = SyncDeltaTime(data->startTime, SyncCurrentTime());
                dt = (dt + 500) / 1000;
                snprintf(temp, sizeof(temp),
                         "content, %d.%03d secs, %u bytes",
                         (int) (dt / 1000), (int) (dt % 1000),
                         (unsigned) bytes);
                SyncNoteUri(root, here, temp, nm);
                ccn_charbuf_destroy(&nm);
            }
            
            const unsigned char *hp = NULL;
            size_t hs = 0;
            size_t bytes = 0;
            int failed = 0;
            int cres = ccn_name_comp_get(info->content_ccnb,
                                         info->content_comps,
                                         data->skipToHash, &hp, &hs);
            if (cres < 0 || hp == NULL) {
                // bad hash, so complain
                failed++;
                SyncNoteFailed(root, here, "bad hash", __LINE__);
            } else if (fauxError(root->base)) {
                failed++;
                if (debug >= CCNL_WARNING)
                    SyncNoteSimple(root, here, "faux error");
            } else {
                struct SyncHashCacheEntry *ce = SyncHashEnter(root->ch, hp, hs,
                                                              SyncHashState_remote);
                noteRemoteHash(root, ce, 1);
                if (!isCovered(ce)) {
                    // may need to make an entry
                    struct SyncNodeComposite *nc = NULL;
                    char *hex = SyncHexStr(hp, hs);
                    if (ce != NULL && ce->ncR != NULL) {
                        nc = ce->ncR;
                        if (debug >= CCNL_INFO)
                            SyncNoteSimple2(root, here, "existing but not covered", hex);
                    } else {
                        nc = extractNode(root, info);
                        if (nc == NULL) {
                            // this is bad news, the parsing failed
                            failed++;
                            if (debug >= CCNL_SEVERE)
                                SyncNoteSimple2(root, here, "extractNode failed", hex);
                        } else {
                            // new entry
                            ce->ncR = nc;
                            SyncNodeIncRC(nc);
                            bytes = info->pco->offset[CCN_PCO_E];
                            if (debug >= CCNL_INFO)
                                SyncNoteSimple2(root, here, "remote entered", hex);
                        }
                    }
                    free(hex);
                }
            }
            if (failed) {
                root->priv->stats->rootAdviseFailed++;
            } else {
                root->priv->stats->rootAdviseReceived++;
                root->priv->stats->rootAdviseBytes += bytes;
            }
            break;
        }
        default:
            // SHOULD NOT HAPPEN
            ret = CCN_UPCALL_RESULT_ERR;
            break;
    }
    return ret;
}

extern int
SyncSendRootAdviseInterest(struct SyncRootStruct *root) {
    static char *here = "Sync.SyncSendRootAdviseInterest";
    enum SyncRegisterActionKind kind = SRI_Kind_RootAdvise;
    int debug = root->base->debug;
    struct SyncActionData *data = SyncFindAction(root, kind);
    struct SyncHashCacheEntry *ce = NULL;
    if (root->currentHash->length > 0) {
        ce = SyncHashLookup(root->ch,
                            root->currentHash->buf,
                            root->currentHash->length);
    }
    if (data != NULL) {
        // don't override exiting interest for this root unless the root has changed
        if (ce == NULL || ce == root->priv->lastLocalSent)
            return 0;
        // mark this as inactive, response to be ignored
        data->kind = SRI_Kind_None;
        if (debug >= CCNL_FINE)
            SyncNoteSimple(root, here, "marked old interest as inactive");
    }
    struct ccn_closure *action = NEW_STRUCT(1, ccn_closure);
    struct ccn_charbuf *prefix = constructCommandPrefix(root, kind);
    struct ccn_charbuf *hash = ccn_charbuf_create();
    
    ccn_charbuf_append_charbuf(hash, root->currentHash);
    ccn_name_append(prefix, hash->buf, hash->length);
    
    data = newActionData(kind);
    data->skipToHash = SyncComponentCount(prefix);
    data->hash = hash;
    data->prefix = prefix;
    action->data = data;
    action->p = &SyncRootAdviseResponse;
    
    struct SyncNameAccum *excl = exclusionsFromHashList(root, root->priv->remoteSeen);
    struct ccn_charbuf *template = SyncGenInterest(NULL,
                                                   syncScope,
                                                   root->base->priv->rootAdviseLifetime,
                                                   -1, -1,
                                                   excl);
    int res = ccn_express_interest(root->base->ccn,
                                   prefix,
                                   action,
                                   template);
    SyncFreeNameAccumAndNames(excl);
    ccn_charbuf_destroy(&template);
    if (res >= 0) {
        // link the request into the root
        if (root->priv->adviseNeed > 0) root->priv->adviseNeed--;
        linkActionData(root, data);
        root->priv->lastAdvise = SyncCurrentTime();
        root->priv->lastLocalSent = ce;
        root->priv->stats->rootAdviseSent++;
        if (debug >= CCNL_INFO)
            SyncNoteUri(root, here, "sent", prefix);
        return 1;
    } else {
        // failed, so return the storage
        data = destroyActionData(data);
        free(action);
        if (debug >= CCNL_ERROR)
            SyncNoteSimple(root, here, "ccn_express_interest failed");
        return -1;
    }
}

static int
MakeNodeFromNames(struct SyncUpdateData *ud, int split) {
    char *here = "Sync.MakeNodeFromNames";
    struct SyncRootStruct *root = ud->root;
    int debug = root->base->debug;
    struct SyncNameAccum *na = ud->sort;
    int lim = na->len;
    if (lim == 0)
        // should not have been called, but no harm done
        return 0;
    int i = 0;
    if (split == 0) split = lim;
    if (debug >= CCNL_FINE) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp),
                 "split %d, lim %d",
                 split, lim);
        SyncNoteSimple(root, here, tmp);
    }
    
    // accum the hash for the node, and see if it exists
    struct SyncLongHashStruct longHash;
    memset(&longHash, 0, sizeof(struct SyncLongHashStruct));
    longHash.pos = MAX_HASH_BYTES;
    for (i = 0; i < split; i++) {
        struct ccn_charbuf *name = na->ents[i].name;
        SyncAccumHash(&longHash, name);
    }
    ssize_t hs = MAX_HASH_BYTES-longHash.pos;
    unsigned char *hp = longHash.bytes+longHash.pos;
    struct SyncHashCacheEntry *ce = SyncHashLookup(root->ch, hp, hs);
    if (ce != NULL && ce->ncL != NULL) {
        // node already exists
        struct SyncNodeComposite *nc = ce->ncL;
        SyncNodeIncRC(nc);
        SyncAccumNode(ud->nodes, nc);
        root->priv->stats->nodesShared++;
        if (debug >= CCNL_FINE) {
            char *hex = SyncHexStr(hp, hs);
            SyncNoteSimple2(root, here, "existing local node", hex);
            free(hex);
        }
    } else {
        // need to create a new node
        if (debug >= CCNL_FINE) {
            char *hex = SyncHexStr(hp, hs);
            SyncNoteSimple2(root, here, "need new local node", hex);
            free(hex);
        }
        struct SyncNodeComposite *nc = SyncAllocComposite(root->base);
        for (i = 0; i < split; i++) {
            struct ccn_charbuf *name = na->ents[i].name;
            SyncNodeAddName(nc, name);
            ccn_charbuf_destroy(&name);
            na->ents[i].name = NULL;
        }
        SyncEndComposite(nc);
        newNodeCommon(root, ud->nodes, nc);
    }
    // shift remaining elements down in the name accum
    ud->nameLenAccum = 0;
    i = 0;
    while (split < lim) {
        struct ccn_charbuf *name = na->ents[split].name;
        ud->nameLenAccum += name->length;
        na->ents[i] = na->ents[split];
        na->ents[split].name = NULL;
        i++;
        split++;
    }
    na->len = i;
    return i;
}

static int
TryNodeSplit(struct SyncUpdateData *ud) {
    char *here = "Sync.TryNodeSplit";
    struct SyncNameAccum *na = ud->sort;
    int lim = na->len;
    if (lim == 0)
        // should not have been called, but no harm done
        return 0;
    struct SyncRootStruct *root = ud->root;
    int debug = root->base->debug;
    struct ccn_charbuf *prev = NULL;
    int accLim = nodeSplitTrigger - nodeSplitTrigger/8;
    int accMin = nodeSplitTrigger/2;
    int res = 0;
    int splitMethod = 3;  // was variable, now is constantly enabled
    int maxLen = 0;
    int accLen = 0;
    int prevMatch = 0;
    int split = 0;
    if (debug >= CCNL_FINE) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp),
                 "entered, %d names",
                 lim);
        SyncNoteSimple(root, here, tmp);
    }
    for (split = 0; split < lim; split++) {
        struct ccn_charbuf *name = na->ents[split].name;
        int nameLen = name->length + 8;
        if (nameLen > maxLen) maxLen = nameLen;
        accLen = accLen + nameLen + (maxLen - nameLen) * 2;
        prev = name;
        if (split+1 < lim) {
            if (splitMethod & 1) {
                // use level shift to split
                struct ccn_charbuf *next = na->ents[split+1].name;
                int match = SyncComponentMatch(name, next);
                if (accLen >= accMin
                    && (match < prevMatch || (match > prevMatch+1))) {
                    // force a break due to level changes
                    if (debug >= CCNL_FINE) {
                        char tmp[64];
                        snprintf(tmp, sizeof(tmp),
                                 "split %d, lim %d, match %d, prev %d, accLen %d",
                                 split, lim, match, prevMatch, accLen);
                        SyncNoteSimple2(root, here, "level split found", tmp);
                    }
                    break;
                }
                prevMatch = match;
            }
            if (splitMethod & 2) {
                // use bits of hash to split
                int pos = name->length - 9;
                if (pos > 0 && accLen >= accMin) {
                    unsigned c = name->buf[pos] & 255;
                    if (c < hashSplitTrigger) {
                        if (debug >= CCNL_FINE) {
                            char tmp[64];
                            snprintf(tmp, sizeof(tmp),
                                     "split %d, lim %d, x %u, accLen %d",
                                     split, lim, c, accLen);
                            SyncNoteSimple2(root, here, "hash split found", tmp);
                        }
                        break;
                    }
                }
            }
        }
        if (accLen >= accLim) {
            break;
        }
    }
    // at this point we take the first "split" elements into a node
    res = MakeNodeFromNames(ud, split);
    return res;
}
    
static int
AddUpdateName(struct SyncUpdateData *ud, struct ccn_charbuf *name) {
    struct SyncNameAccum *dst = ud->sort;
    int nameLen = name->length;
    int accLim = nodeSplitTrigger - nodeSplitTrigger/8;
    int res = 0;
    name = SyncCopyName(name);
    SyncNameAccumAppend(dst, name, 0);
    ud->nameLenAccum += nameLen;
    ud->namesAdded++;
    if (ud->nameLenAccum >= accLim) {
        // we should split, if it is possible
        res = TryNodeSplit(ud);
    }
    return res;
}

// merge the semi-sorted names and the old sync tree
// returns -1 for failure, 0 for incomplete, 1 for complete
static int
SyncTreeMergeNames(struct SyncTreeWorkerHead *head,
                   struct SyncUpdateData *ud) {
    char *here = "Sync.SyncTreeMergeNames";
    struct SyncRootStruct *root = ud->root;
    int debug = root->base->debug;
    IndexSorter_Base ixBase = ud->ixBase;
    struct SyncNameAccum *src = (struct SyncNameAccum *) ixBase->client;
    IndexSorter_Index srcPos = 0;
    struct ccn_charbuf *cb = ud->cb;
    int res = 0;
    int namesLim = ud->namesAdded+namesYieldInc;
    if (head != NULL) {
        while (res == 0) {
            struct SyncTreeWorkerEntry *ent = SyncTreeWorkerTop(head);
            if (ent == NULL) break;
            if (ent->cacheEntry == NULL) {
                // probably a real bug!
                res = -__LINE__;
                break;
            }
            struct SyncHashCacheEntry *ce = ent->cacheEntry;
            if (head->remote <= 0) SyncCacheEntryFetch(ce);
            struct SyncNodeComposite *nc = ((head->remote > 0) ? ce->ncR : ce->ncL);
            if (nc == NULL) {
                // should not happen
                res = -__LINE__;
                break;
            }
            int lim = nc->refLen;
            if (ent->pos >= lim) {
                // done with the current level, go back to the previous level
                ent = SyncTreeWorkerPop(head);
                if (ent == NULL) break;
                ent->pos++;
            } else {
                struct SyncNodeElem *ep = &nc->refs[ent->pos];
                if (ep->kind & SyncElemKind_leaf) {
                    // a leaf, so the element name is inline
                    enum SyncCompareResult cmp = SCR_after;
                    struct ccn_charbuf *name = NULL;
                    
                    if (ud->ixBase->len > 0) {
                        srcPos = IndexSorter_Best(ud->ixBase);
                        name = src->ents[srcPos].name;
                        if (name != NULL)
                            cmp = SyncNodeCompareLeaf(nc, ep, name);
                    }
                    switch (cmp) {
                        case SCR_before:
                            // add the name from src
                            AddUpdateName(ud, name);
                        case SCR_min:
                            // advance the src, remove duplicates
                            if (cmp == SCR_min) {
                                if (debug >= CCNL_FINE) {
                                    SyncNoteUri(root, here, "skip", name);
                                }
                            }
                            for (;;) {
                                IndexSorter_Rem(ud->ixBase);
                                if (ud->ixBase->len <= 0) break;
                                srcPos = IndexSorter_Best(ud->ixBase);
                                struct ccn_charbuf *next = src->ents[srcPos].name;
                                if (SyncCmpNames(name, next) != 0) break;
                                if (debug >= CCNL_FINE) {
                                    SyncNoteUri(root, here, "skip dup", next);
                                }
                            }
                            break;
                        case SCR_after:
                            // add the name from the tree
                            extractBuf(cb, nc, ep);
                            AddUpdateName(ud, cb);
                            ent->pos++;
                            break;
                        default:
                            // this is not kosher
                            res = -__LINE__;
                            break;
                    }
                    if (ud->namesAdded >= namesLim) {
                        int64_t dt = SyncDeltaTime(ud->entryTime, SyncCurrentTime());
                        if (dt >= namesYieldMicros) {
                            // need to yield
                            if (debug >= CCNL_FINE)
                                SyncNoteSimple(root, here, "yield");
                            return 0;
                        }
                        namesLim += namesYieldInc;
                    }
                } else {
                    // a node, so push into it
                    ent = SyncTreeWorkerPush(head);
                    if (ent == NULL) {
                        res = -__LINE__;
                        break;
                    }                
                }
            }
        }
    }
    if (res == 0) {
        // done with the tree, move items from the src
        while (ud->ixBase->len > 0) {
            srcPos = IndexSorter_Best(ud->ixBase);
            struct ccn_charbuf *name = src->ents[srcPos].name;
            AddUpdateName(ud, name);
            for (;;) {
                IndexSorter_Rem(ud->ixBase);
                if (ud->ixBase->len <= 0) break;
                srcPos = IndexSorter_Best(ud->ixBase);
                struct ccn_charbuf *next = src->ents[srcPos].name;
                if (SyncCmpNames(name, next) != 0) break;
            }
            if (ud->namesAdded >= namesLim) {
                int64_t dt = SyncDeltaTime(ud->entryTime, SyncCurrentTime());
                if (dt >= namesYieldMicros) {
                    // need to yield
                    if (debug >= CCNL_FINE)
                        SyncNoteSimple(root, here, "yield");
                    return 0;
                }
                namesLim += namesYieldInc;
            }
        }
        res = 1;
    }
    return res;
}

static int
UpdateAction(struct ccn_schedule *sched,
             void *clienth,
             struct ccn_scheduled_event *ev,
             int flags) {
    char *here = "Sync.UpdateAction";
    sync_time now = SyncCurrentTime();
    struct SyncUpdateData *ud = (struct SyncUpdateData *) ev->evdata;
    struct SyncRootStruct *root = ud->root;
    struct SyncBaseStruct *base = root->base;
    int debug = base->debug;
    struct ccnr_handle *ccnr = base->ccnr;
    int showEntry = base->priv->syncActionsPrivate & 8;
    // sync_time prevTime = ud->entryTime;
    ud->entryTime = now;
    switch (ud->state) {
        case SyncUpdate_init: {
            // we are initialized, and need to insert root->namesToAdd
            // only process a bounded number of names each time
            if (showEntry && debug >= CCNL_INFO) {
                SyncNoteSimple(root, here, "SyncUpdate_init");
            }
            struct SyncNameAccum *src = (struct SyncNameAccum *) ud->ixBase->client;
            IndexSorter_Index srcLen = src->len;
            IndexSorter_Index ix = ud->ixPos;
            IndexSorter_Index ixLim = ix+namesYieldInc;
            if (srcLen < ixLim) ixLim = srcLen;
            
            
            while (ix < srcLen) {
                if (ix > ixLim) {
                    int64_t dt = SyncDeltaTime(ud->entryTime, SyncCurrentTime());
                    if (dt >= namesYieldMicros) {
                        // need to yield
                        if (debug >= CCNL_FINE)
                            SyncNoteSimple(root, here, "yield");
                        break;
                    }
                    ixLim += namesYieldInc;
                }
                if (debug >= CCNL_FINE) {
                    struct ccn_charbuf *name = src->ents[ix].name;
                    SyncNoteUri(root, here, "insert", name);
                }
                IndexSorter_Add(ud->ixBase, ix);
                ix++;
            }
            ud->ixPos = ix;
            if (ix < srcLen)
                // not done yet, so take a break
                return shortDelayMicros;
            
            ud->state = SyncUpdate_inserted;
            struct SyncHashCacheEntry *ent = SyncRootTopEntry(root);
            if (ent != NULL && ud->tw == NULL) {
                SyncCacheEntryFetch(ent);
                ud->tw = SyncTreeWorkerCreate(root->ch, ent, 0);
            }
            ud->sort = SyncAllocNameAccum(0);
            ud->cb = ccn_charbuf_create();
            ud->nodes = SyncAllocNodeAccum(0);
            break;
        }
        case SyncUpdate_inserted: {
            // all names to be added are now in ud->ixBase
            // the old sync tree has not been changed
            if (showEntry && debug >= CCNL_INFO) {
                SyncNoteSimple(root, here, "SyncUpdate_inserted");
            }
            
            int res = SyncTreeMergeNames(ud->tw, ud);
            if (res == 0) break;
                // not done yet, pause requested
            res = MakeNodeFromNames(ud, 0);
            // done, either normally or with error
            // free the resources
            struct SyncNameAccum *src = (struct SyncNameAccum *) ud->ixBase->client;
            ud->tw = SyncTreeWorkerFree(ud->tw);
            src = SyncFreeNameAccumAndNames(src);
            IndexSorter_Free(&ud->ixBase);
            ccn_charbuf_destroy(&ud->cb);
            if (res < 0) {
                // this is bad news!
                ud->sort = SyncFreeNameAccumAndNames(ud->sort);
                SyncNoteFailed(root, here, "merge names", __LINE__);
                return res;
            }
            ud->state = SyncUpdate_busy;
            break;
        }
        case SyncUpdate_busy: {
            // ud->nodes has the noes created from the names
            // the last step is to make up the node superstructure
            if (showEntry && debug >= CCNL_INFO) {
                SyncNoteSimple(root, here, "SyncUpdate_busy");
            }
            int initCount = root->priv->currentSize;
            struct SyncHashCacheEntry *ce = nodeFromNodes(root, ud->nodes);
            int count = ud->namesAdded;
            if (ce == NULL) {
                count = SyncNoteFailed(root, here, "bad nodeFromNames()", __LINE__);
            } else {
                SyncCacheEntryFetch(ce);
                struct SyncNodeComposite *nc = ce->ncL;
                if (nc != NULL) {
                    struct ccn_charbuf *old = root->currentHash;
                    struct ccn_charbuf *hash = SyncLongHashToBuf(&nc->longHash);
                    char *hex = SyncHexStr(hash->buf, hash->length);
                    root->currentHash = hash;
                    root->priv->currentSize = count;
                    now = SyncCurrentTime();
                    if (compareHash(old, hash) != 0) {
                        // note the time of the last hash change
                        root->priv->lastHashChange = now;
                    }
                    int64_t dt = SyncDeltaTime(ud->startTime, now);
                    root->priv->stats->updatesDone++;
                    root->priv->stats->lastUpdateMicros = dt;
                    dt = (dt + 500) / 1000;
                    int64_t mh = SyncDeltaTime(ud->entryTime, now);
                    if (mh < ud->maxHold) mh = ud->maxHold;
                    mh = (mh + 500) / 1000;
                    if (debug >= CCNL_INFO) {
                        int reportStats = base->priv->syncActionsPrivate & 4;
                        char temp[256];
                        snprintf(temp, sizeof(temp)-2,
                                 "%d.%03d secs [%d.%03d], %d names, depth %d, hash %s",
                                 (int) (dt / 1000), (int) (dt % 1000),
                                 (int) (mh / 1000), (int) (mh % 1000),
                                 (int) count, (int) nc->treeDepth, hex);
                        SyncNoteSimple2(root, here, "done", temp);
                        if (reportStats) {
                            struct ccn_charbuf *cb = ccn_charbuf_create();
                            formatStats(root, cb);
                            char *str = ccn_charbuf_as_string(cb);
                            ccnr_msg(root->base->ccnr, "%s, %s", here, str);
                            ccn_charbuf_destroy(&cb);
                        }
                    }
                    struct SyncHashCacheEntry *chk = SyncRootTopEntry(root);
                    if (chk != ce)
                        count = SyncNoteFailed(root, here, "bad top entry", __LINE__);
                    else if (root->base->ccn != NULL)
                        // only do this if it is not the test code
                        SyncSendRootAdviseInterest(root);
                    if (old != NULL) ccn_charbuf_destroy(&old);
                    free(hex);
                    // when this root node is stored we will need to know the stable point
                    ccnr_hwm hwm = root->priv->highWater;
                    ce->stablePoint = hwm;
                    if (debug >= CCNL_INFO) {
                        char temp[64];
                        if (hwm != CCNR_NULL_HWM) {
                            snprintf(temp, sizeof(temp),
                                     "new stable point at %ju",
                                     ccnr_hwm_encode(root->base->ccnr, hwm));
                        } else {
                            snprintf(temp, sizeof(temp), "high water?");
                        }
                        SyncNoteSimple(root, here, temp);
                    }
                } else {
                    count = SyncNoteFailed(root, here, "bad node", __LINE__);
                }
            }
            root->priv->adviseNeed = adviseNeedReset;
            ud->sort = SyncFreeNameAccumAndNames(ud->sort);
            ud->nodes = SyncFreeNodeAccum(ud->nodes);
            if (count <= initCount) {
                // we were supposed to add something?
                if (debug >= CCNL_INFO) {
                    struct ccn_charbuf *hash = root->currentHash;
                    char *hex = SyncHexStr(hash->buf, hash->length);
                    ccnr_msg(ccnr,
                             "%s, root#%u, note, count %d, initCount %d, hash %s",
                             here, root->rootId, count, initCount, hex);
                    free(hex);
                }
            }
            root->update = NULL;
            free(ud);
            ev->evdata = NULL;
            return -1;
        }
        default: {
            // show that we are no longer updating
            return -1;
        }
    }
    int64_t edt = SyncDeltaTime(ud->entryTime, SyncCurrentTime());
    if (edt > ud->maxHold) ud->maxHold = edt;
    return shortDelayMicros;
}

extern int
SyncUpdateRoot(struct SyncRootStruct *root) {
    char *here = "Sync.UpdateAction";
    struct SyncNameAccum *acc = root->namesToAdd;
    if (acc->len == 0) return 0;
    sync_time now = SyncCurrentTime();
    struct SyncBaseStruct *base = root->base;
    struct ccnr_handle *ccnr = base->ccnr;
    struct SyncUpdateData *ud = NEW_STRUCT(1, SyncUpdateData);
    ud->root = root;
    ud->state = SyncUpdate_init;
    ud->startTime = now;
    ud->entryTime = now;
    ud->ixBase = IndexSorter_New(acc->len, -1);
    ud->ixBase->sorter = SyncNameAccumSorter;
    ud->ixBase->client = acc;
    ud->ixPos = 0;
    ud->initLen = root->priv->currentSize;
    struct ccn_scheduled_event *ev = ccn_schedule_event(base->sched,
                                                        0,
                                                        UpdateAction,
                                                        ud,
                                                        0);
    if (ev == NULL) {
        if (base->debug >= CCNL_SEVERE)
            ccnr_msg(ccnr, "%s, initial schedule failed!", here);
        free(ud);
        return -1;
    }
    root->priv->lastUpdate = now;
    root->update = ud;
    root->namesToAdd = SyncAllocNameAccum(0);
    if (base->debug >= CCNL_INFO) {
        struct ccn_charbuf *hash = root->currentHash;
        char *hex = SyncHexStr(hash->buf, hash->length);
        ccnr_msg(ccnr,
                 "%s, root#%u, start, toAdd %d, current %d, hash %s",
                 here, root->rootId,
                 (int) acc->len, (int) ud->initLen, hex);
        free(hex);
    }
    return 1;
}

extern int
SyncStartCompareAction(struct SyncRootStruct *root, struct ccn_charbuf *hashR) {
    char *here = "Sync.SyncStartCompareAction";
    struct SyncPrivate *priv = root->base->priv;
    if (root->compare != NULL
        || priv->comparesBusy >= priv->maxComparesBusy)
        return 0;
    
    struct ccn_charbuf *hashL = root->currentHash;
    struct SyncHashCacheEntry *ceL = NULL;
    
    if (hashL->length > 0) {
        // if L is not empty, check the cache entry
        ceL = SyncHashLookup(root->ch, hashL->buf, hashL->length);
        if (ceL == NULL)
            return SyncNoteFailed(root, here, "bad lookup for L", __LINE__);
    }
    struct SyncHashCacheEntry *ceR = SyncHashEnter(root->ch,
                                                   hashR->buf,
                                                   hashR->length,
                                                   SyncHashState_remote);
    if (ceR == NULL)
        return SyncNoteFailed(root, here, "bad lookup for R", __LINE__);
    
    int debug = root->base->debug;
    struct ccnr_handle *ccnr = root->base->ccnr;
    struct SyncCompareData *data = NEW_STRUCT(1, SyncCompareData);
    sync_time mark = SyncCurrentTime();
    data->startTime = mark;
    data->lastEnter = mark;
    data->lastMark = mark;
    data->lastFetchOK = mark;
    data->root = root;
    root->compare = data;
    root->namesToFetch = SyncFreeNameAccumAndNames(root->namesToFetch);
    data->twL = SyncTreeWorkerCreate(root->ch, ceL, 0);
    if (ceL != NULL) ceL->lastUsed = mark;
    data->twR = SyncTreeWorkerCreate(root->ch, ceR, 1);
    ceR->lastUsed = mark;
    data->hashL = ccn_charbuf_create();
    ccn_charbuf_append_charbuf(data->hashL, hashL);
    data->hashR = ccn_charbuf_create();
    ccn_charbuf_append_charbuf(data->hashR, hashR);
    
    data->cbL = ccn_charbuf_create();
    data->cbR = ccn_charbuf_create();
    
    data->state = SyncCompare_init;
    priv->comparesBusy++;
    
    kickCompare(data, NULL);
    
    if (debug >= CCNL_INFO) {
        char *hexL = SyncHexStr(hashL->buf, hashL->length);
        char *msgL = ((hashL->length > 0) ? hexL : "empty");
        char *hexR = SyncHexStr(hashR->buf, hashR->length);
        char *msgR = ((hashR->length > 0) ? hexR : "empty");
        ccnr_msg(ccnr, "%s, root#%u, L %s, R %s",
                 here, root->rootId, msgL, msgR);
        free(hexL);
        free(hexR);
    }
    
    return 1;
}

#undef M

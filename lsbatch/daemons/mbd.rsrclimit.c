/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mbd.rsrclimit.h"
#include "../../lsf/lib/lsi18n.h"
#include "daemons.h"
#include "mbd.h"
#include <string.h>
#include <sys/syslog.h>

#define NL_SETN 10

enum {
    RL_KEY_NAME,
    RL_KEY_HOSTS,
    RL_KEY_QUEUES,
    RL_KEY_USERS,
    RL_KEY_PROJECTS,
    RL_KEY_RESOURCE,
    RL_KEY_RESOURCES,
    RL_KEY_DESCRIPTION,
    RL_KEY_PER_HOST,
    RL_KEY_PER_QUEUE,
    RL_KEY_PER_USER,
    RL_KEY_PER_PROJECT,
    RL_KEY_JOBS,
    RL_KEY_SLOTS,
    RL_KEY_MEM,
    RL_KEY_SWP,
    RL_KEY_TMP,
    RL_KEY_SLOTS_PER_PROCESSOR,
    RL_KEY_SIZE
};

#define RL_FORMAT_UNDEFINED  0
#define RL_FORMAT_VOLCLAVA   1
#define RL_FORMAT_LSF        2

#define RL_DESC_MAX_LEN 4096

/* Buffer for limit account-table key: limitname#host#queue#user#project.
 * Each name component <= MAX_LSB_NAME_LEN(60); hostname <= MAXHOSTNAMELEN(64).
 * 60+1+64+1+60+1+60+1+60+1 = 309; 384 leaves ample margin. */
#define RL_KEY_BUF_LEN 512

static char *do_Limit(struct lsConf *conf, char *fname, int *lineNum);
static int readRLHvalues(struct keymap *keyList, char *linep,
                         struct lsConf *conf, char *lsfile,
                         int *LineNum, char *section,
                         int *hasUnknownKey,
                         struct keymap **resEntries,
                         int *nResEntries);
static int parseRLConsumer(char *value, enum rl_consumer_type consumerType,
                                RL_CONSUMER_T *consumer);
static int parseRLResources(char *value, RL_RESOURCE_T **resources,
                          int *nResources);
static int addSimpleResource(char *resName, char *value,
                             RL_RESOURCE_T **resources,
                             int *nResources, int *resSize,
                             int type);
static void freeRLLimit(RL_LIMIT_T *limit);
static void freeRLBitmapCacheEntry(void *data);
static void freeRLAllocLimit(RL_ALLOC_RLIMIT_T *allocLimit);
void freeRLAllocLimitEntry(LIST_ENTRY_T *entry);
static void setRLLimitLevel(RL_LIMIT_T *limit);
static int addLimitToConf(RL_LIMIT_T *limit);
static void initRLKeyList(struct keymap *keyList);
static int isValidName(const char *name);
static int isDuplicateName(const char *name);
static void dumpRLConsumer(RL_CONSUMER_T *consumer, int position);
static void dumpRLResource(RL_RESOURCE_T *resource);
static int getLimitKey4Job(RL_LIMIT_T *limit, struct jData *jp, char *hostname,
                           char *buf, int bufSize);
static void validateRLConsumerValue(char **valuePtr, enum rl_consumer_type consumerType,
                                     const char *limitName,
                                     const char *fname, int lineNum);
static int getRLMinAvailSlots4Host(struct jData *jp, struct hData *hp,
                                    RL_JOB_LIMITS_CACHE_T *jRLCache,
                                    RL_ALLOC_RLIMIT_T **allocLimit,
                                    RL_RESOURCE_AVAIL_T **rsrcAvail);
static void collectRunJobRLimits4Acct(struct jData *jp,
                                      RL_JOB_LIMITS_CACHE_T *outCache);
static float calcJobRsrcConsume(const RL_LIMIT_T *limitConf,
                                const RL_RESOURCE_T *rsrc,
                                const RL_ALLOC_RLIMIT_T *cacheLimit,
                                const struct resVal *jobResVal);
static float getRsrcLimitValue(const RL_LIMIT_T *limitConf,
                               const RL_RESOURCE_T *rsrc,
                               const struct hData *hp);
static void dumpResLimitConf(void);
static int *getRLBitmap(void *obj, int consumerPos);
static int *getRLBitmap4Query(const char *value, int consumerPos, int *isFree);
static int getRLBitmap4Job(struct jData *jp, struct hData *hp,
                           int *rlBitmaps[RL_CONSUMER_POSITION_MAX]);
static int getLimitKey(RL_LIMIT_T *limit, char *hostname,
                       char *queue, char *user, char *project,
                       char *buf, int bufSize);
static int getRsrcReserveType(int resNo);
static int *getRLAndBitmap(void);

/*the map of configured resource limits*/
RL_CONF_T generalRLConf;
/*bitmap size in int units, GET_INTNUM(generalRLConf.nLimits)*/
int rlBitmapSize = 0;
/*the usage of active resource limit, key is limitKey, value is RL_USAGE_T*/
hTab rlAccountTab;
/*The cache of job limits which life-cycle is a single scheduling session*/
RL_JOB_LIMITS_CACHE_T jobRLimitsCache;


 /*Cache bitmap result for consumers
  *key=value@pos, value=A bitmap
  */
static hTab rlBitmapCache;
static struct lsConf *resLimitFileConf = NULL;

/*Empty-consumer bitmaps: cached results for NULL consumer value (matches
 *all limits). Used by getRLBitmap() when obj is NULL for HOST/QUEUE/USER.
 *Freed and reset to NULL in freeRsrcLimitConf(). */
static int *hDefRLBitmap = NULL;
static int *qDefRLBitmap = NULL;
static int *uDefRLBitmap = NULL;

/*To avoid frequently malloc, we reusable AND-result bitmap buffer. Returned
 *by getRLAndBitmap(), it is fully overwritten by every caller before being
 *read. So please attention to copy it if child function need to use it.
 */
static int *rlAndBitmap = NULL;
static int rlAndBitmapCap = 0;

/********************************************************************************
 * initRlData
 * Description：
 *     Initialize resource limit data structures, zero out global config
 *     generalRLConf and initialize hash table rlLimitTab
 *
 * Input:
 *     None
 *
 * Return:
 *     None
 ********************************************************************************/
void initRlData(void)
{
    memset(&generalRLConf, 0, sizeof(generalRLConf));
    memset(&jobRLimitsCache, 0, sizeof(jobRLimitsCache));
}

/********************************************************************************
 * readRsrcLimitConf
 * Description：
 *     Read and parse lsb.resources config file to build resource limit
 *     configuration. Reload config file based on mbdInitFlags, parse line
 *     by line, identify Limit sections and invoke do_Limit to process them
 *
 * Input:
 *     mbdInitFlags [in]: MBD init flag, reload config when FIRST_START or
 *                        RECONFIG_CONF
 *
 * Return:
 *     None
 ********************************************************************************/
void
readRsrcLimitConf(int mbdInitFlags)
{
    static char fname[] = "readRsrcLimitConf";
    char file[PATH_MAX];
    char *cp, *word, *section;
    int lineNum = 0;

    if (mbdInitFlags == FIRST_START
        || mbdInitFlags == RECONFIG_CONF) {

        sprintf(file, "%s/lsb.resources",
                daemonParams[LSB_CONFDIR].paramValue);

        resLimitFileConf = ls_getconf(file);
        if (resLimitFileConf == NULL) {
            if (lserrno == LSE_NO_FILE) {
                return;
            }
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_getconf");
            if (lsb_CheckMode) {
                lsb_CheckError = FATAL_ERR;
                return;
            }
            mbdDie(MASTER_FATAL);
        }
    }

    if (resLimitFileConf == NULL) {
        return;
    }

    if (resLimitFileConf->confhandle == NULL) {
        return;
    }

    freeRsrcLimitConf();

    resLimitFileConf->confhandle->curNode
        = resLimitFileConf->confhandle->rootNode;
    resLimitFileConf->confhandle->lineCount = 0;

    for (;;) {
        cp = getNextLineC_conf(resLimitFileConf, &lineNum, TRUE);
        if (cp == NULL) {
            break;
        }

        word = getNextWord_(&cp);
        if (word == NULL) {
            continue;
        }

        if (strcasecmp(word, "begin") == 0) {
            section = getNextWord_(&cp);
            if (!section) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 1,
                        "%s: File %s at line %d: Section name expected after Begin; ignoring section"),
                          fname,
                          resLimitFileConf->confhandle->fname,
                          lineNum);
                lsb_CheckError = WARNING_ERR;
                doSkipSection_conf(resLimitFileConf, &lineNum,
                                   resLimitFileConf->confhandle->fname,
                                   "unknown");
                continue;
            }

            if (strcasecmp(section, "Limit") == 0) {
                do_Limit(resLimitFileConf,
                         resLimitFileConf->confhandle->fname,
                         &lineNum);
            } else {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 2,
                        "%s: File %s at line %d: Invalid section name <%s>; ignoring section"),
                          fname,
                          resLimitFileConf->confhandle->fname,
                          lineNum,
                          section);
                lsb_CheckError = WARNING_ERR;
                doSkipSection_conf(resLimitFileConf, &lineNum,
                                   resLimitFileConf->confhandle->fname,
                                   section);
            }
        } else if (strcasecmp(word, "end") == 0) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 3,
                    "%s: File %s at line %d: Unexpected End without matching Begin; ignoring"),
                      fname,
                      resLimitFileConf->confhandle->fname,
                      lineNum);
            lsb_CheckError = WARNING_ERR;
        } else {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 4,
                    "%s: File %s at line %d: Line outside Begin/End block; ignoring"),
                      fname,
                      resLimitFileConf->confhandle->fname,
                      lineNum);
            lsb_CheckError = WARNING_ERR;
        }
    }

    if (generalRLConf.nLimits > 0) {
        h_initTab_(&rlAccountTab, 511);
        h_initTab_(&rlBitmapCache, 511);
        rlBitmapSize = GET_INTNUM(generalRLConf.nLimits);
    }
    dumpResLimitConf();
}

/********************************************************************************
 * freeRsrcLimitConf
 * Description：
 *     Free all allocated memory in global resource limit config generalRLConf,
 *     including all limit entries and their internal data
 *
 * Input:
 *     None
 *
 * Return:
 *     None
 ********************************************************************************/
void
freeRsrcLimitConf(void)
{
    int i;

    /* Free cached bitmaps (PROJECT dimension) */
    h_freeTab_(&rlBitmapCache, freeRLBitmapCacheEntry);

    /* Free empty-consumer bitmaps; rebuilt on next readRsrcLimitConf().
     * nLimits/rlBitmapSize may change across reconfigs, so the cached
     * size-specific bitmaps must be discarded. */
    FREEUP(hDefRLBitmap);
    FREEUP(qDefRLBitmap);
    FREEUP(uDefRLBitmap);

    /* Free the reusable AND-result buffer; rebuilt on demand by
     * getRLAndBitmap() based on the new rlBitmapSize. */
    FREEUP(rlAndBitmap);
    rlAndBitmapCap = 0;

    for (i = 0; i < generalRLConf.nLimits; i++) {
        freeRLLimit(&generalRLConf.limits[i]);
    }
    FREEUP(generalRLConf.limits);
    generalRLConf.nLimits = 0;
}

/********************************************************************************
 * freeRLBitmapCacheEntry
 * Description:
 *     Free a single cached bitmap entry from rlBitmapCache
 *
 * Input:
 *     data [in]: Pointer to cached bitmap (int *)
 *
 * Return:
 *     None
 ********************************************************************************/
static void
freeRLBitmapCacheEntry(void *data)
{
    int *bitmap = (int *)data;
    FREEUP(bitmap);
}

/********************************************************************************
 * getRLAndBitmap
 * Description:
 *     Return a reusable int buffer of rlBitmapSize ints used to compute the
 *     AND of all consumer-dimension bitmap rows. The buffer is fully
 *     overwritten by every caller before being read (copy row 0 then AND the
 *     remaining rows), and downstream consumers (getJobRLimitOnHost) only read
 *     it via TEST_BIT, so reuse across jobs/hosts is safe. The buffer persists
 *     across the scheduling session and is grown on demand; never freed by
 *     callers.
 *
 * Return:
 *     int *: pointer to the buffer (rlBitmapSize ints). NULL if no limits
 *            configured (rlBitmapSize <= 0).
 ********************************************************************************/
static int *
getRLAndBitmap(void)
{
    static char fname[] = "getRLAndBitmap";

    if (rlBitmapSize <= 0) {
        return NULL;
    }
    if (rlAndBitmapCap < rlBitmapSize) {
        FREEUP(rlAndBitmap);
        rlAndBitmap = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);
        rlAndBitmapCap = rlBitmapSize;
    }
    return rlAndBitmap;
}

/********************************************************************************
 * freeRLAllocLimit
 * Description:
 *     Free an RL_ALLOC_RLIMIT_T struct and all its sub-allocations.
 *
 * Input:
 *     allocLimit [in]: Pointer to the RL_ALLOC_RLIMIT_T to free
 *
 * Return:
 *     None
 ********************************************************************************/
static void
freeRLAllocLimit(RL_ALLOC_RLIMIT_T *allocLimit)
{
    if (allocLimit == NULL) {
        return;
    }

    if (allocLimit->rsrcAvails != NULL) {
        FREEUP(allocLimit->rsrcAvails);
    }

    FREEUP(allocLimit);
}

/********************************************************************************
 * freeRLAllocLimitEntry
 * Description:
 *     Callback for listDestroy() to free an RL_ALLOC_RLIMIT_T list entry.
 *     The entry's forw/back pointers are part of LIST_ENTRY_T, so we cast
 *     back to the enclosing RL_ALLOC_RLIMIT_T.
 *
 * Input:
 *     entry [in]: LIST_ENTRY_T pointer (actually RL_ALLOC_RLIMIT_T *)
 *
 * Return:
 *     None
 ********************************************************************************/
void
freeRLAllocLimitEntry(LIST_ENTRY_T *entry)
{
    RL_ALLOC_RLIMIT_T *allocLimit = (RL_ALLOC_RLIMIT_T *)entry;
    freeRLAllocLimit(allocLimit);
}

/********************************************************************************
 * freeRLJobLimitsCache
 * Description:
 *     Free an RL_JOB_LIMITS_CACHE_T struct and its internal lists.
 *     Each list entry (RL_ALLOC_RLIMIT_T) is freed via freeRLAllocLimitEntry.
 *
 * Input:
 *     cache [in]: Pointer to the RL_JOB_LIMITS_CACHE_T to free
 *
 * Return:
 *     None
 ********************************************************************************/
void
freeRLJobLimitsCache(RL_JOB_LIMITS_CACHE_T *cache)
{
    if (cache == NULL) {
        return;
    }

    /* rlBitmaps is a fixed-size array of borrowed pointers (rows are owned
     * by the consumer cache, e.g. hData->rlBitmap); just clear the pointers. */
    memset(cache->rlBitmaps, 0, sizeof(cache->rlBitmaps));

    if (cache->mainRLimits != NULL) {
        listDestroy(cache->mainRLimits, freeRLAllocLimitEntry);
        cache->mainRLimits = NULL;
    }

    if (cache->hostRLimits != NULL) {
        listDestroy(cache->hostRLimits, freeRLAllocLimitEntry);
        cache->hostRLimits = NULL;
    }
}

/********************************************************************************
 * resetJobRLimitsCache4Host
 * Description:
 *     Reset the host-specific portion of jobRLimitsCache so that the next
 *     candidate host can be evaluated from a clean state.
 *     - Restore avail in mainRLimits from orgAvail (undo any per-slot
 *       deductions applied by mergeAllocRLimit2Cache)
 *     - Free hostRLimits (host-specific limits are rebuilt per host)
 ********************************************************************************/
void
resetJobRLimitsCache4Host(void)
{
    LIST_ITERATOR_T iter;
    LIST_ENTRY_T *ent;

    if (jobRLimitsCache.mainRLimits != NULL
        && !LIST_IS_EMPTY(jobRLimitsCache.mainRLimits)) {
        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, jobRLimitsCache.mainRLimits);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *curAllocLimit = (RL_ALLOC_RLIMIT_T *)ent;
            int i;

            for (i = 0; i < curAllocLimit->rsrcCnt; i++) {
                curAllocLimit->rsrcAvails[i].avail =
                    curAllocLimit->rsrcAvails[i].orgAvail;
            }
        }
    }

    if (jobRLimitsCache.hostRLimits != NULL) {
        listDestroy(jobRLimitsCache.hostRLimits, freeRLAllocLimitEntry);
        jobRLimitsCache.hostRLimits = NULL;
    }
}

/********************************************************************************
 * freeRLLimit
 * Description：
 *     Free dynamically allocated memory within a single RL_LIMIT_T struct,
 *     including name, description, consumers and resources arrays
 *
 * Input:
 *     limit [in]: Pointer to the RL_LIMIT_T struct to free
 *
 * Return:
 *     None
 ********************************************************************************/
static void
freeRLLimit(RL_LIMIT_T *limit)
{
    int i;

    if (limit == NULL) {
        return;
    }

    FREEUP(limit->name);
    FREEUP(limit->desc);

    if (limit->consumers) {
        for (i = 0; i < limit->nConsumers; i++) {
            FREEUP(limit->consumers[i].value);
        }
        FREEUP(limit->consumers);
    }
    limit->nConsumers = 0;

    if (limit->resources) {
        for (i = 0; i < limit->nResources; i++) {
            FREEUP(limit->resources[i].resName);
        }
        FREEUP(limit->resources);
    }
    limit->nResources = 0;
}

/********************************************************************************
 * initRLKeyList
 * Description：
 *     Initialize the keyword mapping table for resource limit config, associate
 *     each enum key with its keyword string, and clear all values and positions
 *
 * Input:
 *     keyList [out]: Keyword mapping table array to store key-value pairs
 *
 * Return:
 *     None
 ********************************************************************************/
static void
initRLKeyList(struct keymap *keyList)
{
    int i;

    keyList[RL_KEY_NAME].key = "NAME";
    keyList[RL_KEY_HOSTS].key = "HOSTS";
    keyList[RL_KEY_QUEUES].key = "QUEUES";
    keyList[RL_KEY_USERS].key = "USERS";
    keyList[RL_KEY_PROJECTS].key = "PROJECTS";
    keyList[RL_KEY_RESOURCE].key = "RESOURCE";
    keyList[RL_KEY_RESOURCES].key = "RESOURCES";
    keyList[RL_KEY_DESCRIPTION].key = "DESCRIPTION";
    keyList[RL_KEY_PER_HOST].key = "PER_HOST";
    keyList[RL_KEY_PER_QUEUE].key = "PER_QUEUE";
    keyList[RL_KEY_PER_USER].key = "PER_USER";
    keyList[RL_KEY_PER_PROJECT].key = "PER_PROJECT";
    keyList[RL_KEY_JOBS].key = "JOBS";
    keyList[RL_KEY_SLOTS].key = "SLOTS";
    keyList[RL_KEY_MEM].key = "MEM";
    keyList[RL_KEY_SWP].key = "SWP";
    keyList[RL_KEY_TMP].key = "TMP";
    keyList[RL_KEY_SLOTS_PER_PROCESSOR].key = "SLOTS_PER_PROCESSOR";
    keyList[RL_KEY_SIZE].key = NULL;

    for (i = 0; i < RL_KEY_SIZE; i++) {
        keyList[i].val = NULL;
        keyList[i].position = 0;
    }
}

/********************************************************************************
 * readRLHvalues
 * Description：
 *     Read key=value pairs from config file line by line and store them into
 *     keyList. Supports parenthesized values, stops when section end is reached
 *
 * Input:
 *     keyList [out]: Keyword mapping table to store parsed key-value pairs
 *     linep [in]: Current line string pointer
 *     conf [in]: Config file handle
 *     lsfile [in]: Config file name
 *     LineNum [in/out]: Current line number pointer
 *     section [in]: Current section name
 *     hasUnknownKey [out]: Flag indicating if unknown keywords exist
 *     resEntries [in/out]: Address of pointer to dynamically-growing array
 *                          of struct keymap entries storing resource name
 *                          (key) and value (val) pairs that are unrecognized
 *                          by keymap but valid resource names. Caller should
 *                          initialize *resEntries to NULL. The function
 *                          reallocates as needed. Both key and val in each
 *                          entry are dynamically allocated (safeSave).
 *     nResEntries [in/out]: Number of entries in *resEntries.
 *                           Caller should initialize to 0.
 *
 * Return:
 *     0: Successfully read until section end
 *     -1: Premature EOF, read failure, or out-of-memory during realloc
 ********************************************************************************/
static int
readRLHvalues(struct keymap *keyList, char *linep,
              struct lsConf *conf, char *lsfile,
              int *LineNum, char *section,
              int *hasUnknownKey,
              struct keymap **resEntries,
              int *nResEntries)
{
    static char fname[] = "readRLHvalues";
    char *key, *value, *sp, *sp1;
    int cap = 0;  /* current allocated capacity of *resEntries */

    while (linep != NULL) {
        sp = linep;
        key = getNextWord_(&linep);
        if (key == NULL) {
            goto nextLine;
        }

        sp1 = strchr(key, '=');
        if (sp1)
            *sp1 = '\0';

        value = strchr(sp, '=');
        if (!value) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 5,
                    "%s: %s(%d): missing '=' after keyword %s, section %s ignoring the line"),
                      fname, lsfile, *LineNum, key, section);
            lsb_CheckError = WARNING_ERR;
            goto nextLine;
        }

        value++;
        while (*value == ' ')
            value++;

        if (value[0] == '\0') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 6,
                    "%s: %s(%d): null value after keyword %s, section %s ignoring the line"),
                      fname, lsfile, *LineNum, key, section);
            lsb_CheckError = WARNING_ERR;
            goto nextLine;
        }

        if (value[0] == '(') {
            value++;
            sp1 = strrchr(value, ')');
            if (sp1)
                *sp1 = '\0';
        }

        if (putValue(keyList, key, value) < 0) {
            if (getResEntry(key) >= 0) {
                if (*nResEntries >= cap) {
                    int newCap = (cap == 0) ? 4 : cap * 2;
                    *resEntries = (struct keymap *)my_realloc(
                        *resEntries, newCap * sizeof(struct keymap),
                        fname);
                    cap = newCap;
                }
                (*resEntries)[*nResEntries].key = safeSave(key);
                (*resEntries)[*nResEntries].val = safeSave(value);
                (*resEntries)[*nResEntries].position = -1;
                (*nResEntries)++;
            } else {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 7,
                        "%s: %s(%d): unknown keyword %s in section %s, ignoring the line"),
                          fname, lsfile, *LineNum, key, section);
                lsb_CheckError = WARNING_ERR;
            }
            *hasUnknownKey = TRUE;
        }

nextLine:
        linep = getNextLineC_conf(conf, LineNum, TRUE);
        if (linep == NULL) {
            ls_syslog(LOG_ERR, I18N_PREMATURE_EOF,
                      fname, lsfile, *LineNum, section);
            return -1;
        }
        if (isSectionEnd(linep, lsfile, LineNum, section)) {
            return 0;
        }
    }

    return -1;
}

/********************************************************************************
 * isValidName
 * Description：
 *     Validate whether a limit name is legal, only letters, digits,
 *     underscores and hyphens are allowed, and length must not exceed
 *     MAX_LSB_NAME_LEN
 *
 * Input:
 *     name [in]: Name string to validate
 *
 * Return:
 *     TRUE: Name is valid
 *     FALSE: Name is invalid
 ********************************************************************************/
static int
isValidName(const char *name)
{
    const char *p;

    if (name == NULL || name[0] == '\0') {
        return FALSE;
    }

    if (strlen(name) > MAX_LSB_NAME_LEN) {
        return FALSE;
    }

    for (p = name; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-') {
            return FALSE;
        }
    }

    return TRUE;
}

/********************************************************************************
 * isDuplicateName
 * Description：
 *     Check if a limit name already exists (case-insensitive comparison)
 *
 * Input:
 *     name [in]: Name string to check
 *
 * Return:
 *     TRUE: Name already exists (duplicate)
 *     FALSE: Name does not exist (not duplicate)
 ********************************************************************************/
static int
isDuplicateName(const char *name)
{
    int i;

    for (i = 0; i < generalRLConf.nLimits; i++) {
        if (strcasecmp(generalRLConf.limits[i].name, name) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

/********************************************************************************
 * parseRLConsumer
 * Description：
 *     Parse consumer config value, supports PER(...) and SHARED(...) formats,
 *     extract the consumer list inside parentheses and fill RL_CONSUMER_T struct
 *
 * Input:
 *     value [in]: Consumer config value string, format PER(...) or SHARED(...)
 *     consumerType [in]: Consumer type (host/queue/user/project)
 *     consumer [out]: Parsed result filled into this struct
 *
 * Return:
 *     0: Parse succeeded
 *     -1: Parse failed, invalid value format
 ********************************************************************************/
static int
parseRLConsumer(char *value, enum rl_consumer_type consumerType,
                     RL_CONSUMER_T *consumer)
{
    static char fname[] = "parseRLConsumer";
    char buf[MAXLINELEN];
    char *parenOpen, *parenClose;
    int mode;
    int prefixLen;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    if (strncasecmp(value, "PER(", 4) == 0) {
        mode = RL_CONSUMER_MODE_PER;
        prefixLen = 4;
    } else if (strncasecmp(value, "SHARED(", 7) == 0) {
        mode = RL_CONSUMER_MODE_SHARED;
        prefixLen = 7;
    } else {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 8,
                "%s: Invalid consumer value <%s>; must be PER(...) or SHARED(...)"),
                  fname, value);
        return -1;
    }

    parenOpen = value + prefixLen - 1;
    parenClose = strrchr(parenOpen, ')');
    if (parenClose == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 9,
                "%s: Missing closing ')' in consumer value <%s>"),
                  fname, value);
        return -1;
    }

    strncpy(buf, value + prefixLen, parenClose - (value + prefixLen));
    buf[parenClose - (value + prefixLen)] = '\0';

    consumer->type = consumerType;
    consumer->mode = mode;
    consumer->value = safeSave(buf);

    return 0;
}

/********************************************************************************
 * validateRLConsumerValue
 * Description：
 *     Validate each name in consumer value exists in the cluster, remove
 *     non-existent names and log warnings. Supports host/hostgroup, queue,
 *     user/usergroup and project type validation
 *
 * Input:
 *     valuePtr [in/out]: Pointer to consumer value string, invalid names removed
 *     consumerType [in]: Consumer type
 *     limitName [in]: Limit name this consumer belongs to, for logging
 *     fname [in]: Config file name, for logging
 *     lineNum [in]: Line number, for logging
 *
 * Return:
 *     None
 ********************************************************************************/
static void
validateRLConsumerValue(char **valuePtr, enum rl_consumer_type consumerType,
                         const char *limitName,
                         const char *fname, int lineNum)
{
    static char funcName[] = "validateRLConsumerValue";
    char *buf;
    char *validBuf;
    int bufSize;
    char *sp, *word;
    int hasInvalid = FALSE;
    const char *typeStr;
    int validLen = 0;

    if (*valuePtr == NULL || (*valuePtr)[0] == '\0') {
        return;
    }

    bufSize = strlen(*valuePtr) + 1;
    buf = (char *)my_calloc(bufSize, sizeof(char), funcName);
    validBuf = (char *)my_calloc(bufSize, sizeof(char), funcName);

    switch (consumerType) {
    case RL_CONSUMER_TYPE_HOST:
        typeStr = "host/hostgroup";
        break;
    case RL_CONSUMER_TYPE_QUEUE:
        typeStr = "queue";
        break;
    case RL_CONSUMER_TYPE_USER:
        typeStr = "user/usergroup";
        break;
    case RL_CONSUMER_TYPE_PROJECT:
        typeStr = "project";
        break;
    default:
        return;
    }

    strcpy(buf, *valuePtr);
    sp = buf;

    while ((word = getNextWord_(&sp)) != NULL) {
        char *name = word;
        int isNegated = FALSE;
        int isGroupExplicit = FALSE;
        int valid = FALSE;
        int  nameLen = 0;

        if (name[0] == '~') {
            isNegated = TRUE;
            name++;
        }

        /* Trailing '/' marks an explicit group reference (e.g. "hg1/");
         * strip it and validate against group tables only. */
        if (consumerType == RL_CONSUMER_TYPE_HOST || consumerType == RL_CONSUMER_TYPE_USER) {
            nameLen = strlen(name);
            if (nameLen > 0 && name[nameLen - 1] == '/') {
                name[nameLen - 1] = '\0';
                isGroupExplicit = TRUE;
            }
        }

        if (strcmp(name, "all") == 0) {
            valid = TRUE;
        } else {
            switch (consumerType) {
            case RL_CONSUMER_TYPE_HOST:
                if (isGroupExplicit) {
                    if (getHGrpData(name) != NULL) {
                        valid = TRUE;
                    } else {
                        name[nameLen - 1] = '/';
                    }
                } else if (getHGrpData(name) != NULL
                           || getHostData(name) != NULL) {
                    valid = TRUE;
                }
                break;
            case RL_CONSUMER_TYPE_QUEUE:
                if (getQueueData(name) != NULL) {
                    valid = TRUE;
                }
                break;
            case RL_CONSUMER_TYPE_USER:
                if (isGroupExplicit) {
                    if (getUGrpData(name) != NULL) {
                        valid = TRUE;
                    } else {
                        name[nameLen - 1] = '/';
                    }
                } else if (getpwlsfuser_(name) != NULL
                           || getUGrpData(name) != NULL) {
                    valid = TRUE;
                }
                break;
            case RL_CONSUMER_TYPE_PROJECT:
                valid = TRUE;
                break;
            default:
                break;
            }
        }

        if (valid) {
            if (validLen > 0) {
                validBuf[validLen++] = ' ';
            }
            if (isNegated) {
                validBuf[validLen++] = '~';
            }
            strcpy(validBuf + validLen, name);
            validLen += strlen(name);
        } else {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 59,
                    "%s: %s(%d): limit <%s>: %s <%s> does not exist in cluster; ignored"),
                      funcName, fname, lineNum, limitName, typeStr, name);
            lsb_CheckError = WARNING_ERR;
            hasInvalid = TRUE;
        }
    }

    if (hasInvalid) {
        FREEUP(*valuePtr);
        if (validLen > 0) {
            *valuePtr = safeSave(validBuf);
        }
    }

    FREEUP(buf);
    FREEUP(validBuf);
}

/********************************************************************************
 * parseRLResources
 * Description：
 *     Parse RESOURCE config value in bracket list format [resName, value],
 *     supports percentage suffix. Store parsed results into RL_RESOURCE_T array
 *
 * Input:
 *     value [in]: RESOURCE config value string
 *     resources [out]: Pointer to resource array, dynamically allocated
 *     nResources [out]: Number of parsed resources
 *
 * Return:
 *     0: Parse succeeded
 *     -1: Parse failed, invalid format or memory allocation failure
 ********************************************************************************/
static int
parseRLResources(char *value, RL_RESOURCE_T **resources,
               int *nResources)
{
    static char fname[] = "parseRLResources";
    char buf[MAXLINELEN];
    char *sp;
    int nRes = 0;
    int resSize = 0;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    snprintf(buf, MAXLINELEN, "%s", value);
    sp = buf;

    while (*sp) {
        char *resName, *valStr, *end;
        char nameDelim, valDelim;
        int isPercent;
        float fval;
        char *numEnd;

        while (*sp && isspace(*sp))
            sp++;
        if (*sp == '\0')
            break;

        if (*sp != '[') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 10,
                    "%s: Expected '[' in RESOURCE value at <%s>"),
                      fname, sp);
            return -1;
        }
        sp++;

        resName = sp;
        while (*sp && *sp != ',' && *sp != ']')
            sp++;
        if (*sp == '\0') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 11,
                    "%s: Unterminated resource specification in RESOURCE"),
                      fname);
            return -1;
        }

        nameDelim = *sp;
        end = sp;
        while (end > resName && isspace(*(end - 1)))
            end--;
        *end = '\0';

        if (resName[0] == '\0') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 52,
                    "%s: Empty resource name in RESOURCE specification"),
                      fname);
            return -1;
        }

        if (nameDelim == ']') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 53,
                    "%s: Missing value for resource <%s> in RESOURCE; expected [res, value]"),
                      fname, resName);
            return -1;
        }

        sp++;

        while (*sp && isspace(*sp))
            sp++;

        valStr = sp;
        while (*sp && *sp != ']')
            sp++;

        valDelim = *sp;
        end = sp;
        while (end > valStr && isspace(*(end - 1)))
            end--;
        *end = '\0';

        if (valDelim != ']') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 54,
                    "%s: Missing ']' for resource <%s> in RESOURCE"),
                      fname, resName);
            return -1;
        }
        sp++;

        if (valStr[0] == '\0') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 55,
                    "%s: Empty value for resource <%s> in RESOURCE"),
                      fname, resName);
            return -1;
        }

        if (strchr(valStr, ',') != NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 56,
                    "%s: Invalid value for resource <%s> in RESOURCE; extra ',' found in value"),
                      fname, resName);
            return -1;
        }

        isPercent = FALSE;
        if (end > valStr && *(end - 1) == '%') {
            isPercent = TRUE;
            *(end - 1) = '\0';
        }

        fval = strtof(valStr, &numEnd);
        if (numEnd == valStr || *numEnd != '\0') {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 57,
                    "%s: Invalid numeric value for resource <%s> in RESOURCES; value must be a number or number%%"),
                      fname, resName);
            return -1;
        }

        if (fval < 0) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 58,
                    "%s: Invalid value for resource <%s> in RESOURCES; value must be >= 0"),
                      fname, resName);
            return -1;
        }

        if (nRes >= resSize) {
            resSize = resSize ? resSize * 2 : 4;
            *resources = realloc(*resources,
                                 resSize * sizeof(RL_RESOURCE_T));
            if (*resources == NULL) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: realloc failed"),
                          fname);
                return -1;
            }
        }

        (*resources)[nRes].resName = safeSave(resName);
        (*resources)[nRes].value = fval;
        (*resources)[nRes].isPercent = isPercent;
        (*resources)[nRes].resNo = -1;
        if (strcasecmp(resName, RL_BUILDIN_RESOURCE_JOBS_NAME) == 0) {
            (*resources)[nRes].type = RL_RESOURCE_TYPE_JOBS;
        } else if (strcasecmp(resName, RL_BUILDIN_RESOURCE_SLOTS_NAME) == 0) {
            (*resources)[nRes].type = RL_RESOURCE_TYPE_SLOTS;
        } else if (strcasecmp(resName, RL_BUILDIN_RESOURCE_MEM_NAME) == 0) {
            (*resources)[nRes].type = RL_RESOURCE_TYPE_MEM;
            (*resources)[nRes].resNo = getResEntry("mem");
        } else if (strcasecmp(resName, RL_BUILDIN_RESOURCE_SWP_NAME) == 0) {
            (*resources)[nRes].type = RL_RESOURCE_TYPE_SWP;
            (*resources)[nRes].resNo = getResEntry("swp");
        } else if (strcasecmp(resName, RL_BUILDIN_RESOURCE_TMP_NAME) == 0) {
            (*resources)[nRes].type = RL_RESOURCE_TYPE_TMP;
            (*resources)[nRes].resNo = getResEntry("tmp");
        } else if (strcasecmp(resName, RL_BUILDIN_RSC_SLOTS_PER_PROCESSOR_NAME) == 0) {
            (*resources)[nRes].type = RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR;
        } else {
            /* Custom resource: verify it exists in the system */
            int resIdx = getResEntry(resName);
            if (resIdx < 0) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 59,
                        "%s: Resource <%s> in RESOURCE is not defined in the system; ignoring this limit"),
                          fname, resName);
                return -1;
            }
            (*resources)[nRes].type = RL_RESOURCE_TYPE_RSRC;
            (*resources)[nRes].resNo = resIdx;
        }

        /* For non-percent MEM/SWP/TMP, convert from configured unit (set by
         * LSF_UNIT_FOR_LIMITS in lsf.conf) to KB for internal use. */
        if (!isPercent
            && ((*resources)[nRes].type == RL_RESOURCE_TYPE_MEM
                || (*resources)[nRes].type == RL_RESOURCE_TYPE_SWP
                || (*resources)[nRes].type == RL_RESOURCE_TYPE_TMP)) {
            (*resources)[nRes].value = convertUnitToMB(fval);
        }

        nRes++;
    }

    *nResources = nRes;
    return 0;
}

/********************************************************************************
 * addSimpleResource
 * Description：
 *     Add a simple resource (e.g. JOBS, SLOTS, MEM) to the resource array,
 *     supports percentage suffix, auto-expands array when capacity is reached
 *
 * Input:
 *     resName [in]: Resource name
 *     value [in]: Resource value string, may have % suffix
 *     resources [in/out]: Pointer to resource array, may be reallocated
 *     nResources [in/out]: Current resource count pointer
 *     resSize [in/out]: Current array capacity pointer
 *
 * Return:
 *     0: Add succeeded
 *     -1: Add failed, invalid value or memory allocation failure
 ********************************************************************************/
static int
addSimpleResource(char *resName, char *value,
                  RL_RESOURCE_T **resources,
                  int *nResources, int *resSize,
                  int type)
{
    static char fname[] = "addSimpleResource";
    char buf[MAXLINELEN];
    char *pct;
    int isPercent;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    snprintf(buf, MAXLINELEN, "%s", value);
    isPercent = FALSE;
    pct = strchr(buf, '%');
    if (pct) {
        isPercent = TRUE;
        *pct = '\0';
    }

    if (*nResources >= *resSize) {
        *resSize = *resSize ? *resSize * 2 : 4;
        *resources = realloc(*resources,
                             *resSize * sizeof(RL_RESOURCE_T));
        if (*resources == NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 13,
                    "%s: realloc failed"),
                      fname);
            return -1;
        }
    }

    (*resources)[*nResources].resName = safeSave(resName);
    switch (type) {
    case RL_RESOURCE_TYPE_MEM:
        (*resources)[*nResources].resNo = getResEntry("mem");
        break;
    case RL_RESOURCE_TYPE_SWP:
        (*resources)[*nResources].resNo = getResEntry("swp");
        break;
    case RL_RESOURCE_TYPE_TMP:
        (*resources)[*nResources].resNo = getResEntry("tmp");
        break;
    case RL_RESOURCE_TYPE_RSRC:
        (*resources)[*nResources].resNo = getResEntry(resName);
        break;
    default:
        (*resources)[*nResources].resNo = -1;
    }
    (*resources)[*nResources].value = (float)atof(buf);
    (*resources)[*nResources].isPercent = isPercent;
    (*resources)[*nResources].type = type;

    /* For non-percent MEM/SWP/TMP, convert from configured unit (set by
     * LSF_UNIT_FOR_LIMITS in lsf.conf) to KB for internal use. */
    if (!isPercent
        && (type == RL_RESOURCE_TYPE_MEM
            || type == RL_RESOURCE_TYPE_SWP
            || type == RL_RESOURCE_TYPE_TMP)) {
        (*resources)[*nResources].value = convertUnitToMB((*resources)[*nResources].value);
    }

    (*nResources)++;

    return 0;
}

/********************************************************************************
 * setRLLimitLevel
 * Description:
 *     Set the level of a limit based on which consumers are configured.
 *     The level is determined by the most specific consumer configured:
 *     - HOST configured    -> RL_PEND_LEVEL_HOST
 *     - QUEUE configured   -> RL_PEND_LEVEL_QUEUE
 *     - USER configured    -> RL_PEND_LEVEL_USER
 *     - PROJECT configured -> RL_PEND_LEVEL_PROJECT
 *     - None configured    -> RL_PEND_LEVEL_CLUSTER
 *
 * Input:
 *     limit [in/out]: Pointer to the RL_LIMIT_T struct to set level
 *
 * Return:
 *     None
 ********************************************************************************/
static void
setRLLimitLevel(RL_LIMIT_T *limit)
{
    if (limit->consumers[RL_CONSUMER_POSITION_HOST].value != NULL) {
        limit->level = RL_PEND_LEVEL_HOST;
    } else if (limit->consumers[RL_CONSUMER_POSITION_QUEUE].value != NULL) {
        limit->level = RL_PEND_LEVEL_QUEUE;
    } else if (limit->consumers[RL_CONSUMER_POSITION_USER].value != NULL) {
        limit->level = RL_PEND_LEVEL_USER;
    } else if (limit->consumers[RL_CONSUMER_POSITION_PROJECT].value != NULL) {
        limit->level = RL_PEND_LEVEL_PROJECT;
    } else {
        limit->level = RL_PEND_LEVEL_CLUSTER;
    }
}

/********************************************************************************
 * addLimitToConf
 * Description：
 *     Add a parsed limit entry to the global config generalRLConf limits array,
 *     array space is auto-expanded
 *
 * Input:
 *     limit [in]: Pointer to the limit entry to add
 *
 * Return:
 *     0: Add succeeded
 *     -1: Add failed, memory allocation failure
 ********************************************************************************/
static int
addLimitToConf(RL_LIMIT_T *limit)
{
    static char fname[] = "addLimitToConf";
    int newSize;

    newSize = generalRLConf.nLimits + 1;
    generalRLConf.limits = realloc(generalRLConf.limits,
                                   newSize * sizeof(RL_LIMIT_T));
    if (generalRLConf.limits == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 14,
                "%s: realloc failed"),
                  fname);
        return -1;
    }

    generalRLConf.limits[generalRLConf.nLimits] = *limit;
    generalRLConf.nLimits = newSize;

    return 0;
}

/********************************************************************************
 * do_Limit
 * Description：
 *     Parse the Limit section in config file, auto-detect Volclava format and
 *     LSF format, validate keywords, consumers and resource config legality,
 *     build RL_LIMIT_T struct and add to global config
 *
 * Input:
 *     conf [in]: Config file handle
 *     fname [in]: Config file name
 *     lineNum [in/out]: Current line number pointer
 *
 * Return:
 *     Non-NULL: Parse succeeded, pointer to empty string
 *     NULL: Parse failed or section skipped
 ********************************************************************************/
static char *
do_Limit(struct lsConf *conf, char *fname, int *lineNum)
{
    static char pname[] = "do_Limit";
    struct keymap keyList[RL_KEY_SIZE + 1];
    char *linep;
    int hasUnknownKey = FALSE;
    int limitFormat = RL_FORMAT_UNDEFINED;
    RL_LIMIT_T limit;
    int i;
    int hasResource = FALSE;
    int needPerHost = FALSE;
    struct keymap *resEntries = NULL;
    int nResEntries = 0;

    initRLKeyList(keyList);

    linep = getNextLineC_conf(conf, lineNum, TRUE);
    if (linep == NULL) {
        ls_syslog(LOG_ERR, I18N_PREMATURE_EOF,
                  pname, fname, *lineNum, "Limit");
        return NULL;
    }

    if (isSectionEnd(linep, fname, lineNum, "Limit")) {
        ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 15,
                "%s: %s(%d): Empty Limit section, ignoring"),
                  pname, fname, *lineNum);
        lsb_CheckError = WARNING_ERR;
        return NULL;
    }

    if (strchr(linep, '=') == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 16,
                "%s: %s(%d): Invalid line format in Limit section, skipping"),
                  pname, fname, *lineNum);
        lsb_CheckError = WARNING_ERR;
        doSkipSection_conf(conf, lineNum, fname, "Limit");
        return NULL;
    }

    if (readRLHvalues(keyList, linep, conf, fname, lineNum,
                      "Limit", &hasUnknownKey,
                      &resEntries, &nResEntries) < 0) {
        for (i = 0; i < nResEntries; i++) {
            FREEUP(resEntries[i].key);
            FREEUP(resEntries[i].val);
        }
        FREEUP(resEntries);
        nResEntries = 0;
        freekeyval(keyList);
        return NULL;
    }

    memset(&limit, 0, sizeof(limit));

    if (keyList[RL_KEY_NAME].val == NULL
        || keyList[RL_KEY_NAME].val[0] == '\0') {
        ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 19,
                "%s: %s(%d): NAME is required in Limit section, skipping this limit"),
                  pname, fname, *lineNum);
        lsb_CheckError = WARNING_ERR;
        goto cleanup;
    }

    if (!isValidName(keyList[RL_KEY_NAME].val)) {
        ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 20,
                "%s: %s(%d): Invalid NAME <%s>; must contain only letters, digits, underscore or hyphen and be at most %d characters, skipping this limit"),
                  pname, fname, *lineNum, keyList[RL_KEY_NAME].val, MAX_LSB_NAME_LEN);
        lsb_CheckError = WARNING_ERR;
        goto cleanup;
    }

    if (isDuplicateName(keyList[RL_KEY_NAME].val)) {
        ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 21,
                "%s: %s(%d): Duplicate limit NAME <%s>, skipping this limit"),
                  pname, fname, *lineNum, keyList[RL_KEY_NAME].val);
        lsb_CheckError = WARNING_ERR;
        goto cleanup;
    }

    limit.name = safeSave(keyList[RL_KEY_NAME].val);

    if (keyList[RL_KEY_HOSTS].val) {
        if (strncasecmp(keyList[RL_KEY_HOSTS].val, "PER(", 4) == 0
            || strncasecmp(keyList[RL_KEY_HOSTS].val, "SHARED(", 7) == 0) {
            limitFormat = RL_FORMAT_VOLCLAVA;
        } else {
            limitFormat = RL_FORMAT_LSF;
        }
    }

    
    if (keyList[RL_KEY_QUEUES].val
        && limitFormat != RL_FORMAT_VOLCLAVA) {
        if (strncasecmp(keyList[RL_KEY_QUEUES].val, "PER(", 4) == 0
            || strncasecmp(keyList[RL_KEY_QUEUES].val, "SHARED(", 7) == 0) {
            limitFormat = RL_FORMAT_VOLCLAVA;    
        } else {
            limitFormat = RL_FORMAT_LSF; 
        }
    }

    if (keyList[RL_KEY_USERS].val
        && limitFormat != RL_FORMAT_VOLCLAVA) {
        if (strncasecmp(keyList[RL_KEY_USERS].val, "PER(", 4) == 0
            || strncasecmp(keyList[RL_KEY_USERS].val, "SHARED(", 7) == 0) {
            limitFormat = RL_FORMAT_VOLCLAVA;
        } else {
            limitFormat = RL_FORMAT_LSF;
        }
    }

    if (keyList[RL_KEY_PROJECTS].val
        && limitFormat != RL_FORMAT_VOLCLAVA) {
        if (strncasecmp(keyList[RL_KEY_PROJECTS].val, "PER(", 4) == 0
            || strncasecmp(keyList[RL_KEY_PROJECTS].val, "SHARED(", 7) == 0) {
            limitFormat = RL_FORMAT_VOLCLAVA;
        } else {
            limitFormat = RL_FORMAT_LSF;
        }
    }

    if ((limitFormat == RL_FORMAT_UNDEFINED)
        && (keyList[RL_KEY_PER_HOST].val
        || keyList[RL_KEY_PER_QUEUE].val
        || keyList[RL_KEY_PER_USER].val
        || keyList[RL_KEY_PER_PROJECT].val
        || keyList[RL_KEY_JOBS].val
        || keyList[RL_KEY_SLOTS].val
        || keyList[RL_KEY_MEM].val
        || keyList[RL_KEY_SWP].val
        || keyList[RL_KEY_TMP].val
        || keyList[RL_KEY_SLOTS_PER_PROCESSOR].val )) {
        limitFormat = RL_FORMAT_LSF;
    }

    if (limitFormat == RL_FORMAT_UNDEFINED) {
        limitFormat = RL_FORMAT_VOLCLAVA;
    }

    if (limitFormat == RL_FORMAT_VOLCLAVA) {
        if (hasUnknownKey || nResEntries > 0) {
            for (i = 0; i < nResEntries; i++) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 7,
                        "%s: %s(%d): unknown keyword %s in section Limit, ignoring the line"),
                          pname, fname, *lineNum, resEntries[i].key, "Limit");
            }
            if (hasUnknownKey) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 17,
                        "%s: %s(%d): limit <%s>: unknown keyword in Volclava format; skipping this limit"),
                          pname, fname, *lineNum, limit.name);
            }
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_RESOURCE].val) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 62,
                    "%s: %s(%d): limit <%s>: RESOURCE not allowed in Volclava format; use RESOURCES instead; skipping this limit"),
                      pname, fname, *lineNum, limit.name);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_PER_HOST].val
            || keyList[RL_KEY_PER_QUEUE].val
            || keyList[RL_KEY_PER_USER].val
            || keyList[RL_KEY_PER_PROJECT].val) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 42,
                    "%s: %s(%d): limit <%s>: PER_HOST/PER_QUEUE/PER_USER/PER_PROJECT not allowed in Volclava format; use HOSTS=PER(...) instead; skipping this limit"),
                      pname, fname, *lineNum, limit.name);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_JOBS].val
            || keyList[RL_KEY_SLOTS].val
            || keyList[RL_KEY_MEM].val
            || keyList[RL_KEY_SWP].val
            || keyList[RL_KEY_TMP].val
            || keyList[RL_KEY_SLOTS_PER_PROCESSOR].val) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 43,
                    "%s: %s(%d): limit <%s>: JOBS/SLOTS/MEM/SWP/TMP/SLOTS_PER_PROCESSOR not allowed as separate fields in Volclava format; use RESOURCES=[...] instead; skipping this limit"),
                      pname, fname, *lineNum, limit.name);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_HOSTS].val
            && strncasecmp(keyList[RL_KEY_HOSTS].val, "PER(", 4) != 0
            && strncasecmp(keyList[RL_KEY_HOSTS].val, "SHARED(", 7) != 0) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 44,
                    "%s: %s(%d): limit <%s>: HOSTS value <%s> invalid, please specify host/host group list with PER(...) or SHARED(...); skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_HOSTS].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_QUEUES].val
            && strncasecmp(keyList[RL_KEY_QUEUES].val, "PER(", 4) != 0
            && strncasecmp(keyList[RL_KEY_QUEUES].val, "SHARED(", 7) != 0) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 45,
                    "%s: %s(%d): limit <%s>: QUEUES value <%s> invalid, please specify queue list with PER(...) or SHARED(...); skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_QUEUES].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_USERS].val
            && strncasecmp(keyList[RL_KEY_USERS].val, "PER(", 4) != 0
            && strncasecmp(keyList[RL_KEY_USERS].val, "SHARED(", 7) != 0) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 46,
                    "%s: %s(%d): limit <%s>: USERS value <%s> invalid, please specify user/user group list with PER(...) or SHARED(...); skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_USERS].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_PROJECTS].val
            && strncasecmp(keyList[RL_KEY_PROJECTS].val, "PER(", 4) != 0
            && strncasecmp(keyList[RL_KEY_PROJECTS].val, "SHARED(", 7) != 0) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 47,
                    "%s: %s(%d): limit <%s>: PROJECTS value <%s> invalid, please specify project list with PER(...) or SHARED(...); skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_PROJECTS].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }
    }

    if (limitFormat == RL_FORMAT_LSF) {
        if (hasUnknownKey && nResEntries <= 0) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 18,
                    "%s: %s(%d): limit <%s>: unknown keyword in LSF format Limit section; ignoring unknown keyword"),
                      pname, fname, *lineNum, limit.name);
            lsb_CheckError = WARNING_ERR;
        }

        if (keyList[RL_KEY_RESOURCES].val) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 63,
                    "%s: %s(%d): limit <%s>: RESOURCES not allowed in LSF format; use RESOURCE instead; skipping this limit"),
                      pname, fname, *lineNum, limit.name);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_PER_HOST].val
            && (strncasecmp(keyList[RL_KEY_PER_HOST].val, "PER(", 4) == 0
                || strncasecmp(keyList[RL_KEY_PER_HOST].val, "SHARED(", 7) == 0)) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 48,
                    "%s: %s(%d): limit <%s>: PER_HOST value <%s> invalid in LSF limit format; use plain host/hostgroup list; skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_PER_HOST].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_PER_QUEUE].val
            && (strncasecmp(keyList[RL_KEY_PER_QUEUE].val, "PER(", 4) == 0
                || strncasecmp(keyList[RL_KEY_PER_QUEUE].val, "SHARED(", 7) == 0)) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 49,
                    "%s: %s(%d): limit <%s>: PER_QUEUE value <%s> invalid in LSF limit format; use plain queue list; skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_PER_QUEUE].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_PER_USER].val
            && (strncasecmp(keyList[RL_KEY_PER_USER].val, "PER(", 4) == 0
                || strncasecmp(keyList[RL_KEY_PER_USER].val, "SHARED(", 7) == 0)) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 50,
                    "%s: %s(%d): limit <%s>: PER_USER value <%s> invalid in LSF limit format; use plain user/usergroup list; skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_PER_USER].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }

        if (keyList[RL_KEY_PER_PROJECT].val
            && (strncasecmp(keyList[RL_KEY_PER_PROJECT].val, "PER(", 4) == 0
                || strncasecmp(keyList[RL_KEY_PER_PROJECT].val, "SHARED(", 7) == 0)) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 51,
                    "%s: %s(%d): limit <%s>: PER_PROJECT value <%s> invalid in LSF limit format; use plain project list; skipping this limit"),
                      pname, fname, *lineNum, limit.name, keyList[RL_KEY_PER_PROJECT].val);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }
    }

    limit.consumers = (RL_CONSUMER_T *)my_calloc(RL_CONSUMER_POSITION_MAX,
                             sizeof(RL_CONSUMER_T), pname);
    limit.nConsumers = RL_CONSUMER_POSITION_MAX;

    if (limitFormat == RL_FORMAT_LSF) {
        if (keyList[RL_KEY_HOSTS].val
            && keyList[RL_KEY_PER_HOST].val) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 23,
                    "%s: %s(%d): limit <%s>: PER_HOST cannot be defined at the same time as HOSTS; ignored"),
                      pname, fname, *lineNum, limit.name);
            FREEUP(keyList[RL_KEY_PER_HOST].val);
            lsb_CheckError = WARNING_ERR;
        }

        if (keyList[RL_KEY_QUEUES].val
            && keyList[RL_KEY_PER_QUEUE].val) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 24,
                    "%s: %s(%d): limit <%s>: PER_QUEUE cannot be defined at the same time as QUEUES; ignored"),
                      pname, fname, *lineNum, limit.name);
            FREEUP(keyList[RL_KEY_PER_QUEUE].val);
            lsb_CheckError = WARNING_ERR;
        }

        if (keyList[RL_KEY_USERS].val
            && keyList[RL_KEY_PER_USER].val) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 25,
                    "%s: %s(%d): limit <%s>: PER_USER cannot be defined at the same time as USERS; ignored"),
                      pname, fname, *lineNum, limit.name);
            FREEUP(keyList[RL_KEY_PER_USER].val);
            lsb_CheckError = WARNING_ERR;
        }

        if (keyList[RL_KEY_PROJECTS].val
            && keyList[RL_KEY_PER_PROJECT].val) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 26,
                    "%s: %s(%d): limit <%s>: PER_PROJECT cannot be defined at the same time as PROJECS; ignored"),
                      pname, fname, *lineNum, limit.name);
            FREEUP(keyList[RL_KEY_PER_PROJECT].val);
            lsb_CheckError = WARNING_ERR;
        }

        if (keyList[RL_KEY_PER_HOST].val) {
            limit.consumers[RL_CONSUMER_POSITION_HOST].type
                = RL_CONSUMER_TYPE_HOST;
            limit.consumers[RL_CONSUMER_POSITION_HOST].mode
                = RL_CONSUMER_MODE_PER;
            limit.consumers[RL_CONSUMER_POSITION_HOST].value
                = safeSave(keyList[RL_KEY_PER_HOST].val);
        } else if (keyList[RL_KEY_HOSTS].val) {
            limit.consumers[RL_CONSUMER_POSITION_HOST].type
                = RL_CONSUMER_TYPE_HOST;
            limit.consumers[RL_CONSUMER_POSITION_HOST].mode
                = RL_CONSUMER_MODE_SHARED;
            limit.consumers[RL_CONSUMER_POSITION_HOST].value
                = safeSave(keyList[RL_KEY_HOSTS].val);
        }

        if (keyList[RL_KEY_PER_QUEUE].val) {
            limit.consumers[RL_CONSUMER_POSITION_QUEUE].type
                = RL_CONSUMER_TYPE_QUEUE;
            limit.consumers[RL_CONSUMER_POSITION_QUEUE].mode
                = RL_CONSUMER_MODE_PER;
            limit.consumers[RL_CONSUMER_POSITION_QUEUE].value
                = safeSave(keyList[RL_KEY_PER_QUEUE].val);
        } else if (keyList[RL_KEY_QUEUES].val) {
            limit.consumers[RL_CONSUMER_POSITION_QUEUE].type
                = RL_CONSUMER_TYPE_QUEUE;
            limit.consumers[RL_CONSUMER_POSITION_QUEUE].mode
                = RL_CONSUMER_MODE_SHARED;
            limit.consumers[RL_CONSUMER_POSITION_QUEUE].value
                = safeSave(keyList[RL_KEY_QUEUES].val);
        }

        if (keyList[RL_KEY_PER_USER].val) {
            limit.consumers[RL_CONSUMER_POSITION_USER].type
                = RL_CONSUMER_TYPE_USER;
            limit.consumers[RL_CONSUMER_POSITION_USER].mode
                = RL_CONSUMER_MODE_PER;
            limit.consumers[RL_CONSUMER_POSITION_USER].value
                = safeSave(keyList[RL_KEY_PER_USER].val);
        } else if (keyList[RL_KEY_USERS].val) {
            limit.consumers[RL_CONSUMER_POSITION_USER].type
                = RL_CONSUMER_TYPE_USER;
            limit.consumers[RL_CONSUMER_POSITION_USER].mode
                = RL_CONSUMER_MODE_SHARED;
            limit.consumers[RL_CONSUMER_POSITION_USER].value
                = safeSave(keyList[RL_KEY_USERS].val);
        }

        if (keyList[RL_KEY_PER_PROJECT].val) {
            limit.consumers[RL_CONSUMER_POSITION_PROJECT].type
                = RL_CONSUMER_TYPE_PROJECT;
            limit.consumers[RL_CONSUMER_POSITION_PROJECT].mode
                = RL_CONSUMER_MODE_PER;
            limit.consumers[RL_CONSUMER_POSITION_PROJECT].value
                = safeSave(keyList[RL_KEY_PER_PROJECT].val);
        } else if (keyList[RL_KEY_PROJECTS].val) {
            limit.consumers[RL_CONSUMER_POSITION_PROJECT].type
                = RL_CONSUMER_TYPE_PROJECT;
            limit.consumers[RL_CONSUMER_POSITION_PROJECT].mode
                = RL_CONSUMER_MODE_SHARED;
            limit.consumers[RL_CONSUMER_POSITION_PROJECT].value
                = safeSave(keyList[RL_KEY_PROJECTS].val);
        }
    } else {
        if (keyList[RL_KEY_HOSTS].val) {
            if (parseRLConsumer(keyList[RL_KEY_HOSTS].val,
                                     RL_CONSUMER_TYPE_HOST,
                                     &limit.consumers[RL_CONSUMER_POSITION_HOST]) < 0) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 27,
                        "%s: %s(%d): limit <%s>: invalid HOSTS value <%s>; use PER(...) or SHARED(...); skipping this limit"),
                          pname, fname, *lineNum, limit.name, keyList[RL_KEY_HOSTS].val);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
        }

        if (keyList[RL_KEY_QUEUES].val) {
            if (parseRLConsumer(keyList[RL_KEY_QUEUES].val,
                                     RL_CONSUMER_TYPE_QUEUE,
                                     &limit.consumers[RL_CONSUMER_POSITION_QUEUE]) < 0) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 28,
                        "%s: %s(%d): limit <%s>: invalid QUEUES value <%s>; use PER(...) or SHARED(...); skipping this limit"),
                          pname, fname, *lineNum, limit.name, keyList[RL_KEY_QUEUES].val);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
        }

        if (keyList[RL_KEY_USERS].val) {
            if (parseRLConsumer(keyList[RL_KEY_USERS].val,
                                     RL_CONSUMER_TYPE_USER,
                                     &limit.consumers[RL_CONSUMER_POSITION_USER]) < 0) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 29,
                        "%s: %s(%d): limit <%s>: invalid USERS value <%s>; use PER(...) or SHARED(...); skipping this limit"),
                          pname, fname, *lineNum, limit.name, keyList[RL_KEY_USERS].val);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
        }

        if (keyList[RL_KEY_PROJECTS].val) {
            if (parseRLConsumer(keyList[RL_KEY_PROJECTS].val,
                                     RL_CONSUMER_TYPE_PROJECT,
                                     &limit.consumers[RL_CONSUMER_POSITION_PROJECT]) < 0) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 30,
                        "%s: %s(%d): limit <%s>: invalid PROJECTS value <%s>; use PER(...) or SHARED(...); skipping this limit"),
                          pname, fname, *lineNum, limit.name, keyList[RL_KEY_PROJECTS].val);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
        }
    }

    for (i = 0; i < RL_CONSUMER_POSITION_MAX; i++) {
        if (limit.consumers[i].value != NULL) {
            validateRLConsumerValue(&limit.consumers[i].value,
                                    limit.consumers[i].type,
                                    limit.name,
                                    fname, *lineNum);
        }
    }

    {
        struct keymap *rsrcKey = (limitFormat == RL_FORMAT_LSF)
                                 ? &keyList[RL_KEY_RESOURCE]
                                 : &keyList[RL_KEY_RESOURCES];

        if (rsrcKey->val) {
            if (parseRLResources(rsrcKey->val,
                               &limit.resources,
                               &limit.nResources) < 0) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 32,
                        "%s: %s(%d): limit <%s>: invalid %s value <%s>; skipping this limit"),
                          pname, fname, *lineNum, limit.name, rsrcKey->key, rsrcKey->val);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
            if (limit.nResources > 0) {
                hasResource = TRUE;
            }
        }
    }

    if (limitFormat == RL_FORMAT_LSF) {
        int resSize = limit.nResources;

        if (keyList[RL_KEY_JOBS].val) {
            if (addSimpleResource("JOBS", keyList[RL_KEY_JOBS].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_JOBS) == 0) {
                hasResource = TRUE;
            }
        }

        if (keyList[RL_KEY_SLOTS].val) {
            if (addSimpleResource("SLOTS", keyList[RL_KEY_SLOTS].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_SLOTS) == 0) {
                hasResource = TRUE;
            }
        }

        if (keyList[RL_KEY_MEM].val) {
            if (addSimpleResource("MEM", keyList[RL_KEY_MEM].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_MEM) == 0) {
                hasResource = TRUE;
            }
        }

        if (keyList[RL_KEY_SWP].val) {
            if (addSimpleResource("SWP", keyList[RL_KEY_SWP].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_SWP) == 0) {
                hasResource = TRUE;
            }
        }

        if (keyList[RL_KEY_TMP].val) {
            if (addSimpleResource("TMP", keyList[RL_KEY_TMP].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_TMP) == 0) {
                hasResource = TRUE;
            }
        }

        if (keyList[RL_KEY_SLOTS_PER_PROCESSOR].val) {
            if (addSimpleResource("SLOTS_PER_PROCESSOR",
                                  keyList[RL_KEY_SLOTS_PER_PROCESSOR].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR) == 0) {
                hasResource = TRUE;
            }
        }

        for (i = 0; i < nResEntries; i++) {
            if (addSimpleResource(resEntries[i].key, resEntries[i].val,
                                  &limit.resources,
                                  &limit.nResources,
                                  &resSize,
                                  RL_RESOURCE_TYPE_RSRC) == 0) {
                hasResource = TRUE;
            }
            FREEUP(resEntries[i].key);
            FREEUP(resEntries[i].val);
        }
        FREEUP(resEntries);
        nResEntries = 0;
    }

    if (!hasResource) {
        ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 33,
                "%s: %s(%d): limit <%s>: no resource configured; skipping this limit"),
                  pname, fname, *lineNum, limit.name);
        lsb_CheckError = WARNING_ERR;
        goto cleanup;
    }

    /* SLOTS and SLOTS_PER_PROCESSOR are mutually exclusive */
    {
        int hasSlots = FALSE, hasSpp = FALSE;
        for (i = 0; i < limit.nResources; i++) {
            if (limit.resources[i].type == RL_RESOURCE_TYPE_SLOTS) {
                hasSlots = TRUE;
            } else if (limit.resources[i].type == RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR) {
                hasSpp = TRUE;
            }
        }
        if (hasSlots && hasSpp) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 60,
                    "%s: %s(%d): limit <%s>: SLOTS cannot be defined at the same time as SLOTS_PER_PROCESSOR: ignored"),
                      pname, fname, *lineNum, limit.name);
            lsb_CheckError = WARNING_ERR;
            goto cleanup;
        }
    }

    for (i = 0; i < limit.nResources; i++) {
        if (limit.resources[i].isPercent) {
            if (strcasecmp(limit.resources[i].resName, "MEM") == 0
                || strcasecmp(limit.resources[i].resName, "SWP") == 0
                || strcasecmp(limit.resources[i].resName, "TMP") == 0) {
                needPerHost = TRUE;
            }
        }
        if (strcasecmp(limit.resources[i].resName,
                       "SLOTS_PER_PROCESSOR") == 0) {
            needPerHost = TRUE;
        }
    }

    if (needPerHost) {
        if (limitFormat == RL_FORMAT_LSF) {
            if (keyList[RL_KEY_PER_HOST].val == NULL) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 34,
                        "%s: %s(%d): limit <%s>: PER_HOST required when MEM/SWP/TMP is percentage or SLOTS_PER_PROCESSOR is used; skipping this limit"),
                          pname, fname, *lineNum, limit.name);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
        } else {
            if (limit.consumers[RL_CONSUMER_POSITION_HOST].mode
                != RL_CONSUMER_MODE_PER) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 35,
                        "%s: %s(%d): limit <%s>: HOSTS=PER(...) required when MEM/SWP/TMP is percentage or SLOTS_PER_PROCESSOR is used; skipping this limit"),
                          pname, fname, *lineNum, limit.name);
                lsb_CheckError = WARNING_ERR;
                goto cleanup;
            }
        }
    }

    if (keyList[RL_KEY_DESCRIPTION].val) {
        if (strlen(keyList[RL_KEY_DESCRIPTION].val) > RL_DESC_MAX_LEN) {
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 36,
                    "%s: %s(%d): limit <%s>: DESCRIPTION exceeds %d characters; truncating"),
                      pname, fname, *lineNum, limit.name, RL_DESC_MAX_LEN);
            lsb_CheckError = WARNING_ERR;
            keyList[RL_KEY_DESCRIPTION].val[RL_DESC_MAX_LEN] = '\0';
        }
        limit.desc = safeSave(keyList[RL_KEY_DESCRIPTION].val);
    }

    setRLLimitLevel(&limit);

    if (addLimitToConf(&limit) < 0) {
        goto cleanup;
    }

    freekeyval(keyList);
    return "";

cleanup:
    freeRLLimit(&limit);
    for (i = 0; i < nResEntries; i++) {
        FREEUP(resEntries[i].key);
        FREEUP(resEntries[i].val);
    }
    FREEUP(resEntries);
    freekeyval(keyList);
    return NULL;
}

/********************************************************************************
 * dumpResLimitConf
 * Description：
 *     Debug dump of global resource limit config generalRLConf, including each
 *     limit's name, consumers and resources. Only outputs when LC_JLIMIT log
 *     class is enabled
 *
 * Input:
 *     None
 *
 * Return:
 *     None
 ********************************************************************************/
static void
dumpResLimitConf(void)
{
    static char fname[] = "dumpResLimitConf";
    int i, j;

    if (!(logclass & LC_JLIMIT)) {
        return;
    }

    if (generalRLConf.nLimits <= 0) {
        ls_syslog(LOG_DEBUG2, "\
%s: ------ no resource limits defined (nLimits=%d) ------",
              fname, generalRLConf.nLimits);
        return;
    }

    ls_syslog(LOG_DEBUG2, "\
%s: ------ Begin Dump generalRLConf (nLimits=%d) ------",
              fname, generalRLConf.nLimits);

    for (i = 0; i < generalRLConf.nLimits; i++) {
        RL_LIMIT_T *limit = &generalRLConf.limits[i];

        ls_syslog(LOG_DEBUG2, "\
%s: Limit[%d]: name=<%s> nConsumers=%d nResources=%d desc=<%s>",
                  fname, i,
                  limit->name ? limit->name : "(-)",
                  limit->nConsumers,
                  limit->nResources,
                  limit->desc ? limit->desc : "(-)");

        for (j = 0; j < limit->nConsumers; j++) {
            dumpRLConsumer(&limit->consumers[j], j);
        }

        for (j = 0; j < limit->nResources; j++) {
            dumpRLResource(&limit->resources[j]);
        }
    }

    ls_syslog(LOG_DEBUG2, "\
%s: ------ End Dump generalRLConf ------", fname);
}

/********************************************************************************
 * dumpRLConsumer
 * Description：
 *     Debug dump of a single consumer's info, including type
 *     (HOST/QUEUE/USER/PROJECT), mode (SHARED/PER) and value
 *
 * Input:
 *     consumer [in]: Consumer struct pointer
 *     position [in]: Index position of the consumer in the array
 *
 * Return:
 *     None
 ********************************************************************************/
static void
dumpRLConsumer(RL_CONSUMER_T *consumer, int position)
{
    static char fname[] = "dumpRLConsumer";
    const char *typeStr;
    const char *modeStr;

    if (consumer->value == NULL) {
        ls_syslog(LOG_DEBUG2, "\
%s:   consumer[%d]: (not configured)", fname, position);
        return;
    }

    switch (consumer->type) {
    case RL_CONSUMER_TYPE_HOST:
        typeStr = "HOST";
        break;
    case RL_CONSUMER_TYPE_QUEUE:
        typeStr = "QUEUE";
        break;
    case RL_CONSUMER_TYPE_USER:
        typeStr = "USER";
        break;
    case RL_CONSUMER_TYPE_PROJECT:
        typeStr = "PROJECT";
        break;
    default:
        typeStr = "UNKNOWN";
        break;
    }

    switch (consumer->mode) {
    case RL_CONSUMER_MODE_SHARED:
        modeStr = "SHARED";
        break;
    case RL_CONSUMER_MODE_PER:
        modeStr = "PER";
        break;
    default:
        modeStr = "UNKNOWN";
        break;
    }

    ls_syslog(LOG_DEBUG2, "\
%s:   consumer[%d]: type=%s mode=%s value=<%s>",
              fname, position, typeStr, modeStr, consumer->value);
}

/********************************************************************************
 * dumpRLResource
 * Description：
 *     Debug dump of a single resource's info, including resource name,
 *     numeric value and whether it is a percentage
 *
 * Input:
 *     resource [in]: Resource struct pointer
 *
 * Return:
 *     None
 ********************************************************************************/
static void
dumpRLResource(RL_RESOURCE_T *resource)
{
    static char fname[] = "dumpRLResource";

    ls_syslog(LOG_DEBUG2, "\
%s:   resource: name=<%s> value=%.2f isPercent=%s",
              fname,
              resource->resName ? resource->resName : "(null)",
              resource->value,
              resource->isPercent ? "TRUE" : "FALSE");
}

/********************************************************************************
 * isConsumerNameMatch
 * Description:
 *     Check if a single consumer name matches the job's value for the given
 *     consumer type. Supports direct name match and group membership check
 *     for HOST and USER types. Special keywords: "all" always matches,
 *     "default" matches the default group for HOST/USER.
 *
 * Input:
 *     name [in]: Consumer name from config (without ~ prefix)
 *     consumerType [in]: Consumer type (HOST/QUEUE/USER/PROJECT)
 *     jobValue [in]: Job's value in this consumer dimension
 *
 * Return:
 *     TRUE: Name matches the job's value
 *     FALSE: Name does not match
 ********************************************************************************/
static int
isConsumerNameMatch(const char *name, enum rl_consumer_type consumerType,
                    const char *jobValue)
{
    struct gData *gp;

    if (name == NULL || name[0] == '\0') {
        return FALSE;
    }

    if (strcmp(name, "all") == 0) {
        return TRUE;
    }

    switch (consumerType) {
    case RL_CONSUMER_TYPE_HOST:
        if (strcasecmp(name, jobValue) == 0) {
            return TRUE;
        }
        gp = getHGrpData((char *)name);
        if (gp != NULL && gMember((char *)jobValue, gp)) {
            return TRUE;
        }
        break;

    case RL_CONSUMER_TYPE_QUEUE:
        if (strcasecmp(name, jobValue) == 0) {
            return TRUE;
        }
        break;

    case RL_CONSUMER_TYPE_USER:
        if (strcasecmp(name, jobValue) == 0) {
            return TRUE;
        }
        gp = getUGrpData((char *)name);
        if (gp != NULL && gMember((char *)jobValue, gp)) {
            return TRUE;
        }
        break;

    case RL_CONSUMER_TYPE_PROJECT:
        if (strcasecmp(name, jobValue) == 0) {
            return TRUE;
        }
        break;
    }

    return FALSE;
}

/********************************************************************************
 * isRLConsumerMatch
 * Description:
 *     Check if a job matches a limit's consumer configuration for a given
 *     consumer type. Parses the space-delimited consumer value list, handles
 *     ~ (exclusion) prefix, "all" keyword, and group membership.
 *
 *     Match rules:
 *     - If consumer value is NULL (not configured), default match (TRUE)
 *     - If any ~name matches the job value, the job is excluded (FALSE)
 *     - If positive names exist and none matches the job value, no match (FALSE)
 *     - If positive names exist and at least one matches, match (TRUE)
 *     - If only exclusion names exist and job is not excluded, match (TRUE)
 *
 * Input:
 *     consumer [in]: Consumer configuration from limit
 *     jobValue [in]: Job's value in this consumer dimension
 *
 * Return:
 *     TRUE: Job matches this consumer configuration
 *     FALSE: Job does not match
 ********************************************************************************/
static int
isRLConsumerMatch(RL_CONSUMER_T *consumer, const char *jobValue)
{
    static char fname[] = "isRLConsumerMatch";
    static char *buf = NULL;
    static int bufSize = 0;
    char *sp, *word;
    int hasPositive = FALSE;
    int positiveMatch = FALSE;
    int excluded = FALSE;
    int neededSize;

    if (consumer == NULL || consumer->value == NULL
        || consumer->value[0] == '\0'
        || (strcasecmp(consumer->value, "all") == 0 && consumer->mode == RL_CONSUMER_MODE_SHARED)) {
        return TRUE;
    }

    if (jobValue == NULL) {
        return FALSE;
    }

    if (strcasecmp(consumer->value, "all") == 0 && consumer->mode == RL_CONSUMER_MODE_PER) {
        return TRUE;
    }

    neededSize = strlen(consumer->value) + 1;
    if (buf == NULL) {
        bufSize = (neededSize < 1024) ? 1024 : neededSize;
        buf = (char *)my_calloc(bufSize, sizeof(char), fname);
    } else if (bufSize <= neededSize) {
        bufSize = neededSize + 1;
        buf = (char *)my_realloc(buf, bufSize, fname);
    }

    memset(buf, 0, bufSize);
    strcpy(buf, consumer->value);
    sp = buf;

    while ((word = getNextWord_(&sp)) != NULL) {
        if (word[0] == '~') {
            char *exclName = word + 1;
            if (exclName[0] != '\0'
                && isConsumerNameMatch(exclName, consumer->type, jobValue)) {
                excluded = TRUE;
            }
        } else {
            hasPositive = TRUE;
            if (isConsumerNameMatch(word, consumer->type, jobValue)) {
                positiveMatch = TRUE;
            }
        }
    }

    if (excluded) {
        return FALSE;
    }

    if (hasPositive) {
        return positiveMatch;
    }

    return TRUE;
}

/********************************************************************************
 * getRLBitmap
 * Description:
 *     Generate a single-dimension bitmap for one consumer type (HOST/QUEUE/
 *     USER/PROJECT). Bit i=1 means limit i matches the given consumer value
 *     in this consumer dimension.
 *
 *     Caching strategy differs by consumer type:
 *     - HOST/QUEUE/USER individual value: the bitmap is cached in the
 *       struct's rlBitmap field (hData/qData/uData). Subsequent calls with
 *       the same struct return the cached bitmap directly.
 *     - HOST/QUEUE/USER NULL value: the bitmap is cached in the
 *       corresponding global default-bitmap variable (hDefRLBitmap/
 *       qDefRLBitmap/uDefRLBitmap). This represents "match all".
 *     - PROJECT: uses rlBitmapCache (hash table) with key format "value@pos".
 *
 * Input:
 *     obj [in]: Consumer value to match. Type depends on consumerPos:
 *               HOST    -> struct hData * (or NULL)
 *               QUEUE   -> struct qData * (or NULL)
 *               USER    -> struct uData * (or NULL)
 *               PROJECT -> const char * project name (or NULL)
 *     consumerPos [in]: Consumer position
 *                       (RL_CONSUMER_POSITION_HOST/QUEUE/USER/PROJECT)
 *
 * Return:
 *     int *: Bitmap owned by the cache/struct, do NOT free. NULL on error.
 ********************************************************************************/
static int *
getRLBitmap(void *obj, int consumerPos)
{
    static char fname[] = "getRLBitmap";
    int *cachedBitmap;
    int i;
    char cacheKey[MAXLINELEN];
    hEnt *cacheEnt;
    const char *value = NULL;
    int **structBitmapSlot = NULL;  /* points to struct rlBitmap field */
    int **defBitmapSlot = NULL;   /* points to global empty-bitmap var */

    if (generalRLConf.nLimits <= 0 || rlBitmapSize <= 0) {
        return NULL;
    }

    switch (consumerPos) {
    case RL_CONSUMER_POSITION_HOST: {
        struct hData *hp = (struct hData *)obj;
        if (hp != NULL) {
            if (hp->rlBitmap != NULL) {
                return hp->rlBitmap;
            }
            value = hp->host;
            structBitmapSlot = &hp->rlBitmap;
        }
        defBitmapSlot = &hDefRLBitmap;
        break;
    }
    case RL_CONSUMER_POSITION_QUEUE: {
        struct qData *qp = (struct qData *)obj;
        if (qp != NULL) {
            if (qp->rlBitmap != NULL) {
                return qp->rlBitmap;
            }
            value = qp->queue;
            structBitmapSlot = &qp->rlBitmap;
        }
        defBitmapSlot = &qDefRLBitmap;
        break;
    }
    case RL_CONSUMER_POSITION_USER: {
        struct uData *up = (struct uData *)obj;
        if (up != NULL) {
            if (up->rlBitmap != NULL) {
                return up->rlBitmap;
            }
            value = up->user;
            structBitmapSlot = &up->rlBitmap;
        }
        defBitmapSlot = &uDefRLBitmap;
        break;
    }
    case RL_CONSUMER_POSITION_PROJECT:
        value = (const char *)obj;
        break;
    default:
        return NULL;
    }

    /* Check the appropriate cache */
    if (consumerPos == RL_CONSUMER_POSITION_PROJECT) {
        snprintf(cacheKey, sizeof(cacheKey), "%s@%d",
                 (value != NULL) ? value : "null", consumerPos);
        cacheEnt = h_getEnt_(&rlBitmapCache, cacheKey);
        if (cacheEnt != NULL) {
            return (int *)cacheEnt->hData;
        }
    } else if (structBitmapSlot == NULL && defBitmapSlot != NULL) {
        /* HOST/QUEUE/USER with NULL obj: use the global empty bitmap */
        if (*defBitmapSlot != NULL) {
            return *defBitmapSlot;
        }
    }

    /* Cache miss: generate new bitmap */
    cachedBitmap = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);

    for (i = 0; i < generalRLConf.nLimits; i++) {
        RL_LIMIT_T *limit = &generalRLConf.limits[i];

        if (isRLConsumerMatch(&limit->consumers[consumerPos], value)) {
            SET_BIT(i, cachedBitmap);
        }
    }

    if (consumerPos == RL_CONSUMER_TYPE_HOST && value != NULL) {
        /* Individul HOST: clear bits for limits with default value "SHARED(all)" or not configured
         */
        for (i = 0; i < generalRLConf.nLimits; i++) {
            RL_LIMIT_T *limit = &generalRLConf.limits[i];
            RL_CONSUMER_T *host = &limit->consumers[RL_CONSUMER_POSITION_HOST];

            if (host->value == NULL || host->value[0] == '\0') {
                CLEAR_BIT(i, cachedBitmap);
            } else if (host->mode != RL_CONSUMER_MODE_SHARED) {
                continue;
            } else if (strcasecmp(host->value, "all") == 0) {
                CLEAR_BIT(i, cachedBitmap);
            }
        }
    }

    /* Store in the appropriate cache slot */
    if (structBitmapSlot != NULL) {
        *structBitmapSlot = cachedBitmap;
    } else if (defBitmapSlot != NULL) {
        *defBitmapSlot = cachedBitmap;
    } else {
        /* PROJECT: add to rlBitmapCache */
        cacheEnt = h_addEnt_(&rlBitmapCache, cacheKey, NULL);
        cacheEnt->hData = cachedBitmap;
    }

    return cachedBitmap;
}

/********************************************************************************
 * getRLBitmap4Query
 * Description:
 *     Generate a single-dimension bitmap for one consumer type (HOST/QUEUE/
 *     USER/PROJECT), for use by blimits query. Reports whether the returned
 *     bitmap is newly allocated (caller must free) or owned by an existing
 *     cache/struct (caller must NOT free).
 *
 *     Caching strategy differs by consumer type:
 *     - HOST: if the host exists and both hData->rlBitmap and hDefRLBitmap
 *       are populated, return a newly allocated bitmap = OR of the two
 *       (isFree=1). Otherwise fall back to fresh generation (isFree=1).
 *     - QUEUE/USER: if the queue/user exists and its struct rlBitmap is
 *       populated, return it directly (isFree=0). Otherwise generate fresh
 *       without caching (isFree=1).
 *     - PROJECT: check rlBitmapCache; on hit return cached (isFree=0), on
 *       miss generate fresh without caching (isFree=1).
 *
 *     Used by blimits query.
 *
 * Input:
 *     value [in]: The consumer value to match (e.g. hostname, queue name).
 *                 May be NULL.
 *     consumerPos [in]: Consumer position
 *                       (RL_CONSUMER_POSITION_HOST/QUEUE/USER/PROJECT)
 *     isFree [out]: 1 if caller must free the returned bitmap; 0 if it is
 *                   owned by an existing cache/struct.
 *
 * Return:
 *     int *: Bitmap of rlBitmapSize integers. NULL on error or invalid input.
 ********************************************************************************/
static int *
getRLBitmap4Query(const char *value, int consumerPos, int *isFree)
{
    static char fname[] = "getRLBitmap4Query";
    int *cachedBitmap = NULL;
    int i, j;
    char cacheKey[MAXLINELEN];
    hEnt *cacheEnt;

    if (generalRLConf.nLimits <= 0 || rlBitmapSize <= 0 || isFree == NULL) {
        return NULL;
    }

    *isFree = 0;

    switch (consumerPos) {
    case RL_CONSUMER_POSITION_HOST: {
        struct hData *hp = (value != NULL) ? getHostData((char *)value) : NULL;
        if (hp != NULL && hp->rlBitmap != NULL && hDefRLBitmap != NULL) {
            /* Both bitmaps available: OR into a new bitmap */
            cachedBitmap = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);
            for (j = 0; j < rlBitmapSize; j++) {
                cachedBitmap[j] = hp->rlBitmap[j] | hDefRLBitmap[j];
            }
            *isFree = 1;
            return cachedBitmap;
        }
        break;  /* fall back to fresh generation */
    }
    case RL_CONSUMER_POSITION_QUEUE: {
        struct qData *qp = (value != NULL) ? getQueueData((char *)value) : NULL;
        if (qp != NULL && qp->rlBitmap != NULL) {
            return qp->rlBitmap;  /* isFree stays 0 */
        }
        break;  /* fall back to fresh generation */
    }
    case RL_CONSUMER_POSITION_USER: {
        struct uData *up = (value != NULL) ? getUserData((char *)value) : NULL;
        if (up != NULL && up->rlBitmap != NULL) {
            return up->rlBitmap;  /* isFree stays 0 */
        }
        break;  /* fall back to fresh generation */
    }
    case RL_CONSUMER_POSITION_PROJECT:
        snprintf(cacheKey, sizeof(cacheKey), "%s@%d",
                 (value != NULL) ? value : "null", consumerPos);
        cacheEnt = h_getEnt_(&rlBitmapCache, cacheKey);
        if (cacheEnt != NULL) {
            return (int *)cacheEnt->hData;  /* isFree stays 0 */
        }
        break;  /* fall back to fresh generation */
    default:
        return NULL;
    }

    /* Fresh generation without caching */
    cachedBitmap = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);
    for (i = 0; i < generalRLConf.nLimits; i++) {
        RL_LIMIT_T *limit = &generalRLConf.limits[i];

        if (isRLConsumerMatch(&limit->consumers[consumerPos], value)) {
            SET_BIT(i, cachedBitmap);
        }
    }

    *isFree = 1;
    return cachedBitmap;
}

/********************************************************************************
 * getRLBitmap4Job
 * Description:
 *     Fill a fixed-size bitmap array recording whether a job matches each
 *     configured limit's consumer configuration. The array has
 *     RL_CONSUMER_POSITION_MAX rows, one per consumer type (HOST/QUEUE/USER/
 *     PROJECT). Each row is a bitmap with rlBitmapSize integers, where bit i
 *     indicates whether the job matches limit i in that consumer dimension.
 *
 *     Match rules per consumer dimension:
 *     A. If limit does not define a consumer type, default match (bit=1)
 *     B. If limit defines a consumer type, check if job's value is in the
 *        configured list; ~ prefix means exclusion
 *     C. For HOST and USER, also check hostgroup/usergroup membership
 *
 *     The individual row bitmaps are owned by getRLBitmap()'s cache (struct
 *     rlBitmap fields, global empty bitmaps, or rlBitmapCache) and are only
 *     borrowed by rlBitmaps; the caller must NOT free the rows.
 *
 * Input:
 *     jp [in]: Job data pointer
 *     hp [in]: Host data pointer for HOST consumer dimension. May be NULL.
 *     rlBitmaps [out]: Fixed-size array of RL_CONSUMER_POSITION_MAX int*
 *                      pointers, filled with cache-owned bitmap rows.
 *
 * Return:
 *     0 on success; -1 on error (NULL jp, no limits configured).
 ********************************************************************************/
static int
getRLBitmap4Job(struct jData *jp, struct hData *hp,
               int *rlBitmaps[RL_CONSUMER_POSITION_MAX])
{
    static char fname[] = "getRLBitmap4Job";
    int j;
    void *objs[RL_CONSUMER_POSITION_MAX];

    if (jp == NULL || generalRLConf.nLimits <= 0 || rlBitmapSize <= 0) {
        return -1;
    }

    objs[RL_CONSUMER_POSITION_HOST] = hp;
    objs[RL_CONSUMER_POSITION_QUEUE] = jp->qPtr;
    objs[RL_CONSUMER_POSITION_USER] = jp->uPtr;
    objs[RL_CONSUMER_POSITION_PROJECT] = (void *)JOB_PROJECT_NAME(jp);

    for (j = 0; j < RL_CONSUMER_POSITION_MAX; j++) {
        rlBitmaps[j] = getRLBitmap(objs[j], j);
    }

    if (logclass & LC_JLIMIT) {
        ls_syslog(LOG_DEBUG2, "\
%s: job <%s> rlBitmap result:", fname, lsb_jobid2str(jp->jobId));
        for (j = 0; j < RL_CONSUMER_POSITION_MAX; j++) {
            int k;
            ls_syslog(LOG_DEBUG2, "\
%s:   dimension[%d]:", fname, j);
            for (k = 0; k < generalRLConf.nLimits; k++) {
                int isSet = 0;
                TEST_BIT(k, rlBitmaps[j], isSet);
                ls_syslog(LOG_DEBUG2, "\
%s:     limit[%d] <%s>: %s",
                          fname, k,
                          generalRLConf.limits[k].name,
                          isSet ? "MATCH" : "NO_MATCH");
            }
        }
    }

    return 0;
}


static int getLimitKey4Job(RL_LIMIT_T *limit, struct jData *jp, char *hostname,
                           char *buf, int bufSize) {
    static char fname[] = "getLimitKey4Job";
    int rc;

    if (limit == NULL || jp == NULL || buf == NULL || bufSize <= 0) {
        return 1;
    }

    rc = getLimitKey(limit, hostname,
                     (jp->qPtr != NULL) ? jp->qPtr->queue : NULL,
                     jp->userName, JOB_PROJECT_NAME(jp),
                     buf, bufSize);
    if (rc != 0) {
        ls_syslog(LOG_ERR, "\
%s: Failed to generate limit key for job <%s> and limit <%s>",
                  fname, lsb_jobid2str(jp->jobId), limit->name);
    }

    return rc;

} /* getLimitKey4Job */


/********************************************************************************
 * getLimitKey
 * Description:
 *     Generate a unique key string for a limit with the given consumer values. The key
 *     format is: limitname#hostval#queueval#userval#projectval
 *
 *     For each consumer dimension:
 *     - If mode is RL_CONSUMER_MODE_SHARED: value is empty
 *     - If mode is RL_CONSUMER_MODE_PER: value is the job's attribute
 *       (hostname / queue name / user name / project name)
 *
 * Input:
 *     limit [in]: Pointer to the RL_LIMIT_T struct
 *     hostname [in]: Host value to match against HOST consumer
 *     queue [in]: Queue name to match against QUEUE consumer
 *     user [in]: User name to match against USER consumer
 *     project [in]: Project name to match against PROJECT consumer
 *     buf [in]: Caller-provided buffer to receive the key string. Must be
 *               large enough to hold the key (RL_KEY_BUF_LEN is sufficient).
 *     bufSize [in]: Size of buf in bytes
 *
 * Return:
 *     int: 0 on success (key written, NUL-terminated into buf).
 *          1 on error or if the key would be truncated.
 ********************************************************************************/
static int
getLimitKey(RL_LIMIT_T *limit, char *hostname, char *queue, char *user, char *project,
            char *buf, int bufSize)
{
    static char fname[] = "getLimitKey";
    const char *consumerValues[RL_CONSUMER_POSITION_MAX];
    int i;
    int needed;

    if (limit == NULL || buf == NULL || bufSize <= 0) {
        return 1;
    }

    /* Determine the value for each consumer dimension based on mode */
    for (i = 0; i < RL_CONSUMER_POSITION_MAX; i++) {
        RL_CONSUMER_T *consumer = &limit->consumers[i];

        if (consumer->value == NULL || consumer->mode == RL_CONSUMER_MODE_SHARED) {
            consumerValues[i] = "";
        } else {
            /* RL_CONSUMER_MODE_PER: use job's attribute */
            switch (i) {
                case RL_CONSUMER_POSITION_HOST:
                    consumerValues[i] = (hostname != NULL) ? hostname : "";
                    break;
                case RL_CONSUMER_POSITION_QUEUE:
                    consumerValues[i] = (queue != NULL) ? queue : "";
                    break;
                case RL_CONSUMER_POSITION_USER:
                    consumerValues[i] = (user != NULL) ? user : "";
                    break;
                case RL_CONSUMER_POSITION_PROJECT:
                    consumerValues[i] = project;
                    if (consumerValues[i] == NULL) {
                        consumerValues[i] = "";
                    }
                    break;
                default:
                    consumerValues[i] = "";
                    break;
            }
        }
    }

    /* Build key directly into caller's buffer. snprintf returns the number
     * of chars that would have been written (excluding NUL); if that equals
     * bufSize the key was truncated. */
    needed = snprintf(buf, bufSize, "%s#%s#%s#%s#%s",
                      limit->name,
                      consumerValues[RL_CONSUMER_POSITION_HOST],
                      consumerValues[RL_CONSUMER_POSITION_QUEUE],
                      consumerValues[RL_CONSUMER_POSITION_USER],
                      consumerValues[RL_CONSUMER_POSITION_PROJECT]);

    if (needed < 0 || needed >= bufSize) {
        ls_syslog(LOG_ERR, "\
%s: limit key truncated (needed=%d, bufSize=%d) for limit <%s>",
                  fname, needed, bufSize, limit->name);
        return 1;
    }

    return 0;
}

/********************************************************************************
 * getRsrcReserveType
 * Description:
 *     Determine the reserve type of a resource based on the current
 *     configuration and the resource index.
 *
 *     A host-based resource (resNo < allLsInfo->numIndx) is controlled by
 *     the global parameter resourcePerTask: if it is set, the resource is
 *     reserved per task (RSRC_PER_TASK); otherwise it is reserved per
 *     host (RL_RSRV_PER_HOST).
 *
 *     A shared resource (resNo >= allLsInfo->numIndx) is controlled by
 *     the global parameter slotResourceReserve: if it is set, the resource
 *     is reserved per task (RSRC_PER_TASK); otherwise it is reserved
 *     per job (RSRC_PER_JOB).
 *
 * Input:
 *     resNo [in]: the resource index in allLsInfo->resTable
 *
 * Return:
 *     RSRC_PER_JOB / RSRC_PER_HOST / RSRC_PER_TASK
 ********************************************************************************/
static int
getRsrcReserveType(int resNo)
{
    if (resNo < 0 || allLsInfo == NULL) {
        return RSRC_PER_JOB;
    }

    if (resNo < allLsInfo->numIndx) { /*host-based resource*/
        if (resourcePerTask) {
            return RSRC_PER_TASK;
        }
        return RSRC_PER_HOST;
    }

    /*shared resource*/
    if (slotResourceReserve) {
        return RSRC_PER_TASK;
    }
    return RSRC_PER_JOB;
}

/********************************************************************************
 * getRsrcLimitValue
 *
 * Resolve the effective (absolute) limit value for a resource of a limit.
 * For non-percent resources, return rsrc->value directly. For percent
 * resources under a per-host host consumer, convert the percentage to an
 * absolute value using the given host's capacity (maxMem/maxSwap/maxTmp/
 * numCPUs). For percent resources not under per-host mode, the percentage
 * form is returned as-is (the limit is interpreted relatively).
 *
 * Mirrors the percentage handling in updRLAccount4Job().
 *
 * Input:
 *   limitConf [in]: pointer to the RL_LIMIT_T configuration
 *   rsrc      [in]: pointer to the RL_RESOURCE_T
 *   hp        [in]: host used to resolve per-host capacities; may be NULL
 *                   when host consumer is not PER mode.
 *
 * Return: the effective limit value (absolute or percentage-as-is).
 ********************************************************************************/
static float
getRsrcLimitValue(const RL_LIMIT_T *limitConf,
                  const RL_RESOURCE_T *rsrc,
                  const struct hData *hp)
{
    int hostMode;

    if (!rsrc->isPercent && rsrc->type != RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR) {
        return rsrc->value;
    }

    hostMode = limitConf->consumers[RL_CONSUMER_POSITION_HOST].mode;
    /* Percentage form: only per-host mode converts to absolute. */
    if (hostMode != RL_CONSUMER_MODE_PER || hp == NULL) {
        return rsrc->value;
    }

    switch (rsrc->type) {
        case RL_RESOURCE_TYPE_MEM:
            /* hp->maxMem and limit value are both in MB. */
            return (float)hp->maxMem * rsrc->value / 100.0;
        case RL_RESOURCE_TYPE_SWP:
            return (float)hp->maxSwap * rsrc->value / 100.0;
        case RL_RESOURCE_TYPE_TMP:
            return (float)hp->maxTmp * rsrc->value / 100.0;
        case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
            /* Truncate to integer first, then convert to float (per
             * project convention for SLOTS_PER_PROCESSOR percent mode). */
            return (float)(int)((float)hp->numCPUs * rsrc->value);
        default:
            return rsrc->value;
    }
} /*getRsrcLimitValue*/

/********************************************************************************
 * checkRsrcLimit
 * Description:
 *     Find all matching limits that do not configure host consumers for a job,
 *     and identify the highest-level limit restricting the job's resource usage.
 *     Used to validate whether a job is ready within the scheduling session.
 *
 *     A "matching limit" is one where the job matches in ALL consumer
 *     dimensions (bitmap AND result bit = 1).
 *     A "restricting limit" is one where the job's resource demand exceeds
 *     the available capacity (avail = limit - used) for any resource.
 *
 *
 * Algorithm:
 *     1. AND all bitmap rows to find limits matching in ALL consumer dimensions
 *     2. For each matching limit:
 *        a. Generate key with getLimitKey4Job(), look up in rlAccountTab
 *        b. If active usage exists: avail = limit - used
 *           - If avail < job demand → restricts job (stop collecting matches)
 *           - If avail >= job demand → collect into matchedRLimits
 *        c. If no active usage: avail = configured limit value
 *           (for % mode MEM/SWP/TMP/SLOTS_PER_PROCESSOR, avail = hp->max * %)
 *           - If avail < job demand → restricts job (stop collecting matches)
 *           - If avail >= job demand → collect into matchedRLimits
 *        d. For per-task resources, job demand = rusage * numProcessors
 *     3. Among all restricting limits, find the one with smallest level
 *     4. If cluster level found, exit early (highest priority)
 *     5. If restricting limit found: free matchedRLimits, return it
 *        If not found: return NULL, matchedRLimits contains all matching limits
 *
 * Input:
 *     rlBitmaps [in/out]: Fixed-size array (RL_CONSUMER_POSITION_MAX) of bitmap
 *                         row pointers. Two usage modes:
 *                           rlBitmaps == NULL  : function fills a local stack
 *                                               array and uses it internally
 *                                               (caller never sees it)
 *                           rlBitmaps[0] == NULL : function fills the caller's
 *                                                 array (rows are cache-owned,
 *                                                 borrowed by the caller)
 *                           rlBitmaps[0] != NULL : caller-provided (already
 *                                                 filled) array is reused
 *     jp [in]: Job data pointer
 *     hp [in]: Host data pointer (for maxMem/maxSwap/maxTmp when % mode)
 *     matchedRLimits [out]: List of matching limits (RL_ALLOC_RLIMIT_T entries)
 *                            Only populated if no restricting limit is found
 *
 * Return:
 *     NULL: No restricting limit found; matchedRLimits contains all matches
 *     RL_ALLOC_RLIMIT_T *: Restricting limit found (caller must free);
 *                           matchedRLimits is freed internally
 ********************************************************************************/
RL_ALLOC_RLIMIT_T *
checkRsrcLimit(int *rlBitmaps[RL_CONSUMER_POSITION_MAX], struct jData *jp,
               struct hData *hp, LIST_T **matchedRLimits)
{
    static char fname[] = "checkRsrcLimit";
    int *andBitmap;
    int i, j;
    int bestLevel = RL_PEND_LEVEL_HOST + 1; /* larger than any valid level */
    RL_ALLOC_RLIMIT_T *bestAllocLimit = NULL;
    RL_ALLOC_RLIMIT_T *curAllocLimit = NULL;
    int collectMatches = TRUE;
    struct resVal *jobResVal = NULL;
    int *localBitmap[RL_CONSUMER_POSITION_MAX] = {0}; /* stack array for ownBitmap mode */
    int **bitmap;      /* the bitmap actually used for matching */

    if (generalRLConf.nLimits <= 0 || jp == NULL || matchedRLimits == NULL) {
        return NULL;
    }

    /* Resolve the bitmap to use:
     *   rlBitmaps == NULL      : fill a local stack array (ownBitmap), use it
     *   rlBitmaps[0] == NULL   : fill the caller's array (rows are borrowed)
     *   rlBitmaps[0] != NULL   : reuse caller-provided array
     */
    if (rlBitmaps == NULL) {
        if (getRLBitmap4Job(jp, hp, localBitmap) < 0) {
            return NULL;
        }
        bitmap = localBitmap;
    } else if (rlBitmaps[0] == NULL) {
        if (getRLBitmap4Job(jp, hp, rlBitmaps) < 0) {
            return NULL;
        }
        bitmap = rlBitmaps;
    } else {
        bitmap = rlBitmaps;
    }

    if (generalRLConf.nLimits <= 0) {
        return NULL;
    }

    /* Initialize matchedRLimits list */
    *matchedRLimits = listCreate("matchedRLimits");

    /* Step 1: AND all consumers bitmap together (reusable static buffer) */
    andBitmap = getRLAndBitmap();
    if (andBitmap == NULL) {
        listDestroy(*matchedRLimits, freeRLAllocLimitEntry);
        *matchedRLimits = NULL;
        return NULL;
    }
    for (j = 0; j < rlBitmapSize; j++) {
        andBitmap[j] = bitmap[0][j];
    }
    for (i = 1; i < RL_CONSUMER_POSITION_MAX; i++) {
        for (j = 0; j < rlBitmapSize; j++) {
            andBitmap[j] &= bitmap[i][j];
        }
    }

    if (jp->shared->mergedResReqEnt != NULL) {
        jobResVal = GET_JOB_MERGED_RES_REQ(jp);
    }

    /* Step 2: Iterate over each matching limit */
    for (i = 0; i < generalRLConf.nLimits; i++) {
        int isSet = FALSE;
        RL_LIMIT_T *limit;
        char keyBuf[RL_KEY_BUF_LEN];
        hEnt *acctEnt = NULL;
        RL_USAGE_T *usage = NULL;
        int restrictsJob = FALSE;

        TEST_BIT(i, andBitmap, isSet);
        if (!isSet) {
            continue;
        }

        limit = &generalRLConf.limits[i];

        /* Step 2.1: Generate key and look up in rlAccountTab */
        if (getLimitKey4Job(limit, jp, NULL, keyBuf, sizeof(keyBuf)) == 0) {
            acctEnt = h_getEnt_(&rlAccountTab, keyBuf);
        }
        if (acctEnt != NULL) {
            usage = (RL_USAGE_T *)acctEnt->hData;
        }

        /* Build RL_ALLOC_RLIMIT_T for this limit */
        curAllocLimit = (RL_ALLOC_RLIMIT_T *)my_calloc(1,
                            sizeof(RL_ALLOC_RLIMIT_T), fname);
        curAllocLimit->limitNo = i;
        curAllocLimit->rsrcCnt = limit->nResources;
        curAllocLimit->usageEnt = acctEnt;
        curAllocLimit->rsrcAvails = (RL_RESOURCE_AVAIL_T *)my_calloc(
            limit->nResources, sizeof(RL_RESOURCE_AVAIL_T), fname);

        /* Check each resource in this limit */
        for (j = 0; j < limit->nResources; j++) {
            RL_RESOURCE_T *rsrc = &limit->resources[j];
            float limitValue = INFINIT_FLOAT;
            float usedValue = 0.0;
            float availValue = 0.0;
            float jobDemand = 0.0;
            int rsrcIsSet = FALSE;
            int reserveType;

            /* Build RL_RESOURCE_AVAIL_T for this resource */
            curAllocLimit->rsrcAvails[j].name = rsrc->resName;
            curAllocLimit->rsrcAvails[j].resNo = rsrc->resNo;


            /* Determine limit value */
            if (usage != NULL && j < usage->rsrcCnt) {
                limitValue = usage->rsrcAccounts[j].limit;
                usedValue = usage->rsrcAccounts[j].used;
            } else {
                limitValue = getRsrcLimitValue(limit, rsrc, hp);
            }

            /* Compute avail = limit - used */
            availValue = limitValue - usedValue;
            curAllocLimit->rsrcAvails[j].orgAvail = availValue;
            curAllocLimit->rsrcAvails[j].avail = availValue;

            /* Determine job's demand for this resource */
            switch (rsrc->type) {
                case RL_RESOURCE_TYPE_JOBS:
                    jobDemand = 1.0; /* each job counts as 1 */
                    break;
                case RL_RESOURCE_TYPE_SLOTS:
                case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
                    jobDemand = (float)jp->shared->jobBill.numProcessors;
                    break;
                case RL_RESOURCE_TYPE_MEM:
                case RL_RESOURCE_TYPE_SWP:
                case RL_RESOURCE_TYPE_TMP:
                case RL_RESOURCE_TYPE_RSRC:
                    if (jobResVal != NULL) {
                        TEST_BIT(rsrc->resNo, jobResVal->rusgBitMaps, rsrcIsSet);
                    }
                    if (jobResVal != NULL && rsrcIsSet) {
                        jobDemand = jobResVal->val[rsrc->resNo];
                        /* Apply per-task scaling: if resource is per-task, multiply by
                         * numProcessors */
                        reserveType = getRsrcReserveType(rsrc->resNo);
                        if (reserveType == RSRC_PER_TASK) {
                            jobDemand *= jp->shared->jobBill.numProcessors;
                        }
                    } else {
                        jobDemand = 0.0;
                    }

                    break;
                default:
                    continue;
            }

            /* Check if this resource restricts the job */
            if (jobDemand > availValue) {
                restrictsJob = TRUE;
                /* Set avail to 0 for this resource to show it restricts the job */
                curAllocLimit->rsrcAvails[j].avail = 0.0;
                break; /* first exceeded resource found */
            }
        }

        /* Step 2.2: If this limit restricts the job */
        if (restrictsJob) {
            collectMatches = FALSE; /* stop collecting matched limits */

            if (limit->level < bestLevel) {
                /* Free previous best if any */
                if (bestAllocLimit != NULL) {
                    freeRLAllocLimit(bestAllocLimit);
                }
                bestLevel = limit->level;
                bestAllocLimit = curAllocLimit;
            } else {
                freeRLAllocLimit(curAllocLimit);
            }

            /* If cluster level, exit early */
            if (limit->level == RL_PEND_LEVEL_CLUSTER) {
                break;
            }
            continue;
        }

        /* Step 2.3: Limit does not restrict job */
        if (collectMatches) {
            /* Collect this matching limit into matchedRLimits */
            listInsertEntryAtBack(*matchedRLimits, (LIST_ENTRY_T *)curAllocLimit);
        } else {
            /* free the limit which needn't be collected*/
            freeRLAllocLimit(curAllocLimit);
        }
    }

    /* andBitmap is the reusable static buffer (getRLAndBitmap()); localBitmap
     * is a stack array and its rows are cache-owned, so neither is freed here. */

    /* Step 3: Return result */
    if ( bestAllocLimit ) {
        /* Found a restricting limit: free matchedRLimits and return it */
        listDestroy(*matchedRLimits, freeRLAllocLimitEntry);
        *matchedRLimits = NULL;
        return bestAllocLimit;
    }

    /* No restricting limit found: matchedRLimits contains all matching limits */
    return NULL;
}

/********************************************************************************
 * getRLMinAvailSlots4Host
 * Description:
 *     Traverse the mainRLimits and hostRLimits lists in jRLCache, and
 *     compute the minimum number of slots that the host can provide to the job.
 *     Used in scheduling session.
 *
 *     For each matching limit, iterate over its resources and compute the
 *     available slots according to the resource type and the job's rusage:
 *       - JOBS: if limit value > 0, availSlots = numProcessors
 *       - SLOTS / SLOTS_PER_PROCESSOR: availSlots = avail
 *       - Other resources:
 *           * if job does not request the resource: availSlots = numProcessors
 *           * if job requests the resource, get the reserve type:
 *               - per-job / per-host: if avail >= job demand,
 *                                      availSlots = numProcessors; else 0
 *               - per-task: availSlots = (int)(avail / job demand)
 *
 *     If any limit yields 0 available slots, that limit is recorded via the
 *     output parameter allocLimit, and the restricting resource is recorded
 *     via the output parameter rsrcAvail. The search stops early.
 *
 * Input:
 *     jp [in]: Job data pointer
 *     hp [in]: Host data pointer
 *     allocLimit [out]: Pointer to receive the RL_ALLOC_RLIMIT_T that restricts
 *                       the job to 0 slots, if any. Only set when 0 is returned.
 *     rsrcAvail [out]: Pointer to receive the RL_RESOURCE_AVAIL_T that causes
 *                      the job to be restricted to 0 slots, if any. Only set
 *                      when 0 is returned.
 *
 * Return:
 *     The minimum number of slots available across all matching limits.
 *     0 if any limit restricts the job to 0 slots (allocLimit and rsrcAvail
 *     are set).
 ********************************************************************************/
static int
getRLMinAvailSlots4Host(struct jData *jp, struct hData *hp,
                        RL_JOB_LIMITS_CACHE_T *jRLCache,
                        RL_ALLOC_RLIMIT_T **failedLimit,
                        RL_RESOURCE_AVAIL_T **failedRsrc)
{
    LIST_ITERATOR_T iter;
    LIST_ENTRY_T *ent;
    RL_ALLOC_RLIMIT_T *curAllocLimit = NULL;
    int minSlots = -1;
    int curSlots;
    int i;
    struct resVal *jobResVal = NULL;
    int numProcessors;

    if (jp == NULL || failedLimit == NULL || failedRsrc == NULL) {
        return -1;
    }

    *failedLimit = NULL;
    *failedRsrc = NULL;

    numProcessors = hp->numCPUs;
    if (numProcessors <= 0) {
        return 0;
    }

    if (jp->shared->mergedResReqEnt != NULL) {
        jobResVal = GET_JOB_MERGED_RES_REQ(jp);
    }

    /* Traverse both mainRLimits and hostRLimits lists */
    {
        LIST_T *lists[2];
        int nLists;

        lists[0] = jRLCache->mainRLimits;
        lists[1] = jRLCache->hostRLimits;

        for (nLists = 0; nLists < 2; nLists++) {
            LIST_T *curList = lists[nLists];

            if (curList == NULL || LIST_IS_EMPTY(curList)) {
                continue;
            }

            LIST_ITERATOR_ZERO_OUT(&iter);
            listIteratorAttach(&iter, curList);

            for (ent = listIteratorGetCurEntry(&iter);
                 ent != NULL && !listIteratorIsEndOfList(&iter);
                 listIteratorNext(&iter, &ent)) {
                curAllocLimit = (RL_ALLOC_RLIMIT_T *)ent;
                curSlots = numProcessors; /* default: full slots available */

                for (i = 0; i < curAllocLimit->rsrcCnt; i++) {
                    RL_RESOURCE_AVAIL_T *curRsrcAvail = &curAllocLimit->rsrcAvails[i];
                    RL_LIMIT_T *limit = &generalRLConf.limits[curAllocLimit->limitNo];
                    RL_RESOURCE_T *rsrc = &limit->resources[i];
                    float avail;
                    float availSlots = 0.0;
                    int rsrcIsSet = FALSE;
                    int reserveType;

                    avail = curRsrcAvail->avail;

                    switch (rsrc->type) {
                        case RL_RESOURCE_TYPE_JOBS:
                            /* If job limit > 0, job can use numProcessors */
                            if (avail > 0.0) {
                                availSlots = (float)numProcessors;
                            } else {
                                availSlots = 0.0;
                            }
                            break;

                        case RL_RESOURCE_TYPE_SLOTS:
                        case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
                            /* availSlots = avail (limit is in slots) */
                            availSlots = avail;
                            break;

                        case RL_RESOURCE_TYPE_MEM:
                        case RL_RESOURCE_TYPE_SWP:
                        case RL_RESOURCE_TYPE_TMP:
                        case RL_RESOURCE_TYPE_RSRC:
                            /* Check if job requested this resource in rusage */
                            if (jobResVal != NULL) {
                                TEST_BIT(rsrc->resNo, jobResVal->rusgBitMaps,
                                         rsrcIsSet);
                            }
                            if (jobResVal == NULL || !rsrcIsSet) {
                                /* Job did not request this resource, the host
                                 * can provide numProcessors slots */
                                availSlots = (float)numProcessors;
                            } else {
                                float jobDemand = jobResVal->val[rsrc->resNo];
                                reserveType = getRsrcReserveType(rsrc->resNo);
                                if (reserveType == RSRC_PER_TASK) {
                                    /* per-task: availSlots = avail / jobDemand */
                                    if (jobDemand > 0.0) {
                                        availSlots = avail / jobDemand;
                                    } else {
                                        availSlots = (float)numProcessors;
                                    }
                                } else {
                                    /* per-job or per-host: if avail >= jobDemand,
                                     * the host can provide numProcessors slots */
                                    if (avail >= jobDemand) {
                                        availSlots = (float)numProcessors;
                                    } else {
                                        availSlots = 0.0;
                                    }
                                }
                            }
                            break;

                        default:
                            continue;
                    }

                    if (availSlots < (float)curSlots) {
                        curSlots = (int)availSlots;
                    }
                    if (curSlots == 0) {
                        /* This resource causes availSlots to be 0, record it
                         * so caller can identify the restricting resource. */
                        *failedRsrc = curRsrcAvail;
                        break;
                    }
                }

                if (curSlots == 0) {
                    /* This limit restricts the job to 0 slots, record it */
                    *failedLimit = curAllocLimit;
                    return 0;
                }

                if (minSlots < 0 || curSlots < minSlots) {
                    minSlots = curSlots;
                }
            }
        }
    }

    if (minSlots < 0) {
        /* No matching limit found, the host can provide numProcessors slots */
        minSlots = numProcessors;
    }

    return minSlots;
}

/********************************************************************************
 * getJobRLimitOnHost
 * Description:
 *     Construct a list of RL_ALLOC_RLIMIT_T entries corresponding to limits
 *     configured with host consumers, which match the target job on the specified
 *     host.
 *
 *     For each matching limit, the function generates a key with getLimitKey4Job()
 *     and looks up active usage in rlAccountTab:
 *       - If active usage exists, avail = limit - used.
 *       - If no active usage, avail = configured limit value. For % mode
 *         MEM/SWP/TMP, avail = hp->max * %; for SLOTS_PER_PROCESSOR, avail
 *         is truncated to integer slots (slots must be integer).
 *
 * Input:
 *     jp [in]: Job data pointer
 *     hp [in]: Host data pointer
 *     andBitmap [in]: The AND result of all consumer-dimension bitmaps,
 *                     indicating which limits match the job.
 *
 * Return:
 *     A newly created list of RL_ALLOC_RLIMIT_T entries (caller must destroy
 *     with listDestroy() + freeRLAllocLimitEntry). Returns NULL on error.
 ********************************************************************************/
static LIST_T *
getJobRLimitOnHost(struct jData *jp, struct hData *hp, int *andBitmap)
{
    static char fname[] = "getJobRLimitOnHost";
    LIST_T *hostRLimits = NULL;
    int i, j;

    if (jp == NULL || hp == NULL || andBitmap == NULL) {
        return NULL;
    }

    hostRLimits = listCreate("hostRLimits");

    for (i = 0; i < generalRLConf.nLimits; i++) {
        int isSet = FALSE;
        RL_LIMIT_T *limit;
        char keyBuf[RL_KEY_BUF_LEN];
        hEnt *acctEnt = NULL;
        RL_USAGE_T *usage = NULL;
        RL_ALLOC_RLIMIT_T *curAllocLimit = NULL;
        RL_CONSUMER_T *hostConsumer;

        TEST_BIT(i, andBitmap, isSet);
        if (!isSet) {
            continue;
        }

        limit = &generalRLConf.limits[i];
        hostConsumer = &limit->consumers[RL_CONSUMER_POSITION_HOST];
        /* Only collect limits that configure a host consumer dimension */
        if (hostConsumer->value == NULL) {
            continue;
        }

        /* Generate key and look up active usage in rlAccountTab */
        if (getLimitKey4Job(limit, jp, hp->host, keyBuf, sizeof(keyBuf)) == 0) {
            acctEnt = h_getEnt_(&rlAccountTab, keyBuf);
        }
        if (acctEnt != NULL) {
            usage = (RL_USAGE_T *)acctEnt->hData;
        }

        /* Build RL_ALLOC_RLIMIT_T for this limit */
        curAllocLimit = (RL_ALLOC_RLIMIT_T *)my_calloc(1,
                            sizeof(RL_ALLOC_RLIMIT_T), fname);
        curAllocLimit->limitNo = i;
        curAllocLimit->rsrcCnt = limit->nResources;
        curAllocLimit->usageEnt = acctEnt;
        if (hostConsumer->mode == RL_CONSUMER_MODE_PER) {
            /* For per-host mode, the host consumer dimension is the host name */
            curAllocLimit->hostRef.hostname = hp->host;
        } else {
            /* For shared mode, the host consumer dimension is the reference count */
            curAllocLimit->hostRef.numRef = 1;
        }
        curAllocLimit->rsrcAvails = (RL_RESOURCE_AVAIL_T *)my_calloc(
            limit->nResources, sizeof(RL_RESOURCE_AVAIL_T), fname);

        /* Compute avail for each resource */
        for (j = 0; j < limit->nResources; j++) {
            RL_RESOURCE_T *rsrc = &limit->resources[j];
            float limitValue = INFINIT_FLOAT;
            float usedValue = 0.0;
            float availValue = 0.0;

            curAllocLimit->rsrcAvails[j].name = rsrc->resName;
            curAllocLimit->rsrcAvails[j].resNo = rsrc->resNo;

            if (usage != NULL && j < usage->rsrcCnt) {
                /* active usage exists, avail = limit - used */
                limitValue = usage->rsrcAccounts[j].limit;
                usedValue = usage->rsrcAccounts[j].used;
            } else {
                /* no active usage, avail = configured limit value.
                 * For % mode MEM/SWP/TMP/SLOTS_PER_PROCESSOR, the limit total
                 * is the host's max resource multiplied by the percentage. */
                limitValue = getRsrcLimitValue(limit, rsrc, hp);
            }

            availValue = limitValue - usedValue;
            curAllocLimit->rsrcAvails[j].orgAvail = availValue;
            curAllocLimit->rsrcAvails[j].avail = availValue;
        }

        /* Collect this matching limit into hostRLimits */
        listInsertEntryAtBack(hostRLimits, (LIST_ENTRY_T *)curAllocLimit);
    }

    return hostRLimits;
}

/********************************************************************************
 * ckHostSlots4RLimits
 * Description:
 *     Check all the resource limits that match the job on the given host, and
 *     return the minimum number of slots that the limits allow the job to use
 *     on this host.
 *     Used when validate and collect candidate hosts.
 *
 * Algorithm:
 *     1. Call getRLBitmap(host) to generate the host-dimension 
 *        bitmap for the input hp, and update
 *        jobRLimitsCache.rlBitmaps[RL_CONSUMER_POSITION_HOST]
 *     2. AND all bitmap rows in jobRLimitsCache.rlBitmaps together to find limits
 *        matching in all consumer dimensions
 *     3. Free jobRLimitsCache.hostRLimits so that the following logic can
 *        rebuild it for the input host
 *     4. Call getJobRLimitOnHost() to collect limits that match and configure
 *        a host consumer into jobRLimitsCache.hostRLimits
 *     5. Traverse jobRLimitsCache.mainRLimits and jobRLimitsCache.hostRLimits
 *        to find the minimum available slots the host can provide to the job.
 *        If a limit yields 0 slots, record the restricting limit via the
 *        output parameter allocLimit and the restricting resource via the
 *        output parameter rsrcAvail, then return 0.
 *
 * Input:
 *     hp [in]: Host data pointer
 *     jp [in]: Job data pointer
 *     allocLimit [out]: Pointer to receive the RL_ALLOC_RLIMIT_T that restricts
 *                       the job to 0 slots, if any. Only set when 0 is returned.
 *     rsrcAvail [out]: Pointer to receive the RL_RESOURCE_AVAIL_T that causes
 *                      the job to be restricted to 0 slots, if any. Only set
 *                      when 0 is returned.
 *
 * Return:
 *     The minimum number of slots the host can provide to the job.
 *     0 if any matching limit restricts the job to 0 slots (allocLimit and
 *     rsrcAvail are set).
 *     -1 if any error occurs.
 *
 * Note: ckHostSlots4RLimits() runs after checkRsrcLimit(). checkRsrcLimit()
 *       caches non-host resource limits in jobRLimitsCache, which we reuse to
 *       calculate available slots on each candidate host (reusing could help me
 *       reduce a lot duplicated limit checking for limits that match the job on
 *       different hosts).
 *       This function only gathers host-specific limits for the current candidate
 *       host.
 */
int
ckHostSlots4RLimits(struct hData *hp, struct jData *jp,
                    RL_ALLOC_RLIMIT_T **failedLimit,
                    RL_RESOURCE_AVAIL_T **failedRsrc)
{
    int *andBitmap = NULL;
    int i, j;
    int *hostBitmap = NULL;

    if (hp == NULL || jp == NULL || failedLimit == NULL || failedRsrc == NULL) {
        return -1;
    }

    *failedLimit = NULL;
    *failedRsrc = NULL;

    if (generalRLConf.nLimits <= 0) {
        return hp->numCPUs;
    }

    /* Step 1: Generate host-dimension bitmap and update
     * jobRLimitsCache.rlBitmaps[RL_CONSUMER_POSITION_HOST] */
    hostBitmap = getRLBitmap(hp, RL_CONSUMER_POSITION_HOST);
    if (hostBitmap == NULL) {
        return -1;
    }

    if (jobRLimitsCache.rlBitmaps[0] == NULL) {
        /* bitmaps not yet built, cannot proceed */
        return -1;
    }
    jobRLimitsCache.rlBitmaps[RL_CONSUMER_POSITION_HOST] = hostBitmap;

    /* Step 2: AND all bitmap rows in jobRLimitsCache.rlBitmaps
     * (reusable static buffer) */
    andBitmap = getRLAndBitmap();
    if (andBitmap == NULL) {
        return -1;
    }
    for (j = 0; j < rlBitmapSize; j++) {
        andBitmap[j] = jobRLimitsCache.rlBitmaps[0][j];
    }
    for (i = 1; i < RL_CONSUMER_POSITION_MAX; i++) {
        if (jobRLimitsCache.rlBitmaps[i] == NULL) {
            continue;
        }
        for (j = 0; j < rlBitmapSize; j++) {
            andBitmap[j] &= jobRLimitsCache.rlBitmaps[i][j];
        }
    }

    /* Step 3: Free jobRLimitsCache.hostRLimits so the following logic
     * can rebuild it for the input host */
    if (jobRLimitsCache.hostRLimits != NULL) {
        listDestroy(jobRLimitsCache.hostRLimits, freeRLAllocLimitEntry);
        jobRLimitsCache.hostRLimits = NULL;
    }

    /* Step 4: Collect limits that match and configure a host consumer
     * into jobRLimitsCache.hostRLimits */
    jobRLimitsCache.hostRLimits = getJobRLimitOnHost(jp, hp, andBitmap);

    /* andBitmap is the reusable static buffer; do not free. */

    /* Step 5: Find the minimum available slots the host can provide to the job */
    return getRLMinAvailSlots4Host(jp, hp, &jobRLimitsCache, failedLimit, failedRsrc);
}

/********************************************************************************
 * getHostSlots4RLimit
 * Description:
 *     Check all the resource limits that match the job on the given host, and
 *     return the minimum number of slots that the limits allow the job to use
 *     on this host. The collected host-level limits are returned via the output
 *     parameter hostRLimits.
 *
 *     Unlike ckHostSlots4RLimits() which writes results back into the global
 *     jobRLimitsCache, this function uses a local temporary cache (tmpCache)
 *     to avoid polluting the global cache, so it is safe to call when the
 *     caller wants to probe a host without committing the results. This logic
 *     is to handle limits which is shared between some candidate hosts.
 *
 *     Used when allocating execution hosts to a job from the list of available
 *     candidate hosts. 
 *
 * Algorithm:
 *     1. Initialize a local RL_JOB_LIMITS_CACHE_T tmpCache
 *     2. If generalRLConf.nLimits <= 0, return numProcessors (no limits to check)
 *     3. Call getRLBitmap(host) to generate the host-dimension bitmap and
 *        update jobRLimitsCache.rlBitmaps[RL_CONSUMER_POSITION_HOST]
 *     4. AND all bitmap rows in jobRLimitsCache.rlBitmaps together
 *     5. Call getJobRLimitOnHost() to build tmpCache.hostRLimits
 *     6. For each limit that exists in both jobRLimitsCache.hostRLimits and
 *        tmpCache.hostRLimits, copy the cached avail value from
 *        jobRLimitsCache.hostRLimits into tmpCache.hostRLimits. This reuses
 *        the previously computed avail for limit which host dimension is shared
 *        mode.
 *     7. Point tmpCache.bitmaps and tmpCache.mainRLimits at the global
 *        jobRLimitsCache's bitmaps and mainRLimits so that
 *        getRLMinAvailSlots4Host() can traverse both lists
 *     8. Call getRLMinAvailSlots4Host() on tmpCache to get the minimum slots
 *        and the failed limit/resource
 *
 * Input:
 *     jp [in]: Job data pointer
 *     hp [in]: Host data pointer
 *     hostRLimits [out]: Pointer to receive the list of host-level matching
 *                        limits (RL_ALLOC_RLIMIT_T entries). On success the
 *                        caller owns this list and must release it with
 *                        listDestroy() + freeRLAllocLimitEntry().
 *
 * Output parameters (only meaningful when return value is 0):
 *     failedLimit [out]: Pointer to receive the RL_ALLOC_RLIMIT_T that restricts
 *                        the job to 0 slots. Points into the returned
 *                        hostRLimits list or the global mainRLimits list; the
 *                        caller must NOT free it separately.
 *     failedRsrc [out]: Pointer to receive the RL_RESOURCE_AVAIL_T that causes
 *                       the job to be restricted to 0 slots. Points into a
 *                       limit inside the returned hostRLimits list or the
 *                       global mainRLimits list; the caller must NOT free it
 *                       separately.
 *
 * Return:
 *     The minimum number of slots the host can provide to the job.
 *     0 if any matching limit restricts the job to 0 slots (failedLimit and
 *     failedRsrc are set).
 *     -1 if any error occurs.
 *
 * Caller notes:
 *     - On success, *hostRLimits is set to a newly created list that the
 *       caller MUST destroy with listDestroy(*hostRLimits, freeRLAllocLimitEntry)
 *       to avoid memory leaks.
 *     - When 0 is returned, failedLimit points to an entry inside *hostRLimits
 *       or the global mainRLimits; the caller must NOT free it separately -
 *       it will be freed when the list is destroyed.
 *     - When -1 is returned, *hostRLimits is NULL and no cleanup is needed.
 *     - This function reuses the global jobRLimitsCache.rlBitmaps and updates
 *       the host-dimension bitmap row in place; callers should not assume the
 *       host bitmap row is preserved.
 ********************************************************************************/
int
getHostSlots4RLimit(struct jData *jp, struct hData *hp,
                    LIST_T **hostRLimits,
                    RL_ALLOC_RLIMIT_T **failedLimit,
                    RL_RESOURCE_AVAIL_T **failedRsrc)
{
    RL_JOB_LIMITS_CACHE_T tmpCache = {{0}};
    int *andBitmap = NULL;
    int *hostBitmap = NULL;
    int i, j;
    int minSlots;

    /* Step 1: Validate input parameters */
    if (jp == NULL || hp == NULL || hostRLimits == NULL || failedLimit == NULL || failedRsrc == NULL) {
        return -1;
    }

    *hostRLimits = NULL;
    *failedLimit = NULL;
    *failedRsrc = NULL;

    /* Step 2: no limits configured, host can provide all CPUs */
    if (generalRLConf.nLimits <= 0) {
        return hp->numCPUs;
    }

    /* Step 3: Generate host-dimension bitmap and update
     * jobRLimitsCache.rlBitmaps[RL_CONSUMER_POSITION_HOST] */
    hostBitmap = getRLBitmap(hp, RL_CONSUMER_POSITION_HOST);
    if (hostBitmap == NULL) {
        return -1;
    }
    if (jobRLimitsCache.rlBitmaps[0] == NULL) {
        /* bitmaps not yet built, cannot proceed */
        return -1;
    }
    jobRLimitsCache.rlBitmaps[RL_CONSUMER_POSITION_HOST] = hostBitmap;

    /* Step 4: AND all bitmap rows in jobRLimitsCache.rlBitmaps
     * (reusable static buffer) */
    andBitmap = getRLAndBitmap();
    if (andBitmap == NULL) {
        return -1;
    }
    for (j = 0; j < rlBitmapSize; j++) {
        andBitmap[j] = jobRLimitsCache.rlBitmaps[0][j];
    }
    for (i = 1; i < RL_CONSUMER_POSITION_MAX; i++) {
        if (jobRLimitsCache.rlBitmaps[i] == NULL) {
            continue;
        }
        for (j = 0; j < rlBitmapSize; j++) {
            andBitmap[j] &= jobRLimitsCache.rlBitmaps[i][j];
        }
    }

    /* Step 5: Build tmpCache.hostRLimits for the input host */
    tmpCache.hostRLimits = getJobRLimitOnHost(jp, hp, andBitmap);

    /* andBitmap is the reusable static buffer; do not free. */

    /* Step 6: Find limits which host dimension is shared mode within hostRLimits,
     * then copy the cached avail values from the global cache into tmpCache.
     */
    if (jobRLimitsCache.hostRLimits != NULL
        && !LIST_IS_EMPTY(jobRLimitsCache.hostRLimits)
        && tmpCache.hostRLimits != NULL
        && !LIST_IS_EMPTY(tmpCache.hostRLimits)) {
        LIST_ITERATOR_T tmpIter;
        LIST_ITERATOR_T cacheIter;
        LIST_ENTRY_T *tmpEnt;
        LIST_ENTRY_T *cacheEnt;

        LIST_ITERATOR_ZERO_OUT(&tmpIter);
        listIteratorAttach(&tmpIter, tmpCache.hostRLimits);

        for (tmpEnt = listIteratorGetCurEntry(&tmpIter);
             tmpEnt != NULL && !listIteratorIsEndOfList(&tmpIter);
             listIteratorNext(&tmpIter, &tmpEnt)) {
            RL_ALLOC_RLIMIT_T *tmpLimit = (RL_ALLOC_RLIMIT_T *)tmpEnt;
            RL_LIMIT_T *tmpLimitConf = &generalRLConf.limits[tmpLimit->limitNo];
            int tmpHostMode = tmpLimitConf->consumers[RL_CONSUMER_POSITION_HOST].mode;

            LIST_ITERATOR_ZERO_OUT(&cacheIter);
            listIteratorAttach(&cacheIter, jobRLimitsCache.hostRLimits);

            if (tmpHostMode != RL_CONSUMER_MODE_SHARED) {
                continue;
            }

            for (cacheEnt = listIteratorGetCurEntry(&cacheIter);
                 cacheEnt != NULL && !listIteratorIsEndOfList(&cacheIter);
                 listIteratorNext(&cacheIter, &cacheEnt)) {
                RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)cacheEnt;
                int k;

                if (tmpLimit->limitNo != cacheLimit->limitNo) {
                    continue;
                }

                /* Same limit found in both lists, copy avail values */
                for (k = 0; k < tmpLimit->rsrcCnt
                            && k < cacheLimit->rsrcCnt; k++) {
                    tmpLimit->rsrcAvails[k].avail =
                        cacheLimit->rsrcAvails[k].avail;
                    tmpLimit->rsrcAvails[k].orgAvail =
                        cacheLimit->rsrcAvails[k].orgAvail;
                }

                break;
            }
        }
    }

    /* Step 7: Share bitmaps and mainRLimits with tmpCache so that
     * getRLMinAvailSlots4Host() can traverse both main and host lists.
     * rlBitmaps is a fixed-size array of borrowed pointers; copy the row
     * pointers (rows remain owned by jobRLimitsCache / consumer cache). */
    memcpy(tmpCache.rlBitmaps, jobRLimitsCache.rlBitmaps,
           sizeof(tmpCache.rlBitmaps));
    tmpCache.mainRLimits = jobRLimitsCache.mainRLimits;

    /* Step 8: Find the minimum available slots */
    minSlots = getRLMinAvailSlots4Host(jp, hp, &tmpCache, failedLimit,
                                       failedRsrc);

    /* Return the collected host-level limits to the caller */
    *hostRLimits = tmpCache.hostRLimits;

    return minSlots;
}

/********************************************************************************
 * mergeAllocRLimit2Cache
 * Description:
 *     (1) Subtract the job's resource usage from the avail values in both
 *         jobRLimitsCache.mainRLimits and the input hostRLimits list.
 *     (2) Merge the input hostRLimits list into jobRLimitsCache.hostRLimits:
 *         for limits present in both, copy avail values from hostRLimits into
 *         the cache; for limits only in hostRLimits, move them into the cache.
 *
 * Resource deduction rules (step 1):
 *     - JOBS: subtract 1
 *     - SLOTS / SLOTS_PER_PROCESSOR: subtract nSlots
 *     - Other resources: call getRsrcReserveType() to get the reserve type
 *       * per-host: subtract the job's rusage demand
 *       * per-task: subtract rusage demand * nSlots
 *       * per-job:   subtract the job's rusage demand
 *
 * Input:
 *     hostRLimit [in]: A list of RL_ALLOC_RLIMIT_T entries collected for a
 *                     specific host (typically returned by getHostSlots4RLimit)
 *     jp [in]: Job data pointer
 *     nSlots [in]: Number of slots the job will use on the host
 *
 * Return:
 *     None
 *
 * Note:
 *     - For limits only in hostRLimit, the entries are MOVED (removed from
 *       hostRLimit and appended to jobRLimitsCache.hostRLimits). After this
 *       call, the caller should NOT free those entries via hostRLimit; they
 *       are now owned by jobRLimitsCache.hostRLimits and will be freed when
 *       the cache is released.
 ********************************************************************************/
void
mergeAllocRLimit2Cache(LIST_T *hostRLimits, struct jData *jp, int nSlots)
{
    struct resVal *jobResVal = NULL;
    LIST_ITERATOR_T iter;
    LIST_ENTRY_T *ent;
    LIST_ENTRY_T *nextEnt = NULL;
    int i;
    LIST_T *listsToDeduct[2];
    int nLists = 0;

    if (hostRLimits== NULL || jp == NULL) {
        return;
    }

    if (jp->shared->mergedResReqEnt != NULL) {
        jobResVal = GET_JOB_MERGED_RES_REQ(jp);
    }

    /* Step 1: Subtract job resource usage from avail in mainRLimits and hostRLimits */
    listsToDeduct[0] = jobRLimitsCache.mainRLimits;
    listsToDeduct[1] = hostRLimits;
    for (nLists = 0; nLists < 2; nLists++) {
        LIST_T *curList = listsToDeduct[nLists];

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *curAllocLimit = (RL_ALLOC_RLIMIT_T *)ent;

            for (i = 0; i < curAllocLimit->rsrcCnt; i++) {
                RL_RESOURCE_AVAIL_T *curRsrcAvail = &curAllocLimit->rsrcAvails[i];
                RL_LIMIT_T *limit;
                RL_RESOURCE_T *rsrc;
                int rsrcIsSet = FALSE;
                int reserveType;
                float jobDemand;

                limit = &generalRLConf.limits[curAllocLimit->limitNo];
                rsrc = &limit->resources[i];

                switch (rsrc->type) {
                    case RL_RESOURCE_TYPE_JOBS:
                        break;

                    case RL_RESOURCE_TYPE_SLOTS:
                    case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
                        curRsrcAvail->avail -= (float)nSlots;
                        break;

                    case RL_RESOURCE_TYPE_MEM:
                    case RL_RESOURCE_TYPE_SWP:
                    case RL_RESOURCE_TYPE_TMP:
                    case RL_RESOURCE_TYPE_RSRC:
                        if (jobResVal != NULL) {
                            TEST_BIT(rsrc->resNo, jobResVal->rusgBitMaps,
                                     rsrcIsSet);
                        }
                        if (!rsrcIsSet) {
                            /* Job did not request this resource, no deduction */
                            break;
                        }
                        jobDemand = jobResVal->val[rsrc->resNo];
                        reserveType = getRsrcReserveType(rsrc->resNo);

                        /*We do not reduce avail for per-job resource, because it shared among all hosts*/
                        if (reserveType == RSRC_PER_TASK) {
                            /* per-task: subtract demand * nSlots */
                            curRsrcAvail->avail -= jobDemand * (float)nSlots;
                        } else if (reserveType == RSRC_PER_HOST){
                            /* per-host: subtract demand once */
                            curRsrcAvail->avail -= jobDemand;
                        }
                        break;

                    default:
                        break;
                }
            }
        }
    }

    /* Step 2: Merge hostRLimit into jobRLimitsCache.hostRLimits 
     * a) copy values for common limits which host dimension is shared mode
     * b) move limits only in hostRLimit into jobRLimitsCache.hostRLimits
     */
    if (jobRLimitsCache.hostRLimits == NULL) {
        jobRLimitsCache.hostRLimits = listCreate("hostRLimits");
    }

    if (LIST_IS_EMPTY(hostRLimits)) {
        return;
    }

    LIST_ITERATOR_ZERO_OUT(&iter);
    listIteratorAttach(&iter, hostRLimits);

    for (ent = listIteratorGetCurEntry(&iter);
         ent != NULL && !listIteratorIsEndOfList(&iter);
         ent = nextEnt) {
        RL_ALLOC_RLIMIT_T *hLimit = (RL_ALLOC_RLIMIT_T *)ent;
        RL_LIMIT_T *hLimitConf = &generalRLConf.limits[hLimit->limitNo];
        int hostMode = hLimitConf->consumers[RL_CONSUMER_POSITION_HOST].mode;
        bool_t found = FALSE;
        LIST_ITERATOR_T cacheIter;
        LIST_ENTRY_T *cacheEnt;

        /* Advance iterator BEFORE any potential listRemoveEntry below to
         * avoid iterator invalidation (see collectRunJobRLimits4Acct). */
        listIteratorNext(&iter, &nextEnt);

        /* Search for the matching limit which host consumer is in SHARED mode within jobRLimitsCache.hostRLimits.
         * For SHARED mode, the limit is shared across hosts, we need update avail value in the cache. */
        if (hostMode == RL_CONSUMER_MODE_SHARED
            && !LIST_IS_EMPTY(jobRLimitsCache.hostRLimits)) {
            LIST_ITERATOR_ZERO_OUT(&cacheIter);
            listIteratorAttach(&cacheIter, jobRLimitsCache.hostRLimits);

            for (cacheEnt = listIteratorGetCurEntry(&cacheIter);
                 cacheEnt != NULL && !listIteratorIsEndOfList(&cacheIter);
                 listIteratorNext(&cacheIter, &cacheEnt)) {
                RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)cacheEnt;
                int k;

                if (cacheLimit->limitNo != hLimit->limitNo) {
                    continue;
                }

                /* Same limit found, copy avail values from hostRLimits.
                 * For SHARED mode, increment the reference count as another
                 * host now contributes to this shared limit. */
                for (k = 0; k < hLimit->rsrcCnt
                            && k < cacheLimit->rsrcCnt; k++) {
                    cacheLimit->rsrcAvails[k].avail =
                        hLimit->rsrcAvails[k].avail;
                }
                cacheLimit->hostRef.numRef++;

                found = TRUE;
                break;
            }
        }

        if (!found) {
            /* Limit not in cache, move it into jobRLimitsCache.hostRLimits.
             * Remove from hostRLimits so the caller does not free it. */
            listRemoveEntry(hostRLimits, ent);
            listInsertEntryAtBack(jobRLimitsCache.hostRLimits, (LIST_ENTRY_T *)hLimit);
        }
    }
}

/********************************************************************************
 * updRLAccountTabByCache
 * Description:
 *     Traverse the limits recorded in jobRLimitsCache (both mainRLimits and
 *     hostRLimits) and update the corresponding usage records in rlAccountTab.
 *
 *     Used in scheduling session to update resource account after job allocation.
 *
 *     For each limit:
 *     - If the limit already has an associated RL_USAGE_T (cacheLimit->usageEnt
 *       != NULL), update its resource 'used' fields by subtracting the amount
 *       the job consumed (orgAvail - avail).
 *     - If the limit has no usage record (usageEnt == NULL), create a new
 *       RL_USAGE_T, initialize its resource accounts with the configured limit
 *       values and subtract the job's consumed amount, generate a key via
 *       getLimitKey4Job() (for PER host mode, pass hostRef.hostname as the hostname
 *       argument), and insert the new usage into rlAccountTab.
 *
 * Input:
 *     jp [in]: Job data pointer (used by getLimitKey4Job for queue/user/project
 *              consumer values)
 *
 * Return:
 *     None
 ********************************************************************************/
void
updRLAccountTabByCache(struct jData *jp)
{
    static char fname[] = "updRLAccountTabByCache";
    LIST_T *lists[2];
    int nLists;
    int j;
    struct resVal *jobResVal = NULL;

    if (jp == NULL) {
        return;
    }

    if (jp->effeResReqEnt != NULL) {
        jobResVal = GET_JOB_EFFE_RES_REQ(jp);
    }

    lists[0] = jobRLimitsCache.mainRLimits;
    lists[1] = jobRLimitsCache.hostRLimits;

    for (nLists = 0; nLists < 2; nLists++) {
        LIST_T *curList = lists[nLists];
        LIST_ITERATOR_T iter;
        LIST_ENTRY_T *ent;

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)ent;
            RL_LIMIT_T *limitConf;
            RL_USAGE_T *usage = (cacheLimit->usageEnt != NULL)
                ? (RL_USAGE_T *)cacheLimit->usageEnt->hData : NULL;
            int rsrcIsSet = FALSE;

            limitConf = &generalRLConf.limits[cacheLimit->limitNo];

            if (usage != NULL) {
                /* Usage record already exists, update used values */
                for (j = 0; j < cacheLimit->rsrcCnt
                            && j < usage->rsrcCnt; j++) {
                    RL_RESOURCE_AVAIL_T *rsrcAvail = &cacheLimit->rsrcAvails[j];

                    switch (generalRLConf.limits[cacheLimit->limitNo].resources[j].type) {
                        case RL_RESOURCE_TYPE_JOBS:
                            usage->rsrcAccounts[j].used += 1.0;
                            break;
                        case RL_RESOURCE_TYPE_SLOTS:
                        case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
                            usage->rsrcAccounts[j].used += (cacheLimit->rsrcAvails[j].orgAvail - cacheLimit->rsrcAvails[j].avail);
                            break;
                        case RL_RESOURCE_TYPE_MEM:
                        case RL_RESOURCE_TYPE_SWP:
                        case RL_RESOURCE_TYPE_TMP:
                        case RL_RESOURCE_TYPE_RSRC:
                            if (jobResVal != NULL) {
                                TEST_BIT(rsrcAvail->resNo, jobResVal->rusgBitMaps,
                                        rsrcIsSet);
                            }
                            if (!rsrcIsSet) {
                                /* Job did not request this resource, no deduction */
                                break;
                            }
                            switch (getRsrcReserveType(rsrcAvail->resNo)) {
                                case RSRC_PER_JOB:
                                    usage->rsrcAccounts[j].used += jobResVal->val[rsrcAvail->resNo];
                                    break;
                                case RSRC_PER_HOST:
                                case RSRC_PER_TASK:
                                    usage->rsrcAccounts[j].used += (cacheLimit->rsrcAvails[j].orgAvail - cacheLimit->rsrcAvails[j].avail);
                                    break;
                                default:
                                    break;
                            }
                            break;
                        default:
                            break;
                    }
                }
            } else {
                /* No usage record, create a new one and add to rlAccountTab */
                char keyBuf[RL_KEY_BUF_LEN];
                hEnt *acctEnt = NULL;
                int newEnt;
                char *hostVal = NULL;

                if (limitConf->consumers[RL_CONSUMER_POSITION_HOST].mode == RL_CONSUMER_MODE_PER) {
                    hostVal = cacheLimit->hostRef.hostname;
                }

                if (getLimitKey4Job(limitConf, jp, hostVal, keyBuf, sizeof(keyBuf)) != 0) {
                    continue;
                }

                /* Create new RL_USAGE_T */
                usage = (RL_USAGE_T *)my_calloc(1, sizeof(RL_USAGE_T),
                                                fname);
                usage->limitNo = cacheLimit->limitNo;
                usage->rsrcCnt = limitConf->nResources;
                usage->rsrcAccounts = (RL_RESOURCE_ACCOUNT_T *)my_calloc(
                        limitConf->nResources, sizeof(RL_RESOURCE_ACCOUNT_T),
                        fname);

                for (j = 0; j < limitConf->nResources; j++) {
                    RL_RESOURCE_T *rsrc = &limitConf->resources[j];
                    RL_RESOURCE_AVAIL_T *rsrcAvail = &cacheLimit->rsrcAvails[j];

                    usage->rsrcAccounts[j].name = rsrc->resName;
                    usage->rsrcAccounts[j].resNo = rsrc->resNo;
                    usage->rsrcAccounts[j].limit = getRsrcLimitValue(limitConf, rsrc, hostVal ? getHostData(hostVal): NULL);
                    usage->rsrcAccounts[j].used = 0.0;

                    /*calculate used amount*/
                    switch (generalRLConf.limits[cacheLimit->limitNo].resources[j].type) {
                        case RL_RESOURCE_TYPE_JOBS:
                            usage->rsrcAccounts[j].used = 1.0;
                            break;
                        case RL_RESOURCE_TYPE_SLOTS:
                        case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
                            usage->rsrcAccounts[j].used = (cacheLimit->rsrcAvails[j].orgAvail - cacheLimit->rsrcAvails[j].avail);
                            break;
                        case RL_RESOURCE_TYPE_MEM:
                        case RL_RESOURCE_TYPE_SWP:
                        case RL_RESOURCE_TYPE_TMP:
                        case RL_RESOURCE_TYPE_RSRC:
                            if (jobResVal != NULL) {
                                TEST_BIT(rsrcAvail->resNo, jobResVal->rusgBitMaps,
                                        rsrcIsSet);
                            }
                            if (!rsrcIsSet) {
                                /* Job did not request this resource, no deduction */
                                break;
                            }
                            switch (getRsrcReserveType(rsrcAvail->resNo)) {
                                case RSRC_PER_JOB:
                                    usage->rsrcAccounts[j].used = jobResVal->val[rsrcAvail->resNo];
                                    break;
                                case RSRC_PER_HOST:
                                case RSRC_PER_TASK:
                                    usage->rsrcAccounts[j].used = (cacheLimit->rsrcAvails[j].orgAvail - cacheLimit->rsrcAvails[j].avail);
                                    break;
                                default:
                                    break;
                            }
                            break;
                        default:
                            break;
                    }
                }

                /* Add to rlAccountTab */
                acctEnt = h_addEnt_(&rlAccountTab, keyBuf, &newEnt);
                if (acctEnt != NULL) {
                    acctEnt->hData = (void *)usage;
                    cacheLimit->usageEnt = acctEnt;
                    generalRLConf.limits[cacheLimit->limitNo].refer++;
                } else {
                    /* Failed to add, free the usage we created */
                    FREEUP(usage->rsrcAccounts);
                    FREEUP(usage);
                }
            }
        }
    }
} /*updRLAccountTabByCache*/

/********************************************************************************
 * collectRunJobRLimits4Acct
 * Description:
 *     collect all resource limits that match the job into a temporary
 *     RL_JOB_LIMITS_CACHE_T. The cache is used for updating rlAccountTab usage
 *     records outside the scheduling session (e.g. when a running job's
 *     resource usage changes).
 *
 *     Unlike the scheduling-time cache (jobRLimitsCache), this function:
 *       - Builds its own local bitmap via getRLBitmap4Job() (host dimension uses
 *         NULL so that all host-consumer limits are considered; per-host
 *         matching is refined per execution host below)
 *       - Collects mainRLimits: limits without a host consumer dimension
 *       - Collects hostRLimits: limits with a host consumer dimension, by
 *         iterating over the job's execution hosts (jp->hPtr, which is
 *         slot-expanded: the same host appears in consecutive positions, one
 *         entry per slot). For each unique execution host, host-dimension
 *         bitmap is regenerated and AND-ed with the other dimensions to find
 *         matching limits. SHARED-mode host limits are merged across hosts
 *         with hostRef.numRef counting the number of hosts (reference count),
 *         mirroring mergeAllocRLimit2Cache() but WITHOUT deducting avail.
 *       - The rsrcAvails fields (orgAvail/avail) are populated by
 *         getJobRLimitOnHost() but are NOT used by the caller; they are
 *         retained only because getJobRLimitOnHost() fills them.
 *
 *     The caller must release outCache with freeRLJobLimitsCache() when done.
 *
 * Algorithm:
 *     1. If no limits configured or jp has no execution hosts, return
 *     2. Build base bitmap via getRLBitmap4Job(jp, NULL); AND all consumer
 *        dimensions to find matching limits
 *     3. Collect mainRLimits: for each matching limit whose host consumer
 *        is not configured, build an RL_ALLOC_RLIMIT_T (limitNo, rsrcCnt,
 *        usage from rlAccountTab) and append to outCache->mainRLimits
 *     4. Collect hostRLimits: iterate jp->hPtr to enumerate unique execution
 *        hosts and their slot counts (consecutive duplicate entries => same
 *        host). For each unique host:
 *        a. Update host-dimension bitmap via getRLBitmap()
 *        b. AND all dimensions
 *        c. Call getJobRLimitOnHost() to build per-host hostRLimits
 *        d. Merge into outCache->hostRLimits:
 *           - For SHARED-mode limits already in the cache, increment
 *             hostRef.numRef (another host contributes to this shared limit)
 *           - For PER-mode or not-yet-cached SHARED limits, move the entry
 *             into outCache->hostRLimits
 *
 * Input:
 *     jp [in]: Job data pointer (must be a running job with jp->hPtr filled)
 *
 * Output:
 *     outCache [out]: Caller-allocated RL_JOB_LIMITS_CACHE_T. Its mainRLimits
 *                     and hostRLimits lists are populated. Caller must call
 *                     freeRLJobLimitsCache(outCache) to release.
 *
 * Return:
 *     None
 ********************************************************************************/
static void
collectRunJobRLimits4Acct(struct jData *jp, RL_JOB_LIMITS_CACHE_T *outCache)
{
    static char fname[] = "collectRunJobRLimits4Acct";
    int *bitmap[RL_CONSUMER_POSITION_MAX] = {0};
    int *andBitmap = NULL;
    int i, j;

    if (jp == NULL || outCache == NULL || generalRLConf.nLimits <= 0
        || jp->numHostPtr <= 0 || jp->hPtr == NULL) {
        return;
    }

    memset(outCache, 0, sizeof(*outCache));

    /* Step 1: Build base bitmap with host dimension = NULL (matches all
     * host consumers). We will refine per-host below. Rows are cache-owned
     * (borrowed); copy the row pointers into outCache->rlBitmaps. */
    if (getRLBitmap4Job(jp, NULL, bitmap) < 0) {
        return;
    }
    memcpy(outCache->rlBitmaps, bitmap, sizeof(outCache->rlBitmaps));

    /* Step 2: AND all consumer dimensions to find matching limits
     * (reusable static buffer) */
    andBitmap = getRLAndBitmap();
    if (andBitmap == NULL) {
        return;
    }
    for (j = 0; j < rlBitmapSize; j++) {
        andBitmap[j] = bitmap[0][j];
    }
    for (i = 1; i < RL_CONSUMER_POSITION_MAX; i++) {
        for (j = 0; j < rlBitmapSize; j++) {
            andBitmap[j] &= bitmap[i][j];
        }
    }

    /* Step 3: Collect mainRLimits (limits without a host consumer dimension).
     * Since host consumer is not configured, the limit is shared across all
     * execution hosts. We need the unique execution host count to set
     * hostRef.numRef. jp->hPtr is slot-expanded (same host in consecutive
     * positions), so we deduplicate adjacent entries to count unique hosts. */
    {
        int hIdx;
        int numExecHosts = 0;
        int numRealSlots = 0;
        for (hIdx = 0; hIdx < jp->numHostPtr; ) {
            struct hData *hp = jp->hPtr[hIdx];
            if (hp != NULL) {
                int nSlots = 0;
                while (hIdx < jp->numHostPtr && jp->hPtr[hIdx] == hp) {
                    nSlots++;
                    hIdx++;
                }
                /* Skip lost_and_found host: it is a virtual placeholder,
                 * not a real execution host contributing to limits. */
                if (!(hp->flags & HOST_LOST_FOUND)) {
                    numExecHosts++;
                    numRealSlots += nSlots;
                }
            } else {
                hIdx++;
            }
        }

        outCache->mainRLimits = listCreate("mainRLimits4Acct");
        for (i = 0; i < generalRLConf.nLimits; i++) {
            int isSet = FALSE;
            RL_LIMIT_T *limit;
            char keyBuf[RL_KEY_BUF_LEN];
            hEnt *acctEnt = NULL;
            RL_ALLOC_RLIMIT_T *curAllocLimit = NULL;

            TEST_BIT(i, andBitmap, isSet);
            if (!isSet) {
                continue;
            }

            limit = &generalRLConf.limits[i];

            /* Look up active usage in rlAccountTab (host is SHARED/empty here) */
            if (getLimitKey4Job(limit, jp, NULL, keyBuf, sizeof(keyBuf)) == 0) {
                acctEnt = h_getEnt_(&rlAccountTab, keyBuf);
            }

            curAllocLimit = (RL_ALLOC_RLIMIT_T *)my_calloc(1,
                                sizeof(RL_ALLOC_RLIMIT_T), fname);
            curAllocLimit->limitNo = i;
            curAllocLimit->rsrcCnt = limit->nResources;
            curAllocLimit->usageEnt = acctEnt;
            /* Host consumer not configured: the limit is shared across all
             * execution hosts, so hostRef.numRef = number of unique hosts. */
            curAllocLimit->hostRef.numRef = numExecHosts;
            /* accumSlots = job's total slot usage on real hosts (excludes
             * lost_and_found slots). hPtr is slot-expanded. */
            curAllocLimit->accumSlots = numRealSlots;
            /* rsrcAvails not needed for account update; leave as NULL */
            listInsertEntryAtBack(outCache->mainRLimits, (LIST_ENTRY_T *)curAllocLimit);
        }
    }

    /* andBitmap (static buffer) is fully consumed by Step 3 above; it is
     * reused below for the per-host AND result. Do not free. */

    /* Step 4: Collect hostRLimits by iterating over execution hosts.
     * jp->hPtr is slot-expanded: the same host occupies consecutive
     * positions, one per slot. We deduplicate adjacent entries to get
     * unique hosts and their slot counts. */
    outCache->hostRLimits = listCreate("hostRLimits4Acct");

    {
        int hIdx = 0;
        while (hIdx < jp->numHostPtr) {
            struct hData *hp = jp->hPtr[hIdx];
            int nSlots = 1;
            int *hostBitmap = NULL;
            LIST_T *oneHostRLimits = NULL;
            LIST_ITERATOR_T iter;
            LIST_ENTRY_T *ent;

            if (hp == NULL) {
                hIdx++;
                continue;
            }

            /* Count consecutive slots on the same host */
            while (hIdx + nSlots < jp->numHostPtr
                   && jp->hPtr[hIdx + nSlots] == hp) {
                nSlots++;
            }

            /* Skip lost_and_found host: it is a virtual placeholder, not a
             * real execution host; its slots must not contribute to any
             * host-consumer limit. */
            if (hp->flags & HOST_LOST_FOUND) {
                hIdx += nSlots;
                continue;
            }

            /* Refine host-dimension bitmap for this host */
            hostBitmap = getRLBitmap(hp, RL_CONSUMER_POSITION_HOST);
            if (hostBitmap == NULL) {
                hIdx += nSlots;
                continue;
            }
            bitmap[RL_CONSUMER_POSITION_HOST] = hostBitmap;

            /* AND all dimensions for this host; reuse the static buffer.
             * getJobRLimitOnHost() only reads it via TEST_BIT, so reuse
             * across hosts is safe. */
            for (j = 0; j < rlBitmapSize; j++) {
                andBitmap[j] = bitmap[0][j];
            }
            for (i = 1; i < RL_CONSUMER_POSITION_MAX; i++) {
                for (j = 0; j < rlBitmapSize; j++) {
                    andBitmap[j] &= bitmap[i][j];
                }
            }

            /* Collect host-consumer limits matching on this host. Reuse
             * getJobRLimitOnHost() which fills limitNo/rsrcCnt/usage/
             * hostRef/rsrcAvails. The rsrcAvails values are not used by
             * the caller but are harmless. */
            oneHostRLimits = getJobRLimitOnHost(jp, hp, andBitmap);

            /* Merge oneHostRLimits into outCache->hostRLimits.
             * For SHARED-mode limits already present in the cache, increment
             * hostRef.numRef and free the duplicate entry. For PER-mode or
             * not-yet-cached SHARED limits, move the entry into the cache. */
            if (oneHostRLimits != NULL && !LIST_IS_EMPTY(oneHostRLimits)) {
                LIST_ENTRY_T *nextEnt = NULL;

                LIST_ITERATOR_ZERO_OUT(&iter);
                listIteratorAttach(&iter, oneHostRLimits);

                for (ent = listIteratorGetCurEntry(&iter);
                     ent != NULL && !listIteratorIsEndOfList(&iter);
                     ent = nextEnt) {
                    RL_ALLOC_RLIMIT_T *hLimit = (RL_ALLOC_RLIMIT_T *)ent;
                    RL_LIMIT_T *hLimitConf =
                        &generalRLConf.limits[hLimit->limitNo];
                    int hostMode =
                        hLimitConf->consumers[RL_CONSUMER_POSITION_HOST].mode;
                    bool_t found = FALSE;
                    LIST_ITERATOR_T cacheIter;
                    LIST_ENTRY_T *cacheEnt;

                    /* Advance iterator BEFORE any potential listRemoveEntry
                     * below. listIteratorNext() reads iter->curEnt->forw to
                     * advance; if we remove+reinsert the current entry first,
                     * ent->forw gets rewired into outCache->hostRLimits and
                     * the iterator would follow into the wrong list. */
                    listIteratorNext(&iter, &nextEnt);

                    if (hostMode == RL_CONSUMER_MODE_SHARED
                        && !LIST_IS_EMPTY(outCache->hostRLimits)) {
                        LIST_ITERATOR_ZERO_OUT(&cacheIter);
                        listIteratorAttach(&cacheIter, outCache->hostRLimits);

                        for (cacheEnt = listIteratorGetCurEntry(&cacheIter);
                             cacheEnt != NULL
                                 && !listIteratorIsEndOfList(&cacheIter);
                             listIteratorNext(&cacheIter, &cacheEnt)) {
                            RL_ALLOC_RLIMIT_T *cacheLimit =
                                (RL_ALLOC_RLIMIT_T *)cacheEnt;

                            if (cacheLimit->limitNo != hLimit->limitNo) {
                                continue;
                            }
                            /* SHARED limit already in cache: increment the
                             * reference count (one more host contributes) and
                             * accumulate this host's slots into the shared
                             * total. */
                            cacheLimit->hostRef.numRef++;
                            cacheLimit->accumSlots += nSlots;
                            found = TRUE;
                            break;
                        }
                    }

                    if (!found) {
                        /* Not in cache: move entry from oneHostRLimits into
                         * outCache->hostRLimits. For SHARED mode, numRef was
                         * initialized to 1 by getJobRLimitOnHost(); we
                         * initialize accumSlots to this host's slots and
                         * subsequent hosts will accumulate via the branch
                         * above. For PER mode, accumSlots is just this
                         * single host's slots. */
                        hLimit->accumSlots = nSlots;
                        listRemoveEntry(oneHostRLimits, ent);
                        listInsertEntryAtBack(outCache->hostRLimits, (LIST_ENTRY_T *)hLimit);
                    }
                }
            }

            /* Free any remaining entries in oneHostRLimits (duplicates that
             * were not moved into the cache) */
            if (oneHostRLimits != NULL) {
                listDestroy(oneHostRLimits, freeRLAllocLimitEntry);
            }

            hIdx += nSlots;
        }
    }

    return;
} /*collectJobRLimits4Acct*/

/********************************************************************************
 * updRLAccount4Job
 *
 * Add the job's resource consumption to matching limit account records in
 * rlAccountTab. Inverse of cleanRLAccount4Job().
 *
 * Input:
 *   jp [in]: Running job which we will handle.
 *   caller [in]: Caller function name, used for logging.
 *
 * Return: None
 ********************************************************************************/
void updRLAccount4Job(struct jData *jp, char *caller)
{
    static char fname[] = "updRLAccount4Job";
    RL_JOB_LIMITS_CACHE_T tmpCache = {{0}};
    LIST_T *lists[2];
    int nLists;
    int j;
    struct resVal *jobResVal = NULL;

    if (jp == NULL) {
        return;
    }

    if (generalRLConf.nLimits <= 0) {
        return;
    }

    if ((logclass & LC_JLIMIT) && (caller != NULL)) {
        ls_syslog(LOG_DEBUG, "\
%s: %s: add resource limit account for job <%s>", fname, caller, lsb_jobid2str(jp->jobId));
    }

    if (jp->effeResReqEnt != NULL) {
        jobResVal = GET_JOB_EFFE_RES_REQ(jp);
    }

    collectRunJobRLimits4Acct(jp, &tmpCache);
    /* Step 2: Traverse both mainRLimits and hostRLimits, update usage */
    lists[0] = tmpCache.mainRLimits;
    lists[1] = tmpCache.hostRLimits;

    for (nLists = 0; nLists < 2; nLists++) {
        LIST_T *curList = lists[nLists];
        LIST_ITERATOR_T iter;
        LIST_ENTRY_T *ent;

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)ent;
            RL_LIMIT_T *limitConf;
            RL_USAGE_T *usage = (cacheLimit->usageEnt != NULL)
                ? (RL_USAGE_T *)cacheLimit->usageEnt->hData : NULL;

            limitConf = &generalRLConf.limits[cacheLimit->limitNo];

            if (usage != NULL) {
                /* Usage record already exists, update used values */
                for (j = 0; j < limitConf->nResources && j < usage->rsrcCnt; j++) {
                    RL_RESOURCE_T *rsrc = &limitConf->resources[j];
                    float consume = calcJobRsrcConsume(limitConf, rsrc,
                                                       cacheLimit, jobResVal);
                    usage->rsrcAccounts[j].used += consume;
                }
            } else {
                /* No usage record, create a new one and add to rlAccountTab */
                char keyBuf[RL_KEY_BUF_LEN];
                hEnt *acctEnt = NULL;
                int newEnt;
                char *hostVal = NULL;

                if (limitConf->consumers[RL_CONSUMER_POSITION_HOST].mode == RL_CONSUMER_MODE_PER) {
                    hostVal = cacheLimit->hostRef.hostname;
                }

                if (getLimitKey4Job(limitConf, jp, hostVal, keyBuf, sizeof(keyBuf)) != 0) {
                    continue;
                }

                /* Create new RL_USAGE_T */
                usage = (RL_USAGE_T *)my_calloc(1, sizeof(RL_USAGE_T),
                                                fname);
                usage->limitNo = cacheLimit->limitNo;
                usage->rsrcCnt = limitConf->nResources;
                usage->rsrcAccounts = (RL_RESOURCE_ACCOUNT_T *)my_calloc(
                        limitConf->nResources, sizeof(RL_RESOURCE_ACCOUNT_T),
                        fname);

                for (j = 0; j < limitConf->nResources; j++) {
                    RL_RESOURCE_T *rsrc = &limitConf->resources[j];

                    usage->rsrcAccounts[j].name = rsrc->resName;
                    usage->rsrcAccounts[j].resNo = rsrc->resNo;
                    usage->rsrcAccounts[j].limit = getRsrcLimitValue(limitConf,
                                                                     rsrc, hostVal ? getHostData(hostVal): NULL);
                    usage->rsrcAccounts[j].used = calcJobRsrcConsume(limitConf,
                                                                     rsrc,
                                                                     cacheLimit,
                                                                     jobResVal);
                }

                /* Add to rlAccountTab */
                acctEnt = h_addEnt_(&rlAccountTab, keyBuf, &newEnt);
                if (acctEnt != NULL) {
                    acctEnt->hData = (void *)usage;
                    cacheLimit->usageEnt = acctEnt;
                    generalRLConf.limits[cacheLimit->limitNo].refer++;
                } else {
                    /* Failed to add, free the usage we created */
                    FREEUP(usage->rsrcAccounts);
                    FREEUP(usage);
                }
            }
        }
    }

    freeRLJobLimitsCache(&tmpCache);
} /*updRLAccount4Job*/

/********************************************************************************
 * cleanRLAccount4Job
 * Description:
 *     Subtract the job's resource consumption from the matching limit account
 *     records in rlAccountTab. This is the inverse of updRLAccount4Job() and
 *     is called when a job finishes or is removed, so that the limit account
 *     records no longer count this job's usage.
 *
 *     For each matching limit that has an RL_USAGE_T record in rlAccountTab:
 *       - For each resource in the limit, compute the job's consumption
 *         (same formula as updRLAccount4Job / mergeAllocRLimit2Cache) and
 *         subtract it from usage->rsrcAccounts[j].used.
 *       - After subtraction, if all resources' used values are 0 (within a
 *         small epsilon to tolerate float drift), remove the RL_USAGE_T
 *         record from rlAccountTab and free it.
 *
 *     Limits without an RL_USAGE_T record are skipped.
 *
 * Algorithm:
 *     1. If no limits configured, return
 *     2. Call collectRunJobRLimits4Acct() to gather matching limits into a
 *        temporary cache (mainRLimits + hostRLimits)
 *     3. Traverse both lists. For each limit with a non-NULL usage:
 *        a. For each resource in the limit, compute the job's consumption
 *           and subtract it from usage->rsrcAccounts[j].used
 *        b. After all resources are updated, check if all used values are 0.
 *           If so, look up the hash entry and remove it from rlAccountTab
 *           (freeing the RL_USAGE_T and its rsrcAccounts array).
 *     4. Free the temporary cache
 *
 *     Resource consumption formula (mirrors updRLAccount4Job):
 *       - JOBS:                   1
 *       - SLOTS / SLOTS_PER_PROC: accumSlots
 *       - MEM/SWP/TMP/RSRC:       jobDemand scaled by reserve type
 *           PER_JOB:  jobDemand
 *           PER_HOST: jobDemand * numHosts   (if host consumer not PER mode)
 *                     jobDemand               (if host consumer is PER mode)
 *           PER_TASK: jobDemand * accumSlots
 *       where jobDemand = jobResVal->val[resNo] (0 if job did not request
 *       the resource).
 *
 * Input:
 *     jp [in]: Job data pointer (must be a running job with jp->hPtr filled)
 *
 * Return:
 *     None
 ********************************************************************************/
 void
cleanRLAccount4Job(struct jData *jp, char *caller)
{
    static char fname[] = "cleanRLAccount4Job";
    RL_JOB_LIMITS_CACHE_T tmpCache;
    LIST_T *lists[2];
    int nLists;
    int j;
    struct resVal *jobResVal = NULL;

    if (jp == NULL) {
        return;
    }

    if (generalRLConf.nLimits <= 0) {
        return;
    }

    if (jp->effeResReqEnt != NULL) {
        jobResVal = GET_JOB_EFFE_RES_REQ(jp);
    }

    if (logclass & LC_JLIMIT) {
        ls_syslog(LOG_DEBUG, "\
%s: %s: reduce resource limit account for job <%s>", fname, caller ? caller : "unknow", lsb_jobid2str(jp->jobId));
    }

    /* Step 1: Collect matching limits into a temporary cache */
    collectRunJobRLimits4Acct(jp, &tmpCache);

    /* Step 2: Traverse both mainRLimits and hostRLimits, subtract the job's
     * consumption from each limit's usage record. */
    lists[0] = tmpCache.mainRLimits;
    lists[1] = tmpCache.hostRLimits;

    for (nLists = 0; nLists < 2; nLists++) {
        LIST_T *curList = lists[nLists];
        LIST_ITERATOR_T iter;
        LIST_ENTRY_T *ent;

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)ent;
            RL_LIMIT_T *limitConf;
            RL_USAGE_T *usage = (cacheLimit->usageEnt != NULL)
                ? (RL_USAGE_T *)cacheLimit->usageEnt->hData : NULL;
            int allZero = TRUE;

            if (usage == NULL) {
                /* No active usage record, nothing to clean */
                continue;
            }

            limitConf = &generalRLConf.limits[cacheLimit->limitNo];

            /* Step 2a: Subtract the job's consumption from each resource */
            for (j = 0; j < limitConf->nResources && j < usage->rsrcCnt; j++) {
                RL_RESOURCE_T *rsrc = &limitConf->resources[j];
                float consume = calcJobRsrcConsume(limitConf, rsrc,
                                                   cacheLimit, jobResVal);

                usage->rsrcAccounts[j].used -= consume;
                /* Clamp tiny negative values produced by float drift to 0 */
                if (usage->rsrcAccounts[j].used < 0.0) {
                    usage->rsrcAccounts[j].used = 0.0;
                }
                if (usage->rsrcAccounts[j].used != 0.0) {
                    allZero = FALSE;
                }
            }

            /* Step 2b: If all resources' used values are 0, remove the
             * RL_USAGE_T record from rlAccountTab and free it. We already
             * hold the hash entry via cacheLimit->usageEnt, so no need to
             * regenerate the key and look it up. */
            if (allZero) {
                hEnt *acctEnt = cacheLimit->usageEnt;
                if (acctEnt != NULL) {
                    /* Detach hData so h_delEnt_ does not free it (we free
                     * rsrcAccounts and the struct ourselves). */
                    acctEnt->hData = NULL;
                    h_delEnt_(&rlAccountTab, acctEnt);
                    if (generalRLConf.limits[cacheLimit->limitNo].refer > 0) {
                        generalRLConf.limits[cacheLimit->limitNo].refer--;
                    }
                }
                /* Free the RL_USAGE_T and its rsrcAccounts array */
                FREEUP(usage->rsrcAccounts);
                FREEUP(usage);
                cacheLimit->usageEnt = NULL;
            }
        }
    }

    /* Step 3: Free the temporary cache */
    freeRLJobLimitsCache(&tmpCache);

} /*cleanRLAccount4Job*/


/********************************************************************************
 * calcJobRsrcConsume
 *
 * Compute a job's consumption for a single resource of a matched limit.
 * Extracted as a helper so that updRLAccount4Job / cleanRLAccount4Job /
 * checkRLimits4RunJob share one formula.
 *
 * Input:
 *   limitConf  [in]: pointer to the RL_LIMIT_T configuration
 *   rsrc       [in]: pointer to the RL_RESOURCE_T in limitConf->resources[j]
 *   cacheLimit [in]: the matched RL_ALLOC_RLIMIT_T (provides accumSlots /
 *                    hostRef)
 *   jobResVal  [in]: the job's merged resVal (may be NULL)
 *
 * Return: the consumption amount for this resource (0 if the job did not
 *         request the resource).
 ********************************************************************************/
static float
calcJobRsrcConsume(const RL_LIMIT_T *limitConf,
                   const RL_RESOURCE_T *rsrc,
                   const RL_ALLOC_RLIMIT_T *cacheLimit,
                   const struct resVal *jobResVal)
{
    int hostMode = limitConf->consumers[RL_CONSUMER_POSITION_HOST].mode;
    int rsrcIsSet = FALSE;
    int reserveType;
    float jobDemand;

    switch (rsrc->type) {
        case RL_RESOURCE_TYPE_JOBS:
            return 1.0;

        case RL_RESOURCE_TYPE_SLOTS:
        case RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR:
            return (float)cacheLimit->accumSlots;

        case RL_RESOURCE_TYPE_MEM:
        case RL_RESOURCE_TYPE_SWP:
        case RL_RESOURCE_TYPE_TMP:
        case RL_RESOURCE_TYPE_RSRC:
            if (jobResVal != NULL) {
                TEST_BIT(rsrc->resNo, jobResVal->rusgBitMaps, rsrcIsSet);
            }
            if (!rsrcIsSet) {
                return 0.0;
            }
            jobDemand = jobResVal->val[rsrc->resNo];
            reserveType = getRsrcReserveType(rsrc->resNo);
            switch (reserveType) {
                case RSRC_PER_JOB:
                    return jobDemand;
                case RSRC_PER_HOST:
                    if (hostMode == RL_CONSUMER_MODE_PER) {
                        return jobDemand;
                    }
                    /* SHARED host or no host consumer */
                    return jobDemand * (float)cacheLimit->hostRef.numRef;
                case RSRC_PER_TASK:
                    return jobDemand * (float)cacheLimit->accumSlots;
                default:
                    return 0.0;
            }

        default:
            return 0.0;
    }
} /*calcJobRsrcConsume*/

/********************************************************************************
 * checkRLimits4RunJob
 *
 * Check whether a bmod on a started job's rusage still satisfies all matching
 * resource limits. Called before applying the modification so that an
 * over-limit request is rejected with LSBE_MOD_JLIMIT.
 *
 * The job's execution hosts (jp->hPtr) and slot layout are assumed unchanged
 * by the bmod; only the rusage values differ. Therefore limits are collected
 * using orgJob, and the consumption is computed twice (orgResVal and
 * newResVal) against the same cacheLimit (accumSlots / hostRef).
 *
 * Two cases per matched limit:
 *   (a) Has an RL_USAGE_T record (running job already accounted):
 *       usedLimit = usage->used - orgConsume + newConsume
 *       must be <= usage->limit.
 *   (b) No RL_USAGE_T record (e.g. limit just added after job start, or
 *       account evicted): fall back to the limit configuration directly.
 *       newConsume (computed from newJob) must be <= the configured limit
 *       value, resolved via getRsrcLimitValue() so percentage limits under
 *       per-host mode are converted.
 *
 * Algorithm:
 *   1. Return LSBE_NO_ERROR early if jp is NULL, not started, or no limits
 *      configured.
 *   2. Resolve orgResVal from orgJob; newResVal from newJob.
 *   3. collectRunJobRLimits4Acct(orgJob, &tmpCache).
 *   4. For each limit in mainRLimits + hostRLimits:
 *      - If usageEnt != NULL: for each resource, check
 *        (used - orgConsume + newConsume) <= usage->limit.
 *      - Otherwise: for each resource, check newConsume <= configured limit
 *        (getRsrcLimitValue).
 *      Reject on the first violation.
 *   5. freeRLJobLimitsCache(&tmpCache); return LSBE_NO_ERROR if all pass.
 *
 * Input:
 *   orgJob [in]: the currently running job (provides hPtr, slot layout, and
 *                the original rusage via mergedResReqEnt). Also used to
 *                resolve per-host capacities for percentage limits.
 *   newJob [in]: the bmod-constructed job carrying the new rusage via
 *                mergedResReqEnt. Its hPtr is not used.
 *
 * Return:
 *   LSBE_NO_ERROR    - all limits satisfied
 *   LSBE_MOD_JLIMIT  - at least one limit would be exceeded
 ********************************************************************************/
int
checkRLimits4Bmod(struct jData *orgJob, struct jData *newJob)
{
    static char fname[] = "checkRLimits4RunJob";
    RL_JOB_LIMITS_CACHE_T tmpCache;
    LIST_T *lists[2];
    int nLists;
    struct resVal *orgResVal = NULL;
    struct resVal *newResVal = NULL;
    int rejected = FALSE;

    if (orgJob == NULL || newJob == NULL) {
        return LSBE_NO_ERROR;
    }

    /* (1) Only started jobs need the check */
    if (!IS_START(orgJob->jStatus)) {
        return LSBE_NO_ERROR;
    }

    /*only do checking in volclava runtime*/
    if (mSchedStage == M_STAGE_REPLAY) {
        return LSBE_NO_ERROR;
    }

    if (generalRLConf.nLimits <= 0) {
        return LSBE_NO_ERROR;
    }

    if (orgJob->effeResReqEnt != NULL) {
        orgResVal = GET_JOB_EFFE_RES_REQ(orgJob);
    }

    newResVal = newJob->shared->resValPtr;

    /* (2) Collect matched limits using orgJob's host/slot layout */
    collectRunJobRLimits4Acct(orgJob, &tmpCache);

    lists[0] = tmpCache.mainRLimits;
    lists[1] = tmpCache.hostRLimits;

    for (nLists = 0; nLists < 2 && !rejected; nLists++) {
        LIST_T *curList = lists[nLists];
        LIST_ITERATOR_T iter;
        LIST_ENTRY_T *ent;

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)ent;
            RL_LIMIT_T *limitConf;
            RL_USAGE_T *usage = NULL;
            int hasUsage = FALSE;
            int j;

            limitConf = &generalRLConf.limits[cacheLimit->limitNo];

            if (cacheLimit->usageEnt != NULL) {
                usage = (RL_USAGE_T *)cacheLimit->usageEnt->hData;
                hasUsage = (usage != NULL);
            }

            if (!hasUsage) {
                /* Log: running job without an account record is unexpected
                 * but we still verify against the configured limit. */
                ls_syslog(LOG_INFO, _i18n_msg_get(ls_catd, NL_SETN, 0,
                            "%s: job <%s> limit <%s> has no usage record, "
                            "falling back to configured limit"),
                          fname, lsb_jobid2str(orgJob->jobId),
                          limitConf->name ? limitConf->name : "");
            }

            /* Check each resource against the appropriate limit. */
            for (j = 0; j < limitConf->nResources; j++) {
                RL_RESOURCE_T *rsrc = &limitConf->resources[j];
                float newConsume = calcJobRsrcConsume(limitConf, rsrc,
                                                      cacheLimit, newResVal);
                float usedLimit;
                float effectiveLimit;

                if (hasUsage) {
                    /* (a) Account record exists: subtract the orgConsume
                     *     already counted and add newConsume. */
                    float orgConsume;
                    if (j >= usage->rsrcCnt) {
                        continue;
                    }
                    orgConsume = calcJobRsrcConsume(limitConf, rsrc,
                                                          cacheLimit,
                                                          orgResVal);
                    usedLimit = usage->rsrcAccounts[j].used
                                - orgConsume + newConsume;
                    effectiveLimit = usage->rsrcAccounts[j].limit;
                } else {
                    /* (b) No account record: compare newConsume directly
                     *     against the configured limit. Resolve percentage
                     *     limits using the first execution host. */
                    usedLimit = newConsume;
                    if (limitConf->consumers[RL_CONSUMER_POSITION_HOST].mode == RL_CONSUMER_MODE_PER) {
                        effectiveLimit = getRsrcLimitValue(limitConf, rsrc, getHostData(cacheLimit->hostRef.hostname));
                    } else {
                        effectiveLimit = getRsrcLimitValue(limitConf, rsrc, NULL);
                    }
                }

                if (usedLimit > effectiveLimit) {
                    if (logclass & LC_JLIMIT) {
                        ls_syslog(LOG_DEBUG1,
                                  "%s: job <%s> limit <%s> resource <%s> "
                                  "usedLimit %.2f > limit %.2f "
                                  "(hasUsage=%d, used=%.2f)",
                                  fname, lsb_jobid2str(orgJob->jobId),
                                  limitConf->name ? limitConf->name : "",
                                  rsrc->resName ? rsrc->resName : "",
                                  usedLimit, effectiveLimit,
                                  hasUsage,
                                  hasUsage ? usage->rsrcAccounts[j].used
                                           : 0.0);
                    }
                    rejected = TRUE;
                    break;
                }
            }

            if (rejected) {
                break;
            }
        }
    }

    freeRLJobLimitsCache(&tmpCache);
    return rejected ? LSBE_MOD_JLIMIT : LSBE_NO_ERROR;
} /*checkRLimits4RunJob*/


/********************************************************************************
 * checkRLimits4Switch
 *
 * Check whether switching a started job to a new queue still satisfies all
 * resource limits that are newly introduced by the new queue. When a job is
 * switched via bswitch, the new queue may cause additional limits to match
 * (e.g. queue-scoped limits configured for the destination queue) that did
 * not apply under the original queue. Those newly-matched limits must have
 * enough headroom for the job's current resource consumption, otherwise the
 * switch is rejected with LSBE_MOD_JLIMIT.
 *
 * Limits that already matched under the original queue are NOT re-checked
 * here: their usage records already account for this job and the switch
 * does not change the job's own consumption.
 *
 * Algorithm:
 *   1. Return LSBE_NO_ERROR early if job is NULL, not started, qtp is NULL,
 *      or no limits configured.
 *   2. Collect matched limits under the original queue into oldCache.
 *   3. Temporarily swap job->qPtr to qtp, collect matched limits into
 *      newCache, then restore job->qPtr.
 *   4. Build diffCache containing only limits present in newCache but NOT
 *      in oldCache (matched by limitNo). Ownership of the moved
 *      RL_ALLOC_RLIMIT_T entries is transferred to diffCache.
 *   5. Resolve the job's effective resVal from mergedResReqEnt.
 *   6. For each limit in diffCache and each resource:
 *        - If a usage record exists: used + consume must be <= usage->limit.
 *        - If no usage record: consume must be <= configured limit value
 *          (resolved via getRsrcLimitValue for percentage handling).
 *      On any violation return LSBE_MOD_JLIMIT immediately.
 *   7. All passed: return LSBE_NO_ERROR.
 *
 * Input:
 *   job  - the job being switched (must be a started job)
 *   qtp  - the destination queue
 *
 * Return:
 *   LSBE_NO_ERROR     - all newly-matched limits are satisfied
 *   LSBE_MOD_JLIMIT - at least one newly-matched limit would be exceeded
 ********************************************************************************/
int
checkRLimits4Switch(struct jData *job, struct qData *qtp)
{
    static char fname[] = "checkRLimits4Switch";
    RL_JOB_LIMITS_CACHE_T oldCache;
    RL_JOB_LIMITS_CACHE_T newCache;
    RL_JOB_LIMITS_CACHE_T diffCache;
    LIST_T *lists[2];
    LIST_T *oldLists[2];
    int nLists;
    int rejected = FALSE;
    struct resVal *jobResVal = NULL;
    struct qData *origQPtr;

    if (job == NULL || qtp == NULL) {
        return LSBE_NO_ERROR;
    }

    /* (1) Only started jobs need the check */
    if (!IS_START(job->jStatus)) {
        return LSBE_NO_ERROR;
    }

    /*only do checking in volclava runtime*/
    if (mSchedStage == M_STAGE_REPLAY) {
        return LSBE_NO_ERROR;
    }

    if (generalRLConf.nLimits <= 0) {
        return LSBE_NO_ERROR;
    }

    /* (2) Collect matched limits under the original queue */
    memset(&oldCache, 0, sizeof(oldCache));
    collectRunJobRLimits4Acct(job, &oldCache);

    /* (3) Collect matched limits under the destination queue. Swap qPtr
     *     temporarily so that getRLBitmap4Job() picks up the new queue name. */
    memset(&newCache, 0, sizeof(newCache));
    origQPtr = job->qPtr;
    job->qPtr = qtp;
    collectRunJobRLimits4Acct(job, &newCache);
    job->qPtr = origQPtr;

    /* (4) Build diffCache = newCache - oldCache (by limitNo).
     *     Iterate newCache's two lists; for each entry whose limitNo does
     *     not appear in oldCache's corresponding list, move it to diffCache. */
    memset(&diffCache, 0, sizeof(diffCache));
    diffCache.mainRLimits = listCreate("diffMainRLimits");
    diffCache.hostRLimits = listCreate("diffHostRLimits");

    oldLists[0] = oldCache.mainRLimits;
    oldLists[1] = oldCache.hostRLimits;
    lists[0] = newCache.mainRLimits;
    lists[1] = newCache.hostRLimits;

    for (nLists = 0; nLists < 2; nLists++) {
        LIST_T *curList = lists[nLists];
        LIST_T *oldList = oldLists[nLists];
        LIST_T *diffList = (nLists == 0) ? diffCache.mainRLimits
                                         : diffCache.hostRLimits;
        LIST_ITERATOR_T iter;
        LIST_ENTRY_T *ent;
        LIST_ENTRY_T *nextEnt = NULL;

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             ent = nextEnt) {
            RL_ALLOC_RLIMIT_T *newLimit = (RL_ALLOC_RLIMIT_T *)ent;
            int foundInOld = FALSE;

            listIteratorNext(&iter, &nextEnt);

            if (oldList != NULL && !LIST_IS_EMPTY(oldList)) {
                LIST_ITERATOR_T oldIter;
                LIST_ENTRY_T *oldEnt;

                LIST_ITERATOR_ZERO_OUT(&oldIter);
                listIteratorAttach(&oldIter, oldList);

                for (oldEnt = listIteratorGetCurEntry(&oldIter);
                     oldEnt != NULL && !listIteratorIsEndOfList(&oldIter);
                     listIteratorNext(&oldIter, &oldEnt)) {
                    RL_ALLOC_RLIMIT_T *oldLimit =
                        (RL_ALLOC_RLIMIT_T *)oldEnt;

                    if (oldLimit->limitNo == newLimit->limitNo
                        && oldLimit->usageEnt == newLimit->usageEnt) {
                        foundInOld = TRUE;
                        break;
                    }
                }
            }

            if (!foundInOld) {
                /* Move entry from newCache list to diffCache list.
                 * listRemoveEntry detaches without freeing. */
                listRemoveEntry(curList, ent);
                listInsertEntryAtBack(diffList, ent);
            }
        }
    }

    /* (5) Resolve the job's effective resVal from effeResReqEnt. */
    if (job->effeResReqEnt != NULL) {
        jobResVal = GET_JOB_EFFE_RES_REQ(job);
    }

    /* (6) Check each newly-matched limit */
    lists[0] = diffCache.mainRLimits;
    lists[1] = diffCache.hostRLimits;

    for (nLists = 0; nLists < 2 && !rejected; nLists++) {
        LIST_T *curList = lists[nLists];
        LIST_ITERATOR_T iter;
        LIST_ENTRY_T *ent;

        if (curList == NULL || LIST_IS_EMPTY(curList)) {
            continue;
        }

        LIST_ITERATOR_ZERO_OUT(&iter);
        listIteratorAttach(&iter, curList);

        for (ent = listIteratorGetCurEntry(&iter);
             ent != NULL && !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, &ent)) {
            RL_ALLOC_RLIMIT_T *cacheLimit = (RL_ALLOC_RLIMIT_T *)ent;
            RL_LIMIT_T *limitConf;
            RL_USAGE_T *usage = NULL;
            int hasUsage = FALSE;
            int j;

            limitConf = &generalRLConf.limits[cacheLimit->limitNo];

            if (cacheLimit->usageEnt != NULL) {
                usage = (RL_USAGE_T *)cacheLimit->usageEnt->hData;
                hasUsage = (usage != NULL);
            }

            if (!hasUsage) {
                /* Newly-matched limit has no prior usage record for this
                 * job; compare against the configured limit directly. */
                if (logclass & LC_JLIMIT) {
                    ls_syslog(LOG_DEBUG1, _i18n_msg_get(ls_catd, NL_SETN, 0,
                                "%s: job <%s> limit <%s> has no usage record, "
                                "falling back to configured limit"),
                            fname, lsb_jobid2str(job->jobId),
                            limitConf->name ? limitConf->name : "");
                }
            }

            for (j = 0; j < limitConf->nResources; j++) {
                RL_RESOURCE_T *rsrc = &limitConf->resources[j];
                float consume = calcJobRsrcConsume(limitConf, rsrc,
                                                   cacheLimit, jobResVal);
                float usedLimit;
                float effectiveLimit;

                if (hasUsage) {
                    if (j >= usage->rsrcCnt) {
                        continue;
                    }
                    /* The job's consumption is NOT yet counted in usage->used
                     * for a newly-matched limit (the limit did not match
                     * before the switch), so usedLimit = used + consume. */
                    usedLimit = usage->rsrcAccounts[j].used + consume;
                    effectiveLimit = usage->rsrcAccounts[j].limit;
                } else {
                    usedLimit = consume;
                    if (limitConf->consumers[RL_CONSUMER_POSITION_HOST].mode == RL_CONSUMER_MODE_PER) {
                        effectiveLimit = getRsrcLimitValue(limitConf, rsrc, getHostData(cacheLimit->hostRef.hostname));
                    } else {
                        effectiveLimit = getRsrcLimitValue(limitConf, rsrc, NULL);
                    }
                }

                if (usedLimit > effectiveLimit) {
                    if (logclass & LC_JLIMIT) {
                        ls_syslog(LOG_DEBUG1,
                                  "%s: job <%s> limit <%s> resource <%s> "
                                  "usedLimit %.2f > limit %.2f "
                                  "(hasUsage=%d, used=%.2f)",
                                  fname, lsb_jobid2str(job->jobId),
                                  limitConf->name ? limitConf->name : "",
                                  rsrc->resName ? rsrc->resName : "",
                                  usedLimit, effectiveLimit,
                                  hasUsage,
                                  hasUsage ? usage->rsrcAccounts[j].used
                                           : 0.0);
                    }
                    rejected = TRUE;
                    break;
                }
            }

            if (rejected) {
                break;
            }
        }
    }

    /* (7) Cleanup and return */
    freeRLJobLimitsCache(&oldCache);
    freeRLJobLimitsCache(&newCache);
    freeRLJobLimitsCache(&diffCache);

    return rejected ? LSBE_MOD_JLIMIT : LSBE_NO_ERROR;
} /*checkRLimits4Switch*/

/********************************************************************************
 * formatConsumerValue
 * Description:
 *     Format a consumer value for client display.
 *     Both modes tokenize the value list and append a trailing '/' to names
 *     that resolve as host/user groups.
 *     - configOnly (-c mode): wrap the tokenized value with MODE(...),
 *       e.g. "PER(hostA hostB/)" / "SHARED(groupX/)".
 *     - usage mode: PER-mode content is prefixed with '@', and if the
 *       result contains spaces it is wrapped in parentheses.
 *
 * Input:
 *     consumer  [in]: the consumer struct
 *     configOnly [in]: TRUE for -c mode (config display), FALSE for
 *                      usage display.
 *
 * Return:
 *     malloc'd formatted string (caller frees), or NULL if not configured.
 ********************************************************************************/
static char *
formatConsumerValue(const RL_CONSUMER_T *consumer, int configOnly)
{
    char buf[MAXLINELEN];
    char *sp;
    char *word;
    int first = TRUE;
    int bufLen = 0;
    char *result;
    const char *modeStr;

    if (consumer->value == NULL || consumer->value[0] == '\0') {
        return NULL;
    }

    modeStr = (consumer->mode == RL_CONSUMER_MODE_PER) ? "PER" : "SHARED";

    /* tokenize and detect group membership for '/' suffix */
    buf[0] = '\0';
    sp = consumer->value;
    while ((word = getNextWord_(&sp)) != NULL) {
        char nameBuf[MAXHOSTNAMELEN];
        char *name = word;
        int isNegated = FALSE;
        int isGroup = FALSE;

        if (name[0] == '~') {
            isNegated = TRUE;
            name++;
        }

        /* detect group membership */
        switch (consumer->type) {
        case RL_CONSUMER_TYPE_HOST:
            if (getHGrpData(name) != NULL) {
                isGroup = TRUE;
            }
            break;
        case RL_CONSUMER_TYPE_USER:
            if (getUGrpData(name) != NULL) {
                isGroup = TRUE;
            }
            break;
        default:
            break;
        }

        if (!first) {
            buf[bufLen++] = ' ';
        }
        first = FALSE;

        if (isNegated) {
            buf[bufLen++] = '~';
        }
        /* append name and optional '/' for group */
        snprintf(nameBuf, sizeof(nameBuf), "%s%s", name, isGroup ? "/" : "");
        bufLen += snprintf(buf + bufLen, sizeof(buf) - bufLen, "%s", nameBuf);
    }

    /* wrap content: configOnly -> MODE(...); usage -> @/(...) per mode */
    {
        char tmp[MAXLINELEN + 16];
        if (configOnly) {
            snprintf(tmp, sizeof(tmp), "%s(%s)", modeStr, buf);
        } else if (strchr(buf, ' ') != NULL) {
            if (consumer->mode == RL_CONSUMER_MODE_PER) {
                snprintf(tmp, sizeof(tmp), "@(%s)", buf);
            } else {
                snprintf(tmp, sizeof(tmp), "(%s)", buf);
            }
        } else {
            if (consumer->mode == RL_CONSUMER_MODE_PER) {
                snprintf(tmp, sizeof(tmp), "@%s", buf);
            } else {
                snprintf(tmp, sizeof(tmp), "%s", buf);
            }
        }
        result = safeSave(tmp);
    }

    return result;
}

/********************************************************************************
 * accountKeySplit
 * Description:
 *     Split an rlAccountTab key "name#hostval#queueval#userval#projectval"
 *     into its five components by destructively modifying the key buffer in
 *     place (replacing '#' with '\0'). The caller must pass a writable copy.
 *
 * Input:
 *     keyBuf   [in/out]: writable copy of the key (will be modified)
 *     name     [out]:    pointer to the limit name portion
 *     vals     [out]:    array of 4 char* (host/queue/user/project), each set
 *                        to point into keyBuf (empty string if dimension is
 *                        SHARED / not specified).
 *
 * Return:
 *     TRUE on success, FALSE if the key is malformed.
 ********************************************************************************/
static int
accountKeySplit(char *keyBuf, char **name, char **vals)
{
    char *p;
    int field;

    *name = keyBuf;
    vals[0] = vals[1] = vals[2] = vals[3] = NULL;

    p = keyBuf;
    field = 0;
    while (*p != '\0' && field < 4) {
        if (*p == '#') {
            *p = '\0';
            vals[field] = p + 1;
            field++;
        }
        p++;
    }
    if (field < 4) {
        return FALSE;
    }
    return TRUE;
}

/********************************************************************************
 * fillLimitEntFromConfig
 * Description:
 *     Populate a client-facing rlimitEnt from a RL_LIMIT_T configuration.
 *     Resources are initialized with used=-1 (no usage). The caller must
 *     later aggregate usage from rlAccountTab if needed.
 *
 * Input:
 *     limit      [in]: the limit config
 *     ent        [out]: pre-allocated rlimitEnt to fill
 *     configOnly [in]: if TRUE, formatConsumerValue uses -c style
 *
 * Return:
 *     TRUE on success, FALSE on allocation failure (ent contents are
 *     partially filled and must be freed by the caller).
 ********************************************************************************/
static int
fillLimitEntFromConfig(const RL_LIMIT_T *limit, struct rlimitEnt *ent,
                       int configOnly)
{
    static char fname[] = "fillLimitEntFromConfig";
    int j;

    ent->name = safeSave(limit->name ? limit->name : "");
    ent->nConsumers = RL_CONSUMER_POSITION_MAX;
    ent->consumers = (struct rlimitConsumerEnt *)my_calloc(
        RL_CONSUMER_POSITION_MAX, sizeof(struct rlimitConsumerEnt), fname);
    for (j = 0; j < RL_CONSUMER_POSITION_MAX; j++) {
        RL_CONSUMER_T *c = &limit->consumers[j];
        ent->consumers[j].type = j;
        ent->consumers[j].value = formatConsumerValue(c, configOnly);
    }

    ent->nResources = limit->nResources;
    ent->resources = (struct rlimitResourceEnt *)my_calloc(
        limit->nResources, sizeof(struct rlimitResourceEnt), fname);
    for (j = 0; j < limit->nResources; j++) {
        RL_RESOURCE_T *r = &limit->resources[j];
        ent->resources[j].resName = safeSave(r->resName ? r->resName : "");
        ent->resources[j].type = r->type;
        /* Non-percent MEM/SWP/TMP values are stored internally in KB;
         * convert back to configured unit for display. */
        if (!r->isPercent
            && (r->type == RL_RESOURCE_TYPE_MEM
                || r->type == RL_RESOURCE_TYPE_SWP
                || r->type == RL_RESOURCE_TYPE_TMP)) {
            ent->resources[j].value = convertUnitFromMB(r->value);
        } else {
            ent->resources[j].value = r->value;
        }
        ent->resources[j].isPercent = r->isPercent;
        ent->resources[j].used = -1.0;
    }

    ent->desc = safeSave(limit->desc ? limit->desc : "");
    return TRUE;
}

/********************************************************************************
 * fillLimitEntFromUsage
 * Description:
 *     Build a client-facing rlimitEnt from one RL_USAGE_T entry. The limit
 *     config is located via usage->limitNo. Consumer values are set as
 *     follows:
 *       - SHARED mode: original config value is tokenized, group names get a
 *         trailing '/', and the result is wrapped in parentheses if it
 *         contains spaces.
 *       - PER mode: the specific value from keyVals[pos] is used directly
 *         with a '@' prefix to mark the PER instance.
 *     Resource values (limit and used) are copied directly from
 *     usage->rsrcAccounts. isPercent is inherited from the limit config by
 *     index correspondence.
 *
 * Input:
 *     usage   [in]:  the rlAccountTab usage entry
 *     ent     [out]: pre-allocated rlimitEnt to fill
 *     keyVals [in]:  array of 4 char* (host/queue/user/project values parsed
 *                    from the key; SHARED dims are "")
 *
 * Return:
 *     TRUE on success, FALSE on allocation failure or invalid usage.
 ********************************************************************************/
static int
fillLimitEntFromUsage(RL_USAGE_T *usage, struct rlimitEnt *ent,
                      char **keyVals)
{
    static char fname[] = "fillLimitEntFromUsage";
    RL_LIMIT_T *limit;
    int j;

    if (usage == NULL || ent == NULL || keyVals == NULL) {
        return FALSE;
    }
    if (usage->limitNo < 0 || usage->limitNo >= generalRLConf.nLimits) {
        return FALSE;
    }
    limit = &generalRLConf.limits[usage->limitNo];

    ent->name = safeSave(limit->name ? limit->name : "");
    ent->nConsumers = RL_CONSUMER_POSITION_MAX;
    ent->consumers = (struct rlimitConsumerEnt *)my_calloc(
        RL_CONSUMER_POSITION_MAX, sizeof(struct rlimitConsumerEnt), fname);
    for (j = 0; j < RL_CONSUMER_POSITION_MAX; j++) {
        RL_CONSUMER_T *c = &limit->consumers[j];
        ent->consumers[j].type = j;
        if (c->mode == RL_CONSUMER_MODE_PER) {
            ent->consumers[j].value=safeSave(keyVals[j]);
        } else {
            /* SHARED mode: format from original config value with group
             * detection and space wrapping */
            ent->consumers[j].value = formatConsumerValue(c, 0);
        }
    }

    ent->nResources = usage->rsrcCnt;
    ent->resources = (struct rlimitResourceEnt *)my_calloc(
        usage->rsrcCnt, sizeof(struct rlimitResourceEnt), fname);
    for (j = 0; j < usage->rsrcCnt; j++) {
        RL_RESOURCE_ACCOUNT_T *ra = &usage->rsrcAccounts[j];
        ent->resources[j].resName = safeSave(ra->name ? ra->name : "");
        ent->resources[j].type = limit->resources[j].type;
        ent->resources[j].isPercent = limit->resources[j].isPercent;
       /*  MEM/SWP/TMP values are stored internally in MB;
         * convert back to configured unit for display. */
        if (ent->resources[j].type == RL_RESOURCE_TYPE_MEM
                || ent->resources[j].type == RL_RESOURCE_TYPE_SWP
                || ent->resources[j].type == RL_RESOURCE_TYPE_TMP) {
            ent->resources[j].value = convertUnitFromMB(ra->limit);
            ent->resources[j].used = convertUnitFromMB(ra->used);
        } else {
            ent->resources[j].value = ra->limit;
            ent->resources[j].used = ra->used;
        }
    }

    ent->desc = safeSave(limit->desc ? limit->desc : "");

    return TRUE;
}

/********************************************************************************
 * newRLBitmapAllOnes
 * Description:
 *     Allocate a fresh bitmap (rlBitmapSize ints) initialized to all-ones.
 *
 * Return:
 *     Pointer to the new bitmap, or NULL on allocation failure.
 ********************************************************************************/
static int *
newRLBitmapAllOnes(void)
{
    static char fname[] = "newRLBitmapAllOnes";
    int *bm;
    int j;

    bm = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);
    for (j = 0; j < rlBitmapSize; j++) {
        bm[j] = ~0;
    }
    return bm;
}

/********************************************************************************
 * bitmapAndInto
 * Description:
 *     dst &= src. Both arrays must hold rlBitmapSize ints.
 ********************************************************************************/
static void
bitmapAndInto(int *dst, const int *src)
{
    int j;

    for (j = 0; j < rlBitmapSize; j++) {
        dst[j] &= src[j];
    }
}

/********************************************************************************
 * bitmapOrInto
 * Description:
 *     dst |= src. Both arrays must hold rlBitmapSize ints.
 ********************************************************************************/
static void
bitmapOrInto(int *dst, const int *src)
{
    int j;

    for (j = 0; j < rlBitmapSize; j++) {
        dst[j] |= src[j];
    }
}

/********************************************************************************
 * ensureReplyCapacity
 * Description:
 *     Grow reply->limits so that index k is valid. Doubles the capacity as
 *     needed and zeroes the newly grown region. On allocation failure
 *     reply->limits is left unchanged (the caller must bail out).
 *
 * Input:
 *     reply [in/out]: the reply whose limits array is grown
 *     cap   [in/out]: current capacity (updated on success)
 *     k     [in]:     index that must become valid
 *
 * Return:
 *     TRUE on success, FALSE on allocation failure.
 ********************************************************************************/
static int
ensureReplyCapacity(struct rsrcLimitInfoReply *reply, int *cap, int k)
{
    static char fname[] = "ensureReplyCapacity";

    if (*cap > 0 && k < *cap) {
        return TRUE;
    }
    if (*cap <= 0) {
        int newCap = 16;
        struct rlimitEnt *tmp = (struct rlimitEnt *)my_calloc(
            newCap, sizeof(struct rlimitEnt), fname);
        reply->limits = tmp;
        *cap = newCap;
        return TRUE;
    }
    {
        int newCap = *cap * 2;
        struct rlimitEnt *tmp = (struct rlimitEnt *)realloc(
            reply->limits, newCap * sizeof(struct rlimitEnt));
        if (tmp == NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                    "%s: realloc failed"), fname);
            return FALSE;
        }
        memset(&tmp[*cap], 0, (newCap - *cap) * sizeof(struct rlimitEnt));
        reply->limits = tmp;
        *cap = newCap;
    }
    return TRUE;
}

/********************************************************************************
 * computeDimFilterBitmap
 * Description:
 *     Compute the filter bitmap for one consumer dimension. If filterStr is
 *     NULL/empty/" " the result is all-ones (no filter -> every limit
 *     matches). Otherwise the result is the OR of the per-value bitmaps
 *     returned by getRLBitmap() for each space-separated word in
 *     filterStr. The returned bitmap is freshly allocated and owned by the
 *     caller.
 *
 * Input:
 *     filterStr [in]: space-separated filter (NULL/empty/" " = no filter)
 *     pos       [in]: RL_CONSUMER_POSITION_*
 *
 * Return:
 *     malloc'd bitmap (rlBitmapSize ints), or NULL on allocation failure.
 ********************************************************************************/

/* Check whether a filter value names an existing cluster object for the
 * given consumer dimension. "all" is always valid. PROJECT is not
 * validated (per design). Used to reject non-existent host/queue/user
 * filter values so they contribute an all-zero bitmap instead of
 * spuriously matching limits that do not restrict that dimension. */
static int
filterValueExists(const char *name, int pos)
{
    if (name == NULL || name[0] == '\0') {
        return FALSE;
    }
    if (strcmp(name, "all") == 0) {
        return TRUE;
    }
    switch (pos) {
    case RL_CONSUMER_POSITION_HOST:
        return (getHGrpData((char *)name) != NULL
                || getHostData((char *)name) != NULL);
    case RL_CONSUMER_POSITION_QUEUE:
        return (getQueueData((char *)name) != NULL);
    case RL_CONSUMER_POSITION_USER:
        return (getpwlsfuser_((char *)name) != NULL
                || getUGrpData((char *)name) != NULL);
    case RL_CONSUMER_POSITION_PROJECT:
    default:
        return TRUE;
    }
}

static int *
computeDimFilterBitmap(const char *filterStr, int pos)
{
    static char fname[] = "computeDimFilterBitmap";
    int *bm;
    char filterBuf[MAXLINELEN];
    char *sp, *word;
    int isFree = FALSE;

    if (filterStr == NULL || filterStr[0] == '\0'
        || strcmp(filterStr, " ") == 0) {
        return newRLBitmapAllOnes();
    }

    bm = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);
    snprintf(filterBuf, sizeof(filterBuf), "%s", filterStr);
    sp = filterBuf;
    while ((word = getNextWord_(&sp)) != NULL) {
        int *cached;
        char wordCopy[2*MAXHOSTNAMELEN];

        /* getNextWord_() uses a static buffer; must copy before calling
         * getRLBitmap4Query(), which internally calls isRLConsumerMatch() ->
         * getNextWord_() on consumer->value, overwriting the static buffer. */
        snprintf(wordCopy, sizeof(wordCopy), "%s", word);

        /* Sanity check: for host/queue/user dimensions, verify the filter
         * value refers to an existing cluster object. A non-existent value
         * must contribute an all-zero bitmap; otherwise getRLBitmap4Query() would
         * set bits for limits that don't restrict this dimension (default
         * match), spuriously returning those limits. */
        if (pos == RL_CONSUMER_POSITION_HOST
            || pos == RL_CONSUMER_POSITION_QUEUE
            || pos == RL_CONSUMER_POSITION_USER) {
            if (!filterValueExists(wordCopy, pos)) {
                ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd, NL_SETN, 61,
                        "%s: filter value <%s> does not exist in cluster; ignored"),
                          fname, wordCopy);
                continue;
            }
        }

        cached = getRLBitmap4Query(wordCopy, pos, &isFree);
        if (cached != NULL) {
            bitmapOrInto(bm, cached);
            if (isFree) {
                FREEUP(cached);
            }
        }
        /* If cached is NULL (value matched nothing), treat that value's
         * bitmap as all-zeros: skip it (bm already zero there). */
    }

    return bm;
}

/********************************************************************************
 * keyValueMatchesFilter
 * Description:
 *     Check if an actual key value (from an rlAccountTab key) matches the
 *     user's filter list. filterStr is space-separated with OR semantics. If
 *     filterStr is NULL/empty/" " there is no filter and every value matches.
 *     Otherwise each filter word is tested via isConsumerNameMatch() against
 *     keyVal, so group membership is honored.
 *
 * Input:
 *     keyVal     [in]: actual consumer value from the rlAccountTab key
 *     filterStr  [in]: space-separated filter list (NULL/empty = no filter)
 *     ctype      [in]: consumer type for group membership checks
 *
 * Return:
 *     TRUE if the filter is empty or any filter word matches.
 ********************************************************************************/
static int
keyValueMatchesFilter(const char *keyVal, const char *filterStr,
                      enum rl_consumer_type ctype)
{
    char filterBuf[MAXLINELEN];
    char *sp, *word;

    if (filterStr == NULL || filterStr[0] == '\0'
        || strcmp(filterStr, " ") == 0) {
        return TRUE;
    }

    if (keyVal == NULL || keyVal[0] == '\0') {
        return FALSE;
    }

    snprintf(filterBuf, sizeof(filterBuf), "%s", filterStr);
    sp = filterBuf;
    while ((word = getNextWord_(&sp)) != NULL) {
        char wordCopy[2*MAXHOSTNAMELEN];

        snprintf(wordCopy, sizeof(wordCopy), "%s", word);
        if (isConsumerNameMatch(wordCopy, ctype, keyVal)) {
            return TRUE;
        }
    }
    return FALSE;
}

/********************************************************************************
 * strategyAEnumLookup
 * Description:
 *     Recursive helper for the Strategy A usage lookup. Enumerates the
 *     cartesian product of the filter words for the user-filtered PER
 *     dimensions. At the leaves, builds a limit key (PER dims use the chosen
 *     word, SHARED dims pass "" which getLimitKey ignores), looks it up in
 *     rlAccountTab, and emits a new rlimitEnt for each found usage. PER
 *     consumer values are overridden with the user filter word so each
 *     instance shows the specific consumer it tracks.
 *
 *     perPos[0..nPer-1] are the PER dimension positions to enumerate;
 *     perWords[d] / perCnt[d] are the word list / count for perPos[d];
 *     vals[4] carries the current per-dimension value (SHARED entries stay
 *     "" for the whole call).
 *
 * Return:
 *     TRUE on success, FALSE on allocation failure (caller must bail out).
 ********************************************************************************/
static int
strategyAEnumLookup(RL_LIMIT_T *limit,
                    int *perPos, char ***perWords, int *perCnt,
                    int nPer, int depth, char *vals[4],
                    struct rsrcLimitInfoReply *reply,
                    int *cap, int *k)
{
    int w;

    if (depth == nPer) {
        /* Leaf: build the key and look it up. SHARED dims pass "" which
         * getLimitKey ignores. */
        char keyBuf[RL_KEY_BUF_LEN];
        hEnt *hashEnt;
        RL_USAGE_T *usage;
        struct rlimitEnt *ent;

        if (getLimitKey(limit,
                        vals[RL_CONSUMER_POSITION_HOST],
                        vals[RL_CONSUMER_POSITION_QUEUE],
                        vals[RL_CONSUMER_POSITION_USER],
                        vals[RL_CONSUMER_POSITION_PROJECT],
                        keyBuf, sizeof(keyBuf)) != 0) {
            return FALSE;
        }
        hashEnt = h_getEnt_(&rlAccountTab, keyBuf);
        if (hashEnt == NULL) {
            return TRUE;  /* no usage for this combination */
        }
        usage = (RL_USAGE_T *)hashEnt->hData;
        if (usage == NULL) {
            return TRUE;
        }

        if (!ensureReplyCapacity(reply, cap, *k)) {
            return FALSE;
        }
        ent = &reply->limits[*k];
        if (!fillLimitEntFromUsage(usage, ent, vals)) {
            return FALSE;
        }
        (*k)++;
        return TRUE;
    }

    for (w = 0; w < perCnt[depth]; w++) {
        vals[perPos[depth]] = perWords[depth][w];
        if (!strategyAEnumLookup(limit, perPos, perWords, perCnt,
                                 nPer, depth + 1, vals, reply, cap, k)) {
            return FALSE;
        }
    }
    vals[perPos[depth]] = "";  /* restore for sibling branches */
    return TRUE;
}

/********************************************************************************
 * buildRsrcLimitInfoReply
 * Description:
 *     Build a client-facing reply from generalRLConf and rlAccountTab based
 *     on the request filters and options. Uses a bitmap-based filter scheme
 *     mirroring getRLBitmap()/rlBitmapCache instead of per-entry
 *     string matching.
 *
 *     Algorithm:
 *       1. For each of the 4 consumer dimensions (HOST/QUEUE/USER/PROJECT)
 *          compute a filter bitmap (rlBitmapSize ints). If the user supplied
 *          no filter for a dimension the bitmap is all-ones; otherwise it is
 *          the OR of getRLBitmap(word, pos) over each space-
 *          separated filter word.
 *       2. If a -n (name) filter is present, build a name bitmap whose bit i
 *          is set iff generalRLConf.limits[i].name equals one of req->names.
 *       3. AND the 4 dimension bitmaps (and the name bitmap, if any) into a
 *          single retBitmap. Bit i=1 means limit i matches ALL filters.
 *       4. RLIMIT_OPT_CONFIG_ONLY (-c): for each set bit, emit the limit
 *          config via fillLimitEntFromConfig(configOnly=1); used=-1; do not
 *          consult rlAccountTab.
 *       5/6. default and -a: for each set bit, if limit->refer>0 look up
 *          usage from rlAccountTab using the 4-case strategy (Strategy A =
 *          key enumeration when every PER dimension is user-filtered;
 *          Strategy B = table scan otherwise). A PER limit splits into
 *          multiple independent usage instances (one per consumer value
 *          combination); each found usage is emitted as its own rlimitEnt
 *          with PER consumer values overridden to the specific value from
 *          the rlAccountTab key or user filter. If limit->refer==0 the limit
 *          is emitted only in -a mode, with used=-1 and no rlAccountTab
 *          lookup.
 *
 * Input:
 *     req [in]: the request with filters
 *     reply [out]: the reply to populate (caller pre-zeroed)
 *
 * Return:
 *     LSBE_NO_ERROR on success, LSBE_NO_MEM on allocation failure.
 ********************************************************************************/
int
buildRsrcLimitInfoReply(struct rsrcLimitInfoReq *req,
                       struct rsrcLimitInfoReply *reply)
{
    static char fname[] = "buildRsrcLimitInfoReply";
    int i, k;
    int configOnly = (req->options & RLIMIT_OPT_CONFIG_ONLY) ? 1 : 0;
    int showAll = (req->options & RLIMIT_OPT_ALL) ? 1 : 0;
    int *dimBitmap[RL_CONSUMER_POSITION_MAX];
    const char *filterStrs[RL_CONSUMER_POSITION_MAX];
    int filtered[RL_CONSUMER_POSITION_MAX]; /* 1 if user gave a filter */
    int *nameBitmap = NULL;
    int *retBitmap = NULL;
    int cap = 0;            /* current capacity of reply->limits */
    int retCode = LSBE_NO_ERROR;

    reply->numLimits = 0;
    reply->limits = NULL;

    if (generalRLConf.nLimits <= 0) {
        return LSBE_NO_ERROR;
    }

    /* ---- Step 1: per-dimension filter bitmaps ---- */
    filterStrs[RL_CONSUMER_POSITION_HOST]    = req->hosts;
    filterStrs[RL_CONSUMER_POSITION_QUEUE]   = req->queue;
    filterStrs[RL_CONSUMER_POSITION_USER]    = req->user;
    filterStrs[RL_CONSUMER_POSITION_PROJECT] = req->project;

    for (i = 0; i < RL_CONSUMER_POSITION_MAX; i++) {
        dimBitmap[i] = computeDimFilterBitmap(filterStrs[i], i);
        filtered[i] = (filterStrs[i] != NULL && filterStrs[i][0] != '\0'
                       && strcmp(filterStrs[i], " ") != 0) ? 1 : 0;
    }

    /* ---- Step 2: name bitmap for -n filter ---- */
    if (req->numNames > 0 && req->names != NULL) {
        nameBitmap = (int *)my_calloc(rlBitmapSize, sizeof(int), fname);

        for (i = 0; i < generalRLConf.nLimits; i++) {
            RL_LIMIT_T *limit = &generalRLConf.limits[i];
            int n;

            if (limit->name == NULL) {
                continue;
            }
            for (n = 0; n < req->numNames; n++) {
                if (req->names[n] != NULL
                    && strcmp(req->names[n], limit->name) == 0) {
                    SET_BIT(i, nameBitmap);
                    break;
                }
            }
        }
    }

    /* ---- Step 3: AND all bitmaps into retBitmap ---- */
    retBitmap = newRLBitmapAllOnes();
    for (i = 0; i < RL_CONSUMER_POSITION_MAX; i++) {
        bitmapAndInto(retBitmap, dimBitmap[i]);
    }
    if (nameBitmap != NULL) {
        bitmapAndInto(retBitmap, nameBitmap);
    }

    /* ---- Step 4: config-only (-c) path ---- */
    if (configOnly) {
        k = 0;
        for (i = 0; i < generalRLConf.nLimits; i++) {
            int isSet = 0;
            RL_LIMIT_T *limit;

            TEST_BIT(i, retBitmap, isSet);
            if (!isSet) {
                continue;
            }
            limit = &generalRLConf.limits[i];
            if (!ensureReplyCapacity(reply, &cap, k)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: calloc failed"), fname);
                reply->numLimits = k;
                retCode = LSBE_NO_MEM;
                goto cleanup;
            }
            if (!fillLimitEntFromConfig(limit, &reply->limits[k], 1)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: calloc failed"), fname);
                reply->numLimits = k;
                retCode = LSBE_NO_MEM;
                goto cleanup;
            }
            k++;
        }
        reply->numLimits = k;
        goto cleanup;
    }

    /* ---- Steps 5 & 6: usage path (default and -a) ---- */
    k = 0;

    for (i = 0; i < generalRLConf.nLimits; i++) {
        int isSet = 0;
        RL_LIMIT_T *limit;
        int anyPerUnfiltered;
        int pos;

        TEST_BIT(i, retBitmap, isSet);
        if (!isSet) {
            continue;
        }
        limit = &generalRLConf.limits[i];

        if (limit->refer <= 0) {
            /* No active usage record. Emit only in -a mode, used=-1. */
            if (!showAll) {
                continue;
            }
            if (!ensureReplyCapacity(reply, &cap, k)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: calloc failed"), fname);
                reply->numLimits = k;
                retCode = LSBE_NO_MEM;
                goto cleanup;
            }
            if (!fillLimitEntFromConfig(limit, &reply->limits[k], 0)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: calloc failed"), fname);
                reply->numLimits = k;
                retCode = LSBE_NO_MEM;
                goto cleanup;
            }
            k++;
            continue;
        }

        /* limit->refer > 0: look up usage. Decide Strategy A vs B.
         * Strategy A (key enumeration) is used when EVERY PER-mode dimension
         * is user-filtered. Otherwise Strategy B (table scan). */
        anyPerUnfiltered = 0;
        for (pos = 0; pos < RL_CONSUMER_POSITION_MAX; pos++) {
            if (limit->consumers[pos].mode == RL_CONSUMER_MODE_PER
                && !filtered[pos]) {
                anyPerUnfiltered = 1;
                break;
            }
        }

        if (!anyPerUnfiltered) {
            /* ---- Strategy A: enumerate cartesian product of PER filters ---- */
            int perPos[RL_CONSUMER_POSITION_MAX];
            char **perWords[RL_CONSUMER_POSITION_MAX];
            int perCnt[RL_CONSUMER_POSITION_MAX];
            int nPer = 0;
            int ok = 1;
            char *vals[RL_CONSUMER_POSITION_MAX];
            char emptyStr[] = "";
            int pp;

            for (pp = 0; pp < RL_CONSUMER_POSITION_MAX; pp++) {
                vals[pp] = emptyStr;
                perWords[pp] = NULL;
                perCnt[pp] = 0;
            }

            /* Collect PER dims (all are filtered here). Split each filter
             * string into a word array (owned by this scope). */
            for (pos = 0; pos < RL_CONSUMER_POSITION_MAX && ok; pos++) {
                char fbuf[MAXLINELEN];
                char *sp, *word;
                int cnt = 0;
                int ww;

                if (limit->consumers[pos].mode != RL_CONSUMER_MODE_PER) {
                    continue;
                }
                perPos[nPer] = pos;

                /* First pass: count words. */
                snprintf(fbuf, sizeof(fbuf), "%s", filterStrs[pos]);
                sp = fbuf;
                while ((word = getNextWord_(&sp)) != NULL) {
                    cnt++;
                }
                if (cnt <= 0) {
                    /* filtered==1 implies non-empty, but guard anyway:
                     * treat as a single empty-key combination. */
                    perWords[nPer] = (char **)my_calloc(1, sizeof(char *), fname);
                    perWords[nPer][0] = emptyStr;
                    perCnt[nPer] = 1;
                    nPer++;
                    continue;
                }
                perWords[nPer] = (char **)my_calloc(cnt, sizeof(char *), fname);
                /* Second pass: copy words (re-scan the filter string). */
                snprintf(fbuf, sizeof(fbuf), "%s", filterStrs[pos]);
                sp = fbuf;
                ww = 0;
                while ((word = getNextWord_(&sp)) != NULL && ww < cnt) {
                    char *copy = safeSave(word);
                    if (copy == NULL) {
                        ok = 0;
                        break;
                    }
                    perWords[nPer][ww++] = copy;
                }
                perCnt[nPer] = ww;
                nPer++;
            }

            if (!ok) {
                int qq, ww;

                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: calloc failed"), fname);
                for (qq = 0; qq < RL_CONSUMER_POSITION_MAX; qq++) {
                    if (perWords[qq] != NULL) {
                        for (ww = 0; ww < perCnt[qq]; ww++) {
                            if (perWords[qq][ww] != emptyStr) {
                                FREEUP(perWords[qq][ww]);
                            }
                        }
                        FREEUP(perWords[qq]);
                    }
                }
                reply->numLimits = k;
                retCode = LSBE_NO_MEM;
                goto cleanup;
            }

            if (!strategyAEnumLookup(limit, perPos, perWords, perCnt,
                                     nPer, 0, vals, reply, &cap, &k)) {
                int qq, ww;

                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                        "%s: calloc failed"), fname);
                for (qq = 0; qq < RL_CONSUMER_POSITION_MAX; qq++) {
                    if (perWords[qq] != NULL) {
                        for (ww = 0; ww < perCnt[qq]; ww++) {
                            if (perWords[qq][ww] != emptyStr) {
                                FREEUP(perWords[qq][ww]);
                            }
                        }
                        FREEUP(perWords[qq]);
                    }
                }
                reply->numLimits = k;
                retCode = LSBE_NO_MEM;
                goto cleanup;
            }
            /* free perWords */
            for (pp = 0; pp < RL_CONSUMER_POSITION_MAX; pp++) {
                int ww;

                if (perWords[pp] != NULL) {
                    for (ww = 0; ww < perCnt[pp]; ww++) {
                        if (perWords[pp][ww] != emptyStr) {
                            FREEUP(perWords[pp][ww]);
                        }
                    }
                    FREEUP(perWords[pp]);
                }
            }
        } else {
            /* ---- Strategy B: full table scan of rlAccountTab ---- */
            sTab searchPtr;
            hEnt *hashEnt;

            hashEnt = h_firstEnt_(&rlAccountTab, &searchPtr);
            while (hashEnt != NULL) {
                RL_USAGE_T *usage = (RL_USAGE_T *)hashEnt->hData;
                char keyBuf[MAXLINELEN];
                char *nameP;
                char *vals[4];
                int matched;
                struct rlimitEnt *ent;

                if (usage == NULL || usage->limitNo != i) {
                    hashEnt = h_nextEnt_(&searchPtr);
                    continue;
                }

                snprintf(keyBuf, sizeof(keyBuf), "%s", hashEnt->keyname);
                if (!accountKeySplit(keyBuf, &nameP, vals)) {
                    hashEnt = h_nextEnt_(&searchPtr);
                    continue;
                }

                /* For each user-filtered PER dimension, the key value must
                 * match the filter. SHARED and unfiltered PER dimensions need
                 * no key check here (retBitmap already filtered the config). */
                matched = 1;
                for (pos = 0; pos < RL_CONSUMER_POSITION_MAX; pos++) {
                    if (limit->consumers[pos].mode == RL_CONSUMER_MODE_PER
                        && filtered[pos]) {
                        if (!keyValueMatchesFilter(
                                vals[pos], filterStrs[pos],
                                (enum rl_consumer_type)pos)) {
                            matched = 0;
                            break;
                        }
                    }
                }
                if (!matched) {
                    hashEnt = h_nextEnt_(&searchPtr);
                    continue;
                }

                if (!ensureReplyCapacity(reply, &cap, k)) {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                            "%s: calloc failed"), fname);
                    reply->numLimits = k;
                    retCode = LSBE_NO_MEM;
                    goto cleanup;
                }
                ent = &reply->limits[k];
                if (!fillLimitEntFromUsage(usage, ent, vals)) {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 12,
                            "%s: calloc failed"), fname);
                    reply->numLimits = k;
                    retCode = LSBE_NO_MEM;
                    goto cleanup;
                }
                k++;
                hashEnt = h_nextEnt_(&searchPtr);
            }
        }
    }

    reply->numLimits = k;

cleanup:
    {
        int p;

        for (p = 0; p < RL_CONSUMER_POSITION_MAX; p++) {
            FREEUP(dimBitmap[p]);
        }
        FREEUP(nameBitmap);
        FREEUP(retBitmap);
    }
    return retCode;
} /*buildRsrcLimitInfoReply*/

/********************************************************************************
 * freeRsrcLimitInfoReply
 * Description:
 *     Free the contents of a rsrcLimitInfoReply populated by
 *     buildRsrcLimitInfoReply. Does not free the reply struct itself.
 *
 * Input:
 *     reply [in/out]: the reply to free
 ********************************************************************************/
void
freeRsrcLimitInfoReply(struct rsrcLimitInfoReply *reply)
{
    int i, j;

    if (reply == NULL || reply->limits == NULL) {
        return;
    }

    for (i = 0; i < reply->numLimits; i++) {
        struct rlimitEnt *ent = &reply->limits[i];
        FREEUP(ent->name);
        if (ent->consumers) {
            for (j = 0; j < ent->nConsumers; j++) {
                FREEUP(ent->consumers[j].value);
            }
            FREEUP(ent->consumers);
        }
        if (ent->resources) {
            for (j = 0; j < ent->nResources; j++) {
                FREEUP(ent->resources[j].resName);
            }
            FREEUP(ent->resources);
        }
        FREEUP(ent->desc);
    }
    FREEUP(reply->limits);
    reply->numLimits = 0;
} /*freeRsrcLimitInfoReply*/


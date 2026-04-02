/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#include "mbd.h"
#include "daemons.h"

/*jobMergedResReqTab
 *key: job_resreq#attrFlags#queueName#
 *value: struct resReqEntry;
 */
hTab jobMergedResReqTab;
/*jobEffResReqTab
 *key: resreq (only a started job has effective resreq)
 *value: struct resReqEntry;
 */
hTab jobEffeResReqTab;

struct resVal* dupResVal(struct resVal *src);
static struct resVal* mergeResReq(struct jData *, int options);
static void mergeSelect(struct resVal *new, struct resVal *merge);
static void mergeOrder(struct resVal *new, struct resVal *merge);
static void mergeRusage(struct resVal *new, struct resVal *merge);
static void mergeSpan(struct resVal *new, struct resVal *merge);
static int  selectHasStringBool(char *selectStr);
static void setDefType4Select(struct resVal *resValPtr, int options);
static char* select2Str(struct resVal *resVal);
static char* order2Str(struct resVal *resVal);
static char* rusage2Str(struct resVal *resVal);
static char* span2Str(struct resVal *resVal);
static char* convertTclSelectStr(char *selectStr);
static struct resReqEntry*  dupResReqEntry(struct resReqEntry *src);

/*merge options*/
#define MERGE_RESREQ_ANY 0x01


/*
 * mergeResReq - Merge job-level and queue-level resource requirements
 * @jp: Pointer to the job data structure
 * @options: Merge options (e.g., MERGE_RESREQ_ANY)
 *
 * This function combines the resource requirements specified at the job level
 * with those inherited from the queue. If neither level has specific requirements,
 * a system default is generated. If both exist, the job-level requirements
 * generally take precedence or are combined with the queue-level constraints
 * (select, order, rusage, and span).
 *
 * Returns: A pointer to the merged resVal structure, or NULL on failure.
 */
struct resVal * mergeResReq(struct jData *jp, int options) {
    struct resVal *jResVal = jp->shared->resValPtr;
    struct resVal *qResVal = jp->qPtr->resValPtr;
    struct resVal *merge = NULL;
    int cc;

    if (jResVal == NULL && qResVal == NULL) {
        merge = (struct resVal *)my_calloc(1, sizeof(struct resVal), "mergeResReq");
        initResVal(merge);

        if (options & MERGE_RESREQ_ANY) {
            char defStr[] = "select[type == any] order[r15s:pg]";
            if ((cc = parseResReq(defStr, merge, allLsInfo, (PR_ALL | PR_BATCH), unitForLimits)) != PARSE_OK) {
                ls_syslog(LOG_ERR, "\
%s: Failed to create default resource requirement for job <%s>, cc <%d>. %m", __func__, lsb_jobid2str(jp->jobId), cc);
                lsbFreeResVal(&merge);
                return NULL;
            }
        } else {
            char defStr[] = "select[type == local] order[r15s:pg]";
            if ((cc = parseResReq(defStr, merge, allLsInfo, (PR_ALL | PR_BATCH), unitForLimits)) != PARSE_OK) {
                ls_syslog(LOG_ERR, "\
%s: Failed to create default resource requirement for job <%s>, cc <%d>. %m", __func__, lsb_jobid2str(jp->jobId), cc);
                lsbFreeResVal(&merge);
                return NULL;
            }
        }
        return merge;
    }

    if (jResVal != NULL && qResVal == NULL) {
        merge = dupResVal(jResVal);
    } else if (jResVal == NULL && qResVal != NULL) {
        merge = dupResVal(qResVal);
    } else {
        /*merge job level and queue level resreq*/
        merge = dupResVal(jResVal);
        if (merge == NULL) {
            return NULL;
        }
        mergeSelect(qResVal, merge);
        mergeOrder(qResVal, merge);
        mergeRusage(qResVal, merge);            
        mergeSpan(qResVal, merge);
    }

    setDefType4Select(merge, options);
    
    return merge;
}
 
/*
 * mergeSelect - Merge queue and job's select requirements
 * @queue: queue level select requirements
 * @merge: Destination resVal structure which is initialized with
 *         job level select requirements
 *
 * Merges selectStr and xorExprs from queue to merge. If merge already has
 * selectStr, it is combined with queue's selectStr using logical AND when
 * queue level selectStr is not xorExprs format.
 * If merge has no selectStr, queue's selectStr is copied directly.
 */
void mergeSelect(struct resVal *queue, struct resVal *merge) {
    if (! (queue->options & PR_SELECT)) {
        return;
    }

    /*If merge already has XOR, then ignore queue's XOR*/
    if (merge->xorExprs) {
        return;
    } else if ((merge->options & PR_SELECT) && queue->xorExprs) { 
        /*if job is simple select, queue is XOR, then use job level directly*/
        return;
    } else if (!(merge->options & PR_SELECT) && queue->xorExprs) {
        // Copy xorExprs from queue to merge
        int i, totalCount=0;
        while (queue->xorExprs[totalCount] != NULL) totalCount++;
        merge->xorExprs = (char **)my_calloc(totalCount + 1, sizeof(char *), "mergeSelect");
        for (i = 0; i < totalCount; i++) {
            merge->xorExprs[i] = safeSave(queue->xorExprs[i]);
        }
        merge->xorExprs[totalCount] = NULL;

        FREEUP(merge->selectStr);
        merge->selectStr = safeSave(queue->selectStr);
        merge->selectStrSize = queue->selectStrSize;
    } else if (queue->selectStr) {
        /*both is simple select*/
        if (merge->selectStr) {
            /* Merge resval and merge's selectStr
             * 4: length of 'expr' just in case original string has no 'expr'
             * 9: the length for spaces,'()&&()' and '\0'
             */
            int newSize = strlen(merge->selectStr) + strlen(queue->selectStr) + 4 + 9;
            char *mStr = NULL, *nStr = NULL, *newSelectStr = NULL;

            if (strncmp(merge->selectStr, "expr", 4) == 0) {
                mStr = merge->selectStr + 4;
                while(isspace(*mStr)) {
                    mStr++;
                }               
            } else {
                mStr = merge->selectStr;
            }

            if (strncmp(queue->selectStr, "expr", 4) == 0) {
                nStr = queue->selectStr + 4;
                while(isspace(*nStr)) {
                    nStr++;
                }               
            } else {
                nStr = queue->selectStr;
            }

            newSelectStr = (char *)my_malloc(newSize, "mergeSelect");
            if (newSelectStr) {
                snprintf(newSelectStr, newSize, "expr (%s) && (%s)", mStr, nStr);
                FREEUP(merge->selectStr);
                merge->selectStr = newSelectStr;
                merge->selectStrSize = newSize;
            }
        } else {
            // Copy selectStr from queue to merge
            merge->selectStr = safeSave(queue->selectStr);
            merge->selectStrSize = queue->selectStrSize;
        }
    }

    // Set PR_SELECT flag in merge
    merge->options |= PR_SELECT;
}

/*
 * mergeOrder - Merge order requirements from queue to merge
 * @queue: Source resVal structure with queue level order requirements
 * @merge: Destination resVal structure with job level order requirements
 *
 * Merges order from queue to merge. If merge specified "order[]",
 * it is used as is. Otherwise, if queue has PR_ORDER set, it is copied
 * to merge.
 */
void mergeOrder(struct resVal *queue, struct resVal *merge) {
    // If merge has PR_ORDER set, use it and return
    if (merge->options & PR_ORDER) {
        return;
    }
    
    // If merge has no PR_ORDER set, check if queue has PR_ORDER set
    if (queue->options & PR_ORDER) {
        // Copy order from queue to merge
        int i;

        merge->nphase = queue->nphase;
        for (i = 0; i < queue->nphase; i++) {
            merge->order[i] = queue->order[i];
        }
        // Set PR_ORDER flag in merge
        merge->options |= PR_ORDER;
    }
}

/*
 * mergeSpan - Merge span requirements from queue to merge
 * @queue: Source resVal structure with queue level span requirements
 * @merge: Destination resVal structure with job level span requirements
 *
 * Merges span-related fields (numHosts, maxNumHosts, pTile) from queue to merge.
 * If merge specified "span[]", it is used as is. Otherwise, if queue has
 * PR_SPAN set, the span fields are copied to merge.
 */
void mergeSpan(struct resVal *queue, struct resVal *merge) {
    // If merge has PR_SPAN set, use it and return
    if (merge->options & PR_SPAN) {
        return;
    }
    
    // If merge has no PR_SPAN set, check if queue has PR_SPAN set
    if (queue->options & PR_SPAN) {
        // Copy span from queue to merge
        merge->numHosts = queue->numHosts;
        merge->maxNumHosts = queue->maxNumHosts;
        merge->pTile = queue->pTile;

        // Set PR_SPAN flag in merge
        merge->options |= PR_SPAN;
    }
}

/*
 * mergeRusage - Merge rusage requirements from queue to merge
 * @queue: Source resVal structure with queue level rusage requirements
 * @merge: Destination resVal structure with job level rusage requirements
 *
 * Merges rusgBitMaps from queue to merge. For overlapping resources,
 * job level values (merge) take precedence. For duration and decay,
 * job level values are used if specified; otherwise queue level values
 * are used if specified.
 */
void mergeRusage(struct resVal *queue, struct resVal *merge) {
    int i, isSet;
    int jobHas = 0, queueHas = 0;

    jobHas = (merge->options & PR_RUSAGE);
    queueHas = (queue->options & PR_RUSAGE);    

    if ((jobHas && !queueHas) || (!jobHas && !queueHas)) {
        return;
    } else if (!jobHas && queueHas) {
        memcpy(merge->rusgBitMaps, queue->rusgBitMaps, GET_INTNUM(allLsInfo->nRes) * sizeof(int));
        memcpy(merge->val, queue->val, allLsInfo->nRes * sizeof(float));
        merge->duration = queue->duration;
        merge->decay = queue->decay;
        if (queue->options & PR_RUSAGE) {
            merge->options |= PR_RUSAGE;
        }
        return;
    } else {
        /*merge them together*/
        for (i = 0; i < allLsInfo->nRes; i++) {
            TEST_BIT(i, merge->rusgBitMaps, isSet);
            if (isSet == 0) {
                TEST_BIT(i, queue->rusgBitMaps, isSet);
                if (isSet == 1) {
                    SET_BIT(i, merge->rusgBitMaps);
                    merge->val[i] = queue->val[i];
                    if (i < MAXSRES) {
                        merge->genClass |= 1 << i;
                    }
                }
            }
        }

        if (merge->duration > queue->duration) {
            merge->duration = queue->duration;
        }

        if (queue->decay != INFINIT_FLOAT && merge->decay != INFINIT_FLOAT) {
            if (queue->decay > merge->decay) {
                merge->decay = queue->decay;
            }
        } else if (queue->decay != INFINIT_FLOAT
                && merge->decay == INFINIT_FLOAT) {
            merge->decay = queue->decay;
        }

        // Set PR_RUSAGE flag if either has it
        merge->options |= PR_RUSAGE;
    }
}

/*
 * selectHasStringBool - Check if a select string contains string or boolean expressions
 * @selectStr: The resource requirement select string to check
 *
 * Returns: 1 if string or boolean expressions are found, 0 otherwise.
 */
int selectHasStringBool (char *selectStr) {
   char *p = NULL;

    if (selectStr == NULL || selectStr[0] == '\0') {
        return 0;
    }

    p = selectStr;

    while(*p != '\0') {
        /*Check whether has string expression
         *format: [res "eq" "xxx"] or [res "ne" "xxx"] or [defined "xx"]
         *check whether has bool expression
         *format: !res()
         */
        if(*p == '[' || *p == '!') {
            return 1;
        }
        /*check whether has bool expression
         *format: res() or res()&&1 or !res()
         */
        if(*p == '(' && *(p+1) == ')') {
            p = p+2; /*skip '()'*/
            if (isspace(*p)) {
                p++;
            }
            if (*p == '\0' || *p == '&' || *p == '|' || *p == ')') {
                return 1;
            }
        }
        p++;
    }

    return 0;
}

#define TYPE_LEN 30
/*
 * setDefType4Select - Set default host type for select expressions
 * @resValPtr: Pointer to the resource value structure
 * @options: Merge options (e.g., MERGE_RESREQ_ANY)
 *
 * This function ensures that select expressions have a host type specified.
 * It appends [type "eq" "any"] or [type "eq" "local"] based on the options
 * and the content of the current select string.
 */
void setDefType4Select(struct resVal *resValPtr, int options) {
    char *tmpStr = NULL;

    if (resValPtr->selectStr && resValPtr->selectStr[0] != '\0') {
        if (options & MERGE_RESREQ_ANY) {
            if (strstr(resValPtr->selectStr, "[type \"eq\" \"any\"]") == NULL) {
                tmpStr = (char *)my_calloc(strlen(resValPtr->selectStr) + TYPE_LEN, sizeof(char), "setDefType4Select");
                sprintf(tmpStr, "expr (%s) && ([type \"eq\" \"any\"])", resValPtr->selectStr + 5);
            }
        } else if (strstr(resValPtr->selectStr, "[type ") == NULL) {
            if (selectHasStringBool(resValPtr->selectStr)) {
                tmpStr = (char *)my_calloc(strlen(resValPtr->selectStr) + TYPE_LEN, sizeof(char), "setDefType4Select");
                sprintf(tmpStr, "expr (%s) && ([type \"eq\" \"any\"])", resValPtr->selectStr + 5);
            } else {
                tmpStr = (char *)my_calloc(strlen(resValPtr->selectStr) + TYPE_LEN, sizeof(char), "setDefType4Select");
                sprintf(tmpStr, "expr (%s) && ([type \"eq\" \"local\"])", resValPtr->selectStr + 5);
            }
        }

        if (tmpStr) {
            FREEUP(resValPtr->selectStr);
            resValPtr->selectStr = tmpStr;
        }
    }
    
    if (resValPtr->xorExprs != NULL) {
        int i;
        for (i = 0; resValPtr->xorExprs[i] != NULL; i++) {
            tmpStr = NULL;
            if (options & MERGE_RESREQ_ANY) {
                if (strstr(resValPtr->xorExprs[i], "[type \"eq\" \"any\"]") == NULL) {
                    tmpStr = (char *)my_calloc(strlen(resValPtr->xorExprs[i]) + TYPE_LEN, sizeof(char), "setDefType4Select");
                    sprintf(tmpStr, "expr (%s) && ([type \"eq\" \"any\"])", resValPtr->xorExprs[i] + 5);
                }
            } else if (strstr(resValPtr->xorExprs[i], "[type ") == NULL) {
                if (selectHasStringBool(resValPtr->xorExprs[i])) {
                    tmpStr = (char *)my_calloc(strlen(resValPtr->xorExprs[i]) + TYPE_LEN, sizeof(char), "setDefType4Select");
                    sprintf(tmpStr, "expr (%s) && ([type \"eq\" \"any\"])", resValPtr->xorExprs[i] + 5);
                } else {
                    tmpStr = (char *)my_calloc(strlen(resValPtr->xorExprs[i]) + TYPE_LEN, sizeof(char), "setDefType4Select");
                    sprintf(tmpStr, "expr (%s) && ([type \"eq\" \"local\"])", resValPtr->xorExprs[i] + 5);
                }
            }

            if (tmpStr) {
                FREEUP(resValPtr->xorExprs[i]);
                resValPtr->xorExprs[i] = tmpStr;
            }
        }
    }
    
    if ((resValPtr->selectStr == NULL || resValPtr->selectStr[0] == '\0') && resValPtr->xorExprs == NULL) {
        FREEUP(resValPtr->selectStr);
        resValPtr->selectStr = (char *)my_calloc(TYPE_LEN, sizeof(char), "setDefType4Select");
        if (options & MERGE_RESREQ_ANY) {
            strcat(resValPtr->selectStr, "expr [type \"eq\" \"any\"]");
        } else {
            strcat(resValPtr->selectStr, "expr [type \"eq\" \"local\"]"); 
        }
    }
}

/*
 * dupResVal - Deep copy a resVal structure
 * @src: The source resVal structure to copy from
 *
 * Returns: A new resVal structure with all fields deep copied, NULL on failure
 */
struct resVal *
dupResVal(struct resVal *src)
{
    struct resVal *dest;
    int i;
    int numExprs = 0;
    
    if (src == NULL) {
        return NULL;
    }
    
    // Allocate memory for the new resVal structure
    dest = (struct resVal *) my_calloc(1, sizeof(struct resVal), "dupResVal");
    
    // Copy simple fields
    dest->options = src->options;
    
    // Copy select related fields
    if (src->selectStr) {
        dest->selectStr = safeSave(src->selectStr);
    }
    dest->selectStrSize = src->selectStrSize;
    if (src->xorExprs) {
        // xorExprs is terminated with NULL
        while (src->xorExprs[numExprs] != NULL) {
            numExprs++;
        }
        
        dest->xorExprs = (char **) my_calloc(numExprs + 1, sizeof(char *), "deepCopyResVal");
        
        for (i = 0; i < numExprs; i++) {
            dest->xorExprs[i] = safeSave(src->xorExprs[i]);
        }
        dest->xorExprs[numExprs] = NULL;
    }

    // Copy order related fields
    dest->nphase = src->nphase;
    for (i = 0; i < dest->nphase; i++) {
        dest->order[i] = src->order[i];
    }

    // It seems "filter" keyword is obsolete, this field may be useless
    dest->nindex = src->nindex;
    if (src->indicies && src->nindex > 0) {
        dest->indicies = (int *) my_calloc(src->nindex, sizeof(int), "dupResVal");
        memcpy(dest->indicies, src->indicies, src->nindex * sizeof(int));
    }
    
    // Copy rusage related fields
    if (src->rusgBitMaps) {
        // Assuming rusgBitMaps has the same size as nindex
        dest->rusgBitMaps = (int *) my_calloc(GET_INTNUM(allLsInfo->nRes), sizeof(int), "dupResVal");
        memcpy(dest->rusgBitMaps, src->rusgBitMaps, GET_INTNUM(allLsInfo->nRes) * sizeof(int));
    }
    if (src->val) {
        dest->val = (float *) my_calloc(allLsInfo->nRes, sizeof(float), "dupResVal");
        memcpy(dest->val, src->val, allLsInfo->nRes * sizeof(float));
    }
    dest->duration = src->duration;
    dest->decay = src->decay;
    dest->genClass = src->genClass;

    /*copy span related fields*/
    dest->numHosts = src->numHosts;
    dest->maxNumHosts = src->maxNumHosts;
    dest->pTile = src->pTile;
    
    return dest;
}

/*
 * dupResReqEntry - Deep copy a resReqEntry structure
 * @src: Source resReqEntry structure to copy
 *
 * Returns a deep copy of the source structure, or NULL on failure.
 * The returned structure must be freed with freeResReqEntry().
 */
static struct resReqEntry *
dupResReqEntry(struct resReqEntry *src)
{
    struct resReqEntry *dest;
    
    if (src == NULL) {
        return NULL;
    }
    
    dest = (struct resReqEntry *) my_calloc(1, sizeof(struct resReqEntry), "dupResReqEntry");
    
    dest->numRef = 0;
    
    if (src->resValPtr != NULL) {
        dest->resValPtr = dupResVal(src->resValPtr);
        if (dest->resValPtr == NULL) {
            FREEUP(dest);
            return NULL;
        }
    }
    
    if (src->resReqStr != NULL) {
        dest->resReqStr = safeSave(src->resReqStr);
    }
    
    return dest;
}

/*
 * freeResReqEntry - Free a resReqEntry structure
 * @entry: resReqEntry structure to free
 *
 * Frees all memory associated with the resReqEntry structure.
 */
void
freeResReqEntry(struct resReqEntry **entry)
{
    if (entry == NULL || *entry == NULL) {
        return;
    }
    
    if ((*entry)->resValPtr != NULL) {
        lsbFreeResVal(&((*entry)->resValPtr));
    }
    
    if ((*entry)->resReqStr != NULL) {
        FREEUP((*entry)->resReqStr);
    }
    
    FREEUP(*entry);
    *entry = NULL;
}

/*
 * mkJobMergedResReqEntry - Create or reference a merged resource request entry for a job
 * @jp: Pointer to the job data structure
 *
 * This function creates a merged resource requirement by combining job-level and
 * queue-level requirements. It manages these entries in the jobMergedResReqTab
 * hash table using reference counts. If an identical requirement already exists,
 * its reference count is incremented; otherwise, a new entry is created.
 * The resulting hash table entry is stored in jp->shared->mergedResReqEnt.
 */
void mkJobMergedResReqEntry(struct jData *jp) {
    char * key = NULL;
    int    keySize = 0;
    int    jobAttr = 0;
    hEnt   *entry = NULL;
    struct resReqEntry *resReqEnt = NULL;

    /* key = job_resreq#jobAttr#queueName*/
    if (jp->numAskedPtr > 0 || jp->askedOthPrio >= 0) {
        jobAttr |= MERGE_RESREQ_ANY;
    }

    if(jp->shared->jobBill.resReq) {
        keySize += strlen(jp->shared->jobBill.resReq);
    }
    keySize += strlen(jp->qPtr->queue) + 22 + 2+ 1;
    key = (char *) my_calloc(keySize, sizeof(char), "mkJobMergedResReqEntry");
    snprintf(key, keySize, "%s#%d#%s", (jp->shared->jobBill.resReq ? jp->shared->jobBill.resReq:""), jobAttr, jp->qPtr->queue);

    /*cache resreq into hash table*/
    entry = (hEnt *) h_getEnt_(&jobMergedResReqTab, key);
    if (!entry) {
        struct resVal *tmpResVal = mergeResReq(jp, jobAttr);

        if (tmpResVal != NULL) {
            resReqEnt = (struct resReqEntry *) my_calloc(1, sizeof(struct resReqEntry), "mkJobMergedResReqEntry");
            resReqEnt->resValPtr = tmpResVal;
            resReqEnt->resReqStr = resVal2Str(resReqEnt->resValPtr);
            if (resReqEnt->resReqStr) {
                resReqEnt->numRef = 1;
                entry = (hEnt *)h_addEnt_(&jobMergedResReqTab, key, NULL);
                entry->hData = (int *) resReqEnt;
            } else {
                ls_syslog(LOG_ERR, "\
%s: Failed to generate job <%s> merged resource requirement string. Job cannot be scheduled, please bkill it.", __func__, lsb_jobid2str(jp->jobId));

                FREEUP(key);
                freeResReqEntry(&resReqEnt);
            }
        } else {
            ls_syslog(LOG_ERR, "\
%s: Failed to merge job <%s> resource requirement. Job cannot be scheduled, please bkill it.", __func__, lsb_jobid2str(jp->jobId)); 
        }
    } else {
        resReqEnt = (struct resReqEntry *) entry->hData;
        resReqEnt->numRef++;
    }

    FREEUP(key);
    if (jp->shared->mergedResReqEnt) {
        detachJobMergedResReqEntry(jp->shared);
    }
    jp->shared->mergedResReqEnt = entry;

    return;
} /*mkJobMergedResReqEntry*/

/*
 * mkJobEffeResReqEntry - Create or reference an effective resource request entry
 * @jp: Pointer to the job data structure
 *
 * This function retrieves or creates a resReqEntry in the jobEffeResReqTab hash table
 * based on the job's merged resource requirement. It manages reference counts
 * and ensures the job points to the correct effective resource request entry.
 */
void mkJobEffeResReqEntry(struct jData *jp) {
    char * key = NULL;
    hEnt   *entry = NULL;
    struct resReqEntry *eResReqEnt = NULL;
    
    if (!jp->shared->mergedResReqEnt) {
        return;
    }

    key = GET_JOB_MERGED_RES_REQ_STR(jp);

    entry = (hEnt *) h_getEnt_(&jobEffeResReqTab, key);
    if (!entry) {
        struct resReqEntry *mResReqEnt = (struct resReqEntry *)(jp->shared->mergedResReqEnt->hData);
        eResReqEnt = dupResReqEntry(mResReqEnt);
        if (eResReqEnt == NULL) {
            return;
        }
        entry = (hEnt *)h_addEnt_(&jobEffeResReqTab, key, NULL);
        if (entry) {
            entry->hData = (int *) eResReqEnt;
            eResReqEnt->numRef++;
        } else {
            freeResReqEntry(&eResReqEnt);
        }
    } else {
        eResReqEnt = (struct resReqEntry *) entry->hData;
        eResReqEnt->numRef++;
    }

    if (jp->effeResReqEnt) {
        detachJobEffeResReqEntry(jp);
    }
    jp->effeResReqEnt = entry;

    return;
}

/*
 * detachJobMergedResReqEntry - Detach merged resource request entry for a job
 * @jp: Job data structure
 * @entry: Hash table entry to detach
 *
 * Decrements the reference count of the merged resource request entry.
 * If the reference count reaches zero, the entry is removed from the hash table
 * and associated resources are freed.
 */
void detachJobMergedResReqEntry(struct jShared *shared) {
    struct resReqEntry *resReqEnt = NULL;

    if (shared == NULL || shared->mergedResReqEnt == NULL) {
        return;
    }

    resReqEnt = (struct resReqEntry *) shared->mergedResReqEnt->hData;
    resReqEnt->numRef--;

    if (resReqEnt->numRef == 0) {
        freeResReqEntry(&resReqEnt);
        h_rmEnt_(&jobMergedResReqTab, shared->mergedResReqEnt);
    }

    shared->mergedResReqEnt = NULL;
} /*detachJobMergedResReqEntry*/

/*
 * detachJobEffeResReqEntry - Detach effective resource request entry for a job
 * @entry: Hash table entry to detach
 *
 * Decrements the reference count of the effective resource request entry.
 * If the reference count reaches zero, the entry is removed from the hash table
 * and associated resources are freed using freeResReqEntry().
 */
void detachJobEffeResReqEntry(struct jData *job) {
    struct resReqEntry *resReqEnt = NULL;

    if (job == NULL || job->effeResReqEnt == NULL) {
        return;
    }

    resReqEnt = (struct resReqEntry *) job->effeResReqEnt->hData;
    resReqEnt->numRef--;
    if (resReqEnt->numRef == 0) {
        freeResReqEntry(&resReqEnt);
        h_rmEnt_(&jobEffeResReqTab, job->effeResReqEnt);
    }

    job->effeResReqEnt = NULL;
} /*detachJobEffeResReqEntry*/

/*
 * resVal2Str - Convert a resVal structure back into a resource requirement string
 * @resVal: Pointer to the resVal structure to convert
 *
 * This function takes the parsed resource requirements stored in a resVal structure
 * and reconstructs the corresponding resource requirement string. It includes
 * sections for select, order, rusage, and span if they are present and configured
 * in the structure's options.
 *
 * Returns: A dynamically allocated string containing the resource requirement,
 *          or NULL if the input is NULL or an error occurs. The caller is 
 *          responsible for freeing the returned string.
 */
char * resVal2Str(struct resVal *resVal)
{
    char *resReqStr = NULL, *tmpStr = NULL;
    int resReqStrSize = 0;
    int curSize = 0;

    if (resVal == NULL) {
        return NULL;
    }
    
    resReqStrSize = resVal->selectStrSize + 1024;
    resReqStr = (char *) my_calloc(resReqStrSize, sizeof(char), "resVal2Str");
    
    tmpStr = select2Str(resVal);
    if (tmpStr) {
        snprintf(resReqStr, resReqStrSize, "select[%s]", tmpStr);
        FREEUP(tmpStr);
    }

#define APPEND_RESREQ_SECTION(keyword, func_call) \
    tmpStr = func_call; \
    if (tmpStr && tmpStr[0] != '\0') { \
        curSize = strlen(resReqStr); \
        if (curSize + strlen(tmpStr) >= resReqStrSize) { \
            resReqStrSize = resReqStrSize + strlen(tmpStr) + 1024; \
            resReqStr = (char *) my_realloc(resReqStr, resReqStrSize, "resVal2Str"); \
        } \
        snprintf(resReqStr + curSize, resReqStrSize - curSize, " %s[%s]", keyword, tmpStr); \
        FREEUP(tmpStr); \
    } else if (tmpStr) { \
        FREEUP(tmpStr); \
    }

    APPEND_RESREQ_SECTION("order", order2Str(resVal));
    if (resVal->options & PR_RUSAGE) {
        APPEND_RESREQ_SECTION("rusage", rusage2Str(resVal));
    }
    if (resVal->options & PR_SPAN) {
        APPEND_RESREQ_SECTION("span", span2Str(resVal));
    }
    
    return resReqStr;
}

/*
 * select2Str - Convert a select requirement to a string
 * @resVal: Pointer to the resVal structure containing select requirements
 *
 * Returns: A dynamically allocated string representation of the select requirement,
 *          or NULL if none is specified. Handles both simple select strings and XOR expressions.
 */
static char * select2Str(struct resVal *resVal){
    char *selectStr = NULL;

    if (resVal->xorExprs) {
        int i = 0, size = 1024;
        selectStr = (char *) my_calloc(size, sizeof(char), "select2Str");

        for (i = 0; resVal->xorExprs[i] != NULL; i++) {
            char * tmpStr = NULL;
            tmpStr = convertTclSelectStr(resVal->xorExprs[i]);
            if (tmpStr == NULL) {
                continue;
            }

            if (size <= strlen(selectStr) + strlen(tmpStr) + 1) {
                size = size + strlen(tmpStr) + 512;
                selectStr = (char *) my_realloc(selectStr, size, "select2Str");
            }
            if (i == 0) {
                snprintf(selectStr + strlen(selectStr), size - strlen(selectStr), "%s", tmpStr);
            } else {
                snprintf(selectStr + strlen(selectStr), size - strlen(selectStr), ",%s", tmpStr);
            }
            FREEUP(tmpStr);
        }
    } else {
        if (resVal->selectStr) {
            selectStr = convertTclSelectStr(resVal->selectStr);
        }
    }
    return selectStr;
}

/*
 * convertSelectStr - Convert select string to simplified format
 * @selectStr: Input select string
 *
 * Converts a select string from the format used in resource requests to a
 * simplified format. Handles function calls, bracket expressions, and logical operators.
 *
 * Returns: Simplified select string on success, NULL on failure
 */
static char *convertTclSelectStr(char *selectStr)
{
    int inSize = strlen(selectStr);
    int outSize = inSize * 2; // Allocate enough space for the output
    char *outStr = NULL;
    int inPos = 0;
    int outPos = 0;
    char *key = NULL;
    char *op = NULL;
    char *value = NULL;

    if (selectStr == NULL) {
        return NULL;
    }

    outStr = (char *)my_malloc(outSize, "convertSelectStr");

    // Skip leading "expr" if present
    while (inPos < inSize && isspace(selectStr[inPos])) {
        inPos++;
    }
    if (strncmp(selectStr + inPos, "expr", 4) == 0) {
        inPos += 4;
        while (inPos < inSize && isspace(selectStr[inPos])) {
            inPos++;
        }
    }

    while (inPos < inSize) {
        char c = selectStr[inPos];

        // Skip whitespace
        if (isspace(c)) {
            inPos++;
            continue;
        }

        // Handle function calls: func()
        if (isalpha(c) || c == '_') {
            int start = inPos;
            while (inPos < inSize && (isalnum(selectStr[inPos]) || selectStr[inPos] == '_')) {
                inPos++;
            }

            // Check if this is a function call (followed by '()')
            if (inPos + 1 < inSize && selectStr[inPos] == '(' && selectStr[inPos + 1] == ')') {
                // Copy function name to output
                int len = inPos - start;
                if (outPos + len >= outSize) {
                    outSize += inSize;
                    outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
                }
                strncpy(outStr + outPos, selectStr + start, len);
                outPos += len;
                inPos += 2; // Skip '()'
            } else {
                // Not a function call, copy the characters
                if (outPos + (inPos - start) >= outSize) {
                    outSize += inSize;
                    outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
                }
                strncpy(outStr + outPos, selectStr + start, inPos - start);
                outPos += inPos - start;
            }
            continue;
        }

        // Handle bracket expressions: [key "eq" "value"] or [defined "key"]
        if (c == '[') {
            int keyStart, keyLen;

            inPos++;
            // Parse key
            while (inPos < inSize && isspace(selectStr[inPos])) {
                inPos++;
            }
            keyStart = inPos;
            while (inPos < inSize && !isspace(selectStr[inPos]) && selectStr[inPos] != '"') {
                inPos++;
            }
            keyLen = inPos - keyStart;
            key = (char *)my_malloc(keyLen + 1, "convertSelectStr");
            strncpy(key, selectStr + keyStart, keyLen);
            key[keyLen] = '\0';

            // Parse operator
            while (inPos < inSize && isspace(selectStr[inPos])) {
                inPos++;
            }
            if (inPos < inSize && selectStr[inPos] == '"') {
                int opStart, opLen;

                inPos++;
                opStart = inPos;
                while (inPos < inSize && selectStr[inPos] != '"') {
                    inPos++;
                }
                opLen = inPos - opStart;
                op = (char *)my_malloc(opLen + 1, "convertSelectStr");
                strncpy(op, selectStr + opStart, opLen);
                op[opLen] = '\0';
                inPos++;
            }

            // Parse value
            while (inPos < inSize && isspace(selectStr[inPos])) {
                inPos++;
            }
            if (inPos < inSize && selectStr[inPos] == '"') {
                int valStart;
                int valLen;

                inPos++;
                valStart = inPos;
                while (inPos < inSize && selectStr[inPos] != '"') {
                    inPos++;
                }
                valLen = inPos - valStart;
                value = (char *)my_malloc(valLen + 1, "convertSelectStr");
                strncpy(value, selectStr + valStart, valLen);
                value[valLen] = '\0';
                inPos++;
            }

            // Find closing bracket
            while (inPos < inSize && selectStr[inPos] != ']') {
                inPos++;
            }
            if (inPos < inSize) {
                inPos++;
            }

            if (key && op && strcmp(key, "defined") == 0) {
                //Convert [defined "res"] to defined(res)
                // Calculate required space: 
                // 9: defined()
                int needed = 9 + strlen(op); // value
                if (outPos + needed >= outSize) {
                    outSize += needed + inSize;
                    outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
                }
                // Write just the value to output
                snprintf(outStr + outPos, outSize - outPos,"defined(%s)", op);
                outPos += strlen(op) + 9;
            } else {
                // Convert to key == value or key != value
                if (key && op && value) {
                    // Regular [key "eq" "value"] or [key "ne" "value"] format
                    // Check if value needs quotes
                    int needQuotes = 0, needed = 0, i;

                    for (i = 0; value[i]; i++) {
                        if (!isalnum(value[i]) && value[i] != '_') {
                            needQuotes = 1;
                            break;
                        }
                    }

                    // Calculate required space
                    needed = strlen(key) + 4; // key + " == " or " != "
                    if (needQuotes) {
                        needed += strlen(value) + 2; // 'value'
                    } else {
                        needed += strlen(value); // value
                    }

                    if (outPos + needed >= outSize) {
                        outSize += needed + inSize;
                        outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
                    }

                    // Write to output
                    strcpy(outStr + outPos, key);
                    outPos += strlen(key);

                    if (strcmp(op, "eq") == 0) {
                        strcpy(outStr + outPos, " == ");
                        outPos += 4;
                    } else if (strcmp(op, "ne") == 0) {
                        strcpy(outStr + outPos, " != ");
                        outPos += 4;
                    } else if (strcmp(op, "gt") == 0) {
                        strcpy(outStr + outPos, " > ");
                        outPos += 3;
                    } else if (strcmp(op, "lt") == 0) {
                        strcpy(outStr + outPos, " < ");
                        outPos += 3;
                    } else if (strcmp(op, "ge") == 0) {
                        strcpy(outStr + outPos, " >= ");
                        outPos += 4;
                    } else if (strcmp(op, "le") == 0) {
                        strcpy(outStr + outPos, " <= ");
                        outPos += 4;
                    }

                    if (needQuotes) {
                        outStr[outPos++] = '\'';
                        strcpy(outStr + outPos, value);
                        outPos += strlen(value);
                        outStr[outPos++] = '\'';
                    } else {
                        strcpy(outStr + outPos, value);
                        outPos += strlen(value);
                    }
                
                }
            }

            // Free temporary memory
            FREEUP(key);
            FREEUP(op);
            FREEUP(value);
            continue;
        }

        // Handle logical operators && and ||
        if (c == '&' && inPos + 1 < inSize && selectStr[inPos + 1] == '&') {
            if (inPos + 2 < inSize && selectStr[inPos + 2] == '1') {
                inPos += 3;
                continue;
            }

            if (outPos + 4 >= outSize) {
                outSize += 4 + inSize;
                outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
            }
            strcpy(outStr + outPos, " && ");
            outPos += 4;
            inPos += 2;
            continue;
        }
        if (c == '|' && inPos + 1 < inSize && selectStr[inPos + 1] == '|') {
            if (outPos + 4 >= outSize) {
                outSize += 4 + inSize;
                outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
            }
            strcpy(outStr + outPos, " || ");
            outPos += 4;
            inPos += 2;
            continue;
        }

        // Handle parentheses
        if (c == '(') {
            // Check if this is an empty parentheses
            int nextPos = inPos + 1;
            while (nextPos < inSize && isspace(selectStr[nextPos])) {
                nextPos++;
            }
            if (nextPos < inSize && selectStr[nextPos] == ')') {
                // Empty parentheses, skip
                inPos = nextPos + 1;
            } else {
                // Parentheses with content, keep
                if (outPos + 2 >= outSize) {
                    outSize += 2 + inSize;
                    outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
                }
                outStr[outPos++] = '(';
                inPos++;
            }
            continue;
        }
        if (c == ')') {
            if (outPos + 2 >= outSize) {
                outSize += 2 + inSize;
                outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
            }
            outStr[outPos++] = ')';
            inPos++;
            continue;
        }

        // Handle other characters
        if (outPos + 1 >= outSize) {
            outSize += 1 + inSize;
            outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
        }
        outStr[outPos++] = c;
        inPos++;
    }

    // Add null terminator
    if (outPos >= outSize) {
        outSize++;
        outStr = (char *)my_realloc(outStr, outSize, "convertSelectStr");
    }
    outStr[outPos] = '\0';

    return outStr;
}

/*
 * order2Str - Convert an order requirement to a string
 * @resVal: Pointer to the resVal structure containing order requirements
 *
 * Returns: A dynamically allocated string representation of the order requirement
 *          (e.g., "r15s:pg"), or NULL if no order is specified.
 */
static char * order2Str(struct resVal *resVal){
    char *orderStr = NULL;
    
    if (resVal->nphase > 0) {
        int i = 0;
        orderStr = (char *) my_calloc(512, sizeof(char), "order2Str");
        if (orderStr == NULL) {
            return NULL;
        }

        for (i = 0; i < resVal->nphase; i++) {
            if (resVal->order[i] > 0) {
                if (i == 0) {
                    snprintf(orderStr + strlen(orderStr), 512 - strlen(orderStr), "%s", allLsInfo->resTable[resVal->order[i]-1].name);
                } else {
                    snprintf(orderStr + strlen(orderStr), 512 - strlen(orderStr), ":%s", allLsInfo->resTable[resVal->order[i]-1].name);
                }
            } else {
                 if (i == 0) {
                    snprintf(orderStr + strlen(orderStr), 512 - strlen(orderStr), "-%s", allLsInfo->resTable[-resVal->order[i]-1].name);
                } else {
                    snprintf(orderStr + strlen(orderStr), 512 - strlen(orderStr), ":-%s", allLsInfo->resTable[-resVal->order[i]-1].name);
                }               
            }
        }
    }
    
    return orderStr;
}

/*
 * rusage2Str - Convert a rusage requirement to a string
 * @resVal: Pointer to the resVal structure containing rusage requirements
 *
 * This function converts the rusage specifications (mem, swap, etc.) back into 
 * a string format (e.g., "mem=100:swap=200"). It also handles duration and decay.
 *
 * Returns: A dynamically allocated string representation of the rusage requirement.
 */
static char * rusage2Str(struct resVal *resVal){
    char *rusageStr = NULL;
    int  size = 512, i = 0, isSet = 0, curSize = 0;

    rusageStr = (char *) my_calloc(size, sizeof(char), "rusage2Str");

    for (i = 0; i < allLsInfo->nRes; i++) {
        TEST_BIT(i, resVal->rusgBitMaps, isSet);
        if (isSet) {
            curSize = strlen(rusageStr);
            /*22: the largest length of int
             *3: the length of '.00'
             *2: the length of  '=' and '\0'
             */
            if (curSize + strlen(allLsInfo->resTable[i].name) + 22 + 3 + 2 >= size) {
                size = size + strlen(allLsInfo->resTable[i].name) + 512;
                rusageStr = (char *) my_realloc(rusageStr, size, "rusage2Str");
            }
            snprintf(rusageStr + curSize,
                    size - curSize,
                    "%s%s=%0.2f", 
                    curSize == 0 ? "" : ":",
                    allLsInfo->resTable[i].name,
                    resVal->val[i]);
        }
    }

    curSize = strlen(rusageStr);
    if (curSize > 0 && resVal->duration != INFINIT_INT) {
        int len = snprintf(rusageStr + curSize, size - curSize, ":duration=%d", resVal->duration);
        if (len > 0 && (resVal->decay != INFINIT_FLOAT)) {
            snprintf(rusageStr + curSize + len, size - curSize - len, ":decay=%0.2f", resVal->decay);
        }
    }
    
    return rusageStr;
}

/*
 * span2Str - Convert a span requirement to a string
 * @resVal: Pointer to the resVal structure containing span requirements
 *
 * This function handles simple span requirements such as "ptile=num" or "hosts=num".
 *
 * Returns: A dynamically allocated string representation of the span requirement.
 */
static char * span2Str(struct resVal *resVal){
    char *spanStr = NULL;

    spanStr = (char *) my_calloc(512, sizeof(char), "span2Str");

    /* So far, span only support the following syntax:
     * ptile=num
     * hosts=num
     */
    if (resVal->pTile != INFINIT_INT) {
        snprintf(spanStr, 512, "ptile=%d", resVal->pTile);
    } else if (resVal->numHosts != INFINIT_INT) {
        snprintf(spanStr, 512, "hosts=%d", resVal->numHosts);
    }

    return spanStr;
}

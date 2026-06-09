#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>

#include "mbd.h"
#include "mbd.query.h"
#include "daemons.h"

extern bool_t xdr_submitReq(XDR *, struct submitReq *, struct LSFHeader *);

#define QMBD_SUBMIT_QUEUE_SIZE 1024

int qmbdSubmitChfd = -1;            /* Channel used for socket submit events. */
int qmbdCtrlChfd = -1;              /* Channel used for qmbd control events. */
int qmbdSubmitSenderState = QMBD_SUBMIT_STOPPED; /* Sender lifecycle state. */

struct qmbdSyncEntry {
    struct qmbdSyncEntry *next;     /* Next immutable replay entry. */
    struct jData *job;              /* qmbd-local replayed job object. */
};

struct qmbdEventMsg {
    char *buf;                      /* Encoded internal event buffer. */
    int len;                        /* Encoded buffer length in bytes. */
};

/*
 * Main mbd side state:
 * - the main mbd thread is the only producer of qmbdSubmitQueue;
 * - qmbdSenderThread() is the only consumer and the only writer of submit fd;
 * - ctrl events are deliberately kept off this queue and sent by main mbd.
 *
 * The queue itself remains lock-free under the SPSC assumption.  The semaphore
 * is only a wake counter; it does not protect queue head/tail.
 */
static pthread_t qmbdSenderTid;     /* Dedicated submit sender thread id. */
static sem_t qmbdSubmitWakeSem;     /* Wake counter for queued submit events. */
static unsigned int qmbdSubmitQueueHead = 0; /* Consumer-owned SPSC head. */
static unsigned int qmbdSubmitQueueTail = 0; /* Producer-owned SPSC tail. */
static struct qmbdEventMsg *qmbdSubmitQueue[QMBD_SUBMIT_QUEUE_SIZE]; /* SPSC ring. */
static struct qmbdSyncEntry qmbdSyncHead = { NULL, NULL };
static struct qmbdSyncEntry *qmbdSyncTail = &qmbdSyncHead;

static int sendQmbdSubmitReq(struct qmbdSubmitReq *);

/*
 * Grow the temporary result array used when qmbd merges replayed socket jobs
 * into a bjobs result.  The array only stores pointers owned by qmbd.
 * @param[in,out] joblist: Pointer to the result array.
 * @param[in] numJobs: Number of entries already stored.
 * @param[in,out] arraySize: Current allocated capacity.
 * @return: LSBE_NO_ERROR on success, LSBE_NO_MEM on allocation failure.
 */
static int qmbdEnsureJobListCapacity(struct jData ***joblist,
                                     int numJobs,
                                     int *arraySize)
{
    struct jData **biglist;

    if (*arraySize == 0) {
        *arraySize = 32;
        *joblist = (struct jData **)calloc(*arraySize, sizeof(struct jData *));
        if (*joblist == NULL)
            return LSBE_NO_MEM;
    } else if (numJobs >= *arraySize) {
        *arraySize *= 2;
        biglist = (struct jData **)realloc(*joblist,
                                           *arraySize * sizeof(struct jData *));
        if (biglist == NULL) {
            FREEUP(*joblist);
            return LSBE_NO_MEM;
        }
        *joblist = biglist;
    }

    return LSBE_NO_ERROR;
}

/*
 * Grow the temporary node result array used for array/job-group queries.
 * The nodeList entries point to replayed qmbd-local jData objects.
 * @param[in,out] list: Pointer to the nodeList array.
 * @param[in] numNodes: Number of entries already stored.
 * @param[in,out] arraySize: Current allocated capacity.
 * @return: LSBE_NO_ERROR on success, LSBE_NO_MEM on allocation failure.
 */
static int qmbdEnsureNodeListCapacity(struct nodeList **list,
                                      int numNodes,
                                      int *arraySize)
{
    struct nodeList *bigList;

    if (*arraySize == 0) {
        *arraySize = 32;
        *list = (struct nodeList *)calloc(*arraySize, sizeof(struct nodeList));
        if (*list == NULL)
            return LSBE_NO_MEM;
    } else if (numNodes >= *arraySize) {
        *arraySize *= 2;
        bigList = (struct nodeList *)realloc(*list,
                                             *arraySize * sizeof(struct nodeList));
        if (bigList == NULL) {
            FREEUP(*list);
            return LSBE_NO_MEM;
        }
        *list = bigList;
    }

    return LSBE_NO_ERROR;
}

/*
 * Return a safe string length for optional submitReq fields.
 * XDR buffer sizing treats NULL strings as empty strings.
 * @param[in] str: Optional string pointer.
 * @return: strlen(str), or 0 if str is NULL.
 */
static int qmbdSafeStrLen(char *str)
{
    return str ? strlen(str) : 0;
}

/*
 * Convert internal qmbd event opcodes to log-friendly names.
 * @param[in] reqType: qmbd internal request type.
 * @return: Static string name for the request type.
 */
static const char *qmbdReqTypeName(mbdReqType reqType)
{
    switch (reqType) {
        case QMBD_CTRL:
            return "QMBD_CTRL";
        case QMBD_SUBMIT:
            return "QMBD_SUBMIT";
        default:
            return "QMBD_UNKNOWN";
    }
}

/*
 * Match replayed qmbd jobs against the same status option classes used by
 * normal bjobs filtering.
 * @param[in] options: jobInfoReq option mask.
 * @param[in] jStatus: Job status bits.
 * @return: TRUE if the status matches, otherwise FALSE.
 */
static int qmbdMatchJobStatus(int options, int jStatus)
{
    if (options & (ALL_JOB|JOBID_ONLY_ALL))
        return TRUE;

    if (((options & CUR_JOB) || (options & LAST_JOB))
        && !(IS_FINISH(jStatus)))
        return TRUE;
    if ((options & DONE_JOB) && IS_FINISH(jStatus))
        return TRUE;
    if ((options & PEND_JOB) && IS_PEND(jStatus))
        return TRUE;
    if ((options & SUSP_JOB) && IS_SUSP(jStatus)
        && !(jStatus & JOB_STAT_UNKWN))
        return TRUE;
    if ((options & RUN_JOB) && (jStatus & JOB_STAT_RUN))
        return TRUE;
    if ((options & ZOMBIE_JOB) && (jStatus & JOB_STAT_ZOMBIE))
        return TRUE;

    return FALSE;
}

/*
 * Decide whether a replayed job should be skipped for job-group/array queries.
 * This mirrors the query-side status filtering before array index matching.
 * @param[in] options: jobInfoReq option mask.
 * @param[in] jStatus: Job status bits.
 * @return: TRUE if the job should be skipped, otherwise FALSE.
 */
static int qmbdSkipJgrpByReq(int options, int jStatus)
{
    if (options & (ALL_JOB|JOBID_ONLY_ALL|JOBID_ONLY))
        return FALSE;
    if ((options & (CUR_JOB | LAST_JOB))
        && (IS_START(jStatus) || IS_PEND(jStatus)))
        return FALSE;
    if ((options & PEND_JOB) && IS_PEND(jStatus))
        return FALSE;
    if ((options & (SUSP_JOB | RUN_JOB)) && IS_START(jStatus))
        return FALSE;
    if ((options & DONE_JOB) && IS_FINISH(jStatus))
        return FALSE;

    return TRUE;
}

/*
 * Match one replayed jData object against a jobInfoReq.
 * This is used only for jobs received through the qmbd socket sync list.
 * @param[in] job: Replayed qmbd-local job.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[in] allqueues: TRUE if queue filtering is disabled.
 * @param[in] allusers: TRUE if user filtering is disabled.
 * @param[in] allhosts: TRUE if host filtering is disabled.
 * @param[in] uGrp: Optional user group filter.
 * @param[in] uPtr: Optional user object for exact user matching.
 * @param[in] searchJobName: TRUE if jobName is a prefix wildcard query.
 * @return: TRUE if the job matches, otherwise FALSE.
 */
static int qmbdCheckJobMatch(struct jData *job,
                             struct jobInfoReq *jobInfoReq,
                             char allqueues,
                             char allusers,
                             char allhosts,
                             struct gData *uGrp,
                             struct uData *uPtr,
                             char searchJobName)
{
    char fullName[MAXPATHLEN];
    int i;
    int prefixLen = 0;

    if (job == NULL || job->jobId < 0 || job->qPtr == NULL || job->uPtr == NULL)
        return FALSE;

    if (!allqueues && strcmp(job->qPtr->queue, jobInfoReq->queue) != 0)
        return FALSE;

    if (!allusers && (job->uPtr != uPtr)) {
        if (uGrp == NULL)
            return FALSE;
        if (!gMember(job->userName, uGrp))
            return FALSE;
    }

    if (jobInfoReq->jobName[0] != '\0') {
        fullJobName_r(job, fullName);
        if (!searchJobName) {
            if (strcmp(jobInfoReq->jobName, fullName) != 0)
                return FALSE;
        } else {
            prefixLen = strlen(jobInfoReq->jobName) - 1;
            if (prefixLen < 0)
                prefixLen = 0;
            if (strncmp(fullName, jobInfoReq->jobName, prefixLen) != 0)
                return FALSE;
        }
    }

    if (jobInfoReq->jobId != 0) {
        if ((LSB_ARRAY_IDX(jobInfoReq->jobId) != 0
             && LSB_ARRAY_IDX(jobInfoReq->jobId) != LSB_ARRAY_IDX(job->jobId))
            || LSB_ARRAY_JOBID(jobInfoReq->jobId) != LSB_ARRAY_JOBID(job->jobId))
            return FALSE;
    }

    if (!qmbdMatchJobStatus(jobInfoReq->options, job->jStatus))
        return FALSE;

    if (!allhosts) {
        if (IS_PEND(job->jStatus))
            return FALSE;
        if (job->hPtr == NULL)
            return FALSE;
        for (i = 0; i < job->numHostPtr; i++) {
            if (job->hPtr[i] == NULL)
                continue;
            if (equalHost_(jobInfoReq->host, job->hPtr[i]->host))
                return TRUE;
        }
        return FALSE;
    }

    return TRUE;
}

/*
 * Match a replayed job or array element for bjobs -A/-J style selection.
 * Array index filters are applied after queue/user/host/status checks.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[in] job: Replayed qmbd-local job or array element.
 * @param[in] allqueues: TRUE if queue filtering is disabled.
 * @param[in] allusers: TRUE if user filtering is disabled.
 * @param[in] allhosts: TRUE if host filtering is disabled.
 * @param[in] uGrp: Optional user group filter.
 * @param[in] idxList: Optional parsed array index filter.
 * @return: TRUE if the job/group matches, otherwise FALSE.
 */
static int qmbdIsSelectedJgrp(struct jobInfoReq *jobInfoReq,
                              struct jData *job,
                              char allqueues,
                              char allusers,
                              char allhosts,
                              struct gData *uGrp,
                              struct idxList *idxList)
{
    struct idxList *idx;
    int i;

    if (job == NULL || job->qPtr == NULL)
        return FALSE;

    if (qmbdSkipJgrpByReq(jobInfoReq->options, job->jStatus))
        return FALSE;

    if (!allqueues && strcmp(job->qPtr->queue, jobInfoReq->queue) != 0)
        return FALSE;

    if (!allusers && strcmp(job->userName, jobInfoReq->userName) != 0) {
        if (uGrp == NULL)
            return FALSE;
        if (!gMember(job->userName, uGrp))
            return FALSE;
    }

    if (!allhosts) {
        if (IS_PEND(job->jStatus) || job->hPtr == NULL)
            return FALSE;

        for (i = 0; i < job->numHostPtr; i++) {
            if (job->hPtr[i] == NULL)
                return FALSE;
            if (equalHost_(jobInfoReq->host, job->hPtr[i]->host))
                break;
        }
        if (i >= job->numHostPtr)
            return FALSE;
    }

    if (idxList == NULL)
        return TRUE;

    for (idx = idxList; idx != NULL; idx = idx->next) {
        if (LSB_ARRAY_IDX(job->jobId) < idx->start
            || LSB_ARRAY_IDX(job->jobId) > idx->end)
            continue;
        if (((LSB_ARRAY_IDX(job->jobId) - idx->start) % idx->step) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Resolve the queue referenced by the submitted job.
 * qmbd replay does not create queues; it only links to existing qData.
 * @param[in] subReq: Submit request containing the queue name.
 * @return: Queue object on success, NULL if the queue cannot be resolved.
 */
static struct qData *qmbdResolveQueue(struct submitReq *subReq)
{
    if (subReq == NULL || subReq->queue == NULL || subReq->queue[0] == '\0')
        return NULL;

    return getQueueData(subReq->queue);
}

/*
 * Build the minimal job-group tree node required by query code for a single
 * replayed job.
 * @param[in] job: Replayed qmbd-local job.
 * @return: Newly allocated tree node, or NULL on allocation failure.
 */
static struct jgTreeNode *qmbdCreateJobNode(struct jData *job)
{
    struct jgTreeNode *node;

    node = treeNewNode(JGRP_NODE_JOB);
    if (node == NULL)
        return NULL;

    node->name = safeSave(job->shared->jobBill.jobName ?
                          job->shared->jobBill.jobName : "");
    node->parent = groupRoot;
    node->ndInfo = job;

    return node;
}

/*
 * Build the array root node used by query code for a replayed job array.
 * The root owns array metadata; elements reference the same node.
 * @param[in] job: Replayed qmbd-local array root.
 * @param[in] maxJLimit: Parsed maximum array concurrency limit.
 * @return: Newly allocated tree node, or NULL on allocation failure.
 */
static struct jgTreeNode *qmbdCreateArrayNode(struct jData *job, int maxJLimit)
{
    struct jgTreeNode *node;

    node = treeNewNode(JGRP_NODE_ARRAY);
    if (node == NULL)
        return NULL;

    node->name = safeSave(job->shared->jobBill.jobName ?
                          job->shared->jobBill.jobName : "");
    node->parent = groupRoot;
    ARRAY_DATA(node)->jobArray = job;
    ARRAY_DATA(node)->userId = job->userId;
    ARRAY_DATA(node)->userName = safeSave(job->userName);
    ARRAY_DATA(node)->maxJLimit = maxJLimit;
    if (job->shared->jobBill.options2 & SUB2_HOST_NT)
        ARRAY_DATA(node)->fromPlatform = AUTH_HOST_NT;
    else if (job->shared->jobBill.options2 & SUB2_HOST_UX)
        ARRAY_DATA(node)->fromPlatform = AUTH_HOST_UX;

    return node;
}

/*
 * Publish a successfully replayed job into qmbd's append-only sync list.
 * qmbd's main thread is the only writer; query worker threads only read.
 * The release store makes the fully initialized entry visible before readers
 * can observe it through qmbdSyncHead.next/entry->next.
 * @param[in] job: Replayed job or array root to publish.
 * @return: 0 on success, -1 on allocation failure.
 */
static int qmbdAppendSyncJob(struct jData *job)
{
    struct qmbdSyncEntry *entry;

    entry = (struct qmbdSyncEntry *)calloc(1, sizeof(struct qmbdSyncEntry));
    if (entry == NULL)
        return -1;

    entry->job = job;
    entry->next = NULL;
    __atomic_store_n(&qmbdSyncTail->next, entry, __ATOMIC_RELEASE);
    qmbdSyncTail = entry;

    return 0;
}

/*
 * Free a replayed job or array chain that has not been published, or that
 * failed while being published.
 * @param[in] job: Replayed job or array chain to free.
 * @return: none.
 */
static void qmbdFreeReplayJob(struct jData *job)
{
    struct jData *nextJob;

    if (job == NULL)
        return;

    if (job->jgrpNode != NULL) {
        if (job->jgrpNode->nodeType == JGRP_NODE_ARRAY) {
            FREEUP(ARRAY_DATA(job->jgrpNode)->userName);
            FREEUP(job->jgrpNode->ndInfo);
        }
        FREEUP(job->jgrpNode->name);
        FREEUP(job->jgrpNode);
    }

    while (job != NULL) {
        nextJob = job->nextJob;
        job->jgrpNode = NULL;
        job->nextJob = NULL;
        freeJData(job);
        job = nextJob;
    }
}

/*
 * Replay one submit event into the minimum qmbd-local jData shape needed by
 * query paths.  This does not execute scheduler-side submit side effects.
 * @param[in] req: Decoded qmbd submit replay request.
 * @return: Replayed qmbd-local job, or NULL on failure.
 */
static struct jData *qmbdReplaySingleSubmit(struct qmbdSubmitReq *req)
{
    struct jShared *shared;
    struct jData *job;

    if (req == NULL || req->userName == NULL || req->userName[0] == '\0')
        return NULL;

    shared = (struct jShared *)my_calloc(1, sizeof(struct jShared),
                                         "qmbdReplaySingleSubmit");
    job = initJData(shared);
    if (job == NULL)
        return NULL;

    job->jobId = req->baseJobId;
    job->userId = req->userId;
    job->userName = safeSave(req->userName);
    job->uPtr = getUserData(job->userName);
    if (job->uPtr == NULL) {
        ls_syslog(LOG_WARNING, "%s: cannot resolve user <%s> for job <%s>",
                  __func__, job->userName, lsb_jobid2str(job->jobId));
        qmbdFreeReplayJob(job);
        return NULL;
    }

    copyJobBill(&req->submitReq, &job->shared->jobBill, FALSE);
    job->shared->jobBill.submitTime = req->masterSubmitTime;
    job->qPtr = qmbdResolveQueue(&job->shared->jobBill);
    if (job->qPtr == NULL) {
        ls_syslog(LOG_WARNING, "%s: cannot resolve queue <%s> for job <%s>",
                  __func__,
                  job->shared->jobBill.queue ? job->shared->jobBill.queue : "",
                  lsb_jobid2str(job->jobId));
        qmbdFreeReplayJob(job);
        return NULL;
    }

    /*
     * Rebuild resource request parsing state because query filters may inspect
     * resValPtr; the encoded submitReq only carries the original string.
     */
    if (job->shared->jobBill.resReq && job->shared->jobBill.resReq[0] != '\0') {
        job->shared->resValPtr = checkResReq(job->shared->jobBill.resReq,
                                             USE_LOCAL | CHK_TCL_SYNTAX | PARSE_XOR);
        if (job->shared->resValPtr == NULL) {
            ls_syslog(LOG_WARNING, "%s: cannot parse resReq for job <%s>",
                      __func__, lsb_jobid2str(job->jobId));
            qmbdFreeReplayJob(job);
            return NULL;
        }
    }

    job->nodeType = JGRP_NODE_JOB;
    job->nextJob = NULL;
    job->jgrpNode = qmbdCreateJobNode(job);
    if (job->jgrpNode == NULL) {
        qmbdFreeReplayJob(job);
        return NULL;
    }

    /*
     * schedHost is not copied by every submit path in the same way, but query
     * output can use it.  Prefer the explicit sched host and fall back to host.
     */
    if (req->submitReq.schedHostType && req->submitReq.schedHostType[0] != '\0')
        job->schedHost = safeSave(req->submitReq.schedHostType);
    else if (req->submitReq.fromHost && req->submitReq.fromHost[0] != '\0')
        job->schedHost = safeSave(req->submitReq.fromHost);

    mkJobMergedResReqEntry(job);
    return job;
}

/*
 * Replay a submit event whose jobName contains an array expression.  The array
 * root keeps the base jobId; each element is linked through nextJob.
 * @param[in] req: Decoded qmbd submit replay request.
 * @param[in] idxList: Parsed array index list.
 * @param[in] maxJLimit: Parsed maximum array concurrency limit.
 * @return: Replayed qmbd-local array root, or NULL on failure.
 */
static struct jData *qmbdReplayArraySubmit(struct qmbdSubmitReq *req,
                                           struct idxList *idxList,
                                           int maxJLimit)
{
    struct jData *job;
    struct jData *tail;
    struct jData *elem;
    struct idxList *idxPtr;
    int idx;
    int numJobs = 0;
    int userPending = FALSE;

    job = qmbdReplaySingleSubmit(req);
    if (job == NULL)
        return NULL;

    if (job->jgrpNode != NULL) {
        FREEUP(job->jgrpNode->name);
        FREEUP(job->jgrpNode);
        job->jgrpNode = NULL;
    }

    job->nodeType = JGRP_NODE_ARRAY;
    job->jgrpNode = qmbdCreateArrayNode(job, maxJLimit);
    if (job->jgrpNode == NULL) {
        qmbdFreeReplayJob(job);
        return NULL;
    }

    userPending = (job->shared->jobBill.options2 & SUB2_HOLD) ? TRUE : FALSE;
    tail = job;
    for (idxPtr = idxList; idxPtr != NULL; idxPtr = idxPtr->next) {
        for (idx = idxPtr->start; idx <= idxPtr->end; idx += idxPtr->step) {
            /* copyJData preserves the root fields; fix per-element identity. */
            elem = copyJData(job);
            if (elem == NULL) {
                qmbdFreeReplayJob(job);
                return NULL;
            }
            tail->nextJob = elem;
            tail = elem;
            elem->nodeType = JGRP_NODE_JOB;
            elem->jobId = LSB_JOBID((LS_LONG_INT)job->jobId, idx);
            elem->jgrpNode = job->jgrpNode;
            if (userPending) {
                /* Held arrays must show pending-suspended elements in queries. */
                elem->newReason = PEND_USER_STOP;
                elem->oldReason = elem->newReason;
                elem->jStatus = JOB_STAT_PSUSP;
            }
            numJobs++;
        }
    }

    if (job->nextJob != NULL) {
        ARRAY_DATA(job->jgrpNode)->counts[getIndexOfJStatus(job->nextJob->jStatus)] = numJobs;
        ARRAY_DATA(job->jgrpNode)->counts[JGRP_COUNT_NJOBS] = numJobs;
    }

    return job;
}

/*
 * Decode the submit jobName shape and dispatch to single-job or array replay.
 * The original array expression is preserved for query-side name matching.
 * @param[in] req: Decoded qmbd submit replay request.
 * @return: Replayed qmbd-local job or array root, or NULL on failure.
 */
static struct jData *qmbdReplaySubmit(struct qmbdSubmitReq *req)
{
    struct idxList *idxList = NULL;
    struct jData *job;
    char *jobNameCopy = NULL;
    int err = LSBE_NO_ERROR;
    int maxJLimit = INFINIT_INT;
    int isArrayReq = FALSE;

    if (req == NULL)
        return NULL;

    if (!(req->submitReq.options & SUB_RESTART)
        && req->submitReq.jobName != NULL
        && strchr(req->submitReq.jobName, '[') != NULL) {
        isArrayReq = TRUE;
        jobNameCopy = safeSave(req->submitReq.jobName);
        if (jobNameCopy == NULL)
            return NULL;
        idxList = parseJobArrayIndex(jobNameCopy, &err, &maxJLimit);
        if (idxList == NULL) {
            if (err != LSBE_NO_ERROR) {
                ls_syslog(LOG_WARNING,
                          "%s: parseJobArrayIndex failed for job <%s> err=%d",
                          __func__, lsb_jobid2str(req->baseJobId), err);
                FREEUP(jobNameCopy);
                return NULL;
            }
            isArrayReq = FALSE;
        }
    }

    if (isArrayReq)
        job = qmbdReplayArraySubmit(req, idxList, maxJLimit);
    else
        job = qmbdReplaySingleSubmit(req);

    freeIdxList(idxList);
    FREEUP(jobNameCopy);
    return job;
}

/*
 * Free an encoded qmbd event after it has been sent or dropped.
 * @param[in] msg: Encoded qmbd event message.
 * @return: none.
 */
static void freeQmbdEventMsg(struct qmbdEventMsg *msg)
{
    if (msg == NULL)
        return;

    FREEUP(msg->buf);
    free(msg);
}

/*
 * Wake the submit sender thread without blocking the mbd submit path.
 * The semaphore is a wake counter: each enqueued submit message posts once,
 * and lifecycle state changes post once to break the sender out of sem_wait.
 * It is not used as the authoritative queue length; head/tail remain the
 * source of truth for queued messages.
 * Extra wakes are allowed.  Missing wakes are not allowed because they can
 * leave queued submit events unsent until the next lifecycle transition.
 * @return: none.
 */
static void qmbdWakeSubmitSender(void)
{
    if (sem_post(&qmbdSubmitWakeSem) < 0) {
        ls_syslog(LOG_WARNING, "%s: wake submit sender failed: %m",
                  __func__);
    }
}

/*
 * Wait until there is work or a state change for the submit sender.
 * EINTR is retried so signal delivery does not lose a submit wakeup.
 * @return: none.
 */
static void qmbdWaitSubmitSenderWake(void)
{
    int rc;

    do {
        rc = sem_wait(&qmbdSubmitWakeSem);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        ls_syslog(LOG_WARNING, "%s: wait submit sender wake failed: %m",
                  __func__);
    }
}

/*
 * Enqueue one encoded submit event into the lock-free SPSC ring buffer.
 * Producer is the main mbd thread; consumer is qmbdSenderThread().
 * The release store publishes the message pointer after the slot is filled.
 * The sender pairs it with an acquire load of tail before reading the slot.
 * @param[in] msg: Encoded qmbd event message to enqueue.
 * @return: 0 on success, -1 if the ring buffer is full.
 */
static int qmbdEnqueueSubmitMsg(struct qmbdEventMsg *msg)
{
    unsigned int head;
    unsigned int tail;
    unsigned int nextTail;

    /*
     * The producer owns tail, so it can read tail normally.  Head is advanced
     * by the consumer, so the producer acquires it before testing for full.
     */
    head = __atomic_load_n(&qmbdSubmitQueueHead, __ATOMIC_ACQUIRE);
    tail = qmbdSubmitQueueTail;
    nextTail = (tail + 1) % QMBD_SUBMIT_QUEUE_SIZE;
    if (nextTail == head)
        return -1;

    qmbdSubmitQueue[tail] = msg;
    /* Publish the slot contents before making the new tail visible. */
    __atomic_store_n(&qmbdSubmitQueueTail, nextTail, __ATOMIC_RELEASE);
    /*
     * The sender consumes at most one submit message per loop.  Post once for
     * each successful enqueue so a burst of N messages can wake N sends without
     * relying on an empty->non-empty transition, which has a lost-wakeup race.
     */
    qmbdWakeSubmitSender();
    return 0;
}

/*
 * Dequeue one encoded submit event from the SPSC ring buffer.
 * Only the sender thread advances the head index.
 * The release store exposes the freed slot to the producer only after the
 * message pointer has been removed from the ring.
 * @return: Encoded qmbd event message, or NULL if the queue is empty.
 */
static struct qmbdEventMsg *qmbdDequeueSubmitMsg(void)
{
    struct qmbdEventMsg *msg;
    unsigned int head;
    unsigned int tail;
    unsigned int nextHead;

    /*
     * The consumer owns head, so it can read head normally.  Tail is advanced
     * by the producer, so the consumer acquires it before reading a slot.
     */
    head = qmbdSubmitQueueHead;
    tail = __atomic_load_n(&qmbdSubmitQueueTail, __ATOMIC_ACQUIRE);
    if (head == tail)
        return NULL;

    msg = qmbdSubmitQueue[head];
    qmbdSubmitQueue[head] = NULL;
    nextHead = (head + 1) % QMBD_SUBMIT_QUEUE_SIZE;
    /* Release the slot before exposing the new head to the producer. */
    __atomic_store_n(&qmbdSubmitQueueHead, nextHead, __ATOMIC_RELEASE);
    return msg;
}

/*
 * Clean old-generation submit sync data during qmbd lifecycle switch.
 * This drops queued incremental events and drains wakeups that no longer
 * correspond to live submit work.  It runs on the sender side of the stop
 * barrier, before the main thread closes the old submit fd.
 * @return: none.
 */
static void qmbdCleanupSubmitSyncData(void)
{
    struct qmbdEventMsg *msg;
    int ret;

    for (;;) {
        msg = qmbdDequeueSubmitMsg();
        if (msg == NULL)
            break;
        freeQmbdEventMsg(msg);
    }

    for (;;) {
        ret = sem_trywait(&qmbdSubmitWakeSem);
        if (ret == 0)
            continue;

        if (errno == EINTR)
            continue;

        if (errno != EAGAIN) {
            ls_syslog(LOG_WARNING, "%s: drain submit sender wake failed: %m",
                      __func__);
        }
        break;
    }
}

/*
 * Estimate the XDR buffer size needed for qmbdSubmitReq.
 * The estimate is intentionally conservative to avoid resizing in submit path.
 * @param[in] req: qmbd submit replay request to size.
 * @return: Estimated buffer size in bytes.
 */
static int qmbdSubmitReqSize(struct qmbdSubmitReq *req)
{
    struct submitReq *subReq = &req->submitReq;
    int i;
    int sz;

    sz = 1024 + ALIGNWORD_(sizeof(struct qmbdSubmitReq));
    sz += ALIGNWORD_(sizeof(int) * 2);
    sz += ALIGNWORD_(qmbdSafeStrLen(req->userName) + 1) + 4;

    sz += ALIGNWORD_(qmbdSafeStrLen(subReq->queue) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->resReq) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->fromHost) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->dependCond) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->jobName) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->command) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->jobFile) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->inFile) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->outFile) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->errFile) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->inFileSpool) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->commandSpool) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->preExecCmd) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->postExecCmd) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->hostSpec) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->chkpntDir) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->subHomeDir) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->cwd) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->mailUser) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->projectName) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->loginShell) + 1) + 4 +
          ALIGNWORD_(qmbdSafeStrLen(subReq->schedHostType) + 1) + 4;

    for (i = 0; i < subReq->numAskedHosts; i++)
        sz += ALIGNWORD_(qmbdSafeStrLen(subReq->askedHosts[i]) + 1) + 4;

    for (i = 0; i < subReq->nxf; i++)
        sz += ALIGNWORD_(sizeof(struct xFile) + 4 * 4);

    return sz;
}

/*
 * Allocate fixed-size submitReq string buffers before XDR decode.
 * xdr_submitReq expects these destination buffers to exist.
 * @param[in,out] subReq: submitReq object prepared for XDR decode.
 * @return: 0 on success.
 */
static int qmbdInitSubmitReqForDecode(struct submitReq *subReq)
{
    static char fname[] = "qmbdInitSubmitReqForDecode";

    memset(subReq, 0, sizeof(*subReq));
    subReq->fromHost = (char *)my_malloc(MAXHOSTNAMELEN, fname);
    subReq->jobFile = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->inFile = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->outFile = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->errFile = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->inFileSpool = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->commandSpool = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->cwd = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->subHomeDir = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->chkpntDir = (char *)my_malloc(MAXFILENAMELEN, fname);
    subReq->hostSpec = (char *)my_malloc(MAXHOSTNAMELEN, fname);

    return 0;
}

/*
 * Encode one qmbd internal event with an LSF header.
 * The caller owns the returned buffer and must free it after send/drop.
 * @param[in] reqType: qmbd internal request type.
 * @param[in] req: Request payload to encode.
 * @param[in] xdrReq: Payload XDR codec.
 * @param[in] payloadSize: Estimated payload size.
 * @param[out] buf: Encoded message buffer.
 * @param[out] len: Encoded message length.
 * @return: 0 on success, -1 on allocation or encode failure.
 */
static int encodeQmbdReq(mbdReqType reqType,
                         void *req,
                         bool_t (*xdrReq)(XDR *, void *, struct LSFHeader *),
                         int payloadSize,
                         char **buf,
                         int *len)
{
    struct LSFHeader hdr;
    XDR xdrs;
    char *msgBuf;

    msgBuf = (char *)malloc(LSF_HEADER_LEN + payloadSize);
    if (msgBuf == NULL)
        return -1;

    initLSFHeader_(&hdr);
    hdr.opCode = reqType;

    xdrmem_create(&xdrs, msgBuf, LSF_HEADER_LEN + payloadSize, XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs, req, &hdr, xdrReq, 0, NULL)) {
        xdr_destroy(&xdrs);
        FREEUP(msgBuf);
        return -1;
    }

    *len = XDR_GETPOS(&xdrs);
    *buf = msgBuf;
    xdr_destroy(&xdrs);
    return 0;
}

/*
 * Dedicated sender for qmbd submit events.  It is the only thread that writes
 * submit messages to qmbd, preserving message order on the submit socketpair.
 *
 * The loop consumes exactly one wake per iteration.  A submit wake maps to at
 * most one dequeued message; a lifecycle wake may map to no message.  This
 * keeps semaphore count bounded by actual work plus lifecycle nudges.
 * @param[in] arg: Unused thread argument.
 * @return: NULL.
 */
static void *qmbdSenderThread(void *arg)
{
    struct qmbdEventMsg *msg;
    int sockfd;
    int writeRc;
    int state;
    int chfd;

    while (1) {
        /*
         * Consume one wake before inspecting state or queue contents.  Submit
         * enqueue posts once per message, while lifecycle transitions may post
         * extra wakes.  Extra wakes are harmless; they just lead to an empty
         * dequeue below instead of accumulating indefinitely.
         */
        qmbdWaitSubmitSenderWake();

        state = qmbdSubmitSenderState;
        if (state == QMBD_SUBMIT_STOP_REQUESTED) {
            /*
             * Stop is a lifecycle barrier: drop stale events before the parent
             * closes the old socketpair and forks the next qmbd.
             */
            qmbdCleanupSubmitSyncData();
            qmbdSubmitSenderState = QMBD_SUBMIT_STOPPED;
            continue;
        }

        if (state != QMBD_SUBMIT_ACTIVE)
            continue;

        msg = qmbdDequeueSubmitMsg();
        if (msg == NULL)
            continue;

        state = qmbdSubmitSenderState;
        if (state != QMBD_SUBMIT_ACTIVE) {
            freeQmbdEventMsg(msg);
            continue;
        }

        chfd = qmbdSubmitChfd;
        if (chfd < 0) {
            freeQmbdEventMsg(msg);
            continue;
        }

        sockfd = chanSock_(chfd);
        writeRc = chanWrite_(chfd, msg->buf, msg->len);
        if (logclass & LC_COMM) {
            if (writeRc == msg->len) {
                ls_syslog(LOG_DEBUG,
                          "%s: sent qmbd submit event chfd=%d sockfd=%d len=%d",
                          __func__, chfd, sockfd, msg->len);
            } else {
                ls_syslog(LOG_DEBUG,
                          "%s: failed to send qmbd submit event chfd=%d sockfd=%d len=%d rc=%d errno=%d",
                          __func__, chfd, sockfd, msg->len, writeRc, errno);
            }
        }
        /* Submit sync is best-effort; never retry here and block lifecycle. */
        freeQmbdEventMsg(msg);
    }

    return NULL;
}

/*
 * Wrap an encoded submit buffer and push it to the sender queue.
 * If qmbd is not active or the queue is full, the event is dropped.
 * Dropping here is intentional best effort: the authoritative job already
 * exists in main mbd and the next qmbd rebuild reloads it from the snapshot.
 * @param[in] buf: Encoded message buffer; ownership transfers to this function.
 * @param[in] len: Encoded message length.
 * @return: 0 on enqueue or best-effort drop, -1 on allocation failure.
 */
static int enqueueEncodedQmbdSubmit(char *buf, int len)
{
    struct qmbdEventMsg *msg;
    int state;

    msg = (struct qmbdEventMsg *)calloc(1, sizeof(struct qmbdEventMsg));
    if (msg == NULL) {
        FREEUP(buf);
        return -1;
    }

    msg->buf = buf;
    msg->len = len;

    state = qmbdSubmitSenderState;
    if (state != QMBD_SUBMIT_ACTIVE || qmbdSubmitChfd < 0) {
        /* No active qmbd can consume this incremental event. */
        freeQmbdEventMsg(msg);
        return 0;
    }

    if (qmbdEnqueueSubmitMsg(msg) < 0) {
        ls_syslog(LOG_WARNING,
                  "%s: qmbd submit queue full, drop incremental sync event",
                  __func__);
        freeQmbdEventMsg(msg);
        return 0;
    }

    return 0;
}

/*
 * Start the process-lifetime qmbd submit sender thread.
 * qmbd lifecycle changes only activate a new channel; they do not recreate
 * this sender thread.
 * @return: 0 on success, -1 on wake semaphore or thread creation failure.
 */
int initQmbdEventSender(void)
{
    int rc;

    if (sem_init(&qmbdSubmitWakeSem, 0, 0) < 0)
        return -1;

    rc = pthread_create(&qmbdSenderTid, NULL, qmbdSenderThread, NULL);
    if (rc != 0)
        return -1;

    return 0;
}

/*
 * Attach the current qmbd submit channel to the process-lifetime sender.
 * @param[in] chfd: Parent-side submit channel descriptor.
 * @return: none.
 */
void activateQmbdEventSender(int chfd)
{
    qmbdSubmitChfd = chfd;
    qmbdSubmitSenderState = QMBD_SUBMIT_ACTIVE;
    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG, "%s: qmbd submit sender active chfd=%d sockfd=%d",
                  __func__, chfd, chfd >= 0 ? chanSock_(chfd) : -1);
    }
    qmbdWakeSubmitSender();
}

/*
 * Ask the submit sender to stop asynchronously before the main thread closes
 * the old submit fd.
 * @return: none.
 */
void requestQmbdEventSenderStop(void)
{
    if (qmbdSubmitSenderState == QMBD_SUBMIT_ACTIVE) {
        qmbdSubmitSenderState = QMBD_SUBMIT_STOP_REQUESTED;
        qmbdWakeSubmitSender();
    }
}

/*
 * XDR codec for qmbd control events.
 * @param[in,out] xdrs: XDR stream.
 * @param[in,out] req: Control request payload.
 * @param[in] hdr: LSF message header.
 * @return: TRUE on XDR success, otherwise FALSE.
 */
static bool_t
xdr_qmbdCtrlReq(XDR *xdrs, struct qmbdCtrlReq *req, struct LSFHeader *hdr)
{
    return xdr_int(xdrs, &req->controlOp);
}

/*
 * XDR codec for qmbd submit replay requests.
 * jobId is split to the legacy 32-bit pair on the wire.
 * @param[in,out] xdrs: XDR stream.
 * @param[in,out] req: Submit replay request payload.
 * @param[in] hdr: LSF message header.
 * @return: TRUE on XDR success, otherwise FALSE.
 */
static bool_t
xdr_qmbdSubmitReq(XDR *xdrs, struct qmbdSubmitReq *req, struct LSFHeader *hdr)
{
    int jobId = 0;
    int elemId = 0;

    if (xdrs->x_op == XDR_ENCODE)
        jobId64To32(req->baseJobId, &jobId, &elemId);

    if (!xdr_int(xdrs, &jobId))
        return FALSE;
    if (!xdr_int(xdrs, &elemId))
        return FALSE;
    if (!xdr_time_t(xdrs, &req->masterSubmitTime))
        return FALSE;
    if (!xdr_int(xdrs, &req->userId))
        return FALSE;
    if (!xdr_string(xdrs, &req->userName, MAXLSFNAMELEN))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE)
        jobId32To64(&req->baseJobId, jobId, elemId);

    return xdr_submitReq(xdrs, &req->submitReq, hdr);
}

/*
 * Select replayed socket-sync jobs for bjobs result merging.
 * The returned array contains borrowed jData pointers owned by qmbd.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[out] jobDataList: Borrowed jData pointer array owned by caller.
 * @param[out] listSize: Number of selected jobs.
 * @return: LSBE_NO_ERROR if jobs are selected, otherwise an LSBE_* code.
 */
int selectQmbdSocketSyncJobs(struct jobInfoReq *jobInfoReq,
                             struct jData ***jobDataList,
                             int *listSize)
{
    struct qmbdSyncEntry *entry;
    struct jData **joblist = NULL;
    struct jData *recentJob = NULL;
    struct gData *uGrp = NULL;
    struct uData *uPtr = NULL;
    int numJobs = 0;
    int arraySize = 0;
    int ret;
    char allqueues = FALSE;
    char allusers = FALSE;
    char allhosts = FALSE;
    char searchJobName = FALSE;

    *jobDataList = NULL;
    *listSize = 0;

    if (jobInfoReq->queue[0] == '\0')
        allqueues = TRUE;
    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        allusers = TRUE;
    else
        uGrp = getUGrpData(jobInfoReq->userName);

    if (jobInfoReq->host[0] == '\0')
        allhosts = TRUE;
    if (jobInfoReq->jobName[0] != '\0'
        && jobInfoReq->jobName[strlen(jobInfoReq->jobName) - 1] == '*')
        searchJobName = TRUE;

    if (!allusers)
        uPtr = getUserData(jobInfoReq->userName);

    for (entry = __atomic_load_n(&qmbdSyncHead.next, __ATOMIC_ACQUIRE);
         entry != NULL;
         entry = __atomic_load_n(&entry->next, __ATOMIC_ACQUIRE)) {
        if (entry->job != NULL && entry->job->nodeType == JGRP_NODE_ARRAY) {
            struct jData *elem;

            /* Array roots are summaries; bjobs normally returns elements. */
            for (elem = entry->job->nextJob; elem != NULL; elem = elem->nextJob) {
                if (!qmbdCheckJobMatch(elem, jobInfoReq, allqueues, allusers,
                                       allhosts, uGrp, uPtr, searchJobName))
                    continue;
                if (findLastJob(jobInfoReq->options, elem, &recentJob) == FALSE)
                    continue;
                ret = qmbdEnsureJobListCapacity(&joblist, numJobs, &arraySize);
                if (ret != LSBE_NO_ERROR)
                    return ret;
                joblist[numJobs++] = elem;
            }
            continue;
        }

        if (!qmbdCheckJobMatch(entry->job, jobInfoReq, allqueues, allusers,
                               allhosts, uGrp, uPtr, searchJobName))
            continue;
        if (findLastJob(jobInfoReq->options, entry->job, &recentJob) == FALSE)
            continue;
        ret = qmbdEnsureJobListCapacity(&joblist, numJobs, &arraySize);
        if (ret != LSBE_NO_ERROR)
            return ret;
        joblist[numJobs++] = entry->job;
    }

    *listSize = numJobs;
    if (numJobs > 0) {
        if (jobInfoReq->options & LAST_JOB) {
            /*
             * This selector only owns socket-incremental jobs.  The caller
             * decides whether these post-snapshot jobs override base results.
             */
            joblist[0] = recentJob;
            *listSize = 1;
        }
        *jobDataList = joblist;
        return LSBE_NO_ERROR;
    }

    FREEUP(joblist);
    if (!allqueues && getQueueData(jobInfoReq->queue) == NULL)
        return LSBE_BAD_QUEUE;
    return LSBE_NO_JOB;
}

/*
 * Select replayed socket-sync array/job-group entries for bjobs -A and
 * job-name array queries.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[out] jgList: Borrowed nodeList array owned by caller.
 * @param[out] listSize: Number of selected nodes.
 * @return: LSBE_NO_ERROR if nodes are selected, otherwise an LSBE_* code.
 */
int selectQmbdSocketSyncJgrps(struct jobInfoReq *jobInfoReq,
                              struct nodeList **jgList,
                              int *listSize)
{
    struct qmbdSyncEntry *entry;
    struct nodeList *nodeList = NULL;
    struct gData *uGrp = NULL;
    struct idxList *idxList = NULL;
    char jobName[MAX_CMD_DESC_LEN];
    char allqueues = FALSE;
    char allusers = FALSE;
    char allhosts = FALSE;
    int numNodes = 0;
    int arraySize = 0;
    int maxJLimit = 0;
    int retError = LSBE_NO_ERROR;

    *jgList = NULL;
    *listSize = 0;
    jobName[0] = '\0';

    if (jobInfoReq->queue[0] == '\0')
        allqueues = TRUE;
    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        allusers = TRUE;
    else
        uGrp = getUGrpData(jobInfoReq->userName);
    if (jobInfoReq->host[0] == '\0')
        allhosts = TRUE;

    if (jobInfoReq->jobId != 0 && (jobInfoReq->options & JGRP_ARRAY_INFO)) {
        for (entry = __atomic_load_n(&qmbdSyncHead.next, __ATOMIC_ACQUIRE);
             entry != NULL;
             entry = __atomic_load_n(&entry->next, __ATOMIC_ACQUIRE)) {
            if (entry->job == NULL || entry->job->nodeType != JGRP_NODE_ARRAY)
                continue;
            if (entry->job->jobId != jobInfoReq->jobId)
                continue;
            if (qmbdEnsureNodeListCapacity(&nodeList, numNodes, &arraySize)
                != LSBE_NO_ERROR)
                return LSBE_NO_MEM;
            nodeList[numNodes].info = entry->job;
            nodeList[numNodes++].isJData = TRUE;
            *jgList = nodeList;
            *listSize = numNodes;
            return LSBE_NO_ERROR;
        }
        return LSBE_NO_JOB;
    }

    /*
     * parseJobArrayIndex() expects an array-style expression.  "/" is the
     * existing wildcard marker for all job arrays.
     */
    if ((strlen(jobInfoReq->jobName) == 1) && (jobInfoReq->jobName[0] == '/'))
        strcpy(jobName, "*");
    else
        ls_strcat(jobName, sizeof(jobName), jobInfoReq->jobName);

    idxList = parseJobArrayIndex(jobName, &retError, &maxJLimit);
    if (idxList == NULL && retError != LSBE_NO_ERROR)
        return retError;

    for (entry = __atomic_load_n(&qmbdSyncHead.next, __ATOMIC_ACQUIRE);
         entry != NULL;
         entry = __atomic_load_n(&entry->next, __ATOMIC_ACQUIRE)) {
        struct jData *elem;
        if (entry->job == NULL)
            continue;

        if (entry->job->nodeType == JGRP_NODE_ARRAY) {
            if (jobInfoReq->options & JGRP_ARRAY_INFO) {
                /* Summary queries return the array root, not each element. */
                if (strlen(jobName)
                    && !matchName(jobName, entry->job->jgrpNode->name))
                    continue;
                if (!qmbdIsSelectedJgrp(jobInfoReq, entry->job, allqueues,
                                        allusers, allhosts, uGrp, idxList))
                    continue;
                if (qmbdEnsureNodeListCapacity(&nodeList, numNodes, &arraySize)
                    != LSBE_NO_ERROR) {
                    freeIdxList(idxList);
                    return LSBE_NO_MEM;
                }
                nodeList[numNodes].info = entry->job;
                nodeList[numNodes++].isJData = TRUE;
                continue;
            }

            /* Element queries expand the replayed array through nextJob. */
            if (strlen(jobName)
                       && !matchName(jobName, entry->job->jgrpNode->name)) {
                continue;
            }

            for (elem = entry->job->nextJob; elem != NULL; elem = elem->nextJob) {
                if (!qmbdIsSelectedJgrp(jobInfoReq, elem, allqueues,
                                        allusers, allhosts, uGrp, idxList))
                    continue;
                if (qmbdEnsureNodeListCapacity(&nodeList, numNodes, &arraySize)
                    != LSBE_NO_ERROR) {
                    freeIdxList(idxList);
                    return LSBE_NO_MEM;
                }
                nodeList[numNodes].info = elem;
                nodeList[numNodes++].isJData = TRUE;
            }
            continue;
        }

        if (jobInfoReq->options & JGRP_ARRAY_INFO)
            continue;

        if (strlen(jobName)
            && !matchName(jobName, entry->job->jgrpNode->name))
            continue;

        if (!qmbdIsSelectedJgrp(jobInfoReq, entry->job, allqueues,
                                allusers, allhosts, uGrp, idxList))
            continue;

        if (qmbdEnsureNodeListCapacity(&nodeList, numNodes, &arraySize)
            != LSBE_NO_ERROR) {
            freeIdxList(idxList);
            return LSBE_NO_MEM;
        }
        nodeList[numNodes].info = entry->job;
        nodeList[numNodes++].isJData = TRUE;
    }

    freeIdxList(idxList);
    if (numNodes > 0) {
        *jgList = nodeList;
        *listSize = numNodes;
        return LSBE_NO_ERROR;
    }

    FREEUP(nodeList);
    return LSBE_NO_JOB;
}

/*
 * Send one control event synchronously on the dedicated ctrl socketpair.
 * Control events are small and rare; submit events use the sender thread.
 * If the write fails, the caller must still close the old ctrl fd so qmbd can
 * observe EPOLLHUP/EPOLLERR and enter expiration.  This failure is a qmbd
 * lifecycle cleanup issue, not a reason to kill the main mbd.
 * @param[in] req: qmbd control request to send.
 * @return: 0 on full write, -1 on encode or write failure.
 */
int sendQmbdCtrlReq(struct qmbdCtrlReq *req)
{
    char *buf = NULL;
    int len = 0;
    int writeRc;
    int sockfd;

    if (qmbdCtrlChfd < 0)
        return -1;

    if (encodeQmbdReq(QMBD_CTRL, req, (bool_t (*)())xdr_qmbdCtrlReq,
                      128, &buf, &len) < 0)
        return -1;

    sockfd = chanSock_(qmbdCtrlChfd);
    writeRc = chanWrite_(qmbdCtrlChfd, buf, len);
    if (writeRc == len) {
        if (logclass & LC_COMM) {
            ls_syslog(LOG_DEBUG,
                      "%s: sent qmbd ctrl event op=%d chfd=%d sockfd=%d len=%d",
                      __func__, req->controlOp, qmbdCtrlChfd, sockfd, len);
        }
        FREEUP(buf);
        return 0;
    }

    ls_syslog(LOG_WARNING,
              "%s: failed to send qmbd ctrl event op=%d chfd=%d sockfd=%d len=%d rc=%d errno=%d",
              __func__, req->controlOp, qmbdCtrlChfd, sockfd, len, writeRc, errno);
    FREEUP(buf);
    return -1;
}

/*
 * Convert a committed main-mbd job into a qmbd submit replay request.
 * The request is best-effort and must not change submit success/failure.
 * Dropping this event only affects the current qmbd's immediate visibility;
 * the next qmbd rebuild reloads authoritative state from the main mbd.
 * @param[in] subReq: Original submit request.
 * @param[in] job: Master mbd job object after commit.
 * @return: 0 on enqueue or best-effort drop, -1 on encode/setup failure.
 */
int
qmbdSocketSyncSubmitJob(struct submitReq *subReq, struct jData *job)
{
    struct qmbdSubmitReq qmbdSubmitReq;

    if (!qmbd_port || isQmbd || subReq == NULL || job == NULL)
        return 0;

    memset(&qmbdSubmitReq, 0, sizeof(qmbdSubmitReq));
    /*
     * qmbd must not allocate a new jobId or use local time; both values must
     * match the master mbd's committed job.
     */
    qmbdSubmitReq.baseJobId = job->jobId;
    qmbdSubmitReq.masterSubmitTime = job->shared ?
                                     job->shared->jobBill.submitTime : 0;
    qmbdSubmitReq.userId = job->userId;
    qmbdSubmitReq.userName = job->userName;
    qmbdSubmitReq.submitReq = *subReq;

    return sendQmbdSubmitReq(&qmbdSubmitReq);
}

/*
 * Encode and enqueue one qmbd submit replay event.
 * @param[in] req: qmbd submit replay request.
 * @return: 0 on enqueue or best-effort drop, -1 on encode/setup failure.
 */
static int
sendQmbdSubmitReq(struct qmbdSubmitReq *req)
{
    char *buf = NULL;
    int len = 0;

    if (encodeQmbdReq(QMBD_SUBMIT, req, (bool_t (*)())xdr_qmbdSubmitReq,
                      qmbdSubmitReqSize(req), &buf, &len) < 0)
        return -1;

    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG, "%s: enqueue qmbd submit event jobId=%s jobName=%s",
                  __func__, lsb_jobid2str(req->baseJobId),
                  req->submitReq.jobName ? req->submitReq.jobName : "");
    }
    return enqueueEncodedQmbdSubmit(buf, len);
}

/*
 * Execute one decoded qmbd control request.
 * A non-zero return means the qmbd should enter expiration.
 * @param[in] req: Decoded qmbd control request.
 * @return: 1 if qmbd should expire, 0 to continue.
 */
static int handleQmbdCtrlReq(struct qmbdCtrlReq *req)
{
    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG, "%s: received qmbd ctrl event op=%d",
                  __func__, req->controlOp);
    }
    if (req->controlOp == QMBD_CTRL_EXIT)
        return 1;

    ls_syslog(LOG_WARNING, "%s: unknown qmbd control op %d",
              __func__, req->controlOp);
    return 0;
}

/*
 * Read, decode, and dispatch one qmbd internal event from the given channel.
 * expectedReqType prevents ctrl and submit channels from accepting each other.
 * @param[in] chfd: qmbd internal event channel descriptor.
 * @param[in] expectedReqType: Event type expected on this channel.
 * @return: 0 on success, non-zero on handler failure or expiration request.
 */
static int handleQmbdInternalEventIO(int chfd, mbdReqType expectedReqType)
{
    struct LSFHeader hdr;
    struct Buffer *buf = NULL;
    XDR msgXdr;
    int ret = 0;

    if (chfd < 0)
        return -1;

    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG, "%s: read qmbd internal event chfd=%d sockfd=%d",
                  __func__, chfd, chanSock_(chfd));
    }
    if (chanDequeue_(chfd, &buf) < 0) {
        ls_syslog(LOG_ERR,
                  "%s: failed to dequeue qmbd internal event chfd=%d sockfd=%d cherrno=%d errno=%d",
                  __func__, chfd, chanSock_(chfd), cherrno, errno);
        return -1;
    }

    xdrmem_create(&msgXdr, buf->data, buf->len, XDR_DECODE);
    if (!xdr_LSFHeader(&msgXdr, &hdr)) {
        ls_syslog(LOG_ERR,
                  "%s: failed to decode qmbd internal event header chfd=%d sockfd=%d len=%d",
                  __func__, chfd, chanSock_(chfd), buf->len);
        xdr_destroy(&msgXdr);
        chanFreeBuf_(buf);
        return -1;
    }

    /* The channel already framed the message; XDR starts at the LSF header. */
    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG,
                  "%s: decoded qmbd internal event type=%s opcode=%d len=%u version=%u",
                  __func__, qmbdReqTypeName(hdr.opCode),
                  hdr.opCode, hdr.length, hdr.version);
    }
    if (hdr.opCode != expectedReqType) {
        ls_syslog(LOG_WARNING,
                  "%s: unexpected qmbd opcode %d on %s channel",
                  __func__, hdr.opCode, qmbdReqTypeName(expectedReqType));
        xdr_destroy(&msgXdr);
        chanFreeBuf_(buf);
        return -1;
    }

    switch (hdr.opCode) {
        case QMBD_CTRL: {
            struct qmbdCtrlReq req;
            memset(&req, 0, sizeof(req));
            if (!xdr_qmbdCtrlReq(&msgXdr, &req, &hdr)) {
                ret = -1;
                break;
            }
            ret = handleQmbdCtrlReq(&req);
            break;
        }
        case QMBD_SUBMIT: {
            struct qmbdSubmitReq req;
            struct jData *job;

            memset(&req, 0, sizeof(req));
            /* submitReq contains many owned strings; prepare decode storage. */
            qmbdInitSubmitReqForDecode(&req.submitReq);
            if (!xdr_qmbdSubmitReq(&msgXdr, &req, &hdr)) {
                ret = -1;
                FREEUP(req.userName);
                freeSubmitReq(&req.submitReq);
                break;
            }

            if (logclass & LC_COMM) {
                ls_syslog(LOG_DEBUG,
                          "%s: received qmbd submit event jobId=%s user=%s payloadLen=%u",
                          __func__, lsb_jobid2str(req.baseJobId),
                          req.userName ? req.userName : "",
                          hdr.length);
            }
            job = qmbdReplaySubmit(&req);
            if (job == NULL) {
                ls_syslog(LOG_WARNING,
                          "%s: failed to replay qmbd submit for job <%s>",
                          __func__, lsb_jobid2str(req.baseJobId));
                ret = -1;
            } else if (qmbdAppendSyncJob(job) < 0) {
                ls_syslog(LOG_WARNING,
                          "%s: failed to append qmbd sync job <%s>",
                          __func__, lsb_jobid2str(req.baseJobId));
                qmbdFreeReplayJob(job);
                ret = -1;
            } else {
                if (logclass & LC_COMM) {
                    ls_syslog(LOG_DEBUG,
                              "%s: appended qmbd replayed job jobId=%s queue=%s user=%s",
                              __func__, lsb_jobid2str(job->jobId),
                              job->qPtr ? job->qPtr->queue : "",
                              job->userName ? job->userName : "");
                }
                ret = 0;
            }

            FREEUP(req.userName);
            /* Free only the decoded request storage, not replayed jData. */
            freeSubmitReq(&req.submitReq);
            break;
        }
        default:
            ls_syslog(LOG_WARNING, "%s: unsupported qmbd internal opcode %d",
                      __func__, hdr.opCode);
            ret = -1;
            break;
    }

    xdr_destroy(&msgXdr);
    chanFreeBuf_(buf);
    return ret;
}

/*
 * Process one readable submit event from qmbd's epoll loop.
 * Replay failures are local to qmbd.  They drop the current incremental event
 * and must not affect the main mbd's committed job state.
 * @return: 0 on success, non-zero on handler failure.
 */
int handleQmbdSubmitEventIO(void)
{
    return handleQmbdInternalEventIO(qmbdSubmitChfd, QMBD_SUBMIT);
}

/*
 * Process one readable control event from qmbd's epoll loop.
 * @return: 0 to continue, non-zero to expire qmbd.
 */
int handleQmbdCtrlEventIO(void)
{
    return handleQmbdInternalEventIO(qmbdCtrlChfd, QMBD_CTRL);
}

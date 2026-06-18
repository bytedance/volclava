#include "mbd.h"
#include "mbd.query.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

extern int ensureJobListCapacity(void ***, int, int *, int);
extern int packJobInfo(struct jData *, int, char **, int, int, int);
extern int findLastJob(int, struct jData *, struct jData **);

long long syncShmXdrBufferSize = 0;     /* SHM XDR ring size in bytes. */
long long syncShmJobNameBufferSize = 0; /* SHM job-name ring size in bytes. */
int syncShmJobCapacity = 0;             /* Number of SHM metadata slots. */
struct syncJobShm *shm = NULL;          /* Attached SHM sync state. */

static int shmCleanerStarted = 0;       /* Nonzero after cleaner thread start. */
static pthread_mutex_t shmInitLock = PTHREAD_MUTEX_INITIALIZER; /* SHM init guard. */

static int isJobStale(struct jobMetaData *job);
static int getJobQueueCount(struct jobMetaQueue *queue);
static long long getBufferUsedBytes(struct bufferQueue *queue);
static int getShmAttachCount(int shmid);
static void removeHead(void);
static int createShmCleanerThread(void);
static void *shmCleaner(void *arg);
static int checkQmbdShmJobMatch(void *, struct jobInfoReq *, char, char, char,
                                struct gData *, struct uData *, char,
                                enum jobSelectType);
static int checkQmbdShmJgrpMatch(struct jobInfoReq *, struct jobMetaData *,
                                 char, char, char, struct gData *,
                                 struct idxList *);

/*
 * Allocate and lay out the shared-memory queues used by the SHM sync backend.
 * @return: Attached shared-memory descriptor, or NULL on failure.
 */
static struct syncJobShm *
initSyncShm(void)
{
    struct syncJobShm *newShm = NULL;
    void *shmBase = NULL;
    int id = 0;
    long long totSize = 0;
    key_t key = IPC_PRIVATE;

    totSize = sizeof(struct jobMetaQueue)
            + syncShmJobCapacity * sizeof(struct jobMetaData)
            + sizeof(struct bufferQueue) + syncShmXdrBufferSize
            + sizeof(struct bufferQueue) + syncShmJobNameBufferSize
            + sizeof(struct syncJobShm);

    id = shmget(key, totSize, IPC_CREAT | 0666);
    if (id == -1) {
        ls_syslog(LOG_ERR, "%s: shmget failed: %s", __func__, strerror(errno));
        return NULL;
    }

    shmBase = shmat(id, NULL, 0);
    if (shmBase == (void *)-1) {
        ls_syslog(LOG_ERR, "%s: shmat failed: %s", __func__, strerror(errno));
        shmctl(id, IPC_RMID, NULL);
        return NULL;
    }

    if (shmctl(id, IPC_RMID, NULL) == -1) {
        ls_syslog(LOG_WARNING, "%s: shmctl IPC_RMID failed: %s",
                  __func__, strerror(errno));
    }

    memset(shmBase, 0, totSize);

    newShm = (struct syncJobShm *)shmBase;
    newShm->jobMetaQueue = (struct jobMetaQueue *)(shmBase + sizeof(struct syncJobShm));
    newShm->jobMetaQueue->capacity = syncShmJobCapacity;
    newShm->xdrBuffer = (struct bufferQueue *)(shmBase + sizeof(struct syncJobShm)
                    + sizeof(struct jobMetaQueue)
                    + syncShmJobCapacity * sizeof(struct jobMetaData));
    newShm->xdrBuffer->capacity = syncShmXdrBufferSize;
    newShm->jobNameBuffer = (struct bufferQueue *)(shmBase + sizeof(struct syncJobShm)
                    + sizeof(struct jobMetaQueue)
                    + syncShmJobCapacity * sizeof(struct jobMetaData)
                    + sizeof(struct bufferQueue)
                    + syncShmXdrBufferSize);
    newShm->jobNameBuffer->capacity = syncShmJobNameBufferSize;
    newShm->shmId = id;

    return newShm;
}

/*
 * Initialize SHM sync backend state and start the cleaner in the main mbd.
 * @return: 0 on success, -1 on shared-memory or cleaner startup failure.
 */
int
initQmbdShmSync(void)
{
    int rc = 0;

    pthread_mutex_lock(&shmInitLock);
    if (shm == NULL) {
        shm = initSyncShm();
        if (shm == NULL) {
            pthread_mutex_unlock(&shmInitLock);
            return -1;
        }
    }

    if (!shmCleanerStarted && !isQmbd) {
        rc = createShmCleanerThread();
        if (rc == 0)
            shmCleanerStarted = 1;
    }
    pthread_mutex_unlock(&shmInitLock);

    return rc;
}

/*
 * Register a newly forked qmbd as a reader of the SHM ring.
 * The readStartIndex protects entries that may still be visible to this qmbd.
 * @param[in] pid: qmbd process id.
 * @param[in] forkTime: qmbd fork timestamp.
 * @return: 0 on success, -1 if no reader slot is available.
 */
int
registerReaderToShm(pid_t pid, time_t forkTime)
{
    int i;
    int slot = -1;

    if (shm == NULL)
        return -1;

    for (i = 0; i < MAX_QMBD_NUMS; i++) {
        if (!shm->queryReaderInfo[i].valid) {
            slot = i;
            break;
        }
    }

    if (slot < 0)
        return -1;

    shm->queryReaderInfo[slot].pid = pid;
    shm->queryReaderInfo[slot].forkTime = forkTime;
    shm->queryReaderInfo[slot].readStartIndex = shm->jobMetaQueue->tail;
    shm->queryReaderInfo[slot].valid = 1;

    return 0;
}

/*
 * Copy a job name out of the SHM circular job-name buffer.
 * @param[in] meta: Metadata entry containing the name offset and length.
 * @param[out] dest: Destination buffer for the copied name.
 * @return: none.
 */
static void
getShmJobName(struct jobMetaData *meta, char *dest)
{
    int wrapLen;

    if (shm == NULL || meta == NULL || dest == NULL)
        return;

    if (meta->jobNameOffset + meta->jobNameLen > shm->jobNameBuffer->capacity) {
        wrapLen = shm->jobNameBuffer->capacity - meta->jobNameOffset;
        memcpy(dest, &shm->jobNameBuffer->buff[meta->jobNameOffset], wrapLen);
        memcpy(dest + wrapLen, &shm->jobNameBuffer->buff[0],
               meta->jobNameLen - wrapLen);
    } else {
        memcpy(dest, &shm->jobNameBuffer->buff[meta->jobNameOffset],
               meta->jobNameLen);
    }
}

/*
 * Return the kernel attachment count for the SHM segment.
 * @param[in] shmid: Shared-memory id.
 * @return: Attachment count, or -1 on shmctl failure.
 */
static int
getShmAttachCount(int shmid)
{
    struct shmid_ds shm_info;

    if (shmctl(shmid, IPC_STAT, &shm_info) == -1)
        return -1;

    return shm_info.shm_nattch;
}

/*
 * Remove the oldest metadata entry and advance all backing ring heads.
 * @return: none.
 */
static void
removeHead(void)
{
    struct jobMetaData *jobMeta = NULL;
    struct jobMetaData *nextJobMeta = NULL;
    int nextMetaHead;

    if (shm == NULL || shm->jobMetaQueue->head == shm->jobMetaQueue->tail)
        return;

    jobMeta = &shm->jobMetaQueue->jobUnit[shm->jobMetaQueue->head];
    jobMeta->state = SHM_BLOCK_STATUS_UNUSED;

    shm->xdrBuffer->head = (shm->xdrBuffer->head + jobMeta->xdrLen)
                         % shm->xdrBuffer->capacity;

    nextMetaHead = (shm->jobMetaQueue->head + 1) % shm->jobMetaQueue->capacity;
    if (nextMetaHead != shm->jobMetaQueue->tail) {
        nextJobMeta = &shm->jobMetaQueue->jobUnit[nextMetaHead];
        if (nextJobMeta->jobNameOffset != jobMeta->jobNameOffset) {
            shm->jobNameBuffer->head = (shm->jobNameBuffer->head
                                      + jobMeta->jobNameLen)
                                     % shm->jobNameBuffer->capacity;
        }
    } else {
        shm->jobNameBuffer->head = (shm->jobNameBuffer->head
                                  + jobMeta->jobNameLen)
                                 % shm->jobNameBuffer->capacity;
    }

    shm->jobMetaQueue->head = (shm->jobMetaQueue->head + 1)
                            % shm->jobMetaQueue->capacity;
}

/*
 * Serialize one committed job into the SHM metadata and XDR rings.
 * parentIndex links array elements back to their array root metadata entry.
 * @param[in] job: Master mbd job to serialize.
 * @param[in] parentIndex: Array root metadata index, or -1 for root/single job.
 * @return: Metadata index on success, -1 on failure.
 */
static int
addJobToSyncShm(struct jData *job, int parentIndex)
{
    char *xdrBuf = NULL;
    int xdrLen, i;
    long long usedSpace;
    static struct jgTreeNode *lastJgrpNode = NULL;
    static long long lastJobNameOffset = -1;
    static int lastJobNameLen = 0;
    static int lastMetaTail = -1;
    char jobName[MAXPATHLEN];
    int jobNameLen;
    long long jobNameOffset = -1;
    long long wrapLen;
    int metaTail;
    struct jobMetaData *jobMeta;
    long long xdrOffset;

    if (shm == NULL)
        return -1;
    if (getShmAttachCount(shm->shmId) <= 1)
        return -1;

    xdrLen = packJobInfo(job, 0, &xdrBuf, 0, 0, 0);
    if (xdrLen < 0 || xdrBuf == NULL)
        return -1;

    usedSpace = getJobQueueCount(shm->jobMetaQueue);
    if (usedSpace + 1 > shm->jobMetaQueue->capacity - 1) {
        FREEUP(xdrBuf);
        return -1;
    }

    usedSpace = getBufferUsedBytes(shm->xdrBuffer);
    if (usedSpace + xdrLen > shm->xdrBuffer->capacity - 1) {
        FREEUP(xdrBuf);
        return -1;
    }

    fullJobName_r(job, jobName);
    jobNameLen = strlen(jobName) + 1;

    if (job->jgrpNode && job->jgrpNode == lastJgrpNode
        && lastJobNameOffset != -1
        && lastMetaTail == ((shm->jobMetaQueue->tail - 1
                           + shm->jobMetaQueue->capacity)
                          % shm->jobMetaQueue->capacity)
        && shm->jobMetaQueue->jobUnit[lastMetaTail].state == SHM_BLOCK_STATUS_USED) {
        /*
         * Consecutive array elements share the same full job name.  Reuse the
         * previous name slot to reduce pressure on the job-name ring.
         */
        jobNameOffset = lastJobNameOffset;
        jobNameLen = lastJobNameLen;
    } else {
        usedSpace = getBufferUsedBytes(shm->jobNameBuffer);
        if (usedSpace + jobNameLen > shm->jobNameBuffer->capacity - 1) {
            FREEUP(xdrBuf);
            return -1;
        }
        jobNameOffset = shm->jobNameBuffer->tail;
        if (jobNameOffset + jobNameLen > shm->jobNameBuffer->capacity - 1) {
            wrapLen = shm->jobNameBuffer->capacity - jobNameOffset;
            memcpy(&shm->jobNameBuffer->buff[jobNameOffset], jobName, wrapLen);
            memcpy(&shm->jobNameBuffer->buff[0], jobName + wrapLen,
                   jobNameLen - wrapLen);
        } else {
            memcpy(&shm->jobNameBuffer->buff[jobNameOffset], jobName, jobNameLen);
        }
        shm->jobNameBuffer->tail = (jobNameOffset + jobNameLen)
                                 % shm->jobNameBuffer->capacity;

        if (job->jgrpNode) {
            lastJgrpNode = job->jgrpNode;
            lastJobNameOffset = jobNameOffset;
            lastJobNameLen = jobNameLen;
        } else {
            lastJgrpNode = NULL;
            lastJobNameOffset = -1;
        }
    }

    metaTail = shm->jobMetaQueue->tail;
    lastMetaTail = metaTail;
    jobMeta = &shm->jobMetaQueue->jobUnit[metaTail];

    jobMeta->nextJgrpIndex = (metaTail + 1) % shm->jobMetaQueue->capacity;
    jobMeta->type = job->nodeType;
    /* Publish metadata only after all fields and XDR payload are copied. */
    jobMeta->state = SHM_BLOCK_STATUS_WRITING;
    jobMeta->jobId = job->jobId;
    strncpy(jobMeta->userName, job->userName, MAXLSFNAMELEN);
    jobMeta->userName[MAXLSFNAMELEN - 1] = '\0';
    jobMeta->jobNameOffset = jobNameOffset;
    jobMeta->jobNameLen = jobNameLen;

    if (job->qPtr)
        strncpy(jobMeta->queue, job->qPtr->queue, MAXLSFNAMELEN);
    else
        jobMeta->queue[0] = '\0';
    jobMeta->queue[MAXLSFNAMELEN - 1] = '\0';

    jobMeta->jStatus = job->jStatus;
    jobMeta->submitTime = job->shared ? job->shared->jobBill.submitTime : 0;
    jobMeta->xdrOffset = shm->xdrBuffer->tail;
    jobMeta->xdrLen = xdrLen;
    jobMeta->numReaders = 0;
    for (i = 0; i < MAX_QMBD_NUMS; i++) {
        if (shm->queryReaderInfo[i].valid)
            jobMeta->readerPids[jobMeta->numReaders++]
                = shm->queryReaderInfo[i].pid;
    }

    xdrOffset = shm->xdrBuffer->tail;
    if (xdrOffset + xdrLen > shm->xdrBuffer->capacity - 1) {
        wrapLen = shm->xdrBuffer->capacity - xdrOffset;
        memcpy(&shm->xdrBuffer->buff[xdrOffset], xdrBuf, wrapLen);
        memcpy(&shm->xdrBuffer->buff[0], xdrBuf + wrapLen, xdrLen - wrapLen);
    } else {
        memcpy(&shm->xdrBuffer->buff[xdrOffset], xdrBuf, xdrLen);
    }
    shm->xdrBuffer->tail = (xdrOffset + xdrLen) % shm->xdrBuffer->capacity;
    jobMeta->state = SHM_BLOCK_STATUS_USED;
    shm->jobMetaQueue->tail = (metaTail + 1) % shm->jobMetaQueue->capacity;
    FREEUP(xdrBuf);

    if (parentIndex >= 0)
        shm->jobMetaQueue->jobUnit[parentIndex].nextJgrpIndex
            = shm->jobMetaQueue->tail;

    return metaTail;
}

/*
 * Publish a committed job or job array through the SHM sync backend.
 * @param[in] subReq: Original submit request.
 * @param[in] job: Master mbd job object after commit.
 * @return: 0; SHM publication failure is best-effort.
 */
int
qmbdShmSyncSubmitJob(struct submitReq *subReq, struct jData *job)
{
    struct jData *elem;
    int parentIndex = -1;

    if (!qmbd_port || isQmbd || subReq == NULL || job == NULL || shm == NULL)
        return 0;

    if (job->nodeType == JGRP_NODE_ARRAY) {
        parentIndex = addJobToSyncShm(job, -1);
        for (elem = job->nextJob; elem != NULL; elem = elem->nextJob)
            addJobToSyncShm(elem, parentIndex);
        return 0;
    }

    addJobToSyncShm(job, -1);
    return 0;
}

/*
 * Match either live jData or SHM metadata against a jobInfoReq.
 * @param[in] jobData: jData or jobMetaData pointer.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[in] allqueues: TRUE if queue filtering is disabled.
 * @param[in] allusers: TRUE if user filtering is disabled.
 * @param[in] allhosts: TRUE if host filtering is disabled.
 * @param[in] uGrp: Optional user group filter.
 * @param[in] uPtr: Optional user object for exact user matching.
 * @param[in] searchJobName: TRUE if jobName is a prefix wildcard query.
 * @param[in] type: Type of jobData.
 * @return: TRUE if the job matches, otherwise FALSE.
 */
static int
checkQmbdShmJobMatch(void *jobData,
                     struct jobInfoReq *jobInfoReq,
                     char allqueues,
                     char allusers,
                     char allhosts,
                     struct gData *uGrp,
                     struct uData *uPtr,
                     char searchJobName,
                     enum jobSelectType type)
{
    int i;
    struct jData *jpbw = NULL;
    struct jobMetaData *metaData = NULL;
    int jStatus = 0;
    LS_LONG_INT jobId = 0;
    char fullName[MAXPATHLEN];

    if (type == JOB_TYPE_JDATA) {
        jpbw = (struct jData *)jobData;
        if (jpbw->jobId < 0)
            return FALSE;
    } else if (type == JOB_TYPE_METADATA) {
        metaData = (struct jobMetaData *)jobData;
        if (metaData->jobId < 0)
            return FALSE;
    } else {
        return FALSE;
    }

    if (!allqueues) {
        if (type == JOB_TYPE_JDATA) {
            if (strcmp(jpbw->qPtr->queue, jobInfoReq->queue) != 0)
                return FALSE;
        } else if (strcmp(metaData->queue, jobInfoReq->queue) != 0) {
            return FALSE;
        }
    }

    if (!allusers) {
        if (type == JOB_TYPE_JDATA && (jpbw->uPtr != uPtr)) {
            if (uGrp == NULL || !gMember(jpbw->userName, uGrp))
                return FALSE;
        } else if (type == JOB_TYPE_METADATA) {
            if (uPtr != NULL && strcmp(metaData->userName, uPtr->user) == 0) {
            } else if (uGrp == NULL || !gMember(metaData->userName, uGrp)) {
                return FALSE;
            }
        }
    }

    if (jobInfoReq->jobName[0] != '\0') {
        if (type == JOB_TYPE_JDATA) {
            fullJobName_r(jpbw, fullName);
        } else {
            getShmJobName(metaData, fullName);
        }

        if ((searchJobName == FALSE
             && strcmp(jobInfoReq->jobName, fullName) != 0)
            || (searchJobName == TRUE
                && strncmp(fullName, jobInfoReq->jobName,
                           strlen(jobInfoReq->jobName)) != 0))
            return FALSE;
    }

    if (jobInfoReq->jobId != 0) {
        jobId = (type == JOB_TYPE_JDATA) ? jpbw->jobId : metaData->jobId;
        if ((LSB_ARRAY_IDX(jobInfoReq->jobId) != 0
             && LSB_ARRAY_IDX(jobInfoReq->jobId) != LSB_ARRAY_IDX(jobId))
            || LSB_ARRAY_JOBID(jobInfoReq->jobId) != LSB_ARRAY_JOBID(jobId))
            return FALSE;
    }

    jStatus = (type == JOB_TYPE_JDATA) ? jpbw->jStatus : metaData->jStatus;
    if (!(jobInfoReq->options & (ALL_JOB|JOBID_ONLY_ALL))) {
        if (((jobInfoReq->options & CUR_JOB) || (jobInfoReq->options & LAST_JOB))
            && !(IS_FINISH(jStatus))) {
        } else if ((jobInfoReq->options & DONE_JOB) && IS_FINISH(jStatus)) {
        } else if ((jobInfoReq->options & PEND_JOB) && IS_PEND(jStatus)) {
        } else if ((jobInfoReq->options & SUSP_JOB) && IS_SUSP(jStatus)
                   && !(jStatus & JOB_STAT_UNKWN)) {
        } else if ((jobInfoReq->options & RUN_JOB) && (jStatus & JOB_STAT_RUN)) {
        } else if ((jobInfoReq->options & ZOMBIE_JOB)
                   && (jStatus & JOB_STAT_ZOMBIE)) {
        } else {
            return FALSE;
        }
    }

    if (!allhosts) {
        if (type == JOB_TYPE_METADATA)
            return FALSE;

        if (IS_PEND(jpbw->jStatus))
            return FALSE;
        if (jpbw->hPtr == NULL)
            return FALSE;
        for (i = 0; i < jpbw->numHostPtr; i++) {
            if (jpbw->hPtr[i] == NULL)
                continue;
            if (equalHost_(jobInfoReq->host, jpbw->hPtr[i]->host))
                break;
        }
        if (i >= jpbw->numHostPtr)
            return FALSE;
    }

    return TRUE;
}

/*
 * Select SHM metadata entries that match a normal bjobs query.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[out] jobMetaDataList: Borrowed metadata pointer array owned by caller.
 * @param[out] listSize: Number of selected metadata entries.
 * @return: LSBE_NO_ERROR if entries are selected, otherwise an LSBE_* code.
 */
int
selectQmbdShmSyncJobs(struct jobInfoReq *jobInfoReq,
                      struct jobMetaData ***jobMetaDataList,
                      int *listSize)
{
    char allqueues = FALSE;
    char allusers = FALSE;
    char allhosts = FALSE;
    char searchJobName = FALSE;
    struct jobMetaData **joblist = NULL;
    struct gData *uGrp = NULL;
    int numJobs = 0;
    int arraysize = 0;
    int ret = 0;
    struct uData *uPtr = NULL;
    int currentIndex = 0, startIndex;
    int i = 0;
    static int currentReaderIndex = -1;

    if (shm == NULL)
        return LSBE_NO_JOB;
    if (currentReaderIndex == -1) {
        pid_t pid = getpid();
        for (i = 0; i < MAX_QMBD_NUMS; i++) {
            if (pid == shm->queryReaderInfo[i].pid)
                currentReaderIndex = i;
        }
    }

    if (currentReaderIndex >= 0)
        startIndex = shm->queryReaderInfo[currentReaderIndex].readStartIndex;
    else
        return LSBE_NO_JOB;

    if (jobInfoReq->queue[0] == '\0')
        allqueues = TRUE;
    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        allusers = TRUE;
    else
        uGrp = getUGrpData(jobInfoReq->userName);

    if (jobInfoReq->host[0] == '\0')
        allhosts = TRUE;
    if (jobInfoReq->jobName[0] != '\0'
        && jobInfoReq->jobName[strlen(jobInfoReq->jobName) - 1] == '*'){
        searchJobName = TRUE;
        jobInfoReq->jobName[strlen(jobInfoReq->jobName) - 1] = '\0';
    }

    if (!allusers)
        uPtr = getUserData(jobInfoReq->userName);

    for (i = 0; i < syncShmJobCapacity - 1; i++) {
        struct jobMetaData *metaData;

        currentIndex = (startIndex + i) % syncShmJobCapacity;
        if (currentIndex == shm->jobMetaQueue->tail)
            break;
        metaData = &shm->jobMetaQueue->jobUnit[currentIndex];
        if (metaData == NULL || metaData->state != SHM_BLOCK_STATUS_USED)
            continue;
        if (metaData->type == JGRP_NODE_ARRAY)
            continue;

        if (checkQmbdShmJobMatch((void *)metaData, jobInfoReq, allqueues,
                                 allusers, allhosts, uGrp, uPtr,
                                 searchJobName, JOB_TYPE_METADATA)) {
            ret = ensureJobListCapacity((void ***)&joblist, numJobs,
                                        &arraysize, JOB_TYPE_METADATA);
            if (ret != LSBE_NO_ERROR) {
                FREEUP(joblist);
                return ret;
            }
            if (jobInfoReq->options & LAST_JOB) {
                /*
                 * The SHM ring is scanned from this qmbd reader's snapshot
                 * boundary to tail, so the last matching metadata entry is
                 * the newest post-snapshot job visible to this qmbd.
                 */
                joblist[0] = metaData;
                numJobs = 1;
            } else {
                joblist[numJobs++] = metaData;
            }
        }
    }

    *listSize = numJobs;
    if (numJobs > 0) {
        *jobMetaDataList = joblist;
        return LSBE_NO_ERROR;
    } else if (!allqueues && getQueueData(jobInfoReq->queue) == NULL) {
        FREEUP(joblist);
        return LSBE_BAD_QUEUE;
    }
    FREEUP(joblist);
    return LSBE_NO_JOB;
}

/*
 * Match one SHM metadata entry for job-array or job-group style queries.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[in] jobMeta: SHM metadata entry to match.
 * @param[in] allqueues: TRUE if queue filtering is disabled.
 * @param[in] allusers: TRUE if user filtering is disabled.
 * @param[in] allhosts: TRUE if host filtering is disabled.
 * @param[in] uGrp: Optional user group filter.
 * @param[in] idxList: Optional parsed array index filter.
 * @return: TRUE if the metadata entry matches, otherwise FALSE.
 */
static int
checkQmbdShmJgrpMatch(struct jobInfoReq *jobInfoReq,
                      struct jobMetaData *jobMeta,
                      char allqueues,
                      char allusers,
                      char allhosts,
                      struct gData *uGrp,
                      struct idxList *idxList)
{
    struct idxList *idx;

    if (jobInfoReq->jobId != 0 && (jobInfoReq->options & JGRP_ARRAY_INFO)) {
        if ((LSB_ARRAY_IDX(jobInfoReq->jobId) != 0
             && LSB_ARRAY_IDX(jobInfoReq->jobId) != LSB_ARRAY_IDX(jobMeta->jobId))
            || LSB_ARRAY_JOBID(jobInfoReq->jobId) != LSB_ARRAY_JOBID(jobMeta->jobId))
            return FALSE;
    }

    if (jobInfoReq->options & (ALL_JOB|JOBID_ONLY_ALL|JOBID_ONLY)) {
    } else if ((jobInfoReq->options & (CUR_JOB | LAST_JOB))
               && (IS_START(jobMeta->jStatus) || IS_PEND(jobMeta->jStatus))) {
    } else if ((jobInfoReq->options & PEND_JOB) && IS_PEND(jobMeta->jStatus)) {
    } else if ((jobInfoReq->options & (SUSP_JOB | RUN_JOB))
               && IS_START(jobMeta->jStatus)) {
    } else if ((jobInfoReq->options & DONE_JOB) && IS_FINISH(jobMeta->jStatus)) {
    } else {
        return FALSE;
    }

    if (!allqueues && strcmp(jobMeta->queue, jobInfoReq->queue) != 0)
        return FALSE;

    if (!allusers && strcmp(jobMeta->userName, jobInfoReq->userName)) {
        if (uGrp == NULL || !gMember(jobMeta->userName, uGrp))
            return FALSE;
    }

    if (!allhosts)
        return FALSE;

    if (idxList == NULL)
        return TRUE;

    for (idx = idxList; idx; idx = idx->next) {
        if (LSB_ARRAY_IDX(jobMeta->jobId) < idx->start
            || LSB_ARRAY_IDX(jobMeta->jobId) > idx->end)
            continue;
        if (((LSB_ARRAY_IDX(jobMeta->jobId) - idx->start) % idx->step) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Select SHM metadata entries for bjobs -A and array-name queries.
 * @param[in] jobInfoReq: Query filter from the client request.
 * @param[out] jobMetaDataList: Borrowed metadata pointer array owned by caller.
 * @param[out] listSize: Number of selected metadata entries.
 * @return: LSBE_NO_ERROR if entries are selected, otherwise an LSBE_* code.
 */
int
selectQmbdShmSyncJgrps(struct jobInfoReq *jobInfoReq,
                       struct jobMetaData ***jobMetaDataList,
                       int *listSize)
{
    char allqueues = FALSE, allusers = FALSE, allhosts = FALSE;
    int arraysize = 0, tmpIndex = 0, retError = LSBE_NO_ERROR;
    int currentIndex = 0, numJobs = 0, maxJLimit = 0, i;
    struct jobMetaData *job;
    struct jobMetaData **joblist = NULL;
    struct gData *uGrp = NULL;
    struct idxList *idxList = NULL;
    char jobName[MAX_CMD_DESC_LEN];
    struct jobMetaData *meta;
    char shmJobName[MAXPATHLEN];
    static int currentReaderIndex = -1;

    if (shm == NULL)
        return LSBE_NO_JOB;

    if (currentReaderIndex == -1) {
        pid_t pid = getpid();
        for (i = 0; i < MAX_QMBD_NUMS; i++) {
            if (pid == shm->queryReaderInfo[i].pid)
                currentReaderIndex = i;
        }
    }

    if (currentReaderIndex >= 0)
        currentIndex = shm->queryReaderInfo[currentReaderIndex].readStartIndex;
    else
        return LSBE_NO_JOB;

    memset(jobName, 0, sizeof(jobName));
    if (jobInfoReq->queue[0] == '\0')
        allqueues = TRUE;
    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        allusers = TRUE;
    else
        uGrp = getUGrpData(jobInfoReq->userName);
    if (jobInfoReq->host[0] == '\0')
        allhosts = TRUE;
    if (((strlen(jobInfoReq->jobName) == 1) && (jobInfoReq->jobName[0] == '/'))
        || (strlen(jobInfoReq->jobName) == 0))
        strcpy(jobName, "*");
    else
        ls_strcat(jobName, sizeof(jobName), jobInfoReq->jobName);

    idxList = parseJobArrayIndex(jobName, &retError, &maxJLimit);
    if (idxList == NULL && retError != LSBE_NO_ERROR)
        return retError;

    while (currentIndex != shm->jobMetaQueue->tail) {
        job = &shm->jobMetaQueue->jobUnit[currentIndex];
        if (job == NULL || job->state != SHM_BLOCK_STATUS_USED) {
            currentIndex = (currentIndex + 1) % syncShmJobCapacity;
            continue;
        }
        getShmJobName(job, shmJobName);
        if (matchName(jobName, shmJobName)) {
            if (jobInfoReq->options & JGRP_ARRAY_INFO) {
                if (job->type == JGRP_NODE_ARRAY
                    && checkQmbdShmJgrpMatch(jobInfoReq, job, allqueues,
                                             allusers, allhosts, uGrp, idxList)) {
                    retError = ensureJobListCapacity((void ***)&joblist, numJobs,
                                                     &arraysize, JOB_TYPE_METADATA);
                    if (retError != LSBE_NO_ERROR) {
                        FREEUP(joblist);
                        freeIdxList(idxList);
                        return retError;
                    }
                    joblist[numJobs++] = job;
                }
            } else if (job->type == JGRP_NODE_ARRAY) {
                tmpIndex = (currentIndex + 1) % syncShmJobCapacity;
                while (tmpIndex != job->nextJgrpIndex) {
                    meta = &shm->jobMetaQueue->jobUnit[tmpIndex];
                    tmpIndex = (tmpIndex + 1) % syncShmJobCapacity;
                    if (meta == NULL || meta->state != SHM_BLOCK_STATUS_USED)
                        continue;
                    if (checkQmbdShmJgrpMatch(jobInfoReq, meta, allqueues,
                                              allusers, allhosts, uGrp, idxList)) {
                        retError = ensureJobListCapacity((void ***)&joblist,
                                                         numJobs, &arraysize,
                                                         JOB_TYPE_METADATA);
                        if (retError != LSBE_NO_ERROR) {
                            FREEUP(joblist);
                            freeIdxList(idxList);
                            return retError;
                        }
                        joblist[numJobs++] = meta;
                    }
                }
            } else if (checkQmbdShmJgrpMatch(jobInfoReq, job, allqueues,
                                             allusers, allhosts, uGrp, idxList)) {
                retError = ensureJobListCapacity((void ***)&joblist, numJobs,
                                                 &arraysize, JOB_TYPE_METADATA);
                if (retError != LSBE_NO_ERROR) {
                    FREEUP(joblist);
                    freeIdxList(idxList);
                    return retError;
                }
                joblist[numJobs++] = job;
            }
        }
        currentIndex = job->nextJgrpIndex;
    }

    freeIdxList(idxList);
    if (numJobs > 0) {
        *jobMetaDataList = joblist;
        *listSize = numJobs;
        return LSBE_NO_ERROR;
    }
    FREEUP(joblist);
    return LSBE_NO_JOB;
}

/*
 * Write the serialized SHM XDR payload for one selected metadata entry.
 * @param[in] chfd: Client channel descriptor.
 * @param[in] jobMeta: Selected SHM metadata entry.
 * @return: 0 on success, -1 on write failure.
 */
int
chanWriteQmbdShmJobXdr(int chfd, struct jobMetaData *jobMeta)
{
    int res, len;

    if (shm == NULL || jobMeta == NULL)
        return -1;

    len = jobMeta->xdrLen;
    res = jobMeta->xdrOffset + jobMeta->xdrLen - shm->xdrBuffer->capacity;
    if (res > 0)
        len -= res;

    if (chanWriteNonBlock_(chfd, &shm->xdrBuffer->buff[0] + jobMeta->xdrOffset,
                           len, DEF_WRITE_TIMEOUT) != len)
        return -1;
    if (res > 0
        && chanWriteNonBlock_(chfd, (&shm->xdrBuffer->buff[0]), res,
                              DEF_WRITE_TIMEOUT) != res)
        return -1;

    return 0;
}

/*
 * Drop SHM entries that no active qmbd can still read.
 * @return: none.
 */
static void
pruneOldJobs(void)
{
    time_t minForkTime = 0;
    int hasValidQmbd = 0;
    int i;
    time_t earliestTime = 0;
    time_t latestExitTime = 0;

    if (shm == NULL)
        return;

    for (i = 0; i < MAX_QMBD_NUMS; i++) {
        if (shm->queryReaderInfo[i].valid) {
            if (kill(shm->queryReaderInfo[i].pid, 0) == -1) {
                shm->queryReaderInfo[i].valid = 0;
                if (shm->queryReaderInfo[i].forkTime > latestExitTime)
                    latestExitTime = shm->queryReaderInfo[i].forkTime;
                shm->queryReaderInfo[i].forkTime = 0;
            } else {
                hasValidQmbd = 1;
                if (earliestTime == 0
                    || shm->queryReaderInfo[i].forkTime < earliestTime) {
                    earliestTime = shm->queryReaderInfo[i].forkTime;
                }
            }
        }
    }
    if (hasValidQmbd)
        minForkTime = earliestTime;

    while (shm->jobMetaQueue->head != shm->jobMetaQueue->tail) {
        struct jobMetaData *jobMeta
            = &shm->jobMetaQueue->jobUnit[shm->jobMetaQueue->head];
        if ((hasValidQmbd && jobMeta->submitTime < minForkTime)
            || (!hasValidQmbd && jobMeta->submitTime < latestExitTime)
            || isJobStale(jobMeta)) {
            removeHead();
        } else {
            break;
        }
    }
}

/*
 * Periodically prune stale SHM entries in the main mbd.
 * @param[in] arg: Unused thread argument.
 * @return: NULL.
 */
static void *
shmCleaner(void *arg)
{
    struct timespec req, rem;

    while (TRUE) {
        req.tv_sec = 1;
        req.tv_nsec = 0;
        while (nanosleep(&req, &rem) == -1 && errno == EINTR)
            req = rem;
        pruneOldJobs();
    }

    return NULL;
}

/*
 * Start the detached SHM cleaner thread.
 * @return: 0 on success, -1 on pthread_create failure.
 */
static int
createShmCleanerThread(void)
{
    pthread_t tid;
    int rc;

    rc = pthread_create(&tid, NULL, shmCleaner, NULL);
    if (rc != 0)
        return -1;
    pthread_detach(tid);
    return 0;
}

/*
 * Return true when no registered qmbd reader still needs this metadata entry.
 * @param[in] job: SHM metadata entry to test.
 * @return: 1 if stale, 0 if any active qmbd still needs it.
 */
static int
isJobStale(struct jobMetaData *job)
{
    int i, j;

    for (i = 0; i < job->numReaders; i++) {
        for (j = 0; j < MAX_QMBD_NUMS; j++) {
            if (shm->queryReaderInfo[j].valid
                && job->readerPids[i] == shm->queryReaderInfo[j].pid)
                return 0;
        }
    }
    return 1;
}

/*
 * Return the number of entries currently stored in the metadata ring.
 * @param[in] queue: Metadata circular queue.
 * @return: Number of occupied entries.
 */
static int
getJobQueueCount(struct jobMetaQueue *queue)
{
    return (queue->tail - queue->head + queue->capacity) % queue->capacity;
}

/*
 * Return the number of bytes currently stored in a circular byte buffer.
 * @param[in] queue: Circular byte buffer.
 * @return: Number of occupied bytes.
 */
static long long
getBufferUsedBytes(struct bufferQueue *queue)
{
    return (queue->tail - queue->head + queue->capacity) % queue->capacity;
}

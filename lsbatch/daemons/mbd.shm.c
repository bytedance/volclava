/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mbd.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <time.h>

static int isJobStale(struct jobMetaData *job);
static int getJobQueueCount(struct jobMetaQueue *queue);
static long long getBufferUsedBytes(struct bufferQueue *queue);

/*
 * Creates, attaches to, and initializes the shared memory region.
 * @return: Pointer to the shared memory structure, or NULL on failure.
 */
struct syncJobShm *
initSyncShm()
{
    struct syncJobShm *shm = NULL;
    void *shmBase = NULL;
    int id = 0;
    long long totSize = 0;
    key_t key = IPC_PRIVATE;
    int i = 0;

    /* We are using IPC_PRIVATE, so this SHM is only accessible by the creating process and its children */
    totSize = sizeof(struct jobMetaQueue) + syncShmJobCapacity * sizeof(struct jobMetaData) + sizeof(struct bufferQueue) + syncShmXdrBufferSize + sizeof(struct bufferQueue) + syncShmJobNameBufferSize + sizeof(struct syncJobShm);
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
    /* 
     * Mark the segment to be destroyed. The segment will actually be destroyed
     * only after the last process detaches it
     */
    if (shmctl(id, IPC_RMID, NULL) == -1) {
        ls_syslog(LOG_WARNING, "%s: shmctl IPC_RMID failed: %s", __func__, strerror(errno));
    }

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Created and initialized SHM id %d, size %lu", __func__, id, sizeof(struct syncJobShm));
    
    memset(shmBase, 0, totSize);

    shm = (struct syncJobShm *)shmBase;
    shm->jobMetaQueue = (struct jobMetaQueue *)(shmBase + sizeof(struct syncJobShm));
    shm->jobMetaQueue->capacity = syncShmJobCapacity;
    shm->xdrBuffer = (struct bufferQueue *)(shmBase + sizeof(struct syncJobShm) + sizeof(struct jobMetaQueue) + syncShmJobCapacity * sizeof(struct jobMetaData));
    shm->xdrBuffer->capacity = syncShmXdrBufferSize;
    shm->jobNameBuffer = (struct bufferQueue *)(shmBase + sizeof(struct syncJobShm) + sizeof(struct jobMetaQueue) + syncShmJobCapacity * sizeof(struct jobMetaData) + sizeof(struct bufferQueue) + syncShmXdrBufferSize);
    shm->jobNameBuffer->capacity = syncShmJobNameBufferSize;
    shm->shmId = id;

    return shm;
}

/* 
 * Removes the oldest job from the shared memory queues (both metadata and XDR buffer).
 * Used when the buffer is full or for garbage collection.
 * @return: void
 */
void
removeHead(void)
{
    struct jobMetaData *jobMeta = NULL;
    struct jobMetaData *nextJobMeta = NULL;
    int nextMetaHead;
    
    if (shm->jobMetaQueue->head == shm->jobMetaQueue->tail) return;
    
    jobMeta = &shm->jobMetaQueue->jobUnit[shm->jobMetaQueue->head];
    jobMeta->state = SHM_BLOCK_STATUS_UNUSED;
    
    shm->xdrBuffer->head = (shm->xdrBuffer->head + jobMeta->xdrLen) % shm->xdrBuffer->capacity;

    /* If this is the last job referring to the jobName at the head of jobNameBuffer, move head */
    nextMetaHead = (shm->jobMetaQueue->head + 1) % shm->jobMetaQueue->capacity;
    if (nextMetaHead != shm->jobMetaQueue->tail) {
        nextJobMeta = &shm->jobMetaQueue->jobUnit[nextMetaHead];
        if (nextJobMeta->jobNameOffset != jobMeta->jobNameOffset) {
            shm->jobNameBuffer->head = (shm->jobNameBuffer->head + jobMeta->jobNameLen) % shm->jobNameBuffer->capacity;
        }
    } else {
        /* Last job in the queue, definitely move the jobNameBuffer head */
        shm->jobNameBuffer->head = (shm->jobNameBuffer->head + jobMeta->jobNameLen) % shm->jobNameBuffer->capacity;
    }

    shm->jobMetaQueue->head = (shm->jobMetaQueue->head + 1) % shm->jobMetaQueue->capacity;
    if(logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"%s: an unused job in shared memory has been deleted. Remaining job capacity: %d, xdr buffer remaining: %lld bytes, jobname buffer remaining: %lld bytes", __func__, shm->jobMetaQueue->capacity - 1 - getJobQueueCount(shm->jobMetaQueue),shm->xdrBuffer->capacity -1 - getBufferUsedBytes(shm->xdrBuffer), shm->jobNameBuffer->capacity -1 - getBufferUsedBytes(shm->jobNameBuffer));
}

/* 
 * Appends a new job to the shared memory.
 * Serializes the job data to XDR format and writes metadata for filtering.
 * @param[in] shm: Pointer to the shared memory structure.
 * @param[in] job: Pointer to the job data (jData) to be appended.
 * @return: index of job
 */
int
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

    if (!shm) {
        return -1;
    }
    if(getShmAttachCount(shm->shmId) <= 1){
        return -1;
    }

    /* Serialize job data to XDR format */
    xdrLen = packJobInfo(job, 0, &xdrBuf, 0, 0, 0); 
    
    if (xdrLen < 0 || xdrBuf == NULL) {
        ls_syslog(LOG_ERR, "%s: packJobInfo failed for job %lld", __func__, job->jobId);
        return -1;
    }
    
    usedSpace = getJobQueueCount(shm->jobMetaQueue);
    if (usedSpace + 1 > shm->jobMetaQueue->capacity - 1) {
            ls_syslog(LOG_ERR, "%s: jobMetaQueue is full", __func__);
        return -1;
    }
    
    usedSpace = getBufferUsedBytes(shm->xdrBuffer);
    if (usedSpace + xdrLen > shm->xdrBuffer->capacity - 1) {
            ls_syslog(LOG_ERR, "%s: xdrBuffer is full, need is %d bytes, left is %d bytes", __func__, xdrLen,shm->xdrBuffer->capacity - 1 - usedSpace);
        return -1;
    }

    /* Handle jobName storage */
    fullJobName_r(job, jobName);
    jobNameLen = strlen(jobName) + 1;

    /* Check if we can reuse the last job's jobName (for job arrays) */
    if (job->jgrpNode && job->jgrpNode == lastJgrpNode && lastJobNameOffset != -1 && 
        lastMetaTail == ((shm->jobMetaQueue->tail - 1 + shm->jobMetaQueue->capacity) % shm->jobMetaQueue->capacity) &&
        shm->jobMetaQueue->jobUnit[lastMetaTail].state == SHM_BLOCK_STATUS_USED) {
        jobNameOffset = lastJobNameOffset;
        jobNameLen = lastJobNameLen;
    } else {
        /* Need to write new jobName to jobNameBuffer */
        usedSpace = getBufferUsedBytes(shm->jobNameBuffer);
        if (usedSpace + jobNameLen > shm->jobNameBuffer->capacity - 1) {
            ls_syslog(LOG_ERR, "%s: jobNameBuffer is full, need is %d bytes, left is %d bytes", __func__, jobNameLen, shm->jobNameBuffer->capacity - 1 - usedSpace);
            FREEUP(xdrBuf);
            return -1;
        }
        jobNameOffset = shm->jobNameBuffer->tail;
        if (jobNameOffset + jobNameLen > shm->jobNameBuffer->capacity - 1) {
            wrapLen = shm->jobNameBuffer->capacity - jobNameOffset;
            memcpy(&shm->jobNameBuffer->buff[jobNameOffset], jobName, wrapLen);
            memcpy(&shm->jobNameBuffer->buff[0], jobName + wrapLen, jobNameLen - wrapLen);
        } else {
            memcpy(&shm->jobNameBuffer->buff[jobNameOffset], jobName, jobNameLen);
        }
        shm->jobNameBuffer->tail = (jobNameOffset + jobNameLen) % shm->jobNameBuffer->capacity;
        
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
    jobMeta->state = SHM_BLOCK_STATUS_WRITING;
    jobMeta->jobId = job->jobId;
    strncpy(jobMeta->userName, job->userName, MAXLSFNAMELEN);
    jobMeta->userName[MAXLSFNAMELEN-1] = '\0';
    
    jobMeta->jobNameOffset = jobNameOffset;
    jobMeta->jobNameLen = jobNameLen;
    
    if (job->qPtr) {
        strncpy(jobMeta->queue, job->qPtr->queue, MAXLSFNAMELEN);
    } else {
        jobMeta->queue[0] = '\0';
    }
    jobMeta->queue[MAXLSFNAMELEN-1] = '\0';
    
    jobMeta->jStatus = job->jStatus;
    
    if (job->shared) {
        jobMeta->submitTime = job->shared->jobBill.submitTime;
    } else {
        jobMeta->submitTime = 0;
    }
    jobMeta->xdrOffset = shm->xdrBuffer->tail;
    jobMeta->xdrLen = xdrLen;
    jobMeta->numReaders = 0;
    for(i = 0; i < MAX_QMBD_NUMS; i++){
        if(shm->queryReaderInfo[i].valid){
            jobMeta->readerPids[jobMeta->numReaders++] = shm->queryReaderInfo[i].pid;
        }
    }
    /* Write XDR data to circular buffer */
    xdrOffset = shm->xdrBuffer->tail;
    if (xdrOffset + xdrLen > shm->xdrBuffer->capacity - 1) {
        wrapLen = shm->xdrBuffer->capacity - xdrOffset;
        memcpy(&shm->xdrBuffer->buff[xdrOffset], xdrBuf, wrapLen);
        memcpy(&shm->xdrBuffer->buff[0], xdrBuf + wrapLen, xdrLen - wrapLen);
    } else {
        memcpy(&shm->xdrBuffer->buff[xdrOffset], xdrBuf, xdrLen);
    }
    shm->xdrBuffer->tail = (xdrOffset + xdrLen) % shm->xdrBuffer->capacity;
    /* Mark as USED to indicate data is ready for reading */
    jobMeta->state = SHM_BLOCK_STATUS_USED;
    shm->jobMetaQueue->tail = (metaTail + 1) % shm->jobMetaQueue->capacity;
    FREEUP(xdrBuf);
    if(parentIndex >= 0){
        shm->jobMetaQueue->jobUnit[parentIndex].nextJgrpIndex = shm->jobMetaQueue->tail;
    }
    if(logclass * LC_TRACE)
        ls_syslog(LOG_DEBUG,"%s: a new job has been added to shared memory. Remaining job capacity: %d, xdr buffer remaining: %lld bytes, jobname buffer remaining: %lld bytes", __func__, shm->jobMetaQueue->capacity - 1 - getJobQueueCount(shm->jobMetaQueue),shm->xdrBuffer->capacity -1 - getBufferUsedBytes(shm->xdrBuffer), shm->jobNameBuffer->capacity -1 - getBufferUsedBytes(shm->jobNameBuffer));
    return metaTail;
}

/* 
 * Cleans up old job data from the shared memory.
 * Removes jobs that are older than the oldest active query mbd process.
 * @return: void
 */
void
pruneOldJobs(void)
{
    if (!shm) return;
    
    time_t minForkTime = 0;
    int hasValidQmbd = 0;
    int i;
    int earliestQmbdIdx = -1;
    time_t earliestTime = 0;
    time_t latestExitTime = 0;
    for (i = 0; i < MAX_QMBD_NUMS; i++) {
        if (shm->queryReaderInfo[i].valid) {
            if (kill(shm->queryReaderInfo[i].pid, 0) == -1) {
                shm->queryReaderInfo[i].valid = 0;
                if(shm->queryReaderInfo[i].forkTime > latestExitTime){
                    latestExitTime = shm->queryReaderInfo[i].forkTime;
                }
                shm->queryReaderInfo[i].forkTime = 0;
            } else {
                hasValidQmbd = 1;
                if (earliestQmbdIdx == -1 || shm->queryReaderInfo[i].forkTime < earliestTime) {
                    earliestTime = shm->queryReaderInfo[i].forkTime;
                    earliestQmbdIdx = i;
                }
            }
        }
    }
    if (hasValidQmbd) {
        minForkTime = earliestTime;
    }

    /* Remove idle jobs from the head of the queue */
    while (shm->jobMetaQueue->head != shm->jobMetaQueue->tail) {
        struct jobMetaData *jobMeta = &shm->jobMetaQueue->jobUnit[shm->jobMetaQueue->head];
        /* 
         * Three conditions to prune jobs:
         * 1. If there are valid qmbd processes: remove jobs submitted before the earliest qmbd was forked
         * 2. If all qmbd processes exited this round: remove jobs submitted before the latest fork time
         * 3. Fallback: remove jobs that are not needed by any query mbd 
         */
        if ((hasValidQmbd && jobMeta->submitTime < minForkTime) || (!hasValidQmbd && jobMeta->submitTime < latestExitTime) || isJobStale(jobMeta)) {
            removeHead();
        } else {
            break;
        }
    }
}

/* 
 * Registers a new query mbd process in the shared memory.
 * Finds an empty slot in queryReaderInfo array or reuses an invalid one.
 * @param[in] shm: Pointer to the shared memory structure.
 * @param[in] pid: PID of the new query mbd process.
 * @return: 0 on success, -1 if no slot is available.
 */
int
registerReaderToShm(pid_t pid, time_t forkTime)
{
    int i;
    int slot = -1;

    if (!shm) {
        ls_syslog(LOG_ERR, "%s: shared memory is NULL", __func__);
        return -1;
    }

    for (i = 0; i < MAX_QMBD_NUMS; i++) {
        if (!shm->queryReaderInfo[i].valid) {
            slot = i;
            break;
        }
    }
    

    if (slot == -1) {
        ls_syslog(LOG_ERR, "%s: No available slot for new qmbd (pid: %d)", __func__, pid);
        return -1;
    }
    shm->queryReaderInfo[slot].pid = pid;
    shm->queryReaderInfo[slot].forkTime = forkTime;
    shm->queryReaderInfo[slot].readStartIndex = shm->jobMetaQueue->tail; 
    shm->queryReaderInfo[slot].valid = 1;
    return 0;
}


/*
 * Get the number of processes attached to a shared memory segment
 * @param[in] shmid: Shared memory segment ID
 * @return: Number of attached processes on success, -1 on failure
 */
int getShmAttachCount(int shmid){
    struct shmid_ds shm_info;
    
    if (shmctl(shmid, IPC_STAT, &shm_info) == -1) {
        ls_syslog(LOG_ERR, "%s: shmctl failed: %m", __func__);
        return -1;
    }
    
    return shm_info.shm_nattch;
}

/*
 * Extracts the job name from the circular jobNameBuffer.
 * Handles cases where the name is split across the end and beginning of the buffer.
 * @param[in] shm: Pointer to the shared memory structure.
 * @param[in] meta: Pointer to the job metadata containing offset and length.
 * @param[out] dest: Destination buffer to copy the job name into.
 */
void
getShmJobName(struct jobMetaData *meta, char *dest) 
{
    int wrapLen;

    if (!shm || !meta || !dest) return;
    
    if (meta->jobNameOffset + meta->jobNameLen > shm->jobNameBuffer->capacity) {
        wrapLen = shm->jobNameBuffer->capacity - meta->jobNameOffset;
        memcpy(dest, &shm->jobNameBuffer->buff[meta->jobNameOffset], wrapLen);
        memcpy(dest + wrapLen, &shm->jobNameBuffer->buff[0], meta->jobNameLen - wrapLen);
    } else {
        memcpy(dest, &shm->jobNameBuffer->buff[meta->jobNameOffset], meta->jobNameLen);
    }
}

/*
 * Check if a job is stale (no longer needed by any query mbd process)
 * @param[in] job: Pointer to the job metadata
 * @param[in] shm: Pointer to the shared memory structure
 * @return: 1 if job is stale, 0 otherwise
 */
static int isJobStale(struct jobMetaData *job){
    int i, j;
    for(i = 0; i < job->numReaders; i++){
        for(j = 0; j < MAX_QMBD_NUMS; j++){
            if(shm->queryReaderInfo[j].valid && job->readerPids[i] == shm->queryReaderInfo[j].pid){
                return 0;
            }
        }
    }
    return 1;
}

/*
 * Get the number of jobs in the job metadata queue
 * @param[in] queue: Pointer to the job metadata queue
 * @return: Number of jobs in the queue
 */
static int getJobQueueCount(struct jobMetaQueue *queue){
    return (queue->tail - queue->head + queue->capacity) % queue->capacity;
}

/*
 * Get the number of bytes used in the XDR buffer queue
 * @param[in] queue: Pointer to the XDR buffer queue
 * @return: Number of bytes used in the queue
 */
static long long getBufferUsedBytes(struct bufferQueue *queue){
    return (queue->tail - queue->head + queue->capacity) % queue->capacity;
}
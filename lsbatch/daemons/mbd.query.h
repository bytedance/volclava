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

#ifndef MBD_QUERY_H
#define MBD_QUERY_H

#include "mbd.h"

/* qmbd common definitions. */
#define DEF_QMBD_ALIVE_TIME        5
#define MIN_QMBD_ALIVE_TIME        5
#define MAX_QMBD_ALIVE_TIME        300
#define DEF_QMBD_THREAD_NUM        0
#define MIN_QMBD_THREAD_NUM        1
#define MAX_QMBD_THREAD_NUM        256
#define DEF_QMBD_MAX_TASK_NUM      2000
#define MIN_QMBD_MAX_TASK_NUM      500
#define MAX_QMBD_MAX_TASK_NUM      5000
#define DEF_WRITE_TIMEOUT          5

/* qmbd common lifecycle and query daemon state. */
extern int            startQueryDaemon(int *);
extern int            qmbdListenSock;           /*Passive listen socket reserved by the main mbd for qmbd*/
extern int            qmbdAliveTime;            /*Alive time (in seconds) for the qmbd subprocess*/
extern int            qmbdThreadNum;            /*Maximum number of threads in qmbd's thread pool*/
extern int            qmbdMaxTaskNum;           /*Capacity of qmbd's thread pool task queue*/
extern short          qmbd_port;                /*Port of the query mbd process*/
extern int            isQmbd;                   /*Flag indicating if the current process is qmbd (1 for qmbd process, 0 otherwise)*/
extern int            getValidatedNumericParam(const char *, char *, char *, int, int, int);

/* qmbd common control channel. */
extern int            qmbdCtrlSockPair[2];      /*Socketpair fds used for qmbd control events*/
extern int            qmbdCtrlChfd;             /*Current process channel descriptor for qmbd control events*/
extern int            handleQmbdCtrlEventIO(void);
extern int            sendQmbdCtrlReq(struct qmbdCtrlReq *);

/* qmbd job synchronization common. */
enum qmbdJobSyncMode {
    QMBD_JOB_SYNC_OFF = 0,        /* Disable incremental qmbd job sync. */
    QMBD_JOB_SYNC_SOCKET = 1,     /* Replay submits through internal socketpair. */
    QMBD_JOB_SYNC_SHM = 2         /* Read incremental submits from shared memory. */
};

extern int            qmbdJobSyncMode;          /*Incremental qmbd job sync mode*/
extern int            initQmbdJobSync(void);
extern int            qmbdSyncSubmitJob(struct submitReq *, struct jData *);

/*--------------- shared-memory job synchronization start ------------------*/
/* qmbd shared-memory sync definitions. */
#define DEF_QMBD_SYNC_SHM_SIZE_MB  1024
#define MIN_QMBD_SYNC_SHM_SIZE_MB  512
#define MAX_QMBD_SYNC_SHM_SIZE_MB  2048
#define MAX_QMBD_NUMS             12

enum jobSelectType {
    JOB_TYPE_JDATA,               /* Query candidate is a full jData object. */
    JOB_TYPE_METADATA             /* Query candidate is SHM job metadata. */
};

enum shmJobStatus {
    SHM_BLOCK_STATUS_UNUSED,      /* Metadata slot is free. */
    SHM_BLOCK_STATUS_WRITING,     /* Main mbd is filling this slot. */
    SHM_BLOCK_STATUS_USED         /* Slot is visible to qmbd readers. */
};

struct jobMetaData {
    LS_LONG_INT jobId;            /* Master mbd jobId for this metadata entry. */
    char userName[MAX_LSB_NAME_LEN];  /* Submit user used for query filtering. */
    char queue[MAX_LSB_NAME_LEN];     /* Queue name used for query filtering. */
    int jStatus;                  /* Job status snapshot. */
    time_t submitTime;            /* Submit time copied from main mbd. */
    int numReaders;               /* Number of qmbd readers tracking this slot. */
    int readerPids[MAX_QMBD_NUMS]; /* qmbd pids that may still read this slot. */
    long long jobNameOffset;      /* Offset into jobNameBuffer. */
    int jobNameLen;               /* Length stored in jobNameBuffer. */
    long long xdrOffset;          /* Offset into xdrBuffer. */
    int xdrLen;                   /* Encoded job info length in xdrBuffer. */
    int state;                    /* One of enum shmJobStatus. */
    int type;                     /* One of enum jobSelectType. */
    int nextJgrpIndex;            /* Linked metadata index for array/group scans. */
};

struct jobMetaQueue {
    int head;                     /* Oldest metadata slot. */
    int tail;                     /* Next metadata slot to write. */
    int capacity;                 /* Number of metadata slots. */
    struct jobMetaData jobUnit[]; /* Ring storage embedded in shared memory. */
};

struct bufferQueue {
    long long head;               /* Oldest byte offset still retained. */
    long long tail;               /* Next byte offset to write. */
    long long capacity;           /* Buffer size in bytes. */
    char buff[];                  /* Ring bytes embedded in shared memory. */
};

struct queryReaderInfo {
    pid_t pid;                    /* qmbd process id. */
    time_t forkTime;              /* Time this qmbd generation was forked. */
    int readStartIndex;           /* First metadata slot this qmbd may need. */
    int valid;                    /* Nonzero while this reader slot is active. */
    int status;                   /* Reader lifecycle/debug status. */
};

struct syncJobShm {
    int shmId;                    /* System V shared-memory id. */
    struct jobMetaQueue *jobMetaQueue; /* Metadata ring in shared memory. */
    struct bufferQueue *xdrBuffer;     /* Encoded job info ring. */
    struct bufferQueue *jobNameBuffer; /* Job name string ring. */
    struct queryReaderInfo queryReaderInfo[MAX_QMBD_NUMS]; /* qmbd readers. */
};

/* qmbd shared-memory sync state and APIs. */
extern long long      syncShmXdrBufferSize;     /* SHM XDR ring size in bytes */
extern long long      syncShmJobNameBufferSize; /* SHM job-name ring size in bytes */
extern int            syncShmJobCapacity;       /* SHM metadata ring capacity */
extern struct syncJobShm *shm;                  /* Attached SHM sync state */
extern int            initQmbdShmSync(void);
extern int            qmbdShmSyncSubmitJob(struct submitReq *, struct jData *);
extern int            registerReaderToShm(pid_t, time_t);
extern int            selectQmbdShmSyncJobs(struct jobInfoReq *,
                                            struct jobMetaData ***, int *);
extern int            selectQmbdShmSyncJgrps(struct jobInfoReq *,
                                             struct jobMetaData ***, int *);
extern int            chanWriteQmbdShmJobXdr(int, struct jobMetaData *);
/*---------------- shared-memory job synchronization end -------------------*/

/*------------------ socket job synchronization start ----------------------*/
/* qmbd socket sync definitions. */
enum qmbdSubmitSenderState {
    QMBD_SUBMIT_STOPPED = 0,      /* Sender is idle and old fd is safe to close. */
    QMBD_SUBMIT_ACTIVE = 1,       /* Sender may dequeue and write submit events. */
    QMBD_SUBMIT_STOP_REQUESTED = 2 /* Sender should drop old events and stop. */
};

/* qmbd socket sync state and APIs. */
extern int            qmbdSubmitSockPair[2];    /*Socketpair fds used for qmbd submit events*/
extern int            qmbdSubmitChfd;           /*Current process channel descriptor for qmbd submit events*/
extern int            qmbdSubmitSenderState;    /*Current submit sender lifecycle state*/
extern int            initQmbdEventSender(void);
extern void           activateQmbdEventSender(int);
extern void           requestQmbdEventSenderStop(void);
extern int            handleQmbdSubmitEventIO(void);
extern int            qmbdSocketSyncSubmitJob(struct submitReq *, struct jData *);
extern int            selectQmbdSocketSyncJobs(struct jobInfoReq *,
                                               struct jData ***, int *);
extern int            selectQmbdSocketSyncJgrps(struct jobInfoReq *,
                                                struct nodeList **, int *);
/*------------------- socket job synchronization end -----------------------*/

#endif

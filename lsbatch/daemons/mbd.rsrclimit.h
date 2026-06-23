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
#ifndef _MBD_RESLIMIT_H
#define _MBD_RESLIMIT_H

#include "mbd.h"

/*Note: If add new consumer type, please append it to the following definitions
 * rl_consumer_type
 * rl_pend_level
 * RL_CONSUMER_POSITION_*
 * RL_CONSUMER_POSITION_MAX++
 */
/*The type of the resource limit consumer*/
enum rl_consumer_type {
    RL_CONSUMER_TYPE_HOST,
    RL_CONSUMER_TYPE_QUEUE,
    RL_CONSUMER_TYPE_USER,
    RL_CONSUMER_TYPE_PROJECT
};

/* Fixed position of consumer type in the limit consumer list and bitmap array*/
#define RL_CONSUMER_POSITION_HOST 0
#define RL_CONSUMER_POSITION_QUEUE 1
#define RL_CONSUMER_POSITION_USER 2
#define RL_CONSUMER_POSITION_PROJECT 3
#define RL_CONSUMER_POSITION_MAX 4 /*If add new consumer type, update this*/

/*The mode of the resource limit consumer*/
#define RL_CONSUMER_MODE_SHARED 1
#define RL_CONSUMER_MODE_PER 2

/*The reserve type of a resource*/
#define RSRC_PER_JOB  1
#define RSRC_PER_HOST 2
#define RSRC_PER_TASK 3

typedef struct rlConsumer {
    char * value;  /*A list of consumer name delimited by a space*/
    int type;      /*The type of the consumer, host/queue/user/project*/
    int mode;      /*SHARED or PER*/
} RL_CONSUMER_T;

enum rl_resource_type {
    RL_RESOURCE_TYPE_JOBS,
    RL_RESOURCE_TYPE_SLOTS,
    RL_RESOURCE_TYPE_MEM,
    RL_RESOURCE_TYPE_SWP,
    RL_RESOURCE_TYPE_TMP,
    RL_RESOURCE_TYPE_SLOTS_PER_PROCESSOR,
    RL_RESOURCE_TYPE_RSRC
};
typedef struct rlResource {
    char * resName;
    int resNo;
    float value;
    int isPercent;
    int type; /*the type of resource, to reduce str compare*/
} RL_RESOURCE_T;

/*The level of pending reason for the resource limit*/
enum rl_pend_level {
    RL_PEND_LEVEL_CLUSTER,
    RL_PEND_LEVEL_QUEUE,
    RL_PEND_LEVEL_USER,
    RL_PEND_LEVEL_PROJECT,
    RL_PEND_LEVEL_HOST,
};
typedef struct resourceLimit {
    char * name;
    int nConsumers;
    RL_CONSUMER_T *consumers;
    int nResources;
    RL_RESOURCE_T * resources;
    char * desc;
    int level;
    int refer;   /*how many entries in rlAccountTab reference this limit config*/
} RL_LIMIT_T;

typedef struct resourceLimitConf {
    int nLimits;
    RL_LIMIT_T * limits;
} RL_CONF_T;

/**
 * RL_JOB_LIMITS_CACHE_T - the cache of a job's matched resource limits in scheduling session
 * @mainLimits: the list of a job's matched limits which do not configure the host consumer
 * @hostLimits: the list of a job's matched limits which configures the host consumer
 */
typedef struct  rlJobLimitsCache {
    int *rlBitmaps[RL_CONSUMER_POSITION_MAX]; /*limit matched bitmap for a job; one row per consumer dimension, rows are cache-owned (do not free individually)*/
    LIST_T *mainRLimits;  /*member is RL_ALLOC_RLIMIT_T*/
    LIST_T *hostRLimits;  /*member is RL_ALLOC_RLIMIT_T*/
} RL_JOB_LIMITS_CACHE_T;

#define RL_BUILDIN_RESOURCE_JOBS_NAME "JOBS"
#define RL_BUILDIN_RESOURCE_SLOTS_NAME "SLOTS"
#define RL_BUILDIN_RESOURCE_MEM_NAME "MEM"
#define RL_BUILDIN_RESOURCE_SWP_NAME "SWP"
#define RL_BUILDIN_RESOURCE_TMP_NAME "TMP"
#define RL_BUILDIN_RSC_SLOTS_PER_PROCESSOR_NAME "SLOTS_PER_PROCESSOR"

typedef struct rlResourceAccount {
    char * name;
    int resNo;
    float limit;
    float used;
} RL_RESOURCE_ACCOUNT_T;

typedef struct rlUsage {
    int limitNo;
    int rsrcCnt;
    RL_RESOURCE_ACCOUNT_T * rsrcAccounts;
} RL_USAGE_T;

typedef struct rlRsrcAvail {
    char * name;
    int resNo;
    float orgAvail;
    float avail;
} RL_RESOURCE_AVAIL_T;

union rlCacheRef {
    char *hostname; /*when host dimension is per-host mode, we store the host name here*/
    int numRef;     /*when host dimension is shared mode, we store the reference count here*/
};

typedef struct allocRlimit {
    struct allocRlimit  *forw;
    struct allocRlimit  *back;
    int limitNo;
    union rlCacheRef hostRef;
    int rsrcCnt;
    RL_RESOURCE_AVAIL_T * rsrcAvails;
    hEnt *usageEnt; /*reference to the rlAccountTab entry; NULL if no usage record*/
    int accumSlots;    /*Used to update the accounting of limits with host dimensions when job resource consumption changes.*/
} RL_ALLOC_RLIMIT_T;

typedef  struct rlRsrcDelta {
    int rsrcType;
    float delta;
    int resNo;
} RL_RSRC_DELTA_T;

extern RL_CONF_T generalRLConf; /*the map of configured resource limits*/
extern hTab rlAccountTab; /*the usage of active resource limit, key is limitKey, value is RL_USAGE_T*/
extern int rlBitmapSize;  /*bitmap size in int units, = GET_INTNUM(generalRLConf.nLimits)*/
extern RL_JOB_LIMITS_CACHE_T jobRLimitsCache; /*The cache of job limits which life-cycle is a single scheduling session*/

extern void initRlData(void);
extern void readRsrcLimitConf(int mbdInitFlags);
extern void freeRsrcLimitConf(void);
extern void freeRLJobLimitsCache(RL_JOB_LIMITS_CACHE_T *cache);
extern void freeRLAllocLimitEntry(LIST_ENTRY_T *entry);
extern RL_ALLOC_RLIMIT_T *checkRsrcLimit(int *rlBitmaps[RL_CONSUMER_POSITION_MAX],
                                          struct jData *jp,
                                          struct hData *hp,
                                          LIST_T **matchedRLimits);
extern int ckHostSlots4RLimits(struct hData *hp, struct jData *jp,
                               RL_ALLOC_RLIMIT_T **allocLimit,
                               RL_RESOURCE_AVAIL_T **rsrcAvail);
extern int getHostSlots4RLimit(struct jData *jp, struct hData *hp,
                               LIST_T **hostRLimits,
                               RL_ALLOC_RLIMIT_T **failedLimit,
                               RL_RESOURCE_AVAIL_T **failedRsrc);
extern void mergeAllocRLimit2Cache(LIST_T *hostRLimit, struct jData *jp,
                                   int nSlots);
extern void resetJobRLimitsCache4Host(void);
extern void updRLAccountTabByCache(struct jData *jp);
extern void updRLAccount4Job(struct jData *jp, char *caller);
extern void cleanRLAccount4Job(struct jData *jp, char *caller);
extern int checkRLimits4Bmod(struct jData *orgJob, struct jData *newJob);
extern int checkRLimits4Switch(struct jData *job, struct qData *qtp);

extern int buildRsrcLimitInfoReply(struct rsrcLimitInfoReq *req,
                                  struct rsrcLimitInfoReply *reply);
extern void freeRsrcLimitInfoReply(struct rsrcLimitInfoReply *reply);

#endif /*_MBD_RESLIMIT_H*/

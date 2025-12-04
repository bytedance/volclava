/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * Copyright (C) 2011 David Bigagli
 * Copyright (C) 2007 Platform Computing Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#include "mbd.h"
#include "mbd.fairshare.h"

#define NL_SETN         10
#define SORT_HOST_NUM   30
#define IS_FS_JOB(job)   (job->qPtr->qAttrib & Q_ATTRIB_FS)

enum candRetCode {
    CAND_NO_HOST,
    CAND_HOST_FOUND,
    CAND_FIRST_RES
};

enum dispatchAJobReturnCode {
    DISP_OK,
    DISP_FAIL,
    DISP_RESERVE,
    DISP_NO_JOB,
    DISP_TIME_OUT
};

struct backfillee {
    struct backfillee *forw;
    struct backfillee *back;
    struct jData *backfilleePtr;
    int indexInCandHostList;
    int backfillSlots;

    int numHostPtr;
    struct hData **hPtr;
};

struct backfillCand {
    int numSlots;
    int numAvailSlots;
    int indexInCandHostList;
    LIST_T *backfilleeList;
};

struct backfilleeData {
    struct backfilleeData *forw;
    struct backfilleeData *back;
    struct jData *backfilleePtr;
    LIST_T *slotsList;
};

struct leftTimeTable {
    int leftTime;
    int slots;
};

#define OUT_SCHED_RS(reason)                    \
    ((reason) == PEND_HOST_JOB_LIMIT            \
     || (reason) ==  PEND_QUE_JOB_LIMIT         \
     || (reason) ==  PEND_QUE_USR_JLIMIT        \
     || (reason) == PEND_QUE_PROC_JLIMIT        \
     || (reason) == PEND_QUE_HOST_JLIMIT        \
     || (reason) == PEND_USER_JOB_LIMIT         \
     || (reason) == PEND_UGRP_JOB_LIMIT         \
     || (reason) == PEND_USER_PROC_JLIMIT       \
     || (reason) == PEND_UGRP_PROC_JLIMIT       \
     || (reason) == PEND_HOST_USR_JLIMIT)

#define QUEUE_IS_BACKFILL(qPtr) ((qPtr)->qAttrib & Q_ATTRIB_BACKFILL)

#define JOB_HAS_RUN_LIMIT(jp)                                   \
    ((jp)->shared->jobBill.rLimits[LSF_RLIMIT_RUN] > 0 ||       \
     (jp)->qPtr->rLimits[LSF_RLIMIT_RUN] > 0)

#define JOB_CAN_BACKFILL(jp) (QUEUE_IS_BACKFILL((jp)->qPtr) &&  \
                              JOB_HAS_RUN_LIMIT(jp))

#define HAS_BACKFILL_POLICY (qAttributes & Q_ATTRIB_BACKFILL)

#define Q_H_REASON_NOT_DUE_TO_LIMIT(qhreason)   \
    ((qhreason) != PEND_QUE_PROC_JLIMIT &&      \
     (qhreason) != PEND_QUE_HOST_JLIMIT &&      \
     (qhreason) != PEND_HOST_JOB_LIMIT)
#define HOST_UNUSABLE_TO_JOB_DUE_TO_Q_H_REASON(qhreason, jp)    \
    (qhreason &&                                                \
     (Q_H_REASON_NOT_DUE_TO_LIMIT(qhreason) ||                  \
      (!Q_H_REASON_NOT_DUE_TO_LIMIT(qhreason) &&                \
       !QUEUE_IS_BACKFILL(jp->qPtr))))

#define HOST_UNUSABLE_DUE_TO_H_REASON(hreason)                          \
    (hreason && !(HAS_BACKFILL_POLICY && hreason == PEND_HOST_JOB_LIMIT))

#define HOST_UNUSABLE_TO_JOB_DUE_TO_H_REASON(hreason, jp)               \
    (hreason &&                                                         \
     !(QUEUE_IS_BACKFILL((jp)->qPtr) && hreason == PEND_HOST_JOB_LIMIT))


#define HOST_UNUSABLE_TO_JOB_DUE_TO_U_H_REASON(uhreason, jp)    \
    (uhreason &&                                                \
     (!QUEUE_IS_BACKFILL((jp)->qPtr) ||                         \
      (uhreason != PEND_USER_PROC_JLIMIT &&                     \
       uhreason != PEND_HOST_USR_JLIMIT)))


#define CANT_FINISH_BEFORE_DEADLINE(runLimit, deadline, cpuFactor)      \
    ((runLimit)/(cpuFactor) + now_disp > (deadline))

#define REASON_TABLE_COPY(src, dest, fname)                             \
    {                                                                   \
        FREEUP (dest->reasonTb);                                        \
        dest->numReasons = src->numReasons;                             \
        if (dest->numReasons > 0) {                                     \
            dest->reasonTb = (int *) my_calloc (dest->numReasons,       \
                                                sizeof(int), fname);    \
            memcpy((char *)dest->reasonTb, (char *)src->reasonTb,       \
                   sizeof(int) * dest->numReasons);                     \
        }                                                               \
    }

#define QUEUE_SCHED_DELAY(jpbw)                         \
    (((jpbw)->qPtr->schedDelay == INFINIT_INT) ?        \
     DEF_Q_SCHED_DELAY : (jpbw)->qPtr->schedDelay)


#define END_OF_JOB_LIST(jp, listNum) ((jp) == jDataList[listNum])


#define HAS_JOB_LEVEL_SPAN_PTILE(jp)                    \
    ((jp)->shared->resValPtr != NULL &&                 \
     (jp)->shared->resValPtr->pTile != INFINIT_INT)

#define QUEUE_HAS_SPAN_PTILE(qPtr)              \
    ((qPtr)->resValPtr != NULL &&               \
     (qPtr)->resValPtr->pTile != INFINIT_INT)

#define HAS_QUEUE_LEVEL_SPAN_PTILE(jp) (QUEUE_HAS_SPAN_PTILE((jp)->qPtr))

#define JOB_LEVEL_SPAN_PTILE(jp) ((jp)->shared->resValPtr->pTile)
#define QUEUE_LEVEL_SPAN_PTILE(jp) ((jp)->qPtr->resValPtr->pTile)

#define HAS_JOB_LEVEL_SPAN_HOSTS(jp)            \
    ((jp)->shared->resValPtr != NULL &&         \
     (jp)->shared->resValPtr->maxNumHosts == 1)

#define QUEUE_HAS_SPAN_HOSTS(qPtr)              \
    ((qPtr)->resValPtr != NULL &&               \
     (qPtr)->resValPtr->maxNumHosts == 1)

#define HAS_QUEUE_LEVEL_SPAN_HOSTS(jp) (QUEUE_HAS_SPAN_HOSTS((jp)->qPtr))

#define HAS_JOB_LEVEL_SPAN(jp)                                          \
    (HAS_JOB_LEVEL_SPAN_PTILE(jp) || HAS_JOB_LEVEL_SPAN_HOSTS(jp))

#define HAS_QUEUE_LEVEL_SPAN(jp)                                        \
    (HAS_QUEUE_LEVEL_SPAN_PTILE(jp) || HAS_QUEUE_LEVEL_SPAN_HOSTS(jp))

#define HOST_HAS_ENOUGH_PROCS(jp, host, requestedProcs)         \
    (jobMaxUsableSlotsOnHost(jp, host) >= requestedProcs)

#define OTHERS_IS_IN_ASKED_HOST_LIST(jp) ((jp)->askedOthPrio >= 0)

static int reservePreemptResourcesForExecCands(struct jData *jp);
static int reservePreemptResources(struct jData *jp, int numHosts,
                                   struct hData **hosts);


#define FORALL_PRMPT_HOST_RSRCS(hostn, resn, val, jp)                   \
    if (jp->numRsrcPreemptHPtr && jp->rsrcPreemptHPtr != NULL) {        \
                                                                        \
    for (hostn = 0;                                                     \
         hostn == 0 || (slotResourceReserve &&                          \
                        hostn < jp->numRsrcPreemptHPtr);                \
         hostn++) {                                                     \
    if (jp->rsrcPreemptHPtr[hostn]->hStatus & HOST_STAT_UNAVAIL)        \
        continue;                                                       \
    FORALL_PRMPT_RSRCS(resn) {                                          \
    GET_RES_RSRC_USAGE(resn, val, jp->shared->resValPtr,                \
                       jp->qPtr->resValPtr);                            \
    if (val <= 0.0)                                                     \
        continue;

#define ENDFORALL_PRMPT_HOST_RSRCS } ENDFORALL_PRMPT_RSRCS;             \
    }                                                                   \
                                                                    }

#define CANNOT_BE_PREEMPTED_FOR_RSRC(s) ( (s->jFlags & JFLAG_URGENT) || \
                                          (s->jFlags & JFLAG_URGENT_NOSTOP) || \
                                          (s->jStatus & JOB_STAT_UNKWN))

static int readyToDisp(struct jData *jpbw, int *numAvailSlots);
static enum candRetCode getCandHosts(struct jData *);
static int getLsbUsable(void);
static struct candHost *getJUsable(struct jData *, int *, int *);
static void addReason(struct jData *jp, int hostId, int aReason);
static int allInOne(struct jData *jp);
static int ckResReserve(struct hData *hD, struct resVal *resValPtr,
                        int *resource, struct jData *jp);

static int getPeerCand(struct jData *jobp);
static int isPeerJob(struct jData *, struct jData *);
static int getPeerCand1(struct jData *jobp, struct jData *jpbw);
static void copyPeerCand(struct jData *jobp, struct jData *jpbw);
static void reserveSlots(struct jData *);

static int cntUQSlots(struct jData *jpbw, int *numAvailSlots);
static int ckPerHULimits(struct qData *, struct hData *, struct uData *,
                         int *numAvailSlots, int *reason);
static int getHostJobSlots(struct jData *, struct hData *, int *, int, LIST_T **);
static int getHostJobSlots1(int, struct jData *, struct hData *, int *, int);

static int cntUserJobs(struct jData *, struct gData *, struct hData *,
                       int *, int *, int *, int *);

static int candHostOk(struct jData *jp, int indx, int *numAvailSlots,
                      int *hReason);
static int allocHosts(struct jData *jp);
static int deallocHosts(struct jData *jp);
static void jobStarted(struct jData *, struct jobReply *);
static void disp_clean(void);
static int overThreshold(float *load, float *thresh, int *reason);

static void hostPreference(struct jData *, int);
static void hostPreference1(struct jData *, int, struct askedHost *,
                            int, int, int *, int);
static int sortHosts(int , int, int, struct candHost *, int, float, bool_t);
static int notOrdered(int , int, float , float , float, float);
static int cntUserSlots(struct hTab *, struct uData *, int *);
static void checkSlotReserve (struct jData **, int *);
static int cntHostSlots(struct hTab *, struct hData *);
static void jobStartTime(struct jData *jp);
static int isAskedHost(struct hData *hData, struct jData *jp);

static void moveHostPos(struct candHost *, int, int);

extern int scheduleAndDispatchJobs(void);

static int checkIfJobIsReady(struct jData *);
static int scheduleAJob(struct jData *, bool_t, bool_t);
static enum dispatchAJobReturnCode dispatchAJob(struct jData *, int);
static enum dispatchAJobReturnCode dispatchAJob0(struct jData *, int);
static enum candRetCode checkIfCandHostIsOk(struct jData *);
static void getNumSlots(struct jData *);
static enum dispatchAJobReturnCode dispatchToCandHost(struct jData *);
static void getNumProcs(struct jData *);
static void removeCandHost(struct jData *, int);
static bool_t schedulerObserverSelect(void *, LIST_EVENT_T *);
static int schedulerObserverEnter(LIST_T *, void *, LIST_EVENT_T *);
static int schedulerObserverLeave(LIST_T *, void *, LIST_EVENT_T *);
static int j1IsBeforeJ2(struct jData *, struct jData *, struct jData *);
static int queueObserverEnter(LIST_T *, void *, LIST_EVENT_T *);
static int queueObserverLeave(LIST_T *, void *, LIST_EVENT_T *);
static int listNumber(struct jData *);
static int jobIsFirstOnSegment(struct jData *, struct jData *);
static int jobIsLastOnSegment(struct jData *, struct jData *);
int jobsOnSameSegment(struct jData *, struct jData *, struct jData *);
static struct jData *nextJobOnSegment(struct jData *, struct jData *);
static struct jData *prevJobOnSegment(struct jData *, struct jData *);
static void setQueueFirstAndLastJob(struct qData *, int);
static int numOfOccuranceOfHost(struct jData *, struct hData *);
static void removeNOccuranceOfHost(struct jData *, struct hData *, int,
                                   struct hData **);
static struct backfillee *backfilleeCreate(void);
static struct backfillee *backfilleeCreateByCopy(struct backfillee *);
static void sortBackfillee(struct jData *, LIST_T *);
static void insertIntoSortedBackfilleeList(struct jData *, LIST_T *,
                                           struct backfillee *);
static int jobHasBackfillee(struct jData *);
static int candHostInBackfillCandList(struct backfillCand *, int, int);
static void freeBackfillSlotsFromBackfillee(struct jData *);
static bool_t backfilleeDataCmp(void *, void *, int);
static void removeBackfillSlotsFromBackfiller(struct jData *);
static void getBackfillSlotsOnExecCandHost(struct jData *);
static void doBackfill(struct jData *);
static void deallocExecCandPtr(struct jData *);
static int jobCantFinshBeforeDeadline(struct jData *, time_t);
static void copyCandHosts(int, struct askedHost *, struct candHost *,
                          int *, struct jData *, int, int, int *);
static void copyCandHostData(struct candHost* , struct candHost* );
static int noPreference(struct askedHost *, int, int);
static int imposeDCSOnJob(struct jData *, time_t *, int *, int *);
static void updateQueueJobPtr(int, struct qData *);
static void copyReason(void);
static void clearJobReason(void);
static int isInCandList (struct candHost *, struct hData *, int);
static bool_t enoughMaxUsableSlots(struct jData *);
static int jobMaxUsableSlotsOnHost(struct jData *, struct hData *);
static void hostHasEnoughSlots(struct jData *, struct hData *, int, int, int,
                               int *);
static void checkHostUsableToSpan(struct jData *, struct hData *, int, int *,
                                  int *);
static void reshapeCandHost(struct jData *, struct candHost *, int *);
static void getSlotsUsableToSpan(struct jData *, struct hData *, int, int *);
static void exchangeHostPos(struct candHost *, int, int);
static int notDefaultOrder (struct resVal *);
static int isQAskedHost (struct hData *, struct jData *);
static int totalBackfillSlots(LIST_T *);
static float getNumericLoadValue(const struct hData *hp, int lidx);
static void  getRawLsbLoad (int, struct candHost *);
static int handleFirstHost(struct jData *, int, struct candHost * );
static int needHandleFirstHost(struct jData *);
static bool_t jobIsReady(struct jData *);
static bool_t isCandHost (char *, struct jData *);
void updPreemptResourceByRUNJob(struct jData *);
void checkAndReserveForPreemptWait(struct jData *);
int markPreemptForPRHQValues(struct resVal *, int, struct hData **,
                             struct qData *);
int markPreemptForPRHQInstance(int needResN, float needVal,
                               struct hData *needHost, struct qData *needQPtr);

static int needHandleXor(struct jData *);
static enum candRetCode handleXor(struct jData *);
static enum candRetCode XORCheckIfCandHostIsOk(struct jData *);
static enum dispatchAJobReturnCode XORDispatch(struct jData *, int, enum dispatchAJobReturnCode (*)(struct jData *, int));
static void copyCandHostPtr(struct candHost **, struct candHost **, int *, int *);
static void removeCandHostFromCandPtr(struct candHost **, int *, int i);
static void groupCandsCopy(struct jData *dest, struct jData *src);
static void groupCandHostsCopy(struct groupCandHosts *dest, struct groupCandHosts *src);
static void groupCandHostsInit(struct groupCandHosts *gc);
static void inEligibleGroupsInit(int **inEligibleGroups, int numGroups);
static void groupCands2CandPtr(int numOfGroups, struct groupCandHosts *gc,
                               int *numCandPtr, struct candHost **candPtr);
static void initSchedPendJobSet(struct fairsharePolicy *policy);
static int addSchedPendJob2Set(struct fsQueueJobSet *, void *);

struct jData *currentHPJob;
static bool_t      lsbPtilePack = FALSE;

float bThresholds[]={0.1, 0.1, 0.1, 0.1, 5.0, 5.0, 1.0, 5.0, 2.0, 2.0, 3.0};
int numLsbUsable = 0;
int freedSomeReserveSlot;

static struct jData *currentJob[PJL+1];

static time_t now_disp;
static int newSession[PJL+1];

#undef MBD_PROF_COUNTER
#define MBD_PROF_COUNTER(Func) { 0, #Func },

struct profileCounters counters[] = {
#   include "mbd.profcnt.def"
    { -1, (char *)NULL }
};


static float timeGetJUsable;
static float timeGetQUsable;
static float timeGetCandHosts;
static float timeReadyToDisp;
static float timeCntUQSlots;
static float timeFSElectAJob;
static float timePickAJob;
static float timeScheduleAJob;
static float timeFindBestHosts;
static float timeHostPreference;
static float timeDispatchAJob;
static float timeHostJobLimitOk1;
static float timeHJobLimitOk;
static float timePJobLimitOk;
static float timeFSUpdLoad;
float        timeCollectPendReason;

#define DUMP_TIMERS(fname)                                              \
    {                                                                   \
        if (logclass & LC_PERFM)                                        \
            ls_syslog(LOG_DEBUG,"\
%s timeGetQUsable %.2f ms timeGetCandHosts %.2f ms \
timeGetJUsable %.2f ms timeReadyToDisp %.2f ms timeCntUQSlots %.2f ms \
timeFSUpdLoad %.2f ms timeFSElectAJob %.2f ms timePickAJob %.2f ms timeScheduleAJob %.2f ms \
timeDispatchAJob %.2f ms timeCollectPendReason %.2f ms", \
                      fname,                                            \
                      timeGetQUsable,                                   \
                      timeGetCandHosts,                                 \
                      timeGetJUsable,                                   \
                      timeReadyToDisp,                                  \
                      timeCntUQSlots,                                   \
                      timeFSUpdLoad,                                    \
                      timeFSElectAJob,                              \
                      timePickAJob,                                     \
                      timeScheduleAJob,                                 \
                      timeDispatchAJob,                                 \
                      timeCollectPendReason);                           \
    }

#define ZERO_OUT_TIMERS()                       \
    {                                           \
        timeGetJUsable      = 0.0;              \
        timeGetCandHosts    = 0.0;              \
        timeGetQUsable      = 0.0;              \
        timeReadyToDisp     = 0.0;              \
        timeCntUQSlots      = 0.0;              \
        timeFSElectAJob     = 0.0;              \
        timePickAJob        = 0.0;              \
        timeScheduleAJob    = 0.0;              \
        timeFindBestHosts   = 0.0;              \
        timeHostPreference  = 0.0;              \
        timeHostJobLimitOk1 = 0.0;              \
        timeHJobLimitOk     = 0.0;              \
        timePJobLimitOk     = 0.0;              \
        timeDispatchAJob    = 0.0;              \
        timeCollectPendReason = 0.0;            \
        timeFSUpdLoad       = 0.0;              \
    }

static bool_t  updateAccountsInQueue;

static void resetSchedulerSession(void);

/* openlava round robin
 */
struct jRef {
    struct jRef *forw;
    struct jRef *back;
#define TYPE_JOB_SINGLE 1
#define TYPE_JOB_SET    2
    int type;  /*when type is TYPE_JOB_SINGLE, the job is struct jData *; 
                *when type is TYPE_JOB_SET, job is struct fsQueueJobSet *
                */
    void *job;
};
static LIST_T *jRefList;
static struct fsQueueJobSet schedPendJobSet = {0};

static int
readyToDisp (struct jData *jpbw, int *numAvailSlots)
{
    static char fname[] = "readyToDisp";
    int jReason = 0;
    time_t deadline;

    if (logclass & (LC_PEND))
        ls_syslog(LOG_DEBUG3, "%s: jobId=%s processed=%x oldReason=%d newReason=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->processed, jpbw->oldReason, jpbw->newReason);

    INC_CNT(PROF_CNT_readyToDisp);


    if (!IS_PEND(jpbw->jStatus)) {
        return FALSE;
    }

    if (!(jpbw->jFlags & JFLAG_READY2)) {
        jpbw->numReasons = 0;
        FREEUP (jpbw->reasonTb);
        jpbw->numSlots = 0;
        *numAvailSlots = 0;
        if (logclass & (LC_PEND))
            ls_syslog(LOG_DEBUG2, "%s: Job %s isn't ready for scheduling; newReason=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->newReason);
        return FALSE;
    }

    if (jpbw->shared->jobBill.termTime) {
        if (now_disp >= jpbw->shared->jobBill.termTime) {
            job_abort (jpbw, TOO_LATE);
            return FALSE;
        }
        if (jobCantFinshBeforeDeadline(jpbw, jpbw->shared->jobBill.termTime)) {
            job_abort(jpbw, MISS_DEADLINE);
            return FALSE;
        }
    }


    if (jpbw->jStatus & JOB_STAT_PSUSP) {
        jReason = PEND_USER_STOP;


        if (jpbw->newReason == PEND_JOB_NO_PASSWD ){
            jReason = jpbw->newReason;
        }
    }
    else if (OUT_SCHED_RS(jpbw->qPtr->reasonTb[1][0])) {
        jReason = jpbw->qPtr->reasonTb[1][0];
    }
    else if (OUT_SCHED_RS(jpbw->uPtr->reasonTb[1][0])) {
        jReason = jpbw->uPtr->reasonTb[1][0];
    }
    else if (!(jpbw->qPtr->qStatus & QUEUE_STAT_ACTIVE)) {
        jReason = PEND_QUE_INACT;
    }
    else if (!(jpbw->qPtr->qStatus & QUEUE_STAT_RUN)) {
        jReason = PEND_QUE_WINDOW;
    }
    else if ((jpbw->qPtr->maxJobs != INFINIT_INT)
             && (jpbw->numSlots = jpbw->qPtr->maxJobs - jpbw->qPtr->numJobs
                 + jpbw->qPtr->numPEND) <= 0) {
        jReason = PEND_QUE_JOB_LIMIT;
    }
    else if ((deadline = jpbw->qPtr->runWinCloseTime) > 0 &&
             jobCantFinshBeforeDeadline(jpbw, deadline)) {
        if (logclass & (LC_SCHED | LC_PEND)) {
            char *timebuf = ctime(&deadline);
            timebuf[strlen(timebuf) - 1] = '\0';
            ls_syslog(LOG_DEBUG2, "%s: job <%s> can't finish before deadline: %s", fname, lsb_jobid2str(jpbw->jobId), timebuf);
        }
        jReason = PEND_QUE_WINDOW_WILL_CLOSE;
    }


    else if (now_disp < jpbw->shared->jobBill.beginTime) {
        jReason = PEND_JOB_START_TIME;
    }

    else if (jpbw->jFlags & JFLAG_DEPCOND_INVALID) {
        jReason = PEND_JOB_DEP_INVALID;
    }
    else if (now_disp < jpbw->dispTime) {
        jReason = PEND_JOB_DELAY_SCHED;
    }
    else {
        int i;
        for (i = 0; i < jpbw->uPtr->numGrpPtr; i++) {
            struct uData *ugp = jpbw->uPtr->gPtr[i];
            if (OUT_SCHED_RS(ugp->reasonTb[1][0])) {
                jReason = ugp->reasonTb[1][0];
                break;
            }
        }
    }
    if (!jReason && jpbw->qPtr->uJobLimit < INFINIT_INT
        && jpbw->shared->jobBill.maxNumProcessors == 1) {
        struct userAcct *uAcct;
        uAcct = getUAcct(jpbw->qPtr->uAcct, jpbw->uPtr);
        if (uAcct && (OUT_SCHED_RS(uAcct->reason)))
            jReason = uAcct->reason;
    }


    if (jpbw->shared->jobBill.options2 & SUB2_USE_DEF_PROCLIMIT) {
        jpbw->shared->jobBill.numProcessors =
            jpbw->shared->jobBill.maxNumProcessors =
            (jpbw->qPtr->defProcLimit > 0 ? jpbw->qPtr->defProcLimit : 1);
    }


    jpbw->numSlots = INFINIT_INT;
    *numAvailSlots = INFINIT_INT;
    if (!jReason && jpbw->shared->jobBill.maxNumProcessors > 1) {
        jpbw->numSlots = cntUQSlots(jpbw, numAvailSlots);
        if (jpbw->numSlots == 0) {
            jReason = jpbw->newReason;
        }
    }


    if (jpbw->qPtr->procLimit > 0) {
        jpbw->numSlots = MIN(jpbw->numSlots, jpbw->qPtr->procLimit);
        *numAvailSlots = MIN(*numAvailSlots, jpbw->qPtr->procLimit);
    }


    if (jpbw->qPtr->procLimit > 0 &&
        (jpbw->shared->jobBill.maxNumProcessors < jpbw->qPtr->minProcLimit ||
         jpbw->shared->jobBill.numProcessors > jpbw->qPtr->procLimit)) {
        jReason = PEND_QUE_PROCLIMIT;
    }


    if (LSB_ARRAY_IDX(jpbw->jobId) &&
        (ARRAY_DATA(jpbw->jgrpNode)->counts[JGRP_COUNT_NRUN] +
         ARRAY_DATA(jpbw->jgrpNode)->counts[JGRP_COUNT_NSSUSP] +
         ARRAY_DATA(jpbw->jgrpNode)->counts[JGRP_COUNT_NUSUSP]) >=
        ARRAY_DATA(jpbw->jgrpNode)->maxJLimit) {
        jReason = PEND_JOB_ARRAY_JLIMIT;
    }



    if (jReason) {
        jpbw->newReason = jReason;
        jpbw->numReasons = 0;
        FREEUP (jpbw->reasonTb);
        jpbw->numSlots = 0;
        *numAvailSlots = 0;
        if (logclass & (LC_PEND))
            ls_syslog(LOG_DEBUG2, "%s: Job %s isn't ready for dispatch; newReason=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->newReason);
        return FALSE;
    }

    jpbw->newReason = 0;

    if (logclass & (LC_PEND))
        ls_syslog(LOG_DEBUG3, "%s: Job %s is ready for dispatch; numSlots=%d numAvailSlots=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->numSlots, *numAvailSlots);

    INC_CNT(PROF_CNT_numReadyJobsPerSession);

    return TRUE;
}

int
getMinGSlots(struct uData *uPtr, struct qData *qPtr, int *numGAvailSlots)
{
    static char     fname[] = "getMinGSlots";
    int             minNumAvailSlots = INFINIT_INT;
    int             minGUsableSlots = INFINIT_INT;

    FOR_EACH_USER_ANCESTOR_UGRP(uPtr, grp) {
        struct userAcct      *grpUAcct;
        int                  numGUsableSlots;
        int                  numGUnUsedSlots;
        int                  numGAvailSlots;

        grpUAcct = getUAcct(qPtr->uAcct, grp);
        if (grpUAcct == NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7200,
                                             "%s: expect a non-NULL uAcct for group %s in queue %s, but got a NULL one, please report."), /* catgets 7200 */
                      fname, grp->user, qPtr->queue);
            continue;
        }

        numGUnUsedSlots = MAX(0, grp->maxJobs
                              - (grp->numJobs - grp->numPEND));
        numGAvailSlots = MIN(grpUAcct->numAvailSUSP + numGUnUsedSlots,
                             grp->maxJobs - (grp->numRUN - grp->numRESERVE));
        minNumAvailSlots = MIN(minNumAvailSlots, numGAvailSlots);

        if (grp->maxJobs == INFINIT_INT)
            numGUsableSlots = INFINIT_INT;
        else {
            numGUsableSlots = numGAvailSlots;
        }

        minGUsableSlots = MIN(minGUsableSlots, numGUsableSlots);

    } END_FOR_EACH_USER_ANCESTOR_UGRP;

    *numGAvailSlots = minNumAvailSlots;

    return(minGUsableSlots);

}


static int
cntUQSlots (struct jData *jpbw, int *numAvailSlots)
{
    static char          fname[] = "cntUQSlots";
    struct uData         *up = jpbw->uPtr;
    struct qData         *qp = jpbw->qPtr;
    struct userAcct      *uAcct = NULL;
    int                  i;
    int                  num;
    int                  jReason;
    int                  jReason1;
    int                  minSlots;
    int                  numQueue;
    int                  numUser;
    int                  numUserGroup;
    int                  minGSlots;
    int                  minGAvailSUSP;

    INC_CNT(PROF_CNT_cntUQSlots);

    jReason = 0;
    if (qp->maxJobs == INFINIT_INT)
        numQueue = INFINIT_INT;
    else
        numQueue = qp->maxJobs - (qp->numJobs - qp->numPEND);


    if ( (qp->maxJobs < jpbw->shared->jobBill.maxNumProcessors)
         || (numQueue <= 0)
         || (jpbw->qPtr->slotHoldTime <=0
             && numQueue < jpbw->shared->jobBill.numProcessors) ) {
        if (jpbw->shared->jobBill.maxNumProcessors == 1)
            jReason = PEND_QUE_JOB_LIMIT;
        else
            jReason = PEND_QUE_PJOB_LIMIT;
    }
    minSlots = numQueue;
    if (up->maxJobs == INFINIT_INT)
        numUser = INFINIT_INT;
    else
        numUser = up->maxJobs - (up->numJobs - up->numPEND);
    if (!jReason) {
        if (!jReason && numUser < jpbw->shared->jobBill.numProcessors) {
            if (jpbw->shared->jobBill.maxNumProcessors == 1)
                jReason = PEND_USER_JOB_LIMIT;
            else
                jReason = PEND_USER_PJOB_LIMIT;
        }
    }
    minSlots = MIN(minSlots, numUser);
    numUserGroup = INFINIT_INT;
    jReason1 = 0;
    for (i = 0; i < up->numGrpPtr; i++) {
        struct uData *ugp = up->gPtr[i];
        if (ugp->maxJobs == INFINIT_INT)
            num = INFINIT_INT;
        else
            num = ugp->maxJobs - (ugp->numJobs - ugp->numPEND);
        if (!jReason1 && num < jpbw->shared->jobBill.numProcessors) {
            if (jpbw->shared->jobBill.maxNumProcessors == 1)
                jReason1 = PEND_UGRP_JOB_LIMIT;
            else
                jReason1 = PEND_UGRP_PJOB_LIMIT;
        }
        numUserGroup = MIN(num, numUserGroup);
    }
    minSlots = MIN(minSlots, numUserGroup);
    if (jReason1 != 0 && jReason == 0)
        jReason = jReason1;


    if ((!jReason && qp->uJobLimit < INFINIT_INT)) {
        uAcct = getUAcct(jpbw->qPtr->uAcct, jpbw->uPtr);
    }
    if (!jReason && uAcct != NULL) {

        if (uAcct && (OUT_SCHED_RS(uAcct->reason)))
            jReason = uAcct->reason;
    }
    if (!jReason && uAcct != NULL) {
        if (qp->uJobLimit == INFINIT_INT)
            num = INFINIT_INT;
        else
            num = qp->uJobLimit - uAcct->numRUN - uAcct->numSSUSP
                - uAcct->numUSUSP - uAcct->numRESERVE;
        if (num < jpbw->shared->jobBill.numProcessors) {
            if (jpbw->shared->jobBill.maxNumProcessors == 1)
                jReason = PEND_QUE_USR_JLIMIT;
            else
                jReason = PEND_QUE_USR_PJLIMIT;

        }
        minSlots = MIN(minSlots, num);
    }

    if (jReason) {
        jpbw->newReason = jReason;
        minSlots = 0;
        if (logclass & (LC_PEND))
            ls_syslog(LOG_DEBUG3, "%s: job=%s reason=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->newReason);
    }
    *numAvailSlots = minSlots;
    jpbw->numSlots = minSlots;
    if (NON_PRMPT_Q(jpbw->qPtr->qAttrib)
        || numQueue < jpbw->shared->jobBill.numProcessors
        || jReason == PEND_QUE_USR_JLIMIT
        || jReason == PEND_QUE_USR_PJLIMIT) {
        if (logclass & (LC_SCHED | LC_JLIMIT))
            ls_syslog(LOG_DEBUG3, "%s: jobId=%s numSlots=%d numAvailSlots=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->numSlots, *numAvailSlots);
        return(jpbw->numSlots);
    }



    if (uAcct == NULL) {
        if ((uAcct = getUAcct(jpbw->qPtr->uAcct, jpbw->uPtr)) == NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7201,
                                             "%s: Cannot get userAcct structure for job <%s> in queue <%s>"), /* catgets 7201 */
                      fname,
                      lsb_jobid2str(jpbw->jobId),
                      jpbw->qPtr->queue);
            return (jpbw->numSlots);
        }
    }
    jReason1 = 0;
    minSlots = numQueue;
    *numAvailSlots = numQueue;

    minGSlots = getMinGSlots(uAcct->uData, jpbw->qPtr, &minGAvailSUSP);

    if (minGSlots < INFINIT_INT) {
        if (minGSlots < jpbw->shared->jobBill.numProcessors) {
            if (jpbw->shared->jobBill.maxNumProcessors == 1)
                jReason1 = PEND_UGRP_JOB_LIMIT;
            else
                jReason1 = PEND_UGRP_PJOB_LIMIT;
        }
        minSlots = MIN (minSlots, minGSlots);
        *numAvailSlots = MIN(minGSlots, minGAvailSUSP);
        *numAvailSlots = MIN(minSlots, *numAvailSlots);
    } else {
        minSlots = MIN (minSlots, numUserGroup);
        *numAvailSlots = minSlots;
    }
    if (up->maxJobs < INFINIT_INT) {
        int numAvail;

        numAvail = MIN(MAX(0, numUser) + uAcct->numAvailSUSP,
                       MIN(qp->maxJobs - uAcct->numRUN - uAcct->numRESERVE,
                           up->maxJobs - up->numRUN - up->numRESERVE));
        num = numAvail;

        if (!jReason1 && num < jpbw->shared->jobBill.numProcessors) {
            if (jpbw->shared->jobBill.maxNumProcessors == 1)
                jReason1 = PEND_USER_JOB_LIMIT;
            else
                jReason1 = PEND_USER_PJOB_LIMIT;
        }

        if (num < minSlots) {
            minSlots = num;
            *numAvailSlots = minSlots;
        }
        num = up->maxJobs - up->numRUN - up->numRESERVE;
        if (num < *numAvailSlots)
            *numAvailSlots = num;
        if (*numAvailSlots < 0)
            *numAvailSlots = 0;
    }
    if (jReason1) {
        if (jReason)
            jpbw->newReason = jReason;
        else
            jpbw->newReason = jReason1;
    }
    jpbw->numSlots = minSlots;
    if (logclass & (LC_SCHED | LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: job=%s numSlots=%d numAvailSlots=%d", fname, lsb_jobid2str(jpbw->jobId), jpbw->numSlots, *numAvailSlots);
    return(jpbw->numSlots);

}

static enum candRetCode
getCandHosts (struct jData *jpbw)
{
    static char      fname[] = "getCandHosts";
    int              numJUsable;
    int              nHosts;
    int              nProc;
    int              i;
    int              k;
    int              tmpVal;
    struct candHost  *jUsable;
    struct resVal    *resValPtr;
    int              firstHostId;

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG2, "%s: Entering this routine for job %s",
                  fname, lsb_jobid2str(jpbw->jobId));

    INC_CNT(PROF_CNT_getCandHosts);


    if (numLsbUsable <= 0 || jpbw->qPtr->numUsable <= 0) {
        if (logclass & (LC_SCHED | LC_PEND)) {
            ls_syslog(LOG_DEBUG1, "%s: job <%s> got no candidate hosts because numLsbUsable or queue's numUsable is 0", fname, lsb_jobid2str(jpbw->jobId));
        }
        return (CAND_NO_HOST);
    }



    if ( (jpbw->shared != NULL) &&
         (jpbw->shared->jobBill.numProcessors <=1 ||
          jpbw->shared->resValPtr == NULL ||
          jpbw->shared->resValPtr->pTile == INFINIT_INT)) {
        if (getPeerCand (jpbw)) {
            if (jpbw->candPtr) {
                enum candRetCode retCode;
                INC_CNT(PROF_CNT_getPeerCandFound);



                if ((firstHostId = handleFirstHost(jpbw, jpbw->numCandPtr,
                                                   jpbw->candPtr))) {

                    addReason(jpbw,
                              firstHostId,
                              PEND_FIRST_HOST_INELIGIBLE);
                    return CAND_FIRST_RES;
                }

                if ((retCode = XORCheckIfCandHostIsOk(jpbw)) == CAND_NO_HOST)
                {
                    jpbw->numCandPtr = 0;
                    FREEUP(jpbw->candPtr);
                    if (logclass & (LC_SCHED | LC_PEND)) {
                        ls_syslog(LOG_DEBUG2, "%s: job <%s> got peer's candHost but candHost are not usable to job", fname, lsb_jobid2str(jpbw->jobId));
                    }
                }
                return retCode;
            } else {

                INC_CNT(PROF_CNT_getPeerCandNoFound);

                if (logclass & (LC_SCHED | LC_PEND)) {
                    ls_syslog(LOG_DEBUG1, "\
%s: job <%s> got no candidate hosts because peer has no candidates",
                              fname, lsb_jobid2str(jpbw->jobId));
                }

                return (CAND_NO_HOST);
            }
        }
    }

    if (jpbw->shared->resValPtr)
        resValPtr = jpbw->shared->resValPtr;
    else
        resValPtr = jpbw->qPtr->resValPtr;



    TIMEVAL2(3, jUsable = getJUsable(jpbw, &numJUsable, &nProc), tmpVal);
    timeGetJUsable += tmpVal;

    if (jUsable == NULL)
        return (CAND_NO_HOST);

    INC_CNT(PROF_CNT_numJobsWithCandHostsPerSession);

    if  (jpbw->shared->jobBill.maxNumProcessors > 1) {
        if (HAS_JOB_LEVEL_SPAN_PTILE(jpbw) &&
            numJUsable * JOB_LEVEL_SPAN_PTILE(jpbw) <
            jpbw->shared->jobBill.numProcessors) {
            addReason(jpbw, 0, PEND_JOB_SPREAD_TASK);
        } else if (!HAS_JOB_LEVEL_SPAN(jpbw) &&
                   HAS_QUEUE_LEVEL_SPAN_PTILE(jpbw) &&
                   numJUsable * QUEUE_LEVEL_SPAN_PTILE(jpbw) <
                   jpbw->shared->jobBill.numProcessors) {
            addReason (jpbw, 0, PEND_QUE_SPREAD_TASK);
        }
    }

    jpbw->numCandPtr = numJUsable;
    jpbw->candPtr = my_calloc (numJUsable,
                               sizeof (struct candHost), fname);
    for (i = 0; i < numJUsable; i++) {
        jpbw->candPtr[i] = jUsable[i];
    }

    if ((firstHostId = handleFirstHost(jpbw, jpbw->numCandPtr, jpbw->candPtr))) {

        addReason(jpbw, firstHostId, PEND_FIRST_HOST_INELIGIBLE);
        return CAND_FIRST_RES;
    }

    nHosts = jpbw->numCandPtr;

    if (allInOne (jpbw)
        && (!needHandleFirstHost(jpbw))
        && !jpbw->usePeerCand
        && jpbw->shared->jobBill.numProcessors
        < jpbw->shared->jobBill.maxNumProcessors) {

        struct candHost tmpCand;
        int nMax, iMax;
        for (i = 0; i < nHosts; i++) {
            nMax = jpbw->candPtr[i].numSlots;
            iMax = i;
            for (k = i + 1; k < jpbw->numCandPtr; k++)
                if (jpbw->candPtr[k].numSlots > nMax) {
                    iMax = k;
                    nMax = jpbw->candPtr[k].numSlots;
                }
            tmpCand.hData = jpbw->candPtr[iMax].hData;
            jpbw->candPtr[iMax].hData = jpbw->candPtr[i].hData;
            jpbw->candPtr[iMax].numSlots = jpbw->candPtr[i].numSlots;
            jpbw->candPtr[i].hData = tmpCand.hData;
            jpbw->candPtr[i].numSlots = nMax;
        }
        return (CAND_HOST_FOUND);
    }


    if (jpbw->shared->resValPtr
        && notDefaultOrder(jpbw->shared->resValPtr) == TRUE) {
        resValPtr = jpbw->shared->resValPtr;
    } else {
        resValPtr = jpbw->qPtr->resValPtr;
    }


    TIMEVAL2(3, nHosts = findBestHosts(jpbw, resValPtr, nHosts, jpbw->numCandPtr,
                                      jpbw->candPtr, FALSE), tmpVal);
    timeFindBestHosts += tmpVal;


    TIMEVAL2(3, hostPreference(jpbw, nHosts), tmpVal);
    timeHostPreference += tmpVal;


    if (needHandleXor(jpbw)) {
        return(handleXor(jpbw));
    }

    reshapeCandHost(jpbw, jpbw->candPtr, &(jpbw->numCandPtr));

    return (CAND_HOST_FOUND);
}

static int
getLsbUsable(void)
{
    int i;
    int nLsbUsable;
    int numReasons;
    int ldReason;
    struct hData *hPtr;
    int hReason;

    INC_CNT(PROF_CNT_getLsbUsable);

    nLsbUsable = numReasons = ldReason = 0;

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        i = hPtr->hostId;
        hReason = 0;
        if (hPtr->hStatus & HOST_STAT_REMOTE)
            continue;

        if (hPtr->numJobs == 0)
            hPtr->acceptTime = 0;

        hPtr->numDispJobs = 0;

        if (OUT_SCHED_RS(hReasonTb[1][i])
            && HOST_UNUSABLE_DUE_TO_H_REASON(hReasonTb[1][i])) {
            hReason = hReasonTb[1][i];
        } else if (LS_ISUNAVAIL(hPtr->limStatus))
            hReason = PEND_LOAD_UNAVAIL;
        else if (hPtr->hStatus & HOST_STAT_UNREACH)
            hReason = PEND_SBD_UNREACH;
        else if (hPtr->hStatus & HOST_STAT_UNAVAIL)
            hReason = PEND_SBD_UNREACH;
        else if (hPtr->hStatus & HOST_STAT_WIND)
            hReason = PEND_HOST_WINDOW;
        else if (hPtr->hStatus & HOST_STAT_DISABLED)
            hReason = PEND_HOST_DISABLED;
        else if (hPtr->hStatus & HOST_STAT_EXCLUSIVE)
            hReason = PEND_HOST_EXCLUSIVE;
        else if (hPtr->hStatus & HOST_STAT_NO_LIM)
            hReason = PEND_HOST_NO_LIM;
        else if (LS_ISLOCKEDU(hPtr->limStatus))
            hReason = PEND_HOST_LOCKED;
        else if (LS_ISLOCKEDM(hPtr->limStatus))
            hReason = PEND_HOST_LOCKED_MASTER;
        else if (hPtr->numJobs >= hPtr->maxJobs) {
            hPtr->hStatus |= HOST_STAT_FULL;
            hReason = PEND_HOST_JOB_LIMIT;
        }

        if (!hReason
            && overThreshold(hPtr->lsbLoad, hPtr->loadSched, &ldReason))
            hReason = ldReason;

        if (hReason) {
            hReasonTb[1][i] = hReason;
            numReasons++;
        } else {
            hPtr->reason = 0;
            hReasonTb[1][i] = 0;
            nLsbUsable += hPtr->numCPUs;
        }

        if (hReason)
            ls_syslog(LOG_DEBUG, "\
%s: Host %s isn't eligible; reason=%d", __func__, hPtr->host, hReason);
        else
            ls_syslog(LOG_DEBUG, "\
%s: Got one eligible host %s", __func__, hPtr->host);

    } /* for (hPtr = hostList->back; ...;...) */


    if (nLsbUsable == 0)
        ls_syslog(LOG_DEBUG, "\
%s: Got no eligible host; numReasons=%d, numofhosts=%d",
                  __func__, numReasons, numofhosts());
    else
        ls_syslog(LOG_DEBUG, "\
%s: Got %d eligible CPUs; numReasons=%d",
                  __func__, nLsbUsable, numReasons);

    return nLsbUsable;

}

int
getQUsable(struct qData *qp)
{
    struct hData *hPtr;
    struct jData *jpbw;
    int i;
    int j;
    int overRideFromType;
    int hReason;

    INC_CNT(PROF_CNT_getQUsable);

    qp->numUsable = 0;
    qp->numSlots = 0;
    qp->numReasons = 0;
    qp->qAttrib &= ~Q_ATTRIB_NO_HOST_TYPE;

    if (OUT_SCHED_RS(qp->reasonTb[1][0])) {
        ls_syslog(LOG_DEBUG, "\
%s: Queue %s can't dispatch jobs at the moment; reason=%d",
                  __func__, qp->queue, qp->reasonTb[1][0]);
        return 0;
    }

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        i = hPtr->hostId;
        if (hReasonTb[1][i])
            continue;

        hReason = 0;
        if (hPtr->hStatus & HOST_STAT_REMOTE)
            continue;

        if (!isHostQMember (hPtr, qp)) {
            hReason = PEND_HOST_QUE_MEMB;
            goto next;
        }

        if (overThreshold (hPtr->lsbLoad, qp->loadSched, &j)) {
            hReason = j;
            goto next;
        }

        if (hPtr->numSSUSP > 0) {
            int numSSUSP = hPtr->numSSUSP;

            for (jpbw = jDataList[SJL]->back;
                 (numSSUSP > 0)
                     && (jpbw != jDataList[SJL]);
                 jpbw = jpbw->back) {

                if (jpbw->qPtr->priority < qp->priority)
                    break;
                if (!(jpbw->jStatus & JOB_STAT_SSUSP))
                    continue;
                if (jpbw->hPtr[0] != hPtr)
                    continue;
                numSSUSP--;
                if (jpbw->newReason & SUSP_QUEUE_WINDOW) {
                    continue;
                }
                /* this host cannot be used because there
                 * are some higher priority jobs in SSUSP.
                 */
                hReason = PEND_HOST_JOB_SSUSP;
                goto next;
            }
        }

        j = 1;
        if (qp->resValPtr
            && !getHostsByResReq(qp->resValPtr,
                                 &j,
                                 &hPtr,
                                 NULL,
                                 NULL,
                                 &overRideFromType))
            hReason = PEND_HOST_QUE_RESREQ;

        if (overRideFromType == TRUE) {
            qp->qAttrib |= Q_ATTRIB_NO_HOST_TYPE;
        }
        if (hReason)
            goto next;

        if (OUT_SCHED_RS(qp->reasonTb[1][i])) {
            hReason = qp->reasonTb[1][i];
            goto next;
        }

        if ((qp->acceptIntvl != 0
             && now_disp - hPtr->acceptTime < qp->acceptIntvl * msleeptime)
            || (qp->acceptIntvl == 0
                && hPtr->numDispJobs >= maxJobPerSession))
            hReason =  PEND_HOST_ACCPT_ONE;

    next:
        if (hReason) {
            qp->reasonTb[1][i] = hReason;
            qp->numReasons++;
            continue;
        }
        qp->reasonTb[1][i] = 0;

        ls_syslog(LOG_DEBUG, "\
%s: Got one eligible host %s",
                  __func__, hPtr->host);
        qp->numUsable += hPtr->numCPUs;

    } /* for (hPtr = hDataList->back; ...; ...) */

    if (!qp->numUsable) {
        ls_syslog(LOG_DEBUG, "\
%s: Got no eligible host for queue %s; numReasons=%d",
                  __func__, qp->queue, qp->numReasons);
    } else {
        ls_syslog(LOG_DEBUG, "\
%s: Got %d eligible CPUs for queue %s; numReasons=%d",
                  __func__, qp->numUsable, qp->queue, qp->numReasons);
    }

    return qp->numUsable;
}

static struct candHost *
getJUsable(struct jData *jp, int *numJUsable, int *nProc)
{
    static char fname[] = "getJUsable";
    static struct hData **jUsable;
    static struct candHost *candHosts;
    static struct hData **jUnusable;
    static int *jReasonTb;
    static int nhosts;
    int numHosts;
    int numSlots;
    int numAvailSlots;
    int i;
    int j;
    int num;
    int numReasons;
    int hReason;
    struct hData **thrown = NULL;
    struct hData *hPtr;
    LIST_T *backfilleeList;
    int isWinDeadline;
    int runLimit;
    time_t deadline;
    int numBackfillSlots;
    int numNonBackfillSlots;
    int numAvailNonBackfillSlots;

    numBackfillSlots
        = numNonBackfillSlots = numAvailNonBackfillSlots = 0;

    if (jp == NULL && numJUsable == NULL && nProc == NULL) {

        ls_syslog(LOG_DEBUG, "\
%s: clean up all static variables during reconfig", __func__);

        FREEUP(jUsable);
        FREEUP(candHosts);
        FREEUP(jUnusable);
        FREEUP(jReasonTb);
        return NULL;
    }

    INC_CNT(PROF_CNT_getJUsable);

    if (nhosts != numofhosts()) {

        nhosts = numofhosts();
        FREEUP(jUsable);
        FREEUP(candHosts);
        FREEUP(jUnusable);
        FREEUP(jReasonTb);
        /* floating host this needs to get resized.
         */
        jUsable = my_calloc(nhosts,
                             sizeof(struct hData *), fname);
        candHosts = my_calloc(nhosts,
                              sizeof(struct candHost), fname);
        jUnusable = my_calloc(nhosts,
                               sizeof (struct hData *), fname);
        jReasonTb = my_calloc(nhosts + 1, sizeof(int), fname);
    }

    FREEUP (jp->reasonTb);
    jp->numReasons = 0;
    numHosts = 0;
    numReasons = 0;

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        i = hPtr->hostId;

        INC_CNT(PROF_CNT_firstLoopgetJUsable);

        if (HOST_UNUSABLE_TO_JOB_DUE_TO_H_REASON(hReasonTb[1][i], jp))
            continue;

        if (HOST_UNUSABLE_TO_JOB_DUE_TO_Q_H_REASON(jp->qPtr->reasonTb[1][i], jp))
            continue;

        if (OUT_SCHED_RS(jp->uPtr->reasonTb[1][i])
            && HOST_UNUSABLE_TO_JOB_DUE_TO_U_H_REASON(jp->uPtr->reasonTb[1][i], jp))
            continue;

        if (hPtr->hStatus & HOST_STAT_REMOTE)
            continue;

        if (jp->numAskedPtr == 0 || jp->askedOthPrio >= 0) {
            jUsable[numHosts++] = hPtr;
            continue;
        }

        if (! isAskedHost (hPtr, jp))
            continue;

        jUsable[numHosts++] = hPtr;
        if (numHosts == jp->numAskedPtr)
            break;

    } /* for (hPtr = hostList->back; ...; ...) */

    if (imposeDCSOnJob(jp, &deadline, &isWinDeadline, &runLimit)) {
        num = numHosts;
        numHosts = 0;

        for (i = 0; i < num; i++) {

            if (CANT_FINISH_BEFORE_DEADLINE(runLimit, deadline,
                                            jUsable[i]->cpuFactor)) {
                jUnusable[numReasons] = jUsable[i];
                if (isWinDeadline) {
                    jReasonTb[numReasons++] = PEND_HOST_WIN_WILL_CLOSE;
                } else {
                    jReasonTb[numReasons++] = PEND_HOST_MISS_DEADLINE;
                }
                if (logclass & (LC_SCHED | LC_PEND)) {
                    char *timebuf;
                    timebuf = ctime(&deadline);
                    timebuf[strlen(timebuf)-1] = '\0';
                    ls_syslog(LOG_DEBUG2, "%s: job <%s> can't finish before deadline <%s> on host %s", fname, lsb_jobid2str(jp->jobId), timebuf, jUsable[i]->host);
                }
            } else {
                if (numHosts != i) {
                    jUsable[numHosts] = jUsable[i];
                }
                numHosts++;
            }
        }
    }

    num = numHosts;
    if ((!jp->qPtr->resValPtr
         || !(jp->qPtr->qAttrib & Q_ATTRIB_NO_HOST_TYPE))
        && !jp->shared->resValPtr
        &&  jp->numAskedPtr == 0) {

        numHosts = 0;
        for (i = 0; i < num; i++) {
            INC_CNT(PROF_CNT_secondLoopGetJUsable) ;
            if (strcmp(jp->schedHost, jUsable[i]->hostType) == 0) {
                if (numHosts != i) {
                    jUsable[numHosts] = jUsable[i];
                }
                numHosts++;
            } else {
                jUnusable[numReasons] = jUsable[i];
                jReasonTb[numReasons++] = PEND_HOST_SCHED_TYPE;
                if (logclass & (LC_SCHED | LC_PEND))
                    ls_syslog(LOG_DEBUG2, "%s: Host %s isn't eligible; reason=%d schedHost=%s hostType=%s", fname, jUsable[i]->host, jReasonTb[numReasons-1], jp->schedHost, jUsable[i]->hostType);
            }
        }
    }

    num = numHosts;
    if (jp->shared->resValPtr || jp->qPtr->resValPtr) {
        int noUse;
        struct hData *hData;

        if ((hData = getHostData(jp->shared->jobBill.fromHost)) == NULL
            || (hData->hStatus & HOST_STAT_REMOTE)) {

            if (hData == NULL)
                ls_syslog (LOG_DEBUG, "\
%s: Job <%s> submission host <%s> is not used by the batch system (a client host ?), use same type instead", __func__,
                           lsb_jobid2str(jp->jobId),
                           jp->shared->jobBill.fromHost);

            if ((hData = getHostByType(jp->schedHost)) == NULL) {
                ls_syslog (LOG_INFO, "\
%s: Not the same type %s host as job %s submission host",
                           __func__,
                           jp->schedHost, lsb_jobid2str(jp->jobId));
            }
        }

        if (jp->shared->resValPtr) {

            if (!(jp->shared->resValPtr->options & PR_SELECT)
                && (jp->qPtr->resValPtr
                    && (jp->qPtr->resValPtr->options & PR_SELECT))) {

                if (jp->shared->resValPtr->selectStr) {
                    FREEUP(jp->shared->resValPtr->selectStr);
                    jp->shared->resValPtr->selectStrSize = 0;
                }
                jp->shared->resValPtr->selectStr =
                    safeSave(jp->qPtr->resValPtr->selectStr);
                jp->shared->resValPtr->selectStrSize =
                    strlen(jp->qPtr->resValPtr->selectStr);

                jp->shared->resValPtr->options |= PR_SELECT;
            }

            numHosts = getHostsByResReq(jp->shared->resValPtr,
                                        &numHosts,
                                        jUsable,
                                        &thrown,
                                        hData,
                                        &noUse);
        } else {

            numHosts = getHostsByResReq(jp->qPtr->resValPtr,
                                        &numHosts,
                                         jUsable,
                                        &thrown,
                                        hData,
                                        &noUse);
        }
    }

    if (numHosts < num) {
        j = num - numHosts;
        for (i = 0; i < j; i++) {
            INC_CNT(PROF_CNT_thirdLoopgetJUsable);
            jUnusable[numReasons] = thrown[i];
            jReasonTb[numReasons++] = PEND_HOST_RES_REQ;
            if (logclass & (LC_SCHED | LC_PEND))
                ls_syslog(LOG_DEBUG2, "%s: Host %s isn't eligible; reason=%d", fname, thrown[i]->host, jReasonTb[numReasons-1]);
        }
    }


    FREEUP(thrown);

    if (logclass & (LC_SCHED)) {
        ls_syslog(LOG_DEBUG3, "%s: Got %d hosts", fname, numHosts);
        for (i = 0; i < numHosts; i++)
            ls_syslog(LOG_DEBUG3, "%s: jUsable[%d]->host = %s", fname, i,
                      jUsable[i]->host);
    }

    *numJUsable = 0;
    *nProc = 0;
    for (i = 0; i < numHosts; i++) {

        INC_CNT(PROF_CNT_innerLoopgetJUsable);
        hReason = 0;
        numSlots = 0;

        if (!hReason && (jp->shared->jobBill.options & SUB_EXCLUSIVE)
            && jUsable[i]->numJobs >= 1) {
            hReason = PEND_HOST_NONEXCLUSIVE;
        }

        if (!hReason) {
            int svReason = jp->newReason;

            numSlots = getHostJobSlots(jp,
                                       jUsable[i],
                                       &numAvailSlots,
                                       FALSE,
                                       &backfilleeList);
            numBackfillSlots = totalBackfillSlots(backfilleeList);
            numNonBackfillSlots = numSlots - numBackfillSlots;
            numAvailNonBackfillSlots = numAvailSlots - numBackfillSlots;

            if (numAvailSlots == 0 && backfilleeList != NULL) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7204,
                                                 "%s: job <%s> host <%s> numAvailSlots is 0 and backfilleeList is not NULL!"), fname, lsb_jobid2str(jp->jobId), jUsable[i]->host); /* catgets 7204 */
            }

            numSlots = MIN (numSlots, jp->numSlots);
            if ((numSlots == 0)
                && ( (!JOB_CAN_BACKFILL(jp))
                     || (needHandleFirstHost(jp) == jUsable[i]->hostId)  ) ) {

                hReason = jp->newReason;
            }
            jp->newReason = svReason;
        }

        if (!hReason && jp->shared->resValPtr) {
            int resource;
            int num = ckResReserve (jUsable[i], jp->shared->resValPtr,
                                    &resource, jp);
            if (num < 1) {
                hReason = resource + PEND_HOST_JOB_RUSAGE;
            }
            numSlots = MIN (numSlots, num);
        }
        if (!hReason && jp->qPtr->resValPtr) {
            int resource;
            int num = ckResReserve (jUsable[i], jp->qPtr->resValPtr,
                                    &resource, jp);
            if (num < 1) {
                hReason = resource + PEND_HOST_QUE_RUSAGE;
            }
            numSlots = MIN (numSlots, num);
        }

        if (!hReason && HAS_JOB_LEVEL_SPAN(jp)) {
            checkHostUsableToSpan(jp, jUsable[i], TRUE, &numSlots, &hReason);
            if (hReason) {
                if (logclass & (LC_SCHED | LC_PEND)) {
                    ls_syslog(LOG_DEBUG2, "%s: host: <%s> is not usable to job <%s> because it doesn't satisfy job's job level span requirement", fname, jUsable[i]->host, lsb_jobid2str(jp->jobId));
                }
            }
        }

        if (!hReason && !HAS_JOB_LEVEL_SPAN(jp) && HAS_QUEUE_LEVEL_SPAN(jp)) {
            checkHostUsableToSpan(jp, jUsable[i], FALSE, &numSlots, &hReason);
            if (hReason) {
                if (logclass & (LC_SCHED | LC_PEND)) {
                    ls_syslog(LOG_DEBUG2, "%s: host: <%s> is not usable to job <%s> because it doesn't satisfy job's queue level span requirement", fname, jUsable[i]->host, lsb_jobid2str(jp->jobId));
                }
            }
        }



        if (!hReason && jp->requeMode == RQE_EXCLUDE) {
            for (j = 0; jp->reqHistory[j].host != NULL; j++)
                if (jUsable[i] == jp->reqHistory[j].host) {
                    hReason = PEND_SBD_JOB_REQUEUE;
                    break;
                }
        }

        if (!hReason && !isHostQMember(jUsable[i], jp->qPtr)) {
            hReason = PEND_HOST_QUE_MEMB;
        }


        if (!hReason && overThreshold (jUsable[i]->lsbLoad,
                                       jp->qPtr->loadSched, &hReason)) {


            if (logclass & LC_SCHED)
                ls_syslog(LOG_DEBUG,"\
%s: job=%s; The host=%s belonging to the queue=%s is over threshold",
                          fname, lsb_jobid2str(jp->jobId),
                          jUsable[i]->host, jp->qPtr->queue);
        }

        if (!hReason && overThreshold (jUsable[i]->lsbLoad,
                                       jUsable[i]->loadSched, &hReason)) {


            if (logclass & LC_SCHED)
                ls_syslog(LOG_DEBUG,"\
%s: job=%s the host=%s is over threshold",
                          fname, lsb_jobid2str(jp->jobId), jUsable[i]->host);

        }

        if (hReason) {
            jUnusable[numReasons] = jUsable[i];
            jReasonTb[numReasons++] = hReason;

            if (logclass & (LC_SCHED | LC_PEND))
                ls_syslog(LOG_DEBUG2, "%s: Host %s isn't eligible; reason = %d", fname, jUsable[i]->host, jReasonTb[numReasons-1]);
            continue;
        }

        if (logclass & (LC_SCHED))
            ls_syslog(LOG_DEBUG3, "%s: Got one eligible host %s; numSlots=%d numAvailSlots=%d", fname, jUsable[i]->host, numSlots, numAvailSlots);


        numAvailSlots = MIN(numAvailSlots, numSlots);
        candHosts[*numJUsable].hData = jUsable[i];
        candHosts[*numJUsable].numSlots = MIN(numSlots, jUsable[i]->numCPUs);
        candHosts[*numJUsable].numAvailSlots
            = MIN(numAvailSlots, jUsable[i]->numCPUs);
        candHosts[*numJUsable].numNonBackfillSlots = MIN(numNonBackfillSlots,
                                                         jUsable[i]->numCPUs);
        candHosts[*numJUsable].numAvailNonBackfillSlots =
            MIN(numAvailNonBackfillSlots, jUsable[i]->numCPUs);
        candHosts[*numJUsable].backfilleeList = backfilleeList;

        (*numJUsable)++;
        if (numSlots != INFINIT_INT)
            *nProc += numSlots;
        else
            *nProc = jUsable[i]->numCPUs;
    }


    if (numReasons) {
        int *reasonTb = my_calloc(numReasons + jp->numReasons,
                                  sizeof(int), fname);
        jp->newReason = 0;

        for (i = 0; i < numReasons; i++) {
            reasonTb[i] = jReasonTb[i];
            PUT_HIGH(reasonTb[i], jUnusable[i]->hostId);
        }

        for (i = 0; i < jp->numReasons; i++) {
            reasonTb[i + numReasons] = jp->reasonTb[i];
        }
        jp->numReasons += numReasons;
        FREEUP(jp->reasonTb);
        jp->reasonTb = reasonTb;
    }

    if (*numJUsable == 0) {
        if (logclass & (LC_SCHED | LC_PEND))
            ls_syslog(LOG_DEBUG1, "%s: Got no eligible host for job %s; numReasons=%d", fname, lsb_jobid2str(jp->jobId), jp->numReasons);
        return (NULL);
    }

    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG2, "%s: Got %d eligible hosts for job %s; numReasons=%d", fname, *numJUsable, lsb_jobid2str(jp->jobId), jp->numReasons);
    }

    return candHosts;
}

static int
allInOne(struct jData *jp)
{
    if (jp->shared->resValPtr && jp->shared->resValPtr->maxNumHosts == 1)
        return TRUE;

    if ((!jp->shared->resValPtr || jp->shared->resValPtr->pTile == INFINIT_INT)
        && (jp->qPtr->resValPtr && jp->qPtr->resValPtr->maxNumHosts == 1))
        return TRUE;

    return FALSE;
}

static int
ckResReserve(struct hData *hD, struct resVal *resValPtr, int *resource,
             struct jData *jp)
{
    int jj, isSet, useVal, rusage = 0;
    int canUse = hD->numCPUs;

    INC_CNT(PROF_CNT_ckResReserve);

    *resource = 0;
    if (resValPtr == NULL)
        return canUse;

    for (jj = 0; jj < GET_INTNUM (allLsInfo->nRes); jj++)
        rusage += resValPtr->rusgBitMaps[jj];

    if (rusage == 0) {
        return canUse;
    }
    for (jj = 0; jj < allLsInfo->nRes; jj++) {

        INC_CNT(PROF_CNT_loopckResReserve);

        if (NOT_NUMERIC(allLsInfo->resTable[jj]))
            continue;

        TEST_BIT(jj, resValPtr->rusgBitMaps, isSet);
        if (isSet == 0) {

            continue;
        }

        *resource = jj;

        if (resValPtr->val[jj] >= INFINIT_LOAD
            || resValPtr->val[jj] < 0.01)
            continue;

        if (jj < allLsInfo->numIndx) {
            if (fabs(hD->lsfLoad[jj] - INFINIT_LOAD) < 0.001 * INFINIT_LOAD) {

                return 0;
            }

            if (allLsInfo->resTable[jj].orderType == INCR) {
                useVal = hD->numCPUs;
            } else {
                if (resourcePerTask) { /*resource consumption is per task*/
                    useVal = (int) (hD->lsbLoad[jj]/ resValPtr->val[jj]);
                } else { /*resource consumption is per host*/
                    useVal = hD->lsbLoad[jj] >= resValPtr->val[jj] ? hD->numCPUs:0;
                }
            }

            if ((jj == MEM)
                && (((int) (hD->leftRusageMem/resValPtr->val[MEM])) == 0)){

                if (logclass & LC_SCHED)
                    ls_syslog(LOG_DEBUG, "ckResReserve: Host <%s> doesn't have enough memory for job <%s> rusage. leftRusageMem is %f, reserve memory is %f, lsbLoad memory is %f", hD->host, lsb_jobid2str(jp->jobId), hD->leftRusageMem, resValPtr->val[MEM], hD->lsbLoad[MEM]);

                return 0;
            }

        } else {

            float rVal;
            struct resourceInstance *instance;
            rVal = getUsablePRHQValue(jj,hD,jp->qPtr,&instance);
            if (rVal == -INFINIT_LOAD) {

                if (logclass & LC_SCHED)
                    ls_syslog (LOG_DEBUG2, "ckResReserve: Host <%s> doesn't have the resource <%s> specified", hD->host, allLsInfo->resTable[jj].name);
                return 0;
            } else if (rVal == INFINIT_LOAD) {

                if (logclass & LC_SCHED)
                    ls_syslog (LOG_DEBUG2, "ckResReserve: Host <%s> doesn't have the resource <%s> available", hD->host, allLsInfo->resTable[jj].name);
                return 0;
            }

            if (isItPreemptResourceIndex(jj)) {

                if (rVal >= resValPtr->val[jj] && !JOB_PREEMPT_WAIT(jp)) {
                    rVal -= getReservedByWaitPRHQValue(jj,hD,jp->qPtr);
                }
            }

            if (slotResourceReserve) {
                if (rVal < jp->shared->jobBill.numProcessors*resValPtr->val[jj]
                    && allLsInfo->resTable[jj].orderType != INCR) {
                    useVal = 0;
                } else {
                    useVal = hD->numCPUs;
                }
            } else {
                if (rVal < resValPtr->val[jj]
                    && allLsInfo->resTable[jj].orderType != INCR) {
                    return 0;
                } else {
                    useVal = hD->numCPUs;
                }
            }
        }
        if (useVal < canUse)
            canUse = useVal;
        if (canUse <= 0)
            return 0;
    }

    return canUse;
}

static void
addReason(struct jData *jp, int hostId, int aReason)
{
    int *newTb;
    int i;
    int oldhostId;

    for (i = 0; i < jp->numReasons; i++) {
        GET_HIGH(oldhostId, jp->reasonTb[i]);
        if (oldhostId == hostId)
            break;
    }
    if (i < jp->numReasons) {
        jp->reasonTb[i] = aReason;
        PUT_HIGH(jp->reasonTb[i], hostId);
    } else {
        newTb = myrealloc((void *)jp->reasonTb,
                          (1 + jp->numReasons) * sizeof(int));
        if (newTb) {
            jp->reasonTb = newTb;
            jp->reasonTb[jp->numReasons] = aReason;
            PUT_HIGH(jp->reasonTb[jp->numReasons], hostId);
            jp->numReasons++;
        }
    }
}

static int
getPeerCand(struct jData *jobp)
{
    struct jData *jpbw;
    int numJobs = 0;

    jobp->usePeerCand = FALSE;

    INC_CNT(PROF_CNT_getPeerCand);

    if (jobp->qPtr->qAttrib & Q_ATTRIB_FS) {
        if (schedPendJobSet.nJobs > 0) {
            hEnt *ent = h_getEnt_(&schedPendJobSet.usersTab, jobp->userName);
            if (ent) {
                LIST_T * jList = (LIST_T *)ent->hData;
                struct jobFSRef * jRef;

                for (jRef = (struct jobFSRef *)jList->forw; 
                     jRef != (struct jobFSRef *)jList; 
                     jRef = jRef->frow) {
                    jpbw = jRef->job;
                    if (! isPeerJob(jobp, jpbw)) {
                        continue;
                    }
                    if (getPeerCand1(jobp, jpbw)) {
                        return TRUE;
                    }
                }
            }
        }
    } else {
        for (jpbw = jobp->forw;
            (numJobs < 100 && jpbw != jDataList[PJL]
            && jpbw != jDataList[MJL]);
            jpbw = jpbw->forw) {

            INC_CNT(PROF_CNT_getPeerCandQuick);

            if (jpbw->qPtr->priority > jobp->qPtr->priority)
                break;

            if (! isPeerJob(jobp, jpbw)) {
                continue;
            }

            if (getPeerCand1(jobp, jpbw)) {
                return TRUE;
            }
            numJobs++;
        }
    }
    return FALSE;
}

static int isPeerJob(struct jData *jobp, struct jData *jpbw) {
    int jobDCS, peerDCS, isWinDeadline, jobRunLimit, peerRunLimit;
    time_t jobDeadline, peerDeadline;

    if (!(jpbw->jStatus & (JOB_STAT_PEND | JOB_STAT_MIG))) {
        return FALSE;
    }

    if (!(jpbw->jFlags & JFLAG_READY)) {
        return FALSE;
    }

    if ((jpbw->qPtr->priority == jobp->qPtr->priority) && (jpbw->qPtr != jobp->qPtr)) {
        return FALSE;
    }

    if (jpbw->uPtr != jobp->uPtr) {
        return FALSE;
    }

    if (!(jpbw->processed & JOB_STAGE_CAND)) {
        return FALSE;
    }

    if (QUEUE_IS_BACKFILL(jobp->qPtr) &&
        jobp->shared->jobBill.rLimits[LSF_RLIMIT_RUN] !=
        jpbw->shared->jobBill.rLimits[LSF_RLIMIT_RUN]) {
        return FALSE;
    }
    jobDCS = imposeDCSOnJob(jobp, &jobDeadline, &isWinDeadline,
                            &jobRunLimit);
    peerDCS = imposeDCSOnJob(jpbw, &peerDeadline, &isWinDeadline,
                                &peerRunLimit);
    if (jobDCS || peerDCS) {
        if (!peerDCS || !jobDCS || jobDeadline != peerDeadline ||
            jobRunLimit != peerRunLimit) {
            return FALSE;
        }
    }

    if (jobp->shared->jobBill.numProcessors
            > jpbw->shared->jobBill.numProcessors) {
        return FALSE;
    } else if (jobp->shared->jobBill.numProcessors
                < jpbw->shared->jobBill.numProcessors
            && jpbw->numCandPtr == 0) {
        return FALSE;
    }

    if (jobp->numAskedPtr == 0 && jpbw->numAskedPtr > 0) {
        return FALSE;
    }

    if (jpbw->dispTime > now_disp) {
        return FALSE;
    }

    if ((!jobp->shared->resValPtr && jpbw->shared->resValPtr)
        || (jobp->shared->resValPtr && !jpbw->shared->resValPtr)) {
        return FALSE;
    }

     if ((jobp->shared->resValPtr && jpbw->shared->resValPtr)
        && (strcmp(jpbw->shared->jobBill.resReq,
                    jobp->shared->jobBill.resReq) != 0)) {
        return FALSE;
    }

    if (jobp->shared->resValPtr) {
        if ((jobp->shared->resValPtr->selectStr != NULL)
            && (strstr(jobp->shared->resValPtr->selectStr, "type \"eq\" \"local\""))
            && (strcmp(jpbw->schedHost, jobp->schedHost) != 0)){
            return FALSE;
        }
    }else if (jobp->qPtr->resValPtr){
        if ((jobp->qPtr->resValPtr->selectStr != NULL)
            && (strstr(jobp->qPtr->resValPtr->selectStr, "type \"eq\" \"local\""))
            && (strcmp(jpbw->schedHost, jobp->schedHost) != 0)){
            return FALSE;
        }
    }

    if (!jobp->shared->resValPtr
            && !jpbw->shared->resValPtr
            && !jobp->qPtr->resValPtr
            && !jobp->numAskedPtr
            && !jpbw->numAskedPtr
            && strcmp(jobp->schedHost, jpbw->schedHost) != 0) {
        return FALSE;
    }

    return TRUE;
}

static int
getPeerCand1(struct jData *jobp, struct jData *jpbw)
{
    static int *jReasonTb;
    int i;

    if (jobp == NULL && jpbw == NULL) {
        FREEUP(jReasonTb);
        return 0;
    }

    if (jReasonTb == NULL)
        jReasonTb = my_calloc(numofhosts() + 1, sizeof(int), __func__);

    if (jobp->numAskedPtr > 0
        && jobp->numAskedPtr == jpbw->numAskedPtr
        && jobp->askedOthPrio == jpbw->askedOthPrio) {

        int sameStr = TRUE;
        for (i = 0; i < jobp->numAskedPtr; i++) {

            if (jobp->askedPtr[i].hData != jpbw->askedPtr[i].hData
                || jobp->askedPtr[i].priority != jpbw->askedPtr[i].priority) {
                sameStr = FALSE;
                break;
            }
        }
        if (sameStr) {
            copyPeerCand(jobp, jpbw);
            return TRUE;
        }
    }

    if (jobp->numAskedPtr == 0
        && jpbw->numAskedPtr == 0) {

        if ((jpbw->shared->jobBill.options & SUB_EXCLUSIVE)
            && !(jobp->shared->jobBill.options & SUB_EXCLUSIVE))
            return FALSE;

        copyPeerCand(jobp, jpbw);
        return TRUE;
    }

    return FALSE;
}

static void
copyPeerCand (struct jData *jobp, struct jData *jpbw)
{
    static char fname[] = "copyPeerCand";
    int i;

    jobp->usePeerCand = TRUE;

    REASON_TABLE_COPY(jpbw, jobp, fname);

    if (logclass & (LC_SCHED)) {
        char  tmpJobId[32];
        strcpy (tmpJobId, lsb_jobid2str(jpbw->jobId));
        if (jpbw->candPtr == NULL)
            ls_syslog(LOG_DEBUG2, "%s: Peer <%s> of job <%s> has no candidate", fname, tmpJobId, lsb_jobid2str(jobp->jobId));
        else
            ls_syslog(LOG_DEBUG3, "%s: Job <%s> will use the candidates of peer <%s>", fname, lsb_jobid2str(jobp->jobId), tmpJobId);
    }

    if (jpbw->groupCands) {
        groupCandsCopy(jobp, jpbw);
        groupCands2CandPtr(jobp->numOfGroups, jobp->groupCands,
                           &jobp->numCandPtr, &jobp->candPtr);
        return;
    }

    if (jpbw->candPtr == NULL || jpbw->numCandPtr <= 0) {
        return;
    }

    jobp->candPtr = my_calloc(jpbw->numCandPtr,
                              sizeof (struct candHost), fname);
    for (i = 0; i < jpbw->numCandPtr; i++) {
        copyCandHostData(&(jobp->candPtr[i]), &(jpbw->candPtr[i]));
    }

    jobp->numCandPtr = jpbw->numCandPtr;
}

static int
overThreshold(float *load, float *thresh, int *reason)
{
    char over = FALSE;
    int i;

    for (i = 0; i < allLsInfo->numIndx; i++) {
        if (load[i] >= INFINIT_LOAD || load[i] <= -INFINIT_LOAD
            || (thresh[i] >= INFINIT_LOAD || thresh[i] <= -INFINIT_LOAD)) {
            continue;
        }
        if (allLsInfo->resTable[i].orderType == INCR) {
            if (load[i] > thresh[i]) {
                *reason = i + PEND_HOST_LOAD;
                over = TRUE;
            }
        } else {
            if (load[i] < thresh[i]) {
                *reason = i + PEND_HOST_LOAD;
                over = TRUE;
            }
        }
    }

    return over;
}

int
userJobLimitOk (struct jData *jp, int disp, int *numAvailSlots)
{
    struct uData *uData;
    int i;
    int count;
    int hCount;
    int lCount;
    int numSlots;
    int noPreCount;

    INC_CNT(PROF_CNT_userJobLimitOk);

    if (jp->qPtr->uJobLimit != INFINIT_INT)
        numSlots = uJobLimitOk (jp, jp->qPtr->uAcct, jp->qPtr->uJobLimit, disp);
    else
        numSlots = jp->qPtr->uJobLimit;

    if (numSlots == 0) {
        if (jp->shared->jobBill.maxNumProcessors > 1)
            jp->newReason = PEND_QUE_USR_PJLIMIT;
        else
            jp->newReason = PEND_QUE_USR_JLIMIT;

        *numAvailSlots = 0;
        return 0;
    }
    *numAvailSlots = numSlots;


    if ((uData = getUserData(jp->userName)) == NULL)
        uData = getUserData ("default");

    if (uData != NULL && uData->maxJobs != INFINIT_INT) {

        if (uData->maxJobs <= 0) {
            jp->newReason = PEND_USER_JOB_LIMIT;
            return 0;
        }

        cntUserJobs (jp, NULL, NULL, &hCount, &lCount, &lCount, &noPreCount);
        count = uData->numJobs - uData->numPEND - lCount;
        numSlots = MIN(numSlots, uData->maxJobs - disp * count);
        if (numSlots <= 0) {
            jp->newReason = PEND_USER_JOB_LIMIT;
            *numAvailSlots = 0;
            return 0;
        }
        *numAvailSlots = MIN (numSlots - noPreCount, *numAvailSlots);

        if (jp->shared->jobBill.numProcessors > numSlots) {
            jp->newReason = PEND_USER_PJOB_LIMIT;
            *numAvailSlots = 0;
            return 0;
        }
    }


    for (i = 0; jp->uPtr && i < jp->uPtr->numGrpPtr; i++) {

        uData = jp->uPtr->gPtr[i];
        if (uData == NULL || uData->maxJobs == INFINIT_INT)
            continue;
        if (uData->maxJobs <= 0) {
            jp->newReason = PEND_UGRP_JOB_LIMIT;
            *numAvailSlots = 0;
            return 0;
        }
        cntUserJobs(jp, uData->gData, NULL,
                    &hCount, &lCount, &lCount, &noPreCount);

        count = uData->numJobs - uData->numPEND - lCount;
        numSlots = MIN(numSlots, uData->maxJobs - disp * count);
        if (numSlots <= 0) {
            jp->newReason = PEND_UGRP_JOB_LIMIT;
            *numAvailSlots = 0;
            return 0;
        }
        *numAvailSlots = MIN (numSlots - noPreCount, *numAvailSlots);
        if (jp->shared->jobBill.numProcessors > numSlots) {
            jp->newReason = PEND_UGRP_PJOB_LIMIT;
            *numAvailSlots = 0;
            return 0;
        }
    }

    return numSlots;
}

static int
ckPerHULimits(struct qData *qp, struct hData *hp, struct uData *up,
              int *numAvailSlots, int *reason)
{
    static char fname[] = "ckPerHULimits";
    int numSlots = INFINIT_INT, numSlots1 = INFINIT_INT;
    int i, num, numAvailSlots1 = INFINIT_INT;
    int pJobLimit, numNonPrmptSlots, numAvailSUSP;
    struct hostAcct *hAcct;
    struct uData *ugp;

    INC_CNT(PROF_CNT_ckPerHULimits);

    *numAvailSlots = INFINIT_INT;


    if ((hp->uJobLimit != INFINIT_INT)
        || (up->pJobLimit < INFINIT_FLOAT)
        || (up->pJobLimit < INFINIT_FLOAT)) {
    } else {
        numAvailSUSP = 0;
    }


    if (hp->uJobLimit != INFINIT_INT) {
        struct userAcct *uAcct = getUAcct(hp->uAcct, up);
        num = numNonPrmptSlots = hp->uJobLimit;
        if (uAcct != NULL) {
            numNonPrmptSlots -= (uAcct->numRUN + uAcct->numRESERVE
                                 + uAcct->numSSUSP + uAcct->numUSUSP);
            num -= (uAcct->numRUN + uAcct->numRESERVE);
        }
        numNonPrmptSlots = MAX(0, numNonPrmptSlots);
        *numAvailSlots = numNonPrmptSlots + numAvailSUSP;
        *numAvailSlots = MAX(0, MIN(*numAvailSlots, num));
        numSlots = *numAvailSlots;
        if (numSlots <= 0) {
            *reason = PEND_HOST_USR_JLIMIT;
            if (logclass & (LC_PEND |LC_JLIMIT))
                ls_syslog(LOG_DEBUG2, "%s: Host %s's JL/U (%d) reached for user %s from the pointview of queue %s; reason=%d", fname, hp->host, hp->uJobLimit, up->user, qp->queue, *reason);
            return 0;
        }
    }

    if (up->pJobLimit < INFINIT_FLOAT) {
        hAcct = getHAcct(up->hAcct, hp);
        pJobLimit = (int) ceil((double)(up->pJobLimit*hp->numCPUs));
        num = numNonPrmptSlots = pJobLimit;
        if (hAcct != NULL) {
            numNonPrmptSlots -= (hAcct->numRUN + hAcct->numRESERVE
                                 + hAcct->numSSUSP + hAcct->numUSUSP);
            num -= (hAcct->numRUN + hAcct->numRESERVE);
        }

        numNonPrmptSlots = MAX(0, numNonPrmptSlots);
        numAvailSlots1 = MAX(0, MIN(numNonPrmptSlots + numAvailSUSP, num));
        numSlots1 = numAvailSlots1;
        if (numSlots1 <= 0) {
            if (logclass & LC_JLIMIT)
                ls_syslog(LOG_DEBUG2, "%s: User %s's JL/P (%f) reached on host %s from the pointview of queue %s", fname, up->user, up->pJobLimit, hp->host, qp->queue);
            *reason = PEND_USER_PROC_JLIMIT;
            *numAvailSlots = 0;
            return 0;
        }
    }

    numSlots = MIN(numSlots, numSlots1);
    *numAvailSlots = MIN(*numAvailSlots, numAvailSlots1);

    for (i = 0; i < up->numGrpPtr; i++) {
        ugp = up->gPtr[i];
        if (ugp->pJobLimit >= INFINIT_FLOAT)
            continue;
        numAvailSUSP = 0;
        hAcct = getHAcct(ugp->hAcct, hp);
        pJobLimit = (int) ceil((double)(ugp->pJobLimit*hp->numCPUs));
        num = numNonPrmptSlots = pJobLimit;
        if (hAcct != NULL) {
            numNonPrmptSlots -= (hAcct->numRUN + hAcct->numRESERVE
                                 + hAcct->numSSUSP + hAcct->numUSUSP);
            num -= (hAcct->numRUN + hAcct->numRESERVE);
        }
        numNonPrmptSlots = MAX(0, numNonPrmptSlots);
        numAvailSlots1 = MAX(0, MIN(numNonPrmptSlots + numAvailSUSP, num));
        numSlots1 = numAvailSlots1;
        if (numSlots1 <= 0) {
            if (logclass & LC_JLIMIT)
                ls_syslog(LOG_DEBUG2, "%s: Group %s's JL/P (%f) reached on host %s from the pointview of queue %s", fname, ugp->user, ugp->pJobLimit, hp->host,qp->queue);
            *reason = PEND_UGRP_PROC_JLIMIT;
            *numAvailSlots = 0;
            return 0;
        }

        numSlots = MIN(numSlots, numSlots1);
        *numAvailSlots = MIN(*numAvailSlots, numAvailSlots1);
    }

    if (logclass & LC_JLIMIT)
        ls_syslog(LOG_DEBUG3, "%s: q=%s h=%s u=%s numAvail=%d numSlots=%d",
                  fname, qp->queue, hp->host, up->user,
                  *numAvailSlots, numSlots);
    return numSlots;
}


int
skipAQueue (struct qData *qp2, struct qData *qp1)
{
    return 1;
}

static int
getHostJobSlots (struct jData *jp, struct hData *hp, int *numAvailSlots,
                 int noHULimits, LIST_T **backfilleeList)
{
    static char fname[] = "getHostJobSlots";
    int numNeeded = 1;
    int numSlots, numSlots1, numAvailSlots1;
    int tmpVal = 0;
    struct hostAcct *hAcct;
    struct qData *qp = jp->qPtr;
    LIST_T *theBackfilleeList = NULL;
    LIST_ITERATOR_T iter;
    struct jData *job;
    int backfillSlots, i, numNewHPtr;
    struct backfillee *backfilleeListEntry;
    struct hData **newHPtr;
    PROXY_LIST_ENTRY_T *listEntry, *nextListEntry;

    if (logclass & (LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: job=%s, host=%s",
                  fname, lsb_jobid2str(jp->jobId), hp->host);
    INC_CNT(PROF_CNT_getHostJobSlots);

#define HOST_USABLE(jp, hp)                                             \
    ((jp)->qPtr->reasonTb[1][(hp)->hostId] != PEND_HOST_ACCPT_ONE)


    *backfilleeList = NULL;
    if (hp->pxyRsvJL != NULL && !LIST_IS_EMPTY(hp->pxyRsvJL) &&
        JOB_CAN_BACKFILL(jp) && HOST_USABLE(jp, hp)) {
        (void)listIteratorAttach(&iter, hp->pxyRsvJL);
        for (listEntry = (PROXY_LIST_ENTRY_T *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter);
             listEntry = nextListEntry) {

            listIteratorNext(&iter, (LIST_ENTRY_T **)&nextListEntry);
            job = JOB_PROXY_GET_JOB(listEntry);
            if ( (job->predictedStartTime == 0) ||
                 (job->predictedStartTime < now + RUN_LIMIT_OF_JOB(jp)/hp->cpuFactor) ) {

                if (logclass & LC_SCHED) {
                    ls_syslog(LOG_DEBUG2, "%s: job <%s> can't be backfillee of job <%s> because its start time is unknown or is earlier than backfiller's expected finish time", fname, lsb_jobid2str(job->jobId), lsb_jobid2str(jp->jobId));
                }
                continue;
            }
            for (i = 0; i < job->numCandPtr; i++) {
                if (job->candPtr[i].hData == hp) {
                    break;
                }
            }
            if (i == job->numCandPtr) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7206,
                                                 "%s: host <%s> should be in candHost list of job <%s> but is not"), fname, hp->host, lsb_jobid2str(job->jobId)); /* catgets 7206 */
                continue;
            }

            backfillSlots = MIN(job->candPtr[i].numAvailSlots,
                                numOfOccuranceOfHost(job, hp));
            if (backfillSlots == 0) {
                continue;
            }

            if (logclass & LC_SCHED) {
                ls_syslog(LOG_DEBUG2, "%s: got a backfillee job <%s> with <%d> backfill slots on host <%s> for job <%s>", fname, lsb_jobid2str(job->jobId), backfillSlots, hp->host, lsb_jobid2str(jp->jobId));
            }

            backfilleeListEntry = backfilleeCreate();

            backfilleeListEntry->hPtr = (struct hData **)my_malloc(job->numHostPtr * sizeof(struct hData *), fname);
            for (i = 0; i < job->numHostPtr; i++) {
                backfilleeListEntry->hPtr[i] = job->hPtr[i];
            }
            backfilleeListEntry->numHostPtr = job->numHostPtr;
            backfilleeListEntry->backfilleePtr = job;
            backfilleeListEntry->backfillSlots = backfillSlots;

            numNewHPtr = job->numHostPtr - backfillSlots;
            if (numNewHPtr != 0) {

                newHPtr = (struct hData **)my_calloc((job->numHostPtr - backfillSlots), sizeof(struct hData *), fname);
                removeNOccuranceOfHost(job, hp, backfillSlots, newHPtr);
            } else {
                newHPtr = NULL;
            }


            freeReserveSlots(job);


            job->hPtr = newHPtr;
            job->numHostPtr = numNewHPtr;
            if (numNewHPtr != 0) {
                reserveSlots(job);
            }
            if (theBackfilleeList == NULL) {
                if ((theBackfilleeList = listCreate(NULL)) == NULL) {
                    mbdDie(MASTER_MEM);
                }
            }
            listInsertEntryAtBack(theBackfilleeList, (LIST_ENTRY_T *)backfilleeListEntry);
        }
        if (logclass & LC_SCHED) {
            if (theBackfilleeList == NULL) {
                ls_syslog(LOG_DEBUG2, "%s: job <%s> can't find any backfillee", fname, lsb_jobid2str(jp->jobId));
            }
        }
    }

    hAcct = getHAcct(qp->hAcct, hp);
    TIMEVAL2(3, numSlots = pJobLimitOk(hp, hAcct, qp->pJobLimit), tmpVal);
    timePJobLimitOk += tmpVal;
    if (numNeeded > numSlots) {
        jp->newReason = PEND_QUE_PROC_JLIMIT;
        if (logclass & (LC_JLIMIT))
            ls_syslog(LOG_DEBUG2, "%s: Q's JL/P reached.  Set reason <%d>",
                      fname, jp->newReason);
        *numAvailSlots = 0;
        return (0);
    }
    *numAvailSlots = numSlots;


    TIMEVAL2(3, numSlots1 = hJobLimitOk(hp, hAcct, qp->hJobLimit), tmpVal);
    timeHJobLimitOk += tmpVal;
    if (numNeeded > numSlots1) {
        jp->newReason = PEND_QUE_HOST_JLIMIT;
        *numAvailSlots = 0;
        return (0);
    }
    numSlots = MIN(numSlots, numSlots1);
    *numAvailSlots = numSlots;


    TIMEVAL2(3, numSlots1 = getHostJobSlots1(numNeeded, jp, hp,
                                            &numAvailSlots1, noHULimits), tmpVal);
    timeHostJobLimitOk1 += tmpVal;

    if (numSlots1 == 0) {
        *numAvailSlots = 0;
        return (0);
    }


    numSlots = MIN(numSlots, numSlots1);
    *numAvailSlots = MIN(*numAvailSlots, numAvailSlots1);


    if (theBackfilleeList != NULL) {
        (void)listIteratorAttach(&iter, theBackfilleeList);
        for (backfilleeListEntry = (struct backfillee *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, (LIST_ENTRY_T **)&backfilleeListEntry)) {
            job = backfilleeListEntry->backfilleePtr;

            if (job->numHostPtr != 0) {
                freeReserveSlots(job);
            }


            job->numHostPtr = backfilleeListEntry->numHostPtr;
            job->hPtr = backfilleeListEntry->hPtr;
            backfilleeListEntry->hPtr = NULL;
            backfilleeListEntry->numHostPtr = 0;
            reserveSlots(job);
        }
    }
    *backfilleeList = theBackfilleeList;

    return numSlots;
}

static int
getHostJobSlots1 (int numNeeded, struct jData *jp, struct hData *hp,
                  int *numAvailSlots, int noHULimits)
{
    static char fname[] = "getHostJobSlots1";
    struct qData *qp = jp->qPtr;
    struct uData *up = jp->uPtr;
    int numSlots;
    int numSlots1, numAvailSlots1;

    if (logclass & (LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: job=%s host=%s maxJobs=%d numJobs=%d",
                  fname, lsb_jobid2str(jp->jobId), hp->host, hp->maxJobs, hp->numJobs);

    INC_CNT(PROF_CNT_getHostJobSlots1);


    if (hp->maxJobs == INFINIT_INT) {
        numSlots = INFINIT_INT;
        *numAvailSlots = INFINIT_INT;
    } else {
        struct hostAcct *hAcct = getHAcct(qp->hAcct, hp);
        if (hAcct != NULL) {

            int numUnUsedSlots = MAX(0, hp->maxJobs - hp->numJobs);

            *numAvailSlots = MIN(hp->maxJobs - hp->numRUN - hp->numRESERVE,
                                 hAcct->numAvailSUSP + numUnUsedSlots);

            *numAvailSlots = MAX(0, *numAvailSlots);
            numSlots = *numAvailSlots;
        } else {
            numSlots = hp->maxJobs - hp->numJobs;
            *numAvailSlots = numSlots;
        }

        *numAvailSlots = MAX(0, *numAvailSlots);
        numSlots = MAX(0, numSlots);

        if (numNeeded > numSlots) {
            jp->newReason = PEND_HOST_JOB_LIMIT;
            if (logclass & (LC_JLIMIT))
                ls_syslog(LOG_DEBUG2, "%s: rs=%d job=%s host=%s maxJobs=%d numJobs=%d", fname, jp->newReason, lsb_jobid2str(jp->jobId), hp->host, hp->maxJobs, hp->numJobs);
            *numAvailSlots = 0;
            return (0);
        }
        if (jp->numCandPtr == 1
            && *numAvailSlots < jp->shared->jobBill.numProcessors) {
            if (hReasonTb[1][hp->hostId] == 0) {
                jp->newReason = PEND_HOST_JOB_LIMIT;
            }
        }
    }


    if (noHULimits)
        return (numSlots);


    numSlots1 = ckPerHULimits(qp, hp, up, &numAvailSlots1, &jp->newReason);


    numSlots = MIN(numSlots, numSlots1);
    *numAvailSlots = MIN(*numAvailSlots, numAvailSlots1);
    return (numSlots);

}

int
uJobLimitOk (struct jData *jp, struct hTab *uAcct, int uJobLimit, int disp)
{
    static char fname[] = "uJobLimitOk";
    struct userAcct *ap;
    char found = FALSE;
    int numSlots, numStartJobs;

    if (logclass & (LC_TRACE | LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: job=%s, uJobLimit=%d",
                  fname, lsb_jobid2str(jp->jobId), uJobLimit);
    INC_CNT(PROF_CNT_uJobLimitOk);

    if (uJobLimit <= 0)
        return (0);
    if (uJobLimit == INFINIT_INT)
        return (INFINIT_INT);

    numSlots = uJobLimit;
    if ((ap = getUAcct(uAcct, jp->uPtr)) != NULL) {
        INC_CNT(PROF_CNT_loopuJobLimitOk);
        numStartJobs = ap->numRUN + ap->numSSUSP + ap->numUSUSP;
        if (1 + disp * (numStartJobs + ap->numRESERVE) > uJobLimit) {
            if (logclass & (LC_JLIMIT))
                ls_syslog(LOG_DEBUG2, "%s: job=%s, uJobLimit=%d, numRUN=%d, numSSUSP=%d, numUSUSP=%d, numRESERVE=%d", fname, lsb_jobid2str(jp->jobId), uJobLimit, ap->numRUN, ap->numSSUSP, ap->numUSUSP, ap->numRESERVE);
            return (0);
        }
        if (jp->shared->jobBill.numProcessors +
            disp * (numStartJobs + ap->numRESERVE)> uJobLimit) {
            if (logclass & (LC_JLIMIT))
                ls_syslog(LOG_DEBUG2, "%s: job=%s, uJobLimit=%d, numRUN=%d, numSSUSP=%d, numUSUSP=%d, numRESERVE=%d", fname, lsb_jobid2str(jp->jobId), uJobLimit, ap->numRUN, ap->numSSUSP, ap->numUSUSP, ap->numRESERVE);
            return (0);
        }
        found = TRUE;
        numSlots = uJobLimit - disp * (numStartJobs + ap->numRESERVE);
    }

    if (!found
        && jp->shared->jobBill.maxNumProcessors > 1
        && jp->shared->jobBill.numProcessors > uJobLimit)
        return (0);

    return (numSlots);

}

int
pJobLimitOk (struct hData *hp, struct hostAcct *hAcct, float pJobLimit)
{
    static char fname[] = "pJobLimitOk";
    int numCPUs, numSlots;

    INC_CNT(PROF_CNT_pJobLimitOk);

    if (pJobLimit <= 0.0)
        return (0);
    if (pJobLimit >= INFINIT_FLOAT)
        return (INFINIT_INT);

    numCPUs = hp->numCPUs == 0? 1 : hp->numCPUs;
    numSlots = (int) ceil((double) (pJobLimit * numCPUs));
    if (hAcct != NULL) {
        numSlots = numSlots - hAcct->numRUN - hAcct->numSSUSP
            - hAcct->numUSUSP - hAcct->numRESERVE;
    }
    if (numSlots <= 0) {
        if ((logclass & LC_JLIMIT) && hAcct != NULL)
            ls_syslog(LOG_DEBUG2, "%s: host=%s, pJobLimit=%f, numRUN=%d, numSSUSP=%d, numUSUSP=%d, numRESERVE=%d", fname, hp->host, pJobLimit, hAcct->numRUN, hAcct->numSSUSP, hAcct->numUSUSP, hAcct->numRESERVE);
        return (0);
    }

    return (numSlots);

}

int
hJobLimitOk (struct hData *hp, struct hostAcct *hAcct, int hJobLimit)
{
    static char fname[] = "hJobLimitOk";
    int numSlots;

    INC_CNT(PROF_CNT_hJobLimitOk);

    if (hJobLimit <= 0)
        return (0);
    if (hJobLimit == INFINIT_INT)
        return (INFINIT_INT);

    numSlots = hJobLimit;
    if (hAcct != NULL) {
        numSlots = numSlots - hAcct->numRUN - hAcct->numSSUSP
            - hAcct->numUSUSP - hAcct->numRESERVE;
    }
    if (numSlots <= 0) {
        if ((logclass & LC_JLIMIT) && hAcct != NULL)
            ls_syslog(LOG_DEBUG2, "%s: host=%s, hJobLimit=%d, numRUN=%d, numSSUSP=%d, numUSUSP=%d, numRESERVE=%d", fname, hp->host, hJobLimit, hAcct->numRUN, hAcct->numSSUSP, hAcct->numUSUSP, hAcct->numRESERVE);
        return (0);
    }

    return (numSlots);

}

static int
cntUserJobs (struct jData *jp, struct gData *gp, struct hData *hp,
             int *hCount, int *lCount, int *uhCount, int *noPreemptedJobs)
{
    struct jData *jpbw;

    *hCount = *lCount = *uhCount = *noPreemptedJobs = 0;

    INC_CNT(PROF_CNT_cntUserJobs);


    for (jpbw = jDataList[SJL]->back; jpbw != jDataList[SJL];
         jpbw = jpbw->back) {
        INC_CNT(PROF_CNT_loopcntUserJobs);

        if (!gp || (gp && gMember (jpbw->userName, gp))) {
            if (jpbw->qPtr->priority >= jp->qPtr->priority) {
                *hCount += jpbw->numHostPtr;
            }
        }
    }
    return (0);
}

int
hostSlots (int numNeeded, struct jData *jp, struct hData *hp,
           int disp, int *numAvailSlots)
{
    static char fname[] = "hostSlots";
    struct jData *jpbw, *next;
    struct uData *uData;
    int i, j, num;
    int slots, rsvSlots, nonpreempt;


    int highJobsHost = 0, lowJobsHost = 0;

    int lowRunJobsHost = 0;


    int highJobsHostUser = 0, lowJobsHostUser = 0;

    int runJobsHostUser = 0;

    int lowNonpreemptJobsHost = 0;

    int lowNonpreemptJobsHostUser = 0;

    INC_CNT(PROF_CNT_hostSlots);

    if ((uData = getUserData (jp->userName)) == NULL)
        uData = getUserData ("default");

    if (logclass & (LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: job=%s, host=%s, maxJobs=%d, uJobLimit=%d, pJobLimit=%f, numNeeded=%d", fname, lsb_jobid2str(jp->jobId), hp->host, hp->maxJobs, hp->uJobLimit, uData->pJobLimit, numNeeded);

    *numAvailSlots = 0;
    if (hp->maxJobs <= 0) {
        jp->newReason = PEND_HOST_JOB_LIMIT;
        return (0);
    }
    if (hp->uJobLimit <= 0) {
        jp->newReason = PEND_HOST_USR_JLIMIT;
        return (0);
    }
    if (uData && uData->pJobLimit <= 0.0) {
        jp->newReason = PEND_USER_PROC_JLIMIT;
        return (0);
    }


    for (jpbw = jDataList[SJL]->back; jpbw != jDataList[SJL];
         jpbw = next) {
        next = jpbw->back;
        if (!disp && jp == jpbw)
            continue;
        for (i = 0; i < jpbw->numHostPtr; i++) {
            nonpreempt = FALSE;
            if (jpbw->hPtr[i] != hp)
                continue;


            if (jpbw->qPtr->priority < jp->qPtr->priority) {

                lowJobsHost++;


                if (jpbw->jStatus & JOB_STAT_RUN) {
                    lowRunJobsHost++;
                }

                lowNonpreemptJobsHost++;
                nonpreempt = TRUE;
            } else {
                if (!(jpbw->jStatus & JOB_STAT_RESERVE))
                    highJobsHost++;
            }


            if (jpbw->uPtr == jp->uPtr) {
                if (jpbw->qPtr->priority < jp->qPtr->priority) {
                    lowJobsHostUser++;
                    if (nonpreempt)
                        lowNonpreemptJobsHostUser++;
                } else {
                    highJobsHostUser++;
                }
            }
        }
    }

    if (logclass & (LC_SCHED | LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: highJobsHost=%d lowJobsHost=%d lowNonpreemptJobsHost=%d highJobsHostUser=%d lowJobsHostUser=%d lowNonpreemptJobsHostUser=%d", fname, highJobsHost, lowJobsHost, lowNonpreemptJobsHost, highJobsHostUser, lowJobsHostUser, lowNonpreemptJobsHostUser);


    if (hp->maxJobs == INFINIT_INT)
        num = INFINIT_INT;
    else
        num = hp->maxJobs - highJobsHost - hp->numRESERVE;


    *numAvailSlots = num - lowJobsHost;
    slots = num - lowNonpreemptJobsHost;

    if (numNeeded > slots) {
        jp->newReason = PEND_HOST_JOB_LIMIT;
        *numAvailSlots = 0;
        if (logclass & (LC_JLIMIT))
            ls_syslog(LOG_DEBUG2, "%s: slots=%d numRESERVE=%d",
                      fname, slots, hp->numRESERVE);
        return (0);
    }


    rsvSlots = cntUserSlots (hp->uAcct, jp->uPtr, &runJobsHostUser);
    num = hp->uJobLimit - highJobsHostUser - rsvSlots;
    *numAvailSlots = MIN (*numAvailSlots, num - lowJobsHostUser);
    slots = MIN (slots, num - lowNonpreemptJobsHostUser);

    if (numNeeded > slots) {
        jp->newReason = PEND_HOST_USR_JLIMIT;
        *numAvailSlots = 0;
        if (logclass & (LC_JLIMIT))
            ls_syslog(LOG_DEBUG2, "%s: slots=%d runJobsHostUser=%d rsvSlots=%d",
                      fname, slots, runJobsHostUser, rsvSlots);
        return (0);
    }


    rsvSlots = cntHostSlots (uData->hAcct, hp);
    if (uData && uData->pJobLimit < INFINIT_FLOAT) {
        num = (int) ceil ((double)(uData->pJobLimit * hp->numCPUs));
        num = num - highJobsHostUser - rsvSlots;
        *numAvailSlots = MIN (*numAvailSlots, num - lowJobsHostUser);
        slots = MIN (slots, num - lowNonpreemptJobsHost);

        if (numNeeded > slots) {
            jp->newReason = PEND_USER_PROC_JLIMIT;
            *numAvailSlots = 0;
            if (logclass & (LC_JLIMIT))
                ls_syslog(LOG_DEBUG2, "%s: slots=%d rsvSlots=%d",
                          fname, slots, rsvSlots);
            return (0);
        }
    }



    for (i = 0; i < jp->uPtr->numGrpPtr; i++) {
        uData = jp->uPtr->gPtr[i];
        if (uData == NULL || uData->pJobLimit >= INFINIT_FLOAT)
            continue;
        if (uData->pJobLimit <= 0.0) {
            jp->newReason = PEND_USER_PROC_JLIMIT;
            *numAvailSlots = 0;
            return (0);
        }

        highJobsHost = 0;
        lowJobsHost = 0;
        lowNonpreemptJobsHost = 0;
        for (jpbw = jDataList[SJL]->back; jpbw != jDataList[SJL];
             jpbw = jpbw->back) {
            if (!disp && jp == jpbw)
                continue;
            if (!gMember(jpbw->userName, uData->gData))
                continue;
            if (!disp
                && IS_SUSP(jpbw->jStatus) && (jpbw->newReason & SUSP_MBD_LOCK))
                continue;
            for (j = 0; j < jpbw->numHostPtr; j++) {
                if (jpbw->hPtr[j] == hp) {
                    if (jpbw->qPtr->priority < jp->qPtr->priority) {
                        if (IS_SUSP(jpbw->jStatus)
                            && (jpbw->newReason & SUSP_MBD_LOCK))
                            continue;
                        lowJobsHost++;
                        if (!(jpbw->jStatus & SUSP_MBD_LOCK))
                            lowNonpreemptJobsHost++;
                    } else {
                        highJobsHost++;
                    }
                }
            }
        }
        if (logclass & (LC_JLIMIT))
            ls_syslog(LOG_DEBUG3, "%s: pJobLimit=%f highJobsHost=%d lowJobsHost=%d lowNonpreemptJobsHost=%d ", fname, uData->pJobLimit, highJobsHost, lowJobsHost, lowNonpreemptJobsHost);

        if (uData && uData->pJobLimit < INFINIT_FLOAT) {
            rsvSlots = cntHostSlots (uData->hAcct, hp);
            num = (int) ceil((double) (uData->pJobLimit * hp->numCPUs));
            num = num - highJobsHost - rsvSlots;
            *numAvailSlots = MIN (*numAvailSlots, num - lowJobsHost);
            slots = MIN(slots, num - lowNonpreemptJobsHost);
            if (numNeeded > slots) {
                jp->newReason = PEND_USER_PROC_JLIMIT;
                *numAvailSlots = 0;
                if (logclass & (LC_JLIMIT))
                    ls_syslog(LOG_DEBUG2, "%s: pJobLimit=%f slots=%d rsvSlots=%d", fname, uData->pJobLimit, slots, rsvSlots);
                return (0);
            }
        }

    }

    if (logclass & (LC_JLIMIT))
        ls_syslog(LOG_DEBUG3, "%s: job=%s, host=%s, numAvailSlots=%d, slots=%d", fname, lsb_jobid2str(jp->jobId), hp->host, *numAvailSlots, slots);

    return (slots);
}

static int
candHostOk (struct jData *jp, int indx, int *numAvailSlots,
            int *hReason)
{
    static char fname[] = "candHostOk";
    struct candHost *hp = &(jp->candPtr[indx]);
    int rtReason = 0;
    int nSlots = INFINIT_INT;
    int numBackfillSlots;
    *numAvailSlots = INFINIT_INT;

    if (logclass & (LC_SCHED | LC_PEND))
        ls_syslog(LOG_DEBUG3, "%s: numSlots1=%d numSlots2=%d indx=%d host1=%s host2=%s", fname, hp->numSlots, jp->candPtr[indx].numSlots, indx, hp->hData->host, jp->candPtr[indx].hData->host);

    if (hp->numSlots <= 0) {
        *numAvailSlots = 0;
        return 0;
    }

    INC_CNT(PROF_CNT_candHostOk);

    jp->newReason = 0;
    *hReason = 0;

    if (HOST_UNUSABLE_TO_JOB_DUE_TO_H_REASON(hReasonTb[1][hp->hData->hostId], jp)) {
        *hReason = hReasonTb[1][hp->hData->hostId];
    } else
        if (HOST_UNUSABLE_TO_JOB_DUE_TO_Q_H_REASON(jp->qPtr->reasonTb[1][hp->hData->hostId], jp)) {
            *hReason = jp->qPtr->reasonTb[1][hp->hData->hostId];
        } else
            if (HOST_UNUSABLE_TO_JOB_DUE_TO_U_H_REASON(jp->uPtr->reasonTb[1][hp->hData->hostId], jp)) {
                *hReason = jp->uPtr->reasonTb[1][hp->hData->hostId];
            }

    if ((*hReason) != 0) {
        if (logclass & (LC_SCHED | LC_PEND))
            ls_syslog (LOG_DEBUG2, "%s: Candidate <%s> not eligible any more to job <%s>; reason=%d", fname, hp->hData->host, lsb_jobid2str(jp->jobId), *hReason);
        *numAvailSlots = 0;
        return 0;
    }

    if (((jp->shared->jobBill.options & SUB_EXCLUSIVE) && (hp->hData->numJobs >= 1))
        || (hp->hData->hStatus & HOST_STAT_EXCLUSIVE)) {
        rtReason = PEND_HOST_EXCLUSIVE;
    } else

        DESTROY_BACKFILLEE_LIST(hp->backfilleeList);
    nSlots = getHostJobSlots(jp, hp->hData, numAvailSlots, FALSE,
                             &hp->backfilleeList);
    numBackfillSlots = totalBackfillSlots(hp->backfilleeList);
    hp->numNonBackfillSlots = MIN(hp->numNonBackfillSlots,
                                  nSlots - numBackfillSlots);
    hp->numAvailNonBackfillSlots = MIN(hp->numAvailNonBackfillSlots,
                                       *numAvailSlots - numBackfillSlots);
    if (nSlots == 0) {
        rtReason = jp->newReason;
    } else
        if (jp->shared->resValPtr
            && jp->shared->resValPtr->maxNumHosts == 1
            && nSlots < jp->shared->jobBill.numProcessors) {
            rtReason = PEND_JOB_NO_SPAN;
        } else
            if ((!jp->shared->resValPtr || jp->shared->resValPtr->pTile == INFINIT_INT)
                && jp->qPtr->resValPtr
                && jp->qPtr->resValPtr->maxNumHosts == 1
                && nSlots < jp->shared->jobBill.numProcessors) {
                rtReason = PEND_QUE_NO_SPAN;
            }


    if (!rtReason && jp->qPtr->resValPtr != NULL) {
        int resource;
        int num = ckResReserve (hp->hData, jp->qPtr->resValPtr,
                                &resource, jp);
        if (num < 1) {
            rtReason = resource + PEND_HOST_QUE_RUSAGE;
        } else {
            if (!jp->shared->resValPtr || jp->shared->resValPtr->maxNumHosts != 1) {
                num = MIN (num, jp->qPtr->resValPtr->pTile);
                nSlots = MIN (nSlots, num);
            }
        }
    }
    if (!rtReason && jp->shared->resValPtr != NULL) {
        int resource;
        int num = ckResReserve (hp->hData, jp->shared->resValPtr,
                                &resource, jp);
        if (num < 1) {
            rtReason = resource + PEND_HOST_JOB_RUSAGE;
        } else  {
            num = MIN (num, jp->shared->resValPtr->pTile);
            nSlots = MIN (nSlots, num);
        }
    }

    if (!rtReason && overThreshold (hp->hData->lsbLoad,
                                    jp->qPtr->loadSched, &rtReason)) {


        if (logclass & LC_SCHED)
            ls_syslog(LOG_DEBUG,"\
%s: job=%s the queue's scheduling parameters are over threshold",
                      fname, lsb_jobid2str(jp->jobId));
    }

    if (!rtReason && overThreshold (hp->hData->lsbLoad,
                                    hp->hData->loadSched, &rtReason)) {


        if (logclass & LC_SCHED)
            ls_syslog(LOG_DEBUG,"\
%s: job=%s the load of the host is over threshold",
                      fname, lsb_jobid2str(jp->jobId));

    }

    if (!rtReason && !(*hReason)) {
        if (jp->qPtr->reasonTb[1][hp->hData->hostId] ==
            PEND_HOST_ACCPT_ONE)
            *numAvailSlots = 0;

        if (debug && (logclass & LC_SCHED))
            ls_syslog(LOG_DEBUG3, "candHostOk: job=%s host=%s nSlots=%d numAvailSlots=%d", lsb_jobid2str(jp->jobId), hp->hData->host, nSlots, *numAvailSlots);
        return nSlots;
    }
    if (rtReason == PEND_HOST_JOB_LIMIT) {
        *hReason = rtReason;

        if (!HAS_BACKFILL_POLICY) {

            numLsbUsable -= hp->hData->numCPUs;
            jp->qPtr->numUsable -= hp->hData->numCPUs;
        }
    }
    else if (rtReason == PEND_QUE_PROC_JLIMIT
             || rtReason == PEND_QUE_HOST_JLIMIT
             || rtReason == PEND_HOST_QUE_RUSAGE
             || rtReason == PEND_QUE_NO_SPAN) {
        *hReason = rtReason;
        jp->qPtr->reasonTb[1][hp->hData->hostId] = rtReason;
        if (!HAS_BACKFILL_POLICY) {
            jp->qPtr->numUsable -= hp->hData->numCPUs;
        }
    }
    else {
        jp->newReason = rtReason;
    }
    *numAvailSlots = 0;
    return 0;

}


static void
moveHostPos (struct candHost *candH, int source, int target)
{
    struct candHost saveH ;
    int i;

    if (source < 0 || target < 0 || source == target)
        return;


    saveH = candH[source];

    if (source > target) {
        for (i = source-1; i >= target; i--)
            candH[i+1] = candH[i];
    }
    else  {
        for (i = source+1; i <= target; i++)
            candH[i-1] = candH[i];
    }

    candH[target] = saveH;
    return;
}


static int
allocHosts (struct jData *jp)
{
    static char         fname[] = "allocHosts";
    struct hData        **hPtr;
    int                 i,
        j,
        numh;

    if (jp->numEligProc <= 0)
        return -1;

    hPtr = (struct hData **) my_calloc (jp->numEligProc,
                                        sizeof(struct hData *), fname);


    numh = 0;
    for (i = 0; i < jp->numExecCandPtr; i++) {
        INC_CNT(PROF_CNT_firstloopallocHosts);
        for (j = 0; j < jp->execCandPtr[i].numSlots; j++) {
            INC_CNT(PROF_CNT_secondloopallocHosts);

            if (jp->execCandPtr[i].numSlots <= 0)
                continue;
            hPtr[numh] = jp->execCandPtr[i].hData;
            hPtr[numh]->numDispJobs++;
            numh++;
        }
    }

    if (numh != jp->numEligProc) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7220,
                                         "%s: numh <%d> jp->numEligProc <%d> are not equal"), /* catgets 7220 */
                  fname, numh, jp->numEligProc);
    }

    jp->hPtr = hPtr;
    jp->numHostPtr = numh;

    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG2, "%s: Allocated %d host/processors to job %s", fname, numh, lsb_jobid2str(jp->jobId));
    }

    return 0;

}

static int
deallocHosts (struct jData *jp)
{
    int i;

    for (i = 0; i < jp->numHostPtr; i++) {
        INC_CNT(PROF_CNT_loopdeallocHosts);
        jp->hPtr[i]->numDispJobs--;
    }

    FREEUP (jp->hPtr);
    jp->numHostPtr = 0;
    return 0;

}

bool_t
dispatch_it (struct jData *jp)
{
    static char fname[] = "dispatch_it";
    sbdReplyType reply;
    struct jobReply jobReply;
    struct jData   *jpbw, *jptr;
    struct jobSig jobSig;
    int i;

    if ((jpbw = getZombieJob(jp->jobId)) != NULL) {
        if (strcmp (jpbw->hPtr[0]->host, jp->hPtr[0]->host) == 0) {
            jp->newReason = PEND_SBD_ZOMBIE;
            return (FALSE);
        }
    }

    jp->dispTime = now_disp;


    TIMEIT (2, (reply = start_job(jp, jp->qPtr, &jobReply)), "start_job");

    jptr = jp;
    i = 1;

    for (; i > 0; i--) {
        jptr->dispTime = now_disp;

        switch (reply) {
            case ERR_NO_ERROR:
                jobStarted (jptr, &jobReply);
                jptr->newReason = 0;

                continue;

            case ERR_NULL:

                jobSig.sigValue = 0;
                jobSig.actFlags  = 0;
                jobSig.chkPeriod = 0;
                jobSig.actCmd = "";
                reply = signal_job (jptr, &jobSig, &jobReply);

                switch (reply) {
                    case ERR_NO_ERROR:
                        jobStarted (jptr, &jobReply);
                        jptr->newReason = 0;
                        continue;
                    case ERR_NO_JOB:
                        jptr->newReason = PEND_JOB_START_FAIL;
                        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7221,
                                                         "%s: Failed to start job <%s> on host <%s>"), /* catgets 7221 */
                                  fname,
                                  lsb_jobid2str(jptr->jobId),
                                  jptr->hPtr[0]->host);
                        return FALSE;
                    default:
                        jptr->newReason = PEND_JOB_START_UNKNWN;
                        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7222,
                                                         "%s: mbatchd does not know job <%s> is started or not on host <%s> (reply=%d) - assuming job not started"), /* catgets 7222 */
                                  fname,
                                  lsb_jobid2str(jptr->jobId),
                                  jptr->hPtr[0]->host,
                                  reply);
                        return FALSE;
                }

            default:
                jobStartError(jptr, reply);
                return FALSE;
        }
    }

    return TRUE;
}

int
jobStartError(struct jData *jData, sbdReplyType reply)
{
    static char fname[] = "jobStartError";
    char *toHost = jData->hPtr[0]->host;
    int newReason = 0;


    switch (reply) {
        case ERR_MEM:
            jData->newReason = PEND_SBD_NO_MEM;
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7223,
                                             "%s: Not enough memory on host <%s>; job <%s> rejected"), /* catgets 7223 */
                      fname, toHost, lsb_jobid2str(jData->jobId));
            break;

        case ERR_FORK_FAIL:
            jData->newReason = PEND_SBD_NO_PROCESS;
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7224,
                                             "%s: sbatchd on host <%s> was unable to fork; job <%s> rejected"), /* catgets 7224 */
                      fname, toHost, lsb_jobid2str(jData->jobId));
            break;

        case ERR_PID_FAIL:
            jData->newReason = PEND_SBD_GETPID;
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7225,
                                             "%s: Failed to get pid on host <%s> for job <%s>"), /* catgets 7225 */
                      fname, toHost, lsb_jobid2str(jData->jobId));
            break;

        case ERR_ROOT_JOB:
            jData->newReason = PEND_SBD_ROOT;
            ls_syslog(LOG_CRIT,I18N(7241,
                                    "%s: Root user's job <%s> was rejected by sbatchd on host <%s>"),  /*catgets 7241 */
                      fname, lsb_jobid2str(jData->jobId), toHost);
            break;

        case ERR_LOCK_FAIL:
            jData->newReason = PEND_SBD_LOCK;
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7226,
                                             "%s: Locking host failed on host <%s> for job <%s>"), fname, toHost, lsb_jobid2str(jData->jobId));           /* catgets 7226 */
            break;

        case ERR_JOB_QUOTA:
            jData->newReason = PEND_SBD_JOB_QUOTA;
            ls_syslog(LOG_DEBUG, "%s: The number of dispatched jobs has reached quota; job <%s> rejected by sbatchd on host <%s>", fname, lsb_jobid2str(jData->jobId), toHost);
            break;

        case ERR_NO_USER:
            jData->newReason = PEND_HOST_NO_USER;
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7227,
                                             "%s: User <%s/%d> not recognizable by sbatchd on host <%s>; job <%s> rejected"), fname, jData->userName, jData->userId, toHost, lsb_jobid2str(jData->jobId));                /* catgets 7227 */
            break;

        case ERR_NO_FILE:
            jData->newReason = PEND_JOB_NO_FILE;
            break;

        case ERR_BAD_REQ:
            jData->newReason = PEND_JOB_START_FAIL;
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7228,
                                             "%s: sbatchd on host <%s> complained of bad request; job <%s> rejected"), fname, toHost, lsb_jobid2str(jData->jobId)); /* catgets 7228 */
            break;

        case ERR_UNREACH_SBD:
        case ERR_FAIL:

            jData->newReason = PEND_JOB_START_FAIL;
            break;

        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7229,
                                             "%s: Unknown reply code %d"), fname, reply); /* catgets 7229 */
            jData->newReason = PEND_JOB_START_FAIL;
    }

    newReason = jData->newReason;
    if ((jData->newReason != PEND_JOB_NO_FILE)
        && (jData->newReason != PEND_HOST_NO_USER)
        && (jData->newReason != PEND_SBD_LOCK)
        && (jData->newReason != PEND_SBD_ROOT)) {

        int hostId = jData->hPtr[0]->hostId;
        hReasonTb[1][hostId] = jData->newReason;

        jData->newReason = 0;
        jData->qPtr->numUsable -= jData->hPtr[0]->numCPUs;
        numLsbUsable -= jData->hPtr[0]->numCPUs;
    }
    return (newReason);

}

static void
jobStarted (struct jData *jp, struct jobReply *jobReply)
{
    int i;
    int hostAcceptJobTime;

    if (IS_FINISH(jp->jStatus)
        || IS_FINISH(jobReply->jStatus))
        return;

    jp->newReason = 0;
    jp->oldReason = 0;
    jp->numReasons = 0;


    hostAcceptJobTime = time(NULL);

    for (i = 0; i < jp->numHostPtr; i++)
        jp->hPtr[i]->acceptTime = hostAcceptJobTime;

    FREEUP (jp->reasonTb);

    jp->dispCount ++;
    jp->jobPid = jobReply->jobPid;
    jp->jobPGid = jobReply->jobPGid;


    if (jp->shared->jobBill.options & SUB_EXCLUSIVE) {
        for (i = 0; i < jp->numHostPtr; i ++)
            jp->hPtr[i]->hStatus |= HOST_STAT_EXCLUSIVE;
    }


    if (jp->shared->jobBill.options & SUB_PRE_EXEC)
        jp->jStatus |= JOB_STAT_PRE_EXEC;

    jStatusChange (jp, JOB_STAT_RUN, LOG_IT, "jobStarted");
    adjLsbLoad (jp, FALSE, TRUE);

    INC_CNT(PROF_CNT_numStartedJobsPerSession);

}

void
disp_clean_job(struct jData *jpbw)
{

    jpbw->jFlags &= ~JFLAG_READY;
    jpbw->processed = 0;
    jpbw->numSlots = 0;
    jpbw->numAvailSlots = 0;
    jpbw->numEligProc = 0;
    jpbw->numAvailEligProc = 0;
    jpbw->oldReason = jpbw->newReason;

    if (jpbw->numCandPtr == 0 && jpbw->groupCands == NULL)
        return;

    FREE_CAND_PTR(jpbw);

    jpbw->numCandPtr = 0;
    jpbw->usePeerCand = FALSE;

    FREE_ALL_GRPS_CAND(jpbw);
}

static void
disp_clean(void)
{
    static char     fname[] = "disp_clean";
    struct jData    *jpbw;
    int             list;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG2, "%s: Entering this routine...", fname);

    mSchedStage = 0;
    freedSomeReserveSlot = FALSE;

    for (list = 0; list < NJLIST; list++) {
        for (jpbw = jDataList[list]->back; jpbw != jDataList[list];
             jpbw = jpbw->back) {
            disp_clean_job(jpbw);
        }

    }

    return;
}

static void
hostPreference (struct jData *jp, int nHosts)
{
    static char fname[] = "hostPreference";
    int pref = FALSE, i;

    if (jp->usePeerCand)
        return;

    INC_CNT(PROF_CNT_hostPreference);

    if (logclass & (LC_TRACE | LC_SCHED)) {
        for (i = 0; i < nHosts; i++)
            ls_syslog(LOG_DEBUG3, "%s: before hostPreference1: jp->candPtr[%d]=%s for job <%s>", fname, i, jp->candPtr[i].hData->host, lsb_jobid2str(jp->jobId));
    }

    if (jp->numAskedPtr)
        hostPreference1 (jp, nHosts, jp->askedPtr, jp->numAskedPtr,
                         jp->askedOthPrio, &pref, TRUE);

    if (logclass & (LC_TRACE | LC_SCHED)) {
        for (i = 0; i < nHosts; i++)
            ls_syslog(LOG_DEBUG3, "%s: after hostPreference1: jp->candPtr[%d]=%s for job <%s>", fname, i, jp->candPtr[i].hData->host, lsb_jobid2str(jp->jobId));
    }

    if (pref)
        return;

    if (jp->qPtr->numAskedPtr)
        hostPreference1 (jp, nHosts, jp->qPtr->askedPtr,
                         jp->qPtr->numAskedPtr, jp->qPtr->askedOthPrio, &pref, FALSE);

    return;
}

static void
hostPreference1 (struct jData *jp, int nHosts, struct askedHost *askedPtr,
                 int numAskedPtr, int askedOthPrio, int *pref, int jobPref)
{
    static char fname[] = "hostPreference1";
    int i, j, k;
    struct candHost *tmpCandPtr;
    int *flags;

#define NEED_COPY         0x1
#define COPY_DONE         0x2

#define CLEAN_AND_RETURN                        \
    {                                           \
        FREE_CAND_PTR(jp);                      \
        jp->candPtr = tmpCandPtr;               \
        jp->numCandPtr =  k;                    \
        free(flags);                            \
        return;                                 \
    }

    if (noPreference (askedPtr, numAskedPtr, askedOthPrio) == TRUE) {
        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG3, "%s: No host preference defined for job <%s>", fname, lsb_jobid2str(jp->jobId));
        }
        return;
    }

    tmpCandPtr = (struct candHost *) my_calloc (jp->numCandPtr,
                                                sizeof (struct candHost), fname);
    flags = (int *)my_calloc(jp->numCandPtr, sizeof(int), fname);
    k = 0;


    for (i = 0; i < numAskedPtr; i++) {
        if (askedPtr[i].priority < 1)  {

            break;
        }
        *pref = TRUE;
        if (askedPtr[i].priority <= askedOthPrio) {
            break;
        }

        for (j = 0; j < nHosts; j++) {
            if (askedPtr[i].hData != jp->candPtr[j].hData)
                continue;

            flags[j] |= NEED_COPY;
            break;
        }

        copyCandHosts(i, askedPtr, tmpCandPtr, &k, jp, nHosts, numAskedPtr,
                      flags);
    }


    if (askedOthPrio < 1) {
        for (i = 0; i < nHosts; i++) {
            if (flags[i] & COPY_DONE) {
                continue;
            }

            copyCandHostData(&tmpCandPtr[k], &jp->candPtr[i]);

            k++;
        }
        CLEAN_AND_RETURN;
    }



    for (i = 0; i < numAskedPtr; i++) {

        if (askedPtr[i].priority != askedOthPrio)
            continue;
        for (j = 0; j < nHosts; j++) {
            if (askedPtr[i].hData != jp->candPtr[j].hData)
                continue;
            flags[j] |= NEED_COPY;
            break;
        }
    }
    for (j = 0; j < nHosts; j++) {
        if (flags[j] & COPY_DONE)
            continue;
        if ((flags[j] & NEED_COPY)
            || (isAskedHost (jp->candPtr[j].hData, jp) == FALSE
                && jp->askedOthPrio >= 0 && jobPref == TRUE)
            || (isQAskedHost (jp->candPtr[j].hData, jp) == FALSE
                && jp->qPtr->askedOthPrio >= 0 && jobPref == FALSE)) {


            copyCandHostData(&tmpCandPtr[k], &jp->candPtr[j]);

            flags[j] &= ~NEED_COPY;
            flags[j] |= COPY_DONE;
            k++;
        }
    }


    for (i = 0; i < numAskedPtr; i++) {
        if (askedPtr[i].priority >= askedOthPrio
            || isInCandList (tmpCandPtr, askedPtr[i].hData, k))
            continue;

        for (j = 0; j < nHosts; j++) {
            if (askedPtr[i].hData != jp->candPtr[j].hData)
                continue;
            flags[j] |= NEED_COPY;
            break;
        }
        copyCandHosts(i, askedPtr, tmpCandPtr, &k, jp, nHosts, numAskedPtr,
                      flags);
    }
    CLEAN_AND_RETURN;
}

static int
isInCandList (struct candHost *candHost, struct hData *hData, int num)
{
    int i;

    if (num <= 0)
        return FALSE;
    for (i = 0; i < num; i++) {
        if (candHost[i].hData == hData)
            return TRUE;
    }
    return FALSE;
}
static int
noPreference (struct askedHost *askedPtr, int numAskedPtr, int askedOthPrio)
{
    int i, priority = -1;

    for (i = 0; i < numAskedPtr; i++) {
        if (priority < 0)
            priority = askedPtr[i].priority;
        else if (priority != askedPtr[i].priority)
            return FALSE;
    }
    if (askedOthPrio >= 0 && priority >= 0 && priority != askedOthPrio)
        return FALSE;

    return TRUE;

}

static void
copyCandHosts (int i, struct askedHost *askedPtr, struct candHost *tmpCandPtr,
               int *k, struct jData *jp, int nHosts, int numAskedPtr,
               int *flags)
{
    int j;

    if (i == numAskedPtr - 1 ||
        askedPtr[i].priority != askedPtr[i+1].priority) {

        for (j = 0; j < nHosts; j++) {
            if (!(flags[j] & NEED_COPY))
                continue;

            copyCandHostData(&tmpCandPtr[*k], &jp->candPtr[j]);

            flags[j] &= ~NEED_COPY;
            flags[j] |= COPY_DONE;
            (*k)++;
        }
    }

}

static void
copyCandHostData(struct candHost* dst, struct candHost* source)
{
    static char fname[] = "copyCandHostData";
    LIST_T*     list;

    *dst = *source;

    if (source->backfilleeList != NULL) {

        list = listDup(source->backfilleeList,
                       sizeof(struct backfillee));
        if (list == NULL) {
            ls_syslog(LOG_ERR,I18N(7242,"\
%s: Duplicating backfillee list failed:%s"),  /* catgets 7242 */
                      fname, listStrError(listerrno));
            mbdDie(MASTER_FATAL);
        }

        dst->backfilleeList = list;

    } else {
        dst->backfilleeList = NULL;
    }
}

int
findBestHosts (struct jData *jp, struct resVal *resValPtr, int needed,
               int ncandidates, struct candHost *hosts, bool_t orderForPreempt)
{
    int i, numHosts, last =FALSE;
    struct resVal defResVal, *resVal;
    float threshold;

    static char fname[]="findBestHosts";

    if (logclass & (LC_EXEC))
        ls_syslog(LOG_DEBUG,"%s: the number of candidates for sort is %d",
                  fname, ncandidates);

    INC_CNT(PROF_CNT_findBestHost);

    if (resValPtr == NULL) {

        defResVal.nphase = 2;
        defResVal.order[0] = R15S + 1;
        defResVal.order[1] = PG + 1;
        resVal = &defResVal;
    } else
        resVal = resValPtr;


    if (logclass & (LC_EXEC)) {
        ls_syslog(LOG_DEBUG,"%s: the resVal->nphase's value is %d",
                  fname, resVal->nphase);
        ls_syslog(LOG_DEBUG,"%s: the value of needed is %d", fname, needed);
        ls_syslog(LOG_DEBUG,"%s: the value of ncandidates is %d", fname, ncandidates );
    }
    for (i = resVal->nphase - 1; i >= 0; i--) {
        if( i == 0)
            last = TRUE;
        if (abs (resVal->order[i]) - 1 < NBUILTINDEX)
            threshold = bThresholds[abs(resVal->order[i]) - 1];
        else
            threshold = 0.0001;

        if (logclass & (LC_EXEC))
            ls_syslog(LOG_DEBUG,"%s: the current sort order is %d",
                      fname, resVal->order[i]);
        if ((resVal->order[0] == R15S + 1) && scheRawLoad)
            getRawLsbLoad(ncandidates, hosts);

        numHosts = sortHosts(resVal->order[i], needed, ncandidates,
                             hosts, last, threshold, orderForPreempt);
        if (numHosts == needed && i > 1)
            i = 1;

    }

    return (numHosts);

}

static void
getRawLsbLoad(int ncandidates, struct candHost *hosts)
{
    static char fname[] = "getRawLsbLoad";
    int i,  num;
    char **hostNames;
    struct hData *hDataPtr;
    struct hostLoad *newHostLoad;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Enter this rountine ...", fname);


    hostNames = (char **) my_malloc(ncandidates * sizeof (char *), fname);
    for (i = 0; i < ncandidates; i++) {
        hostNames[i] = hosts[i].hData->host;
    }

    newHostLoad = ls_loadofhosts ("-:server", &num, 0, NULL, hostNames, ncandidates);
    FREEUP(hostNames);
    if (newHostLoad != NULL) {
        for (i = 0; i < num; i++) {
            if ((hDataPtr = getHostData (newHostLoad[i].hostName)) != NULL) {
                hDataPtr->lsbLoad[R15S] = newHostLoad[i].li[R15S];

                if (logclass & LC_TRACE)
                    ls_syslog(LOG_DEBUG, "%s: host %s R15S raw load is %f", fname, hDataPtr->host, hDataPtr->lsbLoad[R15S]);
            }
        }
    }

}


static int
notOrdered(int increasing, int lidx,
           float load1, float load2,
           float cpuf1, float cpuf2)
{
    float normal1, normal2;

    if ((lidx == R15S) || (lidx == R1M) || (lidx == R15M)) {
        normal1 = (cpuf1 != 0)?(load1 + 1)/cpuf1:load1;
        normal2 = (cpuf2 != 0)?(load2 + 1)/cpuf2:load2;
    }
    else {
        normal1 = load1;
        normal2 = load2;
    }

    if (increasing) {
        return (normal1 > normal2);
    }
    else {
        return (normal1 < normal2);
    }
}

static float
getNumericLoadValue(const struct hData *hp, int lidx)
{
    int i;
    static char fname[]="getNumericLoadValue()";


    if (NOT_NUMERIC(allLsInfo->resTable[lidx])) {
        ls_syslog(LOG_ERR, I18N(7243,"%s, instance is not of numeric type."), fname); /*catgets 7243*/
        return (-INFINIT_LOAD);
    }

    if ( !(allLsInfo->resTable[lidx].flags & RESF_SHARED) && (lidx < allLsInfo->numIndx)) {
        return hp->lsbLoad[lidx];
    }

    if ((allLsInfo->resTable[lidx].flags & RESF_BATCH)
        && strcmp(allLsInfo->resTable[lidx].name,"slots") == 0) {
        return (hp->maxJobs - hp->numJobs);
    }

    for (i = 0; i < hp->numInstances; i++) {
        if ( !(strcmp(hp->instances[i]->resName,
                      allLsInfo->resTable[lidx].name))) {
            if (strcmp(hp->instances[i]->value, "-") == 0) {
                return (INFINIT_LOAD);
            }
            return (atof (hp->instances[i]->value));
        }
    }

    ls_syslog(LOG_ERR, I18N(7244,"%s, instance name not found."), fname);/* catgets 7244 */
    return (-INFINIT_LOAD);
}

static int
sortHosts (int lidx, int numHosts, int ncandidates, struct candHost *hosts,
           int lastSort, float threshold, bool_t orderForPreempt)
{
    char swap;
    int i, j;
    char incr;
    struct  candHost tmp;
    int cutoffs, shrink;
    int order, residual;
    char flip;
    float exld1, exld2;

    static char fname[]="sortHosts()";

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG, "%s, Entering this routine ...", fname);

    if (lidx < 0)
        flip = TRUE;
    else
        flip = FALSE;
    lidx = abs(lidx) -1;

    if ( lidx == R15S || lidx == R1M || lidx == R15M || lidx == LS)
        shrink = 5;
    else
        shrink = 8;
    if (lastSort != TRUE) {
        residual = ncandidates - numHosts;
        if (residual <= 1) {
            if (numHosts <= SORT_HOST_NUM)
                cutoffs = 0 ;
            else
                return ncandidates;
        } else
            cutoffs = (residual - 1)/shrink + 1;
    } else {
        if (ncandidates >= numHosts)
            cutoffs = numHosts;
        else
            cutoffs = ncandidates;
    }

    if (allLsInfo->resTable[lidx].orderType == INCR)
        incr = TRUE;
    else
        incr = FALSE;

    if (flip)
        incr = !incr;

    if (lastSort == FALSE) {
        float bestload = getNumericLoadValue(hosts[0].hData, lidx);
        float bestcpuf = hosts[0].hData->cpuFactor;

        for (i=1; i<ncandidates; i++) {
            if (notOrdered(incr, lidx, bestload, getNumericLoadValue(hosts[0].hData, lidx),
                           bestcpuf,hosts[0].hData->cpuFactor))
                bestload = getNumericLoadValue(hosts[0].hData, lidx);
            bestcpuf = hosts[0].hData->cpuFactor;
        }


        swap = TRUE;
        i = 0;
        while (swap && (i < ncandidates-cutoffs)) {
            swap = FALSE;
            for (j = ncandidates-2; j>= i; j--) {
                order = orderByStatus(hosts, j+1, orderForPreempt);
                if (order == 0) {
                    swap = TRUE;
                    continue;
                } else if (order == 1)
                    continue;


                exld1 = getNumericLoadValue(hosts[j].hData, lidx) * 0.05;
                if (allLsInfo->resTable[lidx].orderType == DECR) {
                    exld1 = -exld1;
                }

                exld2 = getNumericLoadValue(hosts[j + 1].hData, lidx) * 0.05;
                if (allLsInfo->resTable[lidx].orderType == DECR) {
                    exld2 = -exld2;
                }

                if (notOrdered(incr, lidx,
                               getNumericLoadValue(hosts[j].hData, lidx) + exld1,
                               getNumericLoadValue(hosts[j + 1].hData, lidx) + exld2,
                               hosts[j].hData->cpuFactor,
                               hosts[j+1].hData->cpuFactor)) {
                    swap = TRUE;
                    tmp = hosts[j];
                    hosts[j] = hosts[j+1];
                    hosts[j+1] = tmp;
                }
            }
            i++;
        }
        for (i = ncandidates-cutoffs; i < ncandidates; i++)
            if (fabs(getNumericLoadValue(hosts[i].hData, lidx) - bestload)
                >= threshold)
                return i;

        return (ncandidates);
    }


    if (logclass & (LC_EXEC)) {
        ls_syslog(LOG_DEBUG3, "%s, ncandidates = %d, cutoffs = %d ", fname,
                  ncandidates, cutoffs);
    }
    swap = TRUE;
    i = 0;
    while (swap && (i < cutoffs)) {
        swap = FALSE;
        for (j = ncandidates-2; j >= i; j--) {
            order = orderByStatus(hosts, j+1, orderForPreempt);
            if (order == 0) {
                swap = TRUE;
                continue;
            } else if (order == 1)
                continue;


            exld1 = getNumericLoadValue(hosts[j].hData, lidx) * 0.05;
            if (allLsInfo->resTable[lidx].orderType == DECR) {
                exld1 = -exld1;
            }

            exld2 = getNumericLoadValue(hosts[j + 1].hData, lidx) * 0.05;
            if (allLsInfo->resTable[lidx].orderType == DECR) {
                exld2 = -exld2;
            }

            if (notOrdered(incr, lidx,
                           getNumericLoadValue(hosts[j].hData, lidx),
                           getNumericLoadValue(hosts[j + 1].hData, lidx),
                           hosts[j].hData->cpuFactor,
                           hosts[j+1].hData->cpuFactor)) {
                swap = TRUE;
                tmp = hosts[j];
                hosts[j] = hosts[j+1];
                hosts[j+1] = tmp;
            }
        }
        i++;
    }

    if (logclass & (LC_EXEC)) {
        for (i=0; i < ncandidates; i++)
            ls_syslog(LOG_DEBUG2, "%s, host[%d]'s name is %s", fname, i, hosts
                      [i].hData->host);
    }

    return (cutoffs);

}

int
orderByStatus (struct candHost *hosts, int j, bool_t orderByClosedFull)
{
    int status1, status2;
    struct candHost tmp;

    status1 = hosts[j-1].hData->hStatus;
    status2 = hosts[j].hData->hStatus;

    if ((LSB_HOST_OK(status2) && !LSB_HOST_OK(status1))
        || (LSB_HOST_BUSY(status2) && !LSB_HOST_OK(status1)
            && !LSB_HOST_BUSY(status1))
        || (LSB_HOST_CLOSED(status2) && !LSB_HOST_OK(status1)
            && !LSB_HOST_BUSY(status1) && !LSB_HOST_CLOSED(status1))
        || (LSB_HOST_UNREACH(status2) && !LSB_HOST_OK(status1)
            && !LSB_HOST_BUSY(status1) && !LSB_HOST_CLOSED(status1)
            && !LSB_HOST_UNREACH(status1))
        || (LSB_HOST_UNAVAIL(status2) && !LSB_HOST_OK(status1)
            && !LSB_HOST_BUSY(status1) && !LSB_HOST_CLOSED(status1)
            && !LSB_HOST_UNREACH(status1) && !LSB_HOST_UNAVAIL(status1)))  {
        tmp = hosts[j];
        hosts[j] = hosts[j-1];
        hosts[j-1] = tmp;
        return (0);
    }


    if (LSB_HOST_OK(status2) && LSB_HOST_OK(status1))
        return (2);

    if (orderByClosedFull && LSB_HOST_FULL(status2)
        && LSB_HOST_FULL(status1)) {
        return (2);
    }

    return (1);

}


static bool_t
isCandHost (char *hostname, struct jData *jp)
{
    static char fname[] = "isCandHost";
    int i;

    for (i = 0; i < jp->numCandPtr; i++) {
        if (strcmp(hostname, jp->candPtr[i].hData->host) == 0) {

            if (logclass & (LC_SCHED | LC_TRACE))
                ls_syslog(LOG_DEBUG, "%s: host <%s> is a candidate host",
                          fname,hostname);
            return (TRUE);
        }
    }
    return (FALSE);
}

static void
reserveSlots (struct jData *jp)
{
    static char fname[] = "reserveSlots";
    int i;

    if (jp->numHostPtr < 0)
        return;

    if (jp->jStatus & JOB_STAT_RESERVE) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7230,
                                         "%s: Job <%s> already has slots reserved before"),/* catgets 7230 */
                  fname, lsb_jobid2str(jp->jobId));
        return;
    }

    if (jp->reserveTime == 0) {
        jp->reserveTime = now;
        if (jp->qPtr->slotHoldTime <= 0 ) {
            jp->slotHoldTime = 8 * msleeptime;
        }
    }

    updResCounters(jp, jp->jStatus | JOB_STAT_RESERVE);
    jp->jStatus |= JOB_STAT_RESERVE;


    for (i = 0; i < jp->numHostPtr; i++) {
        if (!HAS_BACKFILL_POLICY ) {
            jp->qPtr->numUsable--;
        }
        if (logclass & (LC_SCHED))
            ls_syslog (LOG_DEBUG2, "%s: Reserve host %s for job %s",
                       fname, jp->hPtr[i]->host, lsb_jobid2str(jp->jobId));
    }

}

static int
cntUserSlots(struct hTab *uAcct, struct uData *up, int *runJobSlots)
{
    struct userAcct *ap;

    if ((ap = getUAcct(uAcct, up)) != NULL) {
        *runJobSlots = ap->numRUN;
        return (ap->numRESERVE);
    }
    *runJobSlots = 0;
    return (0);

}

void
freeReserveSlots(struct jData *jpbw)
{
    if (!(jpbw->jStatus & JOB_STAT_RESERVE))
        return;

    updResCounters(jpbw, (jpbw->jStatus & ~JOB_STAT_RESERVE));
    jpbw->jStatus &= ~JOB_STAT_RESERVE;

    deallocHosts(jpbw);

}

static void
checkSlotReserve(struct jData **jobp, int *continueSched)
{
    static char fname[] = "checkSlotReserve";
    struct jData *jpbw;

    jpbw = *jobp;

    INC_CNT(PROF_CNT_checkSlotReserve);

    if (logclass & LC_SCHED)
        ls_syslog(LOG_DEBUG3, "%s: Entering this routine...", fname);

    *continueSched = TRUE;
    if (!(jpbw->jStatus & JOB_STAT_RESERVE)) {
        jpbw->reserveTime = 0;
        jpbw->slotHoldTime = 0;
        return;
    }

    if ((jpbw->qPtr->slotHoldTime > 0 && jpbw->reserveTime > 0
         && now - jpbw->reserveTime > jpbw->qPtr->slotHoldTime)
        || (jpbw->slotHoldTime > 0 && jpbw->reserveTime > 0
            && now - jpbw->reserveTime > jpbw->slotHoldTime)) {

        if (logclass & (LC_SCHED))
            ls_syslog (LOG_DEBUG3, "\
%s: Reserve time is expired; job <%s>, now <%d>, \
begin reservation time <%d>, queue's slotHoldTime<%d>, jFlags<%x>",
                       fname, lsb_jobid2str(jpbw->jobId),
                       (int)now, (int)jpbw->reserveTime,
                       jpbw->qPtr->slotHoldTime,
                       jpbw->jFlags);

        freeReserveSlots(jpbw);
        jpbw->reserveTime = 0;
        jpbw->slotHoldTime = 0;

        jpbw->processed |= JOB_STAGE_READY;
        *continueSched = FALSE;
        freedSomeReserveSlot = TRUE;

        return;

    }

    freeReserveSlots(jpbw);
    freedSomeReserveSlot = TRUE;
}


static int
compareFunc (const void *element1, const void *element2)
{
    struct leftTimeTable *job1, *job2;

    job1 = (struct leftTimeTable *) element1;
    job2 = (struct leftTimeTable *) element2;
    return (job1->leftTime - job2->leftTime);
}


static void
jobStartTime (struct jData *jp)
{
    static char fname[] = "jobStartTime";
    int i, num;
    struct hData *hPtr;
    struct jData *jpbw;
    int needed, eligible;
    int tableSize, totalRunJobs = 0;
    struct leftTimeTable *jobTable;

    if (logclass & LC_SCHED)
        ls_syslog(LOG_DEBUG3, "%s: Determine the start time for job %s",
                  fname, lsb_jobid2str(jp->jobId));

    jp->predictedStartTime = 0;
    needed = jp->shared->jobBill.numProcessors - jp->numEligProc;
    if (needed <= 0)
        return;


    tableSize = 5 * needed;
    eligible = 0;
    jobTable  = my_calloc (tableSize,
                           sizeof(struct leftTimeTable), fname);

    i = 0;
    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        if (!isCandHost(hPtr->host, jp))
            continue;

        num = hPtr->numJobs - hPtr->numRESERVE;
        for (jpbw = jDataList[SJL]->back; num > 0 && jpbw != jDataList[SJL];
             jpbw = jpbw->back) {
            int k, numJobs;
            float runLimit;

            numJobs = 0;
            for (k = 0; k < jpbw->numHostPtr; k++) {
                if (hPtr == jpbw->hPtr[k])
                    numJobs++;
            }

            if (numJobs == 0)
                continue;

            if (totalRunJobs == tableSize) {

                tableSize *= 2;
                jobTable = realloc(jobTable, tableSize * sizeof(struct leftTimeTable));
            }
            num--;
            if ((runLimit = RUN_LIMIT_OF_JOB(jpbw)) <= 0)
                continue;
            eligible += numJobs;

            runLimit = RUN_LIMIT_OF_JOB(jpbw);
            runLimit = runLimit/hPtr->cpuFactor;

            jobTable[totalRunJobs].leftTime = (int)(runLimit) - jpbw->runTime;
            jobTable[totalRunJobs].slots = numJobs;

            if (jobTable[totalRunJobs].leftTime < 0) {

                ls_syslog(LOG_DEBUG, "%s: job <%s> left runtime < 0", fname,
                          lsb_jobid2str(jpbw->jobId));
            }
            totalRunJobs++;

            if (logclass & LC_SCHED)
                ls_syslog(LOG_DEBUG, "%s: job=%s, runLimit=%f, runTime=%d, leftTime=%d, slots can be released=%d, needed=%d, totalRunJobs is %d", fname, lsb_jobid2str(jpbw->jobId), runLimit, jpbw->runTime, jobTable[totalRunJobs-1].leftTime, jpbw->numHostPtr, needed, totalRunJobs - 1);

        }
    }

    if (eligible == 0) {

        FREEUP (jobTable);
        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG1, "%s: can't find enough jobs whose finish time can be determined and whose slots can be used later by job <%s>", fname, lsb_jobid2str(jp->jobId));
        }
        return;
    } else {
        eligible = 0;
    }

    qsort((struct leftTimeTable *)jobTable, totalRunJobs,
          sizeof(struct leftTimeTable), compareFunc);

    for (i = 0; i < totalRunJobs; i++) {
        eligible += jobTable[i].slots;
        if (eligible >= needed) {
            jp->predictedStartTime = now_disp + (time_t) jobTable[i].leftTime;

            if (logclass & LC_SCHED)
                ls_syslog(LOG_DEBUG, "%s: Job %s needs to wait for %d seconds and will start at %s", fname, lsb_jobid2str(jp->jobId), jobTable[i].leftTime, ctime(&jp->predictedStartTime));
            break;
        }
    }

    if ((jp->predictedStartTime == 0)
        && (eligible > 0)) {

        jp->predictedStartTime = now_disp + (time_t) jobTable[i-1].leftTime;

        if (logclass & LC_SCHED)
            ls_syslog(LOG_DEBUG, "%s: Set predictedStartTime even there are no enough slots for job <%s> now", fname, lsb_jobid2str(jp->jobId));
    }

    FREEUP (jobTable);
    return;

    }

static int
isAskedHost (struct hData *hPtr, struct jData *jp)
{
    int j;

    for (j = 0; j < jp->numAskedPtr; j++)
        if (hPtr == jp->askedPtr[j].hData)
            return TRUE;

    return FALSE;
}

static int
isQAskedHost (struct hData *hData, struct jData *jp)
{
    int j;

    for (j = 0; j < jp->qPtr->numAskedPtr; j++)
        if (hData == jp->qPtr->askedPtr[j].hData)
            return TRUE;
    return FALSE;
}
static int
cntHostSlots (struct hTab *hAcct, struct hData *hp)
{
    struct hostAcct *ap;

    if (hAcct == NULL)
        return 0;
    if ((ap = getHAcct(hAcct, hp)) != NULL) {
        return (ap->numRESERVE);
    }
    return (0);

}

#define STAY_TOO_LONG (time(0) - now_disp >= maxSchedStay)

int
scheduleAndDispatchJobs(void)
{
    static  char            fname[] = "scheduleAndDispatchJobs";
    static  struct qData    *nextSchedQ;
    struct  qData           *qp;
    static  time_t          lastUpdTime = 0;
    static  time_t          lastSharedResourceUpdateTime;
    static  struct timeval  scheduleStartTime;
    static  struct timeval  scheduleFinishTime;
    static int             numQUsable = 0;
    int                    i;
    int                    loopCount;
    float                    tmpVal;
    enum dispatchAJobReturnCode dispRet;
    int                    continueSched;
    int                    scheduleTime;
    sTab                   hashSearchPtr;
    hEnt                   *hashEntryPtr;
    struct jRef *jR;
    struct jRef *jR0;
    struct jData *jPtr;
    struct jData *jPtr0;
    struct fsQueueJobSet * fsJobSet0 = NULL;
    struct shareAcct     * attachedShareAcct = NULL;
    int min;
    float qTimeFSElectJob;

    now_disp = time(NULL);
    ZERO_OUT_TIMERS();

    if (jRefList == NULL)
        jRefList = listCreate("job reference list");

    if (mSchedStage == 0) {

        nextSchedQ = qDataList->back;
        freedSomeReserveSlot = FALSE;
        updateAccountsInQueue = TRUE;

        hashEntryPtr = h_firstEnt_(&uDataList, &hashSearchPtr);
        while (hashEntryPtr) {
            struct uData *up = (struct uData *) hashEntryPtr->hData;

            hashEntryPtr = h_nextEnt_(&hashSearchPtr);
            for (i = 0; i <= numofhosts(); i++) {
                if (!OUT_SCHED_RS(up->reasonTb[1][i])) {
                    up->reasonTb[1][i] = 0;
                }
            }
        }

        for (i = MJL; i <= PJL; i++) {

            for (jPtr = jDataList[i]->back;
                 jPtr != jDataList[i];
                 jPtr = jPtr->back) {

                checkSlotReserve(&jPtr, &continueSched);
                if (continueSched == FALSE) {

                    jPtr->processed |= JOB_STAGE_DONE;
                    if (logclass & LC_SCHED) {
                        ls_syslog(LOG_DEBUG2, "\
%s: free reserved slots from job <%s>", fname, lsb_jobid2str(jPtr->jobId));
                    }
                    continue;
                }
                /* The purpose of the pending job reference
                 * list is to make sure that each pending job
                 * is looked at by the scheduler only once.
                 * In the jRef list, there are 2 kinds elements:
                 * 1. single job
                 * 2. job set : represent the job segment which
                 *    belong to a fairshare queue. In the job set,
                 *    jobs are organized by users.
                 * e.g.
                 * 
                 * 
                 * 
                 * jRef-->jRef-->jRef--jRef-->jRef-->jRef-->jRef
                 *        |_type=TYPE_JOB_SET         |_type=TYPE_JOB_SINGLE
                 *        |_job=fsQueueJobSet         |_job=jData
                 *                |_fairsharePolicy
                 *                |_usersTab                                    
                 *                    |_userA: jobFSRef->jobFSRef->jobFSRef
                 *                    |                    |_jData
                 *                    |                    |_idx
                 *                    |_userB: jobFSRef->jobFSRef->jobFSRef
                 *                                         |_jData
                 *                                         |_idx
                 * 
                 */
                if (! IS_FS_JOB(jPtr)) { /*not fs job*/
                    jR = my_calloc(1, sizeof(struct jRef), fname);
                    jR->job = jPtr;
                    jR->type = TYPE_JOB_SINGLE;
                    fsJobSet0 = NULL;

                    listInsertEntryAtFront(jRefList,
                                            (struct _listEntry *)jR);
                } else { /*fs job*/
                    if (fsJobSet0 == NULL) { /*create a new job set*/
                        jR = my_calloc(1, sizeof(struct jRef), fname);

                        fsJobSet0 = sch_fs_newJobSet(jPtr->qPtr->policy);
                        (*fsJobSet0->add)(fsJobSet0, (void *)jPtr);

                        if (fsJobSet0) {
                            jR->job = fsJobSet0;
                            jR->type = TYPE_JOB_SET;
                        } else {
                            jR->job = jPtr;
                            jR->type = TYPE_JOB_SINGLE;
                        }
                        listInsertEntryAtFront(jRefList,
                                                (struct _listEntry *)jR);
                    } else { /*exist a job set*/
                        /*Try to check whether belong to the same job segment*/
                        if (prevJobOnSegment(jPtr, jDataList[i]) == NULL) { /*not belong to the same job segment*/
                            jR = my_calloc(1, sizeof(struct jRef), fname);
                            fsJobSet0 = sch_fs_newJobSet(jPtr->qPtr->policy);
                            (*fsJobSet0->add)(fsJobSet0, (void *)jPtr);

                            if (fsJobSet0) {
                                jR->job = fsJobSet0;
                                jR->type = TYPE_JOB_SET;
                            } else {
                                jR->job = jPtr;
                                jR->type = TYPE_JOB_SINGLE;
                            }
                            listInsertEntryAtFront(jRefList,
                                                    (struct _listEntry *)jR);

                        } else { /*belong to the same job segment*/
                            (*fsJobSet0->add)(fsJobSet0, (void *)jPtr);
                        }
                    }
                }
            }
            /*We schedule migrate jobs firstly, then schedule pending jobs.
             *So we need make sure migrated jobs and pending jobs in different 
             *FS job set.
             */
            fsJobSet0 = NULL;
        }

        if (logclass & LC_SCHED) {
            gettimeofday(&scheduleStartTime, NULL);
            ls_syslog(LOG_DEBUG, "\
%s: begin a new schedule and dispatch session", fname);
        }

        mSchedStage |= M_STAGE_INIT;
    }

    if (!(mSchedStage & M_STAGE_GOT_LOAD)) {

        if (now_disp - lastUpdTime > freshPeriod) {
            int returnCode;

            for (jPtr = jDataList[SJL]->back;
                 jPtr != jDataList[SJL];
                 jPtr = jPtr->back) {

                if (jPtr->jStatus & JOB_STAT_RUN) {
                    TIMEVAL2(3, accumRunTime(jPtr, jPtr->jStatus, now_disp), tmpVal);
                    timeFSUpdLoad += tmpVal;
                }
            }

            if (numResources > 0) {
                TIMEIT(0, getLsbResourceInfo(), "getLsbResourceInfo()");
                lastSharedResourceUpdateTime = now_disp;
            }

            TIMEIT(0, returnCode = getLsbHostLoad(), "getLsbHostLoad()");
            if (returnCode != 0) {

                return -1;
            }
            lastUpdTime = now_disp;
        }

        if (logclass & LC_FAIR) { 
            dumpFSPolicies(fname, LOG_DEBUG2);
        } 

        mSchedStage |= M_STAGE_GOT_LOAD;
    }

    if (STAY_TOO_LONG) {
        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG, "\
%s: Stayed too long in M_STAGE_GOT_LOAD", fname);
        }
        DUMP_CNT();
        RESET_CNT();
        return -1;
    }

    if ( (sharedResourceUpdFactor != INFINIT_INT)
         &&
         (now_disp > (lastSharedResourceUpdateTime
                      + msleeptime/sharedResourceUpdFactor))) {

        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG,"\
%s: now_disp=%d lastSharedResourceUpdateTime=%d diff=%d, mSchedStage=%x",
                      fname, now_disp,
                      lastSharedResourceUpdateTime,
                      now_disp - (lastSharedResourceUpdateTime
                                  + msleeptime/sharedResourceUpdFactor),
                      mSchedStage);
        }

        resetSharedResource();

        for (jPtr  = jDataList[SJL]->back;
             jPtr != jDataList[SJL];
             jPtr  = jPtr->back) {

            updSharedResourceByRUNJob(jPtr);

        }
        lastSharedResourceUpdateTime = now_disp;
    }

    if (!(mSchedStage & M_STAGE_RESUME_SUSP)) {
        TIMEIT(0, tryResume(), "tryResume()");
        mSchedStage |= M_STAGE_RESUME_SUSP;
    }

    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG, "\
%s: M_STAGE_RESUME_SUSP tryResumed",
                  fname);
    }

    if (STAY_TOO_LONG) {
        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG, "\
%s: Stayed too long in M_STAGE_RESUME_SUSP", fname);
        }
        DUMP_CNT();
        RESET_CNT();
        return -1;
    }

    if (!(mSchedStage & M_STAGE_LSB_CAND)) {
        TIMEIT(3, numLsbUsable = getLsbUsable(), "getLsbUsable()");
        mSchedStage |= M_STAGE_LSB_CAND;
    }

    if (numLsbUsable <= 0) {
        numLsbUsable = numQUsable = 0;
        resetSchedulerSession();
        return(0);
    }

    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG, "\
%s: M_STAGE_LSB_CAND got numLsbUsable=%d",
                  fname, numLsbUsable);
    }

    if (STAY_TOO_LONG) {
        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG, "\
%s: Stayed too long in M_STAGE_LSB_CAND", fname);
        }
        DUMP_CNT();
        RESET_CNT();
        return -1;
    }

    if (!(mSchedStage & M_STAGE_QUE_CAND)) {
        if (numLsbUsable > 0) {
            if (nextSchedQ == qDataList->back) {

                numQUsable = 0;
            }
            loopCount = 0;
            for (qp = nextSchedQ; qp != qDataList; qp = qp->back) {
                int num;
                if (qp->numPEND == 0 && qp->numRESERVE == 0) {
                    continue;
                }
                INC_CNT(PROF_CNT_getQUsable);
                TIMEVAL2(3, num = getQUsable(qp), tmpVal);
                timeGetQUsable += tmpVal;
                if (num <= 0) {
                    continue;
                }
                numQUsable += num;
                if (((++loopCount) % 10) == 0 && STAY_TOO_LONG) {
                    nextSchedQ = qp->back;
                    if (logclass & LC_SCHED) {
                        ls_syslog(LOG_DEBUG, "\
%s: Stayed too long in M_STAGE_QUE_CAND; numQUsable=%d timeGetQUsable %d ms",
                                  fname, numQUsable, timeGetQUsable);
                        DUMP_CNT();
                        RESET_CNT();
                    }
                    return -1;
                }
            }
        }
        mSchedStage |= M_STAGE_QUE_CAND;
    }

    if (numQUsable <= 0) {
        numQUsable = 0;
        resetSchedulerSession();
        return(0);
    }

    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG,"\
%s M_STAGE_QUE_CAND numQUsable=%d timeGetQUsable %d ms",
                  fname, numQUsable, timeGetQUsable);
    }

    loopCount = 0;
    ZERO_OUT_TIMERS();
    fsJobSet0 = NULL;

again:
    min = INT32_MAX;
    jPtr = NULL;

    /*Now we look insight a FS queue now, after we schedule all jobs of this FS queue,
     *then schedule other queues' jobs.
     */
    if (fsJobSet0) {
        TIMEVAL2(1,jPtr = sch_fs_electJob(fsJobSet0, &attachedShareAcct), tmpVal);
        timeFSElectAJob += tmpVal;

        if (jPtr == NULL) {
            if (logclass & (LC_SCHED | LC_FAIR)) {
                ls_syslog(LOG_DEBUG1, "\
%s: Fairshare Queue <%s>, timeFSElectAJob %0.2f ms.",
                        fname,
                        fsJobSet0->policy->name,
                        (timeFSElectAJob - qTimeFSElectJob));

                ls_syslog(LOG_DEBUG1, "\
%s: ***** End to schedule jobs for fairshare Queue <%s> *****", fname, fsJobSet0->policy->name);
            }
            sch_fs_freeJobSet(fsJobSet0);
            fsJobSet0 = NULL;
            listRemoveEntry(jRefList, (struct _listEntry *)jR0);                                                                                              
            FREEUP(jR0);
        }
    }

    if (fsJobSet0 == NULL) {
        jR0 = NULL;
    }

    /*Fetch a new job or jobset from jRefList*/
    for (jR = (struct jRef *)jRefList->back;
         jR != (void *)jRefList && !fsJobSet0;
         jR = (struct jRef *)jR->back) {
        jPtr = NULL;

        if (jR->type == TYPE_JOB_SINGLE) {/*fcfs, or round_robin*/
            jPtr = jR->job;
            assert(jPtr->uPtr->numPEND > 0);

            if (! (jPtr->qPtr->qAttrib & Q_ATTRIB_ROUND_ROBIN)) {
                /* this is a fcfs queue so just dequeue the first
                 * job on the priority list and try to run it.
                 */
                listRemoveEntry(jRefList,
                            (struct _listEntry *)jR);
                free(jR);
                break;
            }

            if (jPtr->uPtr->numRUN < min) {
                /* get the job whose user has
                * the least running jobs.
                */
                min = jPtr->uPtr->numRUN;
                jR0 = jR;
            }

            jPtr0 = jR->back->job;
            if (jR->back == (void *)jRefList
                || jPtr->qPtr->priority != jPtr0->qPtr->priority) {
                /* either at the end of the list, in which case
                 * jPtr0 is bogus, or we just hit another queue
                 * so we have to give to the dispatcher the current
                 * higher priority job.
                 */
                listRemoveEntry(jRefList, (struct _listEntry *)jR0);
                jPtr = jR0->job;
                free(jR0);
                break;
            }
        } else if (jR->type == TYPE_JOB_SET) {/*fairshare*/
            /*Start to schedule jobs from Fairshare Queue*/
            fsJobSet0 = (struct fsQueueJobSet *)jR->job;

            if (fsJobSet0 == NULL) {
                /*If dispatch is timeout, it may cause jR is empty and kept in job list*/
                continue;
            }

            if (logclass & (LC_SCHED | LC_FAIR)) {
                ls_syslog(LOG_DEBUG1, "\
%s: ***** Start to schedule jobs for fairshare Queue <%s> *****", fname, fsJobSet0->policy->name);                                                                                                                  
            }

            if (logclass & LC_FAIR) {
                qTimeFSElectJob = timeFSElectAJob; 
            }

            /*Schedule jobs for next queue. Init jobset which caches peer pending jobs 
             *which had been scheduled. The purpose is to reuse those jobs candidate
             *hosts in getCandHosts().
             */
            if (fsJobSet0->status == M_STAGE_FS_INIT) {
                initSchedPendJobSet(fsJobSet0->policy);
                sch_fs_init(fsJobSet0->policy);
            }
            
            TIMEVAL2(2, jPtr = sch_fs_electJob(fsJobSet0, &attachedShareAcct), tmpVal);
            timeFSElectAJob += tmpVal;

            if (jPtr == NULL) {
                /*If dispatch is timeout, jobset could be empty because this func is returned in advance*/

                if (logclass & (LC_SCHED | LC_FAIR)) {
                    ls_syslog(LOG_DEBUG1,"%s: queue <%s> has no pending jobs anymore.", fname, fsJobSet0->policy->name);
                    ls_syslog(LOG_DEBUG1, "\
%s: ***** End to schedule jobs for fairshare Queue <%s> *****", fname, fsJobSet0->policy->name);
                }

                sch_fs_freeJobSet(fsJobSet0);
                fsJobSet0 = NULL;
                jR->job = NULL;
                continue;
            } else {
                /*Find job, this is a valid jobset. Let us pop out and start to schedule jobs in this jobset*/
                fsJobSet0->status = M_STAGE_FS_PROCESS;
                jR0 = jR;
                break;
            }
        } else {
            /*cannot handle this type*/
            continue;
        }
    } /* for (jRef = jRefList->back; ...;...) */

    /*If there are only suspend jobs left, or the last job is from fs queue,
     *we could get jPtr be NULL to tell no job anymore.
     */
    if (jPtr == NULL) {
        goto __END__;
    }

    TIMEVAL2(0, scheduleAJob(jPtr, TRUE, TRUE), tmpVal);
    timeScheduleAJob += tmpVal;

    TIMEVAL2(0, (dispRet = XORDispatch(jPtr, FALSE, dispatchAJob0)), tmpVal);
    timeDispatchAJob += tmpVal;

    /*FS job couldn't been dispatched, we try to cache it for peer jobs which may reuse candidate hosts or pending reason*/
    if ((jPtr->qPtr->qAttrib & Q_ATTRIB_FS ) &&
        (jPtr->jStatus & (JOB_STAT_PEND | JOB_STAT_MIG))) {
        (*schedPendJobSet.add)(&schedPendJobSet, (void *)jPtr);
    }

    if (dispRet == DISP_TIME_OUT) {
        ls_syslog(LOG_DEBUG,"\
%s STAY_TOO_LONG 3 loopCount <%d>", fname, loopCount);
        DUMP_TIMERS(fname);
        DUMP_CNT();
        RESET_CNT();
        return -1;
    }
    if (dispRet == DISP_FAIL && STAY_TOO_LONG) {
        DUMP_TIMERS(fname);
        DUMP_CNT();
        RESET_CNT();
        return -1;
    }

    /*Attached job with shareAcct*/
    if (attachedShareAcct && (jPtr->jStatus &  JOB_STAT_RUN)) {
        attachJob2FSTree(fsJobSet0->policy, jPtr, attachedShareAcct, fname);
    }

    /* if there are more jobs waiting to
     * be processed go ahead and schedule them
     */
    if (jRefList->numEnts > 0 || fsJobSet0)
        goto again;

__END__:
    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG,"\
%s out of pickAJob/scheduleAJob loopCount <%d>", fname, loopCount);
        DUMP_TIMERS(fname);
        DUMP_CNT();
        RESET_CNT();
    }

    copyReason();

    TIMEIT(0, disp_clean(), "disp_clean()");

    if (logclass & LC_SCHED) {
        gettimeofday(&scheduleFinishTime, NULL);
        scheduleTime =
            (scheduleFinishTime.tv_sec - scheduleStartTime.tv_sec)*1000 +
            (scheduleFinishTime.tv_usec - scheduleStartTime.tv_usec)/1000;
        ls_syslog(LOG_DEBUG, "%s: Completed a schedule and dispatch session seqNo=%d, time used: %d ms", fname, schedSeqNo, scheduleTime);
    }

    ++schedSeqNo;

    if (schedSeqNo > INFINIT_INT - 1) {
        schedSeqNo = 0;
    }

    /* Free the pending reference list.
     */
    for (jR = (struct jRef *)jRefList->back;
         jR != (void *)jRefList; ) {

        jR0 = jR->back;
        listRemoveEntry(jRefList, (LIST_ENTRY_T *)jR);
        if (jR->type == TYPE_JOB_SET) {
            sch_fs_freeJobSet(jR->job);
        }
        free(jR);
        jR = jR0;
    }

    DUMP_TIMERS(fname);
    DUMP_CNT();
    RESET_CNT();

    return(0);

}

static int
checkIfJobIsReady(struct jData *jp)
{
    static char fname[] = "checkIfJobIsReady";
    int tmpVal;
    int cc;


    TIMEVAL2(2, cc = readyToDisp(jp, &jp->numAvailSlots), tmpVal);
    timeReadyToDisp += tmpVal;

    if (cc) {


        if (jp->shared->jobBill.maxNumProcessors == 1) {
            TIMEVAL2(2,
                    jp->numSlots = cntUQSlots(jp, &jp->numAvailSlots),
                    tmpVal);
            timeCntUQSlots += tmpVal;
        }
    }

    if (jp->numSlots <= 0) {
        jp->numSlots = 0;
        jp->numAvailSlots = 0;
    } else {

        jp->numAvailSlots = MIN(jp->numSlots, jp->numAvailSlots);
    }

    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG3, "%s: job=%s processed=%x newReason=%d numSlots=%d numAvailSlots=%d", fname, lsb_jobid2str(jp->jobId), jp->processed, jp->newReason, jp->numSlots, jp->numAvailSlots);
    }

    return jp->numSlots;

}

static int
scheduleAJob(struct jData *jp, bool_t checkReady, bool_t checkOtherGroup)
{
    static char fname[] = "scheduleAJob";
    int ret;
    int tmpVal = 0;


    if (jp->pendEvent.sig != SIG_NULL) {
        sigPFjob(jp, jp->pendEvent.sig, 0, LOG_IT);

        if (!IS_PEND(jp->jStatus)) {
            return 0;
        }

        if (logclass & (LC_TRACE | LC_SIGNAL)) {
            ls_syslog (LOG_DEBUG2, "%s: Sent pending signal <%d> to job %s", fname, jp->pendEvent.sig, lsb_jobid2str(jp->jobId));
        }
    }

    if (checkReady && (!jobIsReady(jp)))
        return 0;


    if (jp->processed & JOB_STAGE_CAND) {

        if (checkOtherGroup) {
            ret = XORCheckIfCandHostIsOk(jp);
        } else {

            ret = checkIfCandHostIsOk(jp);
        }
    } else {
        TIMEVAL2(2, ret = getCandHosts(jp), tmpVal);
        timeGetCandHosts += tmpVal;
        if (logclass & (LC_SCHED | LC_PEND)) {

            ls_syslog(LOG_DEBUG2, "%s: Got %d candidate groups for job <%s>", fname, jp->numOfGroups, lsb_jobid2str(jp->jobId));

            ls_syslog(LOG_DEBUG2, "%s: Got %d candidate hosts for job <%s>", fname, jp->numCandPtr, lsb_jobid2str(jp->jobId));
        }
        jp->processed |= JOB_STAGE_CAND;
    }

    switch (ret) {
        case CAND_NO_HOST:
            if (logclass & (LC_SCHED | LC_PEND)) {
                ls_syslog(LOG_DEBUG1, "%s: Can't get enough candidate hosts for job <%s>", fname, lsb_jobid2str(jp->jobId));
            }
            jp->processed |= JOB_STAGE_DONE;
            return 0;
            break;

        case CAND_FIRST_RES:

            jp->processed |= JOB_STAGE_DONE;
            return 0;
        case CAND_HOST_FOUND:
            return 1;
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7232,
                                             "%s: Unknown return code <%d> from getCandHosts() for job <%s>"), fname, ret, lsb_jobid2str(jp->jobId)); /* catgets 7232 */
            return 0;
    }
}

static enum dispatchAJobReturnCode
dispatchAJob0(struct jData *jp, int dontTryNextCandHost)
{
    static enum dispatchAJobReturnCode ret;

    ret = dispatchAJob(jp, dontTryNextCandHost);
    deallocExecCandPtr(jp);
    return ret;
}

static enum dispatchAJobReturnCode
dispatchAJob(struct jData *jp, int dontTryNextCandHost)
{
    static char fname[] = "dispatchAJob";
    int qSchedDelay;
    int notEnoughSlot = FALSE;
    int notEnoughResource = FALSE;
    int reserved;

    qSchedDelay = QUEUE_SCHED_DELAY(jp);
    if (now_disp - jp->shared->jobBill.submitTime < qSchedDelay) {
        if (logclass & (LC_PEND | LC_SCHED)) {
            ls_syslog(LOG_DEBUG1, "%s: Queue %s has a schedule delay (%d) for new job %s; now=%d submitTime=%d", fname, jp->qPtr->queue, qSchedDelay, lsb_jobid2str(jp->jobId), (int)now_disp, (int)jp->shared->jobBill.submitTime);
        }

        jp->newReason = PEND_JOB_DELAY_SCHED;
        jp->processed |= JOB_STAGE_DONE;
        return DISP_NO_JOB;
    }

    getNumSlots(jp);

    while (1) {

        INC_CNT(PROF_CNT_loopdispatchAJob);


        getNumProcs(jp);

        if (jp->numAvailEligProc < MAX(jp->shared->jobBill.numProcessors, jp->qPtr->minProcLimit)) {


            if (logclass & (LC_PEND | LC_SCHED)) {
                ls_syslog(LOG_DEBUG1, "%s: job <%s> can't get enough slots, requested(numProcessors)=%d available(numAvailEligProc)=%d", fname, lsb_jobid2str(jp->jobId),
                          jp->shared->jobBill.numProcessors,
                          jp->numAvailEligProc);
            }


            if ((jp->qPtr->slotHoldTime <= 0)) {
                if (logclass & (LC_PEND | LC_SCHED)) {
                    ls_syslog(LOG_DEBUG1, "%s: job <%s> is not allowed to reserve any slots", fname, lsb_jobid2str(jp->jobId));
                }
                jp->processed |= JOB_STAGE_DONE;
                return DISP_NO_JOB;
            }


            if (jp->qPtr->slotHoldTime <= 0 &&
                jp->numEligProc < MAX(jp->shared->jobBill.numProcessors, jp->qPtr->minProcLimit)) {
                if (logclass & (LC_PEND | LC_SCHED)) {
                    ls_syslog(LOG_DEBUG1, "%s: job <%s> can't get enough slots to reserve and preempt",
                              fname, lsb_jobid2str(jp->jobId));
                }
                jp->processed |= JOB_STAGE_DONE;
                return DISP_NO_JOB;
            }

            notEnoughSlot = TRUE;


            if (jp->shared->resValPtr
                && jp->shared->resValPtr->pTile != INFINIT_INT) {
                addReason (jp, 0, PEND_JOB_SPREAD_TASK);
            } else if ( ((jp->shared->resValPtr == NULL)
                         || (jp->shared->resValPtr->maxNumHosts != 1))
                        && jp->qPtr->resValPtr
                        && (jp->qPtr->resValPtr->pTile != INFINIT_INT) ) {
                addReason (jp, 0, PEND_QUE_SPREAD_TASK);
            }
        }

        if (jp->jStatus & JOB_STAT_RESERVE) {
            freeReserveSlots(jp);
        }

        if (!notEnoughSlot && !notEnoughResource) {
            enum dispatchAJobReturnCode ret;

            ret = dispatchToCandHost(jp);
            if (ret != DISP_FAIL) {
                jp->processed |= JOB_STAGE_DONE;
                if (ret == DISP_OK) {
                    jp->processed |= JOB_STAGE_DISP;

                    updHostLeftRusageMem(jp, -1);
                }
                return ret;
            } else {
                if (dontTryNextCandHost) {
                    if (logclass & (LC_SCHED | LC_PEND)) {
                        ls_syslog(LOG_DEBUG1, "%s: dispatching job <%s> to candHost failed and other candHost are not allowed to be tried at this stage", fname, lsb_jobid2str(jp->jobId));
                    }
                    return DISP_FAIL;
                }
                if (jp->numCandPtr == 0 || (jp->newReason & PEND_JOB_NO_FILE)) {

                    jp->processed |= JOB_STAGE_DONE;
                    return DISP_FAIL;
                }
                if (STAY_TOO_LONG) {
                    if (logclass & (LC_SCHED | LC_PEND)) {
                        ls_syslog(LOG_DEBUG1, "%s: dispatching job <%s> to candHost failed and other candHost will not be tried due to staying here too long", fname, lsb_jobid2str(jp->jobId));
                    }
                    return DISP_TIME_OUT;
                }

                continue;
            }
        } else {
            int hasresources = TRUE;
            int reservedResource = FALSE;

            if (notEnoughResource) {

                if (reservePreemptResourcesForExecCands(jp) != 0) {
                    hasresources = FALSE;
                } else {
                    reservedResource = TRUE;
                }
                if (!notEnoughSlot) {
                    return DISP_RESERVE;
                }
            }
            reserved = FALSE;

            if ( jp->qPtr->slotHoldTime > 0 ) {

                if (enoughMaxUsableSlots(jp)) {


                    if (hasresources == TRUE && allocHosts(jp) >= 0) {

                        if (qAttributes & Q_ATTRIB_BACKFILL) {
                            jobStartTime (jp);
                        }
                        reserveSlots (jp);
                        reserved = TRUE;
                    }
                } else {

                    if (logclass & (LC_SCHED | LC_PEND)) {
                        ls_syslog(LOG_DEBUG1, "%s: job <%s> will not reserve slots because there not enough online host slots for the job", fname, lsb_jobid2str(jp->jobId));
                    }
                }
            }
            jp->processed |= JOB_STAGE_DONE;
            if (reserved || reservedResource ) {
                if (logclass & (LC_PEND | LC_SCHED)) {
                    ls_syslog(LOG_DEBUG1, "%s: job <%s> reserved slots",
                              fname, lsb_jobid2str(jp->jobId));
                }
                return DISP_RESERVE;
            } else {
                if (logclass & (LC_PEND | LC_SCHED)) {
                    ls_syslog(LOG_DEBUG1, "%s: job <%s> can't reserve slots",
                              fname, lsb_jobid2str(jp->jobId));
                }
                return DISP_NO_JOB;
            }
        }
    }
}

static enum candRetCode
checkIfCandHostIsOk(struct jData *jp)
{
    static char fname[] = "checkIfCandHostIsOk";
    int nSlots, nAvailSlots, numTotalSlots = 0;
    int hReason = 0;
    int svReason = jp->newReason;
    int i;

    for (i = 0; i < jp->numCandPtr; i++) {

        nSlots = candHostOk(jp, i, &nAvailSlots, &hReason);
        if (nSlots <= 0) {
            jp->candPtr[i].numSlots = 0;
            jp->candPtr[i].numAvailSlots = 0;
            if (jp->newReason != 0) {
                hReason = jp->newReason;
                addReason(jp, jp->candPtr[i].hData->hostId, hReason);
            }

            jp->newReason = svReason;
            if (logclass & (LC_SCHED | LC_PEND)) {
                ls_syslog (LOG_DEBUG2, "%s: job <%s> candidate %s not eligible any more; reason=%d", fname, lsb_jobid2str(jp->jobId), jp->candPtr[i].hData->host, hReason);
            }
            continue;
        }
        jp->newReason = svReason;

        jp->candPtr[i].numSlots = MIN(jp->candPtr[i].numSlots, nSlots);
        jp->candPtr[i].numAvailSlots = MIN(jp->candPtr[i].numAvailSlots,
                                           nAvailSlots);

        numTotalSlots += jp->candPtr[i].numSlots;

        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG2, "%s: Got a candidate %s/%d/%d",
                      fname,
                      jp->candPtr[i].hData->host,
                      jp->candPtr[i].numSlots,
                      jp->candPtr[i].numAvailSlots);
        }
    }
    if (numTotalSlots) {
        return CAND_HOST_FOUND;
    } else {
        return CAND_NO_HOST;
    }
}

static void
getNumSlots(struct jData *jp)
{
    int i, numSlots = 0, numAvailSlots = 0;

    for (i = 0; i < jp->numCandPtr; i++) {

        jp->candPtr[i].numSlots = MIN(jp->candPtr[i].numSlots,
                                      jp->numSlots);
        jp->candPtr[i].numAvailSlots =
            MIN(jp->candPtr[i].numAvailSlots, jp->candPtr[i].numSlots);
        jp->candPtr[i].numAvailSlots =
            MIN(jp->candPtr[i].numAvailSlots, jp->numAvailSlots);


        if (jp->candPtr[i].numAvailSlots < jp->shared->jobBill.numProcessors) {
            addReason(jp, jp->candPtr[i].hData->hostId,
                      PEND_HOST_LESS_SLOTS);
        }


        if (allInOne(jp) && (!needHandleFirstHost(jp))) {
            if (numSlots == 0) {
                if (jp->candPtr[i].numSlots > 0) {

                    numSlots = jp->candPtr[i].numSlots;
                    numAvailSlots = jp->candPtr[i].numAvailSlots;
                    moveHostPos(jp->candPtr, i, 0);
                }
            } else if (numSlots < jp->shared->jobBill.numProcessors &&
                       jp->candPtr[i].numSlots >=
                       jp->shared->jobBill.numProcessors) {

                numSlots = jp->candPtr[i].numSlots;
                numAvailSlots = jp->candPtr[i].numAvailSlots;
                moveHostPos(jp->candPtr, i, 0);
            }
        } else {
            numSlots += jp->candPtr[i].numSlots;
            numAvailSlots += jp->candPtr[i].numAvailSlots;
        }
    }
    jp->numSlots = MIN(jp->numSlots, numSlots);
    jp->numAvailSlots = MIN(jp->numAvailSlots, numAvailSlots);
}

static enum dispatchAJobReturnCode
dispatchToCandHost(struct jData *jp)
{
    static char fname[] = "dispatchToCandHost";
    int tmpVal;

    if (allocHosts(jp) < 0) {
        if (logclass & (LC_SCHED | LC_PEND)) {
            ls_syslog(LOG_DEBUG1, "%s: allocHosts() failed for job <%s>",
                      fname, lsb_jobid2str(jp->jobId));
        }
        return DISP_NO_JOB;
    }

    if (jp->numHostPtr < jp->shared->jobBill.numProcessors) {
        if (logclass & (LC_SCHED | LC_PEND)) {
            ls_syslog(LOG_DEBUG1, "%s: No enough hosts for job %s",
                      fname, lsb_jobid2str(jp->jobId));
        }
        deallocHosts(jp);
        return DISP_NO_JOB;
    }
    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG2, "%s: Try to dispatch job %s to host %s",
                  fname, lsb_jobid2str(jp->jobId), jp->hPtr[0]->host);
    }

    jp->newReason = 0;
    TIMEIT(3, tmpVal = dispatch_it(jp), "dispatch_it()");
    if (tmpVal) {

        setExecHostsAcceptInterval(jp);

        if (logclass & LC_SCHED) {
            ls_syslog(LOG_DEBUG2, "\
%s: Job %s has been  dispatched to host %s",
                      fname, lsb_jobid2str(jp->jobId),
                      jp->hPtr[0]->host);
        }

        return DISP_OK;

    } else {

        if (logclass & (LC_SCHED | LC_PEND)) {
            ls_syslog(LOG_DEBUG1, "%s: Job %s failed to start on host %s; reason=%d", fname, lsb_jobid2str(jp->jobId), jp->hPtr[0]->host, jp->newReason);
        }

        deallocHosts(jp);
        if (!(jp->newReason & PEND_JOB_NO_FILE)) {

            removeCandHost(jp, 0);

        }
        return DISP_FAIL;
    }
}

static void
getNumProcs(struct jData *jp)
{
#define HAS_HOST_PREFERNECE(jp) ((jp)->numAskedPtr || (jp)->qPtr->numAskedPtr)
    static char fname[] = "getNumProcs";
    int i, nSlots, nAvailSlots, backfillSlots;
    struct candHost *execCandPtr;
    struct backfillCand *backfillCandPtr;
    LIST_T *sortedBackfilleeList;
    LIST_ITERATOR_T iter;
    struct backfillee *backfillee, *nextBackfillee;
    int backfillCandPtrIndex;

    jp->numEligProc = 0;
    jp->numAvailEligProc = 0;

    execCandPtr = (struct candHost *)my_malloc(jp->numCandPtr * sizeof(struct candHost), fname);
    for (i = 0; i < jp->numCandPtr; i++) {
        execCandPtr[i].hData = NULL;
        execCandPtr[i].numSlots = 0;
        execCandPtr[i].numAvailSlots = 0;
        execCandPtr[i].backfilleeList = NULL;
    }

    if (JOB_CAN_BACKFILL(jp) &&
        !HAS_HOST_PREFERNECE(jp) &&
        jobHasBackfillee(jp) &&
        !allInOne(jp)) {
        sortedBackfilleeList = listCreate(NULL);

        sortBackfillee(jp, sortedBackfilleeList);
        backfillCandPtr = (struct backfillCand *)my_malloc(jp->numCandPtr * sizeof(struct backfillCand), fname);
        backfillCandPtrIndex = 0;
        for (i = 0; i < jp->numCandPtr; i++) {
            backfillCandPtr[i].numSlots = 0;
            backfillCandPtr[i].numAvailSlots = 0;
            backfillCandPtr[i].indexInCandHostList = 0;
            backfillCandPtr[i].backfilleeList = NULL;
        }

        (void)listIteratorAttach(&iter, sortedBackfilleeList);

        for (backfillee = (struct backfillee *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter) &&
                 jp->numAvailEligProc < jp->numAvailSlots &&
                 jp->numAvailEligProc < jp->shared->jobBill.maxNumProcessors;
             backfillee = nextBackfillee) {
            int maxSlots, slotsToUse;

            backfillSlots = MIN(backfillee->backfillSlots,
                                jp->shared->jobBill.maxNumProcessors -
                                jp->numAvailEligProc);
            for (i = 0; i < backfillCandPtrIndex; i++) {
                if (backfillee->indexInCandHostList ==
                    backfillCandPtr[i].indexInCandHostList) {

                    break;
                }
            }


            maxSlots =
                jp->candPtr[backfillee->indexInCandHostList].numAvailSlots;

            slotsToUse = MIN(maxSlots, backfillSlots);
            if (i == backfillCandPtrIndex) {

                backfillCandPtr[i].numAvailSlots = slotsToUse;
                backfillCandPtr[i].indexInCandHostList =
                    backfillee->indexInCandHostList;
                backfillCandPtrIndex++;
            } else {


                slotsToUse = MIN(slotsToUse,
                                 maxSlots - backfillCandPtr[i].numAvailSlots);
                backfillCandPtr[i].numAvailSlots += slotsToUse;
            }
            backfillee->backfillSlots = slotsToUse;

            backfillCandPtr[i].numSlots = backfillCandPtr[i].numAvailSlots;

            listIteratorNext(&iter, (LIST_ENTRY_T **)&nextBackfillee);
            listRemoveEntry(sortedBackfilleeList, (LIST_ENTRY_T *)backfillee);
            if (backfillCandPtr[i].backfilleeList == NULL) {
                backfillCandPtr[i].backfilleeList = listCreate(NULL);
            }
            if (slotsToUse != 0) {
                listInsertEntryAtBack(backfillCandPtr[i].backfilleeList,
                                      (LIST_ENTRY_T *)backfillee);
            } else {

                free(backfillee);
            }
            jp->numAvailEligProc += slotsToUse;
        }
        jp->numEligProc = jp->numAvailEligProc;


        if (jp->numAvailEligProc < jp->numAvailSlots &&
            jp->numAvailEligProc < jp->shared->jobBill.maxNumProcessors) {

            for (i = 0;
                 i < backfillCandPtrIndex &&
                     jp->numAvailEligProc < jp->numAvailSlots &&
                     jp->numAvailEligProc < jp->shared->jobBill.maxNumProcessors;
                 i++) {
                nAvailSlots = MIN(jp->candPtr[backfillCandPtr[i].indexInCandHostList].numAvailSlots -
                                  backfillCandPtr[i].numAvailSlots,
                                  jp->shared->jobBill.maxNumProcessors -
                                  jp->numAvailEligProc);
                if (nAvailSlots == 0) {
                    continue;
                }
                backfillCandPtr[i].numAvailSlots += nAvailSlots;
                backfillCandPtr[i].numSlots = backfillCandPtr[i].numAvailSlots;
                jp->numAvailEligProc += nAvailSlots;
            }
            jp->numEligProc = jp->numAvailEligProc;
        }


        if (jp->numAvailEligProc < jp->numAvailSlots &&
            jp->numAvailEligProc < jp->shared->jobBill.maxNumProcessors) {

            for (i = 0;
                 i < jp->numCandPtr &&
                     jp->numAvailEligProc < jp->numAvailSlots &&
                     jp->numAvailEligProc < jp->shared->jobBill.maxNumProcessors;
                 i++) {
                if (candHostInBackfillCandList(backfillCandPtr,
                                               backfillCandPtrIndex,
                                               i)) {

                    continue;
                }
                if (jp->candPtr[i].numAvailSlots == 0) {
                    continue;
                }

                nAvailSlots = MIN(jp->candPtr[i].numAvailSlots,
                                  jp->shared->jobBill.maxNumProcessors -
                                  jp->numAvailEligProc);
                backfillCandPtr[backfillCandPtrIndex].numAvailSlots =
                    nAvailSlots;
                backfillCandPtr[backfillCandPtrIndex].numSlots = nAvailSlots;
                backfillCandPtr[backfillCandPtrIndex].indexInCandHostList = i;
                jp->numAvailEligProc += nAvailSlots;
                backfillCandPtrIndex++;
            }
            jp->numEligProc = jp->numAvailEligProc;
        }


        if (jp->numAvailEligProc < jp->shared->jobBill.numProcessors) {

            for (i = 0;
                 i < backfillCandPtrIndex &&
                     jp->numEligProc < jp->numSlots &&
                     jp->numEligProc < jp->shared->jobBill.numProcessors;
                 i++) {
                nSlots = MIN(jp->candPtr[backfillCandPtr[i].indexInCandHostList].numSlots -
                             backfillCandPtr[i].numSlots,
                             jp->shared->jobBill.maxNumProcessors -
                             jp->numEligProc);
                if (nSlots == 0) {
                    continue;
                }
                backfillCandPtr[i].numSlots += nSlots;
                jp->numEligProc += nSlots;
            }

            if (jp->numEligProc < jp->numSlots &&
                jp->numEligProc < jp->shared->jobBill.numProcessors) {
                for (i = 0;
                     i < jp->numCandPtr &&
                         jp->numEligProc < jp->numSlots &&
                         jp->numEligProc < jp->shared->jobBill.numProcessors;
                     i++) {
                    if (candHostInBackfillCandList(backfillCandPtr,
                                                   backfillCandPtrIndex,
                                                   i)) {

                        continue;
                    }
                    if (jp->candPtr[i].numSlots == 0) {
                        continue;
                    }

                    nSlots = MIN(jp->candPtr[i].numSlots,
                                 jp->shared->jobBill.maxNumProcessors -
                                 jp->numEligProc);
                    backfillCandPtr[backfillCandPtrIndex].numSlots = nSlots;

                    backfillCandPtr[backfillCandPtrIndex].numAvailSlots =
                        MIN(jp->candPtr[i].numAvailSlots, nSlots);
                    backfillCandPtr[backfillCandPtrIndex].indexInCandHostList = i;
                    jp->numEligProc += nSlots;
                    backfillCandPtrIndex++;
                }
            }
        }


        for (i = 0; i < backfillCandPtrIndex; i++) {
            execCandPtr[i].hData =
                jp->candPtr[backfillCandPtr[i].indexInCandHostList].hData;
            execCandPtr[i].numSlots = backfillCandPtr[i].numSlots;
            execCandPtr[i].numAvailSlots = backfillCandPtr[i].numAvailSlots;
            execCandPtr[i].backfilleeList = backfillCandPtr[i].backfilleeList;
        }
        free(backfillCandPtr);

        jp->numExecCandPtr = backfillCandPtrIndex;
        FREEUP(jp->execCandPtr);
        jp->execCandPtr = execCandPtr;
        doBackfill(jp);

        listDestroy(sortedBackfilleeList, NULL);
    } else {



        for (i = 0;
             i < jp->numCandPtr &&
                 jp->numAvailEligProc < jp->numAvailSlots &&
                 jp->numAvailEligProc < jp->shared->jobBill.maxNumProcessors;
             i++) {
            nAvailSlots = MIN(jp->candPtr[i].numAvailSlots,
                              jp->shared->jobBill.maxNumProcessors -
                              jp->numAvailEligProc);


            nAvailSlots = MIN(nAvailSlots,
                              jp->numAvailSlots - jp->numAvailEligProc);

            execCandPtr[i].numAvailSlots = nAvailSlots;
            execCandPtr[i].numSlots = nAvailSlots;
            execCandPtr[i].hData = jp->candPtr[i].hData;
            jp->numAvailEligProc += nAvailSlots;
        }
        jp->numExecCandPtr = i;
        jp->numEligProc = jp->numAvailEligProc;

        if (jp->numAvailEligProc < jp->shared->jobBill.numProcessors) {

            for (i = 0;
                 i < jp->numExecCandPtr &&
                     jp->numEligProc < jp->numSlots &&
                     jp->numEligProc < jp->shared->jobBill.numProcessors;
                 i++) {

                if ( (jp->candPtr[i].hData)->maxJobs <=
                     (jp->candPtr[i].hData)->numRESERVE) {
                    continue;
                }

                nSlots = MIN(jp->candPtr[i].numSlots -
                             execCandPtr[i].numAvailSlots,
                             jp->shared->jobBill.numProcessors -
                             jp->numEligProc);
                execCandPtr[i].numSlots += nSlots;
                jp->numEligProc += nSlots;
            }

            if (jp->numEligProc < jp->numSlots &&
                jp->numEligProc < jp->shared->jobBill.numProcessors) {
                for (i = jp->numExecCandPtr;
                     i < jp->numCandPtr &&
                         jp->numEligProc < jp->numSlots &&
                         jp->numEligProc < jp->shared->jobBill.numProcessors;
                     i++) {

                    if ( (jp->candPtr[i].hData)->maxJobs <=
                         (jp->candPtr[i].hData)->numRESERVE) {
                        continue;
                    }

                    nSlots = MIN(jp->candPtr[i].numSlots,
                                 jp->shared->jobBill.numProcessors -
                                 jp->numEligProc);

                    execCandPtr[i].numAvailSlots =
                        MIN(jp->candPtr[i].numAvailSlots, nSlots);
                    execCandPtr[i].numSlots = nSlots;
                    execCandPtr[i].hData = jp->candPtr[i].hData;
                    jp->numEligProc += nSlots;
                }
                jp->numExecCandPtr = i;
            }
        }

        FREEUP(jp->execCandPtr);
        jp->execCandPtr = execCandPtr;
        if (JOB_CAN_BACKFILL(jp) && jobHasBackfillee(jp)) {

            getBackfillSlotsOnExecCandHost(jp);
            doBackfill(jp);
        }
    }

}

static void
removeCandHost(struct jData *jp, int i)
{
    int groupIdx, memberIdx;


    if (jp->groupCands != NULL ) {
        for (groupIdx=0; groupIdx<jp->numOfGroups; groupIdx++) {
            for (memberIdx=0; memberIdx<jp->groupCands[groupIdx].numOfMembers; memberIdx++) {
                if (jp->groupCands[groupIdx].members[memberIdx].hData->hostId
                    == jp->candPtr[i].hData->hostId ) {
                    removeCandHostFromCandPtr(&(jp->groupCands[groupIdx].members), &(jp->groupCands[groupIdx].numOfMembers), memberIdx);
                }
            }
            if (jp->groupCands[groupIdx].numOfMembers == 0 ) {

                SET_BIT(groupIdx, jp->inEligibleGroups);
            }
        }
    }
    removeCandHostFromCandPtr(&(jp->candPtr), &(jp->numCandPtr), i);

}

static bool_t
schedulerObserverSelect(void *extra, LIST_EVENT_T *event)
{
    if (mSchedStage & M_STAGE_QUE_CAND) {
        return TRUE;
    } else {

        return FALSE;
    }
}

static int
schedulerObserverEnter(LIST_T *list, void *extra, LIST_EVENT_T *event)
{
    struct jData *jp;
    int listNo;

    jp = (struct jData *)event->entry;
    if (JOB_IS_PROCESSED(jp)) {

        return 0;
    }
    if (list == (LIST_T *)jDataList[PJL]) {
        listNo = PJL;
    } else {
        listNo = MJL;
    }

    if (currentJob[MJL] == NULL && currentJob[PJL] == NULL &&
        newSession[MJL] && newSession[PJL]) {

        return 0;
    }
    if (listNo == MJL && currentJob[MJL] == NULL) {

        currentJob[MJL] = jp;
        return 0;
    }
    if (listNo == PJL && currentJob[PJL] == NULL) {

        return 0;
    }
    if ( ((jp->qPtr != currentJob[listNo]->qPtr)
          && j1IsBeforeJ2(jp, currentJob[listNo], (struct jData *)list))
         || ((jp->qPtr == currentJob[listNo]->qPtr)
             && j1IsBeforeJ2(jp, currentJob[listNo], (struct jData *)list)) ) {
        currentJob[listNo] = jp;
    }
    return 0;
}

static int
schedulerObserverLeave(LIST_T *list, void *extra, LIST_EVENT_T *event)
{
    struct jData *jp;
    int listNo;

    jp = (struct jData *)event->entry;
    if (list == (LIST_T *)jDataList[PJL]) {
        listNo = PJL;
    } else {
        listNo = MJL;
    }
    if (currentJob[listNo] == jp) {
        currentJob[listNo] = jp->back;
        if (END_OF_JOB_LIST(currentJob[listNo], listNo)) {
            currentJob[listNo] = NULL;
        }
    }
    return 0;
}

static int
j1IsBeforeJ2(struct jData *j1, struct jData *j2, struct jData *list)
{
    struct jData *jp;

    if (j1->qPtr->priority > j2->qPtr->priority) {
        return TRUE;
    } else if (j1->qPtr->priority < j2->qPtr->priority) {
        return FALSE;
    } else {

        for (jp = j1->back; jp != list; jp = jp->back) {

            if (jp->qPtr->priority < j1->qPtr->priority) {
                return FALSE;
            }
            if (jp == j2) {
                return TRUE;
            }
        }
        return FALSE;
    }
}


void
schedulerInit()
{
    static char fname[] = "schedulerInit";
    char myhostname[MAXHOSTNAMELEN], *myhostp = myhostname;
    static LIST_OBSERVER_T *schedulerObserverOnPJL,
        *schedulerObserverOnMJL,
        *queueObserverOnPJL,
        *queueObserverOnMJL,
        *queueObserverOnSJL;
    struct qData *qp;
    int i;

    mSchedStage = 0;

    for (qp = qDataList->back; qp != qDataList; qp = qp->back) {
        for (i = 0; i <= PJL; i++) {
            if (i != PJL && i != MJL && i != SJL) {
                continue;
            }
            setQueueFirstAndLastJob(qp, i);
            if (logclass & LC_SCHED) {
                if (qp->firstJob[i]) {
                    char tmpJobid[32];
                    strcpy(tmpJobid, lsb_jobid2str(qp->firstJob[i]->jobId));
                    ls_syslog(LOG_DEBUG3, "%s: queue <%s> list <%d> firstJob <%s> lastJob <%s>", fname, qp->queue, i, tmpJobid, lsb_jobid2str(qp->lastJob[i]->jobId));
                }
            }
        }
    }

    listAllowObservers((LIST_T *)jDataList[PJL]);
    listAllowObservers((LIST_T *)jDataList[MJL]);
    listAllowObservers((LIST_T *)jDataList[SJL]);
    schedulerObserverOnPJL = listObserverCreate("schedulerObserverOnPJL",
                                                NULL,
                                                &schedulerObserverSelect,
                                                LIST_EVENT_ENTER,
                                                &schedulerObserverEnter,
                                                LIST_EVENT_LEAVE,
                                                &schedulerObserverLeave,
                                                LIST_EVENT_NULL);
    schedulerObserverOnMJL = listObserverCreate("schedulerObserverOnMJL",
                                                NULL,
                                                &schedulerObserverSelect,
                                                LIST_EVENT_ENTER,
                                                &schedulerObserverEnter,
                                                LIST_EVENT_LEAVE,
                                                &schedulerObserverLeave,
                                                LIST_EVENT_NULL);
    queueObserverOnPJL = listObserverCreate("queueObserverOnPJL",
                                            NULL,
                                            NULL,
                                            LIST_EVENT_ENTER,
                                            &queueObserverEnter,
                                            LIST_EVENT_LEAVE,
                                            &queueObserverLeave,
                                            LIST_EVENT_NULL);
    queueObserverOnMJL = listObserverCreate("queueObserverOnMJL",
                                            NULL,
                                            NULL,
                                            LIST_EVENT_ENTER,
                                            &queueObserverEnter,
                                            LIST_EVENT_LEAVE,
                                            &queueObserverLeave,
                                            LIST_EVENT_NULL);
    queueObserverOnSJL = listObserverCreate("queueObserverOnSJL",
                                            NULL,
                                            NULL,
                                            LIST_EVENT_ENTER,
                                            &queueObserverEnter,
                                            LIST_EVENT_LEAVE,
                                            &queueObserverLeave,
                                            LIST_EVENT_NULL);

    if ((schedulerObserverOnPJL == NULL)
        || (schedulerObserverOnMJL == NULL)
        || (queueObserverOnPJL == NULL)
        || (queueObserverOnMJL == NULL)
        || (queueObserverOnSJL == NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "listObserverCreate");

        if (gethostname(myhostp, MAXHOSTNAMELEN) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "gethostname");
            strcpy(myhostp, "localhost");
        }
        die(MASTER_MEM);
    }
    listObserverAttach(schedulerObserverOnPJL, (LIST_T *)jDataList[PJL]);
    listObserverAttach(schedulerObserverOnMJL, (LIST_T *)jDataList[MJL]);
    listObserverAttach(queueObserverOnPJL, (LIST_T *)jDataList[PJL]);
    listObserverAttach(queueObserverOnMJL, (LIST_T *)jDataList[MJL]);
    listObserverAttach(queueObserverOnSJL, (LIST_T *)jDataList[SJL]);

}

static int
queueObserverEnter(LIST_T *list, void *extra, LIST_EVENT_T *event)
{
    struct jData *jp, *jList;
    int    listNo;

    jp = (struct jData *)event->entry;
    jList = (struct jData *)list;
    listNo = listNumber(jList);

    if (jobIsFirstOnSegment(jp, jList)) {
        jp->qPtr->firstJob[listNo] = jp;
    }
    if (jobIsLastOnSegment(jp, jList)) {
        jp->qPtr->lastJob[listNo] = jp;
    }
    updateQueueJobPtr(listNo, jp->qPtr);
    return 0;
}

static int
queueObserverLeave(LIST_T *list, void *extra, LIST_EVENT_T *event)
{
    struct jData *jp, *jList;
    int    listNo;

    jp = (struct jData *)event->entry;
    jList = (struct jData *)list;
    listNo = listNumber(jList);

    if (jobIsFirstOnSegment(jp, jList)) {
        jp->qPtr->firstJob[listNo] = nextJobOnSegment(jp, jList);
    }
    if (jobIsLastOnSegment(jp, jList)) {
        jp->qPtr->lastJob[listNo] = prevJobOnSegment(jp, jList);
    }
    updateQueueJobPtr(listNo, jp->qPtr);

    return 0;
}

static int
listNumber(struct jData *jList)
{
    static char fname[] = "listNumber";
    char myhostname[MAXHOSTNAMELEN], *myhostp = myhostname;

    if (jList == jDataList[MJL]) {
        return MJL;
    } else if (jList == jDataList[PJL]) {
        return PJL;
    } else if (jList == jDataList[SJL]) {
        return SJL;
    } else {

        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7236,
                                         "%s: internal error"), fname); /* catgets 7236 */

        if (gethostname(myhostp, MAXHOSTNAMELEN) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "gethostname");
            strcpy(myhostp, "localhost");
        }
        die(MASTER_FATAL);
    }


    return 0;

}

static int
jobIsFirstOnSegment(struct jData *jp, struct jData *jList)
{
    struct jData *prevJob;

    prevJob = jp->forw;
    if (prevJob == jList) {
        return TRUE;
    }
    if (jobsOnSameSegment(jp, prevJob, jList)) {
        return FALSE;
    } else {
        return TRUE;
    }
}

static int
jobIsLastOnSegment(struct jData *jp, struct jData *jList)
{
    struct jData *nextJob;

    nextJob = jp->back;
    if (nextJob == jList) {
        return TRUE;
    }
    if (jobsOnSameSegment(jp, nextJob, jList)) {
        return FALSE;
    } else {
        return TRUE;
    }
}

int
jobsOnSameSegment(struct jData *j1, struct jData *j2, struct jData *jList)
{
    if (j2->qPtr == j1->qPtr) {
        return TRUE;
    } else if (j2->qPtr->qAttrib & Q_ATTRIB_FS || j1->qPtr->qAttrib & Q_ATTRIB_FS) {
        return FALSE;
    } else {
        if (j2->qPtr->priority == j1->qPtr->priority) {
            return TRUE;
        } else {
            return FALSE;
        }
    }
}

static struct jData *
nextJobOnSegment(struct jData *jp, struct jData *jList)
{
    struct jData *nextJob;

    nextJob = jp->back;
    if (nextJob == jList) {
        return NULL;
    }
    if (jobsOnSameSegment(jp, nextJob, jList)) {
        return nextJob;
    } else {
        return NULL;
    }
}

static struct jData *
prevJobOnSegment(struct jData *jp, struct jData *jList)
{
    struct jData *prevJob;

    prevJob = jp->forw;
    if (prevJob == jList) {
        return NULL;
    }
    if (jobsOnSameSegment(jp, prevJob, jList)) {
        return prevJob;
    } else {
        return NULL;
    }
}

static void
setQueueFirstAndLastJob(struct qData *qp, int listNo)
{
    struct jData *jp;

    for (jp = jDataList[listNo]->back; jp != jDataList[listNo]; jp = jp->back) {
        if (jp->qPtr->priority == qp->priority) {
            if (jp->qPtr == qp) {
                qp->firstJob[listNo] = jp;
                break;
            } else {
                if (listNo == SJL) {
                    qp->firstJob[listNo] = jp;
                    break;
                } else {
                    qp->firstJob[listNo] = jp;
                    break;
                }
            }
        } else if (jp->qPtr->priority < qp->priority) {
            break;
        }
    }
    if (qp->firstJob[listNo] == NULL) {
        return;
    }
    for (jp = qp->firstJob[listNo]->back; jp != jDataList[listNo]; jp = jp->back) {
        if (jp->qPtr->priority < qp->priority) {
            qp->lastJob[listNo] = jp->forw;
            break;
        }
    }
    if (jp == jDataList[listNo]) {
        qp->lastJob[listNo] = jDataList[listNo]->forw;
    }
    return;
}


static int
numOfOccuranceOfHost(struct jData *jp, struct hData *host)
{
    int i, num = 0;

    for (i = 0; i < jp->numHostPtr; i++) {
        if (jp->hPtr[i] == host) {
            num++;
        }
    }
    return num;
}

static void
removeNOccuranceOfHost(struct jData *jp, struct hData *host, int num,
                       struct hData **newHPtr)
{
    int found = FALSE, index = 0, i = 0;

    while (i < jp->numHostPtr) {
        if (!found) {
            if (jp->hPtr[i] == host) {
                found = TRUE;

                i += num;
                continue;
            }
        }
        newHPtr[index++] = jp->hPtr[i++];
    }
}

static struct backfillee *
backfilleeCreate()
{
    static char fname[] = "backfilleeCreate";
    struct backfillee *ent;

    ent = (struct backfillee *)my_malloc(sizeof(struct backfillee), fname);
    ent->backfilleePtr = NULL;
    ent->indexInCandHostList = 0;
    ent->backfillSlots = 0;
    ent->numHostPtr = 0;
    ent->hPtr = NULL;

    return ent;
}

static struct backfillee *
backfilleeCreateByCopy(struct backfillee *bp)
{
    static char fname[] = "backfilleeCreateByCopy";
    struct backfillee *ent;

    ent = (struct backfillee *)my_malloc(sizeof(struct backfillee), fname);
    ent->backfilleePtr = bp->backfilleePtr;
    ent->indexInCandHostList = bp->indexInCandHostList;
    ent->backfillSlots = bp->backfillSlots;
    ent->numHostPtr = 0;
    ent->hPtr = NULL;

    return ent;
}

static void
sortBackfillee(struct jData *jp, LIST_T *sortedBackfilleeList)
{
    int i;
    LIST_ITERATOR_T iter;
    struct backfillee *entry, *newBackfillee;
    LIST_T *backfilleeList;

    for (i = 0; i < jp->numCandPtr; i++) {
        backfilleeList = jp->candPtr[i].backfilleeList;
        if (backfilleeList == NULL) {
            continue;
        }
        (void)listIteratorAttach(&iter, backfilleeList);
        for (entry = (struct backfillee *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, (LIST_ENTRY_T **)&entry)) {
            entry->indexInCandHostList = i;
            newBackfillee = backfilleeCreateByCopy(entry);
            insertIntoSortedBackfilleeList(jp, sortedBackfilleeList,
                                           newBackfillee);
        }
    }
}

static void
insertIntoSortedBackfilleeList(struct jData *jp, LIST_T *sortedBackfilleeList,
                               struct backfillee *item)
{
    LIST_ITERATOR_T iter;
    struct backfillee *entry;
    int diffItem, diffEntry, finishTimeOnItem, finishTimeOnEntry;

    finishTimeOnItem = now + RUN_LIMIT_OF_JOB(jp)/jp->candPtr[item->indexInCandHostList].hData->cpuFactor;
    diffItem = item->backfilleePtr->predictedStartTime - finishTimeOnItem;
    (void)listIteratorAttach(&iter, sortedBackfilleeList);
    for (entry = (struct backfillee *)listIteratorGetCurEntry(&iter);
         !listIteratorIsEndOfList(&iter);
         listIteratorNext(&iter, (LIST_ENTRY_T **)&entry)) {
        finishTimeOnEntry = now + RUN_LIMIT_OF_JOB(jp)/jp->candPtr[entry->indexInCandHostList].hData->cpuFactor;
        diffEntry = entry->backfilleePtr->predictedStartTime - finishTimeOnEntry;
        if (diffItem < diffEntry) {
            listInsertEntryBefore(sortedBackfilleeList,
                                  (LIST_ENTRY_T *)entry,
                                  (LIST_ENTRY_T *)item);
            return;
        }
    }
    listInsertEntryAtBack(sortedBackfilleeList, (LIST_ENTRY_T *)item);
}

static int
jobHasBackfillee(struct jData *jp)
{
    int i;

    for (i = 0; i < jp->numCandPtr; i++) {
        if (jp->candPtr[i].backfilleeList != NULL) {
            return TRUE;
        }
    }
    return FALSE;
}

static int
candHostInBackfillCandList(struct backfillCand *backfillCandPtr,
                           int numBackfillCand,
                           int candHostIndex)
{
    int j;

    for (j = 0; j < numBackfillCand; j++) {
        if (backfillCandPtr[j].indexInCandHostList == candHostIndex) {

            return TRUE;
        }
    }
    return FALSE;
}

static void
freeBackfillSlotsFromBackfillee(struct jData *jp)
{
    static char fname[] = "freeBackfillSlotsFromBackfillee";

    struct backfillSlotsData {
        struct backfillSlotsData *forw;
        struct backfillSlotsData *back;
        struct hData *hData;
        int backfillSlots;
    };
    LIST_T *backfilleeToFreeList;
    int i;
    LIST_ITERATOR_T iter, slotListIter;
    struct backfillee *backfillee;
    struct backfillSlotsData *backfillSlotsDataPtr;
    struct backfilleeData *backfilleeDataPtr, *backfilleeDataOnList;
    struct jData *job;
    struct hData **hPtrCopy, **tmpHPtr;
    int numHostPtrCopy, tmpNumHostPtr;

    backfilleeToFreeList = listCreate(NULL);

    for (i = 0; i < jp->numExecCandPtr; i++) {
        if (jp->execCandPtr[i].backfilleeList == NULL) {

            continue;
        }
        (void)listIteratorAttach(&iter, jp->execCandPtr[i].backfilleeList);
        for (backfillee = (struct backfillee *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, (LIST_ENTRY_T **)&backfillee)) {

            backfillSlotsDataPtr = (struct backfillSlotsData *)my_malloc(sizeof(struct backfillSlotsData), fname);
            backfillSlotsDataPtr->hData = jp->candPtr[backfillee->indexInCandHostList].hData;
            backfillSlotsDataPtr->backfillSlots = backfillee->backfillSlots;

            backfilleeDataPtr = (struct backfilleeData *)my_malloc(sizeof(struct backfilleeData), fname);
            backfilleeDataPtr->backfilleePtr = backfillee->backfilleePtr;

            backfilleeDataOnList =
                (struct backfilleeData *)listSearchEntry(backfilleeToFreeList,
                                                         backfilleeDataPtr,
                                                         backfilleeDataCmp,
                                                         0);
            if (backfilleeDataOnList == NULL) {

                backfilleeDataPtr->slotsList = listCreate(NULL);
                listInsertEntryAtBack(backfilleeDataPtr->slotsList,
                                      (LIST_ENTRY_T *)backfillSlotsDataPtr);
                listInsertEntryAtBack(backfilleeToFreeList,
                                      (LIST_ENTRY_T *)backfilleeDataPtr);
            } else {
                free(backfilleeDataPtr);
                listInsertEntryAtBack(backfilleeDataOnList->slotsList,
                                      (LIST_ENTRY_T *)backfillSlotsDataPtr);
            }
        }
    }
    (void)listIteratorAttach(&iter, backfilleeToFreeList);
    for (backfilleeDataPtr = (struct backfilleeData *)listIteratorGetCurEntry(&iter);
         !listIteratorIsEndOfList(&iter);
         listIteratorNext(&iter, (LIST_ENTRY_T **)&backfilleeDataPtr)) {

        job = backfilleeDataPtr->backfilleePtr;

        hPtrCopy = (struct hData **)my_malloc(sizeof(struct hData *) * job->numHostPtr, fname);
        for (i = 0; i < job->numHostPtr; i++) {
            hPtrCopy[i] = job->hPtr[i];
        }
        numHostPtrCopy = job->numHostPtr;

        tmpHPtr = (struct hData **)my_malloc(job->numHostPtr * sizeof(struct hData *), fname);

        (void)listIteratorAttach(&slotListIter, backfilleeDataPtr->slotsList);
        for (backfillSlotsDataPtr = (struct backfillSlotsData *)listIteratorGetCurEntry(&slotListIter);
             !listIteratorIsEndOfList(&slotListIter);
             listIteratorNext(&slotListIter, (LIST_ENTRY_T **)&backfillSlotsDataPtr)) {
            removeNOccuranceOfHost(job, backfillSlotsDataPtr->hData,
                                   backfillSlotsDataPtr->backfillSlots, tmpHPtr);
            job->numHostPtr -= backfillSlotsDataPtr->backfillSlots;

            for (i = 0; i < job->numHostPtr; i++) {
                job->hPtr[i] = tmpHPtr[i];
            }
            if (logclass & LC_SCHED) {
                ls_syslog(LOG_DEBUG2, "%s: free <%d> backfill slots on host <%s> of backfillee job <%s> for backfiller job <%s>", fname, backfillSlotsDataPtr->backfillSlots, backfillSlotsDataPtr->hData->host, lsb_jobid2str(job->jobId), lsb_jobid2str(jp->jobId));
            }
        }

        tmpNumHostPtr = job->numHostPtr;


        job->hPtr = hPtrCopy;
        job->numHostPtr = numHostPtrCopy;

        freeReserveSlots(job);

        job->numHostPtr = tmpNumHostPtr;
        if (tmpNumHostPtr != 0) {

            job->hPtr = tmpHPtr;
            reserveSlots(job);
        } else {

            job->hPtr = NULL;
            free(tmpHPtr);
            job->reserveTime = 0;
            job->slotHoldTime = 0;
        }
        listDestroy(backfilleeDataPtr->slotsList, NULL);
    }
    listDestroy(backfilleeToFreeList, NULL);
}

static bool_t
backfilleeDataCmp(void *ent, void *subject, int hint)
{
    struct backfilleeData *entP = (struct backfilleeData *)ent;
    struct backfilleeData *subjectP = (struct backfilleeData *)subject;

    if (entP->backfilleePtr == subjectP->backfilleePtr) {
        return TRUE;
    } else {
        return FALSE;
    }
}

static void
removeBackfillSlotsFromBackfiller(struct jData *jp)
{
    static char fname[] = "removeBackfillSlotsFromBackfiller";
    LIST_ITERATOR_T iter;
    int i, backfillSlots, totalSlots = 0, totalAvailSlots = 0;
    struct backfillee *backfillee;

    for (i = 0; i < jp->numExecCandPtr; i++) {
        int indexInCandPtr;
        if (jp->execCandPtr[i].backfilleeList == NULL) {
            continue;
        }

        for (indexInCandPtr = 0; indexInCandPtr < jp->numCandPtr;
             indexInCandPtr++) {
            if (jp->execCandPtr[i].hData == jp->candPtr[indexInCandPtr].hData) {
                break;
            }
        }
        (void)listIteratorAttach(&iter, jp->execCandPtr[i].backfilleeList);
        for (backfillee = (struct backfillee *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, (LIST_ENTRY_T **)&backfillee)) {
            int numSlotsToRemove, numAvailSlotsToRemove,
                numNonBackfillSlotsUsed, numAvailNonBackfillSlotsUsed,
                numNonBackfillSlotsUnused, numAvailNonBackfillSlotsUnused;
            backfillSlots = backfillee->backfillSlots;


            numNonBackfillSlotsUsed = jp->execCandPtr[i].numSlots -
                backfillSlots;
            numAvailNonBackfillSlotsUsed = jp->execCandPtr[i].numAvailSlots -
                backfillSlots;

            numNonBackfillSlotsUnused =
                jp->candPtr[indexInCandPtr].numNonBackfillSlots -
                numNonBackfillSlotsUsed;
            numAvailNonBackfillSlotsUnused =
                jp->candPtr[indexInCandPtr].numAvailNonBackfillSlots -
                numAvailNonBackfillSlotsUsed;

            numSlotsToRemove =
                MAX(0, backfillSlots - numNonBackfillSlotsUnused);
            numAvailSlotsToRemove =
                MAX(0, backfillSlots - numAvailNonBackfillSlotsUnused);
            jp->execCandPtr[i].numSlots -= numSlotsToRemove;
            jp->execCandPtr[i].numAvailSlots -= numAvailSlotsToRemove;
            totalSlots += numSlotsToRemove;
            totalAvailSlots += numAvailSlotsToRemove;
            if (logclass & LC_SCHED) {
                ls_syslog(LOG_DEBUG2, "%s: remove <%d> backfill slots on host <%s> (borrowed from backfillee job <%s>) from backfiller job <%s>", fname, backfillSlots, jp->execCandPtr[i].hData->host, lsb_jobid2str(backfillee->backfilleePtr->jobId), lsb_jobid2str(jp->jobId));
            }
        }
    }
    jp->numEligProc -= totalSlots;
    jp->numAvailEligProc -= totalAvailSlots;
}

static void
getBackfillSlotsOnExecCandHost(struct jData *jp)
{
    int i, nSlots;
    LIST_ITERATOR_T iter;
    struct backfillee *backfillee, *newBackfillee;

    for (i = 0; i < jp->numExecCandPtr; i++) {
        if (jp->candPtr[i].backfilleeList == NULL) {
            continue;
        }
        nSlots = 0;
        (void)listIteratorAttach(&iter, jp->candPtr[i].backfilleeList);
        for (backfillee = (struct backfillee *)listIteratorGetCurEntry(&iter);
             !listIteratorIsEndOfList(&iter);
             listIteratorNext(&iter, (LIST_ENTRY_T **)&backfillee)) {
            newBackfillee = backfilleeCreateByCopy(backfillee);

            newBackfillee->backfillSlots =
                MIN(jp->execCandPtr[i].numAvailSlots - nSlots,
                    newBackfillee->backfillSlots);
            newBackfillee->indexInCandHostList = i;
            nSlots += newBackfillee->backfillSlots;
            if (jp->execCandPtr[i].backfilleeList == NULL) {
                jp->execCandPtr[i].backfilleeList = listCreate(NULL);
            }
            listInsertEntryAtBack(jp->execCandPtr[i].backfilleeList,
                                  (LIST_ENTRY_T *)newBackfillee);
            if (nSlots == jp->execCandPtr[i].numAvailSlots) {

                break;
            }
        }
    }
}

static void
doBackfill(struct jData *jp)
{
    if (jp->numEligProc >= jp->shared->jobBill.numProcessors) {

        freeBackfillSlotsFromBackfillee(jp);
    } else {
        removeBackfillSlotsFromBackfiller(jp);
    }

}

static void
deallocExecCandPtr(struct jData *jp)
{
    int j;

    if (jp->numExecCandPtr != 0) {
        for (j = 0; j < jp->numExecCandPtr; j++) {
            DESTROY_BACKFILLEE_LIST(jp->execCandPtr[j].backfilleeList);
        }
    }


    FREEUP(jp->execCandPtr);
    jp->numExecCandPtr = 0;

}

static int
jobCantFinshBeforeDeadline(struct jData *jpbw, time_t deadline)
{
    int runLimit;


    if (!IGNORE_DEADLINE(jpbw->qPtr)) {
        runLimit = RUN_LIMIT_OF_JOB(jpbw);
        if (runLimit <= 0) {

            runLimit =
                CPU_LIMIT_OF_JOB(jpbw)/jpbw->shared->jobBill.maxNumProcessors;
        }
        if (runLimit > 0 &&
            CANT_FINISH_BEFORE_DEADLINE(runLimit, deadline, maxCpuFactor)) {
            return TRUE;
        }
    }
    return FALSE;
}

static int
imposeDCSOnJob(struct jData *jp, time_t *deadlinePtr, int *isWinDeadlinePtr,
               int *runLimitPtr)
{
    time_t deadline;
    int isWinDeadline, runLimit;

    if (IGNORE_DEADLINE(jp->qPtr))
        return FALSE;

    runLimit = RUN_LIMIT_OF_JOB(jp);
    if (runLimit <= 0) {
        runLimit =
            CPU_LIMIT_OF_JOB(jp)/jp->shared->jobBill.numProcessors;
    }
    if ((runLimit > 0) &&
        (jp->qPtr->runWinCloseTime > 0 ||
         jp->shared->jobBill.termTime > 0)) {

        if (jp->qPtr->runWinCloseTime > 0 &&
            jp->shared->jobBill.termTime > 0) {

            if (jp->qPtr->runWinCloseTime < jp->shared->jobBill.termTime) {
                isWinDeadline = TRUE;
                deadline = jp->qPtr->runWinCloseTime;
            } else {
                isWinDeadline = FALSE;
                deadline = jp->shared->jobBill.termTime;
            }
        } else if (jp->qPtr->runWinCloseTime > 0) {

            isWinDeadline = TRUE;
            deadline = jp->qPtr->runWinCloseTime;
        } else {

            isWinDeadline = FALSE;
            deadline = jp->shared->jobBill.termTime;
        }
        *deadlinePtr = deadline;
        *isWinDeadlinePtr = isWinDeadline;
        *runLimitPtr = runLimit;
        return TRUE;
    }

    return FALSE;
}

static void
updateQueueJobPtr(int listNo, struct qData *qp)
{
    struct qData *qPtr;

    for (qPtr = qDataList->forw; qPtr != qDataList; qPtr = qPtr->forw) {
        if (qPtr->priority != qp->priority ||
            qPtr == qp) {
            continue;
        }
        qPtr->firstJob[listNo] = qp->firstJob[listNo];
        qPtr->lastJob[listNo] = qp->lastJob[listNo];
    }
}

static void
copyReason(void)
{
    struct qData *qp;
    sTab stab;
    hEnt *e;
    int cc;

    cc = numofhosts() + 1;
    memcpy(hReasonTb[0], hReasonTb[1], cc * sizeof(int));

    for (qp = qDataList->forw; qp != qDataList; qp = qp->forw) {
        if (qp->reasonTb == NULL)
            continue;
        memcpy(qp->reasonTb[0], qp->reasonTb[1], cc * sizeof(int));
    }

    e = h_firstEnt_(&uDataList, &stab);
    while (e) {
        struct uData *up = e->hData;
        memcpy(up->reasonTb[0], up->reasonTb[1], cc * sizeof(int));
        e = h_nextEnt_(&stab);
    }
}

static void
clearJobReason(void)
{
    struct jData *jp;
    int i;

    for (i = MJL; i <= PJL; i++) {
        for (jp = jDataList[i]->back; jp != jDataList[i]; jp = jp->back) {

            if (jp->jFlags & JFLAG_READY2) {
                if (jp->qPtr->reasonTb[1][0]) {

                    jp->newReason = jp->qPtr->reasonTb[1][0];
                } else {
                    jp->newReason = 0;
                }
                jp->oldReason = jp->newReason;
            }
        }
    }
}

static int
notDefaultOrder (struct resVal *resVal)
{
    if (resVal == NULL)
        return FALSE;

    if (resVal->nphase != 2)
        return TRUE;


    if (resVal->order[0] == R15S && resVal->order[1] == PG)
        return FALSE;
    return TRUE;

}


static bool_t
enoughMaxUsableSlots(struct jData *jp)
{
    int numMaxUsableSlots;
    struct hData *hPtr;

    numMaxUsableSlots = 0;
    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        if (hPtr->hStatus & HOST_STAT_REMOTE)
            continue;

        if (hPtr->hStatus & HOST_STAT_NO_LIM)
            continue;
        if (hPtr->hStatus & HOST_STAT_UNREACH)
            continue;
        if (hPtr->hStatus & HOST_STAT_UNAVAIL)
            continue;
        if (!isHostQMember(hPtr, jp->qPtr))
            continue;
        if (jp->numAskedPtr && !OTHERS_IS_IN_ASKED_HOST_LIST(jp) &&
            !isAskedHost (hPtr, jp))
            continue;

        if (jp->qPtr->resValPtr) {
            int num = 1, noUse;
            if (!getHostsByResReq (jp->qPtr->resValPtr, &num, &hPtr, NULL,
                                   NULL, &noUse))
                continue;
        }
        if (jp->shared->resValPtr) {
            int num = 1, noUse;
            if (!getHostsByResReq (jp->shared->resValPtr, &num, &hPtr, NULL,
                                   NULL, &noUse))
                continue;
        }

        if (!jp->qPtr->resValPtr && !jp->shared->resValPtr &&
            jp->numAskedPtr == 0) {
            if (strcmp (jp->schedHost, hPtr->hostType) != 0)
                continue;
        }

        if (HAS_JOB_LEVEL_SPAN(jp)) {
            getSlotsUsableToSpan(jp, hPtr, TRUE, &numMaxUsableSlots);
        } else if (HAS_QUEUE_LEVEL_SPAN(jp)) {
            getSlotsUsableToSpan(jp, hPtr, FALSE, &numMaxUsableSlots);
        } else {
            numMaxUsableSlots += jobMaxUsableSlotsOnHost(jp, hPtr);
        }
    }

    if (numMaxUsableSlots >= jp->shared->jobBill.numProcessors) {
        return TRUE;
    }

    return FALSE;
}

void
setExecHostsAcceptInterval(struct jData* jp)
{
    static char fname[] = "setExecHostAcceptInterval";
    int                    i;
    int                    hostId;
    struct qData*          qp;

    for (i = 0; i < jp->numHostPtr; i++) {

        hostId = jp->hPtr[i]->hostId;

        for (qp = qDataList->back; qp != qDataList; qp = qp->back) {

            if ((jp->qPtr->acceptIntvl > 0
                 || jp->hPtr[i]->numDispJobs >= maxJobPerSession )) {

                if (OUT_SCHED_RS(qp->reasonTb[1][hostId]) == FALSE) {
                    qp->reasonTb[1][hostId] = PEND_HOST_ACCPT_ONE;
                }

                qp->numUsable--;
            }

            if (logclass & (LC_SCHED)) {
                ls_syslog(LOG_DEBUG2,"\
%s: qp->reasonTb[1][%s/%d]=%d qp->numUsable=%d",
                          fname, jp->hPtr[i]->host, hostId,
                          qp->reasonTb[1][hostId], qp->numUsable);
            }
        }
    }
}

static int
jobMaxUsableSlotsOnHost(struct jData *jp, struct hData *host)
{
    int slimit, i;


    slimit = MIN(host->maxJobs, host->uJobLimit);


    slimit = MIN(slimit, jp->uPtr->maxJobs);

    if (jp->uPtr->pJobLimit != INFINIT_FLOAT)
        slimit = MIN(slimit, jp->uPtr->pJobLimit * host->numCPUs);


    for (i = 0; i < jp->uPtr->numGrpPtr; i++) {
        struct uData *ugp = jp->uPtr->gPtr[i];
        slimit = MIN(slimit, ugp->maxJobs);
        if (ugp->pJobLimit != INFINIT_FLOAT) {
            slimit = MIN(slimit, ugp->pJobLimit * host->numCPUs);
        }
    }


    slimit = MIN(slimit, jp->qPtr->maxJobs);

    slimit = MIN(slimit, jp->qPtr->hJobLimit);

    if (jp->qPtr->pJobLimit != INFINIT_FLOAT)
        slimit = MIN(slimit, jp->qPtr->pJobLimit * host->numCPUs);

    slimit = MIN(slimit, jp->qPtr->uJobLimit);


    slimit = MIN(slimit, host->numCPUs);

    return slimit;
}

static void
hostHasEnoughSlots(struct jData *jPtr,
                   struct hData *hPtr,
                   int numSlots,
                   int requestedProcs,
                   int reason,
                   int *hreason)
{
    int   slots;

    slots = jobMaxUsableSlotsOnHost(jPtr, hPtr);
    if (slots >= requestedProcs) {
        /*
         * if (HOST_HAS_ENOUGH_PROCS(jp, host, requestedProcs)) {
         */
        if (numSlots < requestedProcs) {

            addReason(jPtr, hPtr->hostId, reason);
        }

    } else {
        *hreason = reason;
    }
}

static void
checkHostUsableToSpan(struct jData *jp, struct hData *host, int isJobLevel,
                      int *numSlots, int *hreason)
{
    int hasSpanPtile, hasSpanHosts;
    int requestsProcs;
    int reason;

    if (isJobLevel) {
        hasSpanPtile = HAS_JOB_LEVEL_SPAN_PTILE(jp);
        hasSpanHosts = HAS_JOB_LEVEL_SPAN_HOSTS(jp);
        reason = PEND_JOB_NO_SPAN;
    } else {
        hasSpanPtile = HAS_QUEUE_LEVEL_SPAN_PTILE(jp);
        hasSpanHosts = HAS_QUEUE_LEVEL_SPAN_HOSTS(jp);
        reason = PEND_QUE_NO_SPAN;
    }
    if (hasSpanPtile) {
        if (isJobLevel) {
            requestsProcs = JOB_LEVEL_SPAN_PTILE(jp);
        } else {
            requestsProcs = QUEUE_LEVEL_SPAN_PTILE(jp);
        }
        hostHasEnoughSlots(jp, host, *numSlots, requestsProcs, reason,
                           hreason);

        *numSlots = MIN(*numSlots, requestsProcs);
    } else if (hasSpanHosts) {
        requestsProcs = jp->shared->jobBill.numProcessors;
        hostHasEnoughSlots(jp, host, *numSlots, requestsProcs, reason,
                           hreason);
    }
}

static void
reshapeCandHost(struct jData *jp, struct candHost *candHosts, int *numJUsable)
{
    int i;
    int numRequestHosts;
    int pTile;
    int numCandHosts;

    if (HAS_JOB_LEVEL_SPAN_PTILE(jp) || (!HAS_JOB_LEVEL_SPAN(jp) &&
                                         HAS_QUEUE_LEVEL_SPAN_PTILE(jp))) {
        if (HAS_JOB_LEVEL_SPAN_PTILE(jp)) {
            pTile = JOB_LEVEL_SPAN_PTILE(jp);
        } else {
            pTile = QUEUE_LEVEL_SPAN_PTILE(jp);
        }

        numRequestHosts = jp->shared->jobBill.numProcessors/pTile;
        if (jp->shared->jobBill.numProcessors % pTile) {
            numRequestHosts++;
        }

        numCandHosts = 0;
        for (i = 0; i < *numJUsable; i++) {
            if (candHosts[i].numSlots >= pTile) {

                exchangeHostPos(candHosts, i, numCandHosts);
                numCandHosts++;
            }
        }

        if (lsbPtilePack == TRUE) {
            int      pTileRemain;


            pTileRemain = jp->shared->jobBill.numProcessors % pTile;

            if (pTileRemain
                && (numCandHosts >= (numRequestHosts - 1)) ) {

                for (i = numCandHosts; i < *numJUsable; i++) {
                    if (candHosts[i].numSlots < pTile
                        && candHosts[i].numSlots >= pTileRemain) {


                        exchangeHostPos(candHosts, i, numRequestHosts - 1);
                        break;
                    }
                }
            }
        }


        handleFirstHost(jp,*numJUsable,candHosts);


        for (i = numRequestHosts; i < *numJUsable; i++) {

            DESTROY_BACKFILLEE_LIST(candHosts[i].backfilleeList);
        }
        *numJUsable = MIN(numRequestHosts, *numJUsable);
    }
}

static void
getSlotsUsableToSpan(struct jData *jp, struct hData *host, int isJobLevel,
                     int *numMaxUsableSlots)
{
    int pTile;
    int hasSpanPtile, hasSpanHosts;

    if (isJobLevel) {
        hasSpanPtile = HAS_JOB_LEVEL_SPAN_PTILE(jp);
        hasSpanHosts = HAS_JOB_LEVEL_SPAN_HOSTS(jp);
    } else {
        hasSpanPtile = HAS_QUEUE_LEVEL_SPAN_PTILE(jp);
        hasSpanHosts = HAS_QUEUE_LEVEL_SPAN_HOSTS(jp);
    }
    if (hasSpanPtile) {
        if (isJobLevel) {
            pTile = JOB_LEVEL_SPAN_PTILE(jp);
        } else {
            pTile = QUEUE_LEVEL_SPAN_PTILE(jp);
        }
        if (HOST_HAS_ENOUGH_PROCS(jp, host, pTile)) {
            *numMaxUsableSlots += MIN(pTile,
                                      jobMaxUsableSlotsOnHost(jp, host));
        }
    } else if (hasSpanHosts) {
        if (HOST_HAS_ENOUGH_PROCS(jp, host,
                                  jp->shared->jobBill.numProcessors)) {

            *numMaxUsableSlots = MAX(*numMaxUsableSlots,
                                     jobMaxUsableSlotsOnHost(jp, host));
        }
    }
}

static void
exchangeHostPos(struct candHost *candH, int pos1, int pos2)
{
    struct candHost saveH;

    if (pos1 < 0 || pos2 < 0 || pos1 == pos2) {
        return;
    }

    saveH = candH[pos1];
    candH[pos1] = candH[pos2];
    candH[pos2] = saveH;

    return;
}

static int
totalBackfillSlots(LIST_T *theBackfilleeList)
{
    LIST_ITERATOR_T iter;
    struct backfillee *backfilleeListEntry;
    int numBackfillSlots = 0;

    if (theBackfilleeList == NULL) {
        return 0;
    }
    (void)listIteratorAttach(&iter, theBackfilleeList);
    for (backfilleeListEntry =
             (struct backfillee *)listIteratorGetCurEntry(&iter);
         !listIteratorIsEndOfList(&iter);
         listIteratorNext(&iter, (LIST_ENTRY_T **)&backfilleeListEntry)) {
        numBackfillSlots += backfilleeListEntry->backfillSlots;
    }
    return numBackfillSlots;
}


static void
resetSchedulerSession(void)
{
    struct jRef *jR;
    struct jRef *jR0;

    copyReason();
    mSchedStage = 0;
    clearJobReason();

    for (jR = (struct jRef *)jRefList->back;
         jR != (void *)jRefList; ) {

        jR0 = jR->back;
        listRemoveEntry(jRefList, (LIST_ENTRY_T *)jR);
        if (jR->type == TYPE_JOB_SET) {
            sch_fs_freeJobSet(jR->job);    
        }
        free(jR);
        jR = jR0;
    }

    DUMP_TIMERS(__func__);
    DUMP_CNT();
    RESET_CNT();

}

static int
handleFirstHost(struct jData *jpbw, int numCandPtr, struct candHost * candPtr)
{
    int i;
    int firstHostId = 0;



    if ((firstHostId = needHandleFirstHost(jpbw)) != 0 ) {

        int foundFirstHostInCandPtr = FALSE;

        for (i=0; i < numCandPtr; i++) {
            if (firstHostId == candPtr[i].hData->hostId ) {
                foundFirstHostInCandPtr = TRUE;

                moveHostPos(candPtr, i, 0 );
                break;
            }
        }
        if ( !foundFirstHostInCandPtr )
            return firstHostId;
        else
            return 0;
    } else {
        return 0;
    }
}


static int
needHandleFirstHost(struct jData* jp)
{
#define FIRST_HOST_PRIORITY   (unsigned)-1/2

    if (jp->numAskedPtr > 0 ) {
        if (jp->askedPtr[0].priority == FIRST_HOST_PRIORITY ) {
            return jp->askedPtr[0].hData->hostId;
        }
    }

    if ((jp->numAskedPtr == 0 ) && (jp->qPtr->numAskedPtr > 0 )) {
        if ( jp->qPtr->askedPtr[0].priority == FIRST_HOST_PRIORITY ) {
            return jp->qPtr->askedPtr[0].hData->hostId;
        }
    }
    return 0;
}

void
resetStaticSchedVariables(void)
{
    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG, "resetStaticSchedVariables: Entering this routine...");
    }
    getPeerCand1(NULL, NULL);
    getJUsable(NULL, NULL, NULL);
}

static bool_t
jobIsReady(struct jData *jp)
{
    static char fname[] = "jobIsReady()";
    int ret;


    ret = checkIfJobIsReady(jp);

    jp->processed |= JOB_STAGE_READY;
    if (logclass & LC_SCHED) {
        ls_syslog(LOG_DEBUG3, "%s: job=%s numSlots=%d numAvailSlots=%d", fname,
                  lsb_jobid2str(jp->jobId), jp->numSlots, jp->numAvailSlots);
    }
    if (ret) {
        jp->jFlags |= JFLAG_READY;
        return TRUE;
    } else {
        if (logclass & (LC_SCHED | LC_PEND)) {
            ls_syslog(LOG_DEBUG1, "%s: job <%s> is not ready", fname, lsb_jobid2str(jp->jobId));
        }
        jp->processed |= JOB_STAGE_DONE;
        return FALSE;
    }
}

int
reservePreemptResourcesForHosts(struct jData *jp)
{
    static char         fname[] = "reservePreemptResourcesForHosts";
    struct hData **hosts = (struct hData **) my_calloc (jp->numHostPtr,
                                                        sizeof (struct hData *), fname);
    int numh=0;
    int i;
    for (i = 0; i < jp->numHostPtr; i++) {
        hosts[numh] = jp->hPtr[i];
        numh++;
    }
    return reservePreemptResources(jp, numh, hosts);
}

int
reservePreemptResourcesForExecCands(struct jData *jp)
{
    static char         fname[] = "reservePreemptResourcesForExecCands";
    struct hData **hosts = (struct hData **) my_calloc (jp->numExecCandPtr,
                                                        sizeof (struct hData *), fname);
    int numh=0;
    int i;
    for (i = 0; i < jp->numExecCandPtr; i++) {
        if (jp->execCandPtr[i].numSlots <= 0)
            continue;
        hosts[numh] = jp->execCandPtr[i].hData;
        numh++;
    }
    return reservePreemptResources(jp, numh, hosts);
}

int
reservePreemptResources(struct jData *jp, int numHosts, struct hData **hosts)
{
    static char         fname[] = "reservePreemptResources";
    struct resVal              *resValPtr;
    if ((resValPtr = getReserveValues(jp->shared->resValPtr,
                                      jp->qPtr->resValPtr)) == NULL) {
        FREEUP(hosts);
        if (logclass & (LC_SCHED))
            ls_syslog (LOG_DEBUG3, "%s: No resources required; job <%s>", fname, lsb_jobid2str(jp->jobId));
        return 0;
    }

    if (logclass & (LC_SCHED))
        printPRMOValues();
    if (markPreemptForPRHQValues(resValPtr, numHosts, hosts, jp->qPtr) !=0 ) {
        FREEUP(hosts);
        if (logclass & (LC_SCHED))
            ls_syslog (LOG_DEBUG3, "%s: Failed to reserved resources; job <%s>", fname, lsb_jobid2str(jp->jobId));
        return -1;
    }
    if (logclass & (LC_SCHED))
        printPRMOValues();
    jp->rsrcPreemptHPtr = hosts;
    jp->numRsrcPreemptHPtr = numHosts;
    jp->jStatus |= JOB_STAT_RSRC_PREEMPT_WAIT;
    if (logclass & (LC_SCHED))
        ls_syslog (LOG_DEBUG3, "%s: Reserved resources; job <%s>", fname, lsb_jobid2str(jp->jobId));
    return 0;
}

int
freeReservePreemptResources(struct jData *jp)
{
    static char         fname[] = "freeReservePreemptResources";
    int hostn, resn;
    float val;
    FORALL_PRMPT_HOST_RSRCS(hostn, resn, val, jp) {
        removeReservedByWaitPRHQValue(resn, val, jp->rsrcPreemptHPtr[hostn], jp->qPtr);
        if (logclass & (LC_SCHED))
            ls_syslog (LOG_DEBUG3, "%s: Freed reserved resource; job <%s>, res <%s> host <%s>", fname, lsb_jobid2str(jp->jobId), allLsInfo->resTable[resn].name, jp->rsrcPreemptHPtr[hostn]->host);
    } ENDFORALL_PRMPT_HOST_RSRCS;
    deallocReservePreemptResources(jp);
    return 0;
}

int
deallocReservePreemptResources(struct jData *jp)
{
    jp->jStatus &= ~JOB_STAT_RSRC_PREEMPT_WAIT;
    if (jp->numRsrcPreemptHPtr > 0) {
        FREEUP (jp->rsrcPreemptHPtr);
    }
    jp->numRsrcPreemptHPtr = 0;
    return 0;
}

void
updPreemptResourceByRUNJob(struct jData *jp)
{
    int hostn, resn;
    float val;


    if (!(jp->jStatus & JOB_STAT_RUN)) {
        return;
    }
    if (MARKED_WILL_BE_PREEMPTED(jp)) {
        return;
    }

    if (CANNOT_BE_PREEMPTED_FOR_RSRC(jp)) {
        return;
    }

    if (!jp->numHostPtr || jp->hPtr == NULL) {
        return;
    }


    for (hostn = 0; hostn < jp->numHostPtr; hostn++) {
        if (jp->hPtr[hostn]->hStatus & HOST_STAT_UNAVAIL) {
            continue;
        }
        FORALL_PRMPT_RSRCS(resn) {
            GET_RES_RSRC_USAGE(resn, val, jp->shared->resValPtr,
                               jp->qPtr->resValPtr);
            if (val <= 0.0)
                continue;

            addRunJobUsedPRHQValue(resn, val, jp->hPtr[hostn], jp->qPtr);
            if (logclass & (LC_SCHED))
                printPRMOValues();

        } ENDFORALL_PRMPT_RSRCS;

    }
    return;
}

void
checkAndReserveForPreemptWait(struct jData *jp)
{

    return;
}

int
markPreemptForPRHQValues(struct resVal *resValPtr, int numHosts,
                         struct hData **hPPtr, struct qData *qPtr)
{
    int resn, hostn;

    if (numHosts == 0 || hPPtr == NULL)
        return 0;



    for (hostn = 0;
         hostn == 0 || (slotResourceReserve && hostn < numHosts) ;
         hostn++) {
        FORALL_PRMPT_RSRCS(resn) {
            float needPreempt, usable, val;
            struct resourceInstance *instance;
            GET_RES_RSRC_USAGE(resn, val, resValPtr, qPtr->resValPtr);
            if (val <= 0.0)
                continue;

            usable = getUsablePRHQValue(resn, hPPtr[hostn], qPtr, &instance)
                - getReservedByWaitPRHQValue(resn, hPPtr[hostn], qPtr);
            if (usable > val) {
                addReservedByWaitPRHQValue(resn, val, hPPtr[hostn], qPtr);
                continue;
            }
            if (usable > 0.0) {
                addReservedByWaitPRHQValue(resn, usable, hPPtr[hostn], qPtr);
            }

            needPreempt = takeAvailableByPreemptPRHQValue(resn,
                                                          val - usable, hPPtr[hostn], qPtr);
            if (needPreempt <= 0.0) {
                continue;
            }

            if (markPreemptForPRHQInstance(resn, needPreempt, hPPtr[hostn],
                                           qPtr) != 0) {
                return -1;
            }
        } ENDFORALL_PRMPT_RSRCS;
    }
    return 0;
}

int
markPreemptForPRHQInstance(int needResN, float needVal,
                           struct hData *needHost, struct qData *needQPtr)
{
    return -1;
}


static enum candRetCode
handleXor(struct jData *jpbw)
{
    static char fname[] = "handleXor";
    int i, j, foundGroup, numXorExprs;
    int *indicesOfCandPtr;
    struct resVal* resValPtr;
    struct candHost *candPtr;
    int numCandPtr;
    struct tclHostData tclHostData;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG3, "%s: Entering for job <%s>", fname, lsb_jobid2str(jpbw->jobId));
    }


    if (jpbw->shared->resValPtr
        && (jpbw->shared->resValPtr->xorExprs !=NULL )) {
        resValPtr = jpbw->shared->resValPtr;
    } else {
        if (jpbw->qPtr->resValPtr
            && (jpbw->qPtr->resValPtr->xorExprs != NULL)) {
            resValPtr = jpbw->qPtr->resValPtr;
        }
    }


    if (logclass & (LC_TRACE | LC_SCHED)) {
        ls_syslog(LOG_DEBUG3, "%s: Before filtering with xor select, the candidate hosts are: ", fname);
        for (i=0; i<jpbw->numCandPtr; i++)
            ls_syslog(LOG_DEBUG3, "%s: candPtr[%d]=%s.",fname, i, jpbw->candPtr[
                          i].hData->host);
    }

    jpbw->groupCands = NULL;
    foundGroup = 0;
    numXorExprs=0;


    if (resValPtr->xorExprs) {
        while (resValPtr->xorExprs[numXorExprs]) {
            numXorExprs++;
        }
    }

    jpbw->numOfGroups = numXorExprs;
    inEligibleGroupsInit(&jpbw->inEligibleGroups, jpbw->numOfGroups);

    jpbw->groupCands = (struct groupCandHosts *)my_malloc(numXorExprs * sizeof(struct groupCandHosts), fname);
    for (j = 0; j < numXorExprs; j++) {
        groupCandHostsInit(&jpbw->groupCands[j]);
    }
    indicesOfCandPtr = (int *) my_calloc (jpbw->numCandPtr, sizeof (int), fname);

    for (j = 0; j < numXorExprs; j++) {
        numCandPtr = 0;
        candPtr = NULL;


        for (i = 0; i<jpbw->numCandPtr; i++) {
            getTclHostData(&tclHostData, jpbw->candPtr[i].hData, NULL);
            if (evalResReq (resValPtr->xorExprs[j], &tclHostData, FALSE) > 0 ) {
                indicesOfCandPtr[numCandPtr] = i;
                numCandPtr++;
            }
            freeTclHostData(&tclHostData);
        }

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG3, "%s: found %d hosts with expr %s", fname, numCandPtr, resValPtr->xorExprs[j]);
        }

        if (numCandPtr != 0 ) {

            candPtr = (struct candHost *) my_calloc (numCandPtr,
                                                     sizeof (struct candHost), fname);
            for (i=0; i<numCandPtr; i++) {
                copyCandHostData(&(candPtr[i]),&(jpbw->candPtr[indicesOfCandPtr[
                                                         i]]));
                if (logclass  & LC_TRACE)
                    ls_syslog(LOG_DEBUG3, "%s: copy host <%s> to candHosts[%d] with xorRes <%s>.", fname, candPtr[i].hData->host, i, resValPtr->xorExprs[j]);
            }


            reshapeCandHost(jpbw, candPtr, &(numCandPtr));
            foundGroup++;
        }


        jpbw->groupCands[j].members = candPtr;
        jpbw->groupCands[j].numOfMembers = numCandPtr;
    }

    FREEUP(indicesOfCandPtr);
    FREE_IND_CANDPTR(jpbw->candPtr, jpbw->numCandPtr);

    jpbw->numCandPtr = 0;
    jpbw->candPtr = NULL;
    if (logclass  & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "%s: Exiting, found %d groups", fname, foundGroup);
    if (foundGroup == 0 ) {
        FREEUP(jpbw->groupCands);
        FREEUP(jpbw->inEligibleGroups);
        jpbw->numOfGroups = 0;
        return (CAND_NO_HOST);
    }
    return(CAND_HOST_FOUND);
}

static int
needHandleXor(struct jData *jpbw)
{

    if ((jpbw->shared->jobBill.maxNumProcessors > 1 )
        && (!allInOne(jpbw))) {
        if (jpbw->shared->resValPtr
            && (jpbw->shared->resValPtr->xorExprs != NULL))
            return TRUE;
        if (jpbw->qPtr->resValPtr
            && (jpbw->qPtr->resValPtr->xorExprs != NULL))
            return TRUE;
    }
    return FALSE;
}

static void
copyCandHostPtr(struct candHost **sourceCandPtr, struct candHost **destCandPtr, int *sourceNum, int *destNum)
{
    static char fname[]="copyCandHostPtr";
    int j;


    FREE_IND_CANDPTR((*destCandPtr), *destNum);

    if (*sourceNum > 0) {
        *destCandPtr = (struct candHost *)my_malloc((*sourceNum) * sizeof(struct candHost), fname);
    }
    for (j=0; j < (*sourceNum); j++)
        copyCandHostData(&((*destCandPtr)[j]),&((*sourceCandPtr)[j]));
    *destNum = *sourceNum;
}

static enum dispatchAJobReturnCode
XORDispatch(struct jData *jp, int arg2, enum dispatchAJobReturnCode (*func)(struct jData *, int))
{
    int i, reserveIdx, isSet;
    enum dispatchAJobReturnCode ret;
    int numSlots = jp->numSlots;
    int numAvailSlots = jp->numAvailSlots;

    if (jp->groupCands != NULL) {
        reserveIdx = -1;
        for (i=0; i < jp->numOfGroups; i++) {
            TEST_BIT(i, jp->inEligibleGroups, isSet) ;
            if (isSet == 1 )
                continue;
            if (jp->groupCands[i].numOfMembers == 0 ) {
                if (logclass & (LC_TRACE | LC_SCHED)) {
                    ls_syslog(LOG_DEBUG3, "XORDispatch: group with index %d is unusable, marked it.", i);
                }
                SET_BIT(i, jp->inEligibleGroups);
                continue;
            }

            copyCandHostPtr(&(jp->groupCands[i].members),&(jp->candPtr), &(jp->groupCands[i].numOfMembers), &(jp->numCandPtr));

            ret = func(jp, arg2);
            switch (ret) {
                case DISP_FAIL :
                case DISP_NO_JOB:
                    SET_BIT(i, jp->inEligibleGroups);
                    if (logclass & (LC_TRACE | LC_SCHED)) {
                        ls_syslog(LOG_DEBUG3, "XORDispatch: cannot dispatch to group with index %d, marked it.", i);
                    }
                    break;
                case DISP_OK :
                    return (ret);
                case DISP_RESERVE :
                    if (logclass & (LC_TRACE | LC_SCHED)) {
                        ls_syslog(LOG_DEBUG3, "XORDispatch: group with index %d is reservable. Reserved index = %d", i, reserveIdx);
                    }
                    if (reserveIdx < 0) {

                        if (i == jp->numOfGroups - 1) {

                            return DISP_RESERVE;
                        }
                        jp->numSlotsReserve = jp->numSlots;
                        jp->numAvailSlotsReserve = jp->numAvailSlots;
                        reserveIdx = i;
                    }

                    freeReserveSlots(jp);
                    break;
                default :
                    break;
            }
            jp->processed &= ~JOB_STAGE_DONE;

            jp->numSlots = numSlots;
            jp->numAvailSlots = numAvailSlots;


            FREE_CAND_PTR(jp);
            jp->numCandPtr = 0;

            if (ret == DISP_TIME_OUT)
                return(ret);

        }

        if (reserveIdx >= 0) {

            copyCandHostPtr(&(jp->groupCands[reserveIdx].members),&(jp->candPtr), &(jp->groupCands[reserveIdx].numOfMembers), &(jp->numCandPtr));

            if (logclass & (LC_TRACE | LC_SCHED)) {
                ls_syslog(LOG_DEBUG3, "XORDispatch: Trying to dispatch to the reservable group with index %d.", reserveIdx);
            }
            jp->numSlots = jp->numSlotsReserve;
            jp->numAvailSlots = jp->numAvailSlotsReserve;
            ret = func(jp, arg2);
            if (ret == DISP_FAIL || ret == DISP_NO_JOB) {
                if (logclass & (LC_TRACE | LC_SCHED)) {
                    ls_syslog(LOG_DEBUG3, "XORDispatch: Dispatching to the reservable group with index %d failed. Marked it as unusable.", reserveIdx);
                }
                SET_BIT(i, jp->inEligibleGroups);

                if (ret == DISP_FAIL) {
                    ls_syslog(LOG_ERR, "XORDispatch: Gone through all groups of candhosts for job <%s>, group number <%d> could reserve job slots, but when go back to reserve it, got a DISP_FAIL.", lsb_jobid2str(jp->jobId), reserveIdx);
                }
            }
            return(ret);
        } else {

            jp->processed |= JOB_STAGE_DONE;
        }

        return(DISP_NO_JOB);
    } else {

        return(func(jp, arg2));
    }
}

static enum candRetCode
XORCheckIfCandHostIsOk(struct jData *jp)
{
    if (jp->groupCands != NULL) {
        int i, isSet;
        enum candRetCode ret, retVal;

        retVal = CAND_NO_HOST;
        for (i = 0; i<jp->numOfGroups; i++) {
            TEST_BIT(i, jp->inEligibleGroups, isSet) ;
            if (isSet == 1 )
                continue;

            copyCandHostPtr(&(jp->groupCands[i].members), &(jp->candPtr), &(jp->groupCands[i].numOfMembers), &(jp->numCandPtr));


            ret = checkIfCandHostIsOk(jp);

            if (ret != CAND_HOST_FOUND) {
                SET_BIT(i, jp->inEligibleGroups);
            }

            if (retVal == CAND_HOST_FOUND || ret == CAND_HOST_FOUND) {
                retVal = CAND_HOST_FOUND;
            } else if (retVal == CAND_FIRST_RES || ret == CAND_FIRST_RES) {
                retVal = CAND_FIRST_RES;
            } else {
                retVal = CAND_NO_HOST;
            }

            copyCandHostPtr(&(jp->candPtr), &(jp->groupCands[i].members), &(jp->numCandPtr), &(jp->groupCands[i].numOfMembers));
        }
        return(retVal);
    } else {
        return(checkIfCandHostIsOk(jp));
    }
}

static void
removeCandHostFromCandPtr(struct candHost **pCandPtr, int *pNumCandPtr, int i)
{
    int j;
    DESTROY_BACKFILLEE_LIST((*pCandPtr)[i].backfilleeList);
    for (j = i; j < (*pNumCandPtr) - 1; j++) {
        (*pCandPtr)[j] = (*pCandPtr)[j+1];
    }
    (*pNumCandPtr)--;
    if ((*pNumCandPtr) == 0) {
        FREEUP(*pCandPtr);
    }
}

static void
groupCandsCopy(struct jData *dest, struct jData *src)
{
    static char fname[] = "groupCandsCopy";
    int i;

    dest->numOfGroups = src->numOfGroups;
    dest->groupCands =
        (struct groupCandHosts *)my_malloc(dest->numOfGroups *
                                           sizeof(struct groupCandHosts),
                                           fname);
    for (i = 0; i < dest->numOfGroups; i++) {
        groupCandHostsCopy(&dest->groupCands[i], &src->groupCands[i]);
    }
    inEligibleGroupsInit(&dest->inEligibleGroups, dest->numOfGroups);
}

static void
groupCandHostsCopy(struct groupCandHosts *dest, struct groupCandHosts *src)
{
    groupCandHostsInit(dest);
    copyCandHostPtr(&src->members, &dest->members, &src->numOfMembers,
                    &dest->numOfMembers);
}

static void
groupCandHostsInit(struct groupCandHosts *gc)
{
    gc->tried = FALSE;
    gc->numOfMembers = 0;
    gc->members = NULL;
}

static void
inEligibleGroupsInit(int **inEligibleGroups, int numGroups)
{
    static char fname[] = "inEligibleGroupsInit";
    int j;


    *inEligibleGroups = (int *)my_malloc(GET_INTNUM(numGroups)
                                         * sizeof(int), fname);

    for (j = 0; j < GET_INTNUM(numGroups); j++) {
        (*inEligibleGroups)[j] = 0;
    }
}

static void
groupCands2CandPtr(int numOfGroups, struct groupCandHosts *gc,
                   int *numCandPtr, struct candHost **candPtr)
{
    static char fname[] = "groupCands2CandPtr";
    int i, j, k, num;

    for (i = 0, num = 0; i < numOfGroups; i++) {
        num += gc[i].numOfMembers;
    }
    *candPtr = (struct candHost *)my_calloc(num, sizeof(struct candHost),
                                            fname);
    for (i = 0, num = 0; i < numOfGroups; i++) {
        for (j = 0; j < gc[i].numOfMembers; j++) {
            for (k = 0; k < num; k++) {
                if ((*candPtr)[k].hData == gc[i].members[j].hData) {
                    break;
                }
            }
            if (k == num) {

                copyCandHostData(&(*candPtr)[num], &gc[i].members[j]);
                num++;
            }
        }
    }
    *numCandPtr = num;
}

void
setLsbPtilePack(const bool_t x)
{
    if (x == TRUE) {
        lsbPtilePack = TRUE;
    } else {
        lsbPtilePack = FALSE;
    }

}

static void 
initSchedPendJobSet(struct fairsharePolicy *policy) {
    if (schedPendJobSet.policy == NULL) {
        h_initTab_(&(schedPendJobSet.usersTab), 23);
        schedPendJobSet.add = addSchedPendJob2Set;
    } else {
        sTab stab;
        hEnt *ent;
    
        ent = h_firstEnt_(&(schedPendJobSet.usersTab), &stab);
        while(ent) {
            listDestroy((LIST_T *)ent->hData, NULL);
            ent = h_nextEnt_(&stab);
        }
        h_freeRefTab_(&(schedPendJobSet.usersTab));
        h_initTab_(&(schedPendJobSet.usersTab), 23);
    }
    schedPendJobSet.policy = policy;
    schedPendJobSet.nJobs = 0;
} /*initSchedPendJobSet*/

/*We only cache 10 pending jobs which still pend after scheduling for each user*/
static int addSchedPendJob2Set(struct fsQueueJobSet *jobSet, void *data) {
    struct jData *jPtr = (struct jData *)data;
    hEnt *ent;
    LIST_T *jList;
    int   new;
    struct jobFSRef *jRef;

    ent = h_addEnt_(&(schedPendJobSet.usersTab), jPtr->userName, &new);

    if (new) {
        jList = listCreate("user's job reference list in fs queue");
        ent->hData = (int *)jList;
    } else {
        jList = (LIST_T *)ent->hData;
    }

    jRef = (struct jobFSRef *)my_calloc(1, sizeof(struct jobFSRef), "addSchedPendJob2Set");
    jRef->job = jPtr;
    if (jList->numEnts < FS_MAX_CANDHOST) {
        jobSet->nJobs++;
    } else {
        /*There are FS_MAX_CANDHOST jobs already, let us remove the back one, and append new on at front*/
        LIST_ENTRY_T * tmp = listGetBackEntry(jList);
        listRemoveEntry(jList, tmp);
        FREEUP(tmp);
    }
    listInsertEntryAtFront(jList, (LIST_ENTRY_T *)jRef);

    return TRUE;
}

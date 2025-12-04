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
#ifndef MBD_H
#define MBD_H

#include "../lsbatch.h"
#include "daemonout.h"
#include "daemons.h"
#include "../../lsf/intlib/bitset.h"
#include "jgrp.h"

#define  DEF_CLEAN_PERIOD     3600
#define  DEF_MAX_RETRY        5
#define  DEF_MAXSBD_FAIL      3
#define  DEF_ACCEPT_INTVL     1
#define  DEF_PRIO             1
#define  DEF_NICE             0
#define  DEF_SCHED_DELAY      10
#define  DEF_Q_SCHED_DELAY    2
#define  MAX_INTERNAL_JOBID   299999999999999LL
#define  DEF_MAX_JOB_NUM      1000
#define  DEF_EXCLUSIVE        FALSE
#define  DEF_EVENT_WATCH_TIME 60
#define  DEF_COND_CHECK_TIME  600
#define DEF_MAX_SBD_CONNS 32
#define DEF_SCHED_STAY        3
#define DEF_FRESH_PERIOD     15
#define DEF_PEND_EXIT       512
#define DEF_JOB_ARRAY_SIZE  1000

#define DEF_LONG_JOB_TIME  1800

#define MAX_JOB_PRIORITY   INFINIT_INT

#define DEF_PRE_EXEC_DELAY    -1

/* Global MBD job lists
 */
typedef enum {
    SJL,
    MJL,
    PJL,
    FJL,
    NJLIST,
    ZJL,
    ALLJLIST
} jlistno_t;

#define  DEF_USR1_SC    0
#define  DEF_USR1_ST    64000
#define  DEF_USR2_SC    0
#define  DEF_USR2_ST    64000


#define BAD_LOAD     1
#define TOO_LATE     2
#define FILE_MISSING 3
#define BAD_USER     4
#define JOB_MISSING  5
#define BAD_SUB      6
#define MISS_DEADLINE 8


#define JFLAG_LASTRUN_SUCC     0x002
#define JFLAG_DEPCOND_INVALID  0x004

#define JFLAG_READY            0x008
#define JFLAG_EXACT            0x200
#define JFLAG_UPTO             0x400
#define JFLAG_DEPCOND_REJECT   0x8000
#define JFLAG_SEND_SIG         0x10000
#define JFLAG_BTOP             0x20000
#define JFLAG_ADM_BTOP         0x40000
#define JFLAG_READY1           0x100000
#define JFLAG_READY2           0x200000
#define JFLAG_URGENT           0x400000
#define JFLAG_URGENT_NOSTOP    0x800000
#define JFLAG_REQUEUE          0x1000000
#define JFLAG_HAS_BEEN_REQUEUED 0x2000000

#define JFLAG_JOB_ANALYZER  0x20000000

#define JFLAG_WILL_BE_PREEMPTED  0x40000000

#define JFLAG_WAIT_SWITCH  0x80000000


#define M_STAGE_GOT_LOAD    0x0001
#define M_STAGE_LSB_CAND    0x0002
#define M_STAGE_QUE_CAND    0x0004
#define M_STAGE_REPLAY      0x0008
#define M_STAGE_INIT        0x0010
#define M_STAGE_RESUME_SUSP 0x0020
#ifdef MAINTAIN_FCFS_FOR_DEPENDENT_JOBS
#define M_STAGE_CHK_JGRPDEP 0x0040
#endif

#define JOB_STAGE_READY 0x0001
#define JOB_STAGE_CAND  0x0002
#define JOB_STAGE_DISP  0x0004
#define JOB_STAGE_DONE  0x0008
#define JOB_IS_PROCESSED(jp) ((jp)->processed & JOB_STAGE_DONE)

extern int mSchedStage;
extern int freshPeriod;
extern int maxSchedStay;

#define DEL_ACTION_KILL      0x01
#define DEL_ACTION_REQUEUE   0x02

#define ALL_USERS_ADMINS  INFINIT_INT

#define LOG_IT              0

#define CALCULATE_INTERVAL     900

struct qPRValues {
    struct qData *qData;
    float usedByRunJob;
    float reservedByPreemptWait;
    float availableByPreempt;
    float preemptingRunJob;
};

struct preemptResourceInstance {
    struct resourceInstance *instancePtr;
    int nQPRValues;
    struct qPRValues *qPRValues;
};

struct preemptResource {
    int index;
    int numInstances;
    struct preemptResourceInstance *pRInstance;

};

struct objPRMO {
    int numPreemptResources;
    struct preemptResource *pResources;

};
extern struct objPRMO *pRMOPtr;

#define PRMO_ALLVALUES                  0x0000
#define PRMO_USEDBYRUNJOB               0x0001
#define PRMO_RESERVEDBYPREEMPTWAIT      0x0002
#define PRMO_AVAILABLEBYPREEMPT         0x0004
#define PRMO_PREEMPTINGRUNJOB           0x0008

#define  JOB_PREEMPT_WAIT(s)  ((s)->jStatus & JOB_STAT_RSRC_PREEMPT_WAIT)

#define  MARKED_WILL_BE_PREEMPTED(s)  ((s)->jFlags & JFLAG_WILL_BE_PREEMPTED)

#define FORALL_PRMPT_RSRCS(resn) if (pRMOPtr != NULL) { \
    int _pRMOindex;                                     \
    for (_pRMOindex = 0;                                \
             _pRMOindex < pRMOPtr->numPreemptResources; \
             _pRMOindex++) {                            \
        resn = pRMOPtr->pResources[_pRMOindex].index;

#define ENDFORALL_PRMPT_RSRCS }         \
}

#define GET_RES_RSRC_USAGE(resn, val, jResValPtr, qResValPtr) { \
      int jobSet = FALSE, queueSet = FALSE;                     \
      if (jResValPtr)                                           \
          TEST_BIT(resn, jResValPtr->rusgBitMaps, jobSet);      \
      if (qResValPtr)                                           \
          TEST_BIT(resn, qResValPtr->rusgBitMaps, queueSet);    \
      if (jobSet == 0 && queueSet == 0) {                       \
          val = -1.0;                                           \
      } else if (jobSet == 0 && queueSet != 0) {                \
          val = qResValPtr->val[resn];                          \
      } else if (jobSet != 0 && queueSet == 0) {                \
          val = jResValPtr->val[resn];                          \
      } else if (jobSet != 0 && queueSet != 0) {                \
          val = jResValPtr->val[resn];                          \
      }                                                         \
  }

struct candHost {
    struct hData *hData;
    int    numSlots;
    int    numAvailSlots;

    int    numNonBackfillSlots;
    int    numAvailNonBackfillSlots;
    LIST_T *backfilleeList;
};

struct askedHost {
    struct hData *hData;
    int    priority;
};

#define CLEAR_REASON(v, reason) if (v == reason) v = 0;
#define SET_REASON(condition, v, reason) \
        if (condition) v = reason; else CLEAR_REASON(v, reason)

#define NON_PRMPT_Q(qAttrib)    TRUE


struct rqHistory{
    struct hData *host;
    int retry_num;
    time_t lasttime;
};

#define  JOB_PEND(s)  (((s)->jStatus & JOB_STAT_PEND) && !((s)->jStatus & JOB_STAT_PSUSP))


struct groupCandHosts {
    int               numOfMembers;
    int tried;
    struct candHost  *members;
};


struct jData {
    struct  jData *forw;
    struct  jData *back;
    int     userId;
    char    *userName;
    struct  uData *uPtr;
    LS_LONG_INT jobId;
    float   priority;
    int     jStatus;
    time_t  updStateTime;
    int     jFlags;
    int     oldReason;
    int     newReason;
    int     subreasons;
    int     *reasonTb;
    int     numReasons;
    struct  qData *qPtr;
    struct  hData **hPtr;
    int     numHostPtr;
    struct  askedHost *askedPtr;
    int     numAskedPtr;
    int     askedOthPrio;
    struct  candHost *candPtr;
    int     numCandPtr;
    struct  candHost *execCandPtr;
    int     numExecCandPtr;
    int     numEligProc;
    int     numAvailEligProc;
    int     numSlots;
    int     numAvailSlots;
    char    usePeerCand;
    time_t  reserveTime;
    int     slotHoldTime;
    char    processed;
    int     dispCount;
    time_t  dispTime;
    time_t  startTime;
    time_t  resumeTime;
    time_t  predictedStartTime;
    int     runCount;
    int     retryHist;
    int     nextSeq;
    int     jobPid;
    int     jobPGid;
    int     runTime;
    float   cpuTime;
    time_t  endTime;
    time_t  requeueTime;
    struct pendEvent {
        int   notSwitched;
        int   sig;
        int   sig1;
        int   sig1Flags;
        int   sigDel;
        int   notModified;
    } pendEvent;
    int     numDependents;
    char    *schedHost;
    int     actPid;
    time_t  ssuspTime;
    struct  submitReq *newSub;
    struct  lsfRusage *lsfRusage;
    int     execUid;
    struct rqHistory *reqHistory;
    int lastDispHost;
    int requeMode;
    int reqHistoryAlloc;
    int     exitStatus;
    char    *execCwd;
    char    *execHome;
    char    *execUsername;
    char    *queuePreCmd;
    char    *queuePostCmd;
    int     initFailCount;
    time_t  jRusageUpdateTime;
    struct  jRusage runRusage;
    int     numUserGroup;
    char    **userGroup;
    char * execHosts;
    int    sigValue;
    struct jShared  *shared;
    int     numRef;
    struct  jgTreeNode*   jgrpNode;
    int     nodeType;
    struct  jData*        nextJob;
    int     restartPid;
    time_t  chkpntPeriod;
    u_short port;


    int     jSubPriority;
    int     jobPriority;
    char    *jobSpoolDir;
    struct hData **rsrcPreemptHPtr;
    int numRsrcPreemptHPtr;

    struct groupCandHosts *groupCands;
    int    numOfGroups;
    int    reservedGrp;
    int    currentGrp;
    int*   inEligibleGroups;
    int numSlotsReserve;
    int numAvailSlotsReserve;
    struct shareAcct * sa;  /*refer to shareAcct of fairshare tree in global policies*/
};


#define JOB_HAS_CANDHOSTS(Job)  ((Job)->candPtr != NULL)

#define JOB_CANDHOST(Job,I) (Job)->candPtr[(I)].hData

#define JOB_CAND_HOSTNAME(Job,I) (Job)->candPtr[(I)].hData->host

#define JOB_EXECHOST(Job, I) (Job)->hPtr[(I)]

#define JOB_SUBMIT_TIME(Job)  (Job)->shared->jobBill.submitTime

#define JOB_PROJECT_NAME(Job) (Job)->shared->jobBill.projectName


#define FOR_EACH_JOB_LOCAL_EXECHOST(Host, Job) \
{ \
    int   __hidx__; \
    for (__hidx__ = 0; __hidx__ < (Job)->numHostPtr; __hidx__++) { \
        struct hData *Host = (Job)->hPtr[__hidx__]; \
        if (Host == NULL) break; \
        if (Host->hStatus & HOST_STAT_REMOTE) continue;

#define END_FOR_EACH_JOB_LOCAL_EXECHOST }}

#define JOB_RUNSLOT_NONPRMPT(Job) \
    (   ((Job)->jFlags & JFLAG_URGENT) \
     || ((Job)->qPtr->qAttrib & Q_ATTRIB_BACKFILL) \
    )

#define JOB_RSVSLOT_NONPRMPT(Job) \
    (   (! (Job)->jStatus & JOB_STAT_PEND) \
    )


struct jShared {
    int                numRef;
    struct  dptNode   *dptRoot;
    struct  submitReq  jobBill;
    struct  resVal    *resValPtr;
};


#define HAS_LOCAL_RESERVEDHOSTS(JP) \
    ((JP)->jStatus & JOB_STAT_RESERVE && (JP)->numHostPtr > 0)

#define CONFIRM_LOCAL_RESERVEDHOSTS(JP) \
{ \
    updResCounters((JP), ~JOB_STAT_RESERVE); \
    (JP)->jStatus &= ~JOB_STAT_RESERVE; \
}

struct       hostAcct {
    struct   hData *hPtr;
    int      numRUN;
    int      numSSUSP;
    int      numUSUSP;
    int      numRESERVE;
    int      numNonPrmptRsv;
    int      numAvailSUSP;
};

struct uData {
    char   *user;
    int    uDataIndex;
    int    flags;
    struct uData **gPtr;
    int    numGrpPtr;
    struct gData *gData;
    int    maxJobs;
    float  pJobLimit;
    int    maxPendJobs;
    int    maxPendSlots;
    struct hTab *hAcct;
    int    numPEND;// pend slots
    int    numPENDJobs;
    int    numRUN;
    int    numSSUSP;
    int    numUSUSP;
    int    numJobs;
    int    numRESERVE;
    int    **reasonTb;
    int    numSlots;
    LS_BITSET_T *children;
    LS_BITSET_T *descendants;
    LS_BITSET_T *parents;
    LS_BITSET_T *ancestors;
    LIST_T *pxySJL;
};

#define USER_GROUP_IS_ALL_USERS(UserGroup) \
     ((UserGroup)->numGroups == 0 && \
      (UserGroup)->memberTab.numEnts == 0) \

#define FOR_EACH_UGRP_CHILD_USER(User, Grp) \
    if ((User)->children != NULL) { \
        struct uData *Grp; \
        LS_BITSET_ITERATOR_T __iter__; \
        BITSET_ITERATOR_ZERO_OUT(&__iter__); \
        setIteratorAttach(&__iter__, (User)->children, "FOR_EACH_UGRP_CHILD_USER"); \
        for (Grp = (struct uData *)setIteratorBegin(&__iter__); \
             Grp != NULL; \
             Grp = (struct uData *)setIteratorGetNextElement(&__iter__)) \
        {

#define END_FOR_EACH_UGRP_CHILD_USER }}

#define FOR_EACH_UGRP_DESCENDANT_USER(Grp, User) \
    if ((User)->children != NULL) { \
        struct uData *User; \
        LS_BITSET_ITERATOR_T __iter__; \
        BITSET_ITERATOR_ZERO_OUT(&__iter__); \
        setIteratorAttach(&__iter__, (Grp)->descendants, "FOR_EACH_UGRP_CHILD_USER"); \
        for (User = (struct uData *)setIteratorBegin(&__iter__); \
             User != NULL; \
             User = (struct uData *)setIteratorGetNextElement(&__iter__)) \
        {

#define END_FOR_EACH_UGRP_DESCENDANT_USER }}

#define FOR_EACH_USER_ANCESTOR_UGRP(User, Grp) \
    if ((User)->ancestors != NULL) { \
        struct uData *Grp; \
        LS_BITSET_ITERATOR_T __iter__; \
        BITSET_ITERATOR_ZERO_OUT(&__iter__); \
        setIteratorAttach(&__iter__, (User)->ancestors, "FOR_EACH_USER_ANCESTOR_UGRP"); \
        for (Grp = (struct uData *)setIteratorBegin(&__iter__); \
             Grp != NULL; \
             Grp = (struct uData *)setIteratorGetNextElement(&__iter__)) \
        {

#define END_FOR_EACH_USER_ANCESTOR_UGRP }}

struct uDataTable {
    struct uData **_base_;
    int            _cur_;
    int            _size_;
};

typedef struct uDataTable UDATA_TABLE_T;

#define UDATA_TABLE_NUM_ELEMENTS(Table) ( (Table)->_cur_ )

struct       userAcct {
    struct   uData *uData;
    int      userId;
    int      numPEND;
    int      numRUN;
    int      numSSUSP;
    int      numUSUSP;
    int      numRESERVE;
    int      numNonPrmptRsv;
    int      numAvailSUSP;
    int      reason;
    int      numRunFromLastSession;
    int      numVisitedInSession;
    int      numPendJobsInSession;
    bool_t   skipAccount;
};

struct qData {
    struct qData *forw;
    struct qData *back;
    char      *queue;
    int       queueId;
    char      *description;
    struct    gData *hGPtr;
    int       numProcessors;
    int       nAdmins;
    int       *adminIds;
    char      *admins;
    char      *resReq;
    struct    resVal *resValPtr;
    float     *loadSched;
    float     *loadStop;
    int       mig;
    int       schedDelay;
    int       acceptIntvl;
    int       priority;
    int       nice;
    char      *preCmd;
    char      *postCmd;
    char      *prepostUsername;

    struct requeueEStruct {
        int type;
#define RQE_NORMAL   0
#define RQE_EXCLUDE  1
#define RQE_END     255
        int  value;
        int  interval;
    } *requeEStruct;

    char      *requeueEValues;
    char      *windowsD;
    windows_t *week[8];
    char      *windows;
    windows_t *weekR[8];
    time_t    windEdge;
    time_t    runWinCloseTime;
    int       rLimits[LSF_RLIM_NLIMITS];
    int       defLimits[LSF_RLIM_NLIMITS];
    int       procLimit;
    char      *hostSpec;
    char      *defaultHostSpec;
    int       hJobLimit;
    float     pJobLimit;
    struct    hTab *hAcct;
    int       uJobLimit;
    struct    hTab *uAcct;
    int       qAttrib;
    int       qStatus;
    int       maxJobs;
    int       numJobs;
    int       numPEND;
    int       numRUN;
    int       numSSUSP;
    int       numUSUSP;
    int       numRESERVE;
    int       **reasonTb;
    int       numReasons;
    int       numSlots;
    int       numUsable;
    int       schedStage;
    int       slotHoldTime;
    char      *resumeCond;
    struct resVal *resumeCondVal;
    char      *stopCond;
    char      *jobStarter;
    int       flags;
    char    *suspendActCmd;
    char    *resumeActCmd;
    char    *terminateActCmd;
    int     sigMap[LSB_SIG_NUM];
    struct  gData *uGPtr;
    LS_BITSET_T   *hostInQueue;
    char    *hostList;
    int     numHUnAvail;
    struct  askedHost *askedPtr;
    int     numAskedPtr;
    int     askedOthPrio;
    struct jData *firstJob[PJL+1];
    struct jData *lastJob[PJL+1];
    time_t chkpntPeriod;
    char   *chkpntDir;
    int    minProcLimit;
    int    defProcLimit;
    char   *userShares;
    int    posId; /*For fcfs queue, posId is the smallest index
                   *of the queues which has the same priority;
                   *for fs queue, posId is queueId.
                   */
    struct fairsharePolicy * policy;
    struct fsFactors fsFactors;
    char   *actionComment;
};


#define HOST_STAT_REMOTE       0x80000000


struct hData {
    struct hData *forw;
    struct hData *back;
    char      *host;
    int       hostId;
    char      *hostType;
    char      *hostModel;
    struct    hostent hostEnt;
    float     cpuFactor;
    int       numCPUs;
    float     *loadSched;
    float     *loadStop;
    char      *windows;
    windows_t *week[8];
    time_t    windEdge;
    int       acceptTime;
    int       numDispJobs;
    time_t    pollTime;
    int       sbdFail;
    int       hStatus;
    int       uJobLimit;
    struct    hTab *uAcct;
    int       maxJobs;
    int       numJobs;
    int       numRUN;
    int       numSSUSP;
    int       numUSUSP;
    int       numRESERVE;
    int       mig;
    int       chkSig;
    int       maxMem;
    int       maxSwap;
    int       maxTmp;
    int       nDisks;
    int       *resBitMaps;
    int       *limStatus;
    float     *lsfLoad;
    float     *lsbLoad;
    struct    bucket *msgq[3];
    int       *busyStop;
    int       *busySched;
    int       reason;
    int       flags;
    int       numInstances;
    struct resourceInstance **instances;
    LIST_T    *pxySJL;
    LIST_T    *pxyRsvJL;
    float     leftRusageMem;
    char      *actionComment;
};


struct sbdNode {
    struct sbdNode *forw;
    struct sbdNode *back;
    int chanfd;
    struct jData *jData;
    struct hData *hData;
    sbdReqType reqCode;
    time_t lastTime;
    int sigVal;
    int sigFlags;
};

extern struct sbdNode sbdNodeList;

struct fairsharePolicy
{
    char             *name;        /*queue name*/
    struct qData     *qPtr;       /*pointer to qData*/
    struct shareAcct *root;        /*FS tree root node*/
    LIST_T           *userList;    /*all leaf nodes in FS tree which are in shareAcctEntry type*/
    hTab              userTab;     /*leaf users's hTab. key:userName, value:the list of shareAcct which have the same name*/ 
};

struct shareAcct {
    struct shareAcct *forw;
    struct shareAcct *back;
    char             *name;
    char             *path;
    int               share;
#define FS_IS_DEFAULT      0x00000001      /*user_shares is 'default'*/
#define FS_ALL_DEFAULT     0x00000002      /*ug member is all, user_shares has 'default'*/
#define FS_LEAF_GROUP      0x00000004      /*leaf node is a user group*/
#define FS_SKIPABLE        0x00000008      /*The node could be skipped during electing jobs*/
    int   flags;
    struct fairsharePolicy *policy;        /*point to policy*/
    struct shareAcct *parent;              /*point upper level shareAcct*/
    struct shareDistributePolicy *sharePolicy; /*child who assigned shares*/
    float             priority;          /*dynamic priority*/
    int               numStartJobs;      /*the number of started jobs*/
    float             newUsedCpuTime;    /*cputime which hasn't exceeded recent decay duration.*/
    float             histCpuTime;       /*cputime which has been decayed.*/
    int               runTime;           /*started jobs's runtime*/
    int               numSkippedChild;   /*used when electing job from FS tree*/
    hEnt              *jListEntCache;    /*cache job reference in jobSet*/
};

struct shareAcctRef {
    struct shareAcctRef *forw;
    struct shareAcctRef *back;
    struct shareAcct    *sacct;
};

struct shareDistributePolicy {
    struct shareAcct *holder;     /*back pointer of the corresponding shareAcct*/
#define FS_OUT_OTHERS     0x00000001  /*the usersTab records users who are outside of 'others'*/
#define FS_IN_OTHERS      0x00000002  /*the usersTab records users who are inside of 'others'*/
    int               flags;
    LIST_T           *sAcctList;  /*Child nodes which has own user_share*/
    hTab             *usersTab;    /*Users who share 'user_share'*/
};

struct ugrpAttrs {            
    int      numUserShares;   
    struct userShares  *userShares;
};

struct gData {
    char     *group;
    hTab     memberTab;
    int      numGroups;
    struct   gData *gPtr[MAX_GROUPS];
    struct   ugrpAttrs *ugrpAttrs;   /*If gData stand for usergroup, then this field will be used.*/
};

typedef enum {
    DPT_AND             = 0,
    DPT_OR              = 1,
    DPT_NOT             = 2,
    DPT_LEFT_           = 3,
    DPT_RIGHT_          = 4,
    DPT_DONE            = 5,
    DPT_EXIT            = 6,
    DPT_STARTED         = 7,
    DPT_NAME            = 8,
    DPT_ENDED           = 9,
    DPT_NUMPEND         = 10,
    DPT_NUMHOLD         = 11,
    DPT_NUMRUN          = 12,
    DPT_NUMEXIT         = 13,
    DPT_NUMDONE         = 14,
    DPT_NUMSTART        = 15,
    DPT_NUMENDED        = 16,
    DPT_COMMA           = 17,
    DPT_GT              = 18,
    DPT_GE              = 19,
    DPT_LT              = 20,
    DPT_LE              = 21,
    DPT_EQ              = 22,
    DPT_NE              = 23,
    DPT_POST_DONE       = 24,
    DPT_POST_ERR        = 25,
    DPT_TRUE            = 26,
    DPT_WINDOW          = 27,

} dptType;

#define  DP_FALSE     0
#define  DP_TRUE      1
#define  DP_INVALID  -1
#define  DP_REJECT   -2

#define  ARRAY_DEP_ONE_TO_ONE 1

struct jobIdx {
    int numRef;
    struct idxList *idxList;
    struct listSet *depJobList;
};

struct dptNode {
    dptType type;
    int value;
    int updFlag;
    union {
        struct {
            struct dptNode *left;
            struct dptNode *right;
        } opr;
        struct {
            int           opType;
            int           exitCode;
            int           opFlag;
            struct jData  *jobRec;
            struct jobIdx *jobIdx;
        } job;
        struct {
            struct  timeWindow * timeWindow;
        } window;
        struct {
            int                  opType;
            int                  num;
            struct jgArrayBase   *jgArrayBase;
        } jgrp;
    } dptUnion;
};

#define WINDOW_CLOSE       0
#define WINDOW_OPEN        1
struct  timeWindow {
    int                  status;
    char                *windows;
    windows_t           *week[8];
    time_t               windEdge;
};

#define JOB_NEW    1
#define JOB_REQUE  2
#define JOB_REPLAY 3

struct clientNode {
    struct clientNode *forw;
    struct clientNode *back;
    int    chanfd;
    struct sockaddr_in from;
    char *fromHost;
    mbdReqType reqType;
    time_t lastTime;
};

struct condData {
    char *name;
    int status;
    int lastStatus;
    time_t lastTime;
    int   flags;
    struct dptNode *rootNode;
};

struct resourceInstance {
    char      *resName;
    int       nHosts;
    struct hData **hosts;
    char      *lsfValue;
    char      *value;
};

struct profileCounters {
    int cntVal;
    char *cntDescr;
};

#undef MBD_PROF_COUNTER
#define MBD_PROF_COUNTER(Func) PROF_CNT_ ## Func,

typedef enum profCounterType {
#   include "mbd.profcnt.def"
    PROF_CNT_nullfunc
} PROF_COUNTER_TYPE_T;

#define INC_CNT(counterId) \
               { \
                  counters[counterId].cntVal++; \
               }

#define RESET_CNT() \
                 { \
                   int i; \
                   for(i = 0; counters[i].cntDescr != NULL; i++) \
                       counters[i].cntVal = 0; \
                 }
#define DUMP_CNT() \
                { \
                  int i; \
                  if (logclass & LC_PERFM )  \
                  for(i = 0; counters[i].cntDescr != NULL; i++) { \
                     if(counters[i].cntVal != 0) { \
                          ls_syslog(LOG_INFO,"dumpCounters: %s <%d>", \
                              counters[i].cntDescr,counters[i].cntVal); \
                     } \
                  } \
                }

#define CONF_COND 0x001

#define QUEUE_UPDATE      0x01
#define QUEUE_NEEDPOLL    0x02
#define QUEUE_REMOTEONLY  0x04
#define QUEUE_UPDATE_USABLE 0x08

/* Various host flags..
 */
#define HOST_UPDATE       (1 << 0)
#define HOST_NEEDPOLL     (1 << 1)
#define HOST_UPDATE_LOAD  (1 << 2)
#define HOST_JOB_RESUME   (1 << 3)
#define HOST_AUTOCONF_MXJ (1 << 5)
#define HOST_LOST_FOUND   (1 << 6)

#define USER_GROUP     0x001
#define USER_UPDATE    0x002
#define USER_INIT      0x004
#define USER_BROKER    0x008
#define USER_OTHERS    0x010
#define USER_ALL       0x020

#define FIRST_START   1
#define WINDOW_CONF   2
#define RECONFIG_CONF 3
#define NORMAL_RUN    4


#define dptJobRec  dptUnion.job.jobRec
#define dptJobIdx  dptUnion.job.jobIdx
#define dptJobParents  dptUnion.job.parentNodes
#define dptLeft  dptUnion.opr.left
#define dptRight dptUnion.opr.right
#define dptJgrp dptUnion.jgrp.jgArrayBase
#define dptWindow dptUnion.window.timeWindow


extern LIST_T *hostList;
extern struct hTab            hostTab;
extern struct jData           *jDataList[];
extern int                    pendJobSlots;
extern struct migJob          *migJobList;
extern struct qData           *qDataList;
extern UDATA_TABLE_T          *uDataPtrTb;
extern struct hTab            uDataList;
extern struct hTab            calDataList;
extern struct jData           *chkJList;
extern struct clientNode      *clientList;
extern struct hTab            jobIdHT;
extern struct hTab            jgrpIdHT;
extern struct gData           *usergroups[];
extern struct gData           *hostgroups[];
extern struct profileCounters counters[];
extern struct hTab            policies;
extern struct hTab            jobSATab;
extern char                   errstr[];
extern int                    debug;
extern int                    errno;
extern int                    nextId;
extern int                    numRemoteJobsInList;
extern unitTypes              unitForLimits;


extern char                   *defaultQueues;
extern char                   *defaultHostSpec;
extern int                    max_retry;
extern int                    max_sbdFail;
extern int                    accept_intvl;
extern int                    clean_period;
extern int                    delay_period;
extern char                   *dbSelectLoad;
extern char                   *pjobSpoolDir;
extern int                    preExecDelay;
extern int                    slotResourceReserve;
extern int                    maxAcctArchiveNum;
extern int                    acctArchiveInDays;
extern int                    acctArchiveInSize;
extern int                    resourcePerTask;
extern int                    lsbModifyAllJobs;
extern float                  cpuTimeFactor;
extern float                  runTimeFactor;
extern float                  runJobFactor;
extern float                  histHours;
extern float                  clsDecay;


extern int                    numofqueues;
extern int                    numofprocs;
extern int                    numofusers;
extern int                    numofugroups;
extern int                    numofhgroups;
extern int                    maxjobnum;


extern int                    msleeptime;
extern int                    subTryInterval;
extern int                    maxPendJobs;
extern int                    maxPendSlots;
extern int                    defaultLimitIgnoreUserGroup;
extern int                    numRemoveJobs;
extern int                    eventPending;
extern int                    qAttributes;
extern int                    **hReasonTb;
extern uid_t                  *managerIds;
extern char                   **lsbManagers;
extern int                    nManagers;
extern char                   *lsfDefaultProject;
extern int                    dumpToDBPid;
extern int                    dumpToDBExit;
extern int                    maxJobArraySize;
extern int                    jobRunTimes;
extern int                    jobDepLastSub;
extern int                    maxUserPriority;
extern int                    jobPriorityValue;
extern int                    jobPriorityTime;
extern int                    scheRawLoad;

extern time_t                  last_hostInfoRefreshTime;
extern struct hTab             condDataList;
extern int                     readNumber;
extern time_t                  condCheckTime;
extern struct userConf *       userConf;
extern time_t                  last_hostInfoRefreshTime;
extern struct hTab             condDataList;
extern int                     readNumber;
extern time_t                  condCheckTime;
extern struct userConf *       userConf;

extern bool_t                  mcSpanClusters;
extern bool_t                  disableUAcctMap;

extern int                     numResources;
extern struct sharedResource **sharedResources;

extern int                     nSbdConnections;
extern int                     maxSbdConnections;
extern int                     maxJobPerSession;

extern struct hostInfo *       LIMhosts;
extern int                     numLIMhosts;

extern float                   maxCpuFactor;
extern int                     freedSomeReserveSlot;

extern long                    schedSeqNo;
extern struct controlReq       mbdCtrlReq;

extern void                 pollSbatchds(int);
extern void                 hStatChange(struct hData *, int status);
extern int                  checkHosts(struct infoReq*,
                                       struct hostDataReply *);
extern struct hData *       getHostData(char *host);
extern struct hData *       getHostData2(char *host);
extern float *              getHostFactor (char *host);
extern float *              getModelFactor (char *hostModel);
extern int                  getModelFactor_r(char *hostModel, float *cpuFactor);
extern void                 checkHWindow(void);
extern hEnt *               findHost(char *hname);
extern void                 renewJob(struct jData *oldjob);
extern void                 getTclHostData(struct tclHostData *,
                                           struct hData *, struct hData *);
extern int                  getLsbHostNames (char ***);
extern void                 getLsbHostInfo(void);
extern int                  getLsbHostLoad(void);
extern int                  getHostsByResReq(struct resVal *, int *,
                                             struct hData **,
                                             struct hData ***,
                                             struct hData *,int *);

extern struct resVal *      checkResReq(char *, int);
extern void                 adjLsbLoad(struct jData *, int, bool_t);
extern int                  countHostJobs(struct hData *);
extern void                 getLsbResourceInfo(void);
extern struct resVal *      getReserveValues(struct resVal *,struct resVal *);
extern void                 getLsfHostInfo(int);
extern struct hData *       getHostByType(char *);

extern void                 checkQWindow(void);
extern int                  checkQueues(struct infoReq *,
                                        struct queueInfoReply *);
extern int                  ctrlQueue(struct controlReq *, struct lsfAuth *);
extern int                  ctrlHost(struct controlReq *, struct hData *,
                                     struct lsfAuth *);
extern void                 sumHosts(void);
extern void                 inQueueList(struct qData *);
extern struct qData *       getQueueData(char *);
extern char                 hostQMember(char *, struct qData *);
extern char                 userQMember(char *, struct qData *);
extern int                  isQueAd (struct qData *, char *);
extern int                  isAuthQueAd (struct qData *, struct lsfAuth *);
extern int                  isInQueues(char *, char **, int);
extern void                 freeQueueInfoReply(struct queueInfoReply *,
                                               char *);
extern struct hostInfo *    getLsfHostData (char *);
extern int                  createQueueHostSet(struct qData *);
extern int                  gethIndexByhData(void *);
extern void *               gethDataByhIndex(int);
extern bool_t               isHostQMember(struct hData *, struct qData *);
extern int                  getIndexByQueue(void *);
extern void *               getQueueByIndex(int);
extern bool_t               isQInQSet(struct qData *, LS_BITSET_T *);

extern struct listSet      *voidJobList;
extern int                  newJob(struct submitReq *,
                                   struct submitMbdReply *, int,
                                   struct lsfAuth *, int *, int,
                                   struct jData **);
extern int                  chkAskedHosts(int, char **, int, int *,
                                          struct askedHost **,
                                          int *, int *, int);
extern int                  selectJobs(struct jobInfoReq *,
                                      struct jData ***, int *);
extern int                  signalJob(struct signalReq *, struct lsfAuth *);
extern int                  statusJob(struct statusReq *, struct hostent *,
                                      int *);
extern int                  rusageJob(struct statusReq *, struct hostent *);
extern int                  statusMsgAck(struct statusReq *);
extern int                  switchJobArray(struct jobSwitchReq *,
                                           struct lsfAuth *);
extern int                  sbatchdJobs(struct sbdPackage *, struct hData *);
extern int                  countNumSpecs(struct hData *);
extern void                 packJobSpecs(struct jData *, struct jobSpecs *);
extern void                 freeJobSpecs(struct jobSpecs *);
extern int                  peekJob(struct jobPeekReq *,
                                    struct jobPeekReply *,
                                    struct lsfAuth *);
extern int                  migJob(struct migReq *,
                                   struct submitMbdReply *,
                                    struct lsfAuth *);
extern void                 clean(time_t);
extern int                  moveJobArray(struct jobMoveReq *,
                                         int,
                                         struct lsfAuth *);
extern void                 job_abort(struct jData *jData, char reason);
extern void                 marktime(struct jData *, int);
extern int                  rmjobfile(struct jData *jData);
extern void                 jStatusChange(struct jData *,
                                          int,
                                          time_t,
                                          const char *);
extern int                  findLastJob(int, struct jData *, struct jData **);
extern void                 initJobIdHT(void);
extern struct jData *       getJobData(LS_LONG_INT jobId);
extern void                 inPendJobList(struct jData *, int list, time_t);
extern void                 inStartJobList (struct jData *);
extern void                 inFinishJobList(struct jData *);
extern void                 jobInQueueEnd (struct jData *, struct qData *);
extern struct jData *       initJData (struct jShared *);
extern void                 assignLoad (float *, float *, struct qData *,
                                        struct hData *);
extern int                  resigJobs (int *resignal);
extern void                 removeJob(LS_LONG_INT);
extern bool_t               runJob(struct runJobRequest *, struct lsfAuth *);
extern void                 addJobIdHT(struct jData *);
extern struct jData      *createjDataRef (struct jData *);
extern void               destroyjDataRef(struct jData *);
extern void             setJobPendReason(struct jData *, int);
extern void             destroySharedRef(struct jShared *);
extern struct jShared * createSharedRef (struct jShared *);
extern time_t runTimeSinceResume(const struct jData *);

extern void   modifyJobPriority(struct jData *, int);
extern float    queueGetUnscaledRunTimeLimit(struct qData *qp);
extern int    arrayRequeue(struct jData *,
                           struct signalReq *,
                           struct lsfAuth *);

extern int                  modifyJob (struct modifyReq *,
                                       struct submitMbdReply *,
                                       struct lsfAuth *);
extern void                 freeJData (struct jData *);
extern void                 handleJParameters (struct jData *, struct jData *,
                                               struct submitReq *, int, int, int);
extern void                 handleNewJob (struct jData *, int, int);
extern void                 copyJobBill (struct submitReq *,
                                         struct submitReq *, LS_LONG_INT);
extern void                 inZomJobList (struct jData *, int);
extern struct jData *       getZombieJob (LS_LONG_INT);
extern int                  getNextJobId (void);
extern void                 accumRunTime (struct jData *, int, time_t);
extern void                 signalReplyCode (sbdReplyType reply,
                                             struct jData *jData,
                                             int sigValue, int chkFlags);
extern void                 jobStatusSignal(sbdReplyType reply,
                                            struct jData *jData,
                                            int sigValue, int chkFlags,
                                            struct jobReply *jobReply);
extern void                 tryResume (void);
extern void                 freeSubmitReq (struct submitReq *);
extern int                  shouldLockJob (struct jData *, int);
extern int                  sigPFjob (struct jData *, int, time_t, int);
extern void                 offJobList (struct jData *, int);
extern void                 handleRequeueJob (struct jData *, time_t);
extern int                  PJLorMJL(struct jData *);

extern void                 schedulerInit(void);
extern int                  scheduleAndDispatchJobs(void);
extern int                  scheduleJobs(int *schedule, int *dispatch,
                                         struct jData *);
extern int                  dispatchJobs(int *dispatch);
extern void                 updNumDependents(struct dptNode *, int);
extern int                  userJobLimitCk (struct jData *, int disp);
extern int                  pJobLimitOk (struct hData *, struct hostAcct *,
                                         float pJobLimit);
extern int                  uJobLimitOk (struct jData *, struct hTab *,
                                         int, int disp);
extern int                  hostSlots (int, struct jData *, struct hData *,
                                       int disp, int *);
extern void                 disp_clean_job(struct jData *);
extern bool_t               dispatch_it(struct jData *);
extern int                  findBestHosts (struct jData *, struct resVal *, int, int, struct candHost *, bool_t);
extern int                  hJobLimitOk (struct hData *, struct hostAcct *, int);
extern void                 freeReserveSlots (struct jData *);
extern int                  jobStartError(struct jData *jData,
                                          sbdReplyType reply);
extern int                  cntNumPrmptSlots (struct qData *, struct hData *,
                                              struct uData *,
                                              int *numAvailSUSP);
extern int                  skipAQueue(struct qData *qp2, struct qData *qp1);
extern int                  userJobLimitOk (struct jData *, int, int *);
extern int                  getQUsable (struct qData *);
extern int                  handleForeignJob (struct jData *);
extern int                  reservePreemptResourcesForHosts(struct jData *jp);
extern int                  freeReservePreemptResources(struct jData *jp);
extern int                  deallocReservePreemptResources(struct jData *jp);
extern int                  orderByStatus (struct candHost *, int , bool_t);
extern void                 setLsbPtilePack(const bool_t );
extern int                  do_submitReq(XDR *, int, struct sockaddr_in *,
                                         char *, struct LSFHeader *,
                                         struct sockaddr_in *,
                                         struct lsfAuth *, int *, int,
                                         struct jData **);
extern int                  do_signalReq(XDR *, int, struct sockaddr_in *,
                                         char *, struct LSFHeader *,
                                         struct lsfAuth *);
extern int                  do_jobMsg(struct bucket *, XDR *, int,
                                      struct sockaddr_in *,
                                      char *, struct LSFHeader *,
                                      struct lsfAuth *);
extern int                  do_statusReq(XDR *, int, struct sockaddr_in *,
                                         int *,
                                         struct LSFHeader *);
extern int                  do_errorReq(int,  struct LSFHeader *);
extern int                  do_jobSwitchReq(XDR *, int, struct sockaddr_in *,
                                            char *, struct LSFHeader *,
                                            struct lsfAuth *);
extern int                  do_hostInfoReq(XDR *, int, struct sockaddr_in *,
                                           struct LSFHeader *);
extern int                  do_jobPeekReq(XDR *, int, struct sockaddr_in *,
                                          char *, struct LSFHeader *,
                                          struct lsfAuth *);
extern int                  do_jobInfoReq(XDR *, int, struct sockaddr_in *,
                                          struct LSFHeader *, int);
extern int                  do_queueInfoReq(XDR *, int, struct sockaddr_in *,
                                            struct LSFHeader *);
extern int                  do_debugReq(XDR * xdrs, int chfd,
                                        struct sockaddr_in * from,
                                        char *hostName,
                                        struct LSFHeader * reqHdr,
                                        struct lsfAuth * auth);
extern int                  do_groupInfoReq(XDR *, int, struct sockaddr_in *,
                                            struct LSFHeader *);
extern int                  do_queueControlReq(XDR *, int,
                                               struct sockaddr_in *, char *,
                                               struct LSFHeader *,
                                               struct lsfAuth *);
extern int                  do_reconfigReq(XDR *, int, struct sockaddr_in *,
                                           char *, struct LSFHeader *);
extern int                  do_restartReq(XDR *, int, struct sockaddr_in *,
                                          struct LSFHeader *);
extern int                  do_hostControlReq(XDR *, int,
                                              struct sockaddr_in *, char *,
                                              struct LSFHeader *,
                                              struct lsfAuth *);
extern int                  do_jobMoveReq(XDR *, int, struct sockaddr_in *,
                                          char *, struct LSFHeader *,
                                          struct lsfAuth *);
extern int                  do_userInfoReq(XDR *, int , struct sockaddr_in *,
                                           struct LSFHeader *);
extern int                  do_paramInfoReq(XDR *, int , struct sockaddr_in *,
                                            struct LSFHeader *);
extern int                  do_hostPartInfoReq(XDR *, int ,
                                               struct sockaddr_in *,
                                               struct LSFHeader *);
extern int                  do_migReq(XDR *, int, struct sockaddr_in *, char *,
                                      struct LSFHeader *, struct lsfAuth *);
extern int                  do_modifyReq (XDR *, int, struct sockaddr_in *,
                                          char *, struct LSFHeader *,
                                          struct lsfAuth *);
extern void                 doNewJobReply(struct sbdNode *, int);
extern void                 doProbeReply(struct sbdNode *, int);
extern void                 doSignalJobReply(struct sbdNode *sbdPtr, int);
extern void                 doSwitchJobReply(struct sbdNode *sbdPtr, int);
extern int                  do_resourceInfoReq (XDR *, int,
                                                struct sockaddr_in *,
                                                struct LSFHeader *);
extern int                   do_runJobReq(XDR *,
                                         int,
                                         struct sockaddr_in *,
                                         struct lsfAuth *,
                                         struct LSFHeader   *);
extern int                  getQUsable (struct qData *);
extern void                 allocateRemote(struct jData *, int);
extern void                 setExecHostsAcceptInterval(struct jData *);
#if defined(INTER_DAEMON_AUTH)
extern int authDaemonRequest(int chfd,
                             XDR *xdrs,
                             struct LSFHeader *reqHdr,
                             struct sockaddr_in *from_host,
                             char *client,
                             char *server);
#endif


extern int                  requeueEParse (struct requeueEStruct **,
                                           char *, int *);
extern int                  fill_requeueHist(struct rqHistory **,int *,
                                             struct hData *);
extern int                  match_exitvalue (struct requeueEStruct *, int);
extern void                 clean_requeue (struct qData *);

extern LS_BITSET_T          *allUsersSet;
extern LS_BITSET_T          *uGrpAllSet;
extern LS_BITSET_T          *uGrpAllAncestorSet;
extern int                  userSetOnNewUser(LS_BITSET_T *, void *,
                                             LS_BITSET_EVENT_T *);
extern int                  checkGroups(struct infoReq *,
                                        struct groupInfoReply *);
extern void                 fillMembers(struct gData *,
                                        char **,
                                        char);
extern char **              expandGrp(struct gData *,
                                      char *,
                                      int *);
extern void                 fillMembers(struct gData *,
                                        char **,
                                        char);
extern char *               getGroupMembers(struct gData *,
                                            char);
extern char *               catGnames(struct gData *);
extern struct gData *       getGroup(int, char *);
extern char                 gMember(char *word,
                                    struct gData *);
extern char                 gMemberForShare(char *word,
                                    struct gData *);
extern char                 gDirectMember(char *,
                                          struct gData *);
extern int                  countEntries(struct gData *, char );
extern struct gData *       getUGrpData(char *);
extern struct gData *       getHGrpData(char *);
extern struct gData *       getGrpData(struct gData **,
                                       char *,
                                       int);
extern int                  sumMembers (struct gData *,
                                        char r,
                                        int first);
extern void                 createGroupuData();
extern void                 createGroupTbPtr();
extern void                 createGroupSet();
extern int                  getIndexByuData(void *);
extern void *               getuDataByIndex(int);
extern UDATA_TABLE_T *      uDataTableCreate(void);
extern void                 uDataPtrTbInitialize(void);
extern void                 uDataTableAddEntry(UDATA_TABLE_T *,
                                               struct uData *);
extern int                  uDataTableGetNumEntries(UDATA_TABLE_T *);
extern struct uData *       uDataTableGetNextEntry(UDATA_TABLE_T *);
extern void                 setuDataCreate(void);
extern void                 updHostList(void);
extern void                 uDataGroupCreate(void);
extern int                  sizeofGroupInfoReply(struct groupInfoReply *);
extern void                 child_handler(int);
extern void                 terminate_handler(int);
extern void                 announce_master(void);
extern void                 shutDownClient(struct clientNode *);
extern void                 setNextSchedTimeUponNewJob(struct jData *);
extern void                 setJobPriUpdIntvl(void);
extern int                  isAuthManagerExt(struct lsfAuth *);
extern void                 updCounters(struct jData *jData, int newStatus,
                                        time_t);
extern void                 updSwitchJob (struct jData *, struct qData *,
                                          struct qData *, int);
extern void                 updUserData (struct jData *, int, int, int, int,
                                         int, int);
extern void                 updQaccount(struct jData *jData, int, int, int,
                                        int, int, int);
extern struct uData *       getUserData(char *user);
extern struct userAcct *    getUAcct(struct hTab *, struct uData *);
extern struct hostAcct *    getHAcct(struct hTab  *, struct hData *);
extern void                 initUserGroup (struct uData *);
extern struct uData *       addUserData (char *, int, float, int, int, char *, int, int);
extern int                  checkUsers(struct infoReq *,
                                       struct userInfoReply *);
extern void                 checkParams (struct infoReq *,
                                         struct parameterInfo *);
extern void                 mbdDie(int);
extern int                  isManager (char *);
extern int                  isAuthManager (struct lsfAuth *);
extern int                  isJobOwner(struct lsfAuth *, struct jData *);
extern char *               getDefaultProject(void);
extern void                 updResCounters(struct jData *, int);
extern struct hostAcct *    addHAcct(struct hTab **, struct hData *,
                                      int, int, int, int);
extern void                 checkQusable(struct qData *, int, int);
extern void                 updHostLeftRusageMem(struct jData *, int);
extern void                 recoverShareAccts();
extern void                 updJobSAcctForSwitch(struct jData *, struct qData *, struct qData *);
extern int                  minit(int);
extern struct qData *       lostFoundQueue(void);
extern void                 freeHData(struct hData *);
extern void                 deleteQData(struct qData *);

extern int                  my_atoi(char *, int, int);
extern void                 freeKeyVal(struct keymap *);
extern void                 queueHostsPF(struct qData *, int *);
extern struct hData *       initHData(struct hData *);
extern int                  updAllConfCond(void);
extern void                 mbdReConf(int);

extern int                  log_newjob(struct jData *);
extern void                 log_switchjob(struct jobSwitchReq *,
                                          int, char *);
extern void                 log_movejob(struct jobMoveReq *, int , char *);
extern void                 log_startjob(struct jData *, int);
extern void                 log_startjobaccept(struct jData *);
extern void                 log_newstatus(struct jData *);
extern void                 log_chkpnt(struct jData *, int, int);
extern void                 log_mig(struct jData *, int, char *);
extern void                 log_route(struct jData *);
extern int                  log_modifyjob(struct modifyReq *, struct lsfAuth *);
extern void                 log_queuestatus(struct qData *, int, int, char*);
extern void                 log_hoststatus(struct hData *, int, int, char*);
extern void                 log_mbdStart(void);
extern void                 log_mbdDie(int);
extern void                 log_unfulfill(struct jData *);
extern void                 log_jobaccept(struct jData *);
extern void                 log_jobclean(struct jData *);
extern void                 log_jobforward(struct jData *);
extern void                 log_statusack(struct jData *);
extern void                 log_logSwitch(int);
extern void                 replay_requeuejob(struct jData *);
extern int                  init_log(void);
extern void                 switchELog(void);
extern int                  switch_log(void);
extern void                 checkAcctLog(void);
extern int                  switchAcctLog(void);
extern void                 logJobInfo(struct submitReq *, struct jData *,
                                       struct lenData *);
extern int                  rmLogJobInfo_(struct jData *, int);
extern int                  readLogJobInfo(struct jobSpecs *, struct jData *,
                                           struct lenData *, struct lenData *);
extern void                 log_signaljob(struct jData *, struct signalReq *,
                                          int, char *);
extern void                 log_jobmsg(struct jData *, struct lsbMsg *, int);
extern void                 log_jobmsgack(struct bucket *);
extern char *               readJobInfoFile(struct jData *, int *);
extern void                 writeJobInfoFile(struct jData * , char *, int);
extern int                  replaceJobInfoFile(char *, char *, char *, int);
extern void                 log_executejob (struct jData *);
extern void                 log_jobsigact (struct jData *, struct statusReq *,
                                           int);
extern void                 log_jobrequeue(struct jData * jData);
extern void                 log_jobForce(struct jData *, int, char *);

extern time_t               eventTime;
extern void                 log_jobattrset(struct jobAttrInfoEnt *, int);

#define  REPLACE_1ST_CMD_ONLY  (0x1)
extern int                  stripJobStarter(char *, int *, char *);

extern sbdReplyType         start_job(struct jData *, struct qData *,
                                      struct jobReply *);
extern sbdReplyType         signal_job(struct jData *jobPtr, struct jobSig *,
                                       struct jobReply *jobReply);
extern sbdReplyType         switch_job(struct jData *, int options);
extern sbdReplyType         msg_job(struct jData *, struct Buffer *,
                                    struct jobReply *);
extern sbdReplyType         probe_slave(struct hData *, char sendJobs);
extern sbdReplyType         rebootSbd(char *host);
extern sbdReplyType         shutdownSbd(char *host);
extern struct dptNode *     parseDepCond(char *, struct lsfAuth * ,
                                         int *, char **,int *, int);
extern int                  evalDepCond (struct dptNode *, struct jData *);
extern void                 freeDepCond (struct dptNode *);
extern void                 resetDepCond (struct dptNode *);
extern bool_t               autoAdjustIsEnabled(void);
extern int                  getAutoAdjustAtNumPend(void);
extern float                  getAutoAdjustAtPercent(void);
extern bool_t               autoAdjustIsEnabled(void);
extern float                getHRValue(char *, struct hData *,
                                       struct resourceInstance **);
extern int                  checkResources (struct resourceInfoReq *,
                                            struct lsbShareResourceInfoReply *);
extern struct sharedResource * getResource (char *);
extern void                 resetSharedResource(void);
extern void                 updSharedResourceByRUNJob(const struct jData*);
extern int                  sharedResourceUpdFactor;
extern void                 freeSharedResource(void);
extern void                 newPRMO(char *);
extern void                 delPRMO();
extern void                 resetPRMOValues(int);
extern void                 printPRMOValues();
extern void                 mbdReconfPRMO();
extern float                getUsablePRHQValue(int, struct hData *,
                                struct qData *, struct resourceInstance **);
extern float                getAvailableByPreemptPRHQValue(int,
                                struct hData *, struct qData *);
extern float                getReservedByWaitPRHQValue(int,
                                struct hData *, struct qData *);
extern float                takeAvailableByPreemptPRHQValue(int, float,
                                struct hData *, struct qData *);
extern void                 addRunJobUsedPRHQValue(int, float,
                                struct hData *, struct qData *);
extern void                 removeRunJobUsedPRHQValue(int, float,
                                struct hData *, struct qData *);
extern void                 addReservedByWaitPRHQValue(int, float,
                                struct hData *, struct qData *);
extern void                 removeReservedByWaitPRHQValue(int, float,
                                struct hData *, struct qData *);
extern void                 addAvailableByPreemptPRHQValue(int, float,
                                struct hData *, struct qData *);
extern void                 removeAvailableByPreemptPRHQValue(int, float,
                                struct hData *, struct qData *);
extern int                  resName2resIndex(char *);
extern int                  isItPreemptResourceName(char *);
extern int                  isItPreemptResourceIndex(int);
extern int                  isReservePreemptResource(struct  resVal *);
extern int                  isHostsInSameInstance(int, struct hData *,
                                struct hData *);



extern void                 freeIdxList(struct idxList *);
extern struct idxList      *parseJobArrayIndex(char *, int *, int *);
extern void                 handleNewJobArray(struct jData *, struct idxList *, int);
extern void                 offArray(struct jData *);
extern int                  getJobIdList (char *, int *, LS_LONG_INT **);
extern int                  getJobIdIndexList(char *, int *, struct idxList **);
extern struct jData        *copyJData(struct jData *);
extern struct jShared      *copyJShared(struct jData *);
extern struct idxList      *getIdxListContext(void);
extern void                 setIdxListContext(const char *);
extern void                 freeIdxListContext(void);

#define FIRST_CHILD(x)   (x)->child
#define PARENT(x)        (x)->parent
#define LEFT_SIBLING(x)  (x)->left
#define RIGHT_SIBLING(x) (x)->right
#define IS_ANCESTOR(x,y) is_ancestor(x,y)
#define IS_PARENT(x,y)   (((x)==PARENT(y))? TRUE:FALSE)
#define IS_CHILD(x,y)    is_child(x,y)
#define IS_SIBLING(x,y)  ((PARENT(x) == PARENT(y))? TRUE:FALSE)

extern long   schedSeqNo;
#include "proxy.h"

#define QUEUE_LIMIT(qPtr, i) \
     ((qPtr)->defLimits[i] > 0 ? \
     (qPtr)->defLimits[i] : (qPtr)->rLimits[i])
#define LIMIT_OF_JOB(jp, i) \
    ((jp)->shared->jobBill.rLimits[i] > 0 ? \
     (jp)->shared->jobBill.rLimits[i] : \
     ((jp)->qPtr->defLimits[i] > 0 ? \
      (jp)->qPtr->defLimits[i] : (jp)->qPtr->rLimits[i]) \
    )
#define RUN_LIMIT_OF_JOB(jp) LIMIT_OF_JOB(jp, LSF_RLIMIT_RUN)
#define CPU_LIMIT_OF_JOB(jp) LIMIT_OF_JOB(jp, LSF_RLIMIT_CPU)

#define IGNORE_DEADLINE(qp) ((qp)->qAttrib & Q_ATTRIB_IGNORE_DEADLINE)
#define HAS_RUN_WINDOW(qp) (((qp)->windows != NULL) && \
                            (qp)->windows[0] != '\0')


#define NOT_NUMERIC(res) (((res).valueType != LS_NUMERIC))

#define DESTROY_BACKFILLEE_LIST(backfilleeList) \
    if ((backfilleeList) != NULL) { \
        listDestroy((backfilleeList), NULL); \
        (backfilleeList) = NULL; \
    }


#define FREE_IND_CANDPTR( candPtr, num ) { \
                               if ((candPtr)) { \
                                   int ii; \
                                   for (ii=0; ii< (num); ii++) \
                                       DESTROY_BACKFILLEE_LIST((candPtr)[ii].backfilleeList); \
                                   FREEUP((candPtr)); \
                               } \
                           }

#define FREE_CAND_PTR(jp) FREE_IND_CANDPTR((jp)->candPtr, (jp)->numCandPtr)

#define FREE_ALL_GRPS_CAND(job) { \
                     int jj;  \
                     if ((job)->groupCands != NULL) { \
                         for (jj = 0; jj < (job)->numOfGroups ; jj++) { \
                             FREE_IND_CANDPTR((job)->groupCands[jj].members, (job)->groupCands[jj].numOfMembers); \
                         } \
                     }\
                     FREEUP((job)->groupCands);\
                     (job)->numOfGroups = 0; \
                     (job)->reservedGrp = (job)->currentGrp = -1; \
                     FREEUP((job)->inEligibleGroups); \
                  }
#define USE_LOCAL        0x01
#define CHK_TCL_SYNTAX 0x02
#define PARSE_XOR        0x04

extern void copyJUsage(struct jRusage*, struct jRusage*);

extern struct timeWindow *newTimeWindow (void);
extern void freeTimeWindow(struct timeWindow *);
extern void updateTimeWindow(struct timeWindow *);
extern inline int numofhosts(void);
#endif

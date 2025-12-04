/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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

#define SUSP_CAN_PREEMPT_FOR_RSRC(s) !((s)->jStatus & JOB_STAT_USUSP)

#define UNREACHABLE(s) (((s) & HOST_STAT_UNREACH) || ((s) & HOST_STAT_UNAVAIL))
#define RESUME_JOB    1
#define RESERVE_SLOTS 2
#define PREEMPT_FOR_RES 3
#define CANNOT_RESUME 4

#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
#endif



struct listSet  *voidJobList = NULL;

void                 freeNewJob(struct jData *newjob);
extern void          initResVal(struct resVal* );
extern int           userJobLimitOk (struct jData *, int, int *);
extern void          reorderSJL(void);

static int checkSubHost(struct jData *job);
static bool_t  checkUserPriority(struct jData *jp, int userPriority, int *errnum);
static int           queueOk(char *, struct jData *, int *, struct submitReq *,
                             struct lsfAuth *);
static int           acceptJob(struct qData *, struct jData *,
                               int *, struct lsfAuth *);
static int           getCpuLimit(struct jData *, struct submitReq *);
static void          insertAndShift(struct jData *, struct jData *, int, int);
static int           resigJobs1(struct jData *, int *);
static void          sndJobMsgs(struct hData *, int *);
static sbdReplyType  sigStartedJob(struct jData *, int, time_t, int);
static void          reorderSJL1(struct jData *);
static int           matchJobStatus(int, struct jData *);
static double        acumulateValue(double, double);
static void          accumulateRU(struct jData *, struct statusReq *);
static int           checkJobParams(struct jData *, struct submitReq *,
                                    struct submitMbdReply *, struct lsfAuth *);
static int           checkJobPendLimit(struct jData *, struct lsfAuth *, char *, int);
static struct submitReq* saveOldParameters(struct jData *);
static void              freeExecParams(struct jData *);
static int mergeSubReq (struct submitReq *to, struct submitReq *old,
                        struct modifyReq *req, const struct passwd* pwUser);

static void              copyHosts(struct submitReq *, struct submitReq *);
static struct submitReq* getMergeSubReq (struct jData *,
                                         struct modifyReq *, int *);
static struct jData*     isInZomJobList(struct hData *, struct statusReq *);
static void              mailUser(struct jData *);
static void              changeJobParams(struct jData *);
static void              handleFinishJob(struct jData *, int, int);

static sbdReplyType      msgStartedJob(struct jData *, struct bucket *);
static void              breakCallback(struct jData *, bool_t terWhiPendStatus);
static int               rUsagesOk(struct resVal *, struct resVal *);
static void              packJobThresholds(struct thresholds *,
                                           struct jData *);

extern void initResVal (struct resVal *resVal);

static void              resetReserve(struct jData *, int);
static void              getReserveParams(struct resVal *, int *, int *);
static int               shouldResume(struct jData *, int *);
static int               shouldResumeByLoad(struct jData *);
static int               shouldResumeByRes(struct jData *);
static void              freeThresholds(struct thresholds *);

static char              ususpPendingEvent(struct jData *jpbw);
static char              terminatePendingEvent(struct jData *jpbw);
static void              initSubmitReq(struct submitReq *);
static int               skipJobListByReq (int, int);
static void              replaceString (char *, char *, char *);
static void initJobSig (struct jData *, struct jobSig *, int, time_t, int);
static int modifyAJob (struct modifyReq *, struct submitMbdReply *,
                       struct lsfAuth *, struct jData *);
static bool_t isSignificantChange(struct jRusage *, struct jRusage *, float );
static bool_t clusterAdminFlag;
static void setClusterAdmin(bool_t admin);
static bool_t requestByClusterAdmin(void);

void   expandFileNameWithJobId(char *, char *, LS_LONG_INT);
char * getJobAttaDataLocation(struct jData *, int);
static void jobRequeueTimeUpdate(struct jData *, time_t);

static bool_t clusterAdminFlag;
static void setClusterAdmin(bool_t admin);
static bool_t requestByClusterAdmin( );
static int    mbdRcvJobFile(int, struct lenData *);

static void closeSbdConnect4ZombieJob(struct jData *);
extern int glMigToPendFlag;
extern int requeueToBottom;
extern int arraySchedOrder;

extern int statusChanged;

#define DEFAULT_LISTSIZE    200


int            numRemoveJobs = 0;
int            eventPending = FALSE;

extern int     rusageUpdateRate;
extern int     rusageUpdatePercent;

static void    rLimits2lsfLimits(int*, struct lsfLimit*, int, int);

static int     setUrgentJobExecHosts(struct runJobRequest*, struct jData*);

extern int  jobsOnSameSegment(struct jData *, struct jData *, struct jData *);

extern int chkFirstHost( char*, int*);

void cleanCandHosts (struct jData *);
void cleanSbdNode(struct jData *jpbw);

void setNewSub(struct jData *, struct jData *, struct submitReq *, struct
               submitReq *, int, int );
static void updateStopJobPreemptResources(struct jData *);

extern int getXdrStrlen(char *);

static int rusgMatch(struct resVal* resValPtr, const char *resName);

static int   switchAJob(struct jobSwitchReq *,
                        struct lsfAuth      *,
                        struct qData        *);
static int   moveAJob (struct jobMoveReq *, int log, struct lsfAuth *);
static int staticNumPendJobs (void);

int
newJob (struct submitReq *subReq, struct submitMbdReply *Reply, int chan,
        struct lsfAuth *auth, int *schedule, int dispatch,
        struct jData **jobData)
{
    static char fname[] = "newJob";
    static struct jData *newjob;
    int returnErr;
    LS_LONG_INT nextId;
    char jobIdStr[20];
    struct lenData jf;
    struct hData *hData;
    struct hostInfo *hinfo;
    char hostType[MAXHOSTNAMELEN];
    static char pendLimitReason[MAX_CMD_DESC_LEN];

    struct idxList *idxList;
    int    maxJLimit = 0;
    int    arraySize = 1;

    pendLimitReason[0] = '\0';

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);

    if ((nextId = getNextJobId()) < 0)
        return (LSBE_NO_JOBID);

    hData = getHostData (subReq->fromHost);
    if (hData == NULL) {

        if ((hinfo = getLsfHostData (subReq->fromHost)) == NULL) {
            getLsfHostInfo(FALSE);
            hinfo = getLsfHostData (subReq->fromHost);
        }
        if (hinfo == NULL) {
            if (!(subReq->options & SUB_RESTART)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6500,
                                                 "%s: Host <%s> is not used by LSF"), /* catgets 6500 */
                          fname, subReq->fromHost);
                return (LSBE_MBATCHD);
            }
            if (getHostByType (subReq->schedHostType) == NULL) {
                ls_syslog(LOG_ERR, "\
%s: Can not find restarted job's submission host %s and type %s",
                          __func__, subReq->fromHost,
                          subReq->schedHostType);
                return LSBE_BAD_SUBMISSION_HOST;
            }
            strcpy (hostType, subReq->schedHostType);
        } else
            strcpy (hostType, hinfo->hostType);

    } else {
        strcpy (hostType, hData->hostType);
    }

    subReq->options2 &= ~(SUB2_HOST_NT | SUB2_HOST_UX);

    if (auth->options == AUTH_HOST_NT)
        subReq->options2 |= SUB2_HOST_NT;
    else if (auth->options == AUTH_HOST_UX)
        subReq->options2 |= SUB2_HOST_UX;

    newjob = initJData((struct jShared *) my_calloc(1, sizeof(struct jShared), "newJob"));
    newjob->jobId = nextId;
    returnErr = checkJobParams (newjob, subReq, Reply, auth);

    if (returnErr == LSBE_NO_ERROR) {

        if ( !(subReq->options & SUB_CHKPNT_DIR) ){
            struct qData  *qp = getQueueData(subReq->queue);
            if ( qp && (qp->qAttrib & Q_ATTRIB_CHKPNT) ) {
                subReq->options  |= SUB_CHKPNTABLE;
                subReq->options2 |= SUB2_QUEUE_CHKPNT;
                FREEUP(subReq->chkpntDir);
                subReq->chkpntDir = safeSave(qp->chkpntDir);
                subReq->chkpntPeriod = qp->chkpntPeriod;
            }
        }

        if ( !(subReq->options & SUB_RERUNNABLE ) ) {
            struct qData  *qp = getQueueData(subReq->queue);
            if ( qp && (qp->qAttrib & Q_ATTRIB_RERUNNABLE )) {
                subReq->options  |= SUB_RERUNNABLE;
                subReq->options2 |= SUB2_QUEUE_RERUNNABLE;
            }
        }


        if ((subReq->options & SUB_CHKPNT_DIR)
            && (!(subReq->options & SUB_RESTART))) {
            char dir[MAXLINELEN];
            sprintf(jobIdStr, "/%s", lsb_jobidinstr(nextId));
            strcpy(dir, subReq->chkpntDir);
            strcat(dir, jobIdStr);
            FREEUP(subReq->chkpntDir);
            subReq->chkpntDir = safeSave(dir);
        }

    }


    if ((mbdRcvJobFile(chan, &jf)) == -1) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6502,
                                         "%s: %s() failed for user ID <%d>: %M"), /* catgets 6502 */
                  fname, "mbdRcvJobFile", auth->uid);
        freeNewJob (newjob);
        if (returnErr != LSBE_NO_ERROR)
            return (returnErr);
        else
            return (LSBE_MBATCHD);
    }

    if (returnErr != LSBE_NO_ERROR) {
        freeNewJob (newjob);
        FREEUP (jf.data);
        return (returnErr);
    }


    copyJobBill (subReq, &newjob->shared->jobBill, newjob->jobId);
    newjob->restartPid = newjob->shared->jobBill.restartPid;
    newjob->chkpntPeriod = newjob->shared->jobBill.chkpntPeriod;


    newjob->schedHost = safeSave (hostType);

    if ((newjob->shared->jobBill.options & SUB_RESTART) ||
        (idxList = parseJobArrayIndex(newjob->shared->jobBill.jobName,
                                      &returnErr, &maxJLimit)) == NULL) {
        arraySize = 1;
        returnErr = checkJobPendLimit(newjob, auth, pendLimitReason, arraySize);
        if (returnErr == LSBE_NO_ERROR) {
            handleNewJob (newjob, JOB_NEW, LOG_IT);
        }
        else {
            goto error_cleanup;
        }
    }
    else {
        arraySize = (idxList->end - idxList->start)/idxList->step + 1;
        returnErr = checkJobPendLimit(newjob, auth, pendLimitReason, arraySize);
        if (returnErr == LSBE_NO_ERROR) {
            handleNewJobArray(newjob, idxList, maxJLimit);
            freeIdxList(idxList);
        }
        else {
            goto error_cleanup;
        }

    }
    Reply->jobId = newjob->jobId;
    *jobData = newjob;

    if (logclass & (LC_TRACE | LC_EXEC | LC_SCHED))
        ls_syslog(LOG_DEBUG1, "%s: New job <%s> submitted to queue <%s>",
                  fname, lsb_jobid2str(newjob->jobId), newjob->qPtr->queue);

    logJobInfo(subReq, newjob, &jf);
    FREEUP (jf.data);

    return(LSBE_NO_ERROR);

error_cleanup:
    FREEUP (jf.data);
    FREEUP(Reply->badJobName);
    freeIdxList(idxList);
    Reply->badJobName = safeSave(newjob->shared->jobBill.jobName);
    strncpy(Reply->pendLimitReason, pendLimitReason, MAX_CMD_DESC_LEN);
    freeJData(newjob);
    return(returnErr);
}

struct hData *
getHostByType(char *hostType)
{
    struct hData *hPtr;

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        if (strcmp (hostType, hPtr->hostType) != 0)
            continue;

        return hPtr;
    }

    return NULL;
}


int
getNextJobId (void)
{
    static char fname[] = "getNextJobId()";
    int i = 0;
    int freeJobId;


    nextJobId = (nextJobId < maxJobId)? nextJobId : 1;

    while ((getJobData (nextJobId) != NULL
            && i <= maxJobId)) {
        nextJobId++;
        i++;
        if (nextJobId >= maxJobId)
            nextJobId = 1;

    }

    if (i >= maxJobId) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6504,
                                         "%s: Too many jobs in the system; can't accept any new job for the moment"), /* catgets 6504 */
                  fname);
        return (-1);
    }
    freeJobId = nextJobId;
    nextJobId++;
    if (nextJobId >= maxJobId)
        nextJobId = 1;
    return (freeJobId);

}



void
addJobIdHT(struct jData *job)
{
    static char fname[] = "addJobIdHT()";
    hEnt *ent;

    while ((ent=addMemb(&jobIdHT, job->jobId))== NULL)  {

        if (job == getJobData(job->jobId))
            return;

        if (mSchedStage != M_STAGE_REPLAY)
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6505,
                                             "%s: The jobId <%s> already exist and is overwritten by the new job"), /* catgets 6505 */
                      fname,
                      lsb_jobid2str(job->jobId));
        removeJob(job->jobId);
    }
    ent->hData = (int *) job;

}

void
handleNewJob(struct jData *jpbw, int job, int eventTime)
{
    ls_syslog(LOG_DEBUG, "%s: Entering this routine...", __func__);

    addJobIdHT(jpbw);
    inPendJobList(jpbw, PJL, 0);
    jpbw->nodeType = JGRP_NODE_JOB;
    jpbw->nextJob = NULL;

    jpbw->uPtr = getUserData(jpbw->userName);

    putOntoTree(jpbw, job);
    jpbw->jFlags |= JFLAG_READY2;

    if (mSchedStage != M_STAGE_REPLAY) {
        updQaccount(jpbw, jpbw->shared->jobBill.maxNumProcessors,
                    jpbw->shared->jobBill.maxNumProcessors, 0, 0, 0, 0);
        updUserData(jpbw, jpbw->shared->jobBill.maxNumProcessors,
                    jpbw->shared->jobBill.maxNumProcessors, 0, 0, 0, 0);
    }

    if (job == JOB_NEW && eventTime == LOG_IT) {
        log_newjob(jpbw);
        /*If job is submitted to fairshare tree, we need udpate user for default shareAcct*/
        updAliveUserInFSTree(jpbw->qPtr, jpbw->userName);  
    }

    if (job == JOB_REPLAY) {
        if (maxUserPriority > 0) {
            if (jpbw->shared->jobBill.userPriority < 0) {
                modifyJobPriority(jpbw, maxUserPriority/2);
            } else {
                modifyJobPriority(jpbw, jpbw->shared->jobBill.userPriority);
            }
            offJobList(jpbw, PJL);
            inPendJobList(jpbw, PJL, 0);
        }
    }

    if (jpbw->shared->jobBill.options2 & SUB2_HOLD) {
        jpbw->newReason = PEND_USER_STOP;
        jStatusChange(jpbw, JOB_STAT_PSUSP, -1, __func__);
    }
}

int
chkAskedHosts(int inNumAskedHosts, char **inAskedHosts, int numProcessors,
              int *outNumAskedHosts, struct askedHost **outAskedHosts,
              int *badHostIndx, int *askedOthPrio, int returnBadHost)
{
    static char fname[] = "chkAskedHosts";
    int currentsize = inNumAskedHosts + 1;
    int j, i, k;
    struct askedHost *askedHosts;
    int numAskedHosts = 0, numAskProcessors = 0;
    struct hData *hData;
    char hName[MAXHOSTNAMELEN];
    int len, priority;
    struct hostent *hp;
    int allSpecified = FALSE;
    int firstHostIndex = -1;
#define FIRST_HOST_PRIORITY (unsigned)-1/2

    if (logclass & (LC_EXEC))
        ls_syslog (LOG_DEBUG3, "%s: inNumAskedHosts=%d", fname,
                   inNumAskedHosts);

    *outNumAskedHosts = 0;
    *outAskedHosts = NULL;

    askedHosts = (struct askedHost *) my_calloc (currentsize,
                                                 sizeof (struct askedHost), fname);
    *askedOthPrio = -1;

    for (j = 0; j < inNumAskedHosts; j++) {
        struct gData *gp;
        char *sp;
        int needCheck = TRUE;

        strcpy (hName, inAskedHosts[j]);
        len = strlen (hName);
        if (len > 2 && ((sp = strchr (hName, '+')) != NULL)
            && isint_ (++sp)) {
            priority = atoi (sp);
            *(--sp) = '\0';
        }
        else if (len > 1 && hName[len-1] == '+') {
            priority = 1;
            hName[len-1] = '\0';
            for (i = len - 2; i > 1; i--)
                if (hName[i] == '+') {
                    priority++;
                    hName[i] = '\0';
                } else
                    break;
        } else {
            if (chkFirstHost(hName, &needCheck))  {

                if (firstHostIndex != -1) {
                    *badHostIndx = j;

                    if ( returnBadHost ) return (LSBE_MULTI_FIRST_HOST);
                    continue;
                }
                firstHostIndex = j;
                priority = FIRST_HOST_PRIORITY;
                hName[len-1] = '\0';
            } else {
                priority = 0;
            }
        }

        if (strcmp (hName, "others") == 0) {
            if (*askedOthPrio >= 0 && returnBadHost) {
                *badHostIndx = j;
                FREEUP(askedHosts);
                return (LSBE_BAD_HOST);
            }
            if ( firstHostIndex == j ) {
                *badHostIndx = j;

                if ( returnBadHost ) return (LSBE_OTHERS_FIRST_HOST);
                continue;
            }
            *askedOthPrio = priority;
            numAskProcessors = numofprocs;
            if (logclass & (LC_EXEC))
                ls_syslog(LOG_DEBUG3, "%s: askedOthPrio=%d", fname, priority);
            continue;
        }

        if ((gp = getHGrpData (hName)) != NULL) {

            char **gHosts;
            int numh = 0;


            if ( firstHostIndex == j ) {
                *badHostIndx = j;
                if (returnBadHost ) {
                    return (LSBE_HG_FIRST_HOST);
                }
                continue;
            }
            if (gp->memberTab.numEnts == 0 && gp->numGroups == 0) {
                if (*askedOthPrio >= 0 && returnBadHost) {
                    *badHostIndx = j;
                    FREEUP(askedHosts);
                    return (LSBE_BAD_HOST);
                }
                *askedOthPrio = priority;
                numAskProcessors = numofprocs;
                allSpecified = TRUE;
                continue;
            } else {
                gHosts = expandGrp(gp, hName, &numh);

                for (i = 0; i < numh; i++) {
                    if (strcmp (gHosts[i], "all") == 0) {
                        if (*askedOthPrio >= 0 && returnBadHost) {
                            *badHostIndx = j;
                            FREEUP(askedHosts);
                            return (LSBE_BAD_HOST);
                        }
                        *askedOthPrio = priority;
                        numAskProcessors = numofprocs;
                        allSpecified = TRUE;
                        continue;
                    }
                    if ((hData = getHostData (gHosts[i])) == NULL)
                        continue;
                    for (k = 0; k < numAskedHosts; k++)
                        if (hData == askedHosts[k].hData)
                            break;
                    if (k < numAskedHosts)
                        continue;
                    numAskProcessors += hData->numCPUs;
                    askedHosts[numAskedHosts].hData = hData;
                    askedHosts[numAskedHosts].priority = priority;
                    numAskedHosts++;
                    if (numAskedHosts >= currentsize) {
                        struct askedHost *temp;
                        currentsize = currentsize * 2;
                        temp = (struct askedHost *)realloc(askedHosts,
                                                           currentsize * sizeof(struct askedHost));
                        if (temp == NULL)
                            mbdDie(MASTER_MEM);
                        askedHosts = temp;
                    }
                }
                FREEUP(gHosts);
            }
        } else {
            if ((hp = Gethostbyname_(hName)) == NULL
                || (hData = getHostData(hp->h_name)) == NULL
                || (hData->flags & HOST_LOST_FOUND)
                || (hData->hStatus & HOST_STAT_REMOTE)) {
                if (!returnBadHost)
                    continue;
                *badHostIndx = j;
                FREEUP(askedHosts);
                return (LSBE_BAD_HOST);
            }

            for (k = 0; k < numAskedHosts; k++)
                if (hData == askedHosts[k].hData)
                    break;
            if (k < numAskedHosts)
                continue;
            numAskProcessors += hData->numCPUs;
            askedHosts[numAskedHosts].hData = hData;
            askedHosts[numAskedHosts].priority = priority;
            numAskedHosts++;
            if (numAskedHosts >= currentsize) {
                struct askedHost *temp;
                currentsize = currentsize * 2;
                temp = (struct askedHost *)realloc(askedHosts,
                                                   currentsize * sizeof(struct askedHost));
                if (temp == NULL)
                    mbdDie(MASTER_MEM);
                askedHosts = temp;
            }
        }
    }


    if (numAskedHosts == 0 && *askedOthPrio >= 0 && allSpecified == FALSE) {
        FREEUP(askedHosts);
        return LSBE_BAD_HOST;
    }

    if ((returnBadHost  && numProcessors > numAskProcessors)
        || (!returnBadHost && numAskedHosts == 0 && allSpecified == FALSE)) {
        FREEUP(askedHosts);
        return (LSBE_PROC_NUM);
    }




    for ( i=0; i < numAskedHosts; i++) {
        if ( (i != firstHostIndex)
             && (askedHosts[i].priority == FIRST_HOST_PRIORITY) ) {
            askedHosts[i].priority--;
        }
    }



    for (i = 0; i < numAskedHosts; i++) {
        int maxIndx = i;
        int maxPrio = askedHosts[i].priority;
        struct hData *tmpPtr;
        for (j = i + 1; j < numAskedHosts; j++)
            if (askedHosts[j].priority > maxPrio) {
                maxPrio = askedHosts[j].priority;
                maxIndx = j;
            }
        tmpPtr = askedHosts[i].hData;
        askedHosts[i].hData = askedHosts[maxIndx].hData;
        askedHosts[maxIndx].hData = tmpPtr;
        askedHosts[maxIndx].priority = askedHosts[i].priority;
        askedHosts[i].priority = maxPrio;
        if (logclass & (LC_EXEC))
            ls_syslog (LOG_DEBUG3, "%s: askedHosts[%d]=%s, prio=%d", fname, i,
                       askedHosts[i].hData != NULL?
                       askedHosts[i].hData->host: "others",
                       askedHosts[i].priority);
    }

    if( numAskedHosts ) {
        *outAskedHosts = askedHosts;
    } else {
        FREEUP(askedHosts);
        *outAskedHosts = NULL;
    }
    *outNumAskedHosts = numAskedHosts;

    if (logclass & LC_SCHED) {
        for (i = 0; i < numAskedHosts; i++)
            ls_syslog (LOG_DEBUG3, "%s: host <%s>, priority=%d", fname, askedHosts[i].hData->host, askedHosts[i].priority);
    }

    return (LSBE_NO_ERROR);
}

static int
queueOk (char *queuename, struct jData *job, int *errReqIndx,
         struct submitReq *subReq, struct lsfAuth *auth)
{
    struct qData *qp;
    int retVal;

    if ((qp = getQueueData (queuename)) == NULL)
        return (LSBE_BAD_QUEUE);
    if (qp->pJobLimit <= 0.0)
        return (LSBE_PJOB_LIMIT);
    if (qp->hJobLimit <= 0)
        return (LSBE_HJOB_LIMIT);
    if (qp->maxJobs <= 0)
        return (LSBE_QJOB_LIMIT);


    job->qPtr = qp;

    job->slotHoldTime = qp->slotHoldTime;

    if ((retVal = getCpuLimit (job, subReq)) != LSBE_NO_ERROR) {
        return (retVal);
    }


    if (job->shared->jobBill.options2 & SUB2_USE_DEF_PROCLIMIT) {
        job->shared->jobBill.numProcessors =
            job->shared->jobBill.maxNumProcessors =
            (qp->defProcLimit > 0 ? qp->defProcLimit : 1);
    }


    if (!userJobLimitOk (job, 0, errReqIndx)) {
        if (job->newReason == PEND_QUE_USR_JLIMIT
            || job->newReason == PEND_QUE_USR_PJLIMIT)
            return (LSBE_UJOB_LIMIT);
        else
            return (LSBE_USER_JLIMIT);
    }

    if ((retVal = acceptJob (qp, job, errReqIndx, auth)) != LSBE_NO_ERROR) {
        return (retVal);
    }
    return (LSBE_NO_ERROR);
}

static int
acceptJob(struct qData *qp,
          struct jData *jp,
          int *errReqIndx,
          struct lsfAuth *auth)
{
    int j;

    if (!(qp->qStatus & QUEUE_STAT_OPEN))
        return (LSBE_QUEUE_CLOSED);
    if ((jp->shared->jobBill.options & SUB_EXCLUSIVE)
        && !(qp->qAttrib & Q_ATTRIB_EXCLUSIVE))
        return (LSBE_EXCLUSIVE);


    if (jp->shared->jobBill.options2 & SUB2_USE_DEF_PROCLIMIT) {
        jp->shared->jobBill.numProcessors =
            jp->shared->jobBill.maxNumProcessors =
            (qp->defProcLimit > 0 ? qp->defProcLimit : 1);
    }

    if (qp->numProcessors > 0
        && qp->numProcessors < jp->shared->jobBill.numProcessors) {
        if (qp->numHUnAvail == 0)
            return LSBE_PROC_NUM;
    }

    if (qp->qAttrib & Q_ATTRIB_FS && getSAcctForJob(qp->policy, jp, 0) == NULL) {
        return (LSBE_QUEUE_USE);
    }

    if ((jp->shared->jobBill.options & SUB_INTERACTIVE)
        && (qp->qAttrib & Q_ATTRIB_NO_INTERACTIVE))
        return (LSBE_NO_INTERACTIVE);
    if (!(jp->shared->jobBill.options & SUB_INTERACTIVE)
        && (qp->qAttrib & Q_ATTRIB_ONLY_INTERACTIVE))
        return (LSBE_ONLY_INTERACTIVE);

    if (auth->uid != 0  && !isAuthManager(auth)
        && !requestByClusterAdmin( )
        && !userQMember (jp->userName, qp))
        return (LSBE_QUEUE_USE);

    if (qp->procLimit > 0
        && jp->shared->jobBill.numProcessors > qp->procLimit)
        return (LSBE_PROC_NUM);
    if (qp->minProcLimit > 0
        && jp->shared->jobBill.maxNumProcessors < qp->minProcLimit)
        return (LSBE_PROC_LESS);
    if (qp->maxJobs != INFINIT_INT
        && jp->shared->jobBill.numProcessors > qp->maxJobs)
        return (LSBE_PROC_NUM);

    for (j = 0; j < LSF_RLIM_NLIMITS; j++) {
        if (jp->shared->jobBill.rLimits[j] >= 0 &&  qp->rLimits[j] >= 0
            && jp->shared->jobBill.rLimits[j] > qp->rLimits[j]) {
            *errReqIndx = j;
            return (LSBE_OVER_LIMIT);
        }
    }


    for (j = 0; j < jp->numAskedPtr; j++) {
        ls_syslog(LOG_DEBUG3,
                  "acceptJob checking isHostQMember() host=%s queue=%s",
                  jp->askedPtr[j].hData->host, qp->queue);

        if (jp->askedPtr[j].hData != NULL
            && !isHostQMember (jp->askedPtr[j].hData, qp)) {
            *errReqIndx = j;
            ls_syslog(LOG_DEBUG3,"acceptJob Host %s IS NOT member queue=%s",
                      jp->askedPtr[j].hData->host, qp->queue);

            return (LSBE_QUEUE_HOST);

        }

        ls_syslog(LOG_DEBUG3,"acceptJob Host %s IS member queue=%s",
                  jp->askedPtr[j].hData->host, qp->queue);

    }

    if (IS_START (jp->jStatus)) {
        for (j = 0; j < jp->numHostPtr; j++) {
            if (jp->hPtr == NULL || jp->hPtr[j] == NULL)
                continue;
            ls_syslog(LOG_DEBUG3,"acceptJob: checking isHostQMember() host=%s",
                      jp->hPtr[j]->host);

            if (!isHostQMember (jp->hPtr[j], qp)){
                ls_syslog(LOG_DEBUG3,"acceptJob: Host %s IS NOT member",
                          jp->hPtr[j]->host);

                return (LSBE_QUEUE_HOST);
            }

            ls_syslog(LOG_DEBUG3,"acceptJob: Host %s IS member",
                      jp->hPtr[j]->host);

        }
    }

    if (jp->shared->resValPtr && qp->resValPtr)
        if (rUsagesOk (jp->shared->resValPtr, qp->resValPtr) == FALSE)
            return (LSBE_BAD_RESREQ);

    return (LSBE_NO_ERROR);
}

static int
getCpuLimit (struct jData *job, struct submitReq *subReq)
{
    float *cpuFactor, cpuLimit, runLimit;
    char  *spec;

    cpuLimit = subReq->rLimits[LSF_RLIMIT_CPU];
    runLimit = subReq->rLimits[LSF_RLIMIT_RUN];

    if (cpuLimit <= 0 && runLimit <= 0)
        return (LSBE_NO_ERROR);

    if (subReq->options & SUB_HOST_SPEC)
        spec = subReq->hostSpec;
    else if (job->qPtr->defaultHostSpec != NULL)
        spec = job->qPtr->defaultHostSpec;
    else if (defaultHostSpec != NULL)
        spec =  defaultHostSpec;
    else
        spec = subReq->fromHost;

    if ((cpuFactor = getModelFactor (spec)) == NULL)
        if ((cpuFactor = getHostFactor (spec)) == NULL)
            return (LSBE_BAD_HOST_SPEC);

    if (cpuLimit > 0) {
        if (*cpuFactor <= 0) {
            return (LSBE_BAD_LIMIT);
        }
        if (cpuLimit > (INFINIT_INT /(*cpuFactor))) {
            return (LSBE_BAD_LIMIT);
        }

        if ((cpuLimit *= *cpuFactor) < 1)
            cpuLimit = 1;
        else
            cpuLimit += 0.5;

        subReq->rLimits[LSF_RLIMIT_CPU] = cpuLimit;
        job->shared->jobBill.rLimits[LSF_RLIMIT_CPU] = cpuLimit;
    }
    if (runLimit > 0) {
        if (*cpuFactor <= 0) {
            return (LSBE_BAD_LIMIT);
        }


        if (runLimit > (INFINIT_INT /(*cpuFactor))) {
            return (LSBE_BAD_LIMIT);
        }
        if ((runLimit *= *cpuFactor) < 1)
            runLimit = 1;
        else
            runLimit += 0.5;
        subReq->rLimits[LSF_RLIMIT_RUN] = runLimit;
        job->shared->jobBill.rLimits[LSF_RLIMIT_RUN] = runLimit;
    }

    if (!(subReq->options & SUB_HOST_SPEC)) {

        FREEUP (subReq->hostSpec);
        subReq->hostSpec = (char *) my_malloc (MAXHOSTNAMELEN, "getCpuLimit");
        strcpy (subReq->hostSpec, spec);
        subReq->options |= SUB_HOST_SPEC;
    }
    return (LSBE_NO_ERROR);

}

void
freeNewJob (struct jData *newjob)
{
    FREEUP (newjob->userName);

    FREEUP (newjob->askedPtr);
    FREEUP (newjob->reqHistory);
    destroySharedRef(newjob->shared);
    FREEUP (newjob);
}

int
selectJobs (struct jobInfoReq *jobInfoReq, struct jData ***jobDataList,
            int *listSize)
{
    static char fname[] = "selectJobs()";
    char allqueues = FALSE;
    char allusers = FALSE;
    char allhosts = FALSE;
    char searchJobName = FALSE;
    struct jData *jpbw, **joblist = NULL, *recentJob = NULL;
    struct gData *uGrp = NULL;
    int  list = 0;
    int numJobs = 0;
    int arraysize = 0;
    struct  uData *uPtr;


    if (jobInfoReq->queue[0] == '\0')
        allqueues = TRUE;
    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        allusers = TRUE;
    else
        uGrp = getUGrpData (jobInfoReq->userName);

    if (jobInfoReq->host[0] == '\0')
        allhosts = TRUE;
    if (jobInfoReq->jobName[0] != '\0' &&
        jobInfoReq->jobName[strlen(jobInfoReq->jobName) - 1] == '*') {
        searchJobName = TRUE;
        jobInfoReq->jobName[strlen(jobInfoReq->jobName) - 1] = '\0';
    }


    uPtr = getUserData(jobInfoReq->userName);


    for (list = 0; list < NJLIST; list++) {
        struct jData *jp;
        if (skipJobListByReq (jobInfoReq->options, list)  == TRUE)
            continue;

        if (list == SJL && jDataList[list]->back != jDataList[list])
            reorderSJL ();

        for (jp = jDataList[list]->back;
             (jp!= jDataList[list]); jp = jp->back) {
            int i;

            jpbw = jp;

            if (jpbw->jobId < 0)
                continue;

            if (!allqueues
                && strcmp(jpbw->qPtr->queue, jobInfoReq->queue) != 0)
                continue;


            if (!allusers && (jpbw->uPtr != uPtr)) {
                if (uGrp == NULL)
                    continue;
                else if (!gMember(jpbw->userName, uGrp))
                    continue;
            }


            if (jobInfoReq->jobName[0] != '\0') {
                char  fullName[MAXPATHLEN];
                fullJobName_r(jpbw, fullName);
                if ((searchJobName == FALSE &&
                     strcmp(jobInfoReq->jobName, fullName) != 0) ||
                    (searchJobName == TRUE &&
                     strncmp(fullName, jobInfoReq->jobName,
                             strlen (jobInfoReq->jobName)) != 0))
                    continue;
            }



            if (jobInfoReq->jobId != 0
                && ((LSB_ARRAY_IDX(jobInfoReq->jobId) != 0
                     && LSB_ARRAY_IDX(jobInfoReq->jobId) != LSB_ARRAY_IDX(jpbw->jobId))
                    ||
                    LSB_ARRAY_JOBID(jobInfoReq->jobId) != LSB_ARRAY_JOBID(jpbw->jobId))) {

                continue;
            }

            {
                if (jpbw->jStatus & JOB_STAT_PEND) {
                    if (!(jpbw->qPtr->qStatus & QUEUE_STAT_RUN))
                        jpbw->newReason = PEND_QUE_WINDOW;
                    if (!(jpbw->qPtr->qStatus & QUEUE_STAT_ACTIVE))
                        jpbw->newReason = PEND_QUE_INACT;
                }
                else if (jpbw->jStatus & JOB_STAT_ZOMBIE)
                    jpbw->newReason |= EXIT_ZOMBIE;
            }

            if (! matchJobStatus(jobInfoReq->options, jpbw)) {
                continue;
            }


            if (!allhosts) {
                struct gData *gp;

                if (IS_PEND (jpbw->jStatus))
                    continue;

                if (jpbw->hPtr == NULL) {
                    if (!(jpbw->jStatus & JOB_STAT_EXIT))
                        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6510,
                                                         "%s: Execution host for job <%s> is null"), /* catgets 6510 */
                                  fname, lsb_jobid2str(jpbw->jobId));
                    continue;
                }

                gp = getHGrpData (jobInfoReq->host);
                if (gp != NULL) {
                    for (i = 0; i < jpbw->numHostPtr; i++) {
                        if (jpbw->hPtr[i] == NULL)
                            continue;
                        if (gMember(jpbw->hPtr[i]->host, gp))
                            break;
                    }
                    if (i >= jpbw->numHostPtr)
                        continue;
                } else {
                    for (i = 0; i < jpbw->numHostPtr; i++) {
                        if (jpbw->hPtr[i] == NULL)
                            continue;
                        if (equalHost_(jobInfoReq->host, jpbw->hPtr[i]->host))
                            break;
                    }
                    if (i >= jpbw->numHostPtr)
                        continue;
                }
            }


            if (findLastJob(jobInfoReq->options, jpbw, &recentJob) == FALSE)
                continue;

            if (arraysize == 0) {
                arraysize = DEFAULT_LISTSIZE;
                joblist = (struct jData **) calloc (arraysize,
                                                    sizeof (struct jData *));
                if (joblist == NULL)
                    return LSBE_NO_MEM;
            }
            if (numJobs >= arraysize) {

                struct jData **biglist;
                arraysize *= 2;
                biglist = (struct jData **) realloc((char *)joblist,
                                                    arraysize * sizeof (struct jData *));
                if (biglist == NULL) {
                    FREEUP(joblist);
                    return LSBE_NO_MEM;
                }
                joblist = biglist;
            }
            joblist[numJobs] = jpbw;
            numJobs++;
        }
    }

    *listSize = numJobs;

    if (numJobs > 0) {
        if(jobInfoReq->options & LAST_JOB) {
            numJobs = 1;
            joblist[0] = recentJob;
        }
        *jobDataList = joblist;
        return(LSBE_NO_ERROR);
    } else if (!allqueues && getQueueData (jobInfoReq->queue) == NULL) {
        FREEUP(joblist);
        return(LSBE_BAD_QUEUE);
    }
    FREEUP(joblist);
    return(LSBE_NO_JOB);

}

static int
skipJobListByReq (int options, int joblist)
{
    if (options & (ALL_JOB|JOBID_ONLY_ALL)) {
        return FALSE;
    } else if ((options & (CUR_JOB | LAST_JOB))
               && (joblist == SJL || joblist == PJL || joblist == MJL)) {
        return FALSE;
    } else if ((options & PEND_JOB) && (joblist == PJL || joblist == MJL)) {
        return FALSE;
    } else if ((options & (SUSP_JOB | RUN_JOB)) && (joblist == SJL)) {
        return FALSE;
    } else if ((options & DONE_JOB) && joblist == FJL) {
        return FALSE;
    } else if ((options & ZOMBIE_JOB) && (joblist == FJL)) {
        return FALSE;
    }

    return(TRUE);

}

void
reorderSJL (void)
{
    struct jData *tmpSJL, *jp, *next;


    tmpSJL = (struct jData *)
        tmpListHeader ((struct listEntry *) jDataList[SJL]);


    for (jp = tmpSJL->back; jp != tmpSJL; jp = next) {
        next = jp->back;
        reorderSJL1 (jp);
    }
}

static void
reorderSJL1 (struct jData *job)
{
    struct jData *jp;
    int found = FALSE;

    for (jp = jDataList[SJL]->forw; jp != jDataList[SJL]; jp = jp->forw) {
        if (!equalHost_(jp->hPtr[0]->host, job->hPtr[0]->host)) {
            if (found == TRUE)
                break;
            continue;
        }
        found = TRUE;
        if (job->qPtr->priority < jp->qPtr->priority)
            break;
        else if (job->qPtr->priority == jp->qPtr->priority) {
            if (job->startTime > jp->startTime)
                break;
            else if ((job->startTime == jp->startTime || job->startTime == 0)
                     && (job->jobId > jp->jobId))
                break;
        }
    }



    offList((struct listEntry *)job);

    if (found) {
        inList ((struct listEntry *)jp, (struct listEntry *)job);
    } else {
        inList ((struct listEntry *)jDataList[SJL]->forw,
                (struct listEntry *)job);
    }

}

static int
matchJobStatus(int options, struct jData *jobPtr)
{
    if (options & (ALL_JOB|JOBID_ONLY_ALL))
        return TRUE;

    if (((options & CUR_JOB) || (options & LAST_JOB))
        && !(IS_FINISH (jobPtr->jStatus)))
        return TRUE;
    if ((options & DONE_JOB) && IS_FINISH (jobPtr->jStatus))
        return TRUE;
    if ((options & PEND_JOB) && IS_PEND (jobPtr->jStatus))
        return TRUE;
    if ((options & SUSP_JOB) && IS_SUSP (jobPtr->jStatus)
        && !(jobPtr->jStatus & JOB_STAT_UNKWN))
        return TRUE;
    if ((options & RUN_JOB) && (jobPtr->jStatus & JOB_STAT_RUN))
        return TRUE;
    if ((options & ZOMBIE_JOB) && (jobPtr->jStatus & JOB_STAT_ZOMBIE)) {
        return TRUE;
    }
    return FALSE;

}

int
findLastJob(int options, struct jData *jobPtr, struct jData **recentJob)
{
    LS_LONG_INT jobIdDiff;

    if (!(options & LAST_JOB))
        return(TRUE);
    if (*recentJob == NULL) {
        *recentJob = jobPtr;
        return(TRUE);
    }
    if (jobPtr->shared->jobBill.submitTime < (*recentJob)->shared->jobBill.submitTime)
        return (FALSE);
    if (jobPtr->shared->jobBill.submitTime == (*recentJob)->shared->jobBill.submitTime) {


        if ((jobIdDiff = (*recentJob)->jobId - jobPtr->jobId) < 0)
            jobIdDiff += MAX_INTERNAL_JOBID;
        if (jobIdDiff < (MAX_INTERNAL_JOBID / 2))

            return (FALSE);

    }
    *recentJob = jobPtr;
    return FALSE;

}

int
peekJob (struct jobPeekReq *jpeekReq, struct jobPeekReply *jpeekReply,
         struct lsfAuth *auth)
{

    static char fname[]="peekJob";
    struct jData *job = NULL;
    char   jobFile[MAXFILENAMELEN];

    if (logclass & LC_EXEC) {
        ls_syslog(LOG_DEBUG,"%s: Entering peekJob ...", fname);
    }

    if ((job = getJobData (jpeekReq->jobId)) == NULL)
        return (LSBE_NO_JOB);

    if (job->nodeType != JGRP_NODE_JOB) {
        return(LSBE_JOB_ARRAY);
    }


    if (!isJobOwner(auth, job))
        return(LSBE_PERMISSION);

    if (IS_FINISH (job->jStatus))
        return (LSBE_JOB_FINISH);

    if (IS_PEND (job->jStatus))
        return (LSBE_NOT_STARTED);

    if (job->jStatus & JOB_STAT_PRE_EXEC)
        return (LSBE_NO_OUTPUT);

    if (LSB_ARRAY_IDX(job->jobId))
        sprintf(jobFile, "%s.%d", job->shared->jobBill.jobFile,
                LSB_ARRAY_IDX(job->jobId));
    else if (job->shared->jobBill.options & SUB_RESTART) {

        char *tmp;
        sprintf(jobFile, "%s", job->shared->jobBill.jobFile);
        if (strchr(jobFile, '.') != strrchr(jobFile, '.')) {
            tmp = strstr (jobFile, lsb_jobid2str(job->jobId));
            if (tmp != NULL) {
                --tmp;
                *tmp = '\0';
            }
        }
    } else {
        sprintf(jobFile, "%s", job->shared->jobBill.jobFile);
    }
    jpeekReply->outFile = safeSave(jobFile);

    if ((logclass & LC_EXEC)
        && job->jobSpoolDir != NULL) {
        ls_syslog(LOG_DEBUG,"%s: The job <%s>  JOB_SPOOL_DIR is <%s>",
                  fname, lsb_jobid2str(job->jobId), job->jobSpoolDir);
    }
    jpeekReply->pSpoolDir = safeSave(job->jobSpoolDir);

    return(LSBE_NO_ERROR);
}

int
migJob (struct migReq *req, struct submitMbdReply *reply, struct lsfAuth *auth)
{
    static char fname[] = "migJob";
    struct jData *job = NULL;
    int replyStatus, i, askedOthPrio;

    if ((job = getJobData (req->jobId)) == NULL)
        return (LSBE_NO_JOB);


    if (job->nodeType != JGRP_NODE_JOB) {
        return(LSBE_JOB_ARRAY);
    }

    if ((auth->uid != 0) && !jgrpPermitOk(auth, job->jgrpNode) &&
        !isAuthQueAd (job->qPtr, auth))
        return (LSBE_PERMISSION);

    if (IS_FINISH (job->jStatus))
        return (LSBE_JOB_FINISH);

    if (IS_PEND (job->jStatus) || (job->jStatus & JOB_STAT_PRE_EXEC))
        return (LSBE_NOT_STARTED);


    if (!(job->shared->jobBill.options & (SUB_CHKPNTABLE | SUB_RERUNNABLE)))
        return (LSBE_J_UNCHKPNTABLE);

    if (job->jStatus & JOB_STAT_MIG)
        return (LSBE_MIGRATION);

    if (LSB_ARRAY_IDX(job->jobId) != 0) {

        localizeJobElement(job);
    }

    if (req->numAskedHosts) {
        int realNumHosts, saveNumAskedPtr;
        struct askedHost *realAskedHosts, *saveAskedPtr;

        replyStatus = chkAskedHosts (req->numAskedHosts, req->askedHosts,
                                     job->shared->jobBill.numProcessors, &realNumHosts,
                                     &realAskedHosts, &reply->badReqIndx,
                                     &askedOthPrio, 1);

        if (replyStatus != LSBE_NO_ERROR) {
            return (replyStatus);
        } else if ( realNumHosts <= 0 ) {
            return LSBE_BAD_HOST;
        }


        saveNumAskedPtr = job->numAskedPtr;
        saveAskedPtr = job->askedPtr;
        job->numAskedPtr = realNumHosts;
        job->askedPtr = realAskedHosts;

        if ((replyStatus = acceptJob (job->qPtr, job, &reply->badReqIndx,
                                      auth)) != LSBE_NO_ERROR) {
            FREEUP (realAskedHosts);

            job->numAskedPtr = saveNumAskedPtr;
            job->askedPtr = saveAskedPtr;
            return (replyStatus);
        }


        if (job->shared->jobBill.numAskedHosts) {
            for (i = 0; i < job->shared->jobBill.numAskedHosts; i++)
                FREEUP (job->shared->jobBill.askedHosts[i]);
            FREEUP (job->shared->jobBill.askedHosts);
        }
        job->shared->jobBill.askedHosts = (char **) my_calloc (req->numAskedHosts,
                                                               sizeof (char *), fname);
        for (i = 0; i < req->numAskedHosts; i++)
            job->shared->jobBill.askedHosts[i] = safeSave (req->askedHosts[i]);
        job->shared->jobBill.numAskedHosts = req->numAskedHosts;

        if (saveNumAskedPtr) {
            FREEUP (saveAskedPtr);
            job->askedPtr = NULL;
            job->numAskedPtr = 0;
        }
        if (realNumHosts) {
            job->askedPtr = (struct askedHost *) my_calloc (realNumHosts,
                                                            sizeof(struct askedHost), fname);
            job->numAskedPtr = realNumHosts;
            for (i = 0; i < realNumHosts; i++) {
                job->askedPtr[i].hData = realAskedHosts[i].hData;
                job->askedPtr[i].priority = realAskedHosts[i].priority;
            }
            job->askedOthPrio = askedOthPrio;
            FREEUP (realAskedHosts);
        }
        job->shared->jobBill.options |= SUB_HOST;
    }



    if (JOB_PREEMPT_WAIT(job))
        freeReservePreemptResources(job);

    FREEUP (job->schedHost);
    job->schedHost = safeSave (job->hPtr[0]->hostType);

    job->pendEvent.sig1 = job->hPtr[0]->chkSig;
    job->pendEvent.sig1Flags = req->options | LSB_CHKPNT_KILL | LSB_CHKPNT_MIG;

    job->restartPid = job->jobPid;

    eventPending = TRUE;
    log_mig(job, auth->uid, auth->lsfUserName);


    return (LSBE_NO_ERROR);

}


int
signalJob (struct signalReq *signalReq, struct lsfAuth *auth)
{
    struct jData *jpbw;
    int reply;
    sbdReplyType sbdReply;
    struct jData *jPtr;

    if (logclass & (LC_SIGNAL | LC_TRACE))
        ls_syslog(LOG_DEBUG1,"signalJob: signal <%d> job <%s> chkPeriod <%d> actFlags <%d>", signalReq->sigValue,  lsb_jobid2str(signalReq->jobId), (int)signalReq->chkPeriod, signalReq->actFlags);

    if ((jpbw = getJobData (signalReq->jobId)) == NULL)
        return (LSBE_NO_JOB);

    if (signalReq->sigValue == SIGSTOP)
        signalReq->sigValue = SIG_SUSP_USER;

    if (signalReq->sigValue == SIGKILL)
        signalReq->sigValue = SIG_TERM_USER;

    if (signalReq->sigValue == SIGCONT)
        signalReq->sigValue = SIG_RESUME_USER;

    if (auth) {
        if (auth->uid != 0 && !jgrpPermitOk(auth, jpbw->jgrpNode)
            && !isAuthQueAd (jpbw->qPtr, auth)) {
            return (LSBE_PERMISSION);
        }
    }

    if (jpbw->nodeType != JGRP_NODE_JOB &&
        signalReq->sigValue == SIG_CHKPNT)
        return(LSBE_JOB_ARRAY);


    if (signalReq->sigValue == SIG_ARRAY_REQUEUE) {
        int      cc;

        cc = arrayRequeue(jpbw, signalReq, auth);
        if (cc != LSBE_NO_ERROR) {
            return(cc);
        }

        return(LSBE_NO_ERROR);
    }

    if (jpbw->nodeType == JGRP_NODE_ARRAY &&
        ARRAY_DATA(jpbw->jgrpNode)->counts[JGRP_COUNT_NJOBS] ==
        (ARRAY_DATA(jpbw->jgrpNode)->counts[JGRP_COUNT_NEXIT] +
         ARRAY_DATA(jpbw->jgrpNode)->counts[JGRP_COUNT_NDONE])) {
        return(LSBE_JOB_FINISH);
    }

    if (jpbw->nodeType == JGRP_NODE_ARRAY) {

        if (signalReq->sigValue == SIG_KILL_REQUEUE) {
            return(LSBE_JOB_ARRAY);
        }

        for (jPtr = jpbw->nextJob; jPtr; jPtr = jPtr->nextJob) {


            if (IS_FINISH(jPtr->jStatus)) {
                continue;
            }

            jPtr->pendEvent.sig = signalReq->sigValue;
            eventPending = TRUE;
            if (signalReq->sigValue == SIG_DELETE_JOB) {
                jPtr->pendEvent.sigDel = TRUE;
            }
        }

        log_signaljob(jpbw, signalReq, auth->uid, auth->lsfUserName);
        return (LSBE_OP_RETRY);
    }

    if (jpbw->jStatus & JOB_STAT_SIGNAL)
        if (signalReq->sigValue != SIG_TERM_USER
            && signalReq->sigValue != SIG_TERM_FORCE
            && signalReq->sigValue != SIG_KILL_REQUEUE) {
            jpbw->pendEvent.sig = signalReq->sigValue;
            eventPending = TRUE;
            return (LSBE_OP_RETRY);
        }


    if (signalReq->sigValue == SIG_CHKPNT &&
        !(jpbw->shared->jobBill.options & SUB_CHKPNTABLE)) {
        return (LSBE_J_UNCHKPNTABLE);
    }

    if (signalReq->sigValue == SIG_CHKPNT &&
        (jpbw->jStatus & JOB_STAT_WAIT) ) {
        return (LSBE_NOT_STARTED);
    }

    if (signalReq->sigValue == SIG_DELETE_JOB &&
        !IS_FINISH (jpbw->jStatus)) {

        if (signalReq->chkPeriod != 0)

            return (LSBE_J_UNREPETITIVE);
    }

    if (signalReq->sigValue == SIG_DELETE_JOB)
        jpbw->pendEvent.sigDel = TRUE;

    if (signalReq->sigValue == SIG_KILL_REQUEUE &&
        !IS_START(MASK_STATUS(jpbw->jStatus))) {
        if (IS_FINISH (jpbw->jStatus)) {
            if (jpbw->pendEvent.sigDel & DEL_ACTION_REQUEUE) {
                return(LSBE_JOB_REQUEUED);
            }
            return(LSBE_JOB_FINISH);
        }
        return(LSBE_NOT_STARTED);
    } else if (signalReq->sigValue == SIG_KILL_REQUEUE) {
        jpbw->pendEvent.sigDel |= DEL_ACTION_REQUEUE;
    }
    switch (MASK_STATUS(jpbw->jStatus)) {
        case JOB_STAT_DONE:
        case JOB_STAT_EXIT:
        case ( JOB_STAT_PDONE | JOB_STAT_DONE ):
        case ( JOB_STAT_PDONE | JOB_STAT_EXIT ):
        case ( JOB_STAT_PERR | JOB_STAT_DONE ):
        case ( JOB_STAT_PERR | JOB_STAT_EXIT ):

            if ( (signalReq->sigValue == SIG_TERM_USER) ) {
                reply = LSBE_JOB_FINISH;
                break;
            }
        case JOB_STAT_PEND:
        case JOB_STAT_PSUSP:

            if (isSigTerm (signalReq->sigValue)) {

                if (!auth)
                    log_signaljob(jpbw, signalReq, -1, "unknown");
                else {
                    log_signaljob(jpbw, signalReq, auth->uid, auth->lsfUserName);
                }
            }
            reply = sigPFjob (jpbw, signalReq->sigValue,
                              signalReq->chkPeriod, LOG_IT);
            if (jpbw->pendEvent.sigDel && jpbw->numRef)
                reply = LSBE_JOB_DEP;
            break;

        default:
            if  ((((MASK_STATUS(jpbw->jStatus))==JOB_STAT_RUN)
                  && (signalReq->sigValue == SIG_RESUME_USER))
                 ||  (((MASK_STATUS(jpbw->jStatus))==JOB_STAT_SSUSP)
                      && (jpbw->newReason & SUSP_USER_STOP)
                      && (signalReq->sigValue == SIG_SUSP_USER))) {


                return (LSBE_NO_ERROR);
            }

            if ((((signalReq->sigValue > 0)
                  && (signalReq->sigValue != SIGSTOP)
                  && (signalReq->sigValue != SIGTSTP)
                  && (signalReq->sigValue != SIGCONT)
                  && (signalReq->sigValue != SIGCHLD)
                  && (signalReq->sigValue != SIGTTIN)
                  && (signalReq->sigValue != SIGTTOU) &&
                  (signalReq->sigValue != SIGWINCH) &&
                  (signalReq->sigValue != SIGIO))) ||
                ((signalReq->sigValue < 0)
                 && isSigTerm(signalReq->sigValue)) ) {


                if (!auth)
                    log_signaljob(jpbw, signalReq, -1, "unknown");
                else
                    log_signaljob(jpbw, signalReq, auth->uid, auth->lsfUserName);
            }

            if (signalReq->sigValue == SIG_CHKPNT) {
                if (signalReq->chkPeriod != LSB_CHKPERIOD_NOCHNG)
                    jpbw->chkpntPeriod = signalReq->chkPeriod;
                jpbw->pendEvent.sig1 = jpbw->hPtr[0]->chkSig;
                jpbw->pendEvent.sig1Flags = signalReq->actFlags;
                eventPending = TRUE;
                if (jpbw->hPtr && (jpbw->hPtr[0]->hStatus & (HOST_STAT_UNREACH |
                                                             HOST_STAT_UNAVAIL)))
                    reply = LSBE_OP_RETRY;
                else
                    reply = LSBE_NO_ERROR;
                break;
            }
            sbdReply = sigStartedJob (jpbw, signalReq->sigValue,
                                      signalReq->chkPeriod, 0);
            switch (sbdReply) {
                case ERR_NO_ERROR:
                    reply = LSBE_NO_ERROR;
                    break;

                case ERR_NO_JOB:
                    reply = LSBE_JOB_FINISH;
                    break;
                default:
                    reply = LSBE_OP_RETRY;
            }
    }
    return (reply);
}


int
sigPFjob (struct jData *jData, int sigValue, time_t chkPeriod, int logIt)
{
    static char fname[] = "sigPFjob";
    struct jData *zombieData = NULL;

    switch (sigValue) {
        case SIG_DELETE_JOB:
            if (chkPeriod > 0)
                jData->runCount = chkPeriod;
            else {
                jData->runCount = 1;
                jData->newReason = EXIT_NORMAL;
                jData->exitStatus = DEF_PEND_EXIT;
                jData->exitStatus = jData->exitStatus << 8;
                if (IS_PEND(jData->jStatus)) {
                    jStatusChange(jData, JOB_STAT_EXIT, logIt, fname);
                }
                else
                    jData->runCount = MAX(0, jData->runCount-1);
            }
            return (LSBE_NO_ERROR);

        case SIG_CHKPNT:
        case SIG_CHKPNT_COPY:

            if (chkPeriod != LSB_CHKPERIOD_NOCHNG &&
                chkPeriod != jData->chkpntPeriod) {



                jData->chkpntPeriod = chkPeriod;
                log_jobsigact (jData, NULL, 0);
            }

            return (LSBE_NOT_STARTED);

        case SIG_SUSP_USER:
            if ((jData->jStatus & JOB_STAT_PEND)) {
                jData->newReason = PEND_USER_STOP;
                jStatusChange(jData, JOB_STAT_PSUSP, logIt, fname);
            }
            else if (IS_FINISH(jData->jStatus)) {
                return(LSBE_JOB_FINISH);
            }
            return (LSBE_NO_ERROR);

        case SIG_TERM_FORCE:
            if ((zombieData = getZombieJob (jData->jobId)) != NULL) {
                zombieData->newReason = EXIT_REMOVE;
                log_newstatus(zombieData);
                closeSbdConnect4ZombieJob(zombieData);
                offList ((struct listEntry *)zombieData);
                freeJData (zombieData);
                if (jData->newReason == EXIT_KILL_ZOMBIE &&
                    (jData->jStatus & JOB_STAT_ZOMBIE)) {
                    if (getZombieJob (jData->jobId) == NULL) {
                        jData->jStatus &= ~JOB_STAT_ZOMBIE;
                        jData->newReason = EXIT_REMOVE;
                    }
                }
                return (LSBE_NO_ERROR);
            }
        case SIG_TERM_USER:
        case SIGTERM:
        case SIGINT:
            if (!IS_PEND(jData->jStatus))
                return(LSBE_JOB_FINISH);
            jData->newReason = EXIT_NORMAL;
            jStatusChange(jData, JOB_STAT_EXIT, logIt, fname);
            return (LSBE_NO_ERROR);

        case SIG_RESUME_USER:
            if (!IS_PEND(jData->jStatus))
                return(LSBE_JOB_FINISH);
            if (jData->jStatus & JOB_STAT_PSUSP) {

                setJobPendReason(jData, PEND_USER_RESUME);
                jStatusChange(jData, JOB_STAT_PEND, logIt, fname);
            }
            return (LSBE_NO_ERROR);

        default:
            return (LSBE_NOT_STARTED);
    }

}

static sbdReplyType
sigStartedJob (struct jData *jData, int sigValue, time_t chkPeriod,
               int actFlags)
{
    static char fname[] = "sigStartedJob";
    sbdReplyType reply;
    struct jobReply jobReply;
    struct jobSig jobSig;
    int resumeSig, returnCode;

    memset(&jobReply, 0, sizeof(struct jobReply));

    if (jData->hPtr[0]->flags & HOST_LOST_FOUND) {

        if ((sigValue == SIG_TERM_USER)
            || (sigValue == SIGTERM) || (sigValue == SIGINT)
            || (sigValue == SIG_TERM_FORCE) ) {
            jData->shared->jobBill.options &= ~(SUB_RESTART | SUB_RESTART_FORCE);
            jData->newReason &= ~( SUB_RESTART | SUB_RESTART_FORCE);
            jStatusChange(jData, JOB_STAT_EXIT, LOG_IT, fname);
            return (ERR_NO_ERROR);

        } else if ((sigValue >= 0)
                   || (sigValue == SIG_RESUME_USER) || (sigValue == SIG_SUSP_USER)) {
            jData->pendEvent.sig = sigValue;
            eventPending = TRUE;
        }
        return(ERR_SIG_RETRY);
    }

    if (sigValue == SIG_DELETE_JOB || sigValue == SIG_KILL_REQUEUE) {
        if (chkPeriod == 0) {
            jData->runCount = 1;
            if (sigValue == SIG_DELETE_JOB)
                sigValue = SIG_TERM_USER;
        } else {
            jData->runCount = chkPeriod + 1;
            return (ERR_NO_ERROR);
        }
    }
    initJobSig (jData, &jobSig, sigValue, chkPeriod, actFlags);


    switch (sigValue) {
        case SIG_SUSP_USER:
            jobSig.actCmd = jData->qPtr->suspendActCmd;
            jobSig.sigValue = sigValue;
            break;

        case SIG_RESUME_USER:
            jobSig.actCmd = jData->qPtr->resumeActCmd;
            jobSig.sigValue = sigValue;
            if (sigValue == SIG_RESUME_USER
                && (jData->newReason & SUSP_MBD_LOCK)) {
                if (jData->jStatus & JOB_STAT_RESERVE) {
                    updResCounters (jData, ~JOB_STAT_RESERVE & jData->jStatus);
                    jData->jStatus &= ~JOB_STAT_RESERVE;
                }
                resumeSig = SIG_RESUME_USER;
                if ((returnCode = shouldResume(jData, &resumeSig))
                    != CANNOT_RESUME) {

                    if (!(jData->jStatus & JOB_STAT_RESERVE)) {
                        updResCounters (jData,
                                        JOB_STAT_RESERVE | jData->jStatus);
                        jData->jStatus |= JOB_STAT_RESERVE;
                    }
                    if (returnCode == RESERVE_SLOTS) {
                        jData->pendEvent.sig = sigValue;
                        eventPending = TRUE;
                        return (ERR_SIG_RETRY);

                    } else if (returnCode == RESUME_JOB) {

                        adjLsbLoad (jData, TRUE, TRUE);
                    }
                }
            }
            break;

        case SIG_TERM_USER:
        case SIG_TERM_FORCE:
            jobSig.actCmd = jData->qPtr->terminateActCmd;
            jobSig.sigValue = sigValue;
            break;
    }
    if (jobSig.actCmd == NULL)
        jobSig.actCmd = "";

    jobSig.reasons = jData->newReason;
    jobSig.subReasons = jData->subreasons;

    reply = signal_job (jData, &jobSig, &jobReply);
    signalReplyCode(reply, jData, sigValue, actFlags);

    return reply;

}


void
signalReplyCode (sbdReplyType reply, struct jData *jData, int sigValue,
                 int chkFlags)
{
    static char fname[] = "signalReplyCode";

    switch (reply) {
        case ERR_NO_ERROR:
            switch (sigValue) {
                case SIG_CHKPNT:
                case SIG_CHKPNT_COPY:
                    jData->pendEvent.sig1 = SIG_NULL;
                    break;
                default:
                    jData->pendEvent.sig = SIG_NULL;
            }
            jData->jStatus |= JOB_STAT_SIGNAL;
            break;
        case ERR_NO_JOB:
            jData->newReason = EXIT_NORMAL;
            jStatusChange(jData, JOB_STAT_EXIT, LOG_IT, fname);
            break;
        case ERR_SIG_RETRY:
            if ((sigValue >= 0)
                || (sigValue == SIG_RESUME_USER)
                || (sigValue == SIG_SUSP_USER)
                || (sigValue == SIG_TERM_USER)
                || (sigValue == SIG_TERM_FORCE)) {
                jData->pendEvent.sig = sigValue;
                eventPending = TRUE;
            }
            break;
        default:

            if ((isSigTerm(sigValue))
                && !(jData->jStatus & JOB_STAT_ZOMBIE)
                && (UNREACHABLE (jData->hPtr[0]->hStatus))) {
                if (sigValue != SIG_TERM_FORCE) {
                    jData->newReason = EXIT_KILL_ZOMBIE;
                    jData->jStatus |= JOB_STAT_ZOMBIE;
                    inZomJobList (jData, FALSE);
                } else {

                    jData->newReason = EXIT_REMOVE;
                }
                jStatusChange (jData, JOB_STAT_EXIT, LOG_IT, fname);
            }



            if ((sigValue >= 0)
                || (isSigTerm(sigValue) && (sigValue != SIG_TERM_FORCE))
                || (sigValue == SIG_RESUME_USER)
                || (sigValue == SIG_SUSP_USER)
                || (sigValue == SIG_TERM_USER)) {
                eventPending = TRUE;
                jData->pendEvent.sig = sigValue;
            }
    }

}

void
jobStatusSignal(sbdReplyType reply, struct jData *jData, int sigValue,
                int actFlags, struct jobReply *jobReply)
{
    static char fname[] = "jobStatusSignal";
    struct statusReq statusReq;
    char *actCmd;

    if (logclass & LC_SIGNAL)
        ls_syslog(LOG_DEBUG,
                  "%s: Changing status for job <%s>, signal <%d> flags %x",
                  fname, lsb_jobid2str(jData->jobId), sigValue, actFlags);



    jData->actPid = jobReply->actPid;



    if (jobReply->actPid  > 0 && jobReply->actStatus != ACT_NO) {

        statusReq.jobId = jData->jobId;
        statusReq.actPid = jobReply->actPid;
        statusReq.newStatus = jobReply->jStatus;
        statusReq.reason = jobReply->reasons;
        statusReq.sigValue = sigValue;
        statusReq.actStatus = ACT_START;
        jData->actPid = jobReply->actPid;
        jData->sigValue = sigValue;
        log_jobsigact(jData, &statusReq, actFlags);
    }

    switch (sigValue) {
        case SIG_CHKPNT:
        case SIG_CHKPNT_COPY:
            if (actFlags & LSB_CHKPNT_MIG) {
                jData->jStatus |= JOB_STAT_MIG;
            }

            break;

        case SIG_SUSP_USER:
            if ( (jData->jStatus & JOB_STAT_WAIT) ) {
                actCmd = NULL;
            } else {
                actCmd = jData->qPtr->suspendActCmd;
            }
            if (((actCmd != NULL) && (actCmd [0] != '\0'))
                && ((sigNameToValue_(actCmd) == INFINIT_INT)
                    || (sigNameToValue_(actCmd) < 0))
                && (jData->qPtr->sigMap[-sigValue] == 0))
                break;


        case SIGSTOP:
            if (jData->jStatus & JOB_STAT_PRE_EXEC) {
                jData->newReason = PEND_JOB_PRE_EXEC;
                jStatusChange(jData, JOB_STAT_PEND, LOG_IT, fname);
                jData->newReason = PEND_USER_STOP;
                jStatusChange(jData, JOB_STAT_PSUSP, LOG_IT, fname);
            }
            else if (!(jData->jStatus & JOB_STAT_USUSP)) {
                jData->newReason |= (SUSP_USER_STOP | SUSP_MBD_LOCK);
                jStatusChange(jData, JOB_STAT_USUSP, LOG_IT, fname);
            }
            jData->jStatus &= ~JOB_STAT_SIGNAL;
            jData->pendEvent.sig = SIG_NULL;
            break;

        case SIG_RESUME_USER:
            if ( (jData->jStatus & JOB_STAT_WAIT) ) {
                actCmd = NULL;
            } else {
                actCmd = jData->qPtr->resumeActCmd;
            }
            if (((actCmd != NULL) && (actCmd [0] != '\0'))
                && ((sigNameToValue_(actCmd) == INFINIT_INT)
                    || (sigNameToValue_(actCmd) < 0))

                && !((jobReply->actPid == 0)
                     && (jobReply->jStatus & JOB_STAT_SSUSP)
                     && (jData->jStatus & JOB_STAT_USUSP))){
                break;
            }

        case SIGCONT:

            if (jData->jStatus & JOB_STAT_USUSP) {
                jData->newReason &= ~SUSP_USER_STOP;
                if (!(jData->newReason & ~SUSP_MBD_LOCK))
                    jData->newReason |= SUSP_USER_RESUME;
                jData->ssuspTime = now;

                if (jData->jStatus & JOB_STAT_RESERVE) {
                    if (logclass & (LC_TRACE))
                        ls_syslog(LOG_DEBUG3, "%s: job <%s> updRes - <%d> slots <%s:%d>",
                                  fname, lsb_jobid2str(jData->jobId),
                                  jData->numHostPtr,
                                  __FILE__,  __LINE__);

                    updResCounters (jData, jData->jStatus & ~JOB_STAT_RESERVE);
                    jData->jStatus &= ~JOB_STAT_RESERVE;
                }
                jStatusChange(jData, JOB_STAT_SSUSP, LOG_IT, fname);

                if (!(jData->newReason & ~(SUSP_USER_RESUME | SUSP_MBD_LOCK))) {

                    if (!(jData->jStatus & JOB_STAT_RESERVE)) {
                        updResCounters (jData, JOB_STAT_RESERVE | jData->jStatus);
                        jData->jStatus |= JOB_STAT_RESERVE;
                    }
                }

            }
            jData->jStatus &= ~JOB_STAT_SIGNAL;
            jData->pendEvent.sig = SIG_NULL;
            break;

        default:
            jData->pendEvent.sig = SIG_NULL;
            jData->jStatus &= ~JOB_STAT_SIGNAL;
            break;
    }

}

int
sbatchdJobs (struct sbdPackage *sbdPackage, struct hData *hData)
{
    static char fname[] = "sbatchdJobs";
    struct jData *jpbw, *next;
    struct jobSpecs *jobSpecs;
    struct lenData jf;
    int list, num = 0, i;
    int size = sizeof(struct sbdPackage) * 4 / 4;


    for (list = 0; list < ALLJLIST && sbdPackage->numJobs > 0; list++) {
        if (list != SJL && list != ZJL && list != FJL)
            continue;
        for (jpbw = jDataList[list]->back; jpbw != jDataList[list];
             jpbw = next) {
            next= jpbw->back;


            if ( (list == FJL) &&
                 !( (jpbw->jStatus & JOB_STAT_DONE)
                    && !IS_POST_FINISH(jpbw->jStatus) ) ) {
                continue;
            }

            if (!IS_START(jpbw->jStatus) &&
                !(jpbw->jStatus & JOB_STAT_ZOMBIE) &&
                (list != FJL) ) {
                continue;
            }
            if (jpbw->hPtr == NULL || jpbw->hPtr[0] != hData)
                continue;


            if (jpbw->jobPid == 0 && !IS_FINISH(jpbw->jStatus)) {

                jpbw->newReason = PEND_JOB_START_FAIL;
                jpbw->subreasons = 0;
                jStatusChange(jpbw, JOB_STAT_PEND, LOG_IT, fname);
                continue;
            }


            if (num >= sbdPackage->numJobs) {
                ls_syslog(LOG_ERR, I18N(6541,
                                        "%s: Cannot add job<%s>, package full."), /* catgets 6541 */
                          fname, lsb_jobid2str(jpbw->jobId));
                continue;
            }

            jpbw->nextSeq = 1;
            jobSpecs = &(sbdPackage->jobs[num]);
            packJobSpecs (jpbw, jobSpecs);

            size += sizeof(struct jobSpecs)
                + jobSpecs->thresholds.nThresholds *
                jobSpecs->thresholds.nIdx * sizeof(float) * 2
                + jobSpecs->nxf * sizeof(struct xFile)
                + strlen (jobSpecs->loginShell)
                + strlen (jobSpecs->schedHostType)
                + strlen (jobSpecs->execHosts)
                + 10;

            for (i = 0; i < jobSpecs->numToHosts; i++) {

                size += getXdrStrlen(jobSpecs->toHosts[i]);
            }

            if (! (jpbw->jStatus & JOB_STAT_ZOMBIE)) {
                if ( readLogJobInfo(jobSpecs, jpbw, &jf, NULL) == -1) {
                    ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M,
                              fname, lsb_jobid2str(jpbw->jobId), "readLogJobInfo");
                }
                else {
                    size += ALIGNWORD_(jobSpecs->eexec.len) + sizeof(int);
                    FREEUP(jf.data);
                    for (i = 0; i < jobSpecs->numEnv; i++)
                        size += ALIGNWORD_(strlen(jobSpecs->env[i]) + 1);
                }
            }
            num++;
        }
    }
    sbdPackage->numJobs = num;
    sbdPackage->mbdPid = getpid();
    strcpy (sbdPackage->lsbManager, lsbManager);
    sbdPackage->managerId = managerId;
    sbdPackage->sbdSleepTime = sbdSleepTime;
    sbdPackage->retryIntvl = retryIntvl;
    sbdPackage->preemPeriod = preemPeriod;
    sbdPackage->pgSuspIdleT = pgSuspIdleT;
    sbdPackage->maxJobs = hData->maxJobs;
    sbdPackage->uJobLimit = hData->uJobLimit;
    sbdPackage->rusageUpdateRate = rusageUpdateRate;
    sbdPackage->rusageUpdatePercent = rusageUpdatePercent;
    sbdPackage->jobTerminateInterval = jobTerminateInterval;
    sbdPackage->nAdmins = nManagers;
    if ((sbdPackage->admins = (char **)my_calloc(
             nManagers, sizeof(char *), fname)) != NULL) {
        for (i = 0; i < nManagers; i++) {
            sbdPackage->admins[i] = safeSave(lsbManagers[i]);
            size += getXdrStrlen(lsbManagers[i]);
        }
    }

    return (size);

}

int
countNumSpecs (struct hData *hData)
{
    struct jData *jp;
    int numSpecs = 0;

    for (jp = jDataList[SJL]->back; jp != jDataList[SJL]; jp = jp->back) {
        if (jp->hPtr && jp->hPtr[0] == hData) {
            numSpecs++;
        }
    }

    for (jp = jDataList[ZJL]->back; jp != jDataList[ZJL]; jp = jp->back) {
        if (jp->hPtr && jp->hPtr[0] == hData
            && jp->jStatus & JOB_STAT_ZOMBIE) {
            numSpecs++;
        }
    }


    for (jp = jDataList[FJL]->back; jp != jDataList[FJL]; jp = jp->back) {
        if (jp->hPtr && jp->hPtr[0] == hData) {
            if ( (jp->jStatus & JOB_STAT_DONE) &&
                 !IS_POST_FINISH(jp->jStatus) ) {
                numSpecs++;
            }
        }
    }

    return numSpecs;

}

void
packJobSpecs (struct jData *jDataPtr, struct jobSpecs *jobSpecs)
{
    static char fname[] = "packJobSpecs";
    struct hData *hp = jDataPtr->hPtr[0];
    struct qData *qp = jDataPtr->qPtr;
    int i;


    jobSpecs->numEnv = 0;
    jobSpecs->env = NULL;
    jobSpecs->eexec.len = 0;
    jobSpecs->eexec.data = NULL;


    jobSpecs->jobId = jDataPtr->jobId;


    if ( jDataPtr->jgrpNode != NULL ) {
        strcpy (jobSpecs->jobName, fullJobName(jDataPtr));
    } else {
        strcpy (jobSpecs->jobName, jDataPtr->shared->jobBill.command);
    }


    jobSpecs->jStatus = (jDataPtr->jStatus & ~JOB_STAT_UNKWN);

    jobSpecs->lastCpuTime = jDataPtr->cpuTime;
    jobSpecs->startTime = jDataPtr->startTime;
    jobSpecs->runTime = jDataPtr->runTime;
    jobSpecs->submitTime = jDataPtr->shared->jobBill.submitTime;
    jobSpecs->reasons = jDataPtr->newReason;
    jobSpecs->subreasons = jDataPtr->subreasons;

    jobSpecs->userId = jDataPtr->userId;
    strcpy (jobSpecs->userName, jDataPtr->userName);


    jobSpecs->options = jDataPtr->shared->jobBill.options &
        ~SUB_RLIMIT_UNIT_IS_KB;

    jobSpecs->options2 = jDataPtr->shared->jobBill.options2;

    jobSpecs->jobPid = jDataPtr->jobPid;
    jobSpecs->jobPGid = jDataPtr->jobPGid;
    strcpy (jobSpecs->queue, qp->queue);
    jobSpecs->priority = qp->priority;
    jobSpecs->nice = qp->nice;
    strcpy (jobSpecs->fromHost, jDataPtr->shared->jobBill.fromHost);
    strcpy (jobSpecs->resReq, jDataPtr->shared->jobBill.resReq);

    jobSpecs->numToHosts = jDataPtr->numHostPtr;
    jobSpecs->maxNumProcessors = jDataPtr->shared->jobBill.maxNumProcessors;
    jobSpecs->toHosts = (char **) my_calloc (jDataPtr->numHostPtr,
                                             sizeof(char *), fname);
    for (i = 0; i < jDataPtr->numHostPtr; i++)
        jobSpecs->toHosts[i] = jDataPtr->hPtr[i]->host;


    packJobThresholds(&jobSpecs->thresholds, jDataPtr);

    jobSpecs->jAttrib = 0;

    if (((qp->qAttrib & Q_ATTRIB_EXCLUSIVE) &&
         (jDataPtr->shared->jobBill.options & SUB_EXCLUSIVE)) ||
        (IS_START(jDataPtr->jStatus) &&
         (jDataPtr->shared->jobBill.options & SUB_EXCLUSIVE))) {
        jobSpecs->jAttrib |= Q_ATTRIB_EXCLUSIVE;
    }

    if (jDataPtr->jFlags & JFLAG_URGENT)
        jobSpecs->jAttrib |= JOB_URGENT;

    if (jDataPtr->jFlags & JFLAG_URGENT_NOSTOP)
        jobSpecs->jAttrib |= JOB_URGENT_NOSTOP;

    jobSpecs->execUid = jDataPtr->execUid;
    if (jDataPtr->execUsername) {
        strcpy(jobSpecs->execUsername, jDataPtr->execUsername);
    } else {
        jobSpecs->execUsername[0] = '\0';
    }

    jobSpecs->sigValue = jDataPtr->shared->jobBill.sigValue;
    jobSpecs->termTime = jDataPtr->shared->jobBill.termTime;
    if (qp->windows)
        strcpy (jobSpecs->windows, qp->windows);
    else
        strcpy (jobSpecs->windows, " ");


    if (!jDataPtr->queuePreCmd && !qp->preCmd)
        strcpy (jobSpecs->preCmd, "");
    else {
        if (!jDataPtr->queuePreCmd) {
            jDataPtr->queuePreCmd = safeSave (qp->preCmd);
            strcpy (jobSpecs->preCmd, qp->preCmd);
        } else {
            strcpy (jobSpecs->preCmd, jDataPtr->queuePreCmd);
        }
    }

    if (!jDataPtr->queuePostCmd && !qp->postCmd)
        strcpy (jobSpecs->postCmd, "");
    else {
        if (!jDataPtr->queuePostCmd) {
            jDataPtr->queuePostCmd = safeSave (qp->postCmd);
            strcpy (jobSpecs->postCmd, qp->postCmd);
        } else {
            strcpy (jobSpecs->postCmd, jDataPtr->queuePostCmd);
        }
    }

    if (!qp->prepostUsername) {
	strcpy (jobSpecs->prepostUsername, "");
    }
    else {
	strcpy (jobSpecs->prepostUsername, qp->prepostUsername);
    }

    if ((jDataPtr->shared->jobBill.options & SUB_RESTART) && (jDataPtr->execCwd))
        strcpy (jobSpecs->execCwd, jDataPtr->execCwd);
    else
        strcpy (jobSpecs->execCwd, "");

    if (jDataPtr->execHome)
        strcpy (jobSpecs->execHome, jDataPtr->execHome);
    else
        strcpy (jobSpecs->execHome, "");

    if (qp->requeueEValues)
        strcpy (jobSpecs->requeueEValues, qp->requeueEValues);
    else
        strcpy (jobSpecs->requeueEValues, "");
    if (qp->resumeCond) {
        STRNCPY(jobSpecs->resumeCond, qp->resumeCond, MAXLINELEN);
    }
    else
        strcpy (jobSpecs->resumeCond, "");

    if (qp->stopCond) {
        STRNCPY(jobSpecs->stopCond, qp->stopCond, MAXLINELEN);
    }
    else
        strcpy (jobSpecs->stopCond, "");


    if ((qp->suspendActCmd !=NULL) && (qp->suspendActCmd[0] != '\0'))
        strcpy (jobSpecs->suspendActCmd, qp->suspendActCmd);
    else
        strcpy (jobSpecs->suspendActCmd, "");

    if ((qp->resumeActCmd !=NULL) && (qp->resumeActCmd[0] != '\0'))
        strcpy (jobSpecs->resumeActCmd, qp->resumeActCmd);
    else
        strcpy (jobSpecs->resumeActCmd, "");

    if ((qp->terminateActCmd !=NULL)
        && (qp->terminateActCmd[0] != '\0'))
        strcpy (jobSpecs->terminateActCmd, qp->terminateActCmd);
    else
        strcpy (jobSpecs->terminateActCmd, "");

    for (i=0; i<LSB_SIG_NUM; i++)
        jobSpecs->sigMap[i] = qp->sigMap[i];

    jobSpecs->actValue = jDataPtr->sigValue;

    strcpy (jobSpecs->command, jDataPtr->shared->jobBill.command);
    if (LSB_ARRAY_IDX(jDataPtr->jobId) != 0)
        sprintf (jobSpecs->jobFile, "%s.%d", jDataPtr->shared->jobBill.jobFile,
                 LSB_ARRAY_IDX(jDataPtr->jobId));
    else
        sprintf (jobSpecs->jobFile, "%s", jDataPtr->shared->jobBill.jobFile);


    if (jDataPtr->shared->jobBill.options2 & SUB2_IN_FILE_SPOOL) {
        expandFileNameWithJobId ( jobSpecs->inFile,
                                  jDataPtr->shared->jobBill.inFileSpool,
                                  jDataPtr->jobId);
    } else {
        expandFileNameWithJobId ( jobSpecs->inFile,
                                  jDataPtr->shared->jobBill.inFile,
                                  jDataPtr->jobId);
    }


    expandFileNameWithJobId ( jobSpecs->outFile,
                              jDataPtr->shared->jobBill.outFile,
                              jDataPtr->jobId);
    expandFileNameWithJobId ( jobSpecs->errFile,
                              jDataPtr->shared->jobBill.errFile,
                              jDataPtr->jobId);

    jobSpecs->umask = jDataPtr->shared->jobBill.umask;
    strcpy (jobSpecs->cwd, jDataPtr->shared->jobBill.cwd);
    strcpy (jobSpecs->subHomeDir, jDataPtr->shared->jobBill.subHomeDir);

    jobSpecs->restartPid = jDataPtr->restartPid;

    if (LSB_ARRAY_IDX(jDataPtr->jobId)){
        char *sp = strrchr(jDataPtr->shared->jobBill.chkpntDir, '/');
        if (sp) {
            *sp = '\0';
            sprintf(jobSpecs->chkpntDir, "%s/%s",
                    jDataPtr->shared->jobBill.chkpntDir,
                    lsb_jobidinstr(jDataPtr->jobId));
            *sp = '/';
        }
        else
            sprintf(jobSpecs->chkpntDir, jDataPtr->shared->jobBill.chkpntDir);

    }
    else
        if (jDataPtr->shared->jobBill.chkpntDir)
            strcpy (jobSpecs->chkpntDir, jDataPtr->shared->jobBill.chkpntDir);
    jobSpecs->actPid = jDataPtr->actPid;
    jobSpecs->chkPeriod = jDataPtr->shared->jobBill.chkpntPeriod;
    jobSpecs->chkSig = hp->chkSig;
    jobSpecs->migThresh = MIN(hp->mig, qp->mig);
    jobSpecs->lastSSuspTime = jDataPtr->ssuspTime;

    jobSpecs->nxf = jDataPtr->shared->jobBill.nxf;

    if (jobSpecs->nxf == 0) {
        jobSpecs->xf = NULL;
    } else {
        jobSpecs->xf = (struct xFile *)
            my_calloc(jobSpecs->nxf, sizeof(struct xFile), fname);

        for (i = 0; i < jobSpecs->nxf; i++) {
            expandFileNameWithJobId(jobSpecs->xf[i].subFn,
                                    jDataPtr->shared->jobBill.xf[i].subFn,
                                    jDataPtr->jobId);
            expandFileNameWithJobId(jobSpecs->xf[i].execFn,
                                    jDataPtr->shared->jobBill.xf[i].execFn,
                                    jDataPtr->jobId);
            jobSpecs->xf[i].options = jDataPtr->shared->jobBill.xf[i].options;
        }
    }

    strcpy (jobSpecs->mailUser, jDataPtr->shared->jobBill.mailUser);
    strcpy (jobSpecs->preExecCmd, jDataPtr->shared->jobBill.preExecCmd);
    strcpy (jobSpecs->postExecCmd, jDataPtr->shared->jobBill.postExecCmd);
    strcpy (jobSpecs->projectName, jDataPtr->shared->jobBill.projectName);
    jobSpecs->niosPort = jDataPtr->shared->jobBill.niosPort;
    jobSpecs->loginShell = jDataPtr->shared->jobBill.loginShell;
    jobSpecs->schedHostType = jDataPtr->schedHost;

    strcpy (jobSpecs->clusterName, clusterName);

    for(i = 0; i < LSF_RLIM_NLIMITS; i++) {

        if (qp->rLimits[i] < 0) {
            jobSpecs->lsfLimits[i].rlim_maxl = 0xffffffff;
            jobSpecs->lsfLimits[i].rlim_maxh = 0x7fffffff;
        } else {
            rLimits2lsfLimits(qp->rLimits, jobSpecs->lsfLimits, i, 0);
        }


        if (jDataPtr->shared->jobBill.rLimits[i] < 0) {

            switch ( i ) {
                case LSF_RLIMIT_RUN:
                case LSF_RLIMIT_CPU:
                case LSF_RLIMIT_RSS:
                case LSF_RLIMIT_DATA:
                case LSF_RLIMIT_PROCESS:
                    if ( qp->defLimits[i] > 0 ) {

                        rLimits2lsfLimits(qp->defLimits, jobSpecs->lsfLimits, i, 1);
                    } else {

                        jobSpecs->lsfLimits[i].rlim_curl = jobSpecs->lsfLimits[i].rlim_maxl;
                        jobSpecs->lsfLimits[i].rlim_curh = jobSpecs->lsfLimits[i].rlim_maxh;
                    }
                    break;
                default:
                    jobSpecs->lsfLimits[i].rlim_curl = jobSpecs->lsfLimits[i].rlim_maxl;
                    jobSpecs->lsfLimits[i].rlim_curh = jobSpecs->lsfLimits[i].rlim_maxh;
            }

        } else {
            if (qp->rLimits[i] >= jDataPtr->shared->jobBill.rLimits[i]
                || qp->rLimits[i] < 0) {
                rLimits2lsfLimits(jDataPtr->shared->jobBill.rLimits, jobSpecs->lsfLimits, i, 1);
            } else {

                if ( logclass & LC_EXEC ) {
                    ls_syslog(LOG_DEBUG, I18N(6538,
                                              "packJobSpecs: jobId <%s> user specified soft limit(%d) bigger than queue's hard limit(%d) for rlimit[%d]"), /* catgets 6538 */
                              lsb_jobid2str(jobSpecs->jobId),
                              jDataPtr->shared->jobBill.rLimits[i],
                              qp->rLimits[i], i);
                }
                jobSpecs->lsfLimits[i].rlim_curl = jobSpecs->lsfLimits[i].rlim_maxl;
                jobSpecs->lsfLimits[i].rlim_curh = jobSpecs->lsfLimits[i].rlim_maxh;
            }
        }
    }


    scaleByFactor(&jobSpecs->lsfLimits[LSF_RLIMIT_CPU].rlim_curh,
                  &jobSpecs->lsfLimits[LSF_RLIMIT_CPU].rlim_curl,
                  hp->cpuFactor);
    scaleByFactor(&jobSpecs->lsfLimits[LSF_RLIMIT_CPU].rlim_maxh,
                  &jobSpecs->lsfLimits[LSF_RLIMIT_CPU].rlim_maxl,
                  hp->cpuFactor);

    scaleByFactor(&jobSpecs->lsfLimits[LSF_RLIMIT_RUN].rlim_curh,
                  &jobSpecs->lsfLimits[LSF_RLIMIT_RUN].rlim_curl,
                  hp->cpuFactor);
    scaleByFactor(&jobSpecs->lsfLimits[LSF_RLIMIT_RUN].rlim_maxh,
                  &jobSpecs->lsfLimits[LSF_RLIMIT_RUN].rlim_maxl,
                  hp->cpuFactor);

    jobSpecs->execHosts = safeSave(jDataPtr->execHosts);

    if (jDataPtr->jobSpoolDir != NULL ) {

        if (logclass & LC_TRACE)
            ls_syslog(LOG_DEBUG, "%s: the job spooldir is %s", fname,
                      jDataPtr->jobSpoolDir);
        strcpy(jobSpecs->jobSpoolDir, jDataPtr->jobSpoolDir);
    } else {
        jobSpecs->jobSpoolDir[0] = 0x0;
    }
    if (jDataPtr->shared->jobBill.inFileSpool != NULL) {
        strcpy(jobSpecs->inFileSpool, jDataPtr->shared->jobBill.inFileSpool);
    } else {
        jobSpecs->inFileSpool[0] = '\0';
    }
    if (jDataPtr->shared->jobBill.commandSpool != NULL) {
        strcpy(jobSpecs->commandSpool, jDataPtr->shared->jobBill.commandSpool);
    } else {
        jobSpecs->commandSpool[0] = '\0';
    }
    jobSpecs->userPriority = jDataPtr->shared->jobBill.userPriority;
}

void
freeJobSpecs (struct jobSpecs *jobSpecs)
{
    int i;

    if (jobSpecs->toHosts)
        free (jobSpecs->toHosts);

    freeThresholds (&jobSpecs->thresholds);

    if (jobSpecs->numEnv > 0) {
        for (i = 0; i < jobSpecs->numEnv; i++)
            FREEUP (jobSpecs->env[i]);
        FREEUP (jobSpecs->env);
    }
    if (jobSpecs->eexec.len > 0)
        free(jobSpecs->eexec.data);

    FREEUP(jobSpecs->execHosts);

    if (jobSpecs->nxf) {
        FREEUP(jobSpecs->xf);
    }
}

static void
freeThresholds (struct thresholds *thresholds)
{
    int i;

    if (thresholds == NULL)
        return;

    for (i = 0; i < thresholds->nThresholds; i++) {
        FREEUP (thresholds->loadSched[i]);
        FREEUP (thresholds->loadStop[i]);
    }
    FREEUP (thresholds->loadSched);
    FREEUP (thresholds->loadStop);
}
int
statusJob(struct statusReq *statusReq, struct hostent *hp, int *schedule)
{
    static char       fname[] = "statusJob";
    struct jData      *jpbw,
        *jData;
    struct hData      *hData;
    char              stopit = FALSE;
    int               oldStatus,
        lockJob = FALSE;
    char              *host;
    int               diffSTime;
    int               diffUTime;
    int               diffTime;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if ((jpbw = getJobData (statusReq->jobId)) == NULL) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname, lsb_jobid2str(statusReq->jobId), "getJobData");
        return(LSBE_NO_JOB);
    }



    if (IS_PEND(jpbw->jStatus) &&
        jpbw->jFlags & JFLAG_WAIT_SWITCH ) {
        if (IS_FINISH(statusReq->newStatus)) {

            jpbw->jFlags &= ~JFLAG_WAIT_SWITCH;
        }
        return (LSBE_NO_ERROR);
    }


    if (hp == NULL) {
        ls_syslog(LOG_ERR, I18N(6540,
                                "%s: Received job status has no host information."), /* catgets 6540 */
                  fname);
        return (LSBE_NO_JOB);
    }

    hData = getHostData (hp->h_name);
    if (hData == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6513,
                                         "%s: Received job status update from host <%s> that is not configured as a batch server"), fname, hp->h_name); /* catgets 6513 */
        return (LSBE_SBATCHD);
    }
    hStatChange (hData, 0);
    host=hData->host;


    if ( IS_POST_DONE(statusReq->newStatus)
         || IS_POST_ERR(statusReq->newStatus) ) {

        if (jpbw->jStatus & JOB_STAT_DONE) {
            jpbw->jStatus |= statusReq->newStatus;
            log_newstatus(jpbw);
        } else {
            if (logclass & (LC_TRACE)) {
                ls_syslog(LOG_DEBUG,
                          "Wrong job status migration from %d to %d, ignore it",
                          jpbw->jStatus, statusReq->newStatus);
            }
        }
        return (LSBE_NO_ERROR);
    }

    oldStatus = jpbw->jStatus;

    if (jpbw->runRusage.stime < 0)
        diffSTime = statusReq->runRusage.stime;
    else
        diffSTime = statusReq->runRusage.stime - jpbw->runRusage.stime;

    if (jpbw->runRusage.utime < 0)
        diffUTime = statusReq->runRusage.utime;
    else
        diffUTime = statusReq->runRusage.utime - jpbw->runRusage.utime;

    /*update cpuTime for fairshare account*/
    diffTime = diffSTime+diffUTime;
    if (jpbw->sa && diffTime > 0) {
        SACCT_CPUTIME_UPDATE(diffTime, jpbw->sa, fname);
        if (logclass & LC_FAIR) {
            ls_syslog(LOG_DEBUG1, "%s: updated SAAP <%s> cputime <%d> for job <%s>",
                    fname, jpbw->sa->path,
                    diffTime,
                    lsb_jobid2str(jpbw->jobId));
        }
        updSAcctPriorityByPath(jpbw->sa);
    }

    jpbw->jRusageUpdateTime = now;

    copyJUsage(&(jpbw->runRusage), &(statusReq->runRusage));


    if (!IS_START (statusReq->newStatus) && !IS_FINISH (statusReq->newStatus)
        && !(statusReq->newStatus & JOB_STAT_PEND)) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6514,
                                         "%s: Invalid job status <%d> of job <%s> reported from host/cluster <%s>"), /* catgets 6514 */
                  fname,
                  statusReq->newStatus,
                  lsb_jobid2str(jpbw->jobId),
                  host);
        return (LSBE_SBATCHD);
    }

    if ((statusReq->newStatus & JOB_STAT_MIG) && (statusReq->actPid == 0)) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6515,
                                         "%s: Job <%s> is in migration status, but chkpid is 0; illegal status reported from host/cluster <%s>"), /* catgets 6515 */
                  fname,
                  lsb_jobid2str(jpbw->jobId),
                  host);
        return (LSBE_SBATCHD);
    }



    if ((statusReq->newStatus & JOB_STAT_PEND)
        && (statusReq->reason == PEND_JOB_START_FAIL
            || statusReq->reason == PEND_JOB_NO_FILE)) {
        jpbw->newReason = statusReq->reason;

        if (IS_START(jpbw->jStatus)) {

            if (jpbw->hPtr == NULL)
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6516,
                                                 "%s: Job <%s> started but hPtr is NULL"), /* catgets 6516 */
                          fname, lsb_jobid2str(jpbw->jobId));
            else {

                if (jpbw->hPtr[0] == hData)
                    jStatusChange(jpbw, JOB_STAT_PEND, LOG_IT, fname);


            }
        }


        if ((jData = isInZomJobList (hData, statusReq)) != NULL) {


            statusReq->newStatus = JOB_STAT_EXIT;
        } else {
            return (LSBE_NO_ERROR);
        }
    }



    if (IS_PEND (jpbw->jStatus) && IS_START(statusReq->newStatus)) {

        if (isInZomJobList(hData, statusReq) != NULL) {

            if (logclass & LC_SCHED) {
                ls_syslog(LOG_DEBUG,"\
%s: jobId=<%s> is in Zombie State and action=%x running/done/failed",
                          fname, lsb_jobid2str(jpbw->jobId),
                          statusReq->actStatus);
            }

            goto  handleJobJCCA;
        }


        ls_syslog (LOG_DEBUG, "%s: sbatchd on host/cluster <%s> is reporting started job <%s> that is still in PEND status;", fname, host, lsb_jobid2str(jpbw->jobId));

        if (jpbw->hPtr == NULL) {
            jpbw->numHostPtr = 1;
            jpbw->hPtr = (struct hData **) my_malloc
                (sizeof (struct hData *), fname);
            jpbw->hPtr[0] = hData;
            ls_syslog(LOG_DEBUG, "statusJob: Assume it is a sequential job");
        }
        jpbw->jobPid = statusReq->jobPid;
        jpbw->jobPGid = statusReq->jobPGid;
        if (jpbw->shared->jobBill.options & SUB_PRE_EXEC &&
            !(jpbw->jStatus & JOB_STAT_PRE_EXEC))
            jpbw->jStatus |= JOB_STAT_PRE_EXEC;
        jStatusChange(jpbw, JOB_STAT_RUN, LOG_IT, fname);
        if (oldStatus & JOB_STAT_PSUSP && !IS_FINISH(statusReq->newStatus))
            stopit = TRUE;
    }



    if (IS_START(jpbw->jStatus) && IS_START(statusReq->newStatus)) {

        if (jpbw->jobPid == 0) {
            jpbw->jobPid = statusReq->jobPid;
            jpbw->jobPGid = statusReq->jobPGid;
            log_startjobaccept(jpbw);
        }


        if (!jpbw->execHome &&
            statusReq->execCwd && statusReq->execCwd[0] != '\0' &&
            statusReq->execHome && statusReq->execHome[0] != '\0') {

            if (jpbw->execCwd == NULL) {
                jpbw->execCwd = safeSave (statusReq->execCwd);
            } else if (strcmp(jpbw->execCwd, statusReq->execCwd) != 0) {

                FREEUP(jpbw->execCwd);
                jpbw->execCwd = safeSave (statusReq->execCwd);
            }

            jpbw->execHome = safeSave (statusReq->execHome);
            if (jpbw->execUsername)
                FREEUP (jpbw->execUsername);
            jpbw->execUsername = safeSave (statusReq->execUsername);
            jpbw->execUid = statusReq->execUid;
            jpbw->jobPid = statusReq->jobPid;
            jpbw->jobPGid = statusReq->jobPGid;
            log_executejob (jpbw);
            if (oldStatus == statusReq->newStatus)
                return (LSBE_NO_ERROR);
        }
    }


    if ((statusReq->newStatus
         & (JOB_STAT_DONE
            | JOB_STAT_EXIT
            | JOB_STAT_PDONE
            | JOB_STAT_PERR )
            )
        || IS_PEND(statusReq->newStatus)
        || (statusReq->actStatus != ACT_NO
            && (((IS_PEND(jpbw->jStatus) || IS_FINISH(jpbw->jStatus))
                 ||(jpbw->hPtr==NULL || hData != jpbw->hPtr[0]))
                && (jpbw->shared->jobBill.options & SUB_RERUNNABLE)))) {


        if (statusReq->actStatus != ACT_NO
            && (IS_PEND(jpbw->jStatus)
                && (jpbw->shared->jobBill.options & SUB_RERUNNABLE))) {
            jpbw->jStatus &= ~JOB_STAT_MIG;
            jpbw->actPid = 0;
            jpbw->sigValue = SIG_NULL;
        }

        if ((jData = isInZomJobList (hData, statusReq)) != NULL) {
            jData->newReason = EXIT_ZOMBIE_JOB;
            if (jData->lsfRusage == NULL)
                jData->lsfRusage = &(statusReq->lsfRusage);
            accumulateRU (jData, statusReq);
            log_newstatus(jData);
            if (jData->lsfRusage == &(statusReq->lsfRusage))
                jData->lsfRusage = NULL;
            offList ((struct listEntry *)jData);
            closeSbdConnect4ZombieJob(jData);
            freeJData (jData);
            if (jpbw->newReason == EXIT_KILL_ZOMBIE &&
                (jpbw->jStatus & JOB_STAT_ZOMBIE)) {
                if (getZombieJob (jpbw->jobId) == NULL) {
                    jpbw->jStatus &= ~JOB_STAT_ZOMBIE;
                    jpbw->newReason = 0;
                }
            }
            return (LSBE_NO_ERROR);
        }
    }


    if (jpbw->hPtr == NULL || !equalHost_(hp->h_name, jpbw->hPtr[0]->host)
        || (statusReq->seq < jpbw->nextSeq
            && (jpbw->nextSeq - statusReq->seq) < MAX_SEQ_NUM/2) ) {

        ls_syslog(LOG_DEBUG, "%s: Obsolete status of job <%s>; status <%d> discarded", fname, lsb_jobid2str(jpbw->jobId), statusReq->newStatus);
        return (LSBE_NO_ERROR);
    }
    jpbw->nextSeq = statusReq->seq++;
    if (jpbw->nextSeq >= MAX_SEQ_NUM || (statusReq->newStatus & JOB_STAT_PEND))
        jpbw->nextSeq = 1;


    if (jpbw->jStatus & (JOB_STAT_DONE | JOB_STAT_EXIT) ) {

        if (debug)
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6518,
                                             "%s: Obsolete status report of job <%s> from sbatchd on host <%s>: %x-->%x, discarded"), /* catgets 6518 */
                      fname,
                      lsb_jobid2str(jpbw->jobId),
                      host,
                      jpbw->jStatus,
                      statusReq->newStatus);
        return (LSBE_NO_ERROR);
    }

handleJobJCCA:


    if (statusReq->actStatus != ACT_NO) {
        int sigFlags = 0;

        if (statusReq->sigValue == SIG_CHKPNT ||
            statusReq->sigValue == SIG_CHKPNT_COPY) {

            jpbw->sigValue = statusReq->sigValue;

            if (statusReq->actStatus == ACT_START) {

                if (statusReq->newStatus & JOB_STAT_MIG) {

                    if (!(jpbw->jStatus & JOB_STAT_MIG)) {
                        jpbw->jStatus |= JOB_STAT_MIG;
                    }
                    sigFlags = LSB_CHKPNT_MIG | LSB_CHKPNT_KILL;
                } else {
                    jpbw->jStatus &= ~JOB_STAT_MIG;
                }

                if (jpbw->actPid != statusReq->actPid) {

                    jpbw->actPid = statusReq->actPid;
                    log_jobsigact (jpbw, statusReq, sigFlags);
                }
            } else {
                if (statusReq->actStatus == ACT_DONE) {
                    if (statusReq->newStatus & JOB_STAT_MIG) {

                        jpbw->jStatus |= JOB_STAT_MIG;
                        sigFlags = LSB_CHKPNT_MIG;

                        FREEUP (jpbw->schedHost);
                        jpbw->schedHost
                            = safeSave (jpbw->hPtr[0]->hostType);

                        jpbw->restartPid = jpbw->jobPid;


                        if (!jpbw->lsfRusage) {
                            jpbw->lsfRusage = (struct lsfRusage *)
                                my_malloc (sizeof(struct lsfRusage), fname);
                            cleanLsfRusage (jpbw->lsfRusage);
                        }
                    } else {
                        jpbw->jStatus &= ~JOB_STAT_MIG;

                    }

                    if (statusReq->sbdReply == ERR_NO_ERROR)
                        jpbw->jStatus |= JOB_STAT_CHKPNTED_ONCE;

                    jpbw->actPid = statusReq->actPid;
                } else {

                    if (jpbw->jStatus & JOB_STAT_RESERVE) {

                        if (logclass & LC_TRACE)
                            ls_syslog(LOG_DEBUG3, "\
%s: job <%s> updRes - <%d> slots <%s:%d>",
                                      fname,
                                      lsb_jobid2str(jpbw->jobId),
                                      jpbw->numHostPtr,
                                      __FILE__,  __LINE__);
                        updResCounters(jpbw, jpbw->jStatus &~JOB_STAT_RESERVE);
                        jpbw->jStatus &= ~JOB_STAT_RESERVE;
                    }
                }


                if (!(statusReq->newStatus & JOB_STAT_MIG))
                    jpbw->jStatus &= ~JOB_STAT_MIG;

                jpbw->jStatus &= ~JOB_STAT_SIGNAL;
                log_jobsigact (jpbw, statusReq, sigFlags);
                jpbw->actPid = 0;
                jpbw->sigValue = SIG_NULL;
            }
        } else {

            if ((statusReq->actStatus == ACT_START)) {
                jpbw->actPid = statusReq->actPid;
                jpbw->sigValue = statusReq->sigValue;
                log_jobsigact (jpbw, statusReq, sigFlags);
            } else if (statusReq->actStatus == ACT_DONE ||
                       statusReq->actStatus == ACT_FAIL) {
                jpbw->jStatus &= ~JOB_STAT_SIGNAL;
                log_jobsigact (jpbw, statusReq, sigFlags);
                jpbw->actPid = 0;
                jpbw->sigValue = SIG_NULL;
            } else {
                if (logclass & LC_TRACE)
                    ls_syslog (LOG_DEBUG1, "%s: Bad signal action status <%d> reported by <%s> for job <%s>", fname, statusReq->actStatus, hp? hp->h_name : jpbw->hPtr[0]->host, lsb_jobid2str(jpbw->jobId));
            }
        }

        if (IS_PEND(jpbw->jStatus)
            &&
            isInZomJobList(hData, statusReq) != NULL
            &&
            IS_START(statusReq->newStatus)) {

            if (logclass & LC_SCHED) {
                ls_syslog(LOG_DEBUG,"\
%s: jobId/status=<%s/%x> is PEND and has ZJL image, sbatchd is now running JCCA(%d) newStatus=%x no status update has to be performed until the action ends",
                          fname, lsb_jobid2str(jpbw->jobId),
                          jpbw->jStatus, statusReq->actStatus,
                          statusReq->newStatus);
            }

            return(LSBE_NO_ERROR);
        }

    }


    if (!(statusReq->newStatus & JOB_STAT_MIG))
        jpbw->jStatus &= ~JOB_STAT_MIG;


    if (IS_SUSP(statusReq->newStatus))  {

        if (! (statusReq->reason & SUSP_MBD_LOCK)
            && !(jpbw->newReason & SUSP_MBD_LOCK)) {

            if ((lockJob = shouldLockJob (jpbw,
                                          statusReq->newStatus)) == TRUE) {
                jpbw->newReason = statusReq->reason | SUSP_MBD_LOCK;
                jpbw->subreasons = statusReq->subreasons;
            } else {

                jpbw->newReason = statusReq->reason;
                jpbw->subreasons = statusReq->subreasons;
            }
        } else {


            if ((statusReq->reason & SUSP_MBD_LOCK)
                && !(jpbw->newReason & SUSP_MBD_LOCK)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6519,"\
%s: sbatchd on <%s> thinks job <%s> is locked but mbatchd doesn't; lock it"), /* catgets 6519 */
                          fname, host, lsb_jobid2str(jpbw->jobId));
                jpbw->newReason = statusReq->reason;
                jpbw->subreasons = statusReq->subreasons;
            }
        }

        if (!(jpbw->jStatus & JOB_STAT_SSUSP))
            jpbw->ssuspTime = now;
    }

    if ((MASK_STATUS(jpbw->jStatus & ~JOB_STAT_UNKWN)
         == MASK_STATUS (statusReq->newStatus & ~JOB_STAT_UNKWN))
        && !((jpbw->jStatus & JOB_STAT_PRE_EXEC)
             && IS_RUN_JOB_CMD(statusReq->newStatus))) {
        if (jpbw->newReason & SUSP_MBD_LOCK)
            return(LSBE_LOCK_JOB);
        else {
            return(LSBE_NO_ERROR);
        }
    }

    if (statusReq->newStatus & JOB_STAT_PEND) {
        if (IS_START(jpbw->jStatus)) {
            if (!jpbw->lsfRusage) {
                jpbw->lsfRusage = my_malloc (sizeof(struct lsfRusage),
                                             fname);
                cleanLsfRusage (jpbw->lsfRusage);
            }
            accumulateRU(jpbw, statusReq);
        }
        jpbw->exitStatus = statusReq->exitStatus;
        if (jpbw->jStatus & JOB_STAT_PRE_EXEC
            || statusReq->reason == PEND_QUE_PRE_FAIL) {
            jpbw->newReason = statusReq->reason;


            if (preExecDelay == DEF_PRE_EXEC_DELAY) {

                jpbw->dispTime += MIN(1800, (jpbw->dispCount / 50) * msleeptime);
            } else {

                jpbw->dispTime +=  preExecDelay * msleeptime;
            }

        } else if (jpbw->jStatus & JOB_STAT_MIG) {

            jpbw->newReason = PEND_JOB_MIG;

        } else if (jpbw->jStatus & JOB_STAT_PSUSP) {
            return (LSBE_NO_ERROR);
        } else {

            jpbw->newReason = statusReq->reason;

            if (statusReq->reason == PEND_NO_MAPPING
                || (statusReq->reason == PEND_RMT_PERMISSION)) {
                jpbw->lastDispHost = fill_requeueHist(&(jpbw->reqHistory),
                                                      &(jpbw->reqHistoryAlloc), hData);
                jpbw->requeMode = RQE_EXCLUDE;
            } else if (ususpPendingEvent(jpbw)) {
                jStatusChange (jpbw, JOB_STAT_PEND, LOG_IT, "statusJob");
                statusReq->newStatus = JOB_STAT_PSUSP;
            } else if (statusReq->reason == PEND_SBD_JOB_REQUEUE) {



                if (jpbw->qPtr->requeEStruct != NULL) {

                    switch(jpbw->requeMode =
                           match_exitvalue(jpbw->qPtr->requeEStruct,
                                           (statusReq->exitStatus)>>8)) {
                        case RQE_EXCLUDE:

                            jpbw->lastDispHost =
                                fill_requeueHist(&(jpbw->reqHistory),
                                                 &(jpbw->reqHistoryAlloc),hData);
                            break;
                        default:
                            break;
                    }
                }
            } else if (terminatePendingEvent(jpbw)) {
                jStatusChange (jpbw, JOB_STAT_PEND, LOG_IT, "statusJob");
                statusReq->newStatus = JOB_STAT_EXIT;
            } else if (statusReq->reason == PEND_JOB_NO_PASSWD){
                char mailmsg[400];
                sprintf(mailmsg,
                        _i18n_msg_get(ls_catd, NL_SETN, 1515,
                                      "We are unable to start your job <%s> because the your password \ncould not be found or was invalid.\nWe have suspended your job.  You may resume it when the \npassword problem is fixed.\n"), /* catgets 1515 */
                        lsb_jobid2str(jpbw->jobId));
                merr_user(jpbw->userName, jpbw->shared->jobBill.fromHost, mailmsg, "error");
                jStatusChange(jpbw, statusReq->newStatus, LOG_IT, fname);
                statusReq->newStatus = JOB_STAT_PSUSP;
                jpbw->newReason = PEND_JOB_NO_PASSWD;
                jpbw->initFailCount = 0;
            } else {

                jpbw->dispTime += MIN(1800, (jpbw->dispCount / 3)
                                      * msleeptime);
                if ((statusReq->reason != PEND_QUE_PRE_FAIL) &&
                    (statusReq->reason != PEND_JOB_PRE_EXEC)) {
                    jpbw->initFailCount++;
                    if (jpbw->initFailCount >= 20) {
                        char mailmsg[400];
                        sprintf(mailmsg,
                                _i18n_msg_get(ls_catd, NL_SETN, 1500,
                                              "We are unable to start your job <%s> because the initialization \nof your job execution environment failed.  This error has been repeated \nfor 20 times.  We have suspended your job.  You may resume it if the \ninitialization problem is fixed.\n"), /* catgets 1500 */
                                lsb_jobid2str(jpbw->jobId));
                        merr_user(jpbw->userName, jpbw->shared->jobBill.fromHost, mailmsg, "error");
                        jStatusChange(jpbw, statusReq->newStatus, LOG_IT, fname);
                        statusReq->newStatus = JOB_STAT_PSUSP;
                        jpbw->newReason = PEND_USER_STOP;
                        jpbw->initFailCount = 0;
                    }
                } else
                    jpbw->newReason = statusReq->reason;
            }

        }
    } else if (IS_FINISH (statusReq->newStatus)) {

        jpbw->newReason = EXIT_NORMAL;
        jpbw->exitStatus = statusReq->exitStatus;
        accumulateRU(jpbw, statusReq);
        if (jpbw->lsfRusage == NULL)
            jpbw->lsfRusage = &(statusReq->lsfRusage);

        if ((statusReq->newStatus & JOB_STAT_EXIT)
            && statusReq->sbdReply == ERR_HOST_BOOT
            && (jpbw->shared->jobBill.options & SUB_RERUNNABLE)) {
            mailUser (jpbw);
            jpbw->jStatus |= JOB_STAT_ZOMBIE;
            if ( jpbw->shared->jobBill.options & SUB_CHKPNTABLE &&
                 jpbw->jStatus & JOB_STAT_CHKPNTED_ONCE) {
                jpbw->shared->jobBill.options |= SUB_RESTART | SUB_RESTART_FORCE;
                jpbw->newReason = EXIT_RESTART;
            }
            else
                jpbw->newReason = EXIT_RERUN;
        }
        else {
            jpbw->shared->jobBill.options &= ~(SUB_RESTART | SUB_RESTART_FORCE);
            jpbw->newReason &= ~(SUB_RESTART | SUB_RESTART_FORCE);
        }

    }


    if (IS_FINISH(statusReq->newStatus) || IS_PEND(statusReq->newStatus)) {

        updHostLeftRusageMem(jpbw, 1);
        cleanSbdNode(jpbw);
    }

    jStatusChange(jpbw, statusReq->newStatus, LOG_IT, fname);


    if (jpbw->lsfRusage == &(statusReq->lsfRusage))
        jpbw->lsfRusage = NULL;

    if ((oldStatus & JOB_STAT_PRE_EXEC)
        && IS_RUN_JOB_CMD(statusReq->newStatus)) {
        jpbw->jStatus &= ~JOB_STAT_PRE_EXEC;
        jpbw->jobPid = statusReq->jobPid;
        jpbw->jobPGid = statusReq->jobPGid;
        jpbw->execUid = statusReq->execUid;

        FREEUP (jpbw->execUsername);
        if (statusReq->execUsername) {
            jpbw->execUsername = safeSave(statusReq->execUsername);
        } else {
            jpbw->execUsername = NULL;
        }

        log_startjob(jpbw, FALSE);
    }

    if (stopit)
        return (LSBE_STOP_JOB);
    if (jpbw->newReason & SUSP_MBD_LOCK)
        return(LSBE_LOCK_JOB);
    else
        return(LSBE_NO_ERROR);

}

static char
ususpPendingEvent(struct jData *jpbw)
{
    struct sbdNode *sbdPtr, *nextSbdPtr;

    if (jpbw->pendEvent.sig == SIG_SUSP_USER)
        return TRUE;

    for (sbdPtr = sbdNodeList.forw; sbdPtr != &sbdNodeList;
         sbdPtr = nextSbdPtr) {
        nextSbdPtr = sbdPtr->forw;
        if (sbdPtr->jData == jpbw && sbdPtr->reqCode == MBD_SIG_JOB &&
            sbdPtr->sigVal == SIG_SUSP_USER)
            return TRUE;
    }

    return (FALSE);
}


static char
terminatePendingEvent(struct jData *jpbw)
{
    struct sbdNode *sbdPtr, *nextSbdPtr;

    if (isSigTerm(jpbw->pendEvent.sig))
        return TRUE;

    for (sbdPtr = sbdNodeList.forw; sbdPtr != &sbdNodeList;
         sbdPtr = nextSbdPtr) {
        nextSbdPtr = sbdPtr->forw;
        if (sbdPtr->jData == jpbw && sbdPtr->reqCode == MBD_SIG_JOB &&
            isSigTerm(sbdPtr->sigVal))
            return TRUE;
    }

    return (FALSE);
}

int
rusageJob (struct statusReq *statusReq, struct hostent *hp)
{
    static char       fname[] = "rusageJob";
    struct jData      *jpbw;
    struct hData      *hData;
    int               diffSTime;
    int               diffUTime;
    int               diffTime;
    bool_t            significantChange = FALSE;

    if (logclass & (LC_TRACE | LC_SIGNAL))
        ls_syslog(LOG_DEBUG, "%s: Entering ... jobId %s", fname,
                  lsb_jobid2str(statusReq->jobId));


    if ((jpbw = getJobData (statusReq->jobId)) == NULL) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname, lsb_jobid2str(statusReq->jobId), "getJobData");
        return(LSBE_NO_JOB);
    }

    hData = getHostData (hp->h_name);
    if (hData == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6521,
                                         "%s: Received job rusage update from host <%s> that is not configured as a batch server"), fname, hp->h_name); /* catgets 6521 */
        return (LSBE_SBATCHD);
    }
    hStatChange (hData, 0);

    if (jpbw->runRusage.stime < 0)
        diffSTime = statusReq->runRusage.stime;
    else
        diffSTime = statusReq->runRusage.stime - jpbw->runRusage.stime;

    if (jpbw->runRusage.utime < 0)
        diffUTime = statusReq->runRusage.utime;
    else
        diffUTime = statusReq->runRusage.utime - jpbw->runRusage.utime;

    /*update cpuTime for fairshare account*/
    diffTime = diffSTime+diffUTime;
    if (jpbw->sa && diffTime > 0) {
        SACCT_CPUTIME_UPDATE(diffTime, jpbw->sa, fname);
        if (logclass & LC_FAIR) {
            ls_syslog(LOG_DEBUG1, "%s: updated SAAP <%s> cputime <%d> for job <%s>",
                    fname, jpbw->sa->path,
                    diffTime,
                    lsb_jobid2str(jpbw->jobId));
        }
        updSAcctPriorityByPath(jpbw->sa);
    }

    jpbw->jRusageUpdateTime = now;

    significantChange = isSignificantChange(&(jpbw->runRusage), &(statusReq->runRusage),
                                            0.1);

    copyJUsage(&(jpbw->runRusage), &(statusReq->runRusage));

    return (LSBE_NO_ERROR);
}


void
jStatusChange(struct jData *jData,
              int newStatus,
              time_t eventTime,
              const char *fname)
{
    int oldStatus = jData->jStatus;
    int freeExec = FALSE;
    time_t  now = time(NULL);

    if (MASK_STATUS (newStatus & ~JOB_STAT_UNKWN)
        == MASK_STATUS (oldStatus & ~JOB_STAT_UNKWN))
        return;


    newStatus = MASK_STATUS(newStatus);
    if (eventTime == LOG_IT && (jData->jStatus & JOB_STAT_RESERVE))
        resetReserve (jData, newStatus);


    if (eventTime == LOG_IT && JOB_PREEMPT_WAIT(jData) &&
        ((!IS_PEND(newStatus) && !(newStatus & JOB_STAT_SSUSP))
          || (newStatus & JOB_STAT_PSUSP) || IS_FINISH(newStatus))) {
        freeReservePreemptResources(jData);
    }


    if (eventTime == LOG_IT) {
        if ((jData->jStatus & JOB_STAT_RUN)
            && (!(newStatus & JOB_STAT_RUN) || IS_FINISH(newStatus)
                || IS_SUSP(newStatus)))
            updateStopJobPreemptResources(jData);
    }

    if (jData->jobId >= 0)
        updJgrpCountByJStatus(jData, jData->jStatus, newStatus);

    accumRunTime (jData, newStatus, eventTime);

    if ((mSchedStage != M_STAGE_REPLAY)
        && ((IS_PEND(jData->jStatus) && IS_START(newStatus))
            || (IS_START(jData->jStatus) && IS_FINISH(newStatus)))) {
        statusChanged = 1;
    }

    SET_STATE(jData->jStatus, newStatus);

    if ((jData->jStatus & JOB_STAT_PSUSP)
        && (jData->shared->jobBill.options2 & SUB2_HOLD)) {
        jData->startTime = jData->endTime = 0;
    }

    if (IS_START (newStatus) && IS_PEND (oldStatus)) {

        jData->newReason = 0;
        jData->subreasons = 0;
        jData->reserveTime = 0;
        if (eventTime == LOG_IT) {
            if ((jData->jStatus & JOB_STAT_PRE_EXEC))
                log_startjob(jData, TRUE);
            else
                log_startjob (jData, FALSE);
        }
        jData->jStatus &= ~JOB_STAT_MIG;
        offJobList (jData, PJLorMJL(jData));
        inStartJobList (jData);

    } else if (IS_FINISH (newStatus)) {

        if (!(jData->jStatus & JOB_STAT_ZOMBIE)) {
            if (jData->newReason != EXIT_INIT_ENVIRON &&
                jData->newReason != EXIT_NO_MAPPING &&
                jData->newReason != EXIT_PRE_EXEC)
                jData->newReason = EXIT_NORMAL;
        }


        if (((jData->shared->jobBill.options & SUB_INTERACTIVE)
             || (jData->shared->jobBill.options2 & SUB2_BSUB_BLOCK))
            && eventTime == LOG_IT) {
            if (!(jData->jStatus & JOB_STAT_ZOMBIE))
                breakCallback(jData, IS_PEND(oldStatus));
        }

        if (eventTime == LOG_IT) {
            log_newstatus(jData);
        }
        if (jData->runCount != INFINIT_INT)
            jData->runCount = MAX(0, jData->runCount-1);

        handleFinishJob(jData, oldStatus, eventTime);   

    } else if ((newStatus & JOB_STAT_PEND) && IS_START (oldStatus)) {
        if (eventTime == LOG_IT) {
            log_newstatus(jData);
            jobRequeueTimeUpdate(jData, now);
            detachedJobFromFSTree(jData, "jStatusChange()/job got pended");
        } else {
            jobRequeueTimeUpdate(jData, eventTime);
        }

        listRemoveEntry((LIST_T *)jDataList[SJL], (LIST_ENTRY_T *)jData);

        if (jData->newReason == PEND_JOB_PRE_EXEC
            || jData->newReason == PEND_QUE_PRE_FAIL
            || jData->newReason == PEND_SBD_JOB_REQUEUE)

            jData->startTime=0;

        if (jData->jStatus & JOB_STAT_MIG) {

            if (jData->shared->jobBill.options & SUB_CHKPNTABLE)
                jData->shared->jobBill.options |= SUB_RESTART | SUB_RESTART_FORCE;

            if (glMigToPendFlag == TRUE) {
                inPendJobList(jData, PJL, now);
                jData->jStatus &= ~JOB_STAT_MIG;
            } else {
                inPendJobList(jData, MJL, 0);
            }

        } else if (jData->newReason == PEND_SBD_JOB_REQUEUE) {

            inPendJobList(jData, PJL, now);
        } else  {
            inPendJobList(jData, PJL, 0);
        }
        jData->jStatus &= ~JOB_STAT_PRE_EXEC;
        setJobPendReason(jData, jData->newReason);
        freeExec = TRUE;

    } else {

        if (eventTime == LOG_IT)
            log_newstatus(jData);

        if (newStatus & JOB_STAT_RUN) {
            jData->newReason = 0;
            jData->subreasons = 0;
            jData->reserveTime = 0;
            jData->jFlags &= ~JFLAG_SEND_SIG;
        }
    }


    if (mSchedStage != M_STAGE_REPLAY) {
        updCounters (jData, oldStatus, eventTime);
    } else {
        /*mbatchd is replaying */
        if (eventTime == LOG_IT) {
            updCounters (jData, oldStatus, eventTime);
        } else if (oldStatus & JOB_STAT_RUN
                   && oldStatus & JOB_STAT_WAIT
                   && newStatus & (JOB_STAT_SSUSP | JOB_STAT_USUSP)) {
            offJobList(jData, SJL);
            if (newStatus & JOB_STAT_SSUSP) {
                jData->jStatus = JOB_STAT_PEND;
            } else {
                jData->jStatus = JOB_STAT_PSUSP;
            }
            FREEUP(jData->hPtr);
            jData->numHostPtr = 0;
            inPendJobList(jData, PJL, 0);
        }

    }

    if (freeExec == TRUE) {
        freeExecParams (jData);
    }
}

void
cleanSbdNode(struct jData *jpbw)
{
    struct sbdNode *sbdPtr, *nextSbdPtr;

    for (sbdPtr = sbdNodeList.forw; sbdPtr != &sbdNodeList;
         sbdPtr = nextSbdPtr) {
        nextSbdPtr = sbdPtr->forw;
        if (sbdPtr->jData == jpbw) {
            chanClose_(sbdPtr->chanfd);
            offList((struct listEntry *) sbdPtr);
            FREEUP(sbdPtr);
            nSbdConnections--;
        }
    }
}


static void
updateStopJobPreemptResources(struct jData *jp)
{
    int resn;

    if (MARKED_WILL_BE_PREEMPTED(jp)) {
        jp->jFlags &= ~JFLAG_WILL_BE_PREEMPTED;
        return;
    }

    FORALL_PRMPT_RSRCS(resn) {
        int hostn;
        float val;
        GET_RES_RSRC_USAGE(resn, val, jp->shared->resValPtr,
                           jp->qPtr->resValPtr);
        if (val <= 0.0)
            continue;

        for (hostn = 0;
             hostn == 0 || (slotResourceReserve && hostn < jp->numHostPtr);
             hostn++) {
            addAvailableByPreemptPRHQValue(resn, val, jp->hPtr[hostn], jp->qPtr);
        }
    } ENDFORALL_PRMPT_RSRCS;

    return;
}

static void
resetReserve (struct jData *jData, int newStatus)
{
    if (!(jData->jStatus & JOB_STAT_RESERVE))
        return;

    if (((newStatus & JOB_STAT_PSUSP) || IS_FINISH(newStatus))
        && (jData->jStatus & JOB_STAT_PEND)) {



        if (logclass & LC_TRACE )
            ls_syslog(LOG_DEBUG3, "resetReserve: job <%s> freeing <%d> reserved slots <%s:%d>",
                      lsb_jobid2str(jData->jobId),
                      jData->numHostPtr, __FILE__,__LINE__);
        freeReserveSlots(jData);
        jData->jStatus  &=  ~JOB_STAT_RESERVE;
        jData->reserveTime = 0;
    }
    if (IS_START(jData->jStatus) &&
        ((newStatus & JOB_STAT_RUN) || IS_FINISH(newStatus) ||
         (newStatus & JOB_STAT_USUSP) || (newStatus & JOB_STAT_PEND))) {
        if (logclass & (LC_TRACE| LC_SCHED ))
            ls_syslog(LOG_DEBUG3, "resetReserve: job <%s> updRes - <%d> slots <%s:%d>",
                      lsb_jobid2str(jData->jobId), jData->numHostPtr,
                      __FILE__,  __LINE__);

        updResCounters (jData, ~JOB_STAT_RESERVE  & jData->jStatus);
        jData->jStatus &= ~JOB_STAT_RESERVE;
    }

}

static void
handleFinishJob(struct jData *jData, int oldStatus, int eventTime)
{
    int listno;


    if (((jData->shared->jobBill.options & SUB_RERUNNABLE)
         || (jData->pendEvent.sigDel & DEL_ACTION_REQUEUE))
        && ((oldStatus & JOB_STAT_ZOMBIE)
            || (jData->pendEvent.sigDel & DEL_ACTION_REQUEUE))
        && !(jData->newReason & EXIT_KILL_ZOMBIE)) {
        jData->jFlags |= JFLAG_REQUEUE;
    }

    jData->jStatus &= ~JOB_STAT_MIG;
    jData->pendEvent.sig1 = SIG_NULL;
    jData->pendEvent.sig = SIG_NULL;
    jData->pendEvent.notSwitched = FALSE;
    jData->pendEvent.notModified = FALSE;

    if (IS_START(oldStatus)) {
        listno = SJL;
    } else {
        listno = PJLorMJL(jData);
    }
    offJobList(jData, listno);
    inList ((struct listEntry *)jDataList[FJL]->forw,
            (struct  listEntry *)jData);

    if( (jData->shared->jobBill.options & SUB_MODIFY_ONCE) &&
        (jData->newSub) ) {
        changeJobParams (jData);
    }

}

void
handleRequeueJob(struct jData *jData, time_t requeueTime)
{
    jData->jFlags &= ~JFLAG_REQUEUE;

    if (jData->newSub)
        changeJobParams (jData);
    if (jData->shared->dptRoot)
        resetDepCond(jData->shared->dptRoot);

    jData->runCount = 1;


    if (jData->jStatus & JOB_STAT_DONE)
        jData->jFlags |= JFLAG_LASTRUN_SUCC;
    else
        jData->jFlags &= ~JFLAG_LASTRUN_SUCC;


    jData->jFlags &= ~(JFLAG_READY1 | JFLAG_READY2);
    jData->jFlags &= ~(JFLAG_URGENT | JFLAG_URGENT_NOSTOP);

    if ((jData->shared->jobBill.options & SUB_RERUNNABLE)&&
        (jData->jStatus & JOB_STAT_MIG))
        jData->startTime = 0;


    if ((jData->shared->jobBill.options & SUB_RERUNNABLE)
        && (jData->jStatus & JOB_STAT_CHKPNTED_ONCE)) {
        jData->restartPid = jData->jobPid;
    }


    updJgrpCountByJStatus(jData, jData->jStatus, JOB_STAT_PEND);
    jData->jStatus = JOB_STAT_PEND;
    jData->cpuTime = 0;

    FREEUP(jData->lsfRusage);

    if (jData->shared->jobBill.userPriority > 0) {
        jData->jobPriority = jData->shared->jobBill.userPriority;
    } else if (maxUserPriority > 0) {
        jData->jobPriority = maxUserPriority/2 ;
    } else {
        jData->jobPriority = -1;
    }

    offJobList(jData, FJL);
    inPendJobList(jData, PJL, requeueTime);
    jobRequeueTimeUpdate(jData, requeueTime);

    if (jData->runRusage.npids > 0)
        FREEUP (jData->runRusage.pidInfo);
    if (jData->runRusage.npgids > 0)
        FREEUP (jData->runRusage.pgid);
    memset(&jData->runRusage, 0, sizeof(jData->runRusage));

    if (jData->pendEvent.sig1Flags == JOB_STAT_PSUSP){

        jData->jStatus = JOB_STAT_PSUSP;
        setJobPendReason(jData, PEND_USER_STOP);
        updJgrpCountByJStatus(jData, JOB_STAT_PEND, JOB_STAT_PSUSP);
        jData->pendEvent.sig1Flags = 0;

    } else {
        setJobPendReason(jData, PEND_JOB_REQUEUED);
    }

    freeExecParams(jData);
    if (mSchedStage != M_STAGE_REPLAY) {
        if (!jData->uPtr)
            jData->uPtr = getUserData(jData->userName);
        updQaccount(jData, jData->shared->jobBill.maxNumProcessors,
                    jData->shared->jobBill.maxNumProcessors, 0, 0, 0, 0);
        updUserData(jData, jData->shared->jobBill.maxNumProcessors,
                    jData->shared->jobBill.maxNumProcessors, 0, 0, 0, 0);
    }

    if (jData->jFlags & JFLAG_HAS_BEEN_REQUEUED)
        numRemoveJobs++;
    else
        jData->jFlags |= JFLAG_HAS_BEEN_REQUEUED;
}

static void
changeJobParams(struct jData *jData)
{
    static char fname[]="changeJobParams";
    struct submitReq *oldSub = NULL;
    struct qData *qPtr;
    int errcode, jFlags;
    struct lsfAuth auth;
    struct askedHost *askedHosts = NULL;
    int askedOthPrio, numAskedHosts, badReqIndx;
    int i, returnErr;

    if ((jData->newSub->options & SUB_MODIFY_ONCE) &&
        ! (jData->jStatus & JOB_STAT_MODIFY_ONCE)) {

        jData->jStatus |= JOB_STAT_MODIFY_ONCE;

        oldSub = saveOldParameters (jData);
    } else {

        jData->jStatus &= ~JOB_STAT_MODIFY_ONCE;
    }

    freeSubmitReq (&jData->shared->jobBill);
    copyJobBill (jData->newSub, &jData->shared->jobBill, FALSE);
    freeSubmitReq (jData->newSub);
    FREEUP (jData->newSub);


    if (jData->shared->jobBill.options & SUB_HOST) {
        returnErr = chkAskedHosts (jData->shared->jobBill.numAskedHosts,
                                   jData->shared->jobBill.askedHosts,
                                   jData->shared->jobBill.numProcessors,
                                   &numAskedHosts,
                                   &askedHosts, &badReqIndx,
                                   &askedOthPrio, 1);
        if (returnErr != LSBE_NO_ERROR)
            return;
        if (numAskedHosts) {
            if (jData->askedPtr)
                FREEUP(jData->askedPtr);
            jData->askedPtr = (struct askedHost *) my_calloc (numAskedHosts,
                                                              sizeof(struct askedHost), fname);
            for (i = 0; i < numAskedHosts; i++) {
                jData->askedPtr[i].hData = askedHosts[i].hData;
                jData->askedPtr[i].priority = askedHosts[i].priority;
            }
            jData->numAskedPtr = numAskedHosts;
            jData->askedOthPrio = askedOthPrio;
            FREEUP (askedHosts);
        }
    }
    else {
        FREEUP(jData->askedPtr);
        jData->numAskedPtr = 0;
        jData->askedOthPrio = 0;
    }
    if (jData->shared->dptRoot) {
        freeDepCond(jData->shared->dptRoot);
        jData->shared->dptRoot = NULL;
    }

    lsbFreeResVal (&jData->shared->resValPtr);
    if (jData->shared->jobBill.resReq && jData->shared->jobBill.resReq[0] != '\0') {
        int useLocal = TRUE;
        if (jData->numAskedPtr > 0 || jData->askedOthPrio >= 0)
            useLocal = FALSE;

        useLocal = useLocal ? USE_LOCAL : 0;
        if ((jData->shared->resValPtr = checkResReq (jData->shared->jobBill.resReq, useLocal| CHK_TCL_SYNTAX | PARSE_XOR)) == NULL)

            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6522,
                                             "%s:Bad modified resource requirement<%s> for job<%s>"), /* catgets 6522 */
                      fname,
                      jData->shared->jobBill.resReq,
                      lsb_jobid2str(jData->jobId));
    }
    auth.uid = jData->userId;
    strcpy (auth.lsfUserName, jData->userName);

    jFlags = 0;
    if (strcmp (jData->shared->jobBill.dependCond, "") != 0) {


        setIdxListContext((*(*jData).shared).jobBill.jobName);

        jData->shared->dptRoot
            = parseDepCond(jData->shared->jobBill.dependCond,
                           &auth, &errcode, NULL, &jFlags, 0);
        if (jData->shared->dptRoot == NULL) {
            jFlags |= JFLAG_DEPCOND_INVALID;
        }

        freeIdxListContext();

    } else {
        jFlags &= ~JFLAG_DEPCOND_INVALID;
    }

    jFlags = 0;

    jData->jFlags |= jFlags;

    if (oldSub)
        jData->newSub = oldSub;
    qPtr = getQueueData (jData->shared->jobBill.queue);
    if (qPtr != jData->qPtr)
        jData->qPtr = qPtr;

    jData->runCount = 1;

}

static void
freeExecParams(struct jData *jData)
{
    jData->numHostPtr = 0;
    jData->nextSeq = 1;
    FREEUP(jData->execHome);
    FREEUP(jData->queuePreCmd);
    FREEUP(jData->queuePostCmd);


    if (jData->execCwd != NULL
        && jData->hPtr != NULL) {
        int len = 0;
        static char execCwd[MAXLINELEN + 1];

        if (jData->execCwd) {
            len = strlen(jData->execCwd) + strlen(jData->hPtr[0]->host) + 5;
        } else {
            len = strlen(jData->hPtr[0]->host) + 5;
        }

        if (jData->execCwd != NULL
            && ((strlen(jData->execCwd) == 1)
                || ((strlen(jData->execCwd) > 1)
                    && ((jData->execCwd[0] != '/' )
                        || (jData->execCwd[1] != '/'))))) {
            strcpy(execCwd, jData->execCwd);
            jData->execCwd = realloc(jData->execCwd, len);

            sprintf(jData->execCwd, "//%s%s", jData->hPtr[0]->host,
                    execCwd);
        }
    }

    FREEUP(jData->hPtr);
    cleanCandHosts(jData);
}

/* clean()
 * Free done jobs requeue jobs that have to be
 * requeued from FJL to PJL.
 */
void
clean(time_t curTime)
{
    struct jData *jPtr;
    struct jData *nextJobPtr;
    struct sbdNode *sbdPtr;
    int found;

    for (jPtr = jDataList[FJL]->back;
         jPtr != jDataList[FJL];
         jPtr = nextJobPtr) {

        nextJobPtr = jPtr->back;
        found = FALSE;

        if (jPtr->jStatus & JOB_STAT_DONE) {
            if (!IS_POST_FINISH(jPtr->jStatus)) {
                continue;
            }
        }

        if (jPtr->jFlags & JFLAG_REQUEUE) {

            handleRequeueJob(jPtr, time(NULL));
            jPtr->startTime = jPtr->endTime = 0;
            jPtr->pendEvent.sig = SIG_NULL;
            log_jobrequeue(jPtr);
            continue;
        }

        if (getZombieJob(jPtr->jobId) != NULL)
            continue;

        if (nSbdConnections > 0) {

            for (sbdPtr = sbdNodeList.forw; sbdPtr != &sbdNodeList;
                 sbdPtr = sbdPtr->forw) {
                if (sbdPtr->jData != jPtr)
                    continue;
                found = TRUE;
                break;
            }
            if (found == TRUE)
                continue;
        }

        if (jPtr->pendEvent.sigDel) {
            if (!jPtr->runCount)  {
                removeJob(jPtr->jobId);
                continue;
            }
        }

        if (curTime - jPtr->endTime > clean_period)
            removeJob(jPtr->jobId);
    }
}

void
job_abort(struct jData *jData, char reason)
{
    static char fname[] = "job_abort";
    char mailmsg[2048];
    char *cp;
    char  *reasonp;
    char  *temp;

    jData->shared->jobBill.options &= ~(SUB_RESTART | SUB_RESTART_FORCE);
    jData->newReason &= ~(SUB_RESTART | SUB_RESTART_FORCE);
    jStatusChange(jData, JOB_STAT_EXIT, LOG_IT, fname);

    reasonp = _i18n_msg_get(ls_catd, NL_SETN, 1512,
                            "why?"); /* catgets 1512 */

    if (reason == BAD_SUB)
        return;

    if (reason == BAD_LOAD)
        reasonp = ls_sysmsg();
    else if (reason == TOO_LATE)
        reasonp = _i18n_msg_get(ls_catd, NL_SETN, 1501,
                                "Job termination deadline reached"); /* catgets 1501 */
    else if (reason == FILE_MISSING)
        reasonp = _i18n_msg_get(ls_catd, NL_SETN, 1502,
                                "Job info file was missing; see error log file for details"); /* catgets 1502 */
    else if (reason == JOB_MISSING)
        reasonp = _i18n_msg_get(ls_catd, NL_SETN, 1503,
                                "Job was missing by sbatchd"); /* catgets 1503 */
    else if (reason == BAD_USER)
        reasonp = _i18n_msg_get(ls_catd, NL_SETN, 1504,
                                "User not recognizable"); /* catgets 1504 */
    else if (reason == MISS_DEADLINE) {
        char lmsg[1024], *timebuf;
        int runLimit, isRunLimit = TRUE;
        time_t currentTime, finishTime;
        currentTime = time(NULL);
        sprintf(lmsg, _i18n_msg_get(ls_catd, NL_SETN, 1506,
                                    "Job will not finish before termination deadline even on the fastest host(s)\n\
  (the one(s) with the largest cpuFactor) using maximum number of processors:\n")); /* catgets 1506 */
        timebuf = putstr_(_i18n_ctime(ls_catd,
                                      CTIME_FORMAT_a_b_d_T_Y, &currentTime));
        sprintf(lmsg+strlen(lmsg), _i18n_msg_get(ls_catd, NL_SETN, 1507,
                                                 "expected finish time = (current time: %s) +\n"), /* catgets 1507 */
                timebuf);
        free(timebuf);
        if ((runLimit = RUN_LIMIT_OF_JOB(jData)) <= 0) {
            runLimit = CPU_LIMIT_OF_JOB(jData)/jData->shared->jobBill.maxNumProcessors;
            isRunLimit = FALSE;
        }
        runLimit /= maxCpuFactor;
        finishTime = currentTime + runLimit;
        timebuf = putstr_(_i18n_ctime(ls_catd,
                                      CTIME_FORMAT_a_b_d_T_Y, &finishTime));
        if (isRunLimit) {
            sprintf(lmsg+strlen(lmsg), _i18n_msg_get(ls_catd, NL_SETN, 1508,
                                                     "(run time limit on the fastest host: %d seconds)\n"), /* catgets 1508 */
                    runLimit);
        } else {
            sprintf(lmsg+strlen(lmsg), _i18n_msg_get(ls_catd, NL_SETN, 1509,
                                                     "(CPU time limit on the fastest host: %d seconds)\n"), /* catgets 1509 */
                    runLimit);
        }
        sprintf(lmsg+strlen(lmsg), "\
                       = %s;\n", timebuf);
        free(timebuf);
        timebuf = putstr_(_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T_Y,
                                      &jData->shared->jobBill.termTime));
        sprintf(lmsg+strlen(lmsg), _i18n_msg_get(ls_catd, NL_SETN, 1510,
                                                 "termination deadline = %s"), /* catgets 1510 */
                timebuf);
        free(timebuf);
        reasonp = lmsg;
    }

    temp = putstr_(reasonp);
    sprintf(mailmsg, _i18n_msg_get(ls_catd, NL_SETN, 1511,
                                   "We are unable to schedule your job %s: <%s>.  The error is:\n  %s."), /* catgets 1511 */
            lsb_jobid2str(jData->jobId),
            jData->shared->jobBill.command,
            temp);
    free(temp);

    cp = strchr(mailmsg, '\0');
    if (reason == BAD_LOAD) {
        if (jData->shared->jobBill.resReq[0] != '\0') {
            sprintf(cp, _i18n_msg_get(ls_catd, NL_SETN, 1513,
                                      "\nThe resource requirements of the job are: <%s>."), /* catgets 1513 */
                    jData->shared->jobBill.resReq);
        } else {
            sprintf(cp, _i18n_msg_get(ls_catd, NL_SETN, 1514,
                                      "\nThe resource requirements of the job are:\n    the default - same type of host as the one you submitted the job.\n")); /* catgets 1514 */
        }
    }


    if (jData->shared->jobBill.options & SUB_MAIL_USER)
        merr_user(jData->shared->jobBill.mailUser,
                  jData->shared->jobBill.fromHost,
                  mailmsg,
                  I18N_error);
    else
        merr_user(jData->userName, jData->shared->jobBill.fromHost, mailmsg,
                  I18N_error);

    return;

}

int
switchJobArray(struct jobSwitchReq *switchReq,
               struct lsfAuth      *auth)
{
    static char       fname[] = "switchJobArray";
    struct jData      *jArrayPtr;
    struct qData      *qPtr;
    int               cc;


    jArrayPtr = getJobData(switchReq->jobId);
    if (jArrayPtr == NULL) {
        return(LSBE_NO_JOB);
    }


    if (auth != NULL
        && (auth->uid != 0)
        && !jgrpPermitOk(auth, jArrayPtr->jgrpNode)
        && !isAuthQueAd (jArrayPtr->qPtr, auth)) {

        return(LSBE_PERMISSION);
    }


    qPtr = getQueueData(switchReq->queue);
    if (qPtr == NULL) {
        return(LSBE_BAD_QUEUE);
    }

    if (auth != NULL
        && (auth->uid != 0)
        && !jgrpPermitOk(auth, jArrayPtr->jgrpNode)
        && !isAuthQueAd (qPtr, auth)) {

        return(LSBE_PERMISSION);
    }


    if (jArrayPtr->nodeType == JGRP_NODE_ARRAY) {
        struct jData      *jPtr;

        for (jPtr = jArrayPtr->nextJob;
             jPtr != NULL;
             jPtr = jPtr->nextJob) {


            if (IS_FINISH(jPtr->jStatus)) {
                continue;
            }

            switchReq->jobId = jPtr->jobId;

            cc = switchAJob(switchReq, auth, qPtr);
            if (cc != LSBE_NO_ERROR) {

                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: failed to switch element <%s>, cc=%d",
                              fname, lsb_jobid2str(jPtr->jobId), cc);
                }
            }

        }


        FREEUP(jArrayPtr->shared->jobBill.queue);
        jArrayPtr->shared->jobBill.queue = safeSave(qPtr->queue);
        jArrayPtr->qPtr = qPtr;

    } else {


        cc = switchAJob(switchReq, auth, qPtr);

    }

    return(cc);

}


static int
switchAJob (struct jobSwitchReq *switchReq,
            struct lsfAuth *auth,
            struct qData   *qtp)
{
    static char fname[]="switchAJob";
    struct qData  *qfp;
    struct jData  *job = NULL;
    int returnErr, noUse, i;
    struct submitReq *newSub;

    if (qtp == NULL) {
        return(LSBE_BAD_QUEUE);
    }


    if ((job = getJobData(switchReq->jobId)) == NULL)
        return (LSBE_NO_JOB);

    if (job->nodeType != JGRP_NODE_JOB) {
        return(LSBE_JOB_ARRAY);
    }


    if (LSB_ARRAY_IDX(job->jobId) != 0) {

        localizeJobElement(job);
    }

    if (IS_FINISH (job->jStatus))
        return (LSBE_JOB_FINISH);



    if (qtp->pJobLimit <= 0.0)
        return (LSBE_PJOB_LIMIT);
    if (qtp->maxJobs <= 0)
        return (LSBE_QJOB_LIMIT);
    if (qtp->hJobLimit <= 0)
        return (LSBE_HJOB_LIMIT);


    if (IS_PEND(job->jStatus)) {
        if (!uJobLimitOk (job, qtp->uAcct, qtp->uJobLimit, 0))
            return (LSBE_UJOB_LIMIT);
    } else {
        if (!uJobLimitOk (job, qtp->uAcct, qtp->uJobLimit, 1))
            return (LSBE_UJOB_LIMIT);
    }

    if (!IS_PEND (job->jStatus)) {
        if (qtp->maxJobs <= (qtp->numJobs - qtp->numPEND))
            return (LSBE_QJOB_LIMIT);

        for (i = 0; i < job->numHostPtr; i++) {
            struct hostAcct *hAcct = getHAcct(qtp->hAcct, job->hPtr[i]);
            if (!pJobLimitOk (job->hPtr[i], hAcct, qtp->pJobLimit))
                return (LSBE_PJOB_LIMIT);
            if (!hJobLimitOk (job->hPtr[i], hAcct, qtp->hJobLimit))
                return (LSBE_HJOB_LIMIT);
        }
    }


    if (IS_START (job->jStatus)) {
        if (job->numHostPtr > 0 && qtp->procLimit > 0) {
            if (job->numHostPtr > qtp->procLimit)
                return (LSBE_PROC_NUM);
            else if (job->numHostPtr < qtp->minProcLimit)
                return (LSBE_PROC_LESS);
        }
    }

    if ((returnErr = acceptJob (qtp, job, &noUse, auth)) != LSBE_NO_ERROR)
        return(returnErr);


    qfp = job->qPtr;
    if (IS_PEND(job->jStatus) && (job->jStatus & JOB_STAT_RESERVE)) {


        if (logclass & (LC_TRACE| LC_SCHED ))
            ls_syslog(LOG_DEBUG3, "%s: job <%s> freeing <%d> slots <%s:%d>",
                      fname, lsb_jobid2str(job->jobId),
                      job->numHostPtr,
                      __FILE__,  __LINE__);

        freeReserveSlots(job);
        job->reserveTime = 0;
    }

    if (JOB_PREEMPT_WAIT(job))
        freeReservePreemptResources(job);

    jobInQueueEnd (job, qtp);
    updSwitchJob (job, qfp, qtp, job->shared->jobBill.maxNumProcessors);

    if (qfp != qtp) {
        /*Update for fairshare policy*/
        if (mSchedStage != M_STAGE_REPLAY) {
            updJobSAcctForSwitch(job, qfp, qtp);
        }

        if (auth != NULL)
            log_switchjob (switchReq, auth->uid, auth->lsfUserName);
        else
            log_switchjob (switchReq, job->userId, job->userName);
    }
    if (!IS_PEND (job->jStatus)) {

        job->pendEvent.notSwitched = TRUE;
        eventPending = TRUE;
    } else if (!(job->jStatus & JOB_STAT_PSUSP)) {

        if (qfp != qtp) {
            cleanCandHosts (job);
        }
        setJobPendReason(job, PEND_JOB_SWITCH);
    }


    if (job->newSub) {
        freeSubmitReq (job->newSub);
        FREEUP (job->newSub);
    }
    if ( IS_START(job->jStatus) ) {
        newSub = (struct submitReq *) my_malloc
            (sizeof (struct submitReq), "switchAJob");
        copyJobBill (&(job->shared->jobBill), newSub, FALSE);
        job->newSub = newSub;
    }


    if ( qfp != qtp ) {
        if ((job->shared->jobBill.options & SUB_CHKPNT_DIR )
            && !(job->shared->jobBill.options2 & SUB2_QUEUE_CHKPNT)) {


        } else {

            if ( !IS_START (job->jStatus)) {


                FREEUP(job->shared->jobBill.chkpntDir);
                job->shared->jobBill.options  &= ~ SUB_CHKPNT_DIR;
                job->shared->jobBill.options  &= ~ SUB_CHKPNT_PERIOD;
                job->shared->jobBill.options2 &= ~ SUB2_QUEUE_CHKPNT;
                if ( qtp->qAttrib & Q_ATTRIB_CHKPNT ) {

                    char dir[MAXLINELEN];
                    sprintf(dir, "%s/%s", qtp->chkpntDir,
                            lsb_jobid2str(job->jobId));

                    job->shared->jobBill.chkpntDir = safeSave(dir);
                    job->shared->jobBill.chkpntPeriod = qtp->chkpntPeriod;
                    job->shared->jobBill.options  |= SUB_CHKPNT_DIR;
                    job->shared->jobBill.options  |= SUB_CHKPNT_PERIOD;
                    job->shared->jobBill.options2 |= SUB2_QUEUE_CHKPNT;
                }
                else {
                    job->shared->jobBill.chkpntDir = safeSave("");
                }
            } else {


                FREEUP(job->newSub->chkpntDir);
                job->newSub->options  &= ~ SUB_CHKPNT_DIR;
                job->newSub->options  &= ~ SUB_CHKPNT_PERIOD;
                job->newSub->options2 &= ~ SUB2_QUEUE_CHKPNT;
                if ( qtp->qAttrib & Q_ATTRIB_CHKPNT ) {

                    char dir[MAXLINELEN];
                    sprintf(dir, "%s/%s", qtp->chkpntDir,
                            lsb_jobid2str(job->jobId));
                    job->newSub->chkpntDir = safeSave(dir);
                    job->newSub->chkpntPeriod = qtp->chkpntPeriod;
                    job->newSub->options  |= SUB_CHKPNT_DIR;
                    job->newSub->options  |= SUB_CHKPNT_PERIOD;
                    job->newSub->options2 |= SUB2_QUEUE_CHKPNT;
                }
                else {
                    job->newSub->chkpntDir = safeSave("");
                }
            }
        }
    }



    if ( qfp != qtp ) {
        if ((job->shared->jobBill.options & SUB_RERUNNABLE )

            && !(job->shared->jobBill.options2 & SUB2_QUEUE_RERUNNABLE)) {

        } else {

            if ( !IS_START (job->jStatus)) {


                job->shared->jobBill.options  &= ~ SUB_RERUNNABLE;
                job->shared->jobBill.options2  &= ~ SUB2_QUEUE_RERUNNABLE;
                if ( qtp->qAttrib & Q_ATTRIB_RERUNNABLE ) {

                    job->shared->jobBill.options  |= SUB_RERUNNABLE;
                    job->shared->jobBill.options2  |= SUB2_QUEUE_RERUNNABLE;
                }
            } else {


                job->newSub->options  &= ~ SUB_RERUNNABLE;
                job->newSub->options2  &= ~ SUB2_QUEUE_RERUNNABLE;
                if ( qtp->qAttrib & Q_ATTRIB_RERUNNABLE ) {

                    job->newSub->options  |= SUB_RERUNNABLE;
                    job->newSub->options2  |= SUB2_QUEUE_RERUNNABLE;
                }
            }
        }
    }

    return(LSBE_NO_ERROR);

}
int
moveJobArray(struct jobMoveReq *moveReq,
             int               log,
             struct lsfAuth    *auth)
{
    static char       fname[] = "moveJobArray";
    struct jData      *jArrayPtr;
    int               cc;
    int               savePosition;

    jArrayPtr = getJobData(moveReq->jobId);
    if (jArrayPtr == NULL) {
        return(LSBE_NO_JOB);
    }


    if (auth->uid == 0 ||
        isAuthManagerExt(auth) ||
        isAuthQueAd (jArrayPtr->qPtr, auth)) {
    } else {
        if (!jgrpPermitOk(auth, jArrayPtr->jgrpNode)) {

            return(LSBE_PERMISSION);
        }
    }

    if (jArrayPtr->nodeType == JGRP_NODE_ARRAY) {
        struct jData           *jPtr;
        struct jgTreeNode      *jgTreePtr;
        struct jarray          *jArray;


        jgTreePtr = jArrayPtr->jgrpNode;


        jArray    = ARRAY_DATA(jgTreePtr);


        if (jArray->counts[JGRP_COUNT_NJOBS]
            ==  jArray->counts[JGRP_COUNT_NDONE]
            + jArray->counts[JGRP_COUNT_NEXIT]) {


            return(LSBE_JOB_FINISH);
        }


        savePosition = moveReq->position;

        for (jPtr = jArrayPtr->nextJob;
             jPtr != NULL;
             jPtr = jPtr->nextJob) {


            if ( ! IS_PEND(jPtr->jStatus)) {
                continue;
            }

            moveReq->jobId = jPtr->jobId;

            cc = moveAJob(moveReq, log, auth);
            if (cc != LSBE_NO_ERROR) {
                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: failed to move job %s %s", fname, lsb_jobid2str(jPtr->jobId),
                              lsb_sysmsg());
                }

            }

            if (moveReq->opCode == TO_TOP) {
                moveReq->position++;
            }
        }
    } else {

        cc = moveAJob(moveReq, log, auth);

    }


    if (jArrayPtr->nodeType == JGRP_NODE_ARRAY) {
        moveReq->position = savePosition;
    }

    return(cc);

}

static int
moveAJob (struct jobMoveReq *moveReq, int log, struct lsfAuth *auth)
{
    struct jData *jp, *next, *job, *froJob = NULL, *toJob = NULL;
    int nowpos, list;
    int moveJobNum = 0, backword = FALSE;

    if ((job = getJobData (moveReq->jobId)) == NULL)
        return (LSBE_NO_JOB);

    if (job->nodeType != JGRP_NODE_JOB)
        return(LSBE_JOB_ARRAY);


    if (!IS_PEND (job->jStatus)) {
        if (IS_FINISH (job->jStatus))
            return (LSBE_JOB_FINISH);
        else
            return (LSBE_JOB_STARTED);
    }

    job->jFlags |= JFLAG_BTOP;

    nowpos = 0;
    if (moveReq->opCode == TO_TOP) {

        list = PJLorMJL(job);
        for (jp = jDataList[list]->back; jp != jDataList[list]; jp = next) {
            next = jp->back;
            if (nowpos && jp->qPtr->priority != job->qPtr->priority) {
                break;
            }

            if (jp->qPtr != job->qPtr)
                continue;
            if ( maxUserPriority > 0 ) {

                if ( jp->jobPriority != job->jobPriority) {
                    continue;
                }
            }
            if (auth->uid != 0 && !isAuthManagerExt(auth) &&
                !isAuthQueAd (job->qPtr, auth)) {
                if (jp->uPtr != job->uPtr) {

                    continue;
                }
            }
            nowpos++;
            if (jp->jobId == moveReq->jobId) {
                froJob = jp;
                moveJobNum = nowpos;
            }
            if (nowpos ==  moveReq->position)
                toJob = jp;
        }
        if (moveReq->position > nowpos) {

            moveReq->position = nowpos;
            return(moveAJob(moveReq, log, auth));
        }
        if (moveReq->position != moveJobNum) {
            if (froJob == NULL || toJob == NULL)
                return (LSBE_MBATCHD);
            if (moveReq->position < moveJobNum)
                backword = TRUE;
            if (auth->uid == 0 || isAuthManagerExt(auth) ||
                isAuthQueAd (job->qPtr, auth))
                insertAndShift(froJob, toJob, backword, FALSE);
            else
                insertAndShift(froJob, toJob, backword, TRUE);
        }
    } else {
        list = PJLorMJL(job);
        for (jp = jDataList[list]->forw; jp != jDataList[list]; jp = next) {
            next = jp->forw;
            if (nowpos && jp->qPtr->priority != job->qPtr->priority) {
                break;
            }

            if (jp->qPtr != job->qPtr)
                continue;
            if ( maxUserPriority > 0 ) {

                if ( jp->jobPriority != job->jobPriority) {
                    continue;
                }
            }
            if (auth->uid != 0 && !isAuthManagerExt(auth) &&
                !isAuthQueAd (job->qPtr, auth))
                if (jp->uPtr != job->uPtr)
                    continue;
            nowpos++;
            if (jp->jobId == moveReq->jobId) {
                froJob = jp;
                moveJobNum = nowpos;
            }
            if (nowpos ==  moveReq->position)
                toJob = jp;
        }

        if (moveReq->position > nowpos) {

            moveReq->position = nowpos;
            return(moveAJob(moveReq, log, auth));
        }
        if (moveReq->position != moveJobNum) {
            if (froJob == NULL || toJob == NULL)
                return (LSBE_MBATCHD);
            if (moveReq->position > moveJobNum)
                backword = TRUE;
            if (auth->uid == 0 || isAuthManagerExt(auth) ||
                isAuthQueAd (job->qPtr, auth))
                insertAndShift(froJob, toJob, backword, FALSE);
            else
                insertAndShift(froJob, toJob, backword, TRUE);
        }
    }

    if (log ) {
        log_movejob (moveReq, auth->uid, auth->lsfUserName);
    }
    return (LSBE_NO_ERROR);

}

static void
insertAndShift (struct jData *froJob, struct jData *toJob,
                int backward, int ordUser)
{
    struct jData *tmpJobP, *oldJobP, *jobP;
    int listno;

    listno = PJLorMJL(froJob);
    if (backward) {
        oldJobP = toJob->forw;
        tmpJobP = toJob;
        for (jobP = toJob->back; jobP != froJob->back; jobP = jobP->back) {
            if (jobP->qPtr != toJob->qPtr)
                continue;
            if ((ordUser == TRUE) && (jobP->uPtr != toJob->uPtr)) {

                continue;
            }
            offJobList (tmpJobP, listno);

            listInsertEntryAfter((LIST_T *)jDataList[listno],
                                 (LIST_ENTRY_T *)jobP,
                                 (LIST_ENTRY_T *)tmpJobP);
            tmpJobP = jobP;
        }
        offJobList(froJob, listno);
        listInsertEntryBefore((LIST_T *)jDataList[listno],
                              (LIST_ENTRY_T *)oldJobP,
                              (LIST_ENTRY_T *)froJob);
    }
    else {
        oldJobP = toJob->back;
        tmpJobP = toJob;
        for (jobP = toJob->forw; (jobP != froJob->forw); jobP = jobP->forw) {
            if (jobP->qPtr != toJob->qPtr)
                continue;
            if ((ordUser == TRUE) && (jobP->uPtr != toJob->uPtr))
                continue;
            offJobList (tmpJobP, listno);

            listInsertEntryBefore((LIST_T *)jDataList[listno],
                                  (LIST_ENTRY_T *)jobP,
                                  (LIST_ENTRY_T *)tmpJobP);
            tmpJobP = jobP;
        }
        offJobList(froJob, listno);
        listInsertEntryAfter((LIST_T *)jDataList[listno],
                             (LIST_ENTRY_T *)oldJobP,
                             (LIST_ENTRY_T *)froJob);
    }
    return;

}

void
initJobIdHT(void)
{
    h_initTab_ (&jobIdHT, 50);
}

void
removeJob(LS_LONG_INT jobId)
{   struct jData *zp, *jp;
    struct jgTreeNode *node;

    if ((jp = getJobData(jobId)) && jp->nodeType != JGRP_NODE_ARRAY) {
        while ((zp = getZombieJob(jp->jobId)) != NULL) {
            closeSbdConnect4ZombieJob(zp);
            offList((struct listEntry *) zp);
            freeJData(zp);
        }

        remvMemb(&jobIdHT, jp->jobId);
        offJobList (jp, FJL);
        numRemoveJobs ++;
        if (mSchedStage != M_STAGE_REPLAY) {

            log_jobclean(jp);
        }


        if ((node = jp->jgrpNode)) {
            if (node->nodeType == JGRP_NODE_ARRAY) {
                offArray(jp);
                if (ARRAY_DATA(node)->counts[JGRP_COUNT_NJOBS] <= 0) {
                    if (mSchedStage != M_STAGE_REPLAY)
                        log_jobclean(ARRAY_DATA(node)->jobArray);
                    remvMemb(&jobIdHT, ARRAY_DATA(node)->jobArray->jobId);
                    freeJData(ARRAY_DATA(node)->jobArray);
                    treeFree(treeClip(node));
                    rmLogJobInfo_(jp, TRUE);
                }
            }
            else {
                rmLogJobInfo_(jp, TRUE);
                treeFree(treeClip(node));
            }
        }
        else
            rmLogJobInfo_(jp, TRUE);
        freeJData (jp);
    }
}


void
inPendJobList (struct jData *job, int listno, time_t requeueTime)
{
    static char fname[] = "inPendJobList";
    struct jData      *jp;
    time_t compareTime;

    struct jData *lastJob = NULL;

    if (requeueTime && (listno == PJL) &&
        (requeueToBottom || (job->pendEvent.sigDel & DEL_ACTION_REQUEUE))) {

        if (logclass & (LC_TRACE | LC_PEND)) {
            ls_syslog(LOG_DEBUG, "%s: put the requeued or migrated job the bottom of the queue.", fname);
        }
        compareTime = requeueTime;
    } else {
        compareTime = job->shared->jobBill.submitTime;
    }


    if (job->pendEvent.sigDel & DEL_ACTION_REQUEUE) {
        job->pendEvent.sigDel &= ~DEL_ACTION_REQUEUE;
    }


    for (jp = jDataList[listno]->forw;
         jp != jDataList[listno]; jp = jp->forw)
    {

        if (job->qPtr->priority < jp->qPtr->priority) {
            break;
        } else if (job->qPtr->priority == jp->qPtr->priority) {

            if (!jobsOnSameSegment(job, jp, jDataList[listno])) {

                if (job->qPtr->qAttrib & Q_ATTRIB_FS || jp->qPtr->qAttrib & Q_ATTRIB_FS) {
                    /*When a FS queue has the same priority with others, who is in the front of lsb.queue, 
                     *whose jobs are in the front of jobList.
                     */
                    if (job->qPtr->posId > jp->qPtr->posId) {
                        break;
                    } else {
                        continue;
                    }
                } else {
                    if ((job->shared->jobBill.submitTime
                        > jp->shared->jobBill.submitTime)
                        && lastJob == NULL) {
                        lastJob = jp;
                    } else if ((job->shared->jobBill.submitTime
                                == jp->shared->jobBill.submitTime)
                                && lastJob == NULL) {
                        if (job->jobId == -1 || jp->jobId == -1) {
                            lastJob = jp;
                        } else if (job->jobId > jp->jobId) {
                            lastJob = jp;
                        }
                    }
                }

                if ((jp->forw != jDataList[listno])
                    && jobsOnSameSegment(jp->forw, job, jDataList[listno])) {

                    lastJob = NULL;
                }
                continue;
            }


            if (job->qPtr->qAttrib & Q_ATTRIB_ENQUE_INTERACTIVE_AHEAD) {

                if ( (job->shared->jobBill.options & SUB_INTERACTIVE)
                     &&
                     !(jp->shared->jobBill.options & SUB_INTERACTIVE))
                    continue;
            }


            if ( job->jobPriority < jp->jobPriority ) {
                break;
            }

            if ( job->jobPriority == jp->jobPriority) {
                if (compareTime > jp->shared->jobBill.submitTime) {

                    break;
                } else if (compareTime == jp->shared->jobBill.submitTime) {

                    if (arraySchedOrder && LSB_ARRAY_IDX(job->jobId))
                        break;

                    if (job->jobId == -1 || jp->jobId == -1) {

                        break;
                    } else if (job->jobId > jp->jobId)
                        break;
                }
            }
            if ((jp->forw != jDataList[listno])
                && jobsOnSameSegment(job, jp, jDataList[listno])
                && !jobsOnSameSegment(job, jp->forw, jDataList[listno])) {


                jp = jp->forw;
                break;
            }
        }
    }


    if (lastJob != NULL) {
        struct jData *nextLastJob;

        if (!jobsOnSameSegment(job, lastJob, jDataList[listno])) {


            for (nextLastJob = lastJob->back;
                 nextLastJob != jDataList[listno];
                 lastJob = nextLastJob, nextLastJob = nextLastJob->back) {

                if (!jobsOnSameSegment(lastJob, nextLastJob, jDataList[listno])){

                    break;
                }
            }
        }

        jp = lastJob;
    }


    listInsertEntryBefore((LIST_T *)jDataList[listno],
                          (LIST_ENTRY_T *)jp,
                          (LIST_ENTRY_T *)job);
    pendJobSlots += job->shared->jobBill.numProcessors;
    if (logclass & (LC_JLIMIT | LC_SCHED)) {
        ls_syslog(LOG_DEBUG, "%s: mbd pendJobSlots updated to <%d> with add"
                             " pend jobId <%d> with plus numProcessors <%d>.",
                  fname, pendJobSlots, job->jobId, job->shared->jobBill.numProcessors);
    }

}

void
offJobList (struct jData *jp, int listno)
{
    listRemoveEntry((LIST_T *)jDataList[listno], (LIST_ENTRY_T *)jp);
    if (listno == PJL) {
        pendJobSlots -= jp->shared->jobBill.numProcessors;
        if (logclass & (LC_JLIMIT | LC_SCHED)) {
            ls_syslog(LOG_DEBUG, "offJobList: mbd pendJobSlots updated to <%d> with remove"
                                 " pend jobId <%d> with minus numProcessors <%d>.",
                      pendJobSlots, jp->jobId, jp->shared->jobBill.numProcessors);
        }

        pendJobSlots = pendJobSlots < 0 ? 0 : pendJobSlots;
    }

}

void
inStartJobList (struct jData *job)
{
    struct jData *jp;


    for (jp = jDataList[SJL]->forw;
         jp != jDataList[SJL]; jp = jp->forw)
    {
        if (job->qPtr->priority < jp->qPtr->priority)
            break;
        else if (job->qPtr->priority == jp->qPtr->priority) {
            if (job->startTime > jp->startTime)
                break;
            else if ((job->startTime == jp->startTime || job->startTime == 0)
                     && (job->jobId > jp->jobId))
                break;
        }
    }


    listInsertEntryBefore((LIST_T *)jDataList[SJL],
                          (LIST_ENTRY_T *)jp,
                          (LIST_ENTRY_T *)job);

}
void
jobInQueueEnd (struct jData *job, struct qData *qp)
{
    int listno;

    if (IS_FINISH (job->jStatus))
        return;
    if (IS_PEND(job->jStatus)) {
        listno = PJLorMJL(job);
    } else {
        listno = SJL;
    }
    offJobList (job, listno);

    free (job->shared->jobBill.queue);
    job->qPtr = qp;
    job->shared->jobBill.queue = safeSave (qp->queue);
    if (IS_PEND (job->jStatus)) {
        if (job->jStatus & JOB_STAT_MIG) {
            if (glMigToPendFlag == TRUE)  {
                inPendJobList (job, PJL, 0);

                job->jStatus &= ~JOB_STAT_MIG;
            } else
                inPendJobList (job, MJL, 0);
        }
        else
            inPendJobList (job, PJL, 0);
    } else
        inStartJobList (job);
    return;
}

struct jData *
getJobData (LS_LONG_INT jobId)
{
    hEnt *ent;

    if (jobId <= 0 || (ent = chekMemb (&jobIdHT, jobId)) == NULL)
        return NULL;
    return (struct jData *) ent->hData;

}

struct jShared *
createSharedRef (struct jShared *shared)
{
    if (shared) {
        shared->numRef++;
    }
    return(shared);
}

void
destroySharedRef(struct jShared *shared)
{
    if (shared) {
        shared->numRef--;
        if (shared->numRef <= 0) {
            if (shared->dptRoot)
                freeDepCond(shared->dptRoot);
            lsbFreeResVal(&shared->resValPtr);
            FREEUP(shared);
        }
    }
}


struct jData *
initJData (struct jShared  *shared)
{
    struct jData *job;

    job = (struct jData *) my_calloc(1, sizeof (struct jData), "initJData");
    job->shared = createSharedRef(shared);
    job->jobId = 0;
    job->userId = -1;
    job->userName = NULL;
    job->uPtr = NULL;
    job->jStatus = JOB_STAT_PEND;
    job->newReason = PEND_JOB_NEW;
    job->oldReason = job->newReason;
    job->subreasons = 0;
    job->reasonTb = NULL;
    job->numReasons = 0;
    job->priority = -1.0;
    job->qPtr = NULL;
    job->hPtr = NULL;
    job->numHostPtr = 0;
    job->numAskedPtr = 0;
    job->askedPtr = NULL;
    job->askedOthPrio = -1;
    job->numCandPtr = 0;
    job->candPtr = NULL;
    job->numExecCandPtr = 0;
    job->execCandPtr = NULL;
    job->usePeerCand = FALSE;
    job->numSlots = 0;
    job->processed = 0;
    job->dispCount = 0;
    job->dispTime = 0;
    job->startTime = 0;
    job->predictedStartTime = 0;
    job->nextSeq = 1;
    job->jobPid = -1;
    job->jobPGid = -1;
    job->cpuTime = 0.0;
    job->endTime = 0;
    job->requeueTime = 0;
    job->retryHist = 0;
    job->pendEvent.notSwitched = FALSE;
    job->pendEvent.sig = SIG_NULL;
    job->pendEvent.sig1 = SIG_NULL;
    job->pendEvent.sig1Flags = 0;
    job->pendEvent.sigDel = FALSE;
    job->pendEvent.notModified = FALSE;
    job->reqHistoryAlloc = 16;
    job->reqHistory = (struct rqHistory *) my_malloc (job->reqHistoryAlloc*sizeof(struct rqHistory), "initJData");
    job->reqHistory[0].host = NULL;
    job->lastDispHost = -1;
    job->requeMode = -1;
    job->numRef = 0;
    job->actPid = 0;
    job->ssuspTime = 0;
    job->lsfRusage = NULL;
    job->newSub   = NULL;
    job->jFlags = 0;
    job->runCount = jobRunTimes;
    job->execUid = -1;
    job->runTime = -1;
    job->updStateTime = -1;
    job->resumeTime = -1;
    job->exitStatus = 0;
    job->execHome = NULL;
    job->execCwd = NULL;
    job->execUsername = NULL;
    job->queuePreCmd = NULL;
    job->queuePostCmd = NULL;
    job->initFailCount = 0;
    job->slotHoldTime = -1;
    job->reserveTime  = 0;
    job->jRusageUpdateTime = 0;
    job->schedHost = NULL;

    memset((char *) &job->runRusage, 0, sizeof(job->runRusage));
    job->numUserGroup = 0;
    job->userGroup = NULL;

    job->execHosts = NULL;
    job->sigValue = SIG_NULL;
    job->port = 0;


    if (pjobSpoolDir != NULL) {
        job->jobSpoolDir = safeSave(pjobSpoolDir);
    } else {
        job->jobSpoolDir = NULL;
    }
    job->jobPriority  = -1;
    job->numEligProc = 0;
    job->rsrcPreemptHPtr = NULL;
    job->numRsrcPreemptHPtr = 0;

    job->numOfGroups = 0;

    job->currentGrp = -1;
    job->reservedGrp = -1;
    job->groupCands = NULL;
    job->inEligibleGroups = NULL;

    return (job);
}



void
assignLoad(float *loadSched, float *loadStop, struct qData *qp,
           struct hData *hp)
{
    int i;

    for (i = 0; i < allLsInfo->numIndx; i++) {
        if (allLsInfo->resTable[i].orderType == INCR) {

            if (hp->hStatus & HOST_STAT_REMOTE) {
                loadStop[i]  = qp->loadStop[i];
                loadSched[i] = qp->loadSched[i];
            } else {
                loadStop[i]  = qp->loadStop[i] < hp->loadStop[i] ?
                    qp->loadStop[i] : hp->loadStop[i];
                loadSched[i] = qp->loadSched[i] < hp->loadSched[i] ?
                    qp->loadSched[i] : hp->loadSched[i];
            }
        } else {
            if (hp->hStatus & HOST_STAT_REMOTE) {
                loadStop[i]  = qp->loadStop[i];
                loadSched[i] = qp->loadSched[i];
            } else {
                loadStop[i]  = qp->loadStop[i] > hp->loadStop[i] ?
                    qp->loadStop[i] : hp->loadStop[i];
                loadSched[i] = qp->loadSched[i] > hp->loadSched[i] ?
                    qp->loadSched[i] : hp->loadSched[i];
            }
        }
    }
}


static void
accumulateRU (struct jData *job, struct statusReq *statusReq)
{
    float cpuTime = statusReq->lsfRusage.ru_utime +
        statusReq->lsfRusage.ru_stime;

    if (job->cpuTime >= 0.0 && cpuTime > MIN_CPU_TIME)
        job->cpuTime += cpuTime;

    if (!job->lsfRusage)
        return;

    job->lsfRusage->ru_utime = acumulateValue (statusReq->lsfRusage.ru_utime,
                                               job->lsfRusage->ru_utime);
    job->lsfRusage->ru_stime = acumulateValue (statusReq->lsfRusage.ru_stime,
                                               job->lsfRusage->ru_stime);
    if (statusReq->lsfRusage.ru_maxrss > job->lsfRusage->ru_maxrss)
        job->lsfRusage->ru_maxrss = statusReq->lsfRusage.ru_maxrss;
    if (statusReq->lsfRusage.ru_ixrss > job->lsfRusage->ru_ixrss)
        job->lsfRusage->ru_ixrss = statusReq->lsfRusage.ru_ixrss;
    if (statusReq->lsfRusage.ru_ismrss > job->lsfRusage->ru_ismrss)
        job->lsfRusage->ru_ismrss = statusReq->lsfRusage.ru_ismrss;
    if (statusReq->lsfRusage.ru_idrss > job->lsfRusage->ru_idrss)
        job->lsfRusage->ru_idrss = statusReq->lsfRusage.ru_idrss;
    if (statusReq->lsfRusage.ru_isrss > job->lsfRusage->ru_isrss)
        job->lsfRusage->ru_isrss = statusReq->lsfRusage.ru_isrss;
    job->lsfRusage->ru_minflt = acumulateValue (statusReq->lsfRusage.ru_minflt,
                                                job->lsfRusage->ru_minflt);
    job->lsfRusage->ru_majflt = acumulateValue (statusReq->lsfRusage.ru_majflt,
                                                job->lsfRusage->ru_majflt);
    job->lsfRusage->ru_nswap = acumulateValue (statusReq->lsfRusage.ru_nswap,
                                               job->lsfRusage->ru_nswap);
    job->lsfRusage->ru_inblock = acumulateValue
        (statusReq->lsfRusage.ru_inblock, job->lsfRusage->ru_inblock);
    job->lsfRusage->ru_oublock = acumulateValue
        (statusReq->lsfRusage.ru_oublock, job->lsfRusage->ru_oublock);
    job->lsfRusage->ru_ioch = acumulateValue (statusReq->lsfRusage.ru_ioch,
                                              job->lsfRusage->ru_ioch);
    job->lsfRusage->ru_msgsnd = acumulateValue (statusReq->lsfRusage.ru_msgsnd,
                                                job->lsfRusage->ru_msgsnd);
    job->lsfRusage->ru_msgrcv = acumulateValue (statusReq->lsfRusage.ru_msgrcv,
                                                job->lsfRusage->ru_msgrcv);
    job->lsfRusage->ru_nsignals = acumulateValue
        (statusReq->lsfRusage.ru_nsignals, job->lsfRusage->ru_nsignals);
    job->lsfRusage->ru_nvcsw = acumulateValue (statusReq->lsfRusage.ru_nvcsw,
                                               job->lsfRusage->ru_nvcsw);
    job->lsfRusage->ru_nivcsw = acumulateValue (statusReq->lsfRusage.ru_nivcsw,
                                                job->lsfRusage->ru_nivcsw);
    job->lsfRusage->ru_exutime = acumulateValue
        (statusReq->lsfRusage.ru_exutime, job->lsfRusage->ru_exutime);

}


static double
acumulateValue (double statusValue, double jobValue)
{

    if (statusValue > 0.0 && jobValue > 0.0)
        jobValue += statusValue;
    else if (statusValue >= 0.0 && jobValue < 0.0)
        jobValue = statusValue;
    return (jobValue);

}

int
resigJobs(int *resignal)
{
    static char          fname[] = "resigJobs()";
    static char          first = TRUE;
    int                  sigcnt = 0;
    int                  sVsigcnt;
    int                  mxsigcnt = 5;
    int                  list;
    int                  retVal = TRUE;
    struct hData         *hPtr;
    struct jData         *jpbw;
    struct jData         *next;
    static LS_BITSET_T   *processedHosts;

    if (logclass & (LC_TRACE | LC_SIGNAL)) {
        ls_syslog (LOG_DEBUG1, "\
%s: Entering this routine... eP=%d",
                   fname, eventPending);
    }

    sigcnt = 0;
    mxsigcnt = 5;

    for (list = 0; list < NJLIST; list++) {

        if (list != MJL && list != PJL)
            continue;

        /* For pending and migrating jobs only...
         */
        for (jpbw = jDataList[list]->back;
             jpbw != jDataList[list];
             jpbw = next) {

            INC_CNT(PROF_CNT_firstLoopresigJobs);

            next = jpbw->back;

            resigJobs1 (jpbw, &sigcnt);

            if (sigcnt > 100)
                return retVal;
        }
    }

    if (first) {
        processedHosts = simpleSetCreate(numofhosts() + 1,
                                         (char *)__func__);
        if (processedHosts == NULL) {
            ls_syslog(LOG_ERR, "\
%s: simpleSetCreate() failed %s",
                      fname, setPerror(bitseterrno));
            mbdDie(MASTER_MEM);
        }
        first = FALSE;
    }

    sigcnt = 0;
    if (setClear(processedHosts) < 0) {
        ls_syslog(LOG_ERR, "%s: setClear() failed", fname);
        return retVal;
    }

    for (list = 0; list <= NJLIST && sigcnt < mxsigcnt; list++) {
        if (list != SJL && list != FJL && list != ZJL)
            continue;
        /* For all jobs in SJL, FJL and ZJL
         */
        for (jpbw = jDataList[list]->back;
             jpbw != jDataList[list] && sigcnt < mxsigcnt;
             jpbw = next) {

            INC_CNT(PROF_CNT_jobLoopresigJobs);

            next = jpbw->back;

            if (!jpbw->hPtr)
                continue;

            hPtr = jpbw->hPtr[0];

            if (setIsMember(processedHosts, (void *)&hPtr->hostId))
                continue;

            if (hPtr->hStatus & HOST_STAT_REMOTE)
                continue;

            if (UNREACHABLE (hPtr->hStatus)) {
                continue;
            }

            if (hPtr->flags & HOST_LOST_FOUND)
                continue;

            sVsigcnt = sigcnt;
            if (resigJobs1 (jpbw, &sigcnt) < 0) {
                if (logclass & (LC_TRACE | LC_SIGNAL))
                    ls_syslog (LOG_DEBUG2, "\
%s: Signaling job %s failed; sigcnt= %d host=%s",
                               fname, lsb_jobid2str(jpbw->jobId),
                               sigcnt, hPtr->host);
                return retVal;
            }

            if (sigcnt > sVsigcnt)
                setAddElement(processedHosts, (void *)&hPtr->hostId);
        }
    }

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList && sigcnt < mxsigcnt;
         hPtr = hPtr->back) {

        INC_CNT(PROF_CNT_hostLoopresigJobs);

        if (logclass & (LC_SIGNAL))
            ls_syslog (LOG_DEBUG2, "%s: host=%s", fname, hPtr->host);

        if (hPtr->hStatus & HOST_STAT_REMOTE)
            continue;

        if (UNREACHABLE (hPtr->hStatus)) {
            continue;
        }

        if (setIsMember(processedHosts, (void *)&hPtr->hostId))
            continue;

        TIMEIT(1, sndJobMsgs (hPtr, &sigcnt), "sndJobMsgs");
    }

    for (list = 0; list <= NJLIST && sigcnt < mxsigcnt; list++) {

        if (list != SJL && list != FJL && list != ZJL)
            continue;
        for (jpbw = jDataList[list]->back;
             jpbw != jDataList[list] && sigcnt < mxsigcnt;
             jpbw = next) {

            INC_CNT(PROF_CNT_nqsLoopresigJobs);

            next = jpbw->back;
        }
    }

    retVal = FALSE;
    *resignal = FALSE;

    if (sigcnt < mxsigcnt) {
        for (list = 0; list <= NJLIST; list++) {
            if (list != SJL && list != FJL && list != ZJL)
                continue;
            for (jpbw = jDataList[list]->back; jpbw != jDataList[list];
                 jpbw = next) {
                struct bucket *bucket = NULL;

                INC_CNT(PROF_CNT_fourthLoopresigJobs);

                next = jpbw->back;

                if (jpbw->hPtr != NULL
                    && (hPtr = jpbw->hPtr[0]) != NULL)
                    bucket = hPtr->msgq[MSG_STAT_QUEUED];
                else
                    bucket = NULL;

                if (jpbw->pendEvent.notSwitched
                    || jpbw->pendEvent.sig != SIG_NULL
                    || jpbw->pendEvent.sig1 != SIG_NULL
                    || jpbw->pendEvent.notModified
                    || (bucket && bucket->forw != bucket)) {
                    if (logclass & (LC_SIGNAL))
                        ls_syslog (LOG_DEBUG2, "%s: Job %s has pending event; sigcnt=%d", fname, lsb_jobid2str(jpbw->jobId), sigcnt);
                    return retVal;
                }
            }
        }
        eventPending = FALSE;
        retVal = FALSE;
        *resignal = FALSE;
        if (logclass & (LC_TRACE | LC_SIGNAL))
            ls_syslog (LOG_DEBUG1, "%s: no pending event now", fname);
    }
    return retVal;

}

static int
resigJobs1 (struct jData *jpbw, int *sigcnt)
{
    static char fname[] = "resigJobs1";
    sbdReplyType reply;

    if (logclass & (LC_SIGNAL))
        ls_syslog (LOG_DEBUG2, "%s: check job %s", fname, lsb_jobid2str(jpbw->jobId));

    INC_CNT(PROF_CNT_resigJobs1);

    if (jpbw->pendEvent.notSwitched) {
        if (logclass & (LC_TRACE | LC_SIGNAL))
            ls_syslog (LOG_DEBUG2, "%s: switch job %s", fname, lsb_jobid2str(jpbw->jobId));
        if (IS_PEND (jpbw->jStatus))
            reply = ERR_NO_ERROR;
        else {
            reply = switch_job (jpbw, TRUE);
        }
        (*sigcnt)++;
        if (reply == ERR_NO_ERROR || reply == ERR_NO_JOB
            || reply == ERR_JOB_FINISH) {
            jpbw->pendEvent.notSwitched = 0;
            if (!IS_PEND (jpbw->jStatus))
                return 0;
        } else {
            if (!IS_PEND (jpbw->jStatus)
                && UNREACHABLE(jpbw->hPtr[0]->hStatus))
                return -1;
        }
    }
    if (jpbw->pendEvent.notModified) {
        if (IS_PEND (jpbw->jStatus)) {
            reply = ERR_NO_ERROR;
        }
        else {
            reply = switch_job (jpbw, FALSE);
        }
        (*sigcnt)++;
        if (reply == ERR_NO_ERROR || reply == ERR_NO_JOB
            || reply == ERR_JOB_FINISH) {
            jpbw->pendEvent.notModified = 0;
            if (!IS_PEND (jpbw->jStatus))
                return 0;
        } else {
            if (!IS_PEND (jpbw->jStatus)
                && UNREACHABLE(jpbw->hPtr[0]->hStatus))
                return -1;
        }
    }

    if (jpbw->pendEvent.sig != SIG_NULL) {
        if (logclass & (LC_TRACE | LC_SIGNAL))
            ls_syslog (LOG_DEBUG2, "%s: signal job %s", fname, lsb_jobid2str(jpbw->jobId));
        if (IS_PEND (jpbw->jStatus)) {
            sigPFjob (jpbw, jpbw->pendEvent.sig, 0, LOG_IT);
            reply = ERR_NO_ERROR;
        } else {
            if  ((((MASK_STATUS(jpbw->jStatus))==JOB_STAT_RUN)
                  && (jpbw->pendEvent.sig == SIG_RESUME_USER))
                 ||  (((MASK_STATUS(jpbw->jStatus))==JOB_STAT_SSUSP)
                      && (jpbw->newReason & SUSP_USER_STOP)
                      && (jpbw->pendEvent.sig == SIG_SUSP_USER))) {


                reply = ERR_NO_ERROR;
            } else {
                reply = sigStartedJob (jpbw, jpbw->pendEvent.sig, 0, 0);
            }
        }
        (*sigcnt)++;
        if (reply == ERR_NO_ERROR || reply == ERR_NO_JOB) {
            jpbw->pendEvent.sig = SIG_NULL;
            if (!IS_PEND (jpbw->jStatus))
                return 0;
        } else {
            if (!IS_PEND (jpbw->jStatus)
                && UNREACHABLE(jpbw->hPtr[0]->hStatus))
                return -1;
        }
    }

    if (jpbw->pendEvent.sig1 != SIG_NULL) {
        if (logclass & (LC_TRACE | LC_SIGNAL))
            ls_syslog (LOG_DEBUG2, "%s: chkpnt job %s", fname, lsb_jobid2str(jpbw->jobId));
        if (IS_PEND (jpbw->jStatus)) {
            sigPFjob (jpbw, jpbw->pendEvent.sig1,
                      jpbw->chkpntPeriod, LOG_IT);
            reply = ERR_NO_ERROR;
        }
        else {
            reply = sigStartedJob (jpbw, jpbw->pendEvent.sig1,
                                   jpbw->chkpntPeriod,
                                   jpbw->pendEvent.sig1Flags);
        }
        (*sigcnt)++;
        if (reply == ERR_NO_ERROR || reply == ERR_NO_JOB) {
            jpbw->pendEvent.sig1 = SIG_NULL;
            if (!IS_PEND (jpbw->jStatus))
                return 0;
        } else {
            if (!IS_PEND (jpbw->jStatus)
                && UNREACHABLE(jpbw->hPtr[0]->hStatus))
                return -1;
        }
    }
    return 0;
}

int
modifyJob (struct modifyReq *req, struct submitMbdReply *reply,
           struct lsfAuth *auth)
{
    struct jData *jpbw, *jArray;
    int    numJobIds = 0;
    LS_LONG_INT *jobIdList = NULL;
    int    returnErr, successfulOnce = FALSE;
    int    i;
    int    uid=auth->uid;
    char   userName[MAXLSFNAMELEN];

    if ((returnErr = getJobIdList(req->jobIdStr, &numJobIds, &jobIdList))
        != LSBE_NO_ERROR) {
        FREEUP(jobIdList);
        return(returnErr);
    }

    strcpy(userName, auth->lsfUserName);
    for (i = 0; i < numJobIds; i++) {

        if ((jpbw = getJobData (jobIdList[i])) == NULL) {
            if (reply)
                reply->jobId = req->jobId;
            returnErr = LSBE_NO_JOB;
            continue;
        }


        if (LSB_ARRAY_IDX(jpbw->jobId) == 0 &&
            jpbw->nodeType != JGRP_NODE_ARRAY) {

            if ((returnErr = modifyAJob(req, reply, auth, jpbw)) ==
                LSBE_NO_ERROR)
                successfulOnce = TRUE;
            continue;
        }

        if ((jArray = getJobData(LSB_ARRAY_JOBID(jpbw->jobId))) == NULL) {
            if (reply)
                reply->jobId = LSB_ARRAY_JOBID(jpbw->jobId);

            returnErr = LSBE_ARRAY_NULL;
            continue;
        }



        if (req->submitReq.options & SUB_JOB_NAME) {
            char   *sp, *jobName;
            int    newMaxJLimit;

            if (LSB_ARRAY_IDX(jpbw->jobId) == 0
                && req->submitReq.jobName
                && req->submitReq.jobName[0] == '%'
                && isint_(req->submitReq.jobName+1)) {

                if ((newMaxJLimit = atoi(req->submitReq.jobName + 1)) < 0) {
                    returnErr = LSBE_MOD_JOB_NAME;
                    continue;
                }

                ARRAY_DATA(jArray->jgrpNode)->maxJLimit = newMaxJLimit;


                if ((sp = strchr(jArray->shared->jobBill.jobName, ']'))) {
                    *(sp+1) = '\0';
                    jobName = (char *)my_malloc(
                        strlen(jArray->shared->jobBill.jobName) +
                        strlen(req->submitReq.jobName) + 4, "modifyJob");
                    sprintf(jobName, "%s%s",
                            jArray->shared->jobBill.jobName,
                            req->submitReq.jobName);
                    FREEUP(jArray->shared->jobBill.jobName);
                    jArray->shared->jobBill.jobName = jobName;
                }

                if ((sp = strchr(jArray->jgrpNode->name, ']'))) {
                    *(sp+1) = '\0';
                    jobName = (char *)my_malloc(
                        strlen(jArray->jgrpNode->name) +
                        strlen(req->submitReq.jobName) + 4, "modifyJob");
                    sprintf(jobName, "%s%s",
                            jArray->jgrpNode->name,
                            req->submitReq.jobName);
                    FREEUP(jArray->jgrpNode->name);
                    jArray->jgrpNode->name = jobName;
                }
            } else {
                returnErr = LSBE_MOD_JOB_NAME;
                continue;
            }

        }

        if (LSB_ARRAY_IDX(jpbw->jobId) != 0) {

            localizeJobElement(jpbw);
            if ((returnErr = modifyAJob(req, reply, auth, jpbw)) ==
                LSBE_NO_ERROR)
                successfulOnce = TRUE;

            auth->uid = uid;
            strcpy(auth->lsfUserName, userName);
            continue;
        }

        {

            struct jgTreeNode  *jgnode;
            struct jarray      *jarray;

            jgnode = jArray->jgrpNode;
            jarray = ARRAY_DATA(jgnode);

            if (jarray->counts[JGRP_COUNT_NJOBS]
                ==  jarray->counts[JGRP_COUNT_NDONE]
                + jarray->counts[JGRP_COUNT_NEXIT]) {

                FREEUP(jobIdList);
                return(LSBE_JOB_FINISH);
            }
        }

        for (jpbw = jArray->nextJob; jpbw; jpbw = jpbw->nextJob) {
            if (jpbw->shared != jArray->shared
                || IS_START(jpbw->jStatus))
                break;
        }

        if (jpbw) {

            if (jArray->shared->numRef > 1) {
                localizeJobArray(jArray);
            }
        }


        if(req->submitReq.options & SUB_MODIFY_ONCE){
            localizeJobArray(jArray);
        }


        for (jpbw = jArray->nextJob; jpbw; jpbw = jpbw->nextJob) {
            if ((returnErr = modifyAJob(req, reply, auth, jpbw)) ==
                LSBE_NO_ERROR)
                successfulOnce = TRUE;

            auth->uid = uid;
            strcpy(auth->lsfUserName, userName);
        }


        if (jArray->shared->numRef == 1
            && successfulOnce == TRUE) {
            struct jData        *jPtr;
            struct jgTreeNode   *jnode;
            struct jarray       *jarray;
            int                 numPend;

            jnode  = jArray->jgrpNode;
            jarray = ARRAY_DATA(jnode);


            numPend = jarray->counts[JGRP_COUNT_PEND];

            for (jPtr = jArray->nextJob;
                 jPtr != NULL;
                 jPtr = jPtr->nextJob) {


                if (IS_PEND(jPtr->jStatus)) {
                    break;
                }


                if (IS_START(jPtr->jStatus)
                    && numPend == 0) {
                    break;
                }
            }


            if (jPtr != NULL) {

                freeSubmitReq(&(jArray->shared->jobBill));
                copyJobBill(&(jPtr->shared->jobBill),
                            &(jArray->shared->jobBill),
                            FALSE);


                if (req->submitReq.options & SUB_QUEUE) {
                    FREEUP(jArray->shared->jobBill.queue);
                    jArray->shared->jobBill.queue
                        = safeSave(jPtr->qPtr->queue);
                    jArray->qPtr = jPtr->qPtr;
                }
            }

        } else if (jArray->shared->numRef > 1
                   && successfulOnce == TRUE) {

            if (req->submitReq.options & SUB_QUEUE) {
                jArray->qPtr = jArray->nextJob->qPtr;
            }
        }


        if (successfulOnce)
            returnErr = LSBE_NO_ERROR;


        if ((req->submitReq.options2 & SUB2_JOB_PRIORITY)
            && (returnErr == LSBE_NO_ERROR)) {
            modifyJobPriority(jArray, req->submitReq.userPriority);
        }
    }

    if (mSchedStage != M_STAGE_REPLAY && successfulOnce) {
        log_modifyjob(req, auth);
    }
    FREEUP(jobIdList);
    return (returnErr);
}

static float
normalCpuLimit (struct jData *job, struct submitReq subReq, int *retLimit)
{
    float *cpuFactor, cpuLimit;
    char  *spec;

    cpuLimit = subReq.rLimits[LSF_RLIMIT_CPU];

    *retLimit = cpuLimit;
    if (cpuLimit <= 0)
        return (LSBE_NO_ERROR);

    if (subReq.options & SUB_HOST_SPEC)
        spec = subReq.hostSpec;
    else if (job->qPtr->defaultHostSpec != NULL)
        spec = job->qPtr->defaultHostSpec;
    else if (defaultHostSpec != NULL)
        spec =  defaultHostSpec;
    else
        spec = subReq.fromHost;

    if ((cpuFactor = getModelFactor (spec)) == NULL)
        if ((cpuFactor = getHostFactor (spec)) == NULL)
            return (LSBE_BAD_HOST_SPEC);

    if (*cpuFactor <= 0) {
        return (LSBE_BAD_LIMIT);
    }
    if (cpuLimit > (INFINIT_INT /(*cpuFactor))) {
        return (LSBE_BAD_LIMIT);
    }
    if ((cpuLimit *= *cpuFactor) < 1)
        cpuLimit = 1;
    else
        cpuLimit += 0.5;

    *retLimit = cpuLimit;
    return LSBE_NO_ERROR;
}



static int
modifyAJob (struct modifyReq *req, struct submitMbdReply *reply,
            struct lsfAuth *auth, struct jData *jpbw)
{
    struct jData *job;
    struct submitReq *newReq;
    int returnErr;



    if (IS_FINISH (jpbw->jStatus))
        return (LSBE_JOB_FINISH);

    if ((auth->uid != 0) && !jgrpPermitOk(auth, jpbw->jgrpNode) &&
        !isAuthQueAd (jpbw->qPtr, auth))
        return(LSBE_PERMISSION);


    if ((req->submitReq.options & SUB_JOB_NAME)
        && strchr(req->submitReq.jobName, '['))
        return(LSBE_BAD_JOB);



    if (IS_START(jpbw->jStatus)
        && (!((((req->submitReq.options & ~SUB_MODIFY) == SUB_RES_REQ)
               && (req->delOptions == 0))
              || ((req->delOptions == SUB_RES_REQ)
                  && (req->submitReq.options & ~SUB_MODIFY) == 0))
            && (((lsbModifyAllJobs != TRUE)
                 && (mSchedStage != M_STAGE_REPLAY)
                 && (req->submitReq.options2 & SUB2_MODIFY_RUN_JOB))
                || !(req->submitReq.options2 & SUB2_MODIFY_RUN_JOB)))) {
        return (LSBE_JOB_MODIFY);
    }


    if (IS_START(jpbw->jStatus)
        && (req->submitReq.options2 & SUB2_MODIFY_RUN_JOB)
        && (req->submitReq.options2 & SUB2_MODIFY_PEND_JOB)) {
        return (LSBE_MOD_MIX_OPTS);
    }

    if (IS_START(jpbw->jStatus) && (jpbw->shared->jobBill.options & SUB_MODIFY_ONCE)
        && (req->submitReq.options & SUB_MODIFY_ONCE))
        return (LSBE_JOB_MODIFY_USED);

    if (IS_START(jpbw->jStatus) && jpbw->newSub != NULL) {

        if ((req->submitReq.options & SUB_MODIFY_ONCE) &&
            !(jpbw->newSub->options & SUB_MODIFY_ONCE))
            return (LSBE_JOB_MODIFY_ONCE);
        if (!(req->submitReq.options & SUB_MODIFY_ONCE) &&
            (jpbw->newSub->options & SUB_MODIFY_ONCE))
            return (LSBE_JOB_MODIFY_ONCE);
    }
    if (IS_PEND (jpbw->jStatus) && (jpbw->shared->jobBill.options & SUB_MODIFY_ONCE)
        && !(req->submitReq.options & SUB_MODIFY_ONCE))
        return (LSBE_JOB_MODIFY_ONCE);

    if (IS_START(jpbw->jStatus) &&
        (req->submitReq.rLimits[LSF_RLIMIT_CPU] != DEFAULT_RLIMIT) &&
        ((daemonParams[LSB_JOB_CPULIMIT].paramValue == NULL) ||
         strcasecmp(daemonParams[LSB_JOB_CPULIMIT].paramValue, "y"))) {

        int limit, ret;

        ret=normalCpuLimit(jpbw, req->submitReq, &limit);
        if (ret != LSBE_NO_ERROR)
            return ret;
        if (limit != jpbw->shared->jobBill.rLimits[LSF_RLIMIT_CPU])
            return (LSBE_MOD_CPULIMIT);
    }



    if (IS_START(jpbw->jStatus) &&
        (req->submitReq.rLimits[LSF_RLIMIT_RSS] != DEFAULT_RLIMIT ) &&
        (req->submitReq.rLimits[LSF_RLIMIT_RSS] !=
         jpbw->shared->jobBill.rLimits[LSF_RLIMIT_RSS]) &&
        ((daemonParams[LSB_JOB_MEMLIMIT].paramValue == NULL) ||
         strcasecmp(daemonParams[LSB_JOB_MEMLIMIT].paramValue, "y"))){
        return (LSBE_MOD_MEMLIMIT);
    }

    if (IS_START(jpbw->jStatus) && (req->submitReq.options & SUB_ERR_FILE)
        && !(jpbw->shared->jobBill.options & SUB_ERR_FILE))
    {
        return (LSBE_MOD_ERRFILE);
    }

    if ((mSchedStage != M_STAGE_REPLAY) &&
        (req->submitReq.options & SUB_CHKPNT_DIR)) {

        char buf[32];
        sprintf(buf, "/%s", lsb_jobidinstr(jpbw->jobId));

        strcat(req->submitReq.chkpntDir, buf);
    }

    newReq = getMergeSubReq (jpbw, req, &returnErr);

    if (newReq == NULL)
        return (returnErr);

    job = initJData((struct jShared *) my_calloc(1, sizeof(struct jShared),
                                                 "modifyJob"));
    job->jobId = jpbw->jobId;
    if ( isAuthManager(auth)) {
        setClusterAdmin(TRUE);
    } else {
        setClusterAdmin(FALSE);
    }
    auth->uid = jpbw->userId;
    strcpy (auth->lsfUserName, jpbw->userName);
    if ((returnErr = checkJobParams (job, newReq, reply, auth))
        != LSBE_NO_ERROR) {
        if (returnErr == LSBE_NO_JOB  &&
            !(req->submitReq.options & SUB_DEPEND_COND) &&
            job->shared->dptRoot == NULL) {

            job->shared->dptRoot = jpbw->shared->dptRoot;
            jpbw->shared->dptRoot = NULL;
        }
        else {
            freeNewJob (job);
            freeSubmitReq (newReq);
            FREEUP (newReq);
            return (returnErr);
        }
    }

    copyJobBill (newReq, &job->shared->jobBill, FALSE);
    handleJParameters (jpbw, job, &(req->submitReq), FALSE, req->delOptions,
                       req->delOptions2);

    job->schedHost = safeSave (jpbw->schedHost);
    if (jpbw->jgrpNode && (LSB_ARRAY_IDX(jpbw->jobId) == 0) &&
        ((req->submitReq.options & SUB_JOB_NAME) ||
         (newReq->options2 & SUB2_MODIFY_CMD))) {
        treeClip(jpbw->jgrpNode);
        treeFree(jpbw->jgrpNode);
        putOntoTree(jpbw, JOB_NEW);
    }
    else {


        jpbw->runCount = 1;
    }

    job->jobId = jpbw->jobId;
    freeJData (job);
    freeSubmitReq (newReq);
    FREEUP (newReq);
    if (reply)
        reply->jobId = jpbw->jobId;

    if (IS_PEND(jpbw->jStatus))
        setJobPendReason(jpbw, PEND_JOB_MODIFY);
    return (LSBE_NO_ERROR);
}


static struct submitReq *
getMergeSubReq (struct jData *jpbw, struct modifyReq *req, int *errorCode)
{
    struct submitReq *tempSub, *oldReq;
    int returnCode;
    char newCmdArgs[2*MAXLINELEN];
    struct passwd pwUser;

    memset(&pwUser, 0, sizeof(pwUser));
    pwUser.pw_name = jpbw->userName;
    pwUser.pw_uid = jpbw->userId;

    tempSub = (struct submitReq *) my_malloc (sizeof (struct submitReq),
                                              "getMergeSubReq");
    initSubmitReq(tempSub);
    if (IS_PEND (jpbw->jStatus)) {
        oldReq = &jpbw->shared->jobBill;
    } else {
        if (jpbw->newSub)
            oldReq = jpbw->newSub;
        else
            oldReq = &jpbw->shared->jobBill;
    }

    returnCode = mergeSubReq (tempSub, oldReq, req, &pwUser);

    if (req->submitReq.options2 & SUB2_JOB_CMD_SPOOL) {


        if (replace1stCmd_(req->submitReq.command,
                           req->submitReq.commandSpool,
                           newCmdArgs, sizeof(newCmdArgs)) < 0) {
            returnCode = LSBE_MBATCHD;
            goto cleanup;
        }

        if (replaceJobInfoFile(oldReq->jobFile, newCmdArgs,
                               (jpbw->qPtr) ? jpbw->qPtr->jobStarter : NULL,
                               0) < 0) {
            returnCode = LSBE_MBATCHD;
            goto cleanup;
        } else {
            tempSub->options2 |= SUB2_JOB_CMD_SPOOL;
            tempSub->command  = safeSave(req->submitReq.command);
            tempSub->commandSpool  = safeSave(req->submitReq.commandSpool);
        }

        if (oldReq->options2 & SUB2_JOB_CMD_SPOOL) {

            childRemoveSpoolFile(oldReq->commandSpool,
                                 FORK_REMOVE_SPOOL_FILE, &pwUser);
        }
    } else if (req->delOptions2 & SUB2_JOB_CMD_SPOOL) {

        if (oldReq->options2 & SUB2_JOB_CMD_SPOOL) {
            if (replaceJobInfoFile(oldReq->jobFile, oldReq->command,
                                   (jpbw->qPtr) ? jpbw->qPtr->jobStarter : NULL,
                                   REPLACE_1ST_CMD_ONLY) < 0) {
                returnCode = LSBE_MBATCHD;
                goto cleanup;
            }


            childRemoveSpoolFile(oldReq->commandSpool,
                                 FORK_REMOVE_SPOOL_FILE, &pwUser);
        }

        tempSub->command  = safeSave(oldReq->command);
        tempSub->commandSpool  = safeSave("");
    } else if (req->submitReq.options2 & SUB2_MODIFY_CMD) {

        if (replaceJobInfoFile(oldReq->jobFile, req->submitReq.command,
                               (jpbw->qPtr) ? jpbw->qPtr->jobStarter : NULL,
                               0) < 0) {
            returnCode = LSBE_MBATCHD;
        } else {
            tempSub->options2 |= SUB2_MODIFY_CMD;
            tempSub->command = safeSave(req->submitReq.command);
            tempSub->commandSpool  = safeSave("");
        }

        if (oldReq->options2 & SUB2_JOB_CMD_SPOOL) {

            childRemoveSpoolFile(oldReq->commandSpool,
                                 FORK_REMOVE_SPOOL_FILE, &pwUser);
        }
    } else {

        if (oldReq->options2 & SUB2_JOB_CMD_SPOOL) {
            tempSub->options2 |= SUB2_JOB_CMD_SPOOL;
        }
        tempSub->command = safeSave(oldReq->command);
        tempSub->commandSpool  = safeSave(oldReq->commandSpool);
    }

cleanup:

    *errorCode = returnCode;
    if (returnCode == 0)
        return (tempSub);
    else {
        freeSubmitReq(tempSub);
        FREEUP (tempSub);
        return (NULL);
    }
}

void
handleJParameters (struct jData *jpbw, struct jData *job, struct submitReq *modReq, int replay, int delOptions, int delOptions2)
{
    static char fname[]="handleJParameters";
    struct submitReq *newSub;
    struct submitReq *subReq = &(job->shared->jobBill);

    struct qData *qfp;
    int oldMaxCpus = 0;
    bool_t chgPriority;
    int    alreadySetNewSub =0;

    if (IS_PEND (jpbw->jStatus) || IS_FINISH(jpbw->jStatus)) {


        if (!(jpbw->shared->jobBill.options & SUB_MODIFY_ONCE) &&
            (modReq->options & SUB_MODIFY_ONCE)) {

            if(!IS_FINISH(jpbw->jStatus)){
                jpbw->jStatus |= JOB_STAT_MODIFY_ONCE;

                newSub = saveOldParameters (jpbw);
                jpbw->newSub = newSub;
            }else{


                setNewSub(jpbw, job, subReq, modReq, delOptions, delOptions2 );
                alreadySetNewSub = 1;
            }
        }
        oldMaxCpus = jpbw->shared->jobBill.maxNumProcessors;

        if ((mSchedStage != M_STAGE_REPLAY) && 
            IS_PEND (jpbw->jStatus) &&
            (jpbw->qPtr != job->qPtr)) {
            updAliveUserInFSTree(job->qPtr, jpbw->userName);
        }

        if (  ! alreadySetNewSub ) {

            freeSubmitReq (&(jpbw->shared->jobBill));
            copyJobBill (subReq, &jpbw->shared->jobBill, FALSE);
        }

        if (jpbw->shared->dptRoot) {
            freeDepCond(jpbw->shared->dptRoot);
            jpbw->shared->dptRoot = NULL;
        }
        if (job->shared->dptRoot) {
            jpbw->shared->dptRoot = job->shared->dptRoot;
            job->shared->dptRoot = NULL;
        }

        lsbFreeResVal(&jpbw->shared->resValPtr);
        jpbw->shared->resValPtr = job->shared->resValPtr;
        job->shared->resValPtr = NULL;

        if (jpbw->numAskedPtr)
            FREEUP (jpbw->askedPtr);
        jpbw->numAskedPtr = job->numAskedPtr;
        jpbw->askedPtr = job->askedPtr;
        jpbw->askedOthPrio = job->askedOthPrio;
        job->askedPtr = NULL;
        job->numAskedPtr = 0;

        jpbw->jFlags &= ~JFLAG_DEPCOND_INVALID;

        if ( delOptions & SUB_CHKPNT_DIR ) {

            FREEUP(jpbw->shared->jobBill.chkpntDir);
            jpbw->shared->jobBill.options  &= ~ SUB_CHKPNT_DIR;
            jpbw->shared->jobBill.options  &= ~ SUB_CHKPNT_PERIOD;
            jpbw->shared->jobBill.options2 &= ~ SUB2_QUEUE_CHKPNT;

            jpbw->shared->jobBill.chkpntDir = safeSave("");
        }
        else {
            if ( modReq->options & SUB_CHKPNT_DIR) {

                FREEUP(jpbw->shared->jobBill.chkpntDir);
                jpbw->shared->jobBill.chkpntDir = safeSave(subReq->chkpntDir);
                if ( subReq->chkpntPeriod > 0 )
                    jpbw->shared->jobBill.chkpntPeriod = subReq->chkpntPeriod;
                jpbw->shared->jobBill.options  |= SUB_CHKPNT_DIR;
                jpbw->shared->jobBill.options  |= SUB_CHKPNT_PERIOD;
                jpbw->shared->jobBill.options2 &= ~ SUB2_QUEUE_CHKPNT;
            }
        }

        if ( delOptions & SUB_RERUNNABLE ) {
            jpbw->shared->jobBill.options  &= ~ SUB_RERUNNABLE;
            jpbw->shared->jobBill.options2 &= ~ SUB2_QUEUE_RERUNNABLE;
        }
        else {
            if ( modReq->options & SUB_RERUNNABLE) {
                jpbw->shared->jobBill.options  |= SUB_RERUNNABLE;
                jpbw->shared->jobBill.options  &=  ~ SUB2_QUEUE_RERUNNABLE;
            }
        }

        if (jpbw->nodeType == JGRP_NODE_ARRAY &&
            jpbw->shared->jobBill.maxNumProcessors != oldMaxCpus) {

            struct jData *jPtr;
            for (jPtr = jpbw->nextJob; jPtr != NULL; jPtr = jPtr->nextJob) {
                if (replay != TRUE && IS_PEND(jPtr->jStatus)) {
                    updSwitchJob (jPtr, jPtr->qPtr, jPtr->qPtr, oldMaxCpus);
                }
            }
        }
        else {
            if (jpbw->qPtr != job->qPtr ||
                jpbw->shared->jobBill.maxNumProcessors != oldMaxCpus) {

                if (JOB_PREEMPT_WAIT(jpbw))
                    freeReservePreemptResources(jpbw);
                if (IS_PEND(jpbw->jStatus)) {
                    cleanCandHosts (jpbw);
                }
                if (IS_PEND(jpbw->jStatus) &&
                    (jpbw->jStatus & JOB_STAT_RESERVE)) {


                    if (logclass & (LC_TRACE| LC_SCHED ))
                        ls_syslog(LOG_DEBUG3, "%s: job <%s> updRes - <%d> slots <%s:%d>", fname, lsb_jobid2str(jpbw->jobId), jpbw->numHostPtr, __FILE__,  __LINE__);

                    freeReserveSlots(jpbw);
                    jpbw->reserveTime = 0;
                }
                qfp = jpbw->qPtr;
                if (jpbw->qPtr != job->qPtr) {
                    jobInQueueEnd (jpbw, job->qPtr);

                    if (!(delOptions & SUB_CHKPNT_DIR) &&
                        !(modReq->options & SUB_CHKPNT_DIR) &&
                        ((jpbw->shared->jobBill.options2 & SUB2_QUEUE_CHKPNT
                          && jpbw->shared->jobBill.options & SUB_CHKPNT_DIR)
                         || (!(jpbw->shared->jobBill.options2 & SUB2_QUEUE_CHKPNT)
                             &&!(jpbw->shared->jobBill.options & SUB_CHKPNT_DIR)))) {

                        struct qData *qp = job->qPtr;


                        jpbw->shared->jobBill.options  &= ~ SUB_CHKPNT_DIR;
                        jpbw->shared->jobBill.options  &= ~ SUB_CHKPNT_PERIOD;
                        jpbw->shared->jobBill.options2 &= ~ SUB2_QUEUE_CHKPNT;
                        jpbw->shared->jobBill.chkpntPeriod = 0;
                        if (jpbw->shared->jobBill.chkpntDir)
                            FREEUP(jpbw->shared->jobBill.chkpntDir);
                        jpbw->shared->jobBill.chkpntDir = safeSave("");


                        if ( qp->qAttrib & Q_ATTRIB_CHKPNT ) {
                            char dir[MAXLINELEN];
                            char jobIdStr[20];

                            jpbw->shared->jobBill.options  |= SUB_CHKPNT_DIR;
                            jpbw->shared->jobBill.options  |= SUB_CHKPNT_PERIOD;
                            jpbw->shared->jobBill.options2 |= SUB2_QUEUE_CHKPNT;
                            strcpy(dir, qp->chkpntDir);
                            sprintf(jobIdStr, "/%s", lsb_jobidinstr(jpbw->jobId));
                            strcat(dir, jobIdStr);
                            if (jpbw->shared->jobBill.chkpntDir)
                                FREEUP(jpbw->shared->jobBill.chkpntDir);
                            jpbw->shared->jobBill.chkpntDir = safeSave(dir);
                            jpbw->shared->jobBill.chkpntPeriod = qp->chkpntPeriod;
                        }
                    }

                    if ( !(delOptions & SUB_RERUNNABLE) &&
                         !(modReq->options & SUB_RERUNNABLE) &&
                         ((jpbw->shared->jobBill.options2 & SUB2_QUEUE_RERUNNABLE &&
                           jpbw->shared->jobBill.options & SUB_RERUNNABLE)
                          ||(!(jpbw->shared->jobBill.options2 & SUB2_QUEUE_RERUNNABLE)
                             && !(jpbw->shared->jobBill.options & SUB_RERUNNABLE) ))) {
                        struct qData *qp = job->qPtr;


                        jpbw->shared->jobBill.options  &= ~ SUB_RERUNNABLE;
                        jpbw->shared->jobBill.options2  &= ~ SUB2_QUEUE_RERUNNABLE;


                        if ( qp->qAttrib & Q_ATTRIB_RERUNNABLE ) {
                            jpbw->shared->jobBill.options  |= SUB_RERUNNABLE;
                            jpbw->shared->jobBill.options2 |= SUB2_QUEUE_RERUNNABLE;
                        }
                    }
                }

                if (replay != TRUE) {
                    updSwitchJob (jpbw, qfp, job->qPtr, oldMaxCpus);
                }
            }
        }



        chgPriority = FALSE;

        if ( delOptions2 & SUB2_JOB_PRIORITY && maxUserPriority > 0) {

            modifyJobPriority(jpbw, maxUserPriority/2);
            chgPriority = TRUE;
        }
        else if ( modReq->options2 & SUB2_JOB_PRIORITY) {
            int error=0;
            if (checkUserPriority(jpbw, modReq->userPriority, &error)) {
                modifyJobPriority(jpbw, modReq->userPriority);
                chgPriority = TRUE;
            }
        }


        if (chgPriority)  {
            if (IS_PEND (jpbw->jStatus)) {
                int list = PJLorMJL(jpbw);
                offJobList(jpbw, list);
                inPendJobList(jpbw, list, 0);
            }
        }
    } else if (IS_START(jpbw->jStatus)
               && !(modReq->options2 & SUB2_MODIFY_RUN_JOB)) {

        lsbFreeResVal(&jpbw->shared->resValPtr);
        jpbw->shared->resValPtr = job->shared->resValPtr;
        job->shared->resValPtr = NULL;
        FREEUP (jpbw->shared->jobBill.resReq);
        jpbw->shared->jobBill.resReq = safeSave(subReq->resReq);

    } else if (IS_START(jpbw->jStatus)
               && ((lsbModifyAllJobs == TRUE) || (mSchedStage == M_STAGE_REPLAY))
               && (modReq->options2 & SUB2_MODIFY_RUN_JOB)
               && !(modReq->options2 & SUB2_MODIFY_PEND_JOB)) {
        if (modReq->options & SUB_MODIFY_ONCE) {
            jpbw->jStatus |= JOB_STAT_MODIFY_ONCE;

            newSub = saveOldParameters (jpbw);
            jpbw->newSub = newSub;
        }
        if ( mSchedStage != M_STAGE_REPLAY ) {
            jpbw->pendEvent.notModified = TRUE;
        }
        eventPending = TRUE;
        freeSubmitReq (&jpbw->shared->jobBill);
        copyJobBill(subReq, &jpbw->shared->jobBill, FALSE);

        if ((modReq->options & SUB_RES_REQ)
            || (delOptions & SUB_RES_REQ)) {
            lsbFreeResVal(&jpbw->shared->resValPtr);
            jpbw->shared->resValPtr = job->shared->resValPtr;
            job->shared->resValPtr = NULL;
        }
    } else {


        setNewSub(jpbw, job, subReq, modReq, delOptions, delOptions2 );
    }
}
static struct submitReq *
saveOldParameters (struct jData *jpbw)
{
    struct submitReq *newSub;

    newSub = (struct submitReq *) my_malloc
        (sizeof (struct submitReq), "saveJParameters");
    copyJobBill (&jpbw->shared->jobBill, newSub, FALSE);
    return (newSub);
}

static int
checkJobPendLimit(struct jData *job, struct lsfAuth *auth, char *pendLimitReason, int jobArraySize) {

    static char fname[] = "checkJobPendLimit";
    struct uData *userData;
    int    totalNumPendJobs = 0;
    LIST_T *list = NULL;
    int    i;

    userData = getUserData(auth->lsfUserName);
    if (userData == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 6511,
                                         "%s: User <%s> is not found by LSF"), /* catgets 6511 */
                  fname, auth->lsfUserName);
        return (LSBE_NO_USER);
    }
    initUserGroup(userData);

    list = (LIST_T *)jDataList[PJL];
    totalNumPendJobs = list->numEnts;
    if(logclass & LC_JLIMIT) {
        ls_syslog(LOG_DEBUG, "%s: debug jDataList[PJL] numEnts <%d>.", fname, totalNumPendJobs);
    }

    // lsb.users MAX_PEND_SLOTS
    if ((userData->numPEND + (jobArraySize * job->shared->jobBill.numProcessors)) > userData->maxPendSlots) {
        if(logclass & LC_JLIMIT) {
            ls_syslog(LOG_DEBUG, "%s: User <%s> maxPendSlots <%d> reached with user numPEND <%d> plus job "
                                 "jobArraySize <%d> * numProcessors <%d>.",
                                 fname, auth->lsfUserName, userData->maxPendSlots, userData->numPEND, jobArraySize,
                                 job->shared->jobBill.numProcessors);
        }
        sprintf(pendLimitReason, "User <%s>", auth->lsfUserName);
        return (LSBE_SLOTS_MAX_PEND);
    }

    // lsb.users MAX_PEND_JOBS
    if ((userData->numPENDJobs + jobArraySize) > userData->maxPendJobs) {
        if(logclass & LC_JLIMIT) {
            ls_syslog(LOG_DEBUG, "%s: User <%s> maxPendJobs <%d> reached with user numPENDJobs <%d> plus job"
                                 "jobArraySize <%d>.", fname, auth->lsfUserName, userData->maxPendJobs,
                                 userData->numPENDJobs, jobArraySize);
        }
        sprintf(pendLimitReason, "User <%s>", auth->lsfUserName);
        return (LSBE_JOB_MAX_PEND);
    }

    // lsb.params MAX_PEND_JOBS
    if ((totalNumPendJobs + jobArraySize) > maxPendJobs) {
        if(logclass & LC_JLIMIT) {
            ls_syslog(LOG_DEBUG, "%s: User <%s> reached lsb.params maxPendJobs <%d> with "
                                 "total numPEND <%d> plus jobArraySize <%d>.",
                                 fname, auth->lsfUserName, maxPendJobs, totalNumPendJobs, jobArraySize);
        }
        sprintf(pendLimitReason, "System");
        return (LSBE_JOB_MAX_PEND);
    }

    // lsb.params MAX_PEND_SLOTS
    if ((pendJobSlots + (jobArraySize * job->shared->jobBill.numProcessors)) > maxPendSlots) {
        if(logclass & LC_JLIMIT) {
            ls_syslog(LOG_DEBUG, "%s: User <%s> reached lsb.params maxPendSlots <%d> with total "
                                 "numPENDSlots <%d> plus job jobArraySize <%d> * numProcessors <%d>.",
                                 fname, auth->lsfUserName, maxPendSlots, pendJobSlots, jobArraySize,
                                 job->shared->jobBill.numProcessors);
        }
        sprintf(pendLimitReason, "System");
        return (LSBE_SLOTS_MAX_PEND);
    }

    // lsb.users UserGroup
    for (i = 0; i < userData->numGrpPtr; i++) {
        struct uData *ugp = userData->gPtr[i];

        // lsb.users UserGroup MAX_PEND_SLOTS
        if ((ugp->numPEND + (jobArraySize * job->shared->jobBill.numProcessors)) > ugp->maxPendSlots) {
            if(logclass & LC_JLIMIT) {
                ls_syslog(LOG_DEBUG, "%s: User <%s> in userGroup <%s> reached maxPendJobs <%d> with "
                                     "numPEND <%d> plus job jobArraySize <%d> * numProcessors <%d>.",
                                     fname, auth->lsfUserName, ugp->user, ugp->maxPendSlots, ugp->numPEND,
                                     jobArraySize, job->shared->jobBill.numProcessors);
            }
            sprintf(pendLimitReason, "Group <%s>", ugp->user);
            return (LSBE_SLOTS_MAX_PEND);
        }

        // lsb.users UserGroup MAX_PEND_JOBS
        if ((ugp->numPENDJobs + jobArraySize) > ugp->maxPendJobs) {
            if(logclass & LC_JLIMIT) {
                ls_syslog(LOG_DEBUG, "%s: User <%s> in userGroup <%s> reached maxPendJobs <%d> with "
                                     "numPENDJobs <%d> plus job jobArraySize <%d>.",
                                     fname, auth->lsfUserName, ugp->user, ugp->maxPendJobs,
                                     ugp->numPENDJobs, jobArraySize);
            }
            sprintf(pendLimitReason, "Group <%s>", ugp->user);
            return (LSBE_JOB_MAX_PEND);
        }
    }

    return (LSBE_NO_ERROR);
}

static int
checkJobParams (struct jData *job, struct submitReq *subReq,
                struct submitMbdReply *Reply, struct lsfAuth *auth)
{
    static char fname[] = "checkJobParams";
    int i, returnErr;
    int  numAskedHosts = 0;
    int  jFlag = 0;
    struct dptNode *dptRoot = NULL;
    struct askedHost *askedHosts = NULL;
    struct submitMbdReply replyTmp;
    int askedOthPrio;
    char *word, *cp;
    int  parseError, defSpec;
    int  cpuLimit, runLimit, memLimit, dataLimit, processLimit;

    if (Reply == NULL) {
        Reply = &replyTmp;
        Reply->badJobName = NULL;
        Reply->pendLimitReason = NULL;
    }

    Reply->subTryInterval = subTryInterval;

    if (strcmp(subReq->projectName, "") == 0) {
        FREEUP (subReq->projectName);
        subReq->projectName = safeSave (getDefaultProject());
        subReq->options |= SUB_PROJECT_NAME;
    }

    if (!Gethostbyname_(subReq->fromHost)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "IsValidHost_");
        return (LSBE_BAD_HOST);
    }

    if (subReq->options & SUB_HOST) {
        returnErr = chkAskedHosts (subReq->numAskedHosts,
                                   subReq->askedHosts,
                                   subReq->numProcessors, &numAskedHosts,
                                   &askedHosts, &Reply->badReqIndx,
                                   &askedOthPrio, 1);

        if (returnErr != LSBE_NO_ERROR) {
            return (returnErr);
        } else if ( numAskedHosts <= 0 ) {
            return LSBE_BAD_HOST;
        }
    }

    now = time(0);
    if (subReq->termTime && (now > subReq->termTime)) {
        FREEUP (askedHosts);
        return(LSBE_BAD_TIME);
    }
    job->userName = safeSave (auth->lsfUserName);
    job->userId = auth->uid;
    job->jFlags = jFlag;
    memcpy((char *)&job->shared->jobBill, (char *)subReq, sizeof(struct submitReq));

    if ((subReq->options & SUB_HOST) && numAskedHosts) {
        job->askedPtr = (struct askedHost *) my_calloc (numAskedHosts,
                                                        sizeof(struct askedHost), fname);
        for (i = 0; i < numAskedHosts; i++) {
            job->askedPtr[i].hData = askedHosts[i].hData;
            job->askedPtr[i].priority = askedHosts[i].priority;
        }
        job->numAskedPtr = numAskedHosts;
        job->askedOthPrio = askedOthPrio;
        FREEUP (askedHosts);
    }


    if (subReq->resReq && subReq->resReq[0] != '\0') {
        int useLocal = TRUE;
        if (job->numAskedPtr > 0 || job->askedOthPrio >= 0) {
            useLocal = FALSE;
        }

        useLocal = useLocal ? USE_LOCAL : 0;
        job->shared->resValPtr = checkResReq(subReq->resReq,
                                             useLocal | CHK_TCL_SYNTAX | PARSE_XOR);

        if( job->shared->resValPtr == NULL ) {
            return (LSBE_BAD_RESREQ);
        }
    }


    if (subReq->options & SUB_QUEUE)
        cp = subReq->queue;
    else
        cp = defaultQueues;
    Reply->queue = cp;
    returnErr = LSBE_BAD_QUEUE;
    cpuLimit = subReq->rLimits[LSF_RLIMIT_CPU];
    runLimit = subReq->rLimits[LSF_RLIMIT_RUN];
    memLimit = subReq->rLimits[LSF_RLIMIT_RSS];
    dataLimit = subReq->rLimits[LSF_RLIMIT_DATA];
    processLimit = subReq->rLimits[LSF_RLIMIT_PROCESS];

    defSpec = subReq->options & SUB_HOST_SPEC;
    while ((word = getNextWord_(&cp))) {

        subReq->rLimits[LSF_RLIMIT_CPU] = cpuLimit;
        subReq->rLimits[LSF_RLIMIT_RUN] = runLimit;
        subReq->rLimits[LSF_RLIMIT_RSS] = memLimit;
        subReq->rLimits[LSF_RLIMIT_DATA] = dataLimit;
        subReq->rLimits[LSF_RLIMIT_PROCESS] = processLimit;

        if ((returnErr = queueOk (word, job, &Reply->badReqIndx,
                                  subReq, auth)) != LSBE_NO_ERROR) {
            Reply->queue = word;

            if (!defSpec)
                subReq->options &= ~SUB_HOST_SPEC;
            continue;
        } else
            break;
    }
    if (returnErr != LSBE_NO_ERROR) {
        return (returnErr);
    } else {
        Reply->queue = job->qPtr->queue;
        job->shared->jobBill.rLimits[LSF_RLIMIT_CPU]
            = subReq->rLimits[LSF_RLIMIT_CPU];
        job->shared->jobBill.rLimits[LSF_RLIMIT_RUN]
            = subReq->rLimits[LSF_RLIMIT_RUN];
        job->shared->jobBill.rLimits[LSF_RLIMIT_DATA]
            = subReq->rLimits[LSF_RLIMIT_DATA];
        job->shared->jobBill.rLimits[LSF_RLIMIT_RSS]
            = subReq->rLimits[LSF_RLIMIT_RSS];
        job->shared->jobBill.rLimits[LSF_RLIMIT_PROCESS]
            = subReq->rLimits[LSF_RLIMIT_PROCESS];
    }

    returnErr = checkSubHost(job);
    if (returnErr != LSBE_NO_ERROR) {
        return(returnErr);
    }


    subReq->numProcessors = job->shared->jobBill.numProcessors;
    subReq->maxNumProcessors = job->shared->jobBill.maxNumProcessors;


    if ((subReq->options & SUB_HOST) && (subReq->options2 & SUB2_USE_DEF_PROCLIMIT)) {
        returnErr = chkAskedHosts (subReq->numAskedHosts,
                                   subReq->askedHosts,
                                   subReq->numProcessors, &numAskedHosts,
                                   &askedHosts, &Reply->badReqIndx,
                                   &askedOthPrio, 1);

        FREEUP (askedHosts);

        if (returnErr != LSBE_NO_ERROR)
            return (returnErr);
    }

    if (numofprocs < subReq->numProcessors)
    {
        if (job->qPtr->numHUnAvail == 0)
            return (LSBE_PROC_NUM);
    }

    if (subReq->options & SUB_INTERACTIVE) {

        if (subReq->options & SUB_RERUNNABLE)
            return (LSBE_INTERACTIVE_RERUN);
        if ((subReq->options & (SUB_PTY | SUB_PTY_SHELL)) &&
            (subReq->options & SUB_IN_FILE))
            return (LSBE_PTY_INFILE);
    }
    FREEUP (subReq->queue);
    subReq->options |= SUB_QUEUE;
    subReq->queue = safeSave (job->qPtr->queue);



    if (strcmp (subReq->dependCond, "") != 0) {


        setIdxListContext(subReq->jobName);


        if ((getIdxListContext() == NULL)
            && (strstr((*subReq).dependCond, "[*]")) != NULL) {
            return(LSBE_DEP_ARRAY_SIZE);
        }

        dptRoot = parseDepCond(subReq->dependCond, auth, &parseError,
                               &Reply->badJobName, &jFlag, 0);
        if (dptRoot == NULL) {
            freeIdxListContext();
            return(parseError);
        }


        freeIdxListContext();
    }

    job->shared->dptRoot = dptRoot;

    job->jFlags = jFlag;


    if (subReq->options2 & SUB2_JOB_PRIORITY)  {
        int error;
        if ( checkUserPriority(job, subReq->userPriority, &error)) {

            job->jobPriority = subReq->userPriority;
        }
        else {
            return(error);
        }

    }
    else if ( maxUserPriority > 0 ) {

        job->jobPriority  = maxUserPriority/2;
    }


    return (LSBE_NO_ERROR);

}

struct resVal *
checkResReq(char *resReq, int checkOptions)
{
    static char fname[] = "checkResReq";
    int rusgDefined = FALSE, jj, isSet, options;
    struct resVal *resValPtr = NULL;
    struct tclHostData tclHostData;

    resValPtr = (struct resVal *)my_malloc (sizeof (struct resVal),
                                            "checkResReq");
    initResVal (resValPtr);

    if (checkOptions & USE_LOCAL)
        options = (PR_ALL | PR_BATCH | PR_DEFFROMTYPE);
    else
        options = (PR_ALL | PR_BATCH);

    if (checkOptions & PARSE_XOR)
        options |= PR_XOR;

    if (parseResReq (resReq, resValPtr, allLsInfo, options, unitForLimits) != PARSE_OK) {
        goto error;
    }

    getTclHostData(&tclHostData, getHostData(masterHost), NULL);



    if (checkOptions & CHK_TCL_SYNTAX)
        tclHostData.flag = TCL_CHECK_SYNTAX;


    if (resValPtr->xorExprs != NULL) {
        for (jj = 0; resValPtr->xorExprs[jj]; jj++) {
            if (evalResReq(resValPtr->xorExprs[jj], &tclHostData, FALSE) < 0) {
                freeTclHostData (&tclHostData);
                goto error;
            }
        }
    } else {

        if (evalResReq(resValPtr->selectStr, &tclHostData, FALSE) < 0) {
            freeTclHostData (&tclHostData);
            goto error;
        }
    }


    freeTclHostData (&tclHostData);
    for (jj = 0; jj < allLsInfo->nRes; jj++) {
        if (NOT_NUMERIC(allLsInfo->resTable[jj]))
            continue;
        TEST_BIT(jj, resValPtr->rusgBitMaps, isSet);
        if (isSet == 0)
            continue;
        if (jj > allLsInfo->numIndx
            && (getResource (allLsInfo->resTable[jj].name) == NULL))
            goto error;
        rusgDefined = TRUE;
    }
    if (rusgDefined == TRUE) {
        if (resValPtr->duration <= 0 || (resValPtr->decay < 0.0) )
            goto error;
    }

    if (resValPtr->numHosts == INFINIT_INT
        && resValPtr->maxNumHosts < INFINIT_INT)
        resValPtr->numHosts = 1;
    if (resValPtr->maxNumHosts == INFINIT_INT)
        resValPtr->maxNumHosts = resValPtr->numHosts;

    if (resValPtr->numHosts < 0
        || resValPtr->maxNumHosts < resValPtr->numHosts
        || resValPtr->pTile < 0)
        goto error;
    if (logclass & (LC_EXEC))
        ls_syslog(LOG_DEBUG2, "%s: resReq=%s; decay=%f, duration=%d; hosts=%d,%d; ptile=%d", fname, resReq, resValPtr->decay, resValPtr->duration, resValPtr->numHosts, resValPtr->maxNumHosts, resValPtr->pTile);


    if ((resValPtr->numHosts != 1 && resValPtr->numHosts != INFINIT_INT)
        || (resValPtr->numHosts != resValPtr->maxNumHosts)
        || (resValPtr->pTile < 1))
        goto error;

    return (resValPtr);

error:
    lsbFreeResVal(&resValPtr);
    if (logclass & (LC_EXEC) && resReq)
        ls_syslog(LOG_DEBUG1, "%s: parseResReq(%s) failed",
                  fname, resReq);
    return (NULL);

}
void
copyJobBill (struct submitReq *subReq, struct submitReq *jobBill, LS_LONG_INT jobId)
{
    int i;

    memcpy((char *) jobBill, (char *)subReq , sizeof (struct submitReq));
    now = time(NULL);
    if (jobId != FALSE) {
        char fn[MAXFILENAMELEN];

        jobBill->submitTime = now;
        if (subReq->jobFile[0] == '\0') {
            sprintf(fn, "%ld.%s", now, lsb_jobidinstr(jobId));
        } else {

            if (subReq->options & SUB_RESTART) {
                sprintf(fn, "%s.%s", subReq->jobFile, lsb_jobidinstr(jobId));
            } else {
                sprintf(fn, "%s", subReq->jobFile);
            }
        }
        jobBill->jobFile = safeSave(fn);
    } else {
        jobBill->submitTime = subReq->submitTime;
        jobBill->jobFile = safeSave(subReq->jobFile);
    }
    jobBill->cwd = safeSave(subReq->cwd);
    jobBill->fromHost  = safeSave(subReq->fromHost);
    jobBill->subHomeDir = safeSave(subReq->subHomeDir);
    jobBill->command = safeSave(subReq->command);

    jobBill->queue = safeSave (subReq->queue);
    jobBill->resReq = safeSave(subReq->resReq);
    jobBill->dependCond = safeSave(subReq->dependCond);
    jobBill->preExecCmd = safeSave(subReq->preExecCmd);
    jobBill->postExecCmd = safeSave(subReq->postExecCmd);
    jobBill->schedHostType = safeSave(subReq->schedHostType);

    if (subReq->options & SUB_LOGIN_SHELL)
        jobBill->loginShell = safeSave(subReq->loginShell);
    else
        jobBill->loginShell = safeSave("");

    if (subReq->options & SUB_MAIL_USER)
        jobBill->mailUser = safeSave(subReq->mailUser);
    else
        jobBill->mailUser = safeSave("");

    jobBill->projectName = safeSave(subReq->projectName);

    jobBill->jobName = safeSave(subReq->jobName);

    if (subReq->options & SUB_IN_FILE)
        jobBill->inFile = safeSave(subReq->inFile);
    else
        jobBill->inFile = safeSave("/dev/null");
    if (subReq->options & SUB_OUT_FILE)
        jobBill->outFile = safeSave(subReq->outFile);
    else
        jobBill->outFile = safeSave("/dev/null");
    if (subReq->options & SUB_ERR_FILE)
        jobBill->errFile = safeSave(subReq->errFile);
    else if (subReq->options & SUB_OUT_FILE)
        jobBill->errFile = safeSave(jobBill->outFile);
    else
        jobBill->errFile = safeSave("/dev/null");

    if (subReq->options2 & SUB2_IN_FILE_SPOOL) {
        jobBill->inFileSpool = safeSave(subReq->inFileSpool);
        jobBill->inFile = safeSave(subReq->inFile);
    } else {
        jobBill->inFileSpool = safeSave("/dev/null");
    }

    if (subReq->options2 & SUB2_JOB_CMD_SPOOL)
        jobBill->commandSpool = safeSave(subReq->commandSpool);
    else
        jobBill->commandSpool = safeSave("");


    if (subReq->options & SUB_HOST_SPEC)
        jobBill->hostSpec = safeSave (subReq->hostSpec);
    else
        jobBill->hostSpec = safeSave("");

    if (subReq->options & SUB_CHKPNT_DIR) {
        jobBill->chkpntDir = safeSave(subReq->chkpntDir);
    } else {

        jobBill->chkpntDir = safeSave("");
    }

    if (subReq->nxf > 0) {
        jobBill->xf = (struct xFile *) my_calloc(subReq->nxf,
                                                 sizeof(struct xFile), "copyJobBill");
        memcpy((char *) jobBill->xf, (char *) subReq->xf,
               subReq->nxf * sizeof(struct xFile));
    } else {
        jobBill->nxf = 0;
        jobBill->xf = NULL;
    }

    if (subReq->numAskedHosts > 0) {
        jobBill->askedHosts = (char **) my_calloc (subReq->numAskedHosts,
                                                   sizeof(char *), "copyJobBill");
        for (i = 0; i < subReq->numAskedHosts; i++)
            jobBill->askedHosts[i] = safeSave(subReq->askedHosts[i]);
        jobBill->numAskedHosts = subReq->numAskedHosts;
    } else {
        jobBill->askedHosts = NULL;
        jobBill->numAskedHosts = 0;
    }

}

void
freeJData (struct jData *jpbw)
{
    static char fname[]="freeJData";
    PROXY_LIST_ENTRY_T      *pxy;

    if (!jpbw)
        return;

    if (pxyRsvJL != NULL) {
        pxy = (PROXY_LIST_ENTRY_T *)listSearchEntry(pxyRsvJL,
                                                    (LIST_ENTRY_T *)jpbw,
                                                    (LIST_ENTRY_EQUALITY_OP_T)
                                                    proxyListEntryEqual,
                                                    0);
        if (pxy != NULL) {

            if (logclass & (LC_TRACE)) {
                ls_syslog (LOG_DEBUG,
                           "%s: job <%s> will be removed, but still in pxyRsvJL",
                           fname, lsb_jobid2str(jpbw->jobId));
            }
            proxyRsvJLRemoveEntry(jpbw);
        }
    }

    if (IS_PEND(jpbw->jStatus) && jpbw->candPtr) {
        FREEUP (jpbw->candPtr);
        if (jpbw->numHostPtr > 0 && (jpbw->jStatus & JOB_STAT_RESERVE)) {

            if (logclass & (LC_TRACE))
                ls_syslog(LOG_DEBUG3, "%s: job <%s> updRes - <%d> slots <%s:%d>",
                          fname, lsb_jobid2str(jpbw->jobId), jpbw->numHostPtr,
                          __FILE__,  __LINE__);

            freeReserveSlots(jpbw);
        }
    }

    if (JOB_PREEMPT_WAIT(jpbw))
        freeReservePreemptResources(jpbw);

    FREEUP (jpbw->userName);
    FREEUP (jpbw->lsfRusage);
    FREEUP (jpbw->reasonTb);
    FREEUP (jpbw->hPtr);

    FREEUP (jpbw->execHome);
    FREEUP (jpbw->execCwd);
    FREEUP (jpbw->execUsername);
    FREEUP (jpbw->queuePreCmd);
    FREEUP (jpbw->queuePostCmd);
    FREEUP (jpbw->reqHistory);
    FREEUP (jpbw->schedHost);
    if (jpbw->runRusage.npids > 0)
        FREEUP (jpbw->runRusage.pidInfo);
    if (jpbw->runRusage.npgids > 0)
        FREEUP (jpbw->runRusage.pgid);

    if (jpbw->newSub) {
        freeSubmitReq (jpbw->newSub);
        FREEUP (jpbw->newSub);
    }
    if (jpbw->shared->numRef <= 1)
        freeSubmitReq (&(jpbw->shared->jobBill));
    destroySharedRef(jpbw->shared);
    FREEUP (jpbw->askedPtr);

    FREEUP(jpbw->reqHistory);

    FREEUP(jpbw->execHosts);
    FREEUP (jpbw->candPtr);
    FREEUP (jpbw->jobSpoolDir);

    FREE_ALL_GRPS_CAND(jpbw);

    if (jpbw->numRef <= 0 ) {
        FREEUP(jpbw);
    }
    else {
        jpbw->jStatus |= JOB_STAT_VOID;
        voidJobList = listSetInsert((long)jpbw, voidJobList);
        ls_syslog(LOG_DEBUG1, _i18n_msg_get(ls_catd , NL_SETN, 6527,
                                            "%s: job <%s> can not be freed with numRef = <%d>"),
                  fname, lsb_jobid2str(jpbw->jobId), jpbw->numRef);
        /* catgets 6527 */
    }
}

void
freeSubmitReq (struct submitReq *jobBill)
{
    int i;

    FREEUP (jobBill->jobName);
    FREEUP (jobBill->queue);
    FREEUP (jobBill->resReq);
    FREEUP (jobBill->hostSpec);
    FREEUP (jobBill->dependCond);
    FREEUP (jobBill->inFile);
    FREEUP (jobBill->outFile);
    FREEUP (jobBill->errFile);
    FREEUP (jobBill->command);
    FREEUP (jobBill->inFileSpool);
    FREEUP (jobBill->commandSpool);
    FREEUP (jobBill->chkpntDir);
    FREEUP (jobBill->preExecCmd);
    FREEUP (jobBill->postExecCmd);
    FREEUP (jobBill->mailUser);
    FREEUP (jobBill->projectName);
    FREEUP (jobBill->cwd);
    FREEUP (jobBill->subHomeDir);
    FREEUP (jobBill->fromHost);
    FREEUP (jobBill->loginShell);
    FREEUP (jobBill->schedHostType);

    if (jobBill->numAskedHosts > 0) {
        for (i = 0; i < jobBill->numAskedHosts; i++)
            FREEUP (jobBill->askedHosts[i]);
        FREEUP (jobBill->askedHosts);
    }
    jobBill->numAskedHosts = 0;
    if (jobBill->nxf)
        FREEUP (jobBill->xf);
    jobBill->nxf = 0;

    FREEUP (jobBill->jobFile);

}

static int
mergeSubReq (struct submitReq *to, struct submitReq *old,
             struct modifyReq *req, const struct passwd* pwUser)
{
    static char fname[] = "mergeSubReq";
    int i;
    int delOptions = req->delOptions;
    int delOptions2 = req->delOptions2;
    struct submitReq *new = &req->submitReq;

    to->options = 0;
    to->options2 = 0;
    to->numAskedHosts = 0;
    to->askedHosts = NULL;

    to->cwd = safeSave(old->cwd);
    to->fromHost  = safeSave(old->fromHost);
    to->subHomeDir = safeSave(old->subHomeDir);

    to->schedHostType = safeSave(old->schedHostType);
    to->jobFile = safeSave(old->jobFile);
    to->submitTime = old->submitTime;
    to->umask = old->umask;
    to->restartPid = old->restartPid;
    to->niosPort = old->niosPort;

    if (old->options2 & SUB2_BSUB_BLOCK)
        to->options2 |= SUB2_BSUB_BLOCK;

    if (old->options2 & SUB2_HOLD)
        to->options2 |= SUB2_HOLD;

    if (old->options2 & SUB2_HOST_NT)
        to->options2 |= SUB2_HOST_NT;

    if (old->options2 & SUB2_HOST_UX)
        to->options2 |= SUB2_HOST_UX;

    if (old->options & SUB_INTERACTIVE)
        to->options |= SUB_INTERACTIVE;

    if (old->options & SUB_PTY)
        to->options |= SUB_PTY;

    if (old->options & SUB_PTY_SHELL)
        to->options |= SUB_PTY;

    if (new->options & SUB_JOB_NAME) {
        to->options |= SUB_JOB_NAME;
        if (old->options & SUB_JOB_NAME) {
            char newName[1024], *strPos;

            strcpy(newName, new->jobName);
            if ((strPos = strchr(old->jobName, '['))) {
                strcat(newName, strPos);
            }
            to->jobName = safeSave (newName);
        }
        else
            to->jobName = safeSave (new->jobName);
    } else if ((old->options & SUB_JOB_NAME) && !(delOptions & SUB_JOB_NAME)) {
        to->options |= SUB_JOB_NAME;
        to->jobName = safeSave (old->jobName);
    }

    if (!(to->options & SUB_JOB_NAME)) {
        if (new->options2 & (SUB2_MODIFY_CMD | SUB2_JOB_CMD_SPOOL))
            to->jobName = safeSave("");
        else
            to->jobName = safeSave(old->jobName);
    }

    if (new->options & SUB_HOST) {
        to->options |= SUB_HOST;
        copyHosts (to, new);
    } else if ((old->options & SUB_HOST) && !(delOptions & SUB_HOST)) {
        to->options |= SUB_HOST;
        copyHosts (to, old);
    }

#define mergeStrField(fname, fmask) {                                   \
        if(new->options & fmask) { to->options |= fmask;                \
            to->fname  =safeSave(new->fname);}                          \
        else if ((old->options & fmask) && !(delOptions & fmask)) {     \
            to->options |= fmask;                                       \
            to->fname = safeSave(old->fname);}                          \
        else to->fname = safeSave("");}

#define mergeFlagField(f) if ((new->options & (f)) ||                   \
                              ((old->options & (f)) && !(delOptions & (f)))) \
        to->options |= (f);


#define mergeIntField(fname, fmask, dflt) {                             \
        if(new->options & fmask) { to->options |= fmask;                \
            to->fname  =new->fname;}                                    \
        else if ((old->options & fmask) && !(delOptions & fmask)) {     \
            to->options |= fmask;                                       \
            to->fname = old->fname;}                                    \
        else to->fname = (dflt);}

#define mergeIntField2(fname, fmask, dflt) {                            \
        if(new->options2 & fmask) { to->options2 |= fmask;              \
            to->fname  = new->fname;}                                   \
        else if ((old->options2 & fmask) && !(delOptions2 & fmask)) {   \
            to->options2 |= fmask;                                      \
            to->fname = old->fname;}                                    \
        else to->fname = (dflt);}

    mergeStrField(queue, SUB_QUEUE);
    mergeStrField(outFile, SUB_OUT_FILE);
    mergeStrField(errFile, SUB_ERR_FILE);
    mergeStrField(chkpntDir, SUB_CHKPNT_DIR);
    mergeStrField(dependCond, SUB_DEPEND_COND);
    mergeStrField(resReq, SUB_RES_REQ);
    mergeStrField(preExecCmd, SUB_PRE_EXEC);
    mergeStrField(postExecCmd, SUB_POST_EXEC);
    mergeStrField(mailUser, SUB_MAIL_USER);
    mergeStrField(projectName, SUB_PROJECT_NAME);
    mergeStrField(loginShell, SUB_LOGIN_SHELL);
#define mergeStrField2(fname, fmask) {                                  \
        if(new->options2 & fmask) { to->options2 |= fmask;              \
            to->fname  =safeSave(new->fname);}                          \
        else if ((old->options2 & fmask) && !(delOptions2 & fmask)) {   \
            to->options2 |= fmask;                                      \
            to->fname = safeSave(old->fname);}                          \
        else to->fname = safeSave("");}

    if ( maxUserPriority < 0 ) {
        mergeIntField2(userPriority, SUB2_JOB_PRIORITY, -1);
    }
    else {
        mergeIntField2(userPriority, SUB2_JOB_PRIORITY, maxUserPriority/2);
    }

    if(new->options2 & SUB2_IN_FILE_SPOOL) {

        to->options2 |= SUB2_IN_FILE_SPOOL;
        to->inFile  = safeSave(new->inFile);
        to->inFileSpool  = safeSave(new->inFileSpool);

        if (old->options2 & SUB2_IN_FILE_SPOOL) {

            childRemoveSpoolFile(old->inFileSpool,
                                 FORK_REMOVE_SPOOL_FILE, pwUser);
        }
    } else if ((delOptions2 & SUB2_IN_FILE_SPOOL)) {

        if ((old->options & SUB_IN_FILE)
            || (old->options2 & SUB2_IN_FILE_SPOOL)) {
            to->options |= SUB_IN_FILE;
            to->inFile  = safeSave(old->inFile);
        } else {
            to->inFile  = safeSave("");
        }
        to->inFileSpool  = safeSave("");

        if (old->options2 & SUB2_IN_FILE_SPOOL) {

            childRemoveSpoolFile(old->inFileSpool,
                                 FORK_REMOVE_SPOOL_FILE, pwUser);
        }
    } else if(new->options & SUB_IN_FILE) {

        to->options |= SUB_IN_FILE;
        to->inFile  = safeSave(new->inFile);
        to->inFileSpool  = safeSave("");

        if (old->options2 & SUB2_IN_FILE_SPOOL) {

            childRemoveSpoolFile(old->inFileSpool,
                                 FORK_REMOVE_SPOOL_FILE, pwUser);
        }
    } else if (delOptions & SUB_IN_FILE) {

        to->inFile = safeSave("");
        to->inFileSpool  = safeSave("");

        if (old->options2 & SUB2_IN_FILE_SPOOL) {

            childRemoveSpoolFile(old->inFileSpool,
                                 FORK_REMOVE_SPOOL_FILE, pwUser);
        }
    } else {

        if (old->options2 & SUB2_IN_FILE_SPOOL) {
            to->options2 |= SUB2_IN_FILE_SPOOL;
        } else if (old->options & SUB_IN_FILE) {
            to->options |= SUB_IN_FILE;
        }
        to->inFile = safeSave(old->inFile);
        to->inFileSpool  = safeSave(old->inFileSpool);
    }


    if(new->options & SUB_HOST_SPEC) {
        to->options |= SUB_HOST_SPEC;
        to->hostSpec = safeSave (new->hostSpec);
    } else
        to->hostSpec = safeSave ("");


    if (new->options & SUB_CHKPNT_PERIOD) {
        to->options |= SUB_CHKPNT_PERIOD;
        to->chkpntPeriod = new->chkpntPeriod;
    } else
        to->chkpntPeriod = 0;

    mergeFlagField(SUB_EXCLUSIVE);
    mergeFlagField(SUB_NOTIFY_END);
    mergeFlagField(SUB_NOTIFY_BEGIN);
    mergeFlagField(SUB_RERUNNABLE);
    if (old->options2 & SUB2_QUEUE_RERUNNABLE)
        to->options2 |= SUB2_QUEUE_RERUNNABLE;
    mergeFlagField(SUB_MODIFY_ONCE);

    mergeIntField(sigValue, SUB_WINDOW_SIG, 0);

    if ((new->options & SUB_RESTART) || (old->options & SUB_RESTART))
        to->options |= SUB_RESTART;
    if ((new->options & SUB_RESTART_FORCE) ||
        (old->options & SUB_RESTART_FORCE))
        to->options |= SUB_RESTART_FORCE;

    if (new->options & SUB_OTHER_FILES) {
        to->options |= SUB_OTHER_FILES;
        to->nxf = new->nxf;
        to->xf = (struct xFile *) my_calloc(new->nxf,
                                            sizeof(struct xFile), "mergeSubReq");
        memcpy((char *)to->xf, (char *) new->xf,
               new->nxf * sizeof(struct xFile));
    } else if ((old->options & SUB_OTHER_FILES) &&
               !(delOptions & SUB_OTHER_FILES)) {
        to->options |= SUB_OTHER_FILES;
        to->nxf = old->nxf;
        to->xf = (struct xFile *) my_calloc(old->nxf,
                                            sizeof(struct xFile), "mergeSubReq");
        memcpy((char *)to->xf, (char *) old->xf,
               old->nxf * sizeof(struct xFile));
    } else
        to->nxf = 0;



    for (i = 0; i < LSF_RLIM_NLIMITS; i++) {
        float *cpuFactor;
        to->rLimits[i] = DEFAULT_RLIMIT;
        if (new->rLimits[i] != DELETE_NUMBER &&
            new->rLimits[i] != DEFAULT_RLIMIT)
            to->rLimits[i] = new->rLimits[i];
        else if (new->rLimits[i] == DEFAULT_RLIMIT) {

            if (old->rLimits[i] > 0
                && (i == LSF_RLIMIT_CPU || i == LSF_RLIMIT_RUN)) {
                if ((cpuFactor = getModelFactor (old->hostSpec)) == NULL) {
                    if ((cpuFactor = getHostFactor (old->hostSpec)) == NULL) {
                        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6528,
                                                         "%s: Cannot find cpu factor for hostSpec <%s>"), fname, to->hostSpec); /* catgets 6528 */
                        return (LSBE_BAD_HOST_SPEC);
                    }
                }
                if (*cpuFactor != 0)
                    to->rLimits[i] = old->rLimits[i]/(*cpuFactor);
                FREEUP (to->hostSpec);
                to->hostSpec = safeSave (old->hostSpec);
            } else
                to->rLimits[i] = old->rLimits[i];
        }
    }

    if (!limitIsOk_(to->rLimits)) {
        to->options |= SUB_RLIMIT_UNIT_IS_KB;
    }

    if (new->beginTime != DELETE_NUMBER && new->beginTime != 0)
        to->beginTime = new->beginTime;
    else if (new->beginTime == DELETE_NUMBER)
        to->beginTime = 0;
    else
        to->beginTime = old->beginTime;

    if (new->termTime != DELETE_NUMBER && new->termTime != 0)
        to->termTime = new->termTime;
    else if (new->termTime == DELETE_NUMBER)
        to->termTime = 0;
    else
        to->termTime = old->termTime;

    if (new->numProcessors != DEFAULT_NUMPRO &&
        new->numProcessors != DEL_NUMPRO) {
        to->numProcessors = new->numProcessors;
        to->maxNumProcessors = new->maxNumProcessors;
        to->options2 &= ~SUB2_USE_DEF_PROCLIMIT;
    } else if (new->numProcessors == DEL_NUMPRO) {
        to->numProcessors = 1;
        to->maxNumProcessors = 1;
        to->options2 |= SUB2_USE_DEF_PROCLIMIT;
    } else {
        to->numProcessors = old->numProcessors;
        to->maxNumProcessors = old->maxNumProcessors;
        if (old->options2 & SUB2_USE_DEF_PROCLIMIT) {
            to->options2 |= SUB2_USE_DEF_PROCLIMIT;
        }
    }

    return (0);

}


static void
copyHosts (struct submitReq *to , struct submitReq *from)
{
    int i;

    if (from->numAskedHosts > 0) {
        to->askedHosts = (char **)
            my_malloc (from->numAskedHosts * sizeof (char *), "copyHosts");
        for (i = 0; i < from->numAskedHosts; i++)
            to->askedHosts[i] = safeSave (from->askedHosts[i]);
        to->numAskedHosts = from->numAskedHosts;
    } else {
        to->numAskedHosts = 0;
        to->askedHosts = NULL;
    }

}

static void
mailUser (struct jData *oldjob)
{
    char msg[256];

    sprintf (msg, _i18n_msg_get(ls_catd, NL_SETN, 1517,
                                "Your job <%s> has been killed because the execution host <%s> is no longer available.\nThe job will be re-queued and re-run with the same jobId."), /* catgets 1517 */
             lsb_jobid2str(oldjob->jobId),
             oldjob->hPtr[0]->host);


    if (oldjob->shared->jobBill.options & SUB_MAIL_USER)
        merr_user (oldjob->shared->jobBill.mailUser,
                   oldjob->shared->jobBill.fromHost,
                   msg,
                   I18N_info);
    else
        merr_user (oldjob->userName,
                   oldjob->shared->jobBill.fromHost,
                   msg,
                   I18N_info);

    return;

}
void
inZomJobList (struct jData *oldjob, int mail)
{
    struct jData *newjob, *jp = jDataList[ZJL]->forw;
    int i;

    if (mail == TRUE)
        mailUser (oldjob);

    newjob = initJData((struct jShared *) my_calloc(1, sizeof(struct jShared),
                                                    "inZomJobList"));
    newjob->jStatus = oldjob->jStatus;
    SET_STATE(newjob->jStatus, JOB_STAT_EXIT);
    newjob->newReason = oldjob->newReason;
    newjob->oldReason = oldjob->oldReason;
    newjob->subreasons = oldjob->subreasons;
    newjob->jobId = oldjob->jobId;

    memcpy(&newjob->shared->jobBill,
           &oldjob->shared->jobBill,
           sizeof(struct submitReq));

    newjob->jgrpNode = NULL;
    newjob->nextJob  = NULL;
    newjob->numRef   = 0;

    copyJobBill(&oldjob->shared->jobBill,
                &newjob->shared->jobBill,
                newjob->jobId);
    newjob->restartPid = oldjob->restartPid;
    newjob->chkpntPeriod = oldjob->chkpntPeriod;

    newjob->userName  = safeSave(oldjob->userName);
    newjob->jFlags = oldjob->jFlags;
    if (oldjob->numHostPtr > 0) {
        newjob->hPtr = my_calloc(oldjob->numHostPtr,
                                 sizeof(char *),
                                 "inZomJobList");
        for (i = 0; i < oldjob->numHostPtr; i++)
            newjob->hPtr[i] = oldjob->hPtr[i];
    }
    newjob->numHostPtr = oldjob->numHostPtr;
    newjob->qPtr = oldjob->qPtr;
    newjob->userId = oldjob->userId;
    newjob->jobPid = oldjob->jobPid;
    newjob->jobPGid = oldjob->jobPGid;
    newjob->startTime = oldjob->startTime;
    newjob->endTime = oldjob->endTime;

    newjob->actPid = oldjob->actPid;
    newjob->execCwd = safeSave (oldjob->execCwd);
    newjob->execHome = safeSave (oldjob->execHome);
    newjob->execUsername = safeSave (oldjob->execUsername);
    newjob->queuePreCmd = safeSave (oldjob->queuePreCmd);
    newjob->queuePostCmd = safeSave (oldjob->queuePostCmd);
    newjob->sigValue = oldjob->sigValue;
    newjob->schedHost = safeSave (oldjob->schedHost);


    newjob->pendEvent.sig = SIGTERM;
    eventPending = TRUE;

    if ((newjob->shared->jobBill.options & SUB_CHKPNTABLE) &&
        ((oldjob->shared->jobBill.options & SUB_RESTART) ||
         (oldjob->jStatus & JOB_STAT_CHKPNTED_ONCE))) {
        oldjob->shared->jobBill.options |= SUB_RESTART | SUB_RESTART_FORCE;
        newjob->shared->jobBill.restartPid = (oldjob->restartPid) ?
            oldjob->restartPid :
            oldjob->jobPid;
        newjob->restartPid = newjob->shared->jobBill.restartPid;
    }
    oldjob->shared->jobBill.options &= ~JOB_STAT_CHKPNTED_ONCE;

    inList ((struct listEntry *)jp, (struct  listEntry *)newjob);
}

static struct jData *
isInZomJobList(struct hData *hData, struct statusReq *statusReq)
{
    struct jData *jData;

    for (jData = jDataList[ZJL]->back;
         jData != jDataList[ZJL];
         jData = jData->back) {

        if (jData->jobId != statusReq->jobId)
            continue;

        if (jData->hPtr != NULL && hData != jData->hPtr[0])
            continue;
        if (jData->jobPid != statusReq->jobPid)
            continue;

        return jData;
    }

    return NULL;
}

struct jData *
getZombieJob (LS_LONG_INT jobId)
{
    struct jData *jData;

    for (jData = jDataList[ZJL]->back; jData != jDataList[ZJL];
         jData = jData->back) {
        if (jData->jobId != jobId)
            continue;
        return (jData);
    }
    return (NULL);

}


void
accumRunTime (struct jData *jData, int newStatus, time_t eventTime)
{
    static char fname[] = "accumRunTime()";
    int             diffTime = 0;
    time_t          currentTime;

    if (eventTime != 0)
        currentTime = eventTime;
    else
        currentTime = time (0);

    if ((newStatus & JOB_STAT_RUN) && (jData->jStatus & JOB_STAT_PEND)) {
        jData->runTime = 0;
    } else if ((newStatus & JOB_STAT_RUN) && (IS_SUSP(jData->jStatus))) {

        jData->resumeTime = now;
    } else if (!IS_START(newStatus) && IS_START(jData->jStatus)) {
        diffTime = - (jData->runTime);
        jData->runTime = 0;
    } else if (jData->jStatus & JOB_STAT_RUN) {
        diffTime = (int) (currentTime - jData->updStateTime);
        jData->runTime += diffTime;
    }
    jData->updStateTime = currentTime;

    /*update related shareAcct runtime statictis*/
    if (jData->sa && diffTime != 0) {
        SACCT_RUNTIME_UPDATE(diffTime, jData->sa, fname);

        /*In replacing events time, after we update shareAcct all statistics for job, 
         *then we update shareAcct priority and order in one time.
         */
        if (mSchedStage != M_STAGE_REPLAY) {
            updSAcctPriorityByPath(jData->sa);
        }
        if (logclass & LC_FAIR) {
            ls_syslog(LOG_DEBUG1, "%s: updated SAAP <%s> runtime <%ds> for job <%s>",
                    fname, jData->sa->path,
                    diffTime,
                    lsb_jobid2str(jData->jobId));
        }
    }

    return;

}

int
msgJob (struct bucket *bucket, struct lsfAuth *auth)
{
    struct jData *jpbw;
    int reply;
    sbdReplyType sbdReply;

    if (logclass & (LC_SIGNAL | LC_TRACE))
        ls_syslog(LOG_DEBUG1,"msgJob: job <%s>, msgid <%d>",
                  lsb_jobid2str(bucket->proto.jobId), bucket->proto.msgId);

    if ((jpbw = getJobData (bucket->proto.jobId)) == NULL)
        return (LSBE_NO_JOB);


    if (auth) {
        if (auth->uid != 0 && !jgrpPermitOk(auth, jpbw->jgrpNode)
            && !isAuthQueAd (jpbw->qPtr, auth)) {
            if ( !isJobOwner(auth, jpbw))
                return (LSBE_PERMISSION);
        }
    }

    switch (MASK_STATUS(jpbw->jStatus)) {
        case JOB_STAT_DONE:
        case JOB_STAT_EXIT:
            reply = LSBE_JOB_FINISH;
            break;

        case JOB_STAT_PEND:
        case JOB_STAT_PSUSP:
            reply = LSBE_OP_RETRY;
            break;
        default:

            if ( IS_POST_FINISH(jpbw->jStatus) ) {
                reply = LSBE_JOB_FINISH;
                break;
            }


            sbdReply = msgStartedJob (jpbw, bucket);
            switch (sbdReply) {
                case ERR_NO_ERROR:

                    reply = LSBE_NO_ERROR;
                    break;

                case ERR_NO_JOB:
                    reply = LSBE_JOB_FINISH;
                    break;
                default:
                    reply = LSBE_OP_RETRY;
            }
    }

    return (reply);

}


static sbdReplyType
msgStartedJob (struct jData *jData, struct bucket *bucket)
{
    static char fname[] = "msgStartedJob";
    sbdReplyType reply;
    struct jobReply jobReply;

    if (jData->hPtr[0]->flags & HOST_LOST_FOUND) {

        if (bucket->bufstat == MSG_STAT_SENT) {
            QUEUE_REMOVE(bucket);
            QUEUE_APPEND(bucket, jData->hPtr[0]->msgq[MSG_STAT_QUEUED]);
            bucket->bufstat = MSG_STAT_QUEUED;
            return(LSBE_OP_RETRY);
        }
    }

    reply = msg_job (jData, bucket->storage, &jobReply);

    switch (reply) {
        case ERR_NO_ERROR:
            break;

        case ERR_NO_JOB:
            jData->newReason = EXIT_NORMAL;
            jStatusChange(jData, JOB_STAT_EXIT, LOG_IT, fname);
            return(ERR_NO_JOB);
        default:

            if (!(jData->jStatus & JOB_STAT_ZOMBIE)
                && (UNREACHABLE (jData->hPtr[0]->hStatus))) {
                jData->newReason = EXIT_KILL_ZOMBIE;
                jData->jStatus |= JOB_STAT_ZOMBIE;
                inZomJobList (jData, FALSE);
                jStatusChange (jData, JOB_STAT_EXIT, LOG_IT, fname);
            }
    }

    return (ERR_NO_ERROR);
}

static void
sndJobMsgs (struct hData *hData, int *sigcnt)
{
    static char fname[] = "sndJobMsgs";
    int sndrc;
    int num = 0, maxSent = 1;
    struct bucket *bucket, *next, *head = hData->msgq[MSG_STAT_QUEUED];

    for (bucket = head->forw; bucket != head
             && num < maxSent; bucket = next) {
        next = bucket->forw;
        if (logclass & (LC_TRACE | LC_SIGNAL))
            ls_syslog (LOG_DEBUG2, "%s: Send message to host %s",
                       fname, hData->host);
        sndrc = msgJob(bucket, NULL);
        if (sndrc == LSBE_NO_ERROR) {
            (*sigcnt)++;
            bucket->bufstat = MSG_STAT_SENT;
            QUEUE_REMOVE(bucket);
            QUEUE_APPEND(bucket, hData->msgq[MSG_STAT_SENT]);
        } else if (sndrc == LSBE_NO_JOB ||
                   sndrc == LSBE_JOB_FINISH) {
            QUEUE_REMOVE(bucket);
            log_jobmsgack(bucket);
            chanFreeStashedBuf_(bucket->storage);
            FREE_BUCKET(bucket);
        } else {


            break;
        }
    }
}

static void
breakCallback(struct jData *jData, bool_t termWhiPendStatus)
{
    int pid, s;
    struct hostent *hp;
    struct sockaddr_in from;
    int len;
    static char fname[] = "breakCallback";

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Enter ...", fname);

    if ((pid = fork())) {
        if (pid < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fork",
                      lsb_jobid2str(jData->jobId));
        return;
    }


    len = sizeof(from);

    if ((hp = Gethostbyname_(jData->shared->jobBill.fromHost)) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: gethostbyname() %s failed job %s", __func__,
                  jData->shared->jobBill.fromHost,
                  lsb_jobid2str(jData->jobId));
        exit(-1);
    }

    memcpy((char *) &from.sin_addr, (char *) hp->h_addr, (int) hp->h_length);
    from.sin_family = AF_INET;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG, "\
%s: Nios on %s port %d jobId %s exitStatus %x %d", fname,
                  jData->shared->jobBill.fromHost,
                  jData->shared->jobBill.niosPort,
                  lsb_jobid2str(jData->jobId),
                  jData->exitStatus, termWhiPendStatus);
    }

    if ((s = niosCallback_(&from,
                           htons(jData->shared->jobBill.niosPort),
                           -jData->jobId,
                           jData->exitStatus,
                           termWhiPendStatus)) == -1) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "\
%s: Job %s niosCallback_ failed: %M", fname, lsb_jobid2str(jData->jobId));
        exit(-1);
    }

    exit(0);
}


int
statusMsgAck(struct statusReq *statusReq)
{
    static char             fname[] = "statusMsgAck";
    struct jData           *jp;
    struct bucket          *bucket;
    struct bucket          *msgQHead;
    int                     found;

    LSBMSG_DECL(header, jmsg);
    LSBMSG_INIT(header, jmsg);

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering ...", fname);



    if ((jp = getJobData(statusReq->jobId)) == NULL) {
        return LSBE_NO_JOB;
    }

    bucket = msgQHead = jp->hPtr[0]->msgq[MSG_STAT_QUEUED];
    found = FALSE;
    if (bucket->forw == bucket)
        goto QueueSent;
    else
        bucket = bucket->forw;

    do {
        if (bucket->proto.jobId == statusReq->jobId &&
            bucket->proto.msgId == statusReq->msgId) {

            found = TRUE;
            break;
        }
        bucket = bucket->forw;
    } while (bucket != msgQHead);

QueueSent:
    if (! found) {

        bucket = msgQHead = jp->hPtr[0]->msgq[MSG_STAT_SENT];
        if (bucket->forw == bucket)
            goto EndSearch;
        else
            bucket = bucket->forw;

        do {
            if (bucket->proto.jobId == statusReq->jobId &&
                bucket->proto.msgId == statusReq->msgId) {

                found = TRUE;
                break;
            }
            bucket = bucket->forw;
        } while (bucket != msgQHead);
    }

EndSearch:
    if (!found) {
        return LSBE_NO_JOBMSG;
    }

    if (found) {
        log_jobmsgack(bucket);
        QUEUE_REMOVE(bucket);
        chanFreeStashedBuf_(bucket->storage);
        FREE_BUCKET(bucket);
    }

    return (LSBE_NO_ERROR);
}

static int
rUsagesOk (struct resVal *jobResVal, struct resVal *queueResVal)
{

    int    ldx, jobBitSet, queueBitSet;

    if (jobResVal == NULL || queueResVal == NULL) {
        return (TRUE);
    }

    if (queueResVal->duration < jobResVal->duration
        && jobResVal->duration != INFINIT_INT) {
        return (FALSE);
    }

    if (queueResVal->duration == jobResVal->duration &&
        queueResVal->decay > jobResVal->decay) {
        return (FALSE);
    }

    for (ldx = 0; ldx < allLsInfo->numIndx; ldx++) {
        TEST_BIT(ldx, jobResVal->rusgBitMaps, jobBitSet);
        TEST_BIT(ldx, queueResVal->rusgBitMaps, queueBitSet);
        if (!jobBitSet || !queueBitSet)
            continue;



        if (jobResVal->val[ldx] <= queueResVal->val[ldx])
            continue;
        return (FALSE);
    }
    return (TRUE);
}

static void
packJobThresholds(struct thresholds *thresholds, struct jData *jData)
{
    static char fname[] = "packJobThresholds";
    int i, numThresholds = 0;

    if (jData == NULL || jData->qPtr == NULL)
        return;
    thresholds->nIdx = allLsInfo->numIndx;
    thresholds->loadSched =
        (float **)my_calloc(jData->numHostPtr, sizeof (float *), fname);
    thresholds->loadStop =
        (float **)my_calloc(jData->numHostPtr, sizeof (float *), fname);

    for (i = 0; i < jData->numHostPtr; i++) {
        if (i > 0 && jData->hPtr[i] == jData->hPtr[i-1])
            continue;
        thresholds->loadSched[numThresholds] =
            (float *)my_calloc(allLsInfo->numIndx, sizeof(float), fname);
        thresholds->loadStop[numThresholds] =
            (float *)my_calloc(allLsInfo->numIndx, sizeof(float), fname);

        assignLoad(thresholds->loadSched[numThresholds],
                   thresholds->loadStop[numThresholds], jData->qPtr, jData->hPtr[i]);
        numThresholds++;
    }
    thresholds->nThresholds = numThresholds;

}

static void
initSubmitReq(struct submitReq *jobBill)
{
    int i;

    jobBill->jobName = NULL;
    jobBill->queue = NULL;
    jobBill->resReq = NULL;
    jobBill->hostSpec = NULL;
    jobBill->dependCond = NULL;
    jobBill->inFile = NULL;
    jobBill->outFile = NULL;
    jobBill->errFile = NULL;
    jobBill->command = NULL;
    jobBill->inFileSpool = NULL;
    jobBill->commandSpool = NULL;
    jobBill->chkpntDir = NULL;
    jobBill->preExecCmd = NULL;
    jobBill->postExecCmd = NULL;
    jobBill->mailUser = NULL;
    jobBill->projectName = NULL;
    jobBill->cwd = NULL;
    jobBill->subHomeDir = NULL;
    jobBill->fromHost = NULL;
    jobBill->askedHosts = NULL;
    jobBill->xf = NULL;
    jobBill->jobFile = NULL;
    jobBill->loginShell = NULL;
    jobBill->schedHostType = NULL;

    jobBill->options = 0;
    jobBill->numAskedHosts = 0;
    for (i = 0; i < LSF_RLIM_NLIMITS; i++)
        jobBill->rLimits[i] = -1;


    jobBill->numProcessors = 1;
    jobBill->beginTime = 0;
    jobBill->termTime = 0;
    jobBill->sigValue = 0;
    jobBill->chkpntPeriod = 0;
    jobBill->restartPid = 0;
    jobBill->nxf = 0;
    jobBill->submitTime = 0;
    jobBill->umask = umask(0077);
    jobBill->niosPort = 0;
    jobBill->maxNumProcessors = 1;
    jobBill->userPriority = -1;
}


int
shouldLockJob (struct jData *jData, int newStatus)
{
    int duration = INFINIT_INT, resBitMaps = 0;

    if (newStatus & JOB_STAT_USUSP)
        return (TRUE);

    if (jData->shared->resValPtr)
        getReserveParams (jData->shared->resValPtr, &duration, &resBitMaps);

    if (jData->qPtr->resValPtr)
        getReserveParams (jData->qPtr->resValPtr, &duration, &resBitMaps);

    if (duration - jData->runTime > 0 && resBitMaps != 0)
        return (TRUE);

    return (FALSE);
}

static void
getReserveParams (struct resVal *resValPtr, int *duration, int *rusgBitMaps)
{
    int i;

    if (resValPtr) {
        if (resValPtr->duration != INFINIT_INT
            && resValPtr->duration < *duration)
            *duration = resValPtr->duration;
        for (i = 0; i < GET_INTNUM(allLsInfo->nRes); i++)
            *rusgBitMaps += resValPtr->rusgBitMaps[i];
    }
}

void
tryResume(void)
{
    char fname[] = "tryResume";
    struct jData *jp;
    int resumeSig;
    int returnCode;
    int found;
    struct sbdNode *sbdPtr;
    struct jData *next;
    struct hData *hPtr;

    if (logclass & (LC_SCHED | LC_EXEC))
        ls_syslog (LOG_DEBUG1, "%s: Enter this routinue....", fname);

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {

        hPtr->flags &= ~HOST_JOB_RESUME;
    }

    for (jp = jDataList[SJL]->back; jp != jDataList[SJL]; jp = next) {
        next = jp->back;

        if (jp->jStatus & JOB_STAT_UNKWN)
            continue;
        if (!IS_SUSP(jp->jStatus))
            continue;

        if (jp->hPtr[0]->flags & HOST_JOB_RESUME)
            continue;

        if (logclass & (LC_EXEC))
            ls_syslog(LOG_DEBUG3, "%s: job=%s jStatus=%x reason=%x subreason=%d actPid=%d>", fname, lsb_jobid2str(jp->jobId), jp->jStatus, jp->newReason, jp->subreasons, jp->actPid);

        if (!(jp->jStatus & JOB_STAT_SSUSP)
            || !(jp->newReason & SUSP_MBD_LOCK)
            || (jp->actPid != 0))
            continue;

        if ((jp->jFlags & JFLAG_SEND_SIG) && nSbdConnections > 0) {

            for (sbdPtr = sbdNodeList.forw; sbdPtr != &sbdNodeList;
                 sbdPtr = sbdPtr->forw) {
                if (sbdPtr->jData != jp)
                    continue;
                found = TRUE;
                break;
            }
            if (found == TRUE) {
                jp->hPtr[0]->flags |= HOST_JOB_RESUME;
                continue;
            }
        }
        if (jp->jStatus & JOB_STAT_RESERVE) {
            updResCounters (jp, jp->jStatus & ~JOB_STAT_RESERVE);
            jp->jStatus &= ~JOB_STAT_RESERVE;
        }
        resumeSig = 0;
        if ((returnCode = shouldResume (jp, &resumeSig)) != CANNOT_RESUME) {

            if (!(jp->jStatus & JOB_STAT_RESERVE)) {
                if (logclass & (LC_EXEC))
                    ls_syslog(LOG_DEBUG3, "%s: job <%s> updRes - <%d> slots <%s:%d>", fname, lsb_jobid2str(jp->jobId), jp->numHostPtr, __FILE__,  __LINE__);

                updResCounters (jp, jp->jStatus | JOB_STAT_RESERVE);
                jp->jStatus |= JOB_STAT_RESERVE;
            }

            if (returnCode == RESUME_JOB) {
                sigStartedJob (jp, resumeSig, 0, 0);
                jp->jFlags |= JFLAG_SEND_SIG;
                jp->hPtr[0]->flags |= HOST_JOB_RESUME;

                adjLsbLoad (jp, TRUE, TRUE);
                if (logclass & (LC_EXEC))
                    ls_syslog (LOG_DEBUG2, "%s: Resume job <%s> with signal value <%d>", fname, lsb_jobid2str(jp->jobId), resumeSig);
            } else {
                if (returnCode == PREEMPT_FOR_RES) {
                    reservePreemptResourcesForHosts(jp);
                }
                if (logclass & (LC_EXEC))
                    ls_syslog (LOG_DEBUG2, "%s: Slot of job <%s> is reserved", fname, lsb_jobid2str(jp->jobId));
            }
        }
    }

}
static int
shouldResume (struct jData *jp, int *resumeSig)
{
    static char fname[] = "shouldResume";
    int saveReason, saveSubReasons, returnCode = RESUME_JOB;

    if (logclass & (LC_EXEC))
        ls_syslog(LOG_DEBUG3, "%s: job=%s; jStatus=%x; reasons=%x, subreason=%d, numHosts=%d", fname, lsb_jobid2str(jp->jobId), jp->jStatus, jp->newReason, jp->subreasons, jp->numHostPtr);

    if (   jp->jFlags & JFLAG_URGENT_NOSTOP
           && *resumeSig == SIG_RESUME_USER) {
        jp->newReason = 0;
        jp->newReason |= SUSP_USER_RESUME;
        *resumeSig = 0;
        return RESUME_JOB;
    }

    saveReason = jp->newReason;
    saveSubReasons = jp->subreasons;



    if ((IS_SUSP(jp->jStatus)) && (jp->newReason & SUSP_RES_LIMIT) &&
        !(jp->subreasons & SUB_REASON_RUNLIMIT) ) {
        return CANNOT_RESUME;
    }


    if (!IS_SUSP(jp->jStatus) || (jp->jStatus & JOB_STAT_RESERVE))
        return CANNOT_RESUME;


    if (!(jp->newReason & SUSP_MBD_LOCK)) {
        if (shouldLockJob (jp, jp->jStatus) == TRUE) {

            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6535,
                                             "%s: The job<%s> status is <%x> and reasons is <%x>"), /* catgets 6535 */
                      fname,
                      lsb_jobid2str(jp->jobId),
                      jp->jStatus,
                      jp->newReason);
            jp->newReason |= SUSP_MBD_LOCK;
        } else {

            return CANNOT_RESUME;
        }
    }
    if (jp->jFlags & JFLAG_URGENT) {
        if (*resumeSig == SIG_RESUME_USER) {
            jp->newReason = 0;
            jp->newReason |= SUSP_USER_RESUME;
            *resumeSig = 0;
        }
    } else {
        *resumeSig = 0;
    }


    if (jp->qPtr->qStatus & QUEUE_STAT_RUNWIN_CLOSE) {
        jp->newReason |= SUSP_QUEUE_WINDOW;
        return CANNOT_RESUME;
    } else {
        if (*resumeSig == 0 && (jp->newReason & SUSP_QUEUE_WINDOW))
            *resumeSig = SIG_RESUME_WINDOW;
        if ((jp->newReason & ~(SUSP_QUEUE_WINDOW | SUSP_MBD_LOCK)))
            jp->newReason &= ~SUSP_QUEUE_WINDOW;
    }


    if (jp->subreasons & SUB_REASON_RUNLIMIT) {

        if ( (jp->qPtr->rLimits[LSF_RLIMIT_RUN] == -1) ||
             (jp->qPtr->rLimits[LSF_RLIMIT_RUN]
              > (jp->runTime * jp->hPtr[0]->cpuFactor)) ) {
            if (*resumeSig == 0) {
                *resumeSig = SIG_RESUME_OTHER;
            }

            jp->subreasons &= ~SUB_REASON_RUNLIMIT;
        } else {
            return CANNOT_RESUME;
        }
    }


    saveReason = jp->newReason;
    saveSubReasons = jp->subreasons;
    if (shouldResumeByLoad (jp) == FALSE)

        return CANNOT_RESUME;
    else {
        jp->newReason = saveReason;
        jp->subreasons = saveSubReasons;
        if (*resumeSig == 0 && (jp->newReason & LOAD_REASONS))
            *resumeSig = SIG_RESUME_LOAD;
        if ((jp->newReason & ~(SUSP_MBD_LOCK | LOAD_REASONS))) {
            jp->newReason &= ~LOAD_REASONS;
            jp->subreasons = 0;
        }
    }
    if (jp->shared->resValPtr != NULL || jp->qPtr->resValPtr != NULL) {
        int resReturnCode = shouldResumeByRes(jp);
        if (resReturnCode == CANNOT_RESUME) {
            jp->newReason |= SUSP_RES_RESERVE;
            return CANNOT_RESUME;
        } else  if (resReturnCode == PREEMPT_FOR_RES) {
            if (!(jp->newReason & ~SUSP_MBD_LOCK))
                jp->newReason |= SUSP_RES_RESERVE;

            returnCode = PREEMPT_FOR_RES;
        } else {
            if (*resumeSig == 0 && (jp->newReason & SUSP_RES_RESERVE))
                *resumeSig = SIG_RESUME_OTHER;
            if (jp->newReason & ~(SUSP_RES_RESERVE | SUSP_MBD_LOCK))
                jp->newReason &= ~SUSP_RES_RESERVE;
        }
    }
    return returnCode;
}

static int
shouldResumeByLoad (struct jData *jp)
{
    static char fname[] = "shouldResumeByLoad";
    int resume, i, j, numHosts, lastReason = jp->newReason;
    struct tclHostData *tclHostData;
    struct thresholds thresholds;
    struct hostLoad *loads;

    if (logclass & (LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG3, "%s: job=%s; jStatus=%x; reasons=%x, subreasons=%d, numHosts=%d", fname, lsb_jobid2str(jp->jobId), jp->jStatus, jp->newReason, jp->subreasons, jp->numHostPtr);


    if (!IS_SUSP (jp->jStatus))
        return FALSE;

    packJobThresholds(&thresholds, jp);
    numHosts = thresholds.nThresholds;
    loads = (struct hostLoad *)
        my_malloc (numHosts * sizeof(struct hostLoad), fname);
    if (jp->qPtr->resumeCondVal != NULL)
        tclHostData = (struct tclHostData *)
            my_malloc (numHosts * sizeof (struct tclHostData), fname);
    else
        tclHostData = NULL;


    j = 0;
    for (i = 0; i < jp->numHostPtr; i++) {
        if (i > 0 && jp->hPtr[i] == jp->hPtr[i-1])
            continue;

        if (jp->hPtr[i]->flags & HOST_LOST_FOUND) {
            loads[j].li = NULL;
            continue;
        }

        if (!(jp->hPtr[i]->flags & HOST_UPDATE_LOAD)) {

            freeThresholds (&thresholds);
            if (tclHostData != NULL) {
                for (i = 0; i < j; i++)
                    freeTclHostData (&tclHostData[i]);
                FREEUP (tclHostData);
            }
            FREEUP (loads);
            return FALSE;
        }
        strcpy(loads[j].hostName, jp->hPtr[i]->host);
        loads[j].li = jp->hPtr[i]->lsfLoad;
        loads[j].status = jp->hPtr[i]->limStatus;
        if (tclHostData != NULL)
            getTclHostData (&tclHostData[j], jp->hPtr[i], NULL);
        j++;
    }
    jp->newReason &= ~SUSP_MBD_LOCK;
    resume = checkResumeByLoad (jp->jobId, j, thresholds, loads,
                                &jp->newReason,
                                &jp->subreasons, jp->qPtr->qAttrib,
                                jp->qPtr->resumeCondVal, tclHostData);
    if (lastReason & SUSP_MBD_LOCK)
        jp->newReason |= SUSP_MBD_LOCK;
    freeThresholds (&thresholds);
    if (tclHostData != NULL) {
        for (i = 0; i < j; i++)
            freeTclHostData (&tclHostData[i]);
        FREEUP (tclHostData);
    }
    FREEUP (loads);

    if (resume == FALSE)
        return FALSE;
    return TRUE;
}


static int
shouldResumeByRes (struct jData *jp)
{
    static char fname[] = "shouldResumeByRes";
    int i, j, returnCode = RESUME_JOB;
    struct  resVal *resValPtr;
    float **loads;
    int *hBitMaps = NULL;
    hEnt  *ent = NULL;

    if (logclass & (LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG3, "%s: job=%s; jStatus=%x; reasons=%x, subreasons=%d, numHosts=%d", fname, lsb_jobid2str(jp->jobId), jp->jStatus, jp->newReason, jp->subreasons, jp->numHostPtr);

    if (!IS_SUSP (jp->jStatus))
        return RESUME_JOB;

    if (jp->jStatus & JOB_STAT_RESERVE)
        return CANNOT_RESUME;
    if ((resValPtr
         = getReserveValues (jp->shared->resValPtr, jp->qPtr->resValPtr)) == NULL)
        return RESUME_JOB;

    if (resValPtr->duration != INFINIT_INT
        && (resValPtr->duration - jp->runTime <= 0)
        && !isReservePreemptResource(resValPtr)) {
        return RESUME_JOB;
    }

    /*save the original load/resource values*/
    loads = (float **) my_malloc (jp->numHostPtr * sizeof (float *), fname);
    for (i = 0; i < jp->numHostPtr; i++) {
        loads [i] = (float *) my_malloc
            ((allLsInfo->numIndx + jp->hPtr[i]->numInstances) * sizeof (float),
             fname);
        for (j = 0; j < allLsInfo->numIndx; j++)
            loads[i][j] = jp->hPtr[i]->lsbLoad[j];
        for (j = 0; j < jp->hPtr[i]->numInstances; j++)
            loads[i][allLsInfo->numIndx +j] =
                atof (jp->hPtr[i]->instances[j]->value);
    }

    /*temporary consume the resource from the host*/
    adjLsbLoad (jp, TRUE, TRUE);

    FORALL_PRMPT_RSRCS(j) {
        float val;
        GET_RES_RSRC_USAGE(j, val, jp->shared->resValPtr,
                           jp->qPtr->resValPtr);
        if (val <= 0.0)
            continue;

        for (i = 0;
             i == 0 || (slotResourceReserve && i < jp->numHostPtr);
             i++) {
            struct resourceInstance *instancePtr;
            float rVal;
            rVal = getUsablePRHQValue(j,jp->hPtr[i],jp->qPtr,&instancePtr);
            if (!JOB_PREEMPT_WAIT(jp)) {
                rVal -= getReservedByWaitPRHQValue(j,jp->hPtr[i],jp->qPtr);
            }
            if (rVal == INFINIT_LOAD || rVal == -INFINIT_LOAD) {
                returnCode = CANNOT_RESUME;
                break;
            } else if (rVal < 0.0) {
                returnCode = CANNOT_RESUME;
                break;
            }
        }
        if (returnCode == CANNOT_RESUME)
            break;
    } ENDFORALL_PRMPT_RSRCS;

    /*exam whether resources is enrough to resume the job*/
    hBitMaps = (int *)my_calloc(GET_INTNUM(numofhosts()), sizeof(int), fname);
    for (i = 0; i < jp->numHostPtr && returnCode != CANNOT_RESUME; i++) {
        int isSet;

        if (jp->hPtr[i]->flags & HOST_LOST_FOUND) {
            continue;
        }

        TEST_BIT(jp->hPtr[i]->hostId, hBitMaps, isSet);
        if (isSet) {
            /*The host has been checked, we don't need validate load/resource again*/
            continue;
        } else {
            SET_BIT(jp->hPtr[i]->hostId, hBitMaps);
        }
    
        for (j = 0; j < allLsInfo->numIndx; j++) {
            if (jp->hPtr[i]->lsbLoad[j] < 0.0
                || (j == UT && jp->hPtr[i]->lsbLoad[j] > 1.0)) {
                returnCode = CANNOT_RESUME;
                break;
            }
        }

        if (i > 0 && !slotResourceReserve) {
            continue;
        }
        for (j = 0; j < jp->hPtr[i]->numInstances; j++) {
            if (atof (jp->hPtr[i]->instances[j]->value) < 0.0 &&
                !isItPreemptResourceName(jp->hPtr[i]->instances[j]->resName)) {
                if( !rusgMatch(resValPtr, jp->hPtr[i]->instances[j]->resName) ){
                    continue;
                }
                returnCode = CANNOT_RESUME;
                break;
            }
        }
    }
    FREEUP(hBitMaps);

    /*recover the value*/
    for (i = 0; i < jp->numHostPtr; i++) {
        char loadString[MAXLSFNAMELEN];
        for (j = 0; j < allLsInfo->numIndx; j++)
            jp->hPtr[i]->lsbLoad[j] = loads[i][j];
        for (j = 0; j < jp->hPtr[i]->numInstances; j++) {
            FREEUP (jp->hPtr[i]->instances[j]->value);
            sprintf (loadString, "%-10.1f", loads[i][allLsInfo->numIndx+j]);
            jp->hPtr[i]->instances[j]->value = safeSave (loadString);
        }
        FREEUP(loads[i]);
    }
    FREEUP (loads);

    return (returnCode);

}

static void
rLimits2lsfLimits(int *pRLimits, struct lsfLimit *pLsfLimits, int i, int soft)
{
    switch (i) {
        case LSF_RLIMIT_FSIZE:
        case LSF_RLIMIT_DATA:
        case LSF_RLIMIT_STACK:
        case LSF_RLIMIT_CORE:
        case LSF_RLIMIT_RSS:
        case LSF_RLIMIT_VMEM:

            if (soft) {
                pLsfLimits[i].rlim_curh = (unsigned int)pRLimits[i] >> 22;
                pLsfLimits[i].rlim_curl = (unsigned int)pRLimits[i] << 10;
            } else {
                pLsfLimits[i].rlim_maxh = (unsigned int)pRLimits[i] >> 22;
                pLsfLimits[i].rlim_maxl = (unsigned int)pRLimits[i] << 10;
            }
            break;
        default:

            if (soft) {
                pLsfLimits[i].rlim_curh = 0;
                pLsfLimits[i].rlim_curl = pRLimits[i];
            } else {
                pLsfLimits[i].rlim_maxh = 0;
                pLsfLimits[i].rlim_maxl = pRLimits[i];
            }
    }
}

int
PJLorMJL(struct jData *job)
{
    struct jData *jp;

    for (jp = jDataList[MJL]->back; jp != jDataList[MJL]; jp = jp->back) {
        if (jp == job) {
            return MJL;
        }
    }
    return PJL;
}


struct jData *
createjDataRef (struct jData *jp)
{
    if (jp) {
        jp->numRef++;
    }
    return(jp);
}

void
destroyjDataRef(struct jData *jp)
{
    if (jp) {
        jp->numRef--;
        if ((jp->jStatus & JOB_STAT_VOID) && (jp->numRef <= 0)) {

            voidJobList = listSetDel((long)jp, voidJobList);
            FREEUP(jp);
        }
    }
    return;
}


bool_t
runJob(struct runJobRequest*  request, struct lsfAuth *auth)
{
    static char       fname[] = "runJob";
    struct jData*     job;
    int               cc;
    struct candHost   candHost;
    struct candHost*  candidateHostPtr;

    ls_syslog(LOG_DEBUG, "%s: Received request to run a job <%s>",
              fname, lsb_jobid2str(request->jobId));


    memset((struct candHost *)&candHost, 0, sizeof(struct candHost));
    candidateHostPtr = &candHost;


    job = getJobData(request->jobId);
    if (job == NULL ) {
        ls_syslog(LOG_DEBUG, "%s: No matching job found %s",
                  fname, lsb_jobid2str(request->jobId));
        return(LSBE_NO_JOB);
    }


    if (auth)
    {
        if (   auth->uid != 0
               && isAuthManager(auth) == FALSE
               && isAuthQueAd(job->qPtr, auth) == FALSE)
        {
            ls_syslog(LOG_DEBUG, "\
%s: user <%d> is not permitted to issue brun command for job <%s>",
                      fname, auth->uid, lsb_jobid2str(job->jobId));
            return(LSBE_PERMISSION);
        }
    }


    if (job->nodeType != JGRP_NODE_JOB)
        return(LSBE_JOB_ARRAY);


    if (job->jStatus & JOB_STAT_SSUSP || job->jStatus & JOB_STAT_USUSP)
        return(LSBE_JOB_SUSP);


    if (IS_START(job->jStatus))
        return(LSBE_JOB_STARTED);


    if ((request->options & RUNJOB_OPT_FROM_BEGIN)
        && (!(job->shared->jobBill.options & SUB_CHKPNTABLE))) {
        return(LSBE_J_UNCHKPNTABLE);
    }

    if (IS_FINISH(job->jStatus)) {
        if (request->options &  RUNJOB_OPT_PENDONLY) {
            return (LSBE_JOB_FINISH);
        }


        handleRequeueJob(job, time(0));
        log_jobrequeue(job);
    }

    if (job->jStatus & JOB_STAT_RESERVE)
        freeReserveSlots(job);

    if (JOB_PREEMPT_WAIT(job))
        freeReservePreemptResources(job);


    cc = setUrgentJobExecHosts(request, job);
    if (cc != LSBE_NO_ERROR) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "setUrgentJobExecHosts");
        return(cc);
    }


    job->jFlags |= (request->options & RUNJOB_OPT_NOSTOP) ?
        JFLAG_URGENT_NOSTOP : JFLAG_URGENT;


    if (request->options & RUNJOB_OPT_FROM_BEGIN) {
        job->jStatus &= ~JOB_STAT_CHKPNTED_ONCE;
        job->shared->jobBill.options &= ~(SUB_RESTART | SUB_RESTART_FORCE);
    }


    log_jobForce(job, auth->uid, auth->lsfUserName);


    lsberrno = LSBE_NO_ERROR;
    job->newReason = 0;

    cc = dispatch_it(job);
    if (cc == TRUE) {
        ls_syslog(LOG_DEBUG, "%s: job <%s> dispatched succesfully to host %s",
                  fname, lsb_jobid2str(request->jobId), request->hostname[0]);
        cc = LSBE_NO_ERROR;
        if (job->qPtr->acceptIntvl > 0) {
            int hostId = job->hPtr[0]->hostId;
            hReasonTb[1][hostId] = PEND_HOST_ACCPT_ONE;
        }
    } else {
        ls_syslog(LOG_DEBUG, "%s: job <%s> failed to be dispatched to host %s",
                  fname, lsb_jobid2str(request->jobId), request->hostname[0]);
        cc = lsberrno;


        FREEUP(job->hPtr);
        job->numHostPtr = 0;
    }

    return(cc);
}

static int
setUrgentJobExecHosts(struct runJobRequest*    request,
                      struct jData*            job)
{
    static char           fname[] = "setUrgentJobExecHosts()";
    struct hData*         host;
    int                   i;
    int                   j;
    int                   max;
    int                   min;
    int                   numUniqueEntries;
    char                  **reqHosts;
    int                   numHostSlots;

    max = job->shared->jobBill.maxNumProcessors;
    min = job->shared->jobBill.numProcessors;


    reqHosts = (char **)my_calloc(request->numHosts,
                                  sizeof(char *), fname);

    numUniqueEntries = 0;
    for (i = 0; i < request->numHosts; i++) {
        char        *host = request->hostname[i];
        bool_t      uniqueEntry = TRUE;


        for (j = 0; j < numUniqueEntries; j++) {
            if (strcmp(host, reqHosts[j]) == 0) {
                uniqueEntry = FALSE;
                break;
            }
        }

        if (uniqueEntry == TRUE)
            reqHosts[numUniqueEntries++] = host;
    }


    numHostSlots = 0;
    for (i = 0; i < numUniqueEntries; i++) {
        struct hData *hPtr;

        hPtr = getHostData(reqHosts[i]);
        if (   hPtr == NULL
               || (hPtr != NULL && (hPtr->hStatus & HOST_STAT_REMOTE)) )
        {
            FREEUP(reqHosts);
            return (LSBE_BAD_HOST);
        }

        if (hPtr != NULL && (hPtr->hStatus & HOST_STAT_LOCKED_MASTER)) {
            FREEUP(reqHosts);
            return (LSBE_LOCKED_MASTER);
        }
        numHostSlots += hPtr->numCPUs;
    }


    if (max == min) {
        if (numHostSlots < max) {
            ls_syslog(LOG_DEBUG,"\
%s: Wrong number of processors <%d> has been requested for the parallel\
 job <%s>", fname, request->numHosts, lsb_jobid2str(job->jobId));
            FREEUP(reqHosts);
            return(LSBE_PROC_NUM);
        }
    } else {
        if (!(numHostSlots >= min && numHostSlots <= max)) {
            ls_syslog(LOG_DEBUG,"\
%s: Wrong number of processors <%d> has been requested for the parallel\
job <%s>", fname, request->numHosts, lsb_jobid2str(job->jobId));
            FREEUP(reqHosts);
            return(LSBE_PROC_NUM);
        }
    }



    numHostSlots = MIN(numHostSlots, max);

    FREEUP(job->hPtr);
    job->hPtr = (struct hData **)my_calloc(numHostSlots,
                                           sizeof(struct hData *),
                                           fname);
    job->numHostPtr = 0;


    for (i = 0; i < numUniqueEntries; i++) {
        int numCPUNeeded;

        host = getHostData(reqHosts[i]);

        if (job->numHostPtr + host->numCPUs > numHostSlots)
            numCPUNeeded = numHostSlots - job->numHostPtr;
        else
            numCPUNeeded = host->numCPUs;

        for (j = 0; j < numCPUNeeded; j++)
            job->hPtr[job->numHostPtr++] = host;

    }

    FREEUP(reqHosts);
    return(LSBE_NO_ERROR);

}


static void
replaceString (char *s1, char *s2, char *s3)
{
    char temp[MAXFILENAMELEN];
    char *last;
    char *next;


    temp[0]='\0';
    last=s1;
    while ((next=(char *)strstr(last, s2)) != NULL) {
        strncat(temp, last, next-last);
        strcat(temp, s3);
        last = next + strlen(s2);
    }
    strcat(temp, last);
    strcpy(s1, temp);

}

void
expandFileNameWithJobId (char *out, char *in, LS_LONG_INT jobId)
{

    char   jobIdStr[16];
    char   indexStr[16];


    sprintf(jobIdStr, "%d", LSB_ARRAY_JOBID(jobId));
    sprintf(indexStr, "%d", LSB_ARRAY_IDX(jobId));

    strcpy(out, in);


    replaceString(out, "%J", jobIdStr);


    replaceString(out, "%I", indexStr);


}

char *
jDataSprintf(LIST_ENTRY_T *extra, void *hint)
{
    static char strBuf[32];
    struct jData *job = (struct jData *)extra;

    sprintf(strBuf, "<%s>", (job->jobId >= 0) ? lsb_jobid2str(job->jobId) : "-1");
    return (strBuf);

}

void
setJobPendReason(struct jData *jp, int pendReason)
{
    if (!jp)
        return;

    jp->newReason = pendReason;
    return;
}

void
cleanCandHosts (struct jData *job)
{
    if (!IS_PEND(job->jStatus))
        return;

    job->jFlags &= ~JFLAG_READY;
    job->processed = 0;
    FREE_CAND_PTR(job);
    job->numCandPtr = 0;
    job->usePeerCand = FALSE;

    FREE_ALL_GRPS_CAND(job);

}

static void
initJobSig (struct jData *jData, struct jobSig *jobSig, int sigValue,
            time_t chkPeriod, int actFlags)
{
    jobSig->jobId = jData->jobId;
    jobSig->sigValue = sigValue;
    jobSig->chkPeriod = chkPeriod;
    jobSig->actFlags = actFlags;
    jobSig->actCmd = NULL;
    jobSig->reasons = 0;
    jobSig->subReasons = 0;
    jobSig->newJobId = 0;
}

static bool_t
isSignificantChange(struct jRusage *runRusage1, struct jRusage *runRusage2,
                    float diff)
{
    float df;

    df = ((runRusage1->utime + runRusage1->stime) == 0) ?
        (runRusage2->utime + runRusage2->stime) :
        ABS(((runRusage1->utime + runRusage1->stime) -
             (runRusage2->utime + runRusage2->stime)) /
            (runRusage1->utime + runRusage1->stime));

    if (df >= diff)
        return(TRUE);

    df = (runRusage1->mem == 0 ) ? runRusage2->mem :
        ABS((runRusage1->mem - runRusage2->mem)/runRusage1->mem);

    if (df >= diff)
        return(TRUE);
    df = (runRusage1->swap  == 0 ) ? runRusage2->swap :
        ABS((runRusage1->swap - runRusage2->swap)/runRusage1->swap);
    if (df >= diff)
        return(TRUE);
    return(FALSE);
}


void modifyJobPriority(struct jData *jp, int subPriority)
{
    time_t curTime = time(0);
    time_t subTime = jp->shared->jobBill.submitTime;
    unsigned long timeIntvl = ( curTime - subTime) / 60;
    unsigned int newVal;

    if ( jobPriorityValue < 0 || jobPriorityTime < 0 ) {
        jp->jobPriority = subPriority;
    }
    else {
        newVal = subPriority
            + timeIntvl * jobPriorityValue/jobPriorityTime;
        jp->jobPriority = MIN(newVal, (unsigned int)MAX_JOB_PRIORITY);
    }

    return;

}


bool_t
checkUserPriority(struct jData *jp, int userPriority, int *errnum)
{
    int isAdmin = 0 ;

    if ( maxUserPriority <= 0 ) {
        *errnum = LSBE_NO_JOB_PRIORITY;
        return FALSE;
    }

    if ( userPriority <= 0 || userPriority > MAX_JOB_PRIORITY) {

        *errnum = LSBE_BAD_USER_PRIORITY;
        return FALSE;
    }

    if (isManager(jp->userName)) {
        isAdmin = 1;
    }

    isAdmin += isQueAd(jp->qPtr, jp->userName) ;

    if ( userPriority > maxUserPriority && !isAdmin) {
        *errnum = LSBE_BAD_USER_PRIORITY;
        return ( FALSE);
    }

    return TRUE;

}

static void
jobRequeueTimeUpdate(struct jData *jPtr, time_t t)
{
    jPtr->requeueTime = t;
}


static void
setClusterAdmin(bool_t admin)
{
    clusterAdminFlag = admin;
    return;
}

static bool_t
requestByClusterAdmin( )
{
    bool_t retVal = clusterAdminFlag;

    clusterAdminFlag = FALSE;
    return (retVal);
}


void
setNewSub(struct jData *jpbw, struct jData *job,
          struct submitReq *subReq,
          struct submitReq *modReq,
          int delOptions, int delOptions2 )
{
    struct submitReq *newSub;

    newSub = (struct submitReq *) my_malloc
        (sizeof (struct submitReq), "handleJParameters");

    copyJobBill (&(job->shared->jobBill), newSub, FALSE);

    if (jpbw->newSub) {
        freeSubmitReq (jpbw->newSub);
        FREEUP (jpbw->newSub);
    }
    jpbw->newSub = newSub;

    if ( delOptions2 & SUB2_JOB_PRIORITY && maxUserPriority>0 ) {

        newSub->userPriority = maxUserPriority/2 ;
    }
    else if ( modReq->options2 & SUB2_JOB_PRIORITY) {

        int error=0;
        if (checkUserPriority(jpbw, modReq->userPriority, &error)) {
            newSub->userPriority = modReq->userPriority;
        }
    }


    if ( delOptions & SUB_CHKPNT_DIR ) {

        FREEUP(jpbw->newSub->chkpntDir);
        newSub->options  &= ~ SUB_CHKPNT_DIR;
        newSub->options  &= ~ SUB_CHKPNT_PERIOD;
        newSub->options2 &= ~ SUB2_QUEUE_CHKPNT;

        newSub->chkpntDir = safeSave("");
    }
    else
        if ( modReq->options & SUB_CHKPNT_DIR) {

            FREEUP(newSub->chkpntDir);
            newSub->chkpntDir = safeSave(subReq->chkpntDir);
            if ( subReq->chkpntPeriod > 0 )
                newSub->chkpntPeriod = subReq->chkpntPeriod;
            newSub->options  |= SUB_CHKPNT_DIR;
            newSub->options  |= SUB_CHKPNT_PERIOD;
            newSub->options2 &= ~ SUB2_QUEUE_CHKPNT;
        }

    if ( delOptions & SUB_RERUNNABLE ) {
        newSub->options  &= ~ SUB_RERUNNABLE;
        newSub->options2 &= ~ SUB2_QUEUE_RERUNNABLE;
    }
    else
        if ( modReq->options & SUB_RERUNNABLE) {
            newSub->options  |= SUB_RERUNNABLE;
            newSub->options  &=  ~ SUB2_QUEUE_RERUNNABLE;
        }


    if (jpbw->qPtr != job->qPtr)  {
        if (!(delOptions & SUB_CHKPNT_DIR) &&
            !(modReq->options & SUB_CHKPNT_DIR) &&
            ((jpbw->shared->jobBill.options2 & SUB2_QUEUE_CHKPNT
              && jpbw->shared->jobBill.options & SUB_CHKPNT_DIR)
             || (!(jpbw->shared->jobBill.options2 & SUB2_QUEUE_CHKPNT)
                 &&!(jpbw->shared->jobBill.options & SUB_CHKPNT_DIR)))) {

            struct qData *qp = job->qPtr;


            newSub->options  &= ~ SUB_CHKPNT_DIR;
            newSub->options  &= ~ SUB_CHKPNT_PERIOD;
            newSub->options2 &= ~ SUB2_QUEUE_CHKPNT;
            newSub->chkpntPeriod = -1;
            FREEUP(newSub->chkpntDir);


            if ( qp->qAttrib & Q_ATTRIB_CHKPNT ) {
                char dir[MAXLINELEN];
                char jobIdStr[20];

                newSub->options  |= SUB_CHKPNT_DIR;
                newSub->options  |= SUB_CHKPNT_PERIOD;
                newSub->options2 |= SUB2_QUEUE_CHKPNT;
                strcpy(dir, qp->chkpntDir);
                sprintf(jobIdStr, "/%s", lsb_jobidinstr(jpbw->jobId));
                strcat(dir, jobIdStr);
                newSub->chkpntDir = safeSave(dir);
                newSub->chkpntPeriod = qp->chkpntPeriod;
            }
            else
                newSub->chkpntDir = safeSave("");
        }

        if ( !(delOptions & SUB_RERUNNABLE) &&
             !(modReq->options & SUB_RERUNNABLE) &&
             ((jpbw->shared->jobBill.options2 & SUB2_QUEUE_RERUNNABLE &&
               jpbw->shared->jobBill.options & SUB_RERUNNABLE)
              || (!(jpbw->shared->jobBill.options2 & SUB2_QUEUE_RERUNNABLE)
                  && !(jpbw->shared->jobBill.options & SUB_RERUNNABLE) ))) {
            struct qData *qp = job->qPtr;


            newSub->options  &= ~ SUB_RERUNNABLE;
            newSub->options2  &= ~ SUB2_QUEUE_RERUNNABLE;


            if ( qp->qAttrib & Q_ATTRIB_RERUNNABLE ) {
                newSub->options  |= SUB_RERUNNABLE;
                newSub->options2 |= SUB2_QUEUE_RERUNNABLE;
            }
        }
    }


}

#define RECV_JOBFILE_TIMEOUT    5
int
mbdRcvJobFile(int chfd, struct lenData *jf)
{
    static char fname[]="mbdRcvJobFile";
    int timeout = RECV_JOBFILE_TIMEOUT;
    int cc;

    jf->data = NULL;
    jf->len = 0;
    if ((cc = chanReadNonBlock_(chfd, NET_INTADDR_(&jf->len),
                                NET_INTSIZE_, timeout)) != NET_INTSIZE_) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "chanReadNonBlock_");
        return (-1);
    }
    jf->len = ntohl(jf->len);
    jf->data = my_calloc(1, jf->len, "mbdRcvJobFile");
    if ((cc = chanReadNonBlock_(chfd, jf->data, jf->len,
                                timeout)) != jf->len) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "chanReadNonBlock_");
        free(jf->data);
        return (-1);
    }

    return (0);
}

float
queueGetUnscaledRunTimeLimit(struct qData *qp)
{
    static char fname[] = "queueGetUnscaledRunTimeLimit";
    char *spec;
    float             *cpuFactor;
    float             one = 1;
    int qLimit = QUEUE_LIMIT(qp, LSF_RLIMIT_RUN);

    if (qLimit < 0) {
        return qLimit;
    }
    if (qp->hostSpec) {
        spec = qp->hostSpec;
    } else if (qp->defaultHostSpec) {
        spec = qp->defaultHostSpec;
    } else {
        ls_syslog(LOG_ERR,I18N(6542,
                               "%s: queue <%s> doesn't have a hostSpec."), /* catgets 6542 */
                  fname, qp->queue);
        spec = NULL;
    }
    if (spec != NULL) {
        if ((cpuFactor = getModelFactor(spec)) == NULL &&
            (cpuFactor = getHostFactor(spec)) == NULL ) {
            ls_syslog(LOG_ERR,I18N(6543,
                                   "%s: cannot get cpufactor of spec=(%s)"), /* catgets 6543 */
                      fname, spec);
            cpuFactor = &one;
        }
    } else {
        cpuFactor = &one;
    }

    return qLimit / (*cpuFactor);
}

static int
rusgMatch(struct resVal* resValPtr, const char *resName)
{
    int ldx;

    if (resValPtr == NULL || resName == NULL) {
        return (0);
    }

    for (ldx = allLsInfo->numIndx; ldx < allLsInfo->nRes; ldx++) {
        int isSet;

        if (NOT_NUMERIC(allLsInfo->resTable[ldx])) {
            continue;
        }
        TEST_BIT(ldx, resValPtr->rusgBitMaps, isSet);
        if (isSet == 0) {

            continue;
        }
        if ( 0 == strcmp(allLsInfo->resTable[ldx].name, resName) ) {
            return (1);
        }
    }
    return (0);
}



int
arrayRequeue(struct jData      *jArray,
             struct signalReq  *sigPtr,
             struct lsfAuth    *authPtr)
{
    static char       fname[] = "arrayRequeue";
    struct jData      *jPtr;
    time_t            requeueTime;
    int               requeueSuccess = FALSE;
    int               requeueReply  = LSBE_NO_ERROR;
    bool_t            requeueOneElement = FALSE;

    if (logclass & (LC_TRACE | LC_SIGNAL)) {
        ls_syslog(LOG_DEBUG,"\
%s: requeue array %s sig %d status %d options %d",
                  fname, lsb_jobid2str((*jArray).jobId),
                  (*sigPtr).sigValue, (int)(*sigPtr).chkPeriod,
                  (*sigPtr).actFlags);
    }

    requeueTime = time(NULL);

    if (jArray->nodeType == JGRP_NODE_ARRAY) {
        struct jgTreeNode    *jgTreePtr;
        struct jarray        *jArrayPtr;

        jgTreePtr = jArray->jgrpNode;
        jArrayPtr = ARRAY_DATA(jgTreePtr);


        if ( (jArrayPtr->counts[JGRP_COUNT_NRUN] == 0
              && (sigPtr->actFlags & REQUEUE_RUN))
             &&
             ( jArrayPtr->counts[JGRP_COUNT_NDONE] == 0
               && (sigPtr->actFlags & REQUEUE_DONE))
             &&
             ( jArrayPtr->counts[JGRP_COUNT_NEXIT] == 0
               && (sigPtr->actFlags & REQUEUE_EXIT))) {

            return(LSBE_EXCEPT_ACTION);
        }

        requeueOneElement = FALSE;
        jPtr = jArray->nextJob;

    } else {

        requeueOneElement = TRUE;
        jPtr = jArray;
    }


    do {

        if ( (jPtr->jStatus & JOB_STAT_DONE
              && sigPtr->actFlags & REQUEUE_DONE)
             ||
             (jPtr->jStatus & JOB_STAT_EXIT
              && sigPtr->actFlags & REQUEUE_EXIT)) {

            handleRequeueJob(jPtr, requeueTime);

            if (mSchedStage != M_STAGE_REPLAY) {
                log_signaljob(jPtr,
                              sigPtr,
                              authPtr->uid,
                              authPtr->lsfUserName);
            }

            jPtr->startTime = jPtr->endTime = 0;


            if (sigPtr->chkPeriod == JOB_STAT_PSUSP) {

                updJgrpCountByJStatus(jPtr, JOB_STAT_PEND, JOB_STAT_PSUSP);

                SET_STATE(jPtr->jStatus, JOB_STAT_PSUSP);
                setJobPendReason(jPtr, PEND_USER_STOP);
            } else {

                SET_STATE(jPtr->jStatus, JOB_STAT_PEND);
            }

            requeueSuccess = TRUE;

        } else if ( (IS_START(jPtr->jStatus)
                     || (jPtr->jStatus & JOB_STAT_UNKWN))
                    &&
                    (sigPtr->actFlags & REQUEUE_RUN)) {


            jPtr->pendEvent.sig       = SIG_KILL_REQUEUE;
            jPtr->pendEvent.sigDel    = DEL_ACTION_REQUEUE;
            jPtr->pendEvent.sig1Flags = sigPtr->chkPeriod;
            eventPending = TRUE;

            if (mSchedStage != M_STAGE_REPLAY) {
                log_signaljob(jPtr,
                              sigPtr,
                              authPtr->uid,
                              authPtr->lsfUserName);
            }
            requeueSuccess = TRUE;

        } else {
            if (sigPtr->actFlags & REQUEUE_RUN) {
                if (IS_PEND(jPtr->jStatus)) {
                    requeueReply = LSBE_NOT_STARTED;
                }
                if ((jPtr->jStatus & JOB_STAT_DONE) ||
                    (jPtr->jStatus & JOB_STAT_EXIT) ) {
                    requeueReply = LSBE_JOB_FINISH;
                }
            }
            if (sigPtr->actFlags & REQUEUE_DONE) {
                if (IS_PEND(jPtr->jStatus)) {
                    requeueReply = LSBE_NOT_STARTED;
                }
                if (jPtr->jStatus & JOB_STAT_RUN) {
                    requeueReply = LSBE_JOB_STARTED;
                }
                if (jPtr->jStatus & JOB_STAT_EXIT) {
                    requeueReply = LSBE_EXCEPT_ACTION;
                }
            }
            if (sigPtr->actFlags & REQUEUE_EXIT) {
                if (IS_PEND(jPtr->jStatus)) {
                    requeueReply = LSBE_NOT_STARTED;
                }
                if (jPtr->jStatus & JOB_STAT_RUN) {
                    requeueReply = LSBE_JOB_STARTED;
                }
                if (jPtr->jStatus & JOB_STAT_DONE) {
                    requeueReply = LSBE_EXCEPT_ACTION;
                }
            }
        }

    } while ((jPtr = jPtr->nextJob)
             && (requeueOneElement == FALSE));

    if (requeueSuccess)
        return LSBE_NO_ERROR;

    return requeueReply;
}


static
void closeSbdConnect4ZombieJob(struct jData *jData)
{
    struct sbdNode *sbdPtr, *nextSbdPtr;

    for (sbdPtr = sbdNodeList.forw;
         sbdPtr != &sbdNodeList;
         sbdPtr = nextSbdPtr) {
        nextSbdPtr = sbdPtr->forw;
        if (sbdPtr->jData == jData) {
            chanClose_(sbdPtr->chanfd);
            offList((struct listEntry *) sbdPtr);
            FREEUP(sbdPtr);
            nSbdConnections--;
        }
    }
    return;
}

static int checkSubHost(struct jData *job)
{

    int isTypeUnkown = FALSE;
    int isModelUnkown = FALSE;
    int requireType = FALSE;
    int requireModel = FALSE;
    int requireLocalType = FALSE;
    int requireLocalModel = FALSE;
    char *select = NULL;
    char *jSelect = NULL;
    char *qSelect = NULL;
    int len = 0;
    struct hostInfo *submitHost = NULL;

    if (mSchedStage == M_STAGE_REPLAY) {
        return LSBE_NO_ERROR;
    }

    submitHost = getLsfHostData(job->shared->jobBill.fromHost);
    if (submitHost == NULL) {
        return LSBE_BAD_SUBMISSION_HOST;
    }

    if (strcmp(submitHost->hostType, "UNKNOWN_AUTO_DETECT") == 0) {
        isTypeUnkown = TRUE;
    }

    if (strcmp(submitHost->hostModel, "UNKNOWN_AUTO_DETECT") == 0) {
        isModelUnkown = TRUE;
    }

    if (isTypeUnkown == FALSE && isModelUnkown == FALSE ) {
        return LSBE_NO_ERROR;
    }

    if (job->shared->resValPtr
        && job->shared->resValPtr->selectStr
        && job->shared->resValPtr->selectStr[0] != '\0') {
        jSelect = job->shared->resValPtr->selectStr;
        len += strlen(job->shared->resValPtr->selectStr);
    }

    if (job->qPtr->resValPtr
        && job->qPtr->resValPtr->selectStr
        && job->qPtr->resValPtr->selectStr[0] != '\0') {
        qSelect = job->qPtr->resValPtr->selectStr;
        len += strlen(job->qPtr->resValPtr->selectStr);
    }

    if (len == 0) {

        if ( isTypeUnkown == FALSE) {
            return LSBE_NO_ERROR;
        }
        if (job->numAskedPtr > 0) {
            return LSBE_NO_ERROR;
        }

        return LSBE_BAD_SUBMISSION_HOST;
    }


    select = (char *)calloc(1, len + 20);
    if (select == NULL) {
        return LSBE_NO_MEM;
    }

    if (jSelect ) {
        strcat(select, jSelect);
    } else {
        if (qSelect) {
            strcat(select, qSelect);
        } else {

        }
    }


    if ( strstr(select, "[type \"eq\"")) {
        requireType = TRUE;
        if (strstr(select, "[type \"eq\" \"local\"")) {
            requireLocalType = TRUE;
        } else {
            requireLocalType = FALSE;
        }
    } else {
        requireType = FALSE;
    }


    if ( strstr(select, "[model \"eq\"")) {
        requireModel = TRUE;
        if (strstr(select, "[model \"eq\" \"local\"")) {
            requireLocalModel = TRUE;
        } else {
            requireLocalModel = FALSE;
        }
    } else {
        requireModel = FALSE;
    }

    FREEUP(select);

    if (isTypeUnkown && !requireType && !requireModel) {

        if (job->numAskedPtr > 0) {
            return LSBE_NO_ERROR;
        }

        return LSBE_BAD_SUBMISSION_HOST;
    }

    if (isTypeUnkown && requireLocalType ) {
        return LSBE_BAD_SUBMISSION_HOST;
    }

    if (isModelUnkown && requireLocalModel) {
        return LSBE_BAD_SUBMISSION_HOST;
    }

    return LSBE_NO_ERROR;
}

static int staticNumPendJobs (void)
{
    int numPend = 0;
    struct qData *qp;

    for (qp = qDataList->forw; qp != qDataList; qp = qp->forw) {
        numPend += qp->numPEND;
    }

    return numPend;
}


/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: mbd.queue.c 397 2007-11-26 19:04:00Z mblack $
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

#include <math.h>
#include <stdlib.h>

#include "mbd.h"
#include "mbd.fairshare.h"
#include "../../lsf/lib/lsi18n.h"

#define NL_SETN         10
#define JOBIDSTRLEN     20

void fillHostnames();

struct hTab jobIdHT;
struct hTab jgrpIdHT;

static int
isDefQueue (char*);

static int
checkHU (char* , char*, struct qData*);

static int
getCheckList(struct infoReq*, char**, char**);

static time_t
runWindowCloseTime(struct qData*);

static void addSAInfoTree(struct queueInfoEnt *, struct qData *);
static void createSAInfoTree(struct shareAcct *, struct shareAcctInfoEnt *);
extern void freeShareAcctInfoEnt(struct shareAcctInfoEnt *);

void
inQueueList (struct qData *entry)
{
    struct qData *qp;


    for (qp = qDataList->forw; qp != qDataList; qp = qp->forw)
        if (entry->priority <= qp->priority)
            break;

    inList((struct listEntry *)qp, (struct listEntry *)entry);
    numofqueues++;

}

void
checkQWindow (void)
{
    static char fname[] = "checkQWindow";
    struct qData *qp;
    struct dayhour dayhour;
    windows_t *wp;
    char windOpen;

    if (qDataList->forw == qDataList)
        return;

    for (qp = qDataList->forw; (qp != qDataList); qp = qp->forw) {

        if (qp->windEdge > now || qp->windEdge == 0)
            continue;

        getDayHour (&dayhour, now);


        qp->qStatus &= ~(QUEUE_STAT_RUN | QUEUE_STAT_RUNWIN_CLOSE);
        qp->windEdge = now + (24.0 - dayhour.hour) * 3600.0;


        if (qp->weekR[dayhour.day] == NULL) {

            windOpen = TRUE;
        } else {
            windOpen = FALSE;
        }
        for (wp = qp->weekR[dayhour.day]; wp != NULL; wp = wp->nextwind) {
            checkWindow(&dayhour, &windOpen, &qp->windEdge, wp, now);
            if (windOpen)
                break;
        }
        if (!windOpen)  {
            qp->qStatus |= QUEUE_STAT_RUNWIN_CLOSE;
            continue;
        } else
            qp->qStatus &= ~QUEUE_STAT_RUNWIN_CLOSE;

        if (qp->week[dayhour.day] == NULL) {
            qp->qStatus |= QUEUE_STAT_RUN;
            continue;
        }


        windOpen = FALSE;
        for (wp = qp->week[dayhour.day]; wp != NULL; wp = wp->nextwind) {
            checkWindow(&dayhour, &windOpen, &qp->windEdge, wp, now);
            if (windOpen) {
                qp->qStatus |= QUEUE_STAT_RUN;
                break;
            }
        }
    }


    for (qp = qDataList->forw; (qp != qDataList); qp = qp->forw) {
        if (HAS_RUN_WINDOW(qp) && !IGNORE_DEADLINE(qp)) {
            if (qp->qStatus & QUEUE_STAT_RUN) {
                qp->runWinCloseTime = runWindowCloseTime(qp);
                if (qp->runWinCloseTime == 0) {

                    qp->qStatus |= QUEUE_STAT_RUNWIN_CLOSE;
                    qp->qStatus &= ~QUEUE_STAT_RUN;
                }
                if (logclass & LC_SCHED) {
                    if (qp->runWinCloseTime != 0) {
                        ls_syslog(LOG_DEBUG2, "%s: run window of queue <%s> will close at %s", fname, qp->queue, ctime(&qp->runWinCloseTime));
                    } else {
                        ls_syslog(LOG_DEBUG2, "%s: run window of queue <%s> is open but runWinCloseTime is 0, reset queue's run window to close", fname, qp->queue);
                    }
                }
            } else {
                qp->runWinCloseTime = 0;
            }
        }
    }
}

struct qData *
getQueueData(char *queueName)
{
    struct qData *qp;
    if (qDataList->forw == qDataList)
        return((struct qData *)NULL);
    for (qp = qDataList->forw; (qp != qDataList); qp = qp->forw) {
        if (strcmp(qp->queue, queueName) != 0)
            continue;
        return((struct qData *)qp);
    }
    return((struct qData *)NULL);
}

int
checkQueues(struct infoReq*        queueInfoReqPtr,
            struct queueInfoReply* queueInfoReplyPtr)
{
    static char fname[] = "checkQueues()";
    struct qData*           qp;
    struct qData*           next;
    struct queueInfoEnt*    qRep = NULL;
    int                     i;
    int                     j;
    int                     checkRet;
    int                     allQ = FALSE;
    int                     defaultQ = FALSE;
    int                     found = FALSE;
    char*                   checkUsers = NULL;
    char*                   checkHosts = NULL;
    float*                  cpuFactor;

    queueInfoReplyPtr->numQueues = 0;
    queueInfoReplyPtr->nIdx = allLsInfo->numIndx;

    if (queueInfoReqPtr->options & ALL_QUEUE) {
        queueInfoReqPtr->numNames = 1;
        allQ = TRUE;
    } else if (queueInfoReqPtr->options & DFT_QUEUE) {

        queueInfoReqPtr->numNames = 1;
        defaultQ = TRUE;
    }

    if ((checkRet = getCheckList (queueInfoReqPtr, &checkHosts, &checkUsers))
        != LSBE_NO_ERROR)
        return (checkRet);

    for (j = 0; j < queueInfoReqPtr->numNames; j++) {
        for (qp = qDataList->back; (qp != qDataList); qp = next) {
            next = qp->back;

            if (strcmp (qp->queue, LOST_AND_FOUND) == 0 && qp->numJobs == 0) {
                continue;
            }
            if (!allQ && !defaultQ
                && strcmp (qp->queue, queueInfoReqPtr->names[j]) != 0)
                continue;

            if (!allQ && defaultQ && !isDefQueue (qp->queue))
                continue;

            found = TRUE;

            if ((checkRet = checkHU (checkHosts, checkUsers, qp))
                != LSBE_NO_ERROR)
                continue;

            for (i = 0; i < queueInfoReplyPtr->numQueues; i++) {
                if (strcmp (qp->queue, queueInfoReplyPtr->queues[i].queue) == 0) {
                    if (strcmp(qp->queue, LOST_AND_FOUND) !=0) {

                        break;
                    } else {

                        queueInfoReplyPtr->queues[i].numJobs += qp->numJobs;
                        queueInfoReplyPtr->queues[i].numPEND += qp->numPEND;
                        queueInfoReplyPtr->queues[i].numRUN  += qp->numRUN;
                        queueInfoReplyPtr->queues[i].numSSUSP+= qp->numSSUSP;
                        queueInfoReplyPtr->queues[i].numUSUSP+= qp->numUSUSP;
                        break;
                    }
                }
            }
            if (i < queueInfoReplyPtr->numQueues)
                continue;

            qRep = &(queueInfoReplyPtr->queues[queueInfoReplyPtr->numQueues]);

            qRep->queue       = qp->queue;
            qRep->description = qp->description;
            qRep->schedDelay  = qp->schedDelay;
            qRep->mig         =
                (qp->mig != INFINIT_INT) ? qp->mig/60 : INFINIT_INT;

            if (qp->acceptIntvl == DEF_ACCEPT_INTVL
                || qp->acceptIntvl == INFINIT_INT)
                qRep->acceptIntvl = INFINIT_INT;
            else
                qRep->acceptIntvl = qp->acceptIntvl * msleeptime;


            if (qp->windows)
                qRep->windows= safeSave (qp->windows);
            else
               qRep->windows= safeSave (" ");
            if (qp->windowsD)
                qRep->windowsD = safeSave (qp->windowsD);
            else
               qRep->windowsD = safeSave (" ");

            if (qp->uGPtr) {
                qRep->userList = getGroupMembers (qp->uGPtr, FALSE);
            } else {
                qRep->userList = safeSave(" ");
            }
            if (qp->hostList) {



                char *word = NULL, *hostList = NULL;
                int len=0;
                struct gData *gp = NULL;

                hostList = qp->hostList;
                while ((hostList = strstr(hostList, " ")) != NULL) {
                    hostList++;
                    len++;
                }

                qRep->hostList = (char *)calloc((strlen(qp->hostList)+len*2+2),
                                                sizeof(char));
                if(qRep->hostList == NULL) {
                    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "calloc",
                              (strlen(qp->hostList)+len*2+2)*sizeof(char));
                    return(LSBE_NO_MEM);
                }
                hostList = qp->hostList;
                while ((word = getNextWord_(&hostList)) != NULL) {
                    strcat(qRep->hostList,word);
                    if ((gp = getHGrpData(word)) != NULL) {
                        strcat(qRep->hostList, "/");
                    }
                    strcat(qRep->hostList, " ");
                }
            } else {
                qRep->hostList = safeSave(" ");
            }
            qRep->priority = qp->priority;
            qRep->nice = qp->nice;
            qRep->userJobLimit = qp->uJobLimit;
            if (qp->pJobLimit >= INFINIT_FLOAT)
                qRep->procJobLimit = INFINIT_FLOAT;
            else
                qRep->procJobLimit = qp->pJobLimit;
            qRep->hostJobLimit = qp->hJobLimit;
            qRep->maxJobs = qp->maxJobs;
            qRep->numJobs = qp->numJobs;
            qRep->numPEND = qp->numPEND;
            qRep->numRUN = qp->numRUN;
            qRep->numSSUSP = qp->numSSUSP;
            qRep->numUSUSP = qp->numUSUSP;
            qRep->numRESERVE = qp->numRESERVE;

            qRep->qAttrib = qp->qAttrib;
            qRep->qStatus = qp->qStatus;
            for(i = 0; i < LSF_RLIM_NLIMITS; i++) {
                qRep->rLimits[i] = qp->rLimits[i];
                qRep->defLimits[i] = qp->defLimits[i];
            }
            if (qp->hostSpec == NULL)
                qRep->hostSpec = safeSave(" ");
            else {
                qRep->hostSpec = safeSave (qp->hostSpec);
                if ((cpuFactor = getModelFactor (qp->hostSpec)) == NULL) {
                    if ((cpuFactor = getHostFactor (qp->hostSpec)) == NULL) {
                        float one = 1.0;

                        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7500,
                            "%s: Cannot find cpu factor for hostSpec <%s> in queue <%s>; cpuFactor is set to 1.0"), /* catgets 7500 */
                            fname,
                            qp->hostSpec, qp->queue);
                        cpuFactor = &one;
                    }
                }
                if (cpuFactor != NULL) {
                    if (qRep->rLimits[LSF_RLIMIT_CPU] > 0)
                        qRep->rLimits[LSF_RLIMIT_CPU] /= *cpuFactor;
                    if (qRep->rLimits[LSF_RLIMIT_RUN] > 0)
                        qRep->rLimits[LSF_RLIMIT_RUN] /= *cpuFactor;
                    if (qRep->defLimits[LSF_RLIMIT_CPU] > 0)
                        qRep->defLimits[LSF_RLIMIT_CPU] /= *cpuFactor;
                    if (qRep->defLimits[LSF_RLIMIT_RUN] > 0)
                        qRep->defLimits[LSF_RLIMIT_RUN] /= *cpuFactor;
                }
            }
            if (qp->defaultHostSpec)
                qRep->defaultHostSpec = safeSave (qp->defaultHostSpec);
            else
                 qRep->defaultHostSpec = safeSave (" ");
            qRep->loadSched  = qp->loadSched;
            qRep->loadStop = qp->loadStop;

            qRep->procLimit = qp->procLimit;
            qRep->minProcLimit = qp->minProcLimit;
            qRep->defProcLimit = qp->defProcLimit;
            if (qp->nAdmins > 0)
                qRep->admins = safeSave (qp->admins);
            else
                qRep->admins = safeSave(" ");

            if (qp->preCmd)
                qRep->preCmd = safeSave (qp->preCmd);
            else
                qRep->preCmd = safeSave(" ");

            if (qp->prepostUsername) {
                qRep->prepostUsername = safeSave (qp->prepostUsername);
            } else {
                qRep->prepostUsername = safeSave(" ");
            }

            qRep->chkpntPeriod = qp->chkpntPeriod;
            if ( qp->chkpntDir)
                qRep->chkpntDir = safeSave(qp->chkpntDir);
            else
                qRep->chkpntDir = safeSave(" ");

            if (qp->postCmd)
                qRep->postCmd = safeSave (qp->postCmd);
            else
                qRep->postCmd = safeSave(" ");
            if (qp->requeueEValues)
                qRep->requeueEValues = safeSave (qp->requeueEValues);
            else
                qRep->requeueEValues = safeSave(" ");

            if (qp->resReq)
                qRep->resReq = safeSave (qp->resReq);
            else
                qRep->resReq = safeSave(" ");
            qRep->slotHoldTime = qp->slotHoldTime;

            if (qp->resumeCond)
                qRep->resumeCond = safeSave (qp->resumeCond);
            else
                qRep->resumeCond = safeSave(" ");

            if (qp->stopCond)
                qRep->stopCond = safeSave (qp->stopCond);
            else
                qRep->stopCond = safeSave(" ");

            if (qp->jobStarter)
                qRep->jobStarter = safeSave (qp->jobStarter);
            else
                qRep->jobStarter = safeSave(" ");

            if (qp->suspendActCmd) {
                if (strcmp(qp->suspendActCmd, "SIG_CHKPNT") == 0)
                    qRep->suspendActCmd =safeSave ("CHKPNT");
                else
                    qRep->suspendActCmd =safeSave (qp->suspendActCmd);
            } else
                qRep->suspendActCmd = safeSave(" ");

            if (qp->resumeActCmd)
                qRep->resumeActCmd = safeSave (qp->resumeActCmd);
            else
                qRep->resumeActCmd = safeSave(" ");

            if (qp->terminateActCmd) {
                if (strcmp(qp->terminateActCmd, "SIG_CHKPNT") == 0)
                    qRep->terminateActCmd = safeSave ("CHKPNT");
                else
                    qRep->terminateActCmd = safeSave (qp->terminateActCmd);
            } else
                qRep->terminateActCmd = safeSave(" ");

            for (i = 0; i <LSB_SIG_NUM; i++)
                qRep->sigMap[i] = qp->sigMap[i];

            qRep->fsFactors.cpuTimeFactor = qp->fsFactors.cpuTimeFactor;
            qRep->fsFactors.runTimeFactor = qp->fsFactors.runTimeFactor;
            qRep->fsFactors.runJobFactor = qp->fsFactors.runJobFactor;
            qRep->fsFactors.histHours = qp->fsFactors.histHours;
            if (qp->qAttrib & Q_ATTRIB_FS) {
                qRep->userShares = qp->userShares;
                addSAInfoTree(qRep, qp);
            }
            qRep->actionComment = (qp->actionComment ? qp->actionComment : "");

            queueInfoReplyPtr->numQueues++;

        }

        if (!allQ && !defaultQ && !found) {
            if (queueInfoReplyPtr->numQueues > 0)
                freeQueueInfoReply (queueInfoReplyPtr, "freeAll");
            queueInfoReplyPtr->badQueue = j;
            queueInfoReplyPtr->numQueues = 0;
            FREEUP (checkUsers);
            FREEUP (checkHosts);
            return (LSBE_BAD_QUEUE);
        }

        found = FALSE;
        if (allQ || defaultQ)
            break;
    }

    FREEUP (checkUsers);
    FREEUP (checkHosts);
    if (queueInfoReplyPtr->numQueues == 0) {
        return (checkRet);
    }

    return (LSBE_NO_ERROR);

}

static int
isDefQueue (char *qname)
{
    char *cp, *queue;

    if (defaultQueues == NULL)
        return FALSE;
    cp = defaultQueues;
    while ((queue = getNextWord_(&cp)))
        if (strcmp (qname, queue) == 0)
            return TRUE;
    return FALSE;
}

int
ctrlQueue (struct controlReq *qcReq, struct lsfAuth *auth)
{
    struct qData *qp;

    if ((qp = getQueueData(qcReq->name)) == NULL)
        return(LSBE_BAD_QUEUE);

    if (!isAuthManager(auth) && auth->uid != 0 && !isAuthQueAd (qp, auth)) {
        ls_syslog(LOG_CRIT, I18N(7511,
        "ctrlQueue: uid <%d> not allowed to perform control operation"), auth->uid);/* catgets 7511 */
        return (LSBE_PERMISSION);
    }

    if ( (qcReq->opCode < QUEUE_OPEN) || (qcReq->opCode > QUEUE_INACTIVATE) )
        return(LSBE_LSBLIB);

    if (qcReq->opCode == QUEUE_ACTIVATE &&
        !(qp->qStatus & QUEUE_STAT_RUN)) {
        qp->qStatus |= QUEUE_STAT_ACTIVE;
        FREEUP(qp->actionComment);
        qp->actionComment = safeSave(qcReq->msg);
        log_queuestatus(qp, qcReq->opCode, auth->uid, auth->lsfUserName);
        return(LSBE_QUEUE_WINDOW);
    }

    if (((qp->qStatus & QUEUE_STAT_OPEN) &&
                            (qcReq->opCode == QUEUE_OPEN))
          || (!(qp->qStatus & QUEUE_STAT_OPEN) &&
                                (qcReq->opCode == QUEUE_CLOSED))
          || ((qp->qStatus & QUEUE_STAT_ACTIVE) &&
                                (qcReq->opCode == QUEUE_ACTIVATE))
          || (!(qp->qStatus & QUEUE_STAT_ACTIVE) &&
                                (qcReq->opCode == QUEUE_INACTIVATE)))
        return(LSBE_NO_ERROR);

    switch (qcReq->opCode) {
        case QUEUE_OPEN:
            qp->qStatus |= QUEUE_STAT_OPEN;
            break;
        case QUEUE_CLOSED:
            qp->qStatus &= ~QUEUE_STAT_OPEN;
            break;
        case QUEUE_ACTIVATE:
            qp->qStatus |= QUEUE_STAT_ACTIVE;
            break; 
        case QUEUE_INACTIVATE:
            qp->qStatus &= ~QUEUE_STAT_ACTIVE;
            break;
        default:
            break;
    }

    switch (qcReq->opCode) {
        case QUEUE_OPEN:
        case QUEUE_CLOSED:
        case QUEUE_ACTIVATE:
        case QUEUE_INACTIVATE:
            FREEUP(qp->actionComment);
            qp->actionComment = safeSave(qcReq->msg);
            log_queuestatus(qp, qcReq->opCode, auth->uid, auth->lsfUserName);
            break;
        default:
            break;
    }

    return(LSBE_NO_ERROR);

}

char
hostQMember (char *host, struct qData *qp)
{
    int i;

    if (qp->hostList == NULL)
        return TRUE;
    if (qp->askedOthPrio >= 0)
        return TRUE;

    for (i = 0; i < qp->numAskedPtr; i++)  {
         if (equalHost_(host, qp->askedPtr[i].hData->host))
              return TRUE;
    }
    return FALSE;

}

char
userQMember (char *user, struct qData *qp)
{
    if (qp->uGPtr == NULL)
        return TRUE;

    return (gMember(user, qp->uGPtr));

}

static int
checkHU (char *hostList, char *userList, struct qData *qp)
{
    char *sp;

    if (hostList != NULL && qp->hostList != NULL)
        while ((sp = getNextWord_(&hostList)))
            if (!hostQMember (sp, qp))
                return (LSBE_QUEUE_HOST);

    if (userList != NULL && qp->uGPtr != NULL)
        while ((sp = getNextWord_(&userList)))
            if (!gMember(sp, qp->uGPtr))

                return (LSBE_QUEUE_USE);

    return (LSBE_NO_ERROR);

}

static int
getCheckList (struct infoReq *qInfoReq, char **hostList, char **userList)
{
    char *sp;
    int numNames;
    struct hostent *hp;
    struct gData *gp;
    struct passwd *pp;
    char **allHosts;
    int numAllHosts, i;

    *hostList = NULL;
    *userList = NULL;
    numNames = qInfoReq->numNames;

    if (qInfoReq->options & CHECK_USER) {
        sp = qInfoReq->names[numNames];
        ++numNames;

        if (strcmp(sp, "all") == 0)
            *userList = safeSave(sp);
        else if ((pp = getpwlsfuser_(sp)) != NULL) {
            if (!isManager(sp) && pp->pw_uid != 0) {

                *userList = safeSave(sp);
            }
        }
        else if ((gp = getUGrpData(sp)) != NULL)
            *userList = getGroupMembers(gp, TRUE);
        else
            *userList = safeSave (sp);
    }

    if (qInfoReq->options & CHECK_HOST) {
        sp = qInfoReq->names[numNames];

        if (strcmp(sp, "all") == 0)
            *hostList = safeSave (sp);
        else if ((hp = Gethostbyname_(sp)) != NULL)
            *hostList = safeSave(hp->h_name);
        else if ((gp = getHGrpData(sp)) != NULL)
            *hostList = getGroupMembers(gp, TRUE);
        else
            return (LSBE_BAD_HOST);


        if (hostList == NULL) {
          return (LSBE_BAD_HOST);
        }
        if (strcmp(*hostList,"all") == 0) {
            FREEUP(*hostList);
            if ((numAllHosts = getLsbHostNames(&allHosts)) <= 0) {
                ls_syslog(LOG_ERR,I18N(7512,
                   "getCheckList: Unable to obtain host list")); /* catgets 7512 */
                return (LSBE_BAD_HOST);
            }
            (*hostList) =
              (char*)my_malloc(numAllHosts*MAX_LSB_NAME_LEN,"getCheckList");
            if (*hostList == NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL, "getCheckList", "my_malloc");
                return (LSBE_BAD_HOST);
            }
            (*hostList)[0] = '\0';
            for (i=0; i<numAllHosts; i++) {

                strcat(*hostList, allHosts[i]);
                if (i < numAllHosts-1) {
                    strcat(*hostList, " ");
                }
            }
        }
    }

    return (LSBE_NO_ERROR);

}

int
isQueAd (struct qData *qp, char *lsfUserName)
{
    char *admins, *user;

    if (!qp->nAdmins || !qp->admins)
        return (FALSE);

    admins = qp->admins;

    for (user = getNextWord_(&admins); user; user = getNextWord_(&admins)) {
         if ((strcmp(user, lsfUserName) == 0)
                || (strcmp(user, "all users") == 0)) {
             return (TRUE);
         }
    }

    return (FALSE);
}
int
isAuthQueAd (struct qData *qp, struct lsfAuth *auth)
{
    return (isQueAd(qp, auth->lsfUserName));

}

int
isInQueues (char *queue, char **queues, int num)
{
    int i;

    if (num <= 0 || queues == NULL || queue == NULL)
        return (FALSE);
    for (i = 0; i < num; i++) {
        if (!strcmp (queue, queues[i]))
            return (TRUE);
    }
    return (FALSE);

}
bool_t
isQInQSet(struct qData *queue, LS_BITSET_T *queueSet)
{
    if (queue == NULL || queueSet == NULL)
        return(FALSE);

    return(setIsMember(queueSet, queue));
}
void
freeQueueInfoReply (struct queueInfoReply *reply, char *freeAll)
{
    int i;

    if (reply == NULL || reply->queues == NULL)
        return;
    if (freeAll == (char *)0) {
        FREEUP (reply->queues);
        return;
    }
    for (i = 0; i < reply->numQueues; i++) {
        FREEUP (reply->queues[i].windows);
        FREEUP (reply->queues[i].windowsD);
        FREEUP (reply->queues[i].defaultHostSpec);

        FREEUP (reply->queues[i].userList);
        FREEUP (reply->queues[i].hostList);
        FREEUP (reply->queues[i].hostSpec);
        FREEUP (reply->queues[i].admins);
        FREEUP (reply->queues[i].preCmd);
        FREEUP (reply->queues[i].postCmd);
        FREEUP (reply->queues[i].prepostUsername);
        FREEUP (reply->queues[i].requeueEValues);
        FREEUP (reply->queues[i].resReq);
        FREEUP (reply->queues[i].resumeCond);
        FREEUP (reply->queues[i].stopCond);
        FREEUP (reply->queues[i].jobStarter);
        FREEUP (reply->queues[i].suspendActCmd);
        FREEUP (reply->queues[i].resumeActCmd);
        FREEUP (reply->queues[i].terminateActCmd);
        FREEUP (reply->queues[i].chkpntDir);
        if (reply->queues[i].shareAcctTree) {
            freeShareAcctInfoEnt(reply->queues[i].shareAcctTree);
            FREEUP(reply->queues[i].shareAcctTree);
        }
    }
    FREEUP (reply->queues);
}

int
createQueueHostSet(struct qData *qp)
{
    int allHosts;
    int i;
    struct hData *hPtr;

    allHosts = qp->numAskedPtr;
    if (allHosts == 0)
        allHosts = numofhosts();

    qp->hostInQueue = setCreate(allHosts,
                                gethIndexByhData,
                                gethDataByhIndex,
                                (char *)__func__);

    if (allHosts == numofhosts()) {
        for (hPtr = (struct hData *)hostList->back;
             hPtr != (void *)hostList;
             hPtr = hPtr->back)
            setAddElement(qp->hostInQueue, hPtr);
    } else {
        for (i = 0; i < allHosts; i++)
            setAddElement(qp->hostInQueue, qp->askedPtr[i].hData);
    }

    return 0;
}

int
gethIndexByhData(void *userData)
{
    struct hData *host;

    host = (struct hData *)userData;

    return host->hostId;
}
void *
gethDataByhIndex(int index)
{
    struct hData *hPtr;

    for (hPtr = (struct hData *)hostList->back;
         hPtr != (void *)hostList;
         hPtr = hPtr->back) {
        if (hPtr->hostId == index)
            return hPtr;
    }

    return NULL;
}

bool_t
isHostQMember(struct hData *host, struct qData *qp)
{
    bool_t trueOrfalse;

    if (qp->hostList == NULL)
        return TRUE;
    if (qp->askedOthPrio >= 0)
        return TRUE;

    bitseterrno = 0;

    trueOrfalse = setIsMember(qp->hostInQueue, host);

    return trueOrfalse;
}

int
getIndexByQueue(void *queue)
{
    struct qData *qp;

    for (qp = qDataList->forw; qp != qDataList; qp = qp->forw) {
        if (qp == (struct qData *)queue)
            return qp->queueId;
    }
    return -1;
}

void *
getQueueByIndex(int queueIndex)
{
    struct qData *qp;

    for (qp = qDataList->forw; qp != qDataList; qp = qp->forw) {
        if (queueIndex == qp->queueId)
            return qp;
    }
    return NULL;
}

static time_t
runWindowCloseTime(struct qData *qp)
{
    static char fname[] = "runWindowCloseTime";
    struct dayhour dayhour;
    time_t deadline, lastdeadline, midnight, thismidnight, nowTime = now;
    char windOpen;
    windows_t *wp;
    struct tm *tmPtr;

    getDayHour(&dayhour, nowTime);
    if (qp->weekR[dayhour.day] == NULL) {

        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7510,
            "%s: run window for queue <%s> on day <%d> is NULL"), /* catgets 7510 */
            fname, qp->queue, dayhour.day);
        return 0;
    }

    tmPtr = localtime(&nowTime);
    tmPtr->tm_sec = 59;
    tmPtr->tm_min = 59;
    tmPtr->tm_hour = 23;
    deadline = midnight = thismidnight = mktime(tmPtr);
    for (;;) {
        windOpen = FALSE;
        lastdeadline = deadline;
        for (wp = qp->weekR[dayhour.day]; wp != NULL; wp = wp->nextwind) {
            checkWindow(&dayhour, &windOpen, &deadline, wp, nowTime);
            if (windOpen) {
                break;
            }
        }
        if (windOpen) {
            if (deadline == midnight) {

                nowTime = midnight + 1;
                midnight += 3600*24;
                deadline = midnight;
                getDayHour(&dayhour, nowTime);
             } else {
                return deadline;
             }
        } else {
            if (midnight == thismidnight) {

                return 0;
            } else {

                return lastdeadline;
            }
        }
    }
}

/* Translate faireshare tree from the format organized by 'struct shareAcct'
 * into the format organized by 'struct shareAcctInfoEnt'.
 */
static void addSAInfoTree(struct queueInfoEnt *qInfo, struct qData *qp) {
    hEnt *ent = NULL;
    struct fairsharePolicy *policy = NULL;

    ent = h_getEnt_(&policies, qp->queue);
    if (!ent) {
        return;
    }
    policy = (struct fairsharePolicy *)ent->hData;
    qInfo->shareAcctTree = (struct shareAcctInfoEnt *)my_calloc(1, sizeof(struct shareAcctInfoEnt), "addSAInfoTree");
    createSAInfoTree(policy->root, qInfo->shareAcctTree);
    
} /*addSAInfoTree*/

/*Traverse fairshare tree in pre-order way*/
static void createSAInfoTree(struct shareAcct * node, struct shareAcctInfoEnt *saInfo) {
    int i = 0;
    LIST_ENTRY_T        *ent;
    struct shareAcct    *curAcct = NULL;

    saInfo->name = safeSave(node->name);
    saInfo->share = node->share;
    saInfo->priority = node->priority;
    saInfo->numStartJobs = node->numStartJobs;
    saInfo->cpuTime = ((node->histCpuTime + node->newUsedCpuTime) >= MIN_CPU_TIME) ? (node->histCpuTime + node->newUsedCpuTime) : 0.0;
    saInfo->runTime = (float)node->runTime;
    saInfo->nChildShareAcct = 0;
    saInfo->childShareAccts = NULL;

    if (FS_IS_LEAF_NODE(node)) {
        return;
    }

    saInfo->childShareAccts = (struct shareAcctInfoEnt *)my_calloc(FS_NUM_CHILD(node), sizeof(struct shareAcctInfoEnt), "createSAInfoTree");
    saInfo->nChildShareAcct = FS_NUM_CHILD(node);

    i = 0;
    for (ent = node->sharePolicy->sAcctList->back;
         ent != (LIST_ENTRY_T *)node->sharePolicy->sAcctList;
         ent = ent->back){
         curAcct = (struct shareAcct *)ent;
         createSAInfoTree(curAcct, &(saInfo->childShareAccts[i]));
         i++;
    }

    return;
} /*createSAInfoTree*/


/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: mbd.jarray.c 397 2007-11-26 19:04:00Z mblack $
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

#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <malloc.h>

#include "mbd.h"
#include "mbd.fairshare.h"

#define NL_SETN		10	

struct jShared * copyJShared(struct jData *);


extern char *yybuff;


static struct idxList      *arrayIdxList = NULL;
void                       setIdxListContext(const char *);
struct idxList             *getIdxListContext(void);
void                       freeIdxListContext(void);


void
freeIdxList(struct idxList *idxList)
{
    struct idxList *ptr;

    while(idxList) {
         ptr = idxList->next;
         FREEUP(idxList);
         idxList = ptr;
    }
    return;
} 

 
struct idxList *
parseJobArrayIndex(char *job_name, int *error, int *maxJLimit)
{

    struct idxList *idxList = NULL, *idx;
    char   *index;
    int    arraySize = 0;

    index = strchr(job_name, '[');
    *error = LSBE_NO_ERROR;
    if (!index)
        return(NULL);
    yybuff = index;

    *maxJLimit = INFINIT_INT;

    if (idxparse(&idxList, maxJLimit)) {
        freeIdxList(idxList);
        if (idxerrno == IDX_MEM) 
            *error = LSBE_NO_MEM;
        else
            *error = LSBE_BAD_JOB;
        return(NULL);
    }    
    
    for (idx = idxList; idx; idx = idx->next) {
	
	
        if (idx->start < 1 || idx->step < 1) {
            *error = LSBE_BAD_IDX;
        }
	if (idx->end == INFINIT_INT) {
	    idx->end  = idx->start + (maxJobArraySize - 1)*idx->step; 
	}
	
	if (idx->start > idx->end) {
	    *error = LSBE_BAD_IDX;
	}
       
        if ((mSchedStage != M_STAGE_REPLAY)
            && (idx->end >= LSB_MAX_ARRAY_IDX)) {
            *error = LSBE_BIG_IDX;
        }

	arraySize += (idx->end - idx->start)/idx->step + 1;
    }

    if ((mSchedStage != M_STAGE_REPLAY) && (arraySize > maxJobArraySize)) {
        *error = LSBE_BIG_IDX;
    }

    if (*error == LSBE_NO_ERROR)
        return(idxList);
    else {
        freeIdxList(idxList);
        return(NULL);
    }
} 

struct jShared *
copyJShared(struct jData *jp)
{
    struct lsfAuth auth;
    struct jShared *js = jp->shared;
    int errcode, jFlags, useLocal = TRUE;
    struct jShared *newJs = (struct jShared *) my_calloc(1, sizeof(struct jShared), "copyJShared");
   
    strcpy(auth.lsfUserName, jp->userName);
    auth.uid = jp->userId;

    newJs->numRef = 0;
  
    if (strcmp (js->jobBill.dependCond, "") != 0) {

	
	setIdxListContext((*js).jobBill.jobName);

        newJs->dptRoot = parseDepCond(js->jobBill.dependCond,
				      &auth, &errcode, 
				      NULL, &jFlags, 0);
	freeIdxListContext();
    }

    copyJobBill (&js->jobBill, &newJs->jobBill, FALSE);
    if (jp->numAskedPtr > 0 || jp->askedOthPrio >= 0)
       useLocal = FALSE;
    else
       useLocal = TRUE;

    useLocal = useLocal ? USE_LOCAL : 0;
    newJs->resValPtr = checkResReq (js->jobBill.resReq, useLocal | CHK_TCL_SYNTAX | PARSE_XOR);

    return(newJs);
} 


int
localizeJobArray(struct jData *jArray)
{   
    struct jData *jp;

    if (jArray->nodeType != JGRP_NODE_ARRAY)
        return(LSBE_ARRAY_NULL);

    jp = jArray->nextJob;
    while (jp && jArray->shared->numRef > 1) {
        localizeJobElement(jp);
        jp = jp->nextJob;
    }
    return(LSBE_NO_ERROR);
} 


int
localizeJobElement(struct jData *jElement)
{
    struct jShared *sharedData;
    
    if (!jElement || jElement->shared->numRef <= 1)
        return(LSBE_NO_ERROR);

    sharedData = copyJShared(jElement);
    destroySharedRef(jElement->shared);
    jElement->shared = createSharedRef(sharedData);
    mkJobMergedResReqEntry(jElement);
    return(LSBE_NO_ERROR);

} 

struct jData *
copyJData(struct jData *jp)
{
    struct jData *jData;
    struct rqHistory *reqHistory;
    int          i;
 
    jData = initJData(jp->shared);
    
    if (jData->jobSpoolDir) {
	FREEUP(jData->jobSpoolDir);
    }
    reqHistory = jData->reqHistory;
    memcpy((char *)jData, (char *)jp, sizeof(struct jData));
    jData->reqHistory = reqHistory;
    jData->numRef = 0;
    jData->nextJob = NULL;
    
    jData->userName = safeSave(jp->userName);
    jData->schedHost = safeSave(jp->schedHost);
    jData->uPtr = getUserData(jData->userName);
    
    if (jp->askedPtr) {
        jData->askedPtr = (struct askedHost *) my_calloc (jp->numAskedPtr,
                           sizeof(struct askedHost), "copyJData");
        for (i = 0; i < jp->numAskedPtr; i++) {
            jData->askedPtr[i].hData = jp->askedPtr[i].hData;
            jData->askedPtr[i].priority = jp->askedPtr[i].priority;
        }
    }
    if (jp->jobSpoolDir) {
	jData->jobSpoolDir = safeSave(jp->jobSpoolDir);
    }
    return(jData);
} 


int 
updLocalJData(struct jData *jp, struct jData *jpbw)
{
    int    i;

    
    if (jp->numAskedPtr)
         FREEUP (jp->askedPtr);
    jp->numAskedPtr = 0;
    if (jpbw->askedPtr) {
        jp->askedPtr = (struct askedHost *) my_calloc (jpbw->numAskedPtr,
                         sizeof(struct askedHost), "updLocalJData");
        for (i = 0; i < jpbw->numAskedPtr; i++) {
            jp->askedPtr[i].hData = jpbw->askedPtr[i].hData;
            jp->askedPtr[i].priority = jpbw->askedPtr[i].priority;
        }
        jp->numAskedPtr = jpbw->numAskedPtr;
    }

    jp->jFlags &= ~JFLAG_DEPCOND_INVALID; 

    return(LSBE_NO_ERROR);

} 


void
handleNewJobArray(struct jData *jarray, struct idxList *idxList, int maxJLimit)
{
    struct idxList *idxPtr;
    struct jData          *jPtr;
    int  numJobs = 0, i;
    int userPending = 0;

    
    addJobIdHT(jarray);
    jarray->nodeType = JGRP_NODE_ARRAY;
    jarray->nextJob = NULL;

    if (mSchedStage != M_STAGE_REPLAY) {
        putOntoTree(jarray, JOB_NEW);
    } else { 
        putOntoTree(jarray, JOB_REPLAY); 
    }

    
    jarray->uPtr = getUserData(jarray->userName);

    
    if (jarray->shared->jobBill.options2 & SUB2_HOLD) {
	    userPending = 1;
    }

    /*create merged resource requirement for the job*/
    mkJobMergedResReqEntry(jarray);

    jPtr = jarray;
    for (idxPtr = idxList; idxPtr; idxPtr = idxPtr->next) {
        for (i = idxPtr->start; i <= idxPtr->end; i += idxPtr->step) {
             if (getJobData(LSB_JOBID((LS_LONG_INT)jarray->jobId, i)))
                 continue;
             jPtr->nextJob = copyJData(jarray);
             numJobs++;
             jPtr = jPtr->nextJob;
             
             jPtr->nodeType = JGRP_NODE_JOB;
             jPtr->nextJob = NULL;
             jPtr->jobId = LSB_JOBID((LS_LONG_INT)jarray->jobId, i);
             addJobIdHT(jPtr);
             inPendJobList(jPtr, PJL, 0);
	     if ( userPending ) {
		 jPtr->newReason = PEND_USER_STOP;
		 jPtr->jStatus = JOB_STAT_PSUSP;
	     }
        }
    }
  
    ARRAY_DATA(jarray->jgrpNode)->maxJLimit = maxJLimit;

    
    if (mSchedStage != M_STAGE_REPLAY) {
        log_newjob(jarray);
        updAliveUserInFSTree(jarray->qPtr, jarray->userName);
    }

    
    if (mSchedStage != M_STAGE_REPLAY) {
        updQaccount(jarray, jarray->shared->jobBill.maxNumProcessors*numJobs,
                    jarray->shared->jobBill.maxNumProcessors*numJobs, 0, 0, 
                    0, 0);
        updUserData (jarray, jarray->shared->jobBill.maxNumProcessors*numJobs,
                     jarray->shared->jobBill.maxNumProcessors*numJobs, 
                     0, 0, 0, 0);
    }
   
    
   if ( mSchedStage == M_STAGE_REPLAY) {
       if ( maxUserPriority > 0 ) { 
	   if ( jarray->shared->jobBill.userPriority < 0 ) { 
	       	   
	       modifyJobPriority(jarray, maxUserPriority/2);
	       for (jPtr = jarray->nextJob; jPtr; jPtr = jPtr->nextJob) {
		   modifyJobPriority(jPtr, maxUserPriority/2);
	       }
	   } else { 
	       modifyJobPriority(jarray, jarray->shared->jobBill.userPriority);
	       for (jPtr = jarray->nextJob; jPtr; jPtr = jPtr->nextJob) {
	           modifyJobPriority(jPtr, jPtr->shared->jobBill.userPriority);
	       }
	   }
       }
   }

    ARRAY_DATA(jarray->jgrpNode)->counts[getIndexOfJStatus(jarray->nextJob->jStatus)] = numJobs;
    ARRAY_DATA(jarray->jgrpNode)->counts[JGRP_COUNT_NJOBS] = numJobs;
    updJgrpCountByOp(jarray->jgrpNode, 1);

    return;
} 


void
offArray(struct jData *jp)
{
    struct jgTreeNode *node = jp->jgrpNode;
    struct jData *jarray, *ptr;

    if (!node || node->nodeType != JGRP_NODE_ARRAY)
        return;
    jarray = ARRAY_DATA(node)->jobArray;

    for (ptr = jarray; ptr->nextJob && ptr->nextJob != jp;
         ptr = ptr->nextJob);

    if (ptr && ptr->nextJob) {
        ptr->nextJob = ptr->nextJob->nextJob;
    }

    updJgrpCountByJStatus(jp, jp->jStatus, JOB_STAT_NULL);
} 

int
inIdxList(LS_LONG_INT jobId, struct idxList *idxList)
{
    struct idxList *idx;

    if (idxList) {
        for (idx = idxList; idx; idx = idx->next) {
            if (LSB_ARRAY_IDX(jobId) < idx->start ||
                LSB_ARRAY_IDX(jobId) > idx->end)
                continue;
            if (((LSB_ARRAY_IDX(jobId)-idx->start) % idx->step) == 0)
                return(TRUE);
        }
        return(FALSE);
    }
    return(TRUE);
} 

int
getJobIdIndexList (char *jobIdStr, int *outJobId, struct idxList **idxListP)
{
    int jobId = 0;
    char    *startP;
    int     errCode;
    int     maxJLimit = 0;

    *idxListP = NULL;

    
    if ((startP = strchr(jobIdStr, '[')) == NULL) {

        
        if (!isint_(jobIdStr) || ((jobId = atoi(jobIdStr)) < 0)) {
            return(LSBE_BAD_JOBID);
        }
        *outJobId = jobId;
        return(LSBE_NO_ERROR);
    }

    
    *startP = '\0';
    
    if (!isint_(jobIdStr) || ((jobId = atoi(jobIdStr)) <= 0) ||
         (jobId > LSB_MAX_ARRAY_JOBID)) {
        return(LSBE_BAD_JOBID);
    }
    *outJobId = jobId;

    *startP = '[';
    
    if ((*idxListP = parseJobArrayIndex(jobIdStr, &errCode,  &maxJLimit)) == NULL) {
        return(errCode);
    }
    return (LSBE_NO_ERROR);
} 

#define MAX_JOB_IDS 50
int
getJobIdList (char *jobIdStr, int *numJobIds, LS_LONG_INT **jobIdList)
{
    int jobId;
    LS_LONG_INT lsbJobId;
    LS_LONG_INT *temp, *jobIds;
    struct idxList *idxListP = NULL, *idx;
    int sizeOfJobIdArray = MAX_JOB_IDS;
    int i, j, errCode;

    *numJobIds = 0;
    if ((errCode = getJobIdIndexList (jobIdStr, &jobId, &idxListP)) !=
        LSBE_NO_ERROR) {
        return(errCode);
    }

    if (jobId <= 0)
        return(LSBE_BAD_JOBID);

    if ((jobIds = (LS_LONG_INT *) calloc (MAX_JOB_IDS, sizeof (LS_LONG_INT))) == NULL) {
        mbdDie(MASTER_MEM);
    }

    if (idxListP == NULL) {
        jobIds[0] = jobId;
        *numJobIds = 1;
        *jobIdList = jobIds;
        return(LSBE_NO_ERROR);
    }
    
    for (idx = idxListP; idx; idx = idx->next) {
        for (j = idx->start; j <= idx->end; j+= idx->step) {
            lsbJobId = LSB_JOBID(jobId, j);
            if (*numJobIds >= sizeOfJobIdArray) {
                sizeOfJobIdArray += MAX_JOB_IDS;
                if ((temp = (LS_LONG_INT *) realloc(jobIds,
                    sizeOfJobIdArray * sizeof(LS_LONG_INT))) == NULL) {
                    mbdDie(MASTER_MEM);
                }
                jobIds = temp;
            }
            for (i = 0; i < *numJobIds; i++)  
                if (lsbJobId == jobIds[i])
                    break;
            if (i == (*numJobIds)) {
                jobIds[(*numJobIds)++] = lsbJobId;
            }
        }
    }
    freeIdxList(idxListP);
    *jobIdList = jobIds;
    return (LSBE_NO_ERROR);
} 

void
setIdxListContext(const char *jobName)
{
    int      err;
    int      limit;
    
    
    arrayIdxList = parseJobArrayIndex( (char *) jobName,
				      &err,
				      &limit);
} 

struct idxList
*getIdxListContext(void)
{
    return(arrayIdxList);

} 

void
freeIdxListContext(void)
{
    freeIdxList(arrayIdxList);
    arrayIdxList = NULL;

} 

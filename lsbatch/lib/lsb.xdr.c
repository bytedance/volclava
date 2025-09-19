/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.xdr.c 397 2007-11-26 19:04:00Z mblack $
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

#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <stdlib.h>

#include "lsb.h"

bool_t xdr_var_string(XDR *, char **);

static int    allocLoadIdx(float **sched, float **stop, int *outSize, int size);

static bool_t xdr_lsbSharedResourceInfo (XDR *, struct lsbSharedResourceInfo *,
                                         struct LSFHeader *);

static bool_t xdr_lsbShareResourceInstance (XDR *xdrs,
					    struct lsbSharedResourceInstance *,
					    struct LSFHeader *);

static int xdrsize_shareAcctInfoEnt(struct shareAcctInfoEnt *, int *);

static bool_t xdr_shareAcctInfoEnt(struct shareAcctInfoEnt *,
                                   XDR *,
                                   struct LSFHeader *);


int  lsbSharedResConfigured_ = FALSE;

extern bool_t xdr_array_string(XDR *, char **, int, int);
extern int getXdrStrlen(char *);
extern int freeShareAcctInfoEnt(struct shareAcctInfoEnt *);

bool_t
xdr_submitReq (XDR *xdrs, struct submitReq *submitReq, struct LSFHeader *hdr)
{

    int i, nLimits;
    static int numAskedHosts = 0;
    static char **askedHosts = NULL;

    if (xdrs->x_op == XDR_DECODE) {
	submitReq->fromHost[0] = '\0';
	submitReq->jobFile[0] = '\0';
	submitReq->inFile[0] = '\0';
	submitReq->outFile[0] = '\0';
	submitReq->errFile[0] = '\0';
	submitReq->inFileSpool[0] = '\0';
	submitReq->commandSpool[0] = '\0';
	submitReq->chkpntDir[0] = '\0';
	submitReq->hostSpec[0] = '\0';
        submitReq->cwd[0] = '\0';
        submitReq->subHomeDir[0] = '\0';


	FREEUP (submitReq->queue);
        FREEUP (submitReq->command);
        FREEUP (submitReq->jobName);
        FREEUP (submitReq->preExecCmd);
        FREEUP (submitReq->postExecCmd);
        FREEUP (submitReq->dependCond);
        FREEUP (submitReq->resReq);
        FREEUP (submitReq->mailUser);
	FREEUP (submitReq->projectName);
	FREEUP (submitReq->loginShell);
	FREEUP (submitReq->schedHostType);

	if (numAskedHosts > 0) {
	    for (i = 0; i < numAskedHosts; i++)
		FREEUP (askedHosts[i]);
            FREEUP (askedHosts);
        }
        numAskedHosts = 0;
    }


    if (!(xdr_u_int(xdrs, (unsigned int*)&submitReq->options) &&
	  xdr_var_string(xdrs, &submitReq->queue) &&
	  xdr_var_string(xdrs, &submitReq->resReq) &&
	  xdr_int(xdrs, &submitReq->numProcessors))) {
        return (FALSE);
    }

    if (!(xdr_string(xdrs, &submitReq->fromHost, MAXHOSTNAMELEN) &&
         xdr_var_string(xdrs, &submitReq->dependCond) &&
         xdr_var_string(xdrs, &submitReq->jobName) &&
         xdr_var_string(xdrs, &submitReq->command) &&
         xdr_string(xdrs, &submitReq->jobFile, MAXFILENAMELEN) &&
         xdr_string(xdrs, &submitReq->inFile, MAXFILENAMELEN) &&
         xdr_string(xdrs, &submitReq->outFile, MAXFILENAMELEN) &&
         xdr_string(xdrs, &submitReq->errFile, MAXFILENAMELEN) &&
         xdr_var_string(xdrs, &submitReq->preExecCmd) &&
         xdr_var_string(xdrs, &submitReq->postExecCmd) &&
         xdr_string(xdrs, &submitReq->hostSpec, MAXHOSTNAMELEN))) {
        return (FALSE);
    }

    if (!xdr_int(xdrs, &submitReq->numAskedHosts))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE && submitReq->numAskedHosts > 0) {
	submitReq->askedHosts = (char **)
		calloc(submitReq->numAskedHosts, sizeof (char *));
	if (submitReq->askedHosts == NULL)
	    return (FALSE);
    }


    for ( i=0;i<submitReq->numAskedHosts;i++ ) {
        if (!xdr_var_string(xdrs, &submitReq->askedHosts[i])) {
	    submitReq->numAskedHosts = i;
	    goto Error0;
        }
    }

    nLimits = LSF_RLIM_NLIMITS;
    if (!xdr_int(xdrs, &nLimits))
	goto Error0;

    for (i = 0; i < nLimits; i++) {
	if (i < LSF_RLIM_NLIMITS) {
	    if (!xdr_int(xdrs, &submitReq->rLimits[i]))
		goto Error0;
	} else {
	    int j;

	    if (!xdr_int(xdrs, &j))
		goto Error0;
	}
    }

    if (!(xdr_time_t(xdrs, &submitReq->submitTime) &&
         xdr_time_t(xdrs, &submitReq->beginTime) &&
         xdr_time_t(xdrs, &submitReq->termTime) &&
         xdr_int(xdrs,&submitReq->umask) &&
         xdr_int(xdrs,&submitReq->sigValue) &&
	 xdr_int(xdrs,&submitReq->restartPid) &&
	 xdr_time_t(xdrs,&submitReq->chkpntPeriod) &&
         xdr_string(xdrs, &submitReq->chkpntDir, MAXFILENAMELEN) &&
         xdr_string(xdrs, &submitReq->subHomeDir, MAXFILENAMELEN) &&
         xdr_string(xdrs, &submitReq->cwd, MAXFILENAMELEN))) {
	goto Error0;
    }

    if (!xdr_int(xdrs, &submitReq->nxf))
	goto Error0;

    if (xdrs->x_op == XDR_DECODE && submitReq->nxf > 0) {
	if ((submitReq->xf = (struct xFile *)
	     calloc(submitReq->nxf, sizeof(struct xFile))) == NULL)
	    goto Error0;
    }

    for (i = 0; i < submitReq->nxf; i++) {
	if (!xdr_arrayElement(xdrs, (char *) &(submitReq->xf[i]),
			      hdr, xdr_xFile))
	    goto Error1;
    }



    if (!xdr_var_string(xdrs, &submitReq->mailUser))
	goto Error1;

    if (!xdr_var_string(xdrs, &submitReq->projectName))
	goto Error1;



    if (!xdr_int(xdrs, &submitReq->niosPort))
	goto Error1;
    if (!xdr_int(xdrs, &submitReq->maxNumProcessors))
	goto Error1;
    if (!xdr_var_string(xdrs, &submitReq->loginShell))
	goto Error1;
    if (!xdr_var_string(xdrs, &submitReq->schedHostType))
	goto Error1;


    if (xdrs->x_op == XDR_DECODE) {
        numAskedHosts = submitReq->numAskedHosts;
        askedHosts = submitReq->askedHosts;
    }


    if (!xdr_int(xdrs, &submitReq->options2))
        goto Error1;


    if (!(xdr_string(xdrs, &submitReq->inFileSpool, MAXFILENAMELEN) &&
          xdr_string(xdrs, &submitReq->commandSpool, MAXFILENAMELEN))) {
        return (FALSE);
    }

    if (!xdr_int(xdrs, &submitReq->userPriority))
        goto Error1;

    return (TRUE);

  Error1:
    if (xdrs->x_op == XDR_DECODE) {
	FREEUP(submitReq->xf);
	submitReq->xf = NULL;
    }

  Error0:
    if (xdrs->x_op == XDR_DECODE) {
	for (i = 0; i < submitReq->numAskedHosts; i++)
	    FREEUP(submitReq->askedHosts[i]);
	FREEUP(submitReq->askedHosts);
	submitReq->askedHosts = NULL;
	submitReq->numAskedHosts = 0;
    }

    return(FALSE);
}

bool_t
xdr_modifyReq (XDR *xdrs, struct  modifyReq *modifyReq, struct LSFHeader *hdr)
{
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_DECODE) {

        FREEUP (modifyReq->jobIdStr);
    }
    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(modifyReq->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs, &jobArrId) &&
          xdr_int(xdrs, &(modifyReq->delOptions))))
        return(FALSE);

    if (!xdr_arrayElement(xdrs, (char *) &modifyReq->submitReq,
                                                   hdr, xdr_submitReq))
        return(FALSE);


    if (!xdr_var_string(xdrs, &(modifyReq->jobIdStr)))
        return(FALSE);


    if (!(xdr_int(xdrs, &(modifyReq->delOptions2)))) {
        return(FALSE);
    }
    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }
    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&modifyReq->jobId,jobArrId,jobArrElemId);
    }

    return (TRUE);

}

bool_t
xdr_jobInfoReq (XDR *xdrs, struct jobInfoReq *jobInfoReq, struct LSFHeader *hdr)
{
    int jobArrId, jobArrElemId;

    if (!xdr_var_string(xdrs, &jobInfoReq->userName))
	return (FALSE);

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobInfoReq->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs, &jobArrId)     &&
	    xdr_int(xdrs, &(jobInfoReq->options)) &&
	    xdr_var_string(xdrs, &jobInfoReq->queue)))
        return (FALSE);

    if (!xdr_var_string(xdrs, &jobInfoReq->host))
        return (FALSE);
    if (!xdr_var_string(xdrs, &jobInfoReq->jobName))
        return (FALSE);
    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }
    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobInfoReq->jobId,jobArrId,jobArrElemId);
    }

    return(TRUE);
}

bool_t
xdr_signalReq (XDR *xdrs, struct signalReq *signalReq, struct LSFHeader *hdr)
{
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(signalReq->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs, &(signalReq->sigValue)) &&
          xdr_int(xdrs, &jobArrId)))
        return(FALSE);


    if (signalReq->sigValue == SIG_CHKPNT
       || signalReq->sigValue == SIG_DELETE_JOB
       || signalReq->sigValue == SIG_ARRAY_REQUEUE) {
	if (!(xdr_time_t(xdrs, &(signalReq->chkPeriod)) &&
              xdr_int(xdrs, &(signalReq->actFlags)))) {
	    return (FALSE);
       }
    }

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&signalReq->jobId,jobArrId,jobArrElemId);
    }

    return(TRUE);
}


bool_t
xdr_lsbMsg(XDR *xdrs, struct lsbMsg *m, struct LSFHeader *hdr)
{
    int xdrrc;
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_DECODE) {
	m->header->src[0]= '\0';
	m->header->dest[0] = '\0';
    }

    if (xdrs->x_op == XDR_FREE) {
	FREEUP(m->header->src);
	FREEUP(m->header->dest);
	FREEUP(m->msg);
	return(TRUE);
    }

    xdrrc = xdr_int(xdrs, &m->header->msgId);
    if (! xdrrc) goto Failure;

    xdrrc = xdr_int(xdrs, &m->header->usrId);
    if (! xdrrc) goto Failure;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(m->header->jobId, &jobArrId, &jobArrElemId);
    }
    xdrrc = xdr_int(xdrs, &jobArrId);
    if (! xdrrc) goto Failure;

    xdrrc = xdr_int(xdrs, &m->header->type);
    if (! xdrrc) goto Failure;

    xdrrc = xdr_string(xdrs, &m->header->src, LSB_MAX_SD_LENGTH);
    if (! xdrrc) goto Failure;

    xdrrc = xdr_string(xdrs, &m->header->dest, LSB_MAX_SD_LENGTH);
    if (! xdrrc) goto Failure;

    xdrrc = xdr_var_string(xdrs, &m->msg);
    if (! xdrrc) goto Failure;

    xdrrc = xdr_int(xdrs, &jobArrElemId);
    if (! xdrrc) goto Failure;

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&m->header->jobId,jobArrId,jobArrElemId);
    }

    return(TRUE);

  Failure:
    return(FALSE);

}

bool_t
xdr_submitMbdReply (XDR *xdrs, struct submitMbdReply *reply, struct LSFHeader *hdr)
{
    static char queueName[MAX_LSB_NAME_LEN];
    static char jobName[MAX_CMD_DESC_LEN];
    static char pendLimitReason[MAX_CMD_DESC_LEN];
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_DECODE) {
        queueName[0] = '\0';
        jobName[0] = '\0';
        pendLimitReason[0] = '\0';
        reply->queue = queueName;
        reply->badJobName = jobName;
        reply->pendLimitReason = pendLimitReason;
    }

    if (xdrs->x_op == XDR_ENCODE) {
        jobId64To32(reply->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs,&(jobArrId)) &&
    xdr_int(xdrs,&(reply->badReqIndx)) &&
    xdr_int(xdrs,&(reply->subTryInterval)) &&
    xdr_string(xdrs, &reply->queue, MAX_LSB_NAME_LEN) &&
    xdr_string(xdrs, &reply->pendLimitReason, MAX_CMD_DESC_LEN) &&
    xdr_string(xdrs, &reply->badJobName, MAX_CMD_DESC_LEN))){
        return (FALSE);
    }

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        jobId32To64(&reply->jobId,jobArrId,jobArrElemId);
    }
    return(TRUE);
}

bool_t
xdr_parameterInfo (XDR *xdrs, struct parameterInfo *paramInfo,
                   struct LSFHeader *hdr)
{

    if (xdrs->x_op == XDR_DECODE) {
        FREEUP (paramInfo->defaultQueues);
        FREEUP (paramInfo->defaultHostSpec);
	FREEUP (paramInfo->defaultProject);
	FREEUP (paramInfo->pjobSpoolDir);
    }

    if (!(xdr_var_string(xdrs, &paramInfo->defaultQueues) &&
	  xdr_var_string(xdrs, &paramInfo->defaultHostSpec)))
	return (FALSE);

    if (!(xdr_int(xdrs,&(paramInfo->mbatchdInterval)) &&
	  xdr_int(xdrs,&(paramInfo->sbatchdInterval)) &&
	  xdr_int(xdrs,&(paramInfo->subTryInterval)) &&
	  xdr_int(xdrs,&(paramInfo->jobAcceptInterval)) &&
	  xdr_int(xdrs,&(paramInfo->maxDispRetries)) &&
	  xdr_int(xdrs,&(paramInfo->maxSbdRetries)) &&
	  xdr_int(xdrs,&(paramInfo->cleanPeriod)) &&
	  xdr_int(xdrs,&(paramInfo->maxNumJobs)) &&
	  xdr_int(xdrs,&(paramInfo->maxPendJobs)) &&
	  xdr_int(xdrs,&(paramInfo->maxPendSlots)) &&
	  xdr_int(xdrs,&(paramInfo->defaultLimitIgnoreUserGroup)) &&
	  xdr_int(xdrs,&(paramInfo->pgSuspendIt))))
        return (FALSE);

    if (!(xdr_var_string(xdrs, &paramInfo->defaultProject)))
	return(FALSE);

    if (!(xdr_int(xdrs,&(paramInfo->maxJobArraySize)))) {
        return (FALSE);
    }

    if (!xdr_int(xdrs,&(paramInfo->jobTerminateInterval)))
	return(FALSE);

    if (! xdr_bool(xdrs, &paramInfo->disableUAcctMap))
        return(FALSE);

    if (!(xdr_int(xdrs,&(paramInfo->maxJobArraySize))))
        return(FALSE);

    if (!(xdr_var_string(xdrs, &paramInfo->pjobSpoolDir))) {
	return(FALSE);
    }

    if (!(xdr_int(xdrs, &paramInfo->maxUserPriority) &&
	      xdr_int(xdrs, &paramInfo->jobPriorityValue) &&
	      xdr_int(xdrs, &paramInfo->jobPriorityTime))) {
            return(FALSE);
    }

    if (!xdr_int(xdrs, &(paramInfo->maxJobId)))
    {
        return (FALSE);
    }

    if (!(xdr_int(xdrs,&(paramInfo->maxAcctArchiveNum)) &&
	  xdr_int(xdrs,&(paramInfo->acctArchiveInDays)) &&
	  xdr_int(xdrs,&(paramInfo->acctArchiveInSize)))){
	return (FALSE);
    }

    if (!(xdr_int(xdrs,&(paramInfo->jobDepLastSub)) &&
        xdr_int(xdrs,&(paramInfo->sharedResourceUpdFactor)))) {
           return (FALSE);
    }

    if (!(xdr_int(xdrs, &paramInfo->resourcePerTask))) {
        return (FALSE);
    }

    if (!(xdr_float(xdrs, &paramInfo->runTimeFactor) &&
          xdr_float(xdrs, &paramInfo->cpuTimeFactor) &&
          xdr_float(xdrs, &paramInfo->runJobFactor) &&
          xdr_float(xdrs, &paramInfo->histHours))) {
        return (FALSE);
    }

    return(TRUE);
}

bool_t
xdr_jobInfoHead (XDR *xdrs, struct jobInfoHead *jobInfoHead,
                 struct LSFHeader *hdr)
{
    static char **hostNames = NULL;
    static int numJobs = 0, numHosts = 0;
    static LS_LONG_INT *jobIds = NULL;
    char *sp;
    int *jobArrIds = NULL, *jobArrElemIds = NULL;
    int i;

    if (!(xdr_int(xdrs, &(jobInfoHead->numJobs)) &&
          xdr_int(xdrs, &(jobInfoHead->numHosts)))) {
        return (FALSE);
    }
    if ( jobInfoHead->numJobs > 0) {
	if ((jobArrIds = (int *) calloc(jobInfoHead->numJobs,
					sizeof(int))) == NULL) {
	    lsberrno = LSBE_NO_MEM;
	    return (FALSE);
	}
	if ((jobArrElemIds = (int *) calloc(jobInfoHead->numJobs,
					    sizeof(int))) == NULL) {
	    lsberrno = LSBE_NO_MEM;
	    FREEUP(jobArrIds);
	    return (FALSE);
	}
    }
    if (xdrs->x_op == XDR_DECODE) {
        if (jobInfoHead->numJobs > numJobs ) {
            FREEUP (jobIds);
            numJobs = 0;
            if ((jobIds = (LS_LONG_INT *) calloc (jobInfoHead->numJobs,
                                          sizeof(LS_LONG_INT))) == NULL) {
                lsberrno = LSBE_NO_MEM;
                return (FALSE);
            }
            numJobs = jobInfoHead->numJobs;
        }

        if (jobInfoHead->numHosts > numHosts) {
            for (i = 0; i < numHosts; i++)
                FREEUP (hostNames[i]);
            numHosts = 0;
            FREEUP (hostNames);
            if ((hostNames = (char **) calloc(jobInfoHead->numHosts,
                                          sizeof(char *))) == NULL) {
                lsberrno = LSBE_NO_MEM;
                return (FALSE);
            }
            for (i = 0; i < jobInfoHead->numHosts; i++) {
                hostNames[i] = malloc(MAXHOSTNAMELEN);
                if (!hostNames[i]) {
                    numHosts = i;
                    lsberrno = LSBE_NO_MEM;
                    return (FALSE);
                }
            }
            numHosts = jobInfoHead->numHosts;
        }
        jobInfoHead->jobIds = jobIds;
        jobInfoHead->hostNames = hostNames;
    }

    for (i = 0; i < jobInfoHead->numJobs; i++) {
	if (xdrs->x_op == XDR_ENCODE) {
	    jobId64To32(jobInfoHead->jobIds[i],
			&jobArrIds[i],
			&jobArrElemIds[i]);
	}
        if (!xdr_int(xdrs,&(jobArrIds[i])))
            return (FALSE);
    }

    for (i = 0; i < jobInfoHead->numHosts; i++) {
        sp = jobInfoHead->hostNames[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!(xdr_string(xdrs, &sp, MAXHOSTNAMELEN)))
            return (FALSE);
    }

    for (i = 0; i < jobInfoHead->numJobs; i++) {
	if (!xdr_int(xdrs,&jobArrElemIds[i])) {
	    return (FALSE);
	}
	if (xdrs->x_op == XDR_DECODE) {
	    jobId32To64(&jobInfoHead->jobIds[i],jobArrIds[i],jobArrElemIds[i]);
	}
    }

    FREEUP(jobArrIds);
    FREEUP(jobArrElemIds);
    return TRUE;
}

bool_t
xdr_jgrpInfoReply (XDR *xdrs, struct jobInfoReply *jobInfoReply,
		  struct LSFHeader *hdr)
{
    char *sp;
    int jobArrId, jobArrElemId;

    sp = jobInfoReply->userName;
    if (xdrs->x_op == XDR_DECODE) {
	sp[0] = '\0';

        FREEUP( jobInfoReply->jobBill->dependCond );
        FREEUP( jobInfoReply->jobBill->jobName );
    }

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobInfoReply->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs, &jobArrId) &&
	  xdr_int(xdrs, (int *)&(jobInfoReply->status)) &&
	  xdr_int(xdrs, &(jobInfoReply->reasons)) &&
	  xdr_int(xdrs, &(jobInfoReply->subreasons)) &&
	  xdr_time_t(xdrs, &(jobInfoReply->startTime)) &&
	  xdr_time_t(xdrs, &(jobInfoReply->predictedStartTime)) &&
	  xdr_time_t(xdrs, &(jobInfoReply->endTime)) &&
	  xdr_float(xdrs, &(jobInfoReply->cpuTime)) &&
	  xdr_int(xdrs, &(jobInfoReply->numToHosts)) &&
	  xdr_int(xdrs, &jobInfoReply->userId) &&
	  xdr_string(xdrs, &sp, MAXLSFNAMELEN))) {
        return (FALSE);
    }


    if (!(xdr_var_string(xdrs, &jobInfoReply->jobBill->dependCond) &&
	  xdr_time_t(xdrs, &jobInfoReply->jobBill->submitTime) &&
	  xdr_var_string(xdrs, &jobInfoReply->jobBill->jobName))) {
	FREEUP( jobInfoReply->jobBill->dependCond );
	FREEUP( jobInfoReply->jobBill->jobName );
	return(FALSE);
    }

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobInfoReply->jobId,jobArrId,jobArrElemId);
    }
    return(TRUE);

}
bool_t
xdr_jobInfoReply (XDR *xdrs, struct jobInfoReply *jobInfoReply,
		  struct LSFHeader *hdr)
{
    char *sp;
    int i, j;
    static float *loadSched = NULL, *loadStop = NULL;
    static int nIdx = 0;
    static int *reasonTb = NULL;
    static int nReasons = 0;
    int jobArrId, jobArrElemId;


    if (!(xdr_int(xdrs, (int *) &jobInfoReply->jType) &&
	  xdr_var_string(xdrs, &jobInfoReply->parentGroup) &&
	  xdr_var_string(xdrs, &jobInfoReply->jName))) {
	FREEUP( jobInfoReply->parentGroup );
	FREEUP( jobInfoReply->jName );
	return(FALSE);
    }
    for ( i=0; i<NUM_JGRP_COUNTERS; i++ ) {
	if (!xdr_int(xdrs, (int *) &jobInfoReply->counter[i])) {
	    FREEUP( jobInfoReply->parentGroup );
	    FREEUP( jobInfoReply->jName );
	    return(FALSE);
        }
    }
    if (jobInfoReply->jType == JGRP_NODE_GROUP) {
        return(xdr_jgrpInfoReply(xdrs, jobInfoReply,hdr));
    }


    sp = jobInfoReply->userName;
    if (xdrs->x_op == XDR_DECODE)
	sp[0] = '\0';

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobInfoReply->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs, &jobArrId) &&
	  xdr_int(xdrs, (int *)&(jobInfoReply->status)) &&
	  xdr_int(xdrs, &(jobInfoReply->reasons)) &&
	  xdr_int(xdrs, &(jobInfoReply->subreasons)) &&
	  xdr_time_t(xdrs, &(jobInfoReply->startTime)) &&
	  xdr_time_t(xdrs, &(jobInfoReply->endTime)) &&
	  xdr_float(xdrs, &(jobInfoReply->cpuTime)) &&
	  xdr_int(xdrs, &(jobInfoReply->numToHosts)) &&
	  xdr_int(xdrs, &jobInfoReply->nIdx) &&
	  xdr_int(xdrs, &jobInfoReply->numReasons) &&
	  xdr_int(xdrs, &jobInfoReply->userId) &&
	  xdr_string(xdrs, &sp, MAXLSFNAMELEN))) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	if (jobInfoReply->nIdx > nIdx) {
	    if (allocLoadIdx(&loadSched, &loadStop, &nIdx,
			     jobInfoReply->nIdx) == -1)
		return (FALSE);
	}
	jobInfoReply->loadSched = loadSched;
	jobInfoReply->loadStop = loadStop;

        if (jobInfoReply->numReasons > nReasons) {
            FREEUP (reasonTb);
            nReasons = 0;
            reasonTb = calloc (jobInfoReply->numReasons, sizeof(int));
            if (!reasonTb)
                return (FALSE);
            jobInfoReply->reasonTb = reasonTb;
            nReasons = jobInfoReply->numReasons;
        }
    }

    for (i = 0; i < jobInfoReply->nIdx; i++) {
        if (!xdr_float(xdrs, &jobInfoReply->loadSched[i]) ||
	    !xdr_float(xdrs, &jobInfoReply->loadStop[i]))
            return (FALSE);
    }

    for (i = 0; i < jobInfoReply->numReasons; i++)
        if (!xdr_int(xdrs, &jobInfoReply->reasonTb[i]))
            return (FALSE);

    if (xdrs->x_op == XDR_DECODE && jobInfoReply->numToHosts > 0 ) {
        if ((jobInfoReply->toHosts = (char **)
	         calloc(jobInfoReply->numToHosts, sizeof (char *))) == NULL) {
            jobInfoReply->numToHosts = 0;
	    return (FALSE);
        }
    }
    for ( i=0; i<jobInfoReply->numToHosts; i++ ) {
        if (xdrs->x_op == XDR_DECODE) {
	    jobInfoReply->toHosts[i] = calloc(1, MAXHOSTNAMELEN);
	    if (jobInfoReply->toHosts[i] == NULL) {
		for (j=0; j<i; j++)
		    free(jobInfoReply->toHosts[j]);
		free(jobInfoReply->toHosts);
                jobInfoReply->numToHosts = 0;
                jobInfoReply->toHosts = NULL;
		return (FALSE);
	    }
	}
	sp = jobInfoReply->toHosts[i];
        if (!xdr_string(xdrs, &sp, MAXHOSTNAMELEN)) {
	    if (xdrs->x_op == XDR_DECODE) {
		for (j=0; j<i; j++)
		    free(jobInfoReply->toHosts[j]);
		free(jobInfoReply->toHosts);
                jobInfoReply->numToHosts = 0;
                jobInfoReply->toHosts = NULL;
	    }
            return (FALSE);
	}
    }

    if (!xdr_arrayElement(xdrs, (char *) (jobInfoReply->jobBill), hdr,
			  xdr_submitReq)) {
	if (xdrs->x_op == XDR_DECODE) {
	    for (j=0; j<jobInfoReply->numToHosts; j++)
		free(jobInfoReply->toHosts[j]);
	    free(jobInfoReply->toHosts);
            jobInfoReply->numToHosts = 0;
            jobInfoReply->toHosts = NULL;
	}
        return(FALSE);
    }

    if (!(xdr_int(xdrs, (int *) &jobInfoReply->exitStatus) &&
	  xdr_int(xdrs, (int *) &jobInfoReply->execUid) &&
	  xdr_var_string(xdrs, &jobInfoReply->execHome) &&
	  xdr_var_string(xdrs, &jobInfoReply->execCwd) &&
	  xdr_var_string(xdrs, &jobInfoReply->execUsername))) {
	FREEUP( jobInfoReply->execHome );
	FREEUP( jobInfoReply->execCwd );
	FREEUP( jobInfoReply->execUsername );
	return(FALSE);
    }

    if (!xdr_time_t(xdrs, &jobInfoReply->reserveTime)
        || !xdr_int(xdrs, &jobInfoReply->jobPid))
	return(FALSE);


    if (!(xdr_time_t(xdrs, &(jobInfoReply->jRusageUpdateTime))))
        return(FALSE);
    if (!(xdr_jRusage(xdrs, &(jobInfoReply->runRusage), hdr)))
        return (FALSE);


    if (!xdr_time_t(xdrs, &(jobInfoReply->predictedStartTime))) {
        return FALSE;
    }


    if (!xdr_u_short(xdrs, &jobInfoReply->port)) {
	return FALSE;
    }

    if (!xdr_int(xdrs, &jobInfoReply->jobPriority)) {
        return FALSE;
    }

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobInfoReply->jobId,jobArrId,jobArrElemId);
    }

    if (!xdr_var_string(xdrs, &jobInfoReply->chargedSAAP)) {
        return (FALSE);
    }

    return(TRUE);

}

bool_t
xdr_queueInfoReply (XDR *xdrs, struct queueInfoReply *qInfoReply,
		    struct LSFHeader *hdr)
{
    int i;
    static int memSize = 0;
    static struct queueInfoEnt *qInfo = NULL;
    static int nIdx = 0;
    static float *loadStop = NULL, *loadSched = NULL;

    if (!(xdr_int(xdrs, &(qInfoReply->numQueues))
	  && xdr_int(xdrs, &(qInfoReply->badQueue))
	  && xdr_int(xdrs, &qInfoReply->nIdx)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE) {

	if (qInfoReply->numQueues > memSize) {
	    for (i = 0; i < memSize; i++) {
		FREEUP (qInfo[i].queue);
		FREEUP (qInfo[i].hostSpec);
	    }
	    FREEUP (qInfo);


	    memSize = 0;
	    if (!(qInfo = (struct queueInfoEnt *)
		  calloc(qInfoReply->numQueues, sizeof (struct queueInfoEnt))))
		return (FALSE);

	    for (i = 0; i < qInfoReply->numQueues; i++) {
		qInfo[i].queue = NULL;
		qInfo[i].hostSpec = NULL;
		memSize = i + 1;
		if (!(qInfo[i].queue = malloc (MAX_LSB_NAME_LEN))
		    || !(qInfo[i].hostSpec = malloc (MAX_LSB_NAME_LEN)))
		    return (FALSE);
	    }
	}


	for (i = 0; i < qInfoReply->numQueues; i++) {
	    FREEUP (qInfo[i].description);
	    FREEUP (qInfo[i].userList);
	    FREEUP (qInfo[i].hostList);
	    FREEUP (qInfo[i].windows);
            FREEUP (qInfo[i].windowsD);
            FREEUP (qInfo[i].defaultHostSpec);
            FREEUP (qInfo[i].admins);
            FREEUP (qInfo[i].preCmd);
	    FREEUP (qInfo[i].postCmd);
	    FREEUP (qInfo[i].prepostUsername);
	    FREEUP (qInfo[i].requeueEValues);
	    FREEUP (qInfo[i].resReq);
	    FREEUP (qInfo[i].resumeCond);
	    FREEUP (qInfo[i].stopCond);
	    FREEUP (qInfo[i].jobStarter);
	    FREEUP (qInfo[i].chkpntDir);

;
            FREEUP (qInfo[i].suspendActCmd);
            FREEUP (qInfo[i].resumeActCmd);
            FREEUP (qInfo[i].terminateActCmd);
            FREEUP (qInfo[i].userShares);
            if (qInfo[i].shareAcctTree) {
                freeShareAcctInfoEnt(qInfo[i].shareAcctTree);
                FREEUP(qInfo[i].shareAcctTree);
            }
            FREEUP (qInfo[i].actionComment);
        }

	qInfoReply->queues = qInfo;


	if (qInfoReply->numQueues * qInfoReply->nIdx > nIdx
	    && qInfoReply->numQueues > 0) {
	    if (allocLoadIdx(&loadSched, &loadStop, &nIdx,
			     qInfoReply->numQueues * qInfoReply->nIdx) == -1)
		return (FALSE);
	}
    }

    for (i = 0; i < qInfoReply->numQueues; i++) {
	if (xdrs->x_op == XDR_DECODE) {
	    qInfoReply->queues[i].loadSched = loadSched +
		(i * qInfoReply->nIdx);
	    qInfoReply->queues[i].loadStop = loadStop + (i * qInfoReply->nIdx);
	}

        if (!xdr_arrayElement(xdrs, (char *) &(qInfoReply->queues[i]),
                              hdr, xdr_queueInfoEnt, &qInfoReply->nIdx))
            return(FALSE);
    }

    return (TRUE);

}

bool_t
xdr_queueInfoEnt (XDR *xdrs, struct queueInfoEnt *qInfo,
                  struct LSFHeader *hdr, int *nIdx)
{
    char* sp;
    int   i;
    int   j;

    if (xdrs->x_op == XDR_FREE) {
	if (qInfo->chkpntDir != 0) {
	    FREEUP(qInfo->chkpntDir);
	}
	return(TRUE);
    }

    sp = qInfo->queue;
    if (xdrs->x_op == XDR_DECODE) {
	sp[0] = '\0';
        qInfo->suspendActCmd = NULL;
        qInfo->resumeActCmd = NULL;
        qInfo->terminateActCmd = NULL;
    }
    if (!(xdr_string(xdrs, &sp, MAX_LSB_NAME_LEN) &&
          xdr_var_string(xdrs, &qInfo->description)))
        return (FALSE);

    if (!(xdr_var_string(xdrs, &qInfo->userList)))
        return (FALSE);

    if (!(xdr_var_string(xdrs, &qInfo->hostList)))
        return (FALSE);

    if (!(xdr_var_string(xdrs, &qInfo->windows)))
	return (FALSE);

    sp = qInfo->hostSpec;
    if (xdrs->x_op == XDR_DECODE)
        sp[0] = '\0';
    if (!(xdr_string(xdrs, &sp, MAX_LSB_NAME_LEN)))
        return (FALSE);

    for(i=0; i<LSF_RLIM_NLIMITS; i++) {
	if( !(xdr_int(xdrs, &qInfo->rLimits[i])))
	    return (FALSE);
    }

    qInfo->nIdx = *nIdx;
    for (i=0; i<*nIdx; i++) {
        if (! (xdr_float(xdrs, &(qInfo->loadSched[i])))
            || ! (xdr_float(xdrs, &(qInfo->loadStop[i]))))
            return (FALSE);
    }

    if (!(xdr_int(xdrs, &qInfo->priority) &&
          xdr_short(xdrs, &qInfo->nice) &&
          xdr_int(xdrs, &qInfo->userJobLimit) &&
          xdr_float(xdrs, &qInfo->procJobLimit) &&
	  xdr_int(xdrs, &qInfo->qAttrib) &&
	  xdr_int(xdrs, &qInfo->qStatus) &&
          xdr_int(xdrs, &qInfo->maxJobs) &&
          xdr_int(xdrs, &qInfo->numJobs) &&
          xdr_int(xdrs, &qInfo->numPEND) &&
          xdr_int(xdrs, &qInfo->numRUN) &&
          xdr_int(xdrs, &qInfo->numSSUSP) &&
          xdr_int(xdrs, &qInfo->numUSUSP) &&
	  xdr_int(xdrs, &qInfo->mig) &&
          xdr_int(xdrs, &qInfo->acceptIntvl) &&
          xdr_int(xdrs, &qInfo->schedDelay))) {
        return (FALSE);
    }

    if (!(xdr_var_string(xdrs, &qInfo->windowsD) &&
          xdr_var_string(xdrs, &qInfo->defaultHostSpec)))
        return (FALSE);

    if (!(xdr_int(xdrs, &qInfo->procLimit) &&
          xdr_var_string(xdrs, &qInfo->admins) &&
          xdr_var_string(xdrs, &qInfo->preCmd) &&
          xdr_var_string(xdrs, &qInfo->postCmd) &&
          xdr_var_string(xdrs, &qInfo->prepostUsername) &&
	  xdr_var_string(xdrs, &qInfo->requeueEValues) &&
          xdr_int(xdrs, &qInfo->hostJobLimit)))
        return (FALSE);

    if (!(xdr_var_string(xdrs, &qInfo->resReq) &&
          xdr_int(xdrs, &qInfo->numRESERVE) &&
          xdr_int(xdrs, &qInfo->slotHoldTime) &&
          xdr_var_string(xdrs, &qInfo->resumeCond) &&
          xdr_var_string(xdrs, &qInfo->stopCond) &&
          xdr_var_string(xdrs, &qInfo->jobStarter) &&
          xdr_var_string(xdrs, &qInfo->suspendActCmd) &&
          xdr_var_string(xdrs, &qInfo->resumeActCmd) &&
          xdr_var_string(xdrs, &qInfo->terminateActCmd)))
        return (FALSE);

    for (j=0; j < LSB_SIG_NUM ; j++) {
        if (!xdr_int(xdrs, &qInfo->sigMap[j]))
            return (FALSE);
    }

    if ( !( xdr_var_string(xdrs, &qInfo->chkpntDir) &&
 	    xdr_int (xdrs, &qInfo->chkpntPeriod) ))
	return FALSE;

    for(i=0; i<LSF_RLIM_NLIMITS; i++) {
        if( !(xdr_int(xdrs, &qInfo->defLimits[i])))
            return (FALSE);
    }

    if ( !( xdr_int (xdrs, &qInfo->minProcLimit)
        && xdr_int (xdrs, &qInfo->defProcLimit ) ) ) {
        return FALSE;
    }

    if (!(xdr_var_string(xdrs, &qInfo->userShares))) {
        return FALSE;
    }

    if (xdrs->x_op == XDR_DECODE) {
        if (qInfo->userShares && strlen(qInfo->userShares) > 0) {
            qInfo->shareAcctTree = (struct shareAcctInfoEnt *)calloc(1, sizeof(struct shareAcctInfoEnt));
            if (!qInfo->shareAcctTree) {
                return FALSE;
            }
        }
    }

    if (!(xdr_float(xdrs, &qInfo->fsFactors.cpuTimeFactor) && 
          xdr_float(xdrs, &qInfo->fsFactors.runTimeFactor) &&
          xdr_float(xdrs, &qInfo->fsFactors.runJobFactor) &&
          xdr_float(xdrs, &qInfo->fsFactors.histHours))) {
        return FALSE;
    }    
 
    if (qInfo->shareAcctTree) {
        if (!traverseSATree(qInfo->shareAcctTree, TRAVERSE_FS_SACCTXDR, xdr_shareAcctInfoEnt, xdrs, hdr)) {
            return FALSE;
        }   
    }

    if (!(xdr_var_string(xdrs, &qInfo->actionComment))) {
        return FALSE;
    }

    return(TRUE);
}

bool_t
xdr_infoReq (XDR *xdrs, struct infoReq *infoReq,
                 struct LSFHeader *hdr)
{
    int i;
    static int memSize = 0;
    static char **names = NULL, *resReq = NULL;


    if (!(xdr_int(xdrs, &(infoReq->options))))
        return (FALSE);


    if (!(xdr_int(xdrs, &(infoReq->numNames))))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE) {

        if (names) {
            for (i = 0; i < memSize; i++)
                FREEUP (names[i]);
        }
        if (infoReq->numNames + 2 > memSize) {

            FREEUP (names);

            memSize = infoReq->numNames + 2;
            if ((names = (char **)calloc (memSize, sizeof(char *))) == NULL) {
                memSize = 0;
                return(FALSE);
            }
        }
	infoReq->names = names;
    }


    for (i = 0; i < infoReq->numNames; i++)
	if (!(xdr_var_string(xdrs, &infoReq->names[i])))
	    return (FALSE);


    if (infoReq->options & CHECK_HOST) {
        if (!(xdr_var_string(xdrs, &infoReq->names[i])))
            return (FALSE);
        i++;
    }
    if (infoReq->options & CHECK_USER) {
        if (!(xdr_var_string(xdrs, &infoReq->names[i])))
            return (FALSE);
    }


    if (xdrs->x_op == XDR_DECODE) {
        FREEUP (resReq);
    }

    if (!xdr_var_string(xdrs, &infoReq->resReq))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE) {
        resReq = infoReq->resReq;
    }

    return(TRUE);

}

bool_t
xdr_hostDataReply(XDR *xdrs,
                  struct hostDataReply *hostDataReply,
                  struct LSFHeader *hdr)
{
    int i, hostCount;
    struct hostInfoEnt *hInfoTmp ;
    char *sp;
    static struct hostInfoEnt *hInfo = NULL;
    static int curNumHInfo = 0;
    static char *mem = NULL;
    static float *loadSched = NULL, *loadStop = NULL, *load = NULL;
    static float *realLoad = NULL;
    static int nIdx = 0, *busySched = NULL, *busyStop = NULL;

    if (! xdr_int(xdrs, &hostDataReply->numHosts)
        || !xdr_int(xdrs, &hostDataReply->badHost)
        || !xdr_int(xdrs, &hostDataReply->nIdx))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE) {
        hostCount = hostDataReply->numHosts;

        if (curNumHInfo < hostCount) {
            hInfoTmp = calloc(hostCount, sizeof(struct hostInfoEnt));
            if (hInfoTmp == NULL) {
                lsberrno = LSBE_NO_MEM;
                return(FALSE);
            }
            if (!(sp = malloc(hostCount
                              * (MAXHOSTNAMELEN + MAXLINELEN + MAXLINELEN)))) {
                FREEUP(hInfoTmp);
                lsberrno = LSBE_NO_MEM;
                return(FALSE);
            }
            if (hInfo != NULL){
                FREEUP(mem);
                FREEUP(hInfo);
            }
            hInfo = hInfoTmp;
            curNumHInfo = hostCount;
	    mem = sp;
            for (i = 0; i < hostCount; i++ ){
    	        hInfo[i].host = sp;
    	        sp += MAXHOSTNAMELEN;
    	        hInfo[i].windows = sp;
    	        sp += MAXLINELEN;
                hInfo[i].actionComment = sp;
                sp += MAXLINELEN;
            }
        }
        hostDataReply->hosts = hInfo;

	if (hostDataReply->nIdx * hostDataReply->numHosts > nIdx) {
	    if (allocLoadIdx(&loadSched,
                             &loadStop,
                             &nIdx,
			     hostDataReply->nIdx * hostDataReply->numHosts) == -1)
		return FALSE;
	}
        FREEUP(load);
	FREEUP(realLoad);
	FREEUP(busySched);
	FREEUP(busyStop);

        if (hostDataReply->numHosts > 0
	    && (realLoad = malloc(hostDataReply->nIdx
                                  * hostDataReply->numHosts * sizeof(float))) == NULL)
	    return (FALSE);

        if (hostDataReply->numHosts > 0
	    && (load = malloc(hostDataReply->nIdx
			* hostDataReply->numHosts * sizeof(float))) == NULL)
            return (FALSE);
        if (hostDataReply->numHosts > 0
	    && (busySched = malloc(GET_INTNUM (hostDataReply->nIdx)
                                   * hostDataReply->numHosts * sizeof(int))) == NULL)
            return (FALSE);
        if (hostDataReply->numHosts > 0
	    && (busyStop = (int *) malloc(GET_INTNUM (hostDataReply->nIdx)
			* hostDataReply->numHosts * sizeof(int))) == NULL)
            return (FALSE);
    }

    for (i = 0; i < hostDataReply->numHosts; i++) {
	if (xdrs->x_op == XDR_DECODE) {
	    hostDataReply->hosts[i].loadSched =
		loadSched + (i * hostDataReply->nIdx);
	    hostDataReply->hosts[i].loadStop =
		loadStop + (i * hostDataReply->nIdx);
            hostDataReply->hosts[i].realLoad =
		realLoad + (i * hostDataReply->nIdx);
	    hostDataReply->hosts[i].load =
		load + (i * hostDataReply->nIdx);
            hostDataReply->hosts[i].busySched =
		busySched + (i * GET_INTNUM (hostDataReply->nIdx));
            hostDataReply->hosts[i].busyStop =
		busyStop + (i * GET_INTNUM (hostDataReply->nIdx));
	}

	if (!xdr_arrayElement(xdrs,
                              (char *)&(hostDataReply->hosts[i]),
                              hdr,
                              xdr_hostInfoEnt,
                              (char *) &hostDataReply->nIdx))
            return FALSE;
    }

    if (!xdr_int(xdrs, &(hostDataReply->flag))) {
        return FALSE;
    }

    if (xdrs->x_op == XDR_DECODE) {
        if (hostDataReply->flag & LOAD_REPLY_SHARED_RESOURCE)
            lsbSharedResConfigured_ = TRUE;
    }

    return TRUE;
}

bool_t
xdr_hostInfoEnt (XDR *xdrs, struct hostInfoEnt *hostInfoEnt,
		 struct LSFHeader *hdr, int *nIdx)
{
    char *sp = hostInfoEnt->host;
    char *wp = hostInfoEnt->windows;
    char *cmt = hostInfoEnt->actionComment;
    int i;

    if (xdrs->x_op == XDR_DECODE) {
	sp[0] = '\0';
	wp[0] = '\0';
        cmt[0] = '\0';
    }

    if (!(xdr_string(xdrs, &sp, MAXHOSTNAMELEN) &&
        xdr_float(xdrs,&hostInfoEnt->cpuFactor) &&
	xdr_string(xdrs, &wp, MAXLINELEN) &&
        xdr_int(xdrs, &hostInfoEnt->userJobLimit) &&
        xdr_int(xdrs, &hostInfoEnt->maxJobs) &&
        xdr_int(xdrs, &hostInfoEnt->numJobs) &&
        xdr_int(xdrs, &hostInfoEnt->numRUN) &&
        xdr_int(xdrs, &hostInfoEnt->numSSUSP) &&
        xdr_int(xdrs, &hostInfoEnt->numUSUSP) &&
	xdr_int(xdrs, &hostInfoEnt->hStatus) &&
	xdr_int(xdrs, &hostInfoEnt->attr) &&
        xdr_int(xdrs, &hostInfoEnt->mig) &&
        xdr_string(xdrs, &cmt, MAXLINELEN)))
        return (FALSE);

    hostInfoEnt->nIdx = *nIdx;

    for (i = 0; i < GET_INTNUM (*nIdx); i++) {
	if (!xdr_int(xdrs, &hostInfoEnt->busySched[i])
	    || !xdr_int(xdrs, &hostInfoEnt->busyStop[i]))
	    return (FALSE);
    }
    for (i = 0; i < *nIdx; i++) {
	if (!xdr_float(xdrs,&hostInfoEnt->loadSched[i]) ||
	    !xdr_float(xdrs,&hostInfoEnt->loadStop[i]))
	    return (FALSE);
    }

    for (i = 0; i < *nIdx; i++) {
        if (!xdr_float(xdrs,&hostInfoEnt->realLoad[i]))
	    return (FALSE);
	if (!xdr_float(xdrs,&hostInfoEnt->load[i]))
	    return (FALSE);
    }
    if (!xdr_int(xdrs, &hostInfoEnt->numRESERVE))
        return (FALSE);

    return(TRUE);
}

bool_t
xdr_userInfoReply (XDR *xdrs, struct userInfoReply *userInfoReply,
                 struct LSFHeader *hdr)
{
    int i;
    struct userInfoEnt *uInfoTmp;
    char *sp;

    static struct userInfoEnt *uInfo = NULL;
    static int curNumUInfo = 0;
    static char *mem = NULL;

    if (!(xdr_int(xdrs,&(userInfoReply->numUsers))
        && xdr_int(xdrs,&(userInfoReply->badUser))))
        return (FALSE);


    if (xdrs->x_op == XDR_DECODE) {
        if (curNumUInfo < userInfoReply->numUsers) {
            uInfoTmp = (struct userInfoEnt *)
                       calloc(userInfoReply->numUsers,
                                               sizeof(struct userInfoEnt));
            if (uInfoTmp == NULL) {
                lsberrno = LSBE_NO_MEM;
                return(FALSE);
            }
            if (!(sp = malloc(userInfoReply->numUsers * MAX_LSB_NAME_LEN))) {
                free(uInfoTmp);
                lsberrno = LSBE_NO_MEM;
                return(FALSE);
            }
            if (uInfo != NULL){
                free(mem);
                free(uInfo);
            }
            uInfo = uInfoTmp;
            curNumUInfo = userInfoReply->numUsers;
	    mem = sp;
            for ( i = 0; i < userInfoReply->numUsers; i++ ){
                uInfo[i].user = sp;
                sp += MAX_LSB_NAME_LEN;
            }
        }
        userInfoReply->users = uInfo;
    }

    for (i = 0; i < userInfoReply->numUsers; i++ )
        if (!xdr_arrayElement(xdrs, (char *)&(userInfoReply->users[i]), hdr,
                              xdr_userInfoEnt)) {
            return (FALSE);
        }

    return (TRUE);

}

bool_t
xdr_userInfoEnt (XDR *xdrs, struct userInfoEnt *userInfoEnt,
                 struct LSFHeader *hdr)
{
    char *sp;

    sp = userInfoEnt->user;
    if (xdrs->x_op == XDR_DECODE)
	sp[0] = '\0';

    if (!(xdr_string(xdrs, &sp, MAX_LSB_NAME_LEN) &&
        xdr_float(xdrs, &userInfoEnt->procJobLimit) &&
        xdr_int(xdrs, &userInfoEnt->maxJobs) &&
        xdr_int(xdrs, &userInfoEnt->maxPendJobs) &&
        xdr_int(xdrs, &userInfoEnt->maxPendSlots) &&
        xdr_int(xdrs, &userInfoEnt->numStartJobs)))

        return (FALSE);


    if (!(xdr_int(xdrs, &userInfoEnt->numJobs) &&
            xdr_int(xdrs, &userInfoEnt->numPEND) &&
            xdr_int(xdrs, &userInfoEnt->numPENDJobs) &&
            xdr_int(xdrs, &userInfoEnt->numRUN) &&
            xdr_int(xdrs, &userInfoEnt->numSSUSP) &&
            xdr_int(xdrs, &userInfoEnt->numUSUSP)))
        return (FALSE);

    if (!xdr_int(xdrs, &userInfoEnt->numRESERVE))
        return (FALSE);

    return(TRUE);
}


bool_t
xdr_jobPeekReq (XDR *xdrs, struct jobPeekReq *jobPeekReq, struct LSFHeader *hdr)
{

    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobPeekReq->jobId, &jobArrId, &jobArrElemId);
    }
    if (!xdr_int(xdrs, &jobArrId))
        return (FALSE);

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobPeekReq->jobId,jobArrId,jobArrElemId);
    }

    return(TRUE);
}

bool_t
xdr_jobPeekReply (XDR *xdrs, struct jobPeekReply *jobPeekReply,
		 struct LSFHeader *hdr)
{
    static char outFile[MAXFILENAMELEN];

    if (xdrs->x_op == XDR_DECODE) {
        outFile [0] = '\0';
        jobPeekReply->outFile = outFile;
    }

    if (!xdr_string(xdrs, &(jobPeekReply->outFile), MAXFILENAMELEN))
        return (FALSE);


    if (!(xdr_var_string(xdrs, &jobPeekReply->pSpoolDir))) {
        return(FALSE);
    }

    return(TRUE);
}


bool_t
xdr_groupInfoReply (XDR *xdrs, struct groupInfoReply *groupInfoReply,
		    struct LSFHeader *hdr)
{
    int i, j;

    if (xdrs->x_op == XDR_FREE) {
        for (i = 0; i < groupInfoReply->numGroups; i++) {
            FREEUP(groupInfoReply->groups[i].group);
            FREEUP(groupInfoReply->groups[i].memberList);
            for (j = 0; j < groupInfoReply->groups[i].numUserShares; j++) {
                FREEUP(groupInfoReply->groups[i].userShares[j].name);
            }
            FREEUP(groupInfoReply->groups->userShares);
        }
        FREEUP(groupInfoReply->groups);
        groupInfoReply->numGroups = 0;
        return(TRUE);
    }


    if (xdrs->x_op == XDR_DECODE) {
        groupInfoReply->numGroups = 0;
        groupInfoReply->groups = NULL;
    }


    if (!xdr_int(xdrs, &(groupInfoReply->numGroups))) {
        return (FALSE);
    }


    if (xdrs->x_op == XDR_DECODE &&  groupInfoReply->numGroups != 0) {
        groupInfoReply->groups = (struct groupInfoEnt *)calloc(groupInfoReply->numGroups, sizeof(struct groupInfoEnt));
        if (groupInfoReply->groups == NULL) {
            return (FALSE);
        }
    }


    for (i = 0; i < groupInfoReply->numGroups; i++) {
        if (!xdr_arrayElement(xdrs,
                              (char *)&(groupInfoReply->groups[i]),
                              hdr,
                              xdr_groupInfoEnt)) {
            return (FALSE);
        }
    }

    if (xdrs->x_op == XDR_FREE) {
        FREEUP(groupInfoReply->groups);
    }

    return (TRUE);
}


bool_t
xdr_groupInfoEnt (XDR *xdrs, struct groupInfoEnt *gInfo,
		  struct LSFHeader *hdr)
{
    int i;
    if (xdrs->x_op == XDR_DECODE) {
        gInfo->group = NULL;
        gInfo->memberList = NULL;
        gInfo->numUserShares = 0;
        gInfo->userShares = NULL;
    }


    if (!xdr_var_string(xdrs, &(gInfo->group))
        || !xdr_var_string(xdrs, &(gInfo->memberList))
        || !xdr_int(xdrs, &(gInfo->numUserShares))) {
        return(FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        gInfo->userShares = (struct userShares *)calloc(gInfo->numUserShares, sizeof(struct userShares));
        if (! gInfo->userShares) {
            gInfo->numUserShares = 0;
            return(FALSE);
        }
    }

    for (i = 0; i < gInfo->numUserShares; i++) {
        if (!xdr_var_string(xdrs, &(gInfo->userShares[i].name)) 
            || !xdr_int(xdrs, &(gInfo->userShares[i].share))) {
            return(FALSE);
        }
    }

    if (xdrs->x_op == XDR_FREE) {
        FREEUP(gInfo->group);
        FREEUP(gInfo->memberList);
    }

    return (TRUE);

}

bool_t
xdr_controlReq (XDR *xdrs, struct controlReq *controlReq,
                  struct LSFHeader *hdr)
{
    static char *sp = NULL;
    static char *msg = NULL;
    static int first = TRUE;

    if (xdrs->x_op == XDR_DECODE) {
        if (first == TRUE) {
             sp = (char *)malloc(MAXHOSTNAMELEN);
             if (sp == NULL)
                 return (FALSE);

             msg = (char *)calloc(MAXLINELEN, sizeof(char));
             if (msg == NULL)
                 return (FALSE);

             first = FALSE;
        }
        controlReq->name = sp;
        controlReq->msg = msg;
        sp[0] = '\0';
        msg[0] = '\0';
    } else {
        if (!first) {
            FREEUP(sp);
            FREEUP(msg);
            first = TRUE;
        }
        if (controlReq->name == NULL) {
            sp = "";
        } else {
            sp = controlReq->name;
        }
        if (controlReq->msg == NULL) {
            msg = "";
        } else {
            msg = controlReq->msg;
        }
    }
    if (!(xdr_int(xdrs, &controlReq->opCode) &&
	  xdr_string(xdrs, &sp, MAXHOSTNAMELEN) &&
          xdr_string(xdrs, &msg, MAXLINELEN)))
	return FALSE;

    return TRUE;
}

bool_t
xdr_jobSwitchReq (XDR *xdrs, struct jobSwitchReq *jobSwitchReq,
		struct LSFHeader *hdr)
{
    char *sp;
    int jobArrId, jobArrElemId;

    sp = jobSwitchReq->queue;
    if (xdrs->x_op == XDR_DECODE)
	sp[0] = '\0';

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobSwitchReq->jobId, &jobArrId, &jobArrElemId);
    }
    if ( !(xdr_int(xdrs, &jobArrId) &&
	   xdr_string(xdrs, &sp, MAX_LSB_NAME_LEN)))
	return (FALSE);
    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobSwitchReq->jobId,jobArrId,jobArrElemId);
    }
    return(TRUE);
}

bool_t
xdr_jobMoveReq (XDR *xdrs, struct jobMoveReq *jobMoveReq, struct LSFHeader *hdr)
{
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobMoveReq->jobId, &jobArrId, &jobArrElemId);
    }
    if ( !(xdr_int(xdrs,&(jobMoveReq->opCode)) &&
  	  xdr_int(xdrs, &jobArrId) &&
	  xdr_int(xdrs, &(jobMoveReq->position))) )
	return (FALSE);
    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobMoveReq->jobId,jobArrId,jobArrElemId);
    }
    return(TRUE);
}

bool_t
xdr_migReq (XDR *xdrs, struct migReq *req, struct LSFHeader *hdr)
{
    char *sp;
    int i;
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(req->jobId, &jobArrId, &jobArrElemId);
    }
    if (!(xdr_int(xdrs, &jobArrId) &&
	  xdr_int(xdrs, &req->options)))
	return (FALSE);

    if (!xdr_int(xdrs, &req->numAskedHosts))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE && req->numAskedHosts > 0) {
	req->askedHosts = (char **)
	    calloc(req->numAskedHosts, sizeof (char *));
	if (req->askedHosts == NULL)
	    return (FALSE);
	for (i = 0; i < req->numAskedHosts; i++) {
	    req->askedHosts[i] = calloc (1, MAXHOSTNAMELEN);
	    if (req->askedHosts[i] == NULL) {
		while (i--)
		    free(req->askedHosts[i]);
		free(req->askedHosts);
		return (FALSE);
	    }
	}
    }


    for (i = 0; i < req->numAskedHosts; i++) {
        sp = req->askedHosts[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXHOSTNAMELEN)) {
	    for (i = 0; i < req->numAskedHosts; i++)
		free(req->askedHosts[i]);
	    free(req->askedHosts);
	    return (FALSE);
	}
    }
    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&req->jobId,jobArrId,jobArrElemId);
    }

    return(TRUE);
}



bool_t
xdr_xFile(XDR *xdrs, struct xFile *xf, struct LSFHeader *hdr)
{
    char *sp;

    if (xdrs->x_op == XDR_DECODE) {
	xf->subFn[0] = '\0';
	xf->execFn[0] = '\0';
    }
    sp = xf->subFn;

    if (!xdr_string(xdrs, &sp, MAXFILENAMELEN))
	return (FALSE);

    sp = xf->execFn;
    if (!xdr_string(xdrs, &sp, MAXFILENAMELEN))
	return (FALSE);

    if (!xdr_int(xdrs, &xf->options))
	return (FALSE);

    return (TRUE);
}


static int
allocLoadIdx(float **loadSched, float **loadStop, int *outSize, int size)
{
    if (*loadSched)
	free(*loadSched);
    if (*loadStop)
	free(*loadStop);

    *outSize = 0;

    if ((*loadSched = (float *) calloc(size, sizeof(float))) == NULL)
	return (-1);

    if ((*loadStop = (float *) calloc(size, sizeof(float))) == NULL)
	return (-1);

    *outSize = size;
    return (0);
}


bool_t
xdr_lsbShareResourceInfoReply(XDR *xdrs,
                struct  lsbShareResourceInfoReply *lsbShareResourceInfoReply,
	        struct LSFHeader *hdr)
{
    int i, status;

    if (xdrs->x_op == XDR_DECODE) {
	lsbShareResourceInfoReply->numResources = 0;
	lsbShareResourceInfoReply->resources = NULL;
    }
    if (!(xdr_int(xdrs, &lsbShareResourceInfoReply->numResources)
         && xdr_int(xdrs, &lsbShareResourceInfoReply->badResource)))
	return FALSE;

    if (xdrs->x_op == XDR_DECODE &&  lsbShareResourceInfoReply->numResources > 0) {
        if ((lsbShareResourceInfoReply->resources = (struct lsbSharedResourceInfo *)
             malloc (lsbShareResourceInfoReply->numResources
			* sizeof (struct lsbSharedResourceInfo))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }
    }
    for (i = 0; i < lsbShareResourceInfoReply->numResources; i++) {
	status = xdr_arrayElement(xdrs,
		    (char *)&lsbShareResourceInfoReply->resources[i],
		    hdr,
		    xdr_lsbSharedResourceInfo);
	if (! status) {
	    lsbShareResourceInfoReply->numResources = i;

            return FALSE;
        }
    }
    if (xdrs->x_op == XDR_FREE && lsbShareResourceInfoReply->numResources > 0) {
	FREEUP(lsbShareResourceInfoReply->resources);
	lsbShareResourceInfoReply->numResources = 0;
    }
    return TRUE;
}


static bool_t
xdr_lsbSharedResourceInfo (XDR *xdrs, struct
                           lsbSharedResourceInfo *lsbSharedResourceInfo,
			   struct LSFHeader *hdr)
{
    int i, status;

    if (xdrs->x_op == XDR_DECODE) {
	lsbSharedResourceInfo->resourceName = NULL;
	lsbSharedResourceInfo->instances = NULL;
	lsbSharedResourceInfo->nInstances = 0;
    }
    if (!(xdr_var_string (xdrs, &lsbSharedResourceInfo->resourceName) &&
          xdr_int(xdrs, &lsbSharedResourceInfo->nInstances)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE &&  lsbSharedResourceInfo->nInstances > 0) {
        if ((lsbSharedResourceInfo->instances =
             (struct lsbSharedResourceInstance *) malloc (lsbSharedResourceInfo->nInstances * sizeof (struct lsbSharedResourceInstance))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }
    }
    for (i = 0; i < lsbSharedResourceInfo->nInstances; i++) {
	status = xdr_arrayElement(xdrs,
		    (char *)&lsbSharedResourceInfo->instances[i],
		    hdr,
		    xdr_lsbShareResourceInstance);
	if (! status) {
	    lsbSharedResourceInfo->nInstances = i;
            return FALSE;
        }
    }
    if (xdrs->x_op == XDR_FREE && lsbSharedResourceInfo->nInstances > 0) {
	FREEUP (lsbSharedResourceInfo->instances);
	lsbSharedResourceInfo->nInstances = 0;
    }
    return TRUE;
}

static bool_t
xdr_lsbShareResourceInstance (XDR *xdrs,
                           struct  lsbSharedResourceInstance *instance,
			   struct LSFHeader *hdr)
{

    if (xdrs->x_op == XDR_DECODE) {
	instance->totalValue = NULL;
        instance->rsvValue = NULL;
	instance->hostList = NULL;
	instance->nHosts = 0;
    }
    if (!(xdr_var_string (xdrs, &instance->totalValue)
        && xdr_var_string (xdrs, &instance->rsvValue)
        && xdr_int(xdrs, &instance->nHosts)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE &&  instance->nHosts > 0) {
        if ((instance->hostList = (char **)
		   malloc (instance->nHosts * sizeof (char *))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }
    }
    if (! xdr_array_string(xdrs, instance->hostList,
	       MAXHOSTNAMELEN, instance->nHosts)) {
        if (xdrs->x_op == XDR_DECODE) {
	    FREEUP(instance->hostList);
	    instance->nHosts = 0;
        }
	return (FALSE);
    }
    if (xdrs->x_op == XDR_FREE && instance->nHosts > 0) {
	FREEUP (instance->hostList);
	instance->nHosts = 0;
    }
    return TRUE;
}

bool_t
xdr_runJobReq(XDR*                   xdrs,
	      struct runJobRequest*  runJobRequest,
	      struct LSFHeader*      lsfHeader)
{
    int i;
    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(runJobRequest->jobId, &jobArrId, &jobArrElemId);
    }
    if (!xdr_int(xdrs, &(runJobRequest->numHosts)) ||
	!xdr_int(xdrs, &jobArrId) ||
	!xdr_int(xdrs, &(runJobRequest->options))) {
	return(FALSE);
    }


    if (xdrs->x_op == XDR_DECODE) {
	runJobRequest->hostname = (char **)calloc(runJobRequest->numHosts,
					      sizeof(char *));
	if (runJobRequest->hostname == NULL)
	    return(FALSE);
    }

    for (i = 0; i < runJobRequest->numHosts; i++) {
	if (!xdr_var_string(xdrs, &(runJobRequest->hostname[i]))) {
	    return(FALSE);
	}
    }

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&runJobRequest->jobId,jobArrId,jobArrElemId);
    }
    if (xdrs->x_op == XDR_FREE) {
	runJobRequest->numHosts = 0;
	FREEUP(runJobRequest->hostname);
    }

    return(TRUE);
}

bool_t
xdr_jobAttrReq(XDR *xdrs, struct jobAttrInfoEnt *jobAttr, struct LSFHeader *hdr)
{
    char *sp = jobAttr->hostname;

    int jobArrId, jobArrElemId;

    if (xdrs->x_op == XDR_ENCODE) {
	jobId64To32(jobAttr->jobId, &jobArrId, &jobArrElemId);
    }
    if (!xdr_int(xdrs, &jobArrId)) {
        return(FALSE);
    }
    if (!(xdr_u_short(xdrs, &(jobAttr->port))
	&& xdr_string(xdrs, &sp, MAXHOSTNAMELEN))) {
	return(FALSE);
    }

    if (!xdr_int(xdrs, &jobArrElemId)) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
	jobId32To64(&jobAttr->jobId,jobArrId,jobArrElemId);
    }

    return(TRUE);
}

static int xdrsize_shareAcctInfoEnt(struct shareAcctInfoEnt *node, int *size) {
    *size += ALIGNWORD_(sizeof(struct shareAcctInfoEnt));
    *size += getXdrStrlen(node->name);
    return 1;
}

static bool_t xdr_shareAcctInfoEnt(struct shareAcctInfoEnt *sa, XDR *xdrs,
                                   struct LSFHeader *hdr) {
    
    if (!(xdr_var_string(xdrs, &sa->name)))
        return (FALSE);

    if (!(xdr_int(xdrs, &sa->share) && 
          xdr_float(xdrs, &sa->priority) &&
          xdr_int(xdrs, &sa->numStartJobs) &&
          xdr_int(xdrs, &sa->nChildShareAcct) &&
          xdr_float(xdrs, &sa->cpuTime) &&
          xdr_float(xdrs, &sa->runTime))) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE && sa->nChildShareAcct > 0) {
        sa->childShareAccts = (struct shareAcctInfoEnt *)calloc(sa->nChildShareAcct, sizeof(struct shareAcctInfoEnt));
        if (!sa->childShareAccts) {
            return (FALSE);
        }
    }
    return TRUE;
}

int
xdrsize_QueueInfoReply(struct queueInfoReply * qInfoReply)
{
    int len;
    int i;
    int shareLen;

    len = 0;

    for (i = 0; i < qInfoReply->numQueues; i++) {
        shareLen = 0;
        len += getXdrStrlen(qInfoReply->queues[i].description)
            + getXdrStrlen(qInfoReply->queues[i].windows)
            + getXdrStrlen(qInfoReply->queues[i].userList)
            + getXdrStrlen(qInfoReply->queues[i].hostList)
            + getXdrStrlen(qInfoReply->queues[i].defaultHostSpec)
            + getXdrStrlen(qInfoReply->queues[i].hostSpec)
            + getXdrStrlen(qInfoReply->queues[i].windowsD)
            + getXdrStrlen(qInfoReply->queues[i].admins)
            + getXdrStrlen(qInfoReply->queues[i].preCmd)
            + getXdrStrlen(qInfoReply->queues[i].postCmd)
            + getXdrStrlen(qInfoReply->queues[i].prepostUsername)
            + getXdrStrlen(qInfoReply->queues[i].requeueEValues)
            + getXdrStrlen(qInfoReply->queues[i].resReq)
            + getXdrStrlen(qInfoReply->queues[i].resumeCond)
            + getXdrStrlen(qInfoReply->queues[i].stopCond)
            + getXdrStrlen(qInfoReply->queues[i].jobStarter)
            + getXdrStrlen(qInfoReply->queues[i].suspendActCmd)
            + getXdrStrlen(qInfoReply->queues[i].resumeActCmd)
            + getXdrStrlen(qInfoReply->queues[i].terminateActCmd)
            + getXdrStrlen(qInfoReply->queues[i].chkpntDir)
            + getXdrStrlen(qInfoReply->queues[i].userShares)
            + getXdrStrlen(qInfoReply->queues[i].actionComment);
        if (qInfoReply->queues[i].shareAcctTree) {
            traverseSATree(qInfoReply->queues[i].shareAcctTree, TRAVERSE_FS_SIZE, xdrsize_shareAcctInfoEnt, &shareLen);
            len += shareLen;
        }
    }
    len += ALIGNWORD_(sizeof(struct queueInfoReply)
                      + qInfoReply->numQueues * (sizeof(struct queueInfoEnt)+ MAX_LSB_NAME_LEN + qInfoReply->nIdx*2*sizeof(float))
                      + qInfoReply->numQueues * NET_INTSIZE_);

    return len;
}


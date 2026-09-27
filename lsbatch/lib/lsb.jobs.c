/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.jobs.c 397 2007-11-26 19:04:00Z mblack $
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

#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include "lsb.h"

extern int errno;


extern void copyJUsage(struct jRusage *to, struct jRusage *from);
extern int _lsb_recvtimeout;

static int mbdSock = -1;
static __thread int compactJobOutput = FALSE;
static __thread struct jobOutputReply compactReply;
static __thread int jobInfoRemaining;

static void
finishJobInfoRecord(int *more)
{
    if (jobInfoRemaining > 0)
        jobInfoRemaining--;
    if (more)
        *more = jobInfoRemaining;
}

static struct jobInfoEnt *
readCompactJobOutput(char *buffer, struct LSFHeader *hdr, int *more)
{
    static __thread struct jobInfoEnt jobInfo;
    static char empty[] = "";
    XDR xdrs;

    xdr_lsffree(xdr_jobOutputReply, (char *)&compactReply, hdr);
    memset(&compactReply, 0, sizeof(compactReply));
    xdrmem_create(&xdrs, buffer, XDR_DECODE_SIZE_(hdr->length), XDR_DECODE);
    if (!xdr_jobOutputReply(&xdrs, &compactReply, hdr)) {
        xdr_destroy(&xdrs);
        xdr_lsffree(xdr_jobOutputReply, (char *)&compactReply, hdr);
        memset(&compactReply, 0, sizeof(compactReply));
        lsberrno = LSBE_XDR;
        return NULL;
    }
    xdr_destroy(&xdrs);

    memset(&jobInfo, 0, sizeof(jobInfo));
    jobInfo.jobId = compactReply.jobId;
    jobInfo.user = compactReply.userName ? compactReply.userName : empty;
    jobInfo.status = compactReply.status;
    jobInfo.reasons = compactReply.reasons;
    jobInfo.submit.queue = compactReply.queue ? compactReply.queue : empty;
    jobInfo.fromHost = compactReply.fromHost ? compactReply.fromHost : empty;
    jobInfo.numExHosts = compactReply.numExHosts;
    jobInfo.exHosts = compactReply.exHosts;
    jobInfo.submit.jobName = compactReply.jobName
                              ? compactReply.jobName : empty;
    jobInfo.submitTime = compactReply.submitTime;
    jobInfo.submit.projectName = compactReply.projectName
                                  ? compactReply.projectName : empty;
    jobInfo.cpuTime = compactReply.cpuTime;
    jobInfo.maxMem = compactReply.maxMem;
    jobInfo.avgMem = compactReply.avgMem;
    jobInfo.runRusage.mem = compactReply.mem;
    jobInfo.runRusage.swap = compactReply.swap;
    jobInfo.runRusage.npids = compactReply.npids;
    jobInfo.runRusage.pidInfo = compactReply.pidInfo;
    jobInfo.startTime = compactReply.startTime;
    jobInfo.endTime = compactReply.endTime;
    jobInfo.exitStatus = compactReply.exitStatus;

    jobInfo.submit.command = empty;
    jobInfo.submit.resReq = empty;
    jobInfo.submit.inFile = empty;
    jobInfo.submit.outFile = empty;
    jobInfo.submit.errFile = empty;
    jobInfo.submit.hostSpec = empty;
    jobInfo.submit.chkpntDir = empty;
    jobInfo.submit.dependCond = empty;
    jobInfo.submit.preExecCmd = empty;
    jobInfo.submit.postExecCmd = empty;
    jobInfo.submit.mailUser = empty;
    jobInfo.submit.loginShell = empty;
    jobInfo.cwd = empty;
    jobInfo.subHomeDir = empty;
    jobInfo.execHome = empty;
    jobInfo.execCwd = empty;
    jobInfo.execUsername = empty;
    jobInfo.parentGroup = empty;
    jobInfo.jName = empty;
    jobInfo.chargedSAAP = empty;
    jobInfo.mergedResReq = empty;
    jobInfo.effeResReq = empty;

    finishJobInfoRecord(more);
    return &jobInfo;
}

int
lsb_openjobinfo (LS_LONG_INT jobId, char *jobName, char *userName,
                 char *queueName, char *hostName, int options)
{
    struct jobInfoHead *jobInfoHead;

    jobInfoHead = lsb_openjobinfo_a (jobId, jobName, userName, queueName,
				     hostName, options);
    if (!jobInfoHead)
        return (-1);
    return (jobInfoHead->numJobs);

}

struct jobInfoHead *
lsb_openjobinfo_a (LS_LONG_INT jobId, char *jobName, char *userName,
                 char *queueName, char *hostName, int options)
{
    return lsb_openjobinfo_a_fields(jobId, jobName, userName, queueName,
                                    hostName, options, NULL);
}

struct jobInfoHead *
lsb_openjobinfo_a_fields(LS_LONG_INT jobId, char *jobName, char *userName,
                         char *queueName, char *hostName, int options,
                         const char *outputFields)
{
    static __thread int first = TRUE;
    static __thread struct jobInfoReq jobInfoReq;
    static __thread struct jobInfoHead jobInfoHead;
    mbdReqType mbdReqtype;
    int useOutput, requestVersion, retried = 0;
    XDR xdrs, xdrs2;
    char *request_buf = NULL;
    char *reply_buf, *clusterName = NULL;
    int cc, aa;
    size_t outputFieldsLength;
    size_t requestSize;
    struct LSFHeader hdr;
    char lsfUserName[MAXLINELEN];
    jobInfoRemaining = 0;
    if (first) {
        if (   !(jobInfoReq.jobName  = (char *) malloc(MAX_CMD_DESC_LEN))
            || !(jobInfoReq.queue    = (char *) malloc(MAX_LSB_NAME_LEN))
            || !(jobInfoReq.userName = (char *) malloc(MAX_LSB_NAME_LEN))
            || !(jobInfoReq.host     = (char *) malloc(MAXHOSTNAMELEN))) {
            lsberrno = LSBE_SYS_CALL;
            return(NULL);
        }
        first = FALSE;
    }

    if (queueName == NULL)
        jobInfoReq.queue[0] = '\0';
    else {
        if (strlen (queueName) >= MAX_LSB_NAME_LEN - 1) {
            lsberrno = LSBE_BAD_QUEUE;
            return(NULL);
        }
	TIMEIT(1, strcpy(jobInfoReq.queue, queueName), "strcpy");
    }

    if (hostName == NULL)
        jobInfoReq.host[0] = '\0';
    else {

        if (ls_isclustername(hostName) > 0) {
            jobInfoReq.host[0] = '\0';
            clusterName = hostName;
        } else {
            struct hostent *hp;

	    TIMEIT(0, (hp = Gethostbyname_(hostName)), "getHostOfficialByName_");
	    if (hp != NULL) {
		struct hostInfo *hostinfo;
                char officialNameBuf[MAXHOSTNAMELEN];

                strcpy(officialNameBuf, hp->h_name);
		hostinfo = ls_gethostinfo("-",
                                          NULL,
                                          (char **)&hp->h_name,
                                          1,
                                          LOCAL_ONLY);
		if (hostinfo == NULL) {
		    strcpy(jobInfoReq.host, hostName);
		} else {
	            strcpy(jobInfoReq.host, officialNameBuf);
		}
            } else {
                if (strlen (hostName) >= MAXHOSTNAMELEN - 1) {
                    lsberrno = LSBE_BAD_HOST;
                    return(NULL);
                }
	        strcpy(jobInfoReq.host, hostName);
            }
        }

    }

    if (jobName == NULL)
        jobInfoReq.jobName[0] = '\0';
    else {
        if (strlen (jobName) >= MAX_CMD_DESC_LEN - 1) {
            lsberrno = LSBE_BAD_JOB;
            return(NULL);
        }
	strcpy(jobInfoReq.jobName, jobName);
    }

    if (userName == NULL ) {
        TIMEIT(0, (cc = getLSFUser_(lsfUserName, MAXLINELEN)), "getLSFUser_");
        if (cc  != 0) {
           return (NULL);
        }
	TIMEIT(1, strcpy(jobInfoReq.userName, lsfUserName), "strcpy");
    } else {
        if (strlen (userName) >= MAX_LSB_NAME_LEN - 1) {
            lsberrno = LSBE_BAD_USER;
            return(NULL);
        }
	strcpy(jobInfoReq.userName, userName);
    }
    if ((options & ~(JOBID_ONLY | JOBID_ONLY_ALL | HOST_NAME | NO_PEND_REASONS)) == 0)
	jobInfoReq.options = CUR_JOB;
    else
        jobInfoReq.options = options;

    if (jobId < 0) {
	lsberrno = LSBE_BAD_ARG;
	return(NULL);
    }
    jobInfoReq.jobId = jobId;
    jobInfoReq.outputFields = (char *)(outputFields ? outputFields : "");

    outputFieldsLength = strlen(jobInfoReq.outputFields);
    requestSize = (size_t)MSGSIZE + outputFieldsLength + 8;
    if (requestSize > UINT_MAX
        || (request_buf = malloc(requestSize)) == NULL) {
        lsberrno = LSBE_NO_MEM;
        return(NULL);
    }

    compactJobOutput = outputFields && outputFields[0] != '\0'
                       && !(options & JGRP_ARRAY_INFO);
    useOutput = outputFields && outputFields[0] != '\0';
retryOutput:
    mbdReqtype = useOutput ? BATCH_OUTPUT : BATCH_JOB_INFO;
    requestVersion = useOutput ? _VOLCLAVA_VERSION2_3_
                               : _VOLCLAVA_VERSION2_2_;
    xdrmem_create(&xdrs, request_buf, (u_int)requestSize, XDR_ENCODE);

    initLSFHeader_(&hdr);
    hdr.opCode = mbdReqtype;
    hdr.reserved = useOutput ? (compactJobOutput ? OUTPUT_JOB : OUTPUT_JOB_GROUP) : 0;
    TIMEIT(1, (aa = xdr_encodeMsgVersion(&xdrs,
                           (char *) &jobInfoReq, &hdr,
                           xdr_jobInfoReq, 0, NULL,
                           requestVersion)), "xdr_encodeMsgVersion");
    if (aa == FALSE) {
        lsberrno = LSBE_XDR;
        xdr_destroy(&xdrs);
        free(request_buf);
        return(NULL);
    }



    TIMEIT(0, (cc = callmbd (clusterName, request_buf, XDR_GETPOS(&xdrs),
                    &reply_buf, &hdr, &mbdSock, NULL, NULL)), "callmbd");
    if (cc  == -1) {
        xdr_destroy(&xdrs);
	free(request_buf);
	return (NULL);
    }

    /* Negotiate required fields before reading rows: older typed peers may
     * accept a newer request without supplying its added fields. */
    if (useOutput && !retried &&
        (hdr.opCode == LSBE_PROTOCOL ||
         (hdr.opCode == LSBE_NO_ERROR && hdr.version < _VOLCLAVA_VERSION2_3_))) {
        if (cc) free(reply_buf);
        reply_buf = NULL;
        xdr_destroy(&xdrs);
        useOutput = 0;
        retried = 1;
        lsb_closejobinfo();
        compactJobOutput = FALSE;
        goto retryOutput;
    }
    xdr_destroy(&xdrs);
    free(request_buf);



    lsberrno = hdr.opCode;
    if (lsberrno == LSBE_NO_ERROR) {


	xdrmem_create(&xdrs2, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);
	if (! xdr_jobInfoHead (&xdrs2, &jobInfoHead, &hdr) ||
            jobInfoHead.numJobs < 0) {
	    lsberrno = LSBE_XDR;
            xdr_destroy(&xdrs2);
	    if (cc)
		free(reply_buf);
	    return(NULL);
        }
	xdr_destroy(&xdrs2);
	if (cc)
	    free(reply_buf);
        jobInfoRemaining = jobInfoHead.numJobs;
        return (&jobInfoHead);
    }

    if (cc)
	free(reply_buf);
    return(NULL);

}

struct jobInfoEnt *
lsb_readjobinfo(int *more)
{
    XDR  xdrs;
    int num, i, aa;
    struct LSFHeader hdr;
    char *buffer = NULL;
    static struct jobInfoReply jobInfoReply = {0};
    static struct jobInfoEnt jobInfo;
    static struct submitReq submitReq;
    static int first = TRUE;
    static int npids = 0;
    static struct pidInfo  *pidInfo = NULL;
    static int npgids = 0;
    static int *pgid = NULL;


    TIMEIT(0, (num = readNextPacket(&buffer, _lsb_recvtimeout, &hdr,
				    mbdSock)), "readNextPacket");
    if (num < 0) {
	closeSession(mbdSock);
        lsberrno = LSBE_EOF;
	return NULL;
    }

    if (compactJobOutput) {
        struct jobInfoEnt *compactInfo;

        compactInfo = readCompactJobOutput(buffer, &hdr, more);
        free(buffer);
        return compactInfo;
    }

    if (first) {
	if ( (submitReq.fromHost = malloc(MAXHOSTNAMELEN)) == NULL
	    || (submitReq.jobFile = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.inFile  = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.outFile = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.errFile = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.inFileSpool = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.commandSpool = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.hostSpec = malloc(MAXHOSTNAMELEN)) == NULL
	    || (submitReq.chkpntDir = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.subHomeDir = malloc(MAXFILENAMELEN)) == NULL
	    || (submitReq.submitCwd = malloc(MAXFILENAMELEN)) == NULL
	    || (jobInfoReply.userName  = malloc(MAXLSFNAMELEN)) == NULL
	    || (submitReq.cwd       = malloc(MAXFILENAMELEN)) == NULL) {
	    lsberrno = LSBE_NO_MEM;
	    FREEUP(submitReq.fromHost);
	    FREEUP(submitReq.jobFile);
	    FREEUP(submitReq.inFile);
	    FREEUP(submitReq.outFile);
	    FREEUP(submitReq.errFile);
	    FREEUP(submitReq.inFileSpool);
	    FREEUP(submitReq.commandSpool);
	    FREEUP(submitReq.hostSpec);
	    FREEUP(submitReq.chkpntDir);
	    FREEUP(submitReq.subHomeDir);
	    FREEUP(submitReq.submitCwd);
	    FREEUP(jobInfoReply.userName);
	    FREEUP(submitReq.cwd);

	    free(buffer);
	    return NULL;
	}

	submitReq.xf = NULL;
        submitReq.nxf = 0;
        jobInfoReply.numToHosts = 0;
        submitReq.numAskedHosts = 0;
        first = FALSE;
    }

    jobInfoReply.jobBill = &submitReq;

    if (jobInfoReply.numToHosts > 0) {
	for (i=0; i<jobInfoReply.numToHosts; i++)
	    FREEUP(jobInfoReply.toHosts[i]);
        FREEUP(jobInfoReply.toHosts);
	jobInfoReply.numToHosts = 0;
	jobInfoReply.toHosts = NULL;
    }

    if (submitReq.xf) {
	free(submitReq.xf);
        submitReq.xf = NULL;
    }


    FREEUP( jobInfoReply.execHome );
    FREEUP( jobInfoReply.execCwd );
    FREEUP( jobInfoReply.execUsername );
    FREEUP( jobInfoReply.parentGroup );
    FREEUP( jobInfoReply.jName );
    FREEUP( jobInfoReply.chargedSAAP );
    FREEUP( jobInfoReply.mergedResReq );
    FREEUP( jobInfoReply.effeResReq );

    TIMEIT(1, xdrmem_create(&xdrs, buffer, XDR_DECODE_SIZE_(hdr.length), XDR_DECODE), "xdrmem_create");
    TIMEIT(1, (aa = xdr_jobInfoReply(&xdrs, &jobInfoReply, &hdr)), "xdr_jobInfoReply");
    if (aa == FALSE) {
	lsberrno = LSBE_XDR;
	xdr_destroy(&xdrs);
	free(buffer);
	jobInfoReply.toHosts = NULL;
	jobInfoReply.numToHosts = 0;
	return NULL;
    }

    TIMEIT(1, xdr_destroy(&xdrs), "xdr_destroy");
    free(buffer);
    jobInfo.jobId = jobInfoReply.jobId;
    jobInfo.status = jobInfoReply.status;
    jobInfo.numReasons = jobInfoReply.numReasons;
    jobInfo.reasonTb = jobInfoReply.reasonTb;
    jobInfo.reasons = jobInfoReply.reasons;
    jobInfo.subreasons = jobInfoReply.subreasons;
    jobInfo.numLimitDetail = jobInfoReply.numLimitDetail;
    jobInfo.limitDetailTb = jobInfoReply.limitDetailTb;
    jobInfo.startTime = jobInfoReply.startTime;
    jobInfo.predictedStartTime = jobInfoReply.predictedStartTime;
    jobInfo.endTime  = jobInfoReply.endTime;
    jobInfo.cpuTime  = jobInfoReply.cpuTime;
    jobInfo.idleFactor = jobInfoReply.idleFactor;
    jobInfo.numExHosts = jobInfoReply.numToHosts;
    jobInfo.exHosts = jobInfoReply.toHosts;
    jobInfo.nIdx = jobInfoReply.nIdx;
    jobInfo.loadSched = jobInfoReply.loadSched;
    jobInfo.loadStop = jobInfoReply.loadStop;
    jobInfo.exitStatus = jobInfoReply.exitStatus;
    jobInfo.reserveTime = jobInfoReply.reserveTime;
    jobInfo.jobPid = jobInfoReply.jobPid;
    jobInfo.port = jobInfoReply.port;
    jobInfo.jobPriority = jobInfoReply.jobPriority;

    jobInfo.user = jobInfoReply.userName;

    jobInfo.execUid = jobInfoReply.execUid;
    jobInfo.execHome = jobInfoReply.execHome;
    jobInfo.execCwd = jobInfoReply.execCwd;
    jobInfo.execUsername = jobInfoReply.execUsername;


    jobInfo.jType    = jobInfoReply.jType;
    jobInfo.parentGroup = jobInfoReply.parentGroup;
    jobInfo.jName        = jobInfoReply.jName;
    for (i=0; i<NUM_JGRP_COUNTERS; i++)
        jobInfo.counter[i] = jobInfoReply.counter[i];

    jobInfo.submitTime = jobInfoReply.jobBill->submitTime;
    jobInfo.umask = jobInfoReply.jobBill->umask;
    jobInfo.cwd = jobInfoReply.jobBill->submitCwd;
    jobInfo.subHomeDir = jobInfoReply.jobBill->subHomeDir;
    jobInfo.submit.options = jobInfoReply.jobBill->options;
    jobInfo.submit.options2 = jobInfoReply.jobBill->options2;
    jobInfo.submit.numProcessors = jobInfoReply.jobBill->numProcessors;
    jobInfo.submit.maxNumProcessors = jobInfoReply.jobBill->maxNumProcessors;
    jobInfo.submit.jobName = jobInfoReply.jobBill->jobName;
    jobInfo.submit.command = jobInfoReply.jobBill->command;
    jobInfo.submit.resReq = jobInfoReply.jobBill->resReq;
    jobInfo.submit.queue = jobInfoReply.jobBill->queue;
    jobInfo.fromHost = jobInfoReply.jobBill->fromHost;
    jobInfo.submit.inFile = jobInfoReply.jobBill->inFile;
    jobInfo.submit.outFile = jobInfoReply.jobBill->outFile;
    jobInfo.submit.errFile = jobInfoReply.jobBill->errFile;
    jobInfo.submit.beginTime = jobInfoReply.jobBill->beginTime;
    jobInfo.submit.termTime = jobInfoReply.jobBill->termTime;
    jobInfo.submit.userPriority = jobInfoReply.jobBill->userPriority;


    for (i=0; i<LSF_RLIM_NLIMITS; i++) {
	jobInfo.submit.rLimits[i] = jobInfoReply.jobBill->rLimits[i];
    }
    jobInfo.submit.hostSpec = jobInfoReply.jobBill->hostSpec;
    jobInfo.submit.sigValue = jobInfoReply.jobBill->sigValue;
    jobInfo.submit.chkpntDir = jobInfoReply.jobBill->chkpntDir;
    jobInfo.submit.dependCond = jobInfoReply.jobBill->dependCond;
    jobInfo.submit.preExecCmd = jobInfoReply.jobBill->preExecCmd;
    jobInfo.submit.postExecCmd = jobInfoReply.jobBill->postExecCmd;
    jobInfo.submit.chkpntPeriod = jobInfoReply.jobBill->chkpntPeriod;
    jobInfo.submit.numAskedHosts = jobInfoReply.jobBill->numAskedHosts;
    jobInfo.submit.askedHosts = jobInfoReply.jobBill->askedHosts;
    jobInfo.submit.projectName = jobInfoReply.jobBill->projectName;
    jobInfo.submit.mailUser = jobInfoReply.jobBill->mailUser;
    jobInfo.submit.loginShell = jobInfoReply.jobBill->loginShell;
    jobInfo.submit.nxf = jobInfoReply.jobBill->nxf;
    jobInfo.submit.xf = jobInfoReply.jobBill->xf;
    jobInfo.submit.jobDesc = jobInfoReply.jobBill->jobDesc;
    jobInfo.submit.cwd = jobInfoReply.jobBill->cwd;



    jobInfo.jRusageUpdateTime = jobInfoReply.jRusageUpdateTime;
    jobInfo.runRusage.npids = npids;
    jobInfo.runRusage.pidInfo = pidInfo;

    jobInfo.runRusage.npgids = npgids;
    jobInfo.runRusage.pgid = pgid;

    copyJUsage(&(jobInfo.runRusage), &jobInfoReply.runRusage);


    npids = jobInfo.runRusage.npids;
    pidInfo = jobInfo.runRusage.pidInfo;

    npgids = jobInfo.runRusage.npgids;
    pgid = jobInfo.runRusage.pgid;

    if (jobInfoReply.runRusage.npids > 0) {
        FREEUP(jobInfoReply.runRusage.pidInfo);
        jobInfoReply.runRusage.npids = 0;
    }

    if (jobInfoReply.runRusage.npgids > 0) {
        FREEUP(jobInfoReply.runRusage.pgid);
        jobInfoReply.runRusage.npgids = 0;
    }

    jobInfo.chargedSAAP = jobInfoReply.chargedSAAP;
    jobInfo.mergedResReq = jobInfoReply.mergedResReq;
    jobInfo.effeResReq = jobInfoReply.effeResReq;
    jobInfo.maxMem = jobInfoReply.maxMem;
    jobInfo.avgMem = jobInfoReply.avgMem;

    finishJobInfoRecord(more);

    return &jobInfo;
}


void
lsb_closejobinfo()
{
     struct LSFHeader hdr;

     closeSession(mbdSock);
     mbdSock = -1;
     jobInfoRemaining = 0;
     if (compactJobOutput) {
         memset(&hdr, 0, sizeof(hdr));
         xdr_lsffree(xdr_jobOutputReply, (char *)&compactReply, &hdr);
         memset(&compactReply, 0, sizeof(compactReply));
         compactJobOutput = FALSE;
     }
}

int
lsb_runjob(struct runJobRequest* runJobRequest)
{
    XDR                   xdrs;
    struct LSFHeader      lsfHeader;
    struct lsfAuth        auth;
    mbdReqType            mbdReqType;
    char                  request_buf[MSGSIZE/2];
    char*                 reply_buf;
    int                   retVal;
    int                   cc;


    if (runJobRequest == NULL
	|| runJobRequest->numHosts == 0
	|| runJobRequest->hostname == NULL
	|| runJobRequest->jobId < 0
	|| (   runJobRequest->options != 0
	    && ! (runJobRequest->options &
		  (RUNJOB_OPT_NORMAL | RUNJOB_OPT_NOSTOP))))

    {
	lsberrno = LSBE_BAD_ARG;
	return(-1);
    }


    if (!( runJobRequest->options & (RUNJOB_OPT_NORMAL | RUNJOB_OPT_NOSTOP))) {
	runJobRequest->options |= RUNJOB_OPT_NORMAL;
    }


    if (authTicketTokens_(&auth, NULL) == -1) {
	lsberrno = LSBE_LSBLIB;
	return (-1);
    }


    mbdReqType = BATCH_JOB_FORCE;


    xdrmem_create(&xdrs,
		  request_buf,
		  MSGSIZE/2,
		  XDR_ENCODE);

    initLSFHeader_(&lsfHeader);

    lsfHeader.opCode = mbdReqType;

    if (!xdr_encodeMsg(&xdrs,
		       (char *)runJobRequest,
		       &lsfHeader,
		       xdr_runJobReq,
		       0,
		       &auth)) {
	lsberrno = LSBE_XDR;
	xdr_destroy(&xdrs);
	return(-1);
    }



    if ((cc = callmbd(NULL,
		      request_buf,
		      XDR_GETPOS(&xdrs),
		      &reply_buf,
		      &lsfHeader,
		      NULL,
		      NULL,
		      NULL)) == -1) {
	xdr_destroy(&xdrs);
	return(-1);
    }


    xdr_destroy(&xdrs);


    lsberrno = lsfHeader.opCode;

    if (lsberrno == LSBE_NO_ERROR)
	retVal = 0;
    else
	retVal = -1;

    return(retVal);

}

char *
lsb_jobid2str (LS_LONG_INT jobId)
{
    static  char string[32];


    if (LSB_ARRAY_IDX(jobId) == 0) {
	sprintf(string, "%d",  LSB_ARRAY_JOBID(jobId));
    }
    else {
	sprintf(string, "%d[%d]",  LSB_ARRAY_JOBID(jobId),
	    LSB_ARRAY_IDX(jobId));
    }

    return(string);

}

char *
lsb_jobidinstr(LS_LONG_INT jobId)
{
    static  char string[32];

    sprintf(string, LS_LONG_FORMAT, jobId);
    return(string);

}

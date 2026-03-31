/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.modify.c 397 2007-11-26 19:04:00Z mblack $
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
#include <sys/time.h>
#include <sys/resource.h>
#include <string.h>

#include "lsb.h"

#include "lsb.spool.h"

#define  NL_SETN   13   

static LS_LONG_INT sendModifyReq (struct modifyReq *, struct submitReply *,
struct lsfAuth *);
static int esubValModify(struct submit *);
extern void makeCleanToRunEsub();
extern void modifyJobInformation(struct submit *);

LS_LONG_INT
lsb_modify(struct submit *jobSubReq, struct submitReply *submitRep, LS_LONG_INT job)
{
    struct modifyReq modifyReq;
    int i;
    struct lsfAuth auth;
    char homeDir[MAXFILENAMELEN], cwd[MAXFILENAMELEN];
    char resReq[MAXLINELEN], cmd[MAXLINELEN];
    struct jobInfoEnt *jInfo;
    LSB_SUB_SPOOL_FILE_T subSpoolFiles;
    LS_LONG_INT jobId = -1;
    int loop;

    subSpoolFiles.inFileSpool[0] = 0;
    subSpoolFiles.commandSpool[0] = 0;

    lsberrno = LSBE_BAD_ARG;

    if (job <= 0) {
        lsberrno = LSBE_BAD_JOB;
        return (-1);
    }

    
    subNewLine_(jobSubReq->resReq);
    subNewLine_(jobSubReq->dependCond);
    subNewLine_(jobSubReq->preExecCmd);
    subNewLine_(jobSubReq->postExecCmd);
    subNewLine_(jobSubReq->mailUser);
    subNewLine_(jobSubReq->jobName);
    subNewLine_(jobSubReq->queue);
    subNewLine_(jobSubReq->inFile);
    subNewLine_(jobSubReq->outFile);
    subNewLine_(jobSubReq->errFile);
    subNewLine_(jobSubReq->chkpntDir);
    subNewLine_(jobSubReq->projectName);
    for(loop = 0; loop < jobSubReq->numAskedHosts; loop++) {
        subNewLine_(jobSubReq->askedHosts[loop]);
    }


    modifyReq.jobId = job;
    modifyReq.jobIdStr = jobSubReq->command;
    modifyReq.delOptions = jobSubReq->delOptions;
    modifyReq.delOptions2 = jobSubReq->delOptions2;
    if (jobSubReq->numProcessors == 0)
        jobSubReq->numProcessors = DEFAULT_NUMPRO;
    if (jobSubReq->maxNumProcessors == 0)
        jobSubReq->maxNumProcessors = DEFAULT_NUMPRO;
    cwd[0]='\0';
    modifyReq.submitReq.subHomeDir = homeDir;
    modifyReq.submitReq.cwd = cwd;
    modifyReq.submitReq.resReq = resReq;
    modifyReq.submitReq.command = cmd;

    makeCleanToRunEsub();
    
    if (esubValModify(jobSubReq) < 0) {
        goto cleanup;
    }

    modifyJobInformation(jobSubReq);
    
    if (getCommonParams (jobSubReq, &modifyReq.submitReq, submitRep) < 0) {
        goto cleanup;                    
    }

    if (authTicketTokens_(&auth, NULL) == -1) {
        goto cleanup;
    }
    if (getOtherParams (jobSubReq, &modifyReq.submitReq,
                                      submitRep, &auth, &subSpoolFiles) < 0) {
        goto cleanup;
    }

    
    if(lsb_openjobinfo (job, NULL, NULL, NULL, NULL, 0) >= 0
       && (jInfo = lsb_readjobinfo (NULL)) != NULL){
	
         if ((jobSubReq->termTime != 0 && jobSubReq->termTime != DELETE_NUMBER)
             && jobSubReq->beginTime == 0 
             && jInfo->submit.beginTime > jobSubReq->termTime) {
	       lsberrno = LSBE_START_TIME;
	       goto cleanup;
         }
         if (jobSubReq->beginTime!= 0
             && jobSubReq->termTime == 0 
             && jInfo->submit.termTime > 0 
	     && jInfo->submit.termTime < jobSubReq->beginTime) {
	       lsberrno = LSBE_START_TIME;
	       goto cleanup;
         }
    }
    lsb_closejobinfo();

    for (i =0; i < LSF_RLIM_NLIMITS; i++) {  
        if (jobSubReq->rLimits[i] == DELETE_NUMBER)
            modifyReq.submitReq.rLimits[i] = DELETE_NUMBER;
    }

    jobId = sendModifyReq(&modifyReq, submitRep, &auth);

cleanup:

    if (jobId < 0) {
        
        const char* spoolHost;
        int err;

        
        if (subSpoolFiles.inFileSpool[0]) {
            spoolHost = getSpoolHostBySpoolFile(subSpoolFiles.inFileSpool);
            err = chUserRemoveSpoolFile(spoolHost, subSpoolFiles.inFileSpool);
            if (err) {
                fprintf(stderr,
                        (_i18n_msg_get(ls_catd,NL_SETN, 850, "Modification failed, and the spooled file <%s> can not be removed on host <%s>, please manually remove it")), /* catgets  850 */
                        subSpoolFiles.inFileSpool, spoolHost);
            }
        }

        
        if (subSpoolFiles.commandSpool[0]) {
            spoolHost = getSpoolHostBySpoolFile(subSpoolFiles.commandSpool);
            err = chUserRemoveSpoolFile(spoolHost, subSpoolFiles.commandSpool);
            if (err) {
                fprintf(stderr,
                        (_i18n_msg_get(ls_catd,NL_SETN, 850, "Modification failed, and the spooled file <%s> can not be removed on host <%s>, please manually remove it")), /* catgets  850 */
                        subSpoolFiles.commandSpool, spoolHost);
            }
        }
    }

    return(jobId);

} 

static LS_LONG_INT
sendModifyReq (struct modifyReq *modifyReq, struct submitReply *submitReply, struct lsfAuth *auth)
{
    mbdReqType mbdReqtype;
    XDR xdrs;
    char request_buf[2*MSGSIZE];
    char *reply_buf;
    int cc;
    struct LSFHeader hdr;
    struct submitMbdReply *reply;
    LS_LONG_INT jobId;

    
    mbdReqtype = BATCH_JOB_MODIFY;
    xdrmem_create(&xdrs, request_buf, 2*MSGSIZE, XDR_ENCODE);
    hdr.opCode = mbdReqtype;
    if (!xdr_encodeMsg(&xdrs, (char *) modifyReq, &hdr, xdr_modifyReq, 0,
                       auth)) {
        xdr_destroy(&xdrs);
        lsberrno = LSBE_XDR;
        return(-1);
    }
    if ( (cc = callmbd(NULL, request_buf, XDR_GETPOS(&xdrs), &reply_buf,
                       &hdr, NULL, NULL, NULL)) < 0) {
        xdr_destroy(&xdrs);
        return(-1);
    }
    xdr_destroy(&xdrs);

    lsberrno = hdr.opCode;
    if (cc == 0) { 
        submitReply->badJobId = 0;
        submitReply->badReqIndx = 0;
        submitReply->queue = "";
        submitReply->badJobName = "";
        submitReply->pendLimitReason = "";
        submitReply->subTryInterval = DEF_SUB_TRY_INTERVAL;
        return (-1);
    }

    if ((reply = (struct submitMbdReply *)malloc(sizeof(struct submitMbdReply)))
        == NULL) {
        lsberrno = LSBE_NO_MEM;
        free(reply);
        return(-1);
    }

    
    xdrmem_create(&xdrs, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);

    if (!xdr_submitMbdReply(&xdrs, reply, &hdr)) {
        lsberrno = LSBE_XDR;
        free(reply_buf);
        free(reply);
        xdr_destroy(&xdrs);
        return (-1);
    }

    free(reply_buf);

    xdr_destroy(&xdrs);

    

    submitReply->badJobId = reply->jobId;
    submitReply->badReqIndx = reply->badReqIndx;
    submitReply->queue = reply->queue;
    submitReply->badJobName = reply->badJobName;
    submitReply->subTryInterval = reply->subTryInterval;
    submitReply->pendLimitReason = reply->pendLimitReason;


    if (lsberrno == LSBE_NO_ERROR) {
        if (reply->jobId == 0)
            lsberrno = LSBE_PROTOCOL;
        jobId = reply->jobId;
        free(reply);
        return (jobId);
    }

    free(reply);
    return(-1);
} 

 
static int
esubValModify(struct submit *jobSubReq)
{
    struct lenData ed;

    ed.len = 0;
    ed.data = NULL;

    if (runBatchEsub(&ed, jobSubReq, FALSE) < 0)
	return (-1);

    if (ed.len > 0) {
	FREEUP(ed.data);
    }

    return (0);
    
} 

int
lsb_setjobattr(int jobId, struct jobAttrInfoEnt *jobAttr)
{
    XDR xdrs;
    mbdReqType mbdReqtype;
    char request_buf[MSGSIZE];
    struct LSFHeader hdr;
    struct lsfAuth auth;
    char *reply_buf;
    
    if (jobId <= 0) {
        lsberrno = LSBE_BAD_JOBID;
        return (-1);
    }

    if (authTicketTokens_(&auth, NULL) == -1) { 
        return (-1);
    }
    jobAttr->jobId = jobId;
    mbdReqtype = BATCH_SET_JOB_ATTR;
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);
    hdr.opCode = mbdReqtype;
    if (!xdr_encodeMsg(&xdrs, (char *)jobAttr, &hdr, xdr_jobAttrReq, 0, &auth)){
	xdr_destroy(&xdrs);
	lsberrno = LSBE_XDR;
	return (-1);
    }
    if (callmbd(NULL, request_buf, XDR_GETPOS(&xdrs), &reply_buf,
	&hdr, NULL, NULL, NULL) < 0) {
	xdr_destroy(&xdrs);
	return (-1);
    }
    xdr_destroy(&xdrs);
    free(reply_buf);
    lsberrno=hdr.opCode;
    if (lsberrno == LSBE_NO_ERROR)
	return (0);
    else
        return (-1);
} 


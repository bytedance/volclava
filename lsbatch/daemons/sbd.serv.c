/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: sbd.serv.c 397 2007-11-26 19:04:00Z mblack $
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
#include "sbd.h"

#define NL_SETN		11	

#include <malloc.h>


extern int urgentJob;
static int                replyHdrWithRC(int rc, int s, int jobId);
static void do_syslogMsg(int s, char *buf);
static void do_rusageMsg(int, struct jobCard *, struct lsbMsg *, char *);
static void do_regOpMsg(int, struct jobCard *, struct lsbMsg *, char *);
static void do_chkpntEndMsg(int, struct jobCard *, struct lsbMsg *, char *);
static void do_getPid(int, struct jobCard *, struct lsbMsg *, char *);
extern int jRusageUpdatePeriod;
extern int  rusageUpdateRate;
extern int  rusageUpdatePercent;
extern int sbdlog_newstatus (struct jobCard *jp);
extern int lsbJobCpuLimit;
extern int lsbJobMemLimit;

/*
 * Handle a badmin showconf sbd request
 * Build the SBD-visible showconf parameter list and write it back on the
 * already connected SBD command channel.
 * @param[in] chfd: Connected client channel
 * @param[in] reqHdr: Decoded request header; refCode is echoed in the reply
 */
void
do_showConfReq(int chfd, struct LSFHeader *reqHdr)
{
    static char fname[] = "do_showConfReq";
    struct showConfReply reply;
    struct LSFHeader replyHdr;
    char *reply_buf;
    int bufSize;
    XDR xdrs2;
    int rc = LSBE_NO_ERROR;

    memset(&reply, 0, sizeof(reply));
    if (makeShowConfReply(SHOWCONF_SBD, &reply) < 0)
        rc = LSBE_NO_MEM;

    /* Error replies carry only an LSF header; success replies carry entries. */
    bufSize = (rc == LSBE_NO_ERROR) ? xdrShowConfReplySize(&reply)
                                    : LSF_HEADER_LEN;
    reply_buf = malloc(bufSize);
    if (reply_buf == NULL) {
        freeShowConfReply(&reply, TRUE);
        return;
    }

    initLSFHeader_(&replyHdr);
    replyHdr.opCode = rc;
    replyHdr.refCode = reqHdr->refCode;

    /* Keep the payload absent when the header already reports failure. */
    xdrmem_create(&xdrs2, reply_buf, bufSize, XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs2,
                       (rc == LSBE_NO_ERROR) ? (char *)&reply : NULL,
                       &replyHdr,
                       (rc == LSBE_NO_ERROR) ? xdr_showConfReply : NULL,
                       0,
                       NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs2);
        freeShowConfReply(&reply, TRUE);
        FREEUP(reply_buf);
        return;
    }

    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0)
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "chanWrite_",
                  XDR_GETPOS(&xdrs2));

    xdr_destroy(&xdrs2);
    freeShowConfReply(&reply, TRUE);
    FREEUP(reply_buf);
}

void 
do_newjob(XDR *xdrs, int chfd, struct LSFHeader *reqHdr)
{
    static char        fname[] = "do_newjob()";
    char               reply_buf[MSGSIZE];
    XDR                xdrs2;
    struct jobSpecs    jobSpecs;
    struct jobReply    jobReply;
    struct jobCard     *jp;
    sbdReplyType       reply;
    struct LSFHeader   replyHdr;
    char               *replyStruct;
    struct lsfAuth     *auth = NULL;

    memset(&jobReply, 0, sizeof(struct jobReply));
    
    if (!xdr_jobSpecs(xdrs, &jobSpecs, reqHdr)) {
	reply = ERR_BAD_REQ;
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobSpecs");
	goto sendReply;
    }
    
    
    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
        if (jp->jobSpecs.jobId == jobSpecs.jobId) {
	    
	    jobReply.jobId = jp->jobSpecs.jobId;
	    jobReply.jobPid = jp->jobSpecs.jobPid;
	    jobReply.jobPGid = jp->jobSpecs.jobPGid;
	    jobReply.jStatus = jp->jobSpecs.jStatus;
	    reply = ERR_NO_ERROR;
	    goto sendReply;
	}
    }

    jp = (struct jobCard *) calloc(1, sizeof(struct jobCard));
    if (jp == NULL) {		
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
                  lsb_jobid2str(jobSpecs.jobId), "calloc");
	reply = ERR_MEM;
	goto sendReply;
    }
    memcpy((char *) &jp->jobSpecs, (char *) &jobSpecs,
	   sizeof(struct jobSpecs));

    jp->jobSpecs.jStatus &= ~JOB_STAT_MIG;
    jp->jobSpecs.startTime = now;
    jp->jobSpecs.reasons = 0;
    jp->jobSpecs.subreasons = 0;
    

    if (jp->jobSpecs.jAttrib & Q_ATTRIB_EXCLUSIVE) {
	if (lockHosts (jp) < 0) {
	    ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                      lsb_jobid2str(jp->jobSpecs.jobId), "lockHosts");
            unlockHosts (jp, jp->jobSpecs.numToHosts);
	    reply = ERR_LOCK_FAIL;
	    freeWeek(jp->week);
	    FREEUP(jp);
	    goto sendReply;
        }
    }
    jp->runTime = 0;
    if (initJobCard(jp, &jobSpecs, (int *)&reply) < 0) {
	
	if (jp->jobSpecs.jAttrib & Q_ATTRIB_EXCLUSIVE) {
	    unlockHosts (jp, jp->jobSpecs.numToHosts);
	}
	FREEUP(jp);
	goto sendReply;
    }

    jp->execJobFlag = 0;
    
    if( jp->runTime < 0 ) {
        jp->runTime = 0;               
    }
    jp->execGid = 0;
    jp->execUsername[0] = '\0';
    jp->jobSpecs.execUid = -1;
    jp->jobSpecs.execUsername[0] = '\0';

    if (jp->jobSpecs.jobSpoolDir[0] != '\0') {
        char *tmp;
 
        if ((tmp = getUnixSpoolDir (jp->jobSpecs.jobSpoolDir)) == NULL) {
            
            jp->jobSpecs.jobSpoolDir[0] = '\0';
        }
    }

    if ((logclass & LC_TRACE) && jp->jobSpecs.jobSpoolDir[0] != 0) {
        ls_syslog(LOG_DEBUG,
                  "%s: the SpoolDir for  job <%s>  is %s \n",
                  fname, lsb_jobid2str(jp->jobSpecs.jobId),
                  jp->jobSpecs.jobSpoolDir);
    }
    if (jp->jobSpecs.options & SUB_PRE_EXEC) 
	SBD_SET_STATE(jp, (JOB_STAT_RUN | JOB_STAT_PRE_EXEC))
        else
            SBD_SET_STATE(jp, JOB_STAT_RUN);

    reply = job_exec(jp, chfd);
    
    if (reply != ERR_NO_ERROR) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(jp->jobSpecs.jobId), "job_exec");
	if (jp->jobSpecs.jAttrib & Q_ATTRIB_EXCLUSIVE) {
            unlockHosts (jp, jp->jobSpecs.numToHosts);
	}
	deallocJobCard(jp);
    } else {
	jobReply.jobId = jp->jobSpecs.jobId;
	jobReply.jobPid = jp->jobSpecs.jobPid;
	jobReply.jobPGid = jp->jobSpecs.jobPGid;
	jobReply.jStatus = jp->jobSpecs.jStatus;
    }


sendReply:
    xdr_lsffree(xdr_jobSpecs, (char *)&jobSpecs, reqHdr);
#ifdef INTER_DAEMON_AUTH
    if (daemonParams[LSF_AUTH_DAEMONS].paramValue) {
	if (getSbdAuth(&auth_data))
	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "getSbdAuth");
	else
	    auth = &auth_data;
    }
#endif
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE, XDR_ENCODE);
    initLSFHeader_(&replyHdr);
    replyHdr.opCode = reply;
    replyStruct = (reply == ERR_NO_ERROR) ? (char *) &jobReply : (char *) NULL;
    if (!xdr_encodeMsg(&xdrs2, replyStruct, &replyHdr, xdr_jobReply, 0, auth)) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobReply");
	lsb_merr(_i18n_msg_get(ls_catd , NL_SETN, 5804,
			       "Fatal error: xdr_jobReply() failed; sbatchd relifing")); /* catgets 5804 */
	relife(fname, "xdr_jobReply failed");
    }
	
    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5805,
					 "%s: Sending jobReply (len=%d) to master failed: %m"), /* catgets 5805 */
		  fname, XDR_GETPOS(&xdrs2));
    }
    
    xdr_destroy(&xdrs2);

    
    if (reply == ERR_NO_ERROR && !daemonParams[LSB_BSUBI_OLD].paramValue &&
	PURE_INTERACTIVE(&jp->jobSpecs)) {
  	if (status_job (BATCH_STATUS_JOB, jp, jp->jobSpecs.jStatus,
		        ERR_NO_ERROR) < 0) {
            jp->notReported++;           
	}
    }    

}

void
do_switchjob(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char        fname[] = "do_switchjob()";
    char               reply_buf[MSGSIZE];
    XDR                xdrs2;
    struct jobSpecs    jobSpecs;
    struct jobReply    jobReply;
    int                i;
    sbdReplyType       reply;
    char               *cp;
    char               *word;
    char               found = FALSE;
    struct LSFHeader   replyHdr;
    char               *replyStruct;
    struct jobCard     *jp;
    struct lsfAuth     *auth = NULL;
    
    memset(&jobReply, 0, sizeof(struct jobReply));
    
    if (!xdr_jobSpecs(xdrs, &jobSpecs, reqHdr)) {
        reply = ERR_BAD_REQ;
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobSpecs");
        goto sendReply;
    }
    for (jp = jobQueHead->back; jp != jobQueHead; jp = jp->back) {
        if (jp->jobSpecs.jobId == jobSpecs.jobId) {
            found = TRUE;
            break;
        }
    }
    if (!found) {
        reply = ERR_NO_JOB;
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5807,
                                         "%s: mbatchd trying to switch a non-existent job <%s>"), fname, lsb_jobid2str(jobSpecs.jobId)); /* catgets 5807 */
        goto sendReply;
    }
    if (jp->jobSpecs.jStatus & (JOB_STAT_DONE | JOB_STAT_EXIT)) {
        reply = ERR_JOB_FINISH;	
        goto sendReply;
    }
    

    cp = jobSpecs.windows;
    freeWeek(jp->week);		
    while ((word = getNextWord_(&cp)) != NULL) {
        if (addWindow(word, jp->week, "switchJob jobSpecs") < 0) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M, fname, 
                      lsb_jobid2str(jp->jobSpecs.jobId), "addWindow", word);
            freeWeek(jp->week);
            reply = ERR_BAD_REQ;
            goto sendReply;
        }
    }
    jp->windEdge = now;		

    
    if ((jp->jobSpecs.jAttrib & Q_ATTRIB_EXCLUSIVE)
	&& !(jobSpecs.jAttrib & Q_ATTRIB_EXCLUSIVE))
	for (i = 0; i < jp->jobSpecs.numToHosts; i++)
	    if (unlockHost_(jp->jobSpecs.toHosts[i]) < 0
		&& lserrno != LSE_LIM_NLOCKED)
		ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_MM, fname, 
                          lsb_jobid2str(jp->jobSpecs.jobId), "unlockHost_", jp->jobSpecs.toHosts[i]);

    

    strcpy(jp->jobSpecs.queue, jobSpecs.queue);
    strcpy(jp->jobSpecs.windows, jobSpecs.windows);
    jp->jobSpecs.priority = jobSpecs.priority;
    jp->jobSpecs.nice = jobSpecs.nice;
    jp->jobSpecs.jAttrib = jobSpecs.jAttrib;

    freeThresholds (&jp->jobSpecs.thresholds);
    saveThresholds (&jp->jobSpecs, &jobSpecs.thresholds);

    
    memcpy((char *) &jp->jobSpecs.lsfLimits[LSF_RLIMIT_RUN],
	   (char *) &jobSpecs.lsfLimits[LSF_RLIMIT_RUN], 
	   sizeof(struct lsfLimit));   
    

    strcpy (jp->jobSpecs.requeueEValues, jobSpecs.requeueEValues);
    strcpy (jp->jobSpecs.resumeCond, jobSpecs.resumeCond);
    strcpy (jp->jobSpecs.stopCond, jobSpecs.stopCond);

    lsbFreeResVal (&jp->resumeCondVal);
    if (jobSpecs.resumeCond && jobSpecs.resumeCond[0] != '\0') {
        if ((jp->resumeCondVal = checkThresholdCond (jobSpecs.resumeCond, unitForLimits))
            == NULL) 
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S, fname, 
		      lsb_jobid2str(jp->jobSpecs.jobId),
		      "checkThresholdCond", jobSpecs.resumeCond);
    } 

    lsbFreeResVal (&jp->stopCondVal);
    if (jobSpecs.stopCond && jobSpecs.stopCond[0] != '\0') {
        if ((jp->stopCondVal = checkThresholdCond (jobSpecs.stopCond, unitForLimits))
            == NULL) 
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S, fname, 
		      lsb_jobid2str(jp->jobSpecs.jobId),
		      "checkThresholdCond", jobSpecs.stopCond);
    } 
                       
    if (jobSpecs.options & SUB_LOGIN_SHELL) {
	FREEUP (jp->jobSpecs.loginShell);
	jp->jobSpecs.loginShell = safeSave (jobSpecs.loginShell);
    }

    strcpy (jp->jobSpecs.suspendActCmd, jobSpecs.suspendActCmd);
    strcpy (jp->jobSpecs.resumeActCmd, jobSpecs.resumeActCmd);
    strcpy (jp->jobSpecs.terminateActCmd, jobSpecs.terminateActCmd);

    setRunLimit (jp, FALSE);
    offList ((struct listEntry *)jp);
    inJobLink (jp);

    if (reniceJob(jp) < 0)
	ls_syslog(LOG_DEBUG, "%s: renice job <%s> failed",
		  fname, lsb_jobid2str(jp->jobSpecs.jobId));
    
    reply = ERR_NO_ERROR;
    jobReply.jobId = jp->jobSpecs.jobId;
    jobReply.jobPid = jp->jobSpecs.jobPid;
    jobReply.jobPGid = jp->jobSpecs.jobPGid;
    jobReply.jStatus = jp->jobSpecs.jStatus;
    
sendReply:
    xdr_lsffree(xdr_jobSpecs, (char *)&jobSpecs, reqHdr);
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE, XDR_ENCODE);
    initLSFHeader_(&replyHdr);
    replyHdr.opCode = reply;
    if (reply == ERR_NO_ERROR)
	replyStruct = (char *) &jobReply;
    else {
	replyStruct = (char *) 0;
    }

#ifdef INTER_DAEMON_AUTH
    if (daemonParams[LSF_AUTH_DAEMONS].paramValue) {
	if (getSbdAuth(&auth_data))
	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "getSbdAuth");
	else
	    auth = &auth_data;
    }
#endif

    if (!xdr_encodeMsg(&xdrs2, replyStruct, &replyHdr, xdr_jobReply, 0, auth)) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
		  lsb_jobid2str(jp->jobSpecs.jobId), 
		  "xdr_jobReply");
	relife(fname, "xdr_jobReply");
    }
    
    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
		  lsb_jobid2str(jp->jobSpecs.jobId), "chanWrite_");
    }

    xdr_destroy(&xdrs2);

    return;

}				

void
do_modifyjob(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char        fname[] = "do_switchjob()";
    char               reply_buf[MSGSIZE];
    XDR                xdrs2;
    struct jobSpecs    jobSpecs;
    struct jobReply    jobReply;
    sbdReplyType       reply;
    char               found = FALSE;
    struct LSFHeader   replyHdr;
    char               *replyStruct;
    struct jobCard     *jp;
    struct lsfAuth     *auth = NULL;
    
    memset(&jobReply, 0, sizeof(struct jobReply));
    
    if (!xdr_jobSpecs(xdrs, &jobSpecs, reqHdr)) {
	reply = ERR_BAD_REQ;
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobSpecs");
	goto sendReply;
    }
    for (jp = jobQueHead->back; jp != jobQueHead; jp = jp->back)
	if (jp->jobSpecs.jobId == jobSpecs.jobId) {
	    found = TRUE;
	    break;
	}
    if (!found) {
	reply = ERR_NO_JOB;
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5808,
                                         "%s: mbatchd trying to modify a non-existent job <%s>"), fname, lsb_jobid2str(jobSpecs.jobId)); /* catgets 5808 */
	goto sendReply;
    }
    if (jp->jobSpecs.jStatus & (JOB_STAT_DONE | JOB_STAT_EXIT)) {
	reply = ERR_JOB_FINISH;	
	goto sendReply;
    }
    if ((lsbJobCpuLimit != 1) && 
	((jp->jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_maxl
	  != jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_maxl) ||
	 (jp->jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_maxh
	  != jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_maxh) ||
	 (jp->jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_curl
	  != jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_curl) ||
	 (jp->jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_curh
	  != jobSpecs.lsfLimits[LSF_RLIMIT_CPU].rlim_curh) 
	    )) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 5809, "%s, LSB_JOB_CPULIMIT is not set for the host, job <%s>, CPU limit not modified"), fname, lsb_jobid2str(jobSpecs.jobId));
    } else {
	memcpy((char *) &jp->jobSpecs.lsfLimits[LSF_RLIMIT_CPU],
	       (char *) &jobSpecs.lsfLimits[LSF_RLIMIT_CPU], 
	       sizeof(struct lsfLimit));   
    }
    if ((lsbJobMemLimit != 1) &&
	((jp->jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_maxl
	  != jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_maxl) ||
	 (jp->jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_maxh
	  != jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_maxh) ||
	 (jp->jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_curl
	  != jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_curl) ||
	 (jp->jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_curh
	  != jobSpecs.lsfLimits[LSF_RLIMIT_RSS].rlim_curh) 
	    )) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd, NL_SETN, 5810, "%s, LSB_JOB_MEMLIMIT is not set for the host, job <%s>, memory limit not modified"), fname, lsb_jobid2str(jobSpecs.jobId));	
    } else {
	memcpy((char *) &jp->jobSpecs.lsfLimits[LSF_RLIMIT_RSS],
	       (char *) &jobSpecs.lsfLimits[LSF_RLIMIT_RSS], 
	       sizeof(struct lsfLimit));   
    }

    memcpy((char *) &jp->jobSpecs.lsfLimits[LSF_RLIMIT_RUN],
	   (char *) &jobSpecs.lsfLimits[LSF_RLIMIT_RUN], 
	   sizeof(struct lsfLimit));   
    setRunLimit(jp, FALSE);
    if (strcmp(jp->jobSpecs.outFile, jobSpecs.outFile) || 
	!(strcmp(jobSpecs.outFile, "/dev/null")))
    {
	strcpy(jp->jobSpecs.outFile, jobSpecs.outFile);
	if (strcmp(jobSpecs.outFile, "/dev/null") ||
	    (jobSpecs.options & SUB_OUT_FILE)) {
	    jp->jobSpecs.options |= SUB_OUT_FILE;
	}
	else {
	    jp->jobSpecs.options &= ~SUB_OUT_FILE;
	}
    }
    if (strcmp(jp->jobSpecs.errFile, jobSpecs.errFile))
    {
	strcpy(jp->jobSpecs.errFile, jobSpecs.errFile);
	if (!strcmp(jp->jobSpecs.errFile, "/dev/null")
	    && !(jobSpecs.options & SUB_ERR_FILE)) {
            jp->jobSpecs.options &= ~SUB_ERR_FILE;
	}
    }

    if (jobSpecs.options & SUB_RERUNNABLE) { 
	jp->jobSpecs.options |= SUB_RERUNNABLE;
    } else {
	jp->jobSpecs.options &= ~SUB_RERUNNABLE;
    }
    
sendReply:
    xdr_lsffree(xdr_jobSpecs, (char *)&jobSpecs, reqHdr);
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE, XDR_ENCODE);
    initLSFHeader_(&replyHdr);
    replyHdr.opCode = reply;
    if (reply == ERR_NO_ERROR)
	replyStruct = (char *) &jobReply;
    else {
	replyStruct = (char *) 0;
    }

    if (!xdr_encodeMsg(&xdrs2, replyStruct, &replyHdr, xdr_jobReply, 0, auth)) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
		  lsb_jobid2str(jp->jobSpecs.jobId), 
		  "xdr_jobReply");
	relife(fname, "xdr_jobReply");
    }
    
    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
		  lsb_jobid2str(jp->jobSpecs.jobId), "chanWrite_");
    }
    
    xdr_destroy(&xdrs2);

    return;

}
void
do_probe(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char         fname[] = "do_probe()";
    char                reply_buf[MSGSIZE];
    XDR                 xdrs2;
    struct LSFHeader    replyHdr;
    struct sbdPackage   sbdPackage;
    struct jobSpecs     *jobSpecs;
    int                 i;
    struct lsfAuth      *auth = NULL;

    if (reqHdr->length == 0)
        return;

    initLSFHeader_(&replyHdr);
    replyHdr.opCode = ERR_NO_ERROR;
    jobSpecs = NULL;

    if (!xdr_sbdPackage(xdrs, &sbdPackage, reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_sbdPackage");
        relife(fname, "xdr_sbdPackage");
    } else {
        if (sbdPackage.numJobs) {
            jobSpecs = my_calloc(sbdPackage.numJobs,
                                 sizeof(struct jobSpecs), fname);
            for (i = 0; i < sbdPackage.numJobs; i++) {
                if (!xdr_arrayElement(xdrs, (char *) &(jobSpecs[i]),
                                      reqHdr, xdr_jobSpecs)) {
                    replyHdr.opCode = ERR_BAD_REQ;
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5815, 
                                                     "%s: %s(%d) failed for %d jobs"), /* catgets 5815 */
                              fname, "xdr_arrayElement", i, sbdPackage.numJobs);
                    break;
                }
                refreshJob(&(jobSpecs[i]));
                xdr_lsffree(xdr_jobSpecs, (char *)&jobSpecs[i], reqHdr);
            }
        }
    }
    if (replyHdr.opCode == ERR_NO_ERROR)
	if (!xdr_sbdPackage1(xdrs, &sbdPackage, reqHdr)) {
	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_sbdPackage1");
	    relife(fname, "xdr_sbdPackage1");
	}
    if (replyHdr.opCode == ERR_NO_ERROR) {
        if (myStatus & NO_LIM) {
	    replyHdr.opCode = ERR_NO_LIM;
        }
    }
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE, XDR_ENCODE);

    if (!xdr_encodeMsg(&xdrs2, NULL, &replyHdr, NULL, 0, auth)) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
	relife(fname, "xdr_encodeMsg");
    }

    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "chanWrite_");
    }
    
    xdr_destroy(&xdrs2);

    if (jobSpecs != NULL)
	free(jobSpecs);

    
    getManagerId(&sbdPackage);

    mbdPid = sbdPackage.mbdPid;
    sbdSleepTime = sbdPackage.sbdSleepTime;
    retryIntvl = sbdPackage.retryIntvl;
    preemPeriod = sbdPackage.preemPeriod;
    pgSuspIdleT = sbdPackage.pgSuspIdleT;
    maxJobs = sbdPackage.maxJobs;
    uJobLimit = sbdPackage.uJobLimit;
    rusageUpdateRate = sbdPackage.rusageUpdateRate;
    rusageUpdatePercent = sbdPackage.rusageUpdatePercent;
    jobTerminateInterval = sbdPackage.jobTerminateInterval;
    jobCwdTtl = sbdPackage.jobCwdTtl;


    for (i = 0; i < sbdPackage.nAdmins; i++)
	FREEUP(sbdPackage.admins[i]);
    FREEUP(sbdPackage.admins);

    return;
}				

void
do_sigjob(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char        fname[] = "do_sigjob()";
    char               reply_buf[MSGSIZE];
    XDR                xdrs2;
    struct jobSig      jobSig;
    sbdReplyType       reply;
    struct jobReply    jobReply;
    struct LSFHeader   replyHdr;
    char               *replyStruct;
    struct jobCard     *jp = NULL;
    char               found = FALSE;
    int                cc;
    int                sigValue;
    int                savedActReasons;
    int                savedActSubReasons;
    struct lsfAuth     *auth = NULL;
    
    memset(&jobReply, 0, sizeof(struct jobReply));
    if (!xdr_jobSig(xdrs, &jobSig, reqHdr)) {
	reply = ERR_BAD_REQ;
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobSig");
	 
	goto Reply1;
    }
    
    jobSig.sigValue = sig_decode(jobSig.sigValue);
    sigValue = jobSig.sigValue;

    if (logclass & LC_SIGNAL)
        ls_syslog(LOG_DEBUG, "do_sigJob: sigValue =%d", sigValue);
    
    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
        if (jp->jobSpecs.jobId != jobSig.jobId)
            continue;
        found = TRUE;
        break;			
    }
    if (found == FALSE) {	
        reply = ERR_NO_JOB;
        jp = NULL;
        goto Reply1;
    }
    
    if (jobSig.reasons & SUSP_MBD_LOCK) {
        
        jp->jobSpecs.reasons = jobSig.reasons;
        jp->jobSpecs.subreasons = jobSig.subReasons;
        savedActReasons = jp->actReasons;       
        savedActSubReasons = jp->actSubReasons; 
        jp->actReasons = jobSig.reasons;
        jp->actSubReasons = jobSig.subReasons;
    }

    
    if (jp->postJobStarted) {
        reply = ERR_NO_ERROR;   
        goto Reply1;
    }
    
    if (IS_FINISH(jp->jobSpecs.jStatus)) {
        reply = ERR_NO_ERROR;	
        goto Reply1;
    }

    if (jp->jobSpecs.jobPGid == -1) {	
        SBD_SET_STATE(jp, JOB_STAT_EXIT);
        reply = ERR_NO_ERROR;	
        goto Reply;
    }
    
    if (!JOB_STARTED(jp)) { 
        if (isSigTerm(sigValue) == TRUE) {           
            if ((cc = jobSigStart (jp, sigValue, jobSig.actFlags, jobSig.chkPeriod, NO_SIGLOG)) < 0)   
           	reply = ERR_SIG_RETRY;
    	    else
            	reply = ERR_NO_ERROR;

            goto Reply;
        }
    
        reply = ERR_SIG_RETRY;

        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "%s: Retry signal %s for job <%s>",
                      fname, getLsbSigSymbol(sigValue), 
                      lsb_jobid2str(jp->jobSpecs.jobId));
        goto Reply1;     
    }
    
    if (IS_PEND(jp->jobSpecs.jStatus)) {        
        reply = ERR_SIG_RETRY;
        goto Reply1;       
    }
    
    if (jp->jobSpecs.actPid || (jp->jobSpecs.jStatus & JOB_STAT_MIG)) {

        if ((cc = jobSigStart (jp, sigValue, jobSig.actFlags, jobSig.chkPeriod, NO_SIGLOG)) < 0)
            reply = ERR_SIG_RETRY;
        else { 
            jp->jobSpecs.jStatus &= ~JOB_STAT_MIG;
            reply = ERR_NO_ERROR;
        }
        goto Reply;
    }    

    if ((cc = jobSigStart (jp, 
                           sigValue, 
                           jobSig.actFlags, 
                           jobSig.chkPeriod, 
                           NO_SIGLOG)) < 0)        
        reply = ERR_SIG_RETRY;
    else
        reply = ERR_NO_ERROR;
    
Reply:
    sbdlog_newstatus(jp); 

Reply1:

    xdrmem_create(&xdrs2, reply_buf, MSGSIZE, XDR_ENCODE);

    initLSFHeader_(&replyHdr);
    replyHdr.opCode = reply;
    if (reply == ERR_NO_ERROR) {
        jobReply.jobPid = jp->jobSpecs.jobPid;
        jobReply.actPid = jp->jobSpecs.actPid;
        jobReply.jobId = jp->jobSpecs.jobId;
        jobReply.jobPGid = jp->jobSpecs.jobPGid;
        jobReply.jStatus = jp->jobSpecs.jStatus;
        jobReply.reasons = jp->jobSpecs.reasons;
        jobReply.actStatus = jp->actStatus;  
        replyStruct = (char *) &jobReply;
    } else {        
        if (reply != ERR_NO_JOB)   
            if  ((jp != NULL) && (jobSig.reasons & SUSP_MBD_LOCK)) {
                jp->actReasons = savedActReasons;       
                jp->actSubReasons = savedActSubReasons; 
            }
        replyStruct = (char *) 0;
    }

    if (!xdr_encodeMsg(&xdrs2, replyStruct, &replyHdr, xdr_jobReply, 0, auth)) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
                  lsb_jobid2str(jp->jobSpecs.jobId), "xdr_jobReply");
        relife(fname, "xdr_jobReply");
    }
    
    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5821,
                                         "%s: Sending jobReply (len=%d) to master failed for job <%s>: %m"), fname, XDR_GETPOS(&xdrs2), lsb_jobid2str(jobSig.jobId)); /* catgets 5821 */
    }
    if (jp != NULL)
        jp->actStatus = ACT_NO;   

    xdr_destroy(&xdrs2);

    return;
}				


void
do_jobMsg(struct bucket * bucket, XDR *xdrs, int s, struct LSFHeader * reqHdr)
{
    static char      fname[] = "do_jobMsg()";
    struct Buffer    *buf;
    sbdReplyType     reply;
    struct jobCard   *jp = NULL;
    char             found = FALSE;

    LSBMSG_DECL(header, jmsg);

    if (logclass & LC_TRACE)
	ls_syslog(LOG_DEBUG, "%s: Entering this routine ...", fname);

    buf = bucket->storage;
    
    LSBMSG_INIT(header, jmsg);
    if (!xdr_lsbMsg(xdrs, &jmsg, reqHdr)) {
        reply = ERR_BAD_REQ;
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_lsbMsg");
        goto Reply1;
    }

    if (logclass & LC_EXEC)
        ls_syslog(LOG_DEBUG, "%s: jmsg len %d jobId %s msg <%s>",
                  fname, buf->len, lsb_jobid2str(jmsg.header->jobId),
                  jmsg.msg);
    
    xdrmem_create(&bucket->xdrs, buf->data, buf->len, XDR_DECODE);
    
    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
        if (jp->jobSpecs.jobId != jmsg.header->jobId)
            continue;
        found = TRUE;
        break;			
    }
    if (found == FALSE) {	
        reply = ERR_NO_JOB;

        goto Reply1;
    }

    reply = ERR_NO_ERROR;
Reply1:			
    if ( reply != ERR_BAD_REQ)
        LSBMSG_FINALIZE(xdrs, jmsg); 
    if (replyHdrWithRC(reply, s, jmsg.header->jobId) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(jmsg.header->jobId), "replyHdrWithRC");

        return;
    }
    
    LSBMSG_CACHE_BUFFER(bucket, jmsg);
    
    QUEUE_APPEND(bucket, jmQueue);
    bucket->bufstat = MSG_STAT_QUEUED;
    bucket->storage->stashed = TRUE;
    
    deliverMsg(bucket);
}				

void
deliverMsg(struct bucket * bucket)
{
    static char         fname[] = "deliverMsg()";
    struct clientNode   *cliPtr;
    struct clientNode   *nextClient;
    int                 nbytes;
    int                 found;
    struct jobCard      *jp;
    struct Buffer       *mbuf;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering this routine ...", fname);
    
    mbuf = bucket->storage;
    
    found = FALSE;
    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
        if (jp->jobSpecs.jobId == bucket->proto.jobId) {
            found = TRUE;
            break;
        }
    }

    if (!found) {
        chanFreeStashedBuf_(mbuf);
        QUEUE_REMOVE(bucket);
        FREE_BUCKET(bucket);
        return;
    }
        
    found = FALSE;
    for (cliPtr = clientList->forw; cliPtr != clientList;
         cliPtr = nextClient) {
        nextClient = cliPtr->forw;

        if (cliPtr->jobId == bucket->proto.jobId) {
            found = TRUE;
            break;
        }
    }

    if (found == FALSE) {
        return;
    }
        
    if (jp->jobSpecs.jStatus & JOB_STAT_RUN) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "\
%s: jobId <%s> len %d chanfd %d",
                      fname, lsb_jobid2str(bucket->proto.jobId), 
                      mbuf->len, cliPtr->chanfd);

        nbytes = b_write_fix(chanSock_(cliPtr->chanfd),
                             mbuf->data,
                             mbuf->len);

        if (nbytes <= 0) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
                      lsb_jobid2str(jp->jobSpecs.jobId), "b_write_fix");
            return;
        }
    } else if (IS_SUSP(jp->jobSpecs.jStatus)) {	
        return;
    }
    
    jp->delieveredMsgId = bucket->proto.msgId;

    if (status_job(BATCH_STATUS_MSG_ACK, jp, 0, 0) < 0) 
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
                  lsb_jobid2str(jp->jobSpecs.jobId), "status_job");

    if (mbuf->stashed) {
        QUEUE_REMOVE(bucket);
        chanFreeStashedBuf_(mbuf);
    } else {
        chanFreeBuf_(mbuf);
    }
    
    FREE_BUCKET(bucket);

    return;
   
    if (! mbuf->stashed) 
        QUEUE_APPEND(bucket, jmQueue);
    bucket->bufstat = MSG_STAT_QUEUED;
    mbuf->stashed = TRUE;

} 

void
do_reboot(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char        fname[] = "do_reboot()";
    char               reply_buf[MSGSIZE / 8];
    XDR                xdrs2;
    sbdReplyType       reply;
    struct LSFHeader   replyHdr;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    reply = ERR_NO_ERROR;
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE / 8, XDR_ENCODE);

    initLSFHeader_(&replyHdr);
    if (reqHdr->opCode == CMD_SBD_REBOOT)
        replyHdr.opCode = LSBE_NO_ERROR;
    else
        replyHdr.opCode = reply;

    if (!xdr_encodeMsg(&xdrs2, (char *) 0, &replyHdr, 0, 0, NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs2);
        relife(fname, "xdr_encodeMsg");
        return;
    }

    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "chanWrite_");
        xdr_destroy(&xdrs2);
        relife(fname, "chanWrite_");
        return;
    }
    ls_syslog(LOG_NOTICE, _i18n_msg_get(ls_catd , NL_SETN, 5828,
                                        "Slave batch daemon reboot command received")); /* catgets 5828 */
    relife(fname, "Slave batch daemon reboot");
    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5829,
                                     "Unable to relife during rebooting: %m")); /* catgets 5829 */
    xdr_destroy(&xdrs2);

}


int
ctrlSbdDebug( struct debugReq  *pdebug )
{
    static char   fname[]="ctrlSbdDebug()";
    int           opCode;
    int           level;
    int           newClass;
    int           options;
    char          logFileName[MAXLSFNAMELEN];
    char          lsfLogDir[MAXPATHLEN];
    char          *dir;
    char          dynDbgEnv[MAXPATHLEN];
    
    memset(logFileName, 0, sizeof(logFileName));
    memset(lsfLogDir, 0, sizeof(lsfLogDir));

    opCode = pdebug->opCode;
    level = pdebug->level;
    newClass = pdebug->logClass;
    options = pdebug->options;

    if (pdebug->logFileName[0] != '\0') {
        if (((dir = strrchr(pdebug->logFileName,'/')) != NULL) ||  
            ((dir = strrchr(pdebug->logFileName,'\\')) != NULL)) {
            dir++;
            ls_strcat(logFileName, sizeof(logFileName), dir);
            *(--dir) = '\0';
            ls_strcat(lsfLogDir, sizeof(lsfLogDir), pdebug->logFileName);
        }         
        else {
            ls_strcat(logFileName, sizeof(logFileName), pdebug->logFileName);
            if (daemonParams[LSF_LOGDIR].paramValue
                && *(daemonParams[LSF_LOGDIR].paramValue)) {
                ls_strcat(lsfLogDir, sizeof(lsfLogDir), 
                          daemonParams[LSF_LOGDIR].paramValue);
            } 
            else {
                lsfLogDir[0] = '\0';
            }       
        }
        ls_strcat(logFileName, sizeof(logFileName), ".sbatchd");
    }
    else {
  	ls_strcat(logFileName, sizeof(logFileName), "sbatchd");
        if (daemonParams[LSF_LOGDIR].paramValue
            && *(daemonParams[LSF_LOGDIR].paramValue)) {
            ls_strcat(lsfLogDir, sizeof(lsfLogDir), 
                      daemonParams[LSF_LOGDIR].paramValue);
        } else {
            lsfLogDir[0] = '\0';
        }
    }

    if (options==1) { 
        struct config_param *plp;
        for (plp = daemonParams; plp->paramName != NULL; plp++) {
            if (plp->paramValue != NULL)
                FREEUP(plp->paramValue);
        }

        if (initenv_(daemonParams, env_dir) < 0){
            ls_openlog("sbatchd", daemonParams[LSF_LOGDIR].paramValue,
                       (debug > 1), daemonParams[LSF_LOG_MASK].paramValue);
  	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "initenv_");
            die(SLAVE_FATAL);
            return (-1);
        }

        getLogClass_(daemonParams[LSB_DEBUG_SBD].paramValue,
                     daemonParams[LSB_TIME_SBD].paramValue);
        closelog();
        if (debug > 1)
            ls_openlog("sbatchd", daemonParams[LSF_LOGDIR].paramValue, TRUE,
                       daemonParams[LSF_LOG_MASK].paramValue);
        else
            ls_openlog("sbatchd", daemonParams[LSF_LOGDIR].paramValue, FALSE,
                       daemonParams[LSF_LOG_MASK].paramValue);

        if (logclass & LC_TRACE)
            ls_syslog(LOG_DEBUG, "%s: logclass=%x", fname, logclass);
        
        cleanDynDbgEnv();

        return(LSBE_NO_ERROR);
    } 

    if (opCode==SBD_DEBUG) { 
        putMaskLevel(level, &(daemonParams[LSF_LOG_MASK].paramValue));

        if (newClass>=0) {
            logclass = newClass;
            
            sprintf(dynDbgEnv, "%d", logclass);
            putEnv("DYN_DBG_LOGCLASS", dynDbgEnv);
        }

        if ( pdebug->level>=0 ){
            closelog();
            if (debug > 1)
                ls_openlog(logFileName, lsfLogDir, TRUE,
                           daemonParams[LSF_LOG_MASK].paramValue);
            else
                ls_openlog(logFileName, lsfLogDir, FALSE,
                           daemonParams[LSF_LOG_MASK].paramValue);
            
            putEnv("DYN_DBG_LOGDIR", lsfLogDir);
            putEnv("DYN_DBG_LOGFILENAME", logFileName);
            sprintf(dynDbgEnv, "%d", pdebug->level);
            putEnv("DYN_DBG_LOGLEVEL", dynDbgEnv);
        }
    }
    else if (opCode == SBD_TIMING) { 
        if (level>=0)
 	    timinglevel = level;
        if (pdebug->logFileName[0] != '\0') {
            if (debug > 1)
                ls_openlog(logFileName, lsfLogDir, 
                           TRUE, daemonParams[LSF_LOG_MASK].paramValue);
            else
                ls_openlog(logFileName, lsfLogDir,
                           FALSE, daemonParams[LSF_LOG_MASK].paramValue);
        }
    }
    else {
        ls_perror("No this debug command!\n");
        return (-1);
    }
    return (LSBE_NO_ERROR);
} 

void
do_sbdDebug(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char       fname[] = "do_sbdDebug()";
    struct debugReq    debugReq;
    char               reply_buf[MSGSIZE / 8];
    XDR                xdrs2;
    sbdReplyType       reply;
    struct LSFHeader   replyHdr;

    if (!xdr_debugReq(xdrs, &debugReq, reqHdr)) {
        reply = LSBE_XDR;
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_debugReq");
    }
    else 
        reply = ctrlSbdDebug(&debugReq);
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE / 8, XDR_ENCODE);
    initLSFHeader_(&replyHdr);
    replyHdr.opCode = reply;
    if (!xdr_encodeMsg(&xdrs2, (char *) 0, &replyHdr, 0, 0, NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs2);
        return;
    }				       

    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5833,
                                         "%s: Sending  reply to master failed : %m"), /* catgets 5833 */
                  fname);
        xdr_destroy(&xdrs2);
        return;
    }
    xdr_destroy(&xdrs2);
    return;

}  

void
do_shutdown(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char        fname[] = "do_shutdown()";
    char               reply_buf[MSGSIZE / 8];
    XDR                xdrs2;
    sbdReplyType       reply;
    struct LSFHeader   replyHdr;

    reply = ERR_NO_ERROR;
    
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE / 8, XDR_ENCODE);

    initLSFHeader_(&replyHdr);
    if (reqHdr->opCode == CMD_SBD_SHUTDOWN)
        replyHdr.opCode = LSBE_NO_ERROR;
    else
        replyHdr.opCode = reply;
    
    if (!xdr_encodeMsg(&xdrs2, (char *) 0, &replyHdr, 0, 0, NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs2);
        relife(fname, "xdr_encodeMsg");
        return;
    }

    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5835,
                                         "%s: Sending shutdown reply to master failed: %m"), /* catgets 5835 */
                  fname);
        xdr_destroy(&xdrs2);
        relife(fname, "reply to master failed");
        return;
    }
    xdr_destroy(&xdrs2);
    ls_syslog(LOG_NOTICE, _i18n_msg_get(ls_catd , NL_SETN, 5836,
                                        "Slave batch daemon shutdown command received")); /* catgets 5836 */
    die(SLAVE_SHUTDOWN);

} 


void
do_jobSetup(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char       fname[] = "do_jobSetup()";
    struct jobSetup   jsetup;
    struct jobCard    *jp = NULL;
    char              found = FALSE;
    struct jobCard    savejp;

    if (logclass & LC_EXEC)
        ls_syslog(LOG_DEBUG, "%s: Entering ...", fname);

    if (!xdr_jobSetup(xdrs, &jsetup, reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobSetup");
        return;
    }

    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
        if (jp->jobSpecs.jobId != jsetup.jobId)
            continue;
        found = TRUE;
        break;			
    }

    if (found == FALSE) {	
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5838,
                                         "%s: Job <%s> is not found"), /* catgets 5838 */
                  fname, lsb_jobid2str(jsetup.jobId));
        replyHdrWithRC(LSBE_NO_JOB, chfd, jsetup.jobId);	
        return;
    }
    if (jp->jobSpecs.actPid)
        return;

    memcpy((char *) &savejp, (char *) jp, sizeof(savejp));

    jp->execJobFlag |= JOB_EXEC_QPRE_KNOWN;
    if (jsetup.execJobFlag & JOB_EXEC_QPRE_OK)
        jp->execJobFlag |= JOB_EXEC_QPRE_OK;

    jp->jobSpecs.jobPid = jsetup.jobPid;
    jp->jobSpecs.jobPGid = jsetup.jobPGid;
    jp->jobSpecs.execUid = jsetup.execUid;
    strcpy(jp->jobSpecs.execUsername, jsetup.execUsername);
    jp->execGid = jsetup.execGid;
    strcpy(jp->execUsername, jsetup.execUsername);
    strcpy(jp->jobSpecs.execCwd, jsetup.execCwd);
    strcpy(jp->jobSpecs.execHome, jsetup.execHome);

    if (jsetup.jStatus & JOB_STAT_RUN) {
        if (!(jsetup.jStatus & JOB_STAT_PRE_EXEC))
            jp->jobSpecs.jStatus &= ~JOB_STAT_PRE_EXEC;

	if (status_job(BATCH_STATUS_JOB, jp, jp->jobSpecs.jStatus,
                       ERR_NO_ERROR) < 0) {
            memcpy((char *) jp, (char *) &savejp, sizeof(savejp));
            return;		
        }
        jp->execJobFlag |= JOB_EXEC_STARTED;

    } else {
        jp->jobSpecs.reasons = jsetup.reason;
        jp->collectedChild = TRUE;
        jp->notReported = 0;
        jp->exitPid = -1;
        jp->needReportRU = FALSE;
        jp->jobSpecs.jStatus = jsetup.jStatus;
        jp->w_status = jsetup.w_status;
        jp->lsfRusage = jsetup.lsfRusage;
        jp->cpuTime = jsetup.cpuTime;
        if (job_finish(jp, TRUE) < 0) {
            memcpy((char *) jp, (char *) &savejp, sizeof(savejp));
            return;		
        }
    }

    if (replyHdrWithRC(LSBE_NO_ERROR, chfd, jsetup.jobId) < 0) {	
        ls_syslog(LOG_DEBUG, "%s: Reply header failed for job <%s>",
                  fname, lsb_jobid2str(jsetup.jobId));
    }
    if (logclass & LC_EXEC)
        ls_syslog(LOG_DEBUG1, "%s: JobId %s jstatus %d reason %x  jobPid %d jobPGid %d execUid %d execGid <%d> execUser <%s> execHome <%s> execCwd <%s> execJobFlag %x cpuTime %f w_status %d",
                  fname, lsb_jobid2str(jsetup.jobId), jsetup.jStatus,
                  jsetup.reason, jsetup.jobPid,
                  jsetup.jobPGid, jsetup.execUid, jsetup.execGid,
                  jsetup.execUsername, jsetup.execHome, jsetup.execCwd,
                  jsetup.execJobFlag, jsetup.cpuTime, jsetup.w_status);
} 

void
do_jobSyslog(XDR * xdrs, int chfd, struct LSFHeader * reqHdr)
{
    static char        fname[] = "do_jobSyslog()";
    struct jobSyslog   sysMsg;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering ...", fname);

    if (!xdr_jobSyslog(xdrs, &sysMsg, reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_syslog");
        return;
    }
    if (replyHdrWithRC(LSBE_NO_ERROR, chfd, -1) < 0)	
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "replyHdrWithRC");
    ls_syslog(sysMsg.logLevel, sysMsg.msg);
}				

void
do_rmConn(XDR * xdrs, int s, struct LSFHeader * reqHdr, struct clientNode * cln)
{
    static char        fname[] = "do_rmConn()";
    char               src[LSB_MAX_SD_LENGTH];
    char               dest[LSB_MAX_SD_LENGTH];
    struct lsbMsgHdr   header;
    struct lsbMsg      m;
    int                found = FALSE;
    struct jobCard     *jp;
    int                freeMsg = FALSE;
	
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering ... s=%d chanfd=%d", fname, s,
                  cln->chanfd);
    
    header.src = src;
    header.dest = dest;
    m.header = &header;
    m.msg = "";

    if (xdrs->x_op == XDR_DECODE)
        freeMsg = TRUE;
    if (!xdr_lsbMsg(xdrs, &m, reqHdr)) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_lsMsg");
	replyHdrWithRC(LSBE_XDR, s, m.header->jobId);
	return;
    }
    

    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
	if (jp->jobSpecs.jobId == m.header->jobId){
	    found = TRUE;
	    break;
        }
    }

    if (!found) {
	replyHdrWithRC(LSBE_NO_JOB, s, m.header->jobId);	    
	shutDownClient(cln);
        if (freeMsg && m.msg)
            FREEUP (m.msg);
	return;
    }

    jp->client = cln;
    if (jp->client == NULL) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5843,
                                         "%s: jp->client == NULL for parallel job <%s>"), /* catgets 5843 */
                  fname, lsb_jobid2str(m.header->jobId));
    }

    
    jp->newPam = TRUE;
    
    cln->jp = jp;
    cln->jobId = m.header->jobId;
    if (replyHdrWithRC(LSBE_NO_ERROR, s, m.header->jobId) < 0)
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(m.header->jobId),
                  "replyHdrWithRC");

    if (freeMsg && m.msg)
        FREEUP(m.msg);
}				

void
do_lsbMsg(XDR *xdrs, int s, struct LSFHeader * reqHdr)
{
    static char             fname[] = "do_lsbMsg";
    char                    src[LSB_MAX_SD_LENGTH];
    char                    dest[LSB_MAX_SD_LENGTH];
    struct lsbMsgHdr        header;
    
    struct lsbMsg           m;
    char                    opString[MSGSIZE];
    char                    *buf;
    int found = FALSE;
    struct jobCard *jp = NULL;
    int ret, cnt;
    LS_LONG_INT jobId;
    int freeMsg = FALSE;

    if (logclass & LC_TRACE)
	ls_syslog(LOG_DEBUG, "%s: Entering ...", fname);

    header.src = src;
    header.dest = dest;
    m.header = &header;
    m.msg = "";

    if (xdrs->x_op == XDR_DECODE)
        freeMsg = TRUE;
    if (!xdr_lsbMsg(xdrs, &m, reqHdr)) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_lsMsg");
	replyHdrWithRC(LSBE_XDR, s, m.header->jobId);	
	return;
    }

    buf = m.msg;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: received a user msg <%s>.", fname, buf);
    if (strcmp(header.dest, "sbatchd")) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5846,
                                         "%s: Ignore message not intended for sbatchd"), /* catgets 5846 */
                  fname);
	replyHdrWithRC(LSBE_PROTOCOL, s, m.header->jobId);	
        if (freeMsg && m.msg)
            FREEUP(m.msg);
	return;
    }

    ret = sscanf(buf, "%s%lld%n", opString, &jobId, &cnt);
    if (ret != 2) {
	ls_syslog(LOG_DEBUG, "%s: sscanf opString/jobId failed ret=%d: %m",
		  fname, ret);
	replyHdrWithRC(LSBE_PROTOCOL, s, m.header->jobId);
        if (freeMsg && m.msg)
            FREEUP(m.msg);
	return;
    }

    buf += cnt + 1;

    if (m.header->jobId && m.header->jobId != jobId) {
	ls_syslog(LOG_ERR, "%s: mjobId=%s != jobId=%s", fname,
                  lsb_jobid2str(m.header->jobId), lsb_jobid2str(jobId));
	replyHdrWithRC(LSBE_PERMISSION, s, m.header->jobId);
        if (freeMsg && m.msg)
            FREEUP(m.msg);
	return;
    }

    if (logclass & LC_COMM)
	ls_syslog(LOG_DEBUG, "%s: Got opString <%s> from job <%s>",
		  fname, opString, lsb_jobid2str(jobId));

    for (jp = jobQueHead->forw; (jp != jobQueHead); jp = jp->forw) {
	if (jp->jobSpecs.jobId != jobId)
	    continue;
	found = TRUE;
	break;			
    }

    if (!found) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5848,
                                         "%s: JobId <%s> not found"), fname, lsb_jobid2str(jobId)); /* catgets 5848 */
	replyHdrWithRC(LSBE_NO_JOB, s, m.header->jobId);
        if (freeMsg && m.msg)
            FREEUP(m.msg);
	return;
    }
    
    if (!strcmp(opString, "REG_OP"))
	do_regOpMsg(s, jp, &m, buf);
    else if (!strcmp(opString, "GET_PID"))
	do_getPid(s, jp, &m, buf);
    else if (!strcmp(opString, "CHKPNT_END"))
	do_chkpntEndMsg(s, jp, &m, buf);
    else if (!strcmp(opString, "RUSAGE"))
	do_rusageMsg(s, jp, &m, buf);
    else if (!strcmp(opString, "SYSLOG"))
	do_syslogMsg(s, buf);
    else {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5849,
                                         "%s: Got bad opstring <%s>"), fname, opString); /* catgets 5849 */
	replyHdrWithRC(LSBE_PROTOCOL, s, m.header->jobId);
    }
    if (freeMsg && m.msg)
        FREEUP(m.msg);
    
} 


static int
replyHdrWithRC(int rc, int chfd, int jobId)
{
    static char fname[] = "replyHdrWithRC()";
    XDR                     xdrs2;
    char                    reply_buf[MSGSIZE];
    struct LSFHeader        replyHdr;
    LS_LONG_INT tmpJobId;

    initLSFHeader_(&replyHdr);
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE, XDR_ENCODE);
    replyHdr.opCode = rc;
    replyHdr.length = 0;
    tmpJobId = jobId;

    if (!xdr_LSFHeader(&xdrs2, &replyHdr)) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, lsb_jobid2str(tmpJobId),
		  "xdr_LSFHeader");
	xdr_destroy(&xdrs2);
	return (-1);
    }
    
    if (chanWrite_(chfd, reply_buf, XDR_GETPOS(&xdrs2)) <= 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, lsb_jobid2str(tmpJobId), "chanWrite_");
	xdr_destroy(&xdrs2);
	return (-1);
    }
    xdr_destroy(&xdrs2);
    return (0);
}				


static void
do_syslogMsg(int s, char *buf)
{
    static char fname[] = "do_syslog";
    char *level;

    if (logclass & LC_TRACE)
	ls_syslog(LOG_DEBUG, "%s: Entering ...", fname);

    if (replyHdrWithRC(LSBE_NO_ERROR, s, -1) < 0)
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "replyHdrWithRC");

    if ((level = getNextWord_(&buf)) == NULL) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5853,
                                         "%s: got garbage for level"), fname); /* catgets 5853 */
	return;
    }

    ls_syslog(atoi(level), buf);
}		


 
static void
do_rusageMsg(int s, struct jobCard *jp, struct lsbMsg *m, char *workMsgBuf)
{
    static char fname[] = "do_rusageMsg()";
    int rc;

    if (!(jp->regOpFlag & REG_RUSAGE)) {
	rc = LSBE_PERMISSION;
    } else {
	rc = updateRUsageFromSuper(jp, workMsgBuf);	
    }

    if (replyHdrWithRC(rc, s, m->header->jobId) < 0)
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(m->header->jobId), "replyHdrWithRC");
} 

static void
do_regOpMsg(int s, struct jobCard *jp, struct lsbMsg *m, char *workMsgBuf)
{
    static char fname[] = "do_regOpMsg()";
    char *op;

    ls_syslog(LOG_INFO,"\
%s: registering supervisor for job=%d", fname, jp->jobSpecs.jobId);

    if (jp) {
	while ((op = getNextWord_(&workMsgBuf))) {
	    ls_syslog(LOG_INFO,"%s: operation=(%s)", fname, op);
	    if (!strcmp(op, "CHKPNT"))
		jp->regOpFlag |= REG_CHKPNT;
	    else if (!strcmp(op, "RUSAGE"))
		jp->regOpFlag |= REG_RUSAGE;
	    else if (!strcmp(op, "SIGNAL"))
		jp->regOpFlag |= REG_SIGNAL;
	    else if (!strcmp(op, "NICE"))
		jp->regOpFlag |= REG_NICE;
	    else
		ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5855,
                                                 "%s: Unknown reg op <%s> for job <%s>"), /* catgets 5855 */
                          fname, op, lsb_jobid2str(jp->jobSpecs.jobId));
	}
    }

    if (replyHdrWithRC(LSBE_NO_ERROR, s, m->header->jobId) < 0)
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(m->header->jobId), "replyHdrWithRC");
	
    if (jp->regOpFlag & REG_RUSAGE) {
	struct lsbMsgHdr pmMsgHdr;
	struct lsbMsg pmMsg;
	char regParam[128];
	int msgStat;

	pmMsgHdr.usrId = jp->jobSpecs.userId;
	pmMsgHdr.msgId = 0;
	pmMsgHdr.jobId = jp->jobSpecs.jobId;
	pmMsgHdr.src = "sbatchd";
	pmMsgHdr.dest = "pm";
	pmMsg.header = &pmMsgHdr;
	pmMsg.msg = regParam;

	sprintf(regParam, "PM_CONFIG RUSAGE_UPDATE_INTERVAL %d",
		rusageUpdateRate * sbdSleepTime);

	if ( (msgStat = msgSupervisor(&pmMsg, jp->client)) < 0 ) {
	    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5857,
                                             "%s: sending signal message to pm failed for job <%s>: %m"), fname, lsb_jobid2str(jp->jobSpecs.jobId)); /* catgets 5857 */
	}
    }
} 

static void
do_chkpntEndMsg(int s, struct jobCard *jp, struct lsbMsg *m, char *workMsgBuf)
{
    static char fname[] = "do_chkpntEndMsg";

    if (replyHdrWithRC(LSBE_NO_ERROR, s, m->header->jobId) < 0)
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(m->header->jobId), "replyHdrWithRc");
    if (killpg(-jp->jobSpecs.actPid, SIGURG) < 0) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
                  lsb_jobid2str(m->header->jobId), "fork");
    }	
} 

static void
do_getPid(int s, struct jobCard *jp, struct lsbMsg *m, char *workMsgBuf)
{
    static char fname[] = "do_getPid/sbd.serv.c";
    struct lsbMsgHdr pmMsgHdr;
    struct lsbMsg pmMsg;
    char regParam[128];
    int msgStat;

    if (replyHdrWithRC(LSBE_NO_ERROR, s, m->header->jobId) < 0)
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
                  lsb_jobid2str(m->header->jobId), "replyHdrWithRc");
    pmMsgHdr.usrId = jp->jobSpecs.userId;
    pmMsgHdr.msgId = 0;
    pmMsgHdr.jobId = jp->jobSpecs.jobId;
    pmMsgHdr.src = "sbatchd";
    pmMsgHdr.dest = "pm";
    pmMsg.header = &pmMsgHdr;
    pmMsg.msg = regParam;
    sprintf(regParam, "PID %d", jp->jobSpecs.jobPid);
    if ( (msgStat = msgSupervisor(&pmMsg, jp->client)) < 0 ) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5857,
                                         "%s: sending signal message to pm failed for job <%s>: %m"), /* catgets 5857 */
                  fname, lsb_jobid2str(jp->jobSpecs.jobId));
    }
} 

/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: sbd.comm.c 397 2007-11-26 19:04:00Z mblack $
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

#include "../../lsf/lib/lsi18n.h"

#define LOG_MASK(pri)   (1 << (pri))

extern int connTimeout;
extern int readTimeout;
extern int sbdLogMask;
static int msgSbd(LS_LONG_INT, char *, sbdReqType, int (*)());
extern int jRusageUpdatePeriod;

#define NL_SETN     11
int
status_job(mbdReqType reqType,
           struct jobCard *jp,
           int newStatus,
           sbdReplyType err)
{
    static char        fname[] = "status_job()";
    static int         seq = 1;
    static char        lastHost[MAXHOSTNAMELEN];
    int                reply;
    char               *request_buf;
    char               *reply_buf = NULL;
    XDR                xdrs;
    struct LSFHeader   hdr;
    int                cc;
    struct statusReq   statusReq;
    int                flags;
    int                i;
    int                len;
    struct lsfAuth     *auth = NULL;

    if ((logclass & LC_TRACE) && (logclass & LC_SIGNAL))
        ls_syslog(LOG_DEBUG, "%s: Entering ... regType %d jobId %s",
                  fname, reqType, lsb_jobid2str(jp->jobSpecs.jobId));

    if ( newStatus == JOB_STAT_EXIT ) {
        jp->userJobSucc = FALSE;
    }

    if ( MASK_STATUS(newStatus) == JOB_STAT_DONE ) {
        jp->userJobSucc = TRUE;
    }

    if ( IS_POST_FINISH(newStatus) ) {
        if ( jp->userJobSucc != TRUE ) {
            return 0;
        }
    }

    if (masterHost == NULL)
        return -1;

    if (jp->notReported < 0) {
        jp->notReported = -INFINIT_INT;
        return (0);
    }

    statusReq.jobId = jp->jobSpecs.jobId;
    statusReq.actPid = jp->jobSpecs.actPid;
    statusReq.jobPid = jp->jobSpecs.jobPid;
    statusReq.jobPGid = jp->jobSpecs.jobPGid;
    statusReq.newStatus = newStatus;
    statusReq.reason = jp->jobSpecs.reasons;
    statusReq.subreasons = jp->jobSpecs.subreasons;
    statusReq.sbdReply = err;
    statusReq.lsfRusage = jp->lsfRusage;
    statusReq.execUid = jp->jobSpecs.execUid;
    statusReq.numExecHosts = 0;
    statusReq.execHosts = NULL;
    statusReq.exitStatus = jp->w_status;
    statusReq.execCwd=jp->jobSpecs.execCwd;
    statusReq.execHome=jp->jobSpecs.execHome;
    statusReq.execUsername = jp->execUsername;
    statusReq.queuePostCmd = "";
    statusReq.queuePreCmd = "";
    statusReq.msgId = jp->delieveredMsgId;

    if ( IS_FINISH(newStatus) ) {
        if (jp->maxRusage.mem > jp->runRusage.mem)
            jp->runRusage.mem = jp->maxRusage.mem;
        if (jp->maxRusage.swap > jp->runRusage.swap)
            jp->runRusage.swap = jp->maxRusage.swap;
        if (jp->maxRusage.stime > jp->runRusage.stime)
            jp->runRusage.stime = jp->maxRusage.stime;
        if (jp->maxRusage.utime > jp->runRusage.utime)
            jp->runRusage.utime = jp->maxRusage.utime;
    }
    statusReq.runRusage.mem = jp->runRusage.mem;
    statusReq.runRusage.swap = jp->runRusage.swap;
    statusReq.runRusage.utime = jp->runRusage.utime;
    statusReq.runRusage.stime = jp->runRusage.stime;
    statusReq.runRusage.npids = jp->runRusage.npids;
    statusReq.runRusage.pidInfo = jp->runRusage.pidInfo;
    statusReq.runRusage.npgids = jp->runRusage.npgids;
    statusReq.runRusage.pgid = jp->runRusage.pgid;
    statusReq.actStatus = jp->actStatus;
    statusReq.sigValue  = jp->jobSpecs.actValue;
    statusReq.seq = seq;
    seq++;
    if (seq >= MAX_SEQ_NUM)
        seq = 1;

    len = 1024 +
        ALIGNWORD_(sizeof (struct statusReq));

    len += ALIGNWORD_(strlen (statusReq.execHome)) + 4 +
        ALIGNWORD_(strlen (statusReq.execCwd)) + 4 +
        ALIGNWORD_(strlen (statusReq.execUsername)) + 4;

    for (i = 0; i < statusReq.runRusage.npids; i++)
        len += ALIGNWORD_(sizeof (struct pidInfo)) + 4;

    for (i = 0; i < statusReq.runRusage.npgids; i++)
        len += ALIGNWORD_(sizeof (int)) + 4;

    if (logclass & (LC_TRACE | LC_COMM))
        ls_syslog(LOG_DEBUG, "%s: The length of the job message is: <%d>", fname, len);

    if ((request_buf = malloc(len)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        return (-1);
    }

    xdrmem_create(&xdrs, request_buf, len, XDR_ENCODE);
    initLSFHeader_(&hdr);
    hdr.opCode = reqType;

    if (!xdr_encodeMsg (&xdrs, (char *)&statusReq, &hdr, xdr_statusReq, 0,
                        auth)) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M,
                  fname, lsb_jobid2str(jp->jobSpecs.jobId), "xdr_statusReq");
        lsb_merr2( I18N_FUNC_FAIL, fname, "xdr_statusReq");
        xdr_destroy(&xdrs);
        FREEUP(request_buf);
        relife(fname, "xdr_statusReq");
    }

    flags = CALL_SERVER_NO_HANDSHAKE;
    if (statusChan >= 0)
        flags |= CALL_SERVER_USE_SOCKET;

    if (reqType == BATCH_RUSAGE_JOB)
        flags |= CALL_SERVER_NO_WAIT_REPLY;

    if (logclass & LC_COMM)
        ls_syslog(LOG_DEBUG1,"%s: before call_server statusChan=%d flags=%d",fname,statusChan,flags);

    cc = call_server(masterHost,
                     mbd_port,
                     request_buf,
                     XDR_GETPOS(&xdrs),
                     &reply_buf,
                     &hdr,
                     connTimeout,
                     readTimeout,
                     &statusChan,
                     NULL,
                     NULL,
                     flags);
    if (cc < 0) {
        statusChan = -1;
        if (!equalHost_(masterHost, lastHost)) {
            if (errno != EINTR)
                ls_syslog (LOG_DEBUG, "%s: Failed to reach mbatchd on host <%s> for job <%s>: %s", fname, masterHost, lsb_jobid2str(jp->jobSpecs.jobId), lsb_sysmsg());
            strcpy(lastHost, masterHost);
        }
        xdr_destroy(&xdrs);
        FREEUP(request_buf);
        failcnt++;
        return(-1);
    } else if (cc == 0) {

    }

    failcnt = 0;
    lastHost[0] = '\0';
    xdr_destroy(&xdrs);
    FREEUP(request_buf);

    if (cc)
        free(reply_buf);

    if (flags & CALL_SERVER_NO_WAIT_REPLY) {

        struct timeval timeval;

        timeval.tv_sec = 0;
        timeval.tv_usec = 0;

        if (rd_select_(chanSock_(statusChan), &timeval) == 0) {
            jp->needReportRU = FALSE;
            jp->lastStatusMbdTime = now;
            return 0;
        }

        CLOSECD(statusChan);

        if (logclass & LC_COMM)
            ls_syslog(LOG_DEBUG1, "%s: Job <%s> rd_select() failed, assume connection broken", fname, lsb_jobid2str(jp->jobSpecs.jobId));
        return(-1);
    }
    reply = hdr.opCode;
    switch (reply) {
        case LSBE_NO_ERROR:
        case LSBE_LOCK_JOB:
            jp->needReportRU = FALSE;
            jp->lastStatusMbdTime = now;
            if (reply == LSBE_LOCK_JOB) {
                if (IS_SUSP (jp->jobSpecs.jStatus))
                    jp->jobSpecs.reasons |= SUSP_MBD_LOCK;
                else
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5204,
                                                     "%s: Job <%s> is in status <%x> and mbatchd wants to lock it, ignored."), /* catgets 5204 */
                              fname,
                              lsb_jobid2str(jp->jobSpecs.jobId),
                              jp->jobSpecs.jStatus);
            }
            return (0);
        case LSBE_NO_JOB:
            if ( !IS_POST_FINISH(jp->jobSpecs.jStatus) ) {
                ls_syslog( LOG_ERR, _i18n_msg_get( ls_catd , NL_SETN, 5205,
                                                   "%s: Job <%s> is forgotten by mbatchd on host <%s>, ignored."), fname, lsb_jobid2str(jp->jobSpecs.jobId), masterHost); /* catgets 5205 */
            }

            jp->notReported = -INFINIT_INT;
            return (0);
        case LSBE_STOP_JOB:
            if (jobsig (jp, SIGSTOP, TRUE) < 0)
                SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_EXIT);
            else {
                SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_USUSP);
                jp->jobSpecs.reasons |= SUSP_USER_STOP;
            }
            return (-1);
        case LSBE_SBATCHD:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5206,
                                             "%s: mbatchd on host <%s> doesn't think I'm configured as a batch server when I report the status for job <%s>"), /* catgets 5206 */
                      fname,
                      masterHost,
                      lsb_jobid2str(jp->jobSpecs.jobId));
            return (-1);
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5207,
                                             "%s: Illegal reply code <%d> from mbatchd on host <%s> for job <%s>"), /* catgets 5207 */
                      fname,
                      reply,
                      masterHost,
                      lsb_jobid2str(jp->jobSpecs.jobId));
            return(-1);
    }
}

void
getJobsState(struct sbdPackage *sbdPackage)
{
    mbdReqType mbdReqtype;
    char request_buf[MSGSIZE/8];
    char *reply_buf;
    char *myhostnm;
    XDR xdrs;
    struct LSFHeader hdr;
    int reply;
    int i;
    int cc;
    int numJobs;
    struct jobSpecs jobSpecs;
    struct jobCard *job = NULL;

    if ((myhostnm = ls_getmyhostname()) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: Ohmygosh failed to get my hostname... %M", __func__);
        die(SLAVE_FATAL);
    }

znovu:
    mbdReqtype = BATCH_SLAVE_RESTART;
    xdrmem_create(&xdrs, request_buf, MSGSIZE/8, XDR_ENCODE);
    initLSFHeader_(&hdr);
    hdr.opCode = mbdReqtype;

    if (! xdr_encodeMsg(&xdrs, NULL, &hdr, NULL, 0, NULL)) {
        ls_syslog(LOG_ERR, "\
%s: Ohmygosh failed to encode BATCH_SLAVE_RESTART", __func__);
        xdr_destroy(&xdrs);
        lsb_merr("Failed in xdr for BATCH_SLAVE_RESTART\n");
        die(SLAVE_FATAL);
    }

    for (;;) {
        cc = call_server(masterHost,
                         mbd_port,
                         request_buf,
                         XDR_GETPOS(&xdrs),
                         &reply_buf,
                         &hdr,
                         connTimeout,
                         60 * 30,
                         NULL,
                         NULL,
                         NULL,
                         0);
        if (cc >= 0)
            break;

        /* If we did not get an answer from MBD then
         * either we are booting on the master host
         * so we have to start the master, or the master
         * is down for whatever reason and we just have
         * to retry,
         */
        if (equalHost_(masterHost, myhostnm)) {
            if (mbdPid == 0
                || (kill(mbdPid, 0) != 0)) {
                now = time(0);
                start_master();
                millisleep_(sbdSleepTime * 1000);
                continue;
            }
        }

        ls_syslog(LOG_ERR, "\
%s: mbatchd on host %s not responding: %s, EAGAIN...",
                  __func__, masterHost, lsb_sysmsg());

        millisleep_(sbdSleepTime * 1000);
        masterHost = ls_getmastername();
        while (masterHost == NULL) {
            /* Meanwhile master might have
             * changed...
             */
            int cnt = 0;
            if (cnt == 0) {
                ls_syslog(LOG_ERR, "\
%s: waiting to get my master name %M", __func__);
                cnt = 10;
            }
            --cnt;
            millisleep_(sbdSleepTime * 1000);
            masterHost = ls_getmastername();
        }

    } /* for (call_server()) */

    master_unknown = FALSE;
    xdr_destroy(&xdrs);

    reply = hdr.opCode;
    switch (reply) {
        char ebuf[256];

        case LSBE_NO_ERROR:
            xdrmem_create(&xdrs, reply_buf, cc, XDR_DECODE);
            if (!xdr_sbdPackage (&xdrs, sbdPackage, &hdr)) {
                ls_syslog(LOG_ERR, "\
%s: Ohmygosh failed to decode sbdPackage from %s", __func__, masterHost);
                xdr_destroy(&xdrs);
                if (cc)
                    free(reply_buf);
                return;
            }
            numJobs = sbdPackage->numJobs;

            for (i = 0; i < numJobs; i++) {

                if (!xdr_arrayElement(&xdrs,
                                      (char *)&jobSpecs, &hdr,
                                      xdr_jobSpecs)) {
                    sprintf(ebuf, "\
%s: Ohmygosh failed to decode jobSpecs from %s", __func__, masterHost);
                    ls_syslog(LOG_ERR, "%s", ebuf);
                    lsb_merr(ebuf);
                    xdr_destroy(&xdrs);
                    if (cc)
                        free(reply_buf);
                    return;
                }
                job = addJob(&jobSpecs, hdr.version);
                xdr_lsffree(xdr_jobSpecs, (char *)&jobSpecs, &hdr);
            }

            if (!xdr_sbdPackage1 (&xdrs, sbdPackage, &hdr)) {
                sprintf(ebuf, "\
%s: Ohmygosh failed to decode sbdPackage from %s", __func__, masterHost);
                    ls_syslog(LOG_ERR, "%s", ebuf);
                    lsb_merr(ebuf);
            }
            xdr_destroy(&xdrs);
            if (cc)
                free(reply_buf);

            ls_syslog(LOG_DEBUG, "\
%s: got configuration from master %s all right", __func__, masterHost);

            return;

        case LSBE_BAD_HOST:
            ls_syslog(LOG_ERR, "\
%s: This host is not yet used by the batch system... EAGAIN",
                      __func__);
            free(reply_buf);
            millisleep_(sbdSleepTime * 1000);
            goto znovu;
        case LSBE_PORT:
            ls_syslog(LOG_ERR, "\
%s: mbatchd reports that it is using a bad port",
                __func__);
            free(reply_buf);
            die(SLAVE_FATAL);
        default:
            ls_syslog(LOG_ERR, "\
%s: Invalid return code %d from mbatchd on %s",
                      __func__, reply, masterHost);
            xdr_destroy(&xdrs);
            die(SLAVE_FATAL);
    }
}

void
jobSetupStatus(int jStatus, int pendReason, struct jobCard *jp)
{
    static char       fname[] = "jobSetupStatus()";
    struct jobSetup   jsetup;

    jsetup.jobId = jp->jobSpecs.jobId;
    jsetup.jStatus = jStatus;
    jsetup.reason = pendReason;
    jsetup.jobPid = jp->jobSpecs.jobPid;
    jsetup.jobPGid = jp->jobSpecs.jobPGid;
    jsetup.execGid = jp->execGid;
    jsetup.execUid = jp->jobSpecs.execUid;
    jsetup.execJobFlag = jp->execJobFlag;

    strcpy(jsetup.execUsername, jp->execUsername);
    strcpy (jsetup.execCwd, jp->jobSpecs.execCwd);
    strcpy (jsetup.execHome, jp->jobSpecs.execHome);
    jsetup.lsfRusage = jp->lsfRusage;
    jsetup.cpuTime = jp->cpuTime;
    jsetup.w_status = jp->w_status;

    while (msgSbd(jp->jobSpecs.jobId, (char *) &jsetup, SBD_JOB_SETUP,
                  xdr_jobSetup) == -1) {
        ls_syslog(LOG_DEBUG, "%s: Job %s msgSbd() failed", fname,
                  lsb_jobid2str(jp->jobSpecs.jobId));
        millisleep_(10000);
    }

    if (jStatus & JOB_STAT_RUN)
        return;

    exit(jp->w_status);

}

void
sbdSyslog(int level, char *msg)
{
    struct jobSyslog slog;


    if ((sbdLogMask & LOG_MASK(level)) != 0) {
        slog.logLevel = level;
        STRNCPY(slog.msg, msg, MAXLINELEN);
        msgSbd(-1, (char *) &slog, SBD_SYSLOG, xdr_jobSyslog);

    }

}

static int
msgSbd(LS_LONG_INT jobId, char *req, sbdReqType reqType, int (*xdrFunc)())
{
    static char fname[] = "msgSbd";
    XDR xdrs;
    struct LSFHeader hdr;
    char requestBuf[MSGSIZE];
    char *reply_buf = NULL;
    int cc, retryInterval;
    char *myhostnm;

    initLSFHeader_(&hdr);
    hdr.opCode = reqType;
    xdrmem_create(&xdrs, requestBuf, sizeof(requestBuf), XDR_ENCODE);

    if (!xdr_encodeMsg(&xdrs, req, &hdr, xdrFunc, 0, NULL)) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, lsb_jobid2str(jobId), "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        return(-1);
    }

    for (retryInterval = 5, cc = -1; cc != 0;) {


        initenv_(daemonParams, env_dir);
        get_ports();

        if ((myhostnm = ls_getmyhostname()) == NULL) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, lsb_jobid2str(jobId),
                      "ls_getmyhostname");
            myhostnm = "localhost";
        }

        cc = call_server(myhostnm, sbd_port, requestBuf, XDR_GETPOS(&xdrs),
                         &reply_buf, &hdr, 100000, 0, NULL, NULL, NULL, 0);
        if (cc < 0) {
            ls_syslog(LOG_DEBUG, "%s: Job <%s> call_server(%s,%d) failed: %s",
                      fname, lsb_jobid2str(jobId), myhostnm,
                      ntohs(sbd_port), lsb_sysmsg());
            millisleep_(retryInterval*1000);
            retryInterval *= 2;
            if (retryInterval > sbdSleepTime)
                retryInterval = sbdSleepTime;
        }
    }

    xdr_destroy(&xdrs);

    if (hdr.opCode != LSBE_NO_ERROR) {
        lsberrno = hdr.opCode;
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "msgSbd: Job <%s> got error <%s>, exiting",
                      lsb_jobid2str(jobId), lsb_sysmsg());
        exit(-1);
    }

    return (0);
}


int
msgSupervisor(struct lsbMsg *lsbMsg, struct clientNode *cliPtr)
{
    static char fname[] = "msgSupervisor/sbd.comm.c";
    struct LSFHeader reqHdr;
    char reqBuf[MSGSIZE*2];
    int cc;
    XDR xdrs;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering this routine ...", fname);

    if (cliPtr == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5220,
                                         "%s: cliPtr is null"), fname); /* catgets 5220 */
        return (-1);
    }

    initLSFHeader_(&reqHdr);
    xdrmem_create(&xdrs, reqBuf, sizeof(reqBuf), XDR_ENCODE);

    if (!xdr_encodeMsg(&xdrs, (char *) lsbMsg, &reqHdr, xdr_lsbMsg, 0, NULL)) {
        if (logclass & LC_COMM)
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        return (-1);
    }

    if ((cc = b_write_fix(chanSock_(cliPtr->chanfd), reqBuf,
                          XDR_GETPOS(&xdrs)))
        != XDR_GETPOS(&xdrs)) {
        if (logclass & LC_COMM)
            ls_syslog(LOG_ERR, I18N_FUNC_D_D_FAIL_M, fname, "b_write_fix",
                      chanSock_(cliPtr->chanfd),
                      XDR_GETPOS(&xdrs));
        xdr_destroy(&xdrs);
        return (-1);
    }

    xdr_destroy(&xdrs);
    return (0);
}

#ifdef INTER_DAEMON_AUTH
int
getSbdAuth(struct lsfAuth *auth)
{
    static char fname[] = "getSbdAuth";
    int rc;
    char buf[1024];

    if (daemonParams[LSF_AUTH_DAEMONS].paramValue == NULL)
        return 0;

    putEauthClientEnvVar("sbatchd");
    sprintf(buf, "mbatchd@%s", clusterName);
    putEauthServerEnvVar(buf);

    rc = getAuth_(auth, masterHost);

    if (rc) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "getAuth_");
    }

    return (rc);
}

#endif


int
sendUnreportedStatus (struct chunkStatusReq *chunkStatusReq)
{
    static char        fname[] = "sendUnreportedStatus()";
    static char        lastHost[MAXHOSTNAMELEN];
    int                reply;
    char               *request_buf;
    char               *reply_buf = NULL;
    XDR                xdrs;
    struct LSFHeader   hdr;
    int                cc;
    int                flags;
    int                i;
    int                j;
    int                len;
    struct lsfAuth     *auth = NULL;

    if ((logclass & LC_TRACE) && (logclass & LC_SIGNAL))
        ls_syslog(LOG_DEBUG, "%s: Entering ... regType %d",
                  fname, BATCH_STATUS_CHUNK);

    if (masterHost == NULL)
        return -1;

    len = ALIGNWORD_(sizeof (struct chunkStatusReq))
        + ALIGNWORD_(sizeof (struct statusReq *) * chunkStatusReq->numStatusReqs);

    for (i=0; i<chunkStatusReq->numStatusReqs; i++) {
        len += 1024 +
            ALIGNWORD_(sizeof (struct statusReq));
        len += ALIGNWORD_(strlen (chunkStatusReq->statusReqs[i]->execHome)) + 4 +
            ALIGNWORD_(strlen (chunkStatusReq->statusReqs[i]->execCwd)) + 4 +
            ALIGNWORD_(strlen (chunkStatusReq->statusReqs[i]->execUsername)) + 4;

        for (j = 0; j < chunkStatusReq->statusReqs[i]->runRusage.npids; j++)
            len += ALIGNWORD_(sizeof (struct pidInfo)) + 4;
        for (j = 0; j < chunkStatusReq->statusReqs[i]->runRusage.npgids; j++)
            len += ALIGNWORD_(sizeof (int)) + 4;

    }

    if (logclass & (LC_TRACE | LC_COMM))
        ls_syslog(LOG_DEBUG, "\
%s: The length of the job message is: <%d>", fname, len);

    if ((request_buf = (char *) malloc(len)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        return (-1);
    }

    xdrmem_create(&xdrs, request_buf, len, XDR_ENCODE);
    initLSFHeader_(&hdr);
    hdr.opCode = BATCH_STATUS_CHUNK;

    if (!xdr_encodeMsg (&xdrs, (char *)chunkStatusReq, &hdr, xdr_chunkStatusReq, 0,
                        auth)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL,
                  fname, "xdr_chunkStatusReq");
        lsb_merr2( I18N_FUNC_FAIL, fname, "xdr_chunkStatusReq");
        xdr_destroy(&xdrs);
        FREEUP(request_buf);
        relife(fname, "xdr_chunkStatusReq");
    }

    flags = CALL_SERVER_NO_HANDSHAKE;
    if (statusChan >= 0) {
        flags |= CALL_SERVER_USE_SOCKET;
    }
    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG1,"%s: before call_server statusChan=%d flags=%d",
                  fname, statusChan, flags);
    }
    cc = call_server(masterHost,
                     mbd_port,
                     request_buf,
                     XDR_GETPOS(&xdrs),
                     &reply_buf,
                     &hdr,
                     connTimeout,
                     readTimeout,
                     &statusChan,
                     NULL,
                     NULL,
                     flags);
    if (cc < 0) {
        statusChan = -1;

        if (!equalHost_(masterHost, lastHost)) {
            if (errno != EINTR)
                ls_syslog (LOG_DEBUG,
                           "%s: Failed to reach mbatchd on host <%s> : %s",
                           fname, masterHost, lsb_sysmsg());
            strcpy(lastHost, masterHost);
        }
        xdr_destroy(&xdrs);
        FREEUP(request_buf);
        failcnt++;
        return (-1);
    }

    failcnt = 0;
    lastHost[0] = '\0';
    xdr_destroy(&xdrs);
    FREEUP(request_buf);

    if (cc)
        free(reply_buf);

    reply = hdr.opCode;
    switch (reply) {
        case LSBE_NO_ERROR:
            return (0);
        default:
            ls_syslog(LOG_ERR, I18N(5221,
                                    "%s: Illegal reply code <%d> from mbatchd on host <%s>"), /* catgets 5221 */
                      fname,
                      reply,
                      masterHost);
            return(-1);
    }

}

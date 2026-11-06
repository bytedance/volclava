/* $Id: mbd.comm.c 397 2007-11-26 19:04:00Z mblack $
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


#include "mbd.h"
#include <malloc.h>
#include "../../lsf/lib/lsi18n.h"
#define NL_SETN         10

extern int connTimeout;
static sbdReplyType  callSBD(char *,
                             char *,
                             int,
                             char **,
                             struct LSFHeader *, int (*)(),
                             int *,
                             struct hData *,
                             char *,
                             char *,
                             int *,
                             int *,
                             int,
                             struct sbdNode *,
                             int *);

extern sbdReplyType start_ajob (struct jData *jDataPtr, struct qData *qp, struct jobReply *jobReply);

struct sbdNode sbdNodeList = {&sbdNodeList, &sbdNodeList, 0, NULL, NULL, 0};

sbdReplyType
start_job (struct jData *jDataPtr, struct qData *qp, struct jobReply *jobReply)
{
    return(start_ajob(jDataPtr, qp, jobReply));
}


sbdReplyType
start_ajob (struct jData *jDataPtr,
            struct qData *qp,
            struct jobReply *jobReply)
{
    static char        fname[] = "start_job()";
    struct jobSpecs    jobSpecs;
    char               *request_buf = NULL;
    struct LSFHeader   hdr;
    char               *reply_buf = NULL;
    XDR                xdrs;
    sbdReplyType       reply;
    int                cc;
    int                buflen;
    int                i;
    static char        lastHost[MAXHOSTNAMELEN];
    struct hData       *hostData = jDataPtr->hPtr[0];
    char               *toHost = jDataPtr->hPtr[0]->host;
    struct lenData     jf;
    struct lenData     aux_auth_data;
    static int         errcnt;
    struct sbdNode     sbdNode;
    int                socket;
    struct lsfAuth     *auth = NULL;

    if (logclass & (LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG2, "%s: job=%s", fname, lsb_jobid2str(jDataPtr->jobId));

    packJobSpecs (jDataPtr, &jobSpecs);

    if (pjobSpoolDir != NULL) {
        if (jDataPtr->jobSpoolDir == NULL) {

            strcpy(jobSpecs.jobSpoolDir, pjobSpoolDir);
            jDataPtr->jobSpoolDir = safeSave(pjobSpoolDir);

        } else if (strcmp(pjobSpoolDir, jDataPtr->jobSpoolDir) != 0) {

            strcpy(jobSpecs.jobSpoolDir, pjobSpoolDir);
            FREEUP(jDataPtr->jobSpoolDir);
            jDataPtr->jobSpoolDir = safeSave(pjobSpoolDir);
        }
    } else {
        if (jDataPtr->jobSpoolDir != NULL) {

            jobSpecs.jobSpoolDir[0] = '\0';
            FREEUP(jDataPtr->jobSpoolDir);
        }
    }


    if (readLogJobInfo(&jobSpecs, jDataPtr, &jf, &aux_auth_data) == -1) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
                  lsb_jobid2str(jDataPtr->jobId), "readLogJobInfo");
        freeJobSpecs (&jobSpecs);
        return (ERR_NO_FILE);
    }

    if (jobSpecs.options & SUB_RESTART) {

        char *tmp;
        if (strchr(jobSpecs.jobFile, '.') != strrchr(jobSpecs.jobFile, '.')) {
            tmp = strstr (jobSpecs.jobFile, lsb_jobid2str(jobSpecs.jobId));
            if (tmp != NULL) {
                --tmp;
                *tmp = '\0';
            }
        }
    }

    hdr.opCode = MBD_NEW_JOB;
    buflen = sizeof(struct jobSpecs) + sizeof(struct sbdPackage) + 100
        + jobSpecs.numToHosts * MAXHOSTNAMELEN
        + jobSpecs.thresholds.nThresholds
        *jobSpecs.thresholds.nIdx * 2 * sizeof (float)
        + jobSpecs.nxf * sizeof (struct xFile) + jobSpecs.eexec.len;
    for (i = 0; i < jobSpecs.numEnv; i++)
        buflen += strlen(jobSpecs.env[i]);
    buflen = (buflen * 4) / 4;

    request_buf = (char *) my_malloc (buflen, fname);
    xdrmem_create(&xdrs, request_buf, buflen, XDR_ENCODE);
    if (! xdr_encodeMsg(&xdrs, (char *)&jobSpecs, &hdr, xdr_jobSpecs, 0,
                        auth)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        free (request_buf);
        freeJobSpecs (&jobSpecs);
        free(jf.data);
        return (ERR_FAIL);
    }

    sbdNode.jData = jDataPtr;
    sbdNode.hData = hostData;
    sbdNode.reqCode = MBD_NEW_JOB;

    reply = callSBD(toHost, request_buf, XDR_GETPOS(&xdrs), &reply_buf, &hdr,
                    sndJobFile_, (int *) &jf, hostData, lastHost, fname,
                    &errcnt, &cc,
                    CALL_SERVER_NO_WAIT_REPLY | CALL_SERVER_NO_HANDSHAKE,
                    &sbdNode, &socket);

    xdr_destroy(&xdrs);
    free (request_buf);
    freeJobSpecs (&jobSpecs);
    free(jf.data);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return (reply);



    if (reply == ERR_NO_ERROR) {

        jobReply->jobId = jDataPtr->jobId;
        jobReply->jobPid = 0;
        jobReply->jobPGid = 0;
        jobReply->jStatus = jobSpecs.jStatus;
        jobReply->jStatus &= ~JOB_STAT_MIG;
        if (jobSpecs.options & SUB_PRE_EXEC)
            SET_STATE(jobReply->jStatus, JOB_STAT_RUN | JOB_STAT_PRE_EXEC);
        else
            SET_STATE(jobReply->jStatus, JOB_STAT_RUN);
    }

    if (reply_buf)
        free(reply_buf);

    return(reply);

}

sbdReplyType
switch_job (struct jData *jDataPtr, int options)
{
    static char        fname[] = "switch_job()";
    struct jobSpecs    jobSpecs;
    char               *request_buf = NULL;
    struct LSFHeader   hdr;
    char               *reply_buf = NULL;
    XDR                xdrs;
    sbdReplyType       reply;
    int                buflen;
    int                cc;
    static char        lastHost[MAXHOSTNAMELEN];
    static int         errcnt;
    struct hData       *HostData = jDataPtr->hPtr[0];
    char               *toHost = jDataPtr->hPtr[0]->host;
    struct sbdNode     sbdNode;
    int                socket;
    struct lsfAuth     *auth = NULL;

    if (logclass & (LC_SIGNAL | LC_EXEC))
        ls_syslog(LOG_DEBUG2, "\
%s: job=%s", fname, lsb_jobid2str(jDataPtr->jobId));

    packJobSpecs (jDataPtr, &jobSpecs);

    if (options == TRUE) {
        hdr.opCode = MBD_SWIT_JOB;
    } else {
        hdr.opCode = MBD_MODIFY_JOB;
    }
    buflen = sizeof(struct jobSpecs) + sizeof(struct sbdPackage) + 100
        + jobSpecs.numToHosts * MAXHOSTNAMELEN
        + jobSpecs.thresholds.nThresholds
        * jobSpecs.thresholds.nIdx * 2 * sizeof (float)
        + jobSpecs.nxf * sizeof (struct xFile);
    buflen = (buflen * 4) / 4;

    request_buf = (char *) my_malloc (buflen, fname);
    xdrmem_create(&xdrs, request_buf, buflen, XDR_ENCODE);
    if (! xdr_encodeMsg(&xdrs, (char *)&jobSpecs, &hdr, xdr_jobSpecs, 0,
                        auth)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        free (request_buf);
        freeJobSpecs (&jobSpecs);
        return (ERR_FAIL);
    }

    sbdNode.jData = jDataPtr;
    sbdNode.hData = HostData;
    if (options == TRUE) {
        sbdNode.reqCode = MBD_SWIT_JOB;
    } else {
        sbdNode.reqCode = MBD_MODIFY_JOB;
    }


    reply = callSBD (toHost, request_buf, XDR_GETPOS(&xdrs), &reply_buf, &hdr,
                     NULL, NULL, HostData, lastHost, fname, &errcnt,
                     &cc,
                     CALL_SERVER_NO_WAIT_REPLY | CALL_SERVER_NO_HANDSHAKE,
                     &sbdNode, &socket);
    xdr_destroy(&xdrs);
    free (request_buf);
    freeJobSpecs (&jobSpecs);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return (reply);

    if (reply != ERR_NO_ERROR) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5404,
                                         "%s: Job <%s>: Illegal reply code <%d> from host <%s>, switch job failed"), /* catgets 5404 */
                  fname,
                  lsb_jobid2str(jDataPtr->jobId),
                  reply,
                  toHost);
    }

    if (reply_buf)
        free(reply_buf);

    return(reply);

}

sbdReplyType
signal_job (struct jData *jp,
            struct jobSig *sendReq,
            struct jobReply *jobReply)
{
    static char        fname[] = "signal_job()";
    struct LSFHeader   hdr;
    char               request_buf[MSGSIZE];
    char               *reply_buf;
    XDR                xdrs;
    sbdReqType         reqCode;
    sbdReplyType       reply;
    struct jobSig      jobSig;
    static char        lastHost[MAXHOSTNAMELEN];
    static int         errcnt;
    struct hData       *hostData = jp->hPtr[0];
    char               *toHost = jp->hPtr[0]->host;
    int                cc;
    struct sbdNode     sbdNode;
    int                socket;
    struct lsfAuth     *auth = NULL;

    jobSig.jobId = jp->jobId;
    jobSig.sigValue = sig_encode(sendReq->sigValue);
    jobSig.actFlags = sendReq->actFlags;
    jobSig.chkPeriod = sendReq->chkPeriod;
    jobSig.actCmd    = sendReq->actCmd;
    jobSig.reasons   = sendReq->reasons;
    jobSig.subReasons= sendReq->subReasons;
    jobSig.role = sendReq->role;

    if (logclass & LC_SIGNAL) {
        ls_syslog(LOG_DEBUG, "\
%s: Job %s encoded sigValue %d actFlags %x",
                  fname, lsb_jobid2str(jobSig.jobId),
                  jobSig.sigValue, jobSig.actFlags);
    }

    reqCode = MBD_SIG_JOB;
    xdrmem_create(&xdrs, request_buf, MSGSIZE/2, XDR_ENCODE);
    hdr.opCode = reqCode;
    if (! xdr_encodeMsg(&xdrs, (char *)&jobSig, &hdr, xdr_jobSig, 0, auth)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        return(ERR_FAIL);
    }


    sbdNode.jData = jp;
    sbdNode.hData = hostData;
    sbdNode.reqCode = MBD_SIG_JOB;
    sbdNode.sigVal = sendReq->sigValue;
    sbdNode.sigFlags = sendReq->actFlags;

    reply = callSBD(toHost,
                    request_buf,
                    XDR_GETPOS(&xdrs),
                    &reply_buf,
                    &hdr,
                    NULL,
                    NULL,
                    hostData,
                    lastHost,
                    fname,
                    &errcnt,
                    &cc,
                    CALL_SERVER_NO_WAIT_REPLY | CALL_SERVER_NO_HANDSHAKE,
                    &sbdNode,
                    &socket);
    xdr_destroy(&xdrs);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return (reply);

    switch (reply) {
        case ERR_NO_ERROR:
            break;

        case ERR_FORK_FAIL:
        case ERR_BAD_REQ:
        case ERR_NO_JOB:
        case ERR_SIG_RETRY:
            break;

        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5407,
                                             "%s: Job <%s>: Illegal reply code <%d> from sbatchd on host <%s>"), fname, lsb_jobid2str(jp->jobId), reply, toHost); /* catgets 5407 */
            reply = ERR_BAD_REPLY;
    }

    xdr_destroy(&xdrs);

    return (reply);

}

sbdReplyType
msg_job (struct jData *jp, struct Buffer *mbuf, struct jobReply *jobReply)
{
    static char        fname[] = "msg_job()";
    struct LSFHeader   hdr;
    char               *reply_buf;
    sbdReplyType       reply;
    static char        lastHost[MAXHOSTNAMELEN];
    static int         errcnt;
    struct hData       *hostData = jp->hPtr[0];
    char               *toHost = jp->hPtr[0]->host;
    int                cc;

    if (logclass & (LC_SIGNAL | LC_EXEC))
        ls_syslog(LOG_DEBUG2, "%s: job=%s", fname, lsb_jobid2str(jp->jobId));

    reply = callSBD(toHost,
                    mbuf->data,
                    mbuf->len,
                    &reply_buf,
                    &hdr,
                    NULL,
                    NULL,
                    hostData,
                    lastHost,
                    fname,
                    &errcnt,
                    &cc,
                    0,
                    NULL,
                    NULL);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return (reply);

    switch (reply) {
        case ERR_NO_ERROR:
            break;
        case ERR_BAD_REQ:
        case ERR_NO_JOB:
            break;
        default:
            ls_syslog(LOG_ERR, "\
%s: Job %s invalid reply code %d from sbatchd on host %s",
                      __func__, lsb_jobid2str(jp->jobId),
                      reply, toHost);
            reply = ERR_BAD_REPLY;
    }

    if (reply_buf)
        free(reply_buf);
    return (reply);
}

sbdReplyType
probe_slave(struct hData *hData, char sendJobs)
{
    static char         fname[] = "probe_slave()";
    char                *request_buf;
    char                *reply_buf = NULL;
    int                 buflen = 0;
    static char         lastHost[MAXHOSTNAMELEN];
    static int          errcnt = -1;
    struct sbdPackage   sbdPackage;
    XDR                 xdrs;
    int                 i;
    int                 cc;
    sbdReplyType        reply;
    struct LSFHeader    hdr;
    char                *toHost = hData->host;
    int                 *sockPtr;
    int                 socket;
    struct LSFHeader    hdrBuf;
    struct sbdNode      sbdNode;
    struct lsfAuth      *auth = NULL;

    memset(&xdrs, 0, sizeof(XDR));

    ls_syslog(LOG_DEBUG, "\
%s: Probing %s sendJobs %d", __func__, toHost, sendJobs);

    hdr.refCode = 0;
    hdr.reserved = 0;

    if (sendJobs) {
        if ((sbdPackage.numJobs = countNumSpecs (hData)) > 0)
            sbdPackage.jobs = my_calloc (sbdPackage.numJobs,
                                         sizeof (struct jobSpecs), fname);
        else {
            sbdPackage.numJobs = 0;
            sbdPackage.jobs = NULL;
        }

        buflen = sbatchdJobs (&sbdPackage, hData);
        hdr.opCode = MBD_PROBE;
        request_buf = my_malloc (buflen, fname);

        xdrmem_create(&xdrs, request_buf, buflen, XDR_ENCODE);

        if (! xdr_encodeMsg(&xdrs,
                            (char *)&sbdPackage,
                            &hdr,
                            xdr_sbdPackage,
                            0,
                            auth)) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
            xdr_destroy(&xdrs);
            free (request_buf);
            for (i = 0; i < sbdPackage.nAdmins; i++)
                FREEUP(sbdPackage.admins[i]);
            FREEUP(sbdPackage.admins);
            for (i = 0; i < sbdPackage.numJobs; i++)
                freeJobSpecs (&sbdPackage.jobs[i]);
            if (sbdPackage.jobs)
                free (sbdPackage.jobs);
            return (ERR_FAIL);
        }
        sockPtr = &socket;
    } else {
        hdr.opCode = MBD_PROBE;
        hdr.length = 0;
        request_buf = (char *) &hdrBuf;
        xdrmem_create(&xdrs, request_buf, sizeof(hdrBuf), XDR_ENCODE);

        if (!xdr_LSFHeader(&xdrs, &hdr)) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
            xdr_destroy(&xdrs);
            return (ERR_FAIL);
        }
        sockPtr = NULL;
    }

    sbdNode.jData = NULL;
    sbdNode.hData = hData;
    sbdNode.reqCode = MBD_PROBE;

    errcnt = -10;
    reply = callSBD(toHost,
                    request_buf,
                    XDR_GETPOS(&xdrs),
                    &reply_buf,
                    &hdr,
                    NULL,
                    NULL,
                    hData,
                    lastHost,
                    fname,
                    &errcnt,
                    &cc,
                    CALL_SERVER_NO_WAIT_REPLY | CALL_SERVER_NO_HANDSHAKE,
                    &sbdNode,
                    sockPtr);
    xdr_destroy(&xdrs);

    if (sendJobs) {
        free (request_buf);
        for (i = 0; i < sbdPackage.nAdmins; i++)
            FREEUP(sbdPackage.admins[i]);
        FREEUP(sbdPackage.admins);
        for (i = 0; i < sbdPackage.numJobs; i++)
            freeJobSpecs (&sbdPackage.jobs[i]);
        if (sbdPackage.jobs)
            free (sbdPackage.jobs);
    }

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return reply;

    switch (reply) {
        case ERR_NO_ERROR:
        case ERR_NO_LIM:
            break;
        default:
            ls_syslog(LOG_ERR, "\
%s: Unknown reply code %d from sbatchd on host %s", __func__, reply, toHost);
            break;
    }

    if (reply_buf)
        free(reply_buf);

    return reply;
}

sbdReplyType
rebootSbd (char *host)
{
    static char        fname[] = "rebootSbd()";
    sbdReplyType       reply;
    char               request_buf[MSGSIZE];
    char               *reply_buf;
    XDR                xdrs;
    struct hData       *hData;
    static char        lastHost[MAXHOSTNAMELEN];
    static int         errcnt;
    struct LSFHeader   hdr;
    int                cc;
    struct lsfAuth     *auth = NULL;

    hdr.opCode = MBD_REBOOT;
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs, NULL, &hdr, NULL, 0, auth)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        return (ERR_FAIL);
    }

    if ((hData = getHostData(host)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5414,
                                         "%s: Failed to get hData of host <%s>"), /* catgets 5414 */
                  fname,
                  host);
        return (ERR_FAIL);
    }

    reply = callSBD(host,
                    request_buf,
                    XDR_GETPOS(&xdrs),
                    &reply_buf,
                    &hdr,
                    NULL,
                    NULL,
                    hData,
                    lastHost,
                    fname,
                    &errcnt,
                    &cc,
                    0,
                    NULL,
                    NULL);
    xdr_destroy(&xdrs);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return (reply);

    if (reply != ERR_NO_ERROR)
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5415,
                                         "%s: Illegal reply code <%d> from sbatchd on host <%s>"), fname, reply, host); /* catgets 5415 */

    return(reply);
}

sbdReplyType
shutdownSbd (char *host)
{
    static char        fname[] = "shutdownSbd()";
    sbdReplyType       reply;
    char               request_buf[MSGSIZE];
    char               *reply_buf;
    XDR                xdrs;
    static char        lastHost[MAXHOSTNAMELEN];
    static int         errcnt;
    struct hData       *hData;
    struct LSFHeader   hdr;
    int                cc;
    struct lsfAuth     *auth = NULL;

    hdr.opCode = MBD_SHUTDOWN;
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs, (char *)NULL, &hdr, NULL, 0, auth)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
        return (ERR_FAIL);
    }

    if ((hData = getHostData(host)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5417,
                                         "%s: Failed to get hData of host <%s>"), /* catgets 5417 */
                  fname, host);
        return (ERR_FAIL);
    }

    reply = callSBD(host,
                    request_buf,
                    XDR_GETPOS(&xdrs),
                    &reply_buf,
                    &hdr,
                    NULL,
                    NULL,
                    hData,
                    lastHost,
                    fname,
                    &errcnt,
                    &cc,
                    0,
                    NULL,
                    NULL);
    xdr_destroy(&xdrs);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return (reply);

    hStatChange (hData, HOST_STAT_UNREACH);
    if (reply != ERR_NO_ERROR)
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5418,
                                         "%s: Illegal reply code <%d> from sbatchd on host <%s>"), fname, reply, host); /* catgets 5418 */

    return(reply);

}

static sbdReplyType
callSBD(char *toHost,
        char *request_buf,
        int len,
        char **reply_buf,
        struct LSFHeader *replyHdr,
        int (*postSndFunc)(),
        int *postSndFuncArg,
        struct hData *hPtr,
        char *lastHost,
        char *caller,
        int *cnt,
        int *cc,
        int callServerFlags,
        struct sbdNode *sbdPtr,
        int *sockPtr)
{
    static char fname[] = "callSBD";
    struct sbdNode *newSbdNode;

    if (daemonParams[LSB_MBD_BLOCK_SEND].paramValue == NULL
        && strcmp(caller, "msg_job") != 0)
        callServerFlags |= CALL_SERVER_ENQUEUE_ONLY;

    *cc = call_server(toHost,
                      sbd_port,
                      request_buf,
                      len,
                      reply_buf,
                      replyHdr,
                      connTimeout, /* 100 millisec */
                      60 * 30,
                      sockPtr,
                      postSndFunc,
                      postSndFuncArg,
                      callServerFlags);
    if (*cc < 0) {
        if (*cc == -2) {
            if (lsberrno != LSBE_CONN_REFUSED
                && lsberrno != LSBE_CONN_TIMEOUT
                && !(lsberrno == LSBE_LSLIB && lserrno == LSE_TIME_OUT)) {
                return (ERR_FAIL);
            }
            if (equalHost_(toHost, lastHost)) {
                *cnt = *cnt + 1;
                if (*cnt >= 5) {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5419,
                                                     "%s: Failed to call sbatchd on host <%s>: %s"), caller, toHost, lsb_sysmsg()); /* catgets 5419 */
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5420,
                                                     "The above failure repeated %d times"), *cnt); /* catgets 5420 */
                    *cnt = 0;
                }
            } else if (*cnt >= 0) {
                strcpy(lastHost, toHost);
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5421,
                                                 "%s: Failed to call sbatchd on host <%s>: %s"), /* catgets 5421 */
                          caller,
                          toHost,
                          lsb_sysmsg());
                *cnt = 1;
            }
            if (*cnt >= 0) {
                hStatChange (hPtr, HOST_STAT_UNREACH);
            }
            return (ERR_UNREACH_SBD);
        }
        lastHost[0] = '\0';

        if (lsberrno == LSBE_NO_MEM)
            mbdDie(MASTER_MEM);

        if (*cnt >= 0)
            hStatChange (hPtr, HOST_STAT_OK);
        *cnt = 0;
        return ERR_NULL;
    }

    lastHost[0] = '\0';
    if (*cnt >= 0)
        hStatChange (hPtr, HOST_STAT_OK);
    *cnt = 0;

    if (callServerFlags & CALL_SERVER_NO_WAIT_REPLY) {
        if (sockPtr) {
            newSbdNode = my_malloc(sizeof(struct sbdNode),
                                   fname);
            memcpy(newSbdNode, sbdPtr, sizeof(struct sbdNode));
            newSbdNode->chanfd = *sockPtr;
            newSbdNode->lastTime = now;
            chanSetMode_(*sockPtr, CHAN_MODE_NONBLOCK);
            inList((struct listEntry *) &sbdNodeList,
                   (struct listEntry *) newSbdNode);
                nSbdConnections++;

        }
        return ERR_NO_ERROR;
    }

    return replyHdr->opCode;
}

sbdReplyType
callSbdDebug(struct debugReq *pdebug)
{
    static char        fname[]="callSbddebug()";
    sbdReplyType       reply;
    char               request_buf[MSGSIZE];
    char               *reply_buf;
    XDR                xdrs;
    static char        lastHost[MAXHOSTNAMELEN];
    static int         errcnt;
    struct hData       *hData;
    struct LSFHeader   hdr;
    struct debugReq    debug;
    int                cc;

    hdr.opCode = CMD_SBD_DEBUG;
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);

    debug.opCode = pdebug->opCode;
    debug.level = pdebug->level;
    debug.logClass = pdebug->logClass;
    debug.options = pdebug->options;
    strcpy(debug.logFileName, pdebug->logFileName);
    debug.hostName = NULL ;

    if (!xdr_encodeMsg(&xdrs, (char *)&debug, &hdr, xdr_debugReq, 0, NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs);
    }

    if ((hData = getHostData(pdebug->hostName)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5424,
                                         "%s: Failed to get hData of host <%s>"), /* catgets 5424 */
                  fname, pdebug->hostName);
        return (ERR_FAIL);
    }

    reply = callSBD(pdebug->hostName,
                    request_buf,
                    XDR_GETPOS(&xdrs),
                    &reply_buf,
                    &hdr,
                    NULL,
                    NULL,
                    hData,
                    lastHost,
                    fname,
                    &errcnt,
                    &cc,
                    0,
                    NULL,
                    NULL);
    xdr_destroy(&xdrs);

    if (reply == ERR_NULL || reply == ERR_FAIL || reply == ERR_UNREACH_SBD)
        return reply;

    if (reply != ERR_NO_ERROR)
        ls_syslog(LOG_ERR, "\
%s: Invalid reply code %d from sbatchd on host %s",
                  __func__, reply, pdebug->hostName);
    return reply;
}

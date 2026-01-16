/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: daemons.c 397 2007-11-26 19:04:00Z mblack $
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
#include <sys/types.h>
#include <string.h>
#include <sys/file.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include "daemons.h"

#include "../../lsf/lib/lsi18n.h"
#define NL_SETN		10

struct config_param daemonParams[] = {
    {"LSB_DEBUG", NULL},
    {"LSB_CONFDIR", NULL},
    {"LSF_SERVERDIR", NULL},
    {"LSF_LOGDIR", NULL},
    {"LSB_SHAREDIR", NULL},
    {"LSB_MAILTO", NULL},
    {"LSB_MAILPROG", NULL},
    {"LSB_SBD_PORT", NULL},
    {"LSB_MBD_PORT", NULL},
    {"LSF_ID_PORT", NULL},
    {"LSF_AUTH", NULL},
    {"LSB_CRDIR", NULL},
    {"LSF_USE_HOSTEQUIV", NULL},
    {"LSF_ROOT_REX", NULL},
    {"LSB_DEBUG_MBD", NULL},
    {"LSB_DEBUG_SBD", NULL},
    {"LSB_TIME_MBD", NULL},
    {"LSB_TIME_SBD", NULL},
    {"LSB_SIGSTOP", NULL},
    {"LSF_LOG_MASK", NULL},
    {"LSF_BINDIR", NULL},
    {"LSB_MBD_CONNTIMEOUT", NULL},
    {"LSB_SBD_CONNTIMEOUT", NULL},
    {"LSF_CONFDIR", NULL},
    {"LSB_MBD_MAILREPLAY", NULL},
    {"LSB_MBD_MIGTOPEND", NULL},
    {"LSB_SBD_READTIMEOUT", NULL},
    {"LSB_MBD_BLOCK_SEND", NULL},
    {"LSF_GETPWNAM_RETRY", NULL},
    {"LSB_MEMLIMIT_ENFORCE", NULL},
    {"LSB_BSUBI_OLD", NULL},
    {"LSB_STOP_IGNORE_IT", NULL},
    {"LSB_HJOB_PER_SESSION", NULL},
    {"LSF_AUTH_DAEMONS", NULL},
    {"LSB_REQUEUE_HOLD", NULL},
    {"LSB_SMTP_SERVER", NULL},
    {"LSB_MAILSERVER", NULL},
    {"LSB_MAILSIZE_LIMIT", NULL},
    {"LSB_REQUEUE_TO_BOTTOM", NULL},
    {"LSB_ARRAY_SCHED_ORDER", NULL},
    {"LSF_LIBDIR", NULL},
    {"LSB_QPOST_EXEC_ENFORCE", NULL},
    {"LSB_MIG2PEND", NULL},
    {"LSB_UTMP", NULL},
    {"LSB_JOB_CPULIMIT", NULL},
    {"LSB_RENICE_NEVER_AT_RESTART", NULL},
    {"LSF_MLS_LOG", NULL},
    {"LSB_JOB_MEMLIMIT", NULL},
    {"LSB_MOD_ALL_JOBS", NULL},
    {"LSB_SET_TMPDIR", NULL},
    {"LSB_PTILE_PACK", NULL},
    {"LSB_SBD_FINISH_SLEEP", NULL},
    {"LSB_VIRTUAL_SLOT", NULL},
    {"LSB_STDOUT_DIRECT", NULL},
    {"MBD_DONT_FORK", NULL},
    {"LIM_NO_MIGRANT_HOSTS", NULL},
    {"LSF_UNIT_FOR_LIMITS", NULL},
    {NULL, NULL}
};


extern int removeSpoolFile(const char*,const char*);
extern char* getSpoolHostBySpoolFile(const char *);

int
init_ServSock(u_short port)
{
    int ch;

    ch = chanServSocket_(SOCK_STREAM, ntohs(port), 1024, CHAN_OP_SOREUSE);
    if (ch < 0) {
        ls_syslog(LOG_ERR, "\
%s: chanServSocket_() failed to get socket %m", __func__);
        return -1;
    }
    if (chanEpollInit() < 0) {
        ls_syslog(LOG_ERR, "%s: chanEpollInit_() failed %m", __func__);
        return -1;
    }
    if(chanRegisterEpoll_(ch, EPOLLIN|EPOLLERR) < 0){
        ls_syslog(LOG_ERR, "%s: chanRegisterEpoll_() failed: %m",
                  __func__);
        return -1;
    }

    return ch;
}

int
rcvJobFile(int chfd, struct lenData *jf)
{
    static char fname[]="rcvJobFile";
    int cc;

    jf->data = NULL;
    jf->len = 0;

    if ((cc = chanRead_(chfd, NET_INTADDR_(&jf->len), NET_INTSIZE_)) !=
	NET_INTSIZE_) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "chanRead_");
	return -1;
    }

    jf->len = ntohl(jf->len);
    jf->data = my_malloc(jf->len, "rcvJobFile");

    if ((cc = chanRead_(chfd, jf->data, jf->len)) != jf->len) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "chanRead_");
        free(jf->data);
	return -1;
    }

    return 0;
}

int
do_readyOp(XDR *xdrs, int chanfd, struct sockaddr_in *from,
           struct LSFHeader *reqHdr )
{
    static char fname[]="do_readyOp";
    XDR xdrs2;
    struct Buffer *buf;
    struct LSFHeader replyHdr;


    if (chanAllocBuf_(&buf, sizeof(struct LSFHeader)) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "malloc");
        return(-1);
    }
    initLSFHeader_(&replyHdr);
    replyHdr.opCode = READY_FOR_OP;
    replyHdr.length = 0;

    xdrmem_create (&xdrs2, buf->data, sizeof(struct LSFHeader), XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs2, &replyHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
        xdr_destroy(&xdrs2);
        return(-1);

    }

    buf->len = XDR_GETPOS(&xdrs2);

    if (chanEnqueue_(chanfd, buf) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "chanEnqueue_");
        xdr_destroy(&xdrs2);
        return(-1);
    }
    if(chanUpdateListenEvents(chanfd, EPOLLIN|EPOLLOUT|EPOLLERR) < 0){
        ls_syslog(LOG_ERR, "%s: chanUpdateListenEvents() failed %m", __func__);
        xdr_destroy(&xdrs2);
        return(-1);
    }

    xdr_destroy(&xdrs2);
    return(0);
}


void childRemoveSpoolFile(const char* spoolFile, int options,
                          const struct passwd* pwUser)
{
    char fname[] = "childRemoveSpoolFile";
    char apiName[] = "ls_initrex";
    pid_t pid;
    char hostName[MAXHOSTNAMELEN];
    char errMsg[MAXLINELEN];
    int status;
    char * fromHost = NULL;
    const char *sp;
    char dirName[MAXFILENAMELEN];

    status = -1;

    if ((fromHost = (char *)getSpoolHostBySpoolFile(spoolFile)) != NULL) {
        strcpy( hostName, fromHost );
    } else {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8000,
	    "Unable to get spool host from the string \'%s\'") /* catgets 8000 */
            , (spoolFile ? spoolFile : "NULL"));
        goto Done;
    }

    if (pwUser == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8001,
                                         "%s: Parameter const struct passwd * pwUser is NULL") /* catgets 8001 */
            ,fname);
       goto Done;
    }


    sp = getLowestDir_(spoolFile);
    if (sp) {
        strcpy(dirName, sp);
    } else {
        strcpy(dirName, spoolFile);
    }

    sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 3000,
                                  "%s: Unable to remove spool file:\n,\'%s\'\n on host %s\n"), /* catgets 3000 */
            fname, dirName, fromHost);

    if (!(options &  FORK_REMOVE_SPOOL_FILE)) {

        if ((options &  CALL_RES_IF_NEEDED)) {
            if (ls_initrex(1, 0) < 0) {
                status = -1;
                sprintf( errMsg, _i18n_msg_get(ls_catd , NL_SETN, 3001,
                                               "%s: %s failed when trying to delete %s from %s\n"), /* catgets 3001 */
                         fname, apiName, dirName, fromHost);
                goto Error;
            }
        }

        chuser(pwUser->pw_uid);

        status = removeSpoolFile(hostName, dirName);

        chuser(batchId);

        if (status != 0) {
            goto Error;
        }
        goto Done;
    }

    switch(pid = fork()) {
        case 0:

            if (debug < 2) {
                 closeExceptFD(-1);
            }

            if ((options &  CALL_RES_IF_NEEDED)) {
                if (ls_initrex(1, 0) < 0) {
                    status = -1;
                    sprintf( errMsg, _i18n_msg_get(ls_catd , NL_SETN, 3001,
                                                   "%s: %s failed when trying to delete %s from %s\n"), /* catgets 3001 */
                             fname, apiName, dirName, fromHost );
                    goto Error;
                }
            }

            chuser(pwUser->pw_uid);
            status = 0;
            if (removeSpoolFile(hostName, dirName) == 0) {
                exit(0);
            } else {
                exit(-1);
            }
            goto Done;
            break;

        case -1:

            if (logclass & (LC_FILE)) {
                ls_syslog(LOG_ERR,I18N_FUNC_FAIL_M,fname,"fork" );
            }
            status = -1;
            sprintf( errMsg, _i18n_msg_get(ls_catd , NL_SETN, 3002,
                                           "%s: Unable to fork to remove spool file:\n,\'%s\'\n on host %s\n"), /* catgets 3002 */
                     fname, dirName ,fromHost );
            goto Error;
        default:
            status = 0;
            goto Done;
    }

Error:

    if (status == -1) {
        lsb_merr(errMsg);
    }
Done:
    return;
}

/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 *  $Id: misc.c 397 2007-11-26 19:04:00Z mblack $
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

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <pwd.h>

#include "daemonout.h"
#include "daemons.h"
#include "../../lsf/lib/lib.osal.h"
#include "../../lsf/lib/mls.h"

#include <malloc.h>
#ifndef strchr
#include <string.h>
#endif

# include <stdarg.h>

#define NL_SETN         10


#define BATCH_SLAVE_PORT        40001

static int chuserId (uid_t);

extern struct listEntry * mkListHeader (void);
extern int shutdown (int, int);

void
die (int sig)
{
    static char fname[] = "die";
    char myhost[MAXHOSTNAMELEN];

    if (debug > 1)
        fprintf(stderr, "%s: signal %d\n",
            fname,
            sig);

    if (masterme) {
        releaseElogLock();
    }

    if (gethostname(myhost, MAXHOSTNAMELEN) <0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "gethostname", myhost);
        strcpy(myhost, "localhost");
    }

    if (sig > 0 && sig < 100) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8216,
            "Daemon on host <%s> received signal <%d>; exiting"), /* catgets 8216 */
             myhost, sig);
    } else {
        switch (sig) {
        case MASTER_RESIGN:
            ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 8272, "Master daemon on host <%s> resigned; exiting")), myhost);    /* catgets 8272 */
            break;
        case MASTER_RECONFIG:
            ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 8273, "Master daemon on host <%s> exiting for reconfiguration")), myhost);  /* catgets 8273 */
            break;

        case SLAVE_MEM:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8217,
                "Slave daemon on host <%s> failed in memory allocation; fatal error - exiting"), myhost); /* catgets 8217 */
            lsb_merr1(_i18n_msg_get(ls_catd , NL_SETN, 8217,
                "Slave daemon on host <%s> failed in memory allocation; fatal error - exiting"), myhost);
            break;
        case MASTER_MEM:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8218,
                "Master daemon on host <%s> failed in memory allocation; fatal error - exiting"), myhost); /* catgets 8218 */
            break;
        case SLAVE_FATAL:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8219,
                "Slave daemon on host <%s> dying; fatal error - see above messages for reason"), myhost); /* catgets 8219 */
            break;
        case MASTER_FATAL:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8220,
                "Master daemon on host <%s> dying; fatal error - see above messages for reason"), myhost); /* catgets 8220 */
            break;
        case MASTER_CONF:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8221,
                "Master daemon on host <%s> died of bad configuration file"), myhost); /* catgets 8221 */
              break;
        case SLAVE_RESTART:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8222,
                "Slave daemon on host <%s> restarting"), myhost); /* catgets 8222 */
              break;
        case SLAVE_SHUTDOWN:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8223,
                "Slave daemon on host <%s> shutdown"), myhost); /* catgets 8223 */
              break;
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8224,
                "Daemon on host <%s> exiting; cause code <%d> unknown"), myhost, sig); /* catgets 8224 */
            break;
        }
    }

    shutdown(chanSock_(batchSock), 2);

    exit(sig);

}

int
portok (struct sockaddr_in *from)
{
    static char fname[] = "portok";
    if (from->sin_family != AF_INET) {
        ls_syslog(LOG_ERR, "%s: sin_family(%d) != AF_INET(%d)",
            fname,
            from->sin_family,
            AF_INET);
        return FALSE;
    }

    if (debug)
        return TRUE;

    if (  ntohs(from->sin_port) >= IPPORT_RESERVED
        || ntohs(from->sin_port) <  IPPORT_RESERVED/2)
        return FALSE;

    return TRUE;
}

int
get_ports (void)
{

    static char fname[] = "get_ports";
    struct servent *sv;
    if (daemonParams[LSB_MBD_PORT].paramValue != NULL)
    {
        if (!isint_(daemonParams[LSB_MBD_PORT].paramValue)
               || (mbd_port = atoi(daemonParams[LSB_MBD_PORT].paramValue)) <= 0)
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8226,
                "%s: LSB_MBD_PORT <%s> in lsf.conf must be a positive number"), /* catgets 8226 */
                fname,
                daemonParams[LSB_MBD_PORT].paramValue);
        else
            mbd_port = htons(mbd_port);
    }
    else if (debug)
        mbd_port = htons(BATCH_MASTER_PORT);
    else
    {
        sv = getservbyname(MBATCHD_SERV, "tcp");
        if (!sv) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8227,
                "%s: %s service not registered"),
                fname,
                MBATCHD_SERV);
            lsb_merr(_i18n_printf(_i18n_msg_get(ls_catd , NL_SETN, 3208,
                "%s: %s service not registered"),
                fname,
                MBATCHD_SERV));
            return(-1);
        }
        mbd_port = sv->s_port;
    }
    if (daemonParams[LSB_SBD_PORT].paramValue != NULL)
    {
        if (!isint_(daemonParams[LSB_SBD_PORT].paramValue)
                 || (sbd_port = atoi(daemonParams[LSB_SBD_PORT].paramValue)) <= 0)
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8229,
                "%s: LSB_SBD_PORT <%s> in lsf.conf must be a positive number"), /* catgets 8229 */
                fname,
                daemonParams[LSB_SBD_PORT].paramValue);
        else
            sbd_port = htons(sbd_port);
    }
    else if (debug)
        sbd_port = htons(BATCH_SLAVE_PORT);
    else
    {
        sv = getservbyname(SBATCHD_SERV, "tcp");
        if (!sv) {
            lsb_merr(_i18n_printf(_i18n_msg_get(ls_catd , NL_SETN, 3208,
                "%s: %s service not registered"),
                fname,
                SBATCHD_SERV));
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8231,
                "%s: %s service not registered"), /* catgets 8231 */
                fname,
                SBATCHD_SERV);
            return(-1);
        }
        sbd_port = sv->s_port;
    }

    return (0);
}

uid_t chuser (uid_t uid)
{
    uid_t myuid;
    int errnoSv = errno;

    if (debug)
        return(geteuid());

    if ((myuid = geteuid()) == uid)
        return (myuid);


    if (myuid != 0 && uid != 0)
        chuserId(0);
    chuserId(uid);
    errno = errnoSv;
    return (myuid);
}

static int
chuserId (uid_t uid)
{
    static char fname[] = "chuserId";

   if (lsfSetEUid(uid) < 0) {
       ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "setresuid/seteuid",
           (int)uid);
       if (lsb_CheckMode) {
           lsb_CheckError = FATAL_ERR;
           return -1;
       } else
           die(MASTER_FATAL);
   }

   if (uid == 0) {
       if(lsfSetREUid(0, 0) < 0)
       {
           ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "setresuid/setreuid",
               (int)uid);
           if (lsb_CheckMode) {
               lsb_CheckError = FATAL_ERR;
               return -1;
           } else
               if (masterme)
                   die(MASTER_FATAL);
               else
                   die(SLAVE_FATAL);
       }
   }
   return 0;
}

char *
safeSave(char *str)
{
    char *sp;
    char temp[256];

    sp = putstr_(str);
    if (!sp) {
        sprintf(temp, I18N_FUNC_FAIL, "safeSave", "malloc");
        lsb_merr(temp);
        if (masterme)
            die(MASTER_MEM);
        else
            die(SLAVE_MEM);
    }

    return sp;

}

/* my_malloc()
 */
void *
my_malloc(int len, const char *s)
{
    return(malloc(len));

}

/* my_calloc()
 */
void *
my_calloc(int nelem, int esize, const char *caller)
{
    void   *p;

    p = calloc(nelem, esize);
    if (!p) {
        ls_syslog(LOG_ERR, "\
%s: failed %m %s", __func__, (caller ? caller : "unknown"));
    }

    return p;
}

void
daemon_doinit(void)
{

    if (! daemonParams[LSB_CONFDIR].paramValue ||
        ! daemonParams[LSF_SERVERDIR].paramValue ||
        ! daemonParams[LSB_SHAREDIR].paramValue ) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8239,
            "One or more of the following parameters undefined: %s %s %s"), /* catgets 8239 */
            daemonParams[LSB_CONFDIR].paramName,
            daemonParams[LSF_SERVERDIR].paramName,
            daemonParams[LSB_SHAREDIR].paramName);
        if (masterme)
            die(MASTER_FATAL);
        else
            die(SLAVE_FATAL);
    }



    if (daemonParams[LSB_MAILTO].paramValue == NULL)
        daemonParams[LSB_MAILTO].paramValue = safeSave(DEFAULT_MAILTO);
    if (daemonParams[LSB_MAILPROG].paramValue == NULL)
        daemonParams[LSB_MAILPROG].paramValue = safeSave(DEFAULT_MAILPROG);


    if (daemonParams[LSB_CRDIR].paramValue == NULL)
        daemonParams[LSB_CRDIR].paramValue = safeSave(DEFAULT_CRDIR);

}


void
relife(char *call_func, char *call_reason)
{
    int pid;
    char *margv[6];
    int i = 0;

    ls_syslog(LOG_DEBUG, "relife: call_func <%s> call_reason %d ...", call_func, call_reason);

    pid = fork();

    if (pid < 0)
        return;

    if (pid == 0) {
        sigset_t newmask;

        for (i=0; i< NOFILE; i++)
            close(i);
        millisleep_(3000);

        margv[0] = getDaemonPath_("/sbatchd", daemonParams[LSF_SERVERDIR].paramValue);

        i = 1;
        if (debug) {
            margv[i] = my_malloc(MAXFILENAMELEN, "relife");
            sprintf(margv[i], "-%d", debug);
            i++;
        }
        if (env_dir != NULL) {
            margv[i] = "-d";
            i++;
            margv[i] = env_dir;
            i++;
        }
        margv[i] = NULL;
        sigemptyset(&newmask);
        sigprocmask(SIG_SETMASK, &newmask, NULL);
                  /* clear signal mask */


        execve(margv[0], margv, environ);
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8241,
            "Cannot re-execute sbatchd: %m")); /* catgets 8241 */
        lsb_mperr( _i18n_msg_get(ls_catd, NL_SETN, 3211,
            "sbatchd died in an accident, failed in re-execute")); /* catgets 3211 */
        exit(-1);
    }

    die(SLAVE_RESTART);
}


struct listEntry *
tmpListHeader (struct listEntry *listHeader)
{
    static struct listEntry *tmp = NULL;

    if (tmp == NULL)
        tmp = mkListHeader();


    tmp->forw = listHeader->forw;
    tmp->back = listHeader->back;
    listHeader->forw->back = tmp;
    listHeader->back->forw = tmp;
    listHeader->forw = listHeader;
    listHeader->back = listHeader;
    return tmp;

}


int
fileExist (char *file, int uid, struct hostent *hp)
{
    static char fname[] = "fileExist";
    int pid;
    int fds[2], i;
    int answer;

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socketpair");
        return TRUE;
    }

    pid = fork();
    if (pid < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
        return TRUE;
    }

    if (pid > 0) {
        close(fds[1]);
        if (b_read_fix(fds[0], (char *) &answer, sizeof (int)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "read");
            answer = TRUE;
        }
        close(fds[0]);
        return answer;
    } else {
        close(fds[0]);
        if (lsfSetUid (uid) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "setuid",
                uid);
            answer = TRUE;
            write(fds[1], (char *) &answer, sizeof (int));
            close(fds[1]);
            exit(0);
        }
        if ((i = myopen_(file, O_RDONLY, 0, hp)) < 0) {
            ls_syslog(LOG_INFO, I18N_FUNC_S_FAIL_M, "fileExist", "myopen_", file);
            answer = FALSE;
        } else {
            close (i);
            answer = TRUE;
        }
        write(fds[1], &answer, sizeof (int));
        close(fds[1]);
        exit(0);
    }

}

void
freeWeek (windows_t *week[])
{
    windows_t *wp, *wpp;
    int j;

    for (j = 0; j < 8; j++) {
        for (wp = week[j]; wp; wp = wpp) {
            wpp =  wp->nextwind;
            if (wp)
                free (wp);
        }
        week[j] = NULL;
    }

}

void
errorBack(int chan, int replyCode, struct sockaddr_in *from)
{
    static char fname[] = "errorBack";
    struct LSFHeader replyHdr;
    XDR  xdrs;
    char errBuf[MSGSIZE/8];

    xdrmem_create(&xdrs, errBuf, MSGSIZE/8, XDR_ENCODE);
    initLSFHeader_(&replyHdr);
    replyHdr.opCode = replyCode;
    io_block_(chanSock_(chan));
    if (xdr_encodeMsg (&xdrs, NULL, &replyHdr, NULL, 0, NULL)) {
        if (chanWrite_(chan, errBuf, XDR_GETPOS(&xdrs)) < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "chanWrite_",
                sockAdd2Str_(from));
    } else
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "xdr_encodeMsg",
            sockAdd2Str_(from));

    xdr_destroy(&xdrs);
    return;

}

void
scaleByFactor(int *h32, int *l32, float cpuFactor)
{
    double limit, tmp;

    if (*h32 == 0x7fffffff && *l32 == 0xffffffff)

        return;


    limit = *h32;
    limit *= (1<<16);
    limit *= (1<<16);
    limit += *l32;


    limit = limit/cpuFactor + 0.5;
    if (limit < 1.0)
        limit = 1.0;


    tmp = limit/(double)(1<<16);
    tmp = tmp/(double)(1<<16);
    *h32 = tmp;
    tmp = (double) (*h32) * (double) (1<<16);
    tmp *= (double) (1<<16);
    *l32 = limit - tmp;

    return;
}

struct tclLsInfo *
getTclLsInfo(void)
{
    static char fname[] = "getTclLsInfo";
    int resNo, i;


    if (tclLsInfo) {
        freeTclLsInfo(tclLsInfo, 0);
    }

    tclLsInfo = (struct tclLsInfo *)my_malloc(sizeof(struct tclLsInfo ), fname);
    tclLsInfo->numIndx = allLsInfo->numIndx;
    tclLsInfo->indexNames = (char **)my_malloc (allLsInfo->numIndx *
                                                sizeof (char *), fname);
    for (resNo = 0; resNo < allLsInfo->numIndx; resNo++)
       tclLsInfo->indexNames[resNo] = allLsInfo->resTable[resNo].name;

    tclLsInfo->nRes = 0;
    tclLsInfo->resName = (char **)my_malloc(allLsInfo->nRes *sizeof(char*),
                                fname);
    tclLsInfo->stringResBitMaps =
         (int *) my_malloc (GET_INTNUM(allLsInfo->nRes) * sizeof (int), fname);
    tclLsInfo->numericResBitMaps =
         (int *) my_malloc (GET_INTNUM(allLsInfo->nRes) * sizeof (int), fname);

    for (i =0; i< GET_INTNUM(allLsInfo->nRes); i++) {
        tclLsInfo->stringResBitMaps[i] = 0;
        tclLsInfo->numericResBitMaps[i] = 0;
    }
    for (resNo = 0; resNo < allLsInfo->nRes; resNo++) {

        if ((allLsInfo->resTable[resNo].flags & RESF_BUILTIN)
             || ((allLsInfo->resTable[resNo].flags & RESF_DYNAMIC)
                   && (allLsInfo->resTable[resNo].flags & RESF_GLOBAL)))
            continue;

        if (allLsInfo->resTable[resNo].valueType == LS_STRING)
            SET_BIT (tclLsInfo->nRes, tclLsInfo->stringResBitMaps);
        if (allLsInfo->resTable[resNo].valueType == LS_NUMERIC)
            SET_BIT (tclLsInfo->nRes, tclLsInfo->numericResBitMaps);
        tclLsInfo->resName[tclLsInfo->nRes++] = allLsInfo->resTable[resNo].name;
    }

    return (tclLsInfo);

}

struct resVal *
checkThresholdCond (char *resReq, int unitForLimits)
{
    static char fname[] = "checkThresholdCond";
    struct resVal *resValPtr;

    resValPtr = (struct resVal *)my_malloc (sizeof (struct resVal),
                                "checkThresholdCond");
    initResVal (resValPtr);
    if (parseResReq (resReq, resValPtr, allLsInfo, PR_SELECT, unitForLimits)
            != PARSE_OK) {
        lsbFreeResVal (&resValPtr);
        if (logclass & (LC_EXEC) && resReq)
            ls_syslog(LOG_DEBUG1, "%s: parseResReq(%s) failed",
                      fname, resReq);
        return (NULL);
    }
    return (resValPtr);

}

int *
getResMaps(int nRes, char **resource)
{
    int i, *temp, resNo;

    if (nRes < 0)
        return (NULL);

    temp = (int *) my_malloc (GET_INTNUM(allLsInfo->nRes) * sizeof (int),
                         "getResMaps");

    for (i = 0; i < GET_INTNUM(allLsInfo->nRes); i++)
        temp[i] = 0;

    for (i = 0; i < nRes; i++) {
        for (resNo = 0; resNo < tclLsInfo->nRes; resNo++)
            if (!strcmp(resource[i], tclLsInfo->resName[resNo]))
                break;
        if (resNo < allLsInfo->nRes) {
            SET_BIT(resNo, temp);
            }
    }
    return (temp);

}


int
checkResumeByLoad (LS_LONG_INT jobId, int num, struct thresholds thresholds,
      struct hostLoad *loads, int *reason, int *subreasons, int jAttrib,
      struct resVal *resumeCondVal, struct tclHostData *tclHostData)
{
    static char fname[] = "checkResumeByLoad";
    int i, j;
    int resume = TRUE;
    int lastReason = *reason;

    if (logclass & (LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG3, "%s: reason=%x, subreasons=%d, numHosts=%d", fname, *reason, *subreasons, thresholds.nThresholds);

    if (num <= 0)
        return FALSE;



    for (j = 0; j < num; j++) {
         if (loads[j].li == NULL)
             continue;

        if (((*reason & SUSP_PG_IT)
                || ((*reason & SUSP_LOAD_REASON) && (*subreasons) == PG))
            && loads[j].li[IT] < pgSuspIdleT / 60
            && thresholds.loadSched[j][PG] != INFINIT_LOAD) {
            resume = FALSE;
            *reason = SUSP_PG_IT;
            *subreasons = 0;
        }
        else if (LS_ISUNAVAIL (loads[j].status)) {
            resume = FALSE;
            *reason = SUSP_LOAD_UNAVAIL;
        }
        else if (LS_ISLOCKEDU (loads[j].status)
             && !(jAttrib & Q_ATTRIB_EXCLUSIVE)) {
            resume = FALSE;
            *reason = SUSP_HOST_LOCK;
        } else if (LS_ISLOCKEDM (loads[j].status)) {
            resume = FALSE;
            *reason = SUSP_HOST_LOCK_MASTER;
        }

        if (!resume) {
            if (logclass & (LC_SCHED | LC_EXEC))
                ls_syslog(LOG_DEBUG2, "%s: Can't resume job %s; *reason=%x",
                      fname, lsb_jobid2str(jobId), *reason);
            if (lastReason & SUSP_MBD_LOCK)
                *reason |= SUSP_MBD_LOCK;
            return FALSE;
        }



        if (resumeCondVal != NULL) {
            if (evalResReq (resumeCondVal->selectStr,
                                    &tclHostData[j], DFT_FROMTYPE) == 1) {
                resume = TRUE;
                break;
            } else {
                resume = FALSE;
                *reason = SUSP_QUE_RESUME_COND;
                if ((logclass & (LC_SCHED | LC_EXEC)) && !resume)
                    ls_syslog(LOG_DEBUG2, "%s: Can't resume job %s; reason=%x",
                          fname, lsb_jobid2str(jobId), *reason);
                if (lastReason & SUSP_MBD_LOCK)
                    *reason |= SUSP_MBD_LOCK;
                return FALSE;
            }
        }


        if (loads[j].li[R15M] > thresholds.loadSched[j][R15M]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = R15M;
        }
        else if (loads[j].li[R1M] > thresholds.loadSched[j][R1M]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = R1M;
        }
        else if (loads[j].li[R15S] > thresholds.loadSched[j][R15S]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = R15S;
        }
        else if (loads[j].li[UT] > thresholds.loadSched[j][UT]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = UT;
        }
        else if (loads[j].li[PG] > thresholds.loadSched[j][PG]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = PG;
        }
        else if (loads[j].li[IO] > thresholds.loadSched[j][IO]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = IO;
        }
        else if (loads[j].li[LS] > thresholds.loadSched[j][LS]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = LS;
        }
        else if (loads[j].li[IT] < thresholds.loadSched[j][IT]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = IT;
        }
        else if (loads[j].li[MEM] < thresholds.loadSched[j][MEM]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = MEM;
        }

        else if (loads[j].li[TMP] < thresholds.loadSched[j][TMP]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = TMP;
        }
        else if (loads[j].li[SWP] < thresholds.loadSched[j][SWP]) {
            resume = FALSE;
            *reason = SUSP_LOAD_REASON;
            *subreasons = SWP;
        }
        for (i = MEM + 1; resume &&
                          i < MIN(thresholds.nIdx, allLsInfo->numIndx);
                          i++) {
            if (loads[j].li[i] >= INFINIT_LOAD
                || loads[j].li[i] <= -INFINIT_LOAD
                || thresholds.loadSched[j][i] >= INFINIT_LOAD
                || thresholds.loadSched[j][i] <= -INFINIT_LOAD)
                continue;

            if (allLsInfo->resTable[i].orderType == INCR)  {
                if (loads[j].li[i] > thresholds.loadSched[j][i]) {
                    resume = FALSE;
                    *reason = SUSP_LOAD_REASON;
                    *subreasons = i;
                }
            } else {
                if (loads[j].li[i] < thresholds.loadSched[j][i]) {
                    resume = FALSE;
                    *reason = SUSP_LOAD_REASON;
                    *subreasons = i;
                }
            }
        }
    }
    if (lastReason & SUSP_MBD_LOCK)
        *reason |= SUSP_MBD_LOCK;


    if ((logclass & (LC_SCHED | LC_EXEC)) && !resume)
        ls_syslog(LOG_DEBUG2, "%s: Can't resume job %s; reason=%x, subreasons=%d", fname, lsb_jobid2str(jobId), *reason, *subreasons);

    return (resume);

}

void
closeExceptFD(int except_)
{
    int i;

    for (i = sysconf(_SC_OPEN_MAX) - 1; i >= 3 ; i--) {
        if (i != except_)
            close(i);
    }
}

void
freeLsfHostInfo (struct hostInfo  *hostInfo, int num)
{
    int i, j;

    if (hostInfo == NULL || num < 0)
        return;

    for (i = 0; i < num; i++) {
        if (hostInfo[i].resources != NULL) {
            for (j = 0; j < hostInfo[i].nRes; j++)
                FREEUP (hostInfo[i].resources[j]);
            FREEUP (hostInfo[i].resources);
         }
         FREEUP (hostInfo[i].hostType);
         FREEUP (hostInfo[i].hostModel);
    }

}

void
copyLsfHostInfo (struct hostInfo *to, struct hostInfo *from)
{
    int i;


    strcpy (to->hostName, from->hostName);
    to->hostType = safeSave (from->hostType);
    to->hostModel = safeSave (from->hostModel);
    to->cpuFactor = from->cpuFactor;
    to->maxCpus = from->maxCpus;
    to->maxMem = from->maxMem;
    to->maxSwap = from->maxSwap;
    to->maxTmp = from->maxTmp;
    to->nDisks = from->nDisks;
    to->nRes = from->nRes;
    if (from->nRes > 0) {
        to->resources = (char **) my_malloc (from->nRes * sizeof (char *),
                                                      "copyLsfHostInfo");
        for (i = 0; i < from->nRes; i++)
            to->resources[i] = safeSave (from->resources[i]);
    } else
        to->resources = NULL;

    to->isServer = from->isServer;
    to->rexPriority = from->rexPriority;

}

void
freeTclHostData (struct tclHostData *tclHostData)
{

   if (tclHostData == NULL)
       return;
   FREEUP (tclHostData->resPairs);
   FREEUP (tclHostData->loadIndex);

}

void
lsbFreeResVal (struct resVal **resVal)
{

   if (resVal == NULL || *resVal == NULL)
       return;
   freeResVal (*resVal);
   FREEUP (*resVal);
}

void
doDaemonHang(char *caller)
{
    char fname[] = "doDaemonHang()";
    struct timeval timeval;
    bool_t hanging = TRUE;

    while(hanging) {
        timeval.tv_sec = 20;
        timeval.tv_usec = 0;
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8271,
            "%s hanging in %s"), fname , caller); /* catgets 8271 */
        select(0, NULL, NULL, NULL, &timeval);
    }
}

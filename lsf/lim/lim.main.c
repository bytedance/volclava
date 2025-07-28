/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * Copyright (C) 2011 David Bigagli
 *
 * $Id: lim.main.c 397 2007-11-26 19:04:00Z mblack $
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

#include "lim.h"
#include "../lib/mls.h"

extern char *argvmsg_(int argc, char **argv);

int    limSock = -1;
int    limTcpSock = -1;
ushort  lim_port;
ushort  lim_tcp_port;
int probeTimeout = 2;
short  resInactivityCount = 0;

struct clusterNode *myClusterPtr;
struct hostNode *myHostPtr;
int   masterMe;
int   nClusAdmins = 0;
int   *clusAdminIds = NULL;
int   *clusAdminGids = NULL;
char  **clusAdminNames = NULL;

int    kernelPerm;

struct limLock limLock;
char   myClusterName[MAXLSFNAMELEN];
u_int  loadVecSeqNo=0;
u_int  masterAnnSeqNo=0;
int lim_debug = 0;
int lim_CheckMode = 0;
int lim_CheckError = 0;
char *env_dir = NULL;
static int alarmed;
char  ignDedicatedResource = FALSE;
int  numHostResources;
struct sharedResource **hostResources = NULL;
u_short lsfSharedCkSum = 0;

unitTypes unitForLimits = Megabytes;

pid_t pimPid = -1;
static void startPIM(int, char **);

struct config_param limParams[] =
{
    {"LSF_CONFDIR", NULL},
    {"LSF_LIM_DEBUG", NULL},
    {"LSF_SERVERDIR", NULL},
    {"LSF_BINDIR", NULL},
    {"LSF_LOGDIR", NULL},
    {"LSF_LIM_PORT", NULL},
    {"LSF_RES_PORT", NULL},
    {"LSF_DEBUG_LIM",NULL},
    {"LSF_TIME_LIM",NULL},
    {"LSF_LOG_MASK",NULL},
    {"LSF_CONF_RETRY_MAX", NULL},
    {"LSF_CONF_RETRY_INT", NULL},
    {"LSF_CROSS_UNIX_NT", NULL},
    {"LSF_LIM_IGNORE_CHECKSUM", NULL},
    {"LSF_MASTER_LIST", NULL},
    {"LSF_REJECT_NONLSFHOST", NULL},
    {"LSF_LIM_JACKUP_BUSY", NULL},
    {"LIM_RSYNC_CONFIG", NULL},
    {"LIM_COMPUTE_ONLY", NULL},
    {"LSB_SHAREDIR", NULL},
    {"LIM_NO_MIGRANT_HOSTS", NULL},
    {"LIM_NO_FORK", NULL},
    {"LSF_UNIT_FOR_LIMITS", NULL},
    {NULL, NULL},
};

extern int chanIndex;

static int initAndConfig(int, int *);
static void term_handler(int);
static void child_handler(int);
static int  processUDPMsg(void);
static void doAcceptConn(void);
static void initSignals(void);
static void periodic(int);
static struct tclLsInfo *getTclLsInfo(void);
static void printTypeModel(void);
static void initMiscLiStruct(void);
static int getClusterConfig(void);
extern struct extResInfo *getExtResourcesDef(char *);
extern char *getExtResourcesLoc(char *);
extern char *getExtResourcesVal(char *);

/* UDP message buffer.
 */
static char reqBuf[MSGSIZE];

static void
usage(void)
{
    fprintf(stderr, "\
lim: [-C] [-V] [-h] [-t] [-debug_level] [-d env_dir]\n");
}

/* LIM main()
 */
int
main(int argc, char **argv)
{
    struct timeval timer;
    struct timeval t0;
    struct timeval t1;
    int *readyChans;
    int    maxfd;
    char   *sp;
    int    showTypeModel;
    int    cc;

    kernelPerm = 0;
    saveDaemonDir_(argv[0]);
    showTypeModel = 0;

    while ((cc = getopt(argc, argv, "12CVthd:")) != EOF) {

        switch (cc) {
            case 'd':
                env_dir = optarg;
                break;
            case '1':
                lim_debug = 1;
                break;
            case '2':
                lim_debug = 2;
                break;
            case 'C':
                putEnv("RECONFIG_CHECK","YES");
                fputs("\n", stderr);
                fputs(_LS_VERSION_, stdout);
                lim_CheckMode = 1;
                lim_debug = 2;
                break;
            case 'V':
                fputs(_LS_VERSION_, stdout);
                return 0;
            case 't':
                showTypeModel = 1;
                break;
            case 'h':
            case '?':
            default:
                usage();
                return -1;
        }
    }

    if (env_dir == NULL) {
        if ((env_dir = getenv("LSF_ENVDIR")) == NULL) {
            env_dir = LSETCDIR;
        }
    }

    if (lim_debug > 1)
        fprintf(stderr, "\
Reading configuration from %s/lsf.conf\n", env_dir);

    if (initenv_(limParams, env_dir) < 0) {

        sp = getenv("LSF_LOGDIR");
        if (sp != NULL)
            limParams[LSF_LOGDIR].paramValue = sp;
        ls_openlog("lim",
                   limParams[LSF_LOGDIR].paramValue,
                   (lim_debug == 2),
                   limParams[LSF_LOG_MASK].paramValue);
        ls_syslog(LOG_ERR, "\
%s: initenv() failed reading lsf.conf from %s", __func__, env_dir);
        lim_Exit("main");
    }

    if (showTypeModel) {
        /* Print my type, model, architecture
         * and CPU factor, even if some are hardcoded.
         */
        chanEpollStart();
        cc = initAndConfig(lim_CheckMode, &kernelPerm);
        if (cc < 0) {
            ls_syslog(LOG_ERR, "\
%s: failed to configure, exiting...", __func__);
            return -1;
        }
        printTypeModel();
        return 0;
    }

    if (!lim_debug && limParams[LSF_LIM_DEBUG].paramValue) {
        lim_debug = atoi(limParams[LSF_LIM_DEBUG].paramValue);
        if (lim_debug <= 0)
            lim_debug = 1;
    }

    if (limParams[LSF_UNIT_FOR_LIMITS].paramValue != NULL) {
        strToUpper_(limParams[LSF_UNIT_FOR_LIMITS].paramValue);
        unitForLimits = setUnitForLimits(limParams[LSF_UNIT_FOR_LIMITS].paramValue);
    }

    if (getuid() != 0) {
        fprintf(stderr, "\
%s: Real uid is %d, not root\n", __func__, (int)getuid());
    }

    if (geteuid() != 0) {
        fprintf(stderr, "\
%s: Effective uid is %d, not root\n", __func__, (int)geteuid());
    }

    /* If started with -2 in debug mode
     * do not daemonize.
     */
    maxfd = sysconf(_SC_OPEN_MAX);
    if (lim_debug != 2) {

        for (cc = maxfd; cc >= 0; cc--)
            close(cc);

        daemonize_();
        nice(NICE_LEAST);
    }
    chanEpollStart();

    if (lim_debug < 2)
        chdir("/tmp");

    getLogClass_(limParams[LSF_DEBUG_LIM].paramValue,
                 limParams[LSF_TIME_LIM].paramValue);

    if (lim_debug > 1) {
        ls_openlog("lim",
                   limParams[LSF_LOGDIR].paramValue,
                   TRUE,
                   "LOG_DEBUG");
    } else {
        ls_openlog("lim",
                   limParams[LSF_LOGDIR].paramValue,
                   FALSE,
                   limParams[LSF_LOG_MASK].paramValue);
    }

    ls_syslog(LOG_NOTICE, argvmsg_(argc, argv));

    cc = initAndConfig(lim_CheckMode, &kernelPerm);
    if (cc < 0) {
        ls_syslog(LOG_ERR, "\
%s: failed to configure, exiting...", __func__);
        return -1;
    }

    if (showTypeModel) {
        printTypeModel();
        return 0;
    }

    masterMe = (myHostPtr->hostNo == 0);
    myHostPtr->hostInactivityCount = 0;

    if (lim_CheckMode) {

        if (lim_CheckError == EXIT_WARNING_ERROR) {
            ls_syslog(LOG_WARNING, "\
%s: Checking Done. Warning(s)/error(s) found.", __func__);
            exit(EXIT_WARNING_ERROR);
        }

        ls_syslog(LOG_INFO, "%s: Checking Done.", __func__);
        return 0;
    }

    initMiscLiStruct();
    readLoad(kernelPerm);
    initSignals();

    ls_syslog(LOG_INFO, "\
%s: Daemon running (%d %d %d)", __func__, myClusterPtr->checkSum,
              ntohs(myHostPtr->statInfo.portno), VOLCLAVA_VERSION);
    ls_syslog(LOG_DEBUG, "\
%s: sampleIntvl %f exchIntvl %f hostInactivityLimit %d masterInactivityLimit %d retryLimit %d", __func__, sampleIntvl, exchIntvl,
              hostInactivityLimit, masterInactivityLimit, retryLimit);

    /* Initialize and load events.
     */
    logInit();

    if (masterMe)
        initNewMaster();

    if (lim_debug < 2)
        chdir("/tmp");

    /* We use seconds based precision timer
     * which is good enough, just make sure
     * that every 5 seconds we read the load
     * and do mastership operations.
     */
    gettimeofday(&t0, NULL);
    timer.tv_sec = 5;
    timer.tv_usec = 0;

    for (;;) {
        sigset_t oldMask;
        sigset_t newMask;
        int nReady = 0;

        if (pimPid == -1)
            startPIM(argc, argv);

        ls_syslog(LOG_DEBUG2, "\
%s: Before select: timer %dsec", __func__, timer.tv_sec);

        nReady = chanEpoll_(&readyChans, &timer);
        if (nReady < 0) {
            if (errno != EINTR)
                ls_syslog(LOG_ERR, "\
%s: chanEpoll() failed %M", __func__);
            continue;
        }

        /* Check if timer expired, if not
         * reload it with the time till
         * its expiration.
         */
        gettimeofday(&t1, NULL);
        if (t1.tv_sec - t0.tv_sec >= 5) {
            /* set the new timer
             */
            timer.tv_sec = 5;
            timer.tv_usec = 0;
            /* reset the start time
             */
            t0.tv_sec = t1.tv_sec;
            t0.tv_usec = t1.tv_sec;
            alarmed = 1;
        } else {
            timer.tv_sec = 5 - (t1.tv_sec - t0.tv_sec);
            timer.tv_usec = 0;
            alarmed = 0;
        }

        ls_syslog(LOG_DEBUG2,"\
%s: After select: cc %d alarmed %d timer %dsec",
                  __func__, cc, alarmed, timer.tv_sec);

        blockSigs_(0, &newMask, &oldMask);

        if (alarmed) {
            periodic(kernelPerm);
            sigprocmask(SIG_SETMASK, &oldMask, NULL);
        }

        if (nReady <= 0) {
            sigprocmask(SIG_SETMASK, &oldMask, NULL);
            continue;
        }

        if (chanEventsReady(limSock, EPOLLIN)) {
            processUDPMsg();
        }

        if (chanEventsReady(limTcpSock, EPOLLIN)) {
            doAcceptConn();
        }

        clientIO(readyChans, nReady);

        sigprocmask(SIG_SETMASK, &oldMask, NULL);

    } /* for (;;) */

} /* main() */

/* processUDPMsg()
 */
static int
processUDPMsg(void)
{
    struct hostNode *fromHost;
    struct hostent *hp;
    struct LSFHeader reqHdr;
    struct sockaddr_in from;
    enum limReqCode limReqCode;
    int cc;
    XDR xdrs;

    memset(&from, 0, sizeof(from));

    cc = chanRcvDgram_(limSock, reqBuf, MSGSIZE, &from, -1);
    if (cc < 0) {
        ls_syslog(LOG_ERR, "\
%s: chanRcvDgram() failed limSock %d: %m",
                  __func__, limSock);
        return -1;
    }

    xdrmem_create(&xdrs, reqBuf, MSGSIZE, XDR_DECODE);
    cc = XDR_GETPOS(&xdrs);

    if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
        ls_syslog(LOG_ERR, "\
%s: failed to decode xdr_LSFHeader %M", __func__);
        xdr_destroy(&xdrs);
        return -1;
    }

    cc = XDR_GETPOS(&xdrs);
    limReqCode = reqHdr.opCode;
    limReqCode &= 0xFFFF;

    fromHost = findHostbyAddr(&from, "main");
    if (fromHost == NULL) {
        /* Reject hosts that we don't know
         * about if we don't accept hosts
         * at runtime.
         */
        if (limParams[LIM_NO_MIGRANT_HOSTS].paramValue) {

            ls_syslog(LOG_WARNING,"\
%s: Received request %d from non-LSF host %s",
                      __func__, limReqCode, sockAdd2Str_(&from));
            /* tell the remote that we don't know him.
             */
            errorBack(&from, &reqHdr, LIME_NAUTH_HOST, -1);
            xdr_destroy(&xdrs);
            return -1;
        }

        /* If we can accept a host at runtime
         * we must however be able to resolve
         * its address.
         */
        hp = Gethostbyaddr_(&from.sin_addr.s_addr,
                            sizeof(in_addr_t),
                            AF_INET);
        if (hp == NULL) {
            ls_syslog(LOG_WARNING, "\
%s: Received request %d from unresolvable address %s", __func__,
                      limReqCode, sockAdd2Str_(&from));
            errorBack(&from, &reqHdr, LIME_NAUTH_HOST, -1);
            xdr_destroy(&xdrs);
            return -1;
        }
    }

    ls_syslog(LOG_DEBUG, "\
%s: Received request %d from host %s %s",
              __func__, limReqCode,
              (fromHost ? fromHost->hostName : hp->h_name),
              sockAdd2Str_(&from));

    switch (limReqCode) {

        case LIM_PLACEMENT:
            placeReq(&xdrs, &from, &reqHdr, -1);
            break;
        case LIM_LOAD_REQ:
            loadReq(&xdrs, &from, &reqHdr, -1);
            break;
        case LIM_GET_CLUSNAME:
            clusNameReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_GET_MASTINFO:
        case LIM_GET_MASTINFO2:
            masterInfoReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_GET_CLUSINFO:
            clusInfoReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_PING:
            pingReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_GET_HOSTINFO:
            hostInfoReq(&xdrs, fromHost, &from, &reqHdr, -1);
            break;
        case LIM_GET_INFO:
            infoReq(&xdrs, &from, &reqHdr, -1);
            break;
        case LIM_GET_CPUF:
            cpufReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_CHK_RESREQ:
            chkResReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_GET_RESOUINFO:
            resourceInfoReq(&xdrs, &from, &reqHdr, -1);
            break;
        case LIM_REBOOT:
            reconfigReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_SHUTDOWN:
            shutdownReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_LOCK_HOST:
            lockReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_DEBUGREQ:
            limDebugReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_SERV_AVAIL:
            servAvailReq(&xdrs, fromHost, &from, &reqHdr);
            break;
        case LIM_LOAD_UPD:
            rcvLoad(&xdrs, &from, &reqHdr);
            break;
        case LIM_JOB_XFER:
            jobxferReq(&xdrs, &from, &reqHdr);
            break;
        case LIM_MASTER_ANN:
            masterRegister(&xdrs, &from, &reqHdr);
            break;
        case LIM_CONF_INFO:
            rcvConfInfo(&xdrs, &from, &reqHdr);
            break;
        default:
            if (reqHdr.version <= VOLCLAVA_VERSION) {
                static int lastcode;

                errorBack(&from, &reqHdr, LIME_BAD_REQ_CODE, -1);
                if (limReqCode != lastcode)
                    ls_syslog(LOG_ERR, "\
%s: Unknown request code %d vers %d from %s", __func__,
                              limReqCode, reqHdr.version,
                              sockAdd2Str_(&from));
                lastcode = limReqCode;
                break;
            }
    }

    xdr_destroy(&xdrs);
    return 0;
}

static void
doAcceptConn(void)
{
    int  ch;
    struct sockaddr_in from;
    struct hostNode *fromHost;
    struct clientNode *client;

    if (logclass & (LC_TRACE | LC_COMM))
        ls_syslog(LOG_DEBUG, "\
%s: Entering this routine...", __func__);

    ch = chanAccept_(limTcpSock, &from);
    if (ch < 0) {
        ls_syslog(LOG_ERR, "\
%s: failed accept() new connection socket %d: %M", __func__, limTcpSock);
        return;
    }

    fromHost = findHostbyAddr(&from, "doAcceptConn()");
    if (fromHost == NULL
        && limParams[LIM_NO_MIGRANT_HOSTS].paramValue) {
        /* A migrant host is asking the master for
         * a tcp operation it should be its registation.
         */
        ls_syslog(LOG_WARNING,"\
%s: Received request from non-LSF host %s",
                          __func__, sockAdd2Str_(&from));
        return;
    }

    client = calloc(1, sizeof(struct clientNode));
    if (!client) {
        ls_syslog(LOG_ERR, "\
%s: calloc() failed: %M connection from %s dropped",
                  __func__,
                  sockAdd2Str_(&from));
        chanClose_(ch);
        return;
    }

    client->chanfd = ch;
    clientMap[client->chanfd] = client;
    client->inprogress = FALSE;
    client->fromHost   = fromHost;
    client->from       = from;
    client->clientMasks = 0;
    client->reqbuf = NULL;

}

static void
initSignals(void)
{
    sigset_t mask;

    Signal_(SIGHUP, (SIGFUNCTYPE) term_handler);
    Signal_(SIGINT, (SIGFUNCTYPE) term_handler);
    Signal_(SIGTERM, (SIGFUNCTYPE) term_handler);

#ifdef SIGXCPU
    Signal_(SIGXCPU, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGXFSZ
    Signal_(SIGXFSZ, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGPROF
    Signal_(SIGPROF, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGLOST
    Signal_(SIGLOST, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGPWR
    Signal_(SIGPWR, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGDANGER
    Signal_(SIGDANGER, SIG_IGN);
#endif

    Signal_(SIGUSR1, (SIGFUNCTYPE) term_handler);
    Signal_(SIGUSR2, (SIGFUNCTYPE) term_handler);
    Signal_(SIGCHLD, (SIGFUNCTYPE) child_handler);
    Signal_(SIGPIPE, SIG_IGN);
    Signal_(SIGALRM, SIG_IGN);

    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);

}

static int
initAndConfig(int checkMode, int *kernelPerm)
{
    int i;
    int cc;
    struct tclLsInfo *tclLsInfo;

    ls_syslog(LOG_DEBUG, "\
%s: Entering this routine...; checkMode=%d", __func__, checkMode);

    /* LIM is running in a non shared fiel system mode,
     * contact the master and retrieve the shared file
     * and the cluster file.
     */
    if (limParams[LIM_RSYNC_CONFIG].paramValue) {
        cc = getClusterConfig();
        if (cc < 0) {
            ls_syslog(LOG_ERR, "\
%s: failed getting cluster configuration files %M, exiting...", __func__);
            return -1;
        }
    }

    initLiStruct();
    if (readShared() < 0)
        lim_Exit("initAndConfig");

    reCheckRes();

    setMyClusterName();

    if (getenv("RECONFIG_CHECK") == NULL)
        if (initSock(checkMode) < 0)
            lim_Exit("initSock");

    if (readCluster(checkMode) < 0)
        lim_Exit("readCluster");

    if (reCheckClass() < 0)
        lim_Exit("readCluster");

    if ((tclLsInfo = getTclLsInfo()) == NULL)
        lim_Exit("getTclLsInfo");

    if (initTcl(tclLsInfo) < 0)
        lim_Exit("initTcl");
    initParse(&allInfo);

    initReadLoad(checkMode, kernelPerm);
    initTypeModel(myHostPtr);

    if (! checkMode) {
        initConfInfo();
        satIndex();
        loadIndex();
    }

    if (chanInit_() < 0)
        lim_Exit("chanInit_");

    for(i = 0; i < MAXCLIENTS; i++)
        clientMap[i] = NULL;

    {
        char *lsfLimLock;
        int     flag = -1;
        time_t  lockTime =-1;

        if ((lsfLimLock = getenv("LSF_LIM_LOCK")) != NULL
            && lsfLimLock[0] != 0) {

            if (logclass & LC_TRACE) {
                ls_syslog(LOG_DEBUG2, "\
%s: LSF_LIM_LOCK %s", __func__, lsfLimLock);
            }
            sscanf(lsfLimLock, "%d %ld", &flag, &lockTime);
            if (flag > 0) {

                limLock.on = flag;
                limLock.time = lockTime;
                if (LOCK_BY_USER(limLock.on)) {
                    myHostPtr->status[0] |= LIM_LOCKEDU;
                }
                if (LOCK_BY_MASTER(limLock.on)) {
                    myHostPtr->status[0] |= LIM_LOCKEDM;
                }
            }
        } else {
            limLock.on = FALSE;
            limLock.time = 0;

            myHostPtr->status[0] &= ~LIM_LOCKEDU;
        }
    }

    getLastActiveTime();

    return 0;
}

static void
periodic(int kernelPerm)
{
    static time_t ckWtime = 0;
    time_t now = time(0);

    ls_syslog(LOG_DEBUG, "%s: Entering this routine...", __func__);

    TIMEIT(0, readLoad(kernelPerm), "readLoad()");

    if (masterMe)
        announceMaster(myClusterPtr, 1, FALSE);

    if (ckWtime == 0) {
        ckWtime = now;
    }

    if (now - ckWtime > 60) {
        checkHostWd();
        ckWtime = now;
    }

    alarmed = 0;
}

/* term_handler()
 */
static void
term_handler(int signum)
{

    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", __func__);

    Signal_(signum, SIG_DFL);

    ls_syslog(LOG_ERR, "\
%s: Received signal %d, exiting", __func__, signum);
    chanClose_(limSock);
    chanClose_(limTcpSock);

    if (elim_pid > 0) {
        kill(elim_pid, SIGTERM);
        millisleep_(2000);
    }

    logLIMDown();

    exit(0);

} /* term_handler() */

/* child_handler()
 */
static void
child_handler(int sig)
{
    int           pid;
    LS_WAIT_T     status;

    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", __func__);

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == elim_pid) {
            ls_syslog(LOG_ERR, "\
%s: elim (pid=%d) died (exit_code=%d,exit_sig=%d)",
                      __func__,
                      (int)elim_pid,
                      WEXITSTATUS (status),
                      WIFSIGNALED (status) ? WTERMSIG (status) : 0);
            elim_pid = -1;
        }
        if (pid == pimPid) {
            if (logclass & LC_PIM)
                ls_syslog(LOG_DEBUG, "\
child_handler: pim (pid=%d) died", pid);
            pimPid = -1;
        }
    }

} /* child_handler() */

int
initSock(int checkMode)
{
    struct sockaddr_in   limTcpSockId;
    socklen_t            size;

    if (limParams[LSF_LIM_PORT].paramValue == NULL) {
        ls_syslog(LOG_ERR, "\
%s: fatal error LSF_LIM_PORT is not defined in lsf.conf", __func__);
        return -1;
    }

    if ((lim_port = atoi(limParams[LSF_LIM_PORT].paramValue)) <= 0) {
        ls_syslog(LOG_ERR, "\
%s: LSF_LIM_PORT <%s> must be a positive number",
                  __func__, limParams[LSF_LIM_PORT].paramValue);
        return -1;
    }

    limSock = chanServSocket_(SOCK_DGRAM, lim_port, -1,  0);
    if (limSock < 0) {
        ls_syslog(LOG_ERR, "\
%s: unable to create datagram socket port %d; another LIM running?: %M ",
                  __func__, lim_port);
        return -1;
    }

    lim_port = htons(lim_port);

    limTcpSock = chanServSocket_(SOCK_STREAM, 0, 10, 0);
    if (limTcpSock < 0) {
        ls_syslog(LOG_ERR, "%s: chanServSocket_() failed %M", __func__);
        return -1;
    }

    size = sizeof(limTcpSockId);
    if (getsockname(chanSock_(limTcpSock),
                    (struct sockaddr *)&limTcpSockId,
                    &size) < 0) {

        ls_syslog(LOG_ERR, "\
%s: getsockname(%d) failed %M", __func__, limTcpSock);
        return -1;
    }

    lim_tcp_port = limTcpSockId.sin_port;

    return 0;
}

void
errorBack(struct sockaddr_in *from,
          struct LSFHeader *reqHdr,
          enum limReplyCode replyCode, int chan)
{
    char buf[MSGSIZE/4];
    struct LSFHeader replyHdr;
    XDR  xdrs2;
    int cc;

    initLSFHeader_(&replyHdr);
    replyHdr.opCode  = (short) replyCode;
    replyHdr.refCode = reqHdr->refCode;
    replyHdr.length = 0;
    xdrmem_create(&xdrs2, buf, MSGSIZE/4, XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs2, &replyHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, __func__, "xdr_LSFHeader");
        xdr_destroy(&xdrs2);
        return;
    }

    if (chan < 0)
        cc = chanSendDgram_(limSock, buf, XDR_GETPOS(&xdrs2), from);
    else
        cc = chanWrite_(chan, buf, XDR_GETPOS(&xdrs2));

    if (cc < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, __func__,
                  "chanSendDgram_/chanWrite_",
                  limSock);

    xdr_destroy(&xdrs2);
    return;
}
static struct tclLsInfo *
getTclLsInfo(void)
{
    static struct tclLsInfo *tclLsInfo;
    int i;

    if ((tclLsInfo = (struct tclLsInfo *) malloc (sizeof (struct tclLsInfo )))
        == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, __func__, "malloc");
        return NULL;
    }

    if ((tclLsInfo->indexNames = (char **)malloc (allInfo.numIndx *
                                                  sizeof (char *))) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, __func__, "malloc");
        return NULL;
    }
    for (i=0; i < allInfo.numIndx; i++) {
        tclLsInfo->indexNames[i] = allInfo.resTable[i].name;
    }
    tclLsInfo->numIndx = allInfo.numIndx;
    tclLsInfo->nRes = shortInfo.nRes;
    tclLsInfo->resName = shortInfo.resName;
    tclLsInfo->stringResBitMaps = shortInfo.stringResBitMaps;
    tclLsInfo->numericResBitMaps = shortInfo.numericResBitMaps;

    return (tclLsInfo);

}


static void
startPIM(int argc, char **argv)
{
    char *pargv[16];
    int i;
    static time_t lastTime = 0;

    if (time(NULL) - lastTime < 60*2)
        return;

    lastTime = time(NULL);

    if ((pimPid = fork())) {
        if (pimPid < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, __func__, "fork");
        return;
    }

    alarm(0);

    if (lim_debug > 1) {

        for(i = 3; i < sysconf(_SC_OPEN_MAX); i++)
            close(i);

    } else {
        for(i = 0; i < sysconf(_SC_OPEN_MAX); i++)
            close(i);
    }

    for (i = 1; i < NSIG; i++)
        Signal_(i, SIG_DFL);


    for (i = 1; i < argc; i++)
        pargv[i] = argv[i];

    pargv[i] = NULL;
    pargv[0] = getDaemonPath_("/pim", limParams[LSF_SERVERDIR].paramValue);
    lsfExecv(pargv[0], pargv);

    ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, __func__, "execv", pargv[0]);

    exit(-1);
}

void
initLiStruct(void)
{
    if (!li) {
        li_len = 16;
        li = calloc(li_len, sizeof(struct liStruct));
    }

    li[0].name = "R15S";
    li[0].increasing = 1;
    li[0].delta[0] = 0.30;
    li[0].delta[1] = 0.10;
    li[0].extraload[0] = 0.20;
    li[0].extraload[1] = 0.40;
    li[0].valuesent = 0.0;
    li[0].exchthreshold = 0.25;
    li[0].sigdiff = 0.10;

    li[1].name="R1M";
    li[1].increasing=1;
    li[1].delta[0]=0.15;
    li[1].delta[1]=0.10;
    li[1].extraload[0]=0.20;
    li[1].extraload[1]=0.40;
    li[1].valuesent=0.0;
    li[1].exchthreshold=0.25;
    li[1].sigdiff=0.10;

    li[2].name="R15M";
    li[2].increasing=1;
    li[2].delta[0]=0.15;
    li[2].delta[1]=0.10;
    li[2].extraload[0]=0.20;
    li[2].extraload[1]=0.40;
    li[2].valuesent=0.0;
    li[2].exchthreshold=0.25;
    li[2].sigdiff=0.10;

    li[3].name="UT";
    li[3].increasing=1;
    li[3].delta[0]=1.00;
    li[3].delta[1]=1.00;
    li[3].extraload[0]=0.10;
    li[3].extraload[1]=0.20;
    li[3].valuesent=0.0;
    li[3].exchthreshold=0.15;
    li[3].sigdiff=0.10;

    li[4].name="PG";
    li[4].increasing=1;
    li[4].delta[0]=2.5;
    li[4].delta[1]=1.5;
    li[4].extraload[0]=0.8;
    li[4].extraload[1]=1.5;
    li[4].valuesent=0.0;
    li[4].exchthreshold=1.0;
    li[4].sigdiff=5.0;

    li[5].name="IO";
    li[5].increasing=1;
    li[5].delta[0]=80;
    li[5].delta[1]=40;
    li[5].extraload[0]=15;
    li[5].extraload[1]=25.0;
    li[5].valuesent=0.0;
    li[5].exchthreshold=25.0;
    li[5].sigdiff=5.0;

    li[6].name="LS";
    li[6].increasing=1;
    li[6].delta[0]=3;
    li[6].delta[1]=3;
    li[6].extraload[0]=0;
    li[6].extraload[1]=0;
    li[6].valuesent=0.0;
    li[6].exchthreshold=0.0;
    li[6].sigdiff=1.0;

    li[7].name="IT";
    li[7].increasing=0;
    li[7].delta[0]=6000;
    li[7].delta[1]=6000;
    li[7].extraload[0]=0;
    li[7].extraload[1]=0;
    li[7].valuesent=0.0;
    li[7].exchthreshold=1.0;
    li[7].sigdiff=5.0;

    li[8].name="TMP";
    li[8].increasing=0;
    li[8].delta[0]=2;
    li[8].delta[1]=2;
    li[8].extraload[0]=-0.2;
    li[8].extraload[1]=-0.5;
    li[8].valuesent=0.0;
    li[8].exchthreshold=1.0;
    li[8].sigdiff=2.0;

    li[9].name="SMP";
    li[9].increasing=0;
    li[9].delta[0]=10;
    li[9].delta[1]=10;
    li[9].extraload[0]=-0.5;
    li[9].extraload[1]=-1.5;
    li[9].valuesent=0.0;
    li[9].exchthreshold=1.0;
    li[9].sigdiff=2.0;

    li[10].name="MEM";
    li[10].increasing=0;
    li[10].delta[0]=9000;
    li[10].delta[1]=9000;
    li[10].extraload[0]=-0.5;
    li[10].extraload[1]=-1.0;
    li[10].valuesent=0.0;
    li[10].exchthreshold=1.0;
    li[10].sigdiff=3.0;
}

static void
printTypeModel(void)
{
    printf("Host Type             : %s\n", getHostType());
    printf("Host Architecture     : %s\n", getHostModel());
    printf("Matched Type          : %s\n",
           allInfo.hostTypes[myHostPtr->hTypeNo]);
    printf("Matched Architecture  : %s\n",
           allInfo.hostArchs[myHostPtr->hModelNo]);
    printf("Matched Model         : %s\n",
           allInfo.hostModels[myHostPtr->hModelNo]);
    printf("CPU Factor            : %.1f\n",
           allInfo.cpuFactor[myHostPtr->hModelNo]);

    if (myHostPtr->hTypeNo == 1 || myHostPtr->hModelNo == 1) {
        printf("When automatic detection of host type or model fails, the type or\n");
        printf("model is set to DEFAULT. LSF will still work on the host. A DEFAULT\n");
        printf("model may be inefficient because of incorrect CPU factor. A DEFAULT\n");
        printf("type may cause binary incompatibility - a job from a DEFAULT host \n");
        printf("type can be dispatched or migrated to another DEFAULT host type.\n\n");
        printf("User can use lim -t to detect the real model or type for a host. \n");
        printf("Change a DEFAULT host model by adding a new model in HostModel in\n");
        printf("lsf.shared.  Change a DEFAULT host type by adding a new type in \n");
        printf("HostType in lsf.shared.\n\n");
    }
}

/* initMiscLiStruct()
 */
static void
initMiscLiStruct(void)
{
    int i;

    extraload = calloc(allInfo.numIndx, sizeof(float));

    li = realloc(li, sizeof(struct liStruct) * allInfo.numIndx);

    for (i = NBUILTINDEX; i < allInfo.numIndx; i++) {
        li[i].delta[0] = 9000;
        li[i].delta[1] = 9000;
        li[i].extraload[0] = 0;
        li[i].extraload[1] = 0;
        li[i].valuesent = 0.0;
        li[i].exchthreshold = 0.0001;
        li[i].sigdiff = 0.0001;
    }

} /* initMiscLiStruct() */

/* getClusterConfig()
 */
static int
getClusterConfig(void)
{
    static char buf[PATH_MAX];
    struct stat stat2;
    int cc;
    FILE *fp;

    if (! limParams[LIM_RSYNC_CONFIG].paramValue)
        return 0;

    ls_syslog(LOG_DEBUG, "\
%s: volclava non shared fs configured", __func__);

    sprintf(buf, "%s/esync", limParams[LSF_BINDIR].paramValue);

    cc = stat(buf, &stat2);
    if (cc != 0) {
        /* If site does not have their esync let's
         * build our own?
         */
        ls_syslog(LOG_ERR, "\
%s: stat(%s) failed %m", __func__, buf);
        return -1;
    }

    fp = popen(buf, "r");
    if (fp == NULL) {
        ls_syslog(LOG_ERR, "\
%s: popen(%s) failed %m", __func__, buf);
    }

    memset(buf, 0, sizeof(buf));
    while (fgets(buf, sizeof(buf) - 1, fp)) {
        buf[strlen(buf) - 1] = 0;
        ls_syslog(LOG_INFO, "%s: %s", __func__, buf);
    }

    pclose(fp);

    ls_syslog(LOG_INFO, "\
%s: configuration files sync done", __func__);

    return 0;

} /* getClusterConfig() */

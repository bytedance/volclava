/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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

#include "mbd.h"
#include "mbd.fairshare.h"

#define MBD_THREAD_MIN_STACKSIZE  512
#define POLL_INTERVAL MAX(msleeptime/10, 1)
char errbuf[MAXLINELEN];

int debug = 0;
int lsb_CheckMode = 0;
int lsb_CheckError = 0;
int batchSock;
#define MAX_THRNUM     3000

time_t      lastForkTime;
int         statusChanged = 0;

int nextJobId = 1;
char masterme = TRUE;
ushort sbd_port;
ushort mbd_port;
int connTimeout;
int glMigToPendFlag = FALSE;

int requeueToBottom = FALSE;
int arraySchedOrder = FALSE;

uid_t    *managerIds  = NULL;
char   **lsbManagers = NULL;
int    nManagers     = 0;

char   *lsbManager  = NULL;
char   *lsbSys      = "SYS";
int    managerId    = 0;
uid_t  batchId      = 0;
int    jobTerminateInterval = DEF_JTERMINATE_INTERVAL;
int    msleeptime   = DEF_MSLEEPTIME;
int    subTryInterval   = DEF_SUB_TRY_INTERVAL;
int    maxPendJobs   = INFINIT_INT;
int    maxPendSlots   = INFINIT_INT;
int    defaultLimitIgnoreUserGroup = FALSE;
int    sbdSleepTime = DEF_SSLEEPTIME;
int    preemPeriod  = DEF_PREEM_PERIOD;
int    pgSuspIdleT  = DEF_PG_SUSP_IT;
int    rusageUpdateRate = DEF_RUSAGE_UPDATE_RATE;
int    rusageUpdatePercent = DEF_RUSAGE_UPDATE_PERCENT;
int    clean_period = DEF_CLEAN_PERIOD;
int    max_retry    = DEF_MAX_RETRY;
int    retryIntvl   = DEF_RETRY_INTVL;
int    max_sbdFail  = DEF_MAXSBD_FAIL;
int    sendEvMail   = 0;
int    maxJobId = DEF_MAX_JOBID;

int    maxJobArraySize = DEF_JOB_ARRAY_SIZE;
int    jobRunTimes = INFINIT_INT;
int    jobDepLastSub = 0;
int    maxjobnum    = DEF_MAX_JOB_NUM;
int    accept_intvl = DEF_ACCEPT_INTVL;
int    preExecDelay = DEF_PRE_EXEC_DELAY;
int    slotResourceReserve = FALSE;
int    maxAcctArchiveNum = -1;
int    acctArchiveInDays = -1;
int    acctArchiveInSize = -1;
int    resourcePerTask = 0;
float  cpuTimeFactor = DEF_CPU_TIME_FACTOR;
float  runTimeFactor = DEF_RUN_TIME_FACTOR;
float  runJobFactor = DEF_RUN_JOB_FACTOR;
float  histHours = DEF_HIST_HOURS;
float  clsDecay = 1.0; /* cluster-wide decay factor for history of CPU time */

int    numofqueues  = 0;
int    numofprocs   = 0;
int    numofusers    = 0;
int    numofugroups  = 0;
int    numofhgroups  = 0;
int    mSchedStage = 0;
int    maxSchedStay = DEF_SCHED_STAY;
int    freshPeriod = DEF_FRESH_PERIOD;
int    qAttributes = 0;
int    **hReasonTb = NULL;
int    **cReasonTb = NULL;
time_t now;
long   schedSeqNo = 0;
UDATA_TABLE_T * uDataPtrTb;
struct hTab uDataList;

/* Host data main global data structures.
 */
struct hTab hostTab;
LIST_T *hostList = NULL;

struct qData *qDataList = NULL;
struct jData *jDataList[ALLJLIST];
int    pendJobSlots = 0;
struct jData *chkJList;

unitTypes unitForLimits = Megabytes;

struct hTab cpuFactors;
struct gData *usergroups[MAX_GROUPS];
struct gData *hostgroups[MAX_GROUPS];
struct clientNode *clientList = NULL;

struct lsInfo *allLsInfo;
struct hTab calDataList;
struct hTab condDataList;

char   *masterHost = NULL;
char   *clusterName = NULL;
char   *defaultQueues = NULL;
char   *defaultHostSpec = NULL;
char   *env_dir = NULL;
char   *lsfDefaultProject = NULL;
char   *pjobSpoolDir = NULL;
time_t condCheckTime = DEF_COND_CHECK_TIME;
bool_t mcSpanClusters = FALSE;
int    readNumber = 0;
int    dispatch = FALSE;
int    maxJobPerSession = INFINIT_INT;

int    maxUserPriority = -1;
int    jobPriorityValue = -1;
int    jobPriorityTime = -1;
static int jobPriorityUpdIntvl = -1;

int nSbdConnections = 0;
int maxSbdConnections = DEF_MAX_SBD_CONNS;
int numResources = 0;
struct hostInfo *LIMhosts = NULL;
int    numLIMhosts = 0;

float maxCpuFactor = 0.0;
struct sharedResource **sharedResources = NULL;

int sharedResourceUpdFactor = INFINIT_INT;
long   schedSeqNo;
int    schedule;
int    scheRawLoad;
int lsbModifyAllJobs = FALSE;

static int schedule1;
static struct jData *jobData = NULL;
static time_t lastSchedTime = 0;
static time_t nextSchedTime = 0;

void setJobPriUpdIntvl(void);
static void updateJobPriorityInPJL(void);
static void houseKeeping (int *);
static void periodicCheck (void);
static int authRequest(struct lsfAuth *, XDR *, struct LSFHeader *,
                       struct sockaddr_in *, struct sockaddr_in *,
                       char *, int);
static int processClient(struct clientNode *, int *);

static void clientIO();
static int forkOnRequest(mbdReqType);
static void shutdownSbdConnections(void);
static void processSbdNode(struct sbdNode *, int);
static void setNextSchedTimeWhenJobFinish(void);
static void acceptConnection(int);

extern void chanInactivate_(int);
extern void chanActivate_(int);
extern int do_chunkStatusReq(XDR *, int, struct sockaddr_in *, int *,
                             struct LSFHeader *);
extern int do_setJobAttr(XDR *, int, struct sockaddr_in *, char *,
                         struct LSFHeader *, struct lsfAuth *);
extern void chanCloseAllBut_(int);
extern int initLimSock_(void);

int
main (int argc, char **argv)
{
    struct timeval timeout;
    int nready = 0;
    struct epoll_event *ev;
    int i;
    int cc;
    int hsKeeping = FALSE;
    time_t lastPeriodicCheckTime = 0;
    time_t lastElockTouch;

    saveDaemonDir_(argv[0]);

    opterr = 0;
    while ((cc = getopt(argc, argv, "hVd:12C")) != EOF) {
        switch (cc) {
            case '1':
            case '2':
                debug = cc - '0';
                break;
            case 'd':
                env_dir = optarg;
                break;
            case 'C':
                putEnv("RECONFIG_CHECK","YES");
                fputs("\n", stdout);
                lsb_CheckMode = 1;
                break;
            case 'V':
                fputs(_LS_VERSION_, stdout);
                return -1;
            case 'h':
            default:
                fprintf(stderr, "\
%s: mbatchd [-h] [-V] [-C] [-d env_dir] [-1 |-2]\n", __func__);
                return -1;
        }
    }

    if (initenv_(daemonParams, env_dir) < 0) {

        ls_openlog("mbatchd",
                   daemonParams[LSF_LOGDIR].paramValue,
                   (debug > 1 || lsb_CheckMode),
                   daemonParams[LSF_LOG_MASK].paramValue);
        ls_syslog(LOG_ERR, "%s initenv() failed", __func__);
        if (!lsb_CheckMode)
            mbdDie(MASTER_FATAL);
        else
            lsb_CheckError = FATAL_ERR;
    }

    if (!debug && isint_(daemonParams[LSB_DEBUG].paramValue)) {
        debug = atoi(daemonParams[LSB_DEBUG].paramValue);
        if (debug <= 0 || debug > 2)
            debug = 1;
    }

    if (debug < 2 && !lsb_CheckMode) {
        for (i = sysconf(_SC_OPEN_MAX) ; i >= 3 ; i--)
            close(i);
    }

    getLogClass_(daemonParams[LSB_DEBUG_MBD].paramValue,
                 daemonParams[LSB_TIME_MBD].paramValue);

    if (lsb_CheckMode)
        ls_openlog("mbatchd", daemonParams[LSF_LOGDIR].paramValue, TRUE,
                   "LOG_WARN");
    else if (debug > 1)
        ls_openlog("mbatchd", daemonParams[LSF_LOGDIR].paramValue, TRUE,
                   daemonParams[LSF_LOG_MASK].paramValue);
    else
        ls_openlog("mbatchd", daemonParams[LSF_LOGDIR].paramValue, FALSE,
                   daemonParams[LSF_LOG_MASK].paramValue);

    if (logclass)
        ls_syslog(LOG_DEBUG, "%s: logclass=%x", __func__, logclass);


    if (isint_(daemonParams[LSB_MBD_CONNTIMEOUT].paramValue))
        connTimeout = atoi(daemonParams[LSB_MBD_CONNTIMEOUT].paramValue);
    else
        connTimeout = 5;

    glMigToPendFlag = FALSE;

    if (isint_(daemonParams[LSB_MBD_MIGTOPEND].paramValue))
        if (atoi(daemonParams[LSB_MBD_MIGTOPEND].paramValue) != 0)
            glMigToPendFlag = TRUE;

    if (isint_(daemonParams[LSB_MIG2PEND].paramValue)) {
        if (atoi(daemonParams[LSB_MIG2PEND].paramValue) != 0) {
            glMigToPendFlag = TRUE;
        }
    }

    if (isint_(daemonParams[LSB_REQUEUE_TO_BOTTOM].paramValue)) {
        if (atoi(daemonParams[LSB_REQUEUE_TO_BOTTOM].paramValue) != 0)
            requeueToBottom = TRUE;
    }

    if (isint_(daemonParams[LSB_ARRAY_SCHED_ORDER].paramValue)) {
        if (atoi(daemonParams[LSB_ARRAY_SCHED_ORDER].paramValue) != 0)
            arraySchedOrder = TRUE;
    }

    if (daemonParams[LSB_HJOB_PER_SESSION].paramValue != NULL) {
        if (atoi(daemonParams[LSB_HJOB_PER_SESSION].paramValue) > 0) {
            maxJobPerSession =
                atoi(daemonParams[LSB_HJOB_PER_SESSION].paramValue);
        } else {
            ls_syslog(LOG_ERR, "\
%s: Invalid LSB_HJOB_PER_SESSION %s ignored",
                      __func__,
                      daemonParams[LSB_HJOB_PER_SESSION].paramValue);
        }
    }

    if ((daemonParams[LSB_MOD_ALL_JOBS].paramValue != NULL)
        && (strcasecmp(daemonParams[LSB_MOD_ALL_JOBS].paramValue, "y") == 0
            || strcasecmp(
                daemonParams[LSB_MOD_ALL_JOBS].paramValue, "yes") == 0)) {
        lsbModifyAllJobs = TRUE;
    }

    if (daemonParams[LSB_PTILE_PACK].paramValue != NULL
        && (strcasecmp(daemonParams[LSB_PTILE_PACK].paramValue, "y") == 0)) {


        setLsbPtilePack(TRUE);
    }

    if (daemonParams[LSF_UNIT_FOR_LIMITS].paramValue != NULL) {

        strToUpper_(daemonParams[LSF_UNIT_FOR_LIMITS].paramValue);

        unitForLimits = setUnitForLimits(daemonParams[LSF_UNIT_FOR_LIMITS].paramValue);

    }

    daemon_doinit();

    if ((!debug) && (!lsb_CheckMode))  {

        if (getuid() != 0) {
            ls_syslog(LOG_ERR, "\
%s: Real uid is %d, not root", __func__, (int)getuid());
            mbdDie(MASTER_FATAL);
        }

        if (geteuid() != 0) {
            ls_syslog(LOG_ERR, "\
%s: Effective uid is %d, not root", __func__, (int)geteuid());
            mbdDie(MASTER_FATAL);
        }
    }

    now = time(NULL);
    if (lsb_CheckMode == TRUE)
        TIMEIT(0, minit(FIRST_START),"minit");

    masterHost = ls_getmastername();
    for (i = 0; i < 3 && !masterHost && lserrno == LSE_TIME_OUT; i++) {
        millisleep_(6000);
        masterHost = ls_getmastername();
    }
    if (masterHost == NULL) {

        ls_syslog(LOG_ERR, "\
%s: Failed to contact LIM: %M; quit master", __func__);
        if (! lsb_CheckMode)
            mbdDie(MASTER_RESIGN);
        else
            lsb_CheckError = FATAL_ERR;
    } else {
        char *myhostnm;

        if ((myhostnm = ls_getmyhostname()) == NULL) {
            ls_syslog(LOG_ERR, "\
%s: Weird ls_getmyhostname() failed...%M", __func__);
            if (! lsb_CheckMode)
                mbdDie(MASTER_FATAL);
            else
                lsb_CheckError = FATAL_ERR;
        }

        if (!equalHost_ (masterHost, myhostnm)) {
            ls_syslog(LOG_ERR, "\
%s: Local host is not master %s", __func__, masterHost);
            if (! lsb_CheckMode)
                mbdDie(MASTER_RESIGN);
        }

        if (!Gethostbyname_(myhostnm)) {
            ls_syslog(LOG_ERR, "\
%s: Omygosh... cannot resolve my own name %s", __func__, myhostnm);
            if (! lsb_CheckMode)
                mbdDie(MASTER_FATAL);
            else
                lsb_CheckError = FATAL_ERR;
        }
    }

    umask(022);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    if (lsb_CheckMode) {
        ls_syslog(LOG_INFO, "%s: Checking Done", __func__);
        exit(lsb_CheckError);
    }

    /* Go go go...
     */
    chanEpollStart();
    TIMEIT(0, minit(FIRST_START),"minit");
    log_mbdStart();
    ls_syslog(LOG_INFO, "%s: (re-)started", __func__);
    pollSbatchds(FIRST_START);
    lastSchedTime  = 0;
    nextSchedTime  = time(0) + msleeptime;
    lastElockTouch = time(0) - msleeptime;
    schedulerInit();
    setJobPriUpdIntvl();

    for (;;) {
        int maxfd;


        maxfd = sysconf(_SC_OPEN_MAX);
        now = time(0);

        if ( (now - lastSchedTime >= msleeptime)
             || (now >= nextSchedTime) ) {
            schedule = TRUE;
        }

        if (schedule) {
            hsKeeping = TRUE;
            timeout.tv_sec = 0;
        }

        shutdownSbdConnections();

        if (now - lastElockTouch >= msleeptime) {
            touchElogLock();
            lastElockTouch = now;
        }

        if (now - lastPeriodicCheckTime > 5 * 60
            && lastPeriodicCheckTime != 0 ) {
            hsKeeping = FALSE;
            timeout.tv_sec = 0;
        }


        nready = chanEpoll_(&ev, &timeout);
        if (nready < 0) {
            if (errno != EINTR)
                ls_syslog(LOG_ERR, "\
%s: Ohmygosh.. epoll() failed %m", __func__);
            continue;
        }

        if (nready == 0
            || ((now - lastSchedTime) >= 2 * msleeptime)) {

            if (hsKeeping) {
                houseKeeping (&hsKeeping);
            } else {
                periodicCheck ();
                lastPeriodicCheckTime = now;
            }

            if (!hsKeeping) {
                timeout.tv_sec = POLL_INTERVAL;
            } else {
                timeout.tv_sec = 0;
            }

            timeout.tv_usec = 0;
            if (nready == 0)
                continue;
        }

        timeout.tv_sec  = 0;
        timeout.tv_usec = 0;

        if (chanEventsReady(batchSock, EPOLLIN)) {
            acceptConnection(batchSock);
        }

        clientIO();

    } /* for (;;) */
}

static void
acceptConnection(int socket)
{
    int s;
    struct sockaddr_in from;
    struct hostent *hp;
    struct clientNode *client;

    s = chanAccept_(socket, (struct sockaddr_in *)&from);
    if (s == -1) {
        ls_syslog(LOG_ERR, "%s Ohmygosh accept() failed... %m", __func__);
        return;
    }

    hp = Gethostbyaddr_(&from.sin_addr.s_addr,
                        sizeof(in_addr_t),
                        AF_INET);
    if (hp == NULL) {
        ls_syslog(LOG_WARNING, "\
%s: gethostbyaddr() failed for %s", __func__,
                  sockAdd2Str_(&from));
        errorBack(s, LSBE_PERMISSION, &from);
        chanClose_(s);
        return;
    }

    ls_syslog(LOG_DEBUG, "\
%s: Received request from host %s %s on socket %d",
              __func__, hp->h_name, sockAdd2Str_(&from),
              chanSock_(s));

    memcpy(&from.sin_addr, hp->h_addr, hp->h_length);

    client = my_calloc(1, sizeof(struct clientNode), __func__);
    client->chanfd = s;
    client->from =  from;
    client->fromHost = safeSave(hp->h_name);
    client->reqType = 0;
    client->lastTime = 0;

    inList((struct listEntry *)clientList,
           (struct listEntry *) client);

    ls_syslog(LOG_DEBUG, "\
%s: Accepted connection from host %s on channel %d",
              __func__, client->fromHost, client->chanfd);
}

static void
clientIO()
{
    struct clientNode *cliPtr;
    struct clientNode *nextClient;
    struct sbdNode *sbdPtr;
    struct sbdNode *nextSbdPtr;
    int exception;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"clientIO: Entering...");

    for (sbdPtr = sbdNodeList.forw;
         sbdPtr != &sbdNodeList;
         sbdPtr = nextSbdPtr) {
        nextSbdPtr = sbdPtr->forw;
        if (chanEventsReady(sbdPtr->chanfd, EPOLLIN)
            || chanEventsReady(sbdPtr->chanfd, EPOLLERR)) {

            if (chanEventsReady(sbdPtr->chanfd, EPOLLERR))
                exception = TRUE;
            else
                exception = FALSE;
            processSbdNode(sbdPtr, exception);
        }
    }


    for (cliPtr = clientList->forw;
         cliPtr != clientList;
         cliPtr = nextClient) {
        int needFree;
        nextClient = cliPtr->forw;
        if (chanEventsReady(cliPtr->chanfd, EPOLLERR)) {

            shutDownClient(cliPtr);
            continue;
        }
        needFree = FALSE;
        if (chanEventsReady(cliPtr->chanfd, EPOLLIN)) {

            int saveChfd;
            saveChfd = cliPtr->chanfd;
            if (processClient(cliPtr, &needFree) == 0) {

                chanQuitReadyEvents(saveChfd, EPOLLIN);
                if (needFree == TRUE) {
                    offList((struct listEntry *)cliPtr);
                    FREEUP(cliPtr->fromHost);
                    FREEUP(cliPtr);
                }
            }
        }

    }
}

static int
processClient(struct clientNode *client, int *needFree)
{
    static char          fname[]="processClient()";
    struct Buffer        *buf;
    struct bucket        *bucket;
    mbdReqType           mbdReqtype;
    int                  s;
    int                  pid;
    int                  cc = LSBE_NO_ERROR;
    unsigned int         len;
    struct sockaddr_in   from;
    struct sockaddr_in   laddr;
    socklen_t            laddrLen;
    struct lsfAuth       auth;
    struct LSFHeader     reqHdr;
    XDR                  xdrs;
    int                  statusReqCC = 0;
    int                  hostOkFlag = 0;

    laddrLen = sizeof(laddr);
    memset(&auth, 0, sizeof(auth));
    s = client->chanfd;

    if (chanDequeue_(client->chanfd, &buf) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_ENO_D, fname, "chanDequeue_",
                  cherrno);
        shutDownClient(client);
        return(-1);
    }

    xdrmem_create(&xdrs, buf->data, buf->len, XDR_DECODE);
    if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
        xdr_destroy(&xdrs);
        chanFreeBuf_(buf);
        shutDownClient(client);
        return(-1);
    }

    len = reqHdr.length;
    mbdReqtype = reqHdr.opCode;
    from = client->from;


    if (logclass & (LC_COMM | LC_TRACE)) {
        ls_syslog(LOG_DEBUG, "\
%s: Received request <%d> from host <%s/%s> on channel <%d>",
                  fname, mbdReqtype, client->fromHost,
                  sockAdd2Str_(&from), s);
    }

    if( hostIsLocal(client->fromHost) ) {
        hostOkFlag = hostOk(client->fromHost, LOCAL_ONLY);
    } else {
        hostOkFlag = hostOk(client->fromHost, 0);
    }

    switch (hostOkFlag) {
        case -1:

            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd , NL_SETN, 5014,
                                                 "%s: Request from non-LSF host <%s>"), /* catgets 5014 */
                      fname,
                      sockAdd2Str_(&from));
            errorBack(s, LSBE_NOLSF_HOST, &from);
            goto endLoop;
        default:

            break;
    }

    if (reqHdr.opCode != PREPARE_FOR_OP)
        if (io_block_(chanSock_(s)) < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "io_block_");

    if (getsockname(chanSock_(s),
                    (struct sockaddr *) &laddr,
                    &laddrLen) == -1) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "getsockname");
        errorBack(s, LSBE_PROTOCOL, &from);
        goto endLoop;
    }

    if ((cc = authRequest(&auth, &xdrs, &reqHdr, &from, &laddr,
                          client->fromHost, chanSock_(s))) !=
        LSBE_NO_ERROR) {
        errorBack(s, cc, &from);
        goto endLoop;
    }

    if (forkOnRequest(mbdReqtype)) {

        if ((pid = fork()) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
            errorBack(s, LSBE_NO_FORK, &from);
        }

        if (pid != 0) {
            goto endLoop;
        }

        if (debug < 2)
            closeExceptFD(chanSock_(s));
    }

    switch (mbdReqtype) {

        case PREPARE_FOR_OP:
            if (do_readyOp(&xdrs, client->chanfd, &from, &reqHdr) < 0) {
                shutDownClient(client);
                xdr_destroy(&xdrs);
                chanFreeBuf_(buf);
                return(-1);
            }
            break;

        case BATCH_JOB_SUB:
            jobData = NULL;
            TIMEIT(0, do_submitReq(&xdrs, s, &from, client->fromHost, &reqHdr, &laddr, &auth, &schedule1, dispatch, &jobData), "do_submitReq()");
            setNextSchedTimeUponNewJob(jobData);
            statusChanged = 1;
            break;
        case BATCH_JOB_SIG:
            TIMEIT(0, do_signalReq(&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_signalReq()");
            break;
        case BATCH_JOB_MSG:
            NEW_BUCKET(bucket,buf);
            if (bucket) {
                TIMEIT(0, do_jobMsg(bucket, &xdrs, s, &from, client->fromHost, &reqHdr, &auth), "do_jobMsg()");
            } else {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "NEW_BUCKET");
            }
            break;
        case BATCH_QUE_CTRL:
            TIMEIT(0, do_queueControlReq (&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_queueControlReq()");
            break;
        case BATCH_DEBUG:
            TIMEIT(0, do_debugReq (&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_debugReq()");
            break;

        case BATCH_RECONFIG:
            TIMEIT(0, do_reconfigReq(&xdrs, s, &from, client->fromHost, &reqHdr),"do_reconfigReq()");
            break;
        case BATCH_JOB_MIG:
            TIMEIT(0, do_migReq(&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_migReq()");
            if (mSchedStage == 0) {
                setNextSchedTimeWhenJobFinish();
            }
            break;
        case BATCH_STATUS_MSG_ACK:
        case BATCH_STATUS_JOB:
        case BATCH_RUSAGE_JOB:
            TIMEIT(0, (statusReqCC = do_statusReq(&xdrs, s, &from,
                                                  &schedule1, &reqHdr)),
                   "do_statusReq()");

            if (mSchedStage == 0) {
                setNextSchedTimeWhenJobFinish();
            }
            if (client->lastTime == 0)
                nSbdConnections++;
            break;
        case BATCH_STATUS_CHUNK:
            TIMEIT(0, (statusReqCC = do_chunkStatusReq(&xdrs, s, &from,
                                                       &schedule1, &reqHdr)),
                   "do_chunkStatusReq()");

            if (mSchedStage == 0) {
                setNextSchedTimeWhenJobFinish();
            }
            if (client->lastTime == 0)
                nSbdConnections++;
            break;
        case BATCH_SLAVE_RESTART:
            TIMEIT(0, do_restartReq(&xdrs, s, &from, &reqHdr),"do_restartReq()");
            break;
        case BATCH_HOST_CTRL:
            TIMEIT(0, do_hostControlReq(&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_hostControlReq()");
            break;
        case BATCH_JOB_SWITCH:
            TIMEIT(3, do_jobSwitchReq(&xdrs, s, &from, client->fromHost,&reqHdr, &auth),"do_jobSwitchReq()");
            break;
        case BATCH_JOB_MOVE:
            TIMEIT(3, do_jobMoveReq(&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_jobMoveReq()");
            break;
        case BATCH_SET_JOB_ATTR:
            do_setJobAttr(&xdrs, s, &from, client->fromHost, &reqHdr, &auth);
            break;
        case BATCH_JOB_MODIFY:
            TIMEIT(3, do_modifyReq(&xdrs, s, &from, client->fromHost, &reqHdr,
                                   &auth),"do_modifyReq()");
            break;

        case BATCH_JOB_PEEK:
            TIMEIT(0, do_jobPeekReq(&xdrs, s, &from, client->fromHost, &reqHdr, &auth),"do_jobPeekReq()");
            break;
        case BATCH_USER_INFO:
            TIMEIT(0, do_userInfoReq(&xdrs, s, &from, &reqHdr),"do_userInfoReq()");
            break;
        case BATCH_PARAM_INFO:
            TIMEIT(0, do_paramInfoReq(&xdrs, s, &from, &reqHdr),"do_paramInfoReq()");
            break;
        case BATCH_GRP_INFO:
            TIMEIT(3, do_groupInfoReq(&xdrs, s, &from, &reqHdr),"do_groupInfoReq()");
            break;
        case BATCH_QUE_INFO:
            TIMEIT(3, do_queueInfoReq(&xdrs, s, &from, &reqHdr),"do_queueInfoReq()");
            break;
        case BATCH_JOB_INFO:
            TIMEIT(3, do_jobInfoReq(&xdrs, s, &from, &reqHdr, schedule),"do_jobInfoReq()");
            break;
        case BATCH_HOST_INFO:
            TIMEIT(3, do_hostInfoReq(&xdrs, s, &from, &reqHdr),"do_hostInfoReq()");
            break;
        case BATCH_RESOURCE_INFO:
            TIMEIT(3, do_resourceInfoReq(&xdrs, s, &from, &reqHdr),"do_resourceInfoReq()");
            break;
        case BATCH_JOB_FORCE:
            TIMEIT(0,
                   do_runJobReq(&xdrs, s, &from, &auth, &reqHdr),
                   "do_runJobReq()");
            break;
        default:
            errorBack(s, LSBE_PROTOCOL, &from);
            if (reqHdr.version <= VOLCLAVA_VERSION)
                ls_syslog(LOG_ERR, "\
%s: Unknown request type %d from host %s",
                          fname, mbdReqtype, sockAdd2Str_(&from));
            break;
    }


    if (forkOnRequest(mbdReqtype)) {
        chanFreeBuf_(buf);
        exit(0);
    }
endLoop:
    client->reqType = mbdReqtype;
    client->lastTime = now;
    xdr_destroy(&xdrs);
    chanFreeBuf_(buf);
    if ((reqHdr.opCode != PREPARE_FOR_OP &&
         reqHdr.opCode != BATCH_STATUS_JOB &&
         reqHdr.opCode != BATCH_RUSAGE_JOB &&
         reqHdr.opCode != BATCH_STATUS_MSG_ACK &&
         reqHdr.opCode != BATCH_STATUS_CHUNK) ||
        statusReqCC < 0) {
        shutDownClient(client);
        return(-1);
    }
    return(0);

}

void
shutDownClient(struct clientNode *client)
{
    if ((client->reqType == BATCH_STATUS_JOB
         || client->reqType == BATCH_STATUS_MSG_ACK
         || client->reqType == BATCH_RUSAGE_JOB
         || client->reqType == BATCH_STATUS_CHUNK)
        && client->lastTime)
        nSbdConnections--;

    chanClose_(client->chanfd);
    offList((struct listEntry *)client);
    if (client->fromHost)
        free(client->fromHost);
    free(client);
}

static void
houseKeeping (int *hsKeeping)
{
#define SCHED  1
#define DISPT  2
#define RESIG  3
#define T15MIN (60*15)

    static int resignal = FALSE;
    static time_t lastAcctSched = 0;
    static int myTurn = RESIG;

    ls_syslog(LOG_DEBUG, "\
%s: mSchedStage=%x schedule=%d eventPending=%d now=%d lastSchedTime=%d nextSchedTime=%d", __func__, mSchedStage, schedule, eventPending,
              (int)now, (int)lastSchedTime, (int)nextSchedTime);

    if (lastAcctSched == 0){
        lastAcctSched = now;
    } else{
        if ((now - lastAcctSched) > T15MIN){
            lastAcctSched = now;
            checkAcctLog();
        }
    }

    if (myTurn == RESIG)
        myTurn = SCHED;
    if (schedule && myTurn == SCHED) {
        if (eventPending) {
            resignal = TRUE;
        }
        now = time(0);
        if (schedule) {
            lastSchedTime = now;
            nextSchedTime = now + msleeptime;
            TIMEIT(0, schedule = scheduleAndDispatchJobs(),
                   "scheduleAndDispatchJobs");
            if (schedule == 0) {
                schedule = FALSE;
            } else {
                schedule = TRUE;
            }
            return;
        }
    }

    if (myTurn == SCHED)
        myTurn = RESIG;
    if (resignal && myTurn == RESIG) {
        RESET_CNT();
        TIMEIT(0, resigJobs (&resignal), "resigJobs()");
        DUMP_CNT();
        return;
    }

    *hsKeeping = FALSE;
}

static void
periodicCheck(void)
{
    char *myhostnm;
    static time_t last_chk_time = 0;
    static int winConf = FALSE;
    static time_t lastPollTime = 0, last_checkConf = 0;
    static time_t last_hostInfoRefreshTime = 0;
    static time_t last_checkNqsJobsTime = 0;
    static time_t last_tryControlJobs = 0;
    static time_t last_jobPriUpdTime = 0;
    static time_t first_hostInfoRefreshTime = 0;
    static int    histTime = 0;

    ls_syslog(LOG_DEBUG, "%s: Entering this routine...", __func__);

    if (last_chk_time == 0) {
        last_hostInfoRefreshTime = now;
        last_checkNqsJobsTime = now;
        first_hostInfoRefreshTime = now;
    }

    switchELog();

    if (jobPriorityUpdIntvl > 0) {
        if (now - last_jobPriUpdTime >= jobPriorityUpdIntvl * 60 ) {
            TIMEIT(0, updateJobPriorityInPJL( ), "updateJobPriorityInPJL()");
            last_jobPriUpdTime = now;
        }
    }

    if (now - lastPollTime > POLL_INTERVAL) {
        TIMEIT(0, pollSbatchds(NORMAL_RUN),"pollSbatchds()");
        lastPollTime = now;
    }

    if (now - last_chk_time > msleeptime) {

        masterHost = ls_getmastername();
        if (masterHost == NULL) {
            ls_syslog(LOG_ERR, "\
%s: Ohmygosh unable to contact LIM: %M; quit master", __func__);
            mbdDie(MASTER_RESIGN);
        }

        if ((myhostnm = ls_getmyhostname()) == NULL) {
            ls_syslog(LOG_ERR, "\
%s: Weird ls_getmyhostname() failed...%M", __func__);
            mbdDie(MASTER_RESIGN);
        }
        if (!equalHost_(masterHost, myhostnm)) {
            masterHost = myhostnm;
            mbdDie(MASTER_RESIGN);
        }

        clean(now);
        checkQWindow();
        checkHWindow();

        TIMEIT(0, checkJgrpDep(), "checkJgrpDep");

        /* decay and calculate history CPU time every 15 mins */
        histTime += now - last_chk_time;
        if (histTime >= CALCULATE_INTERVAL) {
            TIMEIT(0, updAllSAcctForDecay(now), "updAllSAcctForDecay");
            histTime = 0;
        }

        now = time(0);
        last_chk_time = now;
    }

    if (now - last_tryControlJobs > sbdSleepTime) {
        last_tryControlJobs = now;
        TIMEIT(0, tryResume(), "tryResume()");
    }

    if (now - last_checkConf > condCheckTime) {
        if (winConf == FALSE && updAllConfCond() == TRUE)
            winConf = TRUE;
        if (dispatch == FALSE && winConf == TRUE) {
            readNumber++;
            winConf = FALSE;
        }
        last_checkConf = now;
    }
    if (now - last_hostInfoRefreshTime > 10 * 60) {
        getLsbHostInfo();
        last_hostInfoRefreshTime = now;
    }
}


void
terminate_handler(int sig)
{
    sigset_t newmask;
    sigset_t oldmask;

    sigemptyset(&newmask);
    sigaddset(&newmask, SIGTERM);
    sigaddset(&newmask, SIGINT);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    exit(sig);
}

void
child_handler (int sig)
{
    int pid, saveErrno;
    LS_WAIT_T status;
    sigset_t newmask, oldmask;

    saveErrno = errno;
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGTERM);
    sigaddset(&newmask, SIGINT);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        ;

    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    errno = saveErrno;
}


static int
authRequest(struct lsfAuth *auth,
            XDR *xdrs,
            struct LSFHeader *reqHdr,
            struct sockaddr_in *from,
            struct sockaddr_in *local,
            char *hostName,
            int s)
{
    mbdReqType reqType = reqHdr->opCode;
    char buf[MAXLSFNAMELEN];

    if (!(reqType == BATCH_JOB_SUB
          || reqType == BATCH_JOB_PEEK
          || reqType == BATCH_JOB_SIG
          || reqType == BATCH_QUE_CTRL
          || reqType == BATCH_RECONFIG
          || reqType == BATCH_JOB_MIG
          || reqType == BATCH_HOST_CTRL
          || reqType == BATCH_JOB_SWITCH
          || reqType == BATCH_JOB_MOVE
          || reqType == BATCH_JOB_MODIFY
          || reqType == BATCH_DEBUG
          || reqType == BATCH_JOB_FORCE
          || reqType == BATCH_SET_JOB_ATTR))
        return LSBE_NO_ERROR;

    if (!xdr_lsfAuth(xdrs, auth, reqHdr)) {
        ls_syslog(LOG_ERR, "\
%s: Ohmygosh failed to decode auth from %s", __func__,
                  sockAdd2Str_(from));
        return LSBE_XDR;
    }

    putEauthClientEnvVar("user");
    sprintf(buf, "mbatchd@%s", clusterName);
    putEauthServerEnvVar(buf);

    if (0) {
        /* openlava 20 there is a memory problem that has
         * to be fixed, root cause is xdr_shortLsInfo()
         * invoked by getLSFAdmin() is freeing static
         * memory we use from ls_gethostinfo().
         */
        if (!userok(s, from, hostName, local, auth, debug))
            return LSBE_PERMISSION;
    }

    switch(reqType) {
        case BATCH_JOB_SUB:
            if (auth->uid == 0
                && daemonParams[LSF_ROOT_REX].paramValue  == NULL) {
                ls_syslog(LOG_CRIT, "\
%s: Root user's job submission rejected", __func__);
                return LSBE_PERMISSION;
            }
            break;
        case BATCH_RECONFIG:
        case BATCH_HOST_CTRL:
            if (!isAuthManager(auth) && auth->uid != 0) {
                ls_syslog(LOG_CRIT, "\
%s: uid %d not allowed to perform control operation",
                    __func__, auth->uid);
                return LSBE_PERMISSION;
            }
            break;
        default:
            break;
    }

    return LSBE_NO_ERROR;
}


static int
forkOnRequest(mbdReqType req)
{
    if (daemonParams[MBD_DONT_FORK].paramValue)
        return 0;

    if (req == BATCH_JOB_INFO
        || req == BATCH_QUE_INFO
        || req == BATCH_HOST_INFO
        || req == BATCH_GRP_INFO
        || req == BATCH_RESOURCE_INFO
        || req == BATCH_PARAM_INFO
        || req == BATCH_USER_INFO
        || req == BATCH_JOB_PEEK) {
        return 1;
    }

    return 0;
}

static void
shutdownSbdConnections(void)
{
    struct clientNode *cliPtr;
    struct clientNode *nextClient;
    struct clientNode *deleteCliPtr;
    struct sbdNode *sbdPtr;
    struct sbdNode *nextSbdPtr;
    struct sbdNode *deleteSbdPtr;
    time_t oldest = now + 1;

    if (nSbdConnections < maxSbdConnections)
        return;

    ls_syslog(LOG_DEBUG, "\
%s: nSbdConnections=%d maxSbdConnections=%d",
              __func__, nSbdConnections, maxSbdConnections);

    deleteCliPtr = NULL;
    for(cliPtr = clientList->forw;
        cliPtr != clientList; cliPtr=nextClient) {
        nextClient = cliPtr->forw;

        if (cliPtr->reqType == BATCH_STATUS_JOB
            || cliPtr->reqType == BATCH_STATUS_MSG_ACK
            || cliPtr->reqType == BATCH_RUSAGE_JOB
            || cliPtr->reqType == BATCH_STATUS_CHUNK) {

            if (cliPtr->lastTime < oldest) {
                deleteCliPtr = cliPtr;
                oldest = cliPtr->lastTime;
            }
        }
    }

    if (deleteCliPtr) {
        shutDownClient(deleteCliPtr);
        return;
    }

    deleteSbdPtr = NULL;
    for (sbdPtr = sbdNodeList.forw;
         sbdPtr != &sbdNodeList;
         sbdPtr = nextSbdPtr) {
        nextSbdPtr = sbdPtr->forw;

        if (sbdPtr->lastTime < oldest) {
            if (deleteSbdPtr == NULL
                || sbdPtr->reqCode >= deleteSbdPtr->reqCode) {

                deleteSbdPtr = sbdPtr;
                oldest = sbdPtr->lastTime;
            }
        }
    }

    if (deleteSbdPtr) {
        processSbdNode(deleteSbdPtr, TRUE);
    }
}

static void
processSbdNode(struct sbdNode *sbdPtr, int exception)
{

    switch (sbdPtr->reqCode) {
        case MBD_NEW_JOB:
            doNewJobReply(sbdPtr, exception);
            if (sbdPtr->reqCode == MBD_NEW_JOB_KEEP_CHAN)
                return;
            break;
        case MBD_PROBE:
            doProbeReply(sbdPtr, exception);
            break;
        case MBD_SWIT_JOB:
            doSwitchJobReply(sbdPtr, exception);
            break;
        case MBD_SIG_JOB:
            doSignalJobReply(sbdPtr, exception);
            break;
        case MBD_NEW_JOB_KEEP_CHAN:
            break;
        default:
            ls_syslog(LOG_ERR, "\
%s: Unsupported sbdNode request %d", __func__, sbdPtr->reqCode);
    }

    chanClose_(sbdPtr->chanfd);
    offList((struct listEntry *) sbdPtr);
    FREEUP(sbdPtr);
    nSbdConnections--;
}

void
setNextSchedTimeUponNewJob(struct jData *jPtr)
{
    if (mSchedStage == 0 && jPtr) {

        time_t newTime = INFINIT_INT;
        if (jPtr->qPtr->schedDelay != INFINIT_INT) {
            newTime = now + jPtr->qPtr->schedDelay;
        }
        if (newTime < nextSchedTime) {
            nextSchedTime = newTime;
        }
    }
}

static void
setNextSchedTimeWhenJobFinish(void)
{
    time_t newTime;

    newTime = now + DEF_SCHED_DELAY;
    if (newTime < nextSchedTime) {
        nextSchedTime = newTime;
    }
}

void
setJobPriUpdIntvl(void)
{
    const int MINIMAL = 5;
    int   value;

    if (jobPriorityValue < 0 || jobPriorityTime < 0) {
        jobPriorityUpdIntvl = -1;
        return;
    }

    if (jobPriorityTime <= MINIMAL) {
        jobPriorityUpdIntvl = jobPriorityTime;
        return;
    }

    for(value = 16; value > 1; value /= 2) {
        if (jobPriorityTime / value >= MINIMAL) {
            jobPriorityUpdIntvl = jobPriorityTime / value;
            break;
        }

    }

    if (jobPriorityUpdIntvl < 0) {
        jobPriorityUpdIntvl = MINIMAL;
    }
}


void
updateJobPriorityInPJL(void)
{
    static int count;
    int term;
    int priority;
    struct jData *jp;

    if (jobPriorityTime != jobPriorityUpdIntvl) {

        term     = jobPriorityTime / jobPriorityUpdIntvl;
        count    = (count+1) % term;
        priority = count * jobPriorityValue / term;
    } else {
        priority = jobPriorityValue;
    }

    for (jp = jDataList[PJL]->forw;
         jp != jDataList[PJL]; jp = jp->forw) {
        unsigned int newVal = jp->jobPriority + priority;
        jp->jobPriority = MIN(newVal, (unsigned int)MAX_JOB_PRIORITY);
    }
}

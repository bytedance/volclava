/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
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
#include "mbd.rsrclimit.h"
#include <sched.h>
#include <sys/prctl.h>

#define NL_SETN         10

#define SAFE_JID_GAP 0
#define NULL_RES     "NRR"
#define NULL_HOST    "NULL_HOST"
#define SKIPSPACE(sp)      while (isspace(*(sp))) (sp)++;

time_t eventTime;

extern bool_t          logMapFileEnable;

extern int              sigNameToValue_ (char *sigString);
extern char            *getLsbSigSymbol ( int );
extern char            *getSigSymbol(int);
extern void             chanFreeStashedBuf(struct Buffer *);
extern int              chanAllocBuf_(struct Buffer **, int);
static int              replay_event(char *, int);
static struct jData *   checkJobInCore(LS_LONG_INT jobId);

static int              replay_newjob(char *, int);
static int              replay_startjob(char *, int, int);
static int              replay_executejob(char *, int);
static int              replay_startjobaccept(char *, int);
static int              replay_newstat(char *, int);
static int              replay_switchjob(char *, int);
static int              replay_movejob(char *, int);
static int              replay_qc(char *, int);
static int              replay_hostcontrol(char *, int);
static int              replay_mbdDie(char *, int);
static int              replay_unfulfill(char *, int);
static int              replay_loadIndex(char *, int);
static int              replay_chkpnt(char *, int);
static int              replay_mig(char *, int);
static int              replay_mbdStart(char *, int);
static int              replay_modifyjob(char *, int);
static int              replay_modifyjob2(char *, int);

static struct jData    *replay_jobdata(char *, int, char *);
static int              replay_signaljob(char *, int);
static int              replay_jobmsg(char *, int);
static int              replay_jobmsgack(char *, int);
static int              replay_jobsigact(char *, int);
static int              replay_jobrequeue(char *, int);
static int              replay_cleanjob(char *, int);
static bool_t           replay_jobforce(char *, int);
static int              replay_logSwitch(char *, int);
static int              replay_jobattrset(char *, int );
static hEnt*            replay_jobEffeResReq(char * resreq);

static int replay_logSwitch(char *, int);
extern bool_t memberOfVacateList(struct lsQueueEntry *, struct lsQueue *);

static void             logFinishedjob(struct jData *);
static void             log_loadIndex(void);
static void             ckSchedHost (void);
static int              checkJobStarter(char *, char *);

static FILE            *log_fp     = NULL;
static FILE            *joblog_fp  = NULL;
static int              openEventFile(char *);
static int              putEventRec(char *);
static int              putEventRecTime(char *, time_t);
static int              putEventRec1(char *);
static int              log_jobdata(struct jData *, char *, int);
static int              createEvent0File(void);
static int              renameElogFiles(void);
static int              createAcct0File(void);

void                    log_timeExpired(int, time_t);
static int canSwitch(struct eventRec *, struct jData *);
static char *instrJobStarter1(char *, int, char *, char *, char *);
int                     nextJobId_t = 1;
time_t                  dieTime;
static char             elogFname[MAXFILENAMELEN];
static char             jlogFname[MAXFILENAMELEN];

/*
 * The event log switch is done by a child process.  The parent
 * routes new events to lsb.events.delta and forks; the child builds
 * lsb.events.0 and the new baseline lsb.events.tmp, rotates the archives
 * and rebuilds the index; the parent then appends the delta to .tmp and
 * publishes it as lsb.events.
 *
 * The state of an interrupted round is derived from the files alone,
 * because their order is fixed:
 *
 *   1. lsb.events.tmp is always created before lsb.events.delta.
 *   2. lsb.events.0 is written before any lsb.events.tmp content and
 *      disappears only in the very last rename of the rotation.
 *   3. lsb.events.tmp is complete on disk - its first line carries the
 *      history offset instead of the placeholder - strictly before the
 *      first archive rename.
 *   4. switchLogPublish() switches the write route back to lsb.events
 *      before it unlinks lsb.events.delta.
 *
 * From those four facts:
 *
 *   - a placeholder header (no digits) on .tmp  => baseline unfinished
 *   - digits in the first line of .tmp          => the offset is in place
 *   - .0 exists                                 => rotation unfinished
 *   - no .tmp but a leftover .delta             => the publish succeeded
 *
 * Only the length of the merge target before the delta was appended
 * (.tmp going forward, lsb.events on a rollback) cannot be derived; it is
 * the second number in the never published delta header.  See
 * switchLogRecover().
 */

/* exit codes of the switch child, internal convention only */
#define SWITCH_CHILD_EXIT_OK            0
#define SWITCH_CHILD_EXIT_FAIL          1

/* state of the switch, only changed by the main loop */
#define SWITCH_IDLE                     0
#define SWITCH_CHILD_RUNNING            1
#define SWITCH_STALLED                  2

/*
 * What the caller of switchLogRecover() expects to find.  Only the files
 * decide the action; a mismatch is just reported louder.
 */
#define RECOVER_EXPECT_ANY              0   /* startup / retry: no expectation */
#define RECOVER_EXPECT_COMPLETE         1   /* child exited 0: complete, no .0 */
#define RECOVER_EXPECT_INTERRUPTED      2   /* child failed: anything partial */

/* a switch child running longer than this, in seconds, is killed as
 * hung (typically a blocked NFS call); a normal run takes seconds to
 * a few minutes */
#define SWITCH_CHILD_MAX_RUNTIME        1200

/* while the switch child runs, report it every this many seconds */
#define SWITCH_REMIND_RUNNING           600


/* a parked round is retried on its own this often, in seconds */
#define SWITCH_RETRY_INTERVAL           600

/* how often a killed child that still has not exited is reported, in seconds */
#define SWITCH_REMIND_INTERVAL          1200

/* header of a baseline being built; replaced by the history offset */
#define ELOG_BASELINE_HEADER    "#                                     \n"

/* readHeaderNumbers(): the file could not be opened or read */
#define ELOG_HEADER_UNREADABLE  (-2)

static char             tmpElogFname[MAXFILENAMELEN];   /* lsb.events.tmp: new baseline under construction */
static char             event0Fname[MAXFILENAMELEN];     /* lsb.events.0: archive segment before rotation */
static char             deltaElogFname[MAXFILENAMELEN];  /* lsb.events.delta: events logged while a child runs */

/*
 * The single event write route: while a switch is in flight every new
 * event goes to lsb.events.delta, never to both files.
 */
static char            *elogWriteTarget = elogFname;

static int              switchState = SWITCH_IDLE;  /* SWITCH_*, only changed by the main loop */
static time_t           switchStartTime;    /* frozen before the fork */
static int              switchChildKillSent;  /* watchdog already killed this child */
static pid_t            switchIndexPid;       /* index rebuild child, 0 when none */
static pid_t            eventIndexRebuildPid; /* same, for the synchronous switch_log() */
static int              switchLogStatErrno;   /* errno when the log directory could not be checked, 0 otherwise */
static time_t           lastSwitchDoneTime;   /* when the last switch completed; startup counts as one */
static pid_t            switchParentPid;      /* switch child only: pid of the mbatchd that forked it */

/*
 * Filled in by the SIGCHLD handler.  switchChildPid is only written by
 * the main loop while SIGCHLD is blocked; switchChildDone is written
 * last, so a set flag implies a valid status.
 */
static volatile pid_t          switchChildPid = 0;
static volatile sig_atomic_t   switchChildDone = 0;
static volatile sig_atomic_t   switchChildStatus = 0;

static int              switchLogRecover(int);
static void             switchLogSuspendRound(void);
static void             switchLogReplayDelta(void);
static void             switchChildCheckParent(void);
static int              forkSwitchLog(void);
static int              switchLogBeginRound(void);
static void             switchLogChildMain(void);
static void             switchChildAbort(int);
static int              readHeaderNumbers(const char *, long *, long *);
static int              readDeltaMergeOffset(const char *, off_t *);
static void             switchLogUndoRollback(void);
static int              writeDeltaMergeOffset(off_t);
static int              mergeDeltaEvents(const char *, off_t, off_t *);
static off_t            lastLineEnd(int, off_t);
static int              trimPartialRecord(const char *, off_t *);
static int              switchLogDropRound(void);
static int              switchLogRollback(void);
static int              switchLogRollForward(int);
static int              switchLogPublish(const char *);
static int              writeBaselineEvents(FILE *, FILE *, char *, int);
static int              writeEventRec(FILE *, struct eventRec *, int, time_t);
static int              switchLogCountArchives(void);
static void             switchLogRebuildIndex(void);
static void             switchLogSyncDir(void);

static struct eventRec *logPtr;

static int              logLoadIndex = TRUE;


extern int sigNameToValue_( char *);

extern float version;

struct hostCtrlEvent {
    time_t                time;
    struct hostCtrlLog    *hostCtrlLog;
};

static hTab    hostCtrlEvents;

static void    initHostCtrlTable(void);
static void    destroyHostCtrlTable(void);
static void    destroyHostCtrlEvent(void *);
static void    saveHostCtrlEvent(struct hostCtrlLog *, const time_t);
static void    writeHostCtrlEvent(FILE *);
static void    log_hostStatusAtSwitch(const struct hostCtrlEvent *);

struct queueCtrlEvent {
    time_t                 time;
    struct queueCtrlLog    *queueCtrlLog;
};

static hTab    closedQueueEvents;
static hTab    inactiveQueueEvents;

static void    initQueueCtrlTable(void);
static void    destroyQueueCtrlTable(void);
static void    destroyQueueCtrlEvent(void *);
static void    saveQueueCtrlEvent(struct queueCtrlLog *, const time_t);
static void    queueTableUpdate(hTab *, struct queueCtrlLog *);
static void    writeQueueCtrlEvent(FILE *);
static void    log_queueStatusAtSwitch(const struct queueCtrlEvent *);

extern time_t lsb_getAcctFileTime(char *);

static char    *getSignalSymbol(const struct signalReq *);
static int     replay_arrayrequeue(struct jData *,
                                   const struct signalLog *);
static int renameAcctLogFiles(int);

int
init_log(void)
{
    static char    fname[] = "init_log";
    char           first = TRUE;
    int            ConfigError = 0;
    int            lineNum = 0;
    int            list;
    struct jData   *jp;
    char           dirbuf[MAXPATHLEN];
    char           infoDir[MAXPATHLEN];
    struct stat    sbuf;
    struct stat ebuf;

    mSchedStage = M_STAGE_REPLAY;

    sprintf(elogFname, "%s/logdir/lsb.events",
            daemonParams[LSB_SHAREDIR].paramValue);

    sprintf(jlogFname, "%s/logdir/lsb.acct",
            daemonParams[LSB_SHAREDIR].paramValue);

    sprintf(tmpElogFname, "%s/logdir/lsb.events.tmp",
            daemonParams[LSB_SHAREDIR].paramValue);
    sprintf(event0Fname, "%s/logdir/lsb.events.0",
            daemonParams[LSB_SHAREDIR].paramValue);
    sprintf(deltaElogFname, "%s/logdir/lsb.events.delta",
            daemonParams[LSB_SHAREDIR].paramValue);

    sprintf(dirbuf, "%s/logdir", daemonParams[LSB_SHAREDIR].paramValue);

    if (stat(dirbuf, &sbuf) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat",
                  dirbuf);
        if (!lsb_CheckMode) {
            mbdDie(MASTER_FATAL);
        }

        ConfigError = -1;
    }


    if (sbuf.st_uid != managerId) {
        ls_syslog(LOG_ERR, "\
%s: Log directory %s not owned by LSF administrator: %s/%d (directory owner ID is %d)",
                  fname,
                  dirbuf,
                  lsbManager,
                  managerId,
                  (int)sbuf.st_uid);
        if (debug == 0) {
            mbdDie(MASTER_FATAL);
        }

        ConfigError = -1;
    }

    if (sbuf.st_mode & 02) {

        ls_syslog(LOG_ERR, "\
%s: Mode <%03o> not allowed for job log directory <%s>; the permission bits for others should be 05",
                  fname,
                  (int)sbuf.st_mode,
                  dirbuf);
        if (debug == 0) {
            mbdDie(MASTER_FATAL);
        }

        ConfigError = -1;
    }

    getElogLock();

    /*
     * Finish a switch a previous mbatchd left unfinished: with the event log
     * lock held, before the replay (which only reads lsb.events) and before
     * the synchronous switch below can overwrite its files.  If the log
     * directory cannot even be checked, do not serve on an unknown state:
     * retry a few times, then exit and let sbatchd restart mbatchd.
     */
    switchLogRecover(RECOVER_EXPECT_ANY);
    for (list = 0; switchLogStatErrno != 0 && list < 3; list++) {
        sleep(5);
        switchLogRecover(RECOVER_EXPECT_ANY);
    }
    if (switchLogStatErrno != 0) {
        ls_syslog(LOG_ERR, "%s: Fail to access the log directory <%s>: %s",
                  fname, dirbuf, strerror(switchLogStatErrno));
        mbdDie(MASTER_FATAL);
    }

    chuser(managerId);

    sprintf(infoDir, "%s/logdir/info",
            daemonParams[LSB_SHAREDIR].paramValue);

    if (mkdir(infoDir, 0700) == -1 && errno != EEXIST) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "mkdir",
                  infoDir);
        mbdDie(MASTER_FATAL);
    }

    stat(elogFname, &ebuf);
    log_fp = fopen(elogFname, "r");

    chuser(batchId);

    if (log_fp != NULL) {

        if (lsberrno == LSBE_EOF)
            lsberrno = LSBE_NO_ERROR;


        while (lsberrno != LSBE_EOF) {
            if ((logPtr = lsb_geteventrec(log_fp, &lineNum)) == NULL) {

                if (lsberrno != LSBE_EOF) {
                    ls_syslog(LOG_ERR, "\
%s: Reading event file <%s> at line <%d>: %s",
                              fname,
                              elogFname,
                              lineNum,
                              lsb_sysmsg());
                    first = FALSE;
                    if (lsberrno == LSBE_NO_MEM) {

                        mbdDie(MASTER_MEM);
                    }

                } else {
                    FCLOSEUP(&log_fp);
                    log_fp = NULL;
                }
                continue;
            }

            eventTime = logPtr->eventTime;
            if (!replay_event(elogFname, lineNum) && first) {
                ls_syslog(LOG_ERR, "\
%s: File %s at line %d: First replay_event() failed; line ignored",
                          fname,
                          elogFname,
                          lineNum);
                first = FALSE;
            }
        }

        if (log_fp)
            FCLOSEUP(&log_fp);

        /* events of a round whose recovery failed above, see there */
        switchLogReplayDelta();

        for (list = SJL; list <= PJL; list++) {

            if (list != SJL && list != MJL && list != PJL)
                continue;

            for (jp = jDataList[list]->back;
                 jp != jDataList[list];
                 jp = jp->back) {

                int svJStatus = jp->jStatus;
                int i;
                int num;

                num = jp->shared->jobBill.maxNumProcessors;

                jp->jStatus = JOB_STAT_PEND;

                updQaccount (jp, num, num, 0, 0, 0, 0);
                updUserData (jp, num, num, 0, 0, 0, 0);

                jp->jStatus = svJStatus;
                if (jp->jStatus & JOB_STAT_PEND)
                    continue;

                updCounters (jp, JOB_STAT_PEND, !LOG_IT);

                if ((jp->shared->jobBill.options & SUB_EXCLUSIVE)
                    && IS_START (jp->jStatus)) {
                    for (i = 0; i < jp->numHostPtr; i ++)
                        jp->hPtr[i]->hStatus |= HOST_STAT_EXCLUSIVE;
                }

                if (IS_START(jp->jStatus)) {
                    proxyUSJLAddEntry(jp);
                    proxyHSJLAddEntry(jp);
                    updRLAccount4Job(jp, NULL);
                    updHostLeftRusageMem(jp, -1);
                }
            }
        }

        if (logclass & LC_JGRP)
            printTreeStruct(treeFile);

        if (nextJobId == 1) {
            nextJobId = nextJobId_t + SAFE_JID_GAP;
            if (nextJobId >= maxJobId)
                nextJobId = 1;
        }
    }

    TIMEIT(0, recoverShareAccts(), "init_log/rscoverShareAccts()");

    /*
     * Records lsb.events cannot replay are rewritten away right now,
     * before any new event is appended to a cut off last line; the
     * cleaned jobs counted by the replay are consumed by this switch.
     */
    if (!first) {

        if (switch_log() == -1)
            ConfigError = -1;
        else
            numRemoveJobs = 0;
    }

    switchELog();
    if (logLoadIndex)
        log_loadIndex();

    ckSchedHost();

    mSchedStage = 0;

    checkAcctLog();

    return ConfigError;
}

static int
replay_event(char *filename, int lineNum)
{
    static char fname[] = "replay_event()";

    switch (logPtr->type) {
        case EVENT_JOB_NEW:
            return (replay_newjob(filename, lineNum));
        case EVENT_PRE_EXEC_START:
            return (replay_startjob(filename, lineNum, TRUE));
        case EVENT_JOB_START:
            return (replay_startjob(filename, lineNum, FALSE));
        case EVENT_JOB_STATUS:
            return (replay_newstat(filename, lineNum));
        case EVENT_JOB_SWITCH:
            return (replay_switchjob(filename, lineNum));
        case EVENT_JOB_MOVE:
            return (replay_movejob(filename, lineNum));
        case EVENT_QUEUE_CTRL:
            return (replay_qc(filename, lineNum));
        case EVENT_HOST_CTRL:
            return (replay_hostcontrol(filename, lineNum));
        case EVENT_MBD_UNFULFILL:
            return (replay_unfulfill(filename, lineNum));
        case EVENT_MBD_START:
            return (replay_mbdStart(filename, lineNum));
        case EVENT_MBD_DIE:
            return (replay_mbdDie(filename, lineNum));
        case EVENT_LOAD_INDEX:
            return (replay_loadIndex(filename, lineNum));
        case EVENT_CHKPNT:
            return (replay_chkpnt(filename, lineNum));
        case EVENT_MIG:
            return (replay_mig(filename, lineNum));
        case EVENT_JOB_MODIFY:
            return (replay_modifyjob(filename, lineNum));
        case EVENT_JOB_MODIFY2:
            return (replay_modifyjob2(filename, lineNum));
        case EVENT_JOB_ATTR_SET:
            return (replay_jobattrset(filename, lineNum));
        case EVENT_JOB_SIGNAL:
            return (replay_signaljob(filename, lineNum));
        case EVENT_JOB_EXECUTE:
            return (replay_executejob(filename, lineNum));
        case EVENT_JOB_START_ACCEPT:
            return (replay_startjobaccept(filename, lineNum));
        case EVENT_JOB_MSG:
            return (replay_jobmsg(filename, lineNum));
        case EVENT_JOB_MSG_ACK:
            return (replay_jobmsgack(filename, lineNum));
        case EVENT_JOB_SIGACT:
            return (replay_jobsigact(filename, lineNum));
        case EVENT_JOB_REQUEUE:
            return (replay_jobrequeue(filename, lineNum));
        case EVENT_JOB_CLEAN:
            return (replay_cleanjob(filename, lineNum));
        case EVENT_JOB_FORCE:
            return (replay_jobforce(filename, lineNum));
        case EVENT_LOG_SWITCH:
            return (replay_logSwitch(filename, lineNum));
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6708,
                                             "%s: File %s at line %d: Invalid event_type <%c>"), /* catgets 6708 */
                      fname,
                      filename,
                      lineNum,
                      logPtr->type);
            return (FALSE);
    }
}

static int
replay_newjob(char *filename, int lineNum)
{
    static char      fname[] = "replay_newjob";
    struct jData     *job;
    struct idxList   *idxList;
    int              error;
    int              maxJLimit = 0;

    job = replay_jobdata(filename, lineNum, fname);

    if ((job->shared->jobBill.options & SUB_RESTART) ||
        (idxList = parseJobArrayIndex(job->shared->jobBill.jobName,
                                      &error,
                                      &maxJLimit)) == NULL) {
        handleNewJob(job, JOB_REPLAY, logPtr->eventTime);
    } else {
        handleNewJobArray(job, idxList, maxJLimit);
        freeIdxList(idxList);
    }

    nextJobId_t = (int)job->jobId + 1;

    if (nextJobId_t >= maxJobId)
        nextJobId_t = 1;

    return TRUE;
}

static int
replay_switchjob(char *filename, int lineNum)
{
    static char             fname[] = "replay_switchjob";
    struct jobSwitchReq     switchReq;
    struct qData           *qfp, *qtp;
    struct jData           *jp;

    switchReq.jobId = LSB_JOBID(logPtr->eventLog.jobSwitchLog.jobId,
                                logPtr->eventLog.jobSwitchLog.idx);
    strcpy(switchReq.queue, logPtr->eventLog.jobSwitchLog.queue);


    if ((jp = getJobData(switchReq.jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%s> not found in job list "), /* catgets 6709 */
                  fname, filename, lineNum, lsb_jobid2str(switchReq.jobId));
        return (FALSE);
    }
    qfp = jp->qPtr;


    qtp = getQueueData(switchReq.queue);
    if (qtp == NULL) {
        ls_syslog(LOG_DEBUG, "replay_switchjob: File %s at line %d: Destination queue <%s> not found, switching job <%s> to queue <%s>", filename, lineNum, switchReq.queue, lsb_jobid2str(switchReq.jobId), LOST_AND_FOUND);
        qtp = getQueueData(LOST_AND_FOUND);
        if (qtp == NULL)
            qtp = lostFoundQueue();
    }
    jobInQueueEnd(jp, qtp);

    if ( qfp != qtp ) {
        if ((jp->shared->jobBill.options & SUB_CHKPNT_DIR )
            && !(jp->shared->jobBill.options2 & SUB2_QUEUE_CHKPNT)) {


        } else {


            if ( qfp->qAttrib & Q_ATTRIB_CHKPNT ) {
                jp->shared->jobBill.options  &= ~ SUB_CHKPNT_DIR;
                jp->shared->jobBill.options  &= ~ SUB_CHKPNT_PERIOD;
                jp->shared->jobBill.options2 &= ~ SUB2_QUEUE_CHKPNT;
            }

            FREEUP(jp->shared->jobBill.chkpntDir);

            if ( qtp->qAttrib & Q_ATTRIB_CHKPNT ) {

                char dir[MAXPATHLEN];
                sprintf(dir, "%s/%s", qtp->chkpntDir, lsb_jobid2str(jp->jobId));
                jp->shared->jobBill.chkpntDir = safeSave(dir);
                jp->shared->jobBill.chkpntPeriod = qtp->chkpntPeriod;
                jp->shared->jobBill.options  |= SUB_CHKPNT_DIR;
                jp->shared->jobBill.options  |= SUB_CHKPNT_PERIOD;
                jp->shared->jobBill.options2 |= SUB2_QUEUE_CHKPNT;
            }
            else
                jp->shared->jobBill.chkpntDir = safeSave("");

        }
    }

    if (qfp != qtp) {
        if ((jp->shared->jobBill.options & SUB_RERUNNABLE )
            && !(jp->shared->jobBill.options2 & SUB2_QUEUE_RERUNNABLE)) {

        } else {

            jp->shared->jobBill.options  &= ~ SUB_RERUNNABLE;
            jp->shared->jobBill.options2 &= ~ SUB2_QUEUE_RERUNNABLE;


            if ( qtp->qAttrib & Q_ATTRIB_RERUNNABLE ) {
                jp->shared->jobBill.options  |= SUB_RERUNNABLE;
                jp->shared->jobBill.options2 |= SUB2_QUEUE_RERUNNABLE;
            }
        }
    }

    return (TRUE);

}

static int
replay_cleanjob(char *filename, int lineNum)
{
    LS_LONG_INT  jobId;

    jobId = LSB_JOBID(logPtr->eventLog.jobCleanLog.jobId,
                      logPtr->eventLog.jobCleanLog.idx);
    removeJob(jobId);
    return(TRUE);
}

static int
replay_movejob(char *filename, int lineNum)
{
    static char             fname[] = "replay_movejob";
    struct jobMoveReq       jobMoveReq;
    int                     reply;
    struct lsfAuth          auth;

    (void)memset((void *)&auth, '\0', sizeof (struct lsfAuth));

    auth.uid = logPtr->eventLog.jobMoveLog.userId;
    strcpy(auth.lsfUserName, logPtr->eventLog.jobMoveLog.userName);

    jobMoveReq.jobId = LSB_JOBID(logPtr->eventLog.jobMoveLog.jobId,
                                 logPtr->eventLog.jobMoveLog.idx);
    jobMoveReq.position = logPtr->eventLog.jobMoveLog.position;
    jobMoveReq.opCode = logPtr->eventLog.jobMoveLog.base;

    reply = moveJobArray(&jobMoveReq, FALSE, &auth);
    switch (reply) {
        case LSBE_NO_ERROR:
            return (TRUE);
        case LSBE_NO_JOB:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6711,
                                             "%s: File %s at line %d: Cannot move job <%s>: job not found"), fname, filename, lineNum, lsb_jobid2str(jobMoveReq.jobId)); /* catgets 6711 */
            return (FALSE);
        case LSBE_PERMISSION:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6712,
                                             "%s: File %s at line %d: Cannot move job <%s>: not operated by owner or manager"), fname, filename, lineNum, lsb_jobid2str(jobMoveReq.jobId)); /* catgets 6712 */
            return (FALSE);
        case LSBE_JOB_STARTED:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6713,
                                             "%s: File %s at line %d: Cannot move job <%s>: job already started"), fname, filename, lineNum, lsb_jobid2str(jobMoveReq.jobId)); /* catgets 6713 */
            return (FALSE);
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6714,
                                             "%s: File %s at line %d: Wrong return code <%d> from movejob() for job <%s>"), fname, filename, lineNum, reply, lsb_jobid2str(jobMoveReq.jobId)); /* catgets 6714 */
            return (FALSE);
    }


}

static int
replay_startjob(char *filename, int lineNum, int preExecStart)
{
    struct jData job;
    struct jData *jp;
    struct jShared shared;
    int i;

    job.jobId = LSB_JOBID(logPtr->eventLog.jobStartLog.jobId,
                          logPtr->eventLog.jobStartLog.idx);
    job.jStatus = logPtr->eventLog.jobStartLog.jStatus;
    job.startTime = logPtr->eventTime;
    job.jobPid = logPtr->eventLog.jobStartLog.jobPid;
    job.jobPGid = logPtr->eventLog.jobStartLog.jobPGid;
    job.queuePreCmd = logPtr->eventLog.jobStartLog.queuePreCmd;
    job.queuePostCmd = logPtr->eventLog.jobStartLog.queuePostCmd;
    job.numHostPtr = logPtr->eventLog.jobStartLog.numExHosts;

    job.shared = &shared;

    if ((jp = getJobData(job.jobId)) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: Job %s not found in job list",
                  __func__, lsb_jobid2str(job.jobId));
        return FALSE;
    }

    if (job.numHostPtr > 0) {
        job.hPtr = my_calloc(job.numHostPtr,
                             sizeof(struct hData *), __func__);
    } else {
        ls_syslog(LOG_ERR, "\
%s: File %s at line %d: No execution hosts for job %s ??",
                  __func__,
                  filename,
                  lineNum,
                  lsb_jobid2str(job.jobId));
        return FALSE;
    }

    for (i = 0; i < job.numHostPtr; i++) {

        job.hPtr[i] = getHostData(logPtr->eventLog.jobStartLog.execHosts[i]);
        if (job.hPtr[i] == NULL) {
            ls_syslog(LOG_WARNING, "\
%s: Execution host %s not found; saving job %s to host %s",
                      __func__,
                      logPtr->eventLog.jobStartLog.execHosts[i],
                      lsb_jobid2str(job.jobId),
                      LOST_AND_FOUND);
            job.hPtr[i] = getHostData(LOST_AND_FOUND);
            assert(job.hPtr[i]);
        }
    }

    jp->startTime = job.startTime;
    jp->jobPid = job.jobPid;
    jp->jobPGid = job.jobPGid;
    jp->oldReason = 0;
    jp->newReason = 0;
    jp->subreasons = 0;
    jp->numHostPtr = job.numHostPtr;
    if (jp->hPtr)
        FREEUP(jp->hPtr);
    jp->hPtr = job.hPtr;

    if (!preExecStart
        && (jp->shared->jobBill.options & SUB_PRE_EXEC)
        && (jp->jStatus & JOB_STAT_PRE_EXEC)) {
        jp->jStatus &= ~JOB_STAT_PRE_EXEC;
        return (TRUE);
    }

    if (job.queuePreCmd && job.queuePreCmd[0] != '\0')
        jp->queuePreCmd = safeSave(job.queuePreCmd);
    if (job.queuePostCmd && job.queuePostCmd[0] != '\0')
        jp->queuePostCmd = safeSave(job.queuePostCmd);

    if (!jp->effeResReqEnt && !preExecStart) {
        jp->effeResReqEnt = replay_jobEffeResReq(logPtr->eventLog.jobStartLog.effeResReq);
        if (jp->effeResReqEnt == NULL) {
            /*For compatibility, when event version is earlier than 22, there is no effective
            *resreq string in JOB_START event, we use merged resreq as effective one instead.
            */
            mkJobEffeResReqEntry(jp);
        }
    }
    jStatusChange(jp, job.jStatus, logPtr->eventTime, "replay_startjob");

    if (preExecStart)
        jp->jStatus |= JOB_STAT_PRE_EXEC;

    return TRUE;
}

static int
replay_executejob(char *filename, int lineNum)
{
    static char            fname[] = "replay_executejob";
    struct jobExecuteLog   *jobExecuteLog;
    struct jData           *jp;
    LS_LONG_INT            jobId;

    jobExecuteLog = &logPtr->eventLog.jobExecuteLog;

    jobId = LSB_JOBID(jobExecuteLog->jobId, jobExecuteLog->idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6715,
                                         "%s: Job <%d> not found in job list"),
                  fname, jobExecuteLog->jobId);
        return (FALSE);
    }
    jp->execUid = jobExecuteLog->execUid;
    jp->jobPGid = jobExecuteLog->jobPGid;


    if (jobExecuteLog->jobPid != 0)
        jp->jobPid = jobExecuteLog->jobPid;

    if (jp->execHome)
        FREEUP(jp->execHome);
    jp->execHome = safeSave(jobExecuteLog->execHome);

    if (jp->execCwd)
        FREEUP(jp->execCwd);
    jp->execCwd = safeSave(jobExecuteLog->execCwd);

    if (jp->execUsername)
        FREEUP(jp->execUsername);
    jp->execUsername = safeSave(jobExecuteLog->execUsername);

    return (TRUE);

}

static int
replay_startjobaccept(char *filename, int lineNum)
{
    static char             fname[] = "replay_startjobaccept";
    struct jobStartAcceptLog   *jobStartAcceptLog;
    struct jData           *jp;
    LS_LONG_INT            jobId;
    jobStartAcceptLog = &logPtr->eventLog.jobStartAcceptLog;
    jobId = LSB_JOBID(jobStartAcceptLog->jobId, jobStartAcceptLog->idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6715,
                                         "%s: Job <%d> not found in job list"),
                  fname, jobStartAcceptLog->jobId);
        return (FALSE);
    }

    jp->jobPGid = jobStartAcceptLog->jobPGid;
    jp->jobPid = jobStartAcceptLog->jobPid;
    if ((jp->jgrpNode->nodeType == JGRP_NODE_ARRAY) &&
        (ARRAY_DATA(jp->jgrpNode)->jobArray->startTime == 0)){
        ARRAY_DATA(jp->jgrpNode)->jobArray->startTime = jp->startTime;
    }
    return (TRUE);

}


static int
replay_newstat(char *filename, int lineNum)
{
    static char             fname[] = "replay_newstat";
    struct jData           *jp, *job;
    struct jobStatusLog    *newStat;

    newStat = &logPtr->eventLog.jobStatusLog;

    if ((jp = getJobData(LSB_JOBID(newStat->jobId, newStat->idx))) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list "),
                  fname, filename, lineNum, newStat->jobId);
        return (FALSE);
    }


    if ((IS_POST_DONE(newStat->jStatus))||(IS_POST_ERR(newStat->jStatus))) {
        jp->jStatus = newStat->jStatus;
        jp->endTime = newStat->endTime;
        if ((jp->jgrpNode->nodeType == JGRP_NODE_ARRAY) &&
            ARRAY_DATA(jp->jgrpNode)->counts[JGRP_COUNT_NJOBS] ==
            (ARRAY_DATA(jp->jgrpNode)->counts[JGRP_COUNT_NDONE] +
             ARRAY_DATA(jp->jgrpNode)->counts[JGRP_COUNT_NEXIT])) {
            ARRAY_DATA(jp->jgrpNode)->jobArray->endTime = jp->endTime;
        }
        return TRUE;
    }

    if (newStat->jStatus & JOB_STAT_UNKWN) {
        jp->jStatus |= JOB_STAT_UNKWN;
        return TRUE;
    } else
        jp->jStatus &= ~JOB_STAT_UNKWN;

    if (IS_PEND(jp->jStatus) && IS_START(newStat->jStatus)) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6720,
                                         "replay_newstat: Status of job <%s> transited from <%d> to <%d> without JOB_START event record; ignored"), /* catgets 6720 */
                  lsb_jobid2str(jp->jobId),
                  jp->jStatus,
                  newStat->jStatus);
        return (TRUE);
    }
    if (newStat->reason != EXIT_ZOMBIE_JOB) {
        jp->newReason = newStat->reason;
        jp->oldReason = newStat->reason;
    }
    jp->subreasons = newStat->subreasons;
    if ((newStat->jStatus & JOB_STAT_SSUSP)
        && !(jp->jStatus & JOB_STAT_SSUSP))
        jp->ssuspTime = logPtr->eventTime;


    if (newStat->jStatus & (JOB_STAT_EXIT)) {
        if (newStat->reason & (EXIT_ZOMBIE | EXIT_KILL_ZOMBIE)) {
            jp->jStatus |= JOB_STAT_ZOMBIE;
            inZomJobList(jp, FALSE);
        } else if (newStat->reason & EXIT_RERUN) {
            jp->newReason = EXIT_RERUN;
            jp->oldReason = EXIT_RERUN;
        }
        else if (newStat->reason & EXIT_ZOMBIE_JOB) {
            if ((job = getZombieJob(jp->jobId)) != NULL) {
                offList((struct listEntry *) job);
                freeJData(job);
                if (getZombieJob(jp->jobId) != NULL)
                    return (TRUE);
            }

            if (jp->newReason == EXIT_KILL_ZOMBIE &&
                (jp->jStatus & JOB_STAT_ZOMBIE)) {
                jp->jStatus &= ~JOB_STAT_ZOMBIE;
                jp->newReason = 0;
                jp->oldReason = 0;
            }
            if ((jp->shared->jobBill.options & SUB_RERUNNABLE)) {
                return (TRUE);
            }
        } else if ( newStat->reason & EXIT_RESTART ) {
            jp->shared->jobBill.options |= SUB_RESTART | SUB_RESTART_FORCE;
        } else
            jp->jStatus &= ~JOB_STAT_ZOMBIE;

    }

    if (IS_SUSP(newStat->jStatus) && IS_START(newStat->jStatus)) {
        if (!(jp->newReason & SUSP_MBD_LOCK)
            && shouldLockJob (jp, newStat->jStatus)) {
            jp->newReason = newStat->reason | SUSP_MBD_LOCK;
            jp->oldReason = newStat->reason | SUSP_MBD_LOCK;
        }
    }
    else if (IS_FINISH(newStat->jStatus)) {
        jp->endTime = newStat->endTime;
        jp->cpuTime = newStat->cpuTime;
        jp->exitStatus = newStat->exitStatus;
        jp->maxMem = newStat->maxMem;
        jp->avgMem = newStat->avgMem;
    }

    jStatusChange(jp, newStat->jStatus, logPtr->eventTime, "replay_newstat");


    if (IS_PEND(newStat->jStatus) && newStat->ru && !IS_FINISH(jp->jStatus)) {
        if (!jp->lsfRusage)
            jp->lsfRusage = (struct lsfRusage *)
                my_malloc(sizeof(struct lsfRusage), fname);
        memcpy((char *) jp->lsfRusage, (char *) &newStat->lsfRusage,
               sizeof(struct lsfRusage));
        jp->cpuTime = newStat->cpuTime;
    }



    return (TRUE);

}

static int
replay_qc(char *filename, int lineNum)
{
    static char             fname[] = "replay_qc";
    int                     opCode;
    struct qData           *qp;

    opCode = logPtr->eventLog.queueCtrlLog.opCode;
    if ((qp = getQueueData(logPtr->eventLog.queueCtrlLog.queue)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6722,
                                         "%s: File %s at line %d: Queue <%s> not found in queue list "), fname, filename, lineNum, logPtr->eventLog.queueCtrlLog.queue); /* catgets 6722 */
        return (FALSE);
    }
    if (opCode == QUEUE_OPEN)
        qp->qStatus |= QUEUE_STAT_OPEN;
    if (opCode == QUEUE_CLOSED)
        qp->qStatus &= ~QUEUE_STAT_OPEN;
    if (opCode == QUEUE_ACTIVATE)
        qp->qStatus |= QUEUE_STAT_ACTIVE;
    if (opCode == QUEUE_INACTIVATE)
        qp->qStatus &= ~QUEUE_STAT_ACTIVE;
    FREEUP(qp->actionComment);
    if (logPtr->eventLog.queueCtrlLog.msg && logPtr->eventLog.queueCtrlLog.msg[0] != '\0') {
        qp->actionComment = safeSave(logPtr->eventLog.queueCtrlLog.msg);
    }
    return (TRUE);

}

static int
replay_hostcontrol(char *filename, int lineNum)
{
    static char             fname[] = "replay_hostcontrol";
    int                     opCode;
    char                    host[MAXHOSTNAMELEN];
    struct hData           *hp;

    opCode = logPtr->eventLog.hostCtrlLog.opCode;
    strcpy(host, logPtr->eventLog.hostCtrlLog.host);

    hp = getHostData(host);
    if (hp == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6723,
                                         "%s: File %s at line %d: Host <%s> not found in host list "), fname, filename, lineNum, host); /* catgets 6723 */
        return (FALSE);
    }
    if (opCode == HOST_OPEN)
        hp->hStatus &= ~HOST_STAT_DISABLED;
    if (opCode == HOST_CLOSE)
        hp->hStatus |= HOST_STAT_DISABLED;
    FREEUP(hp->actionComment);
    if (logPtr->eventLog.hostCtrlLog.msg && logPtr->eventLog.hostCtrlLog.msg[0] != '\0') {
        hp->actionComment = safeSave(logPtr->eventLog.hostCtrlLog.msg);
    }

    return (TRUE);

}

static int
replay_mbdStart(char *filename, int lineNum)
{
    int                     list;
    struct jData           *jpbw;

    for (list = 0; list < NJLIST; list++) {
        for (jpbw = jDataList[list]->back; jpbw != jDataList[list];
             jpbw = jpbw->back) {
            jpbw->pendEvent.notSwitched = 0;
            jpbw->pendEvent.sig = SIG_NULL;
            jpbw->pendEvent.sig1 = SIG_NULL;
            jpbw->pendEvent.sig1Flags = 0;
            jpbw->pendEvent.notModified = 0;

            eventPending = FALSE;

        }
    }
    return (TRUE);

}

static int
replay_mbdDie(char *filename, int lineNum)
{
    dieTime = logPtr->eventTime;
    return (TRUE);
}

static int
replay_unfulfill(char *filename, int lineNum)
{
    static char             fname[] = "replay_unfulfill";
    LS_LONG_INT             jobId;
    struct jData           *jp;

    jobId = LSB_JOBID(logPtr->eventLog.unfulfillLog.jobId,
                      logPtr->eventLog.unfulfillLog.idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%s> not found in job list "),
                  fname, filename, lineNum, lsb_jobid2str(jobId));
        return (FALSE);
    }

    jp->pendEvent.notSwitched = logPtr->eventLog.unfulfillLog.notSwitched;
    jp->pendEvent.sig = logPtr->eventLog.unfulfillLog.sig;
    jp->pendEvent.sig1 = logPtr->eventLog.unfulfillLog.sig1;
    jp->pendEvent.sig1Flags = logPtr->eventLog.unfulfillLog.sig1Flags;
    jp->shared->jobBill.chkpntPeriod = logPtr->eventLog.unfulfillLog.chkPeriod;
    jp->chkpntPeriod = logPtr->eventLog.unfulfillLog.chkPeriod;
    jp->pendEvent.notModified = logPtr->eventLog.unfulfillLog.notModified;

    if (jp->pendEvent.notSwitched ||
        jp->pendEvent.sig != SIG_NULL ||
        jp->pendEvent.sig1 != SIG_NULL ||
        jp->pendEvent.notModified)
        eventPending = TRUE;

    return (TRUE);
}

static int
replay_mig(char *filename, int lineNum)
{
    static char             fname[] = "replay_mig";
    struct jData           *jp;
    struct migLog          *migLog;
    int                     i;
    LS_LONG_INT             jobId;

    migLog = &logPtr->eventLog.migLog;

    jobId = LSB_JOBID(migLog->jobId, migLog->idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list"),
                  fname, filename, lineNum, migLog->jobId);
        return (FALSE);
    }

    if (jp->shared->jobBill.numAskedHosts) {
        for (i = 0; i < jp->shared->jobBill.numAskedHosts; i++)
            FREEUP (jp->shared->jobBill.askedHosts[i]);
        FREEUP (jp->shared->jobBill.askedHosts);
        FREEUP (jp->askedPtr);
        jp->numAskedPtr = 0;
    }
    jp->shared->jobBill.options &= ~SUB_HOST;
    jp->shared->jobBill.numAskedHosts = migLog->numAskedHosts;

    if (jp->shared->jobBill.numAskedHosts > 0) {
        int  numAskedHosts = 0;
        struct askedHost *askedHosts;
        int returnErr, badReqIndx, others;

        jp->shared->jobBill.askedHosts = (char **) my_calloc(migLog->numAskedHosts,
                                                             sizeof(char *), fname);
        for (i = 0; i < jp->shared->jobBill.numAskedHosts; i++)
            jp->shared->jobBill.askedHosts[i] = safeSave(migLog->askedHosts[i]);
        jp->shared->jobBill.options |= SUB_HOST;

        returnErr = chkAskedHosts(jp->shared->jobBill.numAskedHosts,
                                  jp->shared->jobBill.askedHosts,
                                  jp->shared->jobBill.numProcessors, &numAskedHosts,
                                  &askedHosts, &badReqIndx, &others, 0);
        if (returnErr == LSBE_NO_ERROR) {
            if (numAskedHosts  > 0) {
                jp->askedPtr = (struct askedHost *) my_calloc (numAskedHosts,
                                                               sizeof(struct askedHost), fname);
                for (i = 0; i < numAskedHosts; i++) {
                    jp->askedPtr[i].hData = askedHosts[i].hData;
                    jp->askedPtr[i].priority = askedHosts[i].priority;
                }
            }
            jp->numAskedPtr = numAskedHosts;
            jp->askedOthPrio = others;
            FREEUP(askedHosts);
        }
    }

    jp->shared->jobBill.restartPid = jp->jobPid;
    jp->restartPid = jp->jobPid;

    return (TRUE);
}
static int
replay_jobsigact(char *filename, int lineNum)
{
    static char             fname[] = "replay_jobsigact";
    struct jData           *jp;
    int                     newActPid;
    LS_LONG_INT             jobId;

    jobId = LSB_JOBID(logPtr->eventLog.sigactLog.jobId,
                      logPtr->eventLog.sigactLog.idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list"),
                  fname, filename, lineNum,
                  logPtr->eventLog.sigactLog.jobId);
        return (FALSE);
    }
    jp->newReason = logPtr->eventLog.sigactLog.reasons;
    jp->oldReason = logPtr->eventLog.sigactLog.reasons;
    jp->shared->jobBill.chkpntPeriod = logPtr->eventLog.sigactLog.period;
    jp->chkpntPeriod = logPtr->eventLog.sigactLog.period;
    if (!IS_START(jp->jStatus) &&
        logPtr->eventLog.sigactLog.actStatus == ACT_NO)

        return (TRUE);

    newActPid = logPtr->eventLog.sigactLog.pid;

    if ((logPtr->eventLog.sigactLog.actStatus == ACT_START)) {
        jp->actPid = newActPid;
        jp->sigValue =  sigNameToValue_ (logPtr->eventLog.sigactLog.signalSymbol);
        if (logPtr->eventLog.sigactLog.flags & LSB_CHKPNT_MIG)
            jp->jStatus |= JOB_STAT_MIG;

    } else if (logPtr->eventLog.sigactLog.actStatus == ACT_DONE ||
               logPtr->eventLog.sigactLog.actStatus == ACT_FAIL) {

        jp->actPid = 0;
        jp->sigValue = SIG_NULL;
        if (logPtr->eventLog.sigactLog.flags & LSB_CHKPNT_MIG) {
            jp->jStatus |= JOB_STAT_MIG;

            if (!IS_PEND (jp->jStatus)) {

                FREEUP (jp->schedHost);
                jp->schedHost = safeSave (jp->hPtr[0]->hostType);
                FREEUP (jp->shared->jobBill.schedHostType);
                jp->shared->jobBill.schedHostType = safeSave (jp->hPtr[0]->hostType);
            }
            jp->shared->jobBill.restartPid = jp->jobPid;
            jp->restartPid = jp->jobPid;

        } else
            jp->jStatus &= ~JOB_STAT_MIG;

        if (logPtr->eventLog.sigactLog.actStatus == ACT_DONE &&
            ( (strcmp(logPtr->eventLog.sigactLog.signalSymbol, "SIG_CHKPNT") == 0)
              || (strcmp(logPtr->eventLog.sigactLog.signalSymbol, "SIG_CHKPNT_COPY") == 0)))
            jp->jStatus |= JOB_STAT_CHKPNTED_ONCE;
    } else {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6727,
                                         "%s: Unknown status <%d> of action <%s> of Job <%s>"), /* catgets 6727 */
                  fname,
                  logPtr->eventLog.sigactLog.actStatus,
                  logPtr->eventLog.sigactLog.signalSymbol,
                  lsb_jobid2str(jp->jobId));
    }
    return (TRUE);
}

static int
replay_jobrequeue(char *filename, int lineNum)
{
    static char             fname[] = "replay_jobrequeue";
    struct jData           *jp;
    LS_LONG_INT             jobId;

    jobId = LSB_JOBID(logPtr->eventLog.jobRequeueLog.jobId,
                      logPtr->eventLog.jobRequeueLog.idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6728,
                                         "%s: File %s at line %d: Requeued job <%d> is not found in job list"), fname, filename, lineNum, logPtr->eventLog.jobRequeueLog.jobId); /* catgets 6728 */
        return (FALSE);
    }
    handleRequeueJob(jp, eventTime);









    jp->endTime   = 0;

    return (TRUE);

}


static int
replay_chkpnt(char *filename, int lineNum)
{
    static char             fname[] = "replay_chkpnt";
    struct jData           *jp;
    int                     newChkPid;
    LS_LONG_INT             jobId;

    jobId = LSB_JOBID(logPtr->eventLog.chkpntLog.jobId,
                      logPtr->eventLog.chkpntLog.idx);

    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list"),
                  fname, filename, lineNum,
                  logPtr->eventLog.chkpntLog.jobId);
        return (FALSE);
    }
    jp->shared->jobBill.chkpntPeriod = logPtr->eventLog.chkpntLog.period;
    jp->chkpntPeriod = logPtr->eventLog.chkpntLog.period;
    if (!IS_START(jp->jStatus))
        return (TRUE);

    newChkPid = logPtr->eventLog.chkpntLog.pid;

    if (newChkPid > 0) {
        jp->actPid = newChkPid;
        if (logPtr->eventLog.chkpntLog.flags & LSB_CHKPNT_MIG)
            jp->jStatus |= JOB_STAT_MIG;
    } else if (newChkPid < 0) {
        jp->actPid = 0;
        if (logPtr->eventLog.chkpntLog.flags & LSB_CHKPNT_MIG) {
            jp->jStatus |= JOB_STAT_MIG;
            FREEUP (jp->schedHost);
            jp->schedHost = safeSave (jp->hPtr[0]->host);
            FREEUP (jp->shared->jobBill.schedHostType);
            jp->shared->jobBill.schedHostType = safeSave (jp->hPtr[0]->hostType);

            jp->shared->jobBill.restartPid = jp->jobPid;
            jp->restartPid = jp->jobPid;

        } else
            jp->jStatus &= ~JOB_STAT_MIG;

        if (logPtr->eventLog.chkpntLog.ok)
            jp->jStatus |= JOB_STAT_CHKPNTED_ONCE;
    } else {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6730,
                                         "%s: The actPid of Job <%s> cannot be 0"), /* catgets 6730 */
                  fname,
                  lsb_jobid2str(jp->jobId));
    }

    return (TRUE);
}


static int
replay_loadIndex(char *filename, int lineNum)
{
    int                     i;

    if (logPtr->eventLog.loadIndexLog.nIdx != allLsInfo->numIndx) {
        logLoadIndex = TRUE;
        return (TRUE);
    }
    for (i = 0; i < allLsInfo->numIndx; i++) {
        if (strcmp(allLsInfo->resTable[i].name,
                   logPtr->eventLog.loadIndexLog.name[i])) {
            logLoadIndex = TRUE;
            return (TRUE);
        }
    }

    logLoadIndex = FALSE;
    return (TRUE);
}

/*
 * replay_jobEffeResReq - Replay effective resource request entry
 * @resreq: Resource requirement string to replay
 *
 * This function is used during log replay to recreate effective resource request
 * entries. It first checks if the resource request already exists in the
 * jobEffeResReqTab hash table. If it exists, it increments the reference count.
 * If it doesn't exist, it creates a new resReqEntry by parsing the resource
 * requirement string and adds it to the hash table with an initial reference
 * count of 1.
 *
 * Returns: Hash table entry pointer on success, NULL on failure
 */
static hEnt*
replay_jobEffeResReq(char * resreq) {
    struct resReqEntry *resReqEnt = NULL;
    hEnt                *ent = NULL;

    /* Validate input - return NULL if resource request string is empty */
    if (resreq == NULL || resreq[0] == '\0') {
        return NULL;
    }

    /* Check if resource request already exists in the hash table */
    ent = h_getEnt_(&jobEffeResReqTab, resreq);
    if (ent == NULL) {
        /* Entry doesn't exist - create new entry */
        struct resVal * resValPtr = (struct resVal *)
            my_calloc(1, sizeof(struct resVal), "replay_jobEffeResReq");

        /* Parse the resource requirement string */
        if (parseResReq(resreq, resValPtr, allLsInfo, (PR_ALL | PR_BATCH)) != PARSE_OK) {
            freeResVal(resValPtr);
            return NULL;
        }

        /* Create new resReqEntry structure */
        resReqEnt = (struct resReqEntry *)
            my_calloc(1, sizeof(struct resReqEntry), "replay_jobEffeResReq");
        resReqEnt->resValPtr = resValPtr;
        resReqEnt->resReqStr = safeSave(resreq);
        resReqEnt->numRef = 1;
        
        /* Add new entry to hash table */
        ent = h_addEnt_(&jobEffeResReqTab, resreq, NULL);
        ent->hData = (int *)resReqEnt;
    } else {
        /* Entry already exists - increment reference count */
        resReqEnt = (struct resReqEntry *) ent->hData;
        resReqEnt->numRef++;
    }
    
    return ent;
}

int
log_modifyjob(struct modifyReq * modReq, struct lsfAuth *auth)
{
    static char             fname[] = "log_modifyjob";
    struct jobModLog        *jobModLog;
    int j;

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(modReq->jobId),
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }

    logPtr->type = EVENT_JOB_MODIFY2;

    jobModLog = &logPtr->eventLog.jobModLog;
    jobModLog->jobIdStr = modReq->jobIdStr;
    jobModLog->delOptions = modReq->delOptions;
    jobModLog->delOptions2 = modReq->delOptions2;
    jobModLog->options    = modReq->submitReq.options;
    jobModLog->options2   = modReq->submitReq.options2;
    jobModLog->userId     = auth->uid;
    jobModLog->userName   = auth->lsfUserName;
    jobModLog->submitTime   = modReq->submitReq.submitTime;
    jobModLog->umask   = modReq->submitReq.umask;
    jobModLog->numProcessors   = modReq->submitReq.numProcessors;
    jobModLog->beginTime   = modReq->submitReq.beginTime;
    jobModLog->termTime   = modReq->submitReq.termTime;
    jobModLog->sigValue   = modReq->submitReq.sigValue;
    jobModLog->restartPid   = modReq->submitReq.restartPid;
    jobModLog->jobName   = modReq->submitReq.jobName;
    jobModLog->queue   = modReq->submitReq.queue;
    jobModLog->numAskedHosts   = modReq->submitReq.numAskedHosts;
    if (jobModLog->numAskedHosts > 0)
        jobModLog->askedHosts = modReq->submitReq.askedHosts;
    jobModLog->resReq   = modReq->submitReq.resReq;
    for (j = 0; j < LSF_RLIM_NLIMITS; j++)
        jobModLog->rLimits[j] = modReq->submitReq.rLimits[j];

    jobModLog->hostSpec   = modReq->submitReq.hostSpec;
    jobModLog->dependCond   = modReq->submitReq.dependCond;
    jobModLog->subHomeDir   = modReq->submitReq.subHomeDir;
    jobModLog->inFile   = modReq->submitReq.inFile;
    jobModLog->outFile   = modReq->submitReq.outFile;
    jobModLog->errFile   = modReq->submitReq.errFile;
    jobModLog->command   = modReq->submitReq.command;
    jobModLog->inFileSpool   = modReq->submitReq.inFileSpool;
    jobModLog->commandSpool   = modReq->submitReq.commandSpool;
    jobModLog->chkpntPeriod   = modReq->submitReq.chkpntPeriod;
    jobModLog->chkpntDir   = modReq->submitReq.chkpntDir;
    jobModLog->nxf   = modReq->submitReq.nxf;
    jobModLog->xf   = modReq->submitReq.xf;
    jobModLog->jobFile   = modReq->submitReq.jobFile;
    jobModLog->fromHost   = modReq->submitReq.fromHost;
    jobModLog->cwd   = modReq->submitReq.cwd;
    jobModLog->preExecCmd   = modReq->submitReq.preExecCmd;
    jobModLog->postExecCmd   = modReq->submitReq.postExecCmd;
    jobModLog->mailUser   = modReq->submitReq.mailUser;
    jobModLog->projectName   = modReq->submitReq.projectName;
    jobModLog->niosPort   = modReq->submitReq.niosPort;
    jobModLog->maxNumProcessors   = modReq->submitReq.maxNumProcessors;
    jobModLog->loginShell   = modReq->submitReq.loginShell;
    jobModLog->schedHostType   = modReq->submitReq.schedHostType;

    jobModLog->userPriority = modReq->submitReq.userPriority;
    jobModLog->jobDesc = modReq->submitReq.jobDesc;

    if (putEventRec(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(modReq->jobId),
                  "putEventRec");
        mbdDie(MASTER_FATAL);
    }
    return (0);

}

int
log_newjob(struct jData * job)
{
    static char             fname[] = "log_newjob";

    return (log_jobdata(job, fname, EVENT_JOB_NEW));
}

static int
replay_logSwitch(char *filename, int lineNum)
{
    nextJobId_t = logPtr->eventLog.logSwitchLog.lastJobId;
    return (TRUE);

}

static int
log_jobdata(struct jData * job, char *fname1, int type)
{
    static char             fname[] = "log_jobdata";
    struct submitReq       *jobBill = &(job->shared->jobBill);
    struct jobNewLog       *jobNewLog;
    int                     j;
    float                  *hostFactor;

    if (openEventFile(fname1) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "openEventFile",
                  fname1);
        if (type == EVENT_JOB_NEW)
            rmLogJobInfo_(job, FALSE);
        mbdDie(MASTER_FATAL);
    }

    logPtr->type = type;

    jobNewLog = &logPtr->eventLog.jobNewLog;
    jobNewLog->jobId = LSB_ARRAY_JOBID(job->jobId);
    jobNewLog->idx = LSB_ARRAY_IDX(job->jobId);

    jobNewLog->userId = job->userId;
    jobNewLog->options = jobBill->options;
    jobNewLog->options2 = jobBill->options2;
    if (jobNewLog->options2 & SUB2_USE_DEF_PROCLIMIT) {

        jobNewLog->numProcessors = 1;
        jobNewLog->maxNumProcessors = 1;
    }
    else {
        jobNewLog->numProcessors = jobBill->numProcessors;
        jobNewLog->maxNumProcessors = jobBill->maxNumProcessors;
    }
    jobNewLog->submitTime = jobBill->submitTime;
    jobNewLog->beginTime = jobBill->beginTime;
    jobNewLog->termTime = jobBill->termTime;
    strcpy(jobNewLog->userName, job->userName);
    jobNewLog->schedHostType = job->schedHost;
    jobNewLog->loginShell = jobBill->loginShell;

    if (jobBill->options & SUB_WINDOW_SIG)
        jobNewLog->sigValue = jobBill->sigValue;
    else
        jobNewLog->sigValue = SIG_NULL;

    jobNewLog->chkpntPeriod = job->chkpntPeriod;
    jobNewLog->restartPid = job->restartPid;

    for (j = 0; j < LSF_RLIM_NLIMITS; j++)
        jobNewLog->rLimits[j] = jobBill->rLimits[j];
    strcpy(jobNewLog->hostSpec, jobBill->hostSpec);
    if (jobNewLog->hostSpec[0] == '\0') {
        hostFactor = getHostFactor(jobBill->fromHost);
        if (hostFactor == NULL) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S,
                      fname,
                      lsb_jobid2str(job->jobId),
                      "getHostFactor",
                      jobBill->fromHost);
            jobNewLog->hostFactor = 1.0;
        }
    } else if ((hostFactor = getModelFactor(jobNewLog->hostSpec)) == NULL) {
        hostFactor = getHostFactor(jobNewLog->hostSpec);
        if (hostFactor == NULL) {
            LS_LONG_INT tmpJobId;
            tmpJobId = jobNewLog->jobId;
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S,
                      fname,
                      lsb_jobid2str(tmpJobId),
                      "getHostFactor",
                      jobBill->hostSpec);
            jobNewLog->hostFactor = 1.0;
        }
    }
    if (hostFactor)
        jobNewLog->hostFactor = *hostFactor;

    jobNewLog->umask = jobBill->umask;
    strcpy(jobNewLog->queue, jobBill->queue);
    jobNewLog->resReq = jobBill->resReq;

    strcpy(jobNewLog->fromHost, jobBill->fromHost);
    strcpy(jobNewLog->cwd, jobBill->cwd);
    strcpy(jobNewLog->subHomeDir, jobBill->subHomeDir);
    strcpy(jobNewLog->submitCwd, jobBill->submitCwd);
    strcpy(jobNewLog->chkpntDir,  jobBill->chkpntDir);

    if ((jobBill->options & SUB_IN_FILE) == SUB_IN_FILE)
        strcpy(jobNewLog->inFile, jobBill->inFile);
    else
        strcpy(jobNewLog->inFile, "");

    if ((jobBill->options & SUB_OUT_FILE) == SUB_OUT_FILE)
        strcpy(jobNewLog->outFile, jobBill->outFile);
    else
        strcpy(jobNewLog->outFile, "");

    if ((jobBill->options & SUB_ERR_FILE) == SUB_ERR_FILE)
        strcpy(jobNewLog->errFile, jobBill->errFile);
    else
        strcpy(jobNewLog->errFile, "");

    if ((jobBill->options2 & SUB2_IN_FILE_SPOOL) == SUB2_IN_FILE_SPOOL) {
        strcpy(jobNewLog->inFileSpool, jobBill->inFileSpool);
        strcpy(jobNewLog->inFile, jobBill->inFile);
    } else {
        strcpy(jobNewLog->inFileSpool, "");
    }

    if ((jobBill->options2 & SUB2_JOB_CMD_SPOOL) == SUB2_JOB_CMD_SPOOL)
        strcpy(jobNewLog->commandSpool, jobBill->commandSpool);
    else
        strcpy(jobNewLog->commandSpool, "");

    if (job->jobSpoolDir != NULL)
        strcpy(jobNewLog->jobSpoolDir, job->jobSpoolDir);
    else
        strcpy(jobNewLog->jobSpoolDir, "");

    strcpy(jobNewLog->jobFile, jobBill->jobFile);

    if (jobBill->numAskedHosts > 0)
        jobNewLog->askedHosts = jobBill->askedHosts;
    jobNewLog->numAskedHosts = jobBill->numAskedHosts;

    if (jobBill->options & SUB_JOB_NAME)
        strcpy(jobNewLog->jobName, jobBill->jobName);
    else
        strcpy(jobNewLog->jobName, "");

    strcpy(jobNewLog->command, jobBill->command);

    jobNewLog->dependCond = jobBill->dependCond;
    jobNewLog->preExecCmd = jobBill->preExecCmd;
    jobNewLog->postExecCmd = jobBill->postExecCmd;
    jobNewLog->mailUser = jobBill->mailUser;

    if (strcmp(jobBill->projectName, "") == 0) {

        jobNewLog->projectName = getDefaultProject();
    } else
        jobNewLog->projectName = jobBill->projectName;


    jobNewLog->nxf = jobBill->nxf;
    jobNewLog->xf = jobBill->xf;

    jobNewLog->niosPort = jobBill->niosPort;

    jobNewLog->userPriority = jobBill->userPriority;

    if (jobBill->options2 & SUB2_JOB_DESC)
        strcpy(jobNewLog->jobDesc, jobBill->jobDesc);
    else
        strcpy(jobNewLog->jobDesc, "");

    if (putEventRec(fname1) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "putEventRec");
        if (type == EVENT_JOB_NEW)
            rmLogJobInfo_(job, FALSE);
        mbdDie(MASTER_FATAL);
    }
    return (0);
}



void
log_switchjob(struct jobSwitchReq * switchReq, int uid, char *userName)
{
    static char             fname[] = "log_switchjob";
    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(switchReq->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_SWITCH;
    logPtr->eventLog.jobSwitchLog.userId = uid;
    strcpy(logPtr->eventLog.jobSwitchLog.userName, userName);
    logPtr->eventLog.jobSwitchLog.jobId = LSB_ARRAY_JOBID(switchReq->jobId);
    logPtr->eventLog.jobSwitchLog.idx = LSB_ARRAY_IDX(switchReq->jobId);
    strcpy(logPtr->eventLog.jobSwitchLog.queue, switchReq->queue);
    if (putEventRec("log_switchjob") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(switchReq->jobId),
                  "putEventRec");
    }
}

void
log_movejob(struct jobMoveReq * moveReq, int uid, char *userName)
{
    static char             fname[] = "log_movejob";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(moveReq->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_MOVE;
    logPtr->eventLog.jobMoveLog.userId = uid;
    strcpy(logPtr->eventLog.jobMoveLog.userName, userName);
    logPtr->eventLog.jobMoveLog.jobId = LSB_ARRAY_JOBID(moveReq->jobId);
    logPtr->eventLog.jobMoveLog.idx = LSB_ARRAY_IDX(moveReq->jobId);
    logPtr->eventLog.jobMoveLog.position = moveReq->position;
    logPtr->eventLog.jobMoveLog.base = moveReq->opCode;
    if (putEventRec("log_movejob") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(moveReq->jobId),
                  "putEventRec");
    }
}

void
log_startjob(struct jData * job, int preExecStart)
{
    static char             fname[] = "log_startjob";
    int                     i;
    struct jobStartLog     *jobStartLog;
    char                  **execHosts = NULL;

    if (openEventFile("log_startjob") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    if (preExecStart) {
        logPtr->type = EVENT_PRE_EXEC_START;
    } else
        logPtr->type = EVENT_JOB_START;

    jobStartLog = &logPtr->eventLog.jobStartLog;
    jobStartLog->jobId = LSB_ARRAY_JOBID(job->jobId);
    jobStartLog->idx = LSB_ARRAY_IDX(job->jobId);
    jobStartLog->jStatus = JOB_STAT_RUN;
    jobStartLog->jFlags  = 0;
    jobStartLog->numExHosts = job->numHostPtr;
    jobStartLog->jobPid = job->jobPid;
    jobStartLog->jobPGid = job->jobPGid;
    if (job->numHostPtr > 0) {
        execHosts = (char **) my_calloc(job->numHostPtr, sizeof(char *),
                                        "log_startjob");
        jobStartLog->execHosts = execHosts;
        for (i = 0; i < job->numHostPtr; i++)
            jobStartLog->execHosts[i] = job->hPtr[i]->host;
        jobStartLog->hostFactor = job->hPtr[0]->cpuFactor;
    }
    job->startTime = time(0);

    if (!job->queuePreCmd)
        jobStartLog->queuePreCmd = "";
    else
        jobStartLog->queuePreCmd = job->queuePreCmd;

    if (!job->queuePostCmd)
        jobStartLog->queuePostCmd = "";
    else
        jobStartLog->queuePostCmd = job->queuePostCmd;

    if (!job->effeResReqEnt) {
        jobStartLog->effeResReq = "";
    } else {
        jobStartLog->effeResReq = GET_JOB_EFFE_RES_REQ_STR(job);
    }

    if (putEventRec(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "putEventRec");
        mbdDie(MASTER_FATAL);
    }
    if (execHosts)
        free(execHosts);

}

void
log_executejob(struct jData * job)
{
    static char             fname[] = "log_executejob";
    struct jobExecuteLog   *jobExecuteLog;

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_EXECUTE;

    jobExecuteLog = &logPtr->eventLog.jobExecuteLog;
    jobExecuteLog->jobId = LSB_ARRAY_JOBID(job->jobId);
    jobExecuteLog->idx = LSB_ARRAY_IDX(job->jobId);
    jobExecuteLog->execUid = job->execUid;
    jobExecuteLog->jobPGid = job->jobPGid;
    jobExecuteLog->jobPid = job->jobPid;
    jobExecuteLog->execHome = job->execHome;
    jobExecuteLog->execCwd = job->execCwd;
    jobExecuteLog->execUsername = job->execUsername;

    if (putEventRec(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "putEventRec");
        mbdDie(MASTER_FATAL);
    }

}

void
log_startjobaccept(struct jData * job)
{
    static char             fname[] = "log_startjobaccept";
    struct jobStartAcceptLog   *jobStartAcceptLog;

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_START_ACCEPT;

    jobStartAcceptLog = &logPtr->eventLog.jobStartAcceptLog;
    jobStartAcceptLog->jobId = LSB_ARRAY_JOBID(job->jobId);
    jobStartAcceptLog->idx = LSB_ARRAY_IDX(job->jobId);
    jobStartAcceptLog->jobPGid = job->jobPGid;
    jobStartAcceptLog->jobPid = job->jobPid;

    if ((job->jgrpNode->nodeType == JGRP_NODE_ARRAY) &&
        (ARRAY_DATA(job->jgrpNode)->jobArray->startTime == 0)) {
        ARRAY_DATA(job->jgrpNode)->jobArray->startTime = job->startTime;
    }
    if (putEventRec("log_startjobaccept") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "putEventRec");
        mbdDie(MASTER_FATAL);
    }
}



void
log_newstatus(struct jData * job)
{
    static char             fname[] = "log_newstatus";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    if (job->jStatus & (JOB_STAT_EXIT | JOB_STAT_DONE)) {
        now = time(0);
        job->endTime = now;
    }
    logPtr->type = EVENT_JOB_STATUS;
    logPtr->eventLog.jobStatusLog.jobId = LSB_ARRAY_JOBID(job->jobId);
    logPtr->eventLog.jobStatusLog.idx = LSB_ARRAY_IDX(job->jobId);
    logPtr->eventLog.jobStatusLog.cpuTime = job->cpuTime;
    logPtr->eventLog.jobStatusLog.endTime = job->endTime;
    logPtr->eventLog.jobStatusLog.reason = job->newReason;
    logPtr->eventLog.jobStatusLog.subreasons = job->subreasons;
    logPtr->eventLog.jobStatusLog.exitStatus = job->exitStatus;


    if ((logPtr->eventLog.jobStatusLog.ru = (job->lsfRusage != NULL)))
        logPtr->eventLog.jobStatusLog.lsfRusage = *(job->lsfRusage);

    if (job->jStatus & JOB_STAT_UNKWN)
        logPtr->eventLog.jobStatusLog.jStatus = JOB_STAT_UNKWN;
    else
        logPtr->eventLog.jobStatusLog.jStatus = job->jStatus;

    logPtr->eventLog.jobStatusLog.jStatus &= MASK_INT_JOB_STAT;

    if (job->jStatus & JOB_STAT_DONE || job->jStatus & JOB_STAT_EXIT) {
        logPtr->eventLog.jobStatusLog.maxMem = job->maxMem;
        logPtr->eventLog.jobStatusLog.avgMem = job->avgMem;
    } else {
        logPtr->eventLog.jobStatusLog.maxMem = 0;
        logPtr->eventLog.jobStatusLog.avgMem = 0;
    }

    if (putEventRec(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "putEventRec");
        mbdDie(MASTER_FATAL);
    }
    if (job->jStatus & JOB_STAT_DONE
        || (job->jStatus & JOB_STAT_EXIT
            && !(job->jStatus & JOB_STAT_ZOMBIE))) {
        if (((IS_POST_DONE(job->jStatus))||(IS_POST_ERR(job->jStatus))) &&
            (job->jgrpNode->nodeType == JGRP_NODE_ARRAY &&
             ARRAY_DATA(job->jgrpNode)->counts[JGRP_COUNT_NJOBS] ==
             (ARRAY_DATA(job->jgrpNode)->counts[JGRP_COUNT_NDONE] +
              ARRAY_DATA(job->jgrpNode)->counts[JGRP_COUNT_NEXIT]))) {
            ARRAY_DATA(job->jgrpNode)->jobArray->endTime = job->endTime;
        }
        logFinishedjob(job);
    }
}

void
log_mig(struct jData * jData, int uid, char *userName)
{
    static char             fname[] = "log_mig";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_MIG;
    logPtr->eventLog.migLog.jobId = LSB_ARRAY_JOBID(jData->jobId);
    logPtr->eventLog.migLog.idx = LSB_ARRAY_IDX(jData->jobId);
    logPtr->eventLog.migLog.numAskedHosts = jData->shared->jobBill.numAskedHosts;
    logPtr->eventLog.migLog.askedHosts = jData->shared->jobBill.askedHosts;
    logPtr->eventLog.migLog.userId = uid;
    strcpy(logPtr->eventLog.migLog.userName, userName);

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "putEventRec");
}

void
log_jobrequeue(struct jData * jData)
{
    static char             fname[] = "log_jobrequeue";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "openeventfile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_REQUEUE;
    logPtr->eventLog.jobRequeueLog.jobId = LSB_ARRAY_JOBID(jData->jobId);
    logPtr->eventLog.jobRequeueLog.idx = LSB_ARRAY_IDX(jData->jobId);

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "putEventRec");
}

void
log_jobclean(struct jData * jData)
{
    static char             fname[] = "log_jobclean";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_CLEAN;
    logPtr->eventLog.jobCleanLog.jobId = LSB_ARRAY_JOBID(jData->jobId);
    logPtr->eventLog.jobCleanLog.idx = LSB_ARRAY_IDX(jData->jobId);

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "putEventRec");
}


void
log_chkpnt(struct jData * jData, int ok, int flags)
{
    static char             fname[] = "log_chkpnt";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_CHKPNT;
    logPtr->eventLog.chkpntLog.jobId = LSB_ARRAY_JOBID(jData->jobId);
    logPtr->eventLog.chkpntLog.idx = LSB_ARRAY_IDX(jData->jobId);
    logPtr->eventLog.chkpntLog.period = jData->chkpntPeriod;
    logPtr->eventLog.chkpntLog.pid = jData->actPid;
    logPtr->eventLog.chkpntLog.ok = ok;
    logPtr->eventLog.chkpntLog.flags = flags;

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "putEventRec");
}


void
log_jobsigact (struct jData *jData, struct statusReq *statusReq, int sigFlags)
{
    static char             fname[] = "log_jobsigact";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_SIGACT;
    logPtr->eventLog.sigactLog.jobId = LSB_ARRAY_JOBID(jData->jobId);
    logPtr->eventLog.sigactLog.idx = LSB_ARRAY_IDX(jData->jobId);
    logPtr->eventLog.sigactLog.flags = sigFlags;
    logPtr->eventLog.sigactLog.period = jData->chkpntPeriod;
    if (statusReq == NULL) {

        logPtr->eventLog.sigactLog.jStatus = jData->jStatus;
        if (jData->newReason)
            logPtr->eventLog.sigactLog.reasons = jData->newReason;
        else
            logPtr->eventLog.sigactLog.reasons = PEND_SYS_UNABLE;
        logPtr->eventLog.sigactLog.pid = 0;
        logPtr->eventLog.sigactLog.actStatus = ACT_NO;
        logPtr->eventLog.sigactLog.flags = 0;
        logPtr->eventLog.sigactLog.signalSymbol = "SIG_CHKPNT";
    } else {
        logPtr->eventLog.sigactLog.jStatus = statusReq->newStatus;
        logPtr->eventLog.sigactLog.reasons = statusReq->reason;
        logPtr->eventLog.sigactLog.pid = statusReq->actPid;
        logPtr->eventLog.sigactLog.actStatus = statusReq->actStatus;
        logPtr->eventLog.sigactLog.flags = sigFlags;
        logPtr->eventLog.sigactLog.signalSymbol = getLsbSigSymbol(statusReq->sigValue);
    }



    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jData->jobId),
                  "putEventRec");
}

void
log_queuestatus(struct qData * qp, int opCode, int userId, char *userName)
{
    static char             fname[] = "log_queuestatus";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_QUEUE_FAIL,
                  fname,
                  qp->queue,
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_QUEUE_CTRL;
    logPtr->eventLog.queueCtrlLog.opCode = opCode;
    strcpy(logPtr->eventLog.queueCtrlLog.queue, qp->queue);
    logPtr->eventLog.queueCtrlLog.userId = userId;
    strcpy(logPtr->eventLog.queueCtrlLog.userName, userName);
    if (qp->actionComment){
        strncpy(logPtr->eventLog.queueCtrlLog.msg, qp->actionComment, MAXLINELEN-1);
    }
    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_QUEUE_FAIL,
                  fname,
                  qp->queue,
                  "putEventRec");
}



void
log_hoststatus(struct hData * hp, int opCode, int userId, char *userName)
{
    static char             fname[] = "log_hoststatus";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_HOST_FAIL,
                  fname,
                  hp->host,
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_HOST_CTRL;
    logPtr->eventLog.hostCtrlLog.opCode = opCode;
    strcpy(logPtr->eventLog.hostCtrlLog.host, hp->host);
    logPtr->eventLog.hostCtrlLog.userId = userId;
    strcpy(logPtr->eventLog.hostCtrlLog.userName, userName);
    if (hp->actionComment) {
        strncpy(logPtr->eventLog.hostCtrlLog.msg, hp->actionComment, MAXLINELEN - 1);
    }

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_HOST_FAIL,
                  fname,
                  hp->host,
                  "putEventRec");
}

void
log_mbdDie(int sig)
{
    static char             fname[] = "log_mbdDie";
    char                    hname[MAXHOSTNAMELEN];

    if (openEventFile(fname) < 0)
        return;
    logPtr->type = EVENT_MBD_DIE;
    if (masterHost)
        strcpy(logPtr->eventLog.mbdDieLog.master, masterHost);
    else if (gethostname(hname, MAXHOSTNAMELEN) >= 0)
        strcpy(logPtr->eventLog.mbdDieLog.master, hname);
    else
        strcpy(logPtr->eventLog.mbdDieLog.master, "unknown");

    logPtr->eventLog.mbdDieLog.numRemoveJobs = numRemoveJobs;
    logPtr->eventLog.mbdDieLog.exitCode = sig;
    if (mbdCtrlReq.msg != NULL)
        strncpy(logPtr->eventLog.mbdDieLog.msg, mbdCtrlReq.msg, MAXLINELEN-1);
    putEventRec(fname);
}

void
log_unfulfill(struct jData * jp)
{
    static char             fname[] = "log_unfulfill";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jp->jobId),
                  "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_MBD_UNFULFILL;

    logPtr->eventLog.unfulfillLog.jobId = LSB_ARRAY_JOBID(jp->jobId);
    logPtr->eventLog.unfulfillLog.idx = LSB_ARRAY_IDX(jp->jobId);
    logPtr->eventLog.unfulfillLog.notSwitched = jp->pendEvent.notSwitched;
    logPtr->eventLog.unfulfillLog.sig = jp->pendEvent.sig;
    logPtr->eventLog.unfulfillLog.sig1 = jp->pendEvent.sig1;
    logPtr->eventLog.unfulfillLog.sig1Flags = jp->pendEvent.sig1Flags;
    logPtr->eventLog.unfulfillLog.chkPeriod = jp->chkpntPeriod;
    logPtr->eventLog.unfulfillLog.notModified = jp->pendEvent.notModified;

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S,
                  fname,
                  lsb_jobid2str(jp->jobId),
                  "putEventRec");
}

static void
log_loadIndex(void)
{
    static char             fname[] = "log_loadIndex";
    int                     i;
    static char            **names;

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    if (!names)
        if (!(names = (char **)malloc(allLsInfo->numIndx*sizeof(char *)))) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "malloc");
            mbdDie(MASTER_FATAL);
        }
    logPtr->type = EVENT_LOAD_INDEX;
    logPtr->eventLog.loadIndexLog.nIdx = allLsInfo->numIndx;
    logPtr->eventLog.loadIndexLog.name = names;

    for (i = 0; i < allLsInfo->numIndx; i++)
        logPtr->eventLog.loadIndexLog.name[i] = allLsInfo->resTable[i].name;
    if (putEventRec(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "putEventRec");
        mbdDie(MASTER_FATAL);
    }
}
void
log_jobForce(struct jData* job, int uid, char *userName)
{
    static char                 fname[] = "log_JobForce()";
    int                         i;
    struct jobForceRequestLog   jobForceRequestLog;

    memset((struct jobForceRequestLog *)&jobForceRequestLog, 0,
           sizeof(struct jobForceRequestLog));


    jobForceRequestLog.jobId        =  LSB_ARRAY_JOBID(job->jobId);
    jobForceRequestLog.idx          =  LSB_ARRAY_IDX(job->jobId);
    jobForceRequestLog.userId       =  uid;
    strcpy(jobForceRequestLog.userName, userName);
    jobForceRequestLog.numExecHosts =  job->numHostPtr;
    jobForceRequestLog.execHosts    = (char **)my_calloc(job->numHostPtr,
                                                         sizeof(char *),
                                                         fname);
    for (i = 0; i < job->numHostPtr; i++) {
        jobForceRequestLog.execHosts[i] = job->hPtr[i]->host;
    }

    jobForceRequestLog.options |= (job->jFlags & JFLAG_URGENT_NOSTOP) ?
        RUNJOB_OPT_NOSTOP : RUNJOB_OPT_NORMAL;


    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "openEventFile");
        mbdDie(MASTER_FATAL);
    }


    logPtr->type = EVENT_JOB_FORCE;


    memcpy(&(logPtr->eventLog.jobForceRequestLog), &jobForceRequestLog,
           sizeof(struct jobForceRequestLog));


    if (putEventRec(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "putEventRec");
        mbdDie(MASTER_FATAL);
    }

    FREEUP(jobForceRequestLog.execHosts);

}

static int
openEventFile(char *fname)
{
    long pos;
    sigset_t newmask, oldmask;
    /*
     * Single write route: while a switch child is running the active
     * event file is frozen and every new event goes to lsb.events.delta.
     */
    char *target = elogWriteTarget;

    chuser(managerId);

    sigemptyset(&newmask);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);
    if ((log_fp = fopen(target, "a+")) == NULL) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", target);
        return -1;
    }
    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    fseek(log_fp, 0L, SEEK_END);


    if ((pos = ftell(log_fp)) == 0) {
        fprintf(log_fp, "#80                                                                            \n");
    }

    chmod(target, 0644);
    chuser(batchId);
    logPtr = my_calloc(1, sizeof(struct eventRec), __func__);

    sprintf(logPtr->version, "%d", VOLCLAVA_VERSION);

    return 0;
}

static int
putEventRec(char *fname)
{
    int    ret = 0;

    now = time(0);
    logPtr->eventTime = now;

    ret = putEventRec1(fname);

    return (ret);
}
static int
putEventRecTime(char *fname, time_t eventTime)
{
    int    ret;

    logPtr->eventTime = eventTime;

    ret = putEventRec1(fname);

    return(ret);

}

static int
putEventRec1(char *fname)
{
    int    ret;
    int    cc;

    ret = 0;

    ftell(log_fp);

    if (lsb_puteventrec(log_fp, logPtr) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_EMSG_S,
                  fname, "lsb_puteventrec", lsb_sysmsg());
        ret = -1;
    }

    free(logPtr);

    chuser(managerId);

    cc = FCLOSEUP(&log_fp);

    chuser(batchId);

    if (cc < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fclose");
        ret = -1;
    }

    return(ret);

}

/*
 * Serialize one event record into the given stream; unlike putEventRec()
 * it neither picks the active event file nor closes the stream.
 */
static int
writeEventRec(FILE *fp, struct eventRec *rec, int type, time_t eventTime)
{
    sprintf(rec->version, "%d", VOLCLAVA_VERSION);
    rec->type = type;
    rec->eventTime = eventTime;

    if (lsb_puteventrec(fp, rec) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_EMSG_S,
                  __func__, "lsb_puteventrec", lsb_sysmsg());
        return -1;
    }

    return 0;
}

static void
logFinishedjob(struct jData *job)
{
    static char           fname[] = "logFinishedjob()";
    struct submitReq      *jobBill = &(job->shared->jobBill);
    struct jobFinishLog   *jobFinishLog;
    int                   i;
    float                 *hostFactor;

    if ( IS_POST_FINISH(job->jStatus) ) {
        return;
    }

    chuser(managerId);
    if ((joblog_fp = fopen(jlogFname, "a")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen",
                  jlogFname);
        return;
    }
    chmod(jlogFname, 0644);
    chuser(batchId);

    logPtr = my_calloc(1, sizeof(struct eventRec), "logFinishedjob");
    jobFinishLog = &logPtr->eventLog.jobFinishLog;

    logPtr->type = EVENT_JOB_FINISH;
    sprintf(logPtr->version, "%d", VOLCLAVA_VERSION);
    jobFinishLog->jobId = LSB_ARRAY_JOBID(job->jobId);
    jobFinishLog->idx = LSB_ARRAY_IDX(job->jobId);
    jobFinishLog->userId = job->userId;
    strcpy(jobFinishLog->userName, job->userName);
    jobFinishLog->options = jobBill->options;
    jobFinishLog->numProcessors = jobBill->numProcessors;
    jobFinishLog->maxNumProcessors = jobBill->maxNumProcessors;
    jobFinishLog->submitTime = jobBill->submitTime;
    jobFinishLog->beginTime = jobBill->beginTime;
    jobFinishLog->termTime = jobBill->termTime;
    jobFinishLog->startTime = job->startTime;

    strcpy(jobFinishLog->queue, jobBill->queue);
    jobFinishLog->resReq = jobBill->resReq;
    jobFinishLog->dependCond = jobBill->dependCond;
    jobFinishLog->preExecCmd = jobBill->preExecCmd;
    jobFinishLog->postExecCmd = jobBill->postExecCmd;
    jobFinishLog->mailUser = jobBill->mailUser;

    if (strcmp(jobBill->projectName, "") == 0) {
        jobFinishLog->projectName = getDefaultProject();
    } else
        jobFinishLog->projectName = jobBill->projectName;

    strcpy(jobFinishLog->fromHost, jobBill->fromHost);
    strcpy(jobFinishLog->cwd, jobBill->cwd);

    if ((jobBill->options & SUB_IN_FILE) == SUB_IN_FILE)
        strcpy(jobFinishLog->inFile, jobBill->inFile);
    else
        strcpy(jobFinishLog->inFile, "");

    if ((jobBill->options & SUB_OUT_FILE) == SUB_OUT_FILE)
        strcpy(jobFinishLog->outFile, jobBill->outFile);
    else
        strcpy(jobFinishLog->outFile, "");

    if ((jobBill->options & SUB_ERR_FILE) == SUB_ERR_FILE)
        strcpy(jobFinishLog->errFile, jobBill->errFile);
    else
        strcpy(jobFinishLog->errFile, "");

    if ((jobBill->options2 & SUB2_IN_FILE_SPOOL) == SUB2_IN_FILE_SPOOL) {
        strcpy(jobFinishLog->inFileSpool, jobBill->inFileSpool);
        strcpy(jobFinishLog->inFile, jobBill->inFile);
    } else {
        strcpy(jobFinishLog->inFileSpool, "");
    }

    if ((jobBill->options2 & SUB2_JOB_CMD_SPOOL) == SUB2_JOB_CMD_SPOOL)
        strcpy(jobFinishLog->commandSpool, jobBill->commandSpool);
    else
        strcpy(jobFinishLog->commandSpool, "");

    strcpy(jobFinishLog->jobFile, jobBill->jobFile);

    jobFinishLog->jStatus = job->jStatus;
    jobFinishLog->jStatus &= MASK_INT_JOB_STAT;
    jobFinishLog->endTime = job->endTime;
    logPtr->eventTime = job->endTime;

    jobFinishLog->exitStatus = job->exitStatus;

    jobFinishLog->numAskedHosts = jobBill->numAskedHosts;
    if (jobFinishLog->numAskedHosts > 0)
        jobFinishLog->askedHosts = jobBill->askedHosts;

    jobFinishLog->numExHosts = job->numHostPtr;
    if (jobFinishLog->numExHosts > 0) {
        jobFinishLog->execHosts = my_calloc(job->numHostPtr,
                                            sizeof(char *),
                                            "logFinishedjob");
        for (i = 0; i < jobFinishLog->numExHosts; i++)
            jobFinishLog->execHosts[i] = job->hPtr[i]->host;
    }
    if (jobFinishLog->numExHosts > 0)
        jobFinishLog->hostFactor = job->hPtr[0]->cpuFactor;
    else {
        hostFactor = getHostFactor(jobFinishLog->fromHost);
        if (hostFactor == NULL) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S,
                      fname,
                      lsb_jobid2str(job->jobId),
                      "getHostFactor",
                      jobFinishLog->fromHost);
            jobFinishLog->hostFactor = 1.0;
        } else
            jobFinishLog->hostFactor = *hostFactor;
    }
    if (jobBill->options & SUB_JOB_NAME)
        strcpy(jobFinishLog->jobName, fullJobName(job));
    else
        strcpy(jobFinishLog->jobName, "");

    strcpy(jobFinishLog->command, jobBill->command);
    jobFinishLog->loginShell = jobBill->loginShell;

    if (job->lsfRusage != NULL)
        jobFinishLog->lsfRusage = *(job->lsfRusage);
    else
        cleanLsfRusage(&jobFinishLog->lsfRusage);

    jobFinishLog->maxRMem = job->runRusage.mem;
    jobFinishLog->maxRSwap = job->runRusage.swap;

    if (lsb_puteventrec(joblog_fp, logPtr) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_MM,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "lsb_puteventrec");

    if (jobFinishLog->numExHosts)
        free(jobFinishLog->execHosts);
    free(logPtr);

    chuser(managerId);
    if (FCLOSEUP(&joblog_fp) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M,
                  fname,
                  lsb_jobid2str(job->jobId),
                  "fclose");
    } else{
        chuser(batchId);
    }

    return ;
}

void
switchELog (void)
{
    /*
     * Step 1: kill a hung child (typically a blocked NFS call), once: a child
     * in D state keeps the SIGKILL pending, re-sending it only floods the log.
     * The SIGCHLD path then recovers the round.  Checked regardless of
     * numRemoveJobs, or a quiet cluster would never notice.
     */
    if (switchState == SWITCH_CHILD_RUNNING && switchChildPid > 0
        && !switchChildDone && !switchChildKillSent
        && time(0) - switchStartTime > SWITCH_CHILD_MAX_RUNTIME) {
        ls_syslog(LOG_ERR, "%s: lsb.events switch process <%d> running for \
%d seconds; killed", __func__, (int)switchChildPid,
                  (int)(time(0) - switchStartTime));
        kill(switchChildPid, SIGKILL);
        switchChildKillSent = TRUE;
    }

    /*
     * Step 2: while the child runs, say so every SWITCH_REMIND_RUNNING
     * seconds; a normal run takes seconds to minutes, so a longer one means
     * the log rotation is stuck somewhere (NFS, disk).  The next switch waits
     * for this one, nothing is lost.
     */
    {
        static time_t nextReminder = 0;
        time_t        t = time(0);
        int           running = (switchState == SWITCH_CHILD_RUNNING
                                 && switchChildPid > 0 && !switchChildDone
                                 && !switchChildKillSent);

        if (!running) {
            nextReminder = 0;
        } else if (nextReminder == 0) {
            nextReminder = switchStartTime + SWITCH_REMIND_RUNNING;
        } else if (t >= nextReminder) {
            ls_syslog(LOG_WARNING, "%s: lsb.events switch process <%d> \
still running after %d seconds", __func__, (int)switchChildPid,
                      (int)(t - switchStartTime));
            nextReminder = t + SWITCH_REMIND_RUNNING;
        }
    }

    /*
     * Step 3: a killed child that does not exit has no other trace; a round
     * that stays parked reports itself on every retry (Step 4)
     */
    {
        static time_t troubleSince = 0;
        static time_t lastReminder = 0;
        time_t        t = time(0);
        int           undead = (switchState == SWITCH_CHILD_RUNNING
                                && switchChildKillSent);

        if (!undead) {
            troubleSince = lastReminder = 0;
        } else if (troubleSince == 0) {
            troubleSince = lastReminder = t;
        } else if (t - lastReminder >= SWITCH_REMIND_INTERVAL) {
            lastReminder = t;
            ls_syslog(LOG_ERR, "%s: lsb.events switch process <%d> \
has not exited for over %d minutes after being killed", __func__,
                      (int)switchChildPid, (int)((t - troubleSince) / 60));
        }
    }

    if (lastSwitchDoneTime == 0)
        lastSwitchDoneTime = time(0);

    /*
     * Step 4: a parked round is retried here, on its own timer: an idle
     * cluster cleans no jobs, and would otherwise keep the round - and the
     * events piling up in the delta - until the next restart.  The first
     * attempt is immediate.
     */
    {
        static time_t lastTry = 0;

        if (switchState != SWITCH_STALLED || mSchedStage == M_STAGE_REPLAY) {
            lastTry = 0;
        } else if (lastTry == 0
                   || time(0) - lastTry >= SWITCH_RETRY_INTERVAL) {
            lastTry = time(0);
            switchLogRecover(RECOVER_EXPECT_ANY);
        }
    }

    /* Step 5: nothing to do until MAX_JOB_NUM finished jobs were cleaned */
    if (numRemoveJobs < maxjobnum)
        return;

    /*
     * MIN_SWITCH_PERIOD: no new round until that long after the last one
     * completed, or many jobs expiring at once make a busy cluster rewrite
     * lsb.events back to back.  The counter keeps counting meanwhile.
     */
    if (minSwitchPeriod > 0
        && (switchState == SWITCH_CHILD_RUNNING
            || time(0) - lastSwitchDoneTime < minSwitchPeriod))
        return;

    /*
     * Step 6: the switch is always done by a child.  During the startup
     * replay no child can be forked yet: the count is kept and the first
     * periodicCheck() starts the round.
     */
    if (mSchedStage == M_STAGE_REPLAY)
        return;

    if (forkSwitchLog() == 0) {
        /*
         * restart the count now: the jobs cleaned while the child runs are
         * what the next switch is decided on
         */
        numRemoveJobs = 0;
    } else {
        /* busy or not startable: back off instead of re-checking on
         * every periodicCheck() */
        numRemoveJobs = numRemoveJobs / 2;
    }
}

void
checkAcctLog(void)
{
    int             needArchive = 0;
    time_t          now;
    struct stat     jbuf;
    static time_t   lastAcctCreationTime = -1;

#define DAYSECONDS      (24*60*60)
#define KILOBYTE        1024

    if (acctArchiveInDays > 0 || acctArchiveInSize > 0) {
        if (stat(jlogFname, &jbuf) != 0){
            lastAcctCreationTime = -1;
            return;
        }

        if (acctArchiveInDays > 0) {
            now = time(0);

            if (lastAcctCreationTime <= 0) {
                lastAcctCreationTime = lsb_getAcctFileTime(jlogFname);
            }

            if ((lastAcctCreationTime > 0)
                && ((now - lastAcctCreationTime) > (acctArchiveInDays*DAYSECONDS))) {
                needArchive = 1;
            }
        }

        if ((!needArchive) && (acctArchiveInSize > 0) && (jbuf.st_size > (acctArchiveInSize*KILOBYTE))){
            needArchive = 1;
        }

        if (needArchive){
            switchAcctLog();
            if (acctArchiveInDays > 0){
                lastAcctCreationTime = now;
            }
        }
    }
}

int
switchAcctLog(void)
{
    static char fname[] = "switchAcctLog";
    int errnoSv;
    int totalAcctFile;

    chuser(managerId);

    if (createAcct0File() == -1) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "createAcct0File");
        goto Acct_exiterr;
    }


    if (unlink(jlogFname) == -1 ) {
        chuser(batchId);
        ls_syslog(LOG_ERR,
                  "%s: unlink(%s) failed: %m",
                  fname,
                  jlogFname);
        chuser(managerId);
    }

    if ((joblog_fp = fopen(jlogFname, "a")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen",
                  jlogFname);
        goto Acct_exiterr;
    }

    FCLOSEUP(&joblog_fp);

    errnoSv = errno;
    chmod(jlogFname, 0644);

    chuser(batchId);
    errno = errnoSv;

    if ((totalAcctFile = renameAcctLogFiles(maxAcctArchiveNum)) > 0)
        return (0);

    chuser(batchId);
    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "renameAcctLogFiles");

Acct_exiterr:
    lsb_merr( _i18n_msg_get(ls_catd, NL_SETN, 1701,
                            "Fail to switch lsb.acct; see mbatchd error log for details")); /* catgets 1701 */
    return (-1);
}

/*
 * Scan the active event file and copy every record that is still needed
 * for state recovery into the new baseline file.
 *
 * The caller must have initialized the host/queue control tables and is
 * responsible for destroying them afterwards.  In child mode (inChild)
 * no mbdDie() is done: the failure is reported to the caller which then
 * _exit()s with the proper switch child exit code.
 *
 * Returns 0 on success, -1 on failure.
 */
/*
 * Scan the frozen event file and write the baseline.  In the switch child
 * this is the longest step: it must not call chuser(), which would clear
 * the PR_SET_PDEATHSIG that the caller armed, and that signal is what stops
 * an orphaned child during the scan.
 */
static int
writeBaselineEvents(FILE *efp, FILE *tmpfp, char *tmpfn, int inChild)
{
    static char             fname[] = "writeBaselineEvents";
    int                     lineNum = 0;
    LS_LONG_INT             jobId = 0;
    struct jData           *jp, *jarray;
    int                     preserved = FALSE;

    if (lsberrno == LSBE_EOF)
        lsberrno = LSBE_NO_ERROR;

    while (lsberrno != LSBE_EOF) {

        if ((logPtr = lsb_geteventrec(efp, &lineNum)) == NULL) {
            if (lsberrno != LSBE_EOF) {
                ls_syslog(LOG_ERR, "\
%s: reading line %d in file %s: %s", fname, lineNum,
                     elogFname, lsb_sysmsg());

                if (lsberrno == LSBE_NO_MEM) {
                    if (inChild)
                        return -1;
                    mbdDie(MASTER_MEM);
                }

            } else {

                break;
            }
            continue;
        }


        eventTime = logPtr->eventTime;

        switch (logPtr->type) {
            case EVENT_JOB_NEW:
            case EVENT_JOB_MODIFY:
                jobId = LSB_JOBID(logPtr->eventLog.jobNewLog.jobId,
                                  logPtr->eventLog.jobNewLog.idx);
                break;
            case EVENT_JOB_MODIFY2:
            {
                struct idxList *idxListP;
                int tmpJobId;
                if (getJobIdIndexList(logPtr->eventLog.jobModLog.jobIdStr,
                                      &tmpJobId, &idxListP) == LSBE_NO_ERROR)
                    jobId = tmpJobId;
                freeIdxList(idxListP);
                break;
            }
            case EVENT_PRE_EXEC_START:
            case EVENT_JOB_START:
                jobId = LSB_JOBID(logPtr->eventLog.jobStartLog.jobId,
                                  logPtr->eventLog.jobStartLog.idx);
                break;
            case EVENT_JOB_STATUS:
                jobId = LSB_JOBID(logPtr->eventLog.jobStatusLog.jobId,
                                  logPtr->eventLog.jobStatusLog.idx);
                break;
            case EVENT_JOB_SWITCH:
                jobId = LSB_JOBID(logPtr->eventLog.jobSwitchLog.jobId,
                                  logPtr->eventLog.jobSwitchLog.idx);
                break;
            case EVENT_JOB_MOVE:
                jobId = LSB_JOBID(logPtr->eventLog.jobMoveLog.jobId,
                                  logPtr->eventLog.jobMoveLog.idx);
                break;
            case EVENT_MIG:
                jobId = LSB_JOBID(logPtr->eventLog.migLog.jobId,
                                  logPtr->eventLog.migLog.idx);
                break;
            case EVENT_JOB_ATTR_SET:
                jobId = LSB_JOBID(logPtr->eventLog.jobAttrSetLog.jobId,
                                  logPtr->eventLog.jobAttrSetLog.idx);
                break;
            case EVENT_CHKPNT:
                jobId = LSB_JOBID(logPtr->eventLog.chkpntLog.jobId,
                                  logPtr->eventLog.chkpntLog.idx);
                break;
            case EVENT_JOB_SIGACT:
                jobId = LSB_JOBID(logPtr->eventLog.sigactLog.jobId,
                                  logPtr->eventLog.sigactLog.idx);
                break;
            case EVENT_JOB_SIGNAL:
                jobId = LSB_JOBID(logPtr->eventLog.signalLog.jobId,
                                  logPtr->eventLog.signalLog.idx);
                break;
            case EVENT_JOB_REQUEUE:
                jobId = LSB_JOBID(logPtr->eventLog.jobRequeueLog.jobId,
                                  logPtr->eventLog.jobRequeueLog.idx);
                break;
            case EVENT_JOB_CLEAN:
                jobId = LSB_JOBID(logPtr->eventLog.jobCleanLog.jobId,
                                  logPtr->eventLog.jobCleanLog.idx);
                break;
            case EVENT_MBD_UNFULFILL:
                jobId = LSB_JOBID(logPtr->eventLog.unfulfillLog.jobId,
                                  logPtr->eventLog.unfulfillLog.idx);
                break;
            case EVENT_LOG_SWITCH:
            case EVENT_LOAD_INDEX:
                preserved  = TRUE;
                break;
            case EVENT_JOB_EXECUTE:
                jobId = LSB_JOBID(logPtr->eventLog.jobExecuteLog.jobId,
                                  logPtr->eventLog.jobExecuteLog.idx);
                break;
            case EVENT_JOB_START_ACCEPT:
                jobId = LSB_JOBID(logPtr->eventLog.jobStartAcceptLog.jobId,
                                  logPtr->eventLog.jobStartAcceptLog.idx);
                break;
            case EVENT_JOB_MSG:
                jobId = LSB_JOBID(logPtr->eventLog.jobMsgLog.jobId,
                                  logPtr->eventLog.jobMsgLog.idx);
                break;
            case EVENT_JOB_MSG_ACK:
                jobId = LSB_JOBID(logPtr->eventLog.jobMsgAckLog.jobId,
                                  logPtr->eventLog.jobMsgAckLog.idx);
                break;
            case EVENT_HOST_CTRL:
                saveHostCtrlEvent(&(logPtr->eventLog.hostCtrlLog),
                                  eventTime);
                break;
            case EVENT_QUEUE_CTRL:
                saveQueueCtrlEvent(&(logPtr->eventLog.queueCtrlLog),
                                   eventTime);
                break;
            default:
                break;
        }


        jarray = checkJobInCore(LSB_ARRAY_JOBID(jobId));


        jp = checkJobInCore(jobId);

        if ((preserved)
            || (jarray != NULL
                && ((canSwitch(logPtr, jp) == FALSE)
                    || (logPtr->type == EVENT_JOB_NEW)
                    || (logPtr->type == EVENT_JOB_MODIFY)
                    || (logPtr->type == EVENT_JOB_MODIFY2)
                    || (logPtr->type == EVENT_JOB_SWITCH)
                    || (logPtr->type == EVENT_JOB_MOVE)
                    || (logPtr->type == EVENT_JOB_CLEAN)))) {

            if (lsb_puteventrec(tmpfp, logPtr) == -1) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_MM,
                          fname, "lsb_puteventrec", tmpfn);
                return -1;
            }
        }
        jobId = 0;
        preserved = FALSE;
    }

    return 0;
}

int
switch_log(void)
{
    static char fname[] = "switch_log";
    sigset_t                newmask, oldmask;
    char                    tmpfn[MAXFILENAMELEN];
    int                     i, errnoSv;
    FILE                   *efp, *tmpfp;
    long                   pos;
    int                    totalEventFile;
    char                   indexFile[MAXFILENAMELEN];
    /*
     * Step 1a: refuse to run while the previous index rebuild runs.
     * When switch_log is triggered frequently, multiple child
     * processes may simultaneously read and write lsb.event.index. It is
     * necessary to ensure that only one child process operates on this index
     * file at any given time.
     */
    if (eventIndexRebuildPid != 0) {
        ls_syslog(LOG_DEBUG, "%s: event index rebuild pid=%d still running", __func__, (int)eventIndexRebuildPid);
        return -2;
    }

    /*
     * Step 1b: never on top of an unfinished fork based round, whose files
     * this would overwrite.  Checked here, not in switchELog(), because
     * init_log() calls switch_log() directly after a replay error.
     */
    if (switchState != SWITCH_IDLE) {
        ls_syslog(LOG_ERR, "\
%s: Skipped: an interrupted lsb.events switch is not completed yet", fname);
        return -1;
    }

    sprintf(tmpfn, "%s/logdir/lsb.events",
            daemonParams[LSB_SHAREDIR].paramValue);

    ls_syslog(LOG_INFO, "\
%s: switching event log file: %s", __FUNCTION__, tmpfn);

    /* Step 2: archive segment of the active file */
    chuser(managerId);

    if (createEvent0File() == -1) { ;
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "createEvent0File");
        goto exiterr;
    }

    /* Step 3: copy the records still needed for replay into .tmp */
    sprintf(tmpfn, "%s/logdir/lsb.events.tmp",
            daemonParams[LSB_SHAREDIR].paramValue);

    if ((tmpfp = fopen(tmpfn, "w")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", tmpfn);
        goto exiterr;
    }

    if (fchmod(fileno(tmpfp),  0644) != 0) {
        ls_syslog(LOG_ERR, I18N(6870,"%s: fchmod on %s failed: %m"), fname, tmpfn);/*catgets 6870 */
    }

    efp = fopen(elogFname, "r");
    if (efp == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", elogFname);
        FCLOSEUP(&tmpfp);
        goto exiterr;
    }

    fprintf(tmpfp, ELOG_BASELINE_HEADER);

    chuser(batchId);

    initHostCtrlTable();
    initQueueCtrlTable();

    if (writeBaselineEvents(efp, tmpfp, tmpfn, FALSE) < 0) {
        chuser(managerId);
        FCLOSEUP(&efp);
        FCLOSEUP(&tmpfp);
        unlink (tmpfn);
        chuser(batchId);
        destroyHostCtrlTable();
        destroyQueueCtrlTable();
        goto exiterr;
    }

    FCLOSEUP(&efp);

    /* Step 4: history seek position = end of the baseline */
    chuser(managerId);

    /*
     * Two barriers, as in the child: the content must be on disk before the
     * offset claims where it ends, and the offset before the rename makes
     * this file the active one - otherwise a crash publishes a truncated
     * lsb.events
     */
    if (fflush(tmpfp) != 0 || fsync(fileno(tmpfp)) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fsync", tmpfn);
        FCLOSEUP(&tmpfp);
        goto exiterr;
    }

    pos = ftell(tmpfp);
    rewind(tmpfp);
    fprintf(tmpfp, "#%ld", pos);

    if (fflush(tmpfp) != 0 || fsync(fileno(tmpfp)) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fsync", tmpfn);
        FCLOSEUP(&tmpfp);
        goto exiterr;
    }

    if (FCLOSEUP(&tmpfp)) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fclose", tmpfn);
        goto exiterr;
    }

    /* Step 5: publish the new active file */
    i = rename(tmpfn, elogFname);
    errnoSv = errno;
    chmod(elogFname, 0644);
    if (i == 0)
        switchLogSyncDir();

    chuser(batchId);
    errno = errnoSv;
    if (i == -1) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M,
                  fname,
                  "rename",
                  tmpfn,
                  elogFname);
        goto exiterr;
    }

    /* Step 6: state records, appended to the new active file */
    writeHostCtrlEvent(NULL);
    destroyHostCtrlTable();

    writeQueueCtrlEvent(NULL);
    destroyQueueCtrlTable();

    if (mSchedStage == M_STAGE_REPLAY)
        log_logSwitch(nextJobId_t);
    else
        log_logSwitch(nextJobId);
    /* Step 7: rotate the archives; Step 8: rebuild the index in a child */
    if ((totalEventFile = renameElogFiles()) > 0)  {
        sigemptyset(&newmask);
        sigaddset(&newmask, SIGCHLD);
        sigprocmask(SIG_BLOCK, &newmask, &oldmask);

        eventIndexRebuildPid = fork();

        if (eventIndexRebuildPid > 0) {
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return (0);
        }

        sigprocmask(SIG_SETMASK, &oldmask, NULL);

        if (eventIndexRebuildPid == 0) {
            closeExceptFD(-1);

            sprintf(indexFile, "%s/logdir/%s",
                    daemonParams[LSB_SHAREDIR].paramValue,
                    LSF_JOBIDINDEX_FILENAME);

            chuser(managerId);
            if (updateJobIdIndexFile(indexFile, elogFname, totalEventFile) < 0) {
                chuser(batchId);
                if (lsberrno == LSBE_SYS_CALL)
                    ls_syslog(LOG_ERR, "\
%s: updateJobIdIndexFile(%s) failed: %s %m",
                              fname, indexFile, lsb_sysmsg());
                else
                    ls_syslog(LOG_ERR, "\
%s: updateJobIdIndexFile(%s) failed: %s",
                              fname, indexFile, lsb_sysmsg());
            }
            exit(0);
        }
        return (0);
    } else {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "renameElogFiles");
    }

exiterr:
    lsb_merr("\
Fail to switch lsb.events; see mbatchd error log for details");
    return (-1);
}

/*
 * ======================================================================
 * Event log switch in a child process
 * ======================================================================
 */

/*
 * Read the two leading numbers of an event file header: the history seek
 * offset of a complete baseline, and the recorded merge point of the delta
 * (see writeDeltaMergeOffset()).  Existing readers only parse the first.
 *
 * @param[in] path: Event file to inspect.
 * @param[out] first: First header number, untouched if absent; may be NULL.
 * @param[out] second: Second header number, untouched if absent; may be NULL.
 * @return: count of numbers present (0, 1 or 2), -1 if the file does not
 *          start with '#', ELOG_HEADER_UNREADABLE with errno set if it
 *          cannot be opened or read - not to be taken as "raw".
 */
static int
readHeaderNumbers(const char *path, long *first, long *second)
{
    FILE *fp;
    long  a = 0;
    long  b = 0;
    char  ch = 0;
    int   cc;

    if ((fp = fopen(path, "r")) == NULL)
        return ELOG_HEADER_UNREADABLE;

    /* %c%ld %ld stops at the first unconvertible field, so the return
     * value doubles as "how much of the header is filled in" */
    cc = fscanf(fp, "%c%ld %ld", &ch, &a, &b);
    if (ferror(fp)) {
        int saveErrno = errno;

        FCLOSEUP(&fp);
        errno = saveErrno;
        return ELOG_HEADER_UNREADABLE;
    }
    FCLOSEUP(&fp);

    if (cc < 1 || ch != '#')
        return -1;

    if (cc >= 2 && first != NULL)
        *first = a;
    if (cc >= 3 && second != NULL)
        *second = b;

    return cc - 1;
}

/*
 * Record in the delta header the length the merge target (.tmp forward,
 * lsb.events on a rollback) had before the merge, so that a retry cuts
 * back to it instead of appending twice.  Must be on disk before the first
 * byte is appended.
 *
 * @param[in] baselineEnd: Length of the merge target before the merge.
 * @return: 0 on success, -1 on failure.
 */
static int
writeDeltaMergeOffset(off_t baselineEnd)
{
    static char fname[] = "writeDeltaMergeOffset";
    char        buf[64];
    int         len;
    int         fd;

    /* the header openEventFile() wrote is 80 bytes wide; "#80 " plus a
     * decimal off_t cannot reach the terminating newline */
    len = sprintf(buf, "#80 %lld", (long long)baselineEnd);

    if ((fd = open(deltaElogFname, O_WRONLY)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "open",
                  deltaElogFname);
        return -1;
    }

    if (pwrite(fd, buf, len, 0) != len || fsync(fd) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "pwrite",
                  deltaElogFname);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/*
 * Read the merge point recorded in the delta header and check that it
 * can be cut back to.  A usable point lies inside the target, behind its
 * header, right after a complete record; anything else is a damaged
 * record and is treated as "nothing recorded", which never truncates.
 * @param[in] target: Merge target, lsb.events or lsb.events.tmp.
 * @param[out] point: The recorded point, set only when 1 is returned.
 * @return: 1 if a usable point is recorded, 0 if none is, -1 with errno
 *          set if the delta or the target cannot be read.
 */
static int
readDeltaMergeOffset(const char *target, off_t *offset)
{
    struct stat st;
    long        recorded = -1;
    char        ch;
    int         cc;
    int         fd;

    cc = readHeaderNumbers(deltaElogFname, NULL, &recorded);
    if (cc == ELOG_HEADER_UNREADABLE)
        return -1;
    if (cc < 2)
        return 0;

    if (stat(target, &st) < 0)
        return -1;
    if (recorded < (long)(sizeof(ELOG_BASELINE_HEADER) - 1)
        || recorded > (long)st.st_size)
        return 0;

    /* every record ends with a newline, so does every header */
    if ((fd = open(target, O_RDONLY)) < 0)
        return -1;
    cc = pread(fd, &ch, 1, (off_t)recorded - 1);
    if (cc < 0) {
        int saveErrno = errno;

        close(fd);
        errno = saveErrno;
        return -1;
    }
    close(fd);
    if (cc != 1 || ch != '\n')
        return 0;

    *offset = (off_t)recorded;
    return 1;
}

/*
 * Append the complete records of lsb.events.delta to an event file.  A
 * half record at the end of the delta is left out, or it would be glued to
 * the next record appended there; a delta without a complete header line
 * counts as empty.  Run as the manager, from the main loop (nothing can
 * grow the delta meanwhile).
 *
 * @param[in] dstPath: Event file to append to.
 * @param[in] truncateTo: Cut dstPath back to this length first; -1 to
 *                        append at the current end.
 * @param[out] copied: Bytes appended; may be NULL.
 * @return: 0 on success, -1 on failure.
 */
static int
mergeDeltaEvents(const char *dstPath, off_t truncateTo, off_t *copied)
{
    static char fname[] = "mergeDeltaEvents";
    FILE       *dfp = NULL;
    FILE       *dstfp = NULL;
    char        buf[MSGSIZE];
    struct stat st;
    off_t       total = 0;
    off_t       recordsEnd;
    off_t       pos;
    size_t      cc;

    if (copied != NULL)
        *copied = 0;

    if ((dfp = fopen(deltaElogFname, "r")) == NULL) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fopen",
                  deltaElogFname);
        return -1;
    }

    /* Step 1: skip the header; without its newline there is no record */
    if (fgets(buf, sizeof(buf), dfp) == NULL
        || strchr(buf, '\n') == NULL) {
        int readError = ferror(dfp);
        int saveErrno = errno;

        FCLOSEUP(&dfp);
        if (readError) {
            errno = saveErrno;
            ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fgets",
                       deltaElogFname);
            return -1;
        }
        return 0;
    }

    /*
     * Step 2: where the last complete record ends (pread() does not move
     * the stream)
     */
    pos = ftell(dfp);
    if (fstat(fileno(dfp), &st) < 0
        || (recordsEnd = lastLineEnd(fileno(dfp), st.st_size)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "pread",
                  deltaElogFname);
        FCLOSEUP(&dfp);
        return -1;
    }

    /* no complete record behind the header: nothing to merge */
    if (recordsEnd <= pos) {
        FCLOSEUP(&dfp);
        return 0;
    }

    /* Step 3: prepare the target - cut it back if asked, seek to its end */
    if ((dstfp = fopen(dstPath, "r+")) == NULL) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fopen", dstPath);
        FCLOSEUP(&dfp);
        return -1;
    }

    if (truncateTo >= 0 && ftruncate(fileno(dstfp), truncateTo) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "ftruncate",
                  dstPath);
        goto append_failed;
    }

    if (fseek(dstfp, 0, SEEK_END) != 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fseek", dstPath);
        goto append_failed;
    }

    /* Step 4: plain byte copy up to the last complete record */
    while (pos < recordsEnd) {
        size_t want = sizeof(buf);

        if ((off_t)want > recordsEnd - pos)
            want = (size_t)(recordsEnd - pos);

        if ((cc = fread(buf, 1, want, dfp)) == 0) {
            ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fread",
                      deltaElogFname);
            goto append_failed;
        }

        if (fwrite(buf, 1, cc, dstfp) != cc) {
            ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fwrite",
                      dstPath);
            goto append_failed;
        }

        pos += cc;
        total += cc;
    }
    FCLOSEUP(&dfp);

    /* Step 5: make the appended records durable */
    if (fflush(dstfp) != 0 || fsync(fileno(dstfp)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fsync", dstPath);
        FCLOSEUP(&dstfp);
        return -1;
    }

    if (FCLOSEUP(&dstfp) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fclose", dstPath);
        return -1;
    }

    if (copied != NULL)
        *copied = total;

    return 0;

append_failed:
    if (dfp != NULL)
        FCLOSEUP(&dfp);
    if (dstfp != NULL)
        FCLOSEUP(&dstfp);
    return -1;
}

/*
 * Remove the files of a round and route events back to lsb.events.
 * Removing .tmp is the commit point (without it, leftovers read as cleanup
 * work); if it fails nothing else is changed and the round is parked.
 * @return: 0 on success, -1 with errno set if .tmp could not be removed.
 */
static int
switchLogDropRound(void)
{

    chuser(managerId);

    /* Step 1: the commit point */
    if (unlink(tmpElogFname) < 0 && errno != ENOENT) {
        chuser(batchId);
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, __func__, "unlink",
                   tmpElogFname);
        switchState = SWITCH_STALLED;
        return -1;
    }

    /*
     * Step 2: the rest is cleanup.  A failure here leaves a file behind,
     * which the next round refuses to start on (see switchLogBeginRound()),
     * so it is reported rather than hidden
     */
    if ((unlink(event0Fname) < 0 && errno != ENOENT)
        || (unlink(deltaElogFname) < 0 && errno != ENOENT)) {
        int saveErrno = errno;

        chuser(batchId);
        ls_syslog(LOG_WARNING, "\
%s: cannot remove the leftovers of the switch: %m; it is retried on the \
next round", __func__);
        errno = saveErrno;
        switchState = SWITCH_STALLED;
        return -1;
    }
    chuser(batchId);

    /* Step 3: back to normal operation */
    elogWriteTarget = elogFname;
    switchState = SWITCH_IDLE;
    return 0;
}

/*
 * Find where the last complete line of a file ends, scanning backwards
 * in blocks so that only the tail of a large file is read.
 * @param[in] fd: File to scan; its offset is not moved.
 * @param[in] size: Length of the file.
 * @return: offset right behind the last newline, 0 if there is none,
 *          -1 with errno set on a read error.
 */
static off_t
lastLineEnd(int fd, off_t size)
{
    char  buf[MSGSIZE];
    off_t end;

    for (end = size; end > 0; end -= (off_t)sizeof(buf)) {
        size_t  want = (off_t)sizeof(buf) > end ? (size_t)end : sizeof(buf);
        size_t  i;

        if (pread(fd, buf, want, end - (off_t)want) != (ssize_t)want)
            return -1;
        for (i = want; i > 0; i--)
            if (buf[i - 1] == '\n')
                return end - (off_t)want + (off_t)i;
    }
    return 0;
}

/*
 * Cut half a record off the end of a file (an unsynced tail lost with the
 * power), so that anything appended is not glued to it.
 *
 * @param[in] path: File to trim in place.
 * @param[out] size: Resulting file size; may be NULL.
 * @return: 0 on success, -1 on failure with the file unchanged.
 */
static int
trimPartialRecord(const char *path, off_t *size)
{
    static char fname[] = "trimPartialRecord";
    struct stat st;
    off_t       end;
    int         fd;

    if ((fd = open(path, O_RDWR)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "open", path);
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fstat", path);
        close(fd);
        return -1;
    }

    if ((end = lastLineEnd(fd, st.st_size)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "pread", path);
        close(fd);
        return -1;
    }

    if (end != st.st_size) {
        ls_syslog(LOG_WARNING, "\
%s: lsb.events ends with an incomplete record; removed %lld bytes",
              fname, (long long)(st.st_size - end));

        if (ftruncate(fd, end) < 0 || fsync(fd) < 0) {
            ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "ftruncate",
                      path);
            close(fd);
            return -1;
        }
    }

    close(fd);
    if (size != NULL)
        *size = end;
    return 0;
}

/*
 * Roll a raw round back: no archive was touched, so only the delta events
 * have to go back to lsb.events.  Like the forward merge, the length of
 * lsb.events (trimmed first) is recorded in the delta header before the
 * append, and a retry cuts back to it.  Removing the files is up to the
 * caller.
 * @return: 0 on success, -1 with errno set, nothing deleted.
 */
static int
switchLogRollback(void)
{
    static char fname[] = "switchLogRollback";
    struct stat st;
    int         recorded;
    off_t       truncateTo = -1;
    off_t       copied = 0;

    chuser(managerId);

    /*
     * Step 1: drop the half built .0 first - on a full disk it is what fills
     * it.  Safe while .tmp is raw: nothing was rotated.  .tmp must stay, or the
     * delta would read as the leftover of a finished publish.
     */
    if (unlink(event0Fname) < 0 && errno != ENOENT)
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "unlink",
                   event0Fname);

    /* a round is only rolled back with its delta file present */
    if (stat(deltaElogFname, &st) < 0) {
        chuser(batchId);
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "stat",
                   deltaElogFname);
        return -1;
    }

    if (st.st_size > 0) {

        /*
         * Step 2: find (an earlier attempt died half way) or record where
         * lsb.events ended before the merge
         */
        if ((recorded = readDeltaMergeOffset(elogFname, &truncateTo)) < 0) {
            chuser(batchId);
            return -1;
        }

        if (!recorded) {
            if (trimPartialRecord(elogFname, &truncateTo) < 0
                || writeDeltaMergeOffset(truncateTo) < 0) {
                chuser(batchId);
                return -1;
            }
        }

        /* Step 3: cut back to that point and append the whole delta */
        if (mergeDeltaEvents(elogFname, truncateTo, &copied) < 0) {
            chuser(batchId);
            return -1;
        }
    }

    chuser(batchId);

    ls_syslog(LOG_DEBUG, "%s: %lld bytes of delta events merged back into %s",
               fname, (long long)copied, elogFname);
    return 0;
}

/*
 * Publish lsb.events.tmp as lsb.events and close the round.  The rename
 * is the atomic commit point; the route must be back on lsb.events before
 * the delta is unlinked, which recovery relies on.
 *
 * @param[in] fname: Caller name for the log lines.
 * @return: 0 on success, -1 on failure with the old active file intact.
 */
static int
switchLogPublish(const char *fname)
{
    /* Step 1: the commit point */
    chuser(managerId);

    if (rename(tmpElogFname, elogFname) < 0) {
        chuser(batchId);
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_S_FAIL_M, fname, "rename",
                  tmpElogFname, elogFname);
        return -1;
    }
    chmod(elogFname, 0644);

    /* Step 2: the new active file is live, route the events back to it */
    elogWriteTarget = elogFname;

    /* Step 3: only now the delta can go (see the order rule above) */
    if (unlink(deltaElogFname) < 0 && errno != ENOENT)
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "unlink",
                  deltaElogFname);

    /* the rename and the unlink above are directory entries */
    switchLogSyncDir();

    chuser(batchId);
    /* Step 4: the round is over; MIN_SWITCH_PERIOD counts from here */
    switchState = SWITCH_IDLE;
    lastSwitchDoneTime = time(0);

    /* numRemoveJobs stays: it counts the jobs cleaned since the round started */
    return 0;
}

/*
 * Finish a round whose baseline is complete: complete an interrupted
 * rotation, merge the delta
 * and publish.  Also the normal commit path, with the rotation already
 * done.  No progress record is needed: renameElogFiles() resumes at the
 * gap the interrupted rotation left.  Here the remaining renames run in the
 * main process, accepted for this rare path.
 *
 * @param[in] expectDelta: TRUE when the delta file is known to exist; a
 *                         missing one is then an error, not "no events".
 * @return: 0 on success, -1 with errno set, every file left in place.
 */
static int
switchLogRollForward(int expectDelta)
{
    static char fname[] = "switchLogRollForward";
    struct stat st;
    off_t       truncateTo = -1;
    off_t       copied = 0;
    int         haveDelta;
    int         haveZero;
    int         recorded;

    chuser(managerId);

    /*
     * Step 1: an unfinished rotation (.0 exists) is completed; the index is
     * rebuilt after the publish, like in a normal round
     */
    haveZero = (stat(event0Fname, &st) == 0);
    if (!haveZero && errno != ENOENT) {
        /* not knowing would publish without rotating: the archive
         * segment of the old active file would be lost */
        chuser(batchId);
        return -1;
    }

    if (haveZero) {

        ls_syslog(LOG_DEBUG, "\
%s: finishing the archive rotation of an interrupted switch", fname);

        if (renameElogFiles() <= 0) {
            chuser(batchId);
            return -1;
        }

        chuser(batchId);
    }

    /*
     * Step 2: merge the delta into .tmp, cutting a half merged attempt off
     * first, or recording the merge point before the first append
     */
    /*
     * a missing delta is only fine when the caller knows it is gone; any
     * other failure keeps the round, or its events would be published away
     */
    haveDelta = (stat(deltaElogFname, &st) == 0);
    if (!haveDelta && (errno != ENOENT || expectDelta)) {
        chuser(batchId);
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "stat",
                   deltaElogFname);
        return -1;
    }

    if (haveDelta && st.st_size > 0) {
        struct stat tmpSt;

        if (stat(tmpElogFname, &tmpSt) < 0) {
            chuser(batchId);
            return -1;
        }

        if ((recorded = readDeltaMergeOffset(tmpElogFname, &truncateTo)) < 0
            || (!recorded && writeDeltaMergeOffset(tmpSt.st_size) < 0)) {
            chuser(batchId);
            return -1;
        }

        if (mergeDeltaEvents(tmpElogFname, truncateTo, &copied) < 0) {
            chuser(batchId);
            return -1;
        }
    }

    chuser(batchId);

    /* Step 3: publish */
    if (switchLogPublish(fname) < 0)
        return -1;

    /* Step 4: the index, in a child: it reads every archive */
    switchLogRebuildIndex();

    ls_syslog(LOG_DEBUG, "%s: %s published, %lld bytes of delta events merged",
               fname, elogFname, (long long)copied);
    return 0;
}

/*
 * Make the directory entries of a round durable.  fsync() on a file does
 * not cover the name it was published under or the removal of the delta,
 * so the commit point needs the directory here; on NFS a directory fsync
 * is commonly a no-op and the server commits the rename itself.
 */
static void
switchLogSyncDir(void)
{
    static char fname[] = "switchLogSyncDir";
    char        path[MAXFILENAMELEN];
    int         fd;

    sprintf(path, "%s/logdir", daemonParams[LSB_SHAREDIR].paramValue);

    if ((fd = open(path, O_RDONLY)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "open", path);
        return;
    }

    if (fsync(fd) < 0)
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fsync", path);

    close(fd);
}

/*
 * How many archive files exist; only stat()s, nothing is renamed.
 */
static int
switchLogCountArchives(void)
{
    char        path[MAXFILENAMELEN];
    struct stat st;
    int         total = 0;

    while (1) {
        sprintf(path, "%s/logdir/lsb.events.%d",
                daemonParams[LSB_SHAREDIR].paramValue, total + 1);
        if (stat(path, &st) < 0)
            break;
        total++;
    }

    return total;
}

/*
 * Rebuild lsb.events.index in a detached child.  The index is derived data
 * and the rebuild reads every archive, so it must not hold up the main
 * loop: the round is published first, and the next switch rebuilds the
 * index again if this child dies or leaves it behind.
 */
static void
switchLogRebuildIndex(void)
{
    static char fname[] = "switchLogRebuildIndex";
    char        indexFile[MAXFILENAMELEN];
    sigset_t    newmask, oldmask;
    pid_t       pid;

    if (switchIndexPid != 0)
        return;                             /* one rebuild at a time */

    /* block SIGCHLD around fork and registration: the child may exit before
     * fork() returns, and its status is what clears the pid (see
     * switchLogChildReaped()) */
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    if ((pid = fork()) < 0) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        ls_syslog(LOG_DEBUG, "%s: fork: %m; the index is rebuilt by the next switch",
                  fname);
        return;
    }

    if (pid > 0) {
        switchIndexPid = pid;
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return;
    }

    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    {
        int total;

        closeExceptFD(-1);
        Signal_(SIGTERM, SIG_DFL);
        Signal_(SIGINT,  SIG_DFL);
        Signal_(SIGCHLD, SIG_DFL);
        chuser(managerId);

        total = switchLogCountArchives();
        sprintf(indexFile, "%s/logdir/%s",
                daemonParams[LSB_SHAREDIR].paramValue, LSF_JOBIDINDEX_FILENAME);
        if (updateJobIdIndexFile(indexFile, elogFname, total) < 0)
            ls_syslog(LOG_ERR, "\
%s: updateJobIdIndexFile(%s) failed: %s; rebuilt when the next switch is due",
                      fname, indexFile, lsb_sysmsg());

        _exit(0);
    }
}

/*
 * Undo a failed rollback: cut lsb.events back to the recorded length, so
 * that a parked round has its events in the delta only and a restart does
 * not replay them twice (switchLogReplayDelta()).  Safe: lsb.events is not
 * written while the round is parked.
 */
static void
switchLogUndoRollback(void)
{
    off_t mergeOffset;
    int   fd;
    int   cc;

    chuser(managerId);
    cc = readDeltaMergeOffset(elogFname, &mergeOffset);
    if (cc == 0) {
        /* nothing recorded, so nothing was appended */
        chuser(batchId);
        return;
    }

    if (cc < 0 || (fd = open(elogFname, O_WRONLY)) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, "\
%s: Fail to undo the merge of the interrupted lsb.events switch: %m; \
its events may be loaded twice after a restart", __func__);
        return;
    }

    if (ftruncate(fd, mergeOffset) < 0 || fsync(fd) < 0) {
        int saveErrno = errno;

        close(fd);
        chuser(batchId);
        errno = saveErrno;
        ls_syslog(LOG_ERR, "\
%s: Fail to undo the merge of the interrupted lsb.events switch: %m; \
its events may be loaded twice after a restart", __func__);
        return;
    }

    close(fd);
    chuser(batchId);
    ls_syslog(LOG_DEBUG, "switchLogUndoRollback: %s cut back to %lld bytes",
               elogFname, (long long)mergeOffset);
}

/*
 * Park a round whose recovery failed; it is retried from switchELog().
 * Events must go to the delta meanwhile - at startup the route still points
 * at lsb.events, whose new events a later roll forward would lose and a
 * later rollback would put in front of older delta events.
 */
static void
switchLogSuspendRound(void)
{
    elogWriteTarget = deltaElogFname;
    switchState = SWITCH_STALLED;
}

/*
 * Replay the delta after lsb.events when the startup recovery failed and
 * the round is parked; otherwise its jobs would be missing, their ids
 * handed out again and the next baseline would drop them.  Correct for
 * both ways the round can still end: a rollback appends exactly this
 * delta, a roll forward publishes lsb.events compressed plus this delta.
 */
static void
switchLogReplayDelta(void)
{
    static char fname[] = "switchLogReplayDelta";
    struct stat st;
    FILE       *fp;
    int         lineNum = 0;

    if (switchState != SWITCH_STALLED || stat(deltaElogFname, &st) < 0)
        return;

    chuser(managerId);
    fp = fopen(deltaElogFname, "r");
    chuser(batchId);

    if (fp == NULL) {
        ls_syslog(LOG_ERR, "\
%s: Fail to read the events of the interrupted lsb.events switch: %m",
              fname);
        return;
    }

    ls_syslog(LOG_DEBUG, "%s: replaying %s after %s", fname, deltaElogFname,
               elogFname);

    if (lsberrno == LSBE_EOF)
        lsberrno = LSBE_NO_ERROR;

    while (lsberrno != LSBE_EOF) {
        if ((logPtr = lsb_geteventrec(fp, &lineNum)) == NULL) {
            if (lsberrno != LSBE_EOF) {
                ls_syslog(LOG_ERR, "\
%s: Fail to read the events of the interrupted lsb.events switch at line \
%d: %s", fname, lineNum, lsb_sysmsg());
                if (lsberrno == LSBE_NO_MEM)
                    mbdDie(MASTER_MEM);
            }
            continue;
        }

        eventTime = logPtr->eventTime;
        replay_event(deltaElogFname, lineNum);
    }

    FCLOSEUP(&fp);
}

/*
 * Decide what an interrupted round left behind and finish or undo it - the
 * single recovery entry point, run at startup, after a switch child ended
 * and before a new round while one is stalled.  The action follows from
 * the files alone (see the order facts at the top of this section):
 *
 *   .tmp   .delta  .0   meaning                       action
 *   -----  ------  ---  ----------------------------  ------------------
 *   -      -       -    nothing happened              none
 *   raw    yes     any  baseline unfinished           roll back
 *   offset yes     any  baseline done                 roll forward
 *   -      yes     -    publish already succeeded     drop the delta
 *   raw    -       any  dead round, no events at risk drop the files
 *   offset -       yes  delta lost, rotation not done drop the files
 *   offset -       -    delta lost, archives rotated  publish if same
 *                                                     generation, else
 *                                                     drop
 *   -      -       yes  synchronous switch died       drop the .0
 *
 * "raw" is a baseline whose first line is still the placeholder (no digits),
 * "offset" one whose first line carries the history offset.
 *
 * A lost delta only happens by hand or after a power loss on a file system
 * without ordered metadata; its events are gone either way, so the choice
 * is made on archive consistency, and a .tmp older than lsb.events is a
 * stale leftover that must not be published.  A stat() failing with
 * anything but ENOENT leaves the round untouched until the next call.
 *
 * @param[in] expect: RECOVER_EXPECT_* - only changes how loudly a mismatch
 *                    is reported, never the action.
 * @return: 1 when a child that exited 0 left a complete round and it was
 *          published (reported by the caller), 0 otherwise.
 */
static int
switchLogRecover(int expect)
{
    static char fname[] = "switchLogRecover";
    struct stat tmpSt, activeSt;
    long        histPos = -1;
    int         haveTmp, haveDelta, haveZero;
    int         ready;
    int         saveErrno;
    int         asExpected;

    /* Step 1: the files still belong to a living child */
    if (switchState == SWITCH_CHILD_RUNNING)
        return 0;

    /* Step 2: what is on disk; an unreadable directory is not "absent" */
    switchLogStatErrno = 0;
    chuser(managerId);

    errno = 0;
    haveTmp = (stat(tmpElogFname, &tmpSt) == 0);
    saveErrno = errno;
    if (!haveTmp && saveErrno != ENOENT)
        goto unreadable;

    errno = 0;
    haveDelta = (stat(deltaElogFname, &tmpSt) == 0);
    saveErrno = errno;
    if (!haveDelta && saveErrno != ENOENT)
        goto unreadable;

    errno = 0;
    haveZero = (stat(event0Fname, &tmpSt) == 0);
    saveErrno = errno;
    if (!haveZero && saveErrno != ENOENT)
        goto unreadable;

    chuser(batchId);

    /* nothing left over: the only state a complete round never shows */
    if (!haveTmp && !haveDelta && !haveZero) {
        if (expect == RECOVER_EXPECT_COMPLETE)
            ls_syslog(LOG_ERR, "\
%s: Events logged during the last lsb.events switch may be lost", fname);
        switchState = SWITCH_IDLE;
        elogWriteTarget = elogFname;
        return 0;
    }

    /*
     * Step 3: complete = the numbers are in the baseline's first line; an
     * unreadable one is not raw (its archives may already be rotated)
     */
    ready = FALSE;
    if (haveTmp) {
        int cc;

        chuser(managerId);
        cc = readHeaderNumbers(tmpElogFname, &histPos, NULL);
        saveErrno = errno;
        chuser(batchId);
        if (cc == ELOG_HEADER_UNREADABLE)
            goto unreadable_nochuser;
        ready = (cc >= 1);
    }

    /*
     * Step 4: report a mismatch with the expectation; a failed child has
     * reported itself
     */
    switch (expect) {
        case RECOVER_EXPECT_COMPLETE:
            asExpected = (ready && haveDelta && !haveZero);
            break;
        case RECOVER_EXPECT_INTERRUPTED:
            asExpected = TRUE;
            break;
        default:
            asExpected = FALSE;
            break;
    }

    if (!asExpected) {
        if (expect == RECOVER_EXPECT_COMPLETE)
            ls_syslog(LOG_ERR, "\
%s: The lsb.events switch process reported success but did not finish; \
recovering", fname);
        else
            ls_syslog(LOG_WARNING, "\
%s: Found an interrupted lsb.events switch; recovering", fname);
    }

    /* Step 5: act on the files, following the table above */
    if (!haveTmp) {
        /*
         * a delta without .tmp: published, only the cleanup was lost; .0
         * alone: a synchronous switch died before its rotation
         */
        chuser(managerId);
        if (haveDelta && unlink(deltaElogFname) < 0 && errno != ENOENT)
            ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "unlink",
                       deltaElogFname);
        if (haveZero && unlink(event0Fname) < 0 && errno != ENOENT)
            ls_syslog(LOG_WARNING, I18N_FUNC_S_FAIL_M, fname, "unlink",
                       event0Fname);
        chuser(batchId);
        elogWriteTarget = elogFname;
        switchState = SWITCH_IDLE;

        if (haveDelta)
            ls_syslog(LOG_WARNING, "\
%s: Completed the interrupted lsb.events switch", fname);
        else
            ls_syslog(LOG_WARNING, "\
%s: Aborted the interrupted lsb.events switch; retried when the next \
switch is due", fname);
        return 0;
    }

    if (!ready) {
        /*
         * nothing rotated yet: the delta goes back to lsb.events and the round
         * is dropped; on failure the append is undone before parking, so the
         * delta events are never in both files
         */
        if ((haveDelta && switchLogRollback() < 0)
            || switchLogDropRound() < 0) {
            saveErrno = errno;
            if (haveDelta) {
                switchLogUndoRollback();
                switchLogSuspendRound();
            }
            goto failed;
        }

        ls_syslog(LOG_WARNING, "\
%s: Aborted the interrupted lsb.events switch; retried when the next \
switch is due", fname);
        return 0;
    }

    if (haveDelta) {
        if (switchLogRollForward(TRUE) < 0) {
            saveErrno = errno;
            switchLogSuspendRound();
            goto failed;
        }

        /* the normal end of a round is reported by the caller */
        if (expect == RECOVER_EXPECT_COMPLETE && asExpected)
            return 1;

        ls_syslog(LOG_WARNING, "\
%s: Completed the interrupted lsb.events switch", fname);
        return 0;
    }

    /*
     * complete, delta gone (by hand or a power loss on unordered metadata):
     * .0 only says the rotation is not finished, so the round is dropped
     */
    if (haveZero) {
        if (switchLogDropRound() < 0) {
            saveErrno = errno;
            goto failed;
        }

        ls_syslog(LOG_ERR, "\
%s: Events logged during the last lsb.events switch may be lost", fname);
        return 0;
    }

    /*
     * rotated already: publish, so the same history is not archived twice -
     * unless the .tmp is older than lsb.events
     */
    chuser(managerId);
    if (stat(tmpElogFname, &tmpSt) < 0 || stat(elogFname, &activeSt) < 0) {
        saveErrno = errno;
        chuser(batchId);
        goto unreadable_nochuser;
    }
    chuser(batchId);

    if (tmpSt.st_mtime >= activeSt.st_mtime) {
        if (switchLogRollForward(FALSE) < 0) {
            saveErrno = errno;
            switchLogSuspendRound();
            goto failed;
        }

        ls_syslog(LOG_ERR, "\
%s: Events logged during the last lsb.events switch may be lost", fname);
        return 0;
    }

    if (switchLogDropRound() < 0) {
        saveErrno = errno;
        goto failed;
    }

    ls_syslog(LOG_WARNING, "\
%s: Discarded an outdated lsb.events switch", fname);
    return 0;

failed:
    errno = saveErrno;
    ls_syslog(LOG_ERR, "\
%s: Fail to recover the interrupted lsb.events switch: %m; retried when \
the next switch is due", fname);
    return 0;

unreadable:
    chuser(batchId);
unreadable_nochuser:
    switchLogStatErrno = saveErrno;
    switchState = SWITCH_STALLED;
    errno = saveErrno;
    ls_syslog(LOG_ERR, "\
%s: Fail to check the interrupted lsb.events switch: %m; retried when \
the next switch is due", fname);
    return 0;
}

/*
 * Create this round's files and freeze the switch start time.  .tmp is
 * created with O_EXCL, its existence marks the log directory as taken.
 * @return: 0 on success, -1 on failure with every created file removed.
 */
static int
switchLogBeginRound(void)
{
    static char fname[] = "switchLogBeginRound";
    struct stat st;
    FILE       *fp;
    int         fd;
    int         haveDelta, haveZero;
    int         deltaErr, zeroErr;

    /*
     * Step 1: a round only starts on a clean slate.  Any file an earlier
     * round left behind is recovered first, never silently overwritten:
     * the delta would be truncated by fopen() below and .0 by
     * createEvent0File() in the child, which would hide the leftover.
     */
    chuser(managerId);
    errno = 0;
    haveDelta = (stat(deltaElogFname, &st) == 0);
    deltaErr = errno;
    errno = 0;
    haveZero = (stat(event0Fname, &st) == 0);
    zeroErr = errno;

    if (switchIndexPid != 0) {
        /* the index rebuild child writes the same file: wait it out */
        chuser(batchId);
        return -1;
    }

    if (haveDelta || haveZero || deltaErr != ENOENT || zeroErr != ENOENT) {
        chuser(batchId);
        switchState = SWITCH_STALLED;
        ls_syslog(LOG_DEBUG, "\
%s: %s or %s is left over from an earlier switch, or cannot be checked; \
this switch is skipped and the leftover is recovered on the next trigger",
                 fname, deltaElogFname, event0Fname);
        return -1;
    }

    /* Step 2: claim the round - create lsb.events.tmp exclusively */

    if ((fd = open(tmpElogFname, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) {
        chuser(batchId);
        if (errno == EEXIST) {
            /*
             * a leftover nobody recovers: park it, so the next trigger runs
             * switchLogRecover() instead of skipping every switch
             */
            switchState = SWITCH_STALLED;
            ls_syslog(LOG_DEBUG, "\
%s: %s already exists, so a previous switch_log did not finish; this \
switch is skipped and the leftover is recovered on the next trigger",
                 fname, tmpElogFname);
        } else
            ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "open",
                      tmpElogFname);
        return -1;
    }

    /*
     * open() is masked by the umask, which bmod sets to 0077 and never
     * restores; the published lsb.events must be 0644
     */
    chmod(tmpElogFname, 0644);

    /*
     * Step 3: create the delta, a regular event file whose header
     * openEventFile() writes
     */
    if ((fp = fopen(deltaElogFname, "w")) == NULL) {
        int saveErrno = errno;

        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fopen",
                  deltaElogFname);
        close(fd);
        chuser(batchId);
        switchLogDropRound();
        errno = saveErrno;
        return -1;
    }
    FCLOSEUP(&fp);
    chmod(deltaElogFname, 0644);

    /*
     * Both files must still exist after a power loss, or the round would
     * start with a missing one: one commit covers both directory entries
     * (the journal is file system wide; NFS commits a CREATE before it
     * replies).  A failure here only weakens that guarantee, the round can
     * still run, so it is not fatal.
     */
    if (fsync(fd) < 0)
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fsync",
                  tmpElogFname);
    close(fd);

    /* Step 4: freeze the start time used by LOG_SWITCH and the watchdog */
    switchStartTime = time(0);

    chuser(batchId);
    return 0;
}

/*
 * Switch child only: report the failure and exit.  errno is only reported
 * for a failed system call, called right after it.
 * @param[in] sysError: TRUE if the failing step was a system call.
 */
static void
switchChildAbort(int sysError)
{
    if (sysError)
        ls_syslog(LOG_ERR, "switchLogChild: Fail to switch lsb.events: %m");
    else
        ls_syslog(LOG_ERR, "switchLogChild: Fail to switch lsb.events");
    _exit(SWITCH_CHILD_EXIT_FAIL);
}

/*
 * Body of the switch child: scan the frozen lsb.events, build .0 and .tmp
 * exactly like the synchronous path, rotate the archives, rebuild the
 * index.  lsb.events cannot change meanwhile, new events go to the delta.
 * @return: never returns; _exit()s with a SWITCH_CHILD_EXIT_* code.
 */
static void
switchLogChildMain(void)
{
    static char     fname[] = "switchLogChild";
    struct eventRec rec;
    FILE           *tmpfp;
    FILE           *efp;
    long            pos;
    int             totalEventFile;

    closeExceptFD(-1);
    chuser(managerId);

    /* Step 1: archive segment of the frozen active file */
    switchChildCheckParent();
    if (createEvent0File() < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_FAIL, fname, "createEvent0File");
        switchChildAbort(TRUE);
    }

    /* Step 2: compressed baseline of the frozen active file */
    switchChildCheckParent();
    if ((tmpfp = fopen(tmpElogFname, "w")) == NULL) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fopen", tmpElogFname);
        switchChildAbort(TRUE);
    }

    if ((efp = fopen(elogFname, "r")) == NULL) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fopen", elogFname);
        switchChildAbort(TRUE);
    }

    fprintf(tmpfp, ELOG_BASELINE_HEADER);

    initHostCtrlTable();
    initQueueCtrlTable();

    if (writeBaselineEvents(efp, tmpfp, tmpElogFname, TRUE) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_FAIL, fname, "writeBaselineEvents");
        switchChildAbort(FALSE);
    }
    FCLOSEUP(&efp);

    /* Step 3: the history position is the end of the baseline */
    pos = ftell(tmpfp);

    /*
     * Step 4: host, queue and switch records; LOG_SWITCH uses the time and
     * job id frozen at the fork, never later than the delta events
     */
    writeHostCtrlEvent(tmpfp);
    writeQueueCtrlEvent(tmpfp);

    memset(&rec, 0, sizeof(rec));
    rec.eventLog.logSwitchLog.lastJobId = nextJobId;

    if (writeEventRec(tmpfp, &rec, EVENT_LOG_SWITCH, switchStartTime) < 0)
        switchChildAbort(FALSE);

    /*
     * the host and queue records do not report write errors, and fflush()
     * would not repeat them: check the stream
     */
    if (ferror(tmpfp)) {
        ls_syslog(LOG_DEBUG, "%s: writing %s failed", fname, tmpElogFname);
        switchChildAbort(FALSE);
    }

    /*
     * Step 5: every byte of the baseline on disk before the offset, which
     * recovery trusts, then the offset itself: two barriers
     */
    if (fflush(tmpfp) != 0 || fsync(fileno(tmpfp)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fsync", tmpElogFname);
        switchChildAbort(TRUE);
    }

    /* writing the offset hands the round to roll forward; not as an orphan */
    switchChildCheckParent();
    rewind(tmpfp);
    fprintf(tmpfp, "#%ld", pos);

    if (fflush(tmpfp) != 0 || fsync(fileno(tmpfp)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fsync", tmpElogFname);
        switchChildAbort(TRUE);
    }

    if (FCLOSEUP(&tmpfp) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_S_FAIL_M, fname, "fclose", tmpElogFname);
        switchChildAbort(TRUE);
    }

    /* Step 6: rotate .N -> .N+1 ... .0 -> .1 (checks the parent per rename) */
    if ((totalEventFile = renameElogFiles()) <= 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_FAIL, fname, "renameElogFiles");
        switchChildAbort(TRUE);
    }

    ls_syslog(LOG_DEBUG, "\
%s: switch child done, %d archive files; the index is rebuilt after the \
publish", fname, totalEventFile);

    _exit(SWITCH_CHILD_EXIT_OK);
}

/*
 * Switch child only: exit as soon as the mbatchd that forked it is gone, so
 * that an orphan never rotates archives while the next mbatchd recovers.
 * Called before every step that changes files and before each archive
 * rename.  The getppid() check is the guarantee; PR_SET_PDEATHSIG, which
 * the kernel clears on every chuser(), is re-armed to stop an orphan
 * sooner.  The fork must stay on the main thread.  A no-op in mbatchd.
 */
static void
switchChildCheckParent(void)
{
    if (switchParentPid == 0)
        return;

    if (getppid() != switchParentPid)
        _exit(SWITCH_CHILD_EXIT_FAIL);

    prctl(PR_SET_PDEATHSIG, SIGKILL);
    /* the parent may have died between the check and prctl() */
    if (getppid() != switchParentPid)
        _exit(SWITCH_CHILD_EXIT_FAIL);
}

/*
 * Start one round of the fork based switch.
 * @return: 0 when a child was started, -1 otherwise.
 */
static int
forkSwitchLog(void)
{
    static char fname[] = "forkSwitchLog";
    sigset_t    newmask, oldmask;
    pid_t       pid;
    pid_t       parentPid;

    /* Step 1: only one round at a time */
    if (switchState != SWITCH_IDLE) {
        ls_syslog(LOG_DEBUG, "\
%s: previous switch_log is still in state %d", fname, switchState);
        return -1;
    }

    /*
     * Step 2: create .tmp and .delta; a leftover .tmp parks the round
     * instead, reported by the recovery
     */
    if (switchLogBeginRound() < 0) {
        if (switchState != SWITCH_STALLED)
            ls_syslog(LOG_ERR, "\
%s: Fail to start the lsb.events switch: %m; retried when the next switch \
is due", fname);
        return -1;
    }

    /* Step 3: block SIGCHLD until the child pid is registered */
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    switchChildPid = 0;
    switchChildDone = 0;

    /* Step 4: new events go to the delta, lsb.events is frozen for the child */
    elogWriteTarget = deltaElogFname;

    /* Step 5: fork; the child does the expensive part and never returns */
    parentPid = getpid();
    pid = fork();

    if (pid == 0) {
        switchParentPid = parentPid;

        sigprocmask(SIG_SETMASK, &oldmask, NULL);

        /*
         * not mbatchd: its handlers would exit() and flush inherited stdio into
         * the event files; plain death is right, recovery handles the rest
         */
        Signal_(SIGTERM, SIG_DFL);
        Signal_(SIGINT,  SIG_DFL);
        Signal_(SIGCHLD, SIG_DFL);

        switchChildCheckParent();
        switchLogChildMain();
        _exit(SWITCH_CHILD_EXIT_FAIL);
    }

    /* Step 6a: fork failed - undo steps 2 and 4 */
    if (pid < 0) {
        /* the cleanup below clobbers errno; %m must report the fork */
        int forkErrno = errno;

        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        switchLogDropRound();
        errno = forkErrno;
        ls_syslog(LOG_ERR, "\
%s: Fail to start the lsb.events switch: %m; retried when the next switch \
is due", fname);
        return -1;
    }

    /* Step 6b: register the pid before SIGCHLD is unblocked */
    switchChildPid = pid;
    switchChildKillSent = FALSE;
    switchState = SWITCH_CHILD_RUNNING;

    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    ls_syslog(LOG_INFO, "%s: Switching lsb.events in background process <%d>",
              fname, (int)pid);

    return 0;
}

/*
 * SIGCHLD handler: save the wait status of the switch child, set
 * switchChildDone last; the main loop decodes it.
 * @param[in] pid: Pid waitpid() just reaped.
 * @param[in] status: Raw wait status belonging to that pid.
 */
void
switchLogChildReaped(pid_t pid, int status)
{
    /* a reaped rebuild child is no longer running, whatever its pid means
     * to the system afterwards */
    if (switchIndexPid != 0 && pid == switchIndexPid) {
        switchIndexPid = 0;
        return;
    }

    if (eventIndexRebuildPid != 0 && pid == eventIndexRebuildPid) {
        eventIndexRebuildPid = 0;
        return;
    }

    if (switchChildPid == 0 || pid != switchChildPid)
        return;

    switchChildStatus = status;
    switchChildDone = 1;
}

/* TRUE while a switch child is running; the main loop uses it to cap the
 * epoll wait so that the completion flag is consumed quickly.
 */
int
switchLogChildRunning(void)
{
    return (switchState == SWITCH_CHILD_RUNNING);
}

/*
 * Kill and reap the switch child when mbatchd exits, before die() releases
 * the event log lock that the restarted mbatchd takes at once.  A no-op
 * without a child, and inside the child itself.
 */
void
switchLogKillChild(void)
{
    pid_t pid = switchChildPid;

    if (pid <= 0)
        return;

    ls_syslog(LOG_INFO, "\
%s: Stopping lsb.events switch process <%d>",
          __func__, (int)pid);

    /*
     * no time limit: a child in a hung NFS call could still rename an
     * archive when it returns, and the next mbatchd would need NFS anyway
     */
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

/*
 * Main loop: consume the status the SIGCHLD handler saved and hand the
 * round to switchLogRecover().  Even an exit 0 is checked against the
 * files: a raw baseline published as complete would be archived whole by
 * the next switch.
 */
void
switchLogCheckChild(void)
{
    static char fname[] = "switchLogCheckChild";
    sigset_t    newmask, oldmask;
    int         status;
    int         expect;
    pid_t       pid;

    if (!switchChildDone)
        return;

    /* Step 1: take the result the SIGCHLD handler saved */
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    status = switchChildStatus;
    pid = switchChildPid;
    switchChildDone = 0;
    switchChildPid = 0;

    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    /* Step 2: the exit status only sets the expectation */
    if (WIFEXITED(status) && WEXITSTATUS(status) == SWITCH_CHILD_EXIT_OK) {
        expect = RECOVER_EXPECT_COMPLETE;
    } else {
        expect = RECOVER_EXPECT_INTERRUPTED;
        /* a failing child has logged its reason itself */
        if (WIFSIGNALED(status))
            ls_syslog(LOG_ERR, "\
%s: lsb.events switch process <%d> was killed by signal %d", fname,
                      (int)pid, WTERMSIG(status));
        else
            ls_syslog(LOG_DEBUG, "\
%s: lsb.events switch process <%d> exited with code %d", fname,
                      (int)pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

    /* Step 3: the files are ours: publish, roll back or drop the round */
    switchState = SWITCH_STALLED;
    if (switchLogRecover(expect) == 1)
        ls_syslog(LOG_INFO, "%s: lsb.events switched", fname);
}

static int
createAcct0File(void)
{
    static char fname[] = "createAcct0File";
    char acct0File[MAXFILENAMELEN];
    struct stat st;
    char buf[MSGSIZE];
    FILE *acctPtr, *acct0Ptr;
    int nread, cc, size;

    sprintf(acct0File, "%s/logdir/lsb.acct.0",
            daemonParams[LSB_SHAREDIR].paramValue);

    if (stat(jlogFname, &st) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat", jlogFname);
        return (-1);
    }
    size = st.st_size;

    if ((acctPtr = fopen(jlogFname, "r")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", jlogFname);
        FCLOSEUP(&acctPtr);
        return (-1);
    }

    if ((acct0Ptr  = fopen(acct0File, "w")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", acct0File);
        FCLOSEUP(&acctPtr);
        return (-1);
    }
    chmod(acct0File, 0644);

    for (cc = 0, nread = 0; nread < size;) {

        if ((cc = fread(buf, 1, MSGSIZE, acctPtr)) > 0) {
            if (nread + cc > size)
                cc = size - nread;

            /*
             * A short write is a failure: fwrite() only returning 0 let
             * a full disk or an NFS error truncate the archive copy
             * silently.  cc was already clipped to the captured size
             * above, so anything less than cc means lost bytes.
             */
            if (fwrite(buf, 1, cc, acct0Ptr) != (size_t)cc) {
                chuser(batchId);
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fwrite");
                goto acct0_failed;
            }
            nread += cc;
        } else if (feof(acctPtr)){
            break;
        } else {
            chuser(batchId);
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fread", jlogFname);
            goto acct0_failed;
        }
    }
    FCLOSEUP(&acctPtr);

    /*
     * lsb.acct is removed and .0 rotated to .1 right after this: the copy
     * must be on disk and complete, or a power loss would leave a cut off
     * archive and no lsb.acct to recover it from.
     */
    if (fflush(acct0Ptr) != 0 || fsync(fileno(acct0Ptr)) < 0
        || FCLOSEUP(&acct0Ptr) != 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fsync", acct0File);
        goto acct0_failed;
    }
    return (0);

acct0_failed:
    /* the partial copy is never used, and on a full disk it holds the space
     * lsb.acct itself needs; lsb.acct is left untouched */
    if (acctPtr != NULL)
        FCLOSEUP(&acctPtr);
    if (acct0Ptr != NULL)
        FCLOSEUP(&acct0Ptr);
    chuser(managerId);
    unlink(acct0File);
    chuser(batchId);
    return (-1);
}


static int
createEvent0File (void)
{
    static char fname[] = "createEvent0File";
    char event0File[MAXFILENAMELEN];
    char buf[MSGSIZE];
    char ch;
    struct stat st;
    off_t nread, size;
    int cc;
    long int pos;
    FILE *eventPtr, *event0Ptr;

    sprintf(event0File, "%s/logdir/lsb.events.0",
            daemonParams[LSB_SHAREDIR].paramValue);

    if (stat(elogFname, &st) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat", elogFname);
        return (-1);
    }
    size = st.st_size;

    if ((eventPtr = fopen(elogFname, "r")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", elogFname);
        FCLOSEUP(&eventPtr);
        return (-1);
    }

    if ((event0Ptr  = fopen(event0File, "w")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", event0File);
        FCLOSEUP(&eventPtr);
        return (-1);
    }
    chmod(event0File, 0644);

    fprintf(event0Ptr, "#%ld                       \n", time(0));

    if (fscanf(eventPtr, "%c%ld ", &ch, &pos) != 2 || ch != '#') {
        /*
         * A file without a numeric header is archived whole, as before;
         * a header that could not be read is not "no header": copying
         * from offset 0 would archive the baseline a second time.
         */
        if (ferror(eventPtr)) {
            chuser(batchId);
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fscanf", elogFname);
            FCLOSEUP(&eventPtr);
            FCLOSEUP(&event0Ptr);
            unlink(event0File);
            return (-1);
        }
        pos = 0;
    }

    if (fseek(eventPtr, pos, SEEK_SET) != 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "fseek");
        return (-1);
    }

    size -= pos;

    for (cc = 0, nread = 0; nread < size;) {
        if ((cc = fread(buf, 1, MSGSIZE, eventPtr)) > 0) {
            if (nread + cc > size)
                cc = size - nread;

            /*
             * a short write is a failure (cc is already clipped); the partial copy
             * is removed, on a full disk it would hold space events need
             */
            if (fwrite(buf, 1, cc, event0Ptr) != (size_t)cc) {
                chuser(batchId);
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fwrite");
                FCLOSEUP(&eventPtr);
                FCLOSEUP(&event0Ptr);
                unlink(event0File);
                return (-1);
            }
            nread += cc;
        } else if (feof(eventPtr)){
            break;
        } else {
            chuser(batchId);
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fread", elogFname);
            FCLOSEUP(&eventPtr);
            FCLOSEUP(&event0Ptr);
            unlink(event0File);
            return (-1);
        }
    }
    FCLOSEUP(&eventPtr);

    /*
     * the segment is rotated right after: without fsync and a checked close a
     * power loss could leave a silently truncated archive
     */
    if (fflush(event0Ptr) != 0 || fsync(fileno(event0Ptr)) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fsync", event0File);
        FCLOSEUP(&event0Ptr);
        unlink(event0File);
        return (-1);
    }

    if (FCLOSEUP(&event0Ptr) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fclose", event0File);
        unlink(event0File);
        return (-1);
    }

    return (0);
}


static int
renameElogFiles(void)
{
    static char fname[] = "renameElogFiles";
    int i;
    int max;
    int total;
    char tmpfn[MAXFILENAMELEN], eventfn[MAXFILENAMELEN];
    struct stat st;

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG, "%s: Entering ... ", fname);

    chuser(managerId);
    i = 0;
    do {
        sprintf(tmpfn, "%s/logdir/lsb.events.%d",
                daemonParams[LSB_SHAREDIR].paramValue, ++i);
    } while (stat(tmpfn, &st) == 0);

    /* a failed stat is not a gap: shifting into it would overwrite an
     * existing archive */
    if (errno != ENOENT) {
        int saveErrno = errno;

        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat", tmpfn);
        errno = saveErrno;
        return (-1);
    }

    max = i;

    /*
     * the first free number may be a gap (interrupted rotation, archive
     * removed by hand); the archives above it belong to the total that the
     * index rebuild gets.  A stat error only ends the count early.
     */
    for (total = max; ; total++) {
        sprintf(tmpfn, "%s/logdir/lsb.events.%d",
                daemonParams[LSB_SHAREDIR].paramValue, total + 1);
        if (stat(tmpfn, &st) < 0)
            break;
    }

    while (i--) {

        /* switch child: never rename once mbatchd is gone */
        switchChildCheckParent();

        sprintf(tmpfn, "%s/logdir/lsb.events.%d",
                daemonParams[LSB_SHAREDIR].paramValue, i);
        sprintf(eventfn, "%s/logdir/lsb.events.%d",
                daemonParams[LSB_SHAREDIR].paramValue, i + 1);

        if (rename(tmpfn, eventfn) == -1 && errno != ENOENT) {
            int saveErrno = errno;

            chuser(batchId);
            ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname,
                      "rename", tmpfn, eventfn);
            errno = saveErrno;
            return (-1);
        }
    }
    chuser(batchId);

    return (total);
}

void
logJobInfo(struct submitReq * req, struct jData *jp, struct lenData * jf)
{
    static char fname[] = "logJobInfo";
    int                     errnoSv;
    char                    logFn[MAXFILENAMELEN];
    FILE                   *fp;
    mode_t                  omask = umask(077);
    sigset_t                newmask, oldmask;

    chuser(managerId);

    sprintf(logFn, "%s/logdir/info/%s",
            daemonParams[LSB_SHAREDIR].paramValue,
            jp->shared->jobBill.jobFile);

    sigemptyset(&newmask);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);
    if ((fp = fopen(logFn, "w")) == NULL) {
        errnoSv = errno;
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        umask(omask);
        chuser(batchId);
        errno = errnoSv;
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen",
                  logFn);
        mbdDie(MASTER_FATAL);
    }
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    umask(omask);

    if (fwrite(jf->data, 1, jf->len, fp) != jf->len) {
        chuser(batchId);
        ls_syslog(LOG_ERR, "%s: fwrite(%s, len=%d) failed: %m",
                  fname,
                  logFn,
                  jf->len);
        chuser(managerId);

        FCLOSEUP(&fp);
        goto error;
    }

    chuser(managerId);
    if (FCLOSEUP(&fp) < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fclose",
                  logFn);
        goto error;
    }


    chuser(batchId);
    return;

error:
    chuser(managerId);
    unlink(logFn);
    chuser(batchId);
    mbdDie(MASTER_FATAL);

}

int
rmLogJobInfo_(struct jData *jp, int check)
{
    static char        fname[] = "rmLogJobInfo_()";
    char               logFn[MAXFILENAMELEN];
    struct stat        st;
    struct submitReq   *req;

    req = &jp->shared->jobBill;

    if (req->options2 & (SUB2_JOB_CMD_SPOOL | SUB2_IN_FILE_SPOOL)) {
        struct passwd pwUser;

        memset(&pwUser, 0, sizeof(pwUser));
        pwUser.pw_name = jp->userName;
        pwUser.pw_uid = jp->userId;

        if (req->options2 & SUB2_JOB_CMD_SPOOL) {

            childRemoveSpoolFile(req->commandSpool,
                                 FORK_REMOVE_SPOOL_FILE, &pwUser);
        }

        if (req->options2 & SUB2_IN_FILE_SPOOL) {

            childRemoveSpoolFile(req->inFileSpool,
                                 FORK_REMOVE_SPOOL_FILE, &pwUser);
        }
    }

    chuser(managerId);
    sprintf(logFn, "%s/logdir/info/%s",
            daemonParams[LSB_SHAREDIR].paramValue, req->jobFile);

    if (stat(logFn, &st) != 0) {
        sprintf(logFn, "%s/logdir/info/%d",
                daemonParams[LSB_SHAREDIR].paramValue,
                LSB_ARRAY_JOBID(jp->jobId));
    }

    if (stat(logFn, &st) == 0) {
        if (unlink(logFn) == -1) {
            chuser(batchId);
            if (check == FALSE)
                ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M,
                          fname,
                          lsb_jobid2str(jp->jobId),
                          "unlink",
                          logFn);
            return (-1);
        }
    } else if (errno != ENOENT) {
        chuser(batchId);
        if (check == FALSE)
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M,
                      fname,
                      lsb_jobid2str(jp->jobId),
                      "stat",
                      logFn);
        return (-1);
    }
    chuser(batchId);

    return (0);
}

int
readLogJobInfo(struct jobSpecs *jobSpecs, struct jData *jpbw,
               struct lenData *jf, struct lenData *aux_auth_data)
{
#define ENVEND "$LSB_TRAPSIGS\n"
    static char fname[] = "readLogJobInfo()";
    char logFn[MAXFILENAMELEN];
    LS_STAT_T st;
    int fd, i, numEnv,cc;
    char *buf, *sp, *edata, *eventAttrs = NULL;
    char *newBuf;

    jobSpecs->numEnv = 0;
    jobSpecs->env = NULL;
    jobSpecs->eexec.len = 0;
    jobSpecs->eexec.data = NULL;
    jf->len = 0;
    jf->data = NULL;

    sprintf (logFn, "%s/logdir/info/%s",
             daemonParams[LSB_SHAREDIR].paramValue,
             jpbw->shared->jobBill.jobFile);
    chuser(managerId);

    fd = open(logFn, O_RDONLY);
    if (fd < 0) {

        sprintf(logFn, "%s/logdir/info/%d",
                daemonParams[LSB_SHAREDIR].paramValue,
                LSB_ARRAY_JOBID(jpbw->jobId));
        fd = open(logFn, O_RDONLY);
    }

    if (fd < 0) {
        chuser(batchId);
        if (errno != ENOENT) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M,
                      fname,
                      lsb_jobid2str(jpbw->jobId),
                      "open",
                      logFn);
        }
        return (-1);
    }

    fstat(fd, &st);
    buf = my_malloc(st.st_size, fname);
    if ((cc = read(fd, buf, st.st_size)) != st.st_size) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M,
                  fname,
                  lsb_jobid2str(jpbw->jobId),
                  "read",
                  logFn);
        close(fd);
        FREEUP(buf);
        return (-1);
    }
    close(fd);
    chuser(batchId);

    for (sp = buf + strlen(SHELLLINE), numEnv = 0;
         strncmp(sp, ENVEND, sizeof(ENVEND) - 1); numEnv++) {

        sp = strstr(sp, TAILCMD);

        if (sp == NULL) {
            ls_syslog(LOG_ERR, "\
%s: Info file is corrupted for job <%d>; could not find %s (numEnv=%d)",
                      fname, LSB_ARRAY_JOBID(jpbw->jobId),
                      TAILCMD, numEnv);
            FREEUP(buf);
            return (-1);
        }
        sp = strchr(sp, '\n');

        if (sp == NULL) {
            ls_syslog(LOG_ERR, "\
%s: Info file is corrupted for job <%d>; could not find newline character (numEnv=%d)",
                      fname, LSB_ARRAY_JOBID(jpbw->jobId), numEnv);
            FREEUP(buf);
            return (-1);
        }
        sp++;
    }

    if (jpbw->qPtr->jobStarter)
        numEnv++;

    if (numEnv) {

        jobSpecs->env = my_calloc(numEnv, sizeof(char *), fname);
        jobSpecs->numEnv = 0;

        for (sp = buf + strlen(SHELLLINE), i = 0;
             strncmp(sp, ENVEND, sizeof(ENVEND) - 1);) {

            char *spp, *tailst;
            char saveChar;

            tailst = strstr(sp, TAILCMD);
            saveChar = *tailst;
            *tailst = '\0';

            jobSpecs->env[i] = safeSave(sp);
            *tailst = saveChar;


            spp = strchr(jobSpecs->env[i], '=');
            if (spp == NULL) {
                ls_syslog(LOG_ERR,"\
%s: Info file seems corrupted, bad system syntax variable=%s in job=%d, trying to recover by skipping the variable",
                          fname,
                          jobSpecs->env[i],
                          LSB_ARRAY_JOBID(jpbw->jobId));
                FREEUP(jobSpecs->env[i]);
            } else {
                spp += 2;
                for (; *spp != '\0'; spp++)
                    *(spp - 1) = *spp;
                *(spp - 1) = '\0';
                i++;
            }

            sp = strchr(tailst, '\n') + 1;
            if (sp == NULL) {
                ls_syslog(LOG_ERR,"\
%s: Info file seems corrupted cannot locate new line, job=%d numEnv=%d",
                          fname, LSB_ARRAY_JOBID(jpbw->jobId),
                          numEnv);
            }
        }

        if (jpbw->qPtr->jobStarter) {
            jobSpecs->env[i] = my_malloc(strlen(jpbw->qPtr->jobStarter) + 1 +
                                         sizeof("LSB_JOB_STARTER="),
                                         "readLogJobInfo/job_starter");
            sprintf(jobSpecs->env[i], "LSB_JOB_STARTER=%s",
                    jpbw->qPtr->jobStarter);
            i++;
        }

        if (eventAttrs) {
            jobSpecs->env[i] = safeSave(eventAttrs);
            ls_syslog(LOG_DEBUG, "%s", eventAttrs);
            FREEUP (eventAttrs);
            i++;
        }
        jobSpecs->numEnv = i;
    } else {
        jobSpecs->env = NULL;
        jobSpecs->numEnv = 0;
    }
    edata = strstr(buf, EDATASTART);

    if (edata) {
        sp = edata + sizeof(EDATASTART) - 1;
        jobSpecs->eexec.len = atoi(sp);
        sp += strlen(sp) + 1;
        if (jobSpecs->eexec.len > 0) {
            jobSpecs->eexec.data = my_malloc(jobSpecs->eexec.len,
                                             "readLogJobinfo/eexec.data");
            memcpy(jobSpecs->eexec.data, sp, jobSpecs->eexec.len);
        } else {
            jobSpecs->eexec.len = 0;
            jobSpecs->eexec.data = NULL;
        }
    } else {
        ls_syslog(LOG_DEBUG, "%s: File <%s> of job <%d> has no <%s>; assume it is a pre-2.2 file", fname, logFn, LSB_ARRAY_JOBID(jpbw->jobId), EDATASTART);
        jobSpecs->eexec.len = 0;
        jobSpecs->eexec.data = NULL;
    }

    i = 0;

    sp = strstr(buf, EXITCMD);

    if (!sp) {

        ls_syslog(LOG_ERR, "\
%s: failed to find EXITCMD tag in jobfile, for job <%d>",
                  fname, LSB_ARRAY_JOBID(jpbw->jobId));
        return(-1);
    }

    sp += strlen(EXITCMD);
    *sp = '\0';

    if ((jpbw->qPtr->jobStarter != NULL) &&
        (jpbw->qPtr->jobStarter[0] != '\0')) {

        if (checkJobStarter(buf, jpbw->qPtr->jobStarter)) {

            jf->data = buf;
            jf->len = strlen(jf->data) + 1;
            return (0);
        } else if ((newBuf = instrJobStarter1(buf, strlen(buf) + 1, CMDSTART,
                                              WAITCLEANCMD, jpbw->qPtr->jobStarter)) == NULL) {
            ls_syslog(LOG_ERR, "\
%s: Failed to insert job stater into jobfile data, job <%s>, queue <%s>",
                      fname, lsb_jobid2str(jpbw->jobId), jpbw->qPtr->queue);
            FREEUP(buf);
            return(-1);
        } else {
            FREEUP(buf);
            jf->data = newBuf;
            jf->len = strlen(jf->data) + 1;
            return (0);
        }
    }

    jf->data = buf;
    jf->len = strlen(jf->data) +1;

    return (0);

}

char *
readJobInfoFile (struct jData *jp, int *len)
{
    static char fname[] = "readJobInfoFile";
    char logFn[MAXFILENAMELEN];
    LS_STAT_T st;
    int fd;
    char *buf;

    sprintf(logFn, "%s/logdir/info/%s",
            daemonParams[LSB_SHAREDIR].paramValue,
            jp->shared->jobBill.jobFile);
    chuser(managerId);

    fd = open(logFn, O_RDONLY);
    if (fd < 0) {

        sprintf(logFn, "%s/logdir/info/%d",
                daemonParams[LSB_SHAREDIR].paramValue,
                LSB_ARRAY_JOBID(jp->jobId));
        fd = open(logFn, O_RDONLY);
    }

    if (fd < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M,
                  fname,
                  "open",
                  logFn);
        return (NULL);
    }
    fstat(fd, &st);

    buf = (char *) my_malloc(st.st_size, fname);
    *len = st.st_size;
    if (read(fd, buf, st.st_size) != st.st_size) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M,
                  fname,
                  lsb_jobid2str(jp->jobId),
                  "read",
                  logFn);
        close(fd);
        free(buf);
        return (NULL);
    }
    close(fd);
    chuser(batchId);
    return (buf);
}

void
writeJobInfoFile(struct jData *jp, char *jf, int len)
{

    static char             fname[] = "writeJobInfoFile";
    char                    logFn[MAXFILENAMELEN];
    int                     fd, errnoSv;

    chuser(managerId);

    sprintf(logFn, "%s/logdir/info/%s",
            daemonParams[LSB_SHAREDIR].paramValue,
            jp->shared->jobBill.jobFile);

    fd = open(logFn, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        sprintf(logFn, "%s/logdir/info/%d",
                daemonParams[LSB_SHAREDIR].paramValue,
                LSB_ARRAY_JOBID(jp->jobId));
        fd = open(logFn, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    }
    if (fd < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "open", logFn);
        mbdDie(MASTER_FATAL);
    }

    if (b_write_fix(fd, jf, len) != len) {
        errnoSv = errno;
        unlink(logFn);
        close(fd);
        chuser(batchId);
        errno = errnoSv;
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "fprintf",
                  logFn, jf);
        mbdDie(MASTER_FATAL);
    }

    close(fd);
}


int
replaceJobInfoFile(char *jobFileName,
                   char *newCommand,
                   char *jobStarter,
                   int options)
{
    static char fname[] = "replaceJobInfoFile";
    char jobFile[MAXFILENAMELEN];
    char workFile[MAXFILENAMELEN];
    char line[MAXLINELEN];
    char *ptr;
    int  nbyte;
    FILE *fdi, *fdo;

    chuser(managerId);

    sprintf(jobFile, "\
%s/logdir/info/%s", daemonParams[LSB_SHAREDIR].paramValue, jobFileName);
    sprintf(workFile, "\
%s/logdir/info/%s.tmp", daemonParams[LSB_SHAREDIR].paramValue,
            jobFileName);

    if ((fdi = fopen(jobFile, "r")) == NULL) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", jobFileName);
        return (-1);
    }

    if ((fdo = fopen(workFile, "w")) == NULL) {
        FCLOSEUP(&fdi);
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", workFile);
        return (-1);
    }

    while ((ptr = fgets(line, MAXLINELEN, fdi)) != NULL) {
        if (strcmp(line, CMDSTART) != 0) {
            fputs(line, fdo);
        } else {
            char outCmdArgs[2*MAXLINELEN];
            char *pFileCmdArgs;

            fputs(line, fdo);

            if (options & REPLACE_1ST_CMD_ONLY) {
                char *oldCmdArgs;


                if ((ptr = fgets(line, MAXLINELEN, fdi)) == NULL) {
                    FCLOSEUP(&fdo);
                    FCLOSEUP(&fdi);
                    remove(workFile);
                    chuser(batchId);
                    ls_syslog(LOG_ERR, "%s: Unexpected the end of (%s)",
                              fname,
                              jobFileName);
                    return -1;
                }

                if (jobStarter) {
                    oldCmdArgs = strstr(line, jobStarter);
                    if (oldCmdArgs) {
                        oldCmdArgs += strlen(jobStarter);
                    } else {
                        oldCmdArgs = line;
                    }
                } else {
                    oldCmdArgs = line;
                }

                if (replace1stCmd_(oldCmdArgs, newCommand,
                                   outCmdArgs, sizeof(outCmdArgs)) < 0) {
                    FCLOSEUP(&fdo);
                    FCLOSEUP(&fdi);
                    remove(workFile);
                    chuser(batchId);
                    ls_syslog(LOG_ERR, "\
%s: The command line is too long when replacing the command of (%s) by the one of (%s)",
                              fname, oldCmdArgs,
                              newCommand);
                    return (-1);

                }

                pFileCmdArgs = outCmdArgs;
            } else {
                pFileCmdArgs = newCommand;
            }

            if (jobStarter) {
                fprintf(fdo, "%s %s", jobStarter, pFileCmdArgs);
            } else {
                fprintf(fdo, "%s", pFileCmdArgs);
            }
            break;
        }
    }
    if (ptr == NULL) {
        FCLOSEUP(&fdo);
        FCLOSEUP(&fdi);
        remove(workFile);
        chuser(batchId);
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6826,
                                         "%s: Unexpected the end of (%s)"), /* catgets 6826 */
                  fname,
                  jobFileName);
        return (-1);
    }


    while ((ptr=fgets(line, MAXLINELEN, fdi)) != NULL) {
        if (strcmp(line, CMDEND) == 0) {
            ptr=fgets(line, MAXLINELEN, fdi);
            break;
        } else {
            if (options & REPLACE_1ST_CMD_ONLY) {
                fputs(line, fdo);
            }
        }
    }

    if (ptr == NULL) {
        FCLOSEUP(&fdo);
        FCLOSEUP(&fdi);
        remove(workFile);
        chuser(batchId);
        ls_syslog(LOG_ERR, "%s: Unexpected the end of (%s)",
                  fname,
                  jobFileName);
        return (-1);
    }

    fputs(WAITCLEANCMD, fdo);

    while ((nbyte=fread(line,1, MAXLINELEN, fdi)) > 0)
        if (fwrite(line,1, nbyte, fdo) != nbyte) {
            FCLOSEUP(&fdo);
            FCLOSEUP(&fdi);
            remove(workFile);
            chuser(batchId);
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fwrite", workFile);
            return (-1);
        }

    FCLOSEUP(&fdo);
    FCLOSEUP(&fdi);
    remove(jobFile);
    rename(workFile, jobFile);
    chuser(batchId);
    return (0);
}
void
log_mbdStart(void)
{
    char hname[MAXHOSTNAMELEN];
    struct qData *qp;
    int num_queues;
    int num_hosts;

    if (openEventFile("log_mbdStart") < 0)
        mbdDie(MASTER_FATAL);

    logPtr->type = EVENT_MBD_START;
    if (masterHost)
        strcpy(logPtr->eventLog.mbdStartLog.master, masterHost);
    else if (gethostname(hname, MAXHOSTNAMELEN) >= 0)
        strcpy(logPtr->eventLog.mbdDieLog.master, hname);
    else
        strcpy(logPtr->eventLog.mbdStartLog.master, "unknown");

    if (clusterName)
        strcpy(logPtr->eventLog.mbdStartLog.cluster, clusterName);
    else
        strcpy(logPtr->eventLog.mbdStartLog.cluster, "unknown");


    num_queues = numofqueues;
    num_hosts = numofhosts();
    for (qp = qDataList->forw; qp != qDataList; qp = qp->forw) {
        if (strcmp(qp->queue, "lost_and_found") == 0) {
            num_queues--;
            break;
        }
    }
    if (h_getEnt_(&hostTab, "lost_and_found") != NULL) {
        num_hosts--;
    }
    logPtr->eventLog.mbdStartLog.numQueues = num_queues;
    logPtr->eventLog.mbdStartLog.numHosts = num_hosts;

    putEventRec("log_mbdStart");
}


static int
replay_modifyjob(char *filename, int lineNum)
{
    static char             fname[] = "replay_modifyjob";
    struct jData           *job, *jpbw;

    job = replay_jobdata (filename, lineNum, fname);
    if ((jpbw = getJobData (job->jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%s> not found in job list"),
                  fname, filename, lineNum, lsb_jobid2str(job->jobId));
        return (FALSE);
    }

    handleJParameters(jpbw, job, &job->shared->jobBill, TRUE, 0, 0);
    if (jpbw->jgrpNode && ((job->shared->jobBill.options & SUB_JOB_NAME) ||
                           (job->shared->jobBill.options2 & SUB2_MODIFY_CMD))) {
        treeClip(jpbw->jgrpNode);
        treeFree(jpbw->jgrpNode);
        putOntoTree(jpbw, JOB_NEW);
    }
    else {
        jpbw->runCount = 1;
    }
    if (jpbw->nodeType == JGRP_NODE_ARRAY) {
        struct jData *jPtr;
        for (jPtr = jpbw->nextJob; jPtr != NULL; jPtr = jPtr->nextJob)
            updLocalJData(jPtr, jpbw);
    }
    freeJData (job);
    return (TRUE);
}

static int
replay_modifyjob2(char *filename, int lineNum)
{
    struct jobModLog       *jobModLog;
    struct modifyReq        modifyReq;
    struct lsfAuth          auth;
    int                     j;

    (void)memset((void *)&modifyReq, '\0', sizeof (modifyReq));
    (void)memset((void *)&auth, '\0', sizeof (auth));

    jobModLog = &logPtr->eventLog.jobModLog;

    auth.options = 0;
    auth.uid = jobModLog->userId;
    strcpy(auth.lsfUserName, jobModLog->userName);

    modifyReq.jobIdStr = jobModLog->jobIdStr;
    modifyReq.jobId = atoi(jobModLog->jobIdStr);
    modifyReq.delOptions = jobModLog->delOptions;
    modifyReq.delOptions2 = jobModLog->delOptions2;
    modifyReq.submitReq.options = jobModLog->options;
    modifyReq.submitReq.options2 = jobModLog->options2;
    modifyReq.submitReq.submitTime = jobModLog->submitTime;
    modifyReq.submitReq.umask = jobModLog->umask;
    modifyReq.submitReq.numProcessors = jobModLog->numProcessors;
    modifyReq.submitReq.beginTime = jobModLog->beginTime;
    modifyReq.submitReq.termTime = jobModLog->termTime;
    modifyReq.submitReq.sigValue = jobModLog->sigValue;
    modifyReq.submitReq.restartPid = jobModLog->restartPid;
    modifyReq.submitReq.jobName = jobModLog->jobName;
    modifyReq.submitReq.queue = jobModLog->queue;
    modifyReq.submitReq.numAskedHosts = jobModLog->numAskedHosts;
    if (jobModLog->numAskedHosts > 0)
        modifyReq.submitReq.askedHosts = jobModLog->askedHosts;
    modifyReq.submitReq.resReq = jobModLog->resReq;
    for (j = 0; j < LSF_RLIM_NLIMITS; j++)
        modifyReq.submitReq.rLimits[j] = jobModLog->rLimits[j];



    modifyReq.submitReq.hostSpec = jobModLog->hostSpec;
    modifyReq.submitReq.dependCond = jobModLog->dependCond;
    modifyReq.submitReq.subHomeDir = jobModLog->subHomeDir;
    modifyReq.submitReq.inFile = jobModLog->inFile;
    modifyReq.submitReq.outFile = jobModLog->outFile;
    modifyReq.submitReq.errFile = jobModLog->errFile;
    modifyReq.submitReq.command = jobModLog->command;
    modifyReq.submitReq.inFileSpool = jobModLog->inFileSpool;
    modifyReq.submitReq.commandSpool = jobModLog->commandSpool;
    modifyReq.submitReq.chkpntPeriod = jobModLog->chkpntPeriod;
    modifyReq.submitReq.chkpntDir = jobModLog->chkpntDir;
    modifyReq.submitReq.nxf = jobModLog->nxf;
    modifyReq.submitReq.xf = jobModLog->xf ;
    modifyReq.submitReq.jobFile = jobModLog->jobFile ;
    modifyReq.submitReq.fromHost = jobModLog->fromHost ;
    modifyReq.submitReq.cwd = jobModLog->cwd ;
    modifyReq.submitReq.preExecCmd = jobModLog->preExecCmd ;
    modifyReq.submitReq.postExecCmd = jobModLog->postExecCmd ;
    modifyReq.submitReq.mailUser = jobModLog->mailUser ;
    modifyReq.submitReq.projectName = jobModLog->projectName ;
    modifyReq.submitReq.niosPort = jobModLog->niosPort ;
    modifyReq.submitReq.maxNumProcessors = jobModLog->maxNumProcessors ;
    modifyReq.submitReq.loginShell = jobModLog->loginShell ;
    modifyReq.submitReq.schedHostType = jobModLog->schedHostType ;
    modifyReq.submitReq.userPriority = jobModLog->userPriority;
    modifyReq.submitReq.jobDesc = jobModLog->jobDesc;

    modifyJob(&modifyReq, NULL, &auth);
    return (TRUE);

}


static struct jData    *
replay_jobdata(char *filename, int lineNum, char *fname)
{
    struct jData           *job;
    struct submitReq       *jobBill;
    struct jobNewLog       *jobNewLog;
    struct qData           *qp;
    int                     i, errcode;
    struct lsfAuth          auth;

    memset(&auth, 0, sizeof (auth));

    jobNewLog = &logPtr->eventLog.jobNewLog;

    qp = getQueueData(jobNewLog->queue);
    if (qp == NULL) {
        ls_syslog(LOG_DEBUG, "\
%s: File %s at line %d: queue %s not found, saving job %d in queue %s",
                  __func__, filename, lineNum,
                  jobNewLog->queue, jobNewLog->jobId, LOST_AND_FOUND);
        qp = getQueueData(LOST_AND_FOUND);
        if (qp == NULL)
            qp = lostFoundQueue();
    }

    job = initJData(my_calloc(1, sizeof(struct jShared), __func__));

    if (job->jobSpoolDir) {
        FREEUP(job->jobSpoolDir);
    }

    job->qPtr = qp;
    job->userId = jobNewLog->userId;

    jobBill = &(job->shared->jobBill);
    jobBill->options = jobNewLog->options;
    jobBill->options2 = jobNewLog->options2;
    if (jobBill->options2 & SUB2_USE_DEF_PROCLIMIT) {

        jobBill->numProcessors =
            jobBill->maxNumProcessors =
            (qp->defProcLimit > 0 ? qp->defProcLimit : 1);
    }
    else {
        jobBill->numProcessors = jobNewLog->numProcessors;
        jobBill->maxNumProcessors = jobNewLog->maxNumProcessors;
    }
    jobBill->submitTime = jobNewLog->submitTime;
    jobBill->beginTime = jobNewLog->beginTime;
    jobBill->termTime = jobNewLog->termTime;
    jobBill->sigValue = jobNewLog->sigValue;
    jobBill->chkpntPeriod = jobNewLog->chkpntPeriod;
    job->chkpntPeriod = jobNewLog->chkpntPeriod;
    jobBill->restartPid = jobNewLog->restartPid;
    job->restartPid = jobNewLog->restartPid;

    for (i = 0; i < LSF_RLIM_NLIMITS; i++)
        jobBill->rLimits[i] = jobNewLog->rLimits[i];

    jobBill->umask = jobNewLog->umask;
    job->cpuTime = 0.0;
    job->jobId = jobNewLog->jobId;
    job->dispTime = now + retryIntvl * msleeptime;
    job->newReason = PEND_SYS_NOT_READY;
    job->oldReason = PEND_SYS_NOT_READY;
    if (debug)
        job->dispTime = 0;

    job->userName = safeSave(jobNewLog->userName);
    job->schedHost = safeSave (jobNewLog->schedHostType);
    jobBill->schedHostType = safeSave (jobNewLog->schedHostType);
    jobBill->queue = safeSave(qp->queue);
    jobBill->fromHost = safeSave(jobNewLog->fromHost);
    jobBill->jobFile = safeSave(jobNewLog->jobFile);
    jobBill->hostSpec = safeSave(jobNewLog->hostSpec);
    jobBill->inFile = safeSave(jobNewLog->inFile);
    jobBill->outFile = safeSave(jobNewLog->outFile);
    jobBill->errFile = safeSave(jobNewLog->errFile);
    jobBill->command = safeSave(jobNewLog->command);
    job->jobSpoolDir = safeSave (jobNewLog->jobSpoolDir);

    jobBill->inFileSpool = safeSave(jobNewLog->inFileSpool);
    jobBill->commandSpool = safeSave(jobNewLog->commandSpool);
    jobBill->preExecCmd = safeSave(jobNewLog->preExecCmd);
    jobBill->postExecCmd = safeSave(jobNewLog->postExecCmd);
    jobBill->mailUser = safeSave(jobNewLog->mailUser);
    jobBill->dependCond = safeSave(jobNewLog->dependCond);
    jobBill->cwd = safeSave(jobNewLog->cwd);
    jobBill->subHomeDir = safeSave(jobNewLog->subHomeDir);
    jobBill->submitCwd = safeSave(jobNewLog->submitCwd);
    jobBill->chkpntDir = safeSave(jobNewLog->chkpntDir);
    jobBill->resReq = safeSave(jobNewLog->resReq);
    jobBill->loginShell = safeSave(jobNewLog->loginShell);


    jobBill->numAskedHosts = jobNewLog->numAskedHosts;
    if (jobBill->numAskedHosts > 0) {
        int  numAskedHosts = 0;
        struct askedHost *askedHosts;
        int returnErr, badReqIndx, others;

        jobBill->askedHosts = my_calloc(jobBill->numAskedHosts,
                                        sizeof(char *), fname);
        for (i = 0; i < jobBill->numAskedHosts; i++)
            jobBill->askedHosts[i] = safeSave(jobNewLog->askedHosts[i]);

        returnErr = chkAskedHosts(jobBill->numAskedHosts,
                                  jobBill->askedHosts,
                                  jobBill->numProcessors, &numAskedHosts,
                                  &askedHosts, &badReqIndx, &others, 0);
        if (returnErr == LSBE_NO_ERROR) {
            if (numAskedHosts > 0) {
                job->askedPtr = my_calloc (numAskedHosts,
                                           sizeof(struct askedHost), fname);
                for (i = 0; i < numAskedHosts; i++) {
                    job->askedPtr[i].hData = askedHosts[i].hData;
                    job->askedPtr[i].priority = askedHosts[i].priority;
                }
            }
            job->numAskedPtr = numAskedHosts;
            job->askedOthPrio = others;
            FREEUP(askedHosts);
        }
    }

    if (strcmp(jobNewLog->projectName, "") == 0)
        jobBill->projectName = safeSave(getDefaultProject());
    else
        jobBill->projectName = safeSave(jobNewLog->projectName);

    if (jobBill->options & SUB_JOB_NAME)
        jobBill->jobName = safeSave(jobNewLog->jobName);
    else
        jobBill->jobName = safeSave("");

    auth.uid = jobNewLog->userId;
    strcpy(auth.lsfUserName, jobNewLog->userName);

    if (strcmp(jobBill->dependCond, "") == 0) {
        job->shared->dptRoot = NULL;
    } else {
        setIdxListContext(jobBill->jobName);

        job->shared->dptRoot
            = parseDepCond(jobBill->dependCond, &auth, &errcode,
                           NULL, &job->jFlags, 0);
        freeIdxListContext();
        if (!job->shared->dptRoot)  {
            job->jFlags |= JFLAG_DEPCOND_INVALID;
        } else
            job->jFlags &= ~JFLAG_DEPCOND_INVALID;
    }

    if (jobBill->resReq  && jobBill->resReq[0] != '\0') {
        int useLocal = TRUE;

        if (job->numAskedPtr > 0 || job->askedOthPrio >= 0)
            useLocal = FALSE;

        useLocal = useLocal ? USE_LOCAL : 0;
        if ((job->shared->resValPtr =
             checkResReq (jobBill->resReq, useLocal | PARSE_XOR | CHK_TCL_SYNTAX)) == NULL)

            ls_syslog(LOG_ERR, "\
%s: File %s at line %d: Bad resource requirement %s job %s",
                      __func__, filename, lineNum, jobBill->resReq,
                      lsb_jobid2str(job->jobId));
    }
    jobBill->nxf = jobNewLog->nxf;
    if (jobBill->nxf > 0) {
        jobBill->xf = my_calloc(jobBill->nxf,
                                sizeof(struct xFile), fname);
        memcpy(jobBill->xf, jobNewLog->xf, jobBill->nxf * sizeof(struct xFile));
    } else {
       jobBill->xf = NULL;
    }

    jobBill->niosPort = jobNewLog->niosPort;

    jobBill->userPriority = jobNewLog->userPriority;
    job->jobPriority = jobBill->userPriority;

    if (jobBill->options2 & SUB2_JOB_DESC)
        jobBill->jobDesc = safeSave(jobNewLog->jobDesc);
    else
        jobBill->jobDesc = safeSave("");

    return job;
}

void
log_logSwitch(int lastJobId)
{
    static char             fname[] = "log_logSwitch";

    if (openEventFile("log_logSwitch") < 0) {
        LS_LONG_INT tmpJobId;
        tmpJobId = lastJobId;
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(tmpJobId), "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_LOG_SWITCH;
    logPtr->eventLog.logSwitchLog.lastJobId = lastJobId;
    if (putEventRec("log_logSwitch") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lastJobId, "putEventRec");
        return;
    }
}


void
log_signaljob(struct jData * jp, struct signalReq * signalReq, int userId,
              char *userName)
{
    static char             fname[] = "log_signaljob";
    int  sigValue;
    int defSigValue;

    if ((signalReq->sigValue != SIG_DELETE_JOB)
        && (signalReq->sigValue != SIG_KILL_REQUEUE)
        && (signalReq->sigValue != SIG_ARRAY_REQUEUE)) {


        defSigValue = getDefSigValue_(signalReq->sigValue,
                                      jp->qPtr->terminateActCmd);
        if (defSigValue == signalReq->sigValue) {
            if (IS_PEND(jp->jStatus))
                defSigValue = SIGKILL;
            else
                return;
        }
    }

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jp->jobId), "openEventFile");
        mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_SIGNAL;
    logPtr->eventLog.signalLog.jobId = LSB_ARRAY_JOBID(jp->jobId);
    logPtr->eventLog.signalLog.idx = LSB_ARRAY_IDX(jp->jobId);
    logPtr->eventLog.signalLog.userId = userId;
    strcpy(logPtr->eventLog.signalLog.userName, userName);
    if (signalReq->sigValue == SIG_DELETE_JOB) {
        logPtr->eventLog.signalLog.signalSymbol = "DELETEJOB";
        logPtr->eventLog.signalLog.runCount = signalReq->chkPeriod;
    } else if (signalReq->sigValue == SIG_KILL_REQUEUE) {
        logPtr->eventLog.signalLog.signalSymbol = "KILLREQUEUE";
        logPtr->eventLog.signalLog.runCount = signalReq->chkPeriod;
    } else if (signalReq->sigValue == SIG_ARRAY_REQUEUE) {
        logPtr->eventLog.signalLog.signalSymbol
            = getSignalSymbol(signalReq);

        logPtr->eventLog.signalLog.runCount = signalReq->actFlags;
    } else {
        sigValue = sig_encode(defSigValue);
        logPtr->eventLog.signalLog.signalSymbol
            = getSigSymbol(sigValue);
        logPtr->eventLog.signalLog.runCount = jp->runCount;
    }

    if (putEventRec("log_signaljob") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jp->jobId), "putEventRec");
        mbdDie(MASTER_FATAL);
    }
}

static char *
getSignalSymbol(const struct signalReq *sigPtr)
{
    static char      nameBuf[32];


    if (sigPtr->chkPeriod == JOB_STAT_PSUSP) {

        sprintf(nameBuf, "REQUEUE_PSUSP");

    } else {
        sprintf(nameBuf, "REQUEUE_PEND");
    }

    return(nameBuf);

}

void
log_jobmsg(struct jData * jp, struct lsbMsg * jmsg, int userId)
{
    static char             fname[] = "log_jobmsg";

    if (openEventFile("log_jobmsg") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jp->jobId), "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    logPtr->type = EVENT_JOB_MSG;
    logPtr->eventLog.jobMsgLog.usrId = userId;
    logPtr->eventLog.jobMsgLog.jobId = LSB_ARRAY_JOBID(jp->jobId);
    logPtr->eventLog.jobMsgLog.idx = LSB_ARRAY_IDX(jp->jobId);
    logPtr->eventLog.jobMsgLog.msgId = jmsg->header->msgId;
    logPtr->eventLog.jobMsgLog.type = jmsg->header->type;
    logPtr->eventLog.jobMsgLog.src = safeSave(jmsg->header->src);
    logPtr->eventLog.jobMsgLog.dest = safeSave(jmsg->header->dest);
    logPtr->eventLog.jobMsgLog.msg = safeSave(jmsg->msg);

    if (putEventRec("log_jobmsg") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jp->jobId), "putEventRec");
        mbdDie(MASTER_FATAL);
    }
}

void
log_jobmsgack(struct bucket * bucket)
{
    static char             fname[] = "log_jobmsgack";
    struct jData           *jp;

    LSBMSG_DECL(header, jmsg);

    LSBMSG_INIT(header, jmsg);
    jp = getJobData(bucket->proto.jobId);
    if (jp == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6715,
                                         "%s: Job <%d> not found in job list "),
                  fname,
                  logPtr->eventLog.jobMsgAckLog.jobId);
    }
    if (openEventFile("log_donemsgjob") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jp->jobId), "openEventFile");
            mbdDie(MASTER_FATAL);
    }
    xdr_setpos(&bucket->xdrs, LSF_HEADER_LEN);
    if (!xdr_lsbMsg(&bucket->xdrs, &jmsg, NULL)) {

        return;
    }
    logPtr->type = EVENT_JOB_MSG_ACK;
    logPtr->eventLog.jobMsgAckLog.usrId = jmsg.header->usrId;
    logPtr->eventLog.jobMsgAckLog.jobId = LSB_ARRAY_JOBID(jmsg.header->jobId);
    logPtr->eventLog.jobMsgAckLog.idx = LSB_ARRAY_IDX(jmsg.header->jobId);
    logPtr->eventLog.jobMsgAckLog.msgId = jmsg.header->msgId;
    logPtr->eventLog.jobMsgAckLog.type = jmsg.header->type;
    logPtr->eventLog.jobMsgAckLog.src = safeSave(jmsg.header->src);
    logPtr->eventLog.jobMsgAckLog.dest = safeSave(jmsg.header->dest);
    logPtr->eventLog.jobMsgAckLog.msg = safeSave(jmsg.msg);
    if (bucket->xdrs.x_op == XDR_DECODE && jmsg.msg)
        FREEUP (jmsg.msg);

    if (putEventRec("log_donemsgjob") < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jp->jobId), "putEventRec");
        mbdDie(MASTER_FATAL);
    }

}

static int
replay_signaljob(char *filename, int lineNum)
{
    static char             fname[] = "replay_signaljob";
    struct jData           *jp;
    LS_LONG_INT             jobId;
    int                    cc;

    jobId = LSB_JOBID(logPtr->eventLog.signalLog.jobId,
                      logPtr->eventLog.signalLog.idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list "),
                  fname, filename, lineNum, logPtr->eventLog.signalLog.jobId);
        return (FALSE);
    }


    cc = replay_arrayrequeue(jp,
                             &(logPtr->eventLog.signalLog));
    if (cc == 0) {
        return(TRUE);
    }

    if (strcmp(logPtr->eventLog.signalLog.signalSymbol, "DELETEJOB") == 0){
        jp->pendEvent.sigDel = TRUE;

    }

    else if (strcmp(logPtr->eventLog.signalLog.signalSymbol, "KILLREQUEUE")
             == 0) {
        jp->pendEvent.sigDel |= DEL_ACTION_REQUEUE;
    }
    else
        getSigVal(logPtr->eventLog.signalLog.signalSymbol);

    if (jp->nodeType == JGRP_NODE_ARRAY) {
        return (TRUE);
    }

    if (strcmp(logPtr->eventLog.signalLog.signalSymbol, "DELETEJOB") == 0) {

        if (IS_FINISH(jp->jStatus)){
            jp->runCount = logPtr->eventLog.signalLog.runCount;

        }else if (IS_PEND(jp->jStatus)) {
            if (logPtr->eventLog.signalLog.runCount == 0) {
                jp->runCount = 1;
                jStatusChange(jp, JOB_STAT_EXIT, logPtr->eventTime, fname);
            } else
                jp->runCount = logPtr->eventLog.signalLog.runCount;

        } else {
            if (logPtr->eventLog.signalLog.runCount == 0) {
                jp->runCount = 1;
                jp->pendEvent.sig = SIGKILL;
                eventPending = TRUE;
            } else
                jp->runCount = logPtr->eventLog.signalLog.runCount + 1;
        }
    }
    return (TRUE);

}


static int
replay_arrayrequeue(struct jData *jPtr,
                    const struct signalLog *sigLogPtr)
{
    struct signalReq      sigReq;
    char                  *p;

    sigReq.sigValue = SIG_ARRAY_REQUEUE;


    p = sigLogPtr->signalSymbol;

    if (strcmp("REQUEUE_PSUSP", p) == 0) {

        sigReq.chkPeriod = JOB_STAT_PSUSP;
        sigReq.actFlags   = sigLogPtr->runCount;

    } else if (strcmp("REQUEUE_PEND", p) == 0) {

        sigReq.chkPeriod = JOB_STAT_PEND;
        sigReq.actFlags  = sigLogPtr->runCount;

    } else {
        return(-1);
    }

    arrayRequeue(jPtr, &sigReq, NULL);

    return(0);

}

static int
replay_jobmsg(char *filename, int lineNum)
{
    static char             fname[] = "replay_jobmsg";
    int                     len;
    struct jData           *jp;
    struct bucket          *bucket;
    struct Buffer          *buf;
    struct lsbMsgHdr        jmHdr;
    struct lsbMsg           jmsg;
    struct LSFHeader        lsfHdr;
    LS_LONG_INT             jobId;

    jobId = LSB_JOBID(logPtr->eventLog.jobMsgLog.jobId,
                      logPtr->eventLog.jobMsgLog.idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list "),
                  fname,
                  filename,
                  lineNum,
                  logPtr->eventLog.jobMsgLog.jobId);

        return (FALSE);
    }


    len = LSF_HEADER_LEN + 4 * sizeof(int) +
        strlen(logPtr->eventLog.jobMsgLog.src) + 1 +
        strlen(logPtr->eventLog.jobMsgLog.dest) + 1 +
        strlen(logPtr->eventLog.jobMsgLog.msg) + 1;

    while (len % sizeof(char *)) len++;
    len += sizeof(char *);

    if (chanAllocBuf_(&buf, len) < 0) return FALSE;
    NEW_BUCKET(bucket, buf);
    if (! bucket) return FALSE;

    buf->len = len;
    bucket->proto.usrId = logPtr->eventLog.jobMsgLog.usrId;
    bucket->proto.jobId = LSB_JOBID(logPtr->eventLog.jobMsgLog.jobId,
                                    logPtr->eventLog.jobMsgLog.idx);
    bucket->proto.msgId = logPtr->eventLog.jobMsgLog.msgId;
    bucket->bufstat = MSG_STAT_QUEUED;

    lsfHdr.opCode = BATCH_JOB_MSG;
    xdrmem_create(&bucket->xdrs, buf->data, buf->len, XDR_ENCODE);

    jmHdr.usrId = logPtr->eventLog.jobMsgLog.usrId;
    jmHdr.msgId = logPtr->eventLog.jobMsgLog.msgId;
    jmHdr.jobId = LSB_JOBID(logPtr->eventLog.jobMsgLog.jobId,
                            logPtr->eventLog.jobMsgLog.idx);
    jmHdr.type = logPtr->eventLog.jobMsgLog.type;
    jmHdr.src = logPtr->eventLog.jobMsgLog.src;
    jmHdr.dest = logPtr->eventLog.jobMsgLog.dest;
    jmsg.header = &jmHdr;
    jmsg.msg = logPtr->eventLog.jobMsgLog.msg;

    if (!xdr_encodeMsg(&bucket->xdrs, (char *)&jmsg,
                       &lsfHdr, xdr_lsbMsg, 0, NULL)) {

        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        chanFreeBuf_(buf);
        return (-1);
    }


    QUEUE_APPEND(bucket, jp->hPtr[0]->msgq[MSG_STAT_QUEUED]);
    buf->stashed = TRUE;

    return (TRUE);

}

static int
replay_jobmsgack(char *filename, int lineNum)
{
    static char             fname[] = "replay_jobmsgack";
    struct jData           *jp;
    struct bucket          *bucket;
    int                     found;
    LS_LONG_INT             jobId;

    jobId = LSB_JOBID(logPtr->eventLog.jobMsgAckLog.jobId,
                      logPtr->eventLog.jobMsgAckLog.idx);
    if ((jp = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6709,
                                         "%s: File %s at line %d: Job <%d> not found in job list "),
                  fname,
                  filename,
                  lineNum,
                  logPtr->eventLog.jobMsgAckLog.jobId);

        return (FALSE);
    }


    bucket = jp->hPtr[0]->msgq[MSG_STAT_QUEUED];

    found = FALSE;
    while (1) {
        if (bucket->forw == bucket)
            break;

        if (bucket->proto.jobId == jobId &&
            bucket->proto.msgId == logPtr->eventLog.jobMsgAckLog.msgId) {

            found = TRUE;
            break;
        }
        bucket = bucket->forw;
    }

    if (found) {

        bucket->bufstat = MSG_STAT_RCVD;
        QUEUE_REMOVE(bucket);
        chanFreeStashedBuf_(bucket->storage);
        FREE_BUCKET(bucket);
    } else {
        return(FALSE);
    }

    return (TRUE);
}

static void
ckSchedHost (void)
{
    static char fname[] = "ckSchedHost";
    int list;
    struct jData *jp;
    struct hData *hp;

    if (logclass & LC_SCHED)
        ls_syslog(LOG_DEBUG3, "%s: Entering this routine....", fname);

    for (list = 0; list < ALLJLIST; list++) {
        for (jp = jDataList[list]->forw; (jp != jDataList[list]);
             jp = jp->forw) {
            if ((hp = getHostData (jp->shared->jobBill.fromHost)) != NULL) {
                if (strcmp (jp->shared->jobBill.schedHostType, "") == 0
                    || strcmp (jp->schedHost, "") == 0) {
                    free(jp->schedHost);
                    free(jp->shared->jobBill.schedHostType);
                    jp->schedHost = safeSave(hp->hostType);
                    jp->shared->jobBill.schedHostType = safeSave(hp->hostType);
                    if (logclass & LC_SCHED)
                        ls_syslog(LOG_DEBUG3, "%s: job=%s fromHost=%s schedHost=%s", fname, lsb_jobid2str(jp->jobId), jp->shared->jobBill.fromHost, jp->schedHost);
                }
            }
        }
    }
}


static bool_t
replay_jobforce(char* file, int line)
{

    static char                fname[] = "replay_jobforce()";
    struct jobForceRequestLog* jobForceRequestLog;
    struct jData*              job;
    LS_LONG_INT                jobId;

    jobForceRequestLog = &(logPtr->eventLog.jobForceRequestLog),

        jobId = LSB_JOBID(jobForceRequestLog->jobId, jobForceRequestLog->idx);
    job = getJobData(jobId);
    if (job == NULL) {
        ls_syslog(LOG_ERR, "\
%s: JobId <%d> at line <%d> cannot be found by master daemon",
                  fname, jobForceRequestLog->jobId, line);
        return(FALSE);
    }

    job->jFlags |= (jobForceRequestLog->options & RUNJOB_OPT_NOSTOP) ?
        JFLAG_URGENT_NOSTOP : JFLAG_URGENT;

    return (TRUE);

}


static int
canSwitch(struct eventRec *logPtr, struct jData *jp)
{
    if (jp) {
        if (jp->startTime > 0 && logPtr->eventTime < jp->startTime)
            return(TRUE);
        else
            return(FALSE);
    }
    else
        return(TRUE);
    return(FALSE);
}

void
log_jobattrset(struct jobAttrInfoEnt *jobAttr, int uid)
{
    static char fname[] = "log_jobattrset()";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jobAttr->jobId), "openEventFile");
        mbdDie(MASTER_FATAL);
    }

    logPtr->type = EVENT_JOB_ATTR_SET;
    logPtr->eventLog.jobAttrSetLog.uid = uid;
    logPtr->eventLog.jobAttrSetLog.jobId = LSB_ARRAY_JOBID(jobAttr->jobId);
    logPtr->eventLog.jobAttrSetLog.idx = LSB_ARRAY_IDX(jobAttr->jobId);
    logPtr->eventLog.jobAttrSetLog.port = (int) jobAttr->port;
    logPtr->eventLog.jobAttrSetLog.hostname = jobAttr->hostname;

    if (putEventRec(fname) < 0)
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jobAttr->jobId), "putEventRec");
}


static int
replay_jobattrset(char *filename, int lineNum)
{
    static char                fname[] = "replay_jobattrset()";
    struct jobAttrSetLog*      jobAttrSetLog;
    struct jData*              job;
    LS_LONG_INT                jobId;

    jobAttrSetLog = &(logPtr->eventLog.jobAttrSetLog);

    jobId = LSB_JOBID(jobAttrSetLog->jobId, jobAttrSetLog->idx);
    if ((job = getJobData(jobId)) == NULL) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6866,
                                         "%s: JobId <%d> at line <%d> cannot be found by master daemon"), /* catgets 6866 */
                  fname, jobAttrSetLog->jobId, lineNum);
        return(FALSE);
    }
    job->port = (u_short) jobAttrSetLog->port;

    return (TRUE);
}

static struct jData *
checkJobInCore(LS_LONG_INT jobId)
{
    struct jData *jp = NULL;
    struct listSet *ptr;

    if ((jp = getJobData(jobId)) != NULL)
        return(jp);

    for (ptr = voidJobList; ptr; ptr = ptr->next) {
        if (jobId == ((struct jData *)ptr->elem)->jobId ||
            jobId == LSB_ARRAY_JOBID(((struct jData *)ptr->elem)->jobId)) {
            return((struct jData *)ptr->elem);
        }
    }
    return(NULL);
}

static char *
instrJobStarter1(char *data, int  datalen, char *begin, char *end, char *jstr)
{
    static char fname[] = "instrJobStarter1()";
    char        *jstr_header = NULL;
    char        *jstr_tailer = NULL;
    char        *ptr1 = NULL;
    char        *ptr2 = NULL;
    int         int_input = 0;
    int         withKeyword = FALSE;
    char        *tmpBuf;

    tmpBuf = (char *) malloc(datalen + strlen(jstr) + 1);
    if (tmpBuf == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "malloc", "tmpBuf");
        return(NULL);
    }
    memset((char *)tmpBuf, 0, datalen + strlen(jstr) + 1);


    ptr1 = strstr(jstr, JOB_STARTER_KEYWORD);
    if (ptr1 == NULL) {
        withKeyword = FALSE;
        jstr_header = (char *) malloc(strlen(jstr)+sizeof(char));
        if (jstr_header == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "malloc",
                      "jstr_header");
            FREEUP(tmpBuf);
            return(NULL);
        } else {
            strcpy(jstr_header, jstr);
        }
        jstr_tailer = "";
    } else {
        withKeyword = TRUE;
        if (jstr == ptr1) {
            jstr_header = NULL;
            jstr_tailer = ptr1+strlen(JOB_STARTER_KEYWORD);
        } else {
            jstr_header = (char *) malloc((ptr1-jstr)+sizeof(char));
            if (jstr_header == NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname,
                          "malloc","jstr_header");
                FREEUP(tmpBuf);
                return(NULL);
            } else {
                memset(jstr_header, '\0', (ptr1-jstr)+sizeof(char));
                strncpy(jstr_header, jstr, (ptr1-jstr));
            }
            jstr_tailer = ptr1+strlen(JOB_STARTER_KEYWORD);
        }
    }


    ptr1 = strstr(data, begin);

    if (ptr1 == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "strstr", "CMDSTART");
        FREEUP(jstr_header);
        FREEUP(tmpBuf);
        return(NULL);
    }

    ptr1 = strchr(ptr1, '\n');

    if (ptr1 == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "strstr", "CMDSTART");
        FREEUP(jstr_header);
        FREEUP(tmpBuf);
        return(NULL);
    }


    if ((ptr2 = strstr(ptr1, "\n$LSB_JOBFILENAME.shell"))!= NULL) {
        int_input = 1;
        ptr1 = ptr2;
    }

    ptr1 = ptr1 + sizeof(char);


    strncat(tmpBuf, data, ptr1-data);

    if (jstr_header != NULL){
        strncat(tmpBuf, jstr_header, strlen(jstr_header));
        if (jstr_tailer[0]=='\0' && !withKeyword) {

            strncat(tmpBuf, " ", 1);
        }
    }

    if (int_input == 1)
        ptr2 = strchr (ptr1, '\n');
    else
        ptr2 = strstr(ptr1, end);

    if (ptr2 == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "strstr","WAITCLEANCMD");
        FREEUP(jstr_header);
        FREEUP(tmpBuf);
        return(NULL);
    }

    strncat(tmpBuf, ptr1, ptr2-ptr1);

    if (strlen(jstr_tailer) != 0) {
        strncat(tmpBuf, jstr_tailer, strlen(jstr_tailer));
    }
    strcat(tmpBuf, ptr2);
    FREEUP(jstr_header);
    return(tmpBuf);
}


static int
checkJobStarter(char *data, char *jStarter)
{
    char *ptr;
    char *header;
    char *tailer;

    ptr = strstr(jStarter, JOB_STARTER_KEYWORD);
    if (!ptr) {

        if (strstr(data, jStarter))
            return(TRUE);
        else
            return(FALSE);
    } else {
        if (jStarter == ptr) {
            tailer = ptr + strlen(JOB_STARTER_KEYWORD);
            SKIPSPACE(tailer);
            if (*tailer == '\0')
                return(FALSE);
            if (strstr(data, tailer))
                return(TRUE);
            else
                return(FALSE);
        } else {
            int retcode;
            char saver;

            header = jStarter;
            saver = *ptr;
            *ptr = '\0';
            if (!strstr(data, header)) {
                retcode = FALSE;
            } else {
                tailer = ptr + strlen(JOB_STARTER_KEYWORD);
                SKIPSPACE(tailer);
                if (*tailer == '\0')
                    retcode = TRUE;
                else if (!strstr(data, tailer))
                    retcode = FALSE;
                else
                    retcode = TRUE;
            }
            *ptr = saver;
            return(retcode);
        }
    }
}

static void
initHostCtrlTable(void)
{
    h_initTab_(&hostCtrlEvents, 64);

}

static void
destroyHostCtrlTable(void)
{
    h_freeTab_(&hostCtrlEvents, destroyHostCtrlEvent);

}

static void
destroyHostCtrlEvent(void *ptr)
{
    struct hostCtrlEvent *ctrlPtr;

    ctrlPtr = (struct hostCtrlEvent *)ptr;

    free(ctrlPtr->hostCtrlLog);
    free(ctrlPtr);

}

/* saveHostCtrlEvent()
 */
static void
saveHostCtrlEvent(struct hostCtrlLog *hostCtrlLog,
                  const time_t       eventTime)
{
    static char             fname[] = "saveHostCtrlEvent()";
    hEnt                    *hEntry;
    int                     new;
    struct hData            *hPtr;
    struct hostCtrlEvent    *ctrlPtr;


    if (hostCtrlLog->opCode != HOST_CLOSE) {
        return;
    }

    hPtr = getHostData(hostCtrlLog->host);
    if (hPtr == NULL) {
        return;
    }


    if (! (hPtr->hStatus & HOST_STAT_DISABLED)) {
        return;
    }

    hEntry = h_getEnt_(&hostCtrlEvents, hostCtrlLog->host);
    if (hEntry == NULL) {


        ctrlPtr =
            (struct hostCtrlEvent *)
            my_calloc(1,
                      sizeof(struct hostCtrlEvent),
                      fname);

        ctrlPtr->hostCtrlLog =
            (struct hostCtrlLog *)
            my_calloc(1,
                      sizeof(struct hostCtrlLog),
                      fname);

        ctrlPtr->time = eventTime;
        memcpy(ctrlPtr->hostCtrlLog,
               hostCtrlLog,
               sizeof(struct hostCtrlLog));

        hEntry = h_addEnt_(&hostCtrlEvents,
                           ctrlPtr->hostCtrlLog->host,
                           &new);
        hEntry->hData = ctrlPtr;

    } else {


        ctrlPtr = (struct hostCtrlEvent *)hEntry->hData;

        ctrlPtr->time = eventTime;
        memcpy((int *)ctrlPtr->hostCtrlLog,
               (int *)hostCtrlLog,
               sizeof(struct hostCtrlLog));
    }

}

/*
 * Write the host status records kept for this switch: through the event
 * write route, or into fp (the switch child's lsb.events.tmp).
 */
static void
writeHostCtrlEvent(FILE *fp)
{
    struct hostCtrlEvent  *ctrlPtr;
    struct eventRec        rec;
    hEnt                  *hEntry;
    char                  *key;

    FOR_EACH_HTAB_ENTRY(key, hEntry, &hostCtrlEvents) {

        ctrlPtr = (struct hostCtrlEvent *)hEntry->hData;

        ctrlPtr->hostCtrlLog->opCode = HOST_CLOSE;

        if (fp == NULL) {
            log_hostStatusAtSwitch(ctrlPtr);
            continue;
        }

        memset(&rec, 0, sizeof(rec));
        rec.eventLog.hostCtrlLog = *ctrlPtr->hostCtrlLog;
        writeEventRec(fp, &rec, EVENT_HOST_CTRL, ctrlPtr->time);

    } END_FOR_EACH_HTAB_ENTRY;

}

static void
log_hostStatusAtSwitch(const struct hostCtrlEvent *ctrlPtr)
{
    static char    fname[] = "log_hostStatusAtSwitch";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_HOST_FAIL,
                  fname,
                  ctrlPtr->hostCtrlLog->host,
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }

    logPtr->type = EVENT_HOST_CTRL;

    logPtr->eventLog.hostCtrlLog.opCode
        = ctrlPtr->hostCtrlLog->opCode;

    strcpy(logPtr->eventLog.hostCtrlLog.host,
           ctrlPtr->hostCtrlLog->host);

    logPtr->eventLog.hostCtrlLog.userId
        = ctrlPtr->hostCtrlLog->userId;

    strcpy(logPtr->eventLog.hostCtrlLog.userName,
           ctrlPtr->hostCtrlLog->userName);

    if (putEventRecTime(fname, ctrlPtr->time) < 0) {
        ls_syslog(LOG_ERR, I18N_HOST_FAIL,
                  fname,
                  ctrlPtr->hostCtrlLog->host,
                  "putEventRecTime");
    }

}

static void
initQueueCtrlTable(void)
{
    h_initTab_(&closedQueueEvents, 64);
    h_initTab_(&inactiveQueueEvents, 64);

}

static void
destroyQueueCtrlTable(void)
{
    h_freeTab_(&closedQueueEvents, destroyQueueCtrlEvent);
    h_freeTab_(&inactiveQueueEvents, destroyQueueCtrlEvent);

}

static void
destroyQueueCtrlEvent(void *ptr)
{
    struct queueCtrlEvent    *ctrlPtr;

    ctrlPtr = (struct queueCtrlEvent *)ptr;

    free(ctrlPtr->queueCtrlLog);
    free(ctrlPtr);

}
static void
saveQueueCtrlEvent(struct queueCtrlLog *queueCtrlLog,
                   const time_t        eventTime)
{
    struct qData    *qPtr;


    if ( (queueCtrlLog->opCode != QUEUE_CLOSED)
         && (queueCtrlLog->opCode != QUEUE_INACTIVATE)) {
        return;
    }

    qPtr = getQueueData(queueCtrlLog->queue);
    if (qPtr == NULL) {
        return;
    }


    if ( (queueCtrlLog->opCode == QUEUE_CLOSED)
         && ( ! (qPtr->qStatus & QUEUE_STAT_OPEN))) {

        queueTableUpdate(&closedQueueEvents, queueCtrlLog);

    }


    if ( (queueCtrlLog->opCode == QUEUE_INACTIVATE)
         && ( ! (qPtr->qStatus & QUEUE_STAT_ACTIVE))) {

        queueTableUpdate(&inactiveQueueEvents, queueCtrlLog);

    }

}
/* queueTableUpdate()
 */
static void
queueTableUpdate(hTab                 *tbPtr,
                 struct queueCtrlLog  *queueCtrlLog)
{
    static char              fname[] = "queueTableUpdate";
    int                      new;
    hEnt                     *hEntry;
    struct queueCtrlEvent    *ctrlPtr;

    hEntry = h_getEnt_(tbPtr, queueCtrlLog->queue);
    if (hEntry == NULL) {

        ctrlPtr =  my_calloc(1,
                             sizeof(struct queueCtrlEvent),
                             fname);

        ctrlPtr->queueCtrlLog = my_calloc(1,
                                          sizeof(struct queueCtrlLog),
                                          fname);
        ctrlPtr->time = eventTime;
        memcpy(ctrlPtr->queueCtrlLog,
               queueCtrlLog,
               sizeof(struct queueCtrlLog));

        hEntry = h_addEnt_(tbPtr,
                           ctrlPtr->queueCtrlLog->queue,
                           &new);
        hEntry->hData = ctrlPtr;

    } else {


        ctrlPtr = (struct queueCtrlEvent *)hEntry->hData;

        ctrlPtr->time = eventTime;
        memcpy((int *)ctrlPtr->queueCtrlLog,
               (int *)queueCtrlLog,
               sizeof(struct queueCtrlLog));
    }

}

/* writeQueueCtrlEvent()
 */
/*
 * Write the queue status records kept for this switch; see
 * writeHostCtrlEvent() for fp.
 */
static void
writeQueueCtrlEvent(FILE *fp)
{
    struct queueCtrlEvent    *ctrlPtr;
    struct eventRec           rec;
    struct {
        hTab *tab;
        int   opCode;
    }                         sets[2];
    hEnt                     *hEntry;
    char                     *key;
    int                       i;

    sets[0].tab = &closedQueueEvents;
    sets[0].opCode = QUEUE_CLOSED;
    sets[1].tab = &inactiveQueueEvents;
    sets[1].opCode = QUEUE_INACTIVATE;

    for (i = 0; i < 2; i++) {

        FOR_EACH_HTAB_ENTRY(key, hEntry, sets[i].tab) {

            ctrlPtr = (struct queueCtrlEvent *)hEntry->hData;

            ctrlPtr->queueCtrlLog->opCode = sets[i].opCode;

            if (fp == NULL) {
                log_queueStatusAtSwitch(ctrlPtr);
                continue;
            }

            memset(&rec, 0, sizeof(rec));
            rec.eventLog.queueCtrlLog = *ctrlPtr->queueCtrlLog;
            writeEventRec(fp, &rec, EVENT_QUEUE_CTRL, ctrlPtr->time);

        } END_FOR_EACH_HTAB_ENTRY;
    }


}

static void
log_queueStatusAtSwitch(const struct queueCtrlEvent *ctrlPtr)
{
    static char    fname[] = "log_queueStatusAtSwitch";

    if (openEventFile(fname) < 0) {
        ls_syslog(LOG_ERR, I18N_QUEUE_FAIL,
                  fname,
                  ctrlPtr->queueCtrlLog->queue,
                  "openEventFile");
        mbdDie(MASTER_FATAL);
    }

    logPtr->type = EVENT_QUEUE_CTRL;

    logPtr->eventLog.queueCtrlLog.opCode
        = ctrlPtr->queueCtrlLog->opCode;

    strcpy(logPtr->eventLog.queueCtrlLog.queue,
           ctrlPtr->queueCtrlLog->queue);

    logPtr->eventLog.queueCtrlLog.userId
        = ctrlPtr->queueCtrlLog->userId;

    strcpy(logPtr->eventLog.queueCtrlLog.userName,
           ctrlPtr->queueCtrlLog->userName);

    if (putEventRecTime(fname, ctrlPtr->time) < 0) {
        ls_syslog(LOG_ERR, I18N_QUEUE_FAIL,
                  fname,
                  ctrlPtr->queueCtrlLog->queue,
                  "putEventRecTime");
    }

}

static int
renameAcctLogFiles(int fileLimit)
{
    static char fname[] = "renameAcctLogFiles";
    int i;
    int max;
    char tmpfn[MAXFILENAMELEN], acctfn[MAXFILENAMELEN];
    struct stat st;

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG, "%s: Entering ... ", fname);

    chuser(managerId);
    i = 0;
    if (fileLimit > 0){
        do {
            sprintf(tmpfn, "%s/logdir/lsb.acct.%d",
                    daemonParams[LSB_SHAREDIR].paramValue, ++i);
        } while ((stat(tmpfn, &st) == 0) && (i < fileLimit));
    } else {
        do {
            sprintf(tmpfn, "%s/logdir/lsb.acct.%d",
                    daemonParams[LSB_SHAREDIR].paramValue, ++i);
        } while (stat(tmpfn, &st) == 0);
    }

    if ((errno != ENOENT) && (errno != 0)) {
        chuser(batchId);
        ls_syslog(LOG_WARNING, I18N_FUNC_S_FAIL_M, fname, "stat", tmpfn);
        chuser(managerId);
    }

    max = i;
    while (i--) {

        sprintf(tmpfn, "%s/logdir/lsb.acct.%d",
                daemonParams[LSB_SHAREDIR].paramValue, i);
        sprintf(acctfn, "%s/logdir/lsb.acct.%d",
                daemonParams[LSB_SHAREDIR].paramValue, i + 1);

        if (rename(tmpfn, acctfn) == -1 && errno != ENOENT) {
            chuser(batchId);
            ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname,
                      "rename", tmpfn, acctfn);
            return (-1);
        }
    }
    chuser(batchId);
    return (max);
}

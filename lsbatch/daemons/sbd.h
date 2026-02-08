/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: sbd.h 397 2007-11-26 19:04:00Z mblack $
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
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pwd.h>
#include <string.h>

#include <sys/ioctl.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include "daemonout.h"
#include "daemons.h"
#include "../../lsf/lib/lib.rf.h"
#include "../../lsf/lib/lib.osal.h"

#define NULLFILE "/dev/null"
#define JOBFILEEXT ""

#define JOBFILE_CREATED -1

enum {
    JSUPER_STAT_SUSP
};

struct jobCard {
    struct jobCard *forw,*back;
    gid_t     execGid;
    char      execUsername[MAX_LSB_NAME_LEN];
    int       notReported;
    time_t    windEdge;
    windows_t *week[8];

    char      active;
    char      timeExpire;
    char      missing;
    char      mbdRestarted;
    time_t    windWarnTime;
    int       runTime;
    int       w_status;
         /* pre-exec report flag */
    float     cpuTime;
    time_t    lastChkpntTime;
    int       migCnt;
    struct jobSpecs jobSpecs;
    struct lsfRusage lsfRusage;
    int needReportRU;
    int    cleanupPid;
    int    collectedChild;
    int    execJobFlag;
#define JOB_EXEC_QPRE_OK          0x1
#define JOB_EXEC_QPRE_KNOWN       0x2
#define JOB_EXEC_STARTED          0x4

    char   *stdinFile;

    time_t lastStatusMbdTime;

    struct jRusage runRusage;
    struct jRusage mbdRusage;

    struct jRusage maxRusage;

    int delieveredMsgId;
    struct clientNode *client;
    int regOpFlag;
    bool_t    newPam;
    struct    jRusage  wrkRusage;
    int       jSupStatus;
#define REG_CHKPNT 0x1
#define REG_SIGNAL 0x2
#define REG_RUSAGE 0x4
#define REG_NICE   0x8
    struct resVal *resumeCondVal;
    struct resVal *stopCondVal;
    int    actFlags;
    int    actStatus;
    int    actReasons;
    int    actSubReasons;
    int    exitPid;
    int    jobDone;
    time_t lastCheck;
    char   *actCmd;
    char   *exitFile;
    char   *clusterName;
    int    servSocket;
    int    crossPlatforms;
    char *spooledExec;
    char   postJobStarted;
    char   userJobSucc;
};

typedef enum {
    NO_SIGLOG,
    SIGLOG
} logType;


#define JOB_RUNNING(jp) (((jp)->jobSpecs.jStatus & JOB_STAT_RUN) && \
                          (JOB_STARTED(jp)))

#define FILE_ERRNO(errno) \
	(errno == ENOENT || errno == EPERM || errno == EACCES || \
	 errno == ELOOP || errno == ENAMETOOLONG || errno == ENOTDIR || \
	 errno == EBADF || errno == EFAULT || \
	 errno == EEXIST || errno == ENFILE || errno == EINVAL || \
	 errno == EISDIR || errno == ENOSPC || errno == ENXIO || \
	 errno == EROFS || errno == ETXTBSY)


struct clientNode {
    struct clientNode *forw;
    struct clientNode *back;
    int    chanfd;
    struct sockaddr_in from;
    int jobType;
    LS_LONG_INT jobId;
    struct jobCard *jp;
};

struct jobSetup {
    LS_LONG_INT jobId;
    int jStatus;
    float cpuTime;
    int w_status;
    struct lsfRusage lsfRusage;
    int reason;
    int jobPid;
    int jobPGid;
    int execGid;
    int execUid;
    char execUsername[MAX_LSB_NAME_LEN];
    char execHome[MAXFILENAMELEN];
    char execCwd[MAXFILENAMELEN];
    int execJobFlag;

#define LSB_PRE_ABORT 99
};

struct jobSyslog {
    int logLevel;
    char msg[MAXLINELEN];
};

#define UID_MAPPED(jp) (strcmp((jp)->jobSpecs.userName, (jp)->execUsername))
#define PURE_INTERACTIVE(jobSpecsPtr) \
     (((jobSpecsPtr)->options & SUB_INTERACTIVE) && \
       !((jobSpecsPtr)->options & (SUB_IN_FILE | SUB_OUT_FILE | SUB_ERR_FILE)) \
       && !( (jobSpecsPtr)->preCmd && (jobSpecsPtr)->preCmd[0] != '\0' \
	     && (jobSpecsPtr)->postCmd && (jobSpecsPtr)->postCmd[0] != '\0') )



#define OTHER_REASONS  (SUSP_ADMIN_STOP | SUSP_RES_RESERVE)


#define SUSP_USER(jp)    ((jp)->jobSpecs.reasons & SUSP_USER_STOP)

#define SUSP_WINDOW(jp)  ((jp)->jobSpecs.reasons & SUSP_QUEUE_WINDOW)

#define SUSP_LOAD(jp)    ((jp)->jobSpecs.reasons & (LOAD_REASONS))

#define SUSP_OTHER(jp)   ((jp)->jobSpecs.reasons & (OTHER_REASONS))

#define JOB_STARTED(jp)  (((jp)->execJobFlag & JOB_EXEC_STARTED) || \
    (!daemonParams[LSB_BSUBI_OLD].paramValue && \
     PURE_INTERACTIVE(&(jp)->jobSpecs)))


extern int mbdPid;
extern int jobcnt;
extern int maxJobs;
extern int uJobLimit;
extern int pgSuspIdleT;
extern int listenNqs;
extern windows_t  *host_week[8];
extern time_t     host_windEdge;
extern char       host_active;
extern char master_unknown;
extern char myStatus;

#define NO_LIM		0x0001

extern char need_checkfinish;
extern int  failcnt;
extern float myFactor;
extern int pgSuspIdleT;
extern char *env_dir;
extern time_t bootTime;
extern unitTypes unitForLimits;

extern struct listEntry *jobQue;
extern struct jobCard *jobQueHead;
extern struct jobTable *joblist[];
extern struct clientNode *clientList;
extern struct bucket     *jmQueue;

extern int statusChan;


extern void start_master(void);
extern void shutDownClient(struct clientNode *);

extern void do_newjob(XDR *xdrs, int s, struct LSFHeader *);
extern void do_switchjob(XDR *xdrs, int s, struct LSFHeader *);
extern void do_sigjob(XDR *xdrs, int s, struct LSFHeader *);
extern void do_probe(XDR *xdrs, int s, struct LSFHeader *);
extern void do_reboot(XDR *xdrs, int s, struct LSFHeader *);
extern void do_shutdown(XDR *xdrs, int s, struct LSFHeader *);
extern void do_jobSetup(XDR *xdrs, int s, struct LSFHeader *);
extern void do_jobSyslog(XDR *xdrs, int s, struct LSFHeader *);
extern void do_jobMsg(struct bucket *, XDR *, int s, struct LSFHeader *);
extern void do_rmConn(XDR *, int, struct LSFHeader *, struct clientNode *);
extern void do_lsbMsg(XDR *, int s, struct LSFHeader *);
extern void deliverMsg(struct bucket *);

extern void getJobsState(struct sbdPackage *sbdPackage);
extern int status_job(mbdReqType, struct jobCard *, int, sbdReplyType);
extern void sbdSyslog(int, char *);
extern void jobSetupStatus(int, int, struct jobCard *);
extern int msgSupervisor(struct lsbMsg *, struct clientNode *);
#ifdef INTER_DAEMON_AUTH
extern int getSbdAuth(struct lsfAuth *);
#endif
extern int sendUnreportedStatus (struct chunkStatusReq *chunkStatusReq);

extern struct jobCard *addJob(struct jobSpecs *, int);
extern void refreshJob(struct jobSpecs *);
extern sbdReplyType job_exec(struct jobCard *jobCardPtr, int);
extern void status_report(void);
extern int job_finish (struct jobCard *, int);
extern void setRunLimit(struct jobCard *, int);
extern void inJobLink(struct jobCard *);
extern void deallocJobCard(struct jobCard *);
extern void freeToHostsEtc(struct jobSpecs *);
extern void saveSpecs(struct jobSpecs *, struct jobSpecs *);
extern void renewJobStat(struct jobCard *jp);
extern void jobGone(struct jobCard *jp);
extern void preJobStatus(struct jobCard *jp, int sfd);
extern int setIds(struct jobCard *jobCardPtr);
extern void preExecFinish(struct jobCard *);
extern void jobGone(struct jobCard *jp);
extern int setJobEnv(struct jobCard *);
extern int runQPost(struct jobCard *);
extern int acctMapOk(struct jobCard *);
extern int acctMapTo(struct jobCard *);
extern int postJobSetup(struct jobCard *);
extern void runUPre(struct jobCard *);
extern int runUPost(struct jobCard *);
extern int reniceJob(struct jobCard *);
extern int updateRUsageFromSuper(struct jobCard *jp, char *mbuf);
extern void sbdChild(char *, char *);
extern int initJobCard(struct jobCard *jp, struct jobSpecs *jobSpecs, int *);
extern void freeThresholds (struct thresholds *);
extern void saveThresholds (struct jobSpecs *, struct thresholds *);
extern void unlockHosts (struct jobCard *, int);
extern int lockHosts (struct jobCard *);

extern void job_checking(void);
extern int  job_resume(struct jobCard *);
extern void checkFinish(void);
extern void setInclRs (struct jobSpecs *jobSpecs, int reason);
extern void resetInclRs (struct jobSpecs *jobSpecs, int reason);
extern int testInclRs (struct jobSpecs *jobSpecs, int reason);


extern int chkpntJob(struct jobCard *, int);
extern void execRestart(struct jobCard *jobCardPtr, struct hostent *hp);


extern int rmJobBufFiles(struct jobCard *);
extern void writePreJobFail(struct jobCard *jp);

extern int appendJobFile(struct jobCard *jobCard, char *header,
			 struct hostent *hp, char *errMsg);
extern int initPaths(struct jobCard *jp, struct hostent *fromHp,
		     struct lenData *jf);
extern int rcpFile(struct jobSpecs *, struct xFile *, char *, int, char *);
extern void delCredFiles (void);
extern void jobFileExitStatus(struct jobCard *jobCard);
extern int isAbsolutePathSub(struct jobCard *, const char *);
extern int isAbsolutePathExec(const char *);

extern void milliSleep( int msec );
extern char window_ok(struct jobCard *jobPtr);
extern void child_handler(int);
extern void shout_err(struct jobCard *jobPtr, char *);
extern int  fcp(char *, char *, struct hostent *);
extern int rmDir(char *);
extern void closeBatchSocket (void);
extern void getManagerId(struct sbdPackage *);

bool_t xdr_jobSetup (XDR *, struct jobSetup *, struct LSFHeader *);
bool_t xdr_jobSyslog (XDR *, struct jobSyslog *, struct LSFHeader *);
bool_t xdr_jobCard(XDR *, struct jobCard*, struct LSFHeader *);
extern int sizeofJobCard(struct jobCard *);

extern int jobSigStart (struct jobCard *jp, int sigValue, int actFlags, int actPeriod, logType logFlag);
extern int jobact (struct jobCard *, int, char *, int, int);
extern int jobsig(struct jobCard *jobTable, int sig, int forkKill);
extern int sbdread_jobstatus (struct jobCard *jp);
extern int sbdCheckUnreportedStatus();
extern void exeActCmd(struct jobCard *jp, char *actCmd, char *exitFile);
extern void exeChkpnt(struct jobCard *jp, int chkFlags, char *exitFile);



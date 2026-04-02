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

#include "sbd.h"
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include "../../lsf/lib/mls.h"
#include <unistd.h>

#define NL_SETN		11

#include <malloc.h>

int setCmp(int *set1, int len1, int *set2, int len2);
#define MAX_ARGS        16

static int terminateAct(struct jobCard *, int, int, int, logType );
int jRunSuspendAct(struct jobCard *, int ,int ,int, int, logType);
static int jSuspResumeAct(struct jobCard *, int , int, logType);
int resumeJob (struct jobCard * ,int ,int ,logType);

static char      *getJobPgids(struct jobCard *);
static char      *getJobPids(struct jobCard *);
void errorPostProcess(struct jobCard *, int, char *);
char * exitFileSuffix ( int );

extern int rusageUpdateRate;
extern int rusageUpdatePercent;
static int jobsig1 (struct jobCard *jp, int sig, int forkSig);
static int mykillpg(struct jobCard *, int);
static int pgkillit(int, int);

static int writeChkLog(char *, char *, struct jobCard *, struct hostent *);
char *getEchkpntDir(char *name);
void suspendUntilSignal(int);
int rcvPidPGid(char *, pid_t *, pid_t *);
void exeChkpnt(struct jobCard *, int, char *);
void exeActCmd(struct jobCard *, char *, char *);

#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
#endif

int sbdlog_newstatus (struct jobCard *jp);
extern void copyJUsage(struct jRusage *to, struct jRusage *from);
int jobSigLog (struct jobCard *jp, int finishStatus);
char * exitFileSuffix ( int sigValue);

int updateJRru(struct jRusage *jru, char *jobFile);

extern char *clusterName;

int
jobSigStart (struct jobCard *jp, int sigValue,
             int actFlags, int actPeriod, logType logFlag)
{
    static char fname[] = "sbd.sig.c/jobSigStart";
    int defSigValue;

    char *actCmd;
    int cc;

    if (logclass & (LC_TRACE | LC_SIGNAL))
        ls_syslog(LOG_DEBUG, "%s: Signal %s to job %s with actFlags=%d; reasons=%x, subresons=%d, sigValue=%d",
                  fname, getLsbSigSymbol (sig_decode(sigValue)),
                  lsb_jobid2str(jp->jobSpecs.jobId), actFlags,
		  jp->jobSpecs.reasons,
                  jp->jobSpecs.subreasons, sigValue);

    defSigValue = sigValue;

    if (   !(jp->jobSpecs.jStatus & JOB_STAT_ZOMBIE)
        && IS_FINISH(jp->jobSpecs.jStatus))
        return (0);


    if (!JOB_STARTED(jp)) {
        switch(sigValue) {
            case SIGKILL:
                jp->actStatus = ACT_NO;
                return (jobsig(jp, SIGKILL, TRUE));
            case SIG_TERM_USER:
	    case SIG_KILL_REQUEUE:
            case SIG_TERM_FORCE:
                return (terminateAct(jp, sigValue, SUSP_USER_STOP, 0, logFlag));
            case SIG_TERM_LOAD:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_WINDOW:
                return (terminateAct(jp, sigValue, SUSP_QUEUE_WINDOW, 0, logFlag));
            case SIG_TERM_OTHER:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_RUNLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_RUNLIMIT, logFlag));
            case SIG_TERM_DEADLINE:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_DEADLINE, logFlag));
            case SIG_TERM_PROCESSLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_PROCESSLIMIT, logFlag));
            case SIG_TERM_CPULIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_CPULIMIT, logFlag));
            case SIG_TERM_MEMLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_MEMLIMIT, logFlag));
            default:
                return (-1);
	}
    }


    if ((jp->jobSpecs.actPid) && (isSigTerm(jp->jobSpecs.actValue)==TRUE))
        return (-1);


    if (jp->jobSpecs.actPid || (jp->jobSpecs.jStatus & JOB_STAT_MIG)) {

        switch(sigValue) {
            case SIGKILL:
                jp->actStatus = ACT_NO;
                if ((cc = jobsig(jp, SIGKILL, TRUE)) >= 0)
                    jp->jobSpecs.jStatus &= ~JOB_STAT_MIG;
                return (cc);
            case SIG_TERM_USER:
	    case SIG_KILL_REQUEUE:
            case SIG_TERM_FORCE:
                return (terminateAct(jp, sigValue, SUSP_USER_STOP, 0, logFlag));
            case SIG_TERM_LOAD:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_WINDOW:
                return (terminateAct(jp, sigValue, SUSP_QUEUE_WINDOW, 0, logFlag));
            case SIG_TERM_OTHER:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_RUNLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_RUNLIMIT, logFlag));
            case SIG_TERM_DEADLINE:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_DEADLINE, logFlag));
            case SIG_TERM_PROCESSLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_PROCESSLIMIT, logFlag));
            case SIG_TERM_CPULIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_CPULIMIT, logFlag));
            case SIG_TERM_MEMLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_MEMLIMIT, logFlag));
            default:
                return (-1);
	    }
    }


    if ( (jp->jobSpecs.jStatus & JOB_STAT_RUN)
          && jp->postJobStarted )  {

       return (-1);
    }
    if (jp->jobSpecs.jStatus & JOB_STAT_RUN)  {
        if (sigValue >= 0) {
            jp->actStatus = ACT_NO;
            switch(sigValue) {
                case SIGSTOP:
                case SIGTSTP:
                case SIGTTIN:
                case SIGTTOU:
                    if (jobsig(jp, sigValue, TRUE) < 0)
                        return (-1);
                    SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_USUSP);
                    jp->jobSpecs.reasons |= SUSP_USER_STOP;
                    jp->notReported++;
                    return (0);
                default:
                    return (jobsig(jp, sigValue, TRUE));
            }
        }

        switch (sigValue) {
            case SIG_CHKPNT:
            case SIG_CHKPNT_COPY:
                jp->actStatus = ACT_NO;
                jp->jobSpecs.chkPeriod = actPeriod;
                if ((cc = jobact(jp, sigValue, NULL, actFlags, TRUE)) >= 0) {
                    jp->actStatus = ACT_START;
                    jp->jobSpecs.jStatus |= JOB_STAT_SIGNAL;
                    if (actFlags & LSB_CHKPNT_MIG)
                        jp->jobSpecs.jStatus |= JOB_STAT_MIG;
                    if (logFlag) jobSigLog (jp, 0);
                 }
                return (cc);
            case SIG_SUSP_USER:
                return (jRunSuspendAct(jp, sigValue,
                        JOB_STAT_USUSP, SUSP_USER_STOP, 0, logFlag));
            case SIG_SUSP_LOAD:
                return (jRunSuspendAct(jp, sigValue,
                        JOB_STAT_SSUSP, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_SUSP_WINDOW:
                return (jRunSuspendAct(jp, sigValue,
                        JOB_STAT_SSUSP, SUSP_QUEUE_WINDOW, 0, logFlag));
            case SIG_SUSP_OTHER:
                return (jRunSuspendAct(jp, sigValue,
                        JOB_STAT_SSUSP, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_RESUME_USER:
            case SIG_RESUME_LOAD:
            case SIG_RESUME_WINDOW:
            case SIG_RESUME_OTHER:

                jp->actStatus = ACT_NO;
                actCmd = jp->jobSpecs.resumeActCmd;
                defSigValue = getDefSigValue_(sigValue, actCmd);

                if (sigValue != defSigValue) {

                    return (jobsig (jp, defSigValue, ((defSigValue == SIGKILL) ? TRUE : FALSE)));
                } else {
                    if ((cc = jobact(jp, sigValue, actCmd, actFlags, TRUE)) >= 0) {
                        jp->actStatus = ACT_START;
                        jp->jobSpecs.jStatus |= JOB_STAT_SIGNAL;
                        if (logFlag) jobSigLog (jp, 0);
                    }
                    return (cc);
                }
            case SIG_TERM_USER:
	    case SIG_KILL_REQUEUE:
            case SIG_TERM_FORCE:
                return (terminateAct(jp, sigValue, SUSP_USER_STOP, 0, logFlag));
            case SIG_TERM_LOAD:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_WINDOW:
                return (terminateAct(jp, sigValue, SUSP_QUEUE_WINDOW, 0, logFlag));
            case SIG_TERM_OTHER:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_RUNLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_RUNLIMIT, logFlag));
            case SIG_TERM_DEADLINE:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_DEADLINE, logFlag));
            case SIG_TERM_PROCESSLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_PROCESSLIMIT, logFlag));
            case SIG_TERM_CPULIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_CPULIMIT, logFlag));
            case SIG_TERM_MEMLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_MEMLIMIT, logFlag));
	    }
    }


    if (IS_SUSP(jp->jobSpecs.jStatus)) {
        if (sigValue >= 0) {


            if (jp->regOpFlag & REG_SIGNAL)
                return (jobsig (jp, sigValue, TRUE));


            jp->actStatus = ACT_NO;
            if (sigValue >= 0) {
                jobsig(jp, SIGCONT, FALSE);

                return (jobsig(jp, sigValue, TRUE));
	    }

	}

        switch (sigValue) {
            case SIGSTOP:
            case SIGTSTP:
            case SIGTTIN:
            case SIGTTOU:
                jp->actStatus = ACT_NO;
                SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_USUSP);
                jp->jobSpecs.reasons |= SUSP_USER_STOP;
                jp->notReported++;
                return (0);
            case SIG_CHKPNT:
            case SIG_CHKPNT_COPY:

                jp->actStatus = ACT_NO;
                jp->jobSpecs.chkPeriod = actPeriod;
                if ((cc = jobact(jp, sigValue, NULL, actFlags, TRUE)) >= 0) {
                    jp->actStatus = ACT_START;
                    jp->jobSpecs.jStatus |= JOB_STAT_SIGNAL;
                    if (actFlags & LSB_CHKPNT_MIG)
                        jp->jobSpecs.jStatus |= JOB_STAT_MIG;
                    if (logFlag) jobSigLog (jp, 0);
                }
                return (cc);

            case SIG_SUSP_USER:
                 SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_USUSP);
                 jp->jobSpecs.reasons |= SUSP_USER_STOP;
                 jp->notReported++;
                 return (0);
            case SIG_SUSP_LOAD:
                 jp->jobSpecs.reasons |= jp->actReasons;
                 jp->jobSpecs.subreasons = jp->actSubReasons;
                 jp->notReported++;
                 return (0);
            case SIG_SUSP_WINDOW:
                 jp->jobSpecs.reasons |= SUSP_QUEUE_WINDOW;
                 jp->notReported++;
                 return (0);
            case SIG_SUSP_OTHER:
                 jp->jobSpecs.reasons |= jp->actReasons;
                 jp->jobSpecs.subreasons = jp->actSubReasons;
                 jp->notReported++;
                 return (0);

            case SIG_RESUME_USER:
                return (jSuspResumeAct(jp, sigValue, SUSP_USER_STOP, logFlag));
            case SIG_RESUME_LOAD:
                return (jSuspResumeAct(jp, sigValue, (LOAD_REASONS), logFlag));
            case SIG_RESUME_WINDOW:
                return (jSuspResumeAct(jp, sigValue, SUSP_QUEUE_WINDOW, logFlag));

            case SIG_RESUME_OTHER:
                return (jSuspResumeAct(jp, sigValue, (OTHER_REASONS), logFlag));
            case SIG_TERM_USER:
	    case SIG_KILL_REQUEUE:
            case SIG_TERM_FORCE:
                return (terminateAct(jp, sigValue, SUSP_USER_STOP, 0, logFlag));
            case SIG_TERM_LOAD:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_WINDOW:
                return (terminateAct(jp, sigValue, SUSP_QUEUE_WINDOW, 0, logFlag));
            case SIG_TERM_OTHER:
                return (terminateAct(jp, sigValue, jp->actReasons, jp->actSubReasons, logFlag));
            case SIG_TERM_RUNLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_RUNLIMIT, logFlag));
            case SIG_TERM_DEADLINE:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_DEADLINE, logFlag));
            case SIG_TERM_PROCESSLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_PROCESSLIMIT, logFlag));
            case SIG_TERM_CPULIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_CPULIMIT, logFlag));
            case SIG_TERM_MEMLIMIT:
                return (terminateAct(jp, sigValue, SUSP_RES_LIMIT, SUB_REASON_MEMLIMIT, logFlag));

	    }
    }
    return(0);
}

static int
terminateAct(struct jobCard *jp, int sigValue,
             int reasons, int subReasons, logType logFlag)
{
    int defSigValue, cc;
    char *actCmd;


    if (sigValue == SIG_TERM_FORCE) {
        jp->jobSpecs.termTime = time(0);
        jp->jobSpecs.jAttrib |= JOB_FORCE_KILL;
    }

    if (sigValue == SIG_KILL_REQUEUE) {
	jp->jobSpecs.reasons |= EXIT_REQUEUE;
	sigValue = SIG_TERM_USER;
    }
    actCmd = jp->jobSpecs.terminateActCmd;
    defSigValue = getDefSigValue_(sigValue, jp->jobSpecs.terminateActCmd);


    if (jp->regOpFlag & REG_SIGNAL
	&& (actCmd[0] == '\0')) {

	if (jp->jSupStatus == JSUPER_STAT_SUSP) {
	    mykillpg(jp, SIGCONT);
	    jp->jSupStatus = -1;
	}

        return (jobsig (jp, SIGKILL, TRUE));
    }


    jp->actReasons = reasons;
    jp->actSubReasons = subReasons;
    jp->actStatus = ACT_NO;

    if (sigValue != defSigValue) {

	if( sigValue == SIG_TERM_RUNLIMIT) {
	    jobsig(jp, SIGQUIT, FALSE);
	}
        return (jobsig (jp, defSigValue, TRUE));
    } else {
        if ((cc = jobact(jp, sigValue, actCmd, 0, TRUE)) >= 0) {
            jp->actStatus = ACT_START;
            jp->jobSpecs.jStatus |= JOB_STAT_SIGNAL;
            if (logFlag) jobSigLog (jp, 0);
        }
        return (cc);
    }
}

int
jRunSuspendAct(struct jobCard *jp, int sigValue,
               int jState, int reasons, int subReasons, logType logFlag)
{
    int cc;
    int defSigValue;
    char *actCmd;

    jp->actReasons = reasons;
    jp->actSubReasons = subReasons;

    jp->actStatus = ACT_NO;
    actCmd = jp->jobSpecs.suspendActCmd;
    defSigValue = getDefSigValue_(sigValue, actCmd);


    if ((jp->regOpFlag & REG_SIGNAL)
	&& (actCmd[0] == '\0')) {

        if ((cc =  jobsig (jp, SIGSTOP, TRUE)) >= 0) {
            SET_STATE(jp->jobSpecs.jStatus, jState);
            jp->jobSpecs.reasons |= reasons;
            jp->jobSpecs.subreasons = subReasons;
            jp->jobSpecs.lastSSuspTime = now;
            jp->notReported++;
        }
        return (cc);
    }


    if (sigValue != defSigValue) {

        if ((cc =  jobsig (jp, defSigValue, TRUE)) >= 0) {
            SET_STATE(jp->jobSpecs.jStatus, jState);
            jp->jobSpecs.reasons |= reasons;
            jp->jobSpecs.subreasons = subReasons;
            jp->jobSpecs.lastSSuspTime = now;
            jp->notReported++;
        }
        return (cc);
    } else {
        if ((cc = jobact(jp, sigValue, actCmd, 0, TRUE)) >= 0) {
            jp->actStatus = ACT_START;
            jp->jobSpecs.jStatus |= JOB_STAT_SIGNAL;

            if (logFlag) jobSigLog (jp, 0);
        }


	if (jp->regOpFlag != 0) {
	    jp->jSupStatus = JSUPER_STAT_SUSP;
	}

        return (cc);
    }

}


static int
jSuspResumeAct(struct jobCard *jp, int sigValue, int suspendReasons,
               logType logFlag)
{

    int defSigValue;
    char *actCmd;

    if (logclass & (LC_TRACE | LC_SIGNAL))
        ls_syslog(LOG_DEBUG1,"jSuspResumeAct: job %s jobSpecs.reasons=%x sigValue=%d suspendReasons=%x",lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.reasons, sigValue, suspendReasons);

    jp->actStatus = ACT_NO;
    actCmd = jp->jobSpecs.resumeActCmd;
    defSigValue = getDefSigValue_(sigValue, actCmd);



    if (jp->jobSpecs.reasons & SUSP_MBD_LOCK) {


        if (sigValue == SIG_RESUME_USER) {
            if (jp->actReasons & ~(SUSP_MBD_LOCK | SUSP_USER_STOP | SUSP_USER_RESUME | SUSP_SBD_STARTUP  )) {

                 jp->jobSpecs.reasons = jp->actReasons & ~(SUSP_USER_STOP | SUSP_USER_RESUME);
                 SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_SSUSP);
                 return (0);
            } else {

                 return (resumeJob(jp, sigValue, suspendReasons, logFlag));
            }
        } else {

             return (resumeJob(jp, sigValue, suspendReasons, logFlag));
        }
    } else {


        if (jp->jobSpecs.reasons
             & ~(suspendReasons | SUSP_USER_RESUME | SUSP_MBD_LOCK)) {


            jp->jobSpecs.reasons &= ~suspendReasons;
            if (sigValue == SIG_RESUME_USER)  {
                SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_SSUSP);
                return (0);
            } else {

		return (resumeJob(jp, sigValue, suspendReasons, logFlag));
	    }
        } else {

            return (resumeJob(jp, sigValue, suspendReasons, logFlag));
        }

    }

}

int
resumeJob (struct jobCard *jp, int sigValue, int suspendReasons,
           logType logFlag)
{
    int cc;
    int defSigValue;
    char *actCmd;

    jp->actStatus = ACT_NO;
    actCmd = jp->jobSpecs.resumeActCmd;
    defSigValue = getDefSigValue_(sigValue, actCmd);


    if (jp->regOpFlag & REG_SIGNAL
	&& (actCmd[0] == '\0')) {

	if (jp->jSupStatus == JSUPER_STAT_SUSP) {
	    mykillpg(jp, SIGCONT);
	    jp->jSupStatus = -1;
	}

        if ((cc =  jobsig (jp, SIGCONT, FALSE)) >= 0) {
            SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_RUN);
            jp->jobSpecs.reasons = 0;
            jp->notReported++;
        }
        return (cc);
    }


    if (sigValue != defSigValue) {
        if ((cc =  jobsig (jp, defSigValue, ((defSigValue == SIGKILL) ? TRUE : FALSE))) >= 0) {
            SET_STATE(jp->jobSpecs.jStatus, JOB_STAT_RUN);
            jp->jobSpecs.reasons = 0;
            jp->notReported++;
        }
        return (cc);
    } else {
        if ((cc = jobact(jp, sigValue, actCmd, 0, TRUE)) >= 0) {
            jp->actStatus = ACT_START;
            jp->jobSpecs.jStatus |= JOB_STAT_SIGNAL;

            if (logFlag) jobSigLog (jp, 0);
        }
        return (cc);
    }

}


int
jobSigLog (struct jobCard *jp, int finishStatus)
{
    if (status_job(BATCH_STATUS_JOB, jp, jp->jobSpecs.jStatus,
       finishStatus == 0 ? ERR_NO_ERROR : ERR_SYSACT_FAIL) < 0) {
        jp->notReported++;
        return (-1);
    } else {
	if (jp->notReported > 0)
            jp->notReported = 0;
        jp->actStatus = ACT_NO;
        return (0);
    }
}


int
jobsig (struct jobCard *jp, int sig, int forkSig)
{
    static char fname[] = "sbd.job/jobsig";
    struct lsbMsgHdr 	pmMsgHdr;
    struct lsbMsg    	pmMsg;
    static int 		pmMsgId = 0;
    char		Msgbuf[MSGSIZE];
    int			msgStat;

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC)) {
        if (sig == 0)
            ls_syslog(LOG_DEBUG3, "%s: Send signal %d to job %s", fname,
                                   sig, lsb_jobid2str(jp->jobSpecs.jobId));
        else
            ls_syslog(LOG_DEBUG2, "%s: Send signal %d to job %s", fname,
                                   sig, lsb_jobid2str(jp->jobSpecs.jobId));
    }


    if ( (jp->regOpFlag & REG_SIGNAL) && sig ) {
	pmMsgHdr.usrId = jp->jobSpecs.userId;
	pmMsgHdr.msgId = pmMsgId++;
	pmMsgHdr.jobId = jp->jobSpecs.jobId;
	pmMsgHdr.src = "sbatchd";
	pmMsgHdr.dest = "pm";
	sprintf(Msgbuf, "SIGNAL %d", sig);
	pmMsg.msg = Msgbuf;
	pmMsg.header = &pmMsgHdr;

	if ( (msgStat = msgSupervisor(&pmMsg, jp->client)) < 0 ) {
	    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5900,
		"%s: sending signal message to pm failed for job <%s>: %m"), fname, lsb_jobid2str(jp->jobSpecs.jobId)); /* catgets 5900 */
	}
	return msgStat;
    }

    if (sig == SIGSTOP) {
	int stopSig;


	if (daemonParams[LSB_SIGSTOP].paramValue &&
	    (stopSig = getSigVal(daemonParams[LSB_SIGSTOP].paramValue)) > 0) {
	    sig = stopSig;
	} else {
	    if (jp->jobSpecs.numToHosts > 1 ||
		(jp->jobSpecs.options & SUB_INTERACTIVE))
		sig = SIGTSTP;
	}
    }

    if (sig == SIGKILL)
        return jobsig1 (jp, sig, forkSig);

    return (mykillpg(jp, sig));
}

static int
jobsig1 (struct jobCard *jp, int sig, int forkSig)
{
    static char fname[] = "jobsig1()";
    int preSig;
    int pid = -1;

    if (sig == SIGSTOP)
        preSig = SIGTSTP;
    else
        preSig = SIGINT;


    if (mykillpg(jp, SIGCONT) < 0)
	return (-1);



    if (forkSig) {
	if ((pid = fork()) < 0)
	    ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
		lsb_jobid2str(jp->jobSpecs.jobId), "fork");
    }

    if (!forkSig || pid <= 0) {

        if (forkSig && pid == 0) {
	    closeBatchSocket();
        }

       if (jp->actSubReasons == SUB_REASON_CPULIMIT) {
#ifdef SIGXCPU
            mykillpg(jp, SIGXCPU);
	    millisleep_(jobTerminateInterval*1000);
#endif
#ifdef SIGCPULIM
            mykillpg(jp, SIGCPULIM);
	    millisleep_(jobTerminateInterval*1000);
#endif
        }

        if (mykillpg(jp, preSig) == 0) {
	    millisleep_(jobTerminateInterval*1000);
	    if (sig == SIGKILL) {
                if (mykillpg(jp, SIGTERM) == 0) {
		    millisleep_(jobTerminateInterval*1000);
		    mykillpg(jp, SIGKILL);
		}
	    } else {
                millisleep_(jobTerminateInterval*1000);
                mykillpg(jp, SIGSTOP);
	    }
	}
	if (forkSig && pid == 0)
	    exit(0);
    }
    return (0);
}



static int
mykillpg(struct jobCard *jp, int sig)
{
    static char fname[] = "mykillpg";
    static int sbdPgid = -1;
    struct jRusage *jru;
    int pgid0[1], *pgid, npgid;
    int i, changed;
    int status;
    int fileStatus;
    char jobFileName[320];
    int isSave = 0;

    if (logclass & LC_SIGNAL)
	ls_syslog(LOG_DEBUG,
		  "mykillpg: Job <%s> pid %d pgid %d resstartpid %d sig %d",
		  lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.jobPid,
		  jp->jobSpecs.jobPGid, jp->jobSpecs.restartPid, sig);




    if (jp->jobSpecs.jobPGid == 0) {


	if (jp->jobSpecs.options & SUB_RESTART) {

	    if (sig == SIGKILL)
		sig = SIGTERM;
	    else if (sig == SIGSTOP)
		sig = SIGTSTP;
	}

	if (kill(jp->jobSpecs.jobPid, sig) == 0) {
	    kill(-jp->jobSpecs.jobPid, sig);
	    return (0);
	}
	return (kill(-jp->jobSpecs.jobPid, sig));
    }


    if (jp->runRusage.npgids == 0) {
	pgid = pgid0;
	pgid[0] = jp->jobSpecs.jobPGid;
	npgid = 1;
    } else {
	pgid = jp->runRusage.pgid;
	npgid = jp->runRusage.npgids;
    }

    if (jp->regOpFlag & REG_RUSAGE) {
        jru = &(jp->runRusage);
    }
    else {
        TIMEIT(0, jru = getJInfo_(npgid, pgid, 0, jp->jobSpecs.jobPGid), "getJInfo_ in mykillpg");
    }

    changed = FALSE;


    sprintf(jobFileName, "/tmp/.sbd/%s.rusage", jp->jobSpecs.jobFile);
    if (logclass & LC_MPI)
        ls_syslog(LOG_DEBUG3, "\
mykillpg: job file name = %s\n", jobFileName);

    if (jru) {


        if (jru->npids >0) {
            int j;
            if (sbdPgid < 0) {
                sbdPgid =  getpgrp ();
            }
            for (i=0; i<jru->npgids; i++) {
                if (sbdPgid == jru->pgid[i]) {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5903,
			"%s: sbatchd's pgid <%d> is the same as job <%s>"), /* catgets 5903 */
			fname, sbdPgid, lsb_jobid2str(jp->jobSpecs.jobId));
                    for (j=i; j < jru->npgids - 1; j++) {
                        jru->pgid[j] = jru->pgid[j+1];
                    }
                    jru->npgids--;
                }
            }
       }

        fileStatus = access(jobFileName, F_OK | R_OK);

        if (fileStatus == 0) {
            status = updateJRru(jru, jobFileName);
            if (status == 0) {
		if (logclass & LC_MPI) {
	            ls_syslog(LOG_DEBUG3, "mykillpg: mem  = <%d>", jru->mem);
                    ls_syslog(LOG_DEBUG3, "mykillpg: swap = <%d>", jru->swap);
	            ls_syslog(LOG_DEBUG3, "mykillpg: utime  = <%d>",jru->utime);
	            ls_syslog(LOG_DEBUG3, "mykillpg: stime  = <%d>",jru->stime);
		}
            }
	}


        if (jru->mem > jp->maxRusage.mem)
            jp->maxRusage.mem = jru->mem;
        if (jru->swap > jp->maxRusage.swap)
            jp->maxRusage.swap = jru->swap;
        if (jru->npids > jp->maxRusage.npids)
            jp->maxRusage.npids = jru->npids;
        if (jru->utime > jp->maxRusage.utime) {
            jp->maxRusage.utime = jru->utime;
        } else {
            jru->utime = jp->maxRusage.utime;
        }
        if (jru->stime > jp->maxRusage.stime) {
            jp->maxRusage.stime = jru->stime;
        } else {
            jru->stime = jp->maxRusage.stime;
        }

        /*Calculate the average of mem*/
        if ((sig == 0) && (now - jp->avgMemLastCalcTime >= sbdSleepTime)) {
            if (jp->avgMemCounters < 0) {
                jp->avgMemCounters = 0;
            }
            jp->avgMem = jp->avgMem * ((float)jp->avgMemCounters/(jp->avgMemCounters+1)) + (float)(jru->mem/(jp->avgMemCounters+1));
            jp->avgMemCounters++;
            jp->avgMemLastCalcTime = now;
            isSave = 1;
        }

	if (ABS(jru->mem - jp->mbdRusage.mem)
            > jp->mbdRusage.mem / 100.0 * (float) rusageUpdatePercent) {
            changed = TRUE;
        } else if (ABS(jru->swap - jp->mbdRusage.swap)
                > jp->mbdRusage.swap / 100.0 * (float) rusageUpdatePercent) {
            changed = TRUE;
	} else if (ABS(jp->avgMem - jp->avgMemMbd) > jp->avgMemMbd / 100.0 * (float) rusageUpdatePercent) {
            changed = TRUE;

    } else if (ABS(jru->utime - jp->mbdRusage.utime)
                > jp->mbdRusage.utime / 100.0 * (float) rusageUpdatePercent) {
            changed = TRUE;
	} else if (ABS(jru->stime - jp->mbdRusage.stime)
                 > jp->mbdRusage.stime / 100.0 * (float) rusageUpdatePercent) {
            changed = TRUE;
	} else if (setCmp(jru->pgid, jru->npgids, jp->mbdRusage.pgid,
			  jp->mbdRusage.npgids) == TRUE) {


	    if (jru->npids != jp->mbdRusage.npids)
		changed = TRUE;
	    else {
		int *set1, *set2;
		set1 = (int *) calloc (jru->npids, sizeof(int));
		set2 = (int *) calloc (jp->mbdRusage.npids, sizeof(int));
		if ((set1 == NULL) || (set2 == NULL)) {
		    changed = TRUE;
		} else {
		    for (i=0; i<jru->npids; i++) {
			set1[i] = jru->pidInfo[i].pid;
			set2[i] = jp->mbdRusage.pidInfo[i].pid;
		    }
		    if (!setCmp(set1, jru->npids, set2, jp->mbdRusage.npids))
			changed = TRUE;
		}
		FREEUP (set1);
		FREEUP (set2);
	    }
	} else {
	    changed = TRUE;
	}

	if (logclass & (LC_SIGNAL|LC_EXEC)) {
	    ls_syslog(LOG_DEBUG, "mykillpg(): Job <%s> pgid %d mem %d swap %d utime %d stime %d rusageUpdatePercent %d current mem %d swap %d avgMem %d rusageUpdateRate %d changed %d needReportRU %d lastStatusMbdTime %d sbdSleepTime %d",
		      lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.jobPGid,
		      jru->mem, jru->swap, jp->avgMem, jru->utime, jru->stime,
		      rusageUpdatePercent, jp->mbdRusage.mem,
		      jp->mbdRusage.swap, rusageUpdateRate, changed,
		      jp->needReportRU, jp->lastStatusMbdTime, sbdSleepTime);
            ls_syslog(LOG_DEBUG, "%s: npgids =%d", fname, jru->npgids);
	    for (i = 0; i < jru->npgids; i++)
		ls_syslog(LOG_DEBUG, "%s: pgid[%d]=%d", fname, i,
			  jru->pgid[i]);
	}

	if (((changed || jp->needReportRU) && now - jp->lastStatusMbdTime >
	     rusageUpdateRate * sbdSleepTime) ||
	    (now - jp->lastStatusMbdTime >
	     rusageUpdateRate * sbdSleepTime * 15)) {
	    if (logclass & LC_SIGNAL)
	        ls_syslog(LOG_DEBUG, "mykillpg(): Job <%s> will report",
			  lsb_jobid2str(jp->jobSpecs.jobId));

	    jp->needReportRU = TRUE;

	    if (! (jp->regOpFlag & REG_RUSAGE)) {
	      copyJUsage( &(jp->mbdRusage), jru);
          jp->avgMemMbd = jp->avgMem;
        }
	}

	if (! (jp->regOpFlag & REG_RUSAGE))
	    copyJUsage( &(jp->runRusage), jru);
    }

    if (isSave) {
        saveJobRusage2File(jp);
    }

    if (kill(jp->jobSpecs.jobPid, sig) == 0) {

	if (logclass & LC_SIGNAL)
	    ls_syslog(LOG_DEBUG, "%s: Job %s kill(pid=%d) is good", fname,
		      lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.jobPid);


	if (jp->runRusage.npgids > 0 && !(jp->regOpFlag & REG_RUSAGE)) {
	    for (i = 0; i < jp->runRusage.npgids; i++)
		pgkillit(jp->runRusage.pgid[i], sig);
	} else {
	    pgkillit(jp->jobSpecs.jobPGid, sig);
	}
	return (0);
    } else {
        ls_syslog(LOG_DEBUG, "%s: Job %s kill(pid=%d) <%d> failed <%m>", fname,
		  lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.jobPid, sig);
   }



    if (jp->runRusage.npgids > 0 && !(jp->regOpFlag & REG_RUSAGE)) {
	int ndead = 0;

	for (i = 0; i < jp->runRusage.npgids; i++) {
	    if (pgkillit(jp->runRusage.pgid[i], sig) < 0) {
		ndead++;
		if (logclass & LC_SIGNAL)
		    ls_syslog(LOG_DEBUG,
			      "%s: Job %s pgkillit(%d) are gone", fname,
			      lsb_jobid2str(jp->jobSpecs.jobId),
			      jp->runRusage.pgid[i]);
	    }
	}
	if (ndead == jp->runRusage.npgids) {
	    if (logclass & LC_SIGNAL)
		ls_syslog(LOG_DEBUG, "%s: Job %s pgids all dead", fname,
			  lsb_jobid2str(jp->jobSpecs.jobId));
	    return (-1);
	}
    } else {
	if (pgkillit(jp->jobSpecs.jobPGid, sig) < 0) {
	    if (logclass & LC_SIGNAL)
		ls_syslog(LOG_DEBUG, "%s: Job %s pgkillit(%d) is gone", fname,
			  lsb_jobid2str(jp->jobSpecs.jobId),
			  jp->jobSpecs.jobPGid);
	    return (-1);
	}
    }

    if (logclass & LC_SIGNAL)
	ls_syslog(LOG_DEBUG,
		  "%s: Job %s child gone, but some pgids still around, jru=%p",
		  fname, lsb_jobid2str(jp->jobSpecs.jobId), jru);

    return (0);
}

int
updateJRru(struct jRusage *jru, char *jobFile)
{
    static char fname[] = "updateJRru()";
    int  status;
    FILE *jrfPtr;
    struct jRusage tmpJru;

    jrfPtr = fopen(jobFile, "r");
    if (jrfPtr == NULL) {
	ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", jobFile);
        return -1;
    }


    tmpJru.mem = 0;
    tmpJru.swap = 0;
    tmpJru.utime = 0;
    tmpJru.stime = 0;


    status = fscanf(jrfPtr, "%d %d %d %d", &tmpJru.mem, &tmpJru.swap,
		    &tmpJru.utime, &tmpJru.stime);
    if (status == 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fscanf", jobFile);
        FCLOSEUP(&jrfPtr);
	return -1;
    }


    jru->mem = tmpJru.mem;
    jru->swap = tmpJru.swap;
    jru->utime = tmpJru.utime;
    jru->stime = tmpJru.stime;

    FCLOSEUP(&jrfPtr);

    return 0;
}
int
setCmp(int *set1, int len1, int *set2, int len2)
{
    int i, j, start;
    int tmp;

    if (len1 != len2)
        return (FALSE);
    for (i = 0; i < len1; i++) {
        start = i;
        for (j=start; j<len2; j++)
            if (set1[i] == set2[j]) {
                tmp = set2[j];
                set2[j] = set2[i];
                set2[i] = tmp;
                break;
            } else if (j == (len2 - 1))
                return (FALSE);
    }
    return (TRUE);
}


static int
pgkillit(int pgid, int sig)
{
    return (kill(-pgid, sig));
}



int
jobact (struct jobCard *jp, int actValue, char *actCmd, int actFlags, int forkSig)
{
    static char fname[] = "jobact";
    pid_t pid;
    char exitFile[MAXFILENAMELEN], msg[MAXLINELEN*2];


    jp->jobSpecs.actValue = actValue;
    jp->actFlags = actFlags;

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
            ls_syslog(LOG_DEBUG2, "%s: Send batch system signal %d to job %s", fname,
                      actValue, lsb_jobid2str(jp->jobSpecs.jobId));


    sprintf(exitFile, "%s/.%s.%s.%s", LSTMPDIR,
        jp->jobSpecs.jobFile, lsb_jobidinstr(jp->jobSpecs.jobId), exitFileSuffix(actValue));

    if (unlink(exitFile) == -1 && errno != ENOENT) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M, fname,
	    lsb_jobid2str(jp->jobSpecs.jobId), "unlink", exitFile);
        return (-1);
    }

    if ((pid = fork()) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
                  lsb_jobid2str(jp->jobSpecs.jobId), "fork");
        return (-1);
    } else if (pid == 0) {

	int cc;

        if ((actValue == SIG_CHKPNT)
            || (actValue == SIG_CHKPNT_COPY)
            || (strcmp (actCmd, "SIG_CHKPNT") == 0)
            || (strcmp (actCmd, "SIG_CHKPNT_COPY") == 0))
            putEnv(LS_EXEC_T, "CHKPNT");
        else
            putEnv(LS_EXEC_T, "JOB_CONTROLS");



        cc =  postJobSetup(jp);
        if (cc < 0 && !(cc == -2 && !JOB_STARTED(jp)) ) {
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
                      lsb_jobid2str(jp->jobSpecs.jobId), "postJobSetup");

            exit(-1);
        }



        if ((actValue == SIG_CHKPNT) || (actValue == SIG_CHKPNT_COPY)) {
            if (actValue == SIG_CHKPNT_COPY) {
                actFlags |= LSB_CHKPNT_COPY;
            }
            exeChkpnt(jp, actFlags, exitFile);
        } else if ((strcmp (actCmd, "SIG_CHKPNT") == 0)
                   || (strcmp (actCmd, "SIG_CHKPNT_COPY") == 0)) {

            if (isSigTerm(actValue)==TRUE)
                exeChkpnt(jp, LSB_CHKPNT_KILL, exitFile);
            else
                exeChkpnt(jp, LSB_CHKPNT_STOP, exitFile);
         } else
            exeActCmd(jp, actCmd, exitFile);
    }


    jp->jobSpecs.actPid = pid;
    if (! (actValue == SIG_CHKPNT ||
           actValue == SIG_CHKPNT_COPY))
        return (0);

    if (jp->regOpFlag & REG_CHKPNT) {
        struct lsbMsgHdr pmMsgHdr;
        struct lsbMsg pmMsg;
        int msgStat;

        pmMsgHdr.usrId = jp->jobSpecs.userId;
        pmMsgHdr.msgId = 0;
        pmMsgHdr.jobId = jp->jobSpecs.jobId;
        pmMsgHdr.src = "sbatchd";
        pmMsgHdr.dest = "pm";

        pmMsg.header = &pmMsgHdr;

        if (actFlags & LSB_CHKPNT_KILL) {
            sprintf(msg, "CHKPNT KILL");
        } else {
            sprintf(msg, "CHKPNT");
        }

        pmMsg.msg = msg;
        if ( (msgStat = msgSupervisor(&pmMsg, jp->client)) < 0) {
            ls_syslog(LOG_ERR,
                      _i18n_msg_get(ls_catd , NL_SETN, 5911,
                                    "%s: sending signal message to pm failed for job <%s>: %m"), /* catgets 5911 */
                      fname, lsb_jobid2str(jp->jobSpecs.jobId));
            return (msgStat);
        }
    }
    return(0);
}



void
exeActCmd(struct jobCard *jp, char *actCmd, char *exitFile)
{
    static char fname[] = "exeActCmd";
    pid_t pid1, actCmdExecPid;
    LS_WAIT_T status;
    char msg[MAXLINELEN*2];
    char errMsg[MAXLINELEN];
    int i, maxfds;
    FILE *fp;
    char jobIdStr[16];
    char suspendReasons[128];
    char suspSubReasons[128];
    char buf[MAXFILENAMELEN];


 /* _i18n_msg_get(ls_catd , NL_SETN, 5913, */






    sprintf(jobIdStr, "%d", LSB_ARRAY_JOBID(jp->jobSpecs.jobId));
    putEnv ("LSB_JOBID", jobIdStr);
    sprintf(jobIdStr, "%d", LSB_ARRAY_IDX(jp->jobSpecs.jobId));
    putEnv ("LSB_JOBINDEX", jobIdStr);


    putEnv ("LSB_JOBPGIDS", getJobPgids(jp));
    putEnv ("LSB_JOBPIDS", getJobPids(jp));


    if (jp->regOpFlag != 0) {

	if (jp->jobSpecs.jobFile[0] == '/') {
	    sprintf(buf, "%s/.%s.pidInfo", LSTMPDIR,
		    strrchr(jp->jobSpecs.jobFile, '/') + 1);
	} else {
	    sprintf(buf, "%s/.%s.pidInfo", LSTMPDIR, jp->jobSpecs.jobFile);
	}

	putEnv("LSB_PIDINFO_FILE", buf);
    }

    sprintf(suspendReasons, "%d", jp->actReasons);
    putEnv ("LSB_SUSP_REASONS", suspendReasons);
    sprintf(suspSubReasons, "%d", jp->actSubReasons);
    putEnv ("LSB_SUSP_SUBREASONS", suspSubReasons);




    if ((pid1 = fork()) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
	    lsb_jobid2str(jp->jobSpecs.jobId), "fork");
        goto Error;
    } else if (pid1 == 0) {

        sigset_t newmask;
        char *myargv[6];

	chuser(batchId);
	if (lsfSetUid(jp->jobSpecs.execUid) < 0) {
	    ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
		lsb_jobid2str(jp->jobSpecs.jobId), "setuid");
	    exit(-1);
	}

        for (i = 1; i < NSIG; i++)
            Signal_(i, SIG_DFL);

        sigemptyset(&newmask);
        sigprocmask(SIG_SETMASK, &newmask, NULL);

        alarm(0);


        i = open ("/dev/null", O_RDWR, 0);
        dup2(i, 0);
        dup2(i, 1);
        dup2(i, 2);
        maxfds = sysconf(_SC_OPEN_MAX);
        for (i = 3; i < maxfds; i++)
            close(i);

        myargv[0] = "/bin/sh";
        myargv[1] = "-c";
        myargv[2] = actCmd;
        myargv[3] = NULL;

        lsfExecvp ("/bin/sh", myargv);
        goto Error;
    }


    actCmdExecPid = pid1;

    while ((pid1 = waitpid(actCmdExecPid, &status, 0)) < 0 && errno == EINTR);

    if (pid1 < 0) {
        sprintf(errMsg, I18N_JOB_FAIL_S_M, fname,
	    lsb_jobid2str(jp->jobSpecs.jobId), "waitpid");
        goto Error;
    } else {
        if (WEXITSTATUS(status)) {
            exit (WEXITSTATUS(status));
        }
    }





    chuser(batchId);

    if ((fp = fopen(exitFile, "w")) == NULL || FCLOSEUP(&fp) == -1) {
        sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "fopen", exitFile);
        goto Error;
    }
    exit(0);


  Error:

    sprintf (msg, "\
We are unable to take the action %s on job %s <%s>.\nThe error is: %s",
        actCmd, lsb_jobidinstr(jp->jobSpecs.jobId),
        jp->jobSpecs.command, errMsg);

    lsb_mperr(msg);

    exit (-1);
}




char *
exitFileSuffix ( int sigValue)
{
    switch (sigValue) {
        case SIG_CHKPNT:
        case SIG_CHKPNT_COPY:
            return ("chk");
        case SIG_SUSP_USER:
        case SIG_SUSP_LOAD:
        case SIG_SUSP_WINDOW:
        case SIG_SUSP_OTHER:
            return ("suspend");
        case SIG_RESUME_USER:
        case SIG_RESUME_LOAD:
        case SIG_RESUME_WINDOW:
        case SIG_RESUME_OTHER:
            return ("resume");
        case SIG_TERM_USER:
        case SIG_KILL_REQUEUE:
        case SIG_TERM_LOAD:
        case SIG_TERM_WINDOW:
        case SIG_TERM_OTHER:
        case SIG_TERM_RUNLIMIT:
        case SIG_TERM_DEADLINE:
        case SIG_TERM_PROCESSLIMIT:
        case SIG_TERM_CPULIMIT:
        case SIG_TERM_MEMLIMIT:
        case SIG_TERM_FORCE:
            return ("terminate");
        default:
            return ("unknown");
    }
}



static char *
getJobPgids(struct jobCard *jp)
{
    static char pgid[256];
    char pgidStr[10];
    int i;

    pgid[0] = '\0';
    if (jp->runRusage.npgids >0 ) {
        for (i=0; i<jp->runRusage.npgids; i++) {
            sprintf(pgidStr, "%d ",jp->runRusage.pgid[i]);
            strcat(pgid, pgidStr);
        }
    } else {
        sprintf(pgid, "%d", jp->jobSpecs.jobPGid);
    }
    return(pgid);

}


static char *
getJobPids(struct jobCard *jp)
{
    static char pid[512];
    char pidStr[10];
    int i;

    pid[0] = '\0';
    if (jp->runRusage.npids > 0) {
        for (i=0; i<jp->runRusage.npids; i++) {
            sprintf(pidStr, "%d ",jp->runRusage.pidInfo[i].pid);
            strcat(pid, pidStr);
        }
    } else {
        sprintf(pid, "%d", jp->jobSpecs.jobPid);
    }

    return(pid);
}



void
execRestart(struct jobCard *jobCardPtr, struct hostent *hp)
{
    static char fname[] = "execRestart()";
    struct jobSpecs *jspecs = &(jobCardPtr->jobSpecs);
    char *rargv[MAX_ARGS];
    char *erestartPath;
    pid_t pid, erestartPid, erestartPGid;
    int i;
    int fds[2];
    char errMsg[MAXLINELEN];
    char pidStr[20], pgidStr[20];

    char chkDir[MAXFILENAMELEN],
         chkpntDir[MAXFILENAMELEN],
         restartDir[MAXFILENAMELEN],
         oldChkpntDir[MAXFILENAMELEN];
    char oldJobId[20], newJobId[20];
    char *strPtr;



    Signal_(SIGTERM, SIG_IGN);
    Signal_(SIGINT, SIG_IGN);
    Signal_(SIGUSR1, SIG_IGN);
    Signal_(SIGUSR2, SIG_IGN);




    if (((strPtr = strrchr(jspecs->chkpntDir, '/')) != NULL)
         && (islongint_(strPtr+1))) {

        sprintf(chkpntDir, "%s", jspecs->chkpntDir);
        *strPtr = '\0';
        sprintf(chkDir, "%s", jspecs->chkpntDir);

        sprintf(oldJobId, "%s", strPtr+1);
        sprintf(newJobId, "%s", lsb_jobidinstr(jspecs->jobId));
        *strPtr = '/';
    } else {
        sprintf(errMsg, "Error in the chkpnt directory: %s",
                jspecs->chkpntDir);
        sbdSyslog(LOG_ERR, errMsg);
        jobSetupStatus(JOB_STAT_PEND, PEND_JOB_EXEC_INIT, jobCardPtr);
        exit (-1);
    }

    if (mychdir_(chkDir, hp) == 0) {
        strcpy(restartDir, newJobId);
        strcpy(oldChkpntDir, oldJobId);
    } else {
        sprintf(restartDir, "%s/%s", chkDir, newJobId);
        sprintf(oldChkpntDir, "%s/%s", chkDir, oldJobId);
    }


    if (pipe(fds) == -1)
    {
        sprintf(errMsg, "restart_: pipe() failed for job <%s>: %s",
                lsb_jobid2str(jobCardPtr->jobSpecs.jobId), strerror(errno));
        sbdSyslog(LOG_ERR, errMsg);
        jobSetupStatus(JOB_STAT_PEND, PEND_JOB_EXEC_INIT, jobCardPtr);
        goto RestartError;
    }


    if ((pid = fork()) == -1) {
        sprintf(errMsg, I18N_JOB_FAIL_S_M, fname,
            lsb_jobid2str(jobCardPtr->jobSpecs.jobId), "fork");
        sbdSyslog(LOG_ERR, errMsg);
        jobSetupStatus(JOB_STAT_PEND, PEND_SBD_NO_PROCESS, jobCardPtr);
        goto RestartError;
    }

    if (pid > 0) {

        char erestartMsg[MSGSIZE];
        char *sp = erestartMsg;
        int len = sizeof(erestartMsg);
        LS_WAIT_T status;

        close(fds[1]);

        erestartPid = pid;
        erestartPGid = jobCardPtr->jobSpecs.jobPGid;



        errno = 0; sp = erestartMsg; erestartMsg[0] = '\0';

again:
        while (((i = read(fds[0], sp, MSGSIZE)) < 0)
            && (errno == EINTR));

        if ((i  > 0) && (errno == EINTR)) {
            sp += i;
            goto again;
        }
        if (i  > 0)
            sp += i;
        *sp = '\0';


        if ((strlen (erestartMsg) == 0)
           || (rcvPidPGid(erestartMsg, &erestartPid, &erestartPGid) != -1)) {

            jobCardPtr->jobSpecs.jobPGid = erestartPGid;
            jobSetupStatus(JOB_STAT_RUN, 0, jobCardPtr);
        } else {

            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 905,
		"%s: Unable to use (%d) for job <%s>:(%s)"), /* catgets 905 */
		fname,
                jobCardPtr->jobSpecs.restartPid,
		lsb_jobid2str(jobCardPtr->jobSpecs.jobId),
		erestartMsg);
            sbdSyslog(LOG_ERR, errMsg);
            jobSetupStatus(JOB_STAT_PEND, PEND_SBD_GETPID, jobCardPtr);
            goto RestartError;
        }



        sp = erestartMsg;
        *sp = '\0';
        while ((i = read(fds[0], sp, len)) > 0) {
            sp += i;
            len -= i;
        }
        *sp = '\0';

        if (strlen(erestartMsg) > 0 ) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 906,
		"%s: Messages: %s"), /* catgets 906 */
		fname, erestartMsg);
            sbdSyslog(LOG_ERR, errMsg);
        }


        while ((pid = waitpid(pid, &status, 0)) < 0 &&
               errno == EINTR);

        if (pid < 0) {
            sprintf(errMsg, I18N_JOB_FAIL_S_D_M, fname,
		lsb_jobid2str(jobCardPtr->jobSpecs.jobId),
		"waitpid",
                jobCardPtr->jobSpecs.jobPGid);
            sbdSyslog(LOG_ERR, errMsg);
            goto RestartError;
        }

        exit(WEXITSTATUS(status));

RestartError:
        if (rename(restartDir, oldChkpntDir) == -1) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 908,
		"Unable to rename chkpnt directory %s to %s: %s."), /* catgets 908 */
                restartDir, oldChkpntDir, strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
        }
        exit(-1);

    }







    i = 0;
    rargv[i++] = "erestart";

    if (jspecs->chkSig == SIG_CHKPNT_COPY)
    rargv[i++] = "-c";

    if (jspecs->options & SUB_RESTART_FORCE)
    rargv[i++] = "-f";

    rargv[i++] = restartDir;
    rargv[i] = NULL;



    {

	char buf[8];
	sprintf(buf, "%d", dup(2));
	putEnv("LSB_STDERR_FD", buf);
    }
    close(fds[0]);
    dup2(fds[1], 2);
    close(fds[1]);

    sprintf(pidStr, "%d", jspecs->jobPid);
    putEnv("LSB_RESTART_PID",  pidStr);

    sprintf(pgidStr, "%d", jspecs->restartPid);
    putEnv("LSB_RESTART_PGID", pgidStr);

    if ((erestartPath = getEchkpntDir("/erestart")) == NULL) {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 909,
	    "Unable to find the erestart executable")); /* catgets 909 */
        exit(-1);
    }

    lsfExecv(erestartPath, rargv);
    perror("execv(erestart) failed");
    exit(-1);

}



int
rcvPidPGid(char *message, pid_t *Pid, pid_t *PGid)
{
    char pidMsgHeader[MSGSIZE], pgidMsgHeader[MSGSIZE];
    char *pidMsgPtr, *pgidMsgPtr;


    sprintf(pidMsgHeader, "pid=");
    sprintf(pgidMsgHeader, "pgid=");



    if (((pidMsgPtr = strstr(message, pidMsgHeader)) != NULL)
        && (( pgidMsgPtr = strstr(message, pgidMsgHeader)) != NULL)) {
        *Pid = atoi(pidMsgPtr + strlen(pidMsgHeader));
        *PGid = atoi(pgidMsgPtr + strlen(pgidMsgHeader));
        return(0);
    } else
        return(-1);

}


void
exeChkpnt(struct jobCard *jp, int chkFlags, char * exitFile)
{
    static char fname[]="exeChkpnt";
    char s[MAXFILENAMELEN], t[MAXFILENAMELEN];


   char chkDir[MAXFILENAMELEN],
         chkpntDir[MAXFILENAMELEN],
         chkpntDirTmp[MAXFILENAMELEN];
    char oldJobId[20], newJobId[20];
    char *strPtr, *p;

    DIR *dirp;
    struct dirent *dp;


    pid_t pid1, echkpntPid;
    char errMsg[MAXLINELEN], msg[MAXLINELEN*2];
    LS_WAIT_T status;
    struct hostent *hp;
    FILE *fp;
    int  argc;
    char *argv[MAX_ARGS];
    char *echkpntPath;
    int fds[2];
    int i;
    char jobPGidStr[20];
    char *sp = errMsg;
    int len = sizeof(errMsg);

    if (logclass & LC_EXEC) {
	sprintf(msg, "%s: Entering the routin ..",fname);
	sbdSyslog(LOG_DEBUG, msg);
    }







    if (jp->regOpFlag&REG_CHKPNT) {

	struct lsbMsgHdr pmMsgHdr;
	struct lsbMsg pmMsg;

	int msgStat;

	pmMsgHdr.usrId = jp->jobSpecs.userId;
	pmMsgHdr.msgId = 0;
	pmMsgHdr.jobId = jp->jobSpecs.jobId;
	pmMsgHdr.src = "sbatchd";
	pmMsgHdr.dest = "pm";

	if (chkFlags & LSB_CHKPNT_KILL) {
	    pmMsg.msg = "CHKPNT KILL";
	} else {
	    pmMsg.msg = "CHKPNT";
	}

	pmMsg.header = &pmMsgHdr;

	if ( (msgStat = msgSupervisor(&pmMsg, jp->client)) < 0) {
	    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5918,
		"%s: sending signal message to pm failed for job <%s>: %m"), /* catgets 5918 */
	        fname, lsb_jobid2str(jp->jobSpecs.jobId));
	    return;
	}

        suspendUntilSignal(SIGURG);
    }
    else {

    if (!(jp->jobSpecs.options & SUB_CHKPNTABLE)) {
	if (jobsig(jp, SIGKILL, FALSE) == 0) {


	    if ((fp = fopen(exitFile, "w")) == NULL || FCLOSEUP(&fp) == -1) {
		sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 910,
		    "Unable to write migration status into %s file: %s"), /* catgets 910 */
		    exitFile, strerror(errno));
		goto Error;
	    }
	    exit(0);
	}

	sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 911,
	    "Unable to send signal to job: %s"), /* catgets 911 */
	    strerror(errno));
	goto Error;
    }


    jobsig(jp, SIGCONT, FALSE);

    if ((hp = Gethostbyname_(jp->jobSpecs.fromHost)) == NULL) {
        sprintf(errMsg, "\
%s: gethostbyname() %s failed job %s", __func__,
                lsb_jobid2str(jp->jobSpecs.jobId),
                jp->jobSpecs.fromHost);
        goto Error;
    }



    if (((strPtr = strrchr(jp->jobSpecs.chkpntDir, '/')) != NULL)
         && (islongint_(strPtr+1))) {

        sprintf(chkpntDir, "%s", jp->jobSpecs.chkpntDir);
        *strPtr = '\0';
        sprintf(chkDir, "%s", jp->jobSpecs.chkpntDir);

        sprintf(oldJobId, "%s", strPtr+1);


            sprintf(newJobId, "%s", lsb_jobidinstr(jp->jobSpecs.jobId));
        *strPtr = '/';
    } else {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 913,
	    "%s, Error in the chkpnt directory: %s"), /* catgets 913 */
	    fname,
            jp->jobSpecs.chkpntDir);
        goto Error;
    }

    if (mychdir_(chkDir, hp) < 0) {
        sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "mychdir_", chkDir);
        goto Error;
    }

    sprintf(chkpntDir, "%s/%s", chkDir, newJobId);
    sprintf(chkpntDirTmp, "%s.tmp", newJobId);

    if (mymkdir_(chkpntDirTmp, 0700, hp) == -1 && errno != EEXIST) {
	sprintf(errMsg, "Unable to create directory %s: %s",
		chkpntDirTmp, strerror(errno));
	goto Error;
    }
    if (jp->jobSpecs.chkpntDir[0] == '/' &&
        (p = strrchr(jp->jobSpecs.chkpntDir, '/')) &&
        p != &(jp->jobSpecs.chkpntDir[0])) {
        *p = '/';

        if (mymkdir_(chkpntDirTmp, 0700, hp) < 0 && errno != EEXIST) {
            sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "mymkdir_",
		chkpntDirTmp);
            *p = '/';
            goto Error;
        }
    } else {
        if (mymkdir_(chkpntDirTmp, 0700, hp) < 0 && errno != EEXIST) {
            sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "mymkdir_",
		chkpntDirTmp);
            goto Error;
        }
    }



    argc = 0;
    argv[argc++] = "echkpnt";

    if (chkFlags & LSB_CHKPNT_COPY)
        argv[argc++] = "-c";

    if (chkFlags & LSB_CHKPNT_FORCE)
        argv[argc++] = "-f";

    if (chkFlags & LSB_CHKPNT_KILL)
        argv[argc++] = "-k";

    if (chkFlags & LSB_CHKPNT_STOP)
        argv[argc++] = "-s";

    argv[argc++] = "-d";
    argv[argc++] = chkpntDirTmp;

    sprintf(jobPGidStr, "%d", jp->jobSpecs.jobPGid);
    argv[argc++] = jobPGidStr;

    argv[argc] = NULL;



    if ((echkpntPath = getEchkpntDir("/echkpnt")) == NULL) {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 917,
	    "%s: Unable to find the echkpnt executable"), /* catgets 917 */
	    fname);
        goto ChildError;
    }

    if (pipe(fds) == -1)
    {
        sprintf(errMsg, I18N_JOB_FAIL_S_M, fname,
		lsb_jobid2str(jp->jobSpecs.jobId), "pipe");
        goto ChildError;
    }

    if ((pid1 = fork()) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
	    lsb_jobid2str(jp->jobSpecs.jobId), "fork");
        goto ChildError;
    } else if (pid1 == 0) {
        close(fds[0]);
        dup2(fds[1], 2);


	chuser(batchId);

        if (lsfSetUid(jp->jobSpecs.execUid) < 0) {
            fprintf(stderr, I18N_JOB_FAIL_S_D_M, fname,
		lsb_jobid2str(jp->jobSpecs.jobId),
		"setuid",
		jp->jobSpecs.execUid);
            fflush(stderr);
            fclose(stderr);
            exit(-1);
        }

        lsfExecv(echkpntPath,argv);
        perror( _i18n_printf( I18N_FUNC_S_FAIL_M, fname,
	    "execv", "echkpnt"));
        exit(-1);
    }


    close(fds[1]);
    echkpntPid = pid1;


    while ((i = read(fds[0], sp, len)) > 0) {
        sp += i;
        len -= i;
    }
    *sp = '\0';

    if ((strlen(errMsg) > 0) && (strstr(errMsg, "Checkpoint done") == NULL)) {
       goto ChildError;
    }
    while ((pid1 = waitpid(echkpntPid, &status, 0)) < 0 && errno == EINTR);

    if (pid1 < 0) {
        sprintf(errMsg, I18N_JOB_FAIL_S_D_M, fname,
	    lsb_jobid2str(jp->jobSpecs.jobId), "waitpid", (int)pid1);
        goto ChildError;
    } else {
        if (WIFEXITED(status)) {
	    if ( WEXITSTATUS(status) != 0 ) {
                sprintf(msg, _i18n_msg_get(ls_catd , NL_SETN, 922,
		        "%s: job <%s> exited with the exit code %d with <%s>"), /* catgets 922 */
		        fname, lsb_jobid2str(jp->jobSpecs.jobId),
                        WEXITSTATUS(status), errMsg);
	        sprintf(errMsg, msg);
                goto ChildError;
	    }
        }
    }


    dirp = opendir(chkpntDirTmp);
    while ((dp = readdir(dirp)) != NULL) {

        if (strcmp(dp->d_name, ".") == 0 ||
            strcmp(dp->d_name, "..") == 0)
            continue;

        sprintf(s, "%s/%s", chkpntDirTmp, dp->d_name);
        sprintf(t, "%s/%s", newJobId, dp->d_name);

        if (rename(s, t) == -1) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 923,
		"%s: Unable to rename checkpoint file %s to %s: %s"), /* catgets 923 */
                fname, s, t, strerror(errno));
            (void) closedir(dirp);
            goto Error;
        }
    }

    (void) closedir(dirp);

    if (strcmp(oldJobId, newJobId) != 0)
        unlink(oldJobId);

    rmDir(chkpntDirTmp);

    }




    sprintf (s, "%s/chklog", newJobId);


    if (writeChkLog(s, chkDir, jp, hp) == -1) {
	sprintf(errMsg, "Unable to write to checkpoint log %s/%s: %s",
		chkDir, s, lsb_sysmsg());
	goto Error;
    }


    chuser(batchId);

    if ((fp = fopen(exitFile, "w")) == NULL || FCLOSEUP(&fp) == -1) {
	sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "fopen", exitFile);
	goto Error;
    }
    exit(0);

  ChildError:

    rmDir(chkpntDirTmp);

  Error:

    sprintf (msg, "\
We are unable to checkpoint your job %s <%s>.\nThe error is: %s",
             lsb_jobid2str(jp->jobSpecs.jobId),
             jp->jobSpecs.command, errMsg);


     if (jp->jobSpecs.options & SUB_MAIL_USER)
         merr_user (jp->jobSpecs.mailUser, jp->jobSpecs.fromHost, msg,
	     I18N_error);
     else
	 merr_user (jp->jobSpecs.userName, jp->jobSpecs.fromHost, msg,
	     I18N_error);

    if (jp->jobSpecs.jStatus & (JOB_STAT_SSUSP | JOB_STAT_USUSP))
	jobsig(jp, SIGSTOP, FALSE);

    exit (-1);
 /* WIN32  */

}



void
sig_wakeup(int signo)
{
return;
}

void
suspendUntilSignal(int signo)
{
    sigset_t sigMask;

    Signal_(signo, (SIGFUNCTYPE) sig_wakeup);

    sigfillset(&sigMask);
    sigdelset(&sigMask, signo);
    sigsuspend(&sigMask);

}



char *
getEchkpntDir(char *name)
{
    static char echpnt_path[MAXPATHLEN];
    char *echkpnt_dir;

    if ((echkpnt_dir = getenv("LSF_ECHKPNTDIR")) == NULL)
       if ((echkpnt_dir = daemonParams[LSF_SERVERDIR].paramValue) == NULL) {
          perror(_i18n_msg_get(ls_catd , NL_SETN, 927,
	      "Can't access echkpnt()")); /* catgets 927 */
          return(NULL);
       }


    strcpy(echpnt_path, echkpnt_dir);
    strcat(echpnt_path, name);


    if (access(echpnt_path, X_OK) < 0) {
       perror(_i18n_msg_get(ls_catd , NL_SETN, 928,
	   "Can't access echkpnt()/erestart()")); /* catgets 928 */
       return(NULL);
    }
    return(echpnt_path);

}


static int
writeChkLog(char *fn, char *chkpntDir, struct jobCard *jp, struct hostent *fromHp)
{
    struct jobNewLog *jobNewLog;
    struct jobStartLog *jobStartLog;
    struct eventRec logPtr;
    int i, cc, eno;
    FILE *fp;

    lsberrno = LSBE_SYS_CALL;

    if ((fp = myfopen_(fn, "w", fromHp)) == NULL)
	return (-1);

    jobNewLog = &logPtr.eventLog.jobNewLog;
    logPtr.type =  EVENT_JOB_NEW;
    sprintf(logPtr.version, "%d", VOLCLAVA_VERSION);
    jobNewLog->jobId = LSB_ARRAY_JOBID(jp->jobSpecs.jobId);
    jobNewLog->idx = 0;
    jobNewLog->options = jp->jobSpecs.options;
    jobNewLog->options2 = jp->jobSpecs.options2;
    jobNewLog->numProcessors = jp->jobSpecs.numToHosts;
    jobNewLog->submitTime = 0;
    jobNewLog->beginTime = 0;
    jobNewLog->termTime = 0;

    jobNewLog->userId = jp->jobSpecs.userId;
    strcpy(jobNewLog->userName, jp->jobSpecs.userName);

    jobNewLog->sigValue = jp->jobSpecs.sigValue;
    jobNewLog->chkpntPeriod = jp->jobSpecs.chkPeriod;

    jobNewLog->restartPid = jp->jobSpecs.jobPGid;

    for(i = 0; i < LSF_RLIM_NLIMITS; i++)
	jobNewLog->rLimits[i] = -1;

    strcpy(jobNewLog->hostSpec, "");
    jobNewLog->hostFactor = 1.0;

    jobNewLog->umask = jp->jobSpecs.umask;
    strcpy(jobNewLog->queue, jp->jobSpecs.queue);
    jobNewLog->resReq = safeSave(jp->jobSpecs.resReq);

    strcpy(jobNewLog->fromHost, jp->jobSpecs.fromHost);
    strcpy(jobNewLog->cwd, jp->jobSpecs.cwd);

    strcpy(jobNewLog->chkpntDir, chkpntDir);
    strcpy(jobNewLog->inFile, jp->jobSpecs.inFile);
    strcpy(jobNewLog->outFile, jp->jobSpecs.outFile);
    strcpy(jobNewLog->errFile, jp->jobSpecs.errFile);
    strcpy(jobNewLog->subHomeDir, jp->jobSpecs.subHomeDir);

    strcpy(jobNewLog->jobFile, strrchr(jp->jobSpecs.jobFile, '/') + 1);

    jobNewLog->mailUser = jp->jobSpecs.mailUser;

    jobNewLog->numAskedHosts = 0;

    strcpy(jobNewLog->jobName, jp->jobSpecs.jobName);
    strcpy(jobNewLog->command, jp->jobSpecs.command);
    jobNewLog->dependCond = "";
    jobNewLog->preExecCmd = "";
    jobNewLog->postExecCmd = "";

    jobNewLog->projectName = jp->jobSpecs.projectName;

    jobNewLog->nxf = jp->jobSpecs.nxf;
    if (jobNewLog->nxf > 0) {
	jobNewLog->xf = (struct xFile *) my_calloc(jp->jobSpecs.nxf,
				       sizeof(struct xFile), "writeChkLog");
	memcpy((char *) jobNewLog->xf, (char *) jp->jobSpecs.xf,
		   sizeof(struct xFile) * jobNewLog->nxf);
    }
    jobNewLog->loginShell = jp->jobSpecs.loginShell;
    jobNewLog->maxNumProcessors = jp->jobSpecs.maxNumProcessors;
    jobNewLog->schedHostType = safeSave (jp->jobSpecs.schedHostType);;

    if ((jobNewLog->options2 & SUB2_IN_FILE_SPOOL) == SUB2_IN_FILE_SPOOL) {
	strcpy(jobNewLog->inFileSpool, jp->jobSpecs.inFileSpool);
	strcpy(jobNewLog->inFile, jp->jobSpecs.inFile);
    } else {
        strcpy(jobNewLog->inFileSpool, "");
    }

    if ((jobNewLog->options2 & SUB2_JOB_CMD_SPOOL) == SUB2_JOB_CMD_SPOOL)
        strcpy(jobNewLog->commandSpool, jp->jobSpecs.commandSpool);
    else
	strcpy(jobNewLog->commandSpool, "");

    if (jp->jobSpecs.jobSpoolDir != NULL) {
        strcpy(jobNewLog->jobSpoolDir, jp->jobSpecs.jobSpoolDir);
    }
    jobNewLog->userPriority = jp->jobSpecs.userPriority;

    now = time(0);
    logPtr.eventTime = now;
    cc = lsb_puteventrec(fp, &logPtr);
    eno = errno;

    if (jobNewLog->nxf > 0)
	free(jobNewLog->xf);
    free(jobNewLog->resReq);

    if (cc == -1) {
	FCLOSEUP(&fp);
	errno = eno;
	return (-1);
    }

    jobStartLog = &logPtr.eventLog.jobStartLog;

    logPtr.type = EVENT_JOB_START;
    sprintf(logPtr.version, "%d", VOLCLAVA_VERSION);
    jobStartLog->jobId = jp->jobSpecs.jobId;
    jobStartLog->jStatus = 0;
    jobStartLog->numExHosts = jp->jobSpecs.numToHosts;
    jobStartLog->execHosts = jp->jobSpecs.toHosts;
    jobStartLog->jobPid = jp->jobSpecs.jobPid;
    jobStartLog->jobPGid = jp->jobSpecs.jobPGid;

    jobStartLog->hostFactor = 1.0;
    logPtr.eventTime = jp->jobSpecs.startTime;
    jobStartLog->queuePreCmd = jp->jobSpecs.preCmd;
    jobStartLog->queuePostCmd = jp->jobSpecs.postCmd;

    if (lsb_puteventrec(fp, &logPtr) == -1) {
	eno = errno;
	FCLOSEUP(&fp);
	errno = eno;
	return (-1);
    }

    return (FCLOSEUP(&fp));

}


int
sbdlog_newstatus (struct jobCard *jp)
{
    static char fname[] = "sbdlog_newstatus()";
    char logFile[MAXFILENAMELEN];
    struct eventRec logPtr;
    int  eno;
    struct sbdJobStatusLog    *jobStatusLog;
    struct jobSpecs *job = &(jp->jobSpecs);
    FILE *fp;

    lsberrno = LSBE_SYS_CALL;


    sprintf(logFile, "%s/.%s.sbd", LSTMPDIR, clusterName);
    if (mkdir(logFile, 0700) == -1 && errno != EEXIST) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M, fname,
	    lsb_jobid2str(jp->jobSpecs.jobId), "mkdir", logFile);
        return (-1);
    }
    sprintf(logFile, "%s/.%s.sbd/jobstatus.%s",LSTMPDIR, clusterName, lsb_jobidinstr(jp->jobSpecs.jobId));

    lsberrno = LSBE_SYS_CALL;

    if ((fp = fopen(logFile, "w")) == NULL)
        return (-1);

    jobStatusLog = &logPtr.eventLog.sbdJobStatusLog;
    sprintf(logPtr.version, "%d", VOLCLAVA_VERSION);
    logPtr.type = EVENT_SBD_JOB_STATUS;
    logPtr.eventTime = now;
    jobStatusLog->jobId = LSB_ARRAY_JOBID(job->jobId);
    jobStatusLog->idx = LSB_ARRAY_IDX(job->jobId);
    jobStatusLog->jStatus = job->jStatus;
    jobStatusLog->reasons = job->reasons & (~SUSP_SBD_STARTUP);
    jobStatusLog->subreasons = job->subreasons;
    jobStatusLog->actPid = job->actPid;
    jobStatusLog->actValue = job->actValue;
    jobStatusLog->actPeriod = job->chkPeriod;
    jobStatusLog->actFlags = jp->actFlags;
    jobStatusLog->actStatus = jp->actStatus;
    jobStatusLog->actReasons = jp->actReasons;
    jobStatusLog->actSubReasons = jp->actSubReasons;


    if (lsb_puteventrec(fp, &logPtr) == -1) {
        eno = errno;
        FCLOSEUP(&fp);
        errno = eno;
        return (-1);
    }

    return (FCLOSEUP(&fp));
}


int
sbdread_jobstatus (struct jobCard *jp)
{
    char logFile[MAXFILENAMELEN];
    struct eventRec *logPtr;

    int  line = INFINIT_INT;
    struct sbdJobStatusLog    *jobStatusLog;
    struct jobSpecs *job = &(jp->jobSpecs);
    FILE *fp;

    sprintf(logFile, "%s/.%s.sbd/jobstatus.%s",LSTMPDIR, clusterName, lsb_jobidinstr(jp->jobSpecs.jobId));

    lsberrno = LSBE_SYS_CALL;

    if ((fp = fopen(logFile, "r")) == NULL) {

	int jobid32, jobid, idx;
	jobid = LSB_ARRAY_JOBID(jp->jobSpecs.jobId);
	idx = LSB_ARRAY_IDX(jp->jobSpecs.jobId);
	jobid32 = (unsigned int)idx << 20 | jobid;
	sprintf(logFile, "%s/.%s.sbd/jobstatus.%d",LSTMPDIR, clusterName, jobid32);
	if((fp = fopen(logFile, "r")) == NULL) {
	    return(-1);
	}
    }
    if ((logPtr = lsb_geteventrec(fp, &line)) == NULL
            || logPtr->type != EVENT_SBD_JOB_STATUS) {

        FCLOSEUP(&fp);
        return (-1);
    }
    jobStatusLog = &logPtr->eventLog.sbdJobStatusLog;

    job->jobId = LSB_JOBID(jobStatusLog->jobId,
			   jobStatusLog->idx);
    job->jStatus = jobStatusLog->jStatus;
    job->reasons = jobStatusLog->reasons;
    job->subreasons = jobStatusLog->subreasons;
    job->actPid = jobStatusLog->actPid;
    job->actValue = jobStatusLog->actValue;
    job->chkPeriod = jobStatusLog->actPeriod;
    jp->actFlags = jobStatusLog->actFlags;
    jp->actStatus = jobStatusLog->actStatus;
    jp->actReasons = jobStatusLog->actReasons;
    jp->actSubReasons = jobStatusLog->actSubReasons;

    FCLOSEUP(&fp);
    return (0);
}



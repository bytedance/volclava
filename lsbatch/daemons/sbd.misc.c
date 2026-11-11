/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: sbd.misc.c 397 2007-11-26 19:04:00Z mblack $
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
#include "../../lsf/lib/lib.channel.h"
#define NL_SETN		11	

extern short mbdExitVal;
extern int mbdExitCnt;
extern int mbdPid;

int sigchldPipeRd = -1;
int sigchldPipeWr = -1;
int sigchldPipeChfd = -1;

struct hTab pidHT;

#define PID_KEY_LEN 32

static char *
pidKey(pid_t pid, char *buf)
{
    sprintf(buf, "%d", (int)pid);
    return buf;
}

void
initPidHT(void)
{
    h_initTab_(&pidHT, 101);
}

void
addPidHT(pid_t pid, struct jobCard *jp)
{
    char key[PID_KEY_LEN];
    hEnt *ent;
    int new;

    if (pid <= 0)
        return;

    ent = h_addEnt_(&pidHT, pidKey(pid, key), &new);
    if (ent == NULL)
        return;
    ent->hData = (void *)jp;
}

void
removePidHT(pid_t pid)
{
    char key[PID_KEY_LEN];
    hEnt *ent;

    if (pid <= 0)
        return;

    ent = h_getEnt_(&pidHT, pidKey(pid, key));
    if (ent != NULL)
        h_rmEnt_(&pidHT, ent);
}

struct jobCard *
getJobCardByPid(pid_t pid)
{
    char key[PID_KEY_LEN];
    hEnt *ent;

    if (pid <= 0)
        return NULL;

    ent = h_getEnt_(&pidHT, pidKey(pid, key));
    if (ent != NULL)
        return (struct jobCard *)ent->hData;
    return NULL;
}

int
initSigchldPipe(void)
{
    int fds[2];
    int flags;

    if (sigchldPipeRd >= 0)
        return 0;

    if (pipe(fds) < 0) {
        ls_syslog(LOG_ERR, "initSigchldPipe: pipe failed: %m");
        return -1;
    }

    flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(fds[1], F_GETFL, 0);
    if (flags >= 0)
        fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

    sigchldPipeRd = fds[0];
    sigchldPipeWr = fds[1];

    sigchldPipeChfd = chanRegisterFd_(sigchldPipeRd, EPOLLIN | EPOLLERR);
    if (sigchldPipeChfd < 0) {
        ls_syslog(LOG_ERR, "initSigchldPipe: chanRegisterFd_ failed");
        close(sigchldPipeRd);
        close(sigchldPipeWr);
        sigchldPipeRd = -1;
        sigchldPipeWr = -1;
        return -1;
    }

    return 0;
}

#define NL_SETN         11  
void
milliSleep( int msec )
{
    struct timeval dtime;
    
    if (msec < 1)
	return;
    dtime.tv_sec = msec/1000;
    dtime.tv_usec = (msec - dtime.tv_sec * 1000) * 1000;

    select( 0,0,0,0, &dtime );

}



char 
window_ok (struct jobCard *jobPtr)
{
    windows_t *wp;
    struct dayhour dayhour;
    char active;
    time_t ckTime;
    time_t now;

    now = time(0);
    active = jobPtr->active;                   

    if (active && (jobPtr->jobSpecs.options & SUB_WINDOW_SIG))
        ckTime = now + WARN_TIME;            
    else
        ckTime = now;

    if (jobPtr->windEdge > ckTime || jobPtr->windEdge == 0)
        return (jobPtr->active);

    getDayHour (&dayhour, ckTime);          
    if (jobPtr->week[dayhour.day] == NULL) {              
        jobPtr->active = TRUE;
        jobPtr->windEdge = now + (24.0 - dayhour.hour) * 3600.0;
        return (jobPtr->active);
    }
   
    jobPtr->active = FALSE;
    jobPtr->windEdge = now + (24.0 - dayhour.hour) * 3600.0;
    for (wp = jobPtr->week[dayhour.day]; wp; wp=wp->nextwind) 
        checkWindow(&dayhour, &jobPtr->active, &jobPtr->windEdge, wp, now);

    if (active && !jobPtr->active && now - jobPtr->windWarnTime >= WARN_TIME
        && (jobPtr->jobSpecs.options & SUB_WINDOW_SIG)) {
        
        if (!(jobPtr->jobSpecs.jStatus & JOB_STAT_RUN)) 
	    job_resume(jobPtr);            
	jobsig (jobPtr, sig_decode (jobPtr->jobSpecs.sigValue), TRUE);
	jobPtr->windWarnTime = now;
    }
    
    return (jobPtr->active);

} 
void
shout_err (struct jobCard *jobPtr, char *msg)
{
     char buf[MSGSIZE];

     sprintf(buf, _i18n_msg_get(ls_catd, NL_SETN, 600, 
         "We are unable to run your job %s:<%s>. The error is:\n%s."), /* catgets 600 */
	 lsb_jobid2str(jobPtr->jobSpecs.jobId),
	 jobPtr->jobSpecs.command, msg);

     
     if (jobPtr->jobSpecs.options & SUB_MAIL_USER) 
         merr_user (jobPtr->jobSpecs.mailUser, jobPtr->jobSpecs.fromHost,
         	buf, I18N_error);
     else
         merr_user (jobPtr->jobSpecs.userName, jobPtr->jobSpecs.fromHost,
		buf, I18N_error);
     
} 


/*
 * Async-signal-safe SIGCHLD handler.
 * It does not call wait*(), walk job lists, or access shared job state.
 * Instead it writes a single byte to a self-pipe to wake the main loop,
 * which will reap children and update job state in process-safe context.
 */
void
child_handler (int sig)
{
    int saved_errno = errno;
    char byte = 1;

    if (sigchldPipeWr >= 0) {
        while (write(sigchldPipeWr, &byte, 1) < 0 && errno == EINTR)
            ;
    }

    errno = saved_errno;
}

static void
processReapedMbd(int pid, LS_WAIT_T status)
{
    static short lastMbdExitVal = MASTER_NULL;
    int sig;

    if (pid != mbdPid)
        return;

    sig = WTERMSIG(status);
    if (mbdExitCnt > 150)
        mbdExitCnt = 150;

    mbdExitVal = WIFSIGNALED(status);
    if (mbdExitVal) {
        if (WCOREDUMP(status))
            mbdExitVal = sig;
        if (mbdExitVal == lastMbdExitVal)
            mbdExitCnt++;
        else {
            mbdExitCnt = 0;
            lastMbdExitVal = mbdExitVal;
        }
    } else {
        mbdExitVal = WEXITSTATUS(status);
        if (mbdExitVal == lastMbdExitVal)
            mbdExitCnt++;
        else {
            mbdExitCnt = 0;
            lastMbdExitVal = mbdExitVal;
        }
        if (mbdExitVal == MASTER_RECONFIG) {
            start_master();
        }
    }
}

static void
processReapedJob(int pid, LS_WAIT_T status, struct rusage *rusage)
{
    struct jobCard *jobCard;
    struct lsfRusage lsfRusage;
    float cpuTime;

    jobCard = getJobCardByPid(pid);
    if (jobCard == NULL)
        return;

    ls_ruunix2lsf(rusage, &lsfRusage);
    cpuTime = lsfRusage.ru_utime + lsfRusage.ru_stime;

    if (jobCard->jobSpecs.jobPid == pid) {
        jobCard->collectedChild = TRUE;
        jobCard->cpuTime = cpuTime;
        jobCard->w_status = LS_STATUS(status);
        memcpy((char *) &jobCard->lsfRusage, (char *) &lsfRusage,
               sizeof(struct lsfRusage));
        jobCard->notReported++;
        need_checkfinish = TRUE;
        removePidHT(pid);
    }
}

/*
 * Called from the main loop when the self-pipe is readable.
 * Drain the pipe and reap all available children, then update job state.
 */
void
processReapedChildren(void)
{
    LS_WAIT_T status;
    struct rusage rusage;
    pid_t pid;
    char buf[256];
    ssize_t n;

    if (sigchldPipeRd < 0)
        return;

    do {
        n = read(sigchldPipeRd, buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);

    cleanRusage(&rusage);

    for (;;) {
        pid = wait3(&status, WNOHANG, &rusage);
        if (pid == 0)
            break;
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pid == mbdPid) {
            processReapedMbd(pid, status);
        } else {
            processReapedJob(pid, status, &rusage);
        }
        cleanRusage(&rusage);
    }
}

#ifndef BSIZE
#define BSIZE 1024
#endif

int
fcp(char *file1, char *file2, struct hostent *hp)
{
    static char fname[] = "fcp()";
    struct stat sbuf;
    int fd1, fd2;
    char buf[BSIZE];
    int cc;

    fd1 = myopen_(file1, O_RDONLY, 0, hp);
    if (fd1 < 0)
	return -1;

    if (fstat(fd1, &sbuf) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fstat", file1);
	close(fd1);
	return -1;
    }

    fd2 = myopen_(file2, O_CREAT | O_TRUNC | O_WRONLY, (int) sbuf.st_mode, hp);
    if (fd2 < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "creat", file1);
	close(fd1);
	return -1;
    }

    for (;;) {
	cc = read(fd1, buf, BSIZE);
	if (cc == 0)
	    break;
	if (cc < 0) {
	    close(fd1);
	    close(fd2);
	    return -1;
	}
	if (write(fd2, buf, cc) != cc) {
	    close(fd1);
	    close(fd2);
	    return -1;
	}
    }

    close(fd1);
    close(fd2);
    return (0);
} 

#include <sys/dir.h>

int
rmDir(char *dir)
{
    DIR *dirp;
    struct direct *dp;
    char path[MAXPATHLEN];

    if ((dirp = opendir(dir)) == NULL)
	return -1;

    readdir(dirp); readdir(dirp);

    for (dp = readdir(dirp); dp != NULL; dp = readdir(dirp)) {
	sprintf (path, "%s/%s", dir, dp->d_name);
	rmdir (path);
        unlink (path);
    }

    closedir (dirp);
    return (rmdir(dir));
} 


void closeBatchSocket (void) 
{
    chanCloseEpoll();
    if (batchSock > 0) { 
        chanClose_(batchSock);
        batchSock = -1;
    } 
} 

void
getManagerId(struct sbdPackage *sbdPackage)
{
    static char fname[]="getManagerId";
    struct passwd   *pw;
    int i;

    FREEUP(lsbManager);

    if (sbdPackage->nAdmins <= 0) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5609,
	    "%s: No LSF administrator defined in sbdPackage"),
	    fname);  /* catgets 5609 */
	die(FATAL_ERR);
    }

    for (i = 0; i < sbdPackage->nAdmins; i++) {
	if ((pw = getpwlsfuser_(sbdPackage->admins[i])) != NULL) {
	    lsbManager = safeSave(sbdPackage->admins[i]);
	    managerId  = pw->pw_uid;
	    break;
	}
    }

    if (lsbManager == NULL) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5609,
	    "%s: No LSF administrator defined in sbdPackage"),
	    fname);  /* catgets 5609 */
	die(FATAL_ERR);
    }
} 

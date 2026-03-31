/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.reason.c 397 2007-11-26 19:04:00Z mblack $
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
#include <pwd.h>
#include "lsb.h"

#define   NL_SETN     13   

static char msgbuf[MSGSIZE];      
char msgline[MAXLINELEN];
struct msgMap {
    int  number;
    char *message;
};

static void userIndexReasons(char *, int, int, struct loadIndexLog *);
static char *getMsg (struct msgMap *msgMap, int *msgId, int number);
static void getMsgByRes(int, int, char **, struct loadIndexLog *);

static char *
getMsg (struct msgMap *msgMap, int *msg_ID,  int number)
{
    int i;

    for (i = 0; msgMap[i].message != NULL; i++)
        if (msgMap[i].number == number)
            return (_i18n_msg_get(ls_catd , NL_SETN, msg_ID[i], msgMap[i].message));

    return ("");
} 

char *
lsb_suspreason (int reasons, int subreasons, struct loadIndexLog *ld)
{
    static char fname[] = "lsb_suspreason";
    msgbuf[0] = '\0';
    
    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: reasons=%x, subreasons=%d",
                              fname, reasons, subreasons);

    
    if (reasons & SUSP_USER_STOP)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 500, 
		" The job was suspended by user;\n")); /* catgets 500 */
    else if (reasons & SUSP_ADMIN_STOP)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 501, 
		" The job was suspended by LSF admin or root;\n")); /* catgets 501 */
    else if (reasons & SUSP_QUEUE_WINDOW)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 502, 
		" The run windows of the queue are closed;\n"));  /* catgets 502 */
    else if (reasons & SUSP_HOST_LOCK)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 506, 
	     " The execution host is locked by LSF administrator now;\n"));  /* catgets 506 */
    else if (reasons & SUSP_HOST_LOCK_MASTER) {
	sprintf(msgbuf, I18N( 531, 
	     " The execution host is locked by master LIM now;\n"));  /* catgets 531 */
    } else if (reasons & SUSP_USER_RESUME)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 507, 
	     " Waiting for re-scheduling after being resumed by user;\n")); /* catgets 507 */
    else if (reasons & SUSP_QUE_STOP_COND)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 508, 
		" STOP_COND is true with current host load;\n")); /* catgets 508 */
    else if (reasons & SUSP_QUE_RESUME_COND)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 509, 
	       " RESUME_COND is false with current host load;\n")); /* catgets 509 */
    else if (reasons & SUSP_RES_RESERVE)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 510, 
	       " Job's requirements for resource reservation not satisfied;\n")); /* catgets 510 */  
    else if (reasons & SUSP_PG_IT)
	sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 511, 
	       " Job was suspended due to paging rate (pg) and the host is not idle yet\n")); /* catgets 511 */

    else if (reasons & SUSP_LOAD_UNAVAIL)
       sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 512, 
	  " Load information on execution host(s) is unavailable\n")); /* catgets 512 */
    else if (reasons & SUSP_LOAD_REASON) {
	strcpy (msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 513, 
		" Host load exceeded threshold: "));  /* catgets 513 */
	if (subreasons == R15S)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 514, 
	       "%s 15-second CPU run queue length (r15s)\n"), msgbuf); /* catgets 514 */
	else if (subreasons == R1M)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 515, 
		"%s 1-minute CPU run queue length (r1m)\n"), msgbuf); /* catgets 515 */
	else if (subreasons == R15M)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 516, 
		"%s 15-minute CPU run queue length (r15m)\n"), msgbuf); /* catgets 516 */
	else if (subreasons == UT)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 517, 
		"%s 1-minute CPU utilization (ut)\n"), msgbuf); /* catgets 517 */
	else if (subreasons == IO)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 518, 
		   "%s Disk IO rate (io)\n"), msgbuf); /* catgets 518 */
	else if (subreasons == PG)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 519, 
		"%s Paging rate (pg)\n"), msgbuf); /* catgets 519 */
	else if (subreasons == IT)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 520, 
		"%s Idle time (it)\n"), msgbuf);  /* catgets 520 */
	else if (subreasons == MEM)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 521, 
		"%s Available memory (mem)\n"), msgbuf);  /* catgets 521 */
	else if (subreasons == SWP)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 522, 
		"%s Available swap space (swp)\n"), msgbuf); /* catgets 522 */
	else if (subreasons == TMP)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 523, 
		"%s Available /tmp space (tmp)\n"), msgbuf); /* catgets 523 */
	else if (subreasons == LS)
	    sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 524, 
		"%s Number of login sessions (ls)\n"), msgbuf); /* catgets 524 */
        else {
            userIndexReasons(msgline, 0, subreasons, ld);
            strcat (msgbuf, "  ");
            strcat (msgbuf, msgline);
	    strcat (msgbuf, "\n");
        }
    } else if (reasons & SUSP_RES_LIMIT) {
        if (subreasons & SUB_REASON_RUNLIMIT)
            sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 525, 
		" RUNLIMIT was reached;\n"));  /* catgets 525 */
        else if (subreasons & SUB_REASON_DEADLINE)
            sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 526, 
		" DEADLINE was reached;\n"));   /* catgets 526 */
        else if (subreasons & SUB_REASON_PROCESSLIMIT)
            sprintf(msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 527, 
		" PROCESSLIMIT was reached;\n"));  /* catgets 527 */
       else if (subreasons & SUB_REASON_CPULIMIT)
            sprintf(msgbuf, I18N(529,
                " CPULIMIT was reached;\n"));  /* catgets 529 */
       else if (subreasons & SUB_REASON_MEMLIMIT)
            sprintf(msgbuf, I18N(530, 
		" MEMLIMIT was reached;\n"));  /* catgets 530 */
    } else
	sprintf (msgbuf, _i18n_msg_get(ls_catd , NL_SETN, 528, 
		" Unknown suspending reason code: %d\n"), reasons); /* catgets 528 */

    return msgbuf;

} 

char *
lsb_pendreason(int numReasons, int *rsTb, struct jobInfoHead *jInfoH,
               struct loadIndexLog *ld)
{
    static char fname[] = "lsb_pendreason";
    int i, j, num;
    int hostId, reason, hostIdJ, reasonJ;
    static int *reasonTb, memSize = 0;
    static char *hostList = NULL, *retMsg = NULL;
    char *sp;
    
    int pendMsg_ID[] = { 550, 551, 552, 553, 554, 555, 556, 557, 558, 559, 
			 560, 561, 562, 563, 564, 566, 567, 568,  
			 571, 583,  
			 655, 656, 657, 586, 587, 
			 588, 589, 590, 591, 592, 596, 597, 598, 599, 600, 
			 662, 601, 602, 603, 604, 605, 606, 607, 
			 608, 609, 610, 611, 612,
			 614, 615, 616, 617, 618, 619, 667, 620, 621, 622,
			 623, 624, 625, 626, 627, 628, 629, 630, 631, 
			 633, 634, 635, 636, 638, 639, 640, 641, 642,
			 664, 646, 647, 648, 649, 650, 651, 652, 653, 654,
			 644, 645, 665, 666
    };  

    struct msgMap pendMsg[] = {
       { PEND_JOB_NEW,
        "New job is waiting for scheduling"},   /* catgets 550 */
       { PEND_JOB_START_TIME,
        "The job has a specified start time"},  /* catgets 551 */
       { PEND_JOB_DEPEND,
        "Job dependency condition not satisfied"}, /* catgets 552 */
       { PEND_JOB_DEP_INVALID,
        "Dependency condition invalid or never satisfied"},  /* catgets 553 */
       { PEND_JOB_MIG,
        "Migrating job is waiting for rescheduling"},  /* catgets 554 */
       { PEND_JOB_PRE_EXEC,
        "The job's pre-exec command exited with non-zero status"}, /* catgets 555 */
       { PEND_JOB_NO_FILE,
        "Unable to access job file"},  /* catgets 556 */
       { PEND_JOB_ENV,
        "Unable to set job's environment variables"},  /* catgets 557 */
       { PEND_JOB_PATHS,
        "Unable to determine job's home/working directories"},  /* catgets 558 */
       { PEND_JOB_OPEN_FILES,
        "Unable to open job's I/O buffers"},  /* catgets 559 */
       { PEND_JOB_EXEC_INIT,
        "Job execution initialization failed"}, /* catgets 560 */
       { PEND_JOB_RESTART_FILE,
        "Unable to copy restarting job's checkpoint files"}, /* catgets 561 */
       { PEND_JOB_DELAY_SCHED,
        "The schedule of the job is postponed for a while"},  /* catgets 562 */
       { PEND_JOB_SWITCH,
        "Waiting for re-scheduling after switching queue"},  /* catgets 563 */
       {PEND_JOB_DEP_REJECT,
        "Event is rejected by eeventd due to syntax error"},  /* catgets 564 */
       {PEND_JOB_NO_PASSWD,
        "Failed to get user password"},        /* catgets 566 */ 
       {PEND_JOB_MODIFY,
        "Waiting for re-scheduling after parameters have been changed"},/* catgets 568 */ 
       { PEND_JOB_REQUEUED,
        "Requeue the job for the next run"}, /* catgets 571 */
       { PEND_SYS_UNABLE,
	 "System is unable to schedule the job" },  /* catgets 583 */
       {PEND_JOB_ARRAY_JLIMIT,
        "The job array has reached its running element limit"},  /* catgets  655 */
       { PEND_CHKPNT_DIR,
        "Checkpoint directory is invalid"},  /* catgets 656 */
       { PEND_JOB_NO_RESREQ ,
    "Failed to generate job resource requirements due to malloc error"},  /* catgets 657 */

       { PEND_QUE_INACT,
        "The queue is inactivated by the administrator"}, /* catgets 586 */
       { PEND_QUE_WINDOW,
        "The queue is inactivated by its time windows"},  /* catgets 587 */
       { PEND_QUE_JOB_LIMIT,
        "The queue has reached its job slot limit"},       /* catgets 588 */
       { PEND_QUE_PJOB_LIMIT,
        "The queue has not enough job slots for the parallel job"}, /* catgets 589 */
       { PEND_QUE_USR_JLIMIT,
        "User has reached the per-user job slot limit of the queue"}, /* catgets 590 */
       { PEND_QUE_USR_PJLIMIT,
        "Not enough per-user job slots of the queue for the parallel job"}, /* catgets 591 */
       { PEND_QUE_PRE_FAIL,
        "The queue's pre-exec command exited with non-zero status"}, /* catgets 592 */
       { PEND_SYS_NOT_READY,
        "System is not ready for scheduling after reconfiguration"}, /* catgets 596 */
       { PEND_SBD_JOB_REQUEUE,
        "Requeued job is waiting for rescheduling"}, /* catgets 597 */
       { PEND_JOB_SPREAD_TASK,
        "Not enough hosts to meet the job's spanning requirement"}, /* catgets 598 */
       { PEND_QUE_SPREAD_TASK,
        "Not enough hosts to meet the queue's spanning requirement"}, /* catgets 599 */
       { PEND_QUE_WINDOW_WILL_CLOSE,
	"Job will not finish before queue's run window is closed"}, /* catgets 600 */
       { PEND_QUE_PROCLIMIT,
	"Job no longer satisfies queue PROCLIMIT configuration"}, /* catgets 662 */
       { PEND_USER_JOB_LIMIT,
        "The user has reached his/her job slot limit"},  /* catgets 601 */
       { PEND_UGRP_JOB_LIMIT,
        "One of the user's groups has reached its job slot limit"}, /* catgets 602 */
       { PEND_USER_PJOB_LIMIT,
        "The user has not enough job slots for the parallel job"},  /* catgets 603 */
       {PEND_UGRP_PJOB_LIMIT,
        "One of user's groups has not enough job slots for the parallel job"}, /* catgets 604 */
       { PEND_USER_RESUME,
        "Waiting for scheduling after resumed by user"}, /* catgets 605 */
       { PEND_USER_STOP,
        "The job was suspended by the user while pending"}, /* catgets 606 */
       { PEND_ADMIN_STOP,
        "The job was suspended by LSF admin or root while pending"}, /* catgets 607 */
       { PEND_NO_MAPPING,
        "Unable to determine user account for execution"},  /* catgets 608 */
       { PEND_RMT_PERMISSION,
        "The user has no permission to run the job on remote host/cluster"}, /* catgets 609 */

       { PEND_HOST_RES_REQ,
        "Job's resource requirements not satisfied"}, /* catgets 610 */
       { PEND_HOST_NONEXCLUSIVE,
        "Job's requirement for exclusive execution not satisfied"}, /* catgets 611 */
       { PEND_HOST_JOB_SSUSP,
        "Higher or equal priority jobs suspended by host load"}, /* catgets 612 */
       { PEND_SBD_GETPID,
        "Unable to get the PID of the restarting job"},  /* catgets 614 */
       { PEND_SBD_LOCK,
        "Unable to lock host for exclusively executing the job"}, /* catgets 615 */
       { PEND_SBD_ZOMBIE,
        "Cleaning up zombie job"}, /* catgets 616 */
       { PEND_SBD_ROOT,
        "Can't run jobs submitted by root"}, /* catgets 617 */
       { PEND_HOST_WIN_WILL_CLOSE,
	 "Job will not finish on the host before queue's run window is closed"}, /* catgets 618 */
       { PEND_HOST_MISS_DEADLINE,
	 "Job will not finish on the host before job's termination deadline"}, /* catgets 619 */
       { PEND_FIRST_HOST_INELIGIBLE,
	 "The specified first exection host is not eligible for this job at this time"}, /* catgets 667 */	
       { PEND_HOST_DISABLED,
        "Closed by LSF administrator"}, /* catgets 620 */
       { PEND_HOST_LOCKED,
        "Host is locked by LSF administrator"},  /* catgets 621 */
       { PEND_HOST_LESS_SLOTS,
        "Not enough job slot(s)"},  /* catgets 622 */
       { PEND_HOST_WINDOW,
        "Dispatch windows closed"}, /* catgets 623 */
       { PEND_HOST_JOB_LIMIT,
        "Job slot limit reached"}, /* catgets 624 */
       { PEND_QUE_PROC_JLIMIT,
        "Queue's per-CPU job slot limit reached"}, /* catgets 625 */
       { PEND_QUE_HOST_JLIMIT,
        "Queue's per-host job slot limit reached"}, /* catgets 626 */
       { PEND_USER_PROC_JLIMIT,
        "User's per-CPU job slot limit reached"},  /* catgets 627 */
       { PEND_UGRP_PROC_JLIMIT,
        "User group's per-CPU job slot limit reached"}, /* catgets 628 */
       { PEND_HOST_USR_JLIMIT,
        "Host's per-user job slot limit reached"},  /* catgets 629 */
       { PEND_HOST_QUE_MEMB,
        "Not usable to the queue"},  /* catgets 630 */
       { PEND_HOST_USR_SPEC,
        "Not specified in job submission"}, /* catgets 631 */
       { PEND_HOST_NO_USER,
        "There is no such user account"},  /* catgets 633 */
       { PEND_HOST_ACCPT_ONE,
        "Just started a job recently"},   /* catgets 634 */
       { PEND_LOAD_UNAVAIL,
        "Load information unavailable"},  /* catgets 635 */
       { PEND_HOST_NO_LIM,
        "LIM is unreachable now"},   /* catgets 636 */
       { PEND_HOST_QUE_RESREQ,
        "Queue's resource requirements not satisfied"},  /* catgets 638 */
       { PEND_HOST_SCHED_TYPE,
        "Not the same type as the submission host"},  /* catgets 639 */
       { PEND_JOB_NO_SPAN,
        "Not enough processors to meet the job's spanning requirement"}, /* catgets 640 */
       { PEND_QUE_NO_SPAN,
        "Not enough processors to meet the queue's spanning requirement"}, /* catgets 641 */ 
       { PEND_HOST_EXCLUSIVE,
        "Running an exclusive job"}, /* catgets 642  */
       { PEND_HOST_LOCKED_MASTER,
        "Host is locked by master LIM"},  /* catgets 664 */
       { PEND_SBD_UNREACH,
        "Unable to reach slave batch server"}, /* catgets 646 */
       { PEND_SBD_JOB_QUOTA,
        "Number of jobs exceeds quota"},  /* catgets 647 */
       { PEND_JOB_START_FAIL,
        "Failed in talking to server to start the job"},  /* catgets 648 */
       { PEND_JOB_START_UNKNWN,
        "Failed in receiving the reply from server when starting the job"}, /* catgets 649 */
       { PEND_SBD_NO_MEM,
        "Unable to allocate memory to run job"}, /* catgets 650 */
       { PEND_SBD_NO_PROCESS,
        "Unable to fork process to run job"},  /* catgets 651 */
       { PEND_SBD_SOCKETPAIR,
        "Unable to communicate with job process"},  /* catgets 652 */
       { PEND_SBD_JOB_ACCEPT,
        "Slave batch server failed to accept job"},  /* catgets 653 */
       { PEND_HOST_LOAD,
        "Load threshold reached"},  /* catgets 654 */

       { PEND_HOST_QUE_RUSAGE,
        "Queue's requirements for resource reservation not satisfied"}, /* catgets 644 */       
       { PEND_HOST_JOB_RUSAGE,
        "Job's requirements for resource reservation not satisfied"}, /* catgets 645 */      
       { PEND_BAD_HOST,
         "Bad host name, host group name or cluster name"}, /* catgets 665 */

       { PEND_QUEUE_HOST,
         "Host or host group is not used by the queue"}, /* catgets 666 */

       { 0, NULL}
    };        
 
    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: numReasons=%d", fname, numReasons);

    if (!numReasons || !rsTb) {
        lsberrno = LSBE_BAD_ARG;
        return ("");
    }
    if (memSize < numReasons) {
        FREEUP (reasonTb);
        reasonTb = (int *)calloc (numReasons, sizeof(int));
        if (!reasonTb) {
            memSize = 0;
            lsberrno = LSBE_NO_MEM;
            return ("");
        }
        memSize = numReasons;
    }
    for (i = 0; i < numReasons; i++)
        reasonTb[i] = rsTb[i]; 

    FREEUP (hostList);
    FREEUP (retMsg);
    if (jInfoH && jInfoH->numHosts != 0 && jInfoH->hostNames != NULL) {
        hostList = malloc (jInfoH->numHosts * MAXHOSTNAMELEN);
        retMsg = malloc (jInfoH->numHosts * MAXHOSTNAMELEN + MSGSIZE);
        if (hostList == NULL || retMsg == NULL) {
            lsberrno = LSBE_NO_MEM;
            return ("");
        }
    } else {
        retMsg = malloc (MSGSIZE);
        if (retMsg == NULL) {
            lsberrno = LSBE_NO_MEM;
            return ("");
        }
    }

    retMsg[0] = '\0';
    for (i = 0; i < numReasons; i++) {
        if (!reasonTb[i])
            continue;
        GET_LOW (reason, reasonTb[i]);
        if (!reason)
            continue;
        GET_HIGH (hostId, reasonTb[i]);
        if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
            ls_syslog(LOG_DEBUG2, "%s: hostId=%d, reason=%d reasonTb[%d]=%d",
                                  fname, hostId, reason, i, reasonTb[i]);

        if (jInfoH && jInfoH->numHosts != 0 && jInfoH->hostNames != NULL) {
           strcpy(hostList, jInfoH->hostNames[hostId]);
        }
        else
            num = 1;
	
        for (j = i + 1; j < numReasons; j++) {
            if (reasonTb[j] == 0)
                continue;
            GET_LOW (reasonJ, reasonTb[j]);
            if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
                ls_syslog(LOG_DEBUG2, "%s: reasonJ=%d reasonTb[j]=%d",
                                       fname, reasonJ, reasonTb[j]);
            if (reasonJ != reason)
                continue;
            GET_HIGH (hostIdJ, reasonTb[j]);
            if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
                ls_syslog(LOG_DEBUG2, "%s: j=%d, hostIdJ=%d", 
                                       fname, j, hostIdJ);
            reasonTb[j] = 0;
            if (jInfoH && jInfoH->numHosts != 0 && jInfoH->hostNames != NULL) {
               sprintf(hostList, "%s, %s", hostList, jInfoH->hostNames[hostIdJ]);
            } else {
                num++;
            }
        }
        if (reason >= PEND_HOST_LOAD
	    && reason < PEND_HOST_QUE_RUSAGE) {	    

	    getMsgByRes(reason - PEND_HOST_LOAD, PEND_HOST_LOAD, &sp, ld);

	} else if (reason >= PEND_HOST_QUE_RUSAGE
		   && reason < PEND_HOST_JOB_RUSAGE) {

	    getMsgByRes(reason - PEND_HOST_QUE_RUSAGE, 
			PEND_HOST_QUE_RUSAGE, 
			&sp, 
			ld);

	} else if (reason >= PEND_HOST_JOB_RUSAGE) {

	    getMsgByRes(reason - PEND_HOST_JOB_RUSAGE, 
			PEND_HOST_JOB_RUSAGE, 
			&sp, 
			ld);

        } else {
            sp = getMsg(pendMsg, pendMsg_ID,  reason);
	}

        if (jInfoH && jInfoH->numHosts != 0 && jInfoH->hostNames != NULL)
            sprintf (retMsg, "%s %s: %s;\n", retMsg, sp, hostList);
        else if (num == 1)
            sprintf (retMsg, _i18n_msg_get(ls_catd , NL_SETN, 713, "%s %s: 1 host;\n"), retMsg, sp); /* catgets 713 */
        else
            sprintf (retMsg, _i18n_msg_get(ls_catd , NL_SETN, 714, "%s %s: %d hosts;\n"), retMsg, sp, num); /* catgets 714 */
    }

    return retMsg;
} 

 
static void
userIndexReasons(char                  *msgline, 
		 int                   resource, 
		 int                   reason, 
		 struct loadIndexLog   *ld)
{

    if (ld == NULL || reason <= MEM || resource >= ld->nIdx) {
        sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 711, "External load index is beyond threshold"));  /* catgets 711 */
        return;
    }

    if (reason == PEND_HOST_LOAD) {
	sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 712, "External load index (%s) is beyond threshold"), ld->name[resource]);  /* catgets 712 */
    } else if (reason == PEND_HOST_QUE_RUSAGE) {
	sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 737, "Queue requirements for reserving resource (%s) not satisfied"), ld->name[resource]);  /* catgets 737 */
    } else if (reason == PEND_HOST_JOB_RUSAGE) {
	sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 738, "Job's requirements for reserving resource (%s) not satisfied"), ld->name[resource]);  /* catgets 738 */
    }

} 

static 
void  getMsgByRes(int                   resource, 
		  int                   reason, 
		  char                  **sp, 
		  struct loadIndexLog   *ld)
{
    switch (resource) {
    case R15S:
	
	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 700, "\
The 15s effective CPU queue length (r15s) is beyond threshold")); /* catgets 700 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 715, "Queue requirements for reserving resource (r15s) not satisfied ")); /* catgets 715 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 716, "Job requirements for reserving resource (r15s) not satisfied")); /* catgets 716 */ 
	}
	break;

    case R1M:
	
	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 701, "The 1 min effective CPU queue length (r1m) is beyond threshold")); /* catgets 701 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 717, "Queue requirements for reserving resource (r1m) not satisfied ")); /* catgets 717 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 718, "Job requirements for reserving resource (r1m) not satisfied")); /* catgets 718 */
	}
	break;

    case R15M:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 702, "The 15 min effective CPU queue length (r15m) is beyond threshold")); /* catgets 702 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 719, "Queue requirements for reserving resource (r15m) not satisfied ")); /* catgets 719 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 720, "Job requirements for reserving resource (r15m) not satisfied")); /* catgets 720 */ 
	}
	break;

    case UT:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 703, "The CPU utilization (ut) is beyond threshold")); /* catgets 703 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 721, "Queue requirements for reserving resource (ut) not satisfied ")); /* catgets 721 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 722, "Job requirements for reserving resource (ut) not satisfied")); /* catgets 722 */ 
	}
	break;

    case PG:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 704, "The paging rate (pg) is beyond threshold"));  /* catgets 704 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 723, "Queue requirements for reserving resource (pg) not satisfied")); /* catgets 723 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 724, "Job requirements for reserving resource (pg) not satisfied")); /* catgets 724 */ 
	}
	break;

    case IO:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 705, "The disk IO rate (io) is beyond threshold"));  /* catgets 705 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 725, "Queue requirements for reserving resource (io) not satisfied")); /* catgets 725 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 726, "Job requirements for reserving resource (io) not satisfied")); /* catgets 726 */ 
	}
	break;

    case LS:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 706, "There are too many login users (ls)"));    /* catgets 706 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 727, "Queue requirements for reserving resource (ls) not satisfied")); /* catgets 727 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 728, "Job requirements for reserving resource (ls) not satisfied")); /* catgets 728 */ 
	}
	break;

    case IT:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 707, "The idle time (it) is not long enough"));  /* catgets 707 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 729, "Queue requirements for reserving resource (it) not satisfied")); /* catgets 729 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 730, "Job requirements for reserving resource (it) not satisfied")); /* catgets 730 */ 
	}
	break;

    case TMP:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 708, "The available /tmp space (tmp) is low"));  /* catgets 708 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 731, "Queue requirements for reserving resource (tmp) not satisfied")); /* catgets 731 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 732, "Job requirements for reserving resource (tmp) not satisfied")); /* catgets 732 */ 
	}
	break;

    case SWP:

	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 709, "The available swap space (swp) is low")); /* catgets 709 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 733, "Queue requirements for reserving resource (swp) not satisfied")); /* catgets 733 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 734, "Job requirements for reserving resource (swp) not satisfied")); /* catgets 734 */ 
	}
	break;

    case MEM:
	if (reason == PEND_HOST_LOAD) {
	    sprintf(msgline, _i18n_msg_get(ls_catd , NL_SETN, 710, "The available memory (mem) is low"));  /* catgets 710 */
	} else if (reason == PEND_HOST_QUE_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 735, "Queue requirements for reserving resource (mem) not satisfied")); /* catgets 735 */ 
	} else if (reason == PEND_HOST_JOB_RUSAGE) {
	    sprintf(msgline, _i18n_msg_get(ls_catd, NL_SETN, 736, "Job requirements for reserving resource (mem) not satisfied")); /* catgets 736 */ 
	}
	break;

    default:
	userIndexReasons(msgline, resource, reason, ld);
	break;
    }

    *sp = msgline;  
}

/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: cmd.job.c 397 2007-11-26 19:04:00Z mblack $
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
#include "cmd.h"
#include "../lib/lsb.h"
#include "../lsbatch.h"

#define NL_SETN 8 	

#include <errno.h>

static char *strXfOptions(int);
void prtJobRusage(struct jobInfoEnt *);
void prtJobRusageUF(struct jobInfoEnt *);


void
prtHeader(struct jobInfoEnt *job, int prt_q, int tFormat)
{
    char prline[MAXLINELEN];

    if (!tFormat) {
        sprintf(prline, "\n%s <%s>,", I18N_Job, lsb_jobid2str(job->jobId));

        printf("%s", prline);
        if (job->submit.options & SUB_JOB_NAME) {
           char *jobName, *pos;
           jobName = job->submit.jobName;
           if ((pos = strchr(jobName, '[')) && LSB_ARRAY_IDX(job->jobId)) {
               *pos = '\0';
               sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId));
           }
           sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,552, " Job Name <%s>,")), jobName); /* catgets  552  */
           printf("%s", prline);
        }
    }
    if (tFormat) {
       sprintf(prline, ",");
       printf("%s", prline);
    }
    sprintf(prline, " %s <%s>,", I18N_User, job->user);
    printf("%s", prline);

    if (lsbMode_ & LSB_MODE_BATCH) {
	sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,554, " Project <%s>,")), job->submit.projectName); /* catgets  554  */
	printf("%s", prline);
    }


    if (job->submit.options & SUB_MAIL_USER) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,556, " Mail <%s>,")), job->submit.mailUser); /* catgets  556  */
        printf("%s", prline);
    }

    if (prt_q) {
	sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,557, " Status <%s>, Queue <%s>,")),  /* catgets  557  */
                get_status(job),
	        job->submit.queue);
	printf("%s", prline);
    }


    if (job->submit.options & SUB_INTERACTIVE) {
	sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,558, " Interactive"))); /* catgets  558  */
	if (job->submit.options & SUB_PTY) {
	    strcat(prline, (_i18n_msg_get(ls_catd,NL_SETN,559, " pseudo-terminal"))); /* catgets  559  */
	    if (job->submit.options & (SUB_PTY_SHELL))
		strcat(prline, (_i18n_msg_get(ls_catd,NL_SETN,560, " shell"))); /* catgets  560  */
	}
	strcat(prline, (_i18n_msg_get(ls_catd,NL_SETN,561, " mode,"))); /* catgets  561  */
	printf("%s", prline);
    }


    if ( job->jobPriority > 0 ) {
	sprintf(prline, " %s <%d>,", I18N_Job_Priority, job->jobPriority);
	printf("%s", prline);
    }

    if (job->submit.options2 & (SUB2_JOB_CMD_SPOOL)) {
        sprintf(prline, " %s(Spooled) <%s>", I18N_Command, job->submit.command);
    } else {
	sprintf(prline, " %s <%s>", I18N_Command, job->submit.command);
    }
    printf("%s", prline);

    if (job->chargedSAAP && strlen(job->chargedSAAP) > 0) {
        if (tFormat) {
            sprintf(prline, ", %s <%s>", I18N_SAAP, job->chargedSAAP);
        } else {
            sprintf(prline, ", %s <%s>\n", I18N_SAAP, job->chargedSAAP);
        }
        printf("%s", prline);
    } else {
        if (!tFormat) {
            printf("\n");
        }
    }
}


void
prtJobSubmit(struct jobInfoEnt *job, int prt_q, int tFormat)
{
    char prline[MAXLINELEN];
    char *timestr;

    timestr = putstr_(_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->submitTime));
    if (tFormat) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,569, "%s: Job <%s> submitted from host <%s>")), /* catgets  569  */
                    timestr, lsb_jobid2str(job->jobId), job->fromHost);
    } else {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,570, "%s: Submitted from host <%s>")), /* catgets  570  */
                    timestr, job->fromHost);
    }

    FREEUP(timestr);
    printf("%s", prline);

    if (job->submit.options2 & SUB2_HOLD) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,570, " with hold")));
        /* catgets  570  */
        printf("%s", prline);
    }

    if (prt_q) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,571, ", to Queue <%s>")), job->submit.queue); /* catgets  571  */
        printf("%s", prline);
    }

    TIMEIT(2, prtBTTime(job), "prtBTTime");

}

void
prtBTTime(struct jobInfoEnt *job)
{
    char prline[MAXLINELEN];

    
    if (job->submit.beginTime > 0) {
        sprintf(prline, ", %s <%s>", 
	        I18N(572, "Specified Start Time"),  
                _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, 
			    (time_t *)&(job->submit.beginTime)));
        printf(prline);
    }
    
    if (job->submit.termTime > 0) {
        sprintf(prline, ", %s <%s>", 
		I18N(573, "Specified Termination Time"), 
                _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, 
			     (time_t *)&(job->submit.termTime)));
        printf(prline);
    }

} 

void
prtFileNames(struct jobInfoEnt *job, int prtCwd)
{
    char prline[MAXLINELEN];
    int i;

    
    if (prtCwd == TRUE) {
        if (job->cwd[0] == '/' || job->cwd[0] == '\\' || 
	    (job->cwd[0] != '\0' && job->cwd[1] == ':'))
	    sprintf(prline, ", CWD <%s>", job->cwd);
        else if (job->cwd[0] == '\0')
	    sprintf(prline, ", CWD <$HOME>"); 
        else
	    sprintf(prline, ", CWD <$HOME/%s>", job->cwd); 
        printf(prline);
    }

    
    if (job->submit.options & SUB_IN_FILE)  {
 	sprintf(prline, ", %s <%s>", 
	 	I18N(577, "Input File"), job->submit.inFile);/* catgets 577 */
 	printf(prline);
    }

    
    if (job->submit.options2 & SUB2_IN_FILE_SPOOL)  {
	sprintf(prline, ", %s(Spooled) <%s>", 
		I18N(577, "Input File"), job->submit.inFile);/* catgets 577 */
	printf(prline);
    }

    
    if (job->submit.options & SUB_OUT_FILE) {
        sprintf(prline, ", %s <%s>", 
	        I18N(578, "Output File"), job->submit.outFile);/* catgets 578 */
        printf(prline);
    }

    
    if (job->submit.options & SUB_ERR_FILE) {
        sprintf(prline, ", %s <%s>", 
		I18N(579, "Error File"), job->submit.errFile); /* catgets 579 */
        printf(prline);
    }

    
    if (job->submit.nxf) {
	sprintf(prline, ", %s",  I18N(580, "Copy Files")); /* catgets 580 */
	printf(prline);
    }
    
    for (i = 0; i < job->submit.nxf; i++) {
	sprintf(prline, " \"%s %s %s\"", job->submit.xf[i].subFn,
		strXfOptions(job->submit.xf[i].options),
		job->submit.xf[i].execFn);
	printf(prline);
    }

} 

static char *
strXfOptions(int options)
{
    static char s[5];

    s[0] = '\0';
    if (options & XF_OP_SUB2EXEC)
	strcat(s, ">");
    if (options & XF_OP_SUB2EXEC_APPEND)
	strcat(s, ">");
    if (options & XF_OP_EXEC2SUB)
	strcat(s, "<");
    if (options & XF_OP_EXEC2SUB_APPEND)
	strcat(s, "<");

    return (s);
}


void
prtSubDetails(struct jobInfoEnt *job, char *hostPtr, float hostFactor)
{
    char prline[MSGSIZE];
    int  k;


    if ((job->submit.options & SUB_NOTIFY_END) &&
       (job->submit.options & SUB_NOTIFY_BEGIN)) {
        sprintf(prline, ", %s",
		I18N(581, "Notify when job begins/ends")); /* catgets 581  */
        printf("%s", prline);
    }
    else if (job->submit.options & SUB_NOTIFY_BEGIN) {
        sprintf(prline, ", %s",
		I18N(582, "Notify when job begins")); /* catgets 582  */
        printf("%s", prline);
    }
    else if (job->submit.options & SUB_NOTIFY_END) {
        sprintf(prline, ", %s",
	        I18N(583, "Notify when job ends")); /* catgets 583 */
        printf("%s", prline);
    }



    if (job->submit.options & SUB_EXCLUSIVE) {
        sprintf(prline, ", %s",
		I18N(584, "Exclusive Execution")); /* catgets  584  */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_RERUNNABLE) {
        sprintf(prline, ", %s", I18N(585, "Re-runnable")); /* catgets  585  */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_RESTART) {
	if (job->submit.options & SUB_RESTART_FORCE)
	    sprintf(prline, ", %s",
		    I18N(586, "Restart force")); /* catgets 586  */
	else
	    sprintf(prline, ", %s", I18N_Restart);
        printf("%s", prline);
    }


    if (job->submit.options & SUB_CHKPNTABLE)  {
	if (job->submit.chkpntPeriod) {
	    sprintf (prline, (_i18n_msg_get(ls_catd,NL_SETN,588, ", Checkpoint period %d min.")), /* catgets  588  */
		     (int) job->submit.chkpntPeriod/60);
        printf("%s", prline);
	}

	sprintf(prline, ", %s <%s>",
		I18N(589, "Checkpoint directory"), /* catgets  589  */
		job->submit.chkpntDir);
        printf("%s", prline);
    }


    if (job->submit.numProcessors > 1
        || job->submit.maxNumProcessors > 1) {
        if (job->submit.numProcessors == job->submit.maxNumProcessors)
            sprintf(prline, ", %d %s",
		    job->submit.numProcessors,
		    I18N(590, "Processors Requested"));  /* catgets  590  */
        else
            sprintf(prline, ", %d-%d %s", job->submit.numProcessors,
                    job->submit.maxNumProcessors,
		    I18N(590, "Processors Requested"));
        printf("%s", prline);
    }



    if (strlen(job->submit.resReq)) {
        sprintf(prline, ", %s <%s>",
		I18N(592, "Requested Resources"), /* catgets  592  */
		job->submit.resReq);
        printf("%s", prline);
    }


    if (strlen(job->submit.dependCond)) {
	char *temp;
	if ((temp = (char *)malloc (strlen(job->submit.dependCond) + 30))
	      == NULL) {
	    perror("malloc");
	    exit(-1);
        } else {
            sprintf(temp, ", %s <%s>",
		    I18N(594, "Dependency Condition"), /* catgets  594  */
		    job->submit.dependCond);
		    printf("%s", temp);
	    free (temp);
        }
    }

    if (strlen(job->submit.loginShell)) {
	sprintf(prline, ", %s <%s>",
	        I18N(597, "Login Shell"),  /* catgets  597  */
	        job->submit.loginShell);
	printf("%s", prline);
    }


    if (strlen(job->submit.preExecCmd)) {
        sprintf(prline, ", %s <%s>",
		I18N(598, "Pre-execute Command"),  /* catgets  598  */
		job->submit.preExecCmd);
        printf("%s", prline);
    }

    /* pre_exec_command string */
    if (strlen(job->submit.postExecCmd)) {
        sprintf(prline, ", Post-execute Command <%s>", job->submit.postExecCmd);
        prtLine(prline);
    }

    /* User Specified Hosts */
    if (job->submit.numAskedHosts) {
        sprintf(prline, ", %s <%s>",
		I18N(599, "Specified Hosts"),   /* catgets  599  */
		job->submit.askedHosts[0]);
        printf("%s", prline);
        for (k = 1; k < job->submit.numAskedHosts; k++) {
	    sprintf(prline, ", <%s>", job->submit.askedHosts[k]);
	    printf("%s", prline);
        }
    }


    if (job->submit.options & SUB_WINDOW_SIG) {
        sprintf(prline, ", %s <%d>",
		I18N(601, "Signal Value"),  /* catgets  601  */
                sig_decode(job->submit.sigValue));
        printf("%s", prline);
    }

    if ( job->submit.options2 & SUB2_JOB_PRIORITY && job->submit.userPriority > 0) {
	sprintf(prline, ", %s <%d>",
	    I18N_User_Priority, job->submit.userPriority);
        printf("%s", prline);
    }

    sprintf(prline, ";\n");
    printf("%s", prline);

    prtResourceLimit (job->submit.rLimits, hostPtr, hostFactor, NULL);

}

void
prtJobStart(struct jobInfoEnt *job, int prtFlag, int jobPid, int tFormat)
{
    char prline[MAXLINELEN], tBuff[20];
    time_t startTime;



    int                 i = 0;
    NAMELIST  *hostList = NULL;


    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
         &&  strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {
            exit(99);
        }
    }


    if (tFormat) {
        sprintf (tBuff, "%s <%s>", I18N_Job, lsb_jobid2str(job->jobId));
    }
    else if (LSB_ARRAY_IDX(job->jobId) > 0 )
        sprintf (tBuff, " [%d]", LSB_ARRAY_IDX(job->jobId));
    else
       tBuff[0] = '\0';

    if (job->startTime && job->numExHosts) {


        if (job->startTime < job->submitTime)
            startTime = job->submitTime;
        else
            startTime = job->startTime;

        if ((job->submit.options & SUB_PRE_EXEC)
             && (prtFlag != BJOBS_PRINT)) {
            if (prtFlag == BHIST_PRINT_PRE_EXEC)
	    {
		if (tBuff[0] == '\0')
	            sprintf(prline, "%s: %s",
		            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
					 &startTime),
		            I18N(604, "The pre-exec command is started on")); /* catgets  604  */
		else
	            sprintf(prline, "%s:%s, %s",
			    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
					&startTime),
		            tBuff,
			    I18N(605, "the pre-exec command is started on")); /* catgets  605  */
	    }
	    else
	    {
		if (tBuff[0] == '\0')
	            sprintf(prline, "%s: %s",
			    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
			    I18N(606, "The batch job command is started on")); /*catgets 606 */
		else
	            sprintf(prline, "%s:%s, %s",
			   _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
			   tBuff,
			   I18N(607, "the batch job command is started on")); /*catgets 607 */
	    }
	} else {
	    if (jobPid > 0)
	    {
		if (tBuff[0] == '\0')
		    sprintf(prline, "%s: %s",
			    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
			    I18N(608, "Started on")); /* catgets  608  */
		else
		    sprintf(prline, "%s:%s %s",
			    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
			    tBuff,
			    I18N(609, "started on")); /* catgets  609  */
	    }
	    else
	    {
		if (tBuff[0] == '\0')
		    sprintf(prline, "%s: %s",
			    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
			    I18N(610, "Dispatched to")); /* catgets 610 */
		else
		    sprintf(prline, "%s: %s %s",
			    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
			    tBuff,
			    I18N(611, "dispatched to")); /* catgets  611 */
	    }
	}


        printf("%s", prline);
        if (job->numExHosts > 1) {
            sprintf(prline, " %d %s",
		    job->numExHosts,
		    I18N(612, "Hosts/Processors")); /* catgets  612  */
            printf("%s", prline);
        }

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
             && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
            for (i = 0; i < hostList->listSize; i++) {
                sprintf(prline, " <%d*%s>", hostList->counter[i],
                                            hostList->names[i]);
                printf("%s", prline);
            }
        } else {
            for (i = 0; i < job->numExHosts; i++) {
                sprintf(prline, " <%s>", job->exHosts[i]);
                printf("%s", prline);
            }
        }

	if (job->execHome && strcmp (job->execHome, "")) {
	    sprintf(prline, ", %s <%s>",
		    I18N(615, "Execution Home"),  /* catgets 615 */
		    job->execHome);
        printf("%s", prline);
        }
	if (job->execCwd && strcmp (job->execCwd, "")) {
	    sprintf(prline, ", %s <%s>",
		    I18N(616, "Execution CWD"), /* catgets 616 */
		    job->execCwd);
        printf("%s", prline);
        }
	if (job->execUsername && strcmp(job->execUsername, "") &&
		strcmp(job->user, job->execUsername)) {
	    sprintf(prline, ", %s <%s>",
		    I18N(617, "Execution user name"), /* catgets 617 */
		    job->execUsername);
            printf("%s", prline);
        }
	sprintf(prline, ";\n");
	printf("%s", prline);
    }
}

void
prtJobReserv(struct jobInfoEnt *job)
{
    char prline[MAXLINELEN];


    int                 i = 0;
    NAMELIST  *hostList = NULL;

    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
         && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {
            exit(99);
        }
    }


    if (job->numExHosts > 0 && job->reserveTime > 0) {
        if (job->numExHosts > 1)
            sprintf(prline, "%s: Reserved <%d> job slots on host(s)", /* catgets  618  */
                    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->reserveTime),
                    job->numExHosts);
        else
            sprintf(prline, "%s: Reserved <%d> job slot on host", /* catgets  619  */
                    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->reserveTime),
                    job->numExHosts);
        printf("%s", prline);

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
            for (i = 0; i < hostList->listSize; i++) {
                sprintf(prline, " <%d*%s>", hostList->counter[i],
                                            hostList->names[i]);
                printf("%s", prline);
            }
        } else {
            for (i = 0; i < job->numExHosts; i++) {
                sprintf(prline, " <%s>", job->exHosts[i]);
                printf("%s", prline);
            }
        }

    }

}

void
prtAcctFinish(struct jobInfoEnt *job)
{
    char prline[MAXLINELEN];

    if (job->status == JOB_STAT_DONE)
        sprintf(prline, "%s: %s.\n", 
		_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->endTime),
		I18N(620, "Completed <done>")); /* catgets  620  */
    else if (job->status == JOB_STAT_EXIT) 
        sprintf(prline, "%s: %s.\n",
	       _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->endTime),
	       I18N(621, "Completed <exit>")); /* catgets  621  */
    else
        sprintf(prline, "%s: %s.\n",
		_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->endTime),
		I18N(622, "sbatchd unavail <zombi>"));  /* catgets  622  */
    printf(prline);
} 

void
prtJobFinish(struct jobInfoEnt *job, struct jobInfoHead *jInfoH)
{
    char prline[MSGSIZE];
    time_t doneTime;
    static struct loadIndexLog *loadIndex = NULL;
    char *pendReasons;

    if (loadIndex == NULL)
	TIMEIT(1, loadIndex = initLoadIndex(), "initLoadIndex");

    doneTime = job->endTime;

    switch (job->status) {
    case JOB_STAT_DONE:
    case (JOB_STAT_DONE | JOB_STAT_PDONE):
    case (JOB_STAT_DONE | JOB_STAT_PERR):
    
        if ((job->startTime < job->submitTime) 
		 && (job->endTime < (job->submitTime 
				     + (time_t) MAX(job->cpuTime, MIN_CPU_TIME)))) {
            doneTime = job->submitTime +
		(time_t) MAX(job->cpuTime, 0.0001);
        } else if (job->startTime >= job->submitTime &&
		 job->endTime < (job->startTime + 
				 (time_t)MAX(job->cpuTime, 0.0001)) &&
		 job->numExHosts == 1) {

	    doneTime = job->startTime + (time_t) MAX(job->cpuTime, 0.0001);

	    if (job->endTime <= doneTime) {
                doneTime = job->endTime;
	    }


        }

    case (JOB_STAT_EXIT | JOB_STAT_PDONE):
    case (JOB_STAT_EXIT | JOB_STAT_PERR):
    case JOB_STAT_EXIT:
        if (job->reasons & EXIT_ZOMBIE) {
	    sprintf(prline, "%s: ", 
		    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->endTime));
	    printf(prline);
	    sprintf(prline, "Termination request issued; the job will be killed once the host is ok;"); /* catgets  623  */
	    printf(prline);
	    break;
        }
        sprintf(prline, "%s: ", 
		_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &doneTime));
        printf(prline);
        if (strcmp(get_status(job), "DONE") == 0)
	{
	    sprintf(prline, "Done successfully."); /* catgets 624 */
        }
        else {
	    LS_WAIT_T wStatus;
	    
	    LS_STATUS(wStatus) = job->exitStatus;

	    if (job->cpuTime >= MIN_CPU_TIME && job->exitStatus) {
		if (WEXITSTATUS(wStatus))
	            sprintf(prline, "Exited with exit code %d.", /* catgets  626  */
			WEXITSTATUS(wStatus));
                else 
		    sprintf(prline, "Exited by signal %d.", WTERMSIG(wStatus)); /* catgets  627  */
            } else
		sprintf(prline, I18N_Exited);
	}

        printf(prline);

	if (job->numExHosts > 0) {
	    if (job->cpuTime < MIN_CPU_TIME)
		sprintf(prline, " The CPU time used is unknown.\n"); /* catgets  629  */
	    else
		sprintf(prline, " The CPU time used is %1.1f seconds.\n",  /* catgets  630  */
			job->cpuTime);
	} else {
	    sprintf(prline, "\n");
	}

	printf(prline);
        break;
    case JOB_STAT_PSUSP:
    case JOB_STAT_PEND:
        sprintf(prline,  " PENDING REASONS:\n"); /* catgets  631  */
        printf(prline);
	pendReasons = lsb_pendreason(job->numReasons, job->reasonTb,
				     jInfoH, loadIndex);
	printf(pendReasons);
        break;			
    case JOB_STAT_SSUSP:
    case JOB_STAT_USUSP:

        
        TIMEIT(1, prtJobRusage(job), "prtJobRusage");

        sprintf(prline, " SUSPENDING REASONS:\n"); /* catgets  632  */
        printf(prline);

        if (job->reasons) {
            sprintf(prline, "%s", lsb_suspreason(job->reasons,
						 job->subreasons,
						 loadIndex));
            printf(prline);
        }
        break;                       
    case JOB_STAT_RUN:
        
        TIMEIT(1, prtJobRusage(job), "prtJobRusage");
        break;
    default:
        break;
    }
} 

char *
get_status(struct jobInfoEnt *job)
{
    char *status;
    
    switch (job->status) {
    case JOB_STAT_NULL:
        status = "NULL";
        break;
    case JOB_STAT_PEND:
        status = "PEND";
        break;
    case JOB_STAT_PSUSP:
        status = "PSUSP";
        break;
    case JOB_STAT_RUN:
        status = "RUN";
        break;
    case JOB_STAT_RUN | JOB_STAT_WAIT:
        status = "WAIT";
        break;
    case JOB_STAT_SSUSP:
        status = "SSUSP";
        break;
    case JOB_STAT_USUSP:
        status = "USUSP";
        break;
    case JOB_STAT_EXIT:
        if (job->reasons & EXIT_ZOMBIE)
	    status = "ZOMBI";
        else
            status = "EXIT";
        break;
    case JOB_STAT_DONE:
    case JOB_STAT_DONE|JOB_STAT_PDONE:
    case JOB_STAT_DONE|JOB_STAT_WAIT:
    case JOB_STAT_DONE|JOB_STAT_PERR:
        status = "DONE";
        break;
    case JOB_STAT_UNKWN:
        status = "UNKWN"; 
        break;
    default:
        status = "ERROR";
    }
    
    return status;
} 

 
struct loadIndexLog *
initLoadIndex(void)
{
    int i;
    struct lsInfo *lsInfo;
    static struct loadIndexLog loadIndex;
    static char *defNames[] = {"r15s", "r1m", "r15m", "ut", "pg", "io", "ls",
			       "it", "swp", "mem", "tmp"};
    static char **names;
    
    TIMEIT(1, (lsInfo = ls_info()), "ls_info");
    if (lsInfo == NULL) {
	loadIndex.nIdx = 11;
	loadIndex.name = defNames;
    } else {
	loadIndex.nIdx = lsInfo->nRes;
        if (!names)
            if(!(names =(char **)malloc(lsInfo->nRes*sizeof(char *)))) {
                lserrno=LSE_MALLOC;
                ls_perror("initLoadIndex"); 
                return NULL;
	    }
	for (i = 0; i < loadIndex.nIdx; i++)
	    names[i] = lsInfo->resTable[i].name;
	loadIndex.name = names;
    }
    return (&loadIndex);
}

void
prtJobRusage(struct jobInfoEnt *job)
{
    char prline[MAXLINELEN];

    int i, j;
    int linepos;



    if (IS_FINISH(job->status))
        return;



    if (IS_PEND(job->status)) {
        if (job->runRusage.utime || job->runRusage.stime) {
            sprintf(prline, "%s: %s.\n",
		    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
				&job->jRusageUpdateTime),
	            I18N(644, "Resource usage collected")); /* catgets 644  */
            printf("%s", prline);
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,645, "                     The CPU time used is %d seconds.\n")),  /* catgets  645  */
                             job->runRusage.utime + job->runRusage.stime);
            printf("%s", prline);
        }
        return;
    };




   if (job->runRusage.utime > 0 || job->runRusage.stime > 0
       || job->runRusage.mem > 0 || job->runRusage.swap > 0
       || job->runRusage.npgids > 0 || job->runRusage.npids > 0) {
        sprintf(prline, "%s: %s.\n",
		 _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
			      &job->jRusageUpdateTime),
		 I18N(646, "Resource usage collected")); /* catgets  646  */
       printf("%s", prline);
    } else
        return;

    if (job->runRusage.utime > 0 || job->runRusage.stime > 0) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,647, "                     The CPU time used is %d seconds.\n")), /* catgets  647  */
                             job->runRusage.utime + job->runRusage.stime);
        printf("%s", prline);
    }


    if (job->runRusage.mem > 0) {
        if (job->runRusage.mem > 1024)
            sprintf(prline, "                     MEM: %d Mbytes", job->runRusage.mem/1024); /* catgets  648  */
        else
            sprintf(prline, "                     MEM: %d Kbytes", job->runRusage.mem); /* catgets  649  */
        printf("%s", prline);
    }

    if (job->runRusage.swap > 0) {
	char *space;

	if (job->runRusage.mem > 0)
	    space = ";  ";
	else
	    space = "                     ";

        if (job->runRusage.swap > 1024)
            sprintf(prline, "%sSWAP: %d Mbytes\n", space, /* catgets  650  */
                    job->runRusage.swap/1024);
        else
            sprintf(prline, "%sSWAP: %d Kbytes\n", space, job->runRusage.swap); /* catgets  651  */
        printf("%s", prline);
    } else {
	if (job->runRusage.mem > 0) {
	    sprintf(prline, "\n");
        printf("%s", prline);
	}
    }

    if (job->runRusage.npgids <= 0)
        return;


    for (i=0; i < job->runRusage.npgids; i++) {
        sprintf(prline, "                     PGID: %d;  ", job->runRusage.pgid[i]); /* catgets  652  */
        linepos = strlen(prline);
        printf("%s", prline);
        sprintf(prline, "PIDs: "); /* catgets  653  */
        linepos += 6;
        printf("%s", prline);
        for (j=0; j < job->runRusage.npids; j++) {
            if (job->runRusage.pgid[i] == job->runRusage.pidInfo[j].pgid) {
                sprintf(prline, "%d ", job->runRusage.pidInfo[j].pid);
		  linepos += strlen(prline);

		  if (linepos >= 80) {
              char *newline ="\n                     ";
              printf("%s", newline);
              printf("%s", prline);
		      linepos = strlen(prline) + 21;
		  }
		  else
              printf("%s", prline);

            }
        }

    }
    sprintf(prline, "\n");
    printf("%s", prline);

    if (job->runRusage.mem > 0) {
        printf ("\n MEMORY USAGE:\n");
        if (job->runRusage.mem > 1024)
            printf(" MAX MEM: %d Mbytes;  AVG MEM: %d Mbytes\n", job->runRusage.mem/1024, job->runRusage.mem/1024);
        else
            printf(" MAX MEM: %d Kbytes;  AVG MEM: %d Kbytes\n", job->runRusage.mem, job->runRusage.mem);
    }
}

void
displayLong (struct jobInfoEnt *job, struct jobInfoHead *jInfoH, 
							  float cpuFactor)
{
    char *hostPtr, *sp;
    char hostName[MAXHOSTNAMELEN];
    float hostFactor, *getFactor;
    static int first = TRUE;
    static struct lsInfo *lsInfo;
    char prline[MAXLINELEN];

    if (first) {
        first = FALSE;
	TIMEIT(0, (lsInfo = ls_info()), "ls_info");
	if (lsInfo == NULL) {
	    ls_perror("ls_info");
	    exit(-1);
	}
    }

    prtHeader(job, TRUE, FALSE);
    prtJobSubmit(job, FALSE, FALSE);
    TIMEIT(1, prtFileNames(job, TRUE), "prtFileNames");

    
    hostPtr = job->submit.hostSpec;
    hostFactor = 1.0;

    

    if (job->numExHosts > 0 
	  && (strcmp (job->exHosts[0], LOST_AND_FOUND) != 0)
	  && !IS_PEND (job->status)) {
	strcpy (hostName, job->exHosts[0]);
        
        if ((sp = strstr (hostName, "@")) != NULL) {
            *sp = '\0';
	    
            if (strcmp(hostName, hostPtr) == 0) {
	        if ((getFactor=getCpuFactor(hostName, TRUE)) == NULL) {
                    printf("\n");
		    fprintf(stderr,
			"Cannot obtain execution host information: %s\n", ls_errmsg[lserrno]);
    	            exit(-1);
	        } else {
	            hostFactor = *getFactor;
	            hostPtr = job->exHosts[0];
                }
            }
        }
    }

    TIMEIT(1, prtSubDetails(job, hostPtr, hostFactor), "prtSubDetails");
    if (job->numExHosts > 0 && job->reserveTime > 0) {
       TIMEIT(1, prtJobReserv(job), "prtJobReserv");
       sprintf(prline, ";\n");
       printf(prline);
    }

    
    if (job->predictedStartTime && IS_PEND(job->status)) {
	char localTimeStr[60];
	strcpy ( localTimeStr, _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->predictedStartTime));
        sprintf(prline, "%s: Will be started;\n", /* catgets  1466  */
		localTimeStr );
        printf(prline);
    }
    
    if (job->startTime && !IS_PEND(job->status)) {
	TIMEIT(1, prtJobStart(job, BJOBS_PRINT, job->jobPid, FALSE), "prtJobStart");
    }

    if ((cpuFactor > 0.0) && (job->cpuTime > 0)) 
        job->cpuTime = job->cpuTime * hostFactor / cpuFactor;

    if (job->jType == JGRP_NODE_ARRAY) {
        
        printf("\n %s:\n", 
	    "COUNTERS"); /* catgets  1467  */
        printf(" NJOBS PEND DONE RUN EXIT SSUSP USUSP PSUSP\n"); /* catgets  1468  */
        printf(" %5d %4d %3d %4d %4d %5d %5d %5d\n",
            job->counter[JGRP_COUNT_NJOBS],
            job->counter[JGRP_COUNT_PEND],
            job->counter[JGRP_COUNT_NDONE],
            job->counter[JGRP_COUNT_NRUN],
            job->counter[JGRP_COUNT_NEXIT],
            job->counter[JGRP_COUNT_NSSUSP],
            job->counter[JGRP_COUNT_NUSUSP],
            job->counter[JGRP_COUNT_NPSUSP]);
        return;
    }
    TIMEIT(1, prtJobFinish(job, jInfoH), "prtJobFinish");

    if (lsbMode_ & LSB_MODE_BATCH) {
	sprintf(prline, "\n %s:\n",
	    "SCHEDULING PARAMETERS"); /* catgets  1469  */
        printf(prline);
	if (printThresholds (job->loadSched, job->loadStop, NULL, NULL,
			     job->nIdx, lsInfo) < 0)
	    exit (-1);
    }

    printf ("\n RESOURCE REQUIREMENT DETAILS:\n");
    if (job->mergedResReq && job->mergedResReq[0] != '\0') {
        printf (" Combined: %s\n", job->mergedResReq);
        if (job->startTime && !IS_PEND(job->status)) {
            printf (" Effective: %s\n", job->effeResReq);
        } else {
            printf (" Effective: -\n");
        }

    } else {
        printf (" Combined: -\n Effective: -\n");
    } 

    return;
}

void
displayUF(struct jobInfoEnt *job, struct jobInfoHead *jInfoH, float cpuFactor,
        int numQueues, struct queueInfoEnt *queueInfo)
{
    char *hostPtr, *sp;
    char hostName[MAXHOSTNAMELEN];
    float hostFactor, *getFactor;
    static int first = TRUE;
    static struct lsInfo *lsInfo;
    char prline[MAXLINELEN];

    struct queueInfoEnt *qp;
    int  i;

    if (first) {
        first = FALSE;
        TIMEIT(0, (lsInfo = ls_info()), "ls_info");
        if (lsInfo == NULL) {
            ls_perror("ls_info");
            exit(-1);
        }
    }

    prtHeaderUF(job, TRUE, FALSE);
    prtJobSubmitUF(job, FALSE, FALSE);
    TIMEIT(1, prtFileNamesUF(job, TRUE), "prtFileNamesUF");


    hostPtr = job->submit.hostSpec;
    hostFactor = 1.0;



    if (job->numExHosts > 0
        && (strcmp (job->exHosts[0], LOST_AND_FOUND) != 0)
        && !IS_PEND (job->status)) {

        strcpy (hostName, job->exHosts[0]);

        if ((sp = strstr (hostName, "@")) != NULL) {
            *sp = '\0';

            if (strcmp(hostName, hostPtr) == 0) {
                if ((getFactor=getCpuFactor(hostName, TRUE)) == NULL) {
                    printf("\n");
                    fprintf(stderr,
                            "Cannot obtain execution host information: %s\n", ls_errmsg[lserrno]);
                    exit(-1);
                } else {
                    hostFactor = *getFactor;
                    hostPtr = job->exHosts[0];
                }
            }
        }
    }

    TIMEIT(1, prtSubDetailsUF(job, hostPtr, hostFactor), "prtSubDetailsUF");
    if (job->numExHosts > 0 && job->reserveTime > 0) {
        TIMEIT(1, prtJobReservUF(job), "prtJobReservUF");
        sprintf(prline, ";\n");
        printf("%s", prline);
    }


    if (job->predictedStartTime && IS_PEND(job->status)) {
        char localTimeStr[60];
        strcpy ( localTimeStr, _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->predictedStartTime));
        printf("%s: Will be started;\n", /* catgets  1466  */
               localTimeStr );
    }

    if (job->startTime && !IS_PEND(job->status)) {
        TIMEIT(1, prtJobStartUF(job, BJOBS_PRINT, job->jobPid, FALSE), "prtJobStartUF");
    }

    if ((cpuFactor > 0.0) && (job->cpuTime > 0))
        job->cpuTime = job->cpuTime * hostFactor / cpuFactor;

    if (job->jType == JGRP_NODE_ARRAY) {

        printf("\n %s:\n",
               "COUNTERS"); /* catgets  1467  */
        printf( " NJOBS PEND DONE RUN EXIT SSUSP USUSP PSUSP\n"); /* catgets  1468  */
        printf(" %5d %4d %3d %4d %4d %5d %5d %5d\n",
               job->counter[JGRP_COUNT_NJOBS],
               job->counter[JGRP_COUNT_PEND],
               job->counter[JGRP_COUNT_NDONE],
               job->counter[JGRP_COUNT_NRUN],
               job->counter[JGRP_COUNT_NEXIT],
               job->counter[JGRP_COUNT_NSSUSP],
               job->counter[JGRP_COUNT_NUSUSP],
               job->counter[JGRP_COUNT_NPSUSP]);
        return;
    }
    TIMEIT(1, prtJobFinishUF(job, jInfoH), "prtJobFinishUF");

    if (job->submit.rLimits[LSF_RLIMIT_RUN] < 0) {
        for (i=0; i<numQueues; i++) {
            qp = &(queueInfo[i]);
            if (strcmp(qp->queue, job->submit.queue) == 0)
                break;
        }
        job->submit.rLimits[LSF_RLIMIT_RUN] = qp->rLimits[LSF_RLIMIT_RUN];
    }
    prtResourceLimit(job->submit.rLimits, hostPtr, hostFactor, NULL);

    if (job->runRusage.mem > 0) {
        printf ("\n MEMORY USAGE:\n");
        if (job->runRusage.mem > 1024)
            printf(" MAX MEM: %d Mbytes;  AVG MEM: %d Mbytes\n", job->runRusage.mem/1024, job->runRusage.mem/1024);
        else
            printf(" MAX MEM: %d Kbytes;  AVG MEM: %d Kbytes\n", job->runRusage.mem, job->runRusage.mem);
    }

    if (lsbMode_ & LSB_MODE_BATCH) {
        printf("\n %s:\n",
               "SCHEDULING PARAMETERS"); /* catgets  1469  */
        if (printThresholds (job->loadSched, job->loadStop, NULL, NULL,
                             job->nIdx, lsInfo) < 0)
            exit (-1);
    }

    printf ("\n RESOURCE REQUIREMENT DETAILS:\n");
    if (job->mergedResReq && job->mergedResReq[0] != '\0') {
        printf (" Combined: %s\n", job->mergedResReq);
        if (job->startTime && !IS_PEND(job->status)) {
            printf (" Effective: %s\n", job->effeResReq);
        } else {
            printf (" Effective: -\n");    
        }

    } else {
        printf (" Combined: -\n Effective: -\n");
    } 

    return;
}

void
prtHeaderUF(struct jobInfoEnt *job, int prt_q, int tFormat)
{
    char prline[MAXLINELEN];

    if (!tFormat) {
        // in Openlava 4, it displays "Job Id" for bjobs -l and "Job" for -UF
        // in LSF, both -l and -UF display only "Job" with no "Id"
        sprintf(prline, "\n%s <%s>,", I18N_Job, lsb_jobid2str(job->jobId));

        printf("%s", prline);
        if (job->submit.options & SUB_JOB_NAME) {
            char *jobName, *pos;
            jobName = job->submit.jobName;
            if ((pos = strchr(jobName, '[')) && LSB_ARRAY_IDX(job->jobId)) {
                *pos = '\0';
                sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId));
            }
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,552, " Job Name <%s>,")), jobName); /* catgets  552  */
            printf("%s", prline);
        }
    }
    if (tFormat) {
        sprintf(prline, ",");
        printf("%s", prline);
    }
    sprintf(prline, " %s <%s>,", I18N_User, job->user);
    printf("%s", prline);

    if (lsbMode_ & LSB_MODE_BATCH) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,554, " Project <%s>,")), job->submit.projectName); /* catgets  554  */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_MAIL_USER) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,556, " Mail <%s>,")), job->submit.mailUser); /* catgets  556  */
        printf("%s", prline);
    }

    if (prt_q) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,557, " Status <%s>, Queue <%s>,")),  /* catgets  557  */
                get_status(job),
                job->submit.queue);
        printf("%s", prline);
    }


    if (job->submit.options & SUB_INTERACTIVE) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,558, " Interactive"))); /* catgets  558  */
        if (job->submit.options & SUB_PTY) {
            strcat(prline, (_i18n_msg_get(ls_catd,NL_SETN,559, " pseudo-terminal"))); /* catgets  559  */
            if (job->submit.options & (SUB_PTY_SHELL))
                strcat(prline, (_i18n_msg_get(ls_catd,NL_SETN,560, " shell"))); /* catgets  560  */
        }
        strcat(prline, (_i18n_msg_get(ls_catd,NL_SETN,561, " mode,"))); /* catgets  561  */
        printf("%s", prline);
    }


    if ( job->jobPriority > 0 ) {
        sprintf(prline, " %s <%d>,", I18N_Job_Priority, job->jobPriority);
        printf("%s", prline);
    }

    if (job->submit.options2 & (SUB2_JOB_CMD_SPOOL)) {
        sprintf(prline, " %s(Spooled) <%s>", I18N_Command, job->submit.command);
    } else {
        sprintf(prline, " %s <%s>", I18N_Command, job->submit.command);
    }
    printf("%s", prline);

    if (job->chargedSAAP && strlen(job->chargedSAAP) > 0) {
        if (tFormat) {
            sprintf(prline, ", %s <%s>", I18N_SAAP, job->chargedSAAP);
        } else {
            sprintf(prline, ", %s <%s>\n", I18N_SAAP, job->chargedSAAP);
        }
        printf("%s", prline);
    } else {
        if (!tFormat) {
            printf("\n");
        }
    }
}

void
prtJobSubmitUF(struct jobInfoEnt *job, int prt_q, int tFormat)
{
    char prline[MAXLINELEN];
    char *timestr;

    timestr = putstr_(_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->submitTime));
    if (tFormat) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,569, "%s: Job <%s> submitted from host <%s>")), /* catgets  569  */
                timestr, lsb_jobid2str(job->jobId), job->fromHost);
    } else {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,570, "%s: Submitted from host <%s>")), /* catgets  570  */
                timestr, job->fromHost);
    }

    FREEUP(timestr);
    printf("%s", prline);

    if (job->submit.options2 & SUB2_HOLD) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,570, " with hold")));
        /* catgets  570  */
        printf("%s", prline);
    }

    if (prt_q) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,571, ", to Queue <%s>")), job->submit.queue); /* catgets  571  */
        printf("%s", prline);
    }

    TIMEIT(2, prtBTTime(job), "prtBTTime");

}

void
prtSubDetailsUF(struct jobInfoEnt *job, char *hostPtr, float hostFactor)
{
    char prline[MSGSIZE];
    int  k;


    if ((job->submit.options & SUB_NOTIFY_END) &&
        (job->submit.options & SUB_NOTIFY_BEGIN)) {
        sprintf(prline, ", %s",
                I18N(581, "Notify when job begins/ends")); /* catgets 581  */
        printf("%s", prline);
    }
    else if (job->submit.options & SUB_NOTIFY_BEGIN) {
        sprintf(prline, ", %s",
                I18N(582, "Notify when job begins")); /* catgets 582  */
        printf("%s", prline);
    }
    else if (job->submit.options & SUB_NOTIFY_END) {
        sprintf(prline, ", %s",
                I18N(583, "Notify when job ends")); /* catgets 583 */
        printf("%s", prline);
    }



    if (job->submit.options & SUB_EXCLUSIVE) {
        sprintf(prline, ", %s",
                I18N(584, "Exclusive Execution")); /* catgets  584  */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_RERUNNABLE) {
        sprintf(prline, ", %s", I18N(585, "Re-runnable")); /* catgets  585  */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_RESTART) {
        if (job->submit.options & SUB_RESTART_FORCE)
            sprintf(prline, ", %s",
                    I18N(586, "Restart force")); /* catgets 586  */
        else
            sprintf(prline, ", %s", I18N_Restart);
        printf("%s", prline);
    }


    if (job->submit.options & SUB_CHKPNTABLE)  {
        if (job->submit.chkpntPeriod) {
            sprintf (prline, (_i18n_msg_get(ls_catd,NL_SETN,588, ", Checkpoint period %d min.")), /* catgets  588  */
                     (int) job->submit.chkpntPeriod/60);
            printf("%s", prline);
        }

        sprintf(prline, ", %s <%s>",
                I18N(589, "Checkpoint directory"), /* catgets  589  */
                job->submit.chkpntDir);
        printf("%s", prline);
    }


    if (job->submit.numProcessors > 1
        || job->submit.maxNumProcessors > 1) {
        if (job->submit.numProcessors == job->submit.maxNumProcessors)
            sprintf(prline, ", %d %s",
                    job->submit.numProcessors,
                    I18N(590, "Task(s)"));  /* catgets  590  */
        else
            sprintf(prline, ", %d-%d %s", job->submit.numProcessors,
                    job->submit.maxNumProcessors,
                    I18N(590, "Task(s)"));
        printf("%s", prline);
    }



    if (strlen(job->submit.resReq)) {
        sprintf(prline, ", %s <%s>",
                I18N(592, "Requested Resources"), /* catgets  592  */
                job->submit.resReq);
        printf("%s", prline);
    }


    if (strlen(job->submit.dependCond)) {
        char *temp;
        if ((temp = (char *)malloc (strlen(job->submit.dependCond) + 30))
            == NULL) {
            perror("malloc");
            exit(-1);
        } else {
            sprintf(temp, ", %s <%s>",
                    I18N(594, "Dependency Condition"), /* catgets  594  */
                    job->submit.dependCond);
            printf("%s", temp);
            free (temp);
        }
    }

    if (strlen(job->submit.loginShell)) {
        sprintf(prline, ", %s <%s>",
                I18N(597, "Login Shell"),  /* catgets  597  */
                job->submit.loginShell);
        printf("%s", prline);
    }


    if (strlen(job->submit.preExecCmd)) {
        sprintf(prline, ", %s <%s>",
                I18N(598, "Pre-execute Command"),  /* catgets  598  */
                job->submit.preExecCmd);
        printf("%s", prline);
    }


    if (job->submit.numAskedHosts) {
        sprintf(prline, ", %s <%s>",
                I18N(599, "Specified Hosts"),   /* catgets  599  */
                job->submit.askedHosts[0]);
        printf("%s", prline);
        for (k = 1; k < job->submit.numAskedHosts; k++) {
            sprintf(prline, ", <%s>", job->submit.askedHosts[k]);
            printf("%s", prline);
        }
    }


    if (job->submit.options & SUB_WINDOW_SIG) {
        sprintf(prline, ", %s <%d>",
                I18N(601, "Signal Value"),  /* catgets  601  */
                sig_decode(job->submit.sigValue));
        printf("%s", prline);
    }

    if ( job->submit.options2 & SUB2_JOB_PRIORITY && job->submit.userPriority > 0) {
        sprintf(prline, ", %s <%d>",
                I18N_User_Priority, job->submit.userPriority);
        printf("%s", prline);
    }

    sprintf(prline, ";\n");
    printf("%s", prline);
}

void
prtJobStartUF(struct jobInfoEnt *job, int prtFlag, int jobPid, int tFormat)
{
    char prline[MAXLINELEN], tBuff[20];
    time_t startTime;



    int                 i = 0;
    NAMELIST  *hostList = NULL;


    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
        &&  strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {
            exit(99);
        }
    }


    if (tFormat) {
        sprintf (tBuff, "%s <%s>", I18N_Job, lsb_jobid2str(job->jobId));
    }
    else if (LSB_ARRAY_IDX(job->jobId) > 0 )
        sprintf (tBuff, " [%d]", LSB_ARRAY_IDX(job->jobId));
    else
        tBuff[0] = '\0';

    if (job->startTime && job->numExHosts) {


        if (job->startTime < job->submitTime)
            startTime = job->submitTime;
        else
            startTime = job->startTime;

        if ((job->submit.options & SUB_PRE_EXEC)
            && (prtFlag != BJOBS_PRINT)) {
            if (prtFlag == BHIST_PRINT_PRE_EXEC)
            {
                if (tBuff[0] == '\0')
                    sprintf(prline, "%s: %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
                                        &startTime),
                            I18N(604, "The pre-exec command is started on")); /* catgets  604  */
                else
                    sprintf(prline, "%s:%s, %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
                                        &startTime),
                            tBuff,
                            I18N(605, "the pre-exec command is started on")); /* catgets  605  */
            }
            else
            {
                if (tBuff[0] == '\0')
                    sprintf(prline, "%s: %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
                            I18N(606, "The batch job command is started on")); /*catgets 606 */
                else
                    sprintf(prline, "%s:%s, %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
                            tBuff,
                            I18N(607, "the batch job command is started on")); /*catgets 607 */
            }
        } else {
            if (jobPid > 0)
            {
                if (tBuff[0] == '\0')
                    sprintf(prline, "%s: %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
                            I18N(608, "Started")); /* catgets  608  */
                else
                    sprintf(prline, "%s:%s %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
                            tBuff,
                            I18N(609, "started")); /* catgets  609  */
            }
            else
            {
                if (tBuff[0] == '\0')
                    sprintf(prline, "%s: %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
                            I18N(610, "Dispatched to")); /* catgets 610 */
                else
                    sprintf(prline, "%s: %s %s",
                            _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &startTime),
                            tBuff,
                            I18N(611, "dispatched to")); /* catgets  611 */
            }
        }


        printf("%s", prline);
        sprintf(prline, " %d %s",
                job->numExHosts,
                I18N(612, "Task(s) on Host(s)")); /* catgets  612  */
        printf("%s", prline);

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
            for (i = 0; i < hostList->listSize; i++) {
                sprintf(prline, " <%d*%s>", hostList->counter[i],
                        hostList->names[i]);
                printf("%s", prline);
            }
        } else {
            for (i = 0; i < job->numExHosts; i++) {
                sprintf(prline, " <%s>", job->exHosts[i]);
                printf("%s", prline);
            }
        }


        sprintf(prline, ", Allocated %d %s",
                job->numExHosts,
                I18N(612, "Slot(s) on Host(s)")); /* catgets  612  */
        printf("%s", prline);

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
            for (i = 0; i < hostList->listSize; i++) {
                sprintf(prline, " <%d*%s>", hostList->counter[i],
                        hostList->names[i]);
                printf("%s", prline);
            }
        } else {
            for (i = 0; i < job->numExHosts; i++) {
                sprintf(prline, " <%s>", job->exHosts[i]);
                printf("%s", prline);
            }
        }


        if (job->execHome && strcmp (job->execHome, "")) {
            sprintf(prline, ", %s <%s>",
                    I18N(615, "Execution Home"),  /* catgets 615 */
                    job->execHome);
            printf("%s", prline);
        }
        if (job->execCwd && strcmp (job->execCwd, "")) {
            sprintf(prline, ", %s <%s>",
                    I18N(616, "Execution CWD"), /* catgets 616 */
                    job->execCwd);
            printf("%s", prline);
        }
        if (job->execUsername && strcmp(job->execUsername, "") &&
            strcmp(job->user, job->execUsername)) {
            sprintf(prline, ", %s <%s>",
                    I18N(617, "Execution user name"), /* catgets 617 */
                    job->execUsername);
            printf("%s", prline);
        }
        sprintf(prline, ";\n");
        printf("%s", prline);
    }
}

void
prtJobReservUF(struct jobInfoEnt *job)
{
    char prline[MAXLINELEN];


    int                 i = 0;
    NAMELIST  *hostList = NULL;

    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
        && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {
            exit(99);
        }
    }


    if (job->numExHosts > 0 && job->reserveTime > 0) {
        if (job->numExHosts > 1)
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,618, "%s: Reserved <%d> job slots on host(s)")), /* catgets  618  */
                    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->reserveTime),
                    job->numExHosts);
        else
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,619, "%s: Reserved <%d> job slot on host")), /* catgets  619  */
                    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->reserveTime),
                    job->numExHosts);
        printf("%s", prline);

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0) {
            for (i = 0; i < hostList->listSize; i++) {
                sprintf(prline, " <%d*%s>", hostList->counter[i],
                        hostList->names[i]);
                printf("%s", prline);
            }
        } else {
            for (i = 0; i < job->numExHosts; i++) {
                sprintf(prline, " <%s>", job->exHosts[i]);
                printf("%s", prline);
            }
        }

    }

}

void
prtJobRusageUF(struct jobInfoEnt *job)
{
    char prline[MAXLINELEN];

    int i, j;



    if (IS_FINISH(job->status))
        return;



    if (IS_PEND(job->status)) {
        if (job->runRusage.utime || job->runRusage.stime) {
            sprintf(prline, "%s: %s. ",
                    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
                                &job->jRusageUpdateTime),
                    I18N(644, "Resource usage collected")); /* catgets 644  */
            printf("%s", prline);
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,645, "The CPU time used is %d seconds.")),  /* catgets  645  */
                    job->runRusage.utime + job->runRusage.stime);
            printf("%s", prline);
        }
        return;
    };




    if (job->runRusage.utime > 0 || job->runRusage.stime > 0
        || job->runRusage.mem > 0 || job->runRusage.swap > 0
        || job->runRusage.npgids > 0 || job->runRusage.npids > 0) {
        sprintf(prline, "%s: %s.",
                _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
                            &job->jRusageUpdateTime),
                I18N(646, "Resource usage collected")); /* catgets  646  */
        printf("%s", prline);
    } else
        return;

    if (job->runRusage.utime > 0 || job->runRusage.stime > 0) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,647, " The CPU time used is %d seconds.")), /* catgets  647  */
                job->runRusage.utime + job->runRusage.stime);
        printf("%s", prline);
    }


    if (job->runRusage.mem > 0) {
        if (job->runRusage.mem > 1024)
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,648, " MEM: %d Mbytes")), job->runRusage.mem/1024); /* catgets  648  */
        else
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,649, " MEM: %d Kbytes")), job->runRusage.mem); /* catgets  649  */
        printf("%s", prline);
    }

    if (job->runRusage.swap > 0) {
        if (job->runRusage.swap > 1024)
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,650, " SWAP: %d Mbytes;")), /* catgets  650  */
                    job->runRusage.swap/1024);
        else
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,651, " SWAP: %d Kbytes;")), job->runRusage.swap); /* catgets  651  */
        printf("%s", prline);
    }

    if (job->runRusage.npgids <= 0)
        return;


    for (i=0; i < job->runRusage.npgids; i++) {
        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,652, " PGID: %d; ")), job->runRusage.pgid[i]); /* catgets  652  */
        printf("%s", prline);

        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,653, "PIDs: "))); /* catgets  653  */
        printf("%s", prline);

        for (j=0; j < job->runRusage.npids; j++) {
            if (job->runRusage.pgid[i] == job->runRusage.pidInfo[j].pgid) {
                sprintf(prline, "%d ", job->runRusage.pidInfo[j].pid);
                printf ("%d%s", job->runRusage.pidInfo[j].pid, j==job->runRusage.npids-1?"":" ");
            }
        }
        printf(";");
    }
    sprintf(prline, "\n");
    printf("%s", prline);

}

void
prtJobFinishUF(struct jobInfoEnt *job, struct jobInfoHead *jInfoH)
{
    char prline[MSGSIZE];
    time_t doneTime;
    static struct loadIndexLog *loadIndex = NULL;
    char *pendReasons;

    if (loadIndex == NULL)
    TIMEIT(1, loadIndex = initLoadIndex(), "initLoadIndex");

    doneTime = job->endTime;

    switch (job->status) {
        case JOB_STAT_DONE:
        case (JOB_STAT_DONE | JOB_STAT_PDONE):
        case (JOB_STAT_DONE | JOB_STAT_PERR):

            if ((job->startTime < job->submitTime)
                && (job->endTime < (job->submitTime
                                    + (time_t) MAX(job->cpuTime, MIN_CPU_TIME)))) {
                doneTime = job->submitTime +
                           (time_t) MAX(job->cpuTime, 0.0001);
            } else if (job->startTime >= job->submitTime &&
                       job->endTime < (job->startTime +
                                       (time_t)MAX(job->cpuTime, 0.0001)) &&
                       job->numExHosts == 1) {

                doneTime = job->startTime + (time_t) MAX(job->cpuTime, 0.0001);

                if (job->endTime <= doneTime) {
                    doneTime = job->endTime;
                }


            }

        case (JOB_STAT_EXIT | JOB_STAT_PDONE):
        case (JOB_STAT_EXIT | JOB_STAT_PERR):
        case JOB_STAT_EXIT:
            if (job->reasons & EXIT_ZOMBIE) {
                sprintf(prline, "%s: ",
                        _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &job->endTime));
                printf("%s", prline);
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,623, "Termination request issued; the job will be killed once the host is ok;"))); /* catgets  623  */
                printf("%s", prline);
                break;
            }
            sprintf(prline, "%s: ",
                    _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T, &doneTime));
            printf("%s", prline);
            if (strcmp(get_status(job), "DONE") == 0)
            {
                sprintf(prline, I18N(624, "Done successfully.")); /* catgets 624 */
            }
            else {
                LS_WAIT_T wStatus;

                LS_STATUS(wStatus) = job->exitStatus;

                if (job->cpuTime >= MIN_CPU_TIME && job->exitStatus) {
                    if (WEXITSTATUS(wStatus))
                        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,626, "Exited with exit code %d.")), /* catgets  626  */
                                WEXITSTATUS(wStatus));
                    else
                        sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,627, "Exited by signal %d.")), WTERMSIG(wStatus)); /* catgets  627  */
                } else
                    sprintf(prline, I18N_Exited);
            }

            printf("%s", prline);

            if (job->numExHosts > 0) {
                if (job->cpuTime < MIN_CPU_TIME)
                    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,629, " The CPU time used is unknown.\n"))); /* catgets  629  */
                else
                    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,630, " The CPU time used is %1.1f seconds.\n")),  /* catgets  630  */
                            job->cpuTime);
            } else {
                sprintf(prline, "\n");
            }

            printf("%s", prline);
            break;
        case JOB_STAT_PSUSP:
        case JOB_STAT_PEND:
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,631, " PENDING REASONS:\n"))); /* catgets  631  */
            printf("%s", prline);
            pendReasons = lsb_pendreason(job->numReasons, job->reasonTb,
                                         jInfoH, loadIndex);
            printf("%s", pendReasons);
            break;
        case JOB_STAT_SSUSP:
        case JOB_STAT_USUSP:


        TIMEIT(1, prtJobRusageUF(job), "prtJobRusageUF");

            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,632, " SUSPENDING REASONS:\n"))); /* catgets  632  */
            printf("%s", prline);

            if (job->reasons) {
                sprintf(prline, "%s", lsb_suspreason(job->reasons,
                                                     job->subreasons,
                                                     loadIndex));
                printf("%s", prline);
            }
            break;
        case JOB_STAT_RUN:

        TIMEIT(1, prtJobRusageUF(job), "prtJobRusageUF");
            break;
        default:
            break;
    }
}

void
prtFileNamesUF(struct jobInfoEnt *job, int prtCwd)
{
    char prline[MAXLINELEN];
    int i;


    if (prtCwd == TRUE) {
        if (job->cwd[0] == '/' || job->cwd[0] == '\\' ||
            (job->cwd[0] != '\0' && job->cwd[1] == ':'))
            sprintf(prline, ", CWD <%s>", job->cwd);
        else if (job->cwd[0] == '\0')
            sprintf(prline, ", CWD <$HOME>");
        else
            sprintf(prline, ", CWD <$HOME/%s>", job->cwd);
        printf("%s", prline);
    }


    if (job->submit.options & SUB_IN_FILE)  {
        sprintf(prline, ", %s <%s>",
                I18N(577, "Input File"), job->submit.inFile);/* catgets 577 */
        printf("%s", prline);
    }


    if (job->submit.options2 & SUB2_IN_FILE_SPOOL)  {
        sprintf(prline, ", %s(Spooled) <%s>",
                I18N(577, "Input File"), job->submit.inFile);/* catgets 577 */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_OUT_FILE) {
        sprintf(prline, ", %s <%s>",
                I18N(578, "Output File"), job->submit.outFile);/* catgets 578 */
        printf("%s", prline);
    }


    if (job->submit.options & SUB_ERR_FILE) {
        sprintf(prline, ", %s <%s>",
                I18N(579, "Error File"), job->submit.errFile); /* catgets 579 */
        printf("%s", prline);
    }


    if (job->submit.nxf) {
        sprintf(prline, ", %s",  I18N(580, "Copy Files")); /* catgets 580 */
        printf("%s", prline);
    }

    for (i = 0; i < job->submit.nxf; i++) {
        sprintf(prline, " \"%s %s %s\"", job->submit.xf[i].subFn,
                strXfOptions(job->submit.xf[i].options),
                job->submit.xf[i].execFn);
        printf("%s", prline);
    }

}

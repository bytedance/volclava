/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: sbd.policy.c 397 2007-11-26 19:04:00Z mblack $
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

#define NL_SETN		11	

#include <malloc.h>
#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
#endif

static void tryResume (struct hostLoad *myload);
static void tryStop (char *myhostnm, struct hostLoad *myload);
void jobSuspendAction (struct jobCard *jp, int sigValue, int suspReasosns, int suspSubReasons);
static int shouldStop (struct hostLoad *loadV, struct jobCard *jobCard, int *reasons, int *subreasons, int num, int *stopmore);
static int shouldStop1 (struct hostLoad *);
static int shouldResume (struct hostLoad *, struct jobCard *, int);
static int jobResumeAction (struct jobCard *jp, int sigValue, int suspReason);
static void tryChkpntMig(void);

static int rmJobBufFilesPid(struct jobCard *);
static int cleanupMigJob(struct jobCard *);

static int getTclHostData (struct hostLoad *, struct tclHostData *, int);
static void ruLimits(struct jobCard *);

static void sigActEnd (struct jobCard *jobCard);
static void chkpntEnd (struct jobCard *, int, bool_t *);
static void initTclHostData (struct tclHostData *);

void suspendActEnd (struct jobCard *jobCard, int w_status);
void resumeActEnd (struct jobCard *jobCard, int w_status);
extern char * exitFileSuffix ( int sigValue);
extern int sbdlog_newstatus (struct jobCard *jp);
extern int jobSigLog (struct jobCard *jp, int finishStatus);

extern int lsbMemEnforce;
extern int lsbJobMemLimit;
extern int lsbJobCpuLimit;
void 
job_checking (void)
{
    static char fname[] = "job_checking";
    struct jobCard *jobCard, *nextJob;
    struct hostLoad *myload, savedLoad;
    char *myhostnm;
    static time_t last_check;    
    static time_t last_preem;    
    char preempted = FALSE;
    int i, cc;

    

    if (last_check == 0)
	last_check = now;                             
    if (jobcnt <= 0) {                                
	last_preem = 0;                               
        last_check = now;
        return;
    }

    checkFinish ();    

    
    for (jobCard = jobQueHead->forw; (jobCard != jobQueHead); 
					 jobCard = nextJob) {
        ls_syslog(LOG_DEBUG, "job_checking: jobCard job %d with postExecCmd=%s", jobCard->jobSpecs.jobId, jobCard->jobSpecs.postExecCmd);

	nextJob = jobCard->forw;         
        if (IS_FINISH(jobCard->jobSpecs.jStatus)
              || (jobCard->jobSpecs.jStatus & JOB_STAT_PEND))
            continue;

	ruLimits(jobCard);

         

        
	if (IS_RUN_JOB_CMD(jobCard->jobSpecs.jStatus)) {
	    
	    jobCard->runTime += (int) (now - last_check);
	}
	if (jobCard->runTime > 
	    jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_RUN].rlim_curl) {
            if ((jobCard->jobSpecs.terminateActCmd == NULL) 
                || (jobCard->jobSpecs.terminateActCmd[0] == '\0')) { 
	        if (jobCard->runTime > 
		    jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_RUN].rlim_curl 
		    + WARN_TIME && jobCard->timeExpire) {

		    
                    if ((IS_SUSP (jobCard->jobSpecs.jStatus))
                       && (jobCard->jobSpecs.reasons & SUSP_RES_LIMIT)
                       && (jobCard->jobSpecs.subreasons & SUB_REASON_RUNLIMIT))
                        continue; 
		    else if (jobCard->jobSpecs.jStatus & JOB_STAT_KILL)
			continue;
                    else {
                        
                        ls_syslog(LOG_INFO,I18N(5703,
                            "%s: warning period expired killing the job=%d"), /* catgets 5703 */
			    fname, jobCard->jobSpecs.jobId);
                        jobSigStart (jobCard, SIG_TERM_RUNLIMIT, 0, 0, SIGLOG);
                        sbdlog_newstatus(jobCard); 
			jobCard->jobSpecs.jStatus |= JOB_STAT_KILL;
                    }
	        } else if (!jobCard->timeExpire) {     
		    ls_syslog(LOG_INFO, I18N(5704,
                        "%s: sending warning signal to job=%d"), /* catgets 5704 */
			fname, jobCard->jobSpecs.jobId);
		    jobsig(jobCard, SIGUSR2, FALSE);
		    jobCard->timeExpire = TRUE;
	        }
            } else { 
                if (jobCard->runTime >
                    jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_RUN].rlim_curl) {

                    
                    if ((IS_SUSP (jobCard->jobSpecs.jStatus))
                       && (jobCard->jobSpecs.reasons & SUSP_RES_LIMIT)
                       && (jobCard->jobSpecs.subreasons & SUB_REASON_RUNLIMIT))
                        continue; 
                    else {
                        jobSigStart (jobCard, SIG_TERM_RUNLIMIT, 0, 0, SIGLOG);
                        sbdlog_newstatus(jobCard); 
                    }
                }  
            }
	    continue;                           
	}

	
        if (jobCard->jobSpecs.termTime && now > jobCard->jobSpecs.termTime 
        
             && !(jobCard->jobSpecs.jAttrib & JOB_FORCE_KILL)) {
            if ((jobCard->jobSpecs.terminateActCmd == NULL) 
                 || (jobCard->jobSpecs.terminateActCmd[0] == '\0')) { 
                if (now > jobCard->jobSpecs.termTime + WARN_TIME 
                                                   && jobCard->timeExpire) {
                    
                    if ((IS_SUSP (jobCard->jobSpecs.jStatus))
                       && (jobCard->jobSpecs.reasons & SUSP_RES_LIMIT)
                       && (jobCard->jobSpecs.subreasons & SUB_REASON_DEADLINE))
                        continue; 
		    else if (jobCard->jobSpecs.jStatus & JOB_STAT_KILL)
			continue; 
                    else {
                        
                        jobSigStart (jobCard, SIG_TERM_DEADLINE, 0, 0, SIGLOG);
                        sbdlog_newstatus(jobCard); 
			jobCard->jobSpecs.jStatus |= JOB_STAT_KILL;
                    }
                } else
		    if (!jobCard->timeExpire) {        
		        jobsig(jobCard, SIGUSR2, FALSE);
		        jobCard->timeExpire = TRUE;
		    }
            } else { 
                if (now > jobCard->jobSpecs.termTime) {
                    
                    if ((IS_SUSP (jobCard->jobSpecs.jStatus))
                       && (jobCard->jobSpecs.reasons & SUSP_RES_LIMIT)
                       && (jobCard->jobSpecs.subreasons & SUB_REASON_DEADLINE))
                        continue; 
                    else {
                        jobSigStart (jobCard, SIG_TERM_DEADLINE, 0, 0, SIGLOG);
                        sbdlog_newstatus(jobCard); 
                    }
                }  
            }
            continue;                               
        }

	
        if (   ! window_ok (jobCard) 
	    && !(jobCard->jobSpecs.jAttrib & JOB_URGENT_NOSTOP)) 
        {
	    if (! (jobCard->jobSpecs.options & SUB_WINDOW_SIG) 
                || ((jobCard->jobSpecs.options & SUB_WINDOW_SIG) 
                          && now - jobCard->windWarnTime >= WARN_TIME)) {

                
	        jobSuspendAction(jobCard, SIG_SUSP_WINDOW, SUSP_QUEUE_WINDOW, 0);
		continue;

	    } 
	} else {   
                

		cc = jobResumeAction(jobCard, SIG_RESUME_WINDOW, SUSP_QUEUE_WINDOW);
                continue;
	}
    } 

    
    if ((myhostnm = ls_getmyhostname()) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_getmyhostname");
        die(SLAVE_FATAL);
    }
    myload = ls_loadofhosts (NULL, 0, EXACT|EFFECTIVE, 0, &myhostnm, 1);
    if (myload == NULL) {
        if (myStatus != NO_LIM)
	    
	    ls_syslog(LOG_INFO, I18N_FUNC_FAIL_MM, fname, "ls_loadofhosts");
	if (lserrno == LSE_LIM_BADHOST)
	    relife(fname, "LSE_LIM_BADHOST");
	if (lserrno == LSE_BAD_XDR)
	    relife(fname, "LSE_BAD_XDR");
	if (lserrno == LSE_LIM_DOWN || lserrno == LSE_TIME_OUT) {
	    myStatus |= NO_LIM;

            
            tryChkpntMig(); 
        }
        last_check = now;
	return;
    } else
	myStatus = 0;

    

    memcpy ((char *)&savedLoad, (char *)myload, sizeof (struct hostLoad));
    savedLoad.li = (float *) my_malloc (allLsInfo->numIndx * sizeof (float),
				   "job_checking");
    savedLoad.status = (int *) my_malloc 
       ((1 + GET_INTNUM(allLsInfo->numIndx)) * sizeof (int), "job_checking");
    for (i = 0; i < allLsInfo->numIndx; i++) 
        savedLoad.li[i] = myload->li[i];
    for (i = 0; i < 1 + GET_INTNUM(allLsInfo->numIndx); i++) 
        savedLoad.status[i] = myload->status[i];
    tryResume (&savedLoad);                       

    if (!preempted)
        tryStop (myhostnm, &savedLoad);            

    tryChkpntMig();

   
    FREEUP(savedLoad.li);
    FREEUP(savedLoad.status);
    last_check = now;
    return;

}

static void
tryChkpntMig(void)
{
    char migrating = FALSE;
    struct jobCard *jobCard, *nextJob;
    
    
    for (jobCard = jobQueHead->forw; (jobCard != jobQueHead);
	 jobCard = jobCard->forw) {
	if (jobCard->jobSpecs.jStatus & JOB_STAT_MIG) {
	    migrating = TRUE;
	    break;
	}
    }
    
    for (jobCard = jobQueHead->forw; jobCard != jobQueHead; 
                                     jobCard = nextJob) {
        nextJob = jobCard->forw;

	if (jobCard->missing)
	    continue;
	
	
	if ((jobCard->jobSpecs.jStatus & JOB_STAT_SSUSP)
	    && !migrating
	    && !(jobCard->jobSpecs.jStatus & JOB_STAT_MIG)
	    && jobCard->jobSpecs.actPid == 0
	    && (jobCard->jobSpecs.options & (SUB_CHKPNTABLE | SUB_RERUNNABLE))
	    && (now - jobCard->jobSpecs.lastSSuspTime 
                > jobCard->jobSpecs.migThresh) 
	    && (now - jobCard->lastChkpntTime 
                > jobCard->migCnt * sbdSleepTime)
            && !(jobCard->jobSpecs.reasons & SUSP_QUEUE_WINDOW))
        {
            
	    if (jobSigStart (jobCard, SIG_CHKPNT, LSB_CHKPNT_KILL,
                             jobCard->jobSpecs.chkPeriod, SIGLOG) == 0) {
		jobCard->jobSpecs.jStatus |= JOB_STAT_MIG;
		migrating = TRUE;
                sbdlog_newstatus(jobCard); 
		continue;
	    }
	}

        
        if (!(jobCard->jobSpecs.jStatus & JOB_STAT_MIG) &&
	    (jobCard->jobSpecs.jStatus & JOB_STAT_RUN) &&
	    jobCard->jobSpecs.actPid == 0 && 
	    jobCard->jobSpecs.chkPeriod &&
	    now - jobCard->lastChkpntTime > jobCard->jobSpecs.chkPeriod) {
            
            
            if (jobSigStart (jobCard, SIG_CHKPNT, 0,
                             jobCard->jobSpecs.chkPeriod, SIGLOG) == 0) {
                sbdlog_newstatus(jobCard); 
                continue;
            }
            
        }
    }
} 
	
static void
tryResume (struct hostLoad *myload)
{
    char fname[] = "tryResume";
    struct jobCard *jobCard, *next;

    static int errCount = 0, lastTryResumeTime = 0;


    if (now - lastTryResumeTime < sbdSleepTime) {
        
        return;
    }
    lastTryResumeTime = now;

    for (jobCard = jobQueHead->back; jobCard != jobQueHead; jobCard = next) {
        next = jobCard->back;

	if (!(jobCard->jobSpecs.jStatus & JOB_STAT_SSUSP) ||
	    jobCard->jobSpecs.actPid)
            continue;

        if (jobCard->jobSpecs.numToHosts == 1) {  
            if (shouldResume (myload, jobCard, 1)) {
                

                if (jobResumeAction(jobCard, SIG_RESUME_LOAD, LOAD_REASONS) < 0)
                    continue;
                else
                    return;          
            }
	} else {   
            int numh;
	    struct hostLoad *load;
            NAMELIST *hostList;

	    numh = jobCard->jobSpecs.numToHosts;
            hostList = lsb_compressStrList(jobCard->jobSpecs.toHosts, numh);
            numh = hostList->listSize;
            load = ls_loadofhosts ("-", &numh, EFFECTIVE, 0,
                                   hostList->names, hostList->listSize);

	    if (load == NULL) {
                if (errCount < 3)      
		    ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
			      lsb_jobid2str(jobCard->jobSpecs.jobId), 
			      "ls_loadofhosts");
                errCount++;
		if (lserrno == LSE_LIM_BADHOST)
		    relife(fname, "LSE_LIM_BADHOST");
		if (lserrno == LSE_BAD_XDR)
		    relife(fname, "LSE_BAD_XDR");
		if (lserrno == LSE_LIM_DOWN || lserrno == LSE_TIME_OUT)
		    myStatus |= NO_LIM;
		continue;
	    } else {
		myStatus = 0;
                errCount = 0;
            }
            if (!shouldResume (load, jobCard, numh))
                continue;

            

	    if (jobResumeAction(jobCard, SIG_RESUME_LOAD, LOAD_REASONS) < 0)      
                
		continue;               
	    else
	        return;                 
        }
    } 
    return;

} 

static void
tryStop (char *myhostnm, struct hostLoad *myload)
{
    static char fname[] = "tryStop";
    struct jobCard *jobCard, *next;
    int reasons, subreasons, stopmore = FALSE;
    static int errCount = 0, lastTryStopTime = 0;

    if (now - lastTryStopTime < sbdSleepTime) {
        
        return;
    }
    lastTryStopTime = now;

    for (jobCard = jobQueHead->forw; jobCard != jobQueHead; jobCard = next) {
        next = jobCard->forw;	
	
	if (jobCard->jobSpecs.numToHosts == 1) {            
        
            if  ((jobCard->jobSpecs.jStatus & JOB_STAT_RUN) 
                && (now >= jobCard->jobSpecs.startTime + sbdSleepTime)
                && shouldStop (myload, jobCard, &reasons, &subreasons, 1, &stopmore)) {

                
    	        jobSuspendAction (jobCard, SIG_SUSP_LOAD, reasons, subreasons);
	        if (stopmore)
                    continue;        
                else
	            return;         
	    }
	} else {     
	    struct hostLoad *load;
            int numh;
            NAMELIST *hostList;
            
            numh = jobCard->jobSpecs.numToHosts;
            hostList = lsb_compressStrList(jobCard->jobSpecs.toHosts, numh);
            numh = hostList->listSize;

	   
            
           if (hostList->listSize == 1) {
               load = myload;
           } else {
                load = ls_loadofhosts ("-", &numh, EFFECTIVE, 0,
                                    hostList->names, hostList->listSize);
           }

	    if (load == NULL) {
                if (errCount < 3)             
		    ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_MM, fname, 
			      lsb_jobid2str(jobCard->jobSpecs.jobId), 
			      "ls_loadofhosts");
		errCount++;
                if (lserrno == LSE_LIM_BADHOST)
                    relife(fname, "LSE_LIM_BADHOST");
		if (lserrno == LSE_BAD_XDR)
	            relife(fname, "LSE_BAD_XDR");
		if (lserrno == LSE_LIM_DOWN || lserrno == LSE_TIME_OUT)
		    myStatus |= NO_LIM;
		continue;
	    } else {
                errCount = 0;
		myStatus = 0;
            }
            
	    if ((jobCard->jobSpecs.jStatus & JOB_STAT_RUN)
                && now >= jobCard->jobSpecs.startTime + sbdSleepTime) {
		if (shouldStop (load, jobCard, &reasons, &subreasons, numh, &stopmore)) {

                    jobSuspendAction (jobCard, SIG_SUSP_LOAD, reasons, subreasons);
                    if (stopmore)
		        break;                   
                    else
		        return;                  
		}
            }
        }
    } 
    return;

} 

static int 
shouldStop (struct hostLoad *loadV, 
	    struct jobCard *jobCard, int *reasons, int *subreasons, int num, int *stopmore)
{
    static char fname[] = "shouldStop";
    int i, numLoad = -1, j;
    struct hostLoad *load = NULL;
    static struct tclHostData tclHostData;
    static int first = TRUE;

    *reasons = 0;                 
    *subreasons = 0;

    
    if( jobCard->postJobStarted ) {
        return (FALSE);
    } 

    
    if (jobCard->jobSpecs.jAttrib & JOB_URGENT_NOSTOP)
	return (FALSE);

        
    if (now - jobCard->windWarnTime < sbdSleepTime)
        return FALSE;                      

    
    if (!JOB_STARTED(jobCard))
        return FALSE;

    
    if (LS_ISUNAVAIL(loadV->status))
	return FALSE;                                         
    if (num <= 0)
	return FALSE;


    for (i = 0; i <jobCard->jobSpecs.numToHosts && (*reasons) == 0; i++) {
        if (i > 0 && !strcmp (jobCard->jobSpecs.toHosts[i],
					     jobCard->jobSpecs.toHosts[i-1]))
            continue;
        numLoad++;
	load = NULL;
        for (j = 0; j < num; j ++) {
    	    if (equalHost_(jobCard->jobSpecs.toHosts[i], loadV[j].hostName)) {
	        load = &(loadV[j]);
	        break;
            }
        }
        if (load == NULL) {
	    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5705,
		"%s: Can not find load information for host <%s>"), fname, jobCard->jobSpecs.toHosts[i]); /* catgets 5705 */
            return FALSE;
        }
        if (LS_ISLOCKEDU(load->status) 
            && !(jobCard->jobSpecs.jAttrib & Q_ATTRIB_EXCLUSIVE)) {
            *reasons = SUSP_HOST_LOCK;   
            *stopmore = TRUE;           
        } 
	else if (LS_ISLOCKEDM(load->status)) {
            *reasons = SUSP_HOST_LOCK_MASTER;  
            *stopmore = TRUE;           
        }
        else if (load->li[IT] <= jobCard->jobSpecs.thresholds.loadStop[numLoad][IT]
            && load->li[IT] != -INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][IT] != -INFINIT_LOAD) {
	    *reasons |= SUSP_LOAD_REASON;
            *subreasons = IT;
            *stopmore = TRUE;      
        }
        else if (load->li[LS] >= 
			  jobCard->jobSpecs.thresholds.loadStop[numLoad][LS]
            && load->li[LS] != INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][LS] 
						      != INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON; 
            *subreasons = LS;
            *stopmore = TRUE;                    
        }
        else if (load->li[UT] >= 
			 jobCard->jobSpecs.thresholds.loadStop[numLoad][UT]
            && load->li[UT] != INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][UT] != 
							   INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON; 
            *subreasons = UT;
        }
        else if(load->li[PG] >= 
		      jobCard->jobSpecs.thresholds.loadStop[numLoad][PG]
            && load->li[PG] != INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][PG] 
						    != INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON; 
            *subreasons = PG;
        }
        else if(load->li[IO] >= 
		     jobCard->jobSpecs.thresholds.loadStop[numLoad][IO]
            && load->li[IO] != INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][IO] 
						      != INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON; 
            *subreasons = IO;
        }
        else if(load->li[MEM] 
			 <= jobCard->jobSpecs.thresholds.loadStop[numLoad][MEM]
            && load->li[MEM] != -INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][MEM] 
						      != -INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON;
            *subreasons = MEM;
        }
	
        else if(load->li[SWP] 
			 <= jobCard->jobSpecs.thresholds.loadStop[numLoad][SWP]
            && load->li[SWP] != -INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][SWP] 
						      != -INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON;
            *subreasons = SWP;
        }
        else if(load->li[TMP] 
			 <= jobCard->jobSpecs.thresholds.loadStop[numLoad][TMP]
            && load->li[TMP] != -INFINIT_LOAD
            && jobCard->jobSpecs.thresholds.loadStop[numLoad][TMP] 
						      != -INFINIT_LOAD) {
            *reasons |= SUSP_LOAD_REASON;
            *subreasons = TMP;
        }

        for (j = R15S; !(*reasons) && j <= R15M; j++) 
	    if ((load->li[j] != INFINIT_LOAD)
	        && (jobCard->jobSpecs.thresholds.loadStop[numLoad][j] 
							 != INFINIT_LOAD)
	        && (load->li[j]  
			>= jobCard->jobSpecs.thresholds.loadStop[numLoad][j])) {
	        *reasons |= SUSP_LOAD_REASON;
                *subreasons = j;
                break;
	    }
        

        for (j = MEM + 1; !(*reasons) &&
               j < MIN(allLsInfo->numIndx, jobCard->jobSpecs.thresholds.nIdx);
	              j++) {
            if (load->li[j] >= INFINIT_LOAD || load->li[j] <= -INFINIT_LOAD
                || jobCard->jobSpecs.thresholds.loadStop[numLoad][j] 
							 >= INFINIT_LOAD
                || jobCard->jobSpecs.thresholds.loadStop[numLoad][j] 
							 <= -INFINIT_LOAD) {
                continue;                        
            }
	    if (allLsInfo->resTable[j].orderType == INCR) {
	        if (load->li[j] 
		       >= jobCard->jobSpecs.thresholds.loadStop[numLoad][j]) {
		    *reasons |= SUSP_LOAD_REASON;
                    *subreasons = j;
		    break;
                }
	    } else {
	        if (load->li[j] 
		      <= jobCard->jobSpecs.thresholds.loadStop[numLoad][j]) {
		    *reasons |= SUSP_LOAD_REASON;
                    *subreasons = j;
		    break;
                }
	    }
        }
        
        if (!(*reasons) && jobCard->stopCondVal != NULL) {
            int returnCode;
            if (first == TRUE) {
                initTclHostData (&tclHostData);
                returnCode = getTclHostData (load, &tclHostData, FALSE);
                first = FALSE;
            } else {
                returnCode = getTclHostData (load, &tclHostData, TRUE);
            }
            if (returnCode >= 0 
		     && evalResReq (jobCard->stopCondVal->selectStr, 
    	       	        &tclHostData, DFT_FROMTYPE) == 1) {
        	*reasons |= SUSP_QUE_STOP_COND;
		break;
            }
        }
    }

    
    if (! (*reasons))
	return FALSE;

    
    if (LS_ISLOCKEDU(load->status) || LS_ISLOCKEDM(load->status)) { 
	return TRUE;
    } else if (shouldStop1 (load)) {                 
        if (logclass & (LC_SCHED | LC_EXEC))
            ls_syslog (LOG_DEBUG2, 
			"%s: Should stop job %s; reason=%x, subreasons=%d",
                        fname, lsb_jobid2str(jobCard->jobSpecs.jobId), 
			*reasons, *subreasons);

        return TRUE;
    }
    return FALSE;

} 

static int
shouldStop1 (struct hostLoad *loadV)
{
    int rcnt = 0;
    struct jobCard *jp;

    
    for (jp = jobQueHead->forw; jp != jobQueHead; jp = jp->forw) {
        if (!(jp->jobSpecs.jStatus & JOB_STAT_RUN)) 
            continue;
        rcnt++;        
    } 
    if (rcnt > 1)                                   
	return TRUE;

    
    if (loadV->li[IT] < 1.0)
	return TRUE;                 
    
    if (daemonParams[LSB_STOP_IGNORE_IT].paramValue != NULL) {
	return TRUE;
    }
    return FALSE;
    
} 

static int 
shouldResume (struct hostLoad *loadV, struct jobCard *jp, int num)
{
    static char fname[] = "shouldResume";
    int i, j, numHosts = -1;
    int resume = TRUE, found;
    int lastReasons = jp->jobSpecs.reasons;
    int lastSubreasons = jp->jobSpecs.subreasons;
    struct hostLoad *loads = NULL;
    struct tclHostData *tclHostData = NULL;
    
    if (logclass & (LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG3, "%s: job=%s; jStatus=%d; reasons=%x, subreasons=%d, numHosts=%d", fname, lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.jStatus, jp->jobSpecs.reasons, jp->jobSpecs.subreasons, num);    

    if (num <= 0)
        return FALSE;

    
    if (!(jp->jobSpecs.jStatus & JOB_STAT_SSUSP))
        return FALSE;

     
    
    if ((jp->jobSpecs.reasons & SUSP_QUEUE_WINDOW)
        || (jp->jobSpecs.reasons & SUSP_USER_STOP)
        || (jp->jobSpecs.reasons & SUSP_MBD_LOCK))
        return FALSE;
 

    

    loads = (struct hostLoad *) 
			my_malloc (num * sizeof (struct hostLoad), fname);
    if (jp->resumeCondVal != NULL) {
        tclHostData = (struct tclHostData *) 
		       my_malloc (num * sizeof (struct tclHostData), fname);
        for (i = 0; i < num; i++) {
            initTclHostData (&tclHostData[i]);
        }
    } else {
	tclHostData = NULL;
    }
    for (j = 0; j <jp->jobSpecs.numToHosts; j++) {
        if (j > 0 && !strcmp (jp->jobSpecs.toHosts[j], 
                                    jp->jobSpecs.toHosts[j-1]))
            continue;
        numHosts++;
        found = FALSE;
        for (i = 0; i < num; i++) {
            if (equalHost_(jp->jobSpecs.toHosts[j], loadV[i].hostName)) {
                loads[numHosts] = loadV[i];
                if (tclHostData != NULL) {
                    if (getTclHostData (&loadV[i], 
                                     &tclHostData[numHosts], FALSE) < 0) {
                        break;
                    }
                }
                found = TRUE;
                break;
            }
        }
        if (found != TRUE) {
             
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5706,
		"%s: Can not find load information for host <%s> to check resume condiftions for job <%s>"), fname, jp->jobSpecs.toHosts[j], lsb_jobid2str(jp->jobSpecs.jobId)); /* catgets 5706 */
            loads[numHosts].li = NULL;
            continue;
        }
    }
    if (numHosts >= 0) {
	numHosts++;
        resume = checkResumeByLoad (jp->jobSpecs.jobId, numHosts, 
               jp->jobSpecs.thresholds, loads, &jp->jobSpecs.reasons, 
               &jp->jobSpecs.subreasons, 
               jp->jobSpecs.jAttrib, jp->resumeCondVal, tclHostData);
        
        FREEUP (loads);
        if (tclHostData != NULL) { 
            for (i = 0; i < numHosts; i++)  {
                 FREEUP (tclHostData[i].resBitMaps); 
                 FREEUP (tclHostData[i].loadIndex);
            }
            FREEUP (tclHostData);
        }
    } else {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5707,
	    "%s: No valid load information is found for job <%s>"), fname, lsb_jobid2str(jp->jobSpecs.jobId)); /* catgets 5707 */
    }
    if ((logclass & (LC_SCHED | LC_EXEC)) && !resume)
        ls_syslog(LOG_DEBUG2, "%s: Can't resume job %s; reason=%x, subreasons=%d", fname, lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.reasons, jp->jobSpecs.subreasons);

    if (!resume) {
	
	
	if ((jp->jobSpecs.reasons != lastReasons ||
	     (jp->jobSpecs.reasons == lastReasons &&
	      jp->jobSpecs.subreasons != lastSubreasons)) &&
	    (now - jp->lastStatusMbdTime > rusageUpdateRate * sbdSleepTime))
	    jp->notReported++;
    }
	     
    return (resume);

} 


int 
job_resume (struct jobCard *jp)
{
    static char fname[] = "job_resume";
    int rep;

    if (jp->jobSpecs.actPid)
	return 0;

    if (jobsig(jp, SIGCONT, FALSE) < 0)      
        return -1;
     
    SBD_SET_STATE(jp, JOB_STAT_RUN);
    
    jp->jobSpecs.reasons = 0;               
    jp->jobSpecs.subreasons = 0;            
    rep = status_job (BATCH_STATUS_JOB, jp, jp->jobSpecs.jStatus,
		      ERR_NO_ERROR); 
    if (rep < 0) 
        jp->notReported++;
    else {
	if (jp->notReported > 0)
            jp->notReported = 0;
    }
    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: Resume job %s", 
                               fname, lsb_jobid2str(jp->jobSpecs.jobId));
    return 0;
} 


static int
jobResumeAction (struct jobCard *jp, int sigValue, int suspReason)
{
    static char fname[] = "jobResumeAction";


    if (jp->jobSpecs.reasons & SUSP_MBD_LOCK) {
        
        return (-1);  
    };

    
    if (jp->jobSpecs.actPid)
        return (0);

     
    if (!(jp->jobSpecs.reasons & suspReason))
        return (-1);

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: Try to resume job %s with the current reason %d and the triggered reason %d;",
                 fname, lsb_jobid2str(jp->jobSpecs.jobId), jp->jobSpecs.reasons, suspReason);
 
    if (jobSigStart(jp, sigValue, 0, 0, SIGLOG) < 0)
        if (jobsig(jp, 0, FALSE) < 0) { 
           
            SBD_SET_STATE(jp, JOB_STAT_EXIT);
            return (-1);
        }
    sbdlog_newstatus(jp); 
    return (0);

} 


void
jobSuspendAction (struct jobCard *jp, int sigValue, int suspReasons, int suspSubReasons)
{
    static char fname[] = "jobSuspendAction";
    int cc;

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: Suspend job %s; reasons=%x, subresons=%d, sigValue=%d, status=%x", 
		  fname, lsb_jobid2str(jp->jobSpecs.jobId), 
		  jp->jobSpecs.reasons, 
		  jp->jobSpecs.subreasons, sigValue, jp->jobSpecs.jStatus);

    
    jp->actReasons = suspReasons;
    jp->actSubReasons = suspSubReasons;


    
    if (!JOB_RUNNING(jp))
        return;

    
    if( jp->postJobStarted ) {
        return;
    } 

    if (IS_SUSP (jp->jobSpecs.jStatus)) {  
        if (jp->jobSpecs.reasons & suspReasons)
            return; 
        else if (jp->jobSpecs.sigMap[-sigValue] == 0)  
            return; 
    } 

    
    if ((jp->jobSpecs.actPid)
        && ((jp->jobSpecs.actValue == sigValue)
            || (jp->jobSpecs.actValue == (sigValue + jp->jobSpecs.sigMap[-sigValue]))))
    return; 

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: Call jobSigStart(sigValue =%d) to suspend job", fname, sigValue + jp->jobSpecs.sigMap[-(sigValue)]);
 
 
    
    cc = jobSigStart(jp, sigValue + jp->jobSpecs.sigMap[-(sigValue)],
                      0, 0, SIGLOG);
    sbdlog_newstatus(jp);  

} 


static void
sigActEnd (struct jobCard *jobCard)
{
    int w_status;
    struct stat st;
    bool_t freed = FALSE;

    char exitFile[MAXFILENAMELEN];

    
    if (jobCard->jobSpecs.actValue < 0) {
        sprintf(exitFile, "%s/.%s.%s.%s",
            LSTMPDIR, jobCard->jobSpecs.jobFile,
                    lsb_jobidinstr(jobCard->jobSpecs.jobId),
                    exitFileSuffix(jobCard->jobSpecs.actValue));

	
        w_status = stat(exitFile, &st);

        if (w_status == 0)  
            jobCard->actStatus = ACT_DONE;
        else {
            jobCard->actStatus = ACT_FAIL;
        }
    }

    jobCard->jobSpecs.jStatus &= ~JOB_STAT_SIGNAL;

    switch (jobCard->jobSpecs.actValue) {
        case SIG_CHKPNT:
        case SIG_CHKPNT_COPY:
            chkpntEnd (jobCard, w_status, &freed);
            break;
        case SIG_SUSP_USER:   
        case SIG_SUSP_LOAD:
        case SIG_SUSP_WINDOW:
        case SIG_SUSP_OTHER:
            suspendActEnd (jobCard, w_status);
            break;

        case SIG_RESUME_USER:
        case SIG_RESUME_LOAD:
        case SIG_RESUME_WINDOW:
        case SIG_RESUME_OTHER:
            resumeActEnd (jobCard, w_status);
            break;

        case SIG_TERM_USER:
        case SIG_KILL_REQUEUE:
        case SIG_TERM_OTHER:
        case SIG_TERM_FORCE:
            
            if (jobSigLog (jobCard, w_status) == 0) {
                jobCard->jobSpecs.actPid = 0;
                jobCard->jobSpecs.actValue = SIG_NULL;
            }
            break;
        case SIG_TERM_LOAD:
        case SIG_TERM_WINDOW:
        case SIG_TERM_RUNLIMIT:
        case SIG_TERM_DEADLINE:
        case SIG_TERM_PROCESSLIMIT:
        case SIG_TERM_CPULIMIT:
        case SIG_TERM_MEMLIMIT:
            suspendActEnd (jobCard, w_status);
            break;
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5708,
                "sigActEnd: unknown sigValue <%d> for job <%s> at the job status <%d> with actPid <%d>"), /* catgets 5708 */
                jobCard->jobSpecs.actValue,
                lsb_jobid2str(jobCard->jobSpecs.jobId),
                jobCard->jobSpecs.jStatus,
                jobCard->jobSpecs.actPid);
            
            jobCard->jobSpecs.actPid = 0;
            return;

    } 

    if (!freed) {
	
	sbdlog_newstatus (jobCard);
    }
} 


static void
chkpntEnd (struct jobCard *jobCard, int w_status, bool_t *freed)
{
    static char fname[] = "chkpntEnd()";
    int savePid, saveStatus;

    
    if (IS_SUSP(jobCard->jobSpecs.jStatus)
       && !(jobCard->jobSpecs.jStatus & JOB_STAT_MIG))
        jobsig(jobCard, SIGSTOP, TRUE);

    saveStatus = jobCard->jobSpecs.jStatus;
    if (jobCard->jobSpecs.jStatus & JOB_STAT_MIG) {
        if (w_status == 0)  {
            if (!jobCard->missing) {
                
                jobCard->missing = TRUE;
                need_checkfinish = TRUE;
                return;
            } else if (jobCard->notReported == 0) 
                return;

            if (jobCard->cleanupPid == 0) {
                if ((jobCard->cleanupPid = rmJobBufFilesPid(jobCard)) > 0)
                    return;

                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5709,
                    "%s: Unable to cleanup migrating job <%s>"), /* catgets 5709 */
                    fname, lsb_jobid2str(jobCard->jobSpecs.jobId));
            }

            SBD_SET_STATE(jobCard, JOB_STAT_PEND);
        } else {
            jobCard->jobSpecs.jStatus &= ~JOB_STAT_MIG;
        }
    }

    savePid = jobCard->jobSpecs.actPid;

    if (status_job (BATCH_STATUS_JOB, jobCard, jobCard->jobSpecs.jStatus,
                    w_status == 0 ? ERR_NO_ERROR :
                    ERR_SYSACT_FAIL) < 0) {
        jobCard->jobSpecs.actPid = savePid; 
        jobCard->jobSpecs.jStatus = saveStatus;
    } else { 
        jobCard->lastChkpntTime = now;
        jobCard->jobSpecs.actPid = 0;
        jobCard->actStatus = ACT_NO;
        jobCard->jobSpecs.actValue = SIG_NULL;

        if (w_status == 0) {
            
            jobCard->migCnt = 1;
        }

        if (saveStatus & JOB_STAT_MIG) {
            if (w_status == 0) {
                
                cleanupMigJob(jobCard);
		deallocJobCard(jobCard);
		*freed = TRUE;
            } else
                jobCard->migCnt *= 2;
        }
    }

} 



void
suspendActEnd (struct jobCard *jobCard, int w_status)
{
    int sbdStartStop = 0;

    if (logclass & (LC_TRACE | LC_SIGNAL))
        ls_syslog(LOG_DEBUG, "suspendActEnd: Suspend job %s; reasons=%x, subresons=%d",
		  lsb_jobid2str(jobCard->jobSpecs.jobId), 
		  jobCard->actReasons,
		  jobCard->actReasons);

    sbdStartStop = (jobCard->actReasons & SUSP_SBD_STARTUP);
 
    jobCard->jobSpecs.lastSSuspTime = now;
    jobCard->jobSpecs.reasons |= jobCard->actReasons & (~SUSP_SBD_STARTUP);
    jobCard->jobSpecs.subreasons = jobCard->actSubReasons;

    if ((jobCard->jobSpecs.actValue == SIG_SUSP_USER) ||
       (jobCard->jobSpecs.actValue == SIG_TERM_USER))
	SET_STATE(jobCard->jobSpecs.jStatus, JOB_STAT_USUSP);
    else
	SET_STATE(jobCard->jobSpecs.jStatus, JOB_STAT_SSUSP);

    if (w_status == 0) 
	jobCard->actStatus = ACT_DONE;
    else
        jobCard->actStatus = ACT_FAIL;

    if (sbdStartStop)
        jobCard->actStatus = ACT_NO;

    if (jobSigLog(jobCard, w_status) == 0) {
        jobCard->jobSpecs.actValue = SIG_NULL;
        jobCard->jobSpecs.actPid = 0;
    }

}


void
resumeActEnd (struct jobCard *jobCard, int w_status)
{
    jobCard->jobSpecs.reasons = 0; 
    jobCard->jobSpecs.subreasons = 0; 
    SET_STATE(jobCard->jobSpecs.jStatus, JOB_STAT_RUN);
    
    if (w_status == 0) 
        jobCard->actStatus = ACT_DONE;
    else
        jobCard->actStatus = ACT_FAIL;

    if (jobSigLog(jobCard, w_status) == 0) {
        jobCard->jobSpecs.actValue = SIG_NULL;
        jobCard->jobSpecs.actPid = 0;
    }
}


static int
rmJobBufFilesPid(struct jobCard *jp)
{
    static char fname[] = "rmJobBufFilesPid()";
    int pid;

    if ((pid = fork()) < 0) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
	    lsb_jobid2str(jp->jobSpecs.jobId), "fork");
	return (pid);
    }

    if (pid)
	return (pid);

    
    
    closeBatchSocket();
    putEnv(LS_EXEC_T, "END");    

    if (postJobSetup(jp) < 0) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, 
	    fname,
	    lsb_jobid2str(jp->jobSpecs.jobId),
	    "postSetupUser");
	exit(-1);
    }
    
    rmJobBufFiles(jp);
    exit(0);
} 


static int
cleanupMigJob(struct jobCard *jp)
{
    static char fname[] = "cleanupMigJob()";
    int pid;

    
    unlockHosts (jp, jp->jobSpecs.numToHosts);

    if (!jp->jobSpecs.postCmd || jp->jobSpecs.postCmd[0] == '\0') 
        return (0);


    if ((pid = fork()) < 0) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname, 
	    lsb_jobid2str(jp->jobSpecs.jobId), "fork");
        lsb_merr2(_i18n_msg_get(ls_catd , NL_SETN, 700,
	    "Unable to fork a child to run the queue's post-exec command for job <%s>.  Please run <%s> manually if necessary.\n"), /* catgets 700 */
	    lsb_jobid2str(jp->jobSpecs.jobId), 
	    jp->jobSpecs.postCmd);
	return (pid);
    }

    if (pid)
	return (pid);

    
    
    closeBatchSocket();
    putEnv(LS_EXEC_T, "END");
    
    if (postJobSetup(jp) == -1) {
	ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname, 
	    lsb_jobid2str(jp->jobSpecs.jobId), "postJobSetup");
	lsb_merr2(_i18n_msg_get(ls_catd , NL_SETN, 701,
	    "Unable to setup the environment for job <%s> to run the queue's post exec.  Please run <%s> manually if necessary.\n"), /* catgets 701 */
	    lsb_jobid2str(jp->jobSpecs.jobId),
	    jp->jobSpecs.postCmd);
	exit(-1);
    }

    runQPost(jp);
    exit(0);
} 

void 
checkFinish (void)
{
    struct jobCard *jobCard, *nextJob;

    for (jobCard = jobQueHead->forw; (jobCard != jobQueHead);
					     jobCard = nextJob) {
        nextJob = jobCard->forw;         

        if (!(IS_FINISH(jobCard->jobSpecs.jStatus))
	    && !(IS_POST_FINISH(jobCard->jobSpecs.jStatus) ) ) {

            
            if ( (jobsig(jobCard, 0, FALSE) < 0)  
                || ( (jobCard->jobSpecs.jAttrib & JOB_FORCE_KILL)
		    && (jobCard->jobSpecs.termTime
			< time(0)-MAX(6,jobTerminateInterval*3)) ) ) {
	        jobGone (jobCard);
	    }
	}

	
	if (jobCard->jobSpecs.actPid) {
	    
	    if (killpg(jobCard->jobSpecs.actPid, SIGCONT) == 0)
		continue;
	    if (kill(jobCard->jobSpecs.actPid, SIGCONT) == 0)
		continue;
	    if (jobCard->cleanupPid > 0 &&
		kill(jobCard->cleanupPid, SIGCONT) == 0)
		continue;
	    
	    sigActEnd(jobCard); 
	    continue;
        }

        if (IS_FINISH(jobCard->jobSpecs.jStatus)
	    || IS_POST_FINISH(jobCard->jobSpecs.jStatus)
	    || (jobCard->jobSpecs.jStatus & JOB_STAT_PEND)) {
            job_finish (jobCard, TRUE);
        }
    }
} 

static int 
getTclHostData (struct hostLoad *load, struct tclHostData *tclHostData,
                                                              int freeMem)
{

    static char fname[] = "getTclHostData";
    static time_t lastUpdHostInfo = 0;
    static int numLsfHosts = 0;
    static struct hostInfo *hostInfo = NULL;
    struct hostInfo *temp;
    int i, num;

    if (now - lastUpdHostInfo > 10 * 60) {
        
        if ((temp = ls_gethostinfo("-:server", &num, 0,
                    0, LOCAL_ONLY)) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_gethostinfo");
	    return -1;
        }
        if (hostInfo != NULL) {
	    freeLsfHostInfo (hostInfo, numLsfHosts);
	    FREEUP (hostInfo);
	    numLsfHosts = 0;
        }
        
        hostInfo = (struct hostInfo *) my_malloc
                (num * sizeof (struct hostInfo), fname);
        for (i = 0; i < num; i++) {
            copyLsfHostInfo (&hostInfo[i], &temp[i]);

	    if ( logclass & LC_TRACE) { 
	        ls_syslog(LOG_DEBUG2, "%s: host <%s> ncpus <%d> maxmem <%u> maxswp <%u> maxtmp <%u> ndisk <%d>", 
		    fname, hostInfo[i].hostName, hostInfo[i].maxCpus, 
	            hostInfo[i].maxMem, hostInfo[i].maxSwap,
	            hostInfo[i].maxTmp, hostInfo[i].nDisks);
	    }
        }
        numLsfHosts = num;
	lastUpdHostInfo = now;

    }
    if (freeMem == TRUE)  {
        FREEUP (tclHostData->resBitMaps); 
        FREEUP (tclHostData->loadIndex);
    }
    for (i = 0; i < numLsfHosts; i++) {
	if (equalHost_(hostInfo[i].hostName, load->hostName))
	    break;
    }
    if (i == numLsfHosts) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5716,
	    "%s: Host <%s> is not used by the batch system"), /* catgets 5716 */
	    fname, load->hostName);
        return -1;
    }
    tclHostData->hostName = hostInfo[i].hostName;
    tclHostData->hostType = hostInfo[i].hostType;
    tclHostData->hostModel = hostInfo[i].hostModel;
    tclHostData->maxCpus = hostInfo[i].maxCpus;
    tclHostData->maxMem = hostInfo[i].maxMem;
    tclHostData->maxSwap = hostInfo[i].maxSwap;
    tclHostData->maxTmp = hostInfo[i].maxTmp;
    tclHostData->nDisks = hostInfo[i].nDisks;
    tclHostData->hostInactivityCount = 0;
    tclHostData->rexPriority = 0;
    tclHostData->fromHostType = tclHostData->hostType;
    tclHostData->fromHostModel = tclHostData->hostModel;
    tclHostData->cpuFactor = hostInfo[i].cpuFactor;
    tclHostData->ignDedicatedResource = FALSE;
    tclHostData->resBitMaps = getResMaps(hostInfo[i].nRes, 
					       hostInfo[i].resources);
    tclHostData->DResBitMaps = NULL;
    tclHostData->flag = TCL_CHECK_EXPRESSION;
    tclHostData->status = load->status;
    tclHostData->loadIndex 
       = (float *) my_malloc (allLsInfo->numIndx * sizeof(float), fname);
    tclHostData->loadIndex[R15S] = (hostInfo[i].cpuFactor != 0.0)?
		((load->li[R15S] + 1.0)/hostInfo[i].cpuFactor):load->li[R15S];
    tclHostData->loadIndex[R1M] = (hostInfo[i].cpuFactor != 0.0)?
		((load->li[R1M] + 1.0)/hostInfo[i].cpuFactor):load->li[R1M];
    tclHostData->loadIndex[R15M] = (hostInfo[i].cpuFactor != 0.0)?
		((load->li[R15M] + 1.0)/hostInfo[i].cpuFactor):load->li[R15M];

    for (i = 3; i < allLsInfo->numIndx; i++)
        tclHostData->loadIndex[i] = load->li[i];

    return 0;

} 

static void
ruLimits(struct jobCard *jobCard)
{
    struct rlimit rlimit;

    
    rlimitDecode_(&jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_CPU],
		  &rlimit, LSF_RLIMIT_CPU);


    
    if (rlimit.rlim_cur != RLIM_INFINITY && lsbJobCpuLimit != 0) {

	if ((long)rlimit.rlim_cur < ((long)jobCard->runRusage.utime +
			       (long)jobCard->runRusage.stime)) {
            
            if (jobCard->jobSpecs.jStatus & JOB_STAT_KILL) {
                
	    } else {
		jobSigStart (jobCard, SIG_TERM_CPULIMIT, 0, 0, SIGLOG);
		sbdlog_newstatus(jobCard); 

		jobCard->jobSpecs.jStatus |= JOB_STAT_KILL;
	    }
	}
    }

    
    rlimitDecode_(&jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_SWAP],
                  &rlimit, LSF_RLIMIT_SWAP);
    if (rlimit.rlim_cur != RLIM_INFINITY) {
        if ((long)(rlimit.rlim_cur / 1024) < (long)jobCard->runRusage.swap) {
            jobsig(jobCard, SIGQUIT, FALSE);
            jobsig(jobCard, SIGKILL, TRUE);
        }
    }

    
    rlimitDecode_(&jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_PROCESS],
                  &rlimit, LSF_RLIMIT_PROCESS);

    if (rlimit.rlim_cur != RLIM_INFINITY) { 
        if ((int)rlimit.rlim_cur + 2 < jobCard->runRusage.npids) { 
                               
            if ((IS_SUSP (jobCard->jobSpecs.jStatus))
               && (jobCard->jobSpecs.reasons & SUSP_RES_LIMIT)
               && (jobCard->jobSpecs.subreasons & SUB_REASON_PROCESSLIMIT)) 
                return; 
            else { 
                jobSigStart (jobCard, SIG_TERM_PROCESSLIMIT, 0, 0, SIGLOG);
                sbdlog_newstatus(jobCard); 
            }
        }
    }
   
    
    if ( (lsbJobMemLimit == 1) || 
	 (lsbJobMemLimit != 0 && lsbMemEnforce == TRUE)) {
        rlimitDecode_(&jobCard->jobSpecs.lsfLimits[LSF_RLIMIT_RSS],
                      &rlimit, LSF_RLIMIT_RSS);
        if (rlimit.rlim_cur != RLIM_INFINITY) {
	    if ((long)(rlimit.rlim_cur / 1024) < (long)jobCard->runRusage.mem) {
                if (jobCard->jobSpecs.jStatus & JOB_STAT_KILL) {
                
	        } else {
		    jobSigStart (jobCard, SIG_TERM_MEMLIMIT, 0, 0, SIGLOG);
		    sbdlog_newstatus(jobCard); 
		    jobCard->jobSpecs.jStatus |= JOB_STAT_KILL;
		}		
	    }
        }
    }
} 

static void
initTclHostData (struct tclHostData *tclHostData)
{
    if (tclHostData == NULL)
        return;

    tclHostData->resBitMaps = NULL;
    tclHostData->loadIndex = NULL;
    tclHostData->resPairs = NULL;
    tclHostData->numResPairs = 0;

} 


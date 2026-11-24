/* $Id: bhist.h 397 2007-11-26 19:04:00Z mblack $
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
 

#ifndef BHISTOUT_H
#define BHISTOUT_H

#include <pwd.h>
#include <time.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/param.h>
#include <string.h>
#include <sys/types.h>
#include <sys/dir.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "../cmd/cmd.h"
#include "../lsbatch.h"

#define OPT_ALL             0x1      
#define OPT_DFTSTATUS       0x2      
#define OPT_DONE            0x4      
#define OPT_PEND            0x8      
#define OPT_SUSP           0x10      
#define OPT_RUN            0x20      
#define OPT_QUEUE          0x40      
#define OPT_USER           0x80      
#define OPT_ALLUSERS      0x100      
#define OPT_HOST          0x200      
#define OPT_DFTFORMAT     0x400      
#define OPT_SHORTFORMAT   0x800      
#define OPT_LONGFORMAT   0x1000      
#define OPT_JOBID        0x2000      
#define OPT_COMPLETE     0x4000      
#define OPT_SUBMIT       0x8000      
#define OPT_DISPATCH    0x10000      
#define OPT_ELOGFILE    0x20000      
#define OPT_NORMALIZECPU 	0x40000      
#define OPT_JOBNAME		0x80000      
#define OPT_NUMLOGFILE         0x100000      
#define OPT_PROJ               0x200000      
#define OPT_ALLPROJ            0x400000      
#define OPT_WIDEFORMAT         0x800000      
#define OPT_CHRONICLE         0x1000000      
#define OPT_TIME_INTERVAL     0x2000000      
#define OPT_ARRAY_INFO        0x4000000      
#define OPT_EXIT              0x8000000      

#define MAX_EVENT_FILE	100          

struct bhistReq {
    int    options;                    
    int    userId;	      	        
    char   userName[MAX_LSB_NAME_LEN];
    char   eventFileName[MAXFILENAMELEN];
    char   queue[MAX_LSB_NAME_LEN]; 
    char   checkHost[MAXHOSTNAMELEN];   
    time_t endTime[2];
    time_t submitTime[2];
    time_t startTime[2];
    time_t searchTime[2]; 
    float  cpuFactor;
    int    numJobs;
    LS_LONG_INT    *jobIds;
    char  *jobName;		
    int    numLogFile;          
    int    numMinLogFile;       
    char   projectName[MAX_LSB_NAME_LEN];
};

struct eventRecord {
    struct    eventRecord *next;
    struct    eventRecord *chronback;
    struct    eventRecord *chronforw;
    struct    jobRecord *jobR;
    char      kind;
    int       jStatus;
    int       reasons;
    int       subreasons;
    struct loadIndexLog *ld; 
    char      queue[MAX_LSB_NAME_LEN];     
    time_t    timeStamp;
    time_t    timeEvent;
    int       numExHosts;
    char      **exHosts;
    int    jobPid;
    int    actFlags;
    time_t chkPeriod;
    int    actStatus;
    int    actPid;
    int    chkOk;
    int    sigValue;
    int    migNumAskedHosts;
    char   **migAskedHosts;
    struct  jobInfoEnt *newParams;           
    float   cpuTime;                         
    int     runCount;
    int     userId;
    char    *userName;
    char    *sigSymbol;
    int     execUid;
    char    *execHome;
    char    *execCwd;
    char    *execUsername;
    int     exitStatus;
    termcode_t termCode;

    int     usrId;
    int     idx;
    int     jmMsgId;
    int     jmMsgType;
    char    *jmSrc;
    char    *jmDest;
    char    *jmMsg;
    int     base;       
    int     position;   
    
    union {
       struct jobModLog  jobModLog;
    } eventRecUnion;

};


struct jobRecord {
    struct    jobInfoEnt *job;
    int       jobPid;
    int       jobGid;
    int       currentStatus;
    time_t    delayTime;
    float     hostFactor;
    float     cpuFactor;
    char      *hostPtr;
    int	      preExecPid;
    int	      preExecPGid;
    struct    eventRecord  *eventhead;     
    struct    eventRecord  *eventtail;     
    struct    jobRecord    *forw;          
    struct    jobRecord    *back;          
    int	      nonNewFromHeadFlag;	
					
					
};

extern struct hTab jobIdHT;
extern struct loadIndexLog *loadIndex;
extern char *bhist_malloc(int);
extern char *bhist_calloc(int, int);
extern struct bhistReq  Req;
extern int readFromHeadFlag;	


extern char read_loadIndex(struct eventRec *);
extern char check_queue(struct bhistReq *, char *);
extern char check_host(struct bhistReq *, struct jobRecord *);
extern struct jobInfoEnt *read_newjob(struct eventRec *);
extern struct jobInfoEnt *copyJobInfoEnt(struct jobInfoEnt *);
extern struct jobRecord *read_startjob(struct eventRec *);
extern char read_newstat(struct eventRec *);
extern char read_chkpnt(struct eventRec *);
extern char read_mig(struct eventRec *);
extern char read_delete(struct eventRec *);
extern char read_switch(struct eventRec *);
extern char read_jobmove(struct eventRec *);
extern char read_signal(struct eventRec *);
extern char read_jobstartaccept(struct eventRec *);
extern char read_jobmsg(struct eventRec *);
extern char read_jobmsgack(struct eventRec *);
extern char read_jobforce(struct eventRec *);
extern int addJob(struct jobRecord *);
extern int addEvent(struct eventRecord *, struct jobRecord *);
extern void freeJobInfoEnt(struct jobInfoEnt *);
extern void freeJobRecord(struct jobRecord *);

extern struct jobRecord *initJobList (void);
extern void removeJobList(struct jobRecord *);
extern struct jobInfoEnt *initJobInfo (void);
extern char read_jobrequeue(struct eventRec *);
extern int  matchJobId(struct bhistReq *, LS_LONG_INT);

extern hEnt *chekMemb(struct hTab *tabPtr, LS_LONG_INT member); 
extern void parse_event(struct eventRec *, struct bhistReq *);
extern int bhistReqInit(struct bhistReq *);

#endif 

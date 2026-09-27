/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * $Id: daemonout.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef MBDOUT_H
#define MBDOUT_H

#include "../../lsf/lsf.h"
#include "../lsbatch.h"

#include "../../lsf/lib/lib.hdr.h"
#include "../../lsf/lib/lproto.h"
#include "../lib/lsb.sig.h"

#define BATCH_MASTER_PORT   40000
#define ALL_HOSTS      "all"                

#define  PUT_LOW(word, s)  (word = (s | (word & ~0x0000ffff)))
#define  PUT_HIGH(word, s) (word = ((s << 16) | (word & 0x0000ffff)))
#define  GET_LOW(s, word)  (s = word & 0x0000ffff)
#define  GET_HIGH(s, word) (s = (word >> 16) & 0x0000ffff)


#define RSCHED_LISTSEARCH_BY_EXECJID       0
#define RSCHED_LISTSEARCH_BY_EXECLUSNAME   1

typedef enum {
    
    BATCH_JOB_SUB       = 1,
    BATCH_JOB_INFO      = 2,    
    BATCH_JOB_PEEK      = 3,
    BATCH_JOB_SIG       = 4,    
    BATCH_HOST_INFO     = 5,
    BATCH_QUE_INFO      = 6,    
    BATCH_GRP_INFO      = 7,   
    BATCH_QUE_CTRL      = 8,
    BATCH_RECONFIG      = 9,    
    BATCH_HOST_CTRL     = 10,
    BATCH_JOB_SWITCH    = 11,    
    BATCH_JOB_MOVE      = 12,
    BATCH_JOB_MIG       = 13,
    BATCH_STATUS_JOB    = 15,         
    BATCH_SLAVE_RESTART = 16,
    BATCH_USER_INFO     = 17,
    BATCH_PARAM_INFO    = 20,
    BATCH_JOB_MODIFY    = 22,
    BATCH_JOB_EXECED    = 25,
    BATCH_JOB_MSG       = 27,
    BATCH_STATUS_MSG_ACK = 28,
    BATCH_DEBUG         = 29,
    BATCH_RESOURCE_INFO = 30,
    BATCH_RUSAGE_JOB    = 32, 
    BATCH_JOB_FORCE      = 37,
    BATCH_UNUSED_38      = 38,
    BATCH_UNUSED_39      = 39,
    BATCH_STATUS_CHUNK   = 40,
    BATCH_JOB_SUB_PACK    = 41,
    BATCH_SHOWCONF       = 42,
    BATCH_RSRC_LIMIT_INFO  = 43,
    BATCH_OUTPUT         = 44,
    BATCH_SET_JOB_ATTR   = 90,
    READY_FOR_OP         = 1023,
    PREPARE_FOR_OP       = 1024,

    QMBD_CTRL            = 2001,  /* Internal qmbd control event. */
    QMBD_SUBMIT          = 2002   /* Internal qmbd submit replay event. */
} mbdReqType;

/* BATCH_OUTPUT uses reserved as its object discriminator. */
#define OUTPUT_JOB   1
#define OUTPUT_HOST  2
#define OUTPUT_QUEUE 3
#define OUTPUT_JOB_GROUP 4 /* Group records retain the legacy record shape. */
#define IS_JOB_OUTPUT(h) ((h)->opCode == BATCH_OUTPUT && \
    (h)->reserved == OUTPUT_JOB)

#define SUB_RLIMIT_UNIT_IS_KB 0x80000000

struct submitReq {
    int     options;        
    int     options2;        
    char    *jobName;
    char    *queue;
    int     numAskedHosts;            
    char    **askedHosts;             
    char    *resReq;
    int     rLimits[LSF_RLIM_NLIMITS]; 
    char    *hostSpec;                
    int     numProcessors;            
    char    *dependCond;              
    time_t  beginTime;                
    time_t  termTime;                 
    int     sigValue;                 
    char    *subHomeDir;              
    char    *submitCwd;               
    char    *inFile;                  
    char    *outFile;                 
    char    *errFile;                 
    char    *command;                 
    char    *inFileSpool;
    char    *commandSpool;
    time_t  chkpntPeriod;
    char    *chkpntDir;
    int     restartPid;               
    int     nxf;                      
    struct  xFile *xf;                
    char    *jobFile;   
    char    *fromHost;
    time_t  submitTime;               
    int     umask;
    char    *cwd;
    char    *preExecCmd;
    char    *postExecCmd;
    char    *mailUser;
    char    *projectName;             
    int     niosPort;                 
    int     maxNumProcessors;         
    char    *loginShell;              
    char    *schedHostType;           
    char    *userGroup;
    int     userPriority;
    char    *jobDesc;
};


/* Pack job submission request */
struct submitPackReq {
    int jobCount;           /* Number of jobs */
    int options;            /* Pack options: 0x01=streaming response, 0x02=continue on failure */
    int maxConcurrency;     /* Maximum concurrent processing, 0=unlimited */
    time_t clientTimestamp; /* Client timestamp */
    char *sourceFile;       /* Source file path */
    struct submitReq *jobs; /* Job array */
};


/*
 * Socket-sync payload used to replay one committed submit into qmbd.
 * submitReq does not carry the master-assigned jobId or authoritative user
 * identity, so those fields are transmitted alongside it.
 */
struct qmbdSubmitReq {
    LS_LONG_INT baseJobId;        /* JobId already allocated by main mbd. */
    time_t masterSubmitTime;      /* Submit time from main mbd's job bill. */
    int userId;                   /* User id copied from main mbd's jData. */
    char *userName;               /* User name copied from main mbd's jData. */
    struct submitReq submitReq;   /* Original submit attributes for replay. */
};

enum qmbdCtrlOp {
    QMBD_CTRL_EXIT = 1            /* Ask the current qmbd generation to exit. */
};

struct qmbdCtrlReq {
    int controlOp;                /* One of enum qmbdCtrlOp. */
};


#define SHELLLINE "#! /bin/sh\n\n"
#define CMDSTART "# LSBATCH: User input\n"
#define CMDEND "# LSBATCH: End user input\n"
#define ENVSSTART "# LSBATCH: Environments\n"
#define LSBNUMENV "#LSB_NUM_ENV="
#define EDATASTART "# LSBATCH: edata\n"
#define AUXAUTHSTART "# LSBATCH: aux_auth_data\n"
#define EXITCMD "exit `expr $? \"|\" $ExitStat`\n"
#define WAITCLEANCMD "\nExitStat=$?\nwait\n# LSBATCH: End user input\ntrue\n"
#define TAILCMD "'; export "
#define TRAPSIGCMD "$LSB_TRAPSIGS\n$LSB_RCP1\n$LSB_RCP2\n$LSB_RCP3\n"
#define JOB_STARTER_KEYWORD "%USRCMD"
#define SCRIPT_WORD "_USER_\\SCRIPT_"
#define SCRIPT_WORD_END "_USER_SCRIPT_"

struct submitMbdReply {
    LS_LONG_INT jobId;
    char    *queue;
    int     badReqIndx;
    int     subTryInterval;
    char    *badJobName;
    char    *pendLimitReason;
    int     replyCode;
};

struct submitMbdPackReply {
    int     numJobs;
    int     numSuccess;
    int     numFailed;
    struct  submitMbdReply *submitMbdReps;
};

struct modifyReq {
    LS_LONG_INT jobId;                
    char * jobIdStr;             
    int    delOptions;                  
    int    delOptions2;                 
    struct submitReq submitReq;
};

struct jobInfoReq {
    int    options;
    char   *userName;      
    LS_LONG_INT jobId;
    char   *jobName;
    char   *queue;
    char   *host;          
    char   *outputFields;
};

struct jobInfoReply {
    LS_LONG_INT jobId;
    int       status;
    int       *reasonTb;             
    int       numReasons;            
    int       reasons;               
    int       subreasons;            
    time_t    startTime;             
    time_t    predictedStartTime;    
    time_t    endTime;
    float     cpuTime;
    int       numToHosts;
    char      **toHosts;
    int       nIdx;                  
    float     *loadSched;            
    float     *loadStop;             
    int       userId;
    char      *userName;
    int       execUid;
    int       exitStatus;
    char      *execHome;
    char      *execCwd;
    char      *execUsername;
    struct    submitReq *jobBill;    
    time_t    reserveTime;
    int       jobPid;
    time_t    jRusageUpdateTime;
    struct    jRusage runRusage;  
    int       jType;              
    char      *parentGroup;          
    char      *jName;          	   
    int       counter[NUM_JGRP_COUNTERS]; 
    u_short   port;                
    int       jobPriority;         
    char      *chargedSAAP;
    char      *mergedResReq;
    char      *effeResReq;
    int       maxMem;
    int       avgMem;
    struct    limitDetailEnt *limitDetailTb;
    int       numLimitDetail;
    float     idleFactor;
};

/*
 * Field selection bitmasks used in jobOutputReply.fields to selectively
 * encode/decode requested job fields over XDR, reducing payload size.
 */
#define JOB_OUTPUT_USER        0x0001
#define JOB_OUTPUT_STAT        0x0002
#define JOB_OUTPUT_QUEUE       0x0004
#define JOB_OUTPUT_FROM_HOST   0x0008
#define JOB_OUTPUT_EXEC_HOST   0x0010
#define JOB_OUTPUT_JOB_NAME    0x0020
#define JOB_OUTPUT_SUBMIT_TIME 0x0040
#define JOB_OUTPUT_PROJ_NAME   0x0080
#define JOB_OUTPUT_CPU_USED    0x0100
#define JOB_OUTPUT_MEM         0x0200
#define JOB_OUTPUT_SWAP        0x0400
#define JOB_OUTPUT_PIDS        0x0800
#define JOB_OUTPUT_START_TIME  0x1000
#define JOB_OUTPUT_FINISH_TIME 0x2000
#define JOB_OUTPUT_EXIT_CODE   0x4000
#define JOB_OUTPUT_REASONS     0x8000 /* v23; needed to distinguish ZOMBI */

#define JOB_OUTPUT_MAX_MEM     0x10000 /* v23 */
#define JOB_OUTPUT_AVG_MEM     0x20000 /* v23 */

struct jobOutputReply {
    LS_LONG_INT jobId;
    unsigned int fields;
    char *userName;
    int status;
    int reasons;
    char *queue;
    char *fromHost;
    int numExHosts;
    char **exHosts;
    char *jobName;
    time_t submitTime;
    char *projectName;
    float cpuTime;
    int mem;
    int swap;
    int maxMem;
    int avgMem;
    int npids;
    struct pidInfo *pidInfo;
    time_t startTime;
    time_t endTime;
    int exitStatus;
};

struct infoReq {
    int options;
    int numNames;
    char **names;
    char  *resReq;
    char  *outputFields;
};


struct userInfoReply {
    int   badUser;
    int   numUsers;                   
    struct  userInfoEnt *users;
};

struct queueInfoReply {
    unsigned long long outputMask;
    int    badQueue;
    int    numQueues;
    int    nIdx; 
    struct queueInfoEnt *queues;
};

struct hostDataReply {
    unsigned long long outputMask;
    int  badHost;
    int  numHosts;
    int  nIdx; 
    int  flag;
#define LOAD_REPLY_SHARED_RESOURCE 0x1
    struct hostInfoEnt  *hosts;
};

struct groupInfoReply {
    int  numGroups;
    struct groupInfoEnt *groups;
};

struct jobPeekReq {
    LS_LONG_INT   jobId;
};

struct jobPeekReply {
    char *outFile;
    char *pSpoolDir;
};



struct signalReq {
    int    sigValue;
    LS_LONG_INT jobId;
    time_t chkPeriod;
    int    actFlags;
};


struct jobMoveReq {
    int         opCode;
    LS_LONG_INT  jobId;          
    int         position;       
};

struct jobSwitchReq {
    LS_LONG_INT jobId;               
    char   queue[MAX_LSB_NAME_LEN];   
};

struct migReq {
    LS_LONG_INT jobId;
    int options;
    int numAskedHosts;
    char **askedHosts;
};

typedef enum {
         
    MBD_NEW_JOB_KEEP_CHAN = 0,
    

        MBD_NEW_JOB     = 1,
        MBD_SIG_JOB     = 2,
        MBD_SWIT_JOB    = 3,
        MBD_PROBE       = 4,
        MBD_REBOOT      = 5,
        MBD_SHUTDOWN    = 6,
	CMD_SBD_DEBUG   = 7, 
        UNUSED_8        = 8,
	MBD_MODIFY_JOB  = 9,

        SBD_JOB_SETUP   = 100, 
        SBD_SYSLOG      = 101,  
        SBD_DONE_MSG_JOB = 102, 

        RM_JOB_MSG      = 200,
        RM_CONNECT      = 201,

 
    CMD_SBD_REBOOT      = 300,
    CMD_SBD_SHUTDOWN    = 301,
    CMD_SBD_SHOWCONF    = 302
} sbdReqType;


struct lenDataList {
    int   numJf;
    struct lenData *jf;
};
 

extern void initTab (struct hTab *tabPtr);
extern hEnt *addMemb (struct hTab *tabPtr, LS_LONG_INT member);
extern char remvMemb (struct hTab *tabPtr, LS_LONG_INT member);
extern hEnt *chekMemb (struct hTab *tabPtr, LS_LONG_INT member);
extern hEnt *addMembStr (struct hTab *tabPtr, char *member);
extern char remvMembStr (struct hTab *tabPtr, char *member);
extern hEnt *chekMembStr (struct hTab *tabPtr, char *member);
extern void convertRLimit(int *pRLimits, int toKb);
extern int limitIsOk_(int *rLimits);

extern int handShake_(int , char, int);

#define CALL_SERVER_NO_WAIT_REPLY 0x1 
#define CALL_SERVER_USE_SOCKET    0x2 
#define CALL_SERVER_NO_HANDSHAKE  0x4 
#define CALL_SERVER_ENQUEUE_ONLY  0x8 
extern int call_server(char *, ushort, char *, int, char **, 
               struct LSFHeader *, int, int, int *, int (*)(),
               int *, int);

extern int sndJobFile_(int, struct lenData *); 

#include "../lib/lsb.xdr.h"

extern struct group *mygetgrnam(const char *);
extern void freeUnixGrp(struct group *);
extern struct group *copyUnixGrp(struct group *);

extern void freeGroupInfoReply(struct groupInfoReply *reply);

extern void appendEData(struct lenData *jf, struct lenData *ed);
extern char* getUnixSpoolDir(char *);
#endif

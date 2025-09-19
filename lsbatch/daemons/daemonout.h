/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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
    BATCH_SET_JOB_ATTR   = 90,
    READY_FOR_OP         = 1023,
    PREPARE_FOR_OP       = 1024
} mbdReqType;

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
};

struct infoReq {
    int options;
    int numNames;
    char **names;
    char  *resReq;
};


struct userInfoReply {
    int   badUser;
    int   numUsers;                   
    struct  userInfoEnt *users;
};

struct queueInfoReply {
    int    badQueue;
    int    numQueues;
    int    nIdx; 
    struct queueInfoEnt *queues;
};

struct hostDataReply {
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
    CMD_SBD_SHUTDOWN    = 301
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

#endif

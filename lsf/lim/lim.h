/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * Copyright (C) 2011 David Bigagli
 *
 * $Id: lim.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef _LIM_H_
#define _LIM_H_

#include "../lsf.h"
#include "../intlib/intlibout.h"
#include "../lib/lproto.h"
#include "limout.h"
#include "../lib/lib.table.h"
#include "../lib/lib.hdr.h"
#include "../lib/lib.xdr.h"

#define EXCHINTVL 	    15
#define SAMPLINTVL           5
#define HOSTINACTIVITYLIMIT   5
#define MASTERINACTIVITYLIMIT 2
#define RESINACTIVITYLIMIT    9
#define RETRYLIMIT            2

#define SBD_ACTIVE_TIME 60*5

#define KEEPTIME   2
#define MAXCANDHOSTS  10
#define MAXCLIENTS   64
#define WARNING_ERR   EXIT_WARNING_ERROR
#define MIN_FLOAT16  2.328306E-10
#define LIM_EVENT_MAXSIZE  (1024 * 1024)

#ifndef MIN
#define MIN(x,y)        ((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x,y)        ((x) > (y) ? (x) : (y))
#endif

#define DEFAULT_AFTER_HOUR "19:00-7:00 5:19:00-1:7:00"

struct timewindow {
    char       *winName;
    windows_t  *week[8];
    time_t     wind_edge;
};

#define LIM_STARTUP    0
#define LIM_CHECK      1
#define LIM_RECONFIG   2

struct statInfo {
    short    hostNo;
    int      maxCpus;
    int      maxMem;
    int      maxSwap;
    int      maxTmp;
    int      nDisks;
    u_short  portno;
    char     hostType[MAXLSFNAMELEN];
    char     hostArch[MAXLSFNAMELEN];
};

#define HF_INFO_VALID   0x01
#define HF_REDUNDANT    0x02
#define HF_CONSTATUS    0x04

struct hostNode {
    char    *hostName;
    short   hModelNo;
    short   hTypeNo;
    short   hostNo;
    u_short naddr;
    in_addr_t   *addr;
    struct  statInfo statInfo;
    char    infoValid;
    unsigned char protoVersion;
    short   availHigh;
    short   availLow;
    short   use;
    int     resClass;
    int     DResClass;
    u_short nRes;
    char    *windows;
    windows_t *week[8];
    time_t  wind_edge;
    time_t  lastJackTime;
    short hostInactivityCount;
    int     *status;
    float   *busyThreshold;
    float   *loadIndex;
    float   *uloadIndex;
    char    conStatus;
    u_int   lastSeqNo;
    int     rexPriority;
    int     infoMask;
    int     loadMask;
    int     *resBitMaps;
    int     *DResBitMaps;
    int     numInstances;
    struct  resourceInstance **instances;
    int     callElim ;
    int     maxResIndex;
    int     *resBitArray;
    struct  hostNode *nextPtr;
    time_t  expireTime;
    uint8_t migrant;
};

#define CLUST_ACTIVE		0x00010000
#define CLUST_MASTKNWN    	0x00020000
#define CLUST_CONNECT     	0x00040000
#define CLUST_INFO_AVAIL  	0x00080000
#define CLUST_HINFO_AVAIL  	0x00100000
#define CLUST_ELIGIBLE    	0x00200000
#define CLUST_ALL_ELIGIBLE 	0x00400000


struct clusterNode {
    short clusterNo;
    char *clName;
    int status;
    in_addr_t candAddrList[MAXCANDHOSTS];
    int     currentAddr;
    char   *masterName;
    u_int   masterAddr;
    u_short masterPort;
    int     resClass;
    int     typeClass;
    int     modelClass;
    char  masterKnown;
    int   masterInactivityCount;
    struct hostNode *masterPtr;
    struct hostNode *prevMasterPtr;
    u_short  checkSum;
    int  numHosts;
    int  numClients;
    int  managerId;
    char *managerName;
    struct hostNode  *hostList;
    struct hostNode  *clientList;
    struct clusterNode *nextPtr;
    char  *eLimArgs;
    char  **eLimArgv;
    int   chanfd;
    int   numIndx;
    int   numUsrIndx;
    int   usrIndxClass;
    char  **loadIndxNames;
    int nAdmins;
    int *adminIds;
    char **admins;
    int  nRes;
    int  *resBitMaps;
    int  *hostTypeBitMaps;
    int  *hostModelBitMaps;
    int numSharedRes;
    char **sharedResource;
    struct shortLsInfo *shortInfo;
};

struct clientNode {
    char   inprogress;
    enum   limReqCode limReqCode;
    int    clientMasks;
    int    chanfd;
    struct hostNode *fromHost;
    struct sockaddr_in from;
    struct Buffer *reqbuf;
};

struct liStruct {
    char  *name;
    char  increasing;
    float delta[2];
    float extraload[2];
    float valuesent;
    float exchthreshold;
    float sigdiff;
    float satvalue;
    float value;
};
int li_len;
struct liStruct *li;

#define  SEND_NO_INFO       0x00
#define  SEND_CONF_INFO     0x01
#define  SEND_LOAD_INFO     0x02
#define  SEND_MASTER_ANN    0x04
#define  SEND_ELIM_REQ      0x08
#define  SEND_MASTER_QUERY  0x10
#define  SLIM_XDR_DATA      0x20
#define  SEND_LIM_LOCKEDM   0x100

struct loadVectorStruct {
    int     hostNo;
    int     *status;
    u_int   seqNo;
    int     checkSum;
    int     flags;
    int     numIndx;
    int     numUsrIndx;
    float   *li;
    int     numResPairs;
    struct resPair *resPairs;
};

#define DETECTMODELTYPE 0

#define MAX_SRES_INDEX	2

struct masterReg {
    char   clName[MAXLSFNAMELEN];
    char   hostName[MAXHOSTNAMELEN];
    int    flags;
    u_int  seqNo;
    int    checkSum;
    u_short portno;
    int    licFlag;
    int    maxResIndex;
    int    *resBitArray;
};

struct resourceInstance {
    char      *resName;
    int       nHosts;
    struct hostNode **hosts;
    char      *orignalValue;
    char      *value;
    time_t    updateTime;
    struct hostNode *updHost;
};


typedef struct sharedResourceInstance{
    char *resName ;
    int nHosts ;
    struct hostNode **hosts;
    struct sharedResourceInstance *nextPtr ;
} sharedResourceInstance ;

struct minSLimConfData {
    int     defaultRunElim;
    int     nClusAdmins;
    int     *clusAdminIds;
    char    **clusAdminNames;
    float   exchIntvl;
    float   sampleIntvl;
    short   hostInactivityLimit;
    short   masterInactivityLimit;
    short   retryLimit;
    short   keepTime;
    struct  resItem *allInfo_resTable;
    int     allInfo_nRes;
    int     allInfo_numIndx;
    int     allInfo_numUsrIndx;
    u_short myCluster_checkSum;
    char    *myCluster_eLimArgs;
    char    *myHost_windows;
    int     numMyhost_weekpair[8];
    windows_t *myHost_week[8];
    time_t  myHost_wind_edge;
    float   *myHost_busyThreshold;
    int     myHost_rexPriority;
    int     myHost_numInstances;
    struct  resourceInstance **myHost_instances;
    struct  sharedResourceInstance *sharedResHead;
};


extern struct sharedResourceInstance *sharedResourceHead ;

#define  BYTE(byte)  (((int)byte)&0xff)
#define THRLDOK(inc,a,thrld)    (inc ? a <= thrld : a >= thrld)

extern int getpagesize(void);

/* These are the entries in the limParams[]
 * LIM configuration array.
 */
typedef enum {
    LSF_CONFDIR,
    LSF_LIM_DEBUG,
    LSF_SERVERDIR,
    LSF_BINDIR,
    LSF_LOGDIR,
    LSF_LIM_PORT,
    LSF_RES_PORT,
    LSF_DEBUG_LIM,
    LSF_TIME_LIM,
    LSF_LOG_MASK,
    LSF_CONF_RETRY_MAX,
    LSF_CONF_RETRY_INT,
    LSF_CROSS_UNIX_NT,
    LSF_LIM_IGNORE_CHECKSUM,
    LSF_MASTER_LIST,
    LSF_REJECT_NONLSFHOST,
    LSF_LIM_JACKUP_BUSY,
    LIM_RSYNC_CONFIG,
    LIM_COMPUTE_ONLY,
    LSB_SHAREDIR,
    LIM_NO_MIGRANT_HOSTS,
    LIM_NO_FORK,
    LSF_UNIT_FOR_LIMITS
} limParams_t;


#define LOOP_ADDR       0x7F000001


extern struct config_param limParams[];
extern int lim_debug;
extern int lim_CheckMode;
extern int lim_CheckError;
extern int limSock;
extern int limTcpSock;
extern ushort lim_port;
extern ushort lim_tcp_port;
extern struct clusterNode *myClusterPtr;
extern struct hostNode *myHostPtr;
extern char myClusterName[];
extern int  masterMe;
extern float exchIntvl;
extern float sampleIntvl;
extern short hostInactivityLimit;
extern short masterInactivityLimit;
extern short resInactivityLimit;
extern short retryLimit;
extern short keepTime;
extern int probeTimeout;
extern short resInactivityCount;
extern char jobxfer;
extern short satper;
extern float *extraload;
extern int   nClusAdmins;
extern int   *clusAdminIds;
extern int   *clusAdminGids;
extern char  **clusAdminNames;
extern struct liStruct *li;
extern int li_len;
extern int defaultRunElim;
extern time_t lastSbdActiveTime;

extern char mustSendLoad;
extern hTab hostModelTbl;

extern char *env_dir;
extern struct hostNode **candidates;
extern u_int  loadVecSeqNo;
extern u_int  masterAnnSeqNo;
extern struct hostNode *fromHostPtr;
extern struct lsInfo allInfo;
extern struct shortLsInfo shortInfo;
extern int clientHosts[];
extern struct floatClientInfo floatClientPool;
extern int ncpus;
extern struct clientNode  *clientMap[];

extern pid_t elim_pid;
extern pid_t pimPid;

extern char  ignDedicatedResource;
extern struct limLock limLock;
extern int  numHostResources;
extern struct sharedResource **hostResources;

extern u_short lsfSharedCkSum;

extern int numMasterCandidates;
extern int isMasterCandidate;
extern int limConfReady;
extern int kernelPerm;

extern unitTypes    unitForLimits;



extern int readShared(void);
extern int readCluster(int);
extern void reCheckRes(void);
extern int reCheckClass(void);
extern void setMyClusterName(void);
extern int resNameDefined(char *);
extern struct sharedResource * inHostResourcs (char *);
extern struct resourceInstance *isInHostList (struct sharedResource *,  char *);
extern struct hostNode *initHostNode(void);
extern void freeHostNodes (struct hostNode *, int);
extern int validResource(const char *);
extern int validLoadIndex(const  char *);
extern int validHostType(const char *);
extern int validHostModel(const char *);
extern char *stripIllegalChars(char *);
extern int initTypeModel(struct hostNode *);
extern char *getHostType(void);
extern struct hostNode *addFloatClientHost(struct hostent *);
extern int removeFloatClientHost(struct hostNode *);
extern void slaveOnlyInit(int checkMode, int *kernelPerm);
extern int slaveOnlyPreConf();
extern int slaveOnlyPostConf(int checkMode, int *kernelPerm);
extern int typeNameToNo(const char *);
extern int archNameToNo(const char *);


extern void reconfigReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void shutdownReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void limDebugReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void lockReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern int limPortOk(struct sockaddr_in *);
extern void servAvailReq(XDR *, struct hostNode *, struct sockaddr_in *, struct LSFHeader *);

extern void pingReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void clusNameReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void masterInfoReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void hostInfoReq(XDR *, struct hostNode *, struct sockaddr_in *,
			struct LSFHeader *, int);
extern void infoReq(XDR *, struct sockaddr_in *, struct LSFHeader *, int);
extern void cpufReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void clusInfoReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void resourceInfoReq(XDR *, struct sockaddr_in *, struct LSFHeader *, int);
extern void masterRegister(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void jobxferReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void rcvConfInfo(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void tellMasterWho(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void whoIsMaster(struct clusterNode *);
extern void announceMaster(struct clusterNode *, char, char);
extern void wrongMaster(struct sockaddr_in *, char *, struct LSFHeader *, int);
extern void checkHostWd(void);
extern void announceMasterToHost(struct hostNode *, int);
extern int  probeMasterTcp(struct clusterNode *);
extern void initNewMaster(void);
extern int  callMasterTcp(enum limReqCode, struct hostNode *, void *, bool_t(*)(), void *, bool_t(*)(), int, int, struct LSFHeader *);
extern int validateHost(char *, int);
extern int validateHostbyAddr(struct sockaddr_in *, int);
extern void checkAfterHourWindow();

extern void sendLoad(void);
extern void rcvLoad(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void copyIndices(float *, int , int, struct hostNode *);
extern float normalizeRq(float rawql, float cpuFactor, int nprocs);
extern struct resPair * getResPairs (struct hostNode *);
extern void satIndex(void);
extern void loadIndex(void);
extern void initReadLoad(int, int *);
extern void initConfInfo(void);
extern void readLoad(int);
extern const char* getHostModel(void);

extern void getLastActiveTime(void);
extern void putLastActiveTime(void);

extern void lim_Exit(const char *fname);
extern int equivHostAddr(struct hostNode *, u_int);
extern struct hostNode *findHost(char *);
extern struct hostNode *findHostbyAddr(struct sockaddr_in *,
                                       char *);
extern struct hostNode *findHostByAddr(in_addr_t);
extern struct hostNode *rmHost(struct hostNode *);
extern struct hostNode *findHostbyList(struct hostNode *, char *);
extern struct hostNode *findHostbyNo(struct hostNode *, int);
extern bool_t findHostInCluster(char *);
extern int  definedSharedResource(struct hostNode *, struct lsInfo *);
extern struct shortLsInfo *shortLsInfoDup(struct shortLsInfo *);
extern void shortLsInfoDestroy(struct shortLsInfo *);
extern void errorBack(struct sockaddr_in *, struct LSFHeader *,
                      enum limReplyCode, int);
extern int initSock(int);
extern void initLiStruct(void);
extern void placeReq(XDR *, struct sockaddr_in *, struct LSFHeader *, int);
extern void loadadjReq(XDR *, struct sockaddr_in *, struct LSFHeader *, int);
extern void updExtraLoad(struct hostNode **, char *, int);
extern void loadReq(XDR *, struct sockaddr_in *, struct LSFHeader *,
                    int);
extern int getEligibleSites(register struct resVal*, struct decisionReq *,
                            char, char *);
extern int validHosts(char **, int, char *, int);
extern int checkValues(struct resVal *, int);
extern void chkResReq(XDR *, struct sockaddr_in *, struct LSFHeader *);
extern void getTclHostData (struct tclHostData *, struct hostNode *,
                            struct hostNode *, int);
extern void reconfig(void);
extern void shutdownLim(void);
extern int xdr_loadvector(XDR *, struct loadVectorStruct *,
                          struct LSFHeader *);
extern int xdr_loadmatrix(XDR *, int, struct loadVectorStruct *,
                          struct LSFHeader *);
extern int xdr_masterReg(XDR *, struct masterReg *, struct LSFHeader *);
extern int xdr_statInfo(XDR *, struct statInfo *, struct LSFHeader *);
extern void clientIO(struct epoll_event *, int);

/* openlava floating host management
 */
extern void addMigrantHost(XDR *,
                           struct sockaddr_in *,
                           struct LSFHeader *,
                           int);
extern void rmMigrantHost(XDR *,
                          struct sockaddr_in *,
                          struct LSFHeader *,
                          int);
extern int logInit(void);
extern int logLIMStart(void);
extern int logLIMDown(void);
extern int logAddHost(struct hostEntry *);
extern int logRmHost(struct hostEntry *);
extern int addHostByTab(hTab *);

#endif

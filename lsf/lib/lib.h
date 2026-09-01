/* $Id: lib.h 397 2007-11-26 19:04:00Z mblack $
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

#include "../lsf.h"
#include "../lim/limout.h"
#include "../res/resout.h"
#include "lib.hdr.h"
#include "lib.xdrlim.h"
#include "lib.xdr.h"
#include "mls.h"

struct taskMsg {
    char *inBuf;
    char *outBuf;
    int len;
};

extern struct lsQueue * requestQ;
extern unsigned int requestSN;

enum lsTMsgType {
    LSTMSG_DATA,
    LSTMSG_IOERR,
    LSTMSG_EOF
};

struct lsTMsgHdr {
    enum lsTMsgType type;
    char *msgPtr;
    int len;
};

struct tid {
    int rtid;
    int sock;
    char *host;
    struct lsQueue *tMsgQ;
    bool_t isEOF;
    int refCount;
    int pid;
    u_short taskPort;
    struct tid *link;
};

#define getpgrp(n)	getpgrp()

#ifndef LOG_PRIMASK
#define LOG_PRIMASK     0xf
#define LOG_MASK(pri)   (1 << (pri))
#define LOG_UPTO(pri)   ((1 << ((pri)+1)) - 1)
#endif

#ifndef LOG_PRI
#define LOG_PRI(p)      ((p) & LOG_PRIMASK)
#endif


#define MIN_REF_NUM          1000
#define MAX_REF_NUM          32760

#define packshort_(buf, x)       memcpy(buf, (char *)&(x), sizeof(short))
#define packint_(buf, x)         memcpy(buf, (char *)&(x), sizeof(int))
#define pack_lsf_rlim_t_(buf, x) memcpy(buf, (char *)&(x), sizeof(lsf_rlim_t))

typedef enum {
    LSF_CONFDIR,
    LSF_SERVERDIR,
    LSF_LIM_DEBUG,
    LSF_RES_DEBUG,
    LSF_STRIP_DOMAIN,
    LSF_LIM_PORT,
    LSF_RES_PORT,
    LSF_LOG_MASK,
    LSF_SERVER_HOSTS,
    LSF_AUTH,
    LSF_USE_HOSTEQUIV,
    LSF_ID_PORT,
    LSF_RES_TIMEOUT,
    LSF_API_CONNTIMEOUT,
    LSF_API_RECVTIMEOUT,
    LSF_AM_OPTIONS,
    LSF_TMPDIR,
    LSF_LOGDIR,
    LSF_SYMBOLIC_LINK,
    LSF_MASTER_LIST,
    LSF_MLS_LOG,
    LSF_INTERACTIVE_STDERR,
    NO_HOSTS_FILE,
    LSB_SHAREDIR, /* we share this with batch system */
    LSF_NON_PRIVILEGED_PORTS
} genparams_t;

#define AM_LAST  (!(genParams_[LSF_AM_OPTIONS].paramValue && \
                  strstr(genParams_[LSF_AM_OPTIONS].paramValue, \
                              AUTOMOUNT_LAST_STR)))

#define AM_NEVER (genParams_[LSF_AM_OPTIONS].paramValue && \
                  strstr(genParams_[LSF_AM_OPTIONS].paramValue, \
                              AUTOMOUNT_NEVER_STR))


#define LOOP_ADDR       0x7F000001

#define _NON_BLOCK_         0x01
#define _LOCAL_             0x02
#define _USE_TCP_           0x04
#define _KEEP_CONNECT_      0x08
#define _USE_PRIMARY_       0x10
#define _USE_PPORT_         0x20
/* openlava call LSF_SERVER_HOSTS regardless of local lim.
 */
#define _SERVER_HOSTS_ONLY_       0x40

#define PRIMARY    0
#define MASTER     1
#define UNBOUND    2
#define TCP        3

#define RES_TIMEOUT 120
#define NIOS_TIMEOUT 120

#define NOCODE 10000

#define MAXCONNECT    256

#define RSIG_ID_ISTID   0x01

#define RSIG_ID_ISPID   0x02

#define RSIG_KEEP_CONN  0x04

#define RID_ISTID       0x01
#define RID_ISPID       0x02

#define NO_SIGS (~(sigmask(SIGTRAP) | sigmask(SIGEMT)))
#define SET_LSLIB_NIOS_HDR(hdr,opcode,l) \
    { (hdr).opCode = (opcode); (hdr).len = (l); }

#define CLOSEFD(s) if ((s) >= 0) {close((s)); (s) = -1;}

extern struct sockaddr_in sockIds_[];
extern int limchans_[];

extern struct config_param genParams_[];
extern struct sockaddr_in limSockId_;
extern struct sockaddr_in limTcpSockId_;
extern char rootuid_;
extern struct sockaddr_in res_addr_;
extern fd_set connection_ok_;
extern int    currentsocket_;
extern int    totsockets_;
extern int    cli_nios_fd[];
extern short  nios_ok_;
extern struct masterInfo masterInfo_;
extern int    masterknown_;
extern char   *indexfilter_;
extern char   *stripDomains_;
extern hTab   conn_table;

extern char **environ;

extern void initconntbl_(void);
extern void inithostsock_(void);
extern int connected_(char *, int, int, int);
extern void hostIndex_(char *, int);
extern int gethostbysock_(int, char *);
extern int _isconnected_(char *, int *);
extern int _getcurseqno_(char *);
extern void _setcurseqno_(char *, int);
extern void _lostconnection_(char *);
extern int _findmyconnections_(struct connectEnt **);

extern int setLockOnOff_(int, time_t, char *hname);

typedef struct lsRequest LS_REQUEST_T;

typedef struct lsRequest requestType;
typedef int (*requestCompletionHandler) (requestType *);
typedef int (*appCompletionHandler) (LS_REQUEST_T *, void *);


struct lsRequest {
    int tid;
    int seqno;
    int connfd;
    int rc;
    int completed;
    void *extra;
    void *replyBuf;
    int replyBufLen;
    requestCompletionHandler replyHandler;
    appCompletionHandler appHandler;
    void *appExtra;
};

extern struct lsRequest * lsReqHandCreate_(int,int,int,void*,requestCompletionHandler,appCompletionHandler, void *);
extern void lsReqHandDestroy_(struct lsRequest *);

extern int     lsConnWait_ (char *);
extern int     lsMsgWait_ (int, int *, int *, int, int*, int *, int *,
			      struct timeval *, int);
extern int     lsMsgRdy_ (int, int *);
extern int     lsMsgRcv_ (int, char *, int, int);
extern int     lsMsgSnd_ (int, char *, int, int);
extern int     lsMsgSnd2_ (int *, int, char *, int, int);
extern int     lsReqTest_ (LS_REQUEST_T *);
extern int     lsReqWait_ (LS_REQUEST_T *, int);
extern void    lsReqFree_ (LS_REQUEST_T *);
extern int     lsRSig_ (char *, int, int, int);
extern int     lsRGetpid_ (int, int);
extern void   *lsRGetpidAsync_ (int, int *);
extern LS_REQUEST_T *
               lsIRGetRusage_ (int, struct jRusage *,
			       appCompletionHandler,
			       void *, int);
extern int     lsRGetRusage (int, struct jRusage *, int);
extern int     lsGetRProcRusage(char *, int, struct jRusage *, int);
extern LS_REQUEST_T *
               lsGetIRProcRusage_(char *, int, int, struct jRusage *,
				 appCompletionHandler, void *);

extern int initenv_(struct config_param *, char *);
extern int readconfenv_(struct config_param *, struct config_param *, char *);
extern int ls_readconfenv(struct config_param *, char *);

extern int callLim_(enum limReqCode, void *, bool_t (*)(), void *, bool_t (*)(), char *, int, struct LSFHeader *);
extern int initLimSock_(void);

extern void err_return_(enum limReplyCode);

extern struct hostLoad *loadinfo_(char *, struct decisionReq *, char *, int *, char ***);

extern struct hostent *Gethostbyname_(char *);
extern short getRefNum_(void);
extern char isint_(char *);
extern char islongint_(char *);
extern int isdigitstr_(char *);
extern LS_LONG_INT    atoi64_(char *);
extern char *chDisplay_(char *);
extern void strToLower_(char *);
extern void initLSFHeader_(struct LSFHeader *);
extern int  isMasterCrossPlatform(void);
extern int  isAllowCross(char *);

extern char **placement_(char *, struct decisionReq *, char *, int *);

extern int sig_encode(int);
extern int sig_decode(int);
extern int getSigVal (char *);
extern char *getSigSymbolList(void);
extern char *getSigSymbol (int);

typedef struct svrsock {
    int  sockfd;
    int  port;
    struct sockaddr_in *localAddr;
    int  backlog;
    int  options;
} ls_svrsock_t;


#define LS_CSO_ASYNC_NT       (0x0001)
#define LS_CSO_PRIVILEGE_PORT (0x0002)

extern int setLSFChanSockOpt_(int newOpt);

extern int CreateSock_(int);
extern int CreateSockEauth_(int);
extern int Socket_(int, int, int);
extern int get_nonstd_desc_(int);
extern int TcpCreate_(int, int);
extern int opensocks_(int);
extern ls_svrsock_t *svrsockCreate_(u_short, int, struct sockaddr_in *, int);
extern int svrsockAccept_(ls_svrsock_t *, int);
extern char *svrsockToString_(ls_svrsock_t *);
extern void svrsockDestroy_(ls_svrsock_t *);
extern int TcpConnect_(char *, u_short, struct timeval *);

extern char *getMsgBuffer_(int, int *);

extern int expSyntax_(char *);

extern int tid_register(int, int, u_short, char *, int);
extern int tid_remove(int);
extern struct tid *tid_find(int);
extern struct tid *tidFindIgnoreConn_(int);
extern void tid_lostconnection(int);
extern int tidSameConnection_(int, int *, int **);

extern int callRes_(int, resCmd, char *, char *, int,
		    bool_t (*)(), int *, struct timeval *, struct lsfAuth *);
extern int sendCmdBill_(int, resCmd, struct resCmdBill *,
				    int *, struct timeval *);
extern void ls_errlog(FILE *fd, const char *fmt, ...)
#if defined(__GNUC__) && defined(CHECK_PRINTF)

	__attribute__((format(printf, 2, 3)))
#endif
	;

extern void ls_verrlog(FILE *fd, const char *fmt, va_list ap);

extern int isPamBlockWait ;


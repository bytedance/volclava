/* $Id: lib.res.c 397 2007-11-26 19:04:00Z mblack $
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
#include <fcntl.h>
#include <termios.h>
#include <limits.h>
#include <sys/wait.h>
#include <pwd.h>
#include "lib.h"
#include "lib.queue.h"
#include "lproto.h"

#define SIGEMT SIGBUS

extern char **environ;
extern int gethostbysock_(int, char *);

int lsf_res_version = -1;
int totsockets_ = 0;
int currentsocket_;
struct sockaddr_in res_addr_;
fd_set connection_ok_;
struct lsQueue *requestQ;

unsigned int requestSN = 0;

unsigned int requestHighWaterMark = 0;

#define REQUESTSN	((requestSN < USHRT_MAX) ? requestSN++ : (requestSN=11 , 10))

unsigned int currentSN;

int do_rstty1_(char *host, int async);
int do_rstty2_(int s, int io_fd, int redirect, int async);

static int getLimits(struct lsfLimit *);

static int mygetLimits(struct lsfLimit *);

int
ls_connect(char *host)
{
    struct  hostent *hp;
    int     s, descriptor[2], size;
    char    official[MAXHOSTNAMELEN];
    struct resConnect connReq;
    char *reqBuf;
    struct lsfAuth auth;
    int resTimeout;



    if (genParams_[LSF_RES_TIMEOUT].paramValue)
	resTimeout = atoi(genParams_[LSF_RES_TIMEOUT].paramValue);
    else
	resTimeout = RES_TIMEOUT;

    if (_isconnected_(host, descriptor))
        return(descriptor[0]);

    if ((hp = Gethostbyname_(host)) == NULL) {
        lserrno = LSE_BAD_HOST;
        return (-1);
    }

    strcpy(official, hp->h_name);
    memcpy((char *)&res_addr_.sin_addr,(char *)hp->h_addr,(int)hp->h_length);
    if ((rootuid_) && (genParams_[LSF_AUTH].paramValue == NULL)) {
        if (currentsocket_ > (FIRST_RES_SOCK + totsockets_ - 1)) {
            lserrno = LSE_NOMORE_SOCK;
            return (-1);
        }
        s = currentsocket_;
        currentsocket_++;
    } else {
        if ((s = CreateSockEauth_(SOCK_STREAM)) < 0)
            return (-1);
    }

    putEauthClientEnvVar("user");
    putEauthServerEnvVar("res");

#ifdef INTER_DAEMON_AUTH

    putEnv("LSF_EAUTH_AUX_PASS", "yes");
#endif


    if (getAuth_(&auth, official) == -1) {
	closesocket(s);
	return (-1);
    }

    runEsub_(&connReq.eexec, NULL);

    size = sizeof(struct LSFHeader) + sizeof(connReq) +
	   sizeof(struct lsfAuth) +
	   ALIGNWORD_(connReq.eexec.len) +
	   sizeof(int) * 5 ;

    if ((reqBuf = malloc(size)) == NULL) {
	lserrno = LSE_MALLOC;
	goto Fail;
    }

    if (b_connect_(s, (struct sockaddr *)&res_addr_,
				 sizeof(res_addr_), resTimeout) < 0) {
	lserrno = LSE_CONN_SYS;
	goto Fail;
    }

    if (callRes_(s, RES_CONNECT, (char *) &connReq, reqBuf,
		 size, xdr_resConnect, 0, 0, &auth) == -1) {
	goto Fail;
    }

    if (connReq.eexec.len > 0)
	free(connReq.eexec.data);

    free(reqBuf);

    (void)connected_(official, s, -1, currentSN);

    return(s);

  Fail:
    CLOSESOCKET(s);

    if (connReq.eexec.len > 0)
	free(connReq.eexec.data);

    free(reqBuf);
    return (-1);
}

int
lsConnWait_(char *host)
{
    int s;
    int descriptor[2];

    if (_isconnected_(host, descriptor))
        s = descriptor[0];
    else
        return (-1);

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0){
	    closesocket(s);
	    _lostconnection_(host);
	    return (-1);
	}
    }

    return(0);
}

int
sendCmdBill_ (int s, resCmd cmd, struct resCmdBill *cmdmsg, int *retsock,
	     struct timeval *timeout)

{
    char *buf;
    int  i,bufsize,cc;

    bufsize = 1024;
    bufsize = bufsize + ALIGNWORD_(strlen(cmdmsg->cwd)) + sizeof(int);
    for (i=0; cmdmsg->argv[i]; i++)
        bufsize = bufsize + ALIGNWORD_(strlen(cmdmsg->argv[i])) + sizeof(int);
    if ((buf=malloc(bufsize)) == NULL) {
        lserrno = LSE_MALLOC;
        return(-1);
    }

    umask(cmdmsg->filemask = (int)umask(0));
    cmdmsg->priority = 0;
    if (getLimits(cmdmsg->lsfLimits) < 0) {
	lserrno = LSE_LIMIT_SYS;
        free(buf);
	return(-1);
    }
    cc=callRes_(s, cmd, (char *) cmdmsg, buf, bufsize, xdr_resCmdBill,
		retsock, timeout, NULL);
    free(buf);
    return(cc);

}

FILE *
ls_popen(int s, char *command, char *type)
{
    return((FILE *)0);
}

int
ls_pclose(FILE *stream)
{
    return(0);
}

#define RSETENV_SYNCH	1
#define RSETENV_ASYNC   2
int
rsetenv_ (char *host, char **envp, int option)
{
    int    descriptor[2];
    struct resSetenv envMsg;
    char   *sendBuf;
    int    bufferSize;
    int    i, s;
    resCmd resCmdOption;

    bufferSize = 512;

    if (logclass & (LC_TRACE))
	ls_syslog(LOG_DEBUG, "rsetenv_: Entering this routine...");

    if (!envp)
        return(0);

    for(i=0; envp[i] != NULL; i++)
        bufferSize = bufferSize + ALIGNWORD_(strlen(envp[i])) + sizeof(int);

    sendBuf = (char *)malloc(bufferSize);
    if (sendBuf == NULL) {
        lserrno = LSE_MALLOC;
        goto err;
    }

    if (_isconnected_(host, descriptor))
	s = descriptor[0];
    else if ((s = ls_connect(host)) < 0) {
        free(sendBuf);
	goto err;
    }

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0) {
	   closesocket(s);
	   _lostconnection_(host);
           free(sendBuf);
	   goto err;
        }
    }

    envMsg.env = envp;
    if (option == RSETENV_SYNCH)
        resCmdOption = RES_SETENV;
    else if (option == RSETENV_ASYNC)
        resCmdOption = RES_SETENV_ASYNC;
    if (callRes_(s, resCmdOption, (char *) &envMsg, sendBuf, bufferSize,
		 xdr_resSetenv, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
        free(sendBuf);
        goto err;
    }
    free(sendBuf);

    if (option == RSETENV_SYNCH){
        if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
            goto err;
        }
    }

    return(0);

err:

    return(-1);
}

int
ls_rsetenv_async (char *host, char **envp)
{
    return rsetenv_ ( host, envp, RSETENV_ASYNC);
}

int
ls_rsetenv(char *host, char **envp)
{
    return rsetenv_ ( host, envp, RSETENV_SYNCH);
}

int
ls_chdir(char *host, char *dir)
{
    int s, descriptor[2];
    struct {
	struct LSFHeader hdr;
	struct resChdir ch;
    } buf;
    struct resChdir chReq;

    if (_isconnected_(host, descriptor))
	s = descriptor[0];
    else if ((s = ls_connect(host)) < 0)
	return(-1);

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0) {
	   closesocket(s);
	   _lostconnection_(host);
	   return (-1);
        }
    }

    if (dir[0] != '/' && dir[1] != ':' && (dir[0] != '\\' || dir[1] != '\\')) {
        lserrno = LSE_BAD_ARGS;
        return(-1);
    }

    strcpy(chReq.dir, dir);

    if (callRes_(s, RES_CHDIR, (char *) &chReq, (char *) &buf,
		 sizeof(buf), xdr_resChdir, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
        return (-1);
    }

    if (ackReturnCode_(s) < 0) {
        if (lserrno == LSE_RES_DIRW)
            return(-2);
        else
            return (-1);
    }

    return(0);
}

struct lsRequest *
lsReqHandCreate_(int tid,
		 int seqno,
		 int connfd,
		 void *extra,
		 requestCompletionHandler replyHandler,
		 appCompletionHandler appHandler,
		 void *appExtra)
{
    struct lsRequest *request;

    request = (struct lsRequest *)malloc(sizeof(struct lsRequest));

    if (! request) {
	lserrno = LSE_MALLOC;
	return(NULL);
    }

    request->tid = tid;
    request->seqno = seqno;
    request->connfd = connfd;
    request->completed = FALSE;
    request->extra = extra;
    request->replyHandler = replyHandler;
    request->appHandler = appHandler;
    request->appExtra = appExtra;

    return(request);
}

int
ackAsyncReturnCode_(int s, struct LSFHeader *replyHdr)
{
    char                  cseqno;
    int                   seqno;
    int                   len;
    int                   rc;
    struct lsQueueEntry   *reqEntry;
    struct lsRequest      *reqHandle;

    seqno = replyHdr->refCode;
    cseqno = seqno;
    reqEntry = lsQueueSearch_(0, &cseqno, requestQ);
    if (reqEntry == NULL) {
	lserrno = LSE_PROTOC_RES;
	return(-1);
    }

    lsQueueEntryRemove_(reqEntry);
    reqHandle = (struct lsRequest *)reqEntry->data;

    reqEntry->data = NULL;

    reqHandle->rc = replyHdr->opCode;
    reqHandle->completed = TRUE;
    if (replyHdr->length > 0) {
	reqHandle->replyBuf = malloc(replyHdr->length);
	if (reqHandle->replyBuf == NULL) {
	    lserrno = LSE_MALLOC;
            return(-1);
	}

	len = b_read_fix(s, reqHandle->replyBuf, replyHdr->length);
	if (len != replyHdr->length) {
	    free(reqHandle->replyBuf);
	    lserrno = LSE_MSG_SYS;
	    return(-1);
	}
	reqHandle->replyBufLen = len;
    }

    if (reqHandle->replyHandler) {
	rc = (*(reqHandle->replyHandler))(reqHandle);
	if (replyHdr->length > 0)
	    free(reqHandle->replyBuf);
	if (rc < 0) {
	    if (reqHandle->appHandler)
		rc=(*(reqHandle->appHandler))(reqHandle, reqHandle->appExtra);

	    lsQueueEntryDestroy_(reqEntry, requestQ);
	    return(rc);
	}
    }

    if (reqHandle->appHandler)
	rc = (*(reqHandle->appHandler))(reqHandle, reqHandle->appExtra);

    lsQueueEntryDestroy_(reqEntry, requestQ);
    return (rc);

}

int
enqueueTaskMsg_(int s, int taskID, struct LSFHeader *msgHdr)
{
    struct tid *tEnt;
    char *msgBuf;
    struct lsTMsgHdr *header;

    tEnt = tid_find(taskID);
    if (tEnt == NULL) {
	return(-1);
    }

    header = (struct lsTMsgHdr *)malloc(sizeof(struct lsTMsgHdr));
    if (! header) {
	lserrno = LSE_MALLOC;
	return(-1);
    }

    header->len = 0;
    header->msgPtr = NULL;
    if (s < 0) {
	header->type = LSTMSG_IOERR;
	lsQueueDataAppend_((char *)header, tEnt->tMsgQ);
	return (0);
    }

    /* or 0xffff ?
     */
    if (msgHdr->reserved == 0
	&& msgHdr->length == 0)
    {
	header->type = LSTMSG_EOF;
	lsQueueDataAppend_((char *)header, tEnt->tMsgQ);
	return(0);
    }


    if (msgHdr->length == 0)
        msgBuf = malloc(1);
    else
        msgBuf = malloc(msgHdr->length);
    if (msgBuf == NULL) {
	lserrno = LSE_MALLOC;
	return(-1);
    }

    if (b_read_fix(s, (char *)msgBuf, msgHdr->length) != msgHdr->length) {
	free(msgBuf);
	lserrno = LSE_MSG_SYS;
	return(-1);
    }

    header->type = LSTMSG_DATA;
    header->len = msgHdr->length;
    header->msgPtr = msgBuf;

    lsQueueDataAppend_((char *)header, tEnt->tMsgQ);

    return(0);
}

int
expectReturnCode_(int s, int seqno, struct LSFHeader *repHdr)
{
    struct LSFHeader buf;
    static char fname[] = "expectReturnCode_";
    XDR xdrs;
    int rc;

    xdrmem_create(&xdrs, (char *) &buf, sizeof(struct LSFHeader), XDR_DECODE);
    for (;;) {
	if (logclass & LC_TRACE)
	    ls_syslog(LOG_DEBUG, "%s: calling readDecodeHdr_...", fname);
	xdr_setpos(&xdrs, 0);
	if (readDecodeHdr_(s, (char *) &buf, b_read_fix, &xdrs, repHdr) < 0) {
	    xdr_destroy(&xdrs);
	    return(-1);
	}

	if (repHdr->opCode == RES_NONRES)  {
	    rc = enqueueTaskMsg_(s, repHdr->refCode, repHdr);
	    if (rc < 0) {
		xdr_destroy(&xdrs);
	        return(rc);
	    }
	} else {
	    if (repHdr->refCode == seqno)
	        break;

	    rc = ackAsyncReturnCode_(s, repHdr);
	    if (rc < 0) {
		xdr_destroy(&xdrs);
		return(rc);
	    }
	}
    }
    xdr_destroy(&xdrs);
    return(0);

}

int
resRC2LSErr_(int resRC)
{
    switch (resRC) {
      case RESE_OK:
	 lserrno = 0;
	 break;
      case RESE_NOMORECONN:
	 lserrno = LSE_RES_NOMORECONN;
	 break;
      case RESE_BADUSER:
	 lserrno = LSE_BADUSER;
	 break;
      case RESE_ROOTSECURE:
	 lserrno = LSE_RES_ROOTSECURE;
	 break;
      case RESE_DENIED:
	 lserrno = LSE_RES_DENIED;
	 break;
      case RESE_REQUEST:
	 lserrno = LSE_PROTOC_RES;
	 break;
      case RESE_CALLBACK:
	 lserrno = LSE_RES_CALLBACK;
	 break;
      case RESE_NOMEM:
	 lserrno = LSE_RES_NOMEM;
	 break;
      case RESE_FATAL:
	 lserrno = LSE_RES_FATAL;
	 break;
      case RESE_CWD:
         lserrno = LSE_RES_DIR;
         break;
      case RESE_PTYMASTER:
      case RESE_PTYSLAVE:
	 lserrno = LSE_RES_PTY;
	 break;
      case RESE_SOCKETPAIR:
	 lserrno = LSE_RES_SOCK;
	 break;
      case RESE_FORK:
	 lserrno = LSE_RES_FORK;
	 break;
      case RESE_INVCHILD:
	 lserrno = LSE_RES_INVCHILD;
	 break;
      case RESE_KILLFAIL:
	 lserrno = LSE_RES_KILL;
	 break;
      case RESE_VERSION:
	 lserrno = LSE_RES_VERSION;
	 break;
      case RESE_DIRW:
         lserrno = LSE_RES_DIRW;
         break;
      case RESE_NOLSF_HOST:
	 lserrno = LSE_NLSF_HOST;
	 break;
      case RESE_RUSAGEFAIL:
	 lserrno = LSE_RES_RUSAGE;
	 break;
      case RESE_RES_PARENT:
	 lserrno = LSE_RES_PARENT;
	 break;
      case RESE_MLS_INVALID:
	 lserrno = LSE_MLS_INVALID;
	 break;
      case RESE_MLS_CLEARANCE:
	 lserrno = LSE_MLS_CLEARANCE;
	 break;
      case RESE_MLS_DOMINATE:
	 lserrno = LSE_MLS_DOMINATE;
	 break;
      case RESE_MLS_RHOST:
	 lserrno = LSE_MLS_RHOST;
	 break;
      default:
	 lserrno = NOCODE + resRC;
     }
    return(lserrno);

}

int
ackReturnCode_(int s)
{
    struct LSFHeader repHdr;
    int rc;
    char hostname[MAXHOSTNAMELEN];
    static char fname[] = "ackReturnCode_";

    if (logclass & (LC_TRACE))
	ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    gethostbysock_(s, hostname);
    currentSN = _getcurseqno_(hostname);
    rc = expectReturnCode_(s, currentSN, &repHdr);
    if (rc < 0)
        return(rc);

    lsf_res_version = (int)repHdr.version;

    rc = resRC2LSErr_(repHdr.opCode);

    if (rc == 0)
        return(0);
    else
	return(-1);

}

static int
getLimits(struct lsfLimit *limits)
{
    return (mygetLimits(limits));
}
static int
mygetLimits(struct lsfLimit *limits)
{
    int i;
    struct rlimit rlimit;

    for (i = 0; i < LSF_RLIM_NLIMITS; i++) {
	rlimit.rlim_cur = RLIM_INFINITY;
	rlimit.rlim_max = RLIM_INFINITY;
	rlimitEncode_(&limits[i], &rlimit, i);
    }

#ifdef  RLIMIT_CPU
    if (getrlimit(RLIMIT_CPU, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_CPU], &rlimit, LSF_RLIMIT_CPU);
#endif

#ifdef  RLIMIT_FSIZE
    if (getrlimit(RLIMIT_FSIZE, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_FSIZE], &rlimit, LSF_RLIMIT_FSIZE);
#endif

#ifdef RLIMIT_DATA
    if (getrlimit(RLIMIT_DATA, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_DATA], &rlimit, LSF_RLIMIT_DATA);
#endif

#ifdef RLIMIT_STACK
    if (getrlimit(RLIMIT_STACK, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_STACK], &rlimit, LSF_RLIMIT_STACK);
#endif

#ifdef RLIMIT_CORE
    if (getrlimit(RLIMIT_CORE, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_CORE], &rlimit, LSF_RLIMIT_CORE);
#endif

#ifdef RLIMIT_NOFILE
    if (getrlimit(RLIMIT_NOFILE, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_NOFILE], &rlimit, LSF_RLIMIT_NOFILE);
#endif

#ifdef RLIMIT_OPEN_MAX
    if (getrlimit(RLIMIT_OPEN_MAX, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_OPEN_MAX], &rlimit, LSF_RLIMIT_OPEN_MAX);
#endif

#ifdef RLIMIT_VMEM
    if (getrlimit(RLIMIT_VMEM, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_VMEM], &rlimit, LSF_RLIMIT_VMEM);
#endif

#ifdef RLIMIT_RSS
    if (getrlimit(RLIMIT_RSS, &rlimit) < 0)
	return -1;
    rlimitEncode_(&limits[LSF_RLIMIT_RSS], &rlimit, LSF_RLIMIT_RSS);
#endif

    return (0);
}

int callRes_(int s,
             resCmd cmd,
             char *data,
             char *reqBuf,
             int reqLen,
	     bool_t (*xdrFunc)(),
             int *rd,
             struct timeval *timeout,
	     struct lsfAuth *auth)
{
    int                  cc;
    int                  t;
    struct sockaddr_in   from;
    socklen_t            fromsize;
    int                  nready;
    sigset_t             oldMask;
    sigset_t             newMask;
    char                 hostname[MAXHOSTNAMELEN];

    blockALL_SIGS_(&newMask, &oldMask);

    memset(hostname, 0, sizeof(hostname));

    currentSN = REQUESTSN;

    gethostbysock_(s, hostname);
    if (strcmp(hostname, "LSF_HOST_NULL"))
	_setcurseqno_(hostname, currentSN);

    if ((cc = lsSendMsg_(s,
                         cmd,
                         0,
                         data,
                         reqBuf,
                         reqLen,
                         xdrFunc,
			 b_write_fix,
                         auth)) < 0) {
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return (-1);
    }

    if (!rd) {
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return (0);
    }

    nready = rd_epoll_(*rd, timeout);
    if (nready <= 0) {
        if (nready == 0)
            lserrno = LSE_TIME_OUT;
        else
            lserrno = LSE_SELECT_SYS;
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return (-1);
    }

    fromsize = sizeof(from);
    t = b_accept_(*rd, (struct sockaddr *)&from, &fromsize);
    if (t < 0) {
        lserrno = LSE_ACCEPT_SYS;
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return (-1);
    }

    closesocket(*rd);
    *rd = t;

    sigprocmask(SIG_SETMASK, &oldMask, NULL);

    return (0);

} /* callRes_() */

int
ls_rstty(char *host)
{
    return (0);
}

int
rstty_(char *host)
{
    return do_rstty1_(host,FALSE);
}

int
rstty_async_(char *host)
{

    return do_rstty1_(host,TRUE);
}

int
do_rstty1_(char *host, int async)
{
    int s, descriptor[2];
    int redirect, io_fd;

    redirect = 0;
    if (isatty(0)) {
	if (!isatty(1))
	    redirect = 1;
	io_fd = 0;
    } else if (isatty(1)) {
	redirect = 1;
	io_fd = 1;
    } else
	return(0);

    if (_isconnected_(host, descriptor))
	s = descriptor[0];
    else if ((s = ls_connect(host)) < 0)
	return(-1);

    if (! FD_ISSET(s,&connection_ok_) ) {
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
	    return (-1);
        }
    }

    if (do_rstty2_(s, io_fd, redirect, async) == -1) {
	closesocket(s);
	_lostconnection_(host);
        return (-1);
    }

    if (!async){
        if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
            return (-1);
        }
    }

    return (0);
}


int
do_rstty_(int s, int io_fd, int redirect)
{
    return do_rstty2_(s, io_fd, redirect, FALSE);
}

int
do_rstty2_(int s, int io_fd, int redirect, int async)
{
    char buf[MSGSIZE];
    char *cp;

    static int termFlag = FALSE;
    static struct resStty tty;

    if (!termFlag) {
        termFlag = TRUE;
	tcgetattr(io_fd, &tty.termattr);

	if (getpgrp(0) != tcgetpgrp(io_fd)) {
	    tty.termattr.c_cc[VEOF] = 04;
	    tty.termattr.c_lflag |= ICANON;
	    tty.termattr.c_lflag |= ECHO;
	}
	if (redirect)
	    tty.termattr.c_lflag &= ~ECHO;

	if ((cp = getenv("LINES")) != NULL)
	    tty.ws.ws_row = atoi(cp);
	else
	    tty.ws.ws_row = 24;
	if ((cp = getenv("COLUMNS")) != NULL)
	    tty.ws.ws_col = atoi(cp);
	else
	    tty.ws.ws_col = 80;
	tty.ws.ws_xpixel = tty.ws.ws_ypixel = 0;
    }

    if (!async) {
        if (callRes_(s, RES_INITTTY, (char *) &tty, buf, MSGSIZE,
		  xdr_resStty, 0, 0, NULL) == -1)
	    return (-1);
    } else {
        if (callRes_(s, RES_INITTTY_ASYNC, (char *) &tty, buf, MSGSIZE,
                  xdr_resStty, 0, 0, NULL) == -1)
            return (-1);
    }

    return(0);
}

int
rgetRusageCompletionHandler_(struct lsRequest *request)
{
    XDR xdrs;
    int rc;

    rc = resRC2LSErr_(request->rc);
    if (rc != 0)
	return(-1);
    xdrmem_create(&xdrs, request->replyBuf, request->replyBufLen, XDR_DECODE);
    if (! xdr_jRusage(&xdrs, (struct jRusage *)request->extra, NULL)) {
	lserrno = LSE_BAD_XDR;
	xdr_destroy(&xdrs);
	return(-1);
    }

    xdr_destroy(&xdrs);
    return(0);

}

LS_REQUEST_T *
lsIRGetRusage_(int rpid,
	       struct jRusage *ru,
	       appCompletionHandler appHandler,
	       void *appExtra,
	       int options)
{
    struct {
	struct LSFHeader hdr;
	struct resRusage rusageReq;
    } requestBuf;

    struct lsRequest *request;

    struct resRusage rusageReq;
    int s;
    struct tid *tid;
    char host[MAXHOSTNAMELEN];

    if ((tid = tid_find(rpid)) == NULL) {
        return(NULL);
    }

    s = tid->sock;
    gethostbysock_(s, host);

    if (!FD_ISSET(s,&connection_ok_)){
        FD_SET(s,&connection_ok_);
        if (ackReturnCode_(s) < 0) {
           closesocket(s);
           _lostconnection_(host);
           return (NULL);
        }
    }

    rusageReq.rid = rpid;
    if (options == 0 || (options & RID_ISTID))
        rusageReq.whatid = RES_RID_ISTID;
    else
        rusageReq.whatid = RES_RID_ISPID;

    if (callRes_(s, RES_RUSAGE, (char *)&rusageReq, (char *)&requestBuf,
		 sizeof(requestBuf), xdr_resGetRusage, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
	return(NULL);
    }

    request = lsReqHandCreate_(rpid,
			       currentSN,
			       s,
			       (void *)ru,
			       rgetRusageCompletionHandler_,
			       appHandler,
			       appExtra);

    if (request == NULL)
        return(NULL);

    if (lsQueueDataAppend_((char *)request, requestQ)) {
        lsReqFree_(request);
        return(NULL);
    }

    return(request);
}

int
lsGetRProcRusage(char *host, int pid, struct jRusage *ru, int options)
{
    struct {
	struct LSFHeader hdr;
	struct resRusage rusageReq;
    } requestBuf;

    struct lsRequest *request;
    struct resRusage rusageReq;
    int s;
    int descriptor[2];

    if (_isconnected_(host, descriptor))
      s = descriptor[0];
    else {
	lserrno = LSE_LOSTCON;
	return(-1);
    }

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
	    return (-1);
	}
    }

    rusageReq.rid = pid;
    rusageReq.whatid = RES_RID_ISPID;

    if (options & RES_RPID_KEEPPID) {
	rusageReq.options |= RES_RPID_KEEPPID;
    } else {
	rusageReq.options = 0;
    }

    if (callRes_(s, RES_RUSAGE, (char *)&rusageReq, (char *)&requestBuf,
		 sizeof(requestBuf), xdr_resGetRusage, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
	return(-1);
    }

    request = lsReqHandCreate_(pid,
			       currentSN,
			       s,
			       (void *)ru,
			       rgetRusageCompletionHandler_,
			       NULL,
			       NULL);

    if (request == NULL)
        return(-1);

    if (lsQueueDataAppend_((char *)request, requestQ))
        return(-1);


    if (! request)
	return (-1);

    if (lsReqWait_(request, 0) < 0)
        return (-1);

    lsReqFree_(request);

    return(0);

}

LS_REQUEST_T *
lsGetIRProcRusage_(char *host, int tid, int pid, struct jRusage *ru,
		 appCompletionHandler appHandler,
		 void *appExtra)
{
    struct {
	struct LSFHeader hdr;
	struct resRusage rusageReq;
    } requestBuf;

    struct lsRequest *request;
    struct resRusage rusageReq;
    int s;
    int descriptor[2];

    if (_isconnected_(host, descriptor))
      s = descriptor[0];
    else {
	lserrno = LSE_LOSTCON;
	return (NULL);
    }

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
	    return (NULL);
	}
    }

    rusageReq.rid = pid;
    rusageReq.whatid = RES_RID_ISPID;

    if (callRes_(s, RES_RUSAGE, (char *)&rusageReq, (char *)&requestBuf,
		 sizeof(requestBuf), xdr_resGetRusage, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
	return(NULL);
    }

    request = lsReqHandCreate_(tid,
			       currentSN,
			       s,
			       (void *)ru,
			       rgetRusageCompletionHandler_,
			       appHandler,
			       appExtra);

    if (request == NULL)
        return(NULL);

    if (lsQueueDataAppend_((char *)request, requestQ)) {
        lsReqFree_(request);
        return(NULL);
    }

    return(request);

}

int
lsRGetRusage(int rpid, struct jRusage *ru, int options)
{
    LS_REQUEST_T *request;

    request = lsIRGetRusage_(rpid,
			     ru,
			     (appCompletionHandler) NULL,
			     NULL,
			     options);

    if (! request)
	return (-1);

    if (lsReqWait_(request, 0) < 0)
        return (-1);

    lsReqFree_(request);

    return(0);
}

int
sendSig_(char *host, int rid, int sig, int options)
{
    struct {
        struct LSFHeader hdr;
        struct resRKill rk;
    } buf;
    struct resRKill killReq;
    int descriptor[2];
    int s;

    if (_isconnected_(host, descriptor))
      s = descriptor[0];
    else {
	lserrno = LSE_LOSTCON;
	return(-1);
    }

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);
	if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
	    return (-1);
	}
    }
    if ( sig >= NSIG || sig < 0 ){
	lserrno = LSE_BAD_ARGS;
	return( -1 );
    }

    killReq.rid = rid;

    if (options & RSIG_ID_ISTID) {
	killReq.whatid = RES_RID_ISTID;
    } else if (options & RSIG_ID_ISPID) {
	killReq.whatid = RES_RID_ISPID;
    } else {
	lserrno = LSE_BAD_ARGS;
	return (-1);
    }

    killReq.signal = sig_encode(sig);

    if (callRes_(s, RES_RKILL, (char *) &killReq, (char *) &buf,
		 sizeof(buf), xdr_resRKill, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
	return (-1);
    }

    if (ackReturnCode_(s) < 0) {
	if (options & RSIG_KEEP_CONN)
	    return(-1);

	closesocket(s);
	_lostconnection_(host);
	return( -1 );
    }
    return(0);
}

int
lsRSig_(char *host, int rid, int sig, int options)
{
    struct connectEnt *conns;
    int nconns;
    int rc;
    int i;

    if (!options)
	options = RSIG_ID_ISTID;

    if ((! (options & (RSIG_ID_ISTID | RSIG_ID_ISPID))) ||
	 rid < 0)  {

	lserrno = LSE_BAD_ARGS;
	return(-1);
    }

    if (host == NULL) {
	nconns = _findmyconnections_(&conns);

	if (nconns == 0) {
	    return(0);
	}

	for (i = 0; i < nconns; i++) {
	    host = conns[i].hostname;
	    rc = sendSig_(host, 0, sig, options);
	    if (rc < 0)
	        return(rc);
	}
    } else {
	return(sendSig_(host, rid, sig, options));
    }
    return(0);

}

int
ls_rkill(int rtid, int sig)
{
    int s;
    struct tid *tid;
    char host[MAXHOSTNAMELEN];
    int rc;

    if (rtid < 0) {
	lserrno = LSE_BAD_ARGS;
	return (-1);
    }

    if (rtid == 0) {
      	rc = lsRSig_(NULL, rtid, sig, RSIG_ID_ISTID);
	return(rc);
    }

    if ((tid = tid_find(rtid)) == NULL) {
        return(-1);
    }

    s = tid->sock;
    gethostbysock_(s, host);

    rc = lsRSig_(host, rtid, sig, RSIG_ID_ISTID);

    return(rc);

}


int
lsMsgRdy_(int taskid, int *msgLen)
{
    struct tid *tEnt;
    struct lsQueueEntry *qEnt;
    struct lsTMsgHdr *header;

    tEnt = tidFindIgnoreConn_(taskid);
    if (tEnt == NULL)
        return (-1);

    if (! LS_QUEUE_EMPTY(tEnt->tMsgQ)) {
	qEnt = tEnt->tMsgQ->start->forw;
	header = (struct lsTMsgHdr *)qEnt->data;
	if (msgLen != NULL)
	    *msgLen = header->len;
	return (1);
    } else
        return (0);

}

void
tMsgDestroy_(void *extra)
{
    struct lsTMsgHdr *header = (struct lsTMsgHdr *)extra;

    if (! header)
        return;

    if (header->msgPtr)
        free(header->msgPtr);

    free(header);

}



int
lsMsgRcv_(int taskid, char *buffer, int len, int options)
{
    struct tid *tEnt;
    struct lsQueueEntry *qEnt;
    int rc;

    tEnt = tidFindIgnoreConn_(taskid);
    if (tEnt == NULL)
        return (-1);

  Again:
    if (tEnt->isEOF) {
	lserrno = LSE_RES_INVCHILD;
	return(-1);
    }

    if (lsMsgRdy_(taskid, NULL)) {
	struct lsTMsgHdr *header;

	qEnt = lsQueueDequeue_(tEnt->tMsgQ);

	if (qEnt == NULL) {
	    lserrno = LSE_MSG_SYS;
	    return (-1);
	}

	header = (struct lsTMsgHdr *)qEnt->data;
	if (header->len > len) {
	    lserrno = LSE_MSG_SYS;

	    lsQueueEntryDestroy_(qEnt, tEnt->tMsgQ);
	    return(-1);
	}

	if (header->type == LSTMSG_IOERR) {
	    lserrno = LSE_LOSTCON;
	    lsQueueEntryDestroy_(qEnt, tEnt->tMsgQ);
	    return(-1);
	} else if (header->type == LSTMSG_EOF) {
	    tEnt->isEOF = TRUE;
	    lsQueueEntryDestroy_(qEnt, tEnt->tMsgQ);
	    return (0);
	}

	memcpy((char *)buffer, (char *)header->msgPtr, header->len);

	rc = header->len;
	lsQueueEntryDestroy_(qEnt, tEnt->tMsgQ);
	return(rc);
    } else {
	int nrdy = 0;

	if (tEnt->sock < 0) {
	    lserrno = LSE_LOSTCON;
	    tid_remove(taskid);
	    return (-1);
	}
	rc = lsMsgWait_(1, &taskid, &nrdy, 0, NULL, NULL, NULL, NULL, 0);
	if (rc < 0)
	    return (rc);

	if (nrdy > 0)
	    goto Again;
	else
	    return(-1);
    }
}

int
lsMsgSnd2_(int *sock, int opcode, char *buffer, int len, int options)
{
    struct LSFHeader header;
    char headerBuf[sizeof(struct LSFHeader)];
    XDR xdrs;
    int rc;
    char hostname[MAXHOSTNAMELEN];

    if (*sock < 0) {
        return (-1);
    }

#ifdef OS_HAS_PTHREAD
    pthread_mutex_lock(&fdLSLIBWriteMutex);
#endif

    header.opCode = opcode;
    header.refCode = currentSN = REQUESTSN;
    header.length = len;
    header.reserved = 0;

    gethostbysock_(*sock, hostname);
    if (strcmp(hostname, "LSF_HOST_NULL"))
        _setcurseqno_(hostname, currentSN);

    xdrmem_create(&xdrs, headerBuf, LSF_HEADER_LEN, XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs, &header)) {
        lserrno = LSE_BAD_XDR;
        xdr_destroy(&xdrs);
        rc = -1;
        goto AbortSnd2;
    }
    xdr_destroy(&xdrs);

    rc = b_write_fix(*sock, headerBuf, LSF_HEADER_LEN);
    if (rc < 0) {
        if (errno == EPIPE) {
            close(*sock);
            *sock = -1;
            _lostconnection_(hostname);
            lserrno = LSE_LOSTCON;
        }
        rc = -1;
        goto AbortSnd2;
    }

    rc = b_write_fix(*sock, buffer, len);
    if (rc < 0) {
        if (errno == EPIPE) {
            close(*sock);
            *sock = -1;
            _lostconnection_(hostname);
            lserrno = LSE_LOSTCON;
        }
        rc = -1;
        goto AbortSnd2;
    }
    if (ackReturnCode_(*sock) < 0) {
        rc = -1;
        goto AbortSnd2;
    }

  AbortSnd2:
#ifdef OS_HAS_PTHREAD
    pthread_mutex_unlock(&fdLSLIBWriteMutex);
#endif

    return(rc);


}

int
lsMsgSnd_(int taskid, char *buffer, int len, int options)
{
    struct LSFHeader header;
    char headerBuf[sizeof(struct LSFHeader)];
    XDR xdrs;
    struct tid *tEnt;
    int rc;
    char hostname[MAXHOSTNAMELEN];

    tEnt = tid_find(taskid);
    if (! tEnt)
	return(-1);

    if (tEnt->sock < 0) {
	lserrno = LSE_LOSTCON;
	return (-1);
    }

#ifdef OS_HAS_PTHREAD
    pthread_mutex_lock(&fdLSLIBWriteMutex);
#endif

    header.opCode = RES_NONRES;
    header.refCode = currentSN = REQUESTSN;
    header.length = len;
    header.reserved = taskid;

    gethostbysock_(tEnt->sock, hostname);
    if (strcmp(hostname, "LSF_HOST_NULL"))
	_setcurseqno_(hostname, currentSN);

    xdrmem_create(&xdrs, headerBuf, LSF_HEADER_LEN, XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs, &header)) {
	lserrno = LSE_BAD_XDR;
	xdr_destroy(&xdrs);
	rc = -1;
        goto AbortSnd;
    }
    xdr_destroy(&xdrs);

    rc = b_write_fix(tEnt->sock, headerBuf, LSF_HEADER_LEN);
    if (rc < 0) {
	if (errno == EPIPE) {

	    close(tEnt->sock);
	    tEnt->sock = -1;
	    _lostconnection_(hostname);
	    lserrno = LSE_LOSTCON;
	}
        rc = -1;
        goto AbortSnd;
    }

    rc = b_write_fix(tEnt->sock, buffer, len);
    if (rc < 0) {
	if (errno == EPIPE) {
	    close(tEnt->sock);
	    tEnt->sock = -1;
	    _lostconnection_(hostname);
	    lserrno = LSE_LOSTCON;
	}
        rc = -1;
        goto AbortSnd;
    }
    if (ackReturnCode_(tEnt->sock) < 0) {
        rc = -1;
        goto AbortSnd;
    }

  AbortSnd:

#ifdef OS_HAS_PTHREAD
    pthread_mutex_unlock(&fdLSLIBWriteMutex);
#endif

    return(rc);

}

int
lsMsgWait_(int inTidCnt, int *tidArray, int *rdyTidCnt,
	   int inFdCnt, int *fdArray, int *rdyFdCnt, int *outFdArray,
	   struct timeval *timeout, int options)
{
    int i;
    fd_set rm;
    struct tid *taskEnt;
    int maxfd;
    int nready;
    char hdrBuf[sizeof(struct LSFHeader)];
    struct LSFHeader msgHdr;
    int rc;
    int rdycnt;
    XDR xdrs;
    bool_t tMsgQNonEmpty;
    int nBitsSet;
    bool_t anythingRdy;

    if ((! rdyTidCnt && ! rdyFdCnt) ||
	(! tidArray && !fdArray) ||
	(! inTidCnt && ! inFdCnt))
        return (0);

    for (i = 0; i < inFdCnt; i++)
        outFdArray[i] = -1;

  Again:
    tMsgQNonEmpty = FALSE;

    FD_ZERO(&rm);
    nBitsSet = 0;
    if (rdyTidCnt) *rdyTidCnt = 0;
    if (rdyFdCnt) *rdyFdCnt = 0;

    if (inFdCnt > 0 && fdArray)
        for (i = 0; i < inFdCnt; i++) {
	    if (FD_NOT_VALID(fdArray[i])) {
		lserrno = LSE_BAD_ARGS;
		rc = -1;
		goto Fail;
	    }
	    FD_SET(fdArray[i], &rm);
	    nBitsSet++;
	}

    rdycnt = 0;
    if (inTidCnt && tidArray) {
        for (i = 0; i < inTidCnt; i++) {
	    rc = lsMsgRdy_(tidArray[i], NULL);
	    if (rc > 0) {

		tMsgQNonEmpty = TRUE;
		rdycnt++;
		continue;
	    }

	    taskEnt = tid_find(tidArray[i]);

	    if (taskEnt == NULL) {
		rc = -1;
		goto Fail;
	    }

	    if (FD_NOT_VALID(taskEnt->sock)) {
		lserrno = LSE_BAD_ARGS;
		rc = -1;
		goto Fail;
	    }

	    nBitsSet++;
	    FD_SET(taskEnt->sock, &rm);
	}
	if (tMsgQNonEmpty) {
	    *rdyTidCnt = rdycnt;
	    return(0);
	}
    }

    if (nBitsSet == 0)
	return (0);

    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd > 1024) maxfd = 1024-1;
    nready = select(maxfd, &rm, NULL, NULL, timeout);

    if (nready < 0)
    {
	if (errno == EINTR) {
	    goto Again;
	} else {
	   lserrno = LSE_SELECT_SYS;
	   rc = -1;
	   goto Fail;
       }
    }

    if (rdyFdCnt) {
	rdycnt = 0;
	for (i = 0; i < inFdCnt; i++) {
	    if (FD_ISSET(fdArray[i], &rm)) {
	        rdycnt++;
		outFdArray[i] = fdArray[i];
            }
            else
		outFdArray[i] = -1;
	}
        *rdyFdCnt = rdycnt;
    }

    if (nready == 0) {
	if (rdyTidCnt)
	    *rdyTidCnt = 0;
	return (0);
    }

    if (rdyTidCnt) {
	rdycnt = 0;
	xdrmem_create(&xdrs, hdrBuf, sizeof(struct LSFHeader), XDR_DECODE);
	for (i = 0; i < inTidCnt; i++) {
	    taskEnt = tidFindIgnoreConn_(tidArray[i]);
	    if (taskEnt == NULL) {
	        rc = -1;
		xdr_destroy(&xdrs);
		goto Fail;
	    }

	    if (FD_NOT_VALID(taskEnt->sock))
		continue;

	    if (! FD_ISSET(taskEnt->sock, &rm))
	        continue;

	    xdr_setpos(&xdrs, 0);
	    rc = readDecodeHdr_(taskEnt->sock, hdrBuf, b_read_fix, &xdrs, &msgHdr);
	    if (rc < 0) {
		int nTids;
		int *tidSameConns;
		int tidIDx;

		rc = tidSameConnection_(taskEnt->sock, &nTids, &tidSameConns);
		for (tidIDx = 0; tidIDx < nTids; tidIDx++) {
		    rc = enqueueTaskMsg_(-1, tidSameConns[tidIDx], NULL);
		    if (rc < 0) {
			free(tidSameConns);
			xdr_destroy(&xdrs);
		        goto Fail;
		    }
		}
		rdycnt += nTids;
		free(tidSameConns);
		_lostconnection_(taskEnt->host);
	    } else if (msgHdr.opCode != RES_NONRES) {
		rc = ackAsyncReturnCode_(taskEnt->sock, &msgHdr);
		if (rc < 0) {
		    xdr_destroy(&xdrs);
		    goto Fail;
		}
	    } else {
		rc = enqueueTaskMsg_(taskEnt->sock, msgHdr.refCode, &msgHdr);
		if (rc < 0) {
		    xdr_destroy(&xdrs);
		    goto Fail;
		}

		FD_CLR(taskEnt->sock, &rm);
		rdycnt++;
	    }
	}
	xdr_destroy(&xdrs);
	*rdyTidCnt = rdycnt;
    }

    anythingRdy = FALSE;
    if (rdyTidCnt)
        if (*rdyTidCnt > 0)
	    anythingRdy = TRUE;

    if (rdyFdCnt)
        if (*rdyFdCnt > 0)
	    anythingRdy = TRUE;


    if (! anythingRdy)
        goto Again;

    rc = 0;

  Fail:
    return (rc);

}

int
lsReqCmp_(char *val, char *reqEnt, int hint)
{
    int                seqno;
    struct lsRequest   *req;

    req = (struct lsRequest *)reqEnt;
    seqno = *val;

    if (seqno == req->seqno)
        return (0);
    else if (seqno > req->seqno)
        return (-1);
    else
        return(1);
}

int
lsReqTest_(LS_REQUEST_T *request)
{
    if (! request)
        return(FALSE);

    if (! request->completed)
        return(FALSE);
    else
        return(TRUE);

}

int
lsReqWait_(LS_REQUEST_T *request, int options)
{
    int rc;
    struct LSFHeader header;

    if (! request)
        return(-1);

    if (lsReqTest_(request))
        return(0);

    rc = expectReturnCode_(request->connfd, request->seqno, &header);
    if (rc < 0)
        return(rc);

    rc = ackAsyncReturnCode_(request->connfd, &header);
    return(rc);

}


void
lsReqFree_(LS_REQUEST_T *request)
{
    if (request)
        free((void *)request);
}

void
_lostconnection_(char *hostName)
{
    int connSockNum;

    connSockNum = getConnectionNum_(hostName);
    if (connSockNum < 0)
        return;

    tid_lostconnection(connSockNum);
    FD_CLR(connSockNum, &connection_ok_);

}

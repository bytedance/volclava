/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: res.init.c 397 2007-11-26 19:04:00Z mblack $
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

#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>


#include "res.h"
#include "../lib/lproto.h"
#include "../lib/mls.h"

#define RES_TIMEOUT_DEFAULT 	60

#define NL_SETN		29

#include <malloc.h>
#include <errno.h>


ttyStruct defaultTty;

static void init_AcceptSock(void);
int initResVcl(void);



static void
initConn2NIOS(void)
{
    static char fname[] = "initConn2NIOS";


    conn2NIOS.task_duped = (int *) calloc(sysconf(_SC_OPEN_MAX), sizeof(int));
    if (!conn2NIOS.task_duped) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        resExit_(-1);
    }
    conn2NIOS.sock.rbuf = (RelayBuf *) malloc(sizeof(RelayBuf));
    if (!conn2NIOS.sock.rbuf) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        resExit_(-1);
    }
    conn2NIOS.sock.wbuf = (RelayLineBuf *) malloc(sizeof(RelayLineBuf));
    if (!conn2NIOS.sock.wbuf) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        resExit_(-1);
    }
    conn2NIOS.sock.fd = -1;
    conn2NIOS.wtag = conn2NIOS.rtag = -1;
    conn2NIOS.num_duped = 0;
    conn2NIOS.sock.rcount = conn2NIOS.sock.wcount = 0;
    conn2NIOS.sock.rbuf->bcount = conn2NIOS.sock.wbuf->bcount = 0;
}


void
init_res(void)
{
    static char fname[] = "init_res";
    int i, maxfds;

    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);



    if (!sbdMode) {
	if (! debug) {

	    if (geteuid() || getuid()) {
		fprintf(stderr, "RES should be run as root.\n");
		fflush(stderr);
		resExit_(1);
	    }



	    chdir("/tmp");
	}



	if (debug <= 1) {

	    daemonize_();



	    ls_openlog("res", resParams[LSF_LOGDIR].paramValue, 0,
		       resParams[LSF_LOG_MASK].paramValue);



	    umask(0);





	    nice(NICE_LEAST);
	}
    }

    if ((Myhost = ls_getmyhostname()) == NULL ) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_getmyhostname");
	resExit_(-1);
    }


    if (isatty(0)) {
        tcgetattr(0, &defaultTty.attr);

        defaultTty.ws.ws_row = 24;
	defaultTty.ws.ws_col = 80;
        defaultTty.ws.ws_xpixel = defaultTty.ws.ws_ypixel = 0;
    } else {
        defaultTty.ws.ws_row = 24;
	defaultTty.ws.ws_col = 80;
        defaultTty.ws.ws_xpixel = defaultTty.ws.ws_ypixel = 0;
    }

    if (!sbdMode) {

	init_AcceptSock();
    }

    client_cnt = child_cnt = 0;

    for (i = 0; i < MAXCLIENTS_HIGHWATER_MARK+1; i++) {
        clients[i] = NULL;
    }

    children = (struct child **) calloc(sysconf(_SC_OPEN_MAX), sizeof(struct children *));
    if (!children) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        resExit_(-1);
    }

    maxfds = sysconf(_SC_OPEN_MAX);

    for (i = 0; i < maxfds; i++) {
        children[i] = NULL;
    }

    initConn2NIOS();
    resNotifyList = listCreate("resNotifyList");
    if (!resNotifyList) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "listCreate");
        resExit_(-1);
    }




    ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5346, "Daemon started")));   /* catgets 5346 */

    initResLog();

}

/* init_AcceptSock()
 */
static void
init_AcceptSock(void)
{
    static char          fname[] ="init_AcceptSock()";
    struct servent       *sv;
    struct sockaddr_in   svaddr;
    socklen_t            len;
    int                  one = 1;
    struct hostent       *hp;

    memset((char*)&svaddr, 0, sizeof(svaddr));
    if ((accept_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "socket", "RES");
        resExit_(1);
    }

    setsockopt(accept_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
               sizeof (int));

    if (io_nonblock_(accept_sock) < 0)
	ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "io_nonblock_",
                  accept_sock);

    fcntl(accept_sock, F_SETFD, (fcntl(accept_sock, F_GETFD) | FD_CLOEXEC));


    if (resParams[LSF_RES_PORT].paramValue) {
	if ((svaddr.sin_port = atoi(resParams[LSF_RES_PORT].paramValue)) == 0)
	{
	    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5307,
                                             "%s: LSF_RES_PORT in lsf.conf (%s) must be positive integer; exiting"), fname, resParams[LSF_RES_PORT].paramValue); /* catgets 5307 */
	    resExit_(1);
	}
	svaddr.sin_port = htons(svaddr.sin_port);
    } else if (debug) {
        svaddr.sin_port = htons(RES_PORT);
    } else {
        if ((sv = getservbyname("res", "tcp")) == NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5309,
                                             "%s: res/tcp: unknown service, exiting"), fname); /* catgets 5309 */
            resExit_(1);
        }
        svaddr.sin_port   = sv->s_port;
    }

    svaddr.sin_family = AF_INET;
    svaddr.sin_addr.s_addr = INADDR_ANY;
    if (Bind_(accept_sock, (struct sockaddr *)&svaddr, sizeof(svaddr)) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "accept_sock",
                  ntohs(svaddr.sin_port));
        resExit_(1);
    }

    if (listen(accept_sock, 1024) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "listen");
        resExit_(1);
    }

    if ((ctrlSock = TcpCreate_(TRUE, 0)) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "TcpCreate_");
        resExit_(1);
    }

    len = sizeof(ctrlAddr);
    memset((char *) &ctrlAddr, 0, sizeof(ctrlAddr));
    if (getsockname(ctrlSock, (struct sockaddr *) &ctrlAddr, &len) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "getsockname",
                  ctrlSock);
	resExit_(-1);
    }

    if ((hp = Gethostbyname_(Myhost)))
	memcpy((char *) &ctrlAddr.sin_addr,
               (char *)hp->h_addr,
	       (int)hp->h_length);
}

#define LOOP_ADDR       0x7F000001

static void
initChildRes(char *envdir)
{
    static char fname[]="initChildRes";
    int i, maxfds;

    getLogClass_(resParams[LSF_DEBUG_RES].paramValue,
                 resParams[LSF_TIME_RES].paramValue);

    openChildLog("res", resParams[LSF_LOGDIR].paramValue,
                 (debug > 1), &(resParams[LSF_LOG_MASK].paramValue));

    if ((Myhost = ls_getmyhostname()) == NULL ) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_getmyhostname");
        resExit_(-1);
    }
    client_cnt = child_cnt = 0;

    for (i = 0; i < MAXCLIENTS_HIGHWATER_MARK+1; i++) {
        clients[i] = NULL;
    }
    children = calloc(sysconf(_SC_OPEN_MAX), sizeof(struct children *));
    if (!children) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        resExit_(-1);
    }
    maxfds = sysconf(_SC_OPEN_MAX);

    for (i = 0; i < maxfds; i++) {
        children[i] = NULL;
    }

    initConn2NIOS();
    resNotifyList = listCreate("resNotifyList");
    if (!resNotifyList) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "listCreate");
        resExit_(-1);
    }
}


int
resParent(int s, struct passwd *pw, struct lsfAuth *auth,
          struct resConnect *connReq, struct hostent *hostp)
{
    static char fname[]="resParent";
    int cc;
    char *argv[7];
    char hndlbuf[64];
    struct resChildInfo childInfo;
    char buf[2*MSGSIZE];
    XDR xdrs;
    int len;
    struct LSFHeader hdr;
    int hpipe[2];
    int wrapPipe[2];
    int pid;

    if (resParams[LSF_SERVERDIR].paramValue != NULL) {
        argv[0] = getDaemonPath_("/res", resParams[LSF_SERVERDIR].paramValue);
    } else {
        argv[0] = "res";
    }

    childInfo.resConnect = connReq;
    childInfo.lsfAuth    = auth;
    childInfo.pw         = pw;
    childInfo.host       = hostp;
    childInfo.parentPort = ctrlAddr.sin_port;
    childInfo.currentRESSN = currentRESSN;


    if(resLogOn == 1) {
        char strLogCpuTime[32];

        sprintf(strLogCpuTime, "%d", resLogcpuTime);
	putEnv("LSF_RES_LOGON", "1");
	putEnv("LSF_RES_CPUTIME", strLogCpuTime );
	putEnv("LSF_RES_ACCTPATH", resAcctFN);
    }else if( resLogOn == 0) {
	putEnv("LSF_RES_LOGON", "0");
    }else if( resLogOn == -1){
	putEnv("LSF_RES_LOGON", "-1");
    }

    xdrmem_create(&xdrs, buf, 2*MSGSIZE, XDR_ENCODE);
    memset((void *)&hdr, 0, sizeof(struct LSFHeader));
    hdr.version = VOLCLAVA_VERSION;
    if (!xdr_resChildInfo(&xdrs, &childInfo, &hdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resChildInfo");
        return(-1);
    }
    len = XDR_GETPOS(&xdrs);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, hpipe) < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socketpair");
	return(-1);
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, wrapPipe) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socketpair");
        return(-1);
    }
    sprintf(hndlbuf,"%d:%d", hpipe[1], s);

    cc=1;
    argv[cc++] = "-d";
    argv[cc++] = env_dir;
    if (debug) {
        if (debug == 1)
            argv[cc++] = "-1";
        else
            argv[cc++] = "-2";
        argv[cc++] = "-s";
        argv[cc++] = hndlbuf;
        argv[cc++] = NULL;
    } else {
        argv[cc++] = "-s";
        argv[cc++] = hndlbuf;
        argv[cc++] = NULL;
    }


    if (getenv("LSF_SETDCEPAG") == NULL)
	putEnv("LSF_SETDCEPAG", "Y");

    pid = fork();
    if (pid < 0) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
	close(hpipe[0]);
	close(hpipe[1]);
        close(wrapPipe[0]);
        close(wrapPipe[1]);
	return(-1);
    }

    if (pid == 0) {
        if (logclass & LC_TRACE) {
            if (debug) {
	        ls_syslog(LOG_DEBUG2, "%s: executing %s %s %s %s %s %s ",
                          fname, argv[0], argv[1], argv[2], argv[3], argv[4],
                          argv[5]);
            } else {
	        ls_syslog(LOG_DEBUG2, "%s: executing %s %s %s %s %s ",
                          fname, argv[0], argv[1], argv[2], argv[3], argv[4]);
            }
        }
	close (hpipe[0]);
        close (wrapPipe[0]);


        if (dup2(wrapPipe[1], 0) == -1) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "dup2");
            exit (-1);
        }
	close(wrapPipe[1]);
	lsfExecv(argv[0], argv);
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "execv");
	exit(-1);
    }

    close(hpipe[1]);
    close(wrapPipe[1]);

    if (connReq->eexec.len > 0) {
        int cc1;
        if ((cc1 = b_write_fix(wrapPipe[0], connReq->eexec.data,
                               connReq->eexec.len)) != connReq->eexec.len) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5333,
                                             "%s: Falied in sending data to wrap for user <%s>, length = %d, cc=1%d: %m"), fname, pw->pw_name,  connReq->eexec.len, cc1) /* catgets 5333 */
                ;
            close(wrapPipe[0]);
            close(hpipe[0]);
            return (-1);
        }
    }
    close (wrapPipe[0]);

    if (write(hpipe[0], (char *)&len, sizeof(len)) != sizeof(len)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "write");
        xdr_destroy(&xdrs);
        close(hpipe[0]);
        return(-1);
    }

    if (write(hpipe[0], buf, len) != len) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "write");
        xdr_destroy(&xdrs);
        close(hpipe[0]);
        return(-1);
    }
    xdr_destroy(&xdrs);
    close(hpipe[0]);
    return(0);
}


void
resChild( char *arg, char *envdir)
{
    static char fname[]="resChild";
    struct passwd pw;
    struct hostent hp;
    struct resConnect connReq;
    struct lsfAuth auth;
    struct resChildInfo childInfo;
    XDR xdrs;
    int clientHandle, resHandle;
    char *sp, *buf;
    struct LSFHeader  hdr;
    int len;
    char *nullist[1];

    initChildRes(envdir);


    sp = strchr(arg,':');
    sp[0]='\0';
    sp++;
    resHandle= atoi(arg);
    clientHandle = atoi(sp);




    if (b_read_fix(resHandle, (char *) &len, sizeof(len)) != sizeof(len)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "b_read_fix");
        resExit_(-1);
    }
    buf = malloc(len);
    if (!buf) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        resExit_(-1);
    }
    if (b_read_fix(resHandle, buf, len) != len) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "b_read_fix");
        resExit_(-1);
    }

    CLOSE_IT(resHandle);

    childInfo.pw     = &pw;
    childInfo.host   = &hp;
    childInfo.resConnect = &connReq;
    childInfo.lsfAuth = &auth;

    xdrmem_create(&xdrs, buf, len, XDR_DECODE);
    hdr.version = VOLCLAVA_VERSION;
    if (!xdr_resChildInfo(&xdrs, &childInfo, &hdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "xdr_resChildInfo");
        resExit_(-1);
    }

    ctrlAddr.sin_addr.s_addr = htonl(LOOP_ADDR);
    ctrlAddr.sin_family = AF_INET;
    ctrlAddr.sin_port = childInfo.parentPort;
    currentRESSN = childInfo.currentRESSN;



    if( getenv("LSF_RES_LOGON") ) {
	if( strcmp(getenv("LSF_RES_LOGON"), "1") == 0 ){
            resLogOn = 1;
            if( getenv("LSF_RES_CPUTIME") ) {
                resLogcpuTime = atoi( getenv("LSF_RES_CPUTIME") );
            }
	    if( getenv("LSF_RES_ACCTPATH") ){
	        strcpy( resAcctFN, getenv("LSF_RES_ACCTPATH") );
	    }
        } else if( strcmp(getenv("LSF_RES_LOGON"), "0") ==0 ){
	    resLogOn = 0;
        } else if( strcmp(getenv("LSF_RES_LOGON"), "-1") ==0 ){
            resLogOn = -1;
	}
    }

    nullist[0]=NULL;
    hp.h_aliases = nullist;

    childAcceptConn(clientHandle, &pw, &auth, &connReq, &hp);


    free(buf);
}

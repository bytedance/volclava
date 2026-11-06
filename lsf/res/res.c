/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
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
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/signal.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>

#include <sys/time.h>

#include "res.h"
#include "../lib/lproto.h"

#define NL_SETN		29

extern void child_channel_clear(struct child *, outputChannel *);
extern char **environ;

int	rexecPriority = 0;

struct client  *clients[MAXCLIENTS_HIGHWATER_MARK+1];
int client_cnt;
struct child   **children;
int child_cnt;
char *Myhost = NULL;
char *myHostType = NULL;

taggedConn_t conn2NIOS;
LIST_T *resNotifyList;

bool_t vclPlugin = FALSE;

char child_res = 0;
char child_go = 0;
char res_interrupted = 0;
char *gobuf="go";

int  accept_sock = INVALID_FD;
char allow_accept = 1;

int ctrlSock = INVALID_FD;
struct sockaddr_in ctrlAddr;

int child_res_port  = INVALID_FD;
int parent_res_port = INVALID_FD;
fd_set readmask, writemask, exceptmask;

int sigchldPipeRd = -1;
int sigchldPipeWr = -1;

int on  = 1;
int off = 0;
int debug = 0;
int res_status = 0;

char *lsbJobStarter = NULL;

int sbdMode = FALSE;
int sbdFlags = 0;

int lastChildExitStatus = 0;

char res_logfile[MAXPATHLEN];
int  res_logop;

int restart_argc;
char **restart_argv;


char *env_dir;

struct config_param resParams[] = {
    {"LSF_RES_DEBUG", NULL},
    {"LSF_SERVERDIR", NULL},
    {"LSF_AUTH", NULL},
    {"LSF_LOGDIR", NULL},
    {"LSF_ROOT_REX", NULL},
    {"LSF_LIM_PORT", NULL},
    {"LSF_RES_PORT", NULL},
    {"LSF_ID_PORT", NULL},
    {"LSF_USE_HOSTEQUIV", NULL},
    {"LSF_RES_ACCTDIR", NULL},
    {"LSF_RES_ACCT", NULL},
    {"LSF_DEBUG_RES", NULL},
    {"LSF_TIME_RES", NULL},
    {"LSF_LOG_MASK", NULL},
    {"LSF_RES_RLIMIT_UNLIM", NULL},
    {"LSF_CMD_SHELL", NULL},
    {"LSF_ENABLE_PTY", NULL},
    {"LSF_TMPDIR", NULL},
    {"LSF_BINDIR", NULL},
    {"LSF_LIBDIR", NULL},
    {"LSF_RES_TIMEOUT", NULL},
    {"LSF_RES_NO_LINEBUF", NULL},
    {"LSF_MLS_LOG", NULL},
    {NULL, NULL}
};

struct config_param resConfParams[] = {
    {"LSB_UTMP", NULL},
    {NULL, NULL}
};

#if __STDC__
#define P_(s) s
#else
#define P_(s) ()
#endif

static void put_mask P_((char *, fd_set *));
static void periodic P_((int));
static void usage P_((char *));
static void initSignals(void);
static void houseKeeping(void);
static void blockSignals(void);
static void unblockSignals(void);
static int initSigchldPipe(void);

#undef P_

static void
usage (char *cmd)
{
    fprintf(stderr, I18N_Usage);
    fprintf(stderr, ": %s  [-V] [-h] [-debug_level] [-d env_dir] [[-p cl_port [-P] [-i] [-o] [-e]] -m cl_host command [args]]\n", cmd);
    exit(-1);
}




int
main(int argc, char **argv)
{
    static char fname[] = "res/main";
    int nready;
    int maxfd;
    int i;
    char *sp;
    char *pathname = NULL;
    int didSomething = 0;
    char exbuf;

    time_t thisPeriodic, lastPeriodic = 0, timeDiff;


    fd_set rm, wm, em;

    int sbdPty = FALSE;
    char *sbdClHost = NULL;
    ushort sbdClPort = 0;
    char **sbdArgv = NULL;
    int selectError = 0;


    _i18n_init(I18N_CAT_RES);

    saveDaemonDir_(argv[0]);



    for (i=1; i<argc; i++) {
      if (strcmp(argv[i], "-d") == 0 && argv[i+1] != NULL) {
         pathname = argv[i+1];
         putEnv("LSF_ENVDIR",pathname);
         break;
      }
    }

    if (pathname == NULL) {
	if ((pathname = getenv("LSF_ENVDIR")) == NULL)
	    pathname = LSETCDIR;
    }


    if (argc > 1) {
        if (!strcmp(argv[1],"-V")) {
            fputs(_LS_VERSION_, stdout);
            exit(0);
        }
    }



    if ((ls_readconfenv(resConfParams, NULL) < 0) ||
        (initenv_(resParams, pathname) < 0) ) {
        if ((sp = getenv("LSF_LOGDIR")) != NULL)
            resParams[LSF_LOGDIR].paramValue = sp;
        ls_openlog("res", resParams[LSF_LOGDIR].paramValue, (debug > 1),
                   resParams[LSF_LOG_MASK].paramValue);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_MM, fname, "initenv_",
            pathname);
        ls_syslog(LOG_ERR, I18N_Exiting);
        exit(-1);
    }



    restart_argc = argc;
    restart_argv = argv;
    for (i=1; i<argc; i++) {
	if (strcmp(argv[i], "-d") == 0 && argv[i+1] != NULL) {
	    pathname = argv[i+1];
	    i++;
	    continue;
	}

	if (strcmp(argv[i], "-1") == 0) {
	    debug = 1;
	    continue;
	}

	if (strcmp(argv[i], "-2") == 0) {
	    debug = 2;
	    continue;
	}

	if ( (strcmp(argv[i], "-j") == 0) && (argv[i+1] != NULL) ) {
	    lsbJobStarter = argv[++i];
	    continue;
	}

	if (strcmp(argv[i], "-P") == 0) {
	    sbdPty = TRUE;
	    continue;
	}

	if (strcmp(argv[i], "-i") == 0) {
	    sbdFlags |= SBD_FLAG_STDIN;
	    continue;
	}

	if (strcmp(argv[i], "-o") == 0) {
	    sbdFlags |= SBD_FLAG_STDOUT;
	    continue;
	}

	if (strcmp(argv[i], "-e") == 0) {
	    sbdFlags |= SBD_FLAG_STDERR;
	    continue;
	}

	if (strcmp(argv[i], "-m") == 0 && argv[i+1] != NULL) {
	    sbdClHost = argv[i+1];
	    i++;
	    sbdMode = TRUE;
	    continue;
	}

	if (strcmp(argv[i], "-p") == 0 && argv[i+1] != NULL) {
	    sbdClPort = atoi(argv[i+1]);
	    i++;
	    sbdMode = TRUE;
	    continue;
	}


	if (argv[i][0] != '-') {
	    sbdMode = TRUE;
	    sbdArgv = argv + i;
	    break;
	}

        usage(argv[0]);
    }

    if (sbdMode) {

	if (sbdClHost == NULL || sbdArgv == NULL) {
	    usage(argv[0]);
	    exit(-1);
	}
	if (sbdClPort) {
	    sbdFlags |= SBD_FLAG_TERM;
	} else {

	    sbdFlags |= SBD_FLAG_STDIN | SBD_FLAG_STDOUT | SBD_FLAG_STDERR;
	}
    } else {


	if (debug < 2)
	    for (i = sysconf(_SC_OPEN_MAX) ; i >= 0 ; i--)
		close(i);
    }


    if (resParams[LSF_SERVERDIR].paramValue == NULL) {
	ls_openlog("res", resParams[LSF_LOGDIR].paramValue, (debug > 1),
				   resParams[LSF_LOG_MASK].paramValue);
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5001,
	    "LSF_SERVERDIR not defined in %s/lsf.conf: %M; res exiting"), /* catgets 5001 */
	    pathname);
	resExit_(-1);
    }


    if (! debug && resParams[LSF_RES_DEBUG].paramValue != NULL) {
	debug = atoi(resParams[LSF_RES_DEBUG].paramValue);
	if (debug <= 0)
	    debug = 1;
    }


    getLogClass_(resParams[LSF_DEBUG_RES].paramValue,
                 resParams[LSF_TIME_RES].paramValue);


    if (getuid() == 0 && debug) {
        if (sbdMode)  {
	   debug = 0;
	} else {
          ls_openlog("res", resParams[LSF_LOGDIR].paramValue, FALSE,
		   resParams[LSF_LOG_MASK].paramValue);
          ls_syslog(LOG_ERR, I18N(5005,"Root cannot run RES in debug mode ... exiting."));/*catgets 5005 */
	   exit(-1);
	}
    }

    if (debug > 1)
	ls_openlog("res", resParams[LSF_LOGDIR].paramValue, TRUE, "LOG_DEBUG");
    else {
 	ls_openlog("res", resParams[LSF_LOGDIR].paramValue, FALSE,
		   resParams[LSF_LOG_MASK].paramValue);
    }
    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG, "%s: logclass=%x", fname, logclass);

    ls_syslog(LOG_DEBUG, "%s: LSF_SERVERDIR=%s", fname, resParams[LSF_SERVERDIR].paramValue);



    init_res();
    initSignals();

    periodic(0);



    if (sbdMode) {
	lsbJobStart(sbdArgv, sbdClPort, sbdClHost, sbdPty);
    }

    initSigchldPipe();

    maxfd = FD_SETSIZE;


    for (;;) {
        struct timeval *timep;
        struct timeval timeout;
loop:
        didSomething = 0;

        for (i = 0; i < child_cnt; i++) {
            if (children[i]->backClnPtr == NULL
                 && !FD_IS_VALID(conn2NIOS.sock.fd)
                 && children[i]->running == 0) {
                delete_child (children[i]);
            }
        }

	if (logclass & LC_TRACE) {
	    ls_syslog(LOG_DEBUG,"\
%s: %s Res child_res=<%d> child_go=<%d> child_cnt=<%d> client_cnt=<%d>",
		      fname, ((child_res) ? "Application" : "Root") ,
		      child_res, child_go, child_cnt, client_cnt);
            if (child_cnt == 1 && children != NULL && children[0] != NULL) {
                dumpChild(children[0], 1, "in main()");
            }
	}

        if (child_res && child_go && child_cnt == 0 && client_cnt == 0)  {


            if (debug > 1)
		printf (" \n Child <%d> Retired! \n", (int)getpid());

	    if (logclass & LC_TRACE) {
		ls_syslog(LOG_DEBUG,"\
%s: Application Res is exiting.....", fname);
	    }


	    millisleep_(5000);


	    if (sbdMode) {

		close(1);
		close(2);
		exit(lastChildExitStatus);
            }
	    resExit_(EXIT_NO_ERROR);
        }

	houseKeeping();
        getMaskReady(&readmask, &writemask, &exceptmask);
	if (debug > 1) {
	    printf("Masks Set: ");
	    display_masks(&readmask, &writemask, &exceptmask);
	    fflush(stdout);
        }


	unblockSignals();



	FD_ZERO(&rm);
	FD_ZERO(&wm);
	FD_ZERO(&em);
	memcpy(&rm, &readmask, sizeof(fd_set));
	memcpy(&wm, &writemask, sizeof(fd_set));
	memcpy(&em, &exceptmask, sizeof(fd_set));


	if (!child_res) {
	    time(&thisPeriodic);
	    timeDiff = lastPeriodic + 90 - thisPeriodic;
	    if (timeDiff <= 0) {
		periodic(0);
		time(&lastPeriodic);
		timeDiff = 90;
	    }
	    timep = &timeout;
	    timep->tv_sec = timeDiff;
	    timep->tv_usec = 0;
	} else {


	    timep = &timeout;
            timep->tv_sec = 20;
            timep->tv_usec = 0;
	}

	if (debug > 1) {
	    printf("\n+++++++++++++++++++++++++++++++++\nselecting ...");
	    if (!child_res) {
		printf(" timeval.tv_sec = <%d> timeval.tv_usec = <%d>\n", (int)timep->tv_sec, (int)timep->tv_usec);
	    } else {
		printf("\n");
	    }
	    fflush(stdout);
	}

	if (res_interrupted > 0) {
	    blockSignals();
	    res_interrupted = 0;
	    processReapedChildren();
	    continue;
	} else {
            nready = select(maxfd, &readmask, &writemask, &exceptmask,
                        (struct timeval *)timep);
            selectError = errno;
	}


	blockSignals();

	if (debug > 1)
	    printf("selected nready=%d\n",nready);
        if (nready <= 0) {
            if (nready == 0 && !child_res) {
                periodic(0);
		time(&lastPeriodic);
            } else if (nready < 0) {
		errno = selectError;

		if (selectError == EBADF) {
		    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MN, fname, "select");
		    if (child_res) {
			resExit_(-1);
		    }
		} else if (selectError != EINTR) {
		    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MN, fname, "select");
		}
	    }
            continue;
        }

        if (FD_IS_VALID(sigchldPipeRd)
            && FD_ISSET(sigchldPipeRd, &readmask)) {
            processReapedChildren();
            goto loop;
        }

	if (debug > 1) {
	    printf("Masks Get:  ");
	    display_masks(&readmask, &writemask, &exceptmask);
        }

        if (FD_IS_VALID(parent_res_port)
           && FD_ISSET(parent_res_port, &readmask))
        {

            if (! allow_accept){
		for (;;)
                    if (write(child_res_port,gobuf,strlen(gobuf)) > 0)
			break;
                for (;;)
                    if (read(child_res_port, &exbuf, 1) >= 0)
			break;
            }
	    close(parent_res_port);
            parent_res_port = INVALID_FD;
            child_go = 1;
            allow_accept = 0;
            closesocket(child_res_port);
	    closesocket(accept_sock);
	    accept_sock = INVALID_FD;
        }

        if (FD_IS_VALID(conn2NIOS.sock.fd)
               && FD_ISSET(conn2NIOS.sock.fd, &readmask)) {
            donios_sock(children, DOREAD);
            goto loop;
        }
        if (FD_IS_VALID(conn2NIOS.sock.fd)
               && FD_ISSET(conn2NIOS.sock.fd, &writemask)) {
            donios_sock(children, DOWRITE);
            goto loop;
        }

        for (i = 0; i < child_cnt; i++) {
            if (  FD_IS_VALID(children[i]->info)
               && FD_ISSET(children[i]->info, &readmask))
            {
		if (logclass & LC_TRACE) {
		    dumpChild(children[i], DOREAD, "child info in readmask");
		}
                dochild_info(children[i], DOREAD);
                goto loop;
            }
        }



        for (i = 0; i < client_cnt; i++) {
            if (  FD_IS_VALID(clients[i]->client_sock)
               && FD_ISSET(clients[i]->client_sock, &readmask))
            {
		if (logclass & LC_TRACE) {
		    dumpClient(clients[i], "client_sock in readmask");
		}
                doclient(clients[i]);
                goto loop;
            }
        }

        for (i = 0; i < child_cnt; i++)  {
            if (  FD_IS_VALID(children[i]->std_out.fd)
               && FD_ISSET(children[i]->std_out.fd, &readmask))
            {
		if (logclass & LC_TRACE) {
		    dumpChild(children[i], DOREAD,
			      "child std_out.fd in readmask");
		}
                dochild_stdio(children[i], DOREAD);
                didSomething = 1;
            }
	    if (  FD_IS_VALID(children[i]->std_err.fd)
               && FD_ISSET(children[i]->std_err.fd, &readmask))
            {
		if (logclass & LC_TRACE) {
		    dumpChild(children[i], DOSTDERR,
			      "child std_err.fd in readmask");
		}
                dochild_stdio(children[i], DOSTDERR);
                didSomething = 1;
            }
        }
	if (didSomething)
	    goto loop;

        for (i = 0; i < child_cnt; i++) {
            if (  FD_IS_VALID(children[i]->stdio)
               && FD_ISSET(children[i]->stdio, &writemask))
            {
		if (logclass & LC_TRACE) {
		    dumpChild(children[i], DOWRITE,
			      "child stdin in writemask");
		}
                dochild_stdio(children[i], DOWRITE);
                didSomething = 1;
            }
        }
	if (didSomething)
	    goto loop;

        if (FD_IS_VALID(accept_sock) &&
	    FD_ISSET(accept_sock, &readmask)) {
            doacceptconn();
        }

        if (FD_IS_VALID(ctrlSock) &&
	    FD_ISSET(ctrlSock, &readmask)) {
            doResParentCtrl();
        }


    }



}

static int
initSigchldPipe(void)
{
    int fds[2];
    int flags;
    int fd;

    if (pipe(fds) < 0) {
        ls_syslog(LOG_ERR, "%s: pipe failed: %m", "initSigchldPipe");
        return -1;
    }

    for (fd = 0; fd < 2; fd++) {
        flags = fcntl(fds[fd], F_GETFL, 0);
        if (flags >= 0)
            fcntl(fds[fd], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(fds[fd], F_GETFD, 0);
        if (flags >= 0)
            fcntl(fds[fd], F_SETFD, flags | FD_CLOEXEC);
    }

    sigchldPipeRd = fds[0];
    sigchldPipeWr = fds[1];
    return 0;
}

static void
initSignals()
{
    Signal_(SIGCHLD, (SIGFUNCTYPE) child_handler);
    Signal_(SIGINT,  (SIGFUNCTYPE) term_handler);
    Signal_(SIGHUP, (SIGFUNCTYPE) sigHandler);
    Signal_(SIGPIPE, (SIGFUNCTYPE) sigHandler);
    Signal_(SIGTTIN, (SIGFUNCTYPE) sigHandler);
    Signal_(SIGTTOU, (SIGFUNCTYPE) sigHandler);
    Signal_(SIGTSTP, (SIGFUNCTYPE) sigHandler);
    Signal_(SIGCONT, (SIGFUNCTYPE) sigHandler);

#ifdef SIGDANGER
    Signal_(SIGDANGER, (SIGFUNCTYPE) sigHandler);
#endif

    Signal_(SIGTERM, (SIGFUNCTYPE) term_handler);
#ifdef SIGXCPU
    Signal_(SIGXCPU, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGXFSZ
    Signal_(SIGXFSZ, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGPROF
    Signal_(SIGPROF, (SIGFUNCTYPE) term_handler);
#endif

#ifdef SIGLOST
    Signal_(SIGLOST, (SIGFUNCTYPE) term_handler);
#endif

    Signal_(SIGUSR1, (SIGFUNCTYPE) term_handler);
    Signal_(SIGUSR2, (SIGFUNCTYPE) term_handler);
#ifdef SIGABRT
    Signal_(SIGABRT, (SIGFUNCTYPE) term_handler);
#endif



    Signal_(SIGQUIT, SIG_IGN);

}

void
getMaskReady(fd_set *rm, fd_set *wm, fd_set *em)
{
    int     i;

    FD_ZERO(rm);
    FD_ZERO(wm);
    FD_ZERO(em);

    if (FD_IS_VALID(sigchldPipeRd)) {
        FD_SET(sigchldPipeRd, rm);
    }

    if (allow_accept && FD_IS_VALID(accept_sock)) {
        FD_SET(accept_sock, rm);
    }

    if (allow_accept && FD_IS_VALID(ctrlSock)) {
	FD_SET(ctrlSock, rm);
    }

    if (child_res && !child_go && FD_IS_VALID(parent_res_port)) {
        FD_SET(parent_res_port, rm);
    }


    for (i = 0; i < client_cnt; i++) {
        if (FD_IS_VALID(clients[i]->client_sock)) {
            FD_SET(clients[i]->client_sock, rm);

	    if (debug > 2)
		fprintf(stderr, "RM: client_sock for client <%d>: %d\n",
		       i, clients[i]->client_sock);
        }
    }

    for (i = 0; i < child_cnt; i++) {
	if (debug > 2) {
	    printf("child %d:\n", i);
	    printf("rpid = %d, pid = %d, stdio fd = %d, remscok fd = %d\n",
		   children[i]->rpid, children[i]->pid, children[i]->stdio,
		   children[i]->remsock.fd);
            printf("running = %d, endstdin = %d, endstdout = %d, endstderr = %d\n",
                children[i]->running, children[i]->endstdin,
		children[i]->std_out.endFlag,
		children[i]->std_err.endFlag);
            printf(
		"stdin buf remains %d chars, stdout buf remains %d chars, stderr buf remains %d chars\n",
		children[i]->i_buf.bcount,
		children[i]->std_out.buffer.bcount,
		children[i]->std_err.buffer.bcount);
	    printf(
              "remsock %d chars input pending, %d chars output to be drained\n",
		children[i]->remsock.rcount, children[i]->remsock.wcount);
	    fflush(stdout);
        }




        if ((children[i]->rexflag & REXF_USEPTY) &&
		    FD_IS_VALID(children[i]->std_out.fd)) {
	    if (children[i]->std_out.buffer.bcount == 0)
		FD_SET(children[i]->std_out.fd, em);
        }


        if (FD_IS_VALID(children[i]->stdio)) {

	    if (children[i]->i_buf.bcount > 0 || children[i]->endstdin)
	        FD_SET(children[i]->stdio, wm);
	}


	if (FD_IS_VALID(children[i]->std_out.fd)
	    && (children[i]->std_out.buffer.bcount < LINE_BUFSIZ) ) {
	    FD_SET(children[i]->std_out.fd, rm);
	}


	if (FD_IS_VALID(children[i]->std_err.fd)
	    && (children[i]->std_err.buffer.bcount < LINE_BUFSIZ) ) {
	    FD_SET(children[i]->std_err.fd, rm);
	}

	if (FD_IS_VALID(children[i]->info)) {
	    FD_SET(children[i]->info, rm);
	}

    }


    if (FD_IS_VALID(conn2NIOS.sock.fd)) {
        if (conn2NIOS.sock.rbuf->bcount == 0)
            FD_SET(conn2NIOS.sock.fd, rm);

        if (conn2NIOS.sock.wcount != 0)
            FD_SET(conn2NIOS.sock.fd, wm);
        else if (conn2NIOS.sock.wbuf->bcount != 0)
            FD_SET(conn2NIOS.sock.fd, wm);
    }

}

void
display_masks(fd_set *rm, fd_set *wm, fd_set *em)
{
    put_mask("RM", rm);
    put_mask("WM", wm);
    put_mask("EM", em);
    fputs("\n", stdout);
}

static void
put_mask(char *name, fd_set *mask)
{
    fputs(name, stdout);
    putchar(':');
#if !defined(__CYGWIN__)
    printf("0x%8.8x ", (int) mask->__fds_bits[0]);
#endif
    fputs("  ", stdout);
}

static void
periodic(int signum)
{
    static char fname[] = "res/periodic";
    time_t now;
    static int count = 0;
    static time_t lastPri =0;

    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if (!child_res) {
	time (&now);

	if (now - lastPri > 1800) {
	    struct hostInfo *hInfo;
	    char *myhostname;

	    lastPri = now;
	    if ((myhostname = ls_getmyhostname()) == NULL ) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname,
		    "ls_getmyhostname");
		rexecPriority = 0;
	    } else {
		hInfo = ls_gethostinfo(NULL, NULL , &myhostname, 1, 0);
		if (!hInfo) {
		    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname,
			"ls_gethostinfo");
		    rexecPriority = 0;
		} else {
		    rexecPriority = hInfo->rexPriority;
		    if (myHostType == NULL)
			myHostType = putstr_(hInfo->hostType);
		}
		getLSFAdmins_();
	    }
	}

	if (!sbdMode) {
            count++;
            if (count >=5) {
                count = 0;
                ls_syslog(LOG_DEBUG, "periodic: ls_servavail called");
            }
	    if (ls_servavail(1, 1) < 0)
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_servavail");
	}
    }

}

static void
houseKeeping(void)
{
    static char fname[] = "houseKeeping";
    static int previousIndex = -1;
    int i, j, ch_cnt;

    if (debug>1)
        printf("houseKeeping\n");

    if (logclass & LC_TRACE) {
	ls_syslog(LOG_DEBUG,"\
%s: Nothing else to do but housekeeping", fname);
    }

    for (i = 0; i < child_cnt; i++) {



        if ( (children[i]->std_out.buffer.bcount == 0)
	     && FD_IS_VALID(children[i]->std_out.fd) ) {
            if (children[i]->std_out.retry) {
                if (children[i]->running == 1)
                    children[i]->std_out.retry = 0;

                if (logclass & LC_TRACE)
                    ls_syslog(LOG_DEBUG,"\
%s: Trying to flush child=<%d> stdout, std_out.retry=<%d>",
                             fname, i, children[i]->std_out.retry);
                child_channel_clear(children[i], &(children[i]->std_out));
            }
        }


        if ( (children[i]->std_err.buffer.bcount == 0)
	     && FD_IS_VALID(children[i]->std_err.fd) ) {
            if (children[i]->std_err.retry) {
                if (children[i]->running == 1)
                    children[i]->std_err.retry = 0;

                if (logclass & LC_TRACE)
                    ls_syslog(LOG_DEBUG,"\
%s: Trying to flush child=<%d> stderr, std_err.retry=<%d>",
                             fname, i, children[i]->std_err.retry);
                child_channel_clear(children[i], &(children[i]->std_err));
            }
        }
    }


    processReapedChildren();


    if (FD_IS_VALID(conn2NIOS.sock.fd)) {
        if (conn2NIOS.sock.wcount == 0)
            deliver_notifications(resNotifyList);
    }


    if (conn2NIOS.sock.rbuf->bcount > 0) {
	if (conn2NIOS.rtag == 0) {
	    int *rpids;
            rpids = conn2NIOS.task_duped;
            for (i=0; i<child_cnt; i++) {
                if (FD_NOT_VALID(children[i]->stdio) || !children[i]->running
			|| !children[i]->stdin_up)
                    continue;

                for (j=0; j<conn2NIOS.num_duped; j++)
                    if (children[i]->rpid == rpids[j])
                        break;

                if (j >= conn2NIOS.num_duped)
                    break;
            }
	}
	else {
            for (i=0; i<child_cnt; i++) {
                if (FD_NOT_VALID(children[i]->stdio) || !children[i]->running
			|| !children[i]->stdin_up)
                    continue;
                if (children[i]->rpid == conn2NIOS.rtag)
		    break;
            }
	}
        if (i >= child_cnt) {
            conn2NIOS.sock.rbuf->bcount = 0;
            conn2NIOS.rtag = -1;
            conn2NIOS.num_duped = 0;
        }
    }


    if (conn2NIOS.sock.rbuf->bcount > 0) {
        for (i = 0; i < child_cnt; i++) {
            if (FD_NOT_VALID(children[i]->stdio) || !children[i]->running
                    || !children[i]->stdin_up)
                continue;
	    if (children[i]->i_buf.bcount == 0)
                dochild_buffer(children[i], DOREAD);
        }
    }

    if (FD_IS_VALID(conn2NIOS.sock.fd)
          && conn2NIOS.sock.wbuf->bcount == 0 && conn2NIOS.sock.wcount == 0) {

        if (previousIndex >= child_cnt)
            previousIndex = -1;
        ch_cnt = child_cnt;
        for (j = 0; j < child_cnt; j++) {

            i = (j + previousIndex +1) % child_cnt;


            if (children[i]->std_out.buffer.bcount > 0) {

                dochild_buffer(children[i], DOWRITE);

            } else if ((children[i]->sigchild && !children[i]->server)
		       || children[i]->std_out.endFlag == 1) {

                dochild_buffer(children[i], DOWRITE);

	    } else if (children[i]->std_err.buffer.bcount > 0) {

		dochild_buffer(children[i], DOSTDERR);

	    } else if ((children[i]->sigchild && !children[i]->server)
		       || children[i]->std_err.endFlag == 1) {

		dochild_buffer(children[i], DOSTDERR);
	    }

            if (conn2NIOS.sock.wbuf->bcount > 0) {
                previousIndex = i;
                break;
            }


	    if (child_cnt < ch_cnt) {
		j--;
		ch_cnt--;
                previousIndex = i - 1;
	    }
        }
    }


}

static void
unblockSignals(void)
{
    sigset_t sigMask;

    sigprocmask(SIG_SETMASK, NULL, &sigMask);
    sigdelset(&sigMask, SIGCHLD);
    sigprocmask(SIG_SETMASK, &sigMask, NULL);
}

static void
blockSignals(void)
{
    sigset_t sigMask;

    sigemptyset(&sigMask);
    sigaddset(&sigMask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigMask, NULL);
}


void
resExit_(int exitCode)
{

    if (exitCode != 0) {
	ls_syslog(LOG_ERR, I18N_Exiting);
    }

    exit(exitCode);
}


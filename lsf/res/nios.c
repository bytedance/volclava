/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: nios.c 397 2007-11-26 19:04:00Z mblack $
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
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/wait.h>
#include "../lsf.h"
#include "../../lsbatch/lsbatch.h"
#include <setjmp.h>
#include "../lib/mls.h"
#include "res.h"
#include "nios.h"
#include "resout.h"
#include "../lib/lproto.h"

#define NL_SETN         29

extern LS_LONG_INT atoi64_(char* ptr);
extern int  requeued;
static void serv(char **, int);
static void PassSig(int);

static void exSuspend(struct lslibNiosHdr *);
static void do_newtask(void);
static void emusig(int, int);
static void reset_uid(void);
static void conin(int);
static void setStdout(struct lslibNiosHdr *);
static void setStdin(struct lslibNiosHdr *);
static void getStdin(struct lslibNiosHdr *);
static void rtask(struct lslibNiosHdr *);
static void rwait(struct lslibNiosHdr *);
static void exExit(struct lslibNiosHdr *);
static void remOn(struct lslibNiosHdr *);
static void remOff(struct lslibNiosHdr *);
static int die(void);
static int acceptCallBack(int);

static int cmpJobStates( struct jobInfoEnt* );
static int printJobSuspend( LS_LONG_INT);
void prtJobStateMsg(struct jobInfoEnt *, struct jobInfoHead* );
char *get_status(struct jobInfoEnt* );
struct loadIndexLog* initLoadIndex(void);
void prtLine(char* );
void JobExitInfo(void);
void checkPendingJobStatus(int s);
JOB_STATUS  getJobStatus(LS_LONG_INT jid, struct jobInfoEnt **job,
                         struct jobInfoHead **jobHead);
int  JobStateInfo(LS_LONG_INT );

static int   niosPid;
#define ERR_SYSTEM      122

#define MIN_CPU_TIME 0.0001
#define BLANKLEN   22
#define WIDTH      80
static int cursor = 0;

int chfd;
int ppid;
int usepty;
int niosSyncTasks = 0;
int lineBuffered = 1;
char *taggingFormat = (char *) NULL;
int stdoutSync = 0;
int heartbeatInterval = 0;
int jobStatusInterval = 0;
int standalone = FALSE;
int niosSbdMode = FALSE;
LS_LONG_INT jobId = -1;
int      pendJobTimeout = 0;
int  msgInterval = 0;

void kill_self(int, int);
char *getTimeStamp(void);

static fd_set nios_rmask, nios_wmask;
static int endstdin;
static int io_fd;
static int directecho = FALSE;
static int inbg;
static int remon;
static char buf[BUFSIZ];

static int stdinBufEmptyEvent = 0;
#define STDIN_FD  0
#define STDOUT_FD 1
#define STDERR_FD 2

static int exit_sig = 0;
static int exit_status = 0;
static int got_eof = FALSE;
static int got_status = FALSE;
static int callbackAccepted = FALSE;
static int sent_tstp = FALSE;
static int msgEnabled = FALSE;
static int standaloneTaskDone = 0;

static int forwardTSTP = 0;
static void myHandler(int sig)
{
    if (sig == SIGUSR2 && !niosSbdMode) {
       PassSig(SIGTSTP);
       forwardTSTP = 1;
    }
}

int ls_niosetdebug(int);

static int niosDebug = 0;
static int maxtasks;
static int maxfds;

static struct config_param niosParams[] = {
#define LSF_NIOS_DEBUG 0
        {"LSF_NIOS_DEBUG", NULL},
#define LSF_PTY 1
        {"LSF_ENABLE_PTY", NULL},
#define LSB_INTERACT_MSG_ENH 2
        {"LSB_INTERACT_MSG_ENH", NULL},
#define LSB_INTERACT_MSG_INTVAL 3
        {"LSB_INTERACT_MSG_INTVAL", NULL},
#define LSF_NIOS_RES_HEARTBEAT 4
        {"LSF_NIOS_RES_HEARTBEAT", NULL},
#define LSF_NIOS_JOBSTATUS_INTERVAL 5
        {"LSF_NIOS_JOBSTATUS_INTERVAL", NULL},
#define LSB_INTERACT_MSG_EXITTIME 6
        {"LSB_INTERACT_MSG_EXITTIME", NULL},
        {NULL, NULL}
    };

#define MSG_POLLING_INTR 60

extern void checkJobStatus(int numTries);

#define NIOS_MAX_TASKTBL       10024

#define MAX_TRY_TIMES           20




static void
signalBufEmpty(int dummy)
{

}

int
main(int argc, char **argv)
{
    static char          fname[] = "nios/main()";
    ushort               port;
    int                  asock;
    socklen_t            len;
    struct sockaddr_in   sin;
    sigset_t             sigmask;
    char                 *sp;
    char                 *timeout;

    setbuf(stdout, 0);

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTSTP);
    sigaddset(&sigmask, SIGUSR2);
    sigaddset(&sigmask, SIGCONT);
#if defined(SIGWINCH)
    sigaddset(&sigmask, SIGWINCH);
#endif
#if defined(SIGWINDOW)
    sigaddset(&sigmask, SIGWINDOW);
#endif
    sigprocmask(SIG_SETMASK, &sigmask, 0);

    reset_uid();
    initenv_(niosParams, NULL);

    if (niosParams[LSF_NIOS_DEBUG].paramValue) {
        niosDebug = (atoi(niosParams[LSF_NIOS_DEBUG].paramValue) > 0);
    } else
        niosDebug = 0;

    if (niosDebug > 0) {
        ls_initdebug ("nios");
        ls_niosetdebug(niosDebug);
    }

    if (niosParams[LSF_NIOS_RES_HEARTBEAT].paramValue) {
        if ( isint_(niosParams[LSF_NIOS_RES_HEARTBEAT].paramValue)) {

            heartbeatInterval = atoi(niosParams[LSF_NIOS_RES_HEARTBEAT].paramValue) * 60;
            if ( heartbeatInterval < 0) {
                heartbeatInterval = 0;
            }
        }
    }

    if (niosParams[LSF_NIOS_JOBSTATUS_INTERVAL].paramValue) {
        if ( isint_(niosParams[LSF_NIOS_JOBSTATUS_INTERVAL].paramValue)) {

            jobStatusInterval = atoi(niosParams[LSF_NIOS_JOBSTATUS_INTERVAL].paramValue) * 60;
            if ( jobStatusInterval < 0) {
                jobStatusInterval = 0;
            }
        }
    }


    timeout = getenv("LSF_NIOS_PEND_TIMEOUT");
    if (timeout != NULL) {
        pendJobTimeout = atoi(timeout);
        if (pendJobTimeout < 1) {
            pendJobTimeout = 0;
        } else {
            pendJobTimeout = pendJobTimeout * 60;
        }
    }


    Signal_(SIGPIPE, SIG_IGN);
    Signal_(SIGHUP, (SIGFUNCTYPE) PassSig);
    Signal_(SIGINT, (SIGFUNCTYPE) PassSig);
    Signal_(SIGQUIT, (SIGFUNCTYPE) PassSig);
    Signal_(SIGTERM, (SIGFUNCTYPE) PassSig);
    Signal_(SIGTSTP, (SIGFUNCTYPE) PassSig);
    Signal_(SIGUSR2, (SIGFUNCTYPE) myHandler);
    Signal_(SIGCONT, (SIGFUNCTYPE) conin);

    Signal_(SIGTTOU, SIG_IGN);
    Signal_(SIGTTIN, SIG_IGN);

#  if defined(SIGWINCH)

    if (getenv("LSF_NIOS_IGNORE_SIGWINDOW") != NULL) {
        Signal_(SIGWINCH, SIG_IGN);
    } else {
        Signal_(SIGWINCH, (SIGFUNCTYPE) PassSig);
    }
#  endif
#  if defined(SIGWINDOW)

    if (getenv("LSF_NIOS_IGNORE_SIGWINDOW") != NULL) {
        Signal_(SIGWINDOW, SIG_IGN);
    } else {
        Signal_(SIGWINDOW, (SIGFUNCTYPE) PassSig);
    }
#  endif


    if ( argc == 2 && strcmp(argv[1], "-V") == 0 ) {
        fputs(_LS_VERSION_, stdout);
        exit(0);
    }

    if ((argc != 3 && argc != 4) || (argc == 4 && strcmp(argv[1], "-n") &&
         strcmp(argv[1], "-N") && strcmp(argv[1], "-p"))) {
        fprintf(stderr,
            "%s: %s {chfd | -n retsock | -p portno | -N asock} usepty\r\n",
            I18N_Usage,
            argv[0]);
        exit(-1);
    }

    endstdin = 0;
    inbg = 0;
    remon = 1;
    niosPid = getpid();

    if (argc == 3) {

        standalone = FALSE;
        chfd = atoi(argv[1]);
        usepty = atoi(argv[2]);
        if (niosParams[LSF_PTY].paramValue &&
            !strcasecmp(niosParams[LSF_PTY].paramValue, "n"))
            usepty = 0;

        io_block_(chfd);

        if (read(chfd, (char *) &ppid, sizeof (ppid)) <= 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "read");
            exit(-1);
        }


        if ((asock = TcpCreate_(TRUE, 0)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "TcpCreate_");
            exit(-1);
        }
        len = sizeof(sin);
        if (getsockname (asock, (struct sockaddr *) &sin, &len) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "getsockname");
            (void)closesocket(asock);
            exit (-1);
        }
        port = sin.sin_port;

        if (write(chfd, &port, sizeof (port)) <0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "write");
            (void)closesocket(asock);
            exit(-1);
        }
        if ((maxfds = ls_nioinit(asock)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_nioinit");
            closesocket(asock);
            exit(-1);
        }
    } else {
        int sock;

        usepty = atoi(argv[3]);
        if (niosParams[LSF_PTY].paramValue &&
            !strcasecmp(niosParams[LSF_PTY].paramValue, "n"))
            usepty = 0;

        standalone = TRUE;

        if (niosDebug) {
            ls_syslog(LOG_DEBUG, "%s: Nios running in standalone mode", fname);
        }

        if (!strcmp(argv[1], "-p")) {
            int asock;

            if ((asock = TcpCreate_(TRUE, atoi(argv[2]))) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname,
                          "tcpCreate", "-p");
                exit(-1);
            }


            sock = acceptCallBack(asock);
        } else if (!strcmp(argv[1], "-N")) {

            if (niosDebug) {
                ls_syslog(LOG_DEBUG,"%s: Nios running in sbdMode mode",
                          fname);
            }


            if (getenv("BSUB_BLOCK") != NULL) {

                msgEnabled = FALSE;
            } else {

                if( niosParams[LSB_INTERACT_MSG_ENH].paramValue != NULL &&
                    strcasecmp(niosParams[LSB_INTERACT_MSG_ENH].paramValue,"y") == 0 ) {
                    msgEnabled = TRUE;

                    if (niosParams[LSB_INTERACT_MSG_INTVAL].paramValue) {
                        if (isint_(niosParams[LSB_INTERACT_MSG_INTVAL].paramValue)) {
                            msgInterval = atoi(niosParams[LSB_INTERACT_MSG_INTVAL].paramValue);
                        }
                    }

                    if (msgInterval <= 0) {
                        msgInterval = MSG_POLLING_INTR;
                    }

                    if ((sp = getenv("LSB_JOBID")) == NULL) {
                        ls_syslog(LOG_ERR, I18N(5803,
                                                "%s: LSB_JOBID is not defined %M"), /* catgets 5803 */
                                  fname);
                        exit(-1);
                    }

                    jobId = atoi64_(sp);

                    if( lsb_init("nios") < 0) {
                        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                                  fname, "lsb_init");
                        exit (-lsberrno);
                    }


                    ls_initdebug("nios");

                    if( atexit( JobExitInfo ) < 0 ) {
                        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname,
                              "atexit : JobExitInfo");
                        exit(-1);
                    }
                }
            }

            inithostsock_();
            niosSbdMode = TRUE;
            inithostsock_();
            sock = acceptCallBack(atoi(argv[2]));
        } else {
            sock = atoi(argv[2]);
        }

        callbackAccepted = TRUE;

        if ((maxfds=ls_nioinit(0)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname,
                              "ls_nioinit");
            exit(-1);
        }

        if (ls_nionewtask(1, sock) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname,
                              "ls_nionewtask");
            exit(-1);
        }

    }

    maxtasks = NIOS_MAX_TASKTBL;


    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

    if (isatty(0)) {
        if (!isatty(1) && isatty(2))
            directecho = 1;
        io_fd = 0;
    } else if (isatty(1))
        io_fd = 1;
    else
        io_fd = -1;
    if (usepty && io_fd >= 0 && isatty(io_fd))
        ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);

    serv(argv,asock);


    exit(0);
}

static void
serv(char **argv, int asock)
{
    static char fname[] = "nios/serv()";
    fd_set  rmask, wmask;
    int     nready;
    int     i, cc;
    int     readCount = 0;
    char    *bp = NULL;
    int     *tid_list;
    struct nioInfo *tasks;
    static int first = 1;
    struct  finishStatus *taskStatus;
    int     oldState = JOB_STAT_RUN;
    int     isResumed = 0;
    int     dumpOption = 0;
    int outputFd;

    tid_list = (int *) calloc(maxtasks, sizeof (int));
    if (tid_list == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        exit(-1);
    }

    taskStatus = (struct finishStatus *)calloc(maxtasks, sizeof (struct finishStatus));
    if (taskStatus == NULL ) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        exit(-1);
    }


    for(i=0; i < maxtasks; i++) {
        taskStatus[i].got_eof = 0;
        taskStatus[i].got_status = 0;
        taskStatus[i].sendSignal = 0;
    }

    FD_ZERO(&nios_rmask);
    FD_ZERO(&nios_wmask);


    if (io_nonblock_(0) == -1) {

        if (read(0, 0, 0) == 0)
            FD_SET(0, &nios_rmask);
    }
    else {
        if ((read(0, 0, 0) == 0) || errno == EAGAIN)
            FD_SET(0, &nios_rmask);

        if (io_block_(0) == -1)

            FD_CLR(0, &nios_rmask);
    }


    if (!standalone) {
        FD_SET(chfd, &nios_rmask);
    }

    for (;;) {
        int m;
        rmask = nios_rmask;
        wmask = nios_wmask;

        m = ls_niotasks(NIO_TASK_ALL, tid_list, maxtasks);

        if (m == 0) {
            FD_CLR(STDIN_FD, &rmask);
            if(standalone) {
                if (niosDebug) {
                    ls_syslog(LOG_DEBUG, "\
%s: Nios ls_niotasks returned 0, got_eof=%d",
                              fname, got_eof);
                    ls_syslog(LOG_DEBUG, "\
%s: Nios exit_sig=%d exit_status=%d. exiting",
                              fname, exit_sig, exit_status);
                }
                PassSig(SIGKILL);
                die();
            }
        } else if (m < 0) {
            PassSig(SIGKILL);
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "ls_niotasks");
            die();
        }

        if (usepty || standalone)
            first = 0;
        if (first) {
            i = ls_niotasks(NIO_TASK_CONNECTED, NULL, maxtasks);
            if (i == 0 || i < niosSyncTasks)
                FD_CLR(STDIN_FD, &rmask);
            else
                first = 0;
        }

        if (niosDebug) {
            ls_syslog(LOG_DEBUG,"%s: Nios into select", fname);
        }


        nready = ls_nioselect(maxfds, &rmask, &wmask, (fd_set *) NULL,
                             &tasks, (struct timeval *) NULL);
        if (nready < 0) {
            if (LSE_SYSCALL(lserrno) && errno == EINTR)
                continue;

            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_nioselect");

            if ( niosSbdMode && standalone
                 && (jobStatusInterval > 0) ) {

                checkJobStatus(-1);
            } else {
                ls_niokill(SIGKILL);
                die();
            }
        }

        if (tasks != NULL) {
            for (i = 0; i < tasks->num; i++) {
                switch (tasks->ioTask[i].type) {
                    case NIO_STDOUT:
                    case NIO_STDERR:
                        if (niosDebug) {
                            if (tasks->ioTask[i].type == NIO_STDERR) {
                                ls_syslog(LOG_DEBUG, "\
%s: Nios Got stderr from connection <%d>",
                                          fname, tasks->ioTask[i].tid);
                            } else {
                                ls_syslog(LOG_DEBUG,"\
%s: Nios Got stdout from connection <%d>",
                                          fname, tasks->ioTask[i].tid);
                            }
                        }

                        if (stdoutSync && !usepty && !standalone)
                            break;

                        if (tasks->ioTask[i].type == NIO_STDERR) {
                            outputFd = STDERR_FD;
                        } else {
                            outputFd = STDOUT_FD;
                        }

                        if (usepty || standalone) {
                            if (usepty &&
                                tasks->ioTask[i].type == NIO_STDERR) {
                                dumpOption = 2;
                            } else {
                                dumpOption = 0;
                            }
                        } else {
                            dumpOption = lineBuffered;
                        }
                        if (ls_niodump(outputFd, tasks->ioTask[i].tid,
                                       dumpOption, taggingFormat)<0) {
                            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                                      fname, "ls_niodump");
                            ls_niokill(SIGKILL);
                            die();
                        }
                        break;
                    case NIO_EOF:
                        if (niosDebug)
                            ls_syslog(LOG_DEBUG,"\
%s: Nios got EOF from remote task standalone=%d got_status=%d",
                                      fname, standalone, got_status);
                        taskStatus[tasks->ioTask[i].tid].got_eof = 1;
                        if ( taskStatus[tasks->ioTask[i].tid].got_status
                             && !taskStatus[tasks->ioTask[i].tid].sendSignal) {
                            emusig(tasks->ioTask[i].tid, tasks->ioTask[i].status);
                            taskStatus[tasks->ioTask[i].tid].sendSignal = 1;
                        }

                        if (standalone) {
                            got_eof = TRUE;
                            if (got_status) {
                                kill_self(exit_sig, exit_status);
                            }
                        }
                        break;

                    case NIO_STATUS:
                        if (niosDebug)
                            ls_syslog(LOG_DEBUG,"\
%s: Nios Got status <%#x> from task remote",
                                      fname, tasks->ioTask[i].status);
                        taskStatus[tasks->ioTask[i].tid].got_status = TRUE;

                        if ( REX_FATAL_ERROR(tasks->ioTask[i].status)) {

                            taskStatus[tasks->ioTask[i].tid].got_eof = TRUE;
                        }


                        {
                            LS_WAIT_T status = *((LS_WAIT_T *)&tasks->ioTask[i].status);
                            if (LS_WIFSTOPPED( status)){

                                if((niosSbdMode == TRUE)&&(msgEnabled == TRUE)) {

                                    if(LS_WSTOPSIG(status) == SIGCONT) {
                                        prtLine(I18N(801,
                                                     "Starting after being resumed")); /* catgets 801 */
                                        isResumed = 1;

                                    } else if( ( LS_WSTOPSIG(status) == SIGTSTP
                                                 ||  LS_WSTOPSIG(status) == SIGSTOP)
                                               && isResumed ){

                                        int retVal = 0;
                                        isResumed = 0;

                                        retVal = printJobSuspend( jobId );
                                        if (retVal == 0){
                                            prtLine(I18N(802, "The job was suspended")); /* catgets 802 */
                                        }
                                    } else {
                                        int tryTimes = MAX_TRY_TIMES;
                                        isResumed = 0;
                                        do {

                                             sleep(1);
                                             tryTimes --;
                                             oldState = JobStateInfo( jobId );

                                        } while ( (oldState == JOB_STAT_RUN) && (tryTimes >=0) );

                                    }
                                }

                                emusig(tasks->ioTask[i].tid, tasks->ioTask[i].status);

                            }
                        }

                        if (taskStatus[tasks->ioTask[i].tid].got_eof
                            && !taskStatus[tasks->ioTask[i].tid].sendSignal){
                            emusig(tasks->ioTask[i].tid, tasks->ioTask[i].status);
                            taskStatus[tasks->ioTask[i].tid].sendSignal = 1;
                        }
                        if (standalone) {
                            got_status = TRUE;
                            if (got_eof) {
                                kill_self(exit_sig, exit_status);
                            }
                        }
                        break;
                    case NIO_IOERR:
                        if (standalone) {
                            ls_syslog(LOG_ERR, I18N(5806, "\
%s: Nios IO_ERR while reading from remote task"), /* catgets 5806 */
                                      fname);

                            if ( (jobStatusInterval > 0) && niosSbdMode) {

                                checkJobStatus(-1);
                            } else {
                                kill_self(0, -1);
                            }
                        } else {

                            kill(ppid, SIGUSR1);
                        }
                        break;

                    case NIO_REQUEUE:
                        if (requeued && niosSbdMode && standalone) {
                            if (niosDebug)
                                ls_syslog(LOG_DEBUG,"\
%s: Nios got REQUEUE from remote task standalone=%d got_status=%d",
                                    fname, standalone, got_status);
                            fprintf(stderr, I18N(803,
                                    "<<Job has been requeued, waiting for dispatch......>>\n")); /* catgets 803 */
                            if (usepty && io_fd >= 0 && isatty(io_fd)) {
                                ls_loctty(io_fd);

                                fprintf(stderr, "\r");
                            }
                            lsfExecv(argv[0],argv);

                            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                                      fname, "execv");
                            exit(-1);
                        }
                        break;

                    default:
                        PassSig(SIGKILL);
                        die();
                }
            }
        }

        if (!standalone && FD_ISSET(chfd, &rmask)) {
            do_newtask();
            continue;
        }

        if (FD_ISSET(STDIN_FD, &rmask) && (readCount == 0)) {
            bp = buf;

            readCount = read(STDIN_FD, bp, BUFSIZ);
            if (readCount > 0) {
                if (usepty && directecho) {

                    if (bp[0] == '\r') {
                        bp[1] = '\n';
                        write(2, bp, 2);
                    } else {
                        write(2, bp, readCount);
                    }
                }

                if (niosDebug) {
                    ls_syslog(LOG_DEBUG,"\
%s: Nios NIOS2RES_STDIN <%d> bytes to remote task",
                            fname, readCount);
                }

                if ((cc = ls_niowrite(bp, readCount)) < 0) {
                    PassSig(SIGKILL);
                    die();
                } else {
                    readCount -= cc;
                    bp += cc;

                    if (readCount == 0) {
                        signalBufEmpty(stdinBufEmptyEvent);

                    }
                }
                if (inbg) {
                    inbg = 0;
                    PassSig(SIGCONT);
                    if (remon && usepty && io_fd >= 0 && isatty(io_fd))
                        ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);
                }
            } else if (readCount == 0) {
                {
                    if (niosDebug) {
                        ls_syslog(LOG_DEBUG,"\
%s: Nios got <EOF> NIOS2RES_EOF for all current remote tasks",
                                fname);
                    }
                    ls_nioclose();
                    FD_CLR(STDIN_FD, &nios_rmask);
                    endstdin = 1;
                }
            } else {
                readCount = 0;
                if (errno == EINTR || errno == EIO) {

                    if (errno == EIO && !inbg)
                        inbg = 1;
                    continue;
                }

                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                          fname, "reading stdin");

                signalBufEmpty(stdinBufEmptyEvent);

            }
        } else if (readCount != 0) {
            if ((cc = ls_niowrite(bp, readCount)) < 0) {
                PassSig(SIGKILL);
                die();
            } else {
                readCount -= cc;
                bp += cc;
                if (readCount == 0) {
                    signalBufEmpty(stdinBufEmptyEvent);

                }
            }
            if (inbg) {
                inbg = 0;
                PassSig(SIGCONT);
                if (remon && usepty && io_fd >= 0 && isatty(io_fd))
                    ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);
            }
        }

    }
}

static int
die(void)
{
    if (niosDebug)
        ls_syslog(LOG_DEBUG, "nios die");

    if (usepty && io_fd >= 0 && isatty(io_fd))
        ls_loctty(io_fd);
    if (!standalone)
        kill(ppid, SIGTERM);
    exit(-1);
}


void
kill_self(int exit_sig, int exit_stat)
{
    static char fname[] = "kill_self()";

    if (niosDebug) {
        ls_syslog(LOG_DEBUG,"\
%s: Nios kill_self, exit_sig=%d exit_stat=%d",
                  fname, exit_sig, exit_stat);
    }

    if (usepty && io_fd >= 0 && isatty(io_fd))
        ls_loctty(io_fd);
    if (exit_sig)
    {
        if (exit_sig >= 3 && exit_sig <= 12)
        {

#ifdef RLIMIT_CORE
            struct rlimit rl = {0, 0};
            setrlimit(RLIMIT_CORE, &rl);
#else
            rename("core", "core.real");
#endif
        }

        switch (exit_sig) {
        case STATUS_REX_NOMEM:
            lserrno = LSE_RES_NOMEM;
            break;
        case STATUS_REX_FATAL:
            lserrno = LSE_RES_FATAL;
            break;
        case STATUS_REX_CWD:
            lserrno = LSE_RES_DIR;
            break;
        case STATUS_REX_PTY:
            lserrno = LSE_RES_PTY;
            break;
        case STATUS_REX_SP:
            lserrno = LSE_RES_SOCK;
            break;
        case STATUS_REX_FORK:
            lserrno = LSE_RES_FORK;
            break;
        case STATUS_REX_UNKNOWN:
            lserrno = LSE_RES_FATAL;
            break;

#if defined(SIGXCPU)
        case SIGXCPU:
            ls_syslog(LOG_ERR, I18N(5807, "\
%s: Nios receives signal SIGXCPU, exit\n"), /* catgets 5807 */
                      fname);
            exit(exit_stat);
            break;
#endif
#if defined(SIGXFSZ)
        case SIGXFSZ:
            ls_syslog(LOG_ERR, I18N(5808, "\
%s: Nios receives signal SIGXFSZ, exit"), /* catgets 5808 */
                      fname);
            exit(exit_stat);
            break;
#endif
        case STATUS_REX_MLS_INVAL:
            lserrno = LSE_MLS_INVALID;
            break;
        case STATUS_REX_MLS_CLEAR:
            lserrno = LSE_MLS_CLEARANCE;
            break;
        case STATUS_REX_MLS_RHOST:
            lserrno = LSE_MLS_RHOST;
            break;
        case STATUS_REX_MLS_DOMIN:
            lserrno = LSE_MLS_DOMINATE;
            break;
        default:

            Signal_(exit_sig, SIG_DFL);
            kill(getpid(), exit_sig);
            ls_syslog(LOG_ERR, I18N(5809, "\
%s: Nios does not die at sig %d: errno %d"), /* catgets 5809 */
                      fname, exit_sig, errno);
            exit(-1);
        }

        ls_syslog(LOG_ERR, I18N(5810,
                  "%s: Nios Failed to create the task"), /* catgets 5810 */
                  fname);
        exit(-10);
    }

    exit(exit_stat);

}


static void
do_newtask(void)
{
    static char fname[] = "do_newtask()";
    struct lslibNiosHdr hdr;
    static int first = 1;
    if (first) {
        if (first == 1)
            first++;
        else if (first == 2) {
            first = 0;
            stdoutSync = 0;
        }
    }

    if (b_read_fix(chfd, (char *)&hdr, sizeof(hdr)) != sizeof(hdr)) {

        PassSig(SIGKILL);
        die();
    }

    switch (hdr.opCode) {
        case LIB_NIOS_RTASK:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s: Nios got LIB_NIOS_RTASK",
                          fname);
            }
            rtask(&hdr);
            break;

        case LIB_NIOS_RWAIT:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s: Nios got LIB_NIOS_RWAIT",
                          fname);
            }
            rwait(&hdr);
            break;

        case LIB_NIOS_REM_ON:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s: Nios got LIB_NIOS_REM_ON",
                          fname);
            }
            remOn(&hdr);
            break;

        case LIB_NIOS_REM_OFF:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_REM_OFF",
                          fname);
            }
            remOff(&hdr);
            break;

        case LIB_NIOS_SETSTDOUT:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_SETSTDOUT",
                          fname);
            }
            setStdout(&hdr);
            break;

        case LIB_NIOS_SETSTDIN:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_SETSTDIN",
                          fname);
            }
            setStdin(&hdr);
            break;

        case LIB_NIOS_GETSTDIN:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_GETSTDIN",
                          fname);
            }
            getStdin(&hdr);
            break;

        case LIB_NIOS_EXIT:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_EXIT",
                          fname);
            }
            exExit(&hdr);
            ls_syslog(LOG_ERR, I18N(5811,
                      "nios: LIB_NIOS_EXIT returned!")); /* catgets 5811 */
            break;

        case LIB_NIOS_SUSPEND:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_SUSPEND",
                          fname);
            }
            exSuspend(&hdr);
            break;

        case LIB_NIOS_SYNC:
            if (niosDebug) {
                ls_syslog(LOG_DEBUG, "%s Nios got LIB_NIOS_SYNC: %d",
                          fname, hdr.len);
            }
            niosSyncTasks = hdr.len;
            hdr.opCode = SYNC_OK;
            if (b_write_fix(chfd, (char *) &hdr, sizeof(hdr)) != sizeof(hdr)) {
                PassSig(SIGKILL);
                die();
            }
            break;

        default:
            ls_syslog(LOG_ERR, I18N(5812, "\
%s: No such service provided by NIOS code = %d"), /* catgets 5812 */
                      fname, hdr.opCode);
    }
}


static void
rtask(struct lslibNiosHdr *hdr)
{
    static char fname[] = "rtask()";
    struct lslibNiosRTask req;

    if (b_read_fix(chfd, (char *)&req.r, sizeof(req.r)) != sizeof(req.r)) {
        PassSig(SIGKILL);
        die();
    }

    if (niosDebug)
        ls_syslog(LOG_DEBUG, "%s: parent registered rpid %d, peer %s",
                  fname, req.r.pid,
                  inet_ntoa(req.r.peer));

    if (req.r.pid < 0) {
        req.r.pid = -req.r.pid;
        niosSyncTasks = maxtasks;
        stdoutSync = 1;
    }

    if (ls_nionewtask(req.r.pid, 0) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_nionewtask");
        if (lserrno == LSE_MALLOC) {
            PassSig(SIGKILL);
            die();
        }
    }
}


static void
rwait(struct lslibNiosHdr *hdr)
{
    int retVal;
    struct lslibNiosWaitReq req;
    struct lslibNiosWaitReply reply;


    if (b_read_fix(chfd, (char *)&req.r, sizeof(req.r)) != sizeof(req.r)) {
        PassSig(SIGKILL);
        die();
    }

    cleanRusage(&(reply.r.ru));

    retVal = ls_niostatus(req.r.tid, &reply.r.status, &reply.r.ru);
    if (retVal == -1) {
        hdr->opCode = CHILD_FAIL;
        hdr->len = 0;
        if (b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr))
            != sizeof(struct lslibNiosHdr)) {

            PassSig(SIGKILL);
            die();
        }
    } else if (retVal == 0) {
        hdr->opCode = NONB_RETRY;
        hdr->len = 0;
        if (b_write_fix(chfd, (char *)hdr,
                         sizeof(struct lslibNiosHdr)) !=
                         sizeof(struct lslibNiosHdr)) {
            PassSig(SIGKILL);
            die();
        }
    } else {
        reply.hdr.opCode = CHILD_OK;
        reply.hdr.len = sizeof(reply.r);
        reply.r.pid = retVal;

        if (b_write_fix(chfd, (char *)&reply, sizeof(reply)) !=
                         sizeof(reply)) {
            PassSig(SIGKILL);
            die();
        }
    }
}

static void
remOn(struct lslibNiosHdr *hdr)
{
    if (!endstdin)
        FD_SET(STDIN_FD, &nios_rmask);
    remon = 1;
    if (usepty && io_fd >= 0 && isatty(io_fd))
        ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);

    hdr->opCode = REM_ONOFF;
    if (b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr)) !=
        sizeof(struct lslibNiosHdr)) {
        PassSig(SIGKILL);
        die();
    }
}

static void
remOff(struct lslibNiosHdr *hdr)
{
    FD_CLR(STDIN_FD, &nios_rmask);
    remon = 0;
    if (usepty && io_fd >= 0 && isatty(io_fd))
        ls_loctty(io_fd);

    hdr->opCode = REM_ONOFF;
    if (b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr)) !=
        sizeof(struct lslibNiosHdr)) {
        PassSig(SIGKILL);
        die();
    }
}

static void
exExit(struct lslibNiosHdr *hdr)
{
    if (usepty && io_fd >= 0 && isatty(io_fd))
        ls_loctty(io_fd);

    PassSig(SIGKILL);

    hdr->opCode = NIOS_OK;
    b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr));
    exit(0);
}

static void
setStdout(struct lslibNiosHdr *hdr)
{
    static struct lslibNiosStdout req;
    int err;
    static int first = TRUE;

    err = 0;

    if (b_read_fix(chfd, (char *)&req.r, sizeof(req.r)) != sizeof(req.r)) {
        PassSig(SIGKILL);
        die();
    }

    if (req.r.set_on)
        lineBuffered = 1;
    else
        lineBuffered = 0;

    if (req.r.len) {
        if (first) {
            req.format = (char *) calloc(MAXLINELEN, sizeof(char));
            if (req.format == NULL) {
                perror("calloc failed");
                exit(-1);
            }
            first = FALSE;
        }
        if (b_read_fix(chfd, (char *) req.format, req.r.len*sizeof(char))
            != req.r.len*sizeof(char)) {
            PassSig(SIGKILL);
            die();
        }
        taggingFormat = req.format;
    }

    if (err)
        hdr->opCode = STDOUT_FAIL;
    else
        hdr->opCode = STDOUT_OK;

    if (b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr))
        != sizeof(struct lslibNiosHdr)) {
        PassSig(SIGKILL);
        die();
    }
}

static void
setStdin(struct lslibNiosHdr *hdr)
{
    static char fname[] = "setStdin()";
    static struct lslibNiosStdin req;
    int i, err;
    static int first = TRUE;

    err = 0;

    if (first) {
        req.rpidlist = (int *) calloc(maxtasks, sizeof(int));
        if (req.rpidlist == NULL) {
            perror("calloc failed");
            exit(-1);
        }
        first = FALSE;
    }

    if (b_read_fix(chfd, (char *)&req.r, sizeof(req.r)) != sizeof(req.r)) {
        PassSig(SIGKILL);
        die();
    }

    if (req.r.len) {
        if (b_read_fix(chfd, (char *)req.rpidlist, req.r.len*sizeof(int))
            != req.r.len*sizeof(int)) {
            PassSig(SIGKILL);
            die();
        }
        for (i=0; i < req.r.len; i++) {
            if (ls_nioctl(req.rpidlist[i], req.r.set_on ?
                      NIO_STDIN_ON : NIO_STDIN_OFF) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_nioctl");
                err = 1;
            }
        }
    }
    if (err)
        hdr->opCode = STDIN_FAIL;
    else
        hdr->opCode = STDIN_OK;

    if (b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr))
        != sizeof(struct lslibNiosHdr)) {
        PassSig(SIGKILL);
        die();
    }
}

static void
getStdin(struct lslibNiosHdr *hdr)
{
    static char fname[] = "nios/getStdin()";
    int retVal;
    static struct lslibNiosStdin req;
    static struct lslibNiosGetStdinReply reply;
    static int first = TRUE;

    if (first) {
        reply.rpidlist = (int *) calloc(maxtasks, sizeof(int));
        req.rpidlist = (int *) calloc(maxtasks, sizeof(int));
        if (req.rpidlist == NULL) {
            perror(_i18n_printf( I18N_FUNC_FAIL, fname, "calloc"));
            exit(-1);
        }
        first = FALSE;
    }

    if (b_read_fix(chfd, (char *)&req.r.set_on, sizeof(req.r.set_on)) !=
        sizeof(req.r.set_on)) {
        PassSig(SIGKILL);
        die();
    }

    retVal = ls_niotasks(req.r.set_on ? NIO_TASK_STDINON : NIO_TASK_STDINOFF,
                       reply.rpidlist, maxtasks);
    if (retVal < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "ls_niotasks");

        PassSig(SIGKILL);
        die();
    }

    reply.hdr = *hdr;
    reply.hdr.len = retVal * sizeof(int);
    reply.hdr.opCode = STDIN_OK;

    if (b_write_fix(chfd, (char *)&reply, sizeof(reply.hdr) + reply.hdr.len)
        != sizeof(reply.hdr) + reply.hdr.len) {
        PassSig(SIGKILL);
        die();
    }
}


static void
emusig(int tid, int st)
{
    static char fname[] = "emusig()";
    SIGFUNCTYPE handle;
    LS_WAIT_T status = *((LS_WAIT_T *)&st);

    if (LS_WIFSTOPPED(status)) {

        if (niosDebug) {
            ls_syslog(LOG_DEBUG, "%s: Nios remote stopped", fname);
        }


        if (standalone) {
            if (niosSbdMode) {
                if (sent_tstp)
                    sent_tstp = FALSE;
                else
                    return;


                if (usepty == 2) {
                    return;
                }
            }

            if (usepty && io_fd >= 0)
                ls_loctty(io_fd);
            handle = Signal_(LS_WSTOPSIG(status), SIG_DFL);
            kill(getpid(), LS_WSTOPSIG(status));
            Signal_(LS_WSTOPSIG(status), handle);
            if (usepty && io_fd >= 0)
                ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);
        }

       else if (forwardTSTP && ! niosSbdMode) {
            forwardTSTP = 0;
            kill(-getpgrp(), SIGSTOP);
        }
        return;
    } else if (LS_WIFSIGNALED(status) || LS_WIFEXITED(status)) {




        if (standalone) {
            if (LS_WIFSIGNALED(status))
                exit_sig = LS_WTERMSIG(status);
            exit_status = LS_WEXITSTATUS(status);
            if (exit_status == 0)
                standaloneTaskDone = 1;

            if (niosDebug) {
                ls_syslog(LOG_DEBUG,"\
%s: Nios remote signaled exit_sig=<%d> exit_status=<%d>",
                        fname, exit_sig, exit_status);
            }

        } else {



            if (niosDebug) {
                ls_syslog(LOG_DEBUG,"\
%s: Nios signaled exit_sig=<%d> sending SIGUSR1 to oparent",
                          fname,  LS_WTERMSIG(status));
            }

            kill(ppid, SIGUSR1);
        }
    }
}

static void
PassSig(int signo)
{
    static char fname[] = "PassSig()";
    sigset_t omask, nmask;

    if (niosDebug) {
        ls_syslog(LOG_DEBUG,"\
%s: Nios NIOS2RES_SIGNAL delivering signal = <%d> to remote tasks.",
                  fname, signo);
    }


    if (getenv("LSF_NIOS_DIE_CMD") && !callbackAccepted) {

        execl("/bin/sh", "/bin/sh", "-c", getenv("LSF_NIOS_DIE_CMD"), NULL);
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "execl");
        exit(-10);
    }


    sigfillset(&nmask);
    sigprocmask(SIG_BLOCK, &nmask, &omask);


    if (ls_niokill(signo) < 0) {
        if (lserrno == LSE_BAD_XDR)
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M,
                      fname, ls_niokill, "failed to xdr");
        else {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                      fname, "ls_niokill");
            die();
        }
    }

    sigprocmask(SIG_SETMASK, &omask, NULL);
    if (signo == SIGTSTP && standalone) {
        if (niosSbdMode)
            sent_tstp = TRUE;
        if (remon && usepty && io_fd >= 0 && isatty(io_fd))
            ls_loctty(io_fd);

        if (remon && usepty && io_fd >= 0 && isatty(io_fd))
            ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);
    }

}


static void
exSuspend(struct lslibNiosHdr *hdr)
{
    if (remon && usepty && io_fd >= 0 && isatty(io_fd))
        ls_loctty(io_fd);

    hdr->opCode = NIOS_OK;
    if (b_write_fix(chfd, (char *)hdr, sizeof(struct lslibNiosHdr)) !=
        sizeof(struct lslibNiosHdr)) {
        PassSig(SIGKILL);
        die();
    }
    kill(getpid(),SIGSTOP);

    if (remon && usepty && io_fd >= 0 && isatty(io_fd))
        ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);
}

static void
conin(int signo)
{
    PassSig(signo);
    if (inbg) {
        inbg = 0;
        if (remon && usepty && io_fd >= 0 && isatty(io_fd))
            ls_remtty(io_fd, usepty == 1 ? TRUE : FALSE);
    }
}

static void
reset_uid(void)
{
    static char fname[] = "nios/reset_uid()";
    int ruid = getuid();

    if (geteuid() == 0) {
       if (  lsfSetREUid(ruid,ruid) < 0

           || (lsfSetREUid(-1,0) >= 0 && ruid != 0)) {


            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "setreuid");
            exit(ERR_SYSTEM);
       }
    }
}


static int
acceptCallBack(int asock)
{
    static char          fname[] = "nios/acceptCallBack()";
    int                  sock;
    char                 *sp;
    struct niosConnect   connReq;
    int                  iofd;
    int                  redirect;
    LS_WAIT_T            status;
    int                  out_status;
    int                  verbose ;

    verbose = (getenv("BSUB_QUIET") == NULL);

    if ((sp = getenv("LSB_JOBID")) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: LSB_JOBID is not defined", fname);
        exit(-1);
    }

    jobId = atoi64_(sp);

    for (;;) {

        if ( standalone && niosSbdMode
             && ((jobStatusInterval > 0)
                 || (pendJobTimeout > 0)
                 || (msgInterval > 0))) {
            checkPendingJobStatus(asock);
        }

        if ((sock = doAcceptResCallback_(asock, &connReq)) < 0) {
            if (niosDebug)
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                          fname, "doAcceptResCallback_()");
        } else {
            if (niosDebug)
                ls_syslog(LOG_DEBUG, "%s: jobId %s opCode %d",
                          fname, lsb_jobid2str(jobId), connReq.rpid);
            if (connReq.rpid == jobId)
                break;

            if (connReq.rpid == -jobId) {

                if (getenv("BSUB_BLOCK") != NULL) {
                    status = *((LS_WAIT_T *)&connReq.exitStatus);

                    if (LS_WIFSIGNALED(status) || LS_WIFEXITED(status)) {

                        out_status = LS_WEXITSTATUS(status);

                        if (connReq.terWhiPendStatus == 1) {
                            if (!getenv("BSUB_QUIET2")) {
                                fprintf(stderr, I18N(804,
                                        "<<Terminated while pending>>\r\n")); /* catgets 804 */
                            }
                            exit(connReq.exitStatus);
                        }
                        else {
                           if (!getenv("BSUB_QUIET2")) {
                               fprintf(stderr, I18N(805,
                                       "<<Job is finished>>\r\n")); /* catgets 805 */
                            }
                            exit(out_status);
                        }
                    }
                }
                if (verbose) {
                    fprintf(stderr, I18N(804,
                        "<<Terminated while pending>>\r\n")); /* catgets 804 */

                    exit(-10);
                }
            }

            closesocket(sock);
        }
    }


    if (!(niosSbdMode)) {
        closesocket(asock);
    }


    if (verbose) {
        struct sockaddr_in   sin;
        socklen_t            len = sizeof(sin);

        if (getpeername(sock, (struct sockaddr *)&sin, &len) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "getpeername");
        } else {
            struct hostent *hp;

            hp = Gethostbyaddr_(&sin.sin_addr.s_addr,
                                sizeof(in_addr_t),
                                AF_INET);
            if (hp == NULL) {
                ls_syslog(LOG_ERR, "\
%s: gethostbyaddr() failed for %s", __func__, sockAdd2Str_(&sin));

            } else {
                fprintf(stderr, "\
<<Starting on %s>>\r\n", hp->h_name);
            }
        }
    }

    if (!usepty)
        return (sock);

    redirect = 0;
    if (isatty(0)) {
        if (!isatty(1))
            redirect = 1;
        iofd = 0;
    } else if (isatty(1)) {
        redirect = 1;
        iofd = 1;
    } else {
        ls_syslog(LOG_ERR, I18N(5816, "\
%s: usepty specified but TTY not detected\r\n"), /* catgets 5816 */
                  fname);
        exit(-1);
    }

    if (do_rstty_(sock, iofd, redirect) == -1) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, "do_rstty_()");
        exit(-1);
    }

    return (sock);
}
char *
getTimeStamp(void)
{
    static char timeStamp[64];
    time_t      t;

    t = time(NULL);

    strncpy(timeStamp, ctime(&t), 19);
    timeStamp[19] = '\0';
    sprintf(timeStamp + strlen(timeStamp)," %d ", niosPid);

    return(timeStamp);
}



int
JobStateInfo(LS_LONG_INT jid)
{
    struct jobInfoHead *jobInfoHead=NULL;
    struct jobInfoEnt  *jobInfo=NULL;
    JOB_STATUS    status;
    int    retval=0;

    status = getJobStatus(jid, &jobInfo, &jobInfoHead);
    switch (status) {
    case JOB_STATUS_FINISH:

        retval = JOB_STAT_DONE;
        break;
    case JOB_STATUS_KNOWN:
        if (jobInfo && jobInfoHead) {
            if( cmpJobStates(jobInfo) != 0 ) {

                prtJobStateMsg(jobInfo, jobInfoHead);
            }
            retval = jobInfo->status;
        } else {

            retval = 0;
        }
        break;
    case JOB_STATUS_ERR:
    case JOB_STATUS_UNKNOWN:
    default:
        retval = 0;
        break;
    }

    return(retval);
}



int
cmpJobStates( struct jobInfoEnt* job)
{
    static char fname[] = "cmpJobStates()";
    static int  status = JOB_STAT_UNKWN;
    static int* reasonTb = NULL;
    static int  numReasons = 0;
    static int  reasons = 0;
    static int  subreasons = 0;
    int stat = 0;
    int i;

    if(job->status == status) {
        switch( job->status ) {
        case JOB_STAT_PEND:
            if( job->numReasons == numReasons ) {
                for( i=0; i< numReasons ; i++) {
                    if( job->reasonTb[i] != reasonTb[i] ) {
                        stat = -1;
                        break;
                    }
                }
            }
            else {
                stat = -1;
            }
            break;
        case JOB_STAT_PSUSP:
        case JOB_STAT_SSUSP:
        case JOB_STAT_USUSP:
            if((job->reasons != reasons)||(job->subreasons != subreasons)) {
                stat = -1;
            }
            break;
        case JOB_STAT_RUN:
            if( status == JOB_STAT_SSUSP || status == JOB_STAT_USUSP )
                stat = -1;
            break;
        default:
            stat = 0;
        }
    }
    else {
        stat = -1;
    }


    status = job->status;
    numReasons = job->numReasons;

    if( reasonTb != NULL )
        FREEUP(reasonTb);
    if( numReasons ) {
        reasonTb = (int*)malloc(sizeof(numReasons)*numReasons);
        if( reasonTb == NULL ) {
             ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "cmpJobStates",
                "malloc", sizeof(numReasons)*numReasons);
            numReasons = 0;
        }
    }
    for( i=0; i < numReasons; i++)
        reasonTb[i] = job->reasonTb[i];

    reasons = job->reasons;
    subreasons = job->subreasons;

    if(niosDebug)
        ls_syslog(LOG_DEBUG,
        "%s: status =%d,numReasons=%d,reasonTb[0]=%d,reasons=%d,subreasons=%d,STAT=%d\n",
                  fname, status, numReasons,
                  (reasonTb != NULL)? reasonTb[0]:0,
                  reasons, subreasons, stat);

    return stat;
}


void
alarmHandler(int signum)
{
    prtLine(I18N(807, "Job status query timed out.")); /* catgets 807 */
    exit(exit_status);
}


void
JobExitInfo(void)
{
    int jStatus;
    int niosExitTimeOut=-1, i;


    if (standaloneTaskDone) {
        char msg[MSGSIZE];

        memset(msg, 0, MSGSIZE);
        sprintf(msg, I18N(808,
                "Job <%s> is done successfully."),  /* catgets 808 */
                lsb_jobid2str(jobId));
        prtLine(msg);
        return;
    }


    for (i=1; i<NSIG; i++) {
        Signal_(i, SIG_DFL);
    }


    Signal_(SIGALRM, (SIGFUNCTYPE) alarmHandler);
    if ( niosParams[LSB_INTERACT_MSG_EXITTIME].paramValue ) {
        if (isint_(niosParams[LSB_INTERACT_MSG_EXITTIME].paramValue) ) {
            niosExitTimeOut = atoi(niosParams[LSB_INTERACT_MSG_EXITTIME].paramValue);
        }
    }

    if (niosExitTimeOut > 0 ) {
        alarm(niosExitTimeOut);
    }
    prtLine(I18N(809, "Job finished, querying job status ...")); /* catgets 809 */
    prtLine(I18N(810, "To interrupt, press Ctrl-C."));  /* catgets 810 */

    while (1) {
        jStatus = JobStateInfo(jobId);
        if ( (jStatus == JOB_STAT_RUN)
             || (jStatus == JOB_STAT_SSUSP)
             || (jStatus == JOB_STAT_USUSP) ) {

            sleep(1);
        } else {

            break;
        }
    }
    return;
}

void
prtJobStateMsg(struct jobInfoEnt *job, struct jobInfoHead *jInfoH)
{
    char prline[MSGSIZE]="";
    time_t doneTime;
    static struct loadIndexLog *loadIndex = NULL;
    char *pendReasons=NULL;

    if (loadIndex == NULL)
        TIMEIT(1, loadIndex = initLoadIndex(), "initLoadIndex");

    doneTime = job->endTime;

    switch (job->status) {
    case JOB_STAT_DONE:
        sprintf(prline, I18N(811,
                "Job <%s> is done successfully."), /* catgets 811 */
                lsb_jobid2str(job->jobId));
        prtLine(prline);
        break;

    case JOB_STAT_EXIT:
        if (job->reasons & EXIT_ZOMBIE) {
            sprintf(prline, I18N(812,
                    "<Termination request issued; the job will be killed once the host is ok>")); /* catgets 812  */
            prtLine(prline);
            break;
        }
        if (strcmp(get_status(job), "DONE") == 0)
        {
            sprintf(prline, I18N(811,
                    "Job <%s> is done successfully."), /* catgets 811 */
                    lsb_jobid2str(job->jobId));
        }
        else {
            LS_WAIT_T wStatus;
            LS_STATUS(wStatus) = job->exitStatus;

            if (job->exitStatus) {
                if (WEXITSTATUS(wStatus)) {
                    sprintf(prline, I18N(813,
                            "Job <%s> exited with exit code %d."), /* catgets  813  */
                            lsb_jobid2str(job->jobId), WEXITSTATUS(wStatus));
                }
                else{
                    sprintf(prline, I18N(814,
                            "Job <%s> has been terminated by user or administrator:\n Exited by signal %d"), /* catgets  814  */
                            lsb_jobid2str(job->jobId), WTERMSIG(wStatus));
                }
            }
            else {
                sprintf(prline, I18N(815,
                        "Job <%s> has been terminated by user or administrator"), /* catgets 815 */
                        lsb_jobid2str(job->jobId));
            }
        }
        prtLine(prline);
        break;
    case JOB_STAT_PSUSP:
    case JOB_STAT_PEND:
        pendReasons = lsb_pendreason(job->numReasons, job->reasonTb,
                                     jInfoH, loadIndex,
                                     job->numLimitDetail,
                                     job->limitDetailTb);
        sprintf(prline,"%s",pendReasons);
        prtLine(prline);

        break;
    case JOB_STAT_SSUSP:
    case JOB_STAT_USUSP:
        if (job->reasons) {
            if ((job->reasons == SUSP_USER_STOP )
                ||(job->reasons == SUSP_ADMIN_STOP))  {
                sprintf(prline, I18N(816,
                        "The job was suspended by user or administrator;")); /* catgets 816 */
            } else {
                sprintf(prline, "%s", lsb_suspreason(job->reasons,
                                                     job->subreasons,
                                                     loadIndex));
            }
            prtLine(prline);
        }
        break;
    case JOB_STAT_RUN:
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
    static char fname[] = "initLoadIndex()";
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
        loadIndex.nIdx = lsInfo->numIndx;
        if (!names)
            if(!(names =(char **)malloc(lsInfo->numIndx*sizeof(char *)))) {
                lserrno=LSE_MALLOC;
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "initLoadIndex");
                return NULL;
            }
        for (i = 0; i < loadIndex.nIdx; i++)
            names[i] = lsInfo->resTable[i].name;
        loadIndex.name = names;
    }
    return (&loadIndex);
}

void
prtLine(char *line)
{
    int length, flag;

    if(line[0] != '\n') {
        fprintf(stderr, "\r\n");
        cursor = 2;
    }
    else cursor = 0;

    fprintf(stderr, "<< ");
    cursor += 3;

    flag = 1;
    length = 0;
    while(flag) {
        if(line[length] == '\n') {
            if(line[length+1] != '\0')
                fprintf(stderr, " >>\r\n<< ");
            cursor = 0;
            length++;
            continue;
        }
        if(cursor == (WIDTH - 7)) {
            fprintf(stderr, " >>\r\n<< ");
            cursor = 0;
            continue;
        }
        fprintf(stderr, "%c", line[length]);
        length++;
        cursor++;
        if(line[length] == '\0') {
            fprintf(stderr, " >>\r\n");
            flag = 0;
        }
    }
    fflush(stderr);
}

int
printJobSuspend( LS_LONG_INT jid )
{
    struct jobInfoHead *jobInfoHead = NULL;
    struct jobInfoEnt* jobInfo = NULL;
    int    tryTimes = MAX_TRY_TIMES;
    JOB_STATUS  jobStatus=JOB_STATUS_UNKNOWN;

    do {
        jobStatus = getJobStatus(jid, &jobInfo, &jobInfoHead);
        if (jobStatus != JOB_STATUS_KNOWN || jobInfo == NULL) {
            break;
        }
        sleep(1);
        tryTimes--;
    } while( (jobInfo->status == JOB_STAT_RUN) && (tryTimes>=0) );

    if(jobInfo) {
        cmpJobStates(jobInfo);
        (void)prtJobStateMsg(jobInfo, jobInfoHead);
        return(jobInfo->status);
    } else {
        return 0;
    }

}


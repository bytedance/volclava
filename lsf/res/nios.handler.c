/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: nios.handler.c 397 2007-11-26 19:04:00Z mblack $
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
#include <ctype.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../lib/lib.h"
#include "../lib/lib.osal.h"
#include "../../lsbatch/lsbatch.h"
#include "../intlib/list.h"
#include "../res/rescom.h"
#include "nios.h"

#define NL_SETN         29
typedef struct dead_tid {
    int tid;
    struct dead_tid *next;
}   Dead_tid;


typedef struct dead_rpid {
    int rpid;
    LS_WAIT_T   status;
    struct rusage ru;
    struct dead_rpid *next;
}   Dead_rpid;

int requeued = 0;

static int do_newconnect(int);
static int get_connect(int, struct LSFHeader *);
static int get_status(int, struct LSFHeader *, LS_WAIT_T *);
static void setMask(fd_set *);
static int bury_task(LS_WAIT_T, struct rusage *, int);
static int do_setstdin(int, int);
static int flush_buffer(void);
static int check_timeout_task(void);
static void add_list(struct nioInfo *, int, int, LS_WAIT_T *);
static int deliver_signal(int);
static int flush_sigbuf(void);
static int flush_databuf(void);
static int flush_eof(void);
static int deliver_eof(void);
static int sendUpdatetty();
static void checkHeartbeat(int nready);
static int sendHeartbeat(void);
void checkJobStatus(int numTries);
JOB_STATUS  getJobStatus(LS_LONG_INT jid, struct jobInfoEnt **job,
                         struct jobInfoHead **jobHead);

int  JobStateInfo(LS_LONG_INT jid);

#define MAXLOOP 10000

#define CHECK_PERIOD     4

static Dead_rpid *dead_rpid;

static fd_set socks_bit;
static fd_set ncon_bit;


#define C_CONNECTED(cent)                                               \
    ((cent).rpid && ((cent).sock.fd != -1) && ((cent).rtime == 0))

#define DISCONNECT(id) {if (conn[id].rpid > 0)          \
            connIndexTable[conn[id].rpid-1] = -1;       \
        closesocket(conn[id].sock.fd);                  \
        conn[id].sock.fd = -1;                          \
        conn[id].rpid = 0;                              \
        FD_CLR(id, &socks_bit);}
LIST_T *notifyList;

typedef struct taskNotice {
    struct taskNotice *forw, *back;
    int tid;
    int opCode;
} taskNotice_t;

typedef struct rtaskInfo {
    struct rtaskInfo *forw, *back;
    int tid;
    time_t rtime;
    int eof;
    int dead;
} rtaskInfo_t;

#define MAX_READ_RETRY         5

bool_t compareTaskId(rtaskInfo_t *, int *, int);
static rtaskInfo_t * getTask(LIST_T *, int);
static rtaskInfo_t * addTask(LIST_T *);
static void removeTask(LIST_T *, rtaskInfo_t *);
static int addNotifyList(LIST_T *, int, int);
static int addTaskList(int, int);
static int notify_task(int, int);
static int readResMsg(int);
static int getFirstFreeIndex(void);

struct connInfo {
    Channel sock;
    int rpid;
    time_t rtime;
    int bytesWritten;
    int eof;
    int dead;


    RelayLineBuf *rbuf;
    LIST_T *taskList;
    int rtag;
    int wtag;
    char *hostname;

    Dead_tid *deadtid;
};

static struct connInfo *conn;
static struct nioEvent *ioTable;
static struct nioEvent *ioTable1;
static struct nioInfo abortedTasks;

static struct {
    int empty;
    int length;
    fd_set socks;
    char buf[BUFSIZ + LSF_HEADER_LEN  ];
} writeBuf;

struct sigbuf {
    int sigval;
    struct sigbuf *next;
};

static struct sigbuf *sigBuf;
static int lastConn = 0;

static int maxfds;
static int maxtasks;
static int *connIndexTable;
static int count_unconn = 0;
static int acceptSock;
static int sendEof;

static int nioDebug = 0;
static time_t lastCheckTime=0;

extern int   msgInterval;

extern char *getTimeStamp();
extern void  kill_self(int, int);

extern int  JobStateInfo(LS_LONG_INT jid);

int
ls_niosetdebug(int debug)
{
    if (debug > 0)
        nioDebug = debug;
    return 0;
}

int
ls_nioinit(int sock)
{
    int i;
    char listname[56];


#ifdef RLIMIT_NOFILE
    struct rlimit rlp;
    getrlimit (RLIMIT_NOFILE, &rlp);
    rlp.rlim_cur = rlp.rlim_max;
    setrlimit (RLIMIT_NOFILE, &rlp);
    maxfds = rlp.rlim_max;
#else
    maxfds = sysconf(_SC_OPEN_MAX);
#endif

    notifyList = listCreate("notifications");
    if (notifyList == NULL) {
        lserrno = LSE_MALLOC;
        FREEUP(notifyList);
        return -1;
    }


    if (maxfds > 1024)
        maxfds = 1024;
    maxtasks = maxfds*maxfds;
    connIndexTable = (int *) malloc(maxtasks*sizeof(int));
    if (connIndexTable == NULL) {
        lserrno = LSE_MALLOC;
        FREEUP(connIndexTable);
        return -1;
    }
    for (i=0; i<maxtasks; i++)
        connIndexTable[i] = -1;

    acceptSock = sock;

    conn = (struct connInfo *) calloc(maxfds, sizeof(struct connInfo));
    ioTable = (struct nioEvent *) calloc(maxfds, sizeof(struct nioEvent));
    ioTable1 = (struct nioEvent *) calloc(maxfds, sizeof(struct nioEvent));
    if (conn == NULL || ioTable == NULL || ioTable1 == NULL) {
        lserrno = LSE_MALLOC;
        FREEUP(conn);
        FREEUP(ioTable);
        FREEUP(ioTable1);
        return -1;
    }

    abortedTasks.num = 0;
    abortedTasks.ioTask = ioTable1;

    for (i = 0 ; i < maxfds ; i++) {
        sprintf(listname, "Rtask_%d", i);
        conn[i].sock.fd = -1;
        conn[i].sock.rbuf = (RelayBuf *)NULL;
        conn[i].sock.rcount = 0;
        conn[i].sock.wbuf = (RelayBuf *)NULL;
        conn[i].sock.wcount = 0;
        conn[i].rpid = 0;
        conn[i].rtime = 0;
        conn[i].bytesWritten = 0;
        conn[i].eof = FALSE;
        conn[i].dead = FALSE;
        conn[i].rbuf = (RelayLineBuf *) NULL;
        conn[i].rtag = -1;
        conn[i].wtag = 0;
        conn[i].hostname = (char *) NULL;
        conn[i].taskList = listCreate(listname);
        conn[i].deadtid = NULL;
        if (conn[i].taskList == NULL) {
            lserrno = LSE_MALLOC;
            FREEUP(conn[i].taskList);
            return -1;
        }
    }
    writeBuf.empty = TRUE;
    writeBuf.length = 0;
    sendEof = FALSE;
    dead_rpid = NULL;
    sigBuf = NULL;
    lastConn = 0;

    return maxfds;
}

int
ls_nioselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
             struct nioInfo **tasks, struct timeval *timeout)
{
    static char fname[] = "ls_nioselect()";
    fd_set  rmask, wmask, emask, devNullMask;
    int     nready = 0;
    int     naborted = 0;
    int     i, j, retVal, first;
    int round2Go;
    LS_WAIT_T   status;
    struct timeval timeval;
    taskNotice_t *notice, *nextNotice;
    rtaskInfo_t *task;
    static int previousIndex = -1;

    struct timeval *tvp, tv1;
    static struct nioInfo readyTaskList;
    static int brokenSelectFlag = 0;
    static int checkedBrokenSelect = 0;
    static dev_t devNullDeviceNumber = 0;
    static fd_set stderrFlag;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }


    if (!checkedBrokenSelect) {
        int fd;
        fd_set tmask;
        struct timeval tm;
        struct stat sbuf;

        if ((fd = open("/dev/null", O_RDONLY)) < 0)

            checkedBrokenSelect = 1;
        else {
            FD_ZERO(&tmask);
            FD_SET(fd, &tmask);
            memset(&tm, 0, sizeof(tm));
            tm.tv_usec = 500;
            if (select(fd, &tmask, NULL, NULL, &tm) == 0) {

                brokenSelectFlag = 1;
                memset(&sbuf, 0, sizeof(sbuf));
                if (!fstat(fd, &sbuf))
                    devNullDeviceNumber = sbuf.st_rdev;
            }
            checkedBrokenSelect = 1;
            close(fd);
        }
    }


    readyTaskList.num = 0;
    readyTaskList.ioTask = ioTable;
    if (tasks)
        *tasks = (struct nioInfo *)NULL;

    if (abortedTasks.num > 0) {

        for (i=0; i<abortedTasks.num; i++) {
            add_list(&readyTaskList, abortedTasks.ioTask[i].tid,
                     abortedTasks.ioTask[i].type, NULL);
        }
        naborted = abortedTasks.num;
        abortedTasks.num = 0;

        timeval.tv_sec = 0;
        timeval.tv_usec = 0;
        tvp = &timeval;
        round2Go = 0;
    } else {
        timeval.tv_sec = CHECK_PERIOD;
        timeval.tv_usec = 0;

        if (timeout && timeout->tv_sec <= CHECK_PERIOD) {
            tvp = timeout;
            round2Go = 0;
        } else {
            tvp = &timeval;
            round2Go = timeout ? timeout->tv_sec/CHECK_PERIOD + 1 : -1;
        }
    }

    for(;;) {
        if (readfds)
            rmask = *readfds;
        else
            FD_ZERO(&rmask);
        if (writefds)
            wmask = *writefds;
        else
            FD_ZERO(&wmask);
        if (exceptfds)
            emask = *exceptfds;
        else
            FD_ZERO(&emask);
        setMask(&rmask);


        if (brokenSelectFlag && devNullDeviceNumber) {
            struct stat sbuf;
            FD_ZERO(&devNullMask);
            for (i = 0; i < nfds; i++) {
                if (FD_ISSET(i, &rmask)) {
                    memset(&sbuf, 0, sizeof(sbuf));
                    if (!fstat(i, &sbuf)) {
                        if (sbuf.st_rdev == devNullDeviceNumber) {
                            FD_SET(i, &devNullMask);
                            FD_CLR(i, &rmask);
                        }
                    }
                }
            }
        }

        if (nioDebug && notifyList->numEnts)
            ls_syslog(LOG_DEBUG, "%s: number of notifications: <%d>",
                      fname, notifyList->numEnts);


        if (writeBuf.empty) {
            for (notice = (taskNotice_t *) notifyList->forw;
                 notice != (taskNotice_t *) notifyList;
                 notice = nextNotice) {
                nextNotice = notice->forw;
                retVal = notify_task(notice->tid, notice->opCode);
                if (retVal == 0)
                    listRemoveEntry(notifyList, (LIST_ENTRY_T *) notice);
                else if (lserrno == LSE_BAD_CHAN || lserrno == LSE_BAD_ARGS)
                    listRemoveEntry(notifyList, (LIST_ENTRY_T *) notice);
                else if (lserrno == LSE_MALLOC || lserrno == LSE_NIO_INIT)
                    return (-1);
            }
        }



        tv1.tv_sec = tvp->tv_sec;
        tv1.tv_usec = tvp->tv_usec;
        nready = select(maxfds, &rmask, &wmask, &emask, &tv1);

        if (brokenSelectFlag && devNullDeviceNumber) {
            for (i = 0; i < nfds; i++) {
                if (FD_ISSET(i, &devNullMask)) {
                    FD_SET(i, &rmask);
                    nready++;
                }
            }
        }

        if ((retVal = check_timeout_task()) != 0) {

            lserrno = retVal;
            return(-1);
        }


        if ( standalone && niosSbdMode && heartbeatInterval > 0) {
            checkHeartbeat(nready);
        }

        if (nready < 0) {
            lserrno = LSE_SELECT_SYS;
            return(-1);
        } else if (nready == 0 && !sendEof) {
            if (round2Go == 0)
                return(nready);
            else if (round2Go == -1)
                continue;
            else {
                round2Go--;
                continue;
            }
        }


        if (acceptSock && FD_ISSET(acceptSock, &rmask)) {

            if (do_newconnect(acceptSock) < 0)
                return(-1);

            continue;
        }

        if (previousIndex >= lastConn)
            previousIndex = -1;
        first = 1;

        for (j =0; j <lastConn; j++) {

            i = (j + previousIndex + 1) % lastConn;



            if (!C_CONNECTED(conn[i]))
                continue;

            if (FD_ISSET(conn[i].sock.fd, &rmask)) {
                if (conn[i].sock.rcount == 0) {
                    struct LSFHeader msgHdr, bufHdr;
                    XDR xdrs;

                    if (conn[i].rbuf->bcount > 0)
                        continue;
                    xdrmem_create(&xdrs, (char *) &bufHdr,
                                  sizeof(struct LSFHeader), XDR_DECODE);

                    if (readDecodeHdr_(conn[i].sock.fd, (char *) &bufHdr,
                                       NB_SOCK_READ_FIX, &xdrs, &msgHdr) < 0) {
                        if (!conn[i].dead) {
                            memset((void *)&status, 0, sizeof(LS_WAIT_T));
                            SETTERMSIG(status, STATUS_IOERR);
                            if (bury_task(status, 0, conn[i].rpid) == -1) {
                                lserrno = LSE_MALLOC;
                                xdr_destroy(&xdrs);
                                return(-1);
                            }
                            add_list(&readyTaskList, conn[i].rpid,
                                     NIO_IOERR, NULL);
                        } else {

                            add_list(&readyTaskList, conn[i].rpid,
                                     NIO_EOF, NULL);
                        }
                        if (nioDebug)
                            ls_syslog(LOG_DEBUG, "\
%s: read hdr failure in connection <%d>: %d",
                                      fname, conn[i].rpid, conn[i].sock.fd);

                        DISCONNECT(i);
                        xdr_destroy(&xdrs);
                        continue;
                    }

                    xdr_destroy(&xdrs);

                    conn[i].rtag = msgHdr.reserved0;

                    if (nioDebug)
                        ls_syslog(LOG_DEBUG, "\
%s: got hdr <%d> with rtag <%d> from connection <%d>",
                                  fname, msgHdr.opCode,
                                  conn[i].rtag, conn[i].rpid);

                    switch(msgHdr.opCode) {
                        case RES2NIOS_STDOUT:
                            conn[i].sock.rcount = msgHdr.length;
                            conn[i].rbuf->bp = conn[i].rbuf->buf;
                            if (conn[i].rtag <= 0)
                                conn[i].rtag = conn[i].rpid;
                            FD_CLR(i, &stderrFlag);
                            break;
                        case RES2NIOS_STDERR:
                            conn[i].sock.rcount = msgHdr.length;
                            conn[i].rbuf->bp = conn[i].rbuf->buf;
                            if (conn[i].rtag <= 0)
                                conn[i].rtag = conn[i].rpid;
                            FD_SET(i, &stderrFlag);
                            break;
                        case RES2NIOS_NEWTASK:

                            retVal = addTaskList(conn[i].rtag, i);
                            if (retVal < 0 && lserrno == LSE_MALLOC) {
                                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
                                add_list(&readyTaskList, conn[i].rpid,
                                         NIO_IOERR, NULL);
                                add_list(&readyTaskList, conn[i].rpid,
                                         NIO_EOF, NULL);
                                naborted++;
                                DISCONNECT(i);
                                lserrno = retVal;
                                return(-1);
                            }
                            break;
                        case RES2NIOS_STATUS:
                            if (conn[i].rtag <= 0)
                                conn[i].rtag = conn[i].rpid;
                            retVal = get_status(i, &msgHdr, &status);
                            if (retVal == LSE_MALLOC) {
                                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
                                add_list(&readyTaskList, conn[i].rpid,
                                         NIO_IOERR, NULL);
                                add_list(&readyTaskList, conn[i].rpid,
                                         NIO_EOF, NULL);
                                naborted++;
                                DISCONNECT(i);
                                lserrno = retVal;
                                return(-1);
                            } else if (retVal == LSE_BAD_XDR) {
                                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "xdr");
                                add_list(&readyTaskList, conn[i].rpid,
                                         NIO_IOERR, NULL);
                                add_list(&readyTaskList, conn[i].rpid,
                                         NIO_EOF, NULL);
                                naborted++;
                                DISCONNECT(i);
                                continue;
                            }
                            add_list(&readyTaskList, conn[i].rtag,
                                     NIO_STATUS, &status);
                            break;

                        case RES2NIOS_EOF:
                            if (conn[i].rtag <= 0)
                                conn[i].rtag = conn[i].rpid;
                            if ((task = getTask(conn[i].taskList, conn[i].rtag)))
                                task->eof = TRUE;
                            add_list(&readyTaskList, conn[i].rtag, NIO_EOF, NULL);
                            break;


                        case RES2NIOS_REQUEUE:
                            if (conn[i].rtag <= 0)
                                conn[i].rtag = conn[i].rpid;
                            if ((task = getTask(conn[i].taskList, conn[i].rtag))){
                                task->eof = TRUE;
                                task->dead = TRUE;
                            }
                            add_list(&readyTaskList, conn[i].rtag, NIO_REQUEUE,
                                     NULL );
                            requeued = 1;
                            for (i=0; i<lastConn; i++) {
                                close(conn[i].sock.fd);
                            }
                            break;

                        case RES2NIOS_CONNECT:

                            get_connect(i, &msgHdr);
                            break;

                        default:
                            memset((void *)&status, 0, sizeof(LS_WAIT_T));
                            SETTERMSIG(status, STATUS_IOERR);
                            if (bury_task(status, 0, conn[i].rpid) == -1) {
                                lserrno = LSE_MALLOC;
                                return(-1);
                            }
                            add_list(&readyTaskList, conn[i].rpid,
                                     NIO_IOERR, &status);
                            add_list(&readyTaskList, conn[i].rpid, NIO_EOF, NULL);
                            naborted++;
                            DISCONNECT(i);
                            break;
                    }
                } else {
                    if (readResMsg(i) < 0) {
                        return (-1);
                    }
                    if (conn[i].sock.rcount == 0) {

                        if (FD_ISSET(i, &stderrFlag)) {
                            add_list(&readyTaskList, conn[i].rpid,
                                     NIO_STDERR, NULL);
                        } else {
                            add_list(&readyTaskList, conn[i].rpid,
                                     NIO_STDOUT, NULL);
                        }
                    }
                    if (first && conn[i].sock.rcount == 0) {
                        previousIndex = i;
                        first = 0;
                    }
                }
            }
        }
        break;
    }


    flush_buffer();


    if (readfds)
        *readfds = rmask;
    if (writefds)
        *writefds = wmask;
    if (exceptfds)
        *exceptfds = emask;
    if (tasks && readyTaskList.num)
        *tasks = &readyTaskList;
    return(nready + naborted);

}

static int
check_timeout_task()
{
    LS_WAIT_T status;
    int i, timeout_cnt;
    int resTimeout;
    time_t rtime;
    rtaskInfo_t *task, *nextTask;

    if (count_unconn <= 0)
        return 0;

    if (genParams_[LSF_RES_TIMEOUT].paramValue)
        resTimeout = atoi(genParams_[LSF_RES_TIMEOUT].paramValue);
    else
        resTimeout = RES_TIMEOUT;

    rtime = time(0);

    for (i = 0 ; i < lastConn ; i++) {
        if (conn[i].rpid == 0)
            continue;
        if (conn[i].sock.fd == -1) {
            if (conn[i].rtime > 0 && (rtime - conn[i].rtime) > resTimeout) {
                memset((void *)&status, 0, sizeof(LS_WAIT_T));
                SETTERMSIG(status, STATUS_TIMEOUT);
                if (bury_task(status, 0, conn[i].rpid) == -1) {
                    return(LSE_MALLOC);
                }
                add_list(&abortedTasks, conn[i].rpid, NIO_IOERR, NULL);
                add_list(&abortedTasks, conn[i].rpid, NIO_EOF, NULL);
                FD_CLR(i, &ncon_bit);
                count_unconn--;
            }
            continue;
        }
        if (!conn[i].taskList || LIST_IS_EMPTY(conn[i].taskList))
            continue;

        timeout_cnt = 0;
        for (task = (rtaskInfo_t *) conn[i].taskList;
             task != (rtaskInfo_t *) conn[i].taskList;
             task = nextTask) {
            nextTask = task->forw;
            if (task->rtime > 0 && (rtime - task->rtime) > resTimeout) {
                addNotifyList(notifyList, task->tid, STATUS_TIMEOUT);
                memset((void *)&status, 0, sizeof(LS_WAIT_T));
                SETTERMSIG(status, STATUS_TIMEOUT);
                if (bury_task(status, 0, task->tid) == -1) {
                    return(LSE_MALLOC);
                }
                add_list(&abortedTasks, task->tid, NIO_IOERR, NULL);
                add_list(&abortedTasks, task->tid, NIO_EOF, NULL);
                timeout_cnt++;
                count_unconn--;
            }
        }
        if (timeout_cnt == conn[i].taskList->numEnts)
            FD_CLR(i, &ncon_bit);
    }
    return(0);
}

int
ls_nioremovetask(int tid)
{
    int i;
    rtaskInfo_t *task;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid <= 0) {
        lserrno = LSE_BAD_TID;
        return -1;
    }

    if (tid > maxtasks) {
        lserrno = LSE_BAD_TID;
        return -1;
    }
    if ((i = connIndexTable[tid-1]) >= 0 && i < lastConn) {
        if (conn[i].rpid > 0 && conn[i].sock.fd != -1) {
            if ((task = getTask(conn[i].taskList, tid)) != NULL) {
                if (task->rtime != 0) {
                    FD_CLR(i, &ncon_bit);
                    count_unconn--;
                }
                addNotifyList(notifyList, tid, STATUS_TIMEOUT);
                removeTask(conn[i].taskList, task);
                connIndexTable[tid-1] = -1;
                if (LIST_IS_EMPTY(conn[i].taskList))
                    DISCONNECT(i);
            }
        }
        else if (conn[i].rpid > 0 && conn[i].rpid == tid) {

            FD_CLR(i, &ncon_bit);
            conn[i].rpid = 0;
            connIndexTable[tid-1] = -1;
            count_unconn--;
        }
        return(0);
    }
    lserrno = LSE_BAD_TID;
    return(-1);
}

static void
add_list(struct nioInfo *list, int tid, int ioType, LS_WAIT_T *status)
{
    list->ioTask[list->num].tid = tid;
    list->ioTask[list->num].type = ioType;
    if (status)
        list->ioTask[list->num].status = *((int *)status);
    list->num++;
}

int
ls_nioctl(int tid, int request)
{
    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }

    switch (request) {
        case NIO_STDIN_ON:
            if (do_setstdin(tid, 1) == -1) {
                lserrno = LSE_BAD_TID;
                return(-1);
            }
            break;
        case NIO_STDIN_OFF:
            if (do_setstdin(tid, 0) == -1) {
                lserrno = LSE_BAD_TID;
                return(-1);
            }
            break;
        default:
            lserrno = LSE_BAD_OPCODE;
            return(-1);
    }
    return(0);
}

int
ls_niowrite(char *buf, int len)
{
    int cc, i, retVal;
    char *bp;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }

    if (!writeBuf.empty) {
        if ((retVal =flush_buffer()) != 0) {
            lserrno = retVal;
            return(-1);
        }
        return(0);
    } else {
        struct LSFHeader reqHdr;
        XDR xdrs;

        if (len > BUFSIZ)
            cc = BUFSIZ;
        else
            cc = len;
        memcpy(writeBuf.buf + LSF_HEADER_LEN, buf, cc);
        bp = writeBuf.buf;
        reqHdr.opCode = NIOS2RES_STDIN;
        reqHdr.version = VOLCLAVA_VERSION;
        reqHdr.length = cc;


        for (i = 0 ; i < lastConn ; i++) {
            if (C_CONNECTED(conn[i]) && FD_ISSET(i, &socks_bit))
                break;
        }
        if (i < lastConn) {
            reqHdr.reserved0 = conn[i].wtag;
        }
        else {
            reqHdr.reserved0 = 0;
        }
        xdrmem_create(&xdrs, bp, sizeof(struct LSFHeader), XDR_ENCODE);
        if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
            xdr_destroy(&xdrs);
            lserrno = LSE_BAD_XDR;
            return(-1);
        }

        writeBuf.length = cc + XDR_GETPOS(&xdrs);
        xdr_destroy(&xdrs);

        writeBuf.socks = socks_bit;
        writeBuf.empty = FALSE;
        flush_buffer();
        return(cc);
    }
}

static int
flush_buffer()
{
    int retVal;

    if (!writeBuf.empty) {
        if ((retVal = flush_databuf()) != 0)
            return(retVal);
        if ((retVal = flush_sigbuf()) != 0)
            return(retVal);
        if ((retVal = flush_eof()) != 0)
            return(retVal);
    }
    return(0);
}

static int
flush_databuf()
{
    int i, cc, empty;
    LS_WAIT_T status;

    if (!writeBuf.empty) {
        empty = TRUE;
        for (i = 0 ; i < lastConn ; i++) {
            if (C_CONNECTED(conn[i]) && FD_ISSET(i, &writeBuf.socks)
                && (conn[i].bytesWritten != writeBuf.length)) {


                cc = write(conn[i].sock.fd,
                           writeBuf.buf + conn[i].bytesWritten,
                           writeBuf.length-conn[i].bytesWritten);
                if (cc > 0) {
                    conn[i].bytesWritten += cc;
                }
                if (conn[i].bytesWritten != writeBuf.length)
                    empty = FALSE;

                if ((cc < 0) && (errno != EPIPE) &&
                    BAD_IO_ERR(errno)) {
                    memset((void *)&status, 0, sizeof(LS_WAIT_T));
                    SETTERMSIG(status, STATUS_IOERR);
                    if (bury_task(status, 0, conn[i].rpid) == -1) {
                        return(LSE_MALLOC);
                    }
                    add_list(&abortedTasks, conn[i].rpid, NIO_IOERR, NULL);
                    add_list(&abortedTasks, conn[i].rpid, NIO_EOF, NULL);
                    DISCONNECT(i);
                }
            }
        }
        writeBuf.empty = empty;

        if (writeBuf.empty) {
            for (i = 0 ; i < lastConn ; i++)
                if (C_CONNECTED(conn[i]) && FD_ISSET(i, &writeBuf.socks))
                    conn[i].bytesWritten = 0;
        }
    }
    return(0);
}

static int
flush_eof()
{
    int retVal;

    if (writeBuf.empty && sendEof) {
        sendEof = FALSE;
        if ((retVal = deliver_eof()) != 0)
            return(retVal);
    }
    return(0);
}

int
ls_nioclose(void)
{
    int retVal;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }

    if (!writeBuf.empty) {
        sendEof = TRUE;
        return (0);
    }
    sendEof = FALSE;
    if ((retVal = deliver_eof()) != 0) {
        lserrno = retVal;
        return(-1);
    } else {
        return(0);
    }
}

static int
deliver_eof()
{
    static char fname[] = "deliver_eof()";
    struct LSFHeader reqHdr, buf;
    XDR xdrs;
    LS_WAIT_T status;
    sigset_t newMask, oldMask;
    int i, numTimeout, len;


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);

    reqHdr.opCode = NIOS2RES_EOF;
    reqHdr.version = VOLCLAVA_VERSION;
    reqHdr.length = 0;
    reqHdr.reserved0 = 0;

    xdrmem_create(&xdrs,
                  (char *)&buf,
                  sizeof(struct LSFHeader),
                  XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
        xdr_destroy(&xdrs);
        lserrno = LSE_BAD_XDR;
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return(-1);
    }
    len = XDR_GETPOS(&xdrs);
    xdr_destroy(&xdrs);

    for (i = 0 ; i < lastConn ; i++) {
        if (C_CONNECTED(conn[i])) {
            numTimeout = 0;
        RETRY:
            if (NB_SOCK_WRITE_FIX(conn[i].sock.fd, (char *)&buf, len) < 0 &&
                errno != EPIPE) {
                if (errno == ETIMEDOUT && numTimeout < 5) {
                    millisleep_(100);
                    numTimeout ++;
                    goto RETRY;
                }

                memset((void *)&status, 0, sizeof(LS_WAIT_T));
                SETTERMSIG(status, STATUS_IOERR);
                if (bury_task(status, 0, conn[i].rpid) == -1) {
                    lserrno = LSE_MALLOC;
                    sigprocmask(SIG_SETMASK, &oldMask, NULL);
                    return(-1);
                }
                ls_syslog(LOG_ERR, I18N(5901, "\
%s: Error writing EOF to task %d\n"), /* catgets 5901 */
                          fname, conn[i].rpid);
                add_list(&abortedTasks, conn[i].rpid, NIO_IOERR, NULL);
                add_list(&abortedTasks, conn[i].rpid, NIO_EOF, NULL);
                DISCONNECT(i);
            }
        }
    }
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(0);
}

int
ls_nioread(int tid, char *buf, int len)
{
    static char fname[] = "ls_nioread()";
    int index, cc;
    LS_WAIT_T status;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid <= 0) {
        lserrno = LSE_BAD_TID;
        return -1;
    }


    if (tid > maxtasks) {
        lserrno = LSE_BAD_TID;
        return -1;
    }
    index = connIndexTable[tid-1];
    if (index < 0 || index >= lastConn) {
        lserrno = LSE_BAD_TID;
        return(-1);
    }

    if (!conn[index].sock.rcount) {
        lserrno = LSE_NO_ERR;
        return(0);
    }

    if (conn[index].sock.rcount > len)
        cc = len;
    else
        cc = conn[index].sock.rcount;
    if ((cc = read(conn[index].sock.fd, buf, cc)) <= 0) {
        int sverrno = errno;
        if (cc == 0 || BAD_IO_ERR(errno)) {
            memset((void *)&status, 0, sizeof(LS_WAIT_T));
            SETTERMSIG(status, STATUS_IOERR);
            if (bury_task(status, 0, conn[index].rpid) == -1) {
                lserrno = LSE_MALLOC;
                return(-1);
            }
            add_list(&abortedTasks, conn[index].rpid, NIO_IOERR, NULL);
            add_list(&abortedTasks, conn[index].rpid, NIO_EOF, NULL);
            DISCONNECT(index);
        }
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "recv");
        errno = sverrno;
        lserrno = LSE_MSG_SYS;
        return(-1);
    }
    conn[index].sock.rcount -= cc;

    return(cc);
}

int
ls_nionewtask(int tid, int sock)
{
    static char fname[] = "ls_nionewtask()";
    int i;
    LS_WAIT_T status;
    rtaskInfo_t *task;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid <= 0) {
        lserrno = LSE_BAD_TID;
        return -1;
    }
    if (tid > maxtasks) {
        lserrno = LSE_TOOMANYTASK;
        return(-1);
    }
    i = connIndexTable[tid-1];
    if (i >= 0 && i < lastConn

        && ((task = getTask(conn[i].taskList, tid)) != NULL
            || conn[i].rpid == tid)) {
        if (((task) ? (task->rtime == 0) : (conn[i].rtime == 0))
            || (conn[i].sock.fd == -1) || (sock != 0)) {

            lserrno = LSE_TASKEXIST;
            return(-1);
        }
        if (task == NULL) {

            lserrno = LSE_BAD_TID;
            return (-1);
        }

        task->rtime = 0;
        if (conn[i].rtime != 0) {
            if (FD_ISSET(i, &ncon_bit))
                FD_SET(i, &socks_bit);
            else
                FD_CLR(i, &socks_bit);
            FD_CLR(i, &ncon_bit);
            io_nonblock_(conn[i].sock.fd);
        }
        conn[i].rtime = 0;
        count_unconn--;
        connIndexTable[tid-1] = i;
        if (nioDebug)
            ls_syslog(LOG_DEBUG, "\
%s: connection to new task <%d> is completed",
                      fname, tid);
        return (0);
    }
    else if ((i = getFirstFreeIndex()) < maxfds) {
        conn[i].eof = FALSE;
        conn[i].rpid = tid;
        conn[i].dead = FALSE;
        if (sock == 0) {
            conn[i].sock.fd = -1;
            conn[i].rtime = time(0);

            FD_SET(i, &ncon_bit);
            count_unconn++;
            if (nioDebug)
                ls_syslog(LOG_DEBUG, "%s: new task <%d> is registered",
                          fname, tid);
        } else {

            while (!LIST_IS_EMPTY(conn[i].taskList))
                removeTask(conn[i].taskList,
                           (rtaskInfo_t*) conn[i].taskList->forw);
            if (conn[i].rbuf != NULL)
                FREEUP(conn[i].rbuf);
            conn[i].rbuf = (RelayLineBuf *) malloc(sizeof(RelayLineBuf));
            if (conn[i].rbuf == NULL) {
                lserrno = LSE_MALLOC;
                return (-1);
            }
            conn[i].rbuf->bcount = 0;
            if ((task = addTask(conn[i].taskList)) == NULL) {
                return (-1);
            }
            task->tid = tid;
            task->eof = FALSE;
            task->dead = FALSE;
            task->rtime = 0;
            conn[i].sock.fd = sock;
            conn[i].rtime = 0;
            conn[i].sock.wcount = 0;
            conn[i].sock.wcount = 0;
            conn[i].rtag = -1;
            conn[i].wtag = 0;
            FD_SET(i, &socks_bit);
            io_nonblock_(conn[i].sock.fd);
            if (nioDebug)
                ls_syslog(LOG_DEBUG, "\
%s: new connection to task <%d> is completed",
                          fname, tid);
        }
        connIndexTable[tid-1] = i;
        if (i == lastConn)
            lastConn++;
        return (0);
    }

    memset((void *)&status, 0, sizeof(LS_WAIT_T));
    SETTERMSIG(status, STATUS_EXCESS);
    if (bury_task(status, 0, tid) < 0)
        lserrno = LSE_MALLOC;
    else
        lserrno = LSE_TOOMANYTASK;
    return(-1);
}

static int
do_setstdin(int tid, int stdinOn)
{
    int i, gotError;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid < 0 || tid > maxtasks) {
        lserrno = LSE_BAD_TID;
        return -1;
    }

    gotError = FALSE;

    if (stdinOn) {
        if (tid == 0) {
            for (i = 0; i < lastConn ; i++) {
                if (conn[i].rpid == 0)
                    continue;
                if (conn[i].sock.fd == -1 || conn[i].rtime != 0)
                    FD_SET(i, &ncon_bit);
                else
                    FD_SET(i, &socks_bit);
                conn[i].wtag = 0;
            }
            i = 0;
        }
        else {
            if ((i = connIndexTable[tid-1]) >=0 && i < lastConn) {
                if (getTask(conn[i].taskList, tid) != NULL) {
                    if (conn[i].rtime != 0) {
                        FD_ZERO(&ncon_bit);
                        FD_SET(i, &ncon_bit);
                    }
                    else {
                        FD_ZERO(&socks_bit);
                        FD_SET(i, &socks_bit);
                    }
                    conn[i].wtag = tid;
                }
                else if (conn[i].rpid == tid) {
                    if (conn[i].sock.fd == -1 || conn[i].rtime != 0) {
                        FD_ZERO(&ncon_bit);
                        FD_SET(i, &ncon_bit);
                    }
                    else {
                        FD_ZERO(&socks_bit);
                        FD_SET(i, &socks_bit);
                    }
                    conn[i].wtag = tid;
                }
            }
        }
        if (i < 0 || i == lastConn) {
            gotError = TRUE;
        }
    } else {
        if (tid == 0) {
            for (i = 0; i < lastConn ; i++) {
                if (conn[i].rpid == 0)
                    continue;
                if (conn[i].sock.fd == -1 || conn[i].rtime != 0)
                    FD_CLR(i, &ncon_bit);
                else
                    FD_CLR(i, &socks_bit);
                conn[i].wtag = -1;
            }
            i = 0;
        }
        else {
            if ((i = connIndexTable[tid-1]) >=0 && i < lastConn) {
                if (getTask(conn[i].taskList, tid) != NULL) {
                    if (conn[i].wtag == tid || conn[i].wtag == -1) {
                        if (conn[i].rtime != 0)
                            FD_CLR(i, &ncon_bit);
                        else
                            FD_CLR(i, &socks_bit);
                        conn[i].wtag = -1;
                    }
                }
                else if (conn[i].rpid == tid) {
                    if (conn[i].sock.fd == -1 || conn[i].rtime != 0)
                        FD_CLR(i, &ncon_bit);
                    else
                        FD_CLR(i, &socks_bit);
                    conn[i].wtag = -1;
                }
            }
        }
        if (i < 0 || i == lastConn) {
            gotError = TRUE;
        }
    }
    return(gotError);
}

int
ls_niotasks(int options, int *tidList, int len)
{
    int listLen, i;
    rtaskInfo_t *task, *nextTask;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }

    for (listLen = 0, i = 0; i < lastConn && listLen <= len; i++) {
        if (conn[i].rpid > 0) {
            for (task = (rtaskInfo_t *) conn[i].taskList->forw;
                 task != (rtaskInfo_t *) conn[i].taskList;
                 task = nextTask) {
                nextTask = task->forw;
                if (!options || options == NIO_TASK_ALL) {

                    tidList[listLen++] = task->tid;
                } else if (options == NIO_TASK_STDINON) {
                    if (FD_ISSET(i, &socks_bit) || FD_ISSET(i, &ncon_bit)) {
                        if (conn[i].wtag == 0)
                            tidList[listLen++] = task->tid;
                        else if (conn[i].wtag == task->tid)
                            tidList[listLen++] = task->tid;
                    }
                } else if (options == NIO_TASK_STDINOFF) {
                    if (FD_ISSET(i, &socks_bit) || FD_ISSET(i, &ncon_bit)) {
                        if (conn[i].wtag > 0 && conn[i].wtag != task->tid)
                            tidList[listLen++] = task->tid;
                    } else {
                        tidList[listLen++] = task->tid;
                    }
                } else if (options == NIO_TASK_CONNECTED) {
                    if (task->rtime ==0)
                        listLen++;
                }
            }
            if (options == NIO_TASK_CONNECTED)
                continue;

            if (!LIST_IS_EMPTY(conn[i].taskList))
                continue;
            if (!options || options == NIO_TASK_ALL) {

                tidList[listLen++] = conn[i].rpid;
            } else if (options == NIO_TASK_STDINON) {
                if (FD_ISSET(i, &socks_bit) || FD_ISSET(i, &ncon_bit))
                    tidList[listLen++] = conn[i].rpid;
            } else if (options == NIO_TASK_STDINOFF) {
                if (!FD_ISSET(i, &socks_bit) && !FD_ISSET(i, &ncon_bit))
                    tidList[listLen++] = conn[i].rpid;
            } else if (options == NIO_TASK_CONNECTED) {
                if (C_CONNECTED(conn[i]))
                    listLen++;
            }
        }
    }
    if (listLen > len) {
        lserrno = LSE_RPIDLISTLEN;
        return(-1);
    } else
        return(listLen);

}

int
ls_niostatus(int tid, int *status, struct rusage *rusage)
{
    Dead_rpid *prpid, *tmprpidp;
    sigset_t newMask, oldMask;
    int i;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid < 0 || tid > maxtasks) {
        lserrno = LSE_BAD_TID;
        return -1;
    }


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);


    prpid = dead_rpid;
    tmprpidp = dead_rpid;

    while (prpid) {
        if (tid == 0 || tid == prpid->rpid) {
            rtaskInfo_t *task=NULL;
            int connIndex;

            if (prpid->rpid-1 >= 0) {
                connIndex = connIndexTable[prpid->rpid-1];
            }
            else {
                connIndex = -1;
            }
            if (connIndex >= 0 && connIndex < lastConn)
                task = getTask(conn[connIndex].taskList, prpid->rpid);

            if (task ) {
                if (!task->eof && !REX_FATAL_ERROR(LS_STATUS(prpid->status))) {

                    tmprpidp = prpid;
                    prpid = prpid->next;
                    continue;
                }
            }

            if (prpid == dead_rpid )
                dead_rpid = prpid->next;
            else
                tmprpidp->next = prpid->next;
            break;
        } else {
            tmprpidp = prpid;
            prpid = prpid->next;
        }
    }
    if (prpid) {
        int taskid;

        *status = *((int *)&prpid->status);
        *rusage = prpid->ru;
        taskid = prpid->rpid;
        free((char *)prpid);
        sigprocmask(SIG_SETMASK, &oldMask, NULL);

        return (taskid);
    } else {


        if (tid == 0) {
            for (i = 0; i < lastConn; i ++) {
                if (conn[i].rpid != 0) {

                    sigprocmask(SIG_SETMASK, &oldMask, NULL);
                    return(0);
                }
            }
        } else {
            if ((i = connIndexTable[tid-1]) >= 0 && i < lastConn) {
                if (conn[i].rpid == tid) {
                    sigprocmask(SIG_SETMASK, &oldMask, NULL);
                    return(0);
                }
                if (getTask(conn[i].taskList, tid)) {
                    sigprocmask(SIG_SETMASK, &oldMask, NULL);
                    return(0);
                }
            }
        }
        lserrno = LSE_BAD_TID;
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return(-1);
    }
}

static int
do_newconnect(int s)
{
    static char          fname[] = "do_newconnect()";
    int                  newsock;
    int                  i;
    socklen_t            len;
    struct niosConnect   connReq;
    rtaskInfo_t          *task;
    time_t               rtime;
    char                 *hostname;
    struct sockaddr_in   sin;

    if (nioDebug)
        ls_syslog(LOG_DEBUG, "%s: accept new connection", fname);

    if ((newsock = doAcceptResCallback_(s, &connReq)) == -1)
        return (-1);

    len = sizeof(sin);
    if (getpeername(newsock, (struct sockaddr *) &sin, &len) <0) {
        close(newsock);
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }
    if ((hostname = (char *) malloc(MAXHOSTNAMELEN*sizeof(char))) == NULL) {
        lserrno = LSE_MALLOC;
        return -1;
    }
    strcpy(hostname, (const char*)inet_ntoa(sin.sin_addr));

    if (connReq.rpid <= 0) {
        lserrno = LSE_BAD_TID;
        return(-1);
    }
    if (connReq.rpid > maxtasks) {
        lserrno = LSE_TOOMANYTASK;
        return(-1);
    }

    i = connIndexTable[connReq.rpid-1];
    if (i >= 0 && i < lastConn) {

        if (conn[i].rpid == connReq.rpid && conn[i].sock.fd != -1)

            return(0);
        rtime = 0;
    }
    else {
        if ((i = getFirstFreeIndex()) >= maxfds) {
            lserrno = LSE_TOOMANYTASK;
            return(-1);
        }
        rtime = time(0);
    }

    while (!LIST_IS_EMPTY(conn[i].taskList))
        removeTask(conn[i].taskList,
                   (rtaskInfo_t*) conn[i].taskList->forw);
    if (conn[i].rbuf != NULL)
        FREEUP(conn[i].rbuf);
    conn[i].rbuf = (RelayLineBuf *) malloc(sizeof(RelayLineBuf));
    if (conn[i].rbuf == NULL) {
        lserrno = LSE_MALLOC;
        return (-1);
    }
    conn[i].rbuf->bcount = 0;
    conn[i].rtag = -1;
    conn[i].wtag = 0;
    if ((task = addTask(conn[i].taskList)) == NULL)
        return (-1);
    task->tid = connReq.rpid;
    task->rtime = rtime;
    task->eof = FALSE;
    task->dead = FALSE;
    conn[i].rpid = connReq.rpid;
    conn[i].rtime = rtime;
    conn[i].sock.fd = newsock;
    if (conn[i].hostname != NULL)
        FREEUP(conn[i].hostname);
    conn[i].hostname = hostname;
    conn[i].sock.rcount = 0;
    conn[i].sock.wcount = 0;
    conn[i].eof = FALSE;
    conn[i].dead = FALSE;
    connIndexTable[connReq.rpid-1] = i;
    if (rtime == 0) {
        if (FD_ISSET(i, &ncon_bit))
            FD_SET(i, &socks_bit);
        else
            FD_CLR(i, &socks_bit);
        FD_CLR(i, &ncon_bit);
        if (io_nonblock_(newsock) < 0) {
            lserrno = LSE_SOCK_SYS;
            return -1;
        }
        count_unconn--;
        if (nioDebug)
            ls_syslog(LOG_DEBUG, "\
%s: completed the leading task's connection: rpid=%d newsock=%d",
                      fname, connReq.rpid, newsock);
    }
    else {

        FD_SET(i, &ncon_bit);
        count_unconn++;
        if (i == lastConn)
            lastConn++;
        if (nioDebug)
            ls_syslog(LOG_DEBUG, "\
%s: received the leading task's request: rpid=%d newsock=%d",
                      fname, connReq.rpid, newsock);
    }
    return (0);

}

int
doAcceptResCallback_(int s, struct niosConnect *connReq)
{
    static char          fname[] = "doAcceptResCallback_()";
    struct sockaddr_in   from;
    socklen_t            fromlen;
    int                  newsock;
    struct LSFHeader     msgHdr;
    XDR                  xdrs;
    char                 buf[MSGSIZE];

    fromlen = sizeof (from);
    newsock = b_accept_(s, (struct sockaddr *)&from, &fromlen);
    if (newsock < 0) {
        if (nioDebug)
            ls_syslog(LOG_DEBUG, "%s: Accept error %d on socket %d",
                      fname, errno, s);
        lserrno = LSE_ACCEPT_SYS;
        return(-1);
    }

    xdrmem_create(&xdrs, buf, MSGSIZE, XDR_DECODE);
    if (readDecodeHdr_(newsock,
                       buf,
                       NB_SOCK_READ_FIX,
                       &xdrs,
                       &msgHdr) < 0) {
        close(newsock);
        return(-1);
    }

    XDR_SETPOS(&xdrs, 0);

    if (readDecodeMsg_(newsock,
                       buf,
                       &msgHdr,
                       NB_SOCK_READ_FIX,
                       &xdrs,
                       (char *)connReq,
                       xdr_niosConnect,
                       NULL) < 0) {
        close(newsock);
        xdr_destroy(&xdrs);
        return(-1);
    }
    xdr_destroy(&xdrs);

    return (newsock);

}

static void
setMask(fd_set *rmask)
{
    int i;


    if (acceptSock)
        FD_SET(acceptSock, rmask);

    for (i = 0; i < lastConn ; i++) {
        if (C_CONNECTED(conn[i])) {
            FD_SET(conn[i].sock.fd, rmask);
        }
    }
}


static int
get_connect(int indx, struct LSFHeader *msgHdr)
{
    XDR xdrs;
    char buf[MSGSIZE];
    struct niosConnect connReq;

    xdrmem_create(&xdrs, buf, MSGSIZE, XDR_DECODE);

    if (readDecodeMsg_(conn[indx].sock.fd, buf, msgHdr, NB_SOCK_READ_FIX,
                       &xdrs, (char *) &connReq, xdr_niosConnect, NULL) < 0) {
        xdr_destroy(&xdrs);
        return -1;
    }

    xdr_destroy(&xdrs);

    return 0;
}


static int
get_status(int indx, struct LSFHeader *msgHdr, LS_WAIT_T *statusp)
{
    static char fname[] = "get_status()";
    LS_WAIT_T status;
    struct niosStatus st;
    char buf[MSGSIZE];
    XDR xdrs;
    rtaskInfo_t *task;

    xdrmem_create(&xdrs, buf, MSGSIZE, XDR_DECODE);

    if (readDecodeMsg_(conn[indx].sock.fd, buf, msgHdr, NB_SOCK_READ_FIX,
                       &xdrs, (char *) &st, xdr_niosStatus, NULL) < 0) {

        xdr_destroy(&xdrs);

        memset((void *)&status, 0, sizeof(LS_WAIT_T));
        SETTERMSIG(status, STATUS_IOERR);
        if (bury_task(status, 0, conn[indx].rpid) == -1)
            return(LSE_MALLOC);
        return(LSE_BAD_XDR);
    }

    xdr_destroy(&xdrs);

    if (nioDebug > 1)
        ls_syslog(LOG_DEBUG, "%s: get_status: ack=%d",
                  fname, st.ack);

    if (st.ack != RESE_SIGCHLD) {

        memset((void *)&status, 0, sizeof(LS_WAIT_T));
        switch (st.ack) {
            case RESE_NOMEM:
                SETTERMSIG(status, STATUS_REX_NOMEM);
                break;
            case RESE_FATAL:
                SETTERMSIG(status, STATUS_REX_FATAL);
                break;
            case RESE_CWD:
                SETTERMSIG(status, STATUS_REX_CWD);
                break;
            case RESE_PTYMASTER:
            case RESE_PTYSLAVE:
                SETTERMSIG(status, STATUS_REX_PTY);
                break;
            case RESE_SOCKETPAIR:
                SETTERMSIG(status, STATUS_REX_SP);
                break;
            case RESE_FORK:
                SETTERMSIG(status, STATUS_REX_FORK);
                break;
            case RESE_NOVCL:
                SETTERMSIG(status, STATUS_REX_NOVCL);
                break;
            case RESE_NOSYM:
                SETTERMSIG(status, STATUS_REX_NOSYM);
                break;
            case RESE_VCL_INIT:
                SETTERMSIG(status, STATUS_REX_VCL_INIT);
                break;
            case RESE_VCL_SPAWN:
                SETTERMSIG(status, STATUS_REX_VCL_SPAWN);
                break;
            case RESE_EXEC:
                SETTERMSIG(status, STATUS_REX_EXEC);
                break;
            case RESE_MLS_INVALID:
                SETTERMSIG(status, STATUS_REX_MLS_INVAL);
                break;
            case RESE_MLS_CLEARANCE:
                SETTERMSIG(status, STATUS_REX_MLS_CLEAR);
                break;
            case RESE_MLS_RHOST:
                SETTERMSIG(status, STATUS_REX_MLS_RHOST);
                break;
            case RESE_MLS_DOMINATE:
                SETTERMSIG(status, STATUS_REX_MLS_DOMIN);
                break;
            default:

                SETTERMSIG(status, STATUS_REX_UNKNOWN);
                break;
        }

        if (bury_task(status, 0, conn[indx].rtag) == -1)
            return(LSE_MALLOC);
        if ((task = getTask(conn[indx].taskList, conn[indx].rtag)) != NULL)
            task->dead = TRUE;
        *statusp = status;
        return(0);
    }


    memcpy((char *)&status, (char *)&st.s.ss, sizeof(LS_WAIT_T));
    if (LS_WIFSIGNALED(status))
        SETTERMSIG(status, sig_decode(LS_WTERMSIG(status)));
    else if (LS_WIFSTOPPED(status))
        SETSTOPSIG(status, sig_decode(LS_WSTOPSIG(status)));
    if (!LS_WIFSTOPPED(status)) {


        if (bury_task(status, &(st.s.ru), conn[indx].rtag) == -1)
            return(LSE_MALLOC);
        if ((task = getTask(conn[indx].taskList, conn[indx].rtag)) != NULL)
            task->dead = TRUE;
    }
    *statusp = status;
    return(0);
}

static int
bury_task(LS_WAIT_T status, struct rusage *ru, int rpid)
{
    Dead_rpid *prpid, *p1, *p2;
    sigset_t newMask, oldMask;
    int connIndex = connIndexTable[rpid-1];
    bool_t found;
    Dead_tid  *ptidList, *ptid;

    if (conn[connIndex].deadtid) {
        ptidList =  conn[connIndex].deadtid;
        found = FALSE;
        while ( !found && ptidList) {
            if ( ptidList->tid == rpid )
                found = TRUE;
            else
                ptidList = ptidList->next;
        }
        if (found)
            return 0;
    }

    if ((ptid = (Dead_tid *)malloc(sizeof(Dead_tid))) == NULL) {
        return(-1);
    }
    ptid->tid = rpid;
    ptid->next = conn[connIndex].deadtid;
    conn[connIndex].deadtid = ptid;


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);

    if ((prpid = (Dead_rpid *)malloc(sizeof(Dead_rpid))) == NULL) {
        return(-1);
    }
    prpid->rpid = rpid;
    prpid->status = status;
    if (ru){
        memcpy((char *)&(prpid->ru), (char *)ru,
               sizeof( struct rusage));
    } else {
        cleanRusage(&(prpid->ru));
    }
    prpid->next = NULL;


    p1 = p2 = dead_rpid;
    while (p1) {
        p2 = p1;
        p1 = p1->next;
    }
    if (p2)
        p2->next = prpid;
    else
        dead_rpid = prpid;

    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(0);
}

int
ls_niokill(int sigval)
{
    static char fname[] = "ls_niokill()";
    int retVal;
    sigset_t newMask, oldMask;
    extern int usepty;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);


#if defined(SIGWINCH) || defined(SIGWINDOW)
    if (sigval == SIGWINCH && usepty) {
        if (sendUpdatetty() < 0) {
            ls_syslog(LOG_ERR, I18N(5902, "\
%s: Unable to update tty information."), /* catgets 5902 */
                      fname);
        }
    }
#endif

    if (!writeBuf.empty) {

        struct sigbuf *sigbufp, *p1, *p2;

        sigbufp = (struct sigbuf *)malloc(sizeof(struct sigbuf));
        if (sigbufp == NULL) {
            lserrno = LSE_MALLOC;
            sigprocmask(SIG_SETMASK, &oldMask, NULL);
            return(-1);
        }
        sigbufp->sigval = sigval;
        sigbufp->next = NULL;


        p1 = p2 = sigBuf;
        while (p1) {
            p2 = p1;
            p1 = p1->next;
        }
        if (p2)
            p2->next = sigbufp;
        else
            sigBuf = sigbufp;
    } else {

        if ((retVal = flush_sigbuf()) != 0) {

            lserrno = retVal;
            sigprocmask(SIG_SETMASK, &oldMask, NULL);
            return(-1);
        }
        if ((retVal = deliver_signal(sigval)) != 0) {
            lserrno = retVal;
            sigprocmask(SIG_SETMASK, &oldMask, NULL);
            return(-1);
        }
    }
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(0);
}

static int
flush_sigbuf()
{
    int retVal;
    struct sigbuf *p1;
    sigset_t newMask, oldMask;


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);

    if (writeBuf.empty) {
        while (sigBuf) {
            p1 = sigBuf;
            sigBuf = sigBuf->next;
            retVal = deliver_signal(p1->sigval);
            free(p1);
            if (retVal != 0) {
                sigprocmask(SIG_SETMASK, &oldMask, NULL);
                return(retVal);
            }
        }
    }
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(0);
}

static int
deliver_signal(int sigval)
{
    struct resSignal sig;
    struct {
        struct LSFHeader hdr;
        struct resSignal sig;
    } reqBuf;

    struct LSFHeader reqHdr;
    XDR xdrs;
    int i;
    LS_WAIT_T status;
    sigset_t newMask, oldMask;




    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);

    initLSFHeader_(&reqHdr);
    reqHdr.opCode = NIOS2RES_SIGNAL;
    reqHdr.reserved0 = 0;
    sig.pid = getpid();
    sig.sigval = sig_encode(sigval);

    xdrmem_create(&xdrs, (char *) &reqBuf, sizeof(reqBuf), XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs, (char *) &sig, &reqHdr, xdr_resSignal,0,NULL)){
        xdr_destroy(&xdrs);
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return(LSE_BAD_XDR);
    }

    for (i = 0 ; i < lastConn ; i++) {

        if (C_CONNECTED(conn[i])) {
            if (NB_SOCK_WRITE_FIX(conn[i].sock.fd, (char *) &reqBuf,
                                  XDR_GETPOS(&xdrs)) < 0 && errno != EPIPE) {
                memset((void *)&status, 0, sizeof(LS_WAIT_T));
                SETTERMSIG(status, STATUS_IOERR);
                if (bury_task(status, 0, conn[i].rpid) == -1) {
                    sigprocmask(SIG_SETMASK, &oldMask, NULL);
                    return(LSE_MALLOC);
                }
                add_list(&abortedTasks, conn[i].rpid, NIO_IOERR, NULL);
                add_list(&abortedTasks, conn[i].rpid, NIO_EOF, NULL);
                DISCONNECT(i);
            }
        }
    }
    xdr_destroy(&xdrs);
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(0);
}




bool_t
compareTaskId(rtaskInfo_t *task, int *tid, int hint)
{
    if (task->tid == *tid)
        return TRUE;
    else
        return FALSE;
}

static int
readResMsg(int connIndex)
{
    static char fname[] = "readResMsg()";
    int cc;
    LS_WAIT_T status;
    int retry_count;

    if (!conn[connIndex].sock.rcount) {
        lserrno = LSE_NO_ERR;
        return(0);
    }

    retry_count = 1;
READ_RETRY:
    if ((cc = read(conn[connIndex].sock.fd, conn[connIndex].rbuf->bp,
                   conn[connIndex].sock.rcount)) <= 0) {
        int sverrno = errno;
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "read");
        if (cc == 0 || BAD_IO_ERR(errno)) {
            memset((void *)&status, 0, sizeof(LS_WAIT_T));
            SETTERMSIG(status, STATUS_IOERR);
            if (bury_task(status, 0, conn[connIndex].rpid) == -1) {
                lserrno = LSE_MALLOC;
                return(-1);
            }
            add_list(&abortedTasks, conn[connIndex].rpid, NIO_IOERR, NULL);
            add_list(&abortedTasks, conn[connIndex].rpid, NIO_EOF, NULL);
            DISCONNECT(connIndex);
        }
        if (errno == EAGAIN && retry_count < MAX_READ_RETRY) {

            retry_count++;
            sleep(1);
            goto READ_RETRY;
        }
        errno = sverrno;
        lserrno = LSE_MSG_SYS;
        return(-1);
    }
    conn[connIndex].sock.rcount -= cc;
    conn[connIndex].rbuf->bcount += cc;
    conn[connIndex].rbuf->bp += cc;

    return(cc);
}

int
ls_niodump(LS_HANDLE_T outputFile, int tid, int options,
           char *taggingFormat)
{
    static char fname[] = "ls_niodump()";
    int index, cc = 0, len;
    int tagLen = 0;
    int nc = 0;
    char tagStr[128], *p, *endbuf;
    static int errDisplayflag = 0;
    static int previousTag = -1;
    static int hasNewLine = 1;
    int manualNewLine = FALSE;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid <= 0) {
        lserrno = LSE_BAD_TID;
        return -1;
    }


    if (tid > maxtasks) {
        lserrno = LSE_BAD_TID;
        return -1;
    }

    index = connIndexTable[tid-1];
    if (index < 0 || index >= lastConn) {
        lserrno = LSE_BAD_TID;
        return(-1);
    }

    if (conn[index].sock.rcount > 0 || conn[index].rbuf->bcount == 0) {
        lserrno = LSE_NO_ERR;
        return(0);
    }


    if (options == 1 && conn[index].rtag > 0) {
        if (conn[index].rtag != previousTag && !hasNewLine) {
            strcpy(tagStr, "\n");
            cc = tagLen = strlen(tagStr);
            cc = b_write_fix(outputFile, tagStr, tagLen);
            if (cc != tagLen || (cc < 0 && BAD_IO_ERR(errno))) {
                if (!errDisplayflag) {
                    errDisplayflag = 1;
                    ls_syslog(LOG_ERR, I18N(5904, "\
%s: error writing remote output"), /* catgets 5904 */
                              fname);
                    lserrno = LSE_MISC_SYS;
                    return (-1);
                }
            } else {
                errDisplayflag = 0;
            }
        }
        nc += cc;
        tagLen = 0;
        previousTag = conn[index].rtag;
        if (taggingFormat) {
            sprintf(tagStr, "R%d: ", conn[index].rtag-1);
            tagLen = strlen(tagStr);
        }
        if (conn[index].rbuf->buf[conn[index].rbuf->bcount-1] == '\n')
            hasNewLine = 1;
        else
            hasNewLine = 0;
    } else if ( taggingFormat && conn[index].rtag > 0) {
        sprintf(tagStr, "R%d: ", conn[index].rtag-1);
        tagLen = strlen(tagStr);
    }


    conn[index].rbuf->bp = conn[index].rbuf->buf;
    endbuf = conn[index].rbuf->bp + conn[index].rbuf->bcount;

    while (conn[index].rbuf->bcount > 0) {
        len = conn[index].rbuf->bcount;
        if (tagLen > 0) {
            cc = tagLen;
            cc = b_write_fix(outputFile, tagStr, tagLen);
            if (cc != tagLen || (cc < 0 && BAD_IO_ERR(errno))) {
                if (!errDisplayflag) {
                    errDisplayflag = 1;
                    ls_syslog(LOG_ERR, I18N(5904, "\
%s: error writing remote output"), /* catgets 5904 */
                              fname);
                    lserrno = LSE_MISC_SYS;
                    return (-1);
                }
            } else {
                errDisplayflag = 0;
            }
            nc += cc;


            p = conn[index].rbuf->bp;
            do {
                if (*p == '\n')
                    break;
            } while (++p < endbuf);
            if (p < endbuf) {
                len = (int) (p + 1 - conn[index].rbuf->bp);
                hasNewLine = 1;
            }
        }


        if (options == 2) {

            manualNewLine = FALSE;
            p = conn[index].rbuf->bp;
            do {
                if (*p == '\n') {
                    break;
                }
            } while (++p < endbuf);
            if (p < endbuf) {
                len = (int) (p - conn[index].rbuf->bp);
                manualNewLine = TRUE;
            }
        }


        cc = len;
        cc = b_write_fix(outputFile, conn[index].rbuf->bp, len);
        if (cc != len || (cc < 0 && BAD_IO_ERR(errno))) {
            if (!errDisplayflag) {
                errDisplayflag = 1;
                ls_syslog(LOG_ERR, I18N(5904, "\
%s: error writing remote output"), /* catgets 5904 */
                          fname);
                lserrno = LSE_MISC_SYS;
                return (-1);
            }
        } else {
            errDisplayflag = 0;
        }

        if (options == 2 && manualNewLine == TRUE) {

            int wrote = 0;
            wrote = b_write_fix(outputFile, "\r\n", 2);
            if (wrote != 2 || (wrote < 0 && BAD_IO_ERR(errno))) {
                if (!errDisplayflag) {
                    errDisplayflag = 1;
                    ls_syslog(LOG_ERR, I18N(5904, "\
%s: error writing remote output"), /* catgets 5904 */
                              fname);
                    lserrno = LSE_MISC_SYS;
                    return (-1);
                }
            } else {
                errDisplayflag = 0;
            }

            nc += cc + 1;
            conn[index].rbuf->bcount -= cc + 1;
            conn[index].rbuf->bp += cc + 1;
        } else {

            nc += cc;
            conn[index].rbuf->bcount -= cc;
            conn[index].rbuf->bp += cc;
        }
    }
    return nc;
}

static rtaskInfo_t *
getTask(LIST_T *taskList, int tid)
{
    rtaskInfo_t *task;

    if (!taskList || LIST_IS_EMPTY(taskList) || taskList->numEnts <= 0) {
        lserrno = LSE_BAD_ARGS;
        return NULL;
    }
    task = (rtaskInfo_t *) listSearchEntry(taskList, (void *) &tid,
                                           (LIST_ENTRY_EQUALITY_OP_T) compareTaskId, 0);
    if (task == NULL)
        lserrno = LSE_BAD_TID;

    return task;
}

static rtaskInfo_t *
addTask(LIST_T *taskList)
{
    rtaskInfo_t *task;
    if (taskList == NULL) {
        lserrno = LSE_BAD_ARGS;
        return NULL;
    }
    if ((task = (rtaskInfo_t *) malloc(sizeof(rtaskInfo_t))) == NULL) {
        lserrno = LSE_MALLOC;
        return NULL;
    }
    if (listInsertEntryAtBack(taskList, (LIST_ENTRY_T *) task) < 0) {
        FREEUP(task);
        lserrno = LSE_INTERNAL;
        return NULL;
    }
    return task;
}

static void
removeTask(LIST_T *taskList, rtaskInfo_t *task)
{
    if (!taskList || !task || LIST_IS_EMPTY(taskList)
        || taskList->numEnts <= 0) {
        lserrno = LSE_BAD_ARGS;
        return;
    }
    listRemoveEntry(taskList, (LIST_ENTRY_T *) task);
    FREEUP(task);
}

static int
addNotifyList(LIST_T *list, int tid, int opCode)
{
    taskNotice_t *notice;
    if (!list || tid <= 0 || tid > maxtasks) {
        lserrno = LSE_BAD_ARGS;
        return (-1);
    }

    if (opCode != STATUS_TIMEOUT) {
        lserrno = LSE_BAD_OPCODE;
        return (-1);
    }

    if ((notice = (taskNotice_t *) malloc(sizeof(taskNotice_t))) == NULL) {
        lserrno = LSE_MALLOC;
        return (-1);
    }
    notice->tid = tid;
    notice->opCode = opCode;

    if (listInsertEntryAtBack(list, (LIST_ENTRY_T *) notice) < 0) {
        FREEUP(notice);
        lserrno = LSE_INTERNAL;
        return (-1);
    }
    return 0;
}

static int
addTaskList(int tid, int connIndex)
{
    static char fname[] = "addTaskList()";
    int i;
    time_t rtime;
    rtaskInfo_t *task;
    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return -1;
    }
    if (tid <=0) {
        lserrno = LSE_BAD_TID;
        return -1;
    }
    if (tid > maxtasks) {
        lserrno = LSE_TOOMANYTASK;
        return(-1);
    }


    if (connIndex < 0 || connIndex >= lastConn) {
        lserrno = LSE_BAD_ARGS;
        return -1;
    }


    if (conn[connIndex].rpid == 0 || conn[connIndex].sock.fd == -1) {
        lserrno = LSE_BAD_ARGS;
        return -1;
    }


    i = connIndexTable[tid-1];
    if (i >= 0 && i < lastConn && conn[i].rpid == tid && conn[i].sock.fd == -1
        && i != connIndex) {
        conn[i].sock.fd = -1;
        conn[i].rpid = 0;
        conn[i].rtime = 0;
        FD_CLR(i, &ncon_bit);
        count_unconn--;
        rtime = 0;
        connIndexTable[tid-1] = -1;
    }
    else
        rtime = time(0);


    if ((task = getTask(conn[connIndex].taskList, tid)) != NULL)

        return 0;

    if ((task = addTask(conn[connIndex].taskList)) == NULL)
        return (-1);
    connIndexTable[tid-1] = connIndex;
    task->tid = tid;
    task->eof = FALSE;
    task->dead = FALSE;
    task->rtime = rtime;
    if (nioDebug) {
        if (rtime > 0)
            ls_syslog(LOG_DEBUG, "%s: received new task's request: rpid=%d",
                      fname, tid);
        else
            ls_syslog(LOG_DEBUG,
                      "%s: completed new task's connection: rpid=%d",
                      fname, tid);
    }
    return 0;
}

static int
notify_task(int tid, int opCode)
{
    static char fname[] = "notify_task()";
    struct LSFHeader reqHdr, buf;
    XDR xdrs;
    LS_WAIT_T status;
    sigset_t newMask, oldMask;
    int len, connIndex;

    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return (-1);
    }
    if (tid <= 0 || tid > maxtasks) {
        lserrno = LSE_BAD_TID;
        return (-1);
    }
    if ((connIndex = connIndexTable[tid-1]) < 0 || connIndex >= lastConn) {
        lserrno = LSE_BAD_ARGS;
        return (-1);
    }

    if (!writeBuf.empty) {
        lserrno = LSE_NO_ERR;
        return (-1);
    }


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);

    reqHdr.opCode = opCode;
    reqHdr.version = VOLCLAVA_VERSION;
    reqHdr.length = 0;
    reqHdr.reserved = tid;

    if (conn[connIndex].rpid > 0 && conn[connIndex].sock.fd != -1) {
        xdrmem_create(&xdrs, (char *) &buf, sizeof(struct LSFHeader),
                      XDR_ENCODE);
        if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
            xdr_destroy(&xdrs);
            lserrno = LSE_BAD_XDR;
            sigprocmask(SIG_SETMASK, &oldMask, NULL);
            return(-1);
        }
        len = XDR_GETPOS(&xdrs);
        xdr_destroy(&xdrs);

        if (NB_SOCK_WRITE_FIX(conn[connIndex].sock.fd, (char *)&buf, len) < 0
            && errno != EPIPE) {
            memset((void *)&status, 0, sizeof(LS_WAIT_T));
            SETTERMSIG(status, STATUS_IOERR);
            if (bury_task(status, 0, conn[connIndex].rpid) == -1) {
                lserrno = LSE_MALLOC;
                sigprocmask(SIG_SETMASK, &oldMask, NULL);
                return(-1);
            }
            ls_syslog(LOG_ERR, I18N(5905,
                                    "%s: Error writing EOF to task %d"), /* catgets 5905 */
                      fname, conn[connIndex].rpid);
            add_list(&abortedTasks, conn[connIndex].rpid, NIO_IOERR, NULL);
            add_list(&abortedTasks, conn[connIndex].rpid, NIO_EOF, NULL);
            DISCONNECT(connIndex);
        }
        else if (nioDebug)
            ls_syslog(LOG_DEBUG,
                      "%s: remote task <%d> has been notified: opCode=%d",
                      fname, tid, opCode);
    }
    else {
        lserrno = LSE_BAD_CHAN;
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        return(-1);
    }
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(0);
}

static int
getFirstFreeIndex(void)
{
    int i;
    if (conn == NULL) {
        lserrno = LSE_NIO_INIT;
        return (-1);
    }
    for (i=0; i<lastConn; i++) {
        if (conn[i].rpid == 0)
            return i;
    }
    return i;
}

static int
sendUpdatetty() {
    static char fname[] = "sendUpdatetty";
    int i, iofd, redirect;
    char buf[MSGSIZE];
#ifndef TIOCGWINSZ
    char *cp;
#endif
    static struct resStty tty;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG, "%s: Entering", fname);
    }


    redirect = 0;
    if (isatty(0)) {
        if (!isatty(1))
            redirect = 1;
        iofd = 0;
    } else if (isatty(1)) {
        redirect = 1;
        iofd = 1;
    } else {
        ls_syslog(LOG_ERR, I18N(5906, "\
%s: usepty specified but TTY not detected"), /* catgets 5906 */
                  fname);
        return(-1);
    }


    tcgetattr(iofd, &tty.termattr);

    if (getpgrp(0) != tcgetpgrp(iofd)) {
        tty.termattr.c_cc[VEOF] = 04;
        tty.termattr.c_lflag |= ICANON;
        tty.termattr.c_lflag |= ECHO;
    }

    if (redirect)
        tty.termattr.c_lflag &= ~ECHO;


#ifdef TIOCGWINSZ
    ioctl(iofd, TIOCGWINSZ, (char *)&tty.ws);
#else
    if ((cp = getenv("LINES")) != NULL)
        tty.ws.ws_row = atoi(cp);
    else
        tty.ws.ws_row = 24;
    if ((cp = getenv("COLUMNS")) != NULL)
        tty.ws.ws_col = atoi(cp);
    else
        tty.ws.ws_col = 80;
    tty.ws.ws_xpixel = tty.ws.ws_ypixel = 0;
#endif


    for (i=0; i<lastConn; i++) {
        if (conn[i].sock.fd > 0) {
            if (callRes_(conn[i].sock.fd, NIOS2RES_SETTTY, (char *) &tty,
                         buf, MSGSIZE,
                         xdr_resStty, 0, 0, NULL) == -1) {
                ls_syslog(LOG_ERR, I18N(5907, "\
%s: Error: could not connect to %d"), /* catgets 5907 */
                          fname, i);
                return (-1);
            }
        }
    }

    return(0);

}



void
checkHeartbeat(int nready)
{
    static char fname[] = "checkHeartbeat()";
    time_t now;


    now = time(0);
    if ( nready == 0) {

        if ( lastCheckTime + heartbeatInterval <= now) {

            int cc;
            cc = sendHeartbeat();
            if ( cc == 0 ) {

                if (nioDebug) {
                    ls_syslog(LOG_DEBUG, "\
%s: Nios sends NIOS2RES_HEARTBEAT okay", fname);
                }
            } else {

                if (nioDebug) {
                    ls_syslog(LOG_DEBUG, "\
%s: Nios fails sending NIOS2RES_HEARTBEAT", fname);
                }
            }
            lastCheckTime = now;
        }
    } else {

        lastCheckTime = now;
    }
    return;
}


static int
sendHeartbeat(void)
{
    static char fname[] = "sendHeartbeat()";
    struct LSFHeader reqHdr, buf;
    XDR xdrs;
    sigset_t newMask, oldMask;
    int len, cc=0;


    sigemptyset(&oldMask);
    sigfillset(&newMask);
    sigprocmask(SIG_BLOCK, &newMask, &oldMask);


    reqHdr.opCode = NIOS2RES_HEARTBEAT;
    reqHdr.version = VOLCLAVA_VERSION;
    reqHdr.length = 0;
    reqHdr.reserved0 = 0;

    xdrmem_create(&xdrs, (char *) &buf, sizeof(struct LSFHeader),
                  XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs, &reqHdr)) {
        xdr_destroy(&xdrs);
        lserrno = LSE_BAD_XDR;
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
        cc = -1;
    } else {
        len = XDR_GETPOS(&xdrs);
        xdr_destroy(&xdrs);

        if (C_CONNECTED(conn[0])) {
            if (NB_SOCK_WRITE_FIX(conn[0].sock.fd, (char *)&buf, len) < 0) {
                ls_syslog(LOG_ERR, I18N(5908, "\
%s: Error sending heartbeat to host <%s>"), /* catgets 5908 */
                          fname, conn[0].hostname);
                cc = -2;
            }
        }
    }

    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return(cc);
}



void
checkJobStatus(int numTries)
{
    static char fname[] = "checkJobStatus()";
    struct jobInfoEnt* job=NULL;
    struct jobInfoHead* jobHead=NULL;
    int jobGone = FALSE;
    JOB_STATUS jobStatus;
    int count = 0;

    if (nioDebug) {
        ls_syslog(LOG_DEBUG, "%s: Nios checking job status", fname);
    }


    while( (numTries == -1) || (count < numTries) ) {

        jobStatus = getJobStatus(jobId, &job, &jobHead);
        if (jobStatus == JOB_STATUS_FINISH) {
            jobGone = TRUE;
        } else if ( jobStatus == JOB_STATUS_KNOWN && job != NULL) {
            if (IS_FINISH(job->status)) {

                jobGone = TRUE;
            }
        }

        if (jobGone) {

            if (job != NULL) {

                LS_WAIT_T wStatus;
                LS_STATUS(wStatus) = job->exitStatus;


                if (nioDebug) {
                    ls_syslog(LOG_DEBUG, "\
%s: Nios job has exited from LSF system exitStatus<%d>",
                              fname, WEXITSTATUS(wStatus));
                }
                kill_self(0, WEXITSTATUS(wStatus));
            } else {

                fprintf(stderr, I18N(901,
                                     "<<<Job gone from LSF system>>>\r\n")); /* catgets 901 */
                kill_self(0, -11);
            }
        }


        count++;
        if ( (numTries == -1) || (count < numTries) ) {

            sleep(jobStatusInterval);
        }
    }

    return;
}


void checkPendingJobStatus(int s)
{
    static char fname[] = "checkPendingJobStatus()";
    int       ready;
    int       lastPendJobCheck;
    int       lastMsgCheck;

    if (nioDebug) {
        ls_syslog(LOG_DEBUG, "\
%s: Nios pendJobTimeout=%dmin jobStatusInterval=%dmin msgInterval=%dmin.",
                  fname, pendJobTimeout/60, jobStatusInterval/60,
                  msgInterval/60);
    }

    lastMsgCheck = lastPendJobCheck = lastCheckTime = time(NULL);


    ready = 0;
    while (ready == 0) {
        struct timeval      selectTimeout;

        selectTimeout.tv_sec =  60;
        selectTimeout.tv_usec = 0;


        ready = rd_poll_(s, &selectTimeout);
        if ( (ready == 0)
             && (jobId > 0) ) {
            time_t              now;


            now = time(NULL);


            if (pendJobTimeout > 0
                && (now - lastPendJobCheck) >= pendJobTimeout) {
                ls_syslog(LOG_INFO, "\
%s: Nios pending job timeout %dmin  expired, killing the job\n",
                          fname, pendJobTimeout/60);

                if (getenv("LSF_NIOS_DIE_CMD")) {
                    execl("/bin/sh",
                          "/bin/sh",
                          "-c",
                          getenv("LSF_NIOS_DIE_CMD"), NULL);
                    perror("/bin/sh");
                    exit(-10);
                }
            }


            if (jobStatusInterval> 0
                && (now - lastCheckTime) >= jobStatusInterval) {
                checkJobStatus(1);
                lastCheckTime = now;
            }


            if (msgInterval > 0
                && (now - lastMsgCheck) >= msgInterval) {
                JobStateInfo(jobId);
                lastMsgCheck = now;
            }
        }
    }
    return;
}

JOB_STATUS
getJobStatus(LS_LONG_INT jid, struct jobInfoEnt **job, struct jobInfoHead **jobHead)
{
    static char fname[] = "getJobStatus()";
    struct jobInfoEnt* jobInfo=NULL;
    struct jobInfoHead* jobInfoHead = NULL;
    JOB_STATUS retval = JOB_STATUS_UNKNOWN;

    if ( jid <= 0 ) {
        return JOB_STATUS_ERR;
    }


    if (lsb_init("nios") != 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "lsb_init");
        kill_self(0, -1);
    }


    ls_initdebug("nios");


    putEnv("LSB_NTRIES", "1");


    jobInfoHead = lsb_openjobinfo_a(jid, NULL, "all",
                                    NULL, NULL, ALL_JOB );
    if (jobInfoHead == NULL ) {

        if (lsberrno == LSBE_NO_JOB) {

            if (nioDebug) {
                ls_syslog(LOG_DEBUG,
                          "%s: Nios job<%s> - unknown by MBD",
                          fname, lsb_jobid2str(jid));
            }
            retval = JOB_STATUS_FINISH;
        } else {

            if (nioDebug) {
                ls_syslog(LOG_DEBUG,
                          "%s: Nios job<%s> - no response from MBD",
                          fname, lsb_jobid2str(jid));
            }
            retval = JOB_STATUS_UNKNOWN;
        }
    } else {

        jobInfo = lsb_readjobinfo(NULL);
        if ( jobInfo == NULL ) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,
                      fname, "lsb_readjobinfo");
            retval = JOB_STATUS_UNKNOWN;
        } else {

            if (job) {
                *job     = jobInfo;
            }
            if ( jobHead) {
                *jobHead = jobInfoHead;
            }
            retval  = JOB_STATUS_KNOWN;

            if (nioDebug) {
                ls_syslog(LOG_DEBUG,
                          "%s: Nios job<%s> status<0x%x>",
                          fname,
                          lsb_jobid2str(jobInfo->jobId),
                          jobInfo->status);
            }
        }
    }


    lsb_closejobinfo();

    return (retval);
}


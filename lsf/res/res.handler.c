/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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
#include <ctype.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <grp.h>
#include <stdlib.h>
#include "res.h"
#include "resout.h"
#include "../lib/lproto.h"
#include "../lib/mls.h"

#define NL_SETN         29
#define CHILD_DELETED     2

void child_channel_clear(struct child *, outputChannel *);
static void declare_eof_condition(struct child *, int);
static void simu_eof(struct child *, int);
static void resSetenv(struct client *, struct LSFHeader *, XDR *);
static void doReopen(void);
static void resDebugReq(struct client *, struct LSFHeader *, XDR *,int);
static void resChdir(struct client *, struct LSFHeader *, XDR *);
static void resStty(struct client *, struct LSFHeader *, XDR *, int);
static void resRKill(struct client *, struct LSFHeader *, XDR *);
static void resGetpid(struct client *, struct LSFHeader *, XDR *);
static void resRusage(struct client *, struct LSFHeader *, XDR *);
static void resControl(struct client *, struct LSFHeader *, XDR *, int);
static void resRexec(struct client *, struct LSFHeader *, XDR *);
static void resTaskMsg(struct client *, struct LSFHeader *, char *, char *, XDR *);
static int forwardTaskMsg(int, int, struct LSFHeader *, char *, char *,bool_t,int);
static struct child *doRexec(struct client *, struct resCmdBill *, int, int,
                             int, resAck *);
static void rexecChild(struct client *, struct resCmdBill *, int, int, int *,
                       int *, int *, int *, int, int *);
static resAck childPty(struct client *, int *, int *, char *, int);
static resAck parentPty(int *pty, int *sv, char *);
static int forkPty(struct client *,int *, int *, int *, int *, char *, resAck *, int, int);
static int forkSV(struct client *, int *, int *, int *, resAck *);
static void execit(char **uargv, char *, int *, int, int, int);
static void lsbExecChild(struct resCmdBill *cmdmsg, int *pty, int *sv,
                         int *err, int *info, int *pid);

static void delete_client(struct client *);
static int unlink_child(struct child *);
static void kill_child(struct child *);
static int notify_client(int, int, resAck, struct sigStatusUsage *);
static void eof_to_nios(struct child *);
static void eof_to_client(struct child *);
static void setptymode(ttyStruct *, int);
static void freeblk(char **);
static char **copyArray(char **);

static int notify_sigchild(struct child *);
static int pairsocket(int, int, int, int [2]);

static int recvConnect(int, struct resConnect *, int (*)(), struct lsfAuth *);
static int setClUid(struct client *cli_ptr);
static int checkPermResCtrl(struct client *);

static int ttyCallback(int, ttyStruct *);
static void setPGid(struct client *cli_ptr, int tflag);

static uid_t setEUid(uid_t uid);
static int changeEUid (uid_t uid);

static int notify_nios(int, int, int);
static int addResNotifyList(LIST_T *, int, int, resAck,
                            struct sigStatusUsage *);

int matchExitVal(int, char *);

extern ttyStruct defaultTty;
extern int initLimSock_(void);

static short is_resumed = FALSE;

#define SET_RLIMIT(rlim, limit,loglevel)                                \
    if (setrlimit(rlim, &limit) < 0 && getuid() == 0) {                 \
        if (loglevel == LOG_INFO && errno == EINVAL) {                  \
            ls_syslog(loglevel,_i18n_msg_get(ls_catd , NL_SETN, 5299,   \
                                             "setrlimit(Resource Limit %d) failed: %m: soft %f hard %f , may be larger than the kernel allowed limit\n"), \
                      rlim, (double)limit.rlim_cur, (double)limit.rlim_max); \
        }else{                                                          \
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5100,   \
                                             "setrlimit(Resouce Limit %d) failed: %m: soft %f hard %f infi=%f\n"), \
                      rlim, (double) limit.rlim_cur,                    \
                      (double) limit.rlim_max, (double) RLIM_INFINITY); \
        }                                                               \
    } /* catgets 5100 */

static void setlimits(struct lsfLimit *);
static void mysetlimits(struct lsfLimit *);
static int addCliEnv(struct client *, char *, char *);
static int setCliEnv(struct client *, char *, char *);
static int resUpdatetty(struct LSFHeader);

typedef enum {
    PTY_BAD,
    PTY_GOOD,
    PTY_NEW,
    PTY_NOMORE
} status_t;

struct hand_shake {
    status_t code;
    char buffer[MAXPATHLEN];
};

int currentRESSN;

extern char **environ;

static void dumpResCmdBill(struct resCmdBill*);

static bool_t    resKeepPid = FALSE;

static struct listSet   *pidSet     = NULL;

static void   cleanUpKeptPids(void);

void
doacceptconn(void)
{
    static char                  fname[] = "doacceptconn()";
    int                          s;
    struct  sockaddr_in          from;
    struct  sockaddr_in          local;
    static struct  sockaddr_in   localSave;
    static char                  first = TRUE;
    socklen_t                    localLen = sizeof(local);
    struct passwd                *pw;
    struct passwd                pwSave;
    char                         pwDir[MAXFILENAMELEN];
    char                         pwShell[MAXFILENAMELEN];
    char                         pwName[MAXLSFNAMELEN];
    struct hostent               *hostp;
    socklen_t                    fromlen;
    int                          i;
    int                          pid;
    int                          pfd[2];
    struct resConnect            connReq;
    struct lsfAuth               auth;
    struct linger                linstr = {1, 1};
    int                          cc;
    int                          crossPlatform;
    char                         *authKind;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Accepting connections on the main socket", fname);
    }

    crossPlatform = -1;

    if (client_cnt >= MAXCLIENTS_HIGHWATER_MARK) {

        ls_syslog(LOG_INFO, "\
%s: High water mark reached, client_cnt (%d),child_cnt(%d)",
                  fname, client_cnt, child_cnt);


        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pfd) < 0) {

            ls_syslog(LOG_ERR, "%s: socketoair9) failed %m", fname);
            fromlen = sizeof(struct sockaddr_in);
            s = accept(accept_sock, (struct sockaddr *)&from, &fromlen);
            if (s >= 0) {
                sendReturnCode(s,RESE_NOMORECONN);
                shutdown(s, 2);
                closesocket(s);
            }
            return;
        }

        pid = fork();
        if (pid < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
            fromlen = sizeof(struct sockaddr_in);
            s = accept(accept_sock, (struct sockaddr *)&from, &fromlen);

            if (s < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "accept");
                return;
            } else {
                sendReturnCode(s,RESE_NOMORECONN);
                shutdown(s, 2);
                close(s);
            }
            close(pfd[0]);
            close(pfd[1]);
            return;
        }

        if (pid == 0) {


            parent_res_port = pfd[0];

            for (i = sysconf(_SC_OPEN_MAX) ; i > 2 ; i--)
                if (i != accept_sock && i != parent_res_port )
                    close(i);
            while (client_cnt--) {
                freeblk(clients[client_cnt]->env);
                free(clients[client_cnt]->username);
                if (clients[client_cnt]->eexec.len > 0)
                    free(clients[client_cnt]->eexec.data);
                free(clients[client_cnt]->homedir);
                free(clients[client_cnt]->hostent.h_name);
                freeblk(clients[client_cnt]->hostent.h_aliases);
                free(clients[client_cnt]);
                clients[client_cnt] = NULL;
            }
            while (child_cnt--) {
                free(children[child_cnt]);
                children[child_cnt] = NULL;
            }
            client_cnt = child_cnt = 0;

            if (initLimSock_() < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "initLimSock_");
            }
            return;
        }



        allow_accept = 0;
        close(accept_sock);
        child_res = 1;
        child_go = 1;

        close(pfd[0]);
        child_res_port = pfd[1];
        return;
    }

    if (debug>1) {
        printf("[%d]res/doacceptconn\n", (int) getpid());
    }

    fromlen = sizeof(struct sockaddr_in);
    if ((s = accept(accept_sock, (struct sockaddr *)&from, &fromlen)) < 0) {
        if (errno != EWOULDBLOCK)
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "accept");
        return;
    }



    if (io_nonblock_(s) < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "io_nonblock_");

    if (recvConnect(s, &connReq, NB_SOCK_READ_FIX, &auth) == -1) {
        shutdown(s, 2);
        closesocket(s);
        return;
    }
    if (auth.options >= 0) {
        if (auth.options & AUTH_HOST_UX)
            crossPlatform = FALSE;
        else
            crossPlatform = TRUE;
    }
    ls_syslog(LOG_DEBUG, "%s: auth.options=%d, crossPlatform=%d: %m", fname,
              auth.options, crossPlatform);

    if (io_block_(s) < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "io_block_");

    setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&linstr, sizeof(linstr));

    if ((pw = getpwlsfuser_(auth.lsfUserName)) == NULL) {
        char tempBuffer[1024];
        sprintf(tempBuffer, "%s@%s", auth.lsfUserName, sockAdd2Str_(&from));
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "getpwnam", tempBuffer);
        sendReturnCode(s,RESE_BADUSER);
        goto doAcceptFail;
    }

    memcpy((char *) &pwSave, (char *) pw, sizeof(struct passwd));

    strcpy(pwDir, pw->pw_dir ? pw->pw_dir : "/tmp");
    pwSave.pw_dir = pwDir;
    strcpy(pwShell, pw->pw_shell ? pw->pw_shell : "/bin/sh");
    pwSave.pw_shell = pwShell;
    strcpy(pwName, pw->pw_name);
    pwSave.pw_name = pwName;
    pw = &pwSave;

    authKind = resParams[LSF_AUTH].paramValue;
    if (authKind != NULL) {
        if (strcmp(authKind, AUTH_PARAM_EAUTH)) authKind = NULL;
    }


    if (getsockname(s, (struct sockaddr *) &local, &localLen) == -1) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "getsockname");
        sendReturnCode(s, RESE_REQUEST);
        goto doAcceptFail;
    }

    if (first) {
        hostp = Gethostbyaddr_(&local.sin_addr.s_addr,
                               sizeof(in_addr_t),
                               AF_INET);
        if (hostp == NULL) {
            ls_syslog(LOG_ERR, "\
%s: gethostbyaddr() for %s failed", __func__,
                      sockAdd2Str_(&local));
            sendReturnCode(s, RESE_REQUEST);
            goto doAcceptFail;
        }

        first = FALSE;

        memcpy((char *)&localSave.sin_addr, (char *)hostp->h_addr,
               (int)hostp->h_length);
    }


    local.sin_addr.s_addr = localSave.sin_addr.s_addr;

    hostp = Gethostbyaddr_(&from.sin_addr.s_addr,
                           sizeof(in_addr_t),
                           AF_INET);
    if (hostp == NULL) {
        ls_syslog(LOG_ERR, "\
%s: gethostbyaddr() for %s failed", __func__,
                  sockAdd2Str_(&from));
        sendReturnCode(s, RESE_NOLSF_HOST);
        goto doAcceptFail;
    }

    if (hostOk(hostp->h_name, RECV_FROM_CLUSTERS) < 0) {
        ls_syslog(LOG_INFO, "\
%s: Received request from non-LSF host %s",
                  fname, hostp->h_name);
        sendReturnCode(s, RESE_NOLSF_HOST);
        goto doAcceptFail;
    }

    ls_syslog(LOG_DEBUG, "\
%s: received connecting request from host %s",
              fname, hostp->h_name);


    memcpy((char *) &from.sin_addr, (char *) hostp->h_addr,
           (int) hostp->h_length);


    putEauthClientEnvVar("user");
    putEauthServerEnvVar("res");

#ifdef INTER_DAEMON_AUTH

    {
        char *aux_file;

        aux_file = tempnam(NULL, ".auxr");
        if (aux_file) {
            putEauthAuxDataEnvVar(aux_file);
            free(aux_file);
        }
        else {

            char aux_file_buf[64];

            sprintf(aux_file_buf, "/tmp/.auxres_%lul", time(0));
            putEauthAuxDataEnvVar(aux_file_buf);
        }
        putenv("LSF_RES_REAL_UID=");
        putenv("LSF_RES_REAL_GID=");
    }
#endif

    if (!userok(s, &from, hostp->h_name, &local, &auth, debug)) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5113,
                                         "%s: Permission denied %s(%d)@%s"), /* catgets 5113 */
                  fname, auth.lsfUserName, auth.uid, hostp->h_name);
        sendReturnCode(s,RESE_DENIED);
        goto doAcceptFail;
    }


    if ((cc = fork()) == 0) {

        childAcceptConn(s, pw, &auth, &connReq, hostp);
        return;
    }


    if (connReq.eexec.len > 0)
        free(connReq.eexec.data);

    closesocket(s);
    if (cc < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");

    return;

doAcceptFail:

    if (connReq.eexec.len > 0)
        free(connReq.eexec.data);

    shutdown(s, 2);
    closesocket(s);
    return;

}

void
childAcceptConn(int s, struct passwd *pw, struct lsfAuth *auth,
                struct resConnect *connReq, struct hostent *hostp)
{
    static char     fname[] = "childAcceptConn";
    struct client   *cli_ptr;
    char            msg[512];
    int             i;
    int             num;
    GETGROUPS_T rootgroups[NGROUPS];
    int             ngroups;
    int             cc;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Application Res begin processing", fname);
    }

    signal (SIGALRM, SIG_IGN);

    CLOSE_IT(accept_sock);
    CLOSE_IT(ctrlSock);
    ctrlSock = INVALID_FD;

    allow_accept = FALSE;
    child_res = TRUE;
    child_go = TRUE;

    cli_ptr = (struct client *) malloc(sizeof(struct client));
    if (cli_ptr == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        sendReturnCode(s,RESE_NOMEM);
        goto doAcceptFail;
    }

    cli_ptr->client_sock = s;
    cli_ptr->ruid = pw->pw_uid;

    if ((cli_ptr->homedir = putstr_(pw->pw_dir ? pw->pw_dir : "/tmp"))
        == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "putstr_",
                  pw->pw_dir ? pw->pw_dir : "/tmp");
        sendReturnCode(s,RESE_NOMEM);
        free(cli_ptr);
        goto doAcceptFail;
    }

    if ((cli_ptr->username = putstr_(auth->lsfUserName)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "putstr_",
                  auth->lsfUserName);
        sendReturnCode(s,RESE_NOMEM);
        free(cli_ptr->homedir);
        free(cli_ptr);
        goto doAcceptFail;
    }



    if ((cli_ptr->hostent.h_name = putstr_(hostp->h_name)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "putstr_",
                  hostp->h_name);
        sendReturnCode(s,RESE_NOMEM);
        free(cli_ptr->homedir);
        free(cli_ptr->username);
        free(cli_ptr);
        goto doAcceptFail;
    }

    num = 0;
    for (i=0; hostp->h_aliases[i] != NULL; i++)
        num++;
    cli_ptr->hostent.h_aliases = (char **) calloc(num+1, sizeof (char **));
    if (cli_ptr->hostent.h_aliases == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        sendReturnCode(s,RESE_NOMEM);
        free(cli_ptr->username);
        free(cli_ptr->homedir);
        free(cli_ptr->hostent.h_name);
        free(cli_ptr);
        goto doAcceptFail;
    }

    for (i=0; i<num; i++)
        if ((cli_ptr->hostent.h_aliases[i] = putstr_(hostp->h_aliases[i]))
            == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "putstr_",
                      hostp->h_aliases[i]);
            sendReturnCode(s,RESE_NOMEM);
            for(i--; i >= 0; i--)
                free(cli_ptr->hostent.h_aliases[i]);
            free(cli_ptr->username);
            free(cli_ptr->homedir);
            free(cli_ptr->hostent.h_name);
            free(cli_ptr);
            goto doAcceptFail;
        }


    if ((cli_ptr->clntdir = putstr_(pw->pw_dir ? pw->pw_dir : LSTMPDIR))
        == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "putstr_",
                  pw->pw_dir ? pw->pw_dir : "/tmp");
        sendReturnCode(s,RESE_NOMEM);
        for (i=0; i<num; i++)
            free(cli_ptr->hostent.h_aliases[i]);
        free(cli_ptr->hostent.h_aliases);
        free(cli_ptr->username);
        free(cli_ptr->homedir);
        free(cli_ptr->hostent.h_name);
        free(cli_ptr);
        goto doAcceptFail;
    }

    cli_ptr->tty = defaultTty;
    if ((cli_ptr->env = (char **)calloc(5, sizeof(char *))) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        sendReturnCode(s,RESE_NOMEM);
        for (i=0; i<num; i++)
            free(cli_ptr->hostent.h_aliases[i]);
        free(cli_ptr->hostent.h_aliases);
        free(cli_ptr->username);
        free(cli_ptr->homedir);
        free(cli_ptr->hostent.h_name);
        free(cli_ptr->clntdir);
        free(cli_ptr);
        goto doAcceptFail;
    }

    sprintf(msg, "HOME=%s", pw->pw_dir ? pw->pw_dir : "/");
    cli_ptr->env[0] = putstr_(msg);

    sprintf(msg, "SHELL=%s", pw->pw_shell ? pw->pw_shell : "/bin/sh");
    cli_ptr->env[1] = putstr_(msg);

    cli_ptr->env[2] = putstr_("PATH=/bin:/usr/bin:/usr/ucb:/usr/bin/X11:/local/bin");


    sprintf(msg, "USER=%s", pw->pw_name);
    cli_ptr->env[3] = putstr_(msg);

    cli_ptr->env[4] = NULL;

    for(i=0; i<4; i++) {
        if (cli_ptr->env[i] == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            sendReturnCode(s,RESE_NOMEM);
            for(i=0; i<4; i++) {
                if (cli_ptr->env[i])
                    free(cli_ptr->env[i]);
            }

            free(cli_ptr->env);
            for (i=0; i<num; i++)
                free(cli_ptr->hostent.h_aliases[i]);
            free(cli_ptr->hostent.h_aliases);
            free(cli_ptr->username);
            free(cli_ptr->homedir);
            free(cli_ptr->hostent.h_name);
            free(cli_ptr->clntdir);
            free(cli_ptr);
            goto doAcceptFail;
        }
    }

    cli_ptr->eexec = connReq->eexec;

    clients[client_cnt] = cli_ptr;
    client_cnt++;

    if (logclass & LC_TRACE) {
        dumpClient(cli_ptr, "new client created");
    }

    ngroups = getgroups(NGROUPS, rootgroups);


    if ((getuid() == 0) && (initgroups(cli_ptr->username, pw->pw_gid) < 0)) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "initgroups",
                  cli_ptr->username, (int) pw->pw_gid);

        cli_ptr->ngroups = 0;
    }
    else
        cli_ptr->ngroups = getgroups(NGROUPS, cli_ptr->groups);


    cli_ptr->gid = pw->pw_gid;

    if (auth->gid != pw->pw_gid)
    {

        for (i = 0; i < cli_ptr->ngroups; i++)
            if (auth->gid == cli_ptr->groups[i])
            {

                cli_ptr->gid = auth->gid;
                goto doac_done;
            }

    }

doac_done:

    if ((cc = setClUid(cli_ptr)) != 0) {
    }

    sendReturnCode(s,RESE_OK);
    return;

doAcceptFail:

    if (connReq->eexec.len > 0)
        free(connReq->eexec.data);

    shutdown(s, 2);
    closesocket(s);
    resExit_(-1);
}

static int
setClUid(struct client *cli_ptr)
{
    static char fname[] = "setClUid";
    char **saveEnv = environ;
    char val[MAXLINELEN];



    if (setCliEnv(cli_ptr, "LSF_FROM_HOST", cli_ptr->hostent.h_name) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LSF_FROM_HOST",
                  cli_ptr->hostent.h_name);
    }

    sprintf(val, "%d", (int)getpid());

    if (setCliEnv(cli_ptr, "LS_JOBPID", val) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LS_JOBPID",
                  val);
    }

    if (setCliEnv(cli_ptr, LS_EXEC_T, "START") < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LS_EXEC_T",
                  "START");
    }

    environ = cli_ptr->env;



    runEexec_("", cli_ptr->ruid, &cli_ptr->eexec, NULL);

    environ = saveEnv;

    if (setgid(cli_ptr->gid) < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "setgid",
                  cli_ptr->username, cli_ptr->gid);


    if (setEUid(cli_ptr->ruid) < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "setEUid",
                  cli_ptr->username, cli_ptr->ruid);

    if (setsid() == -1)
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "setsid",
                  cli_ptr->username);

    return(0);
}

static
int recvConnect(int s, struct resConnect *connReq, int (*readFunc)(),
                struct lsfAuth *auth)
{
    static char     fname[] = "recvConnect";
    char *buf;
    XDR xdrs;
    struct LSFHeader reqHdr;
    char hdrBuf[sizeof(struct LSFHeader)];
    int tmp;

    xdrmem_create(&xdrs, hdrBuf, sizeof(hdrBuf), XDR_DECODE);

    if ((tmp=readDecodeHdr_(s, hdrBuf, readFunc, &xdrs, &reqHdr)) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "readDecodeHdr_");
        sendReturnCode(s, RESE_REQUEST);
        xdr_destroy(&xdrs);
        return (-1);
    }
    currentRESSN = reqHdr.refCode;

    xdr_destroy(&xdrs);

    if ((buf = malloc(reqHdr.length)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        sendReturnCode(s, RESE_NOMEM);
        return (-1);
    }

    xdrmem_create(&xdrs, buf, XDR_DECODE_SIZE_(reqHdr.length), XDR_DECODE);

    if (readDecodeMsg_(s, buf, &reqHdr, readFunc, &xdrs, (char *) connReq,
                       xdr_resConnect, auth) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "readDecodeMsg_");
        sendReturnCode(s, RESE_REQUEST);
        xdr_destroy(&xdrs);
        free(buf);
        return (-1);
    }

    free(buf);
    xdr_destroy(&xdrs);

    return (0);
}

void
doclient(struct client *cli_ptr)
{
    static char fname[] = "doclient";
    char *buf;
    char hdrbuf[sizeof(struct LSFHeader)];
    struct LSFHeader msgHdr;
    XDR xdrs;
    int cc;
    static int called = 0;

    if (debug>1) {
        printf("doclient\n");
    }
    ++called;
    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: called=<%d> client=<%x> client_sock=<%d>, ruid=<%d>, username=<%s>",
                  fname, called, cli_ptr, cli_ptr->client_sock,
                  cli_ptr->ruid, cli_ptr->username);
    }

    xdrmem_create(&xdrs, hdrbuf, sizeof(struct LSFHeader), XDR_DECODE);
    if ((cc = readDecodeHdr_(cli_ptr->client_sock,hdrbuf, SOCK_READ_FIX, &xdrs,
                             &msgHdr)) < 0) {
        currentRESSN = msgHdr.refCode;
        if ( errno != ECONNRESET ) {
            sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        }
        delete_client(cli_ptr);
        xdr_destroy(&xdrs);
        return;
    }
    xdr_destroy(&xdrs);


    currentRESSN = msgHdr.refCode;

    if (msgHdr.length) {
        buf=malloc(msgHdr.length);
        if (!buf) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            sendReturnCode(cli_ptr->client_sock, RESE_NOMEM);
            delete_client(cli_ptr);
        }
    } else
        buf=NULL;

    xdrmem_create(&xdrs, buf, XDR_DECODE_SIZE_(msgHdr.length), XDR_DECODE);

    if (SOCK_READ_FIX(cli_ptr->client_sock, buf, msgHdr.length) !=
        msgHdr.length) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "SOCK_READ_FIX",
                  msgHdr.length);
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        delete_client(cli_ptr);
        xdr_destroy(&xdrs);
        if (msgHdr.length)
            free(buf);
        return;
    }

    if ((msgHdr.opCode != RES_CONTROL)  && cli_ptr->ruid == 0 && ( (resParams[LSF_ROOT_REX].paramValue == NULL) ||
                                                                   ( (strcasecmp(resParams[LSF_ROOT_REX].paramValue, "all") != 0) &&
                                                                     (hostIsLocal(cli_ptr->hostent.h_name) == FALSE) ) ) ) {
        ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5285, "%s: root remote execution from host %s permission denied")), /*
                                                                                                                                    catgets 5285 */
                  fname, cli_ptr->hostent.h_name);
        sendReturnCode(cli_ptr->client_sock,RESE_ROOTSECURE);
        delete_client(cli_ptr);
        xdr_destroy(&xdrs);
        if (msgHdr.length)
            free(buf);
        return;
    }

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"%s: Res got request=<%d>",
                  fname, msgHdr.opCode);
    }

    switch (msgHdr.opCode) {
        case RES_SETENV_ASYNC:
        case RES_SETENV:
            resSetenv(cli_ptr, &msgHdr, &xdrs);
            break;
        case RES_INITTTY:
            resStty(cli_ptr, &msgHdr, &xdrs, FALSE);
            break;
        case RES_INITTTY_ASYNC:
            resStty(cli_ptr, &msgHdr, &xdrs, TRUE);
            break;
        case RES_RKILL:
            resRKill(cli_ptr, &msgHdr, &xdrs);
            break;
        case RES_CONTROL:
            resControl(cli_ptr, &msgHdr, &xdrs, INVALID_FD);
            break;
        case RES_DEBUGREQ:
            resDebugReq(cli_ptr, &msgHdr, &xdrs,INVALID_FD);
            break;
        case RES_CHDIR:
            resChdir(cli_ptr, &msgHdr, &xdrs);
            break;
        case RES_EXEC:
        case RES_SERVER:
            resRexec(cli_ptr, &msgHdr, &xdrs);
            break;
        case RES_GETPID:
            resGetpid(cli_ptr, &msgHdr, &xdrs);
            break;
        case RES_RUSAGE:
            resRusage(cli_ptr, &msgHdr, &xdrs);
            break;
        case RES_NONRES:
            resTaskMsg(cli_ptr, &msgHdr, hdrbuf, buf, &xdrs);
            break;
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5139,
                                             "%s: Unrecognized service request(%d)"), fname, /* catgets 5139 */
                      msgHdr.opCode);
            sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
            break;
    }

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Res request <%d> has been processed, back to the main loop",
                  fname, msgHdr.opCode);
    }

    xdr_destroy(&xdrs);
    if (msgHdr.length)
        free(buf);
    return;
}


static int
forwardTaskMsg(int srcSock, int destSock,
               struct LSFHeader *msgHdr,
               char *hdrbuf, char *dataBuf,
               bool_t noAck, int pid)
{
    static char *fname="forwardTaskMsg";

    if (FD_NOT_VALID(destSock)) {
        sendReturnCode(srcSock, RESE_INVCHILD);
        return(-1);
    }

    if (debug > 1) {
        printf("forwardTaskMsg(destSock=%d)\n",destSock);
    }

    if (SOCK_WRITE_FIX(destSock, hdrbuf, LSF_HEADER_LEN) !=
        LSF_HEADER_LEN) {

        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "SOCK_WRITE_FIX",
                  LSF_HEADER_LEN);

        if (kill(-pid, 0) < 0) {

            sendReturnCode(srcSock, RESE_INVCHILD);
            return (-1);
        } else {
            if (! noAck)
                sendReturnCode(srcSock, RESE_FATAL);
            return(-1);
        }
    }

    if (msgHdr->length) {
        if (SOCK_WRITE_FIX(destSock, dataBuf, msgHdr->length)!=msgHdr->length) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "SOCK_WRITE_FIX",
                      msgHdr->length);

            if (kill(-pid, 0) < 0) {

                sendReturnCode(srcSock, RESE_INVCHILD);
            } else {
                if (debug > 1)
                    ls_syslog(LOG_DEBUG, "\
%s: the process <%d> is still there.\n", fname, pid);
                if (! noAck)
                    sendReturnCode(srcSock, RESE_FATAL);
            }
        }
    }
    if (! noAck)
        sendReturnCode(srcSock, RESE_OK);

    return(0);
}

void
dochild_info(struct child *chld, int op)
{
    struct client *cliPtr;

    static char fname[] = "dochild_info";
    char hdrbuf[sizeof(struct LSFHeader)];
    struct LSFHeader msgHdr;
    XDR xdrs;
    int cc;
    char *buf;

    if (debug>1) {
        printf("dochild_info(%d)\n", chld->rpid);
    }

    xdrmem_create(&xdrs, hdrbuf, sizeof(struct LSFHeader), XDR_DECODE);
    cc = readDecodeHdr_(chld->info,hdrbuf, b_read_fix, &xdrs,
                        &msgHdr);
    if (cc < 0) {
        if (lserrno == LSE_MSG_SYS) {
            FD_CLR(chld->info, &readmask);
            ls_syslog(LOG_DEBUG, "%s: task <%d> closed info <%d>:%M",
                      fname, chld->rpid, chld->info);
            CLOSE_IT(chld->info);
            eof_to_client(chld);
            unlink_child(chld);
            xdr_destroy(&xdrs);
            return;
        } else {
            sendReturnCode(chld->info, RESE_REQUEST);
            xdr_destroy(&xdrs);
            return;
        }
    }

    xdr_destroy(&xdrs);

    if (msgHdr.length) {
        buf=malloc(msgHdr.length);
        if (!buf) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            sendReturnCode(chld->info, RESE_NOMEM);
            return;
        }


        if (b_read_fix(chld->info, buf, msgHdr.length) !=
            msgHdr.length) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "b_read_fix",
                      msgHdr.length);
            sendReturnCode(chld->info, RESE_REQUEST);

            free(buf);

            return;
        }
    } else {
        buf=NULL;
    }

    switch (msgHdr.opCode) {
        default:

            cliPtr = chld->backClnPtr;
            if (cliPtr == NULL) {

                sendReturnCode(chld->info, RESE_NOCLIENT);

                CLOSE_IT(chld->info);
                return;
            }


            (void)forwardTaskMsg(chld->info, cliPtr->client_sock,
                                 &msgHdr, hdrbuf, buf, TRUE, 0);
    }

    FREEUP(buf);
}

static void
resChdir(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs)
{
    static char fname[] = "resChdir";
    int i, cc;
    struct stat statbuf;
    char   resdir[MAXPATHLEN];
    struct resChdir ch;

    if (getcwd(resdir, sizeof(resdir)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "getwd");
        return;
    }
    if (!xdr_resChdir(xdrs, &ch, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resChdir");
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    mychdir_(cli_ptr->homedir, &(cli_ptr->hostent));

    if (mychdir_(ch.dir, &(cli_ptr->hostent)) == 0) {
        cc = RESE_OK;
    } else {
        if (ch.dir[0] != '/'
            || ((i=stat(ch.dir, &statbuf)) < 0 && errno != EACCES)
            || ((i >= 0) && (statbuf.st_mode & S_IFMT) != S_IFDIR)) {
            chdir(resdir);
            sendReturnCode(cli_ptr->client_sock, RESE_CWD);
            return;
        }
        if (access(ch.dir, X_OK) < 0)
            cc = RESE_DIRW;
        else
            cc = RESE_OK;
    }

    free(cli_ptr->clntdir);
    cli_ptr->clntdir = putstr_(ch.dir);

    sendReturnCode(cli_ptr->client_sock, cc);
    chdir(resdir);
    return;
}


static void
resSetenv(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs)
{
    static char fname[] = "resSetenv";
    struct resSetenv envReq;
    char bufHome[MAXLINELEN];
    char bufWinDir[MAXLINELEN];
    int cnt;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Setting the environment for the remote client=<%x>",
                  fname, cli_ptr);
    }

    bufHome[0] = '\0';
    bufWinDir[0] = '\0';
    if (!xdr_resSetenv(xdrs, &envReq, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resSetenv");
        if (msgHdr->opCode == RES_SETENV)
            sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    for (cnt = 0; cli_ptr->env[cnt]; cnt++) {
        if (strncasecmp (cli_ptr->env[cnt], "HOME=", strlen("HOME=")) == 0) {
            strcpy(bufHome, cli_ptr->env[cnt]);
        }
    }


    for (cnt = 0; envReq.env[cnt]; cnt++) {
        if (strncasecmp (envReq.env[cnt], "WINDIR=", strlen("WINDIR=")) == 0) {
            strcpy (bufWinDir, envReq.env[cnt]);
        }
    }

    freeblk(cli_ptr->env);
    cli_ptr->env = envReq.env;


    if (bufWinDir[0] != '\0' && bufHome[0] != '\0') {
        setCliEnv(cli_ptr, "HOME", bufHome + strlen("HOME") +1);
    }

    if (msgHdr->opCode == RES_SETENV)
        sendReturnCode(cli_ptr->client_sock, RESE_OK);

    return;
}


static void
resStty(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs, int async)
{
    static char fname[] = "resStty";
    struct resStty tty;

    if (!xdr_resStty(xdrs, &tty, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resStty");
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    cli_ptr->tty.attr = tty.termattr;
    cli_ptr->tty.ws = tty.ws;

    if (!async)
        sendReturnCode(cli_ptr->client_sock, RESE_OK);

    return;
}


static void
resRKill(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs)
{
    static char fname[] = "resRKill";
    int rempid, sig;
    int i, cc;
    struct resRKill rkill;
    int pid;

    if (debug > 1) {
        printf("resRKill\n");
    }

    if (!xdr_resRKill(xdrs, &rkill, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resRKill");
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    rempid = rkill.rid;
    sig = sig_decode(rkill.signal);

    if (! (rkill.whatid & (RES_RID_ISTID | RES_RID_ISPID))) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5149,
                                         "%s: unexpeced id class for rkill: %x"),  /* catgets 5149 */
                  fname, rkill.whatid);
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    if (rempid) {
        if (rkill.whatid & RES_RID_ISTID) {
            for (cc = 0; cc < child_cnt; cc++)
                if (children[cc]->backClnPtr == cli_ptr
                    && children[cc]->rpid == rempid     )
                    break;
            if (cc == child_cnt) {
                ls_syslog(LOG_DEBUG, "%s: no task with tid = <%d>", fname, rkill.rid);
                sendReturnCode(cli_ptr->client_sock, RESE_INVCHILD);
                return;
            }
            pid = children[cc]->pid;
        } else {
            pid = rkill.rid;
        }

        if ( ! sig ) {

            sendReturnCode(cli_ptr->client_sock, RESE_OK);

        } else if (kill(-pid, sig) == 0) {
            ls_syslog(LOG_DEBUG3, "%s: send signal <%d> to process <%d> tid <%d> ok", fname, sig, pid, rkill.rid);
            sendReturnCode(cli_ptr->client_sock, RESE_OK);
        } else {
            if ( logclass & LC_EXEC ) {
                ls_syslog(LOG_DEBUG, _i18n_msg_get(ls_catd , NL_SETN, 5150,
                                                   "%s: unable to send signal <%d> to process <%d> rid <%d>: %m"), fname, sig, pid, rkill.rid); /* catgets 5150 */
            }
            sendReturnCode(cli_ptr->client_sock, RESE_KILLFAIL);
        }

        return;
    } else {
        i = 0;
        for (cc = 0; cc < child_cnt; cc++) {
            if (children[cc]->backClnPtr == cli_ptr){
                if (!children[cc]->running
                    && !WIFSTOPPED(children[cc]->wait))
                    continue;

                if (! sig) {
                    sendReturnCode(cli_ptr->client_sock, RESE_OK);
                    return;
                } else {
                    if (kill(-children[cc]->pid, sig) < 0) {
                        sendReturnCode(cli_ptr->client_sock, RESE_KILLFAIL);
                        return;
                    }
                    i++;
                }
            }
        }
        if (i == 0) {
            sendReturnCode(cli_ptr->client_sock, RESE_INVCHILD);
            ls_syslog(LOG_DEBUG, "%s: no valid task to be killed", fname);
        } else {
            sendReturnCode(cli_ptr->client_sock, RESE_OK);
        }
        return;
    }

}

static void
resGetpid(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs)
{
    static char fname[] = "resGetpid";
    int rempid;
    struct _buf_ {
        struct LSFHeader hdrbuf;
        struct resPid pidbuf;
    } buf;

    int cc;
    int rc;
    struct LSFHeader replyHdr;
    struct resPid pidreq;


    if (!xdr_resGetpid(xdrs, &pidreq, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resGetpid");
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    rempid = pidreq.rpid;


    {
        for (cc = 0; cc < child_cnt; cc++)
            if (children[cc]->backClnPtr == cli_ptr
                && children[cc]->rpid == rempid     )
                break;

        if (cc == child_cnt) {

            pidreq.pid = -1;
        } else
            if (!children[cc]->running && ! WIFSTOPPED(children[cc]->wait)) {


                pidreq.pid = -1;
            } else
                pidreq.pid = children[cc]->pid;
    }

    initLSFHeader_(&replyHdr);
    replyHdr.opCode = RESE_OK;
    replyHdr.refCode = currentRESSN;

    rc = writeEncodeMsg_(cli_ptr->client_sock, (char *)&buf, sizeof(buf),
                         &replyHdr, (char *)&pidreq, SOCK_WRITE_FIX,
                         xdr_resGetpid, 0);
    if (rc < 0)
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5152,
                                         "%s: failed to reply to a getpid req for uid = <%d> rpid = <%d>"), /* catgets 5152 */
                  fname, cli_ptr->ruid, pidreq.rpid);

    return;
}

/* resRusage()
 */
static void
resRusage(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs)
{
    static char        fname[] = "resRusage()";
    struct resRusage   rusageReq;
    int                rid;
    int                pid;
    int                cc;
    struct LSFHeader   replyHdr;
    char               replyBuf[RES_REPLYBUF_LEN];
    struct jRusage     *jru;
    int                rc;

    if (debug > 1) {
        printf("resRusage\n");
    }

    if (! xdr_resGetRusage(xdrs, &rusageReq, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resGetRusage");
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    rid = rusageReq.rid;

    if (! (rusageReq.whatid & (RES_RID_ISTID | RES_RID_ISPID))) {
        ls_syslog(LOG_ERR, "\
%s: unexpected id class for rusage: %x", fname, rusageReq.whatid);
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    if (rusageReq.whatid & RES_RID_ISTID) {
        for (cc = 0; cc < child_cnt; cc++)
            if (children[cc]->backClnPtr == cli_ptr
                && children[cc]->rpid == rid     )
                break;
        if (cc == child_cnt) {
            sendReturnCode(cli_ptr->client_sock, RESE_INVCHILD);
            return;
        }
        pid = children[cc]->pid;
    } else {
        pid = rusageReq.rid;
        if (kill(pid, 0) < 0) {
            sendReturnCode(cli_ptr->client_sock, RESE_INVCHILD);
            return;
        }
    }


    if (rusageReq.options & RES_RPID_KEEPPID) {

        resKeepPid = TRUE;

        pidSet = listSetInsert(pid, pidSet);

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: client %x is asking RES_RPID_KEEPPID, pid saved", fname, cli_ptr);
        }

    }

    jru = getJInfo_(1, &pid, PIM_API_UPDATE_NOW, pid);

    if (jru == NULL) {
        sendReturnCode(cli_ptr->client_sock, RESE_RUSAGEFAIL);
        if (lserrno == LSE_CONN_SYS) {
            ls_syslog(LOG_DEBUG, _i18n_msg_get(ls_catd , NL_SETN, 5155,
                                               "%s: failed to getrusage for process <%d> tid <%d>"), /* catgets 5155 */
                      fname, pid, rid);
        } else {
            ls_syslog(LOG_DEBUG, "%s: failed to getrusage for process <%d> tid <%d> (not updated)", fname, pid, rid);
        }
        return;
    }

    if (logclass & LC_TRACE) {

        ls_syslog(LOG_DEBUG, "\
%s: Res child=<%x> whatId=<%x> rpid=<%d> mem(%d) swap(%d) utime(%d) stime(%d) npids(%d)",
                  fname,
                  children[cc],
                  rusageReq.whatid,
                  rid,
                  jru->mem,
                  jru->swap,
                  jru->utime,
                  jru->stime,
                  jru->npids);

        if (jru->npids) {
            for (cc = 0; cc < jru->npids; cc++) {
                ls_syslog(LOG_DEBUG,"\
%s: pid(%d) ppid(%d) pgid(%d) jobid(%d)",
                          fname,
                          jru->pidInfo[cc].pid,
                          jru->pidInfo[cc].ppid,
                          jru->pidInfo[cc].pgid,
                          jru->pidInfo[cc].jobid);
            }
        }
    }

    if (resKeepPid == TRUE) {
        if (jru->npids) {
            for (cc = 0; cc < jru->npids; cc++) {
                pidSet = listSetInsert(jru->pidInfo[cc].pid, pidSet);
            }
        }
    }


    initLSFHeader_(&replyHdr);
    replyHdr.opCode = RESE_OK;
    replyHdr.refCode = currentRESSN;

    rc = writeEncodeMsg_(cli_ptr->client_sock,
                         replyBuf,
                         sizeof(replyBuf),
                         &replyHdr,
                         (char *)jru,
                         SOCK_WRITE_FIX,
                         xdr_jRusage,
                         0);

    if (rc < 0) {
        if (debug > 1)
            printf("%s: failed to reply to the getrusage request\n", fname);

        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5156,
                                         "%s: failed to reply to a getrusage request for uid = <%d> %s = <%d>\n"), /* catgets 5156 */
                  fname,
                  cli_ptr->ruid,
                  (rusageReq.whatid & RES_RID_ISTID) ? "rpid" : "pid",
                  rid);
    }

}

static void
resTaskMsg(struct client *cli_ptr, struct LSFHeader *msgHdr,
           char *hdrBuf, char *dataBuf, XDR *xdrs)
{
    int rempid = msgHdr->reserved;
    int cc;
    bool_t intFlag;

    if (debug > 1) {
        printf("resTaskMsg\n");
        printf("looking for taskid %d \n", rempid);
    }

    for (cc = 0; cc < child_cnt; cc++)
        if (children[cc]->backClnPtr == cli_ptr
            && children[cc]->rpid == rempid     )
            break;
    if (cc == child_cnt) {
        sendReturnCode(cli_ptr->client_sock, RESE_INVCHILD);
        return;
    }

    intFlag = FALSE;

    (void)forwardTaskMsg(cli_ptr->client_sock, children[cc]->info,
                         msgHdr, hdrBuf, dataBuf, intFlag, children[cc]->pid);

}

/* doResParentCtrl()
 */
void
doResParentCtrl(void)
{
    static char          fname[] = "doResParentCtrl()";
    struct sockaddr_in   from;
    socklen_t            fromlen;
    int                  s;
    int                  cc;
    char                 hdrbuf[sizeof(struct LSFHeader)];
    struct LSFHeader     msgHdr;
    XDR                  xdrs;
    char                 *buf;

    fromlen = sizeof(struct sockaddr_in);
    if ((s = b_accept_(ctrlSock, (struct sockaddr *)&from, &fromlen)) < 0) {
        if (errno != EWOULDBLOCK)
            ls_syslog(LOG_ERR, "%s: b_accept() failed %M", fname);
        return;
    }

    xdrmem_create(&xdrs, hdrbuf, sizeof(struct LSFHeader), XDR_DECODE);

    cc = readDecodeHdr_(s, hdrbuf, SOCK_READ_FIX, &xdrs, &msgHdr);
    if (cc < 0) {
        ls_syslog(LOG_ERR, "\
%s: readDecodeHdr failed, cc=%d: %M", fname, cc);
        xdr_destroy(&xdrs);
        closesocket(s);
        return;
    }
    xdr_destroy(&xdrs);

    if (msgHdr.length) {
        buf = malloc(msgHdr.length);
        if (!buf) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            closesocket(s);
            return;
        }
    } else
        buf = NULL;

    xdrmem_create(&xdrs, buf, XDR_DECODE_SIZE_(msgHdr.length), XDR_DECODE);

    if (SOCK_READ_FIX(s, buf, msgHdr.length) != msgHdr.length) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "SOCK_READ_FIX");
        xdr_destroy(&xdrs);
        FREEUP(buf);
        closesocket(s);
        return;
    }

    switch (msgHdr.opCode) {
        case RES_CONTROL:
            resControl(NULL, &msgHdr, &xdrs, s);
            break;
        case RES_DEBUGREQ:
            resDebugReq(NULL, &msgHdr, &xdrs,s);
            break;
        case RES_ACCT:
            resParentWriteAcct(&msgHdr, &xdrs, s);
            break;
        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5160,
                                             "%s: Unknown opCode %d"), fname, msgHdr.opCode); /* catgets 5160 */
    }

    xdr_destroy(&xdrs);
    closesocket(s);

    FREEUP(buf);

}


static void
resControl(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs,
           int childSock)
{
    static char fname[] = "resControl";
    char *daemon_path;
    int i, err, maxfds;
    struct resControl ctrl;
    resAck ack;

    if (!xdr_resControl(xdrs, &ctrl, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resControl");
        if (cli_ptr)
            sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;

    }

    if (cli_ptr) {


        err = checkPermResCtrl(cli_ptr);

        if (err != 0) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5162,
                                             "%s: Ctrl opCode %d permission denied, uid = <%d>"), /* catgets 5162 */
                      fname, ctrl.opCode, cli_ptr->ruid);
            sendReturnCode(cli_ptr->client_sock, err);
            return;
        }

        ack = sendResParent(msgHdr, (char *) &ctrl, xdr_resControl);

        sendReturnCode(cli_ptr->client_sock, ack);
        return;
    }



    switch (ctrl.opCode) {
        case RES_CMD_REBOOT :
            daemon_path = getDaemonPath_("/res",resParams[LSF_SERVERDIR].paramValue);
            if (access(daemon_path, X_OK) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "access",
                          daemon_path, X_OK);
                sendReturnCode(childSock, RESE_DENIED);
                return;
            }
            ls_syslog(LOG_DEBUG,"%s: reexecing from %s",fname,daemon_path);
            switch(fork()) {
                case 0:
                    if (debug > 1)
                        i = 3;
                    else
                        i = 0;
                    maxfds = sysconf(_SC_OPEN_MAX);
                    while (i < maxfds)
                        close(i++);
                    millisleep_(5000);
                    lsfExecvp(daemon_path, restart_argv);
                    ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "execvp",
                              daemon_path);
                    resExit_(-1);
                case -1:
                    sendReturnCode(childSock, RESE_FORK);
                    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
                    return;
                default:
                    sendReturnCode(childSock, RESE_OK);
                    ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd , NL_SETN, 5167,
                                                         "%s: RES restart received, restarting."), /* catgets 5167 */
                              fname);
                    allow_accept = 0;
                    closesocket(accept_sock);
                    closesocket(ctrlSock);
                    child_res = 1;
                    child_go = 1;

                    alarm(0);

                    return;
            }

        case RES_CMD_SHUTDOWN :
            sendReturnCode(childSock, RESE_OK);
            ls_syslog(LOG_WARNING, _i18n_msg_get(ls_catd , NL_SETN, 5168,
                                                 "%s: RES shutdown received, keep current service and then exit"), fname); /* catgets 5168 */
            allow_accept = 0;
            closesocket(accept_sock);
            closesocket(ctrlSock);
            child_res = 1;
            child_go = 1;

            return;

        case RES_CMD_LOGON:
            resLogcpuTime = ctrl.data;
            resLogOn = 1;
            initResLog();

            sendReturnCode(childSock, RESE_OK);
            return;

        case RES_CMD_LOGOFF:
            ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5287, "%s: Task Log OFF")), fname);   /* catgets 5287 */
            resLogOn = 0;
            sendReturnCode(childSock, RESE_OK);
            return;

        default:
            sendReturnCode(childSock, RESE_REQUEST);
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5169,
                                             "%s: Unknown request code: %d Ignored."), /* catgets 5169 */
                      fname, ctrl.opCode);
            return;
    }

}

resAck
sendResParent(struct LSFHeader *msgHdr, char *msgBuf, bool_t (*xdrFunc)())
{
    static char fname[] = "sendResParent";
    int s;
    char buf[MSGSIZE];

    if ((s = TcpCreate_(FALSE, 0)) < 0) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "%s: tcpCreate failed: %m", fname);
        return (RESE_NOMORECONN);
    }

    if (b_connect_(s, (struct sockaddr *)&ctrlAddr, sizeof(ctrlAddr), 0) < 0) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "%s: b_connect failed: <%s> %m", fname,
                      sockAdd2Str_(&ctrlAddr));
        closesocket(s);
        return (RESE_RES_PARENT);
    }

    if (lsSendMsg_(s, msgHdr->opCode, 0, msgBuf, buf, MSGSIZE,
                   xdrFunc, SOCK_WRITE_FIX, NULL) < 0) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "%s: lsSendMsg failed: %m", fname);
        closesocket(s);
        return (RESE_RES_PARENT);
    }

    if (lsRecvMsg_(s, buf, sizeof(buf), msgHdr, NULL, NULL, SOCK_READ_FIX)
        < 0) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "%s: lsRecvMsg failed: %m", fname);
        closesocket(s);
        return (RESE_RES_PARENT);
    }

    closesocket(s);

    if (msgHdr->opCode != RESE_OK) {
        if (logclass & LC_EXEC)
            ls_syslog(LOG_DEBUG, "%s: RES parent rejected request: %d",
                      fname, msgHdr->opCode);
    }
    return (resAck) msgHdr->opCode;
}


static int
checkPermResCtrl( struct client *cli_ptr )
{
    static char fname[] = "checkPermResCtrl";
    struct clusterInfo *clusterInfo;
    static char  *mycluster;
    static int nAdmins = -1;
    static char **admins = NULL;
    int i;



    if (cli_ptr->ruid == 0 || debug)
        return(0);

    if (mycluster == NULL) {
        if ((mycluster = ls_getclustername()) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_getclustername");
            goto tryDefault;
        }
    }

    if (nAdmins == -1) {
        if ((clusterInfo = ls_clusterinfo(NULL, NULL, NULL, 0, 0)) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_clusterinfo");
            goto tryDefault;
        }


        nAdmins = (clusterInfo->nAdmins) ? clusterInfo->nAdmins : 1;
        admins = (char **) malloc (nAdmins * sizeof (char *));
        if (admins == NULL) {
            nAdmins = -1;
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            goto tryDefault;
        }

        if (clusterInfo->nAdmins == 0) {
            admins[0] = putstr_(clusterInfo->managerName);
        } else {
            for (i = 0; i < nAdmins; i++) {
                admins[i] = putstr_(clusterInfo->admins[i]);
            }
        }
    }

    for (i = 0; i < nAdmins; i++) {
        if (strcmp(cli_ptr->username, admins[i]) == 0)
            return (0);
    }

    return(RESE_DENIED);

tryDefault:

    if (isLSFAdmin_(cli_ptr->username)) {
        return(0);
    }
    return (RESE_DENIED);

}

/* resRexec()
 */
static void
resRexec(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs)
{
    static char          fname[] = "resRexec()";
    struct resCmdBill    cmdmsg;
    struct sockaddr_in   addr;
    struct sockaddr_in   from;
    socklen_t            addrLen = sizeof(addr);
    socklen_t            fromLen = sizeof(from);
    int                  taskSock = -1;
    resAck               ack;
    struct child         *child_ptr;
    int                  exitStatus = 0;
    int                  terWhiPendStatus = 0;
    struct linger        linstr = {1, 5};

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Executing the task on behalf of the remote client=<%x>",
                  fname, cli_ptr);
    }

    if (!xdr_resCmdBill(xdrs, &cmdmsg, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resCmdBill");
        if (msgHdr->opCode == RES_SERVER)
            sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    if (resParams[LSF_ENABLE_PTY].paramValue &&
        !strcasecmp(resParams[LSF_ENABLE_PTY].paramValue, "n"))
        cmdmsg.options = cmdmsg.options & ~REXF_USEPTY;

    if (cmdmsg.options & REXF_TASKPORT) {
        if ((taskSock = TcpCreate_(TRUE, 0)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "TcpCreate");
            addr.sin_port = 0;
        } else {
            if (getsockname(taskSock,
                            (struct sockaddr *) &addr,
                            &addrLen) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "getsockname",
                          taskSock);
                addr.sin_port = 0;
            }
        }
        sendReturnCode(cli_ptr->client_sock, (int) ntohs(addr.sin_port));
    }

    memset(&from, 0, fromLen);

    if (getpeername(cli_ptr->client_sock,
                    (struct sockaddr *)&from, &fromLen) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "getpeername",
                  cli_ptr->client_sock);
        if (msgHdr->opCode == RES_SERVER)
            sendReturnCode(cli_ptr->client_sock, RESE_CALLBACK);
        if (cmdmsg.options & REXF_TASKPORT) {
            closesocket(taskSock);
        }
        freeblk(cmdmsg.argv);
        return;
    }

    if (logclass & LC_TRACE) {
        dumpResCmdBill(&cmdmsg);
        ls_syslog(LOG_DEBUG,"\
%s: Calling back nios retport=<%d> rpid=<%d>",
                  fname, cmdmsg.retport, cmdmsg.rpid);
    }


    if (child_cnt == 0 && FD_NOT_VALID(conn2NIOS.sock.fd)) {
        if ((conn2NIOS.sock.fd = niosCallback_(&from, cmdmsg.retport,
                                               cmdmsg.rpid, exitStatus, terWhiPendStatus)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, fname, "niosCallback_",
                      ntohs(cmdmsg.retport));
            if (msgHdr->opCode == RES_SERVER)
                sendReturnCode(cli_ptr->client_sock, RESE_CALLBACK);
            if (cmdmsg.options & REXF_TASKPORT) {
                closesocket(taskSock);
            }
            freeblk(cmdmsg.argv);
            return;
        }
        io_nonblock_(conn2NIOS.sock.fd);

        if (setsockopt(conn2NIOS.sock.fd,SOL_SOCKET,SO_LINGER,(char *)&linstr,
                       sizeof(linstr)) < 0) {
            char errSockId[MAXLINELEN];
            sprintf(errSockId,"setsockopt failed on %d",conn2NIOS.sock.fd);
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, errSockId);
        }

        if (debug > 1) {
            printf("NIOS call_back sock is %d, request for task <%d>is sent\n",
                   conn2NIOS.sock.fd, cmdmsg.rpid);
            fflush(stdout);
        }
    }
    else {
        addResNotifyList(resNotifyList, cmdmsg.rpid, RES2NIOS_NEWTASK,
                         -1, NULL);
        if (debug > 1) {
            fprintf(stderr, "request for task <%d> is ready to be sent\n",
                    cmdmsg.rpid);
            fflush(stdout);
        }
    }

    if ((child_ptr = doRexec(cli_ptr, &cmdmsg, conn2NIOS.sock.fd, taskSock,
                             (msgHdr->opCode == RES_SERVER), &ack)) == NULL) {
        addResNotifyList(resNotifyList, cmdmsg.rpid, RES2NIOS_STATUS,
                         ack, NULL);
        freeblk(cmdmsg.argv);
        if (child_cnt == 0) {
            closesocket(conn2NIOS.sock.fd);
            conn2NIOS.sock.fd = INVALID_FD;
        }
        if (msgHdr->opCode == RES_SERVER)
            sendReturnCode(cli_ptr->client_sock, ack);
        if (cmdmsg.options & REXF_TASKPORT) {
            closesocket(taskSock);
        }
        return;
    }


    if (cmdmsg.options & REXF_TASKPORT)
        closesocket(taskSock);

    if (msgHdr->opCode == RES_SERVER){
        sendReturnCode(cli_ptr->client_sock, RESE_OK);
        delete_client(cli_ptr);
    }

    freeblk(cmdmsg.argv);

}

static int
resFindPamJobStarter(void)
{
    char *tmpbuf = getenv("LSB_JOB_STARTER");

    if (tmpbuf != NULL)
        if (strstr(tmpbuf,"pam"))
            return 1;
        else
            return 0;
    else
        return 0;
}

static struct child *
doRexec(struct client *cli_ptr, struct resCmdBill *cmdmsg, int retsock,
        int taskSock, int server, resAck *ack)
{
    static char fname[] = "doRexec";
    int pty[2], sv[2], info[2], errSock[2];
    int pid = -1;
    struct sigStatusUsage *sigStatRu = NULL;
    char pty_name[sizeof(PTY_TEMPLATE)];
    struct child *child_ptr;



    if (cmdmsg->options & REXF_USEPTY) {
        int echoOff = FALSE;

        if (cmdmsg->rpid > 1)
            echoOff = TRUE;

        pid = forkPty(cli_ptr,
                      pty,
                      sv,
                      (cmdmsg->options & REXF_TASKINFO) ? info : NULL,
                      (cmdmsg->options & REXF_STDERR) ? errSock : NULL,
                      pty_name,
                      ack,
                      retsock, echoOff);

    } else {
        pid = forkSV(cli_ptr,
                     sv,
                     (cmdmsg->options & REXF_TASKINFO) ? info : NULL,
                     (cmdmsg->options & REXF_STDERR) ? errSock : NULL,
                     ack);
    }


    if (pid < 0)
        return (NULL);

    if (pid == 0) {


        if (sbdMode) {
            if (Myhost)
                putEnv("HOSTNAME", Myhost);

            if (myHostType)
                putEnv("HOSTTYPE", myHostType);

            lsbExecChild(cmdmsg, pty,
                         sv, errSock,
                         (cmdmsg->options & REXF_TASKINFO) ? info : NULL,
                         &pid);
        } else {
            if (Myhost) {
                if (setCliEnv(cli_ptr, "HOSTNAME", Myhost) < 0) {
                    ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                              "HOSTNAME", Myhost);
                }
            }
            if (myHostType) {
                if (setCliEnv(cli_ptr, "HOSTTYPE", myHostType) < 0) {
                    ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                              "HOSTTYPE", myHostType);
                }
            }

            rexecChild(cli_ptr, cmdmsg, server, taskSock, pty,
                       sv,
                       (cmdmsg->options & REXF_TASKINFO) ? info : NULL,
                       errSock,
                       retsock, &pid);
        }
        exit(-1);
    }



    if (debug > 1)
        ls_syslog(LOG_DEBUG, "%s: Parent ...", fname);


    child_ptr = (struct child *)malloc(sizeof(struct child));

    sigStatRu = (struct sigStatusUsage *) malloc(sizeof(struct sigStatusUsage));
    if (child_ptr == (struct child *) NULL ||
        sigStatRu == (struct sigStatusUsage *) NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc(child or sigStatusUsage)");
        if (cmdmsg->options & REXF_USEPTY) {
            close(pty[0]);
        } else {
            closesocket(sv[0]);
            closesocket(sv[1]);
            closesocket(errSock[0]);
            closesocket(errSock[1]);
        }
        *ack = RESE_NOMEM;
        return NULL;
    }

    child_ptr->sigStatRu = sigStatRu;
    child_ptr->sent_eof = 0;
    child_ptr->sent_status = 0;

    if (cmdmsg->options & REXF_USEPTY) {
        child_ptr->stdio = pty[0];
        child_ptr->std_out.fd = pty[0];
        strcpy(child_ptr->slavepty, pty_translate(pty_name));
    } else {
        child_ptr->stdio = sv[0];
        child_ptr->std_out.fd = sv[0];
        child_ptr->slavepty[0] = 0;
    }

    children[child_cnt] = child_ptr;
    child_cnt++;
    child_ptr->rpid = cmdmsg->rpid;
    child_ptr->pid = pid;
    child_ptr->backClnPtr = cli_ptr;
    strcpy(child_ptr->username, child_ptr->backClnPtr->username);
    strcpy(child_ptr->fromhost, child_ptr->backClnPtr->hostent.h_name);

    if (cmdmsg->options & REXF_TASKINFO)
        child_ptr->info = info[0];
    else
        child_ptr->info = -1;

    if (cmdmsg->options & REXF_STDERR) {
        child_ptr->std_err.fd = errSock[0];
    } else {
        child_ptr->std_err.fd = INVALID_FD;
    }

    child_ptr->remsock.fd = retsock;
    child_ptr->remsock.rbuf = (RelayBuf *)NULL;
    child_ptr->remsock.rcount = 0;
    child_ptr->remsock.wbuf = (RelayLineBuf *)NULL;
    child_ptr->stdin_up = 1;
    child_ptr->remsock.wcount = 0;

    child_ptr->rexflag = cmdmsg->options;


    child_ptr->refcnt = (child_ptr->info >= 0) ? 3 : 2;
    if (cmdmsg->options & REXF_STDERR) {
        child_ptr->refcnt++;
    }

    child_ptr->server = server;
    child_ptr->c_eof = cli_ptr->tty.attr.c_cc[VEOF];
    child_ptr->running = 1;
    child_ptr->sigchild = 0;
    child_ptr->endstdin = 0;
    child_ptr->i_buf.bcount = 0;

    child_ptr->std_out.endFlag = 0;
    child_ptr->std_out.retry = 0;
    child_ptr->std_out.bytes = 0;
    child_ptr->std_out.buffer.bcount = 0;

    child_ptr->std_err.endFlag = 0;
    child_ptr->std_err.retry = 0;
    child_ptr->std_err.bytes = 0;
    child_ptr->std_err.buffer.bcount = 0;

    if (cmdmsg->cwd != NULL)
        child_ptr->cwd = putstr_(cmdmsg->cwd);
    else {
        child_ptr->cwd = NULL;
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5182,
                                         "%s: cwd is null"), fname); /* catgets 5182 */
    }

    child_ptr->dpTime = time(NULL);

    if (resLogOn == 1) {
        child_ptr->cmdln = copyArray(cmdmsg->argv);
    } else {
        child_ptr->cmdln = NULL;
    }

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Child with pid=<%d> executed", fname, pid);
        dumpChild(child_ptr, -1, "child just built");
    }

    if (debug > 1) {
        printf("child(%d)'s stdio: %d, remsock: %d child_cnt: %d\n",
               child_ptr->rpid, child_ptr->stdio, child_ptr->remsock.fd,
               child_cnt);
        fflush(stdout);
    }

    return (child_ptr);
}


static int
forkPty(struct client *cli_ptr, int *pty, int *sv, int *info, int *errSock,
        char *pty_name, resAck *ack, int retsock, int echoOff)
{
    static char fname[] = "forkPty";
    int pid = -1;

    ptyreset();

    setEUid(0);

    pty[0] = ptymaster(pty_name);

    setEUid(cli_ptr->ruid);

    if (pty[0] < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "ptymaster",
                  pty_name);
        *ack = RESE_PTYMASTER;
        return (-1);
    }

    if (debug > 1)
        ls_syslog(LOG_DEBUG, "%s: ptymaster pty_name <%s>", fname, pty_name);



    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socketpair");
        close(pty[0]);
        *ack = RESE_SOCKETPAIR;
        return (-1);
    }

    if (info)
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, info) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socketpair");
            close(sv[0]);
            close(sv[1]);
            close(pty[0]);
            *ack = RESE_SOCKETPAIR;
            return (-1);
        }


    if ( (errSock != NULL)
         && (socketpair(AF_UNIX, SOCK_STREAM, 0, errSock) < 0) ) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "pairsocket");
        *ack = RESE_SOCKETPAIR;
        close(sv[0]);
        close(sv[1]);
        close(pty[0]);
        if (info) {
            close(info[0]);
            close(info[1]);
        }
        return (-1);
    }

    if ((pid = fork()) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
        close(pty[0]);
        close(sv[0]);
        close(sv[1]);
        if (info) {
            close(info[0]);
            close(info[1]);
        }
        if (errSock != NULL) {
            close(errSock[0]);
            close(errSock[1]);
        }
        *ack = RESE_FORK;
        return (-1);
    }




    if (pid == 0) {

        if (info)
            close(info[0]);

        setPGid(cli_ptr, TRUE);
        if ((*ack = childPty(cli_ptr, pty, sv, pty_name, echoOff)) != RESE_OK) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "childPty");
            _exit(127);
        }
    } else {

        if (info)
            close(info[1]);

        if (errSock != NULL) {
            close(errSock[1]);
        }

        if ( (errSock != NULL) && (io_nonblock_(errSock[0]) < 0) ) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "io_nonblock_",
                      errSock[0]);
        }

        setEUid(0);
        *ack = parentPty(pty, sv, pty_name);
        setEUid(cli_ptr->ruid);
        if (*ack != RESE_OK)
            return (-1);
    }

    return (pid);

}


static int
forkSV(struct client *cli_ptr, int *sv, int *info, int *errSock, resAck *ack)
{
    static char fname[] = "forkSV";
    int pid;

    if (info)
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, info) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socketpair");
            *ack = RESE_SOCKETPAIR;
            return (-1);
        }

    if (pairsocket(AF_INET, SOCK_STREAM, 0, sv) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "pairsocket");
        *ack = RESE_SOCKETPAIR;
        return (-1);
    }


    if ( (errSock != NULL)
         && (pairsocket(AF_INET, SOCK_STREAM, 0, errSock) < 0) ) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "pairsocket");
        *ack = RESE_SOCKETPAIR;
        close(sv[0]);
        close(sv[1]);
        return (-1);
    }

    pid = fork();

    if (pid < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
        close(sv[0]);
        close(sv[1]);
        if (info) {
            close(info[0]);
            close(info[1]);
        }
        if (errSock != NULL) {
            close(errSock[0]);
            close(errSock[1]);
        }
        *ack = RESE_FORK;
        return (pid);
    }

    if (pid > 0) {

        close(sv[1]);
        if (info)
            close(info[1]);
        if (errSock != NULL) {
            close(errSock[1]);
        }

        if (io_nonblock_(sv[0]) < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "io_nonblock_",
                      sv[0]);
        if ( (errSock != NULL) && (io_nonblock_(errSock[0]) < 0) ) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "io_nonblock_",
                      errSock[0]);
        }
    } else {

        close(sv[0]);
        if (info)
            close(info[0]);
        if (errSock != NULL) {
            close(errSock[0]);
        }
        setPGid(cli_ptr, FALSE);
    }

    return (pid);
}



static void
setPGid(struct client *cli_ptr, int tflag)
{
    static char fname[] = "setPGid";


    if (sbdMode && !(sbdFlags & SBD_FLAG_TERM))
        return;

    if (tflag) {
        if (setsid() < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "setsid");

    } else {
        if (setpgid(0, getpid()) < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "setpgid", 0);
    }

}




static resAck
parentPty(int *pty, int *sv, char *pty_name)
{
    static char fname[] = "parentPty";
    struct hand_shake handShake;



    close(sv[1]);
    for (;;) {
        b_read_fix(sv[0], (char *)&handShake.code, sizeof(status_t));
        if (handShake.code == PTY_BAD) {
            close(pty[0]);
            if ((pty[0] = ptymaster(pty_name)) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "ptymaster",
                          pty_name);
                handShake.code = PTY_NOMORE;
                write(sv[0], (char *)&handShake, sizeof(handShake));
                close(sv[0]);
                return RESE_PTYMASTER;
            } else {
                handShake.code = PTY_NEW;
                strcpy(handShake.buffer, pty_name);
                write(sv[0], (char *)&handShake, sizeof(handShake));
            }
        } else
            break;
    }
    close(sv[0]);


    if (io_nonblock_(pty[0]) < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "io_nonblock_",
                  pty[0]);


    if (ioctl(pty[0], TIOCPKT, &on) < 0)
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "ioctl",
                  pty[0]);

    return (RESE_OK);
}

static void
set_noecho(int fd)
{
    static char fname[] = "set_noecho";
    struct termios stermios;

    if (tcgetattr(fd, &stermios) < 0){
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "tcgetattr", "slave");
        return;
    }

    stermios.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);

    if (tcsetattr(fd, TCSANOW, &stermios) < 0){
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "tcsetattr", "slave");
    }
}

static resAck
childPty(struct client *cli_ptr, int *pty, int *sv, char *pty_name, int echoOff)
{
    static char fname[] = "childPty";
    struct hand_shake handShake;



    close(sv[0]);
    for (;;) {
        ls_syslog(LOG_DEBUG, "%s: ptyslave(%s) trying", fname, pty_name);

        setEUid(0);
        pty[1] = ptyslave(pty_name);
        setEUid(cli_ptr->ruid);

        if (pty[1] < 0) {
            ls_syslog(LOG_DEBUG, "%s: ptyslave(%s) failed: %m", fname,
                      pty_name);
            handShake.code = PTY_BAD;
            write(sv[1],(char *)&handShake.code, sizeof(status_t));
            b_read_fix(sv[1], (char *)&handShake, sizeof(handShake));
            if (handShake.code == PTY_NEW)
                strcpy(pty_name, handShake.buffer);
            else {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5203,
                                                 "%s: No more pty(%s); dying"), /* catgets 5203 */
                          fname, pty_name);
                return RESE_PTYSLAVE;
            }
        } else {
            ls_syslog(LOG_DEBUG, "%s: ptyslave(%s) handshake", fname,
                      pty_name);
            handShake.code = PTY_GOOD;
            write(sv[1],(char *)&handShake.code, sizeof(status_t));
            ls_syslog(LOG_DEBUG, "%s: ptyslave(%s) handshake ok", fname,
                      pty_name);
            break;
        }
    }
    close(sv[1]);

    if (debug > 1)
        ls_syslog(LOG_DEBUG, "%s: chown", fname);



    setEUid(0);

    if (fchown(pty[1], (uid_t)cli_ptr->ruid, cli_ptr->gid) < 0) {
        ls_syslog(LOG_DEBUG, "%s: fchown failed for uid <%d>/gid <%d>",
                  fname, cli_ptr->ruid, cli_ptr->gid);
    }

    if (fchmod(pty[1], 0600) < 0) {
        ls_syslog(LOG_DEBUG, "%s: fchmod failed", fname);
    }

    setEUid(cli_ptr->ruid);

    setptymode(&cli_ptr->tty, pty[1]);
    if (echoOff){


        set_noecho(pty[1]);
    }
    if (debug > 1)
        ls_syslog(LOG_DEBUG, "%s: Leaving", fname);

    return (RESE_OK);

}



static char *
stripHomeUnix(const char *curdir, const char *home)
{
    int hlen = strlen(home);


    if (strncmp(home, curdir, hlen) == 0) {
        if (curdir[hlen] == '\0') {
            curdir += hlen;
        } else if (curdir[hlen] == '/') {
            curdir += hlen + 1;
        } else {
            curdir = NULL;
        }
    } else {
        curdir = NULL;
    }

    return (char*)curdir;
}


static char *
stripHomeNT(const char *curdir, const char *home)
{
    int hlen = strlen(home);


    if (strncasecmp(home, curdir, hlen) == 0) {
        if (curdir[hlen] == '\0') {
            curdir += hlen;
        } else if ((curdir[hlen] == '/') || (curdir[hlen] == '\\')) {
            curdir += hlen + 1;
        } else {
            curdir = NULL;
        }
    } else {
        curdir = NULL;
    }

    return (char*)curdir;
}

static void
rexecChild(struct client *cli_ptr, struct resCmdBill *cmdmsg, int server,
           int taskSock, int *pty,
           int *sv,
           int *info, int *err, int retsock, int *pid)
{
    static char fname[] = "rexecChild";
    sigset_t sigMask;
    char val[MAXPATHLEN];
    int i, maxfds, lastUnusedFd;
    char *curdir = NULL;
    bool_t fromNT;
    struct passwd  *pwdHome;
    int uid;

    if (setCliEnv(cli_ptr, "LSF_FROM_HOST", cli_ptr->hostent.h_name) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LSF_FROM_HOST", cli_ptr->hostent.h_name);
    }

    if (server) {
        char buf[64];
        sprintf(buf, "%d", cli_ptr->client_sock);
        if (setCliEnv(cli_ptr, "LSF_APP_SERVSOCK", buf) < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                      "LSF_APP_SERVSOCK", buf);
    }

    sprintf(val, "%d", (int)getpid());
    if (setCliEnv(cli_ptr, "LS_JOBPID", val) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LS_JOBPID", val);
    }

    if (setCliEnv(cli_ptr, LS_EXEC_T, "START") < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "setenv",
                  "LS_EXEC_T");
    }


    sprintf(val, "%d", cmdmsg->rpid);
    if (setCliEnv(cli_ptr, "LSF_PM_TASKID", val)) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LSF_PM_TASKID", val);
    }

    if (info) {
        sprintf(val, "%d", info[1]);
        if (setCliEnv(cli_ptr, "LSF_PM_SOCKFD", val)) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                      "LSF_PM_SOCKFD", val);
        }
    }


    if (cmdmsg->options & REXF_CLNTDIR)
        sprintf(val, "%s", cli_ptr->clntdir);
    else
        sprintf(val, "%s", cmdmsg->cwd);

    if (setCliEnv(cli_ptr, "LS_SUBCWD", val) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                  "LS_SUBCWD", val);
    }

#ifdef INTER_DAEMON_AUTH

    if (getenv("LSF_EAUTH_AUX_DATA") != NULL) {
        if (setCliEnv(cli_ptr, "LSF_EAUTH_AUX_DATA",
                      getenv("LSF_EAUTH_AUX_DATA")) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_S_FAIL_M, fname, "setenv",
                      "LSF_EAUTH_AUX_DATA", getenv("LSF_EAUTH_AUX_DATA"));
        }
    }
#endif





    environ = cli_ptr->env;


    if ((getenv("WINDIR") != NULL) || (getenv("windir") != NULL)) {
        char tmppath[MAXPATHLEN];

        fromNT = TRUE;
        sprintf(tmppath,"/bin:/usr/bin:/sbin:/usr/sbin");
        if (resParams[LSF_BINDIR].paramValue != NULL) {
            strcat(tmppath,":");
            strcat(tmppath,resParams[LSF_BINDIR].paramValue);
        }

        putEnv("PATH",tmppath);
    } else {
        fromNT = FALSE;
        if (resParams[LSF_BINDIR].paramValue != NULL) {



            char *tmppath = NULL;
            int len;
            char *envpath;
            int cc = TRUE;

            envpath = getenv("PATH");
            if ( envpath !=NULL) {

                len = strlen(resParams[LSF_BINDIR].paramValue);
                cc  = strncmp(envpath,resParams[LSF_BINDIR].paramValue,len);
                if (cc != 0)
                    len += strlen(envpath) + 2;
            }else {
                len = strlen(resParams[LSF_BINDIR].paramValue) +2;
            }
            if (cc != 0) {
                tmppath = malloc(len);
                if (tmppath == NULL) {
                    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
                    exit(-1);
                }
                strcpy(tmppath, resParams[LSF_BINDIR].paramValue);
                if (envpath != NULL) {
                    strcat(tmppath,":");
                    strcat(tmppath, envpath);
                }
                putEnv("PATH", tmppath);
                FREEUP(tmppath);
            }else {
                putEnv("PATH", envpath);
            }
        }
    }


    if (resParams[LSF_BINDIR].paramValue != NULL) {
        putEnv("LSF_BINDIR", resParams[LSF_BINDIR].paramValue);
    }
    if (resParams[LSF_SERVERDIR].paramValue != NULL) {
        putEnv("LSF_SERVERDIR", resParams[LSF_SERVERDIR].paramValue);
    }
    if (resParams[LSF_LIBDIR].paramValue != NULL) {
        char tmppath[MAXFILENAMELEN];

        putEnv("LSF_LIBDIR", resParams[LSF_LIBDIR].paramValue);

        sprintf(tmppath, "%s/%s", resParams[LSF_LIBDIR].paramValue, "uid");
        putEnv("XLSF_UIDDIR", tmppath);
    }


    setEUid(0);
    setlimits(cmdmsg->lsfLimits);


    if (! debug) {
        int newPriority;
        if ( rexecPriority != 0 ) {

            newPriority = rexecPriority;
            if (cmdmsg->priority > rexecPriority) {
                newPriority = cmdmsg->priority;
            }
        }
        else {

            newPriority = cmdmsg->priority;
        }


        newPriority = newPriority - (-20);
        if (ls_setpriority(newPriority) == FALSE) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_MM, fname, "ls_setpriority",
                      newPriority);
        }
    }

    setEUid(cli_ptr->ruid);

    umask(cmdmsg->filemask);

    if (cmdmsg->options & REXF_USEPTY) {
        dup2(pty[1], 0);
        dup2(pty[1], 1);

        if (cmdmsg->options & REXF_STDERR) {
            struct linger linger;

            linger.l_onoff = 1;
            linger.l_linger = 120;
            if (setsockopt(err[1], SOL_SOCKET, SO_LINGER, (char *)&linger,
                           sizeof(struct linger)) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "setsockopt");
            }
            dup2(err[1], 2);
            close(err[1]);
        } else {
            dup2(pty[1], 2);
        }
        if (pty[1] > 2)
            close(pty[1]);

    } else {
        struct linger linger;

        linger.l_onoff = 1;
        linger.l_linger = 120;

        if (setsockopt(sv[1], SOL_SOCKET, SO_LINGER, (char *)&linger,
                       sizeof(struct linger)) < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "setsockopt");

        dup2(sv[1], 0);
        dup2(sv[1], 1);


        if (cmdmsg->options & REXF_STDERR) {
            if (setsockopt(err[1], SOL_SOCKET, SO_LINGER, (char *)&linger,
                           sizeof(struct linger)) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "setsockopt");
            }
            dup2(err[1], 2);
            close(err[1]);
        } else {
            dup2(sv[1], 2);
        }
        close(sv[1]);
    }


    if (debug != 2) {
        if (fromNT) {
            ls_syslog(LOG_DEBUG, "%s: Submission host is NT", fname);
        } else {
            ls_syslog(LOG_DEBUG, "%s: Submission host is UNIX", fname);
        }
    }


    if (!fromNT) {
        curdir = getenv("HOME");
        if ( curdir == NULL ) {
            pwdHome = getpwdirlsfuser_(cli_ptr->username);
            if (debug != 2 && pwdHome == NULL ) {
                ls_syslog(LOG_DEBUG, "%s: getpwnam failed for user %s",
                          fname,cli_ptr->username);
            }

            if (pwdHome != NULL && pwdHome->pw_dir && pwdHome->pw_dir[0]) {
                curdir = pwdHome->pw_dir;
            }
        }
    } else {
        pwdHome = getpwdirlsfuser_(cli_ptr->username);
        if (debug != 2 && pwdHome == NULL ) {
            ls_syslog(LOG_DEBUG, "%s: getpwnam failed for user %s",
                      fname,cli_ptr->username);
        }

        if (pwdHome != NULL && pwdHome->pw_dir && pwdHome->pw_dir[0]) {
            curdir = pwdHome->pw_dir;
        }
    }

    if (curdir != NULL) {

        if (debug != 2)
            ls_syslog(LOG_DEBUG, "%s: Execution homedir is %s", fname, curdir);
        if (mychdir_(curdir, &(cli_ptr->hostent)) < 0) {
            ls_syslog(LOG_DEBUG, "%s: Couldn't change to %s", fname, curdir);
        }
    }



    if (cmdmsg->options & REXF_CLNTDIR)
        curdir = cli_ptr->clntdir;
    else {
        if (!fromNT) {


            char *home = getenv("HOME");

            if ( home != NULL){
                curdir = stripHomeUnix(cmdmsg->cwd, home);
            }
        } else {


            char home[MAXPATHLEN];
            char *homeshare = getenv("HOMESHARE");
            char *homedrive = getenv("HOMEDRIVE");
            char *homepath = getenv("HOMEPATH");


            curdir = NULL;



            if (homepath != NULL) {

                if (homeshare != NULL) {
                    sprintf(home, "%s%s", homeshare, homepath);
                    curdir = stripHomeNT(cmdmsg->cwd, home);
                }

                if (curdir == NULL) {

                    if (homedrive != NULL) {
                        sprintf(home, "%s%s", homedrive, homepath);
                        curdir = stripHomeNT(cmdmsg->cwd, home);
                    }



                }
            }
        }


        if (curdir == NULL || curdir[0] == '\0') {

            curdir = cmdmsg->cwd;
        }


        osConvertPath_(curdir);
    }


    if (debug != 2)
        ls_syslog(LOG_DEBUG,"%s: Execution relative cwd is %s", fname, curdir);

    if (curdir[0] && mychdir_(curdir, &(cli_ptr->hostent)) < 0) {
        if (!server) {
            if (!getenv("windir") && getenv("LSF_JOB_STARTER") == NULL) {
                fprintf(stderr, I18N_FUNC_S_FAIL, fname, "chdir", curdir);
                perror("LSF error (res)");

                if (info != NULL) {

                    Signal_(SIGTERM, SIG_DFL);
                    kill(getpid(),SIGTERM);
                }
                _exit(127);
            }
            chdir(LSTMPDIR);
        } else {
            char *ep;
            ep = getenv("HOME");

            if (ep != NULL && ep[0] != '\0') {
                if (mychdir_(ep, NULL) < 0) {
                    fprintf(stderr, I18N_FUNC_S_FAIL_M, fname, "chdir", ep);
                    perror("");
                    fputs(_i18n_msg_get(ls_catd, NL_SETN, 4,
                                        "Trying to run in /tmp\n"), stderr); /* catgets 4 */
                    chdir(LSTMPDIR);
                }
            } else {
                fputs(_i18n_msg_get(ls_catd, NL_SETN, 6,
                                    "$HOME not defined, trying /tmp"), stderr);
                chdir(LSTMPDIR);
            }
        }
    }

    for (i = 1; i < NSIG; i++)
        Signal_(i, SIG_DFL);


    sigemptyset(&sigMask);
    sigprocmask(SIG_SETMASK, &sigMask, NULL);


    lastUnusedFd = 3;


    maxfds = sysconf(_SC_OPEN_MAX);
    for (i = lastUnusedFd; i < maxfds; i++) {
        if (!(server && i == cli_ptr->client_sock)
            && (i != taskSock)
            && (i != retsock)
            && ((info == NULL) || (i != info[1])) ) {
            close(i);
        }
    }



    uid = setEUid(0);
    if (lsfSetUid(uid) < 0) {
        perror("setuid");
        exit(-1);
    }

    ls_closelog();


    execit(cmdmsg->argv, getenv("LSF_JOB_STARTER"), pid, -1, taskSock, FALSE);

    if (info != NULL) {

        Signal_(SIGTERM, SIG_DFL);
        kill(getpid(),SIGTERM);
    }
    exit(127);

}

static void

lsbExecChild(struct resCmdBill *cmdmsg, int *pty,
             int *sv, int *err,
             int *info,
             int *pid)
{
    static char fname[] = "lsbExecChild";
    sigset_t sigMask;
    int i, maxfds, lastUnusedFd;
    int iofd;
    struct linger linger;

    if (debug > 1)
        ls_syslog(LOG_DEBUG, "%s: Entering ...", fname);


    mlsSbdMode = sbdMode;

    if (cmdmsg->options & REXF_USEPTY) {
        iofd = pty[1];
    } else {

        iofd = sv[1];


        linger.l_onoff = 1;
        linger.l_linger = 60;

        if (setsockopt(iofd,
                       SOL_SOCKET,
                       SO_LINGER,
                       (char *)&linger,
                       sizeof(struct linger)) < 0) {
            ls_syslog(LOG_ERR,I18N(5292, "\
%s: setsockopt on socket=<%d> option SO_LINGER failed: %m"), /*catgets 5292 */
                      fname,iofd);
        }


        if (cmdmsg->options & REXF_STDERR) {
            if (setsockopt(err[1],
                           SOL_SOCKET,
                           SO_LINGER,
                           (char *)&linger,
                           sizeof(struct linger)) < 0) {
                ls_syslog(LOG_ERR,I18N(5292, "\
%s: setsockopt on socket=<%d> option SO_LINGER failed: %m"), /*catgets 5292 */
                          fname,err[1]);
            }
        }
        if (cmdmsg->options & REXF_TASKINFO) {
            if (setsockopt(info[1],
                           SOL_SOCKET,
                           SO_LINGER,
                           (char *)&linger,
                           sizeof(struct linger)) < 0) {
                ls_syslog(LOG_ERR,I18N(5292, "\
%s: setsockopt on socket=<%d> option SO_LINGER failed: %m"), /*catgets 5292 */
                          fname,info[1]);
            }
        }
    }


    if (!(sbdFlags & SBD_FLAG_STDIN))
        dup2(iofd, 0);
    if (!(sbdFlags & SBD_FLAG_STDOUT))
        dup2(iofd, 1);
    if (!(sbdFlags & SBD_FLAG_STDERR)) {

        if (cmdmsg->options & REXF_STDERR) {
            dup2(err[1], 2);
        } else {
            dup2(iofd, 2);
        }
    }

    for (i = 1; i < NSIG; i++)
        Signal_(i, SIG_DFL);

    Signal_(SIGHUP, SIG_IGN);


    sigemptyset(&sigMask);
    sigprocmask(SIG_SETMASK, &sigMask, NULL);


    lastUnusedFd = 3;


    maxfds = sysconf(_SC_OPEN_MAX);
    for (i = lastUnusedFd; i < maxfds; i++) {

        if (info == NULL || info[1] != i) {
            close(i);
        }
    }

    ls_closelog();

    putEnv("LSB_ACCT_FILE", resAcctFN);

    if (info) {
        char val[MAXPATHLEN];
        sprintf(val, "%d", info[1]);
        if (putEnv("LSF_PM_SOCKFD", val) != 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "putenv",
                      val);
        }
    }

    lsbJobStarter = NULL;
    execit(cmdmsg->argv,
           lsbJobStarter,
           pid,
           iofd,
           -1,
           (cmdmsg->options & REXF_USEPTY));
    _exit(127);
}

static void
execit(char **uargv,
       char *jobStarter,
       int *pid,
       int stdio,
       int taskSock,
       int loseRoot)
{
    static char   fname[] = "execit()";
    char          *cmd = NULL;
    int           i;
    int           num;
    int           cmdSize;
    char          **real_argv;
    int           uid;

    if (loseRoot) {

        uid = setEUid(0);
        if (lsfSetUid(uid) < 0) {
            perror("setuid");
            resExit_(-1);
        }
    }

    if (!strcmp(uargv[0], RF_SERVERD)) {
        rfServ_(taskSock);
        _exit(127);
    }

    for (num = 0; uargv[num]; num++)
        if (debug > 1 && debug != 2) {
            printf("uargv[%d] = <%s>\n", num, uargv[num]);
            fflush(stdout);
        }

    if (jobStarter && jobStarter[0] != '\0') {

        if (debug > 1) {
            printf("jobStarter=<%s>\n", jobStarter);
            fflush(stdout);
        }
        if (getenv("LSMAKE_SERVER_ID")!=NULL) {
            for (i = 0; uargv[i]; i++)
                ;
            if ((real_argv = (char **)malloc(sizeof(char *)*(i+2)))==NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5222,
                                                 "Not enough memory, job not executed")); /* catgets 5222 */
                return;
            }
            real_argv[0] = jobStarter;
            memcpy((void *)(real_argv+1), (void *)uargv, i*sizeof(char *));
            real_argv[i+1] = NULL;

            lsfExecvp(real_argv[0], real_argv);
            perror(real_argv[0]);
            free (real_argv);
        } else {
            cmdSize = strlen(jobStarter) + 1;
            for (i = 0; uargv[i]; i++) {
                cmdSize += strlen(uargv[i]) + 1;
            }

            cmd = (char *) malloc(cmdSize);

            if (cmd == NULL) {
                perror(_i18n_printf(I18N_FUNC_FAIL, fname, "malloc"));
                resExit_(-1);
            }

            strcpy(cmd, jobStarter);

            for (i = 0; uargv[i]; i++) {
                strcat(cmd, " ");
                strcat(cmd, uargv[i]);
            }


            lsfExecLog(cmd);

            execl("/bin/sh", "sh", "-c", cmd, NULL);
            perror("/bin/sh");

            free(cmd);
            cmd = NULL;
        }
    } else {
        lsfExecvp(uargv[0], uargv);
        perror(uargv[0]);
    }

    fflush(stderr);
    fflush(stdout);

}

static void
delete_client(struct client *cli_ptr)
{
    static char   fname[] = "delete_client()";
    int           i;
    int           j;
    char          exbuf;

    if (debug>1) {
        printf("delete_client\n");
    }

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: client_sock=<%d>, ruid=<%d>, username=<%s>",
                  fname, cli_ptr->client_sock, cli_ptr->ruid,
                  cli_ptr->username);
    }
    if (cli_ptr == NULL)
        return;

    for (i = 0; i < client_cnt; i++) {
        if (clients[i] == cli_ptr)
            break;
    }

    if (i == client_cnt) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5226,
                                         "%s: Deleting nonexistent client"), fname); /* catgets 5226 */
        return;
    }

    TIMEIT(0, cleanUpKeptPids(), "cleanUpKeptPids()");


    for (j = 0; j < child_cnt; j++) {
        if (children[j]->backClnPtr == cli_ptr) {
            children[j]->backClnPtr = (struct client *)NULL;

            if (children[j]->info > 0) {
                close(children[j]->info);
                children[j]->info = -1;
                unlink_child(children[j]);
            }


            if (FD_NOT_VALID(conn2NIOS.sock.fd))
                kill_child(children[j]);

        }
    }

    for (j = i; j < client_cnt-1; j++) {
        clients[j] = clients[j+1];
    }
    client_cnt--;
    clients[client_cnt] = NULL;
    if (client_cnt < MAXCLIENTS_LOWWATER_MARK && ! allow_accept
        && ! child_go) {


        for (;;) {
            if (write(child_res_port, gobuf, strlen(gobuf)) > 0)
                break;
        }
        for (;;) {
            if (read(child_res_port, &exbuf, 1) >= 0)
                break;
        }
        allow_accept = 1;
        close(child_res_port);
    }

    closesocket(cli_ptr->client_sock);

    if (FD_IS_VALID(cli_ptr->client_sock)) {
        FD_CLR(cli_ptr->client_sock, &readmask);
        FD_CLR(cli_ptr->client_sock, &writemask);
        FD_CLR(cli_ptr->client_sock, &exceptmask);
    }

    free(cli_ptr->username);
    if (cli_ptr->eexec.len > 0)
        free(cli_ptr->eexec.data);
    free(cli_ptr->clntdir);
    free(cli_ptr->homedir);
    freeblk(cli_ptr->env);
    free(cli_ptr->hostent.h_name);
    freeblk(cli_ptr->hostent.h_aliases);
    free(cli_ptr);

    return;
}


void
delete_child(struct child *cp)
{
    static char fname[] = "delete_child";
    int i, j;

    if (debug>1) {
        printf("delete_child(%d)\n",cp->rpid);
        fflush(stdout);
    }

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"%s: entering the routine...", fname);

    for (i = 0; i < child_cnt; i++) {
        if (children[i] == cp)
            break;
    }

    if (i == child_cnt)  {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5227,
                                         "%s: Deleting nonexistent child"), fname); /* catgets 5227 */
        return;
    }

    for (j = i; j < child_cnt-1; j++)
        children[j] = children[j+1];
    child_cnt--;

    if (FD_IS_VALID(cp->stdio)) {
        FD_CLR(cp->stdio, &readmask);
        FD_CLR(cp->stdio, &writemask);
        FD_CLR(cp->stdio, &exceptmask);

        CLOSE_IT(cp->stdio);
        cp->std_out.fd = INVALID_FD;

        if (check_valid_tty(cp->slavepty)) {
            uid_t euid = setEUid(0);
            chown(cp->slavepty, 0, 0);
            chmod(cp->slavepty, 0666);
            setEUid(euid);
        }
    }



    if (FD_IS_VALID(cp->std_err.fd)) {
        FD_CLR(cp->std_err.fd, &readmask);
        CLOSE_IT(cp->std_err.fd);
    }

    if (cp->cwd)
        free(cp->cwd);

    if (cp->cmdln)
        freeblk(cp->cmdln);

    if (cp->sigStatRu)
        free(cp->sigStatRu);

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Res has destroyed the child=<%x> current number of child is=<%d>",
                  fname, cp, child_cnt);
    }

    free(cp);

    return;
}

static void
kill_child(struct child *cp)
{
    static char fname[] = "kill_child()";
    if (kill(-(cp->pid), 0) == 0) {

        if (kill(-(cp->pid), SIGTERM) < 0 && errno != ESRCH)
            ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "kill",
                      "SIGTERM", cp->pid);
        sleep(1);
        if (kill(-(cp->pid), SIGKILL) < 0 && errno != ESRCH)
            ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "kill",
                      "SIGKILL", cp->pid);
    }
}

static int
unlink_child(struct child *cp)
{
    int rc = 0;
    static char fname[] = "unlink_child";

    if (debug > 1) {
        printf("unlink_child(%d): refcnt %d\n", cp->rpid, cp->refcnt);
        fflush(stdout);
    }


    if (FD_NOT_VALID(conn2NIOS.sock.fd) && client_cnt == 0)
        kill_child(cp);

    cp->refcnt--;
    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Unlinking child=<%x> reference count decremented to=<%d>",
                  fname, cp, cp->refcnt);
    }
    if (cp->refcnt <= 0 && cp->std_out.buffer.bcount == 0
        && (cp->std_err.buffer.bcount == 0) ) {
        delete_child(cp);
        rc = CHILD_DELETED;
    }
    return rc;
}


static int
notify_client(int s, int rpid, resAck ack, struct sigStatusUsage *sigStatRu)
{
    static char fname[] = "notify_client";
    struct niosStatus st;
    char reqBuf[MSGSIZE];
    int reqBufSize;
    struct LSFHeader reqHdr;
    int rc;
    LS_WAIT_T* status = (LS_WAIT_T*)&sigStatRu->ss;

    if (debug>1) {
        printf("%s(%d): retsock=%d ack=%d\n", fname, rpid, s, ack);
        fflush(stdout);
    }

    memset((char*)reqBuf, 0, sizeof(reqBuf));
    initLSFHeader_(&reqHdr);


    if (sbdMode && !(sbdFlags & SBD_FLAG_TERM)) {
        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res in sbdMode no SBD_FLAG_TERM doesn't need to notify_client", fname);
        }
        return 0;
    }

    if (logclass & LC_TRACE) {

        if (LS_WIFSIGNALED(*status)) {
            int sig =  LS_WTERMSIG(*status);

            ls_syslog(LOG_DEBUG,"\
%s Res notifying Nios by RES2NIOS_STATUS child signaled by sig=<%d>",
                      fname, sig);
        }

        if (LS_WIFSTOPPED(*status)) {
            ls_syslog(LOG_DEBUG,"\
%s Res notifying Nios by RES2NIOS_STATUS child stopped", fname);
        }

        if (LS_WIFEXITED(*status)) {
            int exit_status = LS_WEXITSTATUS(*status);

            ls_syslog(LOG_DEBUG,"\
%s Res notifying Nios by RES2NIOS_STATUS child exited exit_status=<%d>",
                      fname, exit_status);
        }
    }


    if ((sbdMode)
        &&(matchExitVal(LS_WEXITSTATUS(*status), getenv("LSB_EXIT_REQUEUE")))) {
        reqHdr.opCode = RES2NIOS_REQUEUE;
    } else {
        reqHdr.opCode = RES2NIOS_STATUS;
    }

    st.ack = ack;
    if (ack == RESE_SIGCHLD) {
        reqBufSize = MSGSIZE;
        st.s = *sigStatRu;
    } else {
        reqBufSize = sizeof(struct LSFHeader) + sizeof(st.ack);
    }

    reqHdr.reserved = rpid;

    if ((rc = writeEncodeMsg_(s, reqBuf, reqBufSize, &reqHdr,
                              (char *) &st, NB_SOCK_WRITE_FIX, xdr_niosStatus, 0)) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "writeEncodeMsg_");
        return (rc);
    }
    if (debug>1) {
        printf("client is notified for task <%d>\n", rpid);
        fflush(stdout);
    }

    return (0);

}


static void
setptymode(ttyStruct *tts, int slave)


{
    static char fname[] = "setptymode()";
    struct termios termattr;
    int i;

    if (debug > 1)
        ls_syslog(LOG_DEBUG, "setptymode: Entering ...");

    if (tcgetattr(slave, &termattr) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "tcgetattr", "slave");
        return;
    }

    termattr.c_iflag = tts->attr.c_iflag;
    termattr.c_oflag = tts->attr.c_oflag;
    termattr.c_cflag = tts->attr.c_cflag;
    termattr.c_lflag = tts->attr.c_lflag;
    for (i = 0; i < NCCS; i++) {
        termattr.c_cc[i] = tts->attr.c_cc[i];
    }

    if (tcsetattr(slave, TCSANOW, &termattr) < 0) {

        if (errno == EINVAL) {
            ls_syslog(LOG_DEBUG, "%s: tcsetattr: Invalid argument!",fname);
        }else {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "tcsetattr", "slave");
        }
    }

    if (debug > 1)
        ls_syslog(LOG_DEBUG, "setptymode: Leaving");

}


void
dochild_stdio(struct child *chld, int op)
{
    static char fname[] = "dochild_stdio";
    int  cc, i;

    if (debug>1) {
        if (op == DOWRITE) {
            printf("%s: op DOWRITE buffer.bcount %d for task<%d>\n",
                   fname, chld->i_buf.bcount, chld->rpid);
        } else if (op == DOSTDERR) {
            printf("%s: op DOSTDERR buffer.bcount %d for task<%d>\n",
                   fname, chld->std_err.buffer.bcount, chld->rpid);
        } else if (op == DOREAD) {
            printf("%s: op DOREAD buffer.bcount %d for task<%d>\n",
                   fname, chld->std_err.buffer.bcount, chld->rpid);
        }
        fflush(stdout);
    }

    switch (op) {

        case DOREAD:
            child_channel_clear(chld, &(chld->std_out));
            break;


        case DOSTDERR:
            child_channel_clear(chld, &(chld->std_err));
            break;


        case DOWRITE:
            if ((chld->rexflag & REXF_USEPTY) && (chld->i_buf.bcount > 128)) {
                i = 128;
            } else
                i = chld->i_buf.bcount;

            if (i == 0) {
                if (chld->endstdin) {
                    simu_eof(chld, chld->stdio);
                    chld->endstdin = 0;
                }
                break;
            }

            if ((cc = write(chld->stdio, chld->i_buf.bp, i)) > 0) {
                chld->i_buf.bp += cc;
                chld->i_buf.bcount -= cc;
                if (debug > 1)
                    printf("i_buf remains: %d chars \n", chld->i_buf.bcount);
                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: Res wrote <%d> bytes to child=<%x> remaining bytes i_buf.bcount=<%d>",
                              fname, cc, chld, chld->i_buf.bcount);
                }
                if (chld->i_buf.bcount == 0 && chld->endstdin) {
                    chld->endstdin = 0;
                    simu_eof(chld, chld->stdio);
                }
            } else if (cc < 0 && BAD_IO_ERR(errno)) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "write",
                          "chld->stdio",
                          chld->stdio);

                simu_eof(chld, chld->stdio);
            }

            break;

        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5237,
                                             "%s: Unrecognized operation(%d) on child's stdin/out"), fname, op); /* catgets 5237 */
            break;
    }

    return;
}

int
resSignal(struct child *chld, struct resSignal sig)
{
    static char fname[] = "resSignal";
    char *sigBuf1=0;
    char *sigBuf2=0;
    static int firstTime = 1;
    static int sigUnixNTCtrlC, sigUnixNTCtrlB;


    sig.sigval = sig_decode(sig.sigval);

    if ((sbdMode && !(sbdFlags & SBD_FLAG_TERM))) {

        if ((chld->rexflag & REXF_USEPTY)
            && (sig.sigval == SIGTTIN ||
                sig.sigval == SIGTTOU))
            sig.sigval = SIGSTOP;
    }
    else if ((chld->rexflag & REXF_USEPTY)
             && (sig.sigval == SIGTTIN ||
                 sig.sigval == SIGTSTP ||
                 sig.sigval == SIGTTOU)) {
        if (sbdMode && sig.sigval == SIGTSTP  && resFindPamJobStarter())

            sig.sigval = SIGUSR2;
        else
            sig.sigval = SIGSTOP;
    }

    if (firstTime) {
        sigBuf1 = getenv("LSF_NT2UNIX_CTRLC");
        sigBuf2 = getenv("LSF_NT2UNIX_CTRLB");


        if (!sigBuf1)
            sigUnixNTCtrlC = SIGINT;
        else {
            if ( (sigUnixNTCtrlC = getSigVal(sigBuf1)) ==-1 ) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "getSigVal");
                return (-1);
            }
        }
        if (!sigBuf2)
            sigUnixNTCtrlB = SIGQUIT;
        else {
            if ( (sigUnixNTCtrlB = getSigVal(sigBuf2)) ==-1 ) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "getSigVal",
                          "CTRL_BREAK");
                return (-1);
            }
        }
    }



    if (sig.sigval==SIG_NT_CTRLC)
        sig.sigval = sigUnixNTCtrlC;
    else if (sig.sigval==SIG_NT_CTRLBREAK)
        sig.sigval = sigUnixNTCtrlB;


    if (sig.sigval==SIGHUP) {

        sig.sigval = SIGKILL;
    }

    if (kill(-chld->pid, sig.sigval) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_D_FAIL_M, fname, "kill",
                  sig.sigval, chld->pid);
    }
    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Res sent a signal=<%d> to the child=<%x> process group with pid=<%d>",
                  fname, sig.sigval, chld, chld->pid);
    }

    if (sig.sigval == SIGCONT )
        kill(-chld->pid, SIGWINCH);


    firstTime = 0;

    return (0);
}

static void
simu_eof(struct child *chld, int which)

{
    static char fname[] = "simul_eof";
    char tbuf[2];

    if (debug > 1) {
        printf("%s(%d)\n", fname, chld->rpid);
        fflush(stdout);
    }


    if (chld->rexflag & REXF_USEPTY) {
        tbuf[0] = tbuf[1] = chld->c_eof;
        write(chld->stdio, tbuf, 2);
    } else {
        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res is sending EOF to child=<%x> by shutting half connection",
                      fname, chld);
        }
        shutdown(chld->stdio, 1);
        chld->stdin_up = 0;
    }
}

static void
freeblk(char **blk)
{
    char **tmpblk;

    if (!blk)
        return;
    tmpblk = blk;
    while (*tmpblk) {
        free(*tmpblk);
        tmpblk++;
    }
    free(blk);
}

static char **
copyArray(char **from)
{
    char **p;
    int size, i;


    for (size = 0; from[size]; size++)
        ;

    if ((p = (char **) malloc((size + 1) * sizeof(char *))) == NULL)
        return (NULL);

    for (i = 0; i < size; i++) {
        if ((p[i] = putstr_(from[i])) == NULL) {
            for (i--; i >= 0; i--)
                free(p[i]);
            free(p);
            return (NULL);
        }
    }

    p[size] = NULL;
    return (p);

}


void
child_handler(void)
{
    static char fname[] = "child_handler";

    child_handler_ext();

    res_interrupted = 1;
    if (debug > 1) {
        printf("%s: Masks Reset: \n", fname);
        display_masks(&readmask, &writemask, &exceptmask);
    }
}

void
child_handler_ext(void)
{
    static char       fname[] = "child_handler_ext";
    int               pid;
    LS_WAIT_T         status;
    struct rusage     ru;
    int               i;
    int               child_exit;
    int               wait_flags;

    memset( (char*)&ru, 0, sizeof( ru ));

    if (debug>1)
        fputs("SIGCHLD: child_handler_ext\n", stdout);

    if (sbdMode && !(sbdFlags & SBD_FLAG_TERM))
        wait_flags = WNOHANG;
    else
        wait_flags = WNOHANG|WUNTRACED;
    while ((pid=wait3(&status, wait_flags, &ru)) > 0) {
        if(logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: pid=<%d> status=<%x> exitcode=<%d> stopped=<%d> signaled=<%d> \
coredumped=<%d> exited=<%d>",
                      fname, pid, LS_STATUS(status),
                      WEXITSTATUS(status), WIFSTOPPED(status),
                      WIFSIGNALED(status), WCOREDUMP(status),
                      WIFEXITED(status));
        }

        if (debug>1) {
            if (child_res == TRUE)
                printf("child res received a signal\n");
            printf("status of the child <%d> is %x\n", pid, LS_STATUS(status));
            if (WIFSTOPPED(status))
                printf("child stopped\n");
            else if (WIFSIGNALED(status)) {
                printf("child terminated by signal(%d)", WTERMSIG(status));
                if (WCOREDUMP(status)) {
                    printf(" - core dumped");
                }
                printf("\n");
            } else
                printf("child exit(%d)\n", WEXITSTATUS(status));
        }

        {
            for (i = 0; i < child_cnt && children[i]->pid != pid ; )
                i++;

            if (i == child_cnt) {
                continue;
            }
        }

        children[i]->wait = status;
        if ( WIFSTOPPED(status) ) {
            child_exit = 0;
        } else {
            children[i]->running = 0;
            children[i]->stdin_up = 0;
            child_exit = 1;
        }


        if (FD_IS_VALID(children[i]->std_out.fd) && child_exit)
            children[i]->std_out.retry = 1;

        if ( child_exit ) {
            children[i]->sigStatRu->ru = ru;

            if (resLogOn == 1) {
                long mcpuTime = 1000 *(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec)
                    + ru.ru_utime.tv_usec / 1000.0
                    + ru.ru_stime.tv_usec / 1000.0;
                if (debug > 1)
                    ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5288,
                                                       "%s: mcpuTime %lu, resLogcpuTime %d")), /* catgets 5288 */ "child_handler_ext",
                              mcpuTime, resLogcpuTime);


                if (resLogcpuTime == 0 || mcpuTime > resLogcpuTime) {
                    resAcctWrite(children[i]);

                }
            }

            lastChildExitStatus = WEXITSTATUS(status);

            if (sbdMode && !(sbdFlags & SBD_FLAG_TERM)) {
                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: Res in sdbMod no SBD_FLAG_TERM exiting, lastChildExitStatus=<%d>",
                              fname, lastChildExitStatus);
                }
                exit(lastChildExitStatus);
            }
        }

        if (debug > 1)
            printf("Valid stdio fd %d\n", children[i]->stdio);

        children[i]->sigchild = 1;

        if (logclass & LC_TRACE) {
            dumpChild(children[i], -2,
                      "child status inside the child_handler_ext");
        }


        if (FD_NOT_VALID(children[i]->remsock.fd) && client_cnt == 0)
            unlink_child(children[i]);

    }


    if ( pid == -1 && errno == ECHILD ) {
        for (i=0; i<child_cnt; i++)
            if (children[i]->running ) {


                delete_child(children[i]);
            }
    }


    if( pid == 0 && is_resumed == TRUE ) {

        if (sbdMode && (sbdFlags & SBD_FLAG_TERM)) {

            SETSTOPSIG( children[0]->wait, SIGCONT);
            notify_sigchild(children[0]);
        }

        is_resumed = FALSE;
    }


}



static int
notify_sigchild(struct child *cp)
{
    static char fname[] = "notify_sigchild";
    struct sigStatusUsage sstat, *sru;
    int rc;
    LS_WAIT_T *pStatus;

    if (debug > 1) {
        ls_syslog(LOG_DEBUG, "%s: entering", fname);
    }

    sru = (cp->sigStatRu) ? cp->sigStatRu : &sstat;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG, "\
%s: Informing Nios about the gotten SIGCHLD for child=<%d>:%d after <%d> bytes" , fname, cp->rpid, cp->wait, cp->std_out.bytes);
    }


    sru->ss = *(int *)&cp->wait;
    pStatus = (LS_WAIT_T *)&sru->ss;



    if (WIFSIGNALED(*pStatus))
        SETTERMSIG(*pStatus, sig_encode(WTERMSIG(sru->ss)));
    else if (WIFSTOPPED(*pStatus)) {
        SETSTOPSIG(*pStatus, sig_encode(WSTOPSIG(sru->ss)));
    }

    cp->sigchild = 0;
    rc = notify_client(conn2NIOS.sock.fd, cp->rpid, RESE_SIGCHLD, sru);
    cp->sent_status = 1;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Nios notified about child=<%x> RESE_SIGCHILD sigchld=<%d> sent_status=<%d>",
                  fname, cp, cp->sigchild, cp->sent_status);
    }
    if (rc == -2) {
        int i;
        int clientDeleted = FALSE;

        conn2NIOS.sock.fd = INVALID_FD;
        for (i=0; i<child_cnt; i++) {
            children[i]->remsock.fd = INVALID_FD;
            if (!clientDeleted) {
                delete_client(children[i]->backClnPtr);
                clientDeleted = TRUE;
            }
            children[i]->backClnPtr = NULL;
            if (!children[i]->sent_eof) {
                int myself = 0;
                if (children[i] == cp) {
                    myself = 1;
                }
                if (unlink_child(children[i]) == CHILD_DELETED && myself) {
                    rc = CHILD_DELETED;
                }
            }
        }
    }

    if (rc != CHILD_DELETED  && ! (WIFSTOPPED(cp->wait))) {


        if (LS_WSTOPSIG(cp->wait) != SIGCONT) {
            if (unlink_child(cp) == CHILD_DELETED)
                rc = CHILD_DELETED;
        }
    }

    return(rc);

}


void
term_handler(int signum)
{
    static char fname[] = "term_handler";

    if (sbdMode) {
        if (sbdFlags & SBD_FLAG_TERM) {
            int i;

            if (logclass & LC_TRACE) {
                ls_syslog(LOG_DEBUG,"\
%s: Res in sbdMode and SBD_FLAG_TERM is killing signal <%d> chld_cnt=<%d>",
                          fname, signum, child_cnt);
            }

            for (i = 0; i < child_cnt; i++)
                kill(-children[i]->pid, signum);
        }

        return;
    }

    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5252,
                                     "%s: Received signal %d, exiting"), /* catgets 5252 */
              fname, signum);
    exit(0);

}

void
sigHandler(int signum)
{
    static char fname[] = "sigHandler";

    if (sbdMode && (sbdFlags & SBD_FLAG_TERM)) {

        int i, sig;

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res in sbdMode and SBD_FLAG_TERM is propagating signal <%d> chld_cnt=<%d>",
                      fname, signum, child_cnt);
        }

        if( signum == SIGCONT )
            is_resumed = TRUE;

        for (i = 0; i < child_cnt; i++) {
            sig = signum;


            if ((children[i]->rexflag & REXF_USEPTY) &&
                (signum == SIGTTIN || signum == SIGTTOU || signum == SIGTSTP))
                sig = SIGSTOP;

            kill(-children[i]->pid, sig);
        }
    }
}



static void
declare_eof_condition(struct child *childPtr, int which)
{
    static char fname[] = "declare_eof_condition";

    if ( (which < 0) || (which > 2) ) {
        ls_syslog(LOG_ERR, I18N(5101, "\
%s: Invalid input parameter which<%d>"), /* catgets 5101 */
                  fname,which);
    }

    if (debug >=2)
        printf("declare_eof_condition(%d)\n", childPtr->rpid);

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Res declaring EOF with child=<%x> to Nios",
                  fname, childPtr);
    }

    eof_to_nios(childPtr);

    if (check_valid_tty(childPtr->slavepty)) {
        int euid = setEUid(0);
        chown(childPtr->slavepty, 0, 0);
        chmod(childPtr->slavepty, 0666);
        setEUid(euid);
    }

    if (which == 2) {
        CLOSE_IT(childPtr->std_err.fd);

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Nios notified, std_err descriptor with the child=<%x> closed",
                      fname, childPtr);
        }
    } else {
        CLOSE_IT(childPtr->stdio);
        childPtr->std_out.fd = INVALID_FD;

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Nios notified, stdio descriptor with the child=<%x> closed",
                      fname, childPtr);
        }
    }

    unlink_child(childPtr);

}

int
matchExitVal(int val, char* requeueEval)
{
    char *pointer = requeueEval;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_ERR,I18N(5294,"Entering matchExitVal...,val=%d, str=%s"), /*catgets 5294*/
                  val,requeueEval);
    }
    if ((pointer == 0 )||(*pointer == 0)) {
        return 0;
    }

    if ( *pointer == 'E' ) {
        while ( (*pointer != 0) &&  (*pointer != ' ') ) {
            pointer++;
        }
    }
    if ( val == atoi(pointer)) {
        return 1;
    }
    while (*pointer) {

        while ((*pointer)&&(isdigit(*pointer))) {
            pointer++;
        }

        while ((*pointer)&&(!isdigit(*pointer))) {
            pointer++;
        }
        if ((*pointer)&&(val == atoi(pointer))) {
            return 1;
        }
    }
    return 0;
}

static void
eof_to_nios(struct child *chld)
{
    static char fname[] = "eof_to_nios";
    struct LSFHeader reqHdr;
    int rc;

    initLSFHeader_(&reqHdr);;
    if (debug>1)
        printf("eof_to_nios(%d)\n", chld->rpid);


    if (sbdMode && !(sbdFlags & SBD_FLAG_TERM)) {
        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res in sbdMode no SBD_FLAG_TERM doesn't send eof_to_nios",
                      fname);
        }
        return;
    }

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Res is sending RES2NIOS_EOF about child=<%x>", fname, chld);
    }

    reqHdr.opCode = RES2NIOS_EOF;
    reqHdr.version = VOLCLAVA_VERSION;
    reqHdr.reserved = chld->rpid;

    rc = writeEncodeHdr_(conn2NIOS.sock.fd, &reqHdr, NB_SOCK_WRITE_FIX);
    if (rc < 0) {
        int i;
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "writeEncodeHdr_");
        if (rc == -2) {

            conn2NIOS.sock.fd = INVALID_FD;
            for (i=0; i<child_cnt; i++) {
                children[i]->remsock.fd = INVALID_FD;
                if (!children[i]->sent_status)

                    unlink_child(children[i]);
            }
        }
    }
    chld->sent_eof = 1;
}

static void
eof_to_client(struct child *chld)
{
    static char fname[] = "eof_to_client";
    struct LSFHeader reqHdr;

    initLSFHeader_(&reqHdr);;
    if (debug>1)
        printf("eof_to_client\n");



    if (sbdMode && !(sbdFlags & SBD_FLAG_TERM)) {
        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res in sbdMode no SBD_FLAG_TERM doesn't send eof_to_client",
                      fname);
        }
        return;
    }

    reqHdr.opCode = RES_NONRES;
    reqHdr.refCode = chld->rpid;
    reqHdr.length = 0;

    if (debug > 1) {
        printf("Sending EOF to client for task <%d>\n", chld->rpid);
        fflush(stdout);
    }

    if (chld->backClnPtr == NULL)

        return;


    if (writeEncodeHdr_(chld->backClnPtr->client_sock, &reqHdr, SOCK_WRITE_FIX))
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "writeEncodeHdr_");

}

static int
pairsocket(int af, int type, int protocol, int *sv)
{
    static char          fname[] = "pairsocket()";
    struct sockaddr_in   sa;
    struct hostent       *hp;
    int                  s;
    socklen_t            sa_size;
    fd_set               fdReadMask;
    struct timeval       tvTime;
    int                  iRetVal;

#define PAIRSOCKET_TIMEOUT      30

    if (debug > 1)
        printf("pairsocket\n");

    if (af != AF_INET || type != SOCK_STREAM)
        return(-1);

    if ((hp = Gethostbyname_(Myhost)) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: gethostbyname() failed for host %s", __func__, Myhost);
        return -1;
    }
    if ((s = socket(af, type, protocol)) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "malloc", "af");
        return (-1);
    }
    memset((char*)&sa, 0, sizeof(sa));
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_family = AF_INET;
    sa.sin_port = 0;
    if (Bind_(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "Bind_");
        close(s);
        return (-1);
    }
    sa_size = sizeof(sa);
    if (getsockname(s, (struct sockaddr *)&sa, &sa_size) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "getsockname");
        close(s);
        return(-1);
    }

    if (listen(s,1) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "listen");
        close(s);
        return(-1);
    }

    if ((sv[1]=socket(af, type, protocol)) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "socket");
        close(s);
        return (-1);
    }

    memcpy((char *)&sa.sin_addr, hp->h_addr, hp->h_length);
    if (connect(sv[1], (struct sockaddr *)&sa, sizeof(sa)) < 0 ) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "connect");
        close(s);
        close(sv[1]);
        return (-1);
    }

    ls_syslog(LOG_DEBUG, "pairsocket(), connected to passive socket '%d'",
              s);


    FD_ZERO(&fdReadMask);
    FD_SET(s, &fdReadMask);


    tvTime.tv_sec = PAIRSOCKET_TIMEOUT;
    tvTime.tv_usec = 0;

    ls_syslog(LOG_DEBUG, "pairsocket(), begin select(), timeout in '%d' sec.",
              (int)tvTime.tv_sec);


    iRetVal = select(s+1, &fdReadMask, NULL, NULL, &tvTime);

    if (iRetVal == 0) {
        ls_syslog(LOG_DEBUG, "pairsocket(), select() timed out.");

        close(s);
        close(sv[1]);
        return(-1);
    } else if (iRetVal == -1) {

        ls_syslog(LOG_DEBUG, "pairsocket(), select() failed: %m");
        close(s);
        close(sv[1]);
        return(-1);
    } else if (FD_ISSET(s, &fdReadMask)) {
        ls_syslog(LOG_DEBUG, "pairsocket(), select() ok");

        sa_size = sizeof(sa);

        if ((sv[0] = accept(s, (struct sockaddr *)&sa, &sa_size)) < 0) {
            ls_syslog(LOG_DEBUG, I18N_FUNC_FAIL_M, fname, "accept");
            close(s);
            close(sv[1]);
            return (-1);
        }


        close(s);
        return (0);
    }



    ls_syslog(LOG_DEBUG, "pairsocket() unknown return from select, ret=%d",
              iRetVal);
    close(s);
    close(sv[1]);
    return(-1);

}

int
sendReturnCode(int s, int code)
{
    static char fname[] = "sendReturnCode()";
    struct LSFHeader replyHdr;

    initLSFHeader_(&replyHdr);
    replyHdr.opCode = code;
    replyHdr.refCode = currentRESSN;

    if (writeEncodeHdr_(s, &replyHdr, SOCK_WRITE_FIX) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "writeEncodeHdr_");
        return (-1);
    }

    return 0;
}

void
child_channel_clear(struct child *chld, outputChannel *channel)
{
    static char fname[] = "child_channel_clear";
    int cc, len;
    char cvalue;
    RelayLineBuf *buffer = &(channel->buffer);

    if (debug > 1) {
        printf("%s: buffer->bcount=%d\n", fname, buffer->bcount);
        fflush(stdout);
    }


    if (buffer->bcount >= LINE_BUFSIZ)
        return;

    len = LINE_BUFSIZ - buffer->bcount;
    if (len > LINE_BUFSIZ)
        len = LINE_BUFSIZ;

    buffer->bp = BUFSTART(buffer) + buffer->bcount;


    for (;;) {
        cc = read(channel->fd, buffer->bp, len);
        if (cc <0 && errno == EINTR)
            continue;
        break;
    }
    if (cc == 0) {
        if (debug>1)
            ls_syslog(LOG_ERR,I18N(5295, "%s: EOF detected for task <%d>"),/*catgets 5295 */
                      fname, chld->rpid);
        channel->endFlag = 1;
        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res read=<%d> bytes (EOF) from child=<%x> setting endFlag=<%d> invalidating the child",
                      fname, cc, chld, channel->endFlag);
        }

        if (channel->fd == chld->stdio) {
            chld->stdio = INVALID_FD;
        }
        CLOSE_IT(channel->fd);
        return;
    }

    if (cc < 0) {

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"%s: Res read error cc=<%d> %m", fname, cc);
        }

        if (BAD_IO_ERR(errno)) {
            if (debug>1)
                ls_syslog(LOG_ERR, I18N(5296,"\
%s: Read error on channel->fd<%d>: %m: EOF assumed"), /*catgets 5296 */
                          fname, channel->fd);
            channel->endFlag = 1;

            if (logclass & LC_TRACE) {
                ls_syslog(LOG_DEBUG,"\
%s: Res read=<%d> from child=<%x>, BAD_IO_ERROR assuming EOF endFlag=<%d>",
                          fname, cc, chld, channel->endFlag);
            }


            if (channel->fd == chld->stdio) {
                chld->stdio = INVALID_FD;
            }
            CLOSE_IT(channel->fd);
            return;
        }


        if (!chld->running && kill(-chld->pid, 0) < 0) {

            millisleep_(50);
            cc = read(channel->fd, buffer->bp, len);
            if (cc < 0) {
                if (debug > 1)
                    ls_syslog(LOG_ERR, I18N(5297,"\
%s: final read failed, assuming EOF: %m"),fname); /* catgets 5297 */
                channel->endFlag = 1;

                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: Res can't flush child=<%x> read=<%d> assuming EOF, endFlag=<%d> %m",
                              fname, chld, cc, channel->endFlag);
                }


                if (channel->fd == chld->stdio) {
                    chld->stdio = INVALID_FD;
                }
                CLOSE_IT(channel->fd);
                return;
            }
            if (cc == 0) {
                if (debug > 1)
                    ls_syslog(LOG_ERR, I18N(5298,
                                            "%s: EOF in final read"), /*catgets 5298 */
                              fname);
                channel->endFlag = 1;

                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: Res flushed child=<%x> read=<%d> EOF, endFlag=<%d>",
                              fname, chld, cc, channel->endFlag);
                }


                if (channel->fd == chld->stdio) {
                    chld->stdio = INVALID_FD;
                }
                CLOSE_IT(channel->fd);
                return;
            }

            channel->retry = 1;

            if (logclass & LC_TRACE) {
                ls_syslog(LOG_DEBUG,"\
%s: Res got some data from child=<%x> read=<%d>, retry=<%d>",
                          fname, chld, cc, channel->retry);
            }

        } else {


            if (logclass & LC_TRACE) {
                ls_syslog(LOG_DEBUG,"\
%s: Res read=<%d> bytes from child=<%x> but process group is not empty, keep on trying",
                          fname, cc, chld);
            }

            return;
        }
    }

    if (debug>1) {
        printf("%s: read %d chars from channel\n", fname, cc);
        fflush(stdout);
    }


    buffer->bcount += cc;

    channel->bytes += cc;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Res read=<%d> bytes from child=<%x> bytes=<%d> buffer->bcount=<%d>",
                  fname, cc, chld, channel->bytes,
                  buffer->bcount);
    }


    if ( (chld->rexflag & REXF_USEPTY)
         && (channel != &(chld->std_err)) ) {
        int i;


        cvalue = *(buffer->bp);

        buffer->bcount--;
        for (i = 0; i < cc - 1; ++i)
            buffer->bp[i] = buffer->bp[i + 1];

        if (cvalue != TIOCPKT_DATA) {
            if (debug>1) {
                printf("received PTY packet mode control char (%x)\n", cvalue);
            }

            return;
        }
    }

    return;

}

static void
setlimits (struct lsfLimit *lsfLimits)
{
    mysetlimits(lsfLimits);
}
static void
mysetlimits (struct lsfLimit *lsfLimits)
{
    struct rlimit rlimit;

#ifdef  RLIMIT_CPU
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_CPU], &rlimit, LSF_RLIMIT_CPU);
    if (resParams[LSF_RES_RLIMIT_UNLIM].paramValue != NULL)
        if (strstr(resParams[LSF_RES_RLIMIT_UNLIM].paramValue, "cpu") != NULL)
            rlimit.rlim_max = RLIM_INFINITY;
    SET_RLIMIT(RLIMIT_CPU, rlimit,LOG_ERR);
    ls_syslog(LOG_DEBUG, "CPU limit, max=%d, min=%d",
              rlimit.rlim_max, rlimit.rlim_cur);
#endif

#ifdef  RLIMIT_FSIZE
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_FSIZE], &rlimit, LSF_RLIMIT_FSIZE);
    if (resParams[LSF_RES_RLIMIT_UNLIM].paramValue != NULL)
        if (strstr(resParams[LSF_RES_RLIMIT_UNLIM].paramValue, "fsize") != NULL)
            rlimit.rlim_max = RLIM_INFINITY;
    SET_RLIMIT(RLIMIT_FSIZE, rlimit,LOG_ERR);
#endif

#ifdef  RLIMIT_DATA
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_DATA], &rlimit, LSF_RLIMIT_DATA);
    if (resParams[LSF_RES_RLIMIT_UNLIM].paramValue != NULL)
        if (strstr(resParams[LSF_RES_RLIMIT_UNLIM].paramValue, "data") != NULL)
            rlimit.rlim_max = RLIM_INFINITY;

    SET_RLIMIT(RLIMIT_DATA, rlimit,LOG_ERR);
#endif

#ifdef  RLIMIT_STACK
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_STACK], &rlimit, LSF_RLIMIT_STACK);
    if (resParams[LSF_RES_RLIMIT_UNLIM].paramValue != NULL)
        if (strstr(resParams[LSF_RES_RLIMIT_UNLIM].paramValue, "stack") != NULL)
            rlimit.rlim_max = RLIM_INFINITY;

    SET_RLIMIT(RLIMIT_STACK, rlimit,LOG_ERR);
#endif

#ifdef  RLIMIT_CORE
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_CORE], &rlimit, LSF_RLIMIT_CORE);
    if (resParams[LSF_RES_RLIMIT_UNLIM].paramValue != NULL)
        if (strstr(resParams[LSF_RES_RLIMIT_UNLIM].paramValue, "core") != NULL)
            rlimit.rlim_max = RLIM_INFINITY;
    SET_RLIMIT(RLIMIT_CORE, rlimit,LOG_ERR);
#endif

#ifdef  RLIMIT_RSS
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_RSS], &rlimit, LSF_RLIMIT_RSS);
    SET_RLIMIT(RLIMIT_RSS, rlimit,LOG_ERR);
#endif

#ifdef  RLIMIT_NOFILE
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_NOFILE], &rlimit, LSF_RLIMIT_NOFILE);
    SET_RLIMIT(RLIMIT_NOFILE, rlimit,LOG_ERR);
#endif

#ifdef RLIMIT_OPEN_MAX
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_OPEN_MAX], &rlimit, LSF_RLIMIT_OPEN_MAX);
    SET_RLIMIT(RLIMIT_OPEN_MAX, rlimit,LOG_ERR);
#endif

#ifdef RLIMIT_VMEM
    rlimitDecode_(&lsfLimits[LSF_RLIMIT_VMEM], &rlimit, LSF_RLIMIT_VMEM);
    if (resParams[LSF_RES_RLIMIT_UNLIM].paramValue != NULL)
        if (strstr(resParams[LSF_RES_RLIMIT_UNLIM].paramValue, "vmem") != NULL)
            rlimit.rlim_max = RLIM_INFINITY;
    SET_RLIMIT(RLIMIT_VMEM, rlimit,LOG_ERR);
#endif

}


static int
setCliEnv(struct client *cl, char *envName, char *value)
{
    int cnt;
    char *cp, *sp;
    char buf[MAXLINELEN];

    for (cnt = 0; cl->env[cnt]; cnt++) {
        if (strncmp (envName, cl->env[cnt],  strlen(envName)) == 0) {
            strcpy (buf, cl->env[cnt]);
            cp = buf;
            if ((sp = strstr (buf, "="))  == NULL)
                return (-1);
            *sp = '\0';
            if (strncmp (buf, envName, strlen(cp)) == 0) {

                FREEUP (cl->env[cnt]);
                sprintf(buf, "%s=%s", envName, value);
                if ((cl->env[cnt] = putstr_(buf)) == NULL)
                    return (-1);
                return (0);
            }
        }
    }
    return (addCliEnv (cl, envName, value));

}


static
int addCliEnv(struct client *cl, char *envName, char *value)
{
    int cnt;
    char **env;
    char buf[MAXLINELEN];

    for (cnt = 0; cl->env[cnt]; cnt++);

    if ((env = (char **) realloc(cl->env, (cnt + 2) * sizeof(char *)))
        == NULL)
        return (-1);

    cl->env = env;
    sprintf(buf, "%s=%s", envName, value);
    if ((cl->env[cnt] = putstr_(buf)) == NULL)
        return (-1);
    cl->env[cnt+1] = NULL;
    return (0);
}


int
lsbJobStart(char **jargv, u_short retPort, char *host, int usePty)
{
    static char fname[] = "lsbJobStart()";
    struct resCmdBill cmdBill;
    char *pwd;
    struct client cli;
    resAck ack;
    int retsock;
    struct child *child;
    struct sockaddr_in from;
    struct hostent *hp;
    char *jf, *jfn, *jidStr;
    int jobId, i;
    sigset_t sigMask, oldSigMask;
    int exitStatus = 0;
    int terWhiPendStatus = 0;
    char *stderrSupport = NULL;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG,"\
%s: Starting a batch job", fname);
    }


    sigemptyset(&sigMask);
    sigaddset(&sigMask, SIGCHLD);
    sigaddset(&sigMask, SIGINT);
    sigaddset(&sigMask, SIGHUP);
    sigaddset(&sigMask, SIGPIPE);
    sigaddset(&sigMask, SIGTTIN);
    sigaddset(&sigMask, SIGTTOU);
    sigaddset(&sigMask, SIGTSTP);
#ifdef SIGDANGER
    sigaddset(&sigMask, SIGDANGER);
#endif
    sigaddset(&sigMask, SIGTERM);
#ifdef SIGXCPU
    sigaddset(&sigMask, SIGXCPU);
#endif
#ifdef SIGXFSZ
    sigaddset(&sigMask, SIGXFSZ);
#endif
#ifdef SIGPROF
    sigaddset(&sigMask, SIGPROF);
#endif
#ifdef SIGLOST
    sigaddset(&sigMask, SIGLOST);
#endif
    sigaddset(&sigMask, SIGUSR1);
    sigaddset(&sigMask, SIGUSR2);
#ifdef SIGABRT
    sigaddset(&sigMask, SIGABRT);
#endif
    sigprocmask(SIG_BLOCK, &sigMask, &oldSigMask);

    jf = getenv("LSB_JOBFILENAME");
    if (jf == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "getenv","LSB_JOBFILENAME");
    } else {
        resLogOn = 1;
        resLogcpuTime = 0;
        jfn = strrchr(jf, '/');
        if (jfn) {
            sprintf(resAcctFN, "%s/.%s.acct", LSTMPDIR, jfn + 1);
        }
    }

    if (resParams[LSF_ENABLE_PTY].paramValue &&
        !strcasecmp(resParams[LSF_ENABLE_PTY].paramValue, "n"))
        usePty = 0;

    if (usePty)
        cmdBill.options = REXF_USEPTY;
    else
        cmdBill.options = 0;


    cmdBill.retport = htons(retPort);
    cmdBill.rpid = 0;

    if (sbdFlags & SBD_FLAG_TERM) {
        cmdBill.rpid = 1;
        if (getenv("LSB_SHMODE"))
            cmdBill.options |= REXF_SHMODE;
    }

    if ((pwd = getenv("PWD")))
        strcpy(cmdBill.cwd, pwd);
    else
        cmdBill.cwd[0] = '\0';

    cmdBill.argv = jargv;

    jidStr = getenv("LSB_REMOTEJID");
    if (jidStr != NULL)

        jobId = atoi(jidStr);
    else {
        jidStr = getenv("LSB_JOBID");
        if (jidStr == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "getenv", "LSB_JOBID");
            resExit_(-1);
        }
        jobId = atoi(jidStr);
    }


    stderrSupport = getenv("LSF_INTERACTIVE_STDERR");
    if ( (stderrSupport != NULL)
         && (strcasecmp(stderrSupport, "y") == 0) ) {
        cmdBill.options |= REXF_STDERR;
    }



    memset((char *) &cli, 0, sizeof(struct client));

    if ((cli.username = getenv("LSFUSER")) == NULL) {

        if ((cli.username = getenv("USER")) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "getenv", "USER");
            resExit_(-1);
        }
    }

    if (usePty) {
        uid_t uid;

        if (getOSUid_(cli.username, &uid) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_MM, fname, "getOSUid_",
                      cli.username);
            resExit_(-1);
        }
        cli.ruid = uid;
    } else {
        cli.ruid = getuid();
    }

    cli.gid = getgid();
    cli.clntdir = getenv("PWD");
    cli.homedir = getenv("HOME");
    cli.tty = defaultTty;
    cli.env = environ;
    cli.hostent.h_name = host;

    if (sbdFlags & SBD_FLAG_TERM) {

        if ((hp = Gethostbyname_(host)) == NULL) {
            ls_syslog(LOG_ERR, "\
%s: gethostbyname() failed for host %s", host);
            resExit_(-1);
        }

        memcpy((char *) &from.sin_addr,
               (char *) hp->h_addr,
               (int) hp->h_length);

        from.sin_family = AF_INET;

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Calling back to Nios retport=<%d> rpid=<%d>",
                      fname, cmdBill.retport, jobId);
        }

        if ((retsock = niosCallback_(&from, cmdBill.retport,
                                     jobId, exitStatus, terWhiPendStatus)) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, fname, "niosCallback_",
                      retPort);
            resExit_(-1);
        }

        conn2NIOS.sock.fd = retsock;

        if (usePty) {
            if (setEUid(cli.ruid) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, fname, "setEUid",
                          cli.ruid);
                resExit_(-1);
            }

            if (ttyCallback(retsock,  &cli.tty) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "ttyCallback");
                resExit_(-1);
            }
        }
    } else {

        retsock = INVALID_FD;
        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: Res no niosCallback_() since no tty suport requested",
                      fname);
        }
    }

    if ((child = doRexec(&cli, &cmdBill, retsock, 0, FALSE, &ack)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, fname, "doRexec", ack);
        resExit_(-1);
    }

    child->backClnPtr = NULL;

    allow_accept = FALSE;
    child_res = TRUE;
    child_go = TRUE;

    if (!(sbdFlags & SBD_FLAG_TERM)) {
        int maxfds = sysconf(_SC_OPEN_MAX);
        int lastUnusedFd;


        lastUnusedFd = 3;


        for (i = lastUnusedFd; i < maxfds; i++) {

            if (child->info != i) {
                close(i);
            }
        }
        child->stdio = INVALID_FD;
        child->std_out.fd = INVALID_FD;
        child->std_err.fd = INVALID_FD;
    }


    sigprocmask(SIG_SETMASK, &oldSigMask, NULL);
    return (0);

}



static int
ttyCallback(int s, ttyStruct *tty)
{
    static char fname[] = "ttyCallback()";
    XDR xdrs;
    struct LSFHeader msgHdr;
    char hdrBuf[sizeof(struct LSFHeader)];
    char *buf;
    int cc;
    struct resStty restty;

    xdrmem_create(&xdrs, hdrBuf, sizeof(struct LSFHeader), XDR_DECODE);
    if ((cc = readDecodeHdr_(s, hdrBuf, SOCK_READ_FIX, &xdrs, &msgHdr)) < 0) {
        ls_syslog(LOG_DEBUG, "ttyCallback: readDecodeHdr failed cc=%d: %M",
                  cc);
        xdr_destroy(&xdrs);
        return (-1);
    }
    xdr_destroy(&xdrs);

    if (msgHdr.length) {
        buf = malloc(msgHdr.length);
        if (!buf) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            return (-1);
        }

        if ((cc = SOCK_READ_FIX(s, buf, msgHdr.length)) != msgHdr.length) {
            ls_syslog(LOG_DEBUG,
                      "ttyCallback: b_read_fix(%d) failed, cc=%d: %m",
                      msgHdr.length, cc);
            free(buf);
            return (-1);
        }
    } else {
        buf = NULL;
    }

    switch (msgHdr.opCode) {
        case RES_INITTTY_ASYNC:
        case RES_INITTTY:
            xdrmem_create(&xdrs, buf, XDR_DECODE_SIZE_(msgHdr.length), XDR_DECODE);
            if (!xdr_resStty(&xdrs, &restty, &msgHdr)) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resStty");
                xdr_destroy(&xdrs);
                return (-1);
            }
            xdr_destroy(&xdrs);
            free(buf);
            tty->attr = restty.termattr;
            tty->ws = restty.ws;

            return (0);

        default:
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5278,
                                             "%s: unknown opCode=%d"), fname, msgHdr.opCode); /* catgets 5278 */
    }
    if (buf)
        free(buf);

    return (-1);
}


void doDebugReq();

static void
resDebugReq(struct client *cli_ptr, struct LSFHeader *msgHdr, XDR *xdrs,
            int childSock)
{
    static char fname[] = "resDebugReq";
    struct debugReq debugReq;
    resAck ack;
    char *dir=NULL;
    char logFileName[MAXLSFNAMELEN];
    char lsfLogDir[MAXPATHLEN];
    char dynDbgEnv[MAXPATHLEN];

    memset(logFileName, 0, sizeof(logFileName));
    memset(lsfLogDir, 0, sizeof(lsfLogDir));

    if (debug > 1 )
        printf ("%s: Entering this routine...Pid is %d \n", fname, (int)getpid());
    if (!xdr_debugReq(xdrs, &debugReq, msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_debugReq");
        sendReturnCode(cli_ptr->client_sock, RESE_REQUEST);
        return;
    }

    if (cli_ptr) {

        ack = sendResParent(msgHdr, (char *) &debugReq, xdr_debugReq);
        sendReturnCode(cli_ptr->client_sock, ack);
        return;
    }

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG, "New debug is: opCode =%d , class=%x, level=%d, options=%d,filename=%s ", debugReq.opCode, debugReq.logClass, debugReq.level, debugReq.options, debugReq.logFileName);

    if (((dir=strrchr(debugReq.logFileName,'/')) != NULL) ||
        ((dir=strrchr(debugReq.logFileName,'\\')) != NULL)) {
        dir++;
        ls_strcat(logFileName, sizeof(logFileName),dir);
        *(--dir)='\0';
        ls_strcat(lsfLogDir, sizeof(lsfLogDir), debugReq.logFileName);
    }
    else {
        ls_strcat(logFileName, sizeof(logFileName), debugReq.logFileName);

        if ( resParams[LSF_LOGDIR].paramValue
             && *(resParams[LSF_LOGDIR].paramValue)) {
            ls_strcat(lsfLogDir, sizeof(lsfLogDir),
                      resParams[LSF_LOGDIR].paramValue);
        } else {
            lsfLogDir[0] = '\0';
        }
    }
    if (debugReq.options == 1 ) {
        doReopen();


        cleanDynDbgEnv();
    }
    else if (debugReq.opCode == RES_DEBUG) {
        putMaskLevel(debugReq.level, &(resParams[LSF_LOG_MASK].paramValue));

        if (debugReq.logClass >= 0) {
            logclass = debugReq.logClass;


            sprintf(dynDbgEnv, "%d", logclass);
            putEnv("DYN_DBG_LOGCLASS", dynDbgEnv);
        }

        if ( debugReq.level>=0 || debugReq.logFileName[0] != '\0') {


            closelog();

            ls_openlog(logFileName,
                       lsfLogDir, (debug > 1),
                       resParams[LSF_LOG_MASK].paramValue);
            ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5289, "Open a new log file!")));  /* catgets 5289 */


            putEnv("DYN_DBG_LOGDIR", lsfLogDir);
            putEnv("DYN_DBG_LOGFILENAME", logFileName);
            sprintf(dynDbgEnv, "%d", debugReq.level);
            putEnv("DYN_DBG_LOGLEVEL", dynDbgEnv);
        }
    }
    else {

        if ( debugReq.level >= 0)
            timinglevel = debugReq.level;
        if (debugReq.logFileName[0] != '\0') {
            closelog();

            ls_openlog(logFileName,
                       lsfLogDir, (debug > 1),
                       resParams[LSF_LOG_MASK].paramValue);
            ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5289, "Open a new log file ! ")));
        }
    }

    sendReturnCode(childSock, RESE_OK);


    return;

}



static void
doReopen(void)
{
    static char fname[] = "doReopen";
    struct config_param *plp;
    char *sp;
    char *pathname = NULL;

    for (plp = resParams; plp->paramName != NULL; plp++) {
        if (plp->paramValue != NULL)
            FREEUP(plp->paramValue);
    }

    if ((pathname = getenv("LSF_ENVDIR")) == NULL)
        pathname = "/etc";

    if (initenv_(resParams, pathname) < 0) {
        if ((sp = getenv("LSF_LOGDIR")) != NULL)
            resParams[LSF_LOGDIR].paramValue = sp;
        ls_openlog("res", resParams[LSF_LOGDIR].paramValue, (debug > 1),
                   resParams[LSF_LOG_MASK].paramValue);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_MM, fname, "initenv_", pathname);
        resExit_(-1);
    }

    getLogClass_(resParams[LSF_DEBUG_RES].paramValue,
                 resParams[LSF_TIME_RES].paramValue);

    closelog();

    if (debug > 1)
        ls_openlog("res", resParams[LSF_LOGDIR].paramValue, TRUE, "LOG_DEBUG");
    else
        ls_openlog("res", resParams[LSF_LOGDIR].paramValue, FALSE,
                   resParams[LSF_LOG_MASK].paramValue);

    if (logclass & (LC_TRACE | LC_HANG))
        ls_syslog(LOG_DEBUG, "doReopen: logclass=%x",  logclass);

    return;

}


static
uid_t setEUid(uid_t uid)
{
    uid_t myuid;
    int errnoSv = errno;

    if (debug)
        return(geteuid());

    if ((myuid = geteuid()) == uid)
        return (myuid);


    if (myuid != 0 && uid != 0)
        changeEUid(0);
    changeEUid(uid);
    errno = errnoSv;
    return (myuid);
}

static void
dumpResCmdBill(struct resCmdBill* bill)
{
    static char fname[] = "dumpResCmdBill()";
    char**      args;

    ls_syslog(LOG_DEBUG,"\
%s: retport=<%d>, rpid=<%d>, priority=<%d>, filemask=<%d>,\
options=<%d>, cwd=<%s>",
              fname, bill->retport, bill->rpid, bill->priority,
              bill->filemask, bill->options, bill->cwd);

    args = bill->argv;
    while (*args != NULL) {
        ls_syslog(LOG_DEBUG,"%s: argv=<%s>", fname, *args);
        args++;
    }

}

static int
changeEUid (uid_t uid)
{
    static char fname[] = "changeEUid()";
    if (lsfSetEUid(uid) < 0) {
        ls_syslog(LOG_WARNING, I18N_FUNC_D_FAIL_M, fname, "setresuid/seteuid",
                  (int)uid);
        return -1;
    }

    if (uid == 0) {
        if(lsfSetREUid(0, 0) < 0)
        {
            ls_syslog(LOG_WARNING, I18N_FUNC_D_FAIL_M, fname,
                      "setresuid/seteuid",
                      (int)uid);
            return -1;
        }
    }
    return 0;
}


void
dumpClient(struct client* client, char* why)
{
    static char fname[] = "dumpClient()";

    ls_syslog(LOG_DEBUG,"\
%s: %s: Client=<%x> socket=<%d> ruid=<%d> username=<%s> from=<%s>",
              fname, why, client,
              client->client_sock, client->ruid,
              client->username, client->hostent.h_name);
}
void
dumpChild(struct child* child, int operation, char* why)
{
    static char fname[] = "dumpChild()";

    ls_syslog(LOG_DEBUG,"\
%s: %s: Operation=<%d> on child=<%x> child->pid=<%d> child->rpid=<%d> backClient=<%x> refcnt=<%d> stdio=<%d> remsock.fd=<%d> remsock.rcount/remsock.wcount=<%d/%d> rexflag=<%d> server=<%d> c_eof=<%d> running=<%d> sigchild=<%d> endstdin=<%d> i_buf.bcount=<%d> std_out.endFlag=<%d> std_out.retry=<%d> std_out.buffer.bcount=<%d> std_out.bytes=<%d> std_err.endFlag=<%d> std_err.retry=<%d> std_err.buffer.bcount=<%d> std_err.bytes=<%d> sent_eof=<%d> sent_status=<%d> child->username=<%s> child->fromhost=<%s> ",
              fname, why, operation,
              child,
              child->pid,
              child->rpid,
              child->backClnPtr,
              child->refcnt,
              child->stdio,
              child->remsock.fd,
              child->remsock.rcount,
              child->remsock.wcount,
              child->rexflag,
              child->server,
              child->c_eof,
              child->running,
              child->sigchild,
              child->endstdin,
              child->i_buf.bcount,
              child->std_out.endFlag,
              child->std_out.retry,
              child->std_out.buffer.bcount,
              child->std_out.bytes,
              child->std_err.endFlag,
              child->std_err.retry,
              child->std_err.buffer.bcount,
              child->std_err.bytes,
              child->sent_eof,
              child->sent_status,
              child->username,
              child->fromhost);
}

void
dochild_buffer(struct child *chld, int op)
{
    static char fname[] = "dochild_buffer";
    static int  linebuf = -1;

    int  i;
    outputChannel *channel = NULL;

    if (debug>1) {
        if (op == DOSTDERR) {
            printf("%s: DOSTDERR bcount %d for task<%d>\n",
                   fname, chld->std_err.buffer.bcount, chld->rpid);
        } else if (op == DOWRITE) {
            printf("%s: DOWRITE bcount %d for task<%d>\n",
                   fname, chld->std_out.buffer.bcount, chld->rpid);
        } else if (op == DOREAD) {
            printf("%s: DOREAD bcount %d for task<%d>\n",
                   fname, conn2NIOS.sock.rbuf->bcount, chld->rpid);
        }
        fflush(stdout);
    }

    switch (op) {

        case DOREAD:

            if (chld->i_buf.bcount > 0)
                return;

            if (conn2NIOS.rtag > 0) {
                if (conn2NIOS.rtag == chld->rpid) {
                    chld->i_buf.bp = BUFSTART(&(chld->i_buf));
                    conn2NIOS.sock.rbuf->bp = BUFSTART(conn2NIOS.sock.rbuf);
                    memcpy((char *) chld->i_buf.bp,
                           (char *) conn2NIOS.sock.rbuf->bp,
                           conn2NIOS.sock.rbuf->bcount);
                    chld->i_buf.bcount = conn2NIOS.sock.rbuf->bcount;


                    conn2NIOS.sock.rbuf->bcount = 0;
                    if ( conn2NIOS.sock.rcount == 0) {
                        conn2NIOS.rtag = -1;
                    }
                    conn2NIOS.num_duped = 0;
                }
            }
            else if (conn2NIOS.rtag == 0) {
                int j, *rpids;

                for (i=0; i<conn2NIOS.num_duped; i++) {
                    if (conn2NIOS.task_duped[i] == chld->rpid)
                        return;
                }

                chld->i_buf.bp = BUFSTART(&(chld->i_buf));
                conn2NIOS.sock.rbuf->bp = BUFSTART(conn2NIOS.sock.rbuf);
                memcpy((char *) chld->i_buf.bp, (char *) conn2NIOS.sock.rbuf->bp,
                       conn2NIOS.sock.rbuf->bcount);
                chld->i_buf.bcount = conn2NIOS.sock.rbuf->bcount;


                conn2NIOS.task_duped[conn2NIOS.num_duped] = chld->rpid;
                conn2NIOS.num_duped++;


                rpids = conn2NIOS.task_duped;
                for (i=0; i<child_cnt; i++) {
                    if (FD_NOT_VALID(children[i]->stdio))
                        continue;
                    for (j=0; j<conn2NIOS.num_duped; j++)
                        if (children[i]->rpid == rpids[j])
                            break;
                    if (j >= conn2NIOS.num_duped)
                        break;
                }
                if (i >= child_cnt) {
                    conn2NIOS.sock.rbuf->bcount = 0;
                    if ( conn2NIOS.sock.rcount == 0) {
                        conn2NIOS.rtag = -1;
                    }
                    conn2NIOS.num_duped = 0;
                }
                if (debug > 1) {
                    printf("%s: child_cnt=%d num_duped=%d bcount=%d\n",
                           fname, child_cnt, conn2NIOS.num_duped,
                           conn2NIOS.sock.rbuf->bcount);
                    fflush(stdout);
                }
            }
            break;


        case DOWRITE:
        case DOSTDERR:

            if (op == DOSTDERR) {
                channel = &(chld->std_err);
            } else {
                channel = &(chld->std_out);
            }


            if (conn2NIOS.sock.wbuf->bcount > 0)
                return;

            {

                if (channel->buffer.bcount == 0) {
                    int delchild = 0;

                    if (chld->sigchild && !chld->server) {
                        delchild = notify_sigchild(chld);
                        if (debug > 1) {
                            printf("sigchild is delivered for task <%d>\n",
                                   chld->rpid);
                            fflush(stdout);
                        }
                    }

                    if (delchild == 0 && channel->endFlag == 1) {
                        channel->endFlag = 0;
                        if (op == DOSTDERR)  {
                            declare_eof_condition(chld, 2);
                        } else {
                            declare_eof_condition(chld, 1);
                        }
                    }
                    return;

                }
                else if (channel->buffer.bcount > 0) {
                    char *p;
                    int bcount;
                    p = channel->buffer.bp = BUFSTART(&(channel->buffer));
                    conn2NIOS.sock.wbuf->bp = BUFSTART(conn2NIOS.sock.wbuf);
                    bcount = channel->buffer.bcount;
                    if ( linebuf == -1 ) {

                        linebuf = 1;
                        if ( !resParams[LSF_RES_NO_LINEBUF].paramValue ) {
                            linebuf = 1;
                        }
                        else if ( strcasecmp(resParams[LSF_RES_NO_LINEBUF].paramValue, "y") == 0) {
                            linebuf = 0;
                        }
                    }


                    if (!(chld->rexflag & REXF_USEPTY)
                        && !sbdMode && linebuf == 1 ) {
                        p += bcount;
                        for (i=bcount-1; i>=0; i--) {
                            p--;
                            if (*p == '\n') break;
                        }
                        if (i >= 0) {
                            p++;
                            bcount = i + 1;
                        }
                        else {
                            if (debug > 1) {
                                p = channel->buffer.bp;
                                printf("%s: leftover: \"", fname);
                                for (i=0; i<channel->buffer.bcount; i++)
                                    printf("%c", p[i]);
                                printf("\"\n");
                                fflush(stdout);
                            }
                            if (bcount < LINE_BUFSIZ && !channel->endFlag)
                                return;
                        }
                    }

                    memcpy((char *) conn2NIOS.sock.wbuf->bp,
                           (char *) channel->buffer.bp,
                           bcount);
                    conn2NIOS.sock.wbuf->bcount = bcount;
                    channel->buffer.bcount -= bcount;
                    conn2NIOS.wtag = chld->rpid;
                    if (channel->buffer.bcount > 0) {

                        for (i=0; i < channel->buffer.bcount; i++)
                            channel->buffer.bp[i] = p[i];
                    }


                    if (op == DOSTDERR) {
                        conn2NIOS.sock.opCode = RES2NIOS_STDERR;
                    } else {
                        conn2NIOS.sock.opCode = RES2NIOS_STDOUT;
                    }

                    if (debug > 1) {
                        printf("%d bytes moved to wbuf, %d bytes left\n",
                               bcount, channel->buffer.bcount);
                        fflush(stdout);
                    }
                }
            }
            break;

        default:
            break;
    }

    return;
}

void
donios_sock(struct child **children, int op)
{
    static char fname[] = "donios_sock";
    int  cc, i;

    if (debug > 1) {
        if (op == DOWRITE)
            printf("%s(%d): DOWRITE wcount=%d bcount=%d\n", fname,
                   conn2NIOS.wtag,
                   conn2NIOS.sock.wcount, conn2NIOS.sock.wbuf->bcount);
        else
            printf("%s(%d): DOREAD rcount=%d bcount=%d\n", fname,
                   children[0]->rpid,
                   conn2NIOS.sock.rcount, conn2NIOS.sock.rbuf->bcount);
        fflush(stdout);
    }

    switch (op) {
        case DOREAD:

            if (conn2NIOS.sock.rcount == 0) {
                struct LSFHeader msgHdr, bufHdr;
                struct resSignal sig;
                XDR xdrs;
                char buf[MSGSIZE];
                int rtag, rc;

                xdrmem_create(&xdrs, (char *) &bufHdr,
                              sizeof(struct LSFHeader), XDR_DECODE);

                if (readDecodeHdr_(conn2NIOS.sock.fd, (char *) &bufHdr,
                                   NB_SOCK_READ_FIX, &xdrs, &msgHdr) < 0) {

                    conn2NIOS.sock.fd = INVALID_FD;
                    for (i=0; i < child_cnt; i++) {
                        children[i]->remsock.fd = INVALID_FD;
                        unlink_child(children[i]);
                    }
                    xdr_destroy(&xdrs);

                    if (child_cnt > 0)
                        ls_syslog(LOG_DEBUG, "%s: Read package head failed: %M",
                                  fname);
                    return;
                }
                xdr_destroy(&xdrs);

                rtag = 0;

                switch(msgHdr.opCode) {
                    case NIOS2RES_STDIN:
                        if (logclass & LC_TRACE) {
                            ls_syslog(LOG_DEBUG,"\
%s: NIOS2RES_STDIN message with tag <%d>", fname, rtag);
                        }
                        if (debug > 1)
                            printf("Received SIDIN for task <%d>\n", rtag);
                        conn2NIOS.rtag = rtag;
                        conn2NIOS.sock.rcount = msgHdr.length;
                        return;

                    case NIOS2RES_SIGNAL:
                        if (logclass & LC_TRACE) {
                            ls_syslog(LOG_DEBUG,"\
%s: NIOS2RES_SIGNAL message with tag <%d>", fname, rtag);
                        }
                        if (debug > 1)
                            printf("Received SIGNAL for task <%d>\n", rtag);


                        xdrmem_create(&xdrs, buf, MSGSIZE, XDR_DECODE);
                        rc = readDecodeMsg_(conn2NIOS.sock.fd, buf, &msgHdr,
                                            NB_SOCK_READ_FIX, &xdrs, (char *) &sig,
                                            xdr_resSignal, NULL);
                        xdr_destroy(&xdrs);
                        if (rc < 0) {
                            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "readDecodeMsg_");
                            if (rc == -2) {



                                sig.pid=0;
                                sig.sigval=sig_encode(SIGKILL);
                                if (rtag == 0) {

                                    for (i=0; i<child_cnt; i++) {
                                        if (resSignal(children[i], sig) == -1) {
                                            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname,
                                                      "resSignal");

                                            unlink_child(children[i]);
                                        }
                                    }
                                }
                                else if (rtag > 0) {

                                    for (i=0; i<child_cnt; i++) {
                                        if (children[i]->rpid != rtag)
                                            continue;
                                        if (resSignal(children[i], sig) == -1) {
                                            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname,
                                                      "resSignal");

                                            unlink_child(children[i]);

                                        }
                                    }
                                }
                                conn2NIOS.sock.fd = INVALID_FD;

                                for (i=0; i<child_cnt; i++) {
                                    children[i]->remsock.fd = INVALID_FD;
                                    unlink_child(children[i]);
                                }
                            }
                            return;
                        }
                        if (rtag == 0) {

                            for (i=0; i<child_cnt; i++) {
                                if (resSignal(children[i], sig) == -1) {
                                    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname,
                                              "resSignal");

                                    unlink_child(children[i]);
                                }
                            }
                        }
                        else if (rtag > 0) {

                            for (i=0; i<child_cnt; i++) {
                                if (children[i]->rpid != rtag)
                                    continue;
                                if (resSignal(children[i], sig) == -1) {
                                    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname,
                                              "resSignal");

                                    unlink_child(children[i]);
                                }
                            }
                        }
                        return;

                    case NIOS2RES_EOF:

                        if (logclass & LC_TRACE) {
                            ls_syslog(LOG_DEBUG,"\
%s: NIOS2RES_EOF message with tag <%d>", fname, rtag);
                        }
                        if (debug > 1)
                            printf("Received EOF notification of stdin for task <%d>\n",
                                   rtag);


                        if (rtag == 0) {
                            for (i=0; i<child_cnt; i++) {
                                children[i]->stdin_up = 0;
                                children[i]->endstdin = 1;
                            }
                        }
                        else if (rtag > 0) {
                            for (i=0; i<child_cnt; i++)
                                if (children[i]->rpid == rtag) {
                                    children[i]->stdin_up = 0;
                                    children[i]->endstdin = 1;
                                    break;
                                }
                        }
                        return;

                    case NIOS2RES_TIMEOUT:
                        if (logclass & LC_TRACE) {
                            ls_syslog(LOG_DEBUG,"\
%s: NIOS2RES_TIMEOUT message with tag <%d>", fname, rtag);
                        }
                        if (debug > 1)
                            printf("Received TIMEOUT notification for task <%d>\n",
                                   rtag);


                        if (rtag == 0) {
                            for (i=0; i<child_cnt; i++) {
                                children[i]->remsock.fd = INVALID_FD;
                                children[i]->stdin_up = 0;
                                unlink_child(children[i]);
                            }
                        }
                        else if (rtag > 0) {
                            for (i=0; i<child_cnt; i++)
                                if (children[i]->rpid == rtag) {
                                    children[i]->remsock.fd = INVALID_FD;
                                    children[i]->stdin_up = 0;
                                    unlink_child(children[i]);
                                    break;
                                }
                        }
                        return;

                    case NIOS2RES_SETTTY:
                        if (resUpdatetty(msgHdr) < 0) {
                            ls_syslog(LOG_ERR, I18N(5290,
                                                    "Could not update tty information.")); /* catgets 5290 */
                        }
                        return;

                    case NIOS2RES_HEARTBEAT:
                        if (logclass & LC_TRACE) {
                            ls_syslog(LOG_DEBUG,"\
%s: NIOS2RES_HEARTBEAT message from nios", fname);
                        }
                        return;

                    default:
                        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5239,
                                                         "%s: Unknown msg type %d: %m"), /* catgets 5239 */
                                  fname, msgHdr.opCode);
                        return;
                }
            } else {

                if (conn2NIOS.sock.rbuf->bcount > 0)
                    return;

                if (conn2NIOS.sock.rcount > BUFSIZ)
                    i = BUFSIZ;
                else
                    i = conn2NIOS.sock.rcount;
                conn2NIOS.sock.rbuf->bp = BUFSTART(conn2NIOS.sock.rbuf);

                if ((cc = read(conn2NIOS.sock.fd, conn2NIOS.sock.rbuf->bp,
                               i)) <= 0) {
                    if (cc == 0 || BAD_IO_ERR(errno)) {
                        if (cc < 0)
                            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname,
                                      "recv/read", conn2NIOS.sock.fd);

                        conn2NIOS.sock.fd = INVALID_FD;
                        for (i=0; i < child_cnt; i++) {
                            children[i]->remsock.fd = INVALID_FD;
                            unlink_child(children[i]);
                        }
                        return;
                    }

                    return;
                }
                if (debug>1)
                    printf("read %d chars from conn2NIOS.sock:%d\n", cc,
                           conn2NIOS.sock.fd);

                conn2NIOS.sock.rcount -= cc;



                conn2NIOS.sock.rbuf->bcount = cc;

                if (logclass & LC_TRACE) {
                    ls_syslog(LOG_DEBUG,"\
%s: Res read <%d> bytes with tag=<%d> rcount=<%d> rbuf->bcount=<%d>",
                              fname, cc, conn2NIOS.rtag,
                              conn2NIOS.sock.rcount, conn2NIOS.sock.rbuf->bcount);
                }
                return;
            }

        case DOWRITE:
            if (conn2NIOS.sock.wcount == 0) {
                struct LSFHeader reqHdr;



                if (conn2NIOS.sock.wbuf->bcount == 0)
                    return;


                initLSFHeader_(&reqHdr);
                reqHdr.opCode = conn2NIOS.sock.opCode;
                reqHdr.version = VOLCLAVA_VERSION;
                reqHdr.length = conn2NIOS.sock.wbuf->bcount;
                reqHdr.reserved = conn2NIOS.wtag;
                conn2NIOS.sock.wbuf->bp -= LSF_HEADER_LEN;

                if (!xdr_packLSFHeader(conn2NIOS.sock.wbuf->bp, &reqHdr)) {
                    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_packLSFHeader");
                    for (i = 0; i < child_cnt; i++)
                        if (children[i]->rpid == conn2NIOS.wtag) {
                            unlink_child(children[i]);
                            break;
                        }
                    return;
                }
                conn2NIOS.sock.wcount = conn2NIOS.sock.wbuf->bcount
                    + LSF_HEADER_LEN;
            }

            if ((cc = write(conn2NIOS.sock.fd, conn2NIOS.sock.wbuf->bp,
                            conn2NIOS.sock.wcount)) <= 0) {
                if (cc < 0 && BAD_IO_ERR(errno)) {
                    ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "write",
                              "conn2NIOS.sock");

                    conn2NIOS.sock.fd = INVALID_FD;

                    for (i=0; i<child_cnt; i++) {
                        children[i]->remsock.fd = INVALID_FD;
                        unlink_child(children[i]);
                    }
                }
                return;
            }

            conn2NIOS.sock.wbuf->bp += cc;
            conn2NIOS.sock.wcount -= cc;

            if (debug > 1) {
                printf("wrote %d bytes with tag=<%d> to NIOS, %d bytes left\n",
                       cc, conn2NIOS.wtag, conn2NIOS.sock.wcount);
                fflush(stdout);
            }

            if (logclass & LC_TRACE) {
                ls_syslog(LOG_DEBUG,"\
%s: Res wrote <%d> bytes with tag=<%d> back to client, remaining bytes=<%d>",
                          fname, cc, conn2NIOS.wtag, conn2NIOS.sock.wcount);
            }

            if (conn2NIOS.sock.wcount == 0)

                conn2NIOS.sock.wbuf->bcount = 0;


            if (debug > 1 && conn2NIOS.sock.wcount != 0) {
                printf("wbuf remains %d chars to be pushed back\n",
                       conn2NIOS.sock.wcount);
                fflush(stdout);
            }
            break;

        case DOEXCEPTION:
            ls_syslog(LOG_DEBUG, "\
%s: DOEXCEPTION situation detected for tag=<%d>", fname, conn2NIOS.rtag);
            break;

        default:
            break;
    }

    return;
}

static int
addResNotifyList(LIST_T *list, int rpid, int opCode, resAck ack,
                 struct sigStatusUsage *sigStatRu)
{
    resNotice_t *notice;
    if (!list || rpid < 0 || opCode < RES2NIOS_CONNECT
        || opCode > RES2NIOS_NEWTASK) {
        lserrno = LSE_BAD_ARGS;
        return (-1);
    }

    if ((notice = (resNotice_t *) malloc(sizeof(resNotice_t))) == NULL) {
        lserrno = LSE_MALLOC;
        return (-1);
    }
    notice->rpid = rpid;
    notice->opCode = opCode;
    notice->retsock = conn2NIOS.sock.fd;

    if (opCode == RES2NIOS_STATUS) {
        notice->ack = ack;
        if (sigStatRu != NULL)
            memcpy((char *) &(notice->sigStatRu), (char *) sigStatRu,
                   sizeof(*sigStatRu));
    }
    else
        notice->ack = -1;

    if (listInsertEntryAtBack(list, (LIST_ENTRY_T *) notice) < 0) {
        FREEUP(notice);
        lserrno = LSE_INTERNAL;
        return (-1);
    }

    return 0;
}

int
deliver_notifications(LIST_T *list)
{
    resNotice_t *notice, *nextNotice;
    int cc;
    int n = 0;
    if (!list) {
        lserrno = LSE_BAD_ARGS;
        return (-1);
    }
    if (list->numEnts == 0)
        return 0;

    for (notice = (resNotice_t *) list->forw;
         notice != (resNotice_t *) list;
         notice = nextNotice) {
        nextNotice = notice->forw;












        if (FD_IS_VALID(conn2NIOS.sock.fd)) {
            if (notice->opCode == RES2NIOS_STATUS)
                cc = notify_client(notice->retsock, notice->rpid, notice->ack,
                                   &(notice->sigStatRu));
            else
                cc = notify_nios(notice->retsock, notice->rpid, notice->opCode);

            if (cc == 0 || cc == -2) {
                listRemoveEntry(list, (LIST_ENTRY_T *) notice);
                n++;
            }
            else if (lserrno == LSE_MALLOC)
                return (-1);
        } else {
            listRemoveEntry(list, (LIST_ENTRY_T *) notice);
            n++;
        }
    }
    return n;
}

static int
notify_nios(int retsock, int rpid, int opCode)
{
    static char fname[] = "notify_nios";
    struct LSFHeader reqHdr;
    int i, rc;

    if (debug>1) {
        printf("%s(%d): retsock=%d opCode=%d\n", fname, rpid, retsock, opCode);
        fflush(stdout);
    }

    initLSFHeader_(&reqHdr);

    reqHdr.opCode = opCode;
    reqHdr.version = VOLCLAVA_VERSION;
    reqHdr.reserved = rpid;
    reqHdr.length = 0;

    rc = writeEncodeHdr_(retsock, &reqHdr, NB_SOCK_WRITE_FIX);
    if (rc < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "writeEncodeHdr_");
        if (rpid == 0) {
            for (i = 0; i < child_cnt; i++)
                unlink_child(children[i]);
        }
        else if (rpid > 0) {
            for (i = 0; i < child_cnt; i++) {
                if (children[i]->rpid == rpid) {
                    unlink_child(children[i]);
                    break;
                }
            }
        }
        return (-1);
    }
    if (debug>1) {
        printf("NIOS is notified for task <%d>\n", rpid);
        fflush(stdout);
    }

    return 0;
}

static int
resUpdatetty(struct LSFHeader msgHdr) {
    static char fname[] = "resUpdatetty";
    XDR xdrs;
    struct resStty restty;
    ttyStruct ttyInfo;
    char *tempBuf;
    int cc;

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG, "%s: Entering", fname);
    }


    if (msgHdr.length) {
        tempBuf = malloc(msgHdr.length);
        if (!tempBuf) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
            return(-1);
        }

        if ((cc = SOCK_READ_FIX(conn2NIOS.sock.fd, tempBuf, msgHdr.length))
            != msgHdr.length) {
            ls_syslog(LOG_DEBUG,
                      "%s: b_read_fix(%d) failed, cc=%d: %m",
                      fname, msgHdr.length, cc);
            free(tempBuf);
            return(-1);
        }
    } else {
        tempBuf = NULL;
    }


    xdrmem_create(&xdrs, tempBuf, XDR_DECODE_SIZE_(msgHdr.length), XDR_DECODE);
    if (!xdr_resStty(&xdrs, &restty, &msgHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_resStty");
        xdr_destroy(&xdrs);
        return(-1);
    }
    xdr_destroy(&xdrs);
    free(tempBuf);


    ttyInfo.attr = restty.termattr;
    ttyInfo.ws = restty.ws;



    return 0;

}

static void
cleanUpKeptPids(void)
{
    static char               fname[] = "cleanUpKeptPids";
    struct listSetIterator    iter;
    long                      *pid;

    if (resKeepPid == FALSE) {
        return;
    }

    listSetIteratorAttach(pidSet, &iter);

    for (pid  = listSetIteratorBegin(&iter);
         pid != listSetIteratorEnd(&iter);
         pid  = listSetIteratorGetNext(&iter)) {

        kill(*pid, SIGKILL);

        if (logclass & LC_TRACE) {
            ls_syslog(LOG_DEBUG,"\
%s: killing process id (%d) with SIGKILL", fname, *pid);
        }
    }

    listSetIteratorDetach(&iter);
    listSetFree(pidSet);

}

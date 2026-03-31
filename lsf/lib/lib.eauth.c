/* $Id: lib.eauth.c 397 2007-11-26 19:04:00Z mblack $
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
#include <fcntl.h>
#include <termios.h>
#include <limits.h>
#include <sys/wait.h>
#include <pwd.h>
#include "lib.h"
#include "mls.h"
#include <malloc.h>
#include "../res/nios.h"
#include "../res/resout.h"
#include "lproto.h"
#include <arpa/inet.h>

#define exit(a)         _exit(a)

#define NL_SETN   23
static int getEAuth(struct eauth *, char *);
static char *getLSFAdmin(void);

int
getAuth_(struct lsfAuth *auth, char *host)
{
    auth->uid = getuid();

    if (getLSFUser_(auth->lsfUserName, sizeof(auth->lsfUserName)) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_FAIL_MM, "getAuth", "getLSFUser_");
        lserrno = LSE_BADUSER;
        return -1;
    }

    auth->gid = getgid();

    if (!genParams_[LSF_AUTH].paramValue)
        auth->kind = CLIENT_SETUID;
    else if (!strcmp(genParams_[LSF_AUTH].paramValue, AUTH_IDENT))
        auth->kind = CLIENT_IDENT;
    else if (!strcmp(genParams_[LSF_AUTH].paramValue, AUTH_PARAM_EAUTH)) {
        auth->kind = CLIENT_EAUTH;
        return getEAuth(&auth->k.eauth, host);
    } else
        auth->kind = CLIENT_SETUID;

    return (0);
}

#define EAUTHNAME "eauth"

static int
getEAuth(struct eauth *eauth, char *host)
{
    char *argv[4];
    char path[MAXPATHLEN];
    struct lenData ld;

    memset(path,0,sizeof(path));
    ls_strcat(path,sizeof(path),genParams_[LSF_SERVERDIR].paramValue);
    ls_strcat(path,sizeof(path),"/");
    ls_strcat(path,sizeof(path),EAUTHNAME);
    argv[0] = path;
    argv[1] = "-c";
    argv[2] = host;
    argv[3] = NULL;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "runEAuth(): path=<%s>", path);

    if (runEClient_(&ld, argv, FALSE) == -1) {
        if (logclass & (LC_AUTH |LC_TRACE))
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL,  "runEAuth", "runEClient", path);
        lserrno = LSE_EAUTH;
        return (-1);
    }

    if (ld.len == 0) {
        if (logclass & (LC_AUTH |LC_TRACE))
            ls_syslog(LOG_DEBUG, "runEAuth: <%s> got no data", path);
        FREEUP(ld.data);
        lserrno = LSE_EAUTH;
        return (-1);
    }

    if (ld.len > EAUTH_SIZE) {
        if (logclass & (LC_AUTH |LC_TRACE))
            ls_syslog(LOG_DEBUG, "runEAuth: <%s> got too much data, size=%d",
                      path, ld.len);
        FREEUP(ld.data);
        lserrno = LSE_EAUTH;
        return (-1);
    }

    memcpy(eauth->data, ld.data, ld.len);
    eauth->data[ld.len] = '\0';
    if (logclass & (LC_AUTH |LC_TRACE))
        ls_syslog(LOG_DEBUG, "runEAuth: <%s> got data=%s",
                  path, ld.data);
    eauth->len = ld.len;

    FREEUP(ld.data);
    if (logclass & (LC_AUTH |LC_TRACE))
        ls_syslog(LOG_DEBUG, "runEAuth: <%s> got len=%d",
                  path, ld.len);

    return (0);

}

int
verifyEAuth_(struct lsfAuth *auth, struct sockaddr_in *from)
{
    static char fname[] = "verifyEAuth/lib.eauth.c";
    char path[MAXPATHLEN], uData[256], ok;
    char *eauth_client, *eauth_server, *eauth_aux_data, *eauth_aux_status;
    int cc, i;
    static int connected = FALSE;
    static int in[2], out[2];

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s ...", fname);

    if (!(genParams_[LSF_AUTH].paramValue &&
          !strcmp(genParams_[LSF_AUTH].paramValue, AUTH_PARAM_EAUTH)))
        return (-1);

    eauth_client = getenv("LSF_EAUTH_CLIENT");
    eauth_server = getenv("LSF_EAUTH_SERVER");
    eauth_aux_data = getenv("LSF_EAUTH_AUX_DATA");
    eauth_aux_status = getenv("LSF_EAUTH_AUX_STATUS");

    sprintf(uData, "%d %d %s %s %d %d %s %s %s %s\n", auth->uid, auth->gid,
            auth->lsfUserName, inet_ntoa(from->sin_addr),
            (int) ntohs(from->sin_port), auth->k.eauth.len,
            (eauth_client ? eauth_client : "NULL"),
            (eauth_server ? eauth_server : "NULL"),
            (eauth_aux_data ? eauth_aux_data : "NULL"),
            (eauth_aux_status ? eauth_aux_status : "NULL"));

    memset(path,0,sizeof(path));
    ls_strcat(path,sizeof(path),genParams_[LSF_SERVERDIR].paramValue);
    ls_strcat(path,sizeof(path),"/");
    ls_strcat(path,sizeof(path),EAUTHNAME);

    if (logclass & (LC_AUTH | LC_TRACE))
        ls_syslog(LOG_DEBUG, "%s: <%s> path <%s> connected=%d", fname, uData,
                  path, connected);

    if (connected) {
        struct timeval tv;
        fd_set  mask;

        FD_ZERO(&mask);
        FD_SET(out[0], &mask);

        tv.tv_sec = 0;
        tv.tv_usec = 0;

        if ((cc = select(out[0] + 1, &mask, NULL, NULL, &tv)) > 0) {
            if (logclass & (LC_AUTH | LC_TRACE))
                ls_syslog(LOG_DEBUG, "%s: <%s> got exception",
                          fname, uData);
            connected = FALSE;
            close(in[1]);
            close(out[0]);
        } else {
            if (cc < 0)
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "select", uData);
        }

        if (logclass & (LC_AUTH | LC_TRACE))
            ls_syslog(LOG_DEBUG, "%s: <%s> select returned cc=%d", fname,
                      uData, cc);

    }

    if (!connected) {

        int pid;
        char *user;

        {
            if ((user = getLSFAdmin()) == NULL) {
                return (-1);
            }
        }

        if (pipe(in) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "pipe(in)", uData);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }

        if (pipe(out) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "pipe(out)", uData);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }


        if ((pid = fork()) == 0) {
            char *myargv[3];
            struct passwd *pw;

            if ((pw = getpwlsfuser_(user)) == (struct passwd *)NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "getpwlsfuser_", user);
                exit(-1);
            }

            if (lsfSetUid(pw->pw_uid) < 0) {
                ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "setuid", (int)pw->pw_uid);
                exit(-1);
            }

            for (i = 1; i < NSIG; i++)
                Signal_(i, SIG_DFL);

            alarm(0);

            close(in[1]);
            if (dup2(in[0], 0) == -1) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "dup2(in[0])", uData);
            }

            close(out[0]);
            if (dup2(out[1], 1) == -1) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "dup2(out[1])", uData);
            }

            for (i = 3; i < sysconf(_SC_OPEN_MAX); i++)
                close(i);

            myargv[0] = path;
            myargv[1] = "-s";
            myargv[2] = NULL;

            lsfExecvp(myargv[0], myargv);
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "execvp", myargv[0]);
            exit(-1);
        }

        close(in[0]);
        close(out[1]);

        if (pid == -1) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fork", path);
            close(in[1]);
            close(out[0]);
            lserrno = LSE_FORK;
            return(-1);
        }

        connected = TRUE;
    }

    i = strlen(uData);

    if ((cc = b_write_fix(in[1], uData, i)) != i) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5513,
                                         "%s: b_write_fix <%s> failed, cc=%d, i=%d: %m"), /* catgets 5513 */
                  fname, uData, cc, i);
        CLOSEHANDLE(in[1]);
        CLOSEHANDLE(out[0]);
        connected = FALSE;
        return (-1);
    }
    if(logclass & (LC_AUTH | LC_TRACE))
        ls_syslog(LOG_DEBUG, _i18n_msg_get(ls_catd , NL_SETN, 5514,
                                           "%s: b_write_fix <%s> ok, cc=%d, i=%d"),
                  fname, uData, cc, i);

    if ((cc = b_write_fix(in[1], auth->k.eauth.data, auth->k.eauth.len))
        != auth->k.eauth.len) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5515,
                                         "%s: b_write_fix <%s> failed, eauth.len=%d, cc=%d"), /* catgets 5515 */
                  fname, uData, auth->k.eauth.len, cc);
        CLOSEHANDLE(in[1]);
        CLOSEHANDLE(out[0]);
        connected = FALSE;
        return (-1);
    }
    if(logclass & (LC_AUTH | LC_TRACE))
        ls_syslog(LOG_DEBUG, _i18n_msg_get(ls_catd , NL_SETN, 5516,
                                           "%s: b_write_fix <%s> ok, eauth.len=%d, eauth.data=%.*s cc=%d:"),
                  fname, uData, auth->k.eauth.len,
                  auth->k.eauth.len, auth->k.eauth.data,cc);

    if ((cc = b_read_fix(out[0], &ok, 1)) != 1) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5517,
                                         "%s: b_read_fix <%s> failed, cc=%d: %m"), /* catgets 5517 */
                  fname, uData, cc);
        CLOSEHANDLE(in[1]);
        CLOSEHANDLE(out[0]);
        connected = FALSE;
        return (-1);
    }

    if (ok != '1') {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5518,
                                         "%s: eauth <%s> len=%d failed, rc=%c"), /* catgets 5518 */
                  fname, uData, auth->k.eauth.len, ok);
        return (-1);
    }

    return (0);
}

static char *
getLSFAdmin(void)
{
    static char admin[MAXLSFNAMELEN];
    static char fname[] = "getLSFAdmin";
    char *mycluster;
    struct clusterInfo *clusterInfo;
    struct passwd *pw;
    char *lsfUserName;

    if (admin[0] != '\0')
        return admin;

    if ((mycluster = ls_getclustername()) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_getclustername");
        return NULL;
    }
    if ((clusterInfo = ls_clusterinfo(NULL, NULL, NULL, 0, 0)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_clusterinfo");
        return (NULL);
    }

    lsfUserName = (clusterInfo->nAdmins == 0 ? clusterInfo->managerName :
                   clusterInfo->admins[0]);

    if ((pw = getpwlsfuser_(lsfUserName)) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M,
                  fname, "getpwlsfuser_", lsfUserName);
        return (NULL);
    }

    strcpy(admin, lsfUserName);

    return admin;
}


#define EAUTH_ENV_BUF_LEN       (MAXPATHLEN+32)

static int putEnvVar(char *buf, const char *envVar, const char *envValue)
{
    int rc, str_size;

    sprintf(buf, "%s=", envVar);
    if (envValue && strlen(envValue)) {
        str_size = strlen(buf) + strlen(envValue) + 1;
        if (str_size > EAUTH_ENV_BUF_LEN) {
            return -2;
        }
        strcat(buf, envValue);
    }

    rc = putenv(buf);
    if (rc != 0) {
        return -1;
    }

    return 0;
}

int
putEauthClientEnvVar(char *client)
{
    static char eauth_client[EAUTH_ENV_BUF_LEN];

    return putEnvVar(eauth_client, "LSF_EAUTH_CLIENT", client);

}

int
putEauthServerEnvVar(char *server)
{
    static char eauth_server[EAUTH_ENV_BUF_LEN];

    return putEnvVar(eauth_server, "LSF_EAUTH_SERVER", server);

}

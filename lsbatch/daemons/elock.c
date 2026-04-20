/* $Id: elock.c 397 2007-11-26 19:04:00Z mblack $
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

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <pwd.h>

#include "daemonout.h"
#include "daemons.h"
#include "../../lsf/lib/lib.osal.h"

#define NL_SETN      10

static pid_t lockOwner;

void
setLockOwnerPid(void)
{

    lockOwner = getpid();

}

#define EVENT_LOCK_FILE  "#lsb.event.lock"

extern int msleeptime;
static char lockfile[MAXFILENAMELEN];
static char gotLock = FALSE;

void
getElogLock(void)
{
    int exitCode;

    if ((exitCode = getElock()))
        die(exitCode);

    setLockOwnerPid();
}

int
getElock(void)
{
    static char fname[] = "getElock";
    int  force = 0;
    int  retry;
    char *myhostnm, *mastername;
    struct stat statbuf;
    time_t lastmodtime;
    char  first = TRUE;
    char buf[MAXHOSTNAMELEN+8];
    int  lock_fd = -1;

    if (lsb_CheckMode)
        return 0;

    if ((myhostnm = ls_getmyhostname()) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "ls_getmyhostname");
        return(MASTER_FATAL);
    }

    sprintf(lockfile, "%s/logdir/%s",
            daemonParams[LSB_SHAREDIR].paramValue,
            EVENT_LOCK_FILE);
access:

    chuser(managerId);
    if (force)
        lock_fd = open(lockfile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    else
        lock_fd = open(lockfile, O_RDWR | O_CREAT | O_EXCL, 0644);

    if (lock_fd >= 0) {
        sprintf(buf, "%s:%d", myhostnm, (int)getpid());
        write(lock_fd, buf, strlen(buf));
        close(lock_fd);
        chuser(batchId);
        ls_syslog(LOG_INFO, "%s: Got lock file", fname);
        gotLock = TRUE;
        return 0;
    } else if (errno == EEXIST) {
        int fd,cc,i,pid;

        fd = open(lockfile, O_RDONLY, 0444);
        chuser(batchId);
        if (fd < 0) {
            ls_syslog(LOG_ERR, "\
%s: Can't open existing lock file <%s>: %m", fname, lockfile);
            return(MASTER_FATAL);
        }
        i = 0;
        while ( ((cc = read(fd, &buf[i], 1)) == 1) &&
                (buf[i] != ':') )
            i++;
        if (buf[i] == ':') {
            buf[i] = '\0';
            if (equalHost_(myhostnm, buf)) {

                i = 0;
                while ( (cc=read(fd, &buf[i],1)) == 1)
                    i++;
                buf[i] = '\0';
                pid = atoi(buf);
                if (kill(pid, 0) < 0) {
                    ls_syslog(LOG_ERR, "\
%s: Last owner of lock file was on this host with pid <%d>; attempting to take over lock file", fname, pid);
                    close(fd);
                    force = 1;
                    goto access;
                }
            }
        }
        close(fd);

        if (stat(lockfile, &statbuf) < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat", lockfile);
            return(MASTER_FATAL);
        }
        lastmodtime = statbuf.st_mtime;
        retry = 0;

        while (1) {
            int j;

            millisleep_(msleeptime * 1000/2);

            mastername = ls_getmastername();
            for (j = 0; j<3 && !mastername && lserrno == LSE_TIME_OUT; j++) {
                millisleep_(6000);
                mastername = ls_getmastername();
            }
            if (mastername == NULL) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8246,
                                                 "%s: Can't get master host name: %M"),fname); /* catgets 8246 */
                return(MASTER_FATAL);
            }

            if (! equalHost_(mastername, myhostnm)) {
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8247,
                                                 "%s: Local host <%s> is not master <%s>"), /* catgets 8247 */
                          fname, myhostnm, mastername);
                return(MASTER_RESIGN);
            }

            if ( stat(lockfile, &statbuf) < 0) {
                if (errno == ENOENT)
                    goto access;
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat", lockfile);
                return(MASTER_FATAL);
            }
            if ( statbuf.st_mtime == lastmodtime ) {
                if (retry > 4) {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8249,
                                                     "%s: Previous mbatchd appears dead; attempting to take over lock file"),fname); /* catgets 8249 */
                    force = 1;
                    goto access;
                } else
                    retry++;
            } else {
                if (first) {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8250,
                                                     "%s: Another mbatchd is accessing lock file; waiting ..."),fname); /* catgets 8250 */
                    first = FALSE;
                }
                lastmodtime = statbuf.st_mtime;
                retry = 0;
            }
        }
    } else {
        chuser(batchId);
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 8251,
                                         "%s: Failed in opening lock file <%s>: %m"), /* catgets 8251 */
                  fname, lockfile);
        return(MASTER_FATAL);
    }
}

void
releaseElogLock(void)
{
    int ul_val;

    if (lsb_CheckMode)
        return;

    if (lockOwner != getpid())
        return;

    if (gotLock) {
        chuser(managerId);
        ul_val = unlink(lockfile);
        chuser(batchId);
        if (ul_val != 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, __FUNCTION__, "unlink", lockfile);
        } else
            ls_syslog(LOG_INFO, "%s: Released lock file", __FUNCTION__);
    }
}

void
touchElogLock(void)
{
    int exitCode;

    if ((exitCode = touchElock()))
        die(exitCode);
}

int
touchElock(void)
{
    static char fname[] = "touchElock";
    char buf[2];
    int lock_fd, cc;
    int i = 0;

    if (lsb_CheckMode)
        return 0;

    chuser (managerId);

    do {
        lock_fd = open(lockfile, O_RDWR, 0644);
    } while ((lock_fd < 0) && (errno == EINTR) && (i++ < 10));

    if (lock_fd < 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "open", lockfile);
        return(MASTER_FATAL);
    }
    else if (lseek(lock_fd, 0, SEEK_SET) != 0)
    {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "lseek",
                  lockfile,
                  lock_fd);
        close(lock_fd);
        return(MASTER_FATAL);
    }
    else if ((cc = read(lock_fd, buf, 1)) != 1)
    {
        chuser(batchId);
        if (cc < 0)
            ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "read",
                      lockfile,
                      lock_fd);

        else
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "read",
                      lockfile);
        close(lock_fd);
        return(MASTER_FATAL);
    }
    else if (lseek(lock_fd, 0, SEEK_SET) != 0)
    {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_D_FAIL_M, fname, "lseek",
                  lockfile,
                  lock_fd);
        close(lock_fd);
        return(MASTER_FATAL);
    }
    else if ((cc = write(lock_fd, buf, 1)) != 1)
    {
        chuser(batchId);
        if (cc < 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "write",
                      lockfile);
        }
        else
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "write",
                      lockfile);
        close(lock_fd);
        return(MASTER_FATAL);
    }

    if (close(lock_fd) != 0) {
        chuser(batchId);
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "close",
                  lockfile);
    } else
        chuser(batchId);

    return 0;
}

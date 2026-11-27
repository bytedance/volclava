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

#include <syslog.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "lib.h"
#include "../lib/lproto.h"
#include "lib.osal.h"

#define LSF_LOG_MASK   7
#define DEF_LOG_MASK   LOG_INFO
#define DEF_LOG_MASK_NAME   "LOG_WARNING"

#ifndef HAS_VSNPRINTF
#define HAS_VSNPRINTF
#endif

extern struct config_param genParams_[];

extern const char *err_str_(int errnum, const char *fmt, char *buf);

int logclass = 0;
int timinglevel = 0;

static char logfile[MAXPATHLEN];
static uid_t logOwner = (uid_t)-1;      /* set by ls_setlogowner(); -1 = leave it alone */
static gid_t logGroup = (gid_t)-1;
static char logident[10];
static int logmask;
static enum {LOGTO_SYS, LOGTO_FILE, LOGTO_STDERR} log_dest;
static int openLogFile(const char *, char *);
static void chownLogFile(void);

char *
argvmsg_(int argc, char **argv)
{
    static char avbuffer[128];
    char *bp = avbuffer;
    const char *ap;
    int i = 0;

    ap = argv[0];
    while ((i < argc) && (bp < (avbuffer + sizeof(avbuffer) - 1))) {
        if (! *ap) {
            i ++;
            if ((i < argc) && ((ap = argv[i]) != NULL)) {
                *bp = ' ';
                bp ++;
            }
        } else {
            *bp = *ap;
            bp ++;
            ap ++;
        }
    }
    *bp = '\0';
    return avbuffer;
}

/*
 * Hand the daemon log to a given owner: a daemon that runs as one identity
 * but switches euid for privileged steps (event log, job files) creates the
 * log as its starting identity, while the messages of those steps are
 * written as the switched one.  Only a daemon that knows which identity owns
 * its data calls this; without it the owner is left exactly as created.
 */
void
ls_setlogowner(uid_t uid, gid_t gid)
{
    logOwner = uid;
    logGroup = gid;

    if (logfile[0] != '\0')            /* already created: fix it now */
        chownLogFile();
}

static void
chownLogFile(void)
{
    if (logOwner != (uid_t)-1)
        chown(logfile, logOwner, logGroup);
}

void
ls_openlog(const char *ident,
           const char *path,
           int use_stderr,
           char *logMask)
{
    char *msg = NULL;

    strncpy(logident, ident, 9);
    logident[9] = '\0';

    logmask = getLogMask(&msg, logMask);

    if (use_stderr) {
        log_dest = LOGTO_STDERR;
        return;
    }

    if (path && *path) {
        char *myname;
        FILE *lfp;
        struct stat st;

        if ((myname = ls_getmyhostname()) == NULL)
            goto syslog;

        sprintf(logfile, "%s/%s.log.%s", path, ident, myname);

        if (lstat(logfile, &st) < 0) {
            if (errno == ENOENT) {
                if (openLogFile(ident, myname) == 0) {
                    if (msg != NULL)
                        ls_syslog(LOG_ERR, "%s", msg);
                    return;
                }
            } else {
                sprintf(logfile, "%s/%s.log.%s", lsTmpDir_, ident, myname);
                if (lstat(logfile, &st) < 0) {
                    if (errno == ENOENT) {
                        if ((lfp = fopen(logfile, "a")) != NULL) {
                            fclose(lfp);
                            if (!strcmp(ident, "res")
                                || (logmask >= LOG_UPTO(LOG_DEBUG)
                                    && logmask <= LOG_UPTO(LOG_DEBUG3)))
                                chmod(logfile, 0666);
                            else
                                chmod(logfile, 0644);
                            chownLogFile();
                            log_dest = LOGTO_FILE;
                            if (msg != NULL)
                                ls_syslog(LOG_ERR, "%s", msg);
                            return;
                        }
                    }
                } else if (S_ISREG(st.st_mode) && st.st_nlink == 1) {

                    if ((lfp = fopen(logfile, "a")) != NULL) {
                        fclose(lfp);
                        if (!strcmp(ident, "res")
                            || (logmask >= LOG_UPTO(LOG_DEBUG)
                                && logmask <= LOG_UPTO(LOG_DEBUG3)))
                            chmod(logfile, 0666);
                        else
                            chmod(logfile, 0644);
                        chownLogFile();
                        log_dest = LOGTO_FILE;
                        if (msg != NULL)
                            ls_syslog(LOG_ERR, "%s", msg);
                        return;
                    }
                }
            }
        } else if (S_ISREG(st.st_mode) && st.st_nlink == 1) {
            if (openLogFile(ident, myname) == 0) {
                if (msg != NULL)
                    ls_syslog(LOG_ERR, "%s", msg);
                return;
            }
        }
    }

syslog:

    log_dest = LOGTO_SYS;
    logfile[0] = '\0';

    openlog((char *) ident, LOG_PID, LOG_DAEMON);
    setlogmask(logmask);

    if (msg != NULL)
        ls_syslog(LOG_ERR, "%s", msg);
}

static int
openLogFile(const char *ident, char *myname)
{
    FILE *lfp;
    struct stat st;

    if ((lfp = fopen(logfile, "a")) == NULL) {

        sprintf(logfile, "%s/%s.log.%s", lsTmpDir_, ident, myname);
        if (lstat(logfile, &st) < 0) {
            if (errno == ENOENT) {
                if ((lfp = fopen(logfile, "a")) == NULL) {
                    return (-1);
                }
            } else {
                return (-1);
            }
        } else if (S_ISREG(st.st_mode) && st.st_nlink == 1) {

            if ((lfp = fopen(logfile, "a")) == NULL) {
                return (-1);
            }
        } else {
            return (-1);
        }
    }

    if (lfp != NULL) {
        fclose(lfp);

        if (!strcmp(ident, "res")
            || (logmask >= LOG_UPTO(LOG_DEBUG)
                && logmask <= LOG_UPTO(LOG_DEBUG3))) {
            chmod(logfile, 0666);
        } else {
            chmod(logfile, 0644);
        }
        chownLogFile();
        log_dest = LOGTO_FILE;
        return (0);
    }
    return (-1);
}

void
ls_syslog (int level, const char *fmt, ...)
{
    int save_errno = errno;
    va_list ap;
    static char lastMsg[16384];
    static int  counter = 0;

    va_start(ap, fmt);

    if (log_dest == LOGTO_STDERR) {

        if ((logmask & LOG_MASK(level)) != 0) {
            errno = save_errno;
            verrlog_(level, stderr, fmt, ap);
        }

    } else if (logfile[0]) {

        if ((logmask & LOG_MASK(level)) != 0) {
            FILE *lfp;
            struct stat st;

            if (lstat(logfile, &st) < 0) {
                if (errno == ENOENT) {
                    if ((lfp = fopen(logfile, "a")) == NULL) {

                        if (log_dest == LOGTO_FILE) {
                            log_dest = LOGTO_SYS;
                            openlog(logident, LOG_PID, LOG_DAEMON);
                            setlogmask(logmask);
                        }
                        goto use_syslog;
                    }
                } else {
                    if (log_dest == LOGTO_FILE) {
                        log_dest = LOGTO_SYS;
                        openlog(logident, LOG_PID, LOG_DAEMON);
                        setlogmask(logmask);
                    }
                    goto use_syslog;
                }
            } else if (!(S_ISREG(st.st_mode) && st.st_nlink == 1)) {

                if (log_dest == LOGTO_FILE) {
                    log_dest = LOGTO_SYS;
                    openlog(logident, LOG_PID, LOG_DAEMON);
                    setlogmask(logmask);
                }
                goto use_syslog;
            } else {
                if ((lfp = fopen(logfile, "a")) == NULL) {
                    if (log_dest == LOGTO_FILE) {
                        log_dest = LOGTO_SYS;
                        openlog(logident, LOG_PID, LOG_DAEMON);
                        setlogmask(logmask);
                    }
                    goto use_syslog;
                }
            }

            if (log_dest == LOGTO_SYS) {

                closelog();
                log_dest = LOGTO_FILE;
            }
            errno = save_errno;
            verrlog_(level, lfp, fmt, ap);
            fclose(lfp);
        }
    }
    else if ((logmask & LOG_MASK(level)) != 0)
    {
        char buf[1024];
#ifndef HAS_VSYSLOG
        char otherbuf[16384];
#endif
    use_syslog:

        if (level > LOG_DEBUG)
            level = LOG_DEBUG;
#ifdef HAS_VSYSLOG
        vsyslog(level, err_str_(save_errno, fmt, buf), ap);
#else

#if defined(HAS_VSNPRINTF)
        vsnprintf(otherbuf, sizeof(otherbuf), err_str_(save_errno, fmt, buf), ap);
#else
        vsprintf(otherbuf, err_str_(save_errno, fmt, buf), ap);
#endif

        if (!strcmp(otherbuf, lastMsg)) {
            counter++;
            if (counter > 10) {
                syslog(level, otherbuf);
                counter = 0;
            }
        } else {
            syslog(level, otherbuf);
            strcpy(lastMsg, otherbuf);
            counter = 0;
        }
#endif
        closelog();
    }

    va_end(ap);
}

void
ls_closelog(void)
{
    if (log_dest == LOGTO_SYS)
        closelog();
}

int
ls_setlogmask(int maskpri)
{
    int oldmask = logmask;

    logmask = maskpri;
    oldmask = setlogmask(logmask);

    return oldmask;
}

int
getLogMask(char **msg, char *logMask)
{
    static char msgbuf[MAXLINELEN];

    *msg = NULL;

    if (logMask == NULL)
        return (LOG_UPTO(DEF_LOG_MASK));
#ifdef LOG_ALERT
    if (strcmp(logMask, "LOG_ALERT") == 0)
        return (LOG_UPTO(LOG_ALERT));
#endif

#ifdef LOG_SALERT
    if (strcmp(logMask, "LOG_SALERT") == 0)
        return (LOG_UPTO(LOG_SALERT));
#endif

#ifdef LOG_EMERG
    if (strcmp(logMask, "LOG_EMERG") == 0)
        return (LOG_UPTO(LOG_EMERG));
#endif

    if (strcmp(logMask, "LOG_ERR") == 0)
        return (LOG_UPTO(LOG_ERR));

#ifdef LOG_CRIT
    if (strcmp(logMask, "LOG_CRIT") == 0)
        return (LOG_UPTO(LOG_CRIT));
#endif

    if (strcmp(logMask, "LOG_WARNING") == 0)
        return (LOG_UPTO(LOG_WARNING));

    if (strcmp(logMask, "LOG_NOTICE") == 0)
        return (LOG_UPTO(LOG_NOTICE));

    if (strcmp(logMask, "LOG_INFO") == 0)
        return (LOG_UPTO(LOG_INFO));

    if (strcmp(logMask, "LOG_DEBUG") == 0)
        return (LOG_UPTO(LOG_DEBUG));

    if (strcmp(logMask, "LOG_DEBUG1") == 0)
        return (LOG_UPTO(LOG_DEBUG1));

    if (strcmp(logMask, "LOG_DEBUG2") == 0)
        return (LOG_UPTO(LOG_DEBUG2));

    if (strcmp(logMask, "LOG_DEBUG3") == 0)
        return (LOG_UPTO(LOG_DEBUG3));

    sprintf(msgbuf, "\
Invalid log mask %s defined, default to %s", logMask, DEF_LOG_MASK_NAME);

    *msg = msgbuf;

    return LOG_UPTO(DEF_LOG_MASK);
}

int
getLogClass_ (char *lsp, char *tsp)
{
    char *word;
    int class = 0;

    timinglevel = 0;
    logclass = 0;

    if (tsp != NULL && isint_(tsp))
        timinglevel = atoi(tsp);

    while (lsp != NULL && (word = getNextWord_(&lsp))) {
        if (strcmp (word, "LC_SCHED") == 0)
            class |= LC_SCHED;
        else if (strcmp (word, "LC_PEND") == 0)
            class |= LC_PEND;
        else if (strcmp (word, "LC_JLIMIT") == 0)
            class |= LC_JLIMIT;
        else if (strcmp (word, "LC_EXEC") == 0)
            class |= LC_EXEC;
        else if (strcmp (word, "LC_TRACE") == 0)
            class |= LC_TRACE;
        else if (strcmp (word, "LC_COMM") == 0)
            class |= LC_COMM;
        else if (strcmp (word, "LC_XDR") == 0)
            class |= LC_XDR;
        else if (strcmp (word, "LC_CHKPNT") == 0)
            class |= LC_CHKPNT;
        else if (strcmp (word, "LC_FILE") == 0)
            class |= LC_FILE;
        else if (strcmp (word, "LC_AUTH") == 0)
            class |= LC_AUTH;
        else if (strcmp (word, "LC_HANG") == 0)
            class |= LC_HANG;
        else if (strcmp (word, "LC_SIGNAL") == 0)
            class |= LC_SIGNAL;
        else if (strcmp (word, "LC_PIM") == 0)
            class |= LC_PIM;
        else if (strcmp (word, "LC_SYS") == 0)
            class |= LC_SYS;
        else if (strcmp (word, "LC_LOADINDX") == 0)
            class |= LC_LOADINDX;
        else if (strcmp (word, "LC_JGRP") == 0)
            class |= LC_JGRP;
        else if (strcmp (word, "LC_JARRAY") == 0)
            class |= LC_JARRAY;
        else if (strcmp (word, "LC_MPI") == 0)
            class |= LC_MPI;
        else if (strcmp (word, "LC_ELIM") == 0)
            class |= LC_ELIM;
        else if (strcmp (word, "LC_M_LOG") == 0)
            class |= LC_M_LOG;
        else if (strcmp (word, "LC_PERFM") == 0)
            class |= LC_PERFM;
        else if (strcmp (word, "LC_FAIR") == 0)
            class |= LC_FAIR;
        else
            continue;
    }
    logclass = class;

    return 0;
}

void
ls_closelog_ext(void)
{
    logfile[0] = '\0';
}

int
ls_getlogmask() {
    return logmask;
}

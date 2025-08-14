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

#include "sbd.h"
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "../../lsf/lib/mls.h"

#include "../daemons/daemons.h"

#include <malloc.h>
#define NL_SETN         11

#include "../lib/lsb.spool.h"
#include "../../lsf/lib/lproto.h"
#include "../../lsf/lib/lib.rcp.h"

static char *lsbTmp(void);
static int cwdJob(struct jobCard *, char *, struct hostent *);
static int lsbatchDir(char *, struct jobCard *, struct hostent *,
                      struct lenData *);
static int lsbDirOk(char *, struct jobCard *, struct hostent *,
                    struct lenData *);
static int createJobFile(char *, char *, struct jobCard *, struct lenData *,
                         struct hostent *);
static int createChkpntJobFile(char *, struct jobCard *, struct lenData *,
                               struct hostent *, char *, char *);
static int restartFiles(char *, char *, struct jobCard *, struct hostent *);
static int localJobRestartFiles(char *, char *,  struct jobCard *,
                                struct hostent *);

static int openStdFiles(char *, char *, struct jobCard *, struct hostent *);

static int unlinkBufFiles(char *, char *, struct jobCard *, struct hostent *);
int myRename(char *, char *);

extern char **environ;
extern void freeLogRec(struct eventRec *);
void rmvJobStarterStr(char *line, char *jobStarter);

extern char chosenPath[MAXPATHLEN];
extern int lsbStdoutDirect;
static void determineFilebufStdoutDirect(char *filebuf,
                                         struct jobSpecs * jobSpecsPtr,
                                         int flag);
static int stdoutDirectSymLink(char *jobFile,
                               char *ext,
                               struct jobSpecs *jobSpecsPtr);
static int replaceAllPattern(char *str, char *find, char *replace);
static int createDirWithMode(char *path, mode_t mode);
static int processJobSpoolDir(struct jobCard *jp);
static int my_access(char * path, int mode);
static void saveCreatedPaths(char ***pathList, int *size, char * newPath, int *len);
static void removeCreatedPaths(char ***pathList, int *size, char * newPath, int *len);

int
rcpFile(struct jobSpecs *jp, struct xFile *xf, char *host, int op,
        char *errMsg)
{
    char rcpCmd[MAXLINELEN];
    char rcpArg[MAXLINELEN];
    char rcpsh[MAXLINELEN];
    int cc, pid, i;
    LS_WAIT_T status;

    int p[2], eno;

    errMsg[0] = '\0';

    if (pipe(p) < 0) {
        sprintf(errMsg, "pipe(): %s",
                strerror(errno));
        return (-1);
    }

    if ((pid = fork()) < 0) {
        sprintf(errMsg, "fork(): %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return (-1);
    }

    if (pid == 0) {
        char debugMsg[MAXLINELEN];
        sigset_t newmask;
        int maxfds =  sysconf(_SC_OPEN_MAX);

        close(p[0]);

        dup2(p[1], 1);
        dup2(p[1], 2);

        i = open ("/dev/null", O_RDONLY, 0);
        dup2(i, 0);

        for (i = 1; i < NSIG; i++)
            Signal_(i, SIG_DFL);

        sigemptyset(&newmask);
        sigprocmask(SIG_SETMASK, &newmask, NULL);

        alarm(0);

        for (i = 3; i < maxfds; i++)
            close(i);

        if (getuid() == batchId) {

            chuser(batchId);
            lsfSetUid(jp->execUid);
        }

        if (daemonParams[LSF_BINDIR].paramValue != NULL)
            sprintf(rcpCmd, "%s/lsrcp", daemonParams[LSF_BINDIR].paramValue);
        else
            sprintf(rcpCmd, "lsrcp");

        sprintf(rcpArg, "%s@%s:%s", jp->userName, host, xf->subFn);

        if (logclass & LC_FILE) {
            sprintf(debugMsg, "rcpFile: Job %s rcpCmd <%s> subFn <%s> execFn <%s> rcpArg <%s> options %x XF_OP_SUB2EXEC %x XF_OP_EXEC2SUB_APPEND %x op %x",
                    lsb_jobidinstr(jp->jobId), rcpCmd,
                    xf->subFn, xf->execFn, rcpArg,
                    xf->options, XF_OP_SUB2EXEC, XF_OP_EXEC2SUB_APPEND, op);
            sbdSyslog(LOG_DEBUG, debugMsg);
        }





        if (op & XF_OP_SUB2EXEC) {
            sprintf(rcpsh, "%s '%s' '%s'", rcpCmd, rcpArg, xf->execFn);
            execlp("/bin/sh", "/bin/sh", "-c", rcpsh, NULL);
        } else {
            if (xf->options & XF_OP_EXEC2SUB_APPEND)
                sprintf(rcpsh, "%s -a '%s' '%s'", rcpCmd, xf->execFn, rcpArg);
            else
                sprintf(rcpsh, "%s '%s' '%s'", rcpCmd, xf->execFn, rcpArg);
            execlp("/bin/sh", "/bin/sh", "-c", rcpsh, NULL);
        }


        perror(rcpCmd);
        exit(-1);
    }

    close(p[1]);

    cc = read(p[0], errMsg, 1024);
    for (i = cc; cc > 0;) {
        cc = read(p[0], errMsg+i, 1024);
        if (cc > 0)
            i += cc;
    }
    eno = errno;

    close(p[0]);

    if (i > 0) {
        errMsg[i] = '\0';

        if (errMsg[i-1] == '\n')
            errMsg[i-1] = '\0';
    } else {
        errMsg[0] = '\0';
    }

    if (cc < 0) {
        sprintf(errMsg, "read(): %s", strerror(eno));
        return (-1);
    }

    if (waitpid(pid, &status, 0) < 0) {
        sprintf(errMsg, "waitpid(%d): %s", pid, strerror(errno));
        return (-1);
    }

    if (LS_STATUS(status)) {
        if (errMsg[0] == '\0')
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 305,
                                          "lsrcp exited with non-zero status %x"), /* catgets 305 */
                    LS_STATUS(status));
        return (-1);
    }

    return (0);
}

int
rmJobBufFiles(struct jobCard *jp)
{
    char errMsg[MAXLINELEN];
    char lsbDir[MAXFILENAMELEN];
    struct hostent *hp;
    char *jf;
    int retVal;
    char *p;

    if (logclass & LC_EXEC) {
        sprintf(errMsg, "rmJobBufFiles: Enter ... job <%s> jobfile <%s>",
                lsb_jobidinstr(jp->jobSpecs.jobId), jp->jobSpecs.jobFile);
        sbdSyslog(LOG_DEBUG, errMsg);
    }


    if (!isAbsolutePathExec(jp->jobSpecs.jobFile))
        return (-1);

    if ((hp = Gethostbyname_(jp->jobSpecs.fromHost)) == NULL) {
        sprintf(errMsg, "\
%s: gethostbyname() %s failed %s", __func__,
                jp->jobSpecs.fromHost,
                lsb_jobid2str(jp->jobSpecs.jobId));
        sbdSyslog(LOG_ERR, errMsg);
        return (-1);
    }


    if ( ( jp->jobSpecs.options2 & SUB2_JOB_CMD_SPOOL )
         && ( jp->spooledExec != NULL ) ) {

        for ( p = jp->spooledExec + strlen(jp->spooledExec) - 1;
              (*p != '/') && (p > jp->spooledExec) ;
              p--);
        *p = '\0';
        ls_syslog(LOG_DEBUG, "cmd spool directory is <%s>", jp->spooledExec);

        retVal = rmDirAndFiles(jp->spooledExec);
        if ( retVal < 0 ) {
            ls_syslog(LOG_ERR,
                      I18N(5300,"Can not remove spooled executable directory <%s>\n please remove it manually"), jp->spooledExec);  /* catgets 5300 */
        }

        free( jp->spooledExec );
        jp->spooledExec = 0;
    }

    strcpy(lsbDir, jp->jobSpecs.jobFile);
    jf = strrchr(lsbDir, '/');
    *jf = '\0';
    jf++;
    return (unlinkBufFiles(lsbDir, jf, jp, hp));
}

static int
unlinkBufFiles(char *lsbDir, char *jobFile, struct jobCard *jp,
               struct hostent *hp)
{
    static char fname[] = "unlinkBufFiles()";
    char fileBuf[MAXFILENAMELEN], jfPath[MAXFILENAMELEN];
    char errMsg[MAXLINELEN];
    int error = FALSE;
    int errCode;
    int doMount = TRUE;

    if (logclass & LC_EXEC) {
        sprintf(errMsg, "%s: Enter ... job <%s> lsbDir <%s> jobFile <%s>",
                fname, lsb_jobidinstr(jp->jobSpecs.jobId), lsbDir, jobFile);
        sbdSyslog(LOG_DEBUG, errMsg);
    }

    if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
        sprintf(jfPath, "%s/%s", lsbDir, jobFile);
    } else {
        sprintf(jfPath, "%s", jobFile);
    }

    sprintf(fileBuf, "%s%s", jfPath, JOBFILEEXT);



    errCode = myunlink_(fileBuf, hp, doMount);
    if (errCode < 0 && errno != ENOENT) {
        sprintf(errMsg, I18N_JOB_FAIL_S_S_M, fname,
                lsb_jobid2str(jp->jobSpecs.jobId), "myunlink", fileBuf);
        sbdSyslog(LOG_ERR, errMsg);
        error = TRUE;
    } else if (errCode == 1) {
        doMount = 0;
    }


    if (!(jp->jobSpecs.jAttrib & JOB_SAVE_OUTPUT)) {
        sprintf(fileBuf, "%s.out", jfPath);
        if (myunlink_(fileBuf, hp, doMount) < 0 && errno != ENOENT) {
            sprintf(errMsg, I18N_JOB_FAIL_S_S_M, fname,
                    lsb_jobid2str(jp->jobSpecs.jobId), "myunlink", fileBuf);
            sbdSyslog(LOG_ERR, errMsg);
            error = TRUE;
        }
    }

    sprintf(fileBuf, "%s.err", jfPath);
    if (myunlink_(fileBuf, hp, doMount) < 0 && errno != ENOENT) {
        sprintf(errMsg, I18N_JOB_FAIL_S_S_M, fname,
                lsb_jobid2str(jp->jobSpecs.jobId), "myunlink", fileBuf);
        sbdSyslog(LOG_ERR, errMsg);
        error = TRUE;
    }

    sprintf(fileBuf, "%s.in", jfPath);
    if (myunlink_(fileBuf, hp, doMount) < 0 && errno != ENOENT) {
        sprintf(errMsg, I18N_JOB_FAIL_S_S_M, fname,
                lsb_jobid2str(jp->jobSpecs.jobId), "myunlink", fileBuf);
        sbdSyslog(LOG_ERR, errMsg);
        error = TRUE;
    }

    sprintf(fileBuf, "%s.tmp", jfPath);
    if (myunlink_(fileBuf, hp, doMount) < 0 && errno != ENOENT) {
        sprintf(errMsg, I18N_JOB_FAIL_S_S_M, fname,
                lsb_jobid2str(jp->jobSpecs.jobId), "myunlink", fileBuf);
        sbdSyslog(LOG_ERR, errMsg);
        error = TRUE;
    }

    sprintf(fileBuf, "%s.shell", jfPath);
    if (myunlink_(fileBuf, hp, doMount) < 0 && errno != ENOENT) {
        sprintf(errMsg, I18N_JOB_FAIL_S_S_M, fname,
                lsb_jobid2str(jp->jobSpecs.jobId), "myunlink", fileBuf);
        sbdSyslog(LOG_ERR, errMsg);
        error = TRUE;
    }
    if (error)
        return (-1);

    return (0);

}

int
initPaths(struct jobCard *jp, struct hostent *fromHp, struct lenData *jf)
{
    static char fname[] = "initPaths()";
    char errMsg[MAXLINELEN];
    char fileBuf[MAXFILENAMELEN];
    char cwd[MAXFILENAMELEN], lsbDir[MAXFILENAMELEN];
    char shellFile[MAXFILENAMELEN];
    int i;
    char *sp;
    bool_t goodSpoolDir = FALSE;

    (void) umask(jp->jobSpecs.umask);

    if (logclass & LC_EXEC) {
        sprintf(errMsg,
                "initPaths: Enter ... Job <%s> subHomeDir <%s> cwd <%s> execHome <%s> execCwd <%s> jobFile <%s> execUid %d userId %d",
                lsb_jobidinstr(jp->jobSpecs.jobId),
                jp->jobSpecs.subHomeDir, jp->jobSpecs.cwd,
                jp->jobSpecs.execHome, jp->jobSpecs.execCwd,
                jp->jobSpecs.jobFile, jp->jobSpecs.execUid,
                jp->jobSpecs.userId);
        sbdSyslog(LOG_DEBUG, errMsg);
    }


    for (i = 0; i < jp->jobSpecs.nxf; i++) {
        if (!isAbsolutePathSub(jp, jp->jobSpecs.xf[i].subFn) ) {
            if (isAbsolutePathSub(jp, jp->jobSpecs.cwd))
                sprintf(fileBuf, "%s/%s", jp->jobSpecs.cwd,
                        jp->jobSpecs.xf[i].subFn);
            else
                sprintf(fileBuf, "%s/%s/%s", jp->jobSpecs.subHomeDir,
                        jp->jobSpecs.cwd, jp->jobSpecs.xf[i].subFn);
            strcpy(jp->jobSpecs.xf[i].subFn, fileBuf);
        }
    }

    if (jp->jobSpecs.execCwd[0] == '\0') {
        if (getenv("WINDIR") != NULL || getenv("windir") != NULL) {
            if((sp=getenv("HOMESHARE")) != NULL){
                i = strlen(sp);
                if (!strncmp(sp, jp->jobSpecs.cwd, i)) {
                    struct passwd *pw;
                    pw = getpwdirlsfuser_(jp->execUsername);
                    if (pw) {
                        strcpy(jp->jobSpecs.execCwd, pw->pw_dir);
                    }
                    strcat(jp->jobSpecs.execCwd, &jp->jobSpecs.cwd[i]);
                    for (i=0; jp->jobSpecs.execCwd[i];i++)
                        if (jp->jobSpecs.execCwd[i] == '\\')
                            jp->jobSpecs.execCwd[i] = '/';
                }
            }
        }
    }


    if (jp->jobSpecs.execCwd[0] == '\0') {
        if (cwdJob(jp, cwd, fromHp) == -1) {
            if (logclass & LC_EXEC) {
                sprintf(errMsg, "cwdJob() failed");
                sbdSyslog(LOG_DEBUG, errMsg);
            }
            return (-1);
        }

        strcpy(jp->jobSpecs.execCwd, cwd);
        putEnv("PWD", cwd);
    } else {

        if (chdir(jp->jobSpecs.execCwd) < 0) {

            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 314,
                                          "%s: Job <%s> chdir(%s) failed, errno=<%s>. Use <%s> as execCwd"), /* catgets 314 */
                    fname,
                    lsb_jobidinstr(jp->jobSpecs.jobId),
                    jp->jobSpecs.execCwd, strerror(errno),
                    lsTmpDir_);
            sbdSyslog(LOG_INFO, errMsg);
            chdir(lsTmpDir_);
            strcpy(jp->jobSpecs.execCwd, lsTmpDir_);
        }
        putEnv("PWD", jp->jobSpecs.execCwd);
    }

    if (jp->jobSpecs.jobSpoolDir[0] != '\0') {
        if (processJobSpoolDir(jp) != 0) {
            jp->jobSpecs.jobSpoolDir[0] = '\0';
        } else {
            goodSpoolDir = TRUE;
        }
    }

    if (jp->jobSpecs.execHome[0] != '\0') {
        if (!isAbsolutePathExec(jp->jobSpecs.jobFile)) {
            if (goodSpoolDir) {
                sprintf(fileBuf, "%s/%s%s", jp->jobSpecs.jobSpoolDir,
                        jp->jobSpecs.jobFile,JOBFILEEXT);
                if (access(fileBuf, R_OK) != 0) {
                    struct stat st;
                    if (stat(fileBuf, &st) < 0)

                        sprintf(fileBuf, "%s/.lsbatch/%s", jp->jobSpecs.execHome,
                                jp->jobSpecs.jobFile);
                    else
                        sprintf(fileBuf, "%s/%s", jp->jobSpecs.jobSpoolDir,
                                jp->jobSpecs.jobFile);
                } else
                    sprintf(fileBuf, "%s/%s", jp->jobSpecs.jobSpoolDir,
                            jp->jobSpecs.jobFile);


            } else
                sprintf(fileBuf, "%s/.lsbatch/%s", jp->jobSpecs.execHome,
                        jp->jobSpecs.jobFile);

            strcpy(jp->jobSpecs.jobFile, fileBuf);
            if (logclass & LC_EXEC) {
                sprintf(errMsg, "%s: job <%s> jp->jobSpecs.jobFile is %s goodspooldir is %d", fname, lsb_jobidinstr(jp->jobSpecs.jobId), jp->jobSpecs.jobFile, goodSpoolDir);
                sbdSyslog(LOG_DEBUG, errMsg);
            }

        }

        putEnv("HOME", jp->jobSpecs.execHome);
        putEnv("LSB_JOBFILENAME", jp->jobSpecs.jobFile);

        if (strlen(jp->jobSpecs.chkpntDir) == 0) {
            sprintf(shellFile, "%s", jp->jobSpecs.jobFile);
        } else {

            char *chkDirEnv = NULL;
            char chkDir[MAXFILENAMELEN];
            chkDirEnv = getenv("LSB_CHKPNT_DIR");
            if (chkDirEnv != NULL ) {
                strcpy(chkDir,chkDirEnv);
                sprintf(shellFile, "%s/%s", chkDir, strrchr(jp->jobSpecs.jobFile, '/') +1);
            } else {
                sprintf(shellFile, "%s/%s", jp->jobSpecs.chkpntDir,
                        strrchr(jp->jobSpecs.jobFile, '/') +1);
            }
        }

        putEnv("LSB_CHKFILENAME", shellFile);
        return (0);
    }

    if (UID_MAPPED(jp) || !isAbsolutePathSub(jp, jp->jobSpecs.subHomeDir) ||
        (jp->jobSpecs.options & SUB_LOGIN_SHELL)) {
        if (lsbatchDir(lsbDir, jp, NULL, jf) == -1) {
            if (logclass & LC_EXEC) {
                sprintf(errMsg, "lsbatchDir() failed");
                sbdSyslog(LOG_DEBUG, errMsg);
            }
            return (-1);
        }
    } else {
        if (lsbatchDir(lsbDir, jp, fromHp, jf) == -1) {
            if (logclass & LC_EXEC) {
                sprintf(errMsg, "lsbatchDir() failed");
                sbdSyslog(LOG_DEBUG, errMsg);
            }
            return (-1);
        }
    }



    sprintf(fileBuf, "%s/%s", lsbDir, jp->jobSpecs.jobFile);
    strcpy(jp->jobSpecs.jobFile, fileBuf);

    if (logclass & LC_EXEC) {
        if (jp->jobSpecs.jobSpoolDir[0] == '\0') {
            sprintf(errMsg, "%s: job <%s>, lsbDir is %s, jp->jobSpecs.jobFile is <%s>, goodspooldir %d", fname, lsb_jobidinstr(jp->jobSpecs.jobId), lsbDir, jp->jobSpecs.jobFile, goodSpoolDir);
            sbdSyslog(LOG_DEBUG, errMsg);
        }else {
            sprintf(errMsg, "%s: job <%s>, spooldir is %s, lsbDir is %s, jp->jobSpecs.jobFile is %s, goodSpoolDir %d", fname, lsb_jobidinstr(jp->jobSpecs.jobId), jp->jobSpecs.jobSpoolDir,  lsbDir, jp->jobSpecs.jobFile, goodSpoolDir);
            sbdSyslog(LOG_DEBUG, errMsg);
        }
    }

    putEnv("LSB_JOBFILENAME", jp->jobSpecs.jobFile);

    if (strlen(jp->jobSpecs.chkpntDir) == 0) {
        sprintf(shellFile, "%s", jp->jobSpecs.jobFile);
    } else {

        char *chkDirEnv = NULL;
        char chkDir[MAXFILENAMELEN];
        chkDirEnv = getenv("LSB_CHKPNT_DIR");
        if (chkDirEnv != NULL ) {
            strcpy(chkDir,chkDirEnv);
            sprintf(shellFile, "%s/%s", chkDir, strrchr(jp->jobSpecs.jobFile, '/') +1);
        } else {
            sprintf(shellFile, "%s/%s", jp->jobSpecs.chkpntDir, strrchr(jp->jobSpecs.jobFile, '/') +1);
        }
    }
    putEnv("LSB_CHKFILENAME", shellFile);

    if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ){
        sp = strrchr(lsbDir, '/');

        if (sp != NULL && sp != lsbDir) {
            *sp = '\0';
        }

        if (goodSpoolDir) {

            strcpy(jp->jobSpecs.execHome, jp->jobSpecs.subHomeDir);
        } else {

            strcpy(jp->jobSpecs.execHome, lsbDir);
        }
        if (sp != NULL)
            *sp = '/';
    }

    if (logclass & LC_EXEC) {
        sprintf(errMsg,
                "initPaths: Leave ... Job <%s> subHomeDir <%s> cwd <%s> execHome <%s> execCwd <%s> jobFile <%s> execUid %d userId %d",
                lsb_jobidinstr(jp->jobSpecs.jobId),
                jp->jobSpecs.subHomeDir, jp->jobSpecs.cwd,
                jp->jobSpecs.execHome, jp->jobSpecs.execCwd,
                jp->jobSpecs.jobFile, jp->jobSpecs.execUid,
                jp->jobSpecs.userId);
        sbdSyslog(LOG_DEBUG, errMsg);
    }
    return (0);

}


static int
cwdJob(struct jobCard *jp, char *cwd, struct hostent *fromHp)
{
    struct passwd *pw;
    char errMsg[MAXLINELEN];

    if (logclass & LC_EXEC) {
        sprintf(errMsg, "cwdJob: cwd=%s", jp->jobSpecs.cwd);
        sbdSyslog(LOG_DEBUG, errMsg);
    }


    if (isAbsolutePathSub(jp, jp->jobSpecs.cwd)) {

        strcpy(cwd, jp->jobSpecs.cwd);
        if (mychdir_(cwd, fromHp) == 0) {
            strcpy(cwd, chosenPath);
            return (0);
        }

        if (logclass & LC_EXEC) {
            sprintf(errMsg,
                    "cwdJob: mychdir_(%s) failed for job <%s>: %s",
                    chosenPath, lsb_jobidinstr(jp->jobSpecs.jobId),
                    strerror(errno));
            sbdSyslog(LOG_DEBUG, errMsg);
        }


        if ((pw = getpwdirlsfuser_(jp->execUsername)) &&
            pw->pw_dir && isAbsolutePathExec(pw->pw_dir)) {
            if (chdir(pw->pw_dir) == 0) {
                char homePath[MAXPATHLEN];
                char subCwd[MAXPATHLEN];
                int i;

                if (getcwd(homePath, sizeof(homePath))) {
                    strcpy(subCwd, jp->jobSpecs.cwd);

                    for (i = 0;
                         homePath[i] != '\0' && subCwd[i] == homePath[i];
                         i++);
                    if ((i != 0) && (homePath[i] == '\0')) {
                        if (subCwd[i] == '\0') {
                            strcpy(cwd, pw->pw_dir);
                        } else if (subCwd[i] == '/') {
                            strcpy(subCwd, subCwd+i+1);
                            sprintf(cwd, "%s/%s", pw->pw_dir, subCwd);
                        }
                        if (chdir(cwd) == 0)
                            return (0);
                    }
                }
            }
        }

        strcpy(cwd, LSTMPDIR);
        if (chdir(cwd) == -1) {
            if (logclass & LC_EXEC) {
                sprintf(errMsg, "cwdJob: chdir(%s) failed after mychdir_(%s) failed for job <%s>: %s", cwd, chosenPath, lsb_jobidinstr(jp->jobSpecs.jobId), strerror(errno));
                sbdSyslog(LOG_DEBUG, errMsg);
            }
            return (-1);
        }

        return (0);
    }


    if (jp->jobSpecs.cwd[0] == '\0')
        strcpy(cwd, jp->jobSpecs.subHomeDir);
    else
        sprintf(cwd, "%s/%s", jp->jobSpecs.subHomeDir, jp->jobSpecs.cwd);

    if (mychdir_(cwd, fromHp) == 0) {
        strcpy(cwd, chosenPath);
        return (0);
    }


    if ((pw = getpwdirlsfuser_(jp->execUsername)) &&
        pw->pw_dir && isAbsolutePathExec(pw->pw_dir)) {

        if (jp->jobSpecs.cwd[0] == '\0')
            strcpy(cwd, pw->pw_dir);
        else
            sprintf(cwd, "%s/%s", pw->pw_dir, jp->jobSpecs.cwd);

        if (chdir(cwd) == 0)
            return (0);

        if (logclass & LC_EXEC) {
            sprintf(errMsg, "cwdJob: chdir(%s) failed for job <%s>: %s",
                    cwd, lsb_jobidinstr(jp->jobSpecs.jobId), strerror(errno));
            sbdSyslog(LOG_DEBUG, errMsg);
        }
    }


    strcpy(cwd, LSTMPDIR);
    if (chdir(cwd) == -1) {
        sprintf(errMsg, "cwdJob: chdir tmp (%s) failed for job <%s>: %s",
                cwd, lsb_jobidinstr(jp->jobSpecs.jobId), strerror(errno));
        sbdSyslog(LOG_DEBUG, errMsg);
        return (-1);
    }

    return (0);
}



static int
lsbatchDir(char *lsbDir, struct jobCard *jp, struct hostent *fromHp,
           struct lenData *jf)

{
    static char fname[] = "lsbatchDir()";
    char errMsg[MAXLINELEN];
    struct passwd *pw;
    bool_t goodSpoolDir = FALSE;

    if (jp->jobSpecs.jobSpoolDir[0] != '\0') {

        if (access(jp->jobSpecs.jobSpoolDir, W_OK) == 0)
            goodSpoolDir = TRUE;
        else {
            jp->jobSpecs.jobSpoolDir[0] = '\0';
        }
    }

    if (jp->jobSpecs.jobSpoolDir[0] != '\0') {
        sprintf(errMsg, "%s: Enter this function... job <%s> spool dir <%s> is %d", fname,  lsb_jobidinstr(jp->jobSpecs.jobId), jp->jobSpecs.jobSpoolDir, goodSpoolDir);
        sbdSyslog(LOG_DEBUG, errMsg);
    }

    if (fromHp) {

        if (goodSpoolDir)
            sprintf(lsbDir, "%s", jp->jobSpecs.jobSpoolDir);
        else
            sprintf(lsbDir, "%s/.lsbatch", jp->jobSpecs.subHomeDir);

        if (lsbDirOk(lsbDir, jp, fromHp, jf) == 0) {
            if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
                char *sp = strrchr(lsbDir, '/');
                if (sp != NULL)
                    *sp = '\0';

                if (goodSpoolDir) {
                    putEnv("HOME", jp->jobSpecs.subHomeDir);
                } else {

                    putEnv("HOME", lsbDir);
                }

                if (sp != NULL)
                    *sp = '/';
            }
            return (0);
        }
    }


    pw = getpwdirlsfuser_(jp->execUsername);

    if ( (pw && pw->pw_dir && isAbsolutePathExec(pw->pw_dir))) {
        if (goodSpoolDir)
            sprintf(lsbDir, "%s", jp->jobSpecs.jobSpoolDir);
        else
            sprintf(lsbDir, "%s/.lsbatch", pw->pw_dir);
        if (lsbDirOk(lsbDir, jp, NULL, jf) == 0) {

            putEnv("HOME", pw->pw_dir);
            return (0);
        }
    } else if ( fromHp == NULL && goodSpoolDir) {

        sprintf(lsbDir, "%s", jp->jobSpecs.jobSpoolDir);
        if (lsbDirOk(lsbDir, jp, NULL, jf) == 0) {

            putEnv("HOME", jp->jobSpecs.subHomeDir);
            return (0);
        }
    }


    sprintf(lsbDir, "%s%d", lsbTmp(), jp->jobSpecs.userId);

    snprintf(errMsg,  MAXLINELEN-1, "%s: Neither spool dir nor <home>/.lsbatch could be used by the job <%s>, temp dir <%s> will be used",
            fname, 
            lsb_jobid2str(jp->jobSpecs.jobId), 
            lsbDir);
    sbdSyslog(LOG_WARNING, errMsg);

    if (mkdir(lsbDir, 0700) == -1 && errno != EEXIST) {
        sprintf(errMsg, I18N_JOB_FAIL_S_M, fname,
                lsb_jobid2str(jp->jobSpecs.jobId), "mkdir");
        sbdSyslog(LOG_ERR, errMsg);

        return (-1);
    }


    if (pw && pw->pw_dir) {
        putEnv("HOME", pw->pw_dir);
    } else {

        if (goodSpoolDir) {
            putEnv("HOME", jp->jobSpecs.subHomeDir);
        } else {
            putEnv("HOME", lsbDir);
        }
    }

    strcat(lsbDir, "/.lsbatch");


    return (lsbDirOk(lsbDir, jp, NULL, jf));
}


static int
lsbDirOk(char *lsbDir, struct jobCard *jp, struct hostent *fromHp,
         struct lenData *jf)
{
    static char fname[] = "lsbDirOk()";
    struct stat st;
    char jobFile[MAXFILENAMELEN];
    char errMsg[MAXLINELEN];

    char chkpntDir[MAXFILENAMELEN],
        restartDir[MAXFILENAMELEN];

    if ((lsbDir != NULL) && (chosenPath != NULL) && (logclass & LC_EXEC)){
        sprintf(errMsg, "Enter %s: lsbDir is %s, chosenPath is %s", fname, lsbDir, chosenPath);
        sbdSyslog(LOG_DEBUG, errMsg);
    }
    if (mystat_(lsbDir, &st, fromHp) == -1)
        mymkdir_(lsbDir, 0700, fromHp);


    if (chosenPath != NULL)
        strcpy(lsbDir, chosenPath);

    if (jf) {

        if ((jp->jobSpecs.options & SUB_CHKPNTABLE) == 0 ) {


            if (createJobFile(lsbDir, NULL, jp, jf, fromHp) < 0) {
                if (!strncmp(lsbTmp(), lsbDir, strlen(lsbTmp())))
                    jobSetupStatus(JOB_STAT_PEND, PEND_JOB_NO_FILE, jp);
                ls_syslog(LOG_DEBUG, "CreateJobfile for job <%s> fail",
                          lsb_jobid2str(jp->jobSpecs.jobId));
                return (-1);
            }


            if (openStdFiles(lsbDir, NULL, jp, fromHp) < 0) {
                unlinkBufFiles(lsbDir, jp->jobSpecs.jobFile, jp, fromHp);
                if (!strncmp(lsbTmp(), lsbDir, strlen(lsbTmp())))
                    jobSetupStatus(JOB_STAT_PEND, PEND_JOB_RESTART_FILE, jp);

                return (-1);
            }
        } else {

            if (createChkpntJobFile(lsbDir, jp, jf, fromHp,
                                    chkpntDir, restartDir) < 0) {
                ls_syslog(LOG_DEBUG, "CreateChkpntJobfile for job <%s> fail",
                          lsb_jobid2str(jp->jobSpecs.jobId));
                return (-1);
            }


            if ( !(jp->jobSpecs.options & SUB_RESTART) ) {

                if (openStdFiles(lsbDir, chkpntDir, jp, fromHp) < 0) {
                    unlinkBufFiles(chkpntDir, jp->jobSpecs.jobFile, jp, fromHp);
                    jobSetupStatus(JOB_STAT_PEND, PEND_JOB_RESTART_FILE, jp);
                    return (-1);
                }
            }

            if ( jp->jobSpecs.options & SUB_RESTART ) {

                if (openStdFiles(lsbDir, restartDir, jp, fromHp) < 0) {
                    unlinkBufFiles(chkpntDir, jp->jobSpecs.jobFile, jp, fromHp);
                    jobSetupStatus(JOB_STAT_PEND, PEND_JOB_RESTART_FILE, jp);
                    return (-1);
                }
            }
        }
    }


    if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
        sprintf(jobFile, "%s/%s%s", lsbDir,
                jp->jobSpecs.jobFile, JOBFILEEXT);
    } else {
        sprintf(jobFile, "%s%s",
                jp->jobSpecs.jobFile, JOBFILEEXT);
    }
    if (stat(jobFile, &st) == -1) {

        if (errno != ENOENT) {
            sprintf(errMsg, I18N_JOB_FAIL_S_M, fname,
                    lsb_jobid2str(jp->jobSpecs.jobId), "stat");
            sbdSyslog(LOG_ERR, errMsg);
        }
        return (-1);
    }


    return (0);

}

static int
createChkpntJobFile(char *lsbDir, struct jobCard *jp, struct lenData *jf, struct hostent *fromHp, char *chkpntDir, char *restartDir)
{
    static char fname[] = "createChkpntJobFile()";
    char errMsg[MAXLINELEN];

    char chkDir[MAXFILENAMELEN],
        restartDirBak[MAXFILENAMELEN];
    char oldJobId[20], newJobId[20];
    char *strPtr, *p;




    if (((strPtr = strrchr(jp->jobSpecs.chkpntDir, '/')) != NULL)
        && (islongint_(strPtr+1))) {
        if (jp->jobSpecs.chkpntDir[0] == '/') {

            sprintf(chkpntDir, "%s", jp->jobSpecs.chkpntDir);
            *strPtr = '\0';
            sprintf(chkDir, "%s", jp->jobSpecs.chkpntDir);

            sprintf(oldJobId, "%s", strPtr+1);
            sprintf(newJobId, "%s", lsb_jobidinstr(jp->jobSpecs.jobId));
            *strPtr = '/';
        } else {
            if (*jp->jobSpecs.execCwd == '\0') {
                char cwd[MAXFILENAMELEN];
                if (cwdJob(jp,cwd,fromHp) == -1) {
                    ls_syslog(LOG_ERR, "%s: cannot find execCwd for job <%s>", fname, lsb_jobid2str(jp->jobSpecs.jobId));
                    return (-1);
                }
                strcpy(jp->jobSpecs.execCwd, cwd);
            }


            sprintf(chkpntDir, "%s/%s",jp->jobSpecs.execCwd, jp->jobSpecs.chkpntDir);
            *strPtr = '\0';
            sprintf(chkDir, "%s/%s", jp->jobSpecs.execCwd, jp->jobSpecs.chkpntDir);

            sprintf(oldJobId, "%s", strPtr+1);
            sprintf(newJobId, "%s", lsb_jobidinstr(jp->jobSpecs.jobId));
            *strPtr = '/';
        }
    } else {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 316,
                                      "Error in the chkpnt directory: %s"), /* catgets 316 */
                jp->jobSpecs.chkpntDir);
        sbdSyslog(LOG_ERR, errMsg);
        jobSetupStatus(JOB_STAT_PEND, PEND_CHKPNT_DIR, jp);
        return (-1);
    }



    if (!(jp->jobSpecs.options & SUB_RESTART)) {


        mode_t mode;
        mode = umask(0);
        if (jp->jobSpecs.chkpntDir[0] == '/' &&
            (p = strrchr(jp->jobSpecs.chkpntDir, '/')) &&
            p != &(jp->jobSpecs.chkpntDir[0])) {
            *p = '/';


            if ((mymkdir_(chkDir, 0777, fromHp) < 0 && errno != EEXIST)
                || (mymkdir_(chkpntDir, 0700, fromHp) < 0 && errno != EEXIST)) {
                sprintf(errMsg, I18N_FUNC_S_FAIL_EMSG_S,
                        fname,
                        "mymkdir_",
                        jp->jobSpecs.chkpntDir,
                        strerror(errno));
                *p = '/';
                sbdSyslog(LOG_ERR, errMsg);
                umask(mode);
                jobSetupStatus(JOB_STAT_PEND, PEND_CHKPNT_DIR, jp);
                return (-1);
            }
        } else {
            if ((mkdir(chkDir, 0777) < 0 && errno != EEXIST)
                || (mkdir(chkpntDir, 0700) < 0 && errno != EEXIST)) {
                sprintf(errMsg, I18N_FUNC_S_FAIL_EMSG_S,
                        fname,
                        "mkdir",
                        jp->jobSpecs.chkpntDir,
                        strerror(errno));
                sbdSyslog(LOG_ERR, errMsg);
                umask(mode);
                jobSetupStatus(JOB_STAT_PEND, PEND_CHKPNT_DIR, jp);
                return (-1);
            }
        }
        umask(mode);


        if (jf->len != JOBFILE_CREATED) {

            if (createJobFile(lsbDir, chkpntDir, jp, jf, fromHp) < 0) {
                if (!strncmp(lsbTmp(), lsbDir, strlen(lsbTmp())))
                    jobSetupStatus(JOB_STAT_PEND, PEND_JOB_NO_FILE, jp);
                return (-1);
            }
        }

    }




    if (jp->jobSpecs.options & SUB_RESTART) {
        DIR *dirp1, *dirp2;





        sprintf(restartDir, "%s/%s", chkDir, newJobId);
        sprintf(restartDirBak, "%s.bak", restartDir);

        if (strcmp(oldJobId, newJobId) != 0) {
            if (((dirp1 = opendir(chkpntDir)) == NULL)
                && (errno == ENOENT)) {
                if (((dirp2 = opendir(restartDir)) == NULL)
                    && (errno == ENOENT)) {
                    sprintf(errMsg, I18N_FUNC_S_FAIL_EMSG_S,
                            fname,
                            "opendir",
                            chkpntDir,
                            strerror(errno));
                    sbdSyslog(LOG_ERR, errMsg);
                    return (-1);
                } else {
                    if (dirp2) {
                        (void) closedir(dirp2);

                    } else {
                        sprintf(errMsg,\
                                "Chkpnt directory %s isn't accessable: %s.",
                                chkpntDir, strerror(errno));
                    }
                }
            } else {
                if (dirp1) {
                    (void) closedir(dirp1);
                } else {
                    sprintf(errMsg,\
                            "Chkpnt directory %s isn't accessable: %s.",
                            chkpntDir, strerror(errno));
                }
                if (((dirp2 = opendir(restartDir)) == NULL)
                    && (errno == ENOENT)) {
                    if (myRename(chkpntDir, restartDir) == -1)
                        return (-1);
                    if ((symlink(restartDir, chkpntDir) == -1) && (errno != EEXIST))
                        return (-1);
                } else {
                    if (dirp2) {
                        (void) closedir(dirp2);
                    } else {
                        sprintf(errMsg,\
                                "Chkpnt directory %s isn't accessable: %s.",
                                chkpntDir, strerror(errno));
                    }
                }

            }

        }
        if (restartFiles(lsbDir, restartDir, jp, fromHp) < 0) {
            unlinkBufFiles(lsbDir, jp->jobSpecs.jobFile, jp, fromHp);
            if (!strncmp(lsbTmp(), lsbDir, strlen(lsbTmp())))
                jobSetupStatus(JOB_STAT_PEND, PEND_JOB_RESTART_FILE, jp);
            return (-1);
        }

        return (0);
    }
    return(0);
}

int
myRename(char *fromDir, char *toDir)
{
    char errMsg[MAXLINELEN];

    if (rename(fromDir, toDir) == -1) {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 320,
                                      "Unable to rename checkpoint directory %s to %s: %s."), /* catgets 320 */
                fromDir, toDir, strerror(errno));
        sbdSyslog(LOG_ERR, errMsg);
        return (-1);
    }
    return (0);

}


static int
stdoutDirectSymLink(char *jobFile, char *ext, struct jobSpecs *jobSpecsPtr)
{
    static char fname[] = "stdoutDirectSymLink";
    char errMsg[MAXLINELEN];
    char fileLink[MAXFILENAMELEN];
    char fullpath[MAXFILENAMELEN*2];

    sprintf(fileLink, "%s.%s", jobFile, ext);


    fullpath[0] = '\0';

    if( chosenPath[0] != '/' ) {
        mygetwd_(fullpath);
        if( strlen(fullpath) && (fullpath[strlen(fullpath)-1] != '/') ) {
            strcat(fullpath, "/");
        }
    }
    strcat(fullpath, chosenPath);

    if ((symlink (fullpath, fileLink) == -1) && (errno != EEXIST)) {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 328,
                                      "%s:Job <%s> Unable to creat a symbolic link from <%s> to <%s>: %s"), /* catgets 328 */
                fname,
                lsb_jobidinstr(jobSpecsPtr->jobId), fileLink,
                fullpath, strerror(errno));
        sbdSyslog(LOG_ERR, errMsg);
        return (-1);
    }
    return (0);
}

#define STDOUT_DIRECT  1
#define STDERR_DIRECT  2
static void
determineFilebufStdoutDirect(char *filebuf,
                             struct jobSpecs * jobSpecsPtr,
                             int flag)
{
    static char fname[] = "determineFilebufStdoutDirect";
    char chr;
    int lastSlash = FALSE;
    int outDirOk = FALSE;
    int outputIsDirectory = FALSE;
    struct stat stb;
    char *usersFile = NULL;
    char *ext = NULL;

    if (logclass & (LC_TRACE | LC_EXEC)) {
        ls_syslog(LOG_DEBUG, "%s: Entering...", fname);
    }

    if( flag == STDOUT_DIRECT ) {
        ext = "out";
        usersFile = jobSpecsPtr->outFile;
    }
    if( flag == STDERR_DIRECT ) {
        ext = "err";
        usersFile = jobSpecsPtr->errFile;
    }


    chr = usersFile[strlen(usersFile) - 1];

    if (chr == '/' || chr == '\\' ) {
        outputIsDirectory = TRUE;
        lastSlash = TRUE;
    }

    if (!stat(usersFile, &stb) && S_ISDIR(stb.st_mode)) {
        outputIsDirectory = TRUE;
        outDirOk = TRUE;
    }

    if( outputIsDirectory && !outDirOk ) {


        if ( mkdir(usersFile, 0700) == 0) {
            outDirOk = TRUE;
        }
    }

    if( outputIsDirectory && lastSlash ) {
        sprintf(filebuf, "%s%s.%s", usersFile,
                lsb_jobidinstr(jobSpecsPtr->jobId), ext );
    } else if( outputIsDirectory ) {
        sprintf(filebuf, "%s/%s.%s", usersFile,
                lsb_jobidinstr(jobSpecsPtr->jobId), ext);
    } else {

        sprintf(filebuf, "%s", usersFile);
    }
}


static int
openStdFiles(char *lsbDir, char *chkpntDir, struct jobCard *jobCardPtr, struct hostent *hp)
{
    static char fname[] = "openStdFiles()";
    int i;
    char filebuf[MAXFILENAMELEN], filebufLink[MAXFILENAMELEN];
    static char stdinName[MAXFILENAMELEN];
    char xMsg[3*MSGSIZE], rcpMsg[MSGSIZE];
    char errMsg[MAXLINELEN];
    struct jobSpecs *jobSpecsPtr = &(jobCardPtr->jobSpecs);
    char jobFile[MAXFILENAMELEN], jobFileLink[MAXFILENAMELEN];
    int outFlag = 0, errFlag = 0;
    char spoolingHost[256];
    char *pStrTmp;
    struct hostent spoolHostEnt;

    int jobfileDotOutIsLink = FALSE;
    int jobfileDotErrIsLink = FALSE;

    if (logclass & LC_EXEC) {
        sprintf(errMsg, "%s, Entering openStdFiles()",fname);
        sbdSyslog(LOG_DEBUG, errMsg);
        sprintf(errMsg, "%s, the lsbDir is <%s>",fname, lsbDir);
        sbdSyslog(LOG_DEBUG, errMsg);
        sprintf(errMsg, "%s, the chkpntDir is <%s>",fname,
                chkpntDir ? chkpntDir:"NULL" );
        sbdSyslog(LOG_DEBUG, errMsg);
        sprintf(errMsg, "%s, jobFile is <%s>", fname, jobSpecsPtr->jobFile);
        sbdSyslog(LOG_DEBUG, errMsg);
    }
    if (chkpntDir == NULL) {
        if ( lsbDir != NULL && strlen(lsbDir) > 0) {
            sprintf(jobFile, "%s/%s", lsbDir, jobSpecsPtr->jobFile);
        } else {
            sprintf(jobFile, "%s", jobSpecsPtr->jobFile);
        }
        if (logclass & LC_EXEC) {
            sprintf(errMsg, "%s, jobFile is <%s> not chkpntDir",
                    fname, jobFile);
            sbdSyslog(LOG_DEBUG, errMsg);
        }
    } else {
        sprintf(jobFile, "%s/%s", chkpntDir, jobSpecsPtr->jobFile);
        if ( (lsbDir == NULL) && (strlen(lsbDir) > 0) ) {
            sprintf(jobFileLink, "%s", jobSpecsPtr->jobFile);
        } else {
            sprintf(jobFileLink, "%s/%s", lsbDir, jobSpecsPtr->jobFile);
        }
        if (logclass & LC_EXEC) {
            sprintf(errMsg, "%s, jobFileLink is <%s>",
                    fname, jobFileLink);
            sbdSyslog(LOG_DEBUG, errMsg);
        }
    }

    if (logclass & LC_EXEC) {
        if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
            sprintf(errMsg, "%s: Job File: %s/%s", fname, lsbDir,
                    jobSpecsPtr->jobFile);
        } else {
            sprintf(errMsg, "%s: Job File: %s", fname,
                    jobSpecsPtr->jobFile);
        }
        sbdSyslog(LOG_DEBUG, errMsg);
    }

    if ( jobCardPtr->jobSpecs.options2 & SUB2_JOB_CMD_SPOOL ) {
        FILE *fp;
        char line[PATH_MAX+1];
        char spooledExecName[PATH_MAX+1];
        const char *cmdPathName;
        int  cmdPathNameLen;
        int  rc;
        struct stat statbuf;
        char *jobStarter;
        char jobFileExt[MAXFILENAMELEN];


        sprintf(jobFileExt, "%s%s", jobFile, JOBFILEEXT);
        fp = fopen(jobFileExt, "rt");
        if ( fp == NULL ) {
            ls_syslog(LOG_ERR, I18N(5301,"Can not open job file %s"), jobFileExt); /* catgets 5301 */
            return -1;
        }

        while ( fgets(line, PATH_MAX, fp) != NULL ) {
            if (!strcmp(line, CMDSTART))
            {
                line[0] = '\0';
                fgets(line, PATH_MAX, fp);
                break;
            }
        }

        if ( feof(fp) || ferror(fp)  || (strlen(line) == 0 ) ) {
            ls_syslog(LOG_ERR, I18N(5302,"Can not find user input in job file %s"), /* catgets 5302 */
                      jobFile);
            fclose(fp);
            return -1;
        }
        fclose(fp);

        jobStarter=getenv("LSB_JOB_STARTER");
        rmvJobStarterStr(line, jobStarter);
        cmdPathName = (char *)getCmdPathName_(line, &cmdPathNameLen);
        memcpy(spooledExecName, cmdPathName, cmdPathNameLen);
        spooledExecName[cmdPathNameLen] = 0;
        if ( ( spooledExecName[cmdPathNameLen - 1 ] == '\r' )
             || ( spooledExecName[cmdPathNameLen - 1 ] == '\n' ) ) {
            spooledExecName[cmdPathNameLen - 1 ] = '\0';
        }
        ls_syslog(LOG_DEBUG, "spooledExec <%s>", spooledExecName);
        if ( ( pStrTmp =(char *)getSpoolHostBySpoolFile(spooledExecName) )
             != NULL ) {
            strcpy( spoolingHost, pStrTmp );
        }

        ls_syslog(LOG_DEBUG, "going to check if it is exist");
        rc = stat(spooledExecName, &statbuf);
        if ( (rc < 0) && (errno == ENOENT) ) {

            struct xFile xf;
            strcpy(xf.subFn, spooledExecName);
            strcpy(xf.execFn, spooledExecName);
            xf.options = XF_OP_SUB2EXEC;
            xMsg[0] = '\0';

            ls_syslog(LOG_DEBUG, "file does not exist, need copy");


            if ( createSpoolSubDir(spooledExecName) != 0 ) {
                ls_syslog(LOG_ERR,I18N(5303, "Can not create spool sub-directories <%s> on execution host"),  /*catgets 5303 */
                          spooledExecName);
                return -1;
            }

            if (rcpFile(jobSpecsPtr, &xf,
                        spoolingHost, XF_OP_SUB2EXEC,
                        rcpMsg) < 0) {
                ls_syslog(LOG_ERR,I18N(5304, "Can not copy spooled executable <%s> from submission host <%s> to execution host"),  /* catgets 5304 */
                          spooledExecName, jobCardPtr->jobSpecs.fromHost);
                return -1;
            }


            jobCardPtr->spooledExec = malloc(strlen(spooledExecName+1));
            if (jobCardPtr->spooledExec != NULL ) {
                strcpy(jobCardPtr->spooledExec, spooledExecName);
            }
            else {
                ls_syslog(LOG_ERR,
                          I18N(5305, "Can not allocate memory to save  spooled executable file name <%s>\n please: remove it manually"), spooledExecName ); /* catgets 5305 */
            }
        } else {
            ls_syslog(LOG_DEBUG, "I can access the file");
            if ( !S_ISREG(statbuf.st_mode) ) {
                ls_syslog(LOG_ERR,
                          I18N(5306,"spooled command file <%s> is not a regular file"),/* catgets 5306*/
                          spooledExecName);
                return -1;
            }
        }
    }


    xMsg[0] = '\0';
    for (i = 0; i < jobSpecsPtr->nxf; i++) {
        if (jobSpecsPtr->xf[i].options & XF_OP_SUB2EXEC) {

            if (rcpFile(jobSpecsPtr, jobSpecsPtr->xf+i,
                        jobCardPtr->jobSpecs.fromHost, XF_OP_SUB2EXEC,
                        rcpMsg) < 0) {
                sprintf(xMsg, _i18n_msg_get(ls_catd , NL_SETN, 322,
                                            "%sLSF: Failed to copy file <%s> from submission host <%s> to file <%s> on execution host: %s\n"), /* catgets 322 */
                        xMsg, jobSpecsPtr->xf[i].subFn, jobSpecsPtr->fromHost,
                        jobSpecsPtr->xf[i].execFn, rcpMsg);
            } else {
                if (rcpMsg[0] != '\0')
                    sprintf(xMsg, _i18n_msg_get(ls_catd , NL_SETN, 323,
                                                "%sLSF: Copy file <%s> from submission host <%s> to file <%s> on execution host: %s\n"), /* catgets 323 */
                            xMsg, jobSpecsPtr->xf[i].subFn,
                            jobSpecsPtr->fromHost, jobSpecsPtr->xf[i].execFn,
                            rcpMsg);
            }
        }
    }



    if ( (jobSpecsPtr->options & SUB_IN_FILE)
         || ( jobSpecsPtr->options2 & SUB2_IN_FILE_SPOOL ) ) {
        strcpy(filebuf, jobSpecsPtr->inFile);
    } else {
        strcpy(filebuf, NULLFILE);
    }
    ls_syslog(LOG_DEBUG, "stdin file is <%s>", filebuf);

OpenStdin:
    if ( jobSpecsPtr->options2 & SUB2_IN_FILE_SPOOL ) {

        if ( ( pStrTmp = (char *)getSpoolHostBySpoolFile(jobSpecsPtr->inFile) )
             != NULL ) {
            strcpy( spoolingHost, pStrTmp );
            memcpy(&spoolHostEnt,
                   Gethostbyname_(spoolingHost),
                   sizeof(spoolHostEnt) );
            i=myopen_(filebuf, O_RDONLY, 0, &spoolHostEnt);
        } else {
            ls_syslog(LOG_ERR,I18N(5307, "jobSpecs.inFile format error:<%s>"), /*catgets 5307 */
                      jobSpecsPtr->inFile);
            return -1;
        }
    } else {

        i=myopen_(filebuf, O_RDONLY, 0, hp);
    }

    if (i < 0) {
        struct xFile xf;

        if (errno != ENOENT) {
            sprintf(xMsg, I18N_FUNC_S_FAIL_M, fname, "myopen_", filebuf);
        } else {


            if (!isAbsolutePathSub(jobCardPtr, jobSpecsPtr->inFile)) {
                if (isAbsolutePathSub(jobCardPtr, jobSpecsPtr->cwd)) {
                    sprintf(xf.subFn, "%s/%s", jobSpecsPtr->cwd,
                            jobSpecsPtr->inFile);
                } else {
                    sprintf(xf.subFn, "%s/%s/%s", jobSpecsPtr->subHomeDir,
                            jobSpecsPtr->cwd, jobSpecsPtr->inFile);
                }
            } else {

                strcpy(xf.subFn, jobSpecsPtr->inFile);
            }

            sprintf(xf.execFn, "%s.in", jobFile);

            xf.options = XF_OP_SUB2EXEC;
            strcpy(filebuf, xf.execFn);


            if ( jobSpecsPtr->options2 & SUB2_IN_FILE_SPOOL ) {
                i = rcpFile(jobSpecsPtr, &xf, spoolingHost,
                            XF_OP_SUB2EXEC, rcpMsg);
            } else {
                i = rcpFile(jobSpecsPtr, &xf, jobSpecsPtr->fromHost,
                            XF_OP_SUB2EXEC, rcpMsg);
            }

            if ( i == 0 ) {
                if (rcpMsg[0] != '\0')
                    sprintf(xMsg, _i18n_msg_get(ls_catd , NL_SETN, 325,
                                                "%sLSF: Copy stdin file <%s> on submissi on host <%s> to <%s> on execution host: %s\n"), /* catgets 325 */
                            xMsg,
                            xf.subFn,
                            jobSpecsPtr->fromHost,
                            xf.execFn,
                            rcpMsg);
                goto OpenStdin;
            }

            sprintf(xMsg, _i18n_msg_get(ls_catd , NL_SETN, 326,
                                        "%sLSF: Unable to copy stdin file <%s> on submission host <%s> to <%s> on execution host: %s\n"), /* catgets 326 */
                    xMsg, xf.subFn, jobSpecsPtr->fromHost, xf.execFn, rcpMsg);
        }
    } else {
        if (dup2(i, 0) == -1) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 327,
                                          "%s: Job <%s> dup2(%d,0) stdin <%s>: %s"), /* catgets 327 */
                    fname,
                    lsb_jobidinstr(jobSpecsPtr->jobId), i, jobSpecsPtr->inFile,
                    strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
            return (-1);
        }
        strcpy(stdinName, chosenPath);
        jobCardPtr->stdinFile = stdinName;
    }


    if ((jobSpecsPtr->options & SUB_OUT_FILE) &&
        !strcmp(jobSpecsPtr->outFile,"/dev/null"))
        sprintf(filebuf, "%s", jobSpecsPtr->outFile);
    else {
        outFlag = 1;


        if( (jobSpecsPtr->options & SUB_OUT_FILE) && lsbStdoutDirect ) {

            determineFilebufStdoutDirect(filebuf, jobSpecsPtr, STDOUT_DIRECT);
        } else {
            sprintf(filebuf, "%s.out", jobFile);
        }

        if (chkpntDir != NULL) {
            sprintf(filebufLink, "%s.out", jobFileLink);
        }
    }

    if (logclass & LC_TRACE) {
        sprintf(errMsg,
                "%s: job <%s> filebuf/stdout file <%s> lsbStdoutDirect=%d",
                fname, lsb_jobidinstr(jobSpecsPtr->jobId), filebuf,
                lsbStdoutDirect);
        sbdSyslog(LOG_DEBUG, errMsg);
    }

    if( lsbStdoutDirect ) {

        i = myopen_(filebuf, O_WRONLY| O_CREAT| O_APPEND, 0666, hp);
    } else {

        i = myopen_(filebuf, O_WRONLY| O_CREAT| O_APPEND, 0600, hp);
    }


    if  ((i < 0) && outFlag && lsbStdoutDirect ) {

        sprintf(errMsg,
                "%s: job <%s> could not open <%s> for writing stdout, %s",
                fname, lsb_jobidinstr(jobSpecsPtr->jobId),
                filebuf, strerror(errno));
        sbdSyslog(LOG_DEBUG, errMsg);
        sprintf(filebuf, "%s.out", jobFile);
        sprintf(errMsg, "%s: job <%s> filebuf/stdout file <%s>",
                fname, lsb_jobidinstr(jobSpecsPtr->jobId), filebuf);
        sbdSyslog(LOG_DEBUG, errMsg);

        i = myopen_(filebuf, O_WRONLY| O_CREAT| O_APPEND, 0600, hp);
    }
    else if( !(i < 0) && outFlag && lsbStdoutDirect ) {

        if( stdoutDirectSymLink(jobFile, "out", jobSpecsPtr ) < 0 ) {
            return (-1);
        }
        jobfileDotOutIsLink = TRUE;
    }

    if ((outFlag) && !(chkpntDir == NULL)) {

        if( jobfileDotOutIsLink ) {
            sprintf(filebuf, "%s.out", jobFile);
        }

        if ((symlink (filebuf, filebufLink) == -1) && (errno != EEXIST)) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 328,
                                          "%s:Job <%s> Unable to creat a symbolic link from <%s> to <%s>: %s"), /* catgets 328 */
                    fname,
                    lsb_jobidinstr(jobSpecsPtr->jobId), filebufLink,
                    filebuf, strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
            return (-1);
        }
    }


    if (i < 0) {
        sprintf(errMsg, I18N_JOB_FAIL_S_M, fname, lsb_jobid2str(jobSpecsPtr->jobId), "open");
        sbdSyslog(LOG_ERR, errMsg);
        return (-1);
    }
    if (dup2(i, 1) == -1) {
        sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 330,
                                      "%s: Job <%s> dup(%d,1) stdout <%s> failed: %s"), /* catgets 330 */
                fname,
                lsb_jobidinstr(jobSpecsPtr->jobId), i, filebuf, strerror(errno));
        sbdSyslog(LOG_ERR, errMsg);
        return (-1);
    }



    if (!(jobSpecsPtr->options & SUB_ERR_FILE) ||
        ((jobSpecsPtr->options & (SUB_ERR_FILE | SUB_OUT_FILE)) ==
         (SUB_ERR_FILE | SUB_OUT_FILE) &&
         !strcmp(jobSpecsPtr->errFile, jobSpecsPtr->outFile))) {

        if (dup2(1, 2) == -1 ){
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 331,
                                          "%s: Job <%s> dup(1,2) stderr failed: %s"), /* catgets 331 */
                    fname,
                    lsb_jobidinstr(jobSpecsPtr->jobId), strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
            return (-1);
        }
    } else {

        if (!strcmp(jobSpecsPtr->errFile, "/dev/null"))
            sprintf(filebuf, "%s", jobSpecsPtr->errFile);
        else {
            errFlag = 1;

            if( lsbStdoutDirect ) {

                determineFilebufStdoutDirect(filebuf, jobSpecsPtr,
                                             STDERR_DIRECT);
            } else {
                sprintf(filebuf, "%s.err", jobFile);
            }

            if (chkpntDir != NULL) {
                sprintf(filebufLink, "%s.err", jobFileLink);
            }
        }

        if (logclass & LC_TRACE) {
            sprintf(errMsg,
                    "%s: job <%s> filebuf/stderr file <%s> lsbStdoutDirect=%d",
                    fname, lsb_jobidinstr(jobSpecsPtr->jobId), filebuf,
                    lsbStdoutDirect );
            sbdSyslog(LOG_DEBUG, errMsg);
        }

        if( lsbStdoutDirect ) {

            i = myopen_(filebuf, O_WRONLY | O_CREAT | O_APPEND, 0666, hp);
        } else {

            i = myopen_(filebuf, O_WRONLY | O_CREAT | O_APPEND, 0600, hp);
        }

        if( (i < 0) && errFlag && lsbStdoutDirect ) {

            sprintf(errMsg,
                    "%s: job <%s> could not open <%s> for writing stderr, %s",
                    fname, lsb_jobidinstr(jobSpecsPtr->jobId),
                    filebuf, strerror(errno));
            sbdSyslog(LOG_DEBUG, errMsg);
            sprintf(filebuf, "%s.err", jobFile);
            sprintf(errMsg,  "%s: job <%s> filebuf/stderr file <%s>",
                    fname, lsb_jobidinstr(jobSpecsPtr->jobId), filebuf);
            sbdSyslog(LOG_DEBUG, errMsg);

            i = myopen_(filebuf, O_WRONLY| O_CREAT| O_APPEND, 0600, hp);
        }
        else if( !(i < 0) && outFlag && lsbStdoutDirect ) {

            if( stdoutDirectSymLink(jobFile, "err", jobSpecsPtr ) < 0 ) {
                return (-1);
            }
            jobfileDotErrIsLink = TRUE;
        }

        if ((errFlag) && !(chkpntDir == NULL)) {

            if( jobfileDotErrIsLink ) {
                sprintf(filebuf, "%s.err", jobFile);
            }
            if ((symlink (filebuf, filebufLink) == -1) && (errno != EEXIST)) {
                sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 332,
                                              "%s:Job <%s> Unable to creat a symbolic link from <%s> to <%s>: %s"), /* catgets 332 */
                        fname,
                        lsb_jobidinstr(jobSpecsPtr->jobId),
                        filebufLink, filebuf, strerror(errno));
                sbdSyslog(LOG_ERR, errMsg);
                return (-1);
            }
        }


        if (i < 0) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 333,
                                          "%s: Job <%s> open(%s) stderr failed: %s"), /* catgets 333 */
                    fname,
                    lsb_jobidinstr(jobSpecsPtr->jobId), filebuf, strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
            return (-1);
        }

        if (dup2(i, 2) == -1) {
            sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 334,
                                          "%s: Job <%s> dup(%d,2) stderr <%s> failed: %s"), /* catgets 334 */
                    fname,
                    lsb_jobidinstr(jobSpecsPtr->jobId),
                    i, filebuf, strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
            return (-1);
        }
    }

    if (xMsg[0] != '\0')
        fprintf(stderr, "%s", xMsg);

    return (0);
}



int
appendJobFile(struct jobCard *jobCard, char *header, struct hostent *hp,
              char *errMsg)
{
    static char fname[] = "appendJobFile()";
    FILE *jobFile_fp;

    if ((jobFile_fp = myfopen_(jobCard->jobSpecs.jobFile, "a", hp)) == NULL) {
        sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "myopen_",
                jobCard->jobSpecs.jobFile);
        return (-1);
    }

    if ((fprintf(jobFile_fp, "# JOB_PID:\n") < 0) ||
        (fprintf(jobFile_fp, "%d\n", jobCard->jobSpecs.jobPid) < 0)) {
        sprintf(errMsg, I18N_FUNC_S_S_FAIL_M, fname, "fprintf",
                jobCard->jobSpecs.jobFile, "JOB_PID");
        FCLOSEUP(&jobFile_fp);
        return (-1);
    }

    if ((fprintf(jobFile_fp, "# JOB_PGID:\n") < 0) ||
        (fprintf(jobFile_fp, "%d\n", jobCard->jobSpecs.jobPGid) < 0)) {
        sprintf(errMsg, I18N_FUNC_S_S_FAIL_M, fname, "fprintf",
                jobCard->jobSpecs.jobFile, "JOB_PGID");
        FCLOSEUP(&jobFile_fp);
        return (-1);
    }

    if ((fprintf(jobFile_fp, header) < 0) ||
        (fprintf(jobFile_fp, "\n%d\n", jobCard->w_status) < 0)) {
        sprintf(errMsg, I18N_FUNC_S_S_FAIL_M, fname, "fprintf",
                jobCard->jobSpecs.jobFile, "STATUS");
        FCLOSEUP(&jobFile_fp);
        return (-1);
    }

    if (FCLOSEUP(&jobFile_fp) < 0) {
        sprintf(errMsg, I18N_FUNC_S_FAIL_M, fname, "fclose",
                jobCard->jobSpecs.jobFile);
        return (-1);
    }

    return (0);
}


void
writePreJobFail(struct jobCard *jp)
{
    static char fname[] = "writePreJobFail()";
    FILE *fp;
    char fn[MAXFILENAMELEN];

    sprintf(fn, "%s/.%s.%s.fail", LSTMPDIR, jp->jobSpecs.jobFile, lsb_jobidinstr(jp->jobSpecs.jobId));
    if ((fp = fopen(fn, "w")) == NULL) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M, fname,
                  lsb_jobid2str(jp->jobSpecs.jobId), "fopen", fn);
    } else {
        if (FCLOSEUP(&fp) < 0)
            ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_S_M, fname,
                      lsb_jobid2str(jp->jobSpecs.jobId), "fclose", fn);
    }
}

static int
createJobFile(char *lsbDir, char *chkpntDir, struct jobCard *jp, struct lenData *jf,
              struct hostent *hp)
{
    static char fname[] = "createJobFile";
    struct jobSpecs *jobSpecsPtr = &jp->jobSpecs;
    int fd, cc, len;
    char errMsg[MAXLINELEN];
    char jobFile[MAXFILENAMELEN], jobFileLink[MAXFILENAMELEN];
    char shellFile[MAXFILENAMELEN], shellFileLink[MAXFILENAMELEN];
    char *sp;

    char *shellLine;


    if (chkpntDir == NULL) {
        sprintf(jobFile, "%s/%s%s",  lsbDir,
                jp->jobSpecs.jobFile, JOBFILEEXT);

    } else {
        sprintf(jobFile, "%s/%s%s", chkpntDir, jp->jobSpecs.jobFile, JOBFILEEXT);
        sprintf(jobFileLink, "%s/%s", lsbDir, jp->jobSpecs.jobFile);
        sprintf(shellFile, "%s.shell", jobFile);
        sprintf(shellFileLink, "%s.shell", jobFileLink);
    }


    if ((fd = myopen_(jobFile, O_CREAT | O_TRUNC | O_WRONLY, 0700, hp))
        == -1) {
        sprintf(errMsg, "createJobFile: Job <%s> open jobfile %s failed: lsbDir is %s, jp->jobSpecs.jobFile is %s,  %s",
                lsb_jobidinstr(jobSpecsPtr->jobId), jobFile, lsbDir, jp->jobSpecs.jobFile, strerror(errno));
        sbdSyslog(LOG_ERR, errMsg);
        return (-1);
    }


    if (logclass & LC_TRACE) {
        sprintf(errMsg, "createJobFile: Job <%s> open jobfile %s success: lsbDir is %s, jp->jobSpecs.jobFile is %s", lsb_jobidinstr(jobSpecsPtr->jobId), jobFile, lsbDir, jp->jobSpecs.jobFile);
        sbdSyslog(LOG_DEBUG, errMsg);
    }
    shellLine=SHELLLINE;
    if ((cc = write(fd, shellLine, strlen(shellLine))) !=
        strlen(shellLine)) {
        sprintf(errMsg, "createJobFile:\
 Job <%s> write jobfile %s len=%lu cc=%d failed: %s",
                lsb_jobidinstr(jobSpecsPtr->jobId),
                jobFile,
                strlen(shellLine) + 1,
                cc,
                strerror(errno));
        sbdSyslog(LOG_DEBUG, errMsg);
        close(fd);
        return (-1);
    }

    if ((sp = strstr (jf->data, TRAPSIGCMD)) == NULL) {
        sprintf(errMsg,
                "createJobFile: No TRAPSIGCMD <$LSB_TRAPSIGS...> is found in jobfile data for job <%s>", lsb_jobidinstr(jobSpecsPtr->jobId));
        sbdSyslog(LOG_DEBUG, errMsg);
        sp = jf->data;
    }

    len = strlen(sp);

    if ((cc = write(fd, sp, len)) != len) {
        sprintf(errMsg,
                "%s: Job <%s> write jobfile %s len=%d cc=%d failed: %s",
                fname, lsb_jobidinstr(jobSpecsPtr->jobId),
                jobFile, len, cc, strerror(errno));
        sbdSyslog(LOG_DEBUG, errMsg);
        close(fd);
        return (-1);
    }

    if (close(fd) == -1) {
        sprintf(errMsg, "createJobFile: Job <%s> close jobfile %s failed: %s",
                lsb_jobidinstr(jobSpecsPtr->jobId),
                jobFile, strerror(errno));
        sbdSyslog(LOG_DEBUG, errMsg);
        return (-1);
    }


    if (chkpntDir != NULL) {
        if ((symlink (jobFile, jobFileLink) == -1) && (errno != EEXIST)) {
            sprintf(errMsg, "createJobFile:Job <%s> Unable to creat a symbolic link from <%s> to <%s>: %s",
                    lsb_jobidinstr(jobSpecsPtr->jobId), jobFileLink, jobFile, strerror(errno));
            sbdSyslog(LOG_DEBUG, errMsg);
            return (-1);
        }

        if ((symlink (shellFile, shellFileLink) == -1) && (errno != EEXIST)) {
            sprintf(errMsg, "createJobFile:Job <%s> Unable to creat shellfile symbolic link from <%s> to <%s>: %s",
                    lsb_jobidinstr(jobSpecsPtr->jobId), shellFileLink, shellFile, strerror(errno));
            sbdSyslog(LOG_DEBUG, errMsg);
            return (-1);
        }
    }

    return (0);

}

int
isAbsolutePathSub(struct jobCard *jp, const char *path)
{
    if (path[0] == '/' ||
        ((jp->jobSpecs.options2 & SUB2_HOST_NT) &&
         (path[0] == '\\' || (path[0] != '\0' && path[1] == ':'))))
        return(TRUE);
    else
        return(FALSE);
}

int
isAbsolutePathExec(const char *path)
{
    if (path[0] ==  '/')
        return(TRUE);
    else
        return(FALSE);
}

void
jobFileExitStatus(struct jobCard *jobCard)
{
    static char fname[] = "jobFileExitStatus()";
    FILE *jobFile_fp;
    char line[MAXLINELEN];
    struct hostent *hp;
    int pid;


    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "jobFileExitStatus: Entering ... job <%s>",
                  lsb_jobid2str(jobCard->jobSpecs.jobId));
    if ((pid = fork()) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S_M, fname,
                  lsb_jobid2str(jobCard->jobSpecs.jobId), "fork");
        jobCard->exitPid = 0;
        return;
    }

    if (pid > 0) {
        jobCard->exitPid = pid;
        return;
    }

    jobCard->w_status = -1;

    putEnv(LS_EXEC_T, "END");

    if (postJobSetup(jobCard) < 0) {
        ls_syslog(LOG_ERR, I18N_JOB_FAIL_S, fname,
                  lsb_jobid2str(jobCard->jobSpecs.jobId), "postJobSetup");
        exit(jobCard->w_status);
    }

    if ((hp = Gethostbyname_(jobCard->jobSpecs.fromHost)) == NULL) {
        ls_syslog(LOG_ERR, "\
%s: gethostbyname() %s failed job %s", __func__,
                  jobCard->jobSpecs.fromHost,
                  lsb_jobid2str(jobCard->jobSpecs.jobId));
                  exit(jobCard->w_status);
    }

    chuser(jobCard->jobSpecs.execUid);

    if ((jobFile_fp = myfopen_(jobCard->jobSpecs.jobFile, "r", hp))
        != NULL) {
        while (fgets(line, MAXLINELEN, jobFile_fp) != NULL) {
            if (strncmp(line, "# JOB_PID:", 10) == 0) {
                if (fgets(line, MAXLINELEN, jobFile_fp) != NULL)
                    sscanf(line, "%d", &jobCard->jobSpecs.jobPid);
                else
                    break;

                if ((fgets(line, MAXLINELEN, jobFile_fp) != NULL)
                    && (strncmp(line, "# JOB_PGID:", 11) == 0)
                    && (fgets(line, MAXLINELEN, jobFile_fp) != NULL))
                    sscanf(line, "%d", &jobCard->jobSpecs.jobPGid);
                else
                    break;

                if ((fgets(line, MAXLINELEN, jobFile_fp) != NULL)
                    && (strncmp(line, "# PRE_EXEC STATUS:", 18) == 0)
                    && (fgets(line, MAXLINELEN, jobFile_fp) != NULL))
                    sscanf(line, "%d", &jobCard->w_status);
                else
                    break;
            } else if (strncmp(line, "# EXIT STATUS:", 14) == 0) {

                if (fgets(line, MAXLINELEN, jobFile_fp) != NULL)
                    sscanf(line, "%d", &jobCard->w_status);
                break;
            }
        }
        FCLOSEUP(&jobFile_fp);
    }

    chuser(batchId);

    if (logclass & LC_EXEC)
        ls_syslog(LOG_DEBUG, "jobFileExitStatus: job <%s> w_status %d",
                  lsb_jobid2str(jobCard->jobSpecs.jobId), jobCard->w_status);

    exit(jobCard->w_status);
}


static char *
lsbTmp(void)
{
    static char tmpDir[MAXFILENAMELEN];

    sprintf(tmpDir, "%s/.lsbtmp", lsTmpDir_);

    return (tmpDir);
}

static int
restartFiles(char *lsbDir, char *restartDir,  struct jobCard *jp, struct hostent
             *hp)
{
    int returnCode = 0;

    returnCode = localJobRestartFiles(lsbDir, restartDir, jp, hp);
    return(returnCode);
}

static int
localJobRestartFiles(char *lsbDir, char *restartDir,  struct jobCard *jp, struct
                     hostent *hp)
{
    static char fname[] = "localJobRestartFiles";
    struct jobSpecs *jspecs = &jp->jobSpecs;
    char errMsg[MAXLINELEN];
    char t[MAXFILENAMELEN], s[MAXFILENAMELEN];
    char jobFile[MAXFILENAMELEN];

    sprintf(jobFile, "%s", jp->jobSpecs.jobFile);


    if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
        sprintf (s, "%s/%s", lsbDir, jobFile);
    } else {
        sprintf (s, "%s", jobFile);
    }
    sprintf (t, "%s/%s", restartDir, jobFile);

    if (symlink(t, s) == -1 && errno != EEXIST) goto return_error;


    if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
        sprintf (s, "%s/%s.out", lsbDir, jobFile);
    } else {
        sprintf (s, "%s.out", jobFile);
    }
    sprintf (t, "%s/%s.out", restartDir, jobFile);

    if (symlink(t, s) == -1 && errno != EEXIST) goto return_error;


    if ( (lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
        sprintf (s, "%s/%s.err", lsbDir, jobFile);
    } else {
        sprintf (s, "%s.err", jobFile);
    }
    sprintf (t, "%s/%s.err", restartDir, jobFile);

    if (symlink(t, s) == -1 && errno != EEXIST) goto return_error;


    if ((lsbDir != NULL) && (strlen(lsbDir) > 0) ) {
        sprintf (s, "%s/%s.shell", lsbDir, jobFile);
    } else {
        sprintf (s, "%s.shell", jobFile);
    }
    sprintf (t, "%s/%s.shell", restartDir, jobFile);

    if (symlink(t, s) == -1 && errno != EEXIST) goto return_error;

    return (0);

return_error:
    sprintf(errMsg, _i18n_msg_get(ls_catd , NL_SETN, 321,
                                  "%s: Job <%s> Unable to creat a symbolic link from <%s> to <%s>: %s"), /* catgets 321 */
            fname, lsb_jobidinstr(jspecs->jobId), t, s, strerror(errno));
    sbdSyslog(LOG_ERR, errMsg);
    return (-1);

}

/*------------------------------------------------------------------------
 * my_access()
 * Param:
 *     path: the path we will check
 *     mode:the mode (F_OK|W_OK|R_OK|X_OK) we will check
 * Return:
 * 0: success
 * -1: failed due to mode missed and other error
 * -2: failed due to file/dir not exist
 *------------------------------------------------------------------------
 */
static int my_access(char * path, int mode)
{
    static char fname[] = "my_access()";
    struct stat stats;
    uid_t euid, ruid;
    gid_t egid, rgid;
    int ret;
    int myMode = 0;
    int nRetry = 1;
    char errMsg[MAXLINELEN];
    
    euid = geteuid();
    egid = getegid();
    ruid = getuid();
    rgid = getgid();

    errno = 0;
    if (ruid==euid && rgid==egid) {
        /*Using access() to check mode*/
        while (nRetry-- >= 0){
            if (access(path, mode) == 0)
                return 0;
        }
        if (errno == ENOENT) {
            return -2;
        } else {
            return -1;
        }
    }

    /*Using stat() to check mode*/
    nRetry = 1;
    errno = 0;
    while (nRetry-- >= 0){
        ret = stat(path, &stats);
        if (ret == 0) {
            break;
        }
    }

    /*check mode details if success*/
    if (ret == 0) {
        mode &= (X_OK | W_OK | R_OK); /*only keep modes which we care about*/
        if (mode == F_OK)
            return 0;   /* The file exists */

        if (euid == 0 && ((mode & X_OK) == 0
            || (stats.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))) {
            return 0;
        }

        if (euid == stats.st_uid) {
            /*checking owner premission*/
            myMode = (unsigned int) (stats.st_mode & (mode << 6)) >> 6;
        } else if (egid == stats.st_gid) {
            /*checking group premission*/
            myMode = (unsigned int) (stats.st_mode & (mode << 3)) >> 3;
        } else {
            /*checking whether this is subgroup*/
            int   gid_size, i, isMember;

            /*set checking of others as default*/
            myMode = (stats.st_mode & mode);
            /*check groups*/
            gid_t *gid_list;
            isMember = 0;
            gid_size = getgroups(0, NULL);
            if (gid_size > 0) {
                gid_list = calloc(gid_size, sizeof(gid_t));
                /* Failed to allocate memory for group list, cannot call
                 * Beowulf API. Set the default number of cpus.
                 */
                if (gid_list == NULL) {
                    sprintf(errMsg, I18N_FUNC_FAIL_M, fname, "calloc()");
                    sbdSyslog(LOG_ERR, errMsg);
                    return(-1);
                }
                getgroups(gid_size, gid_list);

                for(i=0; i<gid_size; i++){
                    if (gid_list[i] == stats.st_gid) {
                        isMember = 1;
                        break;
                    }
                }
                FREEUP(gid_list);
            } else {
                sprintf(errMsg, I18N_FUNC_FAIL_M, fname, "getgroup()");
                sbdSyslog(LOG_ERR, errMsg);
                return -1;
            }

            if (isMember) {
                myMode = (unsigned int) (stats.st_mode & (mode << 3)) >> 3;
            }
        }

        if (myMode == mode)
            return 0;

        return -1;
    } else if (errno == ENOENT) {
        return -2;
    }

    return -1;

}


static int replaceAllPattern(char *str, char *find, char *replace) {
    char buffer[MAXFILENAMELEN] = {0};
    char *p = NULL;
    size_t find_len = strlen(find);
    size_t replace_len = strlen(replace);
    
    if (find_len == 0) {
        return -1;
    }
    
    p = strstr(str, find);
    if (p == NULL) {
        return 0; // No occurrences found
    }
    
    while (p != NULL) {
        // Check if the resulting string would be too long
        if ((strlen(str) - find_len + replace_len) >= MAXFILENAMELEN - 1) {
            return -1;
        }
        
        // Copy the part before the match
        strncpy(buffer, str, p - str);
        buffer[p - str] = '\0';
        
        // Add the replacement
        strcat(buffer, replace);
        
        // Add the rest of the string after the match
        strcat(buffer, p + find_len);
        
        // Copy back to the original string
        strcpy(str, buffer);
        
        // Find the next occurrence
        p = strstr(str, find);
    }
    
    return 1; // Replacements made
}


static void saveCreatedPaths(char ***pathList, int *size, char * newPath, int *len) {
    static char fname[] = "saveCreatedPaths()";
    char **newList = NULL;
    char errMsg[MAXLINELEN];
    
    if (*pathList == NULL) {
        *pathList = (char **)calloc(*size, sizeof(char *));
        if (*pathList == NULL) {
            return;
        }
        *len = 0;
    }

    if (len >= size) {
        newList = (char **)realloc(*pathList, (*size)*2*sizeof(char *));
        if (newList == NULL) {
            snprintf(errMsg, MAXLINELEN-1,"%s: realloc() failed for appending <%s> when creating sub-directory of job spool dir with dynamic patten.", 
                    fname, newPath);
            sbdSyslog(LOG_ERR, errMsg);            
            return;
        }
        *size *= 2;
        *pathList = newList;
    }

    (*pathList)[*len] = calloc(strlen(newPath+1), sizeof(char));
    strcpy((*pathList)[*len], newPath);
    (*len)++;
    return;
} /*saveCreatedPaths*/

static void removeCreatedPaths(char ***pathList, int *size, char * newPath, int *len) {
    static char fname[] = "removeCreatedPaths()";
    int i;
    char errMsg[MAXLINELEN];

    if (*pathList == NULL) {
        return;
    }

    for (i = *len - 1; i >=0; i--) {
        errno = 0;
        if (rmdir((*pathList)[i]) != 0) {
            snprintf(errMsg, MAXLINELEN-1, "%s: Failed to clear directory <%s>. %s", fname, (*pathList)[i], strerror(errno));
            sbdSyslog(LOG_ERR, errMsg);
            break;
        }
    }

    for (i = 0; i < *len; i++) {
        free((*pathList)[i]);
    }
    
    *len = 0;
    free(*pathList);
    *pathList = NULL;
    *size = 0;
}

static int createDirWithMode(char *path, mode_t mode) {
    static char fname[] = "createDirWithMode()";
    char errMsg[MAXLINELEN];

    errno = 0;
    if (mkdir(path, mode) == -1) {
        if (errno == EEXIST) {
            /*Directory already exists, just set permissions*/
            if (chmod(path, mode) == -1) {
                snprintf(errMsg, MAXLINELEN-1, "%s: chmod() failed to change directory <%s> as mode <%#o>. %s", fname, path, mode, strerror(errno));
                sbdSyslog(LOG_DEBUG, errMsg);
                return -1;
            }
            return 0;
        } else {
            snprintf(errMsg, MAXLINELEN-1, "%s: mkdir() failed to create directory <%s>. %s", fname, path, strerror(errno));
            sbdSyslog(LOG_DEBUG, errMsg);
            return -1;
        }
    }
    
    /*There may be umask setting, let us use chmod to modify mode*/
    if (chmod(path, mode) == -1) {
        snprintf(errMsg, MAXLINELEN-1, "%s: chmod() failed to change directory <%s> as mode <%#o>. %s", fname, path, mode, strerror(errno));
        sbdSyslog(LOG_DEBUG, errMsg);
        return -1;
    }

    return 0; /*Directory created*/
}

static int processJobSpoolDir(struct jobCard *jp) {
    static char fname[] = "processJobSpoolDir()";
    char errMsg[MAXLINELEN] = {0};
    char temp_path[MAXFILENAMELEN] = {0};
    char *first_place = NULL;
    char *p = NULL;
    char *str = NULL;
    char **pathList = NULL;
    int  pathSize = 10;
    int  pathCount = 0;
    int  ret = 0;
    
    if (jp->jobSpecs.jobSpoolDir[0] == '\0') {
        return -1;
    }

    if (logclass & LC_EXEC) {
        snprintf(errMsg, MAXLINELEN-1,"%s: job <%s> original jobSpoolDir is <%s>.", fname, lsb_jobidinstr(jp->jobSpecs.jobId), jp->jobSpecs.jobSpoolDir);
        sbdSyslog(LOG_DEBUG, errMsg);
    }

    /*Make a copy of the jobSpoolDir to work with*/
    strncpy(temp_path, jp->jobSpecs.jobSpoolDir, MAXFILENAMELEN - 1);
    
    /*Check if the path contains %U*/
    first_place = strstr(temp_path, "%U");
    
    if (first_place == NULL) {
        /*No %U found, just check if the directory exists*/
        if (my_access(jp->jobSpecs.jobSpoolDir, W_OK) != 0) {

            snprintf(errMsg, MAXLINELEN-1,"%s: job <%s> jobSpoolDir <%s> cannot be used. %s",
                    fname, 
                    lsb_jobidinstr(jp->jobSpecs.jobId),
                    jp->jobSpecs.jobSpoolDir,
                    (errno != 0 ? strerror(errno):""));
            sbdSyslog(LOG_WARNING, errMsg);
            return -1;
        }

        return 0;
    }
    
    /*Replace all %U occurrences with username*/
    if (replaceAllPattern(temp_path, "%U", jp->execUsername) <= 0) {
        snprintf(errMsg, MAXLINELEN-1,"%s: Failed to extend jobSpoolDir <%s> with username <%s> for job <%s>.",
                fname,
                jp->jobSpecs.jobSpoolDir,
                jp->execUsername,
                lsb_jobidinstr(jp->jobSpecs.jobId));
        sbdSyslog(LOG_WARNING, errMsg);
        jp->jobSpecs.jobSpoolDir[0] = '\0';
        return -1;
    }
    
    /*Now process the path*/
    str = temp_path;
    /*quick check*/
    if (my_access(temp_path, W_OK) == 0) {
        if (logclass & LC_EXEC) {
            snprintf(errMsg, MAXLINELEN-1, "%s: Job <%s> execution jobSpoolDir <%s>.", 
                    fname,
                    lsb_jobidinstr(jp->jobSpecs.jobId),
                    temp_path);
            sbdSyslog(LOG_DEBUG, errMsg);  
        }
        strncpy(jp->jobSpecs.jobSpoolDir, temp_path, MAXFILENAMELEN - 1);
        return 0;
    }
    /*check and try to create missing directory*/
    while (str[0] != '\0' && ((p = strchr(str, '/')) != NULL)) {
        mode_t mode;

        str = p + 1;
        if (p == temp_path) {
            /*Skip root directory*/
            continue;
        }

        *p = '\0'; /*Temporarily terminate the string*/
        
        /*Check each sub-directory*/
        errno = 0;
        ret = my_access(temp_path, F_OK);
        if (ret == -2) { /*check F_OK failed*/
            if (logclass & LC_EXEC) {
                snprintf(errMsg, MAXLINELEN-1, "%s: Job <%s> sub-directory <%s> of jobSpoolDir <%s> not exist, let us create it.",
                        fname,
                        lsb_jobidinstr(jp->jobSpecs.jobId),
                        temp_path,
                        jp->jobSpecs.jobSpoolDir);
                sbdSyslog(LOG_DEBUG, errMsg);   
            }
            if (p < first_place) {
                /*Before first %U - create with 777 if needed*/
                mode = 0777;
            } else {
                /*After first %U - create with 700 if needed*/
                mode = 0700;
            }
            if (createDirWithMode(temp_path, mode) < 0) {                
                snprintf(errMsg, MAXLINELEN-1, "%s: Failed to create sub-directory <%s> of jobSpoolDir <%s> for job <%s>, try to use <home>/.lsbatch. %s",
                        fname,
                        temp_path,
                        jp->jobSpecs.jobSpoolDir,
                        lsb_jobidinstr(jp->jobSpecs.jobId),
                        (errno != 0 ? strerror(errno):""));
                sbdSyslog(LOG_WARNING, errMsg);
                jp->jobSpecs.jobSpoolDir[0] = '\0';
                removeCreatedPaths(&pathList, &pathSize, temp_path, &pathCount);
                return -1;
            }
            saveCreatedPaths(&pathList, &pathSize, temp_path, &pathCount);
        } else if (ret != 0) {
            snprintf(errMsg, MAXLINELEN-1, "%s: Job <%s> directory <%s> of jobSpoolDir <%s> cannot be used. Try to use <home>/.lsbatch. %s",
                fname,
                lsb_jobidinstr(jp->jobSpecs.jobId),
                temp_path,
                jp->jobSpecs.jobSpoolDir,
                (errno != 0 ? strerror(errno):""));
            sbdSyslog(LOG_WARNING, errMsg);
            jp->jobSpecs.jobSpoolDir[0] = '\0';
            removeCreatedPaths(&pathList, &pathSize, temp_path, &pathCount);
            return -1;
        }

        *p = '/'; /*Restore the slash*/
    }
    
    /*Handle the last component (after last slash)*/
    if (str[0] != '\0') {
        errno = 0;

        ret = my_access(temp_path, W_OK|F_OK);
        if (ret == -2) {
            if (logclass & LC_EXEC) {
                snprintf(errMsg, MAXLINELEN-1, "%s: Job <%s> directory <%s> of jobSpoolDir <%s> not exist, let us create it.",
                        fname,
                        lsb_jobidinstr(jp->jobSpecs.jobId),
                        temp_path,
                        jp->jobSpecs.jobSpoolDir);
                sbdSyslog(LOG_DEBUG, errMsg);
            }
            if (createDirWithMode(temp_path, 0700) < 0) {     
                snprintf(errMsg, MAXLINELEN-1, "%s: Failed to create directory <%s> of jobSpoolDir <%s> for job <%s>. Try to use <home>/.lsbatch. %s",
                        fname,
                        temp_path,
                        jp->jobSpecs.jobSpoolDir,
                        lsb_jobidinstr(jp->jobSpecs.jobId),
                        (errno != 0 ? strerror(errno):""));
                sbdSyslog(LOG_WARNING, errMsg);
                jp->jobSpecs.jobSpoolDir[0] = '\0';
                removeCreatedPaths(&pathList, &pathSize, temp_path, &pathCount);                
                return -1;
            }             
        } else if (ret != 0) {
            snprintf(errMsg, MAXLINELEN-1, "%s: Job <%s> directory <%s> of jobSpoolDir <%s> cannot be used, try to use <home>/.lsbatch. %s",
                    fname,
                    lsb_jobidinstr(jp->jobSpecs.jobId),
                    temp_path,
                    jp->jobSpecs.jobSpoolDir,
                    (errno != 0 ? strerror(errno):""));
            sbdSyslog(LOG_WARNING, errMsg);
            jp->jobSpecs.jobSpoolDir[0] = '\0';
            removeCreatedPaths(&pathList, &pathSize, temp_path, &pathCount);
            return -1;
        }
    }
    
    /*Copy the result to output directory*/
    if (logclass & LC_EXEC) {
        snprintf(errMsg, MAXLINELEN-1, "%s: Job <%s> execution jobSpoolDir <%s>.", 
                fname,
                lsb_jobidinstr(jp->jobSpecs.jobId),
                temp_path);
        sbdSyslog(LOG_DEBUG, errMsg);
    }
    strncpy(jp->jobSpecs.jobSpoolDir, temp_path, MAXFILENAMELEN - 1);
    removeCreatedPaths(&pathList, &pathSize, temp_path, &pathCount);
    return 0;
}

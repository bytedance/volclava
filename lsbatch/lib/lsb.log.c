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

#include "lsb.h"
#include <errno.h>

#define   NL_SETN     13

extern int errno;

#include "../../lsf/lib/lib.queue.h"

bool_t  logMapFileEnable = FALSE;

static int readJobNew(char *, struct jobNewLog *);
static int readJobMod(char *, struct jobModLog *);
static int readJobStart(char *, struct jobStartLog *);
static int readJobStartAccept(char *, struct jobStartAcceptLog *);
static int readJobExecute(char *, struct jobExecuteLog *);
static int readJobStatus(char *, struct jobStatusLog *);
static int readSbdJobStatus(char *, struct sbdJobStatusLog *);
static int readJobSwitch(char *, struct jobSwitchLog *);
static int readJobMove(char *, struct jobMoveLog *);
static int readJobFinish (char *, struct jobFinishLog *, time_t);
static int readQueueCtrl(char *, struct queueCtrlLog *);
static int readHostCtrl(char *, struct hostCtrlLog *);

static int readMbdStart(char *, struct mbdStartLog *);
static int readMbdDie (char *, struct mbdDieLog *);
static int readChkpnt (char *, struct chkpntLog *);
static int readJobSigAct (char *, struct sigactLog *);
static int readMig (char *, struct migLog *);
static int readUnfulfill (char *, struct unfulfillLog *);
static int readLoadIndex (char *, struct loadIndexLog *);

static int readJobRequeue (char *, struct jobRequeueLog *);

static int readJobSignal (char *, struct signalLog *);
static int readJobMsg (char *, struct jobMsgLog *);
static int readJobMsgAck(char *, struct jobMsgAckLog *);
static int readJobClean(char *, struct jobCleanLog *);
static int readLogSwitch(char *, struct logSwitchLog *);

static int writeJobNew(FILE *, struct jobNewLog *);
static int writeJobMod(FILE *, struct jobModLog *);
static int writeJobStart(FILE *, struct jobStartLog *);
static int writeJobStartAccept(FILE *, struct jobStartAcceptLog *);
static int writeJobExecute(FILE *, struct jobExecuteLog *);
static int writeJobStatus(FILE *, struct jobStatusLog *);
static int writeSbdJobStatus(FILE *, struct sbdJobStatusLog *);
static int writeJobSwitch(FILE *, struct jobSwitchLog *);
static int writeJobMove(FILE *, struct jobMoveLog *);

static int writeQueueCtrl(FILE *, struct queueCtrlLog *);
static int writeHostCtrl(FILE *, struct hostCtrlLog *);
static int writeMbdStart(FILE *, struct mbdStartLog *);
static int writeMbdDie (FILE *, struct mbdDieLog *);
static int writeUnfulfill (FILE *, struct unfulfillLog *);
static int writeMig (FILE *, struct migLog *);
static int writeChkpnt (FILE *, struct chkpntLog *);
static int writeJobSigAct (FILE *, struct sigactLog *);
static int writeJobFinish (FILE *, struct jobFinishLog *);
static int writeLoadIndex (FILE *, struct loadIndexLog *);

static int writeJobSignal(FILE *log_fp, struct signalLog *);
static int writeJobMsg(FILE *log_fp, struct jobMsgLog *);
static int writeJobMsgAck(FILE *log_fp, struct jobMsgLog *);
static int writeJobRqueue(FILE *log_fp, struct jobRequeueLog *);
static int writeJobClean (FILE *log_fp, struct jobCleanLog *);

static int writeLogSwitch(FILE* , struct logSwitchLog*);
static int writeJobForce(FILE* , struct jobForceRequestLog*);
static int readJobForce(char *, struct jobForceRequestLog* );

static int writeJobAttrSet(FILE* , struct jobAttrSetLog*);
static int readJobAttrSet(char *, struct jobAttrSetLog* );

static void freeLogRec(struct eventRec *);

struct eventRec * lsbGetNextJobEvent (struct eventLogHandle *, \
                                      int *, int, LS_LONG_INT *, struct jobIdIndexS *);
static struct eventRec * lsbGetNextJobRecFromFile(FILE *, int *, int, \
                                                  LS_LONG_INT *);
static int checkJobEventAndJobId(char *, int, int,  LS_LONG_INT *);
static int getEventTypeAndKind(char *, int *);
static void readEventRecord (char *, struct eventRec *);
int lsb_readeventrecord(char *, struct eventRec *);
#define   EVENT_JOB_RELATED     1
#define   EVENT_NON_JOB_RELATED 0

int getJobIdIndexFromEventFile (char *, struct sortIntList *, time_t *);
int getJobIdFromEvent (char *, int);
int writeJobIdIndexToIndexFile (FILE *, struct sortIntList *, time_t);
int updateJobIdIndexFile (char *, char *, int);
int getNextFileNumFromIndexS (struct jobIdIndexS *, int, LS_LONG_INT *);


struct eventLogHandle*  lsb_openelog (struct eventLogFile *, int *);
struct eventRec*        lsb_getelogrec (struct eventLogHandle *, int *);
void lsb_closeelog(struct eventLogHandle *);
void   countLineNum(FILE *, long, int *);

struct eventRec *lsb_geteventrec_ex(FILE *log_fp, int *LineNum,
                                    char* usedLine);
time_t lsb_getAcctFileTime(char * fileName);

#define copyQStr(line, maxLen, nonNil, destStr)    {            \
        char *tmpLine;                                          \
        int ccount;                                             \
        if ((tmpLine = (char *) malloc (strlen(line))) == NULL) \
            return (LSBE_NO_MEM);                               \
        if ((ccount = stripQStr(line, tmpLine)) < 0) {          \
            FREEUP (tmpLine);                                   \
            return (LSBE_EVENT_FORMAT);                         \
        }                                                       \
        line += ccount + 1;                                     \
        if (strlen(tmpLine) >= maxLen                           \
            || (nonNil && strlen(tmpLine)==0)) {                \
            FREEUP (tmpLine);                                   \
            return (LSBE_EVENT_FORMAT);                         \
        }                                                       \
        strcpy(destStr, tmpLine);                               \
        FREEUP (tmpLine);                                       \
    }

#define saveQStr(line, destStr)  {                              \
        char *tmpLine;                                          \
        int ccount;                                             \
        if ((tmpLine = (char *) malloc (strlen(line))) == NULL) \
            return (LSBE_NO_MEM);                               \
        if ((ccount = stripQStr(line, tmpLine)) < 0)  {         \
            FREEUP (tmpLine);                                   \
            return (LSBE_EVENT_FORMAT);                         \
        }                                                       \
        line += ccount + 1;                                     \
        if ((destStr = putstr_(tmpLine)) == NULL)     {         \
            FREEUP (tmpLine);                                   \
            return (LSBE_NO_MEM);                               \
        }                                                       \
        FREEUP (tmpLine);                                       \
    }

float version;


struct eventLogHandle *
lsb_openelog (struct eventLogFile *ePtr, int *lineNum)
{
    static char fname[] = "lsb_openelog";
    static struct eventLogHandle *eLogHandle = NULL;
    int lastOpenFile, curOpenFile;
    char ch, eventFile[MAXFILENAMELEN];
    FILE *elog_fp;
    int i, oldFormat, findLast;
    time_t eventTime;
    struct stat st;

    eLogHandle = (struct eventLogHandle *) calloc (1,
                                                   sizeof (struct eventLogHandle));

    if (eLogHandle == NULL) {
        lsberrno = LSBE_NO_MEM;
        return NULL;
    }

    if (ePtr->eventDir == NULL) {

        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5500,
                                         "%s: event directory is NULL"), fname); /* catgets 5500 */
        return NULL;
    }



    curOpenFile = -1;
    lastOpenFile = -1;
    findLast = FALSE;
    oldFormat = FALSE;

    for (i = 1; ; i++) {
        sprintf(eventFile, "%s/lsb.events.%d", ePtr->eventDir, i);
        if (stat(eventFile, &st) == 0) {
            if ((elog_fp = fopen(eventFile, "r")) == NULL) {
                lsberrno = LSBE_SYS_CALL;
                return NULL;
            } else {
                if (fscanf(elog_fp, "%c%ld", &ch, &eventTime) != 2
                    || ch != '#') {
                    ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5501,
                                                     "%s: fscanf(%s) failed: old event file format"),  /* catgets 5501 */
                              fname, eventFile);
                    fclose(elog_fp);
                    oldFormat = TRUE;
                    if (findLast == FALSE)
                        lastOpenFile = i - 1;
                    curOpenFile = i - 1;
                    break;
                }
                fclose (elog_fp);
                if (ePtr->endTime >  eventTime && !findLast) {
                    findLast = TRUE;
                    lastOpenFile = i - 1;
                }

                if (ePtr->beginTime < eventTime) {
                    continue;
                } else {
                    curOpenFile = i--;
                    break;
                }
            }

        } else  {
            if (findLast == FALSE)
                lastOpenFile = i - 1;
            curOpenFile = i - 1;
            break;
        }
    }

    if (oldFormat == TRUE) {
        if (curOpenFile == -1)  {
            if (findLast == FALSE) {

                curOpenFile = 0;
                lastOpenFile = -1;
            } else {

                curOpenFile = i - 1;
            }
        }
    }

    if (curOpenFile == lastOpenFile)
        lastOpenFile = -1;

    if (curOpenFile >= 0) {
        if (curOpenFile == 0)
            sprintf(eventFile, "%s/lsb.events", ePtr->eventDir);
        else
            sprintf(eventFile, "%s/lsb.events.%d", ePtr->eventDir, curOpenFile);
        if ((elog_fp = fopen (eventFile, "r")) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", eventFile);
            return NULL;
        }

        if (curOpenFile == 0)  {
            int pos;

            if (fscanf(elog_fp, "%c%d ", &ch, &pos) != 2 || ch != '#') {
                ls_syslog(LOG_WARNING, I18N(5501,
                                            "%s: fscanf(%s) failed: event file is old format"), fname, eventFile);
                pos = 0;
            } else {
                *lineNum = 1;
                countLineNum(elog_fp, pos, lineNum);
            }
            if (fseek (elog_fp, pos, SEEK_SET) != 0)
                ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "fseek", pos);
        }
    } else {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5505,
                                         "%s: current open event file number < 0"), fname); /* catgets 5505 */
        return  NULL;
    }

    eLogHandle->fp = elog_fp;
    strcpy (eLogHandle->openEventFile, eventFile);
    eLogHandle->curOpenFile = curOpenFile;
    eLogHandle->lastOpenFile = lastOpenFile;

    return (eLogHandle);
}

struct eventRec *
lsb_getelogrec (struct eventLogHandle *ePtr, int *lineNum)
{
    static char fname[] = "lsb_getelogrec";
    struct eventRec *logRec;
    FILE *newfp;
    char *sp, eventFile[MAXFILENAMELEN];

    if (ePtr->fp != NULL)
        logRec = lsb_geteventrec(ePtr->fp, lineNum);

    if (logRec == NULL && ePtr->lastOpenFile >= 0
        && ePtr->curOpenFile > ePtr->lastOpenFile) {


        fclose(ePtr->fp);

        if ((sp = strstr(ePtr->openEventFile, "lsb.events")))
            *(sp-1) = '\0';

        if (ePtr->curOpenFile == 1) {
            sprintf  (eventFile, "%s/lsb.events", ePtr->openEventFile);
            ePtr->curOpenFile = 0;
            ePtr->lastOpenFile = -1;
        } else
            sprintf  (eventFile, "%s/lsb.events.%d", ePtr->openEventFile,
                      --(ePtr->curOpenFile));

        strcpy(ePtr->openEventFile, eventFile);

        if ((newfp = fopen (eventFile, "r")) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen", eventFile);
            return (NULL);
        } else if (ePtr->curOpenFile == 0) {
            char ch;
            int pos;


            if (fscanf(newfp, "%c%d ", &ch, &pos) != 2
                || ch != '#') {
                ls_syslog(LOG_ERR, I18N(5501,
                                        "%s: fscanf(%s) failed: old event file format"), fname, eventFile);
                pos = 0;
            } else {
                *lineNum = 1;
                countLineNum(newfp, pos, lineNum);
            }
            if (fseek (newfp, pos, SEEK_SET) != 0)
                ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M,  fname, "fseek", pos);
        }
        logRec  = lsb_geteventrec(newfp, lineNum);
        ePtr->fp = newfp;
    }
    return (logRec);
}

void
lsb_closeelog(struct eventLogHandle *eventLogHandle)
{
    if (eventLogHandle->fp) {
        fclose(eventLogHandle->fp);
        eventLogHandle->fp = NULL;
    }
}

struct eventRec *
lsb_geteventrec(FILE *log_fp, int *LineNum)
{
    return lsb_geteventrec_ex(log_fp, LineNum, NULL);
}


struct eventRec *
lsb_geteventrec_ex(FILE *log_fp, int *LineNum, char* usedLine)
{
    static  char            fname[] = "lsb_geteventrec";
    int                     cc;
    int                     ccount;
    char*                   line;
    char                    etype[MAX_LSB_NAME_LEN];
    char*                   namebuf = NULL;
    static struct eventRec* logRec;
    int                     eventKind;
    int tempTimeStamp;

    if (logRec != NULL)  {
        freeLogRec(logRec);
        free(logRec);
    }


    logRec = (struct eventRec *) calloc (1, sizeof (struct eventRec));
    if (logRec == NULL) {
        lsberrno = LSBE_NO_MEM;
        return NULL;
    }

    (*LineNum)++;

    if ((line = getNextLine_(log_fp, FALSE)) == NULL) {
        if (lserrno == LSE_NO_MEM) {
            lsberrno = LSBE_NO_MEM;
        } else {
            lsberrno = LSBE_EOF;
        }
        return NULL;
    }

    while (*line == '#') {

        line = getNextLine_(log_fp, FALSE);
        if (line == NULL) {
            fclose(log_fp);
            lsberrno = LSBE_EOF;
            return NULL;
        }
    }

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG2, "%s: line=%s", fname, line);

    if (usedLine) {
        strcpy(usedLine, line);
    }

    namebuf = (char *)calloc(1, strlen(line)+1);
    if (namebuf == NULL) {
        lsberrno = LSBE_NO_MEM;
        return (NULL);
    }


    if ((ccount = stripQStr(line, namebuf)) < 0
        || strlen(line) == ccount
        || strlen(namebuf) >= MAX_LSB_NAME_LEN) {
        lsberrno = LSBE_EVENT_FORMAT;
        free(namebuf);
        return (NULL);
    }

    strcpy(etype, namebuf);
    line += ccount + 1;

    if ((ccount = stripQStr(line, namebuf)) < 0
        || strlen(line) == ccount
        || strlen(namebuf) >= MAX_VERSION_LEN) {
        lsberrno = LSBE_EVENT_FORMAT;
        free(namebuf);
        return (NULL);
    }

    strcpy(logRec->version, namebuf);
    if ((version = atof (logRec->version)) <=0.0) {
        lsberrno = LSBE_EVENT_FORMAT;
        free(namebuf);
        return (NULL);
    }

    line += ccount + 1;


    free(namebuf);


    cc = sscanf(line, "%d%n", &tempTimeStamp, &ccount);
    logRec->eventTime = tempTimeStamp;

    if (cc != 1) {
        lsberrno = LSBE_EVENT_FORMAT;
        return NULL;
    }
    line += ccount + 1;

    if ((logRec->type = getEventTypeAndKind(etype, &eventKind)) == -1)
        return NULL;
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG2, "%s: log.type=%x", fname, logRec->type);

    readEventRecord (line, logRec);

    if (lsberrno == LSBE_NO_ERROR)
        return (logRec);

    return NULL;

}

static void
freeLogRec(struct eventRec *logRec)
{
    int i;

    switch (logRec->type) {
        case EVENT_JOB_NEW:
        case EVENT_JOB_MODIFY:
            if (logRec->eventLog.jobNewLog.numAskedHosts) {
                for (i=0; i<logRec->eventLog.jobNewLog.numAskedHosts; i++)
                    free(logRec->eventLog.jobNewLog.askedHosts[i]);
            }
            if (logRec->eventLog.jobNewLog.askedHosts)
                free(logRec->eventLog.jobNewLog.askedHosts);
            if (logRec->eventLog.jobNewLog.nxf)
                free(logRec->eventLog.jobNewLog.xf);
            if (logRec->eventLog.jobNewLog.resReq)
                free(logRec->eventLog.jobNewLog.resReq);
            if (logRec->eventLog.jobNewLog.dependCond)
                free(logRec->eventLog.jobNewLog.dependCond);
            if (logRec->eventLog.jobNewLog.preExecCmd)
                free(logRec->eventLog.jobNewLog.preExecCmd);
            if (logRec->eventLog.jobNewLog.postExecCmd)
                free(logRec->eventLog.jobNewLog.postExecCmd);
            if (logRec->eventLog.jobNewLog.mailUser)
                free(logRec->eventLog.jobNewLog.mailUser);
            if (logRec->eventLog.jobNewLog.projectName)
                free(logRec->eventLog.jobNewLog.projectName);
            if (logRec->eventLog.jobNewLog.schedHostType)
                free(logRec->eventLog.jobNewLog.schedHostType);
            if (logRec->eventLog.jobNewLog.loginShell)
                free(logRec->eventLog.jobNewLog.loginShell);
            return;
        case EVENT_JOB_MODIFY2:
            FREEUP(logRec->eventLog.jobModLog.userName);
            FREEUP(logRec->eventLog.jobModLog.jobIdStr);
            FREEUP(logRec->eventLog.jobModLog.jobName);
            FREEUP(logRec->eventLog.jobModLog.queue);
            if (logRec->eventLog.jobModLog.numAskedHosts) {
                for (i=0; i<logRec->eventLog.jobModLog.numAskedHosts; i++)
                    FREEUP(logRec->eventLog.jobModLog.askedHosts[i]);
            }
            FREEUP(logRec->eventLog.jobModLog.askedHosts);

            FREEUP(logRec->eventLog.jobModLog.resReq);
            FREEUP(logRec->eventLog.jobModLog.hostSpec);
            FREEUP(logRec->eventLog.jobModLog.dependCond);
            FREEUP(logRec->eventLog.jobModLog.subHomeDir);
            FREEUP(logRec->eventLog.jobModLog.inFile);
            FREEUP(logRec->eventLog.jobModLog.outFile);
            FREEUP(logRec->eventLog.jobModLog.errFile);
            FREEUP(logRec->eventLog.jobModLog.command);
            FREEUP(logRec->eventLog.jobModLog.inFileSpool);
            FREEUP(logRec->eventLog.jobModLog.commandSpool);
            FREEUP(logRec->eventLog.jobModLog.chkpntDir);
            FREEUP(logRec->eventLog.jobModLog.xf);
            FREEUP(logRec->eventLog.jobModLog.jobFile);
            FREEUP(logRec->eventLog.jobModLog.fromHost);
            FREEUP(logRec->eventLog.jobModLog.cwd);
            FREEUP(logRec->eventLog.jobModLog.preExecCmd);
            FREEUP(logRec->eventLog.jobModLog.postExecCmd);
            FREEUP(logRec->eventLog.jobModLog.mailUser);
            FREEUP(logRec->eventLog.jobModLog.projectName);
            FREEUP(logRec->eventLog.jobModLog.loginShell);
            FREEUP(logRec->eventLog.jobModLog.schedHostType);
            return;

        case EVENT_JOB_START:
        case EVENT_PRE_EXEC_START:
            if (logRec->eventLog.jobStartLog.numExHosts) {
                for (i=0; i<logRec->eventLog.jobStartLog.numExHosts; i++)
                    free(logRec->eventLog.jobStartLog.execHosts[i]);
            }
            if (logRec->eventLog.jobStartLog.execHosts)
                free(logRec->eventLog.jobStartLog.execHosts);
            FREEUP (logRec->eventLog.jobStartLog.queuePreCmd);
            FREEUP (logRec->eventLog.jobStartLog.queuePostCmd);

            return;

        case EVENT_JOB_START_ACCEPT:
            return;

        case EVENT_LOAD_INDEX:
            for (i = 0; i < logRec->eventLog.loadIndexLog.nIdx; i++)
                free(logRec->eventLog.loadIndexLog.name[i]);
            if (logRec->eventLog.loadIndexLog.nIdx)
                free(logRec->eventLog.loadIndexLog.name);
            return;

        case EVENT_MIG:
            for (i = 0; i < logRec->eventLog.migLog.numAskedHosts; i++)
                free(logRec->eventLog.migLog.askedHosts[i]);
            if (logRec->eventLog.migLog.numAskedHosts)
                free(logRec->eventLog.migLog.askedHosts);
            return;

        case EVENT_JOB_FINISH:
            if (logRec->eventLog.jobFinishLog.resReq)
                free(logRec->eventLog.jobFinishLog.resReq);
            if (logRec->eventLog.jobFinishLog.dependCond)
                free(logRec->eventLog.jobFinishLog.dependCond);
            if (logRec->eventLog.jobFinishLog.preExecCmd)
                free(logRec->eventLog.jobFinishLog.preExecCmd);
            if (logRec->eventLog.jobFinishLog.postExecCmd)
                free(logRec->eventLog.jobFinishLog.postExecCmd);

            if (logRec->eventLog.jobFinishLog.numAskedHosts) {
                for (i=0; i<logRec->eventLog.jobFinishLog.numAskedHosts; i++)
                    free(logRec->eventLog.jobFinishLog.askedHosts[i]);
            }
            if (logRec->eventLog.jobFinishLog.askedHosts)
                free(logRec->eventLog.jobFinishLog.askedHosts);

            if (logRec->eventLog.jobFinishLog.numExHosts) {
                for (i=0; i<logRec->eventLog.jobFinishLog.numExHosts; i++)
                    free(logRec->eventLog.jobFinishLog.execHosts[i]);
            }
            if (logRec->eventLog.jobFinishLog.execHosts)
                free(logRec->eventLog.jobFinishLog.execHosts);
            if (logRec->eventLog.jobFinishLog.mailUser)
                free(logRec->eventLog.jobFinishLog.mailUser);
            if (logRec->eventLog.jobFinishLog.projectName)
                free(logRec->eventLog.jobFinishLog.projectName);
            if (logRec->eventLog.jobFinishLog.loginShell)
                free(logRec->eventLog.jobFinishLog.loginShell);
            return;
        case EVENT_JOB_SIGNAL:
            free(logRec->eventLog.signalLog.signalSymbol);
            return;
        case EVENT_JOB_SIGACT:
            FREEUP(logRec->eventLog.sigactLog.signalSymbol);
            return;
        case EVENT_JOB_EXECUTE:
            free(logRec->eventLog.jobExecuteLog.execCwd);
            free(logRec->eventLog.jobExecuteLog.execHome);
            free(logRec->eventLog.jobExecuteLog.execUsername);
            return;
        case EVENT_JOB_MSG:
            free(logRec->eventLog.jobMsgLog.src);
            free(logRec->eventLog.jobMsgLog.dest);
            free(logRec->eventLog.jobMsgLog.msg);
            return;
        case EVENT_JOB_FORCE:
            for (i = 0; i < logRec->eventLog.jobForceRequestLog.numExecHosts; i++) {
                if (logRec->eventLog.jobForceRequestLog.execHosts[i])
                    free(logRec->eventLog.jobForceRequestLog.execHosts[i]);
            }
            if (logRec->eventLog.jobForceRequestLog.execHosts)
                free(logRec->eventLog.jobForceRequestLog.execHosts);
            return;
        case EVENT_LOG_SWITCH:
            return;
        default:
            return;
    }
}


static int
readJobNew(char *line, struct jobNewLog *jobNewLog)
{
    int  i, cc, ccount;
    int  tmpSubmit, tmpBegin, tmpTerm;




    cc = sscanf(line, "%d%d%d%d%d%d%d%d%d%d%n",
                &(jobNewLog->jobId),
                &(jobNewLog->userId),
                &(jobNewLog->options),
                &(jobNewLog->numProcessors),
                &(tmpSubmit),
                &(tmpBegin),
                &(tmpTerm),
                &(jobNewLog->sigValue),
                &(jobNewLog->chkpntPeriod),
                &(jobNewLog->restartPid),
                &ccount);

    if (cc != 10)
        return (LSBE_EVENT_FORMAT);
    jobNewLog->submitTime = tmpSubmit;
    jobNewLog->beginTime = tmpBegin;
    jobNewLog->termTime = tmpTerm;

    line += ccount + 1;

    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobNewLog->userName);

    for (i = 0; i < LSF_RLIM_NLIMITS; i++) {
        cc = sscanf(line, "%d%n", &(jobNewLog->rLimits[i]), &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    }

    copyQStr(line, MAXHOSTNAMELEN, 0, jobNewLog->hostSpec);

    cc = sscanf(line, "%f%d%n", &jobNewLog->hostFactor,
                &jobNewLog->umask, &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobNewLog->queue);
    saveQStr(line, jobNewLog->resReq);
    copyQStr(line, MAXHOSTNAMELEN, 1, jobNewLog->fromHost);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->cwd);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->chkpntDir);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->inFile);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->outFile);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->errFile);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->subHomeDir);
    copyQStr(line, MAXFILENAMELEN, 1, jobNewLog->jobFile);

    cc = sscanf(line, "%d%n", &jobNewLog->numAskedHosts, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobNewLog->numAskedHosts > 0) {
        jobNewLog->askedHosts = (char **)
            calloc(jobNewLog->numAskedHosts, sizeof (char *));
        if (jobNewLog->askedHosts == NULL) {
            jobNewLog->numAskedHosts = 0;
            return (LSBE_NO_MEM);
        }

        for (i = 0; i < jobNewLog->numAskedHosts; i++) {
            char hName[MAXLINELEN];
            if ((ccount = stripQStr(line, hName)) < 0) {

                jobNewLog->numAskedHosts = i;
                return (LSBE_EVENT_FORMAT);
            }
            jobNewLog->askedHosts[i] = putstr_(hName);
            if (jobNewLog->askedHosts[i] == NULL) {
                jobNewLog->numAskedHosts = i;
                return (LSBE_NO_MEM);
            }
            line += ccount + 1;
        }
    }

    saveQStr(line, jobNewLog->dependCond);
    saveQStr(line, jobNewLog->preExecCmd);
    if (jobNewLog->options & SUB_POST_EXEC) {
        saveQStr(line, jobNewLog->postExecCmd);
    }

    copyQStr(line, MAX_CMD_DESC_LEN, 0, jobNewLog->jobName);
    copyQStr(line, MAX_CMD_DESC_LEN, 0, jobNewLog->command);

    cc = sscanf(line, "%d%n", &jobNewLog->nxf, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobNewLog->nxf > 0) {
        jobNewLog->xf = (struct xFile *)
            calloc(jobNewLog->nxf, sizeof(struct xFile));
        if (jobNewLog->xf == NULL) {
            jobNewLog->nxf = 0;
            return (LSBE_NO_MEM);
        }
    }

    for (i = 0; i < jobNewLog->nxf; i++) {
        copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->xf[i].subFn);
        copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->xf[i].execFn);
        cc = sscanf(line, "%d%n", &jobNewLog->xf[i].options, &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    }

    saveQStr(line, jobNewLog->mailUser);
    saveQStr(line, jobNewLog->projectName);

    if (jobNewLog->options & SUB_INTERACTIVE) {
        cc = sscanf(line, "%d%n", &jobNewLog->niosPort, &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    } else
        jobNewLog->niosPort = 0;
    cc = sscanf(line, "%d%n", &jobNewLog->maxNumProcessors, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    saveQStr(line, jobNewLog->schedHostType);
    saveQStr(line, jobNewLog->loginShell);

    cc = sscanf(line, "%d%d%n", &(jobNewLog->options2),
                &(jobNewLog->idx),  &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);

    if ((jobNewLog->options2 & SUB2_BSUB_BLOCK)){
        line += ccount;
        if (*line != '\0') {
            cc = sscanf(line, "%d%n", &jobNewLog->niosPort, &ccount);
            if (cc != 1)
                return (LSBE_EVENT_FORMAT);
        }
    }


    if (!(jobNewLog->options & SUB_RLIMIT_UNIT_IS_KB)) {
        convertRLimit(jobNewLog->rLimits, 1);
    }

    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->inFileSpool);
    copyQStr(line, MAXFILENAMELEN, 0, jobNewLog->commandSpool);
    copyQStr(line, MAXPATHLEN, 0, jobNewLog->jobSpoolDir);

    cc = sscanf(line, "%d%n", &jobNewLog->userPriority, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    return (LSBE_NO_ERROR);

}

static int
readJobMod(char *line, struct jobModLog *jobModLog)
{
    int i, cc, ccount;

    saveQStr (line, jobModLog->jobIdStr);

    cc = sscanf(line, "%d%d%d%d%n",
                &(jobModLog->options), &(jobModLog->options2),
                &(jobModLog->delOptions), &(jobModLog->userId),
                &ccount);
    if (cc != 4) {
        return (LSBE_EVENT_FORMAT);
    }
    line += ccount + 1;

    saveQStr (line, jobModLog->userName);
    cc = sscanf(line, "%d%d%d%d%d%d%d%n",
                &(jobModLog->submitTime), &(jobModLog->umask),
                &(jobModLog->numProcessors), &(jobModLog->beginTime),
                &(jobModLog->termTime), &(jobModLog->sigValue),
                &(jobModLog->restartPid), &ccount);
    if (cc != 7)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobModLog->options & SUB_JOB_NAME)
        saveQStr (line, jobModLog->jobName);
    if (jobModLog->options & SUB_QUEUE)
        saveQStr (line, jobModLog->queue);

    if (jobModLog->options & SUB_HOST) {
        cc = sscanf(line, "%d%n", &(jobModLog->numAskedHosts), &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
        if (jobModLog->numAskedHosts) {
            jobModLog->askedHosts = (char **)
                calloc(jobModLog->numAskedHosts, sizeof (char *));
            if (jobModLog->askedHosts == NULL) {
                jobModLog->numAskedHosts = 0;
                return (LSBE_NO_MEM);
            }

            for (i = 0; i < jobModLog->numAskedHosts; i++) {
                saveQStr (line, jobModLog->askedHosts[i]);
            }
        }
    }

    if (jobModLog->options & SUB_RES_REQ)
        saveQStr (line, jobModLog->resReq);
    for(i = 0; i < LSF_RLIM_NLIMITS; i++) {
        cc = sscanf(line, "%d%n",
                    &(jobModLog->rLimits[i]), &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    }
    if (jobModLog->options & SUB_HOST_SPEC)
        saveQStr (line, jobModLog->hostSpec);

    if (jobModLog->options & SUB_DEPEND_COND)
        saveQStr (line, jobModLog->dependCond);

    saveQStr (line, jobModLog->subHomeDir);
    if ( (jobModLog->options & SUB_IN_FILE)
         || (jobModLog->options2 & SUB2_IN_FILE_SPOOL) )
        saveQStr (line, jobModLog->inFile);
    if (jobModLog->options & SUB_OUT_FILE)
        saveQStr (line, jobModLog->outFile);
    if (jobModLog->options & SUB_ERR_FILE)
        saveQStr (line, jobModLog->errFile);
    if (jobModLog->options2 & SUB2_MODIFY_CMD)
        saveQStr (line, jobModLog->command);

    if (jobModLog->options & SUB_CHKPNT_PERIOD) {
        cc = sscanf(line, "%d%n",
                    &(jobModLog->chkpntPeriod), &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    }
    if (jobModLog->options & SUB_CHKPNT_DIR)
        saveQStr (line, jobModLog->chkpntDir);
    if (jobModLog->options & SUB_OTHER_FILES) {
        cc = sscanf(line, "%d%n",
                    &(jobModLog->nxf), &ccount);
        if (cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
        if (jobModLog->nxf > 0) {
            jobModLog->xf = (struct xFile *)
                calloc(jobModLog->nxf, sizeof(struct xFile));
            if (jobModLog->xf == NULL) {
                jobModLog->nxf = 0;
                return (LSBE_NO_MEM);
            }
        }
        for (i = 0; i < jobModLog->nxf; i++) {
            copyQStr(line, MAXFILENAMELEN, 0, jobModLog->xf[i].subFn);
            copyQStr(line, MAXFILENAMELEN, 0, jobModLog->xf[i].execFn);
            cc = sscanf(line, "%d%n", &jobModLog->xf[i].options, &ccount);
            if (cc != 1)
                return (LSBE_EVENT_FORMAT);
            line += ccount + 1;
        }
    }
    saveQStr (line, jobModLog->jobFile);
    saveQStr (line, jobModLog->fromHost);
    saveQStr (line, jobModLog->cwd);

    if (jobModLog->options & SUB_PRE_EXEC)
        saveQStr (line, jobModLog->preExecCmd);
    if (jobModLog->options & SUB_POST_EXEC)
        saveQStr (line, jobModLog->postExecCmd);
    if (jobModLog->options & SUB_MAIL_USER)
        saveQStr (line, jobModLog->mailUser);
    if (jobModLog->options & SUB_PROJECT_NAME)
        saveQStr (line, jobModLog->projectName);

    cc= sscanf(line, "%d%d%n",
               &(jobModLog->niosPort), &(jobModLog->maxNumProcessors),
               &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobModLog->options & SUB_LOGIN_SHELL)
        saveQStr (line, jobModLog->loginShell);
    saveQStr (line, jobModLog->schedHostType);


    cc = sscanf(line, "%d%n", &(jobModLog->delOptions2), &ccount);
    if ( cc != 1) {
        return (LSBE_EVENT_FORMAT);
    }
    line += ccount + 1;

    if (jobModLog->options2 & SUB2_IN_FILE_SPOOL)
        saveQStr (line, jobModLog->inFileSpool);

    if (jobModLog->options2 & SUB2_JOB_CMD_SPOOL)
        saveQStr (line, jobModLog->commandSpool);

    if (jobModLog->options2 & SUB2_JOB_PRIORITY ) {
        cc = sscanf(line, "%d%n", &(jobModLog->userPriority), &ccount);
        if ( cc != 1)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    }

    return(LSBE_NO_ERROR);
}

static int
readJobStart(char *line, struct jobStartLog *jobStartLog)
{
    static char  fname[] = "readJobStart";
    int i, cc, ccount;

    cc = sscanf(line, "%d%d%d%d%f%d%n",
                &(jobStartLog->jobId),
                &(jobStartLog->jStatus),
                &(jobStartLog->jobPid),
                &(jobStartLog->jobPGid),
                &(jobStartLog->hostFactor),
                &(jobStartLog->numExHosts),
                &ccount );
    if (cc != 6)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobStartLog->numExHosts == 0) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5502,
                                         "%s: The number of execution hosts is zero for job <%d>"), /* catgets 5502 */
                  fname,
                  jobStartLog->jobId);
        return (LSBE_EVENT_FORMAT);
    }

    jobStartLog->execHosts = (char **)
        calloc(jobStartLog->numExHosts, sizeof (char *));
    if (jobStartLog->execHosts == NULL) {
        jobStartLog->numExHosts = 0;
        return (LSBE_NO_MEM);
    }

    for (i=0; i<jobStartLog->numExHosts; i++) {
        char hName[MAXLINELEN];
        if ((ccount = stripQStr(line, hName)) < 0) {
            jobStartLog->numExHosts = i;
            return (LSBE_EVENT_FORMAT);
        }
        jobStartLog->execHosts[i] = putstr_(hName);
        if (jobStartLog->execHosts[i] == NULL) {
            jobStartLog->numExHosts = i;
            return (LSBE_NO_MEM);
        }
        line += ccount + 1;
    }
    saveQStr(line, jobStartLog->queuePreCmd);
    saveQStr(line, jobStartLog->queuePostCmd);

    cc = sscanf(line, "%d%n", &(jobStartLog->jFlags), &ccount);
    if (cc != 1) {
        return (LSBE_EVENT_FORMAT);
    }
    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(jobStartLog->idx),  &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    return (LSBE_NO_ERROR);

}

static int
readJobStartAccept(char *line, struct jobStartAcceptLog *jobStartAcceptLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%d%d%n",
                &(jobStartAcceptLog->jobId),
                &(jobStartAcceptLog->jobPid),
                &(jobStartAcceptLog->jobPGid),
                &ccount );
    if (cc != 3)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(jobStartAcceptLog->idx), &ccount );
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    return (LSBE_NO_ERROR);

}

static int
readJobExecute(char *line, struct jobExecuteLog *jobExecuteLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%d%d%n",
                &(jobExecuteLog->jobId),
                &(jobExecuteLog->execUid),
                &(jobExecuteLog->jobPGid),
                &ccount );
    if (cc != 3)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    saveQStr(line, jobExecuteLog->execCwd);
    saveQStr(line, jobExecuteLog->execHome);
    saveQStr(line, jobExecuteLog->execUsername);

    cc = sscanf(line, "%d%n", &jobExecuteLog->jobPid, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(jobExecuteLog->idx),  &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    return (LSBE_NO_ERROR);

}



static int
readJobStatus(char *line, struct jobStatusLog *jobStatusLog)
{
    int cc, ccount;
    int tmpEnd;

    cc = sscanf(line, "%d%d%d%d%f%d%d%n",
                &(jobStatusLog->jobId),
                &(jobStatusLog->jStatus),
                &(jobStatusLog->reason),
                &(jobStatusLog->subreasons),
                &(jobStatusLog->cpuTime),
                &(tmpEnd),
                &(jobStatusLog->ru),
                &ccount);
    if (cc != 7)
        return (LSBE_EVENT_FORMAT);
    jobStatusLog->endTime = tmpEnd;

    line += ccount + 1;

    if (jobStatusLog->ru) {
        if ((cc =  str2lsfRu(line, &jobStatusLog->lsfRusage, &ccount)) != 19)
            return (LSBE_EVENT_FORMAT);
        line += ccount + 1;
    }

    cc = sscanf(line, "%d%n", &(jobStatusLog->exitStatus), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    cc = sscanf(line, "%d%n", &(jobStatusLog->idx),  &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    return (LSBE_NO_ERROR);
}


static int
readSbdJobStatus(char *line, struct sbdJobStatusLog *jobStatusLog)
{
    int cc, ccount;
    int tmpActPeriod;

    cc = sscanf(line, "%d%d%d%d%d%d%d%d%d%d%d%d%n",
                &(jobStatusLog->jobId),
                &(jobStatusLog->jStatus),
                &(jobStatusLog->reasons),
                &(jobStatusLog->subreasons),
                &(jobStatusLog->actPid),
                &(jobStatusLog->actValue),
                &(tmpActPeriod),
                &(jobStatusLog->actFlags),
                &(jobStatusLog->actStatus),
                &(jobStatusLog->actReasons),
                &(jobStatusLog->actSubReasons),
                &(jobStatusLog->idx),
                &ccount);
    if (cc != 12)
        return (LSBE_EVENT_FORMAT);
    jobStatusLog->actPeriod = tmpActPeriod;

    line += ccount + 1;

    return (LSBE_NO_ERROR);
}


static int
readMig(char *line, struct migLog *migLog)
{
    int cc, ccount, i;
    char hName[MAXHOSTNAMELEN];

    cc = sscanf(line, "%d%d%n",
                &(migLog->jobId),
                &(migLog->numAskedHosts),
                &ccount );
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    if (migLog->numAskedHosts) {
        migLog->askedHosts = (char **)
            calloc(migLog->numAskedHosts, sizeof(char *));
        if (migLog->askedHosts == NULL) {
            migLog->numAskedHosts = 0;
            return (LSBE_NO_MEM);
        }

        for (i = 0; i < migLog->numAskedHosts; i++) {
            if ((ccount = stripQStr(line, hName)) < 0) {
                migLog->numAskedHosts = i;
                return (LSBE_EVENT_FORMAT);
            }
            if ((migLog->askedHosts[i] = putstr_(hName)) == NULL) {
                migLog->numAskedHosts = i;
                return (LSBE_NO_MEM);
            }
            line += ccount + 1;
        }
    }
    cc = sscanf(line, "%d%n", &(migLog->userId), &ccount );
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(migLog->idx),  &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    copyQStr(line, MAX_LSB_NAME_LEN, 1, migLog->userName);
    return (LSBE_NO_ERROR);

}

static int
readJobSigAct(char *line, struct sigactLog *sigactLog)
{
    int cc, ccount;
    int tmpPeriod;
    char sigSymbol[MAXLINELEN];

    cc = sscanf(line, "%d%d%d%d%d%d%d%n",
                &(sigactLog->jobId),
                &(tmpPeriod),
                &(sigactLog->pid),
                &(sigactLog->jStatus),
                &(sigactLog->reasons),
                &(sigactLog->flags),
                &(sigactLog->actStatus),
                &ccount);
    if (cc != 7)
        return (LSBE_EVENT_FORMAT);
    sigactLog->period = tmpPeriod;

    line += ccount + 1;
    if ((ccount = stripQStr(line, sigSymbol)) < 0)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    sigactLog->signalSymbol = putstr_(sigSymbol);
    if (!sigactLog->signalSymbol)
        return(LSBE_NO_MEM);

    cc = sscanf(line, "%d%n", &(sigactLog->idx),  &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    return (LSBE_NO_ERROR);

}

static int
readJobRequeue(char *line, struct jobRequeueLog *jobRequeueLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%n", &(jobRequeueLog->jobId),
                &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    cc = sscanf(line, "%d%n", &(jobRequeueLog->idx), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    return (LSBE_NO_ERROR);
}

static int
readJobClean(char *line, struct jobCleanLog *jobCleanLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%d%n", &(jobCleanLog->jobId),
                &(jobCleanLog->idx), &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    return (LSBE_NO_ERROR);
}


static int
readChkpnt(char *line, struct chkpntLog *chkpntLog)
{
    int cc, ccount;
    int tmpPeriod;

    cc = sscanf(line, "%d%d%d%d%d%n",
                &(chkpntLog->jobId),
                &(tmpPeriod),
                &(chkpntLog->pid),
                &(chkpntLog->ok),
                &(chkpntLog->flags),
                &ccount);
    if (cc != 5)
        return (LSBE_EVENT_FORMAT);
    chkpntLog->period = tmpPeriod;

    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(chkpntLog->idx), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    return (LSBE_NO_ERROR);
}

static int
readJobSwitch(char *line, struct jobSwitchLog *jobSwitchLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%d%n", &(jobSwitchLog->userId),
                &(jobSwitchLog->jobId), &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobSwitchLog->queue);

    cc = sscanf(line, "%d%n", &(jobSwitchLog->idx),
                &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobSwitchLog->userName);

    return (LSBE_NO_ERROR);

}

static int
readJobMove(char *line, struct jobMoveLog *jobMoveLog)
{
    int cc;
    int ccount;

    cc = sscanf(line, "%d%d%d%d%n",
                &(jobMoveLog->userId),
                &(jobMoveLog->jobId),
                &(jobMoveLog->position),
                &(jobMoveLog->base),
                &ccount);
    if (cc != 4)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(jobMoveLog->idx), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobMoveLog->userName);

    return (LSBE_NO_ERROR);
}

static int
readQueueCtrl(char *line, struct queueCtrlLog *queueCtrlLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%n", &(queueCtrlLog->opCode), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    copyQStr(line, MAX_LSB_NAME_LEN, 1, queueCtrlLog->queue);

    cc = sscanf(line, "%d%n", &(queueCtrlLog->userId), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    copyQStr(line, MAX_LSB_NAME_LEN, 1, queueCtrlLog->userName);

    if (version >= _VOLCLAVA_VERSION2_1_)
        copyQStr(line, MAXLINELEN, 0, queueCtrlLog->msg);

    return (LSBE_NO_ERROR);
}

static int
readHostCtrl(char *line, struct hostCtrlLog *hostCtrlLog)
{
    int cc, ccount;

    cc = sscanf(line, "%d%n", &(hostCtrlLog->opCode), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;

    copyQStr(line, MAXHOSTNAMELEN, 0, hostCtrlLog->host);

    cc = sscanf(line, "%d%n", &(hostCtrlLog->userId), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);

    copyQStr(line, MAX_LSB_NAME_LEN, 1, hostCtrlLog->userName);

    if (version >= _VOLCLAVA_VERSION2_1_)
        copyQStr(line, MAXLINELEN, 0, hostCtrlLog->msg);    

    return (LSBE_NO_ERROR);

}

static int
readMbdDie (char *line, struct mbdDieLog *mbdDieLog)
{
    int cc;

    copyQStr(line, MAXHOSTNAMELEN, 0,  mbdDieLog->master);
    cc = sscanf(line, "%d%d", &(mbdDieLog->numRemoveJobs),
                &(mbdDieLog->exitCode));
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);

    if (version >= _VOLCLAVA_VERSION2_1_)
        copyQStr(line, MAXLINELEN, 0,  mbdDieLog->msg);

    return (LSBE_NO_ERROR);
}

static int
readUnfulfill (char *line, struct unfulfillLog *unfulfillLog)
{
    int cc;
    int ccount;
    int tmpChkPeriod;

    cc = sscanf(line, "%d%d%d%d%d%d%d%n",
                &(unfulfillLog->jobId), &(unfulfillLog->notSwitched),
                &(unfulfillLog->sig), &(unfulfillLog->sig1),
                &(unfulfillLog->sig1Flags),
                &(tmpChkPeriod),
                &(unfulfillLog->notModified),
                &ccount);
    if (cc != 7)
        return (LSBE_EVENT_FORMAT);
    unfulfillLog->chkPeriod = tmpChkPeriod;

    line += ccount+1;

    cc = sscanf(line, "%d%n", &(unfulfillLog->idx), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount+1;

    return (LSBE_NO_ERROR);
}

static int
readLoadIndex(char *line, struct loadIndexLog *loadIndexLog)
{
    int cc, ccount, i;

    if ((cc = sscanf(line, "%d%n", &loadIndexLog->nIdx, &ccount)) != 1)
        return (LSBE_EVENT_FORMAT);

    if ((loadIndexLog->name = (char **) calloc(loadIndexLog->nIdx,
                                               sizeof(char *))) == NULL) {
        loadIndexLog->nIdx = 0;
        return (LSBE_NO_MEM);
    }

    line += ccount + 1;
    for (i = 0; i < loadIndexLog->nIdx; i++)
        saveQStr(line, loadIndexLog->name[i]);

    return (LSBE_NO_ERROR);
}

static int
readJobFinish(char *line, struct jobFinishLog *jobFinishLog, time_t eventTime)
{
    int  i, cc, ccount;
    int  tmpSubmit, tmpBegin, tmpTerm, tmpStart;
    char hName[MAXLINELEN];

    cc = sscanf(line, "%d%d%d%d%d%d%d%d%n",
                &(jobFinishLog->jobId),
                &(jobFinishLog->userId),
                &(jobFinishLog->options),
                &(jobFinishLog->numProcessors),
                &(tmpSubmit),
                &(tmpBegin),
                &(tmpTerm),
                &(tmpStart),
                &ccount);
    if (cc != 8)
        return (LSBE_EVENT_FORMAT);
    jobFinishLog->submitTime = tmpSubmit;
    jobFinishLog->beginTime  = tmpBegin;
    jobFinishLog->termTime   = tmpTerm;
    jobFinishLog->startTime  = tmpStart;

    line += ccount + 1;

    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobFinishLog->userName);
    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobFinishLog->queue);
    saveQStr(line, jobFinishLog->resReq);
    saveQStr(line, jobFinishLog->dependCond);
    saveQStr(line, jobFinishLog->preExecCmd);
    if (jobFinishLog->options & SUB_POST_EXEC) {
        saveQStr(line, jobFinishLog->postExecCmd);
    }
    copyQStr(line, MAXHOSTNAMELEN, 1, jobFinishLog->fromHost);
    copyQStr(line, MAXFILENAMELEN, 0, jobFinishLog->cwd);
    copyQStr(line, MAXFILENAMELEN, 0, jobFinishLog->inFile);
    copyQStr(line, MAXFILENAMELEN, 0, jobFinishLog->outFile);
    copyQStr(line, MAXFILENAMELEN, 0, jobFinishLog->errFile);
    copyQStr(line, MAXFILENAMELEN, 1, jobFinishLog->jobFile);

    cc = sscanf(line, "%d%n", &jobFinishLog->numAskedHosts, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobFinishLog->numAskedHosts > 0) {
        jobFinishLog->askedHosts = (char **)
            calloc(jobFinishLog->numAskedHosts, sizeof (char *));
        if (jobFinishLog->askedHosts == NULL) {
            jobFinishLog->numAskedHosts = 0;
            return (LSBE_NO_MEM);
        }
    }

    for (i=0; i<jobFinishLog->numAskedHosts; i++) {
        if ((ccount = stripQStr(line, hName)) < 0) {
            jobFinishLog->numAskedHosts = i;
            return (LSBE_EVENT_FORMAT);
        }
        jobFinishLog->askedHosts[i] = putstr_(hName);
        if (jobFinishLog->askedHosts[i] == NULL) {
            jobFinishLog->numAskedHosts = i;
            return (LSBE_NO_MEM);
        }
        line += ccount + 1;
    }

    cc = sscanf(line, "%d%n", &jobFinishLog->numExHosts, &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if (jobFinishLog->numExHosts > 0) {
        jobFinishLog->execHosts = (char **)
            calloc(jobFinishLog->numExHosts, sizeof (char *));
        if (jobFinishLog->execHosts == NULL) {
            jobFinishLog->numExHosts = 0;
            return (LSBE_NO_MEM);
        }
    }

    for (i=0; i<jobFinishLog->numExHosts; i++) {
        if ((ccount = stripQStr(line, hName)) < 0) {
            jobFinishLog->numExHosts = i;
            return (LSBE_EVENT_FORMAT);
        }
        jobFinishLog->execHosts[i] = putstr_(hName);
        if (jobFinishLog->execHosts[i] == NULL) {
            jobFinishLog->numExHosts = i;
            return (LSBE_NO_MEM);
        }
        line += ccount + 1;
    }

    cc = sscanf(line, "%d%f%n", &jobFinishLog->jStatus,
                &jobFinishLog->hostFactor, &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    jobFinishLog->endTime = eventTime;

    copyQStr(line, MAX_CMD_DESC_LEN, 0, jobFinishLog->jobName);
    copyQStr(line, MAX_CMD_DESC_LEN, 0, jobFinishLog->command);

    if ((cc =  str2lsfRu(line, &jobFinishLog->lsfRusage, &ccount)) != 19)
        return (LSBE_EVENT_FORMAT);

    jobFinishLog->cpuTime = (float)(jobFinishLog->lsfRusage.ru_utime +
                                    jobFinishLog->lsfRusage.ru_stime);
    if ( jobFinishLog->cpuTime < 0 )
        jobFinishLog->cpuTime = 0.0;

    line += ccount + 1;
    saveQStr(line, jobFinishLog->mailUser);
    saveQStr(line, jobFinishLog->projectName);

    if ((cc = sscanf(line, "%d%n", &jobFinishLog->exitStatus,
                     &ccount)) != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    if ((cc = sscanf(line, "%d%n", &jobFinishLog->maxNumProcessors,
                     &ccount)) != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    saveQStr(line, jobFinishLog->loginShell);

    cc = sscanf(line, "%d%n", &(jobFinishLog->idx),
                &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    cc = sscanf(line, "%d%d%n", &(jobFinishLog->maxRMem),
                &(jobFinishLog->maxRSwap), &ccount);
    if (cc != 2)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    copyQStr(line, MAXFILENAMELEN, 0, jobFinishLog->inFileSpool);
    copyQStr(line, MAXFILENAMELEN, 0, jobFinishLog->commandSpool);

    return (LSBE_NO_ERROR);

}

int
lsb_puteventrec(FILE *log_fp, struct eventRec *logPtr)
{
    char *etype;

    switch (logPtr->type) {
        case EVENT_JOB_NEW:
            etype = "JOB_NEW";
            break;
        case EVENT_JOB_START:
            etype = "JOB_START";
            break;
        case EVENT_JOB_START_ACCEPT:
            etype = "JOB_START_ACCEPT";
            break;
        case EVENT_JOB_STATUS:
            etype = "JOB_STATUS";
            break;
        case EVENT_JOB_SWITCH:
            etype = "JOB_SWITCH";
            break;
        case EVENT_JOB_MOVE:
            etype = "JOB_MOVE";
            break;
        case EVENT_QUEUE_CTRL:
            etype = "QUEUE_CTRL";
            break;
        case EVENT_HOST_CTRL:
            etype = "HOST_CTRL";
            break;
        case EVENT_MBD_START:
            etype = "MBD_START";
            break;
        case EVENT_MBD_DIE:
            etype = "MBD_DIE";
            break;
        case EVENT_MBD_UNFULFILL:
            etype = "UNFULFILL";
            break;
        case EVENT_JOB_FINISH:
            etype = "JOB_FINISH";
            break;
        case EVENT_LOAD_INDEX:
            etype = "LOAD_INDEX";
            break;
        case EVENT_CHKPNT:
            etype = "CHKPNT";
            break;
        case EVENT_MIG:
            etype = "MIG";
            break;
        case EVENT_PRE_EXEC_START:
            etype = "PRE_EXEC_START";
            break;
        case EVENT_JOB_MODIFY:
            etype = "JOB_MODIFY";
            break;
        case EVENT_JOB_MODIFY2:
            etype = "JOB_MODIFY2";
            break;
        case EVENT_JOB_ATTR_SET:
            etype = "JOB_ATTR_SET";
            break;
        case EVENT_JOB_SIGNAL:
            etype = "JOB_SIGNAL";
            break;
        case EVENT_JOB_EXECUTE:
            etype = "JOB_EXECUTE";
            break;
        case EVENT_JOB_MSG:
            etype = "JOB_MSG";
            break;
        case EVENT_JOB_MSG_ACK:
            etype = "JOB_MSG_ACK";
            break;
        case EVENT_JOB_REQUEUE:
            etype = "JOB_REQUEUE";
            break;
        case EVENT_JOB_CLEAN:
            etype = "JOB_CLEAN";
            break;
        case EVENT_JOB_SIGACT:
            etype = "JOB_SIGACT";
            break;
        case EVENT_SBD_JOB_STATUS:
            etype = "SBD_JOB_STATUS";
            break;
        case EVENT_JOB_FORCE:
            etype = "JOB_FORCE";
            break;
        case EVENT_LOG_SWITCH:
            etype = "LOG_SWITCH";
            break;
        default:
            lsberrno = LSBE_UNKNOWN_EVENT;
            return -1;
    }

    if (fprintf(log_fp, "\"%s\" \"%s\" %d", etype, logPtr->version,
                (int) logPtr->eventTime) < 0) {
        lsberrno = LSBE_SYS_CALL;
        return -1;
    }

    switch (logPtr->type) {
        case EVENT_JOB_NEW:
        case EVENT_JOB_MODIFY:
            lsberrno = writeJobNew(log_fp, &(logPtr->eventLog.jobNewLog));
            break;
        case EVENT_JOB_MODIFY2:
            lsberrno = writeJobMod(log_fp, &(logPtr->eventLog.jobModLog));
            break;
        case EVENT_PRE_EXEC_START:
        case EVENT_JOB_START:
            lsberrno = writeJobStart(log_fp, &(logPtr->eventLog.jobStartLog));
            break;
        case EVENT_JOB_START_ACCEPT:
            lsberrno = writeJobStartAccept(log_fp,
                                           &(logPtr->eventLog.jobStartAcceptLog));
            break;
        case EVENT_JOB_STATUS:
            lsberrno = writeJobStatus(log_fp, &(logPtr->eventLog.jobStatusLog));
            break;
        case EVENT_SBD_JOB_STATUS:
            lsberrno = writeSbdJobStatus(log_fp, &(logPtr->eventLog.sbdJobStatusLog));
            break;
        case EVENT_JOB_SWITCH:
            lsberrno = writeJobSwitch(log_fp, &(logPtr->eventLog.jobSwitchLog));
            break;
        case EVENT_JOB_MOVE:
            lsberrno = writeJobMove(log_fp, &(logPtr->eventLog.jobMoveLog));
            break;
        case EVENT_QUEUE_CTRL:
            lsberrno = writeQueueCtrl(log_fp, &(logPtr->eventLog.queueCtrlLog));
            break;
        case EVENT_HOST_CTRL:
            lsberrno = writeHostCtrl(log_fp, &(logPtr->eventLog.hostCtrlLog));
            break;
        case EVENT_MBD_START:
            lsberrno = writeMbdStart(log_fp, &(logPtr->eventLog.mbdStartLog));
            break;
        case EVENT_MBD_DIE:
            lsberrno = writeMbdDie (log_fp, &(logPtr->eventLog.mbdDieLog));
            break;
        case EVENT_MBD_UNFULFILL:
            lsberrno = writeUnfulfill (log_fp, &(logPtr->eventLog.unfulfillLog));
            break;
        case EVENT_LOAD_INDEX:
            lsberrno = writeLoadIndex (log_fp, &(logPtr->eventLog.loadIndexLog));
            break;
        case EVENT_JOB_FINISH:
            lsberrno = writeJobFinish(log_fp, &(logPtr->eventLog.jobFinishLog));
            break;
        case EVENT_CHKPNT:
            lsberrno = writeChkpnt(log_fp, &(logPtr->eventLog.chkpntLog));
            break;
        case EVENT_MIG:
            lsberrno = writeMig(log_fp, &(logPtr->eventLog.migLog));
            break;
        case EVENT_JOB_ATTR_SET:
            lsberrno = writeJobAttrSet(log_fp, &(logPtr->eventLog.jobAttrSetLog));
            break;
        case EVENT_JOB_SIGNAL:
            lsberrno = writeJobSignal(log_fp, &(logPtr->eventLog.signalLog));
            break;
        case EVENT_JOB_EXECUTE:
            lsberrno = writeJobExecute(log_fp, &(logPtr->eventLog.jobExecuteLog));
            break;
        case EVENT_JOB_MSG:
            lsberrno = writeJobMsg(log_fp, &(logPtr->eventLog.jobMsgLog));
            break;
        case EVENT_JOB_MSG_ACK:
            lsberrno = writeJobMsgAck(log_fp, &(logPtr->eventLog.jobMsgLog));
            break;
        case EVENT_JOB_SIGACT:
            lsberrno = writeJobSigAct(log_fp, &(logPtr->eventLog.sigactLog));
            break;
        case EVENT_JOB_REQUEUE:
            lsberrno = writeJobRqueue(log_fp, &(logPtr->eventLog.jobRequeueLog));
            break;
        case EVENT_JOB_CLEAN:
            lsberrno = writeJobClean(log_fp, &(logPtr->eventLog.jobCleanLog));
            break;
        case EVENT_JOB_FORCE:
            lsberrno = writeJobForce(log_fp, &(logPtr->eventLog.jobForceRequestLog));
            break;
        case EVENT_LOG_SWITCH:
            lsberrno = writeLogSwitch(log_fp, &(logPtr->eventLog.logSwitchLog));
            break;
    }


    if (lsberrno == LSBE_NO_ERROR) {
        return 0;
    }

    return (-1);

}

static int
writeJobNew(FILE *log_fp, struct jobNewLog *jobNewLog)
{
    int i;


    if (!(jobNewLog->options & SUB_RLIMIT_UNIT_IS_KB)) {
        convertRLimit(jobNewLog->rLimits, 0);
    }

    if (fprintf(log_fp, " %d %d %d %d %d %d %d %d %d %d",
                jobNewLog->jobId,
                jobNewLog->userId,
                jobNewLog->options,
                jobNewLog->numProcessors,
                (int) jobNewLog->submitTime,
                (int) jobNewLog->beginTime,
                (int) jobNewLog->termTime,
                jobNewLog->sigValue,
                (int) jobNewLog->chkpntPeriod,
                jobNewLog->restartPid) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobNewLog->userName) < 0)
        return (LSBE_SYS_CALL);
    for(i = 0; i < LSF_RLIM_NLIMITS; i++)
        if (fprintf(log_fp, " %d", jobNewLog->rLimits[i]) <0)
            return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->hostSpec) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %6.2f", jobNewLog->hostFactor) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", jobNewLog->umask) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobNewLog->queue) < 0)
        return (LSBE_SYS_CALL);
    subNewLine_(jobNewLog->resReq);
    if (addQStr (log_fp, jobNewLog->resReq) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->fromHost) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->cwd) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->chkpntDir) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->inFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->outFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->errFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->subHomeDir) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobNewLog->jobFile) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobNewLog->numAskedHosts) < 0)
        return (LSBE_SYS_CALL);
    if (jobNewLog->numAskedHosts)
        for (i = 0; i < jobNewLog->numAskedHosts; i++) {
            if (addQStr (log_fp, jobNewLog->askedHosts[i]) < 0)
                return (LSBE_SYS_CALL);
        }

    subNewLine_(jobNewLog->dependCond);
    if (addQStr (log_fp, jobNewLog->dependCond) < 0)
        return (LSBE_SYS_CALL);
    subNewLine_(jobNewLog->preExecCmd);
    if (addQStr(log_fp, jobNewLog->preExecCmd) < 0)
        return (LSBE_SYS_CALL);
    if (jobNewLog->options & SUB_POST_EXEC) {
        subNewLine_(jobNewLog->postExecCmd);
        if (addQStr(log_fp, jobNewLog->postExecCmd) < 0)
            return (LSBE_SYS_CALL);
    }
    if (addQStr(log_fp, jobNewLog->jobName) < 0)
        return (LSBE_SYS_CALL);
    subNewLine_(jobNewLog->command);
    if (addQStr(log_fp,jobNewLog->command) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobNewLog->nxf) < 0)
        return (LSBE_SYS_CALL);
    for (i = 0; i < jobNewLog->nxf; i++) {
        if (fprintf(log_fp, " \"%s\" \"%s\" %d", jobNewLog->xf[i].subFn,
                    jobNewLog->xf[i].execFn, jobNewLog->xf[i].options) < 0)
            return (LSBE_SYS_CALL);
    }
    subNewLine_(jobNewLog->mailUser);
    if (addQStr (log_fp, jobNewLog->mailUser) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobNewLog->projectName) < 0)
        return (LSBE_SYS_CALL);

    if (jobNewLog->options & SUB_INTERACTIVE) {
        if (fprintf(log_fp, " %d", jobNewLog->niosPort) < 0)
            return (LSBE_SYS_CALL);
    }
    if (fprintf(log_fp, " %d", jobNewLog->maxNumProcessors) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobNewLog->schedHostType) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobNewLog->loginShell) < 0)
        return (LSBE_SYS_CALL);


    if (fprintf(log_fp, " %d" ,
                jobNewLog->options2) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobNewLog->idx) < 0 )
        return (LSBE_SYS_CALL);

    if (jobNewLog->options2 & SUB2_BSUB_BLOCK) {
        if (fprintf(log_fp, " %d", jobNewLog->niosPort) < 0)
            return (LSBE_SYS_CALL);
    }

    if (addQStr (log_fp, jobNewLog->inFileSpool) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobNewLog->commandSpool) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobNewLog->jobSpoolDir) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobNewLog->userPriority) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);

}
static int
writeJobMod(FILE *log_fp, struct jobModLog *jobModLog)
{
    int i;

    if (addQStr (log_fp, jobModLog->jobIdStr) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d %d %d", jobModLog->options,
                jobModLog->options2, jobModLog->delOptions) < 0)
        return (LSBE_SYS_CALL);

    if ((fprintf(log_fp, " %d", jobModLog->userId) < 0) ||
        (addQStr (log_fp, jobModLog->userName) < 0))
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d %d %d %d %d %d %d",
                jobModLog->submitTime, jobModLog->umask,
                jobModLog->numProcessors, jobModLog->beginTime,
                jobModLog->termTime, jobModLog->sigValue,
                jobModLog->restartPid ) < 0)
        return (LSBE_SYS_CALL);

    if ((jobModLog->options & SUB_JOB_NAME) &&
        (addQStr (log_fp, jobModLog->jobName) < 0))
        return (LSBE_SYS_CALL);
    if ((jobModLog->options & SUB_QUEUE) &&
        (addQStr (log_fp, jobModLog->queue) < 0))
        return (LSBE_SYS_CALL);

    if (jobModLog->options & SUB_HOST) {
        if (fprintf(log_fp, " %d", jobModLog->numAskedHosts) < 0)
            return (LSBE_SYS_CALL);
        if (jobModLog->numAskedHosts)
            for (i = 0; i < jobModLog->numAskedHosts; i++) {
                if (addQStr (log_fp, jobModLog->askedHosts[i]) < 0)
                    return (LSBE_SYS_CALL);
            }
    }

    if ((jobModLog->options & SUB_RES_REQ) &&
        (addQStr (log_fp, jobModLog->resReq) < 0))
        return (LSBE_SYS_CALL);
    for(i = 0; i < LSF_RLIM_NLIMITS; i++)
        if (fprintf(log_fp, " %d", jobModLog->rLimits[i]) <0)
            return (LSBE_SYS_CALL);
    if ((jobModLog->options & SUB_HOST_SPEC) &&
        (addQStr (log_fp, jobModLog->hostSpec) < 0))
        return (LSBE_SYS_CALL);

    subNewLine_(jobModLog->dependCond);
    if ((jobModLog->options & SUB_DEPEND_COND ) &&
        (addQStr (log_fp, jobModLog->dependCond) < 0))
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobModLog->subHomeDir) < 0)
        return (LSBE_SYS_CALL);
    if (((jobModLog->options & SUB_IN_FILE)
         || (jobModLog->options2 & SUB2_IN_FILE_SPOOL)) &&
        (addQStr (log_fp, jobModLog->inFile) < 0))
        return (LSBE_SYS_CALL);
    if ((jobModLog->options & SUB_OUT_FILE) &&
        (addQStr (log_fp, jobModLog->outFile) < 0))
        return (LSBE_SYS_CALL);
    if ((jobModLog->options & SUB_ERR_FILE) &&
        (addQStr (log_fp, jobModLog->errFile) < 0))
        return (LSBE_SYS_CALL);
    if ((jobModLog->options2 & SUB2_MODIFY_CMD) &&
        (addQStr (log_fp, jobModLog->command) < 0))
        return (LSBE_SYS_CALL);

    if ((jobModLog->options & SUB_CHKPNT_PERIOD) &&
        (fprintf (log_fp, " %d", jobModLog->chkpntPeriod) < 0))
        return (LSBE_SYS_CALL);
    if ((jobModLog->options & SUB_CHKPNT_DIR) &&
        (addQStr (log_fp, jobModLog->chkpntDir) < 0))
        return (LSBE_SYS_CALL);
    if (jobModLog->options & SUB_OTHER_FILES) {
        if (fprintf(log_fp, " %d", jobModLog->nxf) < 0)
            return (LSBE_SYS_CALL);
        for (i = 0; i < jobModLog->nxf; i++) {
            if (fprintf(log_fp, " \"%s\" \"%s\" %d", jobModLog->xf[i].subFn,
                        jobModLog->xf[i].execFn, jobModLog->xf[i].options) < 0)
                return (LSBE_SYS_CALL);
        }
    }
    if (addQStr (log_fp, jobModLog->jobFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobModLog->fromHost) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobModLog->cwd) < 0)
        return (LSBE_SYS_CALL);

    subNewLine_(jobModLog->preExecCmd);
    if ((jobModLog->options & SUB_PRE_EXEC) &&
        (addQStr (log_fp, jobModLog->preExecCmd) < 0))
        return (LSBE_SYS_CALL);

    if (jobModLog->options & SUB_POST_EXEC) {
        subNewLine_(jobModLog->postExecCmd);
        if ((jobModLog->options & SUB_POST_EXEC) &&
            (addQStr (log_fp, jobModLog->postExecCmd) < 0))
            return (LSBE_SYS_CALL);
    }

    if ((jobModLog->options & SUB_MAIL_USER) &&
        (addQStr (log_fp, jobModLog->mailUser) < 0))
        return (LSBE_SYS_CALL);
    if ((jobModLog->options & SUB_PROJECT_NAME) &&
        (addQStr (log_fp, jobModLog->projectName) < 0))
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d %d",
                jobModLog->niosPort, jobModLog->maxNumProcessors) < 0 )
        return (LSBE_SYS_CALL);

    if ((jobModLog->options & SUB_LOGIN_SHELL) &&
        (addQStr (log_fp, jobModLog->loginShell) < 0))
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobModLog->schedHostType) < 0)
        return (LSBE_SYS_CALL);


    if (fprintf(log_fp, " %d", jobModLog->delOptions2) < 0) {
        return (LSBE_SYS_CALL);
    }

    if ((jobModLog->options2 & SUB2_IN_FILE_SPOOL) &&
        (addQStr (log_fp, jobModLog->inFileSpool) < 0))
        return (LSBE_SYS_CALL);

    if ((jobModLog->options2 & SUB2_JOB_CMD_SPOOL) &&
        (addQStr (log_fp, jobModLog->commandSpool) < 0))
        return (LSBE_SYS_CALL);

    if ((jobModLog->options2 & SUB2_JOB_PRIORITY)
        && fprintf(log_fp, " %d", jobModLog->userPriority) < 0) {
        return (LSBE_SYS_CALL);
    }

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);
    return(LSBE_NO_ERROR);
}



static int
writeJobStart(FILE *log_fp, struct jobStartLog *jobStartLog)
{
    int i;

    if (fprintf(log_fp, " %d %d %d %d %3.1f %d",
                jobStartLog->jobId,
                jobStartLog->jStatus,
                jobStartLog->jobPid,
                jobStartLog->jobPGid,
                jobStartLog->hostFactor,
                jobStartLog->numExHosts) < 0)
        return (LSBE_SYS_CALL);

    if (jobStartLog->numExHosts)
        for (i = 0; i < jobStartLog->numExHosts; i++) {
            if (addQStr (log_fp, jobStartLog->execHosts[i]) < 0)
                return (LSBE_SYS_CALL);
        }
    if (addQStr(log_fp, jobStartLog->queuePreCmd) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobStartLog->queuePostCmd) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobStartLog->jFlags) < 0) {
        return (LSBE_SYS_CALL);
    }


    if (fprintf(log_fp, " %d", jobStartLog->idx) < 0 )
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobStartAccept(FILE *log_fp, struct jobStartAcceptLog *jobStartAcceptLog)
{


    if (fprintf(log_fp, " %d %d %d", jobStartAcceptLog->jobId,
                jobStartAcceptLog->jobPid, jobStartAcceptLog->jobPGid) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobStartAcceptLog->idx) < 0 )
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobExecute(FILE *log_fp, struct jobExecuteLog *jobExecuteLog)
{


    if (fprintf(log_fp, " %d %d %d",
                jobExecuteLog->jobId,
                jobExecuteLog->execUid,
                jobExecuteLog->jobPGid) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobExecuteLog->execCwd) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobExecuteLog->execHome) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobExecuteLog->execUsername) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobExecuteLog->jobPid) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobExecuteLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}



static int
writeJobStatus(FILE *log_fp, struct jobStatusLog *jobStatusLog)
{
    if (fprintf(log_fp, " %d %d %d %d %4.4f %d",
                jobStatusLog->jobId, jobStatusLog->jStatus,
                jobStatusLog->reason, jobStatusLog->subreasons,
                jobStatusLog->cpuTime, (int) jobStatusLog->endTime) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobStatusLog->ru) < 0)
        return (LSBE_SYS_CALL);
    if (jobStatusLog->ru)
        if (lsfRu2Str(log_fp, &jobStatusLog->lsfRusage) < 0)
            return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", jobStatusLog->exitStatus) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobStatusLog->idx) < 0 )
        return (LSBE_SYS_CALL);


    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}


static int
writeSbdJobStatus(FILE *log_fp, struct sbdJobStatusLog *jobStatusLog)
{
    if (fprintf(log_fp, " %d %d %d %d %d %d %d %d %d %d %d",
                jobStatusLog->jobId, jobStatusLog->jStatus,
                jobStatusLog->reasons, jobStatusLog->subreasons,
                jobStatusLog->actPid, jobStatusLog->actValue,
                (int) jobStatusLog->actPeriod, jobStatusLog->actFlags,
                jobStatusLog->actStatus, jobStatusLog->actReasons,
                jobStatusLog->actSubReasons) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobStatusLog->idx) < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobSwitch(FILE *log_fp, struct jobSwitchLog *jobSwitchLog)
{
    if (fprintf(log_fp, " %d %d", jobSwitchLog->userId,
                jobSwitchLog->jobId) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, jobSwitchLog->queue) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobSwitchLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobSwitchLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeMig(FILE *log_fp, struct migLog *migLog)
{
    int i;

    if (fprintf(log_fp, " %d %d", migLog->jobId,
                migLog->numAskedHosts) < 0)
        return (LSBE_SYS_CALL);

    if (migLog->numAskedHosts)
        for (i = 0; i < migLog->numAskedHosts; i++) {
            if (addQStr (log_fp, migLog->askedHosts[i]) < 0)
                return (LSBE_SYS_CALL);
        }
    if (fprintf(log_fp, " %d", migLog->userId) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", migLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, migLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobSigAct (FILE *log_fp, struct sigactLog *sigactLog)
{
    if (fprintf(log_fp, " %d %d %d %d %d %d %d",
                sigactLog->jobId,
                (int) sigactLog->period, sigactLog->pid,
                sigactLog->jStatus, sigactLog->reasons, sigactLog->flags,
                sigactLog->actStatus) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, sigactLog->signalSymbol) < 0)
        return(LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", sigactLog->idx) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobRqueue (FILE *log_fp, struct jobRequeueLog *jobRequeueLog)
{
    if (fprintf(log_fp, " %d %d\n", jobRequeueLog->jobId, jobRequeueLog->idx) < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobClean (FILE *log_fp, struct jobCleanLog *jobCleanLog)
{
    if (fprintf(log_fp, " %d %d\n", jobCleanLog->jobId, jobCleanLog->idx) < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeChkpnt(FILE *log_fp, struct chkpntLog *chkpntLog)
{
    if (fprintf(log_fp, " %d %d %d %d %d %d\n",
                chkpntLog->jobId,
                (int) chkpntLog->period, chkpntLog->pid,
                chkpntLog->ok, chkpntLog->flags, chkpntLog->idx) < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobMove(FILE *log_fp, struct jobMoveLog *jobMoveLog)
{
    if (fprintf(log_fp, " %d %d %d %d %d",
                jobMoveLog->userId, jobMoveLog->jobId,
                jobMoveLog->position, jobMoveLog->base, jobMoveLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobMoveLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeQueueCtrl(FILE *log_fp, struct queueCtrlLog *queueCtrlLog)
{
    if (fprintf(log_fp, " %d", queueCtrlLog->opCode) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, queueCtrlLog->queue) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", queueCtrlLog->userId) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, queueCtrlLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, queueCtrlLog->msg) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeHostCtrl(FILE *log_fp, struct hostCtrlLog *hostCtrlLog)
{
    if (fprintf(log_fp, " %d", hostCtrlLog->opCode) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr (log_fp, hostCtrlLog->host) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", hostCtrlLog->userId) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, hostCtrlLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, hostCtrlLog->msg) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeMbdDie (FILE *log_fp, struct mbdDieLog *mbdDieLog)
{
    if (addQStr(log_fp, mbdDieLog->master) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d %d", mbdDieLog->numRemoveJobs,
                mbdDieLog->exitCode) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, mbdDieLog->msg))
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeUnfulfill (FILE *log_fp, struct unfulfillLog *unfulfillLog)
{
    if (fprintf(log_fp, " %d %d %d %d %d %d %d %d\n",
                unfulfillLog->jobId, unfulfillLog->notSwitched,
                unfulfillLog->sig, unfulfillLog->sig1,
                unfulfillLog->sig1Flags,
                (int) unfulfillLog->chkPeriod,
                unfulfillLog->notModified,
                unfulfillLog->idx) < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeLoadIndex (FILE *log_fp, struct loadIndexLog *loadIndexLog)
{
    int i;

    if (fprintf(log_fp, " %d", loadIndexLog->nIdx) < 0)
        return (LSBE_SYS_CALL);

    if (loadIndexLog->nIdx)
        for (i = 0; i < loadIndexLog->nIdx; i++) {
            if (addQStr (log_fp, loadIndexLog->name[i]) < 0)
                return (LSBE_SYS_CALL);
        }

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobFinish(FILE *log_fp, struct jobFinishLog *jobFinishLog)
{
    int i;

    if (fprintf(log_fp, " %d %d %d %d %d %d %d %d",
                jobFinishLog->jobId,
                jobFinishLog->userId,
                jobFinishLog->options,
                jobFinishLog->numProcessors,
                (int) jobFinishLog->submitTime,
                (int) jobFinishLog->beginTime,
                (int) jobFinishLog->termTime,
                (int) jobFinishLog->startTime) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobFinishLog->userName) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->queue) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->resReq) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->dependCond) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->preExecCmd) < 0)
        return (LSBE_SYS_CALL);
    if (jobFinishLog->options & SUB_POST_EXEC) {
        if (addQStr(log_fp, jobFinishLog->postExecCmd) < 0)
            return (LSBE_SYS_CALL);
    }
    if (addQStr(log_fp, jobFinishLog->fromHost) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->cwd) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->inFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->outFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->errFile) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->jobFile) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobFinishLog->numAskedHosts) < 0)
        return (LSBE_SYS_CALL);
    if (jobFinishLog->numAskedHosts)
        for (i = 0; i < jobFinishLog->numAskedHosts; i++) {
            if (addQStr(log_fp, jobFinishLog->askedHosts[i]) < 0)
                return (LSBE_SYS_CALL);
        }

    if (fprintf(log_fp, " %d", jobFinishLog->numExHosts) < 0)
        return (LSBE_SYS_CALL);
    if (jobFinishLog->numExHosts)
        for (i = 0; i < jobFinishLog->numExHosts; i++) {
            if (addQStr(log_fp, jobFinishLog->execHosts[i]) < 0)
                return (LSBE_SYS_CALL);
        }

    if (fprintf(log_fp, " %d %3.1f",
                jobFinishLog->jStatus,
                jobFinishLog->hostFactor) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobFinishLog->jobName) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, jobFinishLog->command) < 0)
        return (LSBE_SYS_CALL);

    if (lsfRu2Str (log_fp, &jobFinishLog->lsfRusage) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobFinishLog->mailUser) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobFinishLog->projectName) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", jobFinishLog->exitStatus) < 0)
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d", jobFinishLog->maxNumProcessors) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr (log_fp, jobFinishLog->loginShell) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobFinishLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobFinishLog->maxRMem) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobFinishLog->maxRSwap) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobFinishLog->inFileSpool) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobFinishLog->commandSpool) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);

}

static int
writeMbdStart(FILE *log_fp, struct mbdStartLog *mbdStartLog)
{
    if ((addQStr(log_fp, mbdStartLog->master) < 0)
        || (addQStr(log_fp, mbdStartLog->cluster) < 0))
        return (LSBE_SYS_CALL);
    if (fprintf(log_fp, " %d %d\n", mbdStartLog->numHosts,
                mbdStartLog->numQueues) < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
readMbdStart(char *line, struct mbdStartLog *mbdStartLog)
{
    int cc;

    copyQStr(line, MAXHOSTNAMELEN, 0,  mbdStartLog->master);
    copyQStr(line, MAXLSFNAMELEN, 0,  mbdStartLog->cluster);
    cc = sscanf(line, "%d%d", &(mbdStartLog->numHosts),
                &(mbdStartLog->numQueues));
    if (cc != 2)
        return(LSBE_EVENT_FORMAT);

    return(LSBE_NO_ERROR);
}

static int
readLogSwitch(char *line, struct logSwitchLog *logSwitchLog)
{
    int cc,ccount;

    cc = sscanf(line, "%d %n",&logSwitchLog->lastJobId,
                &ccount);
    if (cc != 1)
        return(LSBE_EVENT_FORMAT);
    line += ccount + 1;
    return(LSBE_NO_ERROR);
}

static int
writeJobSignal(FILE *log_fp, struct signalLog *signalLog)
{

    if (fprintf(log_fp, " %d %d %d", signalLog->jobId,
                signalLog->userId,
                signalLog->runCount) < 0)
        return (LSBE_SYS_CALL);
    if (addQStr(log_fp, signalLog->signalSymbol) < 0)
        return(LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", signalLog->idx) < 0 )
        return(LSBE_SYS_CALL);

    if (addQStr (log_fp, signalLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobMsg(FILE *log_fp, struct jobMsgLog *jobMsgLog)
{
    if (fprintf(log_fp, " %d %d %d %d",
                jobMsgLog->usrId,
                jobMsgLog->jobId,
                jobMsgLog->msgId,
                jobMsgLog->type) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobMsgLog->src) < 0)
        return(LSBE_SYS_CALL);
    if (addQStr(log_fp, jobMsgLog->dest) < 0)
        return(LSBE_SYS_CALL);
    if (addQStr(log_fp, jobMsgLog->msg) < 0)
        return(LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobMsgLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}

static int
writeJobMsgAck(FILE *log_fp, struct jobMsgLog *jobMsgLog)
{
    if (fprintf(log_fp, " %d %d %d %d",
                jobMsgLog->usrId,
                jobMsgLog->jobId,
                jobMsgLog->msgId,
                jobMsgLog->type) < 0)
        return (LSBE_SYS_CALL);

    if (addQStr(log_fp, jobMsgLog->src) < 0)
        return(LSBE_SYS_CALL);
    if (addQStr(log_fp, jobMsgLog->dest) < 0)
        return(LSBE_SYS_CALL);
    if (addQStr(log_fp, jobMsgLog->msg) < 0)
        return(LSBE_SYS_CALL);

    if (fprintf(log_fp, " %d", jobMsgLog->idx) < 0)
        return (LSBE_SYS_CALL);

    if (fprintf(log_fp, "\n") < 0)
        return (LSBE_SYS_CALL);

    return (LSBE_NO_ERROR);
}


static int
readJobSignal (char *line, struct signalLog *signalLog)
{
    int cc, ccount;
    char sigSymbol[MAXLINELEN];

    cc = sscanf(line, "%d%d%d%n",
                &(signalLog->jobId),
                &(signalLog->userId),
                &(signalLog->runCount),
                &ccount );
    if (cc != 3)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    if ((ccount = stripQStr(line, sigSymbol)) < 0)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    signalLog->signalSymbol = putstr_(sigSymbol);
    if (!signalLog->signalSymbol)
        return(LSBE_NO_MEM);

    cc = sscanf(line, "%d%n",  &(signalLog->idx), &ccount );
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;

    copyQStr(line, MAX_LSB_NAME_LEN, 1, signalLog->userName);

    return (LSBE_NO_ERROR);

}

static int
readJobMsg (char *line, struct jobMsgLog *jobMsgLog)
{
    int cc, ccount;
    char strBuf[MAXLINELEN];

    cc = sscanf(line, "%d%d%d%d%n",
                &(jobMsgLog->usrId),
                &(jobMsgLog->jobId),
                &(jobMsgLog->msgId),
                &(jobMsgLog->type),
                &ccount);

    if (cc != 4)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    if ((ccount = stripQStr(line, strBuf)) < 0)
        return (LSBE_EVENT_FORMAT);

    jobMsgLog->src = putstr_(strBuf);
    if (!jobMsgLog->src)
        return(LSBE_NO_MEM);

    line += ccount + 1;

    if ((ccount = stripQStr(line, strBuf)) < 0)
        return (LSBE_EVENT_FORMAT);
    jobMsgLog->dest = putstr_(strBuf);
    if (!jobMsgLog->dest)
        return(LSBE_NO_MEM);

    line += ccount + 1;

    if ((ccount = stripQStr(line, strBuf)) < 0)
        return (LSBE_EVENT_FORMAT);
    jobMsgLog->msg = putstr_(strBuf);
    if (!jobMsgLog->msg)
        return(LSBE_NO_MEM);

    line += ccount + 1;
    cc = sscanf(line, "%d%n", &(jobMsgLog->idx), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    return (LSBE_NO_ERROR);

}

static int
readJobMsgAck (char *line, struct jobMsgAckLog *jobMsgAckLog)
{
    int cc, ccount;
    char strBuf[MAXLINELEN];

    cc = sscanf(line, "%d%d%d%d%n",
                &(jobMsgAckLog->usrId),
                &(jobMsgAckLog->jobId),
                &(jobMsgAckLog->msgId),
                &(jobMsgAckLog->type),
                &ccount);

    if (cc != 4)
        return (LSBE_EVENT_FORMAT);

    line += ccount + 1;
    if ((ccount = stripQStr(line, strBuf)) < 0)
        return (LSBE_EVENT_FORMAT);

    jobMsgAckLog->src = putstr_(strBuf);
    if (!jobMsgAckLog->src)
        return(LSBE_NO_MEM);

    line += ccount + 1;

    if ((ccount = stripQStr(line, strBuf)) < 0)
        return (LSBE_EVENT_FORMAT);
    jobMsgAckLog->dest = putstr_(strBuf);
    if (!jobMsgAckLog->dest)
        return(LSBE_NO_MEM);

    line += ccount + 1;

    if ((ccount = stripQStr(line, strBuf)) < 0)
        return (LSBE_EVENT_FORMAT);
    jobMsgAckLog->msg = putstr_(strBuf);
    if (!jobMsgAckLog->msg)
        return(LSBE_NO_MEM);

    line += ccount + 1;

    cc = sscanf(line, "%d%n", &(jobMsgAckLog->idx), &ccount);
    if (cc != 1)
        return (LSBE_EVENT_FORMAT);
    line += ccount + 1;
    return (LSBE_NO_ERROR);

}


static int
writeJobForce(FILE* log_fp, struct jobForceRequestLog* jobForceRequestLog)
{
    int i;

    if (fprintf(log_fp, " %d %d %d %d %d ",
                jobForceRequestLog->jobId,
                jobForceRequestLog->userId,
                jobForceRequestLog->idx,
                jobForceRequestLog->options,
                jobForceRequestLog->numExecHosts) < 0)
        return(LSBE_SYS_CALL);

    for (i = 0; i < jobForceRequestLog->numExecHosts; i++) {
        if (addQStr (log_fp, jobForceRequestLog->execHosts[i]) < 0)
            return (LSBE_SYS_CALL);
    }

    if (addQStr (log_fp, jobForceRequestLog->userName) < 0)
        return (LSBE_SYS_CALL);

    if (fputc('\n', log_fp) == EOF)
        return(LSBE_SYS_CALL);

    return(LSBE_NO_ERROR);

}


static int
writeLogSwitch(FILE* log_fp, struct logSwitchLog* logSwitchLog)
{
    if (fprintf(log_fp, " %d ", logSwitchLog->lastJobId) < 0)
        return(LSBE_SYS_CALL);
    if (fputc('\n', log_fp) == EOF)
        return(LSBE_SYS_CALL);
    return(LSBE_NO_ERROR);
}

static int
readJobForce(char* line, struct jobForceRequestLog* jobForceRequestLog)
{
    int          i;
    int          cc;
    int          ccount;

    cc = sscanf(line, "%d%d%d%d%d%n",
                &(jobForceRequestLog->jobId),
                &(jobForceRequestLog->userId),
                &(jobForceRequestLog->idx),
                &(jobForceRequestLog->options),
                &(jobForceRequestLog->numExecHosts),
                &(ccount));

    if (cc != 5)
        return(LSBE_EVENT_FORMAT);
    line += ccount + 1;


    jobForceRequestLog->execHosts =
        (char **)calloc(jobForceRequestLog->numExecHosts,
                        sizeof (char *));
    if (jobForceRequestLog->execHosts == NULL) {
        jobForceRequestLog->numExecHosts = 0;
        return (LSBE_NO_MEM);
    }

    for (i = 0; i < jobForceRequestLog->numExecHosts; i++) {
        char hName[MAXLINELEN];


        if ((cc = stripQStr(line, hName)) < 0) {
            jobForceRequestLog->numExecHosts = i;
            return (LSBE_EVENT_FORMAT);
        }


        jobForceRequestLog->execHosts[i] = putstr_(hName);
        if (jobForceRequestLog->execHosts[i] == NULL) {
            jobForceRequestLog->numExecHosts = i;
            return (LSBE_NO_MEM);
        }

        line += cc + 1;
    }

    copyQStr(line, MAX_LSB_NAME_LEN, 1, jobForceRequestLog->userName);

    return(LSBE_NO_ERROR);
}

static int
writeJobAttrSet(FILE* log_fp, struct jobAttrSetLog *jobAttrSetLog)
{
    if (fprintf(log_fp, " %d %d %d %d ",
                jobAttrSetLog->jobId,
                jobAttrSetLog->idx,
                jobAttrSetLog->uid,
                jobAttrSetLog->port) < 0)
        return(LSBE_SYS_CALL);

    if (fputc('\n', log_fp) == EOF)
        return(LSBE_SYS_CALL);

    return(LSBE_NO_ERROR);

}

static int
readJobAttrSet(char* line, struct jobAttrSetLog *jobAttrSetLog)
{
    int ccount;

    if (sscanf(line, "%d%d%d%d%n",
               &(jobAttrSetLog->jobId),
               &(jobAttrSetLog->idx),
               &(jobAttrSetLog->uid),
               &(jobAttrSetLog->port),
               &ccount) != 4)
        return(LSBE_EVENT_FORMAT);
    return(LSBE_NO_ERROR);
}

void countLineNum(FILE *fp, long pos,  int *lineNum)
{
    char ch;
    long curPos;

    while (((ch = getc(fp)) != EOF) && ((curPos = ftell(fp)) <= pos)) {
        if (ch == '\n')
            *lineNum += 1;
        if (curPos == pos)
            return;
    }
}

struct eventRec *
lsbGetNextJobEvent (struct eventLogHandle *ePtr, int *lineNum,
                    int numJobIds, LS_LONG_INT *jobIds, struct jobIdIndexS *indexS)
{
    static char fname[] = "lsbGetNextJobEvent";
    struct eventRec *logRec = NULL;
    FILE *newfp;
    char *sp, eventFile[MAXFILENAMELEN];

    while(TRUE) {
        if (ePtr->fp != NULL)
            logRec = lsbGetNextJobRecFromFile(ePtr->fp, lineNum,
                                              numJobIds, jobIds);
        else
            lsberrno = LSBE_EOF;

        if (logRec == NULL &&
            ePtr->curOpenFile > 0 &&
            lsberrno == LSBE_EOF &&
            ePtr->curOpenFile > ePtr->lastOpenFile) {


            if (ePtr->fp != NULL)
                fclose(ePtr->fp);

            if ((sp = strstr(ePtr->openEventFile, "lsb.events")))
                *(sp-1) = '\0';
            if (ePtr->curOpenFile == 1) {
                sprintf  (eventFile, "%s/lsb.events", ePtr->openEventFile);
                ePtr->curOpenFile = 0;
                ePtr->lastOpenFile = -1;
            } else {
                int nextFileNumber = 0;
                if (indexS != NULL) {
                    nextFileNumber = getNextFileNumFromIndexS (indexS,
                                                               numJobIds, jobIds);
                }

                if ((indexS == NULL) || (nextFileNumber == -1)) {
                    sprintf  (eventFile, "%s/lsb.events.%d",
                              ePtr->openEventFile,
                              --(ePtr->curOpenFile));
                } else {
                    if (nextFileNumber == 0) {
                        sprintf  (eventFile, "%s/lsb.events",
                                  ePtr->openEventFile);
                        ePtr->curOpenFile = 0;
                        ePtr->lastOpenFile = -1;
                    } else {
                        sprintf  (eventFile, "%s/lsb.events.%d",
                                  ePtr->openEventFile,
                                  nextFileNumber);
                        ePtr->curOpenFile = nextFileNumber;
                    }
                }
            }

            strcpy(ePtr->openEventFile, eventFile);
            *lineNum = 0;
            if ((newfp = fopen (eventFile, "r")) == NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "fopen",
                          eventFile);
                lsberrno = LSBE_SYS_CALL;
                return (NULL);
            } else if (ePtr->curOpenFile == 0) {
                char ch;
                int pos;


                if (fscanf(newfp, "%c%d ", &ch, &pos) != 2
                    || ch != '#') {
                    ls_syslog(LOG_INFO, I18N(5501,
                                             "%s: fscanf(%s) warning: old event file format"), /* catgets 5501 */
                              fname, eventFile);
                    pos = 0;
                } else {
                    *lineNum = 1;
                    countLineNum(newfp, pos, lineNum);
                }
                if (fseek (newfp, pos, SEEK_SET) != 0)
                    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M,  fname, "fseek", pos);
            }
            ePtr->fp = newfp;
            continue;
        }
        return (logRec);
    }
}

struct eventRec *
lsbGetNextJobRecFromFile(FILE *logFp, int *lineNum,
                         int numJobIds, LS_LONG_INT *jobIds)
{
    static  char            fname[] = "lsbGetNextJobRecFromFile";
    int                     cc;
    int                     ccount;
    char*                   line;
    char                    nameBuf[MAXLINELEN];
    static struct eventRec* logRec;
    int                     eventKind;
    int                     tempTimeStamp;

    if (logRec != NULL)  {
        freeLogRec(logRec);
        free(logRec);
    }


    logRec = (struct eventRec *) calloc (1, sizeof (struct eventRec));
    if (logRec == NULL) {
        lsberrno = LSBE_NO_MEM;
        return NULL;
    }

    while(TRUE) {
        (*lineNum)++;

        if ((line = getNextLine_(logFp, FALSE)) == NULL) {
            if (lserrno == LSE_NO_MEM) {
                lsberrno = LSBE_NO_MEM;
            } else {
                lsberrno = LSBE_EOF;
            }
            break;
        }

        if (*line == '#')

            continue;

        if (logclass & LC_TRACE)
            ls_syslog(LOG_DEBUG2, "%s: line=%s", fname, line);

        if ((ccount = stripQStr(line, nameBuf)) < 0 ||
            strlen(nameBuf) >= MAX_LSB_NAME_LEN) {
            lsberrno = LSBE_EVENT_FORMAT;
            break;
        }
        line += ccount + 1;

        if ((logRec->type = getEventTypeAndKind(nameBuf, &eventKind)) == -1)
            continue;

        if (eventKind != EVENT_JOB_RELATED)
            continue;

        if ((ccount = stripQStr(line, nameBuf)) < 0 ||
            strlen(nameBuf) >= MAX_VERSION_LEN) {
            lsberrno = LSBE_EVENT_FORMAT;
            break;
        }

        strcpy(logRec->version, nameBuf);
        if ((version = atof (logRec->version)) <=0.0) {
            lsberrno = LSBE_EVENT_FORMAT;
            break;
        }

        line += ccount + 1;

        cc = sscanf(line, "%d%n", &tempTimeStamp, &ccount);
        if (cc != 1) {
            lsberrno = LSBE_EVENT_FORMAT;
            break;
        }
        logRec->eventTime=tempTimeStamp;
        line += ccount + 1;


        if (!checkJobEventAndJobId(line, logRec->type, numJobIds, jobIds))
            continue;

        if (logclass & LC_TRACE)
            ls_syslog(LOG_DEBUG2, "%s: log.type=%x", fname, logRec->type);

        readEventRecord (line, logRec);

        break;

    }

    if (lsberrno == LSBE_NO_ERROR)
        return (logRec);

    return(NULL);

}

int
checkJobEventAndJobId(char *line, int eventType, int numJobIds, LS_LONG_INT *jobIds)
{

    int     cc;
    int     eventJobId;
    int     i;

    switch (eventType) {
        case EVENT_JOB_NEW:
        case EVENT_JOB_MODIFY:
        case EVENT_PRE_EXEC_START:
        case EVENT_JOB_START:
        case EVENT_JOB_START_ACCEPT:
        case EVENT_JOB_STATUS:
        case EVENT_SBD_JOB_STATUS:
        case EVENT_JOB_FINISH:
        case EVENT_CHKPNT:
        case EVENT_MIG:
        case EVENT_JOB_ATTR_SET:
        case EVENT_JOB_SIGNAL:
        case EVENT_JOB_EXECUTE:
        case EVENT_JOB_MSG:
        case EVENT_JOB_MSG_ACK:
        case EVENT_JOB_SIGACT:
        case EVENT_JOB_REQUEUE:
        case EVENT_JOB_CLEAN:
        case EVENT_JOB_FORCE:
            cc = sscanf(line, "%d", (int *)&eventJobId);
            if (cc != 1) {
                return(1);
            }
            break;
        case EVENT_JOB_MODIFY2:
            cc = sscanf(line, "\"%d[", (int *)&eventJobId);
            if (cc != 1) {
                return(1);
            }
            break;
        case EVENT_JOB_SWITCH:
        case EVENT_JOB_MOVE:
        { int tempUserId;

                cc = sscanf(line, "%d %d",
                            (int *)&tempUserId,
                            (int *)&eventJobId);
                if (cc != 2) {
                    return(1);
                }
        }
        break;
        default:
            return(0);
    }


    if (!numJobIds || eventType == EVENT_JOB_NEW)
        return(1);


    for (i=0; i<numJobIds; i++) {
        if (LSB_ARRAY_JOBID(jobIds[i]) == eventJobId)
            return(1);
    }

    return(0);

}

int
getEventTypeAndKind(char *typeStr, int *eventKind)
{
    int eventType;

    if (strcmp(typeStr, "JOB_NEW") == 0)
        eventType = EVENT_JOB_NEW;
    else if (strcmp(typeStr, "JOB_START") == 0)
        eventType = EVENT_JOB_START;
    else if (strcmp(typeStr, "JOB_START_ACCEPT") == 0)
        eventType = EVENT_JOB_START_ACCEPT;
    else if (strcmp(typeStr, "JOB_STATUS") == 0)
        eventType = EVENT_JOB_STATUS;
    else if (strcmp(typeStr, "SBD_JOB_STATUS") == 0)
        eventType = EVENT_SBD_JOB_STATUS;
    else if (strcmp(typeStr, "JOB_EXECUTE") == 0)
        eventType = EVENT_JOB_EXECUTE;
    else if (strcmp(typeStr, "JOB_CLEAN") == 0)
        eventType = EVENT_JOB_CLEAN;
    else if (strcmp(typeStr, "JOB_SWITCH") == 0)
        eventType = EVENT_JOB_SWITCH;
    else if (strcmp(typeStr, "JOB_MOVE") == 0)
        eventType = EVENT_JOB_MOVE;
    else if (strcmp(typeStr, "JOB_FINISH") == 0)
        eventType = EVENT_JOB_FINISH;
    else if (strcmp(typeStr, "LOAD_INDEX") == 0)
        eventType = EVENT_LOAD_INDEX;
    else if (strcmp(typeStr, "CHKPNT") == 0)
        eventType = EVENT_CHKPNT;
    else if (strcmp(typeStr, "MIG") == 0)
        eventType = EVENT_MIG;
    else if (strcmp(typeStr, "PRE_EXEC_START") == 0)
        eventType = EVENT_PRE_EXEC_START;
    else if (strcmp(typeStr, "JOB_MODIFY") == 0)
        eventType = EVENT_JOB_MODIFY;
    else if (strcmp(typeStr, "JOB_MODIFY2") == 0)
        eventType = EVENT_JOB_MODIFY2;
    else if (strcmp(typeStr, "JOB_ATTR_SET") == 0)
        eventType = EVENT_JOB_ATTR_SET;
    else if (strcmp(typeStr, "QUEUE_CTRL") == 0)
        eventType = EVENT_QUEUE_CTRL;
    else if (strcmp(typeStr, "HOST_CTRL") == 0)
        eventType = EVENT_HOST_CTRL;
    else if (strcmp(typeStr, "MBD_START") == 0)
        eventType = EVENT_MBD_START;
    else if (strcmp(typeStr, "MBD_DIE") == 0)
        eventType = EVENT_MBD_DIE;
    else if (strcmp(typeStr, "UNFULFILL") == 0)
        eventType = EVENT_MBD_UNFULFILL;
    else if (strcmp(typeStr, "JOB_SIGNAL") == 0)
        eventType = EVENT_JOB_SIGNAL;
    else if (strcmp(typeStr, "JOB_MSG") == 0)
        eventType = EVENT_JOB_MSG;
    else if (strcmp(typeStr, "JOB_MSG_ACK") == 0)
        eventType = EVENT_JOB_MSG_ACK;
    else if (strcmp(typeStr, "JOB_REQUEUE") == 0)
        eventType = EVENT_JOB_REQUEUE;
    else if (strcmp(typeStr, "JOB_SIGACT") == 0)
        eventType = EVENT_JOB_SIGACT;
    else if (strcmp(typeStr, "JOB_FORCE") == 0)
        eventType = EVENT_JOB_FORCE;
    else if (strcmp(typeStr, "LOG_SWITCH") == 0)
        eventType = EVENT_LOG_SWITCH;
    else if (strcmp(typeStr, "LOG_SWITCH") == 0)
        eventType = EVENT_LOG_SWITCH;
    else {
        lsberrno = LSBE_UNKNOWN_EVENT;
        *eventKind = EVENT_NON_JOB_RELATED;
        return (-1);
    }

    switch (eventType) {
        case EVENT_JOB_NEW:
        case EVENT_JOB_START:
        case EVENT_JOB_START_ACCEPT:
        case EVENT_JOB_STATUS:
        case EVENT_SBD_JOB_STATUS:
        case EVENT_JOB_SWITCH:
        case EVENT_JOB_MOVE:
        case EVENT_JOB_FINISH:
        case EVENT_CHKPNT:
        case EVENT_MIG:
        case EVENT_PRE_EXEC_START:
        case EVENT_JOB_MODIFY:
        case EVENT_JOB_MODIFY2:
        case EVENT_JOB_ATTR_SET:
        case EVENT_JOB_SIGNAL:
        case EVENT_JOB_EXECUTE:
        case EVENT_JOB_MSG:
        case EVENT_JOB_MSG_ACK:
        case EVENT_JOB_REQUEUE:
        case EVENT_JOB_CLEAN:
        case EVENT_JOB_SIGACT:
        case EVENT_JOB_FORCE:
            *eventKind = EVENT_JOB_RELATED;
            break;
        default:
            *eventKind = EVENT_NON_JOB_RELATED;
            break;
    }

    return(eventType);
}



int
lsb_readeventrecord(char *line, struct eventRec *logRec)
{
    static  char            fname[] = "lsb_readeventrecord";
    char                    etype[MAX_LSB_NAME_LEN];
    char                    namebuf[MAXLINELEN];
    int                     cc;
    int                     ccount;
    int                     eventKind;


    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG2, "%s: Entering ...", fname);
    }

    if ( line == NULL || logRec == NULL ) {
        ls_syslog(LOG_DEBUG2, "%s: line or logRec is NULL", fname);
        return (-1);
    }
    memset((char *)logRec, 0, sizeof(struct eventRec));


    if ((ccount = stripQStr(line, namebuf)) < 0
        || strlen(namebuf) >= MAX_LSB_NAME_LEN) {
        ls_syslog(LOG_DEBUG2, "%s: get event type fail", fname);
        return (-1);
    }
    line += ccount + 1;
    strcpy(etype, namebuf);


    if ((ccount = stripQStr(line, namebuf)) < 0
        || strlen(namebuf) >= MAX_VERSION_LEN) {
        ls_syslog(LOG_DEBUG2, "%s: get event version fail", fname);
        return (-1);
    }
    line += ccount + 1;
    strcpy(logRec->version, namebuf);


    cc = sscanf(line, "%d%n", (int *)&(logRec->eventTime), &ccount);
    if (cc != 1) {
        ls_syslog(LOG_DEBUG2, "%s: get event time stamp fail", fname);
        return (-1);
    }
    line += ccount + 1;

    if ((version = atof (logRec->version)) <=0.0) {
        ls_syslog(LOG_DEBUG2, "%s: get event version error", fname);
        return (-1);
    }
    if ((logRec->type = getEventTypeAndKind(etype, &eventKind)) == -1) {
        ls_syslog(LOG_DEBUG2, "%s: get event time stamp error", fname);
        return (-1);
    }


    lsberrno = LSBE_NO_ERROR;
    readEventRecord(line, logRec);
    if (lsberrno != LSBE_NO_ERROR) {
        ls_syslog(LOG_DEBUG2, "%s: readEventRecord fail", fname);
        return (-1);
    }
    return (0);

}

void
readEventRecord (char *line, struct eventRec *logRec)
{

    switch (logRec->type) {
        case EVENT_JOB_NEW:
        case EVENT_JOB_MODIFY:
            lsberrno = readJobNew(line, &(logRec->eventLog.jobNewLog));
            break;
        case EVENT_JOB_MODIFY2:
            lsberrno = readJobMod(line, &(logRec->eventLog.jobModLog));
            break;
        case EVENT_PRE_EXEC_START:
        case EVENT_JOB_START:
            lsberrno = readJobStart(line, &(logRec->eventLog.jobStartLog));
            break;
        case EVENT_JOB_START_ACCEPT:
            lsberrno = readJobStartAccept(line,
                                          &(logRec->eventLog.jobStartAcceptLog));
            break;
        case EVENT_JOB_STATUS:
            lsberrno = readJobStatus(line, &(logRec->eventLog.jobStatusLog));
            break;
        case EVENT_SBD_JOB_STATUS:
            lsberrno = readSbdJobStatus(line, &(logRec->eventLog.sbdJobStatusLog));
            break;
        case EVENT_JOB_SWITCH:
            lsberrno = readJobSwitch(line, &(logRec->eventLog.jobSwitchLog));
            break;
        case EVENT_JOB_MOVE:
            lsberrno = readJobMove(line, &(logRec->eventLog.jobMoveLog));
            break;
        case EVENT_QUEUE_CTRL:
            lsberrno = readQueueCtrl(line, &(logRec->eventLog.queueCtrlLog));
            break;
        case EVENT_HOST_CTRL:
            lsberrno = readHostCtrl(line, &(logRec->eventLog.hostCtrlLog));
            break;
        case EVENT_MBD_START:
            lsberrno = readMbdStart(line, &(logRec->eventLog.mbdStartLog));
            break;
        case EVENT_MBD_DIE:
            lsberrno = readMbdDie (line, &(logRec->eventLog.mbdDieLog));
            break;
        case EVENT_MBD_UNFULFILL:
            lsberrno = readUnfulfill (line, &(logRec->eventLog.unfulfillLog));
            break;
        case EVENT_LOAD_INDEX:
            lsberrno = readLoadIndex (line, &(logRec->eventLog.loadIndexLog));
            break;
        case EVENT_JOB_FINISH:
            lsberrno = readJobFinish(line, &(logRec->eventLog.jobFinishLog),
                                     logRec->eventTime);
            break;
        case EVENT_CHKPNT:
            lsberrno = readChkpnt(line, &(logRec->eventLog.chkpntLog));
            break;
        case EVENT_MIG:
            lsberrno = readMig(line, &(logRec->eventLog.migLog));
            break;
        case EVENT_JOB_ATTR_SET:
            lsberrno = readJobAttrSet(line, &(logRec->eventLog.jobAttrSetLog));
            break;
        case EVENT_JOB_SIGNAL:
            lsberrno = readJobSignal(line, &(logRec->eventLog.signalLog));
            break;
        case EVENT_JOB_EXECUTE:
            lsberrno = readJobExecute(line, &(logRec->eventLog.jobExecuteLog));
            break;
        case EVENT_JOB_MSG:
            lsberrno = readJobMsg(line, &(logRec->eventLog.jobMsgLog));
            break;
        case EVENT_JOB_MSG_ACK:
            lsberrno = readJobMsgAck(line, &(logRec->eventLog.jobMsgAckLog));
            break;
        case EVENT_JOB_SIGACT:
            lsberrno = readJobSigAct(line, &(logRec->eventLog.sigactLog));
            break;
        case EVENT_JOB_REQUEUE:
            lsberrno = readJobRequeue(line, &(logRec->eventLog.jobRequeueLog));
            break;
        case EVENT_JOB_CLEAN:
            lsberrno = readJobClean(line, &(logRec->eventLog.jobCleanLog));
            break;
        case EVENT_JOB_FORCE:
            lsberrno = readJobForce(line, &(logRec->eventLog.jobForceRequestLog));
            break;
        case EVENT_LOG_SWITCH:
            lsberrno = readLogSwitch(line, &(logRec->eventLog.logSwitchLog));
            break;
    }

    return;
}

int
getJobIdIndexFromEventFile (char *eventFile, struct sortIntList *header,
                            time_t *timeStamp)
{
    static  char       fname[] = "getJobIdIndexFromEventFile";
    FILE                    *eventFp;
    char                    ch;
    int                     jobId;
    int                     ccount;
    char*                   line;
    char                    nameBuf[MAXLINELEN];
    int                     eventKind;
    int                     eventType;
    int                     tempTimeStamp;
    int                     cc;


    if ((eventFp = fopen(eventFile, "r")) == NULL) {
        lsberrno = LSBE_SYS_CALL;
        return(-1);
    }


    cc = fscanf(eventFp, "%c%d", &ch, &tempTimeStamp);
    if (cc != 2 || ch != '#') {
        ls_syslog(LOG_INFO, I18N(5501,
                                 "%s: fscanf(%s) failed: old event file format"), /* catgets 5501 */
                  fname, eventFile);
        if (fseek (eventFp, 0, SEEK_SET) != 0)
            ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M,  fname, "fseek", 0);
        *timeStamp = 0;
    }
    *timeStamp=tempTimeStamp;


    while(TRUE) {

        if ((line = getNextLine_(eventFp, FALSE)) == NULL) {
            if (lserrno == LSE_NO_MEM) {
                lsberrno = LSBE_NO_MEM;
            } else {
                lsberrno = LSBE_EOF;
            }
            break;
        }

        if (*line == '#')

            continue;

        if ((ccount = stripQStr(line, nameBuf)) < 0 ||
            strlen(nameBuf) >= MAX_LSB_NAME_LEN) {
        }
        line += ccount + 1;

        if ((eventType = getEventTypeAndKind(nameBuf, &eventKind)) == -1)
            continue;

        if (eventKind != EVENT_JOB_RELATED)
            continue;

        if ((ccount = stripQStr(line, nameBuf)) < 0 ||
            strlen(nameBuf) >= MAX_VERSION_LEN) {
            continue;
        }

        line += ccount + 1;

        cc = sscanf(line, "%d%n", &tempTimeStamp,&ccount);
        if (cc != 1)
            continue;

        line += ccount + 1;

        if ((jobId = getJobIdFromEvent(line, eventType)) == 0)
            continue;

        if (insertSortIntList(header, jobId)<0) {
            lsberrno = LSBE_NO_MEM;
            break;
        }

    }

    fclose(eventFp);
    if (lsberrno != LSBE_EOF)
        return (-1);

    return(0);

}

int
getJobIdFromEvent(char *line, int eventType)
{

    int     cc;
    int     eventJobId;

    switch (eventType) {
        case EVENT_JOB_NEW:
        case EVENT_JOB_MODIFY:
        case EVENT_PRE_EXEC_START:
        case EVENT_JOB_START:
        case EVENT_JOB_START_ACCEPT:
        case EVENT_JOB_STATUS:
        case EVENT_SBD_JOB_STATUS:
        case EVENT_JOB_FINISH:
        case EVENT_CHKPNT:
        case EVENT_MIG:
        case EVENT_JOB_ATTR_SET:
        case EVENT_JOB_SIGNAL:
        case EVENT_JOB_EXECUTE:
        case EVENT_JOB_MSG:
        case EVENT_JOB_MSG_ACK:
        case EVENT_JOB_SIGACT:
        case EVENT_JOB_REQUEUE:
        case EVENT_JOB_CLEAN:
        case EVENT_JOB_FORCE:
            cc = sscanf(line, "%d", (int *)&eventJobId);
            if (cc != 1)
                return(0);
            break;
        case EVENT_JOB_MODIFY2:
            cc = sscanf(line, "\"%d[", (int *)&eventJobId);
            if (cc != 1)
                return(0);
            break;
        case EVENT_JOB_SWITCH:
        case EVENT_JOB_MOVE:
        { int tempUserId;
                cc = sscanf(line, "%d %d",
                            (int *)&tempUserId,
                            (int *)&eventJobId);
                if (cc != 2)
                    return(0);
        }
        break;
        default:
            return(0);
    }

    return(eventJobId);

}

int
writeJobIdIndexToIndexFile (FILE *indexFp, struct sortIntList *header,
                            time_t timeStamp)
{
    int                     minJobId;
    int                     maxJobId;
    int                     totalJobIds;
    struct sortIntList      *list;
    int                     col;
    int                     jobId;


    if (getMinSortIntList(header, &minJobId) == -1)
        minJobId = 0;

    if (getMaxSortIntList(header, &maxJobId) == -1)
        maxJobId = 0;

    totalJobIds = getTotalSortIntList(header);

    if (fprintf(indexFp, "#%d %d %s %s\n",
                (int) timeStamp,
                totalJobIds,
                lsb_jobidinstr(minJobId),
                lsb_jobidinstr(maxJobId)) < 0) {
        lsberrno = LSBE_SYS_CALL;
        return(-1);
    }

    list = header;
    col = 0;

    while((list = getNextSortIntList(header, list, &jobId)) != NULL) {
        if (col == 0) {
            if (fprintf(indexFp, "%d", jobId) < 0) {
                lsberrno = LSBE_SYS_CALL;
                return(-1);
            }
        } else {
            if (fprintf(indexFp, " %d", jobId) < 0) {
                lsberrno = LSBE_SYS_CALL;
                return(-1);
            }
        }

        col++;
        if (col == 10) {
            if (fprintf(indexFp, "\n") < 0) {
                lsberrno = LSBE_SYS_CALL;
                return(-1);
            }
            col = 0;
        }

    }

    if (col)
        if (fprintf(indexFp, "\n") < 0) {
            lsberrno = LSBE_SYS_CALL;
            return(-1);
        }

    return(0);

}

int
updateJobIdIndexFile (char *indexFile, char *eventFile, int totalEventFile)
{
    static  char       fname[] = "updateJobIdIndexFile";
    struct stat             st;
    FILE                    *indexFp;
    char                    nameBuf[MAXLINELEN];
    char                    indexVersion[16];
    int                     rows;
    int                     lastUpdate;
    struct sortIntList      *listHeader;
    time_t                  timeStamp;
    int                     addedEventFile;
    int                     i;

    lsberrno = LSBE_NO_ERROR;

    if (stat(indexFile, &st) == 0) {

        if ((indexFp = fopen(indexFile, "r+")) == NULL) {
            lsberrno = LSBE_SYS_CALL;
            return(-1);
        }

        if (fscanf(indexFp, "%s %s %d %d\n",
                   nameBuf, indexVersion, &rows, &lastUpdate) != 4 ||
            strcmp(nameBuf, LSF_JOBIDINDEX_FILETAG)) {
            ls_syslog(LOG_ERR, I18N(5506,
                                    "%s: %s is not an jobid index file"), /* catgets 5506 */
                      fname, indexFile);
            lsberrno = LSBE_INDEX_FORMAT;
            fclose(indexFp);
            unlink(indexFile);
            return(-1);
        }


        if (totalEventFile == rows + 1) {
            addedEventFile = 1;


            if (fseek(indexFp, 0, SEEK_END)) {
                lsberrno = LSBE_SYS_CALL;
                fclose(indexFp);
                return(-1);
            }
        } else {
            fclose(indexFp);
            indexFp = NULL;
        }

    }

    if ((stat(indexFile, &st) != 0) ||
        (indexFp == NULL)) {

        if ((indexFp = fopen(indexFile, "w+")) == NULL) {
            lsberrno = LSBE_SYS_CALL;
            return(-1);
        }


        if (fchmod(fileno(indexFp),  0644) != 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M,
                      fname, "fchmod", indexFile);
        }

        addedEventFile = totalEventFile;
        sprintf(indexVersion, "%d", VOLCLAVA_VERSION);
        rows = 0;


        fprintf(indexFp, "%80s", "\n");
    }


    for (i=addedEventFile; i>0; i--) {

        sprintf(nameBuf, "%s.%d", eventFile, i);


        if ((listHeader = initSortIntList(0)) == NULL) {
            lsberrno = LSBE_NO_MEM;
            break;
        }
        if (getJobIdIndexFromEventFile (nameBuf, listHeader, &timeStamp)) {
            freeSortIntList(listHeader);
            break;
        }
        if (writeJobIdIndexToIndexFile (indexFp, listHeader, timeStamp)) {
            freeSortIntList(listHeader);
            break;
        }
        freeSortIntList(listHeader);
        rows++;
    }


    lastUpdate = (int) time(NULL);


    errno = 0;
    rewind(indexFp);
    if (errno != 0) {
        lsberrno = LSBE_SYS_CALL;
        fclose(indexFp);
        return (-1);
    }


    if (fprintf(indexFp, "%s %s %d %d",
                LSF_JOBIDINDEX_FILETAG, indexVersion, rows, lastUpdate) < 0) {
        lsberrno = LSBE_SYS_CALL;
    }

    fclose(indexFp);
    if (lsberrno != LSBE_NO_ERROR && lsberrno != LSBE_EOF)
        return(-1);
    return(0);

}

int
getNextFileNumFromIndexS (struct jobIdIndexS *indexS, int numJobIds,
                          LS_LONG_INT *jobIds)
{
    int                     position;
    int                     i,j;
    int                     size;

    while(TRUE) {

        indexS->curRow++;
        if (indexS->curRow == indexS->totalRows) {
            free(indexS->jobIds);
            indexS->jobIds = NULL;
            fclose(indexS->fp);
            return(0);
        }

        if (indexS->jobIds != NULL) {
            free(indexS->jobIds);
            indexS->jobIds = NULL;
        }

        if (fscanf(indexS->fp, "#%ld %d %lld %lld\n",
                   &(indexS->timeStamp),
                   &(indexS->totalJobIds),
                   &(indexS->minJobId),
                   &(indexS->maxJobId)) != 4) {
            lsberrno = LSBE_SYS_CALL;
            return(-1);
        }

        size = indexS->totalJobIds*sizeof(int);
        if ((indexS->jobIds = (int *)malloc(size)) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(-1);
        }

        if (indexS->totalJobIds == 0)
            continue;


        position = 0;
        while (indexS->totalJobIds - position >= 10) {
            if (fscanf(indexS->fp, "%d %d %d %d %d %d %d %d %d %d\n",
                       &(indexS->jobIds[position]),
                       &(indexS->jobIds[position+1]),
                       &(indexS->jobIds[position+2]),
                       &(indexS->jobIds[position+3]),
                       &(indexS->jobIds[position+4]),
                       &(indexS->jobIds[position+5]),
                       &(indexS->jobIds[position+6]),
                       &(indexS->jobIds[position+7]),
                       &(indexS->jobIds[position+8]),
                       &(indexS->jobIds[position+9])) != 10) {
                lsberrno = LSBE_SYS_CALL;
                return(-1);
            }
            position+=10;
        }

        for(i=position; i<indexS->totalJobIds; i++) {
            if (fscanf(indexS->fp, "%d", &(indexS->jobIds[i])) != 1) {
                lsberrno = LSBE_SYS_CALL;
                return(-1);
            }
            if (i == indexS->totalJobIds - 1)
                if (fscanf(indexS->fp, "\n") < 0) {
                    lsberrno = LSBE_SYS_CALL;
                    return(-1);
                }
        }


        for (i=0; i<numJobIds; i++) {
            int jobId;

            jobId = LSB_ARRAY_JOBID(jobIds[i]);

            if ((jobId > indexS->maxJobId) ||
                (jobId < indexS->minJobId))
                continue;
            for (j=0; j<indexS->totalJobIds; j++) {
                if (jobId == indexS->jobIds[j]) {
                    return(indexS->totalRows-indexS->curRow+1);
                }

                if (jobId > indexS->jobIds[j])
                    break;
            }
        }

    }

    return(0);

}

time_t
lsb_getAcctFileTime(char * fileName)
{
    static char fname[] = "lsb_getAcctFileTime";
    struct eventRec * acctLogPtr = NULL;
    struct stat jbuf;
    FILE * acctLogFp = NULL;
    int lineNum = 0;
    time_t lastAcctCreationTime = -1;

    if ((fileName == NULL) || (stat(fileName, &jbuf) < 0)){
        return -1;
    }
    if (lsberrno == LSBE_EOF){
        lsberrno = LSBE_NO_ERROR;
    }
    if ((acctLogFp = fopen(fileName, "r")) != NULL){
        while (lsberrno != LSBE_EOF){
            if ((acctLogPtr = lsb_geteventrec(acctLogFp, &lineNum)) == NULL) {
                if (lsberrno != LSBE_EOF){

                    if (logclass & LC_EXEC){
                        ls_syslog(LOG_DEBUG, I18N(5507, "%s: Error in reading event file <%s> at line <%d>\n"), /* catgets 5507 */
                                  fname,
                                  fileName,
                                  lineNum);
                    }
                } else {
                    fclose(acctLogFp);
                    if (jbuf.st_size == 0){
                        lastAcctCreationTime = jbuf.st_mtime;
                    }
                }
            } else {
                lastAcctCreationTime = acctLogPtr->eventTime;
                fclose(acctLogFp);
                break;
            }
        }
    }
    return lastAcctCreationTime;
}

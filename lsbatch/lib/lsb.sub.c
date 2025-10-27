/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.sub.c 397 2007-11-26 19:04:00Z mblack $
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
#include <ctype.h>
#include <sys/file.h>

#include <string.h>
#include <sys/stat.h>
#ifdef _XOPEN_SOURCE
#include <time.h>
#else
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <setjmp.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/wait.h>
#include <stdio.h>
#include <grp.h>

#define exit(a)         _exit(a)

#include <signal.h>
#include <stdio.h>

#include <pwd.h>
#include "../../lsf/lib/lib.h"
#include "lsb.h"
#include "../../lsf/lib/lib.table.h"
#include "../../lsf/lib/mls.h"

#include "../../lsf/intlib/intlibout.h"
#include <dirent.h>

#include "lsb.spool.h"

#define SKIPSPACE(sp)      while (isspace(*(sp))) (sp)++;

#define EMBED_INTERACT     0x01
#define EMBED_OPTION_ONLY  0x02
#define EMBED_BSUB         0x04
#define EMBED_RESTART      0x10
#define EMBED_QSUB         0x20

#define MAX_ESUB_LIST 16
#define MAX_APP_NAME 64

#define PRINT_ERRMSG0(errMsg, fmt)\
    {\
	if (errMsg == NULL)\
	    fprintf(stderr, fmt);\
        else\
	    sprintf(*errMsg, fmt);\
    }
#define PRINT_ERRMSG1(errMsg, fmt, msg1)\
    {\
	if (errMsg == NULL)\
	    fprintf(stderr, fmt, msg1);\
        else\
	    sprintf(*errMsg, fmt, msg1);\
    }
#define PRINT_ERRMSG2(errMsg, fmt, msg1, msg2)\
    {\
	if (errMsg == NULL)\
	    fprintf(stderr, fmt, msg1, msg2);\
        else\
	    sprintf(*errMsg, fmt, msg1, msg2);\
    }
#define PRINT_ERRMSG3(errMsg, fmt, msg1, msg2, msg3)\
    {\
	if (errMsg == NULL)\
	    fprintf(stderr, fmt, msg1, msg2, msg3);\
        else\
	    sprintf(*errMsg, fmt, msg1, msg2, msg3);\
    }

int  optionFlag = FALSE ;
char optionFileName[MAXLSFNAMELEN];
char *loginShell;
static char *additionEsubInfo=NULL;

extern const char *defaultSpoolDir;
static const char* getDefaultSpoolDir();

extern void sub_perror(char *usrMsg);

static void trimSpaces(char *str);
static int parseXF(struct submit *, char *, char **);
void subUsage_(int, char **);
static int checkLimit(int limit, int factor);

int mySubUsage_(void *);
int bExceptionTabInit(void);

static hTab *bExceptionTab;

typedef int (*bException_handler_t)(void *);

typedef struct bException {
    char *name;
    bException_handler_t handler;
} bException_t;

extern char **environ;

extern char *yyerr;

extern int optind;
extern char *optarg;
extern char *my_getopt (int, char**, char*, char **);
extern char * getNextLine_(FILE *fp, int confFormat);
extern uid_t getuid(void);
extern char *lsb_sysmsg (void);
extern int _lsb_conntimeout;

extern int lsbMode_;
extern int lsbUnitForLimits;

static char *useracctmap = NULL;
static struct lenData ed = {0, NULL};

static LS_LONG_INT  send_batch(struct submitReq *, struct lenData *,
		       struct submitReply *, struct lsfAuth *);
static int dependCondSyntax(char *);
static int createJobInfoFile(struct submit *, struct lenData *);
static LS_LONG_INT subRestart(struct submit  *jobSubReq, struct submitReq *submitReq,
		      struct submitReply *submitRep, struct lsfAuth *auth);
static LS_LONG_INT subJob(struct submit  *jobSubReq, struct submitReq *submitReq,
		  struct submitReply *submitRep, struct lsfAuth *auth);
static int getUserInfo(struct submitReq *, struct submit *);
static char * acctMapGet(int *, char *);

static int xdrSubReqSize(struct submitReq *req);
static int createNiosSock(struct submitReq *);

int getChkDir(char *, char *);
static void startNios(struct submitReq *, int, LS_LONG_INT);
static void postSubMsg(struct submit *, LS_LONG_INT, struct submitReply *);
static int readOptFile(char *filename, char *childLine);

static const LSB_SPOOL_INFO_T * chUserCopySpoolFile(const char * srcFile,
				    spoolOptions_t fileType);

static int split_app_list(const char* str, char arr[][MAX_APP_NAME], int max_count);
static int is_executable(const char* path);

extern void makeCleanToRunEsub();
extern char *translateString(char *);
extern void modifyJobInformation(struct submit *);
extern void compactXFReq(struct submit *);
extern char *wrapCommandLine(char *);
extern char *unwrapCommandLine(char *);
extern int  checkEmptyString(char *);
extern int  stringIsToken(char *,char *);
extern int  stringIsDigitNumber(char *s);
extern int processXFReq(char *key,char *line,struct submit *jobSubReq);
extern char *extractStringValue(char *line);


#define  NL_SETN   13


#define ESUBNAME "esub"


char *niosArgv[5];
char niosPath[MAXFILENAMELEN];


LS_LONG_INT
lsb_submit(struct submit  *jobSubReq, struct submitReply *submitRep)
{
    static char fname[] = "lsb_submit";
    struct submitReq submitReq;
    LS_LONG_INT jobId = -1;
    struct lsfAuth auth;
    char cwd[MAXFILENAMELEN];
    struct group *grpEntry;
    int loop;
    char * queue = NULL;

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    lsberrno = LSBE_BAD_ARG;


    subNewLine_(jobSubReq->resReq);
    subNewLine_(jobSubReq->dependCond);
    subNewLine_(jobSubReq->preExecCmd);
    subNewLine_(jobSubReq->postExecCmd);
    subNewLine_(jobSubReq->mailUser);
    subNewLine_(jobSubReq->jobName);
    subNewLine_(jobSubReq->queue);
    subNewLine_(jobSubReq->inFile);
    subNewLine_(jobSubReq->outFile);
    subNewLine_(jobSubReq->errFile);
    subNewLine_(jobSubReq->chkpntDir);
    subNewLine_(jobSubReq->projectName);
    for(loop = 0; loop < jobSubReq->numAskedHosts; loop++) {
        subNewLine_(jobSubReq->askedHosts[loop]);
    }


    if (getCommonParams (jobSubReq, &submitReq, submitRep) < 0)
        return (-1);



    if (!(jobSubReq->options & SUB_QUEUE)) {

        if ((queue = getenv("LSB_DEFAULTQUEUE")) != NULL
             && queue[0] != '\0') {
            submitReq.queue = queue;
            submitReq.options |= SUB_QUEUE;
        }
    }

    submitReq.cwd = cwd;


    if ((grpEntry = getgrgid(getgid())) == NULL) {
	if (logclass & ( LC_TRACE | LC_EXEC))
	    ls_syslog(LOG_DEBUG, "%s: group id %d, does not have an name in the unix group file", fname, (int)getgid());
    } else {


	if (putEnv("LSB_UNIXGROUP", grpEntry->gr_name) < 0) {
	    if (logclass & ( LC_TRACE | LC_EXEC ))
		ls_syslog(LOG_DEBUG, "%s: group <%s>, cannot be set in the environment.", fname, grpEntry->gr_name);
	}
    }

    makeCleanToRunEsub();


    if (getUserInfo(&submitReq, jobSubReq) < 0)
        return (-1);


    if (!(jobSubReq->options & SUB_QUEUE)) {
        if (queue != NULL && queue[0] != '\0') {
            jobSubReq->queue = queue;
            jobSubReq->options |= SUB_QUEUE;
        }
    }

    modifyJobInformation(jobSubReq);
    if (getCommonParams (jobSubReq, &submitReq, submitRep) < 0)
        return (-1);

#ifdef INTER_DAEMON_AUTH

    putEnv("LSF_EAUTH_AUX_PASS", "yes");
#endif


    if ( (lsbParams[LSB_INTERACTIVE_STDERR].paramValue != NULL)
	 && (strcasecmp(lsbParams[LSB_INTERACTIVE_STDERR].paramValue,
			"y") == 0) ) {
	if (putEnv("LSF_INTERACTIVE_STDERR", "y") < 0) {
	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_S, fname, "putenv");
	}
    }

    if (authTicketTokens_(&auth, NULL) == -1) {
        return (-1);
    }

    if (submitReq.options & SUB_RESTART)
        jobId = subRestart(jobSubReq, &submitReq, submitRep, &auth);
    else
        jobId = subJob(jobSubReq, &submitReq, submitRep, &auth);

    return(jobId);

}


int
getCommonParams (struct submit  *jobSubReq, struct submitReq *submitReq,
                 struct submitReply *submitRep)
{
    int i, m, useKb = 0;
    static char fname[] = "getCommonParams";

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if (jobSubReq == NULL || submitRep == NULL)
        return (-1);

    submitReq->options = jobSubReq->options;
    submitReq->options2 = jobSubReq->options2;

    submitRep->subTryInterval = DEF_SUB_TRY_INTERVAL;

    if (jobSubReq->options & SUB_DEPEND_COND) {
        if (dependCondSyntax(jobSubReq->dependCond) < 0)
	    return (-1);
        else {
            submitReq->dependCond = jobSubReq->dependCond;
        }
    } else
        submitReq->dependCond = "";

    if (jobSubReq->options & SUB_PRE_EXEC) {
        if (!jobSubReq->preExecCmd)
	    return (-1);
        if (strlen(jobSubReq->preExecCmd) >= MAXLINELEN - 1)
	    return (-1);
        else
            submitReq->preExecCmd = jobSubReq->preExecCmd;
    } else
        submitReq->preExecCmd = "";

    if (jobSubReq->options & SUB_POST_EXEC) {
        if (!jobSubReq->postExecCmd)
            return (-1);
        if (strlen(jobSubReq->postExecCmd) >= MAXLINELEN - 1)
            return (-1);
        else
            submitReq->postExecCmd = jobSubReq->postExecCmd;
    } else
        submitReq->postExecCmd = "";

    if (jobSubReq->options & SUB_QUEUE) {
        if (!jobSubReq->queue) {
            lsberrno = LSBE_BAD_QUEUE;
	    return (-1);
        }
	submitReq->queue = jobSubReq->queue;
    } else {
        submitReq->queue = "";
    }

    if (jobSubReq->options & SUB_HOST) {
        submitReq->numAskedHosts = jobSubReq->numAskedHosts;
        submitReq->askedHosts = jobSubReq->askedHosts;
    } else
	submitReq->numAskedHosts = 0;

    if (submitReq->numAskedHosts < 0)
	return (-1);

    for (i = 0; i < submitReq->numAskedHosts; i++) {
         if ((submitReq->askedHosts[i])
                && (strlen (submitReq->askedHosts[i]) + 1) < MAXHOSTNAMELEN)
             continue;
         lsberrno = LSBE_BAD_HOST;
         submitRep->badReqIndx = i;
	 return (-1);
    }

    if (jobSubReq->options & SUB_HOST_SPEC) {
        if (!jobSubReq->hostSpec)
	    return (-1);
        if (strlen(jobSubReq->hostSpec) >= MAXHOSTNAMELEN - 1)
	    return (-1);
        else
            submitReq->hostSpec = jobSubReq->hostSpec;
    } else
        submitReq->hostSpec = "";

    submitReq->beginTime = jobSubReq->beginTime;
    submitReq->termTime = jobSubReq->termTime;


    if (!limitIsOk_(jobSubReq->rLimits)) {
	useKb = 1;
	submitReq->options |= SUB_RLIMIT_UNIT_IS_KB;
    }

    for (i = 0; i < LSF_RLIM_NLIMITS; i++)
        submitReq->rLimits[i] = -1;

    if (jobSubReq->rLimits[LSF_RLIMIT_CPU] >= 0)
        submitReq->rLimits[LSF_RLIMIT_CPU] =
				    jobSubReq->rLimits[LSF_RLIMIT_CPU];
    if (jobSubReq->rLimits[LSF_RLIMIT_RUN] >= 0)
        submitReq->rLimits[LSF_RLIMIT_RUN] =
				    jobSubReq->rLimits[LSF_RLIMIT_RUN];

    if (jobSubReq->rLimits[LSF_RLIMIT_FSIZE] > 0) {
	if (useKb) {
	    submitReq->rLimits[LSF_RLIMIT_FSIZE] =
			jobSubReq->rLimits[LSF_RLIMIT_FSIZE];
	} else {
	    submitReq->rLimits[LSF_RLIMIT_FSIZE] =
			jobSubReq->rLimits[LSF_RLIMIT_FSIZE] * 1024;
	}
    }
    if (jobSubReq->rLimits[LSF_RLIMIT_DATA] > 0) {
	if (useKb) {
	    submitReq->rLimits[LSF_RLIMIT_DATA]
		    = jobSubReq->rLimits[LSF_RLIMIT_DATA];
	} else {
	    submitReq->rLimits[LSF_RLIMIT_DATA]
		    = jobSubReq->rLimits[LSF_RLIMIT_DATA] * 1024;
	}
    }
    if (jobSubReq->rLimits[LSF_RLIMIT_STACK] > 0) {
/*        if (useKb) {
            submitReq->rLimits[LSF_RLIMIT_STACK]
                = jobSubReq->rLimits[LSF_RLIMIT_STACK];
        } else {
            submitReq->rLimits[LSF_RLIMIT_STACK]
                = jobSubReq->rLimits[LSF_RLIMIT_STACK] * 1024;
        }*/
        submitReq->rLimits[LSF_RLIMIT_STACK] = jobSubReq->rLimits[LSF_RLIMIT_STACK];
        for (m = 0; m <= lsbUnitForLimits; m++) {
            submitReq->rLimits[LSF_RLIMIT_STACK] *= 1024;
            if (submitReq->rLimits[LSF_RLIMIT_STACK] < 0) {
                lsberrno = LSBE_BAD_RLIMIT;
                return (-1);
            }
        }
    }
    if (jobSubReq->rLimits[LSF_RLIMIT_CORE] >= 0) {
/*        if (useKb) {
            submitReq->rLimits[LSF_RLIMIT_CORE]
               = jobSubReq->rLimits[LSF_RLIMIT_CORE];
        } else {
            submitReq->rLimits[LSF_RLIMIT_CORE]
               = jobSubReq->rLimits[LSF_RLIMIT_CORE] * 1024;
        }*/
        submitReq->rLimits[LSF_RLIMIT_CORE] = jobSubReq->rLimits[LSF_RLIMIT_CORE];
        for (m = 0; m <= lsbUnitForLimits; m++) {
            submitReq->rLimits[LSF_RLIMIT_CORE] *= 1024;
            if (submitReq->rLimits[LSF_RLIMIT_CORE] < 0) {
                lsberrno = LSBE_BAD_RLIMIT;
                return (-1);
            }
        }
    }
    if (jobSubReq->rLimits[LSF_RLIMIT_RSS] > 0) {
/*        if (useKb) {
            submitReq->rLimits[LSF_RLIMIT_RSS]
               = jobSubReq->rLimits[LSF_RLIMIT_RSS];
        } else {
            submitReq->rLimits[LSF_RLIMIT_RSS]
               = jobSubReq->rLimits[LSF_RLIMIT_RSS] * 1024;
        }*/
        submitReq->rLimits[LSF_RLIMIT_RSS] = jobSubReq->rLimits[LSF_RLIMIT_RSS];
        for (m = 0; m <= lsbUnitForLimits; m++) {
            submitReq->rLimits[LSF_RLIMIT_RSS] *= 1024;
            if (submitReq->rLimits[LSF_RLIMIT_RSS] < 0) {
                lsberrno = LSBE_BAD_RLIMIT;
                return (-1);
            }
        }
    }
    if (jobSubReq->rLimits[LSF_RLIMIT_SWAP] > 0) {
/*        if (useKb) {
            submitReq->rLimits[LSF_RLIMIT_SWAP]
                   = jobSubReq->rLimits[LSF_RLIMIT_SWAP];
        } else {
            submitReq->rLimits[LSF_RLIMIT_SWAP]
                   = jobSubReq->rLimits[LSF_RLIMIT_SWAP] * 1024;
        }*/
        submitReq->rLimits[LSF_RLIMIT_SWAP] = jobSubReq->rLimits[LSF_RLIMIT_SWAP];
        for (m = 0; m <= lsbUnitForLimits; m++) {
            submitReq->rLimits[LSF_RLIMIT_SWAP] *= 1024;
            if (submitReq->rLimits[LSF_RLIMIT_SWAP] < 0) {
                lsberrno = LSBE_BAD_RLIMIT;
                return (-1);
            }
        }
    }

    if (jobSubReq->rLimits[LSF_RLIMIT_PROCESS] > 0)
        submitReq->rLimits[LSF_RLIMIT_PROCESS]
               = jobSubReq->rLimits[LSF_RLIMIT_PROCESS];

    if ((jobSubReq->beginTime > 0 && jobSubReq->termTime > 0)
                              && (submitReq->beginTime > submitReq->termTime)) {
	lsberrno = LSBE_START_TIME;
	return (-1);
    }


    submitReq->submitTime = time(0);

    if (jobSubReq->options2 & SUB2_JOB_PRIORITY) {
	submitReq->userPriority = jobSubReq->userPriority;
    } else {
	submitReq->userPriority = -1;
    }

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Okay", fname);

    return(0);

}

static int
createJobInfoFile(struct submit *jobSubReq, struct lenData *jf)
{
    static char fname[] = "createJobInfoFile";
    char **ep;
    char *sp, num[MAX_LSB_NAME_LEN], *p, *oldp;
    int size = MSGSIZE, length = 0, len, len1, numEnv = 0, noEqual;

    int tsoptlen;

    tsoptlen = 0;

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    length = sizeof(CMDSTART) + sizeof(TRAPSIGCMD) + sizeof(WAITCLEANCMD) +
	sizeof(EXITCMD) + strlen(jobSubReq->command) + tsoptlen +
          sizeof(LSBNUMENV) +
	    sizeof(ENVSSTART) + sizeof(EDATASTART) + sizeof(SHELLLINE) + 1 +
		MAX_LSB_NAME_LEN * 2  +
		    ed.len;

    jf->len = 0;
    size = MAX(length,MSGSIZE);
    if ((jf->data = (char *) malloc(size)) == NULL) {
	lsberrno = LSBE_NO_MEM;
	return (-1);
    }
    jf->data[0] = '\0';
    strcat(jf->data, SHELLLINE);

    if (useracctmap) {
        strcat(jf->data, "LSB_ACCT_MAP='");
        strcat(jf->data, useracctmap);
	strcat(jf->data, "'; export LSB_ACCT_MAP\n");
        length += 14 + strlen(useracctmap) + 24;
        free(useracctmap);
        useracctmap=NULL;
    }

    strcat(jf->data, "VOLCLAVA_VERSION='");
    sprintf (num, "%d", VOLCLAVA_VERSION);
    strcat(jf->data, num);
    strcat(jf->data, "'; export VOLCLAVA_VERSION\n");
    length += 13 + strlen(num) + 23;

    for (ep = environ; *ep; ep++) {
        noEqual = FALSE;
        if (logclass & (LC_TRACE | LC_EXEC)) {
            ls_syslog(LOG_DEBUG, "%s: environment variable <%s>", fname, *ep);
        }

	if (!strncmp(*ep, "LSB_JOBID=", 10) ||
	    !strncmp(*ep, "LSB_HOSTS=", 10) ||
	    !strncmp(*ep, "LSB_QUEUE=", 10) ||
	    !strncmp(*ep, "LSB_JOBNAME=", 12) ||
	    !strncmp(*ep, "LSB_TRAPSIGS=", 13) ||
	    !strncmp(*ep, "LSB_JOBFILENAME=", 16) ||
	    !strncmp(*ep, "LSB_RESTART=", 12) ||
	    !strncmp(*ep, "LSB_EXIT_PRE_ABORT=", 19) ||
	    !strncmp(*ep, "LSB_EXIT_REQUEUE=", 17) ||
	    !strncmp(*ep, "LS_JOBPID=", 10) ||
	    !strncmp(*ep, "LSB_INTERACTIVE=", 16) ||
	    !strncmp(*ep, "LSB_ACCT_MAP=", 13) ||
	    !strncmp(*ep, "LSB_JOB_STARTER=", 16) ||
	    !strncmp(*ep, "LSB_EVENT_ATTRIB=", 17) ||
	    !strncmp(*ep, "VOLCLAVA_VERSION=", 12) ||
	    !strncmp(*ep, "LSB_SUB_", 8) ||


	    !strncmp(*ep, "HOME=", 5) ||
	    !strncmp(*ep, "PWD=", 4) ||
	    !strncmp(*ep, "USER=", 5)) {
	    continue;
	}

	if (!(jobSubReq->options & SUB_INTERACTIVE)) {

	    if (!strncmp(*ep, "TERMCAP=", 8) || !strncmp(*ep, "TERM=", 5))
	        continue;
	}

        sp = putstr_(*ep);
        oldp = sp;
	if (!sp) {
	   FREEUP(sp);
	   lsberrno = LSBE_NO_MEM;
	   return (-1);
	}

        if (strncmp(sp, "DISPLAY=", 8) == 0) {

            sp = chDisplay_(sp);
        }

	for (p = sp; *p != '\0' && *p != '='; p++);

        if (*p =='\0') {

            noEqual = TRUE;
            if (logclass & (LC_TRACE | LC_EXEC)) {
                ls_syslog (LOG_DEBUG, "%s: environment variable <%s> doesn't have '='", fname, sp);
            }
        } else {
	   *p = '\0';
        }

        if (noEqual == TRUE) {
            len1 = 2;
        } else {
            len1 = strlen(p+1) + 1;
        }
	if ((length += (len = strlen(sp) + len1
			+ sizeof(TAILCMD) + strlen(sp) + 1)) > size) {
	    char *newp = (char *) realloc(jf->data, (size += (len > MSGSIZE ?
							      len : MSGSIZE)));
	    if (newp == NULL) {
                if (noEqual != TRUE)
		    *p = '=';
		lsberrno = LSBE_NO_MEM;
		FREEUP(oldp);
		free(jf->data);
		return (-1);
	    }
	    jf->data = newp;
	}
	strcat(jf->data, sp);
	strcat(jf->data, "='");
        if (noEqual == TRUE) {
            strcat(jf->data, "\0");
        } else {
	    strcat(jf->data, p+1);
        }
	strcat(jf->data, TAILCMD);
	strcat(jf->data, sp);
	strcat(jf->data, "\n");
        if (noEqual != TRUE) {
	    *p = '=';
        }
        if (logclass & (LC_TRACE | LC_EXEC)) {
            ls_syslog (LOG_DEBUG, "%s:length=%d, size=%d, jf->len=%d, numEnv=%d",
                              fname, length, size, strlen(jf->data), ++numEnv);
        }
	FREEUP(oldp);
    }

    if ((length +=  (len = sizeof (TRAPSIGCMD) + sizeof (CMDSTART)
                + strlen (jobSubReq->command) + tsoptlen + sizeof (WAITCLEANCMD)
                + sizeof (EXITCMD) + sizeof (EDATASTART) + ed.len

                + MAX_LSB_NAME_LEN)) > size) {
        char *newp = (char *) realloc(jf->data,
                             (size += (len > MSGSIZE ? len : MSGSIZE)));
        if (newp == NULL) {
            lsberrno = LSBE_NO_MEM;
            FREEUP(jf->data);
            return (-1);
        }
        jf->data = newp;
    }
    if (logclass & (LC_TRACE | LC_EXEC)) {
        ls_syslog (LOG_DEBUG, "%s:length=%d, size=%d, jf->len=%d, numEnv=%d",
                              fname, length, size, strlen(jf->data), numEnv);
    }

    strcat(jf->data, TRAPSIGCMD);
    strcat(jf->data, CMDSTART);

    strcat(jf->data, jobSubReq->command);

    strcat(jf->data, WAITCLEANCMD);
    strcat(jf->data, EXITCMD);

    appendEData(jf, &ed);

    return (0);
}

void appendEData(struct lenData *jf, struct lenData *ed)
{
    char *sp, num[MAX_LSB_NAME_LEN];

    strcat(jf->data, EDATASTART);
    sprintf(num, "%d\n", ed->len);
    strcat(jf->data, num);

    sp = jf->data + strlen(jf->data) + 1;
    memcpy(sp, ed->data, ed->len);
    jf->len = strlen(jf->data) + 1 + ed->len;
}


static LS_LONG_INT
send_batch (struct submitReq *submitReqPtr, struct lenData *jf,
	    struct submitReply *submitReply, struct lsfAuth *auth)
{
    static char fname[] = "send_batch";
    mbdReqType mbdReqtype;
    XDR xdrs;
    char *request_buf;
    int reqBufSize;
    char *reply_buf;
    int cc;
    struct LSFHeader hdr;
    struct submitMbdReply *reply;
    LS_LONG_INT jobId;

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    reqBufSize = xdrSubReqSize(submitReqPtr);
    reqBufSize += xdr_lsfAuthSize(auth);
    if ((request_buf = (char *) malloc(reqBufSize)) == NULL) {
	if (logclass & LC_EXEC)
	    ls_syslog(LOG_DEBUG, "%s: request_buf malloc (%d) failed: %m",
		      fname, reqBufSize);
	lsberrno = LSBE_NO_MEM;
	return (-1);
    }



    mbdReqtype = BATCH_JOB_SUB;
    xdrmem_create(&xdrs, request_buf, reqBufSize, XDR_ENCODE);
    initLSFHeader_(&hdr);
    hdr.opCode = mbdReqtype;
    if (!xdr_encodeMsg(&xdrs, (char *) submitReqPtr, &hdr, xdr_submitReq, 0,
		       auth)) {
        xdr_destroy(&xdrs);
        lsberrno = LSBE_XDR;
	free(request_buf);
        return(-1);
    }

    if ( (cc = callmbd(NULL, request_buf, XDR_GETPOS(&xdrs), &reply_buf,
                       &hdr, NULL, sndJobFile_, (int *) jf)) < 0) {
	xdr_destroy(&xdrs);
        if (logclass & (LC_TRACE | LC_EXEC))
            ls_syslog(LOG_DEBUG, "%s: callmbd() failed; cc=%d", fname, cc);
	free(request_buf);
	return(-1);
    }
    xdr_destroy(&xdrs);
    free(request_buf);

    lsberrno = hdr.opCode;
    if (cc == 0) {
	submitReply->badJobId = 0;
	submitReply->badReqIndx = 0;
	submitReply->queue = "";
	submitReply->badJobName = "";
	submitReply->pendLimitReason = "";
	submitReply->subTryInterval = DEF_SUB_TRY_INTERVAL;
	return (-1);
    }

    if ((reply = (struct submitMbdReply *)malloc(sizeof(struct submitMbdReply)))
	== NULL) {
        lsberrno = LSBE_NO_MEM;
	free(reply);
	return(-1);
    }


    xdrmem_create(&xdrs, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);

    if (!xdr_submitMbdReply(&xdrs, reply, &hdr)) {
	lsberrno = LSBE_XDR;
	free(reply_buf);
	free(reply);
        xdr_destroy(&xdrs);
        return (-1);
    }

    free(reply_buf);

    xdr_destroy(&xdrs);



    submitReply->badJobId = reply->jobId;
    submitReply->badReqIndx = reply->badReqIndx;
    submitReply->queue = reply->queue;
    submitReply->badJobName = reply->badJobName;
    submitReply->subTryInterval = reply->subTryInterval;
    submitReply->pendLimitReason = reply->pendLimitReason;

    if (lsberrno == LSBE_NO_ERROR) {
	if (reply->jobId == 0)
	    lsberrno = LSBE_PROTOCOL;
        jobId = reply->jobId;
        free(reply);
        if (logclass & (LC_TRACE | LC_EXEC))
            ls_syslog(LOG_DEBUG1, "%s: mbd says job <%s> has been restarted",
                                  fname, lsb_jobid2str(jobId));
        return (jobId);
    }

    free(reply);
    return(-1);
}

static int
dependCondSyntax(char *dependCond)
{
    return (0);
}

static LS_LONG_INT
subRestart(struct submit  *jobSubReq, struct submitReq *submitReq,
               struct submitReply *submitRep, struct lsfAuth *auth)
{
    static char fname[] = "subRestart";
    struct lenData jf;
    struct jobNewLog *jobLog = NULL;
    struct xFile *xFiles = NULL;
    char resReq[MAXLINELEN], fromHost[MAXFILENAMELEN];
    char mailUser[MAX_LSB_NAME_LEN], projectName[MAX_LSB_NAME_LEN];
    char loginShell[MAX_LSB_NAME_LEN], schedHostType[MAX_LSB_NAME_LEN];
    int length, line;
    LS_LONG_INT jobId = -1;
    char *ptr;
    int childIoFd[2];
    pid_t pid;
    LS_WAIT_T status;
    struct {
        int error;
        int eno;
        int lserrno;
        int lsberrno;
    } err;
    char chkPath[MAXFILENAMELEN];
    struct linger   linstr = {1, 5};

    memset(chkPath,0,MAXFILENAMELEN);
    err.error = err.eno = err.lserrno = err.lsberrno = 0;

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);


    if (socketpair (AF_UNIX, SOCK_STREAM, 0, childIoFd) < 0) {
        lsberrno = LSBE_SYS_CALL;
        return (-1);
    }

    makeCleanToRunEsub();

    pid = fork();
    if (pid < 0) {
        lsberrno = LSBE_SYS_CALL;
        return (-1);
    } else if (pid == 0) {


	uid_t uid;
        char chklog[MAXFILENAMELEN];
        FILE *fp;
        struct eventRec *logPtr;
        int exitVal = -1;

        close (childIoFd[0]);

    	setsockopt(childIoFd[1], SOL_SOCKET, SO_LINGER, (char *)&linstr,
		sizeof(linstr));

        uid = getuid();
        if (setuid (uid) < 0) {
            lsberrno = LSBE_BAD_USER;
            goto sendErr;
        }

        if ( getChkDir(jobSubReq->command, chkPath) == -1 ) {
            lsberrno = LSBE_BAD_CHKLOG;
        } else
            sprintf(chklog, "%s/chklog", chkPath);


        if (write(childIoFd[1], (char *) chkPath, sizeof(chkPath)) != sizeof(chkPath)) {
        goto childExit;
        }

	if (lsberrno == LSBE_BAD_CHKLOG) goto sendErr;

        if (logclass & (LC_TRACE | LC_EXEC))
            ls_syslog(LOG_DEBUG1, "%s: Child tries to open chklog file <%s>",
                                 fname, chklog);
        if ((fp = fopen(chklog, "r")) == NULL) {
            lsberrno = LSBE_BAD_CHKLOG;
            goto sendErr;
        }
        if ((logPtr = lsb_geteventrec(fp, &line)) == NULL
            || logPtr->type != EVENT_JOB_NEW) {
            lsberrno = LSBE_BAD_CHKLOG;
            fclose(fp);
            goto sendErr;
        }
        jobLog = &logPtr->eventLog.jobNewLog;

        if (logclass & (LC_TRACE | LC_EXEC))
            ls_syslog(LOG_DEBUG1, "%s: Child got job log from chklog file",
                                  fname);

        err.error = FALSE;
        if (write(childIoFd[1], (char *) &err, sizeof(err)) != sizeof(err)) {
            goto childExit;
        }

	if (write(childIoFd[1], (char *) &ed.len, sizeof(ed.len))
	                                        != sizeof(ed.len)) {
	    goto childExit;
	}

	if (ed.len > 0) {
	    if (write(childIoFd[1], (char *) ed.data, ed.len) != ed.len) {
	        goto childExit;
	    }
	}


        if (b_write_fix(childIoFd[1], (char *) jobLog, sizeof(struct jobNewLog))
                                              != sizeof(struct jobNewLog)) {
            goto childExit;
        }
        if ((length = strlen (jobLog->resReq) + 1) >= MAXLINELEN) {
            goto childExit;
        }
        if (write(childIoFd[1], (char *) &length, sizeof(length))
                                               != sizeof(length)) {
            goto childExit;
        }
        if (write(childIoFd[1], jobLog->resReq, length) != length) {
            goto childExit;
        }
        if ((jobLog->options & SUB_OTHER_FILES) && jobLog->nxf > 0) {
            length = jobLog->nxf * sizeof(struct xFile);
            if (write(childIoFd[1], (char *) jobLog->xf, length) != length) {
                goto childExit;
            }
        }
	if (jobLog->options & SUB_MAIL_USER) {
	    if ((length = strlen (jobLog->mailUser) +1) >= MAX_LSB_NAME_LEN)
		goto childExit;
            if (write(childIoFd[1], (char *) &length, sizeof(length))
                                               != sizeof(length))
                goto childExit;
            if (write(childIoFd[1], jobLog->mailUser, length) != length)
		goto childExit;
        }

	if (jobLog->options & SUB_PROJECT_NAME) {
	    if ((length = strlen (jobLog->projectName) +1) >= MAX_LSB_NAME_LEN)
		goto childExit;
            if (write(childIoFd[1], (char *) &length, sizeof(length))
                                               != sizeof(length))
                goto childExit;
            if (write(childIoFd[1], jobLog->projectName, length) != length)
		goto childExit;
        }

	if (jobLog->options & SUB_LOGIN_SHELL) {
	    if ((length = strlen (jobLog->loginShell) +1) >= MAX_LSB_NAME_LEN)
		goto childExit;
            if (write(childIoFd[1], (char *) &length, sizeof(length))
                                               != sizeof(length))
                goto childExit;
            if (write(childIoFd[1], jobLog->loginShell, length) != length)
		goto childExit;
        }

	if ((length = strlen (jobLog->schedHostType) +1) >= MAX_LSB_NAME_LEN)
		goto childExit;
        if (write(childIoFd[1], (char *) &length, sizeof(length))
                                               != sizeof(length))
            goto childExit;
        if (write(childIoFd[1], jobLog->schedHostType, length) != length)
            goto childExit;


        if ((logPtr = lsb_geteventrec(fp, &line)) == NULL
            || logPtr->type != EVENT_JOB_START) {
            goto childExit;
        }
        length = sizeof (logPtr->eventLog.jobStartLog.jobPGid);
        if (write(childIoFd[1],
		  (char *) &logPtr->eventLog.jobStartLog.jobPGid,
		  length) != length) {
            goto childExit;
        }
        if (logPtr->eventLog.jobStartLog.numExHosts <= 0) {
            goto childExit;
        }
        length = strlen (logPtr->eventLog.jobStartLog.execHosts[0]) + 1;
        if (length >= MAXHOSTNAMELEN) {
            goto childExit;
        }
        if (write(childIoFd[1], (char *) &length, sizeof(length))
                                               != sizeof(length)) {
            goto childExit;
        }
        if (write(childIoFd[1], logPtr->eventLog.jobStartLog.execHosts[0],
                                           length) != length) {
            goto childExit;
        }
        exitVal = 0;

childExit:
        if (logclass & (LC_TRACE | LC_EXEC)) {
            if (exitVal == 0)
                ls_syslog(LOG_DEBUG1, "%s: Child succeeded in sending messages to parent", fname);
            else
                ls_syslog(LOG_DEBUG, "%s: Child failed in sending messages to parent", fname);
        }
        fclose(fp);
        close(childIoFd[1]);
        exit(exitVal);

sendErr:
        err.error = TRUE;
        err.eno = errno;
        err.lserrno = lserrno;
        err.lsberrno = lsberrno;

        if (write(childIoFd[1], (char *) &err, sizeof(err)) != sizeof(err)) {
            close(childIoFd[1]);
            exit(-1);
        }
        close(childIoFd[1]);
        exit(0);
    }



    close (childIoFd[1]);


    if (read(childIoFd[0], (char *) chkPath, sizeof(chkPath)) != sizeof(chkPath)) {
        goto parentErr;
    }



    err.error = FALSE;
    if (read(childIoFd[0], (char *) &err, sizeof(err)) != sizeof(err)) {
        goto parentErr;
    }
    if (err.error) {
        errno = err.eno;
        lserrno = err.lserrno;
        lsberrno = err.lsberrno;
        goto parentErr;
    }


    if (read(childIoFd[0], (char *) &ed.len, sizeof(ed.len)) !=
						    sizeof(ed.len)) {
	goto parentErr;
    }
    if (ed.len > 0) {
	if ((ed.data = (char *) malloc(ed.len)) == NULL)
            goto parentErr;

        if (b_read_fix(childIoFd[0], ed.data, ed.len) != ed.len) {
	    FREEUP(ed.data);
	    ed.len = 0;
	    goto parentErr;
        }
    } else
        ed.data = NULL;


    if ((jobLog = (struct jobNewLog *) malloc (sizeof (struct jobNewLog)))
            == NULL) {
        goto parentErr;
    }
    if (b_read_fix(childIoFd[0], (char *) jobLog, sizeof(struct jobNewLog))
                                         != sizeof(struct jobNewLog)) {
        goto parentErr;
    }
    if (read(childIoFd[0], (char *) &length, sizeof(length))
                                          != sizeof(length)) {
        goto parentErr;
    }
    jobLog->resReq = resReq;
    if (read(childIoFd[0], jobLog->resReq, length) != length) {
        goto parentErr;
    }
    if ((jobLog->options & SUB_OTHER_FILES) && jobLog->nxf > 0) {
        length = jobLog->nxf * sizeof(struct xFile);
        if ((xFiles = (struct xFile *) malloc (length)) == NULL) {
            goto parentErr;
        }
        jobLog->xf = xFiles;
        if (read(childIoFd[0], (char *) jobLog->xf, length) != length) {
            free(jobLog->xf);
            goto parentErr;
        }
    }

    if (jobLog->options & SUB_MAIL_USER) {
        if (read(childIoFd[0], (char *) &length, sizeof(length))
                                          != sizeof(length))
            goto parentErr;
        jobLog->mailUser = mailUser;
        if (read(childIoFd[0], jobLog->mailUser, length) != length)
            goto parentErr;
    }

    if (jobLog->options & SUB_PROJECT_NAME) {
        if (read(childIoFd[0], (char *) &length, sizeof(length))
                                          != sizeof(length))
            goto parentErr;
        jobLog->projectName = projectName;
        if (read(childIoFd[0], jobLog->projectName, length) != length)
            goto parentErr;
    }


    if (jobLog->options & SUB_LOGIN_SHELL) {
        if (read(childIoFd[0], (char *) &length, sizeof(length))
                                          != sizeof(length))
            goto parentErr;
        jobLog->loginShell = loginShell;
        if (read(childIoFd[0], jobLog->loginShell, length) != length)
            goto parentErr;
    }
    if (read(childIoFd[0], (char *) &length, sizeof(length))
                                          != sizeof(length))
        goto parentErr;
    jobLog->schedHostType = schedHostType;
    if (read(childIoFd[0], jobLog->schedHostType, length) != length)
         goto parentErr;


    length = sizeof (submitReq->restartPid);
    if (read(childIoFd[0], (char *) &submitReq->restartPid, length)
                                                         != length) {
        goto parentErr;
    }
    if (read(childIoFd[0], (char *) &length, sizeof(length))
                                          != sizeof(length)) {
        goto parentErr;
    }
    submitReq->fromHost = fromHost;
    if (read(childIoFd[0], submitReq->fromHost, length) != length) {
        goto parentErr;
    }

    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG1, "%s: Parent got the job log from child",
                                 fname);



    lsberrno = LSBE_BAD_CHKLOG;



    if (strlen(jobLog->jobName) >= MAX_CMD_DESC_LEN)
        goto parentErr;

    if ((ptr = strchr(jobLog->jobName, '[')) != NULL
        && strrchr(jobSubReq->command, '/')) {

        char element[10];
        *ptr = '\0';
        ptr = strrchr(jobSubReq->command, '/');
	ptr++;
        if (islongint_(ptr)){

            sprintf(element, "[%d]", LSB_ARRAY_IDX(atoi64_(ptr)));
        }
        strcat(jobLog->jobName, element);
    }

    if ((ptr = strchr(jobLog->jobName, '/')) != NULL) {
        if (ptr == strrchr(jobLog->jobName, '/'))

            sprintf(jobLog->jobName, "%s", ++ptr);
    }

    submitReq->jobName = jobLog->jobName;
    submitReq->options |= SUB_JOB_NAME;

    if (!(jobSubReq->options & SUB_QUEUE)) {
        if (strlen(jobLog->queue) >= MAX_LSB_NAME_LEN - 1)
            goto parentErr;
        submitReq->queue = jobLog->queue;
        submitReq->options |= SUB_QUEUE;
    }

    if (strlen(jobLog->resReq) >= MAXLINELEN - 1)
        goto parentErr;
    submitReq->resReq = jobLog->resReq;
    if (jobLog->options & SUB_RES_REQ)
        submitReq->options |= SUB_RES_REQ;

    if (jobLog->numProcessors < 0)
        goto parentErr;
    submitReq->numProcessors = jobLog->numProcessors;

    if (jobLog->maxNumProcessors < 0)
        goto parentErr;
    submitReq->maxNumProcessors = jobLog->maxNumProcessors;

    if (strlen(jobLog->inFile) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->inFile = jobLog->inFile;
    if (jobLog->options & SUB_IN_FILE)
        submitReq->options |= SUB_IN_FILE;

    if (strlen(jobLog->outFile) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->outFile = jobLog->outFile;
    if (jobLog->options & SUB_OUT_FILE)
        submitReq->options |= SUB_OUT_FILE;

    if (strlen(jobLog->errFile) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->errFile = jobLog->errFile;
    if (jobLog->options & SUB_ERR_FILE)
        submitReq->options |= SUB_ERR_FILE;

    if (strlen(jobLog->inFileSpool) >= MAXFILENAMELEN)
        goto parentErr;
    submitReq->inFileSpool = jobLog->inFileSpool;
    if (jobLog->options2 & SUB2_IN_FILE_SPOOL)
        submitReq->options2 |= SUB2_IN_FILE_SPOOL;

    if (strlen(jobLog->commandSpool) >= MAXFILENAMELEN)
        goto parentErr;
    submitReq->commandSpool = jobLog->commandSpool;
    if (jobLog->options2 & SUB2_JOB_CMD_SPOOL)
        submitReq->options2 |= SUB2_JOB_CMD_SPOOL;


    if (strlen(jobLog->command) >= MAX_CMD_DESC_LEN)
        goto parentErr;
    submitReq->command = jobLog->command;

    if (jobLog->userPriority > 0) {
	submitReq->userPriority = jobLog->userPriority;
    }

    submitReq->chkpntPeriod = jobLog->chkpntPeriod;
    if (jobLog->options & SUB_CHKPNT_PERIOD)
        submitReq->options |= SUB_CHKPNT_PERIOD;

    submitReq->options |= SUB_CHKPNT_DIR;
    if (strlen(jobLog->chkpntDir) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->chkpntDir = chkPath;

    if (strlen(jobLog->jobFile) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->jobFile = jobLog->jobFile;

    submitReq->umask = jobLog->umask;

    if (strlen(jobLog->cwd) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->cwd = jobLog->cwd;

    if (strlen(jobLog->subHomeDir) >= MAXFILENAMELEN - 1)
        goto parentErr;
    submitReq->subHomeDir = jobLog->subHomeDir;

    submitReq->sigValue = (jobLog->options & SUB_WINDOW_SIG)
        ? sig_encode(jobLog->sigValue) : 0;
    if (submitReq->sigValue > 31 || submitReq->sigValue < 0)
        goto parentErr;

    if (jobLog->options & SUB_MAIL_USER) {
        submitReq->mailUser = jobLog->mailUser;
        submitReq->options |= SUB_MAIL_USER;
    } else
	submitReq->mailUser ="";

    if (jobLog->options & SUB_PROJECT_NAME) {
        submitReq->projectName = jobLog->projectName;
	submitReq->options |= SUB_PROJECT_NAME;
    } else
	submitReq->projectName = "";

    if (jobLog->options & SUB_LOGIN_SHELL) {
        submitReq->loginShell = jobLog->loginShell;
	submitReq->options |= SUB_LOGIN_SHELL;
    } else
	submitReq->loginShell = "";


    if (jobLog->options & SUB_RERUNNABLE)
        submitReq->options |= SUB_RERUNNABLE;

    if (jobLog->options2 & SUB2_HOLD)
        submitReq->options2 |= SUB2_HOLD;

    if ((jobLog->options & SUB_OTHER_FILES) && jobLog->nxf > 0) {
        submitReq->options |= SUB_OTHER_FILES;
        submitReq->nxf = jobLog->nxf;
        if (jobLog->nxf <= 0)
            goto parentErr;
        submitReq->xf = jobLog->xf;
    } else {
        submitReq->nxf = 0;
    }
    if (jobLog->schedHostType)
	submitReq->schedHostType = jobLog->schedHostType;
    else
	submitReq->schedHostType = "";

    close(childIoFd[0]);
    if (waitpid (pid, &status, 0) < 0) {
        lsberrno = LSBE_SYS_CALL;
        goto leave;
    }
    if (createJobInfoFile(jobSubReq, &jf) == -1)
	goto leave;

    jobId = send_batch(submitReq, &jf, submitRep, auth);

    if (jobId > 0) {
        if (!getenv("BSUB_QUIET"))
            postSubMsg(jobSubReq, jobId, submitRep);
        }

leave:

    if (jobLog)
        free(jobLog);
    if (xFiles)
        free(xFiles);
    return (jobId);

parentErr:
    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Parent failed in receiving messages from child", fname);
    if (!err.error) {
        lsberrno = LSBE_SYS_CALL;
        err.eno = errno;
    }
    close(childIoFd[0]);
    if (waitpid (pid, &status, 0) < 0)
        lsberrno = LSBE_SYS_CALL;
    else
        errno = err.eno;
    jobId = -1;
    goto leave;

}

int
getChkDir(char *givenDir, char *chkPath)
{
    char *strPtr;

    if (((strPtr = strrchr(givenDir, '/')) != NULL)
        && (islongint_(strPtr+1))) {
        sprintf(chkPath, "%s", givenDir);
        return(0);
    } else {
           DIR *dirp;
           struct dirent *dp;
           char jobIdDir[MAXFILENAMELEN];
           int i;

           i = 0;
           if ((dirp = opendir(givenDir)) == NULL)
               return (-1);
           while ((dp = readdir(dirp)) != NULL) {

               if (strcpy(jobIdDir, dp->d_name) == NULL)
                   i++;
               if (i > 1) {
                   (void) closedir(dirp);
                   return (-1);
               }
           }
           if (islongint_(jobIdDir)) {
               sprintf(chkPath, "%s/%s", givenDir, jobIdDir);
               (void) closedir(dirp);
               return (0);
           } else {
               (void) closedir(dirp);
               return (-1);
           }
    }

}


static LS_LONG_INT
subJob(struct submit  *jobSubReq, struct submitReq *submitReq,
	   struct submitReply *submitRep, struct lsfAuth *auth)
{

    char homeDir[MAXFILENAMELEN];
    char resReq[MAXLINELEN];
    char cmd[MAXLINELEN];
    struct lenData jf;
    int niosSock;
    LSB_SUB_SPOOL_FILE_T subSpoolFiles;
    LS_LONG_INT jobId = -1;

    subSpoolFiles.inFileSpool[0] = 0;
    subSpoolFiles.commandSpool[0] = 0;

    submitReq->subHomeDir = homeDir;
    submitReq->resReq = resReq;
    submitReq->command = cmd;

    if (getOtherParams (jobSubReq, submitReq, submitRep, auth,
                                               &subSpoolFiles) < 0) {
        goto cleanup;
    }

    if (createJobInfoFile(jobSubReq, &jf) == -1) {
        goto cleanup;
    }

    if (submitReq->options & SUB_INTERACTIVE) {
	if (submitReq->options & SUB_PTY) {
	    if (!isatty(0) && !isatty(1))
		submitReq->options &= ~SUB_PTY;
	}
	if ((niosSock = createNiosSock(submitReq)) < 0) {
            goto cleanup;
	}
    }

    if (submitReq->options2 & SUB2_BSUB_BLOCK) {
        if ((niosSock = createNiosSock(submitReq)) < 0) {
            goto cleanup;
        }
    }

    jobId = send_batch(submitReq, &jf, submitRep, auth);
    free(jf.data);

    if (jobId > 0) {

	if (submitReq->options & SUB_INTERACTIVE) {
	    sigset_t sigMask;
	    sigemptyset(&sigMask);
	    sigaddset(&sigMask, SIGINT);
	    sigprocmask(SIG_BLOCK, &sigMask, NULL);

	}

	if (!getenv("BSUB_QUIET"))
	    postSubMsg(jobSubReq, jobId, submitRep);

	if (submitReq->options & SUB_INTERACTIVE) {
	    int jobIdFd, pid;
	    char *envBuf;
	    if ((envBuf = getenv("JOBID_FD")) != NULL) {
		jobIdFd = atoi(envBuf);
		if (b_write_fix(jobIdFd, (char *) &jobId, sizeof(jobId))
		    != sizeof(jobId)) {
                    goto cleanup;
                }
		if (b_read_fix(jobIdFd, (char *) &pid, sizeof(pid))
		    != sizeof(pid)) {
                    goto cleanup;
                }
		close(jobIdFd);
	    }

	    startNios(submitReq, niosSock, jobId);

	    exit(-1);
	}

	if (submitReq->options2 & SUB2_BSUB_BLOCK) {
	    startNios(submitReq, niosSock, jobId);

	    exit(-1);
	}
    }

cleanup:

    if (jobId < 0) {

	const char* spoolHost;
	int err;


	if (subSpoolFiles.inFileSpool[0]) {
	    spoolHost = getSpoolHostBySpoolFile(subSpoolFiles.inFileSpool);
	    err = chUserRemoveSpoolFile(spoolHost, subSpoolFiles.inFileSpool);
        if (err) {
            fprintf(stderr,
                    (_i18n_msg_get(ls_catd,NL_SETN,442, "Submission failed, and the spooled file <%s> can not be removed on host <%s>, please manually remove it")), /* catgets 442 */
                    subSpoolFiles.inFileSpool, spoolHost);
	    }
	}


	if (subSpoolFiles.commandSpool[0]) {
	    spoolHost = getSpoolHostBySpoolFile(subSpoolFiles.commandSpool);
	    err = chUserRemoveSpoolFile(spoolHost, subSpoolFiles.commandSpool);
        if (err) {
            fprintf(stderr,
                    (_i18n_msg_get(ls_catd,NL_SETN,442, "Submission failed, and the spooled file <%s> can not be removed on host <%s>, please manually remove it")), /* catgets 442 */
                    subSpoolFiles.commandSpool, spoolHost);
	    }
	}
    }

    return (jobId);
}



static const char*
getDefaultSpoolDir()
{
    static char   spoolDir[MAXFILENAMELEN];
    char * clusterName;

    clusterName = ls_getclustername();
    if (clusterName == NULL) {
	return NULL;
    }

    sprintf(spoolDir, "%s/%s", lsbParams[LSB_SHAREDIR].paramValue,
			       clusterName);

    return spoolDir;
}


static const LSB_SPOOL_INFO_T *
chUserCopySpoolFile(const char * srcFile, spoolOptions_t fileType)
{
    static LSB_SPOOL_INFO_T parentSpoolInfo;
    pid_t pid;
    LS_WAIT_T status;
    int exitVal, cc;
    int rcpp[2];


    if (defaultSpoolDir == NULL) {
        defaultSpoolDir = getDefaultSpoolDir();
    }

    memset(&parentSpoolInfo, 0, sizeof(parentSpoolInfo));

    if (pipe(rcpp) < 0) {
        perror("chUserCopySpoolFile:pipe");
        lsberrno = LSBE_SYS_CALL;
	return NULL;
    }

    switch(pid = fork()) {
        case 0:

            close(rcpp[0]);

	    exitVal = -1;

	    {
                const LSB_SPOOL_INFO_T *childSpoolInfo;

                setuid( getuid() );
                childSpoolInfo = copySpoolFile(srcFile, fileType);
                if (childSpoolInfo) {
                    memcpy(&parentSpoolInfo, childSpoolInfo,
                            sizeof(parentSpoolInfo));
		    exitVal = 0;
                }
            }

            cc = write(rcpp[1], &parentSpoolInfo,
                             sizeof(parentSpoolInfo));
	    if (cc < sizeof(parentSpoolInfo)) {
	        exitVal = -1;
            }
            close(rcpp[1]);

            exit (exitVal);

        case -1:

            close(rcpp[0]);
            close(rcpp[1]);
            perror("chUserCopySpoolFile:fork");
            lsberrno = LSBE_SYS_CALL;
	    return NULL;

        default:

            close(rcpp[1]);

            cc = read(rcpp[0], &parentSpoolInfo,
                            sizeof(parentSpoolInfo));
	    if (cc < sizeof(parentSpoolInfo)) {
                perror("chUserCopySpoolFile:read");
                lsberrno = LSBE_SYS_CALL;
		return NULL;
            }
            close(rcpp[0]);

            if (waitpid(pid, &status, 0) < 0 ) {
                perror("chUserCopySpoolFile:waitpid");
                lsberrno = LSBE_SYS_CALL;
		return NULL;
            }


            if (WIFEXITED(status) == 0) {
                fprintf(stderr, I18N(443, "Child process killed by signal.\n")); /* catgets 443 */
                lsberrno = LSBE_SYS_CALL;
		return NULL;
            }
            else {
                if (WEXITSTATUS(status) == 0xff) {
                    lsberrno = LSBE_SYS_CALL;
		    return NULL;
                }
            }

            break;
    }

    return &parentSpoolInfo;
}


int
chUserRemoveSpoolFile( const char * hostName, const char * spoolFile)
{
    pid_t pid;
    LS_WAIT_T status;
    const char *sp;
    char dirName[MAXFILENAMELEN];

    sp = getLowestDir_(spoolFile);
    if (sp) {
        strcpy(dirName, sp);
    } else {
        strcpy(dirName, spoolFile);
    }

    switch(pid = fork()) {
        case 0:


            setuid( getuid() );
            if ( removeSpoolFile( hostName, dirName ) == 0 ) {
                exit( 0 );
            } else {
                exit( -1);
            }
            break;

        case -1:

            perror("chUserRemoveSpoolFile:fork");
            lsberrno = LSBE_SYS_CALL;
            return -1;

        default:


            if (waitpid(pid, &status, 0) < 0 ) {
                perror("chUserRemoveSpoolFile:waitpid");
                lsberrno = LSBE_SYS_CALL;
                return -1;
            }


            if (WIFEXITED(status) == 0) {
                fprintf(stderr, I18N(443, "Child process killed by signal.\n")); /* catgets 443 */
                lsberrno = LSBE_SYS_CALL;
                return -1;
            }
            else {
                if (WEXITSTATUS(status) == 0xff) {
                    lsberrno = LSBE_SYS_CALL;
                    return -1;
                }
            }

    }

    return 0;
}


int
getOtherParams (struct submit  *jobSubReq, struct submitReq *submitReq,
                struct submitReply *submitRep, struct lsfAuth *auth,
		LSB_SUB_SPOOL_FILE_T* subSpoolFiles)
{
    char *jobdesp, *sp, jobdespBuf[MAX_CMD_DESC_LEN];
    char lineStrBuf[MAXLINELEN], lastNonSpaceChar;
    int i, lastNonSpaceIdx;
    char *myHostName;
    struct passwd *pw;
    int jobSubReqCmd1Offset;

    if (jobSubReq->options & SUB_JOB_NAME) {
        if (!jobSubReq->jobName) {
	    lsberrno = LSBE_BAD_JOB;
	    return (-1);
	}


        if (strlen(jobSubReq->jobName) >= MAX_CMD_DESC_LEN - 1) {
            lsberrno = LSBE_BAD_JOB;
	    return (-1);
        } else
            submitReq->jobName = jobSubReq->jobName;
    } else
        submitReq->jobName = "";

    if (jobSubReq->options & SUB_IN_FILE) {
        if (!jobSubReq->inFile) {
	    lsberrno = LSBE_BAD_ARG;
	    return (-1);
	}

        if (strlen(jobSubReq->inFile) >= MAXFILENAMELEN - 1) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}
	submitReq->inFile = jobSubReq->inFile;
        submitReq->inFileSpool = "";
    } else if (jobSubReq->options2 & SUB2_IN_FILE_SPOOL) {

	const char* pSpoolFile;
	int spoolFileLen;
	const LSB_SPOOL_INFO_T *spoolInfo;


	spoolInfo = chUserCopySpoolFile(jobSubReq->inFile, SPOOL_INPUT_FILE);
	if (spoolInfo == NULL) {
            return (-1);
	}


	pSpoolFile = spoolInfo->spoolFile;
	spoolFileLen = strlen(pSpoolFile);
	if (spoolFileLen >= MAXFILENAMELEN) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}
        memcpy(subSpoolFiles->inFileSpool, pSpoolFile, spoolFileLen + 1);
	submitReq->inFileSpool = subSpoolFiles->inFileSpool;
	submitReq->inFile = jobSubReq->inFile;
    } else {
        submitReq->inFileSpool = "";
        submitReq->inFile = "";
    }


    if (jobSubReq->options & SUB_MAIL_USER) {
        if (!jobSubReq->mailUser) {
	    lsberrno = LSBE_BAD_ARG;
	    return(-1);
        }
	if (strlen(jobSubReq->mailUser) >= MAXHOSTNAMELEN -1) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}
        submitReq->mailUser = jobSubReq->mailUser;
    } else
	submitReq->mailUser = "";

    if (jobSubReq->options & SUB_PROJECT_NAME) {
	if (!jobSubReq->projectName) {
	    lsberrno = LSBE_BAD_ARG;
	    return(-1);
        }
	if (strlen(jobSubReq->projectName) >= MAX_LSB_NAME_LEN - 1) {
	    lsberrno = LSBE_BAD_ARG;
	    errno = ENAMETOOLONG;
	    return(-1);
        }
	submitReq->projectName = jobSubReq->projectName;
    } else
	submitReq->projectName = "";

    if (jobSubReq->options & SUB_OUT_FILE) {
        if (!jobSubReq->outFile) {
	    lsberrno = LSBE_BAD_ARG;
	    return (-1);
	}

        if (strlen(jobSubReq->outFile) >= MAXFILENAMELEN - 1) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}
	submitReq->outFile = jobSubReq->outFile;
    } else
         submitReq->outFile = "";

    if (jobSubReq->options & SUB_ERR_FILE) {
        if (!jobSubReq->errFile) {
	    lsberrno = LSBE_BAD_ARG;
	    return (-1);
	}

        if (strlen(jobSubReq->errFile) >= MAXFILENAMELEN - 1) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}
	submitReq->errFile = jobSubReq->errFile;
    } else
        submitReq->errFile = "";

    if (jobSubReq->options & SUB_CHKPNT_PERIOD) {
	if (!(jobSubReq->options & SUB_CHKPNTABLE))
	    return (-1);

        if (jobSubReq->chkpntPeriod < 0)
	    return (-1);
        else
            submitReq->chkpntPeriod = jobSubReq->chkpntPeriod;
    } else
        submitReq->chkpntPeriod = 0;

    if (jobSubReq->options & SUB_CHKPNT_DIR) {
        if (!jobSubReq->chkpntDir) {
	    lsberrno = LSBE_BAD_ARG;
	    return (-1);
	}

        if (strlen(jobSubReq->chkpntDir) >= MAXFILENAMELEN - 1) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}

	submitReq->chkpntDir = jobSubReq->chkpntDir;
	submitReq->options |= SUB_CHKPNTABLE;
    } else
        submitReq->chkpntDir = "";

    if (jobSubReq->numProcessors < 0
        || jobSubReq->maxNumProcessors < jobSubReq->numProcessors) {
	lsberrno = LSBE_BAD_ARG;
	return (-1);
    }

    if (jobSubReq->numProcessors == 0 && jobSubReq->maxNumProcessors == 0) {

	jobSubReq->options2 |= SUB2_USE_DEF_PROCLIMIT;
	submitReq->options2 |= SUB2_USE_DEF_PROCLIMIT;
    }

    if (jobSubReq->numProcessors != DEFAULT_NUMPRO
		  && jobSubReq->maxNumProcessors != DEFAULT_NUMPRO) {
        submitReq->numProcessors = (jobSubReq->numProcessors)
        	? jobSubReq->numProcessors : 1;
        submitReq->maxNumProcessors = (jobSubReq->maxNumProcessors)
	    ? jobSubReq->maxNumProcessors : 1;
    } else {
	submitReq->numProcessors = DEFAULT_NUMPRO;
	submitReq->maxNumProcessors = DEFAULT_NUMPRO;
    }

    if (jobSubReq->options & SUB_LOGIN_SHELL) {
	if (!jobSubReq->loginShell) {
	    lsberrno = LSBE_BAD_ARG;
	    return(-1);
        }
	if (strlen(jobSubReq->loginShell) >= MAX_LSB_NAME_LEN - 1) {
	    lsberrno = LSBE_BAD_ARG;
	    errno = ENAMETOOLONG;
	    return(-1);
        }
	submitReq->loginShell = jobSubReq->loginShell;
    } else
	submitReq->loginShell = "";


    submitReq->schedHostType = "";


    submitReq->restartPid = 0;
    submitReq->jobFile = "";

    if (jobSubReq->command == (char *)NULL ) {
	lsberrno = LSBE_BAD_CMD;
	return (-1);
    }

    lineStrBuf[0] = '\0';
    jobdesp = lineStrBuf;
    strncat(lineStrBuf, jobSubReq->command, MAX_CMD_DESC_LEN - 1);

    if ((sp = strstr(jobdesp, "SCRIPT_\n")) != NULL) {

        jobSubReqCmd1Offset = sp + strlen("SCRIPT_\n") - jobdesp;
        jobdesp += jobSubReqCmd1Offset;
        if ((sp = strstr(jobdesp, "SCRIPT_\n")) != NULL) {
           while (*sp != '\n') --sp;
           *sp = '\0';
	}
    } else {
        jobSubReqCmd1Offset = 0;
    }
    jobdespBuf[0] = '\0';
    strncat(jobdespBuf, jobdesp, MAX_CMD_DESC_LEN - 1);
    jobdesp = jobdespBuf;


    for (lastNonSpaceIdx = 0, lastNonSpaceChar = ';', i = 0;
	 jobdesp[i] != '\0'; i++) {
	if (jobdesp[i] == '\n') {
	    if (lastNonSpaceChar != ';' && lastNonSpaceChar != '&')
		jobdesp[i] = ';';
	    else
		jobdesp[i] = ' ';
	}
	if (jobdesp[i] != ' ' && jobdesp[i] != '\t') {
	    lastNonSpaceChar = jobdesp[i];
	    lastNonSpaceIdx = i;
	}
    }
    if (jobdesp[lastNonSpaceIdx] == ';')
	jobdesp[lastNonSpaceIdx] = '\0';

    for (i = 0; jobSubReq->command[i] != '\0'; i++) {

	if (jobSubReq->command[i] == ';' || jobSubReq->command[i] == ' ' ||
	    jobSubReq->command[i] == '&' || jobSubReq->command[i] == '>' ||
	    jobSubReq->command[i] == '<' || jobSubReq->command[i] == '|' ||
	    jobSubReq->command[i] == '\t' || jobSubReq->command[i] == '\n')
	    break;
    }
    if ((i == 0) && (jobSubReq->command[0] != ' ')) {
	lsberrno = LSBE_BAD_CMD;
	return (-1);
    }


    strcpy (submitReq->command, jobdesp);


    if (jobSubReq->options2 & SUB2_MODIFY_CMD) {
        for (i = 0; jobSubReq->newCommand[i] != '\0'; i++) {

	    if (jobSubReq->newCommand[i] == ';' ||
		jobSubReq->newCommand[i] == ' ' ||
	        jobSubReq->newCommand[i] == '&' ||
		jobSubReq->newCommand[i] == '>' ||
	        jobSubReq->newCommand[i] == '<' ||
		jobSubReq->newCommand[i] == '|' ||
	        jobSubReq->newCommand[i] == '\t' ||
		jobSubReq->newCommand[i] == '\n')
	        break;
        }
        if ((i == 0) && (jobSubReq->command[0] != ' ')) {
    	    lsberrno = LSBE_BAD_CMD;
	    return (-1);
        }
    }


    if (jobSubReq->options2 & SUB2_JOB_CMD_SPOOL) {

	const char* pSpoolCmd;
	int spoolCmdLen;
	const char* pSrcCmd;
	int srcCmdLen;
	int ii, jj;
        const LSB_SPOOL_INFO_T* spoolInfo;
        char* sp;


	if (jobSubReq->options & SUB_MODIFY) {

	    strcpy(submitReq->command, jobSubReq->newCommand);


	    pSrcCmd = getCmdPathName_(jobSubReq->newCommand, &srcCmdLen);
	} else {

	    pSrcCmd = getCmdPathName_(&jobSubReq->command[jobSubReqCmd1Offset],
		                     &srcCmdLen);
	}
        if (srcCmdLen >= sizeof(lineStrBuf)) {
	    lsberrno = LSBE_SYS_CALL;
            errno = ENAMETOOLONG;
            return (-1);
        }


	memcpy(lineStrBuf, pSrcCmd, srcCmdLen);
	lineStrBuf[srcCmdLen] = 0;


	spoolInfo = chUserCopySpoolFile(lineStrBuf, SPOOL_COMMAND);
	if (spoolInfo == NULL) {
            return (-1);
        }


	pSpoolCmd = spoolInfo->spoolFile;
	spoolCmdLen = strlen(pSpoolCmd);
	if (spoolCmdLen >= MAXFILENAMELEN) {
	    lsberrno = LSBE_SYS_CALL;
	    errno = ENAMETOOLONG;
	    return (-1);
	}
        memcpy(subSpoolFiles->commandSpool, pSpoolCmd, spoolCmdLen + 1);
	submitReq->commandSpool = subSpoolFiles->commandSpool;

	if (!(jobSubReq->options & SUB_MODIFY)) {

	    char* pChangeSrcCmd = (char*)pSrcCmd;

	    if (spoolCmdLen <= srcCmdLen) {

	        memcpy(pChangeSrcCmd, pSpoolCmd, spoolCmdLen);
	        for (ii = spoolCmdLen, jj = srcCmdLen;
				 pChangeSrcCmd[jj]; ii++, jj++) {
	            pChangeSrcCmd[ii] = pChangeSrcCmd[jj];
	        }
	        pChangeSrcCmd[ii] = 0;
            } else {

	        int oldLen, newLen;


	        sp = jobSubReq->command;
	        oldLen = strlen(sp);
	        newLen = oldLen + spoolCmdLen - srcCmdLen;
	        sp = realloc(sp, newLen);
	        if (sp == NULL) {
                    lsberrno = LSBE_NO_MEM;
                    return (-1);
	        }
	        jobSubReq->command = sp;


	        pChangeSrcCmd = (char*)
		      getCmdPathName_(&jobSubReq->command[jobSubReqCmd1Offset],
		                      &srcCmdLen);


	        for (ii = newLen, jj = oldLen;
			   jj >= srcCmdLen; ii--, jj--) {
	            pChangeSrcCmd[ii] = pChangeSrcCmd[jj];
	        }
	        memcpy(pChangeSrcCmd, pSpoolCmd, spoolCmdLen);
	    }
	}
    } else if (jobSubReq->options2 & SUB2_MODIFY_CMD) {

	strcpy(submitReq->command, jobSubReq->newCommand);
	submitReq->commandSpool = "";
    } else {
	submitReq->commandSpool = "";
    }



    {
        lineStrBuf[0] = '\0';
        strncat(lineStrBuf, jobSubReq->command, MIN(i, MAXLINELEN));
    }

    if ((myHostName = ls_getmyhostname()) == NULL) {
        lsberrno = LSBE_LSLIB;
	return (-1);
    }
    if (jobSubReq->resReq != NULL && jobSubReq->options & SUB_RES_REQ) {
        if (strlen (jobSubReq->resReq) > MAXLINELEN -1) {
	    lsberrno = LSBE_BAD_RESREQ;
	    return (-1);
	}

        strcpy (submitReq->resReq,jobSubReq->resReq);
    } else  {

      ls_eligible(lineStrBuf, submitReq->resReq, LSF_REMOTE_MODE);
    }

    submitReq->fromHost = myHostName;

    umask(submitReq->umask = umask(0077));



    if ((jobSubReq->options & SUB_OTHER_FILES) && jobSubReq->nxf > 0) {
	submitReq->nxf = jobSubReq->nxf;
	submitReq->xf = jobSubReq->xf;
    } else
        submitReq->nxf = 0;

    if ((pw = getpwdirlsfuser_(auth->lsfUserName)) == NULL) {
	lsberrno = LSBE_SYS_CALL;
	return (-1);
    }

    if (strlen(pw->pw_dir) >= MAXFILENAMELEN - 1) {
	lsberrno = LSBE_SYS_CALL;
	errno = ENAMETOOLONG;
	return (-1);
    }


    for (i = 0; pw->pw_dir[i] != '\0' && submitReq->cwd[i] == pw->pw_dir[i];
	 i++);



    if ((i != 0) && (pw->pw_dir[i] == '\0')) {
	if (submitReq->cwd[i] == '\0')
	    submitReq->cwd[0] = '\0';
	else if (submitReq->cwd[i] == '/')
	    strcpy(submitReq->cwd, submitReq->cwd+i+1);
    }

    strcpy(submitReq->subHomeDir, pw->pw_dir);

    submitReq->sigValue = (jobSubReq->options & SUB_WINDOW_SIG)
	? sig_encode(jobSubReq->sigValue) : 0;
    if (submitReq->sigValue > 31 || submitReq->sigValue < 0) {
        lsberrno = LSBE_BAD_SIGNAL;
	return (-1);
    }

    return (0);

}

static char *
acctMapGet(int *fail, char *lsfUserName)
{

   char hostfn[MAXFILENAMELEN];
   FILE *fp;
   char *line, *map, clusorhost[MAX_LSB_NAME_LEN];
  char user[MAX_LSB_NAME_LEN], dir[40];
   int maplen, len, num;
   struct passwd *pw;
   struct stat statbuf;

   if ((pw = getpwdirlsfuser_(lsfUserName)) == NULL)
       return (NULL);

   strcpy(hostfn, pw->pw_dir);
   strcat(hostfn,"/.lsfhosts");

   if ((fp=fopen(hostfn, "r")) == NULL)
       return(NULL);


   if ((fstat(fileno(fp), &statbuf) < 0) ||
	(statbuf.st_uid != 0 && statbuf.st_uid != pw->pw_uid) ||
	(statbuf.st_mode & 066)) {
       fclose(fp);
       return(NULL);
   }

   maplen =256;
   map = malloc(maplen);
   if (!map) {
      lsberrno = LSBE_NO_MEM;
      *fail=1;
      return(NULL);
   }
   map[0]='\0';
   len =0;
   while((line=getNextLine_(fp, TRUE)) != NULL) {
       num = sscanf(line, "%s %s %s", clusorhost, user, dir);
       if (num < 2 || (num ==3 && (strcmp(dir,"recv") == 0 &&
                                  strcmp(dir,"send") != 0)))
           continue;

       if (strcmp(user,"+") == 0) {
           continue;
       }

       len += strlen(clusorhost) + 1 + strlen(user) + 1;
       if (len > maplen) {
          char *newmap;

          maplen += 256;
          if ((newmap=realloc(map, maplen)) == NULL) {
             lsberrno = LSBE_NO_MEM;
	     free(map);
             *fail=1;
	     return(NULL);
          }
          map = newmap;
       }
       strcat(map,clusorhost);
       strcat(map," ");
       strcat(map,user);
       strcat(map," ");
   }
   return(map);

}

static int
getUserInfo(struct submitReq *submitReq, struct submit *jobSubReq)
{
    int childIoFd[2];
    char lsfUserName[MAXLINELEN];
    uid_t uid;
    pid_t pid;
    int cwdLen, len, fail;
    char *usermap;

    struct {
        int error;
        int eno;
        int lserrno;
        int lsberrno;
    } err;

    if (getLSFUser_(lsfUserName, MAXLINELEN) != 0) {
        return(-1);
    }

    uid = getuid();


    if (socketpair(AF_UNIX, SOCK_STREAM, 0, childIoFd) < 0) {
        lsberrno = LSBE_SYS_CALL;
        return (-1);
    }

    if ((pid = fork()) < 0) {
        lsberrno = LSBE_SYS_CALL;
        return (-1);
    } else if (pid > 0) {
        LS_WAIT_T status;
        close(childIoFd[1]);

        err.eno = -1;
        if (read(childIoFd[0], (char *) &err, sizeof(err)) != sizeof(err)) {
            lsberrno = LSBE_SYS_CALL;
            err.error = TRUE;
            goto waitforchild;
        }

        if (err.error) {
            errno = err.eno;
            lserrno = err.lserrno;
            lsberrno = err.lsberrno;
            goto waitforchild;
        }
        err.eno = -1;


        if (read(childIoFd[0], (char *) &cwdLen,
                 sizeof(cwdLen)) != sizeof(cwdLen)) {
            err.error = TRUE;
            lsberrno = LSBE_SYS_CALL;
            goto waitforchild;
        }
        if (read(childIoFd[0], (char *) submitReq->cwd, cwdLen) != cwdLen) {
            err.error = TRUE;
            lsberrno = LSBE_SYS_CALL;
            goto waitforchild;
        }


        err.eno = -1;
        if (read(childIoFd[0], (char *) &err, sizeof(err)) != sizeof(err)) {

            lsberrno = LSBE_SYS_CALL;
            err.error = TRUE;
            goto waitforchild;
        }

        if (err.error) {
            errno = err.eno;
            lserrno = err.lserrno;
            lsberrno = err.lsberrno;
            goto waitforchild;
        }
        err.eno = -1;


        if (read(childIoFd[0], (char *) &ed.len, sizeof(ed.len)) !=
	    sizeof(ed.len)) {
            err.error = TRUE;
            lsberrno = LSBE_SYS_CALL;
            goto waitforchild;
        }

        FREEUP(ed.data);
        if (ed.len > 0) {
            if ((ed.data = (char *) malloc(ed.len)) == NULL) {
                err.error = TRUE;
                lsberrno = LSBE_NO_MEM;
                goto waitforchild;
            }
            if (b_read_fix(childIoFd[0], ed.data, ed.len) != ed.len) {
		FREEUP(ed.data);
                err.error = TRUE;
                lsberrno = LSBE_SYS_CALL;
                goto waitforchild;
            }
        } else
            ed.data = NULL;


        err.eno = -1;
        if (read(childIoFd[0], (char *) &err, sizeof(err)) != sizeof(err)) {

            lsberrno = LSBE_SYS_CALL;
            err.error = TRUE;
            goto waitforchild;
        }

        if (err.error) {
            errno = err.eno;
            lserrno = err.lserrno;
            lsberrno = err.lsberrno;
            goto waitforchild;
        }
        err.eno = -1;

        if (read(childIoFd[0], (char *) &len, sizeof(len)) != sizeof(len)) {
            err.error = TRUE;
            lsberrno = LSBE_SYS_CALL;
            goto waitforchild;
        }
        if (len) {
            FREEUP(useracctmap);
            if ((useracctmap = malloc(len)) == NULL) {
                lsberrno = LSBE_NO_MEM;
                err.error = TRUE;
                goto waitforchild;
            }
            if (read(childIoFd[0], useracctmap, len) != len) {
                err.error = TRUE;
                lsberrno = LSBE_SYS_CALL;
                goto waitforchild;
            }
        }

waitforchild:
        close(childIoFd[0]);

        if (err.eno < 0)
            err.eno = errno;
        if (waitpid(pid, &status, 0) < 0) {
            lsberrno = LSBE_SYS_CALL;
            return (-1);
        }

	if (err.error) {
	    errno = err.eno;
            return(-1);
        }

        return (0);
    }



    close(childIoFd[0]);

    if (setuid(uid) < 0) {
        lsberrno = LSBE_BAD_USER;
        goto errorParent;
    }


    if(mygetwd_(submitReq->cwd) == NULL) {
        lsberrno = LSBE_SYS_CALL;
        goto errorParent;
    }
    cwdLen = strlen (submitReq->cwd) + 1;

    err.error = FALSE;
    if (write(childIoFd[1], (char *) &err, sizeof(err)) != sizeof(err)) {
        close(childIoFd[1]);
        exit(-1);
    }


    if (write(childIoFd[1], (char *) &cwdLen, sizeof(cwdLen))
                                                != sizeof(cwdLen)) {
        close(childIoFd[1]);
        exit(-1);
    }
    if (write(childIoFd[1], (char *) submitReq->cwd, cwdLen) != cwdLen) {
        close(childIoFd[1]);
        exit(-1);
    }

    if (runBatchEsub(&ed, jobSubReq) < 0) {
	goto errorParent;
    } else {
       err.error = FALSE;
       if (write(childIoFd[1], (char *) &err, sizeof(err)) != sizeof(err)) {
           close(childIoFd[1]);
           exit(-1);
       }
    }


    if (write(childIoFd[1], (char *) &ed.len, sizeof(ed.len))
	!= sizeof(ed.len)) {
        close(childIoFd[1]);
        exit (-1);
    }

    if (ed.len > 0) {
        if (write(childIoFd[1], (char *) ed.data, ed.len) != ed.len) {
            close(childIoFd[1]);
            exit (-1);
        }
    }


    fail=0;

    if ((usermap=acctMapGet(&fail, lsfUserName)) == NULL)
        len =0;
    else
        len = strlen(usermap) + 1;

    if (fail)
       goto errorParent;
    else {
       err.error = FALSE;
       if (write(childIoFd[1], (char *) &err, sizeof(err)) != sizeof(err)) {
           close(childIoFd[1]);
           exit(-1);
       }
    }

    if (write(childIoFd[1], (char *)&len, sizeof(len)) != sizeof(len)) {
        close(childIoFd[1]);
        exit(-1);
    }
    if (write(childIoFd[1], (char *)usermap, len) != len) {
        close(childIoFd[1]);
        exit(-1);
    }
    exit(0);

  errorParent:

    err.error = TRUE;
    err.eno = errno;
    err.lserrno = lserrno;
    err.lsberrno = lsberrno;


    if (write(childIoFd[1], (char *) &err, sizeof(err)) != sizeof(err))
        exit(-1);

    exit(0);

}


static int
xdrSubReqSize(struct submitReq *req)
{
    int i, sz;

    sz = 1024  +
	ALIGNWORD_(sizeof(struct submitReq));

    sz += ALIGNWORD_(strlen(req->queue) + 1) + 4 +
	  ALIGNWORD_(strlen(req->resReq)) + 4 +
	  ALIGNWORD_(strlen(req->fromHost) + 1) + 4 +
	  ALIGNWORD_(strlen(req->dependCond) + 1) + 4 +
	  ALIGNWORD_(strlen(req->jobName) + 1) + 4 +
	  ALIGNWORD_(strlen(req->command) + 1) + 4 +
	  ALIGNWORD_(strlen(req->jobFile) + 1) + 4 +
	  ALIGNWORD_(strlen(req->inFile) + 1) +	4 +
	  ALIGNWORD_(strlen(req->outFile) + 1) + 4 +
	  ALIGNWORD_(strlen(req->errFile) + 1) + 4 +
	  ALIGNWORD_(strlen(req->inFileSpool) + 1) + 4 +
	  ALIGNWORD_(strlen(req->commandSpool) + 1) + 4 +
	  ALIGNWORD_(strlen(req->preExecCmd) + 1) + 4 +
	  ALIGNWORD_(strlen(req->postExecCmd) + 1) + 4 +
	  ALIGNWORD_(strlen(req->hostSpec) + 1) + 4 +
	  ALIGNWORD_(strlen(req->chkpntDir) + 1) + 4 +
	  ALIGNWORD_(strlen(req->subHomeDir) + 1) + 4 +
	  ALIGNWORD_(strlen(req->cwd) + 1) + 4 +
	  ALIGNWORD_(strlen(req->mailUser) + 1) + 4 +
	  ALIGNWORD_(strlen(req->projectName) + 1) + 4;

    for (i = 0; i < req->numAskedHosts; i++)
	sz += ALIGNWORD_(strlen(req->askedHosts[i]) + 1 + 4);

    for (i = 0; i < req->nxf; i++)
	sz += ALIGNWORD_(sizeof(struct xFile) + 4 * 4);

    return (sz);
}


static
int createNiosSock(struct submitReq *submitReq)
{
    int                  asock;
    socklen_t            len;
    struct sockaddr_in   sin;

    if ((asock = TcpCreate_(TRUE, 0)) < 0) {
	lsberrno = LSBE_LSLIB;
	return(-1);
    }

    len = sizeof(sin);
    if (getsockname (asock, (struct sockaddr *) &sin, &len) < 0) {
        close(asock);
        lsberrno = LSBE_LSLIB;
       lserrno = LSE_SOCK_SYS;
       return (-1);
    }

    submitReq->niosPort = ntohs(sin.sin_port);
    return (asock);
}

static
void startNios(struct submitReq *submitReq, int asock, LS_LONG_INT jobId)
{
    char sockStr[10];
    char envStr[64];
    struct config_param niosParams[] = {
	{"LSF_SERVERDIR", NULL},
	{NULL, NULL}
    };
#define LSF_NIOSDIR 0


    setuid(getuid());

    if (initenv_(niosParams, NULL) < 0) {
	ls_perror("initenv nios");
	exit(-1);
    }

    if (submitReq->options2 & SUB2_BSUB_BLOCK) {
        sprintf(envStr, "%s", lsb_jobidinstr(jobId));
        putEnv("BSUB_BLOCK", envStr);
    }

    sprintf(envStr, "%s", lsb_jobidinstr(jobId));
    putEnv("LSB_JOBID", envStr);

    sprintf(envStr, "bkill %s", lsb_jobidinstr(jobId));
    putEnv("LSF_NIOS_DIE_CMD", envStr);

    sprintf(sockStr, "%d", asock);

    sprintf(niosPath, "%s/nios", niosParams[LSF_NIOSDIR].paramValue);
    niosArgv[0] = niosPath;
    niosArgv[1] = "-N";
    niosArgv[2] = sockStr;
    if (submitReq->options & SUB_PTY) {
	if (submitReq->options & SUB_PTY_SHELL)
	    niosArgv[3] = "2";
	else
	    niosArgv[3] = "1";
    } else {
	niosArgv[3] = "0";
    }

    niosArgv[4] = NULL;

    execv(niosArgv[0], niosArgv);
    perror(niosArgv[0]);
    exit(-1);
}


static void
postSubMsg(struct submit *req, LS_LONG_INT jobId, struct submitReply *reply)
{



    if ((req->options & SUB_QUEUE) || (req->options & SUB_RESTART)) {
	if(getenv("BSUB_STDERR"))
	    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,400, "Job <%s> is submitted to queue <%s>.\n")),  /* catgets 400 */
		lsb_jobid2str(jobId),  reply->queue);
        else
	    fprintf(stdout, (_i18n_msg_get(ls_catd,NL_SETN,400, "Job <%s> is submitted to queue <%s>.\n")),  /* catgets 400 */
		lsb_jobid2str(jobId),  reply->queue);
    } else {
        if (getenv("BSUB_STDERR"))
	    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,401, "Job <%s> is submitted to default queue <%s>.\n")), /* catgets 401 */
		lsb_jobid2str(jobId), reply->queue);
        else
	    fprintf(stdout, (_i18n_msg_get(ls_catd,NL_SETN,401, "Job <%s> is submitted to default queue <%s>.\n")), /* catgets 401 */
		lsb_jobid2str(jobId), reply->queue);
    }

    fflush(stdout);

    prtBETime_(req);

    if (req->options2 & SUB2_BSUB_BLOCK)
	fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,402, "<<Waiting for dispatch ...>>\n")));  /* catgets 402 */

    if (req->options & SUB_INTERACTIVE)
	fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,402, "<<Waiting for dispatch ...>>\n")));


}


void
prtBETime_(struct submit *req)
{
    static char fname[] = "prtBETime_";
    char *sp;

    if (logclass & (LC_TRACE | LC_EXEC | LC_SCHED))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);

    if (req->beginTime > 0) {
        sp = _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T_Y, &req->beginTime);
        fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,404, "Job will be scheduled after %s\n")), sp);  /* catgets 404 */
    }
    if (req->termTime > 0) {
        sp = _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T_Y, &req->termTime);
        fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,405, "Job will be terminated by %s\n")), sp);  /* catgets 405 */
    }
}



int
gettimefor (char *toptarg, time_t *tTime)
{
    struct tm *tmPtr;
    char *cp;
    int tindex, ttime[5];
    int currhour,currmin,currday;

    TIMEIT(1, *tTime = time(0), "time");
    TIMEIT(1, tmPtr = localtime(tTime), "localtime");
    tmPtr->tm_sec = 0;
    currhour = tmPtr->tm_hour;
    currmin  = tmPtr->tm_min;
    currday  = tmPtr->tm_mday;

    for (tindex = 0; toptarg; tindex++) {
        ttime[tindex] = 0;
        cp = strrchr(toptarg, ':');
        if ( cp != NULL ) {
            if (!isint_(cp+1))
		return(-1);
            ttime[tindex] = atoi(cp+1);
            *cp = '\000';
        } else {
            if (!isint_(toptarg))
		return(-1);
            ttime[tindex] = atoi(toptarg);
            tindex++;
            break;
        }
    }
    if ( tindex < 2 || tindex > 4 ) {
	return(-1);
    }
    if ( ttime[0] < 0 || ttime[0] > 59 ) {
	return(-1);
    }
    tmPtr->tm_min = ttime[0];

    if ( ttime[1] < 0 || ttime[1] > 23 ) {
	return(-1);
    }
    tmPtr->tm_hour = ttime[1];

    tindex -= 2;
    if ( tindex > 0 ) {
        if ( ttime[2] < 1 || ttime[2] > 31 ) {
	    return(-1);
        }
        if (( (ttime[2] < tmPtr->tm_mday) ||
              ((ttime[2] == tmPtr->tm_mday) && (ttime[1] < currhour)) ||
              ((ttime[2] == tmPtr->tm_mday) && (ttime[1] == currhour) &&
               (ttime[0] < currmin))
            ) && tindex == 1)
            tmPtr->tm_mon++;
        tmPtr->tm_mday = ttime[2];

        tindex--;
        switch (tindex) {
        case 1:
            if ( ttime[3] < 0 || ttime[3] > 12 ) {
		return(-1);
            }
            if ( (((ttime[3]-1) < tmPtr->tm_mon) ||
                 (((ttime[3]-1) == tmPtr->tm_mon) && (ttime[2] < currday)) ||
                 (((ttime[3]-1) == tmPtr->tm_mon) && (ttime[2] == currday) &&
                  (ttime[1] < currhour)) ||
                 (((ttime[3]-1) == tmPtr->tm_mon) && (ttime[2] == currday) &&
                  (ttime[1] == currhour) && (ttime[0] < currmin)))
               )
                tmPtr->tm_year++;
            tmPtr->tm_mon = ttime[3] - 1;
            break;
        default:
            break;
        }
    }


    tmPtr->tm_isdst = -1;


    *tTime = *tTime < mktime(tmPtr) ? mktime(tmPtr) : mktime(tmPtr) + 86400;

    return(0);
}

int
setOption_ (int argc, char **argv, char *template, struct submit *req,
	    int mask, int mask2, char **errMsg)
{
    int eflag = 0, oflag = 0;
    int badIdx, v1, v2;
    char *sp, *cp;
    char savearg[MAXLINELEN];
    char *optName;
    char *(*getoptfunc)();
    int flagI = 0;
    int flagK = 0;

    int flagPack = 0;
    int flagPackConflict = 0;

    struct args {
        int argc;
        char **argv;
    } myArgs;
    char* pExclStr;

    myArgs.argc = req->options;
    myArgs.argv = errMsg;

#define checkSubDelOption(option, opt) { \
        if (req->options & SUB_MODIFY && strcmp (optName, opt) == 0) { \
            if (req->options & option) { \
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,406, "You cannot modify and set default at the same time"))); /* catgets 406 */  \
                return (-1);}   \
            req->delOptions |= option; \
            break;}}

#define checkSubDelOption2(option2, opt) { \
        if (req->options & SUB_MODIFY && strcmp (optName, opt) == 0) { \
            if (req->options2 & option2) { \
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,406, "You cannot modify and set default at the same time"))); /* catgets 406 */  \
                return (-1);}   \
            req->delOptions2 |= option2; \
            break;}}

#define checkRLDelOption(rLimit, opt)  if (req->options & SUB_MODIFY && \
                                           strcmp (optName, opt) == 0) { \
            if (req->rLimits[rLimit] != DEFAULT_RLIMIT  \
                && req->rLimits[rLimit] != DELETE_NUMBER) { \
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,406, "You cannot modify and set default at the same  time")));  \
                return (-1);} \
            req->rLimits[rLimit] = DELETE_NUMBER; \
            break;}

    getoptfunc  = my_getopt;

    while ((optName = getoptfunc(argc, argv, template, errMsg)) != NULL) {
	switch (optName[0]) {

	case 'E':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        if (strcmp(optarg,"p")==0) {
            req->options |= SUB_POST_EXEC;

            if( (!argv[optind]) || argv[optind] == NULL || (strcmp(argv[optind], "") == 0)) {
                myArgs.argc = req->options;
                lsb_throw("LSB_BAD_BSUBARGS", &myArgs);
                return (-1);
            }

            if (mask & SUB_POST_EXEC) {
                req->postExecCmd = argv[optind];
                optind++;
            }

            break;
        }

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_PRE_EXEC, "En");
            if (mask & SUB_PRE_EXEC) {
     	        req->options |= SUB_PRE_EXEC;
 	        req->preExecCmd = optarg;
            }
	    break;

    case 'w':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_DEPEND_COND, "wn");
            if (mask & SUB_DEPEND_COND) {
	        req->options |= SUB_DEPEND_COND;
	        req->dependCond = optarg;
            }
	    break;

	case 'L':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
	    checkSubDelOption (SUB_LOGIN_SHELL, "Ln");
            if (mask & SUB_LOGIN_SHELL) {
     	        req->options |= SUB_LOGIN_SHELL;
 	        req->loginShell = optarg;
            }
	    break;

	case 'B':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_NOTIFY_BEGIN, "Bn");
	    req->options |= SUB_NOTIFY_BEGIN;
	    break;

	case 'f':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_OTHER_FILES, "fn");
	    if (req->options & SUB_RESTART) {
		req->options |= SUB_RESTART_FORCE;
		break;
	    }


            if (mask & SUB_OTHER_FILES) {
	        if (parseXF (req, optarg, errMsg) < 0)
	    	    return(-1);
                req->options |= SUB_OTHER_FILES;
            }
            break;

    case 'k':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption ((SUB_CHKPNT_PERIOD | SUB_CHKPNT_DIR), "kn");
            if (!(mask & SUB_CHKPNT_DIR))

                break;

            cp = optarg;
            while (*(cp++) == ' ');

            if ((sp = strchr(cp, ' ')) != NULL) {


                int  bPeriodExist = 0;
                int  bMethodExist = 0;
		char *pCurWord = NULL;


                *sp  = '\0';


                while (*(++sp) == ' ');


                while ((sp != NULL)&&(*sp != '\0')){
                    pCurWord = sp;

                    if (isdigit((int)(*pCurWord))) {

                        if ( bPeriodExist ){
                            PRINT_ERRMSG1(errMsg,
			       _i18n_msg_get(ls_catd,NL_SETN,408,
			       "%s: Bad checkpoint period value"),/* catgets 408 */
			       pCurWord);
                            return(-1);
                        }


                        sp = strchr(pCurWord,' ');
                        if ( sp != NULL){
                            *sp = '\0';
                        }

                        if (!isint_(pCurWord)
                            || (req->chkpntPeriod = atoi(pCurWord) * 60) <= 0){
                            PRINT_ERRMSG1(errMsg,
			       _i18n_msg_get(ls_catd,NL_SETN,408,
			       "%s: Bad checkpoint period value"),/* catgets 408 */
			       pCurWord);
                            return(-1);
                        }
                        bPeriodExist = 1;
                        req->options |= SUB_CHKPNT_PERIOD;

                    }else if (strstr(pCurWord,"method=") == pCurWord) {


                        if ( bMethodExist ){
                             PRINT_ERRMSG1(errMsg,
			       _i18n_msg_get(ls_catd,NL_SETN,445,
			       "%s: Syntax error. Correct syntax is method=name of your checkpoint method"),/* catgets 445 */
			       pCurWord);
                             return(-1);
                        }

                        sp = strchr(pCurWord,' ');
                        if (sp != NULL){
                            *sp = '\0';
                        }

                        pCurWord = strchr(pCurWord,'=');
                        if (*(++pCurWord) != '\0' ){
                            putEnv("LSB_ECHKPNT_METHOD",pCurWord);
                        }else{
                            PRINT_ERRMSG1(errMsg,
			       _i18n_msg_get(ls_catd,NL_SETN,445,
			       "%s: Syntax error. Correct syntax is method=name of your checkpoint method"),/* catgets 445 */
			       pCurWord);
	                    return(-1);
                        }
                        bMethodExist = 1;
                    }else{
                        PRINT_ERRMSG1(errMsg,
			       _i18n_msg_get(ls_catd,NL_SETN,446,
			       "%s: Syntax error. Correct syntax is bsub -k \"chkpnt_dir [period] [method=checkpoint method]\""),/* catgets 446 */
			       pCurWord);
                        return(-1);
                    }

                    if (sp != NULL){
                        while(*(++sp) == ' ');
                    }
                }
            }
	    req->chkpntDir = optarg;
            req->options |= SUB_CHKPNT_DIR;
	    break;

	case 'R':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

            checkSubDelOption (SUB_RES_REQ, "Rn");
            if (mask & SUB_RES_REQ) {
		if (req->resReq != NULL){
		    PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,442,
		    "Invalid syntax; the -R option was used more than once.\n")); /* catgets 442 */
		   return (-1);
		}
	        req->resReq = optarg;
                req->options |= SUB_RES_REQ;
	        while (*(req->resReq) == ' ')
	    	    req->resReq++;
            }
	    break;

	case 'x':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        req->options2 |= SUB2_MODIFY_PEND_JOB;
        checkSubDelOption (SUB_EXCLUSIVE, "xn");
	    req->options |= SUB_EXCLUSIVE;
	    break;

	case 'I':
            if (flagI || flagK || flagPack) {
		myArgs.argc = req->options;
		lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

                return (-1);
            }

	    if (!strcmp(optName, "I")) {
		req->options |= SUB_INTERACTIVE;
                flagI++;
	    } else if (!strcmp(optName, "Ip")) {
		req->options |= SUB_INTERACTIVE | SUB_PTY;
                flagI++;
	    } else if (!strcmp(optName, "Is")) {
		req->options |= SUB_INTERACTIVE | SUB_PTY | SUB_PTY_SHELL;
                flagI++;
	    } else {
		myArgs.argc = req->options;
		lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

		return (-1);
	    }
	    break;

    case 'H':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        checkSubDelOption (SUB2_HOLD, "Hn");
        req->options2 |= SUB2_HOLD;
        break;
    case 'K':
            if (flagI || flagK || flagPack) {
		myArgs.argc = req->options;
		lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

                return (-1);
            }
            flagK++;
            req->options2 |= SUB2_BSUB_BLOCK;
            break;
	case 'r':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        req->options2 |= SUB2_MODIFY_RUN_JOB;
        checkSubDelOption (SUB_RERUNNABLE, "rn");

	    req->options |= SUB_RERUNNABLE;
	    break;

	case 'N':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_NOTIFY_END, "Nn");
	    req->options |= SUB_NOTIFY_END;
	    break;

	case 'h':
        myArgs.argc = req->options;
        lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

	    return (-1);

	case 'm':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_HOST, "mn");
            if (!(mask & SUB_HOST))
                break;

	    req->options |= SUB_HOST;
            if (getAskedHosts_(optarg, &req->askedHosts, &req->numAskedHosts,
			   &badIdx, FALSE) < 0 && lserrno != LSE_BAD_HOST) {
		lsberrno=LSBE_LSLIB;
		PRINT_ERRMSG0(errMsg, ls_sysmsg());
                return (-1);
            }
	    if (req->numAskedHosts == 0) {
                myArgs.argc = req->options;
		lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

                return (-1);
            }
            break;

	case 'J':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        req->options2 |= SUB2_MODIFY_PEND_JOB;
        checkSubDelOption (SUB_JOB_NAME, "Jn");
        if (mask & SUB_JOB_NAME) {
        req->jobName = optarg;
        req->options |= SUB_JOB_NAME;
		req->options2 |= SUB2_MODIFY_PEND_JOB;
            }
	    break;

	case 'i':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        pExclStr = "isn|is|in|i";
	    req->options2 |= SUB2_MODIFY_PEND_JOB;
	    if (strncmp (optName, "is", 2) == 0) {



		if ((req->options & SUB_IN_FILE)
			     || (req->delOptions & SUB_IN_FILE)) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN, 444,
		        "%s options are exclusive"), /* catgets  444 */
		        pExclStr);
		    return(-1);
		}
                checkSubDelOption2 (SUB2_IN_FILE_SPOOL, "isn");
                if (!(mask2 & SUB2_IN_FILE_SPOOL))
                    break;

	        if (strlen(optarg) > MAXFILENAMELEN - 1) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,409,
		        "%s: File name too long"), /* catgets 409 */
		        optarg);
		    return(-1);
	        }
	        req->inFile = optarg;
	        req->options2 |= SUB2_IN_FILE_SPOOL;
	    } else {



		if ((req->options2 & SUB2_IN_FILE_SPOOL)
			     || (req->delOptions2 & SUB2_IN_FILE_SPOOL)) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN, 444,
		        "%s options are exclusive"), /* catgets  444 */
		        pExclStr);
		    return(-1);
		}

                checkSubDelOption (SUB_IN_FILE, "in");
                if (!(mask & SUB_IN_FILE))
                    break;

	        if (strlen(optarg) > MAXFILENAMELEN - 1) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,409,
		        "%s: File name too long"), /* catgets 409 */
		        optarg);
		    return(-1);
	        }
	        req->inFile = optarg;
	        req->options |= SUB_IN_FILE;
	    }
	    break;
	case 'o':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_RUN_JOB;
            checkSubDelOption (SUB_OUT_FILE, "on");

            if (!(mask & SUB_OUT_FILE))
                break;

	    if (strlen(optarg) > MAXFILENAMELEN - 1) {
		PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,410,
		    "%s: File name too long"), /* catgets 410 */
		    optarg);
		return(-1);
	    }
	    req->outFile = optarg;
	    req->options |= SUB_OUT_FILE;
	    oflag = 1;
	    if ( eflag ) {
                if (strcmp(req->outFile, req->errFile) == 0) {
                    req->options &= ~SUB_ERR_FILE;
                    req->errFile = "";
                }
	    }
	    break;

	case 'u':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_MAIL_USER, "un");
	    if ((mask & SUB_MAIL_USER))
	    {
	        if (strlen(optarg) > MAXHOSTNAMELEN - 1) {
	    	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,411,
			"%s: Mail destination name too long"),/* catgets 411 */
			optarg);
		    return(-1);
	        }

	        req->mailUser = optarg;
	        req->options |= SUB_MAIL_USER;
	    }
	    break;

	case 'e':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_RUN_JOB;
            checkSubDelOption (SUB_ERR_FILE, "en");

            if (!(mask & SUB_ERR_FILE))
                break;

	    if (strlen(optarg) > MAXFILENAMELEN - 1) {
		PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,412,
		    "%s: File name too long"), /* catgets 412 */
		    optarg);
		return(-1);
	    }
	    req->errFile = optarg;
	    req->options |= SUB_ERR_FILE;
	    eflag = 1;
	    if (oflag) {
                if (strcmp(req->outFile, req->errFile) == 0) {
                    req->options &= ~SUB_ERR_FILE;
                    req->errFile = "";
	        }
	    }
	    break;

	case 'n':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            if (req->options & SUB_MODIFY && strcmp (optName, "nn") == 0) {
               if (req->numProcessors != 0) {
                    PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,413,
			"You cannot modify and set default at the same time")); /* catgets 413 */
                    return (-1);
                }
                req->numProcessors = DEL_NUMPRO;
                req->maxNumProcessors = DEL_NUMPRO;
                break;
            }
            if (req->numProcessors != 0)

                break;

            if (getValPair (&optarg, &v1, &v2) < 0) {
                PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,414,
		    "Bad argument for option -n")); /* catgets 414 */
                return -1;
            }
            if (v1 <= 0 || v2 <= 0) {
	        PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,415,
		    "The number of processors must be a positive integer")); /* catgets 415 */
	        return(-1);
            }
            if (v1 != INFINIT_INT) {
                req->numProcessors = v1;
                req->maxNumProcessors = v1;
            }
            if (v2 != INFINIT_INT) {
                req->maxNumProcessors = v2;
                if (v1 == INFINIT_INT)
                    req->numProcessors = 1;
            }
            if (req->numProcessors > req->maxNumProcessors) {
                PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,416,
		    "Bad argument for option -n")); /* catgets 416 */
                return -1;
            }
	    break;

	case 'q':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_QUEUE, "qn");
            if (mask & SUB_QUEUE) {
	        req->options |= SUB_QUEUE;
	        req->queue = optarg;
            }
	    break;

	case 'b':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            if (req->options & SUB_MODIFY && strcmp (optName, "bn") == 0) {
                if (req->beginTime != 0 &&
                            req->beginTime != DELETE_NUMBER) {
                    PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,417,
			"You cannot modify and set default at the same time")); /* catgets 417 */
                    return (-1);
                }
                req->beginTime = DELETE_NUMBER;
                break;
            }
            if (req->beginTime != 0)

		break;

	    strcpy (savearg, optarg);
	    if (gettimefor(optarg, &req->beginTime) < 0) {
		lsberrno = LSBE_BAD_TIME;
		if (errMsg != NULL)
		    sprintf(*errMsg, "%s:%s", savearg, lsb_sysmsg());
		else
		    sub_perror (savearg);
		return (-1);
	    }
	    break;

	case 't':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            if (req->options & SUB_MODIFY && strcmp (optName, "tn") == 0) {
                if (req->termTime != 0 &&
                            req->termTime != DELETE_NUMBER) {
                    PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,417,
			"You cannot modify and set default at the same time"));
                    return (-1);
                }
                req->termTime = DELETE_NUMBER;
                break;
            }

            if (req->termTime != 0)

		break;

	    strcpy (savearg, optarg);
	    if (gettimefor(optarg, &req->termTime) < 0) {
		lsberrno = LSBE_BAD_TIME;
		if (errMsg != NULL)
		    sprintf(*errMsg, "%s:%s", savearg, lsb_sysmsg());
	        else
		    sub_perror (savearg);
	        return (-1);
	    }
	    break;

	case 's':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

        req->options2 |= SUB2_MODIFY_PEND_JOB;
	    if ( strncmp (optName, "sp", 2) == 0) {

		checkSubDelOption2(SUB2_JOB_PRIORITY, "spn");
		if (!(mask2 & SUB2_JOB_PRIORITY))
		    break;

		if ( !isint_(optarg) || (req->userPriority = atoi(optarg)) <= 0 ) {
		    PRINT_ERRMSG1(errMsg, I18N(494,
			"%s: Illegal job priority"), /* catgets 494 */
			optarg);
		    return (-1);
	   	}
		req->options2 |= SUB2_JOB_PRIORITY;
	    }
	    else {
		printf("-s option is set\n");
                checkSubDelOption (SUB_WINDOW_SIG, "sn");

                if (!(mask & SUB_WINDOW_SIG))
                    break;
	        if ((req->sigValue = getSigVal(optarg)) < 0) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,419,
		        "%s: Illegal signal value"), /* catgets 419 */
		        optarg);
		    return(-1);
	        }
	        req->options |= SUB_WINDOW_SIG;
	    }
	    break;

	case 'c':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_RUN_JOB;
            checkRLDelOption (LSF_RLIMIT_CPU, "cn");
            if (req->rLimits[LSF_RLIMIT_CPU] != DEFAULT_RLIMIT)

		break;



            strcpy (savearg, optarg);

	    if ((sp = strchr(optarg, '/')) != NULL &&
                 strlen(sp+1) > 0) {
                req->options |= SUB_HOST_SPEC;
                if (req->hostSpec && strcmp (req->hostSpec, sp + 1) != 0) {
                    PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,420,
		    "More than one host_spec is specified: <%s> and <%s>" )), req->hostSpec, sp + 1); /* catgets 420 */
                    return(-1);
                }
		req->hostSpec = sp + 1;
	    }
            if (sp)
                *sp = '\0';

            if ((cp = strchr(optarg, ':')) != NULL)
                *cp = '\0';
	    if ((!isint_(optarg)) || (atoi(optarg) < 0)) {
	        PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,421,
		    "%s: Bad CPULIMIT specification"), /* catgets 421 */
		    savearg);
	        return(-1);
            }
	    else
                req->rLimits[LSF_RLIMIT_CPU] = atoi(optarg);
            if (cp != NULL) {
                optarg = cp + 1;
                if ((!isint_(optarg)) || (atoi(optarg) < 0)) {
                    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,421,
			"%s: Bad CPULIMIT specification"),
			savearg);
                    return(-1);
                }
                else {
                    req->rLimits[LSF_RLIMIT_CPU] *= 60;
                    req->rLimits[LSF_RLIMIT_CPU] += atoi(optarg);
                }
            }
            if (req->rLimits[LSF_RLIMIT_CPU] < 0) {
                PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,423,
		    "%s: CPULIMIT value should be a positive integer"), /* catgets 423 */
		    savearg);
                 return(-1);
            }

	    if (!checkLimit(req->rLimits[LSF_RLIMIT_CPU], 60)) {
                PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,424,
		    "%s: CPULIMIT value is too big"), /* catgets 424 */
		    optarg);
		return(-1);
            }

	    req->rLimits[LSF_RLIMIT_CPU] *= 60;
            break;

	case 'P':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkSubDelOption (SUB_PROJECT_NAME, "Pn");
	    if ((mask & SUB_PROJECT_NAME))
	    {
	        if (strlen(optarg) > MAX_LSB_NAME_LEN - 1) {
	    	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,425,
			"%s:  project name too long"),  /* catgets 425 */
			optarg);
		    return(-1);
	        }

	        req->projectName = optarg;
	        req->options |= SUB_PROJECT_NAME;
	    }
	    break;


	case 'W':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_RUN_JOB;
            checkRLDelOption (LSF_RLIMIT_RUN, "Wn");

            if (req->rLimits[LSF_RLIMIT_RUN] != DEFAULT_RLIMIT)

		break;



            strcpy (savearg, optarg);
	    if ((sp = strchr(optarg, '/')) != NULL) {
                req->options |= SUB_HOST_SPEC;
                if (req->hostSpec && strcmp (req->hostSpec, sp + 1) != 0) {
                    PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,420,
			"More than one host_spec is specified: <%s> and <%s>")),
			req->hostSpec, sp + 1);
                    return(-1);
                }
		req->hostSpec = sp + 1;
		*sp = '\0';
	    }
            if ((cp = strchr(optarg, ':')) != NULL)
                *cp = '\0';
	    if ((!isint_(optarg)) || (atoi(optarg) < 0)) {
	        PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,428,
		    "%s: Bad RUNLIMIT specification"), /* catgets 428 */
		    savearg);
	        return(-1);
            }
	    else
                req->rLimits[LSF_RLIMIT_RUN] = atoi(optarg);
            if (cp != NULL) {
                optarg = cp + 1;
                if ((!isint_(optarg)) || (atoi(optarg) < 0)) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,428,
			"%s: Bad RUNLIMIT specification"),
			savearg);
                    return(-1);
                }
                else {
                    req->rLimits[LSF_RLIMIT_RUN] *= 60;
                    req->rLimits[LSF_RLIMIT_RUN] += atoi(optarg);
                }
            }
            if (req->rLimits[LSF_RLIMIT_RUN] < 0) {
                PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,430,
		    "%s: RUNLIMIT value should be a positive integer"), /* catgets 430 */
		    savearg);
                 return(-1);
            }
	    if (!checkLimit(req->rLimits[LSF_RLIMIT_RUN], 60)) {
                PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,431,
		    "%s: RUNLIMIT value is too big"),  /* catgets 431 */
		    optarg);
		return(-1);
            }

	    req->rLimits[LSF_RLIMIT_RUN] *= 60;
            break;

	case 'F':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkRLDelOption (LSF_RLIMIT_FSIZE, "Fn");
            if (req->rLimits[LSF_RLIMIT_FSIZE] != DEFAULT_RLIMIT)

		break;

	    if (isint_(optarg)
                     && ((req->rLimits[LSF_RLIMIT_FSIZE] = atoi(optarg)) > 0)) {
		break;
            }
	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,432,
		"%s: FILELIMIT value should be a positive integer"), /* catgets 432 */
		optarg);
	    return(-1);

	case 'D':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkRLDelOption (LSF_RLIMIT_DATA, "Dn");
            if (req->rLimits[LSF_RLIMIT_DATA] != DEFAULT_RLIMIT)

		break;

	    if (isint_(optarg)
		  && ((req->rLimits[LSF_RLIMIT_DATA] = atoi(optarg)) > 0)) {
	        break;
            }
	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,433,
		"%s: DATALIMIT value should be a positive integer"), /* catgets 433 */
	    	optarg);
	    return(-1);

	case 'S':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkRLDelOption (LSF_RLIMIT_STACK, "Sn");

            if (req->rLimits[LSF_RLIMIT_STACK] != DEFAULT_RLIMIT)

		break;

	    if (isint_(optarg)
		   && ((req->rLimits[LSF_RLIMIT_STACK] = atoi(optarg)) > 0)) {
	        break;
            }
	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,434,
		"%s: STACKLIMIT value should be a positive integer"), /* catgets 434 */
		optarg);
	    return(-1);

	case 'C':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_PEND_JOB;
            checkRLDelOption (LSF_RLIMIT_CORE, "Cn");
            if (req->rLimits[LSF_RLIMIT_CORE] != DEFAULT_RLIMIT)

		break;

	    if (isint_(optarg)
		&&  ((req->rLimits[LSF_RLIMIT_CORE] = atoi(optarg)) >= 0)) {
	        break;
            }
	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,435,
		"%s: CORELIMIT value should be a positive integer"), /* catgets 435 */
		optarg);
	    return(-1);

	case 'M':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    req->options2 |= SUB2_MODIFY_RUN_JOB;
            checkRLDelOption (LSF_RLIMIT_RSS, "Mn");
            if (req->rLimits[LSF_RLIMIT_RSS] != DEFAULT_RLIMIT)

		break;

	    if (isint_(optarg)
		    && ((req->rLimits[LSF_RLIMIT_RSS] = atoi(optarg)) > 0)) {
		break;
            }
	    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,436,
		"%s: MEMLIMT value should be a positive integer"), /* catgets 436 */
		optarg);
	    return(-1);


    case 'p':

        if (strcmp(optarg,"ack")==0) {

            if (flagI || flagK || flagPack || flagPackConflict) {
                myArgs.argc = req->options;
                lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

                return (-1);
            }

            flagPack ++;
            req->options |= SUB_PACK;

            if( (!argv[optind]) || argv[optind] == NULL || (strcmp(argv[optind], "") == 0)) {
                myArgs.argc = req->options;
                lsb_throw("LSB_BAD_BSUBARGS", &myArgs);
                return (-1);
            }

            req->packFile = argv[optind];
            optind++;

            break;
        }

        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

            checkRLDelOption (LSF_RLIMIT_PROCESS, "pn");
            if (req->rLimits[LSF_RLIMIT_PROCESS] != DEFAULT_RLIMIT)

                break;

            if (isint_(optarg)
                  && ((req->rLimits[LSF_RLIMIT_PROCESS] = atoi(optarg)) > 0)) {
                if (!checkLimit(req->rLimits[LSF_RLIMIT_PROCESS], 1)) {
                    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,437,
			"%s: PROCESSLIMIT value is too big\n"), /* catgets 437 */
			optarg);
                    return(-1);
                }
                break;
            }
            PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,438,
	        "%s: PROCESSLIMIT value should be a positive integer\n"), /* catgets 438 */
                optarg);
            return(-1);

    case 'v':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

            checkRLDelOption (LSF_RLIMIT_SWAP, "vn");
            if (req->rLimits[LSF_RLIMIT_SWAP] != DEFAULT_RLIMIT)

                break;

            if (isint_(optarg)
                    && ((req->rLimits[LSF_RLIMIT_SWAP] = atoi(optarg)) > 0)) {
                break;
            }
            PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,439,
		"%s: SWAPLIMIT value should be a positive integer\n"), /* catgets 439 */
                optarg);
            return(-1);

    case 'O':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    if ( req->options == 0) {
	        optionFlag = TRUE;
		strcpy( optionFileName, optarg);
             }
 	    else
                req->options |= SUB_MODIFY_ONCE;
            break;

    case 'Z':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

	    pExclStr = "Zsn|Zs|Z";
	    req->options2 |= SUB2_MODIFY_PEND_JOB;
	    if (strncmp (optName, "Zs", 2) == 0) {

		if (req->options2 & SUB2_MODIFY_CMD) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN, 444,
		        "%s options are exclusive"), /* catgets  444 */
		        pExclStr);
		    return(-1);
		}

                checkSubDelOption2 (SUB2_JOB_CMD_SPOOL, "Zsn");
                if (!(mask2 & SUB2_JOB_CMD_SPOOL))
                    break;

		if (req->options & SUB_MODIFY) {
	            if (strlen(optarg) >= MAXLINELEN) {
		        PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,409,
		            "%s: File name too long"), /* catgets 409 */
		            optarg);
		        return(-1);
	            }

	            req->newCommand = optarg;
	            req->options2 |= SUB2_MODIFY_CMD;
		}

	        req->options2 |= SUB2_JOB_CMD_SPOOL;
	    } else {

		if ((req->options2 & SUB2_JOB_CMD_SPOOL)
			     || (req->delOptions2 & SUB2_JOB_CMD_SPOOL)) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN, 444,
		        "%s options are exclusive"), /* catgets  444 */
		        pExclStr);
		    return(-1);
		}

	        if (strlen(optarg) >= MAXLINELEN) {
		    PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,409,
		        "%s: File name too long"), /* catgets 409 */
		        optarg);
		    return(-1);
	        }

	        req->newCommand = optarg;
	        req->options2 |= SUB2_MODIFY_CMD;
	    }
	    break;
    case 'a':
        if (flagPack) {
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

            return (-1);
        }
        flagPackConflict = 1;

		additionEsubInfo=putstr_(optarg);
		break;
	case 'V':
	    fputs(_LS_VERSION_, stdout);
	    exit(0);

	default:
            myArgs.argc = req->options;
            lsb_throw("LSB_BAD_BSUBARGS", &myArgs);

	    return (-1);
	}

    }

    if ((req->options & SUB_INTERACTIVE)
        && (req->options & SUB_JOB_NAME)
        && strchr(req->jobName, '[')) {
	PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,440,
	    "Interactive job not supported for job arrays")); /* catgets 440 */
        return(-1);
    }

    if ((req->options2 & SUB2_BSUB_BLOCK)
        && (req->options & SUB_JOB_NAME)
        && strchr(req->jobName, '[')) {
	PRINT_ERRMSG0(errMsg, _i18n_msg_get(ls_catd,NL_SETN,441,
	    "Job array doesn't support -K option")); /* catgets 441 */
        return(-1);
    }
    return (0);
}


static int
parseLine_ (char *line, int *embedArgc, char ***embedArgv, char **errMsg )
{
#define INCREASE 40
    int i;
    static char **argBuf = NULL, *key;
    static char fname[] = "parseLine_";
    static int argNum = 0;
    static int first = TRUE,  bufSize = INCREASE;
    char *sp, *sQuote, *dQuote, quoteMark;
    char **tmp;

    if (first == TRUE) {
        if ((argBuf = (char **) malloc(INCREASE * sizeof(char *)))
             == NULL) {
            PRINT_ERRMSG2(errMsg, I18N_FUNC_FAIL_M, fname, "malloc");
            return (-1);
        }
	first = FALSE;
    } else {
	for (i = 0; i < argNum; i++)
	    FREEUP(argBuf[i + 1]);
        argNum = 0;
    }
    *embedArgc = 1;
    argBuf[0] = "bsub";
    argNum = 1;
    key = "BSUB";
    SKIPSPACE(line);


    if (*line == '\0' || *line != '#')
        return (-1);

    ++line;
    SKIPSPACE(line);
    if (strncmp (line, key, strlen(key)) == 0) {
        line += strlen(key);
        SKIPSPACE(line);
        if (*line != '-') {
            return (-1);
        }
        while (TRUE) {
            quoteMark = '"';
            if ((sQuote = strchr(line, '\'')) != NULL)
                if ((dQuote = strchr(line, '"')) == NULL || sQuote < dQuote)

                    quoteMark = '\'';

            if ((sp = getNextValueQ_(&line, quoteMark, quoteMark)) == NULL)
                goto FINISH;

            if (*sp == '#')
                goto FINISH ;

            if ((*embedArgc) + 2 > bufSize ) {
                if ((tmp = (char **) realloc(argBuf,
                           (bufSize + INCREASE) * sizeof(char *))) == NULL) {
                    PRINT_ERRMSG2(errMsg, I18N_FUNC_FAIL_M, fname,
				"realloc");
                    argNum = *embedArgc - 1;
		    *embedArgv = argBuf;
                    return (-1);
                }
			argBuf = tmp;
			bufSize += INCREASE;
            }
            argBuf[*embedArgc] = putstr_(sp);
            (*embedArgc)++;
			 argBuf[*embedArgc] = NULL;
        }
    }
    else
	return (-1);

FINISH:
    argNum = *embedArgc - 1;
    argBuf[*embedArgc] = NULL;
    *embedArgv = argBuf;
    return (0);
}


struct submit *
parseOptFile_ (char *filename, struct submit *req, char **errMsg)
{
    static char fname[] = "parseOptFile_";
    char *lineBuf;
    int length = 0;
    int lineLen;
    int optArgc;
    char **optArgv;
    char template[] = "E:T:w:f:k:R:m:J:L:u:i:o:e:n:q:b:t:sp:s:c:W:F:D:S:C:M:P:p:v:Ip|Is|I|r|H|x|N|B|h|V|G:X:";
    pid_t pid;
    uid_t uid;
    int childIoFd[2];
    LS_WAIT_T status;

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);
    if (access(filename, F_OK) != 0) {
	PRINT_ERRMSG3(errMsg, I18N_FUNC_S_FAIL,
	    fname, "access", filename);
	return (NULL);
    }

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, childIoFd) < 0) {
	PRINT_ERRMSG2(errMsg, I18N_FUNC_FAIL, fname, "socketpair");
	lsberrno = LSBE_SYS_CALL;
	return (NULL);
    }
    optArgc = 0;

    pid = fork();
    if (pid < 0) {
        lsberrno = LSBE_SYS_CALL;
        PRINT_ERRMSG2(errMsg, I18N_FUNC_FAIL_M, fname, "fork");
        return(NULL);
    } else if (pid == 0) {

	char childLine[MAXLINELEN*4];
	int  exitVal = -1;

        close (childIoFd[0]);
        uid = getuid();
        if (setuid (uid) < 0) {
            lsberrno = LSBE_BAD_USER;
	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "setuid");
	    goto childExit;
        }
        if (logclass & (LC_TRACE | LC_EXEC))
	    ls_syslog(LOG_DEBUG1, "%s: Child tries to open a option file <%s>",
			fname, filename);
	lineLen = 0;
        childLine[0] = '\0';

        if (readOptFile(filename, childLine) < 0)
            exit(exitVal);

        lineLen=strlen(childLine);

        if (write(childIoFd[1], (char *)&lineLen, sizeof(lineLen))
	    != sizeof(lineLen)) {
	        goto childExit;
        }
        if (write(childIoFd[1], (char *) childLine, lineLen) != lineLen) {
	    goto childExit;
        }
        exitVal = 0;
childExit:
        if (logclass & (LC_TRACE | LC_EXEC)) {
	    if (exitVal == 0)
	        ls_syslog(LOG_DEBUG,
	            "%s: Child succeeded in sending messages to parent", fname);
            else
	        ls_syslog(LOG_DEBUG,
	            "%s: Child failed in sending messages to parent", fname);
        }
        close(childIoFd[1]);
        exit(exitVal);
    }


    if (waitpid (pid, &status, 0) < 0)  {
        lsberrno = LSBE_SYS_CALL;
        goto parentErr;
    }

    close (childIoFd[1]);
    if (WEXITSTATUS (status)) {
        ls_syslog(LOG_DEBUG, "%s: child failed!", fname);
	goto parentErr;
    }

    if (read (childIoFd[0], (char *) &length, sizeof(length))
		 != sizeof (length))
        goto parentErr;

    if ((lineBuf = (char *) malloc (length + 1)) == NULL ) {
        if (logclass & (LC_TRACE | LC_EXEC))
	    ls_syslog(LOG_DEBUG, "%s: parent malloc faild!", fname);
	goto parentErr;
    }
    if (read(childIoFd[0], (char *) lineBuf, length) != length) {
        goto parentErr;
    }

    if (length) {
	char *p ;
	p = lineBuf + length;
	*p = '\0';
        if (parseLine_ (lineBuf, &optArgc, &optArgv, errMsg) == -1)
	    goto parentErr;
    }
    if (optArgc > 1) {

        optind = 1;
        if (setOption_ (optArgc, optArgv, template, req,
			~req->options, ~req->options2, errMsg) == -1)
	    goto parentErr;
    }
    close(childIoFd[0]);
    FREEUP (lineBuf);
    return (req);

parentErr:
    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: parent malloc faild!", fname);
    FREEUP (lineBuf);
    close(childIoFd[0]);
    return (NULL);
}


void
subUsage_(int option, char **errMsg)
{
#define I18N_ESUB_INFO_USAGE \
    _i18n_msg_get(ls_catd,NL_SETN,447,"\t\t[-a additional_esub_information]\n")

    if (errMsg == NULL) {
	if (option & SUB_RESTART) {

            fprintf(stderr, I18N_Usage);
	    fprintf(stderr, ": brestart [-h] [-V] [-x] [-f] [-N] [-B] [-q \"queue_name...\"] \n");
	    fprintf(stderr, "\t\t[-m \"host_name[+[pref_level]] | host_group[+[pref_level]]...]\"\n");

	    fprintf(stderr, "\t\t[-w \'dependency_expression\'] [-b begin_time] [-t term_time]\n");
	    fprintf(stderr, "\t\t[-c [hour:]minute[/host_name|/host_model]]\n");
	    fprintf(stderr, "\t\t[-F file_limit] [-D data_limit]\n");
	    fprintf(stderr, "\t\t[-C core_limit] [-M mem_limit] \n");
	    fprintf(stderr, "\t\t[-W run_limit[/host_name|/host_model]] \n");
	    fprintf(stderr, "\t\t[-S stack_limit] [-E \"pre_exec_command [argument ...]\"]\n");

	    fprintf(stderr, "\t\tcheckpoint_dir[job_ID | \"job_ID[index]\"]\n");
	    fprintf(stderr, I18N_ESUB_INFO_USAGE);

	} else if (option & SUB_MODIFY) {
	    fprintf(stderr, I18N_Usage);
	    fprintf(stderr, ": bmod [-h] [-V] [-x | -xn]");

	    fprintf(stderr, "\n");

	    if (lsbMode_ & LSB_MODE_BATCH) {
		fprintf(stderr, "\t\t[-r | -rn] [-N | -Nn] [-B | -Bn]\n");
		fprintf(stderr, "\t\t[-c cpu_limit[/host_spec] | -cn] [-F file_limit | -Fn]\n");
		fprintf(stderr, "\t\t[-M mem_limit | -Mn] [-D data_limit | -Dn] [-S stack_limit | -Sn]\n");
		fprintf(stderr, "\t\t[-C core_limit | -Cn] [-W run_limit[/host_spec] | -Wn ]\n");
		fprintf(stderr, "\t\t[-k chkpnt_dir [chkpnt_period] | -kn] [-P project_name | -Pn]\n");
		fprintf(stderr, "\t\t[-L login_shell | -Ln] \n");
	    }


	    fprintf(stderr, "\t\t[-w depend_cond | -wn] [-R res_req| -Rn] [-J job_name | -Jn]\n");
	    fprintf(stderr, "\t\t[-q queue_name ... | -qn] \n");
	    fprintf(stderr, "\t\t[-m host_name[+[pref_level]] | host_group[+[pref_level]]...| -mn]\"\n");
	    fprintf(stderr, "\t\t[-n min_processors[,max_processors] | -nn]\n");
	    fprintf(stderr, "\t\t[-b begin_time | -bn] [-t term_time | -tn]\n");
	    fprintf(stderr, "\t\t[-i in_file | -is in_file | -in | -isn]\n");
	    fprintf(stderr, "\t\t[-o out_file | -on] [-e err_file | -en]\n");
	    fprintf(stderr, "\t\t[-u mail_user | -un] [[-f \"lfile op [rfile]\"] ... | -fn] \n");
	    fprintf(stderr, "\t\t[-E \"pre_exec_command [argument ...]\" | -En]\n");
	    fprintf(stderr, "\t\t[-sp job_priority | -spn]\n");
	    fprintf(stderr, "\t\t[-Z \"new_command\" | -Zs \"new_command\" | -Zsn] \n");
	    fprintf(stderr, "\t\t[ jobId | \"jobId[index_list]\" ] \n");
	    fprintf(stderr, I18N_ESUB_INFO_USAGE);
	} else {

	    fprintf(stderr, I18N_Usage);
	    fprintf(stderr, ": bsub [-h] [-V] [-x] [-H]");

	    if (lsbMode_ & LSB_MODE_BATCH) {
		fprintf(stderr, " [-r] [-N] [-B] [-I | -K | -Ip | -Is]\n");
		fprintf(stderr, "\t\t[-L login_shell] [-c cpu_limit[/host_spec]] [-F file_limit]\n");
		fprintf(stderr, "\t\t[-W run_limit[/host_spec]] [-k chkpnt_dir [chkpnt_period] [method=chkpnt_dir]]\n");
		fprintf(stderr, "\t\t[-P project_name] ");
	    }
	    fprintf(stderr, "\n");

	    fprintf(stderr, "\t\t[-q queue_name ...]  [-R res_req]\n");
	    fprintf(stderr, "\t\t[-m \"host_name[+[pref_level]] | host_group[+[pref_level]]...]\"\n");
	    fprintf(stderr, "\t\t[-n min_processors[,max_processors]] [-J job_name]\n");
	    fprintf(stderr, "\t\t[-b begin_time] [-t term_time] [-u mail_user]\n");
	    fprintf(stderr, "\t\t[-i in_file | -is in_file] [-o out_file] [-e err_file]\n");
	    fprintf(stderr, "\t\t[-M mem_limit]  [-D data_limit]  [-S stack_limit]\n");

	    fprintf(stderr, "\t\t[[-f \"lfile op [rfile]\"] ...] [-w depend_cond]\n");

	    fprintf(stderr, "\t\t[-E \"pre_exec_command [argument ...]\"] [-Zs]\n");
        fprintf(stderr, "\t\t[-Ep \"post_exec_command [argument ...]\"\n");
	    fprintf(stderr, "\t\t[-sp job_priority]\n");
	    fprintf(stderr, "\t\t[command [argument ...]]\n");

        fprintf(stderr, "\t\t-pack job_submission_file\n");

        fprintf(stderr, I18N_ESUB_INFO_USAGE);
	}

        exit (-1);
    }

}


static int
parseXF(struct submit *req, char *arg, char **errMsg)
{
    static int maxNxf = 0;
    static struct xFile *xp = NULL;
    static char fname[] = "parseXF";
    struct xFile *tmp = NULL;
    int options = 0;
    char op[MAXLINELEN], lf[MAXFILENAMELEN], rf[MAXFILENAMELEN];
    char *p;
    char saveArg[MAXLINELEN];

#define NUMXF 10

    if (maxNxf == 0) {
	if ((xp = (struct xFile *) malloc(NUMXF * sizeof(struct xFile)))
	    == NULL) {
	    if (errMsg != NULL) {
	        sprintf(*errMsg, I18N_FUNC_FAIL_S, fname, "malloc",
			 lsb_sysmsg());
            }
	    else
	        sub_perror(_i18n_msg_get(ls_catd,NL_SETN,484, "Unable to allocate memory for -f option"));  /* catgets 484 */
	    return (-1);
	}
	maxNxf = NUMXF;
    }

    req->xf = xp;

    strcpy(saveArg, arg);


    if ((p = strstr(saveArg, "<<"))) {
        strcpy(op, "<<");
        options = XF_OP_EXEC2SUB | XF_OP_EXEC2SUB_APPEND;
    } else if ((p = strstr(saveArg, "><"))) {
        strcpy(op, "><");
        options = XF_OP_EXEC2SUB | XF_OP_SUB2EXEC;
    } else if ((p = strstr(saveArg, "<>"))) {
        strcpy(op, "<>");
        options = XF_OP_EXEC2SUB | XF_OP_SUB2EXEC;
    } else if ((p = strstr(saveArg, ">>"))) {
        strcpy(op, ">>");
        PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,487,
        "Invalid file operation \"%s\" specification in -f \"%s\"")),  /* catgets 487 */
        op, saveArg);
        return (-1);
    } else if ((p = strstr(saveArg, "<"))) {
        strcpy(op, "<");
        options = XF_OP_EXEC2SUB;
    } else if ((p = strstr(saveArg, ">"))) {
        strcpy(op, ">");
        options = XF_OP_SUB2EXEC;
    } else {
        PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,487,
        "Invalid file operation \"%s\" specification in -f \"%s\"")),  /* catgets 487 */
        op, saveArg);
        return (-1);
    }

    memset(lf, 0, MAXFILENAMELEN);
    memset(rf, 0, MAXFILENAMELEN);
    memcpy(lf, saveArg, p - saveArg);
    memcpy(rf, p + strlen(op), strlen(saveArg) - strlen(lf) - strlen(op));

    if (strstr(lf, ">") || strstr(rf, ">") || strstr(rf, "<")) {
         PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,487,
         "Invalid file operation \"%s\" specification in -f \"%s\"")),  /* catgets 487 */
         op, saveArg);
         return (-1);
    }

    if (strlen(lf) != 0) {
        if ((lf[strlen(lf) - 1]) != ' ') {
            PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,487,
            "Invalid file operation \"%s\" specification in -f \"%s\"")),  /* catgets 487 */
            op, saveArg);
            return (-1);
        }

        trimSpaces(lf);

        if (strlen(lf) == 0) {
            PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,485,
            "Invalid local file specification in -f \"%s\""), /* catgets 485 */
            saveArg);
            return (-1);
       }
    } else {
        PRINT_ERRMSG1(errMsg, _i18n_msg_get(ls_catd,NL_SETN,485,
        "Invalid local file specification in -f \"%s\""), /* catgets 485 */
        saveArg);
        return (-1);
    }

    if (strlen(rf) != 0) {
        if ((rf[0]) != ' ') {
            PRINT_ERRMSG2(errMsg, (_i18n_msg_get(ls_catd,NL_SETN,487,
            "Invalid file operation \"%s\" specification in -f \"%s\"")),  /* catgets 487 */
            op, saveArg);
            return (-1);
        }

        trimSpaces(rf);

        if (strlen(rf) == 0) {
            strcpy(rf, lf);
        }
    } else {
        strcpy(rf, lf);
    }

    if (req->nxf + 1 > maxNxf) {
	tmp = xp;
	if ((xp = (struct xFile *) myrealloc(req->xf,
					    (maxNxf + NUMXF) * sizeof(struct xFile)))
	    == NULL) {
	    if (errMsg != NULL) {
	        sprintf(*errMsg, I18N_FUNC_FAIL_S, fname, "myrealloc",
			 lsb_sysmsg());
            }
	    else
	        sub_perror((_i18n_msg_get(ls_catd,NL_SETN,484, "Unable to allocate memory for -f option")));
            xp = tmp;
	    return (-1);
	}
	maxNxf += NUMXF;
	req->xf = xp;
    }

    strcpy(req->xf[req->nxf].subFn, lf);
    strcpy(req->xf[req->nxf].execFn, rf);
    req->xf[req->nxf].options = options;

    req->nxf++;
    return (0);
}


static int
checkLimit(int limit, int factor)
{
    if ((float) limit * (float) factor >= (float) INFINIT_INT )
        return FALSE;
    else
        return TRUE;
}


static int aggregateFileContent(
    const char *deltaFilePath, 
    char **buffer, 
    size_t *buffer_size) 
{
    static char fname[] = "aggregateFileContent";
    FILE *fp;
    long file_size;
    char *file_content;
    size_t new_size;
    char *new_buffer;

    if (access(deltaFilePath, R_OK) != 0) {
        return 0;
    }

    fp = fopen(deltaFilePath, "r");
    if (fp == NULL) {
        ls_syslog(LOG_WARNING, "%s: Cannot open delta file %s: %m", fname, deltaFilePath);
        unlink(deltaFilePath);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size > 0) {
        file_content = (char *)malloc(file_size + 1);
        if (file_content == NULL) {
            ls_syslog(LOG_ERR, "%s: Failed to allocate memory to read delta file %s", fname, deltaFilePath);
            fclose(fp);
            unlink(deltaFilePath);
            return -1;
        }

        if (fread(file_content, 1, file_size, fp) != file_size) {
            ls_syslog(LOG_WARNING, "%s: Short read from delta file %s", fname, deltaFilePath);
            free(file_content);
            fclose(fp);
            unlink(deltaFilePath);
            return 0;
        }
        file_content[file_size] = '\0';

        new_size = *buffer_size + file_size;
        new_buffer = (char *)realloc(*buffer, new_size + 1);
        if (new_buffer == NULL) {
            ls_syslog(LOG_ERR, "%s: Failed to reallocate aggregation buffer", fname);
            free(file_content);
            fclose(fp);
            unlink(deltaFilePath);
            return -1;
        }
        
        memcpy(new_buffer + *buffer_size, file_content, file_size);
        *buffer = new_buffer;
        *buffer_size = new_size;
        (*buffer)[*buffer_size] = '\0';

        free(file_content);
    }
    
    fclose(fp);
    unlink(deltaFilePath);

    return 0;
}

int
runBatchEsub(struct lenData *ed, struct submit *jobSubReq)
{
    static char fname[] = "runBatchEsub";

    char *subRLimitName[LSF_RLIM_NLIMITS] = {"LSB_SUB_RLIMIT_CPU",
					     "LSB_SUB_RLIMIT_FSIZE",
					     "LSB_SUB_RLIMIT_DATA",
					     "LSB_SUB_RLIMIT_STACK",
					     "LSB_SUB_RLIMIT_CORE",
					     "LSB_SUB_RLIMIT_RSS",
					     "LSB_SUB_RLIMIT_NOFILE",
					     "LSB_SUB_RLIMIT_OPEN_MAX",
					     "LSB_SUB_RLIMIT_SWAP",
					     "LSB_SUB_RLIMIT_RUN",
					     "LSB_SUB_RLIMIT_PROCESS"};
    int cc, i;
    char parmFile[MAXFILENAMELEN], esub[MAXFILENAMELEN];
    FILE *parmfp;
    struct stat sbuf;
#define LSB_SUB_COMMANDNAME 0
    struct config_param myParams[] = { {"LSB_SUB_COMMANDNAME", NULL},
                                       {NULL, NULL} };

    char esub_path[MAXFILENAMELEN];
    char* esub_method;
    char *parmDeltaFile;
    char *envDeltaFile;
    char *aggregated_parms;
    size_t aggregated_parms_size;
    char *aggregated_envs;
    size_t aggregated_envs_size;
    extern char* additionEsubInfo;



#define QUOTE_STR(_str1,_str)  { \
int i, j, cnt=0; \
char ch, next, *tmp_str=NULL; \
    for (i=0; _str1[i]; i++){ \
        if (_str1[i] == '"') \
            cnt++ ; \
    } \
    tmp_str = (char *)malloc((strlen(_str1)+1+cnt+2)*sizeof(char)) ; \
    if (tmp_str != NULL) { \
        tmp_str[0] = '"' ; \
        _str = tmp_str+1 ; \
        strcpy (_str, _str1) ; \
        for (i=0; _str[i]; ){ \
            if (_str[i] == '"') { \
                ch = _str[i] ; \
                _str[i] = '\\' ; \
                next = ch ; \
                for ( j=i+1; _str[j]; j++){ \
                   ch = _str[j] ;  \
                   _str[j] = next ;  \
                   next = ch ; \
                } \
                _str[j] = next ;  \
                _str[j+1] = '\0' ; \
                i += 2;  \
            } \
            i++ ; \
        }  \
        for (i=0; _str[i]; i++ ){} \
        _str[i] = '"' ; \
        _str[i+1] = '\0' ; \
    } \
    _str = tmp_str ; \
}

#define SET_PARM_STR(flag, name, sub, field) { \
        char *quote_field;  \
	if (((sub)->options & flag) && (sub)->field) { \
            QUOTE_STR((sub)->field,quote_field) ; \
            if (quote_field != NULL ){ \
	        fprintf(parmfp, "%s=%s\n", name, quote_field); \
                free(quote_field) ;  \
            } \
	} else if ((sub)->delOptions & flag) { \
	    fprintf(parmfp, "%s=SUB_RESET\n", name); \
	} \
    }

#define SET_PARM_BOOL(flag, name, sub) { \
	if ((sub)->options & flag) { \
	    fprintf(parmfp, "%s=Y\n", name); \
	} else if ((sub)->delOptions & flag) { \
	    fprintf(parmfp, "%s=SUB_RESET\n", name); \
	} \
    }

#define SET_PARM_INT(flag, name, sub, field) { \
	if ((sub)->options & flag) { \
	    fprintf(parmfp, "%s=%d\n", name, (int) (sub)->field); \
	} else if ((sub)->delOptions & flag) { \
	    fprintf(parmfp, "%s=SUB_RESET\n", name); \
	} \
    }

#define SET_PARM_STR_2(flag, name, sub, field) { \
       char *quote_field;  \
       if (((sub)->options2 & flag) && (sub)->field) { \
           QUOTE_STR((sub)->field,quote_field) ; \
           if (quote_field != NULL ){ \
               fprintf(parmfp, "%s=%s\n", name, quote_field); \
               free(quote_field) ;  \
           } \
       } else if ((sub)->delOptions2 & flag) { \
           fprintf(parmfp, "%s=SUB_RESET\n", name); \
       } \
    }

#define SET_PARM_BOOL_2(flag, name, sub) { \
       if ((sub)->options2 & flag) { \
           fprintf(parmfp, "%s=Y\n", name); \
       } else if ((sub)->delOptions2 & flag) { \
           fprintf(parmfp, "%s=SUB_RESET\n", name); \
       } \
    }

#define SET_PARM_INT_2(flag, name, sub, field) { \
	if ((sub)->options2 & flag) { \
	    fprintf(parmfp, "%s=%d\n", name, (int) (sub)->field); \
	} else if ((sub)->delOptions2 & flag) { \
	    fprintf(parmfp, "%s=SUB_RESET\n", name); \
	} \
    }

#define SET_PARM_NUMBER(name, field, delnum, defnum) { \
	    if (field == delnum) { \
		fprintf(parmfp, "%s=SUB_RESET\n", name); \
	    } else if (field != defnum) { \
		fprintf(parmfp, "%s=%d\n", name, (int) field); \
	    } \
	}



    sprintf (esub, "%s/%s", lsbParams[LSB_SERVERDIR].paramValue, ESUBNAME);
    if (stat(esub, &sbuf) < 0)
	return (0);


    sprintf(parmFile, "%s/.lsbsubparm.%d", LSTMPDIR, (int)getpid());

    if ((parmfp = fopen(parmFile, "w")) == NULL) {
	lsberrno = LSBE_SYS_CALL;
	return (-1);
    }

    chmod(parmFile, 0666);

    SET_PARM_STR(SUB_JOB_NAME, "LSB_SUB_JOB_NAME", jobSubReq, jobName);
    SET_PARM_STR(SUB_QUEUE, "LSB_SUB_QUEUE", jobSubReq, queue);
    SET_PARM_STR(SUB_IN_FILE, "LSB_SUB_IN_FILE", jobSubReq, inFile);
    SET_PARM_STR(SUB_OUT_FILE, "LSB_SUB_OUT_FILE", jobSubReq, outFile);
    SET_PARM_STR(SUB_ERR_FILE, "LSB_SUB_ERR_FILE", jobSubReq, errFile);
    SET_PARM_BOOL(SUB_EXCLUSIVE, "LSB_SUB_EXCLUSIVE", jobSubReq);
    SET_PARM_BOOL(SUB_NOTIFY_END, "LSB_SUB_NOTIFY_END", jobSubReq);
    SET_PARM_BOOL(SUB_NOTIFY_BEGIN, "LSB_SUB_NOTIFY_BEGIN", jobSubReq);
    SET_PARM_INT(SUB_CHKPNT_PERIOD, "LSB_SUB_CHKPNT_PERIOD", jobSubReq,
		 chkpntPeriod);
    SET_PARM_STR(SUB_CHKPNT_DIR, "LSB_SUB_CHKPNT_DIR", jobSubReq, chkpntDir);
    SET_PARM_BOOL(SUB_RESTART_FORCE, "LSB_SUB_RESTART_FORCE", jobSubReq);
    SET_PARM_BOOL(SUB_RESTART, "LSB_SUB_RESTART", jobSubReq);
    SET_PARM_BOOL(SUB_RERUNNABLE, "LSB_SUB_RERUNNABLE", jobSubReq);
    SET_PARM_BOOL(SUB_WINDOW_SIG, "LSB_SUB_WINDOW_SIG", jobSubReq);
    SET_PARM_STR(SUB_HOST_SPEC, "LSB_SUB_HOST_SPEC", jobSubReq, hostSpec);
    SET_PARM_STR(SUB_DEPEND_COND, "LSB_SUB_DEPEND_COND", jobSubReq,
		 dependCond);
    SET_PARM_STR(SUB_RES_REQ, "LSB_SUB_RES_REQ", jobSubReq, resReq);
    SET_PARM_STR(SUB_PRE_EXEC, "LSB_SUB_PRE_EXEC", jobSubReq, preExecCmd);
    SET_PARM_STR(SUB_POST_EXEC, "LSB_SUB_POST_EXEC", jobSubReq, postExecCmd);
    SET_PARM_STR(SUB_LOGIN_SHELL, "LSB_SUB_LOGIN_SHELL", jobSubReq,
		 loginShell);
    SET_PARM_STR(SUB_MAIL_USER, "LSB_SUB_MAIL_USER", jobSubReq, mailUser);
    SET_PARM_BOOL(SUB_MODIFY, "LSB_SUB_MODIFY", jobSubReq);
    SET_PARM_BOOL(SUB_MODIFY_ONCE, "LSB_SUB_MODIFY_ONCE", jobSubReq);
    SET_PARM_STR(SUB_PROJECT_NAME, "LSB_SUB_PROJECT_NAME", jobSubReq,
		 projectName);
    SET_PARM_BOOL(SUB_INTERACTIVE, "LSB_SUB_INTERACTIVE", jobSubReq);
    SET_PARM_BOOL(SUB_PTY, "LSB_SUB_PTY", jobSubReq);
    SET_PARM_BOOL(SUB_PTY_SHELL, "LSB_SUB_PTY_SHELL", jobSubReq);

    SET_PARM_BOOL_2(SUB2_HOLD, "LSB_SUB_HOLD", jobSubReq);
    SET_PARM_INT_2(SUB2_JOB_PRIORITY, "LSB_SUB2_JOB_PRIORITY", jobSubReq,
                 userPriority);
    SET_PARM_STR_2(SUB2_IN_FILE_SPOOL, "LSB_SUB2_IN_FILE_SPOOL", jobSubReq,
                 inFile);
    SET_PARM_BOOL_2(SUB2_JOB_CMD_SPOOL, "LSB_SUB2_JOB_CMD_SPOOL", jobSubReq);

    ls_readconfenv(myParams, NULL);


    if (myParams[LSB_SUB_COMMANDNAME].paramValue) {
       int cmdSize, cmdNameSize, tmpCnt;

       int start=0;

       cmdSize = strlen(jobSubReq->command);
       cmdNameSize = 0;

       if ( strstr(jobSubReq->command, SCRIPT_WORD) != NULL)
       {
               int i=0, found;
	       char *p , *q;


               while (jobSubReq->command[i] != '\n')
                       i ++;

               start = i + 1;
               cmdNameSize =i + 1;

	       do {

		  found = 0;


                   while ( (cmdNameSize < cmdSize)  &&
                          (isspace(jobSubReq->command[cmdNameSize]) ||
                           (jobSubReq->command[cmdNameSize] == '\n')) )
		  {
                        start ++;
                        cmdNameSize++;
			found = 1;
                  }


                  while ( (cmdNameSize < cmdSize) &&
                          (jobSubReq->command[start]=='#') )
                  {
                       while ( (cmdNameSize < cmdSize) &&
                               (jobSubReq->command[start] != '\n') )
                       {
                               start ++;
                               cmdNameSize ++;
                       }
                       found = 1;
                  }
               } while (found);


               p = strstr(&jobSubReq->command[start], SCRIPT_WORD_END);
               q = &jobSubReq->command[start];
               while (q != p) {
                  q ++;

                  if (*q == '\n') break;
               }

               if (q == p) start = cmdNameSize = cmdSize;
       }


       while (cmdNameSize < cmdSize)
       {
         if ( isspace(jobSubReq->command[cmdNameSize]) )
	   break;
         cmdNameSize++;
       }


       fprintf(parmfp, "%s=\"", "LSB_SUB_COMMANDNAME");
       for ( tmpCnt = start; tmpCnt < cmdNameSize; tmpCnt++)
       {
         if ( jobSubReq->command[tmpCnt] == '"' ||
	      jobSubReq->command[tmpCnt] == '\\' )
	   fprintf(parmfp, "\\");

         fprintf(parmfp, "%c", jobSubReq->command[tmpCnt]);
       }
       fprintf(parmfp, "\"\n");


       do {
	   char *x=unwrapCommandLine(jobSubReq->command);
	   fprintf(parmfp,"LSB_SUB_COMMAND_LINE=\"%s\"\n",x);
       } while(0);
    }

    if (jobSubReq->options & SUB_HOST) {
	char askedHosts[MAXLINELEN];

	askedHosts[0] = '\0';
	for (i = 0; i < jobSubReq->numAskedHosts; i++) {
	    strcat(askedHosts, jobSubReq->askedHosts[i]);
	    strcat(askedHosts, " ");
	}

	if (askedHosts[0] != '\0') {
	    fprintf(parmfp, "LSB_SUB_HOSTS=\"%s\"\n", askedHosts);
	}
    } else if (jobSubReq->delOptions & SUB_HOST) {
	fprintf(parmfp, "LSB_SUB_HOSTS=SUB_RESET\n");
    }

    for (i = 0; i < LSF_RLIM_NLIMITS; i++) {
	SET_PARM_NUMBER(subRLimitName[i], jobSubReq->rLimits[i],
			DELETE_NUMBER, DEFAULT_RLIMIT);

    }

    SET_PARM_NUMBER("LSB_SUB_NUM_PROCESSORS",
                     (jobSubReq->numProcessors?jobSubReq->numProcessors:DEFAULT_NUMPRO),
                   DEL_NUMPRO, DEFAULT_NUMPRO);
    SET_PARM_NUMBER("LSB_SUB_MAX_NUM_PROCESSORS",
                    (jobSubReq->maxNumProcessors? jobSubReq->maxNumProcessors:DEFAULT_NUMPRO),
                   DEL_NUMPRO, DEFAULT_NUMPRO);
    SET_PARM_NUMBER("LSB_SUB_BEGIN_TIME", jobSubReq->beginTime,
		    DELETE_NUMBER, 0);
    SET_PARM_NUMBER("LSB_SUB_TERM_TIME", jobSubReq->termTime,
		    DELETE_NUMBER, 0);

    if (jobSubReq->delOptions & SUB_OTHER_FILES) {
	fprintf(parmfp, "LSB_SUB_OTHER_FILES=SUB_RESET\n");
    } else if (jobSubReq->options & SUB_OTHER_FILES) {
	char str[MAXLINELEN];

	fprintf(parmfp, "LSB_SUB_OTHER_FILES=%d\n",jobSubReq->nxf);

	for (i = 0; i < jobSubReq->nxf; i++) {
	    sprintf(str, "%s ", jobSubReq->xf[i].subFn);

	    if (jobSubReq->xf[i].options & XF_OP_SUB2EXEC)
		strcat(str, ">");
	    else if (jobSubReq->xf[i].options & XF_OP_SUB2EXEC_APPEND)
		strcat(str, ">>");
	    if (jobSubReq->xf[i].options & XF_OP_EXEC2SUB_APPEND)
		strcat(str, "<<");
	    else if (jobSubReq->xf[i].options & XF_OP_EXEC2SUB)
		strcat(str, "<");

	    sprintf(str, "%s %s", str, jobSubReq->xf[i].execFn);
	    fprintf(parmfp, "LSB_SUB_OTHER_FILES_%d=\"%s\"\n", i, str);
	}
    }

    if(additionEsubInfo!=NULL) {
	fprintf(parmfp,"LSB_SUB_ADDITIONAL=\"%s\"\n",additionEsubInfo);
    }

    fclose(parmfp);

    putEnv("LSB_SUB_ABORT_VALUE", "97");
    putEnv("LSB_SUB_PARM_FILE", parmFile);

    cc = 0;
    esub_method = getenv("LSB_ESUB_METHOD");

    parmDeltaFile = getenv("LSB_SUB_MODIFY_FILE");
    envDeltaFile = getenv("LSB_SUB_MODIFY_ENVFILE");

    aggregated_parms = (char *)malloc(1);
    if (aggregated_parms) aggregated_parms[0] = '\0';
    aggregated_parms_size = 0;

    aggregated_envs = (char *)malloc(1);
    if (aggregated_envs) aggregated_envs[0] = '\0';
    aggregated_envs_size = 0;

    if (!aggregated_parms || !aggregated_envs) {
        lsberrno = LSBE_NO_MEM;
        cc = -1;
        goto cleanup_and_exit;
    }

    if(!esub_method) {
        struct config_param esubParam[] = { {"LSB_ESUB_METHOD", NULL}, {NULL, NULL} };
        if (ls_readconfenv(esubParam, NULL) == 0 && esubParam[0].paramValue) {
            setenv("LSB_ESUB_METHOD", esubParam[0].paramValue, 1);
            esub_method = esubParam[0].paramValue;
        }
    }

    snprintf(esub_path, sizeof(esub_path), "%s/" ESUBNAME, lsbParams[LSB_SERVERDIR].paramValue);
    if (is_executable(esub_path)) {
        cc = runEsub_(ed, NULL);
        if (cc >= 0) {
            if (aggregateFileContent(parmDeltaFile, &aggregated_parms, &aggregated_parms_size) != 0) cc = -1;
        }
        if (cc >= 0) {
            if (aggregateFileContent(envDeltaFile, &aggregated_envs, &aggregated_envs_size) != 0) cc = -1;
        }
        if (cc < 0) goto handle_esub_error;
    }

    if (esub_method && strlen(esub_method) > 0) {
        char app_list[MAX_ESUB_LIST][MAX_APP_NAME];
        int app_count = split_app_list(esub_method, app_list, MAX_ESUB_LIST);
        int i;
        for (i = 0; i < app_count; ++i) {
            snprintf(esub_path, sizeof(esub_path), "%s/" ESUBNAME ".%s", lsbParams[LSB_SERVERDIR].paramValue, app_list[i]);            
            if (is_executable(esub_path)) {
                cc = runEsub_(ed, esub_path);
                if (cc >= 0) {
                    if (aggregateFileContent(parmDeltaFile, &aggregated_parms, &aggregated_parms_size) != 0) cc = -1;
                }
                if (cc >= 0) {
                    if (aggregateFileContent(envDeltaFile, &aggregated_envs, &aggregated_envs_size) != 0) cc = -1;
                }
                if (cc < 0) goto handle_esub_error;
            }
        }
    }

    if (additionEsubInfo && strlen(additionEsubInfo) > 0) {
        char app_list[MAX_ESUB_LIST][MAX_APP_NAME];
        int app_count = split_app_list(additionEsubInfo, app_list, MAX_ESUB_LIST);
        int i;
        for (i = 0; i < app_count; ++i) {
            snprintf(esub_path, sizeof(esub_path), "%s/" ESUBNAME ".%s", lsbParams[LSB_SERVERDIR].paramValue, app_list[i]);
            if (is_executable(esub_path)) {
                cc = runEsub_(ed, esub_path);
                if (cc >= 0) {
                    if (aggregateFileContent(parmDeltaFile, &aggregated_parms, &aggregated_parms_size) != 0) cc = -1;
                }
                if (cc >= 0) {
                    if (aggregateFileContent(envDeltaFile, &aggregated_envs, &aggregated_envs_size) != 0) cc = -1;
                }
                if (cc < 0) goto handle_esub_error;
            }
        }
    }

    goto cleanup_and_exit;

handle_esub_error:
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: runEsub_() or aggregation failed, cc=%d: %M", fname, cc);
    if (cc == -2) {
        lsberrno = LSBE_ESUB_ABORT;
    }
    goto cleanup_and_exit;

cleanup_and_exit:
    if (aggregated_parms_size > 0) {
        FILE *final_parm_fp = fopen(parmDeltaFile, "w");
        if (final_parm_fp) {
            fwrite(aggregated_parms, 1, aggregated_parms_size, final_parm_fp);
            fclose(final_parm_fp);
        } else {
             ls_syslog(LOG_ERR, "%s: Failed to write aggregated parm file %s: %m", fname, parmDeltaFile);
             if (cc >= 0) cc = -1;
        }
    }
    if (aggregated_envs_size > 0) {
        FILE *final_env_fp = fopen(envDeltaFile, "w");
        if (final_env_fp) {
            fwrite(aggregated_envs, 1, aggregated_envs_size, final_env_fp);
            fclose(final_env_fp);
        } else {
            ls_syslog(LOG_ERR, "%s: Failed to write aggregated env file %s: %m", fname, envDeltaFile);
            if (cc >= 0) cc = -1;
        }
    }
    
    free(aggregated_parms);
    free(aggregated_envs);
    unlink(parmFile);
    
    return (cc < 0) ? -1 : 0;

}

static int
readOptFile(char *filename, char *childLine)
{
    char *p, *sp, *sline, *start;
    FILE *fp;

    if ((fp = fopen(filename, "r")) == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, "readOptFile",
		  "fopen", filename);
        return(-1);
    }

    while ((sline = getNextLine_(fp, FALSE)) != NULL) {
        sp = sline;
        SKIPSPACE(sp);

        if (*sp != '#')
            continue;
        if ((p = strstr(sp, "\\n")) != NULL)
            *p = '\0';
	++sp;
	SKIPSPACE(sp);

	if (strncmp (sp, "BSUB", 4) == 0) {
	    sp += 4;
            start = sp;
            SKIPSPACE(sp);
            if (*sp != '-')
                continue;
            else {
                if ( (sp = strstr( sp, "#")) != NULL )

	            *sp = '\0';
                sp = sline;
		if ( childLine[0] == '\0' )
		    strcpy(childLine, sline);
		else
		    strncat (childLine, start, strlen(start));
		continue;
             }
        } else
	        continue;
    }

    fclose(fp);
    return(0);
}

int
bExceptionTabInit(void)
{
    bExceptionTab = calloc(1, sizeof(hTab));
    h_initTab_(bExceptionTab, 3);

    return 0;
}

static bException_t *
bExceptionCreate(void)
{
    bException_t   *exception;

    exception = calloc(1, sizeof(bException_t));

    return exception;
}

int
mySubUsage_(void *args)
{
    struct args {
        int argc;
        char **argv;
    } *myArgs;
    myArgs = (struct args *) args;
    subUsage_(myArgs->argc, myArgs->argv);
    return 0;
}

void
lsb_throw(const char *exceptionName, void *extra)
{
    bException_t *exception;
    hEnt *hEnt;

    if (! exceptionName || *exceptionName == '\0')
	return;

    if (! bExceptionTab)
	return;

    hEnt = h_getEnt_(bExceptionTab, (char *) exceptionName);

    if (! hEnt)
        return;

    exception = (bException_t *) hEnt->hData;

    if (! exception || ! exception->handler)
	return;
    else
	(*exception->handler)(extra);

}

int
lsb_catch(const char *exceptionName, int (*exceptionHandler)(void *))
{
    hEnt           *hEnt;
    bException_t   *exception;

    if (! exceptionName || *exceptionName == '\0') {
	lsberrno = LSBE_BAD_ARG;
	return -1;
    }

    if (! bExceptionTab) {
	lsberrno = LSBE_LSLIB;
	return -1;
    }

    hEnt = h_getEnt_(bExceptionTab, (char *) exceptionName);
    if (! hEnt) {
	hEnt = h_addEnt_(bExceptionTab, (char *) exceptionName, NULL);
	hEnt->hData = bExceptionCreate();
    }

    exception = hEnt->hData;
    if (! exception->name)
	 exception->name = strdup(exceptionName);
    exception->handler = exceptionHandler;

    return 0;
}


#define I18N_MSG(msgid,msg) _i18n_msg_get(ls_catd,NL_SETN,msgid,msg)

#define MSG_BAD_ENVAR2s     I18N_MSG(5550,                                  \
				     "%s: Bad parameter variable name(%s) " \
				     "read from $LSB_SUB_MODIFY_FILE, the " \
				     "setting will be ignored.")
                                     /* catgets 5550 */

#define MSG_BAD_INTVAL3s     I18N_MSG(5551,                                 \
				      "%s: Bad value(%s) read from "        \
				      "$LSB_SUB_MODIFY_FILE for parameter " \
				      "%s, the setting will be ignored. "   \
				      "The value of this parameter can "    \
				      "only be SUB_RESET or an integer.")
                                     /* catgets 5551 */

#define MSG_ALLOC_MEMF_IN_XFs I18N_MSG(5552,                                \
				       "%s: Memory allocate failed for "    \
				       "file transfer request.")
                                     /* catgets 5552 */

#define MSG_BAD_XF_OP2s     I18N_MSG(5553,                                  \
				     "%s: unknown file transfer operator"   \
				     " %s, this transfer request will be"   \
				     " ignored.")
                                     /* catgets 5553 */

#define MSG_BAD_BOOLVAL3s    I18N_MSG(5554,                                 \
				      "%s: Bad value(%s) read from "        \
				      "$LSB_SUB_MODIFY_FILE for parameter"  \
				      " %s, the setting will be ignored. "  \
				      "The value of this parameter can "    \
				      "only be SUB_RESET or 'Y'.")
                                    /* catgets 5554 */

#define MSG_VAL_RESET_INT2s   I18N_MSG(5555,                                \
				       "%s: The value of %s can only be "   \
				       "SUB_RESET or an integer.")
                                    /* catgets 5554 */

#define MSG_BAD_ENVAL3s     I18N_MSG(5556,                                  \
				     "%s: Bad value(%s) read from "         \
				     "$LSB_SUB_MODIFY_FILE for parameter"   \
				     " %s, the setting will be ignored.")
                                    /* catgets 5556 */

#define MSG_WARN_NULLVAL2s  I18N_MSG(5557,                                  \
				     "%s: The value of parameter %s is "    \
				     "empty, the setting will be ignored.")
                                    /* catgets 5557 */
void makeCleanToRunEsub()
{
    char parmDeltaFile[MAXPATHLEN];
    char envDeltaFile[MAXPATHLEN];
    struct stat stbuf;

    sprintf(parmDeltaFile, "%s/.lsbsubdeltaparm.%d.%d",
	    LSTMPDIR, (int)getpid(), (int)getuid());
    sprintf(envDeltaFile, "%s/.lsbsubdeltaenv.%d.%d",
	    LSTMPDIR, (int)getpid(), (int)getuid());

    if(stat(parmDeltaFile,&stbuf)!=ENOENT) {
	unlink(parmDeltaFile);
    }

    if(stat(envDeltaFile,&stbuf)!=ENOENT) {
	unlink(envDeltaFile);
    }

    putEnv("LSB_SUB_MODIFY_FILE",parmDeltaFile);
    putEnv("LSB_SUB_MODIFY_ENVFILE",envDeltaFile);
}

void modifyJobInformation(struct submit *jobSubReq)
{
    static char fname[]="modifyJobInforation";
    char parmDeltaFile[MAXPATHLEN];
    char envDeltaFile[MAXPATHLEN];
    int validKey,v;
    char *sValue;

#define INTPARM   1
#define STRPARM   2
#define NUMPARM   3
#define BOOLPARM  4
#define INT2PARM  5
#define STR2PARM  6
#define BOOL2PARM 7
#define RLIMPARM  8
#define STRSPARM  9

#define FIELD_OFFSET(type,field) (long)(&(((struct type *)0)->field))
#define FIELD_PTR_PTR(base,offset) (((char *)base)+offset)

    FILE *fp;
    int  lineNum;
    char *line=NULL,*key;
    static struct {
	char *parmName;
	int  parmType;
	long fieldOffset;
	long subOption;
    } jobSubReqParams[]=
	{
	    {"LSB_SUB_JOB_NAME",STRPARM,
	     FIELD_OFFSET(submit,jobName),SUB_JOB_NAME},
	    {"LSB_SUB_QUEUE",STRPARM,
	     FIELD_OFFSET(submit,queue),SUB_QUEUE},
	    {"LSB_SUB_IN_FILE",STRPARM,
	     FIELD_OFFSET(submit,inFile),SUB_IN_FILE},
	    {"LSB_SUB_OUT_FILE",STRPARM,
	     FIELD_OFFSET(submit,outFile),SUB_OUT_FILE},
	    {"LSB_SUB_ERR_FILE",STRPARM,
	     FIELD_OFFSET(submit,errFile),SUB_ERR_FILE},
	    {"LSB_SUB_EXCLUSIVE",BOOLPARM,
	     -1,SUB_EXCLUSIVE},
	    {"LSB_SUB_NOTIFY_END",BOOLPARM,
	     -1,SUB_NOTIFY_END},
	    {"LSB_SUB_OUT_NOTIFY_BEGIN",BOOLPARM,
	     -1,SUB_NOTIFY_BEGIN},
	    {"LSB_SUB_CHKPNT_PERIOD",INTPARM,
	     FIELD_OFFSET(submit,chkpntPeriod),SUB_CHKPNT_PERIOD},
	    {"LSB_SUB_CHKPNT_DIR",STRPARM,
	     FIELD_OFFSET(submit,chkpntDir),SUB_CHKPNT_DIR},
	    {"LSB_SUB_RESTART_FORCE",BOOLPARM,
	     -1,SUB_RESTART_FORCE},
	    {"LSB_SUB_RESTART",BOOLPARM,
	     -1,SUB_RESTART},
	    {"LSB_SUB_RERUNNABLE",BOOLPARM,
	     -1,SUB_RERUNNABLE},
	    {"LSB_SUB_WINDOW_SIG",BOOLPARM,
	     -1,SUB_WINDOW_SIG},
	    {"LSB_SUB_HOST_SPEC",STRPARM,
	     FIELD_OFFSET(submit,hostSpec),SUB_HOST_SPEC},
	    {"LSB_SUB_DEPEND_COND",STRPARM,
	     FIELD_OFFSET(submit,dependCond),SUB_DEPEND_COND},
	    {"LSB_SUB_RES_REQ",STRPARM,
	     FIELD_OFFSET(submit,resReq),SUB_RES_REQ},
	    {"LSB_SUB_PRE_EXEC",STRPARM,
	     FIELD_OFFSET(submit,preExecCmd),SUB_PRE_EXEC},
        {"LSB_SUB_POST_EXEC",STRPARM,
	     FIELD_OFFSET(submit,postExecCmd),SUB_POST_EXEC},
	    {"LSB_SUB_LOGIN_SHELL",STRPARM,
	     FIELD_OFFSET(submit,loginShell),SUB_LOGIN_SHELL},
	    {"LSB_SUB_MAIL_USER",STRPARM,
	     FIELD_OFFSET(submit,mailUser),SUB_MAIL_USER},
	    {"LSB_SUB_MODIFY",BOOLPARM,
	     -1,SUB_MODIFY},
	    {"LSB_SUB_MODIFY_ONCE",BOOLPARM,
	     -1,SUB_MODIFY_ONCE},
	    {"LSB_SUB_PROJECT_NAME",STRPARM,
	     FIELD_OFFSET(submit,projectName),SUB_PROJECT_NAME},
	    {"LSB_SUB_INTERACTIVE",BOOLPARM,
	     -1,SUB_INTERACTIVE},
	    {"LSB_SUB_PTY",BOOLPARM,-1,SUB_PTY},
	    {"LSB_SUB_PTY_SHELL",BOOLPARM,-1,SUB_PTY_SHELL},
	    {"LSB_SUB_HOSTS",STRSPARM,
	     FIELD_OFFSET(submit,askedHosts),0},
	    {"LSB_SUB_HOLD",BOOL2PARM,-1,SUB2_HOLD},
	    {"LSB_SUB2_JOB_PRIORITY",INT2PARM,
	     FIELD_OFFSET(submit,userPriority),SUB2_JOB_PRIORITY},
	    {"LSB_SUB_RLIMIT_CPU",RLIMPARM,0,-1},
	    {"LSB_SUB_RLIMIT_FSIZE",RLIMPARM,1,-1},
	    {"LSB_SUB_RLIMIT_DATA",RLIMPARM,2,-1},
	    {"LSB_SUB_RLIMIT_STACK",RLIMPARM,3,-1},
	    {"LSB_SUB_RLIMIT_CORE",RLIMPARM,4,-1},
	    {"LSB_SUB_RLIMIT_RSS",RLIMPARM,5,-1},
	    {"LSB_SUB_RLIMIT_NOFILE",RLIMPARM,6,-1},
	    {"LSB_SUB_RLIMIT_OPEN_MAX",RLIMPARM,7,-1},
	    {"LSB_SUB_RLIMIT_SWAP",RLIMPARM,8,-1},
	    {"LSB_SUB_RLIMIT_RUN",RLIMPARM,9,-1},
	    {"LSB_SUB_RLIMIT_PROCESS",RLIMPARM,10,-1},
	    {"LSB_SUB_NUM_PROCESSORS",NUMPARM,
	     FIELD_OFFSET(submit,numProcessors),DEFAULT_NUMPRO},
	    {"LSB_SUB_MAX_NUM_PROCESSORS",NUMPARM,
	     FIELD_OFFSET(submit,maxNumProcessors),DEFAULT_NUMPRO},
	    {"LSB_SUB_BEGIN_TIME",NUMPARM,
	     FIELD_OFFSET(submit,beginTime),0},
	    {"LSB_SUB_TERM_TIME",NUMPARM,
	     FIELD_OFFSET(submit,termTime),0},
	    {"LSB_SUB_COMMAND_LINE",STRPARM,
	     FIELD_OFFSET(submit,command),0},
	    {NULL,0,0,0}
	};

    sprintf(parmDeltaFile, "%s/.lsbsubdeltaparm.%d.%d",
	    LSTMPDIR, (int)getpid(), (int)getuid());
    sprintf(envDeltaFile, "%s/.lsbsubdeltaenv.%d.%d",
	    LSTMPDIR, (int)getpid(), (int)getuid());

    if(access(parmDeltaFile,R_OK)==F_OK) {
	fp=fopen(parmDeltaFile,"r");
	lineNum=0;

	while((line=getNextLineC_(fp,&lineNum,TRUE))!=NULL) {
	    int i,j;

	    key=getNextWordSet(&line," \t=!@#$%^&*()");

	    while(*line!='=') line++;
	    line++;
	    while(isspace((int)*line)) line++;

	    validKey=0;


	    if (strncmp(key,"LSB_SUB_OTHER_FILES",
			strlen("LSB_SUB_OTHER_FILES"))==0) {
		processXFReq(key,line,jobSubReq);
		continue;
	    }

	    for(i=0;jobSubReqParams[i].parmName;i++) {
		if(strcmp(key,jobSubReqParams[i].parmName)==0) {
		    validKey=1;

		    switch (jobSubReqParams[i].parmType) {
			case STRPARM:
			    if(checkEmptyString(line)) {
				ls_syslog(LOG_WARNING,MSG_WARN_NULLVAL2s,
					  fname,key);
				break;
			    }

			    if ((strcmp(key,"LSB_SUB_COMMAND_LINE")==0) &&
				(jobSubReq->options & SUB_RESTART)) {

				break;
			    }

			    if(stringIsToken(line,"SUB_RESET")) {
				jobSubReq->options &= ~jobSubReqParams[i].subOption;
				jobSubReq->delOptions |= jobSubReqParams[i].subOption;
			    }
			    else {

				sValue=extractStringValue(line);
				if(sValue==NULL) {
				    ls_syslog(LOG_WARNING,MSG_BAD_ENVAL3s,
					      fname,line,key);
				    break;
				}

				if(strcmp(key,"LSB_SUB_COMMAND_LINE")!=0) {
				    *(char **)(FIELD_PTR_PTR(
						   jobSubReq,
						   jobSubReqParams[i].fieldOffset))
					=putstr_(sValue);
				}
				else {
				    *(char **)(FIELD_PTR_PTR(
						   jobSubReq,
						   jobSubReqParams[i].fieldOffset))
					=wrapCommandLine(sValue);
				    if(jobSubReq->options & SUB_MODIFY) {
					jobSubReq->newCommand=jobSubReq->command;
					jobSubReq->options2 |= SUB2_MODIFY_CMD;
					jobSubReq->delOptions2 &= ~SUB2_MODIFY_CMD;
				    }
				}

				jobSubReq->options |= jobSubReqParams[i].subOption;
				jobSubReq->delOptions &= ~jobSubReqParams[i].subOption;
			    }
			    break;
			case INTPARM:
			    if(stringIsToken(line,"SUB_RESET")) {
				jobSubReq->options &= ~jobSubReqParams[i].subOption;
				jobSubReq->delOptions |= jobSubReqParams[i].subOption;
			    }
			    else {
				if (!stringIsDigitNumber(line)) {
				    ls_syslog(LOG_WARNING,MSG_BAD_INTVAL3s,
					      fname,line,key);
				    break;
				}

				v=atoi(line);
				*(int *)(FIELD_PTR_PTR(
					     jobSubReq,
					     jobSubReqParams[i].fieldOffset))
				    =v;
				jobSubReq->options |= jobSubReqParams[i].subOption;
				jobSubReq->delOptions &= ~jobSubReqParams[i].subOption;
			    }
			    break;
			case BOOLPARM:
			    if(stringIsToken(line,"Y")) {
				jobSubReq->options |= jobSubReqParams[i].subOption;
				jobSubReq->delOptions &= ~jobSubReqParams[i].subOption;
			    }
			    else if (stringIsToken(line,"SUB_RESET")){
				jobSubReq->options &= ~jobSubReqParams[i].subOption;
				jobSubReq->delOptions |= jobSubReqParams[i].subOption;
			    }
			    else {

				ls_syslog(LOG_WARNING,MSG_BAD_BOOLVAL3s,
					  fname,line,key);
			    }
			    break;
			case STR2PARM:
			    if(checkEmptyString(line)) {
				ls_syslog(LOG_WARNING,MSG_WARN_NULLVAL2s,
					  fname,key);
				break;
			    }

			    if(stringIsToken(line,"SUB_RESET")) {
				jobSubReq->options2 &= ~jobSubReqParams[i].subOption;
				jobSubReq->delOptions2 |= jobSubReqParams[i].subOption;
			    }
			    else {

				sValue=extractStringValue(line);
				if(sValue==NULL) {
				    ls_syslog(LOG_WARNING,MSG_BAD_ENVAL3s,
					      fname,line,key);
				    break;
				}

				*(char **)(FIELD_PTR_PTR(
					       jobSubReq,
					       jobSubReqParams[i].fieldOffset))
				    =putstr_(sValue);

				jobSubReq->options2 |= jobSubReqParams[i].subOption;
				jobSubReq->delOptions2 &= ~jobSubReqParams[i].subOption;
			    }
			    break;
			case INT2PARM:
			    if(stringIsToken(line,"SUB_RESET")) {
				jobSubReq->options2 &= ~jobSubReqParams[i].subOption;
				jobSubReq->delOptions2 |= jobSubReqParams[i].subOption;
			    }
			    else {
				if (!stringIsDigitNumber(line)) {
				    ls_syslog(LOG_WARNING,MSG_BAD_INTVAL3s,
					      fname,line,key);
				    break;
				}

				v=atoi(line);
				*(int *)(FIELD_PTR_PTR(
					     jobSubReq,
					     jobSubReqParams[i].fieldOffset))
				    =v;
				jobSubReq->options2 |= jobSubReqParams[i].subOption;
				jobSubReq->delOptions2 &= ~jobSubReqParams[i].subOption;
			    }
			    break;
			case BOOL2PARM:
			    if(stringIsToken(line,"Y")) {
				jobSubReq->options2 |= jobSubReqParams[i].subOption;
				jobSubReq->delOptions2 &= ~jobSubReqParams[i].subOption;
			    }
			    else if (stringIsToken(line,"SUB_RESET")){
				jobSubReq->options2 &= ~jobSubReqParams[i].subOption;
				jobSubReq->delOptions2 |= jobSubReqParams[i].subOption;
			    }
			    else {

				ls_syslog(LOG_WARNING,MSG_BAD_BOOLVAL3s,
					  fname,line,key);
			    }
			    break;
			case NUMPARM:
			    if(stringIsToken(line,"SUB_RESET")) {
				*(int *)(FIELD_PTR_PTR(
					     jobSubReq,
					     jobSubReqParams[i].fieldOffset))=
				    jobSubReqParams[i].subOption;
			    }
			    else {
				if (!stringIsDigitNumber(line)) {
				    ls_syslog(LOG_WARNING,MSG_BAD_INTVAL3s,
					      fname,line,key);
				    break;
				}

				v=atoi(line);
				*(int *)(FIELD_PTR_PTR(
					     jobSubReq,
					     jobSubReqParams[i].fieldOffset))
				    =v;
			    }

			    if (jobSubReq->maxNumProcessors<jobSubReq->numProcessors) {
				jobSubReq->maxNumProcessors=jobSubReq->numProcessors;
			    }

			    break;
			case RLIMPARM:
			    j=jobSubReqParams[i].fieldOffset;
			    if(stringIsToken(line,"SUB_RESET")) {
				jobSubReq->rLimits[j]=DELETE_NUMBER;
			    }
			    else {
				if (!stringIsDigitNumber(line)) {
				    ls_syslog(LOG_WARNING,MSG_BAD_INTVAL3s,
					      fname,line,key);
				    break;
				}

				v=atoi(line);
				jobSubReq->rLimits[j]=v;
			    }
			    break;
			case STRSPARM:
			    if(checkEmptyString(line)) {
				ls_syslog(LOG_WARNING,MSG_WARN_NULLVAL2s,
					  fname,key);
				break;
			    }

			    sValue=extractStringValue(line);
			    if(sValue==NULL) {
				ls_syslog(LOG_WARNING,MSG_BAD_ENVAL3s,
					  fname,line,key);
				break;
			    }

			    if (strcmp(key,"LSB_SUB_HOSTS")==0) {
				int badIdx;

				if(getAskedHosts_(sValue,
						  &jobSubReq->askedHosts,
						  &jobSubReq->numAskedHosts,
						  &badIdx,FALSE)<0) {
				    jobSubReq->options &= ~SUB_HOST;
				    ls_syslog(LOG_WARNING,ls_sysmsg());
				}
				else {
				    jobSubReq->options |= SUB_HOST;
				}
			    }
			    break;
			default:
			    ls_syslog(LOG_WARNING,MSG_BAD_ENVAR2s,
				      fname,key);
			    break;
		    }
		    break;
		}
	    }

	    if (!validKey) {
		ls_syslog(LOG_WARNING,MSG_BAD_ENVAR2s,
			  fname,key);
	    }
	}
	fclose(fp);

	unlink(parmDeltaFile);
    }

    compactXFReq(jobSubReq);

    if(access(envDeltaFile,R_OK)==F_OK) {
	fp=fopen(envDeltaFile,"r");
	lineNum=0;

	while((line=getNextLineC_(fp,&lineNum,TRUE))!=NULL) {

	    key=getNextWordSet(&line," \t =!@#$%^&*()");
	    while(*line!='=') line++;

	    line++;

	    putEnv(key,getNextValueQ_(&line,'"','"'));
	}
	fclose(fp);
	unlink(envDeltaFile);
    }
}

char *unwrapCommandLine(char *commandLine)
{
    char *sp = NULL;
    static char *jobDespBuf=NULL;
    static char *lineStrBuf=NULL;
    char *jobdesp=NULL;
    char *p1,*p2;
    int   hasNonSpaceC=0;

    FREEUP(lineStrBuf);
    FREEUP(jobDespBuf);
    lineStrBuf=putstr_(commandLine);
    if (!lineStrBuf) {
       lsberrno = LSBE_NO_MEM;
       return NULL;
    }
    jobdesp=lineStrBuf;
    sp=(char *)strstr(jobdesp,"SCRIPT_\n");
    if(sp==NULL) {
	jobDespBuf=putstr_(jobdesp);
	return &jobDespBuf[0];
    }

    jobdesp=sp+strlen("SCRIPT_\n");
    sp=NULL;
    sp=strstr(jobdesp,"SCRIPT_\n");
    if (sp==NULL) {
	jobDespBuf=putstr_(jobdesp);
	return &jobDespBuf[0];
    }
    while(*sp!='\n') sp--;
    sp++;
    *sp='\0';

    jobDespBuf=putstr_(jobdesp);
    p1=NULL;p2=jobDespBuf;hasNonSpaceC=0;
    while(*p2) {
	if(*p2=='\n') {
	    *p2=' ';
	    if(p1!=NULL) {
		if(hasNonSpaceC) {
		    *p1=';';
		}
		else *p1=' ';
	    }
	    p1=p2;hasNonSpaceC=0;
	}
	else if(!isspace((int)*p2)) hasNonSpaceC=1;
	p2++;
    }
    return &jobDespBuf[0];
}

char *wrapCommandLine(char *command)
{
    static char *szTmpShellCommands= "\n_USER_SCRIPT_\n) "
	"> $LSB_CHKFILENAME.shell\n"
	"chmod u+x $LSB_CHKFILENAME.shell\n"
	"$LSB_JOBFILENAME.shell\n"
	"saveExit=$?\n"
	"/bin/rm -f $LSB_JOBFILENAME.shell\n"
	"(exit $saveExit)\n";

    static char cmdString[MAXLINELEN*4];

    if(strchr(command,(int)'\n')==NULL) {
	strcpy(cmdString,command);
	return cmdString;
    }

    sprintf(cmdString,"(cat <<_USER_\\SCRIPT_\n%s\n%s",command,szTmpShellCommands);
    return cmdString;
}


void compactXFReq(struct submit *jobSubReq)
{
    int i,j;

    i=0; j=0;
    while(i<jobSubReq->nxf) {
	if (jobSubReq->xf[i].options!=0) {
	    if(i!=j) {
		memcpy(&(jobSubReq->xf[j]),&(jobSubReq->xf[i]),
		       sizeof(struct xFile));
	    }
	    j++;
	}
	i++;
    }
    jobSubReq->nxf=j;
}

int checkEmptyString(char *s)
{
    char *p=s;

    while(*p) {
	if(!isspace((int)*p)) return 0;
	p++;
    }

    return 1;
}

int stringIsToken(char *s,char *tok)
{
    char *s1=s;

    while(isspace((int)*s1)
	  && (*s1)) s1++;

    if(strncmp(s1,tok,strlen(tok))==0) {
	char *p=s1+strlen(tok);
	return checkEmptyString(p);
    }

    return 0;
}

int stringIsDigitNumber(char *s)
{
    char *s1=s;

    while(isspace((int)*s1)
	  && (*s1)) s1++;

    if(*s1==0x0) return 0;

    if(*s1=='0') {
	return checkEmptyString(s1+1);
    }

    while(isdigit((int)*s1)) s1++;

    return checkEmptyString(s1);
}

char *extractStringValue(char *line)
{
    char *p;
    static char sValue[MAXLINELEN];
    int i;

    p=line;
    while(isspace((int)*p) && (*p!=0x0)) p++;

    if (*p!='\"') {

	return NULL;
    }

    p++;i=0;
    while((*p!=0x0) && (*p!='\"')) {
	sValue[i]=*p;
	p++;i++;
    }

    if(*p==0) return NULL;

    p++;
    if(checkEmptyString(p)) {
	sValue[i]=0x0;
	return sValue;
    }

    return NULL;
}

int processXFReq(char *key,char *line,struct submit *jobSubReq)
{
    static char fname[]="processXFRequest";
    int v;
    char *sValue;

    if(strcmp(key,"LSB_SUB_OTHER_FILES")==0) {

	if (stringIsToken(line,"SUB_RESET")) {
	    free(jobSubReq->xf);
	    jobSubReq->xf=NULL;
	    jobSubReq->options &= ~SUB_OTHER_FILES;
	    jobSubReq->delOptions |= SUB_OTHER_FILES;
	    jobSubReq->nxf=0;
	}
	else {
	    struct xFile *p;

	    if (!stringIsDigitNumber(line)) {
		ls_syslog(LOG_WARNING,MSG_BAD_INTVAL3s,
			  fname,line,key);
		return -1;
	    }

	    v=atoi(line);
	    p=(struct xFile *)malloc(v*sizeof(struct xFile));

	    if(p==NULL) {

		ls_syslog(LOG_ERR,MSG_ALLOC_MEMF_IN_XFs,
			  fname);
		return -1;
	    }
	    else {
		free(jobSubReq->xf);
		jobSubReq->nxf=v;
		jobSubReq->options |= SUB_OTHER_FILES;
		jobSubReq->delOptions &= SUB_OTHER_FILES;
		jobSubReq->xf=p;
		memset(jobSubReq->xf,0x0,v*sizeof(struct xFile));
	    }
	}
    }
    else {

	char *sp=key+strlen("LSB_SUB_OTHER_FILES_");
	char xfSeq[32];

	xfSeq[0]=0x0;
	strncat(xfSeq,sp,sizeof(xfSeq)-2);

	if(!stringIsDigitNumber(xfSeq)) {

	    ls_syslog(LOG_WARNING,MSG_BAD_ENVAR2s,
		      fname,key);
	    return -1;
	}

	v=atoi(xfSeq);
	if (v<jobSubReq->nxf) {
	    char op[20];
	    char *txt,*srcf;

	    jobSubReq->xf[v].options=0;

	    sValue=extractStringValue(line);
	    if(sValue==NULL) {
		ls_syslog(LOG_WARNING,MSG_BAD_ENVAL3s,fname,
			  line,sValue);
		return -1;
	    }

	    txt=sValue;
	    while(isspace((int)*txt) && (*txt!=0x0)) txt++;

	    srcf=getNextWordSet(&txt," \t<>");

	    if(srcf==NULL) {
		ls_syslog(LOG_WARNING,MSG_BAD_ENVAL3s,fname,
			  line,sValue);
		return -1;
	    }

	    strcpy(jobSubReq->xf[v].subFn,srcf);
	    while(isspace((int)*txt) && (*txt!=0x0)) txt++;


	    op[0]=op[1]=op[2]='\000';
	    op[0]=*txt++;
	    if (!isspace((int)*txt)) op[1]=*txt++;

	    while(isspace((int)*txt) && (*txt!=0x0)) txt++;
	    strcpy(jobSubReq->xf[v].execFn,txt);

	    if(strcmp(op,"<")==0) {
		jobSubReq->xf[v].options |= XF_OP_EXEC2SUB;
	    }
	    else if(strcmp(op,"<<")==0) {
		jobSubReq->xf[v].options |= XF_OP_EXEC2SUB_APPEND;
		jobSubReq->xf[v].options |= XF_OP_EXEC2SUB;
	    }
	    else if((strcmp(op,"<>")==0)||
		    (strcmp(op,"><")==0)) {
		jobSubReq->xf[v].options |= XF_OP_EXEC2SUB;
		jobSubReq->xf[v].options |= XF_OP_SUB2EXEC;
	    }
	    else if(strcmp(op,">")==0) {
		jobSubReq->xf[v].options |= XF_OP_SUB2EXEC;
	    }
	    else if(strcmp(op,">>")==0) {
		jobSubReq->xf[v].options |= XF_OP_SUB2EXEC_APPEND;
		jobSubReq->xf[v].options |= XF_OP_SUB2EXEC;
	    }
	    else {

		ls_syslog(LOG_WARNING,MSG_BAD_XF_OP2s,
			  fname,op);
	    }
	}
	else {

	    ls_syslog(LOG_WARNING,MSG_BAD_ENVAL3s,fname,
		      line,key);
	}
    }

    return 0;
}
static void trimSpaces(char *str)
{
    char *ptr;

    if( !str || str[0] == '\0' ) {
        return;
    }


    while( isspace((int) *str) ) {
        for(ptr = str; *ptr; ptr++) {
            *ptr = ptr[1];
        }
    }


    ptr = str;
    while( *ptr ) {
        ptr++;
    }

    ptr--;
    while( ptr >= str && isspace((int) *ptr) ) {
        *ptr = '\0';
        ptr--;
    }
}

static int split_app_list(const char* str, char arr[][MAX_APP_NAME], int max_count) {
    int count = 0;
    char *tmp;
    char *token;
    int has_long_token = 0;
    
    if (!str) return 0;
    tmp = strdup(str);
    if (!tmp) return 0;
    
    token = strtok(tmp, " ");
    
    while (token && count < max_count) {
        size_t len = strlen(token);
        if (len > MAX_APP_NAME - 1) {
            if (!has_long_token) {
                has_long_token = 1;
                fprintf(stderr, "Warning: App name '%.*s...' exceeds max length %d, truncated to %d characters\n",
                        (int)len > 40 ? 40 : (int)len, token, MAX_APP_NAME, MAX_APP_NAME - 1);
            }
        }
        strncpy(arr[count], token, MAX_APP_NAME-1);
        arr[count][MAX_APP_NAME-1] = '\0';
        count++;
        
        if (count >= max_count) {
            /* Check if there are more tokens remaining */
            token = strtok(NULL, " ");
            if (token) {
                int skipped = 1;
                while ((token = strtok(NULL, " ")) != NULL) {
                    skipped++;
                }
                fprintf(stderr, "Warning: Too many apps specified, exceeding max count %d. %d app(s) skipped\n",
                        max_count, skipped);
            }
            break;
        } else {
            token = strtok(NULL, " ");
        }
    }
    
    if (count > 0) {
        int len0;
        int last;
        int lenLast;
        
        len0 = strlen(arr[0]);
        if (len0 > 0 && (arr[0][0] == '"' || arr[0][0] == '\'')) {
            memmove(arr[0], arr[0]+1, len0);
        }
        last = count - 1;
        lenLast = strlen(arr[last]);
        if (lenLast > 0 && (arr[last][lenLast-1] == '"' || arr[last][lenLast-1] == '\'')) {
            arr[last][lenLast-1] = '\0';
        }
    }
    free(tmp);
    return count;
}
static int is_executable(const char* path) {
    struct stat sbuf;
    return (stat(path, &sbuf) == 0 && (sbuf.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)));
}

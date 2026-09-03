/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * Copyright (C) 2011 David Bigagli
 *
 * $Id: bjobs.c 397 2007-11-26 19:04:00Z mblack $
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
#include <pwd.h>
#include "cmd.h"
#include "../lib/lsb.h"
#include <netdb.h>
#include <errno.h>
#include <cJSON.h>
#define NL_SETN 8


#define ALL_PROJ_STR "all"

extern int sig_decode(int);
extern char *get_status(struct jobInfoEnt *job);

static void do_options(int, char **, int *, char **, char **,
                       char **, char **, float *, int *, char **, char **);
static int  skip_job(struct jobInfoEnt *);
static void displayJobs(struct jobInfoEnt *, struct jobInfoHead *,
                        int, int);
static void displayO(struct jobInfoEnt *, struct jobInfoHead *,
                        int, int, char*);
static cJSON *displayJson(struct jobInfoEnt *, struct jobInfoHead *,
        int, int, char*);

static LS_LONG_INT *usrJids;
static int *numJobs;

static int numJids;
static int foundJids;

#define MAX_TIMERSTRLEN         20
#define MAX_TIMESTRLEN          20
int uflag = FALSE;
int Wflag = FALSE;
int jsonflag = FALSE;

char *fields[] = {
        "JOBID", "USER", "STAT", "QUEUE", "FROM_HOST", "EXEC_HOST", "JOB_NAME", "SUBMIT_TIME",
        "PROJ_NAME", "CPU_USED", "MEM", "SWAP", "PIDS", "START_TIME", "FINISH_TIME"
};
#define FIELD_INDEX          15

static int isLSFAdmin(void);
static char *Timer2String(float timer);
static char *Time2String(int timer);
static struct config_param securebjobsParams[] =
{
# define LSB_SECURE_JOBINFO_USERS 0
    {"LSB_SECURE_JOBINFO_USERS",NULL},
    {NULL,NULL}
};

void
usage (char *cmd)
{
    fprintf(stderr, I18N_Usage);

    fprintf(stderr, \
": %s [-h] [-V] [-w |-l |-UF] [-a] [-d] [-p] [-s] [-r] [-o] [-json]", cmd);

    if (lsbMode_ == LSB_MODE_BATCH)
        fprintf(stderr, " [-A]\n");

    fprintf(stderr, "%s\n",
            "             [-m host_name] [-q queue_name] [-u user_name | -u all]");

    if (lsbMode_ & LSB_MODE_BATCH)
        fprintf(stderr,
                "             [-P project_name] [-N host_spec]\n");

    fprintf(stderr, "             [-J name_spec]");

    if (lsbMode_ & LSB_MODE_BATCH)
        fprintf(stderr, " [jobId | \"jobId[idxList]\" ...]\n");
    else
        fprintf(stderr, " [jobId ...]\n");

    exit(-1);
}

int
main (int argc, char **argv)
{
    char *jobName = NULL;
    int  options = 0;
    char *user = NULL;
    char *queue = NULL;
    char *host = NULL;
    char *realUser = NULL;
    char *projectName = NULL;
    char *fieldName = NULL;
    int  format = 0;
    struct jobInfoHead *jInfoH;
    struct jobInfoEnt *job;
    int  i;
    LS_LONG_INT jobId;
    int  jobDisplayed = 0;
    float cpuFactor = -1;
    char prline[MAXLINELEN];
    char defaultJobName[8] = "/";
    static char lsfUserName[MAXLINELEN];
    int rc,cc;
    cJSON *bjobsJson = NULL;
    cJSON *jobJsonArray = NULL;
    cJSON *jobJsonItem = NULL;
    char jobidStr[MAXLINELEN];
    char *jobInfoErrMsg = NULL;

    int numQueues;
    char **queues = NULL;
    struct queueInfoEnt *queueInfo;
    char *qHost = NULL;
    char *qUser = NULL;

    rc = _i18n_init ( I18N_CAT_MIN );

    if (lsb_init(argv[0]) < 0) {
        lsb_perror("lsb_init");
        exit(-1);
    }


    TIMEIT(0, do_options(argc, argv, &options, &user, &queue, &host, &jobName, &cpuFactor, &format, &projectName, &fieldName), "do_options");

    if (jsonflag) {
        bjobsJson = cJSON_CreateObject();
        jobJsonArray = cJSON_CreateArray();
        cJSON_AddStringToObject(bjobsJson, "COMMAND", "bjobs");

    }

    if (format == O_FORMAT) {

    }

    if ((format == LONG_FORMAT || format == UF_FORMAT) && (options & PEND_JOB))
        options |= HOST_NAME;

    if ((options & JGRP_ARRAY_INFO) && numJids <= 0 ) {
        if (jobName == NULL)
            jobName = defaultJobName;
    }

    /* Create a hash table to populate the
     * requested jobIDs with it.
     */
    if (numJids > 0) {
        numJobs = calloc(numJids, sizeof(int));
        memset(numJobs, 0, numJids * sizeof(int));
    }

    if (numJids == 1)
        jobId = usrJids[0];
    else
        jobId = 0;

    if (format != LONG_FORMAT && format != UF_FORMAT  && !(options & (HOST_NAME | PEND_JOB)))
        options |= NO_PEND_REASONS;

    if (ls_readconfenv(securebjobsParams,NULL)){
        ls_perror("ls_readconfenv");
        exit(-1);
    }

    TIMEIT(0, (cc = getLSFUser_(lsfUserName, MAXLINELEN)), "getLSFUser_");
    if (cc != 0 ) {
        exit(-1);
    }

    TIMEIT(0, (jInfoH = lsb_openjobinfo_a(jobId,
                                          jobName,
                                          user,
                                          queue,
                                          host,
                                          options)), "lsb_openjobinfo_a");

    if (jInfoH == NULL) {

        if (numJids >= 1) {
            for (i = 0; i < numJids; i++) {
                jobInfoErrMsg = jobInfoErr (usrJids[i], jobName, user, queue, host, options);

                if (jobInfoErrMsg != NULL) {
                    if (jsonflag == FALSE) {
                        fprintf (stderr, "%s\n", jobInfoErrMsg);
                    } else {
                        jobJsonItem = cJSON_CreateObject();
                        sprintf(jobidStr, "%d", LSB_ARRAY_JOBID(usrJids[i]));
                        cJSON_AddStringToObject(jobJsonItem, "JOBID", jobidStr);
                        cJSON_AddStringToObject(jobJsonItem, "ERROR", jobInfoErrMsg);
                        cJSON_AddItemToArray(jobJsonArray, jobJsonItem);
                    }
                }
            }
        } else {
            /* openlava. bjobs without a parameter returns an error
             * if there are no jobs in the system, it does not allow
             * the set of jobs specified on the command line to be an
             * empty set. We change it here by still returning an
             * error but with a different numerical value. We hope
             * to minimize problems with backward compatibility.
             */
            jobInfoErrMsg = jobInfoErr(LSB_ARRAY_JOBID(jobId),
                       jobName,
                       user,
                       queue,
                       host,
                       options);
            if (jobInfoErrMsg != NULL) {
                if (jsonflag == FALSE) {
                    fprintf(stderr, "%s\n", jobInfoErrMsg);
                    exit(-2);
                }
            }
        }

        if (jsonflag == FALSE) {
            exit(-1);
        }
    }

    TIMEIT(0, (queueInfo = lsb_queueinfo(queues,
                                         &numQueues,
                                         qHost,
                                         qUser,
                                         0)), "lsb_queueinfo");

    options &= ~NO_PEND_REASONS;
    jobDisplayed = 0;
    for (i = 0; jInfoH != NULL && i < jInfoH->numJobs; i++) {

        TIMEIT(0, (job = lsb_readjobinfo(NULL)), "lsb_readjobinfo");
        if (job == NULL) {
            lsb_perror("lsb_readjobinfo");
            exit(-1);
        }

        if (numJids == 0 && projectName) {
            if (strcmp(job->submit.projectName, projectName) != 0)
                continue;
        }

        if (numJids > 0)
            if (skip_job(job))
                continue;

        if ((securebjobsParams[LSB_SECURE_JOBINFO_USERS].paramValue) &&
            (securebjobsParams[LSB_SECURE_JOBINFO_USERS].paramValue != NULL) &&
            (strstr(securebjobsParams[LSB_SECURE_JOBINFO_USERS].paramValue,
                    lsfUserName) == NULL) && (strcmp(lsfUserName,job->user) != 0)) {
            if (numJids > 0) {
                lsberrno = LSBE_NO_JOB;

                jobInfoErrMsg = jobInfoErr (LSB_ARRAY_JOBID(job->jobId), jobName, user, queue, host, options);

                if (jobInfoErrMsg != NULL) {
                    if (jsonflag == FALSE) {
                        fprintf (stderr, "%s\n", jobInfoErrMsg);
                    } else {
                        jobJsonItem = cJSON_CreateObject();
                        sprintf(jobidStr, "%d", LSB_ARRAY_JOBID(job->jobId));
                        cJSON_AddStringToObject(jobJsonItem, "JOBID", jobidStr);
                        cJSON_AddStringToObject(jobJsonItem, "ERROR", jobInfoErrMsg);
                        cJSON_AddItemToArray(jobJsonArray, jobJsonItem);
                    }
                }
            }
            continue;
        }

        if (format ==  LONG_FORMAT || format == UF_FORMAT) {
            if (i > 0) {
                sprintf(prline, "------------------------------------------------------------------------------\n");
                printf("%s", prline);
            }
            if (options & PEND_JOB) {
                if (format == UF_FORMAT)
                    displayUF(job, jInfoH, cpuFactor, numQueues, queueInfo);
                else
                    displayLong(job, jInfoH, cpuFactor);
            }
            else {
                if (format == UF_FORMAT)
                    displayUF(job, NULL, cpuFactor, numQueues, queueInfo);
                else
                    displayLong(job, NULL, cpuFactor);
            }
        }
        else if (format == O_FORMAT) {

            if (jsonflag) {
                jobJsonItem = displayJson(job, jInfoH, options, format, fieldName);
                cJSON_AddItemToArray(jobJsonArray, jobJsonItem);
            } else {
                displayO(job, jInfoH, options, format, fieldName);
            }
        }
        else
            displayJobs(job, jInfoH, options, format);

        jobDisplayed ++;
    }

    if (format == LONG_FORMAT || format == UF_FORMAT) {
        sprintf(prline, "\n");
        prtLine(prline);
    }

    TIMEIT(0, lsb_closejobinfo(), "lsb_closejobinfo");

    if (numJids > 1 ) {
        int errCount = FALSE;
        lsberrno = LSBE_NO_JOB;
        for (i = 0; i < numJids; i++) {
            if (numJobs[i] <= 0) {
                errCount = TRUE;

                jobInfoErrMsg = jobInfoErr(usrJids[i], jobName, realUser, queue, host, options);
                if (jobInfoErrMsg != NULL) {
                    if (jsonflag == FALSE) {
                        fprintf (stderr, "%s\n", jobInfoErrMsg);
                    } else {
                        jobJsonItem = cJSON_CreateObject();
                        sprintf(jobidStr, "%d", LSB_ARRAY_JOBID(usrJids[i]));
                        cJSON_AddStringToObject(jobJsonItem, "JOBID", jobidStr);
                        cJSON_AddStringToObject(jobJsonItem, "ERROR", jobInfoErrMsg);
                        cJSON_AddItemToArray(jobJsonArray, jobJsonItem);
                    }
                }
            }
        }
        if (errCount == TRUE && jsonflag == FALSE)
            exit(-1);
    } else {

        if (jobDisplayed == 0) {
            if (projectName) {
                fprintf (stderr, "No job found in project %s\n", projectName);
            }
            if (securebjobsParams[LSB_SECURE_JOBINFO_USERS].paramValue) {
                fprintf (stderr, "No job found \n");
            }
        }
    }


    if (jsonflag) {
        cJSON_AddNumberToObject(bjobsJson, "JOBS", cJSON_GetArraySize(jobJsonArray));
        cJSON_AddItemToObject(bjobsJson, "RECORDS", jobJsonArray);

        char *bjobsJsonString = NULL;
        bjobsJsonString = cJSON_Print(bjobsJson);
        if (bjobsJsonString == NULL)
        {
            fprintf(stderr, "Failed to print bjobs json.\n");
        }
        printf("%s\n", bjobsJsonString);

        cJSON_Delete(bjobsJson);
    }

    if (!jobDisplayed) {
        exit(-1);
    }
    _i18n_end ( ls_catd );
    return(0);

}

static void
do_options (int argc, char **argv, int *options, char **user, char **queue,
            char **host, char **jobName, float *cpuFactor, int *format, char **projectName, char **fieldName)
{
    extern char *optarg;
    int cc, Nflag = 0;
    char *norOp = NULL;


    *options = 0;
    *user = NULL;
    *queue = NULL;
    *fieldName = NULL;
    *host  = NULL;
    *jobName = NULL;
    *format = 0;

    while ((cc = getopt(argc, argv, "VladpsrwWgRAhJ:q:u:m:N:P:SU:o:j:")) != EOF) {
        switch (cc) {
            case 'w':
                if (*format == LONG_FORMAT || *format == UF_FORMAT || *format == O_FORMAT )
                    usage(argv[0]);
                *format = WIDE_FORMAT;
                break;
            case 'l':
                if (*format == WIDE_FORMAT || *format == O_FORMAT)
                    usage(argv[0]);

                if (*format != UF_FORMAT)
                    *format = LONG_FORMAT;

                break;
            case 'a':
                *options |= ALL_JOB;
                break;
            case 'd':
                *options |= DONE_JOB;
                break;
            case 'p':
                *options |= PEND_JOB;
                break;
            case 's':
                *options |= SUSP_JOB;
                break;
            case 'r':
                *options |= RUN_JOB;
                break;
            case 'A':
                *options |= JGRP_ARRAY_INFO;
                break;
            case 'J':
                if ((*jobName) || (*optarg == '\0'))
                    usage(argv[0]);
                *jobName = optarg;
                break;
            case 'q':
                if ((*queue) || (*optarg == '\0'))
                    usage(argv[0]);
                *queue = optarg;
                break;
            case 'u':
                if ((*user) || (*optarg == '\0'))
                    usage(argv[0]);
                *user = optarg;
                uflag = TRUE;
                break;
            case 'm':
                if ((*host) || (*optarg == '\0'))
                    usage(argv[0]);
                *host = optarg;
                break;
            case 'N':
                Nflag = TRUE;
                norOp = optarg;
                break;
            case 'P':
                if ((*projectName) || (*optarg == '\0'))
                    usage(argv[0]);
                *projectName = optarg;
                break;
            case 'W':
                Wflag = TRUE;
                *format = WIDE_FORMAT;
                break;
            case 'V':
                fputs(_LS_VERSION_, stdout);
                exit(0);
            case 'U':
                if (*format == O_FORMAT)
                    usage(argv[0]);
                if (strcmp(optarg,"F")==0) {
                    *format = UF_FORMAT;
                    break;
                }
                usage(argv[0]);
            case 'o':
                if ( (*fieldName) || (*optarg == '\0') ||
                (*format == LONG_FORMAT) || (*format == WIDE_FORMAT) || (*format == UF_FORMAT) )
                    usage(argv[0]);
                *format = O_FORMAT;
                *fieldName = optarg;
                break;
            case 'j':
                // -json to display json format, work with -o option
                if (strcmp(optarg,"son")==0) {
                    jsonflag = TRUE;
                    break;
                }
                usage(argv[0]);
            case 'h':
            default:
                usage(argv[0]);
        }
    }

    TIMEIT(1, (numJids = getSpecJobIds (argc, argv, &usrJids, NULL)), "getSpecJobIds");

    if (jsonflag && *format != O_FORMAT) {
        usage(argv[0]);
    }

    if (numJids > 0) {
        *user = "all";
        *options |= ALL_JOB;
    }
    else {
        if (uflag != TRUE && Wflag == TRUE) {
            if ((getuid() == 0) || isLSFAdmin()) {
                *user = "all";
            }
        }
    }

    if ((*options
         & (~JGRP_ARRAY_INFO)) == 0) {
        *options |= CUR_JOB;
    }

    if (Nflag) {
        float *tempPtr;

        *options |= DONE_JOB;
        *format = LONG_FORMAT;
        TIMEIT(0, (tempPtr = getCpuFactor (norOp, FALSE)), "getCpuFactor");
        if (tempPtr == NULL)
            if ((tempPtr = getCpuFactor (norOp, TRUE)) == NULL)
                if (!isanumber_(norOp)
                    || (*cpuFactor = atof(norOp)) <= 0) {
                    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1458, "<%s> is neither a host model, nor a host name, nor a CPU factor\n")), norOp); /* catgets  1458  */
                    exit(-1);
                }
        if (tempPtr)
            *cpuFactor = *tempPtr;
    }

}


static void
displayJobs (struct jobInfoEnt *job, struct jobInfoHead *jInfoH,
             int options, int format)
{
    char *fName = "displayJobs";
    struct submit *submitInfo;
    static char first = TRUE;
    char *status;
    char subtime[64], donetime[64];
    static char  *exechostfmt;
    static struct loadIndexLog *loadIndex = NULL;
    char *exec_host = "";
    char *jobName, *pos;
    NAMELIST  *hostList = NULL;
    char tmpBuf[MAXLINELEN];
    char osUserName[MAXLINELEN];


    int                 i = 0;


    if (getOSUserName_(job->user, osUserName, MAXLINELEN) != 0) {
        strncpy(osUserName, job->user, MAXLINELEN);
        osUserName[MAXLINELEN - 1] = '\0';
    }

    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
        && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {

            exit(99);
        }
    }


    if (loadIndex == NULL)
        loadIndex = initLoadIndex();

    submitInfo = &job->submit;
    status = get_status(job);

    strcpy(subtime, _i18n_ctime( ls_catd, CTIME_FORMAT_b_d_H_M, &job->submitTime));
    if (IS_FINISH (job->status))
        strcpy(donetime, _i18n_ctime( ls_catd, CTIME_FORMAT_b_d_H_M, &(job->endTime)));
    else
        strcpy(donetime, "      ");

    if (IS_PEND(job->status))
        exec_host = "";
    else if ( job->numExHosts == 0)
        exec_host = "   -   ";
    else
    {

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
            sprintf(tmpBuf, "%d*%s", hostList->counter[0], hostList->names[0]);
            exec_host = tmpBuf;
        }
        else
            exec_host = job->exHosts[0];
    }

    if (first) {
        first = FALSE;
        if (job->jType == JGRP_NODE_ARRAY)
            printf((_i18n_msg_get(ls_catd,NL_SETN,1459, "JOBID    ARRAY_SPEC  OWNER   NJOBS PEND DONE  RUN EXIT SSUSP USUSP PSUSP\n"))); /* catgets  1459  */
        else if (options == PEND_JOB) {
            printf((_i18n_msg_get(ls_catd,NL_SETN,1460, "JOBID   USER    STAT  QUEUE       FROM_HOST      JOB_NAME           SUBMIT_TIME\n"))); /* catgets  1460  */
            exechostfmt = "  ";
        } else {
            printf((_i18n_msg_get(ls_catd,NL_SETN,1461, "JOBID   USER    STAT  QUEUE      FROM_HOST   EXEC_HOST   JOB_NAME   SUBMIT_TIME"))); /* catgets  1461  */

            if (Wflag == TRUE) {
                printf((_i18n_msg_get(ls_catd,NL_SETN,1462, "  PROJ_NAME CPU_USED MEM SWAP PIDS START_TIME FINISH_TIME"))); /* catgets  1462  */
            }

            printf("\n");

            exechostfmt = "%45s%-s\n";
        }
    }

    if (job->jType == JGRP_NODE_ARRAY) {
        if (format != WIDE_FORMAT) {
            printf("%-7d  %-8.8s ", LSB_ARRAY_JOBID(job->jobId), job->submit.jobName);
            printf("%8.8s ", osUserName);
        }
        else {
            printf("%-7d  %s ", LSB_ARRAY_JOBID(job->jobId), job->submit.jobName);
            printf("%s ", job->user);
        }
        printf("  %5d %4d %4d %4d %4d %5d %5d %5d\n",
               job->counter[JGRP_COUNT_NJOBS],
               job->counter[JGRP_COUNT_PEND],
               job->counter[JGRP_COUNT_NDONE],
               job->counter[JGRP_COUNT_NRUN],
               job->counter[JGRP_COUNT_NEXIT],
               job->counter[JGRP_COUNT_NSSUSP],
               job->counter[JGRP_COUNT_NUSUSP],
               job->counter[JGRP_COUNT_NPSUSP]);
        goto cleanup;
    }

    jobName = submitInfo->jobName;
    if (LSB_ARRAY_IDX(job->jobId) && (pos = strchr(jobName, '['))) {
        *pos = '\0';
        sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId));
    }
    if (options == PEND_JOB) {

        TRUNC_STR(jobName, 20);
        printf("%-7d %-7.7s %-5.5s %-11.11s %-14.14s %-18.18s %s\n",
               LSB_ARRAY_JOBID(job->jobId), osUserName, status,
               submitInfo->queue, job->fromHost,
               jobName, subtime);
    } else if (format != WIDE_FORMAT) {
        TRUNC_STR(jobName, 10);
        printf("%-7d %-7.7s %-5.5s %-10.10s %-11.11s %-11.11s %-10.10s %s\n",
               LSB_ARRAY_JOBID(job->jobId), osUserName, status,
               submitInfo->queue, job->fromHost,
               exec_host,
               jobName, subtime);
    } else {
        if (IS_PEND(job->status)) {
            exec_host = "   -    ";
        } else {


            static char *execHostList=NULL;
            static int execHostListSize=0;
            int execHostListUsed;


            if (execHostList == NULL) {
                if ((execHostList = (char *) calloc(1, MAXLINELEN)) == NULL) {
                    fprintf(stderr,I18N_FUNC_FAIL, fName, "malloc");
                    exit(-1);
                }
                execHostListSize = MAXLINELEN;
            }


            strcpy(execHostList, exec_host);
            execHostListUsed =strlen(exec_host);

            if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
                && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
                for (i = 1; i < hostList->listSize; i++) {
                    execHostListUsed+=(strlen(job->exHosts[i])+1);
                    if (execHostListUsed >= execHostListSize) {
                        execHostListSize += MAXLINELEN;
                        if ((execHostList =
                             realloc(execHostList, execHostListSize)) == NULL) {
                            fprintf(stderr, I18N_FUNC_FAIL, fName, "realloc");
                            exit(-1);
                        }
                    }
                    strcat(execHostList,":");
                    sprintf(tmpBuf, "%d*%s", hostList->counter[i],
                            hostList->names[i]);
                    strcat(execHostList, tmpBuf);
                }
            } else {
                for (i = 1; i < job->numExHosts; i++) {
                    execHostListUsed+=(strlen(job->exHosts[i])+1);
                    if (execHostListUsed >= execHostListSize) {
                        execHostListSize += MAXLINELEN;
                        if ((execHostList =
                             realloc(execHostList, execHostListSize)) == NULL) {
                            fprintf(stderr, I18N_FUNC_FAIL, fName, "realloc");
                            exit(-1);
                        }
                    }
                    strcat(execHostList,":");
                    strcat(execHostList, job->exHosts[i]);
                }
            }

            if (execHostList[0] == '\0')
                exec_host = "   -   ";
            else
                exec_host = execHostList;

        }

        if (Wflag == TRUE) {
            printf("%-7d %-7s %-5.5s %-10s %-11s %-11s %-10s %-14.14s",
                   LSB_ARRAY_JOBID(job->jobId),
                   job->user,
                   status,
                   submitInfo->queue,
                   job->fromHost,
                   exec_host,
                   jobName,
                   Time2String(job->submitTime));
        } else {
            printf("%-7d %-7s %-5.5s %-10s %-11s %-11s %-10s %s",
                   LSB_ARRAY_JOBID(job->jobId),
                   job->user,
                   status,
                   submitInfo->queue,
                   job->fromHost,
                   exec_host,
                   jobName,

                   subtime);
        }

        if (Wflag == TRUE) {
            int         i;
            float cpuTime;

            if (job->cpuTime > 0) {
                cpuTime = job->cpuTime;
            }
            else {
                cpuTime = job->runRusage.utime + job->runRusage.stime;
            }
            printf(" %-10s %-10s %-6d %-6d ",
                   job->submit.projectName,
                   Timer2String(cpuTime),
                   ((job->runRusage.mem >0)?job->runRusage.mem :0),
                   ((job->runRusage.swap>0)?job->runRusage.swap:0));
            if (job->runRusage.npids) {
                for (i = 0; i < job->runRusage.npids; i++) {
                    if (i == 0) {
                        printf("%d",job->runRusage.pidInfo[i].pid);
                    } else {
                        printf(",%d",job->runRusage.pidInfo[i].pid);
                    }
                }
            } else {
                printf(" - ");
            }


            if (job->startTime == 0)
                printf(" - ");
            else
                printf(" %s",Time2String(job->startTime));

            if (job->endTime == 0) {
                printf(" - ");
            } else {
                printf(" %s",Time2String(job->endTime));
            }
        }

        printf("\n");
    }


    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
        && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
        if (!IS_PEND(job->status) && format != WIDE_FORMAT) {
            for (i = 1 ; i < hostList->listSize; i++) {
                sprintf(tmpBuf, "%d*%s", hostList->counter[i],
                        hostList->names[i]);
                printf(exechostfmt, "", tmpBuf);
            }
        }
    }
    else {
        if (!IS_PEND(job->status) && format != WIDE_FORMAT) {
            for (i = 1; i < job->numExHosts; i++) {
                printf(exechostfmt, "", job->exHosts[i]);
            }
        }
    }



    if ((options & PEND_JOB) &&  IS_PEND(job->status)) {
        printf(lsb_pendreason(job->numReasons, job->reasonTb, NULL,
                              loadIndex,
                              job->numLimitDetail,
                              job->limitDetailTb));
    }


    if ((options & SUSP_JOB) &&  IS_SUSP(job->status)) {
        if (job->status & JOB_STAT_PSUSP && !(options & PEND_JOB))
            printf(lsb_pendreason(job->numReasons, job->reasonTb, NULL,
                                  loadIndex,job->numLimitDetail,
                                  job->limitDetailTb));
        else if (!(job->status & JOB_STAT_PSUSP))
            printf(lsb_suspreason(job->reasons, job->subreasons, loadIndex));
    }

cleanup:

    return;

}

char *string_upper(char *str) {
    size_t i;
    for (i = 0; str[i] != '\0'; i++) {
        str[i] = toupper((unsigned char)str[i]);
    }
    return str;
}

/*
 * display jobs with -o option
 */
static void
displayO (struct jobInfoEnt *job, struct jobInfoHead *jInfoH,
             int options, int format, char *fieldName)
{
    char *fName = "displayO";
    struct submit *submitInfo;
    static char first = TRUE;
    char *status;
    char subtime[64], donetime[64];
    static char  *exechostfmt;
    static struct loadIndexLog *loadIndex = NULL;
    char *exec_host = "";
    char *jobName, *pos;
    NAMELIST  *hostList = NULL;
    char tmpBuf[MAXLINELEN];
    char osUserName[MAXLINELEN];

    int                 i = 0;

    char *customizedFields[FIELD_INDEX];
    char *customizedField;
    char header[MAXLINELEN];
    int j = 0;
    int k = 0;
    int verifiedField = 0;
    char *delimiter = "";

    // TODO take field split out as a separate function
    customizedField = strtok(fieldName, " ");

    while (customizedField != NULL) {
        customizedFields[j] = string_upper(customizedField);

        verifiedField = 0;
        for (k=0; k<FIELD_INDEX; k++) {
            if ( strcmp(customizedFields[j], fields[k]) == 0 ) {
                verifiedField = 1;
                break;
            }
        }

        if (verifiedField == 0) {
//            fprintf(stderr, "Invalid field specs %s\n", customizedField);
            fprintf(stderr, "<%s> in the format string is not a valid field name.\n", customizedField);
            exit(99);
        }

        sprintf(tmpBuf, "%s%s", delimiter, customizedFields[j]);
        strcat(header, tmpBuf);
        delimiter = " ";

        j++;
        customizedField = strtok(NULL, " ");

    }
    strcat(header, "\n");


    if (getOSUserName_(job->user, osUserName, MAXLINELEN) != 0) {
        strncpy(osUserName, job->user, MAXLINELEN);
        osUserName[MAXLINELEN - 1] = '\0';
    }

    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
        && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {

            exit(99);
        }
    }


    if (loadIndex == NULL)
        loadIndex = initLoadIndex();

    submitInfo = &job->submit;
    status = get_status(job);

    strcpy(subtime, _i18n_ctime( ls_catd, CTIME_FORMAT_b_d_H_M, &job->submitTime));
    if (IS_FINISH (job->status))
        strcpy(donetime, _i18n_ctime( ls_catd, CTIME_FORMAT_b_d_H_M, &(job->endTime)));
    else
        strcpy(donetime, "      ");

    if (IS_PEND(job->status))
        exec_host = "-";
    else if ( job->numExHosts == 0)
        exec_host = "   -   ";
    else
    {

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
            sprintf(tmpBuf, "%d*%s", hostList->counter[0], hostList->names[0]);
            exec_host = tmpBuf;
        }
        else
            exec_host = job->exHosts[0];
    }

    if (first) {
        first = FALSE;
        printf((_i18n_msg_get(ls_catd,NL_SETN,1461, header))); /* catgets  1461  */
    }

    jobName = submitInfo->jobName;
    if (LSB_ARRAY_IDX(job->jobId) && (pos = strchr(jobName, '['))) {
        *pos = '\0';
        sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId));
    }

    delimiter = "";
    for( j=0 ; j<FIELD_INDEX && customizedFields[j] != NULL && strlen(customizedFields[j]) > 0; j++ ){
        if ( j > 0) {
            delimiter = " ";
        }

        if ( strcmp(customizedFields[j], "JOBID") == 0 ) {
            printf("%s%d", delimiter, LSB_ARRAY_JOBID(job->jobId));
            continue;
        }
        if ( strcmp(customizedFields[j], "USER") == 0 ) {
            printf("%s%s", delimiter,  osUserName);
            continue;
        }
        if ( strcmp(customizedFields[j], "STAT") == 0 ) {
            printf("%s%s", delimiter,  status);
            continue;
        }
        if ( strcmp(customizedFields[j], "QUEUE") == 0 ) {
            printf("%s%s", delimiter,  submitInfo->queue);
            continue;
        }
        if ( strcmp(customizedFields[j], "FROM_HOST") == 0 ) {
            printf("%s%s", delimiter,  job->fromHost);
            continue;
        }
        if ( strcmp(customizedFields[j], "EXEC_HOST") == 0 ) {
            printf("%s%s", delimiter,  exec_host);
            continue;
        }
        if ( strcmp(customizedFields[j], "JOB_NAME") == 0 ) {
            printf("%s%s", delimiter,  jobName);
            continue;
        }
        if ( strcmp(customizedFields[j], "SUBMIT_TIME") == 0 ) {
            printf("%s%s", delimiter,  subtime);
            continue;
        }
        if ( strcmp(customizedFields[j], "PROJ_NAME") == 0 ) {
            printf("%s%s", delimiter,  job->submit.projectName);
            continue;
        }
        if ( strcmp(customizedFields[j], "CPU_USED") == 0 ) {
            float cpuTime=0;

            if (job->cpuTime > 0) {
                cpuTime = job->cpuTime;
            }
            else {
                cpuTime = job->runRusage.utime + job->runRusage.stime;
            }
            printf("%s%s", delimiter,  Timer2String(cpuTime));
            continue;
        }
        if ( strcmp(customizedFields[j], "MEM") == 0 ) {
            printf("%s%d", delimiter,  ((job->runRusage.mem >0)?job->runRusage.mem :0));
            continue;
        }
        if ( strcmp(customizedFields[j], "SWAP") == 0 ) {
            printf("%s%d", delimiter,  ((job->runRusage.swap>0)?job->runRusage.swap:0));
            continue;
        }

        if ( strcmp(customizedFields[j], "PIDS") == 0 ) {
            if (job->runRusage.npids) {
                printf("%s", delimiter);
                for (i = 0; i < job->runRusage.npids; i++) {
                    if (i == 0) {
                        printf("%d",job->runRusage.pidInfo[i].pid);
                    } else {
                        printf(",%d",job->runRusage.pidInfo[i].pid);
                    }
                }
            } else {
                printf("%s-", delimiter);
            }
            continue;
        }
        if ( strcmp(customizedFields[j], "START_TIME") == 0 ) {
            if (job->startTime == 0)
                printf("%s-", delimiter);
            else
                printf("%s%s", delimiter,Time2String(job->startTime));
            continue;
        }
        if ( strcmp(customizedFields[j], "FINISH_TIME") == 0 ) {
            if (job->endTime == 0) {
                printf("%s-", delimiter);
            } else {
                printf("%s%s", delimiter, Time2String(job->endTime));
            }
            continue;
        }
    }
    printf("\n");

    return;
}

/*
 * display jobs in json format, in conjunction with -o option.
 */
cJSON
*displayJson (struct jobInfoEnt *job, struct jobInfoHead *jInfoH,
        int options, int format, char *fieldName)
{
    char *fName = "displayJson";
    struct submit *submitInfo;
    static char first = TRUE;
    char *status;
    char subtime[64], donetime[64];
    static char  *exechostfmt;
    static struct loadIndexLog *loadIndex = NULL;
    char *exec_host = "";
    char *jobName, *pos;
    NAMELIST  *hostList = NULL;
    char tmpBuf[MAXLINELEN];
    char osUserName[MAXLINELEN];


    int                 i = 0;

    char *customizedFields[FIELD_INDEX];
    char *customizedField;
    char *customizedFieldUpper;
    int j = 0;
    int k = 0;
    int l;
    int verifiedField = 0;
    int duplicatedField = 0;
    char jobIdStr[MAXLINELEN];
    char jobMemStr[MAXLINELEN];
    char jobSwapStr[MAXLINELEN];

    cJSON *jobItem = cJSON_CreateObject();

    // TODO take field split out as a separate function
    customizedField = strtok(fieldName, " ");

    while (customizedField != NULL) {
        customizedFieldUpper = string_upper(customizedField);
        duplicatedField = 0;
        for( l=0 ; l < j; l++ ){
            if ( strcmp(customizedFields[l], customizedFieldUpper) == 0 ) {
                duplicatedField = 1;
                break;
            }
        }

        if (duplicatedField == 1) {
            customizedField = strtok(NULL, " ");
            continue;
        }

        customizedFields[j] = customizedFieldUpper;

        verifiedField = 0;
        for (k=0; k<FIELD_INDEX; k++) {
            if ( strcmp(customizedFields[j], fields[k]) == 0 ) {
                verifiedField = 1;
                break;
            }
        }

        if (verifiedField == 0) {
//            fprintf(stderr, "Invalid field specs %s\n", customizedField);
            fprintf(stderr, "<%s> in the format string is not a valid field name.\n", customizedField);
            exit(99);
        }

        j++;
        customizedField = strtok(NULL, " ");

    }


    if (getOSUserName_(job->user, osUserName, MAXLINELEN) != 0) {
        strncpy(osUserName, job->user, MAXLINELEN);
        osUserName[MAXLINELEN - 1] = '\0';
    }

    if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
        && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
        hostList = lsb_compressStrList(job->exHosts, job->numExHosts);
        if (!hostList) {

            exit(99);
        }
    }


    if (loadIndex == NULL)
        loadIndex = initLoadIndex();

    submitInfo = &job->submit;
    status = get_status(job);

    strcpy(subtime, _i18n_ctime( ls_catd, CTIME_FORMAT_b_d_H_M, &job->submitTime));
    if (IS_FINISH (job->status))
        strcpy(donetime, _i18n_ctime( ls_catd, CTIME_FORMAT_b_d_H_M, &(job->endTime)));
    else
        strcpy(donetime, "      ");

    if (IS_PEND(job->status))
        exec_host = "";
    else if ( job->numExHosts == 0)
        exec_host = "   -   ";
    else
    {

        if (lsbParams[LSB_SHORT_HOSTLIST].paramValue && job->numExHosts > 1
            && strcmp(lsbParams[LSB_SHORT_HOSTLIST].paramValue, "1") == 0 ) {
            sprintf(tmpBuf, "%d*%s", hostList->counter[0], hostList->names[0]);
            exec_host = tmpBuf;
        }
        else
            exec_host = job->exHosts[0];
    }

    jobName = submitInfo->jobName;
    if (LSB_ARRAY_IDX(job->jobId) && (pos = strchr(jobName, '['))) {
        *pos = '\0';
        sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId));
    }

    for( j=0 ; j<FIELD_INDEX && customizedFields[j] != NULL && strlen(customizedFields[j]) > 0; j++ ){

        if ( strcmp(customizedFields[j], "JOBID") == 0 ) {
            sprintf(jobIdStr, "%d", LSB_ARRAY_JOBID(job->jobId));
            cJSON_AddStringToObject(jobItem, "JOBID", jobIdStr);
            continue;
        }

        if ( strcmp(customizedFields[j], "USER") == 0 ) {
            cJSON_AddStringToObject(jobItem, "USER", osUserName);
            continue;
        }

        if ( strcmp(customizedFields[j], "STAT") == 0 ) {
            cJSON_AddStringToObject(jobItem, "STAT", status);
            continue;
        }

        if ( strcmp(customizedFields[j], "QUEUE") == 0 ) {
            cJSON_AddStringToObject(jobItem, "QUEUE", submitInfo->queue);
            continue;
        }

        if ( strcmp(customizedFields[j], "FROM_HOST") == 0 ) {
            cJSON_AddStringToObject(jobItem, "FROM_HOST", job->fromHost);
            continue;
        }

        if ( strcmp(customizedFields[j], "EXEC_HOST") == 0 ) {
            cJSON_AddStringToObject(jobItem, "EXEC_HOST", exec_host);
            continue;
        }

        if ( strcmp(customizedFields[j], "JOB_NAME") == 0 ) {
            cJSON_AddStringToObject(jobItem, "JOB_NAME", jobName);
            continue;
        }

        if ( strcmp(customizedFields[j], "SUBMIT_TIME") == 0 ) {
            cJSON_AddStringToObject(jobItem, "SUBMIT_TIME", subtime);
            continue;
        }

        if ( strcmp(customizedFields[j], "PROJ_NAME") == 0 ) {
            cJSON_AddStringToObject(jobItem, "PROJ_NAME", job->submit.projectName);
            continue;
        }

        if ( strcmp(customizedFields[j], "CPU_USED") == 0 ) {
            float cpuTime=0;

            if (job->cpuTime > 0) {
                cpuTime = job->cpuTime;
            }
            else {
                cpuTime = job->runRusage.utime + job->runRusage.stime;
            }
            cJSON_AddStringToObject(jobItem, "CPU_USED", Timer2String(cpuTime));
            continue;
        }

        if ( strcmp(customizedFields[j], "MEM") == 0 ) {
            sprintf(jobMemStr, "%d", ((job->runRusage.mem >0)?job->runRusage.mem :0));
            cJSON_AddStringToObject(jobItem, "MEM", jobMemStr);
            continue;
        }

        if ( strcmp(customizedFields[j], "SWAP") == 0 ) {
            sprintf(jobSwapStr, "%d", ((job->runRusage.swap>0)?job->runRusage.swap:0));
            cJSON_AddStringToObject(jobItem, "SWAP", jobSwapStr);
            continue;
        }

        if ( strcmp(customizedFields[j], "PIDS") == 0 ) {
            char pids[MAXLINELEN];

            if (job->runRusage.npids) {
                for (i = 0; i < job->runRusage.npids; i++) {
                    if (i == 0) {
                        sprintf(tmpBuf, "%d", job->runRusage.pidInfo[i].pid);
                        strcat(pids, tmpBuf);
                    } else {
                        sprintf(tmpBuf, ",%d", job->runRusage.pidInfo[i].pid);
                        strcat(pids, tmpBuf);
                    }
                }
                cJSON_AddStringToObject(jobItem, "PIDS", pids);
            } else {
                cJSON_AddStringToObject(jobItem, "PIDS", "-");
            }
            continue;
        }

        if ( strcmp(customizedFields[j], "START_TIME") == 0 ) {
            if (job->startTime == 0)
                cJSON_AddStringToObject(jobItem, "START_TIME", "-");
            else
                cJSON_AddStringToObject(jobItem, "START_TIME", Time2String(job->startTime));
            continue;
        }

        if ( strcmp(customizedFields[j], "FINISH_TIME") == 0 ) {
            if (job->endTime == 0) {
                cJSON_AddStringToObject(jobItem, "FINISH_TIME", "-");
            } else {
                cJSON_AddStringToObject(jobItem, "FINISH_TIME", Time2String(job->endTime));
            }
            continue;
        }
    }

    return jobItem;
}

static int
skip_job(struct jobInfoEnt *job)
{
    int i;


    for (i=0; i < numJids; i++) {
        if (job->jobId == usrJids[i] ||
            LSB_ARRAY_JOBID(job->jobId) == usrJids[i]) {
            numJobs[i]++;
            foundJids++;
            return FALSE;
        }
    }


    return TRUE;

}

static int
isLSFAdmin(void)
{
    static char fname[] = "isLSFAdmin";
    struct clusterInfo *clusterInfo;
    char  *mycluster;
    char   lsfUserName[MAXLINELEN];
    int i, j, num;

    if ((mycluster = ls_getclustername()) == NULL) {
        if (logclass & (LC_TRACE))
            ls_syslog(LOG_ERR,
                      "%s: ls_getclustername(): %M", fname);
        return (FALSE);
    }

    num = 0;
    if ((clusterInfo = ls_clusterinfo(NULL, &num, NULL, 0, 0)) == NULL) {
        if (logclass & (LC_TRACE))
            ls_syslog(LOG_ERR,
                      "%s: ls_clusterinfo(): %M", fname);
        return (FALSE);
    }


    if (getLSFUser_(lsfUserName, MAXLINELEN) != 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_MM, fname, "getLSFUser_");
        return (FALSE);
    }

    for (i = 0; i < num; i++) {
        if (!strcmp(mycluster, clusterInfo[i].clusterName)) {
            for (j = 0; j < clusterInfo->nAdmins; j++) {
                if (strcmp(lsfUserName, clusterInfo->admins[j]) == 0)
                    return TRUE;
            }
            return FALSE;
        }
    }

    return(FALSE);

}

static char *
Timer2String(float timer)
{
    static char TimerStr[MAX_TIMERSTRLEN];
    int         Hour, Minute, Second, Point, Time;

    Point   = timer*100.0;
    Point   = Point%100;
    Time    = timer;
    Hour    = Time/3600;
    Minute  = (Time%3600)/60;
    Second  = (Time%3600)%60;
    sprintf(TimerStr,"%03d:%02d:%02d.%02d",
            Hour,
            Minute,
            Second,
            Point);
    return(TimerStr);
}

static char *
Time2String(int timer)
{
    static char TimeStr[MAX_TIMESTRLEN];
    struct tm *Time;
    time_t tempTime;

    memset(TimeStr, '\0', sizeof(TimeStr));
    tempTime = (time_t) timer;
    Time = (struct tm *) localtime(&tempTime);
    sprintf(TimeStr, "%02d/%02d-%02d:%02d:%02d",
            Time->tm_mon+1,
            Time->tm_mday,
            Time->tm_hour,
            Time->tm_min,
            Time->tm_sec);

    return(TimeStr);
}

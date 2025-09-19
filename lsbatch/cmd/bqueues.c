/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: bqueues.c 397 2007-11-26 19:04:00Z mblack $
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

#include "cmd.h"

void load2Str();
static void prtQueuesLong (int, struct queueInfoEnt *);
static void prtQueuesShort (int, struct queueInfoEnt *);
static void printShareAcctTree(struct shareAcctInfoEnt *, char *);
static void prtShareAcctHeader();
static void prtShareAcct(struct shareAcctInfoEnt *);

static char wflag = FALSE;
static char lflag = FALSE;
static char rflag = FALSE;
extern int terminateWhen_(int *, char *);

#define QUEUE_NAME_LENGTH    15
#define QUEUE_PRIO_LENGTH    4
#define QUEUE_STATUS_LENGTH  14
#define QUEUE_JL_U_LENGTH    4
#define QUEUE_JL_P_LENGTH    4
#define QUEUE_JL_H_LENGTH    4
#define QUEUE_MAX_LENGTH     4
#define QUEUE_NJOBS_LENGTH   5
#define QUEUE_PEND_LENGTH    5
#define QUEUE_RUN_LENGTH     5
#define QUEUE_SUSP_LENGTH    5
#define QUEUE_SSUSP_LENGTH   5
#define QUEUE_USUSP_LENGTH   5
#define QUEUE_NICE_LENGTH    4
#define QUEUE_RSV_LENGTH     4
#define QUEUE_FS_NAME        14
#define QUEUE_FS_SHARES      7
#define QUEUE_FS_PRIO        11
#define QUEUE_FS_STARTED     9
#define QUEUE_FS_CPUTIME     11
#define QUEUE_FS_RUNTIME     9

static char fomt[200];

void
usage (char *cmd)
{
     fprintf(stderr, "%s: %s [-h] [-V] [-w | -l | -r] [-m host_name | -m cluster_name]\n", I18N_Usage, cmd);

    if (lsbMode_ & LSB_MODE_BATCH)
        fprintf(stderr, " [-u user_name]");
    fprintf(stderr, " [queue_name ...]\n");
    exit(-1);
}

int
main(int argc, char **argv)
{
    int numQueues;
    char **queueNames=NULL, **queues = NULL;
    struct queueInfoEnt *queueInfo;
    int cc, defaultQ = FALSE;
    char *host = NULL, *user = NULL;

    numQueues = 0;

    _i18n_init ( I18N_CAT_MIN );

    if (lsb_init(argv[0]) < 0) {
        lsb_perror("lsb_init");
        exit(-1);
    }

    while ((cc = getopt(argc, argv, "Vhlwrm:u:")) != EOF) {
        switch (cc) {
            case 'l':
                lflag = TRUE;
                if (wflag || rflag)
                    usage(argv[0]);
                break;
            case 'w':
                wflag = TRUE;
                if (lflag || rflag)
                    usage(argv[0]);
                break;
            case 'm':
                if (host != NULL || *optarg == '\0')
                    usage(argv[0]);
                host = optarg;
                break;
            case 'u':
                if (user != NULL || *optarg == '\0')
                    usage(argv[0]);
                user = optarg;
                break;
            case 'r':
                rflag = TRUE;
                if (lflag || wflag)
                    usage(argv[0]);
                break;
            case 'V':
                fputs(_LS_VERSION_, stdout);
                exit(0);
            case 'h':
            default:
                usage(argv[0]);
        }
    }

    numQueues = getNames(argc,
                         argv,
                         optind,
                         &queueNames,
                         &defaultQ,
                         "queue");
    if (!defaultQ && numQueues != 0)
        queues = queueNames;
    else
        queues = NULL;

    TIMEIT(0, (queueInfo = lsb_queueinfo(queues,
                                         &numQueues,
                                         host,
                                         user,
                                         0)), "lsb_queueinfo");

    if (!queueInfo) {
        if (lsberrno == LSBE_BAD_QUEUE && queues)
            lsb_perror(queues[numQueues]);
        else {
            switch (lsberrno) {
                case LSBE_BAD_HOST   :
                case LSBE_QUEUE_HOST :
                    lsb_perror (host);
                    break;
                case LSBE_BAD_USER   :
                case LSBE_QUEUE_USE  :
                    lsb_perror (user);
                    break;
                default :
                    lsb_perror (NULL);
            }
        }
        return -1;
    }

    if (lflag || rflag)
        prtQueuesLong(numQueues, queueInfo);
    else
        prtQueuesShort(numQueues, queueInfo);

    return 0;
}

static void
prtQueuesLong(int numQueues, struct queueInfoEnt *queueInfo)
{
    struct queueInfoEnt   *qp;
    char                  statusStr[64];
    char                  userJobLimit[MAX_CHARLEN];
    char                  procJobLimit[MAX_CHARLEN];
    char                  hostJobLimit[MAX_CHARLEN];
    char                  maxJobs[MAX_CHARLEN];
    int                   i;
    int                   numDefaults = 0;
    struct lsInfo         *lsInfo;
    int                   printFlag = 0;
    int                   printFlag1 = 0;
    int                   printFlag2 = 0;
    int                   procLimits[3];

    if ((lsInfo = ls_info()) == NULL) {
        ls_perror("ls_info");
        exit(-1);
    }

    printf("\n");

    for (i = 0; i < numQueues; i++ )  {
        qp = &(queueInfo[i]);
        if (qp->qAttrib & Q_ATTRIB_DEFAULT)
            numDefaults++;
    }

    for (i = 0; i < numQueues; i++) {

        qp = &(queueInfo[i]);

        if (qp->qStatus & QUEUE_STAT_OPEN) {
            sprintf(statusStr, "%s:", I18N_Open);
        }
        else {
            sprintf(statusStr, "%s:", I18N_Closed);
        }
        if (qp->qStatus & QUEUE_STAT_ACTIVE) {
            if (qp->qStatus & QUEUE_STAT_RUN) {
                strcat(statusStr, I18N_Active);
            } else {
                strcat(statusStr, I18N_Inact__Win);
            }
        } else {
            strcat(statusStr, I18N_Inact__Adm);
        }


        if (qp->maxJobs < INFINIT_INT)
            strcpy(maxJobs, prtValue(QUEUE_MAX_LENGTH, qp->maxJobs) );
        else
            strcpy(maxJobs, prtDash(QUEUE_MAX_LENGTH) );


        if (qp->userJobLimit < INFINIT_INT)
            strcpy(userJobLimit,
                   prtValue(QUEUE_JL_U_LENGTH, qp->userJobLimit) );
        else
            strcpy(userJobLimit, prtDash(QUEUE_JL_U_LENGTH) );


        if (qp->procJobLimit < INFINIT_FLOAT) {
            sprintf(fomt, "%%%d.1f ", QUEUE_JL_P_LENGTH);
            sprintf (procJobLimit, fomt, qp->procJobLimit);
        }
        else
            strcpy(procJobLimit, prtDash(QUEUE_JL_P_LENGTH) );


        if (qp->hostJobLimit < INFINIT_INT)
            strcpy(hostJobLimit,
                   prtValue(QUEUE_JL_H_LENGTH, qp->hostJobLimit) );
        else
            strcpy(hostJobLimit, prtDash(QUEUE_JL_H_LENGTH) );

        if (i > 0)
            printf("-------------------------------------------------------------------------------\n\n");
        printf("%s: %s\n", _i18n_msg_get(ls_catd,NL_SETN,1210,
                                         "QUEUE"), qp->queue); /* catgets  1210  */

        printf("  -- %s", qp->description);
        if (qp->qAttrib & Q_ATTRIB_DEFAULT) {
            if (numDefaults == 1)
                printf("  %s.\n\n", _i18n_msg_get(ls_catd,NL_SETN,1211,
                                                  "This is the default queue")); /* catgets  1211  */
            else
                printf("  %s.\n\n", _i18n_msg_get(ls_catd,NL_SETN,1212,
                                                  "This is one of the default queues")); /* catgets  1212  */
        } else
            printf("\n\n");

        printf((_i18n_msg_get(ls_catd,NL_SETN,1213, "PARAMETERS/STATISTICS\n"))); /* catgets  1213  */

        prtWord(QUEUE_PRIO_LENGTH, I18N_PRIO, 0);

        if ( lsbMode_ & LSB_MODE_BATCH )
            prtWord(QUEUE_NICE_LENGTH, I18N_NICE, 1);

        prtWord(QUEUE_STATUS_LENGTH, I18N_STATUS, 0);

        if ( lsbMode_ & LSB_MODE_BATCH ) {
            prtWord(QUEUE_MAX_LENGTH,  I18N_MAX, -1);
            prtWord(QUEUE_JL_U_LENGTH, I18N_JL_U, -1);
            prtWord(QUEUE_JL_P_LENGTH, I18N_JL_P, -1);
            prtWord(QUEUE_JL_H_LENGTH, I18N_JL_H, -1);
        };

        prtWord(QUEUE_NJOBS_LENGTH, I18N_NJOBS, -1);
        prtWord(QUEUE_PEND_LENGTH,  I18N_PEND,  -1);
        prtWord(QUEUE_RUN_LENGTH,   I18N_RUN,   -1);
        prtWord(QUEUE_SSUSP_LENGTH, I18N_SSUSP, -1);
        prtWord(QUEUE_USUSP_LENGTH, I18N_USUSP, -1);
        prtWord(QUEUE_RSV_LENGTH,   I18N_RSV,   -1);
        printf("\n");

        prtWordL(QUEUE_PRIO_LENGTH,
                 prtValue(QUEUE_PRIO_LENGTH-1, qp->priority));

        if ( lsbMode_ & LSB_MODE_BATCH )
            prtWordL(QUEUE_NICE_LENGTH,
                     prtValue(QUEUE_NICE_LENGTH-1, qp->nice));

        prtWord(QUEUE_STATUS_LENGTH, statusStr, 0);

        if ( lsbMode_ & LSB_MODE_BATCH ) {
            sprintf(fomt, "%%%ds%%%ds%%%ds%%%ds", QUEUE_MAX_LENGTH,
                    QUEUE_JL_U_LENGTH,
                    QUEUE_JL_P_LENGTH,
                    QUEUE_JL_H_LENGTH );
            printf(fomt,
                   maxJobs, userJobLimit, procJobLimit, hostJobLimit);
        };

        sprintf(fomt, "%%%dd %%%dd %%%dd %%%dd %%%dd %%%dd\n",
                QUEUE_NJOBS_LENGTH,
                QUEUE_PEND_LENGTH,
                QUEUE_RUN_LENGTH,
                QUEUE_SSUSP_LENGTH,
                QUEUE_USUSP_LENGTH,
                QUEUE_RSV_LENGTH );
        printf(fomt,
               qp->numJobs, qp->numPEND, qp->numRUN,
               qp->numSSUSP, qp->numUSUSP, qp->numRESERVE);

        if ( qp->mig < INFINIT_INT )
            printf((_i18n_msg_get(ls_catd,NL_SETN,1215,
                                  "Migration threshold is %d minutes\n")), qp->mig); /* catgets  1215  */

        if ( qp->schedDelay < INFINIT_INT )
            printf((_i18n_msg_get(ls_catd,NL_SETN,1216, "Schedule delay for a new job is %d seconds\n")),  /* catgets  1216  */
                   qp->schedDelay);

        if ( qp->acceptIntvl < INFINIT_INT )
            printf((_i18n_msg_get(ls_catd,NL_SETN,1217, "Interval for a host to accept two jobs is %d seconds\n")), /* catgets  1217  */
                   qp->acceptIntvl);

        if (((qp->defLimits[LSF_RLIMIT_CPU] != INFINIT_INT) &&
             (qp->defLimits[LSF_RLIMIT_CPU] > 0 )) ||
            ((qp->defLimits[LSF_RLIMIT_RUN] != INFINIT_INT) &&
             (qp->defLimits[LSF_RLIMIT_RUN] > 0 )) ||
            ((qp->defLimits[LSF_RLIMIT_DATA] != INFINIT_INT) &&
             (qp->defLimits[LSF_RLIMIT_DATA] > 0 )) ||
            ((qp->defLimits[LSF_RLIMIT_RSS] != INFINIT_INT) &&
             (qp->defLimits[LSF_RLIMIT_RSS] > 0 )) ||
            ((qp->defLimits[LSF_RLIMIT_PROCESS] != INFINIT_INT) &&
             (qp->defLimits[LSF_RLIMIT_PROCESS] > 0 ))) {


            printf("\n");
            printf(_i18n_msg_get(ls_catd,NL_SETN,1270,
                                 "DEFAULT LIMITS:") /* catgets 1270 */ );
            prtResourceLimit (qp->defLimits, qp->hostSpec, 1.0, 0);
            printf("\n");
            printf(_i18n_msg_get(ls_catd,NL_SETN,1271,
                                 "MAXIMUM LIMITS:") /* catgets 1271 */ );
        }

        procLimits[0] = qp->minProcLimit;
        procLimits[1] = qp->defProcLimit;
        procLimits[2] = qp->procLimit;
        prtResourceLimit (qp->rLimits, qp->hostSpec, 1.0, procLimits);

        if ( lsbMode_ & LSB_MODE_BATCH ) {
            printf((_i18n_msg_get(ls_catd,NL_SETN,1218, "\nSCHEDULING PARAMETERS\n")));  /* catgets 1218 */

            if (printThresholds (qp->loadSched,  qp->loadStop, NULL, NULL,
                                 MIN(lsInfo->numIndx, qp->nIdx), lsInfo) < 0)
                exit (-1);
        }

        if ((qp->qAttrib & Q_ATTRIB_EXCLUSIVE)
            || (qp->qAttrib & Q_ATTRIB_BACKFILL)
            || (qp->qAttrib & Q_ATTRIB_IGNORE_DEADLINE)
            || (qp->qAttrib & Q_ATTRIB_ONLY_INTERACTIVE)
            || (qp->qAttrib & Q_ATTRIB_NO_INTERACTIVE)
            || (qp->qAttrib & Q_ATTRIB_FS)) {

            printf("\n%s:", _i18n_msg_get(ls_catd,NL_SETN,1219,
                                          "SCHEDULING POLICIES")); /* catgets  1219  */
            if (qp->qAttrib & Q_ATTRIB_BACKFILL)
                printf("  %s", (_i18n_msg_get(ls_catd,NL_SETN,1223,
                                              "BACKFILL"))); /* catgets  1223  */
            if (qp->qAttrib & Q_ATTRIB_IGNORE_DEADLINE)
                printf("  %s", (_i18n_msg_get(ls_catd,NL_SETN,1224,
                                              "IGNORE_DEADLINE"))); /* catgets  1224  */
            if (qp->qAttrib & Q_ATTRIB_EXCLUSIVE)
                printf("  %s", (_i18n_msg_get(ls_catd,NL_SETN,1225,
                                              "EXCLUSIVE"))); /* catgets  1225  */
            if (qp->qAttrib & Q_ATTRIB_NO_INTERACTIVE)
                printf("  %s", (_i18n_msg_get(ls_catd,NL_SETN,1226,
                                              "NO_INTERACTIVE"))); /* catgets  1226  */
            if (qp->qAttrib & Q_ATTRIB_ONLY_INTERACTIVE)
                printf("  %s", (_i18n_msg_get(ls_catd,NL_SETN,1227,
                                              "ONLY_INTERACTIVE")));  /* catgets  1227  */
            if (qp->qAttrib & Q_ATTRIB_FS) {
                printf("  %s", (_i18n_msg_get(ls_catd,NL_SETN,1227,
                                              "FAIRSAHRE")));                 
            }
            printf("\n");
        }

        if (qp->qAttrib & Q_ATTRIB_FS) {
            char path[MAXPATHLEN] = {0};
            int  hasFactor = 0;
            printf("USER_SHARES:  %s", qp->userShares);
            printf("\n");
            printShareAcctTree(qp->shareAcctTree, path);

            if (qp->fsFactors.runTimeFactor >= 0) {
                printf("\n%s: %f",
                        (_i18n_msg_get(ls_catd,NL_SETN,1233, "RUN_TIME_FACTOR")), qp->fsFactors.runTimeFactor); /* catgets  1233 */
                hasFactor = 1;
            }

            if (qp->fsFactors.cpuTimeFactor >= 0) {
                printf("\n%s: %f",
                        (_i18n_msg_get(ls_catd,NL_SETN,1234, "CPU_TIME_FACTOR")), qp->fsFactors.runTimeFactor); /* catgets  1234 */
                hasFactor = 1;
            }

            if (qp->fsFactors.runJobFactor >= 0) {
                printf("\n%s: %f",
                        (_i18n_msg_get(ls_catd,NL_SETN,1235, "RUN_JOB_FACTOR")), qp->fsFactors.runJobFactor); /* catgets  1235 */
                hasFactor = 1;
            }

            if (qp->fsFactors.histHours >= 0) {
                printf("\n%s: %f",
                       (_i18n_msg_get(ls_catd,NL_SETN,1236, "HIST_HOURS")), qp->fsFactors.histHours); /* catgets  1236 */  
                hasFactor = 1;
            }
            if (hasFactor) {
                printf("\n");
            }
        }

        if (strcmp (qp->defaultHostSpec, " ") !=  0)
            printf("\n%s: %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1230, "DEFAULT HOST SPECIFICATION")), qp->defaultHostSpec); /* catgets  1230  */

        if (qp->windows && strcmp (qp->windows, " " ) !=0)
            printf("\n%s: %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1231, "RUN_WINDOW")), qp->windows); /* catgets  1231  */
        if (strcmp (qp->windowsD, " ")  !=  0)
            printf("\n%s: %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1232, "DISPATCH_WINDOW")), qp->windowsD); /* catgets  1232  */

        if (lsbMode_ & LSB_MODE_BATCH) {
            if ( strcmp(qp->userList, " ") == 0) {
                printf("\n%s:  %s\n", I18N_USERS,
                       I18N(408, "all")); /* catgets 408 */
            } else {
                if (strcmp(qp->userList, " ") != 0 && qp->userList[0] != 0)
                    printf("\n%s: %s\n", I18N_USERS, qp->userList);
            }
        }

        if (strcmp(qp->hostList, " ") == 0) {
            if (lsbMode_ & LSB_MODE_BATCH)
                printf("%s\n",
                       (_i18n_msg_get(ls_catd,NL_SETN,1235, "HOSTS:  all"))); /* catgets  1235  */
            else
                printf("%s\n",
                       (_i18n_msg_get(ls_catd,NL_SETN,1236, "HOSTS: all hosts used by the LSF JobScheduler system"))); /* catgets  1236  */
        } else {
            if (strcmp(qp->hostList, " ") != 0 && qp->hostList[0])
                printf("%s:  %s\n", I18N_HOSTS, qp->hostList);
        }
	if (strcmp (qp->prepostUsername, " ") != 0)
	    printf("%s:  %s\n",
		   (_i18n_msg_get(ls_catd,NL_SETN,1238, "PRE_POST_EXEC_USER")), qp->prepostUsername); /* catgets  1238  */
        if (strcmp (qp->admins, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1239, "ADMINISTRATORS")), qp->admins); /* catgets  1239  */
        if (strcmp (qp->preCmd, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1240, "PRE_EXEC")), qp->preCmd); /* catgets  1240  */
        if (strcmp (qp->postCmd, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1241, "POST_EXEC")), qp->postCmd); /* catgets  1241  */
        if (strcmp (qp->requeueEValues, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1242, "REQUEUE_EXIT_VALUES")), qp->requeueEValues); /* catgets  1242  */
        if (strcmp (qp->resReq, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1243, "RES_REQ")), qp->resReq); /* catgets  1243  */
        if (qp->slotHoldTime > 0)
            printf((_i18n_msg_get(ls_catd,NL_SETN,1244, "Maximum slot reservation time: %d seconds\n")), qp->slotHoldTime); /* catgets  1244  */
        if (strcmp (qp->resumeCond, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1245, "RESUME_COND")), qp->resumeCond); /* catgets  1245  */
        if (strcmp (qp->stopCond, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1246, "STOP_COND")), qp->stopCond); /* catgets  1246  */
        if (strcmp (qp->jobStarter, " ") != 0)
            printf("%s:  %s\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1247, "JOB_STARTER")), qp->jobStarter);   /* catgets  1247  */
        if (qp->qAttrib & Q_ATTRIB_RERUNNABLE)
            printf("RERUNNABLE :  yes\n");

        if ( qp->qAttrib & Q_ATTRIB_CHKPNT ) {
            printf((_i18n_msg_get(ls_catd,NL_SETN,1261, "CHKPNTDIR : %s\n")), qp->chkpntDir); /* catgets  1261  */
            printf((_i18n_msg_get(ls_catd,NL_SETN,1262, "CHKPNTPERIOD : %d\n")), qp->chkpntPeriod); /* catgets  1262  */
        }

        if (qp->qAttrib & Q_ATTRIB_ROUND_ROBIN)
            printf("ROUN_ROBIN_SCHEDULING:  yes\n");

        if (qp->actionComment && qp->actionComment[0] != '\0') {
            printf("%s: %s\n", _i18n_msg_get(ls_catd,NL_SETN,1260, "ADMIN ACTION COMMENT"), qp->actionComment);                                                                                
        }

        printf("\n");
        printFlag = 0;
        if  ((qp->suspendActCmd != NULL)
             && (qp->suspendActCmd[0] != ' '))
            printFlag = 1;

        printFlag1 = 0;
        if  ((qp->resumeActCmd != NULL)
             && (qp->resumeActCmd[0] != ' '))
            printFlag1 = 1;

        printFlag2 = 0;
        if  ((qp->terminateActCmd != NULL)
             && (qp->terminateActCmd[0] != ' '))
            printFlag2 = 1;

        if (printFlag || printFlag1 || printFlag2)
            printf("%s:\n",
                   (_i18n_msg_get(ls_catd,NL_SETN,1251, "JOB_CONTROLS"))); /* catgets  1251  */

        if (printFlag) {
            printf("    %-9.9s", (_i18n_msg_get(ls_catd,NL_SETN,1252, "SUSPEND:"))); /* catgets  1252  */
            if (strcmp (qp->suspendActCmd, " ") != 0)
                printf("    [%s]\n", qp->suspendActCmd);
        }

        if (printFlag1) {
            printf("    %-9.9s", (_i18n_msg_get(ls_catd,NL_SETN,1253, "RESUME:"))); /* catgets  1253  */
            if (strcmp (qp->resumeActCmd, " ") != 0)
                printf("    [%s]\n", qp->resumeActCmd);
        }

        if (printFlag2) {
            printf("    %-9.9s", (_i18n_msg_get(ls_catd,NL_SETN,1254, "TERMINATE:"))); /* catgets  1254  */
            if (strcmp (qp->terminateActCmd, " ") != 0)
                printf("    [%s]\n", qp->terminateActCmd);
        }

        if (printFlag || printFlag1 || printFlag2)
            printf("\n");

        printFlag = terminateWhen_(qp->sigMap, "USER");
        printFlag1 = terminateWhen_(qp->sigMap, "WINDOW");
        printFlag2 = terminateWhen_(qp->sigMap, "LOAD");

        if (printFlag | printFlag1 | printFlag2) {
            printf((_i18n_msg_get(ls_catd,NL_SETN,1255, "TERMINATE_WHEN = "))); /* catgets  1255  */
            if (printFlag) printf((_i18n_msg_get(ls_catd,NL_SETN,1256, "USER "))); /* catgets  1256  */
            if (printFlag1) printf((_i18n_msg_get(ls_catd,NL_SETN,1258, "WINDOW "))); /* catgets  1258  */
            if (printFlag2) printf((_i18n_msg_get(ls_catd,NL_SETN,1259, "LOAD"))); /* catgets  1259  */
            printf("\n");
        }
    }

}
static void
prtQueuesShort(int numQueues, struct queueInfoEnt *queueInfo)
{
    struct queueInfoEnt *qp;
    char statusStr[64];
    char first = FALSE;
    int i;
    char userJobLimit[MAX_CHARLEN],
        procJobLimit[MAX_CHARLEN],
        hostJobLimit[MAX_CHARLEN];
    char maxJobs[MAX_CHARLEN];

    if( !first ) {
        prtWord(QUEUE_NAME_LENGTH, I18N_QUEUE__NAME, 0);
        prtWord(QUEUE_PRIO_LENGTH, I18N_PRIO, 1);
        prtWord(QUEUE_STATUS_LENGTH, I18N_STATUS, 0);

        if ( lsbMode_ & LSB_MODE_BATCH ) {
            prtWord(QUEUE_MAX_LENGTH,  I18N_MAX,  -1);
            prtWord(QUEUE_JL_U_LENGTH, I18N_JL_U, -1);
            prtWord(QUEUE_JL_P_LENGTH, I18N_JL_P, -1);
            prtWord(QUEUE_JL_H_LENGTH, I18N_JL_H, -1);
        };

        prtWord(QUEUE_NJOBS_LENGTH, I18N_NJOBS, -1);
        prtWord(QUEUE_PEND_LENGTH,  I18N_PEND,  -1);
        prtWord(QUEUE_RUN_LENGTH,   I18N_RUN,   -1);
        prtWord(QUEUE_SUSP_LENGTH,  I18N_SUSP,  -1);
        printf("\n");
        first = TRUE;
    }

    for ( i=0; i<numQueues; i++) {

        qp = &(queueInfo[i]);

        if (qp->qStatus & QUEUE_STAT_OPEN) {
            sprintf(statusStr, "%s:", I18N_Open);
        }
        else {
            sprintf(statusStr, "%s:", I18N_Closed);
        }
        if (qp->qStatus & QUEUE_STAT_ACTIVE) {
            if (qp->qStatus & QUEUE_STAT_RUN) {
                strcat(statusStr, I18N_Active);
            } else {
                strcat(statusStr, I18N_Inact);
            }
        } else {
            strcat(statusStr, I18N_Inact);
        }


        if (qp->maxJobs < INFINIT_INT)
            strcpy(maxJobs, prtValue(QUEUE_MAX_LENGTH, qp->maxJobs) );
        else
            strcpy(maxJobs, prtDash(QUEUE_MAX_LENGTH) );


        if (qp->userJobLimit < INFINIT_INT)
            strcpy(userJobLimit,
                   prtValue(QUEUE_JL_U_LENGTH, qp->userJobLimit) );
        else
            strcpy(userJobLimit, prtDash(QUEUE_JL_U_LENGTH) );


        if (qp->procJobLimit < INFINIT_FLOAT) {
            sprintf(fomt, "%%%d.0f ", QUEUE_JL_P_LENGTH);
            sprintf (procJobLimit, fomt, qp->procJobLimit);
        }
        else
            strcpy(procJobLimit, prtDash(QUEUE_JL_P_LENGTH) );

        if (qp->hostJobLimit < INFINIT_INT)
            strcpy(hostJobLimit,
                   prtValue(QUEUE_JL_H_LENGTH, qp->hostJobLimit) );
        else
            strcpy(hostJobLimit, prtDash(QUEUE_JL_H_LENGTH) );

        if (wflag) {
            prtWordL(QUEUE_NAME_LENGTH, qp->queue);
            prtWordL(QUEUE_PRIO_LENGTH,
                     prtValue(3, qp->priority));
            prtWordL(QUEUE_STATUS_LENGTH, statusStr);
        } else {
            prtWord(QUEUE_NAME_LENGTH, qp->queue, 0);
            prtWord(QUEUE_PRIO_LENGTH,
                    prtValue(3, qp->priority), 1);
            prtWord(QUEUE_STATUS_LENGTH, statusStr, 1);
        }

        if ( lsbMode_ & LSB_MODE_BATCH ) {
            sprintf(fomt, "%%%ds%%%ds%%%ds%%%ds", QUEUE_MAX_LENGTH,
                    QUEUE_JL_U_LENGTH,
                    QUEUE_JL_P_LENGTH,
                    QUEUE_JL_H_LENGTH );
            printf(fomt,
                   maxJobs, userJobLimit, procJobLimit, hostJobLimit);
        }

        sprintf(fomt, "%%%dd %%%dd %%%dd %%%dd\n", QUEUE_NJOBS_LENGTH,
                QUEUE_PEND_LENGTH,
                QUEUE_RUN_LENGTH,
                QUEUE_SUSP_LENGTH );
        printf(fomt,
               qp->numJobs, qp->numPEND, qp->numRUN,
               (qp->numSSUSP + qp->numUSUSP) );
    }
}

static 
void printShareAcctTree(struct shareAcctInfoEnt *sAcctInfo, char * path) {
    int i = 0;
    int pos = strlen(path);

    if (!sAcctInfo && sAcctInfo->nChildShareAcct <= 0) {
        return;
    }

    /*Children is only 'default', we don't need print it because we only print shareAcct which had active jobs*/
    if (sAcctInfo->nChildShareAcct == 1 && 
        strcmp(sAcctInfo->childShareAccts[0].name, "default") == 0) {
        return;
    }

    snprintf(path+pos, MAXPATHLEN - pos, "%s/", sAcctInfo->name);
    printf("\nSHARE_INFO_FOR: %s", path);
    printf("\n");
    prtShareAcctHeader();
    printf("\n");
    for (i = 0; i < sAcctInfo->nChildShareAcct; i++ ) {
        prtShareAcct(&(sAcctInfo->childShareAccts[i]));
    }

    if (!rflag) {
        return;
    }

    pos = strlen(path);
    for (i = 0; i < sAcctInfo->nChildShareAcct; i++ ) {
        if (sAcctInfo->childShareAccts[i].nChildShareAcct > 0) {
            printShareAcctTree(&(sAcctInfo->childShareAccts[i]), path);
        }
        path[pos] = '\0';
    }

    return;
} /*printShareAcctTree*/

static void prtShareAcctHeader() {
    prtWord(QUEUE_FS_NAME, I18N_USER_GROUP, 0);
    prtWord(QUEUE_FS_SHARES, I18N_SHARES, -1);
    prtWord(QUEUE_FS_PRIO, I18N_PRIORITY, -1);
    prtWord(QUEUE_FS_STARTED, I18N_STARTED, -1);
    prtWord(QUEUE_FS_CPUTIME, I18N_CPU_TIME, -1);
    prtWord(QUEUE_FS_RUNTIME, I18N_RUN_TIME, -1);
    return;
} /*prtShareAcctHeader*/

static void
prtSAcctName(int len, const char *word)
{
    char fomt[200];

       sprintf(fomt, " %%-%ds ",len-1);
       printf(fomt, word);
}

static void prtShareAcct(struct shareAcctInfoEnt *sAcctInfo) {
    if (strcmp(sAcctInfo->name, "default") == 0) {
        return;
    }
    prtSAcctName(QUEUE_FS_NAME, sAcctInfo->name);
    prtWord2(QUEUE_FS_SHARES, prtValue(QUEUE_FS_SHARES, sAcctInfo->share), -1);
    prtWord2(QUEUE_FS_PRIO, prtFloat(QUEUE_FS_PRIO, 3, sAcctInfo->priority), -1);
    prtWord2(QUEUE_FS_STARTED, prtValue(QUEUE_FS_STARTED, sAcctInfo->numStartJobs), -1);
    prtWord(QUEUE_FS_CPUTIME, prtFloat(QUEUE_FS_CPUTIME, 1, sAcctInfo->cpuTime), -1);
    prtWord2(QUEUE_FS_RUNTIME, prtValue(QUEUE_FS_RUNTIME, sAcctInfo->runTime), -1);
    printf("\n");
    return;
} /*prtShareAcctHeader*/

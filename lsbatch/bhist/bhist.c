/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * $Id: bhist.c 397 2007-11-26 19:04:00Z mblack $
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
#include <ctype.h>
#include "bhist.h"

#include "../../lsf/intlib/intlibout.h"

#define NegtoZero(x)  ( (x<0)? 0 :x )
#define topOrBot(x)   ( ((x)==1) ? "top" : "bottom" )

#define NL_SETN  6

void usage(char *);
void fileNumbers(char* , struct bhistReq *);
static int do_options(int, char **, struct bhistReq *);
static void displayhist(struct bhistReq *);
static int  skip_jobRecord(struct jobRecord *, struct bhistReq *);
static char logfile_check(struct bhistReq *);
static char *getEventStatus(struct eventRecord *);
static void initLoadIndexNames(void);
static void prtParameters(struct jobInfoEnt *, struct bhistReq *, char *);
static char * getUserName(int);
static int dispChkpnt (struct eventRecord *, struct jobRecord *);
static void readEventFromHead(char *, struct bhistReq *);
static void printEvent(struct bhistReq *, struct jobRecord *, struct jobInfoEnt *, struct eventRecord *, time_t, char *, int);
static void printChronicleEventLog(struct eventRec *, struct bhistReq *);
static char * lowFirstChar(char *);

static void prtModifiedJob(struct jobModLog *, struct bhistReq *, char *);
static int initJobIdIndexS( struct jobIdIndexS *indexS, char *fileName );

static int hspecf=0;
static char foundJob = FALSE;
char * actionName (char *sigName);
extern char read_jobforw(struct eventRec *log);
extern char read_jobaccept(struct eventRec *log);
extern char read_sigact(struct eventRec *log);
extern void displayEvent(struct eventRec *, struct histReq *);

extern struct eventLogHandle *lsb_openelog(struct eventLogFile *, int *);
extern struct eventRec *lsb_getelogrec(struct eventLogHandle *, int *);
extern void countLineNum(FILE *fp, long, int *);
extern struct eventRec *lsbGetNextJobEvent(struct eventLogHandle *, int *, int, LS_LONG_INT *, struct jobIdIndexS *);


struct hTab jobIdHT;
struct jobRecord *jobRecordList;
struct loadIndexLog *loadIndex;
struct bhistReq      Req;
int readFromHeadFlag;
struct eventLogHandle *eLogPtr=NULL;

struct jobIdIndexS *jobIdIndexSPtr;
struct jobIdIndexS jobIdIndexStr;
time_t runningTime;
struct config_param bhistParams[] = {
#define LSB_SHAREDIR 0
    {"LSB_SHAREDIR", NULL},
    {NULL, NULL}
};

#define DEFAULT_CHRONICLE_SEARCH_TIME   7*24*60*60

void
usage(char * cmd)
{
    fprintf(stderr, I18N_Usage );
    fprintf(stderr, ": %s [-h] [-V] [-l] [-b] [-w] [-a] [-d] [-e] [-p] [-s] [-r] \n", cmd);

    fprintf(stderr,"             [-f logfile_name | -n num_logfiles | -n min_logfile, max_logfile]\n");
    fprintf(stderr,"             [-C time0,time1] [-S time0,time1] [-D time0,time1]\n");

    if (lsbMode_ & LSB_MODE_BATCH)
	fprintf(stderr,"             [-N host_spec] [-P project_name]\n");

    fprintf(stderr,"             [-q queue_name] [-m host_name] [-J job_name]\n");
    fprintf(stderr,"             [-u user_name | -u all]");

    if (lsbMode_ & LSB_MODE_BATCH)
	fprintf(stderr, " [jobId | \"jobId[index]\" ...]\n");
    else
        fprintf(stderr," [jobId ...]\n");
    fprintf(stderr, "       %s [-h] [-V] -t [-f logfile_name] [-T time0,time1]\n", cmd);

    exit(-1);
}



void fileNumbers(char* inputstr, struct bhistReq *Req)
{
    char *res = strchr(inputstr, ',');
    if (res == NULL) {
        Req->numMinLogFile = 0;
        if (!isint_(inputstr) || (Req->numLogFile = atoi(inputstr)) < 0) {
            fprintf(stderr, "bhist: ");
            fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3176, "The number of event log files must be a positive integer.\n"))); /* catgets  3176  */
            exit (-1);
        }
    } else {
        int pos;
        char *first, *second;
        pos = res - inputstr;
        first = (char*) malloc(pos+1);
        second = (char*) malloc(strlen(inputstr)-pos);
        strncpy(first, (const char *) inputstr, (size_t) pos);
        first[pos] = '\0';
        strcpy(second, (const char *) ++res);


        if ( !isint_(first) || !isint_(second)
             || (Req->numMinLogFile = atoi(first)) <0
             || (Req->numLogFile = atoi(second)) < 0 )  {
            fprintf(stderr, "bhist: ");
            fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3267, "The numbers of both event log files must be positive integers.\n"))); /* catgets  3267  */
            exit (-1);
        }



        if (Req->numMinLogFile > Req->numLogFile) {
            fprintf(stderr, "bhist: ");
            fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3297,
                                           "The value of min_logfile should not exceed the value of max_logfile\n"))); /* catgets  3297  */
            exit (-1);
        }

        free(first);
        free(second);
    }


    if (Req->numMinLogFile)
        ++Req->numLogFile;
}


int
main(int argc, char **argv)
{

    int rc;

    rc = _i18n_init ( I18N_CAT_MIN );

    if (lsb_init("bhist") < 0)  {
        lsb_perror("lsb_init");
        exit(-1);
    }

    if (do_options(argc, argv, &Req) == -1)
        exit(-1);


    h_initTab_(&jobIdHT, 50);

    initLoadIndexNames();


    jobRecordList = (struct jobRecord *) initJobList();

    logfile_check(&Req);

    if (Req.options & OPT_CHRONICLE)
    {

        if (!foundJob)
        {
            printf((_i18n_msg_get(ls_catd,NL_SETN,3160, "No matching job/event found\n"))); /* catgets  3160  */
            exit( -1 );
        }
        else
            printf("\n");
        exit(0);
    }

    displayhist(&Req);

    if (!foundJob) {
        if (Req.options & OPT_ARRAY_INFO)
            printf((_i18n_msg_get(ls_catd,NL_SETN,3161, "No matching job array found\n"))); /* catgets  3161  */
        else
            printf((_i18n_msg_get(ls_catd,NL_SETN,3162, "No matching job found\n"))); /* catgets  3162  */

        exit( -1 );
    }
    else
        printf("\n");

    _i18n_end ( ls_catd );

    exit(0);
}


static int
do_options(int argc, char **argv, struct bhistReq *bhistReq)
{

    extern char *optarg;
    char *envHours;
    struct passwd *pwPtr;
    time_t defaultTime[2];
    int cc;
    float *tempPtr;
    int numJobs, *idxList;
    struct hostent *hp;
    extern int idxerrno;

    defaultTime[1] = time(0);
    runningTime = defaultTime[1];

    if ((envHours = getenv("LSB_BHIST_HOURS")) != NULL)
        defaultTime[0] = defaultTime[1] - atoi(envHours) * 60 * 60;
    else
        defaultTime[0] = 0;

    if (bhistReqInit(bhistReq) < 0) {
	return -1;
    }

    while ((cc = getopt(argc, argv, "VaAhwlbdepstrf:q:m:u:C:D:S:N:J:P:T:n:")) != EOF)
    {
	switch (cc) {
            case 'A':
                bhistReq->options |= OPT_ARRAY_INFO;
                break;
            case 'a':
                bhistReq->options |= OPT_ALL;
                bhistReq->options &= ~OPT_DFTSTATUS;
                bhistReq->options &= ~OPT_DONE;
                bhistReq->options &= ~OPT_EXIT;
                bhistReq->options &= ~OPT_PEND;
                bhistReq->options &= ~OPT_SUSP;
                bhistReq->options &= ~OPT_RUN;
                break;
            case 'b':
                if ((bhistReq->options & OPT_LONGFORMAT) != OPT_LONGFORMAT) {
                    bhistReq->options &= ~OPT_DFTFORMAT;
                    bhistReq->options |= OPT_SHORTFORMAT;
                }
                break;
            case 't':
                bhistReq->options |= OPT_CHRONICLE;
                bhistReq->options &= ~OPT_DFTSTATUS;
                bhistReq->options &= ~OPT_DFTFORMAT;
                bhistReq->options &= ~OPT_ALLPROJ;
                break;
            case 'l':
                bhistReq->options &= ~OPT_DFTFORMAT;
                bhistReq->options &= ~OPT_SHORTFORMAT;
                bhistReq->options |= OPT_LONGFORMAT;
                break;
            case 'd':
                if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
                    bhistReq->options &= ~OPT_DFTSTATUS;
                    bhistReq->options &= ~OPT_EXIT;
                    bhistReq->options |= OPT_DONE;
                }
                break;
            case 'e':
                if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
                    bhistReq->options &= ~OPT_DFTSTATUS;
                    bhistReq->options &= ~OPT_DONE;
                    bhistReq->options |= OPT_EXIT;
                }
                break;
            case 'p':
                if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
                    bhistReq->options &= ~OPT_DFTSTATUS;
                    bhistReq->options |= OPT_PEND;
                }
                break;
            case 's':
                if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
                    bhistReq->options &= ~OPT_DFTSTATUS;
                    bhistReq->options |= OPT_SUSP;
                }
                break;
            case 'r':
                if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
                    bhistReq->options &= ~OPT_DFTSTATUS;
                    bhistReq->options |= OPT_RUN;
                }
                break;
            case 'f':
                if (bhistReq->options & OPT_ELOGFILE)
                    usage(argv[0]);
                bhistReq->options |= OPT_ELOGFILE;
                if (bhistReq->options & OPT_NUMLOGFILE) {
                    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3164, "Cannot use option %s together with %s.\n")), "'-f'", "'-n'"); /* catgets  3164  */
                    return (-1);
                }
                if (optarg && strlen(optarg) < MAXFILENAMELEN -1) {
                    strcpy(bhistReq->eventFileName, optarg);
                    break;
                } else {
                    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3166, "%s : Bad event file name.\n")),optarg); /* catgets  3166  */
                    return (-1);
                }
            case 'u':
                if ( strcmp(optarg, "all") == 0 ) {
                    bhistReq->options |= OPT_ALLUSERS;
                    bhistReq->options &= ~OPT_USER;
                }
                else if ((bhistReq->options & OPT_ALLUSERS) != OPT_ALLUSERS)
                {
                    bhistReq->options |= OPT_USER;
                    if ((pwPtr = getpwlsfuser_(optarg)) != NULL) {
                        bhistReq->userId = pwPtr->pw_uid;
                    } else {
                        bhistReq->userId = -1;
                    }
                    strcpy(bhistReq->userName, optarg);
                }
                break;

            case 'w':
                bhistReq->options |= OPT_WIDEFORMAT;
                break;

            case 'P':
                if (bhistReq->options & OPT_PROJ)
                    usage(argv[0]);
                if (optarg && strlen(optarg) < MAX_LSB_NAME_LEN - 1) {
                    bhistReq->options |= OPT_PROJ;
                    bhistReq->options &= ~OPT_ALLPROJ;
                    strcpy(bhistReq->projectName, optarg);
                } else {
                    fprintf(stderr, "%s, : ", optarg);
                    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3167, "project name too long.\n"))); /* catgets  3167  */
                    return(-1);
                }
                break;

            case 'm':
                if (bhistReq->options & OPT_HOST)
                    usage(argv[0]);
                bhistReq->options |= OPT_HOST;
                if ((hp = Gethostbyname_(optarg)) == NULL) {
                    fprintf(stderr,
                            (_i18n_msg_get(ls_catd,NL_SETN,3168, "Warning: <%s> is not a valid hostname\n")),  /* catgets  3168  */
                            optarg);
                    if (strlen (optarg) < MAXHOSTNAMELEN -1)
                        strcpy(bhistReq->checkHost, optarg);
                    else {
                        fprintf(stderr, "%s : %s.\n",
                                optarg,
                                (_i18n_msg_get(ls_catd,NL_SETN,3169, "Bad host name"))); /* catgets  3169  */
                        return  (-1);
                    }
                } else
                    strcpy(bhistReq->checkHost, hp->h_name);
                hspecf = 1;
                break;
            case 'q':
                if (bhistReq->options & OPT_QUEUE)
                    usage(argv[0]);
                bhistReq->options |= OPT_QUEUE;
                if (optarg && strlen (optarg) < MAX_LSB_NAME_LEN - 1) {
                    strcpy(bhistReq->queue, optarg);
                    break;
                } else {
                    fprintf(stderr, "%s : %s.\n",
                            optarg,
                            _i18n_msg_get(ls_catd,NL_SETN,3170, "Bad queue name")); /* catgets 3170 */
                    return (-1);
                }
            case 'C':
                bhistReq->options |= OPT_COMPLETE;
                if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
                    bhistReq->options &= ~OPT_DFTSTATUS;
                }
                if ((bhistReq->options & OPT_EXIT) != OPT_EXIT) {
                    bhistReq->options |= OPT_DONE;
                }
                if (getBEtime(optarg, 't', bhistReq->endTime) == -1) {
                    ls_perror(optarg);
                    return (-1);
                }

                break;
            case 'S':
                bhistReq->options |= OPT_SUBMIT;
                if (getBEtime(optarg, 't', bhistReq->submitTime) == -1) {
                    ls_perror(optarg);
                    return (-1);
                }
                break;
            case 'D':
                bhistReq->options |= OPT_DISPATCH;
                if (getBEtime(optarg, 't', bhistReq->startTime) == -1) {
                    ls_perror(optarg);
                    return (-1);
                }
                break;

            case 'N':
                bhistReq->options |= OPT_NORMALIZECPU;
                if ((tempPtr = ls_getmodelfactor(optarg)) == NULL)
                    if ((tempPtr = ls_gethostfactor(optarg)) == NULL)
                        if ((!isanumber_(optarg)) ||
                            ((bhistReq->cpuFactor = atof(optarg)) <= 0)) {
                            fprintf(stderr,"<%s> %s.\n",
                                    optarg,
                                    (_i18n_msg_get(ls_catd,NL_SETN,3171, "neither a host model, nor a host name, nor a CPU factor"))); /* catgets  3171  */
                            return (-1);
                        }
                if (tempPtr)
                    bhistReq->cpuFactor = *tempPtr;
                break;

            case 'T':
                if (bhistReq->options & OPT_TIME_INTERVAL)
                    usage(argv[0]);

                bhistReq->options |= OPT_TIME_INTERVAL;

                if (getBEtime(optarg, 't', bhistReq->searchTime) == -1) {
                    ls_perror(optarg);
                    return (-1);
                }
                break;

            case 'V':
                fputs(_LS_VERSION_, stdout);
                exit(0);
            case 'J':
                bhistReq->options |= OPT_JOBNAME;
                bhistReq->jobName = optarg;

                if ((numJobs = getSpecIdxs(bhistReq->jobName, &idxList))==0 && idxerrno!=0)
                    return (-1);
                break;
            case 'n':
                bhistReq->options |= OPT_NUMLOGFILE;
                if (bhistReq->options & OPT_ELOGFILE) {
                    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,3164, "Cannot use option %s together with %s.\n")), "'-n'", "'-f'"); /* catgets  3164  */
                    return (-1);
                }
                fileNumbers(optarg, bhistReq);
                if (bhistReq->numLogFile == 0)
                    bhistReq->numLogFile = MAX_EVENT_FILE;
                break;
            case 'h':
            default:
                usage(argv[0]);
	}
    }

    bhistReq->numJobs = getSpecJobIds (argc, argv, &bhistReq->jobIds, NULL);

    if (bhistReq->options & OPT_CHRONICLE)
    {
	if (bhistReq->numJobs > 0)
	    usage(argv[0]);

	if (bhistReq->options &
            ~OPT_CHRONICLE &
            ~OPT_TIME_INTERVAL &
            ~OPT_ELOGFILE)
	    usage(argv[0]);
        if (bhistReq->searchTime[0] == -1)
	{
	    if (defaultTime[0] == 0)
            	bhistReq->searchTime[0] = defaultTime[1] -
                    DEFAULT_CHRONICLE_SEARCH_TIME;
	    else
		bhistReq->searchTime[0] = defaultTime[0];
            bhistReq->searchTime[1] = defaultTime[1];
        }

        return (0);
    }


    if (!(bhistReq->options & OPT_TIME_INTERVAL))
    {
        if ((bhistReq->numJobs > 0) ||
	    (bhistReq->options & OPT_COMPLETE) ||
	    (bhistReq->options & OPT_SUBMIT) ||
            (bhistReq->options & OPT_NUMLOGFILE) ||
	    (bhistReq->options & OPT_DISPATCH))
        {
            if ((lsbMode_ & LSB_MODE_BATCH) &&
                !(bhistReq->options &
                  (OPT_NUMLOGFILE | OPT_TIME_INTERVAL |
                   OPT_SUBMIT | OPT_DISPATCH | OPT_ELOGFILE))) {
                if (defaultTime[0] == 0)
                    bhistReq->searchTime[0] = defaultTime[1] -
			DEFAULT_CHRONICLE_SEARCH_TIME;
                else
                    bhistReq->searchTime[0] = defaultTime[0];
                bhistReq->searchTime[1] = defaultTime[1];
            }
            else {
                bhistReq->searchTime[0] = defaultTime[0];
                bhistReq->searchTime[1] = defaultTime[1];
            }
        }

        if (!(bhistReq->options & OPT_NUMLOGFILE) &&
            (lsbMode_ == LSB_MODE_BATCH) &&
            !((defaultTime[0] != 0) &&
              (bhistReq->options & OPT_LONGFORMAT)))
        {
            bhistReq->searchTime[0] = -1;
            bhistReq->searchTime[1] = -1;
        }

    }

    if (bhistReq->numJobs > 0) {
	bhistReq->options |= OPT_JOBID;
	bhistReq->options |= OPT_ALLUSERS;
	bhistReq->options |= OPT_ALL;
	bhistReq->options &= ~OPT_USER;
	bhistReq->options &= ~OPT_DONE;
	bhistReq->options &= ~OPT_EXIT;
	bhistReq->options &= ~OPT_PEND;
	bhistReq->options &= ~OPT_SUSP;
	bhistReq->options &= ~OPT_RUN;
	bhistReq->options &= ~OPT_QUEUE;
	bhistReq->options &= ~OPT_HOST;
	bhistReq->options &= ~OPT_COMPLETE;
	bhistReq->options &= ~OPT_SUBMIT;
	bhistReq->options &= ~OPT_DISPATCH;
	bhistReq->options &= ~OPT_DFTSTATUS;
    }

    if (bhistReq->options & OPT_NORMALIZECPU)
        if ((bhistReq->options & OPT_ALL) != OPT_ALL) {
            bhistReq->options &= ~OPT_PEND;
            bhistReq->options &= ~OPT_SUSP;
            bhistReq->options &= ~OPT_RUN;
            if (((bhistReq->options & OPT_DONE) != OPT_DONE)
		|| ((bhistReq->options & OPT_EXIT) != OPT_EXIT))
                bhistReq->options |= OPT_ALL;
        }
    return (0);

}

static char *
getEventStatus(struct eventRecord *event)
{
    static char fname[] = "getEventStatus";
    static char status[MSGSIZE];
    LS_WAIT_T wStatus;


    switch ( event->jStatus & ( JOB_STAT_PDONE | JOB_STAT_PERR ) ) {
	case JOB_STAT_PDONE:
	    strcpy (status,
		    (_i18n_msg_get(ls_catd,NL_SETN, 3174,
				   "Post job process done successfully"))); /* catgets  3174  */
	    break;
	case JOB_STAT_PERR:
	    strcpy (status,
		    (_i18n_msg_get(ls_catd,NL_SETN, 3175,
				   "Post job process failed"  /* catgets  3175  */
                        ) ) );
	    break;
    }
    if ( IS_POST_FINISH(event->jStatus) ) {
	goto succeed_rtn;
    }

    switch(event->jStatus) {
        case JOB_STAT_NULL:
            strcpy (status, "Null");
            break;
        case JOB_STAT_PEND:
            LS_STATUS(wStatus) = event->exitStatus;
            sprintf (status, (_i18n_msg_get(ls_catd,NL_SETN,3178, "Pending:%s")), lsb_pendreason(1, &event->reasons,  /* catgets  3178  */
                                                                                                 NULL, event->ld));
            if ((event->reasons == PEND_SBD_JOB_REQUEUE ||
                 event->reasons == PEND_QUE_PRE_FAIL ||
                 event->reasons == PEND_JOB_PRE_EXEC) && WEXITSTATUS(wStatus)) {
                char *sp;
                sp = status;
                while (*sp != '\n')
                    sp++;
                sprintf (sp, (_i18n_msg_get(ls_catd,NL_SETN,3179, "(exit code %d)\n")), WEXITSTATUS(wStatus)); /* catgets  3179  */
            }
            break;
        case JOB_STAT_RUN:
            strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3180, "Running"))); /* catgets  3180  */
            break;
        case JOB_STAT_RUN | JOB_STAT_WAIT :
            strcpy (status, I18N(3427, "Waiting")); /* catgets  3427  */
            break;
        case JOB_STAT_SSUSP:

            if (event->reasons & SUSP_RES_LIMIT) {
                sprintf (status, "%s: %s",
                         _i18n_msg_get(ls_catd,NL_SETN,3196, "Terminated"),  /* catgets  3196  */
                         lsb_suspreason((event->reasons & ~SUSP_MBD_LOCK),
                                        event->subreasons, event->ld));
            }
            else {
                sprintf (status, "%s: %s",
                         _i18n_msg_get(ls_catd,NL_SETN,3181, "Suspended"),  /* catgets  3181  */
                         lsb_suspreason((event->reasons & ~SUSP_MBD_LOCK),
                                        event->subreasons, event->ld));
            }
            break;
        case JOB_STAT_USUSP:
            strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3182, "Suspended by the user or administrator"))); /* catgets  3182  */
            break;
        case JOB_STAT_PSUSP:
            strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3183, "Suspended by the user or administrator while pending"))); /* catgets  3183  */
            break;
        case JOB_STAT_UNKWN:
            strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3184, "Unknown; unable to reach the execution host"))); /* catgets  3184  */
            break;
        case JOB_STAT_EXIT:
            if (event->reasons & EXIT_ZOMBIE) {
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3185, "Execution host unavailable; job is killed"))); /* catgets  3185  */
            } else if (event->reasons & EXIT_KILL_ZOMBIE)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3186, "Termination request issued; but unable to reach the execution host"))); /* catgets  3186  */
            else if (event->reasons & EXIT_ZOMBIE_JOB)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3187, "sbatchd startup; kill ZOMBIE job"))); /* catgets  3187  */
            else if (event->reasons & EXIT_RERUN)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3188, "Exited; job will be requeued and rerun")));  /* catgets  3188  */
            else if (event->reasons & EXIT_NO_MAPPING)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3189, "Exited; remote job has no user mapping for the local cluster"))); /* catgets  3189  */
            else if (event->reasons & EXIT_INIT_ENVIRON)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3191, "Exited; remote job cannot run locally because of environment problem")));  /* catgets  3191  */
            else if (event->reasons & EXIT_PRE_EXEC)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3192, "Exited; remote job's pre-exec command failed"))); /* catgets  3192  */
            else if (event->reasons & EXIT_REMOVE)
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3428, "Exited; job has been forced to exit"))); /* catgets 3428  */
            else
                strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3193, "Exited"))); /* catgets  3193  */
            break;
        case JOB_STAT_DONE:
            strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3194, "Done successfully"))); /* catgets  3194  */
            break;
        default:
            strcpy (status, (_i18n_msg_get(ls_catd,NL_SETN,3195, "Unknown"))); /* catgets  3195  */
            break;
    }

succeed_rtn:
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG2, "%s: Job status is %s/%x",
                  fname, status, event->jStatus);
    return status;
}

static int
skip_jobRecord(struct jobRecord *jobRecord, struct bhistReq *Req)
{
    int i, options, found = FALSE;
    options = Req->options;

    if (!matchJobId(Req, jobRecord->job->jobId))
        return(TRUE);
    if ( ((options & OPT_ALLUSERS) != OPT_ALLUSERS) &&
         (strcmp(jobRecord->job->user, Req->userName)))
        return(TRUE);

    if ( ((options & OPT_ALLPROJ) != OPT_ALLPROJ) &&
         (strcmp(jobRecord->job->submit.projectName, Req->projectName)))
        return(TRUE);

    if (options & OPT_ARRAY_INFO) {
        if (strchr(jobRecord->job->submit.jobName, '[') == NULL)
            return(TRUE);
        else
            return(FALSE);
    }
    if ((options & OPT_ALL) != OPT_ALL && !(options & OPT_CHRONICLE)) {
        if ((options & OPT_DFTSTATUS) != OPT_DFTSTATUS) {
            if (!(((options & OPT_DONE) &&
                   (jobRecord->currentStatus & JOB_STAT_DONE))
                  || ((options & OPT_DONE) &&
                      (jobRecord->currentStatus & JOB_STAT_EXIT))
                  || ((options & OPT_EXIT) &&
                      (jobRecord->currentStatus & JOB_STAT_EXIT))
                  || ((options & OPT_PEND) && (IS_PEND (jobRecord->currentStatus)))
                  || ((options & OPT_SUSP) && (jobRecord->currentStatus
                                               & (JOB_STAT_SSUSP | JOB_STAT_USUSP)))
                  || ((options & OPT_RUN) &&
                      (jobRecord->currentStatus & JOB_STAT_RUN))))
                return(TRUE);
        }
        else {
            if (jobRecord->currentStatus & (JOB_STAT_DONE |JOB_STAT_EXIT))
	        return(TRUE);
        }
    }

    if ( (options & OPT_QUEUE) &&
         (strcmp(jobRecord->job->submit.queue, Req->queue) != 0) )
        return(TRUE);
    if (options & OPT_HOST)  {
        if  (jobRecord->job->startTime == 0) return(TRUE);
        for (i=0; i < jobRecord->job->numExHosts; i++) {
	    if (strcmp(jobRecord->job->exHosts[i], Req->checkHost) == 0) {
	        found = TRUE;
	        break;
            }
        }
        if (found == FALSE)
	    return(TRUE);
    }

    if (options & OPT_COMPLETE) {
        if (jobRecord->job->endTime == 0) return(TRUE);
        if ((jobRecord->job->endTime < Req->endTime[0]) ||
            (jobRecord->job->endTime > Req->endTime[1]))
	    return(TRUE);
    }

    if (options & OPT_SUBMIT) {
        if (jobRecord->job->submitTime == 0) return(TRUE);
        if ((jobRecord->job->submitTime < Req->submitTime[0]) ||
            (jobRecord->job->submitTime > Req->submitTime[1]))
	    return(TRUE);
    }

    if (options & OPT_DISPATCH) {
        if (jobRecord->job->startTime == 0) return(TRUE);
        if ((jobRecord->job->startTime < Req->startTime[0]) ||
            (jobRecord->job->startTime > Req->startTime[1]))
	    return(TRUE);
    }

    return(FALSE);
}

static void
displayhist(struct bhistReq *bhistReq)
{
    static char fname[] = "displayhist";
    struct jobRecord *jobRecord;
    struct jobInfoEnt   *job;
    struct eventRecord  *event, *nextevent, *lastevent;
    static char first = TRUE;
    char   *currentOrDoneTime = NULL;
    char   timestamp[64];
    char   daytime[64], *hostPtr;
    double stayTime = 0, sysSuspTime, usrSuspTime;
    double pendSuspTime, runTime, pendTime, unknownTime;
    double timeinterval;
    time_t now, nextTimeStamp, timeStamp, startTime, currentTime;
    int laststatus;

    now = time(0);
    strcpy(daytime, ctime((time_t *)&now));


    jobRecord = jobRecordList->forw;

    while(jobRecord != jobRecordList) {


        if (skip_jobRecord(jobRecord, bhistReq)) {
            jobRecord = jobRecord->forw;
            continue;
        }
	foundJob = TRUE;

        job = jobRecord->job;
        if ( !(bhistReq->options & OPT_CHRONICLE)) {
            if ( !(bhistReq->options & OPT_DFTFORMAT)) {
                if (!first)
                    prtLine("------------------------------------------------------------------------------\n");
                prtHeader(job, FALSE, FALSE);
                prtJobSubmit(job, TRUE, FALSE);
                if (bhistReq->options & OPT_LONGFORMAT) {
                    prtFileNames(job, TRUE);
                    hostPtr = job->submit.hostSpec;
                    if (strcmp(job->submit.hostSpec, "") == 0) {
                        if (job->numExHosts > 0)
                            hostPtr = job->exHosts[0];
                        else
                            hostPtr = job->fromHost;
                    }
                    prtSubDetails(job, hostPtr, jobRecord->hostFactor);
                }
                else
                    prtLine(";\n");
            }



            pendTime     = 0.0;
            pendSuspTime = 0.0;
            runTime      = 0.0;
            sysSuspTime  = 0.0;
            usrSuspTime  = 0.0;
            unknownTime  = 0.0;

            nextTimeStamp = 0;
            startTime = 0;
        }
        event = jobRecord->eventhead;

        while(event) {

	    if ((bhistReq->options & OPT_CHRONICLE)) {
                jobRecord = event->jobR;
                if (skip_jobRecord(jobRecord, bhistReq)) {
                    if ((event = event->chronback))
                        jobRecord = event->jobR;
                    continue;
                }
                job = jobRecord->job;
                nextevent = event->chronback;
                pendTime = 0.0;
                pendSuspTime = 0.0;
                runTime = 0.0;
                sysSuspTime = 0.0;
                usrSuspTime = 0.0;
                unknownTime = 0.0;
                nextTimeStamp = 0;
                startTime = 0;
            }
            else
                nextevent  = event->next;



            if (logclass & LC_TRACE)
                ls_syslog(LOG_DEBUG2, "%s: (1)event.kind=%x",
                          fname, event->kind);
            if (nextevent && (logclass & LC_TRACE))
                ls_syslog(LOG_DEBUG2, "%s: (1)nextevent.kind=%x",
                          fname, nextevent->kind);
	    while ((nextevent != NULL) &&
		   ((nextevent->kind == EVENT_JOB_SWITCH))) {
	        if((bhistReq->options & OPT_CHRONICLE) == OPT_CHRONICLE){
                    nextevent = nextevent->chronback;
                } else {
                    nextevent = nextevent->next;
                }
            }
	    if (nextTimeStamp > 0) {



	        timeStamp = nextTimeStamp;
            } else {
	        timeStamp = event->timeStamp;
            }
	    strcpy(timestamp, _i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T,
                                          (time_t *)&(timeStamp)));

	    if (event->jStatus & (JOB_STAT_DONE | JOB_STAT_EXIT)) {
                if ((bhistReq->options & OPT_NORMALIZECPU) && job->cpuTime > 0)
                    job->cpuTime = job->cpuTime * job->cpuFactor
                        / bhistReq->cpuFactor;
            }
            if (matchJobId(bhistReq, LSB_JOBID((int)job->jobId, event->idx))){
                printEvent(bhistReq,
                           jobRecord,
                           job,
                           event,
                           timeStamp,
                           timestamp,
                           0);

            }

            if (nextevent == NULL) {

                if ((event->jStatus & (JOB_STAT_DONE) ||
		     ((event->jStatus & JOB_STAT_EXIT) &&
                      !(event->reasons & (FINISH_PEND | EXIT_ZOMBIE |
                                          EXIT_RERUN | EXIT_KILL_ZOMBIE))))) {
		    stayTime = (double) timeStamp - (double)job->submitTime;
                    currentOrDoneTime = putstr_(_i18n_ctime(ls_catd,  CTIME_FORMAT_a_b_d_T, (time_t *)&(timeStamp)));
                }
                else {
	 	    stayTime = now - job->submitTime;

                    if ((bhistReq->searchTime[1] < runningTime) && (bhistReq->searchTime[1] != -1)) {
                        event->jStatus = JOB_STAT_EXIT;
                    }

		    if (stayTime < 0) {
		        stayTime = pendTime + pendSuspTime + runTime
			    + sysSuspTime + usrSuspTime + unknownTime;
                        currentTime = job->submitTime + (time_t) stayTime;
		        currentOrDoneTime = putstr_(_i18n_ctime(ls_catd,  CTIME_FORMAT_a_b_d_T, (time_t *)&(currentTime)));
		    } else
                        currentOrDoneTime = putstr_(_i18n_ctime(ls_catd,  CTIME_FORMAT_a_b_d_T, (time_t *)&( now )));
                }
                timeinterval = (double) now - (double) timeStamp;

	        if (timeinterval < 0)
		    timeinterval = 0;
            }
	    else {
                timeinterval = (double)nextevent->timeStamp - (double)timeStamp;

                if (timeinterval < 0) {
	 	    timeinterval = 0;
		    nextTimeStamp = timeStamp;
                }
	        else
	 	    nextTimeStamp = 0;
	    }

	    lastevent = event;

            if (!matchJobId(bhistReq, LSB_JOBID((int)job->jobId, lastevent->idx)) &&
		!matchJobId(bhistReq, job->jobId) ) {
                goto End;
            }

	    if ( event->jStatus == 0) {
                event->jStatus = laststatus;
            }

            if ((event->kind != EVENT_JOB_SWITCH)) {


		switch (event->jStatus) {
                    case JOB_STAT_PSUSP:
                        pendSuspTime += timeinterval;
                        break;

                    case JOB_STAT_PEND:
                        pendTime += timeinterval;
                        break;

                    case JOB_STAT_RUN | JOB_STAT_WAIT:
                        pendTime += timeinterval;
                        break;

                    case JOB_STAT_RUN:
                        startTime = timeStamp;
                        runTime += timeinterval;
                        break;

                    case JOB_STAT_SSUSP:
                        sysSuspTime += timeinterval;
                        break;

                    case JOB_STAT_USUSP:

                        if(laststatus == (JOB_STAT_RUN | JOB_STAT_WAIT)) {
                            pendTime += timeinterval;
                        }else{
                            usrSuspTime += timeinterval;
                        }
                        break;

                    case JOB_STAT_DONE:
                        if ( event->reasons & FINISH_PEND
                             || (nextevent && !(nextevent->jStatus & JOB_STAT_PDONE) ) ){
                            pendTime += timeinterval;
                        }
                        break;

                    case JOB_STAT_EXIT:
                        if (event->reasons &
                            (EXIT_RERUN | FINISH_PEND) || nextevent)
                            pendTime += timeinterval;
                        if (event->reasons & (EXIT_KILL_ZOMBIE | EXIT_ZOMBIE))
                            unknownTime += timeinterval;
                        break;

                    default:

                        if ( !IS_POST_FINISH(event->jStatus) ) {
                            unknownTime += timeinterval;
                        }
                        break;
		}
	    }
	    laststatus = event->jStatus;

            if (((bhistReq->options & OPT_DFTFORMAT) != OPT_DFTFORMAT) &&
                (lastevent->kind != EVENT_JOB_NEW) &&
                (lastevent->kind != EVENT_JOB_MODIFY) &&
                (lastevent->kind != EVENT_JOB_EXECUTE)) {

                if (lastevent->kind == EVENT_CHKPNT ||
		    lastevent->kind == EVENT_MIG ||
                    lastevent->kind == EVENT_JOB_SWITCH ||
                    lastevent->kind == EVENT_JOB_SIGNAL ||
                    lastevent->kind == EVENT_JOB_SIGACT ||
                    lastevent->kind == EVENT_JOB_REQUEUE ||
		    lastevent->kind == EVENT_JOB_MODIFY2 ||
		    lastevent->kind == EVENT_JOB_FORCE ||
                    lastevent->kind == EVENT_JOB_MOVE ||
		    ( lastevent->jStatus != JOB_STAT_PEND
                      && lastevent->jStatus != JOB_STAT_SSUSP
                      && lastevent->kind != EVENT_JOB_START)) {
                    if (event) {
                        prtLine(";\n");
                    } else{
                        prtLine(".\n");
                    }
                }

            }
        End:        if ((bhistReq->options & OPT_CHRONICLE) == OPT_CHRONICLE)
                event = event->chronback;
            else
                event = event->next;
	}

        if (bhistReq->options & OPT_CHRONICLE) {
            FREEUP(currentOrDoneTime);
            return;
        }



        if (bhistReq->options & OPT_DFTFORMAT) {
	    if (first == TRUE) {
                printf((_i18n_msg_get(ls_catd,NL_SETN,3197, "Summary of time in seconds spent in various states:\n"))); /* catgets  3197  */
                printf((_i18n_msg_get(ls_catd,NL_SETN,3198, "JOBID   USER    JOB_NAME  PEND    PSUSP   RUN     USUSP "))); /* catgets  3198  */
                printf("  ");
                printf((_i18n_msg_get(ls_catd,NL_SETN,3199, "SSUSP   UNKWN   TOTAL\n"))); /* catgets  3199  */
                first = FALSE;
	    }
            if (bhistReq->options & OPT_WIDEFORMAT) {
                char *jobName, *pos;
                jobName = job->submit.jobName;
                if ((pos = strchr(jobName, '[')) && LSB_ARRAY_IDX(job->jobId)) {                    *pos = '\0';
                    sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId));
                }

	        printf("%-7s %-7s %-9s %-8.0f%-8.0f%-8.0f%-8.0f%-8.0f%-8.0f%-10.0f\n",
                       lsb_jobid2str(job->jobId), jobRecord->job->user,
		       jobName, NegtoZero(pendTime),
		       NegtoZero(pendSuspTime), NegtoZero(runTime),
		       NegtoZero(usrSuspTime), NegtoZero(sysSuspTime),
		       NegtoZero(unknownTime),
		       NegtoZero(pendTime) + NegtoZero(pendSuspTime) +
		       NegtoZero(runTime) + NegtoZero(usrSuspTime) +
		       NegtoZero(sysSuspTime) + NegtoZero(unknownTime) );
            }
            else {
                char *jobName, *pos;
                char osUserName[MAXLINELEN];
                jobName = job->submit.jobName;
                if ((pos = strchr(jobName, '[')) && LSB_ARRAY_IDX(job->jobId)) {
                    *pos = '\0';
                    sprintf(jobName, "%s[%d]", jobName, LSB_ARRAY_IDX(job->jobId)
                        );
                }
	        TRUNC_STR(jobName, 8);


                if (getOSUserName_(jobRecord->job->user, osUserName,
                                   MAXLINELEN) != 0) {
                    strncpy(osUserName, job->user, MAXLINELEN);
                    osUserName[MAXLINELEN - 1] = '\0';
                }

	        printf("%-7.7s %-7.7s %-9.9s %-8.0f%-8.0f%-8.0f%-8.0f%-8.0f%-8.0f%-10.0f\n",
                       lsb_jobid2str(job->jobId), osUserName,
		       jobName, NegtoZero(pendTime),
		       NegtoZero(pendSuspTime), NegtoZero(runTime),
		       NegtoZero(usrSuspTime), NegtoZero(sysSuspTime),
		       NegtoZero(unknownTime),
		       NegtoZero(pendTime) + NegtoZero(pendSuspTime) +
		       NegtoZero(runTime) + NegtoZero(usrSuspTime) +
		       NegtoZero(sysSuspTime) + NegtoZero(unknownTime) );
            }
        }
	else {
            printf("\n%s  ",(_i18n_msg_get(ls_catd,NL_SETN,3200, "Summary of time in seconds spent in various states by")));  /* catgets  3200  */
            printf("%-12.24s\n", currentOrDoneTime);
            FREEUP(currentOrDoneTime);
            printf("  %s    ",(_i18n_msg_get(ls_catd,NL_SETN,3201, "PEND     PSUSP    RUN      USUSP    SSUSP"))); /* catgets  3201  */
            printf((_i18n_msg_get(ls_catd,NL_SETN,3202, "UNKWN    TOTAL\n"))); /* catgets  3202  */
            printf("  %-9.0f%-9.0f%-9.0f%-9.0f%-9.0f%-9.0f%-12.0f\n",
                   NegtoZero(pendTime), NegtoZero(pendSuspTime),
                   NegtoZero(runTime), NegtoZero(usrSuspTime),
                   NegtoZero(sysSuspTime), NegtoZero(unknownTime),
                   NegtoZero(pendTime) + NegtoZero(pendSuspTime) +
                   NegtoZero(runTime) + NegtoZero(usrSuspTime) +
                   NegtoZero(sysSuspTime) + NegtoZero(unknownTime));
        }
        jobRecord = jobRecord->forw;
        first = FALSE;
        FREEUP(currentOrDoneTime);
    }

}

static char
logfile_check(struct bhistReq *Req)
{
    char workDir[MAXFILENAMELEN], eventFileName[MAXFILENAMELEN];
    int lineNum = 0, pos = 0;
    LS_STAT_T statBuf;
    struct eventRec *log;
    static char *envdir;
    static char *clusterName;
    char ch, *sp;
    struct eventLogFile eLogFile;
    struct eventLogHandle eLogHandle;

    if ((envdir = getenv("LSF_ENVDIR")) == NULL)
        envdir = "/etc";

    if (ls_readconfenv(bhistParams, envdir) < 0) {
        ls_perror("ls_readconfenv");
        exit(-1);
    }

    if (!(Req->options & OPT_ELOGFILE)) {
        if ((clusterName = ls_getclustername()) == NULL ) {
            ls_perror("ls_getclustername()");
            exit(-1);
        }

        sprintf(workDir, "\
%s/logdir", bhistParams[LSB_SHAREDIR].paramValue);

        readFromHeadFlag = 1;
        readEventFromHead(workDir, Req);
        readFromHeadFlag = 0;
    }

    if (Req->options & OPT_ELOGFILE) {

        if ((eLogHandle.fp = fopen (Req->eventFileName, "r")) == NULL) {
            perror("logfile_check");
            exit(-1);
        }
        sp = strstr(Req->eventFileName, "lsb.events");

        if (sp && !strcmp(sp, "lsb.events")) {

            if (fscanf(eLogHandle.fp, "%c%d ", &ch, &pos) != 2 || ch != '#') {
                pos = 0;
            } else {

	        lineNum = 1;
                countLineNum(eLogHandle.fp, pos, &lineNum);
            }
            fseek (eLogHandle.fp, pos, SEEK_SET);
        }
        strcpy(eLogHandle.openEventFile, Req->eventFileName);
        eLogHandle.curOpenFile = 0;
        eLogHandle.lastOpenFile = -1;
        eLogPtr = &eLogHandle;

    } else if (Req->options & OPT_NUMLOGFILE) {

        int maxEventFile;

        maxEventFile = 0;
        do {
            sprintf(eventFileName, "\
%s/logdir/lsb.events.%d",
                    bhistParams[LSB_SHAREDIR].paramValue,
                    ++maxEventFile);
        } while (stat(eventFileName, &statBuf) == 0);

	if (Req->numLogFile >= maxEventFile) {
            if (Req->numMinLogFile >= maxEventFile)
                Req->numMinLogFile = 0;
            eLogHandle.curOpenFile = maxEventFile - 1;
	} else
            eLogHandle.curOpenFile = Req->numLogFile - 1;
	eLogHandle.lastOpenFile = Req->numMinLogFile;


        if (eLogHandle.curOpenFile == 0)
            sprintf(eLogHandle.openEventFile, "%s/lsb.events", workDir);
        else
            sprintf(eLogHandle.openEventFile, "%s/lsb.events.%d", workDir,
                    eLogHandle.curOpenFile);

        if ((eLogHandle.fp = fopen(eLogHandle.openEventFile, "r")) == NULL) {
            perror(eLogHandle.openEventFile);
            exit(-1);
        }
        if (eLogHandle.curOpenFile == 0) {

            if (fscanf(eLogHandle.fp,  "%c%d ", &ch, &pos) != 2 || ch != '#') {
                pos = 0;
            } else {
	        lineNum = 1;
                countLineNum(eLogHandle.fp, pos, &lineNum);
            }
            fseek (eLogHandle.fp, pos, SEEK_SET);
        }
        eLogPtr = &eLogHandle;
    } else if (Req->searchTime[1] == -1)
    {

        eLogHandle.curOpenFile =  0;
        eLogHandle.lastOpenFile = 0;
        sprintf(eLogHandle.openEventFile, "%s/lsb.events", workDir);

        if ((eLogHandle.fp = fopen(eLogHandle.openEventFile, "r")) == NULL) {
            perror(eLogHandle.openEventFile);
            exit(-1);
        }

        if (fscanf(eLogHandle.fp,  "%c%d ", &ch, &pos) != 2 || ch != '#') {
            pos = 0;
        } else {
            lineNum = 1;
	    countLineNum(eLogHandle.fp, pos, &lineNum);
        }
        fseek (eLogHandle.fp, pos, SEEK_SET);

        eLogPtr = &eLogHandle;

    } else {


	char indexFileName[MAXFILENAMELEN];

	jobIdIndexSPtr = NULL;
	if (Req->options & OPT_JOBID) {
            Req->searchTime[1] = -1;
	    sprintf(indexFileName, "%s/%s", workDir, LSF_JOBIDINDEX_FILENAME);
	    if (!initJobIdIndexS(&jobIdIndexStr, indexFileName)) {

	        jobIdIndexSPtr = &jobIdIndexStr;
	    }
	}

	if (jobIdIndexSPtr == NULL) {

            strcpy(eLogFile.eventDir, workDir);
            eLogFile.beginTime = Req->searchTime[0];
            eLogFile.endTime   = Req->searchTime[1];
            if ((eLogPtr = lsb_openelog (&eLogFile, &lineNum)) == NULL) {
                perror("lsb_openelog");
                exit (-1);
            }
	} else {

            eLogHandle.curOpenFile =  100;
            eLogHandle.lastOpenFile = 0;
            sprintf(eLogHandle.openEventFile, "%s/lsb.events.%d", workDir,
                    eLogHandle.curOpenFile);
            eLogHandle.fp = NULL;
	    eLogPtr = &eLogHandle;

	}

    }


    if (Req->options & OPT_CHRONICLE) {
        while (TRUE) {

            if ((log = lsb_getelogrec (eLogPtr, &lineNum)) != NULL) {
                if ((Req->searchTime[1] == -1) ||
                    (log->eventTime >= Req->searchTime[0]
                     && log->eventTime <= Req->searchTime[1])) {

                    printChronicleEventLog(log, Req);
                }
                continue;
            }
            if (lsberrno == LSBE_EOF)
                break;

	    ls_syslog(LOG_ERR, I18N(3203,
                                    "File %s at line %d: %s\n"),   /* catgets 3203 */
                      eLogPtr->openEventFile,
                      lineNum,
                      lsb_sysmsg());
        }

        fclose(eLogPtr->fp);
        return 0;
    }


    while (TRUE) {

        if ((log = lsbGetNextJobEvent (eLogPtr,
                                       &lineNum, Req->numJobs, Req->jobIds, jobIdIndexSPtr)) != NULL) {

            if ((Req->searchTime[1] == -1) ||
		(log->eventTime >= Req->searchTime[0]
                 && log->eventTime <= Req->searchTime[1])) {

		parse_event(log, Req);
            }
	    continue;
        }
        if (lsberrno == LSBE_EOF)
	    break;
	ls_syslog(LOG_ERR, I18N(3203,
                                "File %s at line %d: %s\n"), 	/* catgets 3203 */
                  eLogPtr->openEventFile,
                  lineNum,
                  lsb_sysmsg());

    }

    fclose(eLogPtr->fp);
    return 0;

}

static void
readEventFromHead(char *eventDir, struct bhistReq *reqPtr)
{
    char eventFile[MAXFILENAMELEN];
    LS_STAT_T statBuf;
    struct eventRec *log;
    char ch;
    int lineNum = 0, pos, ret;
    FILE *log_fp;

    sprintf(eventFile, "%s/lsb.events", eventDir);


    if (stat(eventFile, &statBuf) < 0) {
        perror(eventFile);
        exit(-1);
    }

    if ((statBuf.st_mode & S_IFREG) != S_IFREG ) {
        fprintf(stderr,"%s: %s\n",eventFile,
		(_i18n_msg_get(ls_catd,NL_SETN,3204, "Not a regular file\n"))); /* catgets  3204  */
        exit(-1);
    }

    if ((log_fp = fopen(eventFile, "r")) == NULL) {
        perror(eventFile);
        exit(-1);
    }

    if (fscanf(log_fp, "%c%d ", &ch, &pos) != 2 || ch != '#') {

        return;
    }

    rewind(log_fp);

    while (((ret=ftell(log_fp)) < pos) && (ret != -1)) {
        if ((log = lsb_geteventrec(log_fp, &lineNum)) != NULL) {

            parse_event(log, reqPtr);


        }else {

            if (lsberrno ==  LSBE_EOF || lsberrno == LSBE_NO_MEM) {
                break;
            }
        }
    }
    fclose(log_fp);
    return;
}


static void
initLoadIndexNames(void)
{
    loadIndex = calloc(1, sizeof(struct loadIndexLog));

    loadIndex->nIdx = 11;
    loadIndex->name = calloc(loadIndex->nIdx, sizeof(char *));
    loadIndex->name[0] = putstr_("r15s");
    loadIndex->name[1] = putstr_("r1m");
    loadIndex->name[2] = putstr_("r15m");
    loadIndex->name[3] = putstr_("ut");
    loadIndex->name[4] = putstr_("pg");
    loadIndex->name[5] = putstr_("io");
    loadIndex->name[6] = putstr_("ls");
    loadIndex->name[7] = putstr_("it");
    loadIndex->name[8] = putstr_("swp");
    loadIndex->name[9] = putstr_("mem");
    loadIndex->name[10] = putstr_("tmp");
}


void prtModifiedJob(struct jobModLog *jobModLog, struct bhistReq *bhistReq,
		    char *timestamp)
{
    char prline[MSGSIZE];
    int  i;
    char *sp;
#define PRT_FMTSTR(i18nstr)                     \
    {                                           \
        prtLine("\n\t\t\t ");                   \
        prtLine(i18nstr);                       \
    }

    sprintf(prline,
	    I18N(3357, "%-12.19s: Parameters of Job are changed:") /* catgets 3357 */,
	    timestamp);
    prtLine(prline);

    if (jobModLog->options & SUB_QUEUE) {
        sprintf(prline,
                I18N(3358, "Job queue changes to : %s") /* catgets 3358 */,
                jobModLog->queue);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_HOST ) {
	sprintf(prline, I18N(3359, "Hosts change to : " /* catgets 3359 */));
        for (i = 0; i < jobModLog->numAskedHosts; i++) {
            strcat(prline, jobModLog->askedHosts[i]);
        }
	if ( jobModLog->numAskedHosts )
	    PRT_FMTSTR(prline);
    }


    if (jobModLog->options2 & SUB2_IN_FILE_SPOOL) {
        sprintf(prline, I18N(3348, "Input file (Spooled) change to : %s"), /* catgets 3348 */
                jobModLog->inFile);
        PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_IN_FILE) {
        sprintf(prline, I18N(3360, "Input file change to : %s") /* catgets 3360 */, jobModLog->inFile);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_OUT_FILE) {
        sprintf(prline, I18N(3361, "Output file change to : %s") /* catgets 3361 */ , jobModLog->outFile);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_ERR_FILE) {
        sprintf(prline, I18N(3362, "Error file change to : %s") /* catgets 3362 */,  jobModLog->errFile);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_EXCLUSIVE) {
        sprintf(prline, I18N(3363, "Job changes to exclusive")); /* catgets 3363 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_NOTIFY_END) {
        sprintf(prline, I18N(3364, "Job will send mail to user when job ends")); /* catgets 3364 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_NOTIFY_BEGIN) {
        sprintf(prline, I18N(3365, "Job will send mail to user when job begins")); /* catgets 3365 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_CHKPNT_PERIOD) {
	sprintf(prline, "%s: %d",
                I18N(3367, "Checkpoint period changes to") /* catgets 3367 */ ,
                jobModLog->chkpntPeriod);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_CHKPNT_DIR) {
        sprintf(prline, "%s: %s",
                I18N(3368, "Chkpnt directory changes to") /* catgets 3368 */ ,
                jobModLog->chkpntDir);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_RERUNNABLE) {
        sprintf(prline, I18N(3369, "Job changes to rerunnable")); /* catgets 3369 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_HOST_SPEC) {
	sprintf(prline, "%s: %s",
                I18N(3370, "Host specification change to"), /* catgets 3370 */
                jobModLog->hostSpec);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_DEPEND_COND) {
        sprintf(prline, "%s: %s",
                I18N(3371, "Dependances condition changes to"), /* catgets 3371 */
                jobModLog->dependCond);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_RES_REQ ) {
	sprintf(prline, "%s: %s",
                I18N(3372, "Resource requirement changes to") /* catgets 3372 */ ,
                jobModLog->resReq);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_OTHER_FILES) {
	sprintf(prline, "%s: %d",
                I18N(3373, "Number of Copied files change to") /* catgets 3373 */  ,
                jobModLog->nxf);
	PRT_FMTSTR(prline);
        for (i = 0; i < jobModLog->nxf; i++) {
            sprintf(prline, I18N(3374, "%3d : subfile name is %s") /* catgets 3374 */ , i, jobModLog->xf[i].subFn);
	    PRT_FMTSTR(prline);
            sprintf(prline, I18N(3375, "      execfile name is %s") /* catgets 3375 */ , jobModLog->xf[i].execFn);
	    PRT_FMTSTR(prline);
	    sprintf(prline, I18N(3376, "      file options %d") /* catgets 3376 */ , jobModLog->xf[i].options);
	    PRT_FMTSTR(prline);
        }
    }

    if (jobModLog->options & SUB_PRE_EXEC) {
        sprintf(prline, I18N(3377, "Pre exec command changes to : %s") /* catgets 3377 */ , jobModLog->preExecCmd);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_POST_EXEC) {
        sprintf(prline, I18N(3377, "Post exec command changes to : %s") /* catgets 3377 */ , jobModLog->postExecCmd);
        PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_LOGIN_SHELL) {
        sprintf(prline, I18N(3378, "Log shell changes to : %s") /* catgets 3378 */, jobModLog->loginShell);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_MAIL_USER) {
	sprintf(prline, I18N(3379, "Mail user changes to : %s") /* catgets 3379 */, jobModLog->mailUser);
	PRT_FMTSTR(prline);
    }

    if (jobModLog->options & SUB_PROJECT_NAME) {
        sprintf(prline, I18N(3380, "Project name changes to : %s") /* catgets 3380 */, jobModLog->projectName);
	PRT_FMTSTR(prline);
    }

    for(i = 0; i < LSF_RLIM_NLIMITS; i++)
	if ( jobModLog->rLimits[i] > 0 ) {
	    switch(i) {
                case LSF_RLIMIT_CPU :
                    sprintf(prline, I18N(3381, "CPU limit changes to : %d (minutes)") /* catgets 3381 */,
                            (jobModLog->rLimits[LSF_RLIMIT_CPU])/60);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_FSIZE :
                    sprintf(prline, I18N(3382, "File limit changes to : %d (KB)") /* catgets 3382 */ ,
                            jobModLog->rLimits[LSF_RLIMIT_FSIZE]);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_DATA  :
                    sprintf(prline, I18N(3383, "Data limit changes to : %d (KB)") /* catgets 3383 */ ,
                            jobModLog->rLimits[LSF_RLIMIT_DATA]);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_STACK :
                    sprintf(prline, I18N(3384, "Stack limit changes to : %d (KB)") /* catgets 3384 */,
                            jobModLog->rLimits[LSF_RLIMIT_STACK]);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_CORE  :
                    sprintf(prline, I18N(3385, "Core limit changes to : %d (KB)")  /* catgets 3385 */ ,
                            jobModLog->rLimits[LSF_RLIMIT_CORE]);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_RSS   :
                    sprintf(prline, I18N(3386, "Memory limit changes to : %d (KB)") /* catgets 3386 */ ,
                            jobModLog->rLimits[LSF_RLIMIT_RSS]);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_SWAP   :
                    sprintf(prline, I18N(3387, "Swap limit changes to : %d (KB)") /* catgets 3387 */,
                            jobModLog->rLimits[LSF_RLIMIT_SWAP]);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_RUN   :
                    sprintf(prline, I18N(3433, "run limit changes to : %d (minutes)") /* catgets 3433 */,
                            jobModLog->rLimits[LSF_RLIMIT_RUN]/60);
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_PROCESS   :
                    sprintf(prline, I18N(3388, "Process limit changes to : %d") /* catgets 3388 */,
                            jobModLog->rLimits[LSF_RLIMIT_PROCESS]);
                    PRT_FMTSTR(prline);
                    break;


	    }
	}

    if (jobModLog->options2 & SUB2_JOB_CMD_SPOOL) {
        sprintf(prline, I18N(3151, "Job command (Spooled) changes to : %s"), /* catgets 3151 */
                jobModLog->command);
        PRT_FMTSTR(prline);
    }

    if (jobModLog->options2 & SUB2_MODIFY_CMD) {
        sprintf(prline, I18N(3390, "Job command changes to : %s") /* catgets 3390 */ , jobModLog->command);
	PRT_FMTSTR(prline);
    }


    if ( jobModLog->numProcessors != DEFAULT_NUMPRO &&
	 jobModLog->numProcessors != DEL_NUMPRO) {
        sprintf(prline, I18N(3391, "New minimal num of processors is : %d") /* catgets 3391 */, jobModLog->numProcessors);
	PRT_FMTSTR(prline);
    }

    if ( jobModLog->maxNumProcessors != DEFAULT_NUMPRO &&
	 jobModLog->maxNumProcessors != DEL_NUMPRO ) {
        sprintf(prline, I18N(3392, "New max num of processors is : %d") /* catgets 3392 */ , jobModLog->maxNumProcessors);
	PRT_FMTSTR(prline);
    }

    if ( jobModLog->beginTime > 0 ) {
	sp = putstr_(_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T_Y,
                                 (time_t *)&jobModLog->beginTime));
        sprintf(prline, I18N(3393, "Job new begin time is : %s") /* catgets 3393 */ , sp);
	PRT_FMTSTR(prline);
	free(sp);
    }

    if ( jobModLog->termTime > 0 ) {
	sp = putstr_(_i18n_ctime(ls_catd, CTIME_FORMAT_a_b_d_T_Y, (time_t *)&jobModLog->termTime));
        sprintf(prline, I18N(3394, "Job new end time is : %s") /* catgets 3394 */ , sp);
	PRT_FMTSTR(prline);
	free(sp);
    }

    if (jobModLog->options2 & SUB2_JOB_PRIORITY) {
        sprintf(prline, I18N(3174, "New job priority is: %d") /* catgets 3174 */ , jobModLog->userPriority);
        PRT_FMTSTR(prline);
    }

    if (jobModLog->options2 & SUB2_JOB_DESC && jobModLog->jobDesc) {
        sprintf(prline, "Job Description changes to : %s", jobModLog->jobDesc);
        PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_QUEUE) {
        sprintf(prline, I18N(3395, "Job queue changes back to default") /* catgets 3395 */ );
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_HOST ) {
	sprintf(prline, I18N(3396, "Host changes to default: ")); /* catgets 3396 */
        PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions2 & SUB2_JOB_CMD_SPOOL) {
        sprintf(prline, I18N(3153, "Job command spooling is canceled")); /* catgets 3153 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions2 & SUB2_IN_FILE_SPOOL) {
        sprintf(prline, I18N(3154, "Input file spooling is canceled")); /* catgets 3154 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_IN_FILE) {
        sprintf(prline, I18N(3397, "Input file is disabled")); /* catgets 3397 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_OUT_FILE) {
        sprintf(prline, I18N(3398, "Output file is disabled")); /* catgets 3398 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_ERR_FILE) {
        sprintf(prline, I18N(3399, "Error file is disabled")); /* catgets 3399 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_EXCLUSIVE) {
        sprintf(prline, I18N(3400, "Job changes to nonexclusive"));
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_NOTIFY_END) {
        sprintf(prline, I18N(3401, "No mail will be sent to user when job ends")); /* catgets 3401 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_NOTIFY_BEGIN) {
        sprintf(prline, I18N(3402, "No mail will be to user when job begins")); /* catgets 3402 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_CHKPNTABLE) {
        sprintf(prline, I18N(3404, "Job changes to be NOT checkpointable")); /* catgets 3404 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_RERUNNABLE) {
        sprintf(prline, I18N(3405, "Job changes to be not rerunnable")); /* catgets 3405 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_HOST_SPEC) {
	sprintf(prline, I18N(3406, "No host specification")); /* catgets 3406 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_DEPEND_COND) {
        sprintf(prline, I18N(3407, "No dependances condition")); /* catgets 3407 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_RES_REQ ) {
	sprintf(prline, I18N(3408, "No  resource requirement")); /* catgets 3408 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_OTHER_FILES) {
	sprintf(prline, I18N(3409, "No transfer files specified")); /* catgets 3409 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_PRE_EXEC) {
        sprintf(prline, I18N(3410, "Pre exec command is disabled")); /* catgets 3410 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_POST_EXEC) {
        sprintf(prline, I18N(3410, "Post exec command is disabled")); /* catgets 3410 */
        PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_LOGIN_SHELL) {
        sprintf(prline, I18N(3411, "Log shell changes to default")); /* catgets 3411 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_MAIL_USER) {
	sprintf(prline, I18N(3412, "Mail user changes to original user")); /* catgets 3412 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions & SUB_PROJECT_NAME) {
        sprintf(prline, I18N(3413, "Project name changes to default")); /* catgets 3413 */
	PRT_FMTSTR(prline);
    }

    for(i = 0; i < LSF_RLIM_NLIMITS; i++)
	if ( jobModLog->rLimits[i] == DELETE_NUMBER ) {
	    switch(i) {
                case LSF_RLIMIT_CPU :
                    sprintf(prline, I18N(3414, "bmod -cn used. CPU limit set to queue CPU limit")); /* catgets 3414 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_FSIZE :
                    sprintf(prline, I18N(3415, "bmod -Fn used. File limit set to queue file limit")); /* catgets 3415 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_DATA  :
                    sprintf(prline, I18N(3416, "bmod -Dn used. Data limit set to queue data limit")); /* catgets 3416 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_STACK :
                    sprintf(prline, I18N(3417, "bmod -Sn used. Stack limit set to queue stack limit")); /* catgets 3417 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_CORE  :
                    sprintf(prline, I18N(3418, "bmod -Cn used. Core limit set to queue core limit")); /* catgets 3418 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_RSS   :
                    sprintf(prline, I18N(3419, "bmod -Mn used. Memory limit set to queue memory limit")); /* catgets 3419 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_SWAP   :
                    sprintf(prline, I18N(3420, "Swap limit is removed")); /* catgets 3420 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_PROCESS   :
                    sprintf(prline, I18N(3421, "Process limit is removed")); /* catgets 3421 */
                    PRT_FMTSTR(prline);
                    break;
                case LSF_RLIMIT_RUN   :
                    sprintf(prline, I18N(3440, "bmod -Wn used. Run limit set to queue run limit")); /* catgets 3440 */
                    PRT_FMTSTR(prline);
                    break;

	    }
	}

    if ( jobModLog->numProcessors == DEL_NUMPRO  ) {
        sprintf(prline, I18N(3423, "Minimal num of processors is removed")); /* catgets 3423 */
	PRT_FMTSTR(prline);
    }

    if ( jobModLog->maxNumProcessors == DEL_NUMPRO ) {
        sprintf(prline, I18N(3424, "Max num of processors is removed")); /* catgets 3424 */
	PRT_FMTSTR(prline);
    }

    if ( jobModLog->beginTime == DELETE_NUMBER) {
        sprintf(prline, I18N(3425, "Job begin time is removed")); /* catgets 3425 */
	PRT_FMTSTR(prline);
    }

    if ( jobModLog->termTime == DELETE_NUMBER ) {
        sprintf(prline, I18N(3426, "Job terminate time is removed")); /* catgets 3426 */
	PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions2 & SUB2_JOB_PRIORITY) {
        sprintf(prline, I18N(3175, "Job priority changes to default")); /* catgets 3175 */
        PRT_FMTSTR(prline);
    }

    if (jobModLog->delOptions2 & SUB2_JOB_DESC) {
        sprintf(prline, "Job Description is removed");
        PRT_FMTSTR(prline);
    }
}



static void
prtParameters(struct jobInfoEnt *params, struct bhistReq *bhistReq, char *timestamp)
{
    char prline[MSGSIZE], tBuff[80];

    if (params->submit.options & SUB_MODIFY_ONCE)
        sprintf(tBuff, "%s: ",
		(_i18n_msg_get(ls_catd,NL_SETN,3210, "Parameters are modified (only used once) to"))); /* catgets  3210  */
    else
        sprintf(tBuff, "%s:  ",
		(_i18n_msg_get(ls_catd,NL_SETN,3211, "Parameters are modified to"))); /* catgets  3211  */

    if (bhistReq->options & OPT_CHRONICLE)
        sprintf(prline,"%-12.19s: %s <%s> %s",
		timestamp,
		I18N_Job,
	    	lsb_jobidinstr(params->jobId),
		lowFirstChar(tBuff));
    else
        sprintf(prline, "%-12.19s: %s", timestamp, tBuff);

    prtLine(prline);
    sprintf(prline, " %s <%s>, ",
            (_i18n_msg_get(ls_catd,NL_SETN,3213, "Project")), params->submit.projectName); /* catgets  3213  */

    prtLine(prline);


    if ((params->submit.options2 & SUB2_JOB_CMD_SPOOL) &&
        (params->submit.command)) {
        sprintf(prline, " %s <%s>, ",
                (_i18n_msg_get(ls_catd,NL_SETN,3152, "Command (Spooled)")), /* catgets 3152 */
                params->submit.command);
        prtLine(prline);
    }

    if ((params->submit.options2 & SUB2_MODIFY_CMD) &&
	(params->submit.command)) {
        sprintf(prline, " %s <%s>, ",
		(_i18n_msg_get(ls_catd,NL_SETN,3214, "Command")), params->submit.command); /* catgets  3214  */
        prtLine(prline);
    }


    if (params->submit.options & SUB_MAIL_USER) {
        sprintf(prline, " %s <%s>, ",
		(_i18n_msg_get(ls_catd,NL_SETN,3217, "Mail")), params->submit.mailUser); /* catgets  3217  */
        prtLine(prline);
    }
    sprintf(prline, "%s <%s>", (_i18n_msg_get(ls_catd,NL_SETN,3218, "Queue")), /* catgets  3218  */
            params->submit.queue);
    prtLine(prline);
    if (params->submit.options & SUB_JOB_NAME) {
        sprintf(prline, ", %s <%s>",
		(_i18n_msg_get(ls_catd,NL_SETN,3219, "Job Name")), /* catgets  3219  */
                params->submit.jobName);
        prtLine(prline);
    }
    prtBTTime(params);
    if (bhistReq->options & OPT_LONGFORMAT) {
        prtFileNames(params, FALSE);
        prtSubDetails(params,
                      params->submit.hostSpec,
                      params->cpuFactor);
    } else
        prtLine(";\n");
}


static char *
getUserName(int userId)
{
    static char lsfUserName[MAXLINELEN];

    if (getLSFUserByUid_(userId, lsfUserName, MAXLINELEN) == 0)
        return (lsfUserName);
    else
        return ((_i18n_msg_get(ls_catd,NL_SETN,3220, "unknown"))); /* catgets  3220  */

}


static int
dispChkpnt (struct eventRecord *event, struct jobRecord *jobRecord)
{

    struct eventRecord *eventP;
    struct jobInfoEnt  *newParams = NULL;


    for (eventP = jobRecord->eventhead; eventP != event;
         eventP = eventP->next) {
	if (eventP->kind == EVENT_JOB_MODIFY)
	    newParams = eventP->newParams;
    }
    if (newParams != NULL) {
	if ((newParams->submit.options & SUB_RERUNNABLE) &&
	    !(newParams->submit.options & SUB_CHKPNTABLE))
            return (FALSE);
    } else {
	if ((jobRecord->job->submit.options & SUB_RERUNNABLE) &&
            !(jobRecord->job->submit.options & SUB_CHKPNTABLE))
            return (FALSE);

    }
    return (TRUE);
}




char *
actionName (char *sigName)
{
    static char *actionString = NULL;
    char *action;

    if (actionString != NULL) {
	free(actionString);
    }

    if ((strcmp(sigName, "CHKPNT") == 0)
        || (strcmp(sigName, "CHKPNT_COPY") == 0))
    {
	action = _i18n_msg_get(ls_catd,NL_SETN,3221,"checkpoint action"); /* catgets 3221 */
    }
    else if (strcmp(sigName, "SIG_SUSP_USER") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3222, "User suspending action"); /* catgets  3222  */
    else if (strcmp(sigName, "SIG_SUSP_LOAD") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3223, "Load suspending action"); /* catgets  3223  */
    else if (strcmp(sigName, "SIG_SUSP_WINDOW") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3225, "Window suspending action"); /* catgets  3225  */
    else if (strcmp(sigName, "SIG_SUSP_OTHER") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3226, "Other suspending action"); /* catgets  3226  */
    else if (strcmp(sigName, "SIG_RESUME_USER") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3227, "User resumption action"); /* catgets  3227  */
    else if (strcmp(sigName, "SIG_RESUME_LOAD") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3228, "Load resumption action");  /* catgets  3228  */
    else if (strcmp(sigName, "SIG_RESUME_WINDOW") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3230, "Window resumption action"); /* catgets  3230  */
    else if (strcmp(sigName, "SIG_RESUME_OTHER") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3231, "Other resumption action"); /* catgets  3231  */
    else if (strcmp(sigName, "SIG_TERM_USER") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3232, "User termination action"); /* catgets  3232  */
    else if (strcmp(sigName, "SIG_TERM_LOAD") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3233, "Load termination action"); /* catgets  3233  */
    else if (strcmp(sigName, "SIG_TERM_WINDOW") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3235, "Window termination action"); /* catgets  3235  */
    else if (strcmp(sigName, "SIG_TERM_OTHER") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3236, "Other termination action"); /* catgets  3236  */
    else if (strcmp(sigName, "SIG_TERM_RUNLIMIT") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3237, "RUNLIMIT termination action"); /* catgets  3237  */
    else if (strcmp(sigName, "SIG_TERM_DEADLINE") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3238, "DEADLINE termination action"); /* catgets  3238  */
    else if (strcmp(sigName, "SIG_TERM_PROCESSLIMIT") == 0)
        action = _i18n_msg_get(ls_catd,NL_SETN,3239, "PROCESSLIMIT termination action"); /* catgets  3239  */
    else if (strcmp(sigName, "SIG_TERM_CPULIMIT") == 0)
	action = I18N (3430, "CPULIMIT termination action"); /* catgets 3430 */
    else if (strcmp(sigName, "SIG_TERM_FORCE") == 0)
	action = I18N (3431, "Kill & Remove action"); /* catgets 3431 */
    else if (strcmp(sigName, "SIG_KILL_REQUEUE") == 0)
	action = I18N (3432, "Kill & Requeue action"); /* catgets 3432 */
    else if (strcmp(sigName, "SIG_TERM_MEMLIMIT") == 0)
	action = I18N (3433, "MEMLIMIT termination action"); /* catgets 3433 */
    else
        action = _i18n_msg_get(ls_catd,NL_SETN,3240, "Unknow action"); /* catgets  3240  */

    actionString = putstr_(action);
    if (actionString == NULL) {
	perror("malloc");
	exit (-1);
    } else
	return (actionString);

}

static void
printEvent(struct bhistReq *bhistReq, struct jobRecord *jobRecord,
           struct jobInfoEnt *job, struct eventRecord *event, time_t timeStamp,
           char *timeStampStr, int option)
{
    static char   fname[] = "printEvent()";
    int           i;
    char          tBuff[20];
    char          *disptime = NULL;
    char          prline[MSGSIZE];
    char          *hostPtr;
    char          *cp;
    char          *cp1;
    int           display;
    int           lastChkPeriod;

    if ((bhistReq->options & OPT_DFTFORMAT) == OPT_DFTFORMAT)
	return;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG2, "%s: (2)event.kind=%x",
                  fname, event->kind);

    if ((bhistReq->options & OPT_CHRONICLE))  {
        if (event->idx > 0)
            sprintf (tBuff, " %s <%d[%d]>",
                     I18N_Job,
                     (int)job->jobId, event->idx);
        else
            sprintf (tBuff, " %s <%d>",
                     I18N_Job,
                     (int)job->jobId);
    }
    else if (event->idx > 0 && (bhistReq->options & OPT_ARRAY_INFO))
        sprintf (tBuff, " [%d]", event->idx);
    else
        tBuff[0] = '\0';

    lastChkPeriod = 0;
    switch (event->kind) {
        case EVENT_JOB_NEW:
            if ((bhistReq->options & OPT_CHRONICLE) != OPT_CHRONICLE)
                break;

            prtJobSubmit(job, TRUE, TRUE);
            prtFileNames(job, TRUE);
            prtHeader(job, FALSE, TRUE);
            hostPtr = job->submit.hostSpec;
            if (strcmp (job->submit.hostSpec, "") == 0)
            {
                if (job->numExHosts > 0)
                    hostPtr = job->exHosts[0];
                else
                    hostPtr = job->fromHost;
            }
            prtSubDetails(job, hostPtr, jobRecord->hostFactor);
            break;
        case EVENT_PRE_EXEC_START:
        case EVENT_JOB_START:
            lastChkPeriod = job->submit.chkpntPeriod;
            if (timeStamp == 0 )
                disptime = ctime((time_t *)&(job->startTime)) ;
            else if (timeStamp > 0)


                job->startTime = timeStamp;


            if (job->exHosts && job->numExHosts > 0) {
                for (i=0; i<job->numExHosts; i++)
                    FREEUP (job->exHosts[i]);
                FREEUP (job->exHosts);
            }
            job->numExHosts = event->numExHosts;
            job->exHosts = calloc(job->numExHosts,
                                  sizeof(char *));
            for (i = 0; i < job->numExHosts; i++) {
                job->exHosts[i] = malloc(MAXHOSTNAMELEN);
                sprintf(job->exHosts[i], "%s", event->exHosts[i]);
            }

            strcpy (job->execHome, "");
            strcpy (job->execCwd, "");
            strcpy (job->execUsername, "");
            if ((bhistReq->options & OPT_ARRAY_INFO) ||
                (bhistReq->options & OPT_CHRONICLE))
                job->jobId = LSB_JOBID(job->jobId, event->idx);
            else
                job->jobId = LSB_ARRAY_JOBID(job->jobId);
            if (event->kind == EVENT_PRE_EXEC_START)
            {
                if (bhistReq->options & OPT_CHRONICLE)
                    prtJobStart(job, BHIST_PRINT_PRE_EXEC, event->jobPid, TRUE);
                else
                    prtJobStart(job, BHIST_PRINT_PRE_EXEC, event->jobPid, FALSE);
            }
            else {
                if (bhistReq->options & OPT_CHRONICLE)
                    prtJobStart(job, BHIST_PRINT_JOB_CMD, event->jobPid, TRUE);
                else
                    prtJobStart(job, BHIST_PRINT_JOB_CMD, event->jobPid, FALSE);
            }
            job->jobId = LSB_ARRAY_JOBID(job->jobId);
            break;

        case EVENT_JOB_SWITCH:
            if (tBuff[0] == '\0')
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3248, "%-12.19s: Switched to Queue <%s> by user or administrator <%s>")), /* catgets  3248  */
                        timeStampStr,
                        event->queue,
                        event->userName);
            else
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3249, "%-12.19s:%s switched to Queue <%s> by user or administrator <%s>")), /* catgets  3249  */
                        timeStampStr,
                        tBuff,
                        event->queue,
                        event->userName);
            prtLine(prline);
            break;

        case EVENT_JOB_MSG:
            sprintf(prline,
                    (_i18n_msg_get(ls_catd,NL_SETN,3250, "%-12.19s:%s Message <%s>  Message ID<%d> requested by user or administrator <%s>")),  /* catgets  3250  */
                    timeStampStr, tBuff,
                    event->jmMsg,
                    event->jmMsgId,
                    getUserName(event->usrId));
            prtLine(prline);
            break;

        case EVENT_JOB_MSG_ACK:
            sprintf(prline,
                    (_i18n_msg_get(ls_catd,NL_SETN,3251, "%-12.19s:%s Message <%s> Message ID<%d>has been dispatchd")),  /* catgets  3251  */
                    timeStampStr,
                    tBuff,
                    event->jmMsg,
                    event->jmMsgId);
            prtLine(prline);
            break;

        case EVENT_JOB_START_ACCEPT:
            if (tBuff[0] == '\0')
                sprintf(prline,"%-12.19s: %s (Pid %d)", timeStampStr,
                        (_i18n_msg_get(ls_catd,NL_SETN,3252, "Starting")), /* catgets  3252  */
                        event->jobPid);
            else
                sprintf(prline,"%-12.19s:%s %s (Pid %d)",
                        timeStampStr,
                        tBuff,
                        (_i18n_msg_get(ls_catd,NL_SETN,3253, "starting")), /* catgets  3253  */
                        event->jobPid);
            prtLine(prline);
            break;

        case EVENT_JOB_SIGNAL:
            if (strcmp (event->sigSymbol, "DELETEJOB") == 0)
            {
                if (tBuff[0] == '\0')
                    sprintf(prline, "%-12.19s: %s <%s>",
                            timeStampStr,
                            (_i18n_msg_get(ls_catd,NL_SETN,3254, "Delete requested by user or administrator")), /* catgets  3254 */
                            event->userName);
                else
                    sprintf(prline, "%-12.19s:%s %s<%s>",
                            timeStampStr,
                            tBuff,
                            (_i18n_msg_get(ls_catd,NL_SETN,3255, "delete requested by user or administrator")),  /* catgets  3255  */
                            event->userName);
                if (event->runCount > 0){
                    char i18nBuf[150];
                    sprintf(i18nBuf,"%s",prline);
                    sprintf(prline, "%s; %s %d",
                            i18nBuf,
                            (_i18n_msg_get(ls_catd,NL_SETN,3256, "Running times is")), /* catgets  3256  */
                            event->runCount);
                }
            } else
                sprintf(prline,
                        _i18n_msg_get(ls_catd,NL_SETN,3257, "%-12.19s:%s Signal <%s> requested by user or administrator <%s>"),  /* catgets  3257  */
                        timeStampStr,
                        tBuff, event->sigSymbol, event->userName);

            prtLine(prline);
            break;

        case EVENT_MIG:
            if (event->userName && event->userName[0])
                sprintf(prline, "%-12.19s:%s %s <%s>",
                        timeStampStr,
                        tBuff,
                        _i18n_msg_get(ls_catd,NL_SETN,3258, "Migration requested by user or administrator"), /* catgets  3258  */
                        event->userName);
            else
                sprintf(prline, "%-12.19s:%s %s",
                        timeStampStr,
                        tBuff,
                        _i18n_msg_get(ls_catd,NL_SETN,3259, "Migration requested by unknown user"));  /* catgets  3259  */

            if (event->migNumAskedHosts)
            {
                sprintf(prline, "%s; %s", prline, (_i18n_msg_get(ls_catd,NL_SETN,3260, "Specified Hosts"))); /* catgets  3260  */
                for (i = 0; i < event->migNumAskedHosts; i++) {
                    sprintf(prline, "%s <%s>", prline, event->migAskedHosts[i]);
                    if (i != event->migNumAskedHosts - 1)
                        strcat(prline, ",");
                }
            }
            prtLine(prline);
            break;

        case EVENT_JOB_SIGACT:
        {
            static char oldSigSymbol[128];

            if ((strcmp(event->sigSymbol, "SIG_CHKPNT") == 0)
                || (strcmp(event->sigSymbol, "SIG_CHKPNT_COPY") == 0))
	    {


	        display = dispChkpnt (event, jobRecord);


	        if (IS_PEND(event->jStatus))
		{

		    if (event->chkPeriod)
		    {
		        sprintf(prline,
                                (_i18n_msg_get(ls_catd,NL_SETN,3261, "%-12.19s:%s Checkpoint period is set to %d min.")),  /* catgets  3261  */
                                timeStampStr,
                                tBuff,
                                (int) (event->chkPeriod / 60));
			prtLine(prline);
		    }
		    break;
		}


		if (event->actStatus == ACT_START)
		{
		    if (display == TRUE)
		    {
			char *i18nBuf;
                        i18nBuf = putstr_((_i18n_msg_get(ls_catd,NL_SETN,3262, "initiated"))); /* catgets  3262  */
		        sprintf(prline,"%-12.19s:%s %s %s (actpid %d)",
                                timeStampStr,
                                tBuff,
                                (event->actFlags & LSB_CHKPNT_MIG) ?
                                (_i18n_msg_get(ls_catd,NL_SETN,3263, "Migration checkpoint")) :  /* catgets  3263  */
                                (_i18n_msg_get(ls_catd,NL_SETN,3264, "Checkpoint")), /* catgets  3264  */
                                i18nBuf,
                                event->actPid);
			if (event->chkPeriod != lastChkPeriod)
			{
			    lastChkPeriod = event->chkPeriod;
			    if (event->chkPeriod)
			        sprintf(prline,
                                        (_i18n_msg_get(ls_catd,NL_SETN,3265, "%s; checkpoint period is %d min.")), /* catgets  3265  */
                                        prline,
                                        (int) (event->chkPeriod / 60));
                        }
			free ( i18nBuf );
		    } else
			sprintf(prline, "%-12.19s:%s %s",
                                timeStampStr,
                                tBuff,
                                _i18n_msg_get(ls_catd,NL_SETN,3266, "Job is being requeued")  /* catgets  3266  */ );
		} else if (event->actStatus == ACT_DONE ||
                           event->actStatus == ACT_FAIL)
		{
		    if (display == TRUE)
		    {
			char *statusPtr;
			statusPtr = putstr_( (event->actStatus == ACT_DONE) ?
                                             (_i18n_msg_get(ls_catd,NL_SETN,3270, "succeeded")) : /* catgets  3270  */
                                             (_i18n_msg_get(ls_catd,NL_SETN,3271, "failed"))); /* catgets  3271  */

		        sprintf(prline,
                                "%-12.19s:%s %s %s (actpid %d)",
                                timeStampStr,
                                tBuff,
                                (event->actFlags & LSB_CHKPNT_MIG) ?
			    	(_i18n_msg_get(ls_catd,NL_SETN,3268, "Migration checkpoint")) :  /* catgets  3268  */
				(_i18n_msg_get(ls_catd,NL_SETN,3269, "Checkpoint")), /* catgets  3269  */
                                statusPtr,
                                event->actPid);
			free ( statusPtr );
                    } else
			sprintf(prline,"%-12.19s:%s %s",
                                timeStampStr,
                                tBuff,
                                _i18n_msg_get(ls_catd,NL_SETN,3272, "Job has been requeued")  /* catgets  3272  */ );
		}
		prtLine(prline);
            } else
	    {

                switch(event->actStatus)
		{
                    case ACT_START:
                        sprintf(prline,"%-12.19s:%s %s %s (actpid %d)",
                                timeStampStr,
                                tBuff,
                                actionName(event->sigSymbol),
                                (_i18n_msg_get(ls_catd,NL_SETN,3262,"initiated")),
                                event->actPid);
                        break;
                    case ACT_DONE:
                    case ACT_FAIL:
                        sprintf(prline, "%-12.19s:%s %s %s (actpid %d)",
                                timeStampStr,
                                tBuff,
                                actionName(event->sigSymbol),
                                (event->actStatus == ACT_DONE) ?
                        	(_i18n_msg_get(ls_catd,NL_SETN,3275, "completed")) :  /* catgets  3275  */
				(_i18n_msg_get(ls_catd,NL_SETN,3276, "exited with non-zero value")), /* catgets  3276  */
                                event->actPid);
                        break;
                }
                prtLine(prline);
            }
            strcpy(oldSigSymbol, event->sigSymbol);
	    break;
        }

        case EVENT_JOB_MOVE:
            if (tBuff[0] == '\0')
                sprintf (prline,
                         (_i18n_msg_get(ls_catd,NL_SETN,3427, "%-12.19s: Job moved to position %d relative to <%s> by user or administrator <%s>")), /* catgets 3427 */
                         timeStampStr,
                         event->position,
                         topOrBot(event->base),
                         event->userName);
            else
                sprintf (prline,
                         (_i18n_msg_get(ls_catd,NL_SETN,3428, "%-12.19s:%s moved to position %d relative to <%s> by user or administrator <%s>")), /* catgets 3428 */
                         timeStampStr,
                         tBuff,
                         event->position,
                         topOrBot(event->base),
                         event->userName);
            prtLine(prline);
            break;
        case EVENT_JOB_STATUS:
        case EVENT_QUEUE_CTRL:
            if (tBuff[0] == '\0')
                sprintf(prline, "%-12.19s: %s", timeStampStr,
                        getEventStatus(event));
            else
                sprintf(prline, "%-12.19s: %s %s", timeStampStr, tBuff,
                        getEventStatus(event));


            cp1 = strchr(prline, '\n');
            if (cp1 != NULL)
                *cp1 = '\000';
            prtLine(prline);
            while (cp1 != NULL)
            {
                prtLine("\n");
                cp = cp1+1;
                cp1 = strchr(cp, '\n');
                if (cp1 != NULL)
                {
                    *cp1 = '\000';
                    prtLine("                     ");
                    prtLine(cp);
                }
            }
            if ((event->jStatus & JOB_STAT_DONE) ||
                ((event->jStatus & JOB_STAT_EXIT) &&
                 !(event->reasons & (EXIT_ZOMBIE | EXIT_RERUN |
                                     EXIT_KILL_ZOMBIE))))
            {
                LS_WAIT_T wStatus;

                LS_STATUS(wStatus) = event->exitStatus;
                if ((event->jStatus & JOB_STAT_EXIT) &&
                    event->exitStatus &&
                    event->cpuTime >= MIN_CPU_TIME)
                {
                    if (WIFEXITED(wStatus))
                        sprintf(prline, " %s %d",
                                (_i18n_msg_get(ls_catd,NL_SETN,3282, "with exit code")), /* catgets  3282  */
                                WEXITSTATUS(wStatus));
                    else
                        sprintf(prline, " %s %d",
                                (_i18n_msg_get(ls_catd,NL_SETN,3283, "by signal")), /* catgets  3283  */
                                WTERMSIG(wStatus));
                    prtLine(prline);
                }

                if ( (job->numExHosts > 0)
                     && ( !IS_POST_FINISH(event->jStatus) ) )
                {
                    if (event->cpuTime < MIN_CPU_TIME)
                        sprintf(prline,
                                _i18n_msg_get(ls_catd,NL_SETN,3284, "The CPU time used is unknown")); /* catgets  3284  */
                    else
                    {
                        if (bhistReq->options & OPT_NORMALIZECPU)
                            event->cpuTime = event->cpuTime *
                                job->cpuFactor /bhistReq->cpuFactor;
                        sprintf(prline,
                                _i18n_msg_get(ls_catd,NL_SETN,3285, "The CPU time used is %1.1f seconds"),  /* catgets  3285  */
                                event->cpuTime);
                    }
                    prtLine(". ");
                    prtLine(prline);
                }
            }
            break;

        case EVENT_JOB_MODIFY:
            prtParameters (event->newParams, bhistReq, timeStampStr);
            break;

        case EVENT_JOB_MODIFY2: {
            prtModifiedJob(&(event->eventRecUnion.jobModLog), bhistReq, timeStampStr);

        }
            break;

        case EVENT_JOB_EXECUTE:
            if (tBuff[0] == '\0')
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3286, "%-12.19s: Running with execution home <%s>, Execution CWD <%s>, Execution Pid <%d>")),  /* catgets  3286  */
                        timeStampStr,
                        event->execHome,
                        event->execCwd,
                        event->jobPid);
            else
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3287, "%-12.19s:%s running with execution home <%s>, Execution CWD <%s>, Execution Pid <%d>")),  /* catgets  3287  */
                        timeStampStr,
                        tBuff,
                        event->execHome,
                        event->execCwd,
                        event->jobPid);
            prtLine(prline);
            if (event->execUsername && strcmp(event->execUsername, "")
                && strcmp(job->user, event->execUsername))
            {
                sprintf(prline, ", %s <%s>",
                        (_i18n_msg_get(ls_catd,NL_SETN,3288, "Execution user name")),  /* catgets  3288  */
                        event->execUsername);
                prtLine(prline);
            }
            prtLine(";\n");
            break;

        case EVENT_JOB_FORCE:
            if (tBuff[0] == '\0')
                sprintf(prline, "%-12.19s: %s <%s>",
                        timeStampStr,
                        I18N(3289, "Job is forced to run by user or administrator")/* catgets  3289  */,
                        event->userName);
            else
                sprintf(prline, "%-12.19s: %s %s <%s>",
                        timeStampStr,
                        tBuff,
                        I18N(3290, "is forced to run by user or administrator"), /* catgets 3290 */
                        event->userName);
            prtLine(prline);
            break;

        case EVENT_JOB_REQUEUE:
            if (tBuff[0] == '\0')
                sprintf(prline, "%-12.19s: %s",
                        timeStampStr,
                        (_i18n_msg_get(ls_catd,NL_SETN,3291, "Pending: Job has been requeued")));  /* catgets  3291  */

            else
                sprintf(prline, "%-12.19s:%s %s",
                        timeStampStr,
                        tBuff,
                        (_i18n_msg_get(ls_catd,NL_SETN,3292, "pending: Job has been requeued")));  /* catgets  3292  */
            prtLine(prline);
            break;
        case EVENT_JOB_CLEAN:
            sprintf(prline, "%-12.19s: %s",
                    timeStampStr,
                    (_i18n_msg_get(ls_catd,NL_SETN,3293, "Cleaned: Job has been removed")));  /* catgets  3293  */
            prtLine(prline);
            break;
    }
    return;

}

static void
printChronicleEventLog(struct eventRec *log, struct bhistReq *req)
{
    static  char fname[] = "printChronicleEventLog";
    struct  jobInfoEnt *job;
    struct  submit *submitPtr;
    struct  eventRecord *event;
    char    prline[MAXLINELEN], tBuff[20];
    int     i;
    char    *hostPtr;
    LS_LONG_INT     jobId;
    char    timeStampStr[64];
    struct  eventRecord eventT;
    char    *cp, *cp1;

    event = &eventT;
    foundJob = TRUE;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG2, "%s: log.type=%x", fname, log->type);

    strcpy(timeStampStr, _i18n_ctime(ls_catd,  CTIME_FORMAT_a_b_d_T, (time_t *)&(log->eventTime)));

    foundJob = TRUE;

    job = NULL;
    switch (log->type) {
        case EVENT_JOB_NEW:
            job = read_newjob(log);
            if (job == NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL,fname, "read_newjob" );
                break;
            }
            submitPtr = &job->submit;
            prtJobSubmit(job, TRUE, TRUE);
            prtFileNames(job, TRUE);
            if (job->submit.options & SUB_JOB_NAME)
            {
                sprintf(prline, ", %s <%s>",
                        I18N(3296, "Job Name"),   /* catgets  3296  */
                        job->submit.jobName);
                prtLine(prline);
            }
            prtHeader(job, FALSE, TRUE);
            hostPtr = job->submit.hostSpec;
            if (strcmp (job->submit.hostSpec, "") == 0)
            {
                if (job->numExHosts > 0)
                    hostPtr = job->exHosts[0];
                else
                    hostPtr = job->fromHost;
            }
            prtSubDetails(job, hostPtr, job->cpuFactor);
            freeJobInfoEnt(job);
            break;

        case EVENT_JOB_START:
        case EVENT_PRE_EXEC_START:
            jobId = LSB_JOBID(log->eventLog.jobStartLog.jobId,
                              log->eventLog.jobStartLog.idx);
            sprintf (tBuff, "%s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));

            if (log->type == EVENT_PRE_EXEC_START)
                sprintf(prline, "%-12.19s:%s, %s",
                        timeStampStr,
                        tBuff,
                        (_i18n_msg_get(ls_catd,NL_SETN,3298, "the pre-exec command is started on"))); /* catgets  3298  */
            else
                sprintf(prline, "%-12.19s:%s, %s",
                        timeStampStr,
                        tBuff,
                        (_i18n_msg_get(ls_catd,NL_SETN,3299, "the batch job command is started on"))); /* catgets  3299  */

            prtLine(prline);
            if (log->eventLog.jobStartLog.numExHosts > 1) {
                sprintf(prline, " %d %s",
                        log->eventLog.jobStartLog.numExHosts,
                        (_i18n_msg_get(ls_catd,NL_SETN,3300, "Hosts/Processors")));  /* catgets  3300  */
                prtLine(prline);
            }
            for (i=0;i<log->eventLog.jobStartLog.numExHosts;i++) {
                sprintf(prline, " <%s>", log->eventLog.jobStartLog.execHosts[i]);
                prtLine(prline);
            }
            prtLine(";\n");
            break;

        case EVENT_JOB_EXECUTE:
            jobId = LSB_JOBID(log->eventLog.jobExecuteLog.jobId,
                              log->eventLog.jobExecuteLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline,
                    (_i18n_msg_get(ls_catd,NL_SETN,3302, "%-12.19s:%s running with execution home <%s>, Execution CWD <%s>, Execution Pid <%d>")),  /* catgets  3302  */
                    timeStampStr,
                    tBuff,
                    log->eventLog.jobExecuteLog.execHome,
                    log->eventLog.jobExecuteLog.execCwd,
                    log->eventLog.jobExecuteLog.jobPid);
            prtLine(prline);
            if (log->eventLog.jobExecuteLog.execUsername &&
                strcmp(log->eventLog.jobExecuteLog.execUsername, ""))
            {
                sprintf(prline, ", %s <%s>",
                        (_i18n_msg_get(ls_catd,NL_SETN,3303, "Execution user name")),  /* catgets  3303  */
                        log->eventLog.jobExecuteLog.execUsername);
                prtLine(prline);
            }
            prtLine(";\n");
            break;

        case EVENT_JOB_STATUS:
        { struct jobStatusLog *jobStatusLog;
                jobStatusLog = &log->eventLog.jobStatusLog;

                event->jStatus = jobStatusLog->jStatus;
                event->cpuTime = jobStatusLog->cpuTime;
                event->reasons = jobStatusLog->reason;
                event->subreasons = jobStatusLog->subreasons;
                event->exitStatus = jobStatusLog->exitStatus;
                event->timeStamp = log->eventTime;
                event->idx = jobStatusLog->idx;
                event->ld = loadIndex;

                jobId = LSB_JOBID(log->eventLog.jobStatusLog.jobId,
                                  log->eventLog.jobStatusLog.idx);
                sprintf (tBuff, " %s <%s>",
                         I18N_Job,
                         lsb_jobid2str(jobId));

                sprintf(prline, "%-12.19s:%s %s", timeStampStr, tBuff,
                        lowFirstChar(getEventStatus(event)));


                cp1 = strchr(prline, '\n');
                if (cp1 != NULL) {
                    *cp1 = '\000';
                    prtLine(prline);
                    while (cp1 != NULL)
                    {
                        prtLine("\n");
                        cp = cp1+1;
                        cp1 = strchr(cp, '\n');
                        if (cp1 != NULL)
                        {
                            *cp1 = '\000';
                            prtLine("                     ");
                            prtLine(cp);
                        }
                    }
                } else {
                    prtLine(prline);
                    prtLine("\n");
                }
                if ((event->jStatus & JOB_STAT_DONE) ||
                    ((event->jStatus & JOB_STAT_EXIT) &&
                     !(event->reasons & (EXIT_ZOMBIE | EXIT_RERUN |
                                         EXIT_KILL_ZOMBIE))))
                {
                    LS_WAIT_T wStatus;

                    LS_STATUS(wStatus) = event->exitStatus;
                    if ((event->jStatus & JOB_STAT_EXIT) &&
                        event->exitStatus &&
                        event->cpuTime >= MIN_CPU_TIME)
                    {
                        if (WIFEXITED(wStatus))
                            sprintf(prline, " %s %d",
                                    (_i18n_msg_get(ls_catd,NL_SETN,3305, "with exit code")), /* catgets  3305  */
                                    WEXITSTATUS(wStatus));
                        else
                            sprintf(prline, " %s %d",
                                    (_i18n_msg_get(ls_catd,NL_SETN,3283, "by signal")), /* catgets  3283  */
                                    WTERMSIG(wStatus));
                        prtLine(prline);
                        prtLine("\n");
                    }
                    if (event->numExHosts > 0)
                    {
                        if (event->cpuTime < MIN_CPU_TIME)
                            sprintf(prline, ". %s",
                                    (_i18n_msg_get(ls_catd,NL_SETN,3307, "The CPU time used is unknown"))); /* catgets  3307  */
                        else
                        {
                            if (req->options & OPT_NORMALIZECPU)
                                event->cpuTime = event->cpuTime *
                                    job->cpuFactor /req->cpuFactor;
                            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,3308, ". The CPU time used is %1.1f seconds")),  /* catgets  3308  */
                                    event->cpuTime);
                        }
                        prtLine(prline);
                        prtLine("\n");
                    }
                }
        }
        break;
        case EVENT_JOB_SWITCH:
            jobId = LSB_JOBID(log->eventLog.jobSwitchLog.jobId,
                              log->eventLog.jobSwitchLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf (prline,
                     (_i18n_msg_get(ls_catd,NL_SETN,3310, "%-12.19s:%s switched to Queue <%s> by user or administrator <%s>")), /* catgets  3310  */
                     timeStampStr,
                     tBuff,
                     log->eventLog.jobSwitchLog.queue,
                     log->eventLog.jobSwitchLog.userName);
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_MOVE:
            jobId = LSB_JOBID(log->eventLog.jobMoveLog.jobId,
                              log->eventLog.jobMoveLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf (prline,
                     (_i18n_msg_get(ls_catd,NL_SETN,3312, "%-12.19s:%s moved to position %d relative to <%s> by user or administrator <%s>")), /* catgets  3312  */
                     timeStampStr,
                     tBuff,
                     log->eventLog.jobMoveLog.position,
                     topOrBot(log->eventLog.jobMoveLog.base),
                     log->eventLog.jobMoveLog.userName);
            prtLine(prline);
            prtLine(";\n");
            break;
        case EVENT_MBD_UNFULFILL:
            break;
        case EVENT_JOB_FINISH:
            break;
        case EVENT_LOAD_INDEX:
            break;
        case EVENT_CHKPNT:
            jobId = LSB_JOBID(log->eventLog.chkpntLog.jobId,
                              log->eventLog.chkpntLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf (prline,
                     (_i18n_msg_get(ls_catd,NL_SETN,3314, "%-12.19s:%s checkpoint info with period<%ld> pid<%d> ok<%d> flags<%d>")), /* catgets  3314  */
                     timeStampStr,
                     tBuff,
                     log->eventLog.chkpntLog.period,
                     log->eventLog.chkpntLog.pid,
                     log->eventLog.chkpntLog.ok,
                     log->eventLog.chkpntLog.flags);
            prtLine(prline);
            prtLine(";\n");
            break;
        case EVENT_MIG:
            jobId = LSB_JOBID(log->eventLog.migLog.jobId,
                              log->eventLog.migLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            if (log->eventLog.migLog.userName && log->eventLog.migLog.userName[0])
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3316, "%-12.19s:%s migration requested by user or administrator <%s>")), /* catgets  3316  */
                        timeStampStr,
                        tBuff,
                        log->eventLog.migLog.userName);
            else
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3317, "%-12.19s:%s migration requested by unknown user")),  /* catgets  3317  */
                        timeStampStr,
                        tBuff);

            if (log->eventLog.migLog.numAskedHosts)
            {
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,3318, "%s; Specified Hosts")), prline); /* catgets  3318  */
                for (i = 0; i < log->eventLog.migLog.numAskedHosts; i++) {
                    sprintf(prline, "%s <%s>", prline,
                            log->eventLog.migLog.askedHosts[i]);
                    if (i != (log->eventLog.migLog.numAskedHosts - 1))
                        strcat(prline, ",");
                }
            }
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_MODIFY:
            job = read_newjob (log);
            if (job == NULL) {
                ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname,"read_newjob" );
                break;
            }
            prtParameters (job, req, timeStampStr);
            freeJobInfoEnt(job);
            break;

        case EVENT_JOB_SIGNAL:
            jobId = LSB_JOBID(log->eventLog.signalLog.jobId,
                              log->eventLog.signalLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            if (strcmp (log->eventLog.signalLog.signalSymbol, "DELETEJOB") == 0)
            {
                sprintf(prline, "%-12.19s:%s, %s <%s>",
                        timeStampStr,
                        tBuff,
                        I18N(3321, "delete requested by user or administrator"), /* catgets 3321 */
                        log->eventLog.signalLog.userName);
                if (log->eventLog.signalLog.runCount > 0)
                    sprintf(prline, "%s; %s %d",
                            prline,
                            (_i18n_msg_get(ls_catd,NL_SETN,3322, "running times is")), /* catgets  3322  */
                            log->eventLog.signalLog.runCount);
            } else
                sprintf(prline,
                        (_i18n_msg_get(ls_catd,NL_SETN,3323, "%-12.19s:%s, signal <%s> requested by user or administrator <%s>")),  /* catgets  3323  */
                        timeStampStr,
                        tBuff,
                        log->eventLog.signalLog.signalSymbol,
                        log->eventLog.signalLog.userName);
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_MSG:
            jobId = LSB_JOBID(log->eventLog.jobMsgLog.jobId,
                              log->eventLog.jobMsgLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline,
                    (_i18n_msg_get(ls_catd,NL_SETN,3329, "%-12.19s:%s, message <%s>  message ID<%d> requested by user or administrator <%s>")),  /* catgets  3329  */
                    timeStampStr, tBuff,
                    log->eventLog.jobMsgLog.msg,
                    log->eventLog.jobMsgLog.msgId,
                    getUserName(log->eventLog.jobMsgLog.usrId));
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_MSG_ACK:
            jobId = LSB_JOBID(log->eventLog.jobMsgAckLog.jobId,
                              log->eventLog.jobMsgAckLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline,
                    (_i18n_msg_get(ls_catd,NL_SETN,3331, "%-12.19s:%s, message <%s> message ID<%d>has been dispatchd")),  /* catgets  3331  */
                    timeStampStr,
                    tBuff,
                    log->eventLog.jobMsgAckLog.msg,
                    log->eventLog.jobMsgAckLog.msgId);
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_REQUEUE:
            jobId = LSB_JOBID(log->eventLog.jobRequeueLog.jobId,
                              log->eventLog.jobRequeueLog.idx);
            sprintf (tBuff, "%s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline, "%-12.19s:%s %s",
                    timeStampStr,
                    tBuff,
                    I18N(3333, "pending: Job has been requeued") /* catgets  3333  */);
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_SIGACT:
        {
            struct sigactLog *sigactLog;

            sigactLog = &log->eventLog.sigactLog;
            jobId = LSB_JOBID(sigactLog->jobId, sigactLog->idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));

            if ((strcmp(sigactLog->signalSymbol, "SIG_CHKPNT") == 0)
                || (strcmp(sigactLog->signalSymbol, "SIG_CHKPNT_COPY") == 0))
            {



                if (IS_PEND(sigactLog->jStatus))
                {

                    if (sigactLog->period)
                        sprintf(prline,
                                (_i18n_msg_get(ls_catd,NL_SETN,3335, "%-12.19s:%s, checkpoint period is set to %d min.")),  /* catgets  3335  */
                                timeStampStr,
                                tBuff,
                                (int) (sigactLog->period / 60));
                    prtLine(prline);
                    break;
                }


                if (sigactLog->actStatus == ACT_START)
                {
                    char *tmpPtr;
                    tmpPtr = putstr_((_i18n_msg_get(ls_catd,NL_SETN,3262, "initiated")));
                    sprintf(prline,"%-12.19s:%s %s %s (actpid %d)",
                            timeStampStr,
                            tBuff,
                            (sigactLog->flags & LSB_CHKPNT_MIG) ?
                            (_i18n_msg_get(ls_catd,NL_SETN,3337, "Migration checkpoint")) :  /* catgets  3337  */
                            (_i18n_msg_get(ls_catd,NL_SETN,3338, "Checkpoint")), /* catgets  3338  */
                            tmpPtr,
                            sigactLog->pid);
                    free( tmpPtr );
                } else if (sigactLog->actStatus == ACT_DONE ||
                           sigactLog->actStatus == ACT_FAIL)
                {
                    char *tmpPtr;
                    tmpPtr = putstr_((sigactLog->actStatus == ACT_DONE) ?
                                     (_i18n_msg_get(ls_catd,NL_SETN,3341, "succeeded")) : /* catgets  3341  */
                                     (_i18n_msg_get(ls_catd,NL_SETN,3342, "failed"))); /* catgets  3342  */
                    sprintf(prline,
                            "%-12.19s:%s %s %s (actpid %d)",
                            timeStampStr,
                            tBuff,
                            (sigactLog->flags & LSB_CHKPNT_MIG) ?
                            (_i18n_msg_get(ls_catd,NL_SETN,3337, "Migration checkpoint")) :  /* catgets  3337  */
                            (_i18n_msg_get(ls_catd,NL_SETN,3338, "Checkpoint")), /* catgets  3338  */
                            tmpPtr,
                            sigactLog->pid);
                    free ( tmpPtr );
                }
                prtLine(prline);
            } else
            {

                switch(sigactLog->actStatus)
                {
                    case ACT_START:
                        sprintf(prline, "%-12.19s:%s %s %s (actpid %d)",
                                timeStampStr,
                                tBuff,
                                actionName(sigactLog->signalSymbol),
                                I18N(3262, "initiated"),
                                sigactLog->pid);
                        break;
                    case ACT_DONE:
                    case ACT_FAIL:
                        sprintf(prline,
                                "%-12.19s:%s %s %s (actpid %d)",
                                timeStampStr,
                                tBuff,
                                actionName(sigactLog->signalSymbol),
                                (sigactLog->actStatus == ACT_DONE) ?
                                (_i18n_msg_get(ls_catd,NL_SETN,3343, "completed")) :  /* catgets  3343  */
                                (_i18n_msg_get(ls_catd,NL_SETN,3344, "exited with non-zero value")), /* catgets  3344  */
                                sigactLog->pid);
                        break;
                }
                prtLine(prline);
            }
        }
	prtLine(";\n");
        break;

        case EVENT_SBD_JOB_STATUS:
            break;

        case EVENT_JOB_START_ACCEPT:
            jobId = LSB_JOBID(log->eventLog.jobStartAcceptLog.jobId,
                              log->eventLog.jobStartAcceptLog.idx);
            sprintf (tBuff, "%s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline,
                    (_i18n_msg_get(ls_catd,NL_SETN,3347, "%-12.19s:%s, starting (Pid %d)")), /* catgets  3347  */
                    timeStampStr,
                    tBuff,
                    log->eventLog.jobStartAcceptLog.jobPid);
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_CLEAN:
            jobId = LSB_JOBID(log->eventLog.jobCleanLog.jobId,
                              log->eventLog.jobCleanLog.idx);
            sprintf (tBuff, "%s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline, "%-12.19s:%s %s",
                    timeStampStr,
                    tBuff,
                    I18N(3349, "has been cleaned") /* catgets 3349 */ );
            prtLine(prline);
            prtLine(";\n");
            break;

        case EVENT_JOB_FORCE:
            jobId = LSB_JOBID(log->eventLog.jobForceRequestLog.jobId,
                              log->eventLog.jobForceRequestLog.idx);
            sprintf (tBuff, " %s <%s>",
                     I18N_Job,
                     lsb_jobid2str(jobId));
            sprintf(prline, "%-12.19s:%s %s <%s>",
                    timeStampStr,
                    tBuff,
                    I18N(3290, "is forced to run by user or administrator"),
                    log->eventLog.jobForceRequestLog.userName);
            prtLine(prline);
            prtLine(";\n");
            break;


        case EVENT_LOG_SWITCH:
        case EVENT_QUEUE_CTRL:
        case EVENT_HOST_CTRL:
        case EVENT_MBD_DIE:
        case EVENT_MBD_START:
        {
            struct histReq histReq;

            histReq.opCode 		= SYS_HIST;
            histReq.names         	= NULL;
            histReq.eventTime[0]  	= 0;
            histReq.eventTime[1]  	= 0;
            histReq.eventFileName 	= NULL;
            histReq.found         	= 0;

            displayEvent(log, &histReq);
        }
	break;
        default:
            ls_syslog(LOG_ERR, "%s: %s, log.type=%x",
                      fname, I18N(3356, "unknown type"), /* catgets 3356 */
                      log->type);
            break;
    }
}

static char *
lowFirstChar(char *statement)
{
    if (isalpha(*statement))
        *statement = tolower(*statement);
    return(statement);

}

static int
initJobIdIndexS( struct jobIdIndexS *indexS, char *fileName )
{
    int   cc;
    char  tag[80];
    char  version[80];

    if ((indexS->fp = fopen(fileName, "r")) == NULL) {
	if (errno != ENOENT) {
	    fprintf(stderr,  "failed to open the jobId index file.\n");
	    perror(fileName);
	}
	return(-1);
    }

    cc = fscanf(indexS->fp, "\
%s %s %d %ld", tag, version, &(indexS->totalRows),
                &(indexS->lastUpdate));
    if (cc != 4
        || strcmp(tag, LSF_JOBIDINDEX_FILETAG)) {
        fprintf(stderr, "wrong jobId index file format.\n");
        fclose(indexS->fp);
        return(-1);
    }

    fseek(indexS->fp, 80, SEEK_SET);

    strcpy(indexS->fileName, fileName);
    indexS->version = atof(version);
    indexS->curRow = 0;

    return(0);
}

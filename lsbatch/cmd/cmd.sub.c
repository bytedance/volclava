/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: cmd.sub.c 397 2007-11-26 19:04:00Z mblack $
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
#include "../lib/lsb.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <wordexp.h>

#define NL_SETN 8

extern char *loginShell;
extern int  optionFlag;
extern char optionFileName[MAXLSFNAMELEN];
extern int sig_decode (int);
extern int isatty(int);

extern  int setOption_ (int argc, char **argv, char *template,
                      struct submit *req, int mask, int mask2, char **errMsg);
extern  struct submit * parseOptFile_(char *filename,
				      struct submit *req, char **errMsg);
extern void subUsage_(int, char **);
static  int parseLine (char *line, int *embedArgc, char ***embedArgv, int option);
extern int createJobInfoFile(struct submit *jobSubReq, struct lenData *jf);
extern void subNewLine_(char *str);
extern void makeCleanToRunEsub(void);
extern void modifyJobInformation(struct submit *jobSubReq);

static int emptyCmd = TRUE;

static int parseScript (FILE *from,  int *embedArgc,
			char ***embedArgv, int option);
static int CopyCommand(char **, int);

static int
addLabel2RsrcReq(struct submit *subreq);

void sub_perror (char *);
void do_pack_sub (int option, char **argv, struct submit *req);
static LS_LONG_INT do_pack_sub_v2(int option, char **argv, struct submit *req);

char **split_commandline(const char *cmdline, int *argc);

static char *commandline;

#define SKIPSPACE(sp)      while (isspace(*(sp))) (sp)++;
#define SCRIPT_WORD        "_USER_\\SCRIPT_"
#define SCRIPT_WORD_END       "_USER_SCRIPT_"

#define EMBED_INTERACT     0x01
#define EMBED_OPTION_ONLY  0x02
#define EMBED_BSUB         0x04
#define EMBED_RESTART      0x10
#define EMBED_QSUB         0x20

#define DEFAULT_LSB_PACK_SKIP_ERROR    "N"
#define DEFAULT_LSB_MAX_PACK_JOBS      (0)

static int packSkipErrFlag = FALSE;
static int lsbMaxPackJobs = DEFAULT_LSB_MAX_PACK_JOBS;


int
do_sub (int argc, char **argv, int option)
{
    static char fname[] = "do_sub";
    struct submit  req;
    struct submitReply  reply;
    static int nRetries = 6;
    static int countTries = 1;
    static int subTryInterval = DEF_SUB_TRY_INTERVAL;
    static char *envLSBNTries;
    LS_LONG_INT jobId = -1;

    if ((envLSBNTries = getenv("LSB_NTRIES")) != NULL) {
        nRetries = atoi64_(envLSBNTries);
    }

    if (lsb_init(argv[0]) < 0) {
        sub_perror("lsb_init");
        fprintf(stderr, ". %s.\n",
                (_i18n_msg_get(ls_catd,NL_SETN,1551, "Job not submitted"))); /* catgets  1551  */
        return (-1);
    }

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if (fillReq (argc, argv, option, &req, FALSE) < 0){
        fprintf(stderr,  ". %s.\n",
                (_i18n_msg_get(ls_catd,NL_SETN,1551, "Job not submitted")));
        return (-1);
    }


    memset(&reply, 0, sizeof(struct submitReply));

    if (req.options & SUB_PACK) {
        if (do_pack_sub_v2(option, argv, &req) < 0) {
            return (-1);
        }
    } else {
        do {
            TIMEIT(0, (jobId = lsb_submit(&req, &reply)), "lsb_submit");

            if (jobId > 0) {
                break;
            }

            subTryInterval = reply.subTryInterval;

            prtErrMsg (&req, &reply);

            if (lsberrno != LSBE_JOB_MAX_PEND && lsberrno != LSBE_SLOTS_MAX_PEND) {
                // currently only retry on LSBE_JOB_MAX_PEND or LSBE_SLOTS_MAX_PEND
                countTries = nRetries;
            }

            if ( countTries >= nRetries) {
                fprintf(stderr,  ". %s.\n",
                        (_i18n_msg_get(ls_catd,NL_SETN,1561, "Job not submitted")));
                return(-1);
            }

            fprintf(stderr,
                    (_i18n_msg_get(ls_catd,NL_SETN,1562, ". Retrying in %d seconds...\n")), subTryInterval);

            countTries++;
            sleep(subTryInterval);
        } while (jobId < 0 && countTries <= nRetries);
    }

    if (req.nxf)
        free(req.xf);

    return(0);

}

void
do_pack_sub (int option, char **argv, struct submit *req)
{
    static char fname[] = "do_pack_sub";

    FILE *fp;
    int lineNum;
    int packParsed;
    int packSubmit;
    int packError;
    int parseError = FALSE;
    char *line;
//    char *packFile = "";

    int size = 10*PATH_MAX;

    char **packedArgv;
    int  packedArgc = 0;
    struct submit  packReq;
    struct submitReply  packReply;
    LS_LONG_INT jobId = -1;
    char tmpBuf[MAXLINELEN];

    if (lsbMaxPackJobs == DEFAULT_LSB_MAX_PACK_JOBS) {
        fprintf(stderr,  "Pack submission disabled by LSB_MAX_PACK_JOBS in lsf.conf. Job not submitted.\n");
        return(-1);
    }

//    if ((packFile = (char *) malloc(size)) == NULL) {
//        fprintf(stderr, I18N_FUNC_FAIL,fname,"malloc" );
//        return (FALSE);
//    }

//    strcpy(packFile, commandline);

    // parse pack file
    fp = fopen(req->packFile, "r");
    if (!fp) {
        lserrno = LSE_NO_FILE;
        fprintf(stderr,  "Cannot read file <%s>. Job not submitted.\n", req->packFile);
        return(-1);
    }

    lineNum = 0;
    packParsed = 0;
    packSubmit = 0;
    packError = 0;
    while ((line = getNextLineC_(fp, &lineNum, TRUE)) != NULL) {
        parseError = FALSE;
        packParsed ++;
        // current implementation have to print "Line#lineNum" at first, and could not
        // check print as stdout or stderr due to it calls the function of lsb_submit,
        // so we just print it at stdout and rewrite it along with the optimize project.
        fprintf(stdout, "Line#%d ", lineNum);

        sprintf(tmpBuf, "%s %s", argv[0], line);
        packedArgv = split_commandline(tmpBuf, &packedArgc);
        if (packedArgv == NULL) {
            fprintf(stderr,  "failed to parsed line %d. %s. %s\n", lineNum, tmpBuf, req->packFile);

            packError ++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        optind = 1;

        if (fillReq (packedArgc, packedArgv, CMD_BSUB, &packReq, TRUE) < 0){
            fprintf(stderr,  ". %s.\n",
                    (_i18n_msg_get(ls_catd,NL_SETN,1551, "Job not submitted")));

            packError ++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        if (packReq.options2 & SUB2_BSUB_BLOCK) {
            fprintf(stderr,  "Option -K is not supported in -pack job submission file."
                             " Job not submitted.\n");
            parseError = TRUE;
        } else if (packReq.options & SUB_INTERACTIVE){
            fprintf(stderr,  "Option -I is not supported in -pack job submission file."
                             " Job not submitted.\n");
            parseError = TRUE;
        } else if (packReq.options & SUB_PACK){
            fprintf(stderr,  "Option -pack is not supported in -pack job submission file."
                             " Job not submitted.\n");
            parseError = TRUE;
        }
        if (parseError) {
            packError ++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }


        memset(&packReply, 0, sizeof(struct submitReply));

        TIMEIT(0, (jobId = lsb_submit(&packReq, &packReply)), "lsb_submit");
        if (jobId < 0) {
            prtErrMsg (&packReq, &packReply);
            fprintf(stderr,  ". %s.\n",
                    (_i18n_msg_get(ls_catd,NL_SETN,1551, "Job not submitted")));
            packError ++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        } else {
            packSubmit ++;
        }
    }

    fprintf(stdout,  "%d lines parsed, %d jobs submitted, %d errors found.\n",
            packParsed, packSubmit, packError);

    fclose(fp);
//    free(packFile);

    return;
}


LS_LONG_INT do_pack_sub_v2(int option, char **argv, struct submit *req)
{
    static char fname[] = "do_pack_sub_v2()";
    FILE *fp;
    int lineNum;
    int packParsedNum, packParsedTotal;
    LS_LONG_INT packSubmit;          /* Successfully submitted jobs */
    int packParseError;      /* Parse/validation errors */
    int parseError = FALSE;
    int mbdHandledError = FALSE;
    char *line;

    struct submit **job_requests = NULL;
    struct packOutputs **pack_outputs = NULL;

    struct submitPackReply submitPackRep;

    char outputTmp[MAXLINELEN];

    int job_count = 0;
    int i, j;

    int outputTotal = 0;
    int outputError = 0;

    int esubPrintFD = 0;

    /* Check if pack submission is enabled */
    if (lsbMaxPackJobs <= DEFAULT_LSB_MAX_PACK_JOBS) {
        fprintf(stderr, "Pack submission disabled by LSB_MAX_PACK_JOBS in lsf.conf. Job not submitted.\n");
        return(-1);
    }

    /* Check if file is a text file before opening */
    if (!is_text_file(req->packFile)) {
        fprintf(stderr, "Error: File <%s> is not a valid text file. Job not submitted.\n", req->packFile);
        return(-1);
    }

    fp = fopen(req->packFile, "r");
    if (!fp) {
        lserrno = LSE_NO_FILE;
        fprintf(stderr, "Cannot read file <%s>. Job not submitted.\n", req->packFile);
        return(-1);
    }

    lineNum = 0;
    packParsedNum = 0;
    packSubmit = 0;
    packParseError = 0;

    packParsedTotal = getTotalLine(req->packFile);
    /* Check if we've reached the maximum job limit */
    if (packParsedTotal > lsbMaxPackJobs) {
        fprintf(stderr, "Warning: The number of valid lines in pack file is %d, exceeds the LSB_MAX_PACK_JOBS=%d "
                        "defined in lsf.conf.\n", packParsedTotal, lsbMaxPackJobs);
        packParsedTotal = lsbMaxPackJobs;
    }

    job_requests = malloc(packParsedTotal * sizeof(struct submit *));
    pack_outputs = malloc(packParsedTotal * sizeof(struct packOutputs *));
    if (!job_requests || !pack_outputs) {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(fp);
        return -1;
    }
    memset(job_requests, 0, packParsedTotal * sizeof(struct submit *));
    memset(pack_outputs, 0, packParsedTotal * sizeof(struct packOutputs *));

    esubPrintFD = open(LSDEVNULL, O_RDWR, 0);

    while ((line = getNextLineC_(fp, &lineNum, TRUE)) != NULL && packParsedNum < lsbMaxPackJobs) {
        char **packedArgv;
        int packedArgc = 0;
        struct submit packReq;
        char tmpBuf[MAXLINELEN];

        packParsedNum++;

        pack_outputs[packParsedNum-1] = malloc(sizeof(struct packOutputs));
        if (!pack_outputs[packParsedNum-1]) {
            fprintf(stderr, "Line#%d Memory allocation failed for job request. Job not submitted.\n", lineNum);

            packParseError++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }
        pack_outputs[packParsedNum-1]->lineNum = lineNum;
        pack_outputs[packParsedNum-1]->outputMSG = NULL;
        pack_outputs[packParsedNum-1]->packSubmitIndex = -1;

        snprintf(tmpBuf, sizeof(tmpBuf), "%s %s", argv[0], line);
        packedArgv = split_commandline(tmpBuf, &packedArgc);
        if (packedArgv == NULL) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d Failed to parse command line: \"%s\". Job not submitted.\n",
                    lineNum, line);
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            packParseError++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        optind = 1;
        if (fillReq(packedArgc, packedArgv, CMD_BSUB, &packReq, TRUE) < 0) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d %s. %s.\n", lineNum, lsb_sysmsg(),
                    (_i18n_msg_get(ls_catd,NL_SETN,1551, "Job not submitted")));
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            packParseError++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        parseError = FALSE;
        if (packReq.options2 & SUB2_BSUB_BLOCK) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d Option -K is not supported in -pack job submission "
                                                   "file. Job not submitted.\n", lineNum);
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            parseError = TRUE;
        } else if (packReq.options & SUB_INTERACTIVE) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d Option -I is not supported in -pack job submission "
                                                   "file. Job not submitted.\n", lineNum);
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            parseError = TRUE;
        } else if (packReq.options & SUB_PACK) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d Option -pack is not supported in -pack job submission "
                                                   "file. Job not submitted.\n", lineNum);
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            parseError = TRUE;
        }

        if (parseError) {
            packParseError++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        subNewLine_(packReq.resReq);
        subNewLine_(packReq.dependCond);
        subNewLine_(packReq.preExecCmd);
        subNewLine_(packReq.postExecCmd);
        subNewLine_(packReq.mailUser);
        subNewLine_(packReq.jobName);
        subNewLine_(packReq.queue);
        subNewLine_(packReq.inFile);
        subNewLine_(packReq.outFile);
        subNewLine_(packReq.errFile);
        subNewLine_(packReq.chkpntDir);
        subNewLine_(packReq.projectName);
        for(i = 0; i < packReq.numAskedHosts; i++) {
            subNewLine_(packReq.askedHosts[i]);
        }

        // 2. Set LSB_UNIXGROUP environment variable - simulate single job environment setup
        struct group *grpEntry = getgrgid(getgid());
        if (grpEntry != NULL) {
            if (putEnv("LSB_UNIXGROUP", grpEntry->gr_name) < 0) {
                fprintf(stderr, "Warning: Failed to set LSB_UNIXGROUP environment variable\n");
            }
        }

        // 3. Clean environment - simulate single job makeCleanToRunEsub call
        makeCleanToRunEsub();

        // 4. Queue default handling (first time) - simulate single job queue processing
        if (!(packReq.options & SUB_QUEUE)) {
            char *queue = getenv("LSB_DEFAULTQUEUE");
            if (queue != NULL && queue[0] != '\0') {
                packReq.queue = queue;
                packReq.options |= SUB_QUEUE;
            }
        }

        // 5. Modify job information - simulate single job modifyJobInformation call
        modifyJobInformation(&packReq);

        // 6. Queue default handling (second time) - simulate single job second queue processing
        if (!(packReq.options & SUB_QUEUE)) {
            char *queue = getenv("LSB_DEFAULTQUEUE");
            if (queue != NULL && queue[0] != '\0') {
                packReq.queue = queue;
                packReq.options |= SUB_QUEUE;
            }
        }

        // 7. Set interactive error handling - simulate single job LSF_INTERACTIVE_STDERR setting
        if ((lsbParams[LSB_INTERACTIVE_STDERR].paramValue != NULL) &&
            (strcasecmp(lsbParams[LSB_INTERACTIVE_STDERR].paramValue, "y") == 0)) {
            if (putEnv("LSF_INTERACTIVE_STDERR", "y") < 0) {
                fprintf(stderr, "Warning: Failed to set LSF_INTERACTIVE_STDERR environment variable\n");
            }
        }

        /* Initialize edata structure, simulate single job flow */
        struct lenData ed;
        memset(&ed, 0, sizeof(ed));

        /* Call runBatchEsub to handle environment variables and resource limits */
        if (runBatchEsub(&ed, &packReq, esubPrintFD) < 0) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d Request aborted by esub. Job not submitted.\n", lineNum);
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            packParseError++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        /* Cleanup edata */
        FREEUP(ed.data);


        /* Save job request and file data */
        job_requests[job_count] = malloc(sizeof(struct submit));
        if (!job_requests[job_count]) {
            snprintf(outputTmp, sizeof(outputTmp), "Line#%d Memory allocation failed for job request. Job not submitted.\n", lineNum);
            pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
            packParseError++;
            if (packSkipErrFlag) {
                continue;
            } else {
                break;
            }
        }

        /* Deep copy packReq to job_requests[job_count] */
        memset(job_requests[job_count], 0, sizeof(struct submit));
        job_requests[job_count]->options = packReq.options;
        job_requests[job_count]->options2 = packReq.options2;
        job_requests[job_count]->numAskedHosts = packReq.numAskedHosts;
        job_requests[job_count]->askedHosts = packReq.askedHosts;
        job_requests[job_count]->rLimits[0] = packReq.rLimits[0];
        job_requests[job_count]->rLimits[1] = packReq.rLimits[1];
        job_requests[job_count]->rLimits[2] = packReq.rLimits[2];
        job_requests[job_count]->rLimits[3] = packReq.rLimits[3];
        job_requests[job_count]->rLimits[4] = packReq.rLimits[4];
        job_requests[job_count]->rLimits[5] = packReq.rLimits[5];
        job_requests[job_count]->rLimits[6] = packReq.rLimits[6];
        job_requests[job_count]->rLimits[7] = packReq.rLimits[7];
        job_requests[job_count]->rLimits[8] = packReq.rLimits[8];
        job_requests[job_count]->rLimits[9] = packReq.rLimits[9];
        job_requests[job_count]->rLimits[10] = packReq.rLimits[10];
        job_requests[job_count]->beginTime = packReq.beginTime;
        job_requests[job_count]->termTime = packReq.termTime;
        job_requests[job_count]->sigValue = packReq.sigValue;
        job_requests[job_count]->chkpntPeriod = packReq.chkpntPeriod;
        job_requests[job_count]->nxf = packReq.nxf;
//        job_requests[job_count]->xf = packReq.xf;
        if (packReq.nxf > 0 && packReq.xf != NULL) {
            job_requests[job_count]->xf = malloc(packReq.nxf * sizeof(struct xFile));
            if (job_requests[job_count]->xf == NULL) {
                snprintf(outputTmp, sizeof(outputTmp), "Line#%d Memory allocation failed for xfiles. Job not submitted.\n", lineNum);
                pack_outputs[packParsedNum-1]->outputMSG = strdup(outputTmp);
                packParseError++;
                if (packSkipErrFlag) {
                    continue;
                } else {
                    break;
                }
            }
            memcpy(job_requests[job_count]->xf, packReq.xf, packReq.nxf * sizeof(struct xFile));
        } else {
            job_requests[job_count]->xf = NULL;
        }
        job_requests[job_count]->delOptions = packReq.delOptions;
        job_requests[job_count]->delOptions2 = packReq.delOptions2;
        job_requests[job_count]->maxNumProcessors = packReq.maxNumProcessors;
        job_requests[job_count]->userPriority = packReq.userPriority;

        /* Deep copy string fields */
        job_requests[job_count]->jobName = packReq.jobName ? strdup(packReq.jobName) : NULL;
        job_requests[job_count]->packFile = packReq.packFile ? strdup(packReq.packFile) : NULL;
        job_requests[job_count]->queue = packReq.queue ? strdup(packReq.queue) : NULL;
        job_requests[job_count]->resReq = packReq.resReq ? strdup(packReq.resReq) : NULL;
        job_requests[job_count]->hostSpec = packReq.hostSpec ? strdup(packReq.hostSpec) : NULL;
        job_requests[job_count]->dependCond = packReq.dependCond ? strdup(packReq.dependCond) : NULL;
        job_requests[job_count]->inFile = packReq.inFile ? strdup(packReq.inFile) : NULL;
        job_requests[job_count]->outFile = packReq.outFile ? strdup(packReq.outFile) : NULL;
        job_requests[job_count]->errFile = packReq.errFile ? strdup(packReq.errFile) : NULL;
        job_requests[job_count]->command = packReq.command ? strdup(packReq.command) : NULL;
        job_requests[job_count]->newCommand = packReq.newCommand ? strdup(packReq.newCommand) : NULL;
        job_requests[job_count]->chkpntDir = packReq.chkpntDir ? strdup(packReq.chkpntDir) : NULL;
        job_requests[job_count]->preExecCmd = packReq.preExecCmd ? strdup(packReq.preExecCmd) : NULL;
        job_requests[job_count]->postExecCmd = packReq.postExecCmd ? strdup(packReq.postExecCmd) : NULL;
        job_requests[job_count]->mailUser = packReq.mailUser ? strdup(packReq.mailUser) : NULL;
        job_requests[job_count]->projectName = packReq.projectName ? strdup(packReq.projectName) : NULL;
        job_requests[job_count]->loginShell = packReq.loginShell ? strdup(packReq.loginShell) : NULL;

        pack_outputs[packParsedNum-1]->packSubmitIndex = job_count;

        job_count++;
    }

    fclose(fp);

    submitPackRep.numJobs = 0;
    submitPackRep.numSuccess = 0;
    submitPackRep.numFailed = 0;

    if (job_count > 0) {
        memset(&submitPackRep, 0, sizeof(submitPackRep));

        /* Call library function to handle the submission */
        packSubmit = lsb_submit_pack(job_requests, job_count, &submitPackRep);
    }

    for (i = 0; i < packParsedNum; i++) {
        if (pack_outputs[i]) {
            if(pack_outputs[i]->packSubmitIndex >= 0){
                // submitted to mbatchd
                int index = pack_outputs[i]->packSubmitIndex;

                if (submitPackRep.submitReps[index].replyCode == LSBE_NO_ERROR) {
                    // mbatchd newJob successfully
                    fprintf(stdout, "Line#%d Job <%s> is submitted to queue <%s>.\n",
                            pack_outputs[i]->lineNum,
                            lsb_jobid2str(submitPackRep.submitReps[index].badJobId),
                            submitPackRep.submitReps[index].queue ? submitPackRep.submitReps[index].queue : "normal");
                } else if (submitPackRep.submitReps[index].replyCode != LSBE_NOT_HANDLED) {
                    // mbatchd handle this request but newJob failed
                    fprintf(stderr, "Line#%d ", pack_outputs[i]->lineNum);
                    lsberrno = submitPackRep.submitReps[index].replyCode;
                    prtErrMsg (job_requests[index], &submitPackRep.submitReps[index]);
                    fprintf(stderr,  ". %s.\n",
                            (_i18n_msg_get(ls_catd,NL_SETN,1561, "Job not submitted")));
                    mbdHandledError = TRUE;
                } else {
                    // mbatchd skipped this request due to LSB_PACK_SKIP_ERROR
                }
            } else if (pack_outputs[i]->outputMSG) {
                // bsub client check submit line failed
                if(mbdHandledError == TRUE && packSkipErrFlag == FALSE) {
                    // if mbdHandledError first, not need to display bsub check error behind that index
                    packParseError--;
                    continue;
                }
                fprintf(stderr, "%s", pack_outputs[i]->outputMSG);
            } else {
                // bsub client skipped line due to LSB_PACK_SKIP_ERROR
//                fprintf(stderr, "Line#%d Job not submitted.\n", pack_outputs[i]->lineNum);
            }
        }
    }

    if (packSubmit < 0) {
        fprintf(stderr, "Pack submission failed\n");
    } else {
        outputError = packParseError + submitPackRep.numFailed;
        outputTotal = outputError + submitPackRep.numSuccess;

        fprintf(stdout, "%d lines parsed, %d jobs submitted, %d errors found.\n",
                outputTotal, submitPackRep.numSuccess, outputError);
    }

    close(esubPrintFD);

    /* Cleanup memory */
    for (i = 0; i < job_count; i++) {
        if (job_requests[i]) {
            /* Free deep copied string fields */
            FREEUP(job_requests[i]->jobName);
            FREEUP(job_requests[i]->packFile);
            FREEUP(job_requests[i]->queue);
            FREEUP(job_requests[i]->resReq);
            FREEUP(job_requests[i]->hostSpec);
            FREEUP(job_requests[i]->dependCond);
            FREEUP(job_requests[i]->inFile);
            FREEUP(job_requests[i]->outFile);
            FREEUP(job_requests[i]->errFile);
            FREEUP(job_requests[i]->command);
            FREEUP(job_requests[i]->newCommand);
            FREEUP(job_requests[i]->chkpntDir);
            FREEUP(job_requests[i]->preExecCmd);
            FREEUP(job_requests[i]->postExecCmd);
            FREEUP(job_requests[i]->mailUser);
            FREEUP(job_requests[i]->projectName);
            FREEUP(job_requests[i]->loginShell);
            if (job_requests[i]->askedHosts) {
                for (j = 0; j < job_requests[i]->numAskedHosts; j++) {
                    FREEUP(job_requests[i]->askedHosts[j]);
                }
                FREEUP(job_requests[i]->askedHosts);
            }
            FREEUP(job_requests[i]->xf);
            FREEUP(job_requests[i]);
        }
    }

    FREEUP(job_requests);

    for (i = 0; i < packParsedNum; i++) {
        if (pack_outputs[i]) {
            FREEUP(pack_outputs[i]->outputMSG);
        }
        FREEUP(pack_outputs[i]);
    }
    FREEUP(pack_outputs);

    return (packSubmit < 0) ? -1 : job_count;
}

char **split_commandline(const char *cmdline, int *argc)
{
    int i;
    char **argv = NULL;
    assert(argc);

    if (!cmdline)
    {
        return NULL;
    }

    wordexp_t p;

    // Note! This expands shell variables.
    if (wordexp(cmdline, &p, 0))
    {
        return NULL;
    }

    *argc = p.we_wordc;

    if (!(argv = calloc((*argc) + 1, sizeof(char *))))
    {
        goto fail;
    }

    for (i = 0; i < p.we_wordc; i++)
    {
        if (!(argv[i] = strdup(p.we_wordv[i])))
        {
            goto fail;
        }
    }
    argv[i]=NULL;

    wordfree(&p);

    return argv;
fail:
    wordfree(&p);

    if (argv)
    {
        for (i = 0; i < *argc; i++)
        {
            if (argv[i])
            {
                free(argv[i]);
            }
        }

        free(argv);
    }

    return NULL;
}

void
prtBETime (struct submit req)
{
    static char fname[] = "prtBETime";
    char sp[60];

    if (logclass & (LC_TRACE | LC_EXEC | LC_SCHED))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);


    if (req.beginTime) {
        strcpy( sp, _i18n_ctime( ls_catd, CTIME_FORMAT_a_b_d_T_Y, &req.beginTime ));
        fprintf(stdout, "%s %s\n",
	    (_i18n_msg_get(ls_catd,NL_SETN,1556, "Job will be scheduled after")), sp); /* catgets  1556  */
    }
    if (req.termTime) {
        strcpy( sp, _i18n_ctime( ls_catd, CTIME_FORMAT_a_b_d_T_Y, &req.termTime ));
        fprintf(stdout, "%s %s\n",
	    (_i18n_msg_get(ls_catd,NL_SETN,1557, "Job will be terminated by")), sp); /* catgets  1557  */
    }
}

int
fillReq (int argc, char **argv, int operate, struct submit *req, int isInPackFile)
{
    static char fname[] = "fillReq";
    struct stat statBuf;
    char *template, **embedArgv;
    int  i, embedArgc = 0, redirect = 0;
    int myArgc;
    char *myArgv0;
    static char chkDir[128];


    memset(req, 0, sizeof(struct submit));

    if (logclass & (LC_TRACE | LC_EXEC | LC_SCHED))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);






    if (operate == CMD_BRESTART) {
	req->options = SUB_RESTART;
        template = "E:w:B|f|x|N|h|m:q:b:t:c:W:F:D:S:C:M:V|G:a:";
    } else if (operate == CMD_BMODIFY) {
        req->options = SUB_MODIFY;
        template = "h|V|O|Tn|T:xn|x|rn|r|Bn|B|Nn|N|En|E:a:"
	"wn|w:fn|f:kn|k:Rn|R:mn|m:Jn|J:isn|is:in|i:en|e:qn|q:bn|b:tn|t:spn|sp:sn|s:cn|c:Wn|W:Fn|F:Dn|D:Sn|S:Cn|C:Mn|M:on|o:nn|n:un|u:Pn|P:Ln|L:Xn|X:Zsn|Zs:Z:"
        ;
    } else if (operate == CMD_BSUB) {
	req->options = 0;
        template = "E:T:a:"
	"w:f:k:R:m:J:L:u:is:i:o:e:Zs|n:q:b:t:sp:s:c:v:p:W:F:D:S:C:M:O:P:Ip|Is|I|r|H|x|N|B|h|V|X:K|"
        ;
    }

    req->options2 = 0;
    commandline = "";
    emptyCmd = TRUE;

    myArgc = 0;
    myArgv0 = (char *) NULL;

    req->beginTime = 0;
    req->termTime  = 0;
    req->command = NULL;
    req->nxf = 0;
    req->numProcessors = 0;
    req->maxNumProcessors = 0;
    for (i = 0; i < LSF_RLIM_NLIMITS; i++)
	req->rLimits[i] = DEFAULT_RLIMIT;
    req->hostSpec = NULL;
    req->resReq = NULL;
    req->loginShell = NULL;
    req->delOptions = 0;
    req->delOptions2 = 0;
    req->userPriority = -1;

    if ((req->projectName = getenv("LSB_DEFAULTPROJECT")) != NULL)
        req->options |= SUB_PROJECT_NAME;


    if (operate == CMD_BMODIFY){
        int index;
        for(index=1; index<argc; index++){
            if (strcmp(argv[index],"-k") == 0){
                break;
            }
        }
        if ((index+1) < argc){
            char *pCurChar = argv[index+1];
            char *pCurWord = NULL;

            while(*(pCurChar) == ' '){
                pCurChar++;
            }
            pCurWord = strstr(pCurChar,"method=");
            if ((pCurWord != NULL) && (pCurWord != pCurChar)){
                if (isInPackFile == TRUE){
                    lsberrno = LSBE_CHANGE_BMOD_CKPT;
                    return(-1);
                }
                fprintf(stderr, "%s %s\n",
	                (_i18n_msg_get(ls_catd,NL_SETN,1580, "Checkpoint method cannot be changed with bmod:")),
                        argv[index+1]); /* catgets  1580  */
                return(-1);
            }
        }
    }

    if (operate == CMD_BSUB){
        char *pChkpntMethodDir = NULL;
        char *pChkpntMethod = NULL;
        char *pConfigPath = NULL;
        char *pIsOutput = NULL;


        struct config_param aParamList[] =
        {
        #define LSB_ECHKPNT_METHOD    0
            {"LSB_ECHKPNT_METHOD",NULL},
        #define LSB_ECHKPNT_METHOD_DIR    1
            {"LSB_ECHKPNT_METHOD_DIR",NULL},
        #define LSB_ECHKPNT_KEEP_OUTPUT    2
            {"LSB_ECHKPNT_KEEP_OUTPUT",NULL},
        #define LSB_MAX_PACK_JOBS    3
            {"LSB_MAX_PACK_JOBS",NULL},
        #define LSB_PACK_SKIP_ERROR    4
            {"LSB_PACK_SKIP_ERROR",NULL},
            {NULL, NULL}
        };


        pConfigPath = getenv("LSF_ENVDIR");
	if (pConfigPath == NULL){
            pConfigPath = "/etc";
	}


        ls_readconfenv(aParamList, pConfigPath);


        pChkpntMethod = getenv("LSB_ECHKPNT_METHOD");
        if ( pChkpntMethod == NULL ){

            if ( aParamList[LSB_ECHKPNT_METHOD].paramValue != NULL){
	        putEnv(aParamList[LSB_ECHKPNT_METHOD].paramName,
	               aParamList[LSB_ECHKPNT_METHOD].paramValue);
            }
            FREEUP(aParamList[LSB_ECHKPNT_METHOD].paramValue);
        }


        pChkpntMethodDir = getenv("LSB_ECHKPNT_METHOD_DIR");
        if ( pChkpntMethodDir == NULL ){
            if ( aParamList[LSB_ECHKPNT_METHOD_DIR].paramValue != NULL){
                putEnv(aParamList[LSB_ECHKPNT_METHOD_DIR].paramName,
                       aParamList[LSB_ECHKPNT_METHOD_DIR].paramValue);
	    }
	    FREEUP(aParamList[LSB_ECHKPNT_METHOD_DIR].paramValue);
        }


        pIsOutput = getenv("LSB_ECHKPNT_KEEP_OUTPUT");
        if ( pIsOutput == NULL ){
            if ( aParamList[LSB_ECHKPNT_KEEP_OUTPUT].paramValue != NULL){
                putEnv(aParamList[LSB_ECHKPNT_KEEP_OUTPUT].paramName,
                       aParamList[LSB_ECHKPNT_KEEP_OUTPUT].paramValue);
            }
	    FREEUP(aParamList[LSB_ECHKPNT_KEEP_OUTPUT].paramValue);
        }


        if ( aParamList[LSB_MAX_PACK_JOBS].paramValue != NULL){
            if (!isint_(aParamList[LSB_MAX_PACK_JOBS].paramValue)
                || (atoi(aParamList[LSB_MAX_PACK_JOBS].paramValue)) < 0) {
                lsbMaxPackJobs = DEFAULT_LSB_MAX_PACK_JOBS;
            } else {
                lsbMaxPackJobs = atoi(aParamList[LSB_MAX_PACK_JOBS].paramValue);
            }
        }
        FREEUP(aParamList[LSB_MAX_PACK_JOBS].paramValue);


        if ( aParamList[LSB_PACK_SKIP_ERROR].paramValue != NULL){
            if (strcmp(aParamList[LSB_PACK_SKIP_ERROR].paramValue,"Y")==0 ||
                strcmp(aParamList[LSB_PACK_SKIP_ERROR].paramValue,"y")==0 ) {
                packSkipErrFlag = TRUE;
            }
        }
        FREEUP(aParamList[LSB_PACK_SKIP_ERROR].paramValue);

    }

    if (setOption_ (argc, argv, template, req, ~0, ~0, NULL) == -1)
        return (-1);

    if (operate == CMD_BSUB && (req->options & SUB_INTERACTIVE)
            && (req->options & SUB_PTY)) {
        if (req->options & SUB_PTY_SHELL)
            putenv(putstr_("LSB_SHMODE=y"));
        else
            putenv(putstr_("LSB_USEPTY=y"));
    }
    if (fstat(0, &statBuf) == 0)

        if ((statBuf.st_mode & S_IFREG) == S_IFREG
             || (statBuf.st_mode & S_IFLNK) == S_IFLNK) {

            redirect = (ftell(stdin) == 0) ? 1 : 0;
        }

    if (operate == CMD_BRESTART || operate == CMD_BMODIFY) {

	if (argc == optind + 1)
            req->command = argv[optind];
        else if (argc == optind + 2) {

		LS_LONG_INT arrayJobId;
		if (getOneJobId (argv[optind+1], &arrayJobId, 0))
		    return(-1);

                   sprintf(chkDir, "%s/%s", argv[optind], lsb_jobidinstr(arrayJobId));

                req->command = chkDir;
            } else
	        subUsage_(req->options, NULL);

    } else {
        if (myArgc > 0 && myArgv0 != NULL) {
            emptyCmd = FALSE;
            argv[argc] = myArgv0;
            if (!CopyCommand(&argv[argc], myArgc))
                return (-1);
        }
        else if (argc >= optind + 1) {
            emptyCmd = FALSE;
            if (!CopyCommand(argv+optind, argc-optind-1))
                return (-1);
        } else {
            if (!(req->options & SUB_PACK) && isInPackFile == FALSE) {
                if (parseScript(stdin, &embedArgc, &embedArgv,
                                EMBED_INTERACT | EMBED_BSUB) == -1)
                    return (-1);
            }
        }
        req->command = commandline;
        SKIPSPACE(req->command);

        if ((req->options & SUB_PACK) && emptyCmd==FALSE){
            subUsage_(req->options, NULL);
            return (-1);
        }

        if (emptyCmd && !(req->options & SUB_PACK)) {
            if (isInPackFile == TRUE){
                lsberrno = LSBE_NO_CMD;
                return(-1);
            }
            if (redirect)
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1559, "No command is specified in the script file"))); /* catgets  1559  */
            else
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1560, "No command is specified"))); /* catgets  1560  */
            return (-1);
        }
    }

    if (embedArgc > 1 && operate == CMD_BSUB) {

        optind = 1;

        if (setOption_ (embedArgc, embedArgv, template, req,
			~req->options, ~req->options2, NULL) == -1)
            return (-1);

        if (req->options2 & SUB2_JOB_CMD_SPOOL) {
            if (isInPackFile == TRUE){
                lsberrno = LSBE_EMBED_ZS;
                return(-1);
            }
            fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1562,
		    "-Zs is not supported for embeded job command"))); /* catgets  1562  */
            return (-1);
        }
    }

    if (optionFlag) {
        if (parseOptFile_(optionFileName, req, NULL) == NULL)
	    return (-1);
	optionFlag = FALSE;
    }


    if (operate == CMD_BSUB) {
        if(addLabel2RsrcReq(req) != 0) {
            if (isInPackFile == TRUE){
                lsberrno = LSBE_MAC_LABEL_ERR;
                return(-1);
            }
            fprintf(stderr, I18N(1581,
                       "Set job mac label failed.")); /* catgets 1581 */
            return(-1);
        }
    }

    return 1;
}


static int
parseScript (FILE *from, int *embedArgc, char ***embedArgv, int option)
{
    static char fname[] = "parseScript";
    char *buf, line[MAXLINELEN*10], *prompt;
    register int ttyin = 0;
    int length = 0, size = 10*MAXLINELEN;
    int i, j, lineLen;
    char firstLine[MAXLINELEN*10];
    char *sp;
    int notBourne = FALSE;
    int isBSUB = FALSE;
    static char szTmpShellCommands[] = "\n%s\n) > $LSB_CHKFILENAME.shell\n"
			"chmod u+x $LSB_CHKFILENAME.shell\n"
			"$LSB_JOBFILENAME.shell\n"
			"saveExit=$?\n"
			"/bin/rm -f $LSB_JOBFILENAME.shell\n"
			"(exit $saveExit)\n";


    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if (option & EMBED_BSUB) {
        prompt = "bsub> ";
    } else if (option & EMBED_QSUB) {
        prompt = "qsub> ";
    }

    if (option & EMBED_INTERACT) {
        firstLine[0] = '\0';
        if ((buf = malloc(size)) == NULL) {
    	    fprintf(stderr, I18N_FUNC_FAIL,fname,"malloc" );
	    return (-1);
        }
        ttyin = isatty(fileno(from));
        if (ttyin){
            printf(prompt);
            fflush(stdout);
        }
    }

    sp = line;
    lineLen = 0;
    while (fgets (sp, 10 *MAXLINELEN - lineLen -1, from) != NULL) {
        lineLen = strlen(line);
	if (line[0] == '#') {
	    if (strstr(line, "BSUB") != NULL) {
	        isBSUB = TRUE;
            }
        }
	if ( isBSUB == TRUE ) {

            isBSUB = FALSE;
            if (line[lineLen-2] == '\\' && line[lineLen-1] == '\n') {
                lineLen -= 2;
                sp = line + lineLen;
                continue;
	    }
        }
        if (parseLine (line, embedArgc, embedArgv, option) == -1)
            return (-1);

        if (option & EMBED_INTERACT) {
            if (!firstLine[0])
            {

	        sprintf (firstLine, "( cat <<%s\n%s", SCRIPT_WORD, line);
	        strcpy(line, firstLine);
	        notBourne = TRUE;
            }
            lineLen = strlen(line);


            if (length + lineLen +MAXLINELEN+ 20 >= size) {
                size = size * 2;
                if ((buf = (char *) realloc(buf, size)) == NULL) {
                    free(buf);
                    fprintf(stderr, I18N_FUNC_FAIL,fname,"realloc" );
                    return (-1);
                }
            }
            for (i=length, j=0; j<lineLen; i++, j++)
                buf[i] = line[j];
            length += lineLen;


            if (ttyin) {
                printf(prompt);
                fflush(stdout);
            }
        }
        sp = line;
        lineLen = 0;
    }

    if (option & EMBED_INTERACT) {
        buf[length] = '\0';
        if (firstLine[0] != '\n' && firstLine[0] != '\0') {


            if (notBourne == TRUE) {

		if (length + strlen(szTmpShellCommands) + 1 >= size) {
		    size = size + strlen(szTmpShellCommands) + 1;
		    if (( buf = (char *) realloc(buf, size)) == NULL ) {
			free(buf);
			fprintf(stderr,I18N_FUNC_FAIL,fname,"realloc" );
			return(-1);
                    }
                }
                sprintf(&buf[length], szTmpShellCommands, SCRIPT_WORD_END);
            }
        }
        commandline = buf;
    }
    return (0);

}

static int
CopyCommand(char **from, int len)
{
    int i, size;
    char *arg, *temP, *endArg, *endArg1, endChar = '\0';
    char fname[]="CopyCommand";
    int oldParenthesis=0;

    if (lsbParams[LSB_32_PAREN_ESC].paramValue) {
        oldParenthesis = 1;
    }




    for (i = 0, size = 0; from[i]; i++) {
	size += strlen(from[i]) + 1 + 4;
    }

    size += 1 + 1;

    if ((commandline = (char *) malloc(size)) == NULL) {
	fprintf(stderr, I18N_FUNC_FAIL,fname,"malloc" );
	return (FALSE);
    }

    if (lsbParams[LSB_API_QUOTE_CMD].paramValue == NULL) {
        strcpy(commandline, from[0]);
	i = 1;
    } else {
	if ((strcasecmp(lsbParams[LSB_API_QUOTE_CMD].paramValue, "yes") == 0)
	    || ((strcasecmp(lsbParams[LSB_API_QUOTE_CMD].paramValue,
	    						"y") == 0))) {
	    memset(commandline, '\0', size);
	    i = 0;
	} else {
            strcpy(commandline, from[0]);
	    i = 1;
	}
    }

    for(; from[i] != NULL ;i++) {
	strcat(commandline, " ");

        if (strchr(from[i], ' ')) {
            if (strchr(from[i], '"')) {
                strcat(commandline, "'");
		strcat(commandline, from[i]);
                strcat(commandline, "'");
	    } else {
		if ( strchr(from[i], '$') ) {
		    strcat(commandline, "'");
		    strcat(commandline, from[i]);
		    strcat(commandline, "'");
                } else {
                    strcat(commandline, "\"");
		    strcat(commandline, from[i]);
                    strcat(commandline, "\"");
                }
            }
   	} else {
            arg = putstr_(from[i]);
            temP = arg;
            while (*arg) {
                endArg = strchr(arg, '"');
                endArg1 = strchr(arg, '\'');
                if (!endArg || (endArg1 && endArg > endArg1))
                    endArg = endArg1;

                endArg1 = strchr(arg, '\\');
                if (from[i][0] == '$') {
		    strcat(commandline, "'");
                }
                if (!endArg || (endArg1 && endArg > endArg1))
                    endArg = endArg1;


                endArg1 = strchr(arg, '(');
                if (!endArg || (endArg1 && endArg > endArg1))
                    endArg = endArg1;
                endArg1 = strchr(arg, ')');
                if (!endArg || (endArg1 && endArg > endArg1))
                    endArg = endArg1;



		if (endArg) {
                    endChar = *endArg;
                    *endArg = '\0';
                }
                strcat(commandline, arg);
		if (from[i][0] == '$') {
		    strcat(commandline, "'");
	        }
                if (endArg) {
                    arg += strlen(arg) + 1;
                    if (endChar == '\\')
                        strcat(commandline, "\\\\");
                    else if (endChar == '"')
                        strcat(commandline, "\\\"");
                    else if (endChar == '\'')
		        strcat(commandline, "\\\'");
                    else if (endChar == '(') {
			 if (oldParenthesis == 0)
                           strcat(commandline, "\\\(");
			 else strcat(commandline,"(");
                    }else if (endChar == ')') {
			 if (oldParenthesis == 0)
                           strcat(commandline, "\\)");
			else strcat(commandline,")");
		   }
                } else
                    arg += strlen(arg);
            }
            free(temP);
	}
    }

    return TRUE;
}


void
prtErrMsg (struct submit *req, struct submitReply *reply)
{
    static char tmpBuf[MAX_CMD_DESC_LEN];
    static char rNames [10][12] = {
                              "CPULIMIT",
                              "FILELIMIT",
                              "DATALIMIT",
                              "STACKLIMIT",
                              "CORELIMIT",
                              "MEMLIMIT",
                              "",
                              "",
                              "",
                              "RUNLIMIT"
                              };
    switch (lsberrno) {
    case LSBE_BAD_QUEUE:
    case LSBE_QUEUE_USE:
    case LSBE_QUEUE_CLOSED:
    case LSBE_EXCLUSIVE:
        if (req->options & SUB_QUEUE)
            sub_perror (req->queue);
        else
            sub_perror (reply->queue);
        break;

    case LSBE_DEPEND_SYNTAX:
	sub_perror (req->dependCond);
        break;

    case LSBE_NO_JOB:
    case LSBE_JGRP_NULL:
    case LSBE_ARRAY_NULL:
        if (reply->badJobId) {
	        char *idStr = putstr_(req->command);
	        sub_perror (idStr);
			FREEUP(idStr);
        } else {
	    if (strlen(reply->badJobName) == 0) {
		sub_perror (req->jobName);
	    }
	    else
                sub_perror (reply->badJobName);
        }
        break;
    case LSBE_QUEUE_HOST:
    case LSBE_BAD_HOST:
	sub_perror (req->askedHosts[reply->badReqIndx]);
        break;
    case LSBE_OVER_LIMIT:
        sub_perror (rNames[reply->badReqIndx]);
        break;

    case LSBE_BAD_HOST_SPEC:
        sub_perror (req->hostSpec);
        break;
    case LSBE_JOB_MAX_PEND:
    case LSBE_SLOTS_MAX_PEND:
        if (strlen(reply->pendLimitReason) == 0) {
            sprintf(tmpBuf, "User <%s>", getenv("USER"));
            sub_perror (tmpBuf);
        } else {
            sub_perror (reply->pendLimitReason);
        }
        break;
    default:
	sub_perror(NULL);
        break;
    }

    return;

}




static int
parseLine (char *line, int *embedArgc, char ***embedArgv, int option)
{
#define INCREASE 40

    static char **argBuf = NULL, *key;
    static int  bufSize = 0;
    char  fname[]="parseLine";

    char *sp, *sQuote, *dQuote, quoteMark;

    if (argBuf == NULL) {
        if ((argBuf = (char **) malloc(INCREASE * sizeof(char *)))
            == NULL) {
            fprintf(stderr, I18N_FUNC_FAIL,fname,"malloc" );
            return (-1);
        }
        bufSize = INCREASE;
        *embedArgc = 1;
        *embedArgv = argBuf;

        if (option & EMBED_BSUB) {
            argBuf[0] = "bsub";
            key = "BSUB";
        }
        else if (option & EMBED_RESTART) {
            argBuf[0] = "brestart";
            key = "BRESTART";
        }
        else if (option & EMBED_QSUB) {
            argBuf[0] = "qsub";
            key = "QSUB";
        }
        else {
            fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1568, "Invalid option"))); /* catgets  1568  */
            return (-1);
        }
        argBuf[1] = NULL;
    }


    SKIPSPACE(line);
    if (*line == '\0')
        return (0);
    if (*line != '#') {
        emptyCmd = FALSE;
        return (0);
    }


    ++line;
    SKIPSPACE(line);
    if (strncmp (line, key, strlen(key)) == 0) {
        line += strlen(key);
        SKIPSPACE(line);
        if (*line != '-') {
            return (0);
        }
        while (TRUE) {
            quoteMark = '"';
            if ((sQuote = strchr(line, '\'')) != NULL)
                if ((dQuote = strchr(line, '"')) == NULL || sQuote < dQuote)

                    quoteMark = '\'';

            if ((sp = getNextValueQ_(&line, quoteMark, quoteMark)) == NULL)
                return (0);

            if (*sp == '#')
                return (0);

            if (*embedArgc + 2 > bufSize) {
                bufSize += INCREASE;
                if ((argBuf = (char **) realloc(argBuf,
                                             bufSize * sizeof(char *)))
                    == NULL) {
                    fprintf(stderr, I18N_FUNC_FAIL,fname,"realloc" );
                    return (-1);
                }
		*embedArgv = argBuf;
            }
            argBuf[*embedArgc] = putstr_(sp);
            (*embedArgc)++;
            argBuf[*embedArgc] = NULL;
        }
    }
    return (0);

}

static int
addLabel2RsrcReq(struct submit *subreq)
{
    char * temp = NULL;
    char * job_label = NULL;
    char * req = NULL;
    char * select = NULL,
         * order = NULL,
         * rusage = NULL,
         * filter = NULL,
         * span = NULL,
         * same = NULL;
    char * and_symbol = " && ";
    int label_len, rsrcreq_len;

    if ((job_label = getenv("LSF_JOB_SECURITY_LABEL")) == NULL) {
        return 0;
    }

    SKIPSPACE(job_label);
    label_len = strlen(job_label);
    if (label_len == 0)
        return 0;

    if (subreq->resReq == NULL) {
        subreq->resReq = job_label;
        subreq->options |= SUB_RES_REQ;
        return 0;
    }

    rsrcreq_len = strlen(subreq->resReq);
    req = strdup(subreq->resReq);
    if ( req == NULL) {
        return -1;
    }


    select = strstr(req, "select[");
    order = strstr(req, "order[");
    rusage = strstr(req, "rusage[");
    filter = strstr(req, "filter[");
    span = strstr(req, "span[");
    same = strstr(req, "same[");

    if (select) {


        int size;
        char * select_start = strchr(select, '[') + 1;
        char * select_end = strchr(select, ']');
        char * rest;

        if(select_end == req + strlen(req) - 1) {
            rest = NULL;
        } else {
            rest = select_end + 1;
        }

        req[select - req] = '\0';
        req[select_end - req] = '\0';


        size = label_len + rsrcreq_len + strlen(and_symbol) + 10;
        temp = (char *)calloc(1, size);
        if (temp == NULL) {
            return(-1);
        }

        sprintf(temp, "%s%s(select[%s]) %s %s", job_label, and_symbol,
                      select_start, (*req)?req:"", rest?rest:"");

     } else if (   order != req
                && rusage != req
                && filter != req
                && span != req
                && same != req ) {



        int size;
        char *select_start = req;
        char *rest = req + strlen(req);

        if(order && order < rest) rest = order;
        if(rusage && rusage < rest) rest = rusage;
        if(filter && filter < rest) rest = filter;
        if(span && span < rest) rest = span;
        if(same && same < rest) rest = same;

        if(rest != (req + strlen(req))) {

            req[rest - req - 1] = '\0';
        }

        size = label_len + rsrcreq_len + strlen(and_symbol) + 4;
        temp = (char *)calloc(1, size);
        if (temp == NULL) {
            return(-1);
        }

        sprintf(temp, "%s%s(%s) %s", job_label, and_symbol,
                      select_start, (rest==req+strlen(req))?"":rest);

     } else {



        int size;
        size = label_len + rsrcreq_len + 2;
        temp = (char *)calloc(1, size);
        if (temp == NULL) {
            return(-1);
        }
        sprintf(temp, "%s %s", job_label, req);
     }

    subreq->resReq = temp;
    free(req);
    return(0);
}

/* Helper function to check if file is a text file */
int is_text_file(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return 0;
    
    unsigned char buffer[1024];
    size_t bytes = fread(buffer, 1, sizeof(buffer), fp);
    size_t i = 0;

    fclose(fp);
    
    if (bytes == 0) return 1;  /* Empty file is considered text */
    
    /* Check for binary content */
    int non_text = 0;
    for (i = 0; i < bytes; i++) {
        unsigned char c = buffer[i];
        /* Reject null bytes (common in binary files) */
        if (c == 0) {
            return 0;
        }
        /* Count non-printable characters (excluding common whitespace) */
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            non_text++;
        }
    }
    
    /* If more than 10% is non-text, consider it binary */
    return (non_text * 10 < bytes);
}


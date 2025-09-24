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

static int emptyCmd = TRUE;

static int parseScript (FILE *from,  int *embedArgc,
			char ***embedArgv, int option);
static int CopyCommand(char **, int);

static int
addLabel2RsrcReq(struct submit *subreq);

void sub_perror (char *);
void do_pack_sub (int option, char **argv, struct submit *req);
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
        /* 使用增强的批量提交功能 */
        jobId = do_batch_pack_sub(option, argv, &req);
        if (jobId < 0) {
            fprintf(stderr, ". %s.\n",
                    (_i18n_msg_get(ls_catd,NL_SETN,1551, "Batch job not submitted")));
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

/**
 * @brief 增强的批量作业提交函数
 * 使用新的批量提交架构
 */
LS_LONG_INT do_batch_pack_sub(int option, char **argv, struct submit *req)
{
    static char fname[] = "do_batch_pack_sub";
    batch_job_collection_t collection;
    batch_transmitter_t *transmitter = NULL;
    job_collector_config_t collector_config;
    char *pack_file_path = NULL;
    LS_LONG_INT result = -1;
    int successful_jobs = 0;

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC)) {
        ls_syslog(LOG_DEBUG, "%s: 开始批量作业提交", fname);
    }

    /* 获取pack文件路径 */
    if (req->packFile) {
        pack_file_path = req->packFile;
    } else {
        fprintf(stderr, "❌ 未指定pack文件路径\n");
        return -1;
    }

    fprintf(stderr, "🚀 开始批量作业提交\n");
    fprintf(stderr, "📁 Pack文件: %s\n", pack_file_path);

    /* 创建默认收集器配置 */
    if (job_collector_create_default_config(&collector_config) != 0) {
        fprintf(stderr, "❌ 创建收集器配置失败\n");
        return -1;
    }

    /* 根据环境变量调整配置 */
    char *max_jobs_env = getenv("LSB_BATCH_MAX_JOBS");
    if (max_jobs_env) {
        collector_config.max_jobs_count = atoi(max_jobs_env);
        if (collector_config.max_jobs_count <= 0) {
            collector_config.max_jobs_count = 10000;
        }
    }

    char *skip_error_env = getenv("LSB_PACK_SKIP_ERROR");
    if (skip_error_env && (strcmp(skip_error_env, "Y") == 0 || strcmp(skip_error_env, "y") == 0)) {
        collector_config.fail_on_first_error = 0;
    } else {
        collector_config.fail_on_first_error = 1;
    }

    /* 收集作业 */
    fprintf(stderr, "📦 收集作业信息...\n");
    if (job_collector_collect_from_pack_file(pack_file_path, &collection, &collector_config) != 0) {
        fprintf(stderr, "❌ 作业收集失败\n");
        goto cleanup;
    }

    if (collection.total_jobs_count <= 0) {
        fprintf(stderr, "❌ 没有找到有效的作业\n");
        goto cleanup;
    }

    fprintf(stderr, "✅ 成功收集 %d 个作业\n", collection.total_jobs_count);

    /* 使用真正的批量传输器进行一次性连接提交 */
    fprintf(stderr, "🚀 开始批量提交（一次连接）...\n");

    /* 创建批量传输器 */
    transmitter = batch_transmitter_create();
    if (!transmitter) {
        fprintf(stderr, "❌ 创建批量传输器失败\n");
        goto cleanup;
    }

    /* 配置传输器 */
    batch_transmitter_configure(transmitter, 1, 3, 30);  /* 启用流式响应 */

    /* 执行批量提交 - 一次连接，流式响应 */
    int submit_result = batch_transmitter_submit_jobs(transmitter, &collection);
    if (submit_result < 0) {
        fprintf(stderr, "❌ 批量提交失败\n");
        goto cleanup;
    }

    successful_jobs = submit_result;
    result = successful_jobs;

cleanup:
    /* 清理批量传输器 */
    if (transmitter) {
        batch_transmitter_destroy(transmitter);
        transmitter = NULL;
    }

    /* 清理作业集合 */
    batch_job_collection_cleanup(&collection);

    if (logclass & (LC_TRACE | LC_SCHED | LC_EXEC)) {
        ls_syslog(LOG_DEBUG, "%s: 批量作业提交完成，结果: %lld", fname, result);
    }

    return result;
}



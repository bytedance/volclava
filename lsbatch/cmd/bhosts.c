/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: bhosts.c 397 2007-11-26 19:04:00Z mblack $
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

#include <netdb.h>
#include "../../lsf/intlib/intlibout.h"

#define HOST_NAME_LENGTH    18
#define HOST_STATUS_LENGTH  14
#define HOST_STATUS_SHORT   11
#define HOST_JL_U_LENGTH    5
#define HOST_MAX_LENGTH     6
#define HOST_NJOBS_LENGTH   6
#define HOST_RUN_LENGTH     6
#define HOST_SSUSP_LENGTH   6
#define HOST_USUSP_LENGTH   6
#define HOST_RSV_LENGTH     6
#define HOST_CPUF_LENGTH    6

extern int _lsb_recvtimeout;

#define ALL_HOSTS "ALLHOSTS"


extern int lsbSharedResConfigured_;


#define NL_SETN 8



struct indexFmt {
    char *name;
    char *hdr;
    char *busy;
    char *ok;
    float scale;
    int  dispLen;
    char *normFmt;
    char *expFmt;
};


struct indexFmt fmt1[] = {
{ "r15s", "%6s", "*%4.1", "%5.1", 1.0 ,  6, "f",   "g"},
{ "r1m",  "%6s", "*%4.1", "%5.1", 1.0,   6, "f",   "g" },
{ "r15m", "%6s", "*%4.1", "%5.1", 1.0,   6, "f",   "g" },
{ "ut",   "%6s", "*%4.0", "%6.0", 100.0, 6, "f%%", "g%%"},
{ "pg",   "%6s", "*%4.1", "%5.1", 1.0,   6, "f",   "g"},
{ "io",   "%6s", "*%4.0", "%4.0", 1.0,   4, "f",   "g"},
{ "ls",   "%5s", "*%2.0", "%3.0", 1.0,   3, "f",   "g"},
{ "it",   "%6s", "*%4.0", "%5.0", 1.0,   5, "f",   "g"},
{ "tmp",  "%6s", "*%3.0", "%4.0", 1.0,   5, "fM",  "fG"},
{ "swp",  "%6s", "*%3.0", "%4.0", 1.0,   5, "fM",  "fG"},
{ "mem",  "%6s", "*%4.0", "%5.0", 1.0,   5, "fM",  "fG"},
#define  DEFAULT_FMT 11
{ "dflt", "%7s", "*%6.1", "%6.1", 1.0,   7, "f",   "g" },
{  NULL,  "%7s", "*%6.1", "%6.1", 1.0,   7, "f",   "g" }
 }, *fmt;

char *defaultindex[]={"r15s", "r1m", "r15m", "ut", "pg", "ls",
				   "it", "tmp", "swp", "mem", NULL};
struct lsInfo *lsInfoPtr = NULL;

#define MAXFIELDSIZE 80

static int makeFields(struct hostInfoEnt *, char *loadval[], char **, int);
static char *formatHeader(char **, int, int);
static char * stripSpaces(char *field);
static float getLoad(char *, float *, int *);
static char ** formLINamesList ( struct lsInfo *);

static void prtHostsLong (int, struct hostInfoEnt *);
static void prtHostsShort(int, struct hostInfoEnt *);
static void prtLoad(struct hostInfoEnt *, struct lsInfo *);
static void sort_host (int, struct hostInfoEnt *);
static int repeatHost (int, struct hostInfoEnt *);
static void getCloseString(int, char **);
static void displayShareRes(int, char **, int);
static void prtResourcesShort(int, struct lsbSharedResourceInfo  *);
static void prtOneInstance(char *, struct lsbSharedResourceInstance  *);
static int makeShareFields(char *, struct lsInfo *, char ***, char ***,
                           char ***, char ***);
static int getDispLastItem(char**, int, int);

static char wflag = FALSE;
static char fomt[200];
static int nameToFmt( char *indx);
static void  insertSlotsToResults(struct hostInfoEnt  *hPtr,
                                  char **loadvalTotal,
                                  char **loadvalRsv,
                                  char **nlp,
                                  int  *last,
                                  struct lsInfo *lsInfo);

void
usage (char *cmd)
{
    fprintf(stderr, I18N_Usage);
    fprintf(stderr, ":\n%s [-h] [-V] [-R res_req] [-w | -l] [host_name ... | cluster_name]\n", cmd);
    fprintf(stderr, I18N_or );
    fprintf(stderr, "\n%s [-h] [-V] -s [ resource_name ] \n", cmd);
    exit(-1);
}

int
main(int argc, char **argv)
{
    int i, cc, local = FALSE;
    struct hostInfoEnt *hInfo;
    char **hosts=NULL, **hostPoint, *resReq = NULL;
    char lflag = FALSE, sOption = FALSE, otherOption = FALSE;
    int numHosts;

    _lsb_recvtimeout = 30;
    _i18n_init ( I18N_CAT_MIN );

    if (lsb_init(argv[0]) < 0) {
	lsb_perror("lsb_init");
        _i18n_end ( ls_catd );
	exit(-1);
    }

    for  (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            _i18n_end ( ls_catd );
            exit (0);
        } else if (strcmp(argv[i], "-V") == 0) {
            fputs(_LS_VERSION_, stdout);
            _i18n_end ( ls_catd );
            exit(0);
        } else if (strcmp(argv[i], "-s") == 0) {
            if (otherOption == TRUE) {
                usage(argv[0]);
                _i18n_end ( ls_catd );
                exit(-1);
            }
            sOption = TRUE;
            optind = i + 1;
        } else if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "-l") == 0
                   || strcmp(argv[i], "-w") == 0) {
            otherOption = TRUE;
            if (sOption == TRUE) {
                usage(argv[0]);
                _i18n_end ( ls_catd );
                exit(-1);
            }
        }
    }

    if (sOption) {
        displayShareRes(argc, argv, optind);
        return (0);
    }
    while ((cc = getopt(argc, argv, "lwR:")) != EOF) {
        switch (cc) {
	case 'l':
	    lflag = TRUE;
            if (wflag)
                usage(argv[0]);
	    break;
	case 'w':
	    wflag = TRUE;
            if (lflag)
                usage(argv[0]);
	    break;
	case 'R':
	    resReq = optarg;
	    break;
        default:
            usage(argv[0]);
        }
    }
    numHosts = getNames (argc, argv, optind, &hosts, &local, "host");
    if ((local && numHosts == 1) || !numHosts)
        hostPoint = NULL;
    else
        hostPoint = hosts;
    TIMEIT(0, (hInfo = lsb_hostinfo_ex(hostPoint, &numHosts, resReq, 0)), "lsb_hostinfo");
    if (!hInfo) {
        if (lsberrno == LSBE_BAD_HOST && hostPoint)
            lsb_perror (hosts[numHosts]);
        else
            lsb_perror (NULL);
        exit (-1);
    }

    if (numHosts > 1 && resReq == NULL)
	sort_host (numHosts, hInfo);

    if ( lflag )
        prtHostsLong(numHosts, hInfo);
    else
        prtHostsShort(numHosts, hInfo);

    _i18n_end ( ls_catd );
    exit(0);

}

static void
prtHostsLong (int numReply, struct hostInfoEnt  *hInfo)
{
    struct hostInfoEnt *hPtr;
    int i, j, retVal = 0;
    char *status, maxJobs[MAX_CHARLEN], userJobLimit[MAX_CHARLEN];
    char **nameTable, **totalValues, **rsvValues, **formats;
    struct lsInfo *lsInfo;

    if ((lsInfo = ls_info()) == NULL) {
	ls_perror("ls_info");
	exit(-1);
    }

    lsInfoPtr = lsInfo;

    for (i = 0; i < numReply; i++) {
	hPtr = &(hInfo[i]);
        if (repeatHost (i, hInfo))
            continue;
	printf("%s  %s\n",
		(_i18n_msg_get(ls_catd,NL_SETN,1604, "HOST")), /* catgets  1604  */
		hPtr->host);


        prtWord(HOST_STATUS_LENGTH, I18N_STATUS, 0);

	if (lsbMode_ & LSB_MODE_BATCH) {
            prtWord(HOST_CPUF_LENGTH, I18N_CPUF, -1);
            prtWord(HOST_JL_U_LENGTH, I18N_JL_U, -1);
	};

            prtWord(HOST_MAX_LENGTH,   I18N_MAX, -1);
            prtWord(HOST_NJOBS_LENGTH, I18N_NJOBS, -1);
            prtWord(HOST_RUN_LENGTH,   I18N_RUN, -1);
            prtWord(HOST_SSUSP_LENGTH, I18N_SSUSP, -1);
            prtWord(HOST_USUSP_LENGTH, I18N_USUSP, -1);
            prtWord(HOST_RSV_LENGTH,   I18N_RSV, -1);

	if (lsbMode_ & LSB_MODE_BATCH)
	    printf(I18N(1608, "DISPATCH_WINDOW\n")); /* catgets  1608  */
	else
	    printf("\n");

	status = I18N_ok;
	if (hPtr->hStatus & HOST_STAT_UNAVAIL)
	    status = I18N_unavail;
	else if (hPtr->hStatus & HOST_STAT_UNREACH)
	    status = (_i18n_msg_get(ls_catd,NL_SETN,1611, "unreach")); /* catgets  1611  */

        else if (hPtr->hStatus & (HOST_STAT_BUSY
                                             | HOST_STAT_WIND
                                             | HOST_STAT_DISABLED
                                             | HOST_STAT_EXCLUSIVE
                                             | HOST_STAT_LOCKED
                                             | HOST_STAT_LOCKED_MASTER
                                             | HOST_STAT_FULL
                                             | HOST_STAT_NO_LIM))
            getCloseString (hPtr->hStatus, &status);

	if (hPtr->userJobLimit < INFINIT_INT)
            strcpy(userJobLimit,
                   prtValue(HOST_JL_U_LENGTH, hPtr->userJobLimit));
        else
            strcpy(userJobLimit,  prtDash(HOST_JL_U_LENGTH));

        if ( hPtr->maxJobs < INFINIT_INT )
            strcpy(maxJobs,
                   prtValue(HOST_MAX_LENGTH, hPtr->maxJobs));
        else
            strcpy(maxJobs,  prtDash(HOST_MAX_LENGTH));

        prtWordL(HOST_STATUS_LENGTH, status);

	if (lsbMode_ & LSB_MODE_BATCH) {
            sprintf(fomt, "%%%d.2f %%s", HOST_CPUF_LENGTH );
	    printf(fomt, hPtr->cpuFactor, userJobLimit);
	};


            sprintf(fomt, "%%s%%%dd %%%dd %%%dd %%%dd %%%dd ",
                                  HOST_NJOBS_LENGTH,
                                  HOST_RUN_LENGTH,
                                  HOST_SSUSP_LENGTH,
                                  HOST_USUSP_LENGTH,
                                  HOST_RSV_LENGTH);

            printf(fomt,
                   maxJobs, hPtr->numJobs, hPtr->numRUN,
		   hPtr->numSSUSP, hPtr->numUSUSP, hPtr->numRESERVE);

	if (lsbMode_ & LSB_MODE_BATCH)
	    printf("%s\n\n",
		   hPtr->windows[0] != '\0' ? hPtr->windows : "     -");
	else
	    printf("\n\n");

	if (!(hPtr->hStatus & (HOST_STAT_UNAVAIL | HOST_STAT_UNREACH))){
	    printf(" %s:\n",
		_i18n_msg_get(ls_catd,NL_SETN,1612, "CURRENT LOAD USED FOR SCHEDULING")); /* catgets  1612  */
	    prtLoad(hPtr, lsInfo);

	    if (lsbSharedResConfigured_) {

		retVal = makeShareFields(hPtr->host, lsInfo, &nameTable,
					 &totalValues, &rsvValues,
					 &formats);
		if (retVal > 0) {
		    int start =0;
		    int end;
                    while ((end = getDispLastItem(nameTable, start, retVal)) > start
	                   && start < retVal) {
		        printf(" %11s", " ");
		        for (j = start; j < end; j++)
                            printf(formats[j], nameTable[j]);

		        printf("\n %-11.11s",
                               _i18n_msg_get(ls_catd,NL_SETN,1613, "Total"));   /* catgets  1613  */
	                for (j = start; j < end; j++)
                            printf(formats[j], totalValues[j]);

		        printf("\n %-11.11s",
                               _i18n_msg_get(ls_catd,NL_SETN,1614, "Reserved"));  /* catgets 1614 */
		        for (j = start; j < end; j++)
                            printf(formats[j], rsvValues[j]);
		        putchar('\n');
		        putchar('\n');
			start = end;
                    }
		    putchar('\n');
		}
	    }
	}
	printf(" %s:\n",
	    _i18n_msg_get(ls_catd,NL_SETN,1615, "LOAD THRESHOLD USED FOR SCHEDULING"));	 /* catgets  1615  */
	if (printThresholds(hPtr->loadSched, hPtr->loadStop,
			    hPtr->busySched, hPtr->busyStop,
			    MIN(hInfo->nIdx, lsInfo->numIndx),
			    lsInfo) < 0)
	    continue;
	printf("\n");


	if (hPtr->mig < INFINIT_INT) {
	    printf(( _i18n_msg_get(ls_catd, NL_SETN, 1616, "Migration threshold is %d min \n")), hPtr->mig);  /* catgets  1616  */
            printf("\n");
        }

        if (hPtr->actionComment && hPtr->actionComment[0] != '\0') {
            printf(( _i18n_msg_get(ls_catd, NL_SETN, 1617, "ADMIN ACTION COMMENT:  %s\n")), hPtr->actionComment);  /* catgets  1617  */
            printf("\n");
        }

        printf("\n");
    }
}

static void
prtHostsShort (int numReply, struct hostInfoEnt  *hInfo)
{
    struct hostInfoEnt *hPtr;
    int i;
    char *status, maxJobs[MAX_CHARLEN], userJobLimit[MAX_CHARLEN];
    char first = TRUE;

    for (i = 0; i < numReply; i++) {
        hPtr = &(hInfo[i]);
        if ( first ) {
            first = FALSE;
            prtWord(HOST_NAME_LENGTH,
		_i18n_msg_get(ls_catd, NL_SETN, 1618, "HOST_NAME"), 0); /* catgets  1618  */
	    if ( wflag )
                prtWord(HOST_STATUS_LENGTH, I18N_STATUS, 0);
            else
                prtWord(HOST_STATUS_SHORT, I18N_STATUS, 0);

		if (lsbMode_ & LSB_MODE_BATCH)
                    prtWord(HOST_JL_U_LENGTH, I18N_JL_U, -1);

                prtWord(HOST_MAX_LENGTH,   I18N_MAX, -1);
                prtWord(HOST_NJOBS_LENGTH, I18N_NJOBS, -1);
                prtWord(HOST_RUN_LENGTH,   I18N_RUN, -1);
                prtWord(HOST_SSUSP_LENGTH, I18N_SSUSP, -1);
                prtWord(HOST_USUSP_LENGTH, I18N_USUSP, -1);
                prtWord(HOST_RSV_LENGTH,   I18N_RSV, -1);
                printf("\n");
        };

        if ( repeatHost (i, hInfo) )
            continue;

        if ( wflag )
            prtWordL(HOST_NAME_LENGTH, hPtr->host);
        else
            prtWord(HOST_NAME_LENGTH, hPtr->host, 0);

        status = I18N_ok;
        if (hPtr->hStatus & HOST_STAT_UNAVAIL)
            status = I18N_unavail;
        else if (hPtr->hStatus & HOST_STAT_UNREACH)
            status = (_i18n_msg_get(ls_catd,NL_SETN,1626, "unreach")); /* catgets  1626  */
        else if (hPtr->hStatus & (HOST_STAT_BUSY
                                             | HOST_STAT_WIND
                                             | HOST_STAT_DISABLED
                                             | HOST_STAT_EXCLUSIVE
                                             | HOST_STAT_LOCKED
                                             | HOST_STAT_LOCKED_MASTER
                                             | HOST_STAT_FULL
                                             | HOST_STAT_NO_LIM)) {
            if ( !wflag )
                status = (_i18n_msg_get(ls_catd,NL_SETN,1627, "closed")); /* catgets  1627  */
            else
		getCloseString (hPtr->hStatus, &status);
        };

        if ( hPtr->userJobLimit < INFINIT_INT )
            strcpy(userJobLimit,
                   prtValue(HOST_JL_U_LENGTH, hPtr->userJobLimit));
        else
            strcpy(userJobLimit,  prtDash(HOST_JL_U_LENGTH));

        if ( hPtr->maxJobs < INFINIT_INT )
            strcpy(maxJobs,
                   prtValue(HOST_MAX_LENGTH, hPtr->maxJobs));
        else
            strcpy(maxJobs,  prtDash(HOST_MAX_LENGTH));

	if ( wflag )
            prtWordL(HOST_STATUS_LENGTH, status);
	else
            prtWordL(HOST_STATUS_SHORT, status);

	    if ( lsbMode_ & LSB_MODE_BATCH )
		printf("%s", userJobLimit);

            sprintf(fomt, "%%s%%%dd %%%dd %%%dd %%%dd %%%dd\n",
                                  HOST_NJOBS_LENGTH,
                                  HOST_RUN_LENGTH,
                                  HOST_SSUSP_LENGTH,
                                  HOST_USUSP_LENGTH,
                                  HOST_RSV_LENGTH);

            printf(fomt,
                   maxJobs,
		   hPtr->numJobs,
		   hPtr->numRUN, hPtr->numSSUSP, hPtr->numUSUSP,
		   hPtr->numRESERVE);

    }

}

static void
sort_host (int replyNumHosts, struct hostInfoEnt *hInfo)
{
    int i,j,k;
    struct hostInfoEnt temHost;

    for (k = replyNumHosts / 2; k > 0; k /= 2) {
        for (i = k; i < replyNumHosts; i++) {
            for (j = i-k; j >= 0; j -= k) {
                 if (strcmp (hInfo[j].host, hInfo[j+k].host) < 0)
                     break;
                 memcpy(&temHost, &hInfo[j], sizeof (struct hostInfoEnt));
                 memcpy(&hInfo[j], &hInfo[j+k], sizeof (struct hostInfoEnt));
                 memcpy(&hInfo[j+k], &temHost, sizeof (struct hostInfoEnt));
            }
        }
    }

    return;
}

static int
repeatHost (int currentNum, struct hostInfoEnt *hInfo)
{
    int i;

    for (i = 0; i < currentNum; i++) {
        if (strcmp (hInfo[i].host, hInfo[currentNum].host) != 0)
            continue;
        return TRUE;
    }
    return FALSE;

}

static int
getDispLastItem(char** dispIndex, int start, int last)
{
    int endItem = start;
    int dispWidth = 11;
    int fieldWidth;
    if (dispIndex != NULL) {
        while (endItem < last) {
	    if (dispIndex[endItem] == NULL) {
		endItem++;
		continue;
            }
            fieldWidth = strlen(dispIndex[endItem]);
	    if (fieldWidth < 7) {
		int fmtId;
		fmtId = nameToFmt(dispIndex[endItem]);
                fieldWidth = fmt[fmtId].dispLen;
	    }
	    else
	      fieldWidth++;
            dispWidth += fieldWidth;
	    if (dispWidth >= 80)
	        break;
            else
                endItem++;
        }
    }
    return endItem;
}


static void
prtLoad (struct hostInfoEnt  *hPtrs, struct lsInfo *lsInfo)
{

    static char fname[] = "prtLoad";
    char **nlp = NULL;
    int i;
    char **loadval;
    char **loadval1;
    int start = 0;
    int end;
    int last;
    int curLoadNum = lsInfo->numIndx + 1; /*number of load + 'slots'*/

    if (!fmt) {
        if(!(fmt=(struct indexFmt *)
            malloc((curLoadNum + 2)*sizeof (struct indexFmt)))) {
            lsberrno=LSBE_NO_MEM;
            lsb_perror("print_long");
            exit(-1);
        }
        for (i=0; i<NBUILTINDEX+2; i++)
            fmt[i]=fmt1[i];
    }
    if ((nlp = formLINamesList (lsInfo)) == NULL) {
        fprintf (stderr, "%s\n",
            _i18n_msg_get(ls_catd,NL_SETN,1629, "Bad load index name specified")); /* catgets  1629  */
        exit (-1);
    }

    if ((loadval=(char **) malloc((curLoadNum + 1) * sizeof(char *)))
							      == NULL) {
	lserrno=LSE_MALLOC;
	lsb_perror(fname);
	exit(-1);
    }
    if ((loadval1=(char **) malloc((curLoadNum + 1) * sizeof(char *)))
							      == NULL) {
	lserrno=LSE_MALLOC;
	lsb_perror(fname);
	exit(-1);
    }

    last = hPtrs->nIdx;

    for (i=0; i < last; i++) {
	loadval[i] = malloc(MAXFIELDSIZE);
	loadval1[i] = malloc(MAXFIELDSIZE);
	if (loadval[i] == NULL || loadval[i] == NULL) {
	    lserrno=LSE_MALLOC;
	    lsb_perror(fname);
	    exit(-1);
	}
    }

    makeFields(hPtrs, loadval, nlp, TRUE);
    makeFields(hPtrs, loadval1, nlp, FALSE);

    insertSlotsToResults(hPtrs, loadval, loadval1, nlp, &last, lsInfo);

    while ((end = getDispLastItem(nlp, start, last)) > start
	   && start < last) {
	printf(" %11s", " ");
        printf("%s\n", formatHeader(nlp, start, end));

        printf( " %-11.11s",
            _i18n_msg_get(ls_catd,NL_SETN,1630, "Total"));  /* catgets  1630  */
        for (i=start; i < end; i++)
            printf("%s", loadval[i]);
        putchar('\n');

        printf( " %-11.11s",
            _i18n_msg_get(ls_catd,NL_SETN,1631, "Reserved")); /* catgets  1631  */
        for (i=start; i < end; i++)
            printf("%s", loadval1[i]);
        putchar('\n');
        putchar('\n');
	start = end;
    }
    putchar('\n');

    for (i=0; i < last; i++) {
	FREEUP(loadval[i]);
	FREEUP(loadval1[i]);
    }
    FREEUP(loadval);
    FREEUP(loadval1);

}

static int
nameToFmt( char *indx)
{
    int i=0;

    if (strcmp(indx,"swap") == 0)
        indx = "swp";
    if (strcmp(indx, "login") == 0)
        indx = "ls";
    if (strcmp(indx, "idle") == 0)
        indx = "it";
    if (strcmp(indx, "cpu") == 0)
        indx = "r1m";

    for (i=0; fmt[i].name; i++) {
        if (strcmp(indx, fmt[i].name) == 0)
            return i;
    }
    return (i-1);
}

static char *
formatHeader(char **dispindex, int start, int end)
{
#define HEADERLEN  132
    int i, fmtid;
    static int maxMem  = HEADERLEN;
    char tmpbuf[MAXLSFNAMELEN];
    static char *line = NULL;
    static int first = TRUE;
    static char fName[] = "formatHeader";


    if (first) {
        if ((line = (char *)malloc(HEADERLEN)) == NULL) {
            fprintf(stderr,I18N_FUNC_FAIL, fName, "malloc");
            exit (-1);
        }
        first = FALSE;
	line[0] = '\0';
    }
    *line = '\0';
    for(i=start; dispindex[i] && i<end; i++) {
        fmtid = nameToFmt(dispindex[i]);

	if (fmtid == DEFAULT_FMT) {
	    if ((maxMem - strlen(line)) < MAXLSFNAMELEN) {
		maxMem = 2 * maxMem;
	        if ((line = (char *)realloc(line, maxMem)) == NULL) {
	            fprintf(stderr,I18N_FUNC_FAIL, fName, "realloc");
	            exit(-1);
	        }
	    }
	    if (strlen(dispindex[i]) >= 7)
	        sprintf(tmpbuf, " %s", dispindex[i]);
	    else
	        sprintf(tmpbuf, fmt[fmtid].hdr, dispindex[i]);
	}
	else
	    sprintf(tmpbuf, fmt[fmtid].hdr, dispindex[i]);

        strcat(line, tmpbuf);
    }
    return(line);
}

static char *
stripSpaces(char *field)
{
    char *cp, *sp;
    int len, i;

    cp = field;
    while (*cp == ' ')
        cp++;


    if (*cp == '*') {
        sp = cp;
	for (sp=sp+1; *sp==' '; sp++) {
	    *(sp-1) = ' ';
	    *sp = '*';
	}
    }


    len = strlen(field);
    i = len - 1;
    while((i > 0) && (field[i] == ' '))
        i--;
    if (i < len-1)
        field[i+1] = '\0';
    return(cp);
}

static int
makeFields(struct hostInfoEnt *host,
           char *loadval[], char **dispindex, int option)
{
    int j, id, nf, index;
    char *sp;
    char tmpfield[MAXFIELDSIZE];
    char fmtField[MAXFIELDSIZE];
    char firstFmt[MAXFIELDSIZE];
    float real, avail, load;


    nf = 0;
    for(j=0; dispindex[j] && j < host->nIdx; j++, nf++) {
	int newIndexLen;

        id = nameToFmt(dispindex[j]);
        if (id == DEFAULT_FMT)
            newIndexLen = strlen(dispindex[j]);

	real  = getLoad(dispindex[j], host->realLoad, &index);
	avail = getLoad(dispindex[j], host->load, &index);
	if (option == TRUE)
	    load = avail;
        else {
	    real  = getLoad(dispindex[j], host->realLoad, &index);
	    load = (avail >= real)? (avail - real):(real - avail);
        }
        if (load >= INFINIT_LOAD)
            sp = "- ";
        else {
            if (option == TRUE && (host->hStatus & HOST_STAT_BUSY)
	          && (LSB_ISBUSYON (host->busySched, index)
		      || LSB_ISBUSYON (host->busyStop, index))) {
                strcpy(firstFmt, fmt[id].busy);
                sprintf(fmtField, "%s%s",firstFmt, fmt[id].normFmt);
                sprintf(tmpfield, fmtField, load * fmt[id].scale);
            } else {
                strcpy(firstFmt, fmt[id].ok);
                sprintf(fmtField, "%s%s", firstFmt, fmt[id].normFmt);
                sprintf(tmpfield, fmtField, load * fmt[id].scale);
            }
            sp = stripSpaces(tmpfield);

            if (strlen(sp) > fmt[id].dispLen) {
                if (load > 1024)
                    sprintf(fmtField, "%s%s", firstFmt, fmt[id].expFmt);
                else
                    sprintf(fmtField, "%s%s", firstFmt, fmt[id].normFmt);
                if ((load > 1024) &&
                    ((!strcmp(fmt[id].name,"mem")) ||
                    (!strcmp(fmt[id].name,"tmp")) ||
                    (!strcmp(fmt[id].name,"swp"))))
                    sprintf(tmpfield,fmtField,(load*fmt[id].scale)/1024);
                else
                    sprintf(tmpfield,fmtField, (load * fmt[id].scale));
            }
            sp = stripSpaces(tmpfield);
        }
        if (id == DEFAULT_FMT && newIndexLen >= 7){
	    char newFmt[10];
	    sprintf(newFmt, " %s%d%s", "%", newIndexLen, "s");
	    sprintf(loadval[j], newFmt, sp);
	}
	else
	    sprintf(loadval[j], fmt[id].hdr, sp);

    }
    return(nf);
}

static char **
formLINamesList (struct lsInfo *lsInfo)
{
    int i;
    static char **names = NULL;
    char **dispindex = NULL;

    if (names == NULL)
        /*add a space for 'slots'*/
        if ((names=(char **)malloc((lsInfo->numIndx+1+1)*sizeof(char *)))
								    == NULL) {
            lserrno = LSE_MALLOC;
            ls_perror(NULL);
            exit(-1);
	}

    for (i = 0; i < lsInfo->numIndx; i++)
        names[i] = lsInfo->resTable[i].name;
    names[i] = NULL;
    dispindex = names;

    return(dispindex);

}

static float
getLoad(char *dispindex, float *loads, int *index)
{
    int i;

    for (i = 0; i < lsInfoPtr->numIndx; i++) {
	if (!strcmp (dispindex, lsInfoPtr->resTable[i].name))  {
	    *index = i;
	    return (loads[i]);
        }
    }
    return (INFINIT_LOAD);

}

static void
getCloseString(int hStatus, char **status)
{
    if (hStatus & HOST_STAT_DISABLED)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1635, "closed_Adm")); /* catgets  1635  */
    else if (hStatus & HOST_STAT_EXCLUSIVE)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1636, "closed_Excl")); /* catgets  1636  */
    else if (hStatus & HOST_STAT_LOCKED)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1637, "closed_Lock")); /* catgets  1637  */
    else if (hStatus & HOST_STAT_LOCKED_MASTER)
        *status = (I18N(1643, "closed_LockM")); /* catgets  1643  */
    else if (hStatus & HOST_STAT_NO_LIM)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1638, "closed_LIM")); /* catgets  1638  */
    else if (hStatus & HOST_STAT_WIND)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1639, "closed_Wind")); /* catgets  1639  */
    else if (hStatus & HOST_STAT_FULL)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1640, "closed_Full")); /* catgets  1640  */
    else if (hStatus & HOST_STAT_BUSY)
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1641, "closed_Busy")); /* catgets  1641  */
    else
        *status = (_i18n_msg_get(ls_catd,NL_SETN,1642, "unknown")); /* catgets  1642  */
}

static void
displayShareRes(int argc, char **argv, int index)
{
    struct lsbSharedResourceInfo  *lsbResourceInfo;
    int   numRes = 0;
    char **resourceNames = NULL, **resources = NULL;
    char fname[]="displayShareRes";

    if (argc > index) {
        if ((resourceNames =
              (char **) malloc ((argc - index) * sizeof (char *))) == NULL) {
		char i18nBuf[100];
		sprintf ( i18nBuf,I18N_FUNC_FAIL,fname,"malloc");
            	perror( i18nBuf );
            	exit (-1);
        }
        numRes = getResourceNames (argc, argv, index, resourceNames);
    }
    if (numRes > 0)
        resources = resourceNames;

    TIMEIT(0, (lsbResourceInfo = lsb_sharedresourceinfo (resources, &numRes, NULL, 0)), "lsb_sharedresourceinfo");

    if (lsbResourceInfo == NULL) {
        if (lsberrno == LSBE_BAD_RESOURCE && resources)
            lsb_perror(NULL);
        else
            lsb_perror("lsb_sharedresourceinfo");
        exit(-1);
    }
    prtResourcesShort(numRes, lsbResourceInfo);
    FREEUP(resourceNames);
}

static void
prtResourcesShort(int num, struct lsbSharedResourceInfo  *info)
{
    struct lsInfo *lsInfo;
    int i, j, k;
    char *buf1, *buf2, *buf3, *buf4;

    if ((lsInfo = ls_info()) == NULL) {
        ls_perror("lsinfo");
        exit(-10);
    }

    buf1 = putstr_(_i18n_msg_get(ls_catd,NL_SETN,1645, "RESOURCE")); /* catgets 1645 */
    buf2 = putstr_(_i18n_msg_get(ls_catd,NL_SETN,1646, "TOTAL"));  /* catgets  1646 */
    buf3 = putstr_(_i18n_msg_get(ls_catd,NL_SETN,1647, "RESERVED")); /* catgets 1647 */
    buf4 = putstr_(_i18n_msg_get(ls_catd,NL_SETN,1648, "LOCATION")); /* catgets 1648 */

    printf("%-20s%10s%15s%15s\n", buf1, buf2, buf3, buf4);

    FREEUP(buf1);
    FREEUP(buf2);
    FREEUP(buf3);
    FREEUP(buf4);


    for (i = 0; i < num; i++) {
        for (j = 0; j < info[i].nInstances; j++)  {


            for (k = 0; k < lsInfo->nRes; k++) {
                if (strcmp(lsInfo->resTable[k].name,
                           info[i].resourceName) == 0) {

                    if (lsInfo->resTable[k].valueType & LS_NUMERIC) {
                         prtOneInstance(info[i].resourceName,
                                          &(info[i].instances[j]));
                    }
                }
            }
        }
    }

}


static void
prtOneInstance(char *resName, struct lsbSharedResourceInstance  *instance)
{
#define ONE_LINE   80
    int i, len, currentPos = 52;
    char space52[] = "                                                    ";
    printf ("%-20s%10s%15s       ", resName,
            stripSpaces(instance->totalValue), stripSpaces(instance->rsvValue));
    for (i = 0; i < instance->nHosts; i++) {
        len = strlen(instance->hostList[i]);
        currentPos = currentPos + len + 1;
        if (currentPos > ONE_LINE) {
            printf ("\n%s", space52);
            currentPos = 52 + len + 1;
        }
        printf ("%s ", instance->hostList[i]);
    }
    printf("\n");
}


static int
makeShareFields(char *hostname, struct lsInfo *lsInfo, char ***nameTable,
                char ***totalValues, char ***rsvValues, char ***formatTable)
{
    static int first = TRUE;
    static struct lsbSharedResourceInfo *resourceInfo;
    static char **namTable;
    static char **totalTable;
    static char **rsvTable;
    static char **fmtTable;
    static int numRes, nRes;
    int k, i, j;
    char *hPtr;
    int ii, numHosts, found;

    if (first == TRUE) {

        TIMEIT(0, (resourceInfo = lsb_sharedresourceinfo (NULL, &numRes, NULL, 0)), "ls_sharedresourceinfo");

        if (resourceInfo == NULL) {
            return (-1);
        }

        if ((namTable =
                        (char **) malloc (numRes * sizeof(char *))) == NULL){
            lserrno = LSE_MALLOC;
            return (-1);
        }
        if ((totalTable =
                        (char **) malloc (numRes * sizeof(char *))) == NULL){
            lserrno = LSE_MALLOC;
            return (-1);
        }
        if ((rsvTable =
                        (char **) malloc (numRes * sizeof(char *))) == NULL){
            lserrno = LSE_MALLOC;
            return (-1);
        }

        if ((fmtTable = (char **) malloc (numRes * sizeof(char *))) == NULL){
            lserrno = LSE_MALLOC;
            return (-1);
        }
        first = FALSE;
    } else {

        for (i = 0; i < nRes; i++) {
            FREEUP(fmtTable[i]);
        }
    }

    nRes = 0;
    for (k = 0; k < numRes; k++) {
	found = FALSE;
	for (j = 0; j < lsInfo->nRes; j++) {
	    if (strcmp(lsInfo->resTable[j].name,
			resourceInfo[k].resourceName) == 0) {
		if ((lsInfo->resTable[j].flags & RESF_SHARED) &&
		    (lsInfo->resTable[j].valueType & LS_NUMERIC)) {

		    found = TRUE;
		    break;
		}
		break;
	    }
	}
	if (!found) {

	    continue;
	}
	namTable[nRes] = resourceInfo[k].resourceName;
	found = FALSE;
        for (i = 0; i < resourceInfo[k].nInstances; i++) {
	    numHosts =  resourceInfo[k].instances[i].nHosts;
	    for (ii = 0; ii < numHosts; ii++) {
		hPtr  = resourceInfo[k].instances[i].hostList[ii];
		if (strcmp(hPtr, hostname) == 0) {
		    totalTable[nRes] = resourceInfo[k].instances[i].totalValue;
		    rsvTable[nRes] = resourceInfo[k].instances[i].rsvValue;
		    found = TRUE;
		    break;
		}
	    }
	    if (found == TRUE) {
		break;
	    }
        }
	if (found == FALSE) {
	    totalTable[nRes] = "-";
	    rsvTable[nRes] =  "-";
	}
	nRes++;
    }
    if (nRes) {
        j = 0;
        for (i = 0; i < nRes; i++) {
             char fmt[16];
             int lens, tmplens;


             lens = strlen( namTable[i] );
             tmplens = strlen( stripSpaces(totalTable[i]) );
             if( lens < tmplens )
                 lens = tmplens;

             tmplens = strlen( stripSpaces(rsvTable[i]) );
             if( lens < tmplens )
                 lens = tmplens;

             sprintf(fmt, "%s%ld%s", "%", (long)(lens + 1), "s");
             fmtTable[j++] = putstr_(fmt);
        }
    }
    *nameTable = namTable;
    *totalValues = totalTable;
    *rsvValues  = rsvTable;
    *formatTable = fmtTable;
    return (nRes);
}

static void  insertSlotsToResults(struct hostInfoEnt  *hPtr,
                                  char **loadvalTotal,
                                  char **loadvalRsv,
                                  char **nlp,
                                  int  *last,
                                  struct lsInfo *lsInfo) {
    int i, idx, hUsrLoadNum;
    char tmpfield[MAXFIELDSIZE];
    char tmpFmt[MAXFIELDSIZE];

    hUsrLoadNum = *last - NBUILTINDEX;
    for (i = 0; i < hUsrLoadNum; i++) {
        loadvalTotal[*last - i] = loadvalTotal[*last - 1 - i];
        loadvalRsv[*last - i] = loadvalRsv[*last - 1 - i];
        nlp[*last - i] = nlp[*last - 1 - i];
    }

    /*insert 'slots' value, it will use default format*/
    *last += 1;
    idx = NBUILTINDEX;
    loadvalTotal[idx] = malloc(MAXFIELDSIZE);
    loadvalRsv[idx] = malloc(MAXFIELDSIZE);
    nlp[idx] = "slots";

    if (loadvalTotal[idx] == NULL || loadvalRsv[idx] == NULL) {
        lserrno=LSE_MALLOC;
        lsb_perror("insertSlotsToResults()");
        exit(-1);
    }

    if (hPtr->maxJobs < INFINIT_INT) {
        sprintf(tmpfield, "%d", (hPtr->maxJobs - hPtr->numJobs));
        if (strlen(tmpfield) >= fmt[DEFAULT_FMT].dispLen) {
            sprintf(tmpFmt, "%s%s", fmt[DEFAULT_FMT].ok, fmt[DEFAULT_FMT].expFmt);
            sprintf(tmpfield, tmpFmt, (float)(hPtr->maxJobs - hPtr->numJobs));
        }
        sprintf(loadvalTotal[idx], fmt[DEFAULT_FMT].hdr, tmpfield);
    } else {
        sprintf(loadvalTotal[idx], fmt[DEFAULT_FMT].hdr, "-");
    }
    sprintf(loadvalRsv[idx], fmt[DEFAULT_FMT].hdr, "-");
}

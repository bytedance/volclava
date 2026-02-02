/* $Id: lib.pim.c 397 2007-11-26 19:04:00Z mblack $
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "../lsf.h"
#include "lproto.h"

#define NL_SETN   23
static struct config_param pimParams[] =
{
#define LSF_PIM_INFODIR 0
    {"LSF_PIM_INFODIR", NULL},
#define LSF_PIM_SLEEPTIME 1
    {"LSF_PIM_SLEEPTIME", NULL},
#define LSF_LIM_DEBUG 2
    {"LSF_LIM_DEBUG", NULL},
#define LSF_LOGDIR 3
    {"LSF_LOGDIR", NULL},
#define LSF_PIM_SLEEPTIME_UPDATE 4
    {"LSF_PIM_SLEEPTIME_UPDATE", NULL},
    {NULL, NULL}
};

#define PGID_LIST_SIZE 16
#define PID_LIST_SIZE 64
#define MAX_NUM_PID   300

static int npidList = 0;
static struct pidInfo *pidList = NULL;
static struct lsPidInfo *pinfoList = NULL;
static int npinfoList = 0;
static int npgidList = 0;
static int *pgidList = NULL;
static int hitPGid = 0;
static char *pimInfoBuf = NULL;
static long pimInfoLen;

static int pimPort(struct sockaddr_in *, char *);
static struct jRusage *readPIMInfo(int, int *);
static int inAddPList(struct lsPidInfo *pinfo);
static int intoPidList(struct lsPidInfo *pinfo);
static FILE *openPIMFile(char *pfile);
static int readPIMFile(char *);
static char *getNextString(char *,char *);
static char *readPIMBuf(char *);

static int argOptions;

struct jRusage *getJInfo_(int npgid, int *pgid, int options, int cpgid)
{
    static char fname[] = "lib.pim.c/getJInfo_()";
    struct jRusage *jru;

    static char pfile[MAXFILENAMELEN];
    char *myHost;
    static struct sockaddr_in pimAddr;
    struct LSFHeader sendHdr, recvHdr, hdrBuf;
    int s, cc;
    struct timeval timeOut;
    static time_t lastTime = 0, lastUpdateNow = 0;
    time_t now;
    static time_t pimSleepTime = PIM_SLEEP_TIME;
    static bool_t periodicUpdateOnly = FALSE;

    now = time(0);

    if (logclass & LC_PIM)
       ls_syslog(LOG_DEBUG3,"now = %ld, lastTime = %ld, sleepTime = %ld",
                 now, lastUpdateNow, pimSleepTime);
    argOptions = options;

    if (lastTime == 0) {
	struct hostent *hp;
        struct config_param *plp;

        for (plp = pimParams; plp->paramName != NULL; plp++) {
             if (plp->paramValue != NULL) {
                 FREEUP (plp->paramValue);
             }
        }

	if (initenv_(pimParams, NULL) < 0) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG, "%s: initenv_() failed: %M", fname);
	    return NULL;
	}

	if ((myHost = ls_getmyhostname()) == NULL) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG,
			  "%s: ls_getmyhostname() failed: %m", fname);
	    return (NULL);
	}

	if (pimParams[LSF_PIM_INFODIR].paramValue)
	    sprintf(pfile, "%s/pim.info.%s",
		    pimParams[LSF_PIM_INFODIR].paramValue, myHost);
	else {
	    if (pimParams[LSF_LIM_DEBUG].paramValue) {
		if (pimParams[LSF_LOGDIR].paramValue)
		    sprintf(pfile, "%s/pim.info.%s",
			    pimParams[LSF_LOGDIR].paramValue, myHost);
		else
		    sprintf(pfile, "/tmp/pim.info.%s.%d", myHost, (int)getuid());
	    } else {
		sprintf(pfile, "/tmp/pim.info.%s", myHost);
	    }
	}

	if (pimParams[LSF_PIM_SLEEPTIME].paramValue) {
	    if ((pimSleepTime =
		 atoi(pimParams[LSF_PIM_SLEEPTIME].paramValue)) < 0) {
		if (logclass & LC_PIM)
		    ls_syslog(LOG_DEBUG, "LSF_PIM_SLEEPTIME value <%s> must be a positive integer, defaulting to %d", pimParams[LSF_PIM_SLEEPTIME].paramValue, PIM_SLEEP_TIME);
		pimSleepTime = PIM_SLEEP_TIME;
	    }
	}

        if (pimParams[LSF_PIM_SLEEPTIME_UPDATE].paramValue != NULL
            && strcasecmp(pimParams[LSF_PIM_SLEEPTIME_UPDATE].paramValue, "y") == 0) {
            periodicUpdateOnly = TRUE;
            if (logclass & LC_PIM)
                ls_syslog(LOG_DEBUG, "%s: Only to call pim each PIM_SLEEP_TIME interval", fname);
        }

	if ((hp = Gethostbyname_(myHost)) == NULL) {
	    return NULL;
	}

	memset((char *) &pimAddr, 0, sizeof(pimAddr));
	memcpy((char *) &pimAddr.sin_addr, (char *) hp->h_addr,
	       (int)hp->h_length);
	pimAddr.sin_family = AF_INET;
    }


    if (now - lastUpdateNow >= pimSleepTime || (options & PIM_API_UPDATE_NOW)) {
        if (logclass & LC_PIM)
	    ls_syslog(LOG_DEBUG,"%s: update now", fname);
	lastUpdateNow = now;

	if ((s = TcpCreate_(FALSE, 0)) < 0) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG, "%s: tcpCreate failed: %m", fname);
	    return (NULL);
	}

	if (pimPort(&pimAddr, pfile) == -1) {
	    close(s);
	    return (NULL);
	}

	if (b_connect_(s, (struct sockaddr *) &pimAddr, sizeof(pimAddr), 0)
	    == -1) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG, "%s: b_connect() failed: %m", fname);
	    lserrno = LSE_CONN_SYS;
	    close(s);
	    return (NULL);
	}

	initLSFHeader_(&sendHdr);
	initLSFHeader_(&recvHdr);

	sendHdr.opCode = options;
	sendHdr.refCode = (short) now & 0xffff;
	sendHdr.reserved = cpgid;

	if ((cc = writeEncodeHdr_(s, &sendHdr, b_write_fix)) < 0) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG,
			  "%s: writeEncodeHdr failed cc=%d: %M", fname, cc);
	    close(s);
	    return (NULL);
	}

	timeOut.tv_sec = 10;
	timeOut.tv_usec = 0;
	if ((cc = rd_poll_(s, &timeOut)) < 0) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG, "%s: rd_poll_ cc=%d: %m", fname, cc);
	    close(s);
	    return (NULL);
	}

	if ((cc = lsRecvMsg_(s, (char *) &hdrBuf, sizeof(hdrBuf), &recvHdr,
			     NULL, NULL, b_read_fix)) < 0) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG, "%s: lsRecvMsg_ failed cc=%d: %M",
			  fname, cc);
	    close(s);
	    return (NULL);
	}
	close(s);

	if (recvHdr.refCode != sendHdr.refCode) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG,
			  "%s: recv refCode=%d not equal to send refCode=%d, server is not PIM",
			  fname, (int) recvHdr.refCode, (int) sendHdr.refCode);
	    return (NULL);
	}
        if (logclass & LC_PIM)
	    ls_syslog(LOG_DEBUG,"%s updated now",fname);
	if (!readPIMFile(pfile)) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL,  fname, "readPIMFile");
		return(NULL);
	}
    }

    lastTime = now;

    if ((jru = readPIMInfo(npgid, pgid)) == NULL &&
	!(options & PIM_API_UPDATE_NOW)
	&& (periodicUpdateOnly == FALSE
	    || (periodicUpdateOnly == TRUE
		&& now - lastUpdateNow >= pimSleepTime))) {
	if (hitPGid > 0) {
	   jru = getJInfo_(npgid, pgid, options | PIM_API_UPDATE_NOW, hitPGid);
	   hitPGid = 0;
	   return jru;
	}
	else {
	   return (getJInfo_(npgid, pgid, options | PIM_API_UPDATE_NOW, cpgid));
	}
    }
    return jru;


}

static char *
readPIMBuf(char *pfile)
{
	char *fname="readPIMBuf";
	struct stat bstat;
	FILE *fp;

	FREEUP(pimInfoBuf);
	pimInfoLen = 0;

	if (stat(pfile,&bstat) < 0) {
		ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "stat", pfile);
		return(NULL);
	}
	pimInfoLen = bstat.st_size;
	if ((pimInfoBuf = (char *)malloc(pimInfoLen+1)) == NULL) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M,  fname, "malloc");
		return(NULL);
	}
        if ((fp = openPIMFile(pfile)) == NULL) {
	    ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "openPIMFile", pfile);
   	    return (FALSE);
        }
	if (fread(pimInfoBuf,sizeof(char),pimInfoLen,fp) <= 0) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fread");
            FREEUP(pimInfoBuf);
            return(NULL);
	}

	fclose(fp);

	pimInfoBuf[pimInfoLen] = '\0';
	return(pimInfoBuf);
}

static char *
getNextString(char *buf,char *string)
{
	char *tmp;
	int  i = 0;

	if ((*buf == EOF) || (*buf == '\0')) {
		return(NULL);
	}
	tmp = buf;
	while ((*tmp!=EOF)&&(*tmp!='\0')&&(*tmp!='\n')) {
		string[i++]=*tmp;
		tmp ++;
	}
	string[i]='\0';
	if (*tmp=='\n') {
		tmp ++;
	}
	return(tmp);
}

static int
readPIMFile(char *pfile)
{
	char *fname = "readPIMFile";
	struct lsPidInfo *tmp;
	char *buffer,*tmpbuf;
	char pimString[MAXLINELEN];

	if ((buffer=readPIMBuf(pfile))==NULL) {
	    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "readPIMBuf");
	    return(FALSE);
	}

	FREEUP(pinfoList);
	npinfoList = 0;
	pinfoList = (struct lsPidInfo *)malloc(sizeof(struct lsPidInfo) * MAX_NUM_PID);
	if (pinfoList == NULL) {
		ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname,  "malloc",
				sizeof(struct lsPidInfo) * MAX_NUM_PID);
		return(FALSE);
	}

	tmpbuf = getNextString(buffer,pimString);
	if (tmpbuf == NULL) {
		ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5908,
	"%s format error"), "pim.info"); /* catgets 5908 */
		return(FALSE);
	}
	buffer = tmpbuf;

        while ((tmpbuf = getNextString(buffer,pimString))!=NULL) {
	    buffer = tmpbuf;
            if (logclass & LC_PIM)
	        ls_syslog(LOG_DEBUG3,"pim info string is %s",pimString);
	    sscanf(pimString, "%d %d %d %d %d %d %d %d %d %d %d %d",
		  &pinfoList[npinfoList].pid, &pinfoList[npinfoList].ppid,
		  &pinfoList[npinfoList].pgid, &pinfoList[npinfoList].jobid,
		  &pinfoList[npinfoList].utime, &pinfoList[npinfoList].stime,
		  &pinfoList[npinfoList].cutime, &pinfoList[npinfoList].cstime,
		  &pinfoList[npinfoList].proc_size, &pinfoList[npinfoList].resident_size,
		  &pinfoList[npinfoList].stack_size,
		  (int *)&pinfoList[npinfoList].status);

		npinfoList ++;
		if (npinfoList % MAX_NUM_PID == 0) {
			tmp = (struct lsPidInfo *)realloc(pinfoList,
			       sizeof(struct lsPidInfo) * (npinfoList + MAX_NUM_PID));
			if (tmp == NULL) {
				ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "realloc",
					  sizeof(struct lsPidInfo)
					  * (npinfoList + MAX_NUM_PID));
				return(FALSE);
			}
			pinfoList = tmp;
		}
	}

	return(TRUE);
}

static struct jRusage *
readPIMInfo(int inNPGids, int *inPGid)
{
    static char fname[] = "readPIMInfo";
    static struct jRusage jru;
    int found = FALSE;
    int cc, i, j, pinfoNum;
    static int *activeInPGid = NULL;

    FREEUP(pgidList);
    FREEUP(pidList);
    FREEUP(activeInPGid);

    memset((char *) &jru, 0, sizeof(jru));

    activeInPGid = (int *) malloc(inNPGids * sizeof(int));
    if (activeInPGid == NULL) {
	ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "malloc",
						inNPGids * sizeof(int));
	return (NULL);
    }

    memset((char *) activeInPGid, 0, inNPGids * sizeof(int));

    pgidList = (int *) malloc((inNPGids < PGID_LIST_SIZE ?
		   			  PGID_LIST_SIZE :
			       inNPGids + PGID_LIST_SIZE) * sizeof(int));
    if (pgidList == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "malloc",
                 (inNPGids < PGID_LIST_SIZE ? PGID_LIST_SIZE :
                                inNPGids + PGID_LIST_SIZE) * sizeof(int));
	return (NULL);
    }

    npgidList = inNPGids;
    memcpy((char *) pgidList, (char *) inPGid, npgidList * sizeof(int));

    pidList = (struct pidInfo *) malloc(PID_LIST_SIZE * sizeof(struct pidInfo));

    if (pidList == NULL) {
	ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, fname, "malloc",
                                PID_LIST_SIZE * sizeof(struct pidInfo));
	return (NULL);
    }
    npidList = 0;

    for (pinfoNum = 0;pinfoNum < npinfoList; pinfoNum ++) {
	if (argOptions & PIM_API_TREAT_JID_AS_PGID)
	    pinfoList[pinfoNum].pgid = pinfoList[pinfoNum].jobid;

	for (i = 0; i < inNPGids; i++) {
	    if (pinfoList[pinfoNum].pgid == inPGid[i])
		activeInPGid[i] = TRUE;
	}

	if ((cc = inAddPList(pinfoList+pinfoNum)) == 1) {
	    if (pinfoList[pinfoNum].status != LS_PSTAT_ZOMBI &&
		pinfoList[pinfoNum].status != LS_PSTAT_EXITING) {
	    	if (pinfoList[pinfoNum].stack_size == -1) {
			hitPGid = pinfoList[pinfoNum].pgid;
			found = FALSE;
			break;
	    	}
		jru.mem += pinfoList[pinfoNum].resident_size;
		jru.swap += pinfoList[pinfoNum].proc_size;
		jru.utime += pinfoList[pinfoNum].utime;
		jru.stime += pinfoList[pinfoNum].stime;

		if (logclass & LC_PIM)
		    ls_syslog(LOG_DEBUG,
			      "%s: Got pid=%d ppid=%d pgid=%d utime=%d stime=%d cutime=%d cstime=%d proc_size=%d resident_size=%d stack_size=%d status=%d",
			      fname,
			      pinfoList[pinfoNum].pid, pinfoList[pinfoNum].ppid,
			      pinfoList[pinfoNum].pgid, pinfoList[pinfoNum].utime,
			      pinfoList[pinfoNum].stime, pinfoList[pinfoNum].cutime,
			      pinfoList[pinfoNum].cstime,
			      pinfoList[pinfoNum].proc_size,
			      pinfoList[pinfoNum].resident_size,
			      pinfoList[pinfoNum].stack_size,
			      (int) pinfoList[pinfoNum].status);

		if (pinfoList[pinfoNum].pid > -1)
		    found = TRUE;
	    }
	} else {
	    if (cc == -1) {
		return (NULL);
	    }
	}
    }

    if (found) {
	int n;

	n = inNPGids;

	for (i = 0; i < n; i++) {
	    if (!activeInPGid[i]) {
		npgidList--;

		for (j = i; j < npgidList; j++) {
		    pgidList[j] = pgidList[j+1];
		}

		n--;
		for (j = i; j < n; j++) {
		    activeInPGid[j] = activeInPGid[j+1];
		}
	    }
	}

        jru.npids = npidList;
        jru.pidInfo = pidList;
	jru.npgids = npgidList;
	jru.pgid = pgidList;
	return (&jru);
    }

    return (NULL);
}

static int
inAddPList(struct lsPidInfo *pinfo)
{
    int i;

    for (i = 0; i < npgidList; i++) {
	if (pinfo->pgid == pgidList[i]) {
	    if (pinfo->pid > -1) {
		if (intoPidList(pinfo) == -1)
		    return (-1);
	    }
	    return (1);
	}
    }

    if (pinfo->pid < 0)
	return (0);

    for (i = 0; i < npidList; i++) {
	if (pinfo->ppid == pidList[i].pid) {
	    pgidList[npgidList] = pinfo->pgid;
	    npgidList++;

	    if (npgidList % PGID_LIST_SIZE == 0) {
		int *tmpPtr;

		tmpPtr = (int *) realloc((char *) pgidList,
					 (npgidList + PGID_LIST_SIZE) *
					 sizeof(int));
		if (tmpPtr == NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "inAddPList",
			"realloc", (npgidList + PGID_LIST_SIZE) * sizeof(int));
		    return (-1);
		}
		pgidList = tmpPtr;
	    }

	    if (intoPidList(pinfo) == -1)
		return (-1);

	    return (1);
	}
    }

    return (0);
}


static int
intoPidList(struct lsPidInfo *pinfo)
{
    pidList[npidList].pid = pinfo->pid;
    pidList[npidList].ppid = pinfo->ppid;
    pidList[npidList].pgid = pinfo->pgid;
    pidList[npidList].jobid = pinfo->jobid;

    npidList++;

    if (npidList % PID_LIST_SIZE == 0) {
	struct pidInfo *tmpPtr;

	tmpPtr = (struct pidInfo *) realloc((char *) pidList,
					    (npidList + PID_LIST_SIZE) *
					    sizeof(struct pidInfo));
	if (tmpPtr == NULL) {
	    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "intoPidList", "realloc",
			(npidList + PID_LIST_SIZE) * sizeof(struct pidInfo));
	    return (-1);
	}
	pidList = tmpPtr;
    }

    return (0);
}


static int
pimPort(struct sockaddr_in *pimAddr, char *pfile)
{
    FILE *fp;
    int port;

    if ((fp = openPIMFile(pfile)) == NULL)
	return (-1);

    fscanf(fp, "%d", &port);

    fclose(fp);

    pimAddr->sin_port = htons(port);
    return (0);
}


static FILE *
openPIMFile(char *pfile)
{
    static char fname[] = "openPIMFile";
    FILE *fp;

    if ((fp = fopen(pfile, "r")) == NULL) {
	millisleep_(1000);
	if ((fp = fopen(pfile, "r")) == NULL) {
	    if (logclass & LC_PIM)
		ls_syslog(LOG_DEBUG, "%s: fopen(%s) failed: %m", fname, pfile);
	    lserrno = LSE_FILE_SYS;
	    return (NULL);
	}
    }

    return (fp);
}


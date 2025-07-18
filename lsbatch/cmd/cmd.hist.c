/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: cmd.hist.c 397 2007-11-26 19:04:00Z mblack $
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

#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include "cmd.h"

#define NL_SETN 8

static int eventMatched = FALSE;

void displayEvent(struct eventRec *, struct histReq *);
static int isRequested(char *, char **);
extern char *myGetOpt (int nargc, char **, char *);


int
sysHist(int argc, char **argv, int opCode)
{
    struct histReq req;
    int all = FALSE, eventFound;
    char **nameList=NULL;
    int numNames = 0;
    char *optName;


    req.opCode = opCode;
    req.names = NULL;
    req.eventFileName = NULL;
    req.eventTime[0] = 0;
    req.eventTime[1] = 0;

    while ((optName = myGetOpt(argc, argv, "t:|f:")) != NULL) {
	switch (optName[0]) {
	case 'f':
            if (strlen(optarg) > MAXFILENAMELEN -1) {
		fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1051, "%s: File name too long\n")), optarg); /* catgets  1051  */
		return(-1);
            } else
	        req.eventFileName = optarg;
	    break;
        case 't':
            if (getBEtime(optarg, 't', req.eventTime) == -1) {
		ls_perror(optarg);
		return (-1);
            }
            break;
	default:
	    return(-2);
	}
    }

    switch (opCode) {
    case QUEUE_HIST:
    case HOST_HIST:
	if (argc > optind) {
	    numNames = getNames(argc, argv, optind, &nameList, &all, "queueC");
	    if (!all && numNames != 0) {
	        nameList[numNames] = NULL;
		req.names = nameList;
	    }
	}
	break;

    case MBD_HIST:
    case SYS_HIST:
        if (argc > optind)
            return(-2);
	break;

    default:
	fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,1052, "Unknown operation code\n"))); /* catgets  1052  */
	return(-2);
    }

    return (searchEventFile(&req, &eventFound));

}

int
searchEventFile(struct histReq *req, int *eventFound)
{
    char eventFileName[MAXFILENAMELEN];
    int lineNum = 0;
    struct stat statBuf;
    struct eventRec *log;
    FILE *log_fp;
    char *envdir;

    struct config_param histParams[] = {
#define LSB_SHAREDIR 0
        {"LSB_SHAREDIR", NULL},
        {NULL, NULL}
    };

    if ((envdir = getenv("LSF_ENVDIR")) == NULL)
	envdir = "/etc";

    if (ls_readconfenv(histParams, envdir) < 0) {
	ls_perror("ls_readconfenv");
	return(-1);
    }

    if (!req->eventFileName) {
	memset(eventFileName, 0, sizeof(eventFileName));
	ls_strcat(eventFileName, sizeof(eventFileName),
                  histParams[LSB_SHAREDIR].paramValue);
	ls_strcat(eventFileName, sizeof(eventFileName),"/logdir/lsb.events");
    } else {
	memset(eventFileName,0, sizeof(eventFileName));
	ls_strcat(eventFileName, sizeof(eventFileName),req->eventFileName);
    }

    FREEUP (histParams[LSB_SHAREDIR].paramValue);

    if (stat(eventFileName, &statBuf) < 0) {
        perror(eventFileName);
        return (-1);
    }

    if ((log_fp = fopen(eventFileName, "r")) == NULL) {
	perror(eventFileName);
	return(-1);
    }

    eventMatched = FALSE;
    while (TRUE) {

	log = lsb_geteventrec(log_fp, &lineNum);
        if (log) {
	    displayEvent(log, req);
	    continue;
        }

        if (lsberrno == LSBE_EOF)
	    break;
	fprintf(stderr, "\
File %s at line %d: %s\n", eventFileName, lineNum, lsb_sysmsg());
    }

    if (!eventMatched)
        fprintf(stderr, "No matching event found\n");
    *eventFound = eventMatched;

    fclose(log_fp);

    return 0;
}

void
displayEvent(struct eventRec *log, struct histReq *req)
{
    char prline[MSGSIZE];
    char localTimeStr[60];

    if (req->eventTime[1] != 0
	&& req->eventTime[0] < req->eventTime[1]) {
	if (log->eventTime < req->eventTime[0]
	    || log->eventTime > req->eventTime[1])
            return;
    }

    switch (log->type) {
    case EVENT_LOG_SWITCH:
         if (req->opCode == MBD_HIST || req->opCode == SYS_HIST) {
            eventMatched = TRUE;
	    strcpy ( localTimeStr, _i18n_ctime(ls_catd,CTIME_FORMAT_a_b_d_T, &log->eventTime ));
            sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1055, "%s: event file is switched; last JobId <%d>\n")), /* catgets  1055  */
            localTimeStr, log->eventLog.logSwitchLog.lastJobId);
            prtLine(prline);
        }
        break;
    case EVENT_MBD_START:
	if (req->opCode == MBD_HIST || req->opCode == SYS_HIST) {
	    strcpy ( localTimeStr, _i18n_ctime(ls_catd,CTIME_FORMAT_a_b_d_T, &log->eventTime ));
            eventMatched = TRUE;
	    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1056, "%s: mbatchd started on host <%s>; cluster name <%s>, %d server hosts, %d queues.\n")),   /* catgets  1056  */
		    localTimeStr,
		    log->eventLog.mbdStartLog.master,
		    log->eventLog.mbdStartLog.cluster,
		    log->eventLog.mbdStartLog.numHosts,
		    log->eventLog.mbdStartLog.numQueues);
	    prtLine(prline);
	}
        break;
    case EVENT_MBD_DIE:
	if (req->opCode == MBD_HIST || req->opCode == SYS_HIST) {
            eventMatched = TRUE;
	    strcpy ( localTimeStr, _i18n_ctime(ls_catd,CTIME_FORMAT_a_b_d_T, &log->eventTime ));
	    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1057, "%s: mbatchd on host <%s> died: ")), /* catgets  1057  */
		    localTimeStr,
		    log->eventLog.mbdStartLog.master);
	    prtLine(prline);
            switch (log->eventLog.mbdDieLog.exitCode) {
            case MASTER_RESIGN:
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1058, "master resigned.\n"))); /* catgets  1058  */
                break;
	    case MASTER_RECONFIG:
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1059, "reconfiguration initiated. %s\n")),
                        log->eventLog.mbdDieLog.msg); /* catgets  1059  */
                break;
	    case MASTER_FATAL:
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1060, "fatal errors.\n"))); /* catgets  1060  */
                break;
	    case MASTER_MEM:
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1060, "fatal memory errors.\n"))); /* catgets  1061  */
                break;
	    case MASTER_CONF:
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1062, "bad configuration file.\n"))); /* catgets  1062  */
                break;
            default:
                sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1063, "killed by signal <%d>.\n")),  /* catgets  1063  */
                        log->eventLog.mbdDieLog.exitCode);
                break;
	    }
            prtLine(prline);
	}
        break;
    case EVENT_QUEUE_CTRL:
	if ((req->opCode == SYS_HIST)
	    || (req->opCode == QUEUE_HIST
		&& isRequested(log->eventLog.queueCtrlLog.queue,
			       req->names))) {
	    strcpy ( localTimeStr, _i18n_ctime(ls_catd,CTIME_FORMAT_a_b_d_T, &log->eventTime ));
            eventMatched = TRUE;
	    sprintf(prline, "%s: %s <%s> ",
		    localTimeStr,
		    I18N_Queue,
	            log->eventLog.queueCtrlLog.queue);
            prtLine(prline);
	    switch (log->eventLog.queueCtrlLog.opCode) {
	    case QUEUE_OPEN:
		sprintf(prline, I18N_opened);
		break;
	    case QUEUE_CLOSED:
		sprintf(prline, I18N_closed);
		break;
	    case QUEUE_ACTIVATE:
		sprintf(prline, I18N_activated);
		break;
 	    case QUEUE_INACTIVATE:
		sprintf(prline, I18N_inactivated);
		break;
            default:
                sprintf(prline, "%s <%d>",
			I18N(1069, "unknown operation code"),/* catgets  1069  */
                        log->eventLog.queueCtrlLog.opCode);
                break;
	    }
            prtLine(prline);
            if (log->eventLog.queueCtrlLog.userName
		&& log->eventLog.queueCtrlLog.userName[0]) {
                if (log->eventLog.queueCtrlLog.msg[0] != '\0') {
                    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1070, " by user or administrator <%s>. %s.\n")),
                            log->eventLog.queueCtrlLog.userName,
                            log->eventLog.queueCtrlLog.msg); /* catgets  1070  */ 
                } else {
                    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1070, " by user or administrator <%s>.\n")), log->eventLog.queueCtrlLog.userName); /* catgets  1070  */
                }
            } else {
                if (log->eventLog.queueCtrlLog.msg[0] != '\0') {
                     sprintf(prline, ". %s.\n", log->eventLog.queueCtrlLog.msg);
                } else {
                     sprintf(prline, ".\n");
                }
            }

	    prtLine(prline);
    	}
	break;
    case EVENT_HOST_CTRL:
	if ((req->opCode == SYS_HIST)
	     || (req->opCode == HOST_HIST
		 && isRequested(log->eventLog.hostCtrlLog.host, req->names))) {
            eventMatched = TRUE;
	    strcpy ( localTimeStr, _i18n_ctime(ls_catd,CTIME_FORMAT_a_b_d_T, &log->eventTime ));
	    sprintf(prline, "%s: %s <%s> ",
		    localTimeStr,
		    I18N_Host,
	            log->eventLog.hostCtrlLog.host);
 	    prtLine(prline);
	    switch (log->eventLog.hostCtrlLog.opCode) {
	    case HOST_OPEN:
		sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1072, "opened"))); /* catgets  1072  */
		break;
	    case HOST_CLOSE:
		sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1073, "closed"))); /* catgets  1073  */
		break;
	    case HOST_REBOOT:
		sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1074, "rebooted"))); /* catgets  1074  */
		break;
	    case HOST_SHUTDOWN:
		sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1075, "shutdown"))); /* catgets  1075  */
		break;
            default:
                sprintf(prline, "%s <%d>",
		        I18N(1069, "unknown operation code"),/* catgets 1069  */
                        log->eventLog.hostCtrlLog.opCode);
                break;
	    }
	    prtLine(prline);
            if (log->eventLog.hostCtrlLog.userName
	        && log->eventLog.hostCtrlLog.userName[0]) {
                if (log->eventLog.hostCtrlLog.msg[0] != '\0') {
                    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1077, " by administrator <%s>. %s.\n")),
                           log->eventLog.hostCtrlLog.userName, log->eventLog.hostCtrlLog.msg); /* catgets  1077  */
                } else {
                    sprintf(prline, (_i18n_msg_get(ls_catd,NL_SETN,1077, " by administrator <%s>.\n")), log->eventLog.hostCtrlLog.userName); /* catgets  1077  */
                }
            } else {
                if (log->eventLog.hostCtrlLog.msg[0] != '\0') {
                    sprintf(prline, ". %s.\n", log->eventLog.hostCtrlLog.msg);
                } else {
                    sprintf(prline, ".\n");
                }
            }

            prtLine(prline);
	}
        break;

    default:
	break;
    }
}

static int
isRequested(char *name, char **nameList)
{
    int  i = 0;

    if (!nameList)
	return(TRUE);

    while (nameList[i]) {
	if (strcmp(name, nameList[i++]) == 0)
	    return(TRUE);
    }

    return(FALSE);
}

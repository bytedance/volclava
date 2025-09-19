/* 
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: cmd.bhc.c 397 2007-11-26 19:04:00Z mblack $
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
#include <ctype.h>

#define NL_SETN 8


extern char *myGetOpt (int nargc, char **nargv, char *ostr);
extern int getConfirm(char *);

static void ctrlHost (char *, int, int, char *);
static int  doConfirm (int, char *);
static int exitrc;
static char *opStr;
static struct hostInfoEnt *getHostList(int *numHosts, char **inputHosts);

int
bhc (int argc, char *argv[], int opCode)
{
    struct hostInfoEnt *hostInfo ;
    char **hostPoint ;
    char **hosts=NULL;
    char *optName;
    int i;
    int fFlag = FALSE;
    int  all = FALSE, numHosts = 0;
    int  inquerFlag = FALSE;
    char message[MAXLINELEN];

    memset(message, 0, MAXLINELEN);
    while ((optName = myGetOpt(argc, argv, "f|C:")) != NULL) {
        switch (optName[0]) {
        case 'f':
            fFlag = TRUE;
            break;
        case 'C':
            strncpy(message, optarg, MAXLINELEN-1);
            if ( strstr(message, "\n") ) {
                fprintf(stderr, "Error: The comment contains an illegal newline character (\\n). Please remove line breaks and submit again.");
                return(-2);
            }
            break;
        default:
            return(-2);
        }
    }
    switch (opCode) {
    case HOST_OPEN :
        opStr = (_i18n_msg_get(ls_catd,NL_SETN,901, "Open")); /* catgets  901  */
        break;
    case HOST_CLOSE :
        opStr = (_i18n_msg_get(ls_catd,NL_SETN,902, "Close")); /* catgets  902  */
        break;
    case HOST_REBOOT :
        opStr = (_i18n_msg_get(ls_catd,NL_SETN,903, "Restart slave batch daemon on")); /* catgets  903  */
        break;
    case HOST_SHUTDOWN :
        opStr = (_i18n_msg_get(ls_catd,NL_SETN,904, "Shut down slave batch daemon on")); /* catgets  904  */
        break;
    default :
        fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,905, "Unknown operation code\n"))); /* catgets  905  */
        exit(-1);
    }

    exitrc = 0;
    numHosts = getNames (argc, argv, optind, &hosts, &all, "hostC");
    hostPoint = NULL;
    if (!numHosts && !all)
        numHosts = 1;
    else if (numHosts)
        hostPoint = hosts;


    if ((opCode == HOST_REBOOT || opCode == HOST_SHUTDOWN) &&
	!(numHosts == 0 && all)) {

	if ((hostInfo = getHostList(&numHosts, hostPoint)) == NULL)
	    return (-1);
    } else {
	if ((hostInfo = lsb_hostinfo (hostPoint, &numHosts)) == NULL) {
	    lsb_perror(NULL);
	    return (-1);
	}
    }

    if (!fFlag && all && (opCode == HOST_REBOOT || opCode == HOST_SHUTDOWN))
        inquerFlag = !doConfirm (opCode, NULL);

    for (i = 0; i < numHosts; i++) {
        if (strcmp(hostInfo[i].host, "lost_and_found") == 0
            && (opCode == HOST_REBOOT || opCode == HOST_SHUTDOWN)) {
            if (!all)
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,906, "<lost_and_found> is not a real host, ignored\n"))); /* catgets  906  */
            continue;
        }
        if (inquerFlag && !(doConfirm (opCode, hostInfo[i].host)))
            continue;

        fprintf(stderr, "%s <%s> ...... ", opStr, hostInfo[i].host);
        fflush(stderr);

        ctrlHost (hostInfo[i].host, hostInfo[i].hStatus, opCode, message);
    }
    return (exitrc);

}

static void
ctrlHost (char *host, int hStatus, int opCode, char *message)
{

    if (lsb_hostcontrol(host, opCode, message) < 0) {
        char i18nBuf[100];
	sprintf(i18nBuf,I18N_FUNC_FAILED,"Host control");
        lsb_perror (i18nBuf );
	exitrc = -1;
        return;
    }
    if (opCode == HOST_OPEN) {
        if (hStatus & (HOST_STAT_BUSY | HOST_STAT_WIND | HOST_STAT_LOCKED_MASTER
                       | HOST_STAT_LOCKED | HOST_STAT_FULL)) {
            fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,910, "done : host remains closed due to "))); /* catgets  910  */
            if (hStatus & HOST_STAT_LOCKED)
                fprintf (stderr, (_i18n_msg_get(ls_catd,NL_SETN,911, "being locked; "))); /* catgets  911  */
            else if (hStatus & HOST_STAT_LOCKED_MASTER)
                fprintf(stderr, (I18N(917, "being locked by master LIM;"))); /* catgets  917  */
            else if (hStatus & HOST_STAT_WIND)
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,912, "dispatch window; "))); /* catgets  912  */
            else if (hStatus & HOST_STAT_FULL)
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,913, "job limit; "))); /* catgets  913  */
            else if (hStatus & HOST_STAT_BUSY)
                fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,914, "load threshold; "))); /* catgets  914  */
            fprintf(stderr, " \n");
            return;
        }
    }

    fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,915, "done\n"))); /* catgets  915  */

}

static int
doConfirm (int opCode, char *host)
{
    char msg[MAXLINELEN];

    if (host == NULL)
        host = (_i18n_msg_get(ls_catd,NL_SETN,916, "all the hosts")); /* catgets  916  */

    sprintf(msg, "\n%s %s? [y/n] ", opStr, host);

    return (getConfirm(msg));

}

static struct hostInfoEnt *
getHostList(int *numHosts, char **inputHosts)
{
    static struct hostInfoEnt *hostInfo = NULL;
    int i;
    char *localHost;

    FREEUP(hostInfo);

    if ((hostInfo = (struct hostInfoEnt *) calloc(*numHosts + 1,
						  sizeof(struct hostInfoEnt)))
	== NULL) {
	perror("calloc");
	return (NULL);
    }

    if (inputHosts) {
	for (i = 0; i < *numHosts; i++)
	    hostInfo[i].host = inputHosts[i];
    } else {
	if ((localHost = ls_getmyhostname()) == NULL)
	    hostInfo[0].host = "localhost";
	else
	    hostInfo[0].host = localHost;
    }

    return hostInfo;
}


/* 
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: badmin.h 397 2007-11-26 19:04:00Z mblack $
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
#include "cmd.h"
#include "../../lsf/lsf.h"
#include "../../lsf/intlib/intlibout.h"
 
extern int bhc (int argc, char **argv, int opCode);
extern int bqc (int argc, char **argv, int opCode);
extern int breconfig (int argc, char **argv, int configFlag);
extern int sysHist(int argc, char **argv, int opCode);

extern void parseAndDo (char *cmdBuf, int (*func)());
extern int adminCmdIndex(char *cmd, char *cmdList[]);
extern void cmdsUsage (char *cmd, char *cmdList[], char *cmdInfo[]);
extern void oneCmdUsage (int i, char *cmdList[], char *cmdSyntax[]);
extern void cmdHelp (int argc, char **argv, char *cmdList[],
                     char *cmdInfo[], char *cmdSyntax[]);
extern char *myGetOpt (int nargc, char **nargv, char *ostr);
extern int getConfirm (char *msg);
extern int startup(int argc, char **argv, int opCode);

extern int optind;

static int opCodeList[] = {  0, 0, QUEUE_OPEN, QUEUE_CLOSED, QUEUE_ACTIVATE,
                             QUEUE_INACTIVATE, QUEUE_HIST, HOST_OPEN, 
                             HOST_CLOSE, HOST_REBOOT, HOST_SHUTDOWN, 0,  
                             HOST_HIST, MBD_HIST, SYS_HIST, MBD_DEBUG, 
			     MBD_TIMING, 0, SBD_DEBUG, SBD_TIMING,  
			     0, 0, 0 }; 

static char *cmdList[] = { 
#define BADMIN_RECONFIG  0
			   "reconfig", 
#define BADMIN_CKCONFIG  1
			   "ckconfig", 
#define BADMIN_QOPEN     2
		           "qopen",
#define BADMIN_QCLOSE    3
		           "qclose",
#define BADMIN_QACT      4
		           "qact",
#define BADMIN_QINACT    5
		           "qinact",
#define BADMIN_QHIST     6
		           "qhist",
#define BADMIN_HOPEN     7
		           "hopen",
#define BADMIN_HCLOSE    8
		           "hclose",
#define BADMIN_HREBOOT   9
		           "hrestart",
#define BADMIN_HSHUT    10
		           "hshutdown",
#define BADMIN_HSTARTUP 11
		           "hstartup",
#define BADMIN_HHIST    12
		           "hhist",
#define BADMIN_MBDHIST  13
		           "mbdhist",
#define BADMIN_HIST     14
		           "hist",
#define BADMIN_MBDDEBUG 15 
			   "mbddebug",
#define BADMIN_MBDTIME  16 
			   "mbdtime",
#define BADMIN_MBDRESTART 17 
			   "mbdrestart",
#define BADMIN_SBDDEBUG 18 
			   "sbddebug",
#define BADMIN_SBDTIME  19
			   "sbdtime",
#define BADMIN_HELP     20 
		           "help",
#define BADMIN_QES      21
		           "?",
#define BADMIN_QUIT     22
		           "quit",
                            NULL
		         };

static char *cmdSyntax[] = 
{
  "[-v] [-f]",
  "[-v]",
  "[-C \"comments\"] [ queue_name ... | all ]",
  "[-C \"comments\"] [ queue_name ... | all ]",
  "[-C \"comments\"] [ queue_name ... | all ]",
  "[-C \"comments\"] [ queue_name ... | all ]",
  "[-t time0,time1] [-f logfile_name] [ queue_name ...]",
  "[-C \"comments\"] [ host_name ... | host_group ... | all ]",
  "[-C \"comments\"] [ host_name ... | host_group ... | all ]",
  "[-f] [ host_name ... | all ]",
  "[-f] [ host_name ... | all ]",
  "[-f] [ host_name ... | all ]",
  "[-t time0,time1] [-f logfile_name] [ host_name ...]",
  "[-t time0,time1] [-f logfile_name]",
  "[-t time0,time1] [-f logfile_name]",
  "[-c class_name] [-l debug_level] [-f logfile_name] [-o]",
  "[-l timing_level] [-f logfile_name] [-o]",
  "[-C \"comments\"] [-v] [-f]",
  "[-c class_name] [-l debug_level] [-f logfile_name] [-o] [ host_name ...]",
  "[-l timing_level] [-f logfile_name] [-o] [ host_name ...]",
  "[ command ...]",
  "[ command ...]",
  "",                                                           
  "[-t time0,time1] [-f logfile_name]",
  NULL
};

#define NL_SETN  8

static char *cmdInfo[] = { 
	"Reconfigure the system", 	/* catgets 3101 */
        "Check configuration files",	/* catgets 3102 */
        "Open LSF queues",		/* catgets 3103 */
        "Close LSF queues",		/* catgets 3104 */
        "Activate LSF queues",	        /* catgets 3105 */
        "Inactivate LSF queues",	/* catgets 3106 */
        "Display the history of LSF queues",	/* catgets 3107 */
        "Open hosts",				/* catgets 3108 */
        "Close hosts",				/* catgets 3109 */
        "Restart slave batch daemon on hosts",	/* catgets 3110 */
        "Shut down slave batch daemon on hosts",	/* catgets 3111 */
	"Start up slave batch daemon on hosts",	/* catgets 3112 */
	"Display the history of hosts",		/* catgets 3113 */
        "Display the history of the master batch daemon",  /* catgets 3114 */
        "Display the history of queues, hosts and mbatchd",/* catgets 3115 */
        "Debug master batch daemon",		/* catgets 3116 */
        "Timing master batch daemon",	/* catgets 3117 */
	"Restart a new mbatchd",	/* catgets 3122 */
        "Debug slave batch daemon",	/* catgets 3118 */
        "Timing slave batch daemon",	/* catgets 3119 */
        "Get help on commands",		/* catgets 3120 */
        "Get help on commands",		/* catgets 3120 */
        "Quit",				/* catgets 3121 */
        NULL
};

#ifdef  I18N_COMPILE
static int cmdInfo_ID[] = { 
	3101, 3102, 3103, 3104, 3105, 3106, 3107, 3108, 3109, 3110, 
	3111, 3112, 3113, 3114, 3115, 3116, 3117, 3122,
	3118, 3119, 3120, 3120, 3121
};
#endif

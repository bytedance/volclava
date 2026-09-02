/* 
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: bparams.c 397 2007-11-26 19:04:00Z mblack $
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

#include "cmd.h"

#define NL_SETN 8 	

static void printLong (struct parameterInfo *);
static void printShort (struct parameterInfo *);
void 
usage (char *cmd)
{
    fprintf(stderr, I18N_Usage);
    fprintf(stderr, ": %s [-h] [-V] [-l]\n", cmd);
    exit(-1);
}

int 
main (int argc, char **argv)
{
    int cc;
    struct parameterInfo  *paramInfo;
    int longFormat;
    int rc;

    rc = _i18n_init ( I18N_CAT_MIN );	



    if (lsb_init(argv[0]) < 0) {
	lsb_perror("lsb_init");
	exit(-1);
    }

    longFormat = FALSE;

    while ((cc = getopt(argc, argv, "Vhl")) != EOF) {
        switch (cc) {
        case 'l':
            longFormat = TRUE;
            break;
	case 'V':
	    fputs(_LS_VERSION_, stdout);
	    exit(0);
        case 'h':
        default:
            usage(argv[0]);
        }
    }
    if (!(paramInfo = lsb_parameterinfo (NULL, NULL, 0))) {
	lsb_perror(NULL);
        exit (-1);
    }
    if (longFormat)
        printLong (paramInfo);
    else
	printShort (paramInfo);

    _i18n_end ( ls_catd );			
    exit(0);

} 

static void
printShort (struct parameterInfo *reply)
{
    printf ("%s:  %s\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2402, "Default Queues")), reply->defaultQueues); /* catgets  2402  */
    if (reply->defaultHostSpec[0] != '\0')
	printf ("%s:  %s\n", 
	    _i18n_msg_get(ls_catd,NL_SETN,2403, "Default Host Specification"), /* catgets  2403  */
	    reply->defaultHostSpec); 
    printf ((_i18n_msg_get(ls_catd,NL_SETN,2404, "Job Dispatch Interval:  %d seconds\n")), reply->mbatchdInterval); /* catgets  2404  */
    printf ((_i18n_msg_get(ls_catd,NL_SETN,2405, "Job Checking Interval:  %d seconds\n")), reply->sbatchdInterval); /* catgets  2405  */
    printf ((_i18n_msg_get(ls_catd,NL_SETN,2406, "Job Accepting Interval:  %d seconds\n")), /* catgets  2406  */
                         reply->jobAcceptInterval * reply->mbatchdInterval);
} 

static void
printLong (struct parameterInfo *reply)
{

    printf ("\n%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2408, "System default queues for automatic queue selection"))); /* catgets  2408  */
    printf (" %16.16s = %s\n\n",  "DEFAULT_QUEUE", reply->defaultQueues); 

    if (reply->defaultHostSpec[0] != '\0') {
	printf ("%s:\n", 
	    (_i18n_msg_get(ls_catd,NL_SETN,2410, "System default host or host model for adjusting CPU time limit"))); /* catgets  2410  */
	printf (" %20.20s = %s\n\n",  "DEFAULT_HOST_SPEC", 
		reply->defaultHostSpec);
    }

    printf ("%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2412, "The interval for dispatching jobs by master batch daemon"))); /* catgets  2412  */
    printf ("    MBD_SLEEP_TIME = %d (%s)\n\n", reply->mbatchdInterval,
	   I18N_seconds);

    printf ("%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2414, "The interval for checking jobs by slave batch daemon"))); /* catgets  2414  */
    printf ("    SBD_SLEEP_TIME = %d (%s)\n\n", reply->sbatchdInterval,
	    I18N_seconds); 

    printf ("%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2416, "The interval for a host to accept two batch jobs"))); /* catgets  2416  */
    printf ("    JOB_ACCEPT_INTERVAL = %d (* MBD_SLEEP_TIME)\n\n", 
            reply->jobAcceptInterval);

    if (lsbMode_ & LSB_MODE_BATCH) {
	printf ("%s:\n", 
	    (_i18n_msg_get(ls_catd,NL_SETN,2423, "The idle time of a host for resuming pg suspended jobs"))); /* catgets  2423  */
	printf ("    PG_SUSP_IT = %d (%s)\n\n", reply->pgSuspendIt,
	     I18N_seconds);
    }
    
    printf ("%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2425, "The amount of time during which finished jobs are kept in core"))); /* catgets  2425  */
    printf ("    CLEAN_PERIOD = %d (%s)\n\n", reply->cleanPeriod,
	  I18N_seconds); 

    printf ("%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2427, "The maximum number of finished jobs that can be stored in current events file"))); /* catgets  2427  */
    printf ("    MAX_JOB_NUM = %d\n\n", reply->maxNumJobs); 

    printf ("%s:\n", 
	(_i18n_msg_get(ls_catd,NL_SETN,2431, "The maximum number of retries for reaching a slave batch daemon"))); /* catgets  2431  */
    printf ("    MAX_SBD_FAIL = %d\n\n", reply->maxSbdRetries); 

    if (lsbMode_ & LSB_MODE_BATCH) {
	char *temp = NULL;

	printf ("%s.\n", 
	    (_i18n_msg_get(ls_catd,NL_SETN,2435, "The default project assigned to jobs"))); /* catgets  2435  */
	printf ("    %15s = %s\n\n", "DEFAULT_PROJECT", reply->defaultProject); 

	printf("%s.\n", 
	    (_i18n_msg_get(ls_catd,NL_SETN,2437, "The interval to terminate a job"))); /* catgets  2437  */
	printf ("    JOB_TERMINATE_INTERVAL = %d \n\n", 
		reply->jobTerminateInterval);

	printf("%s.\n", 
	    (_i18n_msg_get(ls_catd,NL_SETN,2439, "The maximum number of jobs in a job array"))); /* catgets  2439  */
	printf ("    MAX_JOB_ARRAY_SIZE = %d\n\n", reply->maxJobArraySize); 
	
	if (reply->disableUAcctMap == TRUE)
	    temp = putstr_(_i18n_msg_get(ls_catd,NL_SETN,2442, "disabled")); /* catgets  2442 */
	else
	    temp = putstr_(_i18n_msg_get(ls_catd,NL_SETN,2443, "permitted")); /* catgets  2443 */

	printf("%s %s.\n\n", 
	    I18N(2441, "User level account mapping for remote jobs is"), /* catgets  2441  */
	    temp);

        FREEUP(temp);

    }

    
    if (strlen(reply->pjobSpoolDir) > 0) {
	printf ("\n%s:\n",
	       (_i18n_msg_get(ls_catd,NL_SETN,2445, 
		"The batch jobs' temporary output directory"))); /* catgets  2445  */
		printf ("    JOB_SPOOL_DIR = %s\n\n", reply->pjobSpoolDir); 
    }

    if ( reply->maxUserPriority > 0 ) {
        printf("%s \n", I18N(2445, 
	    "Maximal job priority defined for all users:")); /* catgets 2445 */
        printf("    MAX_USER_PRIORITY = %d\n", reply->maxUserPriority);
	printf("%s: %d\n\n", 
  	     I18N(2446, "The default job priority is"), /* catgets 2446 */
	     reply->maxUserPriority/2);
    }

    if ( reply->jobPriorityValue > 0) {
	printf("%s.\n", I18N(2447,
	     "Job priority is increased by the system dynamically based on waiting time")); /* catgets 2447 */
	printf("    JOB_PRIORITY_OVER_TIME = %d/%d (%s)\n\n", 
	    reply->jobPriorityValue, reply->jobPriorityTime, I18N_minutes);
    }

    if (reply->sharedResourceUpdFactor > 0){
        printf("%s:\n", I18N(2478,
               "Static shared resource update interval for the cluster")) /* catgets 2478 */;
        printf("    SHARED_RESOURCE_UPDATE_FACTOR  = %d \n\n",reply->sharedResourceUpdFactor);
    }

    if (reply->jobDepLastSub == 1) {
        printf("%s:\n", I18N(2464,"Used with job dependency scheduling")) /*
 catgets 2464 */;
        printf("    JOB_DEP_LAST_SUB = %d \n\n", reply->jobDepLastSub);
    }

	
    printf("%s:\n", I18N(2422, "The Maximum JobId defined in the system" /* catgets 2422 */));
    printf(I18N(2426, "    MAX_JOBID = %d\n\n"), /* catgets 2426 */ reply->maxJobId);

    
    if (reply->maxAcctArchiveNum>0 ) {
        printf("%s:\n", I18N(2428,"Max number of Acct files")); /* catgets 2428 */
        printf(" %24s = %d\n\n", "MAX_ACCT_ARCHIVE_FILE", reply->maxAcctArchiveNum);    
    }

    
    if (reply->acctArchiveInDays>0 ) {
        printf("%s:\n", I18N(2415, "Mbatchd Archive Interval")); /* catgets 2415 */
        printf(" %19s = %d %s \n\n", "ACCT_ARCHIVE_AGE", reply->acctArchiveInDays, I18N(2417, "days")); /* catgets 2417 */
    }

    
    if (reply->acctArchiveInSize>0 ) {
        printf("%s:\n", I18N(2424,"Mbatchd Archive threshold")); /* catgets 2424 */
        printf(" %20s = %d %s\n\n", "ACCT_ARCHIVE_SIZE", reply->acctArchiveInSize, I18N(2419, "kB")); /* catgets 2419 */
    }

    if (reply->subTryInterval > 0) {
        printf("%s:\n", I18N(2451,"Job submission retry interval")); /* catgets 2451 */
        printf("    %s = %d %s\n\n", "SUB_TRY_INTERVAL", reply->subTryInterval, I18N(2452, "seconds")); /* catgets 2452 */
    }

    if (reply->maxPendJobs > 0 && reply->maxPendJobs != INFINIT_INT) {
        printf("%s:\n", I18N(2453,"System wide max pending jobs")); /* catgets 2453 */
        printf(" %20s = %d\n\n", "MAX_PEND_JOBS", reply->maxPendJobs);
    }

    if (reply->maxPendSlots > 0 && reply->maxPendSlots != INFINIT_INT) {
        printf("%s:\n", I18N(2454,"System wide max pending slots")); /* catgets 2454 */
        printf(" %20s = %d\n\n", "MAX_PEND_SLOTS", reply->maxPendSlots);
    }

    printf("%s:\n", I18N(2424,"the limits of 'default' user set in lsb.users would ignore user group")); /* catgets 2424 */
    if (reply->defaultLimitIgnoreUserGroup == TRUE) {
        printf(" %34s = TRUE\n\n", "DEFAULT_LIMIT_IGNORE_USER_GROUP"); /* catgets 2419 */
    } else {
        printf(" %34s = FALSE\n\n", "DEFAULT_LIMIT_IGNORE_USER_GROUP"); /* catgets 2419 */
    }

    printf("%s:\n", I18N(2455,"Mbatchd reserves resources based on job tasks")); /* catgets 2455 */
    printf(" %28s = %s\n\n", "RESOURCE_RESERVE_PER_TASK", reply->resourcePerTask?"Y":"N");

    printf("%s:\n", I18N(2456, "Run time weighting factor for fairshare scheduling policy")); /* catgets 2456 */
    printf("    %s = %f\n\n", "RUN_TIME_FACTOR", reply->runTimeFactor);

    printf("%s:\n", I18N(2457, "CPU time weighting factor for fairshare scheduling policy")); /* catgets 2457 */
    printf("    %s = %f\n\n", "CPU_TIME_FACTOR", reply->cpuTimeFactor); 

    printf("%s:\n", I18N(2458, "Job slots weighting factor for fairshare scheduling policy")); /* catgets 2458 */
    printf("    %s = %f\n\n", "RUN_JOB_FACTOR", reply->runJobFactor);

    printf("%s:\n", I18N(2459, "The number of hours of resource consumption history:")); /* catgets 2459 */
    printf("    %s = %f\n\n", "HIST_HOURS", reply->runJobFactor);

    if (reply->defaultJobCwd != NULL && reply->defaultJobCwd[0] != '\0') {
        printf("%s\n", "The default CWD directory:");
        printf("    DEFAULT_JOB_CWD = %s\n\n", reply->defaultJobCwd);
    }

    if (reply->jobCwdTtl != INFINIT_INT) {
        printf("%s\n", "The time-to-live of the job CWD directory:");
        printf("    JOB_CWD_TTL = %d (hours)\n\n", reply->jobCwdTtl);
    }
} 



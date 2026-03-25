/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.err.c 397 2007-11-26 19:04:00Z mblack $
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

#include "lsb.h"

#define  NL_SETN   13
int lsberrno = 0;

#ifdef  I18N_COMPILE
static int lsb_errmsg_ID[] = {
     100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
     110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
     120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
     130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
     140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
     150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
     160, 161, 162, 163, 164, 165, 166, 167, 168, 169,
     170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
     180, 181, 182, 183, 184, 185, 186, 187, 188, 189,
     190, 191, 192, 193, 194, 195, 196, 197, 198, 199,
     200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
     210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
     220, 221, 222, 223, 224, 225, 226, 227, 228, 229,
     230, 231, 232, 233
};
#endif

char   *lsb_errmsg[] = {
/*00*/   "No error",                                       /* catgets 100 */
/*01*/   "No matching job found", 			   /* catgets 101 */
/*02*/   "Job has not started yet",                        /* catgets 102 */
/*03*/   "Job has already started",                        /* catgets 103 */
/*04*/   "Job has already finished",                       /* catgets 104 */
/*05*/   "Error 5", /* no message needed */                /* catgets 105 */
/*06*/   "Dependency condition syntax error",              /* catgets 106 */
/*07*/   "Queue does not accept EXCLUSIVE jobs",           /* catgets 107 */
/*08*/   "Root job submission is disabled",                /* catgets 108 */
/*09*/   "Job is already being migrated",                  /* catgets 109 */
/*10*/   "Job is not checkpointable",	                   /* catgets 110 */
/*11*/   "No output so far",                               /* catgets 111 */
/*12*/   "No job Id can be used now",                      /* catgets 112 */
/*13*/   "Queue only accepts interactive jobs", /* unused    catgets 113 */
/*14*/	 "Queue does not accept interactive jobs", /* unused catgets 114 */

/*15*/   "No user is defined in the lsb.users file",       /* catgets 115 */
/*16*/   "Unknown user",                               	   /* catgets 116 */
/*17*/   "User permission denied", 			   /* catgets 117 */
/*18*/   "No such queue", 				   /* catgets 118 */
/*19*/   "Queue name must be specified", 		   /* catgets 119 */
/*20*/   "Queue has been closed", 			   /* catgets 120 */
/*21*/   "Not activated because queue windows are closed", /* catgets 121 */
/*22*/   "User cannot use the queue", 			   /* catgets 122 */
/*23*/   "Bad host name, host group name or cluster name", /* catgets 123 */
/*24*/   "Too many processors requested",    		   /* catgets 124 */
/*25*/   "Reserved for future use",    		           /* catgets 125 */
/*26*/   "Reserved for future use",    		           /* catgets 126 */
/*27*/   "No user/host group defined in the system",       /* catgets 127 */
/*28*/   "No such user/host group", 			   /* catgets 128 */
/*29*/   "Host or host group is not used by the queue",    /* catgets 129 */
/*30*/   "Queue does not have enough per-user job slots",  /* catgets 130 */
/*31*/   "Current host is more suitable at this time",     /* catgets 131 */
/*32*/   "Checkpoint log is not found or is corrupted",    /* catgets 132 */
/*33*/   "Queue does not have enough per-processor job slots", /* catgets 133 */
/*34*/   "Request from non-LSF host rejected",  	   /* catgets 134 */


/*35*/   "Bad argument", 			    	   /* catgets 135 */
/*36*/   "Bad time specification", 			   /* catgets 136 */
/*37*/   "Start time is later than termination time", 	   /* catgets 137 */
/*38*/   "Bad CPU limit specification",			   /* catgets 138 */
/*39*/   "Cannot exceed queue's hard limit(s)", 	   /* catgets 139 */
/*40*/   "Empty job", 					   /* catgets 140 */
/*41*/   "Signal not supported", 			   /* catgets 141 */
/*42*/   "Bad job name", 			           /* catgets 142 */
/*43*/   "The destination queue has reached its job limit", /* catgets 143 */

/*44*/   "Unknown event", 				   /* catgets 144 */
/*45*/   "Bad event format", 				   /* catgets 145 */
/*46*/   "End of file",					   /* catgets 146 */

/*47*/   "Master batch daemon internal error",             /* catgets 147 */
/*48*/   "Slave batch daemon internal error",   	   /* catgets 148 */
/*49*/   "Batch library internal error",                   /* catgets 149 */
/*50*/   "Failed in an LSF library call",                  /* catgets 150 */
/*51*/   "System call failed",    			   /* catgets 151 */
/*52*/   "Cannot allocate memory",                         /* catgets 152 */
/*53*/   "Batch service not registered",                   /* catgets 153 */
/*54*/   "LSB_SHAREDIR not defined",              	   /* catgets 154 */
/*55*/   "Checkpoint system call failed",		   /* catgets 155 */
/*56*/   "Batch daemon cannot fork",  			   /* catgets 156 */

/*57*/   "Batch protocol error", 			   /* catgets 157 */
/*58*/   "XDR encode/decode error", 			   /* catgets 158 */
/*59*/   "Fail to bind to an appropriate port number",     /* catgets 159 */
/*60*/   "Contacting batch daemon: Communication timeout", /* catgets 160 */
/*61*/   "Timeout on connect call to server",		   /* catgets 161 */
/*62*/   "Connection refused by server",		   /* catgets 162 */
/*63*/   "Server connection already exists", 		   /* catgets 163 */
/*64*/   "Server is not connected",			   /* catgets 164 */
/*65*/   "Unable to contact execution host",		   /* catgets 165 */
/*66*/   "Operation is in progress",			   /* catgets 166 */
/*67*/   "User or one of user's groups does not have enough job slots", /* catgets 167 */

/*68*/   "Job parameters cannot be changed now; non-repetitive job is running", /* catgets 168 */
/*69*/   "Modified parameters have not been used",         /* catgets 169 */
/*70*/   "Job cannot be run more than once", 		   /* catgets 170 */
/*71*/   "Unknown cluster name or cluster master", 	   /* catgets 171 */
/*72*/   "Modified parameters are being used", 		   /* catgets 172 */

/*73*/   "Queue does not have enough per-host job slots",  /* catgets 173 */
/*74*/   "Mbatchd could not find the message that SBD mentions about",/* catgets 174 */
/*75*/   "Bad resource requirement syntax", 		   /* catgets 175 */
/*76*/   "Not enough host(s) currently eligible",   	   /* catgets 176 */
/*77*/   "Error 77",  /* internal only; no need to print message */ /* catgets 177 */
/*78*/   "Error 78",  /* internal only; no need to print message */  /* catgets 178 */
/*79*/   "No resource defined", 			   /* catgets 179 */
/*80*/   "Bad resource name", 				   /* catgets 180 */
/*81*/   "Interactive job cannot be rerunnable",	   /* catgets 181 */
/*82 */   "Input file not allowed with pseudo-terminal",   /* catgets 182 */
/*83*/   "Cannot find restarted or newly submitted job's submission host and host type", /* catgets 183 */
/*84*/   "Error 109",  /* internal only; no need to print message */ /* catgets 184 */
/*85*/   "User not in the specified user group",  	   /* catgets 185 */
/*86*/   "Cannot exceed queue's resource reservation",    /* catgets 186 */
/*87*/   "Bad host specification",                        /* catgets 187 */
/*88*/   "Bad user group name", 			   /* catgets 188 */
/*89*/   "Request aborted by esub", 			   /* catgets 189 */
/*90*/   "Bad or invalid action specification",           /* catgets 190 */
/*91*/   "Has dependent jobs", 			   /* catgets 191 */
/*92*/   "Job group does not exist", 			   /* catgets 192 */
/*93*/   "Bad/empty job group name",                      /* catgets 193 */
/*94*/   "Cannot operate on job array", 		   /* catgets 194 */
/*95*/   "Operation not supported for a suspended job",   /* catgets 195 */
/*96*/   "Operation not supported for a forwarded job",   /* catgets 196 */
/*97*/   "Job array index error", 			   /* catgets 197 */
/*98*/   "Job array index too large", 			   /* catgets 198 */
/*99*/   "Job array does not exist", 			   /* catgets 199 */
/*100*/   "Job exists", 			  	   /* catgets 200 */
/*101*/   "Cannot operate on element job",		   /* catgets 201 */
/*102*/   "Bad jobId", 					   /* catgets 202 */
/*103*/   "Change job name is not allowed for job array",  /* catgets 203 */
/*104*/   "Child process died",                            /* catgets 204 */
/*105*/   "Invoker is not in specified project group",     /* catgets 205 */
/*106*/   "No host group defined in the system", 	   /* catgets 206 */
/*107*/   "No user group defined in the system", 	   /* catgets 207 */
/*108*/   "Unknown jobid index file format", 	           /* catgets 208 */

/*109*/   "Source file for spooling does not exist",       /* catgets 209 */
/*110*/   "Number of failed spool hosts reached max",      /* catgets 210 */
/*111*/   "Spool copy failed for this host",               /* catgets 211 */
/*112*/   "Fork for spooling failed",                      /* catgets 212 */
/*113*/   "Status of spool child is not available",        /* catgets 213 */
/*114*/   "Spool child terminated with failure",           /* catgets 214 */
/*115*/   "Unable to find a host for spooling",            /* catgets 215 */
/*116*/   "Cannot get $JOB_SPOOL_DIR for this host",       /* catgets 216 */
/*117*/   "Cannot delete spool file for this host",        /* catgets 217 */

/*118*/   "Bad user priority", 			           /* catgets 218 */
/*119*/   "Job priority control undefined",                /* catgets 219 */
/*120*/   "Job has already been requeued",                /* catgets 220 */

/*121*/   "Multiple first execution hosts specified",      /* catgets 221 */
/*122*/   "Host group specified as first execution host",  /* catgets 222 */
/*123*/   "Host partition specified as first execution host",  /* catgets 223 */
/*124*/   "\"Others\" specified as first execution host",  /* catgets 224 */

/*125*/   "Too few processors requested", /* catgets 225 */
/*126*/   "Only the following parameters can be used to modify a running job: -c, -M, -W, -o, -e, -r", /* catgets 226 */
/*127*/   "You must set LSB_JOB_CPULIMIT in lsf.conf to modify the CPU limit of a running job", /* catgets 227 */
/*128*/   "You must set LSB_JOB_MEMLIMIT in lsf.conf to modify the memory limit of a running job", /*catgets 228 */
/*129*/   "No error file specified before job dispatch. Error file does not exist, so error file name cannot be changed", /*catgets 229 */
/*130*/   "The host is locked by master LIM", /* catgets 230 */
/*131*/  "Dependent arrays do not have the same size", /* catgets 231 */
/*132*/  "Pending job threshold reached", /* catgets 232 */
/*133*/  "Pending job slots threshold reached", /* catgets 233 */
/*134*/  "Bad rlimit specification, valid value 0<= n < 2T with LSF_UNIT_FOR_LIMITS in lsf.conf",			   /* catgets 234 */
/*135*/  "Checkpoint method cannot be changed with bmod",			   /* catgets 235 */
/*136*/  "No command is specified",			                           /* catgets 236 */
/*137*/  "-Zs is not supported for embeded job command",			   /* catgets 237 */
/*138*/  "Set job mac label failed",			                       /* catgets 238 */
/*139*/  "mbatchd did not handled this job",			               /* catgets 239 */

/* when you add a new message here, remember two things: first do not
 * forget to add "," after the error message; second, add its catgets
 * id in the above array lsb_errmsg_ID[].
 */
NULL
};

char *
lsb_sysmsg (void)
{
    static char buf[512];

    if (lsberrno > LSBE_NUM_ERR) {
        sprintf(buf, _i18n_msg_get(ls_catd, NL_SETN, 99, "Unknown batch system error number %d"), lsberrno);  /* catgets 99 */
        return buf;
    }

    if (lsberrno == LSBE_SYS_CALL) {

	if (strerror(errno) != NULL && errno > 0)
	    sprintf(buf, "%s: %s", _i18n_msg_get(ls_catd, NL_SETN, lsb_errmsg_ID[lsberrno], lsb_errmsg[lsberrno]), strerror(errno));
	else {
	    char *temp;
	    temp = putstr_(_i18n_msg_get(ls_catd, NL_SETN,
                lsb_errmsg_ID[lsberrno], lsb_errmsg[lsberrno]));
	    sprintf(buf, "%s:%s %d", temp, _i18n_msg_get(ls_catd, NL_SETN, 98,
		"unknown system error"), /* catgets 98 */
		errno);
            free(temp);
	}
    } else if (lsberrno == LSBE_LSLIB) {
 	sprintf(buf, "%s: %s", _i18n_msg_get(ls_catd, NL_SETN, lsb_errmsg_ID[lsberrno], lsb_errmsg[lsberrno]), ls_sysmsg());
    } else {
        return(_i18n_msg_get(ls_catd, NL_SETN, lsb_errmsg_ID[lsberrno],lsb_errmsg[lsberrno]));
    }
    return buf;
}

void
lsb_perror (char *usrMsg)
{
    if (usrMsg) {
        fputs(usrMsg, stderr);
        fputs(": ", stderr);
    }
    fputs(lsb_sysmsg(), stderr);
    putc('\n', stderr);

}

char *
lsb_sperror(char *usrMsg)
{
    char errmsg[256];
    char *rtstr;

    errmsg[0] = '\0';

    if (usrMsg)
        sprintf(errmsg, "%s: ", usrMsg);

    strcat(errmsg, lsb_sysmsg());

    if ((rtstr=(char *)malloc(sizeof(char)*(strlen(errmsg)+1))) == NULL){
        lserrno = LSE_MALLOC;
        return NULL;
    }

    strcpy(rtstr, errmsg);
    return rtstr;
}



void
sub_perror (char *usrMsg)
{
    if (usrMsg) {
        fputs(usrMsg, stderr);
        fputs(": ", stderr);
    }
    fputs(lsb_sysmsg(), stderr);

}



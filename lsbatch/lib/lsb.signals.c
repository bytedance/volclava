/* $Id: lsb.signals.c 397 2007-11-26 19:04:00Z mblack $
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
#include <string.h>
#include <pwd.h>
#include "lsb.h"

static int lsbSig_map[] =     {
                       SIG_NULL,
                       SIG_CHKPNT,
                       SIG_CHKPNT_COPY,
                       SIG_DELETE_JOB,

                       SIG_SUSP_USER,
                       SIG_SUSP_LOAD,
                       SIG_SUSP_WINDOW,
                       SIG_SUSP_OTHER,

                       SIG_RESUME_USER,
                       SIG_RESUME_LOAD,
                       SIG_RESUME_WINDOW,
                       SIG_RESUME_OTHER,

                       SIG_TERM_USER,
                       SIG_TERM_LOAD,
                       SIG_TERM_WINDOW,
                       SIG_TERM_OTHER,
                       SIG_TERM_RUNLIMIT,
                       SIG_TERM_DEADLINE,
                       SIG_TERM_PROCESSLIMIT,
                       SIG_TERM_FORCE,
		               SIG_KILL_REQUEUE,
                       SIG_TERM_CPULIMIT,
		               SIG_TERM_MEMLIMIT,
                       SIG_NULL,
                       SIG_TERM_SWAPLIMIT
                    };

static char *lsbSigSymbol[] = {
                            "SIG_NULL",
                            "SIG_CHKPNT",
                            "SIG_CHKPNT_COPY",
                            "SIG_DELETE_JOB",

                            "SIG_SUSP_USER",
                            "SIG_SUSP_LOAD",
                            "SIG_SUSP_WINDOW",
                            "SIG_SUSP_OTHER",

                            "SIG_RESUME_USER",
                            "SIG_RESUME_LOAD",
                            "SIG_RESUME_WINDOW",
                            "SIG_RESUME_OTHER",

                            "SIG_TERM_USER",
                            "SIG_TERM_LOAD",
                            "SIG_TERM_WINDOW",
                            "SIG_TERM_OTHER",
                            "SIG_TERM_RUNLIMIT",
                            "SIG_TERM_DEADLINE",
                            "SIG_TERM_PROCESSLIMIT",
                            "SIG_TERM_FORCE",
		       	            "SIG_KILL_REQUEUE",
                            "SIG_TERM_CPULIMIT",
		       	            "SIG_TERM_MEMLIMIT",
			                "SIG_NULL",
                            "SIG_TERM_SWAPLIMIT"
                     };


static int defaultSigValue [] = {
                             SIG_NULL,
                             SIG_CHKPNT,
                             SIG_CHKPNT_COPY,
                             SIG_DELETE_JOB,

                             SIGSTOP,
                             SIGSTOP,
                             SIGSTOP,
                             SIGSTOP,

                             SIGCONT,
                             SIGCONT,
                             SIGCONT,
                             SIGCONT,

                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
                             SIGKILL,
			                 SIGKILL,
                             SIG_NULL,
                             SIGKILL
};


int
sigNameToValue_ (char *sigString)
{
    int i, sigValue;

    if ((sigString == NULL) || (sigString[0] == '\0'))
        return (INFINIT_INT);

    
    if ((sigValue = getSigVal(sigString)) > 0)
        return (sigValue);

    
    for (i=0; i<LSB_SIG_NUM; i++)
        if (strcmp(lsbSigSymbol[i], sigString) == 0)
            return (lsbSig_map[i]);

    return (INFINIT_INT);      

}


char *
getLsbSigSymbol ( int sigValue)
{
    static char symbol[30];

    symbol[0] = '\0';

    if (sigValue >=0) {
        return ( (char *) getSigSymbol(sigValue));     
    } else {                                            
        if ( -sigValue <  LSB_SIG_NUM )
            strcpy(symbol, lsbSigSymbol[-sigValue]);
        else 
            strcpy(symbol, "UNKNOWN");
        return (symbol);
    }
}

int
getDefSigValue_( int sigValue, char *actCmd)
{
    int defSigValue;

    if (sigValue >= 0)
        return (sigValue);

    switch (sigValue) {
        case SIG_CHKPNT:
        case SIG_CHKPNT_COPY:
        case SIG_DELETE_JOB:
            return (sigValue);

        case SIG_SUSP_USER:   
        case SIG_SUSP_LOAD:
        case SIG_SUSP_WINDOW:
        case SIG_SUSP_OTHER:

        case SIG_RESUME_USER:
        case SIG_RESUME_LOAD:
        case SIG_RESUME_WINDOW:
        case SIG_RESUME_OTHER:

        case SIG_TERM_USER:
        case SIG_TERM_LOAD:
        case SIG_TERM_WINDOW:
        case SIG_TERM_OTHER:
        case SIG_TERM_RUNLIMIT:
        case SIG_TERM_DEADLINE:
        case SIG_TERM_PROCESSLIMIT:
        case SIG_TERM_CPULIMIT:
        case SIG_TERM_MEMLIMIT:
        case SIG_TERM_SWAPLIMIT:
        case SIG_TERM_FORCE:
            if ((actCmd == NULL) || ( actCmd[0] == '\0'))
                
                return (defaultSigValue [-sigValue]);
            else 
                if ((defSigValue = sigNameToValue_ (actCmd)) == INFINIT_INT)
                    return (sigValue);   
                else {
                    if ((defSigValue == SIG_CHKPNT)
                       || (defSigValue == SIG_CHKPNT_COPY))  
                        return (sigValue);   
                    else  
                        return (defSigValue); 
                }
    }
    return (sigValue);

} 

int
isSigTerm (int sigValue)
{
    switch (sigValue) {
        case SIG_DELETE_JOB:
        case SIG_TERM_USER:
        case SIG_TERM_LOAD:
        case SIG_TERM_WINDOW:
        case SIG_TERM_OTHER:
        case SIG_TERM_RUNLIMIT:
        case SIG_TERM_DEADLINE:
        case SIG_TERM_PROCESSLIMIT:
        case SIG_TERM_CPULIMIT:
        case SIG_TERM_MEMLIMIT:
        case SIG_TERM_SWAPLIMIT:
        case SIG_TERM_FORCE:
        case SIG_KILL_REQUEUE: 
            return(TRUE);
        default:
            return (FALSE);
    }
} 

int
isSigSusp (int sigValue)
{
    switch (sigValue) {
       case SIG_SUSP_USER:
       case SIG_SUSP_LOAD:
       case SIG_SUSP_WINDOW:
       case SIG_SUSP_OTHER:
            return(TRUE);
        default:
            return (FALSE);
    }
} 

int
terminateWhen_(int *sigMap, char *name)
{
    if (strcmp(name, "WINDOW") == 0) {
        if (sigMap[- SIG_SUSP_WINDOW] != 0)
            return (TRUE);
        else
            return (FALSE);
    } else if (strcmp(name, "USER") == 0) {
        if (sigMap[- SIG_SUSP_USER] != 0)
            return (TRUE);
        else
            return (FALSE);
    } else if (strcmp(name, "LOAD") == 0) {
        if (sigMap[- SIG_SUSP_LOAD] != 0)
            return (TRUE);
        else
            return (FALSE);
    } else
        return (FALSE);

} 

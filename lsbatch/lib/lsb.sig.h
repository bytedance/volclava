/* $Id: lsb.sig.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef LSB_SIG_H
#define LSB_SIG_H

#include "../lsbatch.h"

#define SIG_NULL             -65535  
#define SIG_CHKPNT               -1  
#define SIG_CHKPNT_COPY          -2  
#define SIG_DELETE_JOB           -3  

#define  SIG_SUSP_USER           -4  
#define  SIG_SUSP_LOAD           -5  
#define  SIG_SUSP_WINDOW         -6  
#define  SIG_SUSP_OTHER          -7  

#define  SIG_RESUME_USER         -8  
#define  SIG_RESUME_LOAD         -9 
#define  SIG_RESUME_WINDOW       -10 
#define  SIG_RESUME_OTHER        -11  

#define  SIG_TERM_USER           -12 
#define  SIG_TERM_LOAD           -13  
#define  SIG_TERM_WINDOW         -14  
#define  SIG_TERM_OTHER          -15   
#define  SIG_TERM_RUNLIMIT       -16  
#define  SIG_TERM_DEADLINE       -17  
#define  SIG_TERM_PROCESSLIMIT   -18  
#define  SIG_TERM_FORCE          -19 
#define  SIG_KILL_REQUEUE        -20  
#define  SIG_TERM_CPULIMIT       -21  
#define  SIG_TERM_MEMLIMIT       -22  
#define  SIG_ARRAY_REQUEUE       -23
#define  SIG_TERM_SWAPLIMIT      -24

extern int sigNameToValue_ (char *sigString);
extern char *getLsbSigSymbol ( int sigValue);
extern int getDefSigValue_( int sigValue, char *actCmd);
extern int isSigTerm (int sigValue);
extern int isSigSusp (int sigValue);
extern int terminateWhen_(int *sigMap, char *name);

#endif 

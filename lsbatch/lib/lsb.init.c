/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.init.c 397 2007-11-26 19:04:00Z mblack $
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

#include <netdb.h>
#include "lsb.h"

struct config_param lsbParams[] = {
     {"LSB_DEBUG", NULL},
     {"LSB_SHAREDIR", NULL},
     {"LSB_SBD_PORT", NULL},
     {"LSB_MBD_PORT", NULL},
     {"LSB_DEBUG_CMD", NULL},
     {"LSB_TIME_CMD", NULL},
     {"LSB_CMD_LOGDIR", NULL},
     {"LSB_CMD_LOG_MASK", NULL}, 
     {"LSF_LOG_MASK", NULL},
     {"LSB_API_CONNTIMEOUT",NULL},
     {"LSB_API_RECVTIMEOUT",NULL},
     {"LSF_SERVERDIR", NULL},
     {"LSB_MODE", NULL},
     {"LSB_SHORT_HOSTLIST", NULL},
     {"LSF_INTERACTIVE_STDERR", NULL}, 
     {"LSB_32_PAREN_ESC", NULL},
     {"LSB_API_QUOTE_CMD", NULL},
     {"LSF_UNIT_FOR_LIMITS", NULL},
     {"LSB_QMBD_PORT", NULL},
     {NULL, NULL}
};

#ifdef LSF_LOG_MASK
#undef LSF_LOG_MASK
#endif
#define LSF_LOG_MASK   8

int _lsb_conntimeout = DEFAULT_API_CONNTIMEOUT;
int _lsb_recvtimeout = DEFAULT_API_RECVTIMEOUT;
int _lsb_fakesetuid = 0;

int lsbMode_ = LSB_MODE_BATCH;
unitTypes lsbUnitForLimits = Megabytes;

extern int bExceptionTabInit(void);
extern int mySubUsage_(void *);

int 
lsb_init (char *appName)
{
    static int lsbenvset = FALSE;
    char *logMask;

    if (lsbenvset)
        return 0;                           

    
    if (initenv_(lsbParams, NULL) < 0)
    {
	lsberrno = LSBE_LSLIB;
	return(-1);
    }

    if (lsbParams[LSB_API_CONNTIMEOUT].paramValue) {
	
	_lsb_conntimeout = atoi(lsbParams[LSB_API_CONNTIMEOUT].paramValue);
	if (_lsb_conntimeout < 0) 
	   _lsb_conntimeout = DEFAULT_API_CONNTIMEOUT;
    }

    if (lsbParams[LSB_API_RECVTIMEOUT].paramValue) {
	
	_lsb_recvtimeout = atoi(lsbParams[LSB_API_RECVTIMEOUT].paramValue); 
	if (_lsb_recvtimeout < 0) 
	   _lsb_recvtimeout = DEFAULT_API_RECVTIMEOUT;
    }

    if (! lsbParams[LSB_SHAREDIR].paramValue) {
	lsberrno = LSBE_NO_ENV;
	return(-1);
    }

    lsbenvset = TRUE;

    if (lsbParams[LSB_CMD_LOG_MASK].paramValue != NULL)
        logMask = lsbParams[LSB_CMD_LOG_MASK].paramValue;
    else
        logMask = lsbParams[LSF_LOG_MASK].paramValue;

    if (appName == NULL)
        ls_openlog ("bcmd", lsbParams[LSB_CMD_LOGDIR].paramValue,
           (lsbParams[LSB_CMD_LOGDIR].paramValue == NULL), logMask);
    else 
        ls_openlog (appName, lsbParams[LSB_CMD_LOGDIR].paramValue,
               (lsbParams[LSB_CMD_LOGDIR].paramValue == NULL), logMask);
     
    getLogClass_(lsbParams[LSB_DEBUG_CMD].paramValue,
                 lsbParams[LSB_TIME_CMD].paramValue);

    if (lsbParams[LSB_UNIT_FOR_LIMITS].paramValue != NULL) {
        strToUpper_(lsbParams[LSB_UNIT_FOR_LIMITS].paramValue);
        lsbUnitForLimits = setUnitForLimits(lsbParams[LSB_UNIT_FOR_LIMITS].paramValue);
    }

    if (bExceptionTabInit()) {
	lsberrno = LSBE_LSBLIB;
	return(-1);
    }

    if (lsb_catch("LSB_BAD_BSUBARGS", mySubUsage_))
	return(-1);

    return(0);

} 


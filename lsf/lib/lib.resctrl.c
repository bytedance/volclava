/* $Id: lib.resctrl.c 397 2007-11-26 19:04:00Z mblack $
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

#include "lib.h"
#include <unistd.h>
#include "lib.xdr.h"
#include "lproto.h"

int
ls_rescontrol(char *host, int opCode, int data)
{
    int       s;
    int       descriptor[2];
    int       cc;
    struct    resControl ctrl;
    struct {
	struct LSFHeader    hdr;
	struct resControl   c;
    } buf;
    struct timeval    timeout;

    
    if (genParams_[LSF_RES_TIMEOUT].paramValue) {
	timeout.tv_sec  = atoi(genParams_[LSF_RES_TIMEOUT].paramValue);
    } else {
	timeout.tv_sec = LSF_RES_TIMEOUT;
    }

    timeout.tv_usec = 0;

    if (_isconnected_(host, descriptor))
	s = descriptor[0];
    else if ((s = ls_connect(host)) < 0)
	return(-1);

    if (!FD_ISSET(s,&connection_ok_)){
	FD_SET(s,&connection_ok_);

	
	cc = rd_poll_(s, &timeout);
	if (cc <= 0) {
	    closesocket(s);
	    _lostconnection_(host);
	    lserrno =  LSE_TIME_OUT;
	    return(-1);
	}
	
	if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(host);
	    return (-1);
        }
    }

    if (opCode    != RES_CMD_REBOOT 
	&& opCode != RES_CMD_SHUTDOWN  
	&& opCode != RES_CMD_LOGON 
	&& opCode != RES_CMD_LOGOFF) {
        lserrno = LSE_BAD_OPCODE;
	return (-1);
    }

    ctrl.opCode = opCode;
    ctrl.data   = data;

    if (callRes_(s, RES_CONTROL, (char *) &ctrl, (char *) &buf,
		 sizeof(buf), xdr_resControl, 0, 0, NULL) == -1) {
	closesocket(s);
	_lostconnection_(host);
	return( -1 );
    }

    
    cc = rd_poll_(s, &timeout);
    if (cc <= 0) {
	closesocket(s);
	_lostconnection_(host);
	lserrno =  LSE_TIME_OUT;
	return(-1);
    }
    
    if (ackReturnCode_(s) < 0) {
	closesocket(s);
	_lostconnection_(host);
	return( -1 );
    }

    return(0);
} 


int
oneResDebug (struct debugReq  *pdebug , char *hostname)
{
    int s, descriptor[2];
    char space[ ]=" ";
    struct debugReq debugData;

    struct {
	struct LSFHeader hdr;
	struct debugReq d; 
     } buf;

     if (_isconnected_(hostname, descriptor))
	 s = descriptor[0];
     else if ((s = ls_connect(hostname)) < 0)
	 return(-1);

      if (!FD_ISSET(s,&connection_ok_)){
	  FD_SET(s,&connection_ok_);
	  if (ackReturnCode_(s) < 0) {
	      closesocket(s);
	      _lostconnection_(hostname);
	      return (-1);
            }
        }

       debugData.opCode = pdebug->opCode;
       debugData.logClass = pdebug->logClass;
       debugData.level    = pdebug->level;
       debugData.hostName = space;
       debugData.options  = pdebug->options;
       strcpy(debugData.logFileName, pdebug->logFileName);

       if (callRes_(s, RES_DEBUGREQ, (char *) &debugData, (char *) &buf,
			sizeof(buf), xdr_debugReq, 0, 0, NULL) == -1) {
           closesocket(s);
	   _lostconnection_(hostname);
	   return( -1 );
	  }

	if (ackReturnCode_(s) < 0) {
	    closesocket(s);
	    _lostconnection_(hostname);
	    return( -1 );
	 }

    return(0);
} 




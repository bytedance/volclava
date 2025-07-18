/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: lsb.hc.c 397 2007-11-26 19:04:00Z mblack $
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
#include <sys/types.h>
#include <netdb.h>
#include <string.h>
#include <pwd.h>

#include "lsb.h"

int 
lsb_hostcontrol (char *host, int opCode, char *message)
{
    XDR xdrs;
    char request_buf[MSGSIZE];
    char *reply_buf, *contactHost = NULL;
    static struct controlReq hostControlReq;
    int cc;
    struct LSFHeader hdr;
    struct lsfAuth auth;

    
    if (hostControlReq.name == NULL) {
        hostControlReq.name = (char *) malloc (MAXHOSTNAMELEN);
        if (hostControlReq.name == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(-1);
        }
    }
    if (opCode != HOST_OPEN && opCode != HOST_CLOSE &&
        opCode != HOST_REBOOT && opCode != HOST_SHUTDOWN) {
        lsberrno = LSBE_BAD_ARG;
        return (-1);
    }
    if (host) 
        if (strlen (host) >= MAXHOSTNAMELEN - 1) {
            lsberrno = LSBE_BAD_ARG;
            return (-1);
        }
    
    hostControlReq.opCode = opCode;
    if (host)
	strcpy(hostControlReq.name, host);
    else {
	char *h;
        if ((h = ls_getmyhostname()) == NULL) {
            lsberrno = LSBE_LSLIB;
            return(-1);
        }
	strcpy(hostControlReq.name, h);
    }
    
    switch (opCode) {
    case HOST_REBOOT:
	hdr.opCode = CMD_SBD_REBOOT;
        contactHost = host;
	break;
    case HOST_SHUTDOWN:
	hdr.opCode = CMD_SBD_SHUTDOWN;
        contactHost = host;
	break;
    default:
	hdr.opCode = BATCH_HOST_CTRL;
	break;
    }
  
    hostControlReq.msg = message; 
    
    if (authTicketTokens_(&auth, contactHost) == -1)
	return (-1);

    
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);

    if (!xdr_encodeMsg(&xdrs, (char*) &hostControlReq, &hdr, 
		      xdr_controlReq, 0, &auth)) {
        lsberrno = LSBE_XDR;
        return(-1);
    }

    if (opCode == HOST_REBOOT || opCode == HOST_SHUTDOWN) {
	
	if ((cc = cmdCallSBD_(hostControlReq.name, request_buf,
			      XDR_GETPOS(&xdrs), &reply_buf, 
			      &hdr, NULL)) == -1)
	    return (-1);
    } else {
	
	if ((cc = callmbd (NULL, request_buf, XDR_GETPOS(&xdrs), &reply_buf, 
			   &hdr, NULL, NULL, NULL)) == -1)
	    return (-1);
    }
	

    lsberrno = hdr.opCode;
    if (cc)
	free(reply_buf);
    if (lsberrno == LSBE_NO_ERROR)
	return(0);
    else
	return(-1);

} 

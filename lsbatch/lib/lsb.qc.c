/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: lsb.qc.c 397 2007-11-26 19:04:00Z mblack $
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

int 
lsb_queuecontrol (char *queue, int opCode, char * message)
{
    static struct controlReq qcReq;
    mbdReqType mbdReqtype;
    XDR xdrs;
    char request_buf[MSGSIZE];
    char *reply_buf;
    int cc;
    struct LSFHeader hdr;
    struct lsfAuth auth;

    if (qcReq.name == NULL) {
        qcReq.name = (char *) malloc (MAXHOSTNAMELEN);
        if (qcReq.name == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(-1);
        }
    }
    

    if (authTicketTokens_(&auth, NULL) == -1)
        return (-1);

    
    switch (opCode) {
    case QUEUE_OPEN:
        qcReq.opCode = QUEUE_OPEN;
        break;
    case QUEUE_CLOSED:
        qcReq.opCode = QUEUE_CLOSED;
        break;
    case QUEUE_ACTIVATE:
        qcReq.opCode = QUEUE_ACTIVATE;
        break;
    case QUEUE_INACTIVATE:
        qcReq.opCode = QUEUE_INACTIVATE;
        break;
    default:
        lsberrno = LSBE_BAD_ARG;
        return (-1);
    }

    if (queue == NULL) {
        lsberrno = LSBE_QUEUE_NAME;
        return(-1);
    } else {
        if (strlen (queue) >= MAXHOSTNAMELEN - 1) {
            lsberrno = LSBE_BAD_QUEUE;
            return (-1);
        }
        strcpy(qcReq.name, queue);
    }

    qcReq.msg = message;
    

    mbdReqtype = BATCH_QUE_CTRL;
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);
    initLSFHeader_(&hdr);
    hdr.opCode = mbdReqtype;
    if (!xdr_encodeMsg(&xdrs, (char *)&qcReq, &hdr, xdr_controlReq, 0, &auth)) {
        lsberrno = LSBE_XDR;
        return(-1);
    }

    
    if ((cc = callmbd (NULL, request_buf, XDR_GETPOS(&xdrs), &reply_buf, 
                       &hdr, NULL, NULL, NULL)) == -1)
    {
        xdr_destroy(&xdrs); 
        return (-1);
    }

    xdr_destroy(&xdrs); 

    

    lsberrno = hdr.opCode;
    if (cc)
        free(reply_buf);    
    if (lsberrno == LSBE_NO_ERROR)
        return(0);

    return(-1);

}

/* $Id: lsb.queues.c 397 2007-11-26 19:04:00Z mblack $
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
#include <sys/time.h>
#include <sys/resource.h>
#include <string.h>

#include "lsb.h"

struct queueInfoEnt *
lsb_queueinfo (char **queues, int *numQueues, char *hosts, char *users, int options)
{
    return lsb_queueinfo_fields(queues, numQueues, hosts, users, options,
                                NULL);
}

struct queueInfoEnt *
lsb_queueinfo_fields(char **queues, int *numQueues, char *hosts, char *users,
                     int options, const char *outputFields)
{
    mbdReqType mbdReqtype;
    int useOutput, retried = 0;
    static struct infoReq queueInfoReq;         
    static struct queueInfoReply reply;
    static struct queueInfoReply outputReply;
    static struct queueInfoEnt **qInfo = NULL;
    struct queueInfoEnt **qTmp;
    XDR xdrs, xdrs2;
    char *request_buf;
    char *reply_buf;
    int cc, i;
    size_t outputFieldsLength;
    size_t requestSize;
    static struct LSFHeader hdr;
    char *clusterName = NULL;

    
    if (qInfo != NULL) {
	for (i = 0; i < reply.numQueues; i++) {
	    xdr_lsffree(xdr_queueInfoEnt,
	 	        (char*)qInfo[i],
			    &hdr);
	}
    }

    if (numQueues == NULL) {
        lsberrno = LSBE_BAD_ARG;
        return NULL;
    }
    if ((queues == NULL && *numQueues > 1) || (*numQueues < 0)) {
        lsberrno = LSBE_BAD_ARG;
        return NULL;
    }

    queueInfoReq.options = 0;                

    if (queueInfoReq.names) {
        FREEUP (queueInfoReq.names);           
    }

    if (numQueues == NULL || *numQueues == 0)
        queueInfoReq.options |= ALL_QUEUE;
    else if (queues == NULL && *numQueues == 1)
        queueInfoReq.options |= DFT_QUEUE;

    if ((queueInfoReq.options & ALL_QUEUE) 
         || (queueInfoReq.options & DFT_QUEUE)) {
        if ((queueInfoReq.names = (char **)malloc (3 * sizeof(char *))) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(NULL);
        }
        queueInfoReq.names[0] = "";
	queueInfoReq.numNames = 1;
        cc = 1;                                            
    }
    else {
        if ((queueInfoReq.names = (char **)calloc 
                                   (*numQueues + 2, sizeof(char*))) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(NULL);
        }
        queueInfoReq.numNames = *numQueues;
        for (i = 0; i < *numQueues; i++) {
            if (queues[i] && strlen (queues[i]) + 1 < MAXHOSTNAMELEN) 
                queueInfoReq.names[i] = queues[i];
            else {
                free (queueInfoReq.names);
                queueInfoReq.names = NULL;
                lsberrno = LSBE_BAD_QUEUE;
                *numQueues = i;
                return (NULL);
            }
        }
        cc = queueInfoReq.numNames;                 
    }
    if (users != NULL) {
        if (strlen (users) + 1 < MAX_LSB_NAME_LEN) {
            queueInfoReq.options |= CHECK_USER;
            queueInfoReq.names[cc] = users;
            cc++;
        } else {
            lsberrno = LSBE_BAD_USER;
            *numQueues = 0;
            return (NULL);
        }
    }

    if (hosts != NULL) {
        if (ls_isclustername(hosts) <= 0) {          
            if (strlen (hosts) + 1 < MAXHOSTNAMELEN) {
                queueInfoReq.options |= CHECK_HOST;
                queueInfoReq.names[cc] = hosts;
                cc++;
            } else {
                lsberrno = LSBE_BAD_HOST;
                *numQueues = 0;
                return (NULL);
            }
        } else                       
            clusterName = hosts;
    }
    queueInfoReq.resReq = "";
    queueInfoReq.outputFields = (char *)(outputFields ? outputFields : "");

    

    

    useOutput = outputFields && outputFields[0];
    mbdReqtype = useOutput ? BATCH_OUTPUT : BATCH_QUE_INFO;
    outputFieldsLength = strlen(queueInfoReq.outputFields);
    requestSize = sizeof(struct infoReq)
                  + (size_t)cc * (MAXHOSTNAMELEN + 1)
                  + outputFieldsLength + strlen(queueInfoReq.resReq) + 116;
    if (requestSize > UINT_MAX
        || (request_buf = malloc(requestSize)) == NULL) {
        lsberrno = LSBE_NO_MEM;
        return(NULL);
    }
retryOutput:
    mbdReqtype = useOutput ? BATCH_OUTPUT : BATCH_QUE_INFO;
    xdrmem_create(&xdrs, request_buf, (u_int)requestSize, XDR_ENCODE);
    initLSFHeader_(&hdr); 
    hdr.opCode = mbdReqtype;
    hdr.reserved = useOutput ? OUTPUT_QUEUE : 0;
    if (!xdr_encodeMsgVersion(&xdrs, (char*) &queueInfoReq, &hdr, xdr_infoReq,
		              0, NULL,
                              useOutput ? _VOLCLAVA_VERSION2_3_
                                        : _VOLCLAVA_VERSION2_2_)) {
        lsberrno = LSBE_XDR;
        xdr_destroy(&xdrs);
        free (request_buf);
        return(NULL);
    }

    
    if ((cc = callmbd(clusterName, request_buf, XDR_GETPOS(&xdrs), &reply_buf, 
                      &hdr, NULL, NULL, NULL)) == -1)
    {
        xdr_destroy(&xdrs);
        free (request_buf);
	return (NULL);
    }

    if (useOutput && !retried &&
        (hdr.opCode == LSBE_PROTOCOL ||
         (hdr.opCode == LSBE_NO_ERROR && hdr.version < _VOLCLAVA_VERSION2_3_))) {
        if (cc) free(reply_buf);
        reply_buf = NULL;
        xdr_destroy(&xdrs);
        useOutput = 0;
        retried = 1;
        goto retryOutput;
    }
    xdr_destroy(&xdrs);
    free (request_buf);

    
    lsberrno = hdr.opCode;
    if (lsberrno == LSBE_NO_ERROR || lsberrno == LSBE_BAD_QUEUE) {
        if (useOutput) {
            xdr_lsffree(xdr_queueOutputReply, (char *)&outputReply, &hdr);
            memset(&outputReply, 0, sizeof(outputReply));
            xdrmem_create(&xdrs2, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);
            i = xdr_queueOutputReply(&xdrs2, &outputReply, &hdr);
            xdr_destroy(&xdrs2);
            free(reply_buf);
            if (!i) {
                xdr_lsffree(xdr_queueOutputReply, (char *)&outputReply, &hdr);
                lsberrno = LSBE_XDR;
                return NULL;
            }
            if (lsberrno == LSBE_BAD_QUEUE) {
                *numQueues = outputReply.badQueue;
                return NULL;
            }
            *numQueues = outputReply.numQueues;
            return outputReply.queues;
        }
	xdrmem_create(&xdrs2, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);	
        if (!xdr_queueInfoReply(&xdrs2, &reply, &hdr)) {
	    lsberrno = LSBE_XDR;
            xdr_destroy(&xdrs2);
	    if (cc)
		free(reply_buf);
	    *numQueues = 0;
	    return(NULL);
        }
        xdr_destroy(&xdrs2);
	if (cc)
	    free(reply_buf);
        if (lsberrno == LSBE_BAD_QUEUE) {
            *numQueues = reply.badQueue;
            return (NULL);
        }
	if ((qTmp = (struct queueInfoEnt **)myrealloc(qInfo,
		reply.numQueues*sizeof(struct queueInfoEnt *))) == NULL) {
            lsberrno = LSBE_NO_MEM;
	    return NULL;
        }
	qInfo = qTmp;
        for (i = 0; i < reply.numQueues; i++)
            qInfo[i] = &(reply.queues[i]);
	
        *numQueues = reply.numQueues;
	return(qInfo[0]);
    }

    if (cc)
	free(reply_buf);
    *numQueues = 0;
    return(NULL);

} 

/* $Id: lsb.hosts.c 397 2007-11-26 19:04:00Z mblack $
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

#include "lsb.h"
struct hostInfoEnt * getHostStatus (struct hostDataReply reply);



struct hostInfoEnt * 
lsb_hostinfo (char **hosts, int *numHosts)
{
    return (lsb_hostinfo_ex (hosts, numHosts, NULL, 0));
} 


struct hostInfoEnt * 
lsb_hostinfo_ex (char **hosts, int *numHosts, char *resReq, int options)
{
    return lsb_hostinfo_ex_fields(hosts, numHosts, resReq, options, NULL);
}

struct hostInfoEnt *
lsb_hostinfo_ex_fields(char **hosts, int *numHosts, char *resReq, int options,
                       const char *outputFields)
{
    mbdReqType mbdReqtype;
    int useOutput, retried = 0;
    XDR xdrs;
    struct LSFHeader hdr;
    char *request_buf;
    char *reply_buf;
    int cc, i, numReq = -1;
    size_t outputFieldsLength;
    size_t requestSize;
    char *clusterName =NULL;
    static struct infoReq hostInfoReq;        
    struct hostDataReply reply;
    static struct hostDataReply outputReply;

    if (numHosts) {
        numReq = *numHosts;                     
        *numHosts = 0;                          
    }
    if (numReq < 0) {
	lsberrno = LSBE_BAD_ARG;
	return(NULL);
    }

    FREEUP (hostInfoReq.names);              
    if (numHosts == NULL || numReq == 0) {      
        hostInfoReq.numNames = 0;
        if ((hostInfoReq.names = (char **) malloc (sizeof(char *))) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(NULL);
        }

        hostInfoReq.numNames = 0;          

        
        if (resReq != NULL) {
            TIMEIT(0, (hostInfoReq.names[0] = ls_getmyhostname()), "ls_getmyhostname");
            options |= CHECK_HOST;
        }
        else {
            hostInfoReq.names[0] = "";
        }

        cc = 1;                                         
    }
    else if (numReq == 1 && hosts == NULL) {    
        hostInfoReq.numNames = 1;
        if ((hostInfoReq.names = (char **) malloc (sizeof(char *))) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(NULL);
        }
        TIMEIT(0, (hostInfoReq.names[0] = ls_getmyhostname()), "ls_getmyhostname");
        hostInfoReq.numNames = 1;          
        cc = 1;                                         
    }
    else {                                 
        if ((hostInfoReq.names = (char **) calloc
                                 (numReq, sizeof(char*))) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return(NULL);
        }
        for (i = 0; i < numReq; i++) { 
            if (ls_isclustername(hosts[i]) <= 0)
                continue;
            hostInfoReq.names[0] = "";
            hostInfoReq.numNames = 0;
            clusterName = hosts[i];
            cc = 1;
            break;
        }
        if (clusterName == NULL) {
            for (i = 0; i < numReq; i++) {
                if (hosts[i] && strlen (hosts[i]) + 1 < MAXHOSTNAMELEN) 
          	        hostInfoReq.names[i] = hosts[i];
                else {
                    free (hostInfoReq.names);
                    lsberrno = LSBE_BAD_HOST;
                    *numHosts = i;
                    return (NULL);
                }
            }
            cc = numReq;                               
            hostInfoReq.numNames = numReq;          
        }
    }

    hostInfoReq.options = options;
    if (resReq != NULL) {
	if (strlen(resReq) > MAXLINELEN -1) {
	    lsberrno = LSBE_BAD_RESREQ;
	    return(NULL);
        } else {
	    hostInfoReq.resReq = resReq;
	    hostInfoReq.options |= SORT_HOST;
        }
    } else
	  hostInfoReq.resReq = "";

    hostInfoReq.outputFields = (char *)(outputFields ? outputFields : "");

    useOutput = outputFields && outputFields[0];
    mbdReqtype = useOutput ? BATCH_OUTPUT : BATCH_HOST_INFO;
    outputFieldsLength = strlen(hostInfoReq.outputFields);
    requestSize = sizeof(struct infoReq)
                  + (size_t)cc * (MAXHOSTNAMELEN + 1)
                  + outputFieldsLength + strlen(hostInfoReq.resReq) + 116;
    if (requestSize > UINT_MAX
        || (request_buf = malloc(requestSize)) == NULL) {
        lsberrno = LSBE_NO_MEM;
        return(NULL);
    }
retryOutput:
    mbdReqtype = useOutput ? BATCH_OUTPUT : BATCH_HOST_INFO;
    xdrmem_create(&xdrs, request_buf, (u_int)requestSize, XDR_ENCODE);

    initLSFHeader_(&hdr);
    hdr.opCode = mbdReqtype;
    hdr.reserved = useOutput ? OUTPUT_HOST : 0;
    if (!xdr_encodeMsgVersion(&xdrs, (char *)&hostInfoReq, &hdr, xdr_infoReq,
		              0, NULL,
                              useOutput ? _VOLCLAVA_VERSION2_3_
                                        : _VOLCLAVA_VERSION2_2_)) {
        xdr_destroy(&xdrs);
        free (request_buf);
	lsberrno = LSBE_XDR;
	return(NULL);
    }

    
    TIMEIT(0, (cc = callmbd(clusterName, request_buf, XDR_GETPOS(&xdrs), &reply_buf, &hdr, NULL, NULL, NULL)), "callmbd");
    if (cc == -1) {
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
    if (lsberrno == LSBE_NO_ERROR || lsberrno == LSBE_BAD_HOST) {
        if (useOutput) {
            xdr_lsffree(xdr_hostOutputReply, (char *)&outputReply, &hdr);
            memset(&outputReply, 0, sizeof(outputReply));
            xdrmem_create(&xdrs, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);
            i = xdr_hostOutputReply(&xdrs, &outputReply, &hdr);
            xdr_destroy(&xdrs);
            free(reply_buf);
            if (!i) {
                xdr_lsffree(xdr_hostOutputReply, (char *)&outputReply, &hdr);
                lsberrno = LSBE_XDR;
                return NULL;
            }
            if (lsberrno == LSBE_BAD_HOST) {
                *numHosts = outputReply.badHost;
                return NULL;
            }
            *numHosts = outputReply.numHosts;
            return outputReply.hosts;
        }
	xdrmem_create(&xdrs, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);
	
        if(!xdr_hostDataReply(&xdrs, &reply, &hdr)) {
	    lsberrno = LSBE_XDR;
            xdr_destroy(&xdrs);
	    if (cc)
		free(reply_buf);
	    return(NULL);
        }
        xdr_destroy(&xdrs);
	if (cc)
	    free(reply_buf);
        if (lsberrno == LSBE_BAD_HOST) {
            *numHosts = reply.badHost;
            return (NULL);
        }
        *numHosts = reply.numHosts;
        return(reply.hosts);
    }

    if (cc)
	free(reply_buf);
    return(NULL);

} 

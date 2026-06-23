/*
 * Copyright (C) 2021-2026 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <unistd.h>
#include <string.h>

#include "../../lsf/lib/lib.h"
#include "lsb.h"

/********************************************************************************
 * lsb_rsrclimitinfo
 * Description:
 *     Query resource limit configurations and/or runtime usage from mbatchd.
 *
 * Input:
 *     options   [in]: RLIMIT_OPT_* bitmask
 *     numNames  [in]: number of limit name filters (0 = no filter)
 *     names     [in]: array of limit name filters (may be NULL if numNames==0)
 *     queue     [in]: queue filter (may be NULL)
 *     user      [in]: user filter (may be NULL)
 *     project   [in]: project filter (may be NULL)
 *     hosts     [in]: hosts filter (may be NULL)
 *     numLimits [out]: number of limits returned
 *
 * Return:
 *     Pointer to array of rlimitEnt on success, NULL on failure (lsberrno set).
 ********************************************************************************/
struct rlimitEnt *
lsb_rsrclimitinfo(int options, int numNames, char **names,
                 char *queue, char *user, char *project, char *hosts,
                 int *numLimits)
{
    static char fname[] = "lsb_rsrclimitinfo";
    static struct rsrcLimitInfoReply reply;
    struct rsrcLimitInfoReq req;
    int cc = 0, i;
    char *clusterName = NULL;
    static struct LSFHeader hdr;
    char *request_buf;
    char *reply_buf;
    mbdReqType mbdReqtype;
    XDR xdrs, xdrs2;

#define FREE_REPLY_BUFFER \
    { \
        if (cc) \
            free(reply_buf); \
    }

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if (reply.numLimits > 0)
        xdr_lsffree(xdr_rsrcLimitInfoReply, (char *)&reply, &hdr);

    if (numLimits == NULL ||
        numNames < 0 ||
        (names == NULL && numNames != 0) ||
        (names != NULL && numNames == 0)) {
        lsberrno = LSBE_BAD_ARG;
        return (NULL);
    }

    memset(&req, 0, sizeof(req));
    req.options = options;
    req.numNames = numNames;

    if (numNames > 0) {
        if ((req.names = (char **)calloc(numNames, sizeof(char *))) == NULL) {
            lsberrno = LSBE_NO_MEM;
            return (NULL);
        }
        for (i = 0; i < numNames; i++) {
            if (names[i] && strlen(names[i]) <= MAX_LSB_NAME_LEN) {
                req.names[i] = names[i];
                cc += MAX_LSB_NAME_LEN;
            } else {
                FREEUP(req.names);
                lsberrno = LSBE_BAD_ARG;
                return (NULL);
            }
        }
    }

    req.queue = (queue != NULL) ? queue : " ";
    req.user = (user != NULL) ? user : " ";
    req.project = (project != NULL) ? project : " ";
    req.hosts = (hosts != NULL) ? hosts : " ";
    cc += ALIGNWORD_(strlen(req.queue) + 1);
    cc += ALIGNWORD_(strlen(req.user) + 1);
    cc += ALIGNWORD_(strlen(req.project) + 1);
    cc += ALIGNWORD_(strlen(req.hosts) + 1);

    mbdReqtype = BATCH_RSRC_LIMIT_INFO;
    cc = sizeof(struct rsrcLimitInfoReq) + cc + 100;
    if ((request_buf = malloc(cc)) == NULL) {
        lsberrno = LSBE_NO_MEM;
        FREEUP(req.names);
        return (NULL);
    }
    xdrmem_create(&xdrs, request_buf, MSGSIZE, XDR_ENCODE);
    initLSFHeader_(&hdr);
    hdr.opCode = mbdReqtype;
    if (!xdr_encodeMsg(&xdrs, (char *)&req, &hdr, xdr_rsrcLimitInfoReq,
                       0, NULL)) {
        lsberrno = LSBE_XDR;
        xdr_destroy(&xdrs);
        FREEUP(req.names);
        FREEUP(request_buf);
        return (NULL);
    }

    if ((cc = callmbd(clusterName, request_buf, XDR_GETPOS(&xdrs), &reply_buf,
                      &hdr, NULL, NULL, NULL)) == -1) {
        xdr_destroy(&xdrs);
        FREEUP(req.names);
        FREEUP(request_buf);
        return (NULL);
    }
    FREEUP(req.names);
    xdr_destroy(&xdrs);
    FREEUP(request_buf);

    lsberrno = hdr.opCode;
    if (lsberrno == LSBE_NO_ERROR) {
        xdrmem_create(&xdrs2, reply_buf, XDR_DECODE_SIZE_(cc), XDR_DECODE);
        if (!xdr_rsrcLimitInfoReply(&xdrs2, &reply, &hdr)) {
            lsberrno = LSBE_XDR;
            xdr_destroy(&xdrs2);
            FREE_REPLY_BUFFER;
            return (NULL);
        }
        xdr_destroy(&xdrs2);
        FREE_REPLY_BUFFER;
        *numLimits = reply.numLimits;
        return (reply.limits);
    }

    FREE_REPLY_BUFFER;
    return (NULL);
}

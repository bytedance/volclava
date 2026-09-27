/*
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

#include <limits.h>
#include "lproto.h"
#include "lib.h"

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

static bool_t xdr_hostLoad (XDR *, struct hostLoad *, struct LSFHeader *, char *);
static bool_t xdr_placeInfo (XDR *, struct placeInfo *, struct LSFHeader *);
static bool_t xdr_shortLsInfo(XDR *, struct shortLsInfo *, struct LSFHeader *);
static bool_t xdr_resItem(XDR *, struct resItem *, struct LSFHeader *);
static void freeUpMemp (char *, int);
static bool_t xdr_lsResourceInfo (XDR *, struct  lsSharedResourceInfo *,
                                  struct LSFHeader *);
static bool_t xdr_lsResourceInstance (XDR *,
                                      struct  lsSharedResourceInstance *,
                                      struct LSFHeader *);
int sharedResConfigured_ = FALSE;

static bool_t
xdr_placeInfo(XDR *xdrs, struct placeInfo *placeInfo, struct LSFHeader *hdr)
{
    char *sp;

    sp = placeInfo->hostName;
    if (xdrs->x_op == XDR_DECODE)
        sp[0] = '\0';
    if (!(xdr_string(xdrs, &sp, MAXHOSTNAMELEN) &&
          xdr_int(xdrs, &placeInfo->numtask))) {
        return (FALSE);
    }
    return TRUE;
}

static bool_t
xdr_hostLoad (XDR *xdrs, struct hostLoad *loadVec, struct LSFHeader *hdr, char *cp)
{
    char *sp;
    int i;
    int *nIndicies = (int *)cp;

    sp = loadVec->hostName;
    if (xdrs->x_op == XDR_DECODE)
        sp[0] = '\0';
    if (!xdr_string(xdrs, &sp, MAXHOSTNAMELEN)) {
        return FALSE;
    }
    for (i = 0; i < 1 + GET_INTNUM (*nIndicies); i++) {
        if (!xdr_int(xdrs, (int *) &loadVec->status[i]))
            return(FALSE);
    }
    for (i=0; i<*nIndicies; i++) {
        if ( !xdr_float(xdrs, &loadVec->li[i]))
            return (FALSE);
    }

    return(TRUE);
}

bool_t
xdr_decisionReq(XDR *xdrs, struct decisionReq *decisionReqPtr,
                struct LSFHeader *hdr)
{
    char *sp1 = decisionReqPtr->hostType;
    char *sp2 = decisionReqPtr->resReq;
    int hasOutputFields = hdr
                          && (hdr->opCode == LIM_GET_HOSTINFO || hdr->opCode == LIM_HOST_OUTPUT)
                          && hdr->version >= _VOLCLAVA_VERSION2_3_;
    int i;

    if (xdrs->x_op == XDR_FREE) {
        if (decisionReqPtr->preferredHosts) {
            for (i = 0; i < decisionReqPtr->numPrefs; i++)
                FREEUP(decisionReqPtr->preferredHosts[i]);
        }
        FREEUP(decisionReqPtr->preferredHosts);
        FREEUP(decisionReqPtr->outputFields);
        return TRUE;
    }

    if (xdrs->x_op == XDR_DECODE) {
        decisionReqPtr->resReq[0] = '\0';
        decisionReqPtr->hostType[0] = '\0';
        decisionReqPtr->outputFields = NULL;
    }

    if (!(xdr_enum(xdrs, (int *) &decisionReqPtr->ofWhat) &&
          xdr_int(xdrs,&decisionReqPtr->options) &&
          xdr_string(xdrs, &sp1, MAXLSFNAMELEN) &&
          xdr_int(xdrs, &decisionReqPtr->numHosts) &&
          xdr_string(xdrs, &sp2, MAXLINELEN) &&
          xdr_int(xdrs, &decisionReqPtr->numPrefs))) {
        return (FALSE);
    }


    if (xdrs->x_op == XDR_DECODE) {
        decisionReqPtr->preferredHosts =
            (char **) calloc(decisionReqPtr->numPrefs, sizeof (char *));
        if (decisionReqPtr->preferredHosts == NULL)
            return (FALSE);
    }

    if (! xdr_array_string(xdrs, decisionReqPtr->preferredHosts,
                           MAXHOSTNAMELEN, decisionReqPtr->numPrefs)) {
        if (xdrs->x_op == XDR_DECODE)
            FREEUP(decisionReqPtr->preferredHosts);
        return (FALSE);
    }
    if (hasOutputFields) {
        if (!xdr_var_string(xdrs, &decisionReqPtr->outputFields))
            return (FALSE);
    }

    return(TRUE);

}

bool_t
xdr_placeReply(XDR *xdrs, struct placeReply *placeRepPtr, struct LSFHeader *hdr)
{
    int i, status=FALSE;
    static  char  *memp;

    if (!xdr_int(xdrs,&placeRepPtr->numHosts)) {
        return(FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        if (memp)
            FREEUP(memp);
        placeRepPtr->placeInfo = (struct placeInfo *)
            malloc((int)placeRepPtr->numHosts * sizeof(struct placeInfo));
        if (!placeRepPtr->placeInfo)
            return(FALSE);
        memp = (char *) placeRepPtr->placeInfo;
    }


    for (i = 0; i < placeRepPtr->numHosts; i++) {
        status= xdr_arrayElement(xdrs,
                                 (char *)&placeRepPtr->placeInfo[i],
                                 hdr,
                                 xdr_placeInfo);
        if (!status) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }
    return(TRUE);
}

bool_t
xdr_loadReply(XDR *xdrs, struct loadReply *loadReplyPtr, struct LSFHeader *hdr)
{
    char *sp;
    int  i, status=TRUE;
    static char *memp;

    if (!(xdr_int(xdrs, &loadReplyPtr->nEntry)
          && xdr_int(xdrs, &loadReplyPtr->nIndex))) {
        return(FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        int matSize, hlSize, nameSize, vecSize, staSize;
        char *currp;

        FREEUP(memp);
        if (loadReplyPtr->indicies==NULL)
            loadReplyPtr->indicies=
                (char **)malloc(sizeof(char *)*(loadReplyPtr->nIndex+1));



        hlSize   = ALIGNWORD_(loadReplyPtr->nEntry * sizeof(struct hostLoad));
        vecSize  = loadReplyPtr->nIndex * sizeof(float);
        matSize  = ALIGNWORD_((loadReplyPtr->nEntry + 1) * vecSize);
        nameSize = ALIGNWORD_(loadReplyPtr->nIndex * MAXLSFNAMELEN);
        staSize = ALIGNWORD_((1 + GET_INTNUM(loadReplyPtr->nIndex))
                             * sizeof (int));
        loadReplyPtr->loadMatrix = (struct hostLoad *) malloc
            (hlSize + matSize + nameSize + loadReplyPtr->nEntry * staSize);
        if (!loadReplyPtr->loadMatrix) {
            memp = NULL;
            return(FALSE);
        }

        memp = (char *) loadReplyPtr->loadMatrix;
        currp = (char *)(memp + hlSize);
        for (i=0; i < loadReplyPtr->nEntry; i++, currp += vecSize)
            loadReplyPtr->loadMatrix[i].li = (float *)currp;
        currp = (char *)(memp + hlSize + matSize);
        for (i=0; i < loadReplyPtr->nIndex; i++, currp += MAXLSFNAMELEN)
            loadReplyPtr->indicies[i] = currp;
        for (i=0; i < loadReplyPtr->nEntry; i++, currp += staSize)
            loadReplyPtr->loadMatrix[i].status = (int *) currp;
    }

    for (i=0; i < loadReplyPtr->nIndex; i++) {
        sp = loadReplyPtr->indicies[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN))
            return(FALSE);
    }
    loadReplyPtr->indicies[i] = NULL;


    for (i = 0; i < loadReplyPtr->nEntry; i++) {
        status = xdr_arrayElement(xdrs,
                                  (char *)&loadReplyPtr->loadMatrix[i],
                                  hdr,
                                  xdr_hostLoad,
                                  (char *)&loadReplyPtr->nIndex);

        if (!status) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    if (!xdr_int(xdrs, &loadReplyPtr->flags))
        return FALSE;

    return(TRUE);
}

bool_t
xdr_jobXfer(XDR *xdrs, struct jobXfer *jobXferPtr, struct LSFHeader *hdr)
{
    int i, status=FALSE;
    char *sp;
    static  char  *memp;

    sp = jobXferPtr->resReq;
    if (xdrs->x_op == XDR_DECODE)
        sp[0] = '\0';
    if (!(xdr_int(xdrs,&jobXferPtr->numHosts) &&
          xdr_string(xdrs, &sp, MAXLINELEN))) {
        return (FALSE);
    }

    if (xdrs->x_op == XDR_DECODE) {
        if (memp)
            FREEUP(memp);
        jobXferPtr->placeInfo = (struct placeInfo *)
            malloc((int)jobXferPtr->numHosts * sizeof(struct placeInfo));
        if (!jobXferPtr->placeInfo) {
            lserrno = LSE_MALLOC;
            return(FALSE);
        }
        memp = (char *) jobXferPtr->placeInfo;
    }


    for (i = 0; i < jobXferPtr->numHosts; i++) {
        status= xdr_arrayElement(xdrs,
                                 (char *)&jobXferPtr->placeInfo[i],
                                 hdr,
                                 xdr_placeInfo);
        if (!status) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    return(TRUE);
}

bool_t
xdr_hostInfoReply(XDR *xdrs, struct hostInfoReply *hostInfoReply,
                  struct LSFHeader *hdr)
{
    int i, status;
    static struct shortHInfo *memp = NULL;

    if (!(xdr_int(xdrs, &hostInfoReply->nHost) &&
          xdr_int(xdrs, &hostInfoReply->nIndex)))
        return (FALSE);

    if (!xdr_shortLsInfo(xdrs, hostInfoReply->shortLsInfo, hdr)) {
        return(FALSE);
    }
    if (xdrs->x_op == XDR_DECODE) {
        int shISize, matSize, vecSize, resSize;
        char *currp;

        if (memp)
            FREEUP(memp);

        shISize = ALIGNWORD_(hostInfoReply->nHost * sizeof(struct shortHInfo));
        vecSize = ALIGNWORD_(hostInfoReply->nIndex * sizeof(float));
        resSize = (GET_INTNUM(hostInfoReply->shortLsInfo->nRes)
                   + GET_INTNUM(hostInfoReply->nIndex)) * sizeof(int);
        matSize = ALIGNWORD_(hostInfoReply->nHost * (vecSize + resSize
                                                     + MAXLINELEN + 100));

        hostInfoReply->hostMatrix = (struct shortHInfo *) malloc(shISize + matSize);
        if (hostInfoReply->hostMatrix == NULL)
            return (FALSE);

        memp =  hostInfoReply->hostMatrix;
        currp = (char*) memp + shISize;
        for (i=0; i < hostInfoReply->nHost; i++) {
            hostInfoReply->hostMatrix[i].busyThreshold =(float *) currp;
            currp += vecSize;
            hostInfoReply->hostMatrix[i].resBitMaps =(int *) currp;
            currp += resSize;
            hostInfoReply->hostMatrix[i].windows = (char*) currp;
            currp += MAXLINELEN;
        }
    }
    for (i=0; i<hostInfoReply->nHost;i++)  {
        status = xdr_arrayElement(xdrs,
                                  (char *)&hostInfoReply->hostMatrix[i],
                                  hdr, xdr_shortHInfo, (char *)&hostInfoReply->nIndex);
        if (!status) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return (FALSE);
        }
    }




    for (i=0; i<hostInfoReply->nHost;i++)  {
        if (!xdr_int(xdrs, &(hostInfoReply->hostMatrix[i].maxMem))) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return (FALSE);
        }
    }

    return (TRUE);

}

bool_t
xdr_shortHInfo(XDR *xdrs, struct shortHInfo *shortHInfo, struct LSFHeader *hdr,
               char *nIndex)
{
    char window[MAXLINELEN];
    char *sp;
    char *sp1 = window;
    u_short tIndx, mIndx, mem, cpus;
    unsigned  int  a, b;
    int i;
    int *nIndicies = (int *)nIndex;

    sp = shortHInfo->hostName;
    if (xdrs->x_op == XDR_DECODE) {
        sp[0] = '\0';
        sp1[0] = '\0';
    }

    if (xdrs->x_op == XDR_ENCODE) {
        if (shortHInfo->hTypeIndx >= 0) {

            tIndx = MIN( MAXTYPES, shortHInfo->hTypeIndx);
        } else {
            tIndx = MAXTYPES;
        }
        tIndx &= 0x7FFF;
        if (shortHInfo->windows[0] != '-')
            tIndx |= 0x8000;
        if (shortHInfo->hModelIndx >= 0) {

            mIndx = MIN(MAXMODELS, shortHInfo->hModelIndx);
        } else {
            mIndx = MAXMODELS;
        }
        a = tIndx << 16;
        a &= 0xffffffff;
        a = a + mIndx;
        mem = shortHInfo->maxMem;
        cpus = shortHInfo->maxCpus;
        b = mem << 16;
        b &= 0xffffffff;
        b = b + cpus;
        if (logclass & LC_TRACE)
            ls_syslog(LOG_DEBUG2, "xdr_shortHInfo: host <%s>  type = %d, model = %d",
                      shortHInfo->hostName, tIndx, mIndx);
    }

    if (!xdr_string(xdrs, &sp, MAXHOSTNAMELEN) ||
        !xdr_u_int(xdrs, &a) ||
        !xdr_u_int(xdrs, &b) ||
        !xdr_int(xdrs, &shortHInfo->resClass))
        return FALSE;


    if ((xdrs->x_op == XDR_ENCODE) && (shortHInfo->windows[0] != '-')) {
        sp1 = shortHInfo->windows;
        if (!xdr_string(xdrs, &sp1, MAXLINELEN))
            return FALSE;
    }

    if (xdrs->x_op == XDR_DECODE) {
        if ( (a >> 16) & 0x8000) {
            if (!xdr_string(xdrs, &sp1, MAXLINELEN))
                return FALSE;
        } else {
            sp1[0]='-';
            sp1[1]='\0';
        }
        strcpy (shortHInfo->windows, sp1);
        shortHInfo->hTypeIndx = (int) (a >> 16) & 0x7FFF;
        a = a << 16;

        a &= 0xffffffff;
        shortHInfo->hModelIndx = (int) (a >> 16);
        shortHInfo->maxMem     = (int) (b >> 16);
        b = b << 16;

        b &= 0xffffffff;
        shortHInfo->maxCpus     = (int) (b >> 16);
    }

    for(i=0; i < *nIndicies; i++)
        if (!xdr_float(xdrs, &shortHInfo->busyThreshold[i]))
            return FALSE;

    if ( !xdr_int(xdrs, &shortHInfo->flags) ||
         !xdr_int(xdrs, &shortHInfo->rexPriority) ||
         !xdr_int(xdrs, &shortHInfo->nDisks) ||
         !xdr_int(xdrs, &shortHInfo->maxSwap) ||
         !xdr_int(xdrs, &shortHInfo->maxTmp) )
        return FALSE;
    if (!xdr_int(xdrs, &shortHInfo->nRInt))
        return FALSE;
    for (i = 0; i < shortHInfo->nRInt; i++) {
        if (!xdr_int(xdrs, &shortHInfo->resBitMaps[i]))
            return (FALSE);
    }
    if (shortHInfo->flags & HINFO_SHARED_RESOURCE) {
        sharedResConfigured_ = TRUE;
    }

    return TRUE;
}

static bool_t
xdr_shortLsInfo( XDR *xdrs, struct shortLsInfo *shortLInfo, struct LSFHeader *hdr)
{
    int i;
    static char *memp, *currp;
    char *sp;


    if (!xdr_int(xdrs, &shortLInfo->nRes) ||
        !xdr_int(xdrs, &shortLInfo->nTypes) ||
        !xdr_int(xdrs, &shortLInfo->nModels))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE) {
        if (memp)
            free(memp);

        memp = malloc((shortLInfo->nRes + shortLInfo->nTypes +
                       shortLInfo->nModels) * MAXLSFNAMELEN +
                      shortLInfo->nRes *sizeof (char *));
        if (!memp) {
            return FALSE;
        }
        currp = memp;
        shortLInfo->resName = (char **)currp;
        currp += shortLInfo->nRes *sizeof (char *);
        for (i=0; i < shortLInfo->nRes; i++, currp += MAXLSFNAMELEN)
            shortLInfo->resName[i] = currp;
        for (i=0; i < shortLInfo->nTypes; i++, currp += MAXLSFNAMELEN)
            shortLInfo->hostTypes[i] = currp;
        for (i=0; i < shortLInfo->nModels; i++, currp += MAXLSFNAMELEN)
            shortLInfo->hostModels[i] = currp;
    }

    for(i=0; i < shortLInfo->nRes; i++) {
        sp = shortLInfo->resName[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN))
            return FALSE;
    }
    for(i=0; i < shortLInfo->nTypes; i++) {
        sp = shortLInfo->hostTypes[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN))
            return FALSE;
    }

    for(i=0; i < shortLInfo->nModels; i++) {
        sp = shortLInfo->hostModels[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN))
            return FALSE;
    }

    for(i=0; i < shortLInfo->nModels; i++)
        if (!xdr_float(xdrs, &shortLInfo->cpuFactors[i]))
            return FALSE;
    return TRUE;
}

bool_t
xdr_limLock(XDR *xdrs, struct limLock *limLockPtr, struct LSFHeader *hdr)
{
    char *sp;

    sp = limLockPtr->lsfUserName;
    if (xdrs->x_op == XDR_DECODE) {
        sp[0] = '\0';
    }

    if (!xdr_int(xdrs, &limLockPtr->on)
        || !xdr_time_t(xdrs, &limLockPtr->time)
        || !xdr_int(xdrs, &limLockPtr->uid))
        return FALSE;

    if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN)) {
        return FALSE;
    }

    return TRUE;
}

static bool_t
xdr_resItem(XDR *xdrs, struct resItem *resItem, struct LSFHeader *hdr)
{
    char *sp, *sp1;

    sp = resItem->des;
    sp1= resItem->name;
    if (xdrs->x_op == XDR_DECODE) {
        sp[0] = '\0';
        sp1[0] = '\0';
    }
    if (!xdr_string(xdrs, &sp1, MAXLSFNAMELEN) ||
        !xdr_string(xdrs, &sp, MAXRESDESLEN) ||
        !xdr_enum(xdrs, (int *)&resItem->valueType) ||
        !xdr_enum(xdrs, (int *)&resItem->orderType) ||
        !xdr_int(xdrs, &resItem->flags) ||
        !xdr_int(xdrs, &resItem->interval))
        return FALSE;
    return TRUE;

}

bool_t
xdr_lsInfo(XDR *xdrs, struct lsInfo *lsInfoPtr, struct LSFHeader *hdr)
{
    int i;
    char *sp = (char*)0;
    static char *memp;

    if (!xdr_int(xdrs, &lsInfoPtr->nRes))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE) {
        FREEUP(memp);
        lsInfoPtr->resTable = (struct resItem *)
            malloc(lsInfoPtr->nRes * sizeof(struct resItem));
        if(!lsInfoPtr->resTable)
            return FALSE;
        memp = (char *)lsInfoPtr->resTable;
    }

    for (i=0;i<lsInfoPtr->nRes;i++) {
        if (!xdr_arrayElement(xdrs, (char *)&lsInfoPtr->resTable[i],
                              hdr, xdr_resItem)) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    if (!xdr_int(xdrs, &lsInfoPtr->nTypes)) {
        if (xdrs->x_op == XDR_DECODE)
            FREEUP(memp);
        return FALSE;
    }

    for (i=0;i<lsInfoPtr->nTypes;i++) {
        sp = lsInfoPtr->hostTypes[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN))  {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    if (!xdr_int(xdrs, &lsInfoPtr->nModels)) {
        if (xdrs->x_op == XDR_DECODE)
            FREEUP(memp);
        return FALSE;
    }

    for (i=0;i<lsInfoPtr->nModels;i++) {
        sp = lsInfoPtr->hostModels[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN)) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    if (!xdr_int(xdrs, &lsInfoPtr->numUsrIndx) ||
        !xdr_int(xdrs, &lsInfoPtr->numIndx)) {
        if (xdrs->x_op == XDR_DECODE)
            FREEUP(memp);
        return FALSE;
    }

    for (i=0; i < lsInfoPtr->nModels; i++)
        if (!xdr_float(xdrs, &lsInfoPtr->cpuFactor[i])) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }


    for (i = 0; i < lsInfoPtr->nModels; ++i) {
        sp = lsInfoPtr->hostArchs[i];
        if (xdrs->x_op == XDR_DECODE)
            sp[0] = '\0';
        if (!xdr_string(xdrs, &sp, MAXLSFNAMELEN)) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    for (i=0; i < lsInfoPtr->nModels; ++i) {
        if (!xdr_int(xdrs, &lsInfoPtr->modelRefs[i])) {
            if (xdrs->x_op == XDR_DECODE)
                FREEUP(memp);
            return FALSE;
        }
    }

    return TRUE;

}

bool_t
xdr_masterInfo(XDR *xdrs, struct masterInfo *mInfoPtr, struct LSFHeader *hdr)
{
    char *sp;

    sp = mInfoPtr->hostName;
    if (xdrs->x_op == XDR_DECODE) {
        sp[0] = '\0';
    }

    if (!xdr_string(xdrs, &sp, MAXHOSTNAMELEN))
        return(FALSE);

    if (!xdr_address(xdrs, &mInfoPtr->addr))
        return (FALSE);

    if (!xdr_portno(xdrs, &mInfoPtr->portno))
        return (FALSE);

    return(TRUE);
}

bool_t
xdr_clusterInfoReq(XDR *xdrs, struct clusterInfoReq *clusterInfoReq,
                   struct LSFHeader *hdr)
{
    char line[MAXLINELEN];
    char *sp = line;

    if (xdrs->x_op == XDR_DECODE) {
        if (! xdr_string(xdrs, &sp, MAXLINELEN))
            return (FALSE);
        clusterInfoReq->resReq = putstr_(line);
        if (clusterInfoReq->resReq == NULL)
            return (FALSE);
    } else {
        if (! xdr_string(xdrs, &clusterInfoReq->resReq, MAXLINELEN))
            return (FALSE);
    }

    if (! xdr_int(xdrs, &clusterInfoReq->listsize)
        || ! xdr_int(xdrs, &clusterInfoReq->options)) {
        if (xdrs->x_op == XDR_DECODE)
            free(clusterInfoReq->resReq);
        return (FALSE);
    }

    if (clusterInfoReq->listsize && xdrs->x_op == XDR_DECODE) {
        clusterInfoReq->clusters = (char **) calloc(clusterInfoReq->listsize,
                                                    sizeof (char *));
        if (clusterInfoReq->clusters == NULL) {
            free(clusterInfoReq->resReq);
            return(FALSE);
        }
    }

    if (! xdr_array_string(xdrs, clusterInfoReq->clusters,
                           MAXLSFNAMELEN, clusterInfoReq->listsize)) {
        if (xdrs->x_op == XDR_DECODE) {
            FREEUP(clusterInfoReq->resReq);
            FREEUP(clusterInfoReq->clusters);
        }
        return (FALSE);
    }

    return (TRUE);

}

bool_t
xdr_clusterInfoReply(XDR *xdrs, struct clusterInfoReply *clusterInfoReply,
                     struct LSFHeader *hdr)
{
    int i;
    static char *memp = NULL;
    static int nClus = 0;

    if (!xdr_int(xdrs, &clusterInfoReply->nClus))
        return FALSE;


    if (!xdr_arrayElement(xdrs, (char *)clusterInfoReply->shortLsInfo,
                          hdr, xdr_shortLsInfo))
        return FALSE;


    if (xdrs->x_op == XDR_DECODE) {
        if (memp)
            freeUpMemp(memp, nClus);
        memp = calloc(clusterInfoReply->nClus, sizeof (struct shortCInfo));
        if (memp == NULL) {
            nClus = 0;
            return (FALSE);
        }
        clusterInfoReply->clusterMatrix = (struct shortCInfo *) memp;
    }

    for (i=0; i<clusterInfoReply->nClus; i++) {
        bool_t status = 0;
        status = xdr_arrayElement(xdrs,
                                  (char *)&clusterInfoReply->clusterMatrix[i],
                                  hdr,
                                  xdr_shortCInfo);
        if (! status) {
            if (xdrs->x_op == XDR_DECODE) {
                nClus = 0;
                freeUpMemp(memp,  i -1);
            }
            return (FALSE);
        }
    }
    if (xdrs->x_op == XDR_DECODE) {
        nClus = clusterInfoReply->nClus;
    }
    return (TRUE);

}

static void
freeUpMemp (char *memp, int nClus)
{
    int i, j;
    struct shortCInfo *clusterMatrix;

    clusterMatrix = (struct shortCInfo *) memp;
    for (i = 0; i < nClus; i++) {
        FREEUP (clusterMatrix[i].resBitMaps);
        FREEUP (clusterMatrix[i].hostTypeBitMaps);
        FREEUP (clusterMatrix[i].hostModelBitMaps);
        if (clusterMatrix[i].nAdmins > 0) {
            for (j = 0; j < clusterMatrix[i].nAdmins; j++)
                FREEUP (clusterMatrix[i].admins[j]);
            FREEUP (clusterMatrix[i].adminIds);
            FREEUP (clusterMatrix[i].admins);
        }
    }
    FREEUP (memp);

}

bool_t
xdr_shortCInfo(XDR *xdrs, struct shortCInfo *clustInfoPtr, struct LSFHeader *hdr)
{
    char *sp1, *sp2, *sp3;
    int i;

    sp1 = clustInfoPtr->clName;
    sp2 = clustInfoPtr->masterName;
    sp3 = clustInfoPtr->managerName;

    if (xdrs->x_op == XDR_DECODE) {
        sp1[0] = '\0';
        sp2[0] = '\0';
        sp3[0] = '\0';
        clustInfoPtr->nAdmins = 0;
        clustInfoPtr->adminIds = NULL;
        clustInfoPtr->admins = NULL;
    }

    if (! (xdr_string(xdrs, &sp1, MAXLSFNAMELEN) &&
           xdr_string(xdrs, &sp2, MAXHOSTNAMELEN) &&
           xdr_string(xdrs, &sp3, MAXLSFNAMELEN) &&
           xdr_int(xdrs, &clustInfoPtr->status) &&
           xdr_int(xdrs, &clustInfoPtr->numServers) &&
           xdr_int(xdrs, &clustInfoPtr->numClients) &&
           xdr_int(xdrs, &clustInfoPtr->managerId) &&
           xdr_int(xdrs, &clustInfoPtr->resClass) &&
           xdr_int(xdrs, &clustInfoPtr->typeClass) &&
           xdr_int(xdrs, &clustInfoPtr->modelClass) &&
           xdr_int(xdrs, &clustInfoPtr->numIndx) &&
           xdr_int(xdrs, &clustInfoPtr->numUsrIndx) &&
           xdr_int(xdrs, &clustInfoPtr->usrIndxClass)))
        return FALSE;

    if (!xdr_int(xdrs, &clustInfoPtr->nAdmins))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && clustInfoPtr->nAdmins > 0) {
        clustInfoPtr->admins
            = (char **) malloc (clustInfoPtr->nAdmins * sizeof (char *));
        clustInfoPtr->adminIds
            = (int *)  malloc (clustInfoPtr->nAdmins * sizeof (int));
        if (clustInfoPtr->admins == NULL ||
            clustInfoPtr->adminIds == NULL) {
            FREEUP (clustInfoPtr->adminIds);
            FREEUP (clustInfoPtr->admins);
            clustInfoPtr->nAdmins = 0;
            return (FALSE);
        }
    }
    for (i = 0; i < clustInfoPtr->nAdmins; i++) {
        if (!(xdr_var_string (xdrs, &clustInfoPtr->admins[i]) &&
              xdr_int(xdrs, &clustInfoPtr->adminIds[i])))
            return (FALSE);
    }
    if (!xdr_int(xdrs, &clustInfoPtr->nRes))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && clustInfoPtr->nRes) {
        clustInfoPtr->resBitMaps = (int *)
            malloc (GET_INTNUM(clustInfoPtr->nRes) *sizeof(int));
        if (clustInfoPtr->resBitMaps == NULL) {
            clustInfoPtr->nRes = 0;
            return (FALSE);
        }
    }
    for (i = 0; (i < clustInfoPtr->nRes && i < GET_INTNUM(clustInfoPtr->nRes)); i++) {
        if (!(xdr_int(xdrs, &clustInfoPtr->resBitMaps[i])))
            return (FALSE);
    }

    if (!xdr_int(xdrs, &clustInfoPtr->nTypes))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && clustInfoPtr->nTypes) {
        clustInfoPtr->hostTypeBitMaps = (int *)
            malloc (GET_INTNUM(clustInfoPtr->nTypes) *sizeof(int));
        if (clustInfoPtr->hostTypeBitMaps == NULL) {
            clustInfoPtr->nTypes = 0;
            return (FALSE);
        }
    }


    for (i = 0;  (i < clustInfoPtr->nTypes && i < GET_INTNUM(clustInfoPtr->nTypes)); i++) {
        if (!(xdr_int(xdrs, &clustInfoPtr->hostTypeBitMaps[i])))
            return (FALSE);
    }

    if (!xdr_int(xdrs, &clustInfoPtr->nModels))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && clustInfoPtr->nModels) {
        clustInfoPtr->hostModelBitMaps = (int *)
            malloc (GET_INTNUM(clustInfoPtr->nModels) *sizeof(int));
        if (clustInfoPtr->hostModelBitMaps == NULL) {
            clustInfoPtr->nModels = 0;
            return (FALSE);
        }
    }
    for (i = 0; (i < clustInfoPtr->nModels && i < GET_INTNUM(clustInfoPtr->nModels)); i++) {
        if (!(xdr_int(xdrs, &clustInfoPtr->hostModelBitMaps[i])))
            return (FALSE);
    }

    return TRUE;

}

bool_t
xdr_cInfo(XDR *xdrs, struct cInfo *cInfo, struct LSFHeader *hdr)
{


    char *sp1, *sp2, *sp3;
    int i;

    sp1 = cInfo->clName;
    sp2 = cInfo->masterName;
    sp3 = cInfo->managerName;

    if (xdrs->x_op == XDR_DECODE) {
        sp1[0] = '\0';
        sp2[0] = '\0';
        sp3[0] = '\0';
        cInfo->nAdmins = 0;
        cInfo->adminIds = NULL;
        cInfo->admins = NULL;
    }

    if (! (xdr_string(xdrs, &sp1, MAXLSFNAMELEN) &&
           xdr_string(xdrs, &sp2, MAXHOSTNAMELEN) &&
           xdr_string(xdrs, &sp3, MAXLSFNAMELEN) &&
           xdr_int(xdrs, &cInfo->status) &&
           xdr_int(xdrs, &cInfo->numServers) &&
           xdr_int(xdrs, &cInfo->numClients) &&
           xdr_int(xdrs, &cInfo->managerId) &&
           xdr_int(xdrs, &cInfo->resClass) &&
           xdr_int(xdrs, &cInfo->typeClass) &&
           xdr_int(xdrs, &cInfo->modelClass) &&
           xdr_int(xdrs, &cInfo->numIndx) &&
           xdr_int(xdrs, &cInfo->numUsrIndx) &&
           xdr_int(xdrs, &cInfo->usrIndxClass)))
        return FALSE;

    if (!xdr_int(xdrs, &cInfo->nAdmins))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && cInfo->nAdmins > 0) {
        cInfo->admins
            = (char **) malloc (cInfo->nAdmins * sizeof (char *));
        cInfo->adminIds
            = (int *)  malloc (cInfo->nAdmins * sizeof (int));
        if (cInfo->admins == NULL ||
            cInfo->adminIds == NULL) {
            FREEUP (cInfo->adminIds);
            FREEUP (cInfo->admins);
            cInfo->nAdmins = 0;
            return (FALSE);
        }
    }
    for (i = 0; i < cInfo->nAdmins; i++) {
        if (!(xdr_var_string (xdrs, &cInfo->admins[i]) &&
              xdr_int(xdrs, &cInfo->adminIds[i])))
            return (FALSE);
    }

    if (!xdr_int(xdrs, &cInfo->nRes))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && cInfo->nRes) {
        cInfo->resBitMaps = (int *)
            malloc (GET_INTNUM(cInfo->nRes) *sizeof(int));
        if (cInfo->resBitMaps == NULL) {
            cInfo->nRes = 0;
            return (FALSE);
        }
    }
    for (i = 0; (i < cInfo->nRes && i < GET_INTNUM(cInfo->nRes)); i++) {
        if (!(xdr_int(xdrs, &cInfo->resBitMaps[i])))
            return (FALSE);
    }

    if (cInfo->numIndx > 0 && xdrs->x_op == XDR_DECODE) {
        cInfo->loadIndxNames = (char **)calloc(cInfo->numIndx,
                                               sizeof(char *));
        if (cInfo->loadIndxNames == NULL)
            return(FALSE);
    }

    if (! xdr_shortLsInfo(xdrs, &(cInfo->shortInfo), hdr))
        return (FALSE);

    if (cInfo->numIndx > 0) {
        if (! xdr_array_string(xdrs, cInfo->loadIndxNames, MAXLSFNAMELEN,
                               cInfo->numIndx))
        {
            FREEUP(cInfo->loadIndxNames);
            return (FALSE);
        }
    }

    if (!xdr_int(xdrs, &cInfo->nTypes))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && cInfo->nTypes) {
        cInfo->hostTypeBitMaps = (int *)
            malloc (GET_INTNUM(cInfo->nTypes) *sizeof(int));
        if (cInfo->hostTypeBitMaps == NULL) {
            cInfo->nTypes = 0;
            return (FALSE);
        }
    }


    for (i = 0;  (i < cInfo->nTypes && i < GET_INTNUM(cInfo->nTypes)); i++) {
        if (!(xdr_int(xdrs, &cInfo->hostTypeBitMaps[i])))
            return (FALSE);
    }

    if (!xdr_int(xdrs, &cInfo->nModels))
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && cInfo->nModels) {
        cInfo->hostModelBitMaps = (int *)
            malloc (GET_INTNUM(cInfo->nModels) *sizeof(int));
        if (cInfo->hostModelBitMaps == NULL) {
            cInfo->nModels = 0;
            return (FALSE);
        }
    }
    for (i = 0; (i < cInfo->nModels && i < GET_INTNUM(cInfo->nModels)); i++) {
        if (!(xdr_int(xdrs, &cInfo->hostModelBitMaps[i])))
            return (FALSE);
    }

    return (TRUE);

}

bool_t
xdr_resourceInfoReq(XDR *xdrs, struct  resourceInfoReq *resourceInfoReq,
                    struct LSFHeader *hdr)
{
    int i;



    if (xdrs->x_op == XDR_DECODE) {
        resourceInfoReq->hostName = NULL;
        resourceInfoReq->resourceNames = NULL;
        resourceInfoReq->numResourceNames = 0;
    }
    if (!(xdr_int(xdrs, &resourceInfoReq->numResourceNames)
          && xdr_int(xdrs, &resourceInfoReq->options)))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE && resourceInfoReq->numResourceNames > 0) {
        if ((resourceInfoReq->resourceNames =
             (char **) malloc (resourceInfoReq->numResourceNames * sizeof (char *))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }

    }
    for (i = 0; i < resourceInfoReq->numResourceNames; i++) {
        if (!xdr_var_string(xdrs, &resourceInfoReq->resourceNames[i])) {
            resourceInfoReq->numResourceNames = i;
            return FALSE;
        }
    }
    if (!xdr_var_string(xdrs, &resourceInfoReq->hostName))
        return FALSE;

    if (xdrs->x_op == XDR_FREE && resourceInfoReq->numResourceNames > 0) {
        FREEUP(resourceInfoReq->resourceNames);
        resourceInfoReq->numResourceNames = 0;
    }
    return TRUE;

}

bool_t
xdr_resourceInfoReply(XDR *xdrs, struct  resourceInfoReply *resourceInfoReply,
                      struct LSFHeader *hdr)
{
    int i, status;


    if (xdrs->x_op == XDR_DECODE) {
        resourceInfoReply->numResources = 0;
        resourceInfoReply->resources = NULL;
    }
    if (!(xdr_int(xdrs, &resourceInfoReply->numResources)
          && xdr_int(xdrs, &resourceInfoReply->badResource)))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE &&  resourceInfoReply->numResources > 0) {
        if ((resourceInfoReply->resources = (struct lsSharedResourceInfo *)
             malloc (resourceInfoReply->numResources
                     * sizeof (struct lsSharedResourceInfo))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }
    }
    for (i = 0; i < resourceInfoReply->numResources; i++) {
        status = xdr_arrayElement(xdrs,
                                  (char *)&resourceInfoReply->resources[i],
                                  hdr,
                                  xdr_lsResourceInfo);
        if (! status) {
            resourceInfoReply->numResources = i;
            return FALSE;
        }
    }
    if (xdrs->x_op == XDR_FREE && resourceInfoReply->numResources > 0) {
        FREEUP(resourceInfoReply->resources);
        resourceInfoReply->numResources = 0;
    }
    return TRUE;
}

static bool_t
xdr_lsResourceInfo (XDR *xdrs, struct  lsSharedResourceInfo *lsResourceInfo,
                    struct LSFHeader *hdr)
{
    int i, status;

    if (xdrs->x_op == XDR_DECODE) {
        lsResourceInfo->resourceName = NULL;
        lsResourceInfo->instances = NULL;
        lsResourceInfo->nInstances = 0;
    }
    if (!(xdr_var_string (xdrs, &lsResourceInfo->resourceName) &&
          xdr_int(xdrs, &lsResourceInfo->nInstances)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE &&  lsResourceInfo->nInstances > 0) {
        if ((lsResourceInfo->instances = (struct lsSharedResourceInstance *)
             malloc (lsResourceInfo->nInstances
                     * sizeof (struct lsSharedResourceInstance))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }
    }
    for (i = 0; i < lsResourceInfo->nInstances; i++) {
        status = xdr_arrayElement(xdrs,
                                  (char *)&lsResourceInfo->instances[i],
                                  hdr,
                                  xdr_lsResourceInstance);
        if (! status) {
            lsResourceInfo->nInstances = i;
            return FALSE;
        }
    }
    if (xdrs->x_op == XDR_FREE && lsResourceInfo->nInstances > 0) {
        FREEUP (lsResourceInfo->instances);
        lsResourceInfo->nInstances = 0;
    }
    return TRUE;
}

static bool_t
xdr_lsResourceInstance (XDR *xdrs, struct  lsSharedResourceInstance *instance,
                        struct LSFHeader *hdr)
{

    if (xdrs->x_op == XDR_DECODE) {
        instance->value = NULL;
        instance->hostList = NULL;
        instance->nHosts = 0;
    }
    if (!(xdr_var_string (xdrs, &instance->value) &&
          xdr_int(xdrs, &instance->nHosts)))
        return (FALSE);

    if (xdrs->x_op == XDR_DECODE &&  instance->nHosts > 0) {
        if ((instance->hostList = (char **)
             malloc (instance->nHosts * sizeof (char *))) == NULL) {
            lserrno = LSE_MALLOC;
            return FALSE;
        }
    }
    if (! xdr_array_string(xdrs, instance->hostList,
                           MAXHOSTNAMELEN, instance->nHosts)) {
        if (xdrs->x_op == XDR_DECODE) {
            FREEUP(instance->hostList);
            instance->nHosts = 0;
        }
        return (FALSE);
    }
    if (xdrs->x_op == XDR_FREE && instance->nHosts > 0) {
        FREEUP (instance->hostList);
        instance->nHosts = 0;
    }
    return TRUE;
}

/* xdr_hostEntry()
 */
bool_t
xdr_hostEntry(XDR *xdrs,
              struct hostEntry *hPtr,
              struct LSFHeader *hdr)
{
    char *s;
    int cc;

    s = hPtr->hostName;
    if (!xdr_string(xdrs, &s, MAXHOSTNAMELEN))
        return FALSE;

    s = hPtr->hostModel;
    if (!xdr_string(xdrs, &s, MAXLSFNAMELEN))
        return FALSE;

    s = hPtr->hostType;
    if (! xdr_string(xdrs, &s, MAXLSFNAMELEN))
        return FALSE;

    if (!xdr_int(xdrs, &hPtr->rcv)
        || !xdr_int(xdrs, &hPtr->nDisks)
        || !xdr_float(xdrs, &hPtr->cpuFactor))
        return FALSE;

    /* this must not be zero... somehow the caller
     * must make sure it is at least 11...
     */
    if (! xdr_int(xdrs, &hPtr->numIndx))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE
        && hPtr->numIndx > 0) {
        hPtr->busyThreshold = calloc(hPtr->numIndx,
                                     sizeof(float));
        if (hPtr->busyThreshold == NULL)
            return FALSE;
    }

    for (cc = 0; cc < hPtr->numIndx; cc++) {
        if (! xdr_float(xdrs, &hPtr->busyThreshold[cc]))
            return FALSE;
    }

    if (! xdr_int(xdrs, &hPtr->nRes))
        return FALSE;

    if (xdrs->x_op == XDR_DECODE
        && hPtr->nRes > 0) {
        hPtr->resList = calloc(hPtr->nRes, sizeof(char *));
    }

    for (cc = 0; cc < hPtr->nRes; cc++) {
        if (! xdr_var_string(xdrs, &hPtr->resList[cc]))
            return FALSE;
    }

    if (! xdr_int(xdrs, &hPtr->rexPriority)
        || ! xdr_var_string(xdrs, &hPtr->window))
        return FALSE;

    return TRUE;
}

bool_t
xdr_hostName(XDR *xdrs,
             char *hostname,
             struct LSFHeader *hdr)
{
    if (! xdr_string(xdrs, &hostname, MAXHOSTNAMELEN))
        return FALSE;

    return TRUE;
}

/* Typed LIM output owns its metadata and rows, unlike the legacy decoder. */
bool_t
xdr_hostOutputInfoReply(XDR *xdrs, struct hostInfoReply *reply,
                        struct LSFHeader *hdr)
{
    struct shortLsInfo *info = reply->shortLsInfo;
    int i, j;
    (void)hdr;
    if (xdrs->x_op == XDR_FREE) {
        if (reply->hostMatrix) for (i = 0; i < reply->nHost; i++) {
            FREEUP(reply->hostMatrix[i].windows);
            FREEUP(reply->hostMatrix[i].resBitMaps);
        }
        FREEUP(reply->hostMatrix);
        if (info->resName) for (i = 0; i < info->nRes; i++) FREEUP(info->resName[i]);
        FREEUP(info->resName);
        for (i = 0; i < info->nTypes && i < MAXTYPES; i++) FREEUP(info->hostTypes[i]);
        for (i = 0; i < info->nModels && i < MAXMODELS; i++) FREEUP(info->hostModels[i]);
        memset(info, 0, sizeof(*info));
        reply->nHost = 0;
        return TRUE;
    }
    if (!xdr_u_int(xdrs, &reply->outputMask) || (reply->outputMask >> 12) ||
        !xdr_int(xdrs, &reply->nHost) || reply->nHost < 0 || reply->nHost > 1000000 ||
        !xdr_int(xdrs, &info->nTypes) || info->nTypes < 0 || info->nTypes > MAXTYPES ||
        !xdr_int(xdrs, &info->nModels) || info->nModels < 0 || info->nModels > MAXMODELS ||
        !xdr_int(xdrs, &info->nRes) || info->nRes < 0 || info->nRes > 65536)
        return FALSE;
    if (xdrs->x_op == XDR_DECODE && info->nRes) {
        info->resName = calloc(info->nRes, sizeof(char *));
        if (!info->resName) return FALSE;
    }
    for (i = 0; i < info->nTypes; i++)
        if (!xdr_var_string(xdrs, &info->hostTypes[i])) return FALSE;
    for (i = 0; i < info->nModels; i++)
        if (!xdr_var_string(xdrs, &info->hostModels[i]) ||
            !xdr_float(xdrs, &info->cpuFactors[i])) return FALSE;
    for (i = 0; i < info->nRes; i++)
        if (!xdr_var_string(xdrs, &info->resName[i])) return FALSE;
    if (xdrs->x_op == XDR_DECODE && reply->nHost) {
        reply->hostMatrix = calloc(reply->nHost, sizeof(struct shortHInfo));
        if (!reply->hostMatrix) return FALSE;
    }
    for (i = 0; i < reply->nHost; i++) {
        struct shortHInfo *row = &reply->hostMatrix[i];
        char *name = row->hostName;
        if (xdrs->x_op == XDR_DECODE) {
            row->hTypeIndx = MAXTYPES;
            row->hModelIndx = MAXMODELS;
        }
        if (!xdr_string(xdrs, &name, MAXHOSTNAMELEN)) return FALSE;
#define LIM_MEMBER(bits, member) \
        if ((reply->outputMask & (bits)) && !xdr_int(xdrs, &row->member)) return FALSE;
        LIM_MEMBER(1u << 1, hTypeIndx)
        LIM_MEMBER((1u << 2) | (1u << 3), hModelIndx)
        LIM_MEMBER((1u << 4) | (1u << 10), maxCpus)
        LIM_MEMBER(1u << 5, maxMem)
        LIM_MEMBER(1u << 6, maxSwap)
        LIM_MEMBER((1u << 7) | (1u << 11), flags)
        LIM_MEMBER(1u << 9, maxTmp)
#undef LIM_MEMBER
        if (xdrs->x_op == XDR_DECODE) {
            if (row->hTypeIndx < 0 || row->hTypeIndx >= info->nTypes) row->hTypeIndx = MAXTYPES;
            if (row->hModelIndx < 0 || row->hModelIndx >= info->nModels) row->hModelIndx = MAXMODELS;
        }
        if ((reply->outputMask & (1u << 11)) && !xdr_var_string(xdrs, &row->windows))
            return FALSE;
        if (reply->outputMask & (1u << 8)) {
            if (!xdr_int(xdrs, &row->resClass) || !xdr_int(xdrs, &row->nRInt) ||
                row->nRInt < 0 || row->nRInt > GET_INTNUM(info->nRes)) return FALSE;
            /* The legacy scalar bitmap indexes the resource-name table. */
            if (!row->nRInt && info->nRes < 32 &&
                ((unsigned int)row->resClass >> info->nRes)) return FALSE;
            if (xdrs->x_op == XDR_DECODE && row->nRInt) {
                row->resBitMaps = calloc(GET_INTNUM(info->nRes), sizeof(int));
                if (!row->resBitMaps) return FALSE;
            }
            for (j = 0; j < row->nRInt; j++)
                if (!xdr_int(xdrs, &row->resBitMaps[j])) return FALSE;
        }
    }
    return TRUE;
}

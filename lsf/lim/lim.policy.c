/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * Copyright (C) 2011 David Bigagli
 *
 * $Id: lim.policy.c 397 2007-11-26 19:04:00Z mblack $
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
#include "lim.h"
#include <math.h>

#define NL_SETN 24

float *extraload;
char jobxfer = 0;
struct hostNode **candidates=NULL;
static int candListSize=0;
struct hostNode *fromHostPtr;

#define ELIG_ALL     0x01
#define ELIG_NOLIMIT 0x02

#define SORT_CUT    0x01
#define SORT_FINAL  0x02
#define SORT_SINDX  0x04
#define SORT_INCR   0x08

#define P_(s) s

static int findBestHost P_((register struct resVal *, int, int, char **, int, char, int, int));
static void jackup P_((int, struct hostNode *, float));
static void loadAdj P_((struct jobXfer *, struct hostNode **, int, char));
static void potentialOfCandidates P_((int, struct resVal *));
static void potentialOfHost P_((struct hostNode *, struct resVal *));
static void selectBestInstances P_((int, int, char, int));
static int  getNumInstances P_((int));
static int getOkSites(int, int, int);
static int findNPref(int, int, char **);
static int bsort(int, int, int, int, float, char, int, int);
static void mkexld(struct hostNode *, struct hostNode *, int, float *, float *,
                   float);
static int orderByStatus(int, int);
static int grabHosts(struct hostNode *, struct resVal *, struct decisionReq *,  int, char *, int);

static int addCandList(struct hostNode *, int );
static int initCandList(void);

static void setBusyIndex(int, struct hostNode *);
static float loadIndexValue(int, int, int);

#define effectiveRq(nrq, factor) ((nrq) * (factor) -1)

void
placeReq(XDR *xdrs,
         struct sockaddr_in *from,
         struct LSFHeader *reqHdr,
         int s)
{
    struct tclHostData tclHostData;
    struct placeReply placeReply;
    struct decisionReq plReq;
    struct jobXfer jobXfer;
    struct LSFHeader replyHdr;
    char fromEligible;
    XDR xdrs2;
    enum limReplyCode limReplyCode;
    struct resVal resVal;
    int ncandidates;
    int ncandidateInst;
    int ignore_res;
    int propt;
    int returnCode;
    int i;
    int j;
    int cc;
    char clName;
    char *replyStruct;
    char buf[MSGSIZE];

    if (logclass & (LC_TRACE | LC_HANG | LC_COMM))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", __func__);

    initResVal (&resVal);

    placeReply.numHosts = 0;

    if (!xdr_decisionReq(xdrs, &plReq, reqHdr)) {
        ls_syslog(LOG_ERR, "\
%s: xdr_decisionReq() from %s failed %m", __func__, sockAdd2Str_(from));
        limReplyCode = LIME_BAD_DATA;
        goto Reply1;
    }

    if (! (plReq.ofWhat == OF_HOSTS && plReq.numPrefs == 2
           && plReq.numHosts == 1
           && equalHost_(plReq.preferredHosts[1], myHostPtr->hostName))) {

        if (!masterMe) {
            wrongMaster(from, buf, reqHdr, -1);
            for (i = 0; i < plReq.numPrefs;i++)
                free(plReq.preferredHosts[i]);
            free(plReq.preferredHosts);
            return;
        }
    }

    if (!validHosts(plReq.preferredHosts, plReq.numPrefs, &clName, plReq.options | SEND_TO_CLUSTERS)) {
        limReplyCode = LIME_UNKWN_HOST;
        ls_syslog(LOG_INFO, "\
%s: failed for bad cluster/host name from %s",
                  __func__, sockAdd2Str_(from));
        goto Reply;
    }

    propt = PR_ALL;
    if (plReq.options & DFT_FROMTYPE)
        propt |= PR_DEFFROMTYPE;

    getTclHostData (&tclHostData, myHostPtr, myHostPtr, TRUE);
    cc = parseResReq(plReq.resReq, &resVal, &allInfo, propt, unitForLimits);
    if (cc != PARSE_OK ||
        (returnCode = evalResReq(resVal.selectStr,
                                 &tclHostData,
                                 plReq.options & DFT_FROMTYPE)) < 0) {
        if (cc == PARSE_BAD_VAL)
            limReplyCode = LIME_UNKWN_RVAL;
        else if (cc == PARSE_BAD_NAME)
            limReplyCode = LIME_UNKWN_RNAME;
        else
            limReplyCode = LIME_BAD_RESREQ;
        goto Reply;
    }

    fromHostPtr = findHost(plReq.preferredHosts[0]);
    if (!fromHostPtr) {
        limReplyCode = LIME_NAUTH_HOST;
        goto Reply;
    }
    if (strcmp(plReq.hostType, " ") == 0)
        strcpy(plReq.hostType,
               (fromHostPtr->hTypeNo >= 0) ?
               shortInfo.hostTypes[fromHostPtr->hTypeNo] : "unknown");

    fromEligible = FALSE;
    ncandidates = getEligibleSites(&resVal, &plReq, 0, &fromEligible);
    if (!fromEligible)
        fromHostPtr = NULL;

    if (ncandidates <= 0) {
        limReplyCode = ncandidates ? LIME_NO_MEM : LIME_NO_OKHOST;
        goto Reply;
    }

    ignore_res = (plReq.options & IGNORE_RES);


    ncandidates = getOkSites(ncandidates, 0, ignore_res);


    potentialOfCandidates(ncandidates, &resVal);
    if (fromHostPtr)
        potentialOfHost(fromHostPtr, &resVal);
    ncandidateInst = getNumInstances(ncandidates);

    if ( (ncandidates == 0)
         || (ncandidateInst < plReq.numHosts
             && (plReq.options & EXACT))) {
        limReplyCode = LIME_NO_OKHOST;
        goto Reply;
    }

    if (ncandidates > plReq.numHosts) {
        ncandidates = findBestHost(&resVal,
                                   plReq.numHosts,
                                   plReq.numPrefs,
                                   plReq.preferredHosts,
                                   ncandidates,
                                   TRUE,
                                   ignore_res,
                                   plReq.options);
    } else {
        ncandidates = findBestHost(&resVal,
                                   ncandidates,
                                   plReq.numPrefs,
                                   plReq.preferredHosts,
                                   ncandidates,
                                   TRUE,
                                   ignore_res,
                                   plReq.options);
    }

    if ((getNumInstances(ncandidates) < plReq.numHosts)
        && (plReq.options & EXACT)) {
        limReplyCode = LIME_NO_OKHOST;
        goto Reply;
    }

    selectBestInstances(ncandidates,
                        plReq.numHosts,
                        plReq.options & LOCALITY,
                        ignore_res);
    limReplyCode = LIME_NO_ERR;

    placeReply.numHosts = 0;
    for(i = 0; i < ncandidates ;i++) {
        if (candidates[i]->use > 0) {
            placeReply.numHosts++;
        }
    }

    placeReply.placeInfo = calloc(placeReply.numHosts,
                                  sizeof(struct placeInfo));
    if (placeReply.placeInfo == NULL) {
        limReplyCode = LIME_NO_MEM;
        ls_syslog(LOG_ERR, "%s: %m", __func__);
        goto Reply;
    }

    for (i = 0, j = 0; i < ncandidates; i++) {
        if (candidates[i]->use > 0) {
            strcpy(placeReply.placeInfo[j].hostName,
                   candidates[i]->hostName);
            placeReply.placeInfo[j].numtask = candidates[i]->use;
            j++;
        }
    }

Reply:

    for (i = 0; i < plReq.numPrefs; i++)
        free(plReq.preferredHosts[i]);
    free(plReq.preferredHosts);

Reply1:
    freeResVal (&resVal);

    initLSFHeader_(&replyHdr);
    replyHdr.opCode  = (short) limReplyCode;
    replyHdr.refCode = reqHdr->refCode;
    if (limReplyCode == LIME_NO_ERR)
        replyStruct = (char *)&placeReply;
    else
        replyStruct = NULL;

    xdrmem_create(&xdrs2, buf, MSGSIZE, XDR_ENCODE);

    if (! xdr_encodeMsg(&xdrs2,
                        replyStruct,
                        &replyHdr,
                        xdr_placeReply,
                        0,
                        NULL)) {
        ls_syslog(LOG_ERR, "%s: xdr_encodeMsg() %m", __func__);
        xdr_destroy(&xdrs2);
        if (limReplyCode == LIME_NO_ERR)
            free(placeReply.placeInfo);
        return;
    }

    if (s < 0)
        cc = chanSendDgram_(limSock, buf, XDR_GETPOS(&xdrs2), from);
    else
        cc = chanWrite_(s, buf, XDR_GETPOS(&xdrs2));

    if (cc < 0) {
        ls_syslog(LOG_ERR, "\
%s: chanSendDgram()/chanWrite() failed to %m", __func__, sockAdd2Str_(from));
        xdr_destroy(&xdrs2);
        if (limReplyCode == LIME_NO_ERR)
            free(placeReply.placeInfo);
        return;
    }

    xdr_destroy(&xdrs2);

    if(limReplyCode != LIME_NO_ERR)
        return;

    jobXfer.numHosts = placeReply.numHosts;
    jobXfer.placeInfo = placeReply.placeInfo;
    strcpy(jobXfer.resReq, plReq.resReq);

    loadAdj(&jobXfer, candidates, placeReply.numHosts, TRUE);

    free(placeReply.placeInfo);

    return;
}

static int
getOkSites(int num, int retain, int ignore_res)
{
    int i;
    int j;
    int left;
    int tempstatus[2];

    if (retain >= num)
        return num;

    left = num;
    for (i = 0; i < num; i++) {

        tempstatus[0] = candidates[i]->status[0];
        tempstatus[1] = candidates[i]->status[1];

        if (LS_ISOK(tempstatus))
            continue;

        tempstatus[1] = candidates[i]->status[1] & ~(1 << IT);
        if ((candidates[i] == fromHostPtr)
            && (LS_ISOK(tempstatus)))
            continue;

        if (ignore_res && (LS_ISOKNRES(candidates[i]->status)))
            continue;

        if ((candidates[i] == fromHostPtr) &&
            (LS_ISOKNRES(tempstatus)) )
            continue;

        if (fromHostPtr && (candidates[i] == fromHostPtr))
            fromHostPtr = NULL;
        candidates[i] = NULL;
        left--;

        if (left <= retain)
            break;
    }

    for (i = 0, j = 0; i < num; i++) {
        if (candidates[i]) {
            candidates[j] = candidates[i];
            j++;
        }
    }

    return j;

}

int
getEligibleSites(struct resVal *resValPtr,
                 struct decisionReq *reqPtr,
                 char all,
                 char *fromEligible)
{
    struct clusterNode *clPtr;
    int ncand;
    int flags;
    int j;

    ncand = flags = 0;

    if (initCandList() < 0)
        return(-1);

    if (! fromHostPtr) {
        fromHostPtr = findHost(reqPtr->preferredHosts[0]);
        if (!fromHostPtr)
            return (0);
    }

    for (j = 1; j < reqPtr->numPrefs; j++) {

        if (strcmp(myClusterPtr->clName, reqPtr->preferredHosts[j]) == 0) {
            flags |= ELIG_NOLIMIT;
            break;
        }
    }

    clPtr = myClusterPtr;

    if (clPtr->status & CLUST_ALL_ELIGIBLE)
        flags |= ELIG_ALL;
    else
        flags &= ~ELIG_ALL;

    ncand = grabHosts(clPtr->hostList,
                      resValPtr,
                      reqPtr,
                      ncand,
                      fromEligible, flags);
    if (all) {
        ncand = grabHosts(clPtr->clientList,
                          resValPtr,
                          reqPtr,
                          ncand,
                          fromEligible,
                          flags);
    }

    return ncand;
}

static int
grabHosts(struct hostNode *hList,
          struct resVal *resValPtr,
          struct decisionReq *reqPtr,
          int ncand,
          char *fromEligible,
          int flags)
{
    struct hostNode *hPtr;
    int j;
    struct tclHostData tclHostData;

    for (hPtr = hList; hPtr; hPtr = hPtr->nextPtr)  {

        if ((fabs(resValPtr->val[MEM]) < INFINIT_LOAD) &&
            (resValPtr->val[MEM] > hPtr->statInfo.maxMem) )
            continue;

        if ((fabs(resValPtr->val[SWP]) < INFINIT_LOAD) &&
            (resValPtr->val[SWP] > hPtr->statInfo.maxSwap) )
            continue;

        if ((fabs(resValPtr->val[TMP]) < INFINIT_LOAD)
            && (resValPtr->val[TMP] > hPtr->statInfo.maxTmp))
            continue;

        if (reqPtr->ofWhat == OF_HOSTS && !(flags & ELIG_ALL)) {
            for (j = 1; j < reqPtr->numPrefs; j++) {
                if (equalHost_(hPtr->hostName, reqPtr->preferredHosts[j]))
                    break;
            }
            if (j == reqPtr->numPrefs)
                continue;
        }

        getTclHostData (&tclHostData, hPtr, fromHostPtr, FALSE);
        if (evalResReq(resValPtr->selectStr,
                       &tclHostData,
                       reqPtr->options & DFT_FROMTYPE) != 1)
            continue;

        if (equalHost_(hPtr->hostName, reqPtr->preferredHosts[0]))
            *fromEligible = TRUE;

        if (addCandList(hPtr,ncand++) < 0)
            return -1;

        if ((reqPtr->ofWhat == OF_HOSTS) && !(flags & ELIG_NOLIMIT)
            && (ncand >= reqPtr->numPrefs))
            break;
    }

    return ncand;
}

static int
getNumInstances(int ncandidates)
{
    int i;
    int numInst;

    for (i = 0, numInst = 0; i < ncandidates; i++) {
        if (candidates[i] != NULL )
            numInst += candidates[i]->availHigh;
    }

    return numInst;
}

#define RQ  ((1 << R15S) | (1 << R1M) | (1 << R15M))

static void
potentialOfHost(struct hostNode *hPtr,
                struct resVal *resValPtr)
{
    int indexpot;
    int i;
    int maxCpus;

    maxCpus =  hPtr->statInfo.maxCpus;
    if (resValPtr->genClass & RQ) {

        hPtr->availLow  = hPtr->statInfo.maxCpus/li[R15S].extraload[0];
        hPtr->availHigh = hPtr->statInfo.maxCpus/li[R15S].extraload[0];
    } else {
        hPtr->availLow  = hPtr->statInfo.maxCpus;
        hPtr->availHigh = hPtr->statInfo.maxCpus;
    }

    for (i = 0; i < NBUILTINDEX; i++) {
        if (!(i == R15S || i == R1M || i == R15M ||
              i == MEM  || i == SWP || i == TMP))
            continue;

        if (! (resValPtr->genClass & (1 << i)))
            continue;

        if (resValPtr->val[i] >= INFINIT_LOAD) {
            hPtr->availLow  = 1;
            hPtr->availHigh = 1;
            return;
        }

        if (resValPtr->val[i] < 0.01)
            continue;

        if (i == R15S || i == R1M || i == R15M) {

            indexpot =  (int)(maxCpus - ((int)(hPtr->uloadIndex[i]+0.5) % maxCpus)) / resValPtr->val[i];
            if (indexpot < 1)
                indexpot = 1;
            hPtr->availLow = MIN(hPtr->availLow, indexpot);

            indexpot = (int) (maxCpus/resValPtr->val[i]);
            if (indexpot < 1)
                indexpot = 1;
            hPtr->availHigh = MIN(hPtr->availHigh, indexpot);

        } else {

            indexpot = (int) hPtr->loadIndex[i]/ resValPtr->val[i];
            hPtr->availLow  = MIN(hPtr->availLow, indexpot);
            hPtr->availHigh = MIN(hPtr->availHigh, indexpot);
        }
    }
}


static
void potentialOfCandidates(int ncandidates, struct resVal *resValPtr)
{
    int i;

    for (i = 0; i < ncandidates; i++) {
        if (candidates[i] == NULL)
            continue;
        potentialOfHost(candidates[i], resValPtr);
    }
}

static void
selectBestInstances(int ncandidates,
                    int needed,
                    char locality,
                    int ignore_res)
{
    int avail;
    int additional;
    int i;

    for (i = 0; i < ncandidates; i++)
        candidates[i]->use = 0;

    if (locality) {

        char flags = SORT_FINAL | SORT_SINDX | SORT_INCR;
        int  cand;

        for (i = 0; i < ncandidates; i++) {

            if (candidates[i]->availHigh > needed) {
                candidates[i]->use = needed;
                return;
            }

            candidates[i]->loadIndex[allInfo.numIndx] =
                candidates[i]->availHigh;

            candidates[i]->loadIndex[allInfo.numIndx] +=
                (float)(ncandidates-i)/ncandidates;
        }

        cand = bsort(allInfo.numIndx,
                     ncandidates,
                     0,
                     ncandidates,
                     0,
                     flags,
                     ignore_res,
                     NORMALIZE);
        if (cand != ncandidates)
            ls_syslog(LOG_ERR, "\
%s: Internal scheduling error(1) cand %d ncandidates %d",
                      __func__, cand, ncandidates);

        for (i = 0; (i < ncandidates) && (needed > 0); i++) {
            avail = candidates[i]->availHigh;
            candidates[i]->use = MIN(needed,avail);
            needed -=  candidates[i]->use;
        }

        return;
    }

    for (i = 0; (i < ncandidates) && (needed > 0);i++) {
        avail = candidates[i]->availLow;

        if ((needed <= candidates[i]->availHigh)
            && (i < ncandidates/2) )
            avail = candidates[i]->availHigh;

        candidates[i]->use +=  MIN(needed,avail);
        needed -= candidates[i]->use;
    }

    if (needed > 0) {

        for (i = 0; (i<ncandidates) && (needed > 0);i++) {

            avail = candidates[i]->availHigh - candidates[i]->availLow;
            if (avail==0)
                continue;
            additional= MIN(needed,avail);
            candidates[i]->use += additional;
            candidates[i]->availHigh -= additional;
            needed -= additional;
        }
    }
}

static int
findBestHost(struct resVal *resValPtr,
             int num,
             int numPrefs,
             char **preferredHosts,
             int ncandidates,
             char iflag,
             int ignore_res,
             int rqlOptions)
{
    int i;
    int cc;
    int nec;
    float exld;
    char flags;
    float f;
    int prevncandidates = 0;
    int ninst;

    nec = findNPref(ncandidates, numPrefs, preferredHosts);
    flags = SORT_CUT ;

    for (i = resValPtr->nphase - 1; i >= 0; i--) {

        if (i == 0)
            flags |= SORT_FINAL;

        if (iflag ) {

            if (flags & SORT_FINAL)
                flags = SORT_FINAL;
            prevncandidates = ncandidates;
        }

        ncandidates = bsort(resValPtr->order[i],
                            ncandidates,
                            nec,
                            num,
                            li[abs(resValPtr->order[i]) - 1].sigdiff,
                            flags,
                            ignore_res,
                            rqlOptions);
        if (ncandidates == num)

            if (i > 1 )
                i = 1;

        if (iflag) {
            ninst = getNumInstances(ncandidates);
            if (ninst <= num) {
                ncandidates = prevncandidates;
                if (i > 1 )
                    i = 1;
            }
        }
    }

    if (!fromHostPtr)
        return(ncandidates);

    for (i = 0; i < ncandidates; i++)
        if(candidates[i] == fromHostPtr)
            return(ncandidates);

    for (i = resValPtr->nphase - 1; i >= 0; i--) {
        double a, b;
        int lidx;

        lidx = resValPtr->order[i];
        if (candidates[ncandidates-1]->conStatus == TRUE) {
            cc = 1;
            exld = 0;
        } else {

            cc = 0;
            exld = candidates[ncandidates-1]->loadIndex[lidx]
                * nec /(ncandidates*25.0);
            if(!li[lidx].increasing)
                exld = -exld;
        }
        a = fromHostPtr->loadIndex[lidx];
        b = candidates[ncandidates-1]->loadIndex[lidx]+exld;

        if (lidx == R15S || lidx == R1M || lidx == R15M) {
            float cpuf;


            cpuf = (candidates[ncandidates-1]->hModelNo >= 0) ?
                shortInfo.cpuFactors[candidates[ncandidates-1]->hModelNo]:1.0;
            if (fromHostPtr->hModelNo >= 0)
                f = shortInfo.cpuFactors[fromHostPtr->hModelNo]/ cpuf;
            else
                f = 1.0 / cpuf;

            f = f * li[lidx].delta[cc] / cpuf;
        } else {
            f = li[lidx].delta[cc];
        }

        if (li[lidx].increasing ? (a - b > f) : (b - a > f))
            break;
    }
    if (i < 0) {

        candidates[ncandidates-1] = fromHostPtr;
    }

    return ncandidates;

}

static int
findNPref(int ncandidates, int numPrefs, char **preferredHosts)
{
    int i;
    int j;
    int nec = 0;

    if (numPrefs > 0) {
        for (j = 0; j < ncandidates;j++) {

            for (i = 0; i < numPrefs; i++) {

                candidates[j]->conStatus = FALSE;
                if (equalHost_(preferredHosts[i], candidates[j]->hostName)) {
                    candidates[j]->conStatus = TRUE;
                    nec++;
                    break;
                }
            }
        }
    }

    return nec;
}

#define NOTORDERED(inc,a,b)   ((inc) ? ((a) > (b)) : ((a) < (b)))

static int
bsort(int lidx,
      int ncandidates,
      int nec,
      int numHosts,
      float threshold,
      char flags,
      int ignore_res,
      int rqlOptions)
{
    char swap;
    register int i,j;
    char incr;
    float exld1, exld2;
    struct hostNode *tmp;
    float coef;
    int cutoffs;
    int residual;
    int shrink;
    int order;
    char flip;

    if (lidx < 0)
        flip = TRUE;
    else
        flip = FALSE;
    lidx = abs(lidx) -1;



    if (lidx == R15S || lidx == R1M || lidx == R15M ||
        lidx == LS)
        shrink = 5;
    else
        shrink = 8;

    if (! (flags & SORT_FINAL)) {
        residual = ncandidates - numHosts;
        if (residual < 1)
            cutoffs = 0;
        else
            cutoffs = (residual - 1)/shrink + 1;
    } else {
        if (ncandidates >= numHosts)
            cutoffs = numHosts;
        else
            cutoffs = ncandidates;
    }

    if (!(flags & SORT_CUT))
        cutoffs = ncandidates;

    if (flags & SORT_SINDX)
        if (flags & SORT_INCR)
            incr = FALSE;
        else
            incr = TRUE;
    else {
        incr = li[lidx].increasing;
        if (flip)
            incr = !incr;
    }


    coef = 0.05 * nec/numHosts;

    if (! (flags & SORT_FINAL)) {
        float bestload = loadIndexValue(0, lidx, rqlOptions);

        for (i=1; i<ncandidates; i++) {
            if (NOTORDERED(incr, bestload, loadIndexValue(i, lidx, rqlOptions)))
                bestload = loadIndexValue(i, lidx, rqlOptions);
        }


        swap = TRUE;
        i = 0;
        while (swap && (i<ncandidates-cutoffs)) {
            swap = FALSE;
            for (j=ncandidates-2; j>=i; j--) {
                order = orderByStatus(j+1, ignore_res);
                if (order == 0) {
                    swap = TRUE;
                    continue;
                }
                if (order == 1)
                    continue;


                if (!(flags & SORT_SINDX)) {
                    mkexld(candidates[j], candidates[j+1], lidx,
                           &exld1, &exld2, coef);
                } else {
                    exld1 = 0.0;
                    exld2 = 0.0;
                }

                if (NOTORDERED(incr,
                               loadIndexValue(j, lidx, rqlOptions) + exld1,
                               loadIndexValue(j+1, lidx, rqlOptions) + exld2)) {
                    swap = TRUE;
                    tmp = candidates[j];
                    candidates[j] = candidates[j+1];
                    candidates[j+1] = tmp;
                }
            }
            i++;
        }
        for (i=ncandidates-cutoffs; i<ncandidates; i++)
            if (fabs(loadIndexValue(i, lidx, rqlOptions) - bestload)
                >= threshold)
                return i;

        return (ncandidates);
    }

    swap = TRUE;
    i = 0;
    while (swap && (i < cutoffs)) {
        swap = FALSE;
        for (j=ncandidates-2;j>=i;j--) {
            order = orderByStatus(j+1, ignore_res);
            if (order == 0) {
                swap = TRUE;
                continue;
            }
            if (order == 1)
                continue;

            if (!(flags & SORT_SINDX)) {
                mkexld(candidates[j], candidates[j+1], lidx, &exld1, &exld2, coef);
            } else {
                exld1 = 0.0;
                exld2 = 0.0;
            }

            if (NOTORDERED(incr,
                           loadIndexValue(j, lidx, rqlOptions) + exld1,
                           loadIndexValue(j+1, lidx, rqlOptions) + exld2)) {
                swap = TRUE;
                tmp = candidates[j];
                candidates[j] = candidates[j+1];
                candidates[j+1] = tmp;
            }
        }
        i++;
    }
    return (cutoffs);

}

static int
orderByStatus(int j, int ignore_res)
{
    struct hostNode *tmp;
    int *status1, *status2;

    if (candidates[j-1] == fromHostPtr) {
        status1 = candidates[j-1]->status;
        status1[1] = candidates[j-1]->status[1] & ~(1 << IT);
    } else {
        status1 = candidates[j-1]->status;
    }

    if (candidates[j] == fromHostPtr) {
        status2 = candidates[j]->status;
        status2[1] = candidates[j]->status[1]  & ~(1 << IT);
    } else {
        status2 = candidates[j]->status;
    }

    if ( (ignore_res && LS_ISOKNRES(status2) && ! LS_ISOKNRES(status1)) ||
         (!ignore_res && LS_ISOK(status2) && ! LS_ISOK(status1))     ||
         (!ignore_res && LS_ISOKNRES(status2) &&
          (LS_ISBUSY(status1) || LS_ISLOCKED(status1)) ) ||
         (! LS_ISUNAVAIL(status2) && LS_ISUNAVAIL(status1)) ||
         (LS_ISBUSY(status2) && LS_ISLOCKED(status1))) {
        tmp = candidates[j];
        candidates[j] = candidates[j-1];
        candidates[j-1] = tmp;
        return 0;
    }


    if (ignore_res && LS_ISOKNRES(status1) && ! LS_ISOKNRES(status2))
        return 1;
    if (!ignore_res && LS_ISOK(status1) && ! LS_ISOK(status2))
        return 1;
    if (!ignore_res && LS_ISOKNRES(status1) &&
        (LS_ISBUSY(status2) || LS_ISLOCKED(status2)))
        return 1;
    if (! LS_ISUNAVAIL(status1) && LS_ISUNAVAIL(status2))
        return 1;
    if (LS_ISBUSY(status1) && LS_ISLOCKED(status2))
        return 1;
    if (LS_ISUNAVAIL(status1) && LS_ISUNAVAIL(status2))

        return 1;

    return 2;

}

static void
mkexld(struct hostNode *hn1, struct hostNode *hn2, int lidx, float *exld1,
       float *exld2, float coef)
{
    if(hn1->conStatus == FALSE) {
        *exld1 = hn1->loadIndex[lidx] * coef;
        if(!li[lidx].increasing)
            *exld1 = - *exld1;
    } else
        *exld1 = 0;

    if(hn2->conStatus == FALSE) {
        *exld2 = hn2->loadIndex[lidx] * coef;
        if(!li[lidx].increasing)
            *exld2 = - *exld2;
    } else
        *exld2 = 0;

}

static void
loadAdj(struct jobXfer *jobXferPtr,
        struct hostNode **destHostPtr,
        int num,
        char child)
{
    static char fname[] = "loadAdj()";
    static char buf[MSGSIZE];
    XDR xdrs;
    int i, len;
    struct sockaddr_in addr;
    enum limReqCode limReqCode;
    struct LSFHeader reqHdr;

    if (limSock < 0) {
        ls_syslog(LOG_ERR, "\
%s: invalid limSock%d", fname, limSock);
        return;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = lim_port;
    limReqCode = LIM_JOB_XFER;

    if (child) {

        memcpy(&addr.sin_addr,
               &myClusterPtr->masterPtr->addr[0],
               sizeof(struct in_addr));
        xdrmem_create(&xdrs, buf, MSGSIZE, XDR_ENCODE);
        initLSFHeader_(&reqHdr);
        reqHdr.opCode  = limReqCode;
        reqHdr.refCode = 0;

        if (!(xdr_LSFHeader(&xdrs, &reqHdr)
              && xdr_jobXfer(&xdrs, jobXferPtr, &reqHdr))) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname,
                      "xdr_LSFHeader/xdr_jobXfer");
            xdr_destroy(&xdrs);
            return;
        }
        len = XDR_GETPOS(&xdrs);

        if (logclass & LC_COMM)
            ls_syslog(LOG_DEBUG, "\
loadAdj: tell master(%s) of job xfer (len=%d)",
                      sockAdd2Str_(&addr), len);

        if (chanSendDgram_(limSock, buf, len, &addr) < 0 ) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5809,
                                             "%s: Failed to tell master(%s) of job xfer (len=%d): %m"), /* catgets 5809 */
                      fname, sockAdd2Str_(&addr), len);
            xdr_destroy(&xdrs);
            return;
        }
        xdr_destroy(&xdrs);
        return;
    } else {
        updExtraLoad(destHostPtr, jobXferPtr->resReq, num);
    }

    for (i = 0; i < num; i++) {

        if (myClusterPtr->masterKnown
            && destHostPtr[i] == myClusterPtr->masterPtr)
            continue;

        if (destHostPtr[i] == myHostPtr) {
            updExtraLoad(destHostPtr, jobXferPtr->resReq, num);
            continue;
        }

        if (!destHostPtr[i]->addr)
            continue;

        memcpy(&addr.sin_addr,
               destHostPtr[i]->addr,
               sizeof(struct in_addr));
        xdrmem_create(&xdrs, buf, MSGSIZE, XDR_ENCODE);
        reqHdr.opCode  = limReqCode;
        reqHdr.refCode = 0;

        if (!(xdr_LSFHeader(&xdrs, &reqHdr) &&
              xdr_jobXfer(&xdrs, jobXferPtr, &reqHdr))) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname,
                      "xdr_LSFHeader/xdr_jobXfer");
            xdr_destroy(&xdrs);
            return;
        }
        len = XDR_GETPOS(&xdrs);

        if (logclass & LC_COMM)
            ls_syslog(LOG_DEBUG, "loadAdj: inform destination host %s (len=%d) of job xfer", sockAdd2Str_(&addr), len);

        if (chanSendDgram_(limSock, buf, len, &addr) < 0 ) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5811,
                                             "%s: Failed to inform destination host %s (len=%d) of job xfer: %m"), fname, sockAdd2Str_(&addr), len); /* catgets 5811 */
            xdr_destroy(&xdrs);
            return;
        }
        xdr_destroy(&xdrs);
    }

    return;
}

void
loadadjReq(XDR *xdrs, struct sockaddr_in *from, struct LSFHeader *reqHdr, int s)
{
    static char fname[] = "loadadjReq";
    XDR  xdrs2;
    char buf[MSGSIZE];
    struct jobXfer  jobXfer;
    struct resVal resVal;
    enum limReplyCode limReplyCode;
    int i, j, k, cc, returnCode;
    struct LSFHeader replyHdr;
    struct hostNode *candidate;
    struct tclHostData tclHostData;

    if (logclass & (LC_TRACE | LC_HANG | LC_COMM))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    initResVal (&resVal);

    ignDedicatedResource = FALSE;
    if (!masterMe) {

        wrongMaster(from, buf, reqHdr, -1);
        return;
    }

    if (!xdr_jobXfer(xdrs, &jobXfer, reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_jobXfer");
        limReplyCode = LIME_BAD_DATA;
        goto reply;
    }

    if (initCandList() < 0) {
        limReplyCode = LIME_NO_MEM;
        goto reply;
    }


    getTclHostData (&tclHostData, myHostPtr, myHostPtr, TRUE);
    tclHostData.ignDedicatedResource = ignDedicatedResource;
    cc=parseResReq(jobXfer.resReq, &resVal, &allInfo, PR_RUSAGE, unitForLimits);
    if ((cc != PARSE_OK) ||
        (returnCode = evalResReq(resVal.selectStr, &tclHostData, FALSE)) < 0) {
        if (cc == PARSE_BAD_VAL)
            limReplyCode = LIME_UNKWN_RVAL;
        else if (cc == PARSE_BAD_NAME)
            limReplyCode = LIME_UNKWN_RNAME;
        else
            limReplyCode = LIME_BAD_RESREQ;
        goto reply;
    }

    j = 0;
    for (i=0; i<jobXfer.numHosts; i++) {
        candidate = findHostbyList(myClusterPtr->hostList,
                                   jobXfer.placeInfo[i].hostName);
        if (candidate == NULL)
            continue;

        for (k=0; k < j; k++) {
            if (candidate == candidates[j])
                break;
        }

        if (k == j) {
            if (addCandList(candidate, j) < 0) {
                limReplyCode = LIME_NO_MEM;
                goto reply;
            }
            candidates[j]->use = jobXfer.placeInfo[i].numtask;
            j++;
        } else
            candidates[k]->use += jobXfer.placeInfo[i].numtask;
    }

    loadAdj(&jobXfer, candidates, j, FALSE);
    limReplyCode = LIME_NO_ERR;

reply:

    freeResVal (&resVal);

    initLSFHeader_(&replyHdr);
    replyHdr.opCode  = (short) limReplyCode;
    replyHdr.refCode = reqHdr->refCode;
    xdrmem_create(&xdrs2, buf, MSGSIZE, XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs2, &replyHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
        xdr_destroy(&xdrs2);
        return;
    }

    if (chanWrite_(s, buf, XDR_GETPOS(&xdrs2)) < 0) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5814,
                                         "%s: Failed to send load adjustment decision to %s (len=%d): %m"), fname, sockAdd2Str_(from), XDR_GETPOS(&xdrs2)); /* catgets 5814 */
        xdr_destroy(&xdrs2);
        return;
    }

    xdr_destroy(&xdrs2);
    return;
}

void
updExtraLoad(struct hostNode **destHostPtr, char *resReq, int numHosts)
{
    static char fname[] = "updExtraLoad";
    int j, lidx;
    struct resVal resVal;
    time_t jtime;
    float dupfactor, exval;

    if (!destHostPtr) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5815,
                                         "%s: Null host pointer"), fname); /* catgets 5815 */
        return;
    }
    initResVal (&resVal);

    if (parseResReq(resReq, &resVal, &allInfo, PR_RUSAGE, unitForLimits) != PARSE_OK) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "parseResReq", resReq);
        return;
    }

    mustSendLoad = TRUE;
    jtime = time(0);
    for (j=0; j<numHosts; j++) {


        dupfactor =  MIN((float) destHostPtr[j]->use,
                         (float) destHostPtr[j]->statInfo.maxCpus);

        if (dupfactor < 0)
            dupfactor = 1.0;

        if (jtime < destHostPtr[j]->lastJackTime + keepTime * exchIntvl)
            dupfactor *= 0.2;

        destHostPtr[j]->lastJackTime = jtime;
        for (lidx = 0; lidx < NBUILTINDEX; lidx++) {
            if ( lidx == R15M)
                continue;


            if (resVal.genClass & (1 << lidx)) {
                exval = fabs(resVal.val[lidx]);
                if (exval < INFINIT_LOAD ) {
                    if (li[lidx].increasing) {
                        exval = MIN(exval, li[lidx].extraload[1]);
                    } else {
                        exval = -exval;
                        exval = MAX(exval, li[lidx].extraload[1]);
                    }
                } else
                    exval = li[lidx].extraload[1];

            } else {
                exval = li[lidx].extraload[0];
            }
            jackup(lidx, destHostPtr[j],  exval*dupfactor);


            if (limParams[LSF_LIM_JACKUP_BUSY].paramValue != NULL) {
                setBusyIndex(lidx, destHostPtr[j]);
            }
        }
    }

    freeResVal (&resVal);

}

static void
jackup(int lidx, struct hostNode *hostPtr, float exval)
{

    if (LS_ISUNAVAIL(hostPtr->status))
        return;

    if (lidx == R15S || lidx == R1M || lidx == R15M) {

        hostPtr->uloadIndex[lidx] += exval;


        hostPtr->loadIndex[lidx] = normalizeRq(hostPtr->uloadIndex[lidx],
                                               (hostPtr->hModelNo >= 0) ?
                                               shortInfo.cpuFactors[hostPtr->hModelNo]
                                               : 1.0,
                                               hostPtr->statInfo.maxCpus);
        if (hostPtr == myHostPtr) {
            jobxfer = keepTime;
            extraload[lidx] = exval ;
        }
        return;
    }

    hostPtr->loadIndex[lidx] += exval;
    if (hostPtr->loadIndex[lidx] < 0)
        hostPtr->loadIndex[lidx] = 0.0;

    if ((lidx == UT) && (hostPtr->loadIndex[lidx] >1))
        hostPtr->loadIndex[lidx] = 1.0;

    if (hostPtr == myHostPtr) {
        jobxfer = keepTime;
        extraload[lidx] = exval;
    }

}

void
loadReq(XDR *xdrs, struct sockaddr_in *from, struct LSFHeader *reqHdr, int s)
{
    static char fname[] = "loadReq";
    struct loadReply reply;
    struct decisionReq ldReq;
    XDR xdrs2;
    char   *buf;
    register int i, j, k;
    struct resVal resVal;
    enum limReplyCode limReplyCode;
    int ncandidates, returnCode;
    int ignore_res, cc, propt, hlSize, lvecSize, bufSize, staSize;
    struct LSFHeader replyHdr;
    char *replyStruct;
    char  fromEligible, clName;
    char *currp;
    struct tclHostData tclHostData;

    if (logclass & (LC_TRACE | LC_HANG | LC_COMM))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    initResVal (&resVal);
    reply.indicies = NULL;
    ignDedicatedResource = FALSE;
    reply.nEntry = 0;
    reply.flags = 0;

    if (!xdr_decisionReq(xdrs, &ldReq, reqHdr)) {
        limReplyCode = LIME_BAD_DATA;
        goto Reply1;
    }

    if (! (ldReq.ofWhat == OF_HOSTS && ldReq.numPrefs == 2
           && ldReq.numHosts == 1
           && equalHost_(ldReq.preferredHosts[1], myHostPtr->hostName))) {

        if (!masterMe) {
            char tmpBuf[MSGSIZE];

            wrongMaster(from, tmpBuf, reqHdr, s);
            for (i=0;i<ldReq.numPrefs;i++)
                free(ldReq.preferredHosts[i]);
            free(ldReq.preferredHosts);
            return;
        }
    }

    if (!validHosts(ldReq.preferredHosts,
                    ldReq.numPrefs,
                    &clName,
                    ldReq.options)) {
        limReplyCode = LIME_UNKWN_HOST;
        ls_syslog(LOG_INFO, (_i18n_msg_get(ls_catd , NL_SETN, 5823, "%s: validHosts() failed for bad cluster/host name requested from <%s>")), fname, sockAdd2Str_(from));         /* catgets 5823 */
        goto Reply;
    }

    propt = PR_SELECT | PR_ORDER | PR_FILTER;
    if (ldReq.options & DFT_FROMTYPE)
        propt |= PR_DEFFROMTYPE;

    getTclHostData (&tclHostData, myHostPtr, myHostPtr, TRUE);
    tclHostData.ignDedicatedResource = ignDedicatedResource;
    cc = parseResReq(ldReq.resReq, &resVal, &allInfo, propt, unitForLimits);
    if((cc != PARSE_OK) ||
       (returnCode = evalResReq(resVal.selectStr,
                                &tclHostData, ldReq.options & DFT_FROMTYPE)) < 0) {
        if (cc == PARSE_BAD_VAL)
            limReplyCode = LIME_UNKWN_RVAL;
        else if (cc == PARSE_BAD_NAME)
            limReplyCode = LIME_UNKWN_RNAME;
        else if (cc == PARSE_BAD_FILTER)
            limReplyCode = LIME_BAD_FILTER;
        else
            limReplyCode = LIME_BAD_RESREQ;
        goto Reply;
    }

    fromHostPtr = findHost(ldReq.preferredHosts[0]);
    if (!fromHostPtr) {
        limReplyCode = LIME_NAUTH_HOST;
        goto Reply;
    }
    if (strcmp(ldReq.hostType, " ") == 0)
        strcpy(ldReq.hostType,
               (fromHostPtr->hTypeNo >= 0) ?
               shortInfo.hostTypes[fromHostPtr->hTypeNo] : "unknown");

    fromEligible = FALSE;
    ncandidates = getEligibleSites(&resVal, &ldReq, 0, &fromEligible);
    if (!fromEligible)
        fromHostPtr = NULL;

    if (ncandidates <= 0 ) {
        limReplyCode = ncandidates ? LIME_NO_MEM : LIME_NO_OKHOST;
        goto Reply;
    }

    ignore_res = (ldReq.options & IGNORE_RES);

    if ((ncandidates < ldReq.numHosts) && (ldReq.options & EXACT)) {
        limReplyCode = LIME_NO_OKHOST;
        goto Reply;
    }

    if (ldReq.options & OK_ONLY) {
        ncandidates = getOkSites(ncandidates, 0, ignore_res);
        if ((ncandidates == 0) || ((ncandidates < ldReq.numHosts)
                                   && (ldReq.options & EXACT))) {
            limReplyCode = LIME_NO_OKHOST;
            goto Reply;
        }
    } else {
        ncandidates = getOkSites(ncandidates, ldReq.numHosts, ignore_res);
    }

    if (logclass & LC_SCHED)
        ls_syslog(LOG_DEBUG2,"ldReq: ncandidates=%d ldReq.numHosts=%d clName=%d",
                  ncandidates, ldReq.numHosts, clName);
    if (ncandidates > ldReq.numHosts)  {
        findBestHost(&resVal, ldReq.numHosts, ldReq.numPrefs,
                     ldReq.preferredHosts, ncandidates, FALSE, ignore_res, ldReq.options);
        reply.nEntry = ldReq.numHosts;
    } else {
        findBestHost(&resVal, ncandidates, ldReq.numPrefs,
                     ldReq.preferredHosts, ncandidates, FALSE, ignore_res, ldReq.options);
        reply.nEntry = ncandidates;
    }


    reply.nIndex = resVal.nindex;
    hlSize   =  ALIGNWORD_(reply.nEntry * sizeof(struct hostLoad));
    lvecSize =  ALIGNWORD_(reply.nIndex * sizeof(float));
    staSize = ALIGNWORD_((1 + GET_INTNUM(reply.nIndex)) * sizeof(int));
    reply.loadMatrix = (struct hostLoad *)
        malloc(hlSize + reply.nEntry * (lvecSize + staSize));
    if (reply.loadMatrix == (struct hostLoad *)NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        limReplyCode = LIME_NO_MEM;
        goto Reply;
    }

    currp = (char *) reply.loadMatrix;
    currp += hlSize;
    for (i=0; i < reply.nEntry; i++, currp += lvecSize)
        reply.loadMatrix[i].li = (float *)currp;
    for (i=0; i < reply.nEntry; i++, currp += staSize)
        reply.loadMatrix[i].status = (int *)currp;

    limReplyCode = LIME_NO_ERR;
    if (!(reply.indicies=(char **)malloc((allInfo.numIndx+1)*sizeof(char *)))) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        return;
    }


    for (j=0; j < resVal.nindex; j++)  {
        k = resVal.indicies[j];
        reply.indicies[j] = allInfo.resTable[k].name;
    }

    for (i=0; i < reply.nEntry; i++) {
        strcpy(reply.loadMatrix[i].hostName, candidates[i]->hostName);
        if (definedSharedResource(candidates[i], &allInfo) == TRUE) {
            reply.flags |= LOAD_REPLY_SHARED_RESOURCE;
        }
        reply.loadMatrix[i].status[0] = candidates[i]->status[0];
        if (LS_ISUNAVAIL(candidates[i]->status)) {
            for (j=0; j < resVal.nindex ; j++)
                reply.loadMatrix[i].li[j] = INFINIT_LOAD;
            for (j=0; j < GET_INTNUM(resVal.nindex); j++)
                reply.loadMatrix[i].status[j + 1] = 0;
            continue;
        }


        for (j = 0; j < GET_INTNUM(reply.nIndex); j++)
            reply.loadMatrix[i].status[j+1] = 0;
        for (j=0; j < reply.nIndex; j++) {
            int indx;
            indx =  resVal.indicies[j];
            if (LS_ISBUSYON(candidates[i]->status, indx))
                SET_BIT(INTEGER_BITS + j, reply.loadMatrix[i].status);
            if (indx==R15S || indx==R1M || indx==R15M) {
                if (ldReq.options & NORMALIZE)
                    reply.loadMatrix[i].li[j] = candidates[i]->loadIndex[indx];
                else if (ldReq.options & EFFECTIVE) {
                    float factor;
                    factor = (candidates[i]->hModelNo >= 0) ?
                        shortInfo.cpuFactors[candidates[i]->hModelNo] : 1.0;
                    reply.loadMatrix[i].li[j]
                        = effectiveRq(candidates[i]->loadIndex[indx],factor);

                    if (reply.loadMatrix[i].li[j] < 0.0)
                        reply.loadMatrix[i].li[j] = 0.0;
                } else
                    reply.loadMatrix[i].li[j] = candidates[i]->uloadIndex[indx];
            } else {
                reply.loadMatrix[i].li[j] = candidates[i]->loadIndex[indx];
            }
        }
    }

Reply:
    for (i=0; i < ldReq.numPrefs; i++)
        free(ldReq.preferredHosts[i]);
    free(ldReq.preferredHosts);

Reply1:
    freeResVal (&resVal);

    initLSFHeader_(&replyHdr);
    replyHdr.opCode  = (short) limReplyCode;
    replyHdr.refCode = reqHdr->refCode;
    if (limReplyCode == LIME_NO_ERR) {
        replyStruct = (char *)&reply;
        bufSize = ALIGNWORD_(MAXLSFNAMELEN * allInfo.numIndx
                             + hlSize + reply.nEntry * (lvecSize + staSize));
    } else  {
        replyStruct = (char *)NULL;
        bufSize = 512;
    }

    buf = (char *)malloc(bufSize);
    if (!buf) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
        if (limReplyCode == LIME_NO_ERR)
            FREEUP(reply.loadMatrix);
        FREEUP (reply.indicies);
        return ;
    }

    xdrmem_create(&xdrs2, buf, bufSize, XDR_ENCODE);
    if (!xdr_encodeMsg(&xdrs2,
                       replyStruct,
                       &replyHdr,
                       xdr_loadReply,
                       0,
                       NULL)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_encodeMsg");
        xdr_destroy(&xdrs2);
        if (limReplyCode == LIME_NO_ERR)
            FREEUP(reply.loadMatrix);
        FREEUP(buf);
        FREEUP (reply.indicies);
        return ;
    }
    if (limReplyCode == LIME_NO_ERR)
        free(reply.loadMatrix);
    FREEUP (reply.indicies);

    if (s < 0)
        cc = chanSendDgram_(limSock, buf, XDR_GETPOS(&xdrs2), from);
    else
        cc = chanWrite_(s, buf, XDR_GETPOS(&xdrs2));

    free(buf);

    if (cc < 0) {
        ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 5821,
                                         "%s: Failed in sending lsload reply to %s (len=%d): %m"), fname, sockAdd2Str_(from), XDR_GETPOS(&xdrs2)); /* catgets 5821 */
        xdr_destroy(&xdrs2);
        return;
    }

    xdr_destroy(&xdrs2);

    return;

}

static int
initCandList(void)
{

    FREEUP(candidates);

    candListSize = 256;
    candidates = calloc(candListSize, sizeof(struct hostNode *));
    if (!candidates) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, "initCandList", "calloc");
        candListSize = 0;
        return(-1);
    }
    return(0);

}

static int
addCandList(struct hostNode *hPtr, int pos)
{
    char *memp;

    if (pos >= candListSize) {
        candListSize *= 2;
        memp = realloc(candidates, candListSize * sizeof(struct hostNode *));
        if (!memp) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, "addCandList", "realloc");
            FREEUP(candidates);
            candListSize=0;
            return(-1);
        }
        candidates = (struct hostNode **)memp;
    }
    candidates[pos] = hPtr;
    return(0);
}

void
chkResReq(XDR *xdrs, struct sockaddr_in *from, struct LSFHeader *reqHdr)
{
    static char fname[]="chkResReq";
    int cc;
    struct resVal resVal;
    struct LSFHeader replyHdr, replyBuf;
    char  resReq[MAXLINELEN];
    enum  limReplyCode limReplyCode;
    XDR   xdrs2;
    char  *sp;
    struct tclHostData tclHostData;

    initResVal (&resVal);

    sp = resReq;
    sp[0] = '\0';

    if (!xdr_string(xdrs, &sp, MAXLINELEN)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_string");
        limReplyCode = LIME_BAD_DATA;
        goto Reply;
    }

    limReplyCode = LIME_NO_ERR;
    getTclHostData (&tclHostData, myHostPtr, myHostPtr, TRUE);
    cc = parseResReq(resReq, &resVal, &allInfo, PR_ALL, unitForLimits);
    if (cc != PARSE_OK ||
        evalResReq(resVal.selectStr, &tclHostData, FALSE) < 0) {
        if (cc == PARSE_BAD_VAL)
            limReplyCode = LIME_UNKWN_RVAL;
        else if (cc == PARSE_BAD_NAME)
            limReplyCode = LIME_UNKWN_RNAME;
        else
            limReplyCode = LIME_BAD_RESREQ;
    }

Reply:
    freeResVal (&resVal);
    replyHdr.opCode  = (short) limReplyCode;
    replyHdr.refCode = reqHdr->refCode;

    xdrmem_create(&xdrs2, (char *)&replyBuf, sizeof(struct LSFHeader), XDR_ENCODE);
    if (!xdr_LSFHeader(&xdrs2, &replyHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
        xdr_destroy(&xdrs2);
        return;
    }

    if (chanSendDgram_(limSock, (char *)&replyBuf, XDR_GETPOS(&xdrs2),  from) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL_M, fname, "chanSendDgram_",
                  sockAdd2Str_(from));
        xdr_destroy(&xdrs2);
        return;
    }

    xdr_destroy(&xdrs2);
    return;
}
void
getTclHostData (struct tclHostData *tclHostData, struct hostNode *hostNode, struct hostNode *fromHostNode, int checkSyntax)
{

    tclHostData->hostName = hostNode->hostName;
    tclHostData->maxCpus = hostNode->statInfo.maxCpus;
    tclHostData->maxMem = hostNode->statInfo.maxMem;
    tclHostData->maxSwap = hostNode->statInfo.maxSwap;
    tclHostData->maxTmp = hostNode->statInfo.maxTmp;
    tclHostData->nDisks = hostNode->statInfo.nDisks;

    tclHostData->hostInactivityCount = hostNode->hostInactivityCount;
    tclHostData->status = hostNode->status;
    tclHostData->loadIndex = hostNode->loadIndex;
    tclHostData->rexPriority = hostNode->rexPriority;
    tclHostData->hostType = (hostNode->hTypeNo >= 0) ?
        shortInfo.hostTypes[hostNode->hTypeNo] : "unknown";
    tclHostData->hostModel = (hostNode->hModelNo >= 0) ?
        shortInfo.hostModels[hostNode->hModelNo] : "unknown";
    tclHostData->fromHostType = (fromHostNode->hTypeNo >= 0) ?
        shortInfo.hostTypes[fromHostNode->hTypeNo] : "unknown";
    tclHostData->fromHostModel = (fromHostNode->hModelNo >= 0) ?
        shortInfo.hostModels[fromHostNode->hModelNo] : "unknown";
    tclHostData->cpuFactor = (hostNode->hModelNo >= 0) ?
        shortInfo.cpuFactors[hostNode->hModelNo] : 1.0;
    tclHostData->DResBitMaps = hostNode->DResBitMaps;
    tclHostData->ignDedicatedResource = ignDedicatedResource;
    tclHostData->resBitMaps = hostNode->resBitMaps;
    tclHostData->numResPairs = hostNode->numInstances;
    tclHostData->resPairs = getResPairs(hostNode);
    if (checkSyntax == TRUE)
        tclHostData->flag = TCL_CHECK_SYNTAX;
    else
        tclHostData->flag = TCL_CHECK_EXPRESSION;
    tclHostData->slots = 0;

}

static void
setBusyIndex(int lidx, struct hostNode *host)
{
    float load;

    if (lidx == R15S
        || lidx == R1M) {
        load = normalizeRq(host->uloadIndex[lidx],
                           1,
                           ncpus) - 1;
    } else {
        load = host->loadIndex[lidx];
    }

    if ( ! THRLDOK(li[lidx].increasing,
                   load,
                   host->busyThreshold[lidx])) {

        SET_BIT(lidx + INTEGER_BITS, host->status);

        host->status[0] |= LIM_BUSY;
    }

}

static float
loadIndexValue(int hostIdx, int loadIdx, int rqlOptions)
{
    float loadIndex;

    if (loadIdx == R15S || loadIdx == R1M || loadIdx == R15M) {
        if (rqlOptions & NORMALIZE) {
            loadIndex = candidates[hostIdx]->loadIndex[loadIdx];
        } else if (rqlOptions & EFFECTIVE) {
            float factor;
            factor = (candidates[hostIdx]->hModelNo >= 0) ?
                shortInfo.cpuFactors[candidates[hostIdx]->hModelNo] : 1.0;
            loadIndex = effectiveRq(candidates[hostIdx]->loadIndex[loadIdx],
                                    factor);

            if (loadIndex < 0.0)
                loadIndex = 0.0;
        } else {
            loadIndex = candidates[hostIdx]->uloadIndex[loadIdx];
        }
    } else {
        loadIndex = candidates[hostIdx]->loadIndex[loadIdx];
    }
    return (loadIndex);
}


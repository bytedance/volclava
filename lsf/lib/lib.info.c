/* $Id: lib.info.c 397 2007-11-26 19:04:00Z mblack $
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

#include "lib.h"
#include "lib.xdr.h"
#include "lib.xdrlim.h"
#include "lproto.h"

struct masterInfo masterInfo_;
int    masterknown_ = FALSE;

static struct hostInfo *expandSHinfo(struct hostInfoReply *);
static struct clusterInfo *expandSCinfo(struct clusterInfoReply *);
static int copyAdmins_(struct clusterInfo *, struct  shortCInfo *);
static int getname_(enum limReqCode, char *, int);

char *
ls_getclustername(void)
{

    static char fname[] = "ls_getclustername";
    static char clName[MAXLSFNAMELEN];

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG, "%s: Entering this routine...", fname);

    if (clName[0] == '\0')
        if (getname_(LIM_GET_CLUSNAME, clName, MAXLSFNAMELEN) < 0)
            return NULL;

    return (clName);

}

int
expandList1_(char ***tolist, int num, int *bitmMaps, char **keys)
{

    int ii, jj, isSet;
    char **temp;

    if (num <= 0)
        return 0;

    if ((temp = (char **)calloc (num,  sizeof (char *))) == NULL)  {
        lserrno = LSE_MALLOC;
        return (-1);
    }
    for (ii=0, jj=0; ii <num; ii++) {
        TEST_BIT(ii, bitmMaps, isSet);
        if (isSet == 1) {
            temp[jj++] = keys[ii];
        }
    }
    if (jj > 0) {
        *tolist = temp;
    } else {
        FREEUP (temp);
        *tolist = NULL;
    }
    return(jj);
}

int
expandList_(char ***tolist, int mask, char **keys)
{
    int i,j;
    char *temp[32];

    for(i=0,j=0; i < 32; i++) {
        if (mask & (1 << i))
            temp[j++] = keys[i];
    }
    if (j > 0) {
        *tolist = calloc(j, sizeof (char *));
        if (!*tolist) {
            lserrno = LSE_MALLOC;
            return(-1);
        }
        for(i=0; i < j; i++)
            (*tolist)[i] = temp[i];
    } else
        *tolist = NULL;
    return(j);
}

static int
copyAdmins_(struct clusterInfo *clusPtr, struct  shortCInfo *clusShort)
{
    int i,j;

    if (clusShort->nAdmins <= 0)
        return 0;

    clusPtr->adminIds = calloc(clusShort->nAdmins, sizeof (int));
    clusPtr->admins = calloc(clusShort->nAdmins, sizeof (char *));

    if (!clusPtr->admins || !clusPtr->adminIds)
        goto errReturn;

    for (i = 0; i < clusShort->nAdmins; i++) {
        clusPtr->admins[i] = NULL;
        clusPtr->adminIds[i] = clusShort->adminIds[i];
        clusPtr->admins[i] = putstr_( clusShort->admins[i]);
        if (clusPtr->admins[i] == NULL) {
            for (j = 0; j < i; j++)
                FREEUP (clusPtr->admins[j]);
            goto errReturn;
        }
    }
    return 0;

errReturn:
    FREEUP (clusPtr->admins);
    FREEUP (clusPtr->adminIds);
    lserrno = LSE_MALLOC;
    return(-1);

}

static struct clusterInfo *
expandSCinfo(struct clusterInfoReply *clusterInfoReply)
{
    static int nClus = 0;
    static struct clusterInfo *clusterInfoPtr = NULL;
    struct shortLsInfo *lsInfoPtr;
    int i,j, k;

    if (clusterInfoPtr) {
        for(i=0; i < nClus; i++) {
            free(clusterInfoPtr[i].resources);
            free(clusterInfoPtr[i].hostModels);
            free(clusterInfoPtr[i].hostTypes);
            if (clusterInfoPtr[i].nAdmins > 0) {
                for (j = 0; j < clusterInfoPtr[i].nAdmins; j++)
                    FREEUP (clusterInfoPtr[i].admins[j]);
                FREEUP (clusterInfoPtr[i].admins);
                FREEUP (clusterInfoPtr[i].adminIds);
            }
        }
        FREEUP(clusterInfoPtr);
    }

    clusterInfoPtr = calloc(clusterInfoReply->nClus,
                            sizeof (struct clusterInfo));
    if (!clusterInfoPtr) {
        nClus = 0;
        lserrno = LSE_MALLOC;
        return(NULL);
    }

    nClus          = clusterInfoReply->nClus;
    lsInfoPtr      = clusterInfoReply->shortLsInfo;

    for(i=0; i < clusterInfoReply->nClus; i++) {
        strcpy(clusterInfoPtr[i].clusterName,
               clusterInfoReply->clusterMatrix[i].clName);
        strcpy(clusterInfoPtr[i].masterName,
               clusterInfoReply->clusterMatrix[i].masterName);
        strcpy(clusterInfoPtr[i].managerName,
               clusterInfoReply->clusterMatrix[i].managerName);
        clusterInfoPtr[i].managerId =
            clusterInfoReply->clusterMatrix[i].managerId;
        clusterInfoPtr[i].status =
            clusterInfoReply->clusterMatrix[i].status;
        clusterInfoPtr[i].numServers =
            clusterInfoReply->clusterMatrix[i].numServers;
        clusterInfoPtr[i].numClients =
            clusterInfoReply->clusterMatrix[i].numClients;
        clusterInfoPtr[i].nAdmins =
            clusterInfoReply->clusterMatrix[i].nAdmins;
        if (copyAdmins_(&clusterInfoPtr[i],
                        &clusterInfoReply->clusterMatrix[i]) < 0)
            break;

        clusterInfoPtr[i].resources = NULL;
        clusterInfoPtr[i].hostTypes = NULL;
        clusterInfoPtr[i].hostModels = NULL;

        if (clusterInfoReply->clusterMatrix[i].nRes == 0) {
            clusterInfoPtr[i].nRes =
                expandList_(&clusterInfoPtr[i].resources,
                            clusterInfoReply->clusterMatrix[i].resClass,
                            lsInfoPtr->resName);
        } else {
            clusterInfoPtr[i].nRes = expandList1_(&clusterInfoPtr[i].resources,
                                                  clusterInfoReply->clusterMatrix[i].nRes,
                                                  clusterInfoReply->clusterMatrix[i].resBitMaps,
                                                  lsInfoPtr->resName);
        }
        if (clusterInfoPtr[i].nRes < 0)
            break;

        if (clusterInfoReply->clusterMatrix[i].nTypes == 0) {
            clusterInfoPtr[i].nTypes =
                expandList_(&clusterInfoPtr[i].hostTypes,
                            clusterInfoReply->clusterMatrix[i].typeClass,
                            lsInfoPtr->hostTypes);
        }else {
            clusterInfoPtr[i].nTypes =
                expandList1_(&clusterInfoPtr[i].hostTypes,
                             clusterInfoReply->clusterMatrix[i].nTypes,
                             clusterInfoReply->clusterMatrix[i].hostTypeBitMaps,
                             lsInfoPtr->hostTypes);
        }
        if (clusterInfoPtr[i].nTypes < 0)
            break;

        if (clusterInfoReply->clusterMatrix[i].nModels == 0) {
            clusterInfoPtr[i].nModels =
                expandList_(&clusterInfoPtr[i].hostModels,
                            clusterInfoReply->clusterMatrix[i].modelClass,
                            lsInfoPtr->hostModels);
        } else {
            clusterInfoPtr[i].nModels =
                expandList1_(&clusterInfoPtr[i].hostModels,
                             clusterInfoReply->clusterMatrix[i].nModels,
                             clusterInfoReply->clusterMatrix[i].hostModelBitMaps,
                             lsInfoPtr->hostModels);
        }

        if (clusterInfoPtr[i].nModels < 0)
            break;
    }
    if (i != clusterInfoReply->nClus) {
        for(j=0; j < i; j++) {
            FREEUP(clusterInfoPtr[j].resources);
            FREEUP(clusterInfoPtr[j].hostTypes);
            FREEUP(clusterInfoPtr[j].hostModels);
            if (clusterInfoPtr[j].nAdmins > 0) {
                for (k = 0; k < clusterInfoPtr[j].nAdmins; k++)
                    FREEUP (clusterInfoPtr[j].admins[k]);
                FREEUP (clusterInfoPtr[j].admins);
                FREEUP (clusterInfoPtr[j].adminIds);
            }

        }
        FREEUP(clusterInfoPtr);
        return((struct clusterInfo *) NULL);
    }
    return(clusterInfoPtr);

}

struct clusterInfo *
ls_clusterinfo(char *resReq, int *numclusters, char **clusterList,
               int listsize, int options)
{
    struct clusterInfoReq clusterInfoReq;
    static struct clusterInfoReply clusterInfoReply;
    struct shortLsInfo shortlsInfo;
    int count, ret_val;

    if (listsize != 0 && clusterList == NULL) {
        lserrno = LSE_BAD_ARGS;
        return NULL;
    }

    if (initenv_(NULL, NULL) < 0)
        return (NULL);

    for (count=0;count < listsize;count++) {
        ret_val = ls_isclustername(clusterList[count]);
        if (ret_val <=0){
            if (ret_val < 0) {
                return(NULL);
            }

            lserrno = LSE_BAD_CLUSTER;
            return(NULL);
        }
    }

    if (resReq)
        clusterInfoReq.resReq = resReq;
    else
        clusterInfoReq.resReq = "";

    clusterInfoReq.clusters = clusterList;
    clusterInfoReq.listsize = listsize;
    clusterInfoReq.options = options;

    clusterInfoReply.shortLsInfo = &shortlsInfo;
    if (callLim_(LIM_GET_CLUSINFO, &clusterInfoReq, xdr_clusterInfoReq,
                 &clusterInfoReply, xdr_clusterInfoReply, NULL, 0, NULL) < 0)
        return (NULL);

    if (numclusters != NULL)
        *numclusters = clusterInfoReply.nClus;
    return (expandSCinfo(&clusterInfoReply));

}

char *
ls_getmastername(void)
{
    static char master[MAXHOSTNAMELEN];

    if (getname_(LIM_GET_MASTINFO, master, MAXHOSTNAMELEN) < 0)
        return NULL;

    return master;
}

/* ls_getmastername2()
 * Get the master name by calling the LSF_SERVER_HOSTS
 * only.
 */
char *
ls_getmastername2(void)
{
    static char master[MAXHOSTNAMELEN];

    if (getname_(LIM_GET_MASTINFO2,
                 master,
                 MAXHOSTNAMELEN) < 0)
        return NULL;

    return master;
}

static int
getname_(enum limReqCode limReqCode, char *name, int namesize)
{
    int options;

    if (initenv_(NULL, NULL) < 0)
        return (-1);

    if (limReqCode == LIM_GET_CLUSNAME) {
        struct stringLen str;
        str.name = name;
        str.len  = namesize;
        if (callLim_(limReqCode,
                     NULL,
                     NULL,
                     &str,
                     xdr_stringLen,
                     NULL,
                     _LOCAL_,
                     NULL) < 0)
            return -1;
        return 0;
    }

    /* Force the library not to call _LOCAL_ LIM since
     * it may not know the master yet, this is the
     * case of LIM_ADD_HOST.
     */
    if (limReqCode == LIM_GET_MASTINFO2)
        options = _SERVER_HOSTS_ONLY_;
    else
        options = _LOCAL_;

    if (callLim_(limReqCode,
                 NULL,   /* no data to send */
                 NULL,
                 &masterInfo_,
                 xdr_masterInfo,
                 NULL,  /* host LSF_SERVER_HOSTS */
                 options,
                 NULL) < 0)
        return(-1);

    if (memcmp(&sockIds_[MASTER].sin_addr,
               &masterInfo_.addr,
               sizeof(in_addr_t))) {
        CLOSECD(limchans_[MASTER]);
        CLOSECD(limchans_[TCP]);
    }

    memcpy(&sockIds_[MASTER].sin_addr,
           &masterInfo_.addr, sizeof(in_addr_t));
    memcpy(&sockIds_[TCP].sin_addr,
           &masterInfo_.addr, sizeof(in_addr_t));
    sockIds_[TCP].sin_port = masterInfo_.portno;
    masterknown_ = TRUE;
    strncpy(name, masterInfo_.hostName, namesize);
    name[namesize - 1] = '\0';

    return 0;
}

char *
ls_gethosttype(char *hostname)
{
    struct hostInfo *hostinfo;
    static char hostType[MAXLSFNAMELEN];

    if (hostname == NULL)
        if ((hostname = ls_getmyhostname()) == NULL)
            return(NULL);

    hostinfo = ls_gethostinfo("-", NULL, &hostname, 1, 0);
    if (hostinfo == NULL)
        return (NULL);

    strcpy(hostType, hostinfo[0].hostType);
    return (hostType);

}

char *
ls_gethostmodel(char *hostname)
{
    struct hostInfo *hostinfo;
    static char hostModel[MAXLSFNAMELEN];

    if (hostname == NULL)
        if ((hostname = ls_getmyhostname()) == NULL)
            return(NULL);

    hostinfo = ls_gethostinfo("-", NULL, &hostname, 1, 0);
    if (hostinfo == NULL)
        return (NULL);

    strcpy(hostModel, hostinfo[0].hostModel);
    return hostModel;

}

float *
ls_gethostfactor(char *hostname)
{
    struct hostInfo *hostinfo;
    static float cpufactor;

    if (hostname == NULL)
        if ((hostname = ls_getmyhostname()) == NULL)
            return(NULL);

    hostinfo = ls_gethostinfo("-", NULL, &hostname, 1, 0);
    if (hostinfo == NULL)
        return (NULL);

    cpufactor = hostinfo->cpuFactor;
    return (&cpufactor);

}

float *
ls_getmodelfactor(char *modelname)
{
    static float cpuf;
    struct stringLen str;

    if (!modelname)
        return(ls_gethostfactor(NULL));

    if (initenv_(NULL, NULL) < 0)
        return (NULL);

    str.name = modelname;
    str.len = MAXLSFNAMELEN;
    if (callLim_(LIM_GET_CPUF, &str, xdr_stringLen, &cpuf, xdr_float,
                 NULL, 0, NULL) < 0)
        return (NULL);

    return (&cpuf);

}

static struct hostInfo *
expandSHinfo(struct hostInfoReply *hostInfoReply)
{
    static int nHost = 0;
    static struct hostInfo  *hostInfoPtr = NULL;
    struct shortLsInfo    *lsInfoPtr;
    int i, j,indx;

    if (hostInfoPtr) {
        for(i=0; i < nHost; i++) {
            FREEUP(hostInfoPtr[i].resources);
        }
        FREEUP(hostInfoPtr);
    }

    hostInfoPtr = (struct hostInfo *) malloc( (int) hostInfoReply->nHost *
                                              sizeof(struct hostInfo) );
    if (!hostInfoPtr) {
        nHost = 0;
        lserrno = LSE_MALLOC;
        return(NULL);
    }

    nHost       = hostInfoReply->nHost;
    lsInfoPtr   = hostInfoReply->shortLsInfo;

    for (i=0; i < hostInfoReply->nHost; i++) {
        strcpy(hostInfoPtr[i].hostName, hostInfoReply->hostMatrix[i].hostName);
        hostInfoPtr[i].windows = hostInfoReply->hostMatrix[i].windows;
        hostInfoPtr[i].maxCpus = hostInfoReply->hostMatrix[i].maxCpus;
        hostInfoPtr[i].maxMem  = hostInfoReply->hostMatrix[i].maxMem;
        hostInfoPtr[i].maxSwap = hostInfoReply->hostMatrix[i].maxSwap;
        hostInfoPtr[i].maxTmp  = hostInfoReply->hostMatrix[i].maxTmp;
        hostInfoPtr[i].nDisks   = hostInfoReply->hostMatrix[i].nDisks;
        hostInfoPtr[i].isServer    = (hostInfoReply->hostMatrix[i].flags & HINFO_SERVER) != 0;
        hostInfoPtr[i].rexPriority = hostInfoReply->hostMatrix[i].rexPriority;

        hostInfoPtr[i].numIndx = hostInfoReply->nIndex;
        hostInfoPtr[i].busyThreshold =
            hostInfoReply->hostMatrix[i].busyThreshold;

        indx = hostInfoReply->hostMatrix[i].hTypeIndx;
        hostInfoPtr[i].hostType = (indx == MAXTYPES) ?
            "unknown" : lsInfoPtr->hostTypes[indx];
        indx = hostInfoReply->hostMatrix[i].hModelIndx;
        hostInfoPtr[i].hostModel = (indx == MAXMODELS) ?
            "unknown" : lsInfoPtr->hostModels[indx];
        hostInfoPtr[i].cpuFactor = (indx == MAXMODELS) ?
            1.0 : lsInfoPtr->cpuFactors[indx];

        if (hostInfoReply->hostMatrix[i].nRInt == 0)
            hostInfoPtr[i].nRes = expandList_(&hostInfoPtr[i].resources,
                                              hostInfoReply->hostMatrix[i].resClass,
                                              lsInfoPtr->resName);
        else
            hostInfoPtr[i].nRes = expandList1_(&hostInfoPtr[i].resources,
                                               lsInfoPtr->nRes,
                                               hostInfoReply->hostMatrix[i].resBitMaps,
                                               lsInfoPtr->resName);
        if (hostInfoPtr[i].nRes < 0)
            break;
    }

    if (i !=  hostInfoReply->nHost) {
        for(j=0; j < i; j++)
            free(hostInfoPtr[j].resources);
        FREEUP(hostInfoPtr);
        lserrno = LSE_MALLOC;
        return((struct hostInfo *)NULL);
    }
    return(hostInfoPtr);
}

struct hostInfo *
ls_gethostinfo(char *resReq, int *numhosts, char **hostlist, int listsize,
               int options)
{
    static char fname[] = "ls_gethostinfo";
    struct decisionReq hostInfoReq;
    static struct hostInfoReply hostInfoReply;
    struct shortLsInfo lsInfo;
    char   *hname;
    int    cc, i;

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);

    if ((hname = ls_getmyhostname()) == NULL)
        return(NULL);

    if (listsize) {
        if (hostlist == NULL) {
            lserrno = LSE_BAD_ARGS;
            return NULL;
        }
        hostInfoReq.preferredHosts = (char **)
            calloc(listsize+1, sizeof (char *));
        if (hostInfoReq.preferredHosts == NULL) {
            lserrno = LSE_MALLOC;
            return NULL;
        }
        for (i = 0; i < listsize; i++) {
            if (hostlist[i] == NULL) {
                lserrno = LSE_BAD_ARGS;
                break;
            }

            cc = ls_isclustername(hostlist[i]);
            if (cc <= 0) {
                if (cc < 0)
                    break;
                if (Gethostbyname_(hostlist[i]) == NULL) {
                    lserrno = LSE_BAD_HOST;
                    break;
                }
                hostInfoReq.preferredHosts[i + 1] = putstr_(hostlist[i]);
            } else
                hostInfoReq.preferredHosts[i + 1] = putstr_(hostlist[i]);

            if (!hostInfoReq.preferredHosts[i + 1]) {
                lserrno = LSE_MALLOC;
                break;
            }
        }
        if (i < listsize) {
            int j;
            for (j = 1; j < i + 1; j++)
                free(hostInfoReq.preferredHosts[j]);
            free(hostInfoReq.preferredHosts);
            return (NULL);
        }
        hostInfoReq.ofWhat = OF_HOSTS;
    } else {
        hostInfoReq.preferredHosts = (char **) calloc(1, sizeof (char *));
        if (hostInfoReq.preferredHosts == NULL) {
            lserrno = LSE_MALLOC;
            return NULL;
        }
        hostInfoReq.ofWhat = OF_ANY;
    }
    hostInfoReq.options = options;
    strcpy(hostInfoReq.hostType, " ");

    hostInfoReq.preferredHosts[0] = putstr_(hname);
    hostInfoReq.numPrefs = listsize + 1;

    if (resReq != NULL)
        strcpy(hostInfoReq.resReq, resReq);
    else
        strcpy(hostInfoReq.resReq, " ");

    hostInfoReply.shortLsInfo = &lsInfo;
    hostInfoReq.numHosts=0;
    cc = callLim_(LIM_GET_HOSTINFO,
                  &hostInfoReq,
                  xdr_decisionReq,
                  &hostInfoReply,
                  xdr_hostInfoReply,
                  NULL, _USE_TCP_, NULL);

    for (i=0; i < hostInfoReq.numPrefs; i++)
        free(hostInfoReq.preferredHosts[i]);
    free(hostInfoReq.preferredHosts);
    if (cc < 0){
        *numhosts = 0;
        return NULL;
    }

    if (numhosts != NULL)
        *numhosts = hostInfoReply.nHost;
    return (expandSHinfo(&hostInfoReply));

}

struct lsInfo *
ls_info(void)
{
    static struct lsInfo lsInfo;

    if (initenv_(NULL, NULL) < 0)
        return NULL;

    if (callLim_(LIM_GET_INFO,
                 NULL,
                 NULL,
                 &lsInfo,
                 xdr_lsInfo,
                 NULL,
                 _USE_TCP_,
                 NULL) < 0)
        return NULL;

    return &lsInfo;
}


char **
ls_indexnames(struct lsInfo *lsInfo)
{
    static char **indicies = NULL;
    int i, j;

    if (!lsInfo) {
        lsInfo = ls_info();
        if (!lsInfo)
            return NULL;
    }

    FREEUP (indicies);

    for (i=0,j=0; i < lsInfo->nRes; i++) {
        if ((lsInfo->resTable[i].flags & RESF_DYNAMIC)
            && (lsInfo->resTable[i].flags & RESF_GLOBAL)) {
            j++;
        }
    }
    if (!(indicies = (char **)malloc(sizeof(char *) * (j + 1)))) {
        lserrno = LSE_MALLOC;
        return NULL;
    }

    for (i=0,j=0; i < lsInfo->nRes; i++) {
        if ((lsInfo->resTable[i].flags & RESF_DYNAMIC)
            && (lsInfo->resTable[i].flags & RESF_GLOBAL)) {
            indicies[j] = lsInfo->resTable[i].name;
            j++;
        }
    }
    indicies[j] = NULL;
    return(indicies);

}

int
ls_isclustername(char *name)
{
    char* clname;

    clname = ls_getclustername();
    if (clname && strcmp(clname,name) == 0)
        return(1);
    return (0);
}

struct lsSharedResourceInfo *
ls_sharedresourceinfo(char **resources, int *numResources, char *hostName, int options)
{
    static char fname[] = "ls_sharedresourceinfo";
    static struct resourceInfoReq  resourceInfoReq;
    int cc, i;
    static struct resourceInfoReply resourceInfoReply;
    static struct LSFHeader replyHdr;
    static int first = TRUE;

    if (logclass & (LC_TRACE))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);

    if (first == TRUE) {
        resourceInfoReply.numResources = 0;
        resourceInfoReq.resourceNames = NULL;
        resourceInfoReq.numResourceNames = 0;
        resourceInfoReq.hostName = NULL;
        first = FALSE;
    }

    if (resourceInfoReply.numResources > 0)
        xdr_lsffree(xdr_resourceInfoReply, (char *)&resourceInfoReply, &replyHdr);
    FREEUP (resourceInfoReq.resourceNames);
    FREEUP (resourceInfoReq.hostName);

    if (numResources == NULL || *numResources < 0
        || (resources == NULL && *numResources > 0)) {
        lserrno = LSE_BAD_ARGS;
        return (NULL);
    }
    if (*numResources == 0 && resources == NULL) {
        if ((resourceInfoReq.resourceNames =
             (char **) malloc(sizeof (char *))) == NULL) {
            lserrno = LSE_MALLOC;
            return (NULL);
        }
        resourceInfoReq.resourceNames[0] = "";
        resourceInfoReq.numResourceNames = 1;
    } else {
        if ((resourceInfoReq.resourceNames =
             (char **) malloc (*numResources * sizeof(char *))) == NULL) { lserrno = LSE_MALLOC;
            return(NULL);
        }
        for (i = 0; i < *numResources; i++) {
            if (resources[i] && strlen (resources[i]) + 1 < MAXLSFNAMELEN)
                resourceInfoReq.resourceNames[i] = resources[i];
            else {
                FREEUP (resourceInfoReq.resourceNames);
                lserrno = LSE_BAD_RESOURCE;
                *numResources = i;
                return (NULL);
            }
            resourceInfoReq.numResourceNames = *numResources;
        }
    }
    if (hostName != NULL) {
        if (strlen(hostName) > MAXHOSTNAMELEN -1
            || Gethostbyname_(hostName) == NULL) {
            lserrno = LSE_BAD_HOST;
            return(NULL);
        }
        resourceInfoReq.hostName = putstr_(hostName);
    } else
        resourceInfoReq.hostName = putstr_(" ");


    if (resourceInfoReq.hostName == NULL) {
        lserrno = LSE_MALLOC;
        return (NULL);
    }
    cc = callLim_(LIM_GET_RESOUINFO, &resourceInfoReq, xdr_resourceInfoReq,
                  &resourceInfoReply, xdr_resourceInfoReply, NULL, _USE_TCP_, &replyHdr);
    if (cc < 0) {
        return NULL;
    }

    *numResources = resourceInfoReply.numResources;
    return (resourceInfoReply.resources);

}


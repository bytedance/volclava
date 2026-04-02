/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: mbd.resource.c 397 2007-11-26 19:04:00Z mblack $
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

#include <sys/types.h>
#include <math.h>
#include "mbd.h"
#include <malloc.h>

#include "../../lsf/lib/lsi18n.h"
#define NL_SETN		10

static void addSharedResource (struct lsSharedResourceInfo *);
static void addInstances (struct lsSharedResourceInfo *, struct sharedResource *);
static void freeHostInstances (void);
static void initHostInstances (int);
static int copyResource (struct lsbShareResourceInfoReply *, struct sharedResource *, char *);

struct objPRMO *pRMOPtr = NULL;

static void initQPRValues(struct qPRValues *, struct qData *);
static void freePreemptResourceInstance(struct preemptResourceInstance *);
static void freePreemptResource(struct preemptResource *);
static int addPreemptableResource(int);
static struct preemptResourceInstance * findPRInstance(int, struct hData *);
static struct qPRValues * findQPRValues(int, struct hData *, struct qData *);
static struct qPRValues * addQPRValues(int, struct hData *, struct qData *);
static float roundFloatValue (float);

static float
checkOrTakeAvailableByPreemptPRHQValue(int index, float value,
    struct hData *hPtr, struct qData *qPtr, int update);


void
getLsbResourceInfo (void)
{
    static char fname[] = "getLsbresourceInfo";
    int i, numRes = 0;
    struct lsSharedResourceInfo *resourceInfo;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "%s: Entering ..." , fname);

    if ((resourceInfo = ls_sharedresourceinfo (NULL, &numRes, NULL, 0))
        == NULL) {

        return;
    }
    if (numResources > 0)
        freeSharedResource();
    initHostInstances (numRes);
    for (i = 0; i < numRes; i++)
        addSharedResource(&resourceInfo[i]);

}


static void
addSharedResource (struct lsSharedResourceInfo *lsResourceInfo)
{
    static char fname[] = "addSharedResource";
    struct sharedResource *resource, **temp;

    if (lsResourceInfo == NULL)
        return;

    if (getResource (lsResourceInfo->resourceName) != NULL) {
        if (logclass & LC_TRACE)
            ls_syslog (LOG_DEBUG3, "%s: More than one resource <%s> is found in lsResourceInfo got from lim; ignoring later", fname, lsResourceInfo->resourceName);
        return;
    }
    resource = (struct sharedResource *)
               my_malloc(sizeof (struct sharedResource), fname);
    resource->numInstances = 0;
    resource->instances = NULL;
    resource->resourceName = safeSave (lsResourceInfo->resourceName);
    addInstances (lsResourceInfo, resource);

    if (numResources == 0)
        temp = (struct sharedResource **)
            my_malloc (sizeof (struct sharedResource *), fname);
    else
        temp = (struct sharedResource **) realloc (sharedResources,
                                                   (numResources + 1) *sizeof (struct sharedResource *));
    if (temp == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "realloc");
        mbdDie(MASTER_MEM);
    }
    sharedResources = (struct sharedResource **) temp;
    sharedResources[numResources] = resource;
    numResources++;

}

static void
addInstances (struct lsSharedResourceInfo *lsResourceInfo,
                                         struct sharedResource *resource)
{
    static char fname[] = "addInstances";
    int i, numHosts, j, numInsatnces = 0;
    struct resourceInstance *instance;
    struct hData *hPtr;

    if (lsResourceInfo->nInstances <= 0)
        return;

    resource->instances = (struct resourceInstance **) my_malloc
            (lsResourceInfo->nInstances * sizeof (struct resourceInstance *), fname);
    for (i = 0; i < lsResourceInfo->nInstances; i++) {
        instance = (struct resourceInstance *)my_malloc
                     (sizeof (struct resourceInstance), fname);
        instance->nHosts = 0;
	instance->lsfValue = NULL;
	instance->value = NULL;
        instance->resName = resource->resourceName;
	if (lsResourceInfo->instances[i].nHosts > 0) {
            instance->hosts = (struct hData **) my_malloc
              (lsResourceInfo->instances[i].nHosts * sizeof (struct hData *), fname);
        } else
            instance->hosts = NULL;

        numHosts = 0;
        for (j = 0; j < lsResourceInfo->instances[i].nHosts; j++) {
            if ((hPtr = getHostData
                      (lsResourceInfo->instances[i].hostList[j])) == NULL) {
                if (logclass & LC_TRACE)
                    ls_syslog (LOG_DEBUG3, "%s: Host <%s> is not used by the batch system;ignoring", fname, lsResourceInfo->instances[i].hostList[j]);
                continue;
            }

            if ((hPtr->hStatus & HOST_STAT_REMOTE)
                || (hPtr->flags & HOST_LOST_FOUND)) {
                continue;
            }

            hPtr->instances[hPtr->numInstances] = instance;
	    hPtr->numInstances++;
            instance->hosts[numHosts] = hPtr;
	    numHosts++;
        }
        instance->nHosts = numHosts;
	instance->lsfValue = safeSave(lsResourceInfo->instances[i].value);
	instance->value = safeSave (lsResourceInfo->instances[i].value);


        resource->instances[numInsatnces++] = instance;
    }
    resource->numInstances = numInsatnces;

}

void
freeSharedResource (void)
{
    int i, j;

    if (numResources <= 0)
        return;

    for (i = 0; i < numResources; i++) {
        for (j = 0; j < sharedResources[i]->numInstances; j++) {
            FREEUP (sharedResources[i]->instances[j]->hosts);
            FREEUP (sharedResources[i]->instances[j]->lsfValue);
            FREEUP (sharedResources[i]->instances[j]->value);
            FREEUP (sharedResources[i]->instances[j]);
        }
        FREEUP (sharedResources[i]->instances);
        FREEUP (sharedResources[i]->resourceName);
        FREEUP (sharedResources[i]);
    }
    FREEUP (sharedResources);
    numResources = 0;

    freeHostInstances();

}

static void
freeHostInstances (void)
{
    sTab hashSearchPtr;
    hEnt *hashEntryPtr;
    struct hData *hData;

    hashEntryPtr = h_firstEnt_(&hostTab, &hashSearchPtr);
    while (hashEntryPtr) {
        hData = (struct hData *) hashEntryPtr->hData;
        hashEntryPtr = h_nextEnt_(&hashSearchPtr);

        if (hData->flags & HOST_LOST_FOUND)
            continue;

        if (hData->hStatus & HOST_STAT_REMOTE)
            continue;

        FREEUP(hData->instances);
        hData->numInstances = 0;
    }

}

static void
initHostInstances(int num)
{
    sTab hashSearchPtr;
    hEnt *hashEntryPtr;
    struct hData *hData;

    hashEntryPtr = h_firstEnt_(&hostTab, &hashSearchPtr);
    while (hashEntryPtr) {
        hData = (struct hData *) hashEntryPtr->hData;
        hashEntryPtr = h_nextEnt_(&hashSearchPtr);

        if (hData->flags & HOST_LOST_FOUND)
            continue;

        if (hData->hStatus & HOST_STAT_REMOTE)
            continue;

        hData->instances = my_calloc(num,
                                     sizeof (struct resourceInstance *),
                                     __func__);
        hData->numInstances = 0;
    }

}

struct sharedResource *
getResource (char *resourceName)
{
    int i;
    if (resourceName == NULL || numResources <= 0)
	return NULL;

    for (i = 0; i < numResources; i++) {
	if (!strcmp (sharedResources[i]->resourceName, resourceName))
	    return (sharedResources[i]);
    }
    return NULL;

}

int
checkResources (struct resourceInfoReq *resourceInfoReq,
                struct lsbShareResourceInfoReply *reply)
{
    static char fname[] = "checkResources";
    int i, j, allResources = FALSE, found = FALSE, replyCode;
    struct hData *hPtr;
    struct hostent *hp;

    if (resourceInfoReq->numResourceNames == 1
        && !strcmp (resourceInfoReq->resourceNames[0], "")) {
        allResources = TRUE;
    }
    reply->numResources = 0;

    if (numResources == 0)
        return LSBE_NO_RESOURCE;

    if (resourceInfoReq->hostName == NULL
        || (resourceInfoReq->hostName
            && strcmp (resourceInfoReq->hostName, " ")== 0))
	hPtr = NULL;
    else {

        if ((hp = Gethostbyname_(resourceInfoReq->hostName)) == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_S_FAIL, fname, "getHostOfficialByName_",
		resourceInfoReq->hostName);
	    return LSBE_BAD_HOST;
        }

        if ((hPtr = getHostData (hp->h_name)) == NULL) {
            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 7703,
                                             "%s: Host <%s>  is not used by the batch system"), /* catgets 7703 */
                      fname, resourceInfoReq->hostName);
	    return LSBE_BAD_HOST;
        }
    }

    reply->numResources = 0;
    reply->resources = (struct lsbSharedResourceInfo *)
       my_malloc (numResources * sizeof (struct lsbSharedResourceInfo), fname);

    for (i = 0; i < resourceInfoReq->numResourceNames; i++) {
        found = FALSE;
        for (j = 0; j < numResources; j++) {
            if (allResources == FALSE
                 && (strcmp (resourceInfoReq->resourceNames[i],
                            sharedResources[j]->resourceName)))
                continue;
            found = TRUE;
            if ((replyCode = copyResource (reply, sharedResources[j],
		     (hPtr != NULL)?hPtr->host:NULL)) != LSBE_NO_ERROR) {
                return replyCode;
            }
            reply->numResources++;
            if (allResources == FALSE)
                break;
        }
        if (allResources == FALSE && found == FALSE) {

	    reply->badResource = i;
	    reply->numResources = 0;
	    FREEUP(reply->resources);
            return LSBE_BAD_RESOURCE;
        }
        found = FALSE;
        if (allResources == TRUE)
            break;
    }
    return LSBE_NO_ERROR;

}

static int
copyResource (struct lsbShareResourceInfoReply *reply, struct sharedResource *resource, char *hostName)
{
    static char fname[] = "copyResource";
    int i, j, num, found = FALSE, numInstances;
    float rsvValue;
    char stringValue[12];

    num = reply->numResources;
    reply->resources[num].resourceName = resource->resourceName;


    reply->resources[num].instances = (struct lsbSharedResourceInstance *) my_malloc (resource->numInstances * sizeof (struct lsbSharedResourceInstance), fname);
    reply->resources[num].nInstances = 0;
    numInstances = 0;

    for (i = 0; i < resource->numInstances; i++) {
	if (hostName) {
	    for (j = 0; j < resource->instances[i]->nHosts; j++) {
		if (strcmp (hostName, resource->instances[i]->hosts[j]->host))
		    continue;
                found = TRUE;
		break;
            }
        }
        if (hostName && found == FALSE)
	    continue;

        found = FALSE;
        reply->resources[num].instances[numInstances].totalValue =
                     safeSave (resource->instances[i]->value);

        if ((rsvValue = atoi(resource->instances[i]->value)
             - atoi(resource->instances[i]->lsfValue)) < 0)
            rsvValue = -rsvValue;

        sprintf (stringValue, "%-10.1f", rsvValue);
        reply->resources[num].instances[numInstances].rsvValue =
                     safeSave (stringValue);

        reply->resources[num].instances[numInstances].hostList =
          (char **) my_malloc (resource->instances[i]->nHosts * sizeof (char *),                               fname);
        for (j = 0; j < resource->instances[i]->nHosts; j++) {
            reply->resources[num].instances[numInstances].hostList[j] =
                       safeSave(resource->instances[i]->hosts[j]->host);
        }
        reply->resources[num].instances[numInstances].nHosts =
                                resource->instances[i]->nHosts;
	numInstances++;
    }
    reply->resources[num].nInstances = numInstances;
    return LSBE_NO_ERROR;

}

float
getHRValue(char *resName, struct hData *hPtr,
	   struct resourceInstance **instance)
{
    int i;
    static char fname[]="getHRValue()";

    for (i = 0; i < hPtr->numInstances; i++) {
        if (strcmp (hPtr->instances[i]->resName, resName) != 0)
            continue;
        *instance = hPtr->instances[i];
        if (strcmp(hPtr->instances[i]->value, "-") == 0) {
            return (INFINIT_LOAD);
        }
        return (atof (hPtr->instances[i]->value));
    }
    ls_syslog(LOG_ERR, I18N(7704,"%s, instance name not found."), fname);/*catgets 7704 */
    return (-INFINIT_LOAD);

}

void
resetSharedResource()
{
    int         i;
    int         j;

    for (i = 0; i < numResources; i++) {

	for (j = 0; j < sharedResources[i]->numInstances; j++) {

	    FREEUP(sharedResources[i]->instances[j]->value);

	    sharedResources[i]->instances[j]->value =
		safeSave(sharedResources[i]->instances[j]->lsfValue);
	}
    }
}
void
updSharedResourceByRUNJob(const struct jData* jp)
{
    static char fname[] = "updSharedResourceByRUNJob";
    int                        i;
    int                        ldx;
    int                        resAssign = 0;
    float                      jackValue;
    float                      originalLoad;
    float                      duration;
    float                      decay;
    struct resVal              *resValPtr;
    struct resourceInstance    *instance;
    static int                 *rusgBitMaps =  NULL;
    int adjForPreemptableResource = FALSE;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "\
%s: Updating shared resource by job=%s", fname, lsb_jobid2str(jp->jobId));

    if (rusgBitMaps == NULL) {
        rusgBitMaps = (int *) my_malloc
	    (GET_INTNUM(allLsInfo->nRes) * sizeof (int), fname);
    }

    if (!jp->numHostPtr || jp->hPtr == NULL) {
        return;
    }

    if (jp->effeResReqEnt == NULL) {
        return; 
    }
    resValPtr = GET_JOB_EFFE_RES_REQ(jp);

    for (i = 0; i < GET_INTNUM(allLsInfo->nRes); i++) {
        resAssign += resValPtr->rusgBitMaps[i];
        rusgBitMaps[i] = 0;
    }

    if (resAssign == 0)
        return;

    duration = (float) resValPtr->duration;
    decay = resValPtr->decay;
    if (resValPtr->duration != INFINIT_INT && (duration - jp->runTime <= 0)){

	if ((duration - runTimeSinceResume(jp) <= 0)
	    || !isReservePreemptResource(resValPtr)) {
            return;
        }
        adjForPreemptableResource = TRUE;
    }

    for (i = 0; i < jp->numHostPtr; i++) {
	float load;
	char  loadString[MAXLSFNAMELEN];

	if (jp->hPtr[i]->hStatus & HOST_STAT_UNAVAIL)
	    continue;

        for (ldx = 0; ldx < allLsInfo->nRes; ldx++) {
            float factor;
	    int isSet;


            if (ldx < allLsInfo->numIndx)
		continue;

	    if (NOT_NUMERIC(allLsInfo->resTable[ldx]))
		continue;

            TEST_BIT(ldx, resValPtr->rusgBitMaps, isSet);
	    if (isSet == 0)
	        continue;


	    if (adjForPreemptableResource && (!isItPreemptResourceIndex(ldx))) {
		continue;
            }


	    if (jp->jStatus & JOB_STAT_RUN) {

		goto adjustLoadValue;


	    } else if (IS_SUSP(jp->jStatus)
		       &&
		       ! (allLsInfo->resTable[ldx].flags & RESF_RELEASE)) {

		goto adjustLoadValue;


	    } else if (IS_SUSP(jp->jStatus)
		       &&
		       (jp->jStatus & JOB_STAT_RESERVE)) {

		goto adjustLoadValue;

	    } else {



		continue;

	    }

adjustLoadValue:

	    jackValue = resValPtr->val[ldx];
	    if (jackValue >= INFINIT_LOAD || jackValue <= -INFINIT_LOAD)
		continue;

	    if ( (load = getHRValue(allLsInfo->resTable[ldx].name,
				    jp->hPtr[i],
				    &instance) ) < 0.0) {
		continue;
	    } else {

		TEST_BIT (ldx, rusgBitMaps, isSet)
                if (isSet == TRUE && !slotResourceReserve) {
                    continue;
                }
	    }

	    if (logclass & LC_SCHED)
		ls_syslog(LOG_DEBUG,"\
%s: jobId=<%s>, hostName=<%s>, resource name=<%s>, the specified rusage of the load or instance <%f>, current lsbload or instance value <%f>, duration <%f>, decay <%f>",
			  fname,
			  lsb_jobid2str(jp->jobId),
			  jp->hPtr[i]->host,
			  allLsInfo->resTable[ldx].name,
			  jackValue,
			  load,
			  duration,
			  decay);


            factor = 1.0;
	    if (resValPtr->duration != INFINIT_INT) {
	        if (resValPtr->decay != INFINIT_FLOAT) {
                    float du;

		    if ( isItPreemptResourceIndex(ldx) ) {
                        du = duration - runTimeSinceResume(jp);
                    } else {
                        du = duration - jp->runTime;
                    }
                    if (du > 0) {
                        factor = du/duration;
                        factor = pow (factor, decay);
                    }
                }
                jackValue *= factor;
            }

	    if (allLsInfo->resTable[ldx].orderType == DECR)
	        jackValue = -jackValue;

	    originalLoad = atof (instance->value);
	    load = originalLoad + jackValue;

	    if (load < 0.0)
		load = 0.0;

	    sprintf (loadString, "%-10.1f", load);

	    FREEUP (instance->value);
	    instance->value = safeSave (loadString);

	    SET_BIT (ldx, rusgBitMaps);

	    if (logclass & LC_SCHED)
		ls_syslog(LOG_DEBUG,"\
%s: JobId=<%s>, hostname=<%s>, resource name=<%s>, the amount by which the load or the instance has been adjusted <%f>, original load or instance value <%f>, runTime=<%d>, value of the load or the instance after the adjustment <%f>, factor <%f>",
			  fname,
			  lsb_jobid2str(jp->jobId),
			  jp->hPtr[i]->host,
			  allLsInfo->resTable[ldx].name,
			  jackValue,
			  originalLoad,
			  jp->runTime,
			  load,
			  factor);
        }
    }
}

static void
initQPRValues(struct qPRValues *qPRValuesPtr, struct qData *qPtr)
{
    qPRValuesPtr->qData                 = qPtr;
    qPRValuesPtr->usedByRunJob          = 0.0;
    qPRValuesPtr->reservedByPreemptWait = 0.0;
    qPRValuesPtr->availableByPreempt    = 0.0;
    qPRValuesPtr->preemptingRunJob      = 0.0;

    return;

}


static void
freePreemptResourceInstance(struct preemptResourceInstance *pRIPtr)
{

    pRIPtr->instancePtr = NULL;
    pRIPtr->nQPRValues = 0;
    FREEUP(pRIPtr->qPRValues);

    return;

}

static void
freePreemptResource(struct preemptResource *pRPtr)
{
    int i;

    pRPtr->index = -1;
    for (i=0; i<pRPtr->numInstances; i++) {
	freePreemptResourceInstance(&pRPtr->pRInstance[i]);
    }
    pRPtr->numInstances = 0;
    FREEUP(pRPtr->pRInstance);

    return;

}

void
newPRMO(char *nameList)
{
    static char fname[] = "newPRMO";
    NAMELIST *nameListPtr;
    int i, index, addedNum = 0;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "%s: Entering ..." , fname);

    if (pRMOPtr != NULL) {
	delPRMO();
    }

    if ((nameList == NULL) || (nameList[0] == '\0')) {
	return;
    }


    if ((nameListPtr = lsb_parseLongStr(nameList)) == NULL) {
	return;
    }

    nameList[0]='\0';


    if (nameListPtr->listSize <= 0) {
	return;
    }
    pRMOPtr = (struct objPRMO *) my_malloc(sizeof (struct objPRMO), fname);
    pRMOPtr->numPreemptResources = 0;
    pRMOPtr->pResources = (struct preemptResource *)
	my_malloc(nameListPtr->listSize * sizeof (struct preemptResource), fname);


    for (i=0; i<nameListPtr->listSize; i++) {
	if (nameListPtr->counter[i] > 1) {
            ls_syslog(LOG_WARNING, I18N(7707,
    	        "Duplicate preemptable resource names (%s) defined, take one only."),/* catgets 7707*/
		nameListPtr->names[i]);
            lsberrno = LSBE_CONF_WARNING;

	}
	if ((index = resName2resIndex(nameListPtr->names[i])) < 0) {
            ls_syslog(LOG_ERR, I18N(7708,
    	        "Unknown preemptable resource name (%s) defined, ignored."),/* catgets 7708*/
		nameListPtr->names[i]);
            lsberrno = LSBE_CONF_WARNING;
	    continue;
	}


        if (index < allLsInfo->numIndx) {
            if (allLsInfo->resTable[index].flags & RESF_BUILTIN) {
                ls_syslog(LOG_ERR, I18N(7709,
    	            "Built-in load index (%s) can't be defined as a preemptable resource, ignored."),/* catgets 7709*/
		    nameListPtr->names[i]);
	    } else {
                ls_syslog(LOG_ERR, I18N(7714,
    	            "Host load index (%s) can't be defined as a preemptable resource, ignored."),/* catgets 7714*/
		    nameListPtr->names[i]);
	    }
            lsberrno = LSBE_CONF_WARNING;
	    continue;
	}


	if (NOT_NUMERIC(allLsInfo->resTable[index])) {
            ls_syslog(LOG_ERR, I18N(7710,
    	        "Non-numeric resource (%s) can't be defined as a preemptable resource, ignored."),/* catgets 7710*/
		nameListPtr->names[i]);
            lsberrno = LSBE_CONF_WARNING;
	    continue;
	}


        if (!(allLsInfo->resTable[index].flags & RESF_RELEASE)) {
            ls_syslog(LOG_ERR, I18N(7711,
    	        "Non-releasable resource (%s) can't be defined as a preemptable resource, ignored."),/* catgets 7711*/
		nameListPtr->names[i]);
            lsberrno = LSBE_CONF_WARNING;
	    continue;
	}


	if (allLsInfo->resTable[index].orderType != DECR) {
            ls_syslog(LOG_ERR, I18N(7712,
    	        "Increasing resource (%s) can't be defined as a preemptable resource, ignored."),/* catgets 7712*/
		nameListPtr->names[i]);
            lsberrno = LSBE_CONF_WARNING;
	    continue;
	}


	if (addPreemptableResource(index) == 0) {

	    if (addedNum > 0) {
		strcat(nameList, " ");
	    }
	    strcat(nameList, nameListPtr->names[i]);
	    addedNum++;
	}
    }

    return;

}

void delPRMO()
{
    int i;

    if (pRMOPtr == NULL) {
	return;
    }

    for (i=0; i<pRMOPtr->numPreemptResources; i++) {
	freePreemptResource(&pRMOPtr->pResources[i]);
    }
    FREEUP(pRMOPtr->pResources);
    FREEUP(pRMOPtr);

    return;

}

void mbdReconfPRMO()
{
    struct jData *jp;
    int i;


    delPRMO();


    for (jp = jDataList[SJL]->forw; jp != jDataList[SJL]; jp = jp->forw) {
        jp->jFlags &= ~JFLAG_WILL_BE_PREEMPTED;
    }


    for (i = SJL; i <= PJL; i++) {
        for (jp = jDataList[i]->back; jp != jDataList[i]; jp = jp->back) {
	    if (JOB_PREEMPT_WAIT(jp)) {

	        deallocReservePreemptResources(jp);
	    }
	}
    }
}

void resetPRMOValues(int valueSelectFlag)
{
    int i, j, k;

    if (pRMOPtr == NULL) {
        return;
    }

    for (i = 0; i < pRMOPtr->numPreemptResources; i++) {
	struct preemptResource *pRPtr;

	pRPtr = &(pRMOPtr->pResources[i]);

        for (j = 0; j < pRPtr->numInstances; j++) {
	    struct preemptResourceInstance *pRIPtr;

	    pRIPtr = &(pRPtr->pRInstance[j]);

            for (k = 0; k < pRIPtr->nQPRValues; k++) {
		if ((valueSelectFlag == PRMO_ALLVALUES) ||
		    (valueSelectFlag & PRMO_USEDBYRUNJOB)) {
	            pRIPtr->qPRValues[k].usedByRunJob = 0.0;
		}
		if ((valueSelectFlag == PRMO_ALLVALUES) ||
		    (valueSelectFlag & PRMO_RESERVEDBYPREEMPTWAIT)) {
	            pRIPtr->qPRValues[k].reservedByPreemptWait = 0.0;
		}
		if ((valueSelectFlag == PRMO_ALLVALUES) ||
		    (valueSelectFlag & PRMO_AVAILABLEBYPREEMPT)) {
	            pRIPtr->qPRValues[k].availableByPreempt = 0.0;
		}
		if ((valueSelectFlag == PRMO_ALLVALUES) ||
		    (valueSelectFlag & PRMO_PREEMPTINGRUNJOB)) {
	            pRIPtr->qPRValues[k].preemptingRunJob = 0.0;
		}
            }
        }
    }

    return;

}

float
getUsablePRHQValue(int index, struct hData *hPtr, struct qData *qPtr,
struct resourceInstance **instance)
{
    float currentValue;
    float reservedValue=0.0;
    struct preemptResourceInstance *pRInstancePtr;

    currentValue = getHRValue(allLsInfo->resTable[index].name, hPtr, instance);


    if (currentValue < 0.0 || currentValue == INFINIT_LOAD) {
	return(currentValue);
    }

    if ((pRInstancePtr = findPRInstance(index, hPtr)) == NULL) {
	return(currentValue);
    }

    return(roundFloatValue(currentValue - reservedValue));

}

float
takeAvailableByPreemptPRHQValue(int index, float value, struct hData *hPtr,
				struct qData *qPtr)
{
    return checkOrTakeAvailableByPreemptPRHQValue(index, value, hPtr, qPtr, 1);
}

float
checkOrTakeAvailableByPreemptPRHQValue(int index, float value,
    struct hData *hPtr, struct qData *qPtr, int update)
{
    struct preemptResourceInstance *pRInstancePtr;
    float remainValue = value;

    if ((pRInstancePtr = findPRInstance(index, hPtr)) == NULL) {
        return(remainValue);
    }

    return(remainValue);

}

void
addRunJobUsedPRHQValue(int index, float value, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = addQPRValues(index, hPtr, qPtr)) == NULL) {
        return;
    }
    qPRVPtr->usedByRunJob = roundFloatValue(qPRVPtr->usedByRunJob+value);

    return;

}

void
removeRunJobUsedPRHQValue(int index, float value, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = findQPRValues(index, hPtr, qPtr)) == NULL) {
        return;
    }
    qPRVPtr->usedByRunJob = roundFloatValue(qPRVPtr->usedByRunJob-value);
    if (qPRVPtr->usedByRunJob < 0.1) {
	qPRVPtr->usedByRunJob = 0.0;
    }

    return;

}

void
addReservedByWaitPRHQValue(int index, float value, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = addQPRValues(index, hPtr, qPtr)) == NULL) {
        return;
    }
    qPRVPtr->reservedByPreemptWait =
	roundFloatValue(qPRVPtr->reservedByPreemptWait+value);

    return;
}

void
removeReservedByWaitPRHQValue(int index, float value, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = findQPRValues(index, hPtr, qPtr)) == NULL) {
        return;
    }
    qPRVPtr->reservedByPreemptWait =
	roundFloatValue(qPRVPtr->reservedByPreemptWait-value);
    if (qPRVPtr->reservedByPreemptWait < 0.1) {
	qPRVPtr->reservedByPreemptWait = 0.0;
    }

    return;

}

float
getReservedByWaitPRHQValue(int index, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = findQPRValues(index, hPtr, qPtr)) == NULL) {
        return(0.0);
    }
    return(qPRVPtr->reservedByPreemptWait);

}

void
addAvailableByPreemptPRHQValue(int index, float value, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = addQPRValues(index, hPtr, qPtr)) == NULL) {
        return;
    }
    qPRVPtr->availableByPreempt =
	roundFloatValue(qPRVPtr->availableByPreempt+value);

    return;

}

void
removeAvailableByPreemptPRHQValue(int index, float value, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = addQPRValues(index, hPtr, qPtr)) == NULL) {
        return;
    }
    qPRVPtr->availableByPreempt =
	roundFloatValue(qPRVPtr->availableByPreempt-value);

    return;
}

float
getAvailableByPreemptPRHQValue(int index, struct hData *hPtr,
struct qData *qPtr)
{
    struct qPRValues *qPRVPtr;

    if ((qPRVPtr = findQPRValues(index, hPtr, qPtr)) == NULL) {
        return(0.0);
    }
    return(qPRVPtr->availableByPreempt);

}

int
resName2resIndex(char *resName)
{
    int i;

    for (i = 0; i < allLsInfo->nRes; i++) {
	if (!strcmp(allLsInfo->resTable[i].name, resName)) {
            return(i);
	}
    }

    return(-1);

}

int
isItPreemptResourceName(char *resName)
{

    return(isItPreemptResourceIndex(resName2resIndex(resName)));

}

int
isItPreemptResourceIndex(int index)
{
    int i;

    if (pRMOPtr == NULL) {
        return(0);
    }

    for (i = 0; i < pRMOPtr->numPreemptResources; i++) {
	if (pRMOPtr->pResources[i].index == index) {

	    return(1);
	}
    }

    return(0);
}

int
isReservePreemptResource(struct  resVal *resValPtr)
{
    int resn;

    if (!resValPtr) {
        return(0);
    }

    FORALL_PRMPT_RSRCS(resn) {
	int valSet = FALSE;
	TEST_BIT(resn, resValPtr->rusgBitMaps, valSet);
	if ((valSet != 0) && (resValPtr->val[resn] > 0.0)) {
            return(1);
	}
    } ENDFORALL_PRMPT_RSRCS;

    return(0);

}

static int
addPreemptableResource(int index)
{
    static char fname[] = "addPreemptableResource";
    int i;
    int loc;
    int sharedIndex = -1;

    if (pRMOPtr == NULL) {
	return -1;
    }


    for (i=0; i<pRMOPtr->numPreemptResources; i++) {
        if (pRMOPtr->pResources[i].index == index) {
            ls_syslog(LOG_INFO, I18N(7707,
    	        "Duplicated preemptable resource name (%s) defined, ignored."),/* catgets 7707*/
	        allLsInfo->resTable[index].name);
            lsberrno = LSBE_CONF_WARNING;
	    return -1;
	}
    }


    for (i=0; i<numResources; i++) {
        if (!strcmp(sharedResources[i]->resourceName,
	    allLsInfo->resTable[index].name)) {
	    sharedIndex = i;
	    break;
	}

    }

    if (sharedIndex < 0) {
        ls_syslog(LOG_ERR, I18N(7713,
            "Preemptable resource name (%s) is not a shared resource, ignored."),/* catgets 7713*/
	    allLsInfo->resTable[index].name);
        lsberrno = LSBE_CONF_WARNING;
	return -1;
    }

    loc = pRMOPtr->numPreemptResources;
    pRMOPtr->pResources[loc].index = index;
    pRMOPtr->pResources[loc].numInstances =
	sharedResources[sharedIndex]->numInstances;
    pRMOPtr->pResources[loc].pRInstance =
	(struct preemptResourceInstance *)
	my_malloc ( sharedResources[sharedIndex]->numInstances *
	    sizeof(struct preemptResourceInstance), fname);

    for (i=0; i<pRMOPtr->pResources[loc].numInstances; i++) {
	pRMOPtr->pResources[loc].pRInstance[i].instancePtr =
	    sharedResources[sharedIndex]->instances[i];
	pRMOPtr->pResources[loc].pRInstance[i].nQPRValues  = 0;
	pRMOPtr->pResources[loc].pRInstance[i].qPRValues   = NULL;
    }
    pRMOPtr->numPreemptResources++;

    return 0;

}

static struct preemptResourceInstance *
findPRInstance(int index, struct hData *hPtr)
{
    int i, j;
    struct preemptResource *pRPtr=NULL;

    if (pRMOPtr == NULL || pRMOPtr->numPreemptResources <= 0) {
        return NULL;
    }

    for (i = 0; i < pRMOPtr->numPreemptResources; i++) {

	if (pRMOPtr->pResources[i].index == index) {
	    pRPtr = &(pRMOPtr->pResources[i]);
	    break;
	}
    }

    if (pRPtr == NULL) {
	return(NULL);
    }

    for (i = 0; i < pRPtr->numInstances; i++) {
        for (j = 0; j < pRPtr->pRInstance[i].instancePtr->nHosts; j++) {
	    if (pRPtr->pRInstance[i].instancePtr->hosts[j] == hPtr) {
                return(&(pRPtr->pRInstance[i]));
	    }
	}
    }

    return(NULL);

}

static struct qPRValues *
findQPRValues(int index, struct hData *hPtr, struct qData *qPtr)
{
    struct preemptResourceInstance *pRIPtr;
    int i;

    if ((pRIPtr = findPRInstance(index, hPtr)) == NULL) {
	return(NULL);
    }

    for (i = 0; i < pRIPtr->nQPRValues; i++) {
	if (pRIPtr->qPRValues[i].qData->priority > qPtr->priority) {
	  break;
	}
	if (pRIPtr->qPRValues[i].qData == qPtr) {
	    return(&(pRIPtr->qPRValues[i]));
	}
    }
    return(NULL);

}

static struct qPRValues *
addQPRValues(int index, struct hData *hPtr, struct qData *qPtr)
{
    static char fname[] = "addQPRValues";
    struct preemptResourceInstance *pRIPtr;
    struct qPRValues *temp;
    int i, pos;

    if ((pRIPtr = findPRInstance(index, hPtr)) == NULL) {
	return(NULL);
    }


    pos = pRIPtr->nQPRValues;
    for (i = 0; i < pRIPtr->nQPRValues; i++) {
	if (pRIPtr->qPRValues[i].qData->priority > qPtr->priority) {
	  pos = i;
	  break;
	}
	if (pRIPtr->qPRValues[i].qData == qPtr) {
	    return(&(pRIPtr->qPRValues[i]));
	}
    }


    if (pRIPtr->nQPRValues == 0)
        temp = (struct qPRValues *)
                     my_malloc (sizeof (struct qPRValues), fname);
    else
        temp = (struct qPRValues *) realloc (pRIPtr->qPRValues,
                     (pRIPtr->nQPRValues + 1) *sizeof (struct qPRValues));
    if (temp == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "realloc");
        mbdDie(MASTER_MEM);
    }
    pRIPtr->qPRValues = (struct qPRValues *) temp;
    pRIPtr->nQPRValues++;


    for (i=pRIPtr->nQPRValues-1; i > pos; i--) {
      pRIPtr->qPRValues[i] = pRIPtr->qPRValues[i-1];
    }
    initQPRValues(&(pRIPtr->qPRValues[pos]), qPtr);

    return(&(pRIPtr->qPRValues[pos]));

}

static float
roundFloatValue (float old)
{
    static char stringValue[12];

    sprintf (stringValue, "%-10.1f", old);

    return (atof (stringValue));

}

void printPRMOValues()
{
    static char fname[] = "printPRMOValues";
    int i, j, k;

    if (!(logclass & LC_SCHED)) {
	return;
    }

    if (pRMOPtr == NULL) {
        return;
    }

    for (i = 0; i < pRMOPtr->numPreemptResources; i++) {
        struct preemptResource *pRPtr;

        pRPtr = &(pRMOPtr->pResources[i]);
        ls_syslog(LOG_DEBUG2,
            "%s: preemptable resource(%s, %d) has %d instances:",
	    fname,
            allLsInfo->resTable[pRPtr->index].name,
            pRPtr->index,
            pRPtr->numInstances);

        for (j = 0; j < pRPtr->numInstances; j++) {
            struct preemptResourceInstance *pRIPtr;

            pRIPtr = &(pRPtr->pRInstance[j]);
            ls_syslog(LOG_DEBUG2,
                "%s: instance(%d) has %d queues:",
                fname,
		j,
                pRIPtr->nQPRValues);

            for (k = 0; k < pRIPtr->nQPRValues; k++) {
                ls_syslog(LOG_DEBUG2,
                    "%s: queue(%s) has UBRJ-%f, RBPW-%f, ABP-%f, PRJ-%f.",
		    fname,
                    pRIPtr->qPRValues[k].qData->queue,
                    pRIPtr->qPRValues[k].usedByRunJob,
                    pRIPtr->qPRValues[k].reservedByPreemptWait,
                    pRIPtr->qPRValues[k].availableByPreempt,
                    pRIPtr->qPRValues[k].preemptingRunJob);
            }
        }
    }

    return;

}

int
isHostsInSameInstance(int index, struct hData *hPtr1, struct hData *hPtr2)
{
    int         i;
    int         j;

    if (hPtr1 == hPtr2) {
	return(TRUE);
    }

    for (i = 0; i < numResources; i++) {
        if (strcmp(allLsInfo->resTable[index].name,
	    sharedResources[i]->resourceName)) {
	    continue;
	}
	for (j = 0; j < sharedResources[i]->numInstances; j++) {
	    int found = 0;
	    int k;

	    for (k = 0; k < sharedResources[i]->instances[j]->nHosts; k++) {
		if (sharedResources[i]->instances[j]->hosts[k] == hPtr1) {
		    found++;
		}
		if (sharedResources[i]->instances[j]->hosts[k] == hPtr2) {
		    found++;
		}
	    }
            if (found >= 2) {
		return(TRUE);
	    }
            if (found == 1) {
		return(FALSE);
	    }
	}
	return(FALSE);
    }
    return(FALSE);
}

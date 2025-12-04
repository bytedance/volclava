/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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

#include "mbd.fairshare.h"

hTab policies;

static void buildFSTree(struct shareAcct *, struct gData *, struct userShares *, int);
static void buildIndividualOfGroup(struct shareAcct *, struct userShares *, int);
static void addDescendantInTab(struct gData *, hTab *, hTab *);
static void addDescendantInSAcctList(struct gData *, struct shareAcct *, int);
static void addUsersForOthers(struct gData *, hTab *, hTab *);
static void addOutsideUsrsForOthers(struct userShares *uShares, int nShares, hTab *tab);
static void inShareAcctList(LIST_T *,  struct shareAcct *);
static void freeShareDistributePolicy(struct shareDistributePolicy *);
static void freeShareAcct(LIST_ENTRY_T *);
static void dumpSAcctInfo(struct shareAcct *, void *);
static void traverseFSTree(struct shareAcct *, void *, void(*func)(struct shareAcct *, void *));
static void resetSkipFlag(struct shareAcct *, void *);
static void updSAcctSkippedFlag(struct shareAcct *);
static struct jobFSRef  *electAJob(struct fsQueueJobSet *, struct shareAcct *, struct shareAcct  **,hEnt **);
static int addToFSJobSet(struct fsQueueJobSet * , void *);
static void * getJobFromFSJobSet(struct fsQueueJobSet *, void *, hEnt **);
static int rmJobFromFSJobSet(struct fsQueueJobSet *, struct shareAcct *, void *, void *);
static void appendAliveUser(struct fairsharePolicy *, struct shareAcctRef *, char *);
static float calcDynamicPriority(struct shareAcct *);
static void adjustOrderByPriority(struct shareAcct *); /*update shareAcct order*/
static void traverseUpd4Decay(struct shareAcct * sAcct, int decayTimes);

extern float timeFSFetchUser;
extern float timeFSFetchJob;
extern int timinglevel;

/******************************************************************************************
 * Description:
 * Create fairshare structure for queue with FAIRSHARE configuration
 *
 * Params:
 * q[in]: the target queue
 *
 ******************************************************************************************
 */

void addFSPolicy(struct qData *q) {
    static char fname[] = "addFSPolicy()";
    struct fairsharePolicy *policy = NULL;
    struct userShares *uShares = NULL;
    int count = 0, i = 0, new = 0;
    char *p = NULL, *tmpStr = NULL, *end = NULL, *delimiter = NULL;
    hEnt * policyEnt = NULL;

    if (!q->userShares || strlen(q->userShares) == 0) {
        return;
    }

    /*1. get pair values of user and share*/
    count = 0;
    p = q->userShares;
    while ((p != '\0') && ((p = strchr(p, '[')) != NULL)) {
        count++;
        p++;
    }
    if (count == 0) {
        return;
    }
    uShares = (struct userShares *)my_calloc(count, sizeof(struct userShares), fname);
    tmpStr = safeSave(q->userShares);
    i = 0;
    p = tmpStr;
    /*we don't need do syntax check for q->userShares, because check-configuration has done it.*/
    while ((p != '\0') && ((p = strchr(p, '[')) != NULL)) {
        end = strchr(p, ']');
        end[0] = '\0';
        delimiter = strchr(p, ',');
        delimiter[0] = '\0';
        uShares[i].name = safeSave(p+1);
        uShares[i].share = my_atoi(delimiter+1, INFINIT_INT, 0);
        i++;
        p = end + 1;
    }
    FREEUP(tmpStr);

    /*2. ready to create fs tree*/
    policy = (struct fairsharePolicy *)my_calloc(1, sizeof(struct fairsharePolicy), fname);
    policy->name = safeSave(q->queue);
    policy->qPtr = q;
    policy->root = (struct shareAcct *)my_calloc(1, sizeof(struct shareAcct), fname);
    policy->root->name = safeSave(q->queue);
    policy->root->path = my_calloc(strlen(policy->root->name) + 2, sizeof(char), fname);
    sprintf(policy->root->path, "/%s", policy->root->name);
    policy->root->policy = policy;
    h_initTab_(&(policy->userTab), 23);
    buildFSTree(policy->root, NULL, uShares, count);
    buildIndividualOfGroup(policy->root, uShares, count);

    for(i = 0; i < count; i++) {
        FREEUP(uShares[i].name);
    }
    FREEUP(uShares);

    /*3. add queue's fairshare policy into global variable policies*/
    policyEnt = h_addEnt_(&policies, policy->name, &new);
    if (!new) {
        ls_syslog(LOG_WARNING, "%s: Fairshare policy of queue <%s> is duplicated. Obsolete the old one.", fname, policy->name);
        freeFSPolicy((struct fairsharePolicy *)policyEnt->hData);
        policyEnt->hData = (int *)policy;
    }
    policyEnt->hData = (int *)policy;
    q->policy = policy;

    if (logclass & LC_FAIR) {
        dumpFSPolicy(policy, fname, LOG_DEBUG);
    }

    return;
}/*addFSPolicy*/

/******************************************************************************************
 * Description:
 * the worker function for addFSPolicy(), to create fairshare tree recursivly
 *
 * Params:
 * root   [in]: the shareAcct node which we will create its children now.
 * group  [in]: the child is a group
 * uShares[in]: the array of configured usershare
 * nShares[in]: the number of usershare
 *
 ******************************************************************************************
 */
static void buildFSTree(struct shareAcct *root, struct gData *group, struct userShares *uShares, int nShares) {
    static char fname[] = "buildFSTree";
    int i = 0;
    struct shareAcct * sAcctOthers = NULL;
    hTab *cacheTab = NULL;

    if(uShares && nShares > 0) {
        /*Create structure to record children*/
        root->sharePolicy = (struct shareDistributePolicy *)my_calloc(1, sizeof(struct shareDistributePolicy), fname);
        root->sharePolicy->holder = root;
        root->sharePolicy->sAcctList = listCreate("user shareAcct list");
    } else if (group){
        /*leaf node: user group 
         *The usergroup doesn't configure user share policy, we just add all descendant into usersTab.
         *If usergroup's member is all, then keep usersTab as empty to represent all users.
         */
        hEnt *ent = NULL;
        int new;
        LIST_T * uList;
        struct shareAcctRef *ref = NULL;

        root->sharePolicy = (struct shareDistributePolicy *)my_calloc(1, sizeof(struct shareDistributePolicy), fname);
        root->sharePolicy->holder = root;
        root->sharePolicy->usersTab = (hTab *)my_calloc(1, sizeof(hTab), fname);
        root->flags |= FS_LEAF_GROUP;
        h_initTab_(root->sharePolicy->usersTab, 23);
        addDescendantInTab(group, root->sharePolicy->usersTab, NULL);

        /*cache leaf node in userTab*/
        ent = h_addEnt_(&(root->policy->userTab), root->name, &new);
        if (new) {
            uList = (LIST_T *)listCreate("list of leaf shareAcct with the same username");
            ent->hData = (int *)uList;
        } else {
            uList = (LIST_T *)ent->hData;
        }
        ref = (struct shareAcctRef *)my_calloc(1, sizeof(struct shareAcctRef), fname);
        ref->sacct = root;
        listInsertEntryAtBack(uList, (LIST_ENTRY_T *)ref); 
        return;
    } else {
        /*This is leaf user, just cache in userTab*/
        hEnt *ent = NULL;
        int new;
        LIST_T * uList;
        struct shareAcctRef *ref = NULL;

        ent = h_addEnt_(&(root->policy->userTab), root->name, &new);
        if (new) {
            uList = (LIST_T *)listCreate("list of leaf shareAcct with the same username");
            ent->hData = (int *)uList;
        } else {
            uList = (LIST_T *)ent->hData;
        }
        ref = (struct shareAcctRef *)my_calloc(1, sizeof(struct shareAcctRef), fname);
        ref->sacct = root;
        listInsertEntryAtBack(uList, (LIST_ENTRY_T *)ref);
        return;
    }
    
    /*Create children structure*/
    cacheTab = (hTab *)my_calloc(1, sizeof(hTab), fname);
    h_initTab_(cacheTab, 23);
    for (i = 0; i < nShares; i++) {
        struct shareAcct * sAcct;
        
        if (uShares[i].name[strlen(uShares[i].name)-1] == '@') {
            continue;
        }

        sAcct = (struct shareAcct *)my_calloc(1, sizeof(struct shareAcct), fname);
        sAcct->name = safeSave(uShares[i].name);
        sAcct->path = my_calloc(strlen(root->path)+strlen(sAcct->name)+2 ,sizeof(char), fname);
        sprintf(sAcct->path, "%s/%s", root->path, sAcct->name);
        sAcct->share = uShares[i].share;
        sAcct->parent = root;
        sAcct->policy = root->policy;
        sAcct->priority = calcDynamicPriority(sAcct);
        inShareAcctList(root->sharePolicy->sAcctList, sAcct);

        h_addEnt_(cacheTab, sAcct->name, NULL);

        /*handle children*/
        if (strcasecmp(uShares[i].name, "default") == 0) {
            sAcct->flags |= FS_IS_DEFAULT;
            if (group == NULL || (group->memberTab.numEnts == 0 && group->numGroups == 0)) {/*means usergroup has 'all'*/
                sAcct->flags |= FS_ALL_DEFAULT;
            }
            buildFSTree(sAcct, NULL, NULL, 0);
        } else if (strcasecmp(uShares[i].name, "others") == 0) {
            sAcct->flags |= FS_LEAF_GROUP;
            sAcct->sharePolicy = (struct shareDistributePolicy *)my_calloc(1, sizeof(struct shareDistributePolicy), fname);
            sAcct->sharePolicy->holder = sAcct;

            if (group == NULL || (group->memberTab.numEnts == 0 && group->numGroups == 0)) {/*means usergroup has 'all'*/
                sAcct->sharePolicy->flags |= FS_OUT_OTHERS;
            } else {
                sAcct->sharePolicy->flags |= FS_IN_OTHERS;
            }
            sAcctOthers = sAcct;
            buildFSTree(sAcct, NULL, NULL, 0);
        } else {
            struct uData *u = getUserData(sAcct->name);
            if (u) {
                if (u->flags & USER_GROUP) {
                    if (u->gData->ugrpAttrs) {
                        /*shareAcct is a usergroup, and this ugroup configures USER_SAHRE*/
                        buildFSTree(sAcct, u->gData, u->gData->ugrpAttrs->userShares, u->gData->ugrpAttrs->numUserShares);
                    } else {
                        /*shareAcct is a usergroup, but this ugroup doesn't configure USER_SAHRE*/
                        buildFSTree(sAcct, u->gData, NULL, 0);
                    }
                } else {
                    buildFSTree(sAcct, NULL, NULL, 0);
                }
            }
        }
    }

    /*For 'others' shareAcct, we need add outside/inside users into its usersTab*/
    if (sAcctOthers) {
        sAcctOthers->sharePolicy->usersTab = (hTab *)my_calloc(1, sizeof(hTab), fname);
        h_initTab_(sAcctOthers->sharePolicy->usersTab, 23);
        if (sAcctOthers->sharePolicy->flags & FS_IN_OTHERS) {
            addUsersForOthers(group, sAcctOthers->sharePolicy->usersTab, cacheTab); 
        } else {
            addOutsideUsrsForOthers(uShares, nShares, sAcctOthers->sharePolicy->usersTab);
        }    
    }

    h_delTab_(cacheTab);
    FREEUP(cacheTab);

    return;
}/*buildFSTree*/


/******************************************************************************************
 * Description:
 * The leaf node is a group. When need add the leaf members in userTab
 *
 * Params:
 * g      [in]: the target group
 * tab    [in/out]: we need add all leaf members in this table
 * exclTab[int]: do not add the leaf members in this table into param 'tab' 
 *
 ******************************************************************************************
 */
static void addDescendantInTab(struct gData *g, hTab *tab, hTab *exclTab) {
    int i  = 0;

    if (!g) {
        return;
    }

    if (g->memberTab.numEnts > 0) {
        sTab stab;
        hEnt *ent;

        ent = h_firstEnt_(&(g->memberTab), &stab);
        while(ent) {
            if (exclTab && h_getEnt_(exclTab, ent->keyname)) {
                ent = h_nextEnt_(&stab);
                continue;
            }
            h_addEnt_(tab, ent->keyname, NULL);
            ent = h_nextEnt_(&stab);
        }
    }

    for(i = 0; i < g->numGroups; i++) {
        hEnt *uEnt = h_getEnt_(&uDataList, g->gPtr[i]->group);
        if (uEnt) {
            struct uData *u = (struct uData *)uEnt->hData;
            addDescendantInTab(u->gData, tab, exclTab);
        }
    }
    return;
}

/******************************************************************************************
 * Description:
 * Group in FAIRSHARE end with '@'. We need append all leaf members under fairshare tree
 * 'root', if there is duplicated user, use members' numShare in group@ insteadly.
 *
 * Params:
 * root    [in]: fairshare tree root node
 * uShares [in]: the array of usershare configured in FAIRSHARE.
 * nShares [in]: the number of usershares
 ******************************************************************************************
 */
static void buildIndividualOfGroup(struct shareAcct *root, struct userShares *uShares, int nShares) {
    int i;

    for (i = 0; i < nShares; i++) {
        hEnt *uEnt;
        if (uShares[i].name[strlen(uShares[i].name)-1] == '@') {
            uShares[i].name[strlen(uShares[i].name)-1] = '\0';
            uEnt = h_getEnt_(&uDataList, uShares[i].name);
            uShares[i].name[strlen(uShares[i].name)-1] = '@';
            if (uEnt) {
                struct uData *u = (struct uData *)uEnt->hData;
                addDescendantInSAcctList(u->gData, root, uShares[i].share);
            }
        }
    }
}/*buildIndividualOfGroup*/

/******************************************************************************************
 * Description:
 * helper function of buildIndividualOfGroup(), add leaf members of group@ into fairshare
 * tree.
 *
 * Params:
 * g        [in]: the target group.
 * sAcct    [in]: the shareAcct of fairshare tree node
 * numShares[in]: the share value for group@
 *
 ******************************************************************************************
 */
static void addDescendantInSAcctList(struct gData *g, struct shareAcct *sAcct, int numShares) {
    static char fname[] = "addDescendantInSAcctList()";
    int i  = 0;

    if (!g) {
        return;
    }

    if (g->memberTab.numEnts > 0) {
        sTab stab;
        hEnt *ent;
        hEnt *ent0;

        ent = h_firstEnt_(&(g->memberTab), &stab);
        while(ent) {
            struct shareAcctRef *ref = NULL, *ref0 = NULL;
            LIST_T * uList = NULL;
            struct shareAcct    *sAcct0 = NULL;

            /*check whether this is duplicated usershare, if yes, update numShares*/
            ent0 = h_getEnt_(&(sAcct->policy->userTab), ent->keyname);
            if (ent0) {
                uList = (LIST_T *)ent0->hData;
                for (ref = (struct shareAcctRef *)uList->back; 
                     ref != (struct shareAcctRef *)uList;
                     ref = ref->back) {
                    /*Find the shareAccts under root node*/
                    if (ref->sacct->parent && ref->sacct->parent->parent == NULL) {
                        ref0 = ref;
                        break;
                    }
                }
            }

            if (ref0) { /*found duplicated users, we update its numShares*/
                sAcct0 = ref0->sacct;
                sAcct0->share = numShares;
                sAcct0->priority = calcDynamicPriority(sAcct0);
                adjustOrderByPriority(sAcct0);
            } else  { 
                /*create a new one*/
                sAcct0 = (struct shareAcct *)my_calloc(1, sizeof(struct shareAcct), fname);
                sAcct0->name = safeSave(ent->keyname);
                sAcct0->path = my_calloc(strlen(sAcct->path)+strlen(ent->keyname)+2 ,sizeof(char), fname);
                sprintf(sAcct0->path, "%s/%s", sAcct->path, ent->keyname);
                sAcct0->share = numShares;
                sAcct0->parent = sAcct;
                sAcct0->policy = sAcct->policy;
                sAcct0->priority = calcDynamicPriority(sAcct0);
                inShareAcctList(sAcct->sharePolicy->sAcctList, sAcct0);

                /*add into policy userTab*/
                ent0 = h_addEnt_(&(sAcct->policy->userTab), ent->keyname, NULL);
                uList = (LIST_T *)listCreate("list of leaf shareAcct with the same username");
                ent0->hData = (int *)uList;
                ref0 = (struct shareAcctRef *)my_calloc(1, sizeof(struct shareAcctRef), fname);
                ref0->sacct = sAcct0;
                listInsertEntryAtBack(uList, (LIST_ENTRY_T *)ref0); 
            }

            ent = h_nextEnt_(&stab);
        }
    }

    for(i = 0; i < g->numGroups; i++) {
        hEnt *uEnt = h_getEnt_(&uDataList, g->gPtr[i]->group);
        if (uEnt) {
            struct uData *u = (struct uData *)uEnt->hData;
            addDescendantInSAcctList(u->gData, sAcct, numShares);
        }
    }
    return;
}

/******************************************************************************************
 * Description:
 * Add user who belong to 'others' account to 'others' usersTab
 *
 * Params:
 * g      [in]: the target group 
 * tab    [in/out]: add user into this tab
 * exclTab[in]: do not add user in exclTab into tab
 ******************************************************************************************
 */
static void addUsersForOthers(struct gData *g, hTab *tab, hTab *exclTab) {
    int i  = 0;

    if (!g) {
        return;
    }

    if (g->memberTab.numEnts > 0) {
        sTab stab;
        hEnt *ent;

        ent = h_firstEnt_(&(g->memberTab), &stab);
        while(ent) {
            if (h_getEnt_(exclTab, ent->keyname)) {
                ent = h_nextEnt_(&stab);
                continue;
            }
            h_addEnt_(tab, ent->keyname, NULL);
            ent = h_nextEnt_(&stab);
        }
    }

    for(i = 0; i < g->numGroups; i++) {
        hEnt *uEnt = NULL;
        if (h_getEnt_(exclTab, g->gPtr[i]->group)) {
            continue;
        }

        uEnt = h_getEnt_(&uDataList, g->gPtr[i]->group);
        if (uEnt) {
            struct uData *u = (struct uData *)uEnt->hData;
            addDescendantInTab(u->gData, tab, exclTab);
        }
    }
    return;
}

/******************************************************************************************
 * Description:
 * group member has 'all' defined, so we add user who does not belong to 'others'
 * account to 'others' usersTab
 *
 * Params:
 * uShares [in]: the array of configured usershare
 * nShares [in]: the number of usershares
 * tab     [in]: add the exclusive user into this tab.
 *
 ******************************************************************************************
 */
static void addOutsideUsrsForOthers(struct userShares *uShares, int nShares, hTab *tab) {
    int i = 0;
    hEnt *e = NULL;
    struct uData *u = NULL;

    for (i = 0; i < nShares; i++) {
        e = h_getEnt_(&uDataList, uShares[i].name);
        if (e) {
            u = (struct uData *)e->hData;
            if (u->flags & USER_GROUP) {
                addDescendantInTab(u->gData, tab, NULL);
            } else {
                h_addEnt_(tab, u->user, NULL);
            }
        }
    }
    return;
}

/******************************************************************************************
 * Description:
 * helper function to build faireshare treeNode's children shareAccts list. The children
 * list order is (left side)low_prioirty --> high_priority(right side).
 *
 * Params:
 * list [in]: the list of child shareAccts
 * sAcct[in]: the insert shareAcct.
 *
 ******************************************************************************************
 */
static void inShareAcctList(LIST_T *list,  struct shareAcct * sAcct) {
    LIST_ITERATOR_T     iter;
    LIST_ENTRY_T        *ent;
    struct shareAcct * curEnt = NULL;

    LIST_ITERATOR_ZERO_OUT(&iter);
    listIteratorAttach(&iter, list);

    for (ent = listIteratorGetCurEntry(&iter);
         ent != NULL;
         listIteratorNext(&iter, &ent)){
         curEnt = (struct shareAcct *)ent;

        if (sAcct->priority <= curEnt->priority) { /*find the first node which is bigger/equal than me*/
            listInsertEntryBefore(list, ent, (LIST_ENTRY_T *)sAcct);
            break;
        }
    }

    if (!ent) { /*Do not find smaller one, then I am smallest*/
        listInsertEntryAtBack(list, (LIST_ENTRY_T *)sAcct);
    }
    return; 
}

/******************************************************************************************
 * Description:
 * free up memory of 'struct shareDistributePolicy'
 *
 ******************************************************************************************
 */
static void freeShareDistributePolicy(struct shareDistributePolicy *sharePolicy) {
    if (!sharePolicy || !sharePolicy->sAcctList) {
        return;
    }
    listDestroy(sharePolicy->sAcctList, freeShareAcct);
    FREEUP(sharePolicy);
}/*freeShareDistributePolicy*/

/******************************************************************************************
 * Description:
 * free up memory of 'struct shareAcct'
 *
 ******************************************************************************************
 */
static void freeShareAcct(LIST_ENTRY_T * entry) {
    struct shareAcct *sAcct = (struct shareAcct *)entry;

    FREEUP(sAcct->name);
    FREEUP(sAcct->path);
    freeShareDistributePolicy(sAcct->sharePolicy);
    FREEUP(sAcct);
}/*freeShareAcct*/

/******************************************************************************************
 * Description:
 * free up memory of 'struct policy'
 *
 ******************************************************************************************
 */
void freeFSPolicy(struct fairsharePolicy *policy) {
    hEnt *ent;
    sTab stab;
    LIST_T *uList;

    FREEUP(policy->name);
    freeShareAcct((LIST_ENTRY_T *)policy->root);
    listDestroy(policy->userList, NULL);

    ent = h_firstEnt_(&(policy->userTab), &stab);
    while(ent) {
        uList = (LIST_T *)ent->hData;
        listDestroy(uList, NULL);
        ent = h_nextEnt_(&stab);
    }
    h_freeRefTab_(&(policy->userTab));

    FREEUP(policy);
}/*freeFSPolicy*/

/******************************************************************************************
 * Description:
 * dump the data of all fairshare trees according to input level.
 *
 ******************************************************************************************
 */
void dumpFSPolicies(const char *caller, int level) {
    hEnt *ent;
    sTab stab;
    struct fairsharePolicy * policy;
    int logmask = ls_getlogmask();

    if ((logmask & LOG_MASK(level)) == 0) {
        return;
    }
   
    ent = h_firstEnt_(&policies, &stab);
    while(ent) {
        policy = (struct fairsharePolicy *)ent->hData;
        dumpFSPolicy(policy, caller, level);
        ent = h_nextEnt_(&stab);
    }
} /*dumpFSPolicies*/

/******************************************************************************************
 * Description:
 * dump the data of a fairshare tree according to input level.
 *
 ******************************************************************************************
 */
void dumpFSPolicy(struct fairsharePolicy *policy, const char *caller, int level) {
    int logmask = ls_getlogmask();

    if ((logmask & LOG_MASK(level)) == 0) {
        return;
    }

    ls_syslog(level, "dumpFSPolicy() called by %s:\n-------------  Begin Queue <%s> fairshare accounts ------------", caller, policy->name);

    traverseFSTree(policy->root, (void *)&level, dumpSAcctInfo);
   
    ls_syslog(level, "dumpFSPolicy() called by %s:\n-------------  End Queue <%s> fairshare accounts ------------", caller, policy->name);
}/*dumpFSPolicy*/

/******************************************************************************************
 * Description:
 * helper function to dump a shareAcct's data.
 *
 * Params:
 * sAcct [in]: the target shareAcct
 * param [in]: the point to level info
 ******************************************************************************************
 */
static void dumpSAcctInfo(struct shareAcct *sAcct, void * param) {
    LIST_ENTRY_T        *ent;
    struct shareAcct    *curAcct = NULL;
    int                 level = *(int *)param;

    if (FS_IS_LEAF_NODE(sAcct)) {
        return;
    }

    ls_syslog(level, "    SHARE_INFO_FOR: %s", sAcct->path);

    /*print child shareAcct*/
    for (ent = sAcct->sharePolicy->sAcctList->back;
         ent != (LIST_ENTRY_T *)sAcct->sharePolicy->sAcctList;
         ent = ent->back){
        curAcct = (struct shareAcct *)ent;
        ls_syslog(level, "      %s: shares=%d, priority=%f, histCpuTime=%f, newUsedCpuTime=%f, runTime=%d, numStartJobs=%d, numSkippedChild=%d", 
                curAcct->name,
                curAcct->share,
                curAcct->priority,
                curAcct->histCpuTime,
                curAcct->newUsedCpuTime,
                curAcct->runTime,
                curAcct->numStartJobs,
                curAcct->numSkippedChild);
    }
}

/******************************************************************************************
 * Description:
 * Traverse fairshare tree in depth-first search, 'func' is interface how to do with 
 * each node.
 *
 * Params:
 * sAcct [in]: the current node of shareAcct
 * param [in]: the parameter used by func 
 * func  [in]: the helper function how to do with each shareAcct node.
 ******************************************************************************************
 */
static void traverseFSTree(struct shareAcct *sAcct, void *param, void(*func)(struct shareAcct *, void *)) {
    LIST_ENTRY_T        *ent;
    struct shareAcct    *curAcct = NULL;

    /*execute for share account*/
    func(sAcct, param);

    if (FS_IS_LEAF_NODE(sAcct)) {
        return;
    }

    /*execute children*/
    for (ent = sAcct->sharePolicy->sAcctList->back;
        ent != (LIST_ENTRY_T *)sAcct->sharePolicy->sAcctList;
        ent = ent->back){
        curAcct = (struct shareAcct *)ent;
        traverseFSTree(curAcct, param, func);
    }
}

/******************************************************************************************
 * Description: Worker funciton of FS scheduling.
 * Create a jobSet to cache all jobs of a fairshare queue.
 *
 * Params:
 * policy[in]: the target queue's fs policy
 *
 ******************************************************************************************
 */
struct fsQueueJobSet * sch_fs_newJobSet(struct fairsharePolicy * policy) {
    static char fname[] = "sch_fs_newJobSet";
    struct fsQueueJobSet *fsJobSet = NULL;

    if (policy == NULL) {
        return NULL;
    }

    fsJobSet = my_calloc(1, sizeof(struct fsQueueJobSet), fname);
    fsJobSet->policy = policy;
    fsJobSet->nJobs = 0;
    fsJobSet->status = M_STAGE_FS_INIT;
    fsJobSet->add = addToFSJobSet;
    fsJobSet->get = getJobFromFSJobSet;
    fsJobSet->del = rmJobFromFSJobSet;

    h_initTab_(&fsJobSet->usersTab, 101);

    return fsJobSet;
}

/******************************************************************************************
 * Description: Worker funciton of FS scheduling.
 * free up memory of a jobSet which cached the jobs of a fairshare queue.
 *
 * Params:
 * jobset[in]: the target jobset
 *
 ******************************************************************************************
 */
void sch_fs_freeJobSet(struct fsQueueJobSet * jobSet) {
    sTab stab;
    hEnt *ent;

    if (!jobSet) {
        return;
    }
    
    ent = h_firstEnt_(&(jobSet->usersTab), &stab);
    while(ent) {
        listDestroy((LIST_T *)ent->hData, NULL);
        ent = h_nextEnt_(&stab);
    }
    h_freeRefTab_(&(jobSet->usersTab));
    FREEUP(jobSet);
}

/******************************************************************************************
 * Description:
 * reset FS_SKIPABLE flag in all nodes of fairshare tree. FS_SKIPABLE is for performance
 * enhancement to avoid checking nodes which have no jobs yet during electing jobs from
 * tree. 
 *
 * Params:
 * sAcct[in]: the target shareAcct
 * param[in]: useless here, just adapt to the interface of traverseFSTree()
 *
 ******************************************************************************************
 */
static void resetSkipFlag(struct shareAcct *sAcct, void * param) {
    sAcct->numSkippedChild = 0;
    sAcct->flags &= ~FS_SKIPABLE;
    if (FS_IS_LEAF_NODE(sAcct)) {
        sAcct->jListEntCache = NULL;
    }
}

/******************************************************************************************
 * Description: Worker funciton of FS scheduling.
 * MUST be called in the begining of scheduling each fairshare queue.
 *
 * Params:
 * policy[in]: the target queue's fs policy
 *
 ******************************************************************************************
 */
void sch_fs_init(struct fairsharePolicy *policy) {
    traverseFSTree(policy->root, NULL, resetSkipFlag);
    return;
} /*sch_fs_init*/


/******************************************************************************************
 * Description: Worker funciton of FS scheduling
 * Elect a job from the user who has the highest priority. If users have the same priority,
 * then select the earliest-submitted job from those users.
 *  
 * Params:
 * jobSet    [in] : the jobset where we elect job from.
 * attachedSa[out]: the shareAcct whose job we elected.
 *
 * Return:
 * NULL - jobSet is empty now.
 * struct jData * - the point to the selected job.
 ******************************************************************************************
 */
struct jData * sch_fs_electJob(struct fsQueueJobSet *jobSet, struct shareAcct **attachedSa) {
    struct fairsharePolicy *policy = NULL;
    struct jobFSRef        *jobRef = NULL;
    hEnt                   *ent = NULL;
    struct shareAcct       *shareAcct0 = NULL;
    struct jobFSRef        *jobRef0 = NULL;
    hEnt                   *ent0 = NULL;
    
    *attachedSa = NULL;
    INC_CNT(PROF_CNT_sch_fs_electJob);

    if (jobSet->nJobs < 1) {
        return NULL;
    }
   
    if (logclass & LC_FAIR) {
        /*This is only for development testing, please do not use it
         *in production env.
         */
        dumpFSPolicy(jobSet->policy, "sch_fs_electJob()", LOG_DEBUG3);
    }
 
    /*init*/
    policy = jobSet->policy;

    /*select a job from highest priority user*/
    jobRef0 = electAJob(jobSet, policy->root, &shareAcct0, &ent0);
    
    if (jobRef0 == NULL && jobSet->nJobs > 0) {
        /*need check rest jobs in jobset whose user is not in FS tree,
         *for those jobs, there is no attached shareAcct in FS tree.
         */
        sTab  stab;
        LIST_T *jList;

        ent = h_firstEnt_(&(jobSet->usersTab), &stab);
        while(ent) {
            jList = (LIST_T *)ent->hData;

            if (jobRef0 == NULL) {
                jobRef0 = (struct jobFSRef *)listGetBackEntry(jList);
                ent0 = ent;
            } else {
                jobRef = (struct jobFSRef *)listGetBackEntry(jList);
                if (jobRef && (jobRef->idx < jobRef0->idx)) {
                    jobRef0 = jobRef;
                    ent0 = ent; 
                }
            }
            ent = h_nextEnt_(&stab);
        }
    }

    /*Finally we find the highest priority job, we need fetch it out of job set*/
    if (jobRef0) {
        struct jData  *job0 = NULL;

        job0 = jobRef0->job;
        *attachedSa = shareAcct0;

        /*Remove it from user's joblist, to make sure the job be looked at by the 
         *scheduler only once.
         */
        if (logclass & (LC_SCHED | LC_FAIR)) {
            ls_syslog(LOG_DEBUG1, "\
%s: Job <%s> is elected. user <%s>, queue <%s>, shareAcct <%s>, priority <%f>, idx <%d>", 
                    "sch_fs_electJob()", lsb_jobid2str(job0->jobId), job0->userName,
                    job0->qPtr->queue, 
                    (shareAcct0 ? shareAcct0->path : "-"),
                    (shareAcct0 ? shareAcct0->priority : 0.00),
                    jobRef0->idx);
        }
        (*jobSet->del)(jobSet, shareAcct0, (void *)jobRef0, (void *)ent0);

        return job0;
    } else {
        return NULL;
    }
} /*sch_fs_electJob*/


/******************************************************************************************
 * Description:
 * Helper function of sch_fs_electJob(). The function will choose the highest priority job
 * of the user with highest dynamic priority from current shareAcct node.
 *
 * Params:
 * jobSet [in]: the jobSet from which we select a job.
 * node   [in]: the node from which we elect the job of the offspring with the highest
 *              dynamic priority.
 * sAcctOut[out]: the shareAcct which the elected job belong to.
 * jobSetEntOut[out]: the hEnt which the elected job refer to in the jobSet.
 * 
 * Return:
 * NULL: the current node has no jobs.
 * jobFSRef: the selected job reference in jobSet.
 * 
 ******************************************************************************************
 */
static struct jobFSRef *
electAJob(struct fsQueueJobSet *jobSet,
          struct shareAcct *node,
          struct shareAcct **sAcctOut,
          hEnt **jobSetEntOut) {

    struct jobFSRef   * jobRef0 = NULL;
    LIST_ENTRY_T      * entry = NULL;
    struct shareAcct  * sAcct = NULL, *sAcct0 = NULL, * sAcctTmp = NULL;
    hEnt              *jobSetEntTmp = NULL;

    *sAcctOut = NULL;
    *jobSetEntOut = NULL;

    if (node->flags & FS_SKIPABLE) {
        return NULL;
    }

    if (node->flags & FS_IS_DEFAULT) {
        updSAcctSkippedFlag(node);
        return NULL;
    }

    /*If this is a leaf node which cannot be skipable*/
    if (FS_IS_LEAF_NODE(node)) {
        jobRef0 = (*jobSet->get)(jobSet, node, jobSetEntOut);
        if (jobRef0 == NULL) {
            updSAcctSkippedFlag(node);
        }
        *sAcctOut = node;
        return jobRef0;
    }

    /*If this is middle node, elect job from the highest user. For users with the same priority, we elect the one 
     *who has the earliest job.
     */
    for (entry = node->sharePolicy->sAcctList->back;
         entry != (LIST_ENTRY_T *)node->sharePolicy->sAcctList;
         entry = entry->back) {
        struct jobFSRef  * jobRef = NULL; 
        sAcct = (struct shareAcct  *)entry;

        if (jobRef0) {
            if (sAcct->priority == sAcct0->priority) {
                jobRef = electAJob(jobSet, sAcct, &sAcctTmp, &jobSetEntTmp);
                if (jobRef) {
                    if (jobRef->idx < jobRef0->idx) {
                        /*we find a job that is in a more prominent position.*/
                        *sAcctOut = sAcctTmp;
                        *jobSetEntOut = jobSetEntTmp;
                        jobRef0 = jobRef;
                        sAcct0 = sAcct;
                    }
                }
            } else {
                break;
            }
        } else {
            jobRef = electAJob(jobSet, sAcct, &sAcctTmp, &jobSetEntTmp);
            if(jobRef == NULL) {
                continue;
            }
            *sAcctOut = sAcctTmp;
            *jobSetEntOut = jobSetEntTmp;
            jobRef0 = jobRef;
            sAcct0 = sAcct;
        }
    }

    return jobRef0;
} /*electJob*/

/******************************************************************************************
 * Description:
 * Helper function of electJob. Set FS_SKIPABLE in the node, and update parent flags 
 * accordingly.
 *
 * Params:
 * node[in]: the target shareAcct. 
 ******************************************************************************************
 */
static void updSAcctSkippedFlag(struct shareAcct *node) {
    struct shareAcct *sAcct = NULL;

    if (node == NULL) {
        return;
    }

    if (FS_IS_LEAF_NODE(node)) {
        if (node->numSkippedChild == 1) {
            return;
        }
        node->numSkippedChild = 1;
        node->flags |= FS_SKIPABLE;
    } else {
        return;
    }

    sAcct = node->parent;
    while (sAcct) {
        sAcct->numSkippedChild += 1;
        if (sAcct->numSkippedChild == sAcct->sharePolicy->sAcctList->numEnts) {
            sAcct->flags |= FS_SKIPABLE;
            sAcct = sAcct->parent;
        } else {
            break;
        }
    }
}

/******************************************************************************************
 * Description: worker function for fairshare queue jobSet
 * add a job to jobSet
 *
 * Params:
 * jobSet[in]: the jobSet which we add job.
 * data  [in]: the jData we want to add.
 * 
 ******************************************************************************************
 */
static int addToFSJobSet(struct fsQueueJobSet * jobSet, void *data) {
    hEnt * ent = NULL;
    int    new;
    struct jobFSRef *fsRef = NULL;
    struct jData * jPtr = NULL;

    jPtr = (struct jData *)data;
    jobSet->nJobs++;

    ent = h_addEnt_(&jobSet->usersTab, jPtr->userName, &new);

    if (new) {
        ent->hData = (int *)listCreate("user's job reference list in fs queue");
    }

    /*add job into user's job list*/
    fsRef = (struct jobFSRef *)my_calloc(1, sizeof(struct jobFSRef), "addToFSJobSet()");
    fsRef->job = jPtr;
    fsRef->idx = jobSet->nJobs;
    listInsertEntryAtFront((LIST_T *)ent->hData, (LIST_ENTRY_T *)fsRef);

    return TRUE;
}

/******************************************************************************************
 * Description: worker function for fairshare queue jobSet
 * get input user's top job from jobSet
 *
 * Params:
 * jobSet[in] : the jobSet which we get job from.
 * data  [in] : the user whose we care about.
 * entOut[out]: the hEnt which the returned job refer to in the jobSet.
 *
 * Return:
 * NULL: user has no jobs anymore.
 * jobFSRef: the top job from input user.
 * 
 ******************************************************************************************
 */
static void * getJobFromFSJobSet(struct fsQueueJobSet *jobSet, void *data, hEnt **entOut) {
    struct shareAcct * sAcct;
    LIST_T *jList = NULL;
    sTab    stab;
    struct jobFSRef * job = NULL;
    hEnt   *jListEnt = NULL;

    sAcct = (struct shareAcct *)data;
    *entOut = NULL;

    if(sAcct->flags & FS_LEAF_GROUP) {/*For a group shareAcct*/
        struct jobFSRef *job0 = NULL;
        hEnt *jListEnt0 = NULL;

        if (sAcct->sharePolicy->flags & FS_OUT_OTHERS) {
            /*Currently, others' usersTab records the users who do not belong to group 'others'.
             *We need go through usersTab in jobSet, then find the job with smallest idx for users
             *not in shareAcct's usersTab.
             */
            jListEnt = h_firstEnt_(&jobSet->usersTab, &stab);
            while(jListEnt) {
                if (! h_getEnt_(sAcct->sharePolicy->usersTab, jListEnt->keyname)) {
                    if (!job0) {
                        job0 = (struct jobFSRef *)listGetBackEntry((LIST_T *)jListEnt->hData);
                        jListEnt0 = jListEnt;
                    } else {
                        job = (struct jobFSRef *)listGetBackEntry((LIST_T *)jListEnt->hData);
                        if (job->idx < job0->idx) {
                            job0 = job;
                            jListEnt0 = jListEnt;
                        }                       
                    }
                }
                jListEnt = h_nextEnt_(&stab);
            }
        } else { 
            /*sharePolicy->usersTab caches members belong to a group type shareAcct for the following conditions:
             *1. group doesn't configure USER_SHARE,
             *2. group which has no 'all' configures 'others' in USER_SHARE
             */
            if (sAcct->sharePolicy->usersTab->numEnts == 0) { /*group member is all*/
                /*Find the job with the smallest idx in jobSet*/
                jListEnt = h_firstEnt_(&jobSet->usersTab, &stab);
                while(jListEnt) {
                    if (job0 == NULL) {
                        job0 = (struct jobFSRef *)listGetBackEntry((LIST_T *)jListEnt->hData);
                        jListEnt0 = jListEnt;
                    } else {
                        job = (struct jobFSRef *)listGetBackEntry((LIST_T *)jListEnt->hData);
                        if (job->idx < job0->idx) {
                            job0 = job;
                            jListEnt0 = jListEnt;
                        }
                    }
                    jListEnt = h_nextEnt_(&stab);
                }
            } else {
                /*Find the job with the smallest idx for users in shareAcct's usersTab*/
                hEnt *uEnt = NULL;
                uEnt = h_firstEnt_(sAcct->sharePolicy->usersTab, &stab);
                while(uEnt) {
                    jListEnt = h_getEnt_(&jobSet->usersTab, uEnt->keyname);

                    if (jListEnt) {
                        if (job0 == NULL) {
                            job0 = (struct jobFSRef *)listGetBackEntry((LIST_T *)jListEnt->hData);
                            jListEnt0 = jListEnt;
                        } else {
                            job = (struct jobFSRef *)listGetBackEntry((LIST_T *)jListEnt->hData);
                            if (job->idx < job0->idx) {
                                job0 = job;
                                jListEnt0 = jListEnt;
                            }
                        }
                    }
                    uEnt = h_nextEnt_(&stab);
                }
            }       
        }

        if (job0) {
            *entOut = jListEnt0;
        }
        return job0;

    } else { /*For single user shareAcct*/
        if (sAcct->jListEntCache == NULL) {
            jListEnt = h_getEnt_(&(jobSet->usersTab), sAcct->name);
            if (!jListEnt) {
                return NULL;
            }
            /*cache jList to avoid hash sAcct again, reduce string compare*/
            sAcct->jListEntCache = jListEnt;
        } else {
            jListEnt = sAcct->jListEntCache;
        }
        jList = (LIST_T *)jListEnt->hData;
        *entOut = jListEnt;
        
        return (struct jobFSRef *)listGetBackEntry(jList);
    }
} /*getJobFromFSJobSet*/

/******************************************************************************************
 * Description: worker function for fairshare queue jobSet
 * remove input user's a job from jobSet
 *
 * Params:
 * jobSet[in]: the target jobSet
 * sAcct [in]: the user whose job we will remove
 * data  [in]: the job we will remove
 * param [in]: the hEnt which the job refer to in the jobSet. 
 *
 ******************************************************************************************
 */
static int rmJobFromFSJobSet(struct fsQueueJobSet *jobSet, struct shareAcct *sAcct, void *data, void *param) {
    LIST_T * jList = NULL;
    struct jobFSRef *jobRef;
    hEnt *jListEnt;

    jobRef = (struct jobFSRef *)data;
    jListEnt = (hEnt *)param;
    jobSet->nJobs--;

    /*rm it from user's job list*/
    jList = (LIST_T *)jListEnt->hData;
    listRemoveEntry(jList, (LIST_ENTRY_T *)jobRef);
    FREEUP(jobRef);
    if (jList->numEnts < 1) {
        updSAcctSkippedFlag(sAcct);
    }

    return TRUE;
} /*rmJobFromFSJobSet*/


/******************************************************************************************
 * Description:
 * Create relationship between job and shareAcct. Update shareAcct's job counter for this
 * new related job.
 *
 * Params:
 * policy[in]: the target policy
 * jPtr  [in]: the target job
 * sa    [in]: the target shareAcct 
 *
 * Return:
 * 
 ******************************************************************************************
 */
void attachJob2FSTree(struct fairsharePolicy *policy, struct jData *jPtr, struct shareAcct *sa, char * caller) {
    static char fname[] = "attachJob2FSTree()";
    struct shareAcct *sAcct = NULL;

    sAcct = sa;
    if (!sAcct) {
        sAcct = getSAcctForJob(policy, jPtr, 1);
    }

    if (sAcct) {
        SACCT_NUMSTARTJOBS_UPDATE(jPtr->numHostPtr, sAcct, fname); 
        jPtr->sa = sAcct;

        /*update priority and order of shareAcct*/
        updSAcctPriorityByPath(sa);
        if (logclass & (LC_SCHED | LC_FAIR)) {
            ls_syslog(LOG_DEBUG1, "\
%s: Job <%s> is attached to shareAcct <%s> called by <%s>. New dynamic priroity is <%f>, numStartJobs <%d>, newUsedCpuTime <%f>, histCpuTime <%f>, runTime <%d>", 
                    fname,
                    lsb_jobid2str(jPtr->jobId),
                    sAcct->path,
                    caller ? caller:"-",
                    sAcct->priority,
                    sAcct->numStartJobs,
                    sAcct->newUsedCpuTime,
                    sAcct->histCpuTime,
                    sAcct->runTime);
        }
    }

    return;
} /*attachJob2FSTree*/

/******************************************************************************************
 * Description:
 * Check and return job's potential matched shareAcct.
 *
 * Params:
 * policy[in]: the target fs policy
 * jPtr  [in]: the job we try to find its potential shareAcct.
 * isGetHPrio[in]:
 *     0 - only check whether job's owner has shareAcct in the fairshare tree. Do not need
 *         care about the highest priority one.
 *     1 - find the shareAcct with the highest priority if job owner has.
 *
 * Return:
 * NULL - job's owner has no share in the fairshare tree.
 * struct shareAcct * - the found shareAcct which job's owner has.
 * 
 ******************************************************************************************
 */
struct shareAcct * getSAcctForJob(struct fairsharePolicy *policy, struct jData *jPtr, int isGetHPrio) {
    struct shareAcct *sAcct = NULL;
    hEnt *ent = NULL;
    struct shareAcctRef *ref = NULL, *ref0 = NULL;
    LIST_T * uList = NULL;


    /*find user in userTab for the job, then get the shareAcct with the highest priority*/
    ent = h_getEnt_(&(policy->userTab), jPtr->userName);
    if (ent) {
        uList = (LIST_T *)ent->hData;
        for (ref = (struct shareAcctRef *)uList->back; 
             ref != (struct shareAcctRef *)uList;
             ref = ref->back) {
            /*Find the shareAcct with the highest priority*/
            if (ref0 == NULL) {
                ref0 = ref;
                if (!isGetHPrio) {
                    return ref0->sacct;
                }
            } else {
                if ( ref0->sacct->priority < ref->sacct->priority ) {
                    ref0 = ref;
                }
            }
        }
        sAcct = ref0->sacct;
    } else {
        /*Check whether the job belong to a group type shareAcct,
         *e.g. others, or user group has no shares configured.
         */
        struct uData *u = NULL;
        int i;

        if (!isGetHPrio) {
            /*check whether there is potential 'default' could be used*/
            ent = h_getEnt_(&(policy->userTab), "default");
            if (ent) {
                uList = (LIST_T *)ent->hData;
                for (ref = (struct shareAcctRef *)uList->back;
                     ref != (struct shareAcctRef *)uList;
                     ref = ref->back) {
                    if (ref->sacct->flags & FS_ALL_DEFAULT) {
                        return ref->sacct;
                    }
                }
            }
        }

        /*go through all 'others' if have*/
        ent = h_getEnt_(&(policy->userTab), "others");
        if (ent) {
            struct shareAcct * tmp = NULL;
            uList = (LIST_T *)ent->hData;
            for (ref = (struct shareAcctRef *)uList->back;
                 ref != (struct shareAcctRef *)uList;
                 ref = ref->back) {
                tmp = ref->sacct;
                if (!tmp->sharePolicy) {
                    continue;
                }
                if ((tmp->sharePolicy->flags & FS_IN_OTHERS && h_getEnt_(tmp->sharePolicy->usersTab, jPtr->userName)) 
                    || (tmp->sharePolicy->flags & FS_OUT_OTHERS && !h_getEnt_(tmp->sharePolicy->usersTab, jPtr->userName))) {
                    if (ref0 == NULL) {
                        ref0 = ref;
                        if (!isGetHPrio) {
                            return sAcct = ref0->sacct;
                        }
                    } else {
                        if (ref0->sacct->priority 
                                < ref->sacct->priority) {
                            ref0 = ref;
                        }
                    }
                }
            }
        }

        /*continue to check user's related usergroup*/
        u = getUserData(jPtr->userName);
        initUserGroup(u);
        for (i = 0; i < u->numGrpPtr; i++) {
             ent = h_getEnt_(&(policy->userTab), u->gPtr[i]->user);
            if (ent) {
                /*go through all shareAcctRef with the same name*/
                uList = (LIST_T *)ent->hData;
                for (ref = (struct shareAcctRef *)uList->back;
                     ref != (struct shareAcctRef *)uList;
                     ref = ref->back) {
                    if (ref0 == NULL) {
                        ref0 = ref;
                        if (!isGetHPrio) {
                            return sAcct = ref0->sacct;
                        }
                    } else {
                        if (ref0->sacct->priority 
                                < ref->sacct->priority) {
                            ref0 = ref;
                        }
                    }
                }
            }
        }
        if (ref0) {
            sAcct = ref0->sacct;
        }
    }

    return sAcct;
} /*getSAcctForJob*/

/******************************************************************************************
 * Description:
 * Destory the relationship between job and shareAcct.
 *
 * Params:
 * jPtr  [in]: the target job
 * caller[in]: who call this func
 * 
 * Return:
 * 
 ******************************************************************************************
 */
void detachedJobFromFSTree(struct jData *jPtr, char * caller) {
    struct shareAcct *sa = NULL;

    if (jPtr->sa == NULL) {
        return;
    }

    sa = jPtr->sa;
    if (sa) {
        SACCT_NUMSTARTJOBS_UPDATE(-(jPtr->numHostPtr), sa, "detachedJobFromFSTree()");
        updSAcctPriorityByPath(sa);

        if (logclass & (LC_SCHED | LC_FAIR)) {
            ls_syslog(LOG_DEBUG1, "%s: Job <%s> is detached from shareAcct <%s> called by <%s>. New dynamic priroity is <%f>, numStartJobs <%d>", 
                    "detachedJobFromFSTree()",
                    lsb_jobid2str(jPtr->jobId),
                    sa->path,
                    caller ? caller:"-",
                    sa->priority,
                    sa->numStartJobs);
        }
    }

    jPtr->sa = NULL;
} /*detachedJobFromFSTree*/

/******************************************************************************************
 * Description:
 * Check and add new user into fairshare tree if some nodes have member is 'all' with 
 * configuring 'default' share.
 *
 * Params:
 * qPtr    [in]: the target queue 
 * userName[in]: the target user
 ******************************************************************************************
 */
void updAliveUserInFSTree(struct qData *qPtr, char *userName) {
    static char fname[] = "updAliveUserInFSTree()";
    struct fairsharePolicy *policy;
    hEnt *ent = NULL;
    int    added = 0;


    if (! (qPtr->qAttrib & Q_ATTRIB_FS)) {
        return;
    }

    policy = qPtr->policy;

    ent = h_getEnt_(&(policy->userTab), userName);

    /*FS tree has shareAccts for this user, just use it*/
    if (ent) {
        return;
    }
    
    /*Continue to check whether there is default shareAcct for group which has 'all' member*/
    ent = h_getEnt_(&(policy->userTab), "default");
    if (ent) {
        LIST_T *uList = NULL;
        struct shareAcctRef *ref = NULL, *topDef = NULL;

        uList = (LIST_T *)ent->hData;

        for (ref = (struct shareAcctRef *)uList->back;
             ref != (struct shareAcctRef *)uList;
             ref = ref->back) {
            if (ref->sacct->flags & FS_ALL_DEFAULT) {
                /*Insert a new shareAcct before 'default' shareAcct which parent has all
                 *members. 
                 *Find the 'default' of root if there is. If we cannot find child 'default'
                 *where we can create the new user, we will create it under root's default.
                 */
                if (!topDef) {
                    if (ref->sacct->parent && ref->sacct->parent->parent == NULL) {
                        topDef = ref;
                        continue;
                    }
                } 
                
                /*create alive user's shareAcct under non-root node which has 'default' and member is all*/
                appendAliveUser(policy, ref, userName);
                added = 1;
            }
        }
        if (!added && topDef) {
            /*create alive user under root node which configure 'default'*/
            appendAliveUser(policy, topDef, userName);
            added = 1;
        }
    }

    if (logclass & LC_FAIR) {
        if (added) {
            dumpFSPolicy(policy, fname, LOG_DEBUG2);
        } else {
            ls_syslog(LOG_DEBUG, "%s: User <%s> cannot have shares in queue <%s>. We will schedule his jobs after jobs of users who have shares.", fname, userName, policy->name);
        }
    }

    return;
} /*updAliveUserInFSTree*/

/******************************************************************************************
 * Description:
 * Helper function of updAliveUserInFSTree(), add new user into fairshare tree which
 * node may configure 'all' user with 'default' share.
 *
 * Params:
 * policy  [in]: the target queue's fairshare policy.
 * ref     [in]: the reference of shareAcct in policy's userTab.
 * userName[in]: the new user' name.
 *
 ******************************************************************************************
 */
static void 
appendAliveUser(struct fairsharePolicy *policy, struct shareAcctRef *ref, char *userName) {
    static char fname[] = "appendAliveUser()";
    struct shareAcct *newSA;
    int              new;
    hEnt             *ue;
    LIST_T           *ul;
    struct shareAcctRef *ur;
    struct uData        *u;
    struct ugrpAttrs    *ua;


    /*append user share to group's uData(group should not be root node)*/
    if (ref->sacct->parent && ref->sacct->parent->parent != NULL) {
        u = getUserData(ref->sacct->parent->name);
        if (u->gData && u->gData->ugrpAttrs) {
            struct userShares  *us;

            ua =  u->gData->ugrpAttrs;
            us = realloc(ua->userShares, (ua->numUserShares + 1)*sizeof(struct userShares)); 
            if (us == NULL) {
                ls_syslog(LOG_ERR, "\
%s: realloc() failed. Cannot append new shareAcct <%s/%s> for queue <%s>.",
                        fname, ref->sacct->parent->path, userName, policy->name);
                return;
            }
            us[ua->numUserShares].name = safeSave(userName);
            us[ua->numUserShares].share = ref->sacct->share;
            ua->userShares = us;
            ua->numUserShares++;
        }
    }

    /*create structure*/
    newSA = (struct shareAcct *)my_calloc(1, sizeof(struct shareAcct), fname);
    newSA->name = safeSave(userName);
    newSA->path = my_calloc(strlen(ref->sacct->parent->path)+strlen(userName)+2, sizeof(char), fname);
    sprintf(newSA->path, "%s/%s", ref->sacct->parent->path, userName);
    newSA->share = ref->sacct->share;
    newSA->parent = ref->sacct->parent;
    newSA->policy = policy; 
    newSA->priority = calcDynamicPriority(newSA);

    /*insert related lists and table*/
    listInsertEntryAfter(ref->sacct->parent->sharePolicy->sAcctList, (LIST_ENTRY_T *)ref->sacct, (LIST_ENTRY_T *)newSA);
    ue = h_addEnt_(&(policy->userTab), userName, &new);
    if (new) {
        ul = (LIST_T *)listCreate("list of leaf shareAcct with the same username");
        ue->hData = (int *)ul;
    } else {
        ul = (LIST_T *)ue->hData;
    }
    ur = (struct shareAcctRef *)my_calloc(1, sizeof(struct shareAcctRef), fname);
    ur->sacct = newSA;
    listInsertEntryAtBack(ul, (LIST_ENTRY_T *)ur);

    if (logclass & LC_FAIR) {
        ls_syslog(LOG_DEBUG, "%s: New shareAcct <%s> added for queue <%s>.", fname, newSA->path, policy->name);
    }
    return;
}/*appendAliveUser*/

/******************************************************************************************
 * Description:
 * Update shareAcct's workload in FS tree.
 *
 * Params:
 * sAcct [in]: the shareAcct we will update.
 * upd   [in]: the delta change of workload
 * caller[in]: caller's name
 *
 ******************************************************************************************
 */
void 
updShareAcctStats(struct shareAcct * sAcct, struct shareAcctUpd *upd, const char *caller)
{
    static char fname[] = "updShareAcctStats()";
    struct shareAcct *curSAcct = NULL;

    curSAcct = sAcct;
    switch(upd->type) 
    {
        case SHARE_ACCT_UPD_CPUTIME:
            while(curSAcct) {
                curSAcct->newUsedCpuTime += upd->update.newUsedCpuTime;
                if (curSAcct->newUsedCpuTime < 0.0) { /*shouldn't get here, just for protection*/
                    curSAcct->newUsedCpuTime = 0.0;
                }

                if (logclass & LC_FAIR) {
                    ls_syslog(LOG_DEBUG1, "\
%s: Update <%s> newUsedCpuTime, new value <%f>, delta part <%f>, caller %s",
                            fname, curSAcct->path, 
                            curSAcct->newUsedCpuTime, 
                            upd->update.newUsedCpuTime,
                            caller);
                }
                curSAcct = curSAcct->parent;
            }
            break;
        case SHARE_ACCT_UPD_DECAYED_CPUTIME:
            while(curSAcct) {
                curSAcct->histCpuTime += upd->update.decayedCpuTime;

                if (logclass & LC_FAIR) {
                    ls_syslog(LOG_DEBUG1, "\
%s: Update <%s> histCpuTime, new value <%f>, delta part <%f>, caller %s",
                            fname, curSAcct->path, 
                            curSAcct->histCpuTime, 
                            upd->update.decayedCpuTime,
                            caller);
                }
                curSAcct = curSAcct->parent;
            }
            break;
        case SHARE_ACCT_UPD_RUNTIME:
            while(curSAcct) {
                curSAcct->runTime += upd->update.runTime;
                if (curSAcct->runTime < 0) { /*shouldn't get here, just for protection*/
                    curSAcct->runTime = 0;
                }
                if (logclass & LC_FAIR) {
                    ls_syslog(LOG_DEBUG1, "\
%s: Update <%s> runTime, new value <%d>, delta part <%d>, caller %s",
                            fname, curSAcct->path,
                            curSAcct->runTime,
                            upd->update.runTime,
                            caller);
                }
                curSAcct = curSAcct->parent;
            }
            break;
        case SHARE_ACCT_UPD_DECAYTIMES:
            /*check whether need decay*/
            {
                float decayRate = 0.0;
                double multiplier;

                if(sAcct->policy->qPtr->fsFactors.histHours == 0.0) {
                    break;
                } else if (sAcct->policy->qPtr->fsFactors.histHours > 0.0) {
                    decayRate = (float) pow(10.0, -1.0/(sAcct->policy->qPtr->fsFactors.histHours * 4));
                } else {
                    decayRate = clsDecay;
                }

                multiplier = pow((double)decayRate, (double)(upd->update.decayTimes - 1));
            
                sAcct->histCpuTime = (float)(sAcct->histCpuTime * decayRate + sAcct->newUsedCpuTime) * multiplier;
                sAcct->newUsedCpuTime = 0.0;
                if (logclass & LC_FAIR) {
                    ls_syslog(LOG_DEBUG1, "\
%s: Update <%s> decayed cpu time, new histCpuTime <%f>, decayTimes <%d>, newUsedCpuTime reset to 0.0, caller %s",
                            fname,sAcct->path,sAcct->histCpuTime,upd->update.decayTimes,caller);
                }
            } 
            break;
        case SHARE_ACCT_UPD_NUMSTARTJOBS:
            while(curSAcct) {
                curSAcct->numStartJobs += upd->update.numStartJobs;
                if (curSAcct->numStartJobs < 0) { /*shouldn't get here, just for protection*/
                    curSAcct->numStartJobs = 0;
                }
                if (logclass & LC_FAIR) {
                    ls_syslog(LOG_DEBUG1, "\
%s: Update <%s> numStartedJobs, new value <%d>, delta part <%d>, caller %s", 
                            fname, curSAcct->path,
                            curSAcct->numStartJobs, 
                            upd->update.numStartJobs,
                            caller);
                }
                curSAcct = curSAcct->parent;
            }
            break;
        default:
            break;
    }

    return;
} /*updShareAcctStats*/

/******************************************************************************************
 * Description:
 * Update priority and order of shareAccts in the saap(share account associated path) in
 * fairshare tree
 *
 * Params:
 * sAcct[in]: the node of shareAcct we want to update its saap.
 *
 ******************************************************************************************
 */
void updSAcctPriorityByPath(struct shareAcct *sAcct) {
    struct shareAcct *curSAcct = sAcct;
    while(curSAcct) {
        curSAcct->priority = calcDynamicPriority(curSAcct);
        adjustOrderByPriority(curSAcct);
        curSAcct = curSAcct->parent;
    }
    return;
} /*updSAcctPriorityByPath*/

/******************************************************************************************
 * Description:
 * Calculate dynamic priority for shareAcct: 
 * 
 * Formula:
 * prioirty = shares/(cputime*cpuTimeFactor + runtime*runTimeFactor + (njobs+1)*runJobFactor)
 *
 * Params:
 * sAcct[in]: the target sAcct
 *
 * Return:
 * the dynmaic priority of the shareAcct
 ******************************************************************************************
 */
static float calcDynamicPriority(struct shareAcct * sAcct){
    static char fname[] = "calcDynamicPriority()";
    struct qData *qPtr = sAcct->policy->qPtr;
    float load_metric = 0;
    float priority;

    float cpuTimeFactor_t = qPtr->fsFactors.cpuTimeFactor != -1 ? qPtr->fsFactors.cpuTimeFactor : cpuTimeFactor;
    float runTimeFactor_t = qPtr->fsFactors.runTimeFactor != -1 ? qPtr->fsFactors.runTimeFactor : runTimeFactor;
    float runJobFactor_t = qPtr->fsFactors.runJobFactor != -1 ? qPtr->fsFactors.runTimeFactor : runJobFactor;

    if (logclass & LC_FAIR) {
        ls_syslog(LOG_DEBUG2, "\
%s: User share <%s>, effect fairshare factor: cpuTimeFactor = %f, runTimeFactor = %f, runJobFactor = %f",
                fname, sAcct->path, cpuTimeFactor_t, runTimeFactor_t, runJobFactor_t);
    }    

    load_metric = ((sAcct->newUsedCpuTime + sAcct->histCpuTime)/3600.0)*cpuTimeFactor_t
                    + (sAcct->runTime/3600.0)*runTimeFactor_t 
                    + (sAcct->numStartJobs+1)*runJobFactor_t;

    if (load_metric < 0.01) {
        load_metric = 0.01;
    }

    priority = sAcct->share/load_metric;

    if (logclass & LC_FAIR) {
        ls_syslog(LOG_DEBUG2, "\
%s: User share <%s>, priority = %f, histCpuTime = %f(s), newUsedCpuTime = %f(s), runTime = %d(s), numStartJobs = %d", 
                fname, sAcct->path, priority, sAcct->histCpuTime, sAcct->newUsedCpuTime, sAcct->runTime, sAcct->numStartJobs);
    }

    return(priority);
}/*calcDynamicPriority*/

/******************************************************************************************
 * Description:
 * The priority of node is changed according to workload changed, adjust shareAcct node's
 * pos among its siblings
 *
 * Params:
 * sAcct[in]: the shareAcct which pos need be adjusted. 
 *
 ******************************************************************************************
 */
void adjustOrderByPriority(struct shareAcct *sAcct) {
    LIST_T  *siblings = NULL;
    LIST_ENTRY_T *insertBefore = NULL;
    struct shareAcct *leftSAcct = (struct shareAcct *)sAcct->back,
                     *rightSAcct = (struct shareAcct *)sAcct->forw;

    if(sAcct->parent == NULL) {
        /*this is root node, we don't need adjust it*/
        return;
    }

    siblings = sAcct->parent->sharePolicy->sAcctList;

    if ((LIST_ENTRY_T *)leftSAcct == (LIST_ENTRY_T *)siblings ||
        ((LIST_ENTRY_T *)rightSAcct != (LIST_ENTRY_T *)siblings
           && rightSAcct->priority < sAcct->priority)) {
        /*Try to find a new pos in right side*/
        while((LIST_ENTRY_T *)rightSAcct != (LIST_ENTRY_T *)siblings) {
            if (sAcct->priority <= rightSAcct->priority) {
                break;
            }
            rightSAcct = rightSAcct->forw;
        }
        /*We have found a new pos*/
        if (sAcct->forw != rightSAcct) {
            insertBefore = (LIST_ENTRY_T *)rightSAcct;
        }
    } else if ((LIST_ENTRY_T *)rightSAcct == (LIST_ENTRY_T *)siblings ||
               ((LIST_ENTRY_T *)leftSAcct != (LIST_ENTRY_T *)siblings
                 && leftSAcct->priority > sAcct->priority)) {
        /*Try to find a new pos in left side*/
        while((LIST_ENTRY_T *)leftSAcct != (LIST_ENTRY_T *)siblings) {
            if (sAcct->priority >= leftSAcct->priority) {
                break;
            }
            leftSAcct = leftSAcct->back;
        }
        /*We have found a new pos*/
        if (sAcct != leftSAcct->forw) {
            insertBefore = (LIST_ENTRY_T *)leftSAcct->forw;
        }
    }

    /*No need to change position*/
    if (!insertBefore) {
        return;
    }

    /*remove the adjusted node from sibings*/
    listRemoveEntry(siblings, (LIST_ENTRY_T *)sAcct);

    /*insert the adjusted node into new pos in sibling*/
    listInsertEntryBefore(siblings, insertBefore, (LIST_ENTRY_T *)sAcct);
    return;
}

/******************************************************************************************
 * Description:
 * When job finished, we will accumulate the last cputime reported by job status update to
 * fairshare tree.
 *
 * Params:
 * jp[in]: the target job
 *
 ******************************************************************************************
 */
void accumHistCpuTime(struct jData *jp) {
    static char fname[] = "accumHistCpuTime()";
    float updatedCpuTime = 0;

    if(!jp->sa) {
        return;
    }

    if ((jp->runRusage.utime + jp->runRusage.stime) > 0) {
        updatedCpuTime = fabsf(jp->cpuTime - (jp->runRusage.utime + jp->runRusage.stime));
    } else {
        if ( jp->cpuTime > MIN_CPU_TIME ) {
            updatedCpuTime = jp->cpuTime;
        } else {
            updatedCpuTime = 0;
        }
    }

    if(updatedCpuTime < MIN_CPU_TIME) {
        return;
    }

    SACCT_CPUTIME_UPDATE(updatedCpuTime, jp->sa, fname);
    if (logclass & LC_FAIR) {
        ls_syslog(LOG_DEBUG1, "%s: updated SAAP <%s> cputime <%f> for job <%s>",
                fname, jp->sa->path,
                updatedCpuTime,
                lsb_jobid2str(jp->jobId));
    }
}/*accumHistCpuTime*/

/*Decay history data and update FS tree*/
/******************************************************************************************
 * Description:
 * Decay history data and update FS tree every CALCULATE_INTERVAL, the decay rate is 
 * decided by HIST_HOURS
 *
 * Params:
 * eventTime: the timestamp when the func is called. 
 *
 ******************************************************************************************
 */
void updAllSAcctForDecay(time_t eventTime) {
    static char fname[] = "updAllSAcctForDecay";
    static time_t lastDecayTime = 0;
    int    decayTimes = 0;
    hEnt   *ent;
    sTab   stab;
    struct fairsharePolicy *policy = NULL;
    
    if (lastDecayTime == 0)
        lastDecayTime = eventTime;

    if (eventTime - lastDecayTime < CALCULATE_INTERVAL)
        return;
      
    decayTimes = (eventTime - lastDecayTime) / CALCULATE_INTERVAL;
    lastDecayTime = eventTime;
    if (logclass & LC_FAIR) {
        ls_syslog(LOG_DEBUG1, "%s: Need to decay history CPU time, decayTime=%d, cluster wide decayRate=%f", fname, decayTimes, clsDecay);
    }

    /*Go through each fairshare tree*/
    ent = h_firstEnt_(&policies, &stab);
    while(ent) {
        policy = (struct fairsharePolicy *)ent->hData;
        if (policy->qPtr->fsFactors.histHours == 0.0 || 
            (policy->qPtr->fsFactors.histHours < 0.0 && histHours == 0.0)) {
            if (logclass & LC_FAIR) {
                ls_syslog(LOG_DEBUG1, "%s: HIST_HOURS is 0, we needn't decay history time account for fairshare queue <%s>",
                                fname, policy->name);
            }
            ent = h_nextEnt_(&stab);
            continue;
        }

        traverseUpd4Decay(policy->root, decayTimes);
        if (logclass & LC_FAIR) {
            dumpFSPolicy(policy, fname, LOG_DEBUG1);
        }
        ent = h_nextEnt_(&stab);
    }
} /*updAllSAcctForDecay*/

/******************************************************************************************
 * Description:
 * helper function of updAllSAcctForDecay(). Decay fairshare tree's cputime recursively.
 * The logic will decay tree's cputime level by level.
 *
 * Params:
 * sAcct[in]: the shareAcct which we handle now. At the same time , We also decay the
 *            siblings backward this shareAcct.
 * decayTimes[in]: rate of decay
 *
 ******************************************************************************************
 */
static void traverseUpd4Decay(struct shareAcct * sAcct, int decayTimes) {
    struct shareAcct  *curAcct, *nextAcct;           
    LIST_T            *siblingList;

    if (sAcct->parent == NULL) { 
        /*This is root, silbing is only myself*/
        SACCT_DECAYTIMES_UPDATE(decayTimes, sAcct, "traverseUpd4Decay()");
        /*handle children level*/
        if(!sAcct->sharePolicy || !sAcct->sharePolicy->sAcctList) {                                                                                                                                                     
            return;
        }
        traverseUpd4Decay((struct shareAcct *)sAcct->sharePolicy->sAcctList->forw, decayTimes);
    } else {
        /*go through siblings to reset flag*/
        siblingList = sAcct->parent->sharePolicy->sAcctList;
        for (curAcct = sAcct;
             (LIST_ENTRY_T *)curAcct != (LIST_ENTRY_T *)siblingList; 
             curAcct = curAcct->forw) {                                                                                                                           
            curAcct->flags &= ~FS_SKIPABLE;                                                                                                                                                                             
        }
        /*decay sibling's cputime*/ 
        curAcct = sAcct;
        while((LIST_ENTRY_T *)curAcct != (LIST_ENTRY_T *)siblingList) {
            /*First cache the next node*/  
            nextAcct = curAcct->forw;                                                                                                                                                                                   
    
            /*Check whether the node has been decayed*/
            if (curAcct->flags & FS_SKIPABLE) {
                curAcct = nextAcct;            
                continue;         
            }
    
            /*Decay the history*/
            SACCT_DECAYTIMES_UPDATE(decayTimes, curAcct, "traverseUpd4Decay()");
            curAcct->flags |= FS_SKIPABLE; 
            curAcct->priority = calcDynamicPriority(curAcct);                                                                                                                                                           
    
            /*adjust the pos of curAcct*/  
            adjustOrderByPriority(curAcct);
            curAcct = nextAcct;
        }
        
        /*handle the children level*/
        for (curAcct = (struct shareAcct *)siblingList->forw;
            (LIST_ENTRY_T *)curAcct != (LIST_ENTRY_T *)siblingList; curAcct = curAcct->forw) {   
            if(!curAcct->sharePolicy || !curAcct->sharePolicy->sAcctList) { /*leaf node already*/                                                                                                                                                  
                continue;
            }                                                                                                                                        
            traverseUpd4Decay((struct shareAcct *)curAcct->sharePolicy->sAcctList->forw, decayTimes);                                                                                                                                                                     
        }        
    }
} /*traverseUpd4Decay*/

/* $Id: mbd.jgrp.c 397 2007-11-26 19:04:00Z mblack $
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

#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mbd.h"

#define NL_SETN		10

#include <dirent.h>
#include <malloc.h>

#define MAX_SPEC_LEN  200
#define SET_STATUS(x, y) ((x) == (y)) ? 0 : ((x) = (y)) == (y)

struct jgTreeNode *groupRoot;

struct jgrpInfo {
    struct nodeList  *jgrpList;
    int    numNodes;
    int    arraySize;
    char   jobName[MAX_CMD_DESC_LEN];
    struct idxList *idxList;
    char   allqueues;
    char   allusers;
    char   allhosts;
    struct gData *uGrp;
};

static void       printCounts(struct jgTreeNode *, FILE *);
static int        printNode(struct jgTreeNode *, FILE *);
static void       freeTreeNode(struct jgTreeNode *);
static int        skipJgrpByReq (int, int);
static int        storeToJgrpList(void *, struct jgrpInfo *, int);
static int        makeTreeNodeList(struct jgTreeNode *, struct jobInfoReq *,
                                        struct jgrpInfo *);
static void       treeObserverEvalDep(TREE_OBSERVER_T *, void *, enum treeEventType);
static TREE_OBSERVER_T * treeObserverCreate(char *, void *, TREE_EVENT_OP_T);
static void       treeObserverInvoke(void *, enum treeEventType);
static void       freeGrpNode(struct jgrpData *);
static int isSelected ( struct jobInfoReq *,
                        struct jData *,
                        struct jgrpInfo *
                        );
static int makeJgrpInfo(struct jgTreeNode *, char *, struct jobInfoReq *, struct jgrpInfo *);
extern int ensureJobListCapacity(void ***joblist, int numJobs, int *arraysize, int type);
char treeFile[48];
LIST_T *treeObserverList;
TREE_OBSERVER_T *treeObserverDep;

extern float version;

#define DEFAULT_LISTSIZE    200

void
treeInit()
{
    static char fname[] = "treeInit";
    int mbdPid = getpid();

    groupRoot = treeNewNode(JGRP_NODE_GROUP);
    groupRoot->name = safeSave("/");
    JGRP_DATA(groupRoot)->userId  = managerId;
    JGRP_DATA(groupRoot)->userName = safeSave(lsbSys);
    JGRP_DATA(groupRoot)->status = JGRP_ACTIVE;
    JGRP_DATA(groupRoot)->changeTime = INFINIT_INT;
    JGRP_DATA(groupRoot)->numRef = 0;
    sprintf(treeFile, "/tmp/jgrpTree.%d", mbdPid);



    treeObserverList = listCreate("tree observer");
    if (treeObserverList == NULL) {
	ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6400,
	    "%s: failed to create tree observer list: %s"), /* catgets 6400 */
	    fname, listStrError(listerrno));
	mbdDie(MASTER_MEM);
    }

    treeObserverDep = treeObserverCreate("tree eval dependence",
                                         (void *)groupRoot,
                                         treeObserverEvalDep);
    if (treeObserverDep == NULL)
	mbdDie(MASTER_MEM);

    listInsertEntryAtBack(treeObserverList, (LIST_ENTRY_T *)treeObserverDep);

}

static TREE_OBSERVER_T *
treeObserverCreate(char *name, void *entry, TREE_EVENT_OP_T eventOp)
{
    static char                     fname[] = "treeObserverCreate";
    struct treeObserver             *observer;

     observer = (TREE_OBSERVER_T *)calloc(1, sizeof(TREE_OBSERVER_T));
     if (observer == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        lsberrno = LIST_ERR_NOMEM;
        goto Fail;
    }

    observer->name = safeSave(name);
    observer->eventOp = eventOp;
    observer->entry = entry;
    return (observer);

  Fail:
    FREEUP(observer);
    return (TREE_OBSERVER_T *)NULL;
}

static void
treeObserverEvalDep(TREE_OBSERVER_T * obs, void *operEntry,
                    enum treeEventType eventType)
{
    struct jgTreeNode *operand;

    operand = (struct jgTreeNode *)operEntry;
    switch (eventType) {
        case TREE_EVENT_ADD:
            break;
        case TREE_EVENT_CLIP:
            if (treeObserverDep->entry == operand ||
                isAncestor(operand, treeObserverDep->entry))
                obs->entry = (void *)treeNextSib(operand);
            break;
        case TREE_EVENT_CTRL:
            if (treeObserverDep->entry == operand ||
                isAncestor(operand, treeObserverDep->entry))
                obs->entry = (void *) operand;
            break;
        case TREE_EVENT_NULL:
            break;
    }
    if (logclass & (LC_JGRP))
       ls_syslog(LOG_DEBUG1, "treeObserverEvalDep: obs->entry = <%s/%s>",
                 jgrpNodeParentPath((struct jgTreeNode *)obs->entry), ((struct jgTreeNode *)obs->entry)->name);
    return;
}

static void
treeObserverInvoke(void *operand, enum treeEventType eventType)
{
    LIST_ITERATOR_T     iter;
    LIST_ENTRY_T        *ent;

    LIST_ITERATOR_ZERO_OUT(&iter);
    listIteratorAttach(&iter, treeObserverList);

    for (ent = listIteratorGetCurEntry(&iter);
         ent != NULL;
         listIteratorNext(&iter, &ent))
    {
        (((TREE_OBSERVER_T *)ent)->eventOp)((TREE_OBSERVER_T *)ent,
         operand, eventType);
    }
    return;
}


struct jgTreeNode *
treeLexNext(struct jgTreeNode *node)
{
    struct jgTreeNode *parent;

    if (node == NULL)
        return(NULL);
    if (node->child)
        return(node->child) ;
    if (node->right)
        return(node->right);
    parent = node->parent ;
    while (parent) {
        if (parent->right)
            return(parent->right) ;
        parent = parent->parent;
   }
  return(NULL) ;
}

struct jgTreeNode *
treeNextSib(struct jgTreeNode *node)
{
    if (node == NULL)
        return(NULL);
    if (node->right)
        return(node->right);
    while (node->parent) {
        node = node->parent;
        if (node->right)
            return(node->right);
    }
    return(NULL);
}


struct jgTreeNode *
treeInsertChild(struct jgTreeNode *parent, struct jgTreeNode *child)
{
    struct jgTreeNode *nPtr;

    if (!parent)
        return(child);
    if (!child)
        return(parent);

    if (parent->child) {
        parent->child = treeLinkSibling(parent->child, child);
    } else
        parent->child = child;

    nPtr=child;
    do {
        nPtr->parent=parent;
        nPtr=nPtr->right;
       }
    while (nPtr != NULL);

    if (child->nodeType == JGRP_NODE_JOB)
        updJgrpCountByJStatus(JOB_DATA(child), JOB_STAT_NULL,
                              JOB_DATA(child)->jStatus);
    else
        updJgrpCountByOp(child, 1);


    treeObserverInvoke((void *)child, TREE_EVENT_ADD);
    return(parent);
}

struct jgTreeNode *
treeLinkSibling(struct jgTreeNode *sib1, struct jgTreeNode *sib2)
{
    struct jgTreeNode *nPtr;

    if (!sib1)
        return(sib2);
    if (!sib2)
        return(sib1);
    for(nPtr=sib1; nPtr->right; nPtr=nPtr->right);
    nPtr->right=sib2;
    sib2->left=nPtr;
    return(sib1);
}


void
treeInsertLeft(struct jgTreeNode *node, struct jgTreeNode *subtree)
{
    if (!node || !subtree)
        return;
    subtree->right = node;
    subtree->left = node->left;
    subtree->parent = node->parent;
    if (node->left)
        node->left->right = subtree;
    node->left = subtree;


    if (subtree->nodeType == JGRP_NODE_JOB)
        updJgrpCountByJStatus(JOB_DATA(subtree), JOB_STAT_NULL,
                              JOB_DATA(subtree)->jStatus);
    else
        updJgrpCountByOp(subtree, 1);


    treeObserverInvoke((void *)subtree, TREE_EVENT_ADD);

}

void
treeInsertRight(struct jgTreeNode *node, struct jgTreeNode *subtree)
{
    if (!node || !subtree)
        return;

    subtree->right = node->right;
    subtree->left  = node;
    subtree->parent = node->parent;
    if (node->right)
        node->right->left = subtree;
    node->right = subtree;


    if (subtree->nodeType == JGRP_NODE_JOB)
        updJgrpCountByJStatus(JOB_DATA(subtree), JOB_STAT_NULL,
                              JOB_DATA(subtree)->jStatus);
    else
        updJgrpCountByOp(subtree, 1);


    treeObserverInvoke((void *)subtree, TREE_EVENT_ADD);

}

struct jgTreeNode *
treeClip(struct jgTreeNode *node)
{
    if ( !node )
        return(NULL);
    if (node->left)
        node->left->right = node->right;
    else if (node->parent)
        node->parent->child = node->right;
    if (node->right)
        node->right->left = node->left;


    if (node->nodeType == JGRP_NODE_JOB)
        updJgrpCountByJStatus(JOB_DATA(node), JOB_DATA(node)->jStatus,
                              JOB_STAT_NULL);
    else
        updJgrpCountByOp(node, -1);

    treeObserverInvoke((void *)node, TREE_EVENT_CLIP);
    node->parent = node->left = node->right = NULL;

    return(node);
}


struct jgTreeNode *
treeNewNode(int type)
{
    struct jgTreeNode *node;

    node = (struct jgTreeNode *) my_malloc(sizeof(struct jgTreeNode), "treeNewNode");
    initObj((char *)node, sizeof(struct jgTreeNode));
    node->nodeType = type;
    if (type == JGRP_NODE_GROUP) {

        node->ndInfo = (void *) my_malloc(sizeof (struct jgrpData),
                    "treeNewNode");
        initObj((char *)JGRP_DATA(node), sizeof (struct jgrpData));
        JGRP_DATA(node)->freeJgArray = (FREE_JGARRAY_FUNC_T) freeGrpNode;
        JGRP_DATA(node)->status = JGRP_UNDEFINED;
    }
    else if (type == JGRP_NODE_ARRAY) {
        node->ndInfo = (void *) my_malloc(sizeof(struct jarray),
                    "treeNewNode");
        initObj((char *)ARRAY_DATA(node), sizeof (struct jarray));
        ARRAY_DATA(node)->freeJgArray = (FREE_JGARRAY_FUNC_T) freeJarray;
    }
    return(node);
}

void
treeFree(struct jgTreeNode * node)
{
    struct jgTreeNode *ndPtr, *ndTmp ;

    ndPtr = node;
    while (ndPtr) {
        ndTmp = ndPtr;
        ndPtr = RIGHT_SIBLING(ndPtr);
        if (FIRST_CHILD(ndTmp)) {
           treeFree(FIRST_CHILD(ndTmp));
           freeTreeNode(ndTmp);
        }
        else
           freeTreeNode(ndTmp);
    }
}


static void
freeTreeNode(struct jgTreeNode *node)
{
    if (!node)
	return;
    if (node->name)
        FREEUP(node->name);
    if (node->nodeType == JGRP_NODE_JOB) {
        DESTROY_REF(node->ndInfo, destroyjDataRef);
    }
    else if (node->nodeType == JGRP_NODE_ARRAY) {
        freeJarray(ARRAY_DATA(node));
    }
    else
        freeGrpNode(JGRP_DATA(node));
    FREEUP(node);
}


static void
freeGrpNode(struct jgrpData *jgrpData)
{
    if (!jgrpData)
	return;
    if (jgrpData->userName)
        FREEUP(jgrpData->userName);

    if (jgrpData->numRef <= 0) {
        FREEUP(jgrpData);
    }
    else
        jgrpData->status = JGRP_VOID;
}


void
freeJarray(struct jarray *jarray)
{

    if (!jarray)
	return;
    if (jarray->userName)
        FREEUP(jarray->userName);
    DESTROY_REF(jarray->jobArray, destroyjDataRef);
    jarray->jobArray = NULL;
    if (jarray->numRef <= 0) {
        FREEUP(jarray);
    }
    else
        jarray->status = JGRP_VOID;
}


int
isAncestor(struct jgTreeNode *x, struct jgTreeNode *y)
{
    struct jgTreeNode *ndPtr;

    if (!x || !y)
        return(FALSE);

    for (ndPtr = y->parent; ndPtr; ndPtr = ndPtr->parent)
        if (ndPtr == x)
           return(TRUE);

    return(FALSE);
}

int
isChild(struct jgTreeNode *x, struct jgTreeNode *y)
{
    struct jgTreeNode *ndPtr;

    if (!x || !y)
        return(FALSE);

    for (ndPtr = FIRST_CHILD(x); ndPtr; ndPtr = RIGHT_SIBLING(ndPtr))
        if (ndPtr == y)
           return(TRUE);

    return(FALSE);
}

struct jgTreeNode *
findTreeNode(struct jgTreeNode *node, char *name)
{
    struct jgTreeNode  *nPtr;

    if (!node)
        return(NULL);

    for (nPtr = node; nPtr;   nPtr = nPtr->right) {
        if (strcmp(name, nPtr->name) == 0)
            return(nPtr);
    }
    return(NULL);
}

void
initObj(char *obj, int len)
{
    char *ptr = obj;
    int i;

    for (i = 0; i <  len; i++)
         *ptr++ = 0 ;
}



char *
parentGroup(char * group_spec)
{
    static char parentStr[MAXPATHLEN];
    int    i;

    parentStr[0] = '\0';
    if (!group_spec){
      lsberrno = LSBE_JGRP_BAD;
      return(parentStr);
    }

    if (strlen(group_spec) >= MAXPATHLEN) {
        lsberrno = LSBE_NO_MEM;
        return(parentStr);
    }

    strcpy(parentStr, group_spec);


    for (i = strlen(parentStr)-1; (i >= 0) && (parentStr[i] == '/'); i--);

    if (i < 0)  {
      lsberrno = LSBE_JGRP_NULL;
      parentStr[0] = '\0';
      return(parentStr);
    }


    for (; (i >= 0) && (parentStr[i] != '/'); i--);

    parentStr[i+1] = '\0';
    return(parentStr);
}

char *
parentOfJob(char * group_spec)
{
    static char parentStr[MAXPATHLEN];
    int    i;

    if (!group_spec){
      lsberrno = LSBE_JGRP_NULL;
      return(NULL);
    }

    if (strlen(group_spec) >= MAXPATHLEN) {
        lsberrno = LSBE_NO_MEM;
        return(NULL);
    }

    strcpy(parentStr, group_spec);


    for (i = strlen(parentStr)-1; (i >= 0) && (parentStr[i] != '/'); i--);

    parentStr[i+1] = '\0';
    return(parentStr);
}


char *
myName(char * group_spec)
{
    static char myStr[MAXPATHLEN];
    int    i;

    myStr[0] = '\0';
    if (!group_spec){
      lsberrno = LSBE_JGRP_BAD;
      return(myStr);
    }

    if (strlen(group_spec) >= MAXPATHLEN) {
        lsberrno = LSBE_NO_MEM;
        return(myStr);
    }

    strcpy(myStr, group_spec);


    for (i = strlen(myStr)-1; (i >= 0) && (myStr[i] == '/'); i--);

    myStr[i+1] = '\0';
    if (i < 0)  {
      lsberrno = LSBE_JGRP_BAD;
      return(myStr);
    }


    for (; (i >= 0) && (myStr[i] != '/'); i--);

    return(&myStr[i+1]);
}


char *
jgrpNodeParentPath(struct jgTreeNode * jgrpNode)
{
    static __thread char fullPath[MAXPATHLEN];
    static __thread char oldPath[MAXPATHLEN];
    struct jgTreeNode * jgrpPtr;
    int    first = TRUE;

    if (jgrpNode == NULL) {
      lsberrno = LSBE_JGRP_NULL;
      return(NULL);
    }

    jgrpPtr = jgrpNode->parent;

    fullPath[0] = '\0';

    while (jgrpPtr) {
	strcpy(oldPath, fullPath);

            if (!strcmp(jgrpPtr->name,"/"))
	        sprintf(fullPath,"/%s", oldPath);
	    else {
		if (first == TRUE) {
	            sprintf(fullPath,"%s", jgrpPtr->name);
		    first = FALSE;
		}
		else
	            sprintf(fullPath,"%s/%s", jgrpPtr->name, oldPath);

	    }
	jgrpPtr = jgrpPtr->parent;
    }

    return(fullPath);
}

int
jgrpNodeParentPath_r(struct jgTreeNode * jgrpNode, char *fullPath)
{
    char oldPath[MAXPATHLEN];
    struct jgTreeNode * jgrpPtr;
    int    first = TRUE;

    if (jgrpNode == NULL || fullPath == NULL) {
      lsberrno = LSBE_JGRP_NULL;
      return -1;
    }

    jgrpPtr = jgrpNode->parent;

    fullPath[0] = '\0';

    while (jgrpPtr) {
	strcpy(oldPath, fullPath);

            if (!strcmp(jgrpPtr->name,"/"))
	        sprintf(fullPath,"/%s", oldPath);
	    else {
		if (first == TRUE) {
	            sprintf(fullPath,"%s", jgrpPtr->name);
		    first = FALSE;
		}
		else
	            sprintf(fullPath,"%s/%s", jgrpPtr->name, oldPath);

	    }
	jgrpPtr = jgrpPtr->parent;
    }
    return(0);
}


void
printSubtree(struct jgTreeNode *root, FILE *out_file, int indent)
{

    int    len, i;
    char   space[MAX_SPEC_LEN];
    struct jgTreeNode *gPtr;

    if (!root)
        return;
    len = printNode(root, out_file);
    space[0]='\0' ;
    for (i = 1; i <= (indent+len); i++)
        strcat(space, " ") ;

    for (gPtr = root->child; gPtr; gPtr = gPtr->right) {
        printSubtree(gPtr, out_file, indent+len);
        if (gPtr->right)
            fprintf(out_file, "\n%s", space);
    }
}

void
printTreeStruct(char *fileName)
{
    static char fname[] = "printTreeStruct";
    FILE   *out_file , *fopen() ;

    if ((out_file = fopen(fileName, "w")) == NULL) {
	ls_syslog(LOG_ERR, "%s: can't open file %s: %m", fname, fileName);
	return;
    }
    fprintf(out_file, "***********************************\n");
    fprintf(out_file, "*        Job Group Tree           *\n");
    fprintf(out_file, "***********************************\n");
    printSubtree(groupRoot, out_file, 0);
    fprintf(out_file, "\n");
    printCounts(groupRoot, out_file);
    FCLOSEUP(&out_file);
}

static int
printNode(struct jgTreeNode *root, FILE *out_file)
{
    char ss[MAX_SPEC_LEN];
    if (root->nodeType == JGRP_NODE_GROUP) {
        sprintf(ss, "%s(Root)", root->name);
    }
    else if (root->nodeType == JGRP_NODE_JOB) {
        sprintf(ss, "%s(J)", root->name);
    }
    else if ( root->nodeType == JGRP_NODE_ARRAY) {
        sprintf(ss, "%s(V)", root->name);
    }
    else
         sprintf(ss, "<%lx>UNDEF:", (long)root);
    fprintf(out_file, "%s", ss) ;
    if (root->child!=0){
        fprintf(out_file, "->") ;
        return(strlen(ss)+2);
    }
    else
        return(strlen(ss)) ;
}

static void
printCounts(struct jgTreeNode *root, FILE *out_file)
{
    int i;

    fprintf(out_file, "************** COUNTS ******************\n");
    fprintf(out_file, "\n");
    while (root) {
        if (root->nodeType == JGRP_NODE_GROUP) {
            fprintf(out_file, "GROUP:%s\n", root->name);
            for (i=0; i < NUM_JGRP_COUNTERS; i++)
                 fprintf(out_file, "%d   ", JGRP_DATA(root)->counts[i]);
            fprintf(out_file, "\n");

        }
        else if (root->nodeType == JGRP_NODE_ARRAY) {
	    fprintf(out_file, "ARRAY:%s\n", root->name);
            for (i=0; i < NUM_JGRP_COUNTERS; i++)
                 fprintf(out_file, "%d   ", ARRAY_DATA(root)->counts[i]);
            fprintf(out_file, "\n");
        }

        root = treeLexNext(root);
    }
}


void
putOntoTree(struct jData *jp, int jobType)
{
    static struct jgTreeNode *newj;
    struct jgTreeNode  *parentNode;
    struct jData    *jPtr;



    parentNode = groupRoot;
    newj = treeNewNode(jp->nodeType);
    if (jp->shared->jobBill.options & SUB_JOB_NAME)
        newj->name = safeSave(jp->shared->jobBill.jobName);
    else
        newj->name = safeSave(jp->shared->jobBill.command);

    if (jp->nodeType == JGRP_NODE_JOB)
        newj->ndInfo = (void *)createjDataRef(jp);
    else {
        ARRAY_DATA(newj)->jobArray = createjDataRef(jp);
        ARRAY_DATA(newj)->userId = jp->userId;
        ARRAY_DATA(newj)->userName = safeSave (jp->userName);

	if (jp->shared->jobBill.options2 & SUB2_HOST_NT)
	    ARRAY_DATA(newj)->fromPlatform = AUTH_HOST_NT;
	else if (jp->shared->jobBill.options2 & SUB2_HOST_UX)
	    ARRAY_DATA(newj)->fromPlatform = AUTH_HOST_UX;
    }

    jp->jgrpNode = newj;

    for (jPtr = jp->nextJob; jPtr; jPtr = jPtr->nextJob) {
         jPtr->jgrpNode = newj;

         updJgrpCountByJStatus(jPtr, JOB_STAT_NULL, jPtr->jStatus);
    }

    treeInsertChild(parentNode, newj);

    jp->runCount = 1;



    if (jp->newReason == 0)
        jp->newReason = PEND_JOB_NEW;
    if (jp->shared->dptRoot == NULL) {
        jp->jFlags |= JFLAG_READY2;
    }
    if (logclass & LC_JGRP)
        printTreeStruct(treeFile);
}


struct jgArrayBase *
createJgArrayBaseRef(struct jgArrayBase *jgArrayBase)
{
    if (jgArrayBase) {
        jgArrayBase->numRef++;
    }
    return jgArrayBase;
}

void
destroyJgArrayBaseRef(struct jgArrayBase *jgArrayBase)
{
    if (jgArrayBase) {
        jgArrayBase->numRef--;
        if ((jgArrayBase->status == JGRP_VOID) && (jgArrayBase->numRef <= 0)) {

           (* jgArrayBase->freeJgArray)((char *)jgArrayBase);
        }
    }
    return;
}

void
checkJgrpDep(void)
{
    static char  fname[] = "checkJgrpDep";
    struct jgTreeNode *nPtr;
    int    iterNum = 0;
    time_t   entryTime;
    int    change = TRUE;

    if (treeObserverDep->entry)
        nPtr = (struct jgTreeNode *)treeObserverDep->entry;
    else {
        nPtr = groupRoot;
        treeObserverDep->entry = (void *)groupRoot;
    }

    if (logclass & LC_SCHED)
        ls_syslog(LOG_DEBUG1, "%s: (Re-)entering checkJgrpDep at node <%s%s>;", fname, jgrpNodeParentPath(nPtr), nPtr->name);

    entryTime = time(0);


Entry:
    if (! change)
        goto Exit;
    change = FALSE;
    while(nPtr) {
        switch (nPtr->nodeType) {
            case JGRP_NODE_GROUP:
               nPtr = treeLexNext(nPtr);
               break;
            case JGRP_NODE_JOB:
            case JGRP_NODE_ARRAY: {
               struct jData * jpbw;

               if (nPtr->nodeType == JGRP_NODE_JOB)
                   jpbw = JOB_DATA(nPtr);
               else
                   jpbw = ARRAY_DATA(nPtr)->jobArray->nextJob;

               for (; jpbw; jpbw = jpbw->nextJob) {
                   iterNum ++;
                   if (!JOB_PEND(jpbw)) {
                       continue;
                   }

                   jpbw->jFlags &= ~(JFLAG_READY1 | JFLAG_READY2);
                   jpbw->jFlags |= JFLAG_READY1;


		   if (jpbw->jFlags & JFLAG_WAIT_SWITCH) {
		       jpbw->newReason = PEND_JOB_SWITCH;
		       continue;
		   }

                   if (!jpbw->shared->dptRoot) {
                       jpbw->jFlags |= JFLAG_READY2;
		   }
                   else {
                       int depCond;
                       depCond=evalDepCond(jpbw->shared->dptRoot, jpbw);
                       if (depCond == DP_FALSE) {
                           jpbw->newReason = PEND_JOB_DEPEND;
                       }
                       else if (depCond == DP_INVALID) {

                           jpbw->newReason = PEND_JOB_DEP_INVALID;
                           jpbw->jFlags |= JFLAG_DEPCOND_INVALID;
                       }
                       else if (depCond == DP_REJECT) {
                           jpbw->newReason = PEND_JOB_DEP_REJECT;
                           jpbw->jFlags |= JFLAG_DEPCOND_REJECT;
                       }
                       if (depCond == DP_TRUE) {
                           jpbw->jFlags |= JFLAG_READY2;
                           continue;
                       }
                   }
               }
               nPtr = treeLexNext(nPtr);
               break;
            }
            default:
               nPtr = treeLexNext(nPtr);
               break;
        }
        iterNum ++;

        if (nPtr && (iterNum > 1000) && (time(0) - entryTime > 3))  {
            if (logclass & LC_SCHED)
                ls_syslog(LOG_DEBUG1, "%s: Stayed too long; leave at node <%s%s>;", fname, jgrpNodeParentPath(nPtr), nPtr->name);
            goto Exit;
        }
    }
    if (!nPtr)
       nPtr = groupRoot;
    goto Entry;

    if (logclass & LC_JGRP)
        printTreeStruct(treeFile);
Exit: if (nPtr)
        treeObserverDep->entry = (void *)nPtr;
    else
        treeObserverDep->entry = (void *)groupRoot;
}



void
updJgrpCountByJStatus(struct jData *job, int oldStatus, int newStatus)
{
    struct jgTreeNode *gPtr = job->jgrpNode;

    while (gPtr) {
        if (gPtr->nodeType == JGRP_NODE_GROUP) {
            if (oldStatus != JOB_STAT_NULL) {
                JGRP_DATA(gPtr)->counts[getIndexOfJStatus(oldStatus)] -= 1;
                JGRP_DATA(gPtr)->counts[JGRP_COUNT_NJOBS] -= 1;
            }
            if (newStatus != JOB_STAT_NULL) {
                JGRP_DATA(gPtr)->counts[getIndexOfJStatus(newStatus)] += 1;
                JGRP_DATA(gPtr)->counts[JGRP_COUNT_NJOBS] += 1;
            }
        }
        else if (gPtr->nodeType == JGRP_NODE_ARRAY) {
            if (oldStatus != JOB_STAT_NULL) {
                ARRAY_DATA(gPtr)->counts[getIndexOfJStatus(oldStatus)] -= 1;
                ARRAY_DATA(gPtr)->counts[JGRP_COUNT_NJOBS] -= 1;
            }
            if (newStatus != JOB_STAT_NULL) {
		ARRAY_DATA(gPtr)->counts[getIndexOfJStatus(newStatus)] += 1;
                ARRAY_DATA(gPtr)->counts[JGRP_COUNT_NJOBS] += 1;
            }
        }
        gPtr = gPtr->parent;
    }
    if (logclass & LC_JGRP)
        printTreeStruct(treeFile);
}


int
getIndexOfJStatus(int status)
{
    static char fname[] = "getIndexOfJStatus()";

    switch (MASK_STATUS(status & ~JOB_STAT_UNKWN
			& ~JOB_STAT_PDONE & ~JOB_STAT_PERR)) {
        case JOB_STAT_PEND:
        case JOB_STAT_RUN|JOB_STAT_WAIT:
            return(JGRP_COUNT_PEND);
        case JOB_STAT_PSUSP:
            return(JGRP_COUNT_NPSUSP);
        case JOB_STAT_RUN :
            return(JGRP_COUNT_NRUN);
        case JOB_STAT_SSUSP:
            return(JGRP_COUNT_NSSUSP);
        case JOB_STAT_USUSP:
            return(JGRP_COUNT_NUSUSP);
        case JOB_STAT_EXIT:
        case JOB_STAT_EXIT|JOB_STAT_WAIT:
            return(JGRP_COUNT_NEXIT);
        case JOB_STAT_DONE:
        case JOB_STAT_DONE|JOB_STAT_WAIT:
            return(JGRP_COUNT_NDONE);
        default:

            ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6403,
		"%s: job status <%d> out of bound"), /* catgets 6403 */
		fname, MASK_STATUS(status));
            return(8);
    }
}


void
updJgrpCountByOp(struct jgTreeNode *jgrp, int factor)
{
    struct jgTreeNode *parent;
    int i;

    for (parent = jgrp->parent; parent; parent = parent->parent) {
        for (i=0; i < NUM_JGRP_COUNTERS; i++)
            if (jgrp->nodeType == JGRP_NODE_GROUP) {
                JGRP_DATA(parent)->counts[i] += factor*JGRP_DATA(jgrp)->counts[i];
            }
            else if (jgrp->nodeType == JGRP_NODE_ARRAY)
                JGRP_DATA(parent)->counts[i] += factor*ARRAY_DATA(jgrp)->counts[i];
    }
    return;
}


void
resetJgrpCount(void)
{
    struct jgTreeNode *jgrp;
    struct jData      *jPtr;
    int    i;

    for (jgrp = groupRoot; jgrp; jgrp = treeLexNext(jgrp)) {
        if (jgrp->nodeType == JGRP_NODE_GROUP) {
            for (i=0; i < NUM_JGRP_COUNTERS; i++)
                JGRP_DATA(jgrp)->counts[i] = 0;
        }
        else if (jgrp->nodeType == JGRP_NODE_ARRAY) {
            for (jPtr = ARRAY_DATA(jgrp)->jobArray->nextJob; jPtr;
                 jPtr = jPtr->nextJob)
                updJgrpCountByJStatus(jPtr, JOB_STAT_NULL, jPtr->jStatus);
        }
        else if (jgrp->nodeType == JGRP_NODE_JOB)
            updJgrpCountByJStatus(JOB_DATA(jgrp), JOB_STAT_NULL,
                                  JOB_DATA(jgrp)->jStatus);
    }
    return;
}

bool_t
isSameUser(struct lsfAuth *auth, int userId, char *userName, int fromPlatform)
{

   return(strcmp(auth->lsfUserName, userName) == 0);
}

int
jgrpPermitOk(struct lsfAuth *auth, struct jgTreeNode *jgrp)
{
    struct jgTreeNode *nPtr;

    if (!jgrp)
       return(FALSE);

    if (mSchedStage == M_STAGE_REPLAY)
        return(TRUE);

    if (auth->uid == 0 || isAuthManager(auth)) {

	return TRUE;
    }

    for (nPtr = jgrp; nPtr; nPtr = nPtr->parent) {
        if (nPtr->nodeType == JGRP_NODE_GROUP) {
	    if(isSameUser(auth, JGRP_DATA(nPtr)->userId,
			JGRP_DATA(nPtr)->userName,
			JGRP_DATA(nPtr)->fromPlatform))
	  	return TRUE;
        }
        if (nPtr->nodeType == JGRP_NODE_ARRAY) {
            if(isSameUser(auth, ARRAY_DATA(nPtr)->userId,
			ARRAY_DATA(nPtr)->userName,
			ARRAY_DATA(nPtr)->fromPlatform))
		return TRUE;
        }
        if (nPtr->nodeType == JGRP_NODE_JOB) {
	    if (JOB_DATA(nPtr)->shared->jobBill.options2
		& SUB2_HOST_NT){
	        if(isSameUser(auth, JOB_DATA(nPtr)->userId,
		       JOB_DATA(nPtr)->userName,
		       AUTH_HOST_NT))
		    return TRUE;
            } else if (JOB_DATA(nPtr)->shared->jobBill.options2
		       & SUB2_HOST_UX){
	        if(isSameUser(auth, JOB_DATA(nPtr)->userId,
		       JOB_DATA(nPtr)->userName,
		       AUTH_HOST_UX))
		    return TRUE;
            } else
	        if(isSameUser(auth, JOB_DATA(nPtr)->userId,
				  JOB_DATA(nPtr)->userName,
				  0))
		    return TRUE;
        }
    }
    return(FALSE);
}

bool_t
isJobOwner(struct lsfAuth *auth, struct jData *job)
{

    if (job->shared->jobBill.options2 & SUB2_HOST_UX)
        return (isSameUser(auth, job->userId, job->userName, AUTH_HOST_UX));
    else  if(job->shared->jobBill.options2 & SUB2_HOST_NT)
        return (isSameUser(auth, job->userId, job->userName, AUTH_HOST_NT));
    else
        return (isSameUser(auth, job->userId, job->userName, 0));

}

static int checkJgrpMatch(struct jobInfoReq *jobInfoReq, struct jobMetaData *jobMeta, char allqueues, char allusers, char allhosts, struct gData *uGrp, struct idxList *idxList){
    int i;
    if(jobInfoReq->jobId != 0 && (jobInfoReq->options & JGRP_ARRAY_INFO)){
        if ((LSB_ARRAY_IDX(jobInfoReq->jobId) != 0 && LSB_ARRAY_IDX(jobInfoReq->jobId) != LSB_ARRAY_IDX(jobMeta->jobId)) ||
            LSB_ARRAY_JOBID(jobInfoReq->jobId) != LSB_ARRAY_JOBID(jobMeta->jobId))
            return(FALSE);
    }
    if (skipJgrpByReq (jobInfoReq->options, jobMeta->jStatus))
        return(FALSE);

    if (!allqueues && strcmp(jobMeta->queue, jobInfoReq->queue) != 0)
        return(FALSE);

    if (!allusers && strcmp(jobMeta->userName, jobInfoReq->userName)) {
        if (uGrp == NULL)
            return(FALSE);
        else if (!gMember(jobMeta->userName, uGrp))
            return(FALSE);
    }
    /* Newly submitted jobs have not been dispatched yet, so the hosts field is definitely empty. 
     * Querying with a specified host will inevitably result in a mismatch. 
     */
    if (!allhosts) {
        return FALSE;
    }

    if (idxList) {
        struct idxList *idx;
        for (idx = idxList; idx; idx = idx->next) {
            if (LSB_ARRAY_IDX(jobMeta->jobId) < idx->start ||
                LSB_ARRAY_IDX(jobMeta->jobId) > idx->end)
                continue;
            if (((LSB_ARRAY_IDX(jobMeta->jobId)-idx->start) % idx->step) == 0)
                return(TRUE);
        }
        return(FALSE);
    }
    return(TRUE);
}

int selectJgrpsFromShm(struct jobInfoReq *jobInfoReq, struct jobMetaData ***jobMetaDataList,int *listSize){
    char allqueues, allusers, allhosts;
    int arraysize = 0, tmpIndex = 0, retError = LSBE_NO_ERROR, currentIndex = 0, numJobs = 0, maxJLimit, i;
    struct jobMetaData *job;
    struct jobMetaData **joblist;
    struct gData *uGrp;
    struct idxList * idxList;
    char   jobName[MAX_CMD_DESC_LEN];
    struct jobMetaData *meta;
    char shmJobName[MAXPATHLEN];
    static int currentReaderIndex = -1;
    if(currentReaderIndex == -1){
        pid_t pid = getpid();
        for(i = 0; i < MAX_QMBD_NUMS; i++){
            if(pid == shm->queryReaderInfo[i].pid){
                currentReaderIndex = i;
            }
        }
    }

    if(currentReaderIndex >= 0){
        currentIndex = shm->queryReaderInfo[currentReaderIndex].readStartIndex;
    }else{
        return LSBE_NO_JOB;
    }

    memset(jobName, 0, MAX_CMD_DESC_LEN);
    allqueues = FALSE;
    allusers = FALSE;
    allhosts = FALSE;
    if (jobInfoReq->queue[0] == '\0')
        allqueues = TRUE;

    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        allusers = TRUE;
    else
        uGrp = getUGrpData (jobInfoReq->userName);
    
    if (jobInfoReq->host[0] == '\0')
        allhosts = TRUE;
    if (((strlen(jobInfoReq->jobName) == 1) && (jobInfoReq->jobName[0] == '/')) || (strlen(jobInfoReq->jobName) == 0))
        strcpy(jobName, "*");
    else
        ls_strcat(jobName,sizeof(jobName),jobInfoReq->jobName);
    
    if ((idxList =
          parseJobArrayIndex(jobName, &retError, &maxJLimit))
          == NULL) {
        if (retError != LSBE_NO_ERROR)
            return(retError);
    }

    while(currentIndex != shm->jobMetaQueue->tail){
        job = &shm->jobMetaQueue->jobUnit[currentIndex];
        if(job == NULL || job->state != SHM_BLOCK_STATUS_USED){
            ls_syslog(LOG_ERR, "%s: a job in shm mask deleted, index is %d, block status is %d", __func__, currentIndex, job->state);
        }
        getShmJobName(job, shmJobName);
        if(matchName(jobName, shmJobName)){
            if(jobInfoReq->options & JGRP_ARRAY_INFO){
                if(job->type == JGRP_NODE_ARRAY){
                    if(checkJgrpMatch(jobInfoReq, job, allqueues, allusers, allhosts, uGrp, idxList)){
                        retError = ensureJobListCapacity((void ***)&joblist, numJobs, &arraysize, JOB_TYPE_METADATA);
                        if (retError == LSBE_NO_ERROR) {
                            joblist[numJobs] = job;
                            numJobs++;
                        } else {
                            FREEUP(joblist);
                            return retError;
                        }
                    }
                }
            }else{
                if(job->type == JGRP_NODE_ARRAY){
                    tmpIndex = (currentIndex + 1) % syncShmJobCapacity;
                    while(tmpIndex != job->nextJgrpIndex){
                        meta = &shm->jobMetaQueue->jobUnit[tmpIndex];
                        if(meta == NULL || meta->state != SHM_BLOCK_STATUS_USED){
                            ls_syslog(LOG_WARNING, "%s: a job in shm mask deleted, index is %d, block status is %d", __func__, tmpIndex, meta->state);
                        }
                        tmpIndex = (tmpIndex + 1) % syncShmJobCapacity;
                        if(checkJgrpMatch(jobInfoReq, meta, allqueues, allusers, allhosts, uGrp, idxList)){
                            retError = ensureJobListCapacity((void ***)&joblist, numJobs, &arraysize, JOB_TYPE_METADATA);
                            if (retError == LSBE_NO_ERROR) {
                                joblist[numJobs] = meta;
                                numJobs++;
                            } else {
                                FREEUP(joblist);
                                return retError;
                            }
                        }
                    }
                }else{
                    if(checkJgrpMatch(jobInfoReq, job, allqueues, allusers, allhosts, uGrp, idxList)){
                        retError = ensureJobListCapacity((void ***)&joblist, numJobs, &arraysize, JOB_TYPE_METADATA);
                        if (retError == LSBE_NO_ERROR) {
                            joblist[numJobs] = job;
                            numJobs++;
                        } else {
                            FREEUP(joblist);
                            return retError;
                        }
                    }
                }
            }
        }
        currentIndex = job->nextJgrpIndex;
    }
    if (numJobs > 0) {
        *jobMetaDataList =  joblist;
        *listSize = numJobs;
        return(LSBE_NO_ERROR);
    } else {
        return(LSBE_NO_JOB);
    }
}

int
selectJgrps (struct jobInfoReq *jobInfoReq, void **jgList, int *listSize)
{
    struct jgTreeNode  *parent;
    int retError = 0;

    struct jgrpInfo  jgrp;
    char   jobName[MAX_CMD_DESC_LEN];


    jgrp.allqueues = jgrp.allusers =  jgrp.allhosts = FALSE;
    jgrp.uGrp = NULL;
    jgrp.jgrpList = NULL;
    jgrp.numNodes = 0;
    jgrp.arraySize = 0;
    jobName[0] = '\0';

    *jgList = NULL;
    *listSize = 0;

    memset(jobName, 0, MAX_CMD_DESC_LEN);

    if (jobInfoReq->queue[0] == '\0')
        jgrp.allqueues = TRUE;

    if (strcmp(jobInfoReq->userName, ALL_USERS) == 0)
        jgrp.allusers = TRUE;
    else
        jgrp.uGrp = getUGrpData (jobInfoReq->userName);

    if (jobInfoReq->host[0] == '\0')
        jgrp.allhosts = TRUE;

    if (jobInfoReq->jobId != 0 && (jobInfoReq->options & JGRP_ARRAY_INFO)) {
        struct jData *jp;

        if ((jp = getJobData(jobInfoReq->jobId)) == NULL ||
            jp->nodeType != JGRP_NODE_ARRAY)
            goto ret;

        if (!storeToJgrpList((void *)ARRAY_DATA(jp->jgrpNode)->jobArray,
                        &jgrp, JGRP_NODE_JOB))
            return LSBE_NO_MEM;
        goto ret;
    }


        parent = groupRoot;
        if ((strlen(jobInfoReq->jobName) == 1) && (jobInfoReq->jobName[0] == '/'))
            strcpy(jobName, "*");
	else
	    ls_strcat(jobName,sizeof(jobName),jobInfoReq->jobName);
        if ((retError = makeJgrpInfo(parent, jobName, jobInfoReq, &jgrp))
            != LSBE_NO_ERROR) {
            return (retError);
        }
        goto ret;
ret:
    if (jgrp.numNodes > 0) {
        *jgList =  (void *)jgrp.jgrpList;
        *listSize = jgrp.numNodes;
        return(LSBE_NO_ERROR);
    } else {
        return(LSBE_NO_JOB);
    }
}

static int
makeJgrpInfo(struct jgTreeNode *parent,
             char *jobName,
             struct jobInfoReq *jobInfoReq,
             struct jgrpInfo *jgrp)
{
    int maxJLimit = 0;
    int retError;

    if ((jgrp->idxList =
          parseJobArrayIndex(jobName, &retError, &maxJLimit))
          == NULL) {
        if (retError != LSBE_NO_ERROR)
            return(retError);
    }

    if (!parent)
        return(LSBE_NO_JOB);

    strcpy(jgrp->jobName, jobName);
    if ((retError = makeTreeNodeList(parent, jobInfoReq, jgrp))
                                                        != LSBE_NO_ERROR)
         return(retError);

    return LSBE_NO_ERROR;
}


static int
makeTreeNodeList(struct jgTreeNode *parent,
                 struct jobInfoReq *jobInfoReq,
                 struct jgrpInfo *jgrp)
{
    struct jgTreeNode *nPtr;

    if (!parent)
        return (LSBE_NO_ERROR);


    for (nPtr = parent->child; nPtr; nPtr = nPtr->right){
        if (strlen(jgrp->jobName) && !matchName(jgrp->jobName, nPtr->name))
            continue;
         switch (nPtr->nodeType) {
              case JGRP_NODE_ARRAY:{
                  struct jData *jpbw;
                  if (jobInfoReq->options & JGRP_ARRAY_INFO) {
                     if (isSelected(jobInfoReq, ARRAY_DATA(nPtr)->jobArray,
                                                                jgrp)
                          && (!storeToJgrpList(
                              (void *)ARRAY_DATA(nPtr)->jobArray,
                              jgrp, JGRP_NODE_JOB)))
                          return LSBE_NO_MEM;
                  }
                  else {
                      jpbw = ARRAY_DATA(nPtr)->jobArray->nextJob;
                      for ( ; jpbw; jpbw = jpbw->nextJob) {
                         if (isSelected(jobInfoReq, jpbw, jgrp)) {
                             if (!storeToJgrpList((void *)jpbw, jgrp,
                                                                JGRP_NODE_JOB))
                                 return LSBE_NO_MEM;
                         }
                     }
                  }
                 break;
             }
             case JGRP_NODE_JOB:  {
                 struct jData *jpbw;

                 if (jobInfoReq->options & JGRP_ARRAY_INFO)
                     continue;

                 jpbw = JOB_DATA(nPtr);
                 if (isSelected(jobInfoReq, jpbw, jgrp)) {
                     if (!storeToJgrpList((void *)jpbw, jgrp, JGRP_NODE_JOB))
                         return LSBE_NO_MEM;
                 }
                 break;
             }
         }
    }

    return LSBE_NO_ERROR;
}

static int
skipJgrpByReq (int options, int jStatus)
{
   if (options & (ALL_JOB|JOBID_ONLY_ALL | JOBID_ONLY)) {
       return FALSE;
   }
   else if ((options & (CUR_JOB | LAST_JOB))
                && (IS_START(jStatus) || IS_PEND(jStatus))) {
       return FALSE;
   }
   else if ((options & PEND_JOB) && IS_PEND(jStatus)) {
       return FALSE;
   }
   else if ((options & (SUSP_JOB | RUN_JOB)) && IS_START(jStatus)) {
       return FALSE;
   }
   else if ((options & DONE_JOB) && IS_FINISH(jStatus)) {
       return FALSE;
   }
   return(TRUE);
}

static int
isSelected(struct jobInfoReq *jobInfoReq, struct jData *jpbw,
                struct jgrpInfo *jgrp)
{
    static char fname[] = "isSelected()";
    int i;
    char allqueues = jgrp->allqueues;
    char allusers = jgrp->allusers;
    char allhosts = jgrp->allhosts;

    if (skipJgrpByReq (jobInfoReq->options, jpbw->jStatus))
        return(FALSE);

    if (!allqueues && strcmp(jpbw->qPtr->queue, jobInfoReq->queue) != 0)
        return(FALSE);

    if (!allusers && strcmp(jpbw->userName, jobInfoReq->userName)) {
        if (jgrp->uGrp == NULL)
            return(FALSE);
        else if (!gMember(jpbw->userName, jgrp->uGrp))
            return(FALSE);
    }

    if (!allhosts) {
        struct gData *gp;
        if (IS_PEND (jpbw->jStatus))
            return(FALSE);

        if (jpbw->hPtr == NULL) {
            if (!(jpbw->jStatus & JOB_STAT_EXIT))
                ls_syslog(LOG_ERR, _i18n_msg_get(ls_catd , NL_SETN, 6404,
		    "%s: Execution host for job <%s> is null"), /* catgets 6404 */
		    fname, lsb_jobid2str(jpbw->jobId));
           return(FALSE);
        }
        gp = getHGrpData (jobInfoReq->host);
        if (gp != NULL) {
            for (i = 0; i < jpbw->numHostPtr; i++) {
                if (jpbw->hPtr[i] == NULL)
                    return(FALSE);
                if (gMember(jpbw->hPtr[i]->host, gp))
                    break;
            }
            if (i >= jpbw->numHostPtr)
                return(FALSE);
         }
         else {
             for (i = 0; i < jpbw->numHostPtr; i++) {
                 if (jpbw->hPtr[i] == NULL)
                     return(FALSE);
                if (equalHost_(jobInfoReq->host, jpbw->hPtr[i]->host))
                    break;
            }
            if (i >= jpbw->numHostPtr)
                return(FALSE);
        }
    }

    if (jgrp->idxList) {
        struct idxList *idx;
        for (idx = jgrp->idxList; idx; idx = idx->next) {
            if (LSB_ARRAY_IDX(jpbw->jobId) < idx->start ||
                LSB_ARRAY_IDX(jpbw->jobId) > idx->end)
                continue;
            if (((LSB_ARRAY_IDX(jpbw->jobId)-idx->start) % idx->step) == 0)
                return(TRUE);
        }
        return(FALSE);
    }
    else
    return(TRUE);
}

static int
storeToJgrpList(void *ptr, struct jgrpInfo *jgrp, int type)
{
    if (jgrp->arraySize == 0) {
        jgrp->arraySize = DEFAULT_LISTSIZE;
        jgrp->jgrpList = (struct  nodeList*)
                  calloc (jgrp->arraySize, sizeof (struct  nodeList));
        if (jgrp->jgrpList == NULL)
               return FALSE;
    }
    if (jgrp->numNodes >= jgrp->arraySize) {

        struct  nodeList *biglist;
        jgrp->arraySize *= 2;
        biglist = (struct  nodeList *) realloc((char *)jgrp->jgrpList,
                        jgrp->arraySize * sizeof (struct  nodeList));
        if (biglist == NULL) {
            FREEUP(jgrp->jgrpList);
            jgrp->numNodes = 0;
            return FALSE;
        }
        jgrp->jgrpList = biglist;
    }
    jgrp->jgrpList[jgrp->numNodes].info = ptr;
    if (type == JGRP_NODE_JOB)
        jgrp->jgrpList[jgrp->numNodes++].isJData = TRUE;
    else
        jgrp->jgrpList[jgrp->numNodes++].isJData = FALSE;
    return(TRUE);
}

char *
fullJobName(struct jData *jp)
{
    static char jobName[MAXPATHLEN];

    if (jp->jgrpNode) {
        sprintf(jobName, "%s", jp->jgrpNode->name);
    }
    else
        jobName[0] = '\0';

    return(jobName);
}

void
fullJobName_r(struct jData *jp, char *jobName)
{
    if (jobName == NULL || jp == NULL) {
	return;
    }
    if (jp->jgrpNode) {
        sprintf(jobName, "%s", jp->jgrpNode->name);
    }
    else
        jobName[0] = '\0';

    return;
}


/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * Copyright (C) 2011 David Bigagli
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

#include "../lsf.h"
#include "resreq.h"
#if defined(HAVE_TCL_TCL_H)
#include <tcl/tcl.h>
#endif
#if defined(HAVE_TCL_H)
#include <tcl.h>
#endif
#include "lsftcl.h"
#include "../lib/lproto.h"

static struct tclHostData   *hPtr;
static struct Tcl_Interp    *globinterp;
static char                 overRideFromType;
static char                 runTimeDataQueried;
static int                  numIndx;
static int                  nRes;
static struct tclLsInfo     *myTclLsInfo;
/* Arrays holding symbols used in resource requirement
 * expressions.
 */
static int   *ar;
static int   *ar2;
static int   *ar4;

int numericValue (ClientData, Tcl_Interp *, Tcl_Value *, Tcl_Value *);
int booleanValue (ClientData, Tcl_Interp *, Tcl_Value *, Tcl_Value *);
int stringValue (ClientData, Tcl_Interp *, int, const char **);
static int copyTclLsInfo (struct tclLsInfo *);
static char *getResValue (int);
static int definedCmd(ClientData, Tcl_Interp *, int, const char **);

/* numericValue()
 * Evaluate host or shared resource numerica value.
 */
int
numericValue(ClientData clientData,
             Tcl_Interp *interp,
             Tcl_Value *args,
             Tcl_Value *resultPtr)
{
    int     *indx;
    float   cpuf;
    char    *value;

    indx = clientData;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "numericValue: *indx = %d", *indx);

    cpuf = hPtr->cpuFactor;
    resultPtr->type        = TCL_INT;
    runTimeDataQueried     = TRUE;

    if (*indx < numIndx) {

        resultPtr->type = TCL_DOUBLE;
        if (*indx <= R15M) {
            resultPtr->doubleValue = hPtr->loadIndex[*indx] * cpuf - 1;
        } else {
            resultPtr->doubleValue = hPtr->loadIndex[*indx];
            if (hPtr->loadIndex[*indx] >= (INFINIT_LOAD - 10.0)
                && hPtr->flag !=  TCL_CHECK_SYNTAX) {

                return (TCL_ERROR);
            }
        }

        return TCL_OK;

    }

    if (*indx == CPUFACTOR) {
        runTimeDataQueried = FALSE;
        resultPtr->type = TCL_DOUBLE;
        resultPtr->doubleValue = cpuf;
    } else if (*indx == NDISK) {

        runTimeDataQueried = FALSE;
        resultPtr->intValue    = hPtr->nDisks;

    } else if (*indx == REXPRI) {

        runTimeDataQueried = FALSE;
        resultPtr->intValue    = hPtr->rexPriority;

    } else if (*indx == MAXCPUS_) {
        resultPtr->intValue = hPtr->maxCpus;

    } else if (*indx == MAXMEM) {

        resultPtr->intValue = hPtr->maxMem;

    } else if (*indx == MAXSWAP) {

     resultPtr->intValue = hPtr->maxSwap;

    } else if (*indx == MAXTMP) {

        resultPtr->intValue = hPtr->maxTmp;

    } else if (*indx == SERVER) {

        runTimeDataQueried = FALSE;
        resultPtr->intValue = (hPtr->hostInactivityCount == -1) ? 0 : 1;

    } else if (*indx == SLOTS) {
        resultPtr->intValue = hPtr->slots;
    } else {

        value = getResValue(*indx - myTclLsInfo->numIndx);
        if (value == NULL || !strcmp(value,"-")) {
            resultPtr->intValue = 0;
            return(TCL_OK);
        }

        resultPtr->doubleValue = atof (value) ;
        resultPtr->type = TCL_DOUBLE;

        if (logclass & LC_TRACE)
            ls_syslog(LOG_DEBUG3, "\
numericValue():value = %s, clientData =%d", value, *indx);
    }

    return TCL_OK;
}

/* booleanValue()
 * Evaluate host based on shared resource bool.
 */
int
booleanValue(ClientData clientData,
             Tcl_Interp *interp,
             Tcl_Value *args,
             Tcl_Value *resultPtr)
{
    int    *idx;
    int    isSet;
    char   *value;

    idx = (int *)clientData;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "booleanValue: *idx = %d", *idx);
    if (*idx < 0)
        return(TCL_ERROR);

    overRideFromType = TRUE;

    resultPtr->type = TCL_INT;
    if (hPtr->resBitMaps == NULL) {
        resultPtr->intValue = 0;
        return (TCL_OK);
    }

    /* Is a host based resource.
     */
    TEST_BIT(*idx, hPtr->resBitMaps, isSet);
    if (isSet == 1) {
        resultPtr->intValue = isSet;
        return TCL_OK;
    }

    value = getResValue (*idx);
    if (value == NULL || value[0] == '-') {
        if (hPtr->flag == TCL_CHECK_SYNTAX)
            resultPtr->intValue = 1;
        else
            resultPtr->intValue = 0;
    } else {
        resultPtr->intValue = atoi(value);
    }

    return TCL_OK;
}

/* stringValue()
 * Evaluate host absed on shared resource string value.
 */
int
stringValue(ClientData clientData,
            Tcl_Interp *interp,
            int argc,
            const char *argv[])
{
    int *indx;
    char *sp;
    char *sp2;
    char *value;
    char status[MAXLSFNAMELEN];
    struct hostent *hp;

    if (argc != 3) {
        Tcl_SetResult(interp, "wrong # args", NULL);
        return TCL_ERROR;
    }

    indx = clientData;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "\
stringValue: arg0 %s arg1 %s arg2 %s indx %d hostname %s",
                  argv[0], argv[1], argv[2],
                  *indx, hPtr->hostName);

    switch (*indx) {

        case HOSTNAME:
            overRideFromType = TRUE;
            sp = hPtr->hostName;
            hp = Gethostbyname_((char *)argv[2]);
            if (hp)
                sp2 = hp->h_name;
            else
                sp2 = (char *) argv[2];
            break;

        case HOSTTYPE:
            sp = hPtr->hostType;
            if (strcmp(argv[2], LOCAL_STR) == 0) {
                sp2 = hPtr->fromHostType;
                if (strcmp (argv[1], "eq") != 0)
                    overRideFromType = TRUE;
            } else {
                overRideFromType = TRUE;
                sp2 = (char *) argv[2];
            }
            break;

        case HOSTMODEL:
            overRideFromType = TRUE;
            sp = hPtr->hostModel;
            if (strcmp(argv[2], LOCAL_STR) == 0)
                sp2 = hPtr->fromHostModel;
            else
                sp2 = (char *) argv[2];
            break;

        case HOSTSTATUS:
            status[0] = '\0';
            overRideFromType = TRUE;
            if (LS_ISUNAVAIL(hPtr->status)) {
                strcpy(status, "unavail");
            } else if (LS_ISLOCKED(hPtr->status)) {
                strcpy(status, "lock");
                if (LS_ISLOCKEDU(hPtr->status)) {
                    strcat(status, "U");
                }
                if (LS_ISLOCKEDW(hPtr->status)) {
                    strcat(status, "W");
                }
                if (LS_ISLOCKEDM(hPtr->status)) {
                    strcat(status, "M");
                }
            } else if (LS_ISBUSY(hPtr->status)) {
                strcpy(status, "busy");
            } else {
                strcpy(status,"ok");
            }
            sp = status;
            sp2 = (char *) argv[2];
            break;
        default:

            value = getResValue (*indx - LAST_STRING);
            if (value == NULL || value[0] == '-') {
                if (hPtr->flag == TCL_CHECK_SYNTAX) {
                    Tcl_SetResult(interp, "1", NULL);
                    return(TCL_OK);
                } else {
                    return (TCL_ERROR);
                }
            }
            overRideFromType = TRUE;
            sp = value;
            sp2 = (char *)argv[2];
            break;
    }

    if (logclass & LC_TRACE) {
        ls_syslog(LOG_DEBUG3, "\
stringValue: sp = %s, sp2 = %s", sp, sp2);
    }

    if (strcmp(sp2, WILDCARD_STR) == 0 ) {
        Tcl_SetResult(interp, "1", NULL);
        return TCL_OK;
    }

    if (strcmp(argv[1],"eq") == 0) {
        if (strcmp(sp2, sp) == 0)
            Tcl_SetResult(interp, "1", NULL);
        else
            Tcl_SetResult(interp, "0", NULL);
    } else if (strcmp(argv[1],"ne") == 0) {
        if (strcmp(sp2, sp) != 0)
            Tcl_SetResult(interp, "1", NULL);
        else
            Tcl_SetResult(interp, "0", NULL);
    } else if (strcmp(argv[1],"ge") == 0) {
        if (strcmp(sp2, sp) <= 0)
            Tcl_SetResult(interp, "1", NULL);
        else
            Tcl_SetResult(interp, "0", NULL);
    } else if (strcmp(argv[1],"le") == 0) {
        if (strcmp(sp2,sp) >= 0)
            Tcl_SetResult(interp, "1", NULL);
        else
            Tcl_SetResult(interp, "0", NULL);
    } else if (strcmp(argv[1],"gt") == 0) {
        if (strcmp(sp2, sp) < 0)
            Tcl_SetResult(interp, "1", NULL);
        else
            Tcl_SetResult(interp, "0", NULL);
    } else if (strcmp(argv[1],"lt") == 0) {
        if (strcmp(sp2, sp) > 0)
            Tcl_SetResult(interp, "1", NULL);
        else
            Tcl_SetResult(interp, "0", NULL);
    } else {
        return TCL_ERROR;
    }

    return TCL_OK;
}

static int
definedCmd(ClientData clientData,
           Tcl_Interp *interp,
           int argc,
           const  char *argv[])
{
    int    resNo;
    int    hasRes = FALSE;
    int    isSet;
    int    *indx;
    char   *value;

    if (argc != 2) {
        Tcl_SetResult(interp, "wrong # args", NULL);
        return TCL_ERROR;
    }

    indx = clientData;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "\
definedCmd: argv0 %s argv1 %s indx %d",
                  argv[0], argv[1], *indx);

    overRideFromType = TRUE;
    for (resNo = 0; resNo < myTclLsInfo->nRes; resNo++) {
        if (strcmp (myTclLsInfo->resName[resNo], argv[1]) == 0) {
            hasRes = TRUE;
            break;
        }
    }
    if (hasRes == FALSE)
        return(TCL_ERROR);

    if (hPtr->resBitMaps == NULL) {
        Tcl_SetResult(interp, "0", NULL);
        return (TCL_OK);
    }
    TEST_BIT(resNo, hPtr->resBitMaps, isSet);
    if (isSet == 1)
        Tcl_SetResult(interp, "1", NULL);
    else {
        value = getResValue (resNo);
        if (value == NULL) {
            if (hPtr->flag == TCL_CHECK_SYNTAX)
                Tcl_SetResult(interp, "1", NULL);
            else
                Tcl_SetResult(interp, "0", NULL);
        } else
            Tcl_SetResult(interp, "1", NULL);
    }

    return (TCL_OK);

}

/* initTcl()
 * Initialize the tcl interpreter for the evaluation
 * of resource requirement expressions.
 */
int
initTcl(struct tclLsInfo *tclLsInfo)
{
    int          i;
    int          resNo;
    int          isSet;
    static int   ar3[5];
    attribFunc   *funcPtr;

    static attribFunc attrFuncTable[] = {
        {"cpu"   , R1M},
        {"login" , LS},
        {"idle"  , IT},
        {"swap"  , SWP},
        {"cpuf"  , 0},
        {"ndisks", 0},
        {"rexpri", 0},
        {"ncpus" , 0},
        {"maxmem", 0},
        {"maxswp", 0},
        {"maxtmp" ,0},
        {"server", 0},
        {"slots" , 0},
        {NULL, -1}
    };

    if (myTclLsInfo) {
        freeTclLsInfo(myTclLsInfo, 1);
    }
    if (globinterp) {
        Tcl_DeleteInterp(globinterp);
    }

    if (copyTclLsInfo (tclLsInfo) < 0)
        return -1;

    numIndx = tclLsInfo->numIndx;
    nRes = tclLsInfo->nRes;

    attrFuncTable[4].clientData  = CPUFACTOR;
    attrFuncTable[5].clientData  = NDISK;
    attrFuncTable[6].clientData  = REXPRI;
    attrFuncTable[7].clientData  = MAXCPUS_;
    attrFuncTable[8].clientData  = MAXMEM;
    attrFuncTable[9].clientData  = MAXSWAP;
    attrFuncTable[10].clientData = MAXTMP;
    attrFuncTable[11].clientData = SERVER;
    attrFuncTable[12].clientData = SLOTS;

    globinterp = Tcl_CreateInterp();

    /* The math functions are invoked by the interpreter
     * while evaluating the expression. The expression itself
     * is made of symbols R15S, R1M etc which we define
     * her as functions together with input (ClientData)
     * that tcl has to pass them.
     */
    ar = calloc(tclLsInfo->numIndx + tclLsInfo->nRes, sizeof(int));

    for(i = 0; i < tclLsInfo->numIndx; i++) {

        if (logclass & LC_TRACE)
            ls_syslog (LOG_DEBUG3, "\
initTcl:indexNames=%s, i =%d", tclLsInfo->indexNames[i], i);

        ar[i] = i;
        Tcl_CreateMathFunc(globinterp,
                           tclLsInfo->indexNames[i],
                           0,
                           NULL,
                           numericValue,
                           (ClientData)&ar[i]);
    }

    for (resNo = 0; resNo < tclLsInfo->nRes; resNo++) {

        TEST_BIT (resNo, tclLsInfo->numericResBitMaps, isSet);
        if (isSet == 0)
            continue;

        if (logclass & LC_TRACE)
            ls_syslog (LOG_DEBUG3, "\
initTcl:Name=%s, i =%d", tclLsInfo->resName[resNo], i);

        ar[i] = resNo + tclLsInfo->numIndx;
        Tcl_CreateMathFunc(globinterp,
                           tclLsInfo->resName[resNo],
                           0,
                           NULL,
                           numericValue,
                           (ClientData)&ar[i]);
        i++;
    }

    for (funcPtr = attrFuncTable; funcPtr->name != NULL; funcPtr++) {

        if (logclass & LC_TRACE)
            ls_syslog (LOG_DEBUG3, "\
initTcl:indexNames=%s, i =%d", funcPtr->name, funcPtr->clientData);

        Tcl_CreateMathFunc(globinterp,
                           funcPtr->name,
                           0,
                           NULL,
                           numericValue,
                           (ClientData)&funcPtr->clientData);
    }

    i = 0;
    ar2 = calloc(tclLsInfo->nRes, sizeof(int));
    for (resNo = 0; resNo < tclLsInfo->nRes; resNo++) {

        TEST_BIT (resNo, tclLsInfo->numericResBitMaps, isSet);
        if(isSet == TRUE)
            continue;

        TEST_BIT (resNo, tclLsInfo->stringResBitMaps, isSet);
        if(isSet == TRUE)
            continue;

        ar2[i] = resNo;
        Tcl_CreateMathFunc(globinterp,
                           tclLsInfo->resName[resNo],
                           0,
                           NULL,
                           booleanValue,
                           (ClientData)&ar2[i]);
        ++i;
    }

    ar3[0] = HOSTTYPE;
    Tcl_CreateCommand(globinterp,
                      "type",
                      stringValue,
                      (ClientData)&ar3[0],
                      NULL);
    ar3[1] = HOSTMODEL;
    Tcl_CreateCommand(globinterp,
                      "model",
                      stringValue,
                      (ClientData)&ar3[1],
                      NULL);
    ar3[2] = HOSTSTATUS;
    Tcl_CreateCommand(globinterp,
                      "status",
                      stringValue,
                      (ClientData)&ar3[2],
                      NULL);
    ar3[3] = HOSTNAME;
    Tcl_CreateCommand(globinterp,
                      "hname",
                      stringValue,
                      (ClientData)&ar3[3],
                      NULL);

    ar3[4] = DEFINEDFUNCTION;
    Tcl_CreateCommand(globinterp,
                      "defined",
                      definedCmd,
                      (ClientData)&ar3[4],
                      NULL);

    i = 0;
    ar4 = calloc(tclLsInfo->nRes, sizeof(int));

    for (resNo = 0; resNo < tclLsInfo->nRes; resNo++) {

        TEST_BIT (resNo, tclLsInfo->stringResBitMaps, isSet);
        if (isSet == FALSE)
            continue;

        ar4[i] = resNo + LAST_STRING;
        Tcl_CreateCommand(globinterp,
                          tclLsInfo->resName[resNo],
                          stringValue,
                          (ClientData)&ar4[i],
                          NULL);
        ++i;
    }

    return 0;
}

/* evalResReq()
 */
int
evalResReq(char *resReq,
           struct tclHostData *hPtr2,
           char useFromType)
{
    int code, i, resBits;

    hPtr = hPtr2;

    overRideFromType = FALSE;
    runTimeDataQueried = FALSE;

    hPtr->overRideFromType = FALSE;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "\
evalResReq: resReq=%s, host = %s", resReq, hPtr->hostName);

    code = Tcl_Eval(globinterp, resReq);
    if (code != TCL_OK) {
        return -1;
    }

    hPtr->overRideFromType = overRideFromType;

    resBits = 0;
    if (!hPtr->ignDedicatedResource && hPtr->DResBitMaps != NULL) {
        for (i = 0; i < GET_INTNUM(nRes); i++)
            resBits += hPtr->DResBitMaps[i];

        if (resBits != 0 && hPtr->resBitMaps) {
            resBits = 0;
            for (i = 0; i < GET_INTNUM(nRes); i++)
                resBits += hPtr->resBitMaps[i] & hPtr->DResBitMaps[i];
            if (resBits == 0)
                return 0;
        }
    }

    if (!overRideFromType && useFromType) {
        if (strcmp(hPtr->hostType, hPtr->fromHostType) != 0)
            return 0;
    }

    if (runTimeDataQueried && LS_ISUNAVAIL(hPtr->status))
        return 0;

    if (strcmp(Tcl_GetStringResult(globinterp), "0") == 0)
        return 0;

    return 1;
}

/* getResValue()
 */
static char *
getResValue(int resNo)
{
    int   i;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG3, "\
getResValue:resNo=%d, resName =%s, nRes=%d, hPtr->numResPairs=%d",
                  resNo, myTclLsInfo->resName[resNo],
                  myTclLsInfo->nRes, hPtr->numResPairs);

    if (resNo > myTclLsInfo->nRes || hPtr->numResPairs <= 0)
        return NULL;

    for (i = 0; i < hPtr->numResPairs; i++) {
        if (strcmp (hPtr->resPairs[i].name,
                    myTclLsInfo->resName[resNo]) == 0) {
            return (hPtr->resPairs[i].value);
        }
    }

    return NULL;
}

/* copyTcllsInfo()
 */
static int
copyTclLsInfo(struct tclLsInfo *tclLsInfo)
{
    int   i;

    myTclLsInfo = calloc(1, sizeof (struct tclLsInfo));

    myTclLsInfo->nRes = tclLsInfo->nRes;
    myTclLsInfo->numIndx = tclLsInfo->numIndx;

    myTclLsInfo->resName = calloc(myTclLsInfo->nRes, sizeof(char *));

    myTclLsInfo->stringResBitMaps
        = calloc(GET_INTNUM(myTclLsInfo->nRes), sizeof(int));

    myTclLsInfo->indexNames
        = calloc(myTclLsInfo->numIndx , sizeof(char *));

    myTclLsInfo->numericResBitMaps
        = calloc(GET_INTNUM(myTclLsInfo->nRes), sizeof(int));

    for (i = 0; i < myTclLsInfo->nRes; i++) {
        myTclLsInfo->resName[i] = strdup(tclLsInfo->resName[i]);
    }

    for (i = 0; i < myTclLsInfo->numIndx; i++) {
        myTclLsInfo->indexNames[i] = strdup(tclLsInfo->indexNames[i]);
    }

    for (i = 0; i < GET_INTNUM(myTclLsInfo->nRes); i++) {
        myTclLsInfo->stringResBitMaps[i] = tclLsInfo->stringResBitMaps[i];
        myTclLsInfo->numericResBitMaps[i] = tclLsInfo->numericResBitMaps[i];
    }

    return(0);

}

/* freeTclLsInfo()
 */
void
freeTclLsInfo(struct tclLsInfo *tclLsInfo, int mode)
{
    int i;

    if (tclLsInfo) {
        if (mode == 1) {
            for (i = 0; i < tclLsInfo->numIndx; i++) {
                FREEUP(tclLsInfo->indexNames[i])
                    }
            for (i = 0; i < tclLsInfo->nRes; i++) {
                FREEUP(tclLsInfo->resName[i])
                    }
        }
        FREEUP(tclLsInfo->indexNames);
        FREEUP(tclLsInfo->resName);
        FREEUP(tclLsInfo->stringResBitMaps);
        FREEUP(tclLsInfo->numericResBitMaps);
        FREEUP(tclLsInfo);
    }

}

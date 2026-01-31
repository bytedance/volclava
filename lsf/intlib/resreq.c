/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
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

#include "intlibout.h"

struct sections {
    char   *select;
    char   *order;
    char   *rusage;
    char   *filter;
    char   *span;
};

enum syntaxType {OLD, NEW, EITHER};

static int parseSection(char *, struct sections *);
static int parseSelect(char * ,
                       struct resVal *,
                       struct lsInfo *,
                       bool_t,
                       int,
                       int);
static int parseOrder(char * ,
                      struct resVal *,
                      struct lsInfo *);
static int parseFilter(char * ,
                       struct resVal *,
                       struct lsInfo *);
static int parseUsage(char * ,
                      struct resVal *,
                      struct lsInfo *,
                      int);
static int parseSpan (char *,
                      struct resVal *);
static int resToClassNew(char * ,
                         struct resVal *,
                         struct lsInfo *,
                         int,
                         int);
static int resToClassOld(char * ,
                         struct resVal *,
                         struct lsInfo *,
                         int,
                         int);
static int setDefaults(struct resVal *,
                       struct lsInfo *,
                       int);
static int getVal(char **, float *);
static enum syntaxType getSyntax(char *);
static int getKeyEntry (char *);
static int getTimeVal(char **, float *);
void freeResVal (struct resVal *);
void initResVal (struct resVal *);

extern char isanumber_(char *);

hTab resNameTbl = {NULL, 0, 0};
hTab keyNameTbl = {NULL, 0, 0};
#define KEY_DURATION    1
#define KEY_HOSTS       2
#define KEY_PTILE       3
#define KEY_DECAY       4
#define NUM_KEYS        5

#define ALLOC_STRING(buffer, buffer_len, req_len) {     \
        if (buffer == NULL || buffer_len < req_len) {   \
            FREEUP(buffer);                             \
            buffer = malloc(req_len);                   \
            buffer_len = req_len;                       \
        }                                               \
    }

#define REALLOC_STRING(buffer, buffer_len, req_len) {   \
        if (buffer == NULL) {                           \
            buffer = malloc(req_len);                   \
            buffer_len = req_len;                       \
        }                                               \
        else if (buffer_len < req_len) {                \
            char *tmp;                                  \
            tmp = realloc(buffer, req_len);             \
            if (tmp == NULL)                            \
                FREEUP(buffer);                         \
            buffer = tmp;                               \
            buffer_len = req_len;                       \
        }                                               \
    }

/* initParse()
 */
void
initParse(struct lsInfo *lsInfo)
{
    static char   fname[] = "initParse()";
    int           i;
    int           *indx;
    hEnt          *hashEntPtr;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG1, "%s: Entering this routine...", fname);

    if (!h_TabEmpty_(&resNameTbl)) {
        h_freeRefTab_(&resNameTbl);
    }

    h_initTab_(&resNameTbl, lsInfo->nRes);

    /* We firmly believe those calloc() will
     * succeed.
     */
    indx = calloc(lsInfo->nRes, sizeof(int));
    for (i = 0; i < lsInfo->nRes; i++) {
        hashEntPtr = h_addEnt_(&resNameTbl,
                               lsInfo->resTable[i].name,
                               NULL);
        indx[i] = i;
        hashEntPtr->hData = &indx[i];
    }

    /* login, swap, idle and cpu are more human
     * readable aliases for ls, swp,it and ut (?).
     */
    indx = calloc(4, sizeof(int));
    hashEntPtr = h_addEnt_(&resNameTbl, "login", NULL);
    indx[0] = LS;
    hashEntPtr->hData = &indx[0];

    hashEntPtr = h_addEnt_(&resNameTbl, "swap", NULL);
    indx[1] = SWP;
    hashEntPtr->hData = &indx[1];

    hashEntPtr = h_addEnt_(&resNameTbl, "idle", NULL);
    indx[2] = IT;
    hashEntPtr->hData = &indx[2];

    hashEntPtr = h_addEnt_(&resNameTbl, "cpu", NULL);
    indx[3] = R1M;
    hashEntPtr->hData = &indx[3];

    if (h_TabEmpty_(&keyNameTbl)) {
        int   *key;

        key = calloc(NUM_KEYS, sizeof(int));
        h_initTab_(&keyNameTbl, NUM_KEYS);

        hashEntPtr = h_addEnt_(&keyNameTbl, "duration", NULL);
        key[0] = KEY_DURATION;
        hashEntPtr->hData = &key[0];

        hashEntPtr = h_addEnt_(&keyNameTbl, "decay", NULL);
        key[1] = KEY_DECAY;
        hashEntPtr->hData = &key[1];

        hashEntPtr = h_addEnt_(&keyNameTbl, "hosts", NULL);
        key[2] = KEY_HOSTS;
        hashEntPtr->hData = &key[2];

        hashEntPtr = h_addEnt_(&keyNameTbl, "ptile", NULL);
        key[3] = KEY_PTILE;
        hashEntPtr->hData = &key[3];
    }
}
/* getResEntry()
 */
int
getResEntry(const char *res)
{
    hEnt   *ent;
    int    *indx;

    ent = h_getEnt_(&resNameTbl, res);
    if (ent == NULL)
        return -1;

    indx = ent->hData;
    return *indx;
}

/* getKeyEntry()
 */
static int
getKeyEntry(char *key)
{
    hEnt   *ent;
    int    *x;

    ent = h_getEnt_(&keyNameTbl, key);
    if (ent == NULL)
        return -1;

    x = ent->hData;

    return(*x);
}

int
parseResReq(char *resReq,
            struct resVal *resVal,
            struct lsInfo *lsInfo,
            int options,
            int unitForLimits)
{
    static char       fname[] = "parseResReq()";
    int               cc;
    struct sections   reqSect;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG3, "%s: resReq=%s", fname, resReq);

    initResVal (resVal);

    ALLOC_STRING(resVal->selectStr,
                 resVal->selectStrSize,
                 MAX(3*strlen(resReq) + 1, MAXLINELEN + MAXLSFNAMELEN));

    if ((cc = parseSection(resReq, &reqSect)) != PARSE_OK)
        return(cc);

    if ((cc = setDefaults(resVal, lsInfo, options)) < 0)
        return (cc);

    if (options & PR_SELECT) {

        if (options & PR_XOR) {
            if ((cc = parseSelect(reqSect.select,
                                  resVal,
                                  lsInfo,
                                  TRUE,
                                  options,
                                  unitForLimits)) != PARSE_OK)
                return(cc);
        } else {
            if ((cc = parseSelect(reqSect.select,
                                  resVal,
                                  lsInfo,
                                  FALSE,
                                  options,
                                  unitForLimits)) != PARSE_OK)
                return(cc);
	}
    }

    if (options & PR_ORDER) {
        if ((cc = parseOrder(reqSect.order,
                             resVal,
                             lsInfo)) != PARSE_OK)
            return(cc);
    }

    if (options & PR_RUSAGE) {
        if ((cc = parseUsage(reqSect.rusage,
                             resVal,
                             lsInfo,
                             unitForLimits)) != PARSE_OK)
            return(cc);
    }

    if (options & PR_FILTER) {
        if ((cc = parseFilter(reqSect.filter,
                              resVal,
                              lsInfo)) != PARSE_OK)
            return(cc);
    }

    if (options & PR_SPAN) {
        if ((cc = parseSpan(reqSect.span,
                            resVal)) != PARSE_OK)
            return(cc);
    }

    return(PARSE_OK);
}

static int
parseSection(char *resReq, struct sections *section)
{
    static char   *reqString;
    static int    reqStringSize;
    char          *cp;
    static char   *check_cp;
    char          *p;
    int           i;
    int           j;
    static int    flag_match_section=0;

#define NSECTIONS       5
#define SELECT_SECT     0
#define ORDER_SECT      1
#define RUSAGE_SECT     2
#define FILTER_SECT     3
#define SPAN_SECT       4

    static char   *keywords[]= {
        "select",
        "order",
        "rusage",
        "filter",
        "span"
    };
    char   *sectptr[NSECTIONS];

    for (i = 0; i < NSECTIONS ; i++) {
        sectptr[i] = NULL;
    }

    if (resReq == NULL)
        return PARSE_BAD_EXP;

    ALLOC_STRING(reqString, reqStringSize, strlen(resReq)+1);
    if (reqString == NULL)
        return PARSE_BAD_MEM;

    strcpy(reqString, resReq);

    for (i = 0; i < NSECTIONS ; i++) {
        cp = reqString;
        while ((p = strstr(cp, keywords[i])) != NULL) {
            if (*(p + strlen(keywords[i])) == '[') {
                sectptr[i] = p;
                break;
            } else {
                cp = p + strlen(keywords[i]);
            }
        }
    }

    for (i = 0; i < NSECTIONS ; i++) {

        if (sectptr[i] != NULL) {

            *sectptr[i] = '\0';
            sectptr[i] += strlen(keywords[i]);
            if (*sectptr[i] != '[')
                return PARSE_BAD_EXP;
            sectptr[i]++;


            cp = sectptr[i];
            check_cp = sectptr[i];
            while((*cp != ']') && (*cp != '\0')) {
                cp++;
                check_cp++;
            }
            if (*cp != ']')
                return PARSE_BAD_EXP;
            else
                check_cp++;

            if (*check_cp != '\0') {
                flag_match_section = 0;
                for (j = 0; j < NSECTIONS ; j++) {
                    if (strstr(check_cp, keywords[j]) != NULL) {
                        flag_match_section = 1;
                        if (i==j) {
                            ls_syslog(LOG_DEBUG,"parseSection check failed loop 2.1 j:%d, duplicate section:%s.", j, keywords[j]);
                            return PARSE_BAD_EXP;
                        }
                    }
                }

                while (*check_cp != '\0') {

                    if ((*check_cp != ' ') && (flag_match_section == 0)){
                        ls_syslog(LOG_DEBUG,"parseSection check failed loop 2.2 j:%d, *cp:%c, *check_cp:%c.", j, *cp, *check_cp);
                        return PARSE_BAD_EXP;
                    }
                    check_cp++;
                }

            }

            *cp  = '\0';
        } else {
            if (i == SELECT_SECT)
                sectptr[i] = reqString;
            else
                sectptr[i] = (reqString + reqStringSize - 1);
        }
    }


    for (i=0; i < NSECTIONS; i++) {
        for (j=strlen(sectptr[i]) - 1; j >= 0; j--) {
            if (sectptr[i][j] == ' ')
                sectptr[i][j] = '\0';
            else
                break;
        }
        ls_syslog(LOG_DEBUG,"parseSection loop 3 i:%d, sectptr[i]:%s, keywords[i]:%s",i ,sectptr[i], keywords[i]);

    }

    section->select = sectptr[SELECT_SECT];
    section->order  = sectptr[ORDER_SECT];
    section->rusage = sectptr[RUSAGE_SECT];
    section->filter = sectptr[FILTER_SECT];
    section->span   = sectptr[SPAN_SECT];

    return PARSE_OK;

}

static int
parseSelect(char *resReq, struct resVal *resVal, struct lsInfo *lsInfo, bool_t parseXor, int options, int unitForLimits)
{
    static char fname[] = "parseSelect";
    int cc;
    enum syntaxType syntax;
    char *expr, *countPtr;
    int i, numXorExprs;
    struct resVal tmpResVal;
    char *resReq2 = NULL;

    if (logclass & (LC_TRACE | LC_SCHED))
        ls_syslog(LOG_DEBUG3, "%s: resReq=%s", fname, resReq);



    if (parseXor && resReq[0] != '\0') {

	countPtr = (char *)malloc(strlen(resReq)+1);
	if (countPtr == NULL) {
	    return(PARSE_BAD_MEM);
	}
	strcpy(countPtr, resReq);
	expr = strtok(countPtr, ",");
	if (expr == NULL) {

	    numXorExprs = 0;
	} else {
	    numXorExprs = 1;

	    while (strtok(NULL, ",")) {
	        numXorExprs++;
            }
	}
	free(countPtr);

	if (logclass & (LC_TRACE |LC_SCHED))
	    ls_syslog(LOG_DEBUG3,"%s: numXorExprs = %d", fname, numXorExprs);


        if (numXorExprs > 1) {

	    resVal->xorExprs =
		(char **)calloc(numXorExprs + 1, sizeof(char*));
	    if (resVal->xorExprs == NULL)
		return(PARSE_BAD_MEM);


            resReq2 = (char *)malloc(strlen(resReq) + numXorExprs * 4 - 1);
	    if ( resReq2== NULL)
		return (PARSE_BAD_MEM);
	    resReq2[0] = '\0';
	    expr = strtok(resReq, ",");
	    for ( i =0 ; i < numXorExprs; i++) {
		initResVal(&tmpResVal);

		ALLOC_STRING(tmpResVal.selectStr, tmpResVal.selectStrSize,
                             MAX(3*strlen(expr) + 1, MAXLINELEN + MAXLSFNAMELEN));

    		if (tmpResVal.selectStr == NULL) {
		    FREEUP(resReq2);
        	    return PARSE_BAD_MEM;
		}
		if (setDefaults(&tmpResVal, lsInfo, options) < 0) {
		    FREEUP(resReq2);
		    return (PARSE_BAD_MEM);
		}
		if ((cc = parseSelect(expr, &tmpResVal, lsInfo, FALSE, options, unitForLimits)) != PARSE_OK) {
		    for (i--;i>=0; i--) {
			FREEUP(resVal->xorExprs[i]);
		    }
		    FREEUP(resReq2);
		    return cc;
                }
                resVal->xorExprs[i] =
		    (char *)calloc(strlen(tmpResVal.selectStr) + 1,
				   sizeof(char));
		if (resVal->xorExprs[i] == NULL)
		    return (PARSE_BAD_MEM);

		strcpy(resVal->xorExprs[i], tmpResVal.selectStr);
		if (logclass & (LC_TRACE | LC_SCHED))
		    ls_syslog(LOG_DEBUG3,"%s: xorExprs[%d] = %s", fname, i, resVal->xorExprs[i]);
                if (i == 0 ) {
                    sprintf(resReq2, "(%s)", expr);
		} else {
                    char tmpbuf[4096];
                    snprintf(tmpbuf, sizeof(tmpbuf), "%s||(%s)", resReq2, expr);
                    /* We're assuming resReq2 has enough space — old code,
                     * we trust it *for now*
                     */
                    if (strlen(tmpbuf) + 1 > strlen(resReq) + numXorExprs * 4 - 1) {
                        // Bail out or truncate
                        return PARSE_BAD_MEM;
                    }
                    strcpy(resReq2, tmpbuf);

		}
		freeResVal(&tmpResVal);
		expr = strtok(NULL, ",");
	    }
	    resVal->xorExprs[i] = NULL;
	    if (logclass & (LC_TRACE | LC_SCHED))
		ls_syslog(LOG_DEBUG3,"%s: new selectStr=%s", fname, resReq2);
            resReq = resReq2;
	} else {
	    if (numXorExprs == 1) {

		if (strchr(resReq,',')) {
	            return(PARSE_BAD_EXP);
		}
	    } else {

	        return(PARSE_BAD_EXP);
	    }
	}
    }

    syntax = getSyntax(resReq);
    switch(syntax) {
        case OLD:
            if ( (cc =resToClassOld(resReq, resVal, lsInfo, unitForLimits, options)) != PARSE_OK) {
                resVal->selectStr[0] = '\0';
                FREEUP(resReq2);
                return(cc);
            }
            break;
        case NEW:
            if ( (cc =resToClassNew(resReq, resVal, lsInfo, unitForLimits, options)) != PARSE_OK) {
                resVal->selectStr[0] = '\0';
                FREEUP(resReq2);
                return (cc);
            }
            break;
        case EITHER:
            if ((cc =resToClassOld(resReq, resVal, lsInfo, unitForLimits, options)) != PARSE_OK)  {

                if (cc == PARSE_BAD_NAME || cc == PARSE_BAD_VAL) {
                    resVal->selectStr[0] = '\0';
                    FREEUP(resReq2);
                    return (cc);
                }
                if ( (cc =resToClassNew(resReq, resVal, lsInfo, unitForLimits, options)) != PARSE_OK) {
                    resVal->selectStr[0] = '\0';
                    FREEUP(resReq2);
                    return (cc);
                }
            }
            break;
        default:
            FREEUP(resReq2);
            return PARSE_BAD_EXP;
    }

    FREEUP(resReq2);
    return PARSE_OK;

}

static int
parseOrder(char *orderReq, struct resVal *resVal, struct lsInfo *lsInfo)
{
    int i, m, entry;
    char *token, negate;

    if ((i=strlen(orderReq)) == 0)
        return PARSE_OK;

    for(m=0; m<i; m++)
        if (orderReq[m] != ' ')
            break;

    if (m == i)
        return PARSE_OK;

    resVal->nphase = 0;
    while ((token = getNextToken(&orderReq)) != NULL) {
        negate = FALSE;
        if (token[0] == '-') {
            negate = TRUE;
            token++;
        }
        entry = getResEntry(token);
        if ((entry < 0) || (lsInfo->resTable[entry].orderType == NA))
            return PARSE_BAD_NAME;

        if (resVal->nphase > NBUILTINDEX)
            break;

        if (negate)
            resVal->order[resVal->nphase] = -(entry +1);
        else
            resVal->order[resVal->nphase] =  entry + 1;
        resVal->nphase++;
    }
    resVal->options |= PR_ORDER;
    return PARSE_OK;
}


static int
parseFilter(char *filter, struct resVal *resVal, struct lsInfo *lsInfo)
{
    int i, m, entry;
    char *token;

    if ((i=strlen(filter)) == 0)
        return PARSE_OK;

    for(m=0; m<i; m++)
        if (filter[m] != ' ')
            break;

    if (m == i)
        return PARSE_OK;

    resVal->nindex = 0;
    while ((token = getNextToken(&filter)) != NULL) {
        entry = getResEntry(token);

        if (entry <  0)
            return(PARSE_BAD_FILTER);
        if (!(lsInfo->resTable[entry].flags & RESF_DYNAMIC))
            return(PARSE_BAD_FILTER);
        if (lsInfo->resTable[entry].flags & RESF_SHARED)
            return(PARSE_BAD_FILTER);
        resVal->indicies[resVal->nindex++] = entry;
    }
    resVal->options |= PR_FILTER;
    return(PARSE_OK);
}

static int
parseUsage (char *usageReq, struct resVal *resVal, struct lsInfo *lsInfo, int unitForLimits)
{
    int i, j, m, entry;
    float value;
    char *token;

    if ((i=strlen(usageReq)) == 0)
        return PARSE_OK;

    for(m=0; m<i; m++)
        if (usageReq[m] != ' ')
            break;
    if (m == i)
        return PARSE_OK;

    resVal->genClass = 0;
    while ((token = getNextToken(&usageReq)) != NULL) {
        if (token[0] == '-')
            token++;


        entry = getKeyEntry (token);
        if (entry > 0) {
            if (entry != KEY_DURATION && entry != KEY_DECAY)
                return(PARSE_BAD_NAME);

            if (usageReq[0] == '=') {
    	        int returnValue;
                if (entry == KEY_DURATION)
                    returnValue =  getTimeVal(&usageReq, &value);
                else
                    returnValue = getVal(&usageReq, &value);
                if (returnValue < 0 || value < 0.0)
                    return PARSE_BAD_VAL;
                if (entry == KEY_DURATION)
                    resVal->duration = value;
                else
                    resVal->decay = value;

                continue;
            }
        }


        entry = getResEntry (token);
        if (entry < 0)
            return(PARSE_BAD_NAME);

        /*slots is not supported in rusage*/
        if (strcmp(lsInfo->resTable[entry].name, "slots") == 0) {
            return(PARSE_BAD_NAME);
        }

        if (!(lsInfo->resTable[entry].flags & RESF_DYNAMIC) &&
        (lsInfo->resTable[entry].valueType != LS_NUMERIC)) {
            if (usageReq[0] == '=') {
                if (getVal(&usageReq, &value) < 0 || value < 0.0)
                    return PARSE_BAD_VAL;
            }
            continue;
        }

        if (entry < MAXSRES)
            resVal->genClass |= 1 << entry;
        SET_BIT(entry, resVal->rusgBitMaps);
        if (usageReq[0] == '=') {
            if (getVal(&usageReq, &value) < 0 || value < 0.0)
                return PARSE_BAD_VAL;
            resVal->val[entry] = value;

            // deal with LSF_UNIT_FOR_LIMITS
            if (entry == MEM || entry == SWP || entry == TMP) {
                for (j = 0; j < unitForLimits; j++) {
                    resVal->val[entry] = resVal->val[entry] * 1000;
                    value = value * 1000;
                }
            }
        }
    }
    resVal->options |= PR_RUSAGE;
    return(PARSE_OK);
}

static int
parseSpan (char *usageReq, struct resVal *resVal)
{
    int i, m, entry;
    int val1, val2;
    char *token;

    if ((i = strlen(usageReq)) == 0)
        return PARSE_OK;

    for(m = 0; m < i; m++)
        if (usageReq[m] != ' ')
            break;
    if (m == i)
        return PARSE_OK;

    while ((token = getNextToken(&usageReq)) != NULL) {
        if (token[0] == '-')
            token++;


        entry = getKeyEntry (token);
        if (entry >= 0 && entry < KEY_HOSTS)
            return(PARSE_BAD_NAME);
        if (getValPair (&usageReq, &val1, &val2) < 0
            || val1 <= 0.0 || val2 <= 0.0)
            return PARSE_BAD_VAL;
        switch (entry) {
            case KEY_HOSTS:
                resVal->numHosts = val1;
                resVal->maxNumHosts = val2;
                break;
            case KEY_PTILE:
                resVal->pTile = val1;
                if (val2 != INFINIT_INT)
                    return PARSE_BAD_VAL;
                break;
            default:
                return(PARSE_BAD_NAME);
        }
    }
    resVal->options |= PR_SPAN;
    return(PARSE_OK);
}

static enum syntaxType
getSyntax(char *resReq)
{
    char *sp;

    if (strchr(resReq,':') != NULL)
        return (OLD);

    if (strchr(resReq,' ') != NULL)
        return(NEW);

    if ( (strchr(resReq,'(') != NULL) || (strchr(resReq,')') != NULL) ||
         (strchr(resReq,'|') != NULL) || (strchr(resReq,'!') != NULL) ||
         (strchr(resReq,'>') != NULL) || (strchr(resReq,'<') != NULL) ||
         (strchr(resReq,'&') != NULL) || (strchr(resReq,'+') != NULL) ||
         (strchr(resReq,'*') != NULL) || (strchr(resReq,'/') != NULL) )
        return (NEW);


    if (((sp=strchr(resReq,'=')) != NULL) && (*++sp == '='))
        return(NEW);

    return (EITHER);
}

static int
validValue(char *value, struct lsInfo *lsInfo, int nentry)
{
    int i;


    if (strcmp(value, WILDCARD_STR) == 0 ||
        strcmp(value, LOCAL_STR) == 0 )
        return 0;

    if (strcmp(lsInfo->resTable[nentry].name,"type") == 0) {
        for(i=0; i < lsInfo->nTypes; i++)
            if (strcmp(lsInfo->hostTypes[i], value) == 0)
                break;
        if (i == lsInfo->nTypes)
            return -1;
        else
            return 0;
    }

    if (strcmp(lsInfo->resTable[nentry].name,"model") == 0) {
        for(i=0; i < lsInfo->nModels; i++)
            if (strcmp(lsInfo->hostModels[i], value) == 0)
                break;
        if (i == lsInfo->nModels)
            return -1;
        else
            return 0;
    }

    if (strcmp(lsInfo->resTable[nentry].name,"status") == 0) {
        if (  strcmp(value,"ok") == 0    || strcmp(value,"busy") == 0  ||
              strcmp(value,"lockU") == 0 || strcmp(value,"lockW") == 0 ||
              strcmp(value,"lock") == 0  || strcmp(value,"lockM") == 0 ||
	      strcmp(value,"lockUW") == 0|| strcmp(value,"lockUM") == 0||
	      strcmp(value,"lockWM") == 0|| strcmp(value,"lockUWM") == 0 ||
	      strcmp(value,"unavail") == 0 )
            return 0;
        else
            return -1;
    }
    return 0;

}


static int
resToClassNew(char *resReq, struct resVal *resVal,
              struct lsInfo *lsInfo, int unitForLimits, int options)
{
    int i, j, s, t, len, entry, hasQuote;
    char res[MAXLSFNAMELEN], val[MAXLSFNAMELEN];
    char tmpbuf[MAXLSFNAMELEN * 7];
    char *sp, *op;

    len = strlen(resReq);

    sp = resVal->selectStr;
    strcpy(sp,"expr ");
    s = 0;
    t = strlen(sp);

    while (s < len) {
        if (t >= (resVal->selectStrSize - MAXLSFNAMELEN))
            return PARSE_BAD_EXP;


        if (resReq[s] == ' ') {
            s++;
            continue;
        }

        if ( resReq[s] == '(' || resReq[s] == ')' || resReq[s] == '=' ||
             resReq[s] == '!' || resReq[s] == '>' || resReq[s] == '<' ||
             resReq[s] == '|' || resReq[s] == '&' || resReq[s] == '+' ||
             resReq[s] == '-' || resReq[s] == '*' || resReq[s] == '/' ||
             resReq[s] == '.' ) {

            sp[t++] = resReq[s++];
            continue;
        }

        if (IS_DIGIT(resReq[s])){
            sp[t++] = resReq[s++];

            // deal with LSF_UNIT_FOR_LIMITS
            if (( resReq[s] == '\0' || ! IS_DIGIT(resReq[s]) ) && ( entry == MEM || entry == SWP || entry == TMP )) {
                for (j = 0; j < unitForLimits * 3 ; j++) {
                    sp[t++] = '0';
                }
            }

            continue;
        }

        if (IS_LETTER(resReq[s])) {

            i = 0;
            while(IS_LETTER(resReq[s]) || IS_DIGIT(resReq[s]) ||
                  IS_VALID_OTHER(resReq[s]))
                res[i++] = resReq[s++];
            res[i] = '\0';

            entry = getResEntry(res);

            if (entry < 0) {
                if (strncmp ("defined", res, strlen (res)) == 0) {
                    while (resReq[s] == ' ')
                        s++;
                    if (resReq[s] != '(')
                        return (PARSE_BAD_EXP);
                    i = 0;
                    s++;
                    while (IS_LETTER(resReq[s]) || IS_DIGIT(resReq[s]) ||
                           IS_VALID_OTHER(resReq[s]))
                        res[i++] = resReq[s++];
                    res[i] = '\0';
                    entry = getResEntry(res);
                    if (entry < 0)
                        return PARSE_BAD_NAME;
                    sprintf(tmpbuf,"[%s \"%s\" ]",
                            "defined", lsInfo->resTable[entry].name);
                    sp[t] = '\0';
                    strcat(sp,tmpbuf);
                    t += strlen(tmpbuf);
                    while (resReq[s] == ' ')
                        s++;
                    if (resReq[s] != ')')
                        return (PARSE_BAD_EXP);
                    s++;
                    continue;
                }
                return PARSE_BAD_NAME;
            }

            if (lsInfo->resTable[entry].flags & RESF_BATCH) {
                if (!(options & PR_BATCH)) {
                    return PARSE_BAD_NAME;
                }
            }

            switch(lsInfo->resTable[entry].valueType) {
                case LS_NUMERIC:
                case LS_BOOLEAN:
                    strcat(res,"()");
                    sp[t] = '\0';
                    strcat(sp,res);
                    t += strlen(res);
                    break;
                case LS_STRING:

                    while(resReq[s] == ' ')
                        s++;


                    if (resReq[s] == '\0' || resReq[s+1] == '\0')
                        return(PARSE_BAD_EXP);


                    op = NULL;
                    if (resReq[s] == '=' && resReq[s+1] == '=') {
                        op = "eq";
                        s += 2;
                    } else if (resReq[s] == '!' && resReq[s+1] == '=') {
                        op = "ne";
                        s += 2;
                    } else if (resReq[s] == '>' && resReq[s+1] == '=') {
                        op = "ge";
                        s += 2;
                    } else if (resReq[s] == '<' && resReq[s+1] == '=') {
                        op = "le";
                        s += 2;
                    } else if (resReq[s] == '<') {
                        op = "lt";
                        s += 1;
                    } else if (resReq[s] == '>') {
                        op = "gt";
                        s += 1;
                    } else {
                        return -1;
                    }

                    while(resReq[s] == ' ')
                        s++;

                    if (resReq[s] == '\''){
                        hasQuote = TRUE;
                        s++;
                    } else
                        hasQuote = FALSE;
                    i = 0;
                    if (!hasQuote){
                        while(IS_LETTER(resReq[s]) || IS_DIGIT(resReq[s]) ||
                              IS_VALID_OTHER(resReq[s]))
                            val[i++] = resReq[s++];
                    } else {
                        while(resReq[s] && resReq[s] != '\'' && i < MAXLSFNAMELEN)
                            val[i++] = resReq[s++];
                        if (i-1 == MAXLSFNAMELEN)
                            return(PARSE_BAD_VAL);
                        if (resReq[s] == '\'')
                            s++;
                    }
                    val[i] = '\0';
                    if (i == 0) {
                        return(PARSE_BAD_VAL);
                    }

                    if (validValue(val, lsInfo, entry) < 0) {
                        return(PARSE_BAD_VAL);
                    }

                    sprintf(tmpbuf,"[%s \"%s\" \"%s\"]",lsInfo->resTable[entry].name
                            ,op,val);
                    sp[t] = '\0';
                    strcat(sp,tmpbuf);
                    t += strlen(tmpbuf);
                default:
                    break;
            }
        } else {
            return (PARSE_BAD_EXP);
        }

    }
    sp[t] = '\0';
    resVal->options |= PR_SELECT;
    return(PARSE_OK);
}

static int
resToClassOld(char *resReq, struct resVal *resVal,
              struct lsInfo *lsInfo, int unitForLimits,
              int options)
{
    int i, j, m, entry;
    char *token;
    char tmpbuf[MAXLINELEN];
    float value;
    char  negate, valueSpecified;
    int t;

    if ((i=strlen(resReq)) == 0)
        return PARSE_OK;

    for (m=0; m<i; m++)
        if (resReq[m] != ' ')
            break;

    if (m == i)
        return PARSE_OK;

    if (resReq[0] == ':' || resReq[0] == '=')
        return PARSE_BAD_EXP;

    strcpy(resVal->selectStr, "expr ");
    t = strlen(resVal->selectStr);

    while ((token = getNextToken(&resReq)) != NULL) {
        negate = FALSE;



        if (t >= (resVal->selectStrSize - MAXLSFNAMELEN))
            return PARSE_BAD_EXP;



        if (IS_DIGIT(token[0]))
            return(PARSE_BAD_EXP);

        if (token[0] == '-') {
            if (token[1] == '\0') {
                sprintf(tmpbuf,"[type \"eq\" \"%s\"] &&", WILDCARD_STR);
                strcat(resVal->selectStr, tmpbuf);
                t += strlen(tmpbuf);
                continue;
            }
            token++;
            negate = TRUE;
        }

        entry = getResEntry(token);
        if (entry < 0)
            return(PARSE_BAD_NAME);

        if (lsInfo->resTable[entry].flags & RESF_BATCH) {
            if (!(options & PR_BATCH))
                return(PARSE_BAD_NAME);
        }

        if (lsInfo->resTable[entry].valueType == LS_BOOLEAN) {
            if (negate)
                sprintf(tmpbuf,"!%s()&&",lsInfo->resTable[entry].name);
            else
                sprintf(tmpbuf,"%s()&&",lsInfo->resTable[entry].name);
            strcat(resVal->selectStr, tmpbuf);
            t += strlen(tmpbuf);
            continue;
        }

        if (negate)
            return(PARSE_BAD_EXP);

        if (lsInfo->resTable[entry].valueType == LS_NUMERIC) {

            valueSpecified = FALSE;
            if (resReq[0] == '=') {
                if (getVal(&resReq, &value) < 0 || value < 0.0)
                    return (PARSE_BAD_VAL);

                if (lsInfo->resTable[entry].flags & RESF_DYNAMIC)
                    resVal->val[entry] = value;
                valueSpecified = TRUE;
            }

            if (!valueSpecified && entry == IT) {
                valueSpecified = TRUE;
                value = IDLETIME;
                resVal->val[entry] = value;
     	    }

            // deal with LSF_UNIT_FOR_LIMITS
            if (entry == MEM || entry == SWP || entry == TMP) {
                for (j = 0; j < unitForLimits; j++) {
                    resVal->val[entry] = resVal->val[entry] * 1000;
                    value = value * 1000;
                }
            }

            if (valueSpecified) {
                if (lsInfo->resTable[entry].orderType == INCR)
                    sprintf(tmpbuf,"%s()<=%6.2f&&",lsInfo->resTable[entry].name,value);
                else
                    sprintf(tmpbuf,"%s()>=%6.2f&&",lsInfo->resTable[entry].name,value);
            } else {
                sprintf(tmpbuf,"%s()&&",lsInfo->resTable[entry].name);
            }
            strcat(resVal->selectStr, tmpbuf);
            t += strlen(tmpbuf);
            continue;
        }

        if (lsInfo->resTable[entry].valueType == LS_STRING) {

            if(resReq[0] != '=')
                return(PARSE_BAD_EXP);

	    if ((token = getNextToken(&resReq)) == NULL)
                return(PARSE_BAD_EXP) ;


            if (validValue(token, lsInfo, entry) < 0)
                return (PARSE_BAD_VAL);

            sprintf(tmpbuf,"[%s \"eq\" \"%s\"]&&",lsInfo->resTable[entry].name,token);
            strcat(resVal->selectStr, tmpbuf);
            t += strlen(tmpbuf);
            continue;
        }
    }
    sprintf(tmpbuf,"1");
    strcat(resVal->selectStr, tmpbuf);

    resVal->options |= PR_SELECT;
    return(PARSE_OK);

}

static int
setDefaults(struct resVal *resVal, struct lsInfo *lsInfo, int options)
{
    int i;


    if (options & PR_DEFFROMTYPE)
        strcpy(resVal->selectStr, "expr [type \"eq\" \"local\"]");
    else
        strcpy(resVal->selectStr, "expr [type \"eq\" \"any\"]");



    resVal->nphase = 2;
    resVal->order[0] = R15S + 1;
    resVal->order[1] = PG + 1;
    resVal->val=malloc(lsInfo->nRes*sizeof(float));
    resVal->indicies=malloc((lsInfo->numIndx)*sizeof(int));
    if ((!resVal->val) || (!resVal->indicies)) {
	freeResVal (resVal);
        lserrno=LSE_MALLOC;
        ls_perror("intlib:resreq");
        return( PARSE_BAD_MEM );
    }

    for (i=0; i < lsInfo->nRes; i++)
        resVal->val[i] = INFINIT_LOAD;

    resVal->genClass =  0;
    if (!(options &PR_BATCH)) {
        resVal->genClass |=  1 << R15S;
        resVal->genClass |=  1 << R1M;
        resVal->genClass |=  1 << R15M;
        resVal->val[R15S] = 1.0;
        resVal->val[R1M]  = 1.0;
        resVal->val[R15M] = 1.0;
    }


    resVal->nindex = lsInfo->numIndx;
    for(i=0; i < resVal->nindex; i++)
        resVal->indicies[i] = i;

    resVal->rusgBitMaps =
        (int *)malloc (GET_INTNUM(lsInfo->nRes) * sizeof (int));
    if (resVal->rusgBitMaps == NULL) {
	lserrno=LSE_MALLOC;
	freeResVal (resVal);
        return (PARSE_BAD_MEM);
    }
    for (i = 0; i < GET_INTNUM(lsInfo->nRes); i++)
	resVal->rusgBitMaps[i] = 0;

    if (!(options &PR_BATCH)) {
        SET_BIT(R15S, resVal->rusgBitMaps);
        SET_BIT(R1M, resVal->rusgBitMaps);
        SET_BIT(R15M, resVal->rusgBitMaps);
    }

    resVal->duration = INFINIT_INT;
    resVal->decay = INFINIT_FLOAT;
    resVal->numHosts = INFINIT_INT;
    resVal->maxNumHosts = INFINIT_INT;
    resVal->pTile = INFINIT_INT;


    resVal->options = 0;
    return (0);
}

void
freeResVal (struct resVal *resVal)
{
    if (resVal == NULL)
	return;

    FREEUP (resVal->val);
    FREEUP (resVal->indicies);
    FREEUP (resVal->selectStr);
    resVal->selectStrSize = 0;
    FREEUP (resVal->rusgBitMaps);
    if (resVal->xorExprs) {
        int i;
	for (i=0; resVal->xorExprs[i]; i++)
	    FREEUP (resVal->xorExprs[i]);
	FREEUP (resVal->xorExprs);
    }
}

void
initResVal (struct resVal *resVal)
{
    if (resVal == NULL)
        return;

    resVal->val = NULL;
    resVal->indicies = NULL;
    resVal->rusgBitMaps= NULL;
    resVal->selectStr= NULL;
    resVal->selectStrSize= 0;
    resVal->xorExprs = NULL;

}


static int
getTimeVal(char **time, float *val)
{
    char *token, *cp, oneChar = '\0';

    token = getNextToken(time);
    cp = token;
    while (isdigit(*cp))
	cp++;

    if (*cp == 'm' || *cp == 'h' || *cp == 's') {
        oneChar = *cp;
        *cp = '\0';
    }
    if (!isanumber_(token))
        return -1;
    *val = atof(token);

    if (oneChar == 'h')
        *val *= 60 * 60;
    else if ((oneChar == 'm') || (oneChar == '\0')){
        *val *= 60;
    }
    return 0;
}

static int
getVal(char **resReq, float *val)
{
    char *token;

    token = getNextToken(resReq);
    if (!isanumber_(token))
        return -1;
    *val = atof(token);

    return 0;
}

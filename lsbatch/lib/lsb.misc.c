/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.misc.c 397 2007-11-26 19:04:00Z mblack $
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

#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <ctype.h>

#include "lsb.h"
#include "../../lsf/lib/lib.table.h"

#define NL_SETN     13   

#define MEMBERSTRLEN   20

void
initTab(struct hTab *tabPtr)
{
  
    if (tabPtr) {
        h_initTab_ (tabPtr, 50);    
    }

} 

hEnt *
addMemb(struct hTab *tabPtr, LS_LONG_INT member)
{
    char memberStr[MEMBERSTRLEN];
    hEnt *ent;
    int new;

    if (tabPtr) {
        sprintf (memberStr, LS_LONG_FORMAT, member);
        ent = h_addEnt_(tabPtr, memberStr, &new);
        if (!new) {
            return(NULL);
        }
        else
	    return(ent);
    }
    return(NULL);
} 

char
remvMemb(struct hTab *tabPtr, LS_LONG_INT member)
{
    char memberStr[MEMBERSTRLEN];
    hEnt *ent;

    if (tabPtr) {
        sprintf (memberStr,LS_LONG_FORMAT , member);
        if ((ent = h_getEnt_ (tabPtr, memberStr)) == NULL)
	    return(FALSE);
        else {
            ent->hData = NULL;  
	    h_delEnt_ (tabPtr, ent);
	    return(TRUE);
        }
    }
    return(FALSE);
} 

hEnt *
chekMemb(struct hTab *tabPtr, LS_LONG_INT member)
{
    char memberStr[MEMBERSTRLEN];
    hEnt *ent = NULL;

    if (tabPtr) {
        sprintf (memberStr, LS_LONG_FORMAT , member);
        ent = h_getEnt_ (tabPtr, memberStr);
    }

    return ent;
} 

hEnt *
addMembStr(struct hTab *tabPtr, char *memberStr)
{
    hEnt *ent;
    int new;

    if (tabPtr && memberStr) {
        ent = h_addEnt_(tabPtr, memberStr, &new);
        if (!new) {
            return(NULL);
        }
        else
	    return(ent);
    }
    return(NULL);
} 

char
remvMembStr(struct hTab *tabPtr, char *memberStr)
{
    hEnt *ent;

    if (tabPtr && memberStr) {
        if ((ent = h_getEnt_ (tabPtr, memberStr)) == NULL)
	    return(FALSE);
        else {
            ent->hData = NULL;  
	    h_delEnt_ (tabPtr, ent);
	    return(TRUE);
        }
    }
    return(FALSE);
} 

hEnt *
chekMembStr(struct hTab *tabPtr, char *memberStr)
{
    hEnt *ent = NULL;

    if (tabPtr && memberStr) {
        ent = h_getEnt_ (tabPtr, memberStr);
    }

    return ent;
} 

struct sortIntList *
initSortIntList(int increased)
{
    struct sortIntList *headerPtr;
    if ((headerPtr = (struct sortIntList *) malloc(sizeof (struct sortIntList)))
	== NULL) {
	return (NULL);
    }
    headerPtr->forw = headerPtr->back = headerPtr;
    headerPtr->value = increased;
    return(headerPtr);
} 

int
insertSortIntList(struct sortIntList *header, int value)
{
    struct sortIntList *listPtr;
    struct sortIntList *newPtr;

    listPtr = header->forw;
    while (listPtr != header) {
	 
	if (listPtr->value == value)
	    return(0);
        if (header->value) {
	    
	    if (listPtr->value > value)
	        break;
	    listPtr=listPtr->forw;
	} else {
	    
	    if (listPtr->value < value)
	        break;
	    listPtr=listPtr->forw;
	}
    }
    if ((newPtr = (struct sortIntList *) malloc(sizeof (struct sortIntList)))
	== NULL)
	return (-1);
    newPtr->forw = listPtr;
    newPtr->back = listPtr->back;
    listPtr->back->forw=newPtr;
    listPtr->back=newPtr;
    newPtr->value = value;
    return(0);

} 

struct sortIntList *
getNextSortIntList(struct sortIntList *header, 
    struct sortIntList *current, int *value)
{
    struct sortIntList *nextPtr;

    nextPtr = current->forw;
    if (nextPtr == header)
	return(NULL);
    *value = nextPtr->value;
    return(nextPtr);

} 

void
freeSortIntList(struct sortIntList *header)
{
    struct sortIntList *listPtr;
    struct sortIntList *nextPtr;

    listPtr = header->forw;
    while (listPtr != header) {
	nextPtr = listPtr->forw;
	free(listPtr);
	listPtr = nextPtr;
    }
    free(listPtr);
    return;

} 

int
getMinSortIntList(struct sortIntList *header, int *minValue)
{
    if (header == header->forw)
	return(-1);
    if (header->value)
	*minValue = header->forw->value;
    else
	*minValue = header->back->value;
    return(0);

} 

int
getMaxSortIntList(struct sortIntList *header, int *maxValue)
{

    if (header == header->forw)
	return(-1);
    if (header->value)
	*maxValue = header->back->value;
    else
	*maxValue = header->forw->value;
    return(0);

} 

int
getTotalSortIntList(struct sortIntList *header)
{
    struct sortIntList *listPtr;
    int total=0;

    listPtr=header->forw;
    while(listPtr != header) {
	total++;
	listPtr = listPtr->forw;
    }
    return(total);

} 

int
sndJobFile_(int s, struct lenData *jf)
{
    int nlen = htonl(jf->len);

    if (b_write_fix(s, NET_INTADDR_(&nlen), NET_INTSIZE_) != NET_INTSIZE_) {
	lsberrno = LSBE_SYS_CALL;	
	return (-1);
    }

    if (b_write_fix(s, jf->data, jf->len) != jf->len) {
	lsberrno = LSBE_SYS_CALL;
	return (-1);
    }

    return (0);
} 

/* 
 * Pack all job files into a single lenData structure
 * Format: [file_count(NET_INTSIZE_ bytes)] [file1_len(NET_INTSIZE_ bytes)] [file1_data] [file2_len(NET_INTSIZE_ bytes)] [file2_data] ...
 */
int 
packAllJobFiles(struct lenData *jf_array, int job_count, struct lenData *packed)
{
    int totalSize = NET_INTSIZE_;  /* NET_INTSIZE_ bytes to store file count */
    int offset = 0;
    int nlen;
    int i;
    
    if (!jf_array || !packed || job_count <= 0) {
        return -1;
    }
    
    /* Calculate total size */
    for (i = 0; i < job_count; i++) {
        totalSize += NET_INTSIZE_;  /* NET_INTSIZE_ bytes to store each file length */
        totalSize += jf_array[i].len;  /* File data */
    }
    
    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "packAllJobFiles: Packing %d job files, total size: %d bytes", 
                  job_count, totalSize);
    
    /* Allocate memory */
    packed->len = totalSize;
    packed->data = malloc(totalSize);
    if (!packed->data) {
        ls_syslog(LOG_ERR, "packAllJobFiles: Memory allocation failed for %d bytes", totalSize);
        return -1;
    }
    
    /* Write file count (network byte order) */
    nlen = htonl(job_count);
    memcpy(packed->data + offset, &nlen, NET_INTSIZE_);
    offset += NET_INTSIZE_;
    
    /* Write each file */
    for (i = 0; i < job_count; i++) {
        /* Write file length (network byte order) */
        nlen = htonl(jf_array[i].len);
        memcpy(packed->data + offset, &nlen, NET_INTSIZE_);
        offset += NET_INTSIZE_;
        
        /* Write file data */
        if (jf_array[i].len > 0 && jf_array[i].data) {
            memcpy(packed->data + offset, jf_array[i].data, jf_array[i].len);
            offset += jf_array[i].len;
            if (logclass & LC_TRACE)
                ls_syslog(LOG_DEBUG, "packAllJobFiles: Packed job %d file (%d bytes)", 
                          i+1, jf_array[i].len);
        } else {
            if (logclass & LC_TRACE)
                ls_syslog(LOG_DEBUG, "packAllJobFiles: Warning - Job %d file length is 0", i+1);
        }
    }
    
    if (logclass & (LC_TRACE | LC_EXEC))
        ls_syslog(LOG_DEBUG, "packAllJobFiles: All job files packed successfully! Total size: %d bytes", 
                  packed->len);
    
    return 0;
}



 
void
upperStr(char *in, char *out)
{
    for (; *in != '\0'; in++, out++)
	*out = toupper(*in);
    *out = '\0';
}


void
copyJUsage(struct jRusage *to, struct jRusage *from)
{
    struct pidInfo *newPidInfo;
    int *newPgid;

    to->mem = from->mem;
    to->swap = from->swap;
    to->utime = from->utime;
    to->stime = from->stime;


    
    if (from->npids) {
        newPidInfo = (struct pidInfo *)
                      calloc(from->npids, sizeof(struct pidInfo));
        if (newPidInfo != NULL) { 
            if (to->npids) 
                FREEUP (to->pidInfo);  
            to->pidInfo = newPidInfo;  
            to->npids = from->npids;
            memcpy((char *) to->pidInfo, (char *) from->pidInfo,
                            from->npids * sizeof(struct pidInfo));
        }
    } else if (to->npids) {
        FREEUP (to->pidInfo);
        to->npids = 0;
    }

    
    if (from->npgids) {
        newPgid = (int *) calloc(from->npgids, sizeof(int));
        if (newPgid == NULL) return;   
        if (to->npgids)
            FREEUP (to->pgid);
        to->pgid = newPgid;
        to->npgids = from->npgids; 
        memcpy((char *) to->pgid, (char *) from->pgid,
                        from->npgids * sizeof(int));
    } else if (to->npgids) {
        FREEUP (to->pgid);
        to->npgids = 0;
    }
} 

void
convertRLimit(int *pRLimits, int toKb)
{
    int i;

    for (i = 0; i < LSF_RLIM_NLIMITS; i++) {
	switch (i) {
	    case LSF_RLIMIT_FSIZE:
	    case LSF_RLIMIT_DATA:
//	    case LSF_RLIMIT_STACK:
//	    case LSF_RLIMIT_CORE:
//	    case LSF_RLIMIT_RSS:
//	    case LSF_RLIMIT_VMEM:
		if (pRLimits[i] > 0) {
		    if (toKb) {
			pRLimits[i] /= 1024;
		    } else {
			pRLimits[i] *= 1024;
		    }
		}
		break;
	}
    }
} 

int
limitIsOk_(int *rLimits)
{
#define EXCEED_MAX_INT(x) ( (x) > 0 ? (unsigned int)(x) >> 21 : 0 )

    if ( EXCEED_MAX_INT(rLimits[LSF_RLIMIT_FSIZE])
	 || EXCEED_MAX_INT(rLimits[LSF_RLIMIT_DATA])
	 || EXCEED_MAX_INT(rLimits[LSF_RLIMIT_STACK])
	 || EXCEED_MAX_INT(rLimits[LSF_RLIMIT_CORE])
	 || EXCEED_MAX_INT(rLimits[LSF_RLIMIT_RSS])
	 || EXCEED_MAX_INT(rLimits[LSF_RLIMIT_SWAP]) ) {
	return 0;
    } else {
	return 1;
    }
} 

char *
lsb_splitName(char *str, unsigned int *number)
{
    static char fname[]="lsb_splitName";
    static char name[4*MAXLINELEN];  
    static int  nameNum;              
    int twoPartFlag;
    int i, j;

    
    if (str == NULL || number == NULL) {
        ls_syslog(LOG_ERR, I18N(5650,"%s: bad input.\n"), fname); /* catgets 5650 */
        return NULL;
    }

    
    twoPartFlag = 0;
    j = 0;

    
    for (i=0; i<strlen(str); i++) {
        if (str[i] != '*') {
            
            name[j] = str[i];
            j++;
        }
        else { 
            
            twoPartFlag = 1;
            name[j] = '\0';
            j = 0;

            
            sscanf(name, "%d", &nameNum);

            
            if (nameNum <= 0) {
                nameNum = 1;
                ls_syslog(LOG_ERR, I18N(5651, "%s: bad input format.  Assuming 1 host.\n"), /* catgets 5651 */
			  fname); 
            }
        }
    } 

    
    name[j] = '\0';

    
    if (twoPartFlag == 0 || j == 0) {
        
        
        nameNum = 1;
    }
    
    *number = nameNum;
    return name;
} 

NAMELIST *
lsb_compressStrList(char **strList, int numStr)
{
    static char   fname[]="lsb_compressStrList";
    static NAMELIST nameList;
    int    i;
    int    hIndex=0;
    int    numSameStr; 
    int    headPtr;    

    
    if (nameList.names != NULL) {
         for(i=0; i < nameList.listSize; i++)
             FREEUP(nameList.names[i]);
         FREEUP(nameList.names);
         FREEUP(nameList.counter);
    }

    nameList.listSize = 0;
    nameList.names    = NULL;
    nameList.counter  = NULL;

    
    if ( numStr <= 0 ) {
        return (NAMELIST *)&nameList;
    }

    
    if (strList == NULL)
      return NULL;

    nameList.names   = (char **)calloc(numStr, sizeof(char *));
    nameList.counter = (int *)calloc(numStr,  sizeof(int));

    if (!nameList.names || !nameList.counter)  {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
        return (NAMELIST *)NULL;
    }
    headPtr = 0;
    numSameStr = 1;
    for (i = 1; i < numStr; i++)
        if ( strList[i]) {  

            if (strcmp(strList[i], strList[headPtr])== 0) {
                
                numSameStr++;
                 continue;
             }
             nameList.names[hIndex] = putstr_(strList[headPtr]);
             headPtr = i;
             nameList.counter[hIndex] = numSameStr;
             hIndex++;
             numSameStr = 1;
        }

    nameList.names[hIndex] = putstr_(strList[headPtr]);
    nameList.counter[hIndex] = numSameStr;
    hIndex++;
    nameList.listSize = hIndex;

    
    nameList.names = (char **)realloc(nameList.names,
                                        nameList.listSize*sizeof(char *));
    nameList.counter  = (int *)realloc(nameList.counter,
                                        nameList.listSize*sizeof(int));

    if (!nameList.names || !nameList.counter)  {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "realloc");
	for(i=0; i < nameList.listSize; i++)
	    FREEUP(nameList.names[i]);
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
	nameList.listSize=0;
        return (NAMELIST *)NULL;
    }

    return (NAMELIST *)&nameList;
} 


char *
lsb_printNameList(NAMELIST *nameList, int format)
{
    static   char fname[]="lsb_printNameList";
    unsigned long namelen;    
    char     *buf=NULL;       
    int      buflen;          
    static   char *namestr=NULL;  
    unsigned long allocLen=0; 
    int      i, j;

    
    if (nameList == NULL) {
        ls_syslog(LOG_ERR, I18N(5652, "%s: NULL input.\n"), fname); /* catgets 5652 */
	return NULL;
    }

    if ( namestr != NULL) { 
        FREEUP(namestr);
    }

    allocLen = MAXLINELEN;
    namestr = (char *) calloc(allocLen, sizeof(char));

    if ( !namestr) {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        return (char *)NULL;
    }

    for(i=0; i < nameList->listSize; i++) {
        
        if ( format == PRINT_SHORT_NAMELIST ) {
            buf = (char *)calloc(MAXLINELEN, sizeof(char));
            if (!buf) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
                return NULL;
            }
            sprintf(buf, "%d*%s ", nameList->counter[i], nameList->names[i]);
            buflen = strlen(buf);
        }
        else if ( format == PRINT_MCPU_HOSTS) {
            buf = (char *)calloc(MAXLINELEN, sizeof(char));
            if (!buf) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
                return NULL;
            }
            sprintf(buf, "%s %d ", nameList->names[i], nameList->counter[i]);
            buflen = strlen(buf);
        }
        else { 
            namelen = strlen(nameList->names[i]);
            buflen  = (namelen + 1) * nameList->counter[i] + 1; 
            buf = (char *)calloc(buflen, sizeof(char));
            if (!buf) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
                return NULL;
            }
            for(j=0; j < nameList->counter[i]; j++)
                sprintf(buf+j*(namelen+1), "%s ", nameList->names[i]);
        }

        
        if (buflen + strlen(namestr) >= allocLen ) {
            allocLen +=  buflen + MAXLINELEN;
            namestr = (char *) realloc(namestr, allocLen * sizeof(char));
            if ( !namestr) {
		ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "realloc");
                return NULL;
            }
        }
        strcat(namestr, buf);
	FREEUP(buf);
    } 
    return namestr;
} 


NAMELIST *
lsb_parseLongStr(char *string)
{
    static char   fname[]="lsb_parseLongStr";
    static NAMELIST nameList;
    unsigned long numStr = strlen(string)/2 + 1; 
    char  *prevStr, *curStr;
    unsigned long numSameStr;
    int   i;

    
    if (string == NULL || strlen(string) <= 0) {
        ls_syslog(LOG_ERR, I18N(5653, "%s: bad input"), fname); /* catgets 5653 */
        return (NAMELIST *)NULL;
    }

    
    if (nameList.names != NULL) {
         for(i=0; i < nameList.listSize; i++)
             FREEUP(nameList.names[i]);
         FREEUP(nameList.names);
         FREEUP(nameList.counter);
    }

    nameList.listSize = 0;
    nameList.names   = (char **)calloc(numStr, sizeof(char *));
    nameList.counter = (int *)calloc(numStr,  sizeof(int));

    if (!nameList.names || !nameList.counter)  {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
        return (NAMELIST *)NULL;
    }

    
    numSameStr = 1;
    prevStr = putstr_(getNextWord_(&string));

    
    if (strlen(prevStr) <= 0) {
        ls_syslog(LOG_ERR, I18N(5654, "%s: blank input\n"), /* catgets 5654 */
		fname);
        FREEUP(prevStr);
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
        return (NAMELIST *)&nameList;
    }

    
    while ( (curStr = getNextWord_(&string)) != NULL) {
        if (strcmp(curStr, prevStr)) {

            
            if ( nameList.listSize == numStr ) { 
                ls_syslog(LOG_ERR, I18N(5655, "%s: list exceeded allocated memory (shouldn't happen)\n"), /* catgets 5655 */
                      fname); 
                return (NAMELIST *)NULL;
            }

            
            nameList.names[nameList.listSize] = prevStr;
            nameList.counter[nameList.listSize] = numSameStr;
            nameList.listSize++;
            numSameStr = 1;
            prevStr = putstr_(curStr);
        } else { 
            numSameStr++;
        }
    } 

    
    nameList.names[nameList.listSize] = prevStr;
    nameList.counter[nameList.listSize] = numSameStr;
    nameList.listSize++;

    
    nameList.names = (char **)realloc(nameList.names,
                                        nameList.listSize * sizeof(char *));
    nameList.counter = (int *)realloc(nameList.counter,
                                      nameList.listSize * sizeof(int));

    if (!nameList.names || !nameList.counter)  {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "realloc");
	for(i=0; i < nameList.listSize; i++)
	    FREEUP(nameList.names[i]);
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
	nameList.listSize=0;
        return (NAMELIST *)NULL;
    } 
    return (NAMELIST *)&nameList;

} 


NAMELIST *
lsb_parseShortStr(char *string, int format)
{
    static char   fname[]="lsb_parseShortStr";
    static NAMELIST nameList;
    unsigned long numStr = strlen(string)/2 + 1; 
    unsigned int  numSameStr;
    char namestr[4*MAXLINELEN];
    char *name;
    char *curStr;
    int     i;

    
    if (string == NULL || strlen(string) <= 0) {
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
        return (NAMELIST *)NULL;
    }

    if (nameList.names != NULL) {
         for(i=0; i < nameList.listSize; i++)
             free(nameList.names[i]);
         free(nameList.names);
         free(nameList.counter);
    }

    nameList.listSize = 0;
    nameList.names   = (char **)calloc(numStr, sizeof(char *));
    nameList.counter = (int *)calloc(numStr,  sizeof(int));

    if (!nameList.names || !nameList.counter)  {
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "calloc");
        FREEUP(nameList.names);
        FREEUP(nameList.counter);
        return (NAMELIST *)NULL;
    }

    curStr = getNextWord_(&string);

    while (curStr != NULL) {
        
        if (nameList.listSize >= numStr) {
            ls_syslog(LOG_ERR, I18N(5656, "%s: list exceeded allocated memory (shouldn't happen)\n"), /* catgets 5656 */
                      fname); 
            return (NAMELIST *)NULL;
        }
        
        if (format == PRINT_MCPU_HOSTS) {
            sprintf(namestr, "%s", curStr);
            name = (char *)namestr;
            if ((curStr = getNextWord_(&string)) == NULL) {
                ls_syslog(LOG_ERR, I18N(5657, "%s: LSB_MCPU_HOSTS format error\n"), /* catgets 5657 */
			  fname); 
                FREEUP(nameList.names);
                FREEUP(nameList.counter);
                return (NAMELIST *)NULL;
            }
            numSameStr = atoi(curStr);
        }
        else
            name = lsb_splitName(curStr, &numSameStr);
        if (name != NULL) {
            nameList.names[nameList.listSize] = putstr_(name);
            nameList.counter[nameList.listSize] = numSameStr;
            nameList.listSize++;
        }
        
        curStr = getNextWord_(&string);
   } 

   
   nameList.names = (char **)realloc(nameList.names,
                                     nameList.listSize * sizeof(char *));
   nameList.counter = (int *)realloc(nameList.counter,
                                     nameList.listSize * sizeof(int));
   
   if (!nameList.names || !nameList.counter)  {
       ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "realloc");
       for(i=0; i < nameList.listSize; i++)
           FREEUP(nameList.names[i]);
       FREEUP(nameList.names);
       FREEUP(nameList.counter);
       nameList.listSize=0;
       return (NAMELIST *)NULL;
   }

   return (NAMELIST *)&nameList;
  
} 

char*
getUnixSpoolDir( char *spoolDir )
{
    char* pTemp = NULL;
    if((pTemp = strchr (spoolDir, '|')) != NULL){
        int i = 0; 
	*pTemp = '\0';
        while (isspace(*(--pTemp)))
            i++; 
	if (i != 0) {
            *(++pTemp) = '\0';
        }
        pTemp = spoolDir;
    } else if ( spoolDir[0] == '/') {
	pTemp = spoolDir;
    } else {
        pTemp = NULL;
    }
    return pTemp;

} 

char*
getNTSpoolDir( char *spoolDir )
{
    char* pTemp = NULL;
    if((pTemp = strchr (spoolDir, '|')) != NULL){
	++pTemp;
        
       while ((pTemp[0] != '\0') && (pTemp[0] == ' ')) {
           ++pTemp; 
       }
    } else if ( (spoolDir[0] == '\\') || (spoolDir[1] == ':')) { 
        pTemp = spoolDir;
    } else {
	pTemp = NULL;
    }
    return pTemp;

} 

void
jobId64To32 (LS_LONG_INT interJobId, int* jobId, int* jobArrElemId)
{
    *jobArrElemId = LSB_ARRAY_IDX(interJobId); 
    *jobId = LSB_ARRAY_JOBID(interJobId); 
}



void
jobId32To64(LS_LONG_INT* interJobId, int jobId, int jobArrElemId)
{
    *interJobId =  LSB_JOBID(jobId, jobArrElemId) ;
}


int
supportJobNamePattern(char * jobname)
{
   char * p , * q;

   p = jobname;
   while ( (p != NULL) &&
           ( (q=strchr(p, '*')) != NULL) ) {
        q++;
        p = q;

        
        if (*q == '\0' || *q == '/' || *q == '*')
                continue;
        return -1;
   }

   return 0;
}


/*traverse fairshare tree in pre-order way*/
int traverseSATree(struct shareAcctInfoEnt * node, int op, int (*func)(), ...) {
    va_list valist;
    int i;
    void *xdrs, *hdr;
    int *size;    

    if (!node || !func) {
        return TRUE;
    }

    va_start(valist, func);

    switch(op){
        case TRAVERSE_FS_SIZE:
            size = va_arg(valist, int *);
            if (!(*func)(node, size)) /*use the first one*/
                return (FALSE);
            for (i = 0; i < node->nChildShareAcct; i++) {
                if (! traverseSATree(&(node->childShareAccts[i]), TRAVERSE_FS_SIZE, func, size)) {
                    return FALSE;
                }
            }
            break;
        case TRAVERSE_FS_SACCTXDR:
            xdrs = va_arg(valist, void *);
            hdr = va_arg(valist, void *);
            if (!(*func)(node, xdrs, hdr)) {
                return(FALSE);
            }
            for (i = 0; i < node->nChildShareAcct; i++) {
                if (! traverseSATree(&(node->childShareAccts[i]), TRAVERSE_FS_SACCTXDR, func, xdrs, hdr)) {
                    return FALSE;
                }
            }
            break;
        default:
            break;
    }

    return TRUE;
}





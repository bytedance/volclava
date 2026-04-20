/* $Id: mbd.requeue.c 397 2007-11-26 19:04:00Z mblack $
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
#include <string.h>
#include "mbd.h"

#define NL_SETN		10	

static int numAlloc;
static char *a_getNextWord_(char **string);
static int fill_struct(struct requeueEStruct **rquest, int seqnum, int value,
		      char mode);

int
requeueEParse (struct requeueEStruct **rquest, char *reqstr, int *error)
{
    int numStruct, exitV;
    char mode, *word;
    
    numAlloc =16;
    *error =0;
    mode = RQE_NORMAL;
    
    if (!(*rquest=(struct requeueEStruct *)
	malloc(numAlloc*sizeof (struct requeueEStruct))))
	return 0;
    (*rquest)[0].type = RQE_END; 
    for (numStruct = 0;;) {
	if (!(word = a_getNextWord_(&reqstr)))
	    break;
	if (isint_(word)) {
	    if ((exitV=my_atoi(word,256,-1))!=INFINIT_INT) {
		if (!fill_struct (rquest, numStruct, exitV, mode))
		
		    numStruct ++;
		else
		    *error =1; 
	    }
	    else
		*error =1; 
	}
	else if (strncasecmp(word,"EXCLUDE(",8)==0)
	    mode = RQE_EXCLUDE;
	else if (*word==')')
	    mode = RQE_NORMAL; 
    }
    if (numStruct)
	*rquest=(struct requeueEStruct *)
	    realloc(*rquest, (numStruct+1)*sizeof (struct requeueEStruct));
    return numStruct;
}

static char
*a_getNextWord_(char **line)
{
    char *wordp, *word;
    if (!(word = getNextWord_(line)))
        return NULL;
    if ((wordp=strchr(word,'('))) {
	*(wordp+1)='\0';
	while (**line!='(') (*line)--;
	(*line)++;
    }
    else if ((wordp=strchr(word, ')'))) {
	if (wordp!=word) {
	    wordp--;
	    while (isspace(*wordp)) wordp--;
	    *(wordp+1)='\0';
	    while (**line!=')') (*line)--;
	}
    }
    return word;
}

static int
fill_struct(struct requeueEStruct **rquest, int seqnum, int value, char mode)
{
    int i;
    for (i=0; i< seqnum; i++)
	if ((*rquest)[i].value == value) 
	    return -1;
    if (seqnum >= numAlloc-1) {
	int newAlloc = numAlloc << 1;
	struct requeueEStruct *tmp = (struct requeueEStruct *)
	    realloc(*rquest, newAlloc*sizeof (struct requeueEStruct));
		if (!tmp) {
		    return -1;
		}
		*rquest = tmp;
		numAlloc = newAlloc;
	    }
    (*rquest)[seqnum].value = value;
    (*rquest)[seqnum].type = mode;
    (*rquest)[seqnum].interval = 1;
    (*rquest)[seqnum+1].type = RQE_END;   
    return 0;
}

int
match_exitvalue (struct requeueEStruct *reqE, int exit_value)
{
    for (; reqE->type!= RQE_END; reqE++) 
	
	if (reqE->value == exit_value)
	    return (int)reqE->type;
    return -1;
}

int fill_requeueHist (struct rqHistory **reqHistory, int *reqHistAlloc,
		      struct hData *host)
{
    int i;
    struct rqHistory *temp;

    
    for (i=0; (*reqHistory)[i].host!=NULL; i++)
        if ((*reqHistory)[i].host==host) {   
	    (*reqHistory)[i].retry_num++;
	    return i;
	}
    
    if (i+1 >= *reqHistAlloc) { 
	*reqHistAlloc <<= 1;  
	if (!( temp = (struct rqHistory *)realloc (*reqHistory,
	              (*reqHistAlloc)*sizeof (struct rqHistory)))) {
	    *reqHistAlloc >>= 1; 
            return -1;
	} else
	   (*reqHistory) = temp;
    }

    (*reqHistory)[i].host=host;
    (*reqHistory)[i].retry_num =1;
    (*reqHistory)[i+1].host = NULL;
    return i;
}

void	
clean_requeue (struct qData* qp) 
{
    FREEUP(qp->requeEStruct);
}

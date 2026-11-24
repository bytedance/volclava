/* $Id: lib.wconf.c 397 2007-11-26 19:04:00Z mblack $
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
#include <pwd.h>
#include <grp.h>
#include "lib.h"
#include "lproto.h"
#include "lib.words.h"
#include <ctype.h>
#include <stdio.h>

#define NL_SETN   23   

struct lsConf * 
ls_getconf ( char *fname )
{
    FILE   		*fp=NULL;
    struct lsConf 	*conf;
    struct confNode 	*rootNode = NULL;
    struct confNode 	*temp, *node, *prev = NULL;
    int 		beginLineNum, numLines, oldLineNum, lineNum = 0;
    char 		*linep, *sp, *word, *word1, *cp, **lines, *ptr;
    char 		flag, **tmpPtr, **defNames = NULL, **defConds = NULL;
    int			i, len, numDefs = 0, defsize;
    long        	offset;

    lserrno = LSE_NO_ERR;
    if (fname == NULL) {
	ls_syslog(LOG_ERR, "%s: %s.", "ls_getconf", 
		  I18N(6000, "Null filename")); /* catgets 6000 */
	lserrno = LSE_NO_FILE;
	return (NULL);
    }

    conf = (struct lsConf *) malloc (sizeof(struct lsConf));
    if (conf == NULL) {
	ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "ls_getconf", 
		  "malloc", sizeof (struct lsConf));
	lserrno = LSE_MALLOC;
	return (NULL);
    }

    conf->confhandle = NULL;	        
    conf->numConds = 0;
    conf->conds = NULL;
    conf->values = NULL;
    blockStack = initStack();
    if (blockStack==NULL) {		
	ls_freeconf(conf);
	lserrno = LSE_MALLOC;
	return (NULL);
    } 
    ptrStack = initStack();	
    if (ptrStack==NULL) {		
    	freeStack(blockStack);
	ls_freeconf(conf);
	lserrno = LSE_MALLOC;
	return (NULL);
    }
    defsize = 5;
    defNames = (char **) malloc (defsize*sizeof(char *));
    if (defNames == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "ls_getconf",  
			   "malloc", sizeof (defsize*sizeof(char *)));
	lserrno = LSE_MALLOC;
   	goto Error;
    }
    defConds = (char **) malloc (defsize*sizeof(char *));
    if (defConds == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "ls_getconf",
		   "malloc", sizeof (defsize*sizeof(char *)));
	lserrno = LSE_MALLOC;
   	goto Error;
    }		

    fp = fopen(fname, "r");
    if (fp == NULL) {

	ls_syslog(LOG_ERR, "%s: %s <%s>.", "ls_getconf", 
		  I18N(6001,  "Can't open configuration file"),/* catgets 6001*/
		  fname);
    	lserrno = LSE_NO_FILE;     
	goto Error;
    }

    while ((linep = getNextLineD_(fp, &lineNum, FALSE)) != NULL) {
					
	sp = linep;
        word = getNextWord_(&linep);	
	if (word && word[0]=='#') {
				
	    cp = word;
	    cp++;

	    if (strcasecmp(cp, "define") == 0) {
						
		word = getNextWord_(&linep);
					
		if (word == NULL) {
		    ls_syslog(LOG_ERR, "%s: %s(%d): %s.", "ls_getconf", 
			      fname, lineNum, 
			      I18N(6002, "Both macro and condition name expected after #define")); /* catgets 6002 */ 
		    goto Error;
		}

		word1 = putstr_(word);
	        if (word1 == NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "ls_getconf", 
			      "malloc", strlen(word)+1);
		    lserrno = LSE_MALLOC;
		    goto Error;
	        }

		while (isspace(*linep))	
		    linep++;
		word = linep;

		if (*word != '\0') {
		    if ((ptr = strchr(word, '#')) != NULL)
			*ptr = '\0';

		    i = strlen(word) -1;
		    while (isspace(word[i]))    
		        i--;
		    word[i+1] = '\0';
		}

		if (*word == '\0') {
		    ls_syslog(LOG_ERR, "ls_getconf: %s(%d): %s",
			      fname, lineNum,
			      I18N(6003, "Both macro and condition name expected after #define.")); /* catgets 6003 */
		    FREEUP(word1);
		    goto Error;
		}

		for (i = 0; i < numDefs; i ++) {
		    if (!strcmp(defNames[i], word))
			break;
		}		

		if (i < numDefs) 
		    word = defConds[i];
				

		if (numDefs == defsize) {	
		    tmpPtr = (char **) myrealloc (defNames,
				defsize*2*sizeof(char *));
		    if (tmpPtr != NULL)
			defNames = tmpPtr;
		    else {
		        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
				  "realloc", defsize*2*sizeof(char *));
			FREEUP(word1);
			lserrno = LSE_MALLOC;
		        goto Error;
		    }
		    tmpPtr = (char **) myrealloc (defConds,
				defsize*2*sizeof(char *));
		    if (tmpPtr != NULL)
			defConds = tmpPtr;
		    else {
		        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
				  "realloc", defsize*2*sizeof(char *));
			FREEUP(word1);
			lserrno = LSE_MALLOC;
		        goto Error;
		    }
		    defsize *= 2;
		}
		defNames[numDefs] = putstr_(word1);	
	        if (defNames[numDefs] == NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf", 
			      "malloc", strlen(word1)+1);
		    FREEUP(word1);
		    lserrno = LSE_MALLOC;
		    goto Error;
	        }
		defConds[numDefs] = putstr_(word);
	        if (defConds[numDefs] == NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
			       "malloc", strlen(word)+1);
		    FREEUP(defNames[numDefs]);
		    FREEUP(word1);
		    lserrno = LSE_MALLOC;
		    goto Error;
	        }
		numDefs ++;

		FREEUP(word1);
		continue;
	    } else if (strcasecmp(cp, "if") == 0) {
						
		while (isspace(*linep))		
		    linep++;
		word = linep;

		if (*word != '\0') {
		    if ((ptr = strchr(word, '#')) != NULL)
			*ptr = '\0';

		    i = strlen(word) -1;
		    while (isspace(word[i]))	
		        i--;
		    word[i+1] = '\0';
	        }

		if (*word == '\0') {
		    ls_syslog(LOG_ERR, "ls_getconf: %s(%d): %s.", fname, 
		        lineNum, I18N(6004, "Condition name expected after #if.")); /* catgets 6004 */
		    goto Error;
		}

                if ((node=newNode()) == NULL) {	
		    ls_syslog(LOG_ERR,  I18N_FUNC_D_FAIL, "ls_getconf",
				"malloc", sizeof(struct confNode));
		    lserrno = LSE_MALLOC;
                    goto Error;
		}

		for (i = 0; i < numDefs; i ++) {
		    if (!strcmp(defNames[i], word))
			break;
		}		

		if (i < numDefs) {
				
		    flag = addCond(conf, defConds[i]);
		    node->cond = putstr_(defConds[i]);
		} else {
		    flag = addCond(conf, word);
		    node->cond = putstr_(word);	
		}
	        if (!flag || node->cond==NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
			      "malloc", sizeof(word));
		    lserrno = LSE_MALLOC;
		    goto Error;
	        }

		if (prev != NULL)
		    linkNode ( prev, node );	
 	        prev = node;			
		PUSH_STACK(blockStack, node);	
		PUSH_STACK(ptrStack, node);	
		
		if (rootNode == NULL)		
		    rootNode = node;		

 		continue;
	    } else if (strcasecmp(cp, "elif") == 0) {
						

			
		temp = popStack(blockStack);
		if (temp == NULL) {
		    ls_syslog(LOG_ERR,I18N(6007,
		    "ls_getconf: %s(%d): If-less elif."),fname, lineNum); /*catgets 6007*/
		    goto Error;
		}
		PUSH_STACK(blockStack, temp);

		while (isspace(*linep))		
		    linep++;
		word = linep;

		if (*word != '\0') {
		    if ((ptr = strchr(word, '#')) != NULL)
			*ptr = '\0';

		    i = strlen(word) -1;
		    while (isspace(word[i]))	
		    	i--;
		    word[i+1] = '\0';
		}

		if (*word == '\0') {
		    ls_syslog(LOG_ERR, "ls_getconf: %s(%d): %s.", 
			fname, lineNum, 
			I18N(6005, "Condition name expected after #elif.")); /* catgets 6005 */
		    goto Error;
		}

		if ((node=newNode()) == NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
			       "malloc", sizeof(struct confNode));
		    lserrno = LSE_MALLOC;
		    goto Error;
		}

		for (i = 0; i < numDefs; i ++) {
		    if (!strcmp(defNames[i], word))
			break;
		}		

		if (i < numDefs) {
				
		    flag = addCond ( conf, defConds[i] );
		    node->cond = putstr_(defConds[i]);
		} else {
		    flag = addCond ( conf, word );
		    node->cond = putstr_(word);
		}
	        if (!flag || node->cond==NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_FAIL, "ls_getconf", 
			      "malloc");
		    lserrno = LSE_MALLOC;
		    goto Error;
	        }

		prev = popStack(ptrStack);
		prev->tag = NODE_LEFT_DONE;
		if (prev != NULL)
		    linkNode(prev, node);
		prev = node;
		PUSH_STACK(ptrStack, node);

		continue;
	    } else if (strcasecmp(cp, "else") == 0) {
						
		temp = popStack(blockStack);
		if (temp == NULL) {
		    ls_syslog(LOG_ERR,I18N(6008, 
		    "ls_getconf: %s(%d): If-less else."), /*catgets 6008*/
		    fname, lineNum);
		    goto Error;
		}
		PUSH_STACK(blockStack, temp);

		prev = popStack(ptrStack);
		prev->tag = NODE_LEFT_DONE;
		PUSH_STACK(ptrStack, prev);	
						
		continue;
	    } else if (strcasecmp(cp, "endif") == 0) {
						
		temp = popStack(blockStack);
		if (temp == NULL) {
		    ls_syslog(LOG_ERR, I18N(6009,
		    "ls_getconf: %s(%d): If-less endif."), fname, lineNum); /* catgets 6009*/
		    goto Error;
		}
		PUSH_STACK(blockStack, temp);

		prev = popStack(blockStack);
		popStack(ptrStack);
				
     		prev->tag = NODE_ALL_DONE;	
				
		continue;
	    }
	} 
					
 	beginLineNum = lineNum;
   	numLines = 0;
   	lines = NULL;
	for (;;) {
	    lines = (char **) myrealloc (lines, (numLines+1)*sizeof(char*));
	    if (lines == NULL) {
		ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
			  "malloc", (numLines+1)*sizeof(char*));
		lserrno = LSE_MALLOC;
		goto Error;
	    }
            lines[numLines] = (char *) malloc ((strlen(sp)+1)*sizeof(char));
	    if (lines[numLines] == NULL) {
		ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
		          "malloc", (strlen(sp)+1)*sizeof(char));
	        lserrno = LSE_MALLOC;
		goto Error;
	    }
	    strcpy(lines[numLines], sp);
	    numLines++;
	    offset = ftell(fp);	
	    oldLineNum = lineNum;
	    linep = getNextLineD_(fp, &lineNum, FALSE);
	    if (linep == NULL) {
	        if ((node=newNode()) == NULL) {
		    ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
			      "malloc", sizeof(struct confNode));
		    lserrno = LSE_MALLOC;
	  	    goto Error;
		}
		break;
	    }

	    sp = linep;
            word = getNextWord_(&linep); 

	    if (word && word[0]=='#') {

		cp = word;
		cp++;

	        if (strcasecmp(cp, "define")==0 
		            || strcasecmp(cp, "if")==0 
		            || strcasecmp(cp, "elif")==0 
			    ||  strcasecmp(cp, "else")==0
			    ||  strcasecmp(cp, "endif")==0 ){
	            fseek ( fp, offset, SEEK_SET );
		    lineNum = oldLineNum;
				

		    if ((node=newNode()) == NULL) {
		        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, 
			       "ls_getconf", "malloc", 
			       sizeof(struct confNode));
			lserrno = LSE_MALLOC;
		        goto Error;
		    }
		    break;
		}
 	    }
        }

	node->beginLineNum = beginLineNum;
	node->numLines = numLines;
	node->lines = lines;
	if (prev != NULL)
	    linkNode(prev, node);
 	prev = node;

  	if (rootNode == NULL)
	    rootNode = node;
    }

    temp = popStack(blockStack);
    if (temp != NULL) {
    	ls_syslog(LOG_ERR, "ls_getconf: %s(%d): %s endif.", fname, lineNum, I18N(6006, "Missing")); /* catgets 6006 */
        goto Error;
    }

    for ( i = 0; i < numDefs; i ++ ) {
	FREEUP(defNames[i]);
	FREEUP(defConds[i]);
    }
    FREEUP(defNames);
    FREEUP(defConds);
    freeStack(blockStack);
    freeStack(ptrStack);
    conf->confhandle = (struct confHandle *) malloc(sizeof(struct confHandle));
    if (conf->confhandle == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
		  "malloc", sizeof(struct confHandle));
    	fclose (fp);
	lserrno = LSE_MALLOC;
	return (NULL);
    }
    len = strlen(fname);
    conf->confhandle->rootNode = rootNode;
    conf->confhandle->fname = (char *) malloc((len+1) * sizeof(char));
    if (conf->confhandle->fname == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf",
		  "malloc", (len+1) * sizeof(char));
    	fclose (fp);
	lserrno = LSE_MALLOC;
	return (NULL);
    }
    strcpy ( conf->confhandle->fname, fname );
    conf->confhandle->curNode = rootNode;
    conf->confhandle->lineCount = 0;
    conf->confhandle->ptrStack = initStack();	
    fclose (fp);
    return (conf);

Error:
    for ( i = 0; i < numDefs; i ++ ) {
		if (defNames) FREEUP(defNames[i]);
		if (defConds) FREEUP(defConds[i]);
    }
    FREEUP(defNames);
    FREEUP(defConds);
    freeStack(blockStack);
    freeStack(ptrStack);
    conf->confhandle = (struct confHandle *) malloc(sizeof(struct confHandle));
    if (conf->confhandle == NULL) {
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL, "ls_getconf", 
		  "malloc", sizeof(struct confHandle));
        if (fp)
	   fclose (fp);
	lserrno = LSE_MALLOC;
	return (NULL);
    }
    conf->confhandle->rootNode = rootNode;
    conf->confhandle->fname = NULL;
    conf->confhandle->curNode = NULL;
    conf->confhandle->lineCount = 0;
    conf->confhandle->ptrStack = NULL;
    ls_freeconf(conf);
    if (fp)
       fclose (fp);
    return (NULL);
} 

static struct confNode *
newNode(void)
{
    struct confNode *node;

    node = (struct confNode *)malloc(sizeof(struct confNode));
    if (node == NULL)
	return (NULL);
    node->leftPtr = NULL;
    node->rightPtr = NULL;
    node->fwPtr = NULL;
    node->cond = NULL;
    node->beginLineNum = 0;
    node->numLines = 0;
    node->lines = NULL;
    node->tag = 0;
    return (node);
} 

struct pStack *
initStack(void)
{
   struct pStack *stack;
   
   stack= (struct pStack *)malloc(sizeof(struct pStack ));
   if (stack == NULL)
       return (NULL);
 
   stack->top=-1;
   stack->nodes = (struct confNode **) malloc(5*sizeof(struct confNode *));
   if (stack->nodes == NULL)
       return (NULL);
   stack->size = 5;
   return (stack);
} 

int 
pushStack(struct pStack *stack, struct confNode *node)
{
   struct confNode **sp;

   if (stack == NULL || node == NULL )
       return (-1);

   if (stack->size == stack->top + 1) {
       sp = myrealloc(stack->nodes, stack->size*2*sizeof(struct confNode *));
       if (sp == NULL) {
           ls_syslog(LOG_ERR, I18N_FUNC_FAIL, "pushStack", "malloc");
           return(-1); 
       }
       stack->size *= 2;
       stack->nodes = sp;
   } 
   stack->nodes[++stack->top] = node;
   return (0);
} 

struct confNode *
popStack(struct pStack *stack)
{
   if (stack == NULL || stack->top < 0)
      return (NULL);
   return (stack->nodes[stack->top--]);
} 
 
void
freeStack(struct pStack *stack)
{
   if (stack != NULL) {
   	FREEUP(stack->nodes);
   	FREEUP(stack);
   }
} 

static char 
addCond(struct lsConf *conf, char *cond)
{
    int		i;
    char	**newlist;
    int		*values;

    if (conf==NULL || cond==NULL)
	return (FALSE);

    for ( i = 0; i < conf->numConds; i ++ ) {
	if (strcmp(conf->conds[i], cond) == 0)
	    break;
    }
    
    if (i < conf->numConds)		
	return (TRUE);

    newlist = (char **) malloc ((conf->numConds+1) * sizeof(char *));
    if (newlist == NULL)
	return (FALSE);
    values = (int *) malloc ((conf->numConds+1) * sizeof(int));
    if (values == NULL)
	return (FALSE);
    for ( i = 0; i < conf->numConds; i ++ ) {
 	newlist[i] = conf->conds[i];
	values[i] = conf->values[i];
    }
    newlist[conf->numConds] = putstr_(cond);
    if (newlist[conf->numConds] == NULL)
	return (FALSE);
    values[conf->numConds] = 0;
    FREEUP(conf->conds);
    FREEUP(conf->values);
    conf->conds = newlist;
    conf->values = values;
    conf->numConds++;
    return (TRUE);
} 

static char 
checkCond(struct lsConf *conf, char *cond)
{
    int		i;

    if (conf==NULL || cond==NULL)
	return (FALSE);

    for ( i = 0; i < conf->numConds; i ++ ) {
	if (strcmp(conf->conds[i], cond) == 0)
	    break;
    }

    if (i >= conf->numConds)		
	return (FALSE);
    if (conf->values[i])
	return (TRUE);
    else
	return (FALSE);
} 

static char 
linkNode(struct confNode *prev, struct confNode *node)
{
    if (prev==NULL || node==NULL)
	return (FALSE);

    if (prev->cond) {		
        if (prev->tag == NODE_ALL_DONE) {		
				
	    if (prev->fwPtr == NULL)
		prev->fwPtr = node;
	    else
		return (FALSE);
	} else if (prev->tag == NODE_LEFT_DONE) {
				
	    if (prev->rightPtr == NULL)
		prev->rightPtr = node;
	    else if (prev->fwPtr == NULL)
		prev->fwPtr = node;
	    else
		return (FALSE);
	} else {
            if (prev->leftPtr == NULL)
	        prev->leftPtr = node;
            else if (prev->rightPtr == NULL)
	        prev->rightPtr = node;
            else if (prev->fwPtr == NULL)
	        prev->fwPtr = node;
    	    else
	        return (FALSE);
	}
    } else {			
	if (prev->fwPtr == NULL)
	    prev->fwPtr = node;
	else
	    return (FALSE);
    }
    return (TRUE);
} 

void
ls_freeconf ( struct lsConf *conf )
{
    int		i;
    
    if (conf == NULL)
	return;
    freeNode(conf->confhandle->rootNode);
    FREEUP(conf->confhandle->fname);
    freeStack(conf->confhandle->ptrStack);
    FREEUP(conf->confhandle);
    for ( i = 0; i < conf->numConds; i ++ )
	FREEUP(conf->conds[i]);
    FREEUP(conf->conds);
    FREEUP(conf->values);
    FREEUP(conf);
} 

static void
freeNode(struct confNode *node)
{
    int		i;

    if (node == NULL)
	return;

    freeNode(node->leftPtr);
    freeNode(node->rightPtr);
    freeNode(node->fwPtr);
    FREEUP(node->cond);
    for ( i = 0; i < node->numLines; i ++ )
        FREEUP(node->lines[i]);
    FREEUP(node->lines);
    FREEUP(node);
    return;
} 

char *
getNextLine_conf(struct lsConf *conf, int confFormat)
{
    int dummy = 0;
    return (getNextLineC_conf(conf, &dummy, confFormat));
} 

char *
getNextLineC_conf(struct lsConf *conf, int *LineCount, int confFormat)
{
    static char *longLine = NULL;
    static char *myLine = NULL;
    char 	*sp, *cp, *line;
    int		len, linesize = 0;
    int		toBeContinue;
    int		isUNCPath = 0;

    if (conf == NULL)
	return (NULL);

    FREEUP(longLine);		

    if (confFormat) {
	do {
    	    line = readNextLine ( conf, LineCount );	
    	    if (line == NULL)
	        return (NULL);

				
	    toBeContinue = 0;
	    FREEUP(myLine);
	    len = strlen(line)+1;
    	    myLine = (char *) malloc (len*sizeof(char));
	    if (myLine==NULL)
		return (NULL);

	    sp = line;
	    cp = myLine;
	    while (sp!=&(line[len-1])) {
		if (*sp=='#') {		
		    break;
		} else if (*sp=='\\') {
					 
		    if (sp==&(line[len-2])) {	
				
		    	sp++;
			toBeContinue = 1;
		    } else {	
			 
                       if (!isUNCPath && *(sp+1) =='\\' && !isspace(*(sp+2)))
                           isUNCPath = 1;
                       if (!isspace(*(sp+1))) {
                    	   *cp = *sp;
                           sp++;
                           cp++;
                       } else {
                           sp++;	
                           sp++;
                       }
		    }
		} else if (isspace(*sp)) {
					
		    *cp = ' ';
		    sp++;
		    cp++;
		} else {		
		    *cp = *sp;
		    sp++;
		    cp++;
		}
	    }
	    *cp = '\0';			

	    if (!toBeContinue) {	
		while (cp!=myLine && *(--cp)==' ');
					
		if (cp==myLine && (*cp==' '||*cp=='\0')) {
		    *cp = '\0';
		} else
	            *(++cp) = '\0';
	    }
	     
	    if (!(myLine[0]=='\0' && !longLine)) {
				
	    	if (longLine) {
	    	    linesize += strlen(myLine);
	    	    sp = (char *) malloc (linesize*sizeof(char));
	    	    if (sp == NULL)	
		        return (longLine);

		    strcpy(sp, longLine);	
	            strcat(sp, myLine);		
		    FREEUP(longLine);
		    longLine = sp;
	        } else {
		    linesize = strlen(myLine)+1;
		    longLine = (char *) malloc (linesize*sizeof(char));
		    strcpy(longLine, myLine);
	        }
	    }
			
	} while ((myLine[0]=='\0' && !longLine) || toBeContinue);
		

	return (longLine);
    } else {
	do {
    	    line = readNextLine ( conf, LineCount );	
    	    if (line == NULL)
	        return (NULL);
	} while (line[0]=='\0');
	return (line);
    }
}

static char * 
readNextLine(struct lsConf *conf, int *lineNum)
{
    struct confNode *node, *prev;
    char	*line;

    if (conf == NULL)
	return (NULL);

    node = conf->confhandle->curNode;
    if (node == NULL)
	return (NULL);

    if (node->cond) {			
    	if ( node->tag != NODE_PASED ) {
					
	    node->tag = NODE_PASED;
	    pushStack( conf->confhandle->ptrStack, node );
					

	    if (checkCond(conf, node->cond)) {
		conf->confhandle->curNode = node->leftPtr;
		conf->confhandle->lineCount = 0;
	    } else {
		conf->confhandle->curNode = node->rightPtr;
		conf->confhandle->lineCount = 0;
	    }
	    line = readNextLine(conf, lineNum);
	    if (line)
		return (line);		
        }
	popStack ( conf->confhandle->ptrStack );
					
       
	node->tag &= ~NODE_PASED;
	conf->confhandle->curNode = node->fwPtr;
	conf->confhandle->lineCount = 0;
	line = readNextLine(conf, lineNum);
	if (line)
	    return (line);         	
	else {
	    prev = popStack ( conf->confhandle->ptrStack );
	    conf->confhandle->curNode = prev;
	    conf->confhandle->lineCount = 0;
	    pushStack( conf->confhandle->ptrStack, prev );
	    return (readNextLine(conf, lineNum));
	}
    } else {				

	if (conf->confhandle->lineCount <= node->numLines-1) {
	    line = node->lines[conf->confhandle->lineCount];
	    *lineNum = node->beginLineNum + conf->confhandle->lineCount;
	    conf->confhandle->lineCount++;
	    return(line);		
	} else {
	    conf->confhandle->curNode = node->fwPtr;
	    conf->confhandle->lineCount = 0;
	    line = readNextLine(conf, lineNum);
	    if (line)
		return (line);         
	    else {
	        prev = popStack ( conf->confhandle->ptrStack );
	        conf->confhandle->curNode = prev;
	        conf->confhandle->lineCount = 0;
	        pushStack( conf->confhandle->ptrStack, prev );
	        return (readNextLine(conf, lineNum));
	    } 
	}
    }
} 


/* $Id: callex.c 397 2007-11-26 19:04:00Z mblack $
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
#include "stdio.h"
#include<stdint.h>
#if (defined(__STDC__))
#define SPTR_BUFLEN 1024
char tmpbuf[SPTR_BUFLEN];
extern int yyreject();
extern int yywrap();
extern int yylook();
extern int yyback(int *, int);
extern int yyinput();
extern void yyoutput(int);
extern void yyunput(int);
extern int yylex();
extern int yyless();
#ifdef LEXDEBUG
extern void allprint();
extern void sprint();
#endif
#endif
# define U(x) x
# define NLSTATE yyprevious=YYNEWLINE
# define BEGIN yybgin = yysvec + 1 +
# define INITIAL 0
# define YYLERR yysvec
# define YYSTATE (yyestate-yysvec-1)
# define YYOPTIM 1
# define YYLMAX 200
# define output(c) putc(c,yyout)
# define input() (((yytchar=yysptr>yysbuf?U(*--yysptr):getbufc())==10?(yylineno++,yytchar):yytchar)==EOF?0:yytchar)
# define unput(c) {yytchar= (c);if(yytchar=='\n')yylineno--;*yysptr++=yytchar;}
# define yymore() (yymorfg=1)
# define ECHO fprintf(yyout, "%s",yytext)
# define REJECT { nstr = yyreject(); goto yyfussy;}
int yyleng;
int yylenguc;
extern unsigned char yytextarr[];
# ifdef YYCHAR_ARRAY
extern char yytext[];
# else
extern unsigned char yytext[];
# endif
int yyposix_point=0;
int yynls16=0;
int yynls_wchar=0;
char *yylocale = "C C C C C C";
int yymorfg;
extern unsigned char *yysptr, yysbuf[];
int yytchar;
#define yyin stdin
#define yyout stdout
extern int yylineno;
struct yysvf {
    int yystoff;
    struct yysvf *yyother;
    int *yystops;};
struct yysvf *yyestate;
extern struct yysvf yysvec[], *yybgin;

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#if defined(__linux__)
#include <linux/limits.h>
unsigned char *yysptr, yysbuf[];
#endif
#include <syslog.h>
#include <malloc.h>

#ifndef  MAXLINELEN
#include "../lsf.h"
#endif
#include "tokdefs.h"
#include "yparse.h"
#include "../lib/lsi18n.h"

#define MAXTOKENLEN 302
#define NL_SETN      22

typedef struct stream Stream;
struct stream { char *buf, *bp; Stream * prev; };

static Stream _in, * in=& _in;

char *source=0 ;
char *token = NULL;
char *yybuff;
char yyerr[MAXLINELEN];
extern int calerrno;

static int screen(void);
static void s_lookup(int);
static int getbufc(void);
int yywhere(char *);
void set_lower(char *);
struct rwtable *END(struct rwtable *);
int yymark(void);

# define YYNEWLINE 10
int yylex(){
    int nstr;
    while((nstr = yylook()) >= 0)
        switch(nstr){
            case 0:
                if(yywrap())
                    return(0);
                break;
            case 1:
                yymark() ;
                break;
            case 2:
                yymark() ;
                break;
            case 3:
                return (OR);
                break;
            case 4:
                return (AND);
                break;
            case 5:
                return (LE);
                break;
            case 6:
                return (GE);
                break;
            case 7:
                return (EQ);
                break;
            case 8:
                return (DOTS);
                break;
            case 9:
                return screen();
                break;
            case 10:
            {s_lookup((ICON));
                    return (ICON) ;
            }
            break;
            case 11:
            {s_lookup((RCON));
                return (RCON) ;
            }
            break;
            case 12:
            {s_lookup((ESTRING));
                return (ESTRING) ;
            }
            break;
            case 13:
            {s_lookup((ESTRING));
                return (ESTRING) ;
            }
            break;
            case 14:
                ;
                break;
            case 15:
                return ((yytext[0]));
                break;
            case -1:
                break;
            default:
                fprintf(yyout,"bad switch yylook %d",nstr);
        } return(0); }

extern int main(void);

static struct rwtable {
    char   rw_name[40] ;
    int    rw_yylex ;
} rwtable[] = {
    {"apr", APR},
    {"april", APR},
    {"aug", AUG},
    {"august", AUG},
    {"dates", DATES},
    {"day", DAYS},
    {"dec", DEC},
    {"december", DEC},
    {"feb", FEB},
    {"february", FEB},
    {"fri", FRI},
    {"friday", FRI},
    {"fy",  FY},
    {"h",   HH},
    {"jan", JAN},
    {"january", JAN},
    {"jul", JUL},
    {"july", JUL},
    {"jun", JUN},
    {"june", JUN},
    {"m",    MM},
    {"mar", MAR},
    {"march", MAR},
    {"may", MAY},
    {"mon", MON},
    {"monday", MON},
    {"month", MONTH},
    {"nov", NOV},
    {"november", NOV},
    {"oct", OCT},
    {"october", OCT},
    {"quarter", QUARTER},
    {"range",   RANGE},
    {"sat", SAT},
    {"saturday", SAT},
    {"sep", SEP},
    {"september", SEP},
    {"sun", SUN},
    {"sunday", SUN},
    {"thu", THU},
    {"thursday", THU},
    {"tue",      TUE},
    {"tuesday",  TUE},
    {"wed",      WED},
    {"wednesday",WED},
    {"week", WEEK},
    {"yy",  YY},
    {"zzzz",  	(SZZZZ)}
};



struct rwtable *END(low)
    struct rwtable *low;
{
    int c ;
    struct rwtable *i;

    for (i=low; ; i++){
        if ((c=strcmp(i->rw_name, "zzzz"))==0)
            return(i);
    }

}

void set_lower(str)
    char *str;
{ int i ;

    for (i=0; i<strlen(str); i++){
        if ('A' <=str[i] && str[i]<='Z')
            str[i]=str[i]+'a'-'A' ;
    }
}

static int screen(void)
{ struct rwtable *low=rwtable ,
        *high=END(low),
        *mid ;
    int c;
    char str[YYLMAX];

    strcpy(str,(char *)yytext);
    set_lower((char *)yytext);
    while ((int) (low<=high)){
	mid=low+(high-low)/2 ;
	if ((c=strcmp(mid->rw_name, (char *)yytext))==0) {
	    return (mid->rw_yylex) ;
	}
	else if (c<0)
	    low=mid+1;
	else
	    high=mid-1;
    }
    strcpy((char *)yytext,str);
    s_lookup(NAME);
    return (NAME);
}

static void s_lookup(yylex)
    int yylex;
{
    if (!token){
        token = (char *)malloc(MAXTOKENLEN);
        token[0] = '\0';
    }
    strcpy(token, (char *)yytext) ;
}


int yymark(void)
{
    if (source)
	free(source);
    source=(char *) calloc(yyleng, sizeof(char));
    if (source)
	sscanf((char *)yytext, "# %d %s", &yylineno, source);
    return (1);
}

void idxerror(s)
    register char *s ;
{ extern int idxnerrs ;
    char sptr[YYLMAX];

    sptr[0]='\0';
    yywhere(sptr) ;
    sprintf(yyerr, "[error %d] %s %s", idxnerrs+1,sptr, s);
    yysptr=yysbuf;
    yylineno =1;
    yparseFail(&idxAllocHead);
}



int yywhere(char *sptr)
{ char colon=0;

    if (source && *source && strcmp(source, "\"\""))
    { char * cp=source ;
        int len =strlen(source);

        if (*cp=='*')
            ++cp, len-=2;
        if (strncmp(cp, "./", 2 )==0)
            cp+=2, len-=2;
        sprintf(sptr, "file %.*s", len, cp);
        colon=1;
    }
    if (yylineno >0)
    {	if (colon) {
            snprintf(tmpbuf, sizeof(tmpbuf), "%s, ", sptr);
            strncpy(sptr, tmpbuf, SPTR_BUFLEN);
            sptr[SPTR_BUFLEN - 1] = '\0';
        }
        snprintf(tmpbuf, sizeof(tmpbuf), "%s line %d", sptr,
                 yylineno - (*yytext == '\n' || !*yytext));
        strncpy(sptr, tmpbuf, SPTR_BUFLEN);
        sptr[SPTR_BUFLEN - 1] = '\0';
	colon=1;
    }
    if (*yytext)
    {	register int i;

	for (i=0; i<20; ++i)
            if (!yytext[i] || yytext[i]=='\n')
                break;
	if (i)
   	{
            if (colon) {
                snprintf(tmpbuf, sizeof(tmpbuf), "%s ", sptr);
                strncpy(sptr, tmpbuf, SPTR_BUFLEN);
                sptr[sizeof(sptr) - 1] = '\0';
            }
            snprintf(tmpbuf, sizeof(tmpbuf), "%s near \"%.*s%.8s\"",
                     sptr, i, yytext, yybuff);
            strncpy(sptr, tmpbuf, SPTR_BUFLEN);
            sptr[sizeof(tmpbuf) - 1] = '\0';

            colon=1;
	}
    }
    if (colon) {
        snprintf(tmpbuf, sizeof(tmpbuf), "%s: ", sptr);
        strncpy(sptr, tmpbuf, SPTR_BUFLEN);
        sptr[sizeof(sptr) - 1] = '\0';
    }

    return(yylineno);
}

int
yywrap(void)
{	register Stream * sp;
    if ((sp=in->prev))
    {	if (in->buf)
            free(in->buf);
        if (in) {
            free(in);
            in=NULL;
        }
        in=sp;
        return 0;
    }
    return 1;
}

static int getbufc(void)
{
    if (*yybuff =='\0')
        return(EOF);
    else
        return(*yybuff++);
}


void *
yyalloc(struct mallocList  **head, int size)
{
    struct mallocList  *entry;
    void               *space;

    space = (void *)calloc(1, size);
    if (space) {
        entry = (struct mallocList  *)malloc(sizeof(struct mallocList));
        if (!entry) {
            free(space);
            return(NULL);
        }
        entry->space = space;
        entry->next = *head;
        *head = entry;
    };
    return(space);
}



void
yyfree(struct mallocList  **head, void *space)
{
    struct mallocList  *entry;

    entry = *head;
    while (entry) {
        if (entry->space == space)
            break;
        entry = entry->next;
    }

    if (entry)
        entry->space = NULL;
    free(space);
}

void
yparseSucc(struct mallocList  **head)
{
    struct mallocList  *entry, *freeEntry;

    entry = *head;
    while (entry) {
        freeEntry = entry;
        entry = entry->next;
        free(freeEntry);
    }
    *head = NULL;
    return;
}

void
yparseFail(struct mallocList  **head)
{
    struct mallocList  *entry, *freeEntry;

    if (!head)
        return;
    entry = *head;
    while (entry) {
        freeEntry = entry;
        entry = entry->next;
        if (freeEntry->space)
            free(freeEntry->space);
        free(freeEntry);
    }
    *head = NULL;
    return;
}


int
checkNameSpec(char *name, char **errMsg )
{
    static char bufMess[1024];
    int lexCode, retVal = 1;

    bufMess[0] = '\0';
    yybuff = name;
    lexCode = yylex();

    if ( lexCode != NAME) {
        if (lexCode == UNDEF)
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 350,
                                  "Name is invalid: \"%s\""), /* catgets 350 */
		    name);
        else if (lexCode == ICON || lexCode == RCON)
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 351,
                                  "Name cannot start with digits: \"%s\""), /* catgets 351 */
		    name);
        else if (lexCode == CCON)
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 352,
                                  "Name cannot be const string: \"%s\""), /* catgets 352 */
		    name);
        else if (MON <= lexCode && lexCode <= SUN)
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 353,
                                  "Name cannot be name of week: \"%s\""), /* catgets 353 */
		    name);
        else if (JAN <= lexCode && lexCode <= DEC)
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 354,
                                  "Name cannot be name of month: \"%s\""), /* catgets 354 */
		    name);
        else if (YY <= lexCode && lexCode <= SZZZZ)
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 355,
                                  "Name cannot be reserved word: \"%s\""), /* catgets 355 */
		    name);
        else
            sprintf(bufMess,
		    _i18n_msg_get(ls_catd, NL_SETN, 350,
                                  "Name is invalid: \"%s\""), name);
        retVal = 0;
        goto Exit;
    }
    else {
        lexCode = yylex();
        if ((char)lexCode == '@') {
            lexCode = yylex();
            if (lexCode != NAME) {
                sprintf(bufMess,
			_i18n_msg_get(ls_catd, NL_SETN, 357,
                                      "User name is wrong: \"%s\""), /* catgets 357 */
			token);
                retVal = 0;
                goto Exit;
            }
            lexCode = yylex();
        }
        if (lexCode > 0 ) {
            sprintf(bufMess,
                    _i18n_msg_get(ls_catd, NL_SETN, 350,
                                  "Name is invalid: \"%s\""), name);
            retVal = 0;
            goto Exit;
        }
        else {
            retVal = 1;
            goto Exit;
        }
    }
Exit:
    *errMsg = bufMess;
    yysptr=yysbuf;
    yylineno =1;
    return(retVal);
}
int yyvstop[] = {
    0,

    15,
    0,

    14,
    15,
    0,

    14,
    0,

    15,
    0,

    1,
    15,
    0,

    15,
    0,

    15,
    0,

    15,
    0,

    15,
    0,

    10,
    15,
    0,

    15,
    0,

    15,
    0,

    15,
    0,

    9,
    15,
    0,

    15,
    0,

    13,
    0,

    4,
    0,

    12,
    0,

    8,
    0,

    11,
    0,

    10,
    0,

    5,
    0,

    7,
    0,

    6,
    0,

    9,
    0,

    3,
    0,

    2,
    0,
    0};
# define YYTYPE unsigned char
struct yywork { YYTYPE verify, advance; } yycrank[] = {
    {0,0},	{0,0},	{1,3},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{1,4},	{1,5},
    {0,0},	{9,21},	{4,5},	{4,5},
    {0,0},	{6,18},	{0,0},	{0,0},
    {0,0},	{9,21},	{9,0},	{0,0},
    {0,0},	{6,18},	{6,0},	{33,0},
    {21,0},	{22,0},	{0,0},	{18,0},
    {19,0},	{0,0},	{0,0},	{1,6},
    {1,7},	{4,5},	{0,0},	{1,8},
    {1,9},	{2,6},	{2,7},	{8,20},
    {11,24},	{2,8},	{2,9},	{1,10},
    {1,11},	{1,12},	{6,19},	{9,22},
    {10,23},	{2,10},	{2,11},	{18,19},
    {19,19},	{21,22},	{22,22},	{33,32},
    {9,21},	{1,13},	{1,14},	{1,15},
    {6,18},	{13,27},	{1,16},	{2,13},
    {2,14},	{2,15},	{14,28},	{15,29},
    {0,0},	{0,0},	{0,0},	{0,0},
    {32,0},	{9,21},	{0,0},	{0,0},
    {12,25},	{6,18},	{12,26},	{12,26},
    {12,26},	{12,26},	{12,26},	{12,26},
    {12,26},	{12,26},	{12,26},	{12,26},
    {0,0},	{0,0},	{0,0},	{0,0},
    {1,3},	{25,25},	{25,25},	{25,25},
    {25,25},	{25,25},	{25,25},	{25,25},
    {25,25},	{25,25},	{25,25},	{9,21},
    {32,32},	{0,0},	{0,0},	{6,18},
    {0,0},	{32,33},	{0,0},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{1,17},	{17,31},	{0,0},
    {0,0},	{0,0},	{0,0},	{2,17},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{0,0},	{0,0},
    {0,0},	{0,0},	{16,30},	{0,0},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{16,30},	{16,30},
    {16,30},	{16,30},	{24,24},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{24,24},	{24,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{24,32},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{24,24},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{24,24},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {0,0},	{0,0},	{0,0},	{0,0},
    {24,24},	{0,0},	{0,0},	{0,0},
    {0,0}};
struct yysvf yysvec[] = {
    {0,	0,	0},
    {-1,	0,		0},
    {-7,	yysvec+1,	0},
    {0,	0,		yyvstop+1},
    {5,	0,		yyvstop+3},
    {0,	yysvec+4,	yyvstop+6},
    {-16,	0,		yyvstop+8},
    {0,	0,		yyvstop+10},
    {5,	0,		yyvstop+13},
    {-12,	0,		yyvstop+15},
    {6,	0,		yyvstop+17},
    {2,	0,		yyvstop+19},
    {34,	0,		yyvstop+21},
    {4,	0,		yyvstop+24},
    {9,	0,		yyvstop+26},
    {10,	0,		yyvstop+28},
    {67,	0,		yyvstop+30},
    {2,	0,		yyvstop+33},
    {-21,	yysvec+6,	0},
    {-22,	yysvec+6,	yyvstop+35},
    {0,	0,		yyvstop+37},
    {-18,	yysvec+9,	0},
    {-19,	yysvec+9,	yyvstop+39},
    {0,	0,		yyvstop+41},
    {-189,	0,		0},
    {49,	0,		yyvstop+43},
    {0,	yysvec+12,	yyvstop+45},
    {0,	0,		yyvstop+47},
    {0,	0,		yyvstop+49},
    {0,	0,		yyvstop+51},
    {0,	yysvec+16,	yyvstop+53},
    {0,	0,		yyvstop+55},
    {-66,	yysvec+24,	0},
    {-17,	yysvec+24,	yyvstop+57},
    {0,	0,	0}};
struct yywork *yytop = yycrank+284;
struct yysvf *yybgin = yysvec+1;
unsigned char yymatch[] = {
    00  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,011 ,012 ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    011 ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    '0' ,'0' ,'0' ,'0' ,'0' ,'0' ,'0' ,'0' ,
    '0' ,'0' ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,
    'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,
    'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,
    'A' ,'A' ,'A' ,01  ,01  ,01  ,01  ,'_' ,
    01  ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,
    'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,
    'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,'A' ,
    'A' ,'A' ,'A' ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    01  ,01  ,01  ,01  ,01  ,01  ,01  ,01  ,
    0};
unsigned char yyextra[] = {
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0};
int yylineno =1;
# define YYU(x) x
# define NLSTATE yyprevious=YYNEWLINE

#ifdef YYNLS16_WCHAR
unsigned char yytextuc[YYLMAX * sizeof(wchar_t)];
# ifdef YY_PCT_POINT
wchar_t yytextarr[YYLMAX];
wchar_t *yytext;
# else
wchar_t yytextarr[1];
wchar_t yytext[YYLMAX];
# endif
#else
unsigned char yytextuc;
# ifdef YY_PCT_POINT
unsigned char yytextarr[YYLMAX];
unsigned char *yytext;
# else
unsigned char yytextarr[1];
# ifdef YYCHAR_ARRAY
char yytext[YYLMAX];
# else
unsigned char yytext[YYLMAX];
# endif
# endif
#endif

struct yysvf *yylstate [YYLMAX], **yylsp, **yyolsp;
unsigned char yysbuf[YYLMAX];
unsigned char *yysptr = yysbuf;
int *yyfnd;
extern struct yysvf *yyestate;
int yyprevious = YYNEWLINE;
int yylook(){
    register struct yysvf *yystate, **lsp;
    register struct yywork *yyt;
    struct yysvf *yyz;
    int yych, yyfirst;
    struct yywork *yyr;
# ifdef LEXDEBUG
    int debug;
# endif
    unsigned char *yylastch;

# ifdef LEXDEBUG
    debug = 0;
# endif
    yyfirst=1;
    if (!yymorfg)
#ifdef YYNLS16_WCHAR
        yylastch = yytextuc;
#else
# ifdef YYCHAR_ARRAY
    yylastch = (unsigned char *)yytext;
# else
    yylastch = yytext;
# endif
#endif
    else {
        yymorfg=0;
#ifdef YYNLS16_WCHAR
        yylastch = yytextuc+yylenguc;
#else
# ifdef YYCHAR_ARRAY
        yylastch = (unsigned char *)yytext+yyleng;
# else
        yylastch = yytext+yyleng;
# endif
#endif
    }
    for(;;){
        lsp = yylstate;
        yyestate = yystate = yybgin;
        if (yyprevious==YYNEWLINE) yystate++;
        for (;;){
# ifdef LEXDEBUG
            if(debug)fprintf(yyout,"state %d\n",yystate-yysvec-1);
# endif
            yyt = &yycrank[yystate->yystoff];
            if(yyt == yycrank && !yyfirst){
                yyz = yystate->yyother;
                if(yyz == 0)break;
                if(yyz->yystoff == 0)break;
            }
            *yylastch++ = yych = input();
            yyfirst=0;
        tryagain:
# ifdef LEXDEBUG
            if(debug){
                fprintf(yyout,"char ");
                allprint(yych);
                putchar('\n');
            }
# endif
            yyr = yyt;
            if ( (intptr_t)yyt > (intptr_t)yycrank){
                yyt = yyr + yych;
                if (yyt <= yytop && yyt->verify+yysvec == yystate){
                    if(yyt->advance+yysvec == YYLERR)
                    {unput(*--yylastch);break;}
                    *lsp++ = yystate = yyt->advance+yysvec;
                    goto contin;
                }
            }
# ifdef YYOPTIM
            else if((intptr_t)yyt < (intptr_t)yycrank) {
                yyt = yyr = yycrank+(yycrank-yyt);
# ifdef LEXDEBUG
                if(debug)fprintf(yyout,"compressed state\n");
# endif
                yyt = yyt + yych;
                if(yyt <= yytop && yyt->verify+yysvec == yystate){
                    if(yyt->advance+yysvec == YYLERR)
                    {unput(*--yylastch);break;}
                    *lsp++ = yystate = yyt->advance+yysvec;
                    goto contin;
                }
                yyt = yyr + YYU(yymatch[yych]);
# ifdef LEXDEBUG
                if(debug){
                    fprintf(yyout,"try fall back character ");
                    allprint(YYU(yymatch[yych]));
                    putchar('\n');
                }
# endif
                if(yyt <= yytop && yyt->verify+yysvec == yystate){
                    if(yyt->advance+yysvec == YYLERR)
                    {unput(*--yylastch);break;}
                    *lsp++ = yystate = yyt->advance+yysvec;
                    goto contin;
                }
            }
            if ((yystate = yystate->yyother) && (yyt = &yycrank[yystate->yystoff]) != yycrank){
# ifdef LEXDEBUG
                if(debug)fprintf(yyout,"fall back to state %d\n",yystate-yysvec-1);
# endif
                goto tryagain;
            }
# endif
            else
            {unput(*--yylastch);break;}
        contin:
# ifdef LEXDEBUG
            if(debug){
                fprintf(yyout,"state %d char ",yystate-yysvec-1);
                allprint(yych);
                putchar('\n');
            }
# endif
            ;
        }
# ifdef LEXDEBUG
        if(debug){
            fprintf(yyout,"stopped at %d with ",*(lsp-1)-yysvec-1);
            allprint(yych);
            putchar('\n');
        }
# endif
        while (lsp-- > yylstate){
            *yylastch-- = 0;
            if (*lsp != 0 && (yyfnd= (*lsp)->yystops) && *yyfnd > 0){
                yyolsp = lsp;
                if(yyextra[*yyfnd]){
                    while(yyback((*lsp)->yystops,-*yyfnd) != 1 && lsp > yylstate){
                        lsp--;
                        unput(*yylastch--);
                    }
                }
                yyprevious = YYU(*yylastch);
                yylsp = lsp;
#ifdef YYNLS16_WCHAR
                yylenguc = yylastch-yytextuc+1;
                yytextuc[yylenguc] = 0;
#else
# ifdef YYCHAR_ARRAY
                yyleng = yylastch-(unsigned char*)yytext+1;
# else
                yyleng = yylastch-yytext+1;
# endif
                yytext[yyleng] = 0;
#endif
# ifdef LEXDEBUG
                if(debug){
                    fprintf(yyout,"\nmatch ");
#ifdef YYNLS16_WCHAR
                    sprint(yytextuc);
#else
                    sprint(yytext);
#endif
                    fprintf(yyout," action %d\n",*yyfnd);
                }
# endif
                return(*yyfnd++);
            }
            unput(*yylastch);
        }
#ifdef YYNLS16_WCHAR
        if (yytextuc[0] == 0  )
#else
            if (yytext[0] == 0  )
#endif
            {
                yysptr=yysbuf;
                return(0);
            }
#ifdef YYNLS16_WCHAR
        yyprevious = yytextuc[0] = input();
#else
        yyprevious = yytext[0] = input();
#endif
        if (yyprevious>0) {
            output(yyprevious);
#ifdef YYNLS16
            if (yynls16) {
                int noBytes;
                sec = input();
                third = input();
                fourth = input();
#ifdef YYNLS16_WCHAR
                noBytes = MultiByte(yytextuc[0],sec,third,fourth);
#else
                noBytes = MultiByte(yytext[0],sec,third,fourth);
#endif
                switch(noBytes) {
                    case 2:
#ifdef YYNLS16_WCHAR
                        output(yyprevious=yytextuc[0]=sec);
#else
                        output(yyprevious=yytext[0]=sec);
#endif
                        unput(fourth);
                        unput(third);
                        break;
                    case 3:
#ifdef YYNLS16_WCHAR
                        output(yyprevious=yytextuc[0]=sec);
                        output(yyprevious=yytextuc[0]=third);
#else
                        output(yyprevious=yytext[0]=sec);
                        output(yyprevious=yytext[0]=third);
#endif
                        unput(fourth);
                        break;
                    case 4:
#ifdef YYNLS16_WCHAR
                        output(yyprevious=yytextuc[0]=sec);
                        output(yyprevious=yytextuc[0]=third);
                        output(yyprevious=yytextuc[0]=fourth);
#else
                        output(yyprevious=yytext[0]=sec);
                        output(yyprevious=yytext[0]=third);
                        output(yyprevious=yytext[0]=fourth);
#endif
                        break;
                    default:
                        unput(fourth);
                        unput(third);
                        unput(sec);
                        break;
                }
            }
#endif
        }
#ifdef YYNLS16_WCHAR
        yylastch=yytextuc;
#else
# ifdef YYCHAR_ARRAY
        yylastch=(unsigned char*)yytext;
# else
        yylastch=yytext;
# endif
#endif
# ifdef LEXDEBUG
        if(debug)putchar('\n');
# endif
    }
}

int yyback(int *p, int m)
{
    if (p==0) return(0);
    while (*p)
    {
	if (*p++ == m)
            return(1);
    }
    return(0);
}

int yyinput(){
    return(input());

}

#if (defined(__STDC__))
void yyoutput(int c)
#else
    yyoutput(c)
    int c;
# endif
{
    output(c);
}

#if (defined(__STDC__))
void yyunput(int c)
#else
    yyunput(c)
    int c;
#endif
{
    unput(c);
}

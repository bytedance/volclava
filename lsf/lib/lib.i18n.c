/* $Id: lib.i18n.c 397 2007-11-26 19:04:00Z mblack $
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/lproto.h" 
#include "../lsf.h" 
#include "lsi18n.h" 

int I18nRunningFlag = 0;
int I18nInitFlag = 0;

LS_CATD ls_catd;

int
_i18n_init (int modId)
{

    I18nInitFlag = 1 ;
    return(0);

}

int
_i18n_end()
{
    return(0);
}


int i18n_ct_format_ID[]=
{
    1,
    2,
    3,
    4,
    5,
    6,
    7
};
#define NL_SETN         35      
char *i18n_ct_format[]=
{
    "%Ec\n",                            /* catgets 1 */
    "%a %b %d %T %Y",                   /* catgets 2 */
    "%b %d %T %Y",                      /* catgets 3 */
    "%a %b %d %T",                      /* catgets 4 */
    "%b %d %H:%M",                      /* catgets 5 */
    "%m/%d/%Y",                       	/* catgets 6 */
    "%H:%M:%S"                       	/* catgets 7 */
};
#undef NL_SETN
char *i18nCTFormatStr[MAX_CTIME_FORMATID+1];

void
_i18n_ctime_init(LS_CATD catID)
{
    int i;

    for (i=0; i<=MAX_CTIME_FORMATID; i++) {
	i18nCTFormatStr[i]= i18n_ct_format[i];
    }
    return;
}

char *
_i18n_ctime(LS_CATD catID, int formatID, const time_t * timer )
{
    static char timeStr[MAX_I18N_CTIME_STRING];

    strcpy(timeStr, ctime(timer));
    switch (formatID) 
    {
      case CTIME_FORMAT_a_b_d_T_Y:
	timeStr[24]='\0';
	return(timeStr);
      case CTIME_FORMAT_b_d_T_Y:
	timeStr[24]='\0';
	return(timeStr+4);
      case CTIME_FORMAT_a_b_d_T:
	timeStr[19]='\0';
	return(timeStr);
      case CTIME_FORMAT_b_d_H_M:
	timeStr[16]='\0';
	return(timeStr+4);
      case CTIME_FORMAT_m_d_Y:
	{
	    struct tm *timePtr;
	    timePtr = localtime(timer);
	    sprintf(timeStr, "%.2d/%.2d/%.4d", 
		timePtr->tm_mon+1,
		timePtr->tm_mday,
		timePtr->tm_year + 1900);
	    return(timeStr);
	}
      case CTIME_FORMAT_H_M_S:
	{
	    struct tm *timePtr;
	    timePtr = localtime(timer);
	    sprintf(timeStr, "%.2d:%.2d:%.2d", 
		timePtr->tm_hour,
		timePtr->tm_min,
		timePtr->tm_sec);
	    return(timeStr);
	}
      case CTIME_FORMAT_DEFAULT:
      default:
	return(timeStr);
    }
} 

char *
_i18n_printf(const char *format, ...)
{
    static char i18nPrintBuffer[1024];
    va_list ap;

    va_start(ap, format);
    vsnprintf(i18nPrintBuffer, sizeof(i18nPrintBuffer), format, ap);
    va_end(ap);
    return(i18nPrintBuffer);
} 


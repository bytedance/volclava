/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: misc.c 397 2007-11-26 19:04:00Z mblack $
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
#define NL_SETN      22


#define PRINT_ERRMSG(errMsg, fmt, msg1, msg2) \
    if (errMsg == NULL) \
        fprintf(stderr, fmt, msg1, msg2); \
    else \
	sprintf(*errMsg, fmt, msg1, msg2); \

#ifndef EN0
#define EN0	0
#endif
#ifndef DE1
#define DE1	1
#endif
#define DEF_CRED_SESSION(s) { (s)[0] = 224; (s)[1] = 186; (s)[2] = 234; \
			      (s)[3] = 112; (s)[4] = 11; (s)[5] = 37; \
			      (s)[6] = 121; (s)[7] = 11; }

int
hostValue(void)
{
    static unsigned char value;
    char *sp;

    if (value)
    return value;


    sp = ls_getmyhostname();
    if (sp == NULL)
    return 50;

    while (*sp != '\0') {
       value = value + *sp;
       sp++;
    }

    return (int) value;

}




int
getBootTime(time_t *bootTime)
{
    static char fname[] = "getBootTime";
    FILE *fp;
    char *paths[] = {"/usr/bin/uptime", "/usr/ucb/uptime", "/bin/uptime",
			"/usr/bsd/uptime", "/local/bin/uptime"};
    char dummy[32], str1[32], str2[32], str3[32], str4[32], *c;
    int i, days, hr, minute;
    char *oldIFS=NULL;
    char *envIFS=NULL;
    int len;


    if ((oldIFS = getenv("IFS")) != NULL)
    {
	len = strlen(oldIFS);
	envIFS = (char *)malloc(len+8);
        if(envIFS == NULL) {
            ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "malloc");
    	    return (-1);
	}
	sprintf(envIFS, "IFS=%s", oldIFS);
	putenv("IFS=");
    }

    for (i = 0; i < 5; i++) {
	if ((fp = popen(paths[i], "r"))) {
	    if (fscanf(fp, "%s %s %s %s %s %s", dummy, dummy,
		       str1, str2, str3, str4) != 6) {
		pclose(fp);
		continue;
	    }

	    if (!strncmp(str2, "day", 3)) {
		days = atoi(str1);
		strcpy(str1, str3);
		strcpy(str2, str4);
	    } else {
		days = 0;
	    }

	    if ((c = strchr(str1, ':'))) {
		*c = '\0';
		str1[strlen(c)-1] = '\0';
		c++;
		minute = atoi(c);
		hr = atoi(str1);
	    } else {
		if (!strncmp(str2, "hr", 2)) {
		    hr = atoi(str1);
		    minute = 0;
		} else {
		    hr = 0;
		    if (!strncmp(str2, "min", 3))
			minute = atoi(str1);
		    else
			minute = 0;
		}
	    }

	    *bootTime = time(0) - (days*60*60*24 + hr*60*60 + minute*60);
	    pclose(fp);

    	    if (envIFS != NULL)
    	    {
		putenv(envIFS);
		FREEUP(envIFS);
    	    }
	    return (0);
	}
	ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, "getBootTime", "popen");
    }


    if (envIFS != NULL)
    {
	putenv(envIFS);
	FREEUP(envIFS);
    }

    return (-1);
}


static unsigned char cblock[8];

char*
encryptPassLSF(char *pass)
{
    char* passEnc;
    if ( pass ) {
	passEnc = strdup(pass);
    } else {
	passEnc = strdup("");
    }
    return passEnc;
}


char*
decryptPassLSF(char *pass)
{
    char* passEnc;
    if ( pass ) {
	passEnc = strdup(pass);
    } else {
	passEnc = strdup("");
    }
    return passEnc;
}



char *
encryptByKey_(char *key, char *inputStr)
{
     if (key != NULL)
           strncpy((char*) cblock, key, 8);
     return (encryptPassLSF(inputStr));
 }



char *
decryptByKey_(char *key, char *inputStr)
{
     if (key != NULL)
           strncpy((char *)cblock, key, 8);
     return (decryptPassLSF(inputStr));
}

void
putMaskLevel(int level, char **para)
{
    level = level + LOG_DEBUG;

    if ((level >= LOG_DEBUG) && (level <= LOG_DEBUG3)) {
	FREEUP (*para);

        switch(level) {
	    case LOG_DEBUG:
		*para = putstr_("LOG_DEBUG");
		break;
	    case LOG_DEBUG1:
		*para = putstr_("LOG_DEBUG1");
		break;
            case LOG_DEBUG2:
		*para = putstr_("LOG_DEBUG2");
		break;
	    case LOG_DEBUG3:
		*para = putstr_("LOG_DEBUG3");
		break;
         }
    }
}


char *
safe_calloc(unsigned number, unsigned size)
{
    void *start_address;

    if (!number) {
        return(NULL);
    }
    start_address= (void *) calloc(number, size);
    return(start_address);

}

int
matchName(char *pattern, char *name)
{
    int i, ip;

    if (!pattern || !name)
        return(FALSE);

    ip = (int)strlen(pattern);
    for (i = 0; i < ip && pattern[i] != '['; i++) {

        if (pattern[i] == '*')
            return(TRUE);

        if (name[i] == '\0' || name[i] == '[' || pattern[i] != name[i])
            return(FALSE);
    };

    if (name[i] == '\0' || name[i] == '[' )
        return(TRUE);
    else
        return(FALSE);

}

int
readPassword(char *buffer)
{
    struct termios echo_control, save_control;
    int fd;


  fd = fileno(stdin);

  if (tcgetattr(fd, &echo_control) == -1)
    return -1;

  save_control = echo_control;
  echo_control.c_lflag &= ~(ECHO|ECHONL);


  if (tcsetattr(fd, TCSANOW, &echo_control) == -1)
    return -1;

  scanf("%s", buffer);

  if (tcsetattr(fd, TCSANOW, &save_control) == -1)
    return -1;

  return 0;
}

char **
parseCommandArgs(char *comm, char *args)
{
    int argmax  = 10;
    int argc    = 0;
    int quote   = 0;
    char *i     = args;
    char *j     = args;
    char **argv = NULL;


    if ((argv = (char**)malloc(argmax * sizeof(char*))) == NULL)
        goto END;


    if (comm)
        argv[argc++] = comm;


    if (!args)
        goto END;


    while (*j && (*j == ' ' || *j == '\t'))
        ++j;
    if (!*j)
        goto END;
    *i = *j;


    quote = 0;
    argv[argc++] = i;
    do {
        switch (*j) {


        case ' ':
        case '\t':
            if (quote) {
                *i++ = *j++;
            } else {
                *i++ = *j++ = '\0';
                while (*j && (*j == ' ' || *j == '\t'))
                    ++j;


                if (argc == argmax - 1) {
                    argmax *= 2;
                    argv = (char**)realloc(argv, argmax * sizeof(char*));
                    if (!argv)
                        goto END;
                }

                *i = *j;
                argv[argc++] = i;
            }
            break;


        case '\'':
        case '"':
            if (quote) {
                if (quote == *j) {
                    quote = 0;
                    ++j;
                } else {
                    *i++ = *j++;
                }
            } else {
                quote = *j++;
            }
            break;


        case '\\':
            if (quote != '\'')
                ++j;
            *i++ = *j++;
            break;


        default:
            *i++ = *j++;
            break;
        }
    } while (*j);

END:
    if (argv)
        argv[argc] = NULL;
    return argv;
}

int
FCLOSEUP(FILE** fp)
{
    int n ;

    n = 0;
    if (*fp)
    {
        n=fclose(*fp);
        *fp=NULL;
        if ( n < 0 )
        {
           lserrno=LSE_FILE_SYS;
        }
    } else
    {
        lserrno=LSE_FILE_CLOSE;
    }

    return n;
}

#undef getopt


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int     optind = 1;
char    *optarg = NULL;
int     opterr = 1;
int     optopt = 0;

int	linux_opterr = 1,
	linux_optind = 1,
	linux_optopt;
char	*linux_optarg;

#define	BADCH	(int)'?'
#define	EMSG	""

int
linux_getopt(nargc, nargv, ostr)
	int nargc;
	char * const *nargv;
	const char *ostr;
{
	static char *place = EMSG;
	register char *oli;
	char *p;

	if (!*place) {
		if (linux_optind >= nargc || *(place = nargv[linux_optind]) != '-') {
			place = EMSG;
			opterr = linux_opterr;
			optopt = linux_optopt;
			optind = linux_optind;
			optarg = linux_optarg;
			return(EOF);
		}
		if (place[1] && *++place == '-') {
			++linux_optind;
			place = EMSG;
			opterr = linux_opterr;
			optopt = linux_optopt;
			optind = linux_optind;
			optarg = linux_optarg;

			return(EOF);
		}
	}
	if ((linux_optopt = (int)*place++) == (int)':' ||
	    !(oli = strchr(ostr, linux_optopt))) {

	  if (linux_optopt == (int)'-') {
			opterr = linux_opterr;
			optopt = linux_optopt;
			optind = linux_optind;
			optarg = linux_optarg;

			return(EOF);
	  }
		if (!*place)
			++linux_optind;
		if (linux_opterr) {
			if (!(p = strrchr(*nargv, '/')))
				p = *nargv;
			else
				++p;
			(void)fprintf(stderr, (_i18n_msg_get(ls_catd,NL_SETN,653, "%s: illegal option -- %c\n")),  /* catgets 653 */
			    p, linux_optopt);
		}
		opterr = linux_opterr;
		optopt = linux_optopt;
		optind = linux_optind;
		optarg = linux_optarg;

		return(BADCH);
	}
	if (*++oli != ':') {
		linux_optarg = NULL;
		if (!*place)
			++linux_optind;
	}
	else {
		if (*place)
			linux_optarg = place;
		else if (nargc <= ++linux_optind) {
			place = EMSG;
			if (!(p = strrchr(*nargv, '/')))
				p = *nargv;
			else
				++p;
			if (linux_opterr)
				(void)fprintf(stderr,
				    (_i18n_msg_get(ls_catd,NL_SETN,654, "%s: option requires an argument -- %c\n")),  /* catgets 654 */
				    p, linux_optopt);
			opterr = linux_opterr;
			optopt = linux_optopt;
			optind = linux_optind;
			optarg = linux_optarg;

			return(BADCH);
		}
	 	else
			linux_optarg = nargv[linux_optind];
		place = EMSG;
		++linux_optind;
	}
	opterr = linux_opterr;
	optopt = linux_optopt;
	optind = linux_optind;
	optarg = linux_optarg;
	return(linux_optopt);
}

int
compareAddrValues(char *rangeStr, char *valueStr)
{
    static char fname[] = "compareAddrValues";
    int lowRange = INT_MAX;
    char *lowPtr = NULL;
    int highRange = INT_MAX;
    char *highPtr = NULL;
    char range[MAXADDRSTRING];
    int value = 0;


    mystrncpy(range, rangeStr, sizeof(char)*MAXADDRSTRING);


    value = atoi(valueStr);
    if (value < 0) {
	if (logclass & LC_TRACE) {
	    ls_syslog(LOG_DEBUG3, I18N(5710,
		      "%s: Bad address value <%s>"), /* catgets 5710 */
		      fname, valueStr);
	}
	return(FALSE);
    }

    lowPtr = range;
    highPtr = strchr(range, '-');
    if (highPtr != NULL) {
	*highPtr = '\0';
	highPtr += sizeof(char);
    } else {

	highPtr = range;
    }

    if ( (highPtr == NULL) || (*highPtr == '*') ) {
	highRange = INT_MAX;
    } else {

	highRange = atoi(highPtr);
	if (highRange < 0) {
	    if (logclass & LC_TRACE) {
		ls_syslog(LOG_DEBUG3, I18N(5711,
			  "%s: Bad high range value <%s>"), /* catgets 5711 */
			  fname, highPtr);
	    }
	    return(FALSE);
	}
    }

    if ( (lowPtr == NULL) || (*lowPtr == '*') ) {
	lowRange = 0;
    } else {

	lowRange = atoi(lowPtr);
	if (lowRange < 0) {
		if (logclass & LC_TRACE) {
		    ls_syslog(LOG_DEBUG3, I18N(5712,
			      "%s: Bad low range value <%S>"), /* catgets 5712 */
			      fname, lowPtr);
		}
		return(FALSE);
	}
    }

    if (logclass & LC_TRACE) {
	ls_syslog(LOG_DEBUG3, I18N(5715,
		  "%s: Low <%d> High <%d> Value <%d>"), /* catgets 5715 */
		  fname, lowRange, highRange, value);
    }
    if ( (lowRange <= value) && (highRange >= value) ) {
	return(TRUE);
    }
    return(FALSE);
}

int
withinAddrRange(char *addrRange, char *address)
{
    static char fname[] = "withinAddrRange";
    char *nextAddr = NULL;
    char *mark1 = NULL;
    char *ptr1 = NULL;
    char *mark2 = NULL;
    char *ptr2 = NULL;
    char tempAddrRange[MAXADDRSTRING];
    char tempAddress[MAXADDRSTRING];


    if (addrRange == NULL) {
	return(TRUE);
    }

    if ( (addrRange[0] == '\0')
	 || (address == NULL)
	 || (address[0] == '\0') ) {
	return(FALSE);
    }

    mystrncpy(tempAddrRange, addrRange, sizeof(char)*MAXADDRSTRING);
    mystrncpy(tempAddress, address, sizeof(char)*MAXADDRSTRING);


    nextAddr = strchr(tempAddrRange, ' ');
    if (nextAddr != NULL) {

	*nextAddr = '\0';
	nextAddr += sizeof(char);

	while (*nextAddr == ' ') {
	    nextAddr += sizeof(char);
	}
	if (*nextAddr != '\0') {

	    if (withinAddrRange(nextAddr, tempAddress) == TRUE) {
		return(TRUE);
	    }
	}
    }

    if (logclass & LC_TRACE) {
	ls_syslog(LOG_DEBUG, I18N(5716,
		  "%s: comparing range <%s> with value <%s>"), /* catgets 5716 */
		  fname, tempAddrRange, tempAddress);
    }


    ptr1 = tempAddrRange;
    mark1 = tempAddrRange;
    ptr2 = tempAddress;
    mark2 = tempAddress;


    while (mark1 != NULL && mark2 != NULL) {
	ptr1 = strchr(mark1, '.');
	ptr2 = strchr(mark2, '.');


	if (ptr1 != NULL) {
	    *ptr1 = '\0';
	    ptr1 += sizeof(char);
	}
	if (ptr2 != NULL) {
	    *ptr2 = '\0';
	    ptr2 += sizeof(char);
	}


	if (compareAddrValues(mark1, mark2) == FALSE) {
	    return(FALSE);
	}

	mark1 = ptr1;
	mark2 = ptr2;
    }

    return(TRUE);
}

int
validateAddrValue(char *rangeStr)
{
    int lowRange = 0;
    char *lowPtr = NULL;
    int highRange = 255;
    char *highPtr = NULL;
    char range[MAXADDRSTRING];
    char *digitCheck = NULL;


    mystrncpy(range, rangeStr, sizeof(char)*MAXADDRSTRING);


    lowPtr = range;
    highPtr = strchr(range, '-');
    if (highPtr != NULL) {
	*highPtr = '\0';
	highPtr += sizeof(char);
    }

    if (highPtr != NULL) {
	if (*highPtr == '*') {
	    if (highPtr[1] != '\0') {
		return(FALSE);
	    }
	} else {

	    digitCheck = highPtr;
	    while (*digitCheck != '\0') {
		if (isdigit((int)*digitCheck) == FALSE) {
		    return(FALSE);
		}
		digitCheck += sizeof(char);
	    }

	    highRange = atoi(highPtr);
	    if ( (highRange < 0) || (highRange > 255) ) {
		return(FALSE);
	    }
	}
    }

    if (lowPtr != NULL) {
	if (*lowPtr == '*') {
	    if (lowPtr[1] != '\0') {
		return(FALSE);
	    }
	} else {

	    digitCheck = lowPtr;
	    while (*digitCheck != '\0') {
		if (isdigit((int)*digitCheck) == FALSE) {
		    return(FALSE);
		}
		digitCheck += sizeof(char);
	    }

	    lowRange = atoi(lowPtr);
	    if ( (lowRange < 0) || (lowRange > 255) ) {
		return(FALSE);
	    }
	}
    }

    if (lowRange > highRange) {
	return(FALSE);
    }
    return(TRUE);
}

int
validateAddrRange(char *addrRange)
{
    static char fname[] = "validateAddrRange";
    char *nextAddr = NULL;
    char *mark1 = NULL;
    char *ptr1 = NULL;
    char tempAddrRange[MAXADDRSTRING];
    char debugAddrRange[MAXADDRSTRING];
    int  match = TRUE;
    int fieldCount = 0;


    if (addrRange == NULL) {
	return(TRUE);
    }


    mystrncpy(tempAddrRange, addrRange, sizeof(char)*MAXADDRSTRING);


    nextAddr = strchr(tempAddrRange, ' ');
    if (nextAddr != NULL) {

	*nextAddr = '\0';
	nextAddr += sizeof(char);

	while (*nextAddr == ' ') {
	    nextAddr += sizeof(char);
	}
	if (*nextAddr != '\0') {

	    if (validateAddrRange(nextAddr) == FALSE) {
		return(FALSE);
	    }
	}
    }


    ptr1 = tempAddrRange;
    mark1 = tempAddrRange;


    mystrncpy(debugAddrRange, tempAddrRange, sizeof(char)*MAXADDRSTRING);


    while (match && mark1 != NULL && *mark1 != '\0') {
	ptr1 = strchr(mark1, '.');


	if (ptr1 != NULL) {
	    *ptr1 = '\0';
	    ptr1 += sizeof(char);
	}

	if (fieldCount >= 4) {
	    ls_syslog(LOG_ERR, I18N(5717,
		      "%s: too many fields in address range <%s>"), /* catgets 5717 */
		      fname, debugAddrRange);
	    return(FALSE);
	}


	if (validateAddrValue(mark1) == FALSE) {
	    ls_syslog(LOG_ERR, I18N(9999,
		      "%s: invalid address range <%s>\n"), /* catgets 5718 */
		      fname, debugAddrRange);
	    return(FALSE);
	}
	fieldCount++;


	mark1 = ptr1;
    }
    if ( (fieldCount < 1) || (fieldCount > 4) ) {
	ls_syslog(LOG_ERR, I18N(5718,
		  "%s: invalid address range <%s>\n"), /* catgets 5718 */
		  fname, debugAddrRange);
	return(FALSE);
    }
    return(TRUE);
}

char *
mystrncpy(char *s1, const char *s2, size_t n)
{
    strncpy(s1, s2, n);
    if (n > 0) {
	s1[n-1] = '\0';
    }
    return s1;
}

void
openChildLog(const char *defLogFileName,
             const char *confLogDir,
             int use_stderr,
             char **confLogMaskPtr
            )
{
    #define _RES_CHILD_LOGFILENAME "res"
    static char resChildLogMask[] = "LOG_DEBUG";

    char *dbgEnv;
    char logFileName[MAXFILENAMELEN];
    char *logDir;
    char *logMask;
    int isResChild;

    isResChild = !strcmp(defLogFileName, _RES_CHILD_LOGFILENAME);


    dbgEnv = getenv("DYN_DBG_LOGCLASS");
    if( dbgEnv != NULL && dbgEnv[0] != '\0') {
        logclass = atoi(dbgEnv);
    }

    dbgEnv = getenv("DYN_DBG_LOGLEVEL");
    if( dbgEnv != NULL && dbgEnv[0] != '\0') {
        putMaskLevel(atoi(dbgEnv), confLogMaskPtr);
    }

    dbgEnv = getenv("DYN_DBG_LOGFILENAME");
    if( dbgEnv != NULL && dbgEnv[0] != '\0' ) {
        strcpy(logFileName, dbgEnv);

        if( !isResChild ) {
            strcat(logFileName, "c");
        }
    }
    else {
        strcpy(logFileName, defLogFileName);
    }

    dbgEnv = getenv("DYN_DBG_LOGDIR");
    if( dbgEnv != NULL && dbgEnv[0] != '\0') {
        logDir = dbgEnv;
    }
    else {
        logDir = (char *) confLogDir;
    }

    if( use_stderr && isResChild ) {

        logMask = resChildLogMask;
    }
    else {
        logMask = *confLogMaskPtr;
    }

    ls_openlog(logFileName, logDir, use_stderr, logMask );

    #undef _RES_CHILD_LOGFILENAME
}

void
cleanDynDbgEnv(void)
{
    if( getenv("DYN_DBG_LOGCLASS") ) {
        putEnv("DYN_DBG_LOGCLASS", "");
    }
    if( getenv("DYN_DBG_LOGLEVEL") ) {
        putEnv("DYN_DBG_LOGLEVEL", "");
    }
    if( getenv("DYN_DBG_LOGDIR") ) {
        putEnv("DYN_DBG_LOGDIR", "");
    }
    if( getenv("DYN_DBG_LOGFILENAME") ) {
        putEnv("DYN_DBG_LOGFILENAME", "");
    }
}

void
displayEnhancementNames(void) {
}

unitTypes setUnitForLimits(char *unitParamValue) {
    if (strcasecmp(unitParamValue, UNIT_M) == 0 || strcasecmp(unitParamValue, UNIT_MB) == 0) {
        return Megabytes;
    }

    if (strcasecmp(unitParamValue, UNIT_G) == 0 || strcasecmp(unitParamValue, UNIT_GB) == 0) {
        return Gigabytes;
    }

    if (strcasecmp(unitParamValue, UNIT_T) == 0 || strcasecmp(unitParamValue, UNIT_TB) == 0) {
        return Terabytes;
    }

    if (strcasecmp(unitParamValue, UNIT_P) == 0 || strcasecmp(unitParamValue, UNIT_PB) == 0) {
        return Petabytes;
    }

    if (strcasecmp(unitParamValue, UNIT_E) == 0 || strcasecmp(unitParamValue, UNIT_EB) == 0) {
        return Exabytes;
    }

    return Megabytes;
}

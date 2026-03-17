/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: lib.misc.c 397 2007-11-26 19:04:00Z mblack $
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

#include "lib.h"
#include "lproto.h"

#define BADCH   ":"
#define NL_SETN   23

extern int optind;
extern char *optarg;
extern int  opterr;
extern int  optopt;

#define PRINT_ERRMSG(errMsg, fmt, msg1, msg2)   \
    {                                           \
        if (errMsg == NULL)                     \
            fprintf(stderr, fmt, msg1, msg2);   \
        else                                    \
            sprintf(*errMsg, fmt, msg1, msg2);  \
    }

static struct LSFAdmins {
    int     numAdmins;
    char    **names;
} LSFAdmins;

bool_t  isLSFAdmin(const char *);

char
isanumber_(char *word)
{
    char **eptr;
    double number;

    if (!word || *word == '\0')
        return FALSE;

    if (errno == ERANGE)
        errno = 0;

    eptr = &word;
    number = strtod (word, eptr);
    if (**eptr == '\0' &&  errno != ERANGE)
        if (number <= MAXFLOAT && number > -MAXFLOAT)
            return TRUE;

    return FALSE;
}

char
islongint_(char *word)
{
    long long int number;

    if (!word || *word == '\0')
        return FALSE;

    if(!isdigitstr_(word)) return FALSE;

    if (errno == ERANGE)
        errno = 0;

    sscanf(word, "%lld", &number);
    if (errno != ERANGE) {
        if (number <= INFINIT_LONG_INT && number > -INFINIT_LONG_INT)
            return TRUE;
    }
    return FALSE;

}

int
isdigitstr_(char *string)
{
    int i;

    for (i = 0; i < strlen(string); i++) {
        if (! isdigit(string[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

LS_LONG_INT
atoi64_(char *word)
{
    long long int number;

    if (!word || *word == '\0')
        return 0;

    if (errno == ERANGE)
        errno = 0;

    sscanf(word, "%lld", &number);
    if (errno != ERANGE) {
        if (number <= INFINIT_LONG_INT && number > -INFINIT_LONG_INT)
            return number;
    }
    return 0;
}

char
isint_(char *word)
{
    char **eptr;

    int number;

    if (!word || *word == '\0')
        return FALSE;

    if (errno == ERANGE)
        errno = 0;
    eptr = &word;
    number = strtol (word, eptr, 10);
    if (**eptr == '\0'&&  errno != ERANGE) {
        if (number <= INFINIT_INT && number > -INFINIT_INT)
            return TRUE;
    }

    return FALSE;
}

char *
putstr_(const char *s)
{
    register char *p;

    if (s == (char *)NULL) {
        s = "";
    }

    p = malloc(strlen(s)+1);
    if (!p)
        return NULL;

    strcpy(p, s);

    return p;
}

short
getRefNum_(void)
{
    static short reqRefNum = MIN_REF_NUM;

    reqRefNum++;
    if (reqRefNum >= MAX_REF_NUM)
        reqRefNum = MIN_REF_NUM;

    return reqRefNum;
}

char *
chDisplay_(char *disp)
{
    char *sp;
    char *hostName;
    static char dspbuf[MAXHOSTNAMELEN+10];

    sp = disp +8;
    if (strncmp("unix:", sp, 5) == 0)
        sp += 4;
    else if (strncmp("localhost:", sp, 10) == 0)
        sp += 9;

    if (sp[0] == ':') {
        if ((hostName = ls_getmyhostname()) == NULL)
            return(disp);
        sprintf(dspbuf, "%s=%s%s", "DISPLAY", hostName, sp);
        return dspbuf;
    }

    return disp;

}

void
strToLower_(char *name)
{
    while (*name != '\0') {
        *name = tolower(*name);
        name++;
    }

}

void
strToUpper_(char *name)
{
    while (*name != '\0') {
        *name = toupper(*name);
        name++;
    }

}

char *
getNextToken(char **sp)
{
    static char word[MAXLINELEN];
    char *cp;

    if (!*sp)
        return NULL;

    cp = *sp;
    if (cp[0] == '\0')
        return NULL;

    if (cp[0] == ':' || cp[0] == '=' || cp[0] == ' ')
        *sp += 1;
    cp = *sp;
    if (cp[0] == '\0')
        return NULL;

    strcpy(word, cp);
    if ((cp = strchr(word, ':')) != NULL)
        *cp = '\0';
    if ((cp = strchr(word, '=')) != NULL)
        *cp = '\0';

    *sp += strlen(word);
    return word;

}

int
getValPair(char **resReq, int *val1, int *val2)
{
    char *token, *cp, *wd1 = NULL, *wd2 = NULL;
    int len;

    *val1 = INFINIT_INT;
    *val2 = INFINIT_INT;

    token = getNextToken (resReq);
    if (!token)
        return 0;
    len = strlen (token);
    if (len == 0)
        return 0;
    cp = token;
    while (*cp != '\0' && *cp != ',' && *cp != '/')
        cp++;
    if (*cp != '\0') {
        *cp = '\0';
        if (cp - token > 0)
            wd1 = token;
        if (cp - token < len - 1)
            wd2 = ++cp;
    } else
        wd1 = token;
    if (wd1 && !isint_(wd1))
        return -1;
    if (wd2 && !isint_(wd2))
        return -1;
    if (!wd1 && !wd2)
        return -1;
    if (wd1)
        *val1 = atoi(wd1);
    if (wd2)
        *val2 = atoi(wd2);

    return 0;
}


char *
my_getopt(int nargc, char **nargv, char *ostr, char **errMsg)
{
    char svstr [256];
    char *cp1 = svstr;
    char *cp2 = svstr;
    char *optName;
    int i, num_arg;

    if ((optName = nargv[optind]) == NULL)
        return (NULL);
    if (optind >= nargc || *optName != '-')
        return (NULL);
    if (optName[1] && *++optName == '-') {
        ++optind;
        return(NULL);
    }
    if (ostr == NULL)
        return(NULL);
    strcpy (svstr, ostr);
    num_arg = 0;
    optarg = NULL;

    while (*cp2) {
        int cp2len = strlen(cp2);
        for (i=0; i<cp2len; i++) {
            if (cp2[i] == '|') {
                num_arg = 0;
                cp2[i] = '\0';
                break;
            }
            else if (cp2[i] == ':') {
                num_arg = 1;
                cp2[i] = '\0';
                break;
            }
        }
        if (i >= cp2len)
            return (BADCH);

        if (!strcmp (optName, cp1)) {
            if (num_arg) {
                if (nargc <= optind + 1) {
                    PRINT_ERRMSG (errMsg, (_i18n_msg_get(ls_catd,NL_SETN,650, "%s: option requires an argument -- %s\n")), nargv[0], optName);  /* catgets 650 */
                    return (BADCH);
                }
                optarg = nargv[++optind];
            }
            ++optind;
            return (optName);
        } else if (!strncmp(optName, cp1, strlen(cp1))) {
            if (num_arg == 0) {
                PRINT_ERRMSG (errMsg, (_i18n_msg_get(ls_catd,NL_SETN,651, "%s: option cannot have an argument -- %s\n")),  /* catgets 651 */
                              nargv[0], cp1);
                return (BADCH);
            }

            optarg = optName + strlen(cp1);
            ++optind;
            return (cp1);
        }

        cp1 = &cp2[i];
        cp2 = ++cp1;
    }
    PRINT_ERRMSG (errMsg, (_i18n_msg_get(ls_catd,NL_SETN,652, "%s: illegal option -- %s\n")), nargv[0], optName); /* catgets 652 */
    return (BADCH);

}

int
putEnv(char *env, char *val)
{
    int ret;

    if ((env == NULL) || (env[0] == 0)) {
        return 0;
    }

    if (val == NULL) {
        val = "";
    }
    ret = setenv(env, val, 1);

    return ret;
}

void
initLSFHeader_ (struct LSFHeader *hdr)
{
    memset(hdr, 0, sizeof(struct LSFHeader));
    hdr->version = VOLCLAVA_VERSION;
}

void *
myrealloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return malloc(size);
    } else {
        return realloc(ptr, size);
    }
}

int
Bind_(int sockfd, struct sockaddr *myaddr, int addrlen)
{
    struct sockaddr_in *cliaddr;
    ushort port;
    int i;

    cliaddr = (struct sockaddr_in *)myaddr;
    if (cliaddr->sin_port != 0)
        return(bind(sockfd, myaddr, addrlen));
    else {
        for (i = 1 ; i <= BIND_RETRY_TIMES; i++) {
            if (bind(sockfd, (struct sockaddr *)cliaddr, addrlen) == 0)
                return 0;
            else {
                if (errno == EADDRINUSE) {
                    if (i == 1) {
                        port = (ushort) (time(0) | getpid());
                        port = ((port < 1024) ? (port + 1024) : port);
                    }
                    else {
                        port++;
                        port = ((port < 1024) ? (port + 1024) : port);
                    }
                    ls_syslog(LOG_ERR,(_i18n_msg_get(ls_catd,NL_SETN,5650,
                                                     "%s: retry <%d> times, port <%d> will be bound" /* catgets 5650 */)),
                              "Bind_", i, port);
                    cliaddr->sin_port = htons(port);
                }
                else
                    return (-1);
            }
        }
        ls_syslog(LOG_ERR, I18N_FUNC_D_FAIL_M, "Bind_", "bind", BIND_RETRY_TIMES);
        return (-1);
    }
}

const char *
getCmdPathName_(const char *cmdStr, int* cmdLen)
{
    char* pRealCmd;
    char* sp1;
    char* sp2;

    for (pRealCmd = (char*)cmdStr; *pRealCmd == ' '
             || *pRealCmd == '\t'
             || *pRealCmd == '\n'
             ; pRealCmd++);

    if (pRealCmd[0] == '\'' || pRealCmd[0] == '"') {
        sp1 = &pRealCmd[1];
        sp2 = strchr(sp1, pRealCmd[0]);
    } else {
        int i;

        sp1 = pRealCmd;
        for (i = 0; sp1[i] != '\0'; i++) {
            if (sp1[i] == ';' || sp1[i] == ' ' ||
                sp1[i] == '&' || sp1[i] == '>' ||
                sp1[i] == '<' || sp1[i] == '|' ||
                sp1[i] == '\t' || sp1[i] == '\n')
                break;
        }

        sp2 = &sp1[i];
    }

    if (sp2) {
        *cmdLen = sp2 - sp1;
    } else {
        *cmdLen = strlen(sp1);
    }
    return sp1;
}

int
replace1stCmd_(const char* oldCmdArgs, const char* newCmdArgs,
               char* outCmdArgs, int outLen)
{
    const char *sp1;
    const char *sp2;
    int len2;
    const char *sp3;
    char *curSp;
    const char* newSp;
    int newLen;
    int len;

    newSp = getCmdPathName_(newCmdArgs, &newLen);
    sp1 = oldCmdArgs;
    sp2 = getCmdPathName_(sp1, &len2);
    if (newLen - len2 + strlen(sp1) >= outLen) {
        return -1;
    }
    sp3 = sp2 + len2;

    len = sp2 - sp1;
    curSp = memcpy(outCmdArgs, sp1, len);
    curSp = memcpy(curSp + len, newSp, newLen);
    strcpy(curSp + newLen, sp3);

    return 0;
}

const char*
getLowestDir_(const char* filePath)
{
    static char dirName[MAXFILENAMELEN];
    const char *sp1, *sp2;
    int len;

    sp1 = strrchr(filePath, '/');
    if (sp1 == NULL) {
        sp1 = filePath;
    }
    sp2 = strrchr(filePath, '\\');
    if (sp2 == NULL) {
        sp2 = filePath;
    }
    len = (sp2 > sp1) ? sp2-filePath : sp1-filePath;

    if(len) {
        memcpy(dirName, filePath, len);
        dirName[len] = 0;
    } else {
        return NULL;
    }

    return dirName;
}

void
getLSFAdmins_(void)
{
    struct clusterInfo    *clusterInfo;
    int i;

    clusterInfo = ls_clusterinfo(NULL, NULL, NULL, 0, 0);
    if (clusterInfo == NULL) {
        return;
    }

    if (LSFAdmins.numAdmins != 0) {
        FREEUP(LSFAdmins.names);
    }

    LSFAdmins.numAdmins = clusterInfo->nAdmins;

    LSFAdmins.names = calloc(LSFAdmins.numAdmins, sizeof(char *));
    if (LSFAdmins.names == NULL) {
        LSFAdmins.numAdmins = 0;
        return;
    }

    for (i = 0; i < LSFAdmins.numAdmins; i ++) {
        LSFAdmins.names[i] = putstr_(clusterInfo->admins[i]);
        if (LSFAdmins.names[i] == NULL) {
            int j;

            for (j = 0; j < i; j ++) {
                FREEUP(LSFAdmins.names[j]);
            }
            FREEUP(LSFAdmins.names);
            LSFAdmins.numAdmins = 0;

            return;
        }
    }
}

bool_t
isLSFAdmin_(const char *name)
{
    int    i;

    for (i = 0; i < LSFAdmins.numAdmins; i++) {
        if (strcmp(name, LSFAdmins.names[i]) == 0) {
            return(TRUE);
        }
    }

    return(FALSE);

}

int
ls_strcat(char *trustedBuffer, int bufferLength, char *strToAdd)
{
    int start = strlen(trustedBuffer);
    int remainder = bufferLength - start;
    int i;

    if ((start > bufferLength) || strToAdd == NULL) {
        return -1;
    }

    for (i = 0; i < remainder; i++) {
        trustedBuffer[start+i] = strToAdd[i];
        if (strToAdd[i] == '\0' ) {
            break;
        }
    }
    if (i == remainder) {
        trustedBuffer[bufferLength-1] = '\0';
        return -1;
    }

    return 0;
}

/* Every even has a header like that:
 * EVENT_TYPE openlavaversion unixtime
 */

static char *emap[] =
{
    "LIM_START",
    "LIM_SHUTDOWN",
    "ADD_HOST",
    "REMOVE_HOST"
};

static int writeEventHeader(FILE *, struct lsEventRec *);
static int writeAddHost(FILE *, struct lsEventRec *);
static int writeRmHost(FILE *, struct lsEventRec *);
static int readEventHeader(char *, struct lsEventRec *);
static int readAddHost(char *, struct lsEventRec *);
static int readRmHost(char *, struct lsEventRec *);
static char *getstr_(char *);

/* ls_readeventrec()
 */
struct lsEventRec *
ls_readeventrec(FILE *fp)
{
    static struct lsEventRec ev;
    static char ebuf[BUFSIZ];
    int cc;
    char *p;

    if (fp == NULL) {
        lserrno = LSE_BAD_ARGS;
        return NULL;
    }

    if (! fgets(ebuf, BUFSIZ, fp)) {
        lserrno = LSE_EOF;
        return NULL;
    }

    p = ebuf;
    cc = readEventHeader(ebuf, &ev);
    if (cc < 0) {
        lserrno = LSE_FILE_CLOSE;
        return NULL;
    }
    /* move ahead since we consumed
     * the header.
     */
    p = p + cc;

    switch (ev.event) {
        case EV_LIM_START:
            ev.record = NULL;
            break;
        case EV_LIM_SHUTDOWN:
            ev.record = NULL;
            break;
        case EV_ADD_HOST:
            readAddHost(p, &ev);
            break;
        case EV_REMOVE_HOST:
            readRmHost(p, &ev);
            break;
        case EV_EVENT_LAST:
            break;
    }

    return &ev;
}

/* ls_writeeventrec()
 */
int
ls_writeeventrec(FILE *fp,
                 struct lsEventRec *ev)
{
    if (fp == NULL
        || ev == NULL) {
        lserrno = LSE_BAD_ARGS;
        return -1;
    }

    writeEventHeader(fp, ev);

    switch (ev->event) {
        case EV_LIM_START:
            fputc('\n', fp);
            break;
        case EV_LIM_SHUTDOWN:
            fputc('\n', fp);
            break;
        case EV_ADD_HOST:
            writeAddHost(fp, ev);
            break;
        case EV_REMOVE_HOST:
            writeRmHost(fp, ev);
            break;
        case EV_EVENT_LAST:
            break;
    }

    fflush(fp);

    return 0;
}
/* writeEventHeader()
 */
static int
writeEventHeader(FILE *fp,
                 struct lsEventRec *ev)
{
    if (ev->event < EV_LIM_START
        || ev->event >= EV_EVENT_LAST) {
        lserrno = LSE_BAD_ARGS;
        return -1;
    }

    fprintf(fp, "\
%s %hu %lu ", emap[ev->event], ev->version, ev->etime);

    fflush(fp);

    return 0;
}

/* writeAddHost()
 */
static int
writeAddHost(FILE *fp,
             struct lsEventRec *ev)
{
    struct hostEntryLog *hPtr;
    int cc;

    hPtr = ev->record;

    fprintf(fp, "\
\"%s\" \"%s\" \"%s\" %d %d %4.2f %d ",
            hPtr->hostName, hPtr->hostModel, hPtr->hostType,
            hPtr->rcv, hPtr->nDisks, hPtr->cpuFactor, hPtr->numIndx);

    for (cc = 0; cc < hPtr->numIndx; cc++)
        fprintf(fp, "%4.2f ", hPtr->busyThreshold[cc]);

    fprintf(fp, "%d ", hPtr->nRes);

    for (cc = 0; cc < hPtr->nRes; cc++)
        fprintf(fp, "\"%s\" ",  hPtr->resList[cc]);

    /* the window is really the only string that needs
     * to have quotes around as there can be spaces in it
     * or it may not be configured and does not have
     * a counter.
     */
    fprintf(fp, "%d \"%s\" ", hPtr->rexPriority, hPtr->window);

    /* new line and flush
     */
    fprintf(fp, "\n");
    fflush(fp);

    return 0;
}

/* writermHost
 */
static int
writeRmHost(FILE *fp, struct lsEventRec *ev)
{
    struct hostEntryLog *hLog;

    hLog = ev->record;

    fprintf(fp, "\"%s\" \n", hLog->hostName);
    fflush(fp);

    return 0;
}

/* readEventHeader()
 */
static int
readEventHeader(char *buf, struct lsEventRec *ev)
{
    char event[32];
    int n;
    int cc;

    cc = sscanf(buf, "\
%s %hd %lu %n", event, &ev->version, &ev->etime, &n);
    if (cc != 3)
        return -1;

    if (strcmp("LIM_START", event) == 0)
        ev->event = EV_LIM_START;
    else if (strcmp("LIM_SHUTDOWN", event) == 0)
        ev->event = EV_LIM_SHUTDOWN;
    else if (strcmp("ADD_HOST", event) == 0)
        ev->event = EV_ADD_HOST;
    else if (strcmp("REMOVE_HOST", event) == 0)
        ev->event = EV_REMOVE_HOST;
    else
        abort();

    return n;
}

/* readAddHost()
 */
static int
readAddHost(char *buf,
            struct lsEventRec *ev)
{
    int cc;
    int n;
    int i;
    struct hostEntryLog *hPtr;
    static char name[MAXLSFNAMELEN + 1];
    static char model[MAXLSFNAMELEN + 1];
    static char type[MAXLSFNAMELEN + 1];
    char *p;
    char *window;

    hPtr = calloc(1, sizeof(struct hostEntryLog));

    p = buf;
    /* name, model, type, number of disks
     * and cpu factor.
     */
    cc = sscanf(p, "\
%s%s%s%d%d%f%d%n", name, model, type,
                &hPtr->rcv, &hPtr->nDisks, &hPtr->cpuFactor,
                &hPtr->numIndx, &n);
    if (cc != 7) {
        free(hPtr);
        return -1;
    }
    p = p + n;

    strcpy(hPtr->hostName, getstr_(name));
    strcpy(hPtr->hostModel, getstr_(model));
    strcpy(hPtr->hostType, getstr_(type));

    /* load indexes
     */
    hPtr->busyThreshold = calloc(hPtr->numIndx, sizeof(float));
    for (i = 0; i < hPtr->numIndx; i++) {
        cc = sscanf(p, "%f%n", &hPtr->busyThreshold[i], &n);
        if (cc != 1)
            goto out;
        p = p + n;
    }

    /* resources
     */
    cc = sscanf(p, "%d%n", &hPtr->nRes, &n);
    if (cc != 1)
        goto out;
    if (hPtr->nRes > 0)
        hPtr->resList = calloc(hPtr->nRes, sizeof(char *));
    p = p + n;

    for (i = 0; i < hPtr->nRes; i++) {
        cc = sscanf(p, "%s%n", name, &n);
        if (cc != 1)
            goto out;
        hPtr->resList[i] = strdup(getstr_(name));
        p = p + n;
    }

    cc = sscanf(p, "%d%s%n", &hPtr->rexPriority, name, &n);
    if (cc != 2)
        goto out;
    p = p + n;

    /* windows are coded bit funny, addHost()
     * will handle the NULL pointer all right.
     */
    window = getstr_(name);
    if (window[0] != 0)
        hPtr->window = strdup(window);
    else
        hPtr->window = NULL;

    /* at last my baby is comin' home
     */
    ev->record = hPtr;

    return 0;

out:
    FREEUP(hPtr->busyThreshold);
    FREEUP(hPtr);

    return -1;
}

/* readRmHost()
 */
static int
readRmHost(char *buf,
           struct lsEventRec *ev)
{
    struct hostEntryLog *hPtr;
    static char name[MAXLSFNAMELEN + 1];

    hPtr = calloc(1, sizeof(struct hostEntryLog));

    sscanf(buf, "%s", name);
    strcpy(hPtr->hostName, getstr_(name));

    ev->record = hPtr;

    return 0;
}


/* getstr_()
 * Strip the quotes around the string
 */
static char *
getstr_(char *s)
{
    static char buf[MAXLSFNAMELEN + 1];
    char *p;

    p = buf;
    if (s[0] == '"'
        && s[1] == '"')
        return "";

    ++s;
    while (*s != '"')
        *p++ = *s++;

    *p = 0;

    return buf;
}

/* freeHostEntryLog()
 */
int
freeHostEntryLog(struct hostEntryLog **hPtr)
{
    int cc;

    if (hPtr == NULL
        || *hPtr == NULL)
        return -1;

    FREEUP((*hPtr)->busyThreshold);
    for (cc = 0; cc < (*hPtr)->nRes; cc++)
        FREEUP((*hPtr)->resList[cc]);
    FREEUP((*hPtr)->resList);
    FREEUP((*hPtr)->window);
    FREEUP(*hPtr);

    return 0;
}

/*Reflect the value of the LSF_NON_PRIVILEGED_PORTS configuration*/
int getNonPrivilegedPorts() {
    if (genParams_[LSF_NON_PRIVILEGED_PORTS].paramValue != NULL
        && strcasecmp(genParams_[LSF_NON_PRIVILEGED_PORTS].paramValue, "n") == 0) {
        return 0;
    } else {
        return 1;
    }
}

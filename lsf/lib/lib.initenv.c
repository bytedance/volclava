/*
 * Copuright (C) 2011 David Bigagli
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
#include <unistd.h>
#include <stdlib.h>
#include "lib.h"
#include "lproto.h"
#include "lib.osal.h"

struct config_param genParams_[] =
{
    {"LSF_CONFDIR", NULL},
    {"LSF_SERVERDIR", NULL},
    {"LSF_LIM_DEBUG", NULL},
    {"LSF_RES_DEBUG", NULL},
    {"LSF_STRIP_DOMAIN", NULL},
    {"LSF_LIM_PORT", NULL},
    {"LSF_RES_PORT", NULL},
    {"LSF_LOG_MASK", NULL},
    {"LSF_SERVER_HOSTS", NULL},
    {"LSF_AUTH", NULL},
    {"LSF_USE_HOSTEQUIV", NULL},
    {"LSF_ID_PORT", NULL},
    {"LSF_RES_TIMEOUT", NULL},
    {"LSF_API_CONNTIMEOUT", NULL},
    {"LSF_API_RECVTIMEOUT", NULL},
    {"LSF_AM_OPTIONS", NULL},
    {"LSF_TMPDIR", NULL},
    {"LSF_LOGDIR", NULL},
    {"LSF_SYMBOLIC_LINK", NULL},
    {"LSF_MASTER_LIST", NULL},
    {"LSF_MLS_LOG", NULL},
    {"LSF_INTERACTIVE_STDERR", NULL},
    {"HOSTS_FILE", NULL},
    {"LSB_SHAREDIR", NULL},
    {"LSF_NON_PRIVILEGED_PORTS", NULL},
    {NULL, NULL}
};

char *stripDomains_ = NULL;
int  errLineNum_ = 0;
char *lsTmpDir_;


static int parseLine(char *line, char **keyPtr, char **valuePtr);
static int matchEnv(char *, struct config_param *);
static int setConfEnv(char *, char *, struct config_param *);

static int
doEnvParams_(struct config_param *plp)
{
    char *sp;
    char *spp;

    if (!plp)
        return(0);

    for (; plp->paramName != NULL ; plp++) {
        if ((sp = getenv(plp->paramName)) != NULL) {
            if (NULL == (spp = putstr_(sp))) {
                lserrno = LSE_MALLOC;
                return(-1);
            }
            FREEUP(plp->paramValue);
            plp->paramValue = spp;
        }
    }
    return(0);
}

char *
getTempDir_(void)
{
    static char *sp = NULL;
    char *tmpSp = NULL;
    struct stat stb;

    if (sp) {
        return(sp);
    }

    tmpSp = genParams_[LSF_TMPDIR].paramValue;
    if ((tmpSp != NULL) && (stat(tmpSp, &stb) == 0)
        && (S_ISDIR(stb.st_mode))) {
        sp = tmpSp;
    } else {

        tmpSp = getenv("TMPDIR");
        if ((tmpSp != NULL) && (stat(tmpSp, &stb) == 0)
            && (S_ISDIR(stb.st_mode))) {

            sp = putstr_( tmpSp );
        }

    }

    if (sp == NULL) {
        sp = "/tmp";
    }

    return sp;

}

int
initenv_(struct config_param *userEnv, char *pathname)
{
    int Error = 0;
    char *envdir;
    static int lsfenvset = FALSE;

    if (osInit_() < 0) {
        return(-1);
    }

    if ((envdir = getenv("LSF_ENVDIR")) != NULL)
        pathname = envdir;
    else if (pathname == NULL)
        pathname = LSETCDIR;

    if (lsfenvset) {
        if (userEnv == NULL) {
            return (0);
        }
        if (readconfenv_(NULL, userEnv, pathname) < 0) {
            return (-1);
        } else if (doEnvParams_(userEnv) < 0) {
            return (-1);
        }
        return (0);
    }

    if (readconfenv_(genParams_, userEnv, pathname) < 0)
        return (-1);
    else {
        if (doEnvParams_(genParams_) < 0)
            return (-1);
        lsfenvset = TRUE;
        if (doEnvParams_(userEnv) < 0)
            Error = 1;
    }

    if (! genParams_[LSF_CONFDIR].paramValue ||
        ! genParams_[LSF_SERVERDIR].paramValue) {
        lserrno = LSE_BAD_ENV;
        return(-1);
    }

    if (genParams_[LSF_SERVER_HOSTS].paramValue != NULL) {
        char *sp;
        for (sp=genParams_[LSF_SERVER_HOSTS].paramValue;*sp != '\0'; sp++)
            if (*sp == '\"')
                *sp = ' ';
    }

    lsTmpDir_ = getTempDir_();

    if (Error)
        return(-1);

    return(0);
}

int
ls_readconfenv(struct config_param *paramList, char *confPath)
{
    return (readconfenv_(NULL, paramList, confPath));
}

int
readconfenv_(struct config_param *pList1,
             struct config_param *pList2,
             char *confPath)
{
    char *key;
    char *value;
    char *line;
    FILE *fp;
    char filename[MAXFILENAMELEN];
    struct config_param *plp;
    int lineNum = 0, saveErrNo;

    if (pList1)
        for (plp = pList1; plp->paramName != NULL; plp++) {
            if (plp->paramValue != NULL) {

                lserrno = LSE_BAD_ARGS;
                return(-1);
            }
        }

    if (pList2)
        for (plp = pList2; plp->paramName != NULL; plp++) {
            if (plp->paramValue != NULL) {

                lserrno = LSE_BAD_ARGS;
                return(-1);
            }
        }
    if (confPath) {
        {
            memset(filename,0, sizeof(filename));
            ls_strcat(filename,sizeof(filename),confPath);
            ls_strcat(filename,sizeof(filename),"/lsf.conf");
            fp = fopen(filename, "r");
        }
    } else {
        char *ep = getenv("LSF_ENVDIR");
        char buf[MAXFILENAMELEN];

        if (ep == NULL) {
            sprintf(buf,"%s/lsf.conf",LSETCDIR);
            fp = fopen(buf, "r");
        } else {
            memset(buf,0, sizeof(buf));
            ls_strcat(buf,sizeof(buf),ep);
            ls_strcat(buf,sizeof(buf),"/lsf.conf");
            fp = fopen(buf, "r");
        }
    }

    if (!fp) {

        lserrno = LSE_LSFCONF;
        return(-1);
    }

    lineNum = 0;
    errLineNum_ = 0;
    while ((line = getNextLineC_(fp, &lineNum, TRUE)) != NULL) {
        int cc;
        cc = parseLine(line, &key, &value);
        if (cc < 0 && errLineNum_ == 0) {
            errLineNum_ = lineNum;
            saveErrNo = lserrno;
            continue;
        }
        if (! matchEnv(key, pList1) && ! matchEnv(key, pList2))
            continue;

        if (!setConfEnv(key, value, pList1)
            || !setConfEnv(key, value, pList2)) {
            fclose(fp);
            return(-1);
        }
    }
    fclose(fp);
    if (errLineNum_ != 0) {
        lserrno = saveErrNo;
        return(-1);
    }

    return(0);

}


static int
parseLine(char *line, char **keyPtr, char **valuePtr)
{
    char *sp = line;
#define L_MAXLINELEN_4ENV (8*MAXLINELEN)
    static char key[L_MAXLINELEN_4ENV];
    static char value[L_MAXLINELEN_4ENV];
    char *word;
    char *cp;

    if( strlen(sp)>= L_MAXLINELEN_4ENV-1 )
    {
        lserrno = LSE_BAD_ENV;
        return -1;
    }

    *keyPtr = key;
    *valuePtr = value;

    word = getNextWord_(&sp);

    strcpy(key, word);
    cp = strchr(key, '=');

    if (cp == NULL) {
        lserrno = LSE_CONF_SYNTAX;
        return -1;
    }

    *cp = '\0';

    sp = strchr(line, '=');

    if (sp[1] == ' ' || sp[1] == '\t') {
        lserrno = LSE_CONF_SYNTAX;
        return -1;
    }

    if (sp[1] == '\0') {
        value[0] = '\0';
        return 0;
    }

    sp++;
    word = getNextValueQ_(&sp, '\"', '\"');
    if (!word)
        return -1;

    strcpy(value, word);

    word = getNextValueQ_(&sp, '\"', '\"');
    if (word != NULL || lserrno != LSE_NO_ERR) {
        lserrno = LSE_CONF_SYNTAX;
        return -1;
    }

    return 0;

}

static int
matchEnv(char *name, struct config_param *paramList)
{
    if (paramList == NULL)
        return FALSE;

    for (; paramList->paramName; paramList++)
        if (strcmp(paramList->paramName, name) == 0)
            return TRUE;

    return FALSE;
}

static int
setConfEnv (char *name, char *value, struct config_param *paramList)
{
    if (paramList == NULL)
        return(1);

    if (value == NULL)
        value = "";

    for ( ; paramList->paramName; paramList++) {
        if (strcmp(paramList->paramName, name) == 0) {
            FREEUP (paramList->paramValue);
            paramList->paramValue = putstr_(value);
            if (paramList->paramValue == NULL) {
                lserrno = LSE_MALLOC;
                return(0);
            }
        }
    }
    return(1);
}


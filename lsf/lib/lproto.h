/* Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 * $Id: lproto.h 397 2007-11-26 19:04:00Z mblack $
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

#ifndef LPROTO_
#define LPROTO_

#include "../lsf.h"
#include "lib.table.h"
#include "lib.hdr.h"
#include "lib.channel.h"
#include "../res/resout.h"
#include "lib.pim.h"
#include "lsi18n.h"

#define BIND_RETRY_TIMES 100

struct keymap {
    char *key;
    char *val;
    int   position;
};

struct admins {
    int nAdmins;
    int *adminIds;
    int *adminGIds;
    char **adminNames;
};

struct debugReq {
    int opCode;
    int level;
    int logClass;
    int options;
    char *hostName;
    char logFileName[MAXPATHLEN];
};

extern void putMaskLevel(int, char **);
extern bool_t xdr_debugReq (XDR *,
                            struct debugReq  *,
                            struct LSFHeader *);
#define    MBD_DEBUG         1
#define    MBD_TIMING        2
#define    SBD_DEBUG         3
#define    SBD_TIMING        4
#define    LIM_DEBUG         5
#define    LIM_TIMING        6
#define    RES_DEBUG         7
#define    RES_TIMING        8

struct resPair {
   char *name;
   char *value;
};

struct sharedResource {
    char *resourceName;
    int  numInstances;
    struct resourceInstance **instances;
};

struct resourceInfoReq {
     int  numResourceNames;
     char **resourceNames;
     char *hostName;
     int  options;
};

struct resourceInfoReply {
    int    numResources;
    struct lsSharedResourceInfo *resources;
    int    badResource;
};

struct lsbShareResourceInfoReply {
    int    numResources;
    struct lsbSharedResourceInfo *resources;
    int    badResource;
};

#define HOST_ATTR_SERVER        (0x00000001)
#define HOST_ATTR_CLIENT        (0x00000002)
#define HOST_ATTR_NOT_LOCAL     (0x00000004)
#define HOST_ATTR_NOT_READY     (0xffffffff)

extern int sharedResConfigured_;

#define VALID_IO_ERR(x) ((x) == EWOULDBLOCK || (x) == EINTR || (x) == EAGAIN)
#define BAD_IO_ERR(x)   ( ! VALID_IO_ERR(x))

#define INVALID_FD      (-1)
#define FD_IS_VALID(x)  ((x) >= 0 && (x) < sysconf(_SC_OPEN_MAX) )
#define FD_NOT_VALID(x) ( ! FD_IS_VALID(x))

#define AUTH_IDENT      "ident"
#define AUTH_PARAM_DCE  "dce"
#define AUTH_PARAM_EAUTH  "eauth"
#define AUTOMOUNT_LAST_STR "AMFIRST"
#define AUTOMOUNT_NEVER_STR "AMNEVER"

#define FREEUP(pointer)   if (pointer != NULL) {  \
                              free(pointer);      \
                              pointer = NULL;     \
                          }

#define STRNCPY(str1, str2, len)  { strncpy(str1, str2, len); \
                                    str1[len -1] = '\0';  \
                                  }

#define IS_UNC(a) \
        ((a!=NULL) && (*a == '\\') && (*(a+1) == '\\') ? TRUE : FALSE)

#define TRIM_LEFT(sp) if (sp != NULL) { \
                          while (isspace(*(sp))) (sp)++; \
                      }
#define TRIM_RIGHT(sp)     while (isspace(*(sp+strlen(sp)-1))) *(sp+strlen(sp)-1)='\0';

#define ALIGNWORD_(s)    (((s)&0xfffffffc) + 4)
#define NET_INTADDR_(a) ((char *) (a))

#define NET_INTSIZE_ 4

#define XDR_DECODE_SIZE_(a) (a)

#define LS_EXEC_T "LS_EXEC_T"


#define GET_INTNUM(i) ((i)/INTEGER_BITS + 1)
#define SET_BIT(bitNo, integers)           \
    integers[(bitNo)/INTEGER_BITS] |= (1<< (bitNo)%INTEGER_BITS);
#define CLEAR_BIT(bitNo, integers)           \
    integers[(bitNo)/INTEGER_BITS] &= ~(1<< (bitNo)%INTEGER_BITS);
#define TEST_BIT(bitNo, integers, isSet)  \
   {  \
      if (integers[(bitNo)/INTEGER_BITS] & (1<<(bitNo)%INTEGER_BITS))  \
          isSet = 1;         \
      else                   \
          isSet = 0;         \
   }

#define FOR_EACH_WORD_IN_SPACE_DELIMITED_STRING(String, Word) \
    if ((String) != NULL) { \
        char *Word; \
        while (((Word) = getNextWord_(&String)) != NULL) { \

#define END_FOR_EACH_WORD_IN_SPACE_DELIMITED_STRING }}

#define LSF_LIM_ERESOURCE_OBJECT        "liblimvcl.so"
#define LSF_LIM_ERESOURCE_VERSION       "lim_vcl_get_eres_version"
#define LSF_LIM_ERESOURCE_DEFINE        "lim_vcl_get_eres_def"
#define LSF_LIM_ERESOURCE_LOCATION      "lim_vcl_get_eres_loc"
#define LSF_LIM_ERESOURCE_VALUE         "lim_vcl_get_eres_val"
#define LSF_LIM_ERES_TYPE "!"

extern int lsResMsg_ (int, resCmd, char *, char *, int,
                      bool_t (*)(), int *, struct timeval *);
extern int expectReturnCode_(int, int, struct LSFHeader *);
extern int ackAsyncReturnCode_(int, struct LSFHeader *);
extern int resRC2LSErr_(int);
extern int ackReturnCode_(int);


#define LSF_O_RDONLY    00000
#define LSF_O_WRONLY    00001
#define LSF_O_RDWR      00002
#define LSF_O_NDELAY    00004
#define LSF_O_NONBLOCK  00010
#define LSF_O_APPEND    00020
#define LSF_O_CREAT     00040
#define LSF_O_TRUNC     00100
#define LSF_O_EXCL      00200
#define LSF_O_NOCTTY    00400

#define LSF_O_CREAT_DIR 04000

extern int getConnectionNum_(char *hostName);
extern void inithostsock_(void);

extern int initenv_(struct config_param *, char *);
extern char *lsTmpDir_;
extern short getMasterCandidateNoByName_(char *);
extern char *getMasterCandidateNameByNo_(short);
extern int getNumMasterCandidates_();
extern int initMasterList_();
extern int getIsMasterCandidate_();
extern void freeupMasterCandidate_(int);
extern char *resetLSFUsreDomain(char *);


extern int runEsub_(struct lenData *, char *);
extern int runEexec_(char *, int, struct lenData *, char *);
extern int runEClient_(struct lenData *, char **);
extern char *runEGroup_(char *, char *);

extern int getAuth_(struct lsfAuth *, char *);
extern int verifyEAuth_(struct lsfAuth *, struct sockaddr_in *);
extern int putEauthClientEnvVar(char *);
extern int putEauthServerEnvVar(char *);

extern void sw_remtty(int);
extern void sw_loctty(int);

extern int doAcceptResCallback_(int s, struct niosConnect *connReq);
extern int niosCallback_(struct sockaddr_in *from, u_short port,
           int rpid, int exitStatus, int terWhiPendStatus);

extern int sig_encode(int);
extern int sig_decode(int);
extern int getSigVal (char *);
extern char *getSigSymbolList (void);
extern char *getSigSymbol (int);
extern void (*Signal_ (int, void (*)(int)))(int);
extern int blockALL_SIGS_(sigset_t *, sigset_t *);

extern int TcpCreate_(int, int);

extern int encodeTermios_(XDR *, struct termios *);
extern int decodeTermios_(XDR *, struct termios *);
extern int rstty_(char *host);
extern int rstty_async_(char *host);
extern int do_rstty_(int, int, int);

extern char isanumber_(char *);
extern char islongint_(char *);
extern char isint_(char *);
extern int isdigitstr_(char *);
extern char *putstr_ (const char *);
extern int ls_strcat(char *,int,char *);
extern char *mygetwd_(char *);
extern char *chDisplay_(char *);
extern void initLSFHeader_(struct LSFHeader *);
extern struct group *mygetgrnam( const char *name);
extern void *myrealloc(void *ptr, size_t size);
extern char *getNextToken(char **sp);
extern int getValPair(char **resReq, int *val1, int *val2);
extern char *my_getopt (int nargc, char **nargv, char *ostr, char **errMsg);
extern int putEnv(char *env, char *val);
extern int Bind_(int, struct sockaddr *, int);
extern const char* getCmdPathName_(const char *cmdStr, int* cmdLen);
extern int replace1stCmd_(const char* oldCmdArgs, const char* newCmdArgs,
                 char* outCmdArgs, int outLen);
extern const char* getLowestDir_(const char* filePath);
extern void getLSFAdmins_(void);
extern bool_t isLSFAdmin_(const char *name);
extern int isAllowCross(char *paramValue);
extern int isMasterCrossPlatform(void);
extern LS_LONG_INT atoi64_(char *);

extern void stripDomain_(char *);
extern int equalHost_(const char *, const char *);
extern char *sockAdd2Str_(struct sockaddr_in *);

extern struct hostent *Gethostbyname_ (char *);
extern struct hostent *Gethostbyaddr_(in_addr_t *, socklen_t, int);
extern int getAskedHosts_(char *, char ***, int *, int *, int);
extern int lockHost_(time_t, char *);
extern int unlockHost_(char *);

extern int lsfRu2Str(FILE *, struct lsfRusage *);
extern int  str2lsfRu(char *, struct lsfRusage *, int *);
extern void lsfRusageAdd_(struct lsfRusage *, struct lsfRusage *);

extern void inserttask_(char *, hTab *);
extern int deletetask_(char *, hTab *);
extern int listtask_(char ***, hTab *, int);
extern int readtaskfile_(char *, hTab *, hTab *, hTab *, hTab *, char);
extern int writetaskfile_(char *, hTab *, hTab *, hTab *, hTab *);

extern int expSyntax_(char *);

extern char *getNextLineC_(FILE *, int *, int);
extern char *getNextLine_(FILE *, int);
extern char *getNextWord_(char **);
extern char * getNextWord1_(char **line);
extern char *getNextWordSet(char **, const char *);
extern char * getline_(FILE *fp, int *);
extern char * getThisLine_(FILE *fp, int *LineCount);
extern char * getNextValueQ_(char **, char, char);
extern int  stripQStr (char *q, char *str);
extern int addQStr (FILE *, char *str);
extern struct pStack *initStack(void);
extern int pushStack(struct pStack *, struct confNode *);
extern struct confNode * popStack(struct pStack *);
extern void freeStack(struct pStack *);

extern char *getNextLineD_(FILE *, int *, int);
extern char *getNextLineC_conf(struct lsConf *, int *, int);
extern char *getNextLine_conf(struct lsConf *, int);
extern char *nextline_(FILE *);
extern void subNewLine_(char*);

extern void doSkipSection(FILE *, int *, char *, char *);
extern int isSectionEnd (char *, char *, int *, char *);
extern int keyMatch (struct keymap *keyList, char *line, int exact);
extern int mapValues (struct keymap *keyList, char *line);
extern int readHvalues(struct keymap *, char *, FILE *, char *, int *, int, char *);
extern char *getNextValue(char **line);
extern int putValue(struct keymap *keyList, char *key, char *value);
extern char *getBeginLine(FILE *, int *);
extern int putInLists (char *, struct admins *, int *, char *);
extern int isInlist (char **, char *, int);

extern void doSkipSection_conf(struct lsConf *, int *, char *, char *);
extern char *getBeginLine_conf(struct lsConf *, int *);

extern void defaultAllHandlers(void);

extern int nb_read_fix(int, char *, int);
extern int nb_write_fix(int, char *, int);
extern int nb_read_timeout(int, char *, int, int);
extern int nb_write_timeout(int, char *, int, int);
extern int b_read_fix(int, char *, int);
extern int b_write_fix(int, char *, int);
extern int b_write_timeout(int, char *, int, int);
extern int detectTimeout_(int , int);
extern int b_connect_(int, struct sockaddr *, int, int);
extern int rd_select_(int, struct timeval *);
extern int b_accept_(int, struct sockaddr *, socklen_t *);
extern int blockSigs_(int, sigset_t *, sigset_t *);

extern int readDecodeHdr_ (int s, char *buf, int (*readFunc)(), XDR *xdrs,
                           struct LSFHeader *hdr);
extern int readDecodeMsg_ (int s, char *buf, struct LSFHeader *hdr,
                           int (*readFunc)(), XDR *xdrs, char *data,
                           bool_t (*xdrFunc)(), struct lsfAuth *auth);
extern int writeEncodeMsg_(int, char *, int, struct LSFHeader *, char *,
                           int (*)(), bool_t (*)(), int);
extern int writeEncodeHdr_(int, struct LSFHeader *, int (*)());
extern int lsSendMsg_(int, int, int, char *, char *, int, bool_t (*)(),
                      int (*)(), struct lsfAuth *);
extern int lsRecvMsg_(int, char *, int, struct LSFHeader *, char *,
                      bool_t (*)(), int (*)());

extern int io_nonblock_(int);
extern int io_block_(int);

extern void millisleep_(int);

extern void rlimitEncode_(struct lsfLimit *, struct rlimit *, int);
extern void rlimitDecode_(struct lsfLimit *, struct rlimit *, int);

extern void verrlog_(int level, FILE *fp, const char *fmt, va_list ap);

extern int errnoEncode_(int);
extern int errnoDecode_(int);

extern int getLogClass_ (char *, char *);
extern int getLogMask(char **, char *);
extern void ls_openlog(const char *, const char *, int, char *);
extern void ls_closelog(void);
extern int  ls_setlogmask(int maskpri);
extern int  ls_getlogmask();

extern void initkeylist(struct keymap *, int, int, struct lsInfo *);
extern void freekeyval(struct keymap *);
extern char *parsewindow(char *, char *, int *, char *);

extern int expandList_(char ***, int, char **);
extern int expandList1_(char ***, int, int *, char **);

extern int osInit_(void);
extern char *osPathName_(char *);
extern char *osPathName_(char *);
extern char *osHomeEnvVar_(void);
extern int  osProcAlive_(int);
extern void osConvertPath_(char *);

extern void xdr_lsffree(bool_t (*)(), char *, struct LSFHeader *);

extern int createUtmpEntry(char *, pid_t, char *);
extern int removeUtmpEntry(pid_t);

extern int createSpoolSubDir(const char *);


extern struct passwd *getpwlsfuser_(const char *lsfUserName);
extern struct passwd *getpwdirlsfuser_(const char *lsfUserName);

extern int getLSFUser_(char *lsfUserName, unsigned int lsfUserNameSize);
extern int getLSFUserByName_(const char *osUserName,
                             char *lsfUserName, unsigned int lsfUserNameSize);
extern int getLSFUserByUid_(uid_t uid, char *lsfUserName, unsigned int lsfUserNameSize);

extern int getOSUserName_(const char *lsfUserName,
                          char *osUserName, unsigned int osUserNameSize);
extern int getOSUid_(const char *lsfUserName, uid_t *uid);

#endif

/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: cmd.h 397 2007-11-26 19:04:00Z mblack $
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
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include "../../lsf/lsf.h"
#include "../../lsf/lib/lib.hdr.h"
#include "../lsbatch.h"

#include "../../lsf/lib/lproto.h"
#include "../../lsf/lib/lib.table.h"
#include "../../lsf/lib/mls.h"

#define MIN_CPU_TIME 0.0001 

#define SIGCHK   -1
#define SIGDEL   -2
#define SIGFORCE -3   

#define MAX_JOB_IDS  100

#define CMD_BSUB            1
#define CMD_BRESTART        2
#define CMD_BMODIFY         3

#define LONG_FORMAT     1
#define WIDE_FORMAT     2
#define UF_FORMAT       3
#define O_FORMAT        4

#define QUEUE_HIST      1
#define HOST_HIST       2
#define MBD_HIST        3
#define SYS_HIST        4

#define BJOBS_PRINT             1
#define BHIST_PRINT_PRE_EXEC    2
#define BHIST_PRINT_JOB_CMD     3

#ifndef MIN
#define MIN(x,y)        ((x) < (y) ? (x) : (y))
#endif 

#ifndef MAX
#define MAX(x,y)        ((x) > (y) ? (x) : (y))
#endif 

#define TRUNC_STR(s,len) \
{ \
    int mystrlen = strlen(s); \
    if (mystrlen > (len)) \
    {\
        s[0] = '*';\
         \
        memmove((s) + 1, (s) + mystrlen + 1 - (len), (len)); \
    }\
}

struct histReq {
    int    opCode;
    char   **names;
    time_t eventTime[2];
    char   *eventFileName;               
    int    found;
};

extern void prtLine(char *);
extern char *get_status(struct jobInfoEnt *job);
extern void prtHeader(struct jobInfoEnt *, int, int);
extern void prtHeaderUF(struct jobInfoEnt *, int, int);
extern void prtJobSubmit(struct jobInfoEnt *, int, int);
extern void prtJobSubmitUF(struct jobInfoEnt *, int, int);
extern void prtFileNames(struct jobInfoEnt *, int);
extern void prtFileNamesUF(struct jobInfoEnt *, int);
extern void prtSubDetails(struct jobInfoEnt *, char *, float);
extern void prtSubDetailsUF(struct jobInfoEnt *, char *, float);
extern void prtJobStart(struct jobInfoEnt *, int, int, int);
extern void prtJobStartUF(struct jobInfoEnt *, int, int, int);
extern void prtJobFinish(struct jobInfoEnt *, struct jobInfoHead *);
extern void prtJobFinishUF(struct jobInfoEnt *, struct jobInfoHead *);
extern void prtAcctFinish(struct jobInfoEnt *);
extern struct loadIndexLog *initLoadIndex(void);
extern int fillReq (int, char **, int, struct submit *, int);
extern void prtErrMsg (struct submit *, struct submitReply *);
extern void prtBTTime(struct jobInfoEnt *);
extern void prtJobReserv(struct jobInfoEnt *);
extern void prtJobReservUF(struct jobInfoEnt *);
extern void displayLong(struct jobInfoEnt *, struct jobInfoHead *, float);
extern void displayUF(struct jobInfoEnt *, struct jobInfoHead *, float, int, struct queueInfoEnt *);

extern int lsbMode_;

extern void prtBETime_(struct submit *);

extern int supportJobNamePattern(char *);


extern int  repeatedName(char *, char **, int);
extern char *jobInfoErr (LS_LONG_INT, char *, char *, char *, char *, int);
extern int printThresholds (float *, float *, int *, int *, int, struct lsInfo *);
extern void prtResourceLimit (int *, char *, float, int *);
extern int  getNames (int, char **, int, char ***, int *, char *);
extern int  getJobIds (int, char **, char *, char *, char *, char *, LS_LONG_INT **, int);
extern int  getSpecJobIds (int, char **, LS_LONG_INT **, int *);
extern int  getSpecIdxs (char *, int **);
extern int  getOneJobId (char *, LS_LONG_INT *, int);
extern int  gettimefor (char *toptarg, time_t *tTime);
extern int  skipJob(int, int *, int);

extern void prtWord(int, const char *, int);
extern void prtWord2(int, const char *, int);
extern void prtWordL(int, const char *);
extern char *prtValue(int, int);
extern char *prtFloat(int, int, float);
extern char *prtDash(int);


extern int searchEventFile(struct histReq *, int *);
extern int bmsg(int, char **);

extern void bmove (int, char **, int);

/* Structure for bsub -pack output information */
struct packOutputs {
    int lineNum;               /* Line number in the pack file */
    char *outputMSG;           /* Output message for this line */
    int packSubmitIndex;       /* Index of the packed submission */
};

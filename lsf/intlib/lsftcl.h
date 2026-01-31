/* $Id: lsftcl.h 397 2007-11-26 19:04:00Z mblack $
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
typedef struct {
    char   *name;
    int    clientData;
} attribFunc;


#define   CPUFACTOR  (nRes + numIndx)
#define   NDISK      (nRes + numIndx + 1)
#define   REXPRI     (nRes + numIndx + 2)
#define   MAXCPUS_   (nRes + numIndx + 3)
#define   MAXMEM     (nRes + numIndx + 4)
#define   MAXSWAP    (nRes + numIndx + 5)
#define   MAXTMP     (nRes + numIndx + 6)
#define   CPU_       (nRes + numIndx + 7)
#define   SERVER     (nRes + numIndx + 8)
#define   SLOTS      (nRes + numIndx + 9)

#define   HOSTTYPE   1
#define   HOSTMODEL  2
#define   HOSTSTATUS 3
#define   HOSTNAME   4
#define   LAST_STRING (HOSTNAME + 1)
#define   DEFINEDFUNCTION 5

#define   TCL_CHECK_SYNTAX  0
#define   TCL_CHECK_EXPRESSION 1

struct tclHostData {
    char    *hostName;
    int      maxCpus;
    int      maxMem;
    int      maxSwap;
    int      maxTmp;
    int      nDisks;
    short hostInactivityCount;
    int     *status;
    float   *loadIndex;
    int     rexPriority;
    char    *hostType;
    char    *hostModel;
    char    *fromHostType;
    char    *fromHostModel;
    float   cpuFactor;
    int     ignDedicatedResource;
    int     *resBitMaps;
    int     *DResBitMaps;
    int     numResPairs;
    struct resPair *resPairs;
    int      flag;
    int      overRideFromType;
    int      slots;
};

struct tclLsInfo {
    int  numIndx;
    char **indexNames;
    int  nRes;
    char **resName;
    int  *stringResBitMaps;
    int  *numericResBitMaps;
};

extern int initTcl(struct tclLsInfo *);
extern void freeTclLsInfo(struct tclLsInfo *, int);
extern int evalResReq(char *, struct tclHostData *, char);

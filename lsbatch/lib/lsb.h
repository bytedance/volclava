/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * $Id: lsb.h 397 2007-11-26 19:04:00Z mblack $
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

#include <malloc.h>

#include "../../lsf/lsf.h"
#include "../../lsf/lib/lib.hdr.h"
#include "../../lsf/lib/lproto.h"

#include "../lsbatch.h"

#include "../daemons/daemonout.h"
#include <sys/types.h>
#include <unistd.h>

#ifndef MIN
#define MIN(x,y)        ((x) < (y) ? (x) : (y))
#endif 

#ifndef MAX
#define MAX(x,y)        ((x) > (y) ? (x) : (y))
#endif 

#define DEF_COMMITTED_RUN_TIME_FACTOR 0.0

extern struct config_param lsbParams[];
extern int initenv_(struct config_param *, char *);
extern int sig_encode(int);


#define DEFAULT_API_CONNTIMEOUT 10
#define DEFAULT_API_RECVTIMEOUT 0

#define LSB_DEBUG         0
#define LSB_SHAREDIR      1
#define LSB_SBD_PORT      2
#define LSB_MBD_PORT      3
#define LSB_DEBUG_CMD     4 
#define LSB_TIME_CMD      5 
#define LSB_CMD_LOGDIR    6 
#define LSB_CMD_LOG_MASK  7 
#define LSB_API_CONNTIMEOUT 9
#define LSB_API_RECVTIMEOUT 10
#define LSB_SERVERDIR 11 
#define LSB_MODE 12      
#define LSB_SHORT_HOSTLIST 13  
#define LSB_INTERACTIVE_STDERR 14 
#define LSB_32_PAREN_ESC     15 

#define LSB_API_QUOTE_CMD     16
#define LSB_UNIT_FOR_LIMITS   17
#define LSB_QMBD_PORT   18


typedef struct lsbSubSpoolFile {
    char inFileSpool[MAXFILENAMELEN];   
    char commandSpool[MAXFILENAMELEN];  
} LSB_SUB_SPOOL_FILE_T;

extern int creat_p_socket(void);
extern int serv_connect(char *, ushort, int);
extern int getServerMsg(int, struct LSFHeader *, char **rep_buf);
extern int callmbd(char *, char *, int, char **, struct LSFHeader *, int *,
		       int (*)(), int *);
extern int cmdCallSBD_(char *, char *, int, char **, struct LSFHeader *,
		       int *);

    
extern int PutQStr(FILE *, char *);
extern int Q2Str(char *, char *);
extern int authTicketTokens_(struct lsfAuth *, char *);
    
extern char *getNextValue0(char **line, char, char);
extern int readNextPacket(char **, int, struct LSFHeader *, int);
extern void closeSession(int);
extern void upperStr(char *, char *);
extern char* getUnixSpoolDir(char *);
extern char* getNTSpoolDir(char *);
extern char *getMasterName(void);
extern ushort get_mbd_port (void);
extern ushort get_sbd_port (void);
extern ushort get_qmbd_port (void);
extern int getAuth(struct lsfAuth *);
extern int getCommonParams (struct submit  *, struct submitReq *,
                                                 struct submitReply *);
extern int getOtherParams (struct submit *, struct submitReq *, 
                                   struct submitReply *, struct lsfAuth *,
                                   LSB_SUB_SPOOL_FILE_T*);
extern int chUserRemoveSpoolFile( const char * hostName,
				  const char * spoolFile);
extern void prtBETime_(struct submit *);
extern int runBatchEsub(struct lenData *, struct submit *);

#define TRAVERSE_FS_SIZE 1    
#define TRAVERSE_FS_SACCTXDR 2
extern int traverseSATree(struct shareAcctInfoEnt *, int, int (*)(), ...);

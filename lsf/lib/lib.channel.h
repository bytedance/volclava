/* $Id: lib.channel.h 397 2007-11-26 19:04:00Z mblack $
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
#ifndef CHANNEL_H
#define CHANNEL_H
#include <sys/types.h>
#include "lib.hdr.h"

enum chanState {CH_FREE,
                CH_DISC,
                CH_PRECONN,
                CH_CONN,
                CH_WAIT,
                CH_INACTIVE    
               };

enum chanType {CH_TYPE_UDP, CH_TYPE_TCP, CH_TYPE_LOCAL, CH_TYPE_PASSIVE,
	       CH_TYPE_NAMEDPIPE};

#define CHAN_OP_PPORT  		0x01
#define CHAN_OP_CONNECT		0x02
#define CHAN_OP_RAW		0x04
#define CHAN_OP_NONBLOCK        0x10
#define CHAN_OP_CLOEXEC         0x20
#define CHAN_OP_SOREUSE         0x40


#define CHAN_MODE_BLOCK 	0x01
#define CHAN_MODE_NONBLOCK 	0x02

#define CLOSECD(c) { chanClose_((c)); (c) = -1; }

#define CHAN_INIT_BUF(b)  memset((b), 0, sizeof(struct Buffer));

struct Buffer {
    struct Buffer  *forw;
    struct Buffer  *back;
    char  *data;
    int    pos;
    int    len;
    int stashed;
};

struct Masks {
    fd_set rmask;
    fd_set wmask;
    fd_set emask;
};

typedef enum {
    EPOLL_EVENT_NONE  = 0x0,
    EPOLL_EVENT_READ  = 0x1,
    EPOLL_EVENT_WRITE = 0x2,
    EPOLL_EVENT_ERROR = 0x4
} epoll_event_t;
struct chanData {
    int  handle;		
    enum chanType type;
    enum chanState state;
    enum chanState prestate;   
    int chanerr; 
    struct Buffer *send;
    struct Buffer *recv;

    epoll_event_t readyEvents;                /*Ready events reported by the channel to the upper layer*/

};


#define  CHANE_NOERR      0
#define  CHANE_CONNECTED  1
#define  CHANE_NOTCONN    2
#define  CHANE_SYSCALL    3
#define  CHANE_INTERNAL   4
#define  CHANE_NOCHAN     5
#define  CHANE_MALLOC     6
#define  CHANE_BADHDR     7
#define  CHANE_BADCHAN    8
#define  CHANE_BADCHFD    9
#define  CHANE_NOMSG      10
#define  CHANE_CONNRESET  11
#define  CHANE_EPOLLFAIL  12


int chanInit_(void);


#define chanSend_  chanEnqueue_
#define chanRecv_  chanDequeue_

int chanOpen_(u_int, u_short, int);
int chanEnqueue_(int chfd, struct Buffer *buf);
int chanDequeue_(int chfd, struct Buffer **buf);

int chanSelect_(struct Masks *, struct Masks *, struct timeval *timeout);
int chanEpoll_(int **, struct timeval *timeout);
int chanClose_(int chfd);
void chanCloseAll_(void);
int chanSock_(int chfd);

int chanServSocket_(int, u_short, int, int);
int chanAccept_(int, struct sockaddr_in *);

int chanClientSocket_(int, int, int);
int chanConnect_(int, struct sockaddr_in *, int , int);

int chanSendDgram_(int, char *, int , struct sockaddr_in *);
int chanRcvDgram_(int , char *, int, struct sockaddr_in *, int);
int chanRpc_(int , struct Buffer *, struct Buffer *, struct LSFHeader *, int timeout); 
int chanRead_(int, char *, int);
int chanReadNonBlock_(int, char *, int, int);
int chanWrite_(int, char *, int);
int chanWriteNonBlock_(int, char *, int, int);

int chanAllocBuf_(struct Buffer **buf, int size);
int chanFreeBuf_(struct Buffer *buf);
int chanFreeStashedBuf_(struct Buffer *buf);
int chanOpenSock_(int , int);
int chanOpenPassiveSock_(int, int);
int chanSetMode_(int, int);

int chanEventsReady(int chfd, int events);
void chanClearReadyEvents(int chfd, int events);
void chanCloseEpoll();

extern int chanIndex;
extern int cherrno;
extern struct epoll_event *epoll_events;
extern int chanEpollInit();
extern int chanRegisterEpoll_(int, uint32_t);
extern int chanUpdateListenEvents(int, uint32_t);
extern int chanUnRegisterEpoll_(int);

#endif

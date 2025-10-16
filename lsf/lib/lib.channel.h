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
#include <sys/epoll.h>
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

#define INVALID_HANDLE  -1
#define CLOSECD(c) { chanClose_((c)); (c) = -1;}

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
    EPOLL_EVENTS_NONE  = 0,
    EPOLL_EVENTS_READ  = 1,
    EPOLL_EVENTS_WRITE = 2,
    EPOLL_EVENTS_ERROR = 4
} epoll_events_t;

struct chanData {
    int  handle;
    enum chanType type;
    enum chanState state;
    enum chanState prestate;
    int chanerr;
    struct Buffer *send;
    struct Buffer *recv;
    epoll_events_t events;
};

extern int epoll_fd;
extern struct epoll_event *epoll_events;
extern struct chanData *channels;

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

int chanInit_(void);


#define chanSend_  chanEnqueue_
#define chanRecv_  chanDequeue_

extern int chanOpen_(u_int, u_short, int);
extern int chanEnqueue_(int chfd, struct Buffer *buf);
extern int chanDequeue_(int chfd, struct Buffer **buf);

extern int chanSelect_(struct Masks *, struct Masks *, struct timeval *timeout);
extern int chanClose_(int chfd);
extern void chanCloseAll_(void);
extern int chanSock_(int chfd);

extern int chanServSocket_(int, u_short, int, int);
extern int chanAccept_(int, struct sockaddr_in *);

extern int chanClientSocket_(int, int, int);
extern int chanConnect_(int, struct sockaddr_in *, int , int);

extern int chanSendDgram_(int, char *, int , struct sockaddr_in *);
extern int chanRcvDgram_(int , char *, int, struct sockaddr_in *, int);
extern int chanRpc_(int , struct Buffer *, struct Buffer *,
                    struct LSFHeader *, int timeout);
extern int chanRead_(int, char *, int);
extern int chanReadNonBlock_(int, char *, int, int);
extern int chanWrite_(int, char *, int);


extern int chanAllocBuf_(struct Buffer **buf, int size);
extern int chanFreeBuf_(struct Buffer *buf);
extern int chanFreeStashedBuf_(struct Buffer *buf);
extern int chanSetMode_(int, int);
extern int chanIndex;
extern int cherrno;

/*  epoll API
 */
extern int chanEpollInit_(void);
extern int chanRegisterEpoll_(int, uint32_t);
extern int chanModEpoll_(int, uint32_t);
extern int chanUnRegisterEpoll_(int);
extern int chanEpoll_(int);
extern void chanHandlePreconn(int);
extern void doread2(int);
extern void dowrite2(int);

#endif

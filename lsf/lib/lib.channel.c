/* $Id: lib.channel.c 397 2007-11-26 19:04:00Z mblack $
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
#include <fcntl.h>
#include <syslog.h>
#include "lib.h"
#include "lproto.h"
#include "lib.osal.h"

#define MAXLOOP 3000

#define DEFAULT_MAX_CHANNELS 64
#define INVALID_HANDLE  -1

#define NL_SETN   23

#define CLOSEIT(i) {                                \
        chanUnbindSock(channels[i].handle);      \                                               
        CLOSESOCKET(channels[i].handle);            \
        channels[i].state = CH_DISC;                \   
        channels[i].readyEvents = 0;                \
        channels[i].listenEvents = 0;               \
        channels[i].handle = INVALID_HANDLE; }

static struct chanData *channels;
static struct handleData *handles;
static struct epoll_event *events;
static int nReady;
int cherrno = 0;
extern int errno;
int chanIndex;
static int chanMaxSize;

extern int CreateSock_(int);

static void doread(int , struct Masks *);
static void dowrite(int, struct Masks *);
static struct Buffer *newBuf(void);
static void enqueueTail_(struct Buffer *, struct Buffer *);
static void dequeue_(struct Buffer *);
static int findAFreeChannel(void);


static void chanBindSock(int, int);
static void chanUnbindSock(int);
static void chanUpdateListenEvents(int);
static void upDateListenEvents(int);
static void chanClearReadyEvents(struct epoll_event *, int);
static void doreadEpoll(int chfd);
static void dowriteEpoll(int chfd);

int epollfd=-1;

int
chanInit_(void)
{
    static char first = TRUE;

    if (!first)
        return(0);

    first = FALSE;

    chanMaxSize = sysconf(_SC_OPEN_MAX);

    channels = calloc(chanMaxSize, sizeof(struct chanData));
    if (channels == NULL)
        return -1;

    chanIndex = 0;

    return 0;
}

int
chanServSocket_(int type, u_short port, int backlog, int options)
{
    int ch, s;
    struct sockaddr_in sin;

    if ((ch = findAFreeChannel()) < 0) {
        lserrno = LSE_NO_CHAN;
        return(-1);
    }

    s = socket(AF_INET, type, 0);

    if (SOCK_INVALID(s)) {
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

    memset((char*)&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if (options & CHAN_OP_SOREUSE) {
        int one = 1;

        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
                   sizeof (int));
    }

    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        (void)close(s);
        lserrno = LSE_SOCK_SYS;
        return (-2);
    }

    if (backlog > 0) {
        if (listen(s, backlog) < 0) {
            (void)close(s);
            lserrno = LSE_SOCK_SYS;
            return (-3);
        }
    }

    channels[ch].handle = s;
    channels[ch].state = CH_WAIT;
    if (type == SOCK_DGRAM)
        channels[ch].type  = CH_TYPE_UDP;
    else
        channels[ch].type  = CH_TYPE_PASSIVE;
    
    chanBindSock(channels[ch].handle,ch);
    return(ch);
}

int
chanClientSocket_(int domain, int type, int options)
{
    int ch, i, s0;
    int s1;
    static char first=TRUE;
    static ushort port;
    struct sockaddr_in cliaddr;

    if (domain != AF_INET) {
        lserrno = LSE_INTERNAL;
        return(-1);
    }

    if ((ch = findAFreeChannel()) < 0) {
        lserrno = LSE_NO_CHAN;
        return(-1);
    }

    if (type == SOCK_STREAM)
        channels[ch].type = CH_TYPE_TCP;
    else
        channels[ch].type = CH_TYPE_UDP;

    s0 = socket(domain, type, 0);

    if (SOCK_INVALID(s0)) {
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

    channels[ch].state = CH_DISC;
    channels[ch].handle = s0;
    if (s0 < 3) {
        s1 = get_nonstd_desc_(s0);
        if (s1 < 0)
            close(s0);
        channels[ch].handle = s1;
    }


    if (options & CHAN_OP_PPORT) {
        if  (first) {
            first = FALSE;
            port = IPPORT_RESERVED - 1;
        }
        if (port < IPPORT_RESERVED/2) {
            port = IPPORT_RESERVED - 1;
        }
    }


    s0= channels[ch].handle;
    memset((char*)&cliaddr, 0, sizeof(cliaddr));
    cliaddr.sin_family      = AF_INET;
    cliaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    for (i = 0; i < IPPORT_RESERVED/2; i++) {

        if (options & CHAN_OP_PPORT) {
            cliaddr.sin_port = htons(port);
            port--;
        } else
            cliaddr.sin_port = htons(0);

        if (bind(s0, (struct sockaddr *)&cliaddr, sizeof(cliaddr)) == 0)
            break;


        if (!(options & CHAN_OP_PPORT)) {

            if (errno == EADDRINUSE) {
                port = (ushort) (time(0) | getpid());
                port = ((port < 1024) ? (port + 1024) : port);
                cliaddr.sin_port = htons(port);
                if (bind(s0, (struct sockaddr *)&cliaddr, sizeof(cliaddr)) == 0)
                    break;
            }

            chanClose_(ch);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }
        if (errno != EADDRINUSE && errno != EADDRNOTAVAIL) {
            chanClose_(ch);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }


        if ((options & CHAN_OP_PPORT) && port < IPPORT_RESERVED/2)
            port = IPPORT_RESERVED - 1;
    }

    if ((options & CHAN_OP_PPORT) && i == IPPORT_RESERVED/2) {
        chanClose_(ch);
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

# if defined(FD_CLOEXEC)
    fcntl(s0, F_SETFD, (fcntl(s0, F_GETFD) | FD_CLOEXEC));
# else
#  if defined(FIOCLEX)
    ioctl(s0, FIOCLEX, (char *)NULL);
#  endif
# endif

    return(ch);

}

int
chanAccept_(int chfd, struct sockaddr_in *from)
{
    int         s;
    socklen_t   len;

    if (channels[chfd].type != CH_TYPE_PASSIVE) {
        lserrno = LSE_INTERNAL;
        return(-1);
    }

    len = sizeof(struct sockaddr);
    s = accept(channels[chfd].handle, (struct sockaddr *) from, &len);
    if (SOCK_INVALID(s)) {
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

    return(chanOpenSock_(s, CHAN_OP_NONBLOCK));

}

void
chanInactivate_(int chfd)
{
    if (chfd < 0 || chfd > chanMaxSize)
        return;

    if (channels[chfd].state != CH_INACTIVE) {
        channels[chfd].prestate = channels[chfd].state;
        channels[chfd].state = CH_INACTIVE;
    }
    chanUpdateListenEvents(chfd);
}

void
chanActivate_(int chfd)
{
    if (chfd < 0 || chfd > chanMaxSize)
        return;

    if (channels[chfd].state == CH_INACTIVE) {
        channels[chfd].state = channels[chfd].prestate;
    }
    chanUpdateListenEvents(chfd);
}

int
chanConnect_(int chfd, struct sockaddr_in *peer, int timeout, int options)
{
    int cc;

    if (channels[chfd].state != CH_DISC) {
        lserrno = LSE_INTERNAL;
        return(-1);
    }

    if (logclass & (LC_COMM | LC_TRACE))
        ls_syslog(LOG_DEBUG2,"chanConnect_: Connecting chan=%d to peer %s timeout %d",chfd, sockAdd2Str_(peer), timeout);

    if (channels[chfd].type == CH_TYPE_UDP) {
        cc = connect(channels[chfd].handle, (struct sockaddr *) peer,
                     sizeof(struct sockaddr_in));
        if (SOCK_CALL_FAIL(cc)) {
            lserrno = LSE_CONN_SYS;
            return(-1);
        }
        channels[chfd].state = CH_CONN;
        return(0);
    }

    if (timeout >= 0) {
        if (b_connect_(channels[chfd].handle, (struct sockaddr *) peer,
                       sizeof(struct sockaddr_in), timeout/1000) < 0) {
            if (errno == ETIMEDOUT)
                lserrno = LSE_TIME_OUT;
            else
                lserrno = LSE_CONN_SYS;
            return(-1);
        }
        channels[chfd].state = CH_CONN;
        return(0);
    }
    channels[chfd].state = CH_CONN;
    return(0);

}

int
chanSendDgram_(int chfd, char *buf, int len, struct sockaddr_in *peer)
{
    int s, cc;

    s = channels[chfd].handle;

    if (logclass & (LC_COMM | LC_TRACE))
        ls_syslog(LOG_DEBUG3,"chanSendDgram_: Sending message size=%d peer=%s chan=%d",len,sockAdd2Str_(peer), chfd);

    if (channels[chfd].type != CH_TYPE_UDP) {
        lserrno = LSE_INTERNAL;
        return(-1);
    }
    if (channels[chfd].state == CH_CONN)
        cc=send(s, buf, len, 0);
    else {
        cc=sendto(s, buf, len, 0, (struct sockaddr *)peer, sizeof(struct sockaddr_in));

    }

    if (SOCK_CALL_FAIL(cc)) {
        lserrno = LSE_MSG_SYS;
        return(-1);
    }

    return(0);
}

/* chanRcvDgram_()
 */
int
chanRcvDgram_(int chfd,
              char *buf,
              int len,
              struct sockaddr_in *peer,
              int timeout)
{
    int sock;
    struct timeval timeval, *timep=NULL;
    int nReady, cc;
    socklen_t   peersize;

    peersize = sizeof(struct sockaddr_in);
    sock = channels[chfd].handle;

    if (channels[chfd].type != CH_TYPE_UDP) {
        lserrno = LSE_INTERNAL;
        return -1;
    }

    if (logclass & (LC_COMM | LC_TRACE))
        ls_syslog(LOG_DEBUG2,"\
chanRcvDgram_: Receive on chan %d timeout=%d",chfd, timeout);

    if (timeout < 0) {
        if (channels[chfd].state == CH_CONN)
            cc = recv(sock, buf, len, 0);
        else
            cc = recvfrom(sock,
                          buf,
                          len,
                          0,
                          (struct sockaddr *)peer,
                          &peersize);
        if (SOCK_CALL_FAIL(cc)) {
            lserrno = LSE_MSG_SYS;
            return -1;
        }
        return 0;
    }

    if (timeout > 0) {
        timeval.tv_sec  = timeout/1000;
        timeval.tv_usec = timeout - timeval.tv_sec*1000;
        timep= &timeval;
    }

    for (;;) {

        nReady = rd_select_(sock, timep);
        if (nReady < 0) {
            lserrno = LSE_SELECT_SYS;
            return -1;
        }
        if (nReady == 0) {
            lserrno = LSE_TIME_OUT;
            return -1;
        }
        if (channels[chfd].state == CH_CONN)
            cc = recv(sock, buf, len, 0);
        else
            cc = recvfrom(sock,
                          buf,
                          len,
                          0,
                          (struct sockaddr *)peer,
                          &peersize);
        if (SOCK_CALL_FAIL(cc)) {
            if (channels[chfd].state == CH_CONN
                && (errno == ECONNREFUSED))
                lserrno = LSE_LIM_DOWN;
            else
                lserrno = LSE_MSG_SYS;
            return -1;
        }

        return 0;

    }

    return 0;

} /* chanRcvDgram_() */

/* chanOpen_()
 */
int
chanOpen_(u_int iaddr, u_short port, int options)
{
    int i;
    int cc;
    int oldOpt;
    int newOpt;
    struct sockaddr_in addr;

    if ((i = findAFreeChannel()) < 0) {
        cherrno = CHANE_NOCHAN;
        return -1;
    }

    channels[i].type = CH_TYPE_TCP;

    memset((char*)&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy((char *) &addr.sin_addr, (char *) &iaddr, sizeof(u_int));
    addr.sin_port = port;

    newOpt = 0;
    if (options & CHAN_OP_PPORT) {
        newOpt |= LS_CSO_PRIVILEGE_PORT;
    }

    oldOpt = setLSFChanSockOpt_(newOpt | LS_CSO_ASYNC_NT);
    channels[i].handle = CreateSock_(SOCK_STREAM);
    setLSFChanSockOpt_(oldOpt);
    if (channels[i].handle < 0) {
        cherrno = CHANE_SYSCALL;
        return -1;
    }

    if (io_nonblock_(channels[i].handle) < 0) {
        CLOSEIT(i);
        cherrno = CHANE_SYSCALL;
        return -1;
    }

    cc = connect(channels[i].handle, (struct sockaddr *) &addr, sizeof(addr));
    if (cc < 0) {

        if (errno != EINPROGRESS) {
            struct sockaddr   laddr;
            socklen_t         len;

            ls_syslog(LOG_ERR,I18N(5002,"\
chanOpen: connect() failed, cc=%d, errno=%d"),/*catgets 5002 */
                      cc, errno);

            len = sizeof(laddr);
            if (getsockname (channels[i].handle, &laddr, &len) == 0) {
                ls_syslog(LOG_ERR,I18N(5003, "\
chanOpen: connect() failed, laddr=%s, addr=%s"),/*catgets 5003*/
                          sockAdd2Str_((struct sockaddr_in *)&laddr),
                          sockAdd2Str_((struct sockaddr_in *)&addr));
            }

            CLOSEIT(i);
            cherrno = CHANE_SYSCALL;
            return (-1);
        }
        channels[i].state = CH_PRECONN;
        chanBindSock(channels[i].handle,i);
        return(i);
    }

    channels[i].state = CH_CONN;
    channels[i].send  = newBuf();
    channels[i].recv  = newBuf();
    chanBindSock(channels[i].handle,i);

    if (!channels[i].send || !channels[i].recv) {
        CLOSEIT(i);
        FREEUP(channels[i].send);
        FREEUP(channels[i].recv);
        lserrno = LSE_MALLOC;
        return(-1);
    }

    return(i);

} /* chanOpen_() */


int
chanOpenSock_(int s, int options)
{
    int i;

    if ((i = findAFreeChannel()) < 0) {
        lserrno = LSE_NO_CHAN;
        return(-1);
    }

    if ((options & CHAN_OP_NONBLOCK) &&
        (io_nonblock_(s) < 0)) {
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }
    channels[i].type = CH_TYPE_TCP;
    channels[i].handle = s;
    channels[i].state = CH_CONN;

    if (options & CHAN_OP_RAW)
        return(i);

    channels[i].send  = newBuf();
    channels[i].recv  = newBuf();
    chanBindSock(channels[i].handle,i);
    if (!channels[i].send || !channels[i].recv) {
        CLOSEIT(i);
        FREEUP(channels[i].send);
        FREEUP(channels[i].recv);
        lserrno = LSE_MALLOC;
        return(-1);
    }
    return(i);
}

int
chanClose_(int chfd)
{
    struct Buffer *buf, *nextbuf;
    long maxfds;


    maxfds = sysconf(_SC_OPEN_MAX);

    if (chfd < 0 || chfd > maxfds) {
        lserrno = LSE_INTERNAL;
        return(-1);
    }

    if(channels[chfd].handle < 0) {
        cherrno = CHANE_BADCHFD;
        return(-1);
    }
    chanUnbindSock(channels[chfd].handle);
    close(channels[chfd].handle);

    if (channels[chfd].send
        && channels[chfd].send != channels[chfd].send->forw) {
        for (buf = channels[chfd].send->forw;
             buf != channels[chfd].send; buf = nextbuf) {
            nextbuf = buf->forw;
            FREEUP(buf->data);
            FREEUP(buf);
        }
    }
    if (channels[chfd].recv
        && channels[chfd].recv != channels[chfd].recv->forw) {
        for (buf = channels[chfd].recv->forw;
             buf != channels[chfd].recv; buf = nextbuf) {
            nextbuf = buf->forw;
            FREEUP(buf->data);
            FREEUP(buf);
        }
    }
    FREEUP(channels[chfd].recv);
    FREEUP(channels[chfd].send);
    channels[chfd].state = CH_FREE;
    channels[chfd].handle = INVALID_HANDLE;
    channels[chfd].send  = (struct Buffer *)NULL;
    channels[chfd].recv  = (struct Buffer *)NULL;
    channels[chfd].listenEvents = 0;
    channels[chfd].readyEvents = 0;
    return(0);
}

void
chanCloseAll_(void)
{
    int i;

    for (i=0; i < chanIndex;  i++)
        if (channels[i].state != CH_FREE)
            chanClose_(i);

}

void
chanCloseAllBut_(int chfd)
{
    int i;

    for (i=0; i < chanIndex;  i++)
        if ((channels[i].state != CH_FREE) && (i != chfd))
            chanClose_(i);
}

int
chanEpoll_(struct epoll_event **ev, struct timeval *timeout)
{
    static char fname[] = "chanEpoll_";
    int i, ready;
    
    int timeout_ms = -1;
    int chfd, sockfd;
    int start, end;

    chanClearReadyEvents(events, nReady);
    if (timeout != NULL) {
        timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    }


    nReady = epoll_wait(epollfd, events, chanMaxSize, timeout_ms);
    if (nReady <= 0) {
        return nReady;
    }

    ready=nReady;
    for (i = 0; i < ready; i++) {
        sockfd = events[i].data.fd;
        chfd = sockChan_(sockfd);
        if (chfd < 0 || chfd >= chanMaxSize) {
            ls_syslog(LOG_ERR, "%s: Invalid channel for sockfd %d", fname, sockfd);
            continue;
        }

        if (channels[chfd].state == CH_INACTIVE || channels[chfd].handle == INVALID_HANDLE) {
            ls_syslog(LOG_ERR, "%s: Invalid state or handle for channel %d", fname, chfd);
            continue;
        }

        if (channels[chfd].state == CH_FREE) {
            ls_syslog(LOG_ERR, "%s: channel %d has socket %d but in CH_FREE state", fname,
                      chfd, channels[chfd].handle);
            continue;
        }


        if (logclass & LC_COMM) {
            ls_syslog(LOG_DEBUG3, "%s: processing channel %d handle %d events 0x%x",
                    fname, chfd, sockfd, events[i].events);
        }
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
            channels[chfd].readyEvents |= EPOLLERR;
            continue;
        }

        if ( channels[chfd].state != CH_PRECONN &&
            !channels[chfd].recv && !channels[chfd].send) {
            channels[chfd].readyEvents = events[i].events;
            continue;
        }

        if (channels[chfd].state == CH_PRECONN) {
            if (events[i].events & EPOLLOUT) {
                channels[chfd].state = CH_CONN;
                channels[chfd].send = newBuf();
                channels[chfd].recv = newBuf();
                if (!channels[chfd].send || !channels[chfd].recv) {
                    lserrno = LSE_MALLOC;
                    channels[chfd].readyEvents |= EPOLLERR;
                    channels[chfd].chanerr = CHANE_MALLOC;
                    continue;
                }
                chanUpdateListenEvents(chfd);
                channels[chfd].readyEvents |= EPOLLOUT;
            }
        } else {
            if (events[i].events & EPOLLIN) {
                doreadEpoll(chfd);
                if (!chanEventsReady(chfd, EPOLLIN)&&!chanEventsReady(chfd, EPOLLERR)) {
                    nReady--;
                }
            }

            if (channels[chfd].send && channels[chfd].send->forw != channels[chfd].send &&
                (events[i].events & EPOLLOUT)) {
                dowriteEpoll(chfd);
                if (!chanEventsReady(chfd, EPOLLERR)) {
                    nReady--;
                }
            }

            channels[chfd].readyEvents |= EPOLLOUT;
        }
    }
    //监听到的ready个就绪事件只有nReady个上报给mbd处理，需要全部放到前面
    start = 0;
    end = ready-1;
    while(end>start){
        while(end>start&&!chanEventsReady(sockChan_(events[end].data.fd), EPOLLIN)&&!chanEventsReady(sockChan_(events[end].data.fd), EPOLLERR))
            end--;
        while(end>start&&(chanEventsReady(sockChan_(events[start].data.fd), EPOLLIN)||chanEventsReady(sockChan_(events[start].data.fd), EPOLLERR)))
            start++;
        events[start++] = events[end--];
    }
    *ev = events;
    return nReady;
}

int
chanSelect_(struct Masks *sockmask,
            struct Masks *chanmask,
            struct timeval *timeout)
{
    int i;
    int nReady;
    int maxfds;

    FD_ZERO(&sockmask->wmask);
    FD_ZERO(&sockmask->emask);

    for(i = 0; i < chanIndex; i++) {
        if (channels[i].state == CH_INACTIVE)
            continue;

        if (channels[i].handle == INVALID_HANDLE)
            continue;
        if (channels[i].state == CH_FREE) {
            ls_syslog(LOG_ERR, "\
%s: channel %d has socket %d but in %s state", __func__,
                      i, channels[i].handle, "CH_FREE");
            continue;
        }

        if (channels[i].type == CH_TYPE_UDP &&
            channels[i].state != CH_WAIT)
            continue;

        if (logclass & LC_COMM)
            ls_syslog(LOG_DEBUG3,"\
%s: Considering channel %d handle %d state %d type %d", __func__,
                      i, channels[i].handle,
                      (int)channels[i].state, (int)channels[i].type);

        if (channels[i].type == CH_TYPE_TCP
            && channels[i].state != CH_PRECONN
            && !channels[i].recv
            && !channels[i].send)
            continue;

        if (channels[i].state == CH_PRECONN) {
            FD_SET(channels[i].handle , &(sockmask->wmask));
            continue;
        }

        if (logclass & LC_COMM)
            ls_syslog(LOG_DEBUG3, "\
%s: Adding channel %d handle %d ", __func__, i,
                      channels[i].handle);
        FD_SET(channels[i].handle, &(sockmask->rmask));

        if (channels[i].type != CH_TYPE_UDP)
            FD_SET(channels[i].handle, &(sockmask->emask));

        if (channels[i].send
            && channels[i].send->forw != channels[i].send)
            FD_SET(channels[i].handle, &(sockmask->wmask));
    }

    maxfds = FD_SETSIZE;

    nReady = select(maxfds,
                    &(sockmask->rmask),
                    &(sockmask->wmask),
                    &(sockmask->emask),
                    timeout);
    if (nReady <= 0) {
        return nReady;
    }

    FD_ZERO(&(chanmask->rmask));
    FD_ZERO(&(chanmask->wmask));
    FD_ZERO(&(chanmask->emask));

    for(i = 0; i < chanIndex; i++) {

        if (channels[i].handle == INVALID_HANDLE)
            continue;

        if (FD_ISSET(channels[i].handle, &(sockmask->emask))) {
            ls_syslog(LOG_DEBUG, "\
%s: setting error mask for channel %d", __func__, channels[i].handle);
            FD_SET(i, &(chanmask->emask));
            continue;
        }

        if ((!channels[i].send || !channels[i].recv)
            && (channels[i].state != CH_PRECONN) ) {
            if (FD_ISSET(channels[i].handle, &(sockmask->rmask)))
                FD_SET(i, &(chanmask->rmask));
            if (FD_ISSET(channels[i].handle, &(sockmask->wmask)))
                FD_SET(i, &(chanmask->wmask));
            continue;
        }


        if (channels[i].state == CH_PRECONN) {

            if (FD_ISSET(channels[i].handle, &(sockmask->wmask))) {
                channels[i].state = CH_CONN;
                channels[i].send  = newBuf();
                channels[i].recv  = newBuf();
                FD_SET(i, &(chanmask->wmask));
            }

        } else {

            if (FD_ISSET(channels[i].handle, &(sockmask->rmask))) {
                doread(i, chanmask);
                if (!FD_ISSET(i, &(chanmask->rmask))
                    && !FD_ISSET(i, &(chanmask->emask)) )
                    nReady--;
            }

            if ((channels[i].send->forw != channels[i].send)
                && FD_ISSET(channels[i].handle, &(sockmask->wmask))) {
                dowrite(i, chanmask);
            }
            FD_SET(i, &(chanmask->wmask));
        }

        FD_CLR(channels[i].handle, &(sockmask->rmask));
        FD_CLR(channels[i].handle, &(sockmask->wmask));
        FD_CLR(channels[i].handle, &(sockmask->emask));
    }

    return nReady;
}

int
chanEnqueue_(int chfd, struct Buffer *msg)
{
    long maxfds;
    int needUpdate = 0;
    if(channels[chfd].send == channels[chfd].send->forw)
        needUpdate = 1;

    maxfds = sysconf(_SC_OPEN_MAX);
    maxfds = sysconf(_SC_OPEN_MAX);

    if (chfd < 0 || chfd > maxfds) {
        cherrno = CHANE_BADCHAN;
        return(-1);
    }

    if (channels[chfd].handle == INVALID_HANDLE ||
        channels[chfd].state == CH_PRECONN) {
        cherrno = CHANE_NOTCONN;
        return(-1);
    }

    enqueueTail_(msg, channels[chfd].send);
    if(needUpdate)
        chanUpdateListenEvents(chfd);
    return(0);
}

int
chanDequeue_(int chfd, struct Buffer **buf)
{
    long maxfds;

    maxfds = sysconf(_SC_OPEN_MAX);

    if (chfd < 0 || chfd > maxfds) {
        cherrno = CHANE_BADCHAN;
        return(-1);
    }
    if (channels[chfd].handle == INVALID_HANDLE ||  channels[chfd].state == CH_PRECONN) {
        cherrno = CHANE_NOTCONN;
        return(-1);
    }

    if (channels[chfd].recv->forw == channels[chfd].recv) {
        cherrno = CHANE_NOMSG;
        return(-1);
    }
    *buf = channels[chfd].recv->forw;
    dequeue_(channels[chfd].recv->forw);
    return(0);
}

int
chanReadNonBlock_(int chfd, char *buf, int len, int timeout)
{
    static char fname[] = "chanReadNonBlock_";

    if (io_nonblock_(channels[chfd].handle) < 0) {
        lserrno = LSE_FILE_SYS;
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "io_nonblock_");
        return (-1);
    }

    return (nb_read_timeout(channels[chfd].handle, buf, len, timeout));
}

int
chanRead_(int chfd, char *buf, int len)
{
    return (b_read_fix(channels[chfd].handle, buf, len));

}

int
chanWrite_(int chfd, char *buf, int len)
{

    return (b_write_fix(channels[chfd].handle, buf, len));

}

int
chanRpc_(int chfd, struct Buffer *in, struct Buffer *out,
         struct LSFHeader *outhdr, int timeout)
{
    static char fname[]="chanRpc_";
    XDR xdrs;
    struct LSFHeader hdrBuf;
    struct timeval timeval, *timep=NULL;
    int cc;

    if (logclass & LC_COMM)
        ls_syslog(LOG_DEBUG1, "%s: Entering ... chfd=%d", fname, chfd);

    if (in) {
        if (logclass & LC_COMM)
            ls_syslog(LOG_DEBUG1,"%s: sending %d bytes", fname, in->len);
        if (chanWrite_(chfd, in->data, in->len ) != in->len)
            return(-1);

        if (in->forw != NULL) {
            struct Buffer *buf=in->forw;
            int nlen = htonl(buf->len);

            if (logclass & LC_COMM)
                ls_syslog(LOG_DEBUG1,"%s: sending %d extra bytes", fname, nlen);
            if (chanWrite_(chfd, NET_INTADDR_(&nlen), NET_INTSIZE_)
                != NET_INTSIZE_)
                return (-1);

            if (chanWrite_(chfd, buf->data, buf->len ) != buf->len)
                return(-1);
        }
    }


    if (!out) {
        return (0);
    }

    if (logclass & LC_COMM)
        ls_syslog(LOG_DEBUG2,"%s: waiting for reply timeout=%d ms", fname,timeout);
    if (timeout > 0) {
        timeval.tv_sec = timeout/1000;
        timeval.tv_usec = timeout - timeval.tv_sec*1000;
        timep = &timeval;
    }

    if ((cc = rd_select_(channels[chfd].handle, timep)) <= 0) {
        if (cc == 0)
            lserrno = LSE_TIME_OUT;
        else
            lserrno = LSE_SELECT_SYS;
        return(-1);
    }

    if (logclass & LC_COMM)
        ls_syslog(LOG_DEBUG2,"%s: reading reply header", fname);
    xdrmem_create(&xdrs, (char *)&hdrBuf, sizeof(struct LSFHeader), XDR_DECODE);
    cc = readDecodeHdr_(chfd, (char *)&hdrBuf, chanRead_,
                        &xdrs, outhdr);
    if (cc < 0) {
        xdr_destroy(&xdrs);
        return(-1);
    }
    xdr_destroy(&xdrs);

#define MAXMSGLEN     (1<<28)
    if (outhdr->length > MAXMSGLEN) {
        lserrno = LSE_PROTOCOL;
        return(-1);
    }

    if (logclass & LC_COMM)
        ls_syslog(LOG_DEBUG2,"%s: reading reply size=%d", fname, outhdr->length);
    out->len = outhdr->length;
    if (out->len > 0) {
        if ((out->data = malloc(out->len)) == NULL) {
            lserrno = LSE_MALLOC;
            return(-1);
        }

        if ((cc=chanRead_(chfd, out->data, out->len)) != out->len) {
            FREEUP(out->data);
            if (logclass & LC_COMM)
                ls_syslog(LOG_DEBUG2,"%s: read only %d bytes", fname,cc);
            lserrno = LSE_MSG_SYS;
            return(-1);
        }
    } else
        out->data = NULL;

    if (logclass & LC_COMM)
        ls_syslog(LOG_DEBUG1, "%s: Leaving...repy_size=%d", fname, out->len);
    return(0);
}

int
chanSock_(int chfd)
{
    if (chfd < 0 || chfd > chanMaxSize) {
        lserrno = LSE_BAD_CHAN;
        return(-1);
    }

    return(channels[chfd].handle);
}

int 
sockChan_(int sockfd)
{
    if(sockfd < 0 || sockfd > chanMaxSize){
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }
    return handles[sockfd].chanIndex;
}

int
chanSetMode_(int chfd, int mode)
{
    if (chfd < 0 || chfd > chanMaxSize) {
        lserrno = LSE_BAD_CHAN;
        return -1;
    }

    if (channels[chfd].state == CH_FREE
        || channels[chfd].handle == INVALID_HANDLE) {
        lserrno = LSE_BAD_CHAN;
        return -1;
    }

    if (mode == CHAN_MODE_NONBLOCK) {

        if (io_nonblock_(channels[chfd].handle) < 0) {
            lserrno = LSE_SOCK_SYS;
            return(-1);
        }
        if (!channels[chfd].send)
            channels[chfd].send  = newBuf();
        if (!channels[chfd].recv)
            channels[chfd].recv  = newBuf();
        if (!channels[chfd].send || !channels[chfd].recv) {
            lserrno = LSE_MALLOC;
            return(-1);
        }
        if(sockChan_(channels[chfd].handle) == -1)
            chanBindSock(channels[chfd].handle,chfd);
        return 0;
    }

    if (io_block_(channels[chfd].handle) < 0) {
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

    return 0;
}

static void
doread(int chfd, struct Masks *chanmask)
{
    struct Buffer *rcvbuf;
    int cc;

    if (channels[chfd].recv->forw == channels[chfd].recv) {
        rcvbuf = newBuf();
        if (!rcvbuf) {
            FD_SET(chfd, &(chanmask->emask));
            channels[chfd].chanerr = LSE_MALLOC;
            return;
        }
        enqueueTail_(rcvbuf, channels[chfd].recv);
    } else
        rcvbuf = channels[chfd].recv->forw;

    if (!rcvbuf->len) {
        rcvbuf->data =  malloc(LSF_HEADER_LEN);
        if (!rcvbuf->data) {
            FD_SET(chfd, &(chanmask->emask));
            channels[chfd].chanerr = LSE_MALLOC;
            return;
        }
        rcvbuf->len = LSF_HEADER_LEN;
        rcvbuf->pos = 0;
    }

    if (rcvbuf->pos == rcvbuf->len) {
        FD_SET(chfd, &(chanmask->rmask));
        return;
    }

    errno = 0;

    cc = read(channels[chfd].handle, rcvbuf->data + rcvbuf->pos,
              rcvbuf->len - rcvbuf->pos);
    if (cc == 0 && errno == EINTR) {
        ls_syslog(LOG_ERR, "\
%s: looks like read() has returned EOF when interrupted by a signal",
                  __func__);
        return;
    }

    if (cc <= 0) {
        if (cc == 0 || BAD_IO_ERR(errno)) {
            FD_SET(chfd, &(chanmask->emask));
            channels[chfd].chanerr = CHANE_CONNRESET;
        }
        return;
    }

    rcvbuf->pos += cc;

    if ((rcvbuf->len == LSF_HEADER_LEN)
        && (rcvbuf->pos == rcvbuf->len )) {
        XDR xdrs;
        struct LSFHeader hdr;
        char *newdata;

        xdrmem_create(&xdrs,
                      rcvbuf->data,
                      sizeof(struct LSFHeader),
                      XDR_DECODE);
        if (!xdr_LSFHeader(&xdrs, &hdr)) {
            FD_SET(chfd, &(chanmask->emask));
            channels[chfd].chanerr = CHANE_BADHDR;
            xdr_destroy(&xdrs);
            return;
        }

        if (hdr.length) {
            rcvbuf->len = hdr.length + LSF_HEADER_LEN;
            newdata = realloc(rcvbuf->data, rcvbuf->len);
            if (!newdata) {
                FD_SET(chfd, &(chanmask->emask));
                channels[chfd].chanerr = LSE_MALLOC;
                xdr_destroy(&xdrs);
                return;
            }
            rcvbuf->data = newdata;
        }
        xdr_destroy(&xdrs);
    }

    if (rcvbuf->pos == rcvbuf->len) {
        FD_SET(chfd, &(chanmask->rmask));
    }

    return;
}


static void
doreadEpoll(int chfd)
{
    static char *fname= "doreadEpoll";
    struct Buffer *rcvbuf;
    int cc;

    if (channels[chfd].recv->forw == channels[chfd].recv) {
        rcvbuf = newBuf();
        if (!rcvbuf) {
            //FD_SET(chfd, &(chanmask->emask));
            channels[chfd].readyEvents |= EPOLLERR;
            channels[chfd].chanerr = LSE_MALLOC;
            return;
        }
        enqueueTail_(rcvbuf, channels[chfd].recv);
    } else
        rcvbuf = channels[chfd].recv->forw;

    if (!rcvbuf->len) {
        rcvbuf->data =  malloc(LSF_HEADER_LEN);
        if (!rcvbuf->data) {
            channels[chfd].readyEvents |= EPOLLERR;
            channels[chfd].chanerr = LSE_MALLOC;
            return;
        }
        rcvbuf->len = LSF_HEADER_LEN;
        rcvbuf->pos = 0;
    }

    if (rcvbuf->pos == rcvbuf->len) {
        channels[chfd].readyEvents |= EPOLLIN;
        return;
    }

    errno = 0;

    cc = read(channels[chfd].handle, rcvbuf->data + rcvbuf->pos,
              rcvbuf->len - rcvbuf->pos);
    if (cc == 0 && errno == EINTR) {
        ls_syslog(LOG_ERR, "\
%s: looks like read() has returned EOF when interrupted by a signal",
                  fname);
        return;
    }

    if (cc <= 0) {
        if (cc == 0 || BAD_IO_ERR(errno)) {
            channels[chfd].readyEvents |= EPOLLERR;
            channels[chfd].chanerr = CHANE_CONNRESET;
        }
        return;
    }

    rcvbuf->pos += cc;

    if ((rcvbuf->len == LSF_HEADER_LEN)
        && (rcvbuf->pos == rcvbuf->len )) {
        XDR xdrs;
        struct LSFHeader hdr;
        char *newdata;

        xdrmem_create(&xdrs,
                      rcvbuf->data,
                      sizeof(struct LSFHeader),
                      XDR_DECODE);
        if (!xdr_LSFHeader(&xdrs, &hdr)) {
            channels[chfd].readyEvents |= EPOLLERR;
            channels[chfd].chanerr = CHANE_BADHDR;
            xdr_destroy(&xdrs);
            return;
        }

        if (hdr.length) {
            rcvbuf->len = hdr.length + LSF_HEADER_LEN;
            newdata = realloc(rcvbuf->data, rcvbuf->len);
            if (!newdata) {
                channels[chfd].readyEvents |= EPOLLERR;
                channels[chfd].chanerr = LSE_MALLOC;
                xdr_destroy(&xdrs);
                return;
            }
            rcvbuf->data = newdata;
        }
        xdr_destroy(&xdrs);
    }

    if (rcvbuf->pos == rcvbuf->len) {
        channels[chfd].readyEvents |= EPOLLIN;
    }

    return;
}


static void
dowrite(int chfd, struct Masks *chanmask)
{
    struct Buffer *sendbuf;
    int cc;

    if (channels[chfd].send->forw == channels[chfd].send)
        return;
    else
        sendbuf = channels[chfd].send->forw;

    cc = write(channels[chfd].handle,
               sendbuf->data + sendbuf->pos,
               sendbuf->len - sendbuf->pos);
    if (cc < 0 && BAD_IO_ERR(errno)){
        FD_SET(chfd, &(chanmask->emask));
        channels[chfd].chanerr = LSE_MSG_SYS;
        return;
    }
    sendbuf->pos += cc;
    if (sendbuf->pos == sendbuf->len) {
        dequeue_(sendbuf);
        free(sendbuf->data);
        free(sendbuf);
    }
    return;
}

static void
dowriteEpoll(int chfd)
{
    struct Buffer *sendbuf;
    int cc;

    if (channels[chfd].send->forw == channels[chfd].send)
        return;
    else
        sendbuf = channels[chfd].send->forw;

    cc = write(channels[chfd].handle,
               sendbuf->data + sendbuf->pos,
               sendbuf->len - sendbuf->pos);
    if (cc < 0 && BAD_IO_ERR(errno)){
        channels[chfd].readyEvents |= EPOLLERR;
        channels[chfd].chanerr = LSE_MSG_SYS;
        return;
    }
    sendbuf->pos += cc;
    if (sendbuf->pos == sendbuf->len) {
        dequeue_(sendbuf);
        free(sendbuf->data);
        free(sendbuf);
    }
    if(channels[chfd].send->forw == channels[chfd].send)
        chanUpdateListenEvents(chfd);
    return;
}

static struct Buffer *
newBuf(void)
{
    struct Buffer *newbuf;

    newbuf = calloc(1, sizeof(struct Buffer));
    if (!newbuf)
        return NULL;
    newbuf->forw = newbuf->back = newbuf;
    newbuf->len  = newbuf->pos = 0;
    newbuf->data = NULL;
    newbuf->stashed = FALSE;

    return newbuf;
}

int
chanAllocBuf_(struct Buffer **buf, int size)
{
    *buf = newBuf();
    if (!*buf)
        return -1;

    (*buf)->data = calloc(size, sizeof(char));
    if ((*buf)->data == NULL) {
        free(*buf);
        return -1;
    }

    return 0;
}

int
chanFreeBuf_(struct Buffer *buf)
{
    if (buf) {
        if (buf->stashed) return 0;

        if (buf->data)
            free(buf->data);

        free(buf);
    }
    return(0);
}

int
chanFreeStashedBuf_(struct Buffer *buf)
{
    if (buf) {
        buf->stashed = FALSE;
        return(chanFreeBuf_(buf));
    }
    return -1 ;
}

static void
dequeue_(struct Buffer *entry)
{
    entry->back->forw = entry->forw;
    entry->forw->back = entry->back;
}

static void
enqueueTail_(struct Buffer *entry, struct Buffer *pred)
{
    entry->back = pred->back;
    entry->forw = pred;
    pred->back->forw = entry;
    pred->back  = entry;
}

static int
findAFreeChannel(void)
{
    int i = 0;

    if (chanIndex != 0) {
        for (i = 0; i < chanIndex; i++)
            if (channels[i].handle == INVALID_HANDLE)
                break;
    }

    if (i == chanIndex)
        chanIndex++;

    if (i == chanMaxSize) {
        chanIndex = chanMaxSize;
        return(-1);
    }

    channels[i].handle = INVALID_HANDLE;
    channels[i].state = CH_FREE;
    channels[i].send  = NULL;
    channels[i].recv = NULL;
    channels[i].chanerr = CHANE_NOERR;

    return i;
}

static void
upDateListenEvents(int chfd){
    static char fname[] = "upDateListenEvents";
    switch (channels[chfd].type) {
        case CH_TYPE_UDP:
            if (channels[chfd].state == CH_WAIT) {
                channels[chfd].listenEvents = EPOLLIN;
            } else {
                return; 
            }
            break;

        case CH_TYPE_TCP:
            if (channels[chfd].state == CH_PRECONN) {
                channels[chfd].listenEvents = EPOLLOUT;
            } 
            else if (channels[chfd].recv || channels[chfd].send) {
                channels[chfd].listenEvents = EPOLLIN | EPOLLERR;
                if (channels[chfd].send && channels[chfd].send->forw != channels[chfd].send) {
                    channels[chfd].listenEvents |= EPOLLOUT;
                }
            } 
            else {
                return; 
            }
            break;

        case CH_TYPE_PASSIVE:
            channels[chfd].listenEvents = EPOLLIN | EPOLLERR;
            break;

        default:
            ls_syslog(LOG_ERR, "%s: Unknown channel type %d for channel %d",
                      fname, channels[chfd].type, chfd);
            return;
    }
}

static void
chanBindSock(int sockfd, int chfd)
{
    if(epollfd == -1) 
        return;
    
    static char fname[] = "chanBindSock";
    struct epoll_event ev;
    ev.data.fd = sockfd;

    handles[sockfd].chanIndex = chfd;
    handles[sockfd].channel = &channels[chfd];

    if (channels[chfd].state == CH_DISC || 
        (channels[chfd].type == CH_TYPE_UDP && channels[chfd].state != CH_WAIT)) {
        return;
    }
    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG3, "%s: channel %d handle %d state %d type %d have buffer %s",
                  fname, chfd, channels[chfd].handle, channels[chfd].state, channels[chfd].type, (channels[chfd].recv||channels[chfd].send) ? "yes" : "no");
    }

    upDateListenEvents(chfd);
    ev.events = channels[chfd].listenEvents;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
        lserrno = LSE_SOCK_SYS;
        ls_syslog(LOG_ERR, "%s: epoll_ctl ADD failed for sockfd %d: %m",
                  fname, sockfd);
    }
}

static void 
chanUpdateListenEvents(int chfd)
{
    if (epollfd == -1) 
        return;
    
    static char fname[] = "chanUpdateListenEvents";
    int needCheck = 1;
    int sockfd = channels[chfd].handle;
    struct epoll_event ev;
    ev.data.fd = sockfd;

    if (channels[chfd].state == CH_DISC ||
        channels[chfd].handle == INVALID_FD ||
        channels[chfd].state == CH_INACTIVE ||
        channels[chfd].state == CH_FREE) 
    {
        if (channels[chfd].state == CH_FREE) {
            ls_syslog(LOG_ERR, "%s: channel %d has socket %d but in CH_FREE state",
                      fname, chfd, sockfd);
        }
        needCheck = 0;
    }

    channels[chfd].listenEvents = 0;

    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG3, "%s: channel %d handle %d state %d type %d have buffer %s",
                  fname, chfd, channels[chfd].handle, channels[chfd].state, channels[chfd].type, (channels[chfd].recv||channels[chfd].send) ? "yes" : "no");
    }
    if(needCheck){
        upDateListenEvents(chfd);

    }

    ev.events = channels[chfd].listenEvents;
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &ev) == -1) {
        lserrno = LSE_SOCK_SYS;
        ls_syslog(LOG_ERR, "%s: epoll_ctl MOD failed for sock %d: %m", fname, sockfd);
    } 
}

static void 
chanUnbindSock(int sockfd)
{
    if(epollfd == -1 || handles[sockfd].chanIndex == -1 || handles[sockfd].channel == NULL) return ;
    static char *fname = "chanUnbindSock";
    int chfd = handles[sockfd].chanIndex;
    handles[sockfd].chanIndex = -1;
    handles[sockfd].channel = NULL;

    if (logclass & LC_COMM) {
        ls_syslog(LOG_DEBUG3, "%s: channel %d handle %d state %d type %d",
                  fname, chfd, channels[chfd].handle, channels[chfd].state, channels[chfd].type);
    }
    if(channels[chfd].state == CH_WAIT){//关闭正在listen的socket，说明是子进程，应该关闭epoll
        close(epollfd);
        epollfd = -1;
        return ;
    }
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL) == -1) {//只有子进程可能DEL失败，应该关闭epoll
        if(epollfd >= 0) {
            close(epollfd);
            epollfd == -1;
            return ;
        }
        lserrno = LSE_SOCK_SYS;
        ls_syslog(LOG_ERR, "%s: epoll_ctl DEL failed for socket %d: %m", fname, sockfd);
    }
}

static void
chanClearReadyEvents(struct epoll_event *ev, int nReady)
{
    static char *fname = "chanClearReadyEvents";
    int i;

    if (nReady <= 0) {
        return ;
    }

    for (i = 0; i < nReady; i++) {
        int sockfd = ev[i].data.fd;
        int chfd = sockChan_(sockfd);

        if (chfd < 0 || chfd >= chanMaxSize) {
            continue;
        }

        channels[chfd].readyEvents = 0;

    }

    return ;
}

void
chanQuitReadyEvents(int chfd, int events)
{   
    static char *fname = "chanQuitReadyEvents";
    if (chfd < 0 || chfd >= chanMaxSize) {
        lserrno = LSE_BAD_CHAN;
        return ;
    }
    channels[chfd].readyEvents &= ~events;
}

int
chanEventsReady(int chfd, int events)
{
    if (chfd < 0 || chfd >= chanMaxSize) {
        lserrno = LSE_BAD_CHAN;
        return -1;
    }

    if (channels[chfd].state == CH_FREE || channels[chfd].handle == INVALID_HANDLE) {
        lserrno = LSE_BAD_CHAN;
        return -1;
    }

    return (channels[chfd].readyEvents & events) ? 1 : 0;
}

int chanEpollStart(){
    int i; 

    epollfd = epoll_create(5);
    handles = calloc(chanMaxSize, sizeof(struct handleData));
    events = calloc(chanMaxSize, sizeof(struct epoll_event));
    nReady = 0;
    if(epollfd == -1 || handles == NULL || events == NULL){
        return -1;
    }
    for(i = 0; i<chanMaxSize; i++){
        handles[i].chanIndex = -1;
        handles[i].channel = NULL;
    }
    return 0;
}

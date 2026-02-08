/* $Id: lib.sock.c 397 2007-11-26 19:04:00Z mblack $
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
#include <unistd.h>
#include <fcntl.h>

extern int totsockets_;
extern int currentsocket_;

static int mLSFChanSockOpt = 0;


int
CreateSock_(int protocol)
{
    static char fname[] = "CreateSock_";
    struct sockaddr_in cliaddr;
    int s;
    static ushort port;
    static ushort i;
    static char isroot = FALSE;

    if (geteuid() == 0 && !getNonPrivilegedPorts())
    {
        if (! isroot) {
            port = IPPORT_RESERVED -1;
        }
        isroot = TRUE;
    } else {
        isroot = FALSE;
        port = 0;
    }

    if (isroot && port < IPPORT_RESERVED/2)
        port = IPPORT_RESERVED -1;

    if ((s = Socket_(AF_INET, protocol, 0)) < 0) {
	if(logclass & LC_COMM)
	   ls_syslog(LOG_DEBUG,"%s: Socket_ failed, %s",fname,strerror(errno));
        lserrno = LSE_SOCK_SYS;
        return (-1);
    }

    memset((char*)&cliaddr, 0, sizeof(cliaddr));
    cliaddr.sin_family      = AF_INET;
    cliaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    for (i=0; i < IPPORT_RESERVED/2; i++) {
        cliaddr.sin_port = htons(port);

        if (isroot) {
            port--;
        }
        if (bind(s, (struct sockaddr *)&cliaddr, sizeof(cliaddr)) == 0)
            break;


        if (!isroot) {
            closesocket(s);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }

        if (errno != EADDRINUSE && errno != EADDRNOTAVAIL) {
	    if(logclass & LC_COMM)
	       ls_syslog(LOG_DEBUG,"%s: bind failed, %s",fname,strerror(errno));
            closesocket(s);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }


        if (isroot && port < IPPORT_RESERVED/2)
            port = IPPORT_RESERVED - 1;
    }


    if (isroot && i == IPPORT_RESERVED/2) {
	if(logclass & LC_COMM)
	    ls_syslog(LOG_DEBUG,"%s: went through all , %s",fname,strerror(errno));
        closesocket(s);
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

# if defined(FD_CLOEXEC)
    fcntl(s, F_SETFD, (fcntl(s, F_GETFD) | FD_CLOEXEC));
# else
#  if defined(FIOCLEX)
    (void)ioctl(s, FIOCLEX, (char *)NULL);
#  endif
# endif

    return (s);

}

int
CreateSockEauth_(int protocol)
{
    static char fname[] = "CreateSock_";
    struct sockaddr_in cliaddr;
    int s;
    static ushort port;
    static ushort i;
    static char isroot = FALSE;


    if ((geteuid() == 0) && (genParams_[LSF_AUTH].paramValue == NULL) && !getNonPrivilegedPorts())
    {
        if (! isroot) {
            port = IPPORT_RESERVED -1;
        }
        isroot = TRUE;
    } else {
        isroot = FALSE;
        port = 0;
    }

    if (isroot && port < IPPORT_RESERVED/2)
        port = IPPORT_RESERVED -1;

    if ((s = Socket_(AF_INET, protocol, 0)) < 0) {
	if(logclass & LC_COMM)
	   ls_syslog(LOG_DEBUG,"%s: Socket_ failed, %s",fname,strerror(errno));
        lserrno = LSE_SOCK_SYS;
        return (-1);
    }

    memset((char*)&cliaddr, 0, sizeof(cliaddr));
    cliaddr.sin_family      = AF_INET;
    cliaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    for (i=0; i < IPPORT_RESERVED/2; i++) {
        cliaddr.sin_port = htons(port);

        if (isroot) {
            port--;
        }
        if (bind(s, (struct sockaddr *)&cliaddr, sizeof(cliaddr)) == 0)
            break;


        if (!isroot) {
            closesocket(s);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }

        if (errno != EADDRINUSE && errno != EADDRNOTAVAIL) {
	    if(logclass & LC_COMM)
	       ls_syslog(LOG_DEBUG,"%s: bind failed, %s",fname,strerror(errno));
            closesocket(s);
            lserrno = LSE_SOCK_SYS;
            return (-1);
        }


        if (isroot && port < IPPORT_RESERVED/2)
            port = IPPORT_RESERVED - 1;
    }


    if (isroot && i == IPPORT_RESERVED/2) {
	if(logclass & LC_COMM)
	    ls_syslog(LOG_DEBUG,"%s: went through all , %s",fname,strerror(errno));
        closesocket(s);
        lserrno = LSE_SOCK_SYS;
        return(-1);
    }

# if defined(FD_CLOEXEC)
    fcntl(s, F_SETFD, (fcntl(s, F_GETFD) | FD_CLOEXEC));
# else
#  if defined(FIOCLEX)
    (void)ioctl(s, FIOCLEX, (char *)NULL);
#  endif
# endif

    return (s);

}


int
get_nonstd_desc_(int desc)
{
    int s0 = -1;
    int s1 = -1;
    int s2 = -1;

    while (desc <= 2) {
        switch (desc) {
        case 0:
            s0 = desc;
            break;
        case 1:
            s1 = desc;
            break;
        case 2:
            s2 = desc;
            break;
        default:
            return (-1);
        }

        desc = dup(desc);
    }

    if (s0 >= 0)
        close(s0);
    if (s1 >= 0)
        close(s1);
    if (s2 >= 0)
        close(s2);

    return (desc);

}



int
TcpCreate_(int service, int port)
{
    register int s;
    struct sockaddr_in sin;

    if ((s = Socket_(AF_INET, SOCK_STREAM, 0)) < 0) {
        lserrno = LSE_SOCK_SYS;
        return (-1);
    }

    if (service) {
	memset((char*)&sin, 0, sizeof(sin));
        sin.sin_family      = AF_INET;
        sin.sin_port        = htons(port);
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            (void)close(s);
            lserrno = LSE_SOCK_SYS;
            return (-2);
        }
        if (listen(s, 1024) < 0) {
            (void)close(s);
            lserrno = LSE_SOCK_SYS;
            return (-3);
        }
    }

    return (s);

}

int
io_nonblock_(int s)
{
    return (fcntl(s, F_SETFL, O_NONBLOCK));
}

int
io_block_(int s)
{
    return (fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK));
}


int
setLSFChanSockOpt_(int newOpt)
{
    int oldOpt = mLSFChanSockOpt;

    mLSFChanSockOpt = newOpt;
    return oldOpt;
}


int
Socket_(int domain, int type, int protocol)
{
    int s0, s1;

    if ((s0 = socket(domain, type, protocol)) < 0)
        return (-1);

    if (s0 < 0)
        return (-1);

    if (s0 >=3)
        return (s0);

    s1 = get_nonstd_desc_(s0);
    if (s1 < 0)
        close(s0);
    return (s1);
}

/* svrsockCreate_()
 */
ls_svrsock_t *
svrsockCreate_(u_short port,
               int backlog,
               struct sockaddr_in *addr,
               int options)
{
    ls_svrsock_t         *svrsock;
    struct sockaddr_in   *svrAddr;
    int                  acceptSock;
    socklen_t            length;

    if ((svrsock = malloc(sizeof(ls_svrsock_t))) == NULL) {
        lserrno = LSE_MALLOC;
	return (NULL);
    }

    svrAddr = malloc(sizeof(struct sockaddr_in));
    if (svrAddr == NULL) {
        lserrno = LSE_MALLOC;
        free(svrsock);
        return (NULL);
    }
    svrsock->localAddr = svrAddr;

    if (addr != NULL) {
        port = ntohs(addr->sin_port);
        (*svrAddr) = (*addr);
    } else {
        memset((char *) svrAddr, 0, sizeof(struct sockaddr_in));
        svrAddr->sin_family = AF_INET;
        svrAddr->sin_port = htons(port);
        svrAddr->sin_addr.s_addr = INADDR_ANY;
    }

    if ((acceptSock = socket(svrAddr->sin_family, SOCK_STREAM, 0)) < 0) {
        lserrno = LSE_SOCK_SYS;
        free(svrsock->localAddr);
        free(svrsock);
        return (NULL);
    }

    if (bind(acceptSock,
             (struct sockaddr *)svrAddr,
             sizeof(struct sockaddr_in)) < 0) {
        (void) close(acceptSock);
        lserrno = LSE_SOCK_SYS;
        free(svrsock->localAddr);
        free(svrsock);
        return (NULL);
    }
    if (listen(acceptSock, 5) < 0) {
        (void) closesocket(acceptSock);
        lserrno = LSE_SOCK_SYS;
        free(svrsock->localAddr);
        free(svrsock);
        return (NULL);
    }

    if (port == 0) {
        length = sizeof(struct sockaddr_in);
        if (getsockname(acceptSock,
                        (struct sockaddr *)svrAddr,
                        &length) < 0) {
            lserrno = LSE_SOCK_SYS;
            (void) closesocket(acceptSock);
            free(svrsock->localAddr);
            free(svrsock);
            return (NULL);
        }
        svrsock->port = ntohs(svrAddr->sin_port);
    }
    else
       svrsock->port = port;

    svrsock->sockfd = acceptSock;
    svrsock->options = options;
    if (backlog == 0)
        svrsock->backlog = 50;
    else
        svrsock->backlog = backlog;

    return svrsock;
}

/* svrsockAccept_()
 */
int
svrsockAccept_(ls_svrsock_t *svrsock, int timeout)
{
    socklen_t            len;
    int                  s;
    struct sockaddr_in   from;

    if (svrsock == NULL) {
        lserrno = LSE_BAD_ARGS;
        return (-1);
    }

    s = -1;
    len = sizeof(from);
    s = accept(svrsock->sockfd, (struct sockaddr *) &from, &len);
    if (s < 0) {
        lserrno = LSE_ACCEPT_SYS;
        return (-1);
    }

    return s;
}

char *
svrsockToString_(ls_svrsock_t *svrsock)
{
    char   *string;
    char   *hostname;

    if (svrsock == NULL) {
        lserrno = LSE_BAD_ARGS;
        return (NULL);
    }

    hostname = ls_getmyhostname();

    if ((string = malloc(strlen(hostname)+7)) == NULL) {
        lserrno = LSE_MALLOC;
        return (NULL);
    }

    sprintf(string, "%s:%u", hostname, svrsock->port);

    return string;
}

void
svrsockDestroy_(ls_svrsock_t *svrsock)
{
    (void) close(svrsock->sockfd);
    free(svrsock->localAddr);
    free(svrsock);
}

int
TcpConnect_(char *hostname, u_short port, struct timeval *timeout)
{
    int sock;
    int nwRdy, i;
    struct sockaddr_in server;
    struct hostent *hp;
    fd_set wm;

    server.sin_family = AF_INET;
    if ((hp = Gethostbyname_(hostname)) == NULL) {
        lserrno = LSE_BAD_HOST;
        return (-1);
    }

    memcpy((char *) &server.sin_addr,
           (char *) hp->h_addr,
           (int) hp->h_length);

    server.sin_port = htons(port);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0))<0) {
	lserrno = LSE_SOCK_SYS;
        return (-1);
    }
    if (io_nonblock_(sock) < 0) {
	lserrno = LSE_MISC_SYS;
        closesocket(sock);
        return (-1);
    }

    if (connect(sock, (struct sockaddr *) &server, sizeof(server)) < 0
        && errno != EINPROGRESS) {
	lserrno = LSE_CONN_SYS;
        closesocket(sock);
        return (-1);
    }

    for (i = 0; i < 2; i++) {
        FD_ZERO(&wm);
        FD_SET(sock, &wm);
        nwRdy = select(sock+1, NULL, &wm, NULL, timeout);

        if (nwRdy < 0) {
            if (errno == EINTR)
                continue;
            lserrno = LSE_SELECT_SYS;
            closesocket(sock);
            return (-1);
        } else if (nwRdy == 0) {
            lserrno = LSE_TIME_OUT;
            closesocket(sock);
            return (-1);
        }
        break;
    }

    return sock;
}


char *
getMsgBuffer_(int fd, int *bufferSize)
{
    int rc;
    char hdrbuf[sizeof(struct LSFHeader)];
    struct LSFHeader msgHdr;
    XDR xdrs;
    char *msgBuffer;
    *bufferSize = -1;

    xdrmem_create(&xdrs, hdrbuf, sizeof(struct LSFHeader), XDR_DECODE);
    rc = readDecodeHdr_(fd, hdrbuf, b_read_fix, &xdrs, &msgHdr);
    if (rc < 0) {
	lserrno = LSE_MSG_SYS;
	xdr_destroy(&xdrs);
	return (NULL);
    }
    xdr_destroy(&xdrs);
    *bufferSize = msgHdr.length;
    if (msgHdr.length) {
	if ((msgBuffer = malloc(msgHdr.length)) == NULL ) {
	    lserrno = LSE_MALLOC;
	    return (NULL);
        }
    } else {
	lserrno = LSE_NO_ERR;
	return (NULL);
    }
    if (b_read_fix(fd, msgBuffer, msgHdr.length) != msgHdr.length) {
	lserrno = LSE_MSG_SYS;
	free(msgBuffer);
	return (NULL);
    }
    return msgBuffer;
}

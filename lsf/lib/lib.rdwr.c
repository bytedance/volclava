/* $Id: lib.rdwr.c 397 2007-11-26 19:04:00Z mblack $
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
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include "lib.h"
#include "lproto.h"

#define IO_TIMEOUT	2000

#define US_DIFF(t1, t2)	(((t1).tv_sec - (t2).tv_sec) * 1000000 + (t1).tv_usec - (t2).tv_usec)

static void alarmer_(void);

int
nb_write_fix(int s, char *buf, int len)
{
    int cc;
    int length;
    struct timeval start, now;

    struct timezone junk;

    gettimeofday(&start, &junk);

    for (length = len; len > 0 ; ) {
        if ((cc = write(s, buf, len)) > 0) {
            len -= cc;
            buf += cc;
        } else if (cc < 0 && BAD_IO_ERR(errno)) {
	    if (errno == EPIPE) 
	        lserrno = LSE_LOSTCON;

            return (-1);
        }
	if (len > 0)	
	{
            gettimeofday(&now, &junk);
	    if (US_DIFF(now, start) > IO_TIMEOUT * 1000) {
		errno = ETIMEDOUT;
		return(-1);
	    }
	    millisleep_(IO_TIMEOUT / 20);
	}
    }
    return (length);
} 

int
nb_read_fix(int s, char *buf, int len)
{
    int cc;
    int length;
    struct timeval start, now;
    struct timezone junk;

    if (logclass & LC_TRACE)
	ls_syslog(LOG_DEBUG, "nb_read_fix: Entering this routine...");

    gettimeofday(&start, &junk);

    for (length = len ; len > 0 ; ) {
        if ((cc = read(s, buf, len)) > 0) {
            len -= cc;
            buf += cc;
        } else if (cc == 0 || BAD_IO_ERR(errno)) {
	    if (cc == 0) 
		errno = ECONNRESET;
            return (-1);
        }

	if (len > 0)	
	{
            gettimeofday(&now, &junk);
	    if (US_DIFF(now, start) > IO_TIMEOUT * 1000) {
		errno = ETIMEDOUT;
		return(-1);
	    }
	    millisleep_(IO_TIMEOUT / 20);
	}
    }

    return(length);
} 

#define MAXLOOP	3000

int
b_read_fix(int s, char *buf, int len)
{
    int cc;
    int loop;
    int length;
    int numLoop;

    if (len > MAXLOOP * 1024) {
        numLoop = MAXLOOP * 100;
    } else {
        numLoop = MAXLOOP;
    }

    for (length = len, loop = 0; len > 0 && loop < numLoop; loop++) {

        if ((cc = read(s, buf, len)) > 0) {
            len -= cc;
            buf += cc;
        } else if (cc == 0 || errno != EINTR) {
	    if (cc == 0) 
		errno = ECONNRESET;	    
            return (-1);
        }
    }

    if (len > 0) {
        return(-1);
    }

    return(length);
} 

int
b_write_fix(int s, char *buf, int len)
{

    int cc;
    int loop;
    int length;
    for (length = len, loop = 0; len > 0 && loop < MAXLOOP; loop++) {
        if ((cc = write(s, buf, len)) > 0) {
            len -= cc;
            buf += cc;
        } else if (cc < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
	    lserrno = LSE_SOCK_SYS;
            return (-1);
        }
    }

    if (len > 0) {
	lserrno = LSE_SOCK_SYS;
        return (-1);
    }

    return (length);
} 

void unblocksig(int sig)
{
   sigset_t blockMask,oldMask;
   sigemptyset(&blockMask);
   sigaddset(&blockMask,sig);
   sigprocmask(SIG_UNBLOCK, &blockMask, &oldMask);
}

int 
b_connect_(int s, struct sockaddr *name, int namelen, int timeout)
{
    struct itimerval old_itimer;
    unsigned int oldTimer;
    sigset_t newMask, oldMask;
    struct sigaction action, old_action;

    
    if (getitimer(ITIMER_REAL, &old_itimer) <0) 
	return -1;

    
    action.sa_flags = 0;
    action.sa_handler = (SIGFUNCTYPE) alarmer_;
    
    
    sigfillset(&action.sa_mask);
    sigaction(SIGALRM, &action, &old_action);
    
    unblocksig(SIGALRM);
    
    blockSigs_(SIGALRM, &newMask, &oldMask);

    oldTimer = alarm(timeout);

    if (connect(s, name, namelen) < 0) {
        if (errno == EINTR) 
	    errno = ETIMEDOUT;
	
	alarm(oldTimer);
        setitimer(ITIMER_REAL, &old_itimer, NULL);
	
	sigaction(SIGALRM, &old_action, NULL);
        sigprocmask(SIG_SETMASK, &oldMask, NULL);
	return -1;
    }
    
    alarm(oldTimer);

    setitimer(ITIMER_REAL, &old_itimer, NULL);
    
    sigaction(SIGALRM, &old_action, NULL);
    sigprocmask(SIG_SETMASK, &oldMask, NULL);
    return 0;
}  

/* 
 * Replace select with poll.
 * Reason for replacement:
 *  A core dump occurs when excessively large values are set for host MXJ and MAX_SBD_CONNS.
 *  Root cause analysis shows that the number of sockets exceeds 1024, which makes select unusable (select has a 1024 socket limit).
 * 
 * Comparison between epoll and select:
 *  1. When using epoll in this scenario, it is found that the spin lock consumes a significant amount of time,
 *     and an additional socket is required to monitor events. Poll can reduce resource overhead in this case.
 *  2. Consistent with epoll, poll has no limit on the number of sockets (breaking through select's 1024 socket limit).
 *  3. Poll achieves higher efficiency than both epoll and select in the single socket listening scenario.
 */
int rd_poll_(int rd, struct timeval *timeout)
{
    int cc;
    struct pollfd fds[1];
    int pollTimeout;

    fds[0].fd = rd;
    fds[0].events = POLLIN; 

    if (timeout == NULL) {
        pollTimeout = -1;
    } else {
        pollTimeout = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
        if (pollTimeout < 0) {
            pollTimeout = 0;
        }
    }

    for (;;) {
        fds[0].revents = 0;
        cc = poll(fds, 1, pollTimeout);

        if (cc >= 0) {
            return cc;
        }

        if (errno == EINTR) {
            continue;
        }
        return (-1);
    }
}

/* b_accept_()
 */
int
b_accept_(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    sigset_t   oldMask;
    sigset_t   newMask;
    int        cc;
    
    blockSigs_(0, &newMask, &oldMask);

    cc = accept(s, addr, addrlen);
    sigprocmask(SIG_SETMASK, &oldMask, NULL);

    return (cc);

}  /* b_accept_() */

int
detectTimeout_(int s, int recv_timeout)
{
    struct timeval timeval;
    struct timeval *timep = NULL;
    int ready;

    if (recv_timeout) {
        timeval.tv_sec = recv_timeout;
        timeval.tv_usec = 0;
        timep = &timeval;
    }
    ready = rd_poll_(s, timep);
    if (ready < 0) {
        lserrno = LSE_SELECT_SYS;
        return (-1);
    } else if (ready == 0) {      
        lserrno = LSE_TIME_OUT;
        return(-1);
    }
    return(0);
} 

static void
alarmer_(void)
{
} 


int
blockSigs_(int sig, sigset_t *blockMask, sigset_t *oldMask)
{
    sigfillset(blockMask);

    if (sig)
	sigdelset(blockMask, sig);
    
    sigdelset(blockMask, SIGHUP);
    sigdelset(blockMask, SIGINT);
    sigdelset(blockMask, SIGQUIT);
    sigdelset(blockMask, SIGILL);
    sigdelset(blockMask, SIGTRAP);
    sigdelset(blockMask, SIGFPE);
    sigdelset(blockMask, SIGBUS);
    sigdelset(blockMask, SIGSEGV);
    sigdelset(blockMask, SIGPIPE);
    sigdelset(blockMask, SIGTERM);
    
    return (sigprocmask(SIG_BLOCK, blockMask, oldMask));

} 

int
nb_read_timeout(int s, char *buf, int len, int timeout)
{
    int cc;
    int nReady;
    int length = len;
    struct timeval timeval;

    timeval.tv_sec  = timeout;
    timeval.tv_usec = 0; 
    
    for (;;) {
        nReady = rd_poll_(s, &timeval);
        if (nReady < 0) {
            lserrno = LSE_SELECT_SYS;
            return(-1);
        } else if (nReady == 0) {
            
            lserrno = LSE_TIME_OUT;
            return(-1);
        } else {
            if ((cc = recv(s, buf, len, 0)) > 0) {
                len -= cc;
                buf += cc;
            } else if (cc == 0 || BAD_IO_ERR(errno)) {
                if (cc == 0) 
                    errno = ECONNRESET;
                return (-1);
            }
	    if (len == 0 )  
		break;
	}
    }

    return (length);

} 


int nb_write_timeout(int s, char* buf, int len, int timeout) {
    if (buf == NULL || len <= 0 || timeout < 0) {
        lserrno = LSE_SOCK_SYS;
        return -1;
    }
    static struct timespec sleepTime = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
    int totalSent = 0;
    struct timeval start;
    struct timeval now;
    long long elapsedUs, timeoutUs;
    timeoutUs = timeout * 1000000;

    gettimeofday(&start, NULL);
    /* Tests have shown that directly looping write operations is more efficient 
     * than using poll to check if the socket is writable each time before writing. 
     * Therefore, we directly use loop writing
     */
    while (totalSent < len) {
        ssize_t cc = write(s, buf + totalSent, len - totalSent);

        if (cc > 0) {
            totalSent += cc;
            continue;
        } else if (cc == -1) {
            if (errno == EINTR) {
                continue;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lserrno = LSE_SOCK_SYS;
                return -1;
            }
        } else {
            lserrno = LSE_MSG_SYS;
            errno = ECONNRESET;
            return -1;
        }

        gettimeofday(&now, NULL);
        elapsedUs = US_DIFF(start, now);
        if (elapsedUs >= timeoutUs) {
            lserrno = LSE_TIME_OUT;
            return -1;
        }
        nanosleep(&sleepTime, NULL);
    }
    if (totalSent < len) {
        lserrno = LSE_SOCK_SYS;
        return -1;
    }

    return totalSent;
}
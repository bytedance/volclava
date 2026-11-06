/* $Id: lib.sig.c 397 2007-11-26 19:04:00Z mblack $
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

#ifndef SIGEMT
#define SIGEMT SIGBUS
#endif
#ifndef SIGLOST
#define SIGLOST SIGIO
#endif
#ifndef SIGIOT
#define SIGIOT SIGABRT
#endif

#if !defined(SIGWINCH) && defined(SIGWINDOW)
#    define SIGWINCH SIGWINDOW
#endif

int sig_map[] = {0,
                 SIGHUP,
                 SIGINT,
                 SIGQUIT,
                 SIGILL,
                 SIGTRAP,
                 SIGIOT,
                 SIGEMT,
                 SIGFPE,
                 SIGKILL,
                 SIGBUS,
                 SIGSEGV,
                 SIGSYS,
                 SIGPIPE,
                 SIGALRM,
                 SIGTERM,
                 SIGSTOP,
                 SIGTSTP,
                 SIGCONT,
                 SIGCHLD,
                 SIGTTIN,
                 SIGTTOU,
                 SIGIO,
                 SIGXCPU,
                 SIGXFSZ,
                 SIGVTALRM,
                 SIGPROF,
                 SIGWINCH,
                 SIGLOST,
                 SIGUSR1,
                 SIGUSR2
};

char *sigSymbol[] = {           "",
                                "HUP",
                                "INT",
                                "QUIT",
                                "ILL",
                                "TRAP",
                                "IOT",
                                "EMT",
                                "FPE",
                                "KILL",
                                "BUS",
                                "SEGV",
                                "SYS",
                                "PIPE",
                                "ALRM",
                                "TERM",
                                "STOP",
                                "TSTP",
                                "CONT",
                                "CHLD",
                                "TTIN",
                                "TTOU",
                                "IO",
                                "XCPU",
                                "XFSZ",
                                "VTALRM",
                                "PROF",
                                "WINCH",
                                "LOST",
                                "USR1",
                                "USR2"
};

int NSIG_MAP = (sizeof(sig_map)/sizeof(int));

int
sig_encode(int sig)
{
    int i;

    if (sig < 0)
        return sig;

    for (i=0; i<NSIG_MAP; i++)
        if (sig_map[i] == sig)
            break;
    if (i == NSIG_MAP) {
        if (sig >= NSIG_MAP)
            return(sig);
        else
            return(0);
    } else
        return(i);
}

int
sig_decode(int sig)
{
    if (sig < 0)
        return sig;

    if (sig >= NSIG_MAP) {
        if (sig < NSIG)
            return(sig);
        else {
            return(0);
        }
    }

    return(sig_map[sig]);
}

int
getSigVal(char *sigString)
{
    int sigVal, i;
    char sigSig[16];

    if (sigString == NULL)
        return -1;
    if (sigString[0] == '\0')
        return -1;

    if (isint_(sigString) == TRUE) {
        if ((sigVal=atoi(sigString)) > NSIG)
            return -1;
        else
            return (sigVal);
    }

    for (i=0; i<NSIG_MAP; i++) {
        sprintf(sigSig, "%s%s", "SIG", sigSymbol[i]);
        if ((strcmp(sigSymbol[i], sigString) == 0)
            || (strcmp( sigSig, sigString) == 0))
            return (sig_map[i]);
    }
    return -1;

}

char *
getSigSymbolList (void)
{
    static char list[512];
    int i;

    list[0] = '\0';
    for (i=1; i<NSIG_MAP; i++) {
        strcat(list, sigSymbol[i]);
        strcat(list, " ");
    }
    return(list);

}

SIGFUNCTYPE Signal_(int sig, void (*handler)(int))
{
    struct sigaction act, oact;

    act.sa_handler = handler;
    act.sa_flags = SA_RESTART;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, sig);
    if(sigaction(sig, &act, &oact) == -1){
        oact.sa_handler = (void (*)())SIG_ERR;
    }
    return(oact.sa_handler);
}

char *
getSigSymbol (int sig)
{
    static char symbol[30];

    symbol[0] = '\0';
    if (sig < 0 || sig >= NSIG_MAP)
        strcpy(symbol, "UNKNOWN");
    else
        strcpy(symbol, sigSymbol[sig]);
    return (symbol);

}

int
blockALL_SIGS_(sigset_t *newMask, sigset_t *oldMask)
{
    sigfillset(newMask);
    sigdelset(newMask, SIGTRAP);
    sigdelset(newMask, SIGEMT);
    return (sigprocmask(SIG_BLOCK, newMask, oldMask));
}

/* $Id: lim.cluster.c 397 2007-11-26 19:04:00Z mblack $
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
#include "lim.h"
#include "../../lsf/lib/lsi18n.h"

#define NL_SETN         24

#define ABORT     1
#define RX_HINFO  2
#define RX_LINFO  3

#define HINFO_TIMEOUT   120
#define LINFO_TIMEOUT   60

struct clientNode  *clientMap[MAXCLIENTS];

extern int chanIndex;

static void processMsg(int);
static void clientReq(XDR *, struct LSFHeader *, int );

static void shutDownChan(int);

void
clientIO(struct epoll_event *ev,int nReady)
{
    int  i;

    for(i=0; (i < nReady) && (i < MAXCLIENTS); i++){
        int sockfd = ev[i].data.fd;
        int chfd = sockChan_(sockfd);
        if (chfd == limSock || chfd == limTcpSock)
            continue;

        if (chanEventsReady(chfd, EPOLLERR)) {

            if (clientMap[chfd])
                ls_syslog(LOG_ERR, "\
%s: Lost connection with client %s IO or decode error",
                          __func__,
                          sockAdd2Str_(&clientMap[chfd]->from));
            shutDownChan(chfd);
            continue;
        }

        if (chanEventsReady(chfd, EPOLLIN)) {
            processMsg(chfd);
        }
    }
}

/* processMsg()
 */
static void
processMsg(int chanfd)
{
    struct Buffer *buf;
    struct LSFHeader hdr;
    XDR xdrs;
    struct sockaddr_in from;

    if (clientMap[chanfd] && clientMap[chanfd]->inprogress)
        return;

    if (chanDequeue_(chanfd, &buf) < 0) {
        ls_syslog(LOG_ERR, "\
%s: failed to dequeue from channel %d %M", __func__, chanfd);
        shutDownChan(chanfd);
        return;
    }

    xdrmem_create(&xdrs,
                  buf->data,
                  XDR_DECODE_SIZE_(buf->len),
                  XDR_DECODE);

    if (!xdr_LSFHeader(&xdrs, &hdr)) {
        ls_syslog(LOG_ERR, "\
%s: Bad header received chanfd %d from %s",
                  __func__, chanfd, sockAdd2Str_(&from));
        xdr_destroy(&xdrs);
        shutDownChan(chanfd);
        chanFreeBuf_(buf);
        return;
    }

    if ((clientMap[chanfd] && hdr.opCode >= FIRST_LIM_PRIV)
        || (!clientMap[chanfd] && hdr.opCode < FIRST_INTER_CLUS)) {
        ls_syslog(LOG_ERR, "\
%s: Invalid opCode %d from client %s",
                  __func__, hdr.opCode, sockAdd2Str_(&from));
        xdr_destroy(&xdrs);
        shutDownChan(chanfd);
        chanFreeBuf_(buf);
        return;
    }

    if (hdr.opCode >= FIRST_INTER_CLUS && !masterMe) {
        ls_syslog(LOG_ERR, "\
%s: Intercluster request received from %s, but I'm not master",
                  __func__, sockAdd2Str_(&from));
        xdr_destroy(&xdrs);
        shutDownChan(chanfd);
        chanFreeBuf_(buf);
        return;
    }

    if (!clientMap[chanfd]) {

        if (hdr.opCode != LIM_CLUST_INFO) {
            socklen_t L = sizeof(struct sockaddr_in);

            if (getpeername(chanSock_(chanfd),
                            (struct sockaddr *)&from,
                            &L) < 0) {
                ls_syslog(LOG_ERR, "\
%s: getpeername() on socket %d failed %M",
                          __func__, chanSock_(chanfd));
            }

            ls_syslog(LOG_ERR, "\
%s: Protocol error received opCode %d from %s",
                      __func__, hdr.opCode, sockAdd2Str_(&from));
            xdr_destroy(&xdrs);
            shutDownChan(chanfd);
            chanFreeBuf_(buf);
            return;
        }
    }

    ls_syslog(LOG_DEBUG, "\
%s: Received request %d from %s chan %d len %d",
              __func__, hdr.opCode, sockAdd2Str_(&from),
              chanfd, buf->len);

    switch(hdr.opCode) {

        case LIM_LOAD_REQ:
        case LIM_GET_HOSTINFO:
        case LIM_PLACEMENT:
        case LIM_GET_RESOUINFO:
        case LIM_GET_INFO:
            clientMap[chanfd]->limReqCode = hdr.opCode;
            clientMap[chanfd]->reqbuf = buf;
            clientReq(&xdrs, &hdr, chanfd);
            break;
        case LIM_LOAD_ADJ:
            loadadjReq(&xdrs, &clientMap[chanfd]->from, &hdr, chanfd);
            xdr_destroy(&xdrs);
            shutDownChan(chanfd);
            chanFreeBuf_(buf);
            break;
        case LIM_PING:
            xdr_destroy(&xdrs);
            shutDownChan(chanfd);
            chanFreeBuf_(buf);
            break;
        case LIM_ADD_HOST:
            addMigrantHost(&xdrs, &clientMap[chanfd]->from, &hdr, chanfd);
            xdr_destroy(&xdrs);
            shutDownChan(chanfd);
            chanFreeBuf_(buf);
            break;
        case LIM_RM_HOST:
            rmMigrantHost(&xdrs, &clientMap[chanfd]->from, &hdr, chanfd);
            xdr_destroy(&xdrs);
            shutDownChan(chanfd);
            chanFreeBuf_(buf);
            break;
        default:
            ls_syslog(LOG_ERR, "\
%s: Invalid opCode %d from %s", __func__, hdr.opCode,
                      sockAdd2Str_(&from));
            xdr_destroy(&xdrs);
            chanFreeBuf_(buf);
            break;
    }
}


static void
clientReq(XDR *xdrs, struct LSFHeader *hdr, int chfd)
{
    struct decisionReq decisionRequest;
    int oldpos, i;

    clientMap[chfd]->clientMasks = 0;

    oldpos = XDR_GETPOS(xdrs);


    if (! xdr_decisionReq(xdrs, &decisionRequest, hdr)) {
        goto Reply1;
    }

    clientMap[chfd]->clientMasks = 0;
    for (i = 1; i < decisionRequest.numPrefs; i++) {
        if (!findHostInCluster(decisionRequest.preferredHosts[i])) {
            clientMap[chfd]->clientMasks = 0;
            break;
        }
    }

    for(i = 0; i < decisionRequest.numPrefs; i++)
        free(decisionRequest.preferredHosts[i]);
    free(decisionRequest.preferredHosts);

Reply1:

    {
        pid_t pid = 0;

        if (! limParams[LIM_NO_FORK].paramValue) {
            pid = fork();
            if (pid < 0)  {
                ls_syslog(LOG_ERR, "\
%s: ohmygosh fork() failed %m", __func__);
                return;
            }
        }

        if (pid == 0) {

            if (! limParams[LIM_NO_FORK].paramValue)
                chanClose_(limSock);

            XDR_SETPOS(xdrs, oldpos);
            io_block_(chanSock_(chfd));

            switch(hdr->opCode) {
                case LIM_GET_HOSTINFO:
                    hostInfoReq(xdrs,
                                clientMap[chfd]->fromHost,
                                &clientMap[chfd]->from,
                                hdr,
                                chfd);
                    break;
                case LIM_GET_RESOUINFO:
                    resourceInfoReq(xdrs, &clientMap[chfd]->from, hdr, chfd);
                    break;
                case LIM_LOAD_REQ:
                    loadReq(xdrs, &clientMap[chfd]->from, hdr, chfd);
                    break;
                case LIM_PLACEMENT:
                    placeReq(xdrs, &clientMap[chfd]->from, hdr, chfd);
                    break;
                case LIM_GET_INFO:
                    infoReq(xdrs, &clientMap[chfd]->from, hdr, chfd);
                default:
                    break;
            }

            if (! limParams[LIM_NO_FORK].paramValue)
                exit(0);
        }
        /* parent pid > 0 or LIM_NO_FORK
         *
         * Remember that in LSF the parent
         * shuts down the connection with
         * the client while the child is
         * serving the request. This is not
         * strictly necessary as we could let
         * the child do its work and then wait
         * for the client to exit.
         */
        xdr_destroy(xdrs);
        shutDownChan(chfd);
    }
}

static void
shutDownChan(int chanfd)
{
    chanClose_(chanfd);
    if (clientMap[chanfd]) {
        chanFreeBuf_(clientMap[chanfd]->reqbuf);
        FREEUP(clientMap[chanfd]);
    }
}


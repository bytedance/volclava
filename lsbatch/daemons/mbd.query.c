/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mbd.h"
#include "daemonout.h"
#include <sys/prctl.h>
#define DEF_EPOLL_INTERVAL 1
#define DEF_QMBD_FORCE_EXIT_DELAY 100 
 
int listenChfd;
int qmbdThreadNum = DEF_QMBD_THREAD_NUM;
int qmbdMaxTaskNum = DEF_QMBD_MAX_TASK_NUM;
threadPool_t  *lightQueryPool, *heavyQueryPool;
extern short qmbd_port;
extern int qmbdListenSock;

static int exitStatus = 0;                             /*Flag:1.query mbd should exit after processing remaining requests；2.query mbd should force exit*/
static int initQueryDaemon();                                                   
static void handleClientIO();      
static void processQueryRequestByThread(struct clientNode* client);               
static void acceptConnection(int socket);           
static void* processRequest(void* arg); 
static void shutdownClient(struct clientNode *client);
static void handleDaemonExpiration();
static void alarmHandler(int sig);
static void exitQmbd();
static int initListenSocket(void);
static void* controlPipeMonitorThread(void* arg);

extern int authRequest(struct lsfAuth *, XDR *, struct LSFHeader *,
                       struct sockaddr_in *, struct sockaddr_in *,
                       char *, int);
extern int processClient(struct clientNode* client, int *needFree);               

/*
 * Start the qmbd daemon process and handle client requests
 * @param[out] qmbdPid: Pointer to store the PID of the qmbd process
 * @return: 0 on successful startup of qmbd, -1 on fork failure
 */
int startQueryDaemon(int *qmbdPid){
    int cc = 0;
    int nready = 0;
    int *readyChans = NULL;
    struct timeval timeout;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"%s: Entering...", __func__);

    *qmbdPid = fork();
    if ((*qmbdPid) < 0) {
        ls_syslog(LOG_DEBUG, I18N_FUNC_FAIL_M, __func__, "fork");
        return -1;
    }
    if ((*qmbdPid) > 0){
        ls_syslog(LOG_DEBUG, "%s: query mbd pid is %d", __func__,*qmbdPid);
        close(qmbdPipe[0]);
        qmbdPipe[0] = -1;
        if(syncNewJobs)
            registerReaderToShm(*qmbdPid, time(0));
        return 0;
    }
    close(qmbdPipe[1]);
    qmbdPipe[1] = -1;
    *qmbdPid = getpid();
    cc = initQueryDaemon();
    if(cc < 0){
        ls_syslog(LOG_WARNING, "%s: qmbd init failed", __func__);
        exit(-1);
    }
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    for (;;) {
        /* After query mbd expires, if the set time is exceeded, force it to exit */
        if(exitStatus == 2){
            break;
        }
               
        /* Exit after processing remaining requests */
        if(exitStatus && listenChfd == -1 && clientList->forw == clientList){
            break;
        }


        nready = chanEpoll_(&readyChans, &timeout);
        if (nready < 0) {
            if (errno != EINTR)
                ls_syslog(LOG_ERR, "\
qmbd: Ohmygosh.. qmbd epoll() failed %m");
            continue;
        }
        if(nready == 0){
            timeout.tv_sec = DEF_EPOLL_INTERVAL;
            timeout.tv_usec = 0;
            continue;
        }
        timeout.tv_sec  = 0;
        timeout.tv_usec = 0;

        if (listenChfd != -1 && chanEventsReady(listenChfd, EPOLL_EVENT_READ)) {
            acceptConnection(listenChfd);
        }

        handleClientIO();

    } /* for (;;) */
    exitQmbd();
}

/*
 * Handles query requests from a client
 * Parses request data, identifies request type, and dispatches tasks to thread pool or creates threads
 * @param[in] client: Pointer to the client node needing query request processing
 */
static void processQueryRequestByThread(struct clientNode *client) {
    static char             fname[] = "processQueryRequestByThread()";
    struct Buffer           *buf = NULL;
    mbdReqType              mbdReqtype = 0;
    int                     s = 0;
    int                     ret = 0;
    unsigned int            len = 0;
    struct sockaddr_in      from;
    struct LSFHeader        reqHdr;
    XDR*                    xdrs = NULL;
    struct requestContext*  reqContext = NULL;
    client->state = CLIENT_STATE_PROCESSING;
    s = client->chanfd;
    from = client->from;
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "%s: Entering...",__func__);

    if (chanDequeue_(s, &buf) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_ENO_D, fname, "chanDequeue_", cherrno);
        client->state = CLIENT_STATE_FINISHED;
        return;
    }

    xdrs = (XDR*)malloc(sizeof(XDR));
    if (!xdrs) {
        ls_syslog(LOG_ERR, "%s: Memory allocation failed for XDR", fname);
        chanFreeBuf_(buf);
        client->state = CLIENT_STATE_FINISHED;
        return;
    }
    xdrmem_create(xdrs, buf->data, buf->len, XDR_DECODE);
    
    if (!xdr_LSFHeader(xdrs, &reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
        xdr_destroy(xdrs);
        free(xdrs);
        chanFreeBuf_(buf);
        client->state = CLIENT_STATE_FINISHED;
        return;
    }
    
    len = reqHdr.length;
    mbdReqtype = reqHdr.opCode;

    if (logclass & (LC_COMM | LC_TRACE)) {
        ls_syslog(LOG_DEBUG, 
                 "%s: Received request <%d> from host <%s/%s> on channel <%d>",
                 fname, mbdReqtype, client->fromHost, sockAdd2Str_(&from), s);
    }
    reqContext = malloc(sizeof(struct requestContext));
    if (!reqContext) {
        errorBack(s, LSBE_NO_MEM, &from);
        xdr_destroy(xdrs);
        FREEUP(xdrs);
        chanFreeBuf_(buf);
        client->state = CLIENT_STATE_FINISHED;
        client->lastTime = time(0);
        client->reqType = mbdReqtype;
        return ;
    }
    reqContext->xdr = xdrs;
    reqContext->buf = buf;
    reqContext->reqHdr = reqHdr;
    reqContext->client = client;
    reqContext->schedule = 0;

    /*
     When the bjobs command is executed, the client first sends a BATCH_JOB_INFO request to read a header (job data is not read temporarily), 
     then sends a BATCH_QUE_INFO request. Only after receiving all information of the BATCH_QUE_INFO request 
     does the client start reading the job data of the BATCH_JOB_INFO request. 
     If all requests are put into the same thread pool, a critical issue arises: in the event of sudden concurrent full-volume queries via the bjobs command, 
     all threads in the pool will be occupied with processing BATCH_JOB_INFO requests, leaving no threads available to handle BATCH_QUE_INFO requests. 
     This causes the client to fail to read the BATCH_JOB_INFO data, resulting in all threads blocking on write operations, 
     which leads to a deadlock and inability to process other requests. 
     Therefore, we allocate a dedicated thread pool to handle BATCH_JOB_INFO requests to prevent BATCH_QUE_INFO requests from being unprocessed.
    */
    if(reqHdr.opCode == BATCH_JOB_INFO){
        ret = addTaskToThreadPool(heavyQueryPool, processRequest, reqContext);
    }else{
        ret = addTaskToThreadPool(lightQueryPool, processRequest, reqContext);
    }
    if(ret < 0){
        ls_syslog(LOG_ERR, "%s: addTaskToThreadPool failed : queue task is full",__func__);
        xdr_destroy(xdrs);
        FREEUP(xdrs);
        chanFreeBuf_(buf);
        FREEUP(reqContext);
        client->state = CLIENT_STATE_FINISHED;
        client->lastTime = time(0);
    }
    return;
}

/*Used to monitor the query mbd control pipe; when the pipe is closed, the query mbd is deemed expired.*/
static void* controlPipeMonitorThread(void* arg){
    struct pollfd pfd;
    int ret = 0;
    
    if(qmbdPipe[0] < 0){
        return NULL;
    }
    
    pfd.fd = qmbdPipe[0];
    pfd.events = POLLIN;
    /* After reading data from the pipe, close the listen socket, close the read end of the pipe, and exit the thread */
    while(1){
        /* Wait indefinitely for the main mbd to close the control pipe. */
        ret = poll(&pfd, 1, -1);
        if(ret < 0){
            if(errno == EINTR){
                continue;
            }
            ls_syslog(LOG_ERR, "%s: poll failed: %m", __func__);
            handleDaemonExpiration();
            break;
        }
        handleDaemonExpiration();
        break;
    }
    return NULL;
}

/*
 * Handle qmbd daemon expiration when alive timer fires
 * Closes the listen socket and control pipe, then sets a force-exit timer
 * to allow pending requests to complete before forced shutdown
 */
static void handleDaemonExpiration(){
    struct itimerval timer;
    exitStatus = 1;
    /* Close the listen socket and stop accepting new connections */
    if (listenChfd != -1) {
        chanClose_(listenChfd);
        listenChfd = -1;
    }
    if(qmbdPipe[0] >= 0){
        close(qmbdPipe[0]);
        qmbdPipe[0] = -1;
    }
    
    timer.it_value.tv_sec = DEF_QMBD_FORCE_EXIT_DELAY;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, NULL);
}

/*
 * SIGALRM handler for forced qmbd exit
 * Sets exitStatus to 2 to signal that the graceful shutdown period has expired
 * and the daemon must exit immediately
 */
static void
alarmHandler(int sig){
    int saveErrno = 0;
    sigset_t newmask;
    sigset_t oldmask;
    saveErrno = errno;
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGALRM);
    sigaddset(&newmask, SIGTERM);
    sigaddset(&newmask, SIGINT);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);
    /* Set forced exit flag */
    exitStatus = 2;
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    errno = saveErrno;
}

/*
 * Executes client requests in threads
 * Calls corresponding request handlers based on opcode, releases resources, and updates client state
 * @param[in] arg: Pointer to RequestContext storing request details
 */
static void* processRequest(void* arg) {
    int                  ret = 0;
    socklen_t            laddrLen = 0;
    struct sockaddr_in   laddr;
    struct lsfAuth       auth;
    struct requestContext* reqContext = (struct requestContext*)arg;
    if(reqContext->reqHdr.opCode == BATCH_JOB_INFO){
        ret = do_jobInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr,reqContext->schedule);
    }else{
        switch (reqContext->reqHdr.opCode) {
            case BATCH_QUE_INFO:
                ret = do_queueInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr);
                break;

            case BATCH_HOST_INFO:
                ret = do_hostInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr);
                break;
            
            case BATCH_GRP_INFO:
                ret = do_groupInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr);
                break;
            
            case BATCH_PARAM_INFO:
                ret = do_paramInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr);
                break;

            case BATCH_USER_INFO:
                ret = do_userInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr);
                break;

            case BATCH_RESOURCE_INFO:
                ret = do_resourceInfoReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, &reqContext->reqHdr);
                break;
            
            case BATCH_JOB_PEEK:
                if (getsockname(chanSock_(reqContext->client->chanfd),
                        (struct sockaddr *) &laddr,
                        &laddrLen) == -1) {
                    ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, __func__, "getsockname");
                    errorBack(reqContext->client->chanfd, LSBE_PROTOCOL, &reqContext->client->from);
                    ret = -1;
                    break;
                }

                if ((ret = authRequest(&auth, reqContext->xdr, &reqContext->reqHdr, &reqContext->client->from, &laddr,
                                    reqContext->client->fromHost, chanSock_(reqContext->client->chanfd))) !=
                    LSBE_NO_ERROR) {
                    errorBack(reqContext->client->chanfd, ret, &reqContext->client->from);
                    ret = -1;
                    break;
                }
                ret = do_jobPeekReq(reqContext->xdr, reqContext->client->chanfd, &reqContext->client->from, reqContext->client->fromHost, &reqContext->reqHdr, &auth);
                break;
                
            default:
                errorBack(reqContext->client->chanfd, LSBE_PROTOCOL, &reqContext->client->from);
                ls_syslog(LOG_ERR, "%s: Unknown request type %d from host %s",
                        __func__, reqContext->reqHdr.opCode, sockAdd2Str_(&reqContext->client->from));
                xdr_destroy(reqContext->xdr);
                FREEUP(reqContext->xdr);
                chanFreeBuf_(reqContext->buf);
                reqContext->client->state = CLIENT_STATE_FINISHED;
                reqContext->client->reqType = reqContext->reqHdr.opCode;
                reqContext->client->lastTime = time(0);
                FREEUP(reqContext);
                return (void *)(-1);
        }
    }
    
    xdr_destroy(reqContext->xdr);
    FREEUP(reqContext->xdr);
    chanFreeBuf_(reqContext->buf);
    reqContext->client->state = CLIENT_STATE_FINISHED;
    reqContext->client->reqType = reqContext->reqHdr.opCode;
    reqContext->client->lastTime = time(0);
    FREEUP(reqContext);
    if(ret == -1){
        return (void *)(-1);
    }
    return NULL;
}

/*
 * Processes IO events for all clients in the list
 * Handles channel errors, triggers request processing on read events, and closes invalid clients
 */
static void 
handleClientIO(){
    struct clientNode *cliPtr;
    struct clientNode *nextClient;
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"%s: Entering...", __func__);

    for (cliPtr = clientList->forw;
         cliPtr != clientList;
         cliPtr = nextClient) {
        nextClient = cliPtr->forw;
        if(cliPtr->state == CLIENT_STATE_FINISHED || (chanEventsReady(cliPtr->chanfd, EPOLL_EVENT_ERROR) && cliPtr->state != CLIENT_STATE_PROCESSING)){
            shutdownClient(cliPtr);
            continue;
        }

        if(cliPtr->state == CLIENT_STATE_CONNECTED){
            if (chanEventsReady(cliPtr->chanfd, EPOLL_EVENT_READ)) {
                if (logclass & (LC_TRACE | LC_COMM))
                    ls_syslog(LOG_DEBUG,"Task append chfd is %d socket is %d,from %s",cliPtr->chanfd,chanSock_(cliPtr->chanfd),sockAdd2Str_(&cliPtr->from));
                processQueryRequestByThread(cliPtr);
            }
        }
    }
}


/*
 * Initialize qmbd daemon resources
 * Sets up timers, signal handlers, thread pool, epoll, and listening socket
 * @return: 0 on successful initialization, -1 on failure to create listening socket
 */
static
int initQueryDaemon(){
    int i = 0;
    struct clientNode *cliPtr = NULL;
    struct clientNode *nextClient = NULL;
    sigset_t empty_set;
    pthread_t tid;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_ERR,"%s: Entering...", __func__);

    chanCloseEpoll();
    isQmbd = 1;
    exitStatus = 0;

    if (daemonParams[LSB_QMBD_THREAD_NUM].paramValue != NULL) {
        qmbdThreadNum = getValidatedNumericParam(__func__,
                                                 "LSB_QMBD_THREAD_NUM",
                                                 daemonParams[LSB_QMBD_THREAD_NUM].paramValue,
                                                 MIN_QMBD_THREAD_NUM,
                                                 MAX_QMBD_THREAD_NUM,
                                                 DEF_QMBD_THREAD_NUM);
    }

    if (daemonParams[LSB_QMBD_MAX_TASK_NUM].paramValue != NULL) {
        qmbdMaxTaskNum = getValidatedNumericParam(__func__,
                                                  "LSB_QMBD_MAX_TASK_NUM",
                                                  daemonParams[LSB_QMBD_MAX_TASK_NUM].paramValue,
                                                  MIN_QMBD_MAX_TASK_NUM,
                                                  MAX_QMBD_MAX_TASK_NUM,
                                                  DEF_QMBD_MAX_TASK_NUM);
    }

    sigemptyset(&empty_set);
    sigprocmask(SIG_SETMASK, &empty_set, NULL);
    for(cliPtr=clientList->forw; cliPtr != clientList; cliPtr=nextClient) {
        nextClient = cliPtr->forw;
        shutdownClient(cliPtr);
    }
    
    for(i = 3; i < sysconf(_SC_OPEN_MAX); i++){
        if(i != qmbdPipe[0] && i != qmbdListenSock)
            close(i);
    }
    Signal_(SIGCHLD, (SIGFUNCTYPE) child_handler);
    Signal_(SIGTERM, (SIGFUNCTYPE) terminate_handler);
    Signal_(SIGALRM, (SIGFUNCTYPE) alarmHandler);
    Signal_(SIGHUP,  SIG_IGN);
    Signal_(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGTERM);


    if(jDataList[SJL]->back != jDataList[SJL])
        reorderSJL ();
    lightQueryPool = createThreadPool(qmbdThreadNum, qmbdMaxTaskNum);
    heavyQueryPool = createThreadPool(qmbdThreadNum, qmbdMaxTaskNum);
    if(lightQueryPool == NULL || heavyQueryPool == NULL){
        ls_syslog(LOG_ERR, "%s: createThreadPool failed: %m");
        return -1;
    }
    listenChfd = initListenSocket();
    if (listenChfd < 0) {
        ls_syslog(LOG_ERR, "%s: Cannot get query batch server socket... %M", __func__);
        return -1;
    }else{
        ls_syslog(LOG_INFO, "%s: query batch server start , channel is %d, sockfd is %d,port is %d", __func__,listenChfd,chanSock_(listenChfd),ntohs(qmbd_port));
    }
    if(qmbdPipe[0] >= 0){
        int rc = pthread_create(&tid, NULL, controlPipeMonitorThread, NULL);
        if (rc != 0) {
            ls_syslog(LOG_ERR, "%s: pthread_create for pipe monitor failed: %s",
                      __func__, strerror(rc));
            close(qmbdPipe[0]);
            qmbdPipe[0] = -1;
            return -1;
        }else{
            pthread_detach(tid);
        }
    }else{
        ls_syslog(LOG_ERR, "%s: query mbd pipe err");
        return -1;
    }
    return 0;
}

/*
 * Accept and initialize a new client connection
 * Creates a client node, stores connection details, and adds it to the client list
 * @param[in] socket: Channel descriptor of the listening socket
 */
static void
acceptConnection(int socket)
{
    int s = 0;
    struct sockaddr_in from;
    struct hostent *hp = NULL;
    struct clientNode *client = NULL;

    s = chanAccept_(socket, (struct sockaddr_in *)&from);
    /*
     * If a connection arrival is detected, and at the moment before accept() is called,
     * the thread that the query mbd uses to monitor the pipe timeout sent by the 
     * main mbd and closes the listenfd, then accept() will return -1 with errno set to EBADF
     */
    if (s == -1) {
        if(errno != EBADF && errno != EINVAL)
            ls_syslog(LOG_ERR, "%s Ohmygosh accept() failed... %m", __func__);
        return;
    }
    if(chanRegisterEpoll_(s, EPOLLIN|EPOLLERR) < 0){
        ls_syslog(LOG_ERR, "%s: chanRegisterEpoll_() failed %m",__func__);
        return;
    }

    hp = Gethostbyaddr_(&from.sin_addr.s_addr,
                        sizeof(in_addr_t),
                        AF_INET);
    if (hp == NULL) {
        ls_syslog(LOG_WARNING, "\
%s: gethostbyaddr() failed for %s", __func__,
                  sockAdd2Str_(&from));
        errorBack(s, LSBE_PERMISSION, &from);
        chanClose_(s);
        return;
    }

    ls_syslog(LOG_DEBUG, "\
%s: Received request from host %s %s on socket %d",
              __func__, hp->h_name, sockAdd2Str_(&from),
              chanSock_(s));

    memcpy(&from.sin_addr, hp->h_addr, hp->h_length);

    client = my_calloc(1, sizeof(struct clientNode), __func__);
    client->chanfd = s;
    client->from =  from;
    client->fromHost = safeSave(hp->h_name);
    client->reqType = 0;
    client->lastTime = 0;
    client->state = CLIENT_STATE_CONNECTED;

    inList((struct listEntry *)clientList,
           (struct listEntry *) client);

    ls_syslog(LOG_DEBUG, "\
%s: Accepted connection from host %s on channel %d",
              __func__, client->fromHost, client->chanfd);
}

/*
 * Close client connection and free resources
 * @param[in] client: Pointer to the client node to shutdown
 */
static void
shutdownClient(struct clientNode *client)
{
    chanClose_(client->chanfd);
    offList((struct listEntry *)client);
    if (client->fromHost)
        free(client->fromHost);
    free(client);
}

/*
 * Cleanup and exit the query mbd daemon
 * Closes listen socket, destroys thread pools, and exits with appropriate status
 */
static void exitQmbd(){
    if (listenChfd != -1) {
        chanClose_(listenChfd);
        listenChfd = -1;
    }
    if(lightQueryPool)
        destroyThreadPool(lightQueryPool);
    if(heavyQueryPool)
        destroyThreadPool(heavyQueryPool);

    if(exitStatus == 1){
        ls_syslog(LOG_DEBUG, "%s: query mbd exit normally",__func__);
        exit(0);
    }
    ls_syslog(LOG_WARNING, "%s query mbd exit timeout",__func__);
    exit(-1);
}

/*
 * Initialize listening socket for query mbd daemon
 * Adopts the listening socket inherited from the main mbd and registers it with epoll
 * @return: Channel descriptor on success, -1 on failure
 */
int
initListenSocket(void)
{
    int ch = 0;
    
    if (chanEpollInit() < 0) {
        ls_syslog(LOG_ERR, "%s: chanEpollInit_() failed %m", __func__);
        return -1;
    }
    if (qmbdListenSock < 0) {
        ls_syslog(LOG_ERR, "%s: qmbd listen socket is not initialized", __func__);
        return -1;
    }
    ch = chanOpenPassiveSock_(qmbdListenSock, 0);
    if (ch < 0) {
        ls_syslog(LOG_ERR, "%s: chanOpenPassiveSock_() failed to adopt socket %m",
                  __func__);
        return -1;
    }
    if(chanRegisterEpoll_(ch, EPOLLIN|EPOLLERR) < 0){
        ls_syslog(LOG_ERR, "%s: chanRegisterEpoll_() failed: %m",
                  __func__);
        return -1;
    }

    return ch;
}

#include "mbd.h"
#define DEF_EPOLL_INTERVAL 1
#define DEF_QMBD_ALIVE_TIME 10
#define DEF_THREAD_NUM 8
#define DEF_THREADPOLL_MAX_TASK 10000
int qmbdAliveTime = DEF_QMBD_ALIVE_TIME;
int qmbdThreadNum = DEF_THREAD_NUM;
int qmbdMaxTaskNum = DEF_THREADPOLL_MAX_TASK;
extern threadPool_t*  pool;
static int qmbdDie = 0;                             /*Flag to indicate qmbd should exit after processing remaining requests*/

static int qmbdInit();                              
static void closeClient();                          
static void clientIO();      
static void processClientWithQueryReq(struct clientNode* client);   
static void qmbdDieAlarmHandler (int sig);          
static void acceptConnection(int socket);       
static void* processRequest(void* arg); 

/*
 * Start the qmbd daemon process and handle client requests
 * @param[out] qmbdPid: Pointer to store the PID of the qmbd process
 * @return: 0 on successful startup of qmbd, -1 on fork failure
 */
int startqmbd(int *qmbdPid){
    static char* fname="startqmbd";
    int i, cc;
    int nready = 0;
    int **readyChans;
    struct timeval timeout;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"%s: Entering...",fname);

    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    if(syncNewJobs){
        memset(shm, 0, sizeof(struct sharedJobStore));
        shm->writeIdx = 0;
    }
    if(jDataList[SJL]->back != jDataList[SJL])
        reorderSJL ();
    if ((*qmbdPid = fork()) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_M, fname, "fork");
        return -1;
    }

    if ((*qmbdPid) > 0){
        ls_syslog(LOG_DEBUG, "%s: qmbd pid is %d", __func__,*qmbdPid);
        return 0;
    }
    *qmbdPid = getpid();
    cc = qmbdInit();
    
    for (;;) {
        if(querySock != -1 && qmbdDie){
            close(chanSock_(querySock));
            querySock = -1;
        }
        closeClient();
        if(qmbdDie && querySock == -1 && clientList->forw == clientList){
            break;
        }


        int maxfd;
        maxfd = sysconf(_SC_OPEN_MAX);
        nready = chanEpoll_(&readyChans, &timeout);
        if (nready < 0) {
            if (errno != EINTR)
                ls_syslog(LOG_ERR, "\
%s: Ohmygosh.. qmbd epoll() failed %m", __func__);
            continue;
        }
        if(nready == 0){
            timeout.tv_sec = DEF_EPOLL_INTERVAL;
            timeout.tv_usec = 0;
            continue;
        }
        timeout.tv_sec  = 0;
        timeout.tv_usec = 0;

        if (querySock != -1 && chanEventsReady(querySock, EPOLL_EVENTS_READ)) {
            acceptConnection(querySock);
        }

        clientIO();

    } /* for (;;) */
    destroyThreadPool(pool);
    shmdt(shm);
    ls_syslog(LOG_DEBUG,"%s: qmbd finished",fname);
    exit(0);
}

/*
 * Close clients in CLIENT_STATE_PROCESS_FINISHED state or not in CLIENT_STATE_THREAD_PROCESSING state and channel event is EPOLL_EVENTS_ERROR
 * Delays closing to avoid thread safety issues with concurrent operations
 */
static 
void closeClient(){
    struct clientNode *cliPtr;
    struct clientNode *nextClient; 
    for (cliPtr = clientList->forw;
         cliPtr != clientList;
         cliPtr = nextClient) {
        nextClient = cliPtr->forw;
        if((chanEventsReady(cliPtr->chanfd, EPOLL_EVENTS_ERROR)&& !cliPtr->state == CLIENT_STATE_THREAD_PROCESSING) || cliPtr->state == CLIENT_STATE_PROCESS_FINISHED){
            shutDownClient(cliPtr);
        }
    }
}

/*
 * Handles query requests from a client
 * Parses request data, identifies request type, and dispatches tasks to thread pool or creates threads
 * @param[in] client: Pointer to the client node needing query request processing
 */
static void processClientWithQueryReq(struct clientNode *client) {
    static char          fname[] = "processClientWithQueryReq()";
    struct Buffer        *buf;
    mbdReqType           mbdReqtype;
    int                  s;
    unsigned int         len;
    struct sockaddr_in   from;
    struct LSFHeader     reqHdr;
    XDR*                 xdrs = NULL;
    RequestContext*      req = NULL;
    
    s = client->chanfd;
    from = client->from;
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG, "processClientWithQueryReq: Entering...");

    if (chanDequeue_(s, &buf) < 0) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL_ENO_D, fname, "chanDequeue_", cherrno);
        client->state = CLIENT_STATE_PROCESS_FINISHED;
        return;
    }

    xdrs = (XDR*)malloc(sizeof(XDR));
    if (!xdrs) {
        ls_syslog(LOG_ERR, "%s: Memory allocation failed for XDR", fname);
        chanFreeBuf_(buf);
        client->state = CLIENT_STATE_PROCESS_FINISHED;
        return;
    }
    xdrmem_create(xdrs, buf->data, buf->len, XDR_DECODE);
    
    if (!xdr_LSFHeader(xdrs, &reqHdr)) {
        ls_syslog(LOG_ERR, I18N_FUNC_FAIL, fname, "xdr_LSFHeader");
        xdr_destroy(xdrs);
        free(xdrs);
        chanFreeBuf_(buf);
        client->state = CLIENT_STATE_PROCESS_FINISHED;
        return;
    }
    
    len = reqHdr.length;
    mbdReqtype = reqHdr.opCode;

    if (logclass & (LC_COMM | LC_TRACE)) {
        ls_syslog(LOG_DEBUG, 
                 "%s: Received request <%d> from host <%s/%s> on channel <%d>",
                 fname, mbdReqtype, client->fromHost, sockAdd2Str_(&from), s);
    }

    client->state = CLIENT_STATE_WAITING_THREAD;
    switch (mbdReqtype) {
        case BATCH_QUE_INFO:
            {
                req = malloc(sizeof(RequestContext));
                if (!req) {
                    errorBack(s, LSBE_NO_MEM, &from);
                    xdr_destroy(xdrs);
                    free(xdrs);
                    chanFreeBuf_(buf);
                    break;
                }
                req->xdr = xdrs;
                req->buf = buf;
                req->reqHdr = reqHdr;
                req->client = client;
                req->schedule = 0;
                req->byQmbd = 0;
                addTaskToThreadPool(pool, processRequest, req);
            }
            break;

        case BATCH_JOB_INFO:
            {
                req = malloc(sizeof(RequestContext));
                if (!req) {
                    errorBack(s, LSBE_NO_MEM, &from);
                    xdr_destroy(xdrs);
                    free(xdrs);
                    chanFreeBuf_(buf);
                    break;
                }
                req->xdr = xdrs;
                req->buf = buf;
                req->reqHdr = reqHdr;
                req->client = client;
                req->schedule = 0;
                req->byQmbd = 1;
                createAndRunThread(processRequest, req);
                //addTaskToThreadPool(pool, processRequest, req);
            }
            break;
            
        default:
            errorBack(s, LSBE_PROTOCOL, &from);
            ls_syslog(LOG_ERR, "%s: Unsupported request type %d from host %s",
                      fname, mbdReqtype, sockAdd2Str_(&from));
            xdr_destroy(xdrs);
            free(xdrs);
            chanFreeBuf_(buf);
            client->state = CLIENT_STATE_PROCESS_FINISHED;
            client->lastTime = now;
            client->reqType =mbdReqtype;
            break;
    }
    return;
}

/*
 * Executes client requests in threads
 * Calls corresponding request handlers based on opcode, releases resources, and updates client state
 * @param[in] arg: Pointer to RequestContext storing request details
 */
static void* processRequest(void* arg) {
    int ret;
    RequestContext* req = (RequestContext*)arg;
    req->client->state = CLIENT_STATE_THREAD_PROCESSING;
    switch (req->reqHdr.opCode) {
        case BATCH_QUE_INFO:
            ret = do_queueInfoReq(req->xdr, req->client->chanfd, &req->client->from, &req->reqHdr);
            break;
            
        case BATCH_JOB_INFO:
            ret = do_jobInfoReq(req->xdr, req->client->chanfd, &req->client->from, &req->reqHdr,req->schedule, req->byQmbd);
            break;
            
        default:
            ret = -1;
            break;
    }
    
    xdr_destroy(req->xdr);
    chanFreeBuf_(req->buf);
    req->client->state = CLIENT_STATE_PROCESS_FINISHED;
    req->client->reqType = req->reqHdr.opCode;
    req->client->lastTime = now;
    return NULL;
}

/*
 * Processes IO events for all clients in the list
 * Handles channel errors, triggers request processing on read events, and closes invalid clients
 */
static void 
clientIO(){
    struct clientNode *cliPtr;
    struct clientNode *nextClient;
    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"clientIO: Entering...");

    for (cliPtr = clientList->forw;
         cliPtr != clientList;
         cliPtr = nextClient) {
        nextClient = cliPtr->forw;
        if((chanEventsReady(cliPtr->chanfd, EPOLL_EVENTS_ERROR)&& !cliPtr->state == CLIENT_STATE_THREAD_PROCESSING)){
            shutDownClient(cliPtr);
            continue;
        }

        if(cliPtr->state == CLIENT_STATE_CONNECTED){
            if (chanEventsReady(cliPtr->chanfd, EPOLL_EVENTS_READ)) {
            cliPtr->state = CLIENT_STATE_WAITING_THREAD;
            if (logclass & (LC_TRACE | LC_COMM))
                ls_syslog(LOG_DEBUG,"Task append chfd is %d,from %s",cliPtr->chanfd,sockAdd2Str_(&cliPtr->from));
            processClientWithQueryReq(cliPtr);
        }
        } else if(cliPtr->state == CLIENT_STATE_WAITING_THREAD || cliPtr->state == CLIENT_STATE_THREAD_PROCESSING){
            continue;
        } else{
            shutDownClient(cliPtr);
        }
        

    }
}


/*
 * Signal handler for qmbd exit timer
 * Sets qmbdDie flag to indicate qmbd should exit after processing remaining requests
 * @param[in] sig: Signal number (SIGALRM)
 */
static
void qmbdDieAlarmHandler (int sig){
    int pid, saveErrno;
    LS_WAIT_T status;
    sigset_t newmask, oldmask;

    saveErrno = errno;
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGTERM);
    sigaddset(&newmask, SIGINT);
    sigaddset(&newmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    qmbdDie = 1;

    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    errno = saveErrno;
}

/*
 * Initialize qmbd daemon resources
 * Sets up timers, signal handlers, thread pool, epoll, and listening socket
 * @return: 0 on successful initialization, -1 on failure to create listening socket
 */
static
int qmbdInit(){
    int i;
    struct itimerval timer;
    struct clientNode *cliPtr, *nextClient;
    sigset_t old_mask, empty_set;

    if (logclass & LC_TRACE)
        ls_syslog(LOG_DEBUG,"qmbdInit: Entering...");

    chanCloseEpoll();
    qmbdDie = 0;

    sigemptyset(&empty_set);
    sigprocmask(SIG_SETMASK, &empty_set, &old_mask);
    for(cliPtr=clientList->forw; cliPtr != clientList; cliPtr=nextClient) {
        nextClient = cliPtr->forw;
        shutDownClient(cliPtr);
    }
    
    for(i = 3; i<sysconf(_SC_OPEN_MAX); i++){
        close(i);
    }
    timer.it_value.tv_sec = qmbdAliveTime;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, NULL);
    Signal_(SIGALRM, (SIGFUNCTYPE) qmbdDieAlarmHandler);
    Signal_(SIGCHLD, (SIGFUNCTYPE) child_handler);
    Signal_(SIGTERM, (SIGFUNCTYPE) terminate_handler);
    Signal_(SIGHUP,  SIG_IGN);
    Signal_(SIGPIPE, SIG_IGN);

    pool = createThreadPool(qmbdThreadNum, qmbdMaxTaskNum);
    chanEpollInit();
    querySock = init_ServSock(qmbd_port);
    if (querySock < 0) {
        ls_syslog(LOG_ERR, "%s: Cannot get query batch server socket... %M", __func__);
        return -1;
    }else{
        ls_syslog(LOG_DEBUG, "%s: query batch server start , channel is %d, sockfd is %d,port is %d", __func__,querySock,chanSock_(querySock),qmbd_port);
    }
    
}

/*
 * Accept and initialize a new client connection
 * Creates a client node, stores connection details, and adds it to the client list
 * @param[in] socket: Channel descriptor of the listening socket
 */
static void
acceptConnection(int socket)
{
    int s;
    struct sockaddr_in from;
    struct hostent *hp;
    struct clientNode *client;
    s = chanAccept_(socket, (struct sockaddr_in *)&from);

    if (s == -1) {
        ls_syslog(LOG_ERR, "%s Ohmygosh accept() failed... %m", __func__);
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
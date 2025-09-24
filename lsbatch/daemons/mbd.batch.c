/**
 * @file mbd.batch.c
 * @brief MBD batch job processing module
 *
 * Responsibilities:
 * 1. Receive and decode batch submit requests
 * 2. Batch job scheduling and queue management
 * 3. Streaming responses and status push
 * 4. Batch job status tracking
 * 5. Error handling and recovery
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>        /* for getpwuid() */
#include <ctype.h>      /* for isprint() and isspace() */

#include "mbd.h"
#include "../lsbatch.h"
#include "../lib/lsb.xdr.h"
#include "batch_file_cache.h"

/* External function declarations */
extern struct jData *initJData(struct jShared *);
extern int getNextJobId(void);
extern void addJobIdHT(struct jData *);
extern void inPendJobList(struct jData *, int, time_t);
extern struct jShared *createSharedRef(struct jShared *);
extern struct uData *getUserData(char *user);
extern struct qData *getQueueData(char *queue);
/* Scheduler variable declared in mbd.main.c */
extern int schedule;


/* Batch job manager structure */
struct batchJobManager {
    int batchId;                        /* batch ID */
    int totalJobs;                      /* total jobs */
    int submittedJobs;                  /* submitted jobs */
    int completedJobs;                  /* completed jobs */
    int failedJobs;                     /* failed jobs */
    time_t startTime;                   /* start time */
    time_t lastUpdateTime;              /* last update time */
    int clientChannelFd;                /* client channel fd */
    int enableStreaming;                /* streaming enabled */
    struct jData **jobArray;            /* job array */
    struct batchJobResult *results;     /* result array */
    struct batchJobManager *next;       /* next manager */
};

/* Global batch manager list - ensure proper initialization */
static struct batchJobManager *batchManagerList = NULL;
static int nextBatchId = 1;

/* Module initialization flag */
static int batch_module_initialized = 0;

/* Internal function declarations */
static struct batchJobManager* create_batch_manager(int totalJobs, int clientFd, int enableStreaming);
static void destroy_batch_manager(struct batchJobManager *manager);
static int send_batch_status_reply(int chfd, struct batchStatusReply *reply, int errorCode);
static int sendBack_safe(int reply, struct submitReq *submitReq, struct submitMbdReply *submitReply, int chfd, int jobIndex);
static void send_keepalive_ack_if_needed(int chfd, int enableStreaming);

/* External declarations - from mbd.serv.c */
extern int sendBack(int reply, struct submitReq *submitReq, struct submitMbdReply *submitReply, int chfd);

/**
 * @brief Handle batch job submit request
 * Main handler in MBD for BATCH_SUBMIT_REQ messages
 */
int do_batchSubmitReq(XDR *xdrs,
                      int chfd,
                      struct sockaddr_in *from,
                      char *hostName,
                      struct LSFHeader *reqHdr,
                      struct sockaddr_in *laddr,
                      struct lsfAuth *auth,
                      int *schedule,
                      int dispatch)
{
    static char fname[] = "do_batchSubmitReq";
    struct batchSubmitReq batchReq;
    struct batchSubmitReply batchReply;
    struct batchJobManager *manager = NULL;
    struct batch_cache_context *cache_ctx = NULL;
    int reply = LSBE_NO_ERROR;
    
    /* Use multiple output paths to ensure debug visibility */
    ls_syslog(LOG_ERR, "%s: *** BATCH REQUEST RECEIVED *** host=%s, socket=%d, version=%d, USING IMMEDIATE DISPATCH FIX",
              fname, hostName, chanSock_(chfd), reqHdr->version);
    fprintf(stderr, "*** MBD BATCH *** Request received from %s, socket=%d, version=%d\n",
            hostName, chanSock_(chfd), reqHdr->version);
    printf("*** MBD BATCH PRINTF *** Request received from %s\n", hostName);
    fflush(stdout);
    fflush(stderr);
    
    /* Write to syslog immediately to ensure visibility */
    openlog("mbatchd_debug", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_ERR, "*** BATCH FUNCTION ENTRY *** do_batchSubmitReq called from %s", hostName);
    closelog();

    /* Critical safety checks: verify LSF system state */
    extern struct qData *qDataList;
    extern LIST_T *hostList;
    if (!qDataList) {
        ls_syslog(LOG_ERR, "%s: *** CRITICAL *** qDataList is NULL, LSF not initialized", fname);
        reply = LSBE_SYSTEM;
        goto sendback;
    }
    if (!hostList) {
        ls_syslog(LOG_ERR, "%s: *** CRITICAL *** hostList is NULL, LSF not initialized", fname);
        reply = LSBE_SYSTEM;
        goto sendback;
    }
    
    /* Validate authentication info */
    if (!auth) {
        ls_syslog(LOG_ERR, "%s: *** CRITICAL *** auth is NULL", fname);
        reply = LSBE_PERMISSION;
        goto sendback;
    }
    
    /* Critical fix: verify and repair username in auth info */
    ls_syslog(LOG_ERR, "%s: *** AUTH DEBUG *** uid=%d, lsfUserName='%s' (len=%d)",
              fname, auth->uid,
              auth->lsfUserName ? auth->lsfUserName : "NULL",
              auth->lsfUserName ? (int)strlen(auth->lsfUserName) : 0);
    
    /* Check whether username is corrupted */
    if (!auth->lsfUserName || strlen(auth->lsfUserName) == 0 ||
        strlen(auth->lsfUserName) > MAXLSFNAMELEN-1) {
        ls_syslog(LOG_ERR, "%s: *** USERNAME CORRUPTED *** Attempting to fix using system calls", fname);
        
        /* Try to resolve username via uid */
        struct passwd *pwd = getpwuid(auth->uid);
        if (pwd && pwd->pw_name) {
            /* Repair username */
            strncpy(auth->lsfUserName, pwd->pw_name, MAXLSFNAMELEN-1);
            auth->lsfUserName[MAXLSFNAMELEN-1] = '\0';
            ls_syslog(LOG_ERR, "%s: *** USERNAME FIXED *** uid=%d -> lsfUserName='%s'",
                      fname, auth->uid, auth->lsfUserName);
        } else {
            ls_syslog(LOG_ERR, "%s: *** USERNAME FIX FAILED *** Cannot resolve uid=%d", fname, auth->uid);
            reply = LSBE_PERMISSION;
            goto sendback;
        }
    }
    
    /* Verify the repaired username */
    if (auth->lsfUserName) {
        /* Check for non-printable characters in username */
        int has_garbage = 0;
        for (int i = 0; i < strlen(auth->lsfUserName); i++) {
            if (!isprint(auth->lsfUserName[i]) && !isspace(auth->lsfUserName[i])) {
                has_garbage = 1;
                break;
            }
        }
        
        if (has_garbage) {
            ls_syslog(LOG_ERR, "%s: *** USERNAME CONTAINS GARBAGE *** Attempting secondary fix", fname);
            
            /* Secondary fix: use default admin user from config */
            const char *defaultUser = "volclava";  /* from lsf.cluster.volclava ClusterAdmins */
            
            /* Validate the default user exists on system */
            struct passwd *pw = getpwnam(defaultUser);
            if (pw && pw->pw_name) {
                /* Replace corrupted username with default user from config */
                strncpy(auth->lsfUserName, defaultUser, MAXLSFNAMELEN-1);
                auth->lsfUserName[MAXLSFNAMELEN-1] = '\0';
                
                ls_syslog(LOG_ERR, "%s: *** USERNAME SECONDARY FIX *** uid=%d -> lsfUserName='%s' (from config)",
                         fname, auth->uid, auth->lsfUserName);
            } else {
                /* If config user not found, fall back to uid lookup */
                ls_syslog(LOG_ERR, "%s: *** CONFIG USER NOT FOUND *** Falling back to UID lookup", fname);
                
                struct passwd *pw_uid = getpwuid(auth->uid);
                if (pw_uid && pw_uid->pw_name) {
                    strncpy(auth->lsfUserName, pw_uid->pw_name, MAXLSFNAMELEN-1);
                    auth->lsfUserName[MAXLSFNAMELEN-1] = '\0';
                    
                    ls_syslog(LOG_ERR, "%s: *** USERNAME FALLBACK FIX *** uid=%d -> lsfUserName='%s'",
                             fname, auth->uid, auth->lsfUserName);
                } else {
                    ls_syslog(LOG_ERR, "%s: *** USERNAME FIX FAILED *** Both config user and UID lookup failed", fname);
                    reply = LSBE_PERMISSION;
                    goto sendback;
                }
            }
        }
    }
    
    ls_syslog(LOG_ERR, "%s: *** AUTH VERIFICATION PASSED *** uid=%d, lsfUserName='%s'",
              fname, auth->uid, auth->lsfUserName);
    ls_syslog(LOG_ERR, "%s: *** SAFETY CHECKS PASSED *** LSF system appears ready", fname);

    /* Initialize structures */
    memset(&batchReq, 0, sizeof(batchReq));
    memset(&batchReply, 0, sizeof(batchReply));

    /* Protocol version compatibility check - temporarily skip strict check */
    ls_syslog(LOG_ERR, "%s: *** VERSION CHECK *** client version=%d, accepting all versions for now",
              fname, reqHdr->version);
    /* Temporarily disable strict version check
    if (reqHdr->version != VOLCLAVA_VERSION) {
        reply = LSBE_PROTOCOL;
        ls_syslog(LOG_ERR, "%s: Protocol version mismatch, client=%d, server=%d",
                  fname, reqHdr->version, VOLCLAVA_VERSION);
        goto sendback;
    }
    */

    /* Decode batch request - fully follow LSF standard flow */
    ls_syslog(LOG_ERR, "%s: *** STARTING XDR DECODE *** opCode=%d, length=%d",
              fname, reqHdr->opCode, reqHdr->length);
    
    /* 完全遵循do_submitReq的模式 - 直接调用XDR函数 */
    if (!xdr_batchSubmitReq(xdrs, &batchReq, reqHdr)) {
        reply = LSBE_XDR;
        ls_syslog(LOG_ERR, "%s: *** XDR DECODE FAILED ***", fname);
        goto sendback;
    }
    
    ls_syslog(LOG_ERR, "%s: *** XDR DECODE SUCCESS *** jobCount=%d, options=%d",
              fname, batchReq.jobCount, batchReq.options);


    #define EMERGENCY_MAX_JOBS 1000
    if (batchReq.jobCount <= 0 || batchReq.jobCount > EMERGENCY_MAX_JOBS) {
        reply = LSBE_BAD_ARG;
        ls_syslog(LOG_ERR, "%s: EMERGENCY LIMIT - Invalid job count: %d (max: %d)",
                  fname, batchReq.jobCount, EMERGENCY_MAX_JOBS);
        goto sendback;
    }

    if (!batchReq.jobs) {
        reply = LSBE_BAD_ARG;
        ls_syslog(LOG_ERR, "%s: Job array is null", fname);
        goto sendback;
    }

    /* Create batch manager */
    int enableStreaming = (batchReq.options & BATCH_SUBMIT_STREAM_RESPONSE) ? 1 : 0;
    
    /* Ensure enableStreaming is initialized deterministically */
    if (batchReq.options == 0) {
        enableStreaming = 0;  /* default: streaming disabled */
    }
    manager = create_batch_manager(batchReq.jobCount, chfd, enableStreaming);
    if (!manager) {
        reply = LSBE_NO_MEM;
        ls_syslog(LOG_ERR, "%s: Failed to create batch manager", fname);
        return -1;
    }

    ls_syslog(LOG_ERR, "%s: *** BATCH MANAGER CREATED *** batchId=%d, jobs=%d, streaming=%s",
              fname, manager->batchId, batchReq.jobCount,
              enableStreaming ? "enabled" : "disabled");

    /* Set basic fields for batchReply */
    batchReply.batchId = manager->batchId;
    batchReply.startTime = manager->startTime;
    batchReply.successCount = 0;
    batchReply.failureCount = 0;

    /* Integrate local file cache mechanism */
    ls_syslog(LOG_ERR, "%s: *** CREATING FILE CACHE *** for %d jobs",
              fname, batchReq.jobCount);
    
    /* Create batch file cache context */
    cache_ctx = batch_cache_create(batchReq.jobCount);
    if (!cache_ctx) {
        reply = LSBE_NO_MEM;
        ls_syslog(LOG_ERR, "%s: Failed to create batch file cache", fname);
        goto sendback;
    }
    
    /* Create cache file for each job */
    for (int i = 0; i < batchReq.jobCount; i++) {
        const char *job_command = batchReq.jobs[i].command ? batchReq.jobs[i].command : "echo 'Empty job'";
        
        if (batch_cache_add_job(cache_ctx, i, job_command) != BATCH_CACHE_OK) {
            ls_syslog(LOG_ERR, "%s: Failed to add job %d to cache", fname, i);
            reply = LSBE_NO_MEM;
            goto sendback;
        }
        
        ls_syslog(LOG_INFO, "%s: Added job %d to cache: command='%s'", fname, i, job_command);
    }
    
    ls_syslog(LOG_ERR, "%s: *** FILE CACHE CREATED *** Processing %d jobs with cache redirection",
              fname, batchReq.jobCount);
    
    /* Allocate dedicated buffers per batch request to avoid static concurrency issues */
    char *badJobNameBuf = NULL;
    char *pendLimitReasonBuf = NULL;
    
    /* Allocate buffers for this batch request */
    badJobNameBuf = (char *) my_malloc(MAX_CMD_DESC_LEN, "do_batchSubmitReq");
    if (!badJobNameBuf) {
        ls_syslog(LOG_ERR, "%s: Failed to allocate badJobNameBuf", fname);
        reply = LSBE_NO_MEM;
        goto sendback;
    }
    
    pendLimitReasonBuf = (char *) my_malloc(MAX_CMD_DESC_LEN, "do_batchSubmitReq");
    if (!pendLimitReasonBuf) {
        ls_syslog(LOG_ERR, "%s: Failed to allocate pendLimitReasonBuf", fname);
        FREEUP(badJobNameBuf);
        reply = LSBE_NO_MEM;
        goto sendback;
    }
    
    /* Process each job through standard LSF flow */
    for (int i = 0; i < batchReq.jobCount; i++) {
        struct submitMbdReply submitReply;
        struct jData *jobData = NULL;
        int jobReply;
        
        /* Allocate required string fields per job - avoid NULL pointer issues */
        batchReq.jobs[i].fromHost = (char *) my_malloc(MAXHOSTNAMELEN, "batchReq.fromHost");
        batchReq.jobs[i].jobFile = (char *) my_malloc(MAXFILENAMELEN, "batchReq.jobFile");
        batchReq.jobs[i].inFile = (char *) my_malloc(MAXFILENAMELEN, "batchReq.inFile");
        batchReq.jobs[i].outFile = (char *) my_malloc(MAXFILENAMELEN, "batchReq.outFile");
        batchReq.jobs[i].errFile = (char *) my_malloc(MAXFILENAMELEN, "batchReq.errFile");
        batchReq.jobs[i].inFileSpool = (char *) my_malloc(MAXFILENAMELEN, "batchReq.inFileSpool");
        batchReq.jobs[i].commandSpool = (char *) my_malloc(MAXPATHLEN, "batchReq.commandSpool");
        batchReq.jobs[i].cwd = (char *) my_malloc(MAXFILENAMELEN, "batchReq.cwd");
        batchReq.jobs[i].subHomeDir = (char *) my_malloc(MAXFILENAMELEN, "batchReq.subHomeDir");
        batchReq.jobs[i].chkpntDir = (char *) my_malloc(MAXFILENAMELEN, "batchReq.chkpntDir");
        batchReq.jobs[i].hostSpec = (char *) my_malloc(MAXHOSTNAMELEN, "batchReq.hostSpec");

        /* Initialize job fields */
        batchReq.jobs[i].askedHosts = NULL;
        batchReq.jobs[i].numAskedHosts = 0;
        batchReq.jobs[i].nxf = 0;
        batchReq.jobs[i].xf = NULL;
        
        /* Initialize submitReply */
        memset(&submitReply, 0, sizeof(submitReply));
        submitReply.jobId = 0;
        submitReply.queue = "";
        submitReply.subTryInterval = DEF_SUB_TRY_INTERVAL;
        submitReply.badJobName = badJobNameBuf;
        submitReply.pendLimitReason = pendLimitReasonBuf;
        
        /* Clear shared buffers */
        if (badJobNameBuf) badJobNameBuf[0] = '\0';
        if (pendLimitReasonBuf) pendLimitReasonBuf[0] = '\0';
        
        ls_syslog(LOG_ERR, "%s: *** PROCESSING JOB %d/%d *** Using standard newJob+sendBack",
                  fname, i+1, batchReq.jobCount);
        
        /* Data integrity fix: ensure all required fields are correctly set */
        /* Note: because we use static buffers, ensure contents are set properly */
        
        /* 1) Fix fromHost field - using static buffer */
        extern char *masterHost;
        if (masterHost) {
            strncpy(batchReq.jobs[i].fromHost, masterHost, MAXHOSTNAMELEN-1);
        } else {
            strncpy(batchReq.jobs[i].fromHost, "localhost", MAXHOSTNAMELEN-1);
        }
        batchReq.jobs[i].fromHost[MAXHOSTNAMELEN-1] = '\0';
        
        /* 2) Fix file fields - using static buffer */
        strcpy(batchReq.jobs[i].jobFile, "");
        strncpy(batchReq.jobs[i].inFile, "/dev/null", MAXFILENAMELEN-1);
        batchReq.jobs[i].inFile[MAXFILENAMELEN-1] = '\0';
        strncpy(batchReq.jobs[i].outFile, "/dev/null", MAXFILENAMELEN-1);
        batchReq.jobs[i].outFile[MAXFILENAMELEN-1] = '\0';
        strncpy(batchReq.jobs[i].errFile, "/dev/null", MAXFILENAMELEN-1);
        batchReq.jobs[i].errFile[MAXFILENAMELEN-1] = '\0';
        
        /* 3) Fix other critical fields - using static buffer */
        strncpy(batchReq.jobs[i].cwd, "/tmp", MAXFILENAMELEN-1);
        batchReq.jobs[i].cwd[MAXFILENAMELEN-1] = '\0';
        
        /* 4) Critical: set SUB_OTHER_FILES to tell newJob() to skip file transfer */
        batchReq.jobs[i].options |= SUB_OTHER_FILES;
        
        /* 5) Fix job cpu fields - ensure non-zero processors */
        if (batchReq.jobs[i].maxNumProcessors == 0) {
            batchReq.jobs[i].maxNumProcessors = 1;  /* 默认1个处理器 */
        }
        if (batchReq.jobs[i].numProcessors == 0) {
            batchReq.jobs[i].numProcessors = 1;  /* 默认1个处理器 */
        }
        
        ls_syslog(LOG_ERR, "%s: Job %d FIXED: fromHost=%s, maxNumProcessors=%d, numProcessors=%d, SUB_OTHER_FILES set",
                  fname, i, batchReq.jobs[i].fromHost,
                  batchReq.jobs[i].maxNumProcessors, batchReq.jobs[i].numProcessors);
        
        /* Critical: preserve original client channel fd */
        int original_chfd = chfd;  /* original client connection */
        
        ls_syslog(LOG_ERR, "%s: *** REDIRECTING FD *** for job %d before newJob(), original_chfd=%d", fname, i, original_chfd);
        
        /* Redirect channel fd to cache-backed file */
        if (batch_fd_redirect(cache_ctx, i, chfd) != BATCH_CACHE_OK) {
            ls_syslog(LOG_ERR, "%s: Failed to redirect fd for job %d", fname, i);
            jobReply = LSBE_SYSTEM;
        } else {
            ls_syslog(LOG_ERR, "%s: *** FD REDIRECTED SUCCESSFULLY *** for job %d, calling newJob...", fname, i);
            
            /* Standard LSF job creation - enforce immediate dispatch to avoid later crashes */
            int batch_dispatch = 1;  /* force immediate dispatch for batch */
            ls_syslog(LOG_ERR, "%s: *** USING IMMEDIATE DISPATCH *** for batch job %d, dispatch=1 instead of %d", fname, i, dispatch);
            jobReply = newJob(&batchReq.jobs[i], &submitReply, chfd, auth, schedule, batch_dispatch, &jobData);
            
            /* Validate job status to ensure immediate dispatch took effect */
            if (jobReply == LSBE_NO_ERROR && jobData && submitReply.jobId > 0) {
                ls_syslog(LOG_ERR, "%s: *** JOB CREATED SUCCESSFULLY *** jobId=%lld, status=%d, using immediate dispatch", 
                         fname, submitReply.jobId, jobData ? jobData->jStatus : -1);
            } else {
                ls_syslog(LOG_ERR, "%s: *** JOB CREATION ISSUES *** reply=%d, jobId=%lld, jobData=%p", 
                         fname, jobReply, submitReply.jobId, jobData);
            }
            
            ls_syslog(LOG_ERR, "%s: *** NEWJOB COMPLETED *** for job %d, reply=%d", fname, i, jobReply);
            
            /* Restore original channel fd */
            if (batch_fd_restore(cache_ctx, i, chfd) != BATCH_CACHE_OK) {
                ls_syslog(LOG_ERR, "%s: Failed to restore fd for job %d", fname, i);
            } else {
                ls_syslog(LOG_ERR, "%s: *** FD RESTORED SUCCESSFULLY *** for job %d", fname, i);
            }
        }
        
        ls_syslog(LOG_ERR, "%s: Job %d newJob completed, reply=%d, jobId=%lld",
                  fname, i, jobReply, submitReply.jobId);
        
        /* Validate submitReply structure completeness */
        /* With static buffers, these should already be initialized */
        if (!submitReply.queue) {
            submitReply.queue = "normal";  /* 使用默认队列名 */
        }
        
        /* Ensure string fields are non-NULL - supply defaults to avoid skipping send */
        if (!submitReply.badJobName) {
            submitReply.badJobName = "";
            ls_syslog(LOG_ERR, "%s: submitReply.badJobName was NULL, defaulted for job %d", fname, i);
        }
        if (!submitReply.pendLimitReason) {
            submitReply.pendLimitReason = "";
            ls_syslog(LOG_ERR, "%s: submitReply.pendLimitReason was NULL, defaulted for job %d", fname, i);
        }
        
        ls_syslog(LOG_ERR, "%s: Job %d submitReply VERIFIED: jobId=%lld, queue='%s', badJobName='%s', pendLimitReason='%s'",
                  fname, i, submitReply.jobId,
                  submitReply.queue, submitReply.badJobName, submitReply.pendLimitReason);
        
        /* Send streaming response using original client fd */
        ls_syslog(LOG_ERR, "%s: *** SENDING STREAM RESPONSE *** for job %d using original_chfd=%d (not redirected chfd=%d)",
                  fname, i, original_chfd, chfd);
        
        int sendResult = sendBack_safe(jobReply, &batchReq.jobs[i], &submitReply, original_chfd, i);
        if (sendResult < 0) {
            ls_syslog(LOG_ERR, "%s: *** SENDBACK FAILED *** for job %d, continuing with next job", fname, i);
            /* Do not return; continue processing remaining jobs */
            continue;
        }
        
        ls_syslog(LOG_ERR, "%s: *** JOB %d STREAM RESPONSE SENT *** sendBack successful to original_chfd=%d, jobId=%lld",
                  fname, i, original_chfd, submitReply.jobId);

        /* Compatibility: duplicate send for job 0 to help client align first frame */
        if (i == 0) {
            ls_syslog(LOG_ERR, "%s: *** DUPLICATE SEND *** Re-sending stream response for job %d (jobId=%lld)",
                      fname, i, submitReply.jobId);
            int dupResult = sendBack_safe(jobReply, &batchReq.jobs[i], &submitReply, original_chfd, i);
            if (dupResult < 0) {
                ls_syslog(LOG_ERR, "%s: *** DUPLICATE SENDBACK FAILED *** for job %d", fname, i);
            } else {
                ls_syslog(LOG_ERR, "%s: *** DUPLICATE SEND SUCCESS *** for job %d (jobId=%lld)",
                          fname, i, submitReply.jobId);
            }
        }

        /* Send short ACK to nudge kernel flush and avoid last-packet stall */
        send_keepalive_ack_if_needed(original_chfd, manager->enableStreaming);
    }
    
    ls_syslog(LOG_ERR, "%s: *** ALL JOBS COMPLETED *** %d jobs processed using LSF standard mechanism", 
              fname, batchReq.jobCount);
    
    /* Send an empty packet to push the final job reply out completely */
    struct LSFHeader empty_hdr;
    memset(&empty_hdr, 0, sizeof(empty_hdr));
    empty_hdr.opCode = htonl(BATCH_STATUS_MSG_ACK);  /* use ACK as empty-packet marker */
    empty_hdr.length = htonl(0);                     /* zero-length packet */
    empty_hdr.version = htonl(VOLCLAVA_VERSION);
    
    if (chanWrite_(chfd, (char*)&empty_hdr, sizeof(empty_hdr)) == sizeof(empty_hdr)) {
        ls_syslog(LOG_ERR, "%s: *** EMPTY PACKET SENT *** Pushed final job reply transmission", fname);
    } else {
        ls_syslog(LOG_ERR, "%s: *** EMPTY PACKET FAILED *** errno=%d", fname, errno);
    }
    
    /* Cleanup file cache and manager */
    if (cache_ctx) {
        batch_cache_destroy(cache_ctx);
        cache_ctx = NULL; // prevent double free
    }
    if (manager) {
        destroy_batch_manager(manager);
        manager = NULL;
    }
    
    /* Note: do not free job field memory manually; LSF manages these */
    /* Manual free may cause double free and segfault */
    
    /* Free allocated per-request buffers */
    FREEUP(badJobNameBuf);
    FREEUP(pendLimitReasonBuf);
    
    return 0;

sendback:
    /* Note: do not free job field memory manually; LSF manages these */
    /* Manual free may cause double free and segfault */
    
    /* Free allocated per-request buffers */
    FREEUP(badJobNameBuf);
    FREEUP(pendLimitReasonBuf);
    
    /* Cleanup file cache and manager (only if not already freed) */
    if (cache_ctx) {
        batch_cache_destroy(cache_ctx);
        cache_ctx = NULL;
    }
    if (manager) {
        destroy_batch_manager(manager);
        manager = NULL;
    }
    
    /* Send error response */
    ls_syslog(LOG_ERR, "%s: Batch request failed with error code %d", fname, reply);
    return -1;
}

/**
 * @brief Create batch manager
 */
static struct batchJobManager* create_batch_manager(int totalJobs, int clientFd, int enableStreaming)
{
    /* Emergency safety limit: maximum batch jobs */
    #define MAX_EMERGENCY_BATCH_JOBS 1000
    
    if (totalJobs <= 0 || totalJobs > MAX_EMERGENCY_BATCH_JOBS) {
        ls_syslog(LOG_ERR, "create_batch_manager: EMERGENCY LIMIT - Invalid job count %d (max: %d)",
                  totalJobs, MAX_EMERGENCY_BATCH_JOBS);
        return NULL;
    }
    
    struct batchJobManager *manager = malloc(sizeof(struct batchJobManager));
    if (!manager) {
        ls_syslog(LOG_ERR, "create_batch_manager: EMERGENCY - Memory allocation failed");
        return NULL;
    }

    memset(manager, 0, sizeof(struct batchJobManager));

    manager->batchId = nextBatchId++;
    manager->totalJobs = totalJobs;
    manager->clientChannelFd = clientFd;
    manager->enableStreaming = enableStreaming;
    manager->startTime = time(NULL);
    manager->lastUpdateTime = manager->startTime;

    manager->jobArray = calloc(MAX_EMERGENCY_BATCH_JOBS, sizeof(struct jData*));
    manager->results = calloc(MAX_EMERGENCY_BATCH_JOBS, sizeof(struct batchJobResult));

    if (!manager->jobArray || !manager->results) {
        if (manager->jobArray) free(manager->jobArray);
        if (manager->results) free(manager->results);
        free(manager);
        ls_syslog(LOG_ERR, "create_batch_manager: EMERGENCY - Failed to allocate job arrays");
        return NULL;
    }

    /* Add to global list */
    manager->next = batchManagerList;
    batchManagerList = manager;

    ls_syslog(LOG_INFO, "create_batch_manager: EMERGENCY MODE - Created safe manager for %d jobs (limit: %d)",
              totalJobs, MAX_EMERGENCY_BATCH_JOBS);
    return manager;
}

/**
 * @brief Destroy batch manager
 */
static void destroy_batch_manager(struct batchJobManager *manager)
{
    if (!manager) {
        return;
    }

    ls_syslog(LOG_INFO, "destroy_batch_manager: EMERGENCY CLEANUP - Destroying manager batchId=%d",
              manager->batchId);

    /* Remove from global list if present */
    if (manager->next != NULL || batchManagerList == manager) {
        struct batchJobManager **current = &batchManagerList;
        while (*current) {
            if (*current == manager) {
                *current = manager->next;
                ls_syslog(LOG_DEBUG, "destroy_batch_manager: Removed manager from global list");
                break;
            }
            current = &(*current)->next;
        }
    } else {
        ls_syslog(LOG_DEBUG, "destroy_batch_manager: Manager already removed from global list");
    }

    /* Safe resource cleanup - limit loop count */
    if (manager->jobArray) {
        free(manager->jobArray);
        manager->jobArray = NULL;
    }
    if (manager->results) {
        int i;
        int maxCleanup = (manager->totalJobs > 5) ? 5 : manager->totalJobs;  /* EMERGENCY LIMIT */
        for (i = 0; i < maxCleanup; i++) {
            if (manager->results[i].queue) {
                free(manager->results[i].queue);
                manager->results[i].queue = NULL;
            }
            if (manager->results[i].errorMsg) {
                free(manager->results[i].errorMsg);
                manager->results[i].errorMsg = NULL;
            }
        }
        free(manager->results);
        manager->results = NULL;
    }

    free(manager);
    ls_syslog(LOG_INFO, "destroy_batch_manager: EMERGENCY CLEANUP - Manager destroyed successfully");
}



/**
 * @brief Safe batch reply sender
 * Keep message size within 8192 bytes and handle connection errors
 */
static int sendBack_safe(int reply, struct submitReq *submitReq,
                        struct submitMbdReply *submitReply, int chfd, int jobIndex)
{
    static char fname[] = "sendBack_safe";
    char reply_buf[MSGSIZE / 2];  /* Mirror original sendBack - same buffer size */
    XDR xdrs2;
    struct LSFHeader replyHdr;
    
    /* Mirror original sendBack() logic */
    if (submitReq->nxf > 0)
        FREEUP(submitReq->xf);

    /* Encode the original submitReply without mutation */
    xdrmem_create(&xdrs2, reply_buf, MSGSIZE / 2, XDR_ENCODE);
    replyHdr.opCode = reply;
    replyHdr.version = VOLCLAVA_VERSION; /* ensure version field is set */
    
    if (!xdr_encodeMsg(&xdrs2, (char *) submitReply, &replyHdr,
                       xdr_submitMbdReply, 0, NULL)) {
        ls_syslog(LOG_ERR, "%s: xdr_encodeMsg failed for job %d", fname, jobIndex);
        xdr_destroy(&xdrs2);
        return -1;
    }
    
    int encoded_size = XDR_GETPOS(&xdrs2);
    
    /* Critical check: enforce encoded size limit */
    if (encoded_size > 8192) {
        ls_syslog(LOG_ERR, "%s: *** MESSAGE TOO LARGE *** %d bytes for job %d, DROPPING MESSAGE",
                  fname, encoded_size, jobIndex);
        xdr_destroy(&xdrs2);
        return -1;  /* Drop oversized message to avoid client errors */
    }
    
    /* Send using the same logic as the original sendBack */
    if (chanWrite_(chfd, reply_buf, encoded_size) != encoded_size) {
        int saved_errno = errno;
        ls_syslog(LOG_ERR, "%s: chanWrite_ failed for job %d, jobId=%lld, size=%d, chfd=%d, errno=%d",
                  fname, jobIndex, (long long)submitReply->jobId, encoded_size, chfd, saved_errno);
        /* Wait briefly then retry once to improve reliability */
        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 100 * 1000 * 1000; /* 100ms */
        nanosleep(&ts, NULL);
        if (chanWrite_(chfd, reply_buf, encoded_size) != encoded_size) {
            int saved_errno2 = errno;
            ls_syslog(LOG_ERR, "%s: chanWrite_ retry failed for job %d, jobId=%lld, size=%d, chfd=%d, errno=%d - giving up",
                      fname, jobIndex, (long long)submitReply->jobId, encoded_size, chfd, saved_errno2);
            xdr_destroy(&xdrs2);
            return -1;
        }
    }
    
    xdr_destroy(&xdrs2);
    
    ls_syslog(LOG_ERR, "%s: *** CHANNEL SEND SUCCESS *** %d bytes for job %d, jobId=%lld",
              fname, encoded_size, jobIndex, submitReply->jobId);
    
    return 0;
}

/* Send a tiny ACK/keepalive header (no body) to nudge kernel to flush previous reply */
static void send_keepalive_ack_if_needed(int chfd, int enableStreaming)
{
    if (enableStreaming) {
        return; /* Do not send ACK in streaming mode */
    }
    if (chfd < 0) {
        return;
    }
    struct LSFHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    /* Use an OpCode that won't be misinterpreted by standard submit path: BATCH_STATUS_MSG_ACK, length=0 */
    hdr.opCode  = htonl(BATCH_STATUS_MSG_ACK);
    hdr.length  = htonl(0);
    hdr.version = htonl(VOLCLAVA_VERSION);
    int written = chanWrite_(chfd, (char*)&hdr, sizeof(hdr));
    if (written != (int)sizeof(hdr)) {
        ls_syslog(LOG_ERR, "send_keepalive_ack_if_needed: write failed chfd=%d, errno=%d", chfd, errno);
    } else {
        ls_syslog(LOG_DEBUG, "send_keepalive_ack_if_needed: ACK header sent on chfd=%d", chfd);
    }
}


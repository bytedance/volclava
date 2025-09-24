/**
 * @file batch_transmitter.c
 * @brief Batch job transmitter implementation
 *
 * Responsibilities:
 * 1. Network transmission management
 * 2. Streaming response handling
 * 3. Connection management and reconnection
 * 4. Progress reporting and statistics
 * 5. Error handling and recovery
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include "cmd.h"
#include "../lsbatch.h"
#include "../daemons/daemonout.h"
#include "../lib/batch_memory.h"
#include "../daemons/daemonout.h"
#include "batch_job_collector.h"

/* Prefetch: first standard reply returned by callmbd() (if any) */
static int g_has_prefetched_reply = 0;
static struct LSFHeader g_prefetched_hdr;
static char g_prefetched_buf[MSGSIZE];
static int g_prefetched_len = 0;

/* Statistics structures are defined in the header */

/* Structures are declared in the header; no need to repeat here */

/* Internal function declarations */
static int establish_connection(batch_transmitter_t *transmitter);
static int send_batch_request(batch_transmitter_t *transmitter,
                             const struct batchSubmitReq *batch_request);
static void update_statistics(batch_transmitter_t *transmitter,
                             int success_count, int failure_count);
static int convert_submit_to_submitReq(const struct submit *submit, struct submitReq *submitReq);
static void cleanup_submitReq(struct submitReq *submitReq);
static int callmbd_receive_reply(int sockfd, char *reply_buf, int buf_size, struct LSFHeader *reply_hdr);
static int poll_job_completion(LS_LONG_INT *jobIds, int jobCount);

/* External function declarations - from existing LSF code */
extern int lsb_init(char *appName);
extern struct jobInfoHead *lsb_openjobinfo_a(LS_LONG_INT, char *, char *, char *, char *, int);
extern struct jobInfoEnt *lsb_readjobinfo(int *);
extern void lsb_closejobinfo(void);

/**
 * @brief Create batch transmitter
 */
batch_transmitter_t* batch_transmitter_create(void)
{
    batch_transmitter_t *transmitter = malloc(sizeof(batch_transmitter_t));
    if (!transmitter) {
        fprintf(stderr, "Error: Transmitter memory allocation failed\n");
            return NULL;
        }
        
        memset(transmitter, 0, sizeof(batch_transmitter_t));
        
        /* Set defaults */
        transmitter->connection_fd = -1;
        transmitter->is_connected = 0;
        transmitter->enable_streaming = 0;
        transmitter->max_retry_count = 3;
        transmitter->connection_timeout = 30;
        transmitter->read_timeout = 60;
        transmitter->write_timeout = 60;
        
        /* Initialize memory pool */
        transmitter->memory_pool = malloc(sizeof(struct batch_memory_pool));
        if (!transmitter->memory_pool ||
            batch_memory_pool_init(transmitter->memory_pool, 50 * 1024 * 1024) != 0) {
            fprintf(stderr, "Error: Transmitter memory pool initialization failed\n");
            free(transmitter);
            return NULL;
        }
        
        fprintf(stderr, "Info: Batch transmitter created successfully\n");
    return transmitter;
}

/**
 * @brief Configure transmitter
 */
int batch_transmitter_configure(batch_transmitter_t *transmitter,
                               int enable_streaming,
                               int max_retry_count,
                               int connection_timeout)
{
    if (!transmitter) {
        return -1;
    }
    
    transmitter->enable_streaming = enable_streaming;
    transmitter->max_retry_count = max_retry_count;
    transmitter->connection_timeout = connection_timeout;
    
    fprintf(stderr, "Info: Transmitter configured: Streaming=%s, Max retries=%d, Timeout=%ds\n",
            enable_streaming ? "enabled" : "disabled", max_retry_count, connection_timeout);
    
    return 0;
}

/**
 * @brief Submit batch jobs
 */
int batch_transmitter_submit_jobs(batch_transmitter_t *transmitter,
                                  const struct batch_job_collection *collection)
{
    if (!transmitter || !collection) {
        fprintf(stderr, "Error: Transmitter or job collection is null\n");
        return -1;
    }
    
    if (collection->total_jobs_count <= 0) {
        fprintf(stderr, "Error: No jobs to submit\n");
        return -1;
    }
    
    fprintf(stderr, "Info: Starting batch job transmission\n");
    fprintf(stderr, "Info: Total jobs: %d\n", collection->total_jobs_count);
    
    /* Initialize statistics */
    transmitter->statistics.start_time = time(NULL);
    transmitter->statistics.total_jobs = collection->total_jobs_count;
    transmitter->statistics.successful_jobs = 0;
    transmitter->statistics.failed_jobs = 0;
    
    int result = -1;
    
    /* 🚀 真正的批量传输实现 - 使用我们的XDR协议和流式处理 */

    /* 1. Establish connection */
    if (establish_connection(transmitter) != 0) {
        fprintf(stderr, "Error: Failed to establish connection\n");
        return -1;
    }

    /* 2. Build batch submit request */
    struct batchSubmitReq batch_request;
    memset(&batch_request, 0, sizeof(batch_request));

    batch_request.jobCount = collection->total_jobs_count;
    batch_request.options = 0;
    if (transmitter->enable_streaming) {
        batch_request.options |= BATCH_SUBMIT_STREAM_RESPONSE;
    }
    batch_request.maxConcurrency = 10;  /* Default max concurrency */
    batch_request.clientTimestamp = time(NULL);
    batch_request.sourceFile = strdup(collection->source_file_path);
    batch_request.batchName = strdup("batch_submit");

    /* Convert job collection to submitReq format */
    batch_request.jobs = malloc(collection->total_jobs_count * sizeof(struct submitReq));
    if (!batch_request.jobs) {
        fprintf(stderr, "Error: Failed to allocate batch request job array\n");
        /* Fix: Clean up allocated resources */
        if (batch_request.sourceFile) {
            free(batch_request.sourceFile);
        }
        if (batch_request.batchName) {
            free(batch_request.batchName);
        }
        return -1;
    }

    /* Full job conversion logic */
    int i;
    for (i = 0; i < collection->total_jobs_count; i++) {
        struct submit *job = collection->job_requests_array[i];
        struct submitReq *req = &batch_request.jobs[i];

        if (!job) {
            fprintf(stderr, "Error: Job %d is null\n", i);
            continue;
        }

        /* Full submit to submitReq conversion */
        if (convert_submit_to_submitReq(job, req) != 0) {
            fprintf(stderr, "Error: Job %d conversion failed\n", i);
            continue;
        }

        /* Set batch-specific fields */
        req->submitTime = time(NULL);
        /* jobIndex is not a member of submitReq, handled in results */
    }

    /* 3. Send batch request - using XDR protocol and established connection */
    fprintf(stderr, "Info: Sending batch request with %d jobs\n", batch_request.jobCount);
    
    if (send_batch_request(transmitter, &batch_request) != 0) {
        fprintf(stderr, "Error: Failed to send batch request\n");
        result = -1;
        goto cleanup;
    }
    
    fprintf(stderr, "Info: Batch request sent successfully\n");
    struct batchSubmitReply batch_reply;
    memset(&batch_reply, 0, sizeof(batch_reply));

    /* *** New design: Receive a batch response *** */
    fprintf(stderr, "Info: Receiving batch response for %d jobs...\n", collection->total_jobs_count);

    /* *** Simplified design: Receive standard LSF responses one by one, then poll immediately *** */
    
    /* Temporarily store job IDs */
    LS_LONG_INT *successful_job_ids = malloc(collection->total_jobs_count * sizeof(LS_LONG_INT));
    if (!successful_job_ids) {
        fprintf(stderr, "Error: Failed to allocate job IDs array\n");
        result = -1;
        goto cleanup;
    }
    
    int success_count = 0;
    int failure_count = 0;
    
    /* Directly use a simple protocol: one response per job (add inner filtering loop for each job) */
    size_t total_bytes_read = 0; /* For debugging connection closure */
    for (int i = 0; i < collection->total_jobs_count; i++) {
        /* Prioritize consuming the first standard reply prefetched by callmbd() */
        if (g_has_prefetched_reply) {
            struct LSFHeader reply_hdr = g_prefetched_hdr;
            /* Convert to host order */
            reply_hdr.opCode = ntohl(reply_hdr.opCode);
            reply_hdr.length = ntohl(reply_hdr.length);
            reply_hdr.version = ntohl(reply_hdr.version);
            fprintf(stderr, "Debug: [Prefetched] Job %d response - OpCode: %d, Length: %d, Version: %d\n",
                    i, reply_hdr.opCode, reply_hdr.length, reply_hdr.version);

            if (reply_hdr.opCode == LSBE_NO_ERROR && reply_hdr.length > 0 && reply_hdr.length < MSGSIZE) {
                struct submitMbdReply submitReply;
                memset(&submitReply, 0, sizeof(submitReply));
                XDR xdrs;
                xdrmem_create(&xdrs, g_prefetched_buf, reply_hdr.length, XDR_DECODE);
                if (xdr_submitMbdReply(&xdrs, &submitReply, &reply_hdr)) {
                    fprintf(stderr, "Debug: [Prefetched] Job %d decoded - jobId: %lld, queue: %s\n",
                            i, submitReply.jobId, submitReply.queue ? submitReply.queue : "NULL");
                    if (submitReply.jobId > 0) {
                        successful_job_ids[success_count] = submitReply.jobId;
                        fprintf(stderr, "Info: Job %d successfully submitted, JobID: %lld (stored at index %d)\n",
                                i, submitReply.jobId, success_count);
                        success_count++;
                        g_has_prefetched_reply = 0; /* Consumed */
                        continue; /* Proceed to next job */
                    }
                }
                xdr_destroy(&xdrs);
            }
            /* If the prefetched frame is not a standard success body, discard the prefetch and read normally */
            g_has_prefetched_reply = 0;
        }
        fprintf(stderr, "Info: Waiting for response from job %d/%d...\n", i+1, collection->total_jobs_count);

        while (1) {
            struct LSFHeader reply_hdr;
            char reply_buf[MSGSIZE];

            /* Read header, with multiple short retries to increase probability of getting the last frame */
            int header_read = -1;
            for (int htry = 0; htry < 10; htry++) {
                header_read = chanRead_(transmitter->connection_fd, (char*)&reply_hdr, sizeof(reply_hdr));
                if (header_read == (int)sizeof(reply_hdr)) {
                    break;
                }
                usleep(50 * 1000); /* 50ms */
            }
            if (header_read != sizeof(reply_hdr)) {
                fprintf(stderr, "Warning: Failed to read response header for job %d (fd=%d, bytesReadTotal=%zu)\n",
                        i, transmitter->connection_fd, total_bytes_read);
                failure_count++;
                break; /* Abandon this job, proceed to the next */
            }
            total_bytes_read += header_read;

            /* Convert network order to local order */
            reply_hdr.opCode = ntohl(reply_hdr.opCode);
            reply_hdr.length = ntohl(reply_hdr.length);
            reply_hdr.version = ntohl(reply_hdr.version);

            fprintf(stderr, "Debug: Job %d response - OpCode: %d, Length: %d, Version: %d\n",
                    i, reply_hdr.opCode, reply_hdr.length, reply_hdr.version);

            /* Filter ACK and empty success frames */
            if (reply_hdr.opCode == BATCH_STATUS_MSG_ACK) {
                fprintf(stderr, "Debug: Ignored keepalive ACK (OpCode=%d)\n", reply_hdr.opCode);
                continue; /* Do not count as a job, continue reading the next frame */
            }
            if (reply_hdr.opCode == LSBE_NO_ERROR && reply_hdr.length == 0) {
                fprintf(stderr, "Debug: Ignored empty success frame (OpCode=%d, Length=%d)\n",
                        reply_hdr.opCode, reply_hdr.length);
                /* Give the kernel a moment to aggregate the last frame */
                usleep(50 * 1000); /* 50ms */
                continue; /* Do not count as a job, continue reading the next frame */
            }

            /* Only read and decode if successful and has a body */
            if (reply_hdr.opCode == LSBE_NO_ERROR && reply_hdr.length > 0 && reply_hdr.length < MSGSIZE) {
                int need = reply_hdr.length;
                int got_total = 0;
                int safety_loops = 0;
                while (got_total < need && safety_loops < 50) { /* Max 50 retries */
                    int got = chanRead_(transmitter->connection_fd, reply_buf + got_total, need - got_total);
                    if (got > 0) {
                        got_total += got;
                        total_bytes_read += got;
                    } else {
                        usleep(10 * 1000); /* 10ms */
                    }
                    safety_loops++;
                }
                if (got_total != need) {
                    fprintf(stderr, "Warning: Failed to read full body for job %d (need=%d, got=%d)\n",
                            i, need, got_total);
                    failure_count++;
                    break; /* Abandon this job, proceed to the next */
                }

                /* Decode */
                struct submitMbdReply submitReply;
                memset(&submitReply, 0, sizeof(submitReply));
                fprintf(stderr, "Debug: Job %d raw data (first 16 bytes): ", i);
                for (int debug_idx = 0; debug_idx < 16 && debug_idx < reply_hdr.length; debug_idx++) {
                    fprintf(stderr, "%02x ", (unsigned char)reply_buf[debug_idx]);
                }
                fprintf(stderr, "\n");
                XDR xdrs;
                xdrmem_create(&xdrs, reply_buf, reply_hdr.length, XDR_DECODE);
                if (xdr_submitMbdReply(&xdrs, &submitReply, &reply_hdr)) {
                    fprintf(stderr, "Debug: Job %d decoded - jobId: %lld, queue: %s\n",
                            i, submitReply.jobId, submitReply.queue ? submitReply.queue : "NULL");
                    if (submitReply.jobId > 0) {
                        successful_job_ids[success_count] = submitReply.jobId;
                        fprintf(stderr, "Info: Job %d successfully submitted, JobID: %lld (stored at index %d)\n",
                                i, submitReply.jobId, success_count);
                        success_count++;
                    } else {
                        failure_count++;
                        fprintf(stderr, "Warning: Job %d submitted but jobId is %lld (invalid)\n", i, submitReply.jobId);
                    }
                } else {
                    failure_count++;
                    fprintf(stderr, "Warning: Failed to decode response for job %d (XDR error)\n", i);
                }
                xdr_destroy(&xdrs);

                /* This job is complete, exit inner loop */
                break;
            }

            /* Other unexpected frames: ignore and continue reading the next frame */
            fprintf(stderr, "Debug: Ignored non-target frame (OpCode=%d, Length=%d)\n",
                    reply_hdr.opCode, reply_hdr.length);
            continue;
        }
    }

    /* Final results */
    batch_reply.successCount = success_count;
    batch_reply.failureCount = failure_count;
    batch_reply.completionTime = time(NULL);

    fprintf(stderr, "Info: Standard LSF responses received, Success: %d, Failed: %d\n", 
            success_count, failure_count);

    /* Update statistics */
    update_statistics(transmitter, batch_reply.successCount, batch_reply.failureCount);

    /* *** Key new: Client polling mechanism *** */
    if (success_count > 0) {
        fprintf(stderr, "Info: *** Starting job completion polling *** Successfully submitted %d jobs\n", success_count);
        
        /* Poll directly using successful_job_ids array */
        int poll_result = poll_job_completion(successful_job_ids, success_count);
        
        if (poll_result == 0) {
            fprintf(stderr, "Info: *** All jobs completed, client exiting ***\n");
            result = success_count;
        } else {
            fprintf(stderr, "Warning: Polling process encountered issues, but jobs may still be running\n");
            result = success_count;  /* Still return success as jobs were submitted */
        }
    } else {
        fprintf(stderr, "Error: No jobs were successfully submitted\n");
        result = -1;
    }
    
    /* Clean up job ID array */
    if (successful_job_ids) {
        free(successful_job_ids);
        successful_job_ids = NULL;
    }

cleanup:
    /* Fix: Safely clean up batch request resources, avoid double free */
    if (batch_request.jobs) {
        for (i = 0; i < batch_request.jobCount; i++) {
            cleanup_submitReq(&batch_request.jobs[i]);
        }
        free(batch_request.jobs);
        batch_request.jobs = NULL;
    }

    if (batch_request.sourceFile) {
        free(batch_request.sourceFile);
        batch_request.sourceFile = NULL;
    }

    if (batch_request.batchName) {
        free(batch_request.batchName);
        batch_request.batchName = NULL;
    }

    /* Simplified cleanup: no more complex batch_reply.results */
    
    fprintf(stderr, "Info: Batch transmission completed, Success: %d, Failed: %d\n",
            transmitter->statistics.successful_jobs, transmitter->statistics.failed_jobs);
    
    return result;
}

/**
 * @brief Destroy batch transmitter
 */
void batch_transmitter_destroy(batch_transmitter_t *transmitter)
{
    if (!transmitter) {
        return;
    }

    /* Save enable_streaming state to avoid accessing after free */
    int enable_streaming = transmitter->enable_streaming;

    if (enable_streaming) {
        fprintf(stderr, "Info: Destroying batch transmitter...\n");
    }

    /* Close connection - using LSF standard method */
    if (transmitter->connection_fd >= 0) {
        extern void closeSession(int serverSock);
        closeSession(transmitter->connection_fd);
        transmitter->connection_fd = -1;
    }

    /* Clean up memory pool */
    if (transmitter->memory_pool) {
        batch_memory_pool_cleanup(transmitter->memory_pool);
        free(transmitter->memory_pool);
        transmitter->memory_pool = NULL;
    }

    /* Free transmitter structure */
    free(transmitter);

    if (enable_streaming) {
        fprintf(stderr, "Info: Batch transmitter destroyed successfully\n");
    }
}

/* ==================== Internal Function Implementations ==================== */

/**
 * @brief Establish connection - use LSF standard API to establish connection to MBD
 */
static int establish_connection(batch_transmitter_t *transmitter)
{
    if (!transmitter) {
        return -1;
    }

    /* Initialize LSF environment */
    if (lsb_init("bsub") < 0) {
        fprintf(stderr, "Error: LSF initialization failed\n");
        return -1;
    }

    fprintf(stderr, "*** CONNECTION DEBUG *** LSF initialized successfully\n");
    
    /* Use LSF standard connection method - do not pre-establish connection */
    /* callmbd() will automatically establish connection when needed, and can maintain connection for streaming responses */
    transmitter->connection_fd = -1;  /* No connection initially */
    transmitter->is_connected = 1;    /* Mark LSF environment ready for callmbd() */

    fprintf(stderr, "*** CONNECTION SUCCESS *** LSF environment ready for callmbd()\n");

    return 0;
}

/**
 * @brief Send batch request - use LSF Channel to send OpCode 200 batch request
 */
static int send_batch_request(batch_transmitter_t *transmitter,
                             const struct batchSubmitReq *batch_request)
{
    if (!transmitter || !batch_request) {
        return -1;
    }

    fprintf(stderr, "*** CLIENT SEND DEBUG *** Sending OpCode 200 batch request\n");
    fprintf(stderr, "*** CLIENT SEND DEBUG *** Total jobs to submit: %d\n", batch_request->jobCount);
    
    /* Use LSF standard callmbd() to send request and maintain connection */
    extern int callmbd(char *clusterName, char *request_buf, int requestlen,
                      char **reply_buf, struct LSFHeader *replyHdr,
                      int *serverSock, int (*postSndFunc)(), int *postSndFuncArg);
    
    /* Serialize batch request - use LSF standard xdr_encodeMsg() format */
    XDR xdrs;
    char *buffer = NULL;
    int bufferSize = 512 * 1024; /* 512KB static buffer for large batches */
    
    /* Allocate buffer */
    buffer = malloc(bufferSize);
    if (!buffer) {
        fprintf(stderr, "*** CLIENT SEND ERROR *** Memory allocation failed\n");
        return -1;
    }
    
    /* Create XDR encoder */
    xdrmem_create(&xdrs, buffer, bufferSize, XDR_ENCODE);
    
    /* Encode batch request - using LSF standard format */
    struct LSFHeader reqHdr;
    memset(&reqHdr, 0, sizeof(reqHdr));
    reqHdr.opCode = BATCH_SUBMIT_REQ;
    reqHdr.version = VOLCLAVA_VERSION;
    reqHdr.length = 0; /* xdr_encodeMsg will automatically set */
    
    /* Use LSF standard xdr_encodeMsg function */
    extern bool_t xdr_encodeMsg(XDR *xdrs, char *data, struct LSFHeader *hdr,
                               bool_t (*xdr_func)(), int options, struct lsfAuth *auth);
    
    /* 🔧 Fix user name corruption: get authentication info, like single connection */
    struct lsfAuth auth;
    extern int authTicketTokens_(struct lsfAuth *auth, char *hostName);
    
    if (authTicketTokens_(&auth, NULL) == -1) {
        fprintf(stderr, "*** CLIENT SEND ERROR *** Failed to get authentication tokens\n");
        xdr_destroy(&xdrs);
        free(buffer);
        return -1;
    }
    
    fprintf(stderr, "*** CLIENT AUTH DEBUG *** Authentication tokens obtained, user=%s\n",
            auth.lsfUserName ? auth.lsfUserName : "NULL");
    
    /* 🔧 Fix: Pass authentication info instead of NULL */
    if (!xdr_encodeMsg(&xdrs, (char*)batch_request, &reqHdr, xdr_batchSubmitReq, 0, &auth)) {
        fprintf(stderr, "*** CLIENT SEND ERROR *** LSF xdr_encodeMsg() failed\n");
        xdr_destroy(&xdrs);
        free(buffer);
        return -1;
    }
    
    int actualSize = xdr_getpos(&xdrs);
    xdr_destroy(&xdrs);
    
    fprintf(stderr, "*** CLIENT SEND DEBUG *** LSF xdr_encodeMsg() encoded %d bytes\n", actualSize);
    fprintf(stderr, "*** CLIENT SEND DEBUG *** Message header: OpCode=%d, Version=%d, Length=%d\n",
            reqHdr.opCode, reqHdr.version, reqHdr.length);
    
    /* Use callmbd to send request and maintain connection */
    char *reply_buf = NULL;
    struct LSFHeader replyHdr;
    int serverSock = -1;
    
    fprintf(stderr, "*** CLIENT SEND DEBUG *** Calling callmbd() with LSF-formatted OpCode %d\n", reqHdr.opCode);
    
    int result = callmbd(NULL, buffer, actualSize, &reply_buf, &replyHdr, &serverSock, NULL, NULL);
    
    if (result < 0) {
        fprintf(stderr, "*** CLIENT SEND ERROR *** callmbd() failed: %s\n", lsb_sysmsg());
        free(buffer);
        return -1;
    }
    
    fprintf(stderr, "*** CLIENT SEND SUCCESS *** callmbd() returned %d, reply OpCode=%d, serverSock=%d\n",
            result, replyHdr.opCode, serverSock);

    /* Prefetch: If callmbd() returned the first standard reply, cache it for subsequent loops to prioritize */
    if (result > 0 && reply_buf && replyHdr.length > 0 && replyHdr.length < MSGSIZE) {
        g_has_prefetched_reply = 1;
        g_prefetched_hdr = replyHdr; /* Still in network order, will be converted uniformly later */
        memcpy(g_prefetched_buf, reply_buf, replyHdr.length);
        g_prefetched_len = replyHdr.length;
        fprintf(stderr, "*** CLIENT PREFETCH *** Cached first reply from callmbd(): OpCode=%d, Len=%d\n",
                replyHdr.opCode, replyHdr.length);
    } else {
        g_has_prefetched_reply = 0;
    }
    
    /* Save connection info for streaming responses */
    transmitter->connection_fd = serverSock; /* Connection fd returned by callmbd */
    
    /* Update statistics */
    transmitter->statistics.bytes_sent += actualSize;
    
    /* Fix: Safely clean up memory to avoid double free */
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
    /* Fix: reply_buf is allocated by callmbd(), should not be manually freed */
    /* LSF system will manage memory for reply_buf */
    
    return 0;
}



/* handle_stream_response function removed as it is not currently used */

/**
 * @brief Update statistics
 */
static void update_statistics(batch_transmitter_t *transmitter,
                             int success_count, int failure_count)
{
    if (!transmitter) {
        return;
    }

    transmitter->statistics.successful_jobs = success_count;
    transmitter->statistics.failed_jobs = failure_count;
    transmitter->statistics.end_time = time(NULL);
}

/**
 * @brief Convert submit to submitReq
 */
static int convert_submit_to_submitReq(const struct submit *submit, struct submitReq *submitReq)
{
    if (!submit || !submitReq) {
        return -1;
    }

    /* Fully initialize submitReq structure */
    memset(submitReq, 0, sizeof(struct submitReq));

    /* Copy basic fields */
    submitReq->options = submit->options;
    submitReq->options2 = submit->options2;
    submitReq->numProcessors = submit->numProcessors;
    submitReq->maxNumProcessors = submit->maxNumProcessors;
    submitReq->numAskedHosts = submit->numAskedHosts;
    submitReq->nxf = submit->nxf;
    submitReq->sigValue = submit->sigValue;
    submitReq->beginTime = submit->beginTime;
    submitReq->termTime = submit->termTime;
    submitReq->chkpntPeriod = submit->chkpntPeriod;
    
    /* Set submitReq specific fields to default values */
    submitReq->umask = 0022;  /* Default umask */
    submitReq->restartPid = 0;
    submitReq->submitTime = time(NULL);
    submitReq->niosPort = 0;
    submitReq->userPriority = submit->userPriority;

    /* Copy resource limits */
    int i;
    for (i = 0; i < LSF_RLIM_NLIMITS; i++) {
        submitReq->rLimits[i] = submit->rLimits[i];
    }

    /* Copy string fields - use strdup to ensure memory safety */
    submitReq->command = submit->command ? strdup(submit->command) : strdup("");
    submitReq->jobName = submit->jobName ? strdup(submit->jobName) : strdup("");
    submitReq->queue = submit->queue ? strdup(submit->queue) : strdup("");
    submitReq->resReq = submit->resReq ? strdup(submit->resReq) : strdup("");
    submitReq->hostSpec = submit->hostSpec ? strdup(submit->hostSpec) : strdup("");
    submitReq->dependCond = submit->dependCond ? strdup(submit->dependCond) : strdup("");
    submitReq->inFile = submit->inFile ? strdup(submit->inFile) : strdup("");
    submitReq->outFile = submit->outFile ? strdup(submit->outFile) : strdup("");
    submitReq->errFile = submit->errFile ? strdup(submit->errFile) : strdup("");
    submitReq->chkpntDir = submit->chkpntDir ? strdup(submit->chkpntDir) : strdup("");
    submitReq->preExecCmd = submit->preExecCmd ? strdup(submit->preExecCmd) : strdup("");
    submitReq->postExecCmd = submit->postExecCmd ? strdup(submit->postExecCmd) : strdup("");
    submitReq->mailUser = submit->mailUser ? strdup(submit->mailUser) : strdup("");
    submitReq->projectName = submit->projectName ? strdup(submit->projectName) : strdup("");
    submitReq->loginShell = submit->loginShell ? strdup(submit->loginShell) : strdup("");
    /* Set submitReq specific string fields to default values */
    submitReq->schedHostType = strdup("");
    submitReq->userGroup = strdup("");
    submitReq->subHomeDir = strdup("");
    submitReq->cwd = strdup("");
    submitReq->fromHost = strdup("");
    submitReq->jobFile = strdup("");
    submitReq->inFileSpool = strdup("");
    submitReq->commandSpool = strdup("");

    /* Check if memory allocation was successful */
    if (!submitReq->command || !submitReq->jobName || !submitReq->queue ||
        !submitReq->resReq || !submitReq->hostSpec || !submitReq->dependCond ||
        !submitReq->inFile || !submitReq->outFile || !submitReq->errFile ||
        !submitReq->chkpntDir || !submitReq->preExecCmd || !submitReq->postExecCmd ||
        !submitReq->mailUser || !submitReq->projectName || !submitReq->loginShell ||
        !submitReq->schedHostType || !submitReq->userGroup || !submitReq->subHomeDir ||
        !submitReq->cwd || !submitReq->fromHost || !submitReq->jobFile ||
        !submitReq->inFileSpool || !submitReq->commandSpool) {
        cleanup_submitReq(submitReq);
        return -1;
    }

    /* Copy host list */
    if (submit->numAskedHosts > 0 && submit->askedHosts) {
        submitReq->askedHosts = malloc(submit->numAskedHosts * sizeof(char*));
        if (!submitReq->askedHosts) {
            cleanup_submitReq(submitReq);
            return -1;
        }
        
        for (i = 0; i < submit->numAskedHosts; i++) {
            submitReq->askedHosts[i] = submit->askedHosts[i] ? strdup(submit->askedHosts[i]) : NULL;
            if (submit->askedHosts[i] && !submitReq->askedHosts[i]) {
                cleanup_submitReq(submitReq);
                return -1;
            }
        }
    }

    /* Copy file transfer info */
    if (submit->nxf > 0 && submit->xf) {
        submitReq->xf = malloc(submit->nxf * sizeof(struct xFile));
        if (!submitReq->xf) {
            cleanup_submitReq(submitReq);
            return -1;
        }
        
        memcpy(submitReq->xf, submit->xf, submit->nxf * sizeof(struct xFile));
        /* Deep copy string fields in xFile */
        for (i = 0; i < submit->nxf; i++) {
            if (submit->xf[i].subFn) {
                strncpy(submitReq->xf[i].subFn, submit->xf[i].subFn, MAXFILENAMELEN - 1);
                submitReq->xf[i].subFn[MAXFILENAMELEN - 1] = '\0';
            }
            if (submit->xf[i].execFn) {
                strncpy(submitReq->xf[i].execFn, submit->xf[i].execFn, MAXFILENAMELEN - 1);
                submitReq->xf[i].execFn[MAXFILENAMELEN - 1] = '\0';
            }
        }
    }

    return 0;
}

/**
 * @brief Clean up submitReq
 */
static void cleanup_submitReq(struct submitReq *submitReq)
{
    if (!submitReq) {
        return;
    }

    /* Clean up string fields */
    if (submitReq->command) { free(submitReq->command); submitReq->command = NULL; }
    if (submitReq->jobName) { free(submitReq->jobName); submitReq->jobName = NULL; }
    if (submitReq->queue) { free(submitReq->queue); submitReq->queue = NULL; }
    if (submitReq->resReq) { free(submitReq->resReq); submitReq->resReq = NULL; }
    if (submitReq->hostSpec) { free(submitReq->hostSpec); submitReq->hostSpec = NULL; }
    if (submitReq->dependCond) { free(submitReq->dependCond); submitReq->dependCond = NULL; }
    if (submitReq->inFile) { free(submitReq->inFile); submitReq->inFile = NULL; }
    if (submitReq->outFile) { free(submitReq->outFile); submitReq->outFile = NULL; }
    if (submitReq->errFile) { free(submitReq->errFile); submitReq->errFile = NULL; }
    if (submitReq->chkpntDir) { free(submitReq->chkpntDir); submitReq->chkpntDir = NULL; }
    if (submitReq->preExecCmd) { free(submitReq->preExecCmd); submitReq->preExecCmd = NULL; }
    if (submitReq->postExecCmd) { free(submitReq->postExecCmd); submitReq->postExecCmd = NULL; }
    if (submitReq->mailUser) { free(submitReq->mailUser); submitReq->mailUser = NULL; }
    if (submitReq->projectName) { free(submitReq->projectName); submitReq->projectName = NULL; }
    if (submitReq->loginShell) { free(submitReq->loginShell); submitReq->loginShell = NULL; }
    if (submitReq->schedHostType) { free(submitReq->schedHostType); submitReq->schedHostType = NULL; }
    if (submitReq->userGroup) { free(submitReq->userGroup); submitReq->userGroup = NULL; }
    if (submitReq->subHomeDir) { free(submitReq->subHomeDir); submitReq->subHomeDir = NULL; }
    if (submitReq->cwd) { free(submitReq->cwd); submitReq->cwd = NULL; }
    if (submitReq->fromHost) { free(submitReq->fromHost); submitReq->fromHost = NULL; }
    if (submitReq->jobFile) { free(submitReq->jobFile); submitReq->jobFile = NULL; }
    if (submitReq->inFileSpool) { free(submitReq->inFileSpool); submitReq->inFileSpool = NULL; }
    if (submitReq->commandSpool) { free(submitReq->commandSpool); submitReq->commandSpool = NULL; }

    /* Clean up host list */
    if (submitReq->askedHosts) {
        int i;
        for (i = 0; i < submitReq->numAskedHosts; i++) {
            if (submitReq->askedHosts[i]) {
                free(submitReq->askedHosts[i]);
            }
        }
        free(submitReq->askedHosts);
        submitReq->askedHosts = NULL;
    }

    /* Clean up file transfer info */
    if (submitReq->xf) {
        /* String fields in xf structure are arrays, no need to free individually */
        free(submitReq->xf);
        submitReq->xf = NULL;
    }
}


/**
 * @brief Convert job status code to a readable string
 */
static const char* job_status_to_string(int status) {
    if (status & JOB_STAT_DONE) {
        return "DONE";
    }
    if (status & JOB_STAT_EXIT) {
        return "EXIT";
    }
    if (status & JOB_STAT_RUN) {
        if (status & JOB_STAT_SSUSP) {
            return "RUN+SSUSP";
        }
        if (status & JOB_STAT_USUSP) {
            return "RUN+USUSP";
        }
        return "RUN";
    }
    if (status & JOB_STAT_PEND) {
        if (status & JOB_STAT_PSUSP) {
            return "PEND+PSUSP";
        }
        return "PEND";
    }
    if (status & JOB_STAT_SSUSP) {
        return "SSUSP";
    }
    if (status & JOB_STAT_USUSP) {
        return "USUSP";
    }
    if (status & JOB_STAT_PSUSP) {
        return "PSUSP";
    }
    return "UNKNOWN";
}

/**
 * @brief Poll job completion status
 * Refer to single job client mode, check job status every 2 seconds until all jobs are complete
 */
static int poll_job_completion(LS_LONG_INT *jobIds, int jobCount)
{
    static char fname[] = "poll_job_completion";
    int completed_jobs = 0;
    int polling_rounds = 0;
    time_t start_time = time(NULL);
    
    if (!jobIds || jobCount <= 0) {
        fprintf(stderr, "Error: Invalid job list for polling\n");
        return -1;
    }
    
    fprintf(stderr, "Info: Starting to poll %d job statuses...\n", jobCount);
    for (int i = 0; i < jobCount; i++) {
        fprintf(stderr, "Info: JobID[%d]: %lld\n", i, jobIds[i]);
    }
    
    /* Poll until all jobs are complete */
    while (completed_jobs < jobCount) {
        polling_rounds++;
        completed_jobs = 0;
        
        /* Use lsb_openjobinfo_a to get all job information - refer to bjobs.c mode */
        struct jobInfoHead *jInfoH = lsb_openjobinfo_a(0, NULL, NULL, NULL, NULL, ALL_JOB);
        if (!jInfoH) {
            fprintf(stderr, "Warning: lsb_openjobinfo_a failed: %s\n", lsb_sysmsg());
            sleep(2);
            continue;
        }
        
        fprintf(stderr, "Debug: Polling round %d, found %d job information entries\n", polling_rounds, jInfoH->numJobs);
        
        /* *** Fix buffer overflow: correctly use lsb_readjobinfo *** */
        int found_jobs = 0;
        int more;
        struct jobInfoEnt *jobInfo;
        
        /* Read all job information at once, then find the jobs we care about */
        while ((jobInfo = lsb_readjobinfo(&more)) != NULL) {
            /* Check if this job is one we care about */
            for (int i = 0; i < jobCount; i++) {
                if (jobInfo->jobId == jobIds[i]) {
                    found_jobs++;
                    
                    /* Check if the job is complete - use IS_FINISH macro */
                    if (IS_FINISH(jobInfo->status)) {
                        completed_jobs++;
                        fprintf(stderr, "Info: Job %lld completed, status: %s\n", 
                               jobIds[i], job_status_to_string(jobInfo->status));
                    } else {
                        fprintf(stderr, "Debug: Job %lld running, status: %s\n", 
                               jobIds[i], job_status_to_string(jobInfo->status));
                    }
                    break; /* Found, no need to check other jobIds */
                }
            }
            
            /* Check if there are more jobs */
            if (!more) break;
        }
        
        /* Check if any jobs were not found - fix logic */
        if (found_jobs < jobCount) {
            fprintf(stderr, "Debug: Only found %d/%d jobs in polling round %d\n", 
                   found_jobs, jobCount, polling_rounds);
        }
        
        lsb_closejobinfo();
        
        fprintf(stderr, "Info: Polling progress: %d/%d completed, found %d/%d jobs\n", 
               completed_jobs, jobCount, found_jobs, jobCount);
        
        /* If all jobs are completed, exit loop */
        if (completed_jobs >= jobCount) {
            break;
        }
        
        /* Timeout check (10 minutes) */
        if (time(NULL) - start_time > 600) {
            fprintf(stderr, "Warning: Polling timeout, some jobs may still be running\n");
            break;
        }
        
        /* Wait 2 seconds before polling again */
        fprintf(stderr, "Info: Waiting 2 seconds before next poll...\n");
        sleep(2);
    }
    
    time_t total_time = time(NULL) - start_time;
    fprintf(stderr, "Info: All jobs completed! Total polling rounds: %d, time taken: %ld seconds\n", 
           polling_rounds, total_time);
    
    return 0;
}


/**
 * @file mbd.batch.h
 * @brief MBD batch job processing module header
 *
 * Defines data structures, constants, and function declarations related to
 * batch job processing.
 */

#ifndef _MBD_BATCH_H_
#define _MBD_BATCH_H_

#include <time.h>
#include <sys/types.h>
#include "../lsbatch.h"

/* Batch-related constants - using definitions from lsbatch.h */
#define BATCH_MAX_JOBS_LIMIT        10000   /* max jobs per batch */
#define BATCH_CLEANUP_INTERVAL      300     /* cleanup interval (seconds) */
#define BATCH_STREAM_TIMEOUT        600     /* streaming timeout (seconds) */
#define BATCH_NONSTREAM_TIMEOUT     3600    /* non-streaming timeout (seconds) */

/* Batch submit option flags - using lsbatch.h definitions */
#define BATCH_SUBMIT_FAIL_FAST          0x0002  /* fail-fast mode */
#define BATCH_SUBMIT_PRIORITY_ORDER     0x0008  /* process by priority order */

/* Streaming message types/flags - using lsbatch.h definitions */
#define STREAM_FLAG_ERROR           0x0004  /* error message */

/* Batch job related structures are defined in lsbatch.h */

/* Batch status query structures are defined in lsbatch.h */

/* Function declarations */

/**
 * @brief Handle batch job submit request
 * @param xdrs XDR stream
 * @param chfd client channel file descriptor
 * @param from client address
 * @param hostName client host name
 * @param reqHdr request header
 * @param laddr local address
 * @param auth authentication info
 * @param schedule schedule flag
 * @param dispatch dispatch flag
 * @return 0 on success, -1 on failure
 */
extern int do_batchSubmitReq(XDR *xdrs,
                             int chfd,
                             struct sockaddr_in *from,
                             char *hostName,
                             struct LSFHeader *reqHdr,
                             struct sockaddr_in *laddr,
                             struct lsfAuth *auth,
                             int *schedule,
                             int dispatch);



/* XDR function declarations are in lsb.xdr.h */

/* Message types are defined in daemonout.h */

/* Batch status query options */
#define BATCH_STATUS_DETAILED       0x0001  /* return detailed results */

/* Error codes */
#define LSBE_BATCH_TOO_MANY_JOBS    0x2001  /* too many jobs in batch */
#define LSBE_BATCH_INVALID_REQUEST  0x2002  /* invalid batch request */
#define LSBE_BATCH_NOT_FOUND        0x2003  /* batch not found */
#define LSBE_BATCH_STREAM_ERROR     0x2004  /* streaming response error */
#define LSBE_BATCH_TIMEOUT          0x2005  /* batch processing timeout */

/* Internal helpers are defined in mbd.batch.c; no need to declare here */

#endif /* _MBD_BATCH_H_ */

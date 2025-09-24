/**
 * @file batch_transmitter.h
 * @brief Batch job transmitter header
 */

#ifndef _BATCH_TRANSMITTER_H_
#define _BATCH_TRANSMITTER_H_

#include <time.h>
#include <stddef.h>

/* Forward declarations */
struct batchSubmitReply;
struct batch_memory_pool;
struct batch_job_collection;

/* Transmission statistics */
typedef struct {
    time_t start_time;          /* start time */
    time_t end_time;            /* end time */
    int total_jobs;             /* total jobs */
    int successful_jobs;        /* successful jobs */
    int failed_jobs;            /* failed jobs */
    size_t bytes_sent;          /* bytes sent */
    size_t bytes_received;      /* bytes received */
    int network_errors;         /* network error count */
    int retry_count;            /* retry count */
} transmit_statistics_t;

/* Progress callback type */
typedef void (*transmit_progress_callback_t)(int current, int total, const char *phase, void *user_data);

/* Batch transmitter structure */
struct batch_transmitter {
    int connection_fd;          /* connection file descriptor */
    int is_connected;           /* connection state */
    int enable_streaming;       /* enable streaming responses */
    int max_retry_count;        /* max retry count */
    int connection_timeout;     /* connection timeout (seconds) */
    int read_timeout;           /* read timeout (seconds) */
    int write_timeout;          /* write timeout (seconds) */

    transmit_statistics_t statistics;  /* statistics */
    transmit_progress_callback_t progress_callback;  /* progress callback */
    void *progress_user_data;   /* progress callback user data */

    struct batch_memory_pool *memory_pool;  /* memory pool */
    int is_complete;            /* whether transmission completed */
};

/* Type alias for backward compatibility */
typedef struct batch_transmitter batch_transmitter_t;

/* Transmission options */
typedef struct {
    int enable_streaming;       /* enable streaming responses */
    int max_retry_count;        /* max retry count */
    int connection_timeout;     /* connection timeout (seconds) */
    int read_timeout;           /* read timeout (seconds) */
    int write_timeout;          /* write timeout (seconds) */
    int enable_compression;     /* enable data compression */
    int batch_size_limit;       /* batch size limit */
    char *server_host;          /* server hostname */
    int server_port;            /* server port */
} transmit_options_t;

/* Transmission result */
typedef struct {
    int result_code;            /* result code: 0=success, <0=failure */
    int successful_jobs;        /* successful jobs */
    int failed_jobs;            /* failed jobs */
    double transmission_time;   /* transmission time (seconds) */
    char *error_message;        /* error message */
    struct batchSubmitReply *batch_reply;  /* batch reply */
} transmit_result_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create batch transmitter
 * @return transmitter pointer, or NULL on failure
 */
batch_transmitter_t* batch_transmitter_create(void);

/**
 * @brief Configure transmitter
 * @param transmitter transmitter pointer
 * @param enable_streaming enable streaming responses
 * @param max_retry_count maximum retry count
 * @param connection_timeout connection timeout (seconds)
 * @return 0 on success, -1 on failure
 */
int batch_transmitter_configure(batch_transmitter_t *transmitter,
                               int enable_streaming,
                               int max_retry_count,
                               int connection_timeout);


/**
 * @brief Submit a batch of jobs
 * @param transmitter transmitter pointer
 * @param collection job collection
 * @return number of successfully submitted jobs, or -1 on failure
 */
int batch_transmitter_submit_jobs(batch_transmitter_t *transmitter,
                                  const struct batch_job_collection *collection);




/**
 * @brief Destroy batch transmitter
 * @param transmitter transmitter pointer
 */
void batch_transmitter_destroy(batch_transmitter_t *transmitter);



/* Convenience macro definitions */
#define BATCH_TRANSMITTER_DEFAULT_RETRY_COUNT    3
#define BATCH_TRANSMITTER_DEFAULT_TIMEOUT        30
#define BATCH_TRANSMITTER_DEFAULT_READ_TIMEOUT   60
#define BATCH_TRANSMITTER_DEFAULT_WRITE_TIMEOUT  60
#define BATCH_TRANSMITTER_MAX_BATCH_SIZE         10000

/* Error codes */
#define TRANSMITTER_SUCCESS                0
#define TRANSMITTER_ERROR_INVALID_ARG     -1
#define TRANSMITTER_ERROR_NETWORK         -2
#define TRANSMITTER_ERROR_TIMEOUT         -3
#define TRANSMITTER_ERROR_MEMORY          -4
#define TRANSMITTER_ERROR_PROTOCOL        -5
#define TRANSMITTER_ERROR_SERVER          -6
#define TRANSMITTER_ERROR_CANCELLED       -7

/* Transmission status */
#define TRANSMITTER_STATUS_IDLE           0
#define TRANSMITTER_STATUS_CONNECTING     1
#define TRANSMITTER_STATUS_CONNECTED      2
#define TRANSMITTER_STATUS_TRANSMITTING   3
#define TRANSMITTER_STATUS_RECEIVING      4
#define TRANSMITTER_STATUS_COMPLETED      5
#define TRANSMITTER_STATUS_ERROR          6

#ifdef __cplusplus
}
#endif

#endif /* _BATCH_TRANSMITTER_H_ */

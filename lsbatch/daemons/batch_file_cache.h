/*
 * batch_file_cache.h - LSF batch local file cache header
 *
 * This module provides a local file cache mechanism for batch job submission,
 * avoiding mbdRcvJobFile() waiting on network file data and timing out.
 *
 * Core idea:
 * 1. Create a local temp file for each batch job
 * 2. Write the job command/script into the temp file in packed format
 * 3. Redirect the channel file descriptor so mbdRcvJobFile() reads the local file
 * 4. Keep full compatibility with the XDR protocol
 */

#ifndef _BATCH_FILE_CACHE_H_
#define _BATCH_FILE_CACHE_H_

#include <sys/types.h>
#include <unistd.h>

/* Batch file cache configuration */
#define BATCH_CACHE_MAX_JOBS    1000    /* max jobs per batch */
#define BATCH_CACHE_TEMP_DIR    "/tmp/lsf_batch_cache"  /* temp directory */
#define BATCH_CACHE_FILE_PREFIX "lsf_job_"              /* temp filename prefix */
#define BATCH_CACHE_MAX_PATH    256     /* max path length */

/* Error codes */
#define BATCH_CACHE_OK          0       /* success */
#define BATCH_CACHE_ERR_NOMEM   -1      /* out of memory */
#define BATCH_CACHE_ERR_IO      -2      /* I/O error */
#define BATCH_CACHE_ERR_PARAM   -3      /* invalid parameter */
#define BATCH_CACHE_ERR_LIMIT   -4      /* exceeded limit */

/* Per-job cache information */
struct batch_job_cache {
    int job_index;                      /* job index within the batch */
    char temp_file_path[BATCH_CACHE_MAX_PATH];  /* temp file path */
    int temp_fd;                        /* temp file descriptor */
    size_t file_size;                   /* file size */
    int original_fd;                    /* original fd (for restore) */
};

/* Batch cache context */
struct batch_cache_context {
    int job_count;                      /* total jobs */
    struct batch_job_cache *jobs;       /* job cache array */
    char cache_dir[BATCH_CACHE_MAX_PATH]; /* cache directory path */
    pid_t process_id;                   /* process id (for unique path) */
    time_t create_time;                 /* creation timestamp */
};

/* Function declarations */

/**
 * Create batch file cache context
 *
 * @param job_count number of jobs in the batch
 * @return context pointer on success, NULL on failure
 */
struct batch_cache_context* batch_cache_create(int job_count);

/**
 * Destroy batch file cache context
 *
 * @param ctx context pointer
 */
void batch_cache_destroy(struct batch_cache_context *ctx);

/**
 * Add a cache file for a specified job
 *
 * @param ctx context
 * @param job_index job index (0-based)
 * @param job_command job command string
 * @return BATCH_CACHE_OK on success, error code on failure
 */
int batch_cache_add_job(struct batch_cache_context *ctx, 
                        int job_index, 
                        const char *job_command);

/**
 * Redirect file descriptor to the cache file
 *
 * @param ctx context
 * @param job_index job index
 * @param target_fd target fd (usually the channel fd)
 * @return BATCH_CACHE_OK on success, error code on failure
 */
int batch_fd_redirect(struct batch_cache_context *ctx, 
                      int job_index, 
                      int target_fd);

/**
 * Restore file descriptor to original state
 *
 * @param ctx context
 * @param job_index job index
 * @param target_fd target fd
 * @return BATCH_CACHE_OK on success, error code on failure
 */
int batch_fd_restore(struct batch_cache_context *ctx, 
                     int job_index, 
                     int target_fd);

/**
 * Get job cache file path
 *
 * @param ctx context
 * @param job_index job index
 * @return file path on success, NULL on failure
 */
const char* batch_cache_get_file_path(struct batch_cache_context *ctx, 
                                      int job_index);

/**
 * Get job cache file size
 *
 * @param ctx context
 * @param job_index job index
 * @return size on success, -1 on failure
 */
ssize_t batch_cache_get_file_size(struct batch_cache_context *ctx, 
                                  int job_index);

/**
 * Cleanup all temporary files
 *
 * @param ctx context
 * @return BATCH_CACHE_OK on success, error code on failure
 */
int batch_cache_cleanup(struct batch_cache_context *ctx);

/**
 * Ensure cache directory exists; create if not
 *
 * @param dir_path directory path
 * @return BATCH_CACHE_OK on success, error code on failure
 */
int batch_cache_ensure_dir(const char *dir_path);

/**
 * Generate a unique temp file path
 *
 * @param ctx context
 * @param job_index job index
 * @param path_buffer path buffer
 * @param buffer_size buffer size
 * @return BATCH_CACHE_OK on success, error code on failure
 */
int batch_cache_generate_path(struct batch_cache_context *ctx,
                              int job_index,
                              char *path_buffer,
                              size_t buffer_size);

/* Debug and log macros */
#ifdef BATCH_CACHE_DEBUG
#define BATCH_LOG_DEBUG(fmt, ...) \
    fprintf(stderr, "[BATCH_CACHE_DEBUG] %s:%d " fmt "\n", \
            __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define BATCH_LOG_DEBUG(fmt, ...)
#endif

#define BATCH_LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[BATCH_CACHE_ERROR] %s:%d " fmt "\n", \
            __FILE__, __LINE__, ##__VA_ARGS__)

#define BATCH_LOG_INFO(fmt, ...) \
    fprintf(stderr, "[BATCH_CACHE_INFO] " fmt "\n", ##__VA_ARGS__)

#endif /* _BATCH_FILE_CACHE_H_ */
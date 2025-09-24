/**
 * @file batch_job_collector.h
 * @brief Batch job collector header
 */

#ifndef _BATCH_JOB_COLLECTOR_H_
#define _BATCH_JOB_COLLECTOR_H_

#include <time.h>
#include <limits.h>

/* Forward declarations */
struct submit;
struct batch_memory_pool;

/* Job collector configuration */
struct job_collector_config {
    int max_jobs_count;         /* maximum jobs */
    int max_memory_usage_mb;    /* max memory usage (MB) */
    int fail_on_first_error;    /* stop at first error */
    int collect_all_errors;     /* collect all error details */
    int enable_detailed_logging;/* enable detailed logging */
    char *debug_log_file;       /* debug log file */
};

/* Type alias for backward compatibility */
typedef struct job_collector_config job_collector_config_t;

/* Batch job collection */
struct batch_job_collection {
    struct submit **job_requests_array;    /* array of submit requests */
    int *job_line_numbers;                 /* line number per job */
    char **original_command_lines;         /* original command line per job */
    int total_jobs_count;                  /* total jobs */
    int total_jobs_capacity;               /* total capacity */
    int collect_original_commands;         /* whether to keep original commands */
    time_t collection_start_time;          /* collection start time */
    char source_file_path[PATH_MAX];       /* source file path */
    struct batch_memory_pool *memory_pool; /* memory pool */
};

/* Type alias for backward compatibility */
typedef struct batch_job_collection batch_job_collection_t;

/* Job collection statistics */
typedef struct {
    int total_lines_processed;     /* total lines processed */
    int valid_jobs_collected;      /* valid jobs collected */
    int empty_lines_skipped;       /* empty lines skipped */
    int comment_lines_skipped;     /* comment lines skipped */
    int parse_errors_encountered;  /* parse errors encountered */
    int validation_errors_encountered; /* validation errors encountered */
    double collection_time_seconds; /* collection time (seconds) */
    size_t memory_usage_bytes;     /* memory usage (bytes) */
} job_collection_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create default job collector config
 * @param config config struct pointer
 * @return 0 on success, -1 on failure
 */
int job_collector_create_default_config(struct job_collector_config *config);

/**
 * @brief Collect jobs from a pack file
 * @param pack_file_path pack file path
 * @param collection output job collection
 * @param config collector config, NULL to use defaults
 * @return 0 on success, -1 on failure
 */
int job_collector_collect_from_pack_file(const char *pack_file_path,
                                          struct batch_job_collection *collection,
                                          const struct job_collector_config *config);

/**
 * @brief Collect jobs from a string
 * @param pack_content pack content string
 * @param collection output job collection
 * @param config collector config, NULL to use defaults
 * @return 0 on success, -1 on failure
 */
int job_collector_collect_from_string(const char *pack_content,
                                       batch_job_collection_t *collection,
                                       const job_collector_config_t *config);

/**
 * @brief Validate job collection
 * @param collection job collection
 * @return 0 on success, -1 on failure
 */
int job_collector_validate_collection(const batch_job_collection_t *collection);

/**
 * @brief Get job collection stats
 * @param collection job collection
 * @param stats output stats
 * @return 0 on success, -1 on failure
 */
int job_collector_get_stats(const batch_job_collection_t *collection,
                            job_collection_stats_t *stats);

/**
 * @brief Print job collection summary
 * @param collection job collection
 * @param verbose verbose output flag
 */
void job_collector_print_summary(const batch_job_collection_t *collection, int verbose);

/**
 * @brief Convert job collection to submitReq array
 * @param collection job collection
 * @param submit_reqs output submitReq array pointer
 * @param count output array size
 * @return 0 on success, -1 on failure
 */
int job_collector_convert_to_submit_reqs(const batch_job_collection_t *collection,
                                          struct submitReq **submit_reqs,
                                          int *count);

/**
 * @brief Cleanup batch job collection
 * @param collection job collection
 */
void batch_job_collection_cleanup(struct batch_job_collection *collection);

/**
 * @brief Copy job collection
 * @param dest destination collection
 * @param src source collection
 * @return 0 on success, -1 on failure
 */
int job_collector_copy_collection(batch_job_collection_t *dest,
                                   const batch_job_collection_t *src);

/**
 * @brief Merge multiple job collections
 * @param dest destination collection
 * @param sources array of source collections
 * @param source_count number of sources
 * @return 0 on success, -1 on failure
 */
int job_collector_merge_collections(batch_job_collection_t *dest,
                                     const batch_job_collection_t *sources,
                                     int source_count);

/**
 * @brief Filter job collection
 * @param collection job collection
 * @param filter_func filter function returning 1 to keep, 0 to drop
 * @param user_data user data
 * @return number of kept jobs after filtering, -1 on failure
 */
int job_collector_filter_collection(batch_job_collection_t *collection,
                                     int (*filter_func)(const struct submit *, void *),
                                     void *user_data);

/**
 * @brief Sort job collection
 * @param collection job collection
 * @param compare_func comparator
 * @return 0 on success, -1 on failure
 */
int job_collector_sort_collection(batch_job_collection_t *collection,
                                   int (*compare_func)(const struct submit *, const struct submit *));

/* Convenience macros */
#define JOB_COLLECTOR_FOR_EACH(collection, index, job_req) \
    for ((index) = 0; \
         (index) < (collection)->total_jobs_count && \
         ((job_req) = (collection)->job_requests_array[index]); \
         (index)++)

#define JOB_COLLECTOR_IS_EMPTY(collection) \
    ((collection) == NULL || (collection)->total_jobs_count == 0)

#define JOB_COLLECTOR_GET_JOB(collection, index) \
    (((index) >= 0 && (index) < (collection)->total_jobs_count) ? \
     (collection)->job_requests_array[index] : NULL)

#define JOB_COLLECTOR_GET_LINE_NUMBER(collection, index) \
    (((index) >= 0 && (index) < (collection)->total_jobs_count) ? \
     (collection)->job_line_numbers[index] : -1)

#define JOB_COLLECTOR_GET_ORIGINAL_COMMAND(collection, index) \
    (((index) >= 0 && (index) < (collection)->total_jobs_count) ? \
     (collection)->original_command_lines[index] : NULL)

/* Error codes */
#define JOB_COLLECTOR_SUCCESS           0
#define JOB_COLLECTOR_ERROR_INVALID_ARG -1
#define JOB_COLLECTOR_ERROR_FILE_ACCESS -2
#define JOB_COLLECTOR_ERROR_MEMORY      -3
#define JOB_COLLECTOR_ERROR_PARSE       -4
#define JOB_COLLECTOR_ERROR_VALIDATION  -5
#define JOB_COLLECTOR_ERROR_LIMIT       -6

/* Constants */
#define JOB_COLLECTOR_MAX_JOBS_DEFAULT      10000
#define JOB_COLLECTOR_MAX_MEMORY_MB_DEFAULT 100
#define JOB_COLLECTOR_MAX_LINE_LENGTH       8192

#ifdef __cplusplus
}
#endif

#endif /* _BATCH_JOB_COLLECTOR_H_ */

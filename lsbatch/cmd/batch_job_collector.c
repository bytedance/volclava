/**
 * @file batch_job_collector.c
 * @brief Batch job collector implementation - enhanced version
 *
 * Responsibilities:
 * 1. Parse pack files and collect job requests
 * 2. Reuse existing LSF parameter parsing
 * 3. Enhanced error handling and memory management
 * 4. Support large files and large batches
 * 5. Detailed progress reporting
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

#include "cmd.h"
#include "../lsbatch.h"
#include "../lib/batch_memory.h"

/* Internal constants */
#define JOB_COLLECTOR_MAX_LINE_LENGTH    8192   /* increased per-line limit */
#define JOB_COLLECTOR_INITIAL_CAPACITY   100
#define JOB_COLLECTOR_GROWTH_FACTOR      2
#define JOB_COLLECTOR_PROGRESS_INTERVAL  100    /* report progress every 100 jobs */

/* Structures are defined in the header; no need to repeat here */

/* Internal function declarations */
static int parse_job_line(const char *line, int line_number, struct submit *job_req);
static int expand_job_capacity(struct batch_job_collection *collection, int new_capacity);
static int is_empty_or_comment_line(const char *line);
static void report_progress(int processed, int total, const char *phase);
static int validate_job_request(const struct submit *job_req, int line_number);
static char **split_command_line_enhanced(const char *cmdline, int *argc);
static void cleanup_job_request(struct submit *job_req);

/* External declarations - from existing LSF code */
extern int fillReq(int argc, char **argv, int operate, struct submit *req, int isInPackFile);
extern char **split_commandline(const char *cmdline, int *argc);
extern char *getNextLineC_(FILE *fp, int *lineNum, int flag);

/**
 * @brief Create default job collector config
 */
int job_collector_create_default_config(struct job_collector_config *config)
{
    if (!config) {
        return -1;
    }
    
    memset(config, 0, sizeof(job_collector_config_t));
    
    config->max_jobs_count = 10000;
    config->max_memory_usage_mb = 100;
    config->fail_on_first_error = 0;  /* do not stop on first error by default */
    config->collect_all_errors = 1;   /* collect all errors by default */
    config->enable_detailed_logging = 0;
    config->debug_log_file = NULL;
    
    return 0;
}

/**
 * @brief Collect jobs from pack file - enhanced
 */
int job_collector_collect_from_pack_file(const char *pack_file_path,
                                          struct batch_job_collection *collection,
                                          const struct job_collector_config *config)
{
    FILE *pack_file = NULL;
    char line_buffer[JOB_COLLECTOR_MAX_LINE_LENGTH];
    struct job_collector_config default_config;
    const struct job_collector_config *active_config = config;
    int line_number = 0;
    int valid_jobs_collected = 0;
    int result = -1;
    struct stat file_stat;

    /* Parameter validation */
    if (!pack_file_path || !collection) {
        fprintf(stderr, "Error: Invalid parameters for job collection\n");
        return -1;
    }

    /* Check file existence and readability */
    if (stat(pack_file_path, &file_stat) != 0) {
        fprintf(stderr, "Error: Cannot access file %s: %s\n", pack_file_path, strerror(errno));
        return -1;
    }

    /* Use default config if none provided */
    if (!active_config) {
        job_collector_create_default_config(&default_config);
        active_config = &default_config;
    }

    /* Initialize collection structure */
    memset(collection, 0, sizeof(batch_job_collection_t));
    collection->total_jobs_capacity = JOB_COLLECTOR_INITIAL_CAPACITY;
    collection->job_requests_array = calloc(collection->total_jobs_capacity, sizeof(struct submit*));
    collection->job_line_numbers = calloc(collection->total_jobs_capacity, sizeof(int));
    collection->original_command_lines = calloc(collection->total_jobs_capacity, sizeof(char*));
    
    if (!collection->job_requests_array || !collection->job_line_numbers || !collection->original_command_lines) {
        fprintf(stderr, "Error: Memory allocation failed for job collection arrays\n");
        goto cleanup;
    }

    collection->collect_original_commands = 1;
    collection->collection_start_time = time(NULL);
    strncpy(collection->source_file_path, pack_file_path, sizeof(collection->source_file_path) - 1);

    /* Initialize memory pool */
    collection->memory_pool = malloc(sizeof(struct batch_memory_pool));
    if (!collection->memory_pool ||
        batch_memory_pool_init(collection->memory_pool, active_config->max_memory_usage_mb * 1024 * 1024) != 0) {
        fprintf(stderr, "Error: Memory pool initialization failed\n");
        goto cleanup;
    }

    /* Open pack file */
    pack_file = fopen(pack_file_path, "r");
    if (!pack_file) {
        fprintf(stderr, "Error: Cannot open pack file: %s (%s)\n", pack_file_path, strerror(errno));
        goto cleanup;
    }

    if (active_config->enable_detailed_logging) {
        fprintf(stderr, "Info: Starting pack file parsing: %s\n", pack_file_path);
        fprintf(stderr, "Info: File size: %ld bytes\n", file_stat.st_size);
    }

    /* Parse jobs line-by-line */
    while (fgets(line_buffer, sizeof(line_buffer), pack_file)) {
        line_number++;
        
        /* Trim trailing newline */
        char *newline = strchr(line_buffer, '\n');
        if (newline) *newline = '\0';
        
        /* Skip empty/comment lines */
        if (is_empty_or_comment_line(line_buffer)) {
            continue;
        }
        
        /* Enforce job count limit */
        if (valid_jobs_collected >= active_config->max_jobs_count) {
            fprintf(stderr, "Warning: Reached maximum job count limit %d, stopping parsing\n", active_config->max_jobs_count);
            break;
        }
        
        /* Expand capacity if needed */
        if (valid_jobs_collected >= collection->total_jobs_capacity) {
            int new_capacity = collection->total_jobs_capacity * JOB_COLLECTOR_GROWTH_FACTOR;
            if (expand_job_capacity(collection, new_capacity) != 0) {
                fprintf(stderr, "Error: Failed to expand job capacity\n");
                if (active_config->fail_on_first_error) {
                    goto cleanup;
                }
                continue;
            }
        }
        
        /* Allocate new submit request */
        struct submit *job_req = batch_memory_pool_alloc(collection->memory_pool, sizeof(struct submit));
        if (!job_req) {
            fprintf(stderr, "Error: Line %d: Job request memory allocation failed\n", line_number);
            if (active_config->fail_on_first_error) {
                goto cleanup;
            }
            continue;
        }
        
        /* Parse job line */
        if (parse_job_line(line_buffer, line_number, job_req) != 0) {
            fprintf(stderr, "Error: Line %d: Job parsing failed: %s\n", line_number, line_buffer);
            if (active_config->fail_on_first_error) {
                goto cleanup;
            }
            continue;
        }
        
        /* Validate submit request */
        if (validate_job_request(job_req, line_number) != 0) {
            fprintf(stderr, "Error: Line %d: Job validation failed\n", line_number);
            cleanup_job_request(job_req);
            if (active_config->fail_on_first_error) {
                goto cleanup;
            }
            continue;
        }
        
        /* Add to collection */
        collection->job_requests_array[valid_jobs_collected] = job_req;
        collection->job_line_numbers[valid_jobs_collected] = line_number;
        
        /* Save original command line if requested */
        if (collection->collect_original_commands) {
            collection->original_command_lines[valid_jobs_collected] = 
                batch_memory_pool_alloc(collection->memory_pool, strlen(line_buffer) + 1);
            if (collection->original_command_lines[valid_jobs_collected]) {
                strcpy(collection->original_command_lines[valid_jobs_collected], line_buffer);
            }
        }
        
        valid_jobs_collected++;
        
        /* Progress report */
        if (valid_jobs_collected % JOB_COLLECTOR_PROGRESS_INTERVAL == 0) {
            report_progress(valid_jobs_collected, -1, "Parsing jobs");
        }
    }
    
    collection->total_jobs_count = valid_jobs_collected;
    
    /* Final stats */
    if (active_config->enable_detailed_logging) {
        fprintf(stderr, "Info: Job collection completed\n");
        fprintf(stderr, "Info: Total lines: %d\n", line_number);
        fprintf(stderr, "Info: Valid jobs: %d\n", valid_jobs_collected);
        fprintf(stderr, "Info: Duration: %ld seconds\n", time(NULL) - collection->collection_start_time);
    } else {
        fprintf(stderr, "Collected %d jobs from %d lines\n", valid_jobs_collected, line_number);
    }
    
    result = 0;

cleanup:
    if (pack_file) {
        fclose(pack_file);
    }
    
    if (result != 0 && collection) {
        /* Cleanup resources on failure - prevent leaks */
        if (collection->memory_pool) {
            batch_memory_pool_cleanup(collection->memory_pool);
            free(collection->memory_pool);
            collection->memory_pool = NULL;
        }
        
        if (collection->job_requests_array) {
            free(collection->job_requests_array);
            collection->job_requests_array = NULL;
        }
        
        if (collection->job_line_numbers) {
            free(collection->job_line_numbers);
            collection->job_line_numbers = NULL;
        }
        
        if (collection->original_command_lines) {
            /* Cleanup original command line pointers */
            int i;
            for (i = 0; i < valid_jobs_collected; i++) {
                if (collection->original_command_lines[i]) {
                    /* Note: strings are pool-allocated and freed with the pool */
                    /* free(collection->original_command_lines[i]); */
                }
            }
            free(collection->original_command_lines);
            collection->original_command_lines = NULL;
        }
        
        /* Reset counters */
        collection->total_jobs_count = 0;
        collection->total_jobs_capacity = 0;
    }
    
    return result;
}

/**
 * @brief Parse a single job command line
 */
static int parse_job_line(const char *line, int line_number, struct submit *job_req)
{
    char **argv = NULL;
    int argc = 0;
    int result = -1;
    char temp_buffer[JOB_COLLECTOR_MAX_LINE_LENGTH + 32];

    if (!line || !job_req) {
        return -1;
    }

    /* Build full bsub command line */
    snprintf(temp_buffer, sizeof(temp_buffer), "bsub %s", line);

    /* Split command line */
    argv = split_command_line_enhanced(temp_buffer, &argc);
    if (!argv || argc < 2) {
        fprintf(stderr, "Error: Line %d: Command line splitting failed\n", line_number);
        goto cleanup;
    }

    /* Reset optind for re-parsing */
    optind = 1;

    /* Use existing fillReq to parse arguments */
    if (fillReq(argc, argv, CMD_BSUB, job_req, 1) < 0) {
        fprintf(stderr, "Error: Line %d: Parameter parsing failed\n", line_number);
        goto cleanup;
    }

    /* Reject unsupported options */
    if (job_req->options2 & SUB2_BSUB_BLOCK) {
        fprintf(stderr, "Error: Line %d: -K option not supported in pack file\n", line_number);
        goto cleanup;
    }

    if (job_req->options & SUB_INTERACTIVE) {
        fprintf(stderr, "Error: Line %d: -I option not supported in pack file\n", line_number);
        goto cleanup;
    }

    if (job_req->options & SUB_PACK) {
        fprintf(stderr, "Error: Line %d: -pack option not supported in pack file\n", line_number);
        goto cleanup;
    }

    result = 0;

cleanup:
    if (argv) {
        int i;
        for (i = 0; i < argc; i++) {
            if (argv[i]) {
                free(argv[i]);
            }
        }
        free(argv);
    }

    return result;
}

/**
 * @brief Expand job capacity
 */
static int expand_job_capacity(struct batch_job_collection *collection, int new_capacity)
{
    if (!collection || new_capacity <= collection->total_jobs_capacity) {
        return -1;
    }

    /* Reallocate submit request array */
    struct submit **new_job_array = realloc(collection->job_requests_array,
                                           new_capacity * sizeof(struct submit*));
    if (!new_job_array) {
        return -1;
    }
    collection->job_requests_array = new_job_array;

    /* Reallocate line number array */
    int *new_line_numbers = realloc(collection->job_line_numbers,
                                   new_capacity * sizeof(int));
    if (!new_line_numbers) {
        return -1;
    }
    collection->job_line_numbers = new_line_numbers;

    /* Reallocate original command lines array */
    char **new_command_lines = realloc(collection->original_command_lines,
                                      new_capacity * sizeof(char*));
    if (!new_command_lines) {
        return -1;
    }
    collection->original_command_lines = new_command_lines;

    /* Initialize newly allocated region */
    memset(&collection->job_requests_array[collection->total_jobs_capacity], 0,
           (new_capacity - collection->total_jobs_capacity) * sizeof(struct submit*));
    memset(&collection->job_line_numbers[collection->total_jobs_capacity], 0,
           (new_capacity - collection->total_jobs_capacity) * sizeof(int));
    memset(&collection->original_command_lines[collection->total_jobs_capacity], 0,
           (new_capacity - collection->total_jobs_capacity) * sizeof(char*));

    collection->total_jobs_capacity = new_capacity;

    return 0;
}

/**
 * @brief Check if a line is empty or a comment
 */
static int is_empty_or_comment_line(const char *line)
{
    if (!line) {
        return 1;
    }

    /* Skip leading whitespace */
    while (isspace(*line)) {
        line++;
    }

    /* Empty or comment line */
    return (*line == '\0' || *line == '#');
}

/**
 * @brief Report progress
 */
static void report_progress(int processed, int total, const char *phase)
{
    if (total > 0) {
        double percentage = (double)processed / total * 100.0;
        fprintf(stderr, "🔄 %s: %d/%d (%.1f%%)\n", phase, processed, total, percentage);
    } else {
        fprintf(stderr, "🔄 %s: %d jobs\n", phase, processed);
    }
}

/**
 * @brief Validate job request
 */
static int validate_job_request(const struct submit *job_req, int line_number)
{
    if (!job_req) {
        return -1;
    }

    /* Check required command */
    if (!job_req->command || strlen(job_req->command) == 0) {
        fprintf(stderr, "Error: Line %d: Missing job command\n", line_number);
        return -1;
    }

    /* Validate resource limits */
    if (job_req->numProcessors < 0) {
        fprintf(stderr, "Error: Line %d: Number of processors cannot be negative\n", line_number);
        return -1;
    }

    /* Validate time constraints */
    if (job_req->beginTime > 0 && job_req->termTime > 0 &&
        job_req->beginTime >= job_req->termTime) {
        fprintf(stderr, "Error: Line %d: Begin time cannot be later than termination time\n", line_number);
        return -1;
    }

    return 0;
}

/**
 * @brief Enhanced command line split function
 */
static char **split_command_line_enhanced(const char *cmdline, int *argc)
{
    /* Try existing split_commandline first */
    char **result = split_commandline(cmdline, argc);

    if (result) {
        return result;
    }

    /* Fallback to a simple splitter if needed */
    if (!cmdline || !argc) {
        return NULL;
    }

    /* Simple whitespace-based split */
    char *cmd_copy = strdup(cmdline);
    if (!cmd_copy) {
        return NULL;
    }

    char **argv = malloc(64 * sizeof(char*));  /* up to 64 args */
    if (!argv) {
        free(cmd_copy);
        return NULL;
    }

    *argc = 0;
    char *token = strtok(cmd_copy, " \t");
    while (token && *argc < 63) {
        argv[*argc] = strdup(token);
        if (!argv[*argc]) {
            /* Cleanup allocated memory */
            int i;
            for (i = 0; i < *argc; i++) {
                free(argv[i]);
            }
            free(argv);
            free(cmd_copy);
            return NULL;
        }
        (*argc)++;
        token = strtok(NULL, " \t");
    }

    argv[*argc] = NULL;
    free(cmd_copy);

    return argv;
}

/**
 * @brief Cleanup job request
 */
static void cleanup_job_request(struct submit *job_req)
{
    if (!job_req) {
        return;
    }

    /* Place for request-specific cleanup if needed */
    /* Note: memory pool frees most allocations in bulk */
}

/**
 * @brief Cleanup batch job collection
 */
void batch_job_collection_cleanup(struct batch_job_collection *collection)
{
    if (!collection) {
        return;
    }

    if (collection->collect_original_commands) {
        fprintf(stderr, "Info: Cleaning up job collection...\n");
    }

    /* Cleanup memory pool */
    if (collection->memory_pool) {
        batch_memory_pool_cleanup(collection->memory_pool);
        free(collection->memory_pool);
        collection->memory_pool = NULL;
    }

    /* Cleanup arrays */
    if (collection->job_requests_array) {
        free(collection->job_requests_array);
        collection->job_requests_array = NULL;
    }

    if (collection->job_line_numbers) {
        free(collection->job_line_numbers);
        collection->job_line_numbers = NULL;
    }

    if (collection->original_command_lines) {
        free(collection->original_command_lines);
        collection->original_command_lines = NULL;
    }

    /* Reset counters */
    collection->total_jobs_count = 0;
    collection->total_jobs_capacity = 0;

    if (collection->collect_original_commands) {
        fprintf(stderr, "Info: Job collection cleanup completed\n");
    }
}

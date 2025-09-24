/*
 * batch_file_cache.c - LSF batch local file cache implementation
 *
 * Provides a local file cache to avoid mbdRcvJobFile() waiting for network
 * file data during batch submission, preventing timeouts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <dirent.h>
#include <stdint.h>
#include <arpa/inet.h>

#include "batch_file_cache.h"

/* Internal helper declarations */
static int create_temp_file(const char *file_path, const char *content);
static int safe_close_fd(int fd);
static void cleanup_job_cache(struct batch_job_cache *job);

/**
 * Create batch file cache context
 */
struct batch_cache_context* batch_cache_create(int job_count)
{
    struct batch_cache_context *ctx;
    
    BATCH_LOG_DEBUG("Creating batch cache context for %d jobs", job_count);
    
    /* Parameter validation */
    if (job_count <= 0 || job_count > BATCH_CACHE_MAX_JOBS) {
        BATCH_LOG_ERROR("Invalid job count: %d (max: %d)", 
                        job_count, BATCH_CACHE_MAX_JOBS);
        return NULL;
    }
    
    /* Allocate context */
    ctx = (struct batch_cache_context*)malloc(sizeof(struct batch_cache_context));
    if (!ctx) {
        BATCH_LOG_ERROR("Failed to allocate context memory: %s", strerror(errno));
        return NULL;
    }
    
    /* Allocate per-job cache array */
    ctx->jobs = (struct batch_job_cache*)calloc(job_count, sizeof(struct batch_job_cache));
    if (!ctx->jobs) {
        BATCH_LOG_ERROR("Failed to allocate jobs array: %s", strerror(errno));
        free(ctx);
        return NULL;
    }
    
    /* Initialize context */
    ctx->job_count = job_count;
    ctx->process_id = getpid();
    ctx->create_time = time(NULL);
    
    /* Build cache directory path */
    snprintf(ctx->cache_dir, sizeof(ctx->cache_dir), 
             "%s/lsf_batch_%d_%ld", 
             BATCH_CACHE_TEMP_DIR, ctx->process_id, ctx->create_time);
    
    /* Initialize job cache entries */
    for (int i = 0; i < job_count; i++) {
        ctx->jobs[i].job_index = i;
        ctx->jobs[i].temp_fd = -1;
        ctx->jobs[i].original_fd = -1;
        ctx->jobs[i].file_size = 0;
        memset(ctx->jobs[i].temp_file_path, 0, sizeof(ctx->jobs[i].temp_file_path));
    }
    
    /* Ensure cache directory exists */
    if (batch_cache_ensure_dir(ctx->cache_dir) != BATCH_CACHE_OK) {
        BATCH_LOG_ERROR("Failed to create cache directory: %s", ctx->cache_dir);
        batch_cache_destroy(ctx);
        return NULL;
    }
    
    BATCH_LOG_INFO("Created batch cache context: dir=%s, jobs=%d", 
                   ctx->cache_dir, job_count);
    
    return ctx;
}

/**
 * Destroy batch file cache context
 */
void batch_cache_destroy(struct batch_cache_context *ctx)
{
    if (!ctx) {
        return;
    }
    
    BATCH_LOG_DEBUG("Destroying batch cache context");
    
    /* Cleanup all job caches */
    if (ctx->jobs) {
        for (int i = 0; i < ctx->job_count; i++) {
            cleanup_job_cache(&ctx->jobs[i]);
        }
        free(ctx->jobs);
    }
    
    /* Cleanup temp files and directory */
    batch_cache_cleanup(ctx);
    
    /* Free context */
    free(ctx);
    
    BATCH_LOG_DEBUG("Batch cache context destroyed");
}

/**
 * Add a cache file for the specified job
 */
int batch_cache_add_job(struct batch_cache_context *ctx, 
                        int job_index, 
                        const char *job_command)
{
    struct batch_job_cache *job;
    int ret;
    
    BATCH_LOG_DEBUG("Adding job %d to cache", job_index);
    
    /* Parameter validation */
    if (!ctx || !job_command) {
        BATCH_LOG_ERROR("Invalid parameters: ctx=%p, job_command=%p", ctx, job_command);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    if (job_index < 0 || job_index >= ctx->job_count) {
        BATCH_LOG_ERROR("Invalid job index: %d (max: %d)", job_index, ctx->job_count - 1);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    job = &ctx->jobs[job_index];
    
    /* Generate temp file path */
    ret = batch_cache_generate_path(ctx, job_index, 
                                    job->temp_file_path, 
                                    sizeof(job->temp_file_path));
    if (ret != BATCH_CACHE_OK) {
        BATCH_LOG_ERROR("Failed to generate temp file path for job %d", job_index);
        return ret;
    }
    
    /* Create temp file and write packed job script */
    ret = create_temp_file(job->temp_file_path, job_command);
    if (ret != BATCH_CACHE_OK) {
        BATCH_LOG_ERROR("Failed to create temp file for job %d: %s", 
                        job_index, job->temp_file_path);
        return ret;
    }
    
    /* Record file size */
    job->file_size = strlen(job_command);
    
    BATCH_LOG_INFO("Added job %d to cache: file=%s, size=%zu", 
                   job_index, job->temp_file_path, job->file_size);
    
    return BATCH_CACHE_OK;
}

/**
 * Redirect file descriptor to the cache file
 */
int batch_fd_redirect(struct batch_cache_context *ctx, 
                      int job_index, 
                      int target_fd)
{
    struct batch_job_cache *job;
    int temp_fd;
    
    BATCH_LOG_DEBUG("Redirecting fd %d to job %d cache file", target_fd, job_index);
    
    /* Parameter validation */
    if (!ctx || target_fd < 0) {
        BATCH_LOG_ERROR("Invalid parameters: ctx=%p, target_fd=%d", ctx, target_fd);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    if (job_index < 0 || job_index >= ctx->job_count) {
        BATCH_LOG_ERROR("Invalid job index: %d", job_index);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    job = &ctx->jobs[job_index];
    
    /* Prevent duplicate redirection */
    if (job->original_fd >= 0) {
        BATCH_LOG_ERROR("Job %d fd already redirected (original_fd=%d)", job_index, job->original_fd);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    /* Check temp file existence */
    if (strlen(job->temp_file_path) == 0) {
        BATCH_LOG_ERROR("No temp file for job %d", job_index);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    /* Open temp file */
    temp_fd = open(job->temp_file_path, O_RDONLY);
    if (temp_fd < 0) {
        BATCH_LOG_ERROR("Failed to open temp file %s: %s", 
                        job->temp_file_path, strerror(errno));
        return BATCH_CACHE_ERR_IO;
    }
    
    /* Save original fd */
    job->original_fd = dup(target_fd);
    if (job->original_fd < 0) {
        BATCH_LOG_ERROR("Failed to duplicate original fd %d: %s", 
                        target_fd, strerror(errno));
        close(temp_fd);
        return BATCH_CACHE_ERR_IO;
    }
    
    /* Redirect fd */
    if (dup2(temp_fd, target_fd) < 0) {
        BATCH_LOG_ERROR("Failed to redirect fd %d to temp file: %s", 
                        target_fd, strerror(errno));
        close(temp_fd);
        close(job->original_fd);
        job->original_fd = -1;
        return BATCH_CACHE_ERR_IO;
    }
    
    /* Store temp fd */
    job->temp_fd = temp_fd;
    
    BATCH_LOG_INFO("Redirected fd %d to job %d cache file: %s", 
                   target_fd, job_index, job->temp_file_path);
    
    return BATCH_CACHE_OK;
}

/**
 * Restore file descriptor to original state
 */
int batch_fd_restore(struct batch_cache_context *ctx, 
                     int job_index, 
                     int target_fd)
{
    struct batch_job_cache *job;
    
    BATCH_LOG_DEBUG("Restoring fd %d for job %d", target_fd, job_index);
    
    /* Parameter validation */
    if (!ctx || target_fd < 0) {
        BATCH_LOG_ERROR("Invalid parameters: ctx=%p, target_fd=%d", ctx, target_fd);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    if (job_index < 0 || job_index >= ctx->job_count) {
        BATCH_LOG_ERROR("Invalid job index: %d", job_index);
        return BATCH_CACHE_ERR_PARAM;
    }
    
    job = &ctx->jobs[job_index];
    
    /* Check whether there is an original fd to restore */
    if (job->original_fd < 0) {
        BATCH_LOG_DEBUG("No original fd to restore for job %d", job_index);
        return BATCH_CACHE_OK;
    }
    
    /* Restore original fd */
    if (dup2(job->original_fd, target_fd) < 0) {
        BATCH_LOG_ERROR("Failed to restore fd %d: %s", target_fd, strerror(errno));
        return BATCH_CACHE_ERR_IO;
    }
    
    /* Close saved fds */
    safe_close_fd(job->original_fd);
    safe_close_fd(job->temp_fd);
    
    job->original_fd = -1;
    job->temp_fd = -1;
    
    BATCH_LOG_INFO("Restored fd %d for job %d", target_fd, job_index);
    
    return BATCH_CACHE_OK;
}

/**
 * Get job cache file path
 */
const char* batch_cache_get_file_path(struct batch_cache_context *ctx, 
                                      int job_index)
{
    if (!ctx || job_index < 0 || job_index >= ctx->job_count) {
        return NULL;
    }
    
    return ctx->jobs[job_index].temp_file_path;
}

/**
 * Get job cache file size
 */
ssize_t batch_cache_get_file_size(struct batch_cache_context *ctx, 
                                  int job_index)
{
    if (!ctx || job_index < 0 || job_index >= ctx->job_count) {
        return -1;
    }
    
    return ctx->jobs[job_index].file_size;
}

/**
 * Cleanup all temporary files
 */
int batch_cache_cleanup(struct batch_cache_context *ctx)
{
    DIR *dir;
    struct dirent *entry;
    char file_path[BATCH_CACHE_MAX_PATH];
    int cleanup_count = 0;
    
    if (!ctx) {
        return BATCH_CACHE_ERR_PARAM;
    }
    
    BATCH_LOG_DEBUG("Cleaning up cache directory: %s", ctx->cache_dir);
    
    /* Open cache directory */
    dir = opendir(ctx->cache_dir);
    if (!dir) {
        BATCH_LOG_DEBUG("Cache directory does not exist or cannot be opened: %s", 
                        ctx->cache_dir);
        return BATCH_CACHE_OK;
    }
    
    /* Remove all files in directory */
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(file_path, sizeof(file_path), "%s/%s", ctx->cache_dir, entry->d_name);
        
        if (unlink(file_path) == 0) {
            cleanup_count++;
            BATCH_LOG_DEBUG("Removed temp file: %s", file_path);
        } else {
            BATCH_LOG_ERROR("Failed to remove temp file %s: %s", 
                            file_path, strerror(errno));
        }
    }
    
    closedir(dir);
    
    /* Remove cache directory */
    if (rmdir(ctx->cache_dir) == 0) {
        BATCH_LOG_INFO("Removed cache directory: %s", ctx->cache_dir);
    } else {
        BATCH_LOG_ERROR("Failed to remove cache directory %s: %s", 
                        ctx->cache_dir, strerror(errno));
    }
    
    BATCH_LOG_INFO("Cleanup completed: removed %d files", cleanup_count);
    
    return BATCH_CACHE_OK;
}

/**
 * Ensure cache directory exists (create if missing)
 */
int batch_cache_ensure_dir(const char *dir_path)
{
    struct stat st;
    
    if (!dir_path) {
        return BATCH_CACHE_ERR_PARAM;
    }
    
    /* Check if directory exists */
    if (stat(dir_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return BATCH_CACHE_OK;
        } else {
            BATCH_LOG_ERROR("Path exists but is not a directory: %s", dir_path);
            return BATCH_CACHE_ERR_IO;
        }
    }
    
    /* Create parent directory */
    char parent_dir[BATCH_CACHE_MAX_PATH];
    strncpy(parent_dir, dir_path, sizeof(parent_dir) - 1);
    parent_dir[sizeof(parent_dir) - 1] = '\0';
    
    char *last_slash = strrchr(parent_dir, '/');
    if (last_slash && last_slash != parent_dir) {
        *last_slash = '\0';
        if (batch_cache_ensure_dir(parent_dir) != BATCH_CACHE_OK) {
            return BATCH_CACHE_ERR_IO;
        }
    }
    
    /* Create directory */
    if (mkdir(dir_path, 0755) != 0) {
        BATCH_LOG_ERROR("Failed to create directory %s: %s", dir_path, strerror(errno));
        return BATCH_CACHE_ERR_IO;
    }
    
    BATCH_LOG_DEBUG("Created directory: %s", dir_path);
    return BATCH_CACHE_OK;
}

/**
 * Generate a unique temp file path
 */
int batch_cache_generate_path(struct batch_cache_context *ctx,
                              int job_index,
                              char *path_buffer,
                              size_t buffer_size)
{
    if (!ctx || !path_buffer || buffer_size == 0) {
        return BATCH_CACHE_ERR_PARAM;
    }
    
    int ret = snprintf(path_buffer, buffer_size, 
                       "%s/%s%d_%ld.tmp", 
                       ctx->cache_dir, 
                       BATCH_CACHE_FILE_PREFIX, 
                       job_index, 
                       ctx->create_time);
    
    if (ret >= buffer_size) {
        BATCH_LOG_ERROR("Path buffer too small: need %d, have %zu", ret, buffer_size);
        return BATCH_CACHE_ERR_LIMIT;
    }
    
    return BATCH_CACHE_OK;
}

/* Internal helper function implementations */

/**
 * Create a temporary file and write packed job content
 */
static int create_temp_file(const char *file_path, const char *content)
{
    int fd;
    ssize_t written;
    char *job_file_data = NULL;
    int ret = BATCH_CACHE_OK;
    uint32_t network_length;
    size_t data_len;
    int size = 8192, length = 0;  // initial buffer size, similar to createJobInfoFile
    
    if (!file_path || !content) {
        return BATCH_CACHE_ERR_PARAM;
    }
    
    /* Mirror lsb.sub.c:createJobInfoFile() behavior */
    /* Refer to createJobInfoFile() lines 530-691 for full logic */
    
    /* 1) Compute required buffer size - similar to lines 530-535 */
    /* Note: USER is skipped per original createJobInfoFile() and not included */
    length = strlen("#! /bin/sh\n") +                    // SHELLLINE
             strlen("VOLCLAVA_VERSION='1'; export VOLCLAVA_VERSION\n") +
             strlen("PATH='/usr/bin:/bin'; export PATH\n") +
             strlen("HOME='/tmp'; export HOME\n") +
             /* USER环境变量已移除 - 根据原始createJobInfoFile()第585-587行逻辑 */
             strlen("SHELL='/bin/sh'; export SHELL\n") +
             strlen("$LSB_TRAPSIGS\n$LSB_RCP1\n$LSB_RCP2\n$LSB_RCP3\n") +  // TRAPSIGCMD
             strlen("# LSBATCH: User input\n") +
             strlen(content) +
             strlen("\nExitStat=$?\nwait\n# LSBATCH: End user input\ntrue\n") +  // WAITCLEANCMD
             strlen("exit `expr $? \"|\" $ExitStat`\n") +  // EXITCMD
             64;  // extra headroom
    
    /* 2) Allocate buffer - similar to lines 538-542 */
    size = (length > size) ? length : size;
    job_file_data = (char *)malloc(size);
    if (!job_file_data) {
        BATCH_LOG_ERROR("Failed to allocate memory for job file data");
        return BATCH_CACHE_ERR_PARAM;
    }
    
    /* 3) Build job file content - same format as createJobInfoFile */
    job_file_data[0] = '\0';
    
    /* 3.1 Add SHELLLINE - similar to line 544 */
    strcat(job_file_data, "#! /bin/sh\n");
    
    /* 3.2 Add VOLCLAVA_VERSION - similar to lines 555-559 */
    strcat(job_file_data, "VOLCLAVA_VERSION='1'; export VOLCLAVA_VERSION\n");
    
    /* 3.3 Add basic env vars - same format as lines 641-650 in createJobInfoFile */
    /* format: VAR='value'; export VAR\n */
    /* Note: must use the TAILCMD pattern "'; export " so readLogJobInfo() finds the marker */
    strcat(job_file_data, "PATH='");
    strcat(job_file_data, "/usr/bin:/bin");
    strcat(job_file_data, "'; export ");  // TAILCMD
    strcat(job_file_data, "PATH");
    strcat(job_file_data, "\n");
    
    strcat(job_file_data, "HOME='");
    strcat(job_file_data, "/tmp");
    strcat(job_file_data, "'; export ");  // TAILCMD
    strcat(job_file_data, "HOME");
    strcat(job_file_data, "\n");
    
    /* Per createJobInfoFile() lines 585-587, USER must be skipped.
     * The original code explicitly skips USER: !strncmp(*ep, "USER=", 5) { continue; }
     * Do not include USER in the job file to avoid username corruption issues.
     */
    
    strcat(job_file_data, "SHELL='");
    strcat(job_file_data, "/bin/sh");
    strcat(job_file_data, "'; export ");  // TAILCMD
    strcat(job_file_data, "SHELL");
    strcat(job_file_data, "\n");
    
    /* 3.4 Add TRAPSIGCMD - similar to line 680 */
    strcat(job_file_data, "$LSB_TRAPSIGS\n");
    strcat(job_file_data, "$LSB_RCP1\n");
    strcat(job_file_data, "$LSB_RCP2\n");
    strcat(job_file_data, "$LSB_RCP3\n");
    
    /* 3.5 Add user command - similar to lines 681-687 */
    strcat(job_file_data, "# LSBATCH: User input\n");
    strcat(job_file_data, content);
    
    /* 3.6 Add WAITCLEANCMD - similar to line 685 */
    strcat(job_file_data, "\nExitStat=$?\nwait\n# LSBATCH: End user input\ntrue\n");
    
    /* 3.7 Add EXITCMD - similar to line 686 */
    strcat(job_file_data, "exit `expr $? \"|\" $ExitStat`\n");
    
    data_len = strlen(job_file_data);
    
    /* 4) Write to file in network protocol format */
    fd = open(file_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        BATCH_LOG_ERROR("Failed to create temp file %s: %s", file_path, strerror(errno));
        free(job_file_data);
        return BATCH_CACHE_ERR_IO;
    }
    
    /* 4.1 Write big-endian length field */
    network_length = htonl((uint32_t)data_len);
    written = write(fd, &network_length, sizeof(network_length));
    if (written != sizeof(network_length)) {
        BATCH_LOG_ERROR("Failed to write length field to %s: written=%zd, expected=%zu",
                        file_path, written, sizeof(network_length));
        close(fd);
        unlink(file_path);
        free(job_file_data);
        return BATCH_CACHE_ERR_IO;
    }
    
    /* 4.2 Write the actual job file content */
    written = write(fd, job_file_data, data_len);
    if (written != data_len) {
        BATCH_LOG_ERROR("Failed to write job file data to %s: written=%zd, expected=%zu",
                        file_path, written, data_len);
        close(fd);
        unlink(file_path);
        free(job_file_data);
        return BATCH_CACHE_ERR_IO;
    }
    
    close(fd);
    free(job_file_data);
    
    BATCH_LOG_DEBUG("Created LSF job file (createJobInfoFile format): %s, data_size=%zu, total_size=%zu",
                    file_path, data_len, sizeof(network_length) + data_len);
    
    return BATCH_CACHE_OK;
}

/**
 * Safely close a file descriptor
 */
static int safe_close_fd(int fd)
{
    if (fd >= 0) {
        if (close(fd) != 0) {
            BATCH_LOG_ERROR("Failed to close fd %d: %s", fd, strerror(errno));
            return BATCH_CACHE_ERR_IO;
        }
    }
    return BATCH_CACHE_OK;
}

/**
 * Cleanup a single job cache
 */
static void cleanup_job_cache(struct batch_job_cache *job)
{
    if (!job) {
        return;
    }
    
    /* Close file descriptors */
    safe_close_fd(job->temp_fd);
    safe_close_fd(job->original_fd);
    
    /* Remove temp file */
    if (strlen(job->temp_file_path) > 0) {
        if (unlink(job->temp_file_path) != 0) {
            BATCH_LOG_ERROR("Failed to remove temp file %s: %s", 
                            job->temp_file_path, strerror(errno));
        }
    }
    
    /* Reset state */
    job->temp_fd = -1;
    job->original_fd = -1;
    job->file_size = 0;
    memset(job->temp_file_path, 0, sizeof(job->temp_file_path));
}
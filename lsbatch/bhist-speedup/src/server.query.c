/*
 * Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
 *
 * Server query subsystem with four processing layers:
 *  1. Parse bhist-compatible arguments and map time ranges to local-day tables.
 *  2. Validate the schema and load trusted table names from loader_event_tables.
 *  3. Materialize candidate base job IDs and read raw events in configured
 *     batches.
 *  4. Pass events to the formatter section below for parallel native bhist
 *     replay and formatting.
 *
 * Maintenance invariants:
 * - SQLite is a candidate prefilter, not a replacement for bhist semantics. All
 *   user-visible filters must also be forwarded to bhist workers.
 * - Completion-time queries replay candidates from all retained daily tables:
 *   an earlier DONE or EXIT can be followed by a requeue and later completion.
 * - Table names come only from the validated loader registry. User input may
 *   become SQL values but never SQL identifiers.
 * - Multi-batch queries prepare the next SQLite batch while current bhist
 *   workers run, but collect output in batch order.
 * - Every failure path rolls back open transactions, terminates or reaps active
 *   streams, and warns when already emitted output may be incomplete.
 */

#include "server.h"
#include "daemon.h"
#include "version.h"
#include "lproto.h"
#include "lib.table.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define BHIST_FLAG_A 0x0001u
#define BHIST_FLAG_D 0x0002u
#define BHIST_FLAG_E 0x0004u
#define BHIST_FLAG_P 0x0008u
#define BHIST_FLAG_S 0x0010u
#define BHIST_FLAG_R 0x0020u

#define BHIST_OUTPUT_LINE_SIZE 8192

/* Describes one native bhist invocation assembled by the query backend. */
struct bhistRequest {
    const char *bhistPath;
    const char *selector;
    int longFormat;
    int wideFormat;
    unsigned int stateFlags;
    const char *submitArg;
    const char *dispatchArg;
    const char *completeArg;
    const char *userName;
    const char *queueName;
    const char *hostName;
    int workers;
    long long expectedJobs;
};

/* Preserves output header and pending-line state while batches are merged. */
struct bhistOutputState {
    int printedDefaultHeader;
    int found;
    int hasPendingLine;
    char pendingLine[BHIST_OUTPUT_LINE_SIZE];
};

struct bhistStream;

static int bhistStreamOpen(
    const struct bhistRequest *request,
    struct bhistStream **streamOut);

static int bhistStreamAdd(struct bhistStream *stream,
                   long long jobId, const char *rawLine);

static int bhistStreamStartBatch(
    struct bhistStream *stream);

static int bhistStreamCollectBatch(
    struct bhistStream *stream,
    struct bhistOutputState *outputState,
    int *foundOut);

static int bhistStreamFinish(struct bhistStream *stream, int *foundOut);

static int bhistStreamFinishBatch(
    struct bhistStream *stream,
    struct bhistOutputState *outputState,
    int *foundOut);

static void bhistStreamAbort(struct bhistStream *stream);

static void bhistOutputInit(
    struct bhistOutputState *outputState);

static int bhistOutputFinish(
    struct bhistOutputState *outputState);

/* Maps base job IDs to worker buckets through Volclava's hash table. */
struct jobMap {
    hTab table;
    int initialized;
};

/* Job selector parsing and worker-bucket mapping. */
#define MAX_ARRAY_INDEX 0xffffULL
#define BASE_JOB_ID_MASK 0xffffffffULL

/*
 * Extract a base job ID from a bhist selector.
 * @param[in] selector: Ordinary or array-job selector.
 * @param[out] baseJobIdOut: Parsed base job ID.
 * @return: 0 on success, otherwise -1 for invalid input or overflow.
 */
static int
parseBaseJobId(const char *selector, long long *baseJobIdOut)
{
    const char *p;
    char *end;
    unsigned long long packed;
    unsigned long long base;
    unsigned long long index;

    if (selector == NULL || baseJobIdOut == NULL || selector[0] == '\0' ||
        !isdigit((unsigned char)selector[0]))
        return -1;

    errno = 0;
    packed = strtoull(selector, &end, 10);
    if (errno == ERANGE || end == selector || packed == 0 ||
        packed > (unsigned long long)LLONG_MAX)
        return -1;

    p = end;
    if (*p == '[') {
        const char *contents = ++p;

        /*
         * Validate only that an array selector is nonempty and closed. Native
         * bhist remains responsible for ranges, strides, and exact semantics.
         */
        while (*p != '\0' && *p != ']')
            p++;
        if (p == contents || *p != ']' || p[1] != '\0')
            return -1;
    } else if (*p != '\0') {
        return -1;
    }

    /* Volclava stores the base job ID in the low 32 bits. */
    base = packed & BASE_JOB_ID_MASK;
    index = packed >> 32;
    if (base == 0 || index > MAX_ARRAY_INDEX)
        return -1;

    *baseJobIdOut = (long long)base;
    return 0;
}

/*
 * Convert a base job ID to the stable text key used by Volclava's hash table.
 * @param[in] jobId: Base job ID.
 * @param[out] key: Fixed-size key buffer.
 * @return: 0 on success, otherwise -1 when formatting does not fit.
 */
static int
formatJobId(long long jobId, char key[32])
{
    int length;

    length = snprintf(key, 32, "%lld", jobId);
    return length > 0 && length < 32 ? 0 : -1;
}

/*
 * Initialize a job-ID-to-bucket map.
 * @param[out] map: Map to initialize.
 * @param[in] expectedJobs: Expected number of jobs.
 * @return: 0 on success, otherwise -1.
 */
static int
jobMapInit(struct jobMap *map, long long expectedJobs)
{
    long long requestedSlots;

    if (map == NULL)
        return -1;
    memset(map, 0, sizeof(*map));
    requestedSlots = expectedJobs > 0 ? expectedJobs : 1024;
    if (requestedSlots > INT_MAX)
        requestedSlots = INT_MAX;
    h_initTab_(&map->table, (int)requestedSlots);
    map->initialized = 1;
    return 0;
}

/*
 * Look up the worker bucket for a base job ID.
 * @param[in] map: Job map to search.
 * @param[in] jobId: Base job ID.
 * @param[out] bucketOut: Matching worker bucket.
 * @return: 1 when found, otherwise 0.
 */
static int
jobMapGet(const struct jobMap *map, long long jobId,
                int *bucketOut)
{
    char key[32];
    hEnt *entry;

    if (map == NULL || !map->initialized || bucketOut == NULL ||
        formatJobId(jobId, key) != 0)
        return 0;
    entry = h_getEnt_((hTab *)&map->table, key);
    if (entry == NULL || entry->hData == NULL)
        return 0;
    *bucketOut = *(const int *)entry->hData;
    return 1;
}

/*
 * Insert or update the worker bucket for a base job ID.
 * @param[in,out] map: Job map to update.
 * @param[in] jobId: Base job ID.
 * @param[in] bucket: Worker bucket number.
 * @return: 0 on success, otherwise -1.
 */
static int
jobMapPut(struct jobMap *map, long long jobId, int bucket)
{
    char key[32];
    hEnt *entry;
    int *storedBucket;
    int isNew;

    if (map == NULL || !map->initialized ||
        formatJobId(jobId, key) != 0)
        return -1;
    entry = h_addEnt_(&map->table, key, &isNew);
    if (entry == NULL)
        return -1;
    if (!isNew && entry->hData != NULL) {
        *(int *)entry->hData = bucket;
        return 0;
    }
    storedBucket = (int *)malloc(sizeof(*storedBucket));
    if (storedBucket == NULL) {
        if (isNew)
            h_delEnt_(&map->table, entry);
        return -1;
    }
    *storedBucket = bucket;
    free(entry->hData);
    entry->hData = storedBucket;
    return 0;
}

/*
 * Release a job-ID map.
 * @param[in,out] map: Map to release and reset.
 * @return: None.
 */
static void
jobMapDestroy(struct jobMap *map)
{
    if (map == NULL)
        return;
    if (map->initialized)
        h_freeTab_(&map->table, free);
    memset(map, 0, sizeof(*map));
}

/*
 * Adapter from raw SQLite events to native bhist-formatted output.
 *
 * Every base job ID belongs to one temporary event bucket so different bhist
 * workers never replay parts of the same job. Buckets are assigned by expected
 * base-job-ID count rather than by event row. A numeric job ID can be reused
 * after wraparound, so one ID may contain multiple job generations and the
 * formatter may emit more jobs than LSB_BHIST_JOBS_PER_BATCH. Each nonempty
 * bucket runs one "bhist -f <event-file>" process whose output is captured
 * and merged in a deterministic order.
 *
 * Output merging retains one pending line across workers and batches. Default
 * output suppresses repeated headers, while long output inserts exactly one
 * separator between nonempty results. Callers must preserve bhistOutputState
 * across batches.
 */

#define MAX_BHIST_WORKERS 128
#define BHIST_LONG_SEPARATOR \
    "------------------------------------------------------------------------------\n"

/* Owns temporary input/output files and child state for one bhist worker. */
struct bucketInfo {
    char eventPath[PATH_MAX];
    char outputPath[PATH_MAX];
    char errorPath[PATH_MAX];
    FILE *eventFp;
    long long jobs;
    long long lines;
    pid_t pid;
};

/*
 * Owns a complete batched bhist pipeline, including job-to-bucket assignment,
 * temporary files, worker processes, and cross-batch output state.
 */
struct bhistStream {
    struct bhistRequest request;
    struct bucketInfo *buckets;
    char workDir[PATH_MAX];
    struct jobMap jobToBucket;
    int mapInitialized;
    int bucketCount;
    int currentBucket;
    long long bucketCapacity;
    int found;
    int workersStarted;
};

static int makeWorkDir(char *path, size_t pathLen);
static void cleanupWorkDir(const char *workDir, struct bucketInfo *buckets,
                           int bucketCount);
static int initBuckets(const char *workDir, struct bucketInfo *buckets,
                       int bucketCount);
static void closeBucketFiles(struct bucketInfo *buckets, int bucketCount);
static int isJobNew(const char *rawLine);
static int bucketSqliteEvent(struct bhistStream *stream,
                             long long jobId, const char *rawLine);
static void appendBhistStateFlags(char **argv, int *argc,
                                  unsigned int flags);
static int startBhistWorkers(struct bucketInfo *buckets, int bucketCount,
                             const struct bhistRequest *request);
static int waitBhistWorkers(struct bucketInfo *buckets, int bucketCount);
static void cancelBhistWorkers(struct bucketInfo *buckets, int bucketCount);
static int bucketOutputHasNoMatch(const struct bucketInfo *bucket);
static int mergeBhistOutputs(struct bucketInfo *buckets, int bucketCount,
                             int longFormat,
                             struct bhistOutputState *outputState,
                             int *foundOut);
static int copyBhistOutput(FILE *fp, int longFormat,
                           struct bhistOutputState *outputState);
static int isDefaultHeaderLine(const char *line);

/*
 * Create a private temporary directory for one bhist stream.
 * @param[out] path: Buffer receiving the created directory path.
 * @param[in] pathLen: Size of path.
 * @return: 0 on success, otherwise -1.
 */
static int
makeWorkDir(char *path, size_t pathLen)
{
    if (snprintf(path, pathLen, "/tmp/bhist-speedup-XXXXXX") >=
        (int)pathLen) {
        fprintf(stderr, "bhist speed up: temporary path is too long\n");
        return -1;
    }
    if (mkdtemp(path) == NULL) {
        perror("mkdtemp");
        return -1;
    }
    return 0;
}

/*
 * Close and unlink every temporary artifact owned by a stream.
 * @param[in] workDir: Temporary directory path.
 * @param[in,out] buckets: Optional worker bucket array.
 * @param[in] bucketCount: Number of bucket entries.
 * @return: None.
 */
static void
cleanupWorkDir(const char *workDir, struct bucketInfo *buckets,
               int bucketCount)
{
    int i;

    if (buckets != NULL) {
        for (i = 0; i < bucketCount; i++) {
            if (buckets[i].eventFp != NULL) {
                fclose(buckets[i].eventFp);
                buckets[i].eventFp = NULL;
            }
            if (buckets[i].eventPath[0] != '\0')
                unlink(buckets[i].eventPath);
            if (buckets[i].outputPath[0] != '\0')
                unlink(buckets[i].outputPath);
            if (buckets[i].errorPath[0] != '\0')
                unlink(buckets[i].errorPath);
        }
    }
    if (workDir != NULL && workDir[0] != '\0')
        rmdir(workDir);
}

/*
 * Initialize temporary event, output, and error paths for all buckets.
 * @param[in] workDir: Existing private temporary directory.
 * @param[out] buckets: Worker bucket array to initialize.
 * @param[in] bucketCount: Number of bucket entries.
 * @return: 0 on success, otherwise -1.
 */
static int
initBuckets(const char *workDir, struct bucketInfo *buckets, int bucketCount)
{
    int i;

    for (i = 0; i < bucketCount; i++) {
        memset(&buckets[i], 0, sizeof(buckets[i]));
        if (snprintf(buckets[i].eventPath, sizeof(buckets[i].eventPath),
                     "%s/events.%d", workDir, i + 1) >=
            (int)sizeof(buckets[i].eventPath) ||
            snprintf(buckets[i].outputPath, sizeof(buckets[i].outputPath),
                     "%s/bhist.out.%d", workDir, i + 1) >=
            (int)sizeof(buckets[i].outputPath) ||
            snprintf(buckets[i].errorPath, sizeof(buckets[i].errorPath),
                     "%s/bhist.err.%d", workDir, i + 1) >=
            (int)sizeof(buckets[i].errorPath)) {
            fprintf(stderr, "bhist speed up: temporary path is too long\n");
            return -1;
        }
        buckets[i].eventFp = fopen(buckets[i].eventPath, "w");
        if (buckets[i].eventFp == NULL) {
            perror("fopen");
            return -1;
        }
    }

    return 0;
}

/*
 * Flush and close event input files before starting native bhist workers.
 * @param[in,out] buckets: Worker bucket array.
 * @param[in] bucketCount: Number of bucket entries.
 * @return: None.
 */
static void
closeBucketFiles(struct bucketInfo *buckets, int bucketCount)
{
    int i;

    for (i = 0; i < bucketCount; i++) {
        if (buckets[i].eventFp != NULL) {
            fclose(buckets[i].eventFp);
            buckets[i].eventFp = NULL;
        }
    }
}

/*
 * Test whether a raw scheduler event is a JOB_NEW record.
 * @param[in] rawLine: Raw event text.
 * @return: Nonzero for JOB_NEW, otherwise zero.
 */
static int
isJobNew(const char *rawLine)
{
    const char *p;

    p = rawLine;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (*p == '"')
        p++;
    if (strncmp(p, "JOB_NEW", strlen("JOB_NEW")) != 0)
        return 0;
    p += strlen("JOB_NEW");
    return *p == '\0' || *p == '"' || isspace((unsigned char)*p);
}

/*
 * Assign one raw event to the stable worker bucket for its base job ID.
 * @param[in,out] stream: Stream owning buckets and the job map.
 * @param[in] jobId: Base job ID for the event.
 * @param[in] rawLine: Raw scheduler event text.
 * @return: 0 when written or intentionally skipped, otherwise -1.
 */
static int
bucketSqliteEvent(struct bhistStream *stream,
                  long long jobId, const char *rawLine)
{
    int bucket;

    if (stream == NULL || rawLine == NULL)
        return -1;
    /*
     * Only JOB_NEW establishes the job-to-bucket mapping. Later events without
     * that prefix cannot be replayed with complete semantics and are skipped.
     * Once mapped, every later event keeps the SQLite row order in one bucket.
     */
    if (!jobMapGet(&stream->jobToBucket, jobId, &bucket)) {
        if (!isJobNew(rawLine))
            return 0;
        if (stream->buckets[stream->currentBucket].jobs >=
            stream->bucketCapacity &&
            stream->currentBucket < stream->bucketCount - 1)
            stream->currentBucket++;
        bucket = stream->currentBucket;
        if (jobMapPut(&stream->jobToBucket, jobId, bucket) != 0)
            return -1;
        /* Counts distinct base job IDs; wrapped generations share this bucket. */
        stream->buckets[bucket].jobs++;
    }
    if (fputs(rawLine, stream->buckets[bucket].eventFp) == EOF ||
        fputc('\n', stream->buckets[bucket].eventFp) == EOF) {
        perror("fwrite");
        return -1;
    }
    stream->buckets[bucket].lines++;
    stream->found = 1;
    return 0;
}

/*
 * Append requested native bhist state flags to a child argument vector.
 * @param[in,out] argv: Argument vector receiving flag strings.
 * @param[in,out] argc: Current argument count.
 * @param[in] flags: BHIST_FLAG_* bit mask.
 * @return: None.
 */
static void
appendBhistStateFlags(char **argv, int *argc, unsigned int flags)
{
    if ((flags & BHIST_FLAG_A) != 0)
        argv[(*argc)++] = "-a";
    if ((flags & BHIST_FLAG_D) != 0)
        argv[(*argc)++] = "-d";
    if ((flags & BHIST_FLAG_E) != 0)
        argv[(*argc)++] = "-e";
    if ((flags & BHIST_FLAG_P) != 0)
        argv[(*argc)++] = "-p";
    if ((flags & BHIST_FLAG_S) != 0)
        argv[(*argc)++] = "-s";
    if ((flags & BHIST_FLAG_R) != 0)
        argv[(*argc)++] = "-r";
}

/*
 * Fork native bhist processes for every nonempty worker bucket.
 * @param[in,out] buckets: Buckets receiving child PIDs.
 * @param[in] bucketCount: Number of bucket entries.
 * @param[in] request: Native bhist path, filters, and output format.
 * @return: 0 after all workers start, otherwise -1.
 */
static int
startBhistWorkers(struct bucketInfo *buckets, int bucketCount,
                  const struct bhistRequest *request)
{
    int i;

    for (i = 0; i < bucketCount; i++) {
        int outFd;
        int errFd;

        if (buckets[i].lines <= 0)
            continue;

        outFd = open(buckets[i].outputPath,
                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (outFd < 0) {
            perror("open");
            return -1;
        }
        errFd = open(buckets[i].errorPath,
                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (errFd < 0) {
            perror("open");
            close(outFd);
            return -1;
        }

        buckets[i].pid = fork();
        if (buckets[i].pid < 0) {
            perror("fork");
            close(outFd);
            close(errFd);
            return -1;
        }
        if (buckets[i].pid == 0) {
            /*
             * Each value-taking option below consumes two argv entries, and
             * the terminating NULL consumes one more. Increase this array or
             * introduce a bounds-checked builder before adding more filters.
             */
            char *argv[32];
            int argc;

            if (dup2(outFd, STDOUT_FILENO) < 0 ||
                dup2(errFd, STDERR_FILENO) < 0)
                _exit(127);
            close(outFd);
            close(errFd);

            argc = 0;
            argv[argc++] = (char *)request->bhistPath;
            argv[argc++] = "-f";
            argv[argc++] = buckets[i].eventPath;
            if (request->longFormat)
                argv[argc++] = "-l";
            if (request->wideFormat)
                argv[argc++] = "-w";
            appendBhistStateFlags(argv, &argc, request->stateFlags);
            if (request->selector != NULL) {
                argv[argc++] = (char *)request->selector;
            } else {
                /*
                 * Keep these arguments synchronized with SQLite prefilters.
                 * SQLite narrows base job IDs for speed; native bhist still
                 * receives the original filters and defines final semantics.
                 */
                if (request->userName != NULL) {
                    argv[argc++] = "-u";
                    argv[argc++] = (char *)request->userName;
                }
                if (request->queueName != NULL) {
                    argv[argc++] = "-q";
                    argv[argc++] = (char *)request->queueName;
                }
                if (request->hostName != NULL) {
                    argv[argc++] = "-m";
                    argv[argc++] = (char *)request->hostName;
                }
                if (request->submitArg != NULL) {
                    argv[argc++] = "-S";
                    argv[argc++] = (char *)request->submitArg;
                }
                if (request->dispatchArg != NULL) {
                    argv[argc++] = "-D";
                    argv[argc++] = (char *)request->dispatchArg;
                }
                if (request->completeArg != NULL) {
                    argv[argc++] = "-C";
                    argv[argc++] = (char *)request->completeArg;
                }
            }
            argv[argc] = NULL;
            execv(request->bhistPath, argv);
            perror(request->bhistPath);
            _exit(127);
        }
        close(outFd);
        close(errFd);
    }

    return 0;
}

/*
 * Wait for all native bhist workers and report meaningful child failures.
 * @param[in,out] buckets: Buckets whose child PIDs are reaped and cleared.
 * @param[in] bucketCount: Number of bucket entries.
 * @return: 0 when every worker succeeds or reports no match, otherwise -1.
 */
static int
waitBhistWorkers(struct bucketInfo *buckets, int bucketCount)
{
    int i;
    int rc;

    rc = 0;
    for (i = 0; i < bucketCount; i++) {
        int status;
        pid_t waited;

        if (buckets[i].pid <= 0)
            continue;
        do {
            waited = waitpid(buckets[i].pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        buckets[i].pid = 0;
        if (waited < 0) {
            perror("waitpid");
            rc = -1;
            continue;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            FILE *errFp;
            char line[512];

            if (bucketOutputHasNoMatch(&buckets[i])) {
                buckets[i].lines = 0;
                continue;
            }

            fprintf(stderr, "bhist speed up: bhist worker failed bucket=%d\n",
                    i + 1);
            errFp = fopen(buckets[i].errorPath, "r");
            if (errFp != NULL) {
                while (fgets(line, sizeof(line), errFp) != NULL)
                    fputs(line, stderr);
                fclose(errFp);
            }
            rc = -1;
        }
    }
    return rc;
}

/*
 * Terminate and reap all still-running native bhist workers.
 * @param[in,out] buckets: Worker buckets whose PIDs are cleared.
 * @param[in] bucketCount: Number of bucket entries.
 * @return: None.
 */
static void
cancelBhistWorkers(struct bucketInfo *buckets, int bucketCount)
{
    int i;

    if (buckets == NULL)
        return;
    for (i = 0; i < bucketCount; i++) {
        if (buckets[i].pid > 0 &&
            kill(buckets[i].pid, SIGTERM) != 0 && errno != ESRCH)
            perror("kill");
    }
    for (i = 0; i < bucketCount; i++) {
        pid_t waited;

        if (buckets[i].pid <= 0)
            continue;
        do {
            waited = waitpid(buckets[i].pid, NULL, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno != ECHILD)
            perror("waitpid");
        buckets[i].pid = 0;
    }
}

/*
 * Test whether a failed worker produced native bhist's no-match response.
 * @param[in] bucket: Worker bucket whose output file is inspected.
 * @return: Nonzero when the no-match response is present, otherwise zero.
 */
static int
bucketOutputHasNoMatch(const struct bucketInfo *bucket)
{
    FILE *fp;
    char line[8192];
    int found;

    if (bucket == NULL || bucket->outputPath[0] == '\0')
        return 0;
    fp = fopen(bucket->outputPath, "r");
    if (fp == NULL)
        return 0;
    found = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "No matching job found") != NULL) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/*
 * Identify a header line that native bhist repeats in default output.
 * @param[in] line: Native bhist output line.
 * @return: Nonzero for a repeatable default header, otherwise zero.
 */
static int
isDefaultHeaderLine(const char *line)
{
    const char *p;

    p = line;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "Summary of time in seconds spent",
                strlen("Summary of time in seconds spent")) == 0)
        return 1;
    if (strncmp(p, "JOBID", strlen("JOBID")) == 0)
        return 1;
    return 0;
}

/*
 * Copy one worker output while preserving cross-worker separator state.
 * @param[in] fp: Worker output stream.
 * @param[in] longFormat: Nonzero for bhist long output.
 * @param[in,out] outputState: Header and pending-line state.
 * @return: 1 when output was copied, 0 for empty output, otherwise -1.
 */
static int
copyBhistOutput(FILE *fp, int longFormat,
                struct bhistOutputState *outputState)
{
    char line[BHIST_OUTPUT_LINE_SIZE];
    char last[BHIST_OUTPUT_LINE_SIZE];
    int skippingRepeatedHeader;
    int hasLast;
    int wrote;

    if (fp == NULL || outputState == NULL)
        return -1;
    skippingRepeatedHeader = !longFormat &&
        outputState->printedDefaultHeader;
    hasLast = 0;
    wrote = 0;
    /*
     * Hold the last line until another nonempty output determines whether a
     * long-format separator is needed. bhistOutputFinish() flushes the final
     * pending line after the last batch.
     */
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (skippingRepeatedHeader && isDefaultHeaderLine(line))
            continue;
        skippingRepeatedHeader = 0;
        if (!hasLast && outputState->hasPendingLine) {
            if (longFormat && fputs(BHIST_LONG_SEPARATOR, stdout) == EOF) {
                perror("fputs");
                return -1;
            }
            outputState->hasPendingLine = 0;
        }
        if (hasLast && fputs(last, stdout) == EOF) {
            perror("fputs");
            return -1;
        }
        snprintf(last, sizeof(last), "%s", line);
        hasLast = 1;
        wrote = 1;
    }
    if (ferror(fp)) {
        perror("fgets");
        return -1;
    }
    if (hasLast) {
        snprintf(outputState->pendingLine,
                 sizeof(outputState->pendingLine), "%s", last);
        outputState->hasPendingLine = 1;
        outputState->found = 1;
        if (!longFormat)
            outputState->printedDefaultHeader = 1;
    }
    return wrote;
}

/*
 * Merge worker output files in deterministic bucket order.
 * @param[in] buckets: Completed worker buckets.
 * @param[in] bucketCount: Number of bucket entries.
 * @param[in] longFormat: Nonzero for bhist long output.
 * @param[in,out] outputState: Cross-worker and cross-batch output state.
 * @param[out] foundOut: Whether any worker produced output.
 * @return: 0 on success, otherwise -1.
 */
static int
mergeBhistOutputs(struct bucketInfo *buckets, int bucketCount,
                  int longFormat,
                  struct bhistOutputState *outputState,
                  int *foundOut)
{
    int i;

    *foundOut = 0;

    for (i = 0; i < bucketCount; i++) {
        FILE *fp;
        int copied;

        if (buckets[i].lines <= 0)
            continue;
        fp = fopen(buckets[i].outputPath, "r");
        if (fp == NULL) {
            perror("fopen");
            return -1;
        }
        copied = copyBhistOutput(fp, longFormat, outputState);
        fclose(fp);
        if (copied < 0)
            return -1;
        if (copied)
            *foundOut = 1;
    }

    return 0;
}

/*
 * Create a stream that buckets raw events and runs bhist workers.
 * @param[in] request: bhist path, arguments, worker count, and output format.
 * @param[out] streamOut: Newly created stream.
 * @return: 0 on success, otherwise -1.
 */
static int
bhistStreamOpen(const struct bhistRequest *request,
                            struct bhistStream **streamOut)
{
    struct bhistStream *stream;
    int bucketCount;

    if (request == NULL || request->bhistPath == NULL ||
        streamOut == NULL)
        return -1;
    *streamOut = NULL;

    bucketCount = request->workers > 0 ? request->workers : 1;
    if (request->expectedJobs > 0 && request->expectedJobs < bucketCount)
        bucketCount = (int)request->expectedJobs;
    if (bucketCount < 1)
        bucketCount = 1;
    if (bucketCount > MAX_BHIST_WORKERS)
        bucketCount = MAX_BHIST_WORKERS;

    stream = (struct bhistStream *)calloc(1, sizeof(*stream));
    if (stream == NULL) {
        fprintf(stderr, "bhist speed up: out of memory\n");
        return -1;
    }
    /* Request strings are borrowed until finish, collect, or abort consumes it. */
    stream->request = *request;
    stream->bucketCount = bucketCount;
    stream->bucketCapacity = request->expectedJobs > 0 ?
        (request->expectedJobs + bucketCount - 1) / bucketCount : 1;
    if (stream->bucketCapacity <= 0)
        stream->bucketCapacity = 1;
    stream->buckets = (struct bucketInfo *)calloc(
        (size_t)bucketCount, sizeof(struct bucketInfo));
    if (stream->buckets == NULL) {
        fprintf(stderr, "bhist speed up: out of memory\n");
        bhistStreamAbort(stream);
        return -1;
    }
    if (makeWorkDir(stream->workDir, sizeof(stream->workDir)) != 0 ||
        initBuckets(stream->workDir, stream->buckets, bucketCount) != 0 ||
        jobMapInit(&stream->jobToBucket,
                         request->expectedJobs) != 0) {
        bhistStreamAbort(stream);
        return -1;
    }
    stream->mapInitialized = 1;

    *streamOut = stream;
    return 0;
}

/*
 * Add one raw event to a bucketed stream.
 * @param[in,out] stream: Destination stream.
 * @param[in] jobId: Base job ID owning the event.
 * @param[in] rawLine: Original event text.
 * @return: 0 when added or safely skipped, otherwise -1.
 */
static int
bhistStreamAdd(struct bhistStream *stream,
                           long long jobId, const char *rawLine)
{
    return bucketSqliteEvent(stream, jobId, rawLine);
}

/*
 * Finish input for a batch and start its bhist workers.
 * @param[in,out] stream: Stream to start.
 * @return: 0 on success, otherwise -1.
 */
static int
bhistStreamStartBatch(struct bhistStream *stream)
{
    if (stream == NULL || stream->workersStarted)
        return -1;
    closeBucketFiles(stream->buckets, stream->bucketCount);
    stream->workersStarted = 1;
    if (!stream->found)
        return 0;
    return startBhistWorkers(stream->buckets, stream->bucketCount,
                             &stream->request);
}

/*
 * Wait for a started batch and merge its bhist output.
 * @param[in,out] stream: Started stream, consumed by this call.
 * @param[in,out] outputState: Output state shared across batches.
 * @param[out] foundOut: Whether this batch produced matching output.
 * @return: 0 on success, otherwise -1.
 */
static int
bhistStreamCollectBatch(
    struct bhistStream *stream,
    struct bhistOutputState *outputState,
    int *foundOut)
{
    int rc;
    int mergedFound;

    if (stream == NULL || outputState == NULL || foundOut == NULL ||
        !stream->workersStarted)
        return -1;
    *foundOut = 0;
    rc = -1;

    if (!stream->found) {
        rc = 0;
        goto done;
    }
    if (waitBhistWorkers(stream->buckets, stream->bucketCount) != 0)
        goto done;
    if (mergeBhistOutputs(stream->buckets, stream->bucketCount,
                          stream->request.longFormat, outputState,
                          &mergedFound) != 0)
        goto done;
    *foundOut = mergedFound;
    rc = 0;

done:
    bhistStreamAbort(stream);
    return rc;
}

/*
 * Complete and print one independent batch synchronously.
 * @param[in,out] stream: Stream to complete and consume.
 * @param[out] foundOut: Whether the batch produced matching output.
 * @return: 0 on success, otherwise -1.
 */
static int
bhistStreamFinish(struct bhistStream *stream,
                              int *foundOut)
{
    struct bhistOutputState outputState;
    int rc;

    bhistOutputInit(&outputState);
    rc = bhistStreamFinishBatch(stream, &outputState,
                                             foundOut);
    if (rc == 0 && bhistOutputFinish(&outputState) != 0)
        rc = -1;
    return rc;
}

/*
 * Initialize output state shared across batches.
 * @param[out] outputState: Output state to initialize.
 * @return: None.
 */
static void
bhistOutputInit(
    struct bhistOutputState *outputState)
{
    if (outputState != NULL)
        memset(outputState, 0, sizeof(*outputState));
}

/*
 * Finish cross-batch output and flush standard output.
 * @param[in,out] outputState: Output state to finish.
 * @return: 0 on success, otherwise -1.
 */
static int
bhistOutputFinish(
    struct bhistOutputState *outputState)
{
    if (outputState == NULL)
        return -1;
    if (outputState->hasPendingLine) {
        if (fputs(outputState->pendingLine, stdout) == EOF) {
            perror("fputs");
            return -1;
        }
        outputState->hasPendingLine = 0;
    }
    return fflush(stdout) == 0 ? 0 : -1;
}

/*
 * Complete one batch synchronously using shared output state.
 * @param[in,out] stream: Stream to complete and consume.
 * @param[in,out] outputState: Output state shared across batches.
 * @param[out] foundOut: Whether this batch produced matching output.
 * @return: 0 on success, otherwise -1.
 */
static int
bhistStreamFinishBatch(
    struct bhistStream *stream,
    struct bhistOutputState *outputState,
    int *foundOut)
{
    if (stream == NULL || outputState == NULL || foundOut == NULL)
        return -1;
    if (bhistStreamStartBatch(stream) != 0) {
        bhistStreamAbort(stream);
        return -1;
    }
    return bhistStreamCollectBatch(
        stream, outputState, foundOut);
}

/*
 * Terminate workers and release a stream.
 * @param[in,out] stream: Stream to release, or NULL.
 * @return: None.
 */
static void
bhistStreamAbort(struct bhistStream *stream)
{
    if (stream == NULL)
        return;
    cancelBhistWorkers(stream->buckets, stream->bucketCount);
    cleanupWorkDir(stream->workDir, stream->buckets, stream->bucketCount);
    if (stream->mapInitialized)
        jobMapDestroy(&stream->jobToBucket);
    free(stream->buckets);
    free(stream);
}

/* SQLite candidate selection and batched query orchestration. */
#define USER_NAME_MAX 256

#define NL_SETN 8
#define DEFAULT_SQLITE_BUSY_TIMEOUT_MS 60000
#define QUERY_SQL_BUFFER_SIZE (4 * 1024 * 1024)
#define QUERY_DAY_KEY_LEN 8
#define QUERY_DAY_KEY_SIZE (QUERY_DAY_KEY_LEN + 1)
#define QUERY_TABLE_NAME_SIZE 64
#define QUERY_SQLITE_CACHE_SIZE_KIB (-262144)
#define QUERY_SQLITE_MMAP_SIZE_BYTES 268435456LL
#define QUERY_DEFAULT_JOBS_PER_BATCH 10000
#define QUERY_MAX_FILTERS 8

#define QUERY_TEMP_FILTERED_JOBS "temp.filtered_jobs"
#define QUERY_TEMP_BATCH_JOBS "temp.batch_jobs"
#define QUERY_TEMP_SELECTED_ROWIDS "temp.selected_rowids"

/* Holds query-process tuning values received through the server environment. */
struct queryTuning {
    int formattersPerQuery;
    int jobsPerBatch;
    int sqliteBusyTimeoutMs;
    int sqliteCacheSizeKib;
    long long sqliteMmapSizeBytes;
};

/* Process-local tuning loaded once before a query starts SQLite work. */
static struct queryTuning querySettings;

/* Preserves one parsed bhist time option and its SQLite column mapping. */
struct timeFilter {
    int enabled;
    char opt;
    const char *arg;
    const char *column;
    int hasStart;
    int hasEnd;
    long long startEpoch;
    long long endEpoch;
};

/*
 * Query-filter extension guide
 * ----------------------------
 * Adding an option comparable to -u or -q normally requires every step below:
 *
 *  1. Parse it in runQuery() and update client usage and the man page. Update
 *     optionNeedsValue() and classifyRequestArgs() in server.main.c;
 *     otherwise a separate option value may be mistaken for a job selector or
 *     a new range query may be classified as a full-history scan.
 *  2. If the option narrows SQLite candidates, add a filterKind, its value
 *     structure, an add*Filter() helper, and registration in
 *     buildSqliteFilters(). Output-only bhist options do not belong in SQL.
 *  3. Add the predicate to appendFilterPredicate() for both unaliased set CTEs
 *     and aliased correlated EXISTS clauses. Classify it in exactly one of
 *     isJobIdSetFilter() or isExistsFilter(); metadata equality filters such as
 *     -q normally use EXISTS.
 *  4. Extend bhistRequest and the formatter argv builder so native bhist
 *     receives the original option. SQLite only narrows base job IDs; bhist
 *     remains authoritative for replay, formatting, and option semantics.
 *
 * A field not already stored in daily event tables also requires loader parsing,
 * schema and INSERT binding changes for every new table, a suitable
 * (value, job_id) index, a schema version increment, and a database rebuild.
 * Queries UNION multiple daily tables, so changing only the current table is
 * invalid. SQL identifiers must remain compile-time constants; only values may
 * originate from command-line input.
 *
 * Current filters operate on base job_id. An attribute that differs between
 * elements of one array job requires separate element-level storage and query
 * semantics rather than a simple peer of -q.
 *
 * QUERY_MAX_FILTERS is the maximum number of simultaneously enabled conditions,
 * not the number of enum members. Increase it when the worst-case option set
 * grows.
 */
/* Identifies each SQLite candidate-filter strategy. */
enum filterKind {
    FILTER_JOB_ID,
    FILTER_USER,
    FILTER_QUEUE,
    FILTER_SUBMIT_TIME,
    FILTER_DISPATCH_TIME,
    FILTER_COMPLETE_TIME
};

/* Stores normalized epoch bounds for one indexed event-time column. */
struct timeRange {
    const char *column;
    int hasStart;
    int hasEnd;
    long long startEpoch;
    long long endEpoch;
};

/* Stores the exact base job ID used by a job filter. */
struct jobFilter {
    long long jobId;
};

/* Stores the submitting user used by a metadata filter. */
struct userFilter {
    const char *userName;
};

/* Stores a borrowed string used by a generic equality filter. */
struct stringFilter {
    const char *value;
};

/* Describes one typed candidate filter and its SQL execution strategy. */
struct queryFilter {
    enum filterKind kind;
    const char *cteName;
    int postCandidate;
    union {
        struct timeRange time;
        struct jobFilter job;
        struct userFilter user;
        struct stringFilter string;
    } data;
};

/* Pairs a trusted day key with its validated daily SQLite table name. */
struct eventTableRef {
    char dayKey[QUERY_DAY_KEY_SIZE];
    char tableName[QUERY_TABLE_NAME_SIZE];
};

/* Owns the ordered dynamic array of daily tables used by one query. */
struct eventTableList {
    struct eventTableRef *items;
    size_t count;
    size_t capacity;
};

/* Restricts trusted table discovery to optional inclusive day boundaries. */
struct eventTableRange {
    int hasStartDay;
    int hasEndDay;
    char startDay[QUERY_DAY_KEY_SIZE];
    char endDay[QUERY_DAY_KEY_SIZE];
};

typedef int (*sqliteRowCallback)(sqlite3_stmt *stmt, void *ctx);

static int invalidArguments(void);
static char *trimWhitespace(char *str);
static int loadQuerySqliteTuning(void);
static void reportMissingDatabase(const char *dbPath);
static int sqliteOpenRawDb(const char *dbPath, int busyTimeoutMs,
                           sqlite3 **dbOut);
static int sqliteOpenQueryDb(const char *dbPath, sqlite3 **dbOut);
static int sqliteSetQueryPragmas(sqlite3 *db);
static int sqliteHasReadySchema(sqlite3 *db);
static int sqliteExecRows(sqlite3 *db, const char *sql,
                          sqliteRowCallback callback, void *ctx,
                          long long *rowCountOut);
static int sqliteScalarLongLong(sqlite3 *db, const char *sql,
                                      long long *valueOut);
static int parseTimeRange(const char *value, struct timeFilter *filter);
static int formatDayKey(long long epoch, char *dayKey, size_t dayKeyLen);
static int buildCandidateEventTableRange(const struct timeFilter *submitFilter,
                                        const struct timeFilter *completeFilter,
                                        struct eventTableRange *range);
static int appendSql(char *sql, size_t sqlLen, size_t *used,
                     const char *fragment);
static int appendSqlStringLiteral(char *sql, size_t sqlLen, size_t *used,
                                  const char *value);
static int loadEventTables(sqlite3 *db,
                           const struct eventTableRange *range,
                           struct eventTableList *tables);
static void freeEventTables(struct eventTableList *tables);
static int buildJobIdQuery(const struct eventTableList *tables,
                           long long jobId, char *sql, size_t sqlLen);
static int buildCandidateJobsQuery(const struct timeFilter *submitFilter,
                                   const struct timeFilter *dispatchFilter,
                                   const struct timeFilter *completeFilter,
                                   const char *userName,
                                   const char *queueName,
                                   const struct eventTableList *tables,
                                   char *sql, size_t sqlLen);
static int buildBatchLoadSql(long long lastSeq, int batchSize,
                             char *sql, size_t sqlLen);
static int buildBatchRawQuery(const struct eventTableList *tables,
                              char *sql, size_t sqlLen);
static int appendBaseQueryCtesSql(char *sql, size_t sqlLen, size_t *used,
                                  const struct queryFilter *filters,
                                  int filterCount,
                                  const struct eventTableList *tables);
static void trimTrailingCteComma(char *sql, size_t *used);
static int appendAliasedTimeRangePredicate(char *sql, size_t sqlLen,
                                           size_t *used, const char *alias,
                                           const struct timeRange *time);
static int sqliteRunBhistQuery(sqlite3 *db, const char *sql,
                               const struct bhistRequest *request,
                               struct bhistOutputState *outputState,
                               int *foundOut);

/*
 * Report command-line arguments rejected by the server query parser.
 * @return: -1 for invalid arguments.
 */
static int
invalidArguments(void)
{
    fputs("bhist speed up: invalid query arguments; "
          "run bhist-speedup -h for usage\n", stderr);
    return -1;
}

/*
 * Remove leading and trailing whitespace from a mutable string.
 * @param[in,out] str: Mutable NUL-terminated string.
 * @return: Pointer to the first non-whitespace character.
 */
static char *
trimWhitespace(char *str)
{
    char *end;

    while (*str != '\0' && isspace((unsigned char)*str))
        str++;

    end = str + strlen(str);
    while (end > str && isspace((unsigned char)end[-1])) {
        end--;
        *end = '\0';
    }

    return str;
}

/*
 * Parse an optional nonnegative decimal integer environment value.
 * @param[in] name: Parameter name used in diagnostics.
 * @param[in] value: Optional decimal text; empty means zero.
 * @param[out] out: Parsed integer.
 * @return: 0 on success, otherwise -1.
 */
static int
parseNonNegativeInt(const char *name, const char *value, int *out)
{
    char *end;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        *out = 0;
        return 0;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < 0 || parsed > INT_MAX) {
        fprintf(stderr, "bhist speed up: invalid %s=%s\n", name, value);
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

/*
 * Load and normalize query-process SQLite and bhist tuning values.
 * @return: 0 on success, otherwise -1 for an invalid environment value.
 */
static int
loadQuerySqliteTuning(void)
{
    const char *value;
    int parsed;

    memset(&querySettings, 0, sizeof(querySettings));
    if (parseNonNegativeInt("LSB_BHIST_MAX_FORMATTERS_PER_QUERY",
                            getenv("LSB_BHIST_MAX_FORMATTERS_PER_QUERY"),
                            &querySettings.formattersPerQuery) != 0)
        return -1;
    if (parseNonNegativeInt("LSB_BHIST_JOBS_PER_BATCH",
                            getenv("LSB_BHIST_JOBS_PER_BATCH"),
                            &querySettings.jobsPerBatch) != 0)
        return -1;
    if (querySettings.formattersPerQuery <= 0)
        querySettings.formattersPerQuery = 1;
    if (querySettings.jobsPerBatch <= 0)
        querySettings.jobsPerBatch = QUERY_DEFAULT_JOBS_PER_BATCH;

    querySettings.sqliteBusyTimeoutMs =
        DEFAULT_SQLITE_BUSY_TIMEOUT_MS;
    value = getenv("LSB_BHIST_DB_BUSY_TIMEOUT");
    if (value != NULL && value[0] != '\0') {
        if (parseNonNegativeInt("LSB_BHIST_DB_BUSY_TIMEOUT",
                                value,
                                &querySettings.sqliteBusyTimeoutMs) != 0)
            return -1;
    }

    querySettings.sqliteCacheSizeKib = QUERY_SQLITE_CACHE_SIZE_KIB;
    value = getenv("LSB_BHIST_QUERY_DB_CACHE_SIZE");
    if (value != NULL && value[0] != '\0') {
        parsed = 0;
        if (parseNonNegativeInt(
                "LSB_BHIST_QUERY_DB_CACHE_SIZE", value,
                                &parsed) != 0 || parsed <= 0 ||
            parsed > INT_MAX / 1024) {
            fprintf(stderr, "bhist speed up: invalid %s=%s\n",
                    "LSB_BHIST_QUERY_DB_CACHE_SIZE", value);
            return -1;
        }
        querySettings.sqliteCacheSizeKib = -(parsed * 1024);
    }

    querySettings.sqliteMmapSizeBytes =
        QUERY_SQLITE_MMAP_SIZE_BYTES;
    value = getenv("LSB_BHIST_QUERY_DB_MMAP_SIZE");
    if (value != NULL && value[0] != '\0') {
        parsed = 0;
        if (parseNonNegativeInt(
                "LSB_BHIST_QUERY_DB_MMAP_SIZE", value,
                                &parsed) != 0) {
            fprintf(stderr, "bhist speed up: invalid %s=%s\n",
                    "LSB_BHIST_QUERY_DB_MMAP_SIZE", value);
            return -1;
        }
        querySettings.sqliteMmapSizeBytes =
            (long long)parsed * 1024LL * 1024LL;
    }
    return 0;
}

/*
 * Determine whether the loader PID file beside a database is locked.
 * @param[in] dbPath: Configured SQLite database path.
 * @return: 1 when loader runs, 0 when stopped, otherwise -1.
 */
static int
loaderStatusForDatabase(const char *dbPath)
{
    char error[512];
    char parent[PATH_MAX];
    char pidPath[PATH_MAX];
    pid_t pid;

    if (parentDirectory(dbPath, parent, sizeof(parent),
                             error, sizeof(error)) != 0)
        return -1;
    if (snprintf(pidPath, sizeof(pidPath), "%s/bhist-speedup-loader.pid",
                 parent) >= (int)sizeof(pidPath))
        return -1;
    return pidFileStatus(pidPath, &pid, error, sizeof(error));
}

/*
 * Explain a missing query database using the loader's current lock state.
 * @param[in] dbPath: Configured SQLite database path.
 * @return: None.
 */
static void
reportMissingDatabase(const char *dbPath)
{
    int loaderStatus;

    loaderStatus = loaderStatusForDatabase(dbPath);
    if (loaderStatus > 0) {
        fprintf(stderr,
                "bhist speed up: database is not available; loader import is "
                "not complete\n");
    } else if (loaderStatus == 0) {
        fprintf(stderr,
                "bhist speed up: database is not available and loader is not "
                "running; run bhist-speedup-loader start\n");
    } else {
        fprintf(stderr,
                "bhist speed up: database is not available and loader state "
                "cannot be determined\n");
    }
}

/*
 * Open an existing SQLite database read-only with a busy timeout.
 * @param[in] dbPath: SQLite database path.
 * @param[in] busyTimeoutMs: Busy timeout in milliseconds.
 * @param[out] dbOut: Open SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteOpenRawDb(const char *dbPath, int busyTimeoutMs, sqlite3 **dbOut)
{
    sqlite3 *db;
    int rc;

    if (dbPath == NULL || dbOut == NULL)
        return -1;
    *dbOut = NULL;
    if (access(dbPath, R_OK) < 0) {
        if (errno == ENOENT)
            reportMissingDatabase(dbPath);
        else
            fprintf(stderr, "bhist speed up: database is not readable: %s: %s\n",
                    dbPath, strerror(errno));
        return -1;
    }

    db = NULL;
    rc = sqlite3_open_v2(dbPath, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "bhist speed up: sqlite open failed: %s\n",
                db != NULL ? sqlite3_errmsg(db) : dbPath);
        if (db != NULL)
            sqlite3_close(db);
        return -1;
    }
    rc = sqlite3_busy_timeout(db, busyTimeoutMs);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "bhist speed up: sqlite busy_timeout failed: %s\n",
                sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    *dbOut = db;
    return 0;
}

/*
 * Apply query-only temp-store, mmap, and cache pragmas.
 * @param[in] db: Read-only SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteSetQueryPragmas(sqlite3 *db)
{
    char sql[256];
    char *error;
    int rc;

    if (db == NULL)
        return -1;
    snprintf(sql, sizeof(sql),
             "PRAGMA temp_store=MEMORY; "
             "PRAGMA mmap_size=%lld; "
             "PRAGMA cache_size=%d",
             querySettings.sqliteMmapSizeBytes,
             querySettings.sqliteCacheSizeKib);
    error = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "bhist speed up: sqlite pragma failed: %s\n",
                error != NULL ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return -1;
    }
    return 0;
}

/*
 * Verify the current schema version and absence of obsolete artifacts.
 * @param[in] db: Open SQLite connection.
 * @return: Nonzero for the ready schema, otherwise zero.
 */
static int
sqliteHasReadySchema(sqlite3 *db)
{
    long long ready;

    if (sqliteScalarLongLong(
            db,
            "SELECT CASE WHEN "
            "(SELECT count(*) FROM sqlite_master "
            "  WHERE type='table' AND name='loader_event_tables') = 1 "
            "AND (SELECT count(*) FROM sqlite_master "
            "       WHERE type='table' AND name='loader_progress') = 1 "
            "AND (SELECT count(*) FROM sqlite_master "
            "       WHERE type='table' AND name='job_' || 'generation_map') = 0 "
            "AND (SELECT count(*) FROM loader_meta "
            "       WHERE meta_key='schema_version' "
            "         AND meta_value='4') = 1 "
            "THEN 1 ELSE 0 END;",
            &ready) != 0)
        return false;
    return ready == 1;
}

/*
 * Open and validate the query database, then apply query pragmas.
 * @param[in] dbPath: SQLite database path.
 * @param[out] dbOut: Validated read-only connection.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteOpenQueryDb(const char *dbPath, sqlite3 **dbOut)
{
    sqlite3 *db;

    db = NULL;
    if (dbOut == NULL)
        return -1;
    *dbOut = NULL;
    if (sqliteOpenRawDb(dbPath,
                        querySettings.sqliteBusyTimeoutMs, &db) != 0 ||
        sqliteSetQueryPragmas(db) != 0) {
        if (db != NULL)
            sqlite3_close(db);
        return -1;
    }
    if (!sqliteHasReadySchema(db)) {
        fprintf(stderr,
                "bhist speed up: database schema is not ready or is obsolete; "
                "run bhist-speedup-loader rebuild\n");
        sqlite3_close(db);
        return -1;
    }
    *dbOut = db;
    return 0;
}

/*
 * Execute every statement in a SQL script and visit all result rows.
 * @param[in] db: SQLite connection.
 * @param[in] sql: One or more SQL statements.
 * @param[in] callback: Optional row callback.
 * @param[in,out] ctx: Opaque callback context.
 * @param[out] rowCountOut: Optional total result-row count.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteExecRows(sqlite3 *db, const char *sql, sqliteRowCallback callback,
               void *ctx, long long *rowCountOut)
{
    const char *tail;
    sqlite3_stmt *stmt;
    int rc;
    long long rows;

    if (db == NULL || sql == NULL)
        return -1;
    tail = sql;
    rows = 0;

    while (tail != NULL) {
        const char *start;

        while (*tail != '\0' &&
               (isspace((unsigned char)*tail) || *tail == ';'))
            tail++;
        if (*tail == '\0')
            break;

        start = tail;
        stmt = NULL;
        rc = sqlite3_prepare_v2(db, tail, -1, &stmt, &tail);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "bhist speed up: sqlite prepare failed: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
        if (stmt == NULL) {
            if (tail == start)
                break;
            continue;
        }

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            rows++;
            if (callback != NULL && callback(stmt, ctx) != 0) {
                sqlite3_finalize(stmt);
                return -1;
            }
        }
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "bhist speed up: sqlite step failed: %s\n",
                    sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return -1;
        }
        rc = sqlite3_finalize(stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "bhist speed up: sqlite finalize failed: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
    }

    if (rowCountOut != NULL)
        *rowCountOut = rows;
    return 0;
}

/* Captures the first non-NULL int64 result returned by a scalar query. */
struct sqliteScalarLongLongCtx {
    int seen;
    long long value;
};

/*
 * Capture the first column of the first scalar query row.
 * @param[in] stmt: SQLite statement positioned on a result row.
 * @param[in,out] ctx: sqliteScalarLongLongCtx receiving the value.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteScalarLongLongRow(sqlite3_stmt *stmt, void *ctx)
{
    struct sqliteScalarLongLongCtx *scalar;

    scalar = (struct sqliteScalarLongLongCtx *)ctx;
    if (scalar == NULL || sqlite3_column_count(stmt) < 1)
        return -1;
    if (!scalar->seen) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL)
            return -1;
        scalar->value = sqlite3_column_int64(stmt, 0);
        scalar->seen = 1;
    }
    return 0;
}

/*
 * Execute SQL that must return one non-NULL int64 scalar.
 * @param[in] db: SQLite connection.
 * @param[in] sql: Scalar SQL statement.
 * @param[out] valueOut: Captured integer value.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteScalarLongLong(sqlite3 *db, const char *sql, long long *valueOut)
{
    struct sqliteScalarLongLongCtx scalar;

    if (valueOut == NULL)
        return -1;
    memset(&scalar, 0, sizeof(scalar));
    if (sqliteExecRows(db, sql, sqliteScalarLongLongRow, &scalar, NULL) != 0 ||
        !scalar.seen)
        return -1;
    *valueOut = scalar.value;
    return 0;
}

/*
 * Parse one bhist date/time endpoint into local Unix time.
 * @param[in] value: bhist-compatible time text.
 * @param[out] epochOut: Parsed Unix timestamp.
 * @return: 0 on success, otherwise -1.
 */
static int
parseTimePoint(const char *value, long long *epochOut)
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    char tail;
    struct tm tmValue;
    time_t epoch;

    if (value == NULL || value[0] == '\0')
        return -1;
    if (sscanf(value, "%d/%d/%d/%d:%d%c",
               &year, &month, &day, &hour, &minute, &tail) != 5)
        return -1;
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return -1;

    memset(&tmValue, 0, sizeof(tmValue));
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_isdst = -1;
    epoch = mktime(&tmValue);
    if (epoch == (time_t)-1)
        return -1;

    *epochOut = (long long)epoch;
    return 0;
}

/*
 * Parse a comma-separated bhist time range into optional epoch bounds.
 * @param[in] value: Range text supplied to -S, -D, or -C.
 * @param[in,out] filter: Filter receiving normalized bounds.
 * @return: 0 on success, otherwise -1.
 */
static int
parseTimeRange(const char *value, struct timeFilter *filter)
{
    char buffer[128];
    char *comma;
    char *start;
    char *end;

    if (value == NULL || filter == NULL)
        return -1;
    if (strlen(value) >= sizeof(buffer))
        return -1;
    snprintf(buffer, sizeof(buffer), "%s", value);

    comma = strchr(buffer, ',');
    if (comma != NULL) {
        *comma = '\0';
        start = trimWhitespace(buffer);
        end = trimWhitespace(comma + 1);
    } else {
        start = trimWhitespace(buffer);
        end = "";
    }

    filter->hasStart = 0;
    filter->hasEnd = 0;
    if (start[0] != '\0') {
        if (parseTimePoint(start, &filter->startEpoch) != 0)
            return -1;
        filter->hasStart = 1;
    }
    if (end[0] != '\0') {
        if (parseTimePoint(end, &filter->endEpoch) != 0)
            return -1;
        filter->hasEnd = 1;
    }
    if (!filter->hasStart && !filter->hasEnd)
        return -1;
    if (filter->hasStart && filter->hasEnd &&
        filter->startEpoch > filter->endEpoch)
        return -1;

    return 0;
}

/*
 * Format a Unix timestamp as a local-time YYYYMMDD table key.
 * @param[in] epoch: Unix timestamp.
 * @param[out] dayKey: Buffer receiving the day key.
 * @param[in] dayKeyLen: Size of dayKey.
 * @return: 0 on success, otherwise -1.
 */
static int
formatDayKey(long long epoch, char *dayKey, size_t dayKeyLen)
{
    time_t timestamp;
    struct tm tmValue;

    if (dayKey == NULL || dayKeyLen < QUERY_DAY_KEY_SIZE)
        return -1;
    timestamp = (time_t)epoch;
    if (localtime_r(&timestamp, &tmValue) == NULL)
        return -1;
    if (strftime(dayKey, dayKeyLen, "%Y%m%d", &tmValue) != QUERY_DAY_KEY_LEN)
        return -1;
    return 0;
}

/*
 * Derive candidate-table discovery bounds from submit and complete filters.
 * @param[in] submitFilter: Optional submission-time filter.
 * @param[in] completeFilter: Optional completion-time filter.
 * @param[out] range: Inclusive table-day bounds.
 * @return: 0 on success, otherwise -1.
 */
static int
buildCandidateEventTableRange(const struct timeFilter *submitFilter,
                             const struct timeFilter *completeFilter,
                             struct eventTableRange *range)
{
    if (range == NULL)
        return -1;
    memset(range, 0, sizeof(*range));

    if (submitFilter != NULL && submitFilter->enabled &&
        submitFilter->hasStart) {
        if (formatDayKey(submitFilter->startEpoch, range->startDay,
                         sizeof(range->startDay)) != 0)
            return -1;
        range->hasStartDay = true;
    }
    if (completeFilter != NULL && completeFilter->enabled &&
        completeFilter->hasEnd) {
        if (formatDayKey(completeFilter->endEpoch, range->endDay,
                         sizeof(range->endDay)) != 0)
            return -1;
        range->hasEndDay = true;
    }
    return 0;
}

/*
 * Validate an eight-digit YYYYMMDD registry key.
 * @param[in] value: Candidate day key.
 * @return: Nonzero when valid, otherwise zero.
 */
static int
isValidDayKey(const char *value)
{
    size_t i;

    if (value == NULL || strlen(value) != QUERY_DAY_KEY_LEN)
        return false;
    for (i = 0; i < QUERY_DAY_KEY_LEN; i++) {
        if (!isdigit((unsigned char)value[i]))
            return false;
    }
    return true;
}

/*
 * Verify that a registry table name exactly matches its trusted day key.
 * @param[in] dayKey: Valid eight-digit day key.
 * @param[in] tableName: Candidate job_event_kv_YYYYMMDD name.
 * @return: Nonzero when valid, otherwise zero.
 */
static int
isValidEventTableName(const char *dayKey, const char *tableName)
{
    const char *prefix = "job_event_kv_";
    size_t prefixLen;

    if (!isValidDayKey(dayKey) || tableName == NULL)
        return false;
    prefixLen = strlen(prefix);
    if (strncmp(tableName, prefix, prefixLen) != 0)
        return false;
    if (strcmp(tableName + prefixLen, dayKey) != 0)
        return false;
    return isValidDayKey(tableName + prefixLen);
}

/*
 * Append one validated registry entry to a growable table list.
 * @param[in,out] tables: Owned event table list.
 * @param[in] dayKey: Valid day key.
 * @param[in] tableName: Valid daily table name.
 * @return: 0 on success, otherwise -1.
 */
static int
appendEventTable(struct eventTableList *tables,
                 const char *dayKey,
                 const char *tableName)
{
    struct eventTableRef *newItems;
    size_t newCapacity;

    if (tables == NULL || !isValidEventTableName(dayKey, tableName))
        return -1;
    if (tables->count == tables->capacity) {
        newCapacity = tables->capacity == 0 ? 16 : tables->capacity * 2;
        newItems = realloc(tables->items, newCapacity * sizeof(*newItems));
        if (newItems == NULL)
            return -1;
        tables->items = newItems;
        tables->capacity = newCapacity;
    }
    memcpy(tables->items[tables->count].dayKey, dayKey,
           QUERY_DAY_KEY_SIZE);
    memcpy(tables->items[tables->count].tableName, tableName,
           strlen(tableName) + 1);
    tables->count += 1;
    return 0;
}

/*
 * Build the registry query for optional inclusive table-day bounds.
 * @param[in] range: Requested table-day range.
 * @param[out] sql: Buffer receiving registry SQL.
 * @param[in] sqlLen: Size of sql.
 * @return: 0 on success, otherwise -1.
 */
static int
buildEventTableListSql(const struct eventTableRange *range,
                       char *sql, size_t sqlLen)
{
    size_t used;

    used = 0;
    if (appendSql(sql, sqlLen, &used,
                  "SELECT day_key, table_name FROM loader_event_tables "
                  "WHERE 1=1") != 0)
        return -1;
    if (range != NULL && range->hasStartDay) {
        if (appendSql(sql, sqlLen, &used, " AND day_key >= ") != 0 ||
            appendSqlStringLiteral(sql, sqlLen, &used,
                                   range->startDay) != 0)
            return -1;
    }
    if (range != NULL && range->hasEndDay) {
        if (appendSql(sql, sqlLen, &used, " AND day_key <= ") != 0 ||
            appendSqlStringLiteral(sql, sqlLen, &used,
                                   range->endDay) != 0)
            return -1;
    }
    return appendSql(sql, sqlLen, &used, " ORDER BY day_key;");
}

/* Supplies the destination table list to the registry row callback. */
struct eventTableLoadCtx {
    struct eventTableList *tables;
};

/*
 * Validate and append one loader_event_tables result row.
 * @param[in] stmt: SQLite statement positioned on a registry row.
 * @param[in,out] ctx: eventTableLoadCtx receiving the validated entry.
 * @return: 0 on success, otherwise -1.
 */
static int
loadEventTableRow(sqlite3_stmt *stmt, void *ctx)
{
    struct eventTableLoadCtx *loadCtx;
    const unsigned char *dayKey;
    const unsigned char *tableName;

    loadCtx = (struct eventTableLoadCtx *)ctx;
    if (loadCtx == NULL || loadCtx->tables == NULL ||
        sqlite3_column_count(stmt) < 2)
        return -1;
    dayKey = sqlite3_column_text(stmt, 0);
    tableName = sqlite3_column_text(stmt, 1);
    if (dayKey == NULL || tableName == NULL)
        return -1;
    return appendEventTable(loadCtx->tables, (const char *)dayKey,
                            (const char *)tableName);
}

/*
 * Load trusted daily event table names from the loader registry.
 * @param[in] db: SQLite connection.
 * @param[in] range: Optional table-day bounds.
 * @param[out] tables: Ordered table list.
 * @return: 0 on success, otherwise -1.
 */
static int
loadEventTables(sqlite3 *db,
                const struct eventTableRange *range,
                struct eventTableList *tables)
{
    char sql[512];
    struct eventTableLoadCtx loadCtx;
    int rc;

    if (db == NULL || tables == NULL)
        return -1;
    memset(tables, 0, sizeof(*tables));
    if (buildEventTableListSql(range, sql, sizeof(sql)) != 0)
        return -1;

    loadCtx.tables = tables;
    rc = sqliteExecRows(db, sql, loadEventTableRow, &loadCtx, NULL);
    if (rc < 0) {
        freeEventTables(tables);
        return -1;
    }
    return 0;
}

/*
 * Release an event table list and reset its ownership fields.
 * @param[in,out] tables: Table list to release.
 * @return: None.
 */
static void
freeEventTables(struct eventTableList *tables)
{
    if (tables == NULL)
        return;
    free(tables->items);
    memset(tables, 0, sizeof(*tables));
}

/*
 * Append a constant SQL fragment to a bounded builder buffer.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] fragment: NUL-terminated SQL fragment.
 * @return: 0 on success, otherwise -1 when the buffer is too small.
 */
static int
appendSql(char *sql, size_t sqlLen, size_t *used, const char *fragment)
{
    int written;

    written = snprintf(sql + *used, sqlLen - *used, "%s", fragment);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;
    return 0;
}

/*
 * Append one safely quoted SQLite string literal.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] value: Unquoted value whose apostrophes are doubled.
 * @return: 0 on success, otherwise -1.
 */
static int
appendSqlStringLiteral(char *sql, size_t sqlLen, size_t *used,
                       const char *value)
{
    const char *p;

    if (appendSql(sql, sqlLen, used, "'") != 0)
        return -1;
    for (p = value; p != NULL && *p != '\0'; p++) {
        if (*p == '\'' && appendSql(sql, sqlLen, used, "'") != 0)
            return -1;
        if (*used + 1 >= sqlLen)
            return -1;
        sql[*used] = *p;
        *used += 1;
        sql[*used] = '\0';
    }
    return appendSql(sql, sqlLen, used, "'");
}

/*
 * Append optional lower and upper bounds for one event-time column.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] range: Column and normalized epoch bounds.
 * @return: 0 on success, otherwise -1.
 */
static int
appendTimeRangePredicate(char *sql, size_t sqlLen, size_t *used,
                         const struct timeRange *range)
{
    int written;

    written = snprintf(sql + *used, sqlLen - *used,
                       "%s IS NOT NULL", range->column);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;

    if (range->hasStart) {
        written = snprintf(sql + *used, sqlLen - *used,
                           " AND %s >= %lld",
                           range->column, range->startEpoch);
        if (written < 0 || (size_t)written >= sqlLen - *used)
            return -1;
        *used += (size_t)written;
    }
    if (range->hasEnd) {
        written = snprintf(sql + *used, sqlLen - *used,
                           " AND %s <= %lld",
                           range->column, range->endEpoch);
        if (written < 0 || (size_t)written >= sqlLen - *used)
            return -1;
        *used += (size_t)written;
    }
    return 0;
}

/*
 * Register a submitting-user equality filter when -u names one user.
 * @param[in,out] filters: Fixed-capacity filter array.
 * @param[in,out] count: Number of registered filters.
 * @param[in] userName: Requested user, "all", or NULL.
 * @return: 0 when added or omitted, otherwise -1 for capacity exhaustion.
 */
static int
addUserFilter(struct queryFilter *filters, int *count, const char *userName)
{
    struct queryFilter *filter;

    if (filters == NULL || count == NULL || userName == NULL ||
        userName[0] == '\0' || strcmp(userName, "all") == 0)
        return 0;
    if (*count < 0 || *count >= QUERY_MAX_FILTERS)
        return -1;

    filter = &filters[*count];
    memset(filter, 0, sizeof(*filter));
    filter->kind = FILTER_USER;
    filter->cteName = "u_jobs";
    filter->data.user.userName = userName;
    *count += 1;
    return 0;
}

/*
 * Register a queue equality filter when -q is present.
 * @param[in,out] filters: Fixed-capacity filter array.
 * @param[in,out] count: Number of registered filters.
 * @param[in] queueName: Requested queue name or NULL.
 * @return: 0 when added or omitted, otherwise -1 for capacity exhaustion.
 */
static int
addQueueFilter(struct queryFilter *filters, int *count, const char *queueName)
{
    struct queryFilter *filter;

    if (filters == NULL || count == NULL || queueName == NULL ||
        queueName[0] == '\0')
        return 0;
    if (*count < 0 || *count >= QUERY_MAX_FILTERS)
        return -1;

    filter = &filters[*count];
    memset(filter, 0, sizeof(*filter));
    filter->kind = FILTER_QUEUE;
    filter->cteName = "q_jobs";
    filter->data.string.value = queueName;
    *count += 1;
    return 0;
}

/*
 * Register one enabled time filter with its SQL identity and execution phase.
 * @param[in,out] filters: Fixed-capacity filter array.
 * @param[in,out] count: Number of registered filters.
 * @param[in] kind: Time filter kind.
 * @param[in] cteName: Constant CTE name used by the SQL builder.
 * @param[in] column: Constant indexed column name.
 * @param[in] timeFilter: Parsed bhist time option.
 * @param[in] postCandidate: Whether to apply after candidate materialization.
 * @return: 0 when added or omitted, otherwise -1.
 */
static int
addTimeFilter(struct queryFilter *filters, int *count,
              enum filterKind kind, const char *cteName,
              const char *column, const struct timeFilter *timeFilter,
              int postCandidate)
{
    struct queryFilter *filter;

    if (filters == NULL || count == NULL || timeFilter == NULL ||
        !timeFilter->enabled)
        return 0;
    if (*count < 0 || *count >= QUERY_MAX_FILTERS)
        return -1;

    filter = &filters[*count];
    memset(filter, 0, sizeof(*filter));
    filter->kind = kind;
    filter->cteName = cteName;
    filter->postCandidate = postCandidate;
    filter->data.time.column = column;
    filter->data.time.hasStart = timeFilter->hasStart;
    filter->data.time.hasEnd = timeFilter->hasEnd;
    filter->data.time.startEpoch = timeFilter->startEpoch;
    filter->data.time.endEpoch = timeFilter->endEpoch;
    *count += 1;
    return 0;
}

/*
 * Return the columns emitted by a set-producing filter CTE.
 * @param[in] filter: Filter whose output shape is requested.
 * @return: Constant SQL select-list text.
 */
static const char *
filterSelectList(const struct queryFilter *filter)
{
    (void)filter;
    return "job_id";
}

/*
 * Append the SQL predicate for one typed filter.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filter: Filter to render.
 * @param[in] alias: Optional trusted table alias.
 * @return: 0 on success, otherwise -1.
 */
static int
appendFilterPredicate(char *sql, size_t sqlLen, size_t *used,
                      const struct queryFilter *filter, const char *alias)
{
    int written;

    if (filter == NULL)
        return -1;

    /*
     * Every filterKind used for candidate selection must emit its predicate
     * here. SQL identifiers are fixed in code; command-line values pass through
     * appendSqlStringLiteral(). Never accept a column name from argv.
     */
    switch (filter->kind) {
    case FILTER_USER:
        if (alias != NULL) {
            written = snprintf(sql + *used, sqlLen - *used,
                               "%s.submit_user IS NOT NULL "
                               "AND %s.submit_user = ",
                               alias, alias);
            if (written < 0 || (size_t)written >= sqlLen - *used)
                return -1;
            *used += (size_t)written;
        } else if (appendSql(sql, sqlLen, used,
                             "submit_user IS NOT NULL "
                             "AND submit_user = ") != 0) {
            return -1;
        }
        if (appendSqlStringLiteral(sql, sqlLen, used,
                                   filter->data.user.userName) != 0)
            return -1;
        return 0;
    case FILTER_QUEUE:
        if (alias != NULL) {
            written = snprintf(sql + *used, sqlLen - *used,
                               "%s.submit_queue IS NOT NULL "
                               "AND %s.submit_queue = ",
                               alias, alias);
            if (written < 0 || (size_t)written >= sqlLen - *used)
                return -1;
            *used += (size_t)written;
        } else if (appendSql(sql, sqlLen, used,
                             "submit_queue IS NOT NULL "
                             "AND submit_queue = ") != 0) {
            return -1;
        }
        if (appendSqlStringLiteral(sql, sqlLen, used,
                                   filter->data.string.value) != 0)
            return -1;
        return 0;
    case FILTER_SUBMIT_TIME:
    case FILTER_DISPATCH_TIME:
    case FILTER_COMPLETE_TIME:
        if (alias != NULL)
            return appendAliasedTimeRangePredicate(sql, sqlLen, used, alias,
                                                   &filter->data.time);
        return appendTimeRangePredicate(sql, sqlLen, used,
                                        &filter->data.time);
    case FILTER_JOB_ID:
        if (alias != NULL) {
            written = snprintf(sql + *used, sqlLen - *used,
                               "%s.job_id = %lld", alias,
                               filter->data.job.jobId);
        } else {
            written = snprintf(sql + *used, sqlLen - *used,
                               "job_id = %lld", filter->data.job.jobId);
        }
        if (written < 0 || (size_t)written >= sqlLen - *used)
            return -1;
        *used += (size_t)written;
        return 0;
    default:
        return -1;
    }
}

/*
 * Append a filter-specific SELECT from one trusted daily table.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filter: Filter selecting matching job IDs.
 * @param[in] table: Trusted daily table reference.
 * @param[in] alias: Optional trusted table alias.
 * @return: 0 on success, otherwise -1.
 */
static int
appendFilterTableSelect(char *sql, size_t sqlLen, size_t *used,
                        const struct queryFilter *filter,
                        const struct eventTableRef *table,
                        const char *alias)
{
    int written;

    if (filter == NULL || table == NULL)
        return -1;

    written = snprintf(sql + *used, sqlLen - *used,
                       "SELECT %s FROM %s",
                       filterSelectList(filter), table->tableName);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;
    if (alias != NULL && alias[0] != '\0') {
        written = snprintf(sql + *used, sqlLen - *used, " AS %s", alias);
        if (written < 0 || (size_t)written >= sqlLen - *used)
            return -1;
        *used += (size_t)written;
    }
    if (appendSql(sql, sqlLen, used, " WHERE ") != 0 ||
        appendFilterPredicate(sql, sqlLen, used, filter, alias) != 0)
        return -1;
    return 0;
}

/*
 * Append a typed empty SELECT preserving a filter CTE's output shape.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filter: Filter whose output shape is preserved.
 * @return: 0 on success, otherwise -1.
 */
static int
appendNoRowsFilterSelect(char *sql, size_t sqlLen, size_t *used,
                         const struct queryFilter *filter)
{
    (void)filter;
    return appendSql(sql, sqlLen, used,
                     "SELECT NULL AS job_id WHERE 0");
}

/*
 * Test whether a filter independently produces a set of base job IDs.
 * @param[in] filter: Filter to classify.
 * @return: Nonzero for set-producing time filters, otherwise zero.
 */
static int
isJobIdSetFilter(const struct queryFilter *filter)
{
    /*
     * Each set filter produces a job_id set. INTERSECT combines time filters so
     * a job must satisfy every requested time dimension. Metadata equality
     * filters such as -q do not belong in this class.
     */
    if (filter == NULL)
        return false;
    return filter->kind == FILTER_SUBMIT_TIME ||
        filter->kind == FILTER_DISPATCH_TIME ||
        filter->kind == FILTER_COMPLETE_TIME;
}

/*
 * Test whether a filter is reapplied as correlated metadata existence.
 * @param[in] filter: Filter to classify.
 * @return: Nonzero for user and queue filters, otherwise zero.
 */
static int
isExistsFilter(const struct queryFilter *filter)
{
    /*
     * EXISTS filters inspect metadata rows for each candidate base job ID.
     * Enabled filters are appended independently and therefore have AND
     * semantics. Future string metadata filters comparable to -q belong here.
     */
    if (filter == NULL)
        return false;
    return filter->kind == FILTER_USER || filter->kind == FILTER_QUEUE;
}

/*
 * Append one UNION-based job-ID set CTE across all trusted daily tables.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filter: Set-producing filter.
 * @param[in] tables: Trusted daily table list.
 * @return: 0 on success, otherwise -1.
 */
static int
appendJobIdSetFilterCte(char *sql, size_t sqlLen, size_t *used,
                        const struct queryFilter *filter,
                        const struct eventTableList *tables)
{
    int written;

    if (filter == NULL || filter->cteName == NULL || tables == NULL ||
        !isJobIdSetFilter(filter))
        return -1;

    written = snprintf(sql + *used, sqlLen - *used,
                       "%s AS (SELECT DISTINCT %s FROM (",
                       filter->cteName, filterSelectList(filter));
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;

    if (tables->count == 0) {
        if (appendNoRowsFilterSelect(sql, sqlLen, used, filter) != 0)
            return -1;
    } else {
        size_t i;

        for (i = 0; i < tables->count; i++) {
            if (i > 0 && appendSql(sql, sqlLen, used, " UNION ALL ") != 0)
                return -1;
            if (appendFilterTableSelect(sql, sqlLen, used, filter,
                                        &tables->items[i], NULL) != 0)
                return -1;
        }
    }

    return appendSql(sql, sqlLen, used, "))");
}

/*
 * Append a time-range predicate qualified by a trusted table alias.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] alias: Trusted SQL alias.
 * @param[in] time: Normalized time range.
 * @return: 0 on success, otherwise -1.
 */
static int
appendAliasedTimeRangePredicate(char *sql, size_t sqlLen, size_t *used,
                                const char *alias,
                                const struct timeRange *time)
{
    struct timeRange aliased;
    char column[64];
    int written;

    written = snprintf(column, sizeof(column), "%s.%s", alias, time->column);
    if (written < 0 || (size_t)written >= sizeof(column))
        return -1;

    aliased = *time;
    aliased.column = column;
    return appendTimeRangePredicate(sql, sqlLen, used, &aliased);
}

/*
 * Append candidate_jobs by intersecting set filters or selecting a seed set.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filters: Registered query filters.
 * @param[in] filterCount: Number of filters.
 * @param[in] tables: Trusted daily table list.
 * @return: 0 on success, otherwise -1.
 */
static int
appendCandidateIntersectionCte(char *sql, size_t sqlLen, size_t *used,
                               const struct queryFilter *filters,
                               int filterCount,
                               const struct eventTableList *tables)
{
    const struct queryFilter *seedFilter;
    size_t tableIndex;
    int i;
    int setCount;
    int written;

    if (filters == NULL || filterCount < 0 || tables == NULL)
        return -1;
    if (appendSql(sql, sqlLen, used, "candidate_jobs AS (") != 0)
        return -1;

    setCount = 0;
    for (i = 0; i < filterCount; i++) {
        if (!isJobIdSetFilter(&filters[i]))
            continue;
        if (setCount > 0 &&
            appendSql(sql, sqlLen, used, " INTERSECT ") != 0)
            return -1;
        if (appendSql(sql, sqlLen, used, "SELECT job_id FROM ") != 0 ||
            appendSql(sql, sqlLen, used, filters[i].cteName) != 0)
            return -1;
        setCount++;
    }

    if (setCount == 0) {
        /*
         * Without a set filter, seed candidate_jobs from the first EXISTS
         * filter instead of enumerating every job in every daily table. This is
         * only an optimization: appendExistsFilteredJobsCte() reapplies all
         * EXISTS filters, including the seed, so registration order cannot
         * change query semantics.
         */
        seedFilter = NULL;
        for (i = 0; i < filterCount; i++) {
            if (isExistsFilter(&filters[i])) {
                seedFilter = &filters[i];
                break;
            }
        }
        if (tables->count == 0) {
            if (appendSql(sql, sqlLen, used,
                          "SELECT NULL AS job_id WHERE 0") != 0)
                return -1;
        } else {
            if (appendSql(sql, sqlLen, used,
                          "SELECT DISTINCT job_id FROM (") != 0)
                return -1;
            for (tableIndex = 0; tableIndex < tables->count; tableIndex++) {
                if (tableIndex > 0 &&
                    appendSql(sql, sqlLen, used, " UNION ALL ") != 0)
                    return -1;
                if (seedFilter != NULL) {
                    if (appendFilterTableSelect(
                            sql, sqlLen, used, seedFilter,
                            &tables->items[tableIndex], NULL) != 0)
                        return -1;
                } else {
                    written = snprintf(
                        sql + *used, sqlLen - *used,
                        "SELECT job_id FROM %s",
                        tables->items[tableIndex].tableName);
                    if (written < 0 ||
                        (size_t)written >= sqlLen - *used)
                        return -1;
                    *used += (size_t)written;
                }
            }
            if (appendSql(sql, sqlLen, used, ")") != 0)
                return -1;
        }
    }
    return appendSql(sql, sqlLen, used, "), ");
}

/*
 * Append one table branch of a correlated metadata EXISTS filter.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filter: Metadata equality filter.
 * @param[in] table: Trusted daily table reference.
 * @param[in] alias: Trusted table alias.
 * @return: 0 on success, otherwise -1.
 */
static int
appendExistsFilterForTable(char *sql, size_t sqlLen, size_t *used,
                           const struct queryFilter *filter,
                           const struct eventTableRef *table,
                           const char *alias)
{
    int written;

    if (!isExistsFilter(filter) || table == NULL || alias == NULL)
        return -1;
    written = snprintf(sql + *used, sqlLen - *used,
                       "SELECT 1 FROM %s AS %s",
                       table->tableName, alias);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;
    written = snprintf(sql + *used, sqlLen - *used,
                       " WHERE %s.job_id=candidate_jobs.job_id AND ",
                       alias);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;
    return appendFilterPredicate(sql, sqlLen, used, filter, alias);
}

/*
 * Append one correlated EXISTS clause across all trusted daily tables.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filter: Metadata equality filter.
 * @param[in] index: Filter index used to derive a unique alias.
 * @param[in] tables: Trusted daily table list.
 * @return: 0 on success, otherwise -1.
 */
static int
appendExistsFilter(char *sql, size_t sqlLen, size_t *used,
                   const struct queryFilter *filter, int index,
                   const struct eventTableList *tables)
{
    char alias[16];
    size_t i;

    if (!isExistsFilter(filter) || tables == NULL)
        return -1;
    snprintf(alias, sizeof(alias), "f%d", index);
    if (appendSql(sql, sqlLen, used, "AND EXISTS (") != 0)
        return -1;
    if (tables->count == 0) {
        if (appendSql(sql, sqlLen, used, "SELECT 1 WHERE 0") != 0)
            return -1;
    } else {
        for (i = 0; i < tables->count; i++) {
            if (i > 0 && appendSql(sql, sqlLen, used, " UNION ALL ") != 0)
                return -1;
            if (appendExistsFilterForTable(sql, sqlLen, used, filter,
                                           &tables->items[i], alias) != 0)
                return -1;
        }
    }
    return appendSql(sql, sqlLen, used, ") ");
}

/*
 * Append filtered_jobs and apply every registered metadata EXISTS filter.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filters: Registered query filters.
 * @param[in] filterCount: Number of filters.
 * @param[in] tables: Trusted daily table list.
 * @return: 0 on success, otherwise -1.
 */
static int
appendExistsFilteredJobsCte(char *sql, size_t sqlLen, size_t *used,
                            const struct queryFilter *filters,
                            int filterCount,
                            const struct eventTableList *tables)
{
    int i;

    if (filters == NULL || tables == NULL)
        return -1;
    if (appendSql(sql, sqlLen, used,
                  "filtered_jobs AS (SELECT job_id FROM candidate_jobs "
                  "WHERE 1=1 ") != 0)
        return -1;
    for (i = 0; i < filterCount; i++) {
        if (!isExistsFilter(&filters[i]))
            continue;
        if (appendExistsFilter(sql, sqlLen, used, &filters[i], i,
                               tables) != 0)
            return -1;
    }
    return appendSql(sql, sqlLen, used, "), ");
}

/*
 * Append SQL that materializes ordered unique candidate jobs in a temp table.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filters: Registered query filters.
 * @param[in] filterCount: Number of filters.
 * @param[in] tables: Trusted daily table list.
 * @return: 0 on success, otherwise -1.
 */
static int
appendFilteredJobsTempLoadSql(char *sql, size_t sqlLen, size_t *used,
                              const struct queryFilter *filters,
                              int filterCount,
                              const struct eventTableList *tables)
{
    if (appendSql(sql, sqlLen, used,
                  "CREATE TEMP TABLE " QUERY_TEMP_FILTERED_JOBS
                  "(seq INTEGER PRIMARY KEY, "
                  "job_id INTEGER NOT NULL UNIQUE); ") != 0)
        return -1;
    if (appendBaseQueryCtesSql(sql, sqlLen, used,
                               filters, filterCount, tables) != 0)
        return -1;
    trimTrailingCteComma(sql, used);
    return appendSql(sql, sqlLen, used,
                     " INSERT OR IGNORE INTO " QUERY_TEMP_FILTERED_JOBS
                     "(job_id) SELECT DISTINCT job_id FROM filtered_jobs "
                     "ORDER BY job_id; ");
}

/*
 * Materialize rowids for one day's events belonging to the current job batch.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] table: Trusted daily table reference.
 * @param[in] jobTable: Trusted temporary job table name.
 * @return: 0 on success, otherwise -1.
 */
static int
appendSelectedRowidsLoadForTableSql(char *sql, size_t sqlLen, size_t *used,
                                    const struct eventTableRef *table,
                                    const char *jobTable)
{
    int written;

    if (table == NULL || jobTable == NULL)
        return -1;
    if (appendSql(sql, sqlLen, used,
                  "DELETE FROM " QUERY_TEMP_SELECTED_ROWIDS "; ") != 0)
        return -1;
    written = snprintf(
        sql + *used, sqlLen - *used,
        "INSERT INTO " QUERY_TEMP_SELECTED_ROWIDS "(row_id) "
        "SELECT k.rowid FROM %s f CROSS JOIN %s AS k",
        jobTable, table->tableName);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;
    return appendSql(sql, sqlLen, used,
                     " WHERE k.job_id=f.job_id ORDER BY k.rowid; ");
}

/*
 * Append ordered raw-event selection for one daily table's rowids.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] table: Trusted daily table reference.
 * @param[in] selectExpr: Constant select-list expression.
 * @return: 0 on success, otherwise -1.
 */
static int
appendOrderedRawLineSelectForTableSql(char *sql, size_t sqlLen,
                                      size_t *used,
                                      const struct eventTableRef *table,
                                      const char *selectExpr)
{
    int written;

    if (table == NULL || selectExpr == NULL)
        return -1;
    written = snprintf(
        sql + *used, sqlLen - *used,
        "SELECT %s FROM " QUERY_TEMP_SELECTED_ROWIDS " r "
        "INNER JOIN %s AS k ON k.rowid=r.row_id "
        "ORDER BY r.row_id; ",
        selectExpr, table->tableName);
    if (written < 0 || (size_t)written >= sqlLen - *used)
        return -1;
    *used += (size_t)written;
    return 0;
}

/*
 * Build an ordered raw-event query for one exact base job ID.
 * @param[in] tables: Trusted daily table list.
 * @param[in] jobId: Base job ID to select.
 * @param[out] sql: Buffer receiving query SQL.
 * @param[in] sqlLen: Size of sql.
 * @return: 0 on success, otherwise -1.
 */
static int
buildJobIdQuery(const struct eventTableList *tables,
                long long jobId, char *sql, size_t sqlLen)
{
    size_t used;
    size_t i;
    int written;

    if (tables == NULL)
        return -1;
    used = 0;
    if (appendSql(sql, sqlLen, &used,
                  "SELECT job_id, raw_line FROM (") != 0)
        return -1;
    if (tables->count == 0) {
        if (appendSql(sql, sqlLen, &used,
                      "SELECT NULL AS table_seq, NULL AS row_id, "
                      "NULL AS job_id, NULL AS raw_line WHERE 0") != 0)
            return -1;
    } else {
        for (i = 0; i < tables->count; i++) {
            if (i > 0 && appendSql(sql, sqlLen, &used, " UNION ALL ") != 0)
                return -1;
            written = snprintf(
                sql + used, sqlLen - used,
                "SELECT %zu AS table_seq, k.rowid AS row_id, "
                "k.job_id, k.raw_line FROM %s AS k",
                i, tables->items[i].tableName);
            if (written < 0 || (size_t)written >= sqlLen - used)
                return -1;
            used += (size_t)written;
            written = snprintf(sql + used, sqlLen - used,
                               " WHERE k.job_id = %lld", jobId);
            if (written < 0 || (size_t)written >= sqlLen - used)
                return -1;
            used += (size_t)written;
        }
    }
    return appendSql(sql, sqlLen, &used,
                     ") ORDER BY table_seq, row_id;");
}

/*
 * Register every enabled non-job-ID filter for candidate SQL generation.
 * @param[in] submitFilter: Optional submission-time filter.
 * @param[in] dispatchFilter: Optional dispatch-time filter.
 * @param[in] completeFilter: Optional completion-time filter.
 * @param[in] userName: Optional submitting user.
 * @param[in] queueName: Optional queue name.
 * @param[out] filters: Fixed-capacity filter array.
 * @param[out] filterCount: Number of registered filters.
 * @return: 0 on success, otherwise -1.
 */
static int
buildSqliteFilters(const struct timeFilter *submitFilter,
                   const struct timeFilter *dispatchFilter,
                   const struct timeFilter *completeFilter,
                   const char *userName,
                   const char *queueName,
                   struct queryFilter *filters, int *filterCount)
{
    /*
     * Register every non-job-ID query filter here. Adding only a helper is not
     * sufficient: candidate materialization and profiling must share this list.
     * Registration order also controls CTE and EXISTS generation. New filters
     * belong in the correct set class rather than in an ad hoc trailing WHERE.
     */
    if (filters == NULL || filterCount == NULL)
        return -1;
    *filterCount = 0;
    memset(filters, 0, sizeof(struct queryFilter) * QUERY_MAX_FILTERS);

    if (addTimeFilter(filters, filterCount, FILTER_SUBMIT_TIME,
                      "s_jobs", "submit_time", submitFilter, false) != 0 ||
        addTimeFilter(filters, filterCount, FILTER_DISPATCH_TIME,
                      "d_jobs", "start_time", dispatchFilter, false) != 0 ||
        addTimeFilter(filters, filterCount, FILTER_COMPLETE_TIME,
                      "c_jobs", "end_time", completeFilter, false) != 0)
        return -1;

    if (addUserFilter(filters, filterCount, userName) != 0 ||
        addQueueFilter(filters, filterCount, queueName) != 0)
        return -1;
    return 0;
}

/*
 * Append all candidate and metadata-filter CTE definitions.
 * @param[in,out] sql: SQL output buffer.
 * @param[in] sqlLen: Size of sql.
 * @param[in,out] used: Number of bytes already written.
 * @param[in] filters: Registered query filters.
 * @param[in] filterCount: Number of filters.
 * @param[in] tables: Trusted daily table list.
 * @return: 0 on success, otherwise -1.
 */
static int
appendBaseQueryCtesSql(char *sql, size_t sqlLen, size_t *used,
                       const struct queryFilter *filters, int filterCount,
                       const struct eventTableList *tables)
{
    int i;
    int setCount;

    if (filters == NULL || filterCount < 0 || tables == NULL)
        return -1;
    if (appendSql(sql, sqlLen, used, "WITH ") != 0)
        return -1;

    setCount = 0;
    for (i = 0; i < filterCount; i++) {
        if (!isJobIdSetFilter(&filters[i]))
            continue;
        if (setCount > 0 && appendSql(sql, sqlLen, used, ", ") != 0)
            return -1;
        if (appendJobIdSetFilterCte(sql, sqlLen, used, &filters[i],
                                    tables) != 0)
            return -1;
        setCount++;
    }
    if (setCount > 0 && appendSql(sql, sqlLen, used, ", ") != 0)
        return -1;
    if (appendCandidateIntersectionCte(sql, sqlLen, used,
                                       filters, filterCount, tables) != 0 ||
        appendExistsFilteredJobsCte(sql, sqlLen, used,
                                    filters, filterCount, tables) != 0)
        return -1;
    return 0;
}

/*
 * Remove the final comma and space from an unfinished CTE list.
 * @param[in,out] sql: SQL output buffer.
 * @param[in,out] used: Number of bytes already written.
 * @return: None.
 */
static void
trimTrailingCteComma(char *sql, size_t *used)
{
    if (sql == NULL || used == NULL)
        return;
    if (*used >= 2 && sql[*used - 2] == ',' && sql[*used - 1] == ' ') {
        *used -= 2;
        sql[*used] = '\0';
    }
}

/*
 * Build the script that materializes filtered jobs and batch helper tables.
 * @param[in] submitFilter: Optional submission-time filter.
 * @param[in] dispatchFilter: Optional dispatch-time filter.
 * @param[in] completeFilter: Optional completion-time filter.
 * @param[in] userName: Optional submitting user.
 * @param[in] queueName: Optional queue name.
 * @param[in] tables: Trusted daily table list.
 * @param[out] sql: Buffer receiving SQL.
 * @param[in] sqlLen: Size of sql.
 * @return: 0 on success, otherwise -1.
 */
static int
buildCandidateJobsQuery(const struct timeFilter *submitFilter,
                        const struct timeFilter *dispatchFilter,
                        const struct timeFilter *completeFilter,
                        const char *userName,
                        const char *queueName,
                        const struct eventTableList *tables,
                        char *sql, size_t sqlLen)
{
    struct queryFilter filters[QUERY_MAX_FILTERS];
    size_t used;
    int filterCount;

    used = 0;
    if (buildSqliteFilters(submitFilter, dispatchFilter, completeFilter,
                           userName, queueName,
                           filters, &filterCount) != 0)
        return -1;
    if (appendFilteredJobsTempLoadSql(sql, sqlLen, &used,
                                      filters, filterCount, tables) != 0)
        return -1;
    return appendSql(
        sql, sqlLen, &used,
        "CREATE TEMP TABLE " QUERY_TEMP_BATCH_JOBS
        "(seq INTEGER PRIMARY KEY, job_id INTEGER NOT NULL UNIQUE); "
        "CREATE TEMP TABLE " QUERY_TEMP_SELECTED_ROWIDS
        "(row_id INTEGER PRIMARY KEY); ");
}

/*
 * Build SQL that advances the current candidate-job batch by sequence number.
 * @param[in] lastSeq: Last sequence consumed by the previous batch.
 * @param[in] batchSize: Maximum distinct base job IDs in the next batch.
 * @param[out] sql: Buffer receiving SQL.
 * @param[in] sqlLen: Size of sql.
 * @return: 0 on success, otherwise -1.
 */
static int
buildBatchLoadSql(long long lastSeq, int batchSize,
                  char *sql, size_t sqlLen)
{
    int written;

    if (sql == NULL || batchSize <= 0)
        return -1;
    /*
     * The limit counts numeric job IDs, not job generations. A job ID can be
     * reused after wraparound, so one selected ID may produce multiple jobs.
     */
    written = snprintf(
        sql, sqlLen,
        "DELETE FROM " QUERY_TEMP_BATCH_JOBS "; "
        "INSERT INTO " QUERY_TEMP_BATCH_JOBS "(seq, job_id) "
        "SELECT seq, job_id FROM " QUERY_TEMP_FILTERED_JOBS " "
        "WHERE seq > %lld ORDER BY seq LIMIT %d;",
        lastSeq, batchSize);
    return written >= 0 && (size_t)written < sqlLen ? 0 : -1;
}

/*
 * Build ordered raw-event selection SQL for the current job batch.
 * @param[in] tables: Trusted daily table list.
 * @param[out] sql: Buffer receiving SQL.
 * @param[in] sqlLen: Size of sql.
 * @return: 0 on success, otherwise -1.
 */
static int
buildBatchRawQuery(const struct eventTableList *tables,
                   char *sql, size_t sqlLen)
{
    size_t i;
    size_t used;

    if (tables == NULL || sql == NULL)
        return -1;
    used = 0;
    for (i = 0; i < tables->count; i++) {
        if (appendSelectedRowidsLoadForTableSql(
                sql, sqlLen, &used, &tables->items[i],
                QUERY_TEMP_BATCH_JOBS) != 0 ||
            appendOrderedRawLineSelectForTableSql(
                sql, sqlLen, &used, &tables->items[i],
                "k.job_id, k.raw_line") != 0)
            return -1;
    }
    return 0;
}

/* Supplies the active bhist stream to a raw-event SQLite row callback. */
struct sqliteBhistEventCtx {
    struct bhistStream *stream;
};

/*
 * Add one SQLite job_id/raw_line result row to the active bhist stream.
 * @param[in] stmt: SQLite statement positioned on a raw-event row.
 * @param[in,out] ctx: sqliteBhistEventCtx containing the stream.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteBhistEventRow(sqlite3_stmt *stmt, void *ctx)
{
    struct sqliteBhistEventCtx *eventCtx;
    const unsigned char *rawLine;
    long long jobId;

    eventCtx = (struct sqliteBhistEventCtx *)ctx;
    if (eventCtx == NULL || eventCtx->stream == NULL ||
        sqlite3_column_count(stmt) < 2)
        return -1;
    if (sqlite3_column_type(stmt, 0) == SQLITE_NULL ||
        sqlite3_column_type(stmt, 1) == SQLITE_NULL)
        return -1;
    jobId = sqlite3_column_int64(stmt, 0);
    rawLine = sqlite3_column_text(stmt, 1);
    if (rawLine == NULL)
        return -1;
    return bhistStreamAdd(eventCtx->stream, jobId,
                                      (const char *)rawLine);
}

/*
 * Execute a raw-event query and prepare an unstarted bhist stream.
 * @param[in] db: SQLite connection.
 * @param[in] sql: Raw-event query SQL.
 * @param[in] request: bhist request parameters.
 * @param[out] streamOut: Prepared stream.
 * @return: 0 on success, otherwise -1.
 */
static int
sqlitePrepareBhistQuery(sqlite3 *db, const char *sql,
                       const struct bhistRequest *request,
                       struct bhistStream **streamOut)
{
    struct bhistStream *stream;
    struct sqliteBhistEventCtx eventCtx;
    int rc;

    if (db == NULL || sql == NULL || request == NULL || streamOut == NULL)
        return -1;
    *streamOut = NULL;
    stream = NULL;
    rc = -1;

    if (bhistStreamOpen(request, &stream) != 0)
        goto done;

    eventCtx.stream = stream;
    if (sqliteExecRows(db, sql, sqliteBhistEventRow, &eventCtx, NULL) != 0)
        goto done;
    /* Commit after buckets are prepared so the next batch does not pin this snapshot. */
    if (sqliteExecRows(db, "COMMIT", NULL, NULL, NULL) != 0)
        goto done;
    /* Transfer stream ownership so the cleanup path cannot release it twice. */
    *streamOut = stream;
    stream = NULL;
    rc = 0;

done:
    if (stream != NULL)
        bhistStreamAbort(stream);
    return rc;
}

/*
 * Execute one SQLite batch and format it with bhist synchronously.
 * @param[in] db: SQLite connection.
 * @param[in] sql: Raw-event query SQL.
 * @param[in] request: bhist request parameters.
 * @param[in,out] outputState: Cross-batch state, or NULL for one batch.
 * @param[out] foundOut: Whether bhist produced matching output.
 * @return: 0 on success, otherwise -1.
 */
static int
sqliteRunBhistQuery(sqlite3 *db, const char *sql,
                    const struct bhistRequest *request,
                    struct bhistOutputState *outputState,
                    int *foundOut)
{
    struct bhistStream *stream;
    int rc;

    if (foundOut == NULL)
        return -1;
    stream = NULL;
    if (sqlitePrepareBhistQuery(db, sql, request, &stream) != 0)
        return -1;
    if (outputState != NULL)
        rc = bhistStreamFinishBatch(
            stream, outputState, foundOut);
    else
        rc = bhistStreamFinish(stream, foundOut);
    return rc;
}

/*
 * Run one server query in the dedicated query child.
 * @param[in] argc: Number of bhist-compatible arguments.
 * @param[in] argv: bhist-compatible argument vector.
 * @return: 0 on success, otherwise nonzero for invalid input or query failure.
 */
int
runQuery(int argc, char **argv)
{
    const char *dbPath;
    const char *bhistPath;
    long long baseJobId;
    int longFormat = false;
    int wideFormat = false;
    int cc;
    char *userName = NULL;
    char *queueName = NULL;
    char *hostName = NULL;
    int found;
    int selectorCount;
    int outputStateInitialized;
    unsigned int bhistStateFlags;
    long long expectedJobs;
    long long batchJobs;
    long long batchMaxSeq;
    long long lastSeq;
    char defaultUserName[USER_NAME_MAX];
    const char *requestUserHint;
    char *sql;
    struct eventTableList eventTables;
    struct eventTableRange candidateTableRange;
    struct bhistRequest request;
    struct bhistOutputState outputState;
    struct bhistStream *currentStream;
    struct bhistStream *nextStream;
    struct timeFilter submitFilter;
    struct timeFilter dispatchFilter;
    struct timeFilter completeFilter;
    sqlite3 *queryDb;

    sql = NULL;
    queryDb = NULL;
    currentStream = NULL;
    nextStream = NULL;
    found = false;
    outputStateInitialized = false;
    bhistStateFlags = 0;
    memset(&eventTables, 0, sizeof(eventTables));
    memset(&candidateTableRange, 0, sizeof(candidateTableRange));
    memset(&submitFilter, 0, sizeof(submitFilter));
    memset(&dispatchFilter, 0, sizeof(dispatchFilter));
    memset(&completeFilter, 0, sizeof(completeFilter));
    submitFilter.opt = 'S';
    submitFilter.column = "submit_time";
    dispatchFilter.opt = 'D';
    dispatchFilter.column = "start_time";
    completeFilter.opt = 'C';
    completeFilter.column = "end_time";

    /* exec() used to reset getopt state before entering this function. */
    optind = 1;
    opterr = 1;
    if (loadQuerySqliteTuning() != 0)
        return -1;

    while ((cc = getopt(argc, argv, "Vhlwadepsru:q:m:S:D:C:")) != EOF) {
        switch (cc) {
        case 'l':
            if (wideFormat)
                return invalidArguments();
            longFormat = true;
            break;
        case 'w':
            if (longFormat)
                return invalidArguments();
            wideFormat = true;
            break;
        case 'a':
            bhistStateFlags |= BHIST_FLAG_A;
            break;
        case 'd':
            bhistStateFlags |= BHIST_FLAG_D;
            break;
        case 'e':
            bhistStateFlags |= BHIST_FLAG_E;
            break;
        case 'p':
            bhistStateFlags |= BHIST_FLAG_P;
            break;
        case 's':
            bhistStateFlags |= BHIST_FLAG_S;
            break;
        case 'r':
            bhistStateFlags |= BHIST_FLAG_R;
            break;
        case 'u':
            if (userName != NULL || optarg == NULL || optarg[0] == '\0')
                return invalidArguments();
            userName = optarg;
            break;
        case 'q':
            if (queueName != NULL || optarg == NULL || optarg[0] == '\0')
                return invalidArguments();
            queueName = optarg;
            break;
        case 'm':
            if (hostName != NULL || optarg == NULL || optarg[0] == '\0')
                return invalidArguments();
            hostName = optarg;
            break;
        case 'S':
            submitFilter.enabled = true;
            submitFilter.arg = optarg;
            if (parseTimeRange(optarg, &submitFilter) != 0) {
                fprintf(stderr, "bhist speed up: invalid -S time range: %s\n", optarg);
                return -1;
            }
            break;
        case 'D':
            dispatchFilter.enabled = true;
            dispatchFilter.arg = optarg;
            if (parseTimeRange(optarg, &dispatchFilter) != 0) {
                fprintf(stderr, "bhist speed up: invalid -D time range: %s\n", optarg);
                return -1;
            }
            break;
        case 'C':
            completeFilter.enabled = true;
            completeFilter.arg = optarg;
            if (parseTimeRange(optarg, &completeFilter) != 0) {
                fprintf(stderr, "bhist speed up: invalid -C time range: %s\n", optarg);
                return -1;
            }
            break;
        case 'V':
            fputs(PROGRAM_VERSION, stdout);
            return 0;
        case 'h':
        default:
            return invalidArguments();
        }
    }

    selectorCount = argc - optind;
    /*
     * Native bhist accepts filters together with a job selector, then gives
     * the selector precedence and clears user/queue/host/time/state filters.
     * The selector runner below has the same behavior, so only extra
     * selectors are rejected here.
     */
    if (selectorCount > 1)
        return invalidArguments();

    if (selectorCount == 0 && userName == NULL) {
        requestUserHint = getenv(
            "LSB_BHIST_INTERNAL_REQUEST_USER");
        if (requestUserHint != NULL && requestUserHint[0] != '\0') {
            if (strlen(requestUserHint) >= sizeof(defaultUserName)) {
                fprintf(stderr,
                        "bhist speed up: request user hint is too long\n");
                return -1;
            }
            snprintf(defaultUserName, sizeof(defaultUserName), "%s",
                     requestUserHint);
        } else {
            if (getLSFUser_(defaultUserName,
                            sizeof(defaultUserName)) != 0 ||
                defaultUserName[0] == '\0') {
                fprintf(stderr,
                        "bhist speed up: cannot resolve the calling Volclava user\n");
                return -1;
            }
        }
        userName = defaultUserName;
    }

    dbPath = getenv("LSB_BHIST_INTERNAL_DB_PATH");
    bhistPath = getenv(
        "LSB_BHIST_INTERNAL_BHIST_ORIGINAL_PATH");
    if (dbPath == NULL || dbPath[0] != '/' ||
        bhistPath == NULL || bhistPath[0] != '/') {
        fprintf(stderr,
                "bhist speed up: internal query environment is incomplete\n");
        return -1;
    }
    sql = malloc(QUERY_SQL_BUFFER_SIZE);
    if (sql == NULL) {
        fprintf(stderr, "bhist speed up: out of memory\n");
        return -1;
    }
    sql[0] = '\0';

    if (selectorCount == 0 &&
        buildCandidateEventTableRange(&submitFilter, &completeFilter,
                                     &candidateTableRange) != 0) {
        fprintf(stderr, "bhist speed up: failed to build event table range\n");
        free(sql);
        return -1;
    }
    if (sqliteOpenQueryDb(dbPath, &queryDb) != 0)
        goto queryFail;

    memset(&request, 0, sizeof(request));
    request.bhistPath = bhistPath;
    request.selector = selectorCount == 1 ? argv[optind] : NULL;
    request.longFormat = longFormat;
    request.wideFormat = wideFormat;
    request.stateFlags = bhistStateFlags;
    request.submitArg = submitFilter.enabled ? submitFilter.arg : NULL;
    request.dispatchArg = dispatchFilter.enabled ? dispatchFilter.arg : NULL;
    request.completeArg = completeFilter.enabled ? completeFilter.arg : NULL;
    request.userName = userName;
    request.queueName = queueName;
    request.hostName = hostName;
    request.workers = querySettings.formattersPerQuery;
    if (selectorCount == 1) {
        if (sqliteExecRows(queryDb, "BEGIN", NULL, NULL, NULL) != 0)
            goto queryFail;
        if (loadEventTables(queryDb, NULL, &eventTables) != 0) {
            fprintf(stderr, "bhist speed up: failed to load event tables\n");
            goto queryFail;
        }
        if (parseBaseJobId(argv[optind], &baseJobId) != 0) {
            fprintf(stderr, "bhist speed up: invalid job selector: %s\n",
                    argv[optind]);
            goto queryFail;
        }
        if (buildJobIdQuery(&eventTables, baseJobId, sql,
                            QUERY_SQL_BUFFER_SIZE) != 0) {
            fprintf(stderr, "bhist speed up: failed to build job query\n");
            goto queryFail;
        }
        expectedJobs = 1;
        request.expectedJobs = expectedJobs;
        if (sqliteRunBhistQuery(queryDb, sql, &request, NULL,
                                &found) != 0)
            goto queryFail;
    } else {
        if (sqliteExecRows(queryDb, "BEGIN", NULL, NULL, NULL) != 0)
            goto queryFail;
        if (loadEventTables(queryDb, &candidateTableRange,
                            &eventTables) != 0) {
            fprintf(stderr, "bhist speed up: failed to load event tables\n");
            goto queryFail;
        }
        if (buildCandidateJobsQuery(
                &submitFilter, &dispatchFilter, &completeFilter,
                userName, queueName, &eventTables,
                sql, QUERY_SQL_BUFFER_SIZE) != 0) {
            fprintf(stderr, "bhist speed up: failed to build candidate query\n");
            goto queryFail;
        }
        if (sqliteExecRows(queryDb, sql, NULL, NULL, NULL) != 0 ||
            sqliteScalarLongLong(
                queryDb,
                "SELECT COUNT(*) FROM " QUERY_TEMP_FILTERED_JOBS ";",
                &expectedJobs) != 0 ||
            sqliteExecRows(queryDb, "COMMIT", NULL, NULL, NULL) != 0)
            goto queryFail;
        freeEventTables(&eventTables);

        bhistOutputInit(&outputState);
        outputStateInitialized = true;
        lastSeq = 0;
        /*
         * Preserve the pipeline order:
         *   prepare next -> collect and print current -> start next.
         * Current bhist workers run while SQLite prepares the next batch, but
         * only the current batch is collected. Starting next before collection
         * would run two worker groups concurrently and break the resource limit.
         */
        for (;;) {
            int batchFound;
            int collectRc;

            if (sqliteExecRows(queryDb, "BEGIN", NULL, NULL, NULL) != 0)
                goto queryFail;
            if (buildBatchLoadSql(lastSeq,
                                  querySettings.jobsPerBatch,
                                  sql, QUERY_SQL_BUFFER_SIZE) != 0 ||
                sqliteExecRows(queryDb, sql, NULL, NULL, NULL) != 0 ||
                sqliteScalarLongLong(
                    queryDb,
                    "SELECT COUNT(*) FROM " QUERY_TEMP_BATCH_JOBS ";",
                    &batchJobs) != 0 ||
                sqliteScalarLongLong(
                    queryDb,
                    "SELECT COALESCE(MAX(seq), 0) FROM "
                    QUERY_TEMP_BATCH_JOBS ";",
                    &batchMaxSeq) != 0)
                goto queryFail;
            if (batchJobs == 0) {
                if (sqliteExecRows(queryDb, "COMMIT", NULL, NULL,
                                   NULL) != 0)
                    goto queryFail;
                if (currentStream != NULL) {
                    collectRc = bhistStreamCollectBatch(
                        currentStream, &outputState, &batchFound);
                    currentStream = NULL;
                    if (collectRc != 0)
                        goto queryFail;
                    if (fflush(stdout) != 0) {
                        perror("fflush");
                        goto queryFail;
                    }
                }
                break;
            }
            /* -C must see later state changes when bhist replays each job. */
            if (loadEventTables(queryDb,
                                completeFilter.enabled ? NULL :
                                    &candidateTableRange,
                                &eventTables) != 0) {
                fprintf(stderr, "bhist speed up: failed to reload event tables\n");
                goto queryFail;
            }
            if (buildBatchRawQuery(&eventTables, sql,
                                   QUERY_SQL_BUFFER_SIZE) != 0) {
                fprintf(stderr, "bhist speed up: failed to build batch query\n");
                goto queryFail;
            }
            request.expectedJobs = batchJobs;
            if (sqlitePrepareBhistQuery(queryDb, sql, &request,
                                       &nextStream) != 0)
                goto queryFail;
            freeEventTables(&eventTables);
            lastSeq = batchMaxSeq;
            if (currentStream != NULL) {
                collectRc = bhistStreamCollectBatch(
                    currentStream, &outputState, &batchFound);
                currentStream = NULL;
                if (collectRc != 0)
                    goto queryFail;
                if (fflush(stdout) != 0) {
                    perror("fflush");
                    goto queryFail;
                }
            }
            if (bhistStreamStartBatch(nextStream) != 0)
                goto queryFail;
            currentStream = nextStream;
            nextStream = NULL;
        }
        if (bhistOutputFinish(&outputState) != 0)
            goto queryFail;
        found = outputState.found;
        outputStateInitialized = false;
    }

    sqlite3_close(queryDb);
    queryDb = NULL;
    freeEventTables(&eventTables);
    free(sql);
    sql = NULL;

    if (!found) {
        fprintf(stdout, "No matching job found\n");
    }

    return 0;

queryFail:
    freeEventTables(&eventTables);
    if (queryDb != NULL && !sqlite3_get_autocommit(queryDb))
        sqliteExecRows(queryDb, "ROLLBACK", NULL, NULL, NULL);
    if (nextStream != NULL) {
        bhistStreamAbort(nextStream);
        nextStream = NULL;
    }
    if (currentStream != NULL) {
        int batchFound;

        (void)bhistStreamCollectBatch(
            currentStream, &outputState, &batchFound);
        currentStream = NULL;
    }
    if (outputStateInitialized) {
        (void)bhistOutputFinish(&outputState);
        if (outputState.found)
            fprintf(stderr,
                    "bhist speed up: query failed after partial output; "
                    "results are incomplete\n");
    }
    if (queryDb != NULL) {
        sqlite3_close(queryDb);
    }
    free(sql);
    return -1;
}

/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 */

#include "bhist.loader.h"
#include "bhist.service.h"
#include "bhist.config.h"
#include "bhist.version.h"
#include "bhist.util.h"
#include "lproto.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/*
 * High-level loader state machine and database publication policy.
 *
 * Without a final database, the loader imports into <db>.cbootstrap.tmp,
 * catches up to the live boundary, builds indexes, runs ANALYZE, atomically
 * renames the file into place, and continues live follow in WAL/NORMAL mode.
 * A compatible final database resumes from the committed loader_progress
 * offset. Rebuild explicitly removes the final database and sidecars before
 * repeating the initial-build path.
 *
 * An interrupted bootstrap database is discarded on the next initial start.
 * No incompatible final database is migrated or deleted implicitly.
 * runImportLoop() consumes plan/import/store results and replans after
 * recoverable rotation.
 */

#define LOADER_LIVE_WATCH_INTERVAL_MS 1000
#define LOADER_STALE_MAX_RETRIES 10

/* Holds loader-only configuration in the units consumed by the state machine. */
struct loaderConfig {
    int pollIntervalMs;
    int dbBusyTimeoutMs;
    int importCacheSizeKib;
    long long historyRetentionSeconds;
    long long importTxMaxLines;
    long long importTxMaxRawBytes;
    char logdir[LOADER_PATH_MAX];
    char dbPath[LOADER_PATH_MAX];
    char runtimeLogdir[LOADER_PATH_MAX];
    char logMask[64];
    char clusterName[128];
};

/* Owns the stable identity and last size observed by one live-file watcher. */
struct loaderLiveWatchContext {
    char path[LOADER_PATH_MAX];
    ino_t fileIno;
    off_t lastSize;
};

/* Set once when a watcher sees an in-place live-file size regression. */
static int loaderLiveSizeRegressed;

static int initLoaderConfig(const struct config *shared,
                            struct loaderConfig *config);

/*
 * Count a stale snapshot and delay before another bounded retry.
 * @param[in,out] staleRetries: Consecutive stale-snapshot count.
 * @param[in] pollIntervalMs: Delay between attempts in milliseconds.
 * @return: 0 while retries remain, otherwise -1 after the fixed limit.
 */
static int
retryStaleSnapshot(int *staleRetries, int pollIntervalMs)
{
    (*staleRetries)++;
    if (*staleRetries > LOADER_STALE_MAX_RETRIES) {
        ls_syslog(LOG_ERR,
                "stale_retry_limit_exceeded retries=%d limit=%d",
                *staleRetries - 1, LOADER_STALE_MAX_RETRIES);
        return -1;
    }
    sleepMs(pollIntervalMs);
    return 0;
}

/*
 * Monitor one live generation for a file-size regression.
 * @param[in] opaque: Owned loaderLiveWatchContext.
 * @return: Always NULL.
 */
static void *
watchLiveSize(void *opaque)
{
    struct loaderLiveWatchContext *context;

    context = (struct loaderLiveWatchContext *)opaque;
    for (;;) {
        struct stat st;

        sleepMs(LOADER_LIVE_WATCH_INTERVAL_MS);
        if (stat(context->path, &st) != 0)
            break;
        if (st.st_ino != context->fileIno)
            break;
        if (st.st_size < context->lastSize) {
            __atomic_store_n(&loaderLiveSizeRegressed, 1, __ATOMIC_RELEASE);
            break;
        }
        context->lastSize = st.st_size;
    }
    free(context);
    return NULL;
}

/*
 * Start background size monitoring for a live source.
 * @param[in] plan: Planned live source.
 * @return: 1 when started, 0 when the generation already changed, otherwise -1.
 */
static int
startLiveSizeWatch(const struct loaderSourcePlan *plan)
{
    struct loaderLiveWatchContext *context;
    pthread_t thread;
    char fileIno[32];
    struct stat st;
    int rc;

    if (plan == NULL || plan->kind != LOADER_SOURCE_LIVE_CURRENT)
        return -1;
    if (stat(plan->path, &st) != 0)
        return 0;
    snprintf(fileIno, sizeof(fileIno), "%llu",
             (unsigned long long)st.st_ino);
    if (strcmp(fileIno, plan->fileIno) != 0)
        return 0;
    if ((long long)st.st_size <
        plan->payloadStartOff + plan->currentOffset) {
        __atomic_store_n(&loaderLiveSizeRegressed, 1, __ATOMIC_RELEASE);
        return 1;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        ls_syslog(LOG_ERR, "event_file_monitor_start_failed: %m");
        return -1;
    }
    snprintf(context->path, sizeof(context->path), "%s", plan->path);
    context->fileIno = st.st_ino;
    context->lastSize = st.st_size;
    rc = pthread_create(&thread, NULL, watchLiveSize, context);
    if (rc != 0) {
        ls_syslog(LOG_ERR, "event_file_monitor_start_failed error=%s",
                strerror(rc));
        free(context);
        return -1;
    }
    rc = pthread_detach(thread);
    if (rc != 0) {
        ls_syslog(LOG_ERR, "event_file_monitor_start_failed error=%s",
                strerror(rc));
        return -1;
    }
    return 1;
}

/*
 * Remove one loader-owned file while accepting an already absent path.
 * @param[in] path: File path to unlink.
 * @return: 0 when removed or absent, otherwise -1.
 */
static int
unlinkIfExists(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT)
        return 0;
    ls_syslog(LOG_ERR, "unlink_failed file=%s: %m", path);
    return -1;
}

/*
 * Remove a database and its rollback-journal, WAL and shared-memory sidecars.
 * @param[in] dbPath: Main SQLite database path.
 * @return: 0 on success, otherwise -1.
 */
static int
removeDbFiles(const char *dbPath)
{
    char sidecar[LOADER_PATH_MAX + 16];

    if (unlinkIfExists(dbPath) != 0)
        return -1;
    snprintf(sidecar, sizeof(sidecar), "%s-journal", dbPath);
    if (unlinkIfExists(sidecar) != 0)
        return -1;
    snprintf(sidecar, sizeof(sidecar), "%s-wal", dbPath);
    if (unlinkIfExists(sidecar) != 0)
        return -1;
    snprintf(sidecar, sizeof(sidecar), "%s-shm", dbPath);
    return unlinkIfExists(sidecar);
}

/*
 * Build the bootstrap database path for a final database.
 * @param[in] dbPath: Final SQLite database path.
 * @param[out] out: Buffer receiving the bootstrap path.
 * @param[in] outLen: Size of out.
 * @return: Nonzero when the path fits, otherwise zero.
 */
static int
buildWorkDbPath(const char *dbPath, char *out, size_t outLen)
{
    int written;

    written = snprintf(out, outLen, "%s.cbootstrap.tmp", dbPath);
    return written >= 0 && (size_t)written < outLen;
}

/*
 * Select unsafe but fast SQLite settings for an unpublished bootstrap build.
 * @param[in] config: Loader configuration containing the import cache size.
 * @return: None.
 */
static void
configureSqliteForBulkImport(const struct loaderConfig *config)
{
    loaderStoreConfigureSqlite(
        "DELETE", "OFF", "OFF", "EXCLUSIVE",
        LOADER_SQLITE_PAGE_SIZE_BYTES,
        config->importCacheSizeKib, 0);
}

/*
 * Select WAL/NORMAL SQLite settings for a published live database.
 * @param[in] config: Loader configuration containing the import cache size.
 * @return: None.
 */
static void
configureSqliteForLiveImport(const struct loaderConfig *config)
{
    loaderStoreConfigureSqlite(
        "WAL", "NORMAL", "", "", 0, config->importCacheSizeKib,
        1000);
}

/*
 * Atomically publish a completed bootstrap database at the final path.
 * @param[in] workDbPath: Completed bootstrap database path.
 * @param[in] dbPath: Final database path.
 * @return: 0 on success, otherwise -1.
 */
static int
publishWorkDb(const char *workDbPath,
              const char *dbPath)
{
    if (removeDbFiles(dbPath) != 0)
        return -1;
    if (rename(workDbPath, dbPath) != 0) {
        ls_syslog(LOG_ERR, "database_replace_failed from=%s to=%s: %m",
                  workDbPath, dbPath);
        return -1;
    }
    return 0;
}

/*
 * Build final query indexes and ANALYZE an unpublished bootstrap database.
 * @param[in] workDbPath: Bootstrap database path.
 * @param[in] config: Loader configuration containing SQLite timeout settings.
 * @return: 0 on success, otherwise -1.
 */
static int
buildWorkDbIndexes(const char *workDbPath,
                   const struct loaderConfig *config)
{
    sqlite3 *db;
    time_t startedAt;

    startedAt = time(NULL);
    ls_syslog(LOG_INFO, "index_build_start db=%s", config->dbPath);

    db = NULL;
    if (loaderStoreOpen(workDbPath, config->dbBusyTimeoutMs,
                          &db) != 0) {
        ls_syslog(LOG_ERR, "index_build_open_failed db=%s", config->dbPath);
        return -1;
    }
    if (loaderStoreCreateIndexes(db) != 0 ||
        loaderStoreAnalyze(db) != 0) {
        loaderStoreClose(db);
        ls_syslog(LOG_ERR, "index_build_failed db=%s", config->dbPath);
        return -1;
    }
    loaderStoreClose(db);

    ls_syslog(LOG_INFO, "index_build_done db=%s elapsed_sec=%ld",
              config->dbPath, (long)(time(NULL) - startedAt));
    return 0;
}

/*
 * Create all loader tables, metadata, and the singleton progress row.
 * @param[in] db: Writable SQLite connection for a new database.
 * @param[in] config: Loader runtime configuration carrying the cluster name.
 * @return: 0 on success, otherwise -1 with the transaction rolled back.
 */
static int
initDb(sqlite3 *db, const struct loaderConfig *config)
{
    if (loaderStoreBegin(db) != 0)
        return -1;
    if (loaderStoreInitTables(db) != 0 ||
        loaderStoreInitMeta(db, config->clusterName) != 0 ||
        loaderStoreInitProgress(db) != 0) {
        loaderStoreRollback(db);
        return -1;
    }
    if (loaderStoreCommit(db) != 0)
        return -1;
    return 0;
}

/*
 * Create and initialize a new database.
 * @param[in] dbPath: Database path to create.
 * @param[in] config: Loader SQLite settings.
 * @return: 0 on success, otherwise -1.
 */
static int
createDb(const char *dbPath, const struct loaderConfig *config)
{
    sqlite3 *db;

    db = NULL;
    if (loaderStoreOpen(dbPath, config->dbBusyTimeoutMs,
                          &db) != 0)
        return -1;
    if (initDb(db, config) != 0) {
        loaderStoreClose(db);
        return -1;
    }
    loaderStoreClose(db);
    return 0;
}

/*
 * Repeatedly plan and import archives and live events into one database.
 * @param[in] logdir: Directory containing lsb.events generations.
 * @param[in] dbPath: SQLite database receiving imported events.
 * @param[in] config: Loader polling, retention, and SQLite settings.
 * @param[in] stopAfterLiveOpen: Nonzero to stop once bootstrap reaches live.
 * @return: 0 on the requested bootstrap boundary,
 *          SERVICE_EXIT_CLEAN_REQUIRED when the database must be rebuilt,
 *          otherwise SERVICE_EXIT_FAILURE.
 */
static int
runImportLoop(const char *logdir,
              const char *dbPath,
              const struct loaderConfig *config,
              int stopAfterLiveOpen)
{
    sqlite3 *db;
    struct loaderLiveHandle live;
    struct loaderSourcePlan livePlan;
    struct loaderArchiveQueue archiveQueue;
    int staleRetries;
    int haveLivePlan;
    int exitStatus;
    long long watchedLiveSourceId;

    exitStatus = SERVICE_EXIT_FAILURE;
    db = NULL;
    memset(&live, 0, sizeof(live));
    memset(&livePlan, 0, sizeof(livePlan));
    memset(&archiveQueue, 0, sizeof(archiveQueue));
    staleRetries = 0;
    haveLivePlan = 0;
    watchedLiveSourceId = 0;
    if (loaderStoreOpen(dbPath, config->dbBusyTimeoutMs,
                          &db) != 0) {
        return 1;
    }

    /*
     * Preserve this priority: open live handle, planned archive queue, then a
     * fresh scan and plan. This follows current without skipping known archives.
     * Recoverable rotation returns STALE_REPLAN; only FATAL or
     * REBUILD_REQUIRED terminates the loop.
     */
    for (;;) {
        struct loaderSourcePlan plan;
        struct loaderImportStats stats;
        enum loaderStepResult planRc;
        enum loaderStepResult stepRc;
        long long planTime;

        if (__atomic_load_n(&loaderLiveSizeRegressed,
                            __ATOMIC_ACQUIRE) != 0) {
            ls_syslog(LOG_ERR,
                  "event_file_shrunk; run bhist-admin loader clean, then start "
                  "the loader again");
            goto rebuildRequired;
        }

        if (live.fp != NULL && haveLivePlan) {
            stepRc = loaderImportLiveUntilEof(
                db, &livePlan, &live, &stats);
            if (stats.inputLines > 0 || stepRc == LOADER_STEP_SOURCE_DONE)
                ls_syslog(LOG_INFO,
                          "live_eof_commit file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld",
                          stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines);
            if (stepRc == LOADER_STEP_COMMITTED) {
                staleRetries = 0;
                livePlan.currentOffset = live.currentOffset;
                livePlan.seenRowCount = live.seenRowCount;
                continue;
            }
            if (stepRc == LOADER_STEP_SOURCE_DONE) {
                staleRetries = 0;
                haveLivePlan = 0;
                memset(&livePlan, 0, sizeof(livePlan));
                continue;
            }
            if (stepRc == LOADER_STEP_IDLE) {
                staleRetries = 0;
                sleepMs(config->pollIntervalMs);
                continue;
            }
            if (stepRc == LOADER_STEP_STALE_REPLAN) {
                loaderLiveHandleClose(&live);
                haveLivePlan = 0;
                memset(&livePlan, 0, sizeof(livePlan));
                if (retryStaleSnapshot(
                    &staleRetries, config->pollIntervalMs) != 0)
                    goto fail;
                continue;
            }
            if (stepRc == LOADER_STEP_REBUILD_REQUIRED)
                goto rebuildRequired;
            goto fail;
        }

        if (archiveQueue.pos < archiveQueue.count) {
            plan = archiveQueue.plans[archiveQueue.pos];
            if (loaderPlanBeginSource(db, &plan, (long long)time(NULL)) != 0)
                goto fail;
            archiveQueue.pos += 1;
            stepRc = loaderImportArchiveSource(
                db, &plan, &stats);
            if (stats.inputLines > 0 || stepRc == LOADER_STEP_SOURCE_DONE)
                ls_syslog(LOG_INFO,
                          "archive_source_commit file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld",
                          stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines);
            if (stepRc == LOADER_STEP_COMMITTED ||
                stepRc == LOADER_STEP_SOURCE_DONE) {
                staleRetries = 0;
                continue;
            }
            loaderPlanFreeArchiveQueue(&archiveQueue);
            if (stepRc == LOADER_STEP_IDLE) {
                staleRetries = 0;
                sleepMs(config->pollIntervalMs);
                continue;
            }
            if (stepRc == LOADER_STEP_STALE_REPLAN) {
                if (retryStaleSnapshot(
                    &staleRetries, config->pollIntervalMs) != 0)
                    goto fail;
                continue;
            }
            if (stepRc == LOADER_STEP_REBUILD_REQUIRED)
                goto rebuildRequired;
            goto fail;
        }
        if (archiveQueue.count > 0)
            loaderPlanFreeArchiveQueue(&archiveQueue);

        planRc = loaderPlanArchiveQueue(
            logdir, db, config->historyRetentionSeconds,
            &archiveQueue);
        if (planRc == LOADER_STEP_COMMITTED) {
            ls_syslog(LOG_INFO,
                      "archive_plan_ready planned_archives=%zu "
                      "archive_range=%ld..%ld completed_sources=%lld "
                      "max_archive=%ld",
                      archiveQueue.count, archiveQueue.firstArchiveIndex,
                      archiveQueue.lastArchiveIndex,
                      archiveQueue.completedSources,
                      archiveQueue.maxArchiveIndex);
            continue;
        }
        if (planRc == LOADER_STEP_STALE_REPLAN) {
            if (retryStaleSnapshot(
                &staleRetries, config->pollIntervalMs) != 0)
                goto fail;
            continue;
        }
        if (planRc == LOADER_STEP_REBUILD_REQUIRED)
            goto rebuildRequired;
        if (planRc == LOADER_STEP_FATAL) {
            goto fail;
        }

        planTime = (long long)time(NULL);
        planRc = loaderPlanNextSource(
            logdir, db, config->historyRetentionSeconds,
            planTime, &plan);
        if (planRc == LOADER_STEP_STALE_REPLAN) {
            if (retryStaleSnapshot(
                &staleRetries, config->pollIntervalMs) != 0)
                goto fail;
            continue;
        }
        if (planRc == LOADER_STEP_REBUILD_REQUIRED)
            goto rebuildRequired;
        if (planRc == LOADER_STEP_FATAL) {
            goto fail;
        }
        if (plan.kind == LOADER_SOURCE_LIVE_CURRENT) {
            stepRc = loaderImportRefreshRetainedHead(
                db, &plan, config->historyRetentionSeconds,
                planTime);
            if (stepRc == LOADER_STEP_STALE_REPLAN) {
                if (retryStaleSnapshot(
                    &staleRetries, config->pollIntervalMs) != 0)
                    goto fail;
                continue;
            }
            if (stepRc == LOADER_STEP_REBUILD_REQUIRED)
                goto rebuildRequired;
            if (stepRc == LOADER_STEP_FATAL)
                goto fail;
        }
        if (stopAfterLiveOpen && plan.kind == LOADER_SOURCE_LIVE_CURRENT) {
            loaderLiveHandleClose(&live);
            loaderStoreClose(db);
            return 0;
        }
        if (plan.kind == LOADER_SOURCE_ARCHIVE) {
            stepRc = loaderImportArchiveSource(
                db, &plan, &stats);
            if (stats.inputLines > 0 || stepRc == LOADER_STEP_SOURCE_DONE)
                ls_syslog(LOG_INFO,
                          "archive_source_commit file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld",
                          stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines);
        } else if (plan.kind == LOADER_SOURCE_LIVE_CURRENT) {
            if (watchedLiveSourceId != plan.sourceId) {
                int watchRc;

                watchRc = startLiveSizeWatch(&plan);
                if (watchRc < 0)
                    goto fail;
                if (watchRc > 0)
                    watchedLiveSourceId = plan.sourceId;
            }
            livePlan = plan;
            haveLivePlan = 1;
            stepRc = loaderImportLiveUntilEof(
                db, &livePlan, &live, &stats);
            if (stats.inputLines > 0 || stepRc == LOADER_STEP_SOURCE_DONE)
                ls_syslog(LOG_INFO,
                          "live_eof_commit file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld",
                          stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines);
        } else {
            stepRc = LOADER_STEP_IDLE;
        }

        if (stepRc == LOADER_STEP_COMMITTED) {
            staleRetries = 0;
            if (live.fp != NULL && haveLivePlan) {
                livePlan.currentOffset = live.currentOffset;
                livePlan.seenRowCount = live.seenRowCount;
            }
            continue;
        }
        if (stepRc == LOADER_STEP_SOURCE_DONE) {
            staleRetries = 0;
            if (live.fp == NULL) {
                haveLivePlan = 0;
                memset(&livePlan, 0, sizeof(livePlan));
            }
            continue;
        }
        if (stepRc == LOADER_STEP_IDLE) {
            staleRetries = 0;
            sleepMs(config->pollIntervalMs);
            continue;
        }
        if (stepRc == LOADER_STEP_STALE_REPLAN) {
            loaderLiveHandleClose(&live);
            haveLivePlan = 0;
            memset(&livePlan, 0, sizeof(livePlan));
            if (retryStaleSnapshot(
                &staleRetries, config->pollIntervalMs) != 0)
                goto fail;
            continue;
        }
        if (stepRc == LOADER_STEP_REBUILD_REQUIRED)
            goto rebuildRequired;
        goto fail;
    }

rebuildRequired:
    if (loaderStoreMarkRebuildRequired(db) != 0)
        ls_syslog(LOG_ERR, "persist_rebuild_required_failed");
    else
        ls_syslog(LOG_ERR, "database_status_changed status=rebuild_required");
    exitStatus = SERVICE_EXIT_CLEAN_REQUIRED;
fail:
    ls_syslog(LOG_ERR, "loader_failed");
    loaderPlanFreeArchiveQueue(&archiveQueue);
    loaderLiveHandleClose(&live);
    loaderStoreClose(db);
    return exitStatus;
}

/*
 * Build a fresh bootstrap database, publish it, then enter live follow.
 * @param[in] config: Loader runtime configuration.
 * @return: 0 on success, otherwise nonzero.
 */
static int
buildAndFollow(const struct loaderConfig *config)
{
    char workDbPath[LOADER_PATH_MAX];
    int result;

    configureSqliteForBulkImport(config);
    if (!buildWorkDbPath(config->dbPath, workDbPath,
                         sizeof(workDbPath))) {
        ls_syslog(LOG_ERR, "database_build_prepare_failed db=%s",
                  config->dbPath);
        return 1;
    }
    if (removeDbFiles(workDbPath) != 0) {
        ls_syslog(LOG_ERR, "database_build_prepare_failed db=%s",
                  config->dbPath);
        return 1;
    }
    if (createDb(workDbPath, config) != 0) {
        ls_syslog(LOG_ERR, "database_build_init_failed db=%s",
                  config->dbPath);
        return 1;
    }
    if (loaderStoreValidateSchema(workDbPath,
                                  config->dbBusyTimeoutMs) != 0)
        return 1;
    ls_syslog(LOG_INFO, "database_build_start db=%s logdir=%s",
              config->dbPath, config->logdir);
    result = runImportLoop(config->logdir, workDbPath, config, 1);
    if (result != 0)
        return result;
    if (buildWorkDbIndexes(workDbPath, config) != 0)
        return 1;
    if (publishWorkDb(workDbPath, config->dbPath) != 0) {
        ls_syslog(LOG_ERR, "database_build_publish_failed db=%s",
                  config->dbPath);
        return 1;
    }
    ls_syslog(LOG_INFO, "database_build_done db=%s", config->dbPath);
    configureSqliteForLiveImport(config);
    return runImportLoop(config->logdir, config->dbPath, config, 0);
}

/*
 * Resume a compatible final database, or build one when none exists.
 * An existing database is never deleted here; that is the clean command.
 * @param[in] config: Loader runtime configuration.
 * @return: 0 on success, otherwise a SERVICE_EXIT_* status.
 */
static int
startImport(const struct loaderConfig *config)
{
    if (access(config->dbPath, F_OK) == 0) {
        if (loaderStoreValidateSchema(config->dbPath,
                                      config->dbBusyTimeoutMs) != 0)
            return SERVICE_EXIT_CLEAN_REQUIRED;
        configureSqliteForLiveImport(config);
        return runImportLoop(config->logdir, config->dbPath, config, 0);
    }
    /* First start always rebuilds the unpublished bootstrap database. */
    return buildAndFollow(config);
}

/*
 * Validate the final database schema and bootstrap path before starting.
 * @param[in] sharedConfig: Loaded loader configuration.
 * @return: 0 when valid, otherwise nonzero.
 */
static int
validateStartDatabase(const struct config *sharedConfig)
{
    struct loaderConfig config;
    char workDbPath[LOADER_PATH_MAX];

    if (initLoaderConfig(sharedConfig, &config) != 0)
        return 1;
    ls_openlog("bhist-speedup-loader", config.runtimeLogdir, 1,
                 config.logMask);
    if (access(config.dbPath, F_OK) == 0)
        return loaderStoreValidateSchema(config.dbPath,
                                          config.dbBusyTimeoutMs) == 0 ?
            0 : 1;
    if (errno != ENOENT) {
        ls_syslog(LOG_ERR, "schema_check_access_failed db=%s: %m",
                 config.dbPath);
        return 1;
    }
    if (!buildWorkDbPath(config.dbPath, workDbPath,
                         sizeof(workDbPath))) {
        ls_syslog(LOG_ERR, "database_build_prepare_failed db=%s",
                  config.dbPath);
        return 1;
    }
    return 0;
}

/*
 * Print configured paths, database health, and tracked event rows.
 * @param[in] config: Loader runtime configuration.
 * @return: 0 when status was read or the database is absent, otherwise 1.
 */
static int
printDbStatus(const struct loaderConfig *config)
{
    char dbStatus[32];
    char clusterName[128];
    char createdAt[32];
    sqlite3 *db;
    struct loaderProgressRow progress;
    int metaResult;

    fprintf(stdout, "db=%s\n", config->dbPath);
    fprintf(stdout, "eventsdir=%s\n", config->logdir);
    if (access(config->dbPath, F_OK) != 0) {
        fprintf(stdout, "db_status=missing\n");
        return 0;
    }
    db = NULL;
    if (loaderStoreOpenReadonly(config->dbPath,
                                   config->dbBusyTimeoutMs,
                                   &db) != 0) {
        fprintf(stdout, "db_status=error\n");
        fprintf(stderr, "bhist-speedup-loader: cannot open database %s\n",
                config->dbPath);
        return 1;
    }
    if (loaderStoreLoadProgress(db, &progress) != 0) {
        fprintf(stdout, "db_status=error\n");
        fprintf(stderr, "bhist-speedup-loader: cannot read progress from %s\n",
                config->dbPath);
        loaderStoreClose(db);
        return 1;
    }
    if (loaderStoreGetDbStatus(db, dbStatus, sizeof(dbStatus)) != 0) {
        fprintf(stdout, "db_status=error\n");
        fprintf(stderr, "bhist-speedup-loader: cannot read status from %s\n",
                config->dbPath);
        loaderStoreClose(db);
        return 1;
    }
    fprintf(stdout, "db_status=%s\n", dbStatus);
    /* Databases created before these keys existed simply skip the lines. */
    metaResult = loaderStoreGetMeta(db, "cluster_name",
                                    clusterName, sizeof(clusterName));
    if (metaResult == 0)
        fprintf(stdout, "cluster=%s\n", clusterName);
    else if (metaResult < 0)
        fprintf(stdout, "cluster=unknown\n");
    metaResult = loaderStoreGetMeta(db, "created_at",
                                    createdAt, sizeof(createdAt));
    if (metaResult == 0)
        fprintf(stdout, "created=%s\n", createdAt);
    else if (metaResult < 0)
        fprintf(stdout, "created=unknown\n");
    fprintf(stdout, "event_rows_tracked=%lld\n", progress.totalSeenRowCount);
    loaderStoreClose(db);
    return 0;
}

/*
 * Convert shared role-filtered configuration into loader runtime units.
 * @param[in] shared: Shared configuration loaded for CONFIG_ROLE_LOADER.
 * @param[out] config: Loader-specific configuration.
 * @return: 0 on success, otherwise -1.
 */
static int
initLoaderConfig(const struct config *shared,
                 struct loaderConfig *config)
{
    if (shared == NULL || config == NULL)
        return -1;
    memset(config, 0, sizeof(*config));
    config->pollIntervalMs = shared->eventPollIntervalMs;
    config->dbBusyTimeoutMs = shared->dbBusyTimeoutMs;
    config->importCacheSizeKib =
        -(shared->importCacheSizeMib * 1024);
    config->historyRetentionSeconds = shared->historyRetentionSeconds;
    config->importTxMaxLines = shared->maxEventsPerTransaction;
    config->importTxMaxRawBytes =
        shared->maxRawSizePerTransactionMib * 1024LL * 1024LL;
    if (snprintf(config->logdir, sizeof(config->logdir), "%s",
                 shared->eventDir) >= (int)sizeof(config->logdir) ||
        snprintf(config->dbPath, sizeof(config->dbPath), "%s",
                 shared->dbPath) >= (int)sizeof(config->dbPath) ||
        snprintf(config->runtimeLogdir, sizeof(config->runtimeLogdir),
                 "%s", shared->logDir) >=
            (int)sizeof(config->runtimeLogdir) ||
        snprintf(config->logMask, sizeof(config->logMask), "%s",
                 shared->logMask) >= (int)sizeof(config->logMask) ||
        (shared->clusterName != NULL &&
         snprintf(config->clusterName, sizeof(config->clusterName), "%s",
                  shared->clusterName) >= (int)sizeof(config->clusterName))) {
        ls_syslog(LOG_ERR, "configured path is too long");
        return -1;
    }
    loaderImportConfigureChunkLimits(config->importTxMaxLines,
                                         config->importTxMaxRawBytes);
    return 0;
}

/*
 * Switch to file logging, report readiness, and run the import loop.
 * @param[in] sharedConfig: Loaded loader configuration.
 * @param[in] logPath: Runtime log file path.
 * @return: 0 on success, otherwise a SERVICE_EXIT_* status.
 */
static int
runImport(const struct config *sharedConfig, const char *logPath)
{
    struct loaderConfig config;
    int result;

    if (initLoaderConfig(sharedConfig, &config) != 0)
        return SERVICE_EXIT_CONFIG;
    /* Everything after this goes to the log file, so say where it is. */
    fprintf(stdout, "bhist-speedup-loader: running (pid %ld); log: %s\n",
            (long)getpid(), logPath);
    (void)fflush(stdout);
    ls_openlog("bhist-speedup-loader", config.runtimeLogdir, 0,
                 config.logMask);
    /*
     * Ready once configuration and the database are validated; an initial
     * import can take far longer than any reasonable start timeout.
     */
    serviceNotify("READY=1");
    result = startImport(&config);
    /*
     * Details are in the log file. Repeat the outcome on standard error so
     * it also appears in systemctl status or on the terminal.
     */
    if (result == SERVICE_EXIT_CLEAN_REQUIRED)
        fprintf(stderr, "bhist-speedup-loader: the history database must be "
                "rebuilt; run bhist-admin loader clean, then start the "
                "loader again\n");
    else if (result != 0)
        fprintf(stderr, "bhist-speedup-loader: stopped after an error; "
                "see %s\n", logPath);
    return result;
}

/*
 * Print loader database health and import progress.
 * @param[in] sharedConfig: Loaded loader configuration.
 * @return: 0 on success, otherwise nonzero.
 */
static int
printStatus(const struct config *sharedConfig)
{
    struct loaderConfig config;

    if (initLoaderConfig(sharedConfig, &config) != 0)
        return 1;
    ls_openlog("bhist-speedup-loader", config.runtimeLogdir, 1,
                 config.logMask);
    return printDbStatus(&config);
}

/* Loader command implementation. */

/*
 * Print loader command syntax.
 * @param[out] stream: Destination stream.
 * @return: None.
 */
static void
usage(FILE *stream)
{
    fputs("Usage: bhist-speedup-loader            run the loader in the foreground\n"
          "       bhist-speedup-loader status     show loader state and database progress\n"
          "       bhist-speedup-loader clean      remove the database while the loader is stopped\n",
          stream);
}

/*
 * Validate and create local directories required by the loader.
 * @param[in] config: Loader configuration.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 when directories are usable, otherwise -1.
 */
static int
validateStartDirectories(const struct config *config,
                           char *error, size_t errorSize)
{
    char databaseDirectory[CONFIG_PATH_MAX];
    char reason[512];
    struct stat st;

    if (config->eventDir[0] == '\0') {
        snprintf(error, errorSize, "%.400s: LSB_BHIST_EVENT_DIR is required",
                 config->configPath);
        return -1;
    }
    if (stat(config->eventDir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(config->eventDir, R_OK | X_OK) != 0) {
        snprintf(error, errorSize,
                 "%.200s: LSB_BHIST_EVENT_DIR (%.200s) is not a readable "
                 "directory",
                 config->configPath, config->eventDir);
        return -1;
    }
    if (parentDirectory(config->dbPath, databaseDirectory,
                             sizeof(databaseDirectory),
                             error, errorSize) != 0)
        return -1;
    if (ensureDirectory(databaseDirectory, reason, sizeof(reason)) != 0) {
        snprintf(error, errorSize,
                 "%.200s: cannot create the directory of LSB_BHIST_DB_PATH "
                 "(%.200s); start the loader once as root to create it",
                 config->configPath, reason);
        return -1;
    }
    if (stat(databaseDirectory, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(databaseDirectory, W_OK | X_OK) != 0) {
        snprintf(error, errorSize,
                 "%.200s: the directory of LSB_BHIST_DB_PATH (%.200s) is not "
                 "writable; start the loader once as root to fix its ownership",
                 config->configPath, databaseDirectory);
        return -1;
    }
    if (stat(config->logDir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(config->logDir, W_OK | X_OK) != 0) {
        snprintf(error, errorSize,
                 "%.200s/lsf.conf: LSF_LOGDIR (%.200s) is not a writable "
                 "directory",
                 config->envDir, config->logDir);
        return -1;
    }
    return 0;
}

/*
 * Run the loader service in the foreground until it fails or is signaled.
 * @param[in] config: Loader configuration.
 * @param[in] callerUid: Real UID saved at program entry.
 * @param[in] logPath: Runtime log file path.
 * @return: SERVICE_EXIT_* status.
 */
static int
runLoader(const struct config *config, uid_t callerUid, const char *logPath)
{
    char error[512];
    pid_t pid;
    int result;

    result = findServiceProcess("bhist-speedup-loader", &pid,
                                error, sizeof(error));
    if (result < 0) {
        ls_syslog(LOG_ERR, "%s", error);
        return SERVICE_EXIT_FAILURE;
    }
    if (result > 0) {
        ls_syslog(LOG_ERR, "the loader is already running (pid %ld)",
                  (long)pid);
        return SERVICE_EXIT_RUNNING;
    }
    (void)umask(022);
    if (chdir("/") != 0) {
        ls_syslog(LOG_ERR, "cannot change directory to /: %m");
        return SERVICE_EXIT_FAILURE;
    }
    /*
     * While still root, create the database directory and log file, and
     * hand root-owned leftovers to the service administrator, so the
     * switched process never fails on paths it cannot write.
     */
    if (callerUid == 0 &&
        prepareServicePaths(config->dbPath, logPath, config->admins[0],
                            error, sizeof(error)) != 0) {
        ls_syslog(LOG_ERR, "%s", error);
        return SERVICE_EXIT_CONFIG;
    }
    /* The service always runs as the first administrator. */
    if (switchServiceUser(config->admins[0], error, sizeof(error)) != 0 ||
        validateStartDirectories(config, error, sizeof(error)) != 0) {
        ls_syslog(LOG_ERR, "%s", error);
        return SERVICE_EXIT_CONFIG;
    }
    if (validateStartDatabase(config) != 0)
        return SERVICE_EXIT_CLEAN_REQUIRED;
    return runImport(config, logPath);
}

/*
 * Print loader process state, log file, and database import progress.
 * @param[in] config: Loader configuration.
 * @param[in] logPath: Runtime log file path.
 * @return: Database status result when running, otherwise 1.
 */
static int
showStatus(const struct config *config, const char *logPath)
{
    char error[512];
    pid_t pid;
    int result;
    int metadataResult;

    result = findServiceProcess("bhist-speedup-loader", &pid,
                                error, sizeof(error));
    if (result < 0) {
        ls_syslog(LOG_ERR, "%s", error);
        return 1;
    }
    if (result > 0) {
        fprintf(stdout, "state=running\n");
        fprintf(stdout, "pid=%ld\n", (long)pid);
    } else {
        fprintf(stdout, "state=stopped\n");
    }
    fprintf(stdout, "log=%s\n", logPath);
    metadataResult = printStatus(config);
    return result > 0 ? metadataResult : 1;
}

/*
 * Remove the final and bootstrap databases so the next start rebuilds them
 * from the retained event files.
 * @param[in] sharedConfig: Loaded loader configuration.
 * @return: 0 on success, otherwise a SERVICE_EXIT_* status.
 */
static int
cleanDatabase(const struct config *sharedConfig)
{
    struct loaderConfig config;
    char workDbPath[LOADER_PATH_MAX];
    char error[512];
    pid_t pid;
    int result;

    result = findServiceProcess("bhist-speedup-loader", &pid,
                                error, sizeof(error));
    if (result < 0) {
        ls_syslog(LOG_ERR, "%s", error);
        return SERVICE_EXIT_FAILURE;
    }
    if (result > 0) {
        ls_syslog(LOG_ERR, "the loader is running (pid %ld); stop it "
                  "first with: bhist-admin loader stop", (long)pid);
        return SERVICE_EXIT_RUNNING;
    }
    if (initLoaderConfig(sharedConfig, &config) != 0)
        return SERVICE_EXIT_CONFIG;
    if (!buildWorkDbPath(config.dbPath, workDbPath, sizeof(workDbPath))) {
        ls_syslog(LOG_ERR, "database path is too long: %s", config.dbPath);
        return SERVICE_EXIT_CONFIG;
    }
    if (removeDbFiles(config.dbPath) != 0 ||
        removeDbFiles(workDbPath) != 0) {
        ls_syslog(LOG_ERR, "cannot remove the database %s", config.dbPath);
        return SERVICE_EXIT_FAILURE;
    }
    fprintf(stdout, "cleaned db=%s\n", config.dbPath);
    return 0;
}

/*
 * Run the loader, or dispatch the status and clean commands.
 * @param[in] argc: Number of command-line arguments.
 * @param[in] argv: Command-line argument vector.
 * @return: 0 on success, otherwise a SERVICE_EXIT_* status.
 */
int
main(int argc, char **argv)
{
    struct config config;
    char error[512];
    char logPath[CONFIG_PATH_MAX];
    const char *command;
    uid_t callerUid = getuid();
    int result = SERVICE_EXIT_CONFIG;

    /*
     * Terminal fallback until configuration provides the runtime log
     * directory and mask. Every stage before ls_openlog() reconfiguration
     * reports errors through ls_syslog() on stderr.
     */
    ls_openlog("bhist-speedup-loader", NULL, 1, "LOG_NOTICE");

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "-V") == 0 ||
                      strcmp(argv[1], "--version") == 0)) {
        fputs(PROGRAM_VERSION, stdout);
        return 0;
    }
    /* No argument runs the service; the service lookup relies on that. */
    command = NULL;
    if (argc == 2 && (strcmp(argv[1], "status") == 0 ||
                      strcmp(argv[1], "clean") == 0)) {
        command = argv[1];
    } else if (argc != 1) {
        usage(stderr);
        return SERVICE_EXIT_USAGE;
    }
    /* Only the service writes the database and needs the SQLite level. */
    if (command == NULL && checkSqliteRuntime() != 0)
        return SERVICE_EXIT_CONFIG;

    memset(&config, 0, sizeof(config));
    if (configLoad(CONFIG_ROLE_LOADER, &config,
                        error, sizeof(error)) != 0) {
        ls_syslog(LOG_ERR, "%s", error);
        goto done;
    }
    if (configLoadAdmins(&config, error, sizeof(error)) != 0 ||
        checkAdminUser(&config, callerUid,
                       error, sizeof(error)) != 0 ||
        configRequireServiceHost(&config,
                                 error, sizeof(error)) != 0 ||
        configLogPath(&config, "bhist-speedup-loader",
                            logPath, sizeof(logPath),
                            error, sizeof(error)) != 0) {
        ls_syslog(LOG_ERR, "%s", error);
        goto done;
    }

    if (command == NULL) {
        if (configResolveEventDir(&config, error, sizeof(error)) != 0) {
            ls_syslog(LOG_ERR, "%s", error);
            goto done;
        }
        result = runLoader(&config, callerUid, logPath);
    } else if (strcmp(command, "clean") == 0) {
        /* Database files belong to the first administrator. */
        if (switchServiceUser(config.admins[0],
                              error, sizeof(error)) != 0) {
            ls_syslog(LOG_ERR, "%s", error);
            goto done;
        }
        result = cleanDatabase(&config);
    } else if (configResolveEventDir(&config,
                                     error, sizeof(error)) != 0) {
        ls_syslog(LOG_ERR, "%s", error);
    } else {
        result = showStatus(&config, logPath);
    }

done:
    configFree(&config);
    return result;
}

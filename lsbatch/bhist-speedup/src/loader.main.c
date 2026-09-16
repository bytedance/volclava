#include "loader.h"
#include "daemon.h"
#include "bhist.config.h"
#include "version.h"
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
 * Sleep for a millisecond interval while retrying interrupted nanosleep calls.
 * @param[in] milliseconds: Requested delay.
 * @return: 0 on success, otherwise -1.
 */
static int
sleepMs(int milliseconds)
{
    struct timespec request;

    if (milliseconds <= 0)
        return 0;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&request, &request) != 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

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
        ls_syslog(LOG_ERR, "live_size_watch_alloc_failed: %m");
        return -1;
    }
    snprintf(context->path, sizeof(context->path), "%s", plan->path);
    context->fileIno = st.st_ino;
    context->lastSize = st.st_size;
    rc = pthread_create(&thread, NULL, watchLiveSize, context);
    if (rc != 0) {
        ls_syslog(LOG_ERR, "live_size_watch_start_failed error=%s",
                strerror(rc));
        free(context);
        return -1;
    }
    rc = pthread_detach(thread);
    if (rc != 0) {
        ls_syslog(LOG_ERR, "live_size_watch_detach_failed error=%s",
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
 * Remove a database and its WAL and shared-memory sidecars.
 * @param[in] dbPath: Main SQLite database path.
 * @return: 0 on success, otherwise -1.
 */
static int
removeDbFiles(const char *dbPath)
{
    char sidecar[LOADER_PATH_MAX + 8];

    if (unlinkIfExists(dbPath) != 0)
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
        "OFF", "OFF", "EXCLUSIVE", "MEMORY",
        LOADER_SQLITE_PAGE_SIZE_BYTES,
        config->importCacheSizeKib, 0);
    ls_syslog(LOG_INFO,
              "sqlite_tuning_mode mode=bulk_import journal_mode=OFF "
              "synchronous=OFF locking_mode=EXCLUSIVE "
              "temp_store=MEMORY page_size=%d cache_size_kib=%d "
              "wal_autocheckpoint=0",
              LOADER_SQLITE_PAGE_SIZE_BYTES,
              config->importCacheSizeKib);
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
    ls_syslog(LOG_INFO,
              "sqlite_tuning_mode mode=live_safe journal_mode=WAL "
              "synchronous=NORMAL locking_mode=default "
              "cache_size_kib=%d wal_autocheckpoint=1000",
              config->importCacheSizeKib);
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
        ls_syslog(LOG_ERR, "publish_db_rename_failed src=%s dst=%s: %m",
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
    ls_syslog(LOG_INFO, "index_build_start db=%s", workDbPath);

    db = NULL;
    if (loaderStoreOpen(workDbPath, config->dbBusyTimeoutMs,
                          &db) != 0) {
        ls_syslog(LOG_ERR, "index_build_open_failed db=%s", workDbPath);
        return -1;
    }
    if (loaderStoreCreateIndexes(db) != 0 ||
        loaderStoreAnalyze(db) != 0) {
        loaderStoreClose(db);
        ls_syslog(LOG_ERR, "index_build_failed db=%s", workDbPath);
        return -1;
    }
    loaderStoreClose(db);

    ls_syslog(LOG_INFO, "index_build_done db=%s elapsed_sec=%ld",
              workDbPath, (long)(time(NULL) - startedAt));
    return 0;
}

/*
 * Create all loader tables, metadata, and the singleton progress row.
 * @param[in] db: Writable SQLite connection for a new database.
 * @return: 0 on success, otherwise -1 with the transaction rolled back.
 */
static int
initDb(sqlite3 *db)
{
    if (loaderStoreBegin(db) != 0)
        return -1;
    if (loaderStoreInitTables(db) != 0 ||
        loaderStoreInitMeta(db) != 0 ||
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
    if (initDb(db) != 0) {
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
 * @return: 0 on the requested bootstrap boundary, otherwise nonzero.
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
    long long watchedLiveSourceId;

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
                    "live_event_file_size_regressed action=rebuild_required");
            goto rebuildRequired;
        }

        if (live.fp != NULL && haveLivePlan) {
            stepRc = loaderImportLiveUntilEof(
                db, &livePlan, &live, &stats);
            if (stats.inputLines > 0 || stepRc == LOADER_STEP_SOURCE_DONE)
                ls_syslog(LOG_INFO,
                          "live_eof_commit source_id=%lld file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld "
                          "start_offset=%lld end_offset=%lld",
                          stats.sourceId, stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines, stats.batchStartOffset,
                          stats.batchEndOffset);
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
                if (stats.inputLines == 0 && stats.sourceId > 0)
                    ls_syslog(LOG_INFO,
                              "live_eof_wait source_id=%lld file=%s "
                              "reason=%s eof_count=%d offset=%lld",
                              stats.sourceId, stats.file,
                              stats.reason[0] ? stats.reason : "none",
                              live.eofCount, live.currentOffset);
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
                          "archive_source_commit source_id=%lld file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld "
                          "start_offset=%lld end_offset=%lld",
                          stats.sourceId, stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines, stats.batchStartOffset,
                          stats.batchEndOffset);
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
                          "archive_source_commit source_id=%lld file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld "
                          "start_offset=%lld end_offset=%lld",
                          stats.sourceId, stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines, stats.batchStartOffset,
                          stats.batchEndOffset);
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
                          "live_eof_commit source_id=%lld file=%s reason=%s "
                          "lines=%lld inserted=%lld skipped=%lld "
                          "start_offset=%lld end_offset=%lld",
                          stats.sourceId, stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          stats.inputLines, stats.insertedRows,
                          stats.skippedLines, stats.batchStartOffset,
                          stats.batchEndOffset);
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
            if (plan.kind == LOADER_SOURCE_LIVE_CURRENT &&
                stats.inputLines == 0 && stats.sourceId > 0)
                ls_syslog(LOG_INFO,
                          "live_eof_wait source_id=%lld file=%s reason=%s "
                          "eof_count=%d offset=%lld",
                          stats.sourceId, stats.file,
                          stats.reason[0] ? stats.reason : "none",
                          live.eofCount, live.currentOffset);
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
fail:
    ls_syslog(LOG_ERR, "loader_failed");
    loaderPlanFreeArchiveQueue(&archiveQueue);
    loaderLiveHandleClose(&live);
    loaderStoreClose(db);
    return 1;
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
    char message[LOADER_PATH_MAX * 3 + 512];

    configureSqliteForBulkImport(config);
    if (!buildWorkDbPath(config->dbPath, workDbPath,
                         sizeof(workDbPath))) {
        ls_syslog(LOG_ERR, "work_db_path_failed %s", config->dbPath);
        return 1;
    }
    if (removeDbFiles(workDbPath) != 0) {
        ls_syslog(LOG_ERR, "remove_work_db_failed");
        return 1;
    }
    if (createDb(workDbPath, config) != 0) {
        ls_syslog(LOG_ERR, "build_db_init_failed");
        return 1;
    }
    if (loaderStoreValidateSchema(workDbPath,
                               config->dbBusyTimeoutMs) != 0)
        return 1;
    snprintf(message, sizeof(message),
             "loader_v2_initial_build_start db=%s work_db=%s logdir=%s",
             config->dbPath, workDbPath, config->logdir);
    ls_syslog(LOG_INFO, "%s", message);
    if (runImportLoop(config->logdir, workDbPath, config, 1) != 0)
        return 1;
    if (buildWorkDbIndexes(workDbPath, config) != 0)
        return 1;
    if (publishWorkDb(workDbPath, config->dbPath) != 0) {
        ls_syslog(LOG_ERR, "publish_db_failed");
        return 1;
    }
    ls_syslog(LOG_INFO, "loader_v2_initial_build_publish_done");
    configureSqliteForLiveImport(config);
    return runImportLoop(config->logdir, config->dbPath, config, 0);
}

/*
 * Start the loader with normal-start or rebuild semantics.
 * @param[in] config: Loader runtime configuration.
 * @param[in] replaceExisting: Nonzero for an explicit rebuild.
 * @return: 0 on success, otherwise nonzero.
 */
static int
startImport(const struct loaderConfig *config, int replaceExisting)
{
    /* Normal start reuses a compatible final database. */
    if (!replaceExisting && access(config->dbPath, F_OK) == 0) {
        if (loaderStoreValidateSchema(config->dbPath,
                                   config->dbBusyTimeoutMs) != 0)
            return 1;
        configureSqliteForLiveImport(config);
        return runImportLoop(config->logdir, config->dbPath, config, 0);
    }
    /* First start always rebuilds the unpublished bootstrap database. */
    if (!replaceExisting)
        return buildAndFollow(config);
    /* Only explicit rebuild may delete the final database and sidecars. */
    if (removeDbFiles(config->dbPath) != 0) {
        ls_syslog(LOG_ERR, "remove_db_failed");
        return 1;
    }
    return buildAndFollow(config);
}

/*
 * Validate the final database schema and bootstrap path before daemonizing.
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
        ls_syslog(LOG_ERR, "work_db_path_failed %s", config.dbPath);
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
    sqlite3 *db;
    struct loaderProgressRow progress;

    printf("db=%s\n", config->dbPath);
    printf("eventsdir=%s\n", config->logdir);
    if (access(config->dbPath, F_OK) != 0) {
        printf("db_status=missing\n");
        return 0;
    }
    db = NULL;
    if (loaderStoreOpenReadonly(config->dbPath,
                                   config->dbBusyTimeoutMs,
                                   &db) != 0) {
        printf("db_status=error\n");
        return 1;
    }
    if (loaderStoreLoadProgress(db, &progress) != 0) {
        printf("db_status=error\n");
        loaderStoreClose(db);
        return 1;
    }
    if (loaderStoreGetDbStatus(db, dbStatus, sizeof(dbStatus)) != 0) {
        printf("db_status=error\n");
        loaderStoreClose(db);
        return 1;
    }
    printf("db_status=%s\n", dbStatus);
    printf("event_rows_tracked=%lld\n", progress.totalSeenRowCount);
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
                 shared->logMask) >= (int)sizeof(config->logMask)) {
        fprintf(stderr, "bhist-speedup-loader: configured path is too long\n");
        return -1;
    }
    loaderImportConfigureChunkLimits(config->importTxMaxLines,
                                         config->importTxMaxRawBytes);
    return 0;
}

/*
 * Execute the loader import loop.
 * @param[in] sharedConfig: Loaded loader configuration.
 * @param[in] rebuild: Whether to rebuild the database.
 * @return: 0 on success, otherwise nonzero.
 */
static int
runImport(const struct config *sharedConfig, int rebuild)
{
    struct loaderConfig config;

    if (initLoaderConfig(sharedConfig, &config) != 0)
        return 1;
    ls_openlog("bhist-speedup-loader", config.runtimeLogdir, 0,
                 config.logMask);
    return startImport(&config, rebuild ? 1 : 0);
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

/* Loader management command implementation. */

struct loaderDaemonContext {
    struct config config;
    int rebuild;
};

/*
 * Print loader management command syntax.
 * @param[out] stream: Destination stream.
 * @return: None.
 */
static void
usage(FILE *stream)
{
    fputs("Usage: bhist-speedup-loader {start|rebuild|stop|status}\n", stream);
}

/*
 * Execute the loader state machine in the daemon child.
 * @param[in] opaque: Pointer to loaderDaemonContext.
 * @return: Loader execution result.
 */
static int
runDaemon(void *opaque)
{
    struct loaderDaemonContext *context;

    context = (struct loaderDaemonContext *)opaque;
    return runImport(&context->config, context->rebuild);
}

/*
 * Build the loader PID-file path beside the configured database.
 * @param[in] config: Loader configuration.
 * @param[out] path: Buffer receiving the PID-file path.
 * @param[in] pathSize: Size of path.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
pidPathForConfig(const struct config *config,
                    char *path, size_t pathSize,
                    char *error, size_t errorSize)
{
    char directory[CONFIG_PATH_MAX];

    if (parentDirectory(config->dbPath, directory,
                             sizeof(directory), error, errorSize) != 0)
        return -1;
    if (snprintf(path, pathSize, "%s/bhist-speedup-loader.pid", directory) >=
        (int)pathSize) {
        snprintf(error, errorSize, "loader pid path is too long");
        return -1;
    }
    return 0;
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
    struct stat st;

    if (config->eventDir[0] == '\0') {
        snprintf(error, errorSize, "LSB_BHIST_EVENT_DIR is required");
        return -1;
    }
    if (stat(config->eventDir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(config->eventDir, R_OK | X_OK) != 0) {
        snprintf(error, errorSize,
                 "event log directory is unavailable: %.400s",
                 config->eventDir);
        return -1;
    }
    if (parentDirectory(config->dbPath, databaseDirectory,
                             sizeof(databaseDirectory),
                             error, errorSize) != 0)
        return -1;
    if (ensureDirectory(databaseDirectory,
                             error, errorSize) != 0)
        return -1;
    if (stat(databaseDirectory, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(databaseDirectory, W_OK | X_OK) != 0) {
        snprintf(error, errorSize,
                 "database directory is not writable: %.400s",
                 databaseDirectory);
        return -1;
    }
    if (stat(config->logDir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        access(config->logDir, W_OK | X_OK) != 0) {
        snprintf(error, errorSize,
                 "runtime log directory is unavailable: %.400s",
                 config->logDir);
        return -1;
    }
    return 0;
}

/*
 * Start the loader daemon with normal-start or rebuild semantics.
 * @param[in] context: Daemon startup context.
 * @param[in] rebuild: Nonzero for rebuild.
 * @param[in] pidPath: PID file path.
 * @param[in] logPath: Daemon log path.
 * @return: 0 when started or normal start finds it running, otherwise 1.
 */
static int
doStart(struct loaderDaemonContext *context, int rebuild,
         const char *pidPath, const char *logPath)
{
    char error[512];
    pid_t pid;
    int result;

    result = pidFileStatus(pidPath, &pid, error, sizeof(error));
    if (result < 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        return 1;
    }
    if (result > 0) {
        if (rebuild) {
            fprintf(stderr,
                    "bhist-speedup-loader: loader is running (pid=%ld); "
                    "stop it before rebuild\n", (long)pid);
            return 1;
        }
        printf("bhist-speedup-loader: already running pid=%ld\n", (long)pid);
        return 0;
    }
    if (validateStartDirectories(&context->config,
                                   error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        return 1;
    }
    if (!rebuild && validateStartDatabase(&context->config) != 0)
        return 1;
    context->rebuild = rebuild;
    result = daemonStart(pidPath, logPath, runDaemon,
                              context, &pid, error, sizeof(error));
    if (result < 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        return 1;
    }
    if (result > 0) {
        if (rebuild) {
            fprintf(stderr,
                    "bhist-speedup-loader: loader is running (pid=%ld); "
                    "stop it before rebuild\n", (long)pid);
            return 1;
        }
        printf("bhist-speedup-loader: already running pid=%ld\n", (long)pid);
        return 0;
    }
    printf("bhist-speedup-loader: started pid=%ld db=%s log=%s\n",
           (long)pid, context->config.dbPath, logPath);
    return 0;
}

/*
 * Stop the loader daemon.
 * @param[in] pidPath: PID file path.
 * @return: 0 when stopped or already stopped, otherwise 1.
 */
static int
doStop(const char *pidPath)
{
    char error[512];
    pid_t pid;
    int wasRunning;

    if (daemonStop(pidPath, 10000, &pid, &wasRunning,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        return 1;
    }
    if (wasRunning)
        printf("bhist-speedup-loader: stopped pid=%ld\n", (long)pid);
    else
        puts("bhist-speedup-loader: not running");
    return 0;
}

/*
 * Print loader process state and database import progress.
 * @param[in] config: Loader configuration.
 * @param[in] pidPath: PID file path.
 * @return: Database status result when running, otherwise 1.
 */
static int
doStatus(const struct config *config, const char *pidPath)
{
    char error[512];
    pid_t pid;
    int result;
    int metadataResult;

    result = pidFileStatus(pidPath, &pid, error, sizeof(error));
    if (result < 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        return 1;
    }
    if (result > 0) {
        puts("state=running");
        printf("pid=%ld\n", (long)pid);
    } else {
        puts("state=stopped");
        (void)pidFileRemoveStale(pidPath, error, sizeof(error));
    }
    metadataResult = printStatus(config);
    return result > 0 ? metadataResult : 1;
}

/*
 * Parse and dispatch loader start, rebuild, stop, and status commands.
 * @param[in] argc: Number of command-line arguments.
 * @param[in] argv: Command-line argument vector.
 * @return: 0 on success, 2 for invalid arguments, otherwise 1.
 */
int
main(int argc, char **argv)
{
    struct loaderDaemonContext context;
    char error[512];
    char logPath[CONFIG_PATH_MAX];
    char pidPath[CONFIG_PATH_MAX];
    const char *command;
    uid_t callerUid = getuid();
    int result = 1;

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
    if (argc != 2) {
        usage(stderr);
        return 2;
    }
    command = argv[1];
    if (strcmp(command, "start") != 0 &&
        strcmp(command, "rebuild") != 0 &&
        strcmp(command, "stop") != 0 &&
        strcmp(command, "status") != 0) {
        usage(stderr);
        return 2;
    }

    memset(&context, 0, sizeof(context));
    if (configLoad(CONFIG_ROLE_LOADER, &context.config,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        goto done;
    }
    if (configLoadAdmins(&context.config, error, sizeof(error)) != 0 ||
        checkAdminUser(&context.config, callerUid,
                       error, sizeof(error)) != 0 ||
        configRequireServiceHost(&context.config,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        goto done;
    }
    /* Keep root for inspection and stopping services owned by any admin. */
    if (callerUid == 0 && strcmp(command, "stop") != 0 &&
        strcmp(command, "status") != 0 &&
        switchServiceUser(context.config.admins[0], error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        goto done;
    }
    if (pidPathForConfig(&context.config, pidPath, sizeof(pidPath),
                            error, sizeof(error)) != 0 ||
        configLogPath(&context.config, "bhist-speedup-loader",
                            logPath, sizeof(logPath),
                            error, sizeof(error)) != 0) {
        fprintf(stderr, "bhist-speedup-loader: %s\n", error);
        goto done;
    }

    if (strcmp(command, "start") == 0)
        result = doStart(&context, 0, pidPath, logPath);
    else if (strcmp(command, "rebuild") == 0)
        result = doStart(&context, 1, pidPath, logPath);
    else if (strcmp(command, "stop") == 0)
        result = doStop(pidPath);
    else
        result = doStatus(&context.config, pidPath);

done:
    configFree(&context.config);
    return result;
}

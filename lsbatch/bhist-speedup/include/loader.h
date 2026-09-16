#ifndef LOADER_H
#define LOADER_H

#include <stddef.h>
#include <stdio.h>

#ifndef LOADER_PATH_MAX
#define LOADER_PATH_MAX 4096
#endif

/* Classifies the outcome of parsing one scheduler event line. */
enum loaderEventStatus {
    LOADER_EVENT_OK = 0,
    LOADER_EVENT_NO_EVENT = 1,
    LOADER_EVENT_NO_JOB = 2,
    LOADER_EVENT_PARSE_ERROR = 3,
    LOADER_EVENT_TOKEN_TOO_LONG = 4
};

#define LOADER_EVENT_USER_MAX 256
#define LOADER_EVENT_QUEUE_MAX 256

/* Carries job identity filter fields extracted from one event record. */
struct loaderEventFields {
    int hasEventTime;
    long long eventTime;
    int hasSubmitTime;
    long long submitTime;
    int hasSubmitUser;
    char submitUser[LOADER_EVENT_USER_MAX];
    int hasSubmitQueue;
    char submitQueue[LOADER_EVENT_QUEUE_MAX];
    int hasStartTime;
    long long startTime;
    int hasEndTime;
    long long endTime;
};

int loaderExtractEventFromLine(const char *line, long long *jobId,
                               struct loaderEventFields *times);

#define LOADER_SCHEMA_VERSION 4
#define LOADER_SQLITE_BUSY_TIMEOUT_MS 60000
#define LOADER_SQLITE_PAGE_SIZE_BYTES 65536
#define LOADER_SQLITE_LIVE_CACHE_SIZE_KIB (-32768)
#define LOADER_DB_STATUS_READY "ready"
#define LOADER_DB_STATUS_REBUILD_REQUIRED "rebuild_required"
#define LOADER_EVENT_DAY_KEY_MAX 16
#define LOADER_EVENT_TABLE_NAME_MAX 64
#ifndef LOADER_SQLITE3_TYPEDEF
#define LOADER_SQLITE3_TYPEDEF
typedef struct sqlite3 sqlite3;
#endif

#ifndef LOADER_SQLITE3_STMT_TYPEDEF
#define LOADER_SQLITE3_STMT_TYPEDEF
typedef struct sqlite3_stmt sqlite3_stmt;
#endif

/*
 * Mirrors the singleton loader_progress row used to resume an interrupted
 * import and to expose loader status.
 */
struct loaderProgressRow {
    long long completedSources;
    long long currentSourceId;
    char currentState[32];
    char currentKind[32];
    char currentPathHint[LOADER_PATH_MAX];
    int hasCurrentArchiveIndex;
    long long currentArchiveIndex;
    char currentFileIno[32];
    long long currentPayloadStartOff;
    long long currentOffset;
    int hasCurrentEndOffset;
    long long currentEndOffset;
    long long currentSeenRowCount;
    long long totalSeenRowCount;
    long long openedAt;
    long long lastVisibleMaxArchive;
};

/* Describes a source generation before it is inserted into loader_sources. */
struct loaderSourceRow {
    long long sourceId;
    const char *sourceKind;
    const char *fileIno;
    const char *pathHint;
    long long payloadStartOff;
    long long currentOffset;
    int hasEndOffset;
    long long endOffset;
    long long openedAt;
};

/* Owns prepared statements and day-table state for one import transaction. */
struct loaderStoreEventWriter {
    sqlite3_stmt *insertEventStmt;
    sqlite3_stmt *updateSourceSeenStmt;
    int updateSeenPerRow;
    char dayKey[LOADER_EVENT_DAY_KEY_MAX];
    char tableName[LOADER_EVENT_TABLE_NAME_MAX];
};

int loaderStoreOpen(const char *dbPath, int busyTimeoutMs,
                    sqlite3 **outDb);

int loaderStoreOpenReadonly(const char *dbPath, int busyTimeoutMs,
                            sqlite3 **outDb);

void loaderStoreConfigureSqlite(const char *journalMode,
                                const char *synchronous,
                                const char *lockingMode,
                                const char *tempStore,
                                int pageSizeBytes,
                                int cacheSizeKib,
                                int walAutocheckpointPages);

void loaderStoreClose(sqlite3 *db);

int loaderStoreInitTables(sqlite3 *db);

int loaderStoreCreateIndexes(sqlite3 *db);

int loaderStoreAnalyze(sqlite3 *db);

int loaderStoreInitMeta(sqlite3 *db);

int loaderStoreGetDbStatus(sqlite3 *db, char *status,
                           size_t statusLen);

int loaderStoreValidateSchema(const char *dbPath, int busyTimeoutMs);

int loaderStoreMarkRebuildRequired(sqlite3 *db);

int loaderStoreInitProgress(sqlite3 *db);

int loaderStoreBegin(sqlite3 *db);

int loaderStoreCommit(sqlite3 *db);

int loaderStoreRollback(sqlite3 *db);

int loaderStoreLoadProgress(sqlite3 *db,
                            struct loaderProgressRow *progress);

int loaderStoreCreateSourceRow(sqlite3 *db,
                               const struct loaderSourceRow *source);

int loaderStoreSkipCompletedSources(sqlite3 *db,
                                    long long skippedSources,
                                    long long lastVisibleMaxArchive);

int loaderStoreReadRetainedHeadSourceId(sqlite3 *db,
                                        long long *sourceIdOut);

int loaderStoreFindRetainedFloorDay(sqlite3 *db, const char *cutoffDay,
                                    char *floorDay, size_t floorDayLen);

int loaderStoreLoadExpiredEventTables(
    sqlite3 *db, const char *floorDay,
    char (**tablesOut)[LOADER_EVENT_TABLE_NAME_MAX], size_t *countOut);

int loaderStoreDropExpiredEventTables(
    sqlite3 *db, const char *floorDay,
    char (*tables)[LOADER_EVENT_TABLE_NAME_MAX], size_t tableCount);

int loaderStoreUpdateRetainedHeadSourceId(sqlite3 *db,
                                          long long sourceId);

int loaderStoreFormatEventDay(long long eventTime,
                              char *dayKey,
                              size_t dayKeyLen);

int loaderStoreEventWriterOpen(sqlite3 *db,
                               struct loaderStoreEventWriter *writer,
                               const char *dayKey);

int loaderStoreEventWriterOpenBulk(sqlite3 *db,
                                   struct loaderStoreEventWriter *writer,
                                   const char *dayKey);

int loaderStoreEventWriterOpenUntracked(
    sqlite3 *db,
    struct loaderStoreEventWriter *writer,
    const char *dayKey);

void loaderStoreEventWriterClose(struct loaderStoreEventWriter *writer);

int loaderStoreEventWriterInsert(sqlite3 *db,
                                 struct loaderStoreEventWriter *writer,
                                 long long jobId,
                                 const char *rawLine,
                                 const struct loaderEventFields *times);

int loaderStoreUpdateCurrentOffset(sqlite3 *db,
                                   long long currentOffset);

int loaderStoreUpdateSourceSeenCount(sqlite3 *db, long long sourceId,
                                     long long seenRowCount);

int loaderStoreFinishCurrentSource(sqlite3 *db);

int loaderStoreRemapCurrentSourceToArchive(sqlite3 *db,
                                           const char *fileIno,
                                           const char *pathHint);

/* Identifies whether an import plan targets an archive or live lsb.events. */
enum loaderSourceKind {
    LOADER_SOURCE_NONE = 0,
    LOADER_SOURCE_ARCHIVE = 1,
    LOADER_SOURCE_LIVE_CURRENT = 2
};

/* Reports whether a loader step progressed, should retry, or must rebuild. */
enum loaderStepResult {
    LOADER_STEP_COMMITTED = 0,
    LOADER_STEP_SOURCE_DONE = 1,
    LOADER_STEP_IDLE = 2,
    LOADER_STEP_STALE_REPLAN = 3,
    LOADER_STEP_REBUILD_REQUIRED = 4,
    LOADER_STEP_FATAL = 5
};

/* Immutable description of the next source range selected for import. */
struct loaderSourcePlan {
    enum loaderSourceKind kind;
    long long sourceId;
    long long completedSources;
    long long seenRowCount;
    char path[LOADER_PATH_MAX];
    char fileIno[32];
    long long payloadStartOff;
    long long currentOffset;
    int hasEndOffset;
    long long endOffset;
    int markRemapped;
};

/* Owns the ordered archive plans prepared for one initial-build pass. */
struct loaderArchiveQueue {
    struct loaderSourcePlan *plans;
    size_t count;
    size_t pos;
    long firstArchiveIndex;
    long lastArchiveIndex;
    long maxArchiveIndex;
    long long completedSources;
};

void loaderPlanFreeArchiveQueue(struct loaderArchiveQueue *queue);

int loaderPlanBeginSource(sqlite3 *db,
                          const struct loaderSourcePlan *plan,
                          long long openedAt);

int loaderPlanArchiveQueue(const char *logdir,
                           sqlite3 *db,
                           long long historyRetentionSeconds,
                           struct loaderArchiveQueue *queue);

int loaderPlanNextSource(const char *logdir,
                         sqlite3 *db,
                         long long historyRetentionSeconds,
                         long long openedAt,
                         struct loaderSourcePlan *plan);

/* Retains the open live file and resumable offsets across polling cycles. */
struct loaderLiveHandle {
    FILE *fp;
    long long sourceId;
    char path[LOADER_PATH_MAX];
    char fileIno[32];
    long long payloadStartOff;
    long long currentOffset;
    long long seenRowCount;
    int eofCount;
};

/* Accumulates source and transaction counters used by loader diagnostics. */
struct loaderImportStats {
    long long sourceId;
    long long inputLines;
    long long insertedRows;
    long long skippedLines;
    long long batchStartOffset;
    long long batchEndOffset;
    long long currentOffset;
    char file[LOADER_PATH_MAX];
    char reason[32];
};

void loaderImportConfigureChunkLimits(long long maxLines,
                                      long long maxRawBytes);

void loaderLiveHandleClose(struct loaderLiveHandle *live);

int loaderImportRefreshRetainedHead(
    sqlite3 *db,
    const struct loaderSourcePlan *plan,
    long long historyRetentionSeconds,
    long long now);

int loaderImportArchiveSource(sqlite3 *db,
                              const struct loaderSourcePlan *plan,
                              struct loaderImportStats *stats);

int loaderImportLiveUntilEof(sqlite3 *db,
                             const struct loaderSourcePlan *plan,
                             struct loaderLiveHandle *live,
                             struct loaderImportStats *stats);

#endif

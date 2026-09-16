#include "loader.h"
#include "lproto.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

/*
 * SQLite schema, connection tuning, transaction primitives, and progress-state
 * transitions for the loader.
 *
 * Events are stored in local-day job_event_kv_YYYYMMDD tables.
 * loader_event_tables is the query side's only trusted table registry. Metadata
 * stores schema and retained-head state; the singleton progress row stores the
 * current generation identity, committed position, and source counts.
 *
 * Except for begin, commit, and rollback, write helpers do not create
 * transactions. Import and plan callers must commit event rows and progress in
 * one transaction. SQLite tuning is process-global configuration for the next
 * open and changes only when the loader switches bulk and live modes.
 */

/* Pairs a metadata key with the value inserted when a database is created. */
struct loaderMetaDefault {
    const char *key;
    const char *value;
};

/* Journal mode applied when the next loader database connection opens. */
static char loaderStoreSqliteJournalMode[32] = "WAL";
/* Synchronous mode applied when the next loader connection opens. */
static char loaderStoreSqliteSynchronous[32] = "NORMAL";
/* Optional locking mode applied to each newly opened loader connection. */
static char loaderStoreSqliteLockingMode[32] = "";
/* Optional temp_store mode applied to each newly opened loader connection. */
static char loaderStoreSqliteTempStore[32] = "";
/* Page size requested before a new database creates its first table. */
static int loaderStoreSqlitePageSizeBytes = 0;
/* SQLite page-cache size in KiB; negative values use SQLite's KiB form. */
static int loaderStoreSqliteCacheSizeKib =
    LOADER_SQLITE_LIVE_CACHE_SIZE_KIB;
/* WAL pages between automatic checkpoints for newly opened connections. */
static int loaderStoreSqliteWalAutocheckpointPages = 1000;

static int execSql(sqlite3 *db, const char *sql);
static int applySqliteTuning(sqlite3 *db);
static int bindText(sqlite3_stmt *stmt, int index,
                    const char *value);
static int bindOptionalInt64(sqlite3_stmt *stmt, int index,
                             int hasValue, long long value);
static int bindOptionalText(sqlite3_stmt *stmt, int index,
                            int hasValue,
                            const char *value);
static int formatEventTable(const char *dayKey,
                            char *tableName,
                            size_t tableNameLen);
static int ensureEventTable(sqlite3 *db,
                            const char *dayKey,
                            char *tableName,
                            size_t tableNameLen,
                            int createIndexes);
static int createIndexesForTable(sqlite3 *db,
                                 const char *tableName,
                                 int includeFinalIndexes);
static int openEventWriter(sqlite3 *db,
                           struct loaderStoreEventWriter *writer,
                           const char *dayKey,
                           int updateSeenPerRow,
                           int createFinalIndexes);
static int prepareEventWriter(sqlite3 *db,
                              struct loaderStoreEventWriter *writer,
                              const char *tableName,
                              int updateSeenPerRow);
static int beginCurrentSource(
    sqlite3 *db, const struct loaderProgressRow *source);

/*
 * Execute one SQL statement and route SQLite diagnostics to the loader log.
 * @param[in] db: SQLite connection.
 * @param[in] sql: Complete SQL statement.
 * @return: 0 on success, otherwise -1.
 */
static int
execSql(sqlite3 *db, const char *sql)
{
    char *sqliteError;
    int rc;

    sqliteError = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &sqliteError);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "sqlite_exec_failed sql=%s error=%s", sql,
                  sqliteError ? sqliteError : sqlite3_errmsg(db));
        sqlite3_free(sqliteError);
        return -1;
    }
    sqlite3_free(sqliteError);
    return 0;
}

/*
 * Apply configured connection-level cache, temp, WAL, and locking pragmas.
 * @param[in] db: Newly opened SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
static int
applySqliteTuning(sqlite3 *db)
{
    char sql[128];

    if (loaderStoreSqliteCacheSizeKib != 0) {
        snprintf(sql, sizeof(sql), "PRAGMA cache_size=%d",
                 loaderStoreSqliteCacheSizeKib);
        if (execSql(db, sql) != 0)
            return -1;
    }
    if (loaderStoreSqliteTempStore[0] != '\0') {
        snprintf(sql, sizeof(sql), "PRAGMA temp_store=%s",
                 loaderStoreSqliteTempStore);
        if (execSql(db, sql) != 0)
            return -1;
    }
    snprintf(sql, sizeof(sql), "PRAGMA wal_autocheckpoint=%d",
             loaderStoreSqliteWalAutocheckpointPages);
    if (execSql(db, sql) != 0)
        return -1;
    if (loaderStoreSqliteLockingMode[0] != '\0') {
        snprintf(sql, sizeof(sql), "PRAGMA locking_mode=%s",
                 loaderStoreSqliteLockingMode);
        if (execSql(db, sql) != 0)
            return -1;
    }

    return 0;
}

/*
 * Bind a required text value with transient ownership.
 * @param[in] stmt: Prepared SQLite statement.
 * @param[in] index: One-based bind index.
 * @param[in] value: Text value; NULL is represented as an empty string.
 * @return: SQLite bind result code.
 */
static int
bindText(sqlite3_stmt *stmt, int index, const char *value)
{
    return sqlite3_bind_text(stmt, index, value ? value : "", -1,
                             SQLITE_TRANSIENT);
}

/*
 * Bind either an int64 value or SQL NULL.
 * @param[in] stmt: Prepared SQLite statement.
 * @param[in] index: One-based bind index.
 * @param[in] hasValue: Nonzero when value is present.
 * @param[in] value: Integer value used when present.
 * @return: SQLite bind result code.
 */
static int
bindOptionalInt64(sqlite3_stmt *stmt, int index,
                  int hasValue, long long value)
{
    if (!hasValue)
        return sqlite3_bind_null(stmt, index);
    return sqlite3_bind_int64(stmt, index, value);
}

/*
 * Bind either a transient text value or SQL NULL.
 * @param[in] stmt: Prepared SQLite statement.
 * @param[in] index: One-based bind index.
 * @param[in] hasValue: Nonzero when value is present.
 * @param[in] value: Text used when present.
 * @return: SQLite bind result code.
 */
static int
bindOptionalText(sqlite3_stmt *stmt, int index,
                 int hasValue, const char *value)
{
    if (!hasValue)
        return sqlite3_bind_null(stmt, index);
    return bindText(stmt, index, value);
}

/*
 * Convert an event timestamp to a local-time day key.
 * @param[in] eventTime: Unix event timestamp.
 * @param[out] dayKey: Buffer receiving YYYYMMDD.
 * @param[in] dayKeyLen: Size of dayKey.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreFormatEventDay(long long eventTime,
                              char *dayKey,
                              size_t dayKeyLen)
{
    time_t timestamp;
    struct tm tm_value;

    if (dayKey == NULL || dayKeyLen < LOADER_EVENT_DAY_KEY_MAX) {
        ls_syslog(LOG_ERR, "invalid day key buffer");
        return -1;
    }
    timestamp = (time_t)eventTime;
    if (localtime_r(&timestamp, &tm_value) == NULL) {
        ls_syslog(LOG_ERR, "format event day failed event_time=%lld",
                  eventTime);
        return -1;
    }
    snprintf(dayKey, dayKeyLen, "%04d%02d%02d",
             tm_value.tm_year + 1900, tm_value.tm_mon + 1,
             tm_value.tm_mday);
    return 0;
}

/*
 * Build and validate the daily event table name for a day key.
 * @param[in] dayKey: Eight-digit local-time YYYYMMDD key.
 * @param[out] tableName: Buffer receiving job_event_kv_YYYYMMDD.
 * @param[in] tableNameLen: Size of tableName.
 * @return: 0 on success, otherwise -1.
 */
static int
formatEventTable(const char *dayKey,
                 char *tableName,
                 size_t tableNameLen)
{
    size_t i;

    if (dayKey == NULL || tableName == NULL ||
        tableNameLen < LOADER_EVENT_TABLE_NAME_MAX ||
        strlen(dayKey) != 8) {
        ls_syslog(LOG_ERR, "invalid event day key");
        return -1;
    }
    for (i = 0; i < 8; i++) {
        if (dayKey[i] < '0' || dayKey[i] > '9') {
            ls_syslog(LOG_ERR, "invalid event day key");
            return -1;
        }
    }
    snprintf(tableName, tableNameLen, "job_event_kv_%s", dayKey);
    return 0;
}

/*
 * Create the query indexes required for one registered daily event table.
 * @param[in] db: SQLite connection.
 * @param[in] tableName: Valid daily event table name.
 * @param[in] includeFinalIndexes: Nonzero to create final query indexes.
 * @return: 0 on success, otherwise -1.
 */
static int
createIndexesForTable(sqlite3 *db,
                      const char *tableName,
                      int includeFinalIndexes)
{
    /*
     * Equality filters such as -u and -q support both history seed scans and
     * correlated (value, job_id) EXISTS probes. A nullable filter field needs a
     * (value, job_id) partial index with IS NOT NULL, which serves both access
     * patterns without indexing events that carry no submission metadata.
     */
    static const struct {
        const char *suffix;
        const char *columns;
        const char *whereClause;
    } indexes[] = {
        {
            "submit_time",
            "submit_time, job_id",
            "submit_time IS NOT NULL"
        },
        {
            "submit_user",
            "submit_user, job_id",
            "submit_user IS NOT NULL"
        },
        {
            "submit_queue",
            "submit_queue, job_id",
            "submit_queue IS NOT NULL"
        },
        {
            "start_time",
            "start_time, job_id",
            "start_time IS NOT NULL"
        },
        {
            "end_time",
            "end_time, job_id",
            "end_time IS NOT NULL"
        },
        {
            "job",
            "job_id",
            NULL
        }
    };
    size_t i;

    if (db == NULL || tableName == NULL || tableName[0] == '\0') {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    for (i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
        char indexName[LOADER_EVENT_TABLE_NAME_MAX + 32];
        char sql[512];
        time_t startedAt;

        if (!includeFinalIndexes)
            continue;
        snprintf(indexName, sizeof(indexName), "idx_%s_%s",
                 tableName, indexes[i].suffix);
        if (indexes[i].whereClause != NULL) {
            snprintf(sql, sizeof(sql),
                     "CREATE INDEX IF NOT EXISTS %s ON %s(%s)"
                     " WHERE %s",
                     indexName, tableName, indexes[i].columns,
                     indexes[i].whereClause);
        } else {
            snprintf(sql, sizeof(sql),
                     "CREATE INDEX IF NOT EXISTS %s ON %s(%s)",
                     indexName, tableName, indexes[i].columns);
        }
        startedAt = time(NULL);
        ls_syslog(LOG_INFO, "index_create_start name=%s", indexName);
        if (execSql(db, sql) != 0) {
            ls_syslog(LOG_ERR, "index_create_failed name=%s", indexName);
            return -1;
        }
        ls_syslog(LOG_INFO, "index_create_done name=%s elapsed_sec=%ld",
                  indexName, (long)(time(NULL) - startedAt));
    }
    return 0;
}

/*
 * Ensure a daily event table and its trusted registry row exist.
 * @param[in] db: SQLite connection.
 * @param[in] dayKey: Eight-digit local-time day key.
 * @param[out] tableName: Buffer receiving the table name.
 * @param[in] tableNameLen: Size of tableName.
 * @param[in] createIndexes: Nonzero to create final query indexes now.
 * @return: 0 on success, otherwise -1.
 */
static int
ensureEventTable(sqlite3 *db,
                 const char *dayKey,
                 char *tableName,
                 size_t tableNameLen,
                 int createIndexes)
{
    sqlite3_stmt *stmt;
    char sql[1024];
    int insertedRegistryRow;
    int rc;

    if (db == NULL ||
        formatEventTable(dayKey, tableName,
                         tableNameLen) != 0)
        return -1;

    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS %s (\n"
             "    raw_line TEXT NOT NULL,\n"
             "    job_id    INTEGER NOT NULL,\n"
             "    event_time INTEGER,\n"
             "    submit_time INTEGER,\n"
             "    submit_user TEXT,\n"
             "    submit_queue TEXT,\n"
             "    start_time  INTEGER,\n"
             "    end_time    INTEGER\n"
             ")",
             tableName);
    if (execSql(db, sql) != 0)
        return -1;

    rc = sqlite3_prepare_v2(
        db,
        "INSERT OR IGNORE INTO loader_event_tables (day_key, table_name)"
        " VALUES (?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_event_tables insert failed %s",
                  sqlite3_errmsg(db));
        return -1;
    }
    rc = bindText(stmt, 1, dayKey);
    rc = rc == SQLITE_OK ? bindText(stmt, 2, tableName) : rc;
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind loader_event_tables insert failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "insert loader_event_tables failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    insertedRegistryRow = sqlite3_changes(db) > 0;
    sqlite3_finalize(stmt);

    /*
     * Create indexes only when first registering a day table. Existing entries
     * were handled at creation or by the final bootstrap index pass.
     */
    if (!createIndexes || !insertedRegistryRow)
        return 0;
    return createIndexesForTable(db, tableName, 1);
}

/*
 * Open a read-write SQLite connection for the loader.
 * @param[in] dbPath: Database path.
 * @param[in] busyTimeoutMs: Busy timeout in milliseconds.
 * @param[out] outDb: Open SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreOpen(const char *dbPath, int busyTimeoutMs,
                  sqlite3 **outDb)
{
    sqlite3 *db;
    char sql[128];
    int rc;

    if (dbPath == NULL || outDb == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    *outDb = NULL;
    db = NULL;
    rc = sqlite3_open(dbPath, &db);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "sqlite open failed %s", db ? sqlite3_errmsg(db) : dbPath);
        if (db != NULL)
            sqlite3_close(db);
        return -1;
    }

    if (busyTimeoutMs <= 0)
        busyTimeoutMs = LOADER_SQLITE_BUSY_TIMEOUT_MS;
    sqlite3_busy_timeout(db, busyTimeoutMs);

    if (loaderStoreSqlitePageSizeBytes > 0) {
        snprintf(sql, sizeof(sql), "PRAGMA page_size=%d",
                 loaderStoreSqlitePageSizeBytes);
        if (execSql(db, sql) != 0) {
            sqlite3_close(db);
            return -1;
        }
    }
    snprintf(sql, sizeof(sql), "PRAGMA journal_mode=%s",
             loaderStoreSqliteJournalMode[0] != '\0' ?
             loaderStoreSqliteJournalMode : "WAL");
    if (execSql(db, sql) != 0) {
        sqlite3_close(db);
        return -1;
    }
    snprintf(sql, sizeof(sql), "PRAGMA synchronous=%s",
             loaderStoreSqliteSynchronous[0] != '\0' ?
             loaderStoreSqliteSynchronous : "NORMAL");
    if (execSql(db, sql) != 0 ||
        execSql(db, "PRAGMA foreign_keys=OFF") != 0 ||
        applySqliteTuning(db) != 0) {
        sqlite3_close(db);
        return -1;
    }

    *outDb = db;
    return 0;
}

/*
 * Open a read-only SQLite connection for loader status.
 * @param[in] dbPath: Database path.
 * @param[in] busyTimeoutMs: Busy timeout in milliseconds.
 * @param[out] outDb: Open SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreOpenReadonly(const char *dbPath, int busyTimeoutMs,
                           sqlite3 **outDb)
{
    sqlite3 *db;
    int rc;

    if (dbPath == NULL || outDb == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    *outDb = NULL;
    db = NULL;
    rc = sqlite3_open_v2(dbPath, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "sqlite open failed %s", db ? sqlite3_errmsg(db) : dbPath);
        if (db != NULL)
            sqlite3_close(db);
        return -1;
    }

    if (busyTimeoutMs <= 0)
        busyTimeoutMs = LOADER_SQLITE_BUSY_TIMEOUT_MS;
    sqlite3_busy_timeout(db, busyTimeoutMs);

    /*
     * Status and query-side readers must not run journal_mode or other
     * write-capable PRAGMAs. During write-mode imports, those PRAGMAs can
     * contend with the writer and disturb WAL checkpoint behavior.
     */
    if (execSql(db, "PRAGMA query_only=ON") != 0) {
        sqlite3_close(db);
        return -1;
    }

    *outDb = db;
    return 0;
}

/*
 * Configure SQLite pragmas applied to subsequent read-write connections.
 * @param[in] journalMode: journal_mode value.
 * @param[in] synchronous: synchronous value.
 * @param[in] lockingMode: locking_mode value.
 * @param[in] tempStore: temp_store value.
 * @param[in] pageSizeBytes: Page size in bytes.
 * @param[in] cacheSizeKib: Cache size in KiB.
 * @param[in] walAutocheckpointPages: WAL autocheckpoint interval in pages.
 * @return: None.
 */
void
loaderStoreConfigureSqlite(const char *journalMode,
                              const char *synchronous,
                              const char *lockingMode,
                              const char *tempStore,
                              int pageSizeBytes,
                              int cacheSizeKib,
                              int walAutocheckpointPages)
{
    snprintf(loaderStoreSqliteJournalMode,
             sizeof(loaderStoreSqliteJournalMode), "%s",
             journalMode != NULL && journalMode[0] != '\0' ?
             journalMode : "WAL");
    snprintf(loaderStoreSqliteSynchronous,
             sizeof(loaderStoreSqliteSynchronous), "%s",
             synchronous != NULL && synchronous[0] != '\0' ?
             synchronous : "NORMAL");
    snprintf(loaderStoreSqliteLockingMode,
             sizeof(loaderStoreSqliteLockingMode), "%s",
             lockingMode != NULL ? lockingMode : "");
    snprintf(loaderStoreSqliteTempStore,
             sizeof(loaderStoreSqliteTempStore), "%s",
             tempStore != NULL ? tempStore : "");
    loaderStoreSqlitePageSizeBytes = pageSizeBytes;
    loaderStoreSqliteCacheSizeKib = cacheSizeKib;
    loaderStoreSqliteWalAutocheckpointPages =
        walAutocheckpointPages >= 0 ? walAutocheckpointPages : 1000;
}

/*
 * Close a SQLite connection.
 * @param[in] db: SQLite connection.
 * @return: None.
 */
void
loaderStoreClose(sqlite3 *db)
{
    if (db != NULL)
        sqlite3_close(db);
}

/*
 * Create loader metadata and progress tables.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreInitTables(sqlite3 *db)
{
    static const char *tableSql =
        "CREATE TABLE loader_event_tables (\n"
        "    day_key TEXT PRIMARY KEY,\n"
        "    table_name TEXT NOT NULL UNIQUE\n"
        ");\n"
        "CREATE TABLE loader_progress (\n"
        "    id INTEGER PRIMARY KEY CHECK (id = 1),\n"
        "    completed_sources INTEGER NOT NULL DEFAULT 0,\n"
        "    current_source_id INTEGER NOT NULL DEFAULT 1,\n"
        "    current_state TEXT NOT NULL DEFAULT 'idle',\n"
        "    current_kind TEXT NOT NULL DEFAULT 'none',\n"
        "    current_path_hint TEXT NOT NULL DEFAULT '',\n"
        "    current_archive_index INTEGER,\n"
        "    current_file_ino TEXT NOT NULL DEFAULT '',\n"
        "    current_payload_start_off INTEGER NOT NULL DEFAULT 0,\n"
        "    current_offset INTEGER NOT NULL DEFAULT 0,\n"
        "    current_end_offset INTEGER,\n"
        "    current_seen_row_count INTEGER NOT NULL DEFAULT 0,\n"
        "    total_seen_row_count INTEGER NOT NULL DEFAULT 0,\n"
        "    opened_at INTEGER NOT NULL DEFAULT 0,\n"
        "    last_visible_max_archive INTEGER NOT NULL DEFAULT 0\n"
        ");\n"
        "CREATE TABLE loader_meta (\n"
        "    meta_key     TEXT PRIMARY KEY,\n"
        "    meta_value   TEXT NOT NULL\n"
        ");\n"
        "CREATE TABLE bhist_request_history (\n"
        "    completed_at_ms INTEGER NOT NULL,\n"
        "    request_user TEXT NOT NULL,\n"
        "    command TEXT NOT NULL,\n"
        "    queue_ms INTEGER NOT NULL,\n"
        "    execution_ms INTEGER NOT NULL,\n"
        "    exit_status INTEGER NOT NULL\n"
        ");";

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    return execSql(db, tableSql);
}

/*
 * Create indexes for every registered event table.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreCreateIndexes(sqlite3 *db)
{
    sqlite3_stmt *stmt;
    char (*tables)[LOADER_EVENT_TABLE_NAME_MAX];
    size_t tableCount;
    size_t tableCapacity;
    size_t i;
    int rc;

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    tables = NULL;
    tableCount = 0;
    tableCapacity = 0;

    rc = sqlite3_prepare_v2(
        db,
        "SELECT table_name FROM loader_event_tables ORDER BY day_key",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_event_tables scan failed %s",
                  sqlite3_errmsg(db));
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *tableName;

        tableName = sqlite3_column_text(stmt, 0);
        if (tableName == NULL) {
            sqlite3_finalize(stmt);
            free(tables);
            return -1;
        }
        if (tableCount == tableCapacity) {
            size_t newCapacity;
            void *newTables;

            newCapacity = tableCapacity == 0 ? 16 : tableCapacity * 2;
            newTables = realloc(
                tables, newCapacity * sizeof(*tables));
            if (newTables == NULL) {
                ls_syslog(LOG_ERR, "allocate event table list failed");
                sqlite3_finalize(stmt);
                free(tables);
                return -1;
            }
            tables = newTables;
            tableCapacity = newCapacity;
        }
        snprintf(tables[tableCount], sizeof(tables[tableCount]), "%s",
                 (const char *)tableName);
        tableCount += 1;
    }
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "scan loader_event_tables failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(tables);
        return -1;
    }
    sqlite3_finalize(stmt);
    for (i = 0; i < tableCount; i++) {
        if (createIndexesForTable(db, tables[i], 1) != 0) {
            free(tables);
            return -1;
        }
    }
    free(tables);
    return 0;
}

/*
 * Run ANALYZE on the database.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreAnalyze(sqlite3 *db)
{
    time_t startedAt;

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    startedAt = time(NULL);
    ls_syslog(LOG_INFO, "analyze_start");
    if (execSql(db, "ANALYZE") != 0) {
        ls_syslog(LOG_ERR, "analyze_failed");
        return -1;
    }
    ls_syslog(LOG_INFO, "analyze_done elapsed_sec=%ld",
              (long)(time(NULL) - startedAt));
    return 0;
}

/*
 * Initialize loader metadata.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreInitMeta(sqlite3 *db)
{
    static const struct loaderMetaDefault defaults[] = {
        {"schema_version", "1"},
        {"retained_head_source_id", "0"},
        {"db_status", LOADER_DB_STATUS_READY},
    };
    sqlite3_stmt *stmt;
    size_t i;
    int rc;

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }

    rc = sqlite3_prepare_v2(
        db,
        "INSERT OR REPLACE INTO loader_meta (meta_key, meta_value)"
        " VALUES (?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_meta insert failed %s", sqlite3_errmsg(db));
        return -1;
    }

    for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (bindText(stmt, 1, defaults[i].key) != SQLITE_OK ||
            bindText(stmt, 2, defaults[i].value) != SQLITE_OK) {
            ls_syslog(LOG_ERR, "bind loader_meta failed %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return -1;
        }
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            ls_syslog(LOG_ERR, "insert loader_meta failed %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Read whether the database remains eligible for incremental import.
 * @param[in] db: SQLite connection.
 * @param[out] status: Buffer receiving database status.
 * @param[in] statusLen: Size of status.
 * @return: 0 on success, otherwise -1 for a missing key or read failure.
 */
int
loaderStoreGetDbStatus(sqlite3 *db, char *status, size_t statusLen)
{
    sqlite3_stmt *stmt;
    const unsigned char *value;
    int rc;

    if (db == NULL || status == NULL || statusLen == 0) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }
    rc = sqlite3_prepare_v2(
        db,
        "SELECT meta_value FROM loader_meta WHERE meta_key='db_status'",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare db status read failed %s",
                sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        value = sqlite3_column_text(stmt, 0);
        if (value == NULL || value[0] == '\0') {
            ls_syslog(LOG_ERR, "database status is empty");
            sqlite3_finalize(stmt);
            return -1;
        }
        snprintf(status, statusLen, "%s", (const char *)value);
    } else if (rc == SQLITE_DONE) {
        ls_syslog(LOG_ERR, "database status is missing");
        sqlite3_finalize(stmt);
        return -1;
    } else {
        ls_syslog(LOG_ERR, "read db status failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Verify the loader schema version and reject a database marked for rebuild.
 * @param[in] dbPath: SQLite database path.
 * @param[in] busyTimeoutMs: SQLite busy timeout in milliseconds.
 * @return: 0 for a compatible healthy database, otherwise -1.
 */
int
loaderStoreValidateSchema(const char *dbPath, int busyTimeoutMs)
{
    static const char schemaSql[] =
        "SELECT meta_value FROM loader_meta "
        "WHERE meta_key='schema_version'";
    sqlite3_stmt *statement;
    sqlite3 *db;
    const unsigned char *actual;
    char dbStatus[32];
    char expected[32];
    int compatible;
    int rc;

    db = NULL;
    if (loaderStoreOpenReadonly(dbPath, busyTimeoutMs, &db) != 0) {
        ls_syslog(LOG_ERR,
                "schema_check_open_failed db=%s; run "
                "bhist-speedup-loader rebuild", dbPath);
        return -1;
    }
    statement = NULL;
    rc = sqlite3_prepare_v2(db, schemaSql, -1, &statement, NULL);
    actual = NULL;
    compatible = 0;
    snprintf(expected, sizeof(expected), "%d", LOADER_SCHEMA_VERSION);
    if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        actual = sqlite3_column_text(statement, 0);
        compatible = actual != NULL &&
            strcmp((const char *)actual, expected) == 0;
    }
    if (!compatible) {
        ls_syslog(LOG_ERR,
                "schema_incompatible db=%s expected=%s actual=%s; "
                "run bhist-speedup-loader rebuild",
                dbPath, expected,
                actual != NULL ? (const char *)actual : "missing");
    }
    if (statement != NULL)
        sqlite3_finalize(statement);
    if (compatible &&
        loaderStoreGetDbStatus(db, dbStatus, sizeof(dbStatus)) != 0)
        compatible = 0;
    if (compatible &&
        strcmp(dbStatus, LOADER_DB_STATUS_REBUILD_REQUIRED) == 0) {
        ls_syslog(LOG_ERR,
                "database_rebuild_required db=%s; run "
                "bhist-speedup-loader rebuild",
                dbPath);
        compatible = 0;
    }
    loaderStoreClose(db);
    return compatible ? 0 : -1;
}

/*
 * Persist that the database requires an explicit rebuild.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreMarkRebuildRequired(sqlite3 *db)
{
    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    return execSql(
        db,
        "INSERT OR REPLACE INTO loader_meta(meta_key, meta_value)"
        " VALUES('db_status', 'rebuild_required')");
}

/*
 * Initialize the singleton loader progress row.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreInitProgress(sqlite3 *db)
{
    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    return execSql(
        db,
        "INSERT INTO loader_progress (id) VALUES (1)");
}

/*
 * Begin a SQLite transaction.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreBegin(sqlite3 *db)
{
    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    return execSql(db, "BEGIN");
}

/*
 * Commit a SQLite transaction.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreCommit(sqlite3 *db)
{
    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    return execSql(db, "COMMIT");
}

/*
 * Roll back a SQLite transaction.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreRollback(sqlite3 *db)
{
    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    return execSql(db, "ROLLBACK");
}

/*
 * Load loader progress.
 * @param[in] db: SQLite connection.
 * @param[out] progress: Loaded progress state.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreLoadProgress(sqlite3 *db,
                           struct loaderProgressRow *progress)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL || progress == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }
    memset(progress, 0, sizeof(*progress));

    rc = sqlite3_prepare_v2(
        db,
        "SELECT completed_sources, current_source_id, current_state,"
        "       current_kind, current_path_hint, current_archive_index,"
        "       current_file_ino,"
        "       current_payload_start_off, current_offset,"
        "       current_end_offset,"
        "       current_seen_row_count, total_seen_row_count,"
        "       opened_at, last_visible_max_archive"
        "  FROM loader_progress WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_progress read failed %s", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        ls_syslog(LOG_ERR, "loader_progress row missing");
        sqlite3_finalize(stmt);
        return -1;
    }

    progress->completedSources = sqlite3_column_int64(stmt, 0);
    progress->currentSourceId = sqlite3_column_int64(stmt, 1);
    snprintf(progress->currentState, sizeof(progress->currentState), "%s",
             sqlite3_column_text(stmt, 2) ?
             (const char *)sqlite3_column_text(stmt, 2) : "");
    snprintf(progress->currentKind, sizeof(progress->currentKind), "%s",
             sqlite3_column_text(stmt, 3) ?
             (const char *)sqlite3_column_text(stmt, 3) : "");
    snprintf(progress->currentPathHint,
             sizeof(progress->currentPathHint), "%s",
             sqlite3_column_text(stmt, 4) ?
             (const char *)sqlite3_column_text(stmt, 4) : "");
    progress->hasCurrentArchiveIndex =
        sqlite3_column_type(stmt, 5) != SQLITE_NULL;
    progress->currentArchiveIndex =
        progress->hasCurrentArchiveIndex ? sqlite3_column_int64(stmt, 5) : 0;
    snprintf(progress->currentFileIno,
             sizeof(progress->currentFileIno), "%s",
             sqlite3_column_text(stmt, 6) ?
             (const char *)sqlite3_column_text(stmt, 6) : "");
    progress->currentPayloadStartOff = sqlite3_column_int64(stmt, 7);
    progress->currentOffset = sqlite3_column_int64(stmt, 8);
    progress->hasCurrentEndOffset =
        sqlite3_column_type(stmt, 9) != SQLITE_NULL;
    progress->currentEndOffset =
        progress->hasCurrentEndOffset ? sqlite3_column_int64(stmt, 9) : 0;
    progress->currentSeenRowCount = sqlite3_column_int64(stmt, 10);
    progress->totalSeenRowCount = sqlite3_column_int64(stmt, 11);
    progress->openedAt = sqlite3_column_int64(stmt, 12);
    progress->lastVisibleMaxArchive = sqlite3_column_int64(stmt, 13);
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Replace loader_progress with the identity and offsets of a new source.
 * @param[in] db: SQLite connection.
 * @param[in] source: Fully populated progress state for the source.
 * @return: 0 on success, otherwise -1.
 */
static int
beginCurrentSource(sqlite3 *db,
                   const struct loaderProgressRow *source)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL || source == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    rc = sqlite3_prepare_v2(
        db,
        "UPDATE loader_progress"
        "   SET current_source_id = ?, current_state = ?, current_kind = ?,"
        "       current_path_hint = ?, current_archive_index = ?,"
        "       current_file_ino = ?,"
        "       current_payload_start_off = ?, current_offset = ?,"
        "       current_end_offset = ?,"
        "       current_seen_row_count = 0, opened_at = ?,"
        "       last_visible_max_archive = ?"
        " WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_progress source begin failed %s", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_int64(stmt, 1, source->currentSourceId);
    rc = rc == SQLITE_OK ? bindText(stmt, 2, source->currentState) : rc;
    rc = rc == SQLITE_OK ? bindText(stmt, 3, source->currentKind) : rc;
    rc = rc == SQLITE_OK ? bindText(stmt, 4, source->currentPathHint) : rc;
    rc = rc == SQLITE_OK ? bindOptionalInt64(
        stmt, 5, source->hasCurrentArchiveIndex,
        source->currentArchiveIndex) : rc;
    rc = rc == SQLITE_OK ? bindText(stmt, 6, source->currentFileIno) : rc;
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 7, source->currentPayloadStartOff) : rc;
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 8, source->currentOffset) : rc;
    rc = rc == SQLITE_OK ? bindOptionalInt64(
        stmt, 9, source->hasCurrentEndOffset,
        source->currentEndOffset) : rc;
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 10, source->openedAt) : rc;
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 11, source->lastVisibleMaxArchive) : rc;
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind loader_progress source begin failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "update loader_progress source begin failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Parse an archive index from the final lsb.events.N path component.
 * @param[in] path: Source path.
 * @return: Positive archive index when recognized, otherwise zero.
 */
static long long
parseArchiveIndexFromPath(const char *path)
{
    const char *base;
    const char *prefix;

    if (path == NULL)
        return 0;
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    prefix = "lsb.events.";
    if (strncmp(base, prefix, strlen(prefix)) != 0)
        return 0;
    return atoll(base + strlen(prefix));
}

/*
 * Set a source as the current import object.
 * @param[in] db: SQLite connection.
 * @param[in] source: Source state to store.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreCreateSourceRow(sqlite3 *db,
                               const struct loaderSourceRow *source)
{
    struct loaderProgressRow progress;
    struct loaderProgressRow existing;
    long long visibleMaxArchive;

    if (db == NULL || source == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }
    visibleMaxArchive = 0;
    if (loaderStoreLoadProgress(db, &existing) == 0)
        visibleMaxArchive = existing.lastVisibleMaxArchive;
    memset(&progress, 0, sizeof(progress));
    progress.currentSourceId = source->sourceId;
    snprintf(progress.currentState, sizeof(progress.currentState), "%s",
             "importing");
    snprintf(progress.currentKind, sizeof(progress.currentKind), "%s",
             source->sourceKind != NULL ? source->sourceKind : "none");
    snprintf(progress.currentPathHint, sizeof(progress.currentPathHint),
             "%s", source->pathHint != NULL ? source->pathHint : "");
    progress.currentArchiveIndex =
        strcmp(progress.currentKind, "archive") == 0 ?
        parseArchiveIndexFromPath(source->pathHint) : 0;
    progress.hasCurrentArchiveIndex =
        strcmp(progress.currentKind, "archive") == 0 &&
        progress.currentArchiveIndex > 0;
    if (progress.currentArchiveIndex > visibleMaxArchive)
        visibleMaxArchive = progress.currentArchiveIndex;
    snprintf(progress.currentFileIno, sizeof(progress.currentFileIno),
             "%s", source->fileIno != NULL ? source->fileIno : "");
    progress.currentPayloadStartOff = source->payloadStartOff;
    progress.currentOffset = source->currentOffset;
    progress.hasCurrentEndOffset = source->hasEndOffset;
    progress.currentEndOffset = source->endOffset;
    progress.openedAt = source->openedAt;
    progress.lastVisibleMaxArchive = visibleMaxArchive;
    return beginCurrentSource(db, &progress);
}

/*
 * Skip completed sources outside the import window.
 * @param[in] db: SQLite connection.
 * @param[in] skippedSources: Number of sources to skip.
 * @param[in] lastVisibleMaxArchive: Highest currently visible archive index.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreSkipCompletedSources(sqlite3 *db,
                                    long long skippedSources,
                                    long long lastVisibleMaxArchive)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL || skippedSources <= 0) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    rc = sqlite3_prepare_v2(
        db,
        "UPDATE loader_progress"
        "   SET completed_sources = completed_sources + ?,"
        "       current_source_id = current_source_id + ?,"
        "       last_visible_max_archive = ?"
        " WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_progress archive skip failed %s",
                  sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_int64(stmt, 1, skippedSources);
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 2, skippedSources) : rc;
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(
        stmt, 3, lastVisibleMaxArchive) : rc;
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind loader_progress archive skip failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "update loader_progress archive skip failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Read the source ID used by the last retained-head refresh.
 * @param[in] db: SQLite connection.
 * @param[out] sourceIdOut: Stored retained-head source ID.
 * @return: 0 on success, 1 when metadata is missing, otherwise -1.
 */
int
loaderStoreReadRetainedHeadSourceId(sqlite3 *db, long long *sourceIdOut)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL || sourceIdOut == NULL)
        return -1;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT CAST(meta_value AS INTEGER) FROM loader_meta"
        " WHERE meta_key='retained_head_source_id'",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare retained head metadata read failed %s",
                  sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        ls_syslog(LOG_ERR, "retained head metadata missing");
        sqlite3_finalize(stmt);
        return 1;
    }
    *sourceIdOut = sqlite3_column_int64(stmt, 0);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "read retained head metadata failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Find the first registered day on or after the retention cutoff.
 * @param[in] db: SQLite connection.
 * @param[in] cutoffDay: Inclusive YYYYMMDD cutoff.
 * @param[out] floorDay: Effective retained day.
 * @param[in] floorDayLen: Size of floorDay.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreFindRetainedFloorDay(sqlite3 *db, const char *cutoffDay,
                                char *floorDay, size_t floorDayLen)
{
    sqlite3_stmt *stmt;
    const unsigned char *value;
    int rc;

    if (db == NULL || cutoffDay == NULL || floorDay == NULL ||
        floorDayLen == 0)
        return -1;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT MIN(day_key) FROM loader_event_tables"
        " WHERE day_key >= ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, cutoffDay, -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare retained day lookup failed %s",
                  sqlite3_errmsg(db));
        if (rc == SQLITE_OK)
            sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        ls_syslog(LOG_ERR, "read retained day failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
        snprintf(floorDay, floorDayLen, "%s", cutoffDay);
    } else {
        value = sqlite3_column_text(stmt, 0);
        if (value == NULL || sqlite3_column_bytes(stmt, 0) != 8) {
            ls_syslog(LOG_ERR, "invalid retained day");
            sqlite3_finalize(stmt);
            return -1;
        }
        snprintf(floorDay, floorDayLen, "%s", (const char *)value);
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "finish retained day read failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Load and validate registered event tables older than the retention floor.
 * @param[in] db: SQLite connection.
 * @param[in] floorDay: Exclusive YYYYMMDD retention floor.
 * @param[out] tablesOut: Allocated array of validated table names.
 * @param[out] countOut: Number of names in tablesOut.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreLoadExpiredEventTables(
    sqlite3 *db, const char *floorDay,
    char (**tablesOut)[LOADER_EVENT_TABLE_NAME_MAX], size_t *countOut)
{
    char (*tables)[LOADER_EVENT_TABLE_NAME_MAX];
    size_t count;
    size_t capacity;
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL || floorDay == NULL || tablesOut == NULL ||
        countOut == NULL)
        return -1;
    *tablesOut = NULL;
    *countOut = 0;
    tables = NULL;
    count = 0;
    capacity = 0;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT day_key, table_name FROM loader_event_tables"
        " WHERE day_key < ? ORDER BY day_key",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, floorDay, -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare expired event table scan failed %s",
                  sqlite3_errmsg(db));
        if (rc == SQLITE_OK)
            sqlite3_finalize(stmt);
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *dayKey;
        const unsigned char *tableName;
        char expectedTable[LOADER_EVENT_TABLE_NAME_MAX];

        dayKey = sqlite3_column_text(stmt, 0);
        tableName = sqlite3_column_text(stmt, 1);
        if (dayKey == NULL || tableName == NULL ||
            sqlite3_column_bytes(stmt, 0) != 8 ||
            snprintf(expectedTable, sizeof(expectedTable),
                     "job_event_kv_%s", (const char *)dayKey) >=
                (int)sizeof(expectedTable) ||
            strcmp(expectedTable, (const char *)tableName) != 0) {
            ls_syslog(LOG_ERR, "invalid expired event table registry row");
            sqlite3_finalize(stmt);
            free(tables);
            return -1;
        }
        if (count == capacity) {
            size_t newCapacity;
            void *newTables;

            newCapacity = capacity == 0 ? 8 : capacity * 2;
            newTables = realloc(tables, newCapacity * sizeof(*tables));
            if (newTables == NULL) {
                ls_syslog(LOG_ERR,
                          "allocate expired event table list failed");
                sqlite3_finalize(stmt);
                free(tables);
                return -1;
            }
            tables = newTables;
            capacity = newCapacity;
        }
        snprintf(tables[count], sizeof(tables[count]), "%s",
                 (const char *)tableName);
        count++;
    }
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "scan expired event tables failed %s",
                  sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(tables);
        return -1;
    }
    sqlite3_finalize(stmt);
    *tablesOut = tables;
    *countOut = count;
    return 0;
}

/*
 * Drop expired event tables and delete their registry rows.
 * @param[in] db: SQLite connection inside the refresh transaction.
 * @param[in] floorDay: Exclusive YYYYMMDD retention floor.
 * @param[in] tables: Validated table names to drop.
 * @param[in] tableCount: Number of entries in tables.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreDropExpiredEventTables(
    sqlite3 *db, const char *floorDay,
    char (*tables)[LOADER_EVENT_TABLE_NAME_MAX], size_t tableCount)
{
    sqlite3_stmt *stmt;
    size_t i;
    int rc;

    if (db == NULL || floorDay == NULL ||
        (tableCount > 0 && tables == NULL))
        return -1;
    for (i = 0; i < tableCount; i++) {
        char *sql;
        char *sqliteError;

        sql = sqlite3_mprintf("DROP TABLE \"%w\"", tables[i]);
        if (sql == NULL)
            return -1;
        sqliteError = NULL;
        ls_syslog(LOG_INFO, "retention_drop_expired_table table=%s",
                  tables[i]);
        rc = sqlite3_exec(db, sql, NULL, NULL, &sqliteError);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) {
            ls_syslog(LOG_ERR,
                      "drop expired event table failed table=%s error=%s",
                      tables[i], sqliteError != NULL ? sqliteError :
                      sqlite3_errmsg(db));
            sqlite3_free(sqliteError);
            return -1;
        }
        sqlite3_free(sqliteError);
    }

    rc = sqlite3_prepare_v2(
        db, "DELETE FROM loader_event_tables WHERE day_key < ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, floorDay, -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) {
        ls_syslog(LOG_ERR,
                  "delete expired event table registry failed %s",
                  sqlite3_errmsg(db));
        if (rc == SQLITE_OK)
            sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Record the source generation handled by a retained-head refresh.
 * @param[in] db: SQLite connection inside the refresh transaction.
 * @param[in] sourceId: Refreshed source ID.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreUpdateRetainedHeadSourceId(sqlite3 *db, long long sourceId)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL)
        return -1;
    rc = sqlite3_prepare_v2(
        db,
        "INSERT OR REPLACE INTO loader_meta(meta_key, meta_value)"
        " VALUES('retained_head_source_id', ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 1, sourceId) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "update retained head metadata failed %s",
                  sqlite3_errmsg(db));
        if (rc == SQLITE_OK)
            sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Open an event writer that updates progress per row.
 * @param[in] db: SQLite connection.
 * @param[out] writer: Open event writer.
 * @param[in] dayKey: Target local-time day key.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreEventWriterOpen(sqlite3 *db,
                               struct loaderStoreEventWriter *writer,
                               const char *dayKey)
{
    return openEventWriter(db, writer, dayKey, 1, 1);
}

/*
 * Open an event writer for bulk progress updates.
 * @param[in] db: SQLite connection.
 * @param[out] writer: Open event writer.
 * @param[in] dayKey: Target local-time day key.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreEventWriterOpenBulk(sqlite3 *db,
                                    struct loaderStoreEventWriter *writer,
                                    const char *dayKey)
{
    return openEventWriter(db, writer, dayKey, 0, 0);
}

/*
 * Open an event writer that does not update progress.
 * @param[in] db: SQLite connection.
 * @param[out] writer: Open event writer.
 * @param[in] dayKey: Target local-time day key.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreEventWriterOpenUntracked(
    sqlite3 *db,
    struct loaderStoreEventWriter *writer,
    const char *dayKey)
{
    return openEventWriter(db, writer, dayKey, 0, 1);
}

/*
 * Open an event writer with explicit progress and index behavior.
 * @param[in] db: SQLite connection.
 * @param[out] writer: Writer receiving prepared statements.
 * @param[in] dayKey: Target local-time day key.
 * @param[in] updateSeenPerRow: Nonzero to update progress per inserted row.
 * @param[in] createFinalIndexes: Nonzero to create query indexes immediately.
 * @return: 0 on success, otherwise -1.
 */
static int
openEventWriter(sqlite3 *db,
                struct loaderStoreEventWriter *writer,
                const char *dayKey,
                int updateSeenPerRow,
                int createFinalIndexes)
{
    if (db == NULL || writer == NULL || dayKey == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    memset(writer, 0, sizeof(*writer));
    writer->updateSeenPerRow = updateSeenPerRow ? 1 : 0;
    if (ensureEventTable(
        db, dayKey, writer->tableName, sizeof(writer->tableName),
        createFinalIndexes) != 0)
        return -1;
    snprintf(writer->dayKey, sizeof(writer->dayKey), "%s", dayKey);

    return prepareEventWriter(
        db, writer, writer->tableName, updateSeenPerRow);
}

/*
 * Prepare insertion and optional progress statements for an event writer.
 * @param[in] db: SQLite connection.
 * @param[in,out] writer: Writer receiving prepared statements.
 * @param[in] tableName: Existing target daily table.
 * @param[in] updateSeenPerRow: Nonzero to prepare per-row progress updates.
 * @return: 0 on success, otherwise -1.
 */
static int
prepareEventWriter(sqlite3 *db,
                   struct loaderStoreEventWriter *writer,
                   const char *tableName,
                   int updateSeenPerRow)
{
    char tableCopy[LOADER_EVENT_TABLE_NAME_MAX];
    char sql[512];
    int rc;

    if (db == NULL || writer == NULL || tableName == NULL ||
        tableName[0] == '\0') {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    snprintf(tableCopy, sizeof(tableCopy), "%s", tableName);
    writer->updateSeenPerRow = updateSeenPerRow ? 1 : 0;
    snprintf(writer->tableName, sizeof(writer->tableName), "%s",
             tableCopy);

    snprintf(sql, sizeof(sql),
             "INSERT INTO %s"
             " (raw_line, job_id,"
             "  event_time, submit_time, submit_user, submit_queue,"
             "  start_time, end_time)"
             " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
             writer->tableName);
    rc = sqlite3_prepare_v2(
        db, sql,
        -1, &writer->insertEventStmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare job_event insert failed %s", sqlite3_errmsg(db));
        loaderStoreEventWriterClose(writer);
        return -1;
    }

    if (updateSeenPerRow) {
        rc = sqlite3_prepare_v2(
            db,
            "UPDATE loader_progress"
            "   SET current_seen_row_count = current_seen_row_count + 1,"
            "       total_seen_row_count = total_seen_row_count + 1"
            " WHERE id = 1",
            -1, &writer->updateSourceSeenStmt, NULL);
        if (rc != SQLITE_OK) {
            ls_syslog(LOG_ERR, "prepare source seen count failed %s", sqlite3_errmsg(db));
            loaderStoreEventWriterClose(writer);
            return -1;
        }
    }

    return 0;
}

/*
 * Close an event writer.
 * @param[in,out] writer: Writer to close and reset.
 * @return: None.
 */
void
loaderStoreEventWriterClose(struct loaderStoreEventWriter *writer)
{
    if (writer == NULL)
        return;
    if (writer->insertEventStmt != NULL)
        sqlite3_finalize(writer->insertEventStmt);
    if (writer->updateSourceSeenStmt != NULL)
        sqlite3_finalize(writer->updateSourceSeenStmt);
    memset(writer, 0, sizeof(*writer));
}

/*
 * Insert one raw event and its filter metadata.
 * @param[in] db: SQLite connection.
 * @param[in,out] writer: Event writer.
 * @param[in] jobId: Base job ID.
 * @param[in] rawLine: Original event text.
 * @param[in] times: Event time and filter metadata.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreEventWriterInsert(sqlite3 *db,
                                 struct loaderStoreEventWriter *writer,
                                 long long jobId,
                                 const char *rawLine,
                                 const struct loaderEventFields *times)
{
    int rc;

    if (db == NULL || writer == NULL || writer->insertEventStmt == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }

    /* Resume safety comes from committing event rows and source offset together. */
    sqlite3_reset(writer->insertEventStmt);
    sqlite3_clear_bindings(writer->insertEventStmt);
    rc = bindText(writer->insertEventStmt, 1, rawLine);
    rc = rc == SQLITE_OK ?
        sqlite3_bind_int64(writer->insertEventStmt, 2, jobId) : rc;
    rc = rc == SQLITE_OK ?
        bindOptionalInt64(
            writer->insertEventStmt, 3,
            times != NULL && times->hasEventTime,
            times != NULL ? times->eventTime : 0) : rc;
    rc = rc == SQLITE_OK ?
        bindOptionalInt64(
            writer->insertEventStmt, 4,
            times != NULL && times->hasSubmitTime,
            times != NULL ? times->submitTime : 0) : rc;
    rc = rc == SQLITE_OK ?
        bindOptionalText(
            writer->insertEventStmt, 5,
            times != NULL && times->hasSubmitUser,
            times != NULL ? times->submitUser : NULL) : rc;
    rc = rc == SQLITE_OK ?
        bindOptionalText(
            writer->insertEventStmt, 6,
            times != NULL && times->hasSubmitQueue,
            times != NULL ? times->submitQueue : NULL) : rc;
    rc = rc == SQLITE_OK ?
        bindOptionalInt64(
            writer->insertEventStmt, 7,
            times != NULL && times->hasStartTime,
            times != NULL ? times->startTime : 0) : rc;
    rc = rc == SQLITE_OK ?
        bindOptionalInt64(
            writer->insertEventStmt, 8,
            times != NULL && times->hasEndTime,
            times != NULL ? times->endTime : 0) : rc;
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind job_event insert failed %s", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(writer->insertEventStmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "insert job_event failed %s", sqlite3_errmsg(db));
        return -1;
    }

    if (writer->updateSeenPerRow) {
        if (writer->updateSourceSeenStmt == NULL) {
            ls_syslog(LOG_ERR, "invalid source seen count statement");
            return -1;
        }
        sqlite3_reset(writer->updateSourceSeenStmt);
        sqlite3_clear_bindings(writer->updateSourceSeenStmt);
        rc = sqlite3_step(writer->updateSourceSeenStmt);
        if (rc != SQLITE_DONE) {
            ls_syslog(LOG_ERR, "update source seen count failed %s", sqlite3_errmsg(db));
            return -1;
        }
    }

    return 0;
}

/*
 * Update the seen-event count for the current source.
 * @param[in] db: SQLite connection.
 * @param[in] sourceId: Source identifier.
 * @param[in] seenRowCount: Number of seen events.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreUpdateSourceSeenCount(sqlite3 *db, long long sourceId,
                                      long long seenRowCount)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }

    rc = sqlite3_prepare_v2(
        db,
        "UPDATE loader_progress"
        "   SET current_seen_row_count = ?,"
        "       total_seen_row_count ="
        "           total_seen_row_count + (? - current_seen_row_count)"
        " WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare source seen count failed %s", sqlite3_errmsg(db));
        return -1;
    }

    rc = sqlite3_bind_int64(stmt, 1, seenRowCount);
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 2, seenRowCount) : rc;
    (void)sourceId;
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind source seen count failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "update source seen count failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Remap the current source to its rotated archive path.
 * @param[in] db: SQLite connection.
 * @param[in] fileIno: File inode number.
 * @param[in] pathHint: Archive path.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreRemapCurrentSourceToArchive(sqlite3 *db,
                                             const char *fileIno,
                                             const char *pathHint)
{
    sqlite3_stmt *stmt;
    long long archiveIndex;
    int rc;

    if (db == NULL || fileIno == NULL || pathHint == NULL) {
        ls_syslog(LOG_ERR, "invalid argument");
        return -1;
    }
    archiveIndex = parseArchiveIndexFromPath(pathHint);
    rc = sqlite3_prepare_v2(
        db,
        "UPDATE loader_progress"
        "   SET current_kind = 'archive',"
        "       current_path_hint = ?, current_archive_index = ?,"
        "       current_file_ino = ?,"
        "       current_payload_start_off = 0"
        " WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_progress remap failed %s", sqlite3_errmsg(db));
        return -1;
    }
    rc = bindText(stmt, 1, pathHint);
    rc = rc == SQLITE_OK ? sqlite3_bind_int64(stmt, 2, archiveIndex) : rc;
    rc = rc == SQLITE_OK ? bindText(stmt, 3, fileIno) : rc;
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind loader_progress remap failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "update loader_progress remap failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}
/*
 * Update the committed offset of the current source.
 * @param[in] db: SQLite connection.
 * @param[in] currentOffset: New committed offset.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreUpdateCurrentOffset(sqlite3 *db,
                                   long long currentOffset)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    rc = sqlite3_prepare_v2(
        db,
        "UPDATE loader_progress SET current_offset = ? WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_progress offset failed %s", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_int64(stmt, 1, currentOffset);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "bind loader_progress offset failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "update loader_progress offset failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Mark the current source complete and advance its source ID.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
static int
advanceCompletedSource(sqlite3 *db)
{
    sqlite3_stmt *stmt;
    int rc;

    if (db == NULL) {
        ls_syslog(LOG_ERR, "invalid database handle");
        return -1;
    }
    rc = sqlite3_prepare_v2(
        db,
        "UPDATE loader_progress"
        "   SET completed_sources = completed_sources + 1,"
        "       current_source_id = current_source_id + 1,"
        "       current_state = 'idle',"
        "       current_kind = 'none',"
        "       current_path_hint = '',"
        "       current_archive_index = NULL,"
        "       current_file_ino = '',"
        "       current_payload_start_off = 0,"
        "       current_offset = 0,"
        "       current_end_offset = NULL,"
        "       current_seen_row_count = 0"
        " WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ls_syslog(LOG_ERR, "prepare loader_progress advance failed %s", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ls_syslog(LOG_ERR, "advance loader_progress source failed %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * Finish the current source.
 * @param[in] db: SQLite connection.
 * @return: 0 on success, otherwise -1.
 */
int
loaderStoreFinishCurrentSource(sqlite3 *db)
{
    return advanceCompletedSource(db);
}

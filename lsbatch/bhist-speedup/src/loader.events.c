/*
 * Event-log discovery, parsing, and import pipeline for the loader.
 *
 * This module owns the lsb.events side of the loader: it snapshots current and
 * archived generations, selects resumable source ranges, parses event records,
 * and imports them in bounded transactions through loader.db.c.
 */

#include "loader.h"
#include "lproto.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* Event-log generation discovery and source planning. */
#define LOADER_FILE_DISCOVERY_STALE 1

/* Describes one stable snapshot of lsb.events or a numbered archive. */
struct loaderGeneration {
    char kind[16];
    char path[LOADER_PATH_MAX];
    char fileIno[32];
    long archiveIndex;
    long long mtimeSec;
    long mtimeNsec;
    long long payloadStartOff;
    long long endOffset;
};

static int scanVisibleGenerations(
    const char *logdir, struct loaderGeneration **outItems,
    size_t *outCount);
static void freeGenerations(struct loaderGeneration *items);
static long findMaxArchiveIndex(
    const struct loaderGeneration *items, size_t count);
static const struct loaderGeneration *findArchive(
    const struct loaderGeneration *items, size_t count, long archiveIndex);
static const struct loaderGeneration *findCurrentGeneration(
    const struct loaderGeneration *items, size_t count);
/*
 * Convert loader_progress and a stable generation snapshot into an executable
 * source plan.
 *
 * Larger lsb.events.N suffixes are normally older. The archive queue runs from
 * the oldest unfinished archive toward the newest before entering current.
 * After rotation, current may appear under an archive path with the same
 * inode; planning remaps that source instead of importing it twice.
 *
 * Planning owns no FILE and reads no raw events. A directory change returns
 * STALE_REPLAN so loader.main.c can compute a new plan from a fresh snapshot.
 */

/*
 * Build the path of the live lsb.events file.
 * @param[in] logdir: Event log directory.
 * @param[out] out: Buffer receiving the live source path.
 * @param[in] outLen: Size of out.
 * @return: Nonzero when the path fits, otherwise zero.
 */
static int
buildCurrentPath(const char *logdir, char *out, size_t outLen)
{
    int written;

    written = snprintf(out, outLen, "%s/lsb.events", logdir);
    return written >= 0 && (size_t)written < outLen;
}

/*
 * Count consecutive recent archives starting at lsb.events.1.
 * @param[in] generations: Stable generation snapshot.
 * @param[in] generationCount: Number of snapshot entries.
 * @param[in] maxArchiveIndex: Highest visible archive suffix.
 * @param[in] cutoffTime: Oldest accepted archive modification time.
 * @return: Largest recent archive index, or zero when none is recent.
 */
static long
countRecentArchives(const struct loaderGeneration *generations,
                    size_t generationCount,
                    long maxArchiveIndex,
                    long long cutoffTime)
{
    long recentCount;
    long archiveIndex;

    recentCount = 0;
    for (archiveIndex = 1; archiveIndex <= maxArchiveIndex;
         archiveIndex++) {
        const struct loaderGeneration *archive;

        archive = findArchive(generations, generationCount,
                              archiveIndex);
        if (archive == NULL || archive->mtimeSec < cutoffTime)
            break;
        recentCount = archiveIndex;
    }
    return recentCount;
}

/*
 * Advance initial progress past archives older than the retention window.
 * @param[in] db: SQLite connection.
 * @param[in,out] progress: Progress row updated after a successful skip.
 * @param[in] generations: Stable generation snapshot.
 * @param[in] generationCount: Number of snapshot entries.
 * @param[in] maxArchiveIndex: Highest visible archive suffix.
 * @param[in] historyRetentionSeconds: Retention window in seconds.
 * @param[in] now: Current Unix time.
 * @return: 0 on success, otherwise -1.
 */
static int
skipOldArchives(sqlite3 *db,
                struct loaderProgressRow *progress,
                const struct loaderGeneration *generations,
                size_t generationCount,
                long maxArchiveIndex,
                long long historyRetentionSeconds,
                long long now)
{
    long recentCount;
    long long desiredCompleted;
    long long skippedSources;
    long long cutoffTime;

    cutoffTime = now - historyRetentionSeconds;
    recentCount = countRecentArchives(
        generations, generationCount, maxArchiveIndex, cutoffTime);
    desiredCompleted = (long long)maxArchiveIndex - recentCount;
    if (desiredCompleted <= progress->completedSources)
        return 0;

    skippedSources = desiredCompleted - progress->completedSources;
    if (loaderStoreSkipCompletedSources(
            db, skippedSources, maxArchiveIndex) != 0)
        return -1;

    progress->completedSources += skippedSources;
    progress->currentSourceId += skippedSources;
    progress->lastVisibleMaxArchive = maxArchiveIndex;
    ls_syslog(LOG_INFO,
              "archive_skip_old skipped_archives=%lld cutoff_time=%lld "
              "window_seconds=%lld max_archive=%ld first_recent_archive=%ld",
              skippedSources, cutoffTime, historyRetentionSeconds,
              maxArchiveIndex, recentCount);
    return 0;
}

/*
 * Read the live source identity at the configured event log directory.
 * @param[in] logdir: Event log directory.
 * @param[out] path: Buffer receiving the live source path.
 * @param[in] pathLen: Size of path.
 * @param[out] fileIno: Buffer receiving inode number text.
 * @param[in] fileInoLen: Size of fileIno.
 * @param[out] payloadStartOff: Optional payload offset, currently zero.
 * @param[out] fileSize: Optional current file size.
 * @return: 0 on success, otherwise -1.
 */
static int
statCurrentSource(const char *logdir,
                  char *path,
                  size_t pathLen,
                  char *fileIno,
                  size_t fileInoLen,
                  long long *payloadStartOff,
                  long long *fileSize)
{
    struct stat st;

    if (!buildCurrentPath(logdir, path, pathLen)) {
        ls_syslog(LOG_ERR, "current_event_path_too_long logdir=%s", logdir);
        return -1;
    }
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            ls_syslog(LOG_ERR, "current_lsb_events_not_found file=%s", path);
        else
            ls_syslog(LOG_ERR, "stat_current_failed file=%s: %m", path);
        return -1;
    }
    snprintf(fileIno, fileInoLen, "%llu", (unsigned long long)st.st_ino);
    if (payloadStartOff != NULL)
        *payloadStartOff = 0;
    if (fileSize != NULL)
        *fileSize = (long long)st.st_size;
    return 0;
}

/*
 * Copy a discovered generation into an executable source plan.
 * @param[out] plan: Plan receiving stable source identity and bounds.
 * @param[in] kind: Archive or live source kind.
 * @param[in] generation: Discovered generation snapshot.
 * @return: None.
 */
static void
initPlanFromGeneration(struct loaderSourcePlan *plan,
                       enum loaderSourceKind kind,
                       const struct loaderGeneration *generation)
{
    plan->kind = kind;
    snprintf(plan->path, sizeof(plan->path), "%s", generation->path);
    snprintf(plan->fileIno, sizeof(plan->fileIno), "%s",
             generation->fileIno);
    plan->payloadStartOff = generation->payloadStartOff;
    plan->hasEndOffset = kind == LOADER_SOURCE_ARCHIVE;
    plan->endOffset = kind == LOADER_SOURCE_ARCHIVE ?
        generation->endOffset : 0;
}

/*
 * Register the current source in the database.
 * @param[in] db: SQLite connection.
 * @param[in] plan: Source plan to register.
 * @param[in] openedAt: Source open time.
 * @return: 0 on success, otherwise -1.
 */
int
loaderPlanBeginSource(sqlite3 *db,
                         const struct loaderSourcePlan *plan,
                         long long openedAt)
{
    struct loaderSourceRow source;
    int inTransaction;

    memset(&source, 0, sizeof(source));
    source.sourceId = plan->sourceId;
    source.sourceKind = plan->kind == LOADER_SOURCE_ARCHIVE ?
        "archive" : "live_current";
    source.fileIno = plan->fileIno;
    source.pathHint = plan->path;
    source.payloadStartOff = plan->payloadStartOff;
    source.currentOffset = plan->currentOffset;
    source.hasEndOffset = plan->hasEndOffset;
    source.endOffset = plan->endOffset;
    source.openedAt = openedAt;

    inTransaction = 0;
    if (loaderStoreBegin(db) != 0)
        return -1;
    inTransaction = 1;
    if (loaderStoreCreateSourceRow(db, &source) != 0)
        goto fail;
    if (loaderStoreCommit(db) != 0)
        return -1;
    return 0;

fail:
    if (inTransaction)
        loaderStoreRollback(db);
    return -1;
}

/*
 * Release an archive plan queue.
 * @param[in,out] queue: Queue to release and reset.
 * @return: None.
 */
void
loaderPlanFreeArchiveQueue(struct loaderArchiveQueue *queue)
{
    if (queue == NULL)
        return;
    free(queue->plans);
    memset(queue, 0, sizeof(*queue));
}

/*
 * Resolve one archive index from a stable scan into a resumable source plan.
 * @param[in] db: SQLite connection.
 * @param[in] progress: Current loader progress.
 * @param[in] generations: Stable generation snapshot.
 * @param[in] generationCount: Number of snapshot entries.
 * @param[in] maxArchiveIndex: Highest visible archive suffix.
 * @param[in] archiveIndex: Archive suffix to plan.
 * @param[in] sourceExists: Nonzero when progress already owns this source.
 * @param[in] markRemapped: Nonzero when live progress must become archive.
 * @param[in] openedAt: Source open time.
 * @param[out] plan: Executable archive plan.
 * @return: A loaderStepResult value.
 */
static enum loaderStepResult
planArchiveFromScan(sqlite3 *db,
                    struct loaderProgressRow *progress,
                    struct loaderGeneration *generations,
                    size_t generationCount,
                    long maxArchiveIndex,
                    long archiveIndex,
                    int sourceExists,
                    int markRemapped,
                    long long openedAt,
                    struct loaderSourcePlan *plan)
{
    const struct loaderGeneration *archive;

    if (archiveIndex <= 0) {
        ls_syslog(LOG_ERR,
                  "completed_source_count_exceeds_retained_archives");
        return LOADER_STEP_REBUILD_REQUIRED;
    }
    if (archiveIndex > maxArchiveIndex) {
        ls_syslog(LOG_ERR,
                  "target_archive_outside_retained_window "
                  "archive_index=%ld max_archive=%ld",
                  archiveIndex, maxArchiveIndex);
        return LOADER_STEP_REBUILD_REQUIRED;
    }
    archive = findArchive(generations, generationCount,
                          archiveIndex);
    if (archive == NULL) {
        ls_syslog(LOG_ERR,
                  "target_archive_for_completed_source_count_missing "
                  "archive_index=%ld",
                  archiveIndex);
        return LOADER_STEP_REBUILD_REQUIRED;
    }
    /*
     * Archive rotation renames an existing archive and preserves its inode.
     * A live source is copied into lsb.events.0 instead, so its remap must not
     * compare the archive inode with the inode of the old live file.
     */
    if (sourceExists && !markRemapped &&
        strcmp(archive->fileIno, progress->currentFileIno) != 0) {
        ls_syslog(LOG_ERR,
                  "archive_generation_mismatch source_id=%lld "
                  "archive_index=%ld expected_ino=%s actual_ino=%s",
                  progress->currentSourceId, archiveIndex,
                  progress->currentFileIno, archive->fileIno);
        return LOADER_STEP_REBUILD_REQUIRED;
    }
    initPlanFromGeneration(plan, LOADER_SOURCE_ARCHIVE, archive);
    plan->sourceId = progress->currentSourceId;
    plan->completedSources = progress->completedSources;
    plan->currentOffset = sourceExists ? progress->currentOffset : 0;
    plan->seenRowCount = sourceExists ?
        progress->currentSeenRowCount : 0;
    plan->markRemapped = markRemapped;
    if (!sourceExists) {
        if (loaderPlanBeginSource(db, plan, openedAt) != 0)
            return LOADER_STEP_FATAL;
    }
    return LOADER_STEP_COMMITTED;
}

/*
 * Build the queue of archive sources that still require import.
 * @param[in] logdir: Event log directory.
 * @param[in] db: SQLite connection.
 * @param[in] historyRetentionSeconds: Archive import window in seconds.
 * @param[out] queue: Planned archive queue.
 * @return: A loaderStepResult value.
 */
int
loaderPlanArchiveQueue(const char *logdir,
                          sqlite3 *db,
                          long long historyRetentionSeconds,
                          struct loaderArchiveQueue *queue)
{
    struct loaderProgressRow progress;
    struct loaderGeneration *generations;
    struct loaderSourcePlan *plans;
    size_t generationCount;
    long maxArchiveIndex;
    long remaining;
    long i;
    int discoverRc;

    if (logdir == NULL || db == NULL || queue == NULL ||
        historyRetentionSeconds <= 0) {
        ls_syslog(LOG_ERR, "invalid_archive_queue_argument");
        return LOADER_STEP_FATAL;
    }

    loaderPlanFreeArchiveQueue(queue);
    generations = NULL;
    generationCount = 0;
    plans = NULL;

    if (loaderStoreLoadProgress(db, &progress) != 0)
        return LOADER_STEP_FATAL;
    if (strcmp(progress.currentState, "idle") != 0 ||
        (strcmp(progress.currentKind, "none") != 0 &&
         progress.currentKind[0] != '\0'))
        return LOADER_STEP_IDLE;

    discoverRc = scanVisibleGenerations(
        logdir, &generations, &generationCount);
    if (discoverRc == LOADER_FILE_DISCOVERY_STALE)
        return LOADER_STEP_STALE_REPLAN;
    if (discoverRc != 0)
        return LOADER_STEP_FATAL;

    maxArchiveIndex = findMaxArchiveIndex(generations,
                                          generationCount);
    if (skipOldArchives(
        db, &progress, generations, generationCount, maxArchiveIndex,
        historyRetentionSeconds, (long long)time(NULL)) != 0) {
        freeGenerations(generations);
        return LOADER_STEP_FATAL;
    }
    remaining = maxArchiveIndex - (long)progress.completedSources;
    if (remaining < 0) {
        ls_syslog(LOG_ERR,
                  "completed_source_count_exceeds_retained_archives "
                  "completed_sources=%lld max_archive=%ld",
                  progress.completedSources, maxArchiveIndex);
        freeGenerations(generations);
        return LOADER_STEP_REBUILD_REQUIRED;
    }
    if (remaining == 0) {
        freeGenerations(generations);
        return LOADER_STEP_IDLE;
    }

    plans = calloc((size_t)remaining, sizeof(*plans));
    if (plans == NULL) {
        ls_syslog(LOG_ERR, "archive_queue_alloc_failed: %m");
        freeGenerations(generations);
        return LOADER_STEP_FATAL;
    }
    /* Import remaining contiguous archives from oldest to newest. */
    for (i = 0; i < remaining; i++) {
        long archiveIndex;
        const struct loaderGeneration *archive;
        struct loaderSourcePlan *plan;

        archiveIndex = remaining - i;
        archive = findArchive(generations, generationCount,
                              archiveIndex);
        if (archive == NULL) {
            free(plans);
            freeGenerations(generations);
            ls_syslog(LOG_ERR,
                      "target_archive_for_completed_source_count_missing "
                      "archive_index=%ld",
                      archiveIndex);
            return LOADER_STEP_REBUILD_REQUIRED;
        }
        plan = &plans[i];
        initPlanFromGeneration(plan, LOADER_SOURCE_ARCHIVE, archive);
        plan->sourceId = progress.currentSourceId + i;
        plan->completedSources = progress.completedSources + i;
    }

    freeGenerations(generations);
    queue->plans = plans;
    queue->count = (size_t)remaining;
    queue->pos = 0;
    queue->firstArchiveIndex = remaining;
    queue->lastArchiveIndex = 1;
    queue->maxArchiveIndex = maxArchiveIndex;
    queue->completedSources = progress.completedSources;
    return LOADER_STEP_COMMITTED;
}

/*
 * Plan the next source to process.
 * @param[in] logdir: Event log directory.
 * @param[in] db: SQLite connection.
 * @param[in] historyRetentionSeconds: Archive import window in seconds.
 * @param[in] openedAt: Source open time.
 * @param[out] plan: Planned next source.
 * @return: A loaderStepResult value.
 */
int
loaderPlanNextSource(const char *logdir,
                        sqlite3 *db,
                        long long historyRetentionSeconds,
                        long long openedAt,
                        struct loaderSourcePlan *plan)
{
    struct loaderProgressRow progress;
    struct loaderGeneration *generations;
    const struct loaderGeneration *current;
    char currentPath[LOADER_PATH_MAX];
    char currentIno[32];
    long long currentSize;
    size_t generationCount;
    long maxArchiveIndex;
    int discoverRc;

    if (logdir == NULL || db == NULL || plan == NULL ||
        historyRetentionSeconds <= 0) {
        ls_syslog(LOG_ERR, "invalid_next_source_argument");
        return LOADER_STEP_FATAL;
    }

    memset(plan, 0, sizeof(*plan));
    generations = NULL;
    generationCount = 0;
    current = NULL;

    if (loaderStoreLoadProgress(db, &progress) != 0)
        return LOADER_STEP_FATAL;

    if (strcmp(progress.currentState, "importing") == 0 &&
        strcmp(progress.currentKind, "live_current") == 0) {
        int currentStatRc;

        currentStatRc = statCurrentSource(
            logdir, currentPath, sizeof(currentPath), currentIno,
            sizeof(currentIno), NULL, &currentSize);
        if (currentStatRc == 0 &&
            strcmp(currentIno, progress.currentFileIno) == 0) {
            if (currentSize < progress.currentPayloadStartOff +
                              progress.currentOffset) {
                ls_syslog(LOG_ERR,
                          "live_event_file_size_regressed "
                          "source_id=%lld inode=%s size=%lld "
                          "committed_position=%lld",
                          progress.currentSourceId, currentIno, currentSize,
                          progress.currentPayloadStartOff +
                          progress.currentOffset);
                return LOADER_STEP_REBUILD_REQUIRED;
            }
            plan->kind = LOADER_SOURCE_LIVE_CURRENT;
            plan->sourceId = progress.currentSourceId;
            plan->completedSources = progress.completedSources;
            plan->seenRowCount = progress.currentSeenRowCount;
            snprintf(plan->path, sizeof(plan->path), "%s", currentPath);
            snprintf(plan->fileIno, sizeof(plan->fileIno), "%s",
                     currentIno);
            plan->payloadStartOff = progress.currentPayloadStartOff;
            plan->currentOffset = progress.currentOffset;
            return LOADER_STEP_COMMITTED;
        }
        if (currentStatRc == 0)
            ls_syslog(LOG_INFO,
                      "live_rotate_mismatch source_id=%lld "
                      "old_ino=%s new_ino=%s "
                      "completed_sources=%lld current_offset=%lld",
                      progress.currentSourceId,
                      progress.currentFileIno, currentIno,
                      progress.completedSources, progress.currentOffset);
        discoverRc = scanVisibleGenerations(
            logdir, &generations, &generationCount);
        if (discoverRc == LOADER_FILE_DISCOVERY_STALE)
            return LOADER_STEP_STALE_REPLAN;
        if (discoverRc != 0)
            return LOADER_STEP_FATAL;
        maxArchiveIndex = findMaxArchiveIndex(
            generations, generationCount);
        {
            long remaining;
            enum loaderStepResult result;

            remaining = maxArchiveIndex - (long)progress.completedSources;
            result = planArchiveFromScan(
                db, &progress, generations, generationCount,
                maxArchiveIndex, remaining, 1, 1, openedAt, plan);
            if (result == LOADER_STEP_COMMITTED)
                ls_syslog(LOG_INFO,
                          "live_rotate_remap source_id=%lld archive_file=%s "
                          "remaining=%ld completed_sources=%lld "
                          "start_offset=%lld max_archive=%ld",
                          plan->sourceId, plan->path, remaining,
                          progress.completedSources, plan->currentOffset,
                          maxArchiveIndex);
            freeGenerations(generations);
            return result;
        }
    }

    discoverRc = scanVisibleGenerations(
        logdir, &generations, &generationCount);
    if (discoverRc == LOADER_FILE_DISCOVERY_STALE)
        return LOADER_STEP_STALE_REPLAN;
    if (discoverRc != 0)
        return LOADER_STEP_FATAL;
    maxArchiveIndex = findMaxArchiveIndex(generations,
                                          generationCount);

    if (strcmp(progress.currentState, "importing") == 0 &&
        strcmp(progress.currentKind, "archive") == 0) {
        long archiveIndex;
        enum loaderStepResult result;

        archiveIndex = maxArchiveIndex - (long)progress.completedSources;
        result = planArchiveFromScan(
            db, &progress, generations, generationCount, maxArchiveIndex,
            archiveIndex, 1, 0, openedAt, plan);
        freeGenerations(generations);
        return result;
    }

    if (strcmp(progress.currentState, "idle") != 0 &&
        strcmp(progress.currentKind, "none") != 0 &&
        progress.currentKind[0] != '\0') {
        ls_syslog(LOG_ERR, "unsupported_progress_state state=%s kind=%s",
                  progress.currentState, progress.currentKind);
        freeGenerations(generations);
        return LOADER_STEP_FATAL;
    }

    {
        long remaining;

        if (skipOldArchives(
            db, &progress, generations, generationCount,
            maxArchiveIndex, historyRetentionSeconds,
            openedAt) != 0) {
            freeGenerations(generations);
            return LOADER_STEP_FATAL;
        }
        remaining = maxArchiveIndex - (long)progress.completedSources;
        if (remaining < 0) {
            ls_syslog(LOG_ERR,
                      "completed_source_count_exceeds_retained_archives "
                      "completed_sources=%lld max_archive=%ld",
                      progress.completedSources, maxArchiveIndex);
            freeGenerations(generations);
            return LOADER_STEP_REBUILD_REQUIRED;
        }
    }

    current = findCurrentGeneration(generations, generationCount);
    if (current == NULL) {
        ls_syslog(LOG_ERR, "current_lsb_events_not_found logdir=%s", logdir);
        freeGenerations(generations);
        return LOADER_STEP_STALE_REPLAN;
    }
    initPlanFromGeneration(plan, LOADER_SOURCE_LIVE_CURRENT, current);
    plan->sourceId = progress.currentSourceId;
    plan->completedSources = progress.completedSources;
    if (loaderPlanBeginSource(db, plan, openedAt) != 0) {
        freeGenerations(generations);
        return LOADER_STEP_FATAL;
    }
    freeGenerations(generations);
    return LOADER_STEP_COMMITTED;
}
/*
 * Capture a consistent event-generation snapshot while mbatchd may rotate.
 *
 * The scan records directory inode, mtime, and ctime before and after,
 * plus each file's identity, payload start, and readable end. lsb.events.0/tmp,
 * disappearing files, NFS ESTALE, archive gaps, or a provable second rotation
 * return STALE for a later retry.
 *
 * This code does not lock mbatchd. It rejects snapshots that mix two directory
 * generations, so STALE is a normal concurrency result rather than fatal.
 */

/* Adds archive sort metadata while constructing a generation snapshot. */
struct loaderGenerationBuilder {
    struct loaderGeneration item;
    long archiveIndex;
};

/* Captures directory identity and change times around one unlocked scan. */
struct loaderDirStamp {
    unsigned long long ino;
    long long mtimeSec;
    long mtimeNsec;
    long long ctimeSec;
    long ctimeNsec;
};

static int isStaleSnapshotError(int errorNumber);
static void getStatTimes(const struct stat *st, long long *mtimeSec,
                         long *mtimeNsec, long long *ctimeSec,
                         long *ctimeNsec);
static int readDirStamp(const char *logdir,
                        struct loaderDirStamp *stamp);
static int dirStampsEqual(const struct loaderDirStamp *left,
                          const struct loaderDirStamp *right);
static int parseArchiveIndex(const char *name, long *indexOut);
static int joinPath(char *out, size_t outLen, const char *dir,
                    const char *name);
static int readArchivePayloadStart(FILE *fp,
                                   long long *payloadStartOff);
static int readCurrentPayloadStart(FILE *fp,
                                   long long *payloadStartOff);
static int statPayload(const char *path, const char *kind,
                       long archiveIndex,
                       struct loaderGeneration *item);
static int compareGenerations(const void *left, const void *right);
static int appendGeneration(struct loaderGenerationBuilder **items,
                            size_t *count, size_t *capacity,
                            const struct loaderGenerationBuilder *item);
static int validateArchiveContinuity(
    const struct loaderGenerationBuilder *items,
    size_t count);
static int checkNextArchiveAfterDirChange(
    const char *logdir,
    const struct loaderGenerationBuilder *items,
    size_t count,
    const struct loaderDirStamp *before,
    const struct loaderDirStamp *after);

/*
 * Test whether a discovery error represents a transient generation change.
 * @param[in] errorNumber: errno value from a filesystem operation.
 * @return: Nonzero for ENOENT or ESTALE, otherwise zero.
 */
static int
isStaleSnapshotError(int errorNumber)
{
    return errorNumber == ENOENT || errorNumber == ESTALE;
}

/*
 * Extract portable mtime and ctime second/nanosecond values from stat data.
 * @param[in] st: File status structure.
 * @param[out] mtimeSec: Modification time seconds.
 * @param[out] mtimeNsec: Modification time nanoseconds when available.
 * @param[out] ctimeSec: Change time seconds.
 * @param[out] ctimeNsec: Change time nanoseconds when available.
 * @return: None.
 */
static void
getStatTimes(const struct stat *st, long long *mtimeSec,
             long *mtimeNsec, long long *ctimeSec,
             long *ctimeNsec)
{
#if defined(__linux__)
    *mtimeSec = (long long)st->st_mtim.tv_sec;
    *mtimeNsec = st->st_mtim.tv_nsec;
    *ctimeSec = (long long)st->st_ctim.tv_sec;
    *ctimeNsec = st->st_ctim.tv_nsec;
#else
    *mtimeSec = (long long)st->st_mtime;
    *mtimeNsec = 0;
    *ctimeSec = (long long)st->st_ctime;
    *ctimeNsec = 0;
#endif
}

/*
 * Capture identity and change times for the event log directory.
 * @param[in] logdir: Event log directory path.
 * @param[out] stamp: Directory stamp receiving sampled values.
 * @return: 0 on success, STALE for a transient change, otherwise -1.
 */
static int
readDirStamp(const char *logdir, struct loaderDirStamp *stamp)
{
    struct stat st;

    if (stat(logdir, &st) != 0) {
        if (isStaleSnapshotError(errno))
            return LOADER_FILE_DISCOVERY_STALE;
        ls_syslog(LOG_ERR, "stat_logdir_failed path=%s: %m", logdir);
        return -1;
    }

    memset(stamp, 0, sizeof(*stamp));
    stamp->ino = (unsigned long long)st.st_ino;
    getStatTimes(&st, &stamp->mtimeSec, &stamp->mtimeNsec,
                 &stamp->ctimeSec, &stamp->ctimeNsec);
    return 0;
}

/*
 * Compare two directory stamps for an unchanged scan boundary.
 * @param[in] left: First directory stamp.
 * @param[in] right: Second directory stamp.
 * @return: Nonzero when every identity and timestamp field matches.
 */
static int
dirStampsEqual(const struct loaderDirStamp *left,
               const struct loaderDirStamp *right)
{
    return left->ino == right->ino &&
        left->mtimeSec == right->mtimeSec &&
        left->mtimeNsec == right->mtimeNsec &&
        left->ctimeSec == right->ctimeSec &&
        left->ctimeNsec == right->ctimeNsec;
}

/*
 * Parse a positive numeric suffix from an lsb.events.N file name.
 * @param[in] name: Directory entry name.
 * @param[out] indexOut: Optional parsed archive index.
 * @return: Nonzero for a valid archive name, otherwise zero.
 */
static int
parseArchiveIndex(const char *name, long *indexOut)
{
    const char *prefix = "lsb.events.";
    const char *p;
    long value;

    if (strncmp(name, prefix, strlen(prefix)) != 0)
        return 0;

    p = name + strlen(prefix);
    if (*p == '\0' || *p == '0')
        return 0;

    value = 0;
    while (*p != '\0') {
        if (!isdigit((unsigned char)*p))
            return 0;
        value = value * 10 + (*p - '0');
        p++;
    }

    if (indexOut != NULL)
        *indexOut = value;
    return 1;
}

/*
 * Join an event log directory and one entry name into a bounded path.
 * @param[out] out: Buffer receiving the joined path.
 * @param[in] outLen: Size of out.
 * @param[in] dir: Parent directory.
 * @param[in] name: Entry name.
 * @return: Nonzero when the path fits, otherwise zero.
 */
static int
joinPath(char *out, size_t outLen, const char *dir, const char *name)
{
    int written;

    written = snprintf(out, outLen, "%s/%s", dir, name);
    return written >= 0 && (size_t)written < outLen;
}

/*
 * Locate archive payload after its first header line.
 * @param[in] fp: Open archive stream positioned at its beginning.
 * @param[out] payloadStartOff: Physical byte offset after the first line.
 * @return: 0.
 */
static int
readArchivePayloadStart(FILE *fp, long long *payloadStartOff)
{
    int ch;
    long long count;

    count = 0;
    while ((ch = fgetc(fp)) != EOF) {
        count++;
        if (ch == '\n')
            break;
    }

    *payloadStartOff = count;
    return 0;
}

/*
 * Read an optional numeric payload offset from the live-file header.
 * @param[in] fp: Open live stream positioned at its beginning.
 * @param[out] payloadStartOff: Declared payload offset, or zero if absent.
 * @return: 0.
 */
static int
readCurrentPayloadStart(FILE *fp, long long *payloadStartOff)
{
    char line[128];
    char *p;
    long long value;

    if (fgets(line, sizeof(line), fp) == NULL) {
        *payloadStartOff = 0;
        return 0;
    }

    p = line;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (*p != '#') {
        *payloadStartOff = 0;
        return 0;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;

    value = 0;
    if (!isdigit((unsigned char)*p)) {
        *payloadStartOff = 0;
        return 0;
    }
    while (*p != '\0' && isdigit((unsigned char)*p)) {
        value = value * 10 + (*p - '0');
        p++;
    }

    *payloadStartOff = value;
    return 0;
}

/*
 * Capture stable identity, payload boundary, and readable size for one source.
 * @param[in] path: Event source path.
 * @param[in] kind: "current" or "archive".
 * @param[in] archiveIndex: Numeric suffix for an archive, otherwise zero.
 * @param[out] item: Generation descriptor receiving sampled values.
 * @return: 0 on success, STALE for a transient change, otherwise -1.
 */
static int
statPayload(const char *path, const char *kind,
            long archiveIndex,
            struct loaderGeneration *item)
{
    FILE *fp;
    struct stat st;
    long long payloadStartOff;
    long long size;
    long long ctimeSec;
    long ctimeNsec;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (isStaleSnapshotError(errno))
            return LOADER_FILE_DISCOVERY_STALE;
        ls_syslog(LOG_ERR, "open_event_file_failed file=%s: %m", path);
        return -1;
    }

    if (fstat(fileno(fp), &st) != 0) {
        int savedErrno;

        savedErrno = errno;
        fclose(fp);
        if (isStaleSnapshotError(savedErrno))
            return LOADER_FILE_DISCOVERY_STALE;
        errno = savedErrno;
        ls_syslog(LOG_ERR, "stat_event_file_failed file=%s: %m", path);
        return -1;
    }

    if (strcmp(kind, "current") == 0)
        readCurrentPayloadStart(fp, &payloadStartOff);
    else
        readArchivePayloadStart(fp, &payloadStartOff);

    fclose(fp);

    size = (long long)st.st_size;
    memset(item, 0, sizeof(*item));
    snprintf(item->kind, sizeof(item->kind), "%s", kind);
    snprintf(item->path, sizeof(item->path), "%s", path);
    snprintf(item->fileIno, sizeof(item->fileIno), "%llu",
             (unsigned long long)st.st_ino);
    getStatTimes(&st, &item->mtimeSec, &item->mtimeNsec,
                 &ctimeSec, &ctimeNsec);
    item->archiveIndex = archiveIndex;
    item->payloadStartOff = payloadStartOff < 0 ? 0 : payloadStartOff;
    item->endOffset = size - item->payloadStartOff;
    if (item->endOffset < 0)
        item->endOffset = 0;
    return 0;
}

/*
 * Order archive builders from oldest to newest and current last.
 * @param[in] left: First loaderGenerationBuilder.
 * @param[in] right: Second loaderGenerationBuilder.
 * @return: qsort-style comparison result.
 */
static int
compareGenerations(const void *left, const void *right)
{
    const struct loaderGenerationBuilder *a = left;
    const struct loaderGenerationBuilder *b = right;
    int aCurrent;
    int bCurrent;

    aCurrent = strcmp(a->item.kind, "current") == 0;
    bCurrent = strcmp(b->item.kind, "current") == 0;
    if (aCurrent != bCurrent)
        return aCurrent ? 1 : -1;
    if (!aCurrent && a->archiveIndex != b->archiveIndex)
        return a->archiveIndex > b->archiveIndex ? -1 : 1;
    return strcmp(a->item.path, b->item.path);
}

/*
 * Append one generation builder to a growable scan array.
 * @param[in,out] items: Owned array pointer.
 * @param[in,out] count: Number of populated entries.
 * @param[in,out] capacity: Number of allocated entries.
 * @param[in] item: Builder to copy.
 * @return: 0 on success, otherwise -1.
 */
static int
appendGeneration(struct loaderGenerationBuilder **items,
                 size_t *count, size_t *capacity,
                 const struct loaderGenerationBuilder *item)
{
    struct loaderGenerationBuilder *newItems;
    size_t newCapacity;

    if (*count >= *capacity) {
        newCapacity = *capacity == 0 ? 8 : *capacity * 2;
        newItems = realloc(*items, newCapacity * sizeof(**items));
        if (newItems == NULL) {
            ls_syslog(LOG_ERR, "append_generation_alloc_failed: %m");
            return -1;
        }
        *items = newItems;
        *capacity = newCapacity;
    }

    (*items)[*count] = *item;
    *count += 1;
    return 0;
}

/*
 * Verify that visible archive suffixes form one descending chain ending at 1.
 * @param[in] items: Sorted generation builders.
 * @param[in] count: Number of entries.
 * @return: 0 for a continuous chain, otherwise STALE.
 */
static int
validateArchiveContinuity(
    const struct loaderGenerationBuilder *items,
    size_t count)
{
    long expected;
    int haveArchive;
    size_t i;

    expected = -1;
    haveArchive = 0;
    for (i = 0; i < count; i++) {
        if (strcmp(items[i].item.kind, "archive") != 0)
            continue;
        if (!haveArchive) {
            expected = items[i].archiveIndex;
            haveArchive = 1;
        } else {
            expected--;
        }
        if (items[i].archiveIndex != expected) {
            ls_syslog(LOG_INFO,
                      "generation_snapshot_stale "
                      "reason=archive_chain_gap expected=lsb.events.%ld",
                      expected);
            return LOADER_FILE_DISCOVERY_STALE;
        }
    }

    if (haveArchive && expected != 1) {
        ls_syslog(LOG_INFO,
                  "generation_snapshot_stale "
                  "reason=archive_chain_does_not_end_at_1");
        return LOADER_FILE_DISCOVERY_STALE;
    }
    return 0;
}

/*
 * Find the highest archive suffix in a builder array.
 * @param[in] items: Generation builders.
 * @param[in] count: Number of entries.
 * @return: Highest archive index, or zero when none exists.
 */
static long
findBuilderMaxArchiveIndex(const struct loaderGenerationBuilder *items,
                           size_t count)
{
    long maxIndex;
    size_t i;

    maxIndex = 0;
    for (i = 0; i < count; i++) {
        if (strcmp(items[i].item.kind, "archive") == 0 &&
            items[i].archiveIndex > maxIndex)
            maxIndex = items[i].archiveIndex;
    }
    return maxIndex;
}

/*
 * Detect a second rotation that created the next archive during a scan.
 * @param[in] logdir: Event log directory.
 * @param[in] items: Generations captured by the scan.
 * @param[in] count: Number of captured generations.
 * @param[in] before: Directory stamp before scanning entries.
 * @param[in] after: Directory stamp after scanning entries.
 * @return: 0 when no next archive exists, STALE on rotation, otherwise -1.
 */
static int
checkNextArchiveAfterDirChange(
    const char *logdir,
    const struct loaderGenerationBuilder *items,
    size_t count,
    const struct loaderDirStamp *before,
    const struct loaderDirStamp *after)
{
    char nextName[64];
    char nextPath[LOADER_PATH_MAX];
    struct stat st;
    long maxArchiveIndex;
    int savedErrno;

    maxArchiveIndex = findBuilderMaxArchiveIndex(items, count);
    snprintf(nextName, sizeof(nextName), "lsb.events.%ld",
             maxArchiveIndex + 1);
    if (!joinPath(nextPath, sizeof(nextPath), logdir, nextName)) {
        ls_syslog(LOG_ERR, "event_path_too_long logdir=%s file=%s",
                  logdir, nextName);
        return -1;
    }

    if (stat(nextPath, &st) == 0) {
        if (!S_ISREG(st.st_mode))
            return 0;
        ls_syslog(LOG_INFO,
                  "generation_snapshot_stale "
                  "reason=archive_upper_bound_changed path=%s "
                  "max_archive=%ld next_archive=%s "
                  "before_mtime=%lld.%09ld before_ctime=%lld.%09ld "
                  "after_mtime=%lld.%09ld after_ctime=%lld.%09ld",
                  logdir, maxArchiveIndex, nextName,
                  before->mtimeSec, before->mtimeNsec,
                  before->ctimeSec, before->ctimeNsec,
                  after->mtimeSec, after->mtimeNsec,
                  after->ctimeSec, after->ctimeNsec);
        return LOADER_FILE_DISCOVERY_STALE;
    }

    savedErrno = errno;
    if (savedErrno == ENOENT)
        return 0;
    if (savedErrno == ESTALE)
        return LOADER_FILE_DISCOVERY_STALE;

    errno = savedErrno;
    ls_syslog(LOG_ERR, "stat_next_archive_failed file=%s: %m", nextPath);
    return -1;
}

/*
 * Scan visible event-file generations.
 * @param[in] logdir: Event log directory.
 * @param[out] outItems: Allocated generation array.
 * @param[out] outCount: Number of array entries.
 * @return: 0 on success, LOADER_FILE_DISCOVERY_STALE for an unstable snapshot,
 *          otherwise -1.
 */
static int
scanVisibleGenerations(const char *logdir,
                       struct loaderGeneration **outItems,
                       size_t *outCount)
{
    DIR *dir;
    struct dirent *entry;
    struct loaderGenerationBuilder *builders;
    struct loaderGeneration *items;
    size_t count;
    size_t capacity;
    size_t i;
    struct loaderDirStamp beforeStamp;
    struct loaderDirStamp afterStamp;
    int stampRc;

    if (outItems == NULL || outCount == NULL || logdir == NULL) {
        ls_syslog(LOG_ERR, "invalid_scan_argument");
        return -1;
    }

    *outItems = NULL;
    *outCount = 0;
    builders = NULL;
    count = 0;
    capacity = 0;

    stampRc = readDirStamp(logdir, &beforeStamp);
    if (stampRc != 0)
        return stampRc;

    dir = opendir(logdir);
    if (dir == NULL) {
        if (isStaleSnapshotError(errno))
            return LOADER_FILE_DISCOVERY_STALE;
        ls_syslog(LOG_ERR, "open_logdir_failed path=%s: %m", logdir);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct loaderGenerationBuilder builder;
        struct stat st;
        char path[LOADER_PATH_MAX];
        long archiveIndex;
        const char *kind;

        if (strcmp(entry->d_name, "lsb.events.0") == 0 ||
            strcmp(entry->d_name, "lsb.events.tmp") == 0) {
            closedir(dir);
            free(builders);
            ls_syslog(LOG_INFO,
                      "generation_snapshot_stale reason=rotate_window file=%s",
                      entry->d_name);
            return LOADER_FILE_DISCOVERY_STALE;
        }

        if (strcmp(entry->d_name, "lsb.events") == 0) {
            kind = "current";
            archiveIndex = -1;
        } else if (parseArchiveIndex(entry->d_name, &archiveIndex)) {
            kind = "archive";
        } else {
            continue;
        }

        if (!joinPath(path, sizeof(path), logdir, entry->d_name)) {
            closedir(dir);
            free(builders);
            ls_syslog(LOG_ERR, "event_path_too_long logdir=%s file=%s",
                      logdir, entry->d_name);
            return -1;
        }

        if (stat(path, &st) != 0) {
            if (isStaleSnapshotError(errno)) {
                closedir(dir);
                free(builders);
                return LOADER_FILE_DISCOVERY_STALE;
            }
            closedir(dir);
            free(builders);
            ls_syslog(LOG_ERR, "stat_event_file_failed file=%s: %m", path);
            return -1;
        }
        if (!S_ISREG(st.st_mode))
            continue;

        memset(&builder, 0, sizeof(builder));
        builder.archiveIndex = archiveIndex;
        {
            int statRc;

            statRc = statPayload(path, kind, archiveIndex,
                                 &builder.item);
            if (statRc != 0) {
                closedir(dir);
                free(builders);
                return statRc;
            }
        }

        if (appendGeneration(&builders, &count, &capacity, &builder) != 0) {
            closedir(dir);
            free(builders);
            return -1;
        }
    }

    closedir(dir);
    stampRc = readDirStamp(logdir, &afterStamp);
    if (stampRc != 0) {
        free(builders);
        return stampRc;
    }
    if (!dirStampsEqual(&beforeStamp, &afterStamp)) {
        stampRc = checkNextArchiveAfterDirChange(
            logdir, builders, count, &beforeStamp, &afterStamp);
        if (stampRc != 0) {
            free(builders);
            return stampRc;
        }
    }

    qsort(builders, count, sizeof(*builders), compareGenerations);
    {
        int validateRc;

        validateRc = validateArchiveContinuity(
            builders, count);
        if (validateRc != 0) {
            free(builders);
            return validateRc;
        }
    }

    items = calloc(count == 0 ? 1 : count, sizeof(*items));
    if (items == NULL) {
        free(builders);
        ls_syslog(LOG_ERR, "generation_items_alloc_failed: %m");
        return -1;
    }
    for (i = 0; i < count; i++)
        items[i] = builders[i].item;

    free(builders);
    *outItems = items;
    *outCount = count;
    return 0;
}

/*
 * Release a generation array.
 * @param[in] items: Array to release.
 * @return: None.
 */
static void
freeGenerations(struct loaderGeneration *items)
{
    free(items);
}

/*
 * Find the highest visible archive index.
 * @param[in] items: Generation array.
 * @param[in] count: Number of array entries.
 * @return: Highest archive index, or 0 when none is visible.
 */
static long
findMaxArchiveIndex(const struct loaderGeneration *items,
                    size_t count)
{
    long maxIndex;
    size_t i;

    maxIndex = 0;
    if (items == NULL)
        return 0;
    for (i = 0; i < count; i++) {
        if (strcmp(items[i].kind, "archive") == 0 &&
            items[i].archiveIndex > maxIndex)
            maxIndex = items[i].archiveIndex;
    }
    return maxIndex;
}

/*
 * Find an archive generation by index.
 * @param[in] items: Generation array.
 * @param[in] count: Number of array entries.
 * @param[in] archiveIndex: Archive index to find.
 * @return: Pointer to the matching array entry, otherwise NULL.
 */
static const struct loaderGeneration *
findArchive(const struct loaderGeneration *items,
            size_t count,
            long archiveIndex)
{
    size_t i;

    if (items == NULL)
        return NULL;
    for (i = 0; i < count; i++) {
        if (strcmp(items[i].kind, "archive") == 0 &&
            items[i].archiveIndex == archiveIndex)
            return &items[i];
    }
    return NULL;
}
/*
 * Find the current generation.
 * @param[in] items: Generation array.
 * @param[in] count: Number of array entries.
 * @return: Pointer to the current entry, otherwise NULL.
 */
static const struct loaderGeneration *
findCurrentGeneration(const struct loaderGeneration *items,
                      size_t count)
{
    size_t i;

    if (items == NULL)
        return NULL;
    for (i = 0; i < count; i++) {
        if (strcmp(items[i].kind, "current") == 0)
            return &items[i];
    }
    return NULL;
}

/*
 * Parser for individual lsb.events lines.
 *
 * nextToken() implements only the event format's whitespace separation,
 * single and double quotes, and backslash escapes; it is not a shell parser.
 * findEventJobIndex() records the fixed job-ID position for known event types,
 * while JOB_NEW, JOB_START, and JOB_STATUS expose selected metadata fields.
 * Recognized lines are stored verbatim by the import layer. Invalid formats or
 * unknown job-ID positions are skipped rather than guessed or repaired.
 *
 * When an event format changes, update the mapping and tests from its definition
 * before accepting it. Guessing a job-ID position can attach history to the
 * wrong job.
 */

#define LOADER_EVENT_TOKEN_MAX 256
#define LOADER_JOB_STAT_EXIT 0x20
#define LOADER_JOB_STAT_DONE 0x40
#define LOADER_JOB_NEW_RLIMIT_COUNT 11
static int nextToken(const char **cursor, char *token, size_t tokenLen);
static int appendChar(char *token, size_t tokenLen, size_t *used,
                      char ch);
static int findEventJobIndex(const char *eventType);
static int parseBaseJobId(const char *value, long long *jobId);
static int parseLongLong(const char *value, long long *out);
static void initEventFields(struct loaderEventFields *times);
static int extractJobNewSubmitTime(const char *cursor,
                                   long long *submitTime);
static int extractJobNewUser(const char *cursor, char *user,
                             size_t userLen);
static int extractJobNewQueue(const char *cursor, char *queue,
                              size_t queueLen);
static int extractJobStatusEndTime(const char *cursor,
                                   long long *endTime);

/*
 * Append one decoded character to a bounded event token.
 * @param[in,out] token: Token buffer.
 * @param[in] tokenLen: Size of token.
 * @param[in,out] used: Number of token bytes already used.
 * @param[in] ch: Character to append.
 * @return: LOADER_EVENT_OK or LOADER_EVENT_TOKEN_TOO_LONG.
 */
static int
appendChar(char *token, size_t tokenLen, size_t *used, char ch)
{
    if (*used + 1 >= tokenLen)
        return LOADER_EVENT_TOKEN_TOO_LONG;
    token[*used] = ch;
    *used += 1;
    token[*used] = '\0';
    return LOADER_EVENT_OK;
}
/*
 * Decode the next quoted or unquoted token from an event record.
 * @param[in,out] cursor: Input position advanced past the decoded token.
 * @param[out] token: Buffer receiving the decoded token.
 * @param[in] tokenLen: Size of token.
 * @return: A loaderEventStatus value.
 */
static int
nextToken(const char **cursor, char *token, size_t tokenLen)
{
    const char *p;
    size_t used;
    char quote;

    p = *cursor;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (*p == '\0')
        return LOADER_EVENT_NO_EVENT;

    used = 0;
    quote = '\0';
    token[0] = '\0';

    while (*p != '\0') {
        char ch;

        ch = *p;
        if (quote == '\0' && isspace((unsigned char)ch))
            break;

        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
                p++;
                continue;
            }
            if (ch == '\\' && p[1] != '\0') {
                p++;
                ch = *p;
            }
            if (appendChar(token, tokenLen, &used, ch)
                != LOADER_EVENT_OK)
                return LOADER_EVENT_TOKEN_TOO_LONG;
            p++;
            continue;
        }

        if (ch == '\'' || ch == '"') {
            quote = ch;
            p++;
            continue;
        }
        if (ch == '\\' && p[1] != '\0') {
            p++;
            ch = *p;
        }
        if (appendChar(token, tokenLen, &used, ch)
            != LOADER_EVENT_OK)
            return LOADER_EVENT_TOKEN_TOO_LONG;
        p++;
    }

    if (quote != '\0')
        return LOADER_EVENT_PARSE_ERROR;

    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    *cursor = p;
    return LOADER_EVENT_OK;
}

/*
 * Return the one-based token position holding a job ID for an event type.
 * @param[in] eventType: Scheduler event name.
 * @return: Known token position, otherwise -1.
 */
static int
findEventJobIndex(const char *eventType)
{
    static const char *firstJobEvents[] = {
        "JOB_NEW",
        "JOB_MODIFY",
        "PRE_EXEC_START",
        "JOB_START",
        "JOB_START_ACCEPT",
        "JOB_STATUS",
        "SBD_JOB_STATUS",
        "JOB_FINISH",
        "CHKPNT",
        "MIG",
        "JOB_ATTR_SET",
        "JOB_SIGNAL",
        "JOB_EXECUTE",
        "JOB_MSG",
        "JOB_MSG_ACK",
        "JOB_SIGACT",
        "JOB_REQUEUE",
        "JOB_CLEAN",
        "JOB_FORCE",
        "JOB_MODIFY2",
    };
    static const char *secondJobEvents[] = {
        "JOB_SWITCH",
        "JOB_MOVE",
    };
    size_t i;

    for (i = 0; i < sizeof(firstJobEvents) / sizeof(firstJobEvents[0]);
         i++) {
        if (strcmp(eventType, firstJobEvents[i]) == 0)
            return 3;
    }

    for (i = 0; i < sizeof(secondJobEvents) / sizeof(secondJobEvents[0]);
         i++) {
        if (strcmp(eventType, secondJobEvents[i]) == 0)
            return 4;
    }

    return -1;
}

/*
 * Parse the decimal base portion of a job or array-element selector.
 * @param[in] value: Token beginning with a decimal job ID.
 * @param[out] jobId: Parsed base job ID.
 * @return: A loaderEventStatus value.
 */
static int
parseBaseJobId(const char *value, long long *jobId)
{
    long long parsed;

    if (value == NULL || !isdigit((unsigned char)value[0]))
        return LOADER_EVENT_NO_JOB;

    parsed = 0;
    while (*value != '\0' && isdigit((unsigned char)*value)) {
        int digit;

        digit = *value - '0';
        if (parsed > (LLONG_MAX - digit) / 10)
            return LOADER_EVENT_PARSE_ERROR;
        parsed = parsed * 10 + digit;
        value++;
    }

    *jobId = parsed;
    return LOADER_EVENT_OK;
}

/*
 * Parse a complete signed decimal integer without accepting trailing text.
 * @param[in] value: Decimal token.
 * @param[out] out: Parsed integer.
 * @return: LOADER_EVENT_OK or LOADER_EVENT_PARSE_ERROR.
 */
static int
parseLongLong(const char *value, long long *out)
{
    long long parsed;
    int sign;

    if (value == NULL || out == NULL)
        return LOADER_EVENT_PARSE_ERROR;

    sign = 1;
    if (*value == '-') {
        sign = -1;
        value++;
    }
    if (!isdigit((unsigned char)*value))
        return LOADER_EVENT_PARSE_ERROR;

    parsed = 0;
    while (*value != '\0' && isdigit((unsigned char)*value)) {
        int digit;

        digit = *value - '0';
        if (parsed > (LLONG_MAX - digit) / 10)
            return LOADER_EVENT_PARSE_ERROR;
        parsed = parsed * 10 + digit;
        value++;
    }
    if (*value != '\0')
        return LOADER_EVENT_PARSE_ERROR;

    *out = sign > 0 ? parsed : -parsed;
    return LOADER_EVENT_OK;
}

/*
 * Clear optional metadata before parsing an event line.
 * @param[out] times: Optional metadata structure to initialize.
 * @return: None.
 */
static void
initEventFields(struct loaderEventFields *times)
{
    if (times == NULL)
        return;
    memset(times, 0, sizeof(*times));
}

/*
 * Extract submit time from the fixed JOB_NEW payload layout.
 * @param[in] cursor: JOB_NEW payload after eventTime.
 * @param[out] submitTime: Parsed submission timestamp.
 * @return: A loaderEventStatus value.
 */
static int
extractJobNewSubmitTime(const char *cursor, long long *submitTime)
{
    char token[LOADER_EVENT_TOKEN_MAX];
    int status;
    int i;

    /*
     * Cursor points at JOB_NEW payload after eventTime. The first fields are:
     * jobId userId options numProcessors submitTime ...
     */
    for (i = 0; i < 5; i++) {
        status = nextToken(&cursor, token, sizeof(token));
        if (status != LOADER_EVENT_OK)
            return status;
    }
    return parseLongLong(token, submitTime);
}

/*
 * Extract submitting user from the fixed JOB_NEW payload layout.
 * @param[in] cursor: JOB_NEW payload after eventTime.
 * @param[out] user: Buffer receiving the user name.
 * @param[in] userLen: Size of user.
 * @return: A loaderEventStatus value.
 */
static int
extractJobNewUser(const char *cursor, char *user, size_t userLen)
{
    char token[LOADER_EVENT_TOKEN_MAX];
    int status;
    int i;

    if (user == NULL || userLen == 0)
        return LOADER_EVENT_PARSE_ERROR;

    /*
     * Cursor points at JOB_NEW payload after eventTime. userName is the first
     * quoted field after ten numeric fields:
     * jobId userId options numProcessors submitTime beginTime termTime
     * sigValue chkpntPeriod restartPid userName ...
     */
    for (i = 0; i < 11; i++) {
        status = nextToken(&cursor, token, sizeof(token));
        if (status != LOADER_EVENT_OK)
            return status;
    }
    snprintf(user, userLen, "%s", token);
    return LOADER_EVENT_OK;
}

/*
 * Extract queue name from the fixed JOB_NEW payload layout.
 * @param[in] cursor: JOB_NEW payload after eventTime.
 * @param[out] queue: Buffer receiving the queue name.
 * @param[in] queueLen: Size of queue.
 * @return: A loaderEventStatus value.
 */
static int
extractJobNewQueue(const char *cursor, char *queue, size_t queueLen)
{
    char token[LOADER_EVENT_TOKEN_MAX];
    int status;
    int i;

    if (queue == NULL || queueLen == 0)
        return LOADER_EVENT_PARSE_ERROR;

    /*
     * readJobNew() reads:
     * 10 numeric fields, userName, 11 rLimits, hostSpec, hostFactor, umask,
     * then queue.
     */
    for (i = 0; i < 10 + 1 + LOADER_JOB_NEW_RLIMIT_COUNT + 1 + 2 + 1; i++) {
        status = nextToken(&cursor, token, sizeof(token));
        if (status != LOADER_EVENT_OK)
            return status;
    }
    snprintf(queue, queueLen, "%s", token);
    return LOADER_EVENT_OK;
}

/*
 * Extract end time from a terminal JOB_STATUS payload.
 * @param[in] cursor: JOB_STATUS payload after eventTime.
 * @param[out] endTime: Parsed completion timestamp.
 * @return: A loaderEventStatus value; NO_EVENT means nonterminal status.
 */
static int
extractJobStatusEndTime(const char *cursor, long long *endTime)
{
    char token[LOADER_EVENT_TOKEN_MAX];
    long long statusValue;
    int status;
    int i;

    /*
     * Cursor points at JOB_STATUS payload after eventTime. The first fields are:
     * jobId jStatus reason subreasons cpuTime endTime ...
     */
    status = nextToken(&cursor, token, sizeof(token));
    if (status != LOADER_EVENT_OK)
        return status;
    status = nextToken(&cursor, token, sizeof(token));
    if (status != LOADER_EVENT_OK)
        return status;
    status = parseLongLong(token, &statusValue);
    if (status != LOADER_EVENT_OK)
        return status;
    for (i = 0; i < 4; i++) {
        status = nextToken(&cursor, token, sizeof(token));
        if (status != LOADER_EVENT_OK)
            return status;
    }
    if ((statusValue & (LOADER_JOB_STAT_DONE | LOADER_JOB_STAT_EXIT)) == 0)
        return LOADER_EVENT_NO_EVENT;
    return parseLongLong(token, endTime);
}

/*
 * Extract a base job ID and time metadata from one lsb.events line.
 * @param[in] line: Original event text.
 * @param[out] jobId: Parsed base job ID.
 * @param[out] times: Parsed time and filter metadata, or NULL.
 * @return: A loaderEventStatus value.
 */
int
loaderExtractEventFromLine(const char *line, long long *jobId,
                               struct loaderEventFields *times)
{
    const char *p;
    const char *payload;
    char token[LOADER_EVENT_TOKEN_MAX];
    char eventType[LOADER_EVENT_TOKEN_MAX];
    char jobToken[LOADER_EVENT_TOKEN_MAX];
    long long eventTime;
    int status;
    int jobTokenIndex;
    int tokenIndex;

    if (line == NULL || jobId == NULL)
        return LOADER_EVENT_PARSE_ERROR;
    initEventFields(times);

    p = line;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (*p == '\0' || *p == '#')
        return LOADER_EVENT_NO_EVENT;

    status = nextToken(&p, token, sizeof(token));
    if (status != LOADER_EVENT_OK)
        return status;
    memcpy(eventType, token, sizeof(eventType));

    jobTokenIndex = findEventJobIndex(eventType);
    if (jobTokenIndex < 0)
        return LOADER_EVENT_NO_JOB;

    status = nextToken(&p, token, sizeof(token));
    if (status != LOADER_EVENT_OK)
        return LOADER_EVENT_NO_EVENT;
    status = nextToken(&p, token, sizeof(token));
    if (status != LOADER_EVENT_OK)
        return LOADER_EVENT_NO_EVENT;
    status = parseLongLong(token, &eventTime);
    if (status != LOADER_EVENT_OK)
        return status;
    payload = p;

    jobToken[0] = '\0';
    for (tokenIndex = 3; tokenIndex <= jobTokenIndex; tokenIndex++) {
        status = nextToken(&p, token, sizeof(token));
        if (status != LOADER_EVENT_OK)
            return LOADER_EVENT_NO_EVENT;
        if (tokenIndex == jobTokenIndex)
            memcpy(jobToken, token, sizeof(jobToken));
    }

    status = parseBaseJobId(jobToken, jobId);
    if (status != LOADER_EVENT_OK)
        return status;

    if (times != NULL) {
        times->hasEventTime = 1;
        times->eventTime = eventTime;

        /*
         * Immutable submission metadata is stored only on JOB_NEW. Query-side
         * -u and -q filters locate those rows with EXISTS across a base job ID.
         * Add similar immutable attributes here; mutable attributes require a
         * different storage rule.
         */
        if (strcmp(eventType, "JOB_NEW") == 0) {
            long long submitTime;

            status = extractJobNewSubmitTime(payload, &submitTime);
            if (status == LOADER_EVENT_OK) {
                times->hasSubmitTime = 1;
                times->submitTime = submitTime;
            }
            status = extractJobNewUser(
                payload, times->submitUser, sizeof(times->submitUser));
            if (status == LOADER_EVENT_OK)
                times->hasSubmitUser = 1;
            status = extractJobNewQueue(
                payload, times->submitQueue, sizeof(times->submitQueue));
            if (status == LOADER_EVENT_OK)
                times->hasSubmitQueue = 1;
        } else if (strcmp(eventType, "JOB_START") == 0 ||
                   strcmp(eventType, "PRE_EXEC_START") == 0) {
            times->hasStartTime = 1;
            times->startTime = eventTime;
        } else if (strcmp(eventType, "JOB_STATUS") == 0) {
            long long endTime;

            status = extractJobStatusEndTime(payload, &endTime);
            if (status == LOADER_EVENT_OK) {
                times->hasEndTime = 1;
                times->endTime = endTime;
            }
        }
    }

    return LOADER_EVENT_OK;
}

/*
 * Read event files from source plans and write bounded transactions.
 *
 * Each chunk commits event rows, currentOffset, and the source seen count
 * together. Restart therefore returns to the preceding complete chunk and
 * cannot advance an offset without its events. Archives have a fixed endOffset
 * and can complete a source. Live current has no endpoint, retains its FILE
 * across polls, and checks the inode after repeated EOF.
 *
 * ENOENT, ESTALE, and identity changes are recoverable STALE_REPLAN outcomes,
 * not database corruption. Every return path closes writers, rolls back an
 * active transaction, and releases the getline buffer as applicable.
 */

#define LOADER_LIVE_EOF_ROTATE_CHECKS 3
#define LOADER_IMPORT_TX_MAX_LINES 100000LL
#define LOADER_IMPORT_TX_MAX_RAW_BYTES (64LL * 1024 * 1024)

/* Runtime row limit for one import transaction. */
static long long loaderImportTxMaxLines = LOADER_IMPORT_TX_MAX_LINES;
/* Runtime raw-byte limit for one import transaction. */
static long long loaderImportTxMaxRawBytes =
    LOADER_IMPORT_TX_MAX_RAW_BYTES;

/* Selects whether a writer updates progress and builds indexes immediately. */
enum loaderImportWriterMode {
    LOADER_IMPORT_WRITER_TRACKED = 0,
    LOADER_IMPORT_WRITER_BULK = 1,
    LOADER_IMPORT_WRITER_UNTRACKED = 2
};

/*
 * Configure size limits for one import transaction.
 * @param[in] maxLines: Maximum event rows per transaction.
 * @param[in] maxRawBytes: Maximum raw bytes per transaction.
 * @return: None.
 */
void
loaderImportConfigureChunkLimits(long long maxLines,
                                     long long maxRawBytes)
{
    /* Nonpositive input preserves the current limit rather than disabling it. */
    if (maxLines > 0)
        loaderImportTxMaxLines = maxLines;
    if (maxRawBytes > 0)
        loaderImportTxMaxRawBytes = maxRawBytes;
}

/*
 * Test whether an event-file operation failed because an NFS handle is stale.
 * @param[in] errorNumber: errno value returned by the failed operation.
 * @return: Nonzero for ESTALE, otherwise zero.
 */
static int
isStaleHandleError(int errorNumber)
{
    return errorNumber == ESTALE;
}

/*
 * Return the final component of an event source path.
 * @param[in] path: Optional source path.
 * @return: Pointer inside path, or an empty string for a missing path.
 */
static const char *
pathBasename(const char *path)
{
    const char *slash;

    if (path == NULL || path[0] == '\0')
        return "";
    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/*
 * Remove trailing newline characters from one getline buffer.
 * @param[in,out] line: Mutable event line.
 * @param[in,out] lineLen: Line length updated after stripping.
 * @return: None.
 */
static void
stripTrailingNewlines(char *line, size_t *lineLen)
{
    while (*lineLen > 0 &&
           (line[*lineLen - 1] == '\n' || line[*lineLen - 1] == '\r')) {
        *lineLen -= 1;
        line[*lineLen] = '\0';
    }
}

/*
 * Verify that an open event file still matches its planned inode.
 * @param[in] fp: Open event stream.
 * @param[in] wantIno: Planned inode number text.
 * @return: 0 for a match, 1 for an identity change, or -1 on stat failure.
 */
static int
fileIdentityMatches(FILE *fp,
                    const char *wantIno)
{
    struct stat st;
    char actualIno[32];

    if (fstat(fileno(fp), &st) != 0) {
        ls_syslog(LOG_ERR, "stat_event_file_failed: %m");
        return -1;
    }
    snprintf(actualIno, sizeof(actualIno), "%llu",
             (unsigned long long)st.st_ino);
    if (strcmp(actualIno, wantIno) != 0) {
        ls_syslog(LOG_ERR,
                  "planned_event_file_identity_changed want_ino=%s "
                  "actual_ino=%s",
                  wantIno, actualIno);
        return 1;
    }
    return 0;
}

/*
 * Read the inode and size currently published at a source path.
 * @param[in] path: Event source path.
 * @param[out] fileIno: Buffer receiving the inode number text.
 * @param[in] fileInoLen: Size of fileIno.
 * @param[out] fileSize: Optional current file size.
 * @return: 0 on success, otherwise -1.
 */
static int
statPathIdentity(const char *path,
                 char *fileIno,
                 size_t fileInoLen,
                 long long *fileSize)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    snprintf(fileIno, fileInoLen, "%llu", (unsigned long long)st.st_ino);
    if (fileSize != NULL)
        *fileSize = (long long)st.st_size;
    return 0;
}

/*
 * Close and reset a live file handle.
 * @param[in,out] live: Live handle to close.
 * @return: None.
 */
void
loaderLiveHandleClose(struct loaderLiveHandle *live)
{
    if (live == NULL)
        return;
    if (live->fp != NULL)
        fclose(live->fp);
    memset(live, 0, sizeof(*live));
}

/*
 * Persist a chunk's next offset and source row count in the same transaction.
 * @param[in] db: SQLite connection with an active transaction.
 * @param[in] sourceId: Loader source row being advanced.
 * @param[in] nextOffset: Logical source offset after the committed chunk.
 * @param[in] seenRowCount: Total rows observed from the source.
 * @return: 0 on success, otherwise -1.
 */
static int
commitProgress(sqlite3 *db,
               long long sourceId,
               long long nextOffset,
               long long seenRowCount)
{
    if (loaderStoreUpdateCurrentOffset(db, nextOffset) != 0 ||
        loaderStoreUpdateSourceSeenCount(db, sourceId, seenRowCount) != 0)
        return -1;
    return 0;
}

/*
 * Start a chunk transaction and apply a pending live-to-archive remap once.
 * @param[in] db: SQLite connection.
 * @param[in] plan: Source plan for this chunk.
 * @param[in,out] inTransaction: Whether a transaction is already active.
 * @param[in,out] remapped: Whether the source remap has been applied.
 * @return: 0 on success, otherwise -1.
 */
static int
beginChunk(sqlite3 *db,
           const struct loaderSourcePlan *plan,
           int *inTransaction,
           int *remapped)
{
    if (*inTransaction)
        return 0;
    if (loaderStoreBegin(db) != 0)
        return -1;
    *inTransaction = 1;
    if (plan->markRemapped && !*remapped) {
        if (loaderStoreRemapCurrentSourceToArchive(
                db, plan->fileIno, plan->path) != 0)
            return -1;
        *remapped = 1;
    }
    return 0;
}

/*
 * Close the day writer, persist progress, and commit the current chunk.
 * @param[in] db: SQLite connection with an active transaction.
 * @param[in,out] writer: Event writer to close before commit.
 * @param[in,out] writerOpen: Whether writer currently owns statements.
 * @param[in,out] inTransaction: Transaction state cleared on commit attempt.
 * @param[in] sourceId: Loader source row being advanced.
 * @param[in] nextOffset: Logical source offset after the chunk.
 * @param[in] seenRowCount: Total rows observed from the source.
 * @return: 0 on success, otherwise nonzero.
 */
static int
commitChunk(sqlite3 *db,
            struct loaderStoreEventWriter *writer,
            int *writerOpen,
            int *inTransaction,
            long long sourceId,
            long long nextOffset,
            long long seenRowCount)
{
    if (*writerOpen) {
        loaderStoreEventWriterClose(writer);
        *writerOpen = 0;
    }
    if (commitProgress(
        db, sourceId, nextOffset, seenRowCount) != 0)
        return -1;
    if (loaderStoreCommit(db) != 0) {
        *inTransaction = 0;
        return 1;
    }
    *inTransaction = 0;
    return 0;
}

/*
 * Emit one structured log record for a committed import chunk.
 * @param[in] plan: Source plan identifying the imported generation.
 * @param[in] stats: Cumulative source statistics.
 * @param[in] reason: Commit boundary reason.
 * @param[in] dayKey: Event day written by the chunk.
 * @param[in] chunkLines: Source lines consumed in the chunk.
 * @param[in] chunkRawBytes: Raw bytes consumed in the chunk.
 * @param[in] chunkEndOffset: Logical source offset after the chunk.
 * @param[in] inserted: Cumulative inserted row count.
 * @param[in] skipped: Cumulative skipped row count.
 * @return: None.
 */
static void
logChunkCommit(const struct loaderSourcePlan *plan,
               const struct loaderImportStats *stats,
               const char *reason,
               const char *dayKey,
               long long chunkLines,
               long long chunkRawBytes,
               long long chunkEndOffset,
               long long inserted,
               long long skipped)
{
    ls_syslog(LOG_INFO,
              "import_chunk_commit source_id=%lld file=%s reason=%s "
              "day=%s chunk_lines=%lld chunk_raw_bytes=%lld "
              "end_offset=%lld total_lines=%lld total_inserted=%lld "
              "total_skipped=%lld",
              plan->sourceId, stats->file,
              reason != NULL ? reason : "none",
              dayKey != NULL && dayKey[0] != '\0' ? dayKey : "none",
              chunkLines, chunkRawBytes, chunkEndOffset,
              stats->inputLines, inserted, skipped);
}

/*
 * Switch the import writer to an event day.
 * @param[in] db: SQLite connection.
 * @param[in,out] writer: Current event writer.
 * @param[in,out] writerOpen: Whether writer is open.
 * @param[in] mode: Progress and index update mode.
 * @param[in] dayKey: Target event day.
 * @return: 0 on success, otherwise -1.
 */
static int
openDayWriter(sqlite3 *db,
              struct loaderStoreEventWriter *writer,
              int *writerOpen,
              enum loaderImportWriterMode mode,
              const char *dayKey)
{
    if (*writerOpen) {
        if (strcmp(writer->dayKey, dayKey) == 0)
            return 0;
        loaderStoreEventWriterClose(writer);
        *writerOpen = 0;
    }
    if (mode == LOADER_IMPORT_WRITER_BULK) {
        if (loaderStoreEventWriterOpenBulk(db, writer, dayKey) != 0)
            return -1;
    } else if (mode == LOADER_IMPORT_WRITER_UNTRACKED) {
        if (loaderStoreEventWriterOpenUntracked(
                db, writer, dayKey) != 0)
            return -1;
    } else if (loaderStoreEventWriterOpen(db, writer, dayKey) != 0) {
        return -1;
    }
    *writerOpen = 1;
    return 0;
}

/*
 * Parse one event line and derive the daily table key used for insertion.
 * @param[in,out] line: Mutable event text.
 * @param[out] jobId: Parsed base job ID.
 * @param[out] times: Parsed event and filter fields.
 * @param[out] dayKey: Buffer receiving the event day.
 * @param[in] dayKeyLen: Size of dayKey.
 * @param[in,out] skipped: Counter incremented for non-importable records.
 * @return: 1 for an importable event, 0 when skipped, or -1 on error.
 */
static int
parseImportLine(char *line,
                long long *jobId,
                struct loaderEventFields *times,
                char *dayKey,
                size_t dayKeyLen,
                long long *skipped)
{
    int status;

    status = loaderExtractEventFromLine(line, jobId, times);
    if (status == LOADER_EVENT_OK) {
        if (!times->hasEventTime ||
            loaderStoreFormatEventDay(times->eventTime, dayKey,
                                          dayKeyLen) != 0) {
            return -1;
        }
        return 1;
    }
    *skipped += 1;
    return 0;
}

/*
 * Refresh retained-head events around the live-current retention boundary.
 * @param[in] db: SQLite connection.
 * @param[in] plan: Live source plan.
 * @param[in] historyRetentionSeconds: Import window in seconds.
 * @param[in] now: Current Unix time.
 * @return: A loaderStepResult value.
 */
int
loaderImportRefreshRetainedHead(sqlite3 *db,
                                    const struct loaderSourcePlan *plan,
                                    long long historyRetentionSeconds,
                                    long long now)
{
    char cutoffDay[LOADER_EVENT_DAY_KEY_MAX];
    char floorDay[LOADER_EVENT_DAY_KEY_MAX];
    char formattedFloorDay[LOADER_EVENT_DAY_KEY_MAX];
    char (*dropTables)[LOADER_EVENT_TABLE_NAME_MAX];
    size_t dropCount;
    struct loaderStoreEventWriter writer;
    FILE *fp;
    char *line;
    size_t lineCapacity;
    long long retainedSourceId;
    long long scannedLines;
    long long filteredRows;
    long long scannedBytes;
    long long inserted;
    long long skipped;
    long long headerEnd;
    long long physicalOffset;
    ssize_t rawLen;
    struct stat st;
    struct tm floorTm;
    time_t floorEpoch;
    time_t refreshStarted;
    time_t transactionStarted;
    int floorYear;
    int floorMonth;
    int floorDayOfMonth;
    int identityRc;
    int inTransaction;
    int writerOpen;
    int storeRc;
    enum loaderStepResult result;

    if (db == NULL || plan == NULL ||
        plan->kind != LOADER_SOURCE_LIVE_CURRENT ||
        historyRetentionSeconds <= 0 || now <= historyRetentionSeconds) {
        ls_syslog(LOG_ERR, "invalid_retained_head_refresh");
        return LOADER_STEP_FATAL;
    }

    fp = NULL;
    line = NULL;
    lineCapacity = 0;
    dropTables = NULL;
    dropCount = 0;
    retainedSourceId = 0;
    scannedLines = 0;
    filteredRows = 0;
    scannedBytes = 0;
    inserted = 0;
    skipped = 0;
    inTransaction = 0;
    writerOpen = 0;
    transactionStarted = 0;
    memset(&writer, 0, sizeof(writer));
    result = LOADER_STEP_FATAL;

    storeRc = loaderStoreReadRetainedHeadSourceId(db, &retainedSourceId);
    if (storeRc < 0)
        goto done;
    if (storeRc > 0) {
        result = LOADER_STEP_REBUILD_REQUIRED;
        goto done;
    }

    if (retainedSourceId > plan->sourceId) {
        ls_syslog(LOG_ERR,
                "retained_head_source_ahead retained_source_id=%lld "
                "current_source_id=%lld",
                retainedSourceId, plan->sourceId);
        result = LOADER_STEP_REBUILD_REQUIRED;
        goto done;
    }
    if (retainedSourceId == plan->sourceId) {
        ls_syslog(LOG_INFO,
                "retained_head_refresh_skip file=%s source_id=%lld "
                "reason=same_source",
                plan->path, plan->sourceId);
        result = LOADER_STEP_IDLE;
        goto done;
    }

    if (loaderStoreFormatEventDay(now - historyRetentionSeconds,
                                      cutoffDay,
                                      sizeof(cutoffDay)) != 0)
        goto done;

    if (loaderStoreFindRetainedFloorDay(
            db, cutoffDay, floorDay, sizeof(floorDay)) != 0)
        goto done;

    floorYear = 0;
    floorMonth = 0;
    floorDayOfMonth = 0;
    if (strlen(floorDay) != 8 ||
        sscanf(floorDay, "%4d%2d%2d", &floorYear, &floorMonth,
               &floorDayOfMonth) != 3) {
        ls_syslog(LOG_ERR, "parse retained day failed day=%s", floorDay);
        goto done;
    }
    memset(&floorTm, 0, sizeof(floorTm));
    floorTm.tm_year = floorYear - 1900;
    floorTm.tm_mon = floorMonth - 1;
    floorTm.tm_mday = floorDayOfMonth;
    floorTm.tm_isdst = -1;
    floorEpoch = mktime(&floorTm);
    if (floorEpoch == (time_t)-1 ||
        loaderStoreFormatEventDay((long long)floorEpoch,
                                      formattedFloorDay,
                                      sizeof(formattedFloorDay)) != 0 ||
        strcmp(formattedFloorDay, floorDay) != 0) {
        ls_syslog(LOG_ERR, "invalid retained day day=%s", floorDay);
        goto done;
    }

    fp = fopen(plan->path, "rb");
    if (fp == NULL) {
        int savedErrno;

        savedErrno = errno;
        ls_syslog(LOG_ERR, "open retained head event file failed file=%s: %m",
                plan->path);
        result = savedErrno == ENOENT ? LOADER_STEP_STALE_REPLAN :
            LOADER_STEP_FATAL;
        goto done;
    }
    identityRc = fileIdentityMatches(fp, plan->fileIno);
    if (identityRc != 0) {
        result = identityRc > 0 ? LOADER_STEP_STALE_REPLAN :
            LOADER_STEP_FATAL;
        goto done;
    }
    if (fstat(fileno(fp), &st) != 0) {
        ls_syslog(LOG_ERR, "stat retained head event file failed file=%s: %m",
                plan->path);
        goto done;
    }
    if (plan->payloadStartOff <= 0 ||
        plan->payloadStartOff > (long long)st.st_size) {
        ls_syslog(LOG_INFO,
                "retained_head_payload_boundary_stale file=%s "
                "offset=%lld size=%lld",
                plan->path, plan->payloadStartOff,
                (long long)st.st_size);
        result = LOADER_STEP_STALE_REPLAN;
        goto done;
    }

    rawLen = getline(&line, &lineCapacity, fp);
    if (rawLen < 0) {
        ls_syslog(LOG_INFO, "retained_head_header_unavailable file=%s",
                plan->path);
        result = LOADER_STEP_STALE_REPLAN;
        goto done;
    }
    {
        char *p;

        p = line;
        while (*p != '\0' && isspace((unsigned char)*p))
            p++;
        if (*p != '#') {
            ls_syslog(LOG_INFO, "retained_head_header_invalid file=%s",
                    plan->path);
            result = LOADER_STEP_STALE_REPLAN;
            goto done;
        }
    }
    headerEnd = (long long)ftello(fp);
    if (headerEnd <= 0 || headerEnd > plan->payloadStartOff) {
        ls_syslog(LOG_INFO,
                "retained_head_boundary_stale file=%s header_end=%lld "
                "payload_start=%lld",
                plan->path, headerEnd, plan->payloadStartOff);
        result = LOADER_STEP_STALE_REPLAN;
        goto done;
    }

    refreshStarted = time(NULL);
    if (loaderStoreBegin(db) != 0)
        goto done;
    inTransaction = 1;
    transactionStarted = time(NULL);

    if (loaderStoreLoadExpiredEventTables(
            db, floorDay, &dropTables, &dropCount) != 0)
        goto done;

    ls_syslog(LOG_INFO,
            "retained_head_refresh_start file=%s source_id=%lld "
            "previous_source_id=%lld cutoff_day=%s floor_day=%s "
            "expired_tables=%zu",
            plan->path, plan->sourceId, retainedSourceId,
            cutoffDay, floorDay, dropCount);

    if (loaderStoreDropExpiredEventTables(
            db, floorDay, dropTables, dropCount) != 0)
        goto done;

    physicalOffset = headerEnd;
    while (physicalOffset < plan->payloadStartOff) {
        struct loaderEventFields times;
        char eventDay[LOADER_EVENT_DAY_KEY_MAX];
        long long jobId;
        size_t textLen;
        int parseRc;

        rawLen = getline(&line, &lineCapacity, fp);
        if (rawLen < 0) {
            if (ferror(fp)) {
                int savedErrno;

                savedErrno = errno;
                if (isStaleHandleError(savedErrno)) {
                    ls_syslog(LOG_INFO,
                            "read retained head stale file=%s: %m",
                            plan->path);
                    result = LOADER_STEP_STALE_REPLAN;
                } else {
                    ls_syslog(LOG_ERR,
                            "read retained head failed file=%s: %m",
                            plan->path);
                    result = LOADER_STEP_FATAL;
                }
            } else {
                ls_syslog(LOG_INFO,
                        "retained_head_ended_early file=%s offset=%lld "
                        "end=%lld",
                        plan->path, physicalOffset,
                        plan->payloadStartOff);
                result = LOADER_STEP_STALE_REPLAN;
            }
            goto done;
        }
        physicalOffset = (long long)ftello(fp);
        if (physicalOffset < 0) {
            ls_syslog(LOG_ERR,
                    "tell retained head event file failed file=%s: %m",
                    plan->path);
            goto done;
        }
        if (physicalOffset > plan->payloadStartOff) {
            ls_syslog(LOG_INFO,
                    "retained_head_line_crossed_boundary file=%s "
                    "line_end=%lld payload_start=%lld",
                    plan->path, physicalOffset,
                    plan->payloadStartOff);
            result = LOADER_STEP_STALE_REPLAN;
            goto done;
        }

        textLen = (size_t)rawLen;
        scannedLines += 1;
        scannedBytes += rawLen;
        stripTrailingNewlines(line, &textLen);
        memset(&times, 0, sizeof(times));
        parseRc = loaderExtractEventFromLine(line, &jobId, &times);
        if (parseRc == LOADER_EVENT_OK) {
            if (!times.hasEventTime) {
                ls_syslog(LOG_ERR, "retained head event missing event time");
                goto done;
            }
            if (times.eventTime < (long long)floorEpoch) {
                if (loaderStoreFormatEventDay(
                        times.eventTime, eventDay,
                        sizeof(eventDay)) != 0 ||
                    openDayWriter(
                        db, &writer, &writerOpen,
                        LOADER_IMPORT_WRITER_UNTRACKED, eventDay) != 0 ||
                    loaderStoreEventWriterInsert(
                        db, &writer, jobId, line, &times) != 0) {
                    goto done;
                }
                inserted += 1;
            } else {
                filteredRows += 1;
            }
        } else {
            skipped += 1;
        }
    }
    if (physicalOffset != plan->payloadStartOff) {
        result = LOADER_STEP_STALE_REPLAN;
        goto done;
    }

    ls_syslog(LOG_INFO,
            "retained_head_scan_done file=%s source_id=%lld "
            "header_end=%lld payload_start=%lld scanned_lines=%lld "
            "scanned_bytes=%lld inserted=%lld filtered=%lld skipped=%lld",
            plan->path, plan->sourceId, headerEnd,
            plan->payloadStartOff, scannedLines, scannedBytes,
            inserted, filteredRows, skipped);

    if (writerOpen) {
        loaderStoreEventWriterClose(&writer);
        writerOpen = 0;
    }

    if (loaderStoreUpdateRetainedHeadSourceId(db, plan->sourceId) != 0)
        goto done;

    if (loaderStoreCommit(db) != 0)
        goto done;
    inTransaction = 0;
    ls_syslog(LOG_INFO,
            "retained_head_refresh_done file=%s source_id=%lld "
            "floor_day=%s inserted=%lld filtered=%lld skipped=%lld "
            "dropped_tables=%zu transaction_sec=%ld elapsed_sec=%ld",
            plan->path, plan->sourceId, floorDay, inserted, filteredRows,
            skipped, dropCount,
            (long)(time(NULL) - transactionStarted),
            (long)(time(NULL) - refreshStarted));
    result = LOADER_STEP_COMMITTED;

done:
    if (writerOpen)
        loaderStoreEventWriterClose(&writer);
    if (inTransaction) {
        ls_syslog(LOG_ERR,
                "retained_head_refresh_rollback file=%s source_id=%lld "
                "scanned_lines=%lld inserted=%lld result=%d",
                plan != NULL ? plan->path : "none",
                plan != NULL ? plan->sourceId : 0,
                scannedLines, inserted, (int)result);
        loaderStoreRollback(db);
    }
    if (fp != NULL)
        fclose(fp);
    free(line);
    free(dropTables);
    return result;
}

/*
 * Initialize per-call import statistics from a source plan.
 * @param[in] plan: Source plan being imported.
 * @param[out] stats: Statistics structure to initialize.
 * @return: None.
 */
static void
prepareImportStats(const struct loaderSourcePlan *plan,
                   struct loaderImportStats *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->sourceId = plan->sourceId;
    stats->batchStartOffset = plan->currentOffset;
    stats->batchEndOffset = plan->currentOffset;
    stats->currentOffset = plan->currentOffset;
    snprintf(stats->file, sizeof(stats->file), "%s",
             pathBasename(plan->path));
    snprintf(stats->reason, sizeof(stats->reason), "%s", "none");
}

/*
 * Consume one planned source through EOF using bounded daily transactions.
 * @param[in] db: SQLite connection.
 * @param[in] fp: Source stream positioned at plan->currentOffset.
 * @param[in] plan: Source range and resume metadata.
 * @param[in] useBulkWriter: Nonzero to defer per-table index creation.
 * @param[in] finishSourceOnEnd: Nonzero to mark the source complete at EOF.
 * @param[out] stats: Import counters and final offset.
 * @return: A loaderStepResult describing progress or recovery action.
 */
static enum loaderStepResult
importEventsUntilEof(sqlite3 *db,
                     FILE *fp,
                     const struct loaderSourcePlan *plan,
                     int useBulkWriter,
                     int finishSourceOnEnd,
                     struct loaderImportStats *stats)
{
    char *line;
    size_t capacity;
    ssize_t rawLen;
    long long nextOffset;
    long long inserted;
    long long skipped;
    long long endPhysicalOffset;
    long long txLines;
    long long txRawBytes;
    int inTransaction;
    int remapped;
    int writerOpen;
    char activeDay[LOADER_EVENT_DAY_KEY_MAX];
    struct loaderStoreEventWriter writer;
    enum loaderStepResult result;

    prepareImportStats(plan, stats);
    line = NULL;
    capacity = 0;
    nextOffset = plan->currentOffset;
    inserted = 0;
    skipped = 0;
    txLines = 0;
    txRawBytes = 0;
    inTransaction = 0;
    remapped = 0;
    writerOpen = 0;
    activeDay[0] = '\0';
    memset(&writer, 0, sizeof(writer));
    result = LOADER_STEP_COMMITTED;
    endPhysicalOffset = plan->hasEndOffset ?
        plan->payloadStartOff + plan->endOffset : 0;

    rawLen = getline(&line, &capacity, fp);
    if (rawLen < 0) {
        if (ferror(fp)) {
            int savedErrno = errno;

            errno = savedErrno;
            if (isStaleHandleError(savedErrno)) {
                ls_syslog(LOG_INFO, "read_event_file_stale file=%s: %m",
                          plan->path);
                if (line != NULL)
                    free(line);
                return LOADER_STEP_STALE_REPLAN;
            }
            ls_syslog(LOG_ERR, "read_event_file_failed file=%s: %m",
                      plan->path);
            if (line != NULL)
                free(line);
            return LOADER_STEP_FATAL;
        }
        if (line != NULL)
            free(line);
        snprintf(stats->reason, sizeof(stats->reason), "%s", "eof_empty");
        return LOADER_STEP_IDLE;
    }

    /*
     * Commit on day change or row/byte limit. A day change commits before
     * reopening so a prepared statement never targets the wrong daily table.
     */
    for (;;) {
        struct loaderEventFields times;
        long long physicalOffset;
        long long logicalOffset;
        long long lineNextOffset;
        long long jobId;
        size_t textLen;
        char lineDay[LOADER_EVENT_DAY_KEY_MAX];
        int parseRc;

        physicalOffset = (long long)ftello(fp) - rawLen;
        if (plan->hasEndOffset && physicalOffset >= endPhysicalOffset)
            break;
        if (plan->hasEndOffset && physicalOffset + rawLen > endPhysicalOffset)
            break;

        logicalOffset = physicalOffset - plan->payloadStartOff;
        lineNextOffset = logicalOffset + rawLen;
        textLen = (size_t)rawLen;
        stripTrailingNewlines(line, &textLen);
        if (stats->inputLines == 0)
            stats->batchStartOffset = logicalOffset;

        if (beginChunk(db, plan, &inTransaction,
                       &remapped) != 0)
            goto fatal;

        memset(&times, 0, sizeof(times));
        lineDay[0] = '\0';
        parseRc = parseImportLine(
            line, &jobId, &times, lineDay, sizeof(lineDay), &skipped);
        if (parseRc < 0)
            goto fatal;
        if (parseRc > 0) {
            if (activeDay[0] != '\0' &&
                strcmp(activeDay, lineDay) != 0 && txLines > 0) {
                int commitRc;

                commitRc = commitChunk(
                    db, &writer, &writerOpen,
                    &inTransaction, plan->sourceId, logicalOffset,
                    plan->seenRowCount + inserted);
                if (commitRc < 0) {
                    goto fatal;
                }
                if (commitRc > 0) {
                    goto fatalNoRollback;
                }
                logChunkCommit(
                    plan, stats, "day_change", activeDay, txLines,
                    txRawBytes, logicalOffset, inserted, skipped);
                txLines = 0;
                txRawBytes = 0;
                activeDay[0] = '\0';
                if (beginChunk(db, plan, &inTransaction,
                               &remapped) != 0) {
                    goto fatal;
                }
            }
            if (activeDay[0] == '\0')
                snprintf(activeDay, sizeof(activeDay), "%s", lineDay);
            if (openDayWriter(
                db, &writer, &writerOpen,
                useBulkWriter ? LOADER_IMPORT_WRITER_BULK :
                LOADER_IMPORT_WRITER_TRACKED,
                activeDay) != 0) {
                goto fatal;
            }
            if (loaderStoreEventWriterInsert(
                    db, &writer, jobId, line, &times) != 0) {
                goto fatal;
            }
            inserted += 1;
        }

        stats->inputLines += 1;
        stats->batchEndOffset = lineNextOffset;
        stats->currentOffset = lineNextOffset;
        nextOffset = lineNextOffset;
        txLines += 1;
        txRawBytes += rawLen;
        if (txLines >= loaderImportTxMaxLines ||
            txRawBytes >= loaderImportTxMaxRawBytes) {
            int commitRc;
            const char *chunkReason;

            chunkReason = txLines >= loaderImportTxMaxLines ?
                "chunk_lines" : "chunk_bytes";
            commitRc = commitChunk(
                db, &writer, &writerOpen,
                &inTransaction, plan->sourceId, nextOffset,
                plan->seenRowCount + inserted);
            if (commitRc < 0)
                goto fatal;
            if (commitRc > 0)
                goto fatalNoRollback;
            logChunkCommit(
                plan, stats, chunkReason, activeDay, txLines,
                txRawBytes, nextOffset, inserted, skipped);
            txLines = 0;
            txRawBytes = 0;
            activeDay[0] = '\0';
        }
        rawLen = getline(&line, &capacity, fp);
        if (rawLen < 0)
            break;
    }

    if (ferror(fp)) {
        int savedErrno = errno;

        errno = savedErrno;
        if (isStaleHandleError(savedErrno)) {
            ls_syslog(LOG_INFO, "read_event_file_stale file=%s: %m",
                      plan->path);
            goto stale;
        }
        ls_syslog(LOG_ERR, "read_event_file_failed file=%s: %m",
                  plan->path);
        goto fatal;
    }
    if (stats->inputLines == 0 && plan->hasEndOffset &&
        nextOffset < plan->endOffset) {
        ls_syslog(LOG_ERR,
                  "event_file_ended_before_planned_end_offset file=%s "
                  "current_offset=%lld end_offset=%lld",
                  plan->path, nextOffset, plan->endOffset);
        goto stale;
    }

    if (writerOpen) {
        loaderStoreEventWriterClose(&writer);
        writerOpen = 0;
    }
    if (line != NULL) {
        free(line);
        line = NULL;
    }

    stats->insertedRows = inserted;
    stats->skippedLines = skipped;
    stats->batchEndOffset = nextOffset;
    stats->currentOffset = nextOffset;

    if (inTransaction) {
        int commitRc;

        commitRc = commitChunk(
            db, &writer, &writerOpen,
            &inTransaction, plan->sourceId,
            nextOffset, plan->seenRowCount + inserted);
        if (commitRc < 0)
            goto fatal;
        if (commitRc > 0)
            goto fatalNoRollback;
        logChunkCommit(
            plan, stats, "source_eof", activeDay, txLines,
            txRawBytes, nextOffset, inserted, skipped);
        txLines = 0;
        txRawBytes = 0;
        activeDay[0] = '\0';
    }
    if (finishSourceOnEnd && nextOffset >= plan->endOffset) {
        if (loaderStoreBegin(db) != 0)
            goto fatalNoRollback;
        inTransaction = 1;
        if (loaderStoreFinishCurrentSource(db) != 0)
            goto fatalAfterClose;
        result = LOADER_STEP_SOURCE_DONE;
        snprintf(stats->reason, sizeof(stats->reason), "%s",
                 stats->inputLines > 0 ? "source_done" : "source_done_empty");
        if (loaderStoreCommit(db) != 0) {
            inTransaction = 0;
            goto fatalNoRollback;
        }
        inTransaction = 0;
    } else if (stats->reason[0] == '\0' ||
               strcmp(stats->reason, "none") == 0) {
        snprintf(stats->reason, sizeof(stats->reason), "%s", "eof_flush");
    }
    return result;

fatal:
    if (line != NULL)
        free(line);
    if (writerOpen)
        loaderStoreEventWriterClose(&writer);
fatalAfterClose:
    if (inTransaction) {
        loaderStoreRollback(db);
    }
fatalNoRollback:
    return LOADER_STEP_FATAL;

stale:
    if (line != NULL)
        free(line);
    if (writerOpen)
        loaderStoreEventWriterClose(&writer);
    if (inTransaction) {
        loaderStoreRollback(db);
    }
    return LOADER_STEP_STALE_REPLAN;
}

/*
 * Import one archive source.
 * @param[in] db: SQLite connection.
 * @param[in] plan: Archive source plan.
 * @param[out] stats: Import statistics.
 * @return: A loaderStepResult value.
 */
int
loaderImportArchiveSource(sqlite3 *db,
                             const struct loaderSourcePlan *plan,
                             struct loaderImportStats *stats)
{
    FILE *fp;
    enum loaderStepResult result;

    if (db == NULL || plan == NULL || stats == NULL ||
        plan->kind != LOADER_SOURCE_ARCHIVE) {
        ls_syslog(LOG_ERR, "invalid_archive_import");
        return LOADER_STEP_FATAL;
    }

    prepareImportStats(plan, stats);
    if (plan->currentOffset >= plan->endOffset) {
        if (loaderStoreBegin(db) != 0)
            return LOADER_STEP_FATAL;
        if (loaderStoreFinishCurrentSource(db) != 0) {
            loaderStoreRollback(db);
            return LOADER_STEP_FATAL;
        }
        if (loaderStoreCommit(db) != 0)
            return LOADER_STEP_FATAL;
        snprintf(stats->reason, sizeof(stats->reason), "%s",
                 "source_done_empty");
        return LOADER_STEP_SOURCE_DONE;
    }

    fp = fopen(plan->path, "rb");
    if (fp == NULL) {
        int savedErrno = errno;

        ls_syslog(LOG_ERR, "open_archive_failed file=%s: %m", plan->path);
        return savedErrno == ENOENT ? LOADER_STEP_STALE_REPLAN :
            LOADER_STEP_FATAL;
    }
    {
        int identityRc;

        identityRc = fileIdentityMatches(fp, plan->fileIno);
        if (identityRc != 0) {
            fclose(fp);
            return identityRc > 0 ? LOADER_STEP_STALE_REPLAN :
                LOADER_STEP_FATAL;
        }
    }

    if (fseeko(fp, (off_t)(plan->payloadStartOff + plan->currentOffset),
               SEEK_SET) != 0) {
        ls_syslog(LOG_ERR, "seek_archive_failed file=%s offset=%lld: %m",
                  plan->path, plan->payloadStartOff + plan->currentOffset);
        fclose(fp);
        return LOADER_STEP_FATAL;
    }

    result = importEventsUntilEof(
        db, fp, plan, 1, 1, stats);
    fclose(fp);
    if (result == LOADER_STEP_IDLE && stats->currentOffset < plan->endOffset) {
        ls_syslog(LOG_ERR,
                  "archive_ended_before_planned_end_offset file=%s "
                  "current_offset=%lld end_offset=%lld",
                  plan->path, stats->currentOffset, plan->endOffset);
        return LOADER_STEP_STALE_REPLAN;
    }
    if (result == LOADER_STEP_COMMITTED && stats->currentOffset < plan->endOffset) {
        ls_syslog(LOG_ERR,
                  "archive_ended_before_planned_end_offset file=%s "
                  "current_offset=%lld end_offset=%lld",
                  plan->path, stats->currentOffset, plan->endOffset);
        return LOADER_STEP_STALE_REPLAN;
    }
    return result;
}

/*
 * Open and position the live source described by a plan.
 * @param[in] plan: Live source identity and resume offset.
 * @param[in,out] live: Handle receiving the opened stream and state.
 * @return: 0 on success, 1 if the source identity changed, or -1 on failure.
 */
static int
openLiveSource(const struct loaderSourcePlan *plan,
               struct loaderLiveHandle *live)
{
    int identityRc;

    loaderLiveHandleClose(live);
    live->fp = fopen(plan->path, "rb");
    if (live->fp == NULL) {
        ls_syslog(LOG_ERR, "open_live_failed file=%s: %m", plan->path);
        return -1;
    }
    identityRc = fileIdentityMatches(live->fp, plan->fileIno);
    if (identityRc != 0) {
        loaderLiveHandleClose(live);
        return identityRc > 0 ? 1 : -1;
    }
    if (fseeko(live->fp, (off_t)(plan->payloadStartOff +
                                 plan->currentOffset), SEEK_SET) != 0) {
        ls_syslog(LOG_ERR, "seek_live_failed file=%s offset=%lld: %m",
                  plan->path, plan->payloadStartOff + plan->currentOffset);
        loaderLiveHandleClose(live);
        return -1;
    }
    live->sourceId = plan->sourceId;
    snprintf(live->path, sizeof(live->path), "%s", plan->path);
    snprintf(live->fileIno, sizeof(live->fileIno), "%s", plan->fileIno);
    live->payloadStartOff = plan->payloadStartOff;
    live->currentOffset = plan->currentOffset;
    live->seenRowCount = plan->seenRowCount;
    live->eofCount = 0;
    return 0;
}

/*
 * Import a live source through its current EOF.
 * @param[in] db: SQLite connection.
 * @param[in] plan: Live source plan.
 * @param[in,out] live: File handle retained across polls.
 * @param[out] stats: Import statistics.
 * @return: A loaderStepResult value.
 */
int
loaderImportLiveUntilEof(sqlite3 *db,
                             const struct loaderSourcePlan *plan,
                             struct loaderLiveHandle *live,
                             struct loaderImportStats *stats)
{
    struct loaderSourcePlan effectivePlan;
    enum loaderStepResult result;

    if (db == NULL || plan == NULL || live == NULL || stats == NULL ||
        plan->kind != LOADER_SOURCE_LIVE_CURRENT) {
        ls_syslog(LOG_ERR, "invalid_live_import");
        return LOADER_STEP_FATAL;
    }

    memset(stats, 0, sizeof(*stats));
    stats->sourceId = plan->sourceId;
    stats->batchStartOffset = plan->currentOffset;
    stats->batchEndOffset = plan->currentOffset;
    stats->currentOffset = plan->currentOffset;
    snprintf(stats->file, sizeof(stats->file), "%s",
             pathBasename(plan->path));
    snprintf(stats->reason, sizeof(stats->reason), "%s", "none");

    if (live->fp == NULL || live->sourceId != plan->sourceId ||
        strcmp(live->fileIno, plan->fileIno) != 0) {
        int openRc;

        openRc = openLiveSource(plan, live);
        if (openRc != 0)
            return openRc > 0 ? LOADER_STEP_STALE_REPLAN :
                LOADER_STEP_FATAL;
    }

    effectivePlan = *plan;
    effectivePlan.sourceId = live->sourceId;
    snprintf(effectivePlan.path, sizeof(effectivePlan.path), "%s",
             live->path);
    snprintf(effectivePlan.fileIno, sizeof(effectivePlan.fileIno), "%s",
             live->fileIno);
    effectivePlan.payloadStartOff = live->payloadStartOff;
    effectivePlan.currentOffset = live->currentOffset;
    effectivePlan.seenRowCount = live->seenRowCount;
    effectivePlan.hasEndOffset = 0;
    effectivePlan.endOffset = 0;
    effectivePlan.markRemapped = 0;

    clearerr(live->fp);
    result = importEventsUntilEof(
        db, live->fp, &effectivePlan, 0, 0, stats);
    if (result == LOADER_STEP_COMMITTED) {
        live->currentOffset = stats->currentOffset;
        live->seenRowCount += stats->insertedRows;
        live->eofCount = 0;
        return result;
    }
    if (result == LOADER_STEP_IDLE && stats->inputLines == 0) {
        char currentIno[32];
        long long currentSize;

        live->eofCount += 1;
        if (live->eofCount < LOADER_LIVE_EOF_ROTATE_CHECKS) {
            snprintf(stats->reason, sizeof(stats->reason), "%s", "eof_wait");
            return LOADER_STEP_IDLE;
        }
        if (statPathIdentity(live->path, currentIno,
                             sizeof(currentIno),
                             &currentSize) != 0) {
            ls_syslog(LOG_ERR, "stat_live_path_failed file=%s: %m",
                      live->path);
            snprintf(stats->reason, sizeof(stats->reason), "%s",
                     "eof_stat_retry");
            return LOADER_STEP_IDLE;
        }
        if (strcmp(currentIno, live->fileIno) == 0) {
            if (currentSize < live->payloadStartOff + live->currentOffset) {
                ls_syslog(LOG_ERR,
                          "live_event_file_size_regressed "
                          "source_id=%lld inode=%s size=%lld "
                          "committed_position=%lld",
                          live->sourceId, currentIno, currentSize,
                          live->payloadStartOff + live->currentOffset);
                return LOADER_STEP_REBUILD_REQUIRED;
            }
            snprintf(stats->reason, sizeof(stats->reason), "%s",
                     "eof_still_current");
            return LOADER_STEP_IDLE;
        }
        ls_syslog(LOG_INFO,
                  "live_rotate_detected source_id=%lld file=%s "
                  "old_ino=%s new_ino=%s "
                  "offset=%lld eof_count=%d",
                  live->sourceId, live->path,
                  live->fileIno, currentIno,
                  live->currentOffset, live->eofCount);
        if (loaderStoreBegin(db) != 0)
            return LOADER_STEP_FATAL;
        if (loaderStoreFinishCurrentSource(db) != 0) {
            loaderStoreRollback(db);
            return LOADER_STEP_FATAL;
        }
        if (loaderStoreCommit(db) != 0)
            return LOADER_STEP_FATAL;
        snprintf(stats->reason, sizeof(stats->reason), "%s",
                 "live_rotated_fd_eof");
        loaderLiveHandleClose(live);
        return LOADER_STEP_SOURCE_DONE;
    }
    return result;
}
